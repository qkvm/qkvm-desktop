// A whole profile as a person makes one, with the code of the build this is part of, into a folder (tests only, never
// shipped): the upgrade fixtures of every published version are made by this, built at that version's commit
// (ui/tests/fixtures/profiles/README.md says how). Nothing of it reaches Windows' Credential Manager or the person's
// own profile: the secrets are kept in memory and dumped, the files are written into the folder it is given, and the
// servers it signs in to are stand-ins on this PC's own address. The KVMs are at addresses set aside for examples
// (192.0.2.0/24, 198.51.100.0/24), and nothing is asked of them: no driver is asked anything, and no turn of the loop
// runs while the wall is there.
//
//     qkvm-test-make-profile <empty folder> <commit> [person | wall-screen]
//
// A person (the default) sits at the PC; a wall screen is a PC nobody sits at, signed in to a server's wall by a
// display link, and locked. It writes there: profile/ (settings.json and what is beside it, as that version keeps it),
// secrets.json (every secret the store holds, by its name, as base64 of its bytes), and made.json (what was done and
// typed, in plain words: what the next version must read back the same). What that version's Windows code makes of
// each secret is written beside them by qkvm-test-credential-shape. All made up.

#include <cstdio>
#include <cstdlib>
#include <functional>

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTest>
#include <QTimeZone>
#include <QUrl>
#include <QVersionNumber>

#include <qkvm/core/exchange.h>
#include <qkvm/core/incidents.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/own_actions.h>
#include <qkvm/core/pacing_file.h>
#include <qkvm/core/screen_watch.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/setup_overrides.h>
#include <qkvm/core/sign_in_pacing.h>

#include "app_lock.h"
#include "memory_secrets.h"
#include "server_source.h"
#include "stand_in.h"
#include "wall_screen.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

namespace {

// The stand-in qKVM servers' places: below the ports Windows hands out by itself (49152 and up), so that a test that
// reads the fixture can have them again.
constexpr quint16 serverPort = 47391;
constexpr quint16 screenServerPort = 47392;
// (As the program writes a server's address: ServerClient, with the path its address has.)
const QString serverAddress = u"http://127.0.0.1:47391/"_s;
const QString screenServerAddress = u"http://127.0.0.1:47392/"_s;
// What the servers gave as their sessions (43 signs, as the web's token_urlsafe(32)), and the display link: made up.
const QByteArray serverSession = "q8Zr2vN0x4LmT7cY1pK9dW3sH6fJ5gB0aE2uR8iO1nM";
const QByteArray screenSession = "w3Xk9pL2mN7qR4tV8yB1cF6hJ0dG5sA2eU9iO3zK7nP";
const QByteArray displayKey = "qkvm_Lb4Tz8Qw1Ex6Rv3Ym9Pa2Sd7Kf0Hg5Jc1Nb8Vt4Mq";
const QString appPassword = u"made-up app password 4271"_s;
const QString serverPassword = u"made-up server password"_s;
const QString wrongPassword = u"a wrong made-up password"_s;

[[noreturn]] void stop(const QString& why)
{
    std::fprintf(stderr, "qkvm-test-make-profile: %s\n", qPrintable(why));
    std::exit(2);
}

void check(bool fine, const QString& why)
{
    if (!fine) {
        stop(why);
    }
}

bool waitFor(const std::function<bool()>& done, int ms)
{
    return QTest::qWaitFor(done, QDeadlineTimer(ms));
}

Answered aQkvmOnceSignedIn(const Asked& asked, bool signedIn)
{
    if (!signedIn) {
        return {401, R"({"code": "unauthorized", "message": "Sign in first"})"};
    }
    if (asked.path == "/api/v1/wall") {
        return {200, R"({"interval": 1, "devices": [
            {"id": "a", "name": "Build rack", "host": "198.51.100.40", "kind": "kvmd", "picture": true}]})"};
    }
    if (asked.path == "/api/v1/status") {
        return {200, R"({"a": {"state": "online"}})"};
    }
    return {200, core::testing::aPicture(), "image/jpeg"};
}

const QByteArray healthy = R"({"status": "ok", "version": "0.1.0", "api": ["v1"], "page": "3f2a9c", "locked": false})";

// A QKVM as far as signing in goes: whoever brings the session it gave is signed in.
Answered aQkvm(const Asked& asked)
{
    const bool signedIn = asked.headers.value("cookie").contains("qkvm_session=" + serverSession);
    if (asked.path == "/healthz") {
        return {200, healthy};
    }
    if (asked.path == "/api/v1/session") {
        return {200, signedIn ? QByteArray(R"({"initialized": true, "authenticated": true, "ask_name": true,
                                    "user": {"id": "u1", "name": "anna", "role": "admin", "token": false}})")
                              : QByteArray(R"({"initialized": true, "authenticated": false, "ask_name": true,
                                    "user": null})")};
    }
    if (asked.path == "/api/v1/login" && asked.method == "POST") {
        if (!asked.body.contains(serverPassword.toUtf8())) {
            return {401, R"({"code": "bad_credentials", "message": "Wrong name or password"})"};
        }
        return {200,
                R"({"ok": true, "user": {"id": "u1", "name": "anna", "role": "admin", "token": false}})",
                "application/json",
                {"Set-Cookie: qkvm_session=" + serverSession + "; HttpOnly; Path=/; SameSite=Strict"}};
    }
    return aQkvmOnceSignedIn(asked, signedIn);
}

// A QKVM whose wall a display link signs in to: the link's key opens a session of the link's account.
Answered aScreensQkvm(const Asked& asked)
{
    const bool signedIn = asked.headers.value("cookie").contains("qkvm_session=" + screenSession);
    if (asked.path == "/healthz") {
        return {200, healthy};
    }
    if (asked.path == "/api/v1/session") {
        return {200, signedIn ? QByteArray(R"({"initialized": true, "authenticated": true, "ask_name": false,
                                    "user": {"id": "token:t1", "name": "Lobby screen", "role": "admin", "token": true}})")
                              : QByteArray(R"({"initialized": true, "authenticated": false, "ask_name": false,
                                    "user": null})")};
    }
    if (asked.path == "/api/v1/display" && asked.method == "POST") {
        if (!asked.body.contains(displayKey)) {
            return {401, R"({"code": "link_not_good", "message": "This display link is not valid"})"};
        }
        return {200,
                R"({"ok": true, "user": {"id": "token:t1", "name": "Lobby screen", "role": "admin", "token": true}})",
                "application/json",
                {"Set-Cookie: qkvm_session=" + screenSession + "; HttpOnly; Path=/; SameSite=Strict"}};
    }
    return aQkvmOnceSignedIn(asked, signedIn);
}

QString scopeOf(const QUrl& server, const QString& account)
{
    // (As the program names a server's machines' scope: server_session.cpp, incidentScope.)
    return u"server-"_s + QString::fromLatin1(QCryptographicHash::hash(server.toEncoded() + '\n' + account.toUtf8(),
                                                                       QCryptographicHash::Sha256)
                                                  .toHex());
}

QByteArray madeUpDigest(const QByteArray& of)
{
    return QCryptographicHash::hash(of, QCryptographicHash::Sha256);
}

void writeJson(const QString& file, const QJsonObject& what, const QString& name)
{
    QFile out(file);
    check(out.open(QIODevice::WriteOnly) && out.write(QJsonDocument(what).toJson()) > 0, name + u" was not written"_s);
}

struct Kvm {
    core::OwnKvm kvm;
    QString password;
    QString certificatePem;
};

// The first start, as main.cpp has it: the key of the sign-ins' counts first, then the app password, which is set
// with automatic sign-in on this PC; `then` with the lock, before it goes.
void firstStart(const QString& settingsFile, core::testing::MemorySecrets& secrets, core::PacingFile& signIns,
                const QDateTime& now, const std::function<void(ui::AppLock&)>& then = {})
{
    signIns.read();
    check(signIns.keeps(), u"the sign-ins' counts are not kept"_s);
    const qint64 seconds = now.toSecsSinceEpoch();
    ui::AppLock lock(
        secrets, &ui::AppLock::derivePassword, [seconds] { return seconds; }, nullptr, settingsFile + u".locked"_s,
        [] { return qint64{5000}; });
    check(lock.mode() == u"setup"_s, u"a first start is not asked for an app password: "_s + lock.mode());
    lock.submit({}, appPassword, appPassword, true);
    check(waitFor([&lock] { return !lock.busy(); }, 300'000), u"the app password took too long"_s);
    check(!lock.restricted() && lock.automatic(), u"the app password was not set: "_s + lock.problem());
    if (then) {
        then(lock);
    }
}

// The example certificate of the server's own files of a wall (core/tests/data/export), as a file of the wall holds it:
// one that Windows reads as a certificate, and nobody's.
QString exampleCertificate()
{
    QFile file(QStringLiteral(QKVM_EXPORT_EXAMPLES) + u"/plain.json"_s);
    check(file.open(QIODevice::ReadOnly), u"the example files of a wall are not there"_s);
    const QJsonObject read = QJsonDocument::fromJson(file.readAll()).object();
    return read[u"devices"_s].toArray().first().toObject()[u"cert_pem"_s].toString();
}

void dumpSecrets(const QDir& out, const core::testing::MemorySecrets& secrets)
{
    QJsonObject kept;
    for (auto one = secrets.all.cbegin(); one != secrets.all.cend(); ++one) {
        kept.insert(one.key(), QJsonObject{{u"bytes"_s, QString::fromLatin1(one.value().toBase64())}});
    }
    writeJson(out.filePath(u"secrets.json"_s), kept, u"secrets.json"_s);
}

QJsonObject madeBy(const QString& commit, const QString& kind)
{
    return QJsonObject{{u"version"_s, QStringLiteral(QKVM_DESKTOP_VERSION)}, {u"commit"_s, commit}, {u"kind"_s, kind}};
}

// A PC nobody sits at: the app password set, a display link that signs a server's wall in here (so this PC is a wall
// screen of that server), and locked.
void makeAWallScreen(const QDir& out, const QString& commit, const QDateTime& now)
{
    const QString settingsFile = out.filePath(u"profile/settings.json"_s);
    core::testing::MemorySecrets secrets;
    core::Settings settings(settingsFile);
    core::SignInPacing pacing(core::SignInPacing::mostKept, [] { return qint64{1'000'000}; });
    core::PacingFile::Pace pace;
    pace.writtenAfterMs = 0;
    const qint64 nowMs = now.toMSecsSinceEpoch();
    pace.wallMs = [nowMs] { return nowMs; };
    core::PacingFile signIns(pacing, settingsFile + u".sign-ins"_s, secrets, pace);
    firstStart(settingsFile, secrets, signIns, now, [&](ui::AppLock& lock) {
        const StandIn server(&aScreensQkvm, screenServerPort);
        check(server.isListening(), u"port %1 of this PC is taken"_s.arg(screenServerPort));
        ui::WallScreen screen(secrets);
        ui::ServerSource source(u"e5f6a7b8"_s, settings, secrets, 200);
        source.markScreensIn(screen);
        source.connectTo(u"127.0.0.1:47392/#display&key="_s + QString::fromLatin1(displayKey));
        check(waitFor([&source] { return source.isAtTheWall(); }, 30'000),
              u"the display link did not sign the wall in: "_s + source.shown().problem);
        check(waitFor([&source] { return source.feed().machines().size() == 1; }, 30'000),
              u"the screen's wall shows nothing"_s);
        check(screen.isOne(), u"this PC did not become a wall screen"_s);
        // (As ServerSession tells the lock of a wall screen; and then whoever set it up locked it.)
        lock.becameWallScreen();
        source.feed().stop();
        lock.lock();
        check(lock.locked() && lock.wallScreen(), u"the wall screen was not locked"_s);
    });
    check(QFile::exists(settingsFile + u".locked"_s), u"the lock left no marker"_s);
    dumpSecrets(out, secrets);
    writeJson(out.filePath(u"made.json"_s),
              QJsonObject{{u"made_by"_s, madeBy(commit, u"wall-screen"_s)},
                          {u"now"_s, now.toString(Qt::ISODate)},
                          {u"now_ms"_s, static_cast<double>(nowMs)},
                          {u"app_password"_s, appPassword},
                          {u"automatic"_s, true},
                          {u"locked"_s, true},
                          {u"server"_s, QJsonObject{{u"address"_s, screenServerAddress},
                                                    {u"port"_s, screenServerPort},
                                                    {u"display_key"_s, QString::fromLatin1(displayKey)},
                                                    {u"session"_s, QString::fromLatin1(screenSession)},
                                                    {u"account"_s, u"Lobby screen"_s}}}},
              u"made.json"_s);
}

// The version after this one, as its update would name it ("0.1.0" -> "0.1.1").
QString nextVersion()
{
    const QVersionNumber now = QVersionNumber::fromString(QStringLiteral(QKVM_DESKTOP_VERSION));
    return QVersionNumber(now.majorVersion(), now.minorVersion(), now.microVersion() + 1).toString();
}

void makeAPerson(const QDir& out, const QString& commit, const QDateTime& now);

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (argc != 3 && argc != 4) {
        stop(u"give an empty folder, the commit this was built at, and person or wall-screen"_s);
    }
    const QDir out(QString::fromLocal8Bit(argv[1]));
    const QString commit = QString::fromLocal8Bit(argv[2]);
    const QString kind = argc == 4 ? QString::fromLocal8Bit(argv[3]) : u"person"_s;
    check(kind == u"person"_s || kind == u"wall-screen"_s, u"a profile is of a person or of a wall screen"_s);
    check(out.exists() && out.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
          u"the folder is to be there, and empty"_s);
    check(QDir().mkpath(out.filePath(u"profile"_s)), u"profile/ could not be made"_s);
    // The PC's clock is held still: what is kept of times is what a test reads back.
    const QDateTime now(QDate(2026, 11, 14), QTime(12, 0), QTimeZone::UTC);
    if (kind == u"wall-screen"_s) {
        makeAWallScreen(out, commit, now);
    } else {
        makeAPerson(out, commit, now);
    }
    std::printf("made a profile (%s) of %s into %s\n", qPrintable(kind), QKVM_DESKTOP_VERSION,
                qPrintable(QDir::toNativeSeparators(out.path())));
    return 0;
}

namespace {

void makeAPerson(const QDir& out, const QString& commit, const QDateTime& now)
{
    const QString settingsFile = out.filePath(u"profile/settings.json"_s);
    const qint64 nowMs = now.toMSecsSinceEpoch();
    core::testing::MemorySecrets secrets;
    core::Settings settings(settingsFile);
    core::SignInPacing pacing(core::SignInPacing::mostKept, [] { return qint64{1'000'000}; });
    core::PacingFile::Pace pace;
    pace.writtenAfterMs = 0;
    pace.wallMs = [nowMs] { return nowMs; };
    core::PacingFile signIns(pacing, settingsFile + u".sign-ins"_s, secrets, pace);
    firstStart(settingsFile, secrets, signIns, now);

    // A qKVM server signed in to, kept signed in on this PC; then This PC's own wall is gone to.
    {
        const StandIn server(&aQkvm, serverPort);
        check(server.isListening(), u"port %1 of this PC is taken"_s.arg(serverPort));
        ui::ServerSource source(u"a1b2c3d4"_s, settings, secrets, 200);
        source.connectTo(u"127.0.0.1:47391"_s);
        check(waitFor([&source] { return source.shown().stage == u"signIn"_s; }, 30'000),
              u"the server did not ask for a password: "_s + source.shown().stage);
        source.signIn(u"anna"_s, serverPassword, {}, true);
        check(waitFor([&source] { return source.isAtTheWall(); }, 30'000),
              u"the server's wall was not come to: "_s + source.shown().problem);
        check(waitFor([&source] { return source.feed().machines().size() == 1; }, 30'000),
              u"the server's wall shows nothing"_s);
        source.standAside();
    }
    check(secrets.all.value(u"session at "_s + serverAddress) == serverSession,
          u"the session is not kept (the store holds: %1)"_s.arg(secrets.all.keys().join(u", "_s)));

    // This PC's own wall: a kvmd known by its certificate, a kvmd over plain HTTP with two screen watches, a JetKVM,
    // and a server's management controller, each with its password, as the box that adds a KVM hands it over; and a
    // kvmd from a file of the wall, with its certificate itself. And three saved actions: one that presses keys, one
    // that types a short text, and one whose text is kept in two parts.
    QList<Kvm> kvms;
    {
        core::OwnKvm one;
        one.kind = u"kvmd"_s;
        one.name = u"Build server"_s;
        one.address = u"https://192.0.2.10"_s;
        one.user = u"admin"_s;
        one.group = u"Rack A"_s;
        one.knownByCertificate = true;
        one.certificate = madeUpDigest("a made-up certificate of the build server");
        kvms.append({one, u"made-up kvmd password"_s, {}});
    }
    {
        core::OwnKvm one;
        one.kind = u"kvmd"_s;
        one.name = u"Lab KVM"_s;
        one.address = u"http://192.0.2.11:8080"_s;
        one.user = u"operator"_s;
        one.group = u"Rack A"_s;
        one.noSignalMinutes = 15;
        one.unchangedMinutes = 60;
        kvms.append({one, u"made-up lab password"_s, {}});
    }
    {
        core::OwnKvm one;
        one.kind = u"jetkvm"_s;
        one.name = u"Desk JetKVM"_s;
        one.address = u"http://192.0.2.12"_s;
        one.group = u"Desk"_s;
        one.number = u"jetkvm:"_s + QString::fromLatin1(madeUpDigest("a made-up JetKVM").toHex().left(32));
        kvms.append({one, u"made-up JetKVM password"_s, {}});
    }
    {
        core::OwnKvm one;
        one.kind = u"redfish"_s;
        one.name = u"Storage server"_s;
        one.address = u"https://198.51.100.30"_s;
        one.user = u"operator"_s;
        one.group = u"Rack B"_s;
        one.knownByCertificate = true;
        one.certificate = madeUpDigest("a made-up certificate of the storage server's controller");
        one.machine = u"server:"_s + QString::fromLatin1(madeUpDigest("a made-up server").toHex());
        kvms.append({one, u"made-up controller password"_s, {}});
    }
    {
        const QString pem = exampleCertificate();
        const std::optional<core::exchange::KeptCertificate> read = core::exchange::certificateFrom(pem);
        check(read.has_value(), u"the example certificate does not read"_s);
        core::OwnKvm one;
        one.kind = u"kvmd"_s;
        one.name = u"Imported KVM"_s;
        one.address = u"https://198.51.100.31"_s;
        one.user = u"admin"_s;
        one.group = u"Rack B"_s;
        one.knownByCertificate = true;
        one.certificate = read.value_or(core::exchange::KeptCertificate{}).sha256;
        kvms.append({one, u"made-up imported password"_s, read.value_or(core::exchange::KeptCertificate{}).pem});
    }
    struct Typed {
        core::OwnActions::Typed typed;
        QString id;
    };
    QList<Typed> actions;
    {
        core::LocalWall wall(settings, secrets);
        for (qsizetype index = 0; index < 4; ++index) {
            Kvm& one = kvms[index];
            const base::Result<QString> added = wall.add(one.kvm, one.password);
            check(static_cast<bool>(added), u"%1 was not added"_s.arg(one.kvm.name));
            one.kvm.id = added.value();
        }
        Kvm& imported = kvms[4];
        const QList<base::Result<QString>> taken =
            wall.takeFromAFile({core::LocalWall::FromAFile{imported.kvm, imported.password, imported.certificatePem}});
        check(taken.size() == 1 && taken.front(), u"the KVM of a file was not taken"_s);
        imported.kvm.id = taken.front().value();
        check(!wall.certificateOf(imported.kvm.id).isEmpty(), u"the certificate itself was not kept"_s);
        core::OwnActions::Typed presses;
        presses.name = u"Restart (Ctrl+Alt+Del)"_s;
        presses.keys = u"ControlLeft,AltLeft,Delete"_s;
        presses.danger = true;
        core::OwnActions::Typed types;
        types.name = u"Type the BIOS password"_s;
        types.types = true;
        types.text = u"made-up typed text"_s;
        types.enter = true;
        // (1800 signs, 2600 bytes in UTF-8: two parts of the store. The accented letters are made from their number.)
        core::OwnActions::Typed longer;
        longer.name = u"Type the long note"_s;
        longer.types = true;
        longer.text = QString(u"made-up long text "_s).repeated(55).left(1000) + QString(800, QChar(0x00E9));
        for (const core::OwnActions::Typed& typed : {presses, types, longer}) {
            const auto added = wall.actions().add(typed);
            check(static_cast<bool>(added), u"%1 was not saved"_s.arg(typed.name));
            actions.append({typed, added.value().id});
        }
        check(secrets.all.contains(core::OwnActions::secretOf(actions.back().id, u'a', 2)),
              u"the long text is not kept in two parts"_s);
    }

    // What a person chooses, and where the windows were: as the settings box, the windows and the menus remember it.
    // And a KVM of a family this version does not speak to, as a newer build would have left it: kept as it stands.
    const QUrl server(serverAddress);
    const QString serverScope = scopeOf(server, u"u1"_s);
    const QJsonObject unknownFamily{{u"id"_s, u"0f1e2d3c"_s},
                                    {u"kind"_s, u"vnc"_s},
                                    {u"name"_s, u"A family of later"_s},
                                    {u"address"_s, u"https://198.51.100.32"_s},
                                    {u"user"_s, u"admin"_s},
                                    {u"group"_s, u"Rack B"_s},
                                    {u"known_by_certificate"_s, false},
                                    {u"certificate"_s, u""_s},
                                    {u"asked"_s, false},
                                    {u"number"_s, u""_s},
                                    {u"machine"_s, u""_s},
                                    {u"display"_s, 2}};
    {
        core::Remembered chosen = settings.remembered();
        chosen.theme = u"light"_s;
        chosen.staysInTray = true;
        chosen.tellsNews = false;
        chosen.releaseKey = u"scroll-lock"_s;
        chosen.hardwareDecoding = u"off"_s;
        chosen.graphicsCard = u"power-saving"_s;
        chosen.checkUpdates = false;
        chosen.lastUpdateCheck = now.addDays(-4);
        chosen.showsOwnWall = true;
        chosen.settingsSection = u"security"_s;
        chosen.wallPictureEveryMs = 2000;
        chosen.favorites = {{u"pc"_s, kvms.at(0).kvm.id}, {serverScope, u"a"_s}};
        chosen.recentMachines = {{u"pc"_s, kvms.at(2).kvm.id}, {u"pc"_s, kvms.at(0).kvm.id}, {serverScope, u"a"_s}};
        chosen.pictureScales = {{{u"pc"_s, kvms.at(0).kvm.id}, u"125"_s}, {{serverScope, u"a"_s}, u"75"_s}};
        chosen.reachedEncrypted = {u"198.51.100.20"_s};
        chosen.certificates.insert(u"198.51.100.20:443"_s, madeUpDigest("a made-up certificate of another server"));
        chosen.kvmsItDoesNotRead.append(unknownFamily);
        core::WallWindow first;
        first.source = u"pc"_s;
        first.place = core::WindowPlace{40, 40, 960, 640, false, false};
        first.group = u"Rack A"_s;
        first.order = u"group"_s;
        first.columns = 3;
        core::WallWindow second;
        second.source = serverAddress;
        second.place = core::WindowPlace{80, 60, 900, 600, true, false};
        second.order = u"state"_s;
        second.rotates = true;
        chosen.windows = {first, second};
        core::WallWindow night;
        night.source = u"pc"_s;
        night.place = core::WindowPlace{0, 0, 1024, 700, false, true};
        night.group = u"Desk"_s;
        night.order = u"name"_s;
        night.columns = 2;
        night.rotates = true;
        chosen.workspaces = {core::Workspace{
            QString::fromLatin1(madeUpDigest("night shift").toHex().left(32)), u"Night shift"_s, {night, first}}};
        check(static_cast<bool>(settings.remember(chosen)), u"the settings were not written"_s);
    }

    // Sign-ins that were refused, and one that came to nothing: what the next start is to hold still.
    core::SignInPacing::Steps steps;
    const core::OwnKvm& lab = kvms.at(1).kvm;
    const core::OwnKvm& jet = kvms.at(2).kvm;
    const auto refused = core::SignInPacing::what(u"login", QUrl(lab.address), lab.user, wrongPassword);
    static_cast<void>(pacing.saidNo(refused, steps));
    const qint64 refusedLeft = pacing.saidNo(refused, steps);
    const auto silent = core::SignInPacing::what(u"login", QUrl(jet.address), {}, kvms.at(2).password);
    const qint64 silentLeft = pacing.unanswered(silent, steps);
    check(signIns.waitForWritten(10'000), u"the sign-ins' counts were not written"_s);

    // A screen watch that went off, with the picture that set it off kept (alert-pictures), and was paused; and a
    // server whose BIOS setup boot override may still be set. (Each as this version holds it once it is written: what
    // the next one is to read back.)
    const QString watched = core::Incidents::screenKey(lab.id, u"no_signal"_s);
    QJsonArray madeIncidents;
    {
        core::Incidents incidents(settingsFile + u".incidents"_s, [when = now] { return when; });
        static_cast<void>(incidents.screenAlert(u"pc"_s, lab.id, lab.name, u"no_signal"_s, false, 15));
        incidents.pause(u"pc"_s, watched, true);
        check(incidents.flush(), u"the incidents were not written"_s);
        for (const core::Incidents::Entry& entry : incidents.entries(u"pc"_s)) {
            madeIncidents.append(
                QJsonObject{{u"id"_s, entry.id},
                            {u"scope"_s, entry.scope},
                            {u"key"_s, entry.key},
                            {u"name"_s, entry.name},
                            {u"state"_s, entry.state},
                            {u"opened"_s, entry.opened.toString(Qt::ISODateWithMs)},
                            {u"minutes"_s, entry.minutes},
                            {u"paused_until"_s, incidents.pausedUntil(u"pc"_s, watched).toString(Qt::ISODateWithMs)}});
        }
    }
    QJsonObject madeAlert;
    {
        qint64 clock = 0;
        core::ScreenWatch::Times times;
        times.roundMs = 20;
        times.minuteMs = 40;
        times.clockMs = [&clock] { return clock; };
        times.wallClock = [now, &clock] { return now.addMSecs(clock); };
        core::ScreenWatch watch(times);
        bool read = false;
        QObject::connect(&watch, &core::ScreenWatch::readBack, &watch, [&read] { read = true; });
        QList<core::ScreenWatch::Event> told;
        QObject::connect(&watch, &core::ScreenWatch::alerted, &watch,
                         [&told](const core::ScreenWatch::Event& event) { told.append(event); });
        watch.keepIn(out.filePath(u"profile/alert-pictures"_s));
        const QString machine = lab.id;
        // (Lit first, with a picture of its screen; then dark for the watch's minute: the last lit picture is kept.)
        QString state = u"online"_s;
        watch.watch([machine, &state] { return QList<core::ScreenWatch::Watched>{{machine, state, 1, 0}}; });
        check(waitFor([&read] { return read; }, 10'000), u"the screen watch did not read back"_s);
        QImage screen(64, 36, QImage::Format_RGB32);
        screen.fill(QColor(0x20, 0x60, 0x80));
        QBuffer jpeg;
        check(jpeg.open(QIODevice::WriteOnly) && screen.save(&jpeg, "JPG"), u"no picture of a screen was made"_s);
        const QByteArray pictureOfIt = jpeg.data();
        // (A round first, which takes it in among the watched; then the picture, and a round that keeps it in mind.)
        const auto rounds = [&clock](qint64 until) {
            check(waitFor(
                      [&clock, until] {
                          clock += 10;
                          return clock > until;
                      },
                      10'000),
                  u"the screen watch's clock did not go"_s);
        };
        rounds(100);
        watch.saw(machine, pictureOfIt, true);
        rounds(200);
        state = u"no_signal"_s;
        watch.saw(machine, pictureOfIt, false);
        check(waitFor(
                  [&clock, &told] {
                      clock += 10;
                      return !told.isEmpty();
                  },
                  30'000),
              u"the screen watch did not go off"_s);
        const QString picture = out.filePath(u"profile/alert-pictures/%1-%2.jpg"_s.arg(machine, told.front().stamp));
        check(waitFor(
                  [&] {
                      return QFile::exists(picture) &&
                             QFile::exists(out.filePath(u"profile/alert-pictures/screens.json"_s));
                  },
                  10'000),
              u"the screen watch's picture was not kept"_s);
        madeAlert = QJsonObject{{u"machine"_s, machine},
                                {u"watch"_s, told.front().watch},
                                {u"stamp"_s, told.front().stamp},
                                {u"minutes"_s, told.front().minutes},
                                {u"picture"_s, QString::fromLatin1(pictureOfIt.toBase64())}};
    }
    const core::OwnKvm& storage = kvms.at(3).kvm;
    QJsonArray madeOverrides;
    {
        core::SetupOverrides overrides(settingsFile + u".setup-overrides"_s, [when = now] { return when; });
        static_cast<void>(overrides.mayBeSet(QUrl(storage.address), storage.machine, now.addSecs(120)));
        check(overrides.written().value_or(false), u"the setup overrides were not written"_s);
        for (const core::SetupOverrides::Left& left : overrides.all()) {
            madeOverrides.append(QJsonObject{{u"address"_s, left.address},
                                             {u"machine"_s, left.machine},
                                             {u"since"_s, left.since.toString(Qt::ISODateWithMs)},
                                             {u"doubt_until"_s, left.doubtUntil.toString(Qt::ISODateWithMs)}});
        }
    }
    // An update handed over to Setup ("Install and restart"), and Setup's log of it: the note in the form this
    // version's Updates::noteHandOver writes it (a Setup's start alone runs that), the log as Setup writes one. The
    // log is Setup's file, not the program's, and the repository keeps no .log (tools/check_staged.py): made.json
    // holds it, with the name Setup gives it, and the test puts it there.
    const QString to = nextVersion();
    const QJsonObject setupsLog{
        {u"file"_s, u"updates/setup-%1.log"_s.arg(to)},
        {u"text"_s, u"2026-11-14 12:00:01.000   Log opened. (Time zone: UTC+00:00)\r\n"
                    u"2026-11-14 12:00:01.001   Setup version: Inno Setup version 6.7.4\r\n"
                    u"2026-11-14 12:01:01.002   qKVM-RESULT: 105 the program was still running after 60 seconds; "
                    u"nothing was changed\r\n"
                    u"2026-11-14 12:01:01.003   Log closed.\r\n"_s}};
    {
        check(QDir().mkpath(out.filePath(u"profile/updates"_s)), u"updates/ could not be made"_s);
        QSaveFile note(out.filePath(u"profile/updates/setup-handover.json"_s));
        const QJsonObject said{{u"schema"_s, 1},
                               {u"from"_s, QStringLiteral(QKVM_DESKTOP_VERSION)},
                               {u"to"_s, to},
                               {u"process"_s, static_cast<qint64>(999'999)},
                               {u"created"_s, u"133000000000000000"_s},
                               {u"temp"_s, QString()},
                               {u"at"_s, now.toString(Qt::ISODate)}};
        check(note.open(QIODevice::WriteOnly) && note.write(QJsonDocument(said).toJson(QJsonDocument::Compact)) > 0 &&
                  note.commit(),
              u"the hand-over note was not written"_s);
    }

    dumpSecrets(out, secrets);
    QJsonArray madeKvms;
    for (const Kvm& one : std::as_const(kvms)) {
        madeKvms.append(QJsonObject{{u"id"_s, one.kvm.id},
                                    {u"name"_s, one.kvm.name},
                                    {u"kind"_s, one.kvm.kind},
                                    {u"address"_s, one.kvm.address},
                                    {u"user"_s, one.kvm.user},
                                    {u"group"_s, one.kvm.group},
                                    {u"password"_s, one.password},
                                    {u"known_by_certificate"_s, one.kvm.knownByCertificate},
                                    {u"certificate"_s, QString::fromLatin1(one.kvm.certificate.toHex())},
                                    {u"certificate_pem"_s, one.certificatePem},
                                    {u"number"_s, one.kvm.number},
                                    {u"machine"_s, one.kvm.machine},
                                    {u"no_signal_minutes"_s, one.kvm.noSignalMinutes},
                                    {u"unchanged_minutes"_s, one.kvm.unchangedMinutes}});
    }
    QJsonArray madeActions;
    for (const Typed& one : std::as_const(actions)) {
        madeActions.append(QJsonObject{{u"id"_s, one.id},
                                       {u"name"_s, one.typed.name},
                                       {u"types"_s, one.typed.types},
                                       {u"keys"_s, one.typed.keys},
                                       {u"text"_s, one.typed.text},
                                       {u"enter"_s, one.typed.enter},
                                       {u"danger"_s, one.typed.danger}});
    }
    writeJson(out.filePath(u"made.json"_s),
              QJsonObject{
                  {u"made_by"_s, madeBy(commit, u"person"_s)},
                  {u"now"_s, now.toString(Qt::ISODate)},
                  {u"now_ms"_s, static_cast<double>(nowMs)},
                  {u"app_password"_s, appPassword},
                  {u"automatic"_s, true},
                  {u"kvms"_s, madeKvms},
                  {u"kvm_of_another_family"_s, unknownFamily},
                  {u"actions"_s, madeActions},
                  {u"server"_s, QJsonObject{{u"address"_s, serverAddress},
                                            {u"port"_s, serverPort},
                                            {u"account"_s, u"anna"_s},
                                            {u"account_id"_s, u"u1"_s},
                                            {u"password"_s, serverPassword},
                                            {u"session"_s, QString::fromLatin1(serverSession)},
                                            {u"scope"_s, serverScope}}},
                  {u"pacing"_s, QJsonArray{QJsonObject{{u"road"_s, u"login"_s},
                                                       {u"address"_s, lab.address},
                                                       {u"user"_s, lab.user},
                                                       {u"password"_s, wrongPassword},
                                                       {u"noes"_s, 2},
                                                       {u"silent"_s, 0},
                                                       {u"left_ms"_s, static_cast<double>(refusedLeft)}},
                                           QJsonObject{{u"road"_s, u"login"_s},
                                                       {u"address"_s, jet.address},
                                                       {u"user"_s, QString()},
                                                       {u"password"_s, kvms.at(2).password},
                                                       {u"noes"_s, 0},
                                                       {u"silent"_s, 1},
                                                       {u"left_ms"_s, static_cast<double>(silentLeft)}}}},
                  {u"incidents"_s, madeIncidents},
                  {u"alert"_s, madeAlert},
                  {u"setup_overrides"_s, madeOverrides},
                  {u"update"_s, QJsonObject{{u"from"_s, QStringLiteral(QKVM_DESKTOP_VERSION)},
                                            {u"to"_s, to},
                                            {u"setup_log"_s, setupsLog}}},
              },
              u"made.json"_s);
}

} // namespace
