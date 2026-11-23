#include "line_threads.h"

#include <atomic>
#include <memory>
#include <utility>

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QList>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

namespace qkvm::core::lines {

namespace {

// One of the threads that run: for which place, and since when its line let go of it (-1: it has not).
struct One {
    QThread* thread = nullptr;
    QString place;
    qint64 letGoAtMs = -1;
};

// How many lines' threads run, and which, and whether the application has ended. Made once and never let go of: a
// thread that Windows still held at the application's end counts itself out whenever Windows lets it go, also while
// the process's own statics are ending.
struct Running {
    QMutex mutex;
    QWaitCondition changed;
    int count = 0;
    QList<One> threads;  // (each taken out as it finishes, on itself, before its loop's own end is let go of)
    QElapsedTimer clock; // (started at the first thread)
    bool hooked = false; // the application's end waits for them
    bool over = false;   // the application has ended, or is ending: nothing more is begun
};

Running& theRunning()
{
    static auto* const made = new Running; // NOLINT(cppcoreguidelines-owning-memory): never let go of, as above
    return *made;
}

// The application's end (its post routine, on its thread): the threads that are ending end, and one that Windows holds
// is not waited for beyond endWithinMs.
void atTheEnd()
{
    Running& all = theRunning();
    const QMutexLocker locked(&all.mutex);
    all.over = true;
    const QDeadlineTimer deadline(endWithinMs);
    while (all.count > 0 && !deadline.hasExpired()) {
        all.changed.wait(&all.mutex, deadline);
    }
}

// A test's hook (afterALook, afterTheRoom), where one is set: called on a line's thread with what that thread says
// there, until it answers true, and then taken away. Set and taken away under its lock, and called outside it.
template <typename... Said>
struct Hook {
    QMutex mutex;
    std::shared_ptr<std::function<bool(Said...)>> what;
    std::atomic<bool> set{false};

    void put(std::function<bool(Said...)> hook)
    {
        const QMutexLocker locked(&mutex);
        what = hook ? std::make_shared<std::function<bool(Said...)>>(std::move(hook)) : nullptr;
        set.store(what != nullptr);
    }

    void call(Said... said)
    {
        if (!set.load(std::memory_order_relaxed)) {
            return; // (every line's thread comes here, often: nothing more where no test set a hook)
        }
        std::shared_ptr<std::function<bool(Said...)>> now;
        {
            const QMutexLocker locked(&mutex);
            now = what;
        }
        if (now == nullptr || !(*now)(said...)) {
            return;
        }
        const QMutexLocker locked(&mutex);
        if (what == now) {
            what = nullptr;
            set.store(false);
        }
    }
};

Hook<const void*>& theLookHook()
{
    static Hook<const void*> one;
    return one;
}

Hook<const void*>& theEarlyLookHook()
{
    static Hook<const void*> one;
    return one;
}

Hook<qint64>& theRoomHook()
{
    static Hook<qint64> one;
    return one;
}

Hook<qint64>& theHandedHook()
{
    static Hook<qint64> one;
    return one;
}

std::atomic<bool>& theLook()
{
    static std::atomic<bool> on{true};
    return on;
}

} // namespace

QThread* aThreadOfItsOwn(const QString& name, const QString& place)
{
    Running& all = theRunning();
    const QMutexLocker locked(&all.mutex);
    if (all.over || QCoreApplication::instance() == nullptr) {
        return nullptr;
    }
    if (!all.hooked) {
        all.hooked = true;
        all.clock.start();
        qAddPostRoutine(atTheEnd);
    }
    // So many threads of this place's lines that Windows still holds, long after their lines let go of them: what holds
    // them holds the next one too, and nothing more is begun there until Windows lets go (outside review of tls, L7).
    const qint64 now = all.clock.elapsed();
    const auto held = std::ranges::count_if(all.threads, [&place, now](const One& one) {
        return one.letGoAtMs >= 0 && now - one.letGoAtMs > heldAfterMs && one.place == place;
    });
    if (held >= mostHeldToOnePlace) {
        return nullptr;
    }
    auto* thread = new QThread; // NOLINT(cppcoreguidelines-owning-memory): deleted below once it has finished
    thread->setObjectName(name);
    ++all.count;
    all.threads.append({thread, place, -1});
    // Counted out on the thread itself, as it finishes; and the QThread let go of where it was made, after that.
    QObject::connect(
        thread, &QThread::finished, thread,
        [thread] {
            Running& those = theRunning();
            const QMutexLocker counting(&those.mutex);
            --those.count;
            those.threads.removeIf([thread](const One& one) { return one.thread == thread; });
            those.changed.wakeAll();
        },
        Qt::DirectConnection);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    // Above the others (they are light: a socket's TCP and TLS): what keeps a line to its limits runs there, the look
    // at what has waited too long and the reset of a line ended from its own thread, and a thread put aside by busier
    // work is what let a late piece out (measured on the build box, 2026-11-19: beside 28 threads that spin at normal
    // priority, no piece left late on any of 200 lines).
    thread->start(QThread::HighPriority);
    if (!thread->isRunning()) {
        // (Windows would not start it: it never says `finished`, and would be counted, and waited for at the end, for
        // good. None, as for a place Windows holds.)
        --all.count;
        all.threads.removeIf([thread](const One& one) { return one.thread == thread; });
        thread->deleteLater();
        return nullptr;
    }
    return thread;
}

void letGo(const QThread* thread)
{
    Running& all = theRunning();
    const QMutexLocker locked(&all.mutex);
    for (One& one : all.threads) {
        if (one.thread == thread && one.letGoAtMs < 0) {
            one.letGoAtMs = all.clock.elapsed();
        }
    }
}

int running()
{
    Running& all = theRunning();
    const QMutexLocker locked(&all.mutex);
    return all.count;
}

int askEach(const std::function<void()>& what)
{
    Running& all = theRunning();
    // (Under the lock: a thread that finishes takes itself out under it first, and its loop is let go of after that.)
    const QMutexLocker locked(&all.mutex);
    int asked = 0;
    for (const One& one : std::as_const(all.threads)) {
        if (QAbstractEventDispatcher* const there = one.thread->eventDispatcher()) {
            QMetaObject::invokeMethod(there, what, Qt::QueuedConnection);
            ++asked;
        }
    }
    return asked;
}

void afterALook(std::function<bool(const void* message)> hook)
{
    theLookHook().put(std::move(hook));
}

void lookedBeforeQt(const void* message)
{
    theLookHook().call(message);
}

void beforeALook(std::function<bool(const void* message)> hook)
{
    theEarlyLookHook().put(std::move(hook));
}

void lookingBeforeQt(const void* message)
{
    theEarlyLookHook().call(message);
}

void afterTheRoom(std::function<bool(qint64 room)> hook)
{
    theRoomHook().put(std::move(hook));
}

void foundRoom(qint64 room)
{
    theRoomHook().call(room);
}

void whatWasHanded(std::function<void(qint64 bytes)> hook)
{
    if (!hook) {
        theHandedHook().put({});
        return;
    }
    // (Never taken away by what it answers: it hears every look until it is set to none.)
    theHandedHook().put([hook = std::move(hook)](qint64 bytes) {
        hook(bytes);
        return false;
    });
}

void handed(qint64 bytes)
{
    theHandedHook().call(bytes);
}

void looksBeforeQt(bool on)
{
    theLook().store(on);
}

bool looksBeforeQt()
{
    return theLook().load(std::memory_order_relaxed);
}

} // namespace qkvm::core::lines
