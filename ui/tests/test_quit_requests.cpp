#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QScopeGuard>
#include <QStringList>
#include <QTest>

#include "win/quit_requests.h"

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <aclapi.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// What the program said through Qt (its qWarning), while a test listens.
QStringList said;
void hearing(QtMsgType /*type*/, const QMessageLogContext& /*where*/, const QString& text)
{
    said.append(text);
}

} // namespace

// qKVM's Setup asking an installed copy to end: under names of the test's own, never the contract's (a request to those
// would end whatever installed qKVM runs as this user). Setup is played by this test: it holds its mutex while it runs
// (from each case's start), and asks as Setup does: it makes its event of a request that stands and keeps it, then
// another thread opens the request's event with the one right of setting it, sets it, and lets it go. Giving up, it
// lets go of the event of a request that stands.
class TestQuitRequests : public QObject {
    Q_OBJECT

private:
    int m_made = 0;
    QString m_event;
    QString m_window;
    QString m_wanted;
    QString m_setup;
    HANDLE m_setupRuns = nullptr;
    HANDLE m_stands = nullptr;
    // How long a request is honoured here, and a stall of the interface well past it.
    static constexpr std::chrono::milliseconds fresh{1'000};
    static constexpr int stallMs = 2'500;

    // Names no other test and no other run of this one has, in this session's names as the contract's.
    void newNames()
    {
        ++m_made;
        const qint64 pid = QCoreApplication::applicationPid();
        m_event = u"Local\\qKVM-Desktop-Quit test %1 %2"_s.arg(pid).arg(m_made);
        m_window = u"qKVM Desktop quit answer test %1 %2"_s.arg(pid).arg(m_made);
        m_wanted = u"Local\\qKVM-Desktop-Quit-Wanted test %1 %2"_s.arg(pid).arg(m_made);
        m_setup = u"Local\\qKVM-Desktop-Setup test %1 %2"_s.arg(pid).arg(m_made);
    }

    ui::QuitNames names() const { return {m_event, m_window, m_wanted, m_setup}; }
    static ui::QuitPace pace() { return ui::QuitPace{fresh}; }

    // Setup gives up on its request (Cancel, or a silent install that changed nothing), as packaging/qkvm-desktop.iss
    // does: it takes its ticket back, if it is still there, and lets the event go. Whether the program had taken it
    // first (it then ends, and Setup waits for it as for one that ends; review R1).
    bool withdraw()
    {
        bool taken = false;
        if (m_stands != nullptr) {
            taken = WaitForSingleObject(m_stands, 0) != WAIT_OBJECT_0;
            CloseHandle(m_stands);
            m_stands = nullptr;
        }
        return taken;
    }

    // Setup ends.
    void setupEnds()
    {
        static_cast<void>(withdraw());
        if (m_setupRuns != nullptr) {
            CloseHandle(m_setupRuns);
            m_setupRuns = nullptr;
        }
    }

    // Done as another program would, on a thread of its own; impersonating `as` where given.
    static void elsewhere(const std::function<void()>& work, HANDLE as = nullptr)
    {
        std::thread other([&] {
            if (as != nullptr && ImpersonateLoggedOnUser(as) == FALSE) {
                return;
            }
            work();
            if (as != nullptr) {
                RevertToSelf();
            }
        });
        other.join();
    }

    // Setup's request: its event of a request that stands made (and kept), then whether the request's event could be
    // opened to be set, and was set; Windows' error where it could not be opened.
    bool ask(const QString& name, HANDLE as = nullptr, DWORD* error = nullptr)
    {
        if (m_stands == nullptr) {
            // (One ticket for each request that stands, made and set once: the program takes it, or Setup takes it
            // back, never both, review R1.)
            const std::wstring wanted = m_wanted.toStdWString();
            m_stands = CreateEventW(nullptr, FALSE, FALSE, wanted.c_str());
            SetEvent(m_stands);
        }
        bool asked = false;
        const std::wstring wide = name.toStdWString();
        elsewhere(
            [&] {
                const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide.c_str());
                if (event == nullptr) {
                    if (error != nullptr) {
                        *error = GetLastError();
                    }
                    return;
                }
                asked = SetEvent(event) != FALSE;
                CloseHandle(event);
            },
            as);
        return asked;
    }

    // The answer windows of the name, of any process, as Setup looks for them.
    static std::vector<HWND> windowsOf(const QString& kind)
    {
        const std::wstring wide = kind.toStdWString();
        std::vector<HWND> found;
        for (HWND at = FindWindowExW(nullptr, nullptr, wide.c_str(), nullptr); at != nullptr;
             at = FindWindowExW(nullptr, at, wide.c_str(), nullptr)) {
            found.push_back(at);
        }
        return found;
    }

    static QString titleOf(HWND window)
    {
        std::wstring text(1024, L'\0');
        const int length = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        return QString::fromWCharArray(text.data(), length);
    }

    // This process's Windows user.
    static std::vector<BYTE> ownUser()
    {
        HANDLE token = nullptr;
        std::vector<BYTE> user;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE) {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            user.resize(size);
            if (GetTokenInformation(token, TokenUser, user.data(), size, &size) == FALSE) {
                user.clear();
            }
            CloseHandle(token);
        }
        return user;
    }

    // This process's own token, as it is; with its user taken out (deny-only, as a token of another user has it not);
    // or at low integrity.
    enum class Token { Own, WithoutTheUser, LowIntegrity };
    static HANDLE tokenOf(Token kind)
    {
        HANDLE own = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, &own) == FALSE) {
            return nullptr;
        }
        HANDLE made = nullptr;
        if (kind == Token::WithoutTheUser) {
            std::vector<BYTE> user = ownUser();
            if (!user.empty()) {
                SID_AND_ATTRIBUTES without{reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, 0};
                if (CreateRestrictedToken(own, 0, 1, &without, 0, nullptr, 0, nullptr, &made) == FALSE) {
                    made = nullptr;
                }
            }
        } else if (DuplicateTokenEx(own, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &made) ==
                   FALSE) {
            made = nullptr;
        } else if (kind == Token::LowIntegrity) {
            SID_IDENTIFIER_AUTHORITY label = SECURITY_MANDATORY_LABEL_AUTHORITY;
            PSID low = nullptr;
            AllocateAndInitializeSid(&label, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &low);
            TOKEN_MANDATORY_LABEL level{{low, SE_GROUP_INTEGRITY}};
            if (low == nullptr ||
                SetTokenInformation(made, TokenIntegrityLevel, &level, sizeof(level) + GetLengthSid(low)) == FALSE) {
                CloseHandle(made);
                made = nullptr;
            }
            if (low != nullptr) {
                FreeSid(low);
            }
        }
        CloseHandle(own);
        return made;
    }

private slots:
    void init()
    {
        newNames();
        const std::wstring setup = m_setup.toStdWString();
        m_setupRuns = CreateMutexW(nullptr, FALSE, setup.c_str());
        QVERIFY(m_setupRuns != nullptr);
    }

    void cleanup() { setupEnds(); }

    void theContractsNamesAreOfThisSession()
    {
        QVERIFY(QString::fromLatin1(ui::quitRequestEvent).startsWith(u"Local\\"_s));
        QCOMPARE(QString::fromLatin1(ui::quitRequestEvent), u"Local\\qKVM-Desktop-Quit"_s);
        QCOMPARE(QString::fromLatin1(ui::quitAnswerWindow), u"qKVM Desktop quit answer"_s);
        QCOMPARE(QString::fromLatin1(ui::quitWantedEvent), u"Local\\qKVM-Desktop-Quit-Wanted"_s);
        // (Honoured for longer than Setup waits for an answer, 10 s and the second it gives after it has given up: a
        // request is refused as too late only once Setup has given up on it; until then it is Setup's withdrawal that
        // says. Review N1.)
        QVERIFY(ui::QuitPace{}.fresh >= std::chrono::milliseconds(11'000));
    }

    // Review N1's scene: the interface busy for 6 s (a wall opening) while Setup still waits, as it does for 10 s: the
    // request still stands, and the program ends the orderly way (it is never refused as too late, which Setup would
    // then, told /FORCECLOSE=1, answer with the forced end).
    void aRequestHeardWhileSetupStillWaitsIsHonoured()
    {
        int ended = 0;
        const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; });
        QVERIFY(ask(m_event));
        QTest::qSleep(6'000);
        QTRY_COMPARE(ended, 1);
        QCOMPARE(requests.answer(), u"1"_s);
    }

    // Review M1's scene: Setup asks; the interface does not answer for longer than a request is honoured; Setup gives
    // up (Cancel, back to its Ready page, its mutex still held; or a silent install that changed nothing); the
    // interface answers again. The program stays, and says that the request came too late; a request that stands is
    // honoured.
    void aRequestThatSetupGaveUpOnLeavesTheProgramRunning()
    {
        int ended = 0;
        const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; }, pace());
        QVERIFY(ask(m_event));
        // (The interface stalls: nothing of this thread runs.)
        QTest::qSleep(stallMs);
        QVERIFY(!withdraw());
        QTRY_VERIFY(requests.answer().startsWith(u"1* "_s));
        QVERIFY(requests.answer().contains(u"too late"_s));
        QTest::qWait(300);
        QCOMPARE(ended, 0);
        QVERIFY(ask(m_event));
        QTRY_COMPARE(ended, 1);
        QCOMPARE(requests.answer(), u"2"_s);
    }

    // When a request came is noted as it comes, not when the interface gets to it: one heard long after it was made is
    // too late, though Setup still runs and still wants it. (A too late answer is marked as one: "<n>*", review N1.)
    void aRequestHeardLongAfterItWasMadeIsTooLate()
    {
        int ended = 0;
        const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; }, pace());
        QVERIFY(ask(m_event));
        QTest::qSleep(stallMs);
        QTRY_VERIFY(requests.answer().startsWith(u"1* "_s));
        QVERIFY(requests.answer().contains(u"too late"_s));
        QCOMPARE(ended, 0);
    }

    // One that Setup withdrew before the interface got to it is too late, however soon that is.
    void aWithdrawnRequestIsTooLate()
    {
        int ended = 0;
        const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; }, pace());
        QVERIFY(ask(m_event));
        QVERIFY(!withdraw());
        QTRY_VERIFY(requests.answer().startsWith(u"1* "_s));
        QVERIFY(requests.answer().contains(u"too late"_s));
        QCOMPARE(ended, 0);
    }

    // Review C4 (the outside review): a withdrawal is Setup's word, not the end of its event. A handle to that
    // event kept elsewhere (any program of this user's may keep one, at low integrity too) keeps it there; the program
    // stays all the same once Setup has withdrawn.
    void aWithdrawalStandsThoughItsEventIsKeptElsewhere()
    {
        int ended = 0;
        const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; }, pace());
        QVERIFY(ask(m_event));
        const std::wstring wanted = m_wanted.toStdWString();
        const HANDLE kept = OpenEventW(SYNCHRONIZE, FALSE, wanted.c_str());
        QVERIFY(kept != nullptr);
        const auto letGo = qScopeGuard([kept] { CloseHandle(kept); });
        QVERIFY(!withdraw());
        QTRY_VERIFY(requests.answer().startsWith(u"1* "_s));
        QTest::qWait(300);
        QCOMPARE(ended, 0);
    }

    // Review R1 (the outside review's confirm round): the program's taking of a request and Setup's withdrawal of it
    // are one thing of Windows' that either does once, Setup's ticket taken: never both, and nothing in between that a
    // stall or a timeout could cut (a lock that Setup gave up waiting for let the program take a request Setup went on
    // without). Setup first: withdrawn, and the program stays; the program first: it ends, and Setup finds the ticket
    // gone, and waits for it as for one that ends. And so in rounds where the two come at the same time.
    void aRequestIsTakenOrWithdrawnNeverBoth()
    {
        {
            int ended = 0;
            const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; }, pace());
            QVERIFY(ask(m_event));
            QVERIFY(!withdraw());
            QTRY_VERIFY(requests.answer().startsWith(u"1* "_s));
            QCOMPARE(ended, 0);
        }
        setupEnds();
        newNames();
        m_setupRuns = CreateMutexW(nullptr, FALSE, m_setup.toStdWString().c_str());
        {
            int ended = 0;
            const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; }, pace());
            QVERIFY(ask(m_event));
            QTRY_COMPARE(ended, 1);
            QVERIFY(withdraw());
        }
        for (int round = 0; round < 20; ++round) {
            setupEnds();
            newNames();
            m_setupRuns = CreateMutexW(nullptr, FALSE, m_setup.toStdWString().c_str());
            int ended = 0;
            const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; }, pace());
            QVERIFY(ask(m_event));
            std::atomic<bool> taken{false};
            std::thread setup([this, &taken, round] {
                std::this_thread::sleep_for(std::chrono::milliseconds(round % 10));
                taken = withdraw();
            });
            const auto joined = qScopeGuard([&setup] {
                if (setup.joinable()) {
                    setup.join();
                }
            });
            QTest::qWait(150);
            setup.join();
            QTest::qWait(50);
            QVERIFY2((ended == 1) == taken.load(),
                     qPrintable(u"round %1: ended %2, taken %3"_s.arg(round).arg(ended).arg(taken.load())));
        }
    }

    // And one where no Setup runs (none holds its mutex) is no Setup's: too late as well.
    void aRequestWithNoSetupRunningIsTooLate()
    {
        int ended = 0;
        const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; }, pace());
        CloseHandle(m_setupRuns);
        m_setupRuns = nullptr;
        QVERIFY(ask(m_event));
        QTRY_VERIFY(requests.answer().startsWith(u"1* "_s));
        QVERIFY(requests.answer().contains(u"too late"_s));
        QCOMPARE(ended, 0);
    }

    void aRequestEndsTheProgramWhereNothingHoldsIt()
    {
        int ended = 0;
        const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; });
        QVERIFY(requests.listens());
        QCOMPARE(requests.answer(), u"0"_s);
        QVERIFY(ask(m_event));
        QTRY_COMPARE(ended, 1);
        QCOMPARE(requests.answer(), u"1"_s);
        // Once: a request heard while it ends is answered as ending, and ends nothing again.
        QVERIFY(ask(m_event));
        QTRY_COMPARE(requests.answer(), u"2"_s);
        QTest::qWait(100);
        QCOMPARE(ended, 1);
    }

    void whileSomethingHoldsItTheProgramStaysAndSaysWhat()
    {
        int ended = 0;
        QString holds = u"Virtual media are being uploaded or changed in a qKVM window."_s;
        const ui::QuitRequests requests(names(), [&] { return holds; }, [&] { ++ended; });
        QVERIFY(ask(m_event));
        QTRY_COMPARE(requests.answer(), u"1 Virtual media are being uploaded or changed in a qKVM window."_s);
        QTest::qWait(100);
        QCOMPARE(ended, 0);
        // Asked again while it still holds: the program stays, and says so again under the new number.
        holds = u"An action sent to a machine has not been answered yet."_s;
        QVERIFY(ask(m_event));
        QTRY_COMPARE(requests.answer(), u"2 An action sent to a machine has not been answered yet."_s);
        QCOMPARE(ended, 0);
        // And once nothing holds it, the next request ends it.
        holds.clear();
        QVERIFY(ask(m_event));
        QTRY_COMPARE(ended, 1);
        QCOMPARE(requests.answer(), u"3"_s);
    }

    void theWordsAreOneLineAndNotTooLong()
    {
        // (With control signs that are no blank to Qt, made from their numbers, and Windows' line ends and a tab.)
        QString holds =
            u"one"_s + QChar(0x01) + u"two"_s + QChar(0x1B) + u"\r\nthree\t"_s + QChar(0x7F) + QString(1000, u'x');
        const ui::QuitRequests requests(names(), [&] { return holds; }, [] {});
        QVERIFY(ask(m_event));
        QTRY_VERIFY(requests.answer().startsWith(u"1 one two three x"_s));
        const QString words = requests.answer().mid(2);
        QCOMPARE(words.size(), ui::QuitRequests::longestWords);
        QVERIFY(std::ranges::none_of(words, [](QChar sign) { return sign.unicode() < 0x20; }));
    }

    void setupFindsTheAnswerInAWindowOfThisProcessThatNobodySees()
    {
        const ui::QuitRequests requests(names(), [] { return u"held"_s; }, [] {});
        const std::vector<HWND> found = windowsOf(m_window);
        QCOMPARE(found.size(), std::size_t{1});
        DWORD process = 0;
        GetWindowThreadProcessId(found.front(), &process);
        QCOMPARE(process, GetCurrentProcessId());
        QVERIFY(IsWindowVisible(found.front()) == FALSE);
        // With a caption: another program reads the title of a window that has one without asking the window, so that
        // Setup reads it while this program does not answer (GetWindowText, as Windows says it).
        QVERIFY((GetWindowLongW(found.front(), GWL_STYLE) & WS_CAPTION) == WS_CAPTION);
        QVERIFY(GetParent(found.front()) == nullptr);
        QVERIFY(GetWindow(found.front(), GW_OWNER) == nullptr);
        QCOMPARE(titleOf(found.front()), u"0"_s);
        QVERIFY(ask(m_event));
        QTRY_COMPARE(titleOf(found.front()), u"1 held"_s);
    }

    void nobodyElseSetsTheAnswerOrClosesItsWindow()
    {
        const ui::QuitRequests requests(names(), [] { return QString(); }, [] {});
        const std::vector<HWND> found = windowsOf(m_window);
        QCOMPARE(found.size(), std::size_t{1});
        HWND window = found.front();
        // (Sent from here, they reach the window's own procedure just as another program's would: a thread that sent
        // them from elsewhere would wait for this one, which waits for it.)
        SendMessageW(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"7 forged"));
        SetWindowTextW(window, L"8 forged");
        SendMessageW(window, WM_CLOSE, 0, 0);
        SendMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
        PostMessageW(window, WM_CLOSE, 0, 0);
        QTest::qWait(100);
        QVERIFY(IsWindow(window) != FALSE);
        QCOMPARE(titleOf(window), u"0"_s);
        QCOMPARE(requests.answer(), u"0"_s);
    }

    void onlyThisWindowsUserMaySetTheEvent()
    {
        const ui::QuitRequests requests(names(), [] { return QString(); }, [] {});
        QVERIFY(requests.listens());
        const std::wstring name = m_event.toStdWString();
        const HANDLE opened = OpenEventW(READ_CONTROL, FALSE, name.c_str());
        QVERIFY(opened != nullptr);
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const DWORD read = GetSecurityInfo(opened, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                           nullptr, &descriptor);
        CloseHandle(opened);
        QCOMPARE(read, DWORD{ERROR_SUCCESS});
        const auto freed = qScopeGuard([descriptor] { LocalFree(descriptor); });
        // One entry, of nothing inherited, that lets this user alone do anything with it.
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        QVERIFY(GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE);
        QVERIFY((control & SE_DACL_PROTECTED) != 0);
        QVERIFY(dacl != nullptr);
        ACL_SIZE_INFORMATION size{};
        QVERIFY(GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) != FALSE);
        QCOMPARE(size.AceCount, DWORD{1});
        void* entry = nullptr;
        QVERIFY(GetAce(dacl, 0, &entry) != FALSE);
        QCOMPARE(static_cast<ACE_HEADER*>(entry)->AceType, BYTE{ACCESS_ALLOWED_ACE_TYPE});
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        std::vector<BYTE> user = ownUser();
        QVERIFY(!user.empty());
        QVERIFY(EqualSid(&allowed->SidStart, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) != FALSE);
        QVERIFY((allowed->Mask & EVENT_MODIFY_STATE) == EVENT_MODIFY_STATE);
    }

    void neitherAnotherUserNorALowerIntegrityMayAsk()
    {
        int ended = 0;
        const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; });
        QVERIFY(requests.listens());
        for (const Token kind : {Token::WithoutTheUser, Token::LowIntegrity}) {
            const HANDLE token = tokenOf(kind);
            QVERIFY(token != nullptr);
            DWORD error = 0;
            QVERIFY(!ask(m_event, token, &error));
            // (Refused by the event's own rules: not a failed impersonation, which would say another error.)
            QCOMPARE(error, DWORD{ERROR_ACCESS_DENIED});
            CloseHandle(token);
        }
        QTest::qWait(150);
        QCOMPARE(ended, 0);
        QCOMPARE(requests.answer(), u"0"_s);
        // And the same impersonation with this process's own token may: the refusals above are the event's.
        const HANDLE own = tokenOf(Token::Own);
        QVERIFY(own != nullptr);
        QVERIFY(ask(m_event, own));
        CloseHandle(own);
        QTRY_COMPARE(ended, 1);
    }

    void anEventOfTheNameThatIsThereAlreadyIsNeverListenedTo()
    {
        // Made first by somebody else, with the rules Windows gives by default (or any others).
        const std::wstring name = m_event.toStdWString();
        const HANDLE squatter = CreateEventW(nullptr, FALSE, FALSE, name.c_str());
        QVERIFY(squatter != nullptr);
        int ended = 0;
        {
            said.clear();
            const QtMessageHandler before = qInstallMessageHandler(&hearing);
            const ui::QuitRequests requests(names(), [] { return QString(); }, [&] { ++ended; });
            qInstallMessageHandler(before);
            QVERIFY(!requests.listens());
            // Said, so that a copy that Setup cannot ask is known for what it is (review L2); and no window answers.
            QCOMPARE(said.size(), 1);
            QVERIFY(said.front().contains(u"cannot ask this copy of qKVM to close"_s));
            QCOMPARE(windowsOf(m_window).size(), std::size_t{0});
            QVERIFY(SetEvent(squatter) != FALSE);
            QVERIFY(ask(m_event));
            QTest::qWait(200);
            QCOMPARE(ended, 0);
        }
        CloseHandle(squatter);
        // Once it is gone, the name is free again, and a request is heard.
        const ui::QuitRequests later(names(), [] { return QString(); }, [&] { ++ended; });
        QVERIFY(later.listens());
        QVERIFY(ask(m_event));
        QTRY_COMPARE(ended, 1);
    }

    void whenItGoesTheEventAndTheWindowGoWithIt()
    {
        auto requests = std::make_unique<ui::QuitRequests>(names(), [] { return QString(); }, [] {});
        QVERIFY(requests->listens());
        QCOMPARE(windowsOf(m_window).size(), std::size_t{1});
        requests.reset();
        QCOMPARE(windowsOf(m_window).size(), std::size_t{0});
        DWORD error = 0;
        QVERIFY(!ask(m_event, nullptr, &error));
        QCOMPARE(error, DWORD{ERROR_FILE_NOT_FOUND});
        // And another of the same names is made anew, as the next start of the program makes its own.
        const ui::QuitRequests again(names(), [] { return QString(); }, [] {});
        QVERIFY(again.listens());
        QCOMPARE(again.answer(), u"0"_s);
    }
};

QTEST_GUILESS_MAIN(TestQuitRequests)
#include "test_quit_requests.moc"
