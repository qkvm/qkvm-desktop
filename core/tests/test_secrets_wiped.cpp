#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/pacing_file.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/rfb_screen.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/vnc_auth.h>
#include <qkvm/core/wiped.h>
#include <qkvm/rfb/rfb.h>

#include "jetkvm_by_hand.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "proxmox_by_hand.h"
#include "proxmox_console_by_hand.h"
#include "proxmox_device_tests.h"
#include "redfish_device_tests.h"
#include "rfb_console_by_hand.h"

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
//
// And what was made while it looks is followed until it is given back: every block made by what is looked at is known
// by its number, looked at whichever thread gives it back, and the heap as it stands is looked through for those that
// are held still (heldStill). So a road is over when nothing it made of the secret is held any more, and not after a
// wait: a copy kept past any such wait, or never given back, is found (the outside review of the wave 2 desktop, 6.6:
// a count of what was given back within 300 ms passed both).
class WhatIsGivenBack {
public:
    // `thisThreadOnly`: only what this thread makes and gives back is looked at (OnItsOwnThread's stand-ins make and
    // give back their own); else every thread's but those passed over (passOver). `passOverQtsNetwork`: and not what
    // Qt's network library made itself (its own copies of what a line of this program's sends, the one it hands its
    // thread to send among them, made on this thread, and its TLS's on a line's own: out of reach, SPEC section 8),
    // wherever that is given back. What the program made, or Qt's core on the program's behalf, is looked at whoever
    // gives it back.
    explicit WhatIsGivenBack(const QString& secret, bool utf8Too = true, bool thisThreadOnly = false,
                             bool passOverQtsNetwork = false)
    {
        Looking& now = looking();
        begin(now);
        now.only.store(thisThreadOnly ? std::this_thread::get_id() : std::thread::id());
        for (std::atomic<std::thread::id>& one : now.notThese) {
            one.store(std::thread::id());
        }
        now.qtsNetwork = passOverQtsNetwork ? GetModuleHandleW(L"Qt6Networkd.dll") : nullptr;
        {
            // (Its forms, made and written over before anything is watched: they are this test's, and go before.)
            QByteArray utf16(reinterpret_cast<const char*>(secret.utf16()), secret.size() * 2);
            QByteArray utf8 = utf8Too ? secret.toUtf8() : QByteArray();
            put(now.utf16, now.utf16Size, utf16);
            put(now.utf8, now.utf8Size, utf8);
            core::wipeInPlace(utf16);
            core::wipeInPlace(utf8);
        }
        now.before = _CrtSetAllocHook(&WhatIsGivenBack::look);
    }
    // Bytes as they are (a key's), on every thread. Nothing is made of them first.
    explicit WhatIsGivenBack(QByteArrayView bytes)
    {
        Looking& now = looking();
        begin(now);
        now.only.store(std::thread::id());
        for (std::atomic<std::thread::id>& one : now.notThese) {
            one.store(std::thread::id());
        }
        now.qtsNetwork = nullptr;
        now.utf16Size.store(0);
        const qsizetype length = std::min<qsizetype>(bytes.size(), static_cast<qsizetype>(now.utf8.size()));
        std::copy_n(bytes.data(), length, now.utf8.begin());
        now.utf8Size.store(length);
        now.before = _CrtSetAllocHook(&WhatIsGivenBack::look);
    }
    ~WhatIsGivenBack()
    {
        Looking& now = looking();
        _CrtSetAllocHook(now.before);
        now.utf16Size.store(0);
        now.utf8Size.store(0);
        now.only.store(std::thread::id());
        for (std::atomic<std::thread::id>& one : now.notThese) {
            one.store(std::thread::id());
        }
    }
    WhatIsGivenBack(const WhatIsGivenBack&) = delete;
    WhatIsGivenBack& operator=(const WhatIsGivenBack&) = delete;
    WhatIsGivenBack(WhatIsGivenBack&&) = delete;
    WhatIsGivenBack& operator=(WhatIsGivenBack&&) = delete;

    // How many blocks that were given back (or moved, as one that grows is) held it.
    static int holdingIt() { return looking().found.load(); }
    // What a thread gives back is not looked at (a stand-in's on a thread of its own: what it is sent is its own),
    // where every thread's is: four at the most.
    static void passOver(std::thread::id thread)
    {
        for (std::atomic<std::thread::id>& one : looking().notThese) {
            std::thread::id none;
            if (one.compare_exchange_strong(none, thread)) {
                return;
            }
        }
    }

    // How many blocks made while this looks, by what it looks at (and not by Qt's network itself where that is passed
    // over), hold it still: the heap as it stands, looked through. -1 where that cannot be told: the heap is not laid
    // out as this reads it, or more was made than this follows.
    static int heldStill()
    {
        Looking& now = looking();
        if (!now.laidOut || now.overflowed) {
            return -1;
        }
        // (Looked through from inside the hook, as a block is made on this thread: the runtime holds its heap's lock
        // there, and no thread changes the list of blocks while it is read.)
        now.scanned = -1;
        now.scanBy.store(std::this_thread::get_id());
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): a block of the runtime's own
        void* asking = std::malloc(1);
        std::free(asking); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): given back at once
        now.scanBy.store(std::thread::id());
        return now.scanned;
    }

    // Lets the loop turn until nothing of that is held any more, `ms` at the most: how many are held still then.
    static int heldStillAfter(int ms)
    {
        QElapsedTimer waited;
        waited.start();
        int held = heldStill();
        while (held > 0 && waited.elapsed() < ms) {
            QTest::qWait(20);
            held = heldStill();
        }
        return held;
    }

private:
    // What the debug runtime puts before every block it hands out (debug_heap.cpp's _CrtMemBlockHeader), as it is
    // read here: the blocks in a list, the newest first. Looked at once before each look (begin), and nothing is read
    // by it where it is not so.
    struct BlockHeader {
        const BlockHeader* older;
        const BlockHeader* newer;
        const char* file;
        int line;
        int use;
        size_t size;
        long request;
        unsigned char gap[4];
    };
    // The blocks made while one of these looks that can be followed, by their numbers from the first one of the test
    // program: some 134 million.
    static constexpr long followed = long{1} << 27;
    struct Looking {
        std::array<char, 256> utf16{};
        std::atomic<qsizetype> utf16Size{0};
        std::array<char, 256> utf8{};
        std::atomic<qsizetype> utf8Size{0};
        std::atomic<int> found{0};
        std::atomic<std::thread::id> only{};                    // none: every thread
        std::array<std::atomic<std::thread::id>, 4> notThese{}; // (passOver)
        // Qt's network library, where what it makes itself is passed over (none: nothing is), and the libraries a
        // block is made through on the way (the runtime's, C's and C++'s, Qt's core): the first other one on the stack
        // made it.
        HMODULE qtsNetwork = nullptr;
        std::array<HMODULE, 4> passedBy{};
        // (What follows is read and written in the hook alone, under the runtime's lock, and before and after it.)
        bool laidOut = false;
        bool overflowed = false;
        long origin = -1;  // the number of the first block made while any of these looked
        long first = -1;   // the number of the first block made while this one looks
        long highest = -1; // the highest of this one's that was marked as what it looks at, from the origin
        // By their numbers from the origin: the blocks made by what this one looks at; and those Qt's network made
        // itself, while any of these looked (it may give one back while the next looks: a road ends once nothing of
        // the program's holds the secret, and Qt's line of it may go a moment after). Made outside the runtime's heap,
        // so that nothing of this is a block of it.
        std::uint64_t* mine = nullptr;
        std::uint64_t* qts = nullptr;
        std::atomic<std::thread::id> scanBy{};
        int scanned = -1;
        _CRT_ALLOC_HOOK before = nullptr;
    };
    static Looking& looking()
    {
        static Looking one;
        return one;
    }
    static void begin(Looking& now)
    {
        if (now.mine == nullptr) {
            const size_t bytes = static_cast<size_t>(followed / 8);
            now.mine =
                static_cast<std::uint64_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            now.qts =
                static_cast<std::uint64_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        }
        if (now.highest >= 0 && now.mine != nullptr) {
            const size_t words = static_cast<size_t>(now.highest) / 64 + 1;
            std::fill_n(now.mine, words, std::uint64_t{0});
        }
        now.first = -1;
        now.highest = -1;
        now.overflowed = now.mine == nullptr || now.qts == nullptr;
        now.passedBy = {GetModuleHandleW(L"ucrtbased.dll"), GetModuleHandleW(L"vcruntime140d.dll"),
                        GetModuleHandleW(L"msvcp140d.dll"), GetModuleHandleW(L"Qt6Cored.dll")};
        now.found.store(0);
        // The runtime's heap as this reads it: a block of a size and a number of its own, said so before it.
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): a block of the runtime's own
        void* probe = std::malloc(24);
        long number = 0;
        const bool known = _CrtIsMemoryBlock(probe, 24, &number, nullptr, nullptr) != 0;
        const BlockHeader* header = static_cast<const BlockHeader*>(probe) - 1;
        now.laidOut =
            known && header->size == 24 && header->request == number && _BLOCK_TYPE(header->use) == _NORMAL_BLOCK;
        std::free(probe); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): the block above
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
    static bool holdsIt(const Looking& now, const char* first, const char* last)
    {
        return holds(first, last, now.utf16, now.utf16Size.load()) || holds(first, last, now.utf8, now.utf8Size.load());
    }
    // Whether a block is one of what this one looks at (made since it began), or one Qt's network made itself.
    static bool marked(const std::uint64_t* bits, const Looking& now, long request)
    {
        const long at = now.origin < 0 ? -1 : request - now.origin;
        return at >= 0 && at < followed && bits != nullptr && (bits != now.mine || request >= now.first) &&
               (bits[at / 64] & (std::uint64_t{1} << (at % 64))) != 0;
    }
    // Whether the block being made now is made by Qt's network library itself: the first library on the stack past the
    // runtime's and Qt's core (which make it for whoever calls them) is that one. (Nothing in here makes a block.)
    static bool madeByQtsNetwork(const Looking& now)
    {
        // (Past this and the hook that asks, look(), which are this test's own: with them first, nothing was ever taken
        // for Qt's network, until a test whose stand-in was let in on this thread's own line needed it.)
        std::array<void*, 32> frames{};
        const USHORT count = CaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
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
    // A block that is about to be made: marked as one of what is looked at, or of Qt's network's own (`qts`, asked by
    // the hook itself: madeByQtsNetwork passes over the hook alone on the stack).
    static void made(Looking& now, long request, bool watched, bool qts)
    {
        if (now.origin < 0) {
            now.origin = request;
        }
        if (now.first < 0) {
            now.first = request;
        }
        const long at = request - now.origin;
        if (at < 0 || request < now.first) {
            return;
        }
        if (at >= followed) {
            now.overflowed = true;
            return;
        }
        if (qts) {
            now.qts[at / 64] |= std::uint64_t{1} << (at % 64);
        } else if (watched) {
            // (Cleared up to the highest at the next begin.)
            now.highest = std::max(now.highest, at);
            now.mine[at / 64] |= std::uint64_t{1} << (at % 64);
        }
    }
    // The blocks of the heap as it stands that are of what is looked at, and hold it.
    static int scan(const Looking& now)
    {
        _CrtMemState state{};
        _CrtMemCheckpoint(&state);
        int count = 0;
        for (const auto* at = reinterpret_cast<const BlockHeader*>(state.pBlockHeader); at != nullptr; at = at->older) {
            const int type = _BLOCK_TYPE(at->use);
            if ((type != _NORMAL_BLOCK && type != _CLIENT_BLOCK) || !marked(now.mine, now, at->request)) {
                continue;
            }
            const auto* first = reinterpret_cast<const char*>(at + 1);
            if (holdsIt(now, first, first + at->size)) {
                ++count;
            }
        }
        return count;
    }
    static int __cdecl look(int what, void* block, size_t size, int use, long request, const unsigned char* file,
                            int line)
    {
        Looking& now = looking();
        const std::thread::id only = now.only.load();
        const std::thread::id self = std::this_thread::get_id();
        const bool watched = (only == std::thread::id() || only == self) &&
                             std::ranges::none_of(now.notThese, [self](const std::atomic<std::thread::id>& one) {
                                 return one.load() == self;
                             });
        if (what == _HOOK_ALLOC && now.scanBy.load() == self && now.scanned < 0) {
            now.scanned = scan(now);
        }
        // (Made on any thread: Qt's network thread hands what it made to this one too. And made anew where it grows: a
        // block moved by realloc is another block, of another number.)
        if (what == _HOOK_ALLOC || what == _HOOK_REALLOC) {
            made(now, request, watched, now.qtsNetwork != nullptr && madeByQtsNetwork(now));
        }
        if (block != nullptr && (what == _HOOK_FREE || what == _HOOK_REALLOC) &&
            (_BLOCK_TYPE(use) == _NORMAL_BLOCK || _BLOCK_TYPE(use) == _CLIENT_BLOCK)) {
            // Given back by what is looked at, or made by it and given back by anybody; and not Qt's network's own.
            const long number = now.laidOut ? (static_cast<const BlockHeader*>(block) - 1)->request : -1;
            if ((watched || marked(now.mine, now, number)) && !marked(now.qts, now, number)) {
                const size_t held = _msize_dbg(block, _BLOCK_TYPE(use));
                const auto* first = static_cast<const char*>(block);
                const auto* last = first + (held == static_cast<size_t>(-1) ? 0 : held);
                if (holdsIt(now, first, last)) {
                    now.found.fetch_add(1);
                }
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
                m_id = std::this_thread::get_id();
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
    std::thread::id id() const { return m_id; }

private:
    QThread m_thread;
    std::unique_ptr<QObject> m_there = std::make_unique<QObject>();
    std::unique_ptr<Made> m_made;
    QUrl m_address;
    std::thread::id m_id;
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
// makes of that password is followed until none of it is held any more (10 s at the most: the sign-in's line, its
// replies, and what Qt was handed with them go a turn of the loop later), and each block of it looked at as it is given
// back, by whichever thread; and what this thread gives back meanwhile is looked through too. Where anything held it as
// it was given back, or holds it still, or `use` says no, that is noted in `wrong` under `road`. (What Qt's network
// made itself is passed over, wherever it is given back: the copy of a body it hands its own thread to send is made on
// this one, and given back on either, as the threads' times fall. Out of reach, SPEC section 8: at 77b47156 the gate
// failed twice on that copy, given back on this thread.)
// Its limits, as SPEC section 8 says them (review cx6-1, L3): a block the program makes of the password on another of
// its threads on these roads (a pool's, a line's own) is not followed, for this look cannot tell it from what a
// stand-in on a thread of its own makes of what it is sent, both being this test program's; these roads make none so
// today, and the drivers' roads look at every thread. And where Qt's network is passed over, so is a block a Qt
// Network call makes and hands back (a reply's header, a cookie's raw form): the look's own test shows both edges.
void lookAtWhatIsGivenBack(QStringList& wrong, const QString& road, const char* text,
                           const std::function<bool(QString)>& use)
{
    // (A copy of its own, which nothing gives back while it is looked at.)
    const QString looked = aPassword(text);
    int found = 0;
    int held = 0;
    bool came = false;
    {
        const WhatIsGivenBack watching(looked, true, true, true);
        // (Made while it is looked at: the password itself is followed from its making on.)
        came = use(aPassword(text));
        held = WhatIsGivenBack::heldStillAfter(10000);
        found = WhatIsGivenBack::holdingIt();
    }
    if (!came) {
        wrong.append(u"%1: it did not come to what it was to"_s.arg(road));
    }
    if (found > 0) {
        wrong.append(u"%1: %2 blocks held the password as they were given back"_s.arg(road).arg(found));
    }
    if (held != 0) {
        wrong.append(
            u"%1: %2 blocks made of the password are held still (-1: the heap could not be looked through)"_s.arg(road)
                .arg(held));
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

// A Proxmox guest's console by hand, on a thread of its own (OnItsOwnThread): its WebSocket answers with the
// subprotocol "binary", and lets in an upgrade only where its Authorization header is `consoleToken`.
const QByteArray consoleToken = "PVEAPIToken=qkvm@pve!wall=6c1d0e2f-8a3b-4c5d-9e7f-0a1b2c3d4e5f"_ba;

struct AGuestsConsole {
    AGuestsConsole()
    {
        console.speaks({u"binary"_s});
        console.door = [](const QByteArray& request) {
            const bool signedIn = request.contains("\r\nAuthorization: " + consoleToken + "\r\n");
            return core::testing::RfbConsoleByHand::Door{
                true, signedIn ? std::nullopt
                               : std::optional<QByteArray>("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n")};
        };
    }
    QUrl address() const { return console.address(); }

    core::testing::RfbConsoleByHand console;
};

// A Proxmox by hand over TLS, as a Proxmox is spoken to, on a thread of its own, whose guest 100's console lets in an
// upgrade only where its Authorization header is `consoleToken` (and asks for no ticket: the way in is the test's).
struct AProxmoxsConsole {
    AProxmoxsConsole()
    {
        core::testing::RfbConsoleByHand& console = consoles.of(100);
        console.security = {1}; // (no VNC authentication: the token is what this road is about)
        console.door = [](const QByteArray& request) {
            const bool signedIn = request.contains("\r\nAuthorization: " + consoleToken + "\r\n");
            return core::testing::RfbConsoleByHand::Door{
                true, signedIn ? std::nullopt
                               : std::optional<QByteArray>("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n")};
        };
    }
    QUrl address() const { return pve.address(); }

    core::testing::ProxmoxByHand pve{core::testing::proxmoxTls()};
    core::testing::ProxmoxConsoles consoles{pve};
};

// The way into such a console, with a copy of the token of its own, as a guest's driver hands one in.
core::RfbScreen::WayIn wayInto(const QUrl& address)
{
    core::RfbScreen::WayIn way;
    way.address = address;
    way.path = "/api2/json/nodes/pve1/qemu/100/vncwebsocket?port=5900&vncticket=PVEVNC%3A1"_ba;
    way.authorization = core::SecretBytes::copyOf(consoleToken);
    way.protocol = "binary"_ba;
    return way;
}

core::RfbScreen::Limits consoleQuick()
{
    core::RfbScreen::Limits limits;
    limits.openingMs = 3000;
    limits.handshakeMs = 3000;
    limits.lookPaceMs = 100;
    limits.goodbyeWithinMs = 500;
    return limits;
}

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

    void theLookFindsWhatIsGivenBackUnwrittenAndWhatIsHeldStill()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // The outside review of the wave 2 desktop, 6.6: no test showed the look finding anything. Each way a secret is
        // left behind is made here on purpose, and found; and the same written over first is not. As the roads look
        // (this thread's own, Qt's network passed over).
        const QString secret = aPassword("look 4Wz 8h");
        const auto lookedAt = [&secret](const std::function<void()>& made) {
            const WhatIsGivenBack watching(secret, true, true, true);
            made();
            return std::pair<int, int>(WhatIsGivenBack::holdingIt(), WhatIsGivenBack::heldStill());
        };
        // Its UTF-8 given back as it is; and written over first.
        QCOMPARE(lookedAt([&secret] {
                     QByteArray copy = secret.toUtf8();
                     copy = QByteArray();
                 }),
                 std::pair(1, 0));
        QCOMPARE(lookedAt([&secret] {
                     QByteArray copy = secret.toUtf8();
                     core::wipeInPlace(copy);
                 }),
                 std::pair(0, 0));
        // Its UTF-16, moved where it grows: the room it stood in is given back as it was.
        QCOMPARE(lookedAt([&secret] {
                     QString copy(secret.constData(), secret.size());
                     copy.reserve(4096);
                     core::wipeInPlace(copy);
                 }),
                 std::pair(1, 0));
        // Held still when the look ends, past any wait: found where it is, and nothing of it found once it went written
        // over.
        std::optional<QByteArray> kept;
        QCOMPARE(lookedAt([&secret, &kept] { kept = secret.toUtf8(); }), std::pair(0, 1));
        core::wipeInPlace(*kept);
        kept.reset();
        {
            const WhatIsGivenBack watching(secret, true, true, true);
            QString held(secret.constData(), secret.size());
            QCOMPARE(WhatIsGivenBack::heldStillAfter(200), 1);
            QTimer::singleShot(300, [&held] { core::wipeInPlace(held); });
            QCOMPARE(WhatIsGivenBack::heldStillAfter(10000), 0);
            QCOMPARE(WhatIsGivenBack::holdingIt(), 0);
        }
        // Made here and given back as it was by another thread: found all the same. Made by another thread and given
        // back there, it is that thread's own (a stand-in's), and passed over.
        QCOMPARE(lookedAt([&secret] {
                     QByteArray copy = secret.toUtf8();
                     std::thread([&copy] { copy = QByteArray(); }).join();
                 }),
                 std::pair(1, 0));
        QCOMPARE(lookedAt([&secret] {
                     std::thread([&secret] {
                         QByteArray copy = secret.toUtf8();
                         copy = QByteArray();
                     }).join();
                 }),
                 std::pair(0, 0));
        // Where every thread's is looked at (the drivers' roads), that is found too.
        {
            const WhatIsGivenBack watching(secret, true, false, false);
            std::thread([&secret] {
                QByteArray copy = secret.toUtf8();
                copy = QByteArray();
            }).join();
            QCOMPARE(WhatIsGivenBack::holdingIt(), 1);
        }
        // Where Qt's network is passed over, so is a block a Qt Network call makes and hands back (a cookie's raw form,
        // here): out of the look's reach, as Qt's own copies are (SPEC section 8; review cx6-1, L3). What the program
        // makes of it is its own, and found.
        {
            QByteArray utf8 = secret.toUtf8();
            {
                const QNetworkCookie cookie("a", utf8);
                QCOMPARE(lookedAt([&cookie] {
                             QByteArray raw = cookie.toRawForm();
                             raw = QByteArray();
                         }),
                         std::pair(0, 0));
                QCOMPARE(lookedAt([&cookie] {
                             QByteArray raw = cookie.toRawForm();
                             QByteArray mine(raw.constData(), raw.size());
                             core::wipeInPlace(raw);
                             mine = QByteArray();
                         }),
                         std::pair(1, 0));
            }
            core::wipeInPlace(utf8);
        }
#endif
    }

    void theAnswerToAVncChallengeLeavesNothingOfItsPasswordOrKeyBehind()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // VNC authentication (vnc_auth.h): the password's UTF-8, and the DES key made of its first eight bytes, stand
        // in room that is written over before it is given back, on the way to Windows and after.
        const QString password = aPassword("vnc 7Qx 2m and the rest of it");
        std::array<std::uint8_t, core::vnc::challengeBytes> challenge{};
        for (std::size_t at = 0; at < challenge.size(); ++at) {
            challenge.at(at) = static_cast<std::uint8_t>(17 * at + 3);
        }
        {
            const WhatIsGivenBack watching(password);
            QVERIFY(core::vnc::answer(QStringView(password), challenge).ok());
            QCOMPARE(WhatIsGivenBack::holdingIt(), 0);
            QCOMPARE(WhatIsGivenBack::heldStill(), 0);
        }
        const QByteArray utf8 = password.toUtf8();
        std::array<std::uint8_t, 8> key{};
        rfb::vncKey(std::span(reinterpret_cast<const std::uint8_t*>(utf8.constData()), static_cast<std::size_t>(8)),
                    key);
        {
            const WhatIsGivenBack watching(QByteArrayView(reinterpret_cast<const char*>(key.data()), 8));
            QVERIFY(core::vnc::answer(QStringView(password), challenge).ok());
            QVERIFY(core::vnc::answer(QByteArrayView(utf8), challenge).ok());
            QCOMPARE(WhatIsGivenBack::holdingIt(), 0);
            QCOMPARE(WhatIsGivenBack::heldStill(), 0);
        }
#endif
    }

    void eachDriverWritesItsPasswordOverWhenItGoes()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        QStringList wrong;
        const auto lookAt = [&wrong](const QString& road, const char* text, const std::function<void(QString)>& use) {
            // (A copy of its own, which nothing gives back while it is looked at.)
            const QString looked = aPassword(text);
            int found = 0;
            int held = 0;
            {
                const WhatIsGivenBack watching(looked);
                // (Made while it is looked at, and followed from its making on.)
                use(aPassword(text));
                found = WhatIsGivenBack::holdingIt();
                held = WhatIsGivenBack::heldStill();
            }
            if (found > 0) {
                wrong.append(u"%1: %2 blocks held the password as they were given back"_s.arg(road).arg(found));
            }
            if (held != 0) {
                wrong.append(u"%1: %2 blocks made of the password are held still"_s.arg(road).arg(held));
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
        // (Held still: the store's own copy, and nothing else.)
        const int held = WhatIsGivenBack::heldStill();
        secrets.watching.reset();
        QCOMPARE(found, 0);
        QCOMPARE(held, 1);
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
            QCOMPARE(WhatIsGivenBack::heldStill(), 0);
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
        int held = 0;
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
            held = WhatIsGivenBack::heldStill();
        }
        QCOMPARE(form, 0);
        QCOMPARE(json, 0);
        QCOMPARE(held, 0);
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
        // hands its driver; one taken from a file and not asked yet, whose record the start reads and hands nothing
        // of; and one whose record is a record of two that a change cut off left (Windows did not take its last
        // keeping), which the start reads and makes one (review cx6-2, L5 a). The wall let go of. Of a password the
        // wall made a text of its own, and of the record of two a record of its own (the store's bytes are the
        // store's): nothing of that is left behind in what is given back, by the wall or by a driver. (Looked for as
        // the program's text, and as its UTF-8 in what is given back: the store keeps the record it was handed.)
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        const QString password = aPassword("wall one 5Mr 9g");
        QString cut;
        {
            core::Settings settings(file);
            core::LocalWall wall(settings, secrets, core::KvmdDevice::Pace{});
            core::OwnKvm kvm;
            kvm.address = u"https://127.0.0.1:9"_s;
            kvm.name = u"Rack"_s;
            kvm.knownByCertificate = true;
            kvm.certificate = QByteArray(32, 'c');
            QVERIFY(wall.add(kvm, password).ok());
            core::LocalWall::FromAFile fromAFile;
            fromAFile.kvm.address = u"https://127.0.0.1:8"_s;
            fromAFile.kvm.name = u"From a file"_s;
            fromAFile.kvm.knownByCertificate = true;
            fromAFile.kvm.certificate = QByteArray(32, 'd');
            fromAFile.password = password;
            QVERIFY(wall.takeFromAFile({fromAFile}).constFirst().ok());
            core::OwnKvm third = kvm;
            third.address = u"https://127.0.0.1:7"_s;
            third.name = u"Cut"_s;
            third.certificate = QByteArray(32, 'e');
            const base::Result<QString> added = wall.add(third, password);
            QVERIFY(added.ok());
            cut = added.value();
            core::OwnKvm moved = wall.kvms().constLast();
            moved.address = u"https://127.0.0.1:6"_s;
            secrets.refusesTheKeep = secrets.keeps + 2;
            static_cast<void>(wall.change(cut, moved, password));
            secrets.refusesTheKeep = 0;
        }
        QVERIFY(secrets.all.value(core::LocalWall::secretOf(cut)).startsWith("QKVM-KVM-2+\n"));
        const QHash<QString, QByteArray> asItWas = secrets.all;
        for (const bool utf8Too : {false, true}) {
            secrets.all = asItWas;
            {
                const WhatIsGivenBack watching(password, utf8Too);
                {
                    core::Settings settings(file);
                    core::LocalWall wall(settings, secrets, core::KvmdDevice::Pace{});
                    QCOMPARE(wall.kvms().size(), 3);
                    QVERIFY(wall.wasAskedWhatItIs(wall.kvms().constFirst().id));
                    QVERIFY(!wall.wasAskedWhatItIs(wall.kvms().at(1).id));
                    QVERIFY(wall.wasAskedWhatItIs(cut));
                }
                QCOMPARE(WhatIsGivenBack::holdingIt(), 0);
                // (Its UTF-8 is held still by the store, in the one record it was handed for the KVM that was cut.)
                if (!utf8Too) {
                    QCOMPARE(WhatIsGivenBack::heldStill(), 0);
                }
            }
            // The start made the two one.
            QVERIFY(!secrets.all.value(core::LocalWall::secretOf(cut)).startsWith("QKVM-KVM-2+"));
        }
#endif
    }

    void theConsoleLinesTokenIsWrittenOverOnceItsUpgradeWent()
    {
#ifndef _DEBUG
        QSKIP("Only a program built for debugging can look at what it gives back (the debug heap's hook).");
#else
        // The open item of review c9b-1, L1, after the rebase onto the VNC servers: a Proxmox guest's API token goes
        // in the Authorization header of its console's upgrade, from the way in (RfbScreen::WayIn::authorization) into
        // the line's Where (DeviceLine::Where::authorization). Each copy of it is written over once the upgrade went,
        // and on every road where it never goes: the way that comes when nobody waits for it any more, and the line
        // that is never let in. What every thread gives back is looked at, the line's socket's own thread among them
        // (outside review 2.2: the copy of the asking that thread is handed), but the consoles' by hand, each on a
        // thread of its own. (What Qt's network makes of the asking to send it is Qt's: SPEC section 8.)
        const QString looked = QString::fromLatin1(consoleToken);
        QStringList wrong;
        std::vector<std::thread::id> standIns;
        const auto lookedAt = [&wrong, &looked, &standIns](const QString& road, const std::function<bool()>& use) {
            int found = 0;
            int held = 0;
            bool came = false;
            {
                const WhatIsGivenBack watching(looked, true, false, true);
                for (const std::thread::id one : standIns) {
                    WhatIsGivenBack::passOver(one);
                }
                came = use();
                // (Until nothing made of it is held any more: the line, and the session's reader, go a turn of the
                // loop later.)
                held = WhatIsGivenBack::heldStillAfter(10000);
                found = WhatIsGivenBack::holdingIt();
            }
            if (!came) {
                wrong.append(u"%1: it did not come to what it was to"_s.arg(road));
            }
            if (found > 0) {
                wrong.append(u"%1: %2 blocks held the token as they were given back"_s.arg(road).arg(found));
            }
            if (held != 0) {
                wrong.append(u"%1: %2 blocks made of the token are held still"_s.arg(road).arg(held));
            }
        };
        const auto nothingShown = [](const qkvm::bmc::aspeed::Picture&, std::span<const qkvm::bmc::aspeed::Rect>) {};
        // Let in with the token, its screen shown, and let go of.
        const OnItsOwnThread<AGuestsConsole> there;
        standIns.push_back(there.id());
        lookedAt(u"a console let in and let go of"_s, [&there] {
            int pictures = 0;
            core::RfbScreen screen(
                [address = there.address()](const core::RfbScreen::Answer& answer, const core::RfbScreen::Wanted&) {
                    answer(wayInto(address));
                },
                [&pictures](const qkvm::bmc::aspeed::Picture&, std::span<const qkvm::bmc::aspeed::Rect>) {
                    ++pictures;
                },
                consoleQuick());
            return QTest::qWaitFor(
                [&screen, &pictures] {
                    static_cast<void>(screen.look());
                    return pictures > 0;
                },
                15000);
        });
        // The same over TLS, at a Proxmox's own address, known by its certificate, as a guest's console is: where Qt
        // keeps a QByteArray it is handed (QSslSocket's buffer), and not a copy of its own.
        const OnItsOwnThread<AProxmoxsConsole> proxmox;
        standIns.push_back(proxmox.id());
        lookedAt(u"a console over TLS let in and let go of"_s, [&proxmox] {
            int pictures = 0;
            core::RfbScreen screen(
                [address = proxmox.address()](const core::RfbScreen::Answer& answer, const core::RfbScreen::Wanted&) {
                    core::RfbScreen::WayIn way = wayInto(address);
                    way.knownByCertificate = true;
                    way.certificate = core::testing::certificateOf();
                    // (An asking longer than Qt's ring buffer's chunk, 4 KiB, as a long ticket makes it: one that Qt
                    // would keep itself, shared, were it handed the QByteArray.)
                    way.path += "&padding=" + QByteArray(5000, 'p');
                    answer(std::move(way));
                },
                [&pictures](const qkvm::bmc::aspeed::Picture&, std::span<const qkvm::bmc::aspeed::Rect>) {
                    ++pictures;
                },
                consoleQuick());
            return QTest::qWaitFor(
                [&screen, &pictures] {
                    static_cast<void>(screen.look());
                    return pictures > 0;
                },
                15000);
        });
        // A way that comes once nobody waits for it any more.
        lookedAt(u"a way nobody waits for"_s, [&there, &nothingShown] {
            core::RfbScreen::Answer kept;
            {
                core::RfbScreen screen(
                    [&kept](const core::RfbScreen::Answer& answer, const core::RfbScreen::Wanted&) { kept = answer; },
                    nothingShown, consoleQuick());
                static_cast<void>(screen.look());
                if (!QTest::qWaitFor([&kept] { return static_cast<bool>(kept); }, 5000)) {
                    return false;
                }
            }
            kept(wayInto(there.address()));
            kept = nullptr;
            return true;
        });
        // A line that is never let in: nobody at its port.
        lookedAt(u"a line nobody answers"_s, [&nothingShown] {
            core::RfbScreen screen(
                [](const core::RfbScreen::Answer& answer, const core::RfbScreen::Wanted&) { answer(wayInto(nowhere)); },
                nothingShown, consoleQuick());
            QSignalSpy ended(&screen, &core::ServerConsole::ended);
            static_cast<void>(screen.look());
            return ended.wait(10000);
        });
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u"; "_s)));
#endif
    }
};

QTEST_GUILESS_MAIN(TestSecretsWiped)
#include "test_secrets_wiped.moc"
