#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVersionNumber>

#include <qkvm/core/incidents.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/local_wall.h>
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
const QStringList published{u"0.1.0"_s, u"0.1.1"_s, u"0.1.2"_s};
const QString wallScreen = u"-wall-screen"_s;

QJsonObject jsonIn(const QString& file)
{
    QFile kept(file);
    if (!kept.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(kept.readAll()).object();
}

// A version after every published one, as the version that upgrades them starts (this build's own number may still be
// the last published one's until it is released), and after every version that made a fixture here: a release's own
// fixture, made with this build's maker, is there before its version is in `published` (the merged release review,
// part C, M1: a first start of "0.2.0" over the 0.2.0 fixture was no start of another version).
QString aLaterVersion()
{
    QVersionNumber last;
    for (const QString& one : published) {
        last = std::max(last, QVersionNumber::fromString(one));
    }
    const QDir all(QStringLiteral(QKVM_PROFILE_FIXTURES));
    for (const QString& name : all.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QJsonObject made = jsonIn(all.filePath(name + u"/made.json"_s));
        last = std::max(last, QVersionNumber::fromString(made["made_by"_L1]["version"_L1].toString()));
    }
    return QVersionNumber(last.majorVersion(), last.minorVersion() + 1, 0).toString();
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

// 0.1.2's own reading of a KVM's password, copied from its source (core/src/local_wall.cpp at 5b6c0956: savedFor,
// 242 to 255, and passwordIn, 291 to 311): the three lines it saves a password for, and the password only where the
// secret begins with them. A record of two (from 0.2.0 on) begins otherwise: it reads none of it.
QByteArray linesBy012(const core::OwnKvm& kvm)
{
    QByteArray lines = "QKVM-KVM-2\n" + kvm.address.toUtf8().toPercentEncoding(":/[]") + '\n';
    lines += kvm.knownByCertificate ? "sha256:" + kvm.certificate.toHex() : QByteArray("none");
    return lines + '\n';
}

QString passwordBy012(const QByteArray& secret, const core::OwnKvm& kvm)
{
    const QByteArray lines = linesBy012(kvm);
    return secret.startsWith(lines) ? QString::fromUtf8(secret.sliced(lines.size())) : QString();
}

// What 0.1.2 does with a yes to another certificate of a KVM over HTTPS that no other KVM of its wall is known by,
// copied from its source (knowBy and alter, 1545 to 1732): the kept password goes along where it reads one for the
// KVM as it was, saved anew for that certificate, and stays marked; where it reads none, nothing is written to the
// store, and the mark that a password is kept goes. The KVM is not asked what it is from then on, and the certificate
// kept beside it goes. (Nothing it does erases a record but the removal of its KVM, a change that is taken back, and
// "Reset saved secrets".) The settings as this build writes them, with what 0.1.2 writes of the KVM.
void aYesBy012(const QString& settingsFile, core::SecretStore& secrets, const QString& id, const QByteArray& to)
{
    core::Settings settings(settingsFile);
    core::Remembered with = settings.remembered();
    const auto kvm = std::ranges::find_if(with.kvms, [&id](const core::OwnKvm& one) { return one.id == id; });
    QVERIFY(kvm != with.kvms.end());
    core::OwnKvm moved = *kvm;
    moved.knownByCertificate = true;
    moved.certificate = to;
    moved.askedWhatItIs = false;
    const QString name = core::LocalWall::secretOf(id);
    const QString kept = passwordBy012(secrets.kept(name), *kvm);
    if (kept.isEmpty()) {
        with.kvmPasswordsKept.remove(id);
    } else {
        QVERIFY(secrets.keep(name, linesBy012(moved) + kept.toUtf8()).ok());
        with.kvmPasswordsKept.insert(id);
    }
    with.kvmCertificates.remove(id);
    *kvm = moved;
    QVERIFY(static_cast<bool>(settings.remember(with)));
}

// A start of this build over the profile: the password of the KVM `id` as the file of the wall takes it, and whether
// the wall marks one kept for it.
struct StartedWith {
    QString password;
    bool marked = false;
};

StartedWith aStartOf(const QString& settingsFile, core::SecretStore& secrets, const QString& id)
{
    core::Settings settings(settingsFile);
    core::testing::WallOfATest wall(settings, secrets, core::KvmdDevice::Pace{});
    const core::LocalWall::AsAFile file = wall.asAFile(true);
    const QList<core::exchange::Device> devices = file.content.devices.value_or(QList<core::exchange::Device>());
    const auto kvm = std::ranges::find_if(wall.kvms(), [&id](const core::OwnKvm& one) { return one.id == id; });
    StartedWith started;
    if (kvm != wall.kvms().end()) {
        const auto device = std::ranges::find_if(
            devices, [&kvm](const core::exchange::Device& one) { return one.address == kvm->address; });
        started.password = device != devices.end() ? device->password : QString();
    }
    started.marked = wall.marksAPasswordFor(id);
    return started;
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

    // The rows: every fixture whose KVM a cut change left with a record of two (made.json's `cut_change`, from 0.2.0
    // on), and one that fails where none is there.
    static void fixturesWithACut()
    {
        QTest::addColumn<QString>("folder");
        const QDir all(QStringLiteral(QKVM_PROFILE_FIXTURES));
        bool any = false;
        for (const QString& name : all.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (Fixture(all.filePath(name)).made["cut_change"_L1].isObject()) {
                QTest::newRow(qPrintable(name)) << all.filePath(name);
                any = true;
            }
        }
        if (!any) {
            QTest::newRow("none holds a record of two") << QString();
        }
    }

    // Where the fixture's card of that id keeps its server's computer, as made.json says its maker noted it: none where
    // it says none, or nothing (every version before 0.2.0).
    static core::ServerPlace placeMadeFor(const Fixture& fixture, const QString& id)
    {
        for (const QJsonValue one : fixture.made["kvms"_L1].toArray()) {
            if (one["id"_L1].toString() == id) {
                const QJsonObject place = one["server_place"_L1].toObject();
                return place.isEmpty()
                           ? core::ServerPlace{}
                           : core::ServerPlace{place["place"_L1].toString(), place["among_several"_L1].toBool()};
            }
        }
        return {};
    }

    // The KVM of the row's cut change, as made.json has it.
    static QJsonObject cutKvm(const Fixture& fixture)
    {
        const QString id = fixture.made["cut_change"_L1]["id"_L1].toString();
        for (const QJsonValue one : fixture.made["kvms"_L1].toArray()) {
            if (one["id"_L1].toString() == id) {
                return one.toObject();
            }
        }
        return {};
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
        // A KVM of a family this build does not speak to, as a newer build left it: kept as it stands. (Of a kind no
        // version speaks to: "vnc" stood here, which this build reads from 0.2.0 on, and the test no longer tried what
        // it is for. An entry of a family this build reads that it cannot read is kept too: test_core_settings.)
        QVERIFY2(!core::OwnKvm::namesAFamily(fixture.made["kvm_of_another_family"_L1]["kind"_L1].toString()),
                 "the fixture's KVM of another family is of a family this build speaks to");
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
            // ...and where a server's controller keeps its computer (from 0.2.0's fixture on; none before it, and a
            // card of before keeps none: outside review of 0.2.0's Redfish fixes, M1).
            const QJsonValue at = made["server_place"_L1];
            QVERIFY2(kvm.serverPlace == (core::ServerPlace{at["place"_L1].toString(), at["among_several"_L1].toBool()}),
                     qPrintable(kvm.name));
            // A Proxmox guest's node and number (from 0.2.0's fixture on; none before it).
            QCOMPARE(kvm.node, made["node"_L1].toString());
            QCOMPARE(kvm.vmid, made["vmid"_L1].toInteger());
            // ...and the boot order its driver replaced and had not put back yet (from 0.2.0's fixture on).
            const QJsonValue was = made["boot_order_was"_L1];
            QCOMPARE(kvm.bootOrderWas.has_value(), was.isObject());
            if (kvm.bootOrderWas) {
                QCOMPARE(kvm.bootOrderWas->order, was["order"_L1].toString());
                QCOMPARE(kvm.bootOrderWas->written, was["written"_L1].toString());
                QCOMPARE(kvm.bootOrderWas->vmid, was["vmid"_L1].toInteger());
                QCOMPARE(kvm.bootOrderWas->machine, was["machine"_L1].toString());
            }
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
        // (Written by the version that upgrades, as the program names itself.)
        QCoreApplication::setApplicationVersion(aLaterVersion());
        const auto unnamed = qScopeGuard([] { QCoreApplication::setApplicationVersion({}); });
        core::Settings written(again);
        QVERIFY(written.remember(read.remembered()));
        // Which version wrote them is the one thing that is to change (from 0.1.2 on: "written_by"), and it names the
        // version that wrote them now; the copy of them as they were found keeps the old name (SPEC.md section 11.2).
        const QJsonObject now = jsonIn(again);
        QCOMPARE(now["written_by"_L1].toString(), aLaterVersion());
        QJsonObject old = fixture.settings;
        old.remove(u"written_by"_s);
        QStringList lost;
        lostFrom(old, now, {}, lost);
        QVERIFY2(lost.isEmpty(), qPrintable(lost.join(u"; "_s)));
    }

    // Where a server's controller keeps its computer (OwnKvm::serverPlace, from 0.2.0: outside review of 0.2.0's
    // Redfish fixes, M1), read as the fixture keeps it (none before 0.2.0, the maker's own place from 0.2.0's fixture
    // on: the merged review of 0.2.0, C-M1), then noted elsewhere beside the server card as this build's wall notes a
    // place its driver found: read as it was, and written again as it was. The way back to 0.1.2, whose written() names
    // the keys it writes of a card (copied from its source at 5b6c0956: id, kind, name, address, user, group,
    // known_by_certificate, certificate, asked, number, machine and the two screen watches), drops it and nothing else:
    // up again, the card is as one from before (its place learnt anew at its computer's next number), its mark and the
    // rest as they were.
    void whereAServersComputerIsIsKeptAndTheWayBackTo012DropsItAlone_data() { fixtures(); }
    void whereAServersComputerIsIsKeptAndTheWayBackTo012DropsItAlone()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        const core::ServerPlace elsewhere{u"/redfish/v1/Systems/1"_s, false};
        QList<core::OwnKvm> noted;
        {
            core::Settings settings(profile.settingsFile);
            core::Remembered with = settings.remembered();
            const auto server =
                std::ranges::find_if(with.kvms, [](const core::OwnKvm& one) { return one.kind == "redfish"_L1; });
            QVERIFY2(server != with.kvms.end(), "the fixture has no server's controller");
            const core::ServerPlace made = placeMadeFor(fixture, server->id);
            QVERIFY2(
                server->serverPlace == made,
                qPrintable(
                    u"%1 (among several: %2)"_s.arg(server->serverPlace.place).arg(server->serverPlace.amongSeveral)));
            QVERIFY(made != elsewhere);
            server->serverPlace = elsewhere;
            QVERIFY(settings.remember(with).ok());
            noted = with.kvms;
        }
        QVERIFY(core::Settings(profile.settingsFile).remembered().kvms == noted);
        QJsonObject file = jsonIn(profile.settingsFile);
        QJsonArray kvms = file["kvms"_L1].toArray();
        int dropped = 0;
        for (qsizetype index = 0; index < kvms.size(); ++index) {
            QJsonObject one = kvms.at(index).toObject();
            if (one.contains("server_place"_L1)) {
                QCOMPARE(one["kind"_L1].toString(), u"redfish"_s);
                QCOMPARE(one["server_place"_L1].toObject(),
                         (QJsonObject{{u"place"_s, elsewhere.place}, {u"among_several"_s, elsewhere.amongSeveral}}));
                one.remove(u"server_place"_s);
                ++dropped;
            }
            kvms[index] = one;
        }
        QCOMPARE(dropped, 1);
        file.insert(u"kvms"_s, kvms);
        QFile back(profile.settingsFile);
        QVERIFY(back.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(back.write(QJsonDocument(file).toJson()) > 0);
        back.close();
        const QList<core::OwnKvm> up = core::Settings(profile.settingsFile).remembered().kvms;
        QCOMPARE(up.size(), noted.size());
        for (qsizetype index = 0; index < up.size(); ++index) {
            core::OwnKvm asBefore = noted.at(index);
            asBefore.serverPlace = {};
            QVERIFY2(up.at(index) == asBefore, qPrintable(asBefore.name));
        }
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

    // A KVM whose change a cut left with a record of two (QKVM-KVM-2+, from 0.2.0 on: SPEC.md section 11.4) opens with
    // its password, and the start makes the record one: the one for the KVM as the settings have it, alone; nothing
    // else of the store changes, and the next start keeps nothing (review cx6-1, M2).
    void aRecordOfTwoACutChangeLeftOpensWithItsPasswordAndIsMadeOne_data() { fixturesWithACut(); }
    void aRecordOfTwoACutChangeLeftOpensWithItsPasswordAndIsMadeOne()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "no fixture holds a record of two that a cut change left");
        Profile profile(fixture);
        const QJsonObject made = cutKvm(fixture);
        QVERIFY(!made.isEmpty());
        const QString id = made["id"_L1].toString();
        const QString name = core::LocalWall::secretOf(id);
        const QByteArray password = made["password"_L1].toString().toUtf8();
        const QString address = made["address"_L1].toString();
        // As the fixture has it: the record for the KVM as it was, and the one for it as the settings have it.
        const QJsonObject cut = fixture.made["cut_change"_L1].toObject();
        const QByteArray wasKnownBy = QByteArray::fromHex(cut["certificate_before"_L1].toString().toLatin1());
        const QByteArray isKnownBy = QByteArray::fromHex(made["certificate"_L1].toString().toLatin1());
        const QByteArray before = core::testing::savedFor(password, address, wasKnownBy);
        const QByteArray now = core::testing::savedFor(password, address, isKnownBy);
        QCOMPARE(profile.secrets.all.value(name),
                 "QKVM-KVM-2+\n" + QByteArray::number(before.size()) + '\n' + before + now);
        QHash<QString, QByteArray> others = profile.secrets.all;
        others.remove(name);
        const StartedWith started = aStartOf(profile.settingsFile, profile.secrets, id);
        QCOMPARE(started.password, made["password"_L1].toString());
        QVERIFY(started.marked);
        QCOMPARE(profile.secrets.all.value(name), now);
        QHash<QString, QByteArray> othersNow = profile.secrets.all;
        othersNow.remove(name);
        QCOMPARE(othersNow, others);
        const int keeps = profile.secrets.keeps;
        QCOMPARE(aStartOf(profile.settingsFile, profile.secrets, id).password, made["password"_L1].toString());
        QCOMPARE(profile.secrets.keeps, keeps);
    }

    // The way back to 0.1.2 and up again (review cx6-1, M2), by 0.1.2's own rules copied from its source (aYesBy012):
    // 0.1.2 reads a record of two as no password, and erases nothing of it. Up again with nothing done to that KVM
    // there, this build has its password. Where this build started once between the cut and the way back, the record
    // was made one: 0.1.2 reads it, and a yes there to a renewed certificate takes it along, as ever. Where 0.1.2 came
    // straight after the cut and a yes was said there, 0.1.2 took the KVM on without its password and dropped its mark:
    // up again, the KVM has none, and asks for it (the record stays, used for nothing). That one is lost: SPEC.md
    // section 11.4 and the changelog's known limits say so.
    void theWayBackTo012AndUpAgainKeepsThePasswordWhereThisBuildStartedBetween_data() { fixturesWithACut(); }
    void theWayBackTo012AndUpAgainKeepsThePasswordWhereThisBuildStartedBetween()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "no fixture holds a record of two that a cut change left");
        const QJsonObject made = cutKvm(fixture);
        QVERIFY(!made.isEmpty());
        const QString id = made["id"_L1].toString();
        const QString name = core::LocalWall::secretOf(id);
        const QString password = made["password"_L1].toString();
        core::OwnKvm asMade;
        asMade.address = made["address"_L1].toString();
        asMade.knownByCertificate = true;
        asMade.certificate = QByteArray::fromHex(made["certificate"_L1].toString().toLatin1());
        const QByteArray renewed = QCryptographicHash::hash("renewed on the way back", QCryptographicHash::Sha256);
        {
            // Nothing done to it there: up again, it has its password.
            Profile profile(fixture);
            QCOMPARE(passwordBy012(profile.secrets.all.value(name), asMade), QString());
            const StartedWith up = aStartOf(profile.settingsFile, profile.secrets, id);
            QCOMPARE(up.password, password);
            QVERIFY(up.marked);
        }
        {
            // This build started once between: 0.1.2 reads it, takes it along to a renewed certificate, and up again
            // it is there.
            Profile profile(fixture);
            QCOMPARE(aStartOf(profile.settingsFile, profile.secrets, id).password, password);
            QCOMPARE(passwordBy012(profile.secrets.all.value(name), asMade), password);
            aYesBy012(profile.settingsFile, profile.secrets, id, renewed);
            if (QTest::currentTestFailed()) {
                return;
            }
            const StartedWith up = aStartOf(profile.settingsFile, profile.secrets, id);
            QCOMPARE(up.password, password);
            QVERIFY(up.marked);
        }
        {
            // 0.1.2 straight after the cut, and a yes there: up again, it has none, and its record stays as it was.
            Profile profile(fixture);
            const QByteArray two = profile.secrets.all.value(name);
            aYesBy012(profile.settingsFile, profile.secrets, id, renewed);
            if (QTest::currentTestFailed()) {
                return;
            }
            QCOMPARE(profile.secrets.all.value(name), two);
            const StartedWith up = aStartOf(profile.settingsFile, profile.secrets, id);
            QCOMPARE(up.password, QString());
            QVERIFY(!up.marked);
            QCOMPARE(profile.secrets.all.value(name), two);
        }
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
            // (From 0.1.2 on, a record the controller only queued: no look lets it go, however late.)
            QCOMPARE(one.queued, left.at(index)["queued"_L1].toBool());
            QDateTime dayAfter = now.addDays(1);
            core::SetupOverrides looked(core::profile::setupOverridesFile(profile.settingsFile),
                                        [dayAfter] { return dayAfter; });
            QCOMPARE(looked.seenOff(QUrl(one.address), one.machine, dayAfter), !one.queued);
            QCOMPARE(looked.standing(QUrl(one.address), one.machine).has_value(), one.queued);
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
            // (From 0.1.2 on, the record of the version that started with it.)
            QFileInfo(core::profile::versionFile(settings)).fileName(),
            // (And from 0.2.0 on, the file whose lock says which copy writes it: it holds nothing.)
            QFileInfo(core::profile::runningFile(settings)).fileName(),
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

    // The first start of a later version keeps the settings as the old one left them, byte for byte, named by the
    // version that wrote them where they say one (from 0.1.2 on), else as found before this one; and records the
    // version, with the one recorded before it (SPEC.md section 11.2). The copy changes nothing of how the profile
    // starts.
    void theFirstStartKeepsTheOldSettingsAsTheyWere_data() { fixtures(false, true); }
    void theFirstStartKeepsTheOldSettingsAsTheyWere()
    {
        const Fixture fixture = TestUpgradeContinuity::fixture();
        QVERIFY2(fixture.whole(), "the fixture of a published version is missing or incomplete");
        Profile profile(fixture);
        const QString later = aLaterVersion();
        const QString writer = fixture.settings["written_by"_L1].toString();
        const std::optional<core::profile::Record> before =
            core::profile::recorded(QDir(fixture.profileFolder()).filePath(u"settings.json"_s));
        // (What each version kept: 0.1.2 says it wrote them, and recorded its start; one before it, neither.)
        const QString version = fixture.made["made_by"_L1]["version"_L1].toString();
        const bool recordsItself = QVersionNumber::fromString(version) >= QVersionNumber(0, 1, 2);
        QCOMPARE(writer, recordsItself ? version : QString());
        QCOMPARE(before.has_value(), recordsItself);
        QCOMPARE(before.value_or(core::profile::Record{}).version, recordsItself ? version : QString());
        const auto started = core::profile::recordStart(profile.settingsFile, later);
        QVERIFY(started);
        QVERIFY(started.value().another);
        QCOMPARE(started.value().previous, before.value_or(core::profile::Record{}).version);
        QCOMPARE(QFileInfo(started.value().keptAs).fileName(),
                 u"settings.json.%1.bak"_s.arg(writer.isEmpty() ? u"before-"_s + later : writer));
        QFile kept(started.value().keptAs);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QFile old(QDir(fixture.profileFolder()).filePath(u"settings.json"_s));
        QVERIFY(old.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), old.readAll());
        const core::profile::Record now =
            core::profile::recorded(profile.settingsFile).value_or(core::profile::Record{});
        QCOMPARE(now.version, later);
        QCOMPARE(now.previous, before.value_or(core::profile::Record{}).version);
        // A second start of it keeps nothing more.
        const auto again = core::profile::recordStart(profile.settingsFile, later);
        QVERIFY(again && again.value().keptAs.isEmpty());
        // And the lock comes up as before, the copy beside the settings: open, or asking for its password where it was
        // left locked (a wall screen), never for a reset.
        core::SignInPacing pacing;
        core::PacingFile signIns(pacing, core::profile::signInsFile(profile.settingsFile), profile.secrets,
                                 paceOf(fixture));
        signIns.read();
        const std::unique_ptr<ui::AppLock> lock = lockOf(profile, fixture);
        const bool locked = fixture.made["locked"_L1].toBool();
        QCOMPARE(lock->mode(), locked ? u"unlock"_s : QString());
        QCOMPARE(lock->locked(), locked);
        QVERIFY(locked || !lock->restricted());
        QCOMPARE(lock->problem(), QString());
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
