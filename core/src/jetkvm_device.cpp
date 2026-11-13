#include <qkvm/core/jetkvm_device.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <qkvm/core/device_line.h>
#include <qkvm/core/jetkvm_layouts.h>
#include <qkvm/core/jetkvm_live.h>
#include <qkvm/core/jetkvm_typing.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/typed_keys.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The most of an answer: of a JetKVM that is known, and of its status, which anybody may ask (the web's
// MAX_REPLY and MAX_PROBE).
constexpr qsizetype largestReplyBytes = qsizetype{2} * 1024 * 1024;
constexpr qsizetype largestStatusBytes = qsizetype{64} * 1024;

// The JetKVM's own words where they are the web's (jetkvm.py).
base::Error passwordRefused()
{
    return {"auth_failed", "Login rejected - check the password"};
}
base::Error refusingSignIns()
{
    return {"auth_failed", "The JetKVM is refusing logins after too many wrong passwords - try again later"};
}
base::Error keepsRefusingTheSession()
{
    return {"auth_failed", "The JetKVM keeps rejecting the session"};
}
base::Error noPasswordSaved()
{
    return {"no_password", "This JetKVM asks for a password and none is saved"};
}
// Its JetKVM let anybody in when it was added: one that asks for a password now may be its own (its owner switched one
// on), or another that took its address. Which, it says only once signed in (the web's _rejected).
base::Error noPasswordSavedForOneKnown()
{
    return {
        "no_password",
        "The JetKVM at this address asks for a password now, and this card has none saved: it may be another JetKVM "
        "than the one this card was added for, which had none. Type its password only if it is this card's "
        "JetKVM."};
}
base::Error notToldWhichItIs(int status)
{
    return {"bad_response", u"The JetKVM did not say which it is (HTTP %1)"_s.arg(status).toStdString()};
}
base::Error notSetUp()
{
    return {"not_set_up", "This JetKVM has not been set up yet. Open its own page once and choose how it is signed "
                          "in to; then it can be added here."};
}
// What stands against a JetKVM that is there and does not sign this program in, for another reason than
// the password: as a kvmd's (kvmd_device.cpp), never a question that may have been done.
base::Error notSigningIn(const std::string& why)
{
    return {"no_sign_in", "The device does not sign qKVM in (" + why + "). It is asked again shortly."};
}

// Over HTTPS once, and now it answers only the plain way at its own host: said on its card, in the web's
// two sentences (jetkvm.py, _only_plain), with what this program has one do instead of "change its
// address", which it refuses at the place a KVM is at (LocalWall::intoTheClear): take it off the wall and
// add it again. With a password, that the password would travel unencrypted; without, that nothing would
// be checked.
base::Error answersOnlyThePlainWay(const QUrl& plain, bool withAPassword)
{
    const QString there = plain.toString(QUrl::FullyEncoded);
    const QString said =
        withAPassword
            ? u"This JetKVM no longer answers over HTTPS, only over plain HTTP. Its password would travel unencrypted "
              "there: to allow that, take it off this wall and add it again as %1, and type its password again."_s.arg(
                  there)
            : u"This JetKVM no longer answers over HTTPS, only over plain HTTP. Nothing would be checked or encrypted "
              "there: to use it that way, take it off this wall and add it again as %1."_s.arg(there);
    return {"plain_only", said.toStdString()};
}

// Failures of a look that mean it is not there at all (a certificate that is not its own is not that:
// something answers, and it is something to look at, not a way to look for).
bool meansGone(const std::string& code)
{
    return code == "unreachable" || code == "timeout" || code == "dropped";
}

// Failures that mean "the JetKVM is not there, or is not the JetKVM", and not "it said no".
bool meansAway(const std::string& code)
{
    return code == "unreachable" || code == "timeout" || code == "dropped" || code == "cert_mismatch" ||
           code == "cert_invalid";
}

// Why its signalling line ended before it said what it is, in the drivers' words.
base::Error whyTheLineEnded(DeviceLine::End why)
{
    switch (why) {
    case DeviceLine::End::NotAsked:
        return {"bad_response", "What was to be asked of the JetKVM is not fit to be sent"};
    case DeviceLine::End::TlsDamaged:
        return {tlsDamaged, tlsDamagedSentence};
    case DeviceLine::End::NotReached:
        return DeviceAsker::unreachable();
    case DeviceLine::End::CertificateUnread:
        return DeviceAsker::certificateNotRead();
    case DeviceLine::End::CertificateChanged:
        return DeviceAsker::certificateChanged();
    case DeviceLine::End::CertificateUntrusted:
        return DeviceAsker::certificateNotTrusted();
    case DeviceLine::End::CertificateOfUnknownSigner:
        return DeviceAsker::certificateOfAnUnknownSigner();
    case DeviceLine::End::Silent:
    case DeviceLine::End::Late:
        return DeviceAsker::tookTooLong();
    case DeviceLine::End::SignedOut:
        return keepsRefusingTheSession();
    case DeviceLine::End::NotLetIn:
        return {"bad_response", "The JetKVM refused"};
    case DeviceLine::End::Fell:
    case DeviceLine::End::TakesNothing:
        return DeviceAsker::lineFell();
    case DeviceLine::End::Misbehaved:
        return {"dropped", "The device sent what this program does not accept"};
    }
    return DeviceAsker::lineFell();
}

// The pass a sign-in handed over: its cookie, where it is one (the web's TOKEN, kvmd::isToken).
QByteArray passFrom(const QList<QNetworkCookie>& cookies)
{
    // (Two of that name are none, as a kvmd's driver has it: which of them is its session cannot be told.
    // The web's own cookie jar would take the last of two with one path; refusing both is the safer.)
    QByteArray pass;
    for (const QNetworkCookie& one : cookies) {
        if (one.name() == "authToken") {
            if (!pass.isEmpty() || !kvmd::isToken(one.value())) {
                return {};
            }
            pass = one.value();
        }
    }
    return pass;
}

// A 429's Retry-After, as a wait: a number of seconds, or none. As much as `atMostMs` where it says more,
// "inf" among it.
qint64 comeBackAfterMs(const QByteArray& said, qint64 atMostMs)
{
    bool number = false;
    const double seconds = QString::fromLatin1(said).trimmed().toDouble(&number);
    if (!number || std::isnan(seconds) || seconds <= 0) {
        return 0;
    }
    if (std::isinf(seconds) || seconds * 1000 >= static_cast<double>(atMostMs)) {
        return atMostMs;
    }
    return static_cast<qint64>(seconds * 1000);
}

// Its own number, as a mark to know it by at any address: only ever compared with another, and so kept as
// a digest (the number itself is nobody's business). None where what it gave is no number fit to be one.
QString numberFrom(const QByteArray& answer)
{
    const QJsonValue number = QJsonDocument::fromJson(answer).object().value("deviceId"_L1);
    if (!number.isString()) {
        return {};
    }
    const QString text = number.toString();
    // (Counted in signs, as the web counts them: one beyond the first 65536 is one, not two.)
    const QList<uint> signs = text.toUcs4();
    if (signs.size() < 8 || signs.size() > 64 || QSet<uint>(signs.cbegin(), signs.cend()).size() < 4) {
        return {};
    }
    return u"jetkvm:"_s +
           QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// What GET /device says of which JetKVM it is (the web's _own_number): its mark, or empty where it gives none (none
// fit to be one, or an older one that has no /device: 404); why not where it could not be told. (401, a session it does
// not take, is the asker's to see first.)
base::Result<QString> whichItSays(const base::Result<DeviceAsker::Reply>& reply)
{
    if (!reply) {
        return reply.error();
    }
    const int status = reply.value().status;
    if (status == 404) {
        return QString();
    }
    QJsonParseError error{};
    const QJsonDocument said = QJsonDocument::fromJson(reply.value().body, &error);
    if (status != 200 || error.error != QJsonParseError::NoError || !said.isObject()) {
        return notToldWhichItIs(status);
    }
    return numberFrom(reply.value().body);
}

} // namespace

base::Error JetKvmDevice::anotherJetKvm()
{
    return {"another_machine",
            "Another JetKVM answers at this address: it gives another number for itself than the JetKVM this card was "
            "added for. Nothing is done to it: no key, no power button, no connection to its screen. If it is this "
            "card's JetKVM now, change the card and type its password (one without a password: remove the card and add "
            "it again)."};
}

base::Error JetKvmDevice::noJetKvmNumber()
{
    return {"another_machine",
            "The JetKVM at this address does not say which it is, and the JetKVM this card was added for did. Nothing "
            "is done to it: no key, no power button, no connection to its screen. If it is this card's JetKVM now, "
            "change the card and type its password (one without a password: remove the card and add it again)."};
}

// One asking of what it is, from its signalling line to its number.
struct JetKvmDevice::Identifying {
    std::function<void(base::Result<Identity>)> done;
    bool lineOver = false; // what its signalling line was for is over
    QString version;
    std::optional<QString> number; // as it gave it on the road to its line
};

// One asking for its signalling line, until it is let in or it is said why not.
struct JetKvmDevice::Road {
    std::function<void(base::Result<DeviceLine*>)> done;
    bool signedInAgain = false; // its signalling line refused a session once, and it was signed in to again
    // Identify's: the number it gives is being taken (an addition, an edit with the password typed), and one that could
    // not be read is no number to take (the web's `taking`). And what it gave, as asked on this road; none: not read.
    bool taking = false;
    std::optional<QString> seen;
    QMetaObject::Connection in;
    QMetaObject::Connection end;
};

// A sign-in that is out, on its own line, which can outlive this driver (as a kvmd's): what is needed to tell the
// pacing what it came to, by this driver or, where it was let go of meanwhile, without it.
struct JetKvmDevice::SignInOut {
    QPointer<JetKvmDevice> driver; // none: let go of while this was out
    QPointer<DeviceAsker> line;
    std::shared_ptr<SignInPacing> pacing; // none: the program's
    SignInPacing::Mark mark;
    SignInPacing::Steps steps;
    SignInPacing::Sent sent;
    qint64 laterAtMostMs = 0; // the longest "come back then" taken (Pace)
};

// What a sign-in came to, as the pacing was told it (cameOf).
struct JetKvmDevice::SignInCame {
    enum class Kind {
        NotSent,    // nothing reached the JetKVM (put back)
        Neither,    // nothing answered it, or what answered said neither yes nor no
        Refused,    // 401
        Later,      // 429: it shuts this address out for a while
        OpenAccess, // 400: it runs without a password
        In,
    };
    Kind kind = Kind::NotSent;
    bool heard = false; // the JetKVM answered, whatever it said
    base::Error why;
    QByteArray pass;
};

JetKvmDevice::JetKvmDevice(Where where, QString password, Pace pace, QObject* parent)
    : JetKvmDevice(std::move(where), std::move(password), std::move(pace), nullptr, parent)
{}

JetKvmDevice::JetKvmDevice(Where where, QString password, Pace pace, std::shared_ptr<JetKvmKept> kept, QObject* parent)
    : OwnDevice(parent), m_where(std::move(where)), m_password(SecretText::takenFrom(password)),
      m_pace(std::move(pace)),
      m_asker(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
              DeviceAsker::Times{m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs}, "authToken"_ba),
      m_machine(m_where.machine)
{
    m_clock.start();
    // The JetKVM showed the certificate it is known by: said as every driver says it (own_device.h), once.
    connect(&m_asker, &DeviceAsker::showedItsCertificate, this, &JetKvmDevice::showed);
    m_viewer = std::make_unique<JetKvmViewer>(*this, std::move(kept), m_pace.viewer);
}

JetKvmDevice::~JetKvmDevice()
{
    // (As a kvmd's driver: a sign-in that is out goes on without this, its line the program's from now on, and what it
    // comes to is told to the pacing there, which ends its hold.)
    if (m_signInOut && m_signInOut->line != nullptr) {
        m_signInOut->line->setParent(QCoreApplication::instance());
    }
}

void JetKvmDevice::showed(const QByteArray& der)
{
    if (!std::exchange(m_toldItsCertificate, true)) {
        emit showedItsCertificate(der);
    }
}

void JetKvmDevice::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

void JetKvmDevice::tryAfresh(const Where& where, const QString& password, const Pace& pace)
{
    (pace.pacing ? *pace.pacing : SignInPacing::program()).forget(where.address, QString(), password);
}

bool JetKvmDevice::mayHaveBeenDone(const base::Error& error)
{
    // The question went out, and what came of it is not known: a JetKVM that took it and did not answer in
    // time. Not where its line fell first: what goes to a JetKVM goes on a line that is there, and what
    // found it gone was not sent (the web's jetkvm.py; M6 design, section 3.1). ("unreachable" is before
    // anything went out, and a certificate that is not the JetKVM's is before that.)
    return error.code == "timeout";
}

// --- signing in -----------------------------------------------------------------------------------------

SignInPacing& JetKvmDevice::pacing() const
{
    return m_pace.pacing ? *m_pace.pacing : SignInPacing::program();
}

SignInPacing::Mark JetKvmDevice::offered() const
{
    // (A JetKVM has no user name: none, the same wherever its password is typed: tryAfresh.)
    return SignInPacing::what(u"login", m_where.address, QString(), m_password.view());
}

SignInPacing::Steps JetKvmDevice::steps() const
{
    // (A sign-in is out at most its line's whole: to be let in, its own longer wait for an answer, the rest of it.)
    return SignInPacing::Steps{m_pace.refusedHoldMs, m_pace.signInRetryMs,
                               SignInPacing::outFor(m_pace.connectWithinMs, m_pace.quietMs * 5 / 2, m_pace.transferMs)};
}

std::optional<base::Error> JetKvmDevice::heldSignIn() const
{
    const SignInPacing::Mark mark = offered();
    if (pacing().waitLeftMs(mark) <= 0) {
        return std::nullopt;
    }
    // In its own words while the hold it noted itself is the one that stands: a JetKVM not set up, one that
    // said when to come back, one that refuses the session it gave (as the web keeps its `_login_error`
    // until its time); otherwise as whichever driver of it noted it (as a kvmd's).
    if (m_signInError && m_heldAs && pacing().standing(mark) == m_heldAs) {
        return m_signInError;
    }
    if (pacing().times(mark) > 0) {
        return passwordRefused();
    }
    return notSigningIn("a sign-in to it is under way, or the last one came to nothing");
}

void JetKvmDevice::withToken(std::function<void(base::Result<QByteArray>)> done)
{
    if (!m_token.isEmpty() || m_openAccess) {
        later([done = std::move(done), token = m_token] { done(token); });
        return;
    }
    if (m_password.isEmpty()) {
        // Asked as one that needs none, as a JetKVM is out of its box: nothing is sent to sign in.
        later([done = std::move(done)] { done(QByteArray()); });
        return;
    }
    // Whoever asks while somebody is being signed in waits for that one sign-in.
    if (m_signingIn) {
        m_waitingForToken.append(std::move(done));
        return;
    }
    // What any driver of this JetKVM noted: a password that was refused is not offered again before its time.
    if (std::optional<base::Error> held = heldSignIn()) {
        if (mayWaitForTheOther()) {
            // Another driver's sign-in with it is out: what it comes to is waited for (as a kvmd's driver does).
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

bool JetKvmDevice::mayWaitForTheOther() const
{
    // (As a kvmd's driver: again where, after a yes, another that waited went first.)
    return pacing().onlyOut(offered());
}

void JetKvmDevice::afterTheOtherSignIn()
{
    // (As a kvmd's driver: then looked at again, as its line's end tells.)
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

void JetKvmDevice::lookAtItsHttpsBeforeEverySignIn(TwinCheck check)
{
    m_twinCheck = std::move(check);
}

std::optional<QUrl> JetKvmDevice::itsOwnHttps() const
{
    // Spoken to the plain way, and its other way HTTPS at the same host (as LocalWall::ontoHttps has it).
    if (m_where.address.scheme() != "http"_L1 || !m_pace.otherWay) {
        return std::nullopt;
    }
    std::optional<QUrl> https = m_pace.otherWay(m_where.address);
    if (!https || https->scheme() != "https"_L1 || https->host() != m_where.address.host()) {
        return std::nullopt;
    }
    return https;
}

void JetKvmDevice::signIn()
{
    m_signingIn = true;
    // A plain JetKVM's HTTPS is looked at on this very sign-in, before its password goes in the clear: what shows the
    // certificate another KVM of the wall is known by may be that one, answering at this address too (an address
    // handed out anew). Whatever its HTTPS answers, what it showed is what counts (from the reading of 2g/2h, M1).
    const std::optional<QUrl> https = m_twinCheck ? itsOwnHttps() : std::nullopt;
    if (!https) {
        signInNow();
        return;
    }
    if (!m_wayProbe) {
        m_wayProbe = std::make_unique<KvmProbe>(
            KvmProbe::Pace::beforeASignInWith(m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs));
    }
    m_wayProbe->askForAJetKvm(*https, [this, here = QPointer<JetKvmDevice>(this), from = m_where.address,
                                       at = *https](const KvmProbe::Found& found) {
        if (here == nullptr) {
            return;
        }
        if (const std::optional<base::Error> twin = m_twinCheck ? m_twinCheck(from, found) : std::nullopt) {
            // Nothing goes; the wall has held the KVM back (the check is its judgement of what was seen), and is told
            // what was shown.
            if (!found.shown.isEmpty()) {
                emit httpsShowed(from, at, found.shown);
            }
            signedIn(*twin);
            return;
        }
        signInNow();
    });
}

void JetKvmDevice::signInNow()
{
    Ask login;
    login.method = "POST"_ba;
    login.path = u"/auth/login-local"_s;
    // (Its body is made as it goes, in sendSignIn: nothing of the password waits in the lambdas below.)
    login.bodyIs = "application/json"_ba;
    login.largest = largestReplyBytes;
    // (It may have gone out, and been counted by the JetKVM, before Qt says it went: never "not there" on a guess.)
    login.doesSomething = true;
    login.headers.append("Retry-After"_ba);
    // Waited for longer than any other answer, as a kvmd's sign-in is (the web's SIGN_IN_PATIENCE).
    login.quietMs = m_pace.quietMs * 5 / 2;
    // A password goes only on a line shown to be up (DeviceAsker::whenUp): where nothing of the JetKVM's was heard
    // just now, what anybody may ask (whether it was set up) is asked first, under the guess that tells a JetKVM
    // that is not there. Nothing is counted or sent where that is not answered. Both go on a line made for this
    // sign-in, which outlives this driver while the sign-in is out (as a kvmd's).
    Ask up;
    up.path = u"/device/status"_s;
    up.largest = largestReplyBytes;
    auto* line =
        new DeviceAsker(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
                        DeviceAsker::Times{m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs}, "authToken"_ba,
                        this); // NOLINT(cppcoreguidelines-owning-memory): this driver's, or the program's
    connect(line, &DeviceAsker::showedItsCertificate, this, &JetKvmDevice::showed);
    const auto then = [this, line, login](const std::optional<base::Error>& down) {
        if (!down) {
            m_signInLineHeardAt = now();
        }
        if (down) {
            line->deleteLater();
            signedIn(failed(*down));
            return;
        }
        sendSignIn(line, login);
    };
    if (m_where.address.scheme() == "http"_L1) {
        // Over plain HTTP the password goes in the clear to whatever answers. A JetKVM without a password says which it
        // is to anybody (one with a password to nobody but whoever is signed in): so what is asked first on this line,
        // whatever was heard just now, is its number, with no session; where it says one, it is judged before anything
        // goes (another one is sent nothing), and used as one that lets anybody in, with no session at all. Where it
        // says none so (401: one with a password; an older one with no /device; none fit to be a number), the line is
        // shown to be up, and the sign-in goes on it as before, its number asked under the session after it (the web's
        // _login and _number_to_anybody; from the merged review, 4).
        Ask device;
        device.path = u"/device"_s;
        device.largest = largestReplyBytes;
        line->send(device, {}, [this, line, then](const base::Result<Reply>& reply) {
            if (!reply) {
                then(reply.error());
                return;
            }
            const base::Result<QString> seen =
                reply.value().status == 200 ? whichItSays(reply) : base::Result<QString>(QString());
            if (!seen || seen.value().isEmpty()) {
                then(std::nullopt);
                return;
            }
            m_signInLineHeardAt = now();
            line->deleteLater();
            if (const std::optional<base::Error> other = judge(seen.value())) {
                signedIn(*other);
                return;
            }
            m_openAccess = true;
            m_signInError.reset();
            m_heldAs.reset();
            signedIn(QByteArray());
        });
        return;
    }
    // (Heard just now, on this driver's own line or a sign-in's, the JetKVM is there: as before, nothing is asked
    // first.)
    if (heardJustNow()) {
        later([then] { then(std::nullopt); });
        return;
    }
    line->whenUp(up, then);
}

void JetKvmDevice::sendSignIn(DeviceAsker* line, const Ask& login)
{
    // Looked at again just before it is counted: the question asked first is a round trip, and another driver of this
    // JetKVM (the wall's, a box's) may have sent the same password meanwhile, and heard a no, or have it out still.
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
    out->laterAtMostMs = m_pace.laterAtMostMs;
    out->sent = pacing().sent(out->mark, out->steps);
    m_signInOut = out;
    // Made once, of the password as it is held, as it goes: nothing of it in a JSON document made on the way (F5), and
    // no copy of it held but by what goes, which writes it over once its answer came (DeviceAsker; from the merged
    // review, 3). This one is written over here where nothing went.
    Ask sent = login;
    sent.body = secretJson({{"password", m_password.view()}});
    const WipedOnLeaving overTheBody(sent.body);
    line->send(sent, {}, [out](const base::Result<Reply>& reply) {
        if (out->line != nullptr) {
            out->line->deleteLater();
        }
        const SignInCame came = cameOf(*out, reply);
        if (JetKvmDevice* driver = out->driver) {
            if (came.heard) {
                driver->m_signInLineHeardAt = driver->now();
            }
            driver->m_signInOut.reset();
            driver->signInCame(came);
        }
    });
}

bool JetKvmDevice::heardJustNow() const
{
    // (As a kvmd's driver: on its own line, or on a sign-in's, which is another.)
    return m_asker.heardWithin(m_pace.connectWithinMs) ||
           (m_signInLineHeardAt >= 0 && now() - m_signInLineHeardAt <= m_pace.connectWithinMs);
}

JetKvmDevice::SignInCame JetKvmDevice::cameOf(const SignInOut& out, const base::Result<Reply>& reply)
{
    // Told to the pacing, which outlives drivers: by the driver, or where it was let go of meanwhile, by its line
    // alone.
    SignInPacing& pacing = out.pacing ? *out.pacing : SignInPacing::program();
    SignInCame came;
    if (!reply) {
        // As a kvmd's: where it went out and nothing answered it, held back as one that came to neither yes nor no;
        // where nothing reached the JetKVM, nothing is kept of it.
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
    if (status == 400) {
        // It runs without a password, and refuses a sign-in for that: nothing is needed.
        pacing.accepted(out.mark);
        came.kind = SignInCame::Kind::OpenAccess;
        return came;
    }
    if (status == 401) {
        // Noted before anything else is asked of it: a driver let go of meanwhile has the no noted all the same.
        pacing.saidNo(out.mark, out.steps);
        came.kind = SignInCame::Kind::Refused;
        return came;
    }
    if (status == 429) {
        // It shuts this address out after five wrong passwords, and says when to come back: no word on the password.
        // Then, and not sooner than the first wait after a no.
        const qint64 told = comeBackAfterMs(reply.value().headers.value("retry-after"), out.laterAtMostMs);
        pacing.later(out.mark, std::max<qint64>(told, out.steps.refusedMs), out.sent);
        came.kind = SignInCame::Kind::Later;
        return came;
    }
    came.pass = status == 200 ? passFrom(reply.value().cookies) : QByteArray();
    if (came.pass.isEmpty()) {
        // Whatever is at that address did not sign this program in, and not because of the password.
        pacing.unanswered(out.mark, out.steps, out.sent);
        came.kind = SignInCame::Kind::Neither;
        came.why = base::Error{"bad_response", u"Login failed (HTTP %1)"_s.arg(status).toStdString()};
        return came;
    }
    pacing.accepted(out.mark);
    came.kind = SignInCame::Kind::In;
    return came;
}

void JetKvmDevice::signInCame(const SignInCame& came)
{
    if (came.heard) {
        answered();
    }
    const SignInPacing::Mark mark = offered();
    switch (came.kind) {
    case SignInCame::Kind::NotSent:
        signedIn(failed(came.why));
        return;
    case SignInCame::Kind::Neither:
        // (The web's `_held_back`, as a kvmd's driver does it.)
        m_signInError = pacing().times(mark) > 0 ? passwordRefused() : notSigningIn(came.why.message);
        m_heldAs = pacing().standing(mark);
        signedIn(*m_signInError);
        return;
    case SignInCame::Kind::OpenAccess:
        m_openAccess = true;
        m_signInError.reset();
        m_heldAs.reset();
        signedIn(QByteArray());
        return;
    case SignInCame::Kind::Refused:
        m_signInError = passwordRefused();
        m_heldAs = pacing().standing(mark);
        // Then whether it was set up at all, which it says to anybody. What came of this sign-in is what its waiters
        // are told, and its words are kept only while the hold it noted stands: another asking may note a hold of its
        // own meanwhile (its session refused once more), whose words are its own (carried in from increment 1's
        // readings, as rejected()'s callback).
        freshFromItsBox([this, heldAs = m_heldAs](bool fresh) {
            const base::Error said = fresh ? notSetUp() : passwordRefused();
            if (heldAs && pacing().standing(offered()) == heldAs) {
                m_signInError = said;
            }
            signedIn(said);
        });
        return;
    case SignInCame::Kind::Later:
        m_signInError = refusingSignIns();
        m_heldAs = pacing().standing(mark);
        signedIn(*m_signInError);
        return;
    case SignInCame::Kind::In:
        break;
    }
    m_signInError.reset();
    m_heldAs.reset();
    m_token = came.pass;
    signedIn(came.pass);
}

void JetKvmDevice::signedIn(const base::Result<QByteArray>& token)
{
    m_signingIn = false;
    for (const auto& waiting : std::exchange(m_waitingForToken, {})) {
        waiting(token);
    }
}

void JetKvmDevice::freshFromItsBox(std::function<void(bool fresh)> then)
{
    Ask status;
    status.path = u"/device/status"_s;
    status.largest = largestStatusBytes;
    m_asker.send(status, {}, [then = std::move(then)](const base::Result<Reply>& reply) {
        const bool fresh =
            reply && reply.value().status == 200 &&
            QJsonDocument::fromJson(reply.value().body).object().value("isSetup"_L1) == QJsonValue(false);
        then(fresh);
    });
}

void JetKvmDevice::rejected(std::function<void(base::Error why)> then)
{
    freshFromItsBox([this, then = std::move(then)](bool fresh) {
        if (fresh) {
            then(notSetUp());
        } else if (!m_password.isEmpty()) {
            then(keepsRefusingTheSession());
        } else {
            then(m_machine.isEmpty() ? noPasswordSaved() : noPasswordSavedForOneKnown());
        }
    });
}

// --- what it is -----------------------------------------------------------------------------------------

void JetKvmDevice::identify(std::function<void(base::Result<Identity>)> done)
{
    auto asking = std::make_shared<Identifying>();
    asking->done = std::move(done);
    // A person is waiting (adding it): tried now, whatever hold stands (the web's identify).
    tryNow();
    // (Its number is asked on the road to its line, and judged there: an addition, or an edit with the password typed,
    // takes it, and never one it could not read.)
    auto road = std::make_shared<Road>();
    road->taking = true;
    road->done = [this, asking, ofIt = std::weak_ptr<Road>(road)](const base::Result<DeviceLine*>& opened) {
        if (!opened) {
            finish(asking, opened.error());
            return;
        }
        // (The road is there while it hands over what it came to.)
        if (const std::shared_ptr<Road> came = ofIt.lock()) {
            asking->number = came->seen;
        }
        // In: it can be used, which a search that only wanted to know says (the web's identify).
        answered();
        // (Kept as this driver's child: it is over within versionWithinMs.)
        DeviceLine* line = opened.value();
        const QPointer<DeviceLine> held(line);
        const auto over = [asking, held] {
            asking->lineOver = true;
            if (held != nullptr) {
                held->close();
                held->deleteLater();
            }
        };
        // Its version is the first it says (another word may come before it, and is let go).
        QTimer::singleShot(m_pace.versionWithinMs, line, [this, asking, over] {
            if (!asking->lineOver) {
                over();
                said(asking);
            }
        });
        connect(line, &DeviceLine::textCame, this, [this, asking, over](const QByteArray& text) {
            const QJsonObject heard = QJsonDocument::fromJson(text).object();
            if (asking->lineOver || heard.value("type"_L1).toString() != "device-metadata"_L1) {
                return;
            }
            asking->version = kvmd::plain(heard.value("data"_L1).toObject().value("deviceVersion"_L1), 40);
            over();
            said(asking);
        });
        connect(line, &DeviceLine::ended, this, [this, asking, line](DeviceLine::End /*why*/) {
            line->deleteLater();
            if (std::exchange(asking->lineOver, true)) {
                return;
            }
            // In, and gone before it said its version: what it is all the same, without that.
            said(asking);
        });
    };
    openRoad(road);
}

void JetKvmDevice::openSignalling(std::function<void(base::Result<DeviceLine*>)> done)
{
    auto road = std::make_shared<Road>();
    road->done = std::move(done);
    openRoad(road);
}

void JetKvmDevice::openRoad(const std::shared_ptr<Road>& road)
{
    // Found away just now: nothing goes out until its hold is over, whoever asks (the web's _check_backoff).
    if (std::optional<base::Error> held = heldBack()) {
        arrive(road, *held);
        return;
    }
    askForTheLine(road);
}

void JetKvmDevice::arrive(const std::shared_ptr<Road>& road, const base::Error& why)
{
    // A turn of the loop later: never from inside the signal of a line that is ending.
    later([road, handed = base::Result<DeviceLine*>(why)] {
        if (const auto done = std::exchange(road->done, nullptr)) {
            done(handed);
        }
    });
}

void JetKvmDevice::askForTheLine(const std::shared_ptr<Road>& road)
{
    withToken([this, road](const base::Result<QByteArray>& token) {
        if (!token) {
            arrive(road, token.error());
            return;
        }
        askWhichItIs(road, token.value());
    });
}

void JetKvmDevice::askWhichItIs(const std::shared_ptr<Road>& road, const QByteArray& token)
{
    // Which JetKVM answers, asked under the session the line is then asked with, before it is (the web's _signalling
    // and _same_jetkvm): another one is asked for no line, and so is offered no connection, and sent no key, no text
    // and no press. Its sign-in is the one thing that goes before it is known (it says its number only to whoever is
    // signed in; over HTTPS the certificate it is known by is checked first, and over plain HTTP its HTTPS was looked
    // at).
    Ask device;
    device.path = u"/device"_s;
    device.largest = largestReplyBytes;
    m_asker.send(device, token, [this, road, token](const base::Result<Reply>& reply) {
        if (reply && reply.value().status == 401) {
            // A session it does not take: as on its signalling line (somebody signed in to it since).
            sessionRefused(road, token);
            return;
        }
        const base::Result<QString> seen = whichItSays(reply);
        if (!seen) {
            const base::Error why = meansAway(seen.error().code) ? failed(seen.error()) : seen.error();
            if (!m_machine.isEmpty()) {
                // One is known: a question that came to nothing is no pass.
                arrive(road, why);
                return;
            }
            if (road->taking) {
                // Which JetKVM that is could not be told, and nothing is taken (taken as "none", the card kept no
                // number, and took the next JetKVM to answer there at first use).
                arrive(road,
                       base::Error{why.code, "Which JetKVM answers at this address could not be told: " + why.message});
                return;
            }
            // None is known: there is nothing to tell it apart from, and it is used as before.
            road->seen.reset();
            openTheLine(road, token);
            return;
        }
        road->seen = seen.value();
        if (std::optional<base::Error> other = judge(seen.value())) {
            arrive(road, *other);
            return;
        }
        openTheLine(road, token);
    });
}

std::optional<base::Error> JetKvmDevice::judge(const QString& seen)
{
    if (seen.isEmpty()) {
        // It gives none: another, where one is known.
        if (!m_machine.isEmpty()) {
            return noteAnother(noJetKvmNumber());
        }
        return std::nullopt;
    }
    if (m_machine.isEmpty()) {
        // None known (a card from a file without it, one added before it was kept): the first one seen is taken from
        // now on, as a certificate is at first use, and said (a turn of the loop later: whoever is told may let go of
        // this driver there and then).
        m_machine = seen;
        later([this, from = m_where.address, seen] { emit knownAs(from, QString(), seen); });
        return std::nullopt;
    }
    if (seen != m_machine) {
        return noteAnother(anotherJetKvm());
    }
    // The one it is known by answers at its address (again).
    m_other.reset();
    return std::nullopt;
}

base::Error JetKvmDevice::noteAnother(const base::Error& why)
{
    m_other = why;
    return why;
}

void JetKvmDevice::openTheLine(const std::shared_ptr<Road>& road, const QByteArray& token)
{
    DeviceLine::Where where;
    where.address = m_where.address;
    where.asking = "/webrtc/signaling/client"_ba;
    if (token.isEmpty()) {
        where.withoutCookie = true;
    } else {
        where.cookie = "authToken="_ba + token;
    }
    where.knownByCertificate = m_where.knownByCertificate;
    where.certificate = m_where.certificate;
    DeviceLine::Limits limits;
    limits.letInWithinMs = m_pace.lineWithinMs;
    auto* line = new DeviceLine(std::move(where), limits, this);
    line->receiveText();
    road->in = connect(line, &DeviceLine::letIn, this, [this, road, line] {
        disconnect(road->in);
        disconnect(road->end);
        // Heard from, and no more: that it can be used again is said by whoever uses it (what it is, a sign-in,
        // the first thing it answers over a connection; not a plain look, which only hears it). Said here, a
        // JetKVM that lets a connection be arranged and ends each one at once was never held longer than the first
        // time (the web's _signalling).
        m_lastOkAt = QDateTime::currentSecsSinceEpoch();
        // Handed over here and now: what the JetKVM says next is read once whoever takes it is connected.
        if (const auto done = std::exchange(road->done, nullptr)) {
            done(line);
        }
    });
    road->end = connect(line, &DeviceLine::ended, this, [this, road, line, used = token](DeviceLine::End why) {
        disconnect(road->in);
        disconnect(road->end);
        line->deleteLater();
        if (why != DeviceLine::End::SignedOut) {
            const base::Error error = whyTheLineEnded(why);
            arrive(road, meansAway(error.code) ? failed(error) : error);
            return;
        }
        sessionRefused(road, used);
    });
    line->open();
}

void JetKvmDevice::sessionRefused(const std::shared_ptr<Road>& road, const QByteArray& used)
{
    // Somebody signed in to it since, which ended this program's session (it has one for everybody): signed in to once
    // more. Refused again, or with no password to sign in with, it says why, and the session is not asked for again
    // for a while.
    if (!road->signedInAgain && !m_password.isEmpty()) {
        road->signedInAgain = true;
        if (m_token == used) {
            m_token.clear();
        }
        m_openAccess = false;
        askForTheLine(road);
        return;
    }
    // (The session refused is let go of at once: a question asked meanwhile does not carry it.)
    if (m_token == used) {
        m_token.clear();
    }
    // Refused once more after a sign-in of its own: held as a sign-in that came to neither yes nor no, where every
    // driver of this JetKVM sees it (SignInPacing), and noted before anything else is asked of it, as a no is: a driver
    // let go of meanwhile leaves it, and one made anew, or another asking of this one, does not sign in again at once
    // and sign out the JetKVM's own page.
    const bool holds = road->signedInAgain && m_token.isEmpty();
    // (The hold this asking noted, as it was noted: another asking of this driver may note another while this one asks
    // whether the JetKVM was set up.)
    std::optional<SignInPacing::Kept> heldAs;
    if (holds) {
        const SignInPacing::Mark mark = offered();
        pacing().unanswered(mark, steps());
        m_signInError = keepsRefusingTheSession();
        m_heldAs = pacing().standing(mark);
        heldAs = m_heldAs;
    }
    rejected([this, road, heldAs](const base::Error& refused) {
        // Its words, once it has said whether it was set up: while the hold it noted is still the one that stands (a no
        // heard meanwhile, by this asking or another, has words of its own).
        if (heldAs && pacing().standing(offered()) == heldAs) {
            m_signInError = refused;
        }
        arrive(road, refused);
    });
}

void JetKvmDevice::said(const std::shared_ptr<Identifying>& asking)
{
    // (Its number as it gave it on the road to its line, judged there.)
    if (!asking->version.isEmpty()) {
        m_version = asking->version; // (one it did not say this time is no other version)
    }
    finish(asking, Identity{u"JetKVM"_s, asking->version, asking->number});
}

void JetKvmDevice::finish(const std::shared_ptr<Identifying>& asking, const base::Result<Identity>& result)
{
    // A turn of the loop later: never from inside the signal of a line that is ending, whose driver whoever
    // is told may let go of.
    later([asking, handed = result] {
        if (const auto done = std::exchange(asking->done, nullptr)) {
            done(handed);
        }
    });
}

// --- how it is ------------------------------------------------------------------------------------------

void JetKvmDevice::Away::note(const base::Error& why, qint64 at, const Pace& pace)
{
    if (!error || at >= until) {
        // Questions that were out together fail together: one failed attempt, not several.
        ++failures;
        const qint64 hold = qint64{pace.holdMs} << std::min(failures - 1, 10);
        until = at + std::min<qint64>(pace.longestHoldMs, hold);
    }
    error = why;
}

std::optional<base::Error> JetKvmDevice::Away::held(qint64 at) const
{
    if (error && at < until) {
        return error;
    }
    return std::nullopt;
}

qint64 JetKvmDevice::Away::heldForMs(qint64 at) const
{
    return error && at < until ? until - at : 0;
}

void JetKvmDevice::Away::clear()
{
    failures = 0;
    error.reset();
}

void JetKvmDevice::answered()
{
    // (Its look's count too: a JetKVM that can be used answers a look. Left standing, one look lost after a
    // connection came about was held as long as the looks before it had grown the hold, and said "offline" all that
    // while: an alert for a JetKVM that is well.)
    m_road.clear();
    m_look.clear();
    m_lastOkAt = QDateTime::currentSecsSinceEpoch();
}

base::Error JetKvmDevice::failed(const base::Error& error)
{
    if (meansAway(error.code)) {
        m_road.note(error, now(), m_pace);
    }
    return error;
}

std::optional<base::Error> JetKvmDevice::heldBack() const
{
    return m_road.held(now());
}

qint64 JetKvmDevice::heldBackForMs() const
{
    return m_road.heldForMs(now());
}

void JetKvmDevice::tryNow()
{
    m_road.error.reset();
}

bool JetKvmDevice::isAway(const std::string& code)
{
    return meansAway(code);
}

void JetKvmDevice::look()
{
    if (m_looking) {
        return;
    }
    m_looking = true;
    // While its connection is open, how it is is asked over that (design 3.6): a look never connects, and
    // never counts as somebody watching. Where that connection went meanwhile (given back by this program,
    // which tells what waited "may have been done"), nothing was heard of the JetKVM: it is looked at the
    // plain way, as where none is open.
    const bool overIt = m_viewer->askStatus([this](const std::optional<Status>& heard) {
        if (!heard) {
            lookThePlainWay();
            return;
        }
        Status status = *heard;
        if (status.state == "online"_L1 || status.state == "no_signal"_L1) {
            answered();
            m_version = status.version.isEmpty() ? m_version : status.version;
        }
        status.version = m_version;
        lookOver(std::move(status));
    });
    if (!overIt) {
        lookThePlainWay();
    }
}

void JetKvmDevice::lookThePlainWay()
{
    const auto away = [this](const base::Error& why) {
        // (Gone: what its last connection showed says nothing of when it is back.)
        m_viewer->forgetWhatWasShown();
        Status status;
        // Gone, and a JetKVM answers at its other way over plain HTTP: said so (lookTheOtherWay).
        if (m_plainOnly && (meansGone(why.code) || why.code == "bad_response")) {
            const base::Error plainOnly = answersOnlyThePlainWay(*m_plainOnly, !m_password.isEmpty());
            status.state = u"error"_s;
            status.errorCode = QString::fromStdString(plainOnly.code);
            status.errorMessage = QString::fromStdString(plainOnly.message);
            return status;
        }
        // (A certificate that is not the JetKVM's is something to look at, and not a JetKVM that is away.)
        status.state = why.code == "cert_mismatch" || why.code == "cert_invalid" || why.code == tlsDamaged
                           ? u"error"_s
                           : u"offline"_s;
        status.errorCode = QString::fromStdString(why.code);
        status.errorMessage = QString::fromStdString(why.message);
        return status;
    };
    // Held by its own count alone, and found away by its own looks alone: what connects does not hold it, nor does it
    // hold what connects (the web's probe, which has no _check_backoff and calls no _failed). Held by the road's, it
    // said "offline" through every hold of a JetKVM whose HTTP answers and whose video never comes, about every 30 s
    // an "Offline" and a "Recovered" alert while a card showed it (from the second reading of c7ec58c, A).
    if (std::optional<base::Error> held = m_look.held(now())) {
        // (Still away: whether only its HTTPS is gone is asked by its own pace, not by this look's.)
        if (meansGone(held->code)) {
            lookTheOtherWay(true);
        }
        later([this, status = away(*held)] { lookOver(status); });
        return;
    }
    // What anybody may ask, with no session: a look never connects, and never counts as somebody watching.
    Ask status;
    status.path = u"/device/status"_s;
    status.largest = largestStatusBytes;
    m_asker.send(status, {}, [this, away](const base::Result<Reply>& reply) {
        if (!reply) {
            const base::Error& why = reply.error();
            if (meansAway(why.code)) {
                m_look.note(why, now(), m_pace);
            }
            if (meansGone(why.code)) {
                lookTheOtherWay(true);
            }
            lookOver(away(why));
            return;
        }
        if (reply.value().status != 200) {
            lookTheOtherWay(true);
            lookOver(away(base::Error{"bad_response",
                                      u"The JetKVM answered with HTTP %1"_s.arg(reply.value().status).toStdString()}));
            return;
        }
        // There: how long it was away before is over (its look's own count with it), and so is what was said of its
        // other way. Heard from, and no more: whatever a connection to it would come to (the web's probe), so the hold
        // that connections which never came about grew is not undone by it (the reading of 077e539..2c3f523, M).
        m_look.clear();
        m_plainOnly.reset();
        m_downSince.reset();
        m_lastOkAt = QDateTime::currentSecsSinceEpoch();
        lookTheOtherWay(false);
        Status there;
        there.model = u"JetKVM"_s;
        there.version = m_version;
        there.hasPicture = true;
        if (m_other) {
            // There, and it is another JetKVM than the one this driver is known by (or one that gives no number): said
            // until the one it is known by answers again, which only a connection's asking tells (the web's `_other`,
            // and the "error" its probe says meanwhile).
            there.state = u"error"_s;
            there.errorCode = QString::fromStdString(m_other->code);
            there.errorMessage = QString::fromStdString(m_other->message);
        } else if (m_viewer->aside()) {
            // There, and another viewer has its picture (the web's look, jetkvm.py:1653-1660).
            there.state = u"busy"_s;
            there.errorCode = u"in_use"_s;
            there.errorMessage =
                u"Someone is using this JetKVM's own console. It shows its picture to one viewer at a time."_s;
        } else if (m_viewer->notThrough()) {
            // There, and an offer of this program's came to nothing: cards make no other (JetKvmViewer), and its card
            // says why, with "Show here" (from the second reading of c7ec58c, B). It answers: online.
            there.state = u"online"_s;
            there.errorCode = QString::fromLatin1(JetKvmViewer::notThroughCode);
            there.errorMessage = QString::fromLatin1(JetKvmViewer::notThroughSentence);
        } else {
            // What the last connection showed stands until another shows otherwise (no signal), or "online".
            there.state = m_viewer->lastSignal() == std::optional<bool>(false) ? u"no_signal"_s : u"online"_s;
        }
        lookOver(std::move(there));
    });
}

void JetKvmDevice::otherWayAskedJustNow()
{
    m_wayAskedAt = now();
}

void JetKvmDevice::lookTheOtherWay(bool down)
{
    // One over HTTPS that answers is not asked about its other way: only once it is away.
    const bool secure = m_where.address.scheme() == "https"_L1;
    if ((secure && !down) || m_wayAsking || !m_pace.otherWay) {
        return;
    }
    const qint64 at = now();
    if (down) {
        // Away for less than its patience: a JetKVM that is starting up, whose HTTPS comes a little later.
        m_downSince = m_downSince.value_or(at);
        if (at - *m_downSince < m_pace.wayPatienceMs) {
            return;
        }
    }
    if (m_wayAskedAt && at - *m_wayAskedAt < (down ? m_pace.wayWhenDownMs : m_pace.wayEveryMs)) {
        return;
    }
    const std::optional<QUrl> twin = m_pace.otherWay(m_where.address);
    if (!twin) {
        return;
    }
    m_wayAskedAt = at;
    m_wayAsking = true;
    if (!m_wayProbe) {
        // (Its own, at its times: only the JetKVM's question travels on it, with nothing that says who asks.)
        m_wayProbe = std::make_unique<KvmProbe>(
            KvmProbe::Pace::beforeASignInWith(m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs));
    }
    m_wayProbe->askForAJetKvm(*twin, [this, here = QPointer<JetKvmDevice>(this), from = m_where.address,
                                      twin = *twin](const KvmProbe::Found& found) {
        if (here == nullptr) {
            return;
        }
        m_wayAsking = false;
        if (found.kind != KvmProbe::Kind::JetKvm || found.refused) {
            // What its HTTPS showed is the wall's to look at all the same, whatever it answered (a 503, an answer
            // past its time, a page that is no JetKVM's): it may be another KVM of the wall, answering otherwise just
            // then (LocalWall).
            if (twin.scheme() == "https"_L1 && !found.shown.isEmpty()) {
                emit httpsShowed(from, twin, found.shown);
            }
            return;
        }
        if (twin.scheme() != "https"_L1) {
            // Its HTTPS switched off, or so it looks from here. Never followed by itself, with a password or
            // without (as the web's): over plain HTTP nothing is checked any more, the certificate it is
            // known by would be let go of, and whoever keeps one port quiet for a while and then answers
            // on it would have the card, with nobody asked. Said on its card; a person decides.
            m_plainOnly = twin;
            return;
        }
        // Whether it is followed there is the wall's to decide (LocalWall: its own rules).
        emit offersHttps(from, twin, found);
    });
}

void JetKvmDevice::lookOver(Status status)
{
    m_status = std::move(status);
    m_statusAt = now();
    m_looking = false;
    emit looked();
}

QStringList JetKvmDevice::abilities(bool atx)
{
    QStringList can{u"screen"_s, u"keys"_s,   u"text"_s, u"jiggler"_s, u"wol"_s,
                    u"reboot"_s, u"resume"_s, u"live"_s, u"control"_s};
    if (atx) {
        can.append({u"power"_s, u"reset"_s});
    }
    return can;
}

Status JetKvmDevice::status() const
{
    Status status = m_status.value_or(Status{});
    // What it can be asked, as the web's says it (jetkvm.py `can`): its menu offers that and nothing else.
    status.capabilities = abilities(status.powerIsKnown);
    if (m_lastOkAt) {
        const qint64 ago = std::clamp<qint64>(QDateTime::currentSecsSinceEpoch() - *m_lastOkAt, 0, 2'000'000'000);
        status.lastSeenSecondsAgo = static_cast<int>(ago);
    }
    return status;
}

bool JetKvmDevice::isStale() const
{
    return !m_status || now() - m_statusAt > m_pace.freshMs;
}

void JetKvmDevice::afterLook(int withinMs, std::function<void(bool stillLooking)> then)
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
    *heard = connect(this, &JetKvmDevice::looked, this, run);
    QTimer::singleShot(withinMs, this, run);
}

// --- its picture, and what is done to it ------------------------------------------------------------------

void JetKvmDevice::askSnapshot(int /*width*/, std::function<void(base::Result<Snapshot>)> done)
{
    // Answered as it stands when asked (a card that asks connects: JetKvmViewer), a turn of the loop later.
    later([done = std::move(done), answer = m_viewer->stillForACard()] { done(answer); });
}

void JetKvmDevice::act(const Action& what, std::function<void(base::Result<Done>)> done)
{
    if (what.kind == "resume"_L1) {
        // "Show here": whoever has its own console open is disconnected from it.
        m_viewer->resume(std::move(done));
        return;
    }
    if (what.kind == "ctrl_alt_del"_L1 || what.kind == "keys"_L1 || what.kind == "text"_L1) {
        // Keys pressed together are what the web's key_report takes, refused before anything is asked of the JetKVM; a
        // text is held to its length here, and made into keys once the JetKVM has said its computer's layout
        // (jetkvm_layouts.h): nothing is typed where one character cannot be.
        if (what.kind == "text"_L1) {
            const QString text = what.text + (what.enter ? u"\n"_s : QString());
            if (text.isEmpty() || QStringView(text).toUcs4().size() > typed::mostCharacters) {
                later([done = std::move(done)] {
                    done(base::Error{"invalid",
                                     "Text must be 1 to " + std::to_string(typed::mostCharacters) + " characters"});
                });
                return;
            }
            typeOn({}, text, std::move(done));
            return;
        }
        const base::Result<typed::Report> keys =
            typed::combination(what.kind == "keys"_L1 ? what.keys : u"ControlLeft,AltLeft,Delete"_s, u"A JetKVM"_s);
        if (!keys) {
            later([done = std::move(done), why = keys.error()] { done(why); });
            return;
        }
        typeOn({keys.value()}, std::nullopt, std::move(done));
        return;
    }
    if (what.kind == "atx_power"_L1 || what.kind == "atx_power_long"_L1 || what.kind == "atx_reset"_L1 ||
        what.kind == "reboot"_L1 || what.kind == "wol"_L1 || what.kind == "jiggler_on"_L1 ||
        what.kind == "jiggler_off"_L1) {
        press(what, std::move(done));
        return;
    }
    later([done = std::move(done)] { done(base::Error{"invalid", "A JetKVM cannot do that"}); });
}

void JetKvmDevice::typeOn(std::vector<typed::Report> reports, std::optional<QString> text,
                          std::function<void(base::Result<Done>)> done)
{
    // One action that types at a time (two would interleave): the next begins when the one before is over.
    m_typingWaits.append([this, reports = std::move(reports), text = std::move(text),
                          done = std::move(done)]() mutable {
        const auto over = [here = QPointer<JetKvmDevice>(this),
                           done = std::move(done)](const base::Result<Done>& what) {
            done(what);
            if (here != nullptr) {
                here->m_typingNow = false;
                here->typeNext();
            }
        };
        const char* const notDone = text ? "Nothing was typed: " : "It was not done: ";
        // A person asked for it: tried now, whatever hold stands (the web's _connected, urgent); nothing goes before
        // the connection's keys are ready, and what was owed is let go of first.
        tryNow();
        m_viewer->inputReady([here = QPointer<JetKvmDevice>(this), reports = std::move(reports), text = std::move(text),
                              over, notDone](const base::Result<QPointer<JetKvmConnection>>& on) mutable {
            if (!on || on.value() == nullptr) {
                const base::Error why = on ? base::Error{"dropped", "The connection to the JetKVM ended"} : on.error();
                if (why.code == "in_use" || why.code == "another_machine") {
                    over(why);
                    return;
                }
                // No connection was to be had, so nothing of this went out, whatever the wait came to (the web's
                // _not_sent).
                over(base::Error{"action_failed", notDone + why.message});
                return;
            }
            const jetkvm::TypingPace pace = here != nullptr ? here->m_pace.typing : jetkvm::TypingPace{};
            const QPointer<JetKvmConnection>& connection = on.value();
            // (Every key keeps the connection wanted: a long text with nobody watching is not cut for that.)
            const auto keyGoes = [here] {
                if (here != nullptr) {
                    here->m_viewer->keyGoes();
                }
            };
            if (!text) {
                jetkvm::type(connection, std::move(reports), false, pace, keyGoes, over);
                return;
            }
            // Which keys type it is known once connected: the JetKVM says which layout its computer uses.
            connection->keyboardLayout(
                [connection, text = *text, pace, keyGoes, over, notDone](const base::Result<QString>& layout) {
                    if (!layout) {
                        // Only asked, so far: nothing was typed, whatever became of the question.
                        if (layout.error().code == "in_use") {
                            over(layout.error());
                            return;
                        }
                        const std::string& said = layout.error().message;
                        const bool cut = said.size() >= 18 && said.ends_with("may have been done");
                        over(base::Error{"action_failed",
                                         notDone +
                                             std::string("the JetKVM did not say which keyboard layout its computer "
                                                         "uses") +
                                             (cut ? std::string() : " (" + said + ")")});
                        return;
                    }
                    base::Result<std::vector<typed::Report>> keys = jetkvm::textReports(text, layout.value());
                    if (!keys) {
                        over(keys.error());
                        return;
                    }
                    jetkvm::type(connection, std::move(keys).value(), true, pace, keyGoes, over);
                });
        });
    });
    typeNext();
}

void JetKvmDevice::typeNext()
{
    if (m_typingNow || m_typingWaits.isEmpty()) {
        return;
    }
    m_typingNow = true;
    m_typingWaits.takeFirst()();
}

void JetKvmDevice::askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done)
{
    // The machines saved on it for Wake-on-LAN, over its connection (the web's wol_targets): what it says that is no
    // list, or an entry whose address is none, is left out; and where it could not be asked, there are none.
    m_viewer->connected([done = std::move(done)](const base::Result<QPointer<JetKvmConnection>>& on) {
        if (!on || on.value() == nullptr) {
            done(QList<WakeTarget>{});
            return;
        }
        on.value()->call(u"getWakeOnLanDevices"_s, {}, [done](const base::Result<json::Value>& said) {
            static const QRegularExpression mac(u"\\A[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}\\z"_s);
            QList<WakeTarget> targets;
            if (said && said.value().isList()) {
                for (const json::Value& entry : said.value().list) {
                    const json::Value* address = entry.isObject() ? entry.find(u"macAddress") : nullptr;
                    if (address == nullptr || !address->isText() || !mac.match(address->text).hasMatch()) {
                        continue;
                    }
                    const json::Value* name = entry.find(u"name");
                    targets.append(WakeTarget{
                        name != nullptr && name->isText() ? kvmd::plain(QJsonValue(name->text), 100) : QString(),
                        address->text});
                }
            }
            done(targets);
        });
    });
}
namespace {
const char* const inUseSentence =
    "Someone is using this JetKVM's own console. It shows its picture to one viewer at a time.";
} // namespace

void JetKvmDevice::press(const Action& what, std::function<void(base::Result<Done>)> done)
{
    // What the web's act does to a JetKVM besides typing (jetkvm.py): power and reset by its ATX extension, a restart
    // of the JetKVM itself, Wake-on-LAN, and its mouse jiggler, each one call over its connection, once that is open.
    static const QRegularExpression mac(u"\\A[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}\\z"_s);
    if (what.kind == "wol"_L1 && !mac.match(what.mac).hasMatch()) {
        later([done = std::move(done)] { done(base::Error{"invalid", "Not a valid MAC address"}); });
        return;
    }
    // A person asked for it: tried now, whatever hold stands (the web's _connected, urgent).
    tryNow();
    m_viewer->connected([what, done = std::move(done)](const base::Result<QPointer<JetKvmConnection>>& on) {
        if (!on || on.value() == nullptr) {
            const base::Error why = on ? base::Error{"dropped", "The connection to the JetKVM ended"} : on.error();
            if (why.code == "in_use" || why.code == "another_machine") {
                done(why);
                return;
            }
            // No connection was to be had, so nothing of this went out (the web's _not_sent).
            done(base::Error{"action_failed", "It was not done: " + why.message});
            return;
        }
        const QPointer<JetKvmConnection>& connection = on.value();
        if (what.kind.startsWith("atx_"_L1)) {
            pressAtx(connection, what.kind, done);
        } else if (what.kind == "reboot"_L1) {
            // It may be gone before it can answer: a restart that went out and had no answer is taken as done.
            pressOnce(connection, u"reboot"_s, QJsonObject{{u"force"_s, true}}, [done](const base::Result<Done>& said) {
                done(!said && said.error().code == "timeout" ? base::Result<Done>(Done{}) : said);
            });
        } else if (what.kind == "wol"_L1) {
            pressOnce(connection, u"sendWOLMagicPacket"_s, QJsonObject{{u"macAddress"_s, what.mac}}, done);
        } else {
            pressOnce(connection, u"setJigglerState"_s, QJsonObject{{u"enabled"_s, what.kind == "jiggler_on"_L1}},
                      done);
        }
    });
}

void JetKvmDevice::pressAtx(const QPointer<JetKvmConnection>& on, const QString& kind,
                            const std::function<void(base::Result<Done>)>& done)
{
    // Whether its ATX extension is on, asked first (24d): only asked, so far, and nothing pressed whatever became of
    // the question; cut off by its own page taking it, "in_use".
    // (Whether its own page took it meanwhile is noted as the word comes: the connection may be gone by the time the
    // question's end is told.)
    auto takenOver = std::make_shared<bool>(on->taken());
    QObject::connect(on.data(), &JetKvmConnection::takenByAnother, on.data(), [takenOver] { *takenOver = true; });
    on->call(u"getActiveExtension"_s, {}, [on, kind, done, takenOver](const base::Result<json::Value>& said) {
        if (!said) {
            if (said.error().code == "in_use" || *takenOver || (on != nullptr && on->taken())) {
                done(base::Error{"in_use", inUseSentence});
            } else if (said.error().code == "action_failed") {
                done(said.error()); // (its own refusal, in its own words)
            } else {
                done(base::Error{"action_failed",
                                 "Nothing was pressed: qKVM was not told whether the JetKVM's power extension is on"});
            }
            return;
        }
        if (!said.value().isText() || said.value().text != u"atx-power"_s) {
            done(base::Error{"action_failed", "This JetKVM has no ATX power extension switched on"});
            return;
        }
        if (on == nullptr || on->taken() || *takenOver) {
            // (Its own page took the viewer while the extension was asked about: not pressed.)
            done(base::Error{"in_use", inUseSentence});
            return;
        }
        const QString press = kind == "atx_power"_L1        ? u"power-short"_s
                              : kind == "atx_power_long"_L1 ? u"power-long"_s
                                                            : u"reset"_s;
        pressOnce(on, u"setATXPowerAction"_s, QJsonObject{{u"action"_s, press}}, done);
    });
}

void JetKvmDevice::pressOnce(const QPointer<JetKvmConnection>& on, const QString& method, const QJsonObject& params,
                             const std::function<void(base::Result<Done>)>& done)
{
    if (on == nullptr) {
        done(base::Error{"action_failed", "It was not done: the connection to the JetKVM ended"});
        return;
    }
    // The one call an action presses with. Where it did not go out ("dropped" is said of a call that was never sent,
    // and of nothing else), it was not done, and is said so; one with no answer may have been (timeout).
    on->call(method, params, [done](const base::Result<json::Value>& said) {
        if (!said) {
            done(said.error().code == "dropped"
                     ? base::Error{"action_failed", "It was not done: " + said.error().message}
                     : said.error());
            return;
        }
        done(Done{});
    });
}

void JetKvmDevice::tidy() {}

std::unique_ptr<OwnLive> JetKvmDevice::makeLive()
{
    return std::make_unique<JetKvmLive>(*m_viewer);
}

} // namespace qkvm::core
