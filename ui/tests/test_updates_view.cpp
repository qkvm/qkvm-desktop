#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <thread>
#include <vector>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/redfish_device.h>
#include <qkvm/core/settings.h>

#include "memory_secrets.h"
#include "own_wall.h"
#include "program_end.h"
#include "stand_in.h"
#include "update_by_hand.h"
#include "updates_view.h"
#include "win/hand_over_end.h"
#include "win/own_temp.h"
#include "win/setup_wait.h"
#include "win/shell_start.h"

#include <sddl.h>

using namespace Qt::StringLiterals;
using namespace qkvm;
using namespace qkvm::core::testing;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

namespace {

// A home of its own for a start of the program itself in a test (the hard rule: never the person's own profile): a new
// folder of the build's (test-homes, beside the test), with its local and roaming application data and its Temp, named
// to the child by USERPROFILE, LOCALAPPDATA, APPDATA, TEMP and TMP (Windows' known folders follow them in the child,
// and so do Qt's and the program's data and cache). Before anything is started with it, a probe of the test's own,
// started with the same environment, says where those resolve, and `why` says what is amiss where any of them is not in
// this home or is the person's own qKVM data.
class ScratchHome {
public:
    ScratchHome()
        : m_home(u"%1/test-homes/%2-%3"_s.arg(QCoreApplication::applicationDirPath())
                     .arg(QCoreApplication::applicationPid())
                     .arg(QDateTime::currentMSecsSinceEpoch()))
    {
        for (const QString& part : {u"AppData/Local"_s, u"AppData/Roaming"_s, u"Temp"_s}) {
            static_cast<void>(QDir().mkpath(m_home + u'/' + part));
        }
        const QString home = QDir::toNativeSeparators(m_home);
        const QHash<QString, QString> own{{u"USERPROFILE"_s, home},
                                          {u"LOCALAPPDATA"_s, home + u"\\AppData\\Local"_s},
                                          {u"APPDATA"_s, home + u"\\AppData\\Roaming"_s},
                                          {u"TEMP"_s, home + u"\\Temp"_s},
                                          {u"TMP"_s, home + u"\\Temp"_s}};
        QStringList lines;
        if (LPWCH all = GetEnvironmentStringsW(); all != nullptr) {
            for (const wchar_t* at = all; *at != L'\0'; at += std::wcslen(at) + 1) {
                const QString line = QString::fromWCharArray(at);
                const QString name = line.left(line.indexOf(u'=', 1));
                if (!std::ranges::any_of(own.keys(), [&name](const QString& one) {
                        return one.compare(name, Qt::CaseInsensitive) == 0;
                    })) {
                    lines.append(line);
                }
            }
            FreeEnvironmentStringsW(all);
        }
        for (auto one = own.cbegin(); one != own.cend(); ++one) {
            lines.append(one.key() + u'=' + one.value());
        }
        std::ranges::sort(lines, [](const QString& one, const QString& other) {
            return one.left(one.indexOf(u'=', 1)).compare(other.left(other.indexOf(u'=', 1)), Qt::CaseInsensitive) < 0;
        });
        for (const QString& line : lines) {
            const std::wstring wide = line.toStdWString();
            m_block.insert(m_block.end(), wide.begin(), wide.end());
            m_block.push_back(L'\0');
        }
        m_block.push_back(L'\0');
    }
    ScratchHome(const ScratchHome&) = delete;
    ScratchHome& operator=(const ScratchHome&) = delete;
    ScratchHome(ScratchHome&&) = delete;
    ScratchHome& operator=(ScratchHome&&) = delete;
    ~ScratchHome() { QDir(m_home).removeRecursively(); }

    wchar_t* environment() { return m_block.data(); }

    // Empty where the probe, started with this environment, finds its data, roaming data and Temp in this home and
    // none of them in the person's own qKVM data; what is amiss otherwise.
    QString whyNot()
    {
        const QString said = m_home + u"/where.txt"_s;
        const std::wstring probe =
            QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/qkvm-test-where-data.exe"_s)
                .toStdWString();
        std::wstring line = L"\"" + probe + L"\" \"" + QDir::toNativeSeparators(said).toStdWString() + L"\"";
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        PROCESS_INFORMATION made{};
        if (CreateProcessW(probe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                           environment(), nullptr, &start, &made) == FALSE) {
            return u"the probe could not be started"_s;
        }
        CloseHandle(made.hThread);
        const bool ended = WaitForSingleObject(made.hProcess, 10000) == WAIT_OBJECT_0;
        if (!ended) {
            TerminateProcess(made.hProcess, 1);
        }
        CloseHandle(made.hProcess);
        QFile file(said);
        if (!ended || !file.open(QIODevice::ReadOnly)) {
            return u"the probe said nothing"_s;
        }
        const QByteArray bytes = file.readAll();
        const QStringList places =
            QString::fromWCharArray(reinterpret_cast<const wchar_t*>(bytes.constData()), bytes.size() / 2)
                .split(u'\n', Qt::SkipEmptyParts);
        const QString home = QDir::toNativeSeparators(m_home).toLower();
        const QString own = qEnvironmentVariable("LOCALAPPDATA").toLower();
        if (places.size() != 3 || own.isEmpty()) {
            return u"the probe said: "_s + places.join(u" | "_s);
        }
        for (const QString& place : places) {
            const QString lowered = QDir::toNativeSeparators(place).toLower();
            if (!lowered.startsWith(home) || lowered.startsWith(own + u"\\qkvm desktop"_s) ||
                lowered.startsWith(own + u"\\qkvm"_s)) {
                return u"a place is not in the scratch home: "_s + place;
            }
        }
        return {};
    }

private:
    QString m_home;
    std::vector<wchar_t> m_block;
};

} // namespace

class TestUpdatesView : public QObject {
    Q_OBJECT
private slots:
    void recoveryNeedsAnExplicitChoiceAndIdleWindows()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        UpdateNetwork network;
        core::Updates updates({{}, {}, u"0.1.0"_s}, settings, network, folder.path());
        ui::UpdatesView view(updates);
        bool idle = false;
        QList<bool> requests;
        view.installWith([&] { return idle; }, [](const QString&) { return true; });
        view.recoverWith([&](bool previous) {
            requests.append(previous);
            return true;
        });
        updates.checkIfDue(QDateTime::currentDateTimeUtc());
        QVERIFY(requests.isEmpty());
        view.recover(true);
        QVERIFY(requests.isEmpty());
        QVERIFY(view.problem().contains(u"all qKVM windows"_s));
        idle = true;
        view.recover(true);
        view.recover(false);
        QCOMPARE(requests, QList<bool>({true, false}));
        QVERIFY(view.problem().isEmpty());
        view.recoverWith([](bool) { return false; });
        view.recover(false);
        QVERIFY(!view.problem().isEmpty());
        QVERIFY(network.requests.isEmpty());
    }

    void unconfiguredBuildExplainsWhyAndCannotInstall()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        UpdateNetwork network;
        core::Updates updates({{}, {}, u"0.1.0"_s}, settings, network, folder.path());
        ui::UpdatesView view(updates);
        QCOMPARE(view.currentVersion(), u"0.1.0"_s);
        QVERIFY(view.status().contains(u"development build"_s));
        QVERIFY(!view.canCheck());
        QVERIFY(!view.canDownload());
        QVERIFY(!view.ready());
        bool opened = false;
        view.installWith([] { return true; },
                         [&](const QString&) {
                             opened = true;
                             return true;
                         });
        view.install();
        QVERIFY(!opened);
        QVERIFY(network.requests.isEmpty());
    }

    void installerNeedsAnExplicitChoiceAndAllWindowsToBeIdle()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        UpdateNetwork network;
        ReleaseSigner signer;
        const QByteArray package("made-up package");
        core::Updates updates({QUrl(u"https://updates.example.test/stable.json"_s), signer.publicKey(), u"0.1.0"_s,
                               core::UpdateKind::Msix},
                              settings, network, folder.path());
        ui::UpdatesView view(updates);
        QVERIFY(!view.showsFile());
        network.next = {signer.seal(releaseFor(package, QDateTime::currentDateTimeUtc(), core::UpdateKind::Msix))};
        view.check();
        QTRY_VERIFY(view.canDownload());
        QCOMPARE(view.version(), u"0.2.0"_s);
        QVERIFY(!view.notes().isEmpty());
        QCOMPARE(network.requests.size(), 1);
        bool idle = false;
        int opened = 0;
        view.installWith([&] { return idle; },
                         [&](const QString&) {
                             ++opened;
                             return true;
                         });
        view.install();
        QCOMPARE(opened, 0);
        network.next = {package};
        view.download();
        QTRY_VERIFY(view.ready());
        QCOMPARE(opened, 0);
        view.install();
        QCOMPARE(opened, 0);
        QVERIFY(view.problem().contains(u"all qKVM windows"_s));
        // Once the hold lifts, its words go as they are read next, and for good (review 012-hold's INFO 3): nothing is
        // opened before a press, and a hold that comes again is said at the next press. What another refusal says is
        // its own, and a copy that cannot open an installer goes on saying so.
        idle = true;
        QVERIFY(view.problem().isEmpty());
        idle = false;
        QVERIFY(view.problem().isEmpty());
        QCOMPARE(opened, 0);
        view.install();
        QVERIFY(view.problem().contains(u"all qKVM windows"_s));
        view.recover(false);
        QVERIFY2(view.problem().contains(u"before recovery"_s), qPrintable(view.problem()));
        view.installWith([] { return true; }, {});
        view.install();
        for (int read = 0; read < 2; ++read) {
            QVERIFY(view.problem().contains(u"all qKVM windows"_s));
        }
        view.installWith([&] { return idle; },
                         [&](const QString&) {
                             ++opened;
                             return true;
                         });
        idle = true;
        view.install();
        QCOMPARE(opened, 1);
        QVERIFY(view.problem().isEmpty());
        QVERIFY(view.status().contains(u"has not been closed"_s));
        QCOMPARE(updates.state(), core::Updates::State::Ready);
        // Never shown as a file.
        int shown = 0;
        view.showWith([&](const QString&) {
            ++shown;
            return true;
        });
        view.showFile();
        QCOMPARE(shown, 0);
    }

    // A portable copy: "Show the verified file", never an installer; the file is the one that was downloaded.
    void aPortableCopyShowsTheVerifiedFileAndNeverInstalls()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        UpdateNetwork network;
        ReleaseSigner signer;
        const QByteArray package("made-up zip");
        core::Updates updates({QUrl(u"https://updates.example.test/stable.json"_s), signer.publicKey(), u"0.1.0"_s,
                               core::UpdateKind::Portable},
                              settings, network, folder.path());
        ui::UpdatesView view(updates);
        QVERIFY(view.showsFile());
        int opened = 0;
        view.installWith([] { return true; },
                         [&](const QString&) {
                             ++opened;
                             return true;
                         });
        QStringList shown;
        network.next = {signer.seal(releaseFor(package))};
        view.check();
        QTRY_VERIFY(view.canDownload());
        view.showFile();
        QVERIFY(shown.isEmpty());
        network.next = {package};
        view.download();
        QTRY_VERIFY(view.ready());
        QCOMPARE(view.status(), u"qKVM 0.2.0 is downloaded and verified."_s);
        // No way to show it was supplied: said, and nothing shown.
        view.showFile();
        QVERIFY(!view.problem().isEmpty());
        view.showWith([&](const QString& file) {
            shown.append(file);
            return true;
        });
        view.install();
        QCOMPARE(opened, 0);
        view.showFile();
        QCOMPARE(shown.size(), 1);
        QFile file(shown.first());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), package);
        QVERIFY(view.problem().isEmpty());
        QCOMPARE(view.status(), u"The verified file is shown in its folder."_s);
        QCOMPARE(updates.state(), core::Updates::State::Ready);
        QCOMPARE(opened, 0);
        // Checking again starts over.
        view.cancel();
        QVERIFY(!view.status().contains(u"shown"_s));
    }

    // A Setup copy: "Install and restart" only when asked, with every window idle, and only where the program can hand
    // over (close its windows and end); then Setup has been started, from the verified file, and the hand-over asked.
    void installAndRestartNeedsAnExplicitChoiceIdleWindowsAndAHandOver()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        UpdateNetwork network;
        ReleaseSigner signer;
        QFile standIn(QCoreApplication::applicationDirPath() + u"/qkvm-test-stand-in-setup.exe"_s);
        QVERIFY(standIn.open(QIODevice::ReadOnly));
        const QByteArray package = standIn.readAll();
        const QString program = folder.filePath(u"program"_s);
        QVERIFY(QDir().mkpath(program));
        QFile marker(program + u'/' + QString::fromLatin1(core::setupMarkerName));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write(core::setupMarkerContent), 6);
        marker.close();
        core::Updates updates({QUrl(u"https://updates.example.test/stable-setup.json"_s), signer.publicKey(),
                               u"0.1.0"_s, core::UpdateKind::Setup, program, folder.filePath(u"data"_s)},
                              settings, network, folder.filePath(u"downloads"_s));
        ui::UpdatesView view(updates);
        QVERIFY(view.restarts());
        QVERIFY(!view.showsFile());
        network.next = {signer.seal(releaseFor(package, QDateTime::currentDateTimeUtc(), core::UpdateKind::Setup))};
        view.check();
        QTRY_VERIFY(view.canDownload());
        network.next = {package};
        view.download();
        QTRY_VERIFY(view.ready());
        QCOMPARE(view.status(), u"qKVM 0.2.0 is downloaded and verified."_s);
        const QString log = folder.filePath(u"data/updates/setup-0.2.0.log"_s);
        bool idle = false;
        int handedOver = 0;
        int opened = 0;
        view.installWith([&] { return idle; },
                         [&](const QString&) {
                             ++opened;
                             return true;
                         });
        // Busy windows: nothing is run.
        view.handOverWith([&] { ++handedOver; });
        view.install();
        QVERIFY(view.problem().contains(u"all qKVM windows"_s));
        QCOMPARE(handedOver, 0);
        QVERIFY(!view.status().contains(u"qKVM closes now"_s));
        // And what holds it is said, as the program says it (WallWindows::whatHoldsAnUpdate).
        view.holdWith([] { return u"\"Add a KVM\" has changes that are not saved."_s; });
        view.install();
        QCOMPARE(view.problem(), u"\"Add a KVM\" has changes that are not saved."_s);
        QCOMPARE(handedOver, 0);
        // Once it lifts, its words go as they are read next (review 012-hold's INFO 3); nothing is run before a press.
        idle = true;
        QVERIFY(view.problem().isEmpty());
        QCOMPARE(handedOver, 0);
        QVERIFY(!view.status().contains(u"qKVM closes now"_s));
        idle = false;
        view.holdWith({});
        // No way to hand over: nothing is run either.
        idle = true;
        view.handOverWith({});
        view.install();
        QVERIFY(!view.problem().isEmpty());
        QCOMPARE(handedOver, 0);
        QVERIFY(!view.status().contains(u"qKVM closes now"_s));
        QTest::qWait(300);
        QVERIFY(!QFileInfo::exists(log));
        // Asked, idle, and the program can end: Setup is started, and the program asked to close and end.
        view.handOverWith([&] { ++handedOver; });
        view.install();
        QCOMPARE(handedOver, 1);
        QCOMPARE(opened, 0);
        QVERIFY(view.problem().isEmpty());
        QVERIFY(view.status().contains(u"qKVM closes now"_s));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(log).size() > 0, 15000);
        QFile ran(log);
        QVERIFY(ran.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(ran.readAll()).contains(u"/UPDATE=1 /DIR=\""_s + QDir::toNativeSeparators(program)));
        // Never shown as a file.
        int shown = 0;
        view.showWith([&](const QString&) {
            ++shown;
            return true;
        });
        view.showFile();
        QCOMPARE(shown, 0);
        QTest::qWait(500); // (the stand-in ends just after writing)
    }

    // What the start after a hand-over found is told by the view, and as wrong where it went wrong.
    void theStartAfterAHandOverIsTold()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        UpdateNetwork network;
        const QString note = folder.filePath(u"data/updates/setup-handover.json"_s);
        QVERIFY(QDir().mkpath(QFileInfo(note).absolutePath()));
        QFile written(note);
        QVERIFY(written.open(QIODevice::WriteOnly));
        QVERIFY(written.write(R"({"schema":1,"from":"0.1.0","to":"0.2.0","process":4,"created":"1"})") > 0);
        written.close();
        core::Updates updates(
            {{}, {}, u"0.2.0"_s, core::UpdateKind::Setup, folder.filePath(u"program"_s), folder.filePath(u"data"_s)},
            settings, network, folder.filePath(u"downloads"_s));
        const ui::UpdatesView view(updates);
        QCOMPARE(view.afterSetup(), u"Updated to qKVM 0.2.0."_s);
        QVERIFY(!view.afterSetupWentWrong());
    }

    // The forced end of a program that has handed over to Setup is counted from the end of main's own quit goodbyes
    // (endTheProgram): a server's session that takes a while to be given back is given back first, and the end comes
    // its pace after that; and where the goodbyes would take longer than the whole bound, the end comes at that bound
    // (within Setup's minute).
    void theForcedEndComesAfterTheQuitsGoodbyesAndWithinTheWhole_data()
    {
        QTest::addColumn<int>("wholeMs");
        QTest::addColumn<bool>("afterTheGoodbyes");
        QTest::newRow("after the goodbyes") << 20000 << true;
        QTest::newRow("at the whole bound") << 600 << false;
    }

    void theForcedEndComesAfterTheQuitsGoodbyesAndWithinTheWhole()
    {
        QFETCH(int, wholeMs);
        QFETCH(bool, afterTheGoodbyes);
        std::atomic<int> deletions{0};
        StandIn controller([&deletions](const Asked& asked) {
            if (asked.method == "DELETE") {
                ++deletions;
            }
            Answered later{204, QByteArray()};
            later.afterMs = 1500;
            return later;
        });
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        ui::OwnWall own(settings, secrets);
        QElapsedTimer clock;
        clock.start();
        std::atomic<qint64> endedAt{-1};
        ui::HandOverEnd end({std::chrono::milliseconds(wholeMs), std::chrono::milliseconds(300)},
                            [&endedAt, &clock] { endedAt = clock.elapsed(); });
        const auto stop = qScopeGuard([&end] { end.cancel(); });
        end.begin();
        core::ControllerGoodbyes::program().giveBack({QUrl(controller.address()), false, {}}, 5000,
                                                     u"/redfish/v1/SessionService/Sessions/1"_s, "test-token", 5000);
        ui::endTheProgram(own, end);
        const qint64 goodbyesOver = clock.elapsed();
        QCOMPARE(deletions.load(), 1);
        QVERIFY2(goodbyesOver >= 1300, qPrintable(QString::number(goodbyesOver)));
        if (afterTheGoodbyes) {
            QCOMPARE(endedAt.load(), qint64{-1});
            QTRY_VERIFY(endedAt.load() >= 0);
            QVERIFY2(endedAt.load() >= goodbyesOver + 250 && endedAt.load() <= goodbyesOver + 5000,
                     qPrintable(u"%1 after %2"_s.arg(endedAt.load()).arg(goodbyesOver)));
        } else {
            QVERIFY(endedAt.load() >= 0);
            QVERIFY2(endedAt.load() >= 500 && endedAt.load() < goodbyesOver,
                     qPrintable(u"%1 before %2"_s.arg(endedAt.load()).arg(goodbyesOver)));
        }
    }

    // The program that has handed over to Setup ends within its time, whatever holds up its quit: its mutex (as the
    // program's single-instance one) goes with it, and Setup, which waits for that, goes on.
    void aProgramThatHandedOverEndsInTimeEvenWhereItsQuitIsStuck()
    {
        const QString mutex = u"Local\\qkvm-test-stuck-"_s + QString::number(QCoreApplication::applicationPid()) +
                              u'-' + QString::number(QDateTime::currentMSecsSinceEpoch());
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        PROCESS_INFORMATION made{};
        const std::wstring program =
            QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/qkvm-test-stuck-program.exe"_s)
                .toStdWString();
        std::wstring line = L"\"" + program + L"\" \"" + mutex.toStdWString() + L"\" 500";
        QVERIFY(CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &start,
                               &made) != FALSE);
        CloseHandle(made.hThread);
        const auto held = [&] {
            const HANDLE opened = OpenMutexW(SYNCHRONIZE, FALSE, mutex.toStdWString().c_str());
            if (opened != nullptr) {
                CloseHandle(opened);
            }
            return opened != nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(held() || WaitForSingleObject(made.hProcess, 0) == WAIT_OBJECT_0, 10000);
        const bool ended = WaitForSingleObject(made.hProcess, 8000) == WAIT_OBJECT_0;
        if (!ended) {
            TerminateProcess(made.hProcess, 1);
            WaitForSingleObject(made.hProcess, 5000);
        }
        DWORD code = 1;
        GetExitCodeProcess(made.hProcess, &code);
        CloseHandle(made.hProcess);
        QVERIFY2(ended, "The program that handed over did not end within its time.");
        QCOMPARE(code, DWORD{0});
        QVERIFY(!held());
    }

    // A start while Setup works (its mutex there, no --after-update) ends at once: no box and no window that waits for
    // a click, and nothing waited on, for Setup replaces the program's files meanwhile (Inno retries a file in use for
    // four seconds, then gives up and rolls back). The program itself is started, on a desktop of the test's own that
    // nobody sees, while the test holds Setup's mutex for a moment.
    void aStartWhileSetupWorksEndsAtOnceWithoutAWord()
    {
        const HANDLE setup =
            CreateMutexW(nullptr, TRUE, QString::fromLatin1(ui::setupMutexName).toStdWString().c_str());
        QVERIFY(setup != nullptr);
        // (And Setup's own event, as one at work makes it: named with its process's number, this test's here, review
        // R2.)
        const HANDLE runs = CreateEventW(
            nullptr, TRUE, FALSE,
            ui::setupRunsName(QString::fromLatin1(ui::setupMutexName).toStdWString(), GetCurrentProcessId()).c_str());
        QVERIFY(runs != nullptr);
        const auto letGo = qScopeGuard([setup, runs] {
            CloseHandle(runs);
            CloseHandle(setup);
        });
        const std::wstring desk = L"qkvm-test-" + std::to_wstring(GetCurrentProcessId());
        // (Its own, open to this user at low integrity too, for the program is started so below.)
        HANDLE token = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE);
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> user(size);
        LPWSTR userText = nullptr;
        const bool told =
            size > 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &userText) != FALSE;
        CloseHandle(token);
        QVERIFY(told);
        const std::wstring deskList = L"D:P(A;;GA;;;" + std::wstring(userText) + L")S:(ML;;NW;;;LW)";
        LocalFree(userText);
        PSECURITY_DESCRIPTOR deskSecurity = nullptr;
        QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(deskList.c_str(), SDDL_REVISION_1, &deskSecurity,
                                                                     nullptr) != FALSE);
        SECURITY_ATTRIBUTES deskMade{};
        deskMade.nLength = sizeof(deskMade);
        deskMade.lpSecurityDescriptor = deskSecurity;
        const HDESK unseen = CreateDesktopW(desk.c_str(), nullptr, nullptr, 0, GENERIC_ALL, &deskMade);
        LocalFree(deskSecurity);
        QVERIFY(unseen != nullptr);
        const auto closeDesk = qScopeGuard([unseen] { CloseDesktop(unseen); });
        std::wstring on = desk;
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        start.lpDesktop = on.data();
        PROCESS_INFORMATION made{};
        const std::wstring program =
            QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/qkvm-desktop.exe"_s).toStdWString();
        std::wstring line = L"\"" + program + L"\"";
        // At low integrity: a program that went on all the same (a build that is wrong) could write nothing of the
        // person's own data, which it would otherwise use.
        HANDLE own = nullptr;
        HANDLE low = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(),
                                 TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY,
                                 &own) != FALSE);
        const auto closeTokens = qScopeGuard([&own, &low] {
            CloseHandle(own);
            if (low != nullptr) {
                CloseHandle(low);
            }
        });
        QVERIFY(DuplicateTokenEx(own, 0, nullptr, SecurityImpersonation, TokenPrimary, &low) != FALSE);
        PSID lowLevel = nullptr;
        QVERIFY(ConvertStringSidToSidW(L"S-1-16-4096", &lowLevel) != FALSE);
        TOKEN_MANDATORY_LABEL label{};
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        label.Label.Sid = lowLevel;
        const bool lowered =
            SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(lowLevel)) != FALSE;
        LocalFree(lowLevel);
        QVERIFY(lowered);
        // And with a home of its own: never the person's profile (its data, its cache, its Temp).
        ScratchHome home;
        const QString notSafe = home.whyNot();
        QVERIFY2(notSafe.isEmpty(), qPrintable(u"Not started with the person's own profile: "_s + notSafe));
        QVERIFY(CreateProcessAsUserW(low, program.c_str(), line.data(), nullptr, nullptr, FALSE,
                                     CREATE_UNICODE_ENVIRONMENT, home.environment(), nullptr, &start, &made) != FALSE);
        CloseHandle(made.hThread);
        const bool ended = WaitForSingleObject(made.hProcess, 3000) == WAIT_OBJECT_0;
        if (!ended) {
            TerminateProcess(made.hProcess, 1);
            WaitForSingleObject(made.hProcess, 5000);
        }
        DWORD code = 1;
        GetExitCodeProcess(made.hProcess, &code);
        CloseHandle(made.hProcess);
        QVERIFY2(ended, "The program started while Setup works did not end at once.");
        QCOMPARE(code, DWORD{0});
    }
    // While qKVM's Setup is at work (its mutex there), a start ends at once, unless it is Setup's own (--after-update),
    // which waits for Setup to be over, at most so long, and then starts the program anew through the person's shell
    // (a starter of the test's own here) and ends; where that fails, or it runs above the person, it ends starting
    // nothing, and never goes on itself (the contract with Setup; review C1).
    void aStartWhileSetupWorksEndsUnlessItIsSetupsOwn()
    {
        const QString name = u"Local\\qkvm-test-setup-"_s + QString::number(QCoreApplication::applicationPid()) + u'-' +
                             QString::number(QDateTime::currentMSecsSinceEpoch());
        using ui::WhileSetupWorks;
        QElapsedTimer clock;
        clock.start();
        std::atomic<qint64> startedAt{-1};
        int starts = 0;
        bool startsWell = true;
        const auto starter = [&] {
            ++starts;
            startedAt = clock.elapsed();
            return startsWell;
        };
        // None there: a person's start goes on, and never starts anew; Setup's own starts anew at once, and ends.
        QCOMPARE(ui::whileSetupWorks(name, false, std::chrono::milliseconds(5000), starter), WhileSetupWorks::GoOn);
        QCOMPARE(starts, 0);
        QCOMPARE(ui::whileSetupWorks(name, true, std::chrono::milliseconds(5000), starter),
                 WhileSetupWorks::StartedAnew);
        QCOMPARE(starts, 1);
        QVERIFY(clock.elapsed() < 1000);
        // Where it cannot start anew, it never goes on itself, with Setup's token: it ends, starting nothing (review
        // C1).
        startsWell = false;
        QCOMPARE(ui::whileSetupWorks(name, true, std::chrono::milliseconds(5000), starter),
                 WhileSetupWorks::NotStartedAgain);
        QCOMPARE(starts, 2);
        startsWell = true;
        // Where it runs above the person (elevated, or above medium integrity), it ends at once, and starts nothing
        // through the shell either; a person's own start is not asked that.
        const auto above = [] { return true; };
        QCOMPARE(ui::whileSetupWorks(name, true, std::chrono::milliseconds(5000), starter, above),
                 WhileSetupWorks::NotStartedAgain);
        QCOMPARE(starts, 2);
        QCOMPARE(ui::whileSetupWorks(name, false, std::chrono::milliseconds(5000), starter, above),
                 WhileSetupWorks::GoOn);
        // There for 700 ms: a person's start ends at once; Setup's own waits until it is gone, and only then starts
        // anew.
        std::atomic<bool> made{false};
        std::atomic<bool> letGo{false};
        std::atomic<qint64> goneAt{-1};
        std::thread setup([&] {
            // (As Setup is at work: its mutex, and its own event named with its process's number, this test's here.)
            const HANDLE held = CreateMutexW(nullptr, TRUE, name.toStdWString().c_str());
            const HANDLE runs = CreateEventW(nullptr, TRUE, FALSE,
                                             ui::setupRunsName(name.toStdWString(), GetCurrentProcessId()).c_str());
            made = true;
            while (!letGo) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            goneAt = clock.elapsed();
            CloseHandle(runs);
            CloseHandle(held);
        });
        QTRY_VERIFY(made.load());
        const qint64 before = clock.elapsed();
        QCOMPARE(ui::whileSetupWorks(name, false, std::chrono::milliseconds(10000), starter),
                 WhileSetupWorks::BeingUpdated);
        QVERIFY2(clock.elapsed() - before < 500, qPrintable(QString::number(clock.elapsed() - before)));
        QCOMPARE(starts, 2);
        letGo = true;
        QCOMPARE(ui::whileSetupWorks(name, true, std::chrono::milliseconds(10000), starter),
                 WhileSetupWorks::StartedAnew);
        setup.join();
        QCOMPARE(starts, 3);
        QVERIFY2(startedAt.load() >= goneAt.load(), qPrintable(u"%1 < %2"_s.arg(startedAt.load()).arg(goneAt.load())));
        // Review C4 (the outside review): a handle to Setup's mutex kept elsewhere keeps it there after Setup has
        // ended. Setup's own start waits for Setup's own process (here: a thread of the test's that ends after 700 ms)
        // to end, never for the mutex, and starts anew then though the mutex is still there.
        {
            const HANDLE kept = CreateMutexW(nullptr, FALSE, name.toStdWString().c_str());
            QVERIFY(kept != nullptr);
            const auto letGoOfIt = qScopeGuard([kept] { CloseHandle(kept); });
            std::atomic<qint64> setupEndedAt{-1};
            std::thread setupRuns([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                setupEndedAt = clock.elapsed();
            });
            const auto joined = qScopeGuard([&setupRuns] {
                if (setupRuns.joinable()) {
                    setupRuns.join();
                }
            });
            QCOMPARE(ui::whileSetupWorks(
                         name, true, std::chrono::milliseconds(10000), starter, [] { return false; },
                         setupRuns.native_handle()),
                     WhileSetupWorks::StartedAnew);
            QCOMPARE(starts, 4);
            QVERIFY2(startedAt.load() >= setupEndedAt.load() && setupEndedAt.load() >= 0,
                     qPrintable(u"%1 < %2"_s.arg(startedAt.load()).arg(setupEndedAt.load())));
            // And Setup's own process that does not end within `most`: it gives up, and starts nothing.
            const HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            QVERIFY(never != nullptr);
            const auto closeNever = qScopeGuard([never] { CloseHandle(never); });
            QCOMPARE(ui::whileSetupWorks(
                         name, true, std::chrono::milliseconds(300), starter, [] { return false; }, never),
                     WhileSetupWorks::StillUpdating);
            QCOMPARE(starts, 4);
        }
        // The starter of this test program is found, began before it, and is not this program.
        const HANDLE starterOfThis = ui::openTheStarter();
        QVERIFY(starterOfThis != nullptr);
        QVERIFY(GetProcessId(starterOfThis) != GetCurrentProcessId());
        CloseHandle(starterOfThis);
        // Review R2 (the outside review's confirm round): a handle to Setup's mutex kept elsewhere after Setup has
        // ended, with no process that runs saying it is that Setup (none, or one named with a number no process has):
        // a person's start goes on, the restart through the shell among them; only a Setup that runs holds it.
        {
            const HANDLE kept = CreateMutexW(nullptr, FALSE, name.toStdWString().c_str());
            QVERIFY(kept != nullptr);
            const auto letGoOfIt = qScopeGuard([kept] { CloseHandle(kept); });
            QCOMPARE(ui::whileSetupWorks(name, false, std::chrono::milliseconds(5000), starter), WhileSetupWorks::GoOn);
            const HANDLE ofNone =
                CreateEventW(nullptr, TRUE, FALSE, ui::setupRunsName(name.toStdWString(), 0xFFFFFFFCUL).c_str());
            QVERIFY(ofNone != nullptr);
            const auto closeOfNone = qScopeGuard([ofNone] { CloseHandle(ofNone); });
            QCOMPARE(ui::whileSetupWorks(name, false, std::chrono::milliseconds(5000), starter), WhileSetupWorks::GoOn);
            const HANDLE runs = CreateEventW(nullptr, TRUE, FALSE,
                                             ui::setupRunsName(name.toStdWString(), GetCurrentProcessId()).c_str());
            QVERIFY(runs != nullptr);
            const auto closeRuns = qScopeGuard([runs] { CloseHandle(runs); });
            QCOMPARE(ui::whileSetupWorks(name, false, std::chrono::milliseconds(5000), starter),
                     WhileSetupWorks::BeingUpdated);
            QCOMPARE(starts, 4);
        }
        // There all the time: Setup's own start gives up after `most`, and starts nothing.
        const HANDLE held = CreateMutexW(nullptr, TRUE, name.toStdWString().c_str());
        QVERIFY(held != nullptr);
        const HANDLE heldRuns =
            CreateEventW(nullptr, TRUE, FALSE, ui::setupRunsName(name.toStdWString(), GetCurrentProcessId()).c_str());
        QVERIFY(heldRuns != nullptr);
        const auto closeHeldRuns = qScopeGuard([heldRuns] { CloseHandle(heldRuns); });
        const qint64 waited = clock.elapsed();
        QCOMPARE(ui::whileSetupWorks(name, true, std::chrono::milliseconds(800), starter),
                 WhileSetupWorks::StillUpdating);
        QVERIFY2(clock.elapsed() - waited >= 700 && clock.elapsed() - waited < 5000,
                 qPrintable(QString::number(clock.elapsed() - waited)));
        QCOMPARE(starts, 4);
        CloseHandle(held);
    }
    // Review C1: what runs above the person (elevated, or at an integrity above medium) is never let start qKVM after
    // an update; this test runs as the person, and so does its own process, by the rule the shell is held to; a process
    // of the system's, or none, is not the person's.
    void aboveThePersonIsElevatedOrAboveMediumAndTheShellIsHeldToThePerson()
    {
        QVERIFY(ui::aboveThePerson(true, SECURITY_MANDATORY_MEDIUM_RID));
        QVERIFY(ui::aboveThePerson(false, SECURITY_MANDATORY_HIGH_RID));
        QVERIFY(ui::aboveThePerson(false, SECURITY_MANDATORY_MEDIUM_PLUS_RID));
        QVERIFY(ui::aboveThePerson(false, SECURITY_MANDATORY_SYSTEM_RID));
        QVERIFY(!ui::aboveThePerson(false, SECURITY_MANDATORY_MEDIUM_RID));
        QVERIFY(!ui::aboveThePerson(false, SECURITY_MANDATORY_LOW_RID));
        HANDLE token = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE);
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        const bool told = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
        CloseHandle(token);
        QVERIFY(told);
        if (elevation.TokenIsElevated != 0) {
            QSKIP("This test program runs elevated: the person's own process is not what it is here.");
        }
        QVERIFY(!ui::runsAboveThePerson());
        QVERIFY(ui::processIsThePersons(GetCurrentProcessId()));
        // (The system's own process, and a number of none.)
        QVERIFY(!ui::processIsThePersons(4));
        QVERIFY(!ui::processIsThePersons(0));
        // Another user's token is not the person's: Windows' anonymous one stands for it here. One of the person's own
        // at low integrity is theirs, and not above them.
        HANDLE ours = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT, &ours) !=
                FALSE);
        const auto closeOurs = qScopeGuard([ours] { CloseHandle(ours); });
        QVERIFY(ui::tokenIsThePersons(ours, ours));
        QVERIFY(ImpersonateAnonymousToken(GetCurrentThread()) != FALSE);
        HANDLE anonymous = nullptr;
        const bool openedAnonymous = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &anonymous) != FALSE;
        QVERIFY(RevertToSelf() != FALSE);
        QVERIFY(openedAnonymous);
        const auto closeAnonymous = qScopeGuard([anonymous] { CloseHandle(anonymous); });
        QVERIFY(!ui::tokenIsThePersons(anonymous, ours));
        HANDLE low = nullptr;
        QVERIFY(DuplicateTokenEx(ours, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, nullptr, SecurityImpersonation, TokenPrimary,
                                 &low) != FALSE);
        const auto closeLow = qScopeGuard([low] { CloseHandle(low); });
        PSID lowLevel = nullptr;
        QVERIFY(ConvertStringSidToSidW(L"S-1-16-4096", &lowLevel) != FALSE);
        TOKEN_MANDATORY_LABEL label{};
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        label.Label.Sid = lowLevel;
        const bool lowered =
            SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(lowLevel)) != FALSE;
        LocalFree(lowLevel);
        QVERIFY(lowered);
        QVERIFY(ui::tokenIsThePersons(low, ours));
        QVERIFY(!ui::tokenRunsAboveThePerson(low));
    }

    // The program that Setup started at its end has Setup's own TEMP and TMP: put back to the person's own at its
    // start, and only those.
    void setupsOwnTempIsPutBackAtTheStart()
    {
        QTemporaryDir folder;
        const std::filesystem::path downloads = (folder.path() + u"/Cache/Downloads"_s).toStdWString();
        const auto variable = [](const wchar_t* name) {
            std::vector<wchar_t> value(32768);
            const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
            return length > 0 && length < value.size() ? std::wstring(value.data(), length) : std::wstring();
        };
        const std::wstring keptTemp = variable(L"TEMP");
        const std::wstring keptTmp = variable(L"TMP");
        const auto putBack = qScopeGuard([&keptTemp, &keptTmp] {
            SetEnvironmentVariableW(L"TEMP", keptTemp.c_str());
            SetEnvironmentVariableW(L"TMP", keptTmp.c_str());
            static_cast<void>(_wputenv_s(L"TEMP", keptTemp.c_str()));
            static_cast<void>(_wputenv_s(L"TMP", keptTmp.c_str()));
        });
        const std::wstring setups = (downloads / L"setup-temp-Ab1234").wstring();
        QVERIFY(SetEnvironmentVariableW(L"TEMP", setups.c_str()) != FALSE);
        QVERIFY(SetEnvironmentVariableW(L"TMP", (setups + L"\\").c_str()) != FALSE);
        QVERIFY(ui::takeBackOwnTemp(downloads));
        QVERIFY(!ui::ownTemp(L"TEMP").empty());
        QVERIFY(variable(L"TEMP") == ui::ownTemp(L"TEMP"));
        QVERIFY(variable(L"TMP") == ui::ownTemp(L"TMP"));
        QCOMPARE(qEnvironmentVariable("TEMP"), QString::fromStdWString(ui::ownTemp(L"TEMP")));

        // Not another folder of Downloads, and not a folder of that name elsewhere.
        for (const std::wstring& other :
             {(downloads / L"update-Ab1234").wstring(),
              (std::filesystem::path(folder.path().toStdWString()) / L"setup-temp-Ab1234").wstring(),
              (downloads / L"setup-temp-Ab12345").wstring()}) {
            QVERIFY(SetEnvironmentVariableW(L"TEMP", other.c_str()) != FALSE);
            QVERIFY(SetEnvironmentVariableW(L"TMP", other.c_str()) != FALSE);
            QVERIFY(!ui::takeBackOwnTemp(downloads));
            QVERIFY(variable(L"TEMP") == other);
        }
    }
    // Setup is given its TEMP as a final path (through no junction, in long names): it is put back all the same where
    // the program's own cache is named otherwise, behind a junction or in short (8.3) names.
    void setupsOwnTempIsPutBackWhereTheCacheIsNamedOtherwise()
    {
        QTemporaryDir folder;
        const QString real = folder.filePath(u"Real Cache Folder"_s);
        QVERIFY(QDir().mkpath(real + u"/Downloads/setup-temp-Ab1234"_s));
        const QString named = folder.filePath(u"Named"_s);
        QProcess making;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"QKVM_TEST_JUNCTION"_s, named);
        environment.insert(u"QKVM_TEST_TARGET"_s, real);
        making.setProcessEnvironment(environment);
        making.start(
            qEnvironmentVariable("SystemRoot") + u"/System32/WindowsPowerShell/v1.0/powershell.exe"_s,
            {u"-NoProfile"_s, u"-NonInteractive"_s, u"-Command"_s,
             u"$ErrorActionPreference='Stop'; New-Item -ItemType Junction -Path $env:QKVM_TEST_JUNCTION -Target $env:QKVM_TEST_TARGET | Out-Null"_s});
        QVERIFY(making.waitForFinished(15000) && making.exitCode() == 0);
        const auto variable = [](const wchar_t* name) {
            std::vector<wchar_t> value(32768);
            const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
            return length > 0 && length < value.size() ? std::wstring(value.data(), length) : std::wstring();
        };
        const std::wstring keptTemp = variable(L"TEMP");
        const std::wstring keptTmp = variable(L"TMP");
        const auto putBack = qScopeGuard([&keptTemp, &keptTmp] {
            SetEnvironmentVariableW(L"TEMP", keptTemp.c_str());
            SetEnvironmentVariableW(L"TMP", keptTmp.c_str());
            static_cast<void>(_wputenv_s(L"TEMP", keptTemp.c_str()));
            static_cast<void>(_wputenv_s(L"TMP", keptTmp.c_str()));
        });
        const std::wstring setups = QDir::toNativeSeparators(real + u"/Downloads/setup-temp-Ab1234"_s).toStdWString();
        const auto putBackFrom = [&](const std::filesystem::path& downloads) {
            SetEnvironmentVariableW(L"TEMP", setups.c_str());
            SetEnvironmentVariableW(L"TMP", setups.c_str());
            return ui::takeBackOwnTemp(downloads) && variable(L"TEMP") == ui::ownTemp(L"TEMP");
        };
        // Behind a junction.
        QVERIFY(putBackFrom((named + u"/Downloads"_s).toStdWString()));
        // In short names, where this volume makes them.
        const std::wstring longName = QDir::toNativeSeparators(real + u"/Downloads"_s).toStdWString();
        std::vector<wchar_t> shortName(32768);
        const DWORD length =
            GetShortPathNameW(longName.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
        if (length > 0 && length < shortName.size() && std::wstring(shortName.data(), length) != longName) {
            QVERIFY(putBackFrom(std::wstring(shortName.data(), length)));
        }
    }
};

QTEST_GUILESS_MAIN(TestUpdatesView)
#include "test_updates_view.moc"
