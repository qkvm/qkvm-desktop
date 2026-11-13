#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace qkvm::engine {

// The threads that decode. A stream is not given a thread of its own: it says when it has a packet,
// and one of these threads does one packet's worth of work for it. So a wall of sixty-four tiles
// that mostly stand still costs a handful of threads that mostly sleep.
//
// Two lines (M6 2d; design 4, review 6's 2 and review 8's 4): work that is urgent (a stream shown
// large or controlled) goes before previews that wait, and previews never have the last thread: they
// run on all but one at most, so that one is free for an urgent stream also while previews' decoding
// is under way (a priority alone would not preempt that). Nor do previews starve where the pool has three
// threads or more: urgent work runs on all but one at most while a preview waits, so one is theirs (2d's
// reading); where none waits, it takes every thread (2e's reading: one stood idle for previews that were not
// there). On a pool of two, urgent work may have both threads, and previews wait while it does; on a pool of
// one they share it.
class DecodePool {
public:
    class Work {
    public:
        // Does a little: for a stream, one packet. On a thread of the pool. If more is left, the
        // work says so itself with wake().
        virtual void run() = 0;
        // Whether it goes before previews when it is woken (a stream shown large or controlled).
        virtual bool urgent() const { return false; }

    protected:
        ~Work() = default;

    private:
        friend class DecodePool;
        std::atomic<bool> m_waiting{false};
    };

    explicit DecodePool(unsigned threads);
    ~DecodePool();
    DecodePool(const DecodePool&) = delete;
    DecodePool& operator=(const DecodePool&) = delete;

    // Any thread: this work has something to do. Asking twice before it ran is asking once; asked as
    // urgent while it waits as a preview, it goes to the urgent line.
    void wake(Work* work, bool urgent);
    // As urgent as the work says it is.
    void wake(Work* work) { wake(work, work->urgent()); }
    // Any thread: this work is woken once `atUs` has come (on base::steadyNowUs' clock), and not
    // before, as urgent as it says it is then. For what is to be done later and that nothing else
    // wakes it for: a copy to the card that failed is tried again so (StreamCore::takeChanges).
    // Asked again before that time, it is woken once, at the sooner of the two. forget() and
    // retire() take it out of this as well; it is not asked of work that was handed to either.
    void wakeAt(Work* work, std::int64_t atUs);
    // Any thread but the pool's own: this work is about to be destroyed. Comes back when no thread
    // is running it and none will.
    void forget(Work* work);
    // Any thread but the pool's own: this work goes, and `end` is called once no thread runs it and
    // none will: at once where none runs it, or otherwise on the pool's thread as its run is over,
    // without the caller waiting (2c's reading, L4: a card taken away while its key frame decodes held
    // the interface thread up to 45 ms). What of it waits is taken out of line; it is run no more.
    void retire(Work* work, std::function<void()> end);
    // How often a piece of work ended by throwing, which nothing of ours does. Any thread.
    std::uint64_t thrown() const noexcept { return m_thrown.load(std::memory_order_relaxed); }

private:
    void serve(unsigned index);
    // With the lock held: whether a thread may take something now, and whether urgent work may.
    bool somethingToTake() const;
    bool urgentMayRun() const;
    // With the lock held: into its line, as wake() puts it there.
    void putInLine(Work* work, bool urgent);
    // With the lock held: what is to be woken later and is due goes into its line; and for how
    // long the soonest of the rest is not yet (0: nothing else is to be woken later).
    std::int64_t wakeWhatIsDue();
    // With the lock held: this work is to be woken later no more.
    void forgetLater(Work* work);

    std::mutex m_lock;
    std::condition_variable m_something;
    std::condition_variable m_finished;
    std::deque<Work*> m_urgent;
    std::deque<Work*> m_previews;
    std::vector<Work*> m_running; // by thread: what it is running, or null
    // Work retired while a thread ran it, and what ends it once none does.
    std::vector<std::pair<Work*, std::function<void()>>> m_retiring;
    // Work to be woken later, and when (wakeAt): once each, at the soonest time it was asked for.
    std::vector<std::pair<Work*, std::int64_t>> m_later;
    unsigned m_previewsRunning = 0;
    unsigned m_previewsAtMost = 1; // all threads but one, and one at the least
    unsigned m_urgentRunning = 0;
    // All threads but one where there are three or more: previews keep one, while one waits.
    unsigned m_urgentAtMost = 1;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
    std::atomic<std::uint64_t> m_thrown{0};
};

} // namespace qkvm::engine
