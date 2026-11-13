#include <qkvm/core/redfish_device.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <utility>

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QtLogging>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/redfish_answers.h>
#include <qkvm/core/setup_overrides.h>
#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The most of an answer a controller may send (the web's MAX_REPLY).
constexpr qsizetype largestReplyBytes = qsizetype{2} * 1024 * 1024;
// Where the standard puts signing in, and the root of every place a controller has.
constexpr QLatin1StringView sessions{"/redfish/v1/SessionService/Sessions"};
constexpr QLatin1StringView root{"/redfish/v1/"};
// A computer names the controller that manages it among the first few it lists (the web's).
constexpr qsizetype mostLinksLookedAt = 8;

// The controller's words where they are the web's (redfish.py, kvm.py).
base::Error passwordRefused()
{
    return {"auth_failed", "Login rejected - check the username and password"};
}
base::Error keepsRefusingTheSession()
{
    return {"auth_failed", "The controller keeps rejecting the session"};
}
// A resource this account may not read, under a session the controller honours (not the web's words: the web
// signs in again for it).
base::Error mayNotRead()
{
    return {"auth_failed", "The controller does not let this account read that"};
}
// A command that never went out, in words that say so (and not as one that may have been done); another server at its
// address said as that.
// A command that never went out, in words that say so, where what came of it would read as "it may have been done";
// what says already that nothing was done keeps its own words, and its kind (another server, a refused password).
base::Error nothingSent(const base::Error& why, const char* because)
{
    if (!RedfishDevice::mayHaveBeenDone(why)) {
        return why;
    }
    return {"action_failed", std::string("Nothing was sent: ") + because + " (" + why.message + ")"};
}
base::Error noPasswordSaved()
{
    return {"no_password", "No password saved for this device"};
}
// What stands against a controller that is there and does not sign this program in, for another reason
// than the password: as a kvmd's (kvmd_device.cpp), never a question that may have been done.
base::Error notSigningIn(const std::string& why)
{
    return {"no_sign_in", "The device does not sign qKVM in (" + why + "). It is asked again shortly."};
}
base::Error listsNoComputer()
{
    return {"bad_response", "The controller lists no computer"};
}
// The first place among the first few of a list the controller gives (the web's: a computer names its chassis
// and its controller among them), that is a place of its own; none where none is.
QString firstNamed(const QJsonValue& list)
{
    const QJsonArray named = list.isArray() ? list.toArray() : QJsonArray();
    for (qsizetype index = 0; index < named.size() && index < mostLinksLookedAt; ++index) {
        if (const QString place = redfish::link(named.at(index).toObject().value("@odata.id"_L1)); !place.isEmpty()) {
            return place;
        }
    }
    return {};
}
// The sessions signed in right after the controller's number was compared, kept (CONTROLLER_SESSIONS): a read
// under the one before may come back after a new one was signed in, and is no less this controller's.
constexpr qsizetype mostComparedSessions = 4;
// Times a command asks again which server answers, where the controller signed this program in anew since it last
// did (MOST_ASKED).
constexpr int mostAsked = 3;
void keepCompared(QList<QByteArray>& kept, const QByteArray& token)
{
    kept.append(token);
    while (kept.size() > mostComparedSessions) {
        kept.removeFirst();
    }
}

// Failures that mean "the controller is not there, or is not the controller", and not "it said no".
bool meansAway(const std::string& code)
{
    return code == "unreachable" || code == "timeout" || code == "dropped" || code == "cert_mismatch" ||
           code == "cert_invalid";
}

// Where a session a sign-in made is given back: one name directly under the sessions collection the sign-in
// went to (a DELETE anywhere else could end something else: "/redfish/v1/AccountService/Accounts/2" is an
// account). As its Location says it (the path alone, whatever host it put before it), else as its own answer
// says it ("@odata.id"); none otherwise.
QString placeOf(const QByteArray& location, const QByteArray& body)
{
    const auto oneUnderSessions = [](const QString& said) {
        const QString place = redfish::link(said);
        const QString under = QString(sessions) + u'/';
        if (!place.startsWith(under)) {
            return QString();
        }
        QString name = place.sliced(under.size());
        if (name.endsWith(u'/')) {
            name.chop(1);
        }
        return name.isEmpty() || name.contains(u'/') ? QString() : place;
    };
    const QUrl said(QString::fromLatin1(location), QUrl::StrictMode);
    if (!location.isEmpty() && said.isValid()) {
        if (QString place = oneUnderSessions(said.path(QUrl::FullyEncoded)); !place.isEmpty()) {
            return place;
        }
    }
    return oneUnderSessions(QJsonDocument::fromJson(body).object().value("@odata.id"_L1).toString());
}

} // namespace

// --- which server answers: the words ------------------------------------------------------------------
// (The web's, a card where the web has a tile.)

base::Error RedfishDevice::anotherServer()
{
    return {"another_machine",
            "Another server answers at this address: it gives another number for itself (its UUID) than the server "
            "this card was added for. Nothing is done to it: no power command, no key, no console. If it is this "
            "card's server now, change the card and type its password."};
}
base::Error RedfishDevice::noServerNumber()
{
    return {"another_machine",
            "The server at this address does not say which it is (its UUID), and the server this card was added for "
            "did. Nothing is done to it: no power command, no key, no console. If it is this card's server now, change "
            "the card and type its password."};
}
base::Error RedfishDevice::anotherController()
{
    return {"another_machine",
            "Another server answers at this address: its controller gives another number for itself (the UUID of its "
            "Redfish service) than the one this card was added for, so it was not sent the password, and nothing is "
            "done to it. If it is this card's server now (another board, or its controller set back to its factory "
            "settings), change the card and type its password."};
}
base::Error RedfishDevice::noPicture()
{
    return {"no_picture", "A management controller has no picture of the screen"};
}
base::Error RedfishDevice::noControllerNumber()
{
    return {"another_machine",
            "The controller at this address does not say which it is (the UUID of its Redfish service), and the one "
            "this card was added for did, so it was not sent the password, and nothing is done to it. If it is this "
            "card's server now, change the card and type its password."};
}

// --- giving sessions back -----------------------------------------------------------------------------

namespace {

// (Nothing of the session in either: only that one went unsaid, or may.)
void sayLate()
{
    qWarning("qKVM: a server controller's session is given back as the program ends: it may never be");
}

} // namespace

ControllerGoodbyes::ControllerGoodbyes(QObject* parent) : QObject(parent), m_door(std::make_shared<Door>())
{
    m_door->m_goodbyes = this;
    m_clock.start();
}

// (A lock that cannot be had is the words of an error out of memory or the like: nothing of ours throws.)
// NOLINTNEXTLINE(bugprone-exception-escape)
ControllerGoodbyes::~ControllerGoodbyes()
{
    // Before anything of this is gone: no goodbye is handed in from another thread from now on, and one handed in
    // already is dropped with this (its event).
    const std::scoped_lock closing(m_door->m_lock);
    m_door->m_goodbyes = nullptr;
}

bool ControllerGoodbyes::Door::leaveWebPage(const DeviceAsker::Where& where, int connectWithinMs,
                                            const QByteArray& session, int withinMs)
{
    const std::scoped_lock held(m_lock);
    if (m_goodbyes == nullptr) {
        qWarning("qKVM: a server controller's web page session was not given back: the program had ended");
        return false;
    }
    if (m_finished) {
        sayLate(); // (its loop is ending: what is handed to it now may never run)
    }
    ControllerGoodbyes* goodbyes = m_goodbyes;
    QMetaObject::invokeMethod(
        goodbyes,
        [goodbyes, where, connectWithinMs, session, withinMs] {
            goodbyes->leave(where, connectWithinMs, session, withinMs);
        },
        Qt::QueuedConnection);
    return true;
}

bool ControllerGoodbyes::finished() const
{
    const std::scoped_lock held(m_door->m_lock);
    return m_door->m_finished;
}

ControllerGoodbyes& ControllerGoodbyes::program()
{
    // Made on first use, as a child of the program's application object, and gone with it: whatever is
    // still out then is dropped.
    static QPointer<ControllerGoodbyes> made;
    if (made == nullptr) {
        Q_ASSERT(QCoreApplication::instance() != nullptr);
        made = new ControllerGoodbyes(QCoreApplication::instance()); // NOLINT(cppcoreguidelines-owning-memory)
    }
    return *made;
}

void ControllerGoodbyes::giveBack(const DeviceAsker::Where& where, int connectWithinMs, const QString& place,
                                  const QByteArray& token, int withinMs)
{
    // (Not one handed in while the program's end waits for what is out: a clearing's, after it. That is waited for.)
    if (finished() && !m_waiting) {
        sayLate();
    }
    ++m_given;
    // Its own line, held to what its controller is known by; this one's, and gone at the answer or after
    // `withinMs` in all, whichever is first (and its question with it). The line's own limits are on each
    // piece of an answer, and a controller that dawdles, a byte at a time, keeps within each.
    auto* asker = new DeviceAsker(where, // NOLINT(cppcoreguidelines-owning-memory): this one's
                                  DeviceAsker::Times{std::min(connectWithinMs, withinMs), withinMs, withinMs},
                                  DeviceAsker::Carried{DeviceAsker::Carried::In::Header, "X-Auth-Token"_ba}, this);
    counted(asker);
    DeviceAsker::Ask goodbye;
    goodbye.method = "DELETE"_ba;
    goodbye.path = place;
    goodbye.largest = largestReplyBytes;
    asker->send(goodbye, token,
                [asker](const base::Result<DeviceAsker::Reply>& /*whatever*/) { asker->deleteLater(); });
    QTimer::singleShot(withinMs, asker, [asker] { asker->deleteLater(); });
}

void ControllerGoodbyes::leaveWebPage(const DeviceAsker::Where& where, int connectWithinMs, const QByteArray& session,
                                      int withinMs)
{
    if (finished()) {
        sayLate();
    }
    leave(where, connectWithinMs, session, withinMs);
}

void ControllerGoodbyes::leave(const DeviceAsker::Where& where, int connectWithinMs, const QByteArray& session,
                               int withinMs)
{
    ++m_given;
    // (As giveBack: a line of its own, gone at the answer or after `withinMs` in all.)
    auto* asker =
        new DeviceAsker(where, // NOLINT(cppcoreguidelines-owning-memory): this one's
                        DeviceAsker::Times{std::min(connectWithinMs, withinMs), withinMs, withinMs},
                        DeviceAsker::Carried{DeviceAsker::Carried::In::CookiesAndHeader, "X-CSRFTOKEN"_ba}, this);
    counted(asker);
    DeviceAsker::Ask goodbye;
    goodbye.method = "DELETE"_ba;
    goodbye.path = u"/api/session"_s;
    goodbye.largest = largestReplyBytes;
    asker->send(goodbye, session,
                [asker](const base::Result<DeviceAsker::Reply>& /*whatever*/) { asker->deleteLater(); });
    QTimer::singleShot(withinMs, asker, [asker] { asker->deleteLater(); });
}

void ControllerGoodbyes::waitFor(DeviceAsker* line)
{
    // (Its asking ends within the sign-in's own time, the line's limits: then it is let go of by whoever
    // made it, and with it what this counts.)
    line->setParent(this);
    counted(line);
}

void ControllerGoodbyes::counted(QObject* line)
{
    ++m_underWay;
    connect(line, &QObject::destroyed, this, [this] {
        if (--m_underWay == 0) {
            emit allGone();
        }
    });
}

void ControllerGoodbyes::finish(int withinMs)
{
    // From now on a goodbye handed in says that it may never be said: noted first, under the door's lock, so that none
    // comes in after the last of what is drained here unwarned. (Noted after the drain, one handed in between was
    // queued, never warned of, and never said.)
    {
        const std::scoped_lock held(m_door->m_lock);
        m_door->m_finished = true;
    }
    // What came through the door before this (a console's screen closed on a thread of its own) is said first: queued
    // still, nothing of it was out, and this ended at once without it.
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    if (m_underWay > 0) {
        QEventLoop waiting;
        connect(this, &ControllerGoodbyes::allGone, &waiting, &QEventLoop::quit);
        // (A clearing out is given its own time, and the goodbye of its session after it: each is bounded by itself.)
        const qint64 longest = std::max<qint64>(withinMs, m_endsBy - m_clock.elapsed());
        QTimer::singleShot(std::chrono::milliseconds(longest), &waiting, &QEventLoop::quit);
        m_waiting = true;
        waiting.exec();
        m_waiting = false;
    }
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall); // (and what came in meanwhile, warned of as it came)
}

// --- what a start into BIOS setup may have left, cleared on the way out ----------------------------------

// One clearing under way: its line, its time, and what was seen.
struct ControllerGoodbyes::Clearing {
    SetupClearing what;
    DeviceAsker* line = nullptr; // these goodbyes', gone at its end
    QDeadlineTimer until;
    bool oursFirst = false; // a read before the clearing showed this program's own override set
    bool over = false;
};

void ControllerGoodbyes::clearSetupOverride(SetupClearing clearing)
{
    if (finished() && !m_waiting) {
        // (Its loop is ending: what is handed to it now may never run.)
        qWarning("qKVM: a server's BIOS setup boot override is cleared as the program ends: it may never be");
    }
    const int within = std::max(1, clearing.withinMs);
    // Its own line, held to what its controller is known by, under the session the driver had; gone at its end, or
    // after `within` in all, whichever is first (and whatever of it is out with it).
    auto* line = new DeviceAsker(clearing.where, // NOLINT(cppcoreguidelines-owning-memory): this one's
                                 DeviceAsker::Times{std::min(clearing.connectWithinMs, within), within, within},
                                 DeviceAsker::Carried{DeviceAsker::Carried::In::Header, "X-Auth-Token"_ba}, this);
    counted(line);
    // (The program's end waits for it, and for the session's goodbye after it: finish.)
    m_endsBy = std::max(m_endsBy, m_clock.elapsed() + within + std::max(0, clearing.signOutWithinMs));
    auto state = std::make_shared<Clearing>();
    state->what = std::move(clearing);
    state->line = line;
    state->until = QDeadlineTimer(within);
    QTimer::singleShot(within, line, [this, state] { clearingOver(state, false); });
    readBeforeClearing(state, false);
}

std::optional<QJsonObject> ControllerGoodbyes::systemIn(const Clearing& state,
                                                        const base::Result<DeviceAsker::Reply>& reply)
{
    if (!reply) {
        return std::nullopt;
    }
    const base::Result<QJsonObject> system =
        redfish::resourceFrom(state.what.system, reply.value().status, reply.value().body);
    if (!system) {
        return std::nullopt;
    }
    // Only the controller that made the session knows it; and where the driver knew the server by its computer's
    // number, that very number (anything else is another server's, and nothing is changed there).
    const QString kept = redfish::marksOf(state.what.machine).server;
    if (kept.startsWith(u"server:"_s) && kept != redfish::saidNone && redfish::serverNumber(system.value()) != kept) {
        return std::nullopt;
    }
    return system.value();
}

void ControllerGoodbyes::readBeforeClearing(const std::shared_ptr<Clearing>& state, bool meant)
{
    DeviceAsker::Ask read;
    read.path = state->what.system;
    read.largest = largestReplyBytes;
    read.sent = {{"Accept"_ba, "application/json"_ba}};
    read.headersAsSent = {"ETag"_ba};
    state->line->send(read, state->what.token, [this, state, meant](const base::Result<DeviceAsker::Reply>& reply) {
        if (state->over) {
            return;
        }
        const std::optional<QJsonObject> system = systemIn(*state, reply);
        if (!system) {
            clearingOver(state, false);
            return;
        }
        if (!redfish::anotherOverride(*system).isEmpty()) {
            // Another tool's override in its place: the BIOS setup one is not set, and theirs is left as it is.
            clearingOver(state, true);
            return;
        }
        state->oursFirst = state->oursFirst ||
                           redfish::bootOverride(*system) == std::pair<QString, QString>(u"once"_s, u"biossetup"_s);
        sendClearing(state, meant ? "*"_ba : redfish::markOf(reply.value().headersAsSent.value("etag"), *system),
                     meant);
    });
}

void ControllerGoodbyes::sendClearing(const std::shared_ptr<Clearing>& state, const QByteArray& mark, bool meant)
{
    DeviceAsker::Ask change;
    change.method = "PATCH"_ba;
    change.path = state->what.system;
    change.body = QJsonDocument(QJsonObject{{u"Boot"_s, QJsonObject{{u"BootSourceOverrideEnabled"_s, u"Disabled"_s}}}})
                      .toJson(QJsonDocument::Compact);
    change.bodyIs = "application/json"_ba;
    change.doesSomething = true;
    change.largest = largestReplyBytes;
    change.sent = {{"Accept"_ba, "application/json"_ba}};
    if (!mark.isEmpty()) {
        change.sent.append({"If-Match"_ba, mark});
    }
    state->line->send(change, state->what.token, [this, state, meant](const base::Result<DeviceAsker::Reply>& reply) {
        if (state->over) {
            return;
        }
        if (!reply) {
            clearingOver(state, false);
            return;
        }
        const int status = reply.value().status;
        if ((status == 412 || status == 428) && !meant) {
            // (The state changed since it was read, or the controller wants a mark it did not give: read and looked at
            // again, then sent as meant whatever the state.)
            readBeforeClearing(state, true);
            return;
        }
        if (status == 200 || status == 202 || status == 204) {
            readClearingBack(state);
            return;
        }
        clearingOver(state, false); // (refused, or an error of its own: not cleared)
    });
}

void ControllerGoodbyes::readClearingBack(const std::shared_ptr<Clearing>& state)
{
    DeviceAsker::Ask read;
    read.path = state->what.system;
    read.largest = largestReplyBytes;
    read.sent = {{"Accept"_ba, "application/json"_ba}};
    state->line->send(read, state->what.token, [this, state](const base::Result<DeviceAsker::Reply>& reply) {
        if (state->over) {
            return;
        }
        if (const std::optional<QJsonObject> system = systemIn(*state, reply)) {
            // Off only where it says "Disabled", or shows another tool's in its place (review 4's MEDIUM 2).
            if (redfish::overrideOff(*system) == std::optional<bool>(true) ||
                !redfish::anotherOverride(*system).isEmpty()) {
                clearingOver(state, true);
                return;
            }
        }
        // (Not shown off, or not read: read again within its time.)
        const int again = std::max(1, state->what.againEveryMs);
        if (state->until.remainingTime() <= again) {
            clearingOver(state, false);
            return;
        }
        QTimer::singleShot(again, state->line, [this, state] { readClearingBack(state); });
    });
}

void ControllerGoodbyes::clearingOver(const std::shared_ptr<Clearing>& state, bool off)
{
    if (std::exchange(state->over, true)) {
        return;
    }
    if (const auto then = std::exchange(state->what.then, nullptr)) {
        then(off, state->oursFirst);
    }
    // Then its session, where it named a place of its own (before the line is let go of: what is counted goes on).
    if (!state->what.sessionPlace.isEmpty()) {
        giveBack(state->what.where, state->what.connectWithinMs, state->what.sessionPlace, state->what.token,
                 state->what.signOutWithinMs);
    }
    state->line->deleteLater();
}

// --- the driver ---------------------------------------------------------------------------------------

RedfishDevice::Ask::Ask()
{
    largest = largestReplyBytes;
    // (As the web asks every question under a session.)
    sent.append({"Accept"_ba, "application/json"_ba});
}

// A sign-in that is out, on a line of its own: what is needed to tell what it came to, by this driver or,
// where the driver was let go of meanwhile, by the goodbyes that took its line over.
struct RedfishDevice::SignInOut {
    QPointer<RedfishDevice> driver; // none: let go of while this was out
    QPointer<DeviceAsker> line;
    std::shared_ptr<SignInPacing> pacing; // none: the program's
    SignInPacing::Mark mark;
    SignInPacing::Steps steps;
    SignInPacing::Sent sent;
    bool compared = false; // the controller's number was compared just before it (sameController)
    QJsonObject root;      // its root, read on this line just before it (sameController)
    // Where a session it brings after its driver went is given back.
    DeviceAsker::Where where;
    int connectWithinMs = 0;
    int signOutWithinMs = 0;
};

// One press of a button, from the computer read and judged to what came of it.
struct RedfishDevice::Pressing {
    QString kind;              // the action: "power_on", "atx_reset", "reboot", ...
    bool ofTheComputer = true; // else of the controller itself (its restart)
    bool sent = false;         // the command went (or may have gone) out
    QString place;             // the computer's, where it is confirmed
    QString sentKind;          // the kind of reset sent
    std::function<void(base::Result<Done>)> done;
};

void RedfishDevice::sentFor(const std::shared_ptr<Pressing>& pressing)
{
    pressing->sent = true;
}

// One asking of what it is, from its root to its controller.
struct RedfishDevice::Identifying {
    std::function<void(base::Result<Identity>)> done;
    Identity found;
};

RedfishDevice::RedfishDevice(Where where, QString password, Pace pace, QObject* parent)
    : OwnDevice(parent), m_where(std::move(where)), m_password(SecretText::takenFrom(password)),
      m_pace(std::move(pace)),
      m_asker(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
              DeviceAsker::Times{m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs},
              DeviceAsker::Carried{DeviceAsker::Carried::In::Header, "X-Auth-Token"_ba})
{
    m_clock.start();
    m_machine = m_where.machine;
    // The controller showed the certificate it is known by: said as every driver says it (own_device.h).
    connect(&m_asker, &DeviceAsker::showedItsCertificate, this, &RedfishDevice::showed);
}

// (What tidy follows to, here and in the two below, is the words of an answer out of memory: nothing of ours throws,
// DESIGN.md section 7.)
// NOLINTNEXTLINE(bugprone-exception-escape)
RedfishDevice::~RedfishDevice()
{
    // Its screen first: its console says goodbye on a line the program keeps until it has gone (its web page's session
    // given back from there, ServerScreen::WayIn::done), and nothing of that is told to this driver.
    if (m_screen != nullptr) {
        m_screen->disconnect(this);
        m_screen.reset();
    }
    letGoOfTheWays();
    // (Nothing that must finish is waited for here: whoever reads the log learns what may be left on the server, and
    // the goodbyes clear it on the way out, bounded, noted beside the settings until that is seen: review 4's
    // MEDIUM 1.)
    if (anOverrideMayBeSet()) {
        qWarning("qKVM: a server was let go of while it started into its BIOS setup: its one-time boot override may "
                 "be set for the next boot, and is cleared on the way out");
    }
    const bool clearing = handTheSetupsOver();
    // A press under way is told what became of it: where it went out (or may have), whether it was done was not
    // seen, which is "it may have been done"; where it did not, nothing was done.
    for (const std::shared_ptr<Pressing>& pressing : std::as_const(m_pressing)) {
        if (const auto done = std::exchange(pressing->done, nullptr)) {
            const base::Error why =
                pressing->sent ? base::Error{"timeout", "The connection to the controller was let go of before "
                                                        "it was seen whether this was done: it may have been "
                                                        "done. Look at the server before pressing again."}
                               : base::Error{"stopped", "It was not done: the connection to the controller was "
                                                        "let go of before it was sent."};
            // NOLINTNEXTLINE(bugprone-exception-escape)
            QTimer::singleShot(0, QCoreApplication::instance(), [done, why] { done(why); });
        }
    }
    ControllerGoodbyes& goodbyes = m_pace.goodbyes ? *m_pace.goodbyes : ControllerGoodbyes::program();
    // A sign-in that is out goes on without this driver: its line is the goodbyes' from now on, and what it
    // comes to is told and given back there (cameAfterwards). Dropped, a session the controller made for it
    // would use one of its few up for half an hour.
    if (m_signInOut && m_signInOut->line != nullptr) {
        goodbyes.waitFor(m_signInOut->line);
    }
    // A controller has only so many sessions: this one's is given back, by a goodbye that outlives this
    // driver and that nobody waits for. Not where it had none, or none it named a place of its own for; nor
    // where the controller was away the last time it was asked (the web's close). (After the clearing, where one
    // goes: it goes under this session.)
    if (clearing || m_downError) {
        return;
    }
    giveBack(m_session);
}

void RedfishDevice::giveBack(const Session& session)
{
    if (session.token.isEmpty() || session.place.isEmpty()) {
        return; // (none had, or none it named a place of its own for)
    }
    ControllerGoodbyes& goodbyes = m_pace.goodbyes ? *m_pace.goodbyes : ControllerGoodbyes::program();
    goodbyes.giveBack(DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
                      m_pace.connectWithinMs, session.place, session.token, m_pace.signOutWithinMs);
}

void RedfishDevice::showed(const QByteArray& der)
{
    if (!std::exchange(m_toldItsCertificate, true)) {
        emit showedItsCertificate(der);
    }
}

void RedfishDevice::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

void RedfishDevice::tryAfresh(const Where& where, const QString& password, const Pace& pace)
{
    (pace.pacing ? *pace.pacing : SignInPacing::program()).forget(where.address, where.user, password);
}

bool RedfishDevice::mayHaveBeenDone(const base::Error& error)
{
    // It went out, and what came of it is not known: a controller that took it and did not answer in time,
    // or whose line fell under it (the web's server counts both so). ("unreachable" is before anything went
    // out, and a certificate that is not the controller's is before that.)
    return error.code == "timeout" || error.code == "dropped";
}

// --- signing in ---------------------------------------------------------------------------------------

SignInPacing& RedfishDevice::pacing() const
{
    return m_pace.pacing ? *m_pace.pacing : SignInPacing::program();
}

SignInPacing::Mark RedfishDevice::offered() const
{
    return SignInPacing::what(u"login", m_where.address, m_where.user, m_password.view());
}

SignInPacing::Mark RedfishDevice::sessionsGiven() const
{
    return SignInPacing::what(u"session", m_where.address, m_where.user, m_password.view());
}

SignInPacing::Steps RedfishDevice::steps() const
{
    // (A sign-in, here or to its web page, is out at most its line's whole: to be let in, the sign-in's own wait for an
    // answer, the rest of it.)
    return SignInPacing::Steps{m_pace.refusedHoldMs, m_pace.signInRetryMs,
                               SignInPacing::outFor(m_pace.connectWithinMs, m_pace.signInQuietMs, m_pace.transferMs)};
}

std::optional<base::Error> RedfishDevice::heldSignIn() const
{
    const SignInPacing::Mark mark = offered();
    if (pacing().waitLeftMs(mark) <= 0) {
        return std::nullopt;
    }
    if (pacing().times(mark) > 0) {
        return passwordRefused();
    }
    // In its own words while the hold it noted itself is the one that stands; otherwise as whichever driver
    // of this controller noted it, as the web says it (`_not_yet`).
    if (m_signInError && m_heldAs && pacing().standing(mark) == m_heldAs) {
        return m_signInError;
    }
    return notSigningIn("a sign-in to it is under way, or the last one came to nothing");
}

std::optional<base::Error> RedfishDevice::heldSessions() const
{
    // A controller that keeps refusing the sessions it gives: not signed in to again before its time, by this driver
    // nor by one made anew. In its own words while the hold it noted itself stands.
    const SignInPacing::Mark refused = sessionsGiven();
    if (pacing().waitLeftMs(refused) <= 0) {
        return std::nullopt;
    }
    if (m_sessionsHeldAs && pacing().standing(refused) == m_sessionsHeldAs) {
        return keepsRefusingTheSession();
    }
    return notSigningIn("a sign-in to it is under way, or the last one came to nothing");
}

void RedfishDevice::heldBackAfter(const SignInPacing::Mark& mark, const base::Error& why)
{
    // (The web's `_held_back`; the pacing was told by cameOf.)
    m_signInError = pacing().times(mark) > 0 ? passwordRefused() : notSigningIn(why.message);
    m_heldAs = pacing().standing(mark);
}

void RedfishDevice::withToken(std::function<void(base::Result<QByteArray>)> done)
{
    if (!m_session.token.isEmpty()) {
        later([done = std::move(done), token = m_session.token] { done(token); });
        return;
    }
    if (m_password.isEmpty()) {
        // Nothing is sent: a controller is asked nothing without a session.
        later([done = std::move(done)] { done(noPasswordSaved()); });
        return;
    }
    // Whoever asks while somebody is being signed in waits for that one sign-in.
    if (m_signingIn) {
        m_waitingForToken.append(std::move(done));
        return;
    }
    // What any driver of this controller noted: a password that was refused is not offered again before its
    // time.
    if (std::optional<base::Error> held = heldSignIn()) {
        if (mayWaitForTheOther()) {
            // Another driver's sign-in with it is out (the wall's, while a box signs in): what it comes to is waited
            // for, as the web's second asker waits for the one sign-in, and not said as a failure.
            m_waitingForToken.append(std::move(done));
            m_signingIn = true;
            afterTheOtherSignIn();
            return;
        }
        later([done = std::move(done), held = std::move(*held)] { done(held); });
        return;
    }
    if (std::optional<base::Error> held = heldSessions()) {
        later([done = std::move(done), held = std::move(*held)] { done(held); });
        return;
    }
    m_waitingForToken.append(std::move(done));
    signIn();
}

bool RedfishDevice::mayWaitForTheOther() const
{
    // (As a kvmd's driver: again where, after a yes, another that waited went first. Each one it waits for ends with
    // its line: a time from the first wait cut the third of three off on the slow board, whose root is 12 to 20 s a
    // read.)
    return pacing().onlyOut(offered());
}

void RedfishDevice::afterTheOtherSignIn()
{
    // (As a kvmd's driver: then looked at again, as the other's line's end tells.)
    emit waitsForAnotherSignIn();
    const SignInPacing::Mark mark = offered();
    pacing().whenSettled(mark, this, pacing().waitLeftMs(mark) + 5, [this] {
        std::optional<base::Error> held = heldSignIn();
        if (held && mayWaitForTheOther()) {
            afterTheOtherSignIn();
            return;
        }
        if (!held) {
            held = heldSessions();
        }
        if (held) {
            signedIn(*held);
            return;
        }
        signIn();
    });
}

void RedfishDevice::signIn()
{
    m_signingIn = true;
    // A line of its own, which can outlive this driver (~RedfishDevice), held to the same certificate: for the root
    // that is compared and for the sign-in both, so that the line kept for the one carries the other. Two
    // controllers at one address (an old static one, a clash of leases) that show one certificate are then not
    // asked one each, the root of one and the password of the other. (Where the controller closes the line after
    // the root, the sign-in goes on a new one: as the web's goes.)
    const DeviceAsker::Where where{m_where.address, m_where.knownByCertificate, m_where.certificate};
    auto* line = new DeviceAsker(where, // NOLINT(cppcoreguidelines-owning-memory): this driver's, or the goodbyes'
                                 DeviceAsker::Times{m_pace.connectWithinMs, m_pace.quietMs, m_pace.transferMs},
                                 DeviceAsker::Carried{DeviceAsker::Carried::In::Header, "X-Auth-Token"_ba}, this);
    connect(line, &DeviceAsker::showedItsCertificate, this, &RedfishDevice::showed);
    sameController(*line, [this, line](const base::Result<bool>& compared) {
        if (!compared) {
            // (Nothing was sent: nothing of the pacing is touched.)
            line->deleteLater();
            signedIn(compared.error());
            return;
        }
        // Looked at again just before it is counted: the root is a round trip, and another driver of this controller
        // may have sent the same password meanwhile (two failures at once, with a wrong one), or have it out still.
        std::optional<base::Error> held = heldSignIn();
        if (held && mayWaitForTheOther()) {
            line->deleteLater();
            afterTheOtherSignIn();
            return;
        }
        if (!held) {
            held = heldSessions();
        }
        if (held) {
            line->deleteLater();
            signedIn(*held);
            return;
        }
        sendSignIn(line, compared.value());
    });
}

void RedfishDevice::askRoot(DeviceAsker& line, bool vital, std::function<void(base::Result<QJsonObject>)> done)
{
    // One board answered it in 12 to 20.5 seconds (ROOT_WAIT_TIMES). (A line the controller closed while it sat
    // unused: Qt asks a GET once more itself.)
    DeviceAsker::Ask rootAsked;
    rootAsked.path = QString(root);
    rootAsked.largest = largestReplyBytes;
    rootAsked.sent = {{"Accept"_ba, "application/json"_ba}};
    rootAsked.quietMs = m_pace.rootUnsignedMs;
    line.send(rootAsked, {}, [this, vital, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(vital ? failed(reply.error()) : reply.error());
            return;
        }
        answered();
        const QJsonDocument said = QJsonDocument::fromJson(reply.value().body);
        if (reply.value().status != 200 || !said.isObject() || !said.object().contains("RedfishVersion"_L1)) {
            // (Not a service root at all just now: one of its moments. Nothing is judged by that, and nothing sent.)
            done(base::Error{"bad_response",
                             u"The controller did not show its service root (HTTP %1), which is read before its "
                             u"password goes"_s.arg(reply.value().status)
                                 .toStdString()});
            return;
        }
        done(said.object());
    });
}

void RedfishDevice::sameController(DeviceAsker& line, std::function<void(base::Result<bool>)> done)
{
    // Read before every sign-in, also where no number is kept to compare it with: a password goes only on a line shown
    // to be up (as a kvmd's and a JetKVM's, DeviceAsker::whenUp), and this is the question that carries nothing.
    // Where no number is kept, whatever answers is let through: the password is held to this controller by its
    // certificate alone then, on this road and on its web page's (the console's), as on the web, which has the same
    // gap (SPEC section 8).
    const QString remembered = redfish::marksOf(m_machine).controller;
    askRoot(line, true, [this, remembered, done = std::move(done)](const base::Result<QJsonObject>& rootSaid) {
        m_rootBeforeSignIn = rootSaid ? rootSaid.value() : QJsonObject();
        if (remembered.isEmpty()) {
            // (Nothing to compare: it answered, whatever it said, or it is not there.)
            done(rootSaid || rootSaid.error().code == "bad_response" ? base::Result<bool>(false)
                                                                     : base::Result<bool>(rootSaid.error()));
            return;
        }
        if (!rootSaid) {
            done(rootSaid.error());
            return;
        }
        const QString seen = redfish::controllerNumber(rootSaid.value());
        if (seen != remembered) {
            done(another(seen.isEmpty() ? noControllerNumber() : anotherController()));
            return;
        }
        done(true);
    });
}

void RedfishDevice::sendSignIn(DeviceAsker* line, bool compared)
{
    DeviceAsker::Ask login;
    login.method = "POST"_ba;
    login.path = QString(sessions);
    // (Made once, of the password as it is held, in the order QJsonObject wrote it: nothing of it in a JSON document
    // made on the way. F5. And no copy of it held but by what goes, which writes it over once its answer came
    // (DeviceAsker; from the merged review, 3): this one is written over here where nothing went.)
    login.body = secretJson({{"Password", m_password.view()}, {"UserName", m_where.user}});
    const WipedOnLeaving overTheBody(login.body);
    login.bodyIs = "application/json"_ba;
    login.largest = largestReplyBytes;
    // (It may have gone out, and been counted by the controller, before Qt says it went: never "not there" on a guess.)
    login.doesSomething = true;
    // (Both compared as they were sent: in small letters the token would be another.)
    login.headersAsSent = {"X-Auth-Token"_ba, "Location"_ba};
    // Waited for as every family's sign-in is (the web's WAITS x SIGN_IN_PATIENCE): some devices hold a
    // no back on purpose, and a no heard as none would have the password go out again at the next look.
    login.quietMs = m_pace.signInQuietMs;
    auto out = std::make_shared<SignInOut>();
    out->driver = this;
    out->line = line;
    out->pacing = m_pace.pacing;
    out->mark = offered();
    out->steps = steps();
    out->where = line->where();
    out->connectWithinMs = m_pace.connectWithinMs;
    out->signOutWithinMs = m_pace.signOutWithinMs;
    out->compared = compared;
    out->root = std::exchange(m_rootBeforeSignIn, {});
    // Counted as it goes out (SignInPacing::sent): a driver let go of while it is out leaves its mark, until
    // what it came to is told (cameAfterwards).
    out->sent = pacing().sent(out->mark, out->steps);
    m_signInOut = out;
    line->send(login, {}, [out](const base::Result<Reply>& reply) {
        if (out->line != nullptr) {
            out->line->deleteLater();
        }
        if (RedfishDevice* driver = out->driver) {
            driver->m_signInOut.reset();
            driver->signInCame(cameOf(*out, reply, false));
            return;
        }
        cameAfterwards(out, reply);
    });
}

RedfishDevice::SignInCame RedfishDevice::cameOf(const SignInOut& out, const base::Result<Reply>& reply, bool late)
{
    SignInPacing& pacing = out.pacing ? *out.pacing : SignInPacing::program();
    // (Late, nothing standing for the password: another driver was let in with it meanwhile, which a sign-in that
    // came to nothing, after its driver went, does not undo.)
    const bool noted = !late || pacing.standing(out.mark).has_value();
    SignInCame came;
    if (!reply) {
        // As a kvmd's: where it went out and nothing answered it, held back as one that came to neither yes
        // nor no; where nothing reached the controller, nothing is kept of it.
        came.why = reply.error();
        if (reply.error().code == "timeout" || reply.error().code == "dropped") {
            if (noted) {
                pacing.unanswered(out.mark, out.steps, out.sent);
            }
            came.kind = SignInCame::Kind::Neither;
        } else {
            pacing.putBack(out.mark, out.sent);
            came.kind = SignInCame::Kind::NotSent;
        }
        return came;
    }
    const int status = reply.value().status;
    if (status == 401 || status == 403) {
        // A controller locks an account after a few wrong passwords: the same one is not offered every few
        // seconds, and less often each time it says no (SignInPacing).
        pacing.saidNo(out.mark, out.steps);
        came.kind = SignInCame::Kind::Refused;
        return came;
    }
    const QByteArray token = redfish::token(reply.value().headersAsSent.value("x-auth-token"));
    if ((status != 200 && status != 201) || token.isEmpty()) {
        // Not the password's doing (every session it has is given out, or it is no controller at all). Not
        // sent again at every look meanwhile (the web's `_neither_yes_nor_no`).
        if (noted) {
            pacing.unanswered(out.mark, out.steps, out.sent);
        }
        came.kind = SignInCame::Kind::Neither;
        came.why = base::Error{"bad_response", u"Login failed (HTTP %1)"_s.arg(status).toStdString()};
        return came;
    }
    pacing.accepted(out.mark);
    came.kind = SignInCame::Kind::In;
    came.compared = out.compared;
    came.root = out.root;
    came.session = Session{token, placeOf(reply.value().headersAsSent.value("location"), reply.value().body)};
    return came;
}

void RedfishDevice::cameAfterwards(const std::shared_ptr<SignInOut>& out, const base::Result<Reply>& reply)
{
    // Told to the pacing as the driver would have told it, which outlives drivers; and a session it brought
    // given back, by the goodbyes that waited for it (its line's own now).
    const SignInCame came = cameOf(*out, reply, true);
    auto* goodbyes = out->line != nullptr ? qobject_cast<ControllerGoodbyes*>(out->line->parent()) : nullptr;
    if (came.kind == SignInCame::Kind::In && !came.session.place.isEmpty() && goodbyes != nullptr) {
        goodbyes->giveBack(out->where, out->connectWithinMs, came.session.place, came.session.token,
                           out->signOutWithinMs);
    }
}

void RedfishDevice::signInCame(const SignInCame& came)
{
    switch (came.kind) {
    case SignInCame::Kind::NotSent:
        signedIn(failed(came.why));
        return;
    case SignInCame::Kind::Neither:
        if (came.why.code != "timeout" && came.why.code != "dropped") {
            answered();
        }
        heldBackAfter(offered(), came.why);
        signedIn(*m_signInError);
        return;
    case SignInCame::Kind::Refused:
        answered();
        m_signInError = passwordRefused();
        m_heldAs = pacing().standing(offered());
        signedIn(*m_signInError);
        return;
    case SignInCame::Kind::In:
        answered();
        m_signInError.reset();
        m_heldAs.reset();
        m_session = came.session;
        // What was refused under the session before may have been that session ending (its time, somebody else's
        // sign-in on a board that keeps one session an account): asked again under this one.
        m_refusedPlaces.clear();
        if (came.compared) {
            // (A session of the controller whose number was just compared: see judge.)
            keepCompared(m_controllerTokens, came.session.token);
        }
        if (!came.root.isEmpty()) {
            m_rootOfSession = std::pair(came.root, came.session.token);
        }
        signedIn(came.session.token);
        return;
    }
}

void RedfishDevice::signedIn(const base::Result<QByteArray>& token)
{
    m_signingIn = false;
    for (const auto& waiting : std::exchange(m_waitingForToken, {})) {
        waiting(token);
    }
}

// --- asking -------------------------------------------------------------------------------------------

void RedfishDevice::request(const Ask& question, std::function<void(base::Result<Reply>)> done)
{
    request(question, std::move(done), Tries{});
}

void RedfishDevice::request(const Ask& question, std::function<void(base::Result<Reply>)> done, Tries tries)
{
    // A session signed in for this very question that the controller refuses at once did not run out: it is not
    // signed in to again for that question (two sign-ins a round, and where its sign-outs are refused too, a dozen
    // sessions lingering at once). One another question was signed in for is as any other.
    const bool ownSignIn = m_session.token.isEmpty() && !m_signingIn;
    withToken([this, question, tries, ownSignIn,
               done = std::move(done)](const base::Result<QByteArray>& token) mutable {
        if (!token) {
            // (A command whose sign-in came to nothing never went, or went and was refused: not "done", nor "may have
            // been done", whatever the sign-in's own root or line came to.)
            if (question.command && tries.refused && mayHaveBeenDone(token.error())) {
                done(base::Error{"action_failed",
                                 "The controller refused this under its session, and was not signed in to again (" +
                                     token.error().message + "): it was not done."});
                return;
            }
            done(question.command ? nothingSent(token.error(), "the controller did not sign this program in")
                                  : token.error());
            return;
        }
        if (ownSignIn) {
            tries.signedInAgain = true;
        }
        const QByteArray& used = token.value();
        if (question.command && !m_machine.isEmpty() && used != m_checkedToken) {
            // Signed in anew since the computer was found to be this one: which server answers is asked under this
            // session before the command goes (another server at the address does not know the one before).
            if (tries.asked >= mostAsked) {
                done(base::Error{"action_failed",
                                 "Nothing was sent: the controller at this address signed this program "
                                 "in anew while it was asked which server it is. Try again."});
                return;
            }
            const auto notSent = [](const base::Error& why) {
                return nothingSent(why, "which server answers could not be read");
            };
            find([this, question, tries, done, notSent](const base::Result<Places>& places) {
                if (!places) {
                    done(notSent(places.error()));
                    return;
                }
                checkedRead(places.value().system, true,
                            [this, question, tries, done, notSent](const base::Result<QJsonObject>& judged,
                                                                   const Checked& /*under*/) {
                                if (!judged) {
                                    done(notSent(judged.error()));
                                    return;
                                }
                                request(question, done, Tries{tries.signedInAgain, tries.asked + 1, tries.refused});
                            });
            });
            return;
        }
        m_asker.send(question, used, [this, question, tries, used, done = std::move(done)](base::Result<Reply> reply) {
            if (!reply) {
                done(question.vital ? failed(reply.error()) : reply.error());
                return;
            }
            answered();
            const int status = reply.value().status;
            if (status != 401) {
                // (Read by whoever is handed this, before anything else runs: checkedRead, controllerUnder.)
                m_answeredToken = used;
                done(std::move(reply));
                return;
            }
            if (question.detail && question.method == "GET"_ba && used == m_answeredToken) {
                // A resource this account may not read (an operator's, of an older board's sensors), under the session
                // the read before was answered under: every look and every identify begins with a read that must
                // be. The resource is refused, not the session: kept, and that place is not asked again until where
                // it keeps things is found anew. (Signed in twice at every look otherwise: some seven hundred
                // passwords an hour, the card turning from online to auth and back.)
                m_refusedPlaces.insert(question.path);
                done(mayNotRead());
                return;
            }
            if (!tries.signedInAgain) {
                // The session ran out, or the controller restarted: once more, signed in afresh. (Given back all
                // the same: one it still keeps is no session lost. One it forgot answers the goodbye 401.)
                if (m_session.token == used) {
                    giveBack(std::exchange(m_session, Session{}));
                }
                request(question, done, Tries{true, tries.asked, tries.refused || question.command});
                return;
            }
            if (question.detail) {
                // A detail refused under a session it has just given, and again under the next: the detail is refused
                // to the account (an operator's), as under a session it honoured. Only what is not a detail may say
                // that the session is.
                m_refusedPlaces.insert(question.path);
                done(mayNotRead());
                return;
            }
            // It signs this program in and refuses the session it has just given. Every look ends up here: the
            // password is not sent to it again with each of them, by this driver nor by one made anew (the hold is
            // the pacing's, which outlives drivers): ten seconds, twice as long each time it does it again, five
            // minutes at the most, until a session of it is honoured. (Counted as a sign-in that nothing answered
            // is, sent and no answer noted; nothing is out.)
            if (m_session.token == used) {
                giveBack(std::exchange(m_session, Session{}));
                const SignInPacing::Mark refused = sessionsGiven();
                pacing().cameToNothingAgain(refused, steps());
                m_sessionsHeldAs = pacing().standing(refused);
            }
            done(keepsRefusingTheSession());
        });
    });
}

void RedfishDevice::read(const QString& path, bool vital, std::function<void(base::Result<QJsonObject>)> done)
{
    read(path, vital, !vital, std::move(done));
}

void RedfishDevice::read(const QString& path, bool vital, bool detail,
                         std::function<void(base::Result<QJsonObject>)> done)
{
    if (detail && m_refusedPlaces.contains(path)) {
        // (Refused to this account: not asked again before where it keeps things is found anew.)
        later([done = std::move(done)] { done(mayNotRead()); });
        return;
    }
    Ask question;
    question.path = path;
    question.vital = vital;
    question.detail = detail;
    request(question, [path, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(reply.error());
            return;
        }
        done(redfish::resourceFrom(path, reply.value().status, reply.value().body));
    });
}

void RedfishDevice::firstOf(const QJsonValue& collection, bool vital, std::function<void(base::Result<QString>)> done)
{
    const QString place = redfish::link(collection.toObject().value("@odata.id"_L1));
    if (place.isEmpty()) {
        later([done = std::move(done)] { done(QString()); });
        return;
    }
    read(place, vital, [done = std::move(done)](const base::Result<QJsonObject>& listed) {
        if (!listed) {
            done(listed.error());
            return;
        }
        const QJsonValue members = listed.value().value("Members"_L1);
        // (The first member of a list that has one; a member that is no place of its own is none.)
        done(members.isArray() && !members.toArray().isEmpty()
                 ? redfish::link(members.toArray().first().toObject().value("@odata.id"_L1))
                 : QString());
    });
}

// --- what it is ---------------------------------------------------------------------------------------

void RedfishDevice::identify(std::function<void(base::Result<Identity>)> done)
{
    auto asking = std::make_shared<Identifying>();
    asking->done = std::move(done);
    // Where it keeps things, its computer read and judged (which server answers), and the controller that manages
    // it (the web's `identify` and `_read_about`).
    find([this, asking](const base::Result<Places>& places) {
        if (!places) {
            finish(asking, places.error());
            return;
        }
        checkedRead(
            places.value().system, true,
            [this, asking, manager = places.value().manager,
             update = places.value().update](const base::Result<QJsonObject>& computer, const Checked& checked) {
                if (!computer) {
                    finish(asking, computer.error());
                    return;
                }
                const QJsonObject& said = computer.value();
                asking->found.model = redfish::named({said.value("Model"_L1), said.value("Manufacturer"_L1)});
                asking->found.hostname = redfish::named({said.value("HostName"_L1)});
                if (const QString own = redfish::serverNumber(said); !own.isEmpty()) {
                    asking->found.number = own;
                } else if (const QString kept = redfish::marksOf(m_machine).server;
                           kept.isEmpty() || kept == redfish::saidNone) {
                    asking->found.number = QString();
                }
                // Its version is a detail: a controller that cannot be read just then is what it is all
                // the same, without that.
                if (manager.isEmpty()) {
                    finish(asking, asking->found);
                    return;
                }
                read(manager, false,
                     [this, asking, manager, update, judged = checked.token](const base::Result<QJsonObject>& itself) {
                         // (Under the session the server was judged under: one signed in anew meanwhile may be
                         // another controller's.)
                         if (!itself || m_answeredToken != judged) {
                             finish(asking, asking->found);
                             return;
                         }
                         versionOf(
                             itself.value(), manager, update, [this, judged] { return m_answeredToken == judged; },
                             [this, asking](const QString& version) {
                                 asking->found.version = version;
                                 finish(asking, asking->found);
                             });
                     });
            });
    });
}

void RedfishDevice::finish(const std::shared_ptr<Identifying>& asking, const base::Result<Identity>& result)
{
    later([asking, handed = result] {
        if (const auto done = std::exchange(asking->done, nullptr)) {
            done(handed);
        }
    });
}

// --- where it keeps things ------------------------------------------------------------------------------

void RedfishDevice::find(std::function<void(base::Result<Places>)> done)
{
    if (m_places && now() - m_placesAt < m_pace.placesFreshMs) {
        later([done = std::move(done), places = *m_places] { done(places); });
        return;
    }
    // (What it refused this account is asked again, as everything is.)
    m_refusedPlaces.clear();
    read(QString(root), true, [this, done = std::move(done)](const base::Result<QJsonObject>& itsRoot) {
        if (!itsRoot) {
            done(itsRoot.error());
            return;
        }
        firstOf(itsRoot.value().value("Systems"_L1), true,
                [this, done, rootSaid = itsRoot.value()](const base::Result<QString>& system) {
                    if (!system) {
                        done(system.error());
                        return;
                    }
                    if (system.value().isEmpty()) {
                        done(listsNoComputer());
                        return;
                    }
                    placesOf(rootSaid, system.value(), done);
                });
    });
}

void RedfishDevice::placesOf(const QJsonObject& rootSaid, const QString& system,
                             std::function<void(base::Result<Places>)> done)
{
    read(system, true, [this, rootSaid, system, done = std::move(done)](const base::Result<QJsonObject>& computer) {
        if (!computer) {
            done(computer.error());
            return;
        }
        // The chassis and the controller the computer names as its own, taken first (an iDRAC lists every disk
        // enclosure as a chassis too); the first the root lists only where it names none of its own, and none
        // where that cannot be read.
        auto places = std::make_shared<Places>();
        places->root = rootSaid;
        places->system = system;
        places->update = redfish::link(rootSaid.value("UpdateService"_L1).toObject().value("@odata.id"_L1));
        const QJsonObject links = computer.value().value("Links"_L1).toObject();
        places->chassis = firstNamed(links.value("Chassis"_L1));
        places->manager = firstNamed(links.value("ManagedBy"_L1));
        const auto over = [this, places, done] {
            sensorPlaces(places, [this, places, done] {
                m_places = *places;
                m_placesAt = now();
                done(*places);
            });
        };
        const auto manager = [this, places, over] {
            if (!places->manager.isEmpty()) {
                over();
                return;
            }
            firstOf(places->root.value("Managers"_L1), false, [places, over](const base::Result<QString>& found) {
                places->manager = found ? found.value() : QString();
                over();
            });
        };
        if (!places->chassis.isEmpty()) {
            manager();
            return;
        }
        firstOf(rootSaid.value("Chassis"_L1), false, [places, manager](const base::Result<QString>& found) {
            places->chassis = found ? found.value() : QString();
            manager();
        });
    });
}

void RedfishDevice::sensorPlaces(const std::shared_ptr<Places>& places, std::function<void()> done)
{
    // Where the chassis keeps its sensors: the older shape (Thermal), else the newer (ThermalSubsystem's
    // ThermalMetrics and Fans). A chassis that cannot be read has none to show: the server is there all the same.
    if (places->chassis.isEmpty()) {
        later(std::move(done));
        return;
    }
    read(places->chassis, false, [this, places, done = std::move(done)](const base::Result<QJsonObject>& chassis) {
        if (!chassis) {
            done();
            return;
        }
        places->thermal = redfish::link(chassis.value().value("Thermal"_L1).toObject().value("@odata.id"_L1));
        const QString subsystem =
            redfish::link(chassis.value().value("ThermalSubsystem"_L1).toObject().value("@odata.id"_L1));
        if (!places->thermal.isEmpty() || subsystem.isEmpty()) {
            done();
            return;
        }
        read(subsystem, false, [places, done](const base::Result<QJsonObject>& parts) {
            if (parts) {
                places->metrics =
                    redfish::link(parts.value().value("ThermalMetrics"_L1).toObject().value("@odata.id"_L1));
                places->fans = redfish::link(parts.value().value("Fans"_L1).toObject().value("@odata.id"_L1));
            }
            done();
        });
    });
}

// --- which server answers -------------------------------------------------------------------------------
// A controller's maker may have put one certificate, and one key, on every board, and a fleet often has one
// password: after a new lease another server's controller answers at a card's address, takes the saved
// password, and would be switched off from this card. So a server is known by the number its computer gives
// for itself, asked under the session before anything is done to it, and by the number its controller's
// service root gives, which anybody may read, asked before the password goes (sameController).

void RedfishDevice::checkedRead(const QString& place, bool vital,
                                std::function<void(base::Result<QJsonObject>, Checked)> done)
{
    // (Its computer itself: never a detail the account may be refused, though the read that confirms a power command
    // is not vital. Its mark as it was sent.)
    Ask question;
    question.path = place;
    question.vital = vital;
    question.headersAsSent = {"ETag"_ba};
    request(question, [this, place, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(reply.error(), {});
            return;
        }
        const base::Result<QJsonObject> system = redfish::resourceFrom(place, reply.value().status, reply.value().body);
        if (!system) {
            done(system.error(), {});
            return;
        }
        // (The session of this very answer: nothing ran since it came. The judge may sign in anew before it is
        // over, to ask the controller's number: what is handed on is the session it judged.)
        const Checked checked{m_answeredToken,
                              redfish::markOf(reply.value().headersAsSent.value("etag"), system.value())};
        judge(redfish::serverNumber(system.value()), checked.token,
              [this, done, checked, said = system.value()](const base::Result<void>& judged) {
                  if (!judged) {
                      done(judged.error(), {});
                      return;
                  }
                  // A session of it honoured, its computer read and judged under it: what was held against its
                  // sessions is over. (Not by its root, which Redfish has it answer whatever the session, nor by a
                  // list: a controller that keeps an account from its computer answers both.)
                  pacing().accepted(sessionsGiven());
                  done(said, checked);
              });
    });
}

void RedfishDevice::judge(const QString& seen, const QByteArray& token, std::function<void(base::Result<void>)> done)
{
    const QString remembered = m_machine;
    const redfish::Marks marks = redfish::marksOf(remembered);
    if (remembered.isEmpty()) {
        // Nothing kept (a card from a file without its numbers, or one added while it could not be asked): the
        // numbers seen are taken from now on, as a certificate is at first use: the computer's, or saidNone where
        // it gives none, and the controller's as it says it under this same session.
        controllerUnder(token, [this, seen, token, done](const QString& rootSaid, bool asked) {
            if (!m_machine.isEmpty()) {
                judge(seen, token, done); // (taken meanwhile, by another read: judged by that)
                return;
            }
            if (seen.isEmpty() && !asked) {
                // (Its computer gives no number, and its controller's could not be asked just now: nothing is taken
                // on the strength of that, and it is said as one of its moments.)
                done(base::Error{"bad_response", "Which server answers here could not be told just now: its computer "
                                                 "gives no number, and its controller's could not be read"});
                return;
            }
            const QString taken = (seen.isEmpty() ? QString(redfish::saidNone) : seen) + rootSaid;
            if (taken == redfish::saidNone) {
                // No computer's number, and no controller's: nothing to know it by, and nothing taken. What is read
                // under this session is its own all the same (underJudged).
                m_checkedToken = token;
                done({});
                return;
            }
            m_machine = taken;
            m_checkedToken = token;
            if (!rootSaid.isEmpty() && !token.isEmpty()) {
                keepCompared(m_controllerTokens, token);
            }
            // (A turn of the loop later: whoever is told may let go of this driver there and then.)
            later([this, taken] { emit knownAs(QString(), taken); });
            done({});
        });
        return;
    }
    // Whether the controller's number was compared before this very session was signed in.
    const bool compared = !marks.controller.isEmpty() && !token.isEmpty() && m_controllerTokens.contains(token);
    std::optional<base::Error> otherwise;
    if (marks.server == redfish::saidNone && !marks.controller.isEmpty()) {
        // Its computer said none when it was added: known by its controller alone, whatever the computer says now.
        if (!compared) {
            otherwise = noControllerNumber();
        }
    } else if (seen.isEmpty()) {
        // One that says none where one was given is this one's only where the controller's number was compared
        // before this very session (a controller that learns it from the server's firmware may say none while
        // the server is off).
        if (!compared) {
            otherwise = noServerNumber();
        }
    } else if (seen != marks.server) {
        otherwise = anotherServer();
    }
    if (otherwise) {
        later([done = std::move(done), why = another(*otherwise)] { done(why); });
        return;
    }
    m_checkedToken = token;
    m_other.reset();
    if (marks.controller.isEmpty() && remembered == marks.server && marks.server != redfish::saidNone) {
        // Its computer's number alone was kept (a card from a file that kept no more): its controller's is taken the
        // first time the computer is found to be this one, under that same session.
        controllerUnder(token, [this, remembered, token, server = marks.server, done](const QString& rootSaid, bool) {
            if (!rootSaid.isEmpty() && m_machine == remembered) {
                m_machine = server + rootSaid;
                if (!token.isEmpty()) {
                    keepCompared(m_controllerTokens, token);
                }
                later([this, remembered, now = m_machine] { emit knownAs(remembered, now); });
            }
            done({});
        });
        return;
    }
    later([done = std::move(done)] { done({}); });
}

void RedfishDevice::controllerUnder(const QByteArray& token, std::function<void(const QString& found, bool asked)> done)
{
    // The root is open to anybody and says nothing of which controller made a session; the manager is read under
    // the session, and another controller does not know it (401). Taken only where the two name the same.
    if (m_rootSeen && !token.isEmpty() && m_rootSeen->second == token) {
        later([done = std::move(done), found = m_rootSeen->first] { done(found, true); });
        return;
    }
    // (From its root as anybody may read it, which is what it is compared with before the password goes: one
    // that shows its number only under a session would be known by a number it never shows there, and refused
    // for good. Unlike the web, which reads it under the session.)
    // (Read on the sign-in's own line just before the password went, where that brought this very session: not asked
    // a second time.)
    const auto withRoot = [this, token, done](const base::Result<QJsonObject>& rootSaid) {
        if (!rootSaid) {
            done(QString(), false);
            return;
        }
        find([this, token, done, rootSaid = rootSaid.value()](const base::Result<Places>& places) {
            const auto under = [this, token, done](const QString& found) {
                if (m_answeredToken != token) {
                    done(QString(), false); // (signed in anew meanwhile: not read under that session)
                    return;
                }
                m_rootSeen = std::pair(found, token);
                done(found, true);
            };
            if (!places) {
                done(QString(), false);
                return;
            }
            if (places.value().manager.isEmpty()) {
                under(QString());
                return;
            }
            read(places.value().manager, false, [rootSaid, under, done](const base::Result<QJsonObject>& manager) {
                if (!manager) {
                    done(QString(), false);
                    return;
                }
                const QString number = redfish::controllerNumber(rootSaid);
                under(!number.isEmpty() && redfish::managerNumbers(manager.value()).contains(number) ? number
                                                                                                     : QString());
            });
        });
    };
    if (m_rootOfSession && !token.isEmpty() && m_rootOfSession->second == token) {
        later([withRoot, root = m_rootOfSession->first] { withRoot(root); });
        return;
    }
    askRoot(m_asker, false, withRoot);
}

base::Error RedfishDevice::another(const base::Error& why)
{
    // What was read of it is another server's: nothing of it is taken for this one's, and no command goes under the
    // session it signed in (and where it keeps things is looked up anew).
    m_other = why;
    m_checkedToken.clear();
    m_places.reset();
    m_good.reset();
    return why;
}

// --- how it is ----------------------------------------------------------------------------------------

void RedfishDevice::answered()
{
    m_failures = 0;
    m_downError.reset();
    m_lastOkAt = QDateTime::currentSecsSinceEpoch();
    m_heardAt = now();
}

base::Error RedfishDevice::failed(const base::Error& error)
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

Status RedfishDevice::status() const
{
    Status status = m_status.value_or(Status{});
    status.hasPicture = false;
    // Its own screen, where its console can be had (the web's `screen`, `live`; and once the console is known,
    // `keys`, `text` and `control`), and why it is not shown where it could be and is not (the web's no_screen).
    status.hasScreen = hasScreen();
    if (m_screen != nullptr && !m_screen->picture().rgb.empty()) {
        status.width = m_screen->picture().width;
        status.height = m_screen->picture().height;
    }
    if (status.capabilities && status.hasScreen) {
        *status.capabilities << u"screen"_s << u"live"_s;
        if (m_hasConsole == std::optional<bool>(true)) {
            *status.capabilities << u"keys"_s << u"text"_s << u"control"_s;
        }
    }
    if (const std::optional<base::Error> why = noScreen(); why && status.server) {
        status.server->noScreen = QString::fromStdString(why->message);
    }
    // A start into BIOS setup that did not end may have left its override set: said until a look shows it off.
    if (status.server && overrides().standing(m_where.address, m_machine)) {
        status.server->setupLeft = SetupOverrides::onTheCard();
    }
    if (m_lastOkAt) {
        const qint64 ago = std::clamp<qint64>(QDateTime::currentSecsSinceEpoch() - *m_lastOkAt, 0, 2'000'000'000);
        status.lastSeenSecondsAgo = static_cast<int>(ago);
    }
    return status;
}

bool RedfishDevice::isStale() const
{
    // A server's state changes slowly, and its controller is a small computer: a look every so often; sooner only
    // after one that left the one before standing for one of its moments (the web's `_hiccup`). After another
    // server, a refusal, or a failure with nothing good standing: in its time. And at the moments after something
    // was done to it (FOLLOW_UP).
    if (!m_followUpAt.isEmpty() && now() >= m_followUpAt.first()) {
        return true;
    }
    return !m_status || now() - m_statusAt > (m_hiccup ? m_pace.retryLookMs : m_pace.freshMs);
}

std::optional<base::Error> RedfishDevice::heldBack() const
{
    if (m_downError && now() < m_downUntil) {
        return m_downError;
    }
    return std::nullopt;
}

// --- the look -------------------------------------------------------------------------------------------

void RedfishDevice::look()
{
    if (m_looking) {
        return;
    }
    m_looking = true;
    while (!m_followUpAt.isEmpty() && now() >= m_followUpAt.first()) {
        m_followUpAt.removeFirst();
    }
    if (std::optional<base::Error> held = heldBack()) {
        later([this, why = *held] { lookFailed(why); });
        return;
    }
    find([this](const base::Result<Places>& places) {
        if (!places) {
            lookFailed(places.error());
            return;
        }
        // Which server answers, at every look as before a button: what its card shows of a server is this one's.
        checkedRead(
            places.value().system, true,
            [this, places = places.value()](const base::Result<QJsonObject>& system, const Checked& /*checked*/) {
                if (!system) {
                    lookFailed(system.error());
                    return;
                }
                lookedAtTheOverride(system.value());
                sensors(places, [this, places, system = system.value()](const QList<redfish::Temperature>& temperatures,
                                                                        const QList<redfish::Fan>& fans) {
                    itself(places, [this, system, temperatures, fans](const QString& version) {
                        redfish::Looked looked{system, temperatures, fans, version, std::nullopt};
                        if (m_faultAt && now() - *m_faultAt < m_pace.faultHoldMs) {
                            looked.faultHeld = m_faultWhere;
                        }
                        if (m_other) {
                            // Another server was found at its address meanwhile (a sign-in made anew in the middle
                            // of this look): said as that, and not as what was read of this one before.
                            lookFailed(*m_other);
                            return;
                        }
                        const redfish::Seen seen = redfish::statusFrom(looked);
                        if (seen.fault) {
                            m_faultAt = now();
                            m_faultWhere = *seen.fault;
                        }
                        m_good = seen.status;
                        m_goodAt = now();
                        lookOver(seen.status, false);
                    });
                });
            });
    });
}

void RedfishDevice::lookFailed(const base::Error& why)
{
    if (why.code == "another_machine") {
        // Said at once, and not as one of its moments: nothing of the other server's is shown as this one's.
        Status other;
        other.state = u"error"_s;
        other.errorCode = QString::fromStdString(why.code);
        other.errorMessage = QString::fromStdString(why.message);
        lookOver(other, false);
        return;
    }
    if (why.code == "auth_failed" || why.code == "no_sign_in" || why.code == "bad_response") {
        // Nothing was asked that needs a sign-in, or what was asked said nothing: whether it is there at all is
        // asked apart. (Not where no password is saved, as the web does: a controller of this wall that has none
        // hears nothing at all.)
        stillThere(why);
        return;
    }
    lookFailedAs(why);
}

void RedfishDevice::lookFailedAs(const base::Error& why)
{
    Status status;
    if (why.code == "auth_failed" || why.code == "no_password") {
        status.state = u"auth"_s;
    } else if (why.code == "cert_mismatch" || why.code == "cert_invalid" || why.code == "bad_response" ||
               why.code == "no_sign_in" || why.code == tlsDamaged) {
        // (Something for whoever sits here to look at, and not a controller that is away.)
        status.state = u"error"_s;
    } else {
        status.state = u"offline"_s;
    }
    status.errorCode = QString::fromStdString(why.code);
    status.errorMessage = QString::fromStdString(why.message);
    // Not being let in, and not being who it was, are said at once. The rest may be one of its moments.
    const bool passing = status.state != u"auth"_s && why.code != "cert_mismatch" && why.code != "cert_invalid" &&
                         why.code != tlsDamaged;
    if (passing && m_good && now() - m_goodAt < m_pace.hiccupMs) {
        lookOver(*m_good, true);
        return;
    }
    lookOver(status, false);
}

void RedfishDevice::lookOver(Status status, bool hiccup)
{
    // (A look that left the last good one standing for one of its moments is followed by another sooner.)
    m_hiccup = hiccup;
    status.hasPicture = false;
    m_status = std::move(status);
    m_statusAt = now();
    m_looking = false;
    emit looked();
}

void RedfishDevice::stillThere(const base::Error& why)
{
    // (The web's `_gone_meanwhile`.) While a refused password waits to be offered again, up to an hour, nothing
    // that needs a sign-in is asked, and so nothing said that the controller went away. This asks what anybody may
    // ask (its root), with no password and no session; what it answers does not matter, only that it does. Not at
    // every look: one that answered of late (with the no, for one) is there. Then the look is said as `why`, or as
    // what found it gone.
    if (m_heardAt >= 0 && now() - m_heardAt < m_pace.stillThereEveryMs) {
        lookFailedAs(why);
        return;
    }
    if (std::optional<base::Error> away = heldBack()) {
        lookFailedAs(*away);
        return;
    }
    DeviceAsker::Ask rootAsked;
    rootAsked.path = QString(root);
    rootAsked.largest = largestReplyBytes;
    rootAsked.quietMs = m_pace.rootUnsignedMs;
    // (What tidy follows to is a Qt container out of memory: nothing of ours throws, DESIGN.md section 7.)
    // NOLINTNEXTLINE(bugprone-exception-escape)
    m_asker.send(rootAsked, {}, [this, why](const base::Result<Reply>& reply) {
        if (!reply && meansAway(reply.error().code)) {
            lookFailedAs(failed(reply.error()));
            return;
        }
        if (reply) {
            answered();
        }
        lookFailedAs(why);
    });
}

// --- its sensors, and its version -------------------------------------------------------------------------

// One look's walk through the fans of the newer shape: in turn, the ones read longest ago first, until the
// sensors' time is up (the web's `_sensors`).
struct RedfishDevice::FanWalk {
    QList<redfish::Temperature> temperatures;
    QList<QString> order;   // the places, in the controller's order
    QList<QString> pending; // still to be read in this look, longest ago first
    QHash<QString, qint64> freshAt;
    QHash<QString, std::optional<redfish::Fan>> fresh;
    qint64 until = 0;
    std::function<void(QList<redfish::Temperature>, QList<redfish::Fan>)> done;
};

void RedfishDevice::sensors(const Places& places,
                            std::function<void(QList<redfish::Temperature>, QList<redfish::Fan>)> done)
{
    if (!places.thermal.isEmpty()) {
        // The older shape: one resource, every temperature and every fan in it.
        read(places.thermal, false, [this, done = std::move(done)](const base::Result<QJsonObject>& thermal) {
            if (!thermal || !underJudged()) {
                done({}, {});
                return;
            }
            done(redfish::temperaturesOf(thermal.value()), redfish::fansOf(thermal.value()));
        });
        return;
    }
    auto walk = std::make_shared<FanWalk>();
    walk->until = now() + m_pace.sensorsWithinMs;
    walk->done = std::move(done);
    const auto fansNow = [this, walk, fans = places.fans] {
        const auto known = [this, walk] {
            QList<redfish::Fan> fans;
            for (const QString& place : std::as_const(m_fanOrder)) {
                if (const auto found = m_fansRead.constFind(place); found != m_fansRead.constEnd() && found->fan) {
                    fans.append(*found->fan);
                }
            }
            walk->done(walk->temperatures, fans);
        };
        if (fans.isEmpty() || now() >= walk->until) {
            if (fans.isEmpty()) {
                walk->done(walk->temperatures, {});
                return;
            }
            known(); // (no time left for the list: the fans as the last look found them)
            return;
        }
        read(fans, false, [this, walk, known](const base::Result<QJsonObject>& listed) {
            if (!listed || !underJudged()) {
                known();
                return;
            }
            // One resource a fan: no more places than the card would list, each once, in the controller's order;
            // read longest ago first (never before that), the others in the next look.
            const QJsonValue members = listed.value().value("Members"_L1);
            const QJsonArray all = members.isArray() ? members.toArray() : QJsonArray();
            for (qsizetype index = 0; index < all.size() && index < redfish::mostSensors; ++index) {
                const QString place = redfish::link(all.at(index).toObject().value("@odata.id"_L1));
                if (!place.isEmpty() && !walk->order.contains(place)) {
                    walk->order.append(place);
                }
            }
            walk->pending = walk->order;
            std::stable_sort(walk->pending.begin(), walk->pending.end(),
                             [this](const QString& one, const QString& other) {
                                 const auto first = m_fansRead.constFind(one);
                                 const auto second = m_fansRead.constFind(other);
                                 const qint64 oneAt =
                                     first == m_fansRead.constEnd() ? std::numeric_limits<qint64>::min() : first->at;
                                 const qint64 otherAt =
                                     second == m_fansRead.constEnd() ? std::numeric_limits<qint64>::min() : second->at;
                                 return oneAt < otherAt;
                             });
            nextFan(walk);
        });
    };
    if (places.metrics.isEmpty()) {
        fansNow();
        return;
    }
    read(places.metrics, false, [this, walk, fansNow](const base::Result<QJsonObject>& metrics) {
        if (metrics && underJudged()) {
            walk->temperatures = redfish::readingsOf(metrics.value());
        }
        fansNow();
    });
}

void RedfishDevice::nextFan(const std::shared_ptr<FanWalk>& walk)
{
    if (walk->pending.isEmpty() || now() >= walk->until) {
        // What was read now, and what the last look read of the rest; one the controller no longer lists is gone.
        QHash<QString, FanRead> kept;
        QList<redfish::Fan> fans;
        for (const QString& place : std::as_const(walk->order)) {
            if (walk->freshAt.contains(place)) {
                kept.insert(place, FanRead{walk->freshAt.value(place), walk->fresh.value(place)});
            } else if (m_fansRead.contains(place)) {
                kept.insert(place, m_fansRead.value(place));
            } else {
                continue;
            }
            if (kept.value(place).fan) {
                fans.append(*kept.value(place).fan);
            }
        }
        m_fansRead = kept;
        m_fanOrder = walk->order;
        walk->done(walk->temperatures, fans);
        return;
    }
    const QString place = walk->pending.takeFirst();
    read(place, false, [this, walk, place](const base::Result<QJsonObject>& fan) {
        if (fan && underJudged()) {
            walk->freshAt.insert(place, now());
            walk->fresh.insert(place, redfish::fanOf(fan.value()));
        }
        nextFan(walk);
    });
}

void RedfishDevice::itself(const Places& places, std::function<void(QString version)> done)
{
    if (m_versionAt && now() - *m_versionAt < m_pace.placesFreshMs) {
        later([done = std::move(done), version = m_version] { done(version); });
        return;
    }
    if (places.manager.isEmpty()) {
        m_versionAt = now();
        later([done = std::move(done), version = m_version] { done(version); });
        return;
    }
    read(places.manager, false,
         [this, own = places.manager, update = places.update,
          done = std::move(done)](const base::Result<QJsonObject>& manager) {
             if (!manager || !underJudged()) {
                 // What was known before stands meanwhile, and it is asked again sooner (ABOUT_RETRY).
                 m_versionAt = now() - m_pace.placesFreshMs + m_pace.aboutRetryMs;
                 done(m_version);
                 return;
             }
             versionOf(
                 manager.value(), own, update, [this] { return underJudged(); },
                 [this, done](const QString& version) {
                     m_version = version;
                     m_versionAt = now();
                     done(m_version);
                 });
         });
}

// One reading of its firmware inventory: the places still to be read, what the controller's own active images said,
// and until when a read may begin.
struct RedfishDevice::ImageWalk {
    QStringList pending;
    QString own;
    qint64 until = 0;
    QSet<QString> said;
    std::function<bool()> mine;
    std::function<void(QString)> done;
};

void RedfishDevice::versionOf(const QJsonObject& manager, const QString& own, const QString& update,
                              const std::function<bool()>& mine, std::function<void(QString version)> done)
{
    if (manager.contains("FirmwareVersion"_L1)) {
        // (Said: as it is. What is said and is no text is not looked for elsewhere.)
        later([done = std::move(done), version = kvmd::plain(manager.value("FirmwareVersion"_L1), 40)] {
            done(version);
        });
        return;
    }
    // bmcweb (OpenBMC) says its version only where it knows which of its images runs, and then names that image
    // (Links.ActiveSoftwareImage); OpenBMC's romulus image in QEMU said neither, long after it started. In the
    // standard's order: the image the controller says it runs; else, of the pieces of software in its UpdateService's
    // FirmwareInventory, those that name this very controller as what they are for (RelatedItem) and are in use
    // (Status.State "Enabled"): the version those agree on, and none where two say two. None either where not every
    // piece could be read: more than mostImages, more than the list holds, one that is no place on this controller, a
    // read that failed, or the time up (no read begins after imagesWithinMs).
    const qint64 until = now() + m_pace.imagesWithinMs;
    const QString active = redfish::link(
        manager.value("Links"_L1).toObject().value("ActiveSoftwareImage"_L1).toObject().value("@odata.id"_L1));
    if (!active.isEmpty()) {
        read(active, false, [mine, done = std::move(done)](const base::Result<QJsonObject>& image) {
            done(image && mine() ? kvmd::plain(image.value().value("Version"_L1), 40) : QString());
        });
        return;
    }
    if (update.isEmpty()) {
        later([done = std::move(done)] { done(QString()); });
        return;
    }
    read(update, false, [this, own, until, mine, done = std::move(done)](const base::Result<QJsonObject>& service) {
        const QString inventory =
            service && mine()
                ? redfish::link(service.value().value("FirmwareInventory"_L1).toObject().value("@odata.id"_L1))
                : QString();
        if (inventory.isEmpty() || now() >= until) {
            done(QString());
            return;
        }
        read(inventory, false, [this, own, until, mine, done](const base::Result<QJsonObject>& listed) {
            if (!listed || !mine()) {
                done(QString());
                return;
            }
            const QJsonValue members = listed.value().value("Members"_L1);
            const QJsonArray all = members.isArray() ? members.toArray() : QJsonArray();
            const std::optional<double> count = redfish::number(listed.value().value("Members@odata.count"_L1), 0, 1e9);
            auto walk = std::make_shared<ImageWalk>();
            for (const QJsonValue member : all) {
                const QString place = redfish::link(member.toObject().value("@odata.id"_L1));
                if (place.isEmpty()) {
                    done(QString()); // (one that is no place here may be the other active image)
                    return;
                }
                if (!walk->pending.contains(place)) {
                    walk->pending.append(place);
                }
            }
            if (all.size() > redfish::mostImages || listed.value().contains("Members@odata.nextLink"_L1) ||
                (count && *count > static_cast<double>(all.size()))) {
                done(QString()); // (not every one would be read)
                return;
            }
            walk->own = own;
            while (walk->own.endsWith(u'/')) {
                walk->own.chop(1);
            }
            walk->until = until;
            walk->mine = mine;
            walk->done = done;
            nextImage(walk);
        });
    });
}

void RedfishDevice::nextImage(const std::shared_ptr<ImageWalk>& walk)
{
    if (walk->pending.isEmpty()) {
        walk->done(walk->said.size() == 1 ? *walk->said.cbegin() : QString());
        return;
    }
    if (now() >= walk->until) {
        walk->done(QString());
        return;
    }
    read(walk->pending.takeFirst(), false, [this, walk](const base::Result<QJsonObject>& image) {
        if (!image || !walk->mine()) {
            walk->done(QString());
            return;
        }
        const QJsonValue related = image.value().value("RelatedItem"_L1);
        const QJsonArray items = related.isArray() ? related.toArray() : QJsonArray();
        bool forIt = false;
        for (qsizetype index = 0; index < items.size() && index < redfish::mostImages; ++index) {
            QString item = redfish::link(items.at(index).toObject().value("@odata.id"_L1));
            while (item.endsWith(u'/')) {
                item.chop(1);
            }
            forIt = forIt || (!item.isEmpty() && item == walk->own);
        }
        const QJsonValue state = image.value().value("Status"_L1).toObject().value("State"_L1);
        if (forIt && state == QJsonValue(u"Enabled"_s)) {
            // (An empty one for a version that is no text: an active image all the same.)
            walk->said.insert(kvmd::plain(image.value().value("Version"_L1), 40));
        }
        nextImage(walk);
    });
}

void RedfishDevice::afterLook(int withinMs, std::function<void(bool stillLooking)> then)
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
    *heard = connect(this, &RedfishDevice::looked, this, run);
    QTimer::singleShot(withinMs, this, run);
}

void RedfishDevice::act(const Action& what, std::function<void(base::Result<Done>)> done)
{
    // Keys, Ctrl+Alt+Del and a text go over its console, as the web's (redfish_screen.cpp).
    if (what.kind == "keys"_L1 || what.kind == "text"_L1 || what.kind == "ctrl_alt_del"_L1) {
        typeOnTheScreen(what, std::move(done));
        return;
    }
    // Start into BIOS setup (redfish_setup.cpp).
    if (what.kind == "bios_setup"_L1) {
        auto pressing = std::make_shared<Pressing>();
        pressing->kind = what.kind;
        pressing->done = std::move(done);
        m_pressing.append(pressing);
        startIntoSetup(pressing);
        return;
    }
    static const QStringList buttons{u"reboot"_s,         u"power_on"_s,  u"power_off"_s, u"atx_power"_s,
                                     u"atx_power_long"_s, u"atx_reset"_s, u"locate_on"_s, u"locate_off"_s};
    if (!buttons.contains(what.kind)) {
        // (What a KVM does, a controller does not: said before anything is sent, on the program's loop, as what came of
        // a press.)
        // NOLINTNEXTLINE(bugprone-exception-escape)
        QTimer::singleShot(0, QCoreApplication::instance(), [done = std::move(done)] {
            done(base::Error{"invalid", "A management controller cannot do that"});
        });
        return;
    }
    auto pressing = std::make_shared<Pressing>();
    pressing->kind = what.kind;
    pressing->ofTheComputer = what.kind != "reboot"_L1;
    pressing->done = std::move(done);
    m_pressing.append(pressing);
    find([this, pressing](const base::Result<Places>& places) {
        if (!places) {
            pressed(pressing, places.error());
            return;
        }
        // Which server answers, asked before anything is sent to it (the controller's restart included): what
        // decides the command is this read, and the command goes under the session it was answered under.
        checkedRead(
            places.value().system, true,
            [this, pressing, places = places.value()](const base::Result<QJsonObject>& system, const Checked& checked) {
                if (!system) {
                    pressed(pressing, system.error());
                    return;
                }
                pressing->place = places.system;
                if (pressing->kind == "locate_on"_L1 || pressing->kind == "locate_off"_L1) {
                    const std::optional<redfish::Light> lights = redfish::locator(system.value());
                    if (!lights) {
                        pressed(pressing,
                                base::Error{"action_failed", "This controller has no locator light to switch"});
                        return;
                    }
                    light(pressing, places.system, pressing->kind == "locate_on"_L1 ? lights->lightUp : lights->putOut,
                          checked.mark, false);
                    return;
                }
                if (!pressing->ofTheComputer) {
                    if (places.manager.isEmpty()) {
                        pressed(pressing,
                                base::Error{"action_failed", "This controller does not say how to restart it"});
                        return;
                    }
                    read(places.manager, true,
                         [this, pressing, manager = places.manager](const base::Result<QJsonObject>& itself) {
                             if (!itself) {
                                 pressed(pressing, itself.error());
                                 return;
                             }
                             offer(pressing, itself.value(), "#Manager.Reset"_L1, manager,
                                   redfish::wanted(pressing->kind, true));
                         });
                    return;
                }
                // What depends on whether it is on is not decided on a state it does not say (counted as on, a
                // power button would shut it down): stricter than the web, and said why.
                static const QStringList states{u"On"_s, u"Off"_s, u"PoweringOn"_s, u"PoweringOff"_s};
                const QString power = kvmd::plain(system.value().value("PowerState"_L1), 20);
                if (!states.contains(power) && (pressing->kind == "power_on"_L1 || pressing->kind == "power_off"_L1 ||
                                                pressing->kind == "atx_power"_L1)) {
                    pressed(
                        pressing,
                        base::Error{"action_failed",
                                    (power.isEmpty() ? u"The controller does not say whether the server is on or off: "
                                                       u"nothing was sent"_s
                                                     : u"The controller says the server is \"%1\", neither on nor off: "
                                                       u"nothing was sent"_s.arg(power))
                                        .toStdString()});
                    return;
                }
                const bool running = redfish::runs(system.value());
                if ((pressing->kind == "power_on"_L1 && running) || (pressing->kind == "power_off"_L1 && !running)) {
                    // Asked for by what is wanted, not by which button: forty servers ticked after a power
                    // cut, of which some came back by themselves, are not sorted into two piles by hand first.
                    pressed(pressing, Done{running ? u"already on"_s : u"already off"_s});
                    return;
                }
                offer(pressing, system.value(), "#ComputerSystem.Reset"_L1, places.system,
                      redfish::wanted(pressing->kind, running));
            });
    });
}

void RedfishDevice::offer(const std::shared_ptr<Pressing>& pressing, const QJsonObject& resource,
                          QLatin1StringView action, const QString& place, const QStringList& wanted)
{
    const redfish::Offered offered = redfish::offeredKinds(resource, action);
    QString target = offered.target;
    if (target.isEmpty()) {
        QString base = place;
        while (base.endsWith(u'/')) {
            base.chop(1);
        }
        target = base + u"/Actions/"_s + QString(action).mid(1);
    }
    const auto choose = [this, pressing, target, wanted](const QStringList& kinds) {
        const QString kind = redfish::kindToSend(wanted, kinds);
        if (kind.isEmpty()) {
            pressed(pressing, base::Error{"action_failed",
                                          u"This controller does not offer %1"_s.arg(wanted.value(0)).toStdString()});
            return;
        }
        press(pressing, target, kind);
    };
    if (offered.kinds) {
        choose(*offered.kinds);
        return;
    }
    if (offered.described.isEmpty()) {
        choose({});
        return;
    }
    // (Its description of the action: where that cannot be read, the usual kind is asked for, as ever.)
    read(offered.described, false, [choose](const base::Result<QJsonObject>& info) {
        choose(info ? redfish::kindsIn(info.value()) : QStringList());
    });
}

void RedfishDevice::press(const std::shared_ptr<Pressing>& pressing, const QString& target, const QString& kind)
{
    Ask command;
    command.method = "POST"_ba;
    command.path = target;
    command.body = QJsonDocument(QJsonObject{{u"ResetType"_s, kind}}).toJson(QJsonDocument::Compact);
    command.bodyIs = "application/json"_ba;
    command.doesSomething = true;
    command.command = true;
    pressing->sentKind = kind;
    pressing->sent = true;
    request(command, [this, pressing](const base::Result<Reply>& reply) {
        if (!reply) {
            if (!pressing->ofTheComputer && (reply.error().code == "dropped" || reply.error().code == "timeout")) {
                pressed(pressing, Done{}); // (a controller that is already going down may never answer)
                return;
            }
            pressed(pressing, reply.error());
            return;
        }
        // Only a 4xx is a no; an error of its own, or an answer a command does not have, says neither.
        if (const base::Result<void> verdict = redfish::verdictOn(reply.value().status, reply.value().body); !verdict) {
            pressed(pressing, verdict.error());
            return;
        }
        followUp();
        // "Accepted" is not "done": where it can be seen whether a command was carried out (on, off), it is
        // looked for before anybody is told that it was.
        if (pressing->ofTheComputer && (pressing->sentKind == "On"_L1 || pressing->sentKind == "ForceOff"_L1)) {
            carriedOut(pressing, now() + m_pace.confirmWithinMs, QString());
            return;
        }
        pressed(pressing, Done{});
    });
}

void RedfishDevice::carriedOut(const std::shared_ptr<Pressing>& pressing, qint64 until, const QString& seen)
{
    const bool on = pressing->sentKind == "On"_L1;
    checkedRead(
        pressing->place, false,
        [this, pressing, until, seen, on](const base::Result<QJsonObject>& system, const Checked& /*checked*/) {
            // (One of its moments, or another server, which says nothing of this one: asked again.)
            const QString power = system ? kvmd::plain(system.value().value("PowerState"_L1), 20) : QString();
            if (power == (on ? "On"_L1 : "Off"_L1)) {
                pressed(pressing, Done{});
                return;
            }
            const QString sawLast = power.isEmpty() ? seen : power;
            if (now() >= until) {
                if (sawLast.isEmpty()) {
                    pressed(pressing,
                            base::Error{
                                "timeout",
                                u"The controller accepted this, and no look at the server came back in %1 seconds to "
                                u"say whether it did %2: it may have been done. Look at the server before pressing "
                                u"again."_s.arg(m_pace.confirmWithinMs / 1000)
                                    .arg(on ? u"switch on"_s : u"switch off"_s)
                                    .toStdString()});
                    return;
                }
                pressed(pressing,
                        base::Error{"action_failed",
                                    u"The controller accepted this, but the server is still %1. The command was not "
                                    u"carried out."_s.arg(on ? u"off"_s : u"on"_s)
                                        .toStdString()});
                return;
            }
            QTimer::singleShot(m_pace.confirmEveryMs, this,
                               [this, pressing, until, sawLast] { carriedOut(pressing, until, sawLast); });
        });
}

void RedfishDevice::pressed(const std::shared_ptr<Pressing>& pressing, const base::Result<Done>& result)
{
    m_pressing.removeOne(pressing);
    // Before the command went (its computer could not be read, or judged): nothing of it was done. (`sent` is set
    // before it goes, sign-in and all: that errs on the side of "it may have been done", which request() has words
    // for where its sign-in came to nothing.)
    base::Result<Done> said = result;
    if (!result && !pressing->sent && mayHaveBeenDone(result.error())) {
        said = nothingSent(result.error(), "the server could not be asked which it is");
    }
    // Handed on the program's loop, and not this driver's: one let go of in this very turn hands it all the same.
    // (As the driver's destructor: the words of an answer out of memory.)
    // NOLINTNEXTLINE(bugprone-exception-escape)
    QTimer::singleShot(0, QCoreApplication::instance(), [pressing, said] {
        if (const auto done = std::exchange(pressing->done, nullptr)) {
            done(said);
        }
    });
}

void RedfishDevice::light(const std::shared_ptr<Pressing>& pressing, const QString& place, const QJsonObject& change,
                          const QByteArray& mark, bool meant)
{
    Ask asked;
    asked.method = "PATCH"_ba;
    asked.path = place;
    asked.body = QJsonDocument(change).toJson(QJsonDocument::Compact);
    asked.bodyIs = "application/json"_ba;
    asked.doesSomething = true;
    asked.command = true;
    const QByteArray shown = meant ? "*"_ba : mark;
    if (!shown.isEmpty()) {
        asked.sent = {{"If-Match"_ba, shown}};
    }
    pressing->sent = true;
    request(asked, [this, pressing, place, change, mark, meant](const base::Result<Reply>& reply) {
        if (!reply) {
            pressed(pressing, reply.error());
            return;
        }
        // A controller wants to be shown the state the change was decided on. Where it will not take the mark it gave
        // out itself (some compare them more strictly than they hand them out), or gave none and wants one all the
        // same, it is told that the change is meant whatever the state.
        const int status = reply.value().status;
        if (!meant && (status == 412 || status == 428)) {
            light(pressing, place, change, mark, true);
            return;
        }
        if (const base::Result<void> verdict = redfish::verdictOn(status, reply.value().body); !verdict) {
            pressed(pressing, verdict.error());
            return;
        }
        followUp();
        pressed(pressing, Done{});
    });
}

void RedfishDevice::followUp()
{
    // A controller says "accepted" and does what was asked a little later: its card is out of date now, and it is
    // looked at again at these moments, instead of saying the old thing until its next turn comes round.
    m_statusAt = std::numeric_limits<qint64>::min() / 2;
    m_followUpAt.clear();
    for (const int after : std::as_const(m_pace.followUpMs)) {
        m_followUpAt.append(now() + after);
    }
}

void RedfishDevice::askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done)
{
    // (A controller keeps no list of computers to wake: none.)
    later([done = std::move(done)] { done(QList<WakeTarget>()); });
}

// (Its console is given back by its screen when nobody looked for a while: ServerScreen::Limits::idleMs.)
void RedfishDevice::tidy() {}

} // namespace qkvm::core
