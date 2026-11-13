#include "hand_over_end.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include <windows.h>

namespace qkvm::ui {

using Clock = std::chrono::steady_clock;

struct HandOverEnd::State {
    std::mutex lock;
    std::condition_variable moved;
    Clock::time_point deadline = Clock::time_point::max();
    bool cancelled = false;
    std::function<void()> end;
};

HandOverEnd::HandOverEnd(HandOverPace pace, std::function<void()> end)
    : m_pace(pace), m_state(std::make_shared<State>())
{
    m_state->end = end ? std::move(end) : [] { TerminateProcess(GetCurrentProcess(), 0); };
}

HandOverEnd::~HandOverEnd() = default;

void HandOverEnd::begin()
{
    if (m_begun) {
        return;
    }
    m_begun = true;
    {
        const std::lock_guard held(m_state->lock);
        m_state->deadline = Clock::now() + m_pace.whole;
    }
    // Detached, with the state its own: a program that ends by itself first takes it along, waiting.
    std::thread([state = m_state] {
        std::unique_lock held(state->lock);
        while (!state->cancelled) {
            const Clock::time_point until = state->deadline;
            if (Clock::now() >= until) {
                std::function<void()> end = state->end;
                held.unlock();
                end();
                return;
            }
            state->moved.wait_until(held, until);
        }
    }).detach();
}

void HandOverEnd::goodbyesOver()
{
    if (!m_begun) {
        return;
    }
    {
        const std::lock_guard held(m_state->lock);
        m_state->deadline = std::min(m_state->deadline, Clock::now() + m_pace.afterGoodbyes);
    }
    m_state->moved.notify_all();
}

void HandOverEnd::cancel()
{
    {
        const std::lock_guard held(m_state->lock);
        m_state->cancelled = true;
    }
    m_state->moved.notify_all();
}

} // namespace qkvm::ui
