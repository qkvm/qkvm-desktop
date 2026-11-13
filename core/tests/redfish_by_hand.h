#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include "stand_in.h"

namespace qkvm::core::testing {

// A server's management controller by hand, as the web's stand-in answers (../app/tests/fake_redfish.py),
// which encodes what the two AMI boards answered: as much of Redfish as this program asks of one so far, on
// a StandIn, over plain HTTP or TLS (two of these may show one certificate, as boards whose maker put one on
// every board do).
//
// * Laid out as AMI lays it out (one computer, one chassis and one controller, each "Self") or as OpenBMC
//   does ("system", "chassis", "bmc").
// * GET /redfish/v1 and /redfish/v1/ to anybody: its version, the number of its Redfish service (its
//   UUID), its maker or its update service, and where its computers, chassis, controllers and sessions are.
// * POST .../SessionService/Sessions with {"UserName", "Password"}: 201 with a token (X-Auth-Token) and
//   the session's place (Location); 401 for another user or password; whatever `loginStatus` says instead
//   ("No more sessions"), and 503 once it has `mostSessions` sessions out.
// * DELETE .../SessionService/Sessions/{id}: the session is gone; 204, or a byte at a time (`dawdles`).
// * Everything else needs a token it gave: 401 "NoValidSession" otherwise.
// * Never Basic: an Authorization or a Cookie that comes with a question is counted, and none ever may.
class RedfishByHand : public QObject {
public:
    enum class Layout { Ami, OpenBmc };
    enum class LocationIs { Own, AnotherHost, OutsideRedfish, None, Account, Deeper };

    // At a port of the test's choosing where one controller is to take the place of another.
    explicit RedfishByHand(Layout layout = Layout::Ami, const std::optional<Tls>& tls = {}, quint16 port = 0)
        : m_layout(layout)
    {
        m_clock.start();
        const auto answer = [this](const Asked& asked) { return answered(asked); };
        stand = tls ? std::make_unique<StandIn>(answer, *tls, port) : std::make_unique<StandIn>(answer, port);
        stand->handsLinesOver = false;
    }

    // What it is.
    QString user = QStringLiteral("admin");
    QString password = QStringLiteral("secret");
    QString power = QStringLiteral("On");
    QString health = QStringLiteral("OK");
    QJsonValue model = QStringLiteral("SRV-2200");
    QJsonValue manufacturer = QStringLiteral("Example Systems");
    QJsonValue hostname;           // (none)
    QJsonValue uuid = fresh();     // the computer's own number: every one its own
    QJsonValue rootUuid = fresh(); // its Redfish service's; none: the root says none
    // What its manager names as the service it hosts (ServiceEntryPointUUID; a real AMI board: the root's).
    // None set: the root's; a null: not said.
    std::optional<QJsonValue> entryUuid;
    bool managerNamesItself = true;  // (ami layout) its manager's own UUID is the root's, as a real AMI board's
    bool rootUuidSignedOnly = false; // its root says its number only to a session it knows
    QJsonValue firmware = QStringLiteral("1.42.0");
    // (openbmc layout) What its controller says of its firmware: "both" its version and the image it runs
    // (Links.ActiveSoftwareImage), as bmcweb does where it knows which image runs; "image" the image alone; "neither",
    // as OpenBMC's romulus image in QEMU did, where the version is in the firmware inventory alone.
    QString managerSays = QStringLiteral("both");
    // (openbmc layout) The firmware inventory, in its order, as bmcweb lists the software it has: an image of purpose
    // BMC names the controller as its RelatedItem, one of purpose Host the computer's BIOS; "Enabled" where it is
    // active. None: a Host image first, then the controller's own, whose version is `firmware`. And what the
    // controller's own image names as what it is for instead of the controller; more entries of the list, as they are;
    // more that the list says of itself (a next page, a count).
    struct Image {
        QString id;
        QString purpose;
        QString state;
        QJsonValue version;
    };
    std::optional<QList<Image>> images;
    std::optional<QJsonValue> imageLink;
    QJsonArray inventoryMore;
    QJsonObject inventorySays;
    int inventoryTakesMs = 0; // its UpdateService and inventory are answered only after this long
    // The light to find the server by: the older light with its states, and what it takes; the newer plain on
    // and off where that is set (always in the openbmc layout: bmcweb has retired the older one).
    QJsonValue led = QStringLiteral("Off");
    QJsonValue ledKinds = QJsonArray{QStringLiteral("Lit"), QStringLiteral("Blinking"), QStringLiteral("Off")};
    QJsonValue locating;
    QString systemLink;          // what it says its computer is (empty: the usual)
    bool managerNamed = true;    // its computer names the controller that manages it
    int managerFails = 0;        // its controller answers so many reads with 503 first
    int rootTakesMs = 0;         // its root is answered only after this long
    int systemTakesMs = 0;       // its computer is answered only after this long
    int rootUnsignedTakesMs = 0; // its root, asked with no session, is answered only after this long
    bool systemUnwell = false;   // its computer is answered with an error of its own (503), the rest as ever
    // The sensors (fake_redfish.py): the first processor's temperature and health; a list of chassis that begins
    // with a disk enclosure, which has no sensors (as an iDRAC's may); (openbmc layout) so many fans more than the
    // three, the last of them not well, each answered after so long.
    QJsonValue cpuTemperature = 61;
    QString temperatureHealth = QStringLiteral("OK");
    bool enclosuresFirst = false;
    int moreFans = 0;
    bool lastFanUnwell = false;
    int fanTakesMs = 0;
    bool alsoNewerShape = false; // (ami layout) its chassis has the newer shape of the sensors too, as an iDRAC's has
    int rootFails = 0;           // so many reads of its root without a session are answered 503 first
    bool rootFailsIn200 = false; // ...or with an error said as 200, as some controllers do at their moments
    int rootHangsUp = 0;         // so many reads of its root without a session are taken and hung up on
    int loginStatus = 201;       // another: a sign-in is answered with it, whatever the password
    int loginTakesMs = 0;        // a sign-in is answered only after this long
    bool loginNever = false;     // a sign-in is taken, and never answered
    int loginsNever = 0;         // ...so many of them, from the next one on
    bool bodyNamesItsPlace = false; // a sign-in's answer names the session's place (@odata.id)
    QString unauthorizedAt;         // a place it refuses (401) under a session it knows: one the account may not read
    bool signOutNever = false;      // a sign-out is taken, and never answered
    QByteArray tokenIs;             // where set, the token every sign-in hands out, as it is
    LocationIs locationIs = LocationIs::Own;
    int mostSessions = 4;
    bool dawdles = false;         // a sign-out is answered a byte at a time, and never to its end
    bool refusesSessions = false; // it signs in, and refuses the session it gave everywhere else

    // What is done to it (fake_redfish.py 402-446, 643-653): the kinds of reset it offers (in its ResetActionInfo);
    // kinds taken and not done ("accepted", and nothing happens); the power seen to change only after so long; a
    // reset answered with this (an error of its own, say) and not done; one never answered, or hung up on, after
    // it was done; its controller's own restart answered with this, or away so long, its sessions gone.
    QStringList offered{QStringLiteral("On"), QStringLiteral("ForceOff"), QStringLiteral("GracefulShutdown"),
                        QStringLiteral("ForceRestart")};
    bool offeredOnTheAction = false; // said on the action itself, and no ActionInfo
    QStringList drops;
    int powerTakesMs = 0;
    int resetStatus = 0;
    bool resetNeverAnswered = false;
    bool resetHangsUp = false;
    int managerResetStatus = 0;
    int restartTakesMs = 0;
    // Its light's change (337-366): the mark it puts on the computer's state; marks it hands out weak and does not
    // take back; a change of the light answered with this.
    QString mark = QStringLiteral("\"1791401332\"");
    bool strictMarks = false;
    bool putsNoMark = false;           // ...and one that puts none on it, and wants one shown all the same
    bool marksInItsHeaderOnly = false; // ...or says it in the ETag header alone, not in the resource
    int lightStatus = 0;
    // Its boot override (Boot): the one-time targets it allows (none: it says no Boot at all), and the override as it
    // stands; a change of it answered with this (0: made, and answered 204, as the real board answers), or answered
    // 202 and not kept, or answered only after so long; a clearing of it answered with this; and whether a server
    // restarted or switched on with its setup override set takes it as it starts, after so long (the override back
    // to Disabled, and the server in its setup).
    QJsonValue bootTargets;
    QString bootEnabled = QStringLiteral("Disabled");
    QString bootTarget = QStringLiteral("None");
    int bootStatus = 0;
    bool bootNotKept = false;
    int bootTakesMs = 0;
    int clearStatus = 0;
    // ...a clearing made and answered with this in place of 204 (200: with no body, as some controllers do); and, where
    // set, what its computer says as its Boot in place of the override as it stands (undefined: no Boot at all; a
    // null; an empty one), whatever the override is.
    int clearAnswer = 204;
    std::optional<QJsonValue> bootSaidAs;
    bool startsIntoSetup = true;
    int setupTakesMs = 0;
    bool inSetup = false;

    // Its own web page (AMI's), the way to its console (fake_redfish.py 650-712): there at all (none: every place of it
    // answers 404); a sign-in there answered with this whatever the password, or only after so long; its two words for
    // the server's own monitor going dark while a console is open, as it hands them out with the pass (a real board
    // says 0 or 1; undefined: not said at all); its firmware lets a console reconnect; it says what it can do.
    bool ownConsole = true;
    int webLoginStatus = 0;
    int webSignInTakesMs = 0;
    QJsonValue automaticOff = 0;
    QJsonValue localMonitorOff = 0;
    bool reconnects = false;
    bool saysWhatItCan = true;
    // ...and what it hands out there in place of the usual: a token that is none; no cookie; a pass that is none.
    QString webTokenIs;
    bool webGivesNoCookie = false;
    QString passIs;
    // ...a cookie of this value; a sign-in taken and never answered; the pass, to a session, only after so long.
    QByteArray webCookieIs;
    bool webLoginNever = false;
    int tokenTakesMs = 0;

    // What it was asked.
    // Of its web page: its sessions (a cookie, and the token it wants beside it); its sign-ins, and what they said;
    // its sessions ended; how often its console's pass was asked for (signed in or not), and the passes handed out.
    QHash<QByteArray, QByteArray> webSessions;
    int webLogins = 0;
    QList<QByteArray> webLoginBodies;
    int webSignedOut = 0;
    int tokenAsks = 0;
    QSet<QByteArray> consolePasses;
    QStringList actions;         // what it did, as "system On", "controller ForceRestart"
    QList<QJsonObject> changes;  // every change made to the computer's resource
    QList<QByteArray> ifMatches; // the If-Match of every change asked for ("(none)" where none came)
    int logins = 0;
    int signedOut = 0;
    int rootsUnsigned = 0;               // the root read without a session of its own
    QStringList requests;                // every signed-in question, as "GET /path"
    QHash<QByteArray, QString> sessions; // token -> session
    // Called with every question as it comes, before it is answered: a moment a test makes happen just then.
    std::function<void(const Asked&)> whenAsked;
    QList<QByteArray> loginBodies;
    int authorizations = 0; // questions that came with an Authorization: none may
    int cookies = 0;        // and with a Cookie
    std::unique_ptr<StandIn> stand;

    QUrl address() const { return QUrl(stand->address()); }
    QString system() const { return QStringLiteral("/redfish/v1/Systems/") + names().at(0); }
    QString manager() const { return QStringLiteral("/redfish/v1/Managers/") + names().at(2); }
    int asked(const QString& request) const { return static_cast<int>(requests.count(request)); }
    // Another server at its address (a new lease): new numbers, and the sessions of the one before unknown.
    void anotherServer()
    {
        uuid = fresh();
        rootUuid = fresh();
        sessions.clear();
    }
    // One of its moments: for so long, whatever is asked is answered with an error (503; or said with 200, as
    // some controllers do).
    void unwellFor(int ms, bool in200 = false)
    {
        m_unwellUntil = m_clock.elapsed() + ms;
        m_unwellIn200 = in200;
    }
    // Its places as another make lays them out, from now on (another board at its address).
    void becomeLayout(Layout layout) { m_layout = layout; }
    // A server of this one's numbers, from now on, with no session of the one before.
    void beServer(const QJsonValue& system, const QJsonValue& root)
    {
        uuid = system;
        rootUuid = root;
        sessions.clear();
    }

    static QString fresh() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

private:
    QStringList names() const
    {
        return m_layout == Layout::Ami
                   ? QStringList{QStringLiteral("Self"), QStringLiteral("Self"), QStringLiteral("Self")}
                   : QStringList{QStringLiteral("system"), QStringLiteral("chassis"), QStringLiteral("bmc")};
    }
    static QByteArray written(const QJsonObject& said) { return QJsonDocument(said).toJson(QJsonDocument::Compact); }
    static Answered error(int status, const QString& message, const QString& code = {})
    {
        QJsonObject said{{QStringLiteral("message"), message}};
        if (!code.isEmpty()) {
            said.insert(QStringLiteral("code"), code);
        }
        return {status, written(QJsonObject{{QStringLiteral("error"), said}})};
    }

    Answered answered(const Asked& asked)
    {
        const QByteArray path = asked.path.endsWith('/') && asked.path.size() > 1 ? asked.path.chopped(1) : asked.path;
        const QByteArray token = asked.headers.value("x-auth-token");
        authorizations += asked.headers.contains("authorization") ? 1 : 0;
        cookies += asked.headers.contains("cookie") ? 1 : 0;
        if (whenAsked) {
            whenAsked(asked);
        }
        if (m_clock.elapsed() < m_unwellUntil) {
            // (Restarting, or busy with itself: there, and not ready.)
            if (m_unwellIn200) {
                return {200, R"({"error": "Service Unavailable"})"};
            }
            return error(503, QStringLiteral("Service Unavailable"));
        }
        if (path.startsWith("/api/")) {
            return webPage(asked, QString::fromLatin1(path));
        }
        const bool signedIn = !token.isEmpty() && sessions.contains(token);
        if (path == "/redfish/v1" && asked.method == "GET") {
            if (!signedIn) {
                ++rootsUnsigned;
                if (rootFails > 0) {
                    --rootFails;
                    Answered unwell = error(rootFailsIn200 ? 200 : 503, QStringLiteral("Service Unavailable"));
                    return unwell;
                }
                if (rootHangsUp > 0) {
                    --rootHangsUp;
                    Answered gone;
                    gone.hangUp = true;
                    return gone;
                }
            } else if (!refusesSessions) {
                requests.append(QStringLiteral("GET /redfish/v1"));
            }
            {
                // (To anybody, under any session or none, as Redfish has it: a session it refuses everywhere else
                // included.)
                QJsonObject said = rootSaid();
                if (!signedIn && rootUuidSignedOnly) {
                    said.remove(QStringLiteral("UUID"));
                }
                Answered root(200, written(said));
                root.afterMs = signedIn ? rootTakesMs : std::max(rootTakesMs, rootUnsignedTakesMs);
                return root;
            }
        }
        if (path == "/redfish/v1/SessionService/Sessions" && asked.method == "POST") {
            return signIn(asked);
        }
        if (!signedIn || refusesSessions || (!unauthorizedAt.isEmpty() && path == unauthorizedAt.toLatin1())) {
            return error(401, QStringLiteral("Sign in first"), QStringLiteral("Base.1.5.0.NoValidSession"));
        }
        requests.append(QString::fromLatin1(asked.method + ' ' + path));
        const QString at = QString::fromLatin1(path);
        if (asked.method == "DELETE" && at.startsWith(QStringLiteral("/redfish/v1/SessionService/Sessions/"))) {
            const QString session = at.section(u'/', -1);
            for (auto one = sessions.begin(); one != sessions.end();) {
                if (one.value() == session) {
                    one = sessions.erase(one);
                    ++signedOut;
                } else {
                    ++one;
                }
            }
            if (signOutNever) {
                Answered never;
                never.never = true;
                return never;
            }
            if (dawdles) {
                Answered slowly(200, QByteArray(100, ' '));
                slowly.byteEveryMs = 100;
                return slowly;
            }
            return {204, {}};
        }
        if (std::optional<Answered> done = doneAt(asked, at)) {
            return *done;
        }
        if (asked.method != "GET") {
            return error(405, QStringLiteral("Not allowed"));
        }
        if (at == QStringLiteral("/redfish/v1/Systems")) {
            const QString place = systemLink.isEmpty() ? system() : systemLink;
            return {200, written(QJsonObject{
                             {QStringLiteral("Members"), QJsonArray{QJsonObject{{QStringLiteral("@odata.id"), place}}}},
                             {QStringLiteral("Members@odata.count"), 1}})};
        }
        if (at == system()) {
            if (systemUnwell) {
                return error(503, QStringLiteral("Service Unavailable"));
            }
            if (m_powerAt >= 0 && m_clock.elapsed() >= m_powerAt) {
                power = m_powerLater;
                m_powerAt = -1;
            }
            tookItsOverride();
            Answered computerSaid(200, written(computer()));
            computerSaid.afterMs = systemTakesMs;
            if (!putsNoMark) {
                computerSaid.more.append("ETag: " + shownMark().toLatin1());
            }
            return computerSaid;
        }
        if (at == system() + QStringLiteral("/ResetActionInfo")) {
            QJsonArray kinds;
            for (const QString& kind : std::as_const(offered)) {
                kinds.append(kind);
            }
            return {200,
                    written(QJsonObject{{QStringLiteral("Parameters"),
                                         QJsonArray{QJsonObject{{QStringLiteral("Name"), QStringLiteral("ResetType")},
                                                                {QStringLiteral("Required"), true},
                                                                {QStringLiteral("AllowableValues"), kinds}}}}})};
        }
        if (std::optional<Answered> sensors = sensorsAt(at)) {
            return *sensors;
        }
        if (at == QStringLiteral("/redfish/v1/Managers")) {
            return {200, written(QJsonObject{{QStringLiteral("Members"),
                                              QJsonArray{QJsonObject{{QStringLiteral("@odata.id"), manager()}}}}})};
        }
        if (std::optional<Answered> inventory = inventoryAt(at)) {
            return *inventory;
        }
        if (at == manager()) {
            if (managerFails > 0) {
                --managerFails;
                return error(503, QStringLiteral("Service Unavailable"));
            }
            return {200, written(controller())};
        }
        return error(404, QStringLiteral("No such resource"), QStringLiteral("Base.1.5.0.ResourceMissingAtURI"));
    }

    Answered signIn(const Asked& asked)
    {
        ++logins;
        loginBodies.append(asked.body);
        Answered said;
        if (loginStatus != 201) {
            said = error(loginStatus, QStringLiteral("No more sessions"));
            if (loginStatus == 204) {
                said.body.clear();
            }
        } else if (sessions.size() >= mostSessions) {
            said = error(503, QStringLiteral("No more sessions"));
        } else {
            const QJsonObject body = QJsonDocument::fromJson(asked.body).object();
            if (body.value(QStringLiteral("UserName")).toString() != user ||
                body.value(QStringLiteral("Password")).toString() != password) {
                said = error(401, QStringLiteral("Invalid credentials"));
            } else {
                const QString session = QString::number(++m_made);
                const QByteArray token =
                    tokenIs.isEmpty() ? QUuid::createUuid().toByteArray(QUuid::Id128) + "Ab+/=" : tokenIs;
                sessions.insert(token, session);
                QJsonObject answer{{QStringLiteral("Id"), session}};
                if (bodyNamesItsPlace) {
                    answer.insert(QStringLiteral("@odata.id"),
                                  QStringLiteral("/redfish/v1/SessionService/Sessions/") + session);
                }
                said = Answered(201, written(answer));
                said.more.append("X-Auth-Token: " + token);
                const QByteArray place = "/redfish/v1/SessionService/Sessions/" + session.toLatin1();
                switch (locationIs) {
                case LocationIs::Own:
                    said.more.append("Location: " + place);
                    break;
                case LocationIs::AnotherHost:
                    said.more.append("Location: https://elsewhere.example" + place);
                    break;
                case LocationIs::OutsideRedfish:
                    said.more.append("Location: /api/session/" + session.toLatin1());
                    break;
                case LocationIs::Account:
                    said.more.append("Location: /redfish/v1/AccountService/Accounts/2");
                    break;
                case LocationIs::Deeper:
                    said.more.append("Location: " + place + "/More");
                    break;
                case LocationIs::None:
                    break;
                }
            }
        }
        said.afterMs = loginTakesMs;
        said.never = loginNever || loginsNever > 0;
        loginsNever = std::max(0, loginsNever - 1);
        return said;
    }

    QJsonObject rootSaid() const
    {
        QJsonObject said{
            {QStringLiteral("RedfishVersion"), QStringLiteral("1.15.1")},
            {QStringLiteral("Systems"),
             QJsonObject{{QStringLiteral("@odata.id"), QStringLiteral("/redfish/v1/Systems")}}},
            {QStringLiteral("Chassis"),
             QJsonObject{{QStringLiteral("@odata.id"), QStringLiteral("/redfish/v1/Chassis")}}},
            {QStringLiteral("Managers"),
             QJsonObject{{QStringLiteral("@odata.id"), QStringLiteral("/redfish/v1/Managers")}}},
            {QStringLiteral("Links"),
             QJsonObject{
                 {QStringLiteral("Sessions"),
                  QJsonObject{{QStringLiteral("@odata.id"), QStringLiteral("/redfish/v1/SessionService/Sessions")}}}}},
        };
        if (m_layout == Layout::Ami) {
            said.insert(QStringLiteral("Vendor"), QStringLiteral("AMI"));
            said.insert(QStringLiteral("Product"), QStringLiteral("AMI Redfish Server"));
        } else {
            said.insert(QStringLiteral("UpdateService"),
                        QJsonObject{{QStringLiteral("@odata.id"), QStringLiteral("/redfish/v1/UpdateService")}});
        }
        if (!rootUuid.isNull()) {
            said.insert(QStringLiteral("UUID"), rootUuid);
        }
        return said;
    }

    QJsonObject computer() const
    {
        QJsonObject links{{QStringLiteral("Chassis"),
                           QJsonArray{QJsonObject{{QStringLiteral("@odata.id"),
                                                   QStringLiteral("/redfish/v1/Chassis/") + names().at(1)}}}}};
        if (managerNamed) {
            links.insert(QStringLiteral("ManagedBy"),
                         QJsonArray{QJsonObject{{QStringLiteral("@odata.id"), manager()}}});
        }
        QJsonObject light{{QStringLiteral("IndicatorLED"), led},
                          {QStringLiteral("IndicatorLED@Redfish.AllowableValues"), ledKinds}};
        if (!locating.isNull() || m_layout == Layout::OpenBmc) {
            light = QJsonObject{{QStringLiteral("LocationIndicatorActive"), locating}};
        }
        QJsonObject said{
            {QStringLiteral("Id"), names().at(0)},
            {QStringLiteral("Model"), model},
            {QStringLiteral("Manufacturer"), manufacturer},
            {QStringLiteral("HostName"), hostname},
            {QStringLiteral("PowerState"), power},
            {QStringLiteral("UUID"), uuid},
            {QStringLiteral("Status"), QJsonObject{{QStringLiteral("Health"), QStringLiteral("OK")},
                                                   {QStringLiteral("HealthRollup"), health},
                                                   {QStringLiteral("State"), QStringLiteral("Enabled")}}},
            {QStringLiteral("ProcessorSummary"),
             QJsonObject{{QStringLiteral("Count"), 2},
                         {QStringLiteral("Model"), QStringLiteral("Example Processor 4000")}}},
            {QStringLiteral("MemorySummary"), QJsonObject{{QStringLiteral("TotalSystemMemoryGiB"), 256}}},
            {QStringLiteral("Links"), links},
        };
        for (auto part = light.cbegin(); part != light.cend(); ++part) {
            said.insert(part.key(), part.value());
        }
        QJsonObject reset{{QStringLiteral("target"), system() + QStringLiteral("/Actions/ComputerSystem.Reset")}};
        if (offeredOnTheAction) {
            QJsonArray kinds;
            for (const QString& kind : std::as_const(offered)) {
                kinds.append(kind);
            }
            reset.insert(QStringLiteral("ResetType@Redfish.AllowableValues"), kinds);
        } else {
            reset.insert(QStringLiteral("@Redfish.ActionInfo"), system() + QStringLiteral("/ResetActionInfo"));
        }
        said.insert(QStringLiteral("Actions"), QJsonObject{{QStringLiteral("#ComputerSystem.Reset"), reset}});
        if (!putsNoMark && !marksInItsHeaderOnly) {
            said.insert(QStringLiteral("@odata.etag"), shownMark());
        }
        if (!bootTargets.isNull()) {
            said.insert(QStringLiteral("Boot"),
                        QJsonObject{{QStringLiteral("BootSourceOverrideEnabled"), bootEnabled},
                                    {QStringLiteral("BootSourceOverrideTarget"), bootTarget},
                                    {QStringLiteral("BootSourceOverrideTarget@Redfish.AllowableValues"), bootTargets}});
        }
        if (bootSaidAs) {
            said.insert(QStringLiteral("Boot"), *bootSaidAs); // (undefined takes it out)
        }
        return said;
    }

    // A server that starts with its setup override set takes it, a while later.
    void startsWithItsOverride()
    {
        if (startsIntoSetup && bootEnabled == QStringLiteral("Once") && bootTarget == QStringLiteral("BiosSetup")) {
            m_setupAt = m_clock.elapsed() + setupTakesMs;
        }
    }
    void tookItsOverride()
    {
        if (m_setupAt >= 0 && m_clock.elapsed() >= m_setupAt) {
            bootEnabled = QStringLiteral("Disabled");
            inSetup = true;
            m_setupAt = -1;
        }
    }

    QString shownMark() const
    {
        return putsNoMark ? QString() : (strictMarks ? QStringLiteral("W/") : QString()) + mark;
    }

    // What is done to it, at `place`; none where `place` is no place things are done at.
    std::optional<Answered> doneAt(const Asked& asked, const QString& place)
    {
        const QJsonObject said = QJsonDocument::fromJson(asked.body).object();
        if (asked.method == "POST" && place == system() + QStringLiteral("/Actions/ComputerSystem.Reset")) {
            const QString kind = said.value(QStringLiteral("ResetType")).toString();
            if (resetStatus != 0) {
                return error(resetStatus, QStringLiteral("Internal error"));
            }
            if (!offered.contains(kind)) {
                return Answered{
                    400,
                    written(QJsonObject{
                        {QStringLiteral("error"),
                         QJsonObject{{QStringLiteral("@Message.ExtendedInfo"),
                                      QJsonArray{QJsonObject{{QStringLiteral("Message"),
                                                              QStringLiteral("The value %1 for the property ResetType "
                                                                             "is not in the list of acceptable values.")
                                                                  .arg(kind)}}}}}}})};
            }
            actions.append(QStringLiteral("system ") + kind);
            Answered done(204, {});
            done.never = resetNeverAnswered;
            done.hangUp = resetHangsUp;
            if (drops.contains(kind)) {
                done.status = 202; // accepted, and that is all that comes of it
                return done;
            }
            const QString becomes = kind == QStringLiteral("ForceOff") || kind == QStringLiteral("GracefulShutdown")
                                        ? QStringLiteral("Off")
                                        : QStringLiteral("On");
            if (becomes == QStringLiteral("On")) {
                startsWithItsOverride();
            }
            if (powerTakesMs > 0) {
                m_powerAt = m_clock.elapsed() + powerTakesMs;
                m_powerLater = becomes;
                done.status = 202;
                return done;
            }
            power = becomes;
            return done;
        }
        if (asked.method == "POST" && place == manager() + QStringLiteral("/Actions/Manager.Reset")) {
            if (managerResetStatus != 0) {
                return error(managerResetStatus, QStringLiteral("Internal error"));
            }
            actions.append(QStringLiteral("controller ") + said.value(QStringLiteral("ResetType")).toString());
            if (restartTakesMs > 0) {
                unwellFor(restartTakesMs);
                sessions.clear();
            }
            Answered restarted(204, {});
            restarted.hangUp = resetHangsUp; // (going down, it may never answer)
            return restarted;
        }
        if (asked.method == "PATCH" && place == system()) {
            // As AMI's controllers do it: no change without being shown the state it was decided on.
            ifMatches.append(asked.headers.value("if-match", "(none)"));
            if (!asked.headers.contains("if-match")) {
                return error(428, QStringLiteral("If-Match or If-None-Match header is required"));
            }
            const QByteArray shown = asked.headers.value("if-match");
            if (shown != "*" && (shown != mark.toLatin1() || strictMarks)) {
                return error(412, QStringLiteral("The resource changed, or the mark is not a strong one"));
            }
            if (said.contains(QStringLiteral("Boot"))) {
                const QJsonObject boot = said.value(QStringLiteral("Boot")).toObject();
                const bool clearing =
                    boot.value(QStringLiteral("BootSourceOverrideEnabled")).toString() == QStringLiteral("Disabled");
                const int status = clearing ? clearStatus : bootStatus;
                if (status != 0) {
                    Answered failed = error(status, QStringLiteral("The boot override could not be changed"));
                    failed.afterMs = bootTakesMs;
                    return failed;
                }
                changes.append(said);
                mark = QStringLiteral("\"%1\"").arg(mark.mid(1, mark.size() - 2).toLongLong() + 1);
                if (bootNotKept && !clearing) {
                    Answered accepted{202, {}};
                    accepted.afterMs = bootTakesMs;
                    return accepted;
                }
                if (boot.contains(QStringLiteral("BootSourceOverrideEnabled"))) {
                    bootEnabled = boot.value(QStringLiteral("BootSourceOverrideEnabled")).toString();
                }
                if (boot.contains(QStringLiteral("BootSourceOverrideTarget"))) {
                    bootTarget = boot.value(QStringLiteral("BootSourceOverrideTarget")).toString();
                }
                Answered made{clearing ? clearAnswer : 204, {}};
                made.afterMs = bootTakesMs;
                return made;
            }
            if (lightStatus != 0) {
                return error(lightStatus, QStringLiteral("Internal error"));
            }
            changes.append(said);
            if (said.contains(QStringLiteral("IndicatorLED"))) {
                led = said.value(QStringLiteral("IndicatorLED"));
            }
            if (said.contains(QStringLiteral("LocationIndicatorActive"))) {
                locating = said.value(QStringLiteral("LocationIndicatorActive"));
            }
            mark = QStringLiteral("\"%1\"").arg(mark.mid(1, mark.size() - 2).toLongLong() + 1);
            return Answered{202, {}}; // "accepted", which is what the real one says
        }
        return std::nullopt;
    }

    static QJsonObject at(const QString& place) { return QJsonObject{{QStringLiteral("@odata.id"), place}}; }

    // Its web page's session, where the question carries one it gave out (its cookie, and the token beside it).
    std::optional<QByteArray> webSessionOf(const Asked& asked) const
    {
        QByteArray cookie;
        for (const QByteArray& part : asked.headers.value("cookie").split(';')) {
            const QByteArray one = part.trimmed();
            if (one.startsWith("QSESSIONID=")) {
                cookie = one.mid(11);
            }
        }
        if (cookie.isEmpty() || !webSessions.contains(cookie) ||
            asked.headers.value("x-csrftoken") != webSessions.value(cookie)) {
            return std::nullopt;
        }
        return cookie;
    }

    Answered webPage(const Asked& asked, const QString& at)
    {
        static const Answered notThere(404, R"({"error": "Not Found"})");
        static const Answered signInFirst(401, R"({"cc": 7, "error": "Invalid Authentication"})");
        if (at == QStringLiteral("/api/kvm/token") && asked.method == "GET") {
            ++tokenAsks;
        }
        if (!ownConsole) {
            return notThere;
        }
        if (at == QStringLiteral("/api/session") && asked.method == "POST") {
            ++webLogins;
            webLoginBodies.append(asked.body);
            const QUrlQuery form(QString::fromUtf8(asked.body));
            if (webLoginStatus != 0) {
                Answered other = error(webLoginStatus, QStringLiteral("Not now"));
                other.afterMs = webSignInTakesMs; // (as long as a yes)
                return other;
            }
            if (form.queryItemValue(QStringLiteral("username"), QUrl::FullyDecoded) != user ||
                form.queryItemValue(QStringLiteral("password"), QUrl::FullyDecoded) != password) {
                Answered no = signInFirst; // (a no takes as long as a yes)
                no.afterMs = webSignInTakesMs;
                return no;
            }
            const QByteArray cookie = fresh().remove(u'-').left(24).toLatin1();
            const QByteArray token = fresh().remove(u'-').left(8).toLatin1();
            webSessions.insert(cookie, token);
            Answered in(200, written(QJsonObject{{QStringLiteral("ok"), 0},
                                                 {QStringLiteral("privilege"), 4},
                                                 {QStringLiteral("CSRFToken"),
                                                  webTokenIs.isNull() ? QString::fromLatin1(token) : webTokenIs}}));
            if (!webGivesNoCookie) {
                in.more.append("Set-Cookie: QSESSIONID=" + (webCookieIs.isEmpty() ? cookie : webCookieIs) +
                               "; path=/; HttpOnly");
            }
            in.never = webLoginNever;
            in.afterMs = webSignInTakesMs;
            return in;
        }
        const std::optional<QByteArray> session = webSessionOf(asked);
        if (at == QStringLiteral("/api/session") && asked.method == "DELETE") {
            if (!session) {
                return signInFirst;
            }
            webSessions.remove(*session);
            ++webSignedOut;
            return {200, R"({"ok": 0})"};
        }
        if (at == QStringLiteral("/api/configuration/project") && asked.method == "GET") {
            if (!saysWhatItCan) {
                return notThere;
            }
            if (!session) {
                return signInFirst;
            }
            QJsonArray can{
                QJsonObject{{QStringLiteral("id"), 1}, {QStringLiteral("feature"), QStringLiteral("KVM")}},
                QJsonObject{{QStringLiteral("id"), 2}, {QStringLiteral("feature"), QStringLiteral("VMEDIA")}}};
            if (reconnects) {
                can.append(QJsonObject{{QStringLiteral("id"), 3},
                                       {QStringLiteral("feature"), QStringLiteral("KVM_SESSION_RECONNECT")}});
            }
            return {200, QJsonDocument(can).toJson(QJsonDocument::Compact)};
        }
        if (at == QStringLiteral("/api/kvm/token") && asked.method == "GET") {
            if (!session) {
                return signInFirst;
            }
            const QByteArray made = fresh().remove(u'-').toLatin1();
            consolePasses.insert(made);
            QJsonObject said{{QStringLiteral("client_ip"), QStringLiteral("127.0.0.1")},
                             {QStringLiteral("token"), passIs.isNull() ? QString::fromLatin1(made) : passIs},
                             {QStringLiteral("session"), fresh().left(8)},
                             {QStringLiteral("server_ip"), QStringLiteral("127.0.0.1")},
                             {QStringLiteral("kvm_service_status"), 1},
                             {QStringLiteral("power_save_mode"), 0}};
            if (!automaticOff.isUndefined()) {
                said.insert(QStringLiteral("automatic_off"), automaticOff);
            }
            if (!localMonitorOff.isUndefined()) {
                said.insert(QStringLiteral("local_monitor_off"), localMonitorOff);
            }
            Answered given(200, written(said));
            given.afterMs = tokenTakesMs;
            return given;
        }
        return notThere;
    }
    QString chassis() const { return QStringLiteral("/redfish/v1/Chassis/") + names().at(1); }
    static QJsonObject fine()
    {
        return QJsonObject{{QStringLiteral("Health"), QStringLiteral("OK")},
                           {QStringLiteral("State"), QStringLiteral("Enabled")}};
    }

    // (name, reading, status) of every temperature, in whichever shape it is then said.
    QList<std::tuple<QString, QJsonValue, QJsonObject>> temperatures() const
    {
        return {{QStringLiteral("CPU0_TEMP"), cpuTemperature,
                 QJsonObject{{QStringLiteral("Health"), temperatureHealth},
                             {QStringLiteral("State"), QStringLiteral("Enabled")}}},
                {QStringLiteral("CPU1_TEMP"), 55, fine()},
                {QStringLiteral("DIMM_A_TEMP"), 45, fine()},
                {QStringLiteral("M2_TEMP"), QJsonValue(),
                 QJsonObject{{QStringLiteral("State"), QStringLiteral("Absent")}}}};
    }

    // The fans of the newer shape, by their place's last part, in the controller's order.
    QList<std::pair<QString, QJsonObject>> everyFan() const
    {
        const auto fan = [](const QString& name, const QJsonObject& speed, const QJsonObject& status) {
            return QJsonObject{{QStringLiteral("Name"), name},
                               {QStringLiteral("SpeedPercent"), speed},
                               {QStringLiteral("Status"), status}};
        };
        QList<std::pair<QString, QJsonObject>> fans{
            {QStringLiteral("fan_CPU0_FAN"),
             fan(QStringLiteral("CPU0_FAN"), QJsonObject{{QStringLiteral("SpeedRPM"), 1650}}, fine())},
            {QStringLiteral("fan_CPU1_FAN"),
             fan(QStringLiteral("CPU1_FAN"), QJsonObject{{QStringLiteral("Reading"), 18}},
                 QJsonObject{{QStringLiteral("Health"), QStringLiteral("Warning")},
                             {QStringLiteral("State"), QStringLiteral("Enabled")}})},
            {QStringLiteral("fan_SYS_FAN5"),
             fan(QStringLiteral("SYS_FAN5"), QJsonObject{{QStringLiteral("SpeedRPM"), QJsonValue()}},
                 QJsonObject{{QStringLiteral("State"), QStringLiteral("Absent")}})}};
        for (int index = 0; index < moreFans; ++index) {
            const bool unwell = lastFanUnwell && index == moreFans - 1;
            fans.append(
                {QStringLiteral("fan_EXTRA_%1").arg(index),
                 fan(QStringLiteral("EXTRA_%1").arg(index), QJsonObject{{QStringLiteral("SpeedRPM"), 1000 + index}},
                     unwell ? QJsonObject{{QStringLiteral("Health"), QStringLiteral("Warning")},
                                          {QStringLiteral("State"), QStringLiteral("Enabled")}}
                            : fine())});
        }
        return fans;
    }

    // The chassis and its sensors, in the layout's shape; none where `place` is none of them.
    std::optional<Answered> sensorsAt(const QString& place) const
    {
        const QString own = chassis();
        const QString enclosure = QStringLiteral("/redfish/v1/Chassis/Enclosure.Internal.0-1");
        if (place == QStringLiteral("/redfish/v1/Chassis")) {
            QJsonArray members{at(own)};
            if (enclosuresFirst) {
                members.prepend(at(enclosure));
            }
            return Answered{200, written(QJsonObject{{QStringLiteral("Members"), members}})};
        }
        if (place == enclosure && enclosuresFirst) {
            return Answered{
                200, R"({"Id": "Enclosure.Internal.0-1", "ChassisType": "StorageEnclosure", "Model": "Backplane"})"};
        }
        if (place == own) {
            QJsonObject said{{QStringLiteral("Id"), names().at(1)}, {QStringLiteral("Model"), model}};
            if (m_layout == Layout::Ami) {
                said.insert(QStringLiteral("Thermal"), at(own + QStringLiteral("/Thermal")));
                if (alsoNewerShape) {
                    said.insert(QStringLiteral("ThermalSubsystem"), at(own + QStringLiteral("/ThermalSubsystem")));
                }
            } else {
                said.insert(QStringLiteral("ThermalSubsystem"), at(own + QStringLiteral("/ThermalSubsystem")));
                said.insert(QStringLiteral("Sensors"), at(own + QStringLiteral("/Sensors")));
            }
            return Answered{200, written(said)};
        }
        if (m_layout == Layout::Ami && place == own + QStringLiteral("/Thermal")) {
            QJsonArray readings;
            for (const auto& [name, reading, status] : temperatures()) {
                readings.append(QJsonObject{{QStringLiteral("Name"), name},
                                            {QStringLiteral("ReadingCelsius"), reading},
                                            {QStringLiteral("UpperThresholdCritical"), 98},
                                            {QStringLiteral("Status"), status}});
            }
            const QJsonArray fans{QJsonObject{{QStringLiteral("Name"), QStringLiteral("CPU0_FAN")},
                                              {QStringLiteral("Reading"), 1650},
                                              {QStringLiteral("ReadingUnits"), QStringLiteral("RPM")},
                                              {QStringLiteral("Status"), fine()}},
                                  QJsonObject{{QStringLiteral("Name"), QStringLiteral("CPU1_FAN")},
                                              {QStringLiteral("Reading"), 450},
                                              {QStringLiteral("ReadingUnits"), QStringLiteral("RPM")},
                                              {QStringLiteral("Status"),
                                               QJsonObject{{QStringLiteral("Health"), QStringLiteral("Warning")},
                                                           {QStringLiteral("State"), QStringLiteral("Enabled")}}}},
                                  QJsonObject{{QStringLiteral("Name"), QStringLiteral("SYS_FAN5")},
                                              {QStringLiteral("Reading"), QJsonValue()},
                                              {QStringLiteral("ReadingUnits"), QStringLiteral("RPM")},
                                              {QStringLiteral("Status"),
                                               QJsonObject{{QStringLiteral("State"), QStringLiteral("Absent")}}}}};
            return Answered{
                200, written(QJsonObject{{QStringLiteral("Temperatures"), readings}, {QStringLiteral("Fans"), fans}})};
        }
        if (m_layout != Layout::OpenBmc && !alsoNewerShape) {
            return std::nullopt;
        }
        const QString subsystem = own + QStringLiteral("/ThermalSubsystem");
        if (place == subsystem) {
            return Answered{200, written(QJsonObject{{QStringLiteral("Id"), QStringLiteral("ThermalSubsystem")},
                                                     {QStringLiteral("Fans"), at(subsystem + QStringLiteral("/Fans"))},
                                                     {QStringLiteral("ThermalMetrics"),
                                                      at(subsystem + QStringLiteral("/ThermalMetrics"))}})};
        }
        if (place == subsystem + QStringLiteral("/ThermalMetrics")) {
            // As bmcweb says a reading: its value, and where the sensor itself is; no name. The first carries its
            // health as the older shape does.
            QJsonArray readings;
            for (const auto& [name, reading, status] : temperatures()) {
                QJsonObject one{
                    {QStringLiteral("Reading"), reading},
                    {QStringLiteral("DataSourceUri"), own + QStringLiteral("/Sensors/temperature_") + name}};
                if (readings.isEmpty()) {
                    one.insert(QStringLiteral("Status"), QJsonObject{{QStringLiteral("Health"), temperatureHealth}});
                }
                readings.append(one);
            }
            return Answered{200, written(QJsonObject{{QStringLiteral("Id"), QStringLiteral("ThermalMetrics")},
                                                     {QStringLiteral("TemperatureReadingsCelsius"), readings}})};
        }
        if (place == subsystem + QStringLiteral("/Fans")) {
            QJsonArray members;
            for (const auto& [name, fan] : everyFan()) {
                members.append(at(subsystem + QStringLiteral("/Fans/") + name));
            }
            return Answered{200, written(QJsonObject{{QStringLiteral("Members"), members},
                                                     {QStringLiteral("Members@odata.count"), members.size()}})};
        }
        if (place.startsWith(subsystem + QStringLiteral("/Fans/"))) {
            for (const auto& [name, fan] : everyFan()) {
                if (place == subsystem + QStringLiteral("/Fans/") + name) {
                    QJsonObject said = fan;
                    said.insert(QStringLiteral("Id"), name);
                    Answered one(200, written(said));
                    one.afterMs = fanTakesMs;
                    return one;
                }
            }
        }
        return std::nullopt;
    }

    QJsonObject controller() const
    {
        QJsonObject said{
            {QStringLiteral("Id"), names().at(2)},
            {QStringLiteral("ManagerType"), QStringLiteral("BMC")},
            {QStringLiteral("FirmwareVersion"), firmware},
            {QStringLiteral("Actions"),
             QJsonObject{{QStringLiteral("#Manager.Reset"),
                          QJsonObject{{QStringLiteral("target"), manager() + QStringLiteral("/Actions/Manager.Reset")},
                                      {QStringLiteral("ResetType@Redfish.AllowableValues"),
                                       QJsonArray{QStringLiteral("ForceRestart")}}}}}}};
        const QJsonValue entry = entryUuid.value_or(rootUuid);
        if (!entry.isNull()) {
            said.insert(QStringLiteral("ServiceEntryPointUUID"), entry);
        }
        if (m_layout == Layout::Ami && managerNamesItself && !rootUuid.isNull()) {
            said.insert(QStringLiteral("UUID"), rootUuid);
        }
        if (m_layout == Layout::OpenBmc) {
            said.insert(QStringLiteral("Model"), QStringLiteral("OpenBmc"));
            // Its firmware's version, and the image it runs, are said where bmcweb knows which of its images runs.
            if (managerSays != QStringLiteral("both")) {
                said.remove(QStringLiteral("FirmwareVersion"));
            }
            const QList<Image> all = firmwareImages();
            const auto running = std::find_if(all.cbegin(), all.cend(), [](const Image& one) {
                return one.purpose == QStringLiteral("BMC") && one.state == QStringLiteral("Enabled");
            });
            if (managerSays != QStringLiteral("neither") && running != all.cend()) {
                said.insert(
                    QStringLiteral("Links"),
                    QJsonObject{{QStringLiteral("ActiveSoftwareImage"),
                                 at(QStringLiteral("/redfish/v1/UpdateService/FirmwareInventory/") + running->id)}});
            }
        }
        return said;
    }

    QList<Image> firmwareImages() const
    {
        return images.value_or(
            QList<Image>{{QStringLiteral("3d6f0a11"), QStringLiteral("Host"), QStringLiteral("Enabled"),
                          QStringLiteral("host-2.13")},
                         {QStringLiteral("7ed25e88"), QStringLiteral("BMC"), QStringLiteral("Enabled"), firmware}});
    }

    // Its UpdateService and firmware inventory (openbmc layout); none where `place` is none of them.
    std::optional<Answered> inventoryAt(const QString& place) const
    {
        const QString update = QStringLiteral("/redfish/v1/UpdateService");
        const QString inventory = update + QStringLiteral("/FirmwareInventory");
        if (m_layout != Layout::OpenBmc || !place.startsWith(update)) {
            return std::nullopt;
        }
        if (place == update) {
            Answered service(200, written(QJsonObject{{QStringLiteral("Id"), QStringLiteral("UpdateService")},
                                                      {QStringLiteral("FirmwareInventory"), at(inventory)}}));
            service.afterMs = inventoryTakesMs;
            return service;
        }
        if (place == inventory) {
            QJsonArray members;
            for (const Image& one : firmwareImages()) {
                members.append(at(inventory + QStringLiteral("/") + one.id));
            }
            for (const QJsonValue more : inventoryMore) {
                members.append(more);
            }
            QJsonObject said{{QStringLiteral("Members"), members},
                             {QStringLiteral("Members@odata.count"), members.size()}};
            for (auto part = inventorySays.cbegin(); part != inventorySays.cend(); ++part) {
                said.insert(part.key(), part.value());
            }
            return Answered{200, written(said)};
        }
        for (const Image& one : firmwareImages()) {
            if (place == inventory + QStringLiteral("/") + one.id) {
                const QJsonValue related = one.purpose == QStringLiteral("BMC")
                                               ? imageLink.value_or(QJsonValue(manager()))
                                               : QJsonValue(system() + QStringLiteral("/Bios"));
                return Answered{200, written(QJsonObject{
                                         {QStringLiteral("Id"), one.id},
                                         {QStringLiteral("Description"), one.purpose + QStringLiteral(" image")},
                                         {QStringLiteral("RelatedItem"),
                                          QJsonArray{QJsonObject{{QStringLiteral("@odata.id"), related}}}},
                                         {QStringLiteral("Status"), QJsonObject{{QStringLiteral("State"), one.state}}},
                                         {QStringLiteral("Version"), one.version}})};
            }
        }
        return std::nullopt;
    }

    Layout m_layout;
    int m_made = 0;
    QElapsedTimer m_clock;
    qint64 m_unwellUntil = -1;
    qint64 m_powerAt = -1;
    QString m_powerLater;
    qint64 m_setupAt = -1; // when a server started with its setup override set takes it (-1: none does)
    bool m_unwellIn200 = false;
};

} // namespace qkvm::core::testing
