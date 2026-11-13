#include <memory>

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/settings.h>

#include "made_up_kvm.h"
#include "memory_secrets.h"

#include "alerts_view.h"
#include "control.h"
#include "control_view.h"
#include "keys_by_hand.h"
#include "server_session.h"
#include "session_view.h"
#include "stand_in.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::MadeUpKvm;
using core::testing::StandIn;
using ui::WallModel;

namespace {

// A QKVM as far as signing in and looking at the wall go: one account, or several; with or without a
// second step; a wall of two KVMs and a server.
struct Pretended {
    bool setUp = true;
    bool severalAccounts = false;
    bool secondStep = false;
    bool signedIn = false; // the server knows the session it gave, or is taken to have given
    QByteArray wall = R"({"interval": 0.25, "devices": [
        {"id": "a", "name": "Build server", "host": "192.168.1.21", "kind": "kvmd", "picture": true},
        {"id": "b", "name": "NAS", "host": "192.168.1.22", "kind": "kvmd", "picture": true},
        {"id": "s", "name": "Database server", "host": "192.168.1.71", "kind": "redfish", "picture": false}]})";
    QByteArray status = R"({"a": {"state": "online"}, "b": {"state": "no_signal"},
                           "s": {"state": "online", "picture": false, "server": {"health": "OK"}}})";

    // Who the server takes the one who is signed in for: an admin, until a test says otherwise.
    QByteArray user = R"({"id": "u1", "name": "anna", "role": "admin", "token": false})";
    // The session it gives: this one to the first who signs in, and another to each one after.
    QByteArray session = "abc123";
    int signIns = 0;
    // How long its answer to a sign-out is on its way; and whether a sign-out reaches it at all.
    int signOutAnsweredAfterMs = 0;
    bool deafToSignOut = false;
    // The key of the one display link the server has given out, and how often it was handed in.
    QByteArray displayKey = "qkvm_" + QByteArray(43, 'd');
    int displaysOpened = 0;
    bool fallsOverAtALink = false; // closes the line on whoever hands a display link in
    // Each order of the wall it was told, as it was sent; and whether it takes one at all.
    QList<QStringList> arranged;
    bool refusesToArrange = false;
    // Whether it has live pictures of its machines: it says so in its wall, with the form of
    // them it speaks, and gives a pass for a line; and the one machine it has none of.
    bool hasLive = false;
    QByteArray noLiveOf;
    // The one machine whose snapshots take three seconds to come.
    QByteArray slowSnapshotOf;
    // The wall as a file: what was asked for one, and what was handed in to be read and taken.
    QList<QJsonObject> exportsAsked;
    QList<QJsonObject> filesRead;
    QList<QJsonObject> filesTaken;
    // What it says of itself to anybody: the versions of its API it speaks, and whether it was
    // restarted locked and nobody has signed in since.
    QByteArray apis = R"(["v1"])";
    bool locked = false;
    void giveLive(int form = 2)
    {
        hasLive = true;
        wall.replace("{\"interval\"", "{\"live\": " + QByteArray::number(form) + ", \"interval\"");
    }

    static Answered refused(int status, const char* code, const char* message)
    {
        return {status, QJsonDocument(QJsonObject{{u"code"_s, QLatin1StringView(code)},
                                                  {u"message"_s, QLatin1StringView(message)}})
                            .toJson()};
    }

    // Whether who asks is signed in: the server knows the session, and the question carries it.
    bool knows(const Asked& asked) const
    {
        return signedIn && asked.headers.value("cookie").split(';').contains(QByteArray("qkvm_session=") + session);
    }

    Answered answer(const Asked& asked)
    {
        if (asked.path == "/healthz") {
            return {200, R"({"status": "ok", "version": "0.1.0", "api": )" + apis +
                             R"(, "page": "3f2a9c", "locked": )" + QByteArray(locked ? "true" : "false") + "}"};
        }
        if (asked.path == "/api/v1/session") {
            const bool known = knows(asked);
            QByteArray said = R"({"initialized": )" + QByteArray(setUp ? "true" : "false") + R"(, "authenticated": )" +
                              QByteArray(known ? "true" : "false") + R"(, "ask_name": )" +
                              QByteArray(severalAccounts ? "true" : "false") + R"(, "user": )" +
                              (known ? user : QByteArray("null")) + "}";
            return {200, said};
        }
        if (asked.path == "/api/v1/login") {
            const QJsonObject typed = QJsonDocument::fromJson(asked.body).object();
            if (severalAccounts && typed["name"_L1].toString().isEmpty()) {
                return refused(422, "name_required", "Enter your user name");
            }
            if (typed["password"_L1].toString() != "the right one"_L1) {
                return refused(401, "bad_password", "Wrong password");
            }
            if (secondStep && typed["code"_L1].toString().isEmpty()) {
                return refused(401, "code_required", "Enter the code from your authenticator app");
            }
            if (secondStep && typed["code"_L1].toString() != "123456"_L1) {
                return refused(401, "bad_code", "That code is not right. Codes change every 30 seconds and work once.");
            }
            signedIn = true;
            // (Whoever signs in unlocks it.)
            locked = false;
            if (++signIns > 1) {
                session = "again" + QByteArray::number(signIns);
            }
            return {200,
                    QByteArray(R"({"ok": true, "user": )") + user + "}",
                    "application/json",
                    {"Set-Cookie: qkvm_session=" + session + "; Path=/; HttpOnly"}};
        }
        if (asked.path == "/api/v1/display" && fallsOverAtALink) {
            Answered nothing;
            nothing.hangUp = true;
            return nothing;
        }
        if (asked.path == "/api/v1/display") {
            if (QJsonDocument::fromJson(asked.body).object()["key"_L1].toString().toLatin1() != displayKey) {
                return refused(401, "bad_key", "This display link is not valid any more. Ask an admin for a new one.");
            }
            // A session of its own each time, as a server gives one.
            signedIn = true;
            session = "shown" + QByteArray::number(++displaysOpened);
            return {
                200,
                R"({"ok": true, "user": {"id": "token:t1", "name": "Lobby screen", "role": "viewer", "token": true}})",
                "application/json",
                {"Set-Cookie: qkvm_session=" + session + "; Path=/; HttpOnly"}};
        }
        if (asked.path == "/api/v1/logout") {
            Answered done{
                200, R"({"ok": true})", "application/json", {"Set-Cookie: qkvm_session=\"\"; Max-Age=0; Path=/"}};
            if (deafToSignOut) {
                done.never = true;
                return done;
            }
            // As a QKVM does it: the session the question names is ended, and no other; and the
            // answer takes the cookie back.
            if (knows(asked)) {
                signedIn = false;
            }
            done.afterMs = signOutAnsweredAfterMs;
            return done;
        }
        if (!knows(asked)) {
            return refused(401, "unauthorized", "Sign in first");
        }
        if (asked.method == "POST" && asked.path == "/api/v1/export") {
            const QJsonObject wanted = QJsonDocument::fromJson(asked.body).object();
            exportsAsked.append(wanted);
            // As a QKVM does it: the password of whoever asks, once more.
            if (wanted["password"_L1].toString() != "the right one"_L1) {
                return refused(401, "bad_password", "Wrong password");
            }
            // (Each file says the how-manieth it is, so that one is not taken for another.)
            const bool sealed = !wanted["passphrase"_L1].toString().isEmpty();
            return {200,
                    QJsonDocument(QJsonObject{{u"format"_s, u"qkvm-export"_s},
                                              {sealed ? u"sealed"_s : u"made"_s, exportsAsked.size()}})
                        .toJson(),
                    "application/json",
                    {QByteArray("Content-Disposition: attachment; filename=\"qkvm-2026-10-11.") +
                         (sealed ? "qkvm" : "json") + "\"",
                     "X-QKVM-Not-Opened: 0"}};
        }
        if (asked.method == "POST" && asked.path == "/api/v1/import/read") {
            filesRead.append(QJsonDocument::fromJson(asked.body).object());
            return {200, QJsonDocument(QJsonObject{{u"encrypted"_s, false},
                                                   {u"secrets"_s, false},
                                                   {u"made"_s, 1791676800},
                                                   {u"program"_s, u"0.1.0"_s},
                                                   {u"devices"_s, QJsonArray{QJsonObject{{u"name"_s, u"Rack"_s},
                                                                                         {u"host"_s, u"192.168.1.30"_s},
                                                                                         {u"kind"_s, u"kvmd"_s},
                                                                                         {u"password"_s, false},
                                                                                         {u"new"_s, true}}}},
                                                   {u"actions"_s, QJsonValue::Null},
                                                   {u"alerts"_s, QJsonValue::Null},
                                                   {u"wall"_s, QJsonObject()},
                                                   {u"left_out"_s, QJsonArray()}})
                             .toJson()};
        }
        if (asked.method == "POST" && asked.path == "/api/v1/import") {
            filesTaken.append(QJsonDocument::fromJson(asked.body).object());
            // The machine is on the wall from now on, as on a QKVM that took a file in.
            QJsonObject whole = QJsonDocument::fromJson(wall).object();
            QJsonArray devices = whole["devices"_L1].toArray();
            devices.append(QJsonObject{{u"id"_s, u"r"_s},
                                       {u"name"_s, u"Rack"_s},
                                       {u"host"_s, u"192.168.1.30"_s},
                                       {u"kind"_s, u"kvmd"_s},
                                       {u"picture"_s, true}});
            whole["devices"_L1] = devices;
            wall = QJsonDocument(whole).toJson(QJsonDocument::Compact);
            QJsonObject states = QJsonDocument::fromJson(status).object();
            states["r"_L1] = QJsonObject{{u"state"_s, u"connecting"_s}};
            status = QJsonDocument(states).toJson(QJsonDocument::Compact);
            return {200, R"({"devices": 1, "actions": 0, "alerts": false, "wall": false, "left_out": []})"};
        }
        if (asked.method == "POST" && asked.path.endsWith("/live")) {
            return hasLive && asked.path != "/api/v1/devices/" + noLiveOf + "/live"
                       ? Answered{200, R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})"}
                       : refused(404, "no_live", "This machine has no live picture");
        }
        // Whatever is asked of a machine is done.
        if (asked.method == "POST" && asked.path.endsWith("/actions")) {
            return {200, R"({"ok": true, "note": null})"};
        }
        if (asked.method == "PUT" && asked.path == "/api/v1/wall/order") {
            if (refusesToArrange) {
                return refused(403, "forbidden", "Only an admin can do that");
            }
            // As a QKVM does it: the machines that are named first, as named, the rest after them
            // as they stood; and its wall is in that order from then on.
            QStringList named;
            for (const auto& id : QJsonDocument::fromJson(asked.body).object()["ids"_L1].toArray()) {
                named.append(id.toString());
            }
            arranged.append(named);
            QJsonObject whole = QJsonDocument::fromJson(wall).object();
            QJsonArray before = whole["devices"_L1].toArray();
            QJsonArray after;
            for (const QString& id : std::as_const(named)) {
                for (const auto& device : std::as_const(before)) {
                    if (device.toObject()["id"_L1].toString() == id) {
                        after.append(device);
                    }
                }
            }
            for (const auto& device : std::as_const(before)) {
                if (!named.contains(device.toObject()["id"_L1].toString())) {
                    after.append(device);
                }
            }
            whole["devices"_L1] = after;
            wall = QJsonDocument(whole).toJson(QJsonDocument::Compact);
            QJsonArray ids;
            for (const auto& device : std::as_const(after)) {
                ids.append(device.toObject()["id"_L1]);
            }
            return {200, QJsonDocument(QJsonObject{{u"ok"_s, true}, {u"ids"_s, ids}}).toJson()};
        }
        if (asked.path == "/api/v1/wall") {
            return {200, wall};
        }
        if (asked.path == "/api/v1/status") {
            return {200, status};
        }
        Answered picture{200, core::testing::aPicture(), "image/jpeg"};
        if (!slowSnapshotOf.isEmpty() && asked.path.startsWith("/api/v1/devices/" + slowSnapshotOf + "/snapshot")) {
            picture.afterMs = 3000;
        }
        return picture;
    }
};

// The machines of a wall in the wall's own order, by the server's names for them.
QStringList keysOf(ui::WallView& wall)
{
    QStringList keys;
    for (int row = 0; row < wall.machines()->rowCount(); ++row) {
        keys.append(wall.machines()->index(row, 0).data(WallModel::KeyRole).toString());
    }
    return keys;
}

// A window more, as far as a session goes: a wall and screens of its own.
struct Window {
    ui::WallView wall;
    ui::ActionsView actions;
};

// What a session is made with, for one test: the two views, settings in a folder of the test's own,
// and secrets in memory.
struct Desk {
    // `paceChosenBefore`: how often a picture is asked for was chosen on the wall before there was
    // a session at all, as it is when the program starts with what it remembers; 0: it was not.
    // `statusEveryMs`: how often the machines are looked at; far more often than the program does,
    // unless a test wants to see what happens between two looks.
    // `livePace`: how long a window waits before it asks again for a machine live.
    explicit Desk(int paceChosenBefore = 0, int statusEveryMs = 100,
                  ui::LivePicture::Pace livePace = {10, 150, 600, 5, 20})
        : wall(paced(paceChosenBefore)), session{view, wall, settings, secrets, statusEveryMs}
    {
        session.retryLiveAt(livePace);
        // One window, which shows the program's own wall.
        session.add(wall, actions, nullptr);
        session.tellTheTimeWith([this] { return now; });
    }

    // What time the session is told it is.
    QDateTime now{QDate(2026, 10, 10), QTime(14, 2, 30)};

    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    core::testing::MemorySecrets secrets;
    ui::SessionView view;
    // (Made by the session's maker before the session: the members are made in this order.)
    ui::WallView storedWall;
    ui::WallView& wall;
    ui::ActionsView actions;
    ui::ServerSession session;

    ui::WallView& paced(int everyMs)
    {
        storedWall.setChosenPace(everyMs);
        return storedWall;
    }

    // How the settings name a stand-in server: its name and port.
    static QString authorityOf(const StandIn& server) { return u"127.0.0.1:%1"_s.arg(server.serverPort()); }
    // As if somebody had said yes to this certificate for this server before.
    void knows(const StandIn& server, const QByteArray& sha256)
    {
        core::Remembered remembered = settings.remembered();
        remembered.certificates.insert(authorityOf(server), sha256);
        static_cast<void>(settings.remember(remembered));
    }
    // The display link of a stand-in server, as the page shows it to be copied.
    static QString linkTo(const StandIn& server, const QByteArray& key)
    {
        return server.address() + u"/#display&key="_s + QString::fromLatin1(key);
    }
    static QString linkNameFor(const StandIn& server) { return u"display link at "_s + server.address() + u'/'; }
    // Under which name the session of a stand-in server is kept.
    static QString nameFor(const StandIn& server) { return u"session at "_s + server.address() + u'/'; }
    // As if somebody had signed in to this server before, and asked to stay signed in.
    void hasKept(const StandIn& server, const QByteArray& token = "abc123")
    {
        secrets.all.insert(nameFor(server), token);
    }
    // A machine's key on the wall: of the server's by the server's id for it, and of this PC's own
    // wall by the KVM's id there.
    static QString key(const QString& machine)
    {
        return ui::wallKey(ui::ServerSession::serverId(), machine).value_or(QString());
    }
    static QString ownKey(const QString& kvm)
    {
        return ui::wallKey(ui::PcSource::idOfThisPc(), kvm).value_or(QString());
    }
};

// The times of the drivers of this PC's own KVMs, made short.
core::KvmdDevice::Pace quickKvms()
{
    core::KvmdDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.freshMs = 100;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    return pace;
}

// A made-up KVM as this PC's own wall knows one.
core::OwnKvm at(const MadeUpKvm& kvm, const QString& name)
{
    core::OwnKvm own;
    own.address = kvm.stand->address();
    own.name = name;
    return own;
}

// How a file is chosen in a test: it is kept here, and that one is opened. Where no name is
// given, nobody said where, or which.
ui::FileDoor doorTo(const QString& savesTo, const QString& opens)
{
    return {[savesTo](const QString&) { return base::Result<QString>(savesTo); },
            [opens] { return base::Result<QString>(opens); }};
}

// What a file that was written holds.
QJsonObject written(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

} // namespace

// What the screens ask of the session, done against a stand-in server; and the wall it fills.
class TestServerSession : public QObject {
    Q_OBJECT

private:
    // One of the two certificates tools/make_testdata.ps1 makes, with its key.
    static core::testing::Tls tls(const char* name)
    {
        const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
        return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
    }

    static QByteArray sha256Of(const char* name)
    {
        return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
    }

    // What the card of a machine shows of one kind, by the wall's name for the machine.
    static QString onTheCardOf(ui::WallView& wall, const QString& machine, WallModel::Role what)
    {
        for (int row = 0; row < wall.machines()->rowCount(); ++row) {
            const QModelIndex card = wall.machines()->index(row, 0);
            if (card.data(WallModel::KeyRole).toString() == machine) {
                return card.data(what).toString();
            }
        }
        return u"(no such card)"_s;
    }

    static QStringList namesOn(ui::WallView& wall)
    {
        QStringList names;
        for (int row = 0; row < wall.machines()->rowCount(); ++row) {
            names.append(wall.machines()->index(row, 0).data(WallModel::NameRole).toString());
        }
        return names;
    }

private slots:
    void requiredMfaEnrollmentDoesNotOpenTheWall()
    {
        Pretended qkvm;
        qkvm.user = R"({"id":"u1","name":"anna","role":"admin","must_enroll":true})";
        StandIn server([&](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn({}, u"the right one"_s, {}, true);
        QTRY_VERIFY(desk.view.problem().contains(u"two-factor"_s));
        QCOMPARE(desk.wall.machines()->rowCount(), 0);
        QVERIFY(!desk.actions.allowed());
        QVERIFY(std::none_of(server.asked.cbegin(), server.asked.cend(),
                             [](const Asked& asked) { return asked.path == "/api/v1/wall"; }));
    }

    void backgroundMonitoringStopsWhenTheAccountBecomesADisplayLink()
    {
        Pretended pretend;
        pretend.signedIn = true;
        StandIn server([&pretend](const Asked& request) { return pretend.answer(request); });
        Desk desk;
        desk.hasKept(server);
        ui::AppLock lock(desk.secrets, desk.folder.filePath(u"locked"_s));
        core::Incidents incidents(desk.folder.filePath(u"incidents"_s));
        ui::AlertsView alerts(incidents, lock);
        desk.session.useAlerts(alerts);
        desk.session.begin();
        QTRY_VERIFY(server.count("/api/v1/status") >= 3);
        pretend.user.replace("\"token\": false", "\"token\": true");
        QTRY_VERIFY(server.count("/api/v1/session") >= 2);
        QTest::qWait(100);
        const int before = server.count("/api/v1/status");
        QTest::qWait(400);
        QVERIFY(server.count("/api/v1/status") <= before + 1);
        QCOMPARE(server.count("/api/v1/display"), 0);
    }

    void backgroundServersDoNotBypassMissingCredentialsOrUnknownCertificates()
    {
        Pretended expired;
        Pretended missing;
        Pretended secure;
        secure.signedIn = true;
        StandIn one([&expired](const Asked& request) { return expired.answer(request); });
        StandIn two([&missing](const Asked& request) { return missing.answer(request); });
        StandIn three([&secure](const Asked& request) { return secure.answer(request); }, tls("first"));
        Desk desk;
        ui::AppLock lock(desk.secrets, desk.folder.filePath(u"locked"_s));
        core::Incidents incidents(desk.folder.filePath(u"incidents"_s));
        ui::AlertsView alerts(incidents, lock);
        desk.hasKept(one);
        desk.hasKept(three);
        auto saved = desk.settings.remembered();
        saved.servers = {one.address() + u'/', two.address() + u'/', three.address() + u'/'};
        QVERIFY(desk.settings.remember(saved));
        desk.session.useAlerts(alerts);
        desk.session.begin();
        QTRY_COMPARE(one.count("/api/v1/session"), 1);
        QTest::qWait(500);
        QCOMPARE(one.count("/api/v1/login"), 0);
        QCOMPARE(one.count("/api/v1/status"), 0);
        QVERIFY(two.asked.isEmpty());
        QCOMPARE(three.count("/api/v1/session"), 0);
        QVERIFY(!desk.secrets.all.contains(Desk::nameFor(one)));
        QVERIFY(desk.settings.remembered().certificates.isEmpty());
        QCOMPARE(desk.view.stage(), u"address"_s);
    }

    void savedServersAreMonitoredTogetherAndNotificationsOpenTheCorrectAccountAndMachine()
    {
        Pretended first;
        Pretended second;
        first.signedIn = true;
        second.signedIn = true;
        StandIn one([&first](const Asked& request) { return first.answer(request); });
        StandIn two([&second](const Asked& request) { return second.answer(request); });
        std::unique_ptr<core::LocalWall> own;
        Desk desk;
        own = std::make_unique<core::LocalWall>(desk.settings, desk.secrets, quickKvms());
        const auto fast = [](QByteArrayView text, QByteArrayView salt,
                             const std::stop_token&) -> base::Result<QByteArray> {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(salt);
            hash.addData(text);
            return hash.result();
        };
        ui::AppLock lock(desk.secrets, fast, [] { return 1000; });
        lock.submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!lock.restricted());
        desk.hasKept(one);
        desk.hasKept(two);
        auto saved = desk.settings.remembered();
        saved.showsOwnWall = true;
        saved.server = two.address() + u'/';
        // Earlier builds kept both credential names but only the last server in settings.
        QVERIFY(desk.settings.remember(saved));
        core::Incidents incidents(desk.folder.filePath(u"incidents"_s));
        ui::AlertsView alerts(incidents, lock);
        desk.session.useOwnWall(*own);
        desk.session.useAppLock(lock);
        desk.session.useAlerts(alerts);
        ui::QuickAccess shortcuts(desk.settings, alerts, lock);
        desk.session.useQuickAccess(shortcuts);
        desk.session.remove(desk.wall);
        desk.session.add(desk.wall, desk.actions, nullptr);
        QSignalSpy notifications(&alerts, &ui::AlertsView::notification);
        QSignalSpy unavailable(&desk.session, &ui::ServerSession::incidentUnavailable);
        desk.session.begin();
        QTRY_VERIFY(one.count("/api/v1/status") >= 4 && two.count("/api/v1/status") >= 4);
        QVERIFY(desk.view.ownWall());
        QCOMPARE(desk.settings.remembered().server, two.address() + u'/');
        QCOMPARE(alerts.sources().size(), 3);
        QCOMPARE(desk.settings.remembered().servers.size(), 2);
        QVERIFY(shortcuts.recent().isEmpty()); // Background monitoring is not a visit.
        first.status.replace("\"a\": {\"state\": \"online\"}", "\"a\": {\"state\": \"offline\"}");
        QTRY_COMPARE(notifications.size(), 1);
        const QString scope = notifications.front().at(3).toString();
        QCOMPARE(notifications.front().at(4).toString(), u"a"_s);
        QCOMPARE(one.count("/api/v1/devices/a/snapshot"), 0);
        lock.lock();
        desk.session.openIncident(scope, u"a"_s, desk.wall);
        QVERIFY(desk.view.ownWall());
        QVERIFY(desk.wall.enlarged().isEmpty());
        lock.submit(u"example-password"_s, {}, {});
        QTRY_VERIFY(!lock.restricted());
        QTRY_COMPARE(desk.wall.enlarged(), Desk::key(u"a"_s));
        QCOMPARE(desk.view.address(), Desk::authorityOf(one));
        QTRY_COMPARE(shortcuts.recent().size(), 1);
        QCOMPARE(shortcuts.recent().front().reference.scope, scope);
        QCOMPARE(shortcuts.recent().front().reference.machine, u"a"_s);
        desk.wall.toggleFavorite(Desk::key(u"a"_s));
        QTRY_VERIFY(desk.wall.favoriteKeys().contains(Desk::key(u"a"_s)));
        Window other;
        desk.session.add(other.wall, other.actions, nullptr);
        QVERIFY(other.wall.favoriteKeys().contains(Desk::key(u"a"_s)));
        other.wall.toggleFavorite(Desk::key(u"a"_s));
        QVERIFY(desk.wall.favoriteKeys().isEmpty());
        desk.wall.toggleFavorite(Desk::key(u"a"_s));
        desk.session.remove(other.wall);
        const int otherLooked = two.count("/api/v1/status");
        QTRY_VERIFY(two.count("/api/v1/status") >= otherLooked + 2);
        QCOMPARE(unavailable.size(), 0);
        desk.wall.closeEnlarged();
        desk.session.openIncident(scope, u"removed"_s, desk.wall);
        QTRY_COMPARE(unavailable.size(), 1);
        QVERIFY(desk.wall.enlarged().isEmpty());
        first.user.replace("\"id\": \"u1\"", "\"id\": \"u2\"");
        desk.session.openIncident(scope, u"a"_s, desk.wall);
        QTRY_COMPARE(unavailable.size(), 2);
        QVERIFY(desk.wall.enlarged().isEmpty());
        QVERIFY(shortcuts.favorites().isEmpty());
        QVERIFY(shortcuts.recent().isEmpty()); // The old account's IDs cannot alias the new account.
        QCOMPARE(one.count("/api/v1/devices/a/actions"), 0);
        desk.view.signOut();
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        const int stopped = one.count("/api/v1/status");
        QTest::qWait(350);
        QVERIFY(one.count("/api/v1/status") <= stopped + 1);
        QVERIFY(!desk.settings.remembered().servers.contains(one.address() + u'/'));
        desk.session.openIncident(scope, u"a"_s, desk.wall);
        QCOMPARE(unavailable.size(), 3);
        desk.session.stopForReset();
        const int reset = two.count("/api/v1/status");
        QTest::qWait(350);
        QVERIFY(two.count("/api/v1/status") <= reset + 1);
        desk.session.remove(desk.wall);
    }

    void liveIdentitySeparatesAddressesAndSignIns()
    {
        Desk desk;
        ui::ServerSource first(u"one"_s, desk.settings, desk.secrets);
        ui::ServerSource second(u"two"_s, desk.settings, desk.secrets);
        auto& one = static_cast<core::ServerClient&>(first.place());
        auto& two = static_cast<core::ServerClient&>(second.place());
        QVERIFY(one.setAddress(u"http://127.0.0.1:12340/"_s));
        QVERIFY(two.setAddress(u"http://127.0.0.1:12340/"_s));
        one.setSession("example-one");
        two.setSession("example-one");
        QCOMPARE(first.liveIdentity(), second.liveIdentity());
        two.setSession("example-two");
        QVERIFY(first.liveIdentity() != second.liveIdentity());
        two.setSession("example-one");
        QVERIFY(two.setAddress(u"http://127.0.0.1:12341/"_s));
        two.setSession("example-one");
        QVERIFY(first.liveIdentity() != second.liveIdentity());
        QCOMPARE(first.liveIdentity().size(), 64);
        QVERIFY(!first.liveIdentity().contains(u"example-one"_s));
    }

    void independentSessionsShareMonitoringAndSignOutOnlyTheSameServer()
    {
        Pretended first;
        Pretended second;
        first.signedIn = true;
        first.giveLive();
        second.signedIn = true;
        StandIn one([&](const Asked& request) {
            auto answer = first.answer(request);
            if (request.method == "POST" && request.path.endsWith("/actions"))
                answer.never = true;
            return answer;
        });
        StandIn two([&](const Asked& request) { return second.answer(request); });
        std::unique_ptr<core::LocalWall> own;
        Desk desk;
        own = std::make_unique<core::LocalWall>(desk.settings, desk.secrets, quickKvms());
        ui::AppLock lock(
            desk.secrets,
            [](QByteArrayView text, QByteArrayView salt, const std::stop_token&) {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                hash.addData(text);
                hash.addData(salt);
                return base::Result<QByteArray>(hash.result());
            },
            [] { return 1000; });
        lock.submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!lock.restricted());
        desk.hasKept(one);
        desk.hasKept(two);
        core::Incidents incidents(desk.folder.filePath(u"incidents"_s));
        ui::AlertsView alerts(incidents, lock);
        ui::QuickAccess shortcuts(desk.settings, alerts, lock);
        desk.session.useOwnWall(*own);
        desk.session.useAppLock(lock);
        desk.session.useAlerts(alerts);
        desk.session.useQuickAccess(shortcuts);
        Window left;
        Window right;
        Window other;
        ui::SessionView leftView;
        ui::SessionView rightView;
        ui::SessionView otherView;
        core::WindowSlot leftSlot(desk.settings, 0);
        core::WindowSlot rightSlot(desk.settings, 1);
        core::WindowSlot otherSlot(desk.settings, 2);
        auto leftSession = desk.session.makeWindow(leftView, left.wall, leftSlot);
        auto rightSession = desk.session.makeWindow(rightView, right.wall, rightSlot);
        auto otherSession = desk.session.makeWindow(otherView, other.wall, otherSlot);
        leftSession->add(left.wall, left.actions, nullptr);
        rightSession->add(right.wall, right.actions, nullptr);
        otherSession->add(other.wall, other.actions, nullptr);
        desk.session.begin();
        leftSession->beginWindow();
        rightSession->beginWindow();
        otherSession->beginWindow();
        leftView.chooseSource(one.address() + u'/');
        rightView.chooseSource(one.address() + u'/');
        otherView.chooseSource(two.address() + u'/');
        QTRY_COMPARE(left.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(right.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(other.wall.machines()->rowCount(), 3);
        const auto scope = leftSession->selectedScope();
        QTRY_VERIFY(!scope.isEmpty());
        left.wall.toggleFavorite(Desk::key(u"a"_s));
        QTRY_VERIFY(right.wall.favoriteKeys().contains(Desk::key(u"a"_s)));
        QVERIFY(other.wall.favoriteKeys().isEmpty());
        const auto treeMachines = [](const QVariantList& rows) {
            return std::count_if(rows.cbegin(), rows.cend(),
                                 [](const auto& row) { return row.toMap().value(u"depth"_s).toInt() == 2; });
        };
        QTRY_COMPARE(treeMachines(leftView.sourceTree()), 6);
        QCOMPARE(leftView.sourceTree(), rightView.sourceTree());
        rightView.openMachine(scope, u"a"_s);
        QTRY_COMPARE(right.wall.enlarged(), Desk::key(u"a"_s));
        QVERIFY(left.wall.enlarged().isEmpty());
        QVERIFY(other.wall.enlarged().isEmpty());
        left.wall.setEnlargedPixels(1000);
        right.wall.setEnlargedPixels(1400);
        left.wall.enlarge(Desk::key(u"a"_s));
        QTRY_VERIFY(one.live.isConnected());
        QTRY_COMPARE(one.count("/api/v1/devices/a/live"), 1);
        QTest::qWait(400); // Both independent windows have passed their live-start delay.
        QCOMPARE(one.count("/api/v1/devices/a/live"), 1);
        left.wall.setUnseen(true);
        QTest::qWait(50);
        QVERIFY(one.live.isConnected());
        lock.lock();
        QVERIFY(leftView.sourceTree().isEmpty());
        QVERIFY(rightView.sourceTree().isEmpty());
        QVERIFY(!left.actions.allowed());
        QVERIFY(!right.actions.allowed());
        QVERIFY(!other.actions.allowed());
        rightView.chooseSource(two.address() + u'/');
        QCOMPARE(rightView.source(), QString());
        QCOMPARE(rightSession->selectedSource(), one.address() + u'/');
        lock.ask(u"unlock"_s);
        lock.submit(u"example-password"_s, {}, {});
        QTRY_VERIFY(!lock.restricted());
        QVERIFY(right.actions.allowed());
        QTRY_COMPARE(onTheCardOf(left.wall, Desk::key(u"a"_s), WallModel::StateRole), u"online"_s);
        left.actions.open(Desk::key(u"a"_s));
        left.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(one.count("/api/v1/devices/a/actions"), 1);
        right.actions.open(Desk::key(u"a"_s));
        right.actions.choose(u"keys:ShiftLeft"_s);
        QVERIFY(!right.actions.busy()); // Another window already has a command pending on this machine.
        other.actions.open(Desk::key(u"a"_s));
        other.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(two.count("/api/v1/devices/a/actions"), 1); // Same ID at another server is independent.
        QCOMPARE(one.count("/api/v1/devices/a/actions"), 1);
        leftView.signOut();
        QTRY_COMPARE(treeMachines(leftView.sourceTree()), 3);
        QTRY_VERIFY(!one.live.isConnected());
        QVERIFY(!right.actions.allowed());
        QVERIFY(other.actions.allowed());
        QCOMPARE(other.wall.machines()->rowCount(), 3);
        QCOMPARE(one.count("/api/v1/devices/a/actions"), 1);
        QTRY_VERIFY(!first.signedIn);
        rightView.connectTo(one.address());
        QTRY_COMPARE(rightView.stage(), u"signIn"_s);
        rightView.signIn({}, u"the right one"_s, {}, false);
        QTRY_COMPARE(right.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(shortcuts.favorites().size(), 1);
        QVERIFY(!desk.settings.remembered().servers.contains(one.address() + u'/'));
        leftView.chooseSource(two.address() + u'/');
        QTRY_COMPARE(left.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(leftSession->selectedScope(), otherSession->selectedScope());
        rightView.chooseSource(two.address() + u'/');
        QTRY_COMPARE(right.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(rightSession->selectedScope(), otherSession->selectedScope());
        QTRY_VERIFY(shortcuts.favorites().isEmpty()); // Leaving an unsaved source for one already observed.
        rightView.goToOwnWall();
        QTRY_VERIFY(shortcuts.favorites().isEmpty()); // No saved sign-in remains to monitor this source.
        leftSession->remove(left.wall);
        rightSession->remove(right.wall);
        otherSession->remove(other.wall);
    }

    void localIncidentsContinueWhileAServerWallIsSelected()
    {
        MadeUpKvm kvm;
        kvm.signal = false;
        std::unique_ptr<core::LocalWall> own;
        Desk desk;
        own = std::make_unique<core::LocalWall>(desk.settings, desk.secrets, quickKvms());
        const auto added = own->add(at(kvm, u"Background KVM"_s), kvm.password);
        QVERIFY(added.ok());
        ui::AppLock lock(desk.secrets, desk.folder.filePath(u"locked"_s));
        core::Incidents incidents(desk.folder.filePath(u"incidents"_s));
        ui::AlertsView alerts(incidents, lock);
        desk.session.useOwnWall(*own);
        desk.session.useAlerts(alerts);
        desk.view.goToOwnWall();
        QTRY_COMPARE(incidents.entries(u"pc"_s).size(), 1);
        Pretended pretend;
        StandIn server([&pretend](const Asked& request) { return pretend.answer(request); });
        desk.hasKept(server);
        pretend.signedIn = true;
        desk.view.anotherServer();
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!desk.view.ownWall());
        kvm.signal = true;
        QTRY_VERIFY_WITH_TIMEOUT(incidents.entries(u"pc"_s).front().closed.isValid(), 6000);
    }

    void localWallFeedsIncidentsAndClosesRemovedDevicesWithoutInventingRecovery()
    {
        MadeUpKvm kvm;
        kvm.signal = false;
        std::unique_ptr<core::LocalWall> own;
        Desk desk;
        const auto fast = [](QByteArrayView text, QByteArrayView salt,
                             const std::stop_token&) -> base::Result<QByteArray> {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(salt);
            hash.addData(text);
            return hash.result();
        };
        ui::AppLock lock(desk.secrets, fast, [] { return 1000; });
        lock.submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!lock.restricted());
        // Establish the empty profile before saving any device credentials.
        own = std::make_unique<core::LocalWall>(desk.settings, desk.secrets, quickKvms());
        const auto added = own->add(at(kvm, u"Test KVM"_s), kvm.password);
        QVERIFY(added.ok());
        core::Incidents incidents(desk.folder.filePath(u"incidents"_s));
        ui::AlertsView alerts(incidents, lock);
        desk.session.useOwnWall(*own);
        desk.session.useAlerts(alerts);
        desk.view.goToOwnWall();
        QTRY_COMPARE(alerts.entries().size(), 1);
        QCOMPARE(incidents.entries(u"pc"_s).front().key, added.value());
        QCOMPARE(incidents.entries(u"pc"_s).front().state, u"no_signal"_s);
        QVERIFY(own->remove(added.value()).ok());
        QTRY_VERIFY(alerts.entries().isEmpty());
        alerts.setHistory(true);
        QCOMPARE(alerts.entries().size(), 1);
        QVERIFY(incidents.entries(u"pc"_s).front().removed);
    }

    void freshStatusDrivesPersistentIncidentsWhileHiddenAndStopsOnSignOut()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        const auto fast = [](QByteArrayView text, QByteArrayView salt,
                             const std::stop_token&) -> base::Result<QByteArray> {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(salt);
            hash.addData(text);
            return hash.result();
        };
        ui::AppLock lock(desk.secrets, fast, [] { return 1000; });
        lock.submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!lock.restricted());
        core::Incidents incidents(desk.folder.filePath(u"incidents"_s));
        ui::AlertsView alerts(incidents, lock);
        desk.session.useAlerts(alerts);
        QSignalSpy notifications(&alerts, &ui::AlertsView::notification);
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(alerts.entries().size(), 1); // Initially no signal: quietly recorded.
        QCOMPARE(notifications.size(), 0);
        desk.wall.setUnseen(true);
        qkvm.status.replace("\"a\": {\"state\": \"online\"}", "\"a\": {\"state\": \"offline\"}");
        QTRY_COMPARE(alerts.entries().size(), 2);
        QCOMPARE(notifications.size(), 1);
        lock.lock();
        qkvm.status.replace("\"a\": {\"state\": \"offline\"}", "\"a\": {\"state\": \"online\"}");
        const int before = server.count("/api/v1/status");
        QTRY_VERIFY(server.count("/api/v1/status") >= before + 4);
        QVERIFY(alerts.entries().isEmpty());
        QCOMPARE(notifications.size(), 1);
        QVERIFY(incidents.flush());
        const QJsonObject saved = written(desk.folder.filePath(u"incidents"_s));
        int closed = 0;
        for (const auto& entry : saved[u"entries"_s].toArray())
            closed += !entry.toObject()[u"closed"_s].toString().isEmpty() ? 1 : 0;
        QCOMPARE(closed, 1);
        desk.view.signOut();
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!alerts.allowed());
        const int stopped = server.count("/api/v1/status");
        QTest::qWait(300);
        QCOMPARE(server.count("/api/v1/status"), stopped);
    }

    void initTestCase()
    {
        // Whatever speaks over TLS here chooses Windows' own, which is refused once TLS is in
        // use: so one is made before anything else touches TLS (the stand-ins do).
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void theServerIsAskedWhoIsSignedInAndThenForAPassword()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::SessionView& view = desk.view;
        QCOMPARE(view.stage(), u"address"_s);

        view.connectTo(server.address());
        QVERIFY(view.busy());
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        QVERIFY(!view.busy());
        QCOMPARE(view.problem(), QString());
        QCOMPARE(view.address(), u"127.0.0.1:%1"_s.arg(server.serverPort()));
        QVERIFY(!view.encrypted());
        QVERIFY(!view.asksName());
        QVERIFY(!view.asksCode());
    }

    void aServerIsAskedWhatItSpeaksBeforeAnythingElseAndBeforeEveryPassword()
    {
        // (This test had it asked once for each time a server is come to, and a password posted
        // on the strength of that: what answered there by then was asked nothing first, which
        // was the defect.)
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        // Somebody stayed signed in here once. Who answers at the address now is not known yet.
        desk.hasKept(server, "kept-from-before");
        desk.view.connectTo(server.address());
        QVERIFY(desk.view.busy());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        // First what it is, without the session; then who is signed in, with it. And no more.
        QCOMPARE(server.asked.size(), 2);
        QCOMPARE(server.asked[0].method, QByteArray("GET"));
        QCOMPARE(server.asked[0].path, QByteArray("/healthz"));
        QVERIFY(!server.asked[0].headers.contains("cookie"));
        QCOMPARE(server.asked[1].path, QByteArray("/api/v1/session"));
        QCOMPARE(server.asked[1].headers.value("cookie"), QByteArray("qkvm_session=kept-from-before"));

        // Once for each time a server is come to, and once more before each password that goes
        // to it: not with the looks at the machines, and not with a sign-out.
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_VERIFY(server.count("/api/v1/status") >= 5);
        QCOMPARE(server.count("/healthz"), 2);
        desk.view.signOut();
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_VERIFY(server.count("/api/v1/wall") >= 2);
        QCOMPARE(server.count("/healthz"), 3);
        // Each password went out behind such a question of its own, and none of those carried
        // anything that says who asks.
        int passwords = 0;
        qsizetype lastOfItself = -1;
        qsizetype lastPassword = -1;
        for (qsizetype at = 0; at < server.asked.size(); ++at) {
            const Asked& one = server.asked[at];
            if (one.path == "/healthz") {
                QVERIFY(!one.headers.contains("cookie"));
                QVERIFY(one.body.isEmpty());
                lastOfItself = at;
            } else if (one.path == "/api/v1/login") {
                ++passwords;
                QVERIFY(lastOfItself > lastPassword);
                lastPassword = at;
            }
        }
        QCOMPARE(passwords, 2);
        // Come to anew, it is asked anew. (Nobody signed out: the wall again.)
        desk.view.anotherServer();
        QCOMPARE(desk.view.stage(), u"address"_s);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(server.count("/healthz"), 4);
    }

    void whatAnswersAndIsNoQkvmIsSaidWhereItsAddressWasTypedAndAskedNothingButWhatItIs_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("a page, whatever is asked") << 200 << QByteArray("<html>A router</html>");
        QTest::newRow("nothing at that place") << 404 << QByteArray("<html>Not Found</html>");
        QTest::newRow("something that wants a sign-in first")
            << 401 << QByteArray(R"({"code": "unauthorized", "message": "Sign in first"})");
        QTest::newRow("a qKVM from before its API had versions")
            << 200 << QByteArray(R"({"status": "ok", "version": "0.0.9", "locked": false})");
    }

    void whatAnswersAndIsNoQkvmIsSaidWhereItsAddressWasTypedAndAskedNothingButWhatItIs()
    {
        // (Named "... and asked nothing more" before a screen that opens by its link was asked
        // again by itself.)
        QFETCH(const int, status);
        QFETCH(const QByteArray, body);
        const QString notAQkvm = u"The server answered in a way this program does not understand."_s;
        {
            // Somebody stayed signed in at the QKVM that was at this address.
            StandIn server([&](const Asked&) { return Answered{status, body}; });
            Desk desk;
            desk.hasKept(server);
            desk.view.connectTo(server.address());
            QVERIFY(desk.view.busy());
            QTRY_VERIFY(!desk.view.busy());
            QCOMPARE(desk.view.stage(), u"address"_s);
            QCOMPARE(desk.view.problem(), notAQkvm);
            QVERIFY(!desk.view.locked());
            // It was asked what it is, and nothing after that: the session did not go to it, and
            // is still kept.
            QTest::qWait(300);
            QCOMPARE(server.asked.size(), 1);
            QCOMPARE(server.asked[0].path, QByteArray("/healthz"));
            QVERIFY(!server.asked[0].headers.contains("cookie"));
            QCOMPARE(desk.secrets.all.size(), 1);
            QVERIFY(!desk.view.tryingAgain());
            // Nor is it remembered as the server that was used.
            QCOMPARE(desk.settings.remembered().server, QString());
        }
        // A display link is kept for it as well: a screen nobody sits at. (This test had such a
        // screen asked once too, and left at the line for an address for good: where its server
        // was only on its way up behind a gateway, it waited for a person who never came. It
        // is asked again by itself now. With the one question that says nothing of who asks,
        // and nothing else: neither the session nor the link goes to what is no QKVM.)
        StandIn server([&](const Asked&) { return Answered{status, body}; });
        Desk desk;
        desk.hasKept(server);
        desk.secrets.all.insert(Desk::linkNameFor(server), "qkvm_" + QByteArray(43, 'd'));
        desk.view.connectTo(server.address());
        QVERIFY(desk.view.busy());
        QTRY_VERIFY(!desk.view.busy());
        QCOMPARE(desk.view.stage(), u"address"_s);
        QCOMPARE(desk.view.problem(), notAQkvm);
        QVERIFY(!desk.view.locked());
        QTRY_VERIFY(server.asked.size() >= 4);
        QVERIFY(desk.view.tryingAgain());
        for (const Asked& one : std::as_const(server.asked)) {
            QCOMPARE(one.path, QByteArray("/healthz"));
            QVERIFY(!one.headers.contains("cookie"));
            QVERIFY(one.body.isEmpty());
        }
        QCOMPARE(desk.secrets.all.size(), 2);
        QCOMPARE(desk.settings.remembered().server, QString());
    }

    void aQkvmThatSpeaksAnotherVersionOfItsApiIsSaidAndAskedNothingMore()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.apis = R"(["v2", "v3"])";
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(!desk.view.busy());
        QCOMPARE(desk.view.stage(), u"address"_s);
        QCOMPARE(
            desk.view.problem(),
            u"This qKVM speaks v2 and v3 of its API, and this program speaks v1. One of the two has to be updated."_s);
        // Nothing more was asked of it, though somebody is signed in to it: not under the version
        // it does speak, and not under this program's.
        QTest::qWait(300);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].path, QByteArray("/healthz"));
        QVERIFY(!server.asked[0].headers.contains("cookie"));
        QCOMPARE(desk.wall.machines()->rowCount(), 0);
        // What is kept for it stays kept: it is the same QKVM once one of the two was updated.
        QVERIFY(desk.secrets.all.contains(Desk::nameFor(server)));

        // Updated, it speaks this program's version beside its own: straight to the wall.
        qkvm.apis = R"(["v1", "v2", "v3"])";
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.problem(), QString());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
    }

    void aQkvmThatComesBackSpeakingAnotherVersionOfItsApiIsSaidWhereAServerIsNamed()
    {
        // Somebody stayed signed in, and the wall is up. The QKVM is updated: for a moment
        // nothing answers at its address, and what comes back speaks another version of its
        // API. That is said where a server is named, as when such a one is named first; not
        // by a wall that says "contact lost" for as long as it is left standing.
        Pretended qkvm;
        qkvm.signedIn = true;
        bool down = false;
        StandIn server([&](const Asked& asked) {
            Answered answer = qkvm.answer(asked);
            answer.hangUp = down;
            return answer;
        });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        down = true;
        QTRY_VERIFY(!desk.view.inContact());
        qkvm.apis = R"(["v2"])";
        down = false;
        QTRY_COMPARE(desk.view.stage(), u"address"_s);
        QCOMPARE(desk.view.problem(),
                 u"This qKVM speaks v2 of its API, and this program speaks v1. One of the two has to be updated."_s);
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.wall.machines()->rowCount(), 0);
        // It is asked no more. What is kept for it stays kept: it is the same QKVM once one of
        // the two was updated.
        QTest::qWait(200);
        const qsizetype asked = server.asked.size();
        QTest::qWait(500);
        QCOMPARE(server.asked.size(), asked);
        QCOMPARE(desk.secrets.all.value(Desk::nameFor(server)), QByteArray("abc123"));
    }

    void aQkvmThatWasRestartedLockedSaysSoWhereSomebodySignsInUntilSomebodyHas()
    {
        Pretended qkvm;
        qkvm.locked = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::SessionView& view = desk.view;
        QVERIFY(!view.locked());
        view.connectTo(server.address());
        // Not while it is being asked: only where somebody signs in.
        QVERIFY(!view.locked());
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        QVERIFY(view.locked());
        QCOMPARE(view.problem(), QString());
        // A wrong password changes nothing about it.
        view.signIn(QString(), u"a wrong one"_s, QString(), false);
        QTRY_VERIFY(!view.busy());
        QCOMPARE(view.stage(), u"signIn"_s);
        QVERIFY(view.locked());
        // Somebody has signed in: it is said no more, also where they sign out again. (Whether
        // it is locked by then would be a guess: it is asked when it is come to, and again
        // when a password is about to go to it.)
        view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(view.stage(), u"wall"_s);
        QVERIFY(!view.locked());
        view.signOut();
        QCOMPARE(view.stage(), u"signIn"_s);
        QVERIFY(!view.locked());

        // Another QKVM, which is not locked, says nothing of the kind; and what the first said
        // is not said of the second while that one is still being asked.
        Pretended otherQkvm;
        StandIn other([&otherQkvm](const Asked& asked) { return otherQkvm.answer(asked); });
        qkvm.locked = true;
        view.anotherServer();
        view.connectTo(server.address());
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        QVERIFY(view.locked());
        view.anotherServer();
        QVERIFY(!view.locked());
        view.connectTo(other.address());
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        QVERIFY(!view.locked());
    }

    void afterACertificateWasSaidYesToTheServerIsAskedWhatItSpeaksFirst()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.apis = R"(["v7"])";
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("first"));
        QVERIFY(server.isListening());
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"trust"_s);
        QCOMPARE(server.asked.size(), 0);
        // Yes to its certificate: now it is asked what it is, and it is none this program speaks to.
        desk.view.trustCertificate();
        QTRY_VERIFY(!desk.view.busy());
        QCOMPARE(desk.view.stage(), u"address"_s);
        QCOMPARE(desk.view.problem(),
                 u"This qKVM speaks v7 of its API, and this program speaks v1. One of the two has to be updated."_s);
        QTest::qWait(300);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].path, QByteArray("/healthz"));
        QVERIFY(!server.asked[0].headers.contains("cookie"));
    }

    void whatAnswersWhereAScreensQkvmWasIsNotHandedItsLink_data()
    {
        QTest::addColumn<bool>("isAQkvm");
        QTest::addColumn<QString>("sentence");
        // (Of that nothing is said: the screen goes on asking.)
        QTest::newRow("no qKVM just now") << false << QString();
        QTest::newRow("a qKVM of another version")
            << true
            << u"This qKVM speaks v2 of its API, and this program speaks v1. One of the two has to be updated."_s;
    }

    void whatAnswersWhereAScreensQkvmWasIsNotHandedItsLink()
    {
        QFETCH(const bool, isAQkvm);
        QFETCH(const QString, sentence);
        Pretended qkvm;
        // What stands at the address later, where it is no QKVM: something that wants a sign-in of
        // its own for whatever is asked.
        bool replaced = false;
        StandIn server([&](const Asked& asked) {
            return replaced && !isAQkvm ? Answered{401, "<html>Sign in to go on</html>", "text/html"}
                                        : qkvm.answer(asked);
        });
        const auto lastAsked = [&server](const QByteArray& path) {
            qsizetype found = -1;
            for (qsizetype at = 0; at < server.asked.size(); ++at) {
                found = server.asked[at].path == path ? at : found;
            }
            return found;
        };
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QCOMPARE(server.count("/api/v1/display"), 1);
        QCOMPARE(server.count("/healthz"), 1);
        // The way a screen comes back, where it is a QKVM this program speaks to that answers:
        // asked once more what it is, without a session, and only then handed the link.
        const qsizetype beforeTheRestart = server.asked.size();
        qkvm.signedIn = false;
        QTRY_COMPARE(qkvm.displaysOpened, 2);
        QTRY_VERIFY(desk.view.inContact());
        QCOMPARE(server.count("/healthz"), 2);
        QVERIFY(lastAsked("/healthz") >= beforeTheRestart);
        QVERIFY(lastAsked("/healthz") < lastAsked("/api/v1/display"));
        QVERIFY(!server.asked[lastAsked("/healthz")].headers.contains("cookie"));

        // The server is restarted once more, and what answers at its address then is something
        // else, or a QKVM of another version.
        replaced = true;
        qkvm.apis = R"(["v2"])";
        qkvm.signedIn = false;
        if (!isAQkvm) {
            // Something else answers there: a gateway's page while the server is on its way up, as
            // likely as not. Nobody sits at a wall screen to name its server again: it stays at
            // its wall, says that the server is away, and goes on asking what is there, with
            // nothing that says who asks. The link is handed to nothing meanwhile.
            QTRY_VERIFY_WITH_TIMEOUT(!desk.view.inContact(), 20000);
            const int asked = server.count("/healthz");
            QTRY_VERIFY_WITH_TIMEOUT(server.count("/healthz") >= asked + 3, 20000);
            QCOMPARE(desk.view.stage(), u"wall"_s);
            QVERIFY(!desk.view.inContact());
            QCOMPARE(desk.view.problem(), QString());
            QCOMPARE(server.count("/api/v1/display"), 2);
            QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
            // The QKVM is back: so is the screen, by itself.
            replaced = false;
            qkvm.apis = R"(["v1"])";
            QTRY_COMPARE_WITH_TIMEOUT(qkvm.displaysOpened, 3, 20000);
            QTRY_VERIFY(desk.view.inContact());
            QCOMPARE(desk.view.stage(), u"wall"_s);
            QCOMPARE(desk.view.account(), u"Lobby screen"_s);
            QCOMPARE(sentence, QString());
            return;
        }
        // A QKVM that says it speaks another version: one of the two has to be updated, and that
        // is said where a server is named.
        QTRY_COMPARE_WITH_TIMEOUT(desk.view.stage(), u"address"_s, 20000);
        QCOMPARE(desk.view.problem(), sentence);
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.wall.machines()->rowCount(), 0);
        QCOMPARE(desk.view.account(), QString());
        // The link was not handed to it, and is not, however long it stands there; and it is
        // still kept for the QKVM it is for.
        QTest::qWait(500);
        QCOMPARE(server.count("/api/v1/display"), 2);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);

        // The QKVM is back as it was: named by its address alone, the screen is in again with
        // the link that is kept for it.
        replaced = false;
        qkvm.apis = R"(["v1"])";
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(qkvm.displaysOpened, 3);
        QCOMPARE(desk.view.account(), u"Lobby screen"_s);
        // (No person: a screen on the wall, as the account's drop says it.)
        QCOMPARE(desk.view.role(), u"display"_s);
    }

    void whoIsSignedInMayActOnTheWallsMachines()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        // Nobody is signed in: nobody may.
        QVERIFY(!desk.actions.allowed());
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!desk.actions.allowed());
        // An admin signs in.
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(desk.actions.allowed());
        // The wall's machines have their menus.
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        desk.actions.open(desk.key(u"b"_s));
        QCOMPARE(desk.actions.menu().value(u"title"_s).toString(), u"NAS"_s);

        // Signed out: nobody may any more, and nothing is left open.
        desk.view.signOut();
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!desk.actions.allowed());
        QVERIFY(desk.actions.menu().isEmpty());
        desk.actions.open(desk.key(u"b"_s));
        QVERIFY(desk.actions.menu().isEmpty());
    }

    // b10: each machine is shown large at the size last chosen for it, kept on this PC for the server's
    // machines by the server and the account (as a favorite is), so that the next start has it too; a
    // machine shown at "fit" keeps nothing.
    void eachMachineIsShownLargeAtTheSizeLastChosenForIt()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        desk.wall.enlarge(desk.key(u"a"_s));
        QCOMPARE(desk.wall.enlargedScale(), u"fit"_s);
        desk.wall.chooseScale(u"140"_s);
        QCOMPARE(desk.wall.enlargedScale(), u"140"_s);
        QCOMPARE(desk.settings.remembered().pictureScales.size(), 1);
        const core::MachineScale kept = desk.settings.remembered().pictureScales.front();
        QCOMPARE(kept.machine.machine, u"a"_s);
        QVERIFY(kept.machine.scope.startsWith(u"server-"_s));
        QCOMPARE(kept.scale, u"140"_s);
        // Another machine: as fitted; and back: as it was chosen.
        desk.wall.stepEnlarged(1);
        QVERIFY(desk.wall.enlarged() != desk.key(u"a"_s));
        QCOMPARE(desk.wall.enlargedScale(), u"fit"_s);
        desk.wall.chooseScale(u"75"_s);
        desk.wall.closeEnlarged();
        QCOMPARE(desk.wall.enlargedScale(), u"fit"_s);
        desk.wall.enlarge(desk.key(u"a"_s));
        QCOMPARE(desk.wall.enlargedScale(), u"140"_s);
        // (The one chosen last first.)
        QCOMPARE(desk.settings.remembered().pictureScales.size(), 2);
        QCOMPARE(desk.settings.remembered().pictureScales.front().scale, u"75"_s);
        // What is no scale is not taken; "fit" keeps nothing.
        desk.wall.chooseScale(u"150"_s);
        QCOMPARE(desk.wall.enlargedScale(), u"140"_s);
        desk.wall.chooseScale(u"fit"_s);
        QCOMPARE(desk.wall.enlargedScale(), u"fit"_s);
        QCOMPARE(desk.settings.remembered().pictureScales.size(), 1);
        QVERIFY(desk.settings.remembered().pictureScales.front().machine.machine != u"a"_s);
    }

    void whoIsSignedInAndWhatTheyMayDoIsAskedAgainNowAndThen()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QVERIFY(desk.actions.allowed());
        QVERIFY(desk.view.manages());
        QCOMPARE(desk.view.account(), u"anna"_s);
        // (And what the server lets her do, as the account's drop says it.)
        QCOMPARE(desk.view.role(), u"admin"_s);
        desk.actions.open(desk.key(u"b"_s));
        QVERIFY(!desk.actions.menu().isEmpty());
        desk.wall.setSelecting(true);

        // Elsewhere an admin makes anna somebody who may only look. The page asks who is signed in
        // about once a minute, with every fifteenth look at the machines; so does this, and this
        // desk looks ten times a second.
        qkvm.user = R"({"id": "u1", "name": "anna", "role": "viewer", "token": false})";
        QTRY_VERIFY_WITH_TIMEOUT(!desk.actions.allowed(), 8000);
        // What was open is closed, nothing is being picked, and the QKVM's own page is not offered.
        QVERIFY(desk.actions.menu().isEmpty());
        QVERIFY(!desk.wall.selecting());
        QVERIFY(!desk.view.manages());
        QCOMPARE(desk.view.role(), u"viewer"_s);
        // The wall stays, and whoever sits here is still signed in.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.account(), u"anna"_s);
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
        desk.actions.open(desk.key(u"b"_s));
        QVERIFY(desk.actions.menu().isEmpty());

        // Made an operator, and given another name: may act, does not manage.
        qkvm.user = R"({"id": "u1", "name": "anna b", "role": "operator", "token": false})";
        QTRY_VERIFY_WITH_TIMEOUT(desk.actions.allowed(), 8000);
        QCOMPARE(desk.view.account(), u"anna b"_s);
        QVERIFY(!desk.view.manages());
        QCOMPARE(desk.view.role(), u"operator"_s);
        desk.actions.open(desk.key(u"b"_s));
        QCOMPARE(desk.actions.menu().value(u"title"_s).toString(), u"NAS"_s);
        // An admin again: the page is offered, and what is open stays open.
        qkvm.user = R"({"id": "u1", "name": "anna b", "role": "admin", "token": false})";
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.manages(), 8000);
        QVERIFY(desk.actions.allowed());
        QCOMPARE(desk.actions.menu().value(u"title"_s).toString(), u"NAS"_s);

        // Now and then, and not with every look at the machines.
        const int who = server.count("/api/v1/session");
        const int looks = server.count("/api/v1/status");
        QVERIFY(who >= 4);
        QVERIFY2(who <= 1 + looks / 15, qPrintable(u"%1 times, in %2 looks"_s.arg(who).arg(looks)));
    }

    void whoWasSignedInIsNotTakenForWhoIsOnceTheWallWasLeft()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        bool slow = false;
        StandIn server([&](const Asked& asked) {
            Answered answer = qkvm.answer(asked);
            if (slow && asked.path == "/api/v1/session") {
                answer.afterMs = 700;
            }
            return answer;
        });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(desk.actions.allowed());
        // The question who is signed in is out, and its answer takes its time. Meanwhile whoever
        // sat here signs out.
        slow = true;
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/session") >= 2, 8000);
        desk.view.signOut();
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!desk.actions.allowed());
        // The answer names who was signed in when it was asked. Nobody is now: nobody may act,
        // and nobody is named.
        QTest::qWait(1200);
        QVERIFY(!desk.actions.allowed());
        QCOMPARE(desk.view.account(), QString());
        QVERIFY(!desk.view.manages());
        QCOMPARE(desk.view.role(), QString());
        QCOMPARE(desk.view.stage(), u"signIn"_s);
    }

    void aKvmThatWasToldToRestartSaysSoOnItsCardUntilItIsBack()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QByteArray there = qkvm.status;
        const QByteArray away =
            R"({"a": {"state": "offline", "error": {"code": "unreachable", "message": "Device is unreachable"}},
                                    "b": {"state": "no_signal"},
                                    "s": {"state": "online", "picture": false, "server": {"health": "OK"}}})";
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::StateRole), u"online"_s);

        // The KVM is told to restart itself, which is asked about first.
        desk.actions.open(desk.key(u"a"_s));
        desk.actions.choose(u"reboot"_s);
        QVERIFY(!desk.actions.question().isEmpty());
        desk.actions.answer(true);
        QTRY_COMPARE(server.count("/api/v1/devices/a/actions"), 1);
        // It has not gone down yet: its card is as it was.
        QTest::qWait(250);
        QCOMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::StateRole), u"online"_s);
        QCOMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::TitleRole), QString());

        // It goes away: that is called what it is, and not an outage.
        qkvm.status = away;
        QTRY_COMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::TitleRole), u"Rebooting…"_s);
        QCOMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::StateRole), u"connecting"_s);
        QCOMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::TextRole), u"Back in about a minute."_s);
        // Its neighbour, which was told nothing, says how it is.
        QCOMPARE(onTheCardOf(desk.wall, desk.key(u"b"_s), WallModel::TitleRole), u"No signal"_s);

        // It is back: its card is its own again.
        qkvm.status = there;
        QTRY_COMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::StateRole), u"online"_s);
        QCOMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::TitleRole), QString());
        // Away once more, later and untold: that is an outage, and is called one.
        qkvm.status = away;
        QTRY_COMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::TitleRole), u"Offline"_s);
        QCOMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::StateRole), u"offline"_s);
    }

    void aRestartIsNotWaitedForOnAWallThatWasLeft()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        desk.actions.open(desk.key(u"a"_s));
        desk.actions.choose(u"reboot"_s);
        desk.actions.answer(true);
        QTRY_COMPARE(server.count("/api/v1/devices/a/actions"), 1);
        QTest::qWait(250);

        // Signed out and in again: what was done before is not this wall's to wait for.
        desk.view.signOut();
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        qkvm.status = R"({"a": {"state": "offline"}, "b": {"state": "no_signal"},
                          "s": {"state": "online", "picture": false, "server": {"health": "OK"}}})";
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::StateRole), u"offline"_s);
        QCOMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::TitleRole), u"Offline"_s);
    }

    void leavingTheWallEndsThePickingOfMachines()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        desk.wall.setSelecting(true);
        desk.wall.togglePicked(desk.key(u"a"_s));
        QCOMPARE(desk.wall.picked(), 1);
        // A look at how the machines are does not let go of what is picked.
        QTest::qWait(350);
        QCOMPARE(desk.wall.picked(), 1);
        desk.view.signOut();
        QVERIFY(!desk.wall.selecting());
        QCOMPARE(desk.wall.picked(), 0);
    }

    void whoManagesTheQkvmIsOfferedItsPage()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        QList<QUrl> opened;
        desk.session.openPagesWith([&](const QUrl& address) {
            opened.append(address);
            return true;
        });
        // Nobody is signed in: nobody manages, and there is no page to go to.
        QVERIFY(!desk.view.manages());
        desk.view.openPage();
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!desk.view.manages());
        desk.view.openPage();
        QVERIFY(opened.isEmpty());

        // An admin signs in.
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(desk.view.manages());
        desk.view.openPage();
        QCOMPARE(opened, QList<QUrl>{QUrl(server.address() + u'/')});

        desk.view.signOut();
        QVERIFY(!desk.view.manages());
        desk.view.openPage();
        QCOMPARE(opened.size(), 1);
    }

    void aScreenOpenedWithADisplayLinkOnlyLooks()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        // The server's word for what a display link is for: it looks.
        QVERIFY(!desk.actions.allowed());
        QVERIFY(!desk.view.manages());
        desk.actions.open(desk.key(u"b"_s));
        desk.actions.showPort(desk.key(u"b"_s), 1);
        QVERIFY(desk.actions.menu().isEmpty());
        QTest::qWait(200);
        QCOMPARE(server.count("/api/v1/devices/b/actions"), 0);
    }

    void whatIsNoAddressIsSaidWithoutAskingAnybody()
    {
        Desk desk;
        ui::SessionView& view = desk.view;
        view.connectTo(u"file:///C:/Windows/win.ini"_s);
        QCOMPARE(view.stage(), u"address"_s);
        QVERIFY(!view.busy());
        QVERIFY(!view.problem().isEmpty());
    }

    void aServerThatIsNotThereIsSaid()
    {
        // A port nothing listens on.
        const core::testing::NobodyThere nobody;
        const QString address = nobody.address();
        Desk desk;
        ui::SessionView& view = desk.view;
        view.connectTo(address);
        QTRY_VERIFY(!view.busy());
        QCOMPARE(view.stage(), u"address"_s);
        QVERIFY(!view.problem().isEmpty());
    }

    void aQkvmThatIsNotSetUpIsSetUpOnItsPage()
    {
        Pretended qkvm;
        qkvm.setUp = false;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::SessionView& view = desk.view;
        view.connectTo(server.address());
        QTRY_VERIFY(!view.busy());
        QCOMPARE(view.stage(), u"address"_s);
        QVERIFY(view.problem().contains(u"not been set up"_s));
    }

    void aWrongPasswordIsSaidInTheServersWordsAndTheRightOneOpensTheWall()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::SessionView& view = desk.view;
        ui::WallView& wall = desk.wall;
        view.connectTo(server.address());
        QTRY_COMPARE(view.stage(), u"signIn"_s);

        view.signIn(QString(), u"a wrong one"_s, QString(), false);
        QVERIFY(view.busy());
        QTRY_VERIFY(!view.busy());
        QCOMPARE(view.stage(), u"signIn"_s);
        QCOMPARE(view.problem(), u"Wrong password"_s);

        view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(view.stage(), u"wall"_s);
        QCOMPARE(view.problem(), QString());
        QCOMPARE(view.account(), u"anna"_s);
        QVERIFY(view.inContact());

        // The wall fills with the server's machines, in the server's order, each as it is.
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        QCOMPARE(namesOn(wall), (QStringList{u"Build server"_s, u"NAS"_s, u"Database server"_s}));
        QTRY_COMPARE(wall.machines()->online(), 2);
        QCOMPARE(wall.machines()->noSignal(), 1);
        // The two KVMs have a picture each, under two different numbers; the server has none.
        const quint32 first = wall.machines()->index(0, 0).data(WallModel::StreamRole).toUInt();
        const quint32 second = wall.machines()->index(1, 0).data(WallModel::StreamRole).toUInt();
        QVERIFY(first != 0 && second != 0 && first != second);
        QCOMPARE(wall.machines()->index(2, 0).data(WallModel::StreamRole).toUInt(), 0U);
        // And what is asked for carries the session the sign-in gave.
        QTRY_VERIFY(server.count("/api/v1/status") >= 1);
        QCOMPARE(server.asked.constLast().headers.value("cookie"), QByteArray("qkvm_session=abc123"));
        // An ordinary sign-in is kept nowhere; the server is remembered, and suggested the next time.
        QVERIFY(desk.secrets.all.isEmpty());
        QCOMPARE(desk.settings.remembered().server, server.address() + u'/');
        QCOMPARE(view.suggested(), u"127.0.0.1:%1"_s.arg(server.serverPort()));
    }

    void whoeverAsksToStaySignedInIsStillSignedInAtTheNextStart()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::testing::MemorySecrets secrets;
        {
            core::Settings settings(file);
            ui::SessionView view;
            ui::WallView wall;
            ui::ActionsView actions;
            ui::ServerSession session(view, wall, settings, secrets, 100);
            session.add(wall, actions, nullptr);
            // A PC that never ran the program: nothing to go to.
            session.begin();
            QCOMPARE(view.stage(), u"address"_s);
            QVERIFY(!view.busy());
            QCOMPARE(view.suggested(), QString());

            view.connectTo(server.address());
            QTRY_COMPARE(view.stage(), u"signIn"_s);
            view.signIn(QString(), u"the right one"_s, QString(), true);
            QTRY_COMPARE(view.stage(), u"wall"_s);
            // The server was asked for a session that lasts, and it is kept: with the secrets, under
            // this server's name, and not in the settings. (What it is, who is signed in, what it
            // is once more before the password goes to it, the sign-in. This had the sign-in third:
            // no question before the password.)
            QCOMPARE(server.asked[2].path, QByteArray("/healthz"));
            QCOMPARE(server.asked[3].path, QByteArray("/api/v1/login"));
            QCOMPARE(QJsonDocument::fromJson(server.asked[3].body).object()["remember"_L1].toBool(), true);
            QCOMPARE(secrets.all.size(), 1);
            QCOMPARE(secrets.all.value(u"session at "_s + server.address() + u'/'), QByteArray("abc123"));
        }
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray settingsAsWritten = written.readAll();
        QVERIFY(settingsAsWritten.contains("127.0.0.1"));
        QVERIFY(!settingsAsWritten.contains("abc123"));

        // The program is started again: straight to the wall, with nothing typed.
        core::Settings settings(file);
        ui::SessionView view;
        ui::WallView wall;
        ui::ActionsView actions;
        ui::ServerSession session(view, wall, settings, secrets, 100);
        session.add(wall, actions, nullptr);
        const int loginsBefore = server.count("/api/v1/login");
        session.begin();
        QVERIFY(view.busy());
        QTRY_COMPARE(view.stage(), u"wall"_s);
        QCOMPARE(view.account(), u"anna"_s);
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        QCOMPARE(server.count("/api/v1/login"), loginsBefore);
    }

    void atTheNextStartAnOrdinarySignInIsAskedForAgainAtTheSameServer()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::testing::MemorySecrets secrets;
        {
            core::Settings settings(file);
            ui::SessionView view;
            ui::WallView wall;
            ui::ActionsView actions;
            ui::ServerSession session(view, wall, settings, secrets, 100);
            session.add(wall, actions, nullptr);
            view.connectTo(server.address());
            QTRY_COMPARE(view.stage(), u"signIn"_s);
            view.signIn(QString(), u"the right one"_s, QString(), false);
            QTRY_COMPARE(view.stage(), u"wall"_s);
        }
        core::Settings settings(file);
        ui::SessionView view;
        ui::WallView wall;
        ui::ActionsView actions;
        ui::ServerSession session(view, wall, settings, secrets, 100);
        session.add(wall, actions, nullptr);
        session.begin();
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        QCOMPARE(view.address(), u"127.0.0.1:%1"_s.arg(server.serverPort()));
        QCOMPARE(view.problem(), QString());
    }

    void aKeptSessionTheServerNoLongerKnowsIsForgottenAndThePasswordAskedFor()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server, "one-the-server-never-gave");
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        // It was tried, and it was not taken: nothing is said of it, and it is not tried again.
        // (After the question what the server is, which goes without it.)
        QCOMPARE(server.asked[1].path, QByteArray("/api/v1/session"));
        QCOMPARE(server.asked[1].headers.value("cookie"), QByteArray("qkvm_session=one-the-server-never-gave"));
        QCOMPARE(desk.view.problem(), QString());
        QVERIFY(desk.secrets.all.isEmpty());
    }

    void anOrdinarySignInForgetsASessionThatWasKeptBefore()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server, "an-older-one");
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(desk.secrets.all.isEmpty());
    }

    void whereWindowsKeepsNothingTheWallIsShownAllTheSame()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.secrets.refuses = true;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), true);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.problem(), QString());
    }

    void aServerWithACertificateOfItsOwnIsShownBeforeAnythingIsSentToIt()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("first"));
        QVERIFY(server.isListening());
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"trust"_s);
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.problem(), QString());
        // What it showed, as QKVM prints it when it starts; and nothing it was known by before.
        QCOMPARE(desk.view.certificate(), core::ServerClient::written(sha256Of("first")));
        QCOMPARE(desk.view.certificate().size(), 95);
        QCOMPARE(desk.view.certificateBefore(), QString());
        // Not a question has reached it, and so not the session that is kept for it either.
        QCOMPARE(server.asked.size(), 0);
        QVERIFY(desk.settings.remembered().certificates.isEmpty());

        // Yes: it is known by this certificate from now on, and asked who is signed in.
        desk.view.trustCertificate();
        QVERIFY(desk.view.busy());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(desk.view.encrypted());
        const QString name = u"127.0.0.1:%1"_s.arg(server.serverPort());
        QCOMPARE(desk.settings.remembered().certificates.value(name), sha256Of("first"));
        desk.view.signIn(QString(), u"the right one"_s, QString(), true);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        // Reached by HTTPS: remembered as that, under its name without the port.
        QCOMPARE(desk.settings.remembered().reachedEncrypted, QStringList{u"127.0.0.1"_s});
    }

    void aCertificateThatWasSaidYesToIsNotAskedAboutAgain()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("first"));
        Desk desk;
        desk.knows(server, sha256Of("first"));
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QCOMPARE(desk.view.certificate(), QString());
    }

    void noIsNoAndNothingIsRemembered()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("first"));
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"trust"_s);
        desk.view.anotherServer();
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(desk.settings.remembered().certificates.isEmpty());
        QCOMPARE(server.asked.size(), 0);
        // And a yes that comes when nothing is shown any more is no yes.
        desk.view.trustCertificate();
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(desk.settings.remembered().certificates.isEmpty());
        // Named again, it is asked about again.
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"trust"_s);
        QCOMPARE(server.asked.size(), 0);
    }

    void anotherCertificateThanBeforeStopsEverythingUntilSomebodyHasLooked()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("first"));
        Desk desk;
        desk.hasKept(server);
        // Known by one certificate, and showing another: as after somebody put a server of their
        // own where the QKVM was, and as after the QKVM was given a new certificate.
        desk.knows(server, sha256Of("second"));
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"trust"_s);
        QCOMPARE(desk.view.certificate(), core::ServerClient::written(sha256Of("first")));
        QCOMPARE(desk.view.certificateBefore(), core::ServerClient::written(sha256Of("second")));
        // It was told nothing: not the session that is kept for the server it was taken for.
        QCOMPARE(server.asked.size(), 0);
        QCOMPARE(desk.secrets.all.size(), 1);
        QCOMPARE(desk.settings.remembered().certificates.value(Desk::authorityOf(server)), sha256Of("second"));

        // Looked at, and found to be the QKVM's new one: on with it, to the wall.
        desk.view.trustCertificate();
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.settings.remembered().certificates.value(Desk::authorityOf(server)), sha256Of("first"));
    }

    void aCertificateThatChangesWhileTheWallIsUpTakesTheWallDown()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        auto server =
            std::make_unique<StandIn>([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("first"));
        const quint16 port = server->serverPort();
        Desk desk;
        desk.hasKept(*server);
        desk.knows(*server, sha256Of("first"));
        desk.view.connectTo(server->address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);

        // The server goes, and at its address there is one with another certificate.
        server.reset();
        StandIn other([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("second"), port);
        if (!other.isListening()) {
            QSKIP("Windows has given the port to somebody else in the meantime.");
        }
        QTRY_COMPARE_WITH_TIMEOUT(desk.view.stage(), u"trust"_s, 20000);
        QCOMPARE(desk.view.certificate(), core::ServerClient::written(sha256Of("second")));
        QCOMPARE(desk.view.certificateBefore(), core::ServerClient::written(sha256Of("first")));
        QCOMPARE(desk.wall.machines()->rowCount(), 0);
        QCOMPARE(desk.view.account(), QString());
        // The other one was asked nothing, and nothing is asked of it while the question stands.
        QTest::qWait(400);
        QCOMPARE(other.asked.size(), 0);
    }

    void aDisplayLinkSignsAScreenInWithoutAPassword()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        QStringList stages;
        connect(&desk.view, &ui::SessionView::changed, this, [&] { stages.append(desk.view.stage()); });
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.account(), u"Lobby screen"_s);
        QCOMPARE(desk.view.problem(), QString());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        // On the way there, nobody was shown a line for a password.
        QVERIFY(!stages.contains(u"signIn"_s));
        // The key went to the server once, where a display link is handed in, and nowhere else: not
        // into an address, and not into the settings.
        QCOMPARE(server.count("/api/v1/display"), 1);
        for (const Asked& asked : std::as_const(server.asked)) {
            QVERIFY(!asked.path.contains("qkvm_"));
        }
        QCOMPARE(desk.view.address(), Desk::authorityOf(server));
        QCOMPARE(desk.settings.remembered().server, server.address() + u'/');
        QFile written(desk.folder.filePath(u"settings.json"_s));
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(!written.readAll().contains("qkvm_"));
        // It is kept with the secrets, under this server's name: what the screen comes back in with.
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
        QCOMPARE(server.count("/api/v1/login"), 0);
    }

    void aScreenOpenedWithADisplayLinkShowsTheWallAndNothingElse()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        // Somebody signs in: the wall as a person sees it, with its bar.
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!desk.wall.displaying());
        // They choose to have it only shown, and sign out: the next one to sign in has the bar.
        desk.wall.setDisplaying(true);
        desk.view.signOut();
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!desk.wall.displaying());
        desk.view.signOut();

        // A display link, on another PC: a screen nobody sits at. The wall, and nothing else.
        Desk screen;
        screen.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(screen.view.stage(), u"wall"_s);
        QVERIFY(screen.wall.displaying());
    }

    void whoeverSignsOutAndInAgainAtOnceStaysSignedIn()
    {
        Pretended qkvm;
        // The answer to the sign-out is on its way for longer than it takes to sign in again.
        qkvm.signOutAnsweredAfterMs = 400;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        desk.view.signOut();
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        // The sign-out's answer comes and takes the cookie back: the one it was sent with. The
        // wall is looked at several times after that, and whoever signed in is still signed in.
        QTest::qWait(600);
        const int looks = server.count("/api/v1/status");
        QTRY_VERIFY(server.count("/api/v1/status") >= looks + 3);
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
    }

    void aSignOutThatNeverReachedTheServerLeavesNoWayBackIn()
    {
        Pretended qkvm;
        qkvm.deafToSignOut = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), true);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.secrets.all.value(Desk::nameFor(server)), QByteArray("abc123"));

        desk.view.signOut();
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        QTRY_COMPARE(server.count("/api/v1/logout"), 1);
        // The server never heard of it, and would still know the session.
        QVERIFY(qkvm.signedIn);
        const qsizetype askedBefore = server.asked.size();

        // Naming the server anew does not bring the wall back: somebody has to sign in.
        desk.view.anotherServer();
        QCOMPARE(desk.view.stage(), u"address"_s);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(server.asked.size() > askedBefore);
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QTest::qWait(300);
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        // Nothing that was asked since carried the session, and it is not kept for the next start.
        for (qsizetype at = askedBefore; at < server.asked.size(); ++at) {
            QVERIFY2(!server.asked[at].headers.contains("cookie"), server.asked[at].path.constData());
        }
        QVERIFY(!desk.secrets.all.contains(Desk::nameFor(server)));
    }

    void afterTheServerWasRestartedAScreenComesBackByItself()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        const quint32 nas = desk.wall.machines()->index(1, 0).data(WallModel::StreamRole).toUInt();
        QStringList stages;
        connect(&desk.view, &ui::SessionView::changed, this, [&] { stages.append(desk.view.stage()); });

        // A server that starts again knows no session it gave before.
        qkvm.signedIn = false;
        QTRY_COMPARE(qkvm.displaysOpened, 2);
        QTRY_VERIFY(qkvm.signedIn);
        QTRY_VERIFY(desk.view.inContact());
        QTRY_VERIFY(server.count("/api/v1/status") >= 3);
        // The wall never went away: nobody was asked anything, the cards stayed, and each kept its
        // picture's place.
        QVERIFY(!stages.contains(u"signIn"_s));
        QVERIFY(!stages.contains(u"address"_s));
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
        QCOMPARE(desk.wall.machines()->index(1, 0).data(WallModel::StreamRole).toUInt(), nas);
        QCOMPARE(desk.view.problem(), QString());
    }

    void whileTheServerIsDownAScreenWaitsForItAndComesBack()
    {
        Pretended qkvm;
        auto server = std::make_unique<StandIn>([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const quint16 port = server->serverPort();
        Desk desk;
        desk.view.connectTo(Desk::linkTo(*server, qkvm.displayKey));
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);

        // The power goes: nothing answers. The wall stays, and says that contact is lost, and
        // since when, with the day: a wall nobody sits at may be read days later.
        QCOMPARE(desk.view.awaySince(), QString());
        server.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.inContact(), 20000);
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
        const QString since = desk.view.awaySince();
        QCOMPARE(since, QLocale::system().toString(desk.now, QLocale::ShortFormat));
        QVERIFY2(since.contains(u"10"_s) && since.contains(u"02"_s), qPrintable(since));
        // Hours go by, and the link is handed in again and again: it is since then all the same.
        desk.now = desk.now.addSecs(10'800);
        QTest::qWait(500);
        QCOMPARE(desk.view.awaySince(), since);

        // And comes back: the server is up again, and knows no session from before.
        qkvm.signedIn = false;
        StandIn back([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, port);
        if (!back.isListening()) {
            QSKIP("Windows has given the port to somebody else in the meantime.");
        }
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.inContact(), 20000);
        QCOMPARE(desk.view.awaySince(), QString());
        QCOMPARE(qkvm.displaysOpened, 2);
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
    }

    void aServerThatIsNotBackYetIsAskedAgainUntilItIs()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);

        // The server is half up: it says the session is gone, and falls over at the link.
        qkvm.fallsOverAtALink = true;
        qkvm.signedIn = false;
        QTRY_VERIFY(server.count("/api/v1/display") >= 4);
        // Meanwhile the wall stays, says that contact is lost, and asks nobody for anything.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!desk.view.inContact());
        // Since when is since the first time it did not answer, however often it is asked again.
        const QString since = desk.view.awaySince();
        QCOMPARE(since, QLocale::system().toString(desk.now, QLocale::ShortFormat));
        desk.now = desk.now.addSecs(10'800);
        const int asked = server.count("/api/v1/display");
        QTRY_VERIFY(server.count("/api/v1/display") >= asked + 3);
        QCOMPARE(desk.view.awaySince(), since);
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);

        // And then it is up.
        qkvm.fallsOverAtALink = false;
        QTRY_VERIFY(desk.view.inContact());
        QCOMPARE(qkvm.displaysOpened, 2);
        QCOMPARE(desk.view.stage(), u"wall"_s);
    }

    void aDisplayLinkThatWasTakenBackAsksForSomebodyToSignIn()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);

        // An admin takes the link back: the screen goes dark, and the link is good for nothing.
        qkvm.displayKey = "qkvm_" + QByteArray(43, 'n');
        qkvm.signedIn = false;
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QCOMPARE(desk.view.problem(), u"This display link is not valid any more. Ask an admin for a new one."_s);
        QCOMPARE(desk.wall.machines()->rowCount(), 0);
        QVERIFY(!desk.secrets.all.contains(Desk::linkNameFor(server)));
        // It is not handed in again and again.
        const int handedIn = server.count("/api/v1/display");
        QTest::qWait(400);
        QCOMPARE(server.count("/api/v1/display"), handedIn);
        // Somebody can sign in instead.
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
    }

    void aLinkThatIsNotValidIsSaidWhereItWasPasted()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, "qkvm_" + QByteArray(43, 'x')));
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.problem(), u"This display link is not valid any more. Ask an admin for a new one."_s);
        QVERIFY(desk.secrets.all.isEmpty());
    }

    void atTheNextStartAScreenIsBackWithItsLink()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::testing::MemorySecrets secrets;
        {
            core::Settings settings(file);
            ui::SessionView view;
            ui::WallView wall;
            ui::ActionsView actions;
            ui::ServerSession session(view, wall, settings, secrets, 100);
            session.add(wall, actions, nullptr);
            view.connectTo(Desk::linkTo(server, qkvm.displayKey));
            QTRY_COMPARE(view.stage(), u"wall"_s);
        }
        // The PC was off, and so was the server.
        qkvm.signedIn = false;
        core::Settings settings(file);
        ui::SessionView view;
        ui::WallView wall;
        ui::ActionsView actions;
        ui::ServerSession session(view, wall, settings, secrets, 100);
        session.add(wall, actions, nullptr);
        session.begin();
        QTRY_COMPARE(view.stage(), u"wall"_s);
        QCOMPARE(view.account(), u"Lobby screen"_s);
        QCOMPARE(qkvm.displaysOpened, 2);
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
    }

    void signingOutPutsTheLinkAwayToo()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        desk.view.signOut();
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(desk.secrets.all.isEmpty());
        // And the screen does not sign itself in again behind whoever signed it out.
        const int handedIn = server.count("/api/v1/display");
        QTest::qWait(400);
        QCOMPARE(server.count("/api/v1/display"), handedIn);
        QCOMPARE(desk.view.stage(), u"signIn"_s);
    }

    void aLinkIsThatServersAndNoOthers()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Pretended otherQkvm;
        StandIn other([&otherQkvm](const Asked& asked) { return otherQkvm.answer(asked); });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);

        // Another QKVM is named, by its address alone: it is asked for a password, and never sees
        // the first one's link.
        desk.view.anotherServer();
        desk.view.connectTo(other.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QTest::qWait(300);
        QCOMPARE(other.count("/api/v1/display"), 0);
        // Back at the first, the screen is in again with the link that is kept for it.
        desk.view.anotherServer();
        qkvm.signedIn = false;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(qkvm.displaysOpened, 2);
    }

    void aLinksKeyIsNotHandedInBeforeTheCertificateWasLookedAt()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("first"));
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.view.stage(), u"trust"_s);
        QTest::qWait(300);
        QCOMPARE(server.asked.size(), 0);
        QVERIFY(desk.secrets.all.isEmpty());
        desk.view.trustCertificate();
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.account(), u"Lobby screen"_s);
    }

    void aServersCardShowsItsOwnScreenWhereTheServerHasOne()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        // The third machine is a server whose controller has a console the QKVM server reads.
        qkvm.status = R"({"a": {"state": "online"}, "b": {"state": "no_signal"},
                         "s": {"state": "online", "picture": false, "screen": true,
                               "video": {"width": 1024, "height": 768}, "server": {"health": "Warning"}}})";
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        const QModelIndex card = desk.wall.machines()->index(2, 0);

        // Pictures of it are asked for as of a KVM, and once one has come its card shows it: a
        // stream of its own, no panel, and under it what the panel would have said.
        QTRY_VERIFY(server.count("/api/v1/devices/s/snapshot") >= 1);
        QTRY_VERIFY(card.data(WallModel::StreamRole).toUInt() != 0);
        QVERIFY(card.data(WallModel::ServerRole).toMap().isEmpty());
        QCOMPARE(card.data(WallModel::MetaRole).toString(), u"1024×768 · Warning"_s);
        QCOMPARE(card.data(WallModel::TitleRole).toString(), QString());
        const quint32 stream = card.data(WallModel::StreamRole).toUInt();
        QVERIFY(stream != desk.wall.machines()->index(0, 0).data(WallModel::StreamRole).toUInt());

        // Switched off: the card says how the server is, and shows no last picture.
        qkvm.status = R"({"a": {"state": "online"}, "b": {"state": "no_signal"},
                         "s": {"state": "no_signal", "picture": false, "screen": true,
                               "atx": {"enabled": false, "power": false}, "server": {"health": "OK"}}})";
        QTRY_COMPARE(card.data(WallModel::StreamRole).toUInt(), 0U);
        QCOMPARE(card.data(WallModel::ServerRole).toMap()[u"power"_s].toString(), u"off"_s);

        // On again: its screen is back, under the same number.
        qkvm.status = R"({"a": {"state": "online"}, "b": {"state": "no_signal"},
                         "s": {"state": "online", "picture": false, "screen": true, "server": {"health": "OK"}}})";
        QTRY_COMPARE(card.data(WallModel::StreamRole).toUInt(), stream);
    }

    void aServersScreenStaysOnItsCardWhileTheQkvmServerCannotBeReached()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.status = R"({"a": {"state": "online"}, "b": {"state": "no_signal"},
                         "s": {"state": "online", "picture": false, "screen": true, "server": {"health": "OK"}}})";
        auto server = std::make_unique<StandIn>([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(*server);
        desk.view.connectTo(server->address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        const QModelIndex card = desk.wall.machines()->index(2, 0);
        QTRY_VERIFY(card.data(WallModel::StreamRole).toUInt() != 0);
        const quint32 stream = card.data(WallModel::StreamRole).toUInt();

        // The QKVM server is gone. That says nothing of the machines: the wall shows what it last
        // knew of them, and the server's screen with the rest.
        server.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.inContact(), 20000);
        QTest::qWait(1500);
        QCOMPARE(card.data(WallModel::StreamRole).toUInt(), stream);
        QVERIFY(card.data(WallModel::ServerRole).toMap().isEmpty());
    }

    void aServerWithoutAScreenToReadIsNotAskedForOne()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= 2);
        QCOMPARE(server.count("/api/v1/devices/s/"), 0);
        QCOMPARE(desk.wall.machines()->index(2, 0).data(WallModel::StreamRole).toUInt(), 0U);
        QVERIFY(!desk.wall.machines()->index(2, 0).data(WallModel::ServerRole).toMap().isEmpty());
    }

    void aServerWhosePictureCannotBeHadSaysHowItIsInstead_data()
    {
        QTest::addColumn<QString>("word");
        QTest::addColumn<QString>("sentence");
        QTest::newRow("its console is in use") << u"no_stream"_s << u"The console is in use"_s;
        // The QKVM server's word for a controller it cannot reach: not this program's finding that
        // the QKVM server cannot be reached, after which the card would keep its last picture.
        QTest::newRow("its controller is not reached") << u"unreachable"_s << u"Device is unreachable"_s;
    }

    void aServerWhosePictureCannotBeHadSaysHowItIsInstead()
    {
        QFETCH(QString, word);
        QFETCH(QString, sentence);
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.status = R"({"a": {"state": "online"}, "b": {"state": "no_signal"},
                         "s": {"state": "online", "picture": false, "screen": true, "server": {"health": "OK"}}})";
        bool consoleBusy = false;
        StandIn server([&](const Asked& asked) {
            if (consoleBusy && asked.path.startsWith("/api/v1/devices/s/snapshot") && qkvm.knows(asked)) {
                return Pretended::refused(502, word.toUtf8().constData(), sentence.toUtf8().constData());
            }
            return qkvm.answer(asked);
        });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        const QModelIndex card = desk.wall.machines()->index(2, 0);
        QTRY_VERIFY(card.data(WallModel::StreamRole).toUInt() != 0);

        consoleBusy = true;
        QTRY_COMPARE(card.data(WallModel::StreamRole).toUInt(), 0U);
        QVERIFY(!card.data(WallModel::ServerRole).toMap().isEmpty());
        consoleBusy = false;
        QTRY_VERIFY(card.data(WallModel::StreamRole).toUInt() != 0);
    }

    void aServerOnceReachedEncryptedIsNotSpokenToInTheClear()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        core::Remembered remembered;
        remembered.reachedEncrypted = {u"127.0.0.1"_s};
        QVERIFY(desk.settings.remember(remembered).ok());

        // Typed with "http://": refused, and the server hears nothing.
        desk.view.connectTo(server.address());
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(!desk.view.busy());
        QVERIFY(desk.view.problem().contains(u"https://"_s));
        QCOMPARE(server.asked.size(), 0);
        QCOMPARE(server.notHttp, 0);
        // Typed without saying how: taken to mean HTTPS. The stand-in speaks none and hangs up, so
        // nothing comes of it; what matters is that an encrypted conversation was begun, and that
        // not one question reached the server in the clear.
        desk.view.connectTo(u"127.0.0.1:%1"_s.arg(server.serverPort()));
        QVERIFY(desk.view.busy());
        QVERIFY(desk.view.encrypted());
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.busy(), 20000);
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(!desk.view.problem().isEmpty());
        QCOMPARE(server.asked.size(), 0);
        QVERIFY(server.notHttp >= 1);
    }

    void aSignInIsOrdinaryAndNotALongOne()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::SessionView& view = desk.view;
        view.connectTo(server.address());
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(view.stage(), u"wall"_s);
        QCOMPARE(server.count("/api/v1/login"), 1);
        for (const Asked& asked : std::as_const(server.asked)) {
            if (asked.path == "/api/v1/login") {
                const QJsonObject typed = QJsonDocument::fromJson(asked.body).object();
                QCOMPARE(typed["remember"_L1].toBool(true), false);
                // A call that changes something says it is this program's and not a page's form.
                QCOMPARE(asked.headers.value("x-qkvm"), QByteArray("1"));
            }
        }
    }

    void aNameIsAskedForWhereTheServerHasSeveralAccounts()
    {
        Pretended qkvm;
        qkvm.severalAccounts = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::SessionView& view = desk.view;
        view.connectTo(server.address());
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        QVERIFY(view.asksName());
        view.signIn(u" anna "_s, u"the right one"_s, QString(), false);
        QTRY_COMPARE(view.stage(), u"wall"_s);
    }

    void aCodeIsAskedForOnlyOnceThePasswordWasRight()
    {
        Pretended qkvm;
        qkvm.secondStep = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::SessionView& view = desk.view;
        view.connectTo(server.address());
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        QVERIFY(!view.asksCode());

        // What stands in the code's line before it is asked for is not sent.
        view.signIn(QString(), u"the right one"_s, u"999999"_s, false);
        QTRY_VERIFY(view.asksCode());
        QVERIFY(!view.busy());
        QCOMPARE(view.stage(), u"signIn"_s);
        // That the code is wanted is the next thing to do, and not said as a mistake.
        QCOMPARE(view.problem(), QString());

        view.signIn(QString(), u"the right one"_s, u"000000"_s, false);
        QTRY_VERIFY(!view.busy());
        QCOMPARE(view.stage(), u"signIn"_s);
        QVERIFY(view.problem().startsWith(u"That code is not right."_s));

        view.signIn(QString(), u"the right one"_s, u" 123456 "_s, false);
        QTRY_COMPARE(view.stage(), u"wall"_s);
    }

    void whoeverIsStillSignedInGoesStraightToTheWall()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::SessionView& view = desk.view;
        ui::WallView& wall = desk.wall;
        view.connectTo(server.address());
        QTRY_COMPARE(view.stage(), u"wall"_s);
        QCOMPARE(view.account(), u"anna"_s);
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
    }

    void signingOutTellsTheServerAndEmptiesTheWall()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::SessionView& view = desk.view;
        ui::WallView& wall = desk.wall;
        view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);

        view.signOut();
        QCOMPARE(view.stage(), u"signIn"_s);
        QCOMPARE(view.account(), QString());
        QCOMPARE(wall.machines()->rowCount(), 0);
        // And nothing is kept that could sign in again.
        QVERIFY(desk.secrets.all.isEmpty());
        QTRY_COMPARE(server.count("/api/v1/logout"), 1);
        QTRY_VERIFY(!qkvm.signedIn);
        // And nothing is asked of the wall any more.
        const int asked = server.count("/api/v1/status");
        QTest::qWait(350);
        QCOMPARE(server.count("/api/v1/status"), asked);
    }

    void aSessionTheServerEndedAsksForASignInAgain()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::SessionView& view = desk.view;
        ui::WallView& wall = desk.wall;
        view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);

        qkvm.signedIn = false;
        QTRY_COMPARE(view.stage(), u"signIn"_s);
        QVERIFY(view.problem().contains(u"signed out"_s));
        QCOMPARE(wall.machines()->rowCount(), 0);
        // A session the server has ended is not kept.
        QVERIFY(desk.secrets.all.isEmpty());
    }

    void aServerThatStopsAnsweringIsSaidAndTheWallStays()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        auto server = std::make_unique<StandIn>([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(*server);
        ui::SessionView& view = desk.view;
        ui::WallView& wall = desk.wall;
        view.connectTo(server->address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        QVERIFY(view.inContact());

        server.reset();
        QTRY_VERIFY(!view.inContact());
        // What was last known stays on the wall, and nobody is sent back to signing in.
        QCOMPARE(view.stage(), u"wall"_s);
        QCOMPARE(wall.machines()->rowCount(), 3);
    }

    void aMachineThatLeavesTheWallLeavesTheScreen()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::SessionView& view = desk.view;
        ui::WallView& wall = desk.wall;
        view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        const quint32 nas = wall.machines()->index(1, 0).data(WallModel::StreamRole).toUInt();

        // Somebody removed two machines elsewhere: the server speaks of one from now on.
        qkvm.wall = R"({"interval": 0.25, "devices": [
            {"id": "b", "name": "NAS", "host": "192.168.1.22", "kind": "kvmd", "picture": true}]})";
        qkvm.status = R"({"b": {"state": "no_signal"}})";
        QTRY_COMPARE(wall.machines()->rowCount(), 1);
        QCOMPARE(namesOn(wall), QStringList{u"NAS"_s});
        // A machine that stays keeps its picture's number: its card is not another machine's.
        QCOMPARE(wall.machines()->index(0, 0).data(WallModel::StreamRole).toUInt(), nas);
    }

    void picturesAreAskedForTheWidthOfACardAsThePageAsks()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::SessionView& view = desk.view;
        ui::WallView& wall = desk.wall;
        view.connectTo(server.address());
        // Before the screen has said how wide a card is: the least that is asked for.
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=480&") >= 1);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot?w=480&") >= 1);
        // The server has no picture, and is not asked for one.
        QCOMPARE(server.count("/api/v1/devices/s/"), 0);

        // Twice the pixels a card's picture covers.
        wall.setCardPixels(400);
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=800&") >= 1);
    }

    void aSecondWindowHasTheSameMachinesOnAWallOfItsOwn()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QCOMPARE(second.wall.machines()->rowCount(), 3);
        // Whoever is signed in may act from either, and the server's pace is the pace of both.
        QVERIFY(desk.actions.allowed());
        QVERIFY(second.actions.allowed());
        QCOMPARE(second.wall.pace(), 250);
        // Each wall is narrowed by itself. (How the machines are is another answer than which
        // there are, and may come a moment later.)
        second.wall.shown()->setShow(u"no_signal"_s);
        QTRY_COMPARE(second.wall.shown()->count(), 1);
        QCOMPARE(desk.wall.shown()->count(), 3);
        // How the machines are reaches both.
        qkvm.status = R"({"a": {"state": "offline"}, "b": {"state": "no_signal"},
                          "s": {"state": "online", "picture": false, "server": {"health": "OK"}}})";
        QTRY_COMPARE(desk.wall.machines()->attention(), 1);
        QCOMPARE(second.wall.machines()->attention(), 1);

        // A machine's menu is its window's: opened in one, it is not open in the other.
        second.actions.open(desk.key(u"b"_s));
        QTRY_VERIFY(!second.actions.menu().isEmpty());
        QVERIFY(desk.actions.menu().isEmpty());

        // Signed out: both walls are empty, neither window may act, and nothing is open.
        desk.view.signOut();
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QCOMPARE(desk.wall.machines()->rowCount(), 0);
        QCOMPARE(second.wall.machines()->rowCount(), 0);
        QVERIFY(!second.actions.allowed());
        QVERIFY(second.actions.menu().isEmpty());
        // (Taken from the wall before it ends, as WallWindows takes each window: the wall's actions of it
        // go while its views stand.)
        desk.session.remove(second.wall);
    }

    void aPictureIsAskedForOnceAsWideAsTheWidestWindowDrawsIt()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        // Small cards in the first window, of both machines; large ones in the second, of which
        // only the first machine's is in sight.
        desk.wall.setCardPixels(300);
        second.wall.setCardPixels(500);
        second.wall.setInSight(0, 0);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=1000&") >= 2);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot?w=600&") >= 2);
        // Once, and not once for each window.
        QCOMPARE(server.count("/api/v1/devices/a/snapshot?w=600&"), 0);
        QCOMPARE(server.count("/api/v1/devices/b/snapshot?w=1000&"), 0);

        // Whichever window draws it widest: the first, now.
        desk.wall.setCardPixels(700);
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=1400&") >= 2);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot?w=1400&") >= 2);
        desk.wall.setCardPixels(300);
        // The second window goes out of sight: what the first shows is asked for, as wide as it
        // draws it.
        second.wall.setUnseen(true);
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=600&") >= 2);
        // Both go: no pictures at all.
        desk.wall.setUnseen(true);
        QTest::qWait(300);
        const int first = server.count("/api/v1/devices/a/snapshot");
        const int other = server.count("/api/v1/devices/b/snapshot");
        QTest::qWait(900);
        QVERIFY(server.count("/api/v1/devices/a/snapshot") <= first + 1);
        QVERIFY(server.count("/api/v1/devices/b/snapshot") <= other + 1);
        // The second alone is back: its one card, and not the first window's other.
        second.wall.setUnseen(false);
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= first + 4);
        QVERIFY(server.count("/api/v1/devices/b/snapshot") <= other + 1);
    }

    void aMachineShownLargeInOneWindowLeavesTheOthersWallAtItsPace()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        // A picture every five seconds was chosen.
        Desk desk(5000);
        desk.hasKept(server);
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= 1);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= 1);
        QTRY_COMPARE(second.wall.machines()->rowCount(), 3);

        // The second window shows one machine large, as wide as it draws it; the first goes on
        // showing the wall.
        second.wall.setEnlargedPixels(1500);
        second.wall.enlarge(desk.key(u"b"_s));
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/devices/b/snapshot?w=3000&") >= 5, 3000);
        // (One for the new beginning, and no more: the five seconds are not over.)
        QVERIFY2(server.count("/api/v1/devices/a/snapshot") <= 2,
                 qPrintable(QString::number(server.count("/api/v1/devices/a/snapshot"))));
        // Back to its wall: the machine at the wall's pace again, at the width of a card.
        second.wall.closeEnlarged();
        QTest::qWait(400);
        const int large = server.count("/api/v1/devices/b/snapshot?w=3000&");
        QTest::qWait(900);
        QVERIFY(server.count("/api/v1/devices/b/snapshot?w=3000&") <= large);
    }

    void aMachineThatIsShownLargeIsHadLiveWhereTheServerHasLivePictures()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        // (The machines are looked at every few seconds, as in the program: nothing here waits
        // for the next look.)
        Desk desk(0, 2500);
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        // On the wall nothing is had live.
        QTest::qWait(200);
        QCOMPARE(server.count("/api/v1/devices/a/live"), 0);

        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        QCOMPARE(server.live.path, QByteArray("/api/v1/devices/a/live"));
        QTRY_COMPARE(server.live.said.size(), 1);
        // No wider than it is drawn, to the next step.
        QCOMPARE(server.live.said.at(0)[u"max_w"_s].toInt(), 1120);
        // A line is no picture yet: the machine's snapshots are what there is, and are asked for.
        QCOMPARE(wall.liveStream(), 0U);
        QVERIFY(!wall.isLive());
        const int early = server.count("/api/v1/devices/a/snapshot?w=2000&");
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=2000&") >= early + 2);

        // Pictures come over the line: they are what is drawn, there and then, and no snapshot
        // is asked for.
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY_WITH_TIMEOUT(wall.liveStream() != 0, 1000);
        QTest::qWait(300);
        const int before = server.count("/api/v1/devices/a/snapshot");
        QTest::qWait(900);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), before);
        // The window is made larger.
        wall.setEnlargedPixels(1300);
        QTRY_COMPARE(server.live.saidOf(u"shape"_s).size(), 1);
        QCOMPARE(server.live.saidOf(u"shape"_s).at(0)[u"max_w"_s].toInt(), 1440);

        // Back to the wall: the line is ended, and the machine has its snapshots again.
        wall.closeEnlarged();
        QCOMPARE(wall.liveStream(), 0U);
        QTRY_VERIFY(!server.live.isConnected());
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= before + 2);
        QCOMPARE(server.count("/api/v1/devices/a/live"), 1);
    }

    // Part D (web 0.2.0): a live line no longer keeps its sign-in alive on the server; only the
    // person's own requests do (app/qkvm/server.py, live_standing: Sessions.get(use=False)). The
    // program's looks at the wall are such requests, carry the session, and go on at their pace while
    // a machine is had live: a console held open keeps its sign-in as the wall does (the server ends
    // an idle one after 12 hours, or 30 days where it is kept), and the line goes on. A window put away
    // ends its line itself (nobody looks at it), and the looks go on all the same.
    void aConsoleHeldOpenGoesOnLookingAtTheWallWithItsSession_data()
    {
        QTest::addColumn<bool>("putAway");
        QTest::newRow("window shown") << false;
        QTest::newRow("window put away") << true;
    }
    void aConsoleHeldOpenGoesOnLookingAtTheWallWithItsSession()
    {
        QFETCH(bool, putAway);
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk(0, 200);
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY_WITH_TIMEOUT(wall.liveStream() != 0, 1000);
        wall.setUnseen(putAway);
        const auto withTheSession = [&] {
            int found = 0;
            for (const Asked& one : server.asked) {
                found += one.path.startsWith("/api/v1/status") &&
                                 one.headers.value("cookie").split(';').contains("qkvm_session=" + qkvm.session)
                             ? 1
                             : 0;
            }
            return found;
        };
        const int before = withTheSession();
        QTest::qWait(2000);
        const int looked = withTheSession() - before;
        QVERIFY2(looked >= 6, qPrintable(u"%1 looks at the wall in 2 s at a pace of 200 ms"_s.arg(looked)));
        QCOMPARE(server.live.isConnected(), !putAway);
        QCOMPARE(server.live.lines, 1);
    }
    void aServerThatDoesNotSayItHasLivePicturesIsAskedForNone()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        const int before = server.count("/api/v1/devices/a/snapshot?w=2000&");
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=2000&") >= before + 3);
        QCOMPARE(server.count("/api/v1/devices/a/live"), 0);
        QCOMPARE(wall.liveStream(), 0U);
    }

    void aServerThatSpeaksAnotherFormOfLivePicturesIsAskedForNone()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive(3);
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        const int before = server.count("/api/v1/devices/a/snapshot?w=2000&");
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=2000&") >= before + 3);
        QCOMPARE(server.count("/api/v1/devices/a/live"), 0);
    }

    void aMachineTheServerHasNoLivePictureOfIsAskedForOnceAndItsSnapshotsGoOn()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        qkvm.noLiveOf = "a";
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.count("/api/v1/devices/a/live"), 1);
        const int before = server.count("/api/v1/devices/a/snapshot?w=2000&");
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=2000&") >= before + 3);
        QCOMPARE(server.count("/api/v1/devices/a/live"), 1);
        QCOMPARE(server.live.lines, 0);
        QCOMPARE(wall.liveStream(), 0U);
        // A machine that has no picture at all is not asked for one live either.
        wall.enlarge(desk.key(u"s"_s));
        QTest::qWait(300);
        QCOMPARE(server.count("/api/v1/devices/s/live"), 0);
    }

    void aLiveLineThatEndsKeepsItsLastPictureUntilASnapshotHasCome()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        // (A line that ended is not tried again for a minute here.)
        Desk desk(0, 100, {10, 60'000, 60'000, 5, 20});
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        QTRY_VERIFY(server.live.isConnected());
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(wall.liveStream() != 0);
        QTest::qWait(300);
        const int before = server.count("/api/v1/devices/a/snapshot");

        // The line falls over. A snapshot is asked for, and when it has come it is what is drawn.
        server.live.fallOver();
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= before + 1);
        QTRY_COMPARE(wall.liveStream(), 0U);
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= before + 3);
    }

    void onlyASnapshotOfThatMachineTakesTheLastLivePicturesPlace()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        // The machine's snapshots take three seconds to come.
        qkvm.slowSnapshotOf = "a";
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        // (A line that ended is not tried again for a minute here.)
        Desk desk(0, 100, {10, 60'000, 60'000, 5, 20});
        desk.hasKept(server);
        // A second window, which shows the machine beside it and no other: that one's snapshots
        // go on coming.
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        second.wall.shown()->setShow(u"no_signal"_s);
        QTRY_COMPARE(second.wall.shown()->count(), 1);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        QTRY_VERIFY(server.live.isConnected());
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(wall.liveStream() != 0);
        const quint32 last = wall.liveStream();
        // Whatever snapshot of it was asked for before its live pictures came has come by now.
        QTest::qWait(3400);
        QCOMPARE(wall.liveStream(), last);

        // The line falls over. A snapshot of the machine is asked for, and is three seconds
        // away; snapshots of the machine beside it come meanwhile, and are nothing to this
        // one's last picture.
        const int ofTheOther = server.count("/api/v1/devices/b/snapshot");
        server.live.fallOver();
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= ofTheOther + 3);
        QTest::qWait(300);
        QCOMPARE(wall.liveStream(), last);
        QVERIFY(!wall.isLive());
        // Its own has come: that is what is drawn.
        QTRY_COMPARE_WITH_TIMEOUT(wall.liveStream(), 0U, 8000);
    }

    void aLiveLineThatEndsIsOpenedAgain()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        QTRY_VERIFY(server.live.isConnected());
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(wall.liveStream() != 0);
        const quint32 first = wall.liveStream();

        server.live.fallOver();
        QTRY_COMPARE(server.live.lines, 2);
        QTRY_VERIFY(server.live.isConnected());
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(wall.liveStream() != 0 && wall.liveStream() != first);
    }

    void aWindowNobodyCanSeeHasNoLiveLine()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        QTRY_VERIFY(server.live.isConnected());
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(wall.liveStream() != 0);
        const quint32 last = wall.liveStream();
        QVERIFY(wall.isLive());
        // (A snapshot that was on its way when the live pictures came has come by now.)
        QTest::qWait(300);
        // Minimized, or behind the program's icon: the line is ended. What it showed last is
        // what the window shows when somebody looks again, until there is something newer, and
        // it is not said to be live.
        wall.setUnseen(true);
        QVERIFY(!wall.isLive());
        QTRY_VERIFY(!server.live.isConnected());
        QTest::qWait(400);
        QCOMPARE(server.live.lines, 1);
        QCOMPARE(wall.liveStream(), last);
        // Seen again: live again.
        wall.setUnseen(false);
        QTRY_COMPARE(server.live.lines, 2);
    }

    void eachWindowHasALineOfItsOwnForTheMachineItShowsLarge()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(second.wall.machines()->rowCount(), 3);
        desk.wall.setEnlargedPixels(1000);
        desk.wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        second.wall.setEnlargedPixels(600);
        second.wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 2);
        QCOMPARE(server.count("/api/v1/devices/a/live"), 2);
        QTRY_VERIFY(server.live.isConnected());
        // The picture goes over the second's line: it is the second's, and the first window still
        // has the machine's snapshots, which are asked for on.
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(second.wall.liveStream() != 0);
        QCOMPARE(desk.wall.liveStream(), 0U);
        const int before = server.count("/api/v1/devices/a/snapshot");
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= before + 2);
        // The window is closed: its line goes with it.
        desk.session.remove(second.wall);
        QTRY_VERIFY(!server.live.isConnected());
    }

    void leavingTheWallEndsTheLiveLine()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        QTRY_VERIFY(server.live.isConnected());
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(wall.liveStream() != 0);

        desk.view.signOut();
        QCOMPARE(wall.liveStream(), 0U);
        QTRY_VERIFY(!server.live.isConnected());
        QTest::qWait(500);
        QCOMPARE(server.count("/api/v1/devices/a/live"), 1);
        QCOMPARE(server.live.lines, 1);
    }

    void aMachineIsHadLiveAgainAsSoonAsTheServerAnswersAgain()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        auto server = std::make_unique<StandIn>([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const quint16 port = server->serverPort();
        // (A line that ended is not tried again for a minute here: what opens the next one is
        // that the server is back.)
        Desk desk(0, 100, {10, 60'000, 60'000, 5, 20});
        desk.hasKept(*server);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server->address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server->live.lines, 1);
        QTRY_VERIFY(server->live.isConnected());
        server->live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(wall.liveStream() != 0);
        const quint32 last = wall.liveStream();

        // The server is gone. What the machine's screen showed last stays, for as long as
        // nothing newer is known: not the snapshot from before it was had live.
        server.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.inContact(), 20000);
        QCOMPARE(wall.liveStream(), last);
        StandIn back([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, port);
        if (!back.isListening()) {
            QSKIP("Windows has given the port to somebody else in the meantime.");
        }
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.inContact(), 20000);
        QTRY_COMPARE(back.live.lines, 1);
    }

    void oneMachinesPictureThatFailsDoesNotTakeTheLiveMachineDown_data()
    {
        QTest::addColumn<QString>("how");
        QTest::newRow("it is larger than is taken") << u"too large"_s;
        QTest::newRow("it is longer on its way than a picture may be") << u"too slow"_s;
        QTest::newRow("a gateway answers for it with a page") << u"a page"_s;
    }

    void oneMachinesPictureThatFailsDoesNotTakeTheLiveMachineDown()
    {
        // The wall is up in two windows. One shows a machine large and has it live, over a line
        // of its own: keyboard and mouse go by that line. The other shows the wall, and there
        // the picture of the machine beside it goes wrong. That is one picture's failure: the
        // live line stays, the wall never says that contact is lost, and the other questions
        // are answered. (The server was given up on with every such picture: the look at the
        // machines that was out went with it, the wall went away, and the live line with that.)
        QFETCH(const QString, how);
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        bool wrong = false;
        int wentWrong = 0;
        StandIn server([&](const Asked& asked) {
            Answered answer = qkvm.answer(asked);
            if (asked.path == "/api/v1/status") {
                // (Slow enough for a look to be out most of the time.)
                answer.afterMs = 300;
            } else if (wrong && asked.path.startsWith("/api/v1/devices/b/snapshot")) {
                ++wentWrong;
                if (how == u"too large"_s) {
                    answer.moreToCome = static_cast<int>(core::largestPictureBytes) + 1;
                } else if (how == u"too slow"_s) {
                    // (The first of them: a picture's whole time is long enough to wait for once.)
                    answer.never = wentWrong == 1;
                } else {
                    answer = Answered{404, "<html>Not Found</html>", "text/html"};
                }
            }
            return answer;
        });
        // (The machines are looked at every 20 ms: the next look goes when the last has come.)
        Desk desk(0, 20);
        desk.hasKept(server);
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(second.wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_COMPARE(server.live.lines, 1);
        QTRY_VERIFY(server.live.isConnected());
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(wall.liveStream() != 0);
        const quint32 live = wall.liveStream();
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= 1);
        int saidAway = 0;
        connect(&desk.view, &ui::SessionView::changed, &second.wall,
                [&saidAway, &desk] { saidAway += desk.view.inContact() ? 0 : 1; });
        const int saidOfItself = server.count("/healthz");

        // The pictures of the machine beside it go wrong: three times, where that does not take
        // a picture's whole time; and what comes of the last of them has come.
        wrong = true;
        QTRY_VERIFY_WITH_TIMEOUT(wentWrong >= (how == u"too slow"_s ? 1 : 3), 30'000);
        QTest::qWait(how == u"too slow"_s ? 16'500 : 1000);
        wrong = false;
        QCOMPARE(server.live.lines, 1);
        QVERIFY(server.live.isConnected());
        QCOMPARE(wall.liveStream(), live);
        QCOMPARE(saidAway, 0);
        QVERIFY(desk.view.inContact());
        // What answers there was asked what it is where a gateway's page had come, and only there.
        if (how == u"a page"_s) {
            QVERIFY(server.count("/healthz") > saidOfItself);
        } else {
            QCOMPARE(server.count("/healthz"), saidOfItself);
        }
        // The looks go on, and are answered; and so are that machine's pictures again.
        const int looks = server.count("/api/v1/status");
        const int pictures = server.count("/api/v1/devices/b/snapshot");
        QTRY_VERIFY(server.count("/api/v1/status") >= looks + 2);
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/devices/b/snapshot") > pictures, 10'000);
        QCOMPARE(saidAway, 0);
        QCOMPARE(server.live.lines, 1);
    }

    void theWallIsWrittenToAFileByTheServerAndKeptWhereItWasSaid()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::FilesView files;
        const QString kept = desk.folder.filePath(u"wall.qkvm"_s);
        files.setDoor(doorTo(kept, {}));
        desk.session.serve(files);
        const QSignalSpy done(&files, &ui::FilesView::done);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);

        // The wrong password: said in the server's words, and nobody is signed out by it.
        files.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"not it"_s);
        QTRY_COMPARE(files.problem(), u"Wrong password"_s);
        QVERIFY(!files.isBusy());
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!QFile::exists(kept));

        files.exportWall({u"devices"_s, u"actions"_s}, u"a passphrase of mine"_s, u"the right one"_s);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(),
                 u"Exported. The file opens with its passphrase only: keep the two apart."_s);
        QCOMPARE(qkvm.exportsAsked.size(), 2);
        QCOMPARE(qkvm.exportsAsked[1], (QJsonObject{{u"password"_s, u"the right one"_s},
                                                    {u"passphrase"_s, u"a passphrase of mine"_s},
                                                    {u"parts"_s, QJsonArray{u"devices"_s, u"actions"_s}}}));
        QCOMPARE(written(kept), QJsonObject({{u"format"_s, u"qkvm-export"_s}, {u"sealed"_s, 2}}));
    }

    void aFileIsReadAndTakenInAndItsMachinesAreOnTheWall()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::FilesView files;
        const QString chosen = desk.folder.filePath(u"wall.json"_s);
        {
            QFile file(chosen);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"devices\": [\"as a qKVM wrote them\"]}");
        }
        files.setDoor(doorTo({}, chosen));
        desk.session.serve(files);
        const QSignalSpy done(&files, &ui::FilesView::done);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);

        files.chooseFile();
        QCOMPARE(files.fileName(), u"wall.json"_s);
        files.readFile({});
        QTRY_VERIFY(files.wasRead());
        QCOMPARE(qkvm.filesRead.size(), 1);
        QCOMPARE(qkvm.filesRead[0], (QJsonObject{{u"file"_s, u"{\"devices\": [\"as a qKVM wrote them\"]}"_s},
                                                 {u"passphrase"_s, QString()}}));
        QCOMPARE(files.parts().size(), 1);
        QCOMPARE(files.parts()[0].toMap().value(u"label"_s).toString(), u"KVMs and servers: 1 to add"_s);
        // Reading changed nothing.
        QCOMPARE(desk.wall.machines()->rowCount(), 3);

        files.takeIn({u"devices"_s}, {});
        QVERIFY(files.isTaking());
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"1 device added"_s);
        QCOMPARE(qkvm.filesTaken.size(), 1);
        QCOMPARE(qkvm.filesTaken[0]["take"_L1].toArray(), (QJsonArray{u"devices"_s}));
        // The wall follows by itself: the machine is there.
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 4);
    }

    void whoeverLeavesTheWallLeavesNoFileUnderWay()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        // The server takes its time over the first file, as it does with a passphrase.
        int exports = 0;
        StandIn server([&](const Asked& asked) {
            Answered answer = qkvm.answer(asked);
            if (asked.path == "/api/v1/export" && ++exports == 1) {
                answer.afterMs = 600;
            }
            return answer;
        });
        Desk desk;
        ui::FilesView files;
        const QString kept = desk.folder.filePath(u"wall.json"_s);
        files.setDoor(doorTo(kept, {}));
        desk.session.serve(files);
        const QSignalSpy done(&files, &ui::FilesView::done);
        // (Signed in by hand, so that the session after this one is another to the server.)
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn({}, u"the right one"_s, {}, false);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        files.exportWall({u"devices"_s}, {}, u"the right one"_s);
        QVERIFY(files.isBusy());
        QTRY_COMPARE(qkvm.exportsAsked.size(), 1);
        desk.view.signOut();
        QVERIFY(!files.isBusy());
        // Somebody signs in again and asks for a file before the server's first answer is there.
        // That answer belongs to nobody who is at the wall now: it is not what is written.
        desk.view.signIn({}, u"the right one"_s, {}, false);
        QTRY_VERIFY2(desk.view.stage() == u"wall"_s, qPrintable(desk.view.stage() + u": "_s + desk.view.problem()));
        files.exportWall({u"devices"_s}, {}, u"the right one"_s);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(written(kept), QJsonObject({{u"format"_s, u"qkvm-export"_s}, {u"made"_s, 2}}));
        QTest::qWait(900);
        QCOMPARE(done.count(), 1);
        QCOMPARE(written(kept), QJsonObject({{u"format"_s, u"qkvm-export"_s}, {u"made"_s, 2}}));
        QCOMPARE(files.problem(), QString());
    }

    void whatIsAskedOfAFileWhileNobodyIsAtTheWallGoesNowhere()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::FilesView files;
        const QString kept = desk.folder.filePath(u"wall.json"_s);
        files.setDoor(doorTo(kept, {}));
        desk.session.serve(files);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        // A box that was left open over the sign-in, with what was typed in it: pressed there, it
        // sends nothing, and is as a box that was opened anew.
        files.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"the right one"_s);
        QVERIFY(!files.isBusy());
        // And so for each of the three, however it came to be asked.
        emit files.exportAsked(1, {u"devices"_s}, {}, u"the right one"_s);
        emit files.readAsked(2, "{}", {});
        emit files.takeAsked(3, "{}", {}, {u"devices"_s});
        QTest::qWait(300);
        QCOMPARE(server.count("/api/v1/export") + server.count("/api/v1/import/read") + server.count("/api/v1/import"),
                 0);
        QVERIFY(qkvm.exportsAsked.isEmpty() && qkvm.filesRead.isEmpty() && qkvm.filesTaken.isEmpty());
        QVERIFY(!QFile::exists(kept));
    }

    void eachWindowsFilesAreItsOwnAndOneThatEndedIsAnsweredNoMore()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) {
            Answered answer = qkvm.answer(asked);
            if (asked.path == "/api/v1/export") {
                answer.afterMs = 300;
            }
            return answer;
        });
        Desk desk;
        desk.hasKept(server);
        ui::FilesView first;
        auto second = std::make_unique<ui::FilesView>();
        const QString keptByFirst = desk.folder.filePath(u"first.json"_s);
        const QString keptBySecond = desk.folder.filePath(u"second.json"_s);
        first.setDoor(doorTo(keptByFirst, {}));
        second->setDoor(doorTo(keptBySecond, {}));
        desk.session.serve(first);
        desk.session.serve(*second);
        const QSignalSpy done(&first, &ui::FilesView::done);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        // Both ask at once. One window is closed before its answer is there: that answer is
        // nobody's, and the other window's file is its own.
        second->exportWall({u"devices"_s}, {}, u"the right one"_s);
        first.exportWall({u"devices"_s}, {}, u"the right one"_s);
        QVERIFY(first.isBusy() && second->isBusy());
        QTRY_COMPARE(qkvm.exportsAsked.size(), 2);
        second.reset();
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(QFile::exists(keptByFirst));
        QTest::qWait(400);
        QVERIFY(!QFile::exists(keptBySecond));
        // Whoever leaves the wall leaves it in every window that is there.
        first.exportWall({u"devices"_s}, {}, u"the right one"_s);
        QVERIFY(first.isBusy());
        desk.view.signOut();
        QVERIFY(!first.isBusy());
    }

    void whatIsTypedForAFileGoesOnlyToWhatWasJustAskedWhatItIsAndIsAQkvm()
    {
        // The wall is up, and the address passes to something else: a gateway that answers the
        // wall's own looks as the QKVM did (so that the wall stays up while this is tried), and
        // takes whatever is posted to it. The password of whoever is signed in, which an export
        // posts once more, is not for that to have; nor is a file that is handed in, which may
        // hold the passwords of machines, or its passphrase.
        Pretended qkvm;
        qkvm.signedIn = true;
        bool replaced = false;
        StandIn server([&](const Asked& asked) {
            return replaced && asked.path == "/healthz" ? Answered{200, "<html>Welcome</html>", "text/html"}
                                                        : qkvm.answer(asked);
        });
        const auto lastAsked = [&server](const QByteArray& path) {
            qsizetype found = -1;
            for (qsizetype at = 0; at < server.asked.size(); ++at) {
                found = server.asked[at].path == path ? at : found;
            }
            return found;
        };
        Desk desk;
        desk.hasKept(server);
        ui::FilesView files;
        const QString kept = desk.folder.filePath(u"wall.qkvm"_s);
        const QString chosen = desk.folder.filePath(u"another.qkvm"_s);
        {
            QFile file(chosen);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"format\": \"qkvm-export\", \"sealed\": {\"with\": \"the machines' passwords\"}}");
        }
        files.setDoor(doorTo(kept, chosen));
        desk.session.serve(files);
        const QSignalSpy done(&files, &ui::FilesView::done);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QCOMPARE(server.count("/healthz"), 1);

        // While the QKVM answers there: it is asked what it is, with nothing that says who asks,
        // and only then is anything posted. Once for each of the three.
        files.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"the right one"_s);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(server.count("/healthz"), 2);
        QVERIFY(lastAsked("/healthz") < lastAsked("/api/v1/export"));
        QVERIFY(!server.asked[lastAsked("/healthz")].headers.contains("cookie"));
        QVERIFY(server.asked[lastAsked("/healthz")].body.isEmpty());
        files.chooseFile();
        files.readFile(u"of the other file"_s);
        QTRY_VERIFY(files.wasRead());
        QCOMPARE(server.count("/healthz"), 3);
        QVERIFY(lastAsked("/healthz") < lastAsked("/api/v1/import/read"));
        files.takeIn({u"devices"_s}, u"of the other file"_s);
        QTRY_COMPARE(done.count(), 2);
        QCOMPARE(server.count("/healthz"), 4);
        QVERIFY(lastAsked("/healthz") < lastAsked("/api/v1/import"));
        QVERIFY(!server.asked[lastAsked("/healthz")].headers.contains("cookie"));

        // Something else answers there now.
        replaced = true;
        const qsizetype askedBefore = server.asked.size();
        const QString notAQkvm = u"The server answered in a way this program does not understand."_s;
        files.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"the right one"_s);
        QVERIFY(files.isBusy());
        QTRY_VERIFY(!files.isBusy());
        QCOMPARE(files.problem(), notAQkvm);
        files.chooseFile();
        files.readFile(u"of the other file"_s);
        QVERIFY(files.isBusy());
        QTRY_VERIFY(!files.isBusy());
        QCOMPARE(files.problem(), notAQkvm);
        QVERIFY(!files.wasRead());
        // (A file that the QKVM had read before something else stood there.)
        replaced = false;
        files.readFile(u"of the other file"_s);
        QTRY_VERIFY(files.wasRead());
        replaced = true;
        files.takeIn({u"devices"_s}, u"of the other file"_s);
        QVERIFY(files.isBusy());
        QTRY_VERIFY(!files.isBusy());
        // Nothing was sent, so nothing can have been taken in: that is not left open.
        QCOMPARE(files.problem(), notAQkvm);
        // (A picture of the wall's that left while the QKVM still answered there is counted by
        // the server when it has come, which may be a moment after this: this test read that
        // moment too, and failed one run in a dozen. What is asked from here on is what
        // counts: some looks of the wall's, at its pace.)
        QTest::qWait(300);
        const qsizetype sinceItSaidSo = server.asked.size();
        QTest::qWait(600);
        // None of it went anywhere: not the password, not the passphrases, and not the file.
        QCOMPARE(server.count("/api/v1/export"), 1);
        QCOMPARE(server.count("/api/v1/import/read"), 2);
        // (Whatever was asked at a place that begins so: the two readings, and the one taking.)
        QCOMPARE(server.count("/api/v1/import"), 3);
        QCOMPARE(qkvm.exportsAsked.size(), 1);
        QCOMPARE(qkvm.filesTaken.size(), 1);
        QCOMPARE(done.count(), 2);
        int askedWhatItIs = 0;
        for (qsizetype at = askedBefore; at < server.asked.size(); ++at) {
            const Asked& one = server.asked[at];
            if (one.path == "/healthz") {
                ++askedWhatItIs;
                QVERIFY(!one.headers.contains("cookie"));
                QVERIFY(one.body.isEmpty());
            }
            if (one.path != "/api/v1/import/read") {
                QVERIFY2(!one.body.contains("the right one") && !one.body.contains("passphrase") &&
                             !one.body.contains("the machines' passwords"),
                         one.path.constData());
            }
        }
        // (Four times at the least: for the export, twice for a reading, and for the taking.
        // Turned round: this counted exactly four, and had the wall's own looks go on to what
        // stood there, with the session, "so that the wall stays up". Since something that is no
        // QKVM answered there, the wall's own questions have it asked what it is as well, each
        // time, and are not sent: nothing but that question reaches it.)
        QVERIFY2(askedWhatItIs >= 4, QByteArray::number(askedWhatItIs).constData());
        QVERIFY2(server.asked.size() > sinceItSaidSo, "the wall asked nothing meanwhile: nothing was shown");
        for (qsizetype at = sinceItSaidSo; at < server.asked.size(); ++at) {
            QCOMPARE(server.asked[at].path, QByteArray("/healthz"));
        }
        // The wall stays, and says that contact is lost. When the QKVM answers there again, the
        // wall is back, with the machine the file brought.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QTRY_VERIFY(!desk.view.inContact());
        replaced = false;
        QTRY_VERIFY(desk.view.inContact());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 4);
    }

    void whatWasTypedForAFileGoesNowhereWhereTheWallWasLeftOrComeToAgainMeanwhile_data()
    {
        QTest::addColumn<bool>("comesBack");
        QTest::newRow("the wall is left while its server is asked what it is") << false;
        QTest::newRow("the session goes, and the wall is come to again by its link, meanwhile") << true;
    }

    void whatWasTypedForAFileGoesNowhereWhereTheWallWasLeftOrComeToAgainMeanwhile()
    {
        // A file is asked for, with the password of whoever sits there typed once more. Before
        // it is posted, what answers at the server's address is asked what it is, and that
        // answer takes a while. Whoever typed it was at the wall then: where the wall is not
        // the one it was by the time the answer comes, the password goes nowhere.
        QFETCH(bool, comesBack);
        Pretended qkvm;
        bool slowOnce = false;
        StandIn server([&](const Asked& asked) {
            Answered answer = qkvm.answer(asked);
            if (asked.path == "/healthz" && std::exchange(slowOnce, false)) {
                answer.afterMs = 700;
            }
            return answer;
        });
        Desk desk;
        ui::FilesView files;
        files.setDoor(doorTo(desk.folder.filePath(u"wall.qkvm"_s), QString()));
        desk.session.serve(files);
        const QSignalSpy done(&files, &ui::FilesView::done);
        if (comesBack) {
            desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        } else {
            qkvm.signedIn = true;
            desk.hasKept(server);
            desk.view.connectTo(server.address());
        }
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        const int askedWhatItIs = server.count("/healthz");
        slowOnce = true;
        files.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"the right one"_s);
        if (comesBack) {
            // (R4b: a screen opened by a display link only shows, and asks nothing of the wall as
            // a file: whoever asked is told so at once, and what answers there is not even asked
            // what it is.)
            QTRY_VERIFY(!files.isBusy());
            QCOMPARE(files.problem(), u"This PC only shows the wall: nothing is done from it. Nothing was sent."_s);
            QCOMPARE(server.count("/healthz"), askedWhatItIs);
            // The server knows no session from before: the wall's next look says so, and the
            // screen is back at its wall by its link, with another session.
            qkvm.signedIn = false;
            QTRY_COMPARE(qkvm.displaysOpened, 2);
            QTRY_VERIFY(desk.view.inContact());
        } else {
            QTRY_COMPARE(server.count("/healthz"), askedWhatItIs + 1);
            desk.view.anotherServer();
            QCOMPARE(desk.view.stage(), u"address"_s);
        }
        QTest::qWait(1000);
        QCOMPARE(server.count("/api/v1/export"), 0);
        QVERIFY(qkvm.exportsAsked.isEmpty());
        QCOMPARE(done.count(), 0);
        for (const Asked& one : std::as_const(server.asked)) {
            QVERIFY2(!one.body.contains("the right one") && !one.body.contains("a passphrase of mine"),
                     one.path.constData());
        }
    }

    void aWindowThatComesWhileTheWallIsUpHasItAtOnce()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= 1);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= 1);
        // The pictures are paused: none is asked for.
        desk.wall.setPace(0);
        QTest::qWait(400);
        const int first = server.count("/api/v1/devices/a/snapshot");
        const int other = server.count("/api/v1/devices/b/snapshot");
        QTest::qWait(600);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), first);

        // A window more: it has the machines, whoever is signed in may act from it, and each
        // machine it shows is given a picture for it, paused or not; and one only.
        auto second = std::make_unique<Window>();
        desk.session.add(second->wall, second->actions, nullptr);
        QCOMPARE(second->wall.machines()->rowCount(), 3);
        QVERIFY(second->actions.allowed());
        // The pace the server named is one of its choices as well, and holds in it.
        QCOMPARE(second->wall.pace(), 250);
        // Whoever signed in sees the wall as a person does, in this window too.
        QVERIFY(!second->wall.displaying());
        QTRY_COMPARE(server.count("/api/v1/devices/a/snapshot"), first + 1);
        QTRY_COMPARE(server.count("/api/v1/devices/b/snapshot"), other + 1);
        QTest::qWait(600);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), first + 1);

        // The window is closed: what it showed is asked for no more, and its wall is left as it
        // was, whatever becomes of the machines.
        desk.wall.setPace(250);
        desk.wall.setInSight(0, 0);
        second->wall.setInSight(1, 1);
        int ofTheOther = server.count("/api/v1/devices/b/snapshot");
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= ofTheOther + 3);
        desk.session.remove(second->wall);
        QTest::qWait(300);
        ofTheOther = server.count("/api/v1/devices/b/snapshot");
        QTest::qWait(700);
        QVERIFY(server.count("/api/v1/devices/b/snapshot") <= ofTheOther);
        qkvm.status = R"({"a": {"state": "offline"}, "b": {"state": "no_signal"},
                          "s": {"state": "online", "picture": false, "server": {"health": "OK"}}})";
        QTRY_COMPARE(desk.wall.machines()->attention(), 1);
        QCOMPARE(second->wall.machines()->attention(), 0);
        // What it says from now on is nobody's.
        second->wall.setInSight(0, 2);
        second->actions.open(desk.key(u"b"_s));
        QTest::qWait(200);
        QVERIFY(second->actions.menu().isEmpty());
        QVERIFY(server.count("/api/v1/devices/b/snapshot") <= ofTheOther);
        second.reset();
        // And the first goes on as before.
        const int still = server.count("/api/v1/devices/a/snapshot");
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= still + 3);
    }

    void aWindowThatIsTakenAwayIsAskedForNoMoreFromThatMoment()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        // The machines are looked at as seldom as the program looks: nothing but the window's
        // going makes its pictures stop in the seconds between two looks.
        Desk desk(0, 5000);
        desk.hasKept(server);
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        // Each window shows one machine, and not the other's.
        desk.wall.setInSight(0, 0);
        second.wall.setInSight(1, 1);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= 3);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= 3);

        desk.session.remove(second.wall);
        QTest::qWait(300);
        const int ofTheOther = server.count("/api/v1/devices/b/snapshot");
        const int ofTheFirst = server.count("/api/v1/devices/a/snapshot");
        QTest::qWait(900);
        QCOMPARE(server.count("/api/v1/devices/b/snapshot"), ofTheOther);
        QVERIFY(server.count("/api/v1/devices/a/snapshot") >= ofTheFirst + 2);
    }

    void aWindowThatComesToAScreenNobodySitsAtIsOnlyShownToo()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        Window beside;
        desk.session.add(beside.wall, beside.actions, nullptr);
        // Opened with a display link: the wall and nothing else, in every window there is.
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QVERIFY(desk.wall.displaying());
        QVERIFY(beside.wall.displaying());
        // And in one that comes later.
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        QVERIFY(second.wall.displaying());
        QCOMPARE(second.wall.machines()->rowCount(), 3);
        // And nothing is done to a machine from it: a screen may only look.
        QVERIFY(!second.actions.allowed());
    }

    void anAdminMakesTheWallsOwnOrderAndEveryWindowHasItAtOnce()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        // (The machines are looked at seldom: the order reaches the walls by the server's answer
        // to it, and not by a look that happens to come.)
        Desk desk(0, 5000);
        desk.hasKept(server);
        Window second;
        desk.session.add(second.wall, second.actions, nullptr);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QCOMPARE(keysOf(desk.wall), (QStringList{desk.key(u"a"_s), desk.key(u"b"_s), desk.key(u"s"_s)}));

        desk.wall.arrange();
        desk.wall.placeAt(desk.key(u"s"_s), 0);
        desk.wall.saveOrder();
        QVERIFY(desk.wall.isSavingOrder());
        QTRY_VERIFY(!desk.wall.arranging());
        // Sent as it was made, once.
        QCOMPARE(qkvm.arranged, (QList<QStringList>{{u"s"_s, u"a"_s, u"b"_s}}));
        // The wall's own order is that one from this moment, in the window it was made in and in
        // the one beside it; the wall was not asked for a second time for it.
        QCOMPARE(keysOf(desk.wall), (QStringList{desk.key(u"s"_s), desk.key(u"a"_s), desk.key(u"b"_s)}));
        QCOMPARE(keysOf(second.wall), (QStringList{desk.key(u"s"_s), desk.key(u"a"_s), desk.key(u"b"_s)}));
        int timesTheWallWasAsked = 0;
        for (const Asked& one : std::as_const(server.asked)) {
            timesTheWallWasAsked += one.method == "GET" && one.path == "/api/v1/wall" ? 1 : 0;
        }
        QCOMPARE(timesTheWallWasAsked, 1);
        // Told in the window it was made in, and in no other.
        QCOMPARE(desk.actions.notices()->count(), 1);
        QCOMPARE(desk.actions.notices()->index(0, 0).data(ui::Notices::KindRole).toString(), u"ok"_s);
        QCOMPARE(desk.actions.notices()->index(0, 0).data(ui::Notices::TextRole).toString(),
                 u"The order is saved, for everybody who looks at this wall"_s);
        QCOMPARE(second.actions.notices()->count(), 0);
    }

    void anOrderTheServerDoesNotTakeIsToldAndStaysAsItWasMade()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.refusesToArrange = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk(0, 5000);
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        desk.wall.arrange();
        desk.wall.placeAt(desk.key(u"b"_s), 0);
        desk.wall.saveOrder();
        QTRY_VERIFY(!desk.wall.isSavingOrder());
        // Said in the server's words; the order is still as it was made, and still being made.
        QVERIFY(desk.wall.arranging());
        QCOMPARE(desk.wall.arrangement(), (QStringList{desk.key(u"b"_s), desk.key(u"a"_s), desk.key(u"s"_s)}));
        QCOMPARE(keysOf(desk.wall), (QStringList{desk.key(u"a"_s), desk.key(u"b"_s), desk.key(u"s"_s)}));
        QCOMPARE(desk.actions.notices()->count(), 1);
        QCOMPARE(desk.actions.notices()->index(0, 0).data(ui::Notices::KindRole).toString(), u"error"_s);
        QCOMPARE(desk.actions.notices()->index(0, 0).data(ui::Notices::TextRole).toString(),
                 u"Only an admin can do that"_s);
        // Sent again when the server takes it.
        qkvm.refusesToArrange = false;
        desk.wall.saveOrder();
        QTRY_VERIFY(!desk.wall.arranging());
        QCOMPARE(keysOf(desk.wall), (QStringList{desk.key(u"b"_s), desk.key(u"a"_s), desk.key(u"s"_s)}));
    }

    void theWallsOwnOrderIsAnAdminsToMake()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.user = R"({"id": "u2", "name": "olga", "role": "operator", "token": false})";
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        // Whoever may act on machines, and no more: an order that such a one's screen asks for
        // is not sent, and is not left on its way.
        desk.wall.arrange();
        desk.wall.placeAt(desk.key(u"s"_s), 0);
        desk.wall.saveOrder();
        QVERIFY(!desk.wall.isSavingOrder());
        QTest::qWait(200);
        QCOMPARE(server.count("/api/v1/wall/order"), 0);
        desk.wall.stopArranging();

        // An admin who is in the middle of it, and is made an operator elsewhere: out of it, once
        // the server has said so.
        qkvm.user = R"({"id": "u2", "name": "olga", "role": "admin", "token": false})";
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.manages(), 8000);
        desk.wall.arrange();
        QVERIFY(desk.wall.arranging());
        qkvm.user = R"({"id": "u2", "name": "olga", "role": "operator", "token": false})";
        QTRY_VERIFY_WITH_TIMEOUT(!desk.wall.arranging(), 8000);
        QVERIFY(!desk.view.manages());
    }

    void whoeverLeavesTheWallIsNotLeftArrangingIt()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        desk.wall.arrange();
        desk.wall.placeAt(desk.key(u"s"_s), 0);
        desk.view.signOut();
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!desk.wall.arranging());
    }

    void picturesAreAskedOnlyForCardsThatAreInSight()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        // Until a screen has said which cards are in sight, all of them are.
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= 1);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= 1);

        // The screen says: the first card, and no other.
        desk.wall.setInSight(0, 0);
        int first = server.count("/api/v1/devices/a/snapshot");
        int second = server.count("/api/v1/devices/b/snapshot");
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= first + 4);
        // (One that was on its way when the screen said so may still have come.)
        QVERIFY(server.count("/api/v1/devices/b/snapshot") <= second + 1);

        // What the bar takes off the wall asks for nothing either: of the machines without a
        // signal the second is the only one, and now it is the one in sight.
        desk.wall.shown()->setShow(u"no_signal"_s);
        first = server.count("/api/v1/devices/a/snapshot");
        second = server.count("/api/v1/devices/b/snapshot");
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= second + 4);
        QVERIFY(server.count("/api/v1/devices/a/snapshot") <= first + 1);
    }

    void aWallNobodyCanSeeAsksForNoPicturesAndStillLooksHowTheMachinesAre()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= 2);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= 2);

        // The window is minimized, or out of sight behind the program's icon.
        desk.wall.setUnseen(true);
        QTest::qWait(300);
        const int first = server.count("/api/v1/devices/a/snapshot");
        const int second = server.count("/api/v1/devices/b/snapshot");
        const int looks = server.count("/api/v1/status");
        QTRY_VERIFY(server.count("/api/v1/status") >= looks + 5);
        // (One that was on its way when the window went may still have come.)
        QVERIFY(server.count("/api/v1/devices/a/snapshot") <= first + 1);
        QVERIFY(server.count("/api/v1/devices/b/snapshot") <= second + 1);
        // What the icon says of the wall is kept up meanwhile.
        qkvm.status = R"({"a": {"state": "offline"}, "b": {"state": "no_signal"},
                          "s": {"state": "online", "picture": false, "server": {"health": "OK"}}})";
        QTRY_COMPARE(desk.wall.machines()->attention(), 1);

        // Nor of a machine that was shown large when the window went.
        desk.wall.enlarge(desk.key(u"b"_s));
        QTest::qWait(300);
        QVERIFY(server.count("/api/v1/devices/b/snapshot") <= second + 1);
        desk.wall.closeEnlarged();

        // In sight again: pictures at once, and from then on.
        desk.wall.setUnseen(false);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= second + 4);
    }

    void aWallStopsAskingTheMomentItGoesOutOfSightAndAsksTheMomentItIsBack()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        // The machines are looked at as seldom as the program looks: nothing but the wall's own
        // word makes the pictures stop or come in the seconds between two looks.
        Desk desk(0, 5000);
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot") >= 2);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot") >= 2);

        desk.wall.setUnseen(true);
        QTest::qWait(300);
        const int first = server.count("/api/v1/devices/a/snapshot");
        const int second = server.count("/api/v1/devices/b/snapshot");
        QTest::qWait(1200);
        // (One that was on its way when the window went may still have come.)
        QVERIFY(server.count("/api/v1/devices/a/snapshot") <= first + 1);
        QVERIFY(server.count("/api/v1/devices/b/snapshot") <= second + 1);

        desk.wall.setUnseen(false);
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/devices/a/snapshot") >= first + 3, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/devices/b/snapshot") >= second + 3, 2000);
        // (All of that between two looks at the machines.)
        QVERIFY(server.count("/api/v1/status") <= 2);
    }

    void picturesAreAskedForAsOftenAsWasChosenOnTheWall()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const auto pictures = [&] { return server.count("/api/v1/devices/a/snapshot"); };
        {
            Desk desk;
            desk.hasKept(server);
            desk.view.connectTo(server.address());
            QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
            // At the server's pace, four a second: which is then one of the choices.
            QTRY_VERIFY(pictures() >= 3);
            QCOMPARE(desk.wall.pace(), 250);
            QVERIFY(desk.wall.paces().contains(250));

            // Paused on the wall: none are asked for.
            desk.wall.setPace(0);
            QTest::qWait(300);
            const int before = pictures();
            QTest::qWait(800);
            QCOMPARE(pictures(), before);
            // And asked for again, at once.
            desk.wall.setPace(10000);
            QTRY_COMPARE_WITH_TIMEOUT(pictures(), before + 1, 1000);
            QTest::qWait(800);
            QCOMPARE(pictures(), before + 1);
        }

        // A pace that was chosen before the wall was come to (remembered from the last time) holds
        // from the first picture on.
        const int before = pictures();
        Desk desk(5000);
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(pictures(), before + 1);
        QTest::qWait(1200);
        QCOMPARE(pictures(), before + 1);
        QCOMPARE(desk.wall.pace(), 5000);
    }

    void aMachineShownLargeIsAskedForAloneLargerAndFaster()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        // A wall whose own pace is slow: a picture every two seconds.
        qkvm.wall.replace("\"interval\": 0.25", "\"interval\": 2");
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        desk.wall.setCardPixels(300);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(server.count("/api/v1/devices/a/snapshot?w=600&") >= 1);
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot?w=600&") >= 1);

        // The second machine is shown large, 1500 pixels wide.
        desk.wall.setEnlargedPixels(1500);
        desk.wall.enlarge(desk.wall.machines()->index(1, 0).data(WallModel::KeyRole).toString());
        // Pictures of it come as large as it is drawn, and four a second however slow the wall is.
        QTRY_VERIFY(server.count("/api/v1/devices/b/snapshot?w=3000&") >= 6);
        // And one that somebody else took an eighth of a second ago will do: half the pace.
        QByteArray asked;
        for (const Asked& one : std::as_const(server.asked)) {
            asked = one.path.startsWith("/api/v1/devices/b/snapshot?w=3000&") ? one.path : asked;
        }
        QVERIFY2(asked.endsWith("age=0.13") || asked.endsWith("age=0.12"), asked.constData());
        // And of no other machine meanwhile: not at the wall's pace either, which is one in two
        // seconds here.
        QVERIFY(server.count("/api/v1/devices/a/snapshot") <= 2);
        const int ofTheOther = server.count("/api/v1/devices/a/snapshot");
        QTest::qWait(2600);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), ofTheOther);

        // Back on the wall: at the wall's pace and the cards' width again, for all that are in sight.
        const int first = server.count("/api/v1/devices/a/snapshot?w=600&");
        const int large = server.count("/api/v1/devices/b/snapshot?w=3000&");
        desk.wall.closeEnlarged();
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/devices/a/snapshot?w=600&") > first, 6000);
        QVERIFY(server.count("/api/v1/devices/b/snapshot?w=3000&") <= large + 1);
    }

    void anAnswerToWhatWasAskedBeforeGoingElsewhereIsIgnored()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        ui::SessionView& view = desk.view;
        view.connectTo(server.address());
        QVERIFY(view.busy());
        // Not waited for: back to saying which server, before the answer is here.
        view.anotherServer();
        QCOMPARE(view.stage(), u"address"_s);
        QVERIFY(!view.busy());
        // The answer comes, and moves nothing: nobody is asked for a password to a server they
        // left, and the server is not asked who is signed in.
        QTRY_COMPARE(server.count("/healthz"), 1);
        QTest::qWait(300);
        QCOMPARE(view.stage(), u"address"_s);
        QVERIFY(!view.busy());
        QCOMPARE(server.asked.size(), 1);
    }

    void oneServersAnswerIsNotTakenForAnothers()
    {
        // The first server would say that somebody is signed in. The second says nothing at all.
        Pretended first;
        first.signedIn = true;
        StandIn firstServer([&first](const Asked& asked) { return first.answer(asked); });
        StandIn secondServer([](const Asked&) {
            Answered nothing;
            nothing.never = true;
            return nothing;
        });
        Desk desk;
        ui::SessionView& view = desk.view;
        desk.hasKept(firstServer);
        // The first is named and left at once for the second, before its answer is here.
        view.connectTo(firstServer.address());
        view.anotherServer();
        view.connectTo(secondServer.address());
        QTRY_COMPARE(secondServer.count("/healthz"), 1);
        QTest::qWait(300);
        // The second is still being asked. Whatever came of the question to the first, its answer
        // or the news that it was dropped: it is not the second's, and nobody is on the second's
        // wall on the first one's word.
        QCOMPARE(view.stage(), u"address"_s);
        QVERIFY(view.busy());
        QCOMPARE(view.account(), QString());
        QCOMPARE(view.address(), u"127.0.0.1:%1"_s.arg(secondServer.serverPort()));
        QCOMPARE(secondServer.count("/api/v1/wall"), 0);
    }

    // --- this PC's own wall: KVMs the program speaks to itself, with no server ---

    void withNoServerEverUsedTheProgramStartsIntoThisPcsOwnWall()
    {
        Desk desk;
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        const MadeUpKvm kvm;
        const QString id = own.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        desk.session.useOwnWall(own);
        desk.session.begin();

        const ui::SessionView& view = desk.view;
        QCOMPARE(view.stage(), u"wall"_s);
        QVERIFY(view.ownWall());
        // Nobody is signed in to anything, and whoever sits here manages this wall.
        QVERIFY(view.manages());
        QCOMPARE(view.account(), QString());
        QCOMPARE(view.address(), QString());
        QVERIFY(view.inContact());
        QVERIFY(!view.busy());
        // Its KVMs are the wall's machines, and may be acted on.
        QTRY_COMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});
        QVERIFY(desk.actions.allowed());
        QTRY_COMPARE(desk.wall.machines()->index(0, 0).data(WallModel::StateRole).toString(), u"online"_s);
        QCOMPARE(desk.wall.machines()->index(0, 0).data(WallModel::NameRole).toString(), u"Build server"_s);
        // And that this wall was the one shown is remembered.
        QVERIFY(desk.settings.remembered().showsOwnWall);
    }

    void thisPcsOwnWallSaysWhereAKvmIsSpokenToOverPlainHttp()
    {
        Desk desk;
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        desk.session.useOwnWall(own);
        desk.session.begin();
        // Nothing on the wall: nothing travels anywhere.
        QVERIFY(desk.view.ownWall());
        QVERIFY(desk.view.encrypted());

        // A KVM over HTTPS (nothing answers there, which is nothing to this).
        core::OwnKvm encrypted;
        encrypted.address = u"https://127.0.0.1:9"_s;
        encrypted.name = u"Build server"_s;
        const QString first = own.add(encrypted, u"secret"_s).value();
        QVERIFY(desk.view.encrypted());
        // One over plain HTTP: said at once, and for as long as it is on the wall.
        const MadeUpKvm kvm;
        const QString second = own.add(at(kvm, u"NAS"_s), u"secret"_s).value();
        QVERIFY(!desk.view.encrypted());
        QTRY_COMPARE(keysOf(desk.wall), (QStringList{desk.ownKey(first), desk.ownKey(second)}));
        QCOMPARE(desk.wall.machines()->index(0, 0).data(WallModel::HostRole).toString(), u"127.0.0.1:9"_s);
        QCOMPARE(desk.wall.machines()->index(1, 0).data(WallModel::HostRole).toString(),
                 u"http://127.0.0.1:%1"_s.arg(kvm.stand->serverPort()));
        QVERIFY(own.remove(second).ok());
        QVERIFY(desk.view.encrypted());
    }

    void aKvmThatIsAddedComesOntoTheWallByItself()
    {
        // A wall whose machines are looked at every eight seconds, as one is that is in no hurry.
        Desk desk(0, 8000);
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        desk.session.useOwnWall(own);
        desk.session.begin();
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(keysOf(desk.wall).isEmpty());

        // It is on the wall at once, and not at the wall's next look: also while the wall, which
        // was come to this moment, is still being asked what is on it.
        const MadeUpKvm kvm;
        const QString id = own.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        QTRY_COMPARE_WITH_TIMEOUT(keysOf(desk.wall), QStringList{desk.ownKey(id)}, 600);
        // How it is, is asked again soon: it is being signed in to, and is there a moment later.
        QTRY_COMPARE_WITH_TIMEOUT(desk.wall.machines()->index(0, 0).data(WallModel::StateRole).toString(), u"online"_s,
                                  4000);
        // And one that is taken off goes at once.
        QVERIFY(own.remove(id).ok());
        QTRY_VERIFY_WITH_TIMEOUT(keysOf(desk.wall).isEmpty(), 600);
    }

    void fromThisPcsOwnWallToAServerAndBack()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        const MadeUpKvm kvm;
        const QString id = own.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        desk.session.useOwnWall(own);
        desk.session.begin();
        ui::SessionView& view = desk.view;
        QTRY_COMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});

        // "Connect to a QKVM server": the wall is left, with nothing of it shown any more.
        const QSignalSpy left(&desk.session, &ui::ServerSession::wallWasLeft);
        view.anotherServer();
        QCOMPARE(view.stage(), u"address"_s);
        QVERIFY(!view.ownWall());
        QVERIFY(!view.manages());
        QVERIFY(keysOf(desk.wall).isEmpty());
        QVERIFY(!desk.actions.allowed());
        QCOMPARE(left.count(), 1);

        // The server's wall: its machines, and none of this PC's own among them.
        view.connectTo(server.address());
        QTRY_COMPARE(view.stage(), u"wall"_s);
        QVERIFY(!view.ownWall());
        QTRY_VERIFY(!keysOf(desk.wall).isEmpty());
        QVERIFY(!keysOf(desk.wall).contains(desk.ownKey(id)));
        QVERIFY(!desk.settings.remembered().showsOwnWall);
        // What a menu asks for here goes to the server, and not to this PC's own wall.
        QTRY_VERIFY(desk.actions.allowed());
        QTRY_COMPARE(onTheCardOf(desk.wall, desk.key(u"a"_s), WallModel::StateRole), u"online"_s);
        desk.actions.open(desk.key(u"a"_s));
        desk.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(server.count("/api/v1/devices/a/actions"), 1);
        desk.actions.close();
        const int asked = server.count("/api/v1/status");

        // And back: the server's wall is left without signing out there, and is asked no more.
        view.goToOwnWall();
        QCOMPARE(view.stage(), u"wall"_s);
        QVERIFY(view.ownWall());
        QTRY_COMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});
        QVERIFY(desk.settings.remembered().showsOwnWall);
        QCOMPARE(server.count("/api/v1/logout"), 0);
        QVERIFY(desk.secrets.all.contains(Desk::nameFor(server)));
        QTest::qWait(500);
        QVERIFY(server.count("/api/v1/status") <= asked + 1);
        // Nor who is signed in there, which a server's wall asks with every fifteenth look: here
        // nobody is signed in to anything. (Twenty looks of this wall.)
        const qsizetype askedOfIt = server.asked.size();
        QTest::qWait(2000);
        QCOMPARE(server.asked.size(), askedOfIt);
    }

    void theWallThatWasShownLastIsTheOneTheProgramStartsInto()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        // A server was used once, and this PC's own wall was shown last: the program starts into
        // that, and asks nothing of the server.
        {
            Desk desk;
            desk.hasKept(server);
            core::Remembered remembered = desk.settings.remembered();
            remembered.server = server.address() + u'/';
            remembered.showsOwnWall = true;
            QVERIFY(desk.settings.remember(remembered).ok());
            core::LocalWall own(desk.settings, desk.secrets, quickKvms());
            desk.session.useOwnWall(own);
            desk.session.begin();
            QCOMPARE(desk.view.stage(), u"wall"_s);
            QVERIFY(desk.view.ownWall());
            QTest::qWait(300);
            QVERIFY(server.asked.isEmpty());
            // The server that was used last is what the address's line begins with all the same.
            QCOMPARE(desk.view.suggested(), u"127.0.0.1:%1"_s.arg(server.serverPort()));
        }
        // The server's wall was shown last: the program goes to the server.
        {
            Desk desk;
            desk.hasKept(server);
            core::Remembered remembered = desk.settings.remembered();
            remembered.server = server.address() + u'/';
            QVERIFY(desk.settings.remember(remembered).ok());
            core::LocalWall own(desk.settings, desk.secrets, quickKvms());
            desk.session.useOwnWall(own);
            desk.session.begin();
            QTRY_COMPARE(desk.view.stage(), u"wall"_s);
            QVERIFY(!desk.view.ownWall());
        }
        // And a program that has no wall of its own goes to the server, whatever is remembered.
        {
            Desk desk;
            desk.hasKept(server);
            core::Remembered remembered = desk.settings.remembered();
            remembered.server = server.address() + u'/';
            remembered.showsOwnWall = true;
            QVERIFY(desk.settings.remember(remembered).ok());
            desk.session.begin();
            QTRY_COMPARE(desk.view.stage(), u"wall"_s);
            QVERIFY(!desk.view.ownWall());
        }
    }

    void whatAMenuAsksForAtThisPcsOwnWallGoesToTheKvmAndToNoServer()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        QList<QUrl> opened;
        desk.session.openPagesWith([&](const QUrl& address) {
            opened.append(address);
            return true;
        });
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        const MadeUpKvm kvm;
        const QString id = own.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        desk.session.useOwnWall(own);

        // At a server's wall first, and from there to this PC's own: what the windows do things
        // with goes over with the wall.
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_VERIFY(!keysOf(desk.wall).isEmpty());
        desk.view.goToOwnWall();
        QTRY_COMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});
        QTRY_COMPARE(desk.wall.machines()->index(0, 0).data(WallModel::StateRole).toString(), u"online"_s);
        const qsizetype askedOfTheServer = server.asked.size();

        desk.actions.open(desk.ownKey(id));
        QCOMPARE(desk.actions.menu().value(u"title"_s).toString(), u"Build server"_s);
        desk.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(kvm.asked("/api/hid/events/send_shortcut"), 1);

        // There is no page of a server's here, whoever asks for it (the program's icon does).
        desk.view.openPage();
        QTest::qWait(300);
        QVERIFY(opened.isEmpty());
        // And the server heard of none of it.
        QCOMPARE(server.asked.size(), askedOfTheServer);
    }

    void aTileOfThisPcsWallSaysWhatItsKvmCameToWithinAMomentAndNotAtTheNextLook()
    {
        // The machines are looked at every four seconds, as in the program.
        Desk desk(0, 4000);
        // A KVM over plain HTTP that was added in an earlier run of the program: in this run it
        // is asked what it is before it is signed in to. And an asking gives up its place at
        // once here, so that whoever asks how the KVMs are is not kept waiting for it: the first
        // answer has the KVM "connecting", as a wall has those of its KVMs whose asking takes
        // longer than a place is held.
        const MadeUpKvm kvm;
        QString id;
        {
            core::LocalWall earlier(desk.settings, desk.secrets, quickKvms());
            id = earlier.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        }
        core::LocalWall::Own ownTimes;
        ownTimes.placeHeldMs = 0;
        core::LocalWall own(desk.settings, desk.secrets, quickKvms(), ownTimes);
        desk.session.useOwnWall(own);
        QElapsedTimer watch;
        watch.start();
        desk.session.begin();
        QTRY_COMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});
        const auto tile = [&desk] { return desk.wall.machines()->index(0, 0).data(WallModel::StateRole).toString(); };
        QCOMPARE(tile(), u"connecting"_s);
        // What the KVM came to, the wall says by itself (LocalWall::cameToSomething), and the
        // machines are looked at then: the tile is online long before the next look is due.
        QTRY_COMPARE_WITH_TIMEOUT(tile(), u"online"_s, 2500);
        qInfo().noquote() << u"looked at every four seconds, the tile was online %1 ms after the start"_s.arg(
            watch.elapsed());
        QCOMPARE(kvm.signIns, 1);
    }

    void noServerIsNamedFromThisPcsOwnWall()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        desk.session.useOwnWall(own);
        desk.session.begin();
        QVERIFY(desk.view.ownWall());
        // (No screen asks for it there; whatever does is not heard.)
        desk.view.connectTo(server.address());
        QTest::qWait(400);
        QVERIFY(server.asked.isEmpty());
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(desk.view.ownWall());
        QVERIFY(!desk.view.busy());
        // From where a server is asked for, it is.
        desk.view.anotherServer();
        desk.view.connectTo(server.address());
        QTRY_VERIFY(!server.asked.isEmpty());
    }

    void whatAServerAnswersAfterItsWallWasLeftForThisPcsOwnReachesNobody()
    {
        Pretended qkvm;
        StandIn server([&qkvm](const Asked& asked) {
            Answered answer = qkvm.answer(asked);
            // The server is slow to say what it is.
            answer.afterMs = 400;
            return answer;
        });
        Desk desk;
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        const MadeUpKvm kvm;
        const QString id = own.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        desk.session.useOwnWall(own);
        desk.session.begin();
        QTRY_COMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});

        // "Connect to a QKVM server", an address, and while that server is being asked: back.
        desk.view.anotherServer();
        desk.view.connectTo(server.address());
        QVERIFY(desk.view.busy());
        QTRY_VERIFY(!server.asked.isEmpty());
        desk.view.goToOwnWall();
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(desk.view.ownWall());
        QVERIFY(!desk.view.busy());

        // The server's answer comes: nobody is asked to sign in, and the wall is this PC's.
        QTest::qWait(900);
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(desk.view.ownWall());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.address(), QString());
        QCOMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});
        QVERIFY(desk.settings.remembered().showsOwnWall);
        // And where a server is asked for the next time, it is asked for: that answer left
        // nobody half way into the server it came from.
        desk.view.anotherServer();
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.problem(), QString());
    }

    void somethingIsDoneToAKvmOfThisPcsOwnWallAndItsOrderIsMade()
    {
        Desk desk;
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        MadeUpKvm one;
        one.atx = true;
        const MadeUpKvm two;
        const QString a = own.add(at(one, u"A"_s), u"secret"_s).value();
        const QString b = own.add(at(two, u"B"_s), u"secret"_s).value();
        desk.session.useOwnWall(own);
        desk.session.begin();
        QTRY_COMPARE(keysOf(desk.wall), (QStringList{desk.ownKey(a), desk.ownKey(b)}));
        QTRY_COMPARE(desk.wall.machines()->index(0, 0).data(WallModel::StateRole).toString(), u"online"_s);

        // The wall's own order, made by hand, is this PC's.
        desk.wall.arrange();
        QVERIFY(desk.wall.arranging());
        desk.wall.placeAt(desk.ownKey(b), 0);
        desk.wall.saveOrder();
        QTRY_COMPARE(keysOf(desk.wall), (QStringList{desk.ownKey(b), desk.ownKey(a)}));
        QCOMPARE(own.kvms().constFirst().id, b);

        // And the wall as a file is a server's to write: nothing is asked of anybody for it here.
        ui::FilesView files;
        files.setDoor(doorTo(desk.folder.filePath(u"wall.qkvm"_s), {}));
        desk.session.serve(files);
        files.exportWall({u"devices"_s}, u"a passphrase of ten"_s, u"secret"_s);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(desk.folder.filePath(u"wall.qkvm"_s)));
    }

    void aKvmOfThisPcsOwnWallThatIsShownLargeIsHadLiveUntilTheWallIsLeft()
    {
        Desk desk;
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        MadeUpKvm kvm;
        // A KVM that serves its stream of pictures: the first of them, and the line is kept.
        const QByteArray picture = core::testing::jpeg(1920, 1080);
        Answered stream(200,
                        "--boundarydonotcross\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                            QByteArray::number(picture.size()) + "\r\n\r\n" + picture + "\r\n--boundarydonotcross\r\n",
                        "multipart/x-mixed-replace;boundary=boundarydonotcross");
        stream.streams = true;
        kvm.instead.insert("/streamer/stream", stream);
        const QString id = own.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        desk.session.useOwnWall(own);
        desk.session.begin();
        ui::WallView& wall = desk.wall;
        QTRY_COMPARE(keysOf(wall), QStringList{desk.ownKey(id)});
        QTRY_COMPARE(onTheCardOf(wall, desk.ownKey(id), WallModel::StateRole), u"online"_s);
        // On the wall nothing is had live.
        QTest::qWait(300);
        QCOMPARE(kvm.asked("/streamer/stream"), 0);
        QCOMPARE(wall.liveStream(), 0U);

        // Shown large: the KVM itself is asked for its stream, with nobody signed in to any
        // server. Its pictures are what is drawn, and no snapshot of it is asked for meanwhile.
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.ownKey(id));
        QTRY_COMPARE(kvm.asked("/streamer/stream"), 1);
        QTRY_VERIFY_WITH_TIMEOUT(wall.liveStream() != 0, 2000);
        QVERIFY(wall.isLive());
        QTest::qWait(400);
        const int before = kvm.asked("/api/streamer/snapshot");
        QTest::qWait(1500);
        QCOMPARE(kvm.asked("/api/streamer/snapshot"), before);
        QVERIFY(kvm.stand->isStreaming());

        // Back to the wall: not live any more, and its snapshots again.
        wall.closeEnlarged();
        QCOMPARE(wall.liveStream(), 0U);
        QTRY_VERIFY(!kvm.stand->isStreaming());
        QTRY_VERIFY(kvm.asked("/api/streamer/snapshot") > before);
        QCOMPARE(kvm.asked("/streamer/stream"), 1);

        // Shown large again, and the wall is left for a server's: the KVM's stream is given up
        // there and then, and it is not come back to.
        wall.enlarge(desk.ownKey(id));
        QTRY_COMPARE(kvm.asked("/streamer/stream"), 2);
        QTRY_VERIFY(kvm.stand->isStreaming());
        QTRY_VERIFY(wall.liveStream() != 0);
        desk.view.anotherServer();
        QVERIFY(!desk.view.ownWall());
        QCOMPARE(wall.liveStream(), 0U);
        QVERIFY(!wall.isLive());
        QTRY_VERIFY(!kvm.stand->isStreaming());
        QTest::qWait(300);
        const qsizetype heard = kvm.stand->asked.size();
        QTest::qWait(900);
        QCOMPARE(kvm.stand->asked.size(), heard);

        // At the wall again, and shown large there: live again.
        desk.view.goToOwnWall();
        QTRY_COMPARE(keysOf(wall), QStringList{desk.ownKey(id)});
        wall.enlarge(desk.ownKey(id));
        QTRY_COMPARE(kvm.asked("/streamer/stream"), 3);
        QTRY_VERIFY(wall.liveStream() != 0);
    }

    // --- what a third reviewer found in the first question to a server ---

    void aPasswordGoesOnlyToWhatWasJustAskedWhatItIsAndIsAQkvm()
    {
        Pretended qkvm;
        // What stands at the address later: a NAS, a gateway, which wants a sign-in of its own
        // for whatever it is asked.
        bool replaced = false;
        StandIn server([&](const Asked& asked) {
            return replaced ? Answered{401, "<html>Sign in to go on</html>", "text/html"} : qkvm.answer(asked);
        });
        Desk desk;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), true);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QVERIFY(desk.secrets.all.contains(Desk::nameFor(server)));
        QCOMPARE(server.count("/api/v1/login"), 1);

        // The wall is up. The QKVM ends the session, in its own words: it is forgotten, and the
        // line for a password comes up. Then the address passes to the other thing.
        // (This test had the other thing's 401 end the session and forget it: any 401 was taken
        // for the server's word, and whatever took a server's place signed its wall out for
        // good. What such a thing does to a wall that is up has a test of its own, below.)
        qkvm.signedIn = false;
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!desk.secrets.all.contains(Desk::nameFor(server)));
        QTest::qWait(300); // (what was still on its way to the wall has come)
        replaced = true;
        const qsizetype askedBefore = server.asked.size();
        // Somebody types the QKVM's password there.
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QVERIFY(desk.view.busy());
        QTRY_VERIFY(!desk.view.busy());
        QTest::qWait(300);
        // It went nowhere. What answers there now was asked what it is, with nothing that says
        // who asks, and is no QKVM: nothing was asked of it after that.
        QCOMPARE(server.count("/api/v1/login"), 1);
        QCOMPARE(server.asked.size(), askedBefore + 1);
        QCOMPARE(server.asked.constLast().path, QByteArray("/healthz"));
        QVERIFY(!server.asked.constLast().headers.contains("cookie"));
        QVERIFY(!server.asked.constLast().body.contains("the right one"));
        // Said as where a server is first named: the one sentence, where the address is typed.
        QCOMPARE(desk.view.stage(), u"address"_s);
        QCOMPARE(desk.view.problem(), u"The server answered in a way this program does not understand."_s);
        QCOMPARE(desk.wall.machines()->rowCount(), 0);

        // The QKVM is back at its address. Named again, it is asked what it is, and the password
        // that is typed then is the QKVM's to have.
        replaced = false;
        qkvm.signedIn = false;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QCOMPARE(desk.view.problem(), QString());
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(server.count("/api/v1/login"), 2);
    }

    void whatAnswers401ToEverythingInTheServersPlaceSignsNobodyOut()
    {
        // Somebody stayed signed in, for thirty days, and the wall is up. For a while something
        // stands at the server's address that wants a sign-in of its own for whatever it is
        // asked: a gateway, a NAS. Its 401 is not the QKVM's word that the session is gone.
        Pretended qkvm;
        qkvm.signedIn = true;
        bool replaced = false;
        StandIn server([&](const Asked& asked) {
            return replaced ? Answered{401, "<html>Sign in to go on</html>", "text/html"} : qkvm.answer(asked);
        });
        Desk desk;
        desk.hasKept(server);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        replaced = true;
        QTRY_VERIFY(!desk.view.inContact());
        QTest::qWait(300); // (what was still on its way to the wall has come)
        const qsizetype from = server.asked.size();
        QTest::qWait(600);
        // The wall stays, and says that contact is lost. Nobody is asked for a password, and the
        // session is kept.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!desk.view.inContact());
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
        QCOMPARE(desk.secrets.all.value(Desk::nameFor(server)), QByteArray("abc123"));
        // What answers there is asked what it is, with nothing that says who asks; and nothing else.
        QVERIFY(server.asked.size() > from);
        for (qsizetype at = from; at < server.asked.size(); ++at) {
            QCOMPARE(server.asked[at].path, QByteArray("/healthz"));
            QVERIFY(!server.asked[at].headers.contains("cookie"));
        }
        // The QKVM is back, and knows the session as before: so is the wall, and nobody has
        // signed in again.
        replaced = false;
        QTRY_VERIFY(desk.view.inContact());
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(server.count("/api/v1/login"), 0);
        QCOMPARE(desk.secrets.all.value(Desk::nameFor(server)), QByteArray("abc123"));
    }

    void whatASlowServerAnswersToAKeptSessionsQuestionReachesNobodyAtThisPcsOwnWall()
    {
        // Somebody stayed signed in at the server, which still knows that session, and is slow.
        Pretended qkvm;
        qkvm.signedIn = true;
        StandIn server([&qkvm](const Asked& asked) {
            Answered answer = qkvm.answer(asked);
            answer.afterMs = 400;
            return answer;
        });
        Desk desk;
        desk.hasKept(server);
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        const MadeUpKvm kvm;
        const QString id = own.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        desk.session.useOwnWall(own);
        desk.session.begin();
        QTRY_COMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});

        // "Connect to a QKVM server", its address, and while it is being asked what it is: back.
        desk.view.anotherServer();
        desk.view.connectTo(server.address());
        QVERIFY(desk.view.busy());
        QTRY_COMPARE(server.asked.size(), 1);
        desk.view.goToOwnWall();
        QVERIFY(desk.view.ownWall());

        // The server says what it is, late. That is an answer to nobody's question now: nothing
        // more is asked of the server, and the kept session goes nowhere.
        QTest::qWait(1500);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].path, QByteArray("/healthz"));
        QVERIFY(!server.asked[0].headers.contains("cookie"));
        // The wall is this PC's own, as it was left: nobody is on it by the server's word, it
        // is the wall that is remembered as shown, and no server as used.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(desk.view.ownWall());
        QVERIFY(!desk.view.busy());
        QCOMPARE(keysOf(desk.wall), QStringList{desk.ownKey(id)});
        QVERIFY(desk.actions.allowed());
        QVERIFY(desk.settings.remembered().showsOwnWall);
        QCOMPARE(desk.settings.remembered().server, QString());
        QVERIFY(desk.secrets.all.contains(Desk::nameFor(server)));
    }

    void aLinkWhoseSessionIsGoneAgainAtOnceIsNotHandedInAtThePaceOfTheAnswers()
    {
        // A QKVM that takes the link and does not know the session it gave for it: the next
        // question is answered with "sign in first".
        Pretended qkvm;
        bool forgetful = true;
        StandIn server([&](const Asked& asked) {
            const Answered answer = qkvm.answer(asked);
            if (forgetful && asked.path == "/api/v1/display") {
                qkvm.signedIn = false;
            }
            return answer;
        });
        // (The machines are looked at, and a server that is away is asked again, every 400 ms.)
        Desk desk(0, 400);
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_VERIFY(server.count("/api/v1/display") >= 1);
        QTest::qWait(2500);
        // Handed in once when the server was named, and after that no sooner again than the
        // server is asked while it is away: seven times at the most in this time, and not the
        // hundreds that the answers would allow. And it does go on.
        const int handedIn = server.count("/api/v1/display");
        QVERIFY2(handedIn <= 8, QByteArray::number(handedIn).constData());
        QVERIFY2(handedIn >= 3, QByteArray::number(handedIn).constData());
        // Meanwhile the wall stays and says that contact is lost, and the link is kept.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
        // The QKVM knows its sessions again: the screen is back by itself.
        forgetful = false;
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/status") >= 3, 10'000);
        QTRY_VERIFY(desk.view.inContact());
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
    }

    void aLinkWhoseSessionHeldForOneLookAndIsGoneIsNotHandedInAtThePaceOfTheAnswers()
    {
        // A QKVM that takes the link, answers the first look at the machines under the session
        // it gave for it, and knows that session no more after that. The taking has held, so
        // it is not one that counts as none (below); and come back to at once, the link would be
        // handed in again as fast as the QKVM answers, for ever.
        Pretended qkvm;
        StandIn server([&](const Asked& asked) {
            const Answered answer = qkvm.answer(asked);
            if (asked.path == "/api/v1/status") {
                qkvm.signedIn = false;
            }
            return answer;
        });
        // (The machines are looked at, and a server that is away is asked again, every 400 ms.)
        Desk desk(0, 400);
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_VERIFY(server.count("/api/v1/display") >= 1);
        QTest::qWait(2500);
        // No sooner again than a server that is away is asked: seven times at the most in this
        // time. And it does go on.
        const int handedIn = server.count("/api/v1/display");
        QVERIFY2(handedIn <= 8, QByteArray::number(handedIn).constData());
        QVERIFY2(handedIn >= 3, QByteArray::number(handedIn).constData());
        // The wall stays, and the link is kept.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
    }

    void aLinkThatIsTakenAndWhoseSessionNeverHoldsIsHandedInLessAndLessOften()
    {
        // A QKVM that takes the link each time, and never knows the session it gave for it:
        // the first question that carries that session is answered with "sign in first". Such
        // a taking counts as none for how long the link waits: it was posted every few seconds
        // for ever, since each taking began the waiting anew.
        Pretended qkvm;
        StandIn server([&](const Asked& asked) {
            const Answered answer = qkvm.answer(asked);
            if (asked.path == "/api/v1/display") {
                qkvm.signedIn = false;
            }
            return answer;
        });
        // When each handing-in of the link reached the server.
        QList<qint64> handedIn;
        QElapsedTimer clock;
        clock.start();
        QTimer looking;
        connect(&looking, &QTimer::timeout, &looking, [&] {
            while (handedIn.size() < server.count("/api/v1/display")) {
                handedIn.append(clock.elapsed());
            }
        });
        looking.start(3);
        // (The machines are looked at, and a server that is away is asked again, every 150 ms.)
        Desk desk(0, 150);
        // What the wall says of its contact from when it first said that it is lost: a taking
        // that has not held is no contact. It said "in contact" for the length of one question
        // each time the link was taken, and began "away since" anew with each.
        int saidBackMeanwhile = 0;
        QString firstAwaySince;
        connect(&desk.view, &ui::SessionView::changed, &desk.view, [&] {
            if (!desk.view.inContact() && firstAwaySince.isEmpty()) {
                firstAwaySince = desk.view.awaySince();
                desk.now = desk.now.addDays(1);
            } else if (desk.view.inContact() && !firstAwaySince.isEmpty()) {
                ++saidBackMeanwhile;
            }
        });
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        // Handed in when the server was named, and then after 150 ms, 300, 600, 1200: each wait
        // twice the one before.
        QTRY_VERIFY_WITH_TIMEOUT(handedIn.size() >= 5, 20'000);
        const QList<int> waits{150, 300, 600, 1200};
        for (qsizetype index = 0; index < waits.size(); ++index) {
            const qint64 between = handedIn[index + 1] - handedIn[index];
            QVERIFY2(between >= waits[index] - 30,
                     qPrintable(u"handed in %1 ms after the time before, where %2 were to be waited"_s.arg(between).arg(
                         waits[index])));
        }
        // The wall stays meanwhile and says that contact is lost, since when it first was, and the
        // link is kept. (The answer to the last handing-in is waited for: it is what said "in
        // contact" for a moment.)
        QTest::qWait(200);
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!desk.view.inContact());
        QCOMPARE(saidBackMeanwhile, 0);
        QVERIFY(!firstAwaySince.isEmpty());
        QCOMPARE(desk.view.awaySince(), firstAwaySince);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
    }

    void onlyTheQkvmsOwnWordThatALinkIsNotGoodForgetsTheLink_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("a gateway that cannot reach the qKVM") << 502 << QByteArray("<html>Bad Gateway</html>");
        QTest::newRow("a qKVM that is not ready yet")
            << 503 << QByteArray(R"({"code": "unavailable", "message": "This QKVM is starting."})");
        QTest::newRow("too many questions in a short time")
            << 429 << QByteArray(R"({"code": "rate_limited", "message": "Too many attempts. Try again in a minute."})");
        QTest::newRow("a refusal that does not say why") << 403 << QByteArray();
        QTest::newRow("an answer that is not understood") << 200 << QByteArray("<html>Welcome</html>");
    }

    void onlyTheQkvmsOwnWordThatALinkIsNotGoodForgetsTheLink()
    {
        QFETCH(const int, status);
        QFETCH(const QByteArray, body);
        Pretended qkvm;
        // What answers where the link is handed in, for a while: not the QKVM's word about the link.
        bool inTheWay = false;
        StandIn server([&](const Asked& asked) {
            return inTheWay && asked.path == "/api/v1/display" ? Answered{status, body} : qkvm.answer(asked);
        });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);

        // The server is restarted, and knows no session from before.
        inTheWay = true;
        qkvm.signedIn = false;
        QTRY_VERIFY(server.count("/api/v1/display") >= 2);
        QTest::qWait(300);
        // Nobody sits at a wall screen: it is not left at a line for a password. The wall stays
        // and says that contact is lost, the link is kept, and it is handed in again later, as
        // where the server cannot be reached.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!desk.view.inContact());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
        QTRY_VERIFY(server.count("/api/v1/display") >= 5);
        QCOMPARE(desk.view.stage(), u"wall"_s);

        // What was in the way is gone: the screen is back by itself.
        inTheWay = false;
        QTRY_VERIFY(desk.view.inContact());
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.account(), u"Lobby screen"_s);
        QCOMPARE(qkvm.displaysOpened, 2);
    }

    void aLinkThatIsRefusedOtherwiseWhereTheServerIsNamedStaysKept()
    {
        Pretended qkvm;
        bool inTheWay = true;
        StandIn server([&](const Asked& asked) {
            return inTheWay && asked.path == "/api/v1/display"
                       ? Pretended::refused(503, "unavailable", "This qKVM is starting.")
                       : qkvm.answer(asked);
        });
        // (The machines are looked at, and a server that is away is asked again, every 400 ms.)
        Desk desk(0, 400);
        // A screen that was opened with its link before, at its next start.
        desk.secrets.all.insert(Desk::linkNameFor(server), qkvm.displayKey);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(!desk.view.busy());
        // Said where the server was named, as where it cannot be reached; no line for a
        // password, and the link is still kept.
        QCOMPARE(desk.view.stage(), u"address"_s);
        QCOMPARE(desk.view.problem(), u"This qKVM is starting."_s);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
        QCOMPARE(server.count("/api/v1/display"), 1);

        // (This test had the server named again here by somebody, once it was up: the screen
        // waited at the line for an address until then, which at a wall screen is for ever.) It
        // is asked again by itself, the screen says so, and it is in with the link that is kept
        // for it as soon as the QKVM takes it.
        QVERIFY(desk.view.tryingAgain());
        QTRY_VERIFY(server.count("/api/v1/display") >= 3);
        QCOMPARE(desk.view.stage(), u"address"_s);
        QCOMPARE(desk.view.problem(), u"This qKVM is starting."_s);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
        inTheWay = false;
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.account(), u"Lobby screen"_s);
        QVERIFY(!desk.view.tryingAgain());
        QCOMPARE(desk.view.problem(), QString());
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
    }

    void aScreenThatStartsWhileItsServerIsAwayComesToItsWallByItself_data()
    {
        QTest::addColumn<QByteArray>("away");
        QTest::addColumn<QString>("sentence");
        QTest::newRow("the server falls over at whatever it is asked") << "down"_ba << QString();
        QTest::newRow("a gateway answers in its place, with 502")
            << "gateway"_ba << u"The server answered in a way this program does not understand."_s;
        QTest::newRow("the qKVM says what it is, and a gateway answers for the rest")
            << "half up"_ba << u"The server refused (502) and did not say why."_s;
    }

    void aScreenThatStartsWhileItsServerIsAwayComesToItsWallByItself()
    {
        // A wall screen in a hallway, started by a power strip in the morning, a moment before
        // its QKVM is up: the address it remembers, and the link that is kept for it.
        QFETCH(const QByteArray, away);
        QFETCH(const QString, sentence);
        Pretended qkvm;
        bool up = false;
        StandIn server([&](const Asked& asked) {
            if (up || (away == "half up" && asked.path == "/healthz")) {
                return qkvm.answer(asked);
            }
            Answered instead{502, "<html><head><title>502 Bad Gateway</title></head></html>", "text/html"};
            instead.hangUp = away == "down";
            return instead;
        });
        // (The machines are looked at, and a server that is away is asked again, every 300 ms.)
        Desk desk(0, 300);
        core::Remembered remembered = desk.settings.remembered();
        remembered.server = server.address() + u'/';
        QVERIFY(desk.settings.remember(remembered).ok());
        desk.secrets.all.insert(Desk::linkNameFor(server), qkvm.displayKey);
        QStringList stages;
        connect(&desk.view, &ui::SessionView::changed, this, [&] { stages.append(desk.view.stage()); });
        desk.session.begin();
        QVERIFY(desk.view.busy());
        QTRY_VERIFY(!desk.view.busy());

        // Why it is not at its wall is said where a server is named, and that it is trying. It
        // is not busy meanwhile: another address can be typed at any time.
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(desk.view.tryingAgain());
        QVERIFY(!desk.view.problem().isEmpty());
        if (!sentence.isEmpty()) {
            QCOMPARE(desk.view.problem(), sentence);
        }
        // Asked again and again, at the pace a server that is away is asked at and no faster:
        // five times in a second and a half, give or take, and not as often as it answers.
        // (Where the server falls over, Qt asks a question whose line fell again by itself, a
        // few times: how often it was asked says nothing of the pace there.)
        const int askedBefore = server.count("/healthz");
        QTest::qWait(1500);
        const int askedMeanwhile = server.count("/healthz") - askedBefore;
        QVERIFY2(askedMeanwhile >= 3 && (askedMeanwhile <= 6 || away == "down"),
                 QByteArray::number(askedMeanwhile).constData());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(desk.view.tryingAgain());
        // The link went to nothing that did not say it is a QKVM, the session that was kept
        // neither, and the link is still kept.
        QCOMPARE(server.count("/api/v1/display"), 0);
        for (const Asked& one : std::as_const(server.asked)) {
            if (one.path == "/healthz") {
                QVERIFY(!one.headers.contains("cookie"));
            }
            QVERIFY(!one.body.contains("qkvm_"));
        }
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
        QCOMPARE(desk.wall.machines()->rowCount(), 0);

        // The QKVM is up: the screen is at its wall, by itself, and nobody was shown a line for
        // a password on the way.
        up = true;
        QTRY_COMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.view.account(), u"Lobby screen"_s);
        QVERIFY(!desk.view.tryingAgain());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(qkvm.displaysOpened, 1);
        QVERIFY(!stages.contains(u"signIn"_s));
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QVERIFY(desk.wall.displaying());
    }

    void aScreenThatWaitsForItsServerStopsWhenSomebodyTypesOrNamesAnother()
    {
        Pretended qkvm;
        // Its own server, away behind a gateway that is slow to say so.
        StandIn server([&](const Asked&) {
            Answered instead{502, "<html><head><title>502 Bad Gateway</title></head></html>", "text/html"};
            instead.afterMs = 150;
            return instead;
        });
        Pretended otherQkvm;
        StandIn other([&otherQkvm](const Asked& asked) { return otherQkvm.answer(asked); });
        Desk desk(0, 200);
        desk.secrets.all.insert(Desk::linkNameFor(server), qkvm.displayKey);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(desk.view.tryingAgain());
        QTRY_VERIFY(server.count("/healthz") >= 3);

        // Somebody types at the line for an address: it is asked no more, and the screen says
        // no longer that it is trying. Why it was not reached stays said.
        desk.view.stopTrying();
        QVERIFY(!desk.view.tryingAgain());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.stage(), u"address"_s);
        QCOMPARE(desk.view.problem(), u"The server answered in a way this program does not understand."_s);
        QTest::qWait(300); // (what was still on its way has come)
        const int asked = server.count("/healthz");
        QTest::qWait(700);
        QCOMPARE(server.count("/healthz"), asked);
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(!desk.view.tryingAgain());

        // Named again, it is waited for again. And while a question that was asked by itself is
        // under way, another server is named: taken at once, as at any other moment.
        desk.view.connectTo(server.address());
        QTRY_VERIFY(desk.view.tryingAgain());
        const int before = server.count("/healthz");
        QTRY_VERIFY(server.count("/healthz") > before);
        QVERIFY(!desk.view.busy());
        desk.view.connectTo(other.address());
        QVERIFY(desk.view.busy());
        QVERIFY(!desk.view.tryingAgain());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        QCOMPARE(desk.view.address(), Desk::authorityOf(other));
        QCOMPARE(desk.view.problem(), QString());
        // What the first still answers reaches nobody, it is asked no more, and the other never
        // sees the first one's link.
        const int then = server.count("/healthz");
        QTest::qWait(700);
        QCOMPARE(server.count("/healthz"), then);
        QCOMPARE(desk.view.stage(), u"signIn"_s);
        QVERIFY(!desk.view.tryingAgain());
        QCOMPARE(other.count("/api/v1/display"), 0);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
    }

    void aLinkThatWasPastedForAServerThatIsAwayIsNotTriedByItself()
    {
        // Somebody sits there and pastes it: they are told, and name the server again when it
        // is up. Only a link that is kept, which is one that opened a session before, is a
        // screen's own way back in.
        Pretended qkvm;
        StandIn server([&](const Asked&) {
            return Answered{502, "<html><head><title>502 Bad Gateway</title></head></html>", "text/html"};
        });
        Desk desk;
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_VERIFY(!desk.view.busy());
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(!desk.view.tryingAgain());
        QTest::qWait(500);
        QCOMPARE(server.asked.size(), 1);
        QVERIFY(desk.secrets.all.isEmpty());
    }

    void whatAnswersWhereTheServerWasWhileItIsAwayIsHandedNoSession()
    {
        // Somebody is signed in, and the wall is up. The server goes away, and something else
        // answers at its address: whatever it is asked, it answers.
        Pretended qkvm;
        auto server = std::make_unique<StandIn>([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const quint16 port = server->serverPort();
        Desk desk;
        desk.view.connectTo(server->address());
        QTRY_COMPARE(desk.view.stage(), u"signIn"_s);
        desk.view.signIn(QString(), u"the right one"_s, QString(), false);
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QTRY_VERIFY(server->count("/api/v1/devices/") >= 2);
        server.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.inContact(), 20000);
        {
            StandIn other([](const Asked&) { return Answered{200, "<html>It works!</html>"_ba, "text/html"_ba}; },
                          port);
            if (!other.isListening()) {
                QSKIP("Windows has given the port to somebody else in the meantime.");
            }
            // The wall goes on asking at its pace, for how the machines are and for their
            // pictures. What answers there is asked what it is each time, with nothing that says
            // who asks, and is no QKVM: not one question with the session reaches it.
            QTRY_VERIFY_WITH_TIMEOUT(other.asked.size() >= 5, 20000);
            QTest::qWait(600);
            for (const Asked& one : std::as_const(other.asked)) {
                QCOMPARE(one.path, QByteArray("/healthz"));
                QVERIFY(!one.headers.contains("cookie"));
            }
            // The wall stays, and says that contact is lost.
            QCOMPARE(desk.view.stage(), u"wall"_s);
            QVERIFY(!desk.view.inContact());
            QCOMPARE(desk.wall.machines()->rowCount(), 3);
        }
        // The QKVM is back at its address, and knows the session: asked what it is first, then
        // the wall's questions go to it again, with the session, and contact is back.
        StandIn back([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, port);
        if (!back.isListening()) {
            QSKIP("Windows has given the port to somebody else in the meantime.");
        }
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.inContact(), 20000);
        QCOMPARE(back.asked.constFirst().path, QByteArray("/healthz"));
        QVERIFY(!back.asked.constFirst().headers.contains("cookie"));
        QTRY_VERIFY(back.count("/api/v1/status") >= 2);
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(desk.wall.machines()->rowCount(), 3);
    }

    void aLinkThatAQkvmDoesNotTakeIsHandedInLessAndLessOften()
    {
        // The server was restarted and knows no session from before; and it answers the link
        // with "too many", each time.
        Pretended qkvm;
        bool inTheWay = false;
        QByteArray saysToWait;
        StandIn server([&](const Asked& asked) {
            if (inTheWay && asked.path == "/api/v1/display") {
                Answered refusal = Pretended::refused(429, "rate_limited", "Too many attempts. Try again in a minute.");
                if (!saysToWait.isEmpty()) {
                    refusal.more = {"Retry-After: " + saysToWait};
                }
                return refusal;
            }
            return qkvm.answer(asked);
        });
        // When each handing-in of the link reached the server.
        QList<qint64> handedIn;
        QElapsedTimer clock;
        clock.start();
        QTimer looking;
        connect(&looking, &QTimer::timeout, &looking, [&] {
            while (handedIn.size() < server.count("/api/v1/display")) {
                handedIn.append(clock.elapsed());
            }
        });
        looking.start(3);
        // (The machines are looked at, and a server that is away is asked again, every 150 ms.)
        Desk desk(0, 150);
        desk.view.connectTo(Desk::linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(desk.wall.machines()->rowCount(), 3);
        QTRY_COMPARE(handedIn.size(), 1);
        inTheWay = true;
        qkvm.signedIn = false;
        // Handed in again at once, then after 150 ms, 300, 600, 1200: each wait twice the one
        // before, and never at the wall's pace for ever.
        QTRY_VERIFY_WITH_TIMEOUT(handedIn.size() >= 6, 20'000);
        const QList<int> waits{150, 300, 600, 1200};
        for (qsizetype index = 0; index < waits.size(); ++index) {
            const qint64 between = handedIn[index + 2] - handedIn[index + 1];
            QVERIFY2(between >= waits[index] - 30,
                     qPrintable(u"handed in %1 ms after the time before, where %2 were to be waited"_s.arg(between).arg(
                         waits[index])));
        }
        // The wall stays meanwhile, and the link is kept.
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QVERIFY(!desk.view.inContact());
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
        // The QKVM takes it: the screen is back, and its session holds, since the machines are
        // looked at under it. The waiting begins anew with that: refused again later, the link is
        // handed in after the first wait, and not the one it had got to.
        // (This test had the server forget the session in the same breath as it took the link,
        // before one look was answered under it, and the waiting begin anew all the same. Such
        // a taking counts as none now, and has a test of its own.)
        inTheWay = false;
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.inContact(), 20'000);
        const int looks = server.count("/api/v1/status");
        QTRY_VERIFY(server.count("/api/v1/status") >= looks + 2);
        const qsizetype taken = handedIn.size();
        inTheWay = true;
        qkvm.signedIn = false;
        QTRY_VERIFY_WITH_TIMEOUT(handedIn.size() >= taken + 3, 20'000);
        const qint64 anew = handedIn[taken + 2] - handedIn[taken + 1];
        QVERIFY2(anew >= 300 - 30 && anew < 1200, qPrintable(QString::number(anew)));
        // Where the server says how long to wait, that is the least that is waited.
        saysToWait = "2"_ba;
        const qsizetype told = handedIn.size();
        QTRY_VERIFY_WITH_TIMEOUT(handedIn.size() >= told + 2, 20'000);
        const qint64 asSaid = handedIn[told + 1] - handedIn[told];
        QVERIFY2(asSaid >= 2000 - 30, qPrintable(QString::number(asSaid)));
    }

    void aScreenThatWaitsWhereItsServerIsNamedHandsItsLinkInLessAndLessOften()
    {
        // A screen that starts with the link that is kept for its server, which answers the
        // link with "starting", each time.
        Pretended qkvm;
        StandIn server([&](const Asked& asked) {
            return asked.path == "/api/v1/display" ? Pretended::refused(503, "unavailable", "This qKVM is starting.")
                                                   : qkvm.answer(asked);
        });
        QList<qint64> handedIn;
        QElapsedTimer clock;
        clock.start();
        QTimer looking;
        connect(&looking, &QTimer::timeout, &looking, [&] {
            while (handedIn.size() < server.count("/api/v1/display")) {
                handedIn.append(clock.elapsed());
            }
        });
        looking.start(3);
        Desk desk(0, 150);
        desk.secrets.all.insert(Desk::linkNameFor(server), qkvm.displayKey);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(desk.view.tryingAgain());
        // Asked again by itself, from the first question each time; and the link, which is
        // handed in each time, after 150 ms, 300, 600, 1200.
        QTRY_VERIFY_WITH_TIMEOUT(handedIn.size() >= 5, 20'000);
        const QList<int> waits{150, 300, 600, 1200};
        for (qsizetype index = 0; index < waits.size(); ++index) {
            const qint64 between = handedIn[index + 1] - handedIn[index];
            QVERIFY2(between >= waits[index] - 30,
                     qPrintable(u"handed in %1 ms after the time before, where %2 were to be waited"_s.arg(between).arg(
                         waits[index])));
        }
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(desk.view.tryingAgain());
        QCOMPARE(desk.view.problem(), u"This qKVM is starting."_s);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
    }

    void aScreenThatWaitsForItsServerStopsWhereAQkvmOfAnotherVersionAnswers()
    {
        // Its server, away behind a gateway; then a QKVM answers there that speaks another
        // version of its API. One of the two has to be updated, which somebody has to do: that
        // is said where a server is named, it is asked no more, and the link was not handed to
        // it and stays kept.
        Pretended qkvm;
        bool up = false;
        StandIn server([&](const Asked& asked) {
            return up ? qkvm.answer(asked)
                      : Answered{502, "<html><head><title>502 Bad Gateway</title></head></html>", "text/html"};
        });
        Desk desk(0, 150);
        desk.secrets.all.insert(Desk::linkNameFor(server), qkvm.displayKey);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(desk.view.tryingAgain());
        qkvm.apis = R"(["v9"])";
        up = true;
        QTRY_VERIFY(!desk.view.tryingAgain());
        QCOMPARE(desk.view.stage(), u"address"_s);
        QVERIFY(!desk.view.busy());
        QVERIFY2(desk.view.problem().contains(u"v9"_s), qPrintable(desk.view.problem()));
        const qsizetype asked = server.asked.size();
        QTest::qWait(700);
        QCOMPARE(server.asked.size(), asked);
        QCOMPARE(server.count("/api/v1/display"), 0);
        QCOMPARE(server.count("/api/v1/session"), 0);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
    }

    void aScreenThatWaitsForItsServerStopsWhereItsCertificateHasToBeLookedAt()
    {
        // Its server is known by a certificate. While the screen waits for it, what answers
        // there shows another one: everything stops until somebody has looked, the link is
        // handed to nobody, and nothing is asked by itself any more.
        Pretended qkvm;
        auto away = std::make_unique<StandIn>(
            [](const Asked&) {
                return Answered{502, "<html><head><title>502 Bad Gateway</title></head></html>", "text/html"};
            },
            tls("first"));
        const quint16 port = away->serverPort();
        const QString address = away->address();
        Desk desk(0, 150);
        desk.knows(*away, sha256Of("first"));
        desk.secrets.all.insert(Desk::linkNameFor(*away), qkvm.displayKey);
        desk.view.connectTo(address);
        QTRY_VERIFY(desk.view.tryingAgain());
        away.reset();
        StandIn other([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("second"), port);
        if (!other.isListening()) {
            QSKIP("Windows has given the port to somebody else in the meantime.");
        }
        QTRY_COMPARE_WITH_TIMEOUT(desk.view.stage(), u"trust"_s, 20000);
        QVERIFY(!desk.view.tryingAgain());
        QTest::qWait(700);
        QCOMPARE(desk.view.stage(), u"trust"_s);
        QVERIFY(other.asked.isEmpty());
        QCOMPARE(desk.secrets.all.value(u"display link at "_s + address + u'/'), qkvm.displayKey);
    }

    void aScreenThatWaitsForItsServerStopsWhereThisPcsOwnWallIsGoneTo()
    {
        // Its server, away behind a gateway that is slow to say so; and somebody goes to this
        // PC's own wall. The server is asked no more, what it still answers reaches nobody, and
        // its link is handed to nothing.
        Pretended qkvm;
        StandIn server([&](const Asked&) {
            Answered instead{502, "<html><head><title>502 Bad Gateway</title></head></html>", "text/html"};
            instead.afterMs = 150;
            return instead;
        });
        Desk desk(0, 200);
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        desk.session.useOwnWall(own);
        desk.secrets.all.insert(Desk::linkNameFor(server), qkvm.displayKey);
        desk.view.connectTo(server.address());
        QTRY_VERIFY(desk.view.tryingAgain());
        // Between two askings: the last one was answered, and the next is being waited for.
        const int before = server.count("/healthz");
        QTRY_VERIFY(server.count("/healthz") > before);
        QTest::qWait(200);
        const int asked = server.count("/healthz");
        desk.view.goToOwnWall();
        QVERIFY(desk.view.ownWall());
        QVERIFY(!desk.view.tryingAgain());
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QTest::qWait(1000);
        QCOMPARE(server.count("/healthz"), asked);
        QVERIFY(desk.view.ownWall());
        QCOMPARE(desk.view.stage(), u"wall"_s);
        QCOMPARE(server.count("/api/v1/display"), 0);
        QCOMPARE(desk.secrets.all.value(Desk::linkNameFor(server)), qkvm.displayKey);
    }

    // --- keyboard and mouse of the machine a window shows large ---

    void keyboardAndMouseOfAServersMachineGoOverTheLineItsWindowHasItLiveBy()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const auto control = [](bool on, const QString& code = {}, const QString& message = {}) {
            return QJsonObject{{u"t"_s, u"control"_s}, {u"on"_s, on}, {u"code"_s, code}, {u"message"_s, message}};
        };
        const input::Pressed keyA{0x1E, false, 0x41};
        Desk desk;
        desk.hasKept(server);
        // The window's own: what its screen is told, keys pressed by hand, and who sends them.
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl machine(view, keys);
        machine.setInFront(true);
        desk.session.drive(desk.wall, machine);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        // On the wall there is nothing to take.
        QVERIFY(desk.actions.allowed());
        QVERIFY(!view.isPossible());

        // A machine is shown large and is had live: they can be taken once its line is open.
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_VERIFY(server.live.isConnected());
        QTRY_VERIFY(view.isPossible());
        server.live.say({{u"t"_s, u"state"_s}, {u"pointer"_s, u"absolute"_s}});
        QTRY_VERIFY(!view.mouseSelectable());
        view.take();
        QVERIFY(view.isAsking());
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 1);
        QVERIFY(server.live.saidOf(u"control"_s).at(0)[u"on"_s].toBool());
        QVERIFY(!keys.listens);
        server.live.say(control(true));
        QTRY_VERIFY(view.isOn());
        QVERIFY(keys.swallows());
        // Keys, the pointer by where it is in the picture, a button and the wheel: over that line.
        keys.press(keyA);
        keys.release(keyA);
        server.live.say({{u"t"_s, u"video"_s}, {u"codec"_s, u"jpeg"_s}, {u"w"_s, 1920}, {u"h"_s, 1080}});
        QTRY_COMPARE(server.live.saidOf(u"k"_s).size(), 2);
        // (The picture's size has come: a pointer over a picture of no known size is over none,
        // and its button and wheel go to no machine. The keys may have been said before it came:
        // this test pointed in that moment once, in a whole check.)
        QTRY_COMPARE(view.picture(), QSize(1920, 1080));
        view.pointerAt(480, 270, 960, 540);
        view.button(Qt::LeftButton, true);
        view.wheel(0, 120);
        QTRY_COMPARE(server.live.saidOf(u"mw"_s).size(), 1);
        QCOMPARE(server.live.saidOf(u"k"_s).at(0)[u"u"_s].toInt(), 0x04);
        QVERIFY(server.live.saidOf(u"k"_s).at(0)[u"d"_s].toBool());
        QVERIFY(!server.live.saidOf(u"k"_s).at(1)[u"d"_s].toBool());
        QCOMPARE(server.live.saidOf(u"m"_s).size(), 1);
        QCOMPARE(server.live.saidOf(u"m"_s).at(0)[u"x"_s].toInt(), 16384);
        QCOMPARE(server.live.saidOf(u"m"_s).at(0)[u"y"_s].toInt(), 16384);
        QCOMPARE(server.live.saidOf(u"mb"_s).at(0)[u"b"_s].toInt(), 1);
        QCOMPARE(server.live.saidOf(u"mw"_s).at(0)[u"dy"_s].toInt(), 1);

        // Another machine is shown large: what is down on this one is let go on its own line,
        // and they are given back there, before that line is ended.
        QVERIFY(server.live.saidOf(u"up"_s).isEmpty());
        wall.enlarge(desk.key(u"b"_s));
        QVERIFY(!view.isOn());
        QVERIFY(!view.isPossible());
        QVERIFY(!keys.listens);
        QTRY_COMPARE(server.live.lines, 2);
        // (And the first word on the next line has come: a line is taken before it is spoken on.)
        QTRY_COMPARE(server.live.saidOf(u"hello"_s).size(), 2);
        const QList<QJsonObject> said = server.live.said;
        qsizetype letGoAt = -1;
        qsizetype givenBackAt = -1;
        qsizetype nextLineAt = -1;
        for (qsizetype at = 0; at < said.size(); ++at) {
            const QString kind = said.at(at)[u"t"_s].toString();
            if (kind == u"up"_s && letGoAt < 0) {
                letGoAt = at;
            } else if (kind == u"control"_s && !said.at(at)[u"on"_s].toBool()) {
                givenBackAt = at;
            } else if (kind == u"hello"_s && at > 0) {
                nextLineAt = at;
            }
        }
        QVERIFY2(letGoAt > 0 && letGoAt < givenBackAt && givenBackAt < nextLineAt,
                 qPrintable(u"%1 %2 %3"_s.arg(letGoAt).arg(givenBackAt).arg(nextLineAt)));

        // Of that one they are taken, and the server takes them away again: why is said, and
        // nothing more is listened for.
        QTRY_VERIFY(view.isPossible());
        view.take();
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 3);
        server.live.say(control(true));
        QTRY_VERIFY(view.isOn());
        server.live.say(control(false, u"taken"_s, u"An admin took keyboard and mouse of this machine."_s));
        QTRY_VERIFY(!view.isOn());
        QCOMPARE(view.problem(), u"An admin took keyboard and mouse of this machine."_s);
        QVERIFY(!keys.listens);
        QVERIFY(view.isPossible());
        // (The server says its word for the giving back that followed: until it has, or the
        // time for it is over, they are not asked for again.)
        QVERIFY(view.isBusy());
        server.live.say(control(false));
        QTRY_VERIFY(!view.isBusy());

        // A wall that is only shown: nobody sits at it, and nothing can be taken from it. Taken
        // before, they are given back when it begins.
        view.take();
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 5);
        server.live.say(control(true));
        QTRY_VERIFY(view.isOn());
        wall.setDisplaying(true);
        QVERIFY(!view.isOn());
        QVERIFY(!view.isPossible());
        QVERIFY(!keys.listens);
        view.take();
        QVERIFY(!view.isAsking());
        wall.setDisplaying(false);
        QTRY_VERIFY(view.isPossible());

        // Whoever may only look is offered nothing: an admin made this account a viewer.
        qkvm.user = R"({"id": "u1", "name": "anna", "role": "viewer", "token": false})";
        QTRY_VERIFY_WITH_TIMEOUT(!desk.actions.allowed(), 20'000);
        QVERIFY(!view.isPossible());
        view.take();
        QVERIFY(!view.isAsking());

        // The window goes: its machine's control is told that it has no machine any more.
        desk.session.remove(wall);
        QVERIFY(!view.isPossible());
    }

    void keyboardAndMouseOfAKvmOfThisPcsOwnWallGoToTheKvmItself()
    {
        Desk desk;
        core::LocalWall own(desk.settings, desk.secrets, quickKvms());
        const MadeUpKvm kvm;
        const QString id = own.add(at(kvm, u"Build server"_s), u"secret"_s).value();
        desk.session.useOwnWall(own);
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl machine(view, keys);
        machine.setInFront(true);
        desk.session.drive(desk.wall, machine);
        desk.session.begin();
        ui::WallView& wall = desk.wall;
        QTRY_COMPARE(keysOf(wall), QStringList{desk.ownKey(id)});
        QTRY_COMPARE(onTheCardOf(wall, desk.ownKey(id), WallModel::StateRole), u"online"_s);
        QVERIFY(!view.isPossible());
        // What the KVM heard on its lines of keys and the mouse, in the order it came.
        const auto heard = [&kvm] {
            QStringList all;
            for (const QJsonObject& said : std::as_const(kvm.stand->live.said)) {
                const QJsonObject event = said[u"event"_s].toObject();
                if (said[u"event_type"_s].toString() == u"key"_s) {
                    all.append(event[u"key"_s].toString() + (event[u"state"_s].toBool() ? u" down"_s : u" up"_s));
                } else if (said[u"event_type"_s].toString() == u"mouse_button"_s) {
                    all.append(event[u"button"_s].toString() + (event[u"state"_s].toBool() ? u" down"_s : u" up"_s));
                }
            }
            return all;
        };

        // Shown large, it is had live from the KVM itself, and whoever sits at this PC may take
        // its keyboard and mouse: the KVM lets a line of its own in for them.
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.ownKey(id));
        QTRY_VERIFY(view.isPossible());
        // Ctrl is down as they are taken: it is not the machine's, and under it C is no "c".
        const input::Pressed leftCtrl{0x1D, false, 0xA2};
        const input::Pressed keyC{0x2E, false, 0x43};
        const input::Pressed keyA{0x1E, false, 0x41};
        keys.press(leftCtrl);
        view.take();
        QVERIFY(view.isAsking());
        QTRY_VERIFY_WITH_TIMEOUT(view.isOn(), 10'000);
        QVERIFY(keys.swallows());
        keys.press(keyC);
        keys.release(keyC);
        keys.release(leftCtrl);
        keys.press(keyA);
        QTRY_COMPARE(heard(), QStringList{u"KeyA down"_s});
        // Given back with the key still held: it is let go on the machine. What the KVM's live
        // says to the giving back is no news, and nothing to complain of.
        view.giveBack();
        QVERIFY(!view.isOn());
        // (Nothing is swallowed any more; the keys are listened for until the finger is off A,
        // which Windows never saw go down. This looked for the listening to end at once.)
        QVERIFY(!keys.swallows());
        QVERIFY(keys.listens);
        QTRY_COMPARE(heard(), (QStringList{u"KeyA down"_s, u"KeyA up"_s}));
        QTest::qWait(300);
        QCOMPARE(view.problem(), QString());
        QVERIFY(view.isPossible());
        // Taken again, on another line of the KVM's: the key that is still held is not pressed
        // again by its repeat, and is a key once it was let go.
        view.take();
        QTRY_VERIFY_WITH_TIMEOUT(view.isOn(), 10'000);
        keys.repeat(keyA);
        keys.release(keyA);
        keys.press(keyA);
        QTRY_COMPARE(heard(), (QStringList{u"KeyA down"_s, u"KeyA up"_s, u"KeyA down"_s}));
        // Its picture's size is known from the KVM's own pictures: a click in the middle of it.
        QTRY_VERIFY_WITH_TIMEOUT(wall.liveStream() != 0, 5000);
        // (Waited for: a click before the size is known is a click beside the picture, and goes
        // nowhere. Nothing waited for it, and the click's arrival was not counted.)
        QTRY_VERIFY_WITH_TIMEOUT(!view.picture().isEmpty(), 5000);
        view.pointerAt(480, 270, 960, 540);
        view.button(Qt::LeftButton, true);
        QTRY_VERIFY(heard().contains(u"left down"_s));

        // Back to the wall: everything that is down on the machine is let go there, the button
        // and then the key, before the KVM's line for them goes.
        wall.closeEnlarged();
        QVERIFY(!view.isOn());
        QVERIFY(!keys.swallows());
        QTRY_VERIFY(heard().size() >= 4 && heard().constLast() == u"KeyA up"_s);
        const QStringList all = heard();
        QCOMPARE(all.count(u"KeyA down"_s), 2);
        QCOMPARE(all.count(u"KeyA up"_s), 2);
        QVERIFY(!all.contains(u"KeyC down"_s));
        QVERIFY(!all.contains(u"ControlLeft down"_s));
        // (The click arrived, once, and was let go once: this held at none and none.)
        QCOMPARE(all.count(u"left down"_s), 1);
        QCOMPARE(all.count(u"left up"_s), 1);
        QVERIFY(!view.isPossible());
        // The finger comes off A: the keys are listened for no longer.
        keys.release(keyA);
        QVERIFY(!keys.listens);
    }

    void whyTheLineEndedIsSaidWhereKeyboardAndMouseWereOnlyAskedFor()
    {
        Pretended qkvm;
        qkvm.signedIn = true;
        qkvm.giveLive();
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Desk desk;
        desk.hasKept(server);
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl machine(view, keys);
        machine.setInFront(true);
        desk.session.drive(desk.wall, machine);
        ui::WallView& wall = desk.wall;
        desk.view.connectTo(server.address());
        QTRY_COMPARE(wall.machines()->rowCount(), 3);
        wall.setEnlargedPixels(1000);
        wall.enlarge(desk.key(u"a"_s));
        QTRY_VERIFY(server.live.isConnected());
        QTRY_VERIFY(view.isPossible());
        // Asked for, and the machine's line ends before the server has said yes or no. As the
        // program is wired the window's live picture hears of that end first, and whoever asked
        // is told why all the same: the bar does not simply stop saying that it asks.
        view.take();
        QVERIFY(view.isAsking());
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 1);
        server.live.fallOver();
        QTRY_VERIFY(!view.isAsking());
        QVERIFY(!view.isOn());
        QVERIFY2(!view.problem().isEmpty(), "Nothing was said of why the line ended.");
        QVERIFY(!keys.listens);
        // The line comes back, and they are asked for anew: the sentence goes with that.
        QTRY_VERIFY_WITH_TIMEOUT(view.isPossible(), 10'000);
        view.take();
        QCOMPARE(view.problem(), QString());
    }
};

QTEST_GUILESS_MAIN(TestServerSession)

#include "test_server_session.moc"
