#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_live.h>
#include <qkvm/core/kvmd_media.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>

#include <qkvm/core/tls.h>

#include <qkvm/core/kvm_certificate.h>
#include <qkvm/core/kvm_probe.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The words are the server's driver's: a card shows them as they are, whoever said them.
base::Error slow(const char* how)
{
    return {"no_stream", how};
}
// (The stream's sentence for the same, said of keyboard and mouse: kvmd_stream.cpp.)
base::Error lineNotAllowed()
{
    return {"not_allowed", "The device signed this program in and refuses keyboard and mouse all the same"};
}

// What a refused password is said as.
base::Error passwordRefused()
{
    return {"auth_failed", "Login rejected - check the username and password"};
}
// What stands against a KVM that is there and does not sign this program in, for another reason than
// the password (the web's `not_signing_in`): whatever waited for that sign-in was never sent, and is
// no question that may have been done.
base::Error notSigningIn(const std::string& why)
{
    return {"no_sign_in", "The device does not sign qKVM in (" + why + "). It is asked again shortly."};
}

// Failures that mean "the KVM is not there, or is not the KVM", and not "it said no".
bool meansAway(const std::string& code)
{
    return code == "unreachable" || code == "timeout" || code == "dropped" || code == "cert_mismatch" ||
           code == "cert_invalid";
}

// Pictures that were asked for close together are one picture.
constexpr qint64 sameLookMs = 200;

} // namespace

// A sign-in that is out, on its own line, which can outlive this driver: what is needed to tell the pacing what it came
// to, by this driver or, where it was let go of meanwhile, without it.
struct KvmdDevice::SignInOut {
    QPointer<KvmdDevice> driver; // none: let go of while this was out
    QPointer<DeviceAsker> line;
    std::shared_ptr<SignInPacing> pacing; // none: the program's
    SignInPacing::Mark mark;
    SignInPacing::Steps steps;
    SignInPacing::Sent sent;
    QUrl address; // the KVM's, where its answer may send it on to HTTPS (kvm::httpsInstead)
};

// What a sign-in came to, as the pacing was told it (cameOf).
struct KvmdDevice::SignInCame {
    enum class Kind {
        NotSent, // nothing reached the KVM (put back)
        Neither, // nothing answered it, or what answered said neither yes nor no
        Refused,
        In,
    };
    Kind kind = Kind::NotSent;
    bool heard = false; // the KVM answered, whatever it said
    base::Error why;
    QByteArray token;
};

KvmdDevice::KvmdDevice(Where where, QString password, QObject* parent)
    : KvmdDevice(std::move(where), std::move(password), Pace{}, parent)
{}

KvmdDevice::KvmdDevice(Where where, QString password, Pace pace, QObject* parent)
    : OwnDevice(parent), m_where(std::move(where)), m_password(SecretText::takenFrom(password)),
      m_pace(std::move(pace)),
      m_asker(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
              DeviceAsker::Times{m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs}, "auth_token"_ba)
{
    // The KVM showed the certificate it is known by: said as every driver says it (own_device.h), once.
    connect(&m_asker, &DeviceAsker::showedItsCertificate, this, &KvmdDevice::showed);
    m_clock.start();
    m_awakeTimer.setInterval(std::clamp(m_pace.restAfterMs / 6, 50, 5000));
    connect(&m_awakeTimer, &QTimer::timeout, this, [this] {
        // A stream of pictures that is open is somebody watching, for as long as it is: its
        // picture is wanted at every look of this timer. (And once more at the moment the stream
        // ends, which the stream notes: the video is not let rest at the look after that.)
        if (!m_streams.isEmpty() ||
            std::ranges::any_of(m_videoLines, [](const auto& line) { return line != nullptr && line->isIn(); })) {
            m_wantedAt = now();
        }
        if (m_streams.isEmpty() && (!m_awake || now() - m_wantedAt > m_pace.restAfterMs)) {
            if (AwakeLine* gone = m_awake.release()) {
                gone->deleteLater();
            }
            m_awakeTimer.stop();
        } else if (!m_awake) {
            // The line fell under an open stream. kvmd rests its video once the last such line
            // has gone, and the stream would end with it: asked for anew, at this timer's pace
            // and no faster.
            keepAwake();
        }
    });
}

KvmdDevice::~KvmdDevice()
{
    // The streams of pictures are their callers' and outlast this. They are ended here, before
    // anything of this goes: each lets go of its question, which is the manager's and would go
    // with it unseen, and knows of no device from then on. (The list is taken first: a stream
    // that is ended takes itself off it.)
    const QList<KvmdStream*> streams = std::exchange(m_streams, {});
    for (KvmdStream* stream : streams) {
        stream->deviceGone();
    }
    // A sign-in that is out goes on without this driver: its line is the program's from now on, and what it comes to
    // is told to the pacing there (cameOf), which ends its hold for whoever waits for it, and a driver made anew. (Gone
    // with this, it was never heard: the hold lasted as long as it could be out, and a Save after Cancel waited 49.5
    // s.)
    if (m_signInOut && m_signInOut->line != nullptr) {
        m_signInOut->line->setParent(QCoreApplication::instance());
    }
    // A press of its board whose click is out, and whose answer nobody will hear now: the board may take it as late as
    // the KVM goes on with it, and hold the button. Left alone from now for as long as an answer is waited for and the
    // button's hold, whoever asks (a driver made anew for the same KVM among them): the web's rule after a press that
    // came to no clear answer, "the tile edited meanwhile".
    if (m_pressOut != 0) {
        boards().mayBePressing(boardOf(), qint64{m_pace.boardAnswerMs} + m_pressOutHeldMs);
    } else if (m_pressReserved != 0) {
        // (Held for a press of its own whose click had not gone out: nothing went, and nothing holds the board for it.)
        boards().notPressed(boardOf(), m_pressReserved);
    }
    // The same for a Fingerbot's press (kvmd_fingerbot.cpp): the Comet goes on with it.
    if (m_fingerbotOut != 0) {
        boards().mayBePressing(boardOf(), m_fingerbotMayTakeMs, BoardHolds::By::fingerbot);
    } else if (m_fingerbotReserved != 0) {
        // (Held for a press of its own whose click had not gone out: nothing went, and nothing holds the buttons for
        // it.)
        boards().notPressed(boardOf(), m_fingerbotReserved);
    }
}

void KvmdDevice::showed(const QByteArray& der)
{
    if (!std::exchange(m_toldItsCertificate, true)) {
        emit showedItsCertificate(der);
    }
}

void KvmdDevice::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

base::Result<QNetworkReply*> KvmdDevice::sendOut(const Ask& question, const QByteArray& token)
{
    return m_asker.sendOut(question, token);
}

std::optional<base::Error> KvmdDevice::whyNoAnswer(const QNetworkReply* reply)
{
    return DeviceAsker::whyNoAnswer(reply);
}

void KvmdDevice::send(const Ask& question, const QByteArray& token, std::function<void(base::Result<Reply>)> done)
{
    // (Where a redirect says to go is read of every answer: kvm::httpsInstead.)
    Ask asked = question;
    asked.headersAsSent.append("location"_ba);
    m_asker.send(asked, token, [done = std::move(done)](base::Result<DeviceAsker::Reply> reply) {
        if (!reply) {
            done(reply.error());
            return;
        }
        Reply said;
        said.status = reply.value().status;
        said.body = std::move(reply.value().body);
        said.kind = reply.value().kind;
        said.online = reply.value().headers.value("x-ustreamer-online"_ba);
        said.cookies = reply.value().cookies;
        said.location = reply.value().headersAsSent.value("location"_ba);
        done(std::move(said));
    });
}

std::optional<base::Error> KvmdDevice::sentOn(int status, const QByteArray& location) const
{
    // (The web's https_instead and on_https, web 0.3.2: its plain address sends everybody on to HTTPS at the same host,
    // a web server's before kvmd once HTTPS is its only way in. Said, with that address, which an edit offers; never
    // followed: the password would go wherever a redirect says.)
    if (const std::optional<QUrl> to = kvm::httpsInstead(m_where.address, status, location)) {
        return kvm::httpsOnly(*to);
    }
    return std::nullopt;
}

void KvmdDevice::request(const Ask& question, std::function<void(base::Result<Reply>)> done)
{
    request(question, std::move(done), Tries{});
}

void KvmdDevice::request(const Ask& question, std::function<void(base::Result<Reply>)> done, Tries tries)
{
    if (!question.urgent) {
        if (std::optional<base::Error> held = heldBack()) {
            later([done = std::move(done), held = std::move(*held)] { done(held); });
            return;
        }
    }
    // A typing waits among those that a stop answers at once (stopTyping) until it is handed to its line: one answered
    // so is never handed to it. (Again where it is asked a second time, signed in anew.)
    std::shared_ptr<std::function<void(base::Result<Reply>)>> typing;
    if (question.types) {
        typing = std::make_shared<std::function<void(base::Result<Reply>)>>(std::move(done));
        m_typingsWaiting.append(typing);
        done = [typing](base::Result<Reply> reply) {
            if (*typing) {
                std::exchange(*typing, {})(std::move(reply));
            }
        };
    }
    withToken([this, question, tries, typing, done = std::move(done)](const base::Result<QByteArray>& token) mutable {
        if (typing != nullptr) {
            m_typingsWaiting.removeOne(typing);
            if (!*typing) {
                return; // (answered "stopped" while it waited: nothing of it goes)
            }
        }
        if (!token) {
            done(token.error());
            return;
        }
        const QByteArray& used = token.value();
        if (question.goingOut) {
            question.goingOut();
        }
        send(question, used, [this, question, tries, used, done = std::move(done)](base::Result<Reply> reply) mutable {
            if (!reply) {
                done(question.vital ? failed(reply.error(), question.urgent) : reply.error());
                return;
            }
            answered();
            // Its plain address sends everybody on to HTTPS since it was signed in to (HTTPS made its only way in):
            // said, with that address, and not followed. The card says so ("error": it is there), and an edit offers
            // it. And its pass goes on nothing more the plain way: let go of here, and what needs one is held back as
            // the sign-in's own https_only holds it (signIn), until a sign-in that asks first with no password, however
            // lately the KVM was heard, says it again or finds kvmd there (the merged review of 0.2.0, B-M1: the pass
            // went in the clear at every look, to an address that only sends everybody on).
            if (std::optional<base::Error> onto = sentOn(reply.value().status, reply.value().location)) {
                m_sentOnToHttps = true;
                if (m_token == used) {
                    m_token.clear();
                    m_signInError = *onto;
                    m_signInRetryAt = now() + m_pace.signInRetryMs;
                }
                done(std::move(*onto));
                return;
            }
            if (reply.value().status != 401 && reply.value().status != 403) {
                done(std::move(reply));
                return;
            }
            if (const std::optional<base::Error> keeps = sessionRefused(used, tries)) {
                done(*keeps);
                return;
            }
            request(question, std::move(done), tries);
        });
    });
}

std::optional<base::Error> KvmdDevice::sessionRefused(const QByteArray& used, Tries& tries)
{
    if (!tries.relogged) {
        // The session ran out, or kvmd was started anew: once more, signed in afresh.
        if (m_token == used) {
            m_token.clear();
        }
        tries.relogged = true;
        return std::nullopt;
    }
    // It signs this program in and refuses the session it has just given. Every look ends up
    // here: the password is not sent to it again with each of them.
    base::Error keeps{"auth_failed", "Device keeps rejecting the session"};
    if (m_token == used) {
        m_token.clear();
        m_signInError = keeps;
        m_signInRetryAt = now() + m_pace.signInRetryMs;
    }
    return keeps;
}

void KvmdDevice::ask(const Ask& question, std::function<void(base::Result<QJsonObject>)> done)
{
    request(question, [path = question.path, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(reply.error());
            return;
        }
        if (reply.value().status != 200) {
            done(base::Error{"bad_response",
                             u"%1 failed (HTTP %2)"_s.arg(path).arg(reply.value().status).toStdString()});
            return;
        }
        base::Result<QJsonObject> said = kvmd::resultFrom(reply.value().body);
        if (!said) {
            done(base::Error{"bad_response", u"%1 returned unexpected data"_s.arg(path).toStdString()});
            return;
        }
        done(std::move(said));
    });
}

SignInPacing& KvmdDevice::pacing() const
{
    return m_pace.pacing ? *m_pace.pacing : SignInPacing::program();
}

SignInPacing::Mark KvmdDevice::offered() const
{
    return SignInPacing::what(u"login", m_where.address, m_where.user, m_password.view());
}

SignInPacing::Steps KvmdDevice::steps() const
{
    // (A sign-in is out at most its line's whole: to be let in, its own longer wait for an answer, the rest of it.)
    return SignInPacing::Steps{m_pace.refusedHoldMs, m_pace.signInRetryMs,
                               SignInPacing::outFor(m_pace.connectWithinMs, m_pace.quietMs * 5 / 2, m_pace.transferMs)};
}

std::optional<base::Error> KvmdDevice::heldSignIn() const
{
    const SignInPacing::Mark mark = offered();
    if (pacing().waitLeftMs(mark) <= 0) {
        return std::nullopt;
    }
    if (pacing().times(mark) > 0) {
        return passwordRefused();
    }
    // In its own words while the hold it noted itself is the one that stands; otherwise, whichever
    // driver of this KVM noted it (one before it, or one beside it whose sign-in is out), as the web
    // says it (`_not_yet`).
    if (m_signInError && m_heldAs && pacing().standing(mark) == m_heldAs) {
        return m_signInError;
    }
    return notSigningIn("a sign-in to it is under way, or the last one came to nothing");
}

void KvmdDevice::tryAfresh(const Where& where, const QString& password, const Pace& pace)
{
    (pace.pacing ? *pace.pacing : SignInPacing::program()).forget(where.address, where.user, password);
}

void KvmdDevice::withToken(std::function<void(base::Result<QByteArray>)> done)
{
    if (!m_token.isEmpty()) {
        later([done = std::move(done), token = m_token] { done(token); });
        return;
    }
    // (A session it was given and refuses, held here: sessionRefused.)
    if (m_signInError && now() < m_signInRetryAt) {
        later([done = std::move(done), error = *m_signInError] { done(error); });
        return;
    }
    if (m_password.isEmpty()) {
        later([done = std::move(done)] { done(base::Error{"no_password", "No password saved for this device"}); });
        return;
    }
    // Whoever asks while somebody is being signed in waits for that one sign-in.
    if (m_signingIn) {
        m_waitingForToken.append(std::move(done));
        return;
    }
    // What another driver of this KVM noted, or this one before it was made anew: a password that was
    // refused is not offered again before its time (SignInPacing).
    if (std::optional<base::Error> held = heldSignIn()) {
        if (mayWaitForTheOther()) {
            // Another driver's sign-in with it is out (the wall's, while the Edit box signs in): what it comes to
            // is waited for, as the web's second asker waits for the one sign-in, and not said as a failure.
            m_waitingForToken.append(std::move(done));
            m_signingIn = true;
            afterTheOtherSignIn();
            return;
        }
        later([done = std::move(done), held = std::move(*held)] { done(held); });
        return;
    }
    m_waitingForToken.append(std::move(done));
    signIn();
}

bool KvmdDevice::mayWaitForTheOther() const
{
    // Where nothing but another driver's sign-in with it stands; again where, after a yes, another that waited with
    // this one went first. (Each such sign-in ends with its line: a time from the first wait cut off the third of three
    // on a slow board, told "does not sign qKVM in" while the second was out.)
    return pacing().onlyOut(offered());
}

void KvmdDevice::afterTheOtherSignIn()
{
    // Then looked at again: a no it heard is this one's no; after a yes this one signs in itself. (Its line's end tells
    // it; the hold's cap, only where a line never ends.)
    emit waitsForAnotherSignIn();
    const SignInPacing::Mark mark = offered();
    pacing().whenSettled(mark, this, pacing().waitLeftMs(mark) + 5, [this] {
        if (std::optional<base::Error> held = heldSignIn()) {
            if (mayWaitForTheOther()) {
                afterTheOtherSignIn();
                return;
            }
            signedIn(*held);
            return;
        }
        signIn();
    });
}

void KvmdDevice::signIn()
{
    m_signingIn = true;
    Ask login;
    login.method = "POST"_ba;
    login.path = u"/api/auth/login"_s;
    // (Its body is made as it goes, in sendSignIn: nothing of the password waits in the lambdas below.)
    login.bodyIs = "application/x-www-form-urlencoded"_ba;
    // (It may have gone out, and been counted by the KVM, before Qt says it went: never "not there" on a guess.)
    login.doesSomething = true;
    // Its answer is waited for longer than any other's: some KVMs hold a no back on purpose, and a no
    // that came after the usual wait was heard as none, and the password went out again at the next
    // look (the web's SIGN_IN_PATIENCE).
    login.quietMs = m_pace.quietMs * 5 / 2;
    // (And where its answer says to go: cameOf.)
    login.headersAsSent.append("location"_ba);
    // A password goes only on a line shown to be up (DeviceAsker::whenUp): where nothing of the KVM's was heard just
    // now, whether it signs this program in is asked first with nothing to sign in with, under the guess that tells
    // a KVM that is not there. Nothing is counted or sent where that is not answered. Both go on a line made for this
    // sign-in, held to what the KVM is known by, which outlives this driver while the sign-in is out (~KvmdDevice).
    Ask up;
    up.path = u"/api/auth/check"_s;
    // (And where it says to go: a plain address that sends everybody on to HTTPS is said before the password goes the
    // plain way to be sent on, as the web asks its API with no credentials first, sent_on_to_https of web 0.3.2.)
    up.headersAsSent.append("location"_ba);
    auto* line =
        new DeviceAsker(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
                        DeviceAsker::Times{m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs}, "auth_token"_ba,
                        this); // NOLINT(cppcoreguidelines-owning-memory): this driver's, or the program's (~KvmdDevice)
    connect(line, &DeviceAsker::showedItsCertificate, this, &KvmdDevice::showed);
    const auto then = [this, line, login](const std::optional<base::Error>& down, bool heard) {
        heardOn(*line, heard);
        if (down && down->code == "https_only") {
            // Nothing was sent, and nothing is asked again at every look: held as a session refused is (withToken).
            line->deleteLater();
            answered();
            m_sentOnToHttps = true;
            m_signInError = *down;
            m_signInRetryAt = now() + m_pace.signInRetryMs;
            signedIn(*m_signInError);
            return;
        }
        if (down) {
            line->deleteLater();
            signedIn(failed(*down, false));
            return;
        }
        sendSignIn(line, login);
    };
    // (Heard just now, on this driver's own line or a sign-in's, the KVM is there: as before, nothing is asked first.
    // But not where what it said last was that its plain address sends everybody on to HTTPS: m_sentOnToHttps.)
    if (heardJustNow() && !m_sentOnToHttps) {
        later([then] { then(std::nullopt, true); });
        return;
    }
    // (As DeviceAsker::whenUp asks it, on a line that heard nothing yet, and its answer read.)
    line->send(up, {}, [this, then](const base::Result<DeviceAsker::Reply>& reply) {
        if (!reply) {
            then(reply.error(), false);
            return;
        }
        then(sentOn(reply.value().status, reply.value().headersAsSent.value("location"_ba)), true);
    });
}

void KvmdDevice::sendSignIn(DeviceAsker* line, const Ask& login)
{
    // Looked at again just before it is counted: the question asked first is a round trip, and another driver of this
    // KVM (the wall's, a box's) may have sent the same password meanwhile, and heard a no, or have it out still.
    if (std::optional<base::Error> held = heldSignIn()) {
        line->deleteLater();
        if (mayWaitForTheOther()) {
            afterTheOtherSignIn();
            return;
        }
        signedIn(*held);
        return;
    }
    // Counted as it goes out (SignInPacing::sent): a driver let go of while it is out leaves its mark, until what it
    // came to is heard on its line (cameOf).
    auto out = std::make_shared<SignInOut>();
    out->driver = this;
    out->line = line;
    out->pacing = m_pace.pacing;
    out->mark = offered();
    out->steps = steps();
    out->address = m_where.address;
    out->sent = pacing().sent(out->mark, out->steps);
    m_signInOut = out;
    // Made once, of the password as it is held, as it goes: nothing of it in a text made on the way (F5), and no copy
    // of it held but by what goes, which writes it over once its answer came (DeviceAsker; from the merged review, 3).
    // This one is written over here where nothing went.
    Ask sent = login;
    sent.body = secretForm({{"user", m_where.user}, {"passwd", m_password.view()}});
    const WipedOnLeaving overTheBody(sent.body);
    line->send(sent, {}, [out](const base::Result<DeviceAsker::Reply>& reply) {
        if (out->line != nullptr) {
            out->line->deleteLater();
        }
        const SignInCame came = cameOf(*out, reply);
        if (KvmdDevice* driver = out->driver) {
            if (out->line != nullptr) {
                driver->heardOn(*out->line, came.heard);
            }
            driver->m_signInOut.reset();
            driver->signInCame(came);
        }
    });
}

bool KvmdDevice::heardJustNow() const
{
    return m_asker.heardWithin(m_pace.connectWithinMs) ||
           (m_signInLineHeardAt >= 0 && now() - m_signInLineHeardAt <= m_pace.connectWithinMs);
}

void KvmdDevice::heardOn(const DeviceAsker& line, bool heard)
{
    if (heard) {
        m_signInLineHeardAt = now();
    }
    if (!line.shownCertificate().isEmpty()) {
        m_signInLineShowed = line.shownCertificate(); // (as this driver's own line keeps what it was shown)
    }
}

KvmdDevice::SignInCame KvmdDevice::cameOf(const SignInOut& out, const base::Result<DeviceAsker::Reply>& reply)
{
    // Told to the pacing, which outlives drivers: by the driver, or where it was let go of meanwhile, by its line
    // alone.
    SignInPacing& pacing = out.pacing ? *out.pacing : SignInPacing::program();
    SignInCame came;
    if (!reply) {
        // Where it went out and nothing answered it ("timeout", "dropped"), it is held back as one that came to neither
        // yes nor no; where nothing reached the KVM, or what answered is not the KVM (its certificate), nothing is kept
        // of it: the KVM is away, or not the one, and is asked again as such (the web's rule, `_came_to_nothing`).
        came.why = reply.error();
        if (reply.error().code == "timeout" || reply.error().code == "dropped") {
            pacing.unanswered(out.mark, out.steps, out.sent);
            came.kind = SignInCame::Kind::Neither;
        } else {
            pacing.putBack(out.mark, out.sent);
        }
        return came;
    }
    came.heard = true;
    const int status = reply.value().status;
    if (const std::optional<QUrl> to =
            kvm::httpsInstead(out.address, status, reply.value().headersAsSent.value("location"_ba))) {
        // Its plain address sends everybody on to HTTPS at the same host. Not followed: said, with that address, and
        // held back as a sign-in that was neither yes nor no, so that the password does not go out the plain way at
        // every look (the web's login, 0.3.2).
        pacing.unanswered(out.mark, out.steps, out.sent);
        came.kind = SignInCame::Kind::Neither;
        came.why = kvm::httpsOnly(*to);
        return came;
    }
    if (status == 401 || status == 403) {
        // Every look ends up here: a password the KVM has refused is not hammered at it, and is offered again later
        // each time it is refused once more (SignInPacing).
        pacing.saidNo(out.mark, out.steps);
        came.kind = SignInCame::Kind::Refused;
        return came;
    }
    came.token = status == 200 ? kvmd::tokenFrom(reply.value().body, reply.value().cookies) : QByteArray();
    if (came.token.isEmpty()) {
        // Whatever is at that address did not sign this program in, and not because of the password. Nor is the
        // password sent to it again and again meanwhile (the web's `_neither_yes_nor_no`).
        pacing.unanswered(out.mark, out.steps, out.sent);
        came.kind = SignInCame::Kind::Neither;
        came.why =
            base::Error{"bad_response", status != 200 ? u"Login failed (HTTP %1)"_s.arg(status).toStdString()
                                                      : "Login succeeded but the device returned no session token"};
        return came;
    }
    pacing.accepted(out.mark);
    came.kind = SignInCame::Kind::In;
    return came;
}

void KvmdDevice::signInCame(const SignInCame& came)
{
    if (came.heard) {
        answered();
    }
    const SignInPacing::Mark mark = offered();
    switch (came.kind) {
    case SignInCame::Kind::NotSent:
        signedIn(failed(came.why, false));
        return;
    case SignInCame::Kind::Neither:
        // (The web's `_held_back`.) Not sent again at every look, and said as what stands from now on: whether the KVM
        // is there is asked apart (stillThere), and a question that waited for this sign-in was never sent, which "no
        // answer" would say may have been done.
        m_sentOnToHttps = m_sentOnToHttps || came.why.code == "https_only";
        m_signInError = came.why.code == "https_only" ? came.why
                        : pacing().times(mark) > 0    ? passwordRefused()
                                                      : notSigningIn(came.why.message);
        m_heldAs = pacing().standing(mark);
        m_signInRetryAt = 0;
        signedIn(*m_signInError);
        return;
    case SignInCame::Kind::Refused:
        m_signInError = passwordRefused();
        m_heldAs = pacing().standing(mark);
        m_signInRetryAt = 0;
        signedIn(*m_signInError);
        return;
    case SignInCame::Kind::In:
        break;
    }
    m_signInError.reset();
    m_heldAs.reset();
    m_sentOnToHttps = false; // (kvmd itself signed it in there)
    m_token = came.token;
    // (The first pass after a line's refusal had the one before forgotten: passWasRefused. And what was noted of a pass
    // before this one is not kept beyond it.)
    m_givenAfterALinesRefusal = std::exchange(m_lineHadThePassForgotten, false) ? came.token : QByteArray();
    if (m_lineRefusedUnder != came.token) {
        m_lineRefusedUnder.clear();
    }
    const QPointer<KvmdDevice> self(this);
    signedIn(came.token);
    // (After whoever waited for the pass has it: what they do with it may end this.)
    if (self != nullptr) {
        emit passGiven();
    }
}

void KvmdDevice::signedIn(const base::Result<QByteArray>& token)
{
    m_signingIn = false;
    const auto waiting = std::exchange(m_waitingForToken, {});
    for (const auto& done : waiting) {
        done(token);
    }
}

void KvmdDevice::answered()
{
    m_failures = 0;
    m_downError.reset();
    m_lastOkAt = QDateTime::currentSecsSinceEpoch();
    m_heardAt = now();
}

base::Error KvmdDevice::failed(const base::Error& error, bool inPassing)
{
    if (meansAway(error.code)) {
        m_inPassing = inPassing;
        if (!m_downError || now() >= m_downUntil) {
            // Questions that were out together fail together: one failed attempt, not several.
            ++m_failures;
            const qint64 hold = qint64{m_pace.holdMs} << std::min(m_failures - 1, 10);
            m_downUntil = now() + std::min<qint64>(m_pace.longestHoldMs, hold);
        }
        m_downError = error;
    }
    return error;
}

std::optional<base::Error> KvmdDevice::heldBack() const
{
    if (m_downError && now() < m_downUntil) {
        return m_downError;
    }
    return std::nullopt;
}

void KvmdDevice::identify(std::function<void(base::Result<Identity>)> done)
{
    Ask check;
    check.path = u"/api/auth/check"_s;
    check.urgent = true;
    ask(check, [this, done = std::move(done)](const base::Result<QJsonObject>& in) mutable {
        if (!in) {
            done(in.error());
            return;
        }
        // What only one maker's KVMs answer: one that does not is still the KVM.
        Ask version;
        version.path = u"/api/upgrade/version"_s;
        version.urgent = true;
        request(version, [this, done = std::move(done)](const base::Result<Reply>& reply) mutable {
            const base::Result<std::optional<QJsonObject>> said = familyFrom(reply);
            if (!said) {
                done(said.error());
                return;
            }
            const kvmd::About about = said.value() ? kvmd::aboutFrom(*said.value()) : kvmd::About{};
            Ask name;
            name.path = u"/api/system/get_hostname"_s;
            name.urgent = true;
            ask(name, [about, done = std::move(done)](const base::Result<QJsonObject>& named) {
                if (!named && named.error().code != "bad_response") {
                    done(named.error());
                    return;
                }
                done(Identity{about.model, about.version, named ? kvmd::hostnameFrom(named.value()) : QString()});
            });
        });
    });
}

Status KvmdDevice::status() const
{
    Status status = m_status.value_or(Status{});
    if (m_lastOkAt) {
        const qint64 ago = std::clamp<qint64>(QDateTime::currentSecsSinceEpoch() - *m_lastOkAt, 0, 2'000'000'000);
        status.lastSeenSecondsAgo = static_cast<int>(ago);
    }
    // (As the KVM said last on a line to its events, whichever driver of it heard it.)
    status.fingerbot = fingerbotNow();
    // What is in its drive, and whether it has one, as it was last said (kvmd_media.cpp): of a KVM that answered.
    if (m_status && m_status->errorCode.isEmpty() && m_msd) {
        status.media = kvmd::mountedOf(*m_msd, kvmd::MediaNotes::program(), mediaKey());
        status.offersMedia = mediaOffer().has_value();
    }
    return status;
}

bool KvmdDevice::isStale() const
{
    return !m_status || now() - m_statusAt > m_pace.freshMs;
}

void KvmdDevice::look()
{
    if (m_looking) {
        return;
    }
    m_looking = true;
    ++m_look;
    m_boardWordsAtLook = m_boardWords;
    Ask streamer;
    streamer.path = u"/api/streamer"_s;
    ask(streamer, [this](const base::Result<QJsonObject>& said) {
        if (said) {
            lookFurther(said.value());
            return;
        }
        const base::Error& why = said.error();
        if (why.code == "auth_failed" || why.code == "no_sign_in" || why.code == "https_only") {
            // Nothing was asked that needs a sign-in, and so nothing said whether the KVM is there. (Not
            // where no password is saved, as the web does: a KVM of this wall that has none hears
            // nothing at all, withoutAPasswordNothingIsSent.)
            stillThere([this, held = why](const std::optional<base::Error>& gone) { lookFailed(gone.value_or(held)); });
            return;
        }
        lookFailed(why);
    });
}

void KvmdDevice::lookFailed(const base::Error& why)
{
    Status status;
    if (why.code == "auth_failed" || why.code == "no_password") {
        status.state = u"auth"_s;
    } else if (why.code == "no_sign_in" || why.code == "https_only" || why.code == "cert_mismatch" ||
               why.code == "cert_invalid" || why.code == tlsDamaged) {
        // (Something for whoever sits here to look at, and not a KVM that is away: one that is there
        // and does not sign this program in is not "offline", which an alert tells as gone.)
        status.state = u"error"_s;
    } else {
        status.state = u"offline"_s;
    }
    status.errorCode = QString::fromStdString(why.code);
    status.errorMessage = QString::fromStdString(why.message);
    lookOver(std::move(status));
}

void KvmdDevice::stillThere(std::function<void(std::optional<base::Error>)> then)
{
    // (The web's `_gone_meanwhile`.) While a refused password waits to be offered again, up to an
    // hour, nothing that needs a sign-in is asked, and so nothing said that the KVM went away: its
    // card went on saying that the sign-in failed. This asks what anybody may ask, with no password
    // and no session; what it answers does not matter, only that it does. Not at every look: one
    // that answered of late (with the no, for one) is there.
    if (m_heardAt >= 0 && now() - m_heardAt < m_pace.stillThereEveryMs) {
        then(std::nullopt);
        return;
    }
    if (std::optional<base::Error> away = heldBack()) {
        then(std::move(away));
        return;
    }
    Ask check;
    check.path = u"/api/auth/check"_s;
    send(check, {}, [this, then = std::move(then)](const base::Result<Reply>& reply) {
        if (!reply && meansAway(reply.error().code)) {
            then(failed(reply.error(), false));
            return;
        }
        if (reply) {
            answered();
            // (A redirect heard here is heard as one a question with a pass heard: the next sign-in asks first with no
            // password, however lately this answer came. Round 2 of the merged review of 0.2.0, L1.)
            if (sentOn(reply.value().status, reply.value().location)) {
                m_sentOnToHttps = true;
            }
        }
        then(std::nullopt);
    });
}

void KvmdDevice::lookFurther(const QJsonObject& streamer)
{
    // The KVM is there: it has just answered. Whether each of these details comes changes what a
    // card can tell, and not whether the KVM counts as away, which would take its picture away.
    struct Gathered {
        kvmd::Looked looked;
        int out = 0;
    };
    auto gathered = std::make_shared<Gathered>();
    gathered->looked.streamer = streamer;
    gathered->looked.about = m_about;
    const auto came = [this, gathered] {
        if (--gathered->out > 0) {
            return;
        }
        m_sourceWidth = kvmd::sourceWidthFrom(gathered->looked.streamer);
        Status status = kvmd::statusFrom(gathered->looked);
        if (gathered->looked.ports && m_portSeen != std::optional<int>(gathered->looked.ports->shown)) {
            // Another computer is on its screen than this driver knew of: somebody pressed the
            // button on the KVM. A KVM with several goes on saying "no keyboard" after that until
            // a key has been sent: that is not knowing, and is told as not knowing, not as a cable
            // that is out. And what was seen of its screen was the other computer's. (The first
            // look of all finds which one is there, and nothing was seen of another.)
            m_usbUnsure = m_portSeen.has_value();
            if (m_portSeen) {
                forgetWhatWasSeen();
            }
            m_portSeen = gathered->looked.ports->shown;
        }
        if (gathered->looked.hid) {
            // (Only a plain yes or no says how the mouse is set.)
            const QJsonValue absolute = gathered->looked.hid->value("mouse"_L1).toObject().value("absolute"_L1);
            m_mouseAbsolute = absolute.isBool() ? std::optional<bool>(absolute.toBool()) : std::nullopt;
        }
        if (status.keyboard == std::optional<bool>(true)) {
            m_usbUnsure = false;
        } else if (m_usbUnsure) {
            status.keyboard.reset();
        }
        keepTheBoard(status);
        lookOver(std::move(status));
    };
    const auto detail = [this, gathered, came](const QString& path, std::optional<QJsonObject> kvmd::Looked::* into) {
        ++gathered->out;
        Ask one;
        one.path = path;
        one.vital = false;
        ask(one, [gathered, came, into](const base::Result<QJsonObject>& said) {
            if (said) {
                gathered->looked.*into = said.value();
            }
            came();
        });
    };
    ++gathered->out; // held until every question is out, so that none of them ends the look alone
    detail(u"/api/info"_s, &kvmd::Looked::info);
    detail(u"/api/hid"_s, &kvmd::Looked::hid);
    // The board as its line for events said it last, where that says the board is there; asked otherwise (a
    // Comet's line says "no board" too while the board answers somebody else's questions, which says nothing of it).
    if (const std::optional<QJsonObject> heard = boardOnTheLine(); heard && kvmd::boardIsThere(*heard)) {
        gathered->looked.atx = *heard;
    } else {
        detail(u"/api/atx"_s, &kvmd::Looked::atx);
    }
    if (!m_aboutAt || now() - *m_aboutAt > m_pace.aboutFreshMs) {
        ++gathered->out;
        Ask version;
        version.path = u"/api/upgrade/version"_s;
        version.vital = false;
        request(version, [this, gathered, came](const base::Result<Reply>& reply) {
            // (A KVM of the maker that has no such thing says so every time: asked rarely, too.)
            if (const base::Result<std::optional<QJsonObject>> said = familyFrom(reply)) {
                m_about = said.value() ? kvmd::aboutFrom(*said.value()) : kvmd::About{};
                m_aboutAt = now();
                gathered->looked.about = m_about;
            }
            came();
        });
    }
    if (!m_onePortAt || now() - *m_onePortAt > m_pace.aboutFreshMs) {
        ++gathered->out;
        Ask ports;
        ports.path = u"/api/switch"_s;
        ports.vital = false;
        ask(ports, [this, gathered, came](const base::Result<QJsonObject>& said) {
            if (said || said.error().code == "bad_response") {
                gathered->looked.ports = said ? kvmd::portsFrom(said.value()) : std::nullopt;
                // It said that it has one computer: it is not asked every few seconds.
                m_onePortAt = gathered->looked.ports ? std::nullopt : std::optional<qint64>(now());
            }
            came();
        });
    }
    // Its drive (what is mounted, and whether it has one), where its line has not said it of late and it was not asked
    // within Pace::msdEveryMs; one that did not answer is asked again after that pause, not at every look. Asked beside
    // the look, last, and not waited for: a GL.iNet KVM looks at its storage's partitions for every such answer, and
    // the card is told when what it says of the drive changes (driveSaid).
    if (!m_askingTheDrive && driveIsDue()) {
        m_askingTheDrive = true;
        askTheDrive(false, false, [this](const base::Result<QJsonObject>& said) {
            m_askingTheDrive = false;
            if (!said) {
                m_msdAt = now();
            }
        });
    }
    came();
}

base::Result<std::optional<QJsonObject>> KvmdDevice::familyFrom(const base::Result<Reply>& reply)
{
    if (!reply) {
        return reply.error();
    }
    // Stock kvmd has none of GL.iNet's routes, and says so before it asks who asks (kvmd 4.226).
    if (reply.value().status == 404) {
        m_glinet = false;
        return std::optional<QJsonObject>();
    }
    // Anything else that is not GL.iNet's answer says neither (an error of the moment, an answer of another kind):
    // what was known stays as it was.
    if (reply.value().status != 200) {
        return std::optional<QJsonObject>();
    }
    const base::Result<QJsonObject> said = kvmd::resultFrom(reply.value().body);
    if (!said) {
        return std::optional<QJsonObject>();
    }
    m_glinet = true;
    return std::optional<QJsonObject>(said.value());
}

void KvmdDevice::keepTheBoard(Status& status)
{
    // (The web's BOARD_KEPT and POWER_KEPT.) Only an answer in which the board says it is there says what it says of
    // the computer (kvmd::statusFrom). One that says it is not, or did not come, from a KVM whose board was there a
    // moment ago leaves the power buttons where they are, and what the board said last of the computer for a while: a
    // Comet's board says "none" and "off" to whoever asks while it answers somebody else (two askers at once on a real
    // one: one answer in five), and the card's buttons went with each such answer.
    if (status.powerIsKnown) {
        m_boardAt = now();
        m_boardPower = status.power;
        return;
    }
    if (m_boardAt && now() - *m_boardAt < m_pace.boardKeptMs) {
        status.powerIsKnown = true;
        status.power = now() - *m_boardAt < m_pace.powerKeptMs ? m_boardPower : std::nullopt;
    }
}

void KvmdDevice::keepWith(std::shared_ptr<KvmdKept> kept)
{
    m_kept = std::move(kept);
    if (m_kept && !m_kept->shownSeveralBy.isEmpty() && m_kept->shownSeveralBy == keptKey()) {
        m_shownSeveral = true;
    }
}

QByteArray KvmdDevice::keptKey() const
{
    return m_where.address.toString(QUrl::FullyEncoded).toUtf8() + ' ' +
           (m_where.knownByCertificate ? m_where.certificate.toHex() : "windows"_ba);
}

void KvmdDevice::noteThePower(Status& status)
{
    // (The web's POWER_LED_UNWIRED, web 0.3.2.) A board that says the computer is off (its light, or GLKVM's word)
    // while the computer sends a picture is no witness of the power: its card says neither on nor off then. Why, in
    // the wall's words, beside the board's word. Not on a KVM that has ever shown this driver several inputs (this
    // look's among them): its board may be wired to another computer than the one on screen, and the wires may be as
    // they should; nor after one answer of its switch that was no yes, which has it taken for a KVM of one input for a
    // while (Pace::aboutFreshMs). Its card says "Power unknown" there, without a why, while the look says its inputs,
    // and nothing of the power after such an answer. Its short press is refused all the same (noWitnessOfThePower).
    if (!status.ports.isEmpty()) {
        m_shownSeveral = true;
        if (m_kept) {
            m_kept->shownSeveralBy = keptKey();
        }
    }
    if (status.powerIsKnown && status.power == std::optional<bool>(false) &&
        status.signal == std::optional<bool>(true) && !m_shownSeveral) {
        status.powerNoteCode = u"power_led_unwired"_s;
        status.powerNote = u"The KVM's board says the power light is off while the screen shows a picture: its "
                           "power-LED wires are probably not connected."_s;
    }
}

void KvmdDevice::lookOver(Status status)
{
    // What the KVM's state said of its video, and what it left unsaid: there what the last
    // picture said stands in.
    const bool answered = status.errorCode.isEmpty();
    m_stateSaysSignal = answered && status.signal.has_value();
    m_stateSaysSize = answered && status.width > 0 && status.height > 0;
    static_cast<void>(standIn(status));
    // (By what the look and the last picture say of the picture.)
    noteThePower(status);
    m_status = std::move(status);
    m_statusAt = now();
    if (m_boardWords != m_boardWordsAtLook) {
        // The board said something on its line while the look was under way, which the look may have read before it:
        // looked at again when next asked.
        m_statusAt = now() - m_pace.freshMs - 1;
    }
    m_looking = false;
    // A Fingerbot's battery, where it is time to ask it again.
    batterySoon();
    emit statusChanged();
    emit looked();
}

void KvmdDevice::afterLook(int withinMs, std::function<void(bool stillLooking)> then)
{
    if (!m_looking) {
        later([then = std::move(then)] { then(false); });
        return;
    }
    // Whichever of the two comes first, and that one alone.
    auto over = std::make_shared<bool>(false);
    auto heard = std::make_shared<QMetaObject::Connection>();
    const auto run = [this, over, heard, then = std::move(then)] {
        if (std::exchange(*over, true)) {
            return;
        }
        disconnect(*heard);
        then(m_looking);
    };
    *heard = connect(this, &KvmdDevice::looked, this, run);
    QTimer::singleShot(withinMs, this, run);
}

bool KvmdDevice::standIn(Status& status) const
{
    // Only for a KVM that answered, and only while the picture that said it is new: what a
    // picture said an hour ago says nothing of a computer that may have been switched off, or
    // set to another size, since.
    if (!status.errorCode.isEmpty() || !m_sight) {
        return false;
    }
    const auto isNew = [this](qint64 at) { return now() - at <= m_pace.restAfterMs; };
    bool stoodIn = false;
    if (!m_stateSaysSignal && m_sight->signal.has_value() && isNew(m_sight->at) && status.signal != m_sight->signal) {
        // (As the state says it where it does: kvmd_answers.cpp.)
        status.signal = m_sight->signal;
        status.state = *m_sight->signal ? u"online"_s : u"no_signal"_s;
        stoodIn = true;
    }
    // (The size by the time of the picture that said it, and not of the last that came.)
    const QSize size = m_sight->size;
    if (!m_stateSaysSize && size.isValid() && isNew(m_sight->sizeAt) &&
        (status.width != size.width() || status.height != size.height())) {
        status.width = size.width();
        status.height = size.height();
        stoodIn = true;
    }
    return stoodIn;
}

void KvmdDevice::pictureSays(std::optional<bool> signal, QSize size)
{
    Sight sight{signal, now(), size, now()};
    if (!size.isValid() && m_sight) {
        // The size the computer sent last stays what is known of it under a picture that says
        // none, with the time it was said at: such a picture does not make it new.
        sight.size = m_sight->size;
        sight.sizeAt = m_sight->sizeAt;
    }
    m_sight = sight;
    if (m_status && standIn(*m_status)) {
        emit statusChanged();
    }
}

void KvmdDevice::forgetWhatWasSeen()
{
    m_frames.clear();
    m_unseen.clear();
    m_sight.reset();
    // (A picture that is on its way was asked of the screen as it was: handed to whoever
    // waits for it, and not kept.)
    ++m_screen;
}

bool KvmdDevice::sawItsVideoAwake() const
{
    return m_sight && now() - m_sight->at <= m_pace.freshMs;
}

void KvmdDevice::expectUp(const std::function<void(std::optional<base::Error>)>& then)
{
    // Cards ask for pictures all the time, and a KVM that is not there takes seconds to fail.
    // Answered at that pace, a few dead KVMs hold up the pictures of all the live ones. So
    // finding out whether a KVM is there is the look's part, and a card is told at once what
    // is known.
    const auto further = [this, then] {
        if (m_downError && now() >= m_downUntil) {
            // The pause after the failure is over: time to look again.
            look();
            const bool there = m_status && m_status->state != "offline"_L1 && m_status->state != "connecting"_L1;
            if (m_inPassing && there && m_failures < 2 && m_look != m_awaitedLook) {
                // One failed action is not an outage: it took too long, or went out on a line the
                // KVM had closed. A KVM that was there at last sight says so again within moments,
                // and has its picture taken after all. That is waited for once.
                m_awaitedLook = m_look;
                afterLook(m_pace.firstContactMs, [this, then](bool) { then(m_downError); });
                return;
            }
        }
        then(m_downError);
    };
    if (!m_downError && !m_lastOkAt) {
        // Never heard from: the look makes the first contact, and gets a moment for it.
        if (isStale()) {
            look();
        }
        if (m_looking) {
            afterLook(m_pace.firstContactMs, [then, further](bool stillLooking) {
                if (stillLooking) {
                    then(slow("Connecting to the KVM"));
                } else {
                    further();
                }
            });
            return;
        }
    }
    further();
}

void KvmdDevice::askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done)
{
    askPicture(width, false, std::move(done));
}

void KvmdDevice::askNewSnapshot(int width, std::function<void(base::Result<Snapshot>)> done)
{
    askPicture(width, true, std::move(done));
}

void KvmdDevice::askPicture(int width, bool anew, std::function<void(base::Result<Snapshot>)> done)
{
    m_wantedAt = now();
    const qint64 arrived = m_wantedAt;
    // Always a turn of the loop later, whichever way it comes about.
    const auto hand = [this, done = std::move(done)](const base::Result<Snapshot>& result) {
        later([done, result = base::Result<Snapshot>(result)] { done(result); });
    };
    expectUp([this, width, anew, arrived, hand](const std::optional<base::Error>& away) {
        if (away) {
            hand(*away);
            return;
        }
        picture(kvmd::sizeFor(width, m_sourceWidth), arrived, anew, hand);
    });
}

void KvmdDevice::picture(const QString& size, qint64 arrived, bool anew,
                         const std::function<void(base::Result<Snapshot>)>& done)
{
    // A picture nobody was handed yet is new to whoever asks. Otherwise its age counts from when
    // it was asked for to when this one was: nobody is handed, as new, the picture of last time.
    // And whoever asks for a new one is not handed one that somebody had before, however
    // closely the two askings follow on each other: that is five pictures a second at the
    // most, where a KVM gives one as captured in a few hundredths of a second.
    //
    // Nor is "nobody was handed it yet" new for ever. It lands after whoever asked had given up,
    // and is kept for as long as any picture of the KVM is wanted: whoever asked for a new one
    // minutes later was handed it at once, a live view's first picture, and took it for the
    // sign that the KVM's video is awake. So whoever asks for a new one is handed it only where
    // it was asked of the KVM after this asking began, or came no longer ago than a picture
    // that came says how the KVM is (Pace::freshMs, by which the stream's question judges the
    // same: sawItsVideoAwake). What is older is thrown away here. (Counted from when it came,
    // not from when it was asked for: a KVM that takes seconds over a picture has none that
    // is newer than that.)
    if (const auto frame = m_frames.constFind(size); frame != m_frames.constEnd()) {
        const bool unseen = m_unseen.contains(size);
        const bool tooOld = anew && unseen && frame->askedAt < arrived && now() - frame->cameAt > m_pace.freshMs;
        if (tooOld) {
            m_unseen.remove(size);
            m_frames.remove(size);
        } else if (unseen || (!anew && arrived - frame->askedAt < sameLookMs)) {
            const Snapshot kept = frame->picture;
            m_unseen.remove(size);
            done(kept);
            return;
        }
    }
    if (const auto refused = m_frameErrors.constFind(size); refused != m_frameErrors.constEnd()) {
        // A stream that is waking up has pictures within moments: not a whole pause for that.
        const int hold = refused->second.code == "no_stream" ? m_pace.holdMs / 2 : m_pace.holdMs;
        if (now() - refused->first < hold) {
            done(refused->second);
            return;
        }
    }
    const bool mine = !m_fetching.contains(size);
    if (mine) {
        m_fetching.insert(size, arrived);
        fetch(size, arrived);
    }
    // The patience is with the KVM, not with whoever asks: a question that has been with the KVM
    // for long already is not waited for by one after the other.
    const qint64 patience = std::min(arrived, m_fetching.value(size)) + m_pace.patienceMs - now();
    if (patience <= 0) {
        done(slow("The KVM is slow to answer"));
        return;
    }
    // The picture or the end of the patience, whichever comes first, and that one alone.
    auto over = std::make_shared<bool>(false);
    m_waitingForFrame[size].append([this, over, mine, anew, size, arrived, done](const base::Result<Snapshot>& result) {
        if (std::exchange(*over, true)) {
            return;
        }
        if (mine || anew) {
            // Its own question; or one that was under way when a new picture was asked for,
            // which that asking joined (the KVM is not asked twice at once for one size): what it
            // brought came after the asking, and is new to whoever asked.
            m_unseen.remove(size);
            done(result);
        } else {
            // Somebody else's question, now over: whether what it brought will do is looked
            // at anew.
            picture(size, arrived, false, done);
        }
    });
    QTimer::singleShot(patience, this, [over, done] {
        if (std::exchange(*over, true)) {
            return;
        }
        // The question to the KVM carries on by itself, and what it brings goes to whoever asks next.
        done(slow("The KVM is slow to answer"));
    });
}

void KvmdDevice::fetch(const QString& size, qint64 asked)
{
    Ask question;
    question.path = u"/api/streamer/snapshot"_s;
    question.query = kvmd::snapshotQuery(size);
    question.largest = kvmd::largestPictureBytes;
    // (One the KVM makes smaller for a card has another size than the computer's own picture.)
    const bool asCaptured = !question.query.hasQueryItem(u"preview"_s);
    request(question, [this, size, asked, asCaptured, screen = m_screen](const base::Result<Reply>& reply) {
        const auto none = [this, &size](const base::Error& why) {
            if (!meansAway(why.code)) { // those are held back for every question alike
                m_frameErrors.insert(size, {now(), why});
            }
            landed(size, why);
        };
        if (!reply) {
            none(reply.error());
            return;
        }
        const Reply& said = reply.value();
        if (said.status == 503) {
            none(resting());
            return;
        }
        if (said.status != 200) {
            none({"bad_response", u"Snapshot failed (HTTP %1)"_s.arg(said.status).toStdString()});
            return;
        }
        const std::optional<QSize> stated = kvmd::pictureSize(said.body);
        const bool isPicture = said.kind.isEmpty() || said.kind == "image/jpeg"_ba || said.kind == "image/png"_ba;
        if (!isPicture || !stated) {
            none({"bad_response", "The device sent something that is not a picture"});
            return;
        }
        if (!kvmd::isShown(*stated)) {
            // Small to send, enormous to draw.
            none({"bad_response",
                  u"The device sent a picture of %1 by %2 pixels, which is not shown"_s.arg(stated->width())
                      .arg(stated->height())
                      .toStdString()});
            return;
        }
        Snapshot picture;
        picture.picture = said.body;
        picture.signal = said.online != "false"_ba;
        m_frameErrors.remove(size);
        if (screen == m_screen) { // else it may show the computer the KVM was switched away from meanwhile
            m_frames.insert(size, Frame{picture, asked, now()});
            m_unseen.insert(size);
            // What it says of the computer's video, for where the KVM's state says nothing:
            // whether the computer sends a picture, where it came with kvmd's word on that; and
            // its size, where it is the computer's own picture as the KVM captured it, and not
            // one the KVM sends in the place of none.
            pictureSays(said.online.isEmpty() ? std::nullopt : std::optional<bool>(picture.signal),
                        asCaptured && picture.signal ? *stated : QSize());
        }
        landed(size, picture);
    });
}

void KvmdDevice::landed(const QString& size, const base::Result<Snapshot>& result)
{
    m_fetching.remove(size);
    const auto waiting = m_waitingForFrame.take(size);
    for (const auto& one : waiting) {
        one(result);
    }
}

QSslConfiguration KvmdDevice::knownByItsCertificateAlone(QSslConfiguration line)
{
    line = kvm::knownByItsCertificateAlone(line);
    return line;
}

std::unique_ptr<KvmdStream> KvmdDevice::watch()
{
    // Somebody wants its picture: as with a snapshot, its video is not let rest at once when
    // the stream has ended, since whoever was told to ask again soon does. (The stream notes
    // that moment itself when it ends. And where one is open already, what is made here asks
    // nothing and says "busy": the stream sees that for itself.)
    m_wantedAt = now();
    return std::make_unique<KvmdStream>(KvmdStream::Made{}, this);
}

base::Error KvmdDevice::resting()
{
    // kvmd rests its video while nobody is connected, and has no picture until somebody is.
    // And where this program is connected and it rests all the same, the line is one
    // that ended without a word (the KVM lost its power, a router forgot the line):
    // a new one.
    if (m_awake && now() - m_awakeSince > m_pace.quietMs) {
        m_awake.release()->deleteLater();
    }
    keepAwake();
    return slow("Video stream is idle - waking it up");
}

void KvmdDevice::keepAwake()
{
    if (m_awake || m_token.isEmpty()) {
        return;
    }
    AwakeLine::Where where;
    where.address = m_where.address;
    // GL.iNet's own page hands the pass over both ways, and so does this to a KVM known to be GL.iNet's; to any other
    // in the cookie alone (stock kvmd writes the address of every request into its log: SPEC.md section 8).
    where.asking =
        "/api/ws?stream=1"_ba + (m_glinet == std::optional<bool>(true) ? "&auth_token="_ba + m_token : QByteArray());
    where.cookie = "auth_token="_ba + m_token;
    where.knownByCertificate = m_where.knownByCertificate;
    where.certificate = m_where.certificate;
    m_awake = std::make_unique<AwakeLine>(std::move(where), m_pace.connectWithinMs);
    m_awakeSince = now();
    AwakeLine* line = m_awake.get();
    // It is the KVM's line for events: its power board says its state there, which a look reads there and does not
    // ask (boardOnTheLine). What a line before it said is no word of this one's.
    m_lineBoard.reset();
    // (Up to the longest state of its drive: kvmd_media.h.)
    line->hearWhatItSays(kvmd::mostMsdWord, m_pace.eventsPingMs);
    connect(line, &AwakeLine::heard, this, [this, line] {
        if (m_awake.get() == line) {
            m_eventsHeardAt = now();
        }
    });
    connect(line, &AwakeLine::said, this, [this, line](const QByteArray& message) {
        if (m_awake.get() == line) {
            heardOnTheLine(message);
        }
    });
    connect(line, &AwakeLine::ended, this, [this, line] {
        if (m_awake.get() == line) {
            m_awake.release()->deleteLater();
            // (Whoever waits for the board's next word there asks it instead.)
            emit boardSaid();
        }
    });
    line->open();
    m_awakeTimer.start();
}

bool KvmdDevice::noWitnessOfThePower(std::optional<bool> boardSays) const
{
    // Off, beside a picture (by the last look): its power light probably not wired (the card's "Power unknown" with its
    // why, noteThePower), or, on a KVM that has shown several inputs, its board perhaps wired to another computer than
    // the one on screen (the card's "Power unknown" alone, or nothing of the power where its switch did not answer the
    // last look). (The merged review of 0.2.0, A-M1: the second was pressed.)
    return boardSays == std::optional<bool>(false) && m_status && m_status->signal == std::optional<bool>(true);
}

base::Error KvmdDevice::powerUnknown() const
{
    if (m_shownSeveral) {
        return {"power_unknown",
                "Nothing was pressed: whether this machine is on is not known. Its KVM's board says the power light is "
                "off while the screen shows a picture, and on a KVM with several inputs the board may be wired to "
                "another computer than the one on screen, so a press of the power button could shut a running machine "
                "down. A hold of the power button still goes; to switch on the computer the board is wired to, show "
                "its input first and press again."};
    }
    return {"power_unknown",
            "Nothing was pressed: whether this machine is on is not known. Its KVM's board says the power light is off "
            "while the screen shows a picture, so its power-LED wires are probably not connected, and a press of the "
            "power button could shut a running machine down."};
}

void KvmdDevice::act(const Action& what, std::function<void(base::Result<Done>)> done)
{
    // (The owner's rule of 2026-11-19, stricter than the web.) Where its board is no witness of the power (the board
    // says off beside a picture: noWitnessOfThePower), a short press of the power button, its board's or its
    // Fingerbot's, may shut a running machine down where it was to switch it on: refused, with nothing sent. A hold of
    // the power button cuts the power, and a reset resets, whatever the machine is doing: those go as before. (kvmd's
    // own /api/atx/power, on, off, off_hard and reset_hard, is never asked: kvmd_answers.cpp.)
    if ((what.kind == "atx_power"_L1 || what.kind == "fingerbot_power"_L1) && m_status && m_status->powerIsKnown &&
        noWitnessOfThePower(m_status->power)) {
        later([done = std::move(done), why = powerUnknown()] { done(why); });
        return;
    }
    if (pressesTheFingerbot(what.kind)) {
        pressFingerbot(what.kind, std::move(done));
        return;
    }
    const base::Result<kvmd::Call> call = kvmd::callFor(what, m_status ? static_cast<int>(m_status->ports.size()) : 0);
    if (!call) {
        later([done = std::move(done), error = call.error()] { done(error); });
        return;
    }
    Ask question;
    question.method = "POST"_ba;
    question.path = call.value().path;
    question.query = call.value().query;
    question.body = call.value().body;
    question.bodyIs = "text/plain; charset=utf-8"_ba;
    question.quietMs = call.value().quietMs;
    question.urgent = true;
    question.doesSomething = true;
    question.types = what.kind == "text"_L1 || what.kind == "keys"_L1 || what.kind == "ctrl_alt_del"_L1;
    if (pressesTheBoard(what.kind)) {
        pressBoard(what.kind, std::move(question), std::move(done));
        return;
    }
    request(question,
            [this, kind = what.kind, port = what.port, done = std::move(done)](const base::Result<Reply>& reply) {
                if (!reply) {
                    // A KVM that is already going down may never answer its own restart.
                    if (kind == "reboot"_L1 && (reply.error().code == "dropped" || reply.error().code == "timeout")) {
                        done(Done{});
                        return;
                    }
                    done(reply.error());
                    return;
                }
                if (reply.value().status != 200) {
                    base::Error refusal = kvmd::refusalOf(reply.value().status, reply.value().body);
                    if (reply.value().status >= 500) {
                        // An error of the KVM's own, or of what stands before it, in the middle of what
                        // was asked: that is no "no". A long text may be typed on while the answer to
                        // it has been given up.
                        refusal.code = "action_unknown";
                    }
                    done(refusal);
                    return;
                }
                if (kind == "port"_L1) {
                    // What was on its screen belongs to the other computer now.
                    forgetWhatWasSeen();
                    if (m_status && port >= 0 && port < m_status->ports.size()) {
                        m_status->shownPort = port;
                        // Which computer is there is known from here on: the look that follows
                        // finds the one this driver put there, and what a picture has said of
                        // that one meanwhile is not dropped a second time. (Its keyboard is not
                        // known there until it is found, as after every switch: lookFurther.)
                        m_portSeen = port;
                        m_usbUnsure = true;
                    }
                    m_statusAt = now() - m_pace.freshMs - 1;
                    emit statusChanged();
                }
                done(Done{});
            });
}

void KvmdDevice::stopTyping()
{
    // (From a list of their own: whoever is answered may ask for another, which waits as this one did.)
    const base::Error stopped{"stopped", "It was not done: whoever asked for it may no longer do that"};
    const auto waiting = std::exchange(m_typingsWaiting, {});
    for (const auto& typing : waiting) {
        if (*typing) {
            std::exchange(*typing, {})(stopped);
        }
    }
}

std::unique_ptr<OwnLive> KvmdDevice::makeLive()
{
    return std::make_unique<KvmdLive>(*this);
}

bool KvmdDevice::mayHaveBeenDone(const base::Error& error)
{
    // The question went out, and what came of it is not known. ("unreachable" is before anything
    // went out, and a certificate that is not the KVM's is before that.)
    return error.code == "timeout" || error.code == "dropped" || error.code == "action_unknown";
}

void KvmdDevice::openVideoLine(std::function<void(base::Result<std::unique_ptr<DeviceLine>>)> done)
{
    withToken([this, done = std::move(done)](const base::Result<QByteArray>& token) {
        if (!token) {
            done(token.error());
            return;
        }
        const QByteArray& pass = token.value();
        DeviceLine::Where where;
        where.address = m_where.address;
        where.asking = "/janus/ws"_ba;
        where.cookie = "auth_token="_ba + pass;
        where.knownByCertificate = m_where.knownByCertificate;
        where.certificate = m_where.certificate;
        DeviceLine::Limits limits;
        limits.letInWithinMs = m_pace.connectWithinMs;
        limits.largestFrameBytes = qint64{128} * 1024;
        limits.mostUnsentBytes = qint64{128} * 1024;
        auto line = std::make_unique<DeviceLine>(std::move(where), limits);
        line->receiveText("janus-protocol"_ba);
        m_videoLines.removeIf([](const auto& old) { return old == nullptr; });
        m_videoLines.append(line.get());
        connect(line.get(), &DeviceLine::letIn, this, [this] {
            m_wantedAt = now();
            keepAwake();
        });
        connect(line.get(), &DeviceLine::ended, this, [this, pass](DeviceLine::End why) {
            // (The web's _line: a session refused as not signed in, or as not allowed, is replaced once.)
            const bool refused = why == DeviceLine::End::SignedOut || why == DeviceLine::End::Refused;
            if (refused && m_token == pass && now() >= m_videoAuthRetryAt) {
                m_token.clear();
                m_videoAuthRetryAt = now() + m_pace.refusedHoldMs;
            }
        });
        connect(this, &QObject::destroyed, line.get(), &DeviceLine::cut);
        done(std::move(line));
    });
}

void KvmdDevice::openConsole(std::function<void(base::Result<std::unique_ptr<KvmdConsoleLine>>)> done)
{
    withToken([this, done = std::move(done)](const base::Result<QByteArray>& token) {
        if (!token) {
            done(token.error());
            return;
        }
        const QByteArray& pass = token.value();
        // Under this very pass the KVM refused the line a moment ago, though it had only just
        // given the pass: it is not asked again so soon, and no password goes out.
        if (pass == m_lineRefusedUnder && now() < m_lineRefusedUntil) {
            done(lineNotAllowed());
            return;
        }
        KvmdConsoleLine::Where where;
        where.address = m_where.address;
        // Who asks is said as for the line that keeps the video awake: in the cookie, and in the address as well
        // only to a KVM known to be GL.iNet's, as its own page says it (SPEC.md section 8). "stream=0": this line
        // watches nothing.
        where.query = "stream=0"_ba + (m_glinet == std::optional<bool>(true) ? "&auth_token="_ba + pass : QByteArray());
        where.cookie = "auth_token="_ba + pass;
        where.knownByCertificate = m_where.knownByCertificate;
        where.certificate = m_where.certificate;
        auto line = std::make_unique<KvmdConsoleLine>(std::move(where));
        // A line that the KVM lets in under a pass says that it does not refuse the line under
        // that one: what was noted of it for the next refusal is noted no more.
        connect(line.get(), &KvmdConsoleLine::opened, this, [this, pass] {
            if (m_givenAfterALinesRefusal == pass) {
                m_givenAfterALinesRefusal.clear();
            }
            if (m_lineRefusedUnder == pass) {
                m_lineRefusedUnder.clear();
            }
        });
        done(std::move(line));
    });
}

void KvmdDevice::askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done)
{
    Ask list;
    list.path = u"/api/wol/list"_s;
    list.urgent = true;
    ask(list, [done = std::move(done)](const base::Result<QJsonObject>& said) {
        if (said) {
            done(kvmd::wakeTargetsFrom(said.value()));
        } else if (said.error().code == "bad_response") {
            done(QList<WakeTarget>{}); // a KVM of the maker that keeps no such list
        } else {
            done(said.error());
        }
    });
}

void KvmdDevice::tidy()
{
    if (now() - m_wantedAt > m_pace.restAfterMs) {
        // Nobody is looking: the last pictures, a megabyte each and more, need not be kept.
        m_frames.clear();
        m_unseen.clear();
        m_frameErrors.clear();
    }
}

std::optional<base::Error> KvmdDevice::passWasRefused(const QByteArray& pass)
{
    // Only the one that is held: a line that was made with an older pass says nothing of the
    // one that replaced it.
    if (pass.isEmpty() || m_token != pass) {
        return std::nullopt;
    }
    // Refused under a pass the KVM has only just given, or under which it refused the line in
    // this way before: it is the line that it refuses, and not the pass, which every other
    // question is served under. Given up for lost, it had every asking for keyboard and mouse
    // throw a session away that works, and post the password again.
    if (pass == m_givenAfterALinesRefusal || pass == m_lineRefusedUnder) {
        m_lineRefusedUnder = pass;
        m_lineRefusedUntil = now() + m_pace.signInRetryMs;
        return lineNotAllowed();
    }
    // The session ran out, or kvmd was started anew, for all that is known. What follows is the
    // next question's: it signs in anew (withToken), and the pass it is given is the one a line
    // is next asked for with.
    m_token.clear();
    m_lineHadThePassForgotten = true;
    return std::nullopt;
}

} // namespace qkvm::core
