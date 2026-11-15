#include <algorithm>
#include <functional>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/profile.h>

#include "disposable_machine.h"
#include "memory_secrets.h"
#include "profile_loading.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

// The VM step's own tool (profile_loading.h, qkvm-test-load-profile): a published version's profile put in place byte
// for byte, and every loss an upgrade could make of it seen. Into a store in memory and a scratch folder: the tool's
// own program is started only to be seen to refuse, with a scratch home, and is never let reach the Credential
// Manager of whoever runs this.
namespace {

const QString version = u"0.1.2"_s;

QString fixtureFolder()
{
    return QStringLiteral(QKVM_PROFILE_FIXTURES) + u"/0.1.1"_s;
}

bool write(const QString& file, const QByteArray& bytes)
{
    QFile into(file);
    return into.open(QIODevice::WriteOnly) && into.write(bytes) == bytes.size();
}

// A fixture loaded, and a first start of this version over it as the program makes one (its record and its copy).
struct Loaded {
    Loaded() : fixture(fixtureFolder())
    {
        folder = home.filePath(u"QKVM Desktop"_s);
        settings = QDir(folder).filePath(u"settings.json"_s);
        wrong = testing::profiles::load(fixture, store, folder);
        started = static_cast<bool>(core::profile::recordStart(settings, version));
    }
    QJsonObject check() { return testing::profiles::check(fixture, store, folder, version); }

    testing::profiles::Fixture fixture;
    QTemporaryDir home;
    core::testing::MemorySecrets store;
    QString folder;
    QString settings;
    QStringList wrong;
    bool started = false;
};

QString failuresOf(const QJsonObject& report)
{
    QStringList all;
    for (const QJsonValue one : report["failures"_L1].toArray()) {
        all.append(one.toString());
    }
    return all.join(u"; "_s);
}

} // namespace

class TestProfileLoading : public QObject {
    Q_OBJECT

private slots:
    void aFixtureIsPutInPlaceByteForByteAndAnUpgradeThatKeptItPasses()
    {
        Loaded loaded;
        QVERIFY(loaded.fixture.whole());
        QVERIFY2(loaded.wrong.isEmpty(), qPrintable(loaded.wrong.join(u"; "_s)));
        QVERIFY(loaded.started);
        QCOMPARE(loaded.store.all.size(), loaded.fixture.secrets.size());
        for (auto one = loaded.fixture.secrets.begin(); one != loaded.fixture.secrets.end(); ++one) {
            QCOMPARE(loaded.store.all.value(one.key()), loaded.fixture.secret(one.key()));
        }
        // (Every file, in the profile's folders too.)
        QVERIFY(loaded.fixture.files().contains(u"updates/setup-handover.json"_s));
        QVERIFY(std::ranges::any_of(loaded.fixture.files(),
                                    [](const QString& file) { return file.startsWith(u"alert-pictures/"_s); }));
        for (const QString& file : loaded.fixture.files()) {
            QCOMPARE(testing::profiles::bytesOf(QDir(loaded.folder).filePath(file)),
                     testing::profiles::bytesOf(QDir(loaded.fixture.profileFolder()).filePath(file)));
        }
        const QJsonObject report = loaded.check();
        QVERIFY2(report["passed"_L1].toBool(), qPrintable(failuresOf(report)));
        QVERIFY(report["kept_whole"_L1].toBool());
        QCOMPARE(report["recorded"_L1]["version"_L1].toString(), version);
        QVERIFY(report["windows_moved"_L1].toArray().isEmpty());
        // (Nothing of a secret is in what it says: the passwords, the session, the text, the key. The setup marker is
        // the one byte "1", which is no secret, and is in any count.)
        const QByteArray said = QJsonDocument(report).toJson();
        for (auto one = loaded.fixture.secrets.begin(); one != loaded.fixture.secrets.end(); ++one) {
            if (loaded.fixture.secret(one.key()).size() > 1) {
                QVERIFY2(!said.contains(loaded.fixture.secret(one.key())), qPrintable(one.key()));
            }
        }
        for (const QJsonValue kvm : loaded.fixture.made["kvms"_L1].toArray()) {
            QVERIFY(!said.contains(kvm["password"_L1].toString().toUtf8()));
        }
        QVERIFY(!said.contains(loaded.fixture.made["server"_L1]["session"_L1].toString().toUtf8()));
        QVERIFY(!said.contains(loaded.fixture.made["app_password"_L1].toString().toUtf8()));
    }

    void itLoadsOnlyWhereTheProgramKeepsNothingYet()
    {
        const testing::profiles::Fixture fixture(fixtureFolder());
        QTemporaryDir home;
        core::testing::MemorySecrets store;
        QVERIFY(store.keep(u"sign-in-pacing"_s, QByteArray(32, 'k')));
        QVERIFY(!testing::profiles::load(fixture, store, home.filePath(u"one"_s)).isEmpty());
        QCOMPARE(store.all.size(), 1);
        QVERIFY(!QDir(home.filePath(u"one"_s)).exists());
        core::testing::MemorySecrets empty;
        QVERIFY(QDir().mkpath(home.filePath(u"two"_s)));
        QVERIFY(write(home.filePath(u"two/settings.json"_s), "{}"));
        QVERIFY(!testing::profiles::load(fixture, empty, home.filePath(u"two"_s)).isEmpty());
        QVERIFY(empty.all.isEmpty());
        QVERIFY(!testing::profiles::load(testing::profiles::Fixture(home.filePath(u"none"_s)), empty,
                                         home.filePath(u"three"_s))
                     .isEmpty());
    }

    // Each loss an upgrade could make, alone: the check fails, and says which.
    void everyLossIsSeen_data()
    {
        QTest::addColumn<int>("loss");
        QTest::addColumn<QString>("said");
        QTest::newRow("a KVM's password changed") << 0 << u"is changed"_s;
        QTest::newRow("the app password's record gone") << 1 << u"app password is missing"_s;
        QTest::newRow("a settings key gone") << 2 << u"settings.json lost theme"_s;
        QTest::newRow("a settings value changed") << 3 << u"settings.json lost kvms[1].group"_s;
        QTest::newRow("the copy of the settings gone") << 4 << u"were not kept"_s;
        QTest::newRow("the copy of the settings not whole") << 5 << u"were not kept"_s;
        QTest::newRow("the version not recorded") << 6 << u"not recorded"_s;
        QTest::newRow("the lock's marker left") << 7 << u"left its marker"_s;
        QTest::newRow("the sign-ins' counts sealed under another key") << 8 << u"do not open"_s;
        QTest::newRow("a refusal forgotten") << 9 << u"do not hold"_s;
        QTest::newRow("the alerts gone") << 10 << u"alert"_s;
        QTest::newRow("the setup override gone") << 11 << u"setup override"_s;
        QTest::newRow("a file gone") << 12 << u"settings.json.setup-overrides is gone"_s;
    }
    void everyLossIsSeen()
    {
        QFETCH(int, loss);
        QFETCH(QString, said);
        Loaded loaded;
        QVERIFY(loaded.wrong.isEmpty() && loaded.started);
        const QString settings = loaded.settings;
        const auto rewritten = [&](const std::function<void(QJsonObject&)>& change) {
            QJsonObject now = testing::profiles::jsonIn(settings);
            change(now);
            return write(settings, QJsonDocument(now).toJson());
        };
        const QString lab = loaded.fixture.made["kvms"_L1][1]["id"_L1].toString();
        switch (loss) {
        case 0:
            loaded.store.all[u"kvm:"_s + lab] = "QKVM-KVM-2\nhttp://192.0.2.11:8080\nnone\nanother";
            break;
        case 1:
            loaded.store.all.remove(u"app password"_s);
            break;
        case 2:
            QVERIFY(rewritten([](QJsonObject& now) { now.remove(u"theme"_s); }));
            break;
        case 3:
            QVERIFY(rewritten([](QJsonObject& now) {
                QJsonArray kvms = now[u"kvms"_s].toArray();
                QJsonObject one = kvms.at(1).toObject();
                one[u"group"_s] = u"Elsewhere"_s;
                kvms.replace(1, one);
                now[u"kvms"_s] = kvms;
            }));
            break;
        case 4:
            QVERIFY(QFile::remove(settings + u".before-"_s + version + u".bak"_s));
            break;
        case 5:
            QVERIFY(write(settings + u".before-"_s + version + u".bak"_s, "{}"));
            break;
        case 6:
            QVERIFY(QFile::remove(core::profile::versionFile(settings)));
            break;
        case 7:
            QVERIFY(write(core::profile::lockFile(settings), "locked\n"));
            break;
        case 8:
            loaded.store.all[u"sign-in-pacing"_s] = QByteArray(32, 'x');
            break;
        case 9: {
            // (The counts sealed again under their key, with the refusals forgotten: as a build that drops them would.)
            core::SignInPacing pacing;
            core::PacingFile::Pace pace;
            pace.writtenAfterMs = 0;
            core::PacingFile signIns(pacing, core::profile::signInsFile(settings), loaded.store, pace);
            signIns.read();
            pacing.clear();
            static_cast<void>(pacing.saidNo("a digest of another", {}));
            QVERIFY(signIns.waitForWritten(5000));
            break;
        }
        case 10:
            QVERIFY(write(core::profile::incidentsFile(settings), "{}"));
            break;
        case 11:
            QVERIFY(write(core::profile::setupOverridesFile(settings), R"({"version": 1, "servers": []})"));
            break;
        case 12:
            QVERIFY(QFile::remove(core::profile::setupOverridesFile(settings)));
            break;
        default:
            QFAIL("no such loss");
        }
        const QJsonObject report = loaded.check();
        QVERIFY(!report["passed"_L1].toBool());
        QVERIFY2(failuresOf(report).contains(said), qPrintable(failuresOf(report)));
    }

    // A window that Windows moved onto the screen it is shown on is said, and is no loss.
    void aWindowMovedIsSaidAndIsNoLoss()
    {
        Loaded loaded;
        QVERIFY(loaded.wrong.isEmpty() && loaded.started);
        QJsonObject now = testing::profiles::jsonIn(loaded.settings);
        QJsonArray windows = now[u"windows"_s].toArray();
        QJsonObject first = windows.at(0).toObject();
        QJsonObject place = first[u"place"_s].toObject();
        place[u"x"_s] = 0;
        first[u"place"_s] = place;
        windows.replace(0, first);
        now[u"windows"_s] = windows;
        QVERIFY(write(loaded.settings, QJsonDocument(now).toJson()));
        const QJsonObject report = loaded.check();
        QVERIFY2(report["passed"_L1].toBool(), qPrintable(failuresOf(report)));
        QCOMPARE(report["windows_moved"_L1].toArray(), QJsonArray{u"windows[0].place.x"_s});
    }

    // What the program moves on by its own rules is no loss: the time of its last look for an update later than it was
    // (a start that looked: dry run 3 of the VM step, from 0.1.0, 2026-11-15), which is said; and the servers and the
    // addresses reached encrypted in another order or with more (the server signed in to last comes first). An earlier
    // time, none, or one that does not read is lost; so is a server or an address that is there no more.
    void whatTheProgramMovesOnIsNoLoss_data()
    {
        QTest::addColumn<int>("change");
        QTest::addColumn<bool>("passes");
        QTest::addColumn<QString>("said");
        QTest::newRow("looked for an update later") << 0 << true << u"last_update_check"_s;
        QTest::newRow("looked for an update earlier") << 1 << false << u"settings.json lost last_update_check"_s;
        QTest::newRow("the last look gone") << 2 << false << u"settings.json lost last_update_check"_s;
        QTest::newRow("the last look unreadable") << 3 << false << u"settings.json lost last_update_check"_s;
        QTest::newRow("a server more, first") << 4 << true << QString();
        QTest::newRow("an address reached encrypted more, first") << 5 << true << QString();
        QTest::newRow("a server gone") << 6 << false << u"settings.json lost servers[0]"_s;
        QTest::newRow("an address reached encrypted gone")
            << 7 << false << u"settings.json lost reached_encrypted[0]"_s;
    }
    void whatTheProgramMovesOnIsNoLoss()
    {
        QFETCH(int, change);
        QFETCH(bool, passes);
        QFETCH(QString, said);
        Loaded loaded;
        QVERIFY(loaded.wrong.isEmpty() && loaded.started);
        QJsonObject now = testing::profiles::jsonIn(loaded.settings);
        const QDateTime looked =
            QDateTime::fromString(loaded.fixture.settings["last_update_check"_L1].toString(), Qt::ISODate);
        QVERIFY(looked.isValid());
        const auto more = [&now](const QString& key, const QString& first) {
            QJsonArray all = now[key].toArray();
            QVERIFY(!all.isEmpty());
            all.prepend(first);
            now[key] = all;
        };
        switch (change) {
        case 0:
            now[u"last_update_check"_s] = looked.addDays(5).toString(Qt::ISODate);
            break;
        case 1:
            now[u"last_update_check"_s] = looked.addSecs(-1).toString(Qt::ISODate);
            break;
        case 2:
            now.remove(u"last_update_check"_s);
            break;
        case 3:
            now[u"last_update_check"_s] = u"soon"_s;
            break;
        case 4:
            more(u"servers"_s, u"http://127.0.0.1:47392/"_s);
            break;
        case 5:
            more(u"reached_encrypted"_s, u"198.51.100.99"_s);
            break;
        case 6:
            now[u"servers"_s] = QJsonArray();
            break;
        case 7:
            now[u"reached_encrypted"_s] = QJsonArray{u"198.51.100.99"_s};
            break;
        default:
            QFAIL("no such change");
        }
        QVERIFY(write(loaded.settings, QJsonDocument(now).toJson()));
        const QJsonObject report = loaded.check();
        QCOMPARE(report["passed"_L1].toBool(), passes);
        if (passes) {
            QCOMPARE(report["settings_moved_on"_L1].toArray(), said.isEmpty() ? QJsonArray() : QJsonArray{said});
        } else {
            QVERIFY2(failuresOf(report).contains(said), qPrintable(failuresOf(report)));
        }
    }

    // Whether this is a machine made for the VM step is asked of Windows, never of the environment (review 012, L5): a
    // caller that sets COMPUTERNAME and USERPROFILE to a scratch folder holding a marker with that name has made no
    // machine of this PC. Nor one that sets _CLUSTER_NETWORK_NAME_, which Windows itself takes for the machine's
    // NetBIOS name where it is set (a cluster's virtual server): the name asked for is the physical one (review 012
    // last, R2). (And the profile Windows names for whoever runs this holds no marker: this PC is none.)
    //   PASS: not a machine made for it.  FAIL (before): the environment's word taken, the guard open.
    void whetherThisIsAMachineMadeForItIsAskedOfWindows()
    {
        QTemporaryDir home;
        QVERIFY(write(testing::disposableMarkerIn(home.path()), "K012-NOT-A-VM"));
        const QByteArray name = qgetenv("COMPUTERNAME");
        const QByteArray profile = qgetenv("USERPROFILE");
        const bool clustered = qEnvironmentVariableIsSet("_CLUSTER_NETWORK_NAME_");
        const QByteArray cluster = qgetenv("_CLUSTER_NETWORK_NAME_");
        const auto back = qScopeGuard([&] {
            qputenv("COMPUTERNAME", name);
            qputenv("USERPROFILE", profile);
            if (clustered) {
                qputenv("_CLUSTER_NETWORK_NAME_", cluster);
            } else {
                qunsetenv("_CLUSTER_NETWORK_NAME_");
            }
        });
        qputenv("COMPUTERNAME", "K012-NOT-A-VM");
        qputenv("_CLUSTER_NETWORK_NAME_", "K012-NOT-A-VM");
        qputenv("USERPROFILE", QDir::toNativeSeparators(home.path()).toLocal8Bit());
        QVERIFY(!testing::isDisposableMachine());
        // What Windows says is not what was set.
        QVERIFY(!testing::machineNameOfWindows().isEmpty());
        QVERIFY2(testing::machineNameOfWindows() != u"K012-NOT-A-VM"_s,
                 "the machine's name was taken from _CLUSTER_NETWORK_NAME_ or COMPUTERNAME");
        QVERIFY(!testing::profileOfWindows().isEmpty());
        QVERIFY(QDir(testing::profileOfWindows()) != QDir(home.path()));
        QVERIFY(!QFile::exists(testing::disposableMarkerIn(testing::profileOfWindows())));
        // A scratch profile whose marker names this machine as Windows names it: still none.
        QVERIFY(write(testing::disposableMarkerIn(home.path()), testing::machineNameOfWindows().toUtf8()));
        qputenv("COMPUTERNAME", name);
        QVERIFY(!testing::isDisposableMachine());
        // And a marker is one only where it names the machine.
        QVERIFY(testing::markerNames(home.path(), testing::machineNameOfWindows()));
        QVERIFY(!testing::markerNames(home.path(), u"ANOTHER-MACHINE-0"_s));
        QVERIFY(!testing::markerNames(home.path(), QString()));
        QVERIFY(!testing::markerNames(QString(), testing::machineNameOfWindows()));
    }

    // A machine made for the VM steps is a virtual one, as its firmware says (outside review of 0.1.2, T5): a marker in
    // the profile that names the machine proves no VM, so a machine whose firmware names no hypervisor is none, marker
    // or not; one whose firmware names one (QEMU, VMware, Hyper-V, VirtualBox) is one with the marker, and only so.
    void onlyAMachineWhoseFirmwareNamesAHypervisorIsOneMadeForIt()
    {
        const QTemporaryDir home;
        const QString name = u"K012-VM-1"_s;
        QVERIFY(write(testing::disposableMarkerIn(home.path()), name.toUtf8()));
        const testing::Firmware pc{u"A maker of PCs"_s, u"A desk PC"_s};
        QCOMPARE(testing::hypervisorNamedBy(pc), QString());
        QVERIFY2(!testing::whyNotDisposable(home.path(), name, pc).isEmpty(),
                 "a PC with the marker was taken for a VM");
        const std::vector<std::pair<testing::Firmware, QString>> virtualOnes{
            {{u"QEMU"_s, u"Standard PC (Q35 + ICH9, 2009)"_s}, u"QEMU"_s},
            {{u"VMware, Inc."_s, u"VMware Virtual Platform"_s}, u"VMware"_s},
            {{u"Microsoft Corporation"_s, u"Virtual Machine"_s}, u"Hyper-V"_s},
            {{u"innotek GmbH"_s, u"VirtualBox"_s}, u"VirtualBox"_s},
        };
        for (const auto& [firmware, hypervisor] : virtualOnes) {
            QCOMPARE(testing::hypervisorNamedBy(firmware), hypervisor);
            QVERIFY2(testing::whyNotDisposable(home.path(), name, firmware).isEmpty(), qPrintable(hypervisor));
            // (Without the marker naming it: none all the same.)
            QVERIFY(!testing::whyNotDisposable(home.path(), u"ANOTHER-MACHINE-0"_s, firmware).isEmpty());
        }
        // A machine of the same maker as a hypervisor that is none (a PC of Microsoft's), and firmware that says
        // nothing.
        QCOMPARE(testing::hypervisorNamedBy({u"Microsoft Corporation"_s, u"A laptop"_s}), QString());
        QCOMPARE(testing::hypervisorNamedBy({}), QString());
        QVERIFY(!testing::whyNotDisposable(home.path(), name, {}).isEmpty());
        // And this PC: whatever its firmware says (nothing of it is said here), it is none.
        QVERIFY(!testing::isDisposableMachine());
    }

    // The program itself refuses on a machine that is not made for it, before it looks at anything: here, with a
    // scratch home that holds no marker, and one whose marker names another machine.
    void theToolRefusesOnAMachineNotMadeForIt()
    {
        const QString writer = QCoreApplication::applicationDirPath() + u"/qkvm-test-write-profile.exe"_s;
        const QString checker = QCoreApplication::applicationDirPath() + u"/qkvm-test-check-profile.exe"_s;
        QVERIFY(QFile::exists(writer) && QFile::exists(checker));
        for (const QByteArray& marker : {QByteArray(), QByteArray("ANOTHER-MACHINE-0")}) {
            QTemporaryDir home;
            QVERIFY(QDir().mkpath(home.filePath(u"local"_s)) && QDir().mkpath(home.filePath(u"roaming"_s)) &&
                    QDir().mkpath(home.filePath(u"temp"_s)));
            if (!marker.isEmpty()) {
                QVERIFY(write(home.filePath(u"qkvm-disposable-vm.txt"_s), marker));
            }
            QProcessEnvironment scratch = QProcessEnvironment::systemEnvironment();
            scratch.insert(u"USERPROFILE"_s, QDir::toNativeSeparators(home.path()));
            scratch.insert(u"LOCALAPPDATA"_s, QDir::toNativeSeparators(home.filePath(u"local"_s)));
            scratch.insert(u"APPDATA"_s, QDir::toNativeSeparators(home.filePath(u"roaming"_s)));
            scratch.insert(u"TEMP"_s, QDir::toNativeSeparators(home.filePath(u"temp"_s)));
            scratch.insert(u"TMP"_s, QDir::toNativeSeparators(home.filePath(u"temp"_s)));
            const QString report = home.filePath(u"report.json"_s);
            for (const auto& [tool, arguments] :
                 {std::pair{writer, QStringList{u"--disposable"_s, fixtureFolder()}},
                  std::pair{writer, QStringList{fixtureFolder()}},
                  std::pair{checker, QStringList{u"--disposable"_s, fixtureFolder(), report}},
                  std::pair{checker, QStringList{fixtureFolder(), report}}}) {
                QProcess run;
                run.setProcessEnvironment(scratch);
                run.start(tool, arguments);
                QVERIFY(run.waitForFinished(30'000));
                QCOMPARE(run.exitStatus(), QProcess::NormalExit);
                QCOMPARE(run.exitCode(), 2);
            }
            QVERIFY(QDir(home.filePath(u"local"_s)).isEmpty());
            QVERIFY(!QFile::exists(home.filePath(u"report.json"_s)));
        }
    }
};

QTEST_GUILESS_MAIN(TestProfileLoading)
#include "test_profile_loading.moc"
