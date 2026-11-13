#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QElapsedTimer>
#include <QTest>

#include <qkvm/base/clock.h>

#include "decode_pool.h"

using qkvm::engine::DecodePool;

namespace {
// Work that notes when it ran and then waits until it is let go, for as long as a test says.
class Held final : public DecodePool::Work {
public:
    explicit Held(int name) : name(name) {}
    ~Held() = default;
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;

    void run() override
    {
        // Its place in the order first: a test that has seen it running, or its run counted, then finds
        // the place there too (written after, a loaded PC let a test read the order without it).
        {
            const std::lock_guard<std::mutex> lock(*orderLock);
            order->push_back(name);
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_runs;
            m_running = true;
        }
        m_changed.notify_all();
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_letGo; });
        m_running = false;
    }
    void release()
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_letGo = true;
        }
        m_changed.notify_all();
    }
    bool isRunning()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_running;
    }
    int ran()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_runs;
    }

    const int name;
    // Shared by the works of one test: the order they began in, and the lock it is written under (the
    // tests read it through Works::begun).
    std::vector<int>* order = nullptr;
    std::mutex* orderLock = nullptr;

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_letGo = false;
    bool m_running = false;
    int m_runs = 0;
};

// The works of one test. Gone, they are let go and forgotten by the pool first, also where the test
// ended early (a red one would leave the pool's threads waiting on them for good).
struct Works {
    Works(DecodePool& pool, int count) : pool(pool)
    {
        for (int name = 0; name < count; ++name) {
            all.push_back(std::make_unique<Held>(name));
            all.back()->order = &order;
            all.back()->orderLock = &orderLock;
        }
    }
    Held& operator[](int name) { return *all[static_cast<std::size_t>(name)]; }
    int running()
    {
        int count = 0;
        for (const auto& one : all) {
            count += one->isRunning() ? 1 : 0;
        }
        return count;
    }
    void releaseAll()
    {
        for (const auto& one : all) {
            one->release();
        }
    }
    // The order the works began in so far, read under its lock.
    std::vector<int> begun()
    {
        const std::lock_guard<std::mutex> lock(orderLock);
        return order;
    }
    ~Works()
    {
        releaseAll();
        for (const auto& one : all) {
            pool.forget(one.get());
        }
    }
    Works(const Works&) = delete;
    Works& operator=(const Works&) = delete;
    Works(Works&&) = delete;
    Works& operator=(Works&&) = delete;
    DecodePool& pool;
    std::vector<std::unique_ptr<Held>> all;
    std::mutex orderLock;
    std::vector<int> order;
};
} // namespace

// The engine's pool of decoding threads (design 4, review 6's 2, review 8's 4; M6 2d): a stream shown
// large or controlled goes before previews that wait, and previews never have the pool's last thread,
// so that one is always free for such a stream, also while a preview's decoding is under way.
class TestDecodePool : public QObject {
    Q_OBJECT
private slots:
    // Three previews that each hold a thread: of two, they have one; an urgent one runs at once on the
    // other, while they still hold theirs.
    void previewsNeverTakeTheLastThread()
    {
        DecodePool pool(2);
        Works works(pool, 4);
        for (int name = 0; name < 3; ++name) {
            pool.wake(&works[name], false);
        }
        QTRY_COMPARE(works.running(), 1);
        QTest::qWait(100);
        QCOMPARE(works.running(), 1);
        pool.wake(&works[3], true);
        QTRY_VERIFY(works[3].isRunning());
        QCOMPARE(works.running(), 2);
        works.releaseAll();
        for (int name = 0; name < 4; ++name) {
            QTRY_COMPARE(works[name].ran(), 1);
        }
        for (int name = 0; name < 4; ++name) {
            pool.forget(&works[name]);
        }
    }

    // Both threads busy, one with a shown-large stream and one with a preview; a preview and then a
    // shown-large stream come to wait. The thread that comes free takes the shown-large one first.
    void shownLargeGoesBeforeWaitingPreviews()
    {
        DecodePool pool(2);
        Works works(pool, 4);
        pool.wake(&works[0], true);
        QTRY_VERIFY(works[0].isRunning());
        pool.wake(&works[1], false);
        QTRY_VERIFY(works[1].isRunning());
        pool.wake(&works[2], false);
        pool.wake(&works[3], true);
        QTest::qWait(50);
        QCOMPARE(works.running(), 2);
        works[1].release();
        QTRY_VERIFY(works[3].isRunning());
        QVERIFY(!works[2].isRunning());
        works.releaseAll();
        QTRY_COMPARE(works[2].ran(), 1);
        QCOMPARE(works.begun(), (std::vector<int>{0, 1, 3, 2}));
        for (int name = 0; name < 4; ++name) {
            pool.forget(&works[name]);
        }
    }

    // With nothing urgent, previews have every thread but one: of four, three at once, and the rest as
    // those come free (they do not starve).
    void previewsUseAllButOneWhenNothingIsUrgent()
    {
        DecodePool pool(4);
        Works works(pool, 5);
        for (int name = 0; name < 5; ++name) {
            pool.wake(&works[name], false);
        }
        QTRY_COMPARE(works.running(), 3);
        QTest::qWait(100);
        QCOMPARE(works.running(), 3);
        // The first comes free: the next that waits, and no other, takes its thread. (Waited for once the first has
        // ended: "three running" alone was true at once, before it ended, and not a moment later, before the next
        // began; a loaded PC saw the second, 2 where 3 was looked for.)
        works[0].release();
        QTRY_VERIFY(!works[0].isRunning() && works[3].isRunning());
        QTest::qWait(100);
        QCOMPARE(works.running(), 3);
        QCOMPARE(works[4].ran(), 0);
        works.releaseAll();
        for (int name = 0; name < 5; ++name) {
            QTRY_COMPARE(works[name].ran(), 1);
        }
        for (int name = 0; name < 5; ++name) {
            pool.forget(&works[name]);
        }
    }

    // 2d's reading (L3): previews do not starve: on a pool of three, urgent work has two threads at most
    // while a preview waits, and a thread that comes free goes to the waiting preview, not to urgent work
    // beyond its two. (Rewritten in 2e's reading: urgent work may take the third where no preview waits.)
    void previewsKeepOneThreadWhereThereAreThree()
    {
        DecodePool pool(3);
        Works works(pool, 6);
        pool.wake(&works[0], true);
        pool.wake(&works[1], true);
        QTRY_COMPARE(works.running(), 2);
        pool.wake(&works[4], false);
        QTRY_VERIFY(works[4].isRunning());
        pool.wake(&works[2], true);
        pool.wake(&works[3], true);
        pool.wake(&works[5], false);
        QTest::qWait(50);
        works[4].release();
        QTRY_VERIFY(works[5].isRunning());
        QTest::qWait(50);
        QCOMPARE(works.running(), 3);
        QVERIFY(!works[2].isRunning() && !works[3].isRunning());
        works.releaseAll();
        for (int name = 0; name < 6; ++name) {
            QTRY_COMPARE(works[name].ran(), 1);
        }
    }

    // 2e and the SPS reader, after their reading (L5): where no preview waits, urgent work takes every thread
    // of a pool of three (before: one stayed idle for previews that were not there).
    void urgentWorkTakesTheLastThreadWhereNoPreviewWaits()
    {
        DecodePool pool(3);
        Works works(pool, 3);
        for (int name = 0; name < 3; ++name) {
            pool.wake(&works[name], true);
        }
        QTRY_COMPARE(works.running(), 3);
        works.releaseAll();
        for (int name = 0; name < 3; ++name) {
            QTRY_COMPARE(works[name].ran(), 1);
        }
    }

    // A pool of one thread still decodes previews: there is no last thread to keep.
    void aPoolOfOneStillRunsPreviews()
    {
        DecodePool pool(1);
        Works works(pool, 1);
        works[0].release();
        pool.wake(&works[0], false);
        QTRY_COMPARE(works[0].ran(), 1);
        pool.forget(&works[0]);
    }

    // forget() comes back only once the work that runs has ended, and what waits is taken out of
    // either line.
    void forgetStillWaitsForTheRunning()
    {
        DecodePool pool(2);
        Works works(pool, 3);
        pool.wake(&works[0], true);
        QTRY_VERIFY(works[0].isRunning());
        pool.wake(&works[1], false);
        QTRY_VERIFY(works[1].isRunning());
        pool.wake(&works[2], false);
        pool.forget(&works[2]);
        std::atomic<bool> forgotten{false};
        std::thread forgetting([&] {
            pool.forget(&works[0]);
            forgotten = true;
        });
        QTest::qWait(100);
        QVERIFY(!forgotten);
        works[0].release();
        forgetting.join();
        QVERIFY(forgotten);
        works.releaseAll();
        QTRY_COMPARE(works[1].ran(), 1);
        QTest::qWait(100);
        QCOMPARE(works[2].ran(), 0);
        pool.forget(&works[1]);
    }

    // 2c's reading, L4: work that is retired while it runs is ended by the pool once its run is over,
    // and the one who retired it does not wait; work that does not run is ended at once, and what of it
    // waits is taken out of line.
    void retiredWorkEndsOnceItsRunIsOverAndNobodyWaits()
    {
        DecodePool pool(2);
        Works works(pool, 3);
        pool.wake(&works[0], true);
        QTRY_VERIFY(works[0].isRunning());
        std::atomic<int> ended{0};
        QElapsedTimer took;
        took.start();
        pool.retire(&works[0], [&] { ++ended; });
        QVERIFY2(took.elapsed() < 50, qPrintable(QString::number(took.elapsed())));
        QTest::qWait(100);
        QCOMPARE(ended.load(), 0);
        works[0].release();
        QTRY_COMPARE(ended.load(), 1);
        // Waiting, never run: ended at once, and never run after.
        pool.wake(&works[1], false);
        QTRY_VERIFY(works[1].isRunning());
        pool.wake(&works[2], false);
        std::atomic<bool> endedNow{false};
        pool.retire(&works[2], [&] { endedNow = true; });
        QVERIFY(endedNow);
        works.releaseAll();
        QTest::qWait(100);
        QCOMPARE(works[2].ran(), 0);
        QCOMPARE(ended.load(), 1);
    }

    // (The outside review of 2026-11-05, F4.) Work that is to be woken later is woken at its time and
    // not before, once, at the sooner of two times asked for, and not at all once it was forgotten or
    // retired before its time.
    void workIsWokenLaterAtItsTimeAndNotBefore()
    {
        DecodePool pool(2);
        Works works(pool, 5);
        works.releaseAll();
        pool.wakeAt(&works[0], qkvm::base::steadyNowUs() + 300'000);
        QTest::qWait(100);
        QCOMPARE(works[0].ran(), 0);
        QTRY_COMPARE(works[0].ran(), 1);
        QTest::qWait(300);
        QCOMPARE(works[0].ran(), 1);
        // Asked for a minute from now and then for a moment from now; and the other way round.
        pool.wakeAt(&works[1], qkvm::base::steadyNowUs() + 60'000'000);
        pool.wakeAt(&works[1], qkvm::base::steadyNowUs() + 100'000);
        pool.wakeAt(&works[2], qkvm::base::steadyNowUs() + 100'000);
        pool.wakeAt(&works[2], qkvm::base::steadyNowUs() + 60'000'000);
        QTRY_COMPARE(works[1].ran(), 1);
        QTRY_COMPARE(works[2].ran(), 1);
        // Forgotten, and retired, before its time: never run. (Retired, it is ended at once.)
        pool.wakeAt(&works[3], qkvm::base::steadyNowUs() + 200'000);
        pool.forget(&works[3]);
        pool.wakeAt(&works[4], qkvm::base::steadyNowUs() + 200'000);
        std::atomic<bool> ended{false};
        pool.retire(&works[4], [&] { ended = true; });
        QVERIFY(ended);
        QTest::qWait(600);
        QCOMPARE(works[3].ran(), 0);
        QCOMPARE(works[4].ran(), 0);
        QCOMPARE(works[1].ran(), 1);
        QCOMPARE(works[2].ran(), 1);
    }

    // A preview that becomes urgent while it waits is moved to the front.
    void aWaitingPreviewWokenAsUrgentGoesFirst()
    {
        DecodePool pool(2);
        Works works(pool, 5);
        pool.wake(&works[0], true);
        QTRY_VERIFY(works[0].isRunning());
        pool.wake(&works[1], false);
        QTRY_VERIFY(works[1].isRunning());
        pool.wake(&works[2], false);
        pool.wake(&works[3], false);
        pool.wake(&works[3], true);
        works[0].release();
        QTRY_VERIFY(works[3].isRunning());
        works.releaseAll();
        QTRY_COMPARE(works[2].ran(), 1);
        QCOMPARE(works.begun(), (std::vector<int>{0, 1, 3, 2}));
        for (int name = 0; name < 5; ++name) {
            pool.forget(&works[name]);
        }
    }
};
QTEST_GUILESS_MAIN(TestDecodePool)
#include "test_decode_pool.moc"
