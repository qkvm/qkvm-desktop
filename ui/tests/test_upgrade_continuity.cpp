#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <qkvm/core/incidents.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/pacing_file.h>
#include <qkvm/core/profile.h>
#include <qkvm/core/screen_watch.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/setup_overrides.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/update_release.h>
#include <qkvm/core/updates.h>

#include "app_lock.h"
#include "application.h"
#include "memory_secrets.h"
#include "profile_writing.h"
#include "server_source.h"
#include "stand_in.h"
#include "wall_of_a_test.h"
#include "wall_screen.h"
#include "win/cache_paths.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

// Everything a published version kept survives the upgrade to this one (SPEC.md section 11, the owner's word of
// 2026-11-14): each version's own profile, as its own code made it (tests/fixtures/profiles/<version>, the README
// there says how), is read by this build from a scratch folder and a store in memory, and has to come up as the
// person left it. Nothing of a test reaches Windows' Credential Manager or the person's own profile, and the one
// server spoken to is a stand-in on this PC's own address; the KVMs are at addresses set aside for examples, and
// nothing is asked of them.
namespace {

// Every version that was published: a fixture of each, of a person at the PC and of a wall screen, and none may go.
const QStringList published{u"0.1.0"_s, u"0.1.1"_s};
const QString wallScreen = u"-wall-screen"_s;

QJsonObject jsonIn(const QString& file)
{
    QFile kept(file);
    if (!kept.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(kept.readAll()).object();
}

// A published version's fixture, as it is kept here (read only).
struct Fixture {
    QString folder;
    // What the person did and typed, in plain words (made.json).
    QJsonObject made;
    // The settings as that version wrote them.
    QJsonObject settings;
    // Every secret, by its name: the name Windows kept it under, and its bytes.
    QJsonObject secrets;

    explicit Fixture(const QString& at)
        : folder(at), made(jsonIn(at + u"/made.json"_s)), settings(jsonIn(at + u"/profile/settings.json"_s)),
          secrets(jsonIn(at + u"/secrets.json"_s))
    {}
    bool whole() const { return !folder.isEmpty() && !made.isEmpty() && !settings.isEmpty() && !secrets.isEmpty(); }
    QString profileFolder() const { return folder + u"/profile"_s; }
    QDateTime now() const { return QDateTime::fromString(made["now"_L1].toString(), Qt::ISODate); }
    qint64 nowMs() const { return made["now_ms"_L1].toInteger(); }
    QJsonObject server() const { return made["server"_L1].toObject(); }
};

// The fixture as a profile of this PC's: its files copied into a scratch folder, its secrets into a store in memory.
struct Profile {
    explicit Profile(const Fixture& fixture)
    {
        const QDir into(home.filePath(u"QKVM Desktop"_s));
        if (!QDir().mkpath(into.path())) {
            return;
        }
        settingsFile = into.filePath(u"settings.json"_s);
        for (const QString& file : testing::profiles::filesUnder(fixture.profileFolder())) {
            copied = QDir().mkpath(QFileInfo(into.filePath(file)).absolutePath()) &&
                     QFile::copy(QDir(fixture.profileFolder()).filePath(file), into.filePath(file)) && copied;
        }
        // And Setup's log, where Setup writes it: the repository keeps no .log, and made.json holds it.
        const QJsonObject log = fixture.made["update"_L1].toObject()["setup_log"_L1].toObject();
        if (!log.isEmpty()) {
            QFile written(into.filePath(log["file"_L1].toString()));
            copied = QDir().mkpath(QFileInfo(written).absolutePath()) && written.open(QIODevice::NewOnly) &&
                     written.write(log["text"_L1].toString().toUtf8()) > 0 && copied;
        }
        for (auto one = fixture.secrets.begin(); one != fixture.secrets.end(); ++one) {
            secrets.all.insert(one.key(),
                               QByteArray::fromBase64(one.value().toObject()["bytes"_L1].toString().toLatin1()));
        }
    }

    QTemporaryDir home;
    QString settingsFile;
    bool copied = true;
    core::testing::MemorySecrets secrets;
};

core::PacingFile::Pace paceOf(const Fixture& fixture)
{
    core::PacingFile::Pace pace;
    pace.writtenAfterMs = 0;
    pace.wallMs = [now = fixture.nowMs()] { return now; };
    return pace;
}

// The app lock as the program makes it, on the fixture's clock.
std::unique_ptr<ui::AppLock> lockOf(Profile& profile, const Fixture& fixture)
{
    const qint64 seconds = fixture.now().toSecsSinceEpoch();
    return std::make_unique<ui::AppLock>(
        profile.secrets, &ui::AppLock::derivePassword, [seconds] { return seconds; }, nullptr,
        core::profile::lockFile(profile.settingsFile), [] { return qint64{5000}; });
}

QList<core::MachineReference> referencesIn(const QJsonValue& value)
{
    QList<core::MachineReference> all;
    for (const QJsonValue one : value.toArray()) {
        all.append({one["scope"_L1].toString(), one["machine"_L1].toString()});
    }
    return all;
}

core::WallWindow windowIn(const QJsonValue& said)
{
    core::WallWindow window;
    window.source = said["source"_L1].toString();
    window.group = said["group"_L1].toString();
    window.order = said["order"_L1].toString();
    window.columns = said["columns"_L1].toInt();
    window.rotates = said["rotate"_L1].toBool();
    if (const QJsonObject place = said["place"_L1].toObject(); !place.isEmpty()) {
        window.place = core::WindowPlace{place["x"_L1].toInt(),          place["y"_L1].toInt(),
                                         place["width"_L1].toInt(),      place["height"_L1].toInt(),
                                         place["maximized"_L1].toBool(), place["full_screen"_L1].toBool()};
    }
    return window;
}

// Where `old` says something that `now` does not say the same: every key of an object, every place of a list, as a
// path ("kvms[2].user"). What `now` says more is no loss.
void lostFrom(const QJsonValue& old, const QJsonValue& now, const QString& at, QStringList& lost)
{
    if (old.isObject() && now.isObject()) {
        const QJsonObject was = old.toObject();
        const QJsonObject is = now.toObject();
        for (auto one = was.begin(); one != was.end(); ++one) {
            const QString path = at.isEmpty() ? one.key() : at + u'.' + one.key();
            if (!is.contains(one.key())) {
                lost.append(path + u" (gone)"_s);
            } else {
                lostFrom(one.value(), is[one.key()], path, lost);
            }
        }
        return;
    }
    if (old.isArray() && now.isArray()) {
        const QJsonArray was = old.toArray();
        const QJsonArray is = now.toArray();
        for (qsizetype index = 0; index < was.size(); ++index) {
            const QString path = u"%1[%2]"_s.arg(at).arg(index);
            if (index >= is.size()) {
                lost.append(path + u" (gone)"_s);
            } else {
                lostFrom(was.at(index), is.at(index), path, lost);
            }
        }
        return;
    }
    if (old != now) {
        lost.append(u"%1 (%2, now %3)"_s.arg(
            at, QString::fromUtf8(QJsonDocument(QJsonArray{old}).toJson(QJsonDocument::Compact)),
            QString::fromUtf8(QJsonDocument(QJsonArray{now}).toJson(QJsonDocument::Compact))));
    }
}

// A QKVM as far as signing in goes: whoever brings the session it gave is signed in.
Answered aQkvm(const Asked& asked, bool signedIn)
{
    if (asked.path == "/healthz") {
        return {200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"], "page": "3f2a9c", "locked": false})"};
    }
    if (asked.path == "/api/v1/session") {
        return {200, signedIn ? QByteArray(R"({"initialized": true, "authenticated": true, "ask_name": true,
                                    "user": {"id": "u1", "name": "anna", "role": "admin", "token": false}})")
                              : QByteArray(R"({"initialized": true, "authenticated": false, "ask_name": true,
                                    "user": null})")};
    }
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

} // namespace

class TestUpgradeContinuity : public QObject {
    Q_OBJECT

    // The rows: each published version's fixture of a person (`screens`: of a wall screen; `both`: of either), and any
    // other of that kind that is there. A published one that is not there is a row that fails.
    static void fixtures(bool screens = false, bool both = false)
    {
        QTest::addColumn<QString>("folder");
        const QDir all(QStringLiteral(QKVM_PROFILE_FIXTURES));
        const QStringList there = all.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const bool ofScreens : {false, true}) {
            if (!both && ofScreens != screens) {
                continue;
            }
            for (const QString& version : published) {
                if (!there.contains(version + (ofScreens ? wallScreen : QString()))) {
                    QTest::newRow(qPrintable(version + (ofScreens ? wallScreen : QString()))) << QString();
                }
            }
            for (const QString& name : there) {
                if (name.endsWith(wallScreen) == ofScreens) {
                    QTest::newRow(qPrintable(name)) << all.filePath(name);
                }
            }
        }
    }

    // The fixture of the row; the row fails where it is not there whole.
    static Fixture fixture()
    {
        QFETCH(QString, folder);
        return Fixture(folder);
    }

private slots:
    // The first thing the person sees: the wall, with no box that asks for the app password and no reset. (Secrets
    // without a readable app-password record go to "Reset saved secrets": app_lock.cpp. So the record of every
    // published version has to read.)
    void itStartsUnlockedUnderAutomaticSignIn_data() { fixtures(); }
    void itStartsUnlockedUnderAutomaticSignIn()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        QVERIFY(profile.copied);
        QVERIFY(fixture.made["automatic"_L1].toBool());
        // (As the program starts: the sign-ins' counts first, then the settings, then the lock.)
        core::SignInPacing pacing;
        core::PacingFile signIns(pacing, core::profile::signInsFile(profile.settingsFile), profile.secrets,
                                 paceOf(fixture));
        signIns.read();
        const core::Settings settings(profile.settingsFile);
        QVERIFY(!settings.wasUnreadable());
        const std::unique_ptr<ui::AppLock> lock = lockOf(profile, fixture);
        QCOMPARE(lock->mode(), QString());
        QVERIFY2(!lock->locked() && !lock->restricted(), qPrintable(lock->problem()));
        QVERIFY(lock->automatic());
        QVERIFY(lock->canStartSources());
        QVERIFY(!lock->wallScreen());
        QCOMPARE(lock->problem(), QString());
        QVERIFY(!QFile::exists(core::profile::lockFile(profile.settingsFile)));
    }

    // The wall, the saved actions, the favorites and the machines seen last, the windows and the layouts, what was
    // chosen, and the updates' settings: all as the old version wrote them.
    void theWallAndEverythingChosenReadTheSame_data() { fixtures(); }
    void theWallAndEverythingChosenReadTheSame()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        core::Settings settings(profile.settingsFile);
        QVERIFY(!settings.wasUnreadable());
        const core::Remembered& now = settings.remembered();
        const QJsonObject& old = fixture.settings;

        // What was chosen.
        QCOMPARE(now.theme, old["theme"_L1].toString());
        QCOMPARE(now.staysInTray, old["stay_in_tray"_L1].toBool());
        QCOMPARE(now.tellsNews, old["tell_news"_L1].toBool());
        QCOMPARE(now.releaseKey, old["release_key"_L1].toString());
        QCOMPARE(now.hardwareDecoding, old["hardware_decoding"_L1].toString());
        QCOMPARE(now.graphicsCard, old["graphics_card"_L1].toString());
        QCOMPARE(now.wallPictureEveryMs, old["picture_every_ms"_L1].toInt());
        QCOMPARE(now.showsOwnWall, old["own_wall"_L1].toBool());
        QCOMPARE(now.settingsSection, old["settings_section"_L1].toString());
        // The updates.
        QCOMPARE(now.checkUpdates, old["check_updates"_L1].toBool());
        QCOMPARE(now.lastUpdateCheck, QDateTime::fromString(old["last_update_check"_L1].toString(), Qt::ISODate));
        QVERIFY(now.lastUpdateCheck.isValid());
        // (Each of those differs from what a new profile has: one that was not read would not pass for read.)
        const core::Remembered fresh;
        QVERIFY(now.theme != fresh.theme && now.staysInTray != fresh.staysInTray && now.tellsNews != fresh.tellsNews &&
                now.releaseKey != fresh.releaseKey && now.hardwareDecoding != fresh.hardwareDecoding &&
                now.graphicsCard != fresh.graphicsCard && now.wallPictureEveryMs != fresh.wallPictureEveryMs &&
                now.showsOwnWall != fresh.showsOwnWall && now.settingsSection != fresh.settingsSection &&
                now.checkUpdates != fresh.checkUpdates);
        // The servers.
        QCOMPARE(now.server, fixture.server()["address"_L1].toString());
        QCOMPARE(now.servers, QStringList{fixture.server()["address"_L1].toString()});
        QCOMPARE(now.reachedEncrypted, old["reached_encrypted"_L1].toVariant().toStringList());
        QCOMPARE(now.certificates.size(), old["certificates"_L1].toObject().size());
        for (auto one = now.certificates.cbegin(); one != now.certificates.cend(); ++one) {
            QCOMPARE(QString::fromLatin1(one.value().toHex()), old["certificates"_L1][one.key()].toString());
        }
        // Favorites, the machines seen last, the size each was shown at.
        QVERIFY(now.favorites == referencesIn(old["favorites"_L1]));
        QVERIFY(now.recentMachines == referencesIn(old["recent_machines"_L1]));
        QCOMPARE(now.pictureScales.size(), old["picture_scales"_L1].toArray().size());
        for (qsizetype index = 0; index < now.pictureScales.size(); ++index) {
            const QJsonValue was = old["picture_scales"_L1][index];
            QVERIFY(now.pictureScales.at(index).machine == referencesIn(QJsonArray{was}).front());
            QCOMPARE(now.pictureScales.at(index).scale, was["scale"_L1].toString());
        }
        QVERIFY(!now.favorites.isEmpty() && !now.recentMachines.isEmpty() && !now.pictureScales.isEmpty());
        // Where the windows were, how each showed its wall, and the layouts.
        const QJsonArray windows = old["windows"_L1].toArray();
        QCOMPARE(now.windows.size(), windows.size());
        QVERIFY(!windows.isEmpty());
        for (qsizetype index = 0; index < windows.size(); ++index) {
            QVERIFY2(now.windows.at(index) == windowIn(windows.at(index)), qPrintable(u"window %1"_s.arg(index)));
            QVERIFY(core::WindowSlot(settings, index).remembered() == now.windows.at(index));
        }
        const QJsonArray workspaces = old["workspaces"_L1].toArray();
        QCOMPARE(now.workspaces.size(), workspaces.size());
        QVERIFY(!workspaces.isEmpty());
        for (qsizetype index = 0; index < workspaces.size(); ++index) {
            const core::Workspace& one = now.workspaces.at(index);
            QCOMPARE(one.id, workspaces.at(index)["id"_L1].toString());
            QCOMPARE(one.name, workspaces.at(index)["name"_L1].toString());
            const QJsonArray layout = workspaces.at(index)["windows"_L1].toArray();
            QCOMPARE(one.windows.size(), layout.size());
            for (qsizetype window = 0; window < layout.size(); ++window) {
                QVERIFY(one.windows.at(window) == windowIn(layout.at(window)));
            }
        }

        // This PC's own wall, in its order, each KVM as it was added, with its group; and as the wall has them.
        const QJsonArray kvms = fixture.made["kvms"_L1].toArray();
        QCOMPARE(now.kvms.size(), kvms.size());
        // A KVM of a family this build does not speak to, as a newer build left it: kept as it stands.
        QCOMPARE(now.kvmsItDoesNotRead, QJsonArray{fixture.made["kvm_of_another_family"_L1]});
        for (qsizetype index = 0; index < kvms.size(); ++index) {
            const QJsonValue made = kvms.at(index);
            const core::OwnKvm& kvm = now.kvms.at(index);
            QCOMPARE(kvm.id, made["id"_L1].toString());
            QCOMPARE(kvm.name, made["name"_L1].toString());
            QCOMPARE(kvm.kind, made["kind"_L1].toString());
            QCOMPARE(kvm.address, made["address"_L1].toString());
            QCOMPARE(kvm.user, made["user"_L1].toString());
            QCOMPARE(kvm.group, made["group"_L1].toString());
            QCOMPARE(kvm.knownByCertificate, made["known_by_certificate"_L1].toBool());
            QCOMPARE(QString::fromLatin1(kvm.certificate.toHex()), made["certificate"_L1].toString());
            QCOMPARE(kvm.number, made["number"_L1].toString());
            QCOMPARE(kvm.machine, made["machine"_L1].toString());
            QCOMPARE(kvm.noSignalMinutes, made["no_signal_minutes"_L1].toInt());
            QCOMPARE(kvm.unchangedMinutes, made["unchanged_minutes"_L1].toInt());
            QCOMPARE(kvm.askedWhatItIs, old["kvms"_L1][index]["asked"_L1].toBool());
            QVERIFY(now.kvmPasswordsKept.contains(kvm.id));
            // And the certificate itself, where a file of the wall brought it.
            QCOMPARE(now.kvmCertificates.value(kvm.id), made["certificate_pem"_L1].toString());
        }
        // The saved actions, in their order.
        const QJsonArray actions = fixture.made["actions"_L1].toArray();
        QCOMPARE(now.actions.size(), actions.size());
        for (qsizetype index = 0; index < actions.size(); ++index) {
            const QJsonValue made = actions.at(index);
            const core::OwnAction& action = now.actions.at(index);
            QCOMPARE(action.id, made["id"_L1].toString());
            QCOMPARE(action.name, made["name"_L1].toString());
            QCOMPARE(action.types, made["types"_L1].toBool());
            QCOMPARE(action.keys, made["types"_L1].toBool() ? QString() : made["keys"_L1].toString());
            QCOMPARE(action.enter, made["enter"_L1].toBool());
            QCOMPARE(action.danger, made["danger"_L1].toBool());
            QCOMPARE(action.writing, old["actions"_L1][index]["writing"_L1].toString());
        }
        core::testing::WallOfATest wall(settings, profile.secrets, core::KvmdDevice::Pace{});
        QVERIFY(wall.kvms() == now.kvms);
        QCOMPARE(wall.actions().all().size(), actions.size());
    }

    // What this build writes of what it read says all that the old file said, the same: nothing of it is lost at
    // the first writing (a key it no longer reads, a value it reads otherwise).
    void writingItAgainLosesNothingOfTheOldFile_data() { fixtures(false, true); }
    void writingItAgainLosesNothingOfTheOldFile()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        const core::Settings read(profile.settingsFile);
        const QString again = profile.home.filePath(u"again/settings.json"_s);
        core::Settings written(again);
        QVERIFY(written.remember(read.remembered()));
        QStringList lost;
        lostFrom(fixture.settings, jsonIn(again), {}, lost);
        QVERIFY2(lost.isEmpty(), qPrintable(lost.join(u"; "_s)));
    }

    // Every KVM's password, and every saved action's typed text, is read by the code that uses it: none is missing.
    void everyPasswordIsReadByTheCodeThatUsesIt_data() { fixtures(); }
    void everyPasswordIsReadByTheCodeThatUsesIt()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        core::Settings settings(profile.settingsFile);
        core::testing::WallOfATest wall(settings, profile.secrets, core::KvmdDevice::Pace{});
        const core::LocalWall::AsAFile file = wall.asAFile(true);
        QVERIFY2(file.withoutPassword.isEmpty(), "a KVM's password is not read");
        QVERIFY(file.content.devices);
        const QList<core::exchange::Device> devices = file.content.devices.value_or(QList<core::exchange::Device>());
        const QJsonArray kvms = fixture.made["kvms"_L1].toArray();
        QCOMPARE(devices.size(), kvms.size());
        for (const QJsonValue made : kvms) {
            const auto device = std::ranges::find_if(devices, [&made](const core::exchange::Device& one) {
                return one.address == made["address"_L1].toString();
            });
            QVERIFY(device != devices.end());
            QCOMPARE(device->password, made["password"_L1].toString());
            QVERIFY(!device->password.isEmpty());
            QVERIFY(wall.marksAPasswordFor(made["id"_L1].toString()));
        }
        const core::OwnActions::WithTheirTexts texts = wall.actions().textsHandedOutForASealedFile();
        QVERIFY2(texts.withoutText.isEmpty(), "a saved action's text is not read");
        int typing = 0;
        for (const QJsonValue made : fixture.made["actions"_L1].toArray()) {
            if (!made["types"_L1].toBool()) {
                continue;
            }
            ++typing;
            const auto action = std::ranges::find_if(texts.actions, [&made](const core::exchange::Action& one) {
                return one.name == made["name"_L1].toString();
            });
            QVERIFY(action != texts.actions.end());
            QCOMPARE(action->text, made["text"_L1].toString());
        }
        QVERIFY(typing > 0);
    }

    // The server signed in to is still signed in to: the program goes back to its wall by the session it kept, and
    // nobody is asked for a password.
    void theServerSessionIsStillKept_data() { fixtures(); }
    void theServerSessionIsStillKept()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        core::Settings settings(profile.settingsFile);
        const QJsonObject server = fixture.server();
        const QByteArray session = server["session"_L1].toString().toLatin1();
        QVERIFY(!session.isEmpty());
        int brought = 0;
        const StandIn qkvm(
            [&](const Asked& asked) {
                const bool signedIn = asked.headers.value("cookie").contains("qkvm_session=" + session);
                brought += signedIn ? 1 : 0;
                return aQkvm(asked, signedIn);
            },
            static_cast<quint16>(server["port"_L1].toInt()));
        QVERIFY2(qkvm.isListening(),
                 qPrintable(u"port %1 of this PC is taken: the stand-in server cannot be there"_s.arg(
                     server["port"_L1].toInt())));
        ui::ServerSource source(u"a1b2c3d4"_s, settings, profile.secrets, 200);
        // (As the program goes back to the server's wall that was shown last.)
        source.connectTo(settings.remembered().server);
        QCOMPARE(source.detailsClient()->session(), session);
        QTRY_VERIFY_WITH_TIMEOUT(source.isAtTheWall(), 30'000);
        QCOMPARE(source.shown().account, server["account"_L1].toString());
        QVERIFY(brought > 0);
        QCOMPARE(qkvm.count("/api/v1/login"), 0);
        source.standAside();
        // And nothing that was kept is gone: every secret is there, as it was.
        for (auto one = fixture.secrets.begin(); one != fixture.secrets.end(); ++one) {
            QCOMPARE(profile.secrets.all.value(one.key()),
                     QByteArray::fromBase64(one.value().toObject()["bytes"_L1].toString().toLatin1()));
        }
    }

    // The wrong-password counts still hold: a refused password is not offered again before its time.
    void theSignInCountsStillHold_data() { fixtures(); }
    void theSignInCountsStillHold()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        core::SignInPacing pacing(core::SignInPacing::mostKept, [] { return qint64{2'000'000}; });
        core::PacingFile signIns(pacing, core::profile::signInsFile(profile.settingsFile), profile.secrets,
                                 paceOf(fixture));
        signIns.read();
        QCOMPARE(signIns.putAside(), QString());
        QVERIFY(signIns.keeps());
        const QJsonArray kept = fixture.made["pacing"_L1].toArray();
        QVERIFY(!kept.isEmpty());
        QCOMPARE(pacing.kept(), kept.size());
        for (const QJsonValue one : kept) {
            const core::SignInPacing::Mark mark =
                core::SignInPacing::what(one["road"_L1].toString(), QUrl(one["address"_L1].toString()),
                                         one["user"_L1].toString(), one["password"_L1].toString());
            QCOMPARE(pacing.times(mark), one["noes"_L1].toInt());
            QCOMPARE(pacing.silent(mark), one["silent"_L1].toInt());
            QCOMPARE(pacing.waitLeftMs(mark), one["left_ms"_L1].toInteger());
            QVERIFY(pacing.waitLeftMs(mark) > 0);
        }
    }

    // The alerts' history, a paused alert, and a server whose BIOS setup boot override may still be set.
    void theAlertsAndTheSetupOverridesStillHold_data() { fixtures(); }
    void theAlertsAndTheSetupOverridesStillHold()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        QDateTime now = fixture.now();
        const core::Incidents incidents(core::profile::incidentsFile(profile.settingsFile), [now] { return now; });
        QVERIFY(!incidents.damaged());
        const QJsonArray made = fixture.made["incidents"_L1].toArray();
        const QList<core::Incidents::Entry> entries = incidents.entries(u"pc"_s);
        QCOMPARE(entries.size(), made.size());
        QVERIFY(!entries.isEmpty());
        for (qsizetype index = 0; index < made.size(); ++index) {
            const core::Incidents::Entry& entry = entries.at(index);
            const QJsonValue was = made.at(index);
            QCOMPARE(entry.id, was["id"_L1].toString());
            QCOMPARE(entry.key, was["key"_L1].toString());
            QCOMPARE(entry.name, was["name"_L1].toString());
            QCOMPARE(entry.state, was["state"_L1].toString());
            QCOMPARE(entry.opened, QDateTime::fromString(was["opened"_L1].toString(), Qt::ISODateWithMs));
            QCOMPARE(entry.minutes, was["minutes"_L1].toInt());
            QCOMPARE(incidents.pausedUntil(u"pc"_s, entry.key),
                     QDateTime::fromString(was["paused_until"_L1].toString(), Qt::ISODateWithMs));
        }
        const core::SetupOverrides overrides(core::profile::setupOverridesFile(profile.settingsFile),
                                             [now] { return now; });
        const QJsonArray left = fixture.made["setup_overrides"_L1].toArray();
        QCOMPARE(overrides.all().size(), left.size());
        QVERIFY(!left.isEmpty());
        const QList<core::SetupOverrides::Left> all = overrides.all();
        for (qsizetype index = 0; index < left.size(); ++index) {
            const core::SetupOverrides::Left& one = all.at(index);
            QCOMPARE(one.address, left.at(index)["address"_L1].toString());
            QCOMPARE(one.machine, left.at(index)["machine"_L1].toString());
            QCOMPARE(one.since, QDateTime::fromString(left.at(index)["since"_L1].toString(), Qt::ISODateWithMs));
            QCOMPARE(one.doubtUntil,
                     QDateTime::fromString(left.at(index)["doubt_until"_L1].toString(), Qt::ISODateWithMs));
        }
    }

    // Every name the old version kept something under is one this build looks under: each secret's name in Windows'
    // Credential Manager, and each file of the profile.
    void everyNameIsOneTheProgramLooksUnder_data() { fixtures(false, true); }
    void everyNameIsOneTheProgramLooksUnder()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        // (Their Credential Manager names, as each version's own Windows code made them: test_ui_credential_shapes.)
        const QString settings = QDir(fixture.profileFolder()).filePath(u"settings.json"_s);
        const QStringList read{
            u"settings.json"_s,
            QFileInfo(core::profile::signInsFile(settings)).fileName(),
            QFileInfo(core::profile::lockFile(settings)).fileName(),
            QFileInfo(core::profile::incidentsFile(settings)).fileName(),
            QFileInfo(core::profile::setupOverridesFile(settings)).fileName(),
        };
        // (And the folders beside them: the screen watches' pictures, and the notes of an update.)
        const QString pictures = QFileInfo(core::profile::alertPicturesFolder(settings)).fileName() + u'/';
        const QStringList files = testing::profiles::filesUnder(fixture.profileFolder());
        QVERIFY(files.size() >= 2);
        for (const QString& file : files) {
            QVERIFY2(read.contains(file) || file.startsWith(pictures) || file.startsWith(u"updates/setup-"_s),
                     qPrintable(file + u" is no file the program reads"_s));
        }
    }

    // And the folders: the settings in %LOCALAPPDATA%\QKVM Desktop, by the program's internal name, and its cache in
    // %LOCALAPPDATA%\qKVM\Cache. (Only the names are made: nothing is read or written there.)
    void theProgramsFoldersAreWhereTheyWere()
    {
        ui::prepareApplication();
        const QString local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        QVERIFY(!local.isEmpty());
        QCOMPARE(core::Settings::usualFile(), QDir(local).filePath(u"QKVM Desktop/settings.json"_s));
        const QTemporaryDir base;
        const std::filesystem::path root = base.path().toStdWString();
        QVERIFY(storage::cachePath(root, {}, storage::Area::Downloads) == root / L"qKVM" / L"Cache" / L"Downloads");
        QVERIFY(storage::cachePath(root, {}, storage::Area::Diagnostics) == root / L"qKVM" / L"Cache" / L"Diagnostics");
        QCOMPARE(core::windowsTargetOf(u"kvm:0a1b2c3d"_s), u"QKVM Desktop: kvm:0a1b2c3d"_s);
    }

    // The first start of this version keeps the settings as the old one left them, byte for byte, and records the
    // version (from 0.1.2: SPEC.md section 11.2); the copy changes nothing of how the profile starts.
    void theFirstStartKeepsTheOldSettingsAsTheyWere_data() { fixtures(); }
    void theFirstStartKeepsTheOldSettingsAsTheyWere()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        const auto started = core::profile::recordStart(profile.settingsFile, u"0.1.2"_s);
        QVERIFY(started);
        QVERIFY(started.value().another);
        QCOMPARE(started.value().previous, QString());
        QCOMPARE(QFileInfo(started.value().keptAs).fileName(), u"settings.json.before-0.1.2.bak"_s);
        QFile kept(started.value().keptAs);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QFile old(QDir(fixture.profileFolder()).filePath(u"settings.json"_s));
        QVERIFY(old.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), old.readAll());
        QCOMPARE(core::profile::recorded(profile.settingsFile).value_or(core::profile::Record{}).version, u"0.1.2"_s);
        // A second start of it keeps nothing more.
        const auto again = core::profile::recordStart(profile.settingsFile, u"0.1.2"_s);
        QVERIFY(again && again.value().keptAs.isEmpty());
        // And the lock comes up as before, the copy beside the settings.
        core::SignInPacing pacing;
        core::PacingFile signIns(pacing, core::profile::signInsFile(profile.settingsFile), profile.secrets,
                                 paceOf(fixture));
        signIns.read();
        const std::unique_ptr<ui::AppLock> lock = lockOf(profile, fixture);
        QCOMPARE(lock->mode(), QString());
        QVERIFY(!lock->restricted());
    }

    // The screen watch's alert, with the picture that set it off: told again, and its picture there to be shown.
    void theAlertsPictureIsStillThere_data() { fixtures(); }
    void theAlertsPictureIsStillThere()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        QVERIFY(profile.copied);
        const QJsonObject alert = fixture.made["alert"_L1].toObject();
        QVERIFY(!alert.isEmpty());
        core::ScreenWatch::Times times;
        times.wallClock = [when = fixture.now()] { return when; };
        core::ScreenWatch watch(times);
        bool read = false;
        connect(&watch, &core::ScreenWatch::readBack, this, [&read] { read = true; });
        watch.keepIn(core::profile::alertPicturesFolder(profile.settingsFile));
        QTRY_VERIFY(read);
        const QString machine = alert["machine"_L1].toString();
        const QList<core::ScreenWatch::Event> told = watch.told(machine);
        QVERIFY(!told.isEmpty());
        QCOMPARE(told.front().watch, alert["watch"_L1].toString());
        QCOMPARE(told.front().stamp, alert["stamp"_L1].toString());
        std::optional<QByteArray> picture;
        bool answered = false;
        watch.askPicture(machine, alert["stamp"_L1].toString(), [&](std::optional<QByteArray> bytes) {
            picture = std::move(bytes);
            answered = true;
        });
        QTRY_VERIFY(answered);
        QCOMPARE(picture.value_or(QByteArray()), QByteArray::fromBase64(alert["picture"_L1].toString().toLatin1()));
    }

    // An update handed over to Setup before the upgrade: the next start says what came of it, from the note and
    // Setup's log as the old version left them.
    void theHandOverToSetupIsStillTold_data() { fixtures(); }
    void theHandOverToSetupIsStillTold()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        const QJsonObject update = fixture.made["update"_L1].toObject();
        QVERIFY(update["setup_log"_L1].toObject()["text"_L1].toString().contains(u"qKVM-RESULT: 105"_s));
        QNetworkAccessManager network;
        const auto channel = [](const Profile& profile, const QString& version) {
            core::UpdateChannel made;
            made.currentVersion = version;
            made.kind = core::UpdateKind::Setup;
            made.dataFolder = QFileInfo(profile.settingsFile).absolutePath();
            return made;
        };
        {
            // Installed: the version the note names is the one that starts.
            Profile profile(fixture);
            core::Settings settings(profile.settingsFile);
            const core::Updates after(channel(profile, update["to"_L1].toString()), settings, network,
                                      profile.home.filePath(u"downloads"_s));
            QCOMPARE(after.afterSetup(), u"Updated to qKVM %1."_s.arg(update["to"_L1].toString()));
            QVERIFY(!after.afterSetupWentWrong());
            QVERIFY(!QFile::exists(QFileInfo(profile.settingsFile).absolutePath() + u"/updates/setup-handover.json"_s));
        }
        {
            // Not installed: Setup's log says why.
            Profile profile(fixture);
            core::Settings settings(profile.settingsFile);
            const core::Updates after(channel(profile, update["from"_L1].toString()), settings, network,
                                      profile.home.filePath(u"downloads"_s));
            QVERIFY2(after.afterSetup().contains(u"qKVM was still running"_s), qPrintable(after.afterSetup()));
            QVERIFY(after.afterSetupWentWrong());
        }
    }

    // A wall screen (a PC nobody sits at, signed in to a server's wall by a display link), locked: after the upgrade
    // it asks for the app password, never for a reset, and is a wall screen still.
    void aLockedWallScreenAsksForItsPasswordAndIsAWallScreenStill_data() { fixtures(true); }
    void aLockedWallScreenAsksForItsPasswordAndIsAWallScreenStill()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        QVERIFY(fixture.made["locked"_L1].toBool());
        Profile profile(fixture);
        QVERIFY(profile.copied);
        // (Its lock left its second record beside the settings.)
        QVERIFY(QFile::exists(core::profile::lockFile(profile.settingsFile)));
        core::SignInPacing pacing;
        core::PacingFile signIns(pacing, core::profile::signInsFile(profile.settingsFile), profile.secrets,
                                 paceOf(fixture));
        signIns.read();
        const std::unique_ptr<ui::AppLock> lock = lockOf(profile, fixture);
        QCOMPARE(lock->mode(), u"unlock"_s);
        QVERIFY(lock->locked());
        QVERIFY(lock->wallScreen());
        QVERIFY(lock->canStartSources());
        QCOMPARE(lock->reason(), QString());
        QCOMPARE(lock->problem(), QString());
        lock->submit(fixture.made["app_password"_L1].toString(), {}, {});
        QTRY_VERIFY_WITH_TIMEOUT(!lock->busy(), 120'000);
        QVERIFY2(!lock->locked(), qPrintable(lock->problem()));
        QVERIFY(lock->wallScreen());
        const ui::WallScreen screen(profile.secrets);
        QVERIFY(screen.isOne());
    }

    // And its display link still signs its server's wall in: the program goes back to that wall by the link it kept.
    void aWallScreensDisplayLinkStillSignsItsWallIn_data() { fixtures(true); }
    void aWallScreensDisplayLinkStillSignsItsWallIn()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        core::Settings settings(profile.settingsFile);
        const QJsonObject server = fixture.server();
        const QByteArray key = server["display_key"_L1].toString().toLatin1();
        const QByteArray session = server["session"_L1].toString().toLatin1();
        int handedIn = 0;
        const StandIn qkvm(
            [&](const Asked& asked) -> Answered {
                const bool signedIn = asked.headers.value("cookie").contains("qkvm_session=" + session);
                if (asked.path == "/healthz") {
                    return {
                        200,
                        R"({"status": "ok", "version": "0.1.0", "api": ["v1"], "page": "3f2a9c", "locked": false})"};
                }
                if (asked.path == "/api/v1/session") {
                    return {200, signedIn
                                     ? QByteArray(R"({"initialized": true, "authenticated": true, "ask_name": false,
                        "user": {"id": "token:t1", "name": "Lobby screen", "role": "admin", "token": true}})")
                                     : QByteArray(R"({"initialized": true, "authenticated": false,
                        "ask_name": false, "user": null})")};
                }
                if (asked.path == "/api/v1/display" && asked.body.contains(key)) {
                    ++handedIn;
                    return {
                        200,
                        R"({"ok": true, "user": {"id": "token:t1", "name": "Lobby screen", "role": "admin", "token": true}})",
                        "application/json",
                        {"Set-Cookie: qkvm_session=" + session + "; HttpOnly; Path=/; SameSite=Strict"}};
                }
                return aQkvm(asked, signedIn);
            },
            static_cast<quint16>(server["port"_L1].toInt()));
        QVERIFY2(qkvm.isListening(), qPrintable(u"port %1 of this PC is taken"_s.arg(server["port"_L1].toInt())));
        ui::WallScreen screen(profile.secrets);
        ui::ServerSource source(u"e5f6a7b8"_s, settings, profile.secrets, 200);
        source.markScreensIn(screen);
        QCOMPARE(settings.remembered().server, server["address"_L1].toString());
        source.connectTo(settings.remembered().server);
        QTRY_VERIFY_WITH_TIMEOUT(source.isAtTheWall(), 30'000);
        QCOMPARE(handedIn, 1);
        QCOMPARE(qkvm.count("/api/v1/login"), 0);
        QVERIFY(screen.isOne());
        source.feed().stop();
    }

    // The app password itself still opens the lock, once it is locked.
    void theAppPasswordStillUnlocks_data() { fixtures(); }
    void theAppPasswordStillUnlocks()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        const std::unique_ptr<ui::AppLock> lock = lockOf(profile, fixture);
        QVERIFY(!lock->restricted());
        lock->lock();
        QCOMPARE(lock->mode(), u"unlock"_s);
        lock->submit(fixture.made["app_password"_L1].toString(), {}, {});
        QTRY_VERIFY_WITH_TIMEOUT(!lock->busy(), 120'000);
        QVERIFY2(!lock->restricted(), qPrintable(lock->problem()));
        QVERIFY(lock->automatic());
    }
};

QTEST_MAIN(TestUpgradeContinuity)
#include "test_upgrade_continuity.moc"
