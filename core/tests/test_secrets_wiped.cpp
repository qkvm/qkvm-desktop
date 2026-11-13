#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUrl>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/pacing_file.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/wiped.h>

#include "jetkvm_by_hand.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "redfish_device_tests.h"

#ifdef _DEBUG
// Microsoft's C runtime in its debug form, whose heap the test program and Qt's debug libraries share:
// it shows a hook of the program's every block of memory before the block is given back.
#include <crtdbg.h>
// And Windows' own word on which library a return address is in (WhatIsGivenBack, what Qt's network made).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::MemorySecrets;

namespace {

#ifdef _DEBUG
// What is looked for in every block of memory that is given back while one of these is there: a secret, in either
// form the program holds it in, its UTF-16 (a QString's) and its UTF-8. A block that still holds it as it goes was
// given back without being written over, and leaves the secret in memory that nobody holds. (As in the wall's own test
// of what its store is handed, ui/tests/test_own_wall.cpp: the runtime's hook is one for the whole program, and what
// other threads give back meanwhile is looked through too, and holds nothing of a secret this test made for itself.)
class WhatIsGivenBack {
public:
    // `thisThreadOnly`: only what this thread gives back is looked at (OnItsOwnThread); and not what Qt's network
    // library made itself on it (its own copy of a body it is handed, made on this thread to be sent on its own, and
    // given back here where that thread let go of it first: out of reach, SPEC section 8). What the program made, or
    // Qt's core on the program's behalf, is looked at whoever gives it back.
    explicit WhatIsGivenBack(const QString& secret, bool utf8Too = true, bool thisThreadOnly = false)
    {
        Looking& now = looking();
        now.only.store(thisThreadOnly ? std::this_thread::get_id() : std::thread::id());
        now.qtsNetwork = thisThreadOnly ? GetModuleHandleW(L"Qt6Networkd.dll") : nullptr;
        now.passedBy = {GetModuleHandleW(L"ucrtbased.dll"), GetModuleHandleW(L"Qt6Cored.dll")};
        for (std::atomic<long>& one : now.qtsOwn) {
            one.store(0);
        }
        {
            // (Its forms, made and written over before anything is watched: they are this test's, and go before.)
            QByteArray utf16(reinterpret_cast<const char*>(secret.utf16()), secret.size() * 2);
            QByteArray utf8 = utf8Too ? secret.toUtf8() : QByteArray();
            put(now.utf16, now.utf16Size, utf16);
            put(now.utf8, now.utf8Size, utf8);
            core::wipeInPlace(utf16);
            core::wipeInPlace(utf8);
        }
        now.found.store(0);
        now.before = _CrtSetAllocHook(&WhatIsGivenBack::look);
    }
    // Bytes as they are (a key's), on every thread. Nothing is made of them first.
    explicit WhatIsGivenBack(QByteArrayView bytes)
    {
        Looking& now = looking();
        now.only.store(std::thread::id());
        now.qtsNetwork = nullptr;
        now.utf16Size.store(0);
        const qsizetype length = std::min<qsizetype>(bytes.size(), static_cast<qsizetype>(now.utf8.size()));
        std::copy_n(bytes.data(), length, now.utf8.begin());
        now.utf8Size.store(length);
        now.found.store(0);
        now.before = _CrtSetAllocHook(&WhatIsGivenBack::look);
    }
    ~WhatIsGivenBack()
    {
        Looking& now = looking();
        _CrtSetAllocHook(now.before);
        now.utf16Size.store(0);
        now.utf8Size.store(0);
        now.only.store(std::thread::id());
    }
    WhatIsGivenBack(const WhatIsGivenBack&) = delete;
    WhatIsGivenBack& operator=(const WhatIsGivenBack&) = delete;
    WhatIsGivenBack(WhatIsGivenBack&&) = delete;
    WhatIsGivenBack& operator=(WhatIsGivenBack&&) = delete;

    // How many blocks that were given back (or moved, as one that grows is) held it.
    static int holdingIt() { return looking().found.load(); }

private:
    struct Looking {
        std::array<char, 256> utf16{};
        std::atomic<qsizetype> utf16Size{0};
        std::array<char, 256> utf8{};
        std::atomic<qsizetype> utf8Size{0};
        std::atomic<int> found{0};
        std::atomic<std::thread::id> only{}; // none: every thread
        // Qt's network library, where what it makes itself is passed over (none: nothing is), and the libraries a
        // block is made through on the way (the runtime's, Qt's core): the first other one on the stack made it.
        HMODULE qtsNetwork = nullptr;
        std::array<HMODULE, 2> passedBy{};
        // The blocks Qt's network library made, by their numbers (the last so many).
        std::array<std::atomic<long>, 8192> qtsOwn{};
        std::atomic<unsigned> nextOwn{0};
        _CRT_ALLOC_HOOK before = nullptr;
    };
    static Looking& looking()
    {
        static Looking one;
        return one;
    }
    static void put(std::array<char, 256>& to, std::atomic<qsizetype>& size, const QByteArray& bytes)
    {
        const qsizetype length = std::min<qsizetype>(bytes.size(), static_cast<qsizetype>(to.size()));
        std::copy_n(bytes.constData(), length, to.begin());
        size.store(length);
    }
    static bool holds(const char* first, const char* last, const std::array<char, 256>& bytes, qsizetype length)
    {
        return length > 0 && std::search(first, last, bytes.data(), bytes.data() + length) != last;
    }
    // Whether the block being made now is made by Qt's network library itself: the first library on the stack past the
    // runtime's and Qt's core (which make it for whoever calls them) is that one. (Nothing in here makes a block.)
    static bool madeByQtsNetwork(const Looking& now)
    {
        std::array<void*, 24> frames{};
        const USHORT count = CaptureStackBackTrace(1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
        for (USHORT at = 0; at < count; ++at) {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCWSTR>(frames.at(at)), &module)) {
                return false;
            }
            if (std::ranges::find(now.passedBy, module) != now.passedBy.end()) {
                continue;
            }
            return module == now.qtsNetwork;
        }
        return false;
    }
    static bool isQtsOwn(const Looking& now, const void* block, size_t held)
    {
        long number = 0;
        if (now.qtsNetwork == nullptr ||
            _CrtIsMemoryBlock(block, static_cast<unsigned int>(held), &number, nullptr, nullptr) == 0) {
            return false;
        }
        return std::ranges::any_of(now.qtsOwn, [number](const std::atomic<long>& one) { return one.load() == number; });
    }
    static int __cdecl look(int what, void* block, size_t size, int use, long request, const unsigned char* file,
                            int line)
    {
        Looking& now = looking();
        const std::thread::id only = now.only.load();
        const bool watched = only == std::thread::id() || only == std::this_thread::get_id();
        // (Made on any thread: Qt's network thread hands what it made to this one too. And made anew where it grows: a
        // block moved by realloc is another block, of another number.)
        if ((what == _HOOK_ALLOC || what == _HOOK_REALLOC) && now.qtsNetwork != nullptr && madeByQtsNetwork(now)) {
            now.qtsOwn.at(now.nextOwn.fetch_add(1) % now.qtsOwn.size()).store(request);
        }
        if (block != nullptr && (what == _HOOK_FREE || what == _HOOK_REALLOC) && watched &&
            (_BLOCK_TYPE(use) == _NORMAL_BLOCK || _BLOCK_TYPE(use) == _CLIENT_BLOCK)) {
            const size_t held = _msize_dbg(block, _BLOCK_TYPE(use));
            const auto* first = static_cast<const char*>(block);
            const auto* last = first + (held == static_cast<size_t>(-1) ? 0 : held);
            if ((holds(first, last, now.utf16, now.utf16Size.load()) ||
                 holds(first, last, now.utf8, now.utf8Size.load())) &&
                !isQtsOwn(now, block, held)) {
                now.found.fetch_add(1);
            }
        }
        // (And it is given back, or moved, as it was to be.)
        return now.before != nullptr ? now.before(what, block, size, use, request, file, line) : 1;
    }
};
#endif

// A password nothing else has made: on the heap, as one that is read or typed is, and held by nobody but whoever it
// is handed to.
QString aPassword(const char* text)
{
    return QString::fromUtf8(text);
}

const QUrl nowhere(u"http://127.0.0.1:9"_s);

// A stand-in on a thread of its own, made and let go of there (`set`: on that thread, before anything is asked of it).
// What it is sent, and what it makes of that and gives back, is its own, and is not looked at where only this thread's
// is (WhatIsGivenBack). Nothing of it is touched from here but its address.
template <typename Made>
class OnItsOwnThread {
public:
    explicit OnItsOwnThread(const std::function<void(Made&)>& set = {})
    {
        m_there->moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(
            m_there.get(),
            [this, &set] {
                m_made = std::make_unique<Made>();
                if (set) {
                    set(*m_made);
                }
                m_address = m_made->address();
            },
            Qt::BlockingQueuedConnection);
    }
    ~OnItsOwnThread()
    {
        QMetaObject::invokeMethod(m_there.get(), [this] { m_made.reset(); }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    OnItsOwnThread(const OnItsOwnThread&) = delete;
    OnItsOwnThread& operator=(const OnItsOwnThread&) = delete;
    OnItsOwnThread(OnItsOwnThread&&) = delete;
    OnItsOwnThread& operator=(OnItsOwnThread&&) = delete;

    QUrl address() const { return m_address; }

private:
    QThread m_thread;
    std::unique_ptr<QObject> m_there = std::make_unique<QObject>();
    std::unique_ptr<Made> m_made;
    QUrl m_address;
};

// What a call hands back, waited for (20 s at the most).
template <typename Answer>
base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
{
    QList<base::Result<Answer>> answers;
    ask([&answers](base::Result<Answer> answer) { answers.append(std::move(answer)); });
    if (!QTest::qWaitFor([&answers] { return !answers.isEmpty(); }, 20000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

#ifdef _DEBUG
// `use` is handed a password made of `text`, and says whether what it did came to what it was to; what this thread
// gives back meanwhile, and in a turn of the loop after it, is looked through for that password. Where anything held
// it, or `use` says no, that is noted in `wrong` under `road`.
void lookAtWhatIsGivenBack(QStringList& wrong, const QString& road, const char* text,
                           const std::function<bool(QString)>& use)
{
    QString password = aPassword(text);
    // (A copy of its own, which nothing gives back while it is looked at.)
    const QString looked(password.constData(), password.size());
    int found = 0;
    bool came = false;
    {
        const WhatIsGivenBack watching(looked, true, true);
        came = use(std::move(password));
        // (What goes a turn of the loop later: the sign-in's line, its replies, and what Qt was handed with them.)
        QTest::qWait(200);
        found = WhatIsGivenBack::holdingIt();
    }
    if (!came) {
        wrong.append(u"%1: it did not come to what it was to"_s.arg(road));
    }
    if (found > 0) {
        wrong.append(u"%1: %2 blocks held the password as they were given back"_s.arg(road).arg(found));
    }
}

// A store as Windows' is: what it is handed it copies (into a credential of Windows'), and it keeps nothing of what it
// was handed. The first secret it is handed is looked for from then on (WhatIsGivenBack) in what anybody gives back.
class StoreThatWatchesWhatItKeeps final : public core::SecretStore {
public:
    base::Result<void> keep(const QString& name, const QByteArray& secret) override
    {
        if (!watching) {
            watching.emplace(QByteArrayView(secret));
        }
        return held.keep(name, QByteArray(secret.constData(), secret.size()));
    }
    QByteArray kept(const QString& name) override { return held.kept(name); }
    base::Result<std::optional<QByteArray>> read(const QString& name) override { return held.read(name); }
    void forget(const QString& name) override { held.forget(name); }
    base::Result<QStringList> names() override { return held.names(); }
    base::Result<void> erase(const QString& name) override { return held.erase(name); }

    MemorySecrets held;
    std::optional<WhatIsGivenBack> watching;
};
#endif

} // namespace

// From the outside review of the desktop (F5): a device's password, and what is made of it for a sign-in, is written
// over before what held it is given back, at every boundary where it changes hands in the program's own code: what a
// driver holds for as long as it is, the mark the pacing knows a sign-in by, the body a sign-in sends, and what the
// wall hands its drivers. (What Qt's network keeps of a question it was handed to send is Qt's, and is not looked at
// here.)
class TestSecretsWiped : public QObject {
    Q_OBJECT

private slots:
    // (The drivers choose Windows' own TLS, which is refused once TLS is in use: chosen before anything else uses it.)
    void initTestCase() { QVERIFY(core::useWindowsOwnTls()); }

    void aSecretTextIsItsOwnAndIsWrittenOverWhenItGoes()
    {
        QString handed = aPassword("text one 2Ab 9z");
        const QString seen = handed;
        core::SecretText held = core::SecretText::takenFrom(handed);
        // (A copy of its own: the one it was handed is another's, and is left as it was where another holds it too.)
        QVERIFY(handed.isEmpty());
        QCOMPARE(held.view(), QStringView(seen));
        core::SecretText moved = std::move(held);
        // NOLINTNEXTLINE(bugprone-use-after-move): what is moved from is looked at, to hold nothing
        QVERIFY(held.isEmpty());
        QCOMPARE(moved.view(), QStringView(seen));
        QVERIFY(!core::SecretText().view().data());
    }

    void eachDriverWritesItsPasswordOverWhenItGoes()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        QStringList wrong;
        const auto lookAt = [&wrong](const QString& road, const char* text, const std::function<void(QString)>& use) {
            QString password = aPassword(text);
            // (A copy of its own, which nothing gives back while it is looked at.)
            const QString looked(password.constData(), password.size());
            int found = 0;
            {
                const WhatIsGivenBack watching(looked);
                use(std::move(password));
                found = WhatIsGivenBack::holdingIt();
            }
            if (found > 0) {
                wrong.append(u"%1: %2 blocks held the password as they were given back"_s.arg(road).arg(found));
            }
        };
        lookAt(u"a kvmd's driver"_s, "kvmd one 7Qx 4b", [](QString password) {
            core::KvmdDevice::Where where;
            where.address = nowhere;
            const core::KvmdDevice device(where, std::move(password));
        });
        lookAt(u"a JetKVM's driver"_s, "jetkvm one 8Ry 5c", [](QString password) {
            core::JetKvmDevice::Where where;
            where.address = nowhere;
            const core::JetKvmDevice device(where, std::move(password), core::JetKvmDevice::Pace{});
        });
        lookAt(u"a controller's driver"_s, "redfish one 9Sz 6d", [](QString password) {
            core::RedfishDevice::Where where;
            where.address = nowhere;
            const core::RedfishDevice device(where, std::move(password), core::RedfishDevice::Pace{});
        });
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u"; "_s)));
#endif
    }

    void everySignInsBodyIsWrittenOverOnceItsAnswerCame()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // The merged review's MEDIUM 3 (2026-11-06): what a sign-in sends is made once (secretForm, secretJson), and
        // nobody wrote it over: copied into a driver's lambdas and into the asker's, and handed to Qt, its last holder
        // gave it back as it was ("user=admin&passwd=..." in memory nobody holds, after any kvmd's sign-in). Each road
        // really signed in to, its stand-in on a thread of its own; all that this thread gives back is looked at, from
        // the driver's making to its going and to the letting go of its lines and their replies that follows. (What
        // Qt's network makes of a body on a thread of its own is Qt's, and is not looked at: SPEC section 8.)
        QStringList wrong;
        lookAtWhatIsGivenBack(wrong, u"a kvmd's sign-in"_s, "kvmdTwo7Qx4b", [](QString password) {
            const OnItsOwnThread<core::testing::MadeUpKvm> kvm(
                [](core::testing::MadeUpKvm& made) { made.password = u"kvmdTwo7Qx4b"_s; });
            core::KvmdDevice::Where where;
            where.address = kvm.address();
            core::KvmdDevice::Pace pace;
            pace.pacing = std::make_shared<core::SignInPacing>();
            core::KvmdDevice device(where, std::move(password), pace);
            return answerTo<core::KvmdDevice::Identity>([&device](auto done) { device.identify(std::move(done)); })
                .ok();
        });
        lookAtWhatIsGivenBack(wrong, u"a JetKVM's sign-in"_s, "jetkvmTwo8Ry5c", [](QString password) {
            const OnItsOwnThread<core::testing::JetKvmByHand> jet(
                [](core::testing::JetKvmByHand& made) { made.password = u"jetkvmTwo8Ry5c"_s; });
            core::JetKvmDevice::Where where;
            where.address = jet.address();
            core::JetKvmDevice::Pace pace;
            pace.pacing = std::make_shared<core::SignInPacing>();
            core::JetKvmDevice device(where, std::move(password), pace);
            return answerTo<core::JetKvmDevice::Identity>([&device](auto done) { device.identify(std::move(done)); })
                .ok();
        });
        lookAtWhatIsGivenBack(
            wrong, u"a controller's session and its web page's"_s, "redfishTwo9Sz6d", [](QString password) {
                using core::testing::aRoot;
                using core::testing::aSystem;
                const OnItsOwnThread<core::testing::RedfishByHand> bmc([](core::testing::RedfishByHand& made) {
                    made.password = u"redfishTwo9Sz6d"_s;
                    made.beServer(aSystem, aRoot);
                });
                core::RedfishDevice::Where where;
                where.address = bmc.address();
                where.machine = core::testing::markOf(aSystem) + core::testing::digestOf(aRoot);
                core::RedfishDevice::Pace pace = core::testing::quick();
                pace.questionAgainMs = 100;
                core::RedfishDevice device(where, std::move(password), pace);
                // (The way into its console: a session of its own, and its web page signed in to.)
                return answerTo<core::RedfishDevice::ConsoleWay>(
                           [&device](auto done) { device.consoleWay(std::move(done)); })
                    .ok();
            });
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u"; "_s)));
#endif
    }

    void thePasswordTypedIntoABoxIsWrittenOverWhenItEnds()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // The merged review's MEDIUM 3: the Add box and the Change box held what was typed as a QString, cleared and
        // never written over. Each signs in with it, really, and is refused (nothing of it goes onto the wall, whose
        // store is the store's); the box is over, and goes: nothing this thread gave back meanwhile held it.
        QStringList wrong;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::LocalWall wall(settings, secrets, core::KvmdDevice::Pace{});
        const OnItsOwnThread<core::testing::MadeUpKvm> kvm;
        core::OwnKvm kept;
        kept.address = kvm.address().toString();
        kept.name = u"Rack"_s;
        kept.kind = u"kvmd"_s;
        const base::Result<QString> id = wall.add(kept, u"secret"_s);
        QVERIFY(id.ok());
        const OnItsOwnThread<core::testing::MadeUpKvm> another;
        lookAtWhatIsGivenBack(wrong, u"the Add box"_s, "addedTwo3Kp7e", [&wall, &another](QString password) {
            core::KvmAdding adding(wall, core::KvmAdding::Pace{});
            core::KvmAdding::Typed typed;
            typed.address = another.address().toString();
            typed.password = std::move(password);
            const auto added = answerTo<core::KvmAdding::Added>(
                [&adding, &typed](auto done) { adding.add(std::move(typed), std::move(done)); });
            return !added.ok() && added.error().code == "auth_failed";
        });
        lookAtWhatIsGivenBack(wrong, u"the Change box"_s, "changedTwo4Lq8f", [&wall, &id](QString password) {
            core::KvmEditing editing(wall, core::KvmEditing::Pace{});
            core::KvmEditing::Typed typed;
            typed.address = wall.kvms().constFirst().address;
            typed.password = std::move(password);
            QList<core::KvmEditing::Outcome> told;
            editing.change(id.value(), std::move(typed),
                           [&told](const core::KvmEditing::Outcome& outcome) { told.append(outcome); });
            if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 20000)) {
                return false;
            }
            const auto& result = told.constFirst().result;
            return !result.ok() && result.error().code == "auth_failed";
        });
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u"; "_s)));
#endif
    }

    void theKeyOfTheSignInCountsIsWrittenOverWhereItWasCopied()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // The merged review's LOW 7 (2026-11-06): the key the wrong-password counts are sealed under on disk is made at
        // a first start and handed to the store (PacingFile::read), and the copy made for that was given back as it
        // was. Nothing that held it is given back unwritten, through a writing of the file and the file's going, while
        // the store keeps its own.
        const QTemporaryDir folder;
        StoreThatWatchesWhatItKeeps secrets;
        core::SignInPacing pacing;
        {
            core::PacingFile kept(pacing, folder.filePath(u"settings.json.sign-ins"_s), secrets);
            kept.read();
            QVERIFY(kept.keeps());
            QVERIFY(secrets.watching.has_value());
            pacing.saidNo(core::SignInPacing::what(u"login", nowhere, u"admin"_s, u"wrong"_s), {});
            QVERIFY(kept.waitForWritten(5000));
        }
        const int found = WhatIsGivenBack::holdingIt();
        secrets.watching.reset();
        QCOMPARE(found, 0);
        QCOMPARE(secrets.held.all.value(QString::fromLatin1(core::PacingFile::keyName)).size(), 32);
#endif
    }

    void theMarkOfASignInLeavesNothingOfItsPasswordBehind()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        const QString password = aPassword("mark one 3Kp 7e");
        core::SignInPacing::Mark mark;
        {
            const WhatIsGivenBack watching(password);
            mark = core::SignInPacing::what(u"login", nowhere, u"admin"_s, password);
            QCOMPARE(WhatIsGivenBack::holdingIt(), 0);
        }
        // What it is, as ever: a SHA-256 of the four, as UTF-8, a nought between each two.
        const QByteArray said = "login"_ba + '\0' + nowhere.toString(QUrl::FullyEncoded).toUtf8() + '\0' + "admin" +
                                '\0' + password.toUtf8();
        QCOMPARE(mark, QCryptographicHash::hash(said, QCryptographicHash::Sha256));
#endif
    }

    void aSignInsBodyIsMadeOnceWithNothingOfItsPasswordBesideIt()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // (Of signs a form and JSON take as they are: what is looked for is what the body holds.)
        const QString password = aPassword("bodyOne4Lq8f");
        // (What each holds, looked at where nothing is watched: a search may make room of its own for what it seeks.)
        QVERIFY(core::secretForm({{"passwd", password}}).contains(password.toUtf8()));
        QVERIFY(core::secretJson({{"Password", password}}).contains(password.toUtf8()));
        // (Much after it: a body that grew on the way would have left the whole of it behind where it stood before.)
        const QString after(300, u'u');
        int form = 0;
        int json = 0;
        {
            const WhatIsGivenBack watching(password);
            QByteArray made = core::secretForm({{"passwd", password}, {"user", after}});
            core::wipeInPlace(made);
            made = QByteArray();
            form = WhatIsGivenBack::holdingIt();
            made = core::secretJson({{"Password", password}, {"UserName", after}});
            core::wipeInPlace(made);
            made = QByteArray();
            json = WhatIsGivenBack::holdingIt() - form;
        }
        QCOMPARE(form, 0);
        QCOMPARE(json, 0);
#endif
    }

    void aSignInsBodyIsWhatQtWouldHaveMadeOfIt_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("plain") << u"secret"_s;
        QTest::newRow("blanks and signs") << u" another one & = + % / ? # ~ . - _ "_s;
        QTest::newRow("quotes and backslashes") << uR"(say "hi" \ back\slash)"_s;
        QTest::newRow("control characters")
            << u"tab\there\nnew\rret\bback\fform"_s + QChar(0x01) + u"one"_s + QChar(0x1F) + u"end"_s;
        QTest::newRow("letters of other scripts")
            << QString::fromUtf8("gr\xc3\xbc\xc3\x9f \xd0\xbf\xd1\x80\xd0\xb8 \xe4\xbd\xa0\xe5\xa5\xbd");
        QTest::newRow("a sign of two places") << QString::fromUtf8("smile \xf0\x9f\x98\x80 end");
        QTest::newRow("nothing") << QString();
    }

    void aSignInsBodyIsWhatQtWouldHaveMadeOfIt()
    {
        QFETCH(QString, text);
        QCOMPARE(core::secretForm({{"user", u"ad min"}, {"passwd", text}}),
                 "user="_ba + QUrl::toPercentEncoding(u"ad min"_s) + "&passwd="_ba + QUrl::toPercentEncoding(text));
        QCOMPARE(core::secretJson({{"Password", text}, {"UserName", u"ad\"min"}}),
                 QJsonDocument(QJsonObject{{u"UserName"_s, u"ad\"min"_s}, {u"Password"_s, text}})
                     .toJson(QJsonDocument::Compact));
        QCOMPARE(core::secretJson({{"password", text}}),
                 QJsonDocument(QJsonObject{{u"password"_s, text}}).toJson(QJsonDocument::Compact));
        QCOMPARE(core::secretUtf8({u"a", text}).view().toByteArray(), "a"_ba + '\0' + text.toUtf8());
    }

    void aSurrogateByItselfIsLeftOutAsQtLeavesItOut()
    {
        // (As the program sent such a password before its bodies were made so: Qt's UTF-8 of it.)
        const QString alone = u"a"_s + QChar(0xD800) + u"b"_s + QChar(0xDC00);
        QCOMPARE(core::secretUtf8({alone}).view().toByteArray(), alone.toUtf8());
        QCOMPARE(core::secretForm({{"p", alone}}), "p="_ba + QUrl::toPercentEncoding(alone));
        QCOMPARE(core::secretJson({{"p", alone}}),
                 QJsonDocument(QJsonObject{{u"p"_s, alone}}).toJson(QJsonDocument::Compact));
    }

    void whatTheWallHandsItsDriversIsWrittenOverWhenTheyGo()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // A KVM of the wall, asked what it is in a run before, whose kept password the wall reads at its start and
        // hands its driver; and the wall let go of. Of its password the wall made a text of its own (the store's bytes
        // are the store's): nothing of that is left behind in what is given back, by the wall or by the driver.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const QString password = aPassword("wall one 5Mr 9g");
        {
            core::Settings settings(file);
            core::LocalWall wall(settings, secrets, core::KvmdDevice::Pace{});
            core::OwnKvm kvm;
            kvm.address = u"https://127.0.0.1:9"_s;
            kvm.name = u"Rack"_s;
            kvm.knownByCertificate = true;
            kvm.certificate = QByteArray(32, 'c');
            QVERIFY(wall.add(kvm, password).ok());
        }
        {
            const WhatIsGivenBack watching(password, false);
            {
                core::Settings settings(file);
                core::LocalWall wall(settings, secrets, core::KvmdDevice::Pace{});
                QCOMPARE(wall.kvms().size(), 1);
                QVERIFY(wall.wasAskedWhatItIs(wall.kvms().constFirst().id));
            }
            QCOMPARE(WhatIsGivenBack::holdingIt(), 0);
        }
#endif
    }
};

QTEST_GUILESS_MAIN(TestSecretsWiped)
#include "test_secrets_wiped.moc"
