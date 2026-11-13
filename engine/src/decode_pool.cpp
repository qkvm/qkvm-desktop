#include "decode_pool.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include <windows.h>

#include <qkvm/base/clock.h>

namespace qkvm::engine {

DecodePool::DecodePool(unsigned threads)
    : m_running(threads, nullptr), m_previewsAtMost(threads > 1 ? threads - 1 : 1),
      m_urgentAtMost(threads >= 3 ? threads - 1 : threads)
{
    m_threads.reserve(threads);
    for (unsigned index = 0; index < threads; ++index) {
        m_threads.emplace_back([this, index] { serve(index); });
        // A name, so that a debugger and a profiler say which thread this is.
        const std::wstring name = L"qkvm decode " + std::to_wstring(index + 1);
        SetThreadDescription(m_threads.back().native_handle(), name.c_str());
    }
}

DecodePool::~DecodePool()
{
    {
        const std::lock_guard guard(m_lock);
        m_stopping = true;
    }
    m_something.notify_all();
    for (std::thread& thread : m_threads) {
        thread.join();
    }
}

void DecodePool::wake(Work* work, bool urgent)
{
    if (work->m_waiting.exchange(true, std::memory_order_acq_rel)) {
        // It waits already. As a preview, and urgent now: to the urgent line.
        if (urgent) {
            bool moved = false;
            {
                const std::lock_guard guard(m_lock);
                if (const auto found = std::ranges::find(m_previews, work); found != m_previews.end()) {
                    m_previews.erase(found);
                    m_urgent.push_back(work);
                    moved = true;
                }
            }
            if (moved) {
                m_something.notify_one();
            }
        }
        return;
    }
    {
        const std::lock_guard guard(m_lock);
        (urgent ? m_urgent : m_previews).push_back(work);
    }
    m_something.notify_one();
}

void DecodePool::wakeAt(Work* work, std::int64_t atUs)
{
    {
        const std::lock_guard guard(m_lock);
        const auto found = std::ranges::find(m_later, work, &std::pair<Work*, std::int64_t>::first);
        if (found == m_later.end()) {
            m_later.emplace_back(work, atUs);
        } else if (atUs < found->second) {
            found->second = atUs;
        } else {
            return;
        }
    }
    // (A thread that waits knows of no sooner time than it had: each looks again.)
    m_something.notify_all();
}

void DecodePool::putInLine(Work* work, bool urgent)
{
    // As wake() does, with the lock held already.
    if (work->m_waiting.exchange(true, std::memory_order_acq_rel)) {
        if (urgent) {
            if (const auto found = std::ranges::find(m_previews, work); found != m_previews.end()) {
                m_previews.erase(found);
                m_urgent.push_back(work);
            }
        }
        return;
    }
    (urgent ? m_urgent : m_previews).push_back(work);
}

std::int64_t DecodePool::wakeWhatIsDue()
{
    const std::int64_t now = base::steadyNowUs();
    std::int64_t soonest = 0;
    bool woke = false;
    for (auto later = m_later.begin(); later != m_later.end();) {
        if (later->second <= now) {
            putInLine(later->first, later->first->urgent());
            later = m_later.erase(later);
            woke = true;
            continue;
        }
        const std::int64_t notYet = later->second - now;
        soonest = soonest == 0 ? notYet : std::min(soonest, notYet);
        ++later;
    }
    if (woke) {
        m_something.notify_all();
    }
    return soonest;
}

void DecodePool::forgetLater(Work* work)
{
    std::erase_if(m_later, [work](const std::pair<Work*, std::int64_t>& later) { return later.first == work; });
}

void DecodePool::forget(Work* work)
{
    std::unique_lock guard(m_lock);
    std::erase(m_urgent, work);
    std::erase(m_previews, work);
    forgetLater(work);
    m_finished.wait(guard, [&] { return std::ranges::find(m_running, work) == m_running.end(); });
    // It may have asked again while it ran for the last time.
    std::erase(m_urgent, work);
    std::erase(m_previews, work);
    forgetLater(work);
}

void DecodePool::retire(Work* work, std::function<void()> end)
{
    {
        const std::lock_guard guard(m_lock);
        std::erase(m_urgent, work);
        std::erase(m_previews, work);
        forgetLater(work);
        if (std::ranges::find(m_running, work) != m_running.end()) {
            m_retiring.emplace_back(work, std::move(end));
            return;
        }
    }
    end();
}

bool DecodePool::urgentMayRun() const
{
    // (Beyond its threads but one only while no preview waits: 2e and the SPS reader, after their reading.)
    return !m_urgent.empty() && (m_urgentRunning < m_urgentAtMost || m_previews.empty());
}

bool DecodePool::somethingToTake() const
{
    return urgentMayRun() || (!m_previews.empty() && m_previewsRunning < m_previewsAtMost);
}

void DecodePool::serve(unsigned index)
{
    std::unique_lock guard(m_lock);
    for (;;) {
        // Until something may be taken; and where something is to be woken later, no longer than until the soonest
        // of that is due, which then goes into its line.
        while (!m_stopping && !somethingToTake()) {
            const std::int64_t notYetUs = wakeWhatIsDue();
            if (somethingToTake()) {
                break;
            }
            if (notYetUs == 0) {
                m_something.wait(guard);
            } else {
                m_something.wait_for(guard, std::chrono::microseconds(notYetUs));
            }
        }
        if (m_stopping) {
            return;
        }
        // Urgent first, while urgent work has fewer than all threads but one (where there are three or more:
        // previews keep one), or while no preview waits; a preview only while previews have fewer than all
        // threads but one.
        const bool preview = !urgentMayRun();
        std::deque<Work*>& line = preview ? m_previews : m_urgent;
        Work* work = line.front();
        line.pop_front();
        m_previewsRunning += preview ? 1 : 0;
        m_urgentRunning += preview ? 0 : 1;
        m_running[index] = work;
        // From here on, a new wake() puts it in line again: what it then finds may be gone already
        // or not, and running once for nothing costs nothing.
        work->m_waiting.store(false, std::memory_order_release);
        guard.unlock();

        // Nothing of ours throws; what the standard library might throw (no memory) must not end
        // the thread, and with it every stream's pictures.
        try {
            work->run();
        } catch (...) {
            m_thrown.fetch_add(1, std::memory_order_relaxed);
        }

        guard.lock();
        m_running[index] = nullptr;
        // Work of that line that waits may run now, on a thread that is idle.
        if (preview) {
            --m_previewsRunning;
        } else {
            --m_urgentRunning;
        }
        m_something.notify_one();
        // Retired while it ran, and run by no other thread now: it ends here, out of line (it may have
        // woken itself as it ran), and without the lock.
        std::function<void()> end;
        if (const auto retired = std::ranges::find(m_retiring, work, &std::pair<Work*, std::function<void()>>::first);
            retired != m_retiring.end() && std::ranges::find(m_running, work) == m_running.end()) {
            end = std::move(retired->second);
            m_retiring.erase(retired);
            std::erase(m_urgent, work);
            std::erase(m_previews, work);
            forgetLater(work);
        }
        if (end) {
            guard.unlock();
            end();
            guard.lock();
        }
        m_finished.notify_all();
    }
}

} // namespace qkvm::engine
