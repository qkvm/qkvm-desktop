#include <qkvm/core/proxmox_device.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QHash>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QtLogging>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// Where every question of Proxmox's API goes, below its address.
constexpr QLatin1StringView api{"/api2/json"};
// The most of an answer a Proxmox may send (the web's MAX_REPLY), but for the list of a cluster's guests.
constexpr qsizetype largestReplyBytes = qsizetype{2} * 1024 * 1024;
// The versions the sign-ins of this program found, by the token's mark at its Proxmox: a guest that goes on without a
// sign-in of its own, once another's with the same token was taken, says the version that one was told (the web hands
// it on with the verdict). Bounded; nothing of the token is in a mark (SignInPacing::what).
constexpr qsizetype mostVersionsKept = 256;
// A question held to a moment is given at least this much more than getting a connection may take (CONNECT_MARGIN).
constexpr qint64 connectMarginMs = 500;
// The most that is read of what answers the unsigned question of its API (a version is some forty bytes).
constexpr qsizetype largestVersionBytes = qsizetype{64} * 1024;
// How Proxmox names a task (its UPID), and says that a task runs, or ended well with warnings (the web's).
const QRegularExpression& upidShape()
{
    static const QRegularExpression shape(u"\\AUPID:[A-Za-z0-9:._@!+-]{1,500}\\z"_s);
    return shape;
}
constexpr QLatin1StringView taskRuns{"running"};
constexpr QLatin1StringView taskOk{"OK"};
// Words that say nothing was sent, where what came of it would read as "it may have been done".
base::Error nothingSent(const base::Error& why)
{
    if (!ProxmoxDevice::mayHaveBeenDone(why)) {
        return why;
    }
    return {"action_failed", "Nothing was sent: the guest could not be asked how it is (" + why.message + ")"};
}
// Proxmoxes whose list of guests is kept, and guests for which it was read (the web's 256 and 4096).
constexpr qsizetype mostProxmoxesListed = 256;
constexpr qsizetype mostGuestsFollowed = 4096;

// When contact with each Proxmox was last found lost, by any guest's driver of it (outOfTouch): by its address, a stamp
// that only grows; and an epoch, which grows where room is made. A guest's sign-in stands only while its Proxmox's
// stamp and the epoch are what they were when it signed in (signedIn): so every guest of that Proxmox asks what answers
// there before its token goes again, and not only the one whose question failed (review cx3-3, L3). A certificate
// refused is stamped by its address as the driver holds it to a certificate (wayOf: its pin, or Windows' trust), and so
// stands for the guests known that same way alone: one card still on an old certificate does not sign out the others,
// which said yes to the new one (fresh review of 4.2, M1). Bounded; where room is made every sign-in stands no more,
// which asks more, never less. On the interface thread, as every driver is.
struct TouchLost {
    quint64 epoch = 1;
    quint64 last = 0;
    QHash<QString, quint64> at;
};
constexpr qsizetype mostProxmoxesTouched = 1024;

TouchLost& touchLost()
{
    static TouchLost lost;
    return lost;
}

QHash<QByteArray, QString>& versionsTaken()
{
    static QHash<QByteArray, QString> taken;
    return taken;
}

void keepVersion(const QByteArray& mark, const QString& version)
{
    QHash<QByteArray, QString>& taken = versionsTaken();
    if (taken.size() >= mostVersionsKept && !taken.contains(mark)) {
        taken.clear(); // (what is lost is a version said a sign-in later)
    }
    taken.insert(mark, version);
}

base::Error noPasswordSaved()
{
    return {"no_password", "No password saved for this device"};
}
// A sign-in that came to neither yes nor no, or one held back as such (the web's `_not_yet` for a token).
base::Error notSigningIn(const std::string& why)
{
    return {"no_sign_in", "Proxmox does not take the token just now (" + why + "). It is asked again shortly."};
}
base::Error refusesAfterTaking()
{
    return {"bad_response", "Proxmox takes the token and refuses the request after it"};
}
// A question that does something to the guest, refused 401 and not sent again: nothing of it was done.
base::Error notSentAgain(const std::string& why)
{
    return {"action_failed",
            "Nothing was done: Proxmox asked for the token again, and it was not sent a second time (" + why + ")."};
}

// A question that does something to the guest, held back where its driver had to sign in again right before it (contact
// with what answers there failed meanwhile, or another question was answered 401), and not sent: nothing of it was
// done.
base::Error notSentAfterASignIn(const std::string& why)
{
    return {"action_failed", "Nothing was done: qKVM had to sign in to Proxmox again right before the command, and it "
                             "was not sent (" +
                                 why + ")."};
}

// A command Proxmox answered "not on this node", not sent where the guest is now: nothing of it was done.
base::Error notSentWhereItWent(const std::string& why)
{
    return {"action_failed", "Nothing was done: Proxmox said the guest is not on the node the command went to, and it "
                             "was not sent where the guest is now (" +
                                 why + ")."};
}

// Failures that mean "Proxmox is not there, or is not the one", and not "it said no".
bool meansAway(const std::string& code)
{
    return code == "unreachable" || code == "timeout" || code == "dropped" || code == "cert_mismatch" ||
           code == "cert_invalid";
}

// ...and of those, the ones that are a failure of contact with what answers at its address, where nothing came back at
// all (outOfTouch): nothing there, the line fallen, or a certificate other than it is known by (or one Windows does not
// trust where it is known by none) shown. Not one that got no answer in time ("timeout", whose line was let in by what
// the Proxmox is known by, and carried the token: asking what answers there before the token goes again keeps it from
// nobody, and costs an unsigned question and a sign-in; review cx3-3, L2), the program's own bound among them.
bool contactLost(const std::string& code)
{
    return code == "unreachable" || code == "dropped" || code == "cert_mismatch" || code == "cert_invalid";
}

// Whether an answer began on a question: its status line came, whatever became of the rest of it. Read off the
// question's own reply as it is handed back, which is there until a turn of the loop after that: the asker's own
// refusal of an answer too large or packed comes out of the very moment its status line does, before anybody else hears
// of that moment (review cx3-3, L3).
bool answerBegan(const QPointer<QNetworkReply>& line)
{
    return line != nullptr && line->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 0;
}

} // namespace

// One press of a power button, from its state read to what came of it.
struct ProxmoxDevice::Pressing {
    QString kind;      // "power_on", "atx_reset", ...
    qint64 began = 0;  // the press, by this driver's clock: every wait is bounded from here
    bool sent = false; // the command went (or may have gone) out
    QString verb;      // Proxmox's: start, shutdown, stop, reset
    QString upid;      // the task Proxmox answered it with
    // What is to be so of the guest's settings, on the press's own reading of them, for its command to go (none:
    // nothing more than rule 30), and what that found amiss: said, and nothing sent.
    SettingsCheck lastCheck;
    std::optional<base::Error> amiss;
    // The sign-in that reading went under (none: no reading was made): its command goes under that one, or is decided
    // again first (Ask::decidedUnder; outside review 2, 4.1).
    std::optional<quint64> decidedUnder;
    bool handed = false; // ...one of its high availability ("hastart", "hastop")
    // The wait for a start or a stop: the task's end, once it is seen (the press until then); whether a read since
    // that end found the guest as it was; how the task stands (none: not known; "running"; "OK"; Proxmox's words).
    qint64 endedAt = 0;
    bool seenWas = false;
    std::optional<QString> task;
    std::function<void(base::Result<Done>)> done;
};

// A sign-in that is out: what is needed to tell the pacing what it came to.
struct ProxmoxDevice::SignInOut {
    std::shared_ptr<SignInPacing> pacing; // none: the program's
    SignInPacing::Mark mark;
    SignInPacing::Steps steps;
    SignInPacing::Sent sent;
};

ProxmoxDevice::ProxmoxDevice(Where where, QString secret, Pace pace, QObject* parent)
    : OwnDevice(parent), m_where(std::move(where)), m_secret(SecretText::takenFrom(secret)), m_pace(std::move(pace)),
      m_asker(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
              DeviceAsker::Times{m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs},
              DeviceAsker::Carried{DeviceAsker::Carried::In::Header, "Authorization"_ba})
{
    m_clock.start();
    m_machine = m_where.machine;
    m_bootWas = m_where.bootOrderWas;
    m_followUpAt.clear();
    connect(&m_asker, &DeviceAsker::showedItsCertificate, this, &ProxmoxDevice::showed);
}

// (What tidy follows to is the words of an answer out of memory: nothing of ours throws, DESIGN.md section 7.)
// NOLINTNEXTLINE(bugprone-exception-escape)
ProxmoxDevice::~ProxmoxDevice()
{
    // Its screen first: its console says goodbye on its own line, and nothing of that is told to this driver.
    if (m_screen != nullptr) {
        m_screen->disconnect(this);
        m_screen.reset();
    }
    // A sign-in that is out goes with this driver's line: it came to neither yes nor no, and is told so, a short hold
    // (the next driver of this guest, or another guest of its Proxmox, signs in after it, and not after the clock's
    // cap on a sign-in that never ends).
    if (m_signInOut) {
        SignInPacing& kept = m_signInOut->pacing ? *m_signInOut->pacing : SignInPacing::program();
        kept.unanswered(m_signInOut->mark, m_signInOut->steps, m_signInOut->sent);
    }
    // (Whoever waited for its sign-in is told by the wall that the driver went: own_device.h.)
    // A press under way is told what became of it: where the command went out (or may have), whether it was carried out
    // was not seen, which is "it may have been done"; where it did not, nothing was done.
    for (const std::shared_ptr<Pressing>& pressing : std::as_const(m_pressing)) {
        if (const auto done = std::exchange(pressing->done, nullptr)) {
            const base::Error why =
                pressing->sent
                    ? base::Error{"timeout", "The connection to the guest's Proxmox was let go of before it "
                                             "was seen whether this was done: it may have been done. Look "
                                             "at the guest before pressing again."}
                    : base::Error{"stopped", "It was not done: the connection to the guest's Proxmox was let "
                                             "go of before it was sent."};
            // NOLINTNEXTLINE(bugprone-exception-escape)
            QTimer::singleShot(0, QCoreApplication::instance(), [done, why] { done(why); });
        }
    }
}

void ProxmoxDevice::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

void ProxmoxDevice::tryAfresh(const Where& where, const QString& secret, const Pace& pace)
{
    (pace.pacing ? *pace.pacing : SignInPacing::program()).forget(where.address, where.user, secret);
}

bool ProxmoxDevice::mayHaveBeenDone(const base::Error& error)
{
    // It went out, and what came of it is not known (the web's server counts both so).
    return error.code == "timeout" || error.code == "dropped";
}

base::Error ProxmoxDevice::tokenRefused()
{
    return {"auth_failed", "Proxmox does not take the saved API token - check the token's ID and secret"};
}

void ProxmoxDevice::showed(const QByteArray& der)
{
    if (!std::exchange(m_toldItsCertificate, true)) {
        emit showedItsCertificate(der);
    }
}

// --- the token ---------------------------------------------------------------------------------------

SignInPacing& ProxmoxDevice::pacing() const
{
    return m_pace.pacing ? *m_pace.pacing : SignInPacing::program();
}

SignInPacing::Mark ProxmoxDevice::offered() const
{
    // (One mark for every guest of this Proxmox with this token: they share one verdict on it.)
    return SignInPacing::what(u"login", m_where.address, m_where.user, m_secret.view());
}

SignInPacing::Steps ProxmoxDevice::steps() const
{
    return SignInPacing::Steps{m_pace.refusedHoldMs, m_pace.signInRetryMs,
                               SignInPacing::outFor(m_pace.connectWithinMs, m_pace.signInQuietMs, m_pace.transferMs)};
}

QByteArray ProxmoxDevice::authorization() const
{
    // (Made once at its size, of the texts as they are held: nothing of the secret is left in a text made on the way.)
    const QByteArray prefix = "PVEAPIToken="_ba;
    const QByteArray id = m_where.user.toLatin1();
    QByteArray made;
    made.reserve(prefix.size() + id.size() + 1 + m_secret.view().size());
    made.append(prefix).append(id).append('=');
    for (const QChar sign : m_secret.view()) {
        made.append(static_cast<char>(sign.unicode())); // (a secret's shape is checked first: hex digits and dashes)
    }
    return made;
}

SecretBytes ProxmoxDevice::consoleAuthorization() const
{
    const QByteArray prefix = "PVEAPIToken="_ba;
    const QByteArray id = m_where.user.toLatin1();
    SecretBytes made(prefix.size() + id.size() + 1 + m_secret.view().size());
    made.append(prefix);
    made.append(id);
    made.append("=");
    for (const QChar sign : m_secret.view()) {
        const char one =
            static_cast<char>(sign.unicode()); // (a secret's shape is checked first: hex digits and dashes)
        made.append(QByteArrayView(&one, 1));
    }
    return made;
}

std::optional<base::Error> ProxmoxDevice::heldSignIn() const
{
    const SignInPacing::Mark mark = offered();
    if (pacing().waitLeftMs(mark) <= 0) {
        return std::nullopt;
    }
    if (pacing().times(mark) > 0) {
        return tokenRefused();
    }
    if (m_signInError && m_heldAs && pacing().standing(mark) == m_heldAs) {
        return m_signInError;
    }
    return notSigningIn("a sign-in with this token is under way, or the last one came to nothing");
}

QString ProxmoxDevice::wayOf() const
{
    // (Its address, and what a line to it is held to: the pin, or Windows' trust where it is known by none.)
    return m_where.address.toString(QUrl::FullyEncoded) + QChar(0) +
           (m_where.knownByCertificate ? QString::fromLatin1(m_where.certificate.toHex()) : u"windows"_s);
}

ProxmoxDevice::Touch ProxmoxDevice::touchNow() const
{
    const TouchLost& lost = touchLost();
    return Touch{lost.epoch, lost.at.value(m_where.address.toString(QUrl::FullyEncoded)), lost.at.value(wayOf())};
}

bool ProxmoxDevice::signedIn() const
{
    return m_signedIn && m_touched == touchNow();
}

void ProxmoxDevice::signedInNow()
{
    m_signedIn = true;
    ++m_signIns; // (a sign-in of its own from here: what was read before it is a sign-in old)
    // On what answered its unsigned question, as contact with its Proxmox stood then: where it was lost after that, by
    // any guest's driver of it, while this sign-in was out, the sign-in stands for nothing, and is asked again first
    // (signedInOrAskedAgain; outside review 2, 4.2).
    m_touched = m_askedTouch;
}

void ProxmoxDevice::signedInFirst(std::function<void(std::optional<base::Error>)> done)
{
    if (signedIn()) {
        done(std::nullopt);
        return;
    }
    if (m_secret.isEmpty()) {
        later([done = std::move(done)] { done(noPasswordSaved()); });
        return;
    }
    if (!proxmox::isTokenId(m_where.user) || !proxmox::isTokenSecret(m_secret.view())) {
        // Checked before anything is sent: both go into a header, and what is no token is sent nowhere.
        later([done = std::move(done)] { done(proxmox::notAToken()); });
        return;
    }
    if (m_signingIn) {
        m_waitingForSignIn.append(std::move(done));
        return;
    }
    // Somebody else answered on its line a moment ago: nothing goes, and it is not asked again before its hold is over.
    if (std::optional<base::Error> held = heldAsNoProxmox()) {
        later([done = std::move(done), held = std::move(*held)] { done(held); });
        return;
    }
    if (std::optional<base::Error> held = heldSignIn()) {
        if (pacing().onlyOut(offered())) {
            // Another guest's sign-in with this token is out (at a wall's start they all look at once): what it comes
            // to is waited for, and not said as a failure (the web's `_SIGNING_IN`).
            m_waitingForSignIn.append(std::move(done));
            m_signingIn = true;
            afterTheOtherSignIn();
            return;
        }
        later([done = std::move(done), held = std::move(*held)] { done(held); });
        return;
    }
    m_waitingForSignIn.append(std::move(done));
    signIn();
}

void ProxmoxDevice::afterTheOtherSignIn()
{
    emit waitsForAnotherSignIn();
    const SignInPacing::Mark mark = offered();
    pacing().whenSettled(mark, this, pacing().waitLeftMs(mark) + 5, [this] { settledByTheOther(); });
}

void ProxmoxDevice::settledByTheOther()
{
    const SignInPacing::Mark mark = offered();
    std::optional<base::Error> held = heldSignIn();
    if (held && pacing().onlyOut(mark)) {
        afterTheOtherSignIn();
        return;
    }
    if (held) {
        signInOver(held);
        return;
    }
    if (!pacing().standing(mark)) {
        // Taken, and nothing noted against the token since: so is this guest, the same token at the same Proxmox, and
        // nothing is sent for it; once what answers on its own line said it is a Proxmox (the other's sign-in went on
        // the other's line). (Every question after it carries the token, and a 401 to one is a sign-in again,
        // counted, as for any guest.)
        if (!askedJustNow()) {
            askWhatAnswers([this] { settledByTheOther(); });
            return;
        }
        m_askedAt.reset(); // (this taking's: the next sign-in asks again)
        signedInNow();
        m_version = versionsTaken().value(mark.digest);
        signedInOrAskedAgain();
        return;
    }
    signIn();
}

bool ProxmoxDevice::askedJustNow() const
{
    // (Within a sign-in's time: as long as one guest waits for another's.)
    return m_askedAt &&
           now() - *m_askedAt <= SignInPacing::outFor(m_pace.connectWithinMs, m_pace.signInQuietMs, m_pace.transferMs);
}

std::optional<base::Error> ProxmoxDevice::heldAsNoProxmox() const
{
    if (m_noProxmox && now() < m_noProxmoxUntil) {
        return m_noProxmox;
    }
    return std::nullopt;
}

void ProxmoxDevice::askWhatAnswers(std::function<void()> then)
{
    // With nothing in it of the token, nor of who asks; on this driver's own line, held to what its Proxmox is known by
    // as every question of its is (what shows another certificate hears nothing at all).
    DeviceAsker::Ask question;
    question.path = QString(api) + u"/version"_s;
    question.largest = largestVersionBytes;
    question.sent = {{"Accept"_ba, "application/json"_ba}};
    question.headers = {"WWW-Authenticate"_ba};
    // Waited for as the sign-in is: a real Proxmox answers it three seconds late, as it holds back every 401 of its
    // (review cx3-2, L3; measured on a real one: "401 No ticket" after 3.07 s).
    question.quietMs = m_pace.signInQuietMs;
    // (Its reply; and contact with its Proxmox as it stands as it is asked: what a sign-in after it stands on. Outside
    // review 2, 4.2.)
    struct Asking {
        QPointer<QNetworkReply> line;
        Touch touch;
    };
    auto asking = std::make_shared<Asking>();
    asking->touch = touchNow();
    question.handed = [asking](QNetworkReply* reply) { asking->line = reply; };
    m_asker.send(question, QByteArray(), [this, then = std::move(then), asking](const base::Result<Reply>& reply) {
        if (!reply) {
            // Nothing answered, or not on a line held to what it is known by: as a sign-in that could not go. Where
            // nothing came back at all, a failure of contact as any question's is, for every guest of its Proxmox
            // (outside review 2, 4.2).
            if (!answerBegan(asking->line)) {
                outOfTouch(reply.error());
            }
            signInOver(reply.error());
            return;
        }
        answered();
        // (A sign-in of its own asked for: somebody else's, as proxmox::saysItIsAProxmox says.)
        const bool challenged = !reply.value().headers.value("www-authenticate"_ba).isEmpty();
        if (!proxmox::saysItIsAProxmox(answerIn(reply.value()), challenged)) {
            // Somebody else at its address: told so, sent nothing, and asked again later each time, as a failure.
            if (++m_noProxmoxTimes == 1) {
                qWarning("qKVM: what answers at a Proxmox guest's address does not say it is Proxmox VE: the token is "
                         "not sent to it");
            }
            const qint64 hold = qint64{m_pace.holdMs} << std::min(m_noProxmoxTimes - 1, 10);
            m_noProxmoxUntil = now() + std::min<qint64>(m_pace.longestHoldMs, hold);
            m_noProxmox = proxmox::notAProxmoxThere(reply.value().status, challenged);
            signInOver(m_noProxmox);
            return;
        }
        m_noProxmox.reset();
        m_noProxmoxTimes = 0;
        m_askedAt = now();
        m_askedTouch = asking->touch;
        then();
    });
}

void ProxmoxDevice::signIn()
{
    m_signingIn = true;
    if (!askedJustNow()) {
        askWhatAnswers([this] {
            // Asked: the sign-in decided afresh for whoever waits, as at first (another guest's with the token may be
            // out by now: then waited for, and taken for this one's).
            m_signingIn = false;
            for (auto& waiting : std::exchange(m_waitingForSignIn, {})) {
                signedInFirst(std::move(waiting));
            }
        });
        return;
    }
    m_askedAt.reset(); // (this sign-in's: the next one asks again)
    DeviceAsker::Ask question;
    question.path = QString(api) + u"/version"_s;
    question.largest = largestReplyBytes;
    question.sent = {{"Accept"_ba, "application/json"_ba}};
    // Waited for as every family's sign-in is: Proxmox holds a no back three seconds, and a no heard as none would
    // have the token go out again at the next look. (Counted as it goes: never "not there" on a guess.)
    question.quietMs = m_pace.signInQuietMs;
    question.doesSomething = true;
    auto out = std::make_shared<SignInOut>();
    out->pacing = m_pace.pacing;
    out->mark = offered();
    out->steps = steps();
    out->sent = pacing().sent(out->mark, out->steps);
    m_signInOut = out;
    auto line = std::make_shared<QPointer<QNetworkReply>>();
    question.handed = [line](QNetworkReply* reply) { *line = reply; };
    QByteArray token = authorization();
    const WipedOnLeaving overTheToken(token);
    m_asker.send(question, token, [this, out, line](const base::Result<Reply>& reply) {
        m_signInOut.reset();
        SignInPacing& kept = pacing();
        if (!reply) {
            // Where nothing came back at all, a failure of contact as any question's is, for every guest of its
            // Proxmox (outside review 2, 4.2).
            if (!answerBegan(*line)) {
                outOfTouch(reply.error());
            }
            // Where it went out and nothing answered it, held back as one that came to neither yes nor no; where
            // nothing reached Proxmox, nothing is kept of it.
            if (mayHaveBeenDone(reply.error())) {
                kept.unanswered(out->mark, out->steps, out->sent);
                m_signInError = notSigningIn(reply.error().message);
                m_heldAs = kept.standing(out->mark);
            } else {
                kept.putBack(out->mark, out->sent);
            }
            signInOver(failed(reply.error()));
            return;
        }
        answered();
        if (reply.value().status == 401) {
            kept.saidNo(out->mark, out->steps);
            m_signInError = tokenRefused();
            m_heldAs = kept.standing(out->mark);
            qWarning("qKVM: a Proxmox rejected the API token of a guest");
            signInOver(m_signInError);
            return;
        }
        const base::Result<QJsonValue> said =
            proxmox::dataOf(answerIn(reply.value()), u"say its version", m_where.user, m_secret.view());
        const QJsonValue version = said ? said.value().toObject().value("version"_L1) : QJsonValue();
        if (!version.isString()) {
            // Neither yes nor no: not sent again at every look meanwhile (the web's `_neither_yes_nor_no`).
            kept.unanswered(out->mark, out->steps, out->sent);
            const base::Error why =
                said ? base::Error{"bad_response", "What answers does not say it is Proxmox VE"} : said.error();
            m_signInError = notSigningIn(why.message);
            m_heldAs = kept.standing(out->mark);
            signInOver(m_signInError);
            return;
        }
        kept.accepted(out->mark);
        m_signInError.reset();
        m_heldAs.reset();
        m_version = kvmd::plain(version, 20);
        keepVersion(out->mark.digest, m_version);
        signedInNow();
        signedInOrAskedAgain();
    });
}

void ProxmoxDevice::signedInOrAskedAgain()
{
    // Contact with its Proxmox was lost while the sign-in was out, after its unsigned question went, by any guest's
    // driver of it: the sign-in stands for nothing, and whoever waits for it has what answers there asked again, and a
    // sign-in again, before the token goes with their question (outside review 2, 4.2). Else signed in, for all of
    // them.
    if (!signedIn()) {
        m_signingIn = false;
        for (auto& waiting : std::exchange(m_waitingForSignIn, {})) {
            signedInFirst(std::move(waiting));
        }
        return;
    }
    signInOver(std::nullopt);
}

void ProxmoxDevice::signInOver(const std::optional<base::Error>& why)
{
    m_signingIn = false;
    for (const auto& waiting : std::exchange(m_waitingForSignIn, {})) {
        waiting(why);
    }
}

// --- asking ------------------------------------------------------------------------------------------------

void ProxmoxDevice::request(const Ask& question, std::function<void(base::Result<Reply>)> done)
{
    request(question, std::move(done), Tries{});
}

void ProxmoxDevice::request(const Ask& question, std::function<void(base::Result<Reply>)> done, Tries tries)
{
    // A question that does something to the guest (a power command, a console's ticket, a change of its settings) that
    // finds its driver signed out on its way (contact with what answers at its address failed while the reading it was
    // decided on was out, its console's line fell, or another question was answered 401) is treated as one refused
    // 401: the sign-in, and which virtual machine has the number and what is to be so of its settings asked again on a
    // reading after it, within its moment, before it goes; and it is sent only where both hold. Otherwise it went on a
    // reading a sign-in old, past the bound of a press (review cx3-3, L1). And so is one decided on a reading that went
    // out under an earlier sign-in than the one that stands now (Ask::decidedUnder: signed out and in again in full
    // while that reading was out, by another question of the driver's; outside review 2, 4.1): a reading a sign-in old
    // decides nothing, also where its driver is signed in again by the time it comes. Signed out or in again by the
    // time the reading after that is judged, it is not sent at all.
    const bool decidedBefore = question.decidedUnder && *question.decidedUnder != m_signIns;
    if (question.doesSomething && (!signedIn() || decidedBefore)) {
        if (tries.checkedAfterASignIn) {
            later([done = std::move(done)] {
                done(notSentAfterASignIn("its driver had to sign in again while the guest was asked"));
            });
            return;
        }
        tries.checkedAfterASignIn = true;
        checkedAgain(question, std::move(done), tries, notSentAfterASignIn);
        return;
    }
    // A question held to a moment (Ask::untilMs: a press's wait) is held to it while it waits for a sign-in as well:
    // the sign-in goes on for whoever else waits for it, and this question is answered at that moment, nothing sent for
    // it (review c9b-1, L6). What is answered first is the answer.
    auto told = std::make_shared<bool>(false);
    if (question.untilMs && !signedIn()) {
        const qint64 left = std::clamp<qint64>(*question.untilMs - now(), 0, std::numeric_limits<int>::max());
        QTimer::singleShot(static_cast<int>(left), this, [told, done, doesSomething = question.doesSomething] {
            if (std::exchange(*told, true)) {
                return;
            }
            done(doesSomething ? base::Error{"unreachable", "Proxmox's sign-in did not come back in time: nothing was "
                                                            "sent to the guest"}
                               : base::Error{"timeout", "Proxmox's sign-in did not come back in time"});
        });
    }
    signedInFirst([this, question, tries, told, done = std::move(done)](const std::optional<base::Error>& why) {
        if (std::exchange(*told, true)) {
            return;
        }
        if (why) {
            done(question.vital ? failed(*why) : *why);
            return;
        }
        DeviceAsker::Ask asked;
        asked.method = question.method;
        // (A guest's question goes to the node it is on as it goes: sent again after a 401, where the question of rule
        // 30 before it followed the guest to another node, it goes there. Review cx3-1, L1.)
        asked.path = QString(api) + (question.ofTheGuest ? guestPath(question.path) : question.path);
        asked.query = question.query;
        asked.body = question.body;
        if (!question.body.isEmpty()) {
            asked.bodyIs = "application/x-www-form-urlencoded"_ba;
        }
        asked.largest = question.largest > 0 ? question.largest : largestReplyBytes;
        asked.doesSomething = question.doesSomething;
        if (question.untilMs) {
            // What is left until then, and never more than the question's own whole: the moment shortens a question's
            // wait, it does not lengthen it; and never less than getting a connection may take (the web's `bounded`).
            const qint64 whole = qint64{m_pace.connectWithinMs} + m_pace.quietMs + m_pace.transferMs;
            const qint64 least = qint64{m_pace.connectWithinMs} + connectMarginMs;
            asked.wholeMs = static_cast<int>(std::clamp(*question.untilMs - now(), least, std::max(least, whole)));
        }
        asked.sent = {{"Accept"_ba, "application/json"_ba}};
        // Whether an answer began on this very question: its status line came, whatever became of the rest of it. Read
        // off the question's own reply as it is handed back, which is there until a turn of the loop after that: the
        // asker's own refusal of an answer too large or packed comes out of the very moment its status line does,
        // before anybody else hears of that moment (review cx3-3, L3).
        auto line = std::make_shared<QPointer<QNetworkReply>>();
        asked.handed = [line](QNetworkReply* reply) { *line = reply; };
        // (The sign-in it goes under, for whoever decides on its answer: outside review 2, 4.1.)
        if (question.wentUnder) {
            *question.wentUnder = m_signIns;
        }
        QByteArray token = authorization();
        const WipedOnLeaving overTheToken(token);
        m_asker.send(asked, token, [this, question, tries, done, line](base::Result<Reply> reply) {
            if (!reply) {
                // (Only a question that got nothing back at all: one whose answer began, and was too slow or too large,
                // was answered on a line held to what the Proxmox is known by.)
                if (!answerBegan(*line)) {
                    outOfTouch(reply.error());
                }
                done(question.vital ? failed(reply.error()) : reply.error());
                return;
            }
            answered();
            if (reply.value().status != 401) {
                done(std::move(reply));
                return;
            }
            m_signedIn = false;
            if (!tries.signedInAgain) {
                // The token was taken back, or Proxmox restarted: signed in once more, counted, and asked again, as
                // the web does (a command answered 401 was not carried out: sent again, it is sent once).
                if (question.doesSomething) {
                    // A question that does something to the guest (a power command, a console's ticket, a change of
                    // its settings) goes again only once which virtual machine has its number was asked again (rule
                    // 30; outside review of 0.2.0, 3.3; the web sends it again unasked): a sign-in's time went by
                    // since that was asked, in which another could have been made under it, and Proxmox's commands and
                    // its console take nothing that holds them to the guest. Another is sent nothing; and where that
                    // could not be asked, nothing was done: the first was refused, and none went a second time.
                    checkedAgain(question, done, Tries{true, tries.checkedAfterASignIn}, notSentAgain);
                    return;
                }
                request(question, done, Tries{true, tries.checkedAfterASignIn});
                return;
            }
            // It takes the token for its version and refuses it for this: every look would end here. Not sent at
            // every look meanwhile: held back as a sign-in that came to nothing.
            pacing().cameToNothingAgain(offered(), steps());
            m_signInError = refusesAfterTaking();
            m_heldAs = pacing().standing(offered());
            done(*m_signInError);
        });
    });
}

void ProxmoxDevice::checkedAgain(const Ask& question, std::function<void(base::Result<Reply>)> done, Tries tries,
                                 base::Error (*notSent)(const std::string& why))
{
    // Its moment: the one it is to go out by or not at all (a press's), else the one its answer is to be in by.
    const std::optional<qint64> until = question.againUntilMs ? question.againUntilMs : question.untilMs;
    // (And what is to be so of its settings for it, looked at on that very reading, as before it was sent the first
    // time: a sign-in's time and more went by, in which another hand could have changed them. Review cx3-2, L2.)
    auto read = std::make_shared<bool>(false);
    auto under = std::make_shared<quint64>(0);
    auto amiss = std::make_shared<std::optional<base::Error>>();
    sameGuest(
        question.vital,
        [this, question, done = std::move(done), tries, until, read, under, amiss,
         notSent](const base::Result<void>& same) {
            if (same && until && now() > *until) {
                done(notSent("the time it was to go in was over by then"));
                return;
            }
            if (!same) {
                done(same.error().code == "another_machine"
                         ? same.error()
                         : notSent("which virtual machine has the guest's number could not be asked after that: " +
                                   same.error().message));
                return;
            }
            if (question.lastCheck && !*read) {
                done(notSent("the guest's settings could not be read right before it"));
                return;
            }
            if (*amiss) {
                done(**amiss);
                return;
            }
            // Decided on that reading from here on, and held to the sign-in it went under (outside review 2, 4.1); on
            // none where none was made (no number kept, and its settings not read: nothing to tell it apart by).
            Ask decided = question;
            decided.decidedUnder = *read ? std::optional<quint64>(*under) : std::nullopt;
            request(decided, done, tries);
        },
        until,
        [read, under, amiss, check = question.lastCheck](const QJsonValue& config, quint64 signIn) {
            *read = true;
            *under = signIn;
            if (check) {
                *amiss = check(config);
            }
        });
}

proxmox::Answer ProxmoxDevice::answerIn(const Reply& reply)
{
    return proxmox::Answer{reply.status, reply.body, reply.reason};
}

QString ProxmoxDevice::guestPath(const QString& step) const
{
    return guestPath(m_where.node, step);
}

QString ProxmoxDevice::guestPath(const QString& node, const QString& step) const
{
    return u"/nodes/%1/qemu/%2/%3"_s.arg(node).arg(m_where.vmid).arg(step);
}

void ProxmoxDevice::ofTheGuest(const Ask& question, const QString& what,
                               std::function<void(base::Result<QJsonValue>)> done, bool followed)
{
    if (!proxmox::isNode(m_where.node) || !proxmox::isGuestNumber(m_where.vmid)) {
        later([done = std::move(done)] { done(base::Error{"invalid", "This is not the address of a Proxmox guest"}); });
        return;
    }
    Ask asked = question;
    asked.ofTheGuest = true;
    request(asked, [this, question, what, followed, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(reply.error());
            return;
        }
        const proxmox::Answer answer = answerIn(reply.value());
        base::Result<QJsonValue> said = proxmox::dataOf(answer, what, m_where.user, m_secret.view());
        if (!said && question.doesSomething && proxmox::faultOf(answer) == proxmox::Fault::AnswerLost) {
            // Its answer did not come back, and the command may have been carried out: said as a press with no answer
            // is, never as not done.
            followUp();
            done(base::Error{"timeout", "Proxmox's answer to the command did not come back (" + said.error().message +
                                            "): it may have been done. Look at the guest before trying again."});
            return;
        }
        if (said || followed || proxmox::faultOf(answer) != proxmox::Fault::WrongNode) {
            done(std::move(said));
            return;
        }
        // Not on the node asked (moved within its cluster, or the node is no more): followed once to where its
        // Proxmox lists it now, once the virtual machine with its number there is found to be this guest. Another one,
        // made there after this one was deleted, is asked nothing and sent nothing.
        const base::Error notThere = said.error();
        // (Held to the question's moment, where it has one: a follow in a press's wait is part of that wait; and a
        // command's to the moment it is to go out by or not at all, as after a 401: review cx3-1, L1.)
        const std::optional<qint64> by = question.untilMs ? question.untilMs : question.againUntilMs;
        // (A command Proxmox answered so was not carried out: whatever stops it going where the guest is now, nothing
        // of it was done, and that is said; another virtual machine there is said as that.)
        const auto notOn = [doesSomething = question.doesSomething](const base::Error& why) {
            return !doesSomething || why.code == "another_machine" ? why : notSentWhereItWent(why.message);
        };
        // (Nothing of the follow goes once that moment is over, a reading's as a command's, nor in a reading's last
        // half second: each question of it would be given a connection's time at the least, past it. Outside review
        // 2, 4.4.)
        const auto overBy = [this, by, doesSomething = question.doesSomething]() -> std::optional<base::Error> {
            if (!by || now() < *by - (doesSomething ? 0 : connectMarginMs)) {
                return std::nullopt;
            }
            return doesSomething ? notSentWhereItWent("the time it was to go in was over by then")
                                 : base::Error{"timeout", "Proxmox said the guest is not on the node it was asked on, "
                                                          "and its time was over before it was followed to where it "
                                                          "is now"};
        };
        if (const std::optional<base::Error> over = overBy()) {
            done(*over);
            return;
        }
        whereNow(
            by,
            // (What tidy follows to is a text made out of memory: nothing of ours throws, DESIGN.md section 7.)
            // NOLINTNEXTLINE(bugprone-exception-escape)
            [this, question, what, notThere, by, notOn, overBy,
             done](const base::Result<std::optional<QString>>& movedTo) {
                if (!movedTo) {
                    done(notOn(movedTo.error()));
                    return;
                }
                if (!movedTo.value() || *movedTo.value() == m_where.node) {
                    done(notOn(base::Error{
                        "bad_response",
                        u"Proxmox has no guest %1 on %2, nor on another node the token can see (%3)"_s.arg(m_where.vmid)
                            .arg(m_where.node, QString::fromStdString(notThere.message))
                            .toStdString()}));
                    return;
                }
                if (const std::optional<base::Error> over = overBy()) {
                    done(*over);
                    return;
                }
                const QString node = *movedTo.value();
                auto there = std::make_shared<QJsonValue>();
                numberAt(
                    node,
                    [this, question, what, node, notOn, overBy, done, there](const base::Result<QString>& number) {
                        if (!number) {
                            done(notOn(number.error()));
                            return;
                        }
                        if (const std::optional<base::Error> refused = judge(number.value())) {
                            done(*refused);
                            return;
                        }
                        if (!std::exchange(m_movedSaid, true)) {
                            qInfo("qKVM: a Proxmox guest is on another node of its cluster now, and is followed there");
                        }
                        m_where.node = node;
                        if (const std::optional<base::Error> over = overBy()) {
                            done(*over);
                            return;
                        }
                        // (And what is to be so of its settings for it, on that very reading there, as before it was
                        // sent where it went: review cx3-2, L2.)
                        if (question.lastCheck) {
                            if (const std::optional<base::Error> amiss = question.lastCheck(*there)) {
                                done(*amiss);
                                return;
                            }
                        }
                        // (A command goes there held to the sign-in of the reading it was decided on, as it was:
                        // signed in again since, it is decided again first, on the node it went to. Outside review 2,
                        // 4.1.)
                        ofTheGuest(question, what, done, true);
                    },
                    by, [there](const QJsonValue& config, quint64 /*signIn*/) { *there = config; });
            });
    });
}

// --- rule 30: which virtual machine has the guest's number ----------------------------------------------

base::Error ProxmoxDevice::anotherGuest()
{
    return {"another_machine",
            "Another virtual machine has this guest's number on its Proxmox now: it gives another SMBIOS UUID than "
            "the guest this tile was added for (Proxmox gives a new virtual machine the number of one that was "
            "deleted). Nothing is done to it: no power button, no key, no console. If it is this tile's guest now, "
            "edit the tile and type its token's secret."};
}

base::Error ProxmoxDevice::noNumberGiven()
{
    return {"another_machine",
            "The virtual machine with this guest's number on its Proxmox gives no SMBIOS UUID, and the guest this "
            "tile was added for did. Nothing is done to it: no power button, no key, no console. If it is this "
            "tile's guest now, edit the tile and type its token's secret."};
}

ProxmoxDevice::Places& ProxmoxDevice::Places::program()
{
    static Places places;
    return places;
}

qint64 ProxmoxDevice::Places::now() const
{
    return m_clock.isValid() ? m_clock.elapsed() : 0;
}

std::optional<QString> ProxmoxDevice::Places::knownNode(const QString& listing, qint64 vmid, qint64 withinMs) const
{
    const auto found = m_listed.constFind(listing);
    if (found == m_listed.cend() || now() - found->at >= withinMs || !found->nodes.contains(vmid)) {
        return std::nullopt;
    }
    return found->nodes.value(vmid);
}

bool ProxmoxDevice::Places::mayReadFor(const QString& guest, qint64 withinMs)
{
    if (!m_clock.isValid()) {
        m_clock.start();
    }
    const auto before = m_readFor.constFind(guest);
    if (before != m_readFor.cend() && now() - *before < withinMs) {
        return false;
    }
    if (m_readFor.size() >= mostGuestsFollowed) {
        m_readFor.clear(); // (what is lost is a reading of a list sooner than the minute, once)
    }
    m_readFor.insert(guest, now());
    return true;
}

void ProxmoxDevice::Places::read(const QString& listing, const QHash<qint64, QString>& nodes)
{
    if (!m_clock.isValid()) {
        m_clock.start();
    }
    if (m_listed.size() >= mostProxmoxesListed && !m_listed.contains(listing)) {
        m_listed.clear();
    }
    m_listed.insert(listing, Listed{now(), nodes});
}

ProxmoxDevice::Places& ProxmoxDevice::places() const
{
    return m_pace.places ? *m_pace.places : Places::program();
}

void ProxmoxDevice::whereNow(std::optional<qint64> untilMs,
                             std::function<void(base::Result<std::optional<QString>>)> done)
{
    // By Proxmox and token (nothing of the secret: its ID names the token), and by guest.
    const QString ofTheToken = m_where.address.toString(QUrl::FullyEncoded) + QChar(0) + m_where.user;
    const QString guest = ofTheToken + QChar(0) + QString::number(m_where.vmid);
    if (const std::optional<QString> known = places().knownNode(ofTheToken, m_where.vmid, m_pace.relistEveryMs);
        known && *known != m_where.node) {
        // (A list read a moment ago, for this guest or another, says where it went.)
        later([done = std::move(done), known] { done(known); });
        return;
    }
    if (!places().mayReadFor(guest, m_pace.relistEveryMs)) {
        later([done = std::move(done)] { done(std::optional<QString>()); });
        return;
    }
    listingBy(untilMs, [this, ofTheToken, done = std::move(done)](const base::Result<proxmox::Listing>& listed) {
        if (!listed) {
            done(listed.error());
            return;
        }
        QHash<qint64, QString> nodes;
        for (const proxmox::Listed& one : listed.value().guests) {
            nodes.insert(one.vmid, one.node);
        }
        places().read(ofTheToken, nodes);
        done(nodes.contains(m_where.vmid) ? std::optional<QString>(nodes.value(m_where.vmid)) : std::nullopt);
    });
}

void ProxmoxDevice::numberAt(const QString& node, std::function<void(base::Result<QString>)> done,
                             std::optional<qint64> untilMs, Reading read)
{
    Ask asked;
    asked.path = guestPath(node, u"config"_s);
    asked.untilMs = untilMs;
    asked.wentUnder = std::make_shared<quint64>(0);
    request(asked, [this, done = std::move(done), read = std::move(read),
                    under = asked.wentUnder](const base::Result<Reply>& reply) {
        if (!reply) {
            done(reply.error());
            return;
        }
        const base::Result<QJsonValue> said =
            proxmox::dataOf(answerIn(reply.value()), u"read this guest's settings", m_where.user, m_secret.view());
        if (!said) {
            done(said.error());
            return;
        }
        if (read) {
            read(said.value(), *under);
        }
        done(proxmox::ownNumber(said.value()));
    });
}

void ProxmoxDevice::sameGuest(bool vital, std::function<void(base::Result<void>)> done, std::optional<qint64> untilMs,
                              Reading read)
{
    Ask asked;
    asked.path = u"config"_s;
    asked.vital = vital;
    asked.untilMs = untilMs;
    // (Told the sign-in its reading went under: where it is followed to another node, the last, which is judged.)
    asked.wentUnder = std::make_shared<quint64>(0);
    ofTheGuest(asked, u"read this guest's settings"_s,
               [this, done = std::move(done), read = std::move(read),
                under = asked.wentUnder](const base::Result<QJsonValue>& config) {
                   if (!config) {
                       // Not read just now: where a number is kept, nothing is decided on that (it is not "it gives
                       // none"), and nothing goes. Where none is kept, there is nothing to tell it apart by.
                       if (!m_machine.isEmpty() || config.error().code == "another_machine") {
                           done(config.error());
                           return;
                       }
                       done(base::Result<void>());
                       return;
                   }
                   if (const std::optional<base::Error> refused = judge(proxmox::ownNumber(config.value()))) {
                       done(*refused);
                       return;
                   }
                   driveFrom(config.value()); // (the guest's: what its CD-ROM drive holds, for its card)
                   if (read) {
                       read(config.value(), *under);
                   }
                   done(base::Result<void>());
               });
}

std::optional<base::Error> ProxmoxDevice::judge(const QString& seen)
{
    m_judgedAt = now();
    ++m_judges;
    if (seen.isEmpty()) {
        if (!m_machine.isEmpty()) {
            return another(noNumberGiven());
        }
        return std::nullopt;
    }
    if (m_machine.isEmpty()) {
        // None was kept (a guest from a file without one): the first seen is taken, as a certificate is, and said.
        m_machine = seen;
        qInfo("qKVM: a Proxmox guest is known by the number it gives for itself from now on");
        emit knownAs(QString(), seen);
        return std::nullopt;
    }
    if (seen != m_machine) {
        return another(anotherGuest());
    }
    if (m_other) {
        qInfo("qKVM: the Proxmox guest a card was added for has its number again");
        m_other.reset();
        m_statusAt = std::numeric_limits<qint64>::min() / 2; // (its card says so at the next look)
    }
    return std::nullopt;
}

base::Error ProxmoxDevice::another(const base::Error& words)
{
    if (!m_other || m_other->message != words.message) {
        qWarning("qKVM: another virtual machine has a Proxmox guest's number now: nothing is sent to it");
        m_statusAt = std::numeric_limits<qint64>::min() / 2; // (its card says it at the next look)
    }
    m_other = words;
    // What was read of it is another's: its power is not taken for this guest's, nor its process's start.
    m_powerOn.reset();
    m_beganAt.reset();
    // Nor its screen: every live of the guest ends, in these words, and its console is let go of (outside review of
    // 0.2.0, 3.3: one opened between the question which virtual machine has the number and its ticket may be the
    // other's; the web leaves it open). The next is opened, where the guest has its number again, only once that is
    // known (rfbWay).
    endLives(words);
    if (m_screen != nullptr) {
        m_screen->disconnect(this);
        m_screen->close();
        // (Let go of a turn of the loop later: this may be the answer to its own asking for a way in.)
        m_screen.release()->deleteLater();
    }
    m_still.reset();
    return words;
}

bool ProxmoxDevice::anew(const proxmox::Current& current) const
{
    if (!current.on) {
        return false;
    }
    if (!m_powerOn || *current.on != *m_powerOn) {
        return true;
    }
    if (!*current.on || !current.uptime || *current.uptime == 0 || !m_beganAt) {
        return false; // (stopped and still stopped; or no uptime said: the next look finds it)
    }
    return now() - *current.uptime * 1000 > *m_beganAt + m_pace.restartedByMs;
}

void ProxmoxDevice::beganAgain(const proxmox::Current& current)
{
    if (!current.uptime || *current.uptime == 0) {
        return; // (none said; or a process in its first second, which the next look finds)
    }
    m_beganAt = now() - *current.uptime * 1000;
}

void ProxmoxDevice::seen(const proxmox::Current& current)
{
    m_powerOn = current.on;
    if (current.on == std::optional<bool>(false)) {
        m_beganAt.reset();
    } else if (current.on) {
        beganAgain(current);
    }
}

Status ProxmoxDevice::anotherOnTheCard(const base::Error& why)
{
    // Said at once, and nothing of the other virtual machine with it: no power, no processors, no screen.
    Status status;
    status.state = u"error"_s;
    status.hasPicture = false;
    status.errorCode = QString::fromStdString(why.code);
    status.errorMessage = QString::fromStdString(why.message);
    return status;
}

void ProxmoxDevice::answered()
{
    m_failures = 0;
    m_downError.reset();
}

base::Error ProxmoxDevice::failed(const base::Error& error)
{
    if (meansAway(error.code)) {
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

void ProxmoxDevice::outOfTouch(const base::Error& why)
{
    // Contact with what answers at its Proxmox's address failed, nothing back at all: nothing there, the line fell, or
    // another certificate than it is known by (or one Windows does not trust) was shown. The sign-in stood for what
    // answered on the line then, and the token is no session: whatever answers there next is asked what it is before
    // the token goes again (review cx3-2, L1). An answer of any kind, a refusal or an error among them, or one that
    // began and was too slow or too large, came on a line held to what the Proxmox is known by, and is no failure of
    // contact; nor is no answer in time, on a line let in so (contactLost: review cx3-3, L2). And so for every guest of
    // that Proxmox: their sign-ins stood for what answered then too (signedIn, touchLost: review cx3-3, L3). A
    // certificate refused, for the guests known the same way alone (its pin, or Windows' trust: wayOf): the others'
    // lines are held to what they accept, which it says nothing of, and a card still on its Proxmox's old certificate
    // would sign out at each of its askings the cards said yes to the new one (fresh review of 4.2, M1). Nothing there,
    // or a line fallen, for all of them: a backend switched under the same certificate shows only so.
    if (contactLost(why.code)) {
        m_signedIn = false;
        TouchLost& lost = touchLost();
        const bool certificate = why.code == "cert_mismatch" || why.code == "cert_invalid";
        const QString at = certificate ? wayOf() : m_where.address.toString(QUrl::FullyEncoded);
        if (lost.at.size() >= mostProxmoxesTouched && !lost.at.contains(at)) {
            lost.at.clear();
            ++lost.epoch;
        }
        lost.at.insert(at, ++lost.last);
    }
}

std::optional<base::Error> ProxmoxDevice::heldBack() const
{
    if (m_downError && now() < m_downUntil) {
        return m_downError;
    }
    return std::nullopt;
}

// --- what the wall asks -------------------------------------------------------------------------------------

void ProxmoxDevice::identify(std::function<void(base::Result<Identity>)> done)
{
    Ask asked;
    asked.path = u"status/current"_s;
    ofTheGuest(asked, u"look at this guest"_s, [this, done = std::move(done)](const base::Result<QJsonValue>& said) {
        if (!said) {
            done(said.error());
            return;
        }
        const proxmox::Current current = proxmox::currentOf(said.value());
        auto found = std::make_shared<Identity>();
        found->model = u"VM %1 on %2"_s.arg(m_where.vmid).arg(m_where.node);
        found->version = m_version.isEmpty() ? QString() : u"Proxmox VE "_s + m_version;
        found->hostname = current.name;
        // Added: the guest is known by the number it gives for itself from here on. Edited: it is the guest kept,
        // asked afresh, or the edit is refused (unless its token's secret was typed: the wall's to say).
        Ask settings;
        settings.path = u"config"_s;
        ofTheGuest(settings, u"read this guest's settings"_s,
                   [this, found, done](const base::Result<QJsonValue>& config) {
                       if (!config) {
                           if (!m_machine.isEmpty() && config.error().code != "another_machine") {
                               // Not read just now: that says nothing of the number, which is not "it gives none".
                               // What it is known by stays, and every command asks again before anything goes.
                               found->number.reset();
                               done(*found);
                               return;
                           }
                           // None kept, and which virtual machine has the number could not be told: nothing is
                           // taken (taken as "none", the next one made under the number would be taken on first use).
                           done(base::Error{config.error().code,
                                            "Which virtual machine has this guest's number could not be told: " +
                                                config.error().message});
                           return;
                       }
                       const QString seen = proxmox::ownNumber(config.value());
                       if (const std::optional<base::Error> refused = judge(seen)) {
                           done(*refused);
                           return;
                       }
                       driveFrom(config.value());
                       found->number = seen;
                       done(*found);
                   });
    });
}

void ProxmoxDevice::listing(std::function<void(base::Result<proxmox::Listing>)> done)
{
    listingBy(std::nullopt, std::move(done));
}

void ProxmoxDevice::listingBy(std::optional<qint64> untilMs, std::function<void(base::Result<proxmox::Listing>)> done)
{
    Ask asked;
    asked.path = u"/cluster/resources"_s;
    asked.untilMs = untilMs;
    asked.query.addQueryItem(u"type"_s, u"vm"_s);
    asked.largest = proxmox::largestListBytes;
    request(asked, [this, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(reply.error());
            return;
        }
        const base::Result<QJsonValue> said =
            proxmox::dataOf(answerIn(reply.value()), u"list its guests", m_where.user, m_secret.view());
        if (!said) {
            done(said.error());
            return;
        }
        done(proxmox::guestsIn(said.value()));
    });
}

void ProxmoxDevice::numberOfGuest(const QString& node, qint64 vmid, std::function<void(base::Result<QString>)> done)
{
    if (!proxmox::isNode(node) || !proxmox::isGuestNumber(vmid)) {
        later([done = std::move(done)] { done(base::Error{"invalid", "This is not the address of a Proxmox guest"}); });
        return;
    }
    Ask asked;
    asked.path = u"/nodes/%1/qemu/%2/config"_s.arg(node).arg(vmid);
    asked.vital = false;
    request(asked, [this, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(reply.error());
            return;
        }
        const base::Result<QJsonValue> said =
            proxmox::dataOf(answerIn(reply.value()), u"read a guest's settings", m_where.user, m_secret.view());
        if (!said) {
            done(said.error());
            return;
        }
        done(proxmox::ownNumber(said.value()));
    });
}

Status ProxmoxDevice::status() const
{
    Status status = m_status.value_or(Status{});
    status.hasPicture = false;
    // Its screen, keys, text, the live line and its control while it runs and nothing stands in the way of its console
    // (the web's `can`); why not, where something does (the web's no_screen).
    status.hasScreen = hasScreen() && (status.state == "online"_L1);
    if (status.hasScreen && status.capabilities) {
        *status.capabilities << u"screen"_s << u"keys"_s << u"text"_s << u"live"_s << u"control"_s;
    }
    if (m_screen != nullptr && !m_screen->picture().rgb.empty()) {
        status.width = m_screen->picture().width;
        status.height = m_screen->picture().height;
    }
    if (const std::optional<base::Error> why = noScreen();
        why && status.server && m_powerOn == std::optional<bool>(true)) {
        status.server->noScreen = QString::fromStdString(why->message);
    }
    // What is in its CD-ROM drive (proxmox_media.cpp), said to whoever sees the card; nothing of another virtual
    // machine's.
    status.media = mediaNow();
    return status;
}

bool ProxmoxDevice::isStale() const
{
    // (And at the moments after a command: FOLLOW_UP.)
    if (!m_followUpAt.isEmpty() && now() >= m_followUpAt.first()) {
        return true;
    }
    return !m_status || now() - m_statusAt > m_pace.freshMs;
}

void ProxmoxDevice::look()
{
    if (m_looking) {
        return;
    }
    m_looking = true;
    while (!m_followUpAt.isEmpty() && now() >= m_followUpAt.first()) {
        m_followUpAt.removeFirst();
    }
    if (m_other && m_judgedAt && now() - *m_judgedAt < m_pace.sameGuestEveryMs) {
        // Another virtual machine has the guest's number: said, and asked again once a minute, not at every look.
        later([this, why = *m_other] { lookOver(anotherOnTheCard(why)); });
        return;
    }
    if (std::optional<base::Error> held = heldBack()) {
        later([this, why = *held] { lookFailed(why); });
        return;
    }
    const int judges = m_judges;
    Ask asked;
    asked.path = u"status/current"_s;
    ofTheGuest(asked, u"look at this guest"_s, [this, judges](const base::Result<QJsonValue>& said) {
        if (!said) {
            lookFailed(said.error());
            return;
        }
        const proxmox::Current current = proxmox::currentOf(said.value());
        const auto over = [this, current] {
            // Another question judged meanwhile that another virtual machine has the guest's number (a press, a
            // console, its media): nothing this look read is said as the guest's (review c9b-1, L5).
            if (m_other) {
                lookOver(anotherOnTheCard(*m_other));
                return;
            }
            seen(current);
            Status status = proxmox::statusOf(current, m_where.vmid, m_where.node, m_version);
            // Its power buttons, and its virtual media (every guest: what it has is read at each question of them).
            status.capabilities = QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"media"_s};
            status.offersMedia = true;
            lookOver(std::move(status));
        };
        // Which virtual machine has the guest's number, asked after its state was read and before anything is taken
        // from it: once a minute, and at once where the look finds it otherwise than a virtual machine made under its
        // number mostly is. Not where the state's own read just asked, following the guest to another node.
        if (m_judges == judges && (!m_judgedAt || now() - *m_judgedAt >= m_pace.sameGuestEveryMs || anew(current))) {
            sameGuest(true, [this, over](const base::Result<void>& same) {
                if (!same) {
                    lookFailed(same.error());
                    return;
                }
                over();
            });
            return;
        }
        over();
    });
}

void ProxmoxDevice::lookFailed(const base::Error& why)
{
    if (why.code == "another_machine") {
        lookOver(anotherOnTheCard(why));
        return;
    }
    Status status;
    if (why.code == "auth_failed" || why.code == "no_password") {
        status.state = u"auth"_s;
    } else if (why.code == "cert_mismatch" || why.code == "cert_invalid" || why.code == "bad_response" ||
               why.code == "no_sign_in" || why.code == "invalid" || why.code == "not_found" || why.code == tlsDamaged) {
        // (Something for whoever sits here to look at, and not a Proxmox that is away.)
        status.state = u"error"_s;
    } else {
        status.state = u"offline"_s;
    }
    status.errorCode = QString::fromStdString(why.code);
    status.errorMessage = QString::fromStdString(why.message);
    lookOver(std::move(status));
}

void ProxmoxDevice::lookOver(Status status)
{
    status.hasPicture = false;
    m_status = std::move(status);
    m_statusAt = now();
    m_looking = false;
    emit looked();
}

void ProxmoxDevice::afterLook(int withinMs, std::function<void(bool stillLooking)> then)
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
    *heard = connect(this, &ProxmoxDevice::looked, this, run);
    QTimer::singleShot(withinMs, this, run);
}

void ProxmoxDevice::act(const Action& what, std::function<void(base::Result<Done>)> done)
{
    // Keys, Ctrl+Alt+Del and a text go over its console, as the web's (proxmox_screen.cpp).
    if (what.kind == "keys"_L1 || what.kind == "text"_L1 || what.kind == "ctrl_alt_del"_L1) {
        typeOnTheScreen(what, std::move(done));
        return;
    }
    static const QStringList buttons{u"power_on"_s, u"power_off"_s, u"atx_power"_s, u"atx_power_long"_s,
                                     u"atx_reset"_s};
    if (!buttons.contains(what.kind)) {
        const base::Error why =
            what.kind == "reboot"_L1
                ? base::Error{"invalid",
                              "A guest of Proxmox has no controller of its own to restart: reset the guest itself"}
                : base::Error{"invalid", "A guest of Proxmox cannot do that"};
        // (Said on the program's loop, as what came of a press: never from inside the call.)
        // NOLINTNEXTLINE(bugprone-exception-escape)
        QTimer::singleShot(0, QCoreApplication::instance(), [done = std::move(done), why] { done(why); });
        return;
    }
    pressButton(what.kind, nullptr, std::move(done));
}

void ProxmoxDevice::pressButton(const QString& kind, SettingsCheck lastCheck,
                                std::function<void(base::Result<Done>)> done)
{
    auto pressing = std::make_shared<Pressing>();
    pressing->kind = kind;
    pressing->began = now();
    pressing->lastCheck = std::move(lastCheck);
    pressing->done = std::move(done);
    m_pressing.append(pressing);
    // What it is decided by, held to one bound from the press, a sign-in and a follow on their way included.
    const qint64 decideBy = pressing->began + m_pace.decideWithinMs;
    Ask state;
    state.path = u"status/current"_s;
    state.untilMs = decideBy;
    ofTheGuest(state, u"look at this guest"_s, [this, pressing, decideBy](const base::Result<QJsonValue>& found) {
        if (!found) {
            pressed(pressing, found.error());
            return;
        }
        const proxmox::Current current = proxmox::currentOf(found.value());
        // Which virtual machine has the guest's number, right before the command: another one is sent nothing, and
        // what the question above found of it decides nothing (not even "already on"). And what is to be so of its
        // settings for this press (a start from the image: that image in its drive), on that very reading.
        auto checked = std::make_shared<std::optional<base::Error>>();
        auto read = std::make_shared<bool>(false);
        sameGuest(
            true,
            [this, pressing, current, checked, read](const base::Result<void>& same) {
                if (!same) {
                    pressed(pressing, same.error());
                    return;
                }
                if (pressing->lastCheck) {
                    // (Said where the command would go: one that is so already is said so, as for any press.)
                    pressing->amiss = !*read ? std::optional<base::Error>(base::Error{
                                                   "changed", "The guest's settings could not be read right before its "
                                                              "command: nothing was sent."})
                                             : *checked;
                }
                if (!current.on) {
                    // (Not "stopped" for want of a word: a button decided on a guess could stop what runs.)
                    pressed(pressing, base::Error{"bad_response", "Proxmox did not say whether the guest runs"});
                    return;
                }
                press(pressing, *current.on);
            },
            decideBy,
            [pressing, checked, read](const QJsonValue& config, quint64 signIn) {
                *read = true;
                pressing->decidedUnder = signIn;
                if (pressing->lastCheck) {
                    *checked = pressing->lastCheck(config);
                }
            });
    });
}

void ProxmoxDevice::press(const std::shared_ptr<Pressing>& pressing, bool running)
{
    const QString& kind = pressing->kind;
    if ((kind == "power_on"_L1 || kind == "power_off"_L1) && running == (kind == "power_on"_L1)) {
        // By what is wanted, not by which button: forty guests ticked after a power cut, some of which came back by
        // themselves, are not sorted into two piles by hand first.
        pressed(pressing, Done{running ? u"already on"_s : u"already off"_s});
        return;
    }
    if (kind == "atx_reset"_L1 && !running) {
        pressed(pressing, base::Error{"action_failed", "The guest is stopped: there is nothing to reset"});
        return;
    }
    if (pressing->amiss) {
        pressed(pressing, *pressing->amiss);
        return;
    }
    pressing->verb = kind == "power_on"_L1         ? u"start"_s
                     : kind == "power_off"_L1      ? u"shutdown"_s
                     : kind == "atx_power_long"_L1 ? u"stop"_s
                     : kind == "atx_reset"_L1      ? u"reset"_s
                     : running                     ? u"shutdown"_s
                                                   : u"start"_s;
    // Within the press's bound for it, or not at all: a question held to a moment is given a connection's time at the
    // least, and may come back past it.
    if (now() - pressing->began > m_pace.decideWithinMs) {
        pressed(pressing, base::Error{"action_failed", u"Nothing was sent: asking Proxmox how the guest is took longer "
                                                       u"than a press waits to send its command (%1 seconds)."_s
                                                           .arg((m_pace.decideWithinMs + 500) / 1000)
                                                           .toStdString()});
        return;
    }
    Ask command;
    command.method = "POST"_ba;
    command.path = u"status/"_s + pressing->verb;
    command.doesSomething = true;
    command.againUntilMs = pressing->began + m_pace.decideWithinMs;
    // ...and its own answer is held to that bound too, once it went: what is left of it, a connection's time and half
    // a second at the least (outside review 2, 4.4: it had a question's own whole time, 36.5 s more).
    command.untilMs = command.againUntilMs;
    command.lastCheck = pressing->lastCheck;
    // (Under the sign-in its reading of rule 30 went under, or decided again first: outside review 2, 4.1.)
    command.decidedUnder = pressing->decidedUnder;
    pressing->sent = true;
    ofTheGuest(command, pressing->verb + u" this guest"_s, [this, pressing](const base::Result<QJsonValue>& said) {
        if (!said && said.error().code == "timeout" && said.error().message == DeviceAsker::tookTooLong().message) {
            // Its answer did not come in its time: it went, and may have been carried out (and is looked at again).
            followUp();
            pressed(pressing, base::Error{"timeout", "Proxmox's answer to the command did not come back in time: it "
                                                     "may have been done. Look at the guest before trying again."});
            return;
        }
        if (!said) {
            pressed(pressing, said.error());
            return;
        }
        const QString upid = said.value().isString() ? said.value().toString() : QString();
        if (!upidShape().match(upid).hasMatch()) {
            pressed(pressing, base::Error{"bad_response", "Proxmox did not say that it took the power command"});
            return;
        }
        pressing->upid = upid;
        pressing->handed = upid.section(u':', 5, 5).startsWith(u"ha"_s);
        followUp();
        if (pressing->verb == "start"_L1 || pressing->verb == "stop"_L1) {
            pressing->endedAt = pressing->began;
            carriedOut(pressing);
            return;
        }
        pressed(pressing, Done{});
    });
}

void ProxmoxDevice::carriedOut(const std::shared_ptr<Pressing>& pressing)
{
    // Only once the guest is seen running (or stopped, for a stop) was the command carried out; meanwhile its task is
    // asked how it stands. Every read in the wait is held to the wait's bound from the press, and none is begun in its
    // last half second, or after it (a command whose answer came back then, a look that did, the last look held to
    // the bound itself): one begun then could not even be let in by the bound, and would be given more than a
    // connection's time past it. The wait is judged then on what was read (outside review 2, 4.4).
    const qint64 waitBy = pressing->began + m_pace.taskWithinMs + m_pace.confirmWithinMs;
    const qint64 readsBy = waitBy - connectMarginMs;
    if (now() >= readsBy) {
        judgeTheWait(pressing);
        return;
    }
    Ask look;
    look.path = u"status/current"_s;
    look.vital = false;
    look.untilMs = waitBy;
    ofTheGuest(look, u"look at this guest"_s, [this, pressing, readsBy](const base::Result<QJsonValue>& found) {
        const std::optional<bool> on = found ? proxmox::currentOf(found.value()).on : std::nullopt;
        const bool wanted = pressing->verb == "start"_L1;
        if (on == std::optional<bool>(wanted)) {
            pressed(pressing, Done{});
            return;
        }
        pressing->seenWas = pressing->seenWas || on == std::optional<bool>(!wanted);
        if ((!pressing->task || *pressing->task == taskRuns) && now() < readsBy) {
            taskOf(pressing, [this, pressing](const std::optional<QString>& task) {
                // (A read that fails says nothing: what was known stands.)
                if (task) {
                    pressing->task = task;
                    if (*task != taskRuns) {
                        pressing->endedAt = now();
                        pressing->seenWas = false;
                    }
                }
                judgeTheWait(pressing);
            });
            return;
        }
        judgeTheWait(pressing);
    });
}

void ProxmoxDevice::judgeTheWait(const std::shared_ptr<Pressing>& pressing)
{
    const std::optional<QString>& task = pressing->task;
    const QString verb = pressing->verb;
    if (task && *task != taskRuns && *task != taskOk) {
        pressed(pressing,
                base::Error{"action_failed", u"Proxmox could not %1 the guest: %2"_s.arg(verb, *task).toStdString()});
        return;
    }
    const qint64 waited = now() - pressing->began;
    const qint64 sinceEnd = now() - pressing->endedAt;
    const bool runs = task == std::optional<QString>(QString(taskRuns));
    const bool over = runs || pressing->handed ? waited >= m_pace.taskWithinMs
                                               : sinceEnd >= m_pace.confirmWithinMs ||
                                                     waited >= m_pace.taskWithinMs + m_pace.confirmWithinMs;
    if (!over) {
        // Looked at again a pace later where reads may still be begun then (carriedOut); else judged at the wait's
        // bound on what was read, nothing read after it (outside review 2, 4.4).
        const qint64 left = pressing->began + m_pace.taskWithinMs + m_pace.confirmWithinMs - now();
        if (left > m_pace.confirmEveryMs + connectMarginMs) {
            QTimer::singleShot(m_pace.confirmEveryMs, this, [this, pressing] { carriedOut(pressing); });
        } else {
            QTimer::singleShot(static_cast<int>(std::max<qint64>(left, 0)), this,
                               [this, pressing] { judgeTheWait(pressing); });
        }
        return;
    }
    const QString wanted = verb == "start"_L1 ? u"running"_s : u"stopped"_s;
    const QString was = verb == "start"_L1 ? u"stopped"_s : u"running"_s;
    const qint64 seconds = (waited + 500) / 1000;
    const auto timeout = [&](const QString& words) { pressed(pressing, base::Error{"timeout", words.toStdString()}); };
    if (runs) {
        timeout(u"Proxmox's task for the command is still running after %1 seconds, and the guest is not %2 yet. Look "
                u"at it in a moment before trying again."_s.arg(seconds)
                    .arg(wanted));
    } else if (pressing->handed && pressing->seenWas) {
        timeout(u"Proxmox handed the command to its high availability, and the guest is not %2 after %1 seconds. "
                u"Look at it in a moment before trying again."_s.arg(seconds)
                    .arg(wanted));
    } else if (pressing->handed) {
        // (No read of the guest came back after that: nothing is said of how it is. Second review, N1, as L1.)
        timeout(u"Proxmox handed the command to its high availability, and how the guest is was not read after that "
                u"(%1 seconds). Look at it in a moment before trying again."_s.arg(seconds));
    } else if (!task && pressing->seenWas) {
        timeout(u"The guest is still %2 after %1 seconds, and Proxmox did not say how its task went. Look at it before "
                u"trying again."_s.arg(seconds)
                    .arg(was));
    } else if (!task) {
        // (No read of the guest came back since the command: nothing is said of how it is. Fresh review of 4.4, L1.)
        timeout(u"How the guest is was not read since the command (%1 seconds), and Proxmox did not say how its task "
                u"went. Look at it before trying again."_s.arg(seconds));
    } else if (!pressing->seenWas) {
        timeout(u"Proxmox's task for the command ended well, and how the guest is was not read since. Look at it "
                u"before trying again."_s);
    } else if (sinceEnd < m_pace.confirmWithinMs) {
        timeout(u"Proxmox's task for the command ended well, and the guest is not %2 yet after %1 seconds. Look at it "
                u"in a moment before trying again."_s.arg(seconds)
                    .arg(wanted));
    } else {
        pressed(pressing,
                base::Error{"action_failed",
                            u"Proxmox took the command, but the guest is still %1. It was not carried out."_s.arg(was)
                                .toStdString()});
    }
}

void ProxmoxDevice::taskOf(const std::shared_ptr<Pressing>& pressing, std::function<void(std::optional<QString>)> done)
{
    // Asked of the node that runs it, which its name says (UPID:<node>:...).
    const QString node = pressing->upid.section(u':', 1, 1);
    if (!proxmox::isNode(node)) {
        later([done = std::move(done)] { done(std::nullopt); });
        return;
    }
    Ask asked;
    asked.path = u"/nodes/%1/tasks/%2/status"_s.arg(node, QString::fromLatin1(QUrl::toPercentEncoding(pressing->upid)));
    asked.vital = false;
    asked.untilMs = pressing->began + m_pace.taskWithinMs + m_pace.confirmWithinMs;
    request(asked, [this, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(std::nullopt);
            return;
        }
        const base::Result<QJsonValue> said =
            proxmox::dataOf(answerIn(reply.value()), u"say how its task went", m_where.user, m_secret.view());
        if (!said || !said.value().isObject()) {
            done(std::nullopt); // (the token may not, one of its moments: no verdict either way)
            return;
        }
        const QJsonObject task = said.value().toObject();
        if (task.value("status"_L1) == QJsonValue(QString(taskRuns))) {
            done(QString(taskRuns));
            return;
        }
        const QString ended = task.value("status"_L1) == QJsonValue(u"stopped"_s)
                                  ? kvmd::plain(task.value("exitstatus"_L1), 200)
                                  : QString();
        if (ended.isEmpty()) {
            done(std::nullopt);
            return;
        }
        // (Ended well with warnings is done, as Proxmox's own page counts it.)
        static const QRegularExpression warned(u"\\AWARNINGS: [0-9]{1,9}\\z"_s);
        done(warned.match(ended).hasMatch() ? QString(taskOk) : ended);
    });
}

void ProxmoxDevice::pressed(const std::shared_ptr<Pressing>& pressing, const base::Result<Done>& result)
{
    m_pressing.removeOne(pressing);
    // Before the command went (its state could not be read, or judged): nothing of it was done.
    base::Result<Done> said = result;
    if (!result && !pressing->sent) {
        said = nothingSent(result.error());
    }
    // Handed on the program's loop, and not this driver's: one let go of in this very turn hands it all the same.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    QTimer::singleShot(0, QCoreApplication::instance(), [pressing, said] {
        if (const auto done = std::exchange(pressing->done, nullptr)) {
            done(said);
        }
    });
}

void ProxmoxDevice::followUp()
{
    // What its card says is out of date now; it is looked at again at these moments, while the guest gets round to it.
    m_statusAt = std::numeric_limits<qint64>::min() / 2;
    m_followUpAt.clear();
    for (const int after : std::as_const(m_pace.followUpMs)) {
        m_followUpAt.append(now() + after);
    }
}

void ProxmoxDevice::askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done)
{
    // (Proxmox keeps no list of computers to wake: none.)
    later([done = std::move(done)] { done(QList<WakeTarget>()); });
}

// (Its console is given back by its screen when nobody looked for a while: RfbScreen::Limits::idleMs.)
void ProxmoxDevice::tidy() {}

void ProxmoxDevice::stopTyping()
{
    // (Its console's, where it has one: OwnDevice::stopTyping.)
    if (m_screen != nullptr) {
        m_screen->stopTyping();
    }
}

} // namespace qkvm::core
