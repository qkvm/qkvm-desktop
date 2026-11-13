#include "../installer/health_exchange.h"
#include "../installer/native_health.h"
#include "../installer/setup_context.h"
#include "../installer/setup_launch.h"
#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QtTest>
#include <cstdio>
#include <shellapi.h>
#include <winrt/base.h>
using namespace qkvm::installer;
using namespace Qt::StringLiterals;
namespace {
DWORD seenPolicy = 0;
DWORD seenFlags = 0;
bool sawAttributes = false;
std::wstring seenCommand;
BOOL WINAPI seePolicy(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags, DWORD_PTR attribute, PVOID value, SIZE_T size,
                      PVOID previous, PSIZE_T returned)
{
    if (attribute == PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY && size == sizeof(DWORD))
        seenPolicy = *static_cast<DWORD*>(value);
    return UpdateProcThreadAttribute(list, flags, attribute, value, size, previous, returned);
}
BOOL WINAPI seeCreate(LPCWSTR, LPWSTR command, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL inherits, DWORD flags,
                      LPVOID, LPCWSTR, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION)
{
    seenFlags = flags;
    seenCommand = command;
    sawAttributes = !inherits && startup->cb == sizeof(STARTUPINFOEXW) &&
                    reinterpret_cast<STARTUPINFOEXW*>(startup)->lpAttributeList != nullptr;
    SetLastError(ERROR_CANCELLED);
    return FALSE; // No deployment helper is started by a policy probe.
}
const Package candidate{L"synthetic.msix", L"", 2, 1};
QString self()
{
    return QCoreApplication::applicationFilePath().replace(u'/', u'\\');
}
NativeHealthApi backend(QProcess& child, const QString& mode)
{
    NativeHealthApi api;
    api.registered = [] { return HealthRegistration{candidate, self().toStdWString()}; };
    api.activate = [&child, mode](LPCWSTR app, LPCWSTR args, ACTIVATEOPTIONS options, DWORD* pid) {
        if (std::wstring(app) != L"!App" || options != AO_NOERRORUI)
            return E_INVALIDARG;
        const QStringList arguments = QString::fromWCharArray(args).split(u' ');
        if (arguments.size() != 3 || arguments[0] != u"--check-package" || arguments[1] != u"--health-check")
            return E_INVALIDARG;
        child.start(self(), {u"--health-child"_s, arguments[2], mode, u"1000"_s});
        if (!child.waitForStarted(30000))
            return E_FAIL;
        // Activated once it runs its own code (it says "up" first): on the loaded slow cores Windows took up to 20 s
        // to bring this program up, with under 0.1 s of work, and refused to end it meanwhile ("access denied"). The
        // backend's waits are about the child, and count from here.
        if (!child.waitForReadyRead(60000) || child.readLine().trimmed() != "up")
            return E_FAIL;
        *pid = static_cast<DWORD>(child.processId());
        return S_OK;
    };
    // This process is deliberately unpackaged. Substitute only the Windows family
    // lookup; process handles, executable path, creation time and waits stay real.
    api.family = [](HANDLE, UINT32* size, PWSTR family) -> LONG {
        if (*size < 1)
            return ERROR_INSUFFICIENT_BUFFER;
        family[0] = L'\0';
        *size = 1;
        return ERROR_SUCCESS;
    };
    // (Short where the case is the wait running out; otherwise as long as a loaded PC takes, which is not what is
    // looked at.)
    api.replyTimeoutMs = mode == u"silent" ? 700 : 60000;
    api.exitTimeoutMs = mode == u"hang" ? 300 : 60000;
    return api;
}
void finish(QProcess& child)
{
    if (!child.waitForFinished(1500) && child.state() != QProcess::NotRunning) {
        child.kill();
        static_cast<void>(child.waitForFinished(30000));
    }
}
} // namespace
class TestInstallerBackend : public QObject {
    Q_OBJECT
private slots:
    void aDeploymentErrorAfterWindowsCommittedDoesNotSpoilSuccess_data()
    {
        QTest::addColumn<bool>("rollback");
        QTest::newRow("install") << false;
        QTest::newRow("rollback") << true;
    }
    void aDeploymentErrorAfterWindowsCommittedDoesNotSpoilSuccess()
    {
        QFETCH(bool, rollback);
        const Package older{L"old.msix", L"synthetic", 1, 1};
        const Package newer{L"new.msix", L"synthetic", 2, 1};
        History history{rollback ? newer : older, rollback ? std::optional(older) : std::nullopt, std::nullopt};
        auto registered = history.current;
        std::wstring deployProblem;
        const Backend transactions{[](const Package&) { return true; }, [&] { return registered; },
                                   [](const History&) { return true; },
                                   [&](const Package& package, bool) {
                                       registered = package; // Windows committed although its API returned an error.
                                       deployProblem = L"synthetic deploy error 0x80070005";
                                       return false;
                                   },
                                   [](const Package&) { return true; }};
        const auto result = apply(history, rollback ? older : newer, rollback, transactions);
        QCOMPARE(result, rollback ? Result::RolledBack : Result::Installed);
        const auto message = QString::fromStdWString(resultMessage(result, L"1.0.0", deployProblem, {}));
        QVERIFY2(!message.contains(u"Windows:") && !message.contains(u"0x80070005"), qPrintable(message));
        QVERIFY(message.contains(u"startup check passed"));
    }
    void resultMessagesKeepOnlyRelevantDiagnostics()
    {
        for (const auto result : {Result::KeptCurrent, Result::Refused, Result::RecoveryNeeded}) {
            const auto message = resultMessage(result, L"1.0.0", L"synthetic deploy error", L"diagnostic 0x00000011");
            QVERIFY(message.find(L"synthetic deploy error") != std::wstring::npos);
            QVERIFY(message.find(L"diagnostic 0x00000011") != std::wstring::npos);
        }
        const auto putBack =
            resultMessage(Result::PutBack, L"1.0.0", L"synthetic deploy error", L"diagnostic 0x00000011");
        QVERIFY(putBack.find(L"synthetic deploy error") == std::wstring::npos);
        QVERIFY(putBack.find(L"diagnostic 0x00000011") != std::wstring::npos);
        const auto installed = resultMessage(Result::Installed, L"1.0.0", {}, L"stale diagnostic");
        QVERIFY(installed.find(L"stale diagnostic") == std::wstring::npos);
    }
    // L-2. Each result says which version runs now, a rollback as asked is told apart from a version
    // put back after a failed check, and the startup check's own words are not prefixed twice.
    void resultMessagesSayWhatRunsNow()
    {
        const std::wstring failedCheck =
            L"Startup check failed (diagnostic 0x00000011). No user session was terminated.";
        const auto installed = QString::fromStdWString(resultMessage(Result::Installed, L"2.0.0", {}, {}));
        QVERIFY2(installed.contains(u"2.0.0"), qPrintable(installed));
        const auto rolledBack = QString::fromStdWString(resultMessage(Result::RolledBack, L"1.0.0", {}, {}));
        QVERIFY2(rolledBack.contains(u"1.0.0") && rolledBack.contains(u"as asked"), qPrintable(rolledBack));
        const auto putBack = QString::fromStdWString(resultMessage(Result::PutBack, L"2.0.0", {}, failedCheck));
        QVERIFY2(putBack.contains(u"2.0.0") && putBack.contains(u"did not pass"), qPrintable(putBack));
        QVERIFY2(!putBack.contains(u"startup check passed. Open"), qPrintable(putBack));
        QVERIFY2(!putBack.contains(u"Startup check: Startup check"), qPrintable(putBack));
        QVERIFY2(putBack.contains(u"diagnostic 0x00000011"), qPrintable(putBack));
        const auto kept = QString::fromStdWString(resultMessage(Result::KeptCurrent, L"1.0.0", {}, {}));
        QVERIFY2(kept.contains(u"1.0.0") && kept.contains(u"was kept"), qPrintable(kept));
    }
    void registrationFailureDuringRecoveryRetainsTheJournal()
    {
        const Package current{L"current.msix", L"", 1, 1};
        History history{current, std::nullopt, candidate};
        const History before = history;
        bool escaped = false;
        int saves = 0;
        Result result = Result::Installed;
        const Backend transactions{[](const Package&) { return true; },
                                   []() -> std::optional<Package> { throw winrt::hresult_error(E_ACCESSDENIED); },
                                   [&](const History&) {
                                       ++saves;
                                       return true;
                                   },
                                   [](const Package&, bool) { return true; }, [](const Package&) { return true; }};
        try {
            result = recover(history, transactions);
        } catch (...) {
            escaped = true;
        }
        QVERIFY(!escaped);
        QCOMPARE(result, Result::RecoveryNeeded);
        QVERIFY(history == before);
        QCOMPARE(saves, 0);
    }
    void theRealTokenAndPackageQueriesAnswerForThisProcess()
    {
        bool elevated = true;
        QCOMPARE(processElevation(elevated), DWORD(ERROR_SUCCESS));
        QCOMPARE(processPackageStatus(), LONG(APPMODEL_ERROR_NO_PACKAGE));
        QCOMPARE(setupContextProblem().empty(), !elevated); // elevated run: must be refused
    }
    void relayArgumentsSurviveQuotingExactly_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("version");
        QTest::newRow("spaces") << u"C:\\Test area\\Sample folder\\q kvm.msix"_s << u"0.2.0"_s;
        QTest::newRow("trailing backslashes") << u"C:\\x\\\\"_s << u"0.2.0\\"_s;
        QTest::newRow("quote injection") << u"C:\\a\" --rollback \"b.msix"_s << u"0.2.0\" \"--recover"_s;
        QTest::newRow("backslash quote") << u"C:\\a\\\\\"b"_s << u"\\\"\\\\\""_s;
        QTest::newRow("tab and newline") << u"C:\\a\tb\nc.msix"_s << u"0.2.0 extra"_s;
    }
    void relayArgumentsSurviveQuotingExactly()
    {
        QFETCH(QString, path);
        QFETCH(QString, version);
        const SetupLaunchApi api{seeCreate, seePolicy};
        const std::wstring exe = LR"(C:\Program Files\q kvm\qkvm-setup.exe)";
        const std::vector<std::wstring> sent{L"--install", path.toStdWString(), L"--version", version.toStdWString()};
        for (const auto hop : {SetupHop::Relay, SetupHop::Installer}) {
            seenCommand.clear();
            QCOMPARE(launchSetup(exe, sent, hop, false, api), DWORD(ERROR_CANCELLED));
            int count = 0;
            LPWSTR* argv = CommandLineToArgvW(seenCommand.c_str(), &count);
            QVERIFY(argv != nullptr);
            const auto release = qScopeGuard([&] { LocalFree(static_cast<void*>(argv)); });
            const std::vector<std::wstring> got(argv, argv + count);
            std::vector<std::wstring> expected{exe};
            if (hop == SetupHop::Relay)
                expected.push_back(L"--relay");
            expected.insert(expected.end(), sent.begin(), sent.end());
            QVERIFY2(got == expected, qPrintable(QString::fromStdWString(seenCommand)));
        }
    }
    void launcherSetsPolicyOnTheRelayAndStartsSetupAsItsChild()
    {
        const SetupLaunchApi api{seeCreate, seePolicy};
        seenPolicy = 0;
        QCOMPARE(launchSetup(self().toStdWString(), {L"--rollback"}, SetupHop::Relay, false, api),
                 DWORD(ERROR_CANCELLED));
        QCOMPARE(seenPolicy, DWORD(PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE));
        QVERIFY((seenFlags & EXTENDED_STARTUPINFO_PRESENT) != 0);
        QVERIFY(sawAttributes);
        QVERIFY(seenCommand.find(L" --relay ") != std::wstring::npos);
        seenPolicy = 0;
        QCOMPARE(launchSetup(self().toStdWString(), {L"--rollback"}, SetupHop::Installer, false, api),
                 DWORD(ERROR_CANCELLED));
        QCOMPARE(seenPolicy, 0UL);
        QVERIFY((seenFlags & EXTENDED_STARTUPINFO_PRESENT) == 0);
        QVERIFY(seenCommand.find(L"--relay") == std::wstring::npos);
        const auto previous = seenCommand;
        QCOMPARE(launchSetup(self().toStdWString(), {L"--arbitrary"}, SetupHop::Relay, false, api),
                 DWORD(ERROR_INVALID_PARAMETER));
        QCOMPARE(seenCommand, previous);
    }
    void setupRefusesPackageIdentityAndAnUnknownPackageState_data()
    {
        QTest::addColumn<long>("status");
        QTest::newRow("has package") << long(ERROR_INSUFFICIENT_BUFFER);
        QTest::newRow("success") << long(ERROR_SUCCESS);
        QTest::newRow("query error") << long(ERROR_ACCESS_DENIED);
    }
    void setupRefusesPackageIdentityAndAnUnknownPackageState()
    {
        QFETCH(long, status);
        SetupContextApi api;
        api.packageStatus = [=] { return status; };
        api.elevation = [](bool& value) {
            value = false;
            return DWORD(ERROR_SUCCESS);
        };
        QVERIFY(!setupContextProblem(api).empty());
    }
    void anUnverifiedOrPreexistingProcessIsNeverStopped_data()
    {
        QTest::addColumn<int>("wrong");
        QTest::newRow("family") << 0;
        QTest::newRow("image") << 1;
        QTest::newRow("creation before activation") << 2;
    }
    void anUnverifiedOrPreexistingProcessIsNeverStopped()
    {
        QFETCH(int, wrong);
        QProcess child;
        const auto cleanup = qScopeGuard([&] { finish(child); });
        auto api = backend(child, u"silent"_s);
        if (wrong == 0)
            api.family = [](HANDLE, UINT32*, PWSTR family) -> LONG {
                family[0] = L'x';
                family[1] = L'\0';
                return ERROR_SUCCESS;
            };
        if (wrong == 1)
            api.registered = [] { return HealthRegistration{candidate, L"C:\\not-the-check.exe"}; };
        if (wrong == 2) {
            child.start(self(), {u"--health-child"_s, u"unused"_s, u"silent"_s, u"0"_s});
            QVERIFY(child.waitForStarted(3000));
            Sleep(30);
            api.activate = [&](LPCWSTR, LPCWSTR, ACTIVATEOPTIONS, DWORD* pid) {
                *pid = DWORD(child.processId());
                return S_OK;
            };
        }
        std::wstring problem;
        QVERIFY(!healthy(candidate, problem, api));
        QVERIFY(!child.waitForFinished(30));
        QCOMPARE(child.state(), QProcess::Running);
        QVERIFY2(QString::fromStdWString(problem).contains(u"process identity 0x0000000D"),
                 qPrintable(QString::fromStdWString(problem)));
    }
    void registrationMismatchAndMissingPidNeverStartAHealthWait_data()
    {
        QTest::addColumn<int>("bad");
        QTest::newRow("wrong registration") << 0;
        QTest::newRow("zero PID") << 1;
        QTest::newRow("unopenable PID") << 2;
    }
    void registrationMismatchAndMissingPidNeverStartAHealthWait()
    {
        QFETCH(int, bad);
        QProcess child;
        auto api = backend(child, u"ok"_s);
        bool activated = false;
        api.activate = [&](LPCWSTR, LPCWSTR, ACTIVATEOPTIONS, DWORD* pid) {
            activated = true;
            *pid = bad == 2 ? 0xFFFFFFFF : 0;
            return S_OK;
        };
        if (bad == 0)
            api.registered = [] {
                auto wrong = candidate;
                ++wrong.version;
                return HealthRegistration{wrong, self().toStdWString()};
            };
        std::wstring problem;
        QVERIFY(!healthy(candidate, problem, api));
        QCOMPARE(activated, bad != 0);
        QVERIFY(!problem.empty());
        QCOMPARE(child.state(), QProcess::NotRunning);
    }
    void recoveryKeepsThePendingJournalWhenHealthThrows()
    {
        QProcess child;
        auto api = backend(child, u"ok"_s);
        api.registered = []() -> HealthRegistration { throw winrt::hresult_error(E_ACCESSDENIED); };
        History history{std::nullopt, std::nullopt, candidate};
        int saves = 0;
        std::wstring problem;
        const Backend transactions{[](const Package&) { return true; },
                                   [] { return std::optional<Package>(candidate); },
                                   [&](const History&) {
                                       ++saves;
                                       return true;
                                   },
                                   [](const Package&, bool) { return false; },
                                   [&](const Package& value) { return healthy(value, problem, api); }};
        QCOMPARE(recover(history, transactions), Result::RecoveryNeeded);
        QCOMPARE(saves, 0);
        QVERIFY(history.pending.has_value());
    }
    void setupRefusesElevatedAndUnknownContexts_data()
    {
        QTest::addColumn<bool>("elevated");
        QTest::addColumn<uint>("error");
        QTest::addColumn<bool>("refused");
        QTest::newRow("ordinary") << false << 0U << false;
        QTest::newRow("administrator") << true << 0U << true;
        QTest::newRow("unreadable token") << false << uint(ERROR_ACCESS_DENIED) << true;
    }
    void setupRefusesElevatedAndUnknownContexts()
    {
        QFETCH(bool, elevated);
        QFETCH(uint, error);
        QFETCH(bool, refused);
        SetupContextApi api;
        api.packageStatus = [] { return APPMODEL_ERROR_NO_PACKAGE; };
        api.elevation = [=](bool& value) {
            value = elevated;
            return DWORD(error);
        };
        QCOMPARE(!setupContextProblem(api).empty(), refused);
    }
    void diagnosticAndProcessResultsHaveTheirOwnCodes_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("expected");
        QTest::newRow("healthy") << u"ok"_s << QString();
        QTest::newRow("diagnostic") << u"diagnostic"_s << u"diagnostic 0x00000011"_s;
        QTest::newRow("nonzero exit after reply") << u"exit"_s << u"process exit 0x00000017"_s;
        QTest::newRow("zero exit without reply") << u"exit-zero"_s << u"process exit 0x00000000"_s;
        QTest::newRow("early crash") << u"crash"_s << u"process exit 0xC0000123"_s;
        QTest::newRow("missing reply") << u"silent"_s << u"Win32 0x000005B4"_s;
        QTest::newRow("reply but no exit") << u"hang"_s << u"Win32 0x000005B4"_s;
    }
    void diagnosticAndProcessResultsHaveTheirOwnCodes()
    {
        QFETCH(QString, mode);
        QFETCH(QString, expected);
        QProcess child;
        const auto cleanup = qScopeGuard([&] { finish(child); });
        auto api = backend(child, mode);
        std::wstring problem;
        // The child, to be asked when it ended. (QProcess has its own, and keeps the process there until then.)
        HANDLE watched = nullptr;
        const auto unwatch = qScopeGuard([&] {
            if (watched != nullptr)
                CloseHandle(watched);
        });
        const auto activate = api.activate;
        api.activate = [&](LPCWSTR app, LPCWSTR args, ACTIVATEOPTIONS options, DWORD* pid) {
            const auto result = activate(app, args, options, pid);
            if (SUCCEEDED(result))
                watched = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid);
            return result;
        };
        const bool good = healthy(candidate, problem, api);
        FILETIME returned{};
        GetSystemTimePreciseAsFileTime(&returned);
        QCOMPARE(good, expected.isEmpty());
        if (!expected.isEmpty())
            QVERIFY2(QString::fromStdWString(problem).contains(expected), qPrintable(QString::fromStdWString(problem)));
        if (mode == u"crash" || mode == u"exit-zero") {
            // Its end is seen as it comes, not once the reply's wait has run out: from the child's own end, as Windows
            // recorded it. (From the activation, the old measure, a loaded PC's slow start of the child came in too.)
            FILETIME created{}, ended{}, kernel{}, user{};
            QVERIFY(watched != nullptr && GetProcessTimes(watched, &created, &ended, &kernel, &user));
            const auto at = [](const FILETIME& time) {
                return static_cast<qint64>((ULONGLONG(time.dwHighDateTime) << 32) | time.dwLowDateTime);
            };
            QVERIFY(at(ended) != 0);
            const qint64 tookMs = (at(returned) - at(ended)) / 10000;
            QVERIFY2(
                tookMs < 5000,
                qPrintable(u"Exit detection took %1 ms; must be below 5000 ms, versus the 60000 ms reply wait."_s.arg(
                    tookMs)));
        }
        if (mode == u"silent" || mode == u"hang") {
            // Ended by the backend, and by nothing else: with ERROR_TIMEOUT, its code for a child it stopped (the
            // child sleeps a minute otherwise). Waited for as long as Windows takes to end it.
            QVERIFY(child.waitForFinished(30000));
            QCOMPARE(child.exitCode(), int(ERROR_TIMEOUT));
        }
    }
    void activationErrorsAreLabelledAndNeverClaimSuccess()
    {
        QProcess child;
        auto api = backend(child, u"ok"_s);
        api.activate = [](LPCWSTR, LPCWSTR, ACTIVATEOPTIONS, DWORD*) { return E_ACCESSDENIED; };
        std::wstring problem;
        QVERIFY(!healthy(candidate, problem, api));
        QVERIFY(QString::fromStdWString(problem).contains(u"HRESULT 0x80070005"));
        QCOMPARE(child.state(), QProcess::NotRunning);
    }
    void healthExceptionsBecomeRecoverableFailures_data()
    {
        QTest::addColumn<bool>("hresult");
        QTest::newRow("Windows") << true;
        QTest::newRow("standard") << false;
    }
    void healthExceptionsBecomeRecoverableFailures()
    {
        QFETCH(bool, hresult);
        QProcess child;
        auto api = backend(child, u"ok"_s);
        api.registered = [=]() -> HealthRegistration {
            if (hresult)
                throw winrt::hresult_error(E_ACCESSDENIED);
            throw std::runtime_error("synthetic registration failure");
        };
        bool escaped = false;
        bool result = true;
        std::wstring problem;
        try {
            result = healthy(candidate, problem, api);
        } catch (...) {
            escaped = true;
        }
        QVERIFY(!escaped);
        QVERIFY(!result);
        QVERIFY(!problem.empty());
        if (hresult)
            QVERIFY(QString::fromStdWString(problem).contains(u"HRESULT 0x80070005"));
    }
    void aRestoredUpdateKeepsTheCandidatesLabelledCode()
    {
        // Same health diagnostic as installer/main.cpp, separate from deployment errors,
        // and recover() called with the candidate journalled and registered (what apply() does
        // right after AddPackageAsync).
        const Package current{L"current.msix", L"", 1, 1};
        History history{current, std::nullopt, candidate};
        std::optional<Package> registered = candidate;
        std::wstring problem;
        QProcess candidateChild;
        QProcess currentChild;
        const auto cleanup = qScopeGuard([&] {
            finish(candidateChild);
            finish(currentChild);
        });
        const auto candidateApi = backend(candidateChild, u"diagnostic"_s); // replies 17, exits 0
        auto currentApi = backend(currentChild, u"ok"_s);
        currentApi.registered = [&] { return HealthRegistration{current, self().toStdWString()}; };
        const Backend transactions{
            [](const Package&) { return true; }, [&] { return registered; }, [](const History&) { return true; },
            [&](const Package& package, bool) {
                registered = package;
                return true;
            },
            [&](const Package& package) {
                return healthy(package, problem, package == candidate ? candidateApi : currentApi);
            }};
        const auto result = recover(history, transactions);
        QCOMPARE(result, Result::PutBack);
        const auto message = resultMessage(result, L"1.0.0", L"stale deployment error", problem);
        QVERIFY(message.find(L"stale deployment error") == std::wstring::npos);
        QVERIFY2(QString::fromStdWString(message).contains(u"diagnostic 0x00000011"),
                 qPrintable(u"Setup's final dialog would add: ["_s + QString::fromStdWString(problem) + u"]"_s));
        // L-2: the check's own words are not prefixed twice, and nothing says the record kept a
        // pending installation that the put-back has cleared.
        QVERIFY2(message.find(L"Startup check: Startup check") == std::wstring::npos,
                 qPrintable(QString::fromStdWString(message)));
        QVERIFY2(message.find(L"installation record was kept") == std::wstring::npos,
                 qPrintable(QString::fromStdWString(message)));
        QVERIFY(!history.pending);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 5 && args[1] == u"--health-child") {
        // Up, in its own code: what the test's activation waits for (backend()).
        std::puts("up");
        std::fflush(stdout);
        // Deliberately slow startup: the health deadline must not classify this as failure.
        Sleep(args[4].toULong());
        const auto& mode = args[3];
        if (mode == u"crash") {
            Sleep(100);
            ExitProcess(0xC0000123);
        }
        if (mode == u"exit-zero") {
            Sleep(100);
            return 0;
        }
        if (mode == u"silent") {
            Sleep(60000);
            return 2;
        }
        if (!reportHealth(args[2].toStdWString(), mode == u"diagnostic" ? 17U : 0U))
            return 3;
        if (mode == u"hang") {
            Sleep(60000);
            return 4;
        }
        return mode == u"exit" ? 23 : 0;
    }
    TestInstallerBackend test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_installer_backend.moc"