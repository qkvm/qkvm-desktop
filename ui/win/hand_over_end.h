#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace qkvm::ui {

// How long the end of a program that has handed over to Setup waits (HandOverEnd).
struct HandOverPace {
    std::chrono::milliseconds whole{50'000};
    std::chrono::milliseconds afterGoodbyes{9'000};
};

// The end of a program that has handed over to Setup. Setup waits up to 60 s for the program's single-instance mutex,
// which goes only with the process, and gives up after that, changing nothing; so the program, once it has started
// Setup, closes its windows and quits as usual, and is ended by force where that is held up: `afterGoodbyes` after its
// quit's goodbyes are over (the sessions given back, a start into BIOS setup cleared: main's endTheProgram), and in
// any case `whole` after the hand-over, within Setup's minute. A thread of its own waits, and ends the process (exit
// code 0) where it has not ended by itself. Nothing is done before `begin`. From the interface thread.
class HandOverEnd {
public:
    // `end`: what ends the program; none: TerminateProcess of this one (a test's own in a test).
    explicit HandOverEnd(HandOverPace pace = {}, std::function<void()> end = {});
    // The waiting thread is not stopped (an end after main returns is still bounded); a test lets it go first (cancel).
    ~HandOverEnd();
    HandOverEnd(const HandOverEnd&) = delete;
    HandOverEnd& operator=(const HandOverEnd&) = delete;
    HandOverEnd(HandOverEnd&&) = delete;
    HandOverEnd& operator=(HandOverEnd&&) = delete;

    // Setup has started: the end comes `whole` from now at the latest.
    void begin();
    bool begun() const { return m_begun; }
    // The quit's goodbyes are over: the end comes `afterGoodbyes` from now (never later than `whole` allows). Nothing
    // where nothing was begun.
    void goodbyesOver();
    // Tests only: nothing is ended any more.
    void cancel();

private:
    struct State;
    HandOverPace m_pace;
    std::shared_ptr<State> m_state;
    bool m_begun = false;
};

} // namespace qkvm::ui
