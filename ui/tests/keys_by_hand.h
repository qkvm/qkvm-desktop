#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "key_source.h"

namespace qkvm::ui {

// A key source that hears no keyboard: a test presses its keys by hand, as fingers would, and
// looks at what it was told. For the tests only: nothing of Windows is in it, and no key of the
// PC a test runs on is listened for.
//
// It keeps what Windows' own way of hearing keys keeps, and no more: which keys it heard go down
// since it began to listen, which of them it kept from Windows, and which keys Windows has as
// down (those that went down while nothing was swallowed). A key that went down before it began
// anew, and that Windows was never shown, it does not know of: it forgets as the real one does.
// And it tells what that one tells: while it swallows, every press and every release, each press
// with whether it was kept from Windows; while it does not, only the release of a key that it had
// kept from Windows itself.
//
// And when, as the real one: a key that goes down may be told a moment late (`holdsBackPresses`:
// as while Windows' two accounts of the keyboard are not at one), and is told when the test says
// so (`tellWhatWaits`); a key that goes up is told at once, but never before its own going down,
// and a modifier never before any key going down that waits; a key that is down and is only said
// again is told at once, and not at all while its own going down still waits. With the key that
// went up and left no key down, and nothing waiting, it says so. And each key is told with its
// number, in the order they were heard in, from 1 again whenever none is on its way.
class KeysByHand final : public KeySource {
public:
    using KeySource::KeySource;

    bool begin(std::uint16_t leaveKey = input::usageRightCtrl) override
    {
        chosenLeaveKey = leaveKey;
        ++begun;
        if (listens && !lost && canBegin) {
            // (It listens, and goes on.)
            return true;
        }
        // Anew, or not at all: what went down before this is known only where Windows has it.
        heard.clear();
        kept.clear();
        forgetWhatWaits();
        m_unsaidBehind = false;
        lost = false;
        swallowing = false;
        listens = canBegin;
        return listens;
    }
    void end() override
    {
        ++ended;
        listens = false;
        swallowing = false;
        lost = false;
        heard.clear();
        kept.clear();
        forgetWhatWaits();
        m_unsaidBehind = false;
    }
    std::uint16_t chosenLeaveKey = input::usageRightCtrl;
    std::vector<input::Pressed> whatIsDown() const override
    {
        // What it heard go down since it began, and what Windows knows of.
        std::vector<input::Pressed> down = windowsHas;
        if (listens) {
            for (const input::Pressed& key : heard) {
                if (!isIn(down, key)) {
                    down.push_back(key);
                }
            }
        }
        // (And what happens in the moment after that was looked up.)
        if (whileAskedWhatIsDown) {
            whileAskedWhatIsDown();
        }
        return down;
    }
    std::vector<input::Pressed> whatIsKept() const override { return listens ? kept : std::vector<input::Pressed>(); }
    bool isWatched() const override { return watched; }
    bool isToldOfKeyboardsGoing() const override { return toldOfKeyboardsGoing; }
    bool swallows() const override { return swallowing; }
    void setSwallowing(bool swallow) override
    {
        const bool was = swallowing;
        swallowing = swallow && listens && !lost;
        if (swallowing && !was) {
            m_saidTyped = false;
        }
    }
    void stillThere() override { ++saidAlive; }
    OnTheirWay onTheirWay() const override { return {m_last, static_cast<int>(m_waits.size())}; }
    void tellNow() override
    {
        // Why it hears no more, where that was found and not said yet.
        if (m_unsaidBehind) {
            m_unsaidBehind = false;
            emit fellBehind();
        }
    }

    // A finger presses a key, holds it long enough for Windows to say it again, and lets go of
    // it. Heard only while this listens, as a keyboard is.
    void press(const input::Pressed& key)
    {
        if (!isIn(held, key)) {
            held.push_back(key);
        }
        goesDown(key);
    }
    void repeat(const input::Pressed& key) { goesDown(key); }
    void release(const input::Pressed& key)
    {
        const bool windowsHadIt = isIn(windowsHas, key);
        const bool wasKept = isIn(kept, key);
        take(held, key);
        take(windowsHas, key);
        take(heard, key);
        take(kept, key);
        if (!listens) {
            return;
        }
        if (lost) {
            // It swallows no more, and tells only of a key that it had kept from Windows going
            // up: where that is still heard at all.
            if (wasKept && hearsWhatGoesUp) {
                tell({key, false, false}, false);
            }
            return;
        }
        // While nothing is swallowed, only the going up of a key that was kept from Windows.
        if (swallowing || wasKept) {
            tell({key, false, swallowing && !stalled && !windowsHadIt}, false);
        }
    }
    // What waited to be told is told, in its order.
    void tellWhatWaits()
    {
        holdsBackPresses = false;
        while (!m_waits.empty()) {
            const Waiting next = m_waits.front();
            m_waits.erase(m_waits.begin());
            say(next);
        }
    }
    // Another program types a key into this PC: it goes to Windows, and is told to nobody. That
    // such a key came is said, once for each time keys are swallowed.
    void typeAsAProgram()
    {
        if (listens && swallowing && !m_saidTyped) {
            m_saidTyped = true;
            emit typedByAProgram();
        }
    }
    // The leave-key was pressed and let go where the keys are heard, and that gave the keyboard
    // back there, as Windows' own way of hearing them does it.
    void leaveByItself() { swallowing = false; }
    // Keys are heard no more, as the real one finds and says it: nothing more is swallowed until
    // it begins anew, with a hook that knows nothing of before.
    void loseHearing(Lost why)
    {
        lost = true;
        swallowing = false;
        forgetWhatWaits();
        emit lostHearing(why);
    }
    void fallBehind()
    {
        lost = true;
        swallowing = false;
        forgetWhatWaits();
        emit fellBehind();
    }
    // Windows says that a keyboard was taken away from this PC: unplugged, with whatever was
    // held on it. While keys are swallowed nothing is heard any more, as the real one has it;
    // while they are not, it is nothing. No "up" comes for what a finger held there: the keys
    // that are held stay as they are, here as in the real one's table.
    void aKeyboardGoes()
    {
        if (listens && swallowing && toldOfKeyboardsGoing) {
            loseHearing(Lost::AKeyboardWent);
        }
    }
    // The same, found where the keys are heard and not said yet: the word for it is still on its
    // way to whoever listens, and is said when it is asked for (`tellNow`).
    void fallBehindUnsaid()
    {
        lost = true;
        swallowing = false;
        forgetWhatWaits();
        m_unsaidBehind = true;
    }

    bool canBegin = true; // false: as where Windows does not let the program listen
    bool listens = false;
    bool swallowing = false;
    // It said that keys are heard no more, and has not begun anew since.
    bool lost = false;
    // No key is kept from Windows, and a press is told as Windows' own: as the real one tells a
    // press that it could not keep from Windows while it goes on swallowing.
    bool stalled = false;
    // Whether, once keys are heard no more, the going up of a key is still heard by anything of
    // Windows. It is not while Windows shows a screen of its own; nor where its hook is gone and
    // Windows has no other account of the keyboard (over a remote desktop it may have none).
    bool hearsWhatGoesUp = true;
    // Whether Windows' second account of the keyboard is there: false as on a PC that gives none.
    bool watched = true;
    // Whether Windows says when a keyboard is taken away from this PC: false as on a PC where
    // that word cannot be had.
    bool toldOfKeyboardsGoing = true;
    // A key that goes down is not told yet: it waits, as while the two accounts are not at one.
    bool holdsBackPresses = false;
    // The keys that fingers are on; those of them that Windows has as down; those this heard go
    // down since it began; and those of them it kept from Windows.
    std::vector<input::Pressed> held;
    std::vector<input::Pressed> windowsHas;
    std::vector<input::Pressed> heard;
    std::vector<input::Pressed> kept;
    // Run inside whatIsDown(), after what is down was looked up.
    std::function<void()> whileAskedWhatIsDown;
    int begun = 0;
    int ended = 0;
    int saidAlive = 0;

private:
    // A key that is to be told, and may have to wait; with its number, once it has one.
    struct Waiting {
        input::Pressed key;
        bool down = false;
        bool swallowed = false;
        quint64 number = 0;
    };

    static bool same(const input::Pressed& one, const input::Pressed& other)
    {
        return one.scanCode == other.scanCode && one.extended == other.extended;
    }
    static bool isIn(const std::vector<input::Pressed>& keys, const input::Pressed& key)
    {
        return std::ranges::any_of(keys, [&](const input::Pressed& one) { return same(one, key); });
    }
    static void take(std::vector<input::Pressed>& keys, const input::Pressed& key)
    {
        std::erase_if(keys, [&](const input::Pressed& one) { return same(one, key); });
    }
    void forgetWhatWaits()
    {
        m_waits.clear();
        m_last = 0;
    }
    bool pressesWait() const
    {
        return std::ranges::any_of(m_waits, [](const Waiting& one) { return one.down; });
    }
    // Told now, or behind what it may not pass. `saidAgain`, of a key that goes down: it was
    // heard to go down before, and not up since.
    void tell(Waiting what, bool saidAgain)
    {
        const bool itsOwnPressWaits =
            std::ranges::any_of(m_waits, [&](const Waiting& one) { return one.down && same(one.key, what.key); });
        if (what.down && saidAgain && itsOwnPressWaits) {
            // (It adds nothing to the press that waits, and is not told.)
            return;
        }
        what.number = ++m_last;
        // (A key that goes up: behind its own going down, and a modifier behind any press.)
        const bool isAModifier = input::isModifier(input::usageOf(what.key));
        const bool anUpIsHeldBack = itsOwnPressWaits || (isAModifier && pressesWait());
        const bool staysBehind = what.down ? !saidAgain && (holdsBackPresses || !m_waits.empty()) : anUpIsHeldBack;
        if (staysBehind) {
            m_waits.push_back(what);
        } else {
            say(what);
        }
    }
    void say(const Waiting& what)
    {
        // (As the real one numbers them: from 1 again once none is on its way.)
        if (m_waits.empty()) {
            m_last = 0;
        }
        emit keyChanged(what.key, what.down, what.swallowed, what.number);
        if (!what.down && heard.empty() && m_waits.empty()) {
            emit nothingIsDown();
        }
    }
    void goesDown(const input::Pressed& key)
    {
        const bool windowsHadIt = isIn(windowsHas, key);
        if (!listens || lost) {
            // Nobody hears it: it is Windows' key.
            if (!windowsHadIt) {
                windowsHas.push_back(key);
            }
            return;
        }
        const bool saidAgain = isIn(heard, key);
        if (!saidAgain) {
            heard.push_back(key);
        }
        // A press is kept from Windows only where it can be, and is a press of its own only where
        // Windows does not have the key already: what it saw go down, it goes on having.
        const bool canSwallow = swallowing && !stalled;
        if (!canSwallow && !windowsHadIt) {
            windowsHas.push_back(key);
        }
        if (canSwallow && !windowsHadIt && !isIn(kept, key)) {
            kept.push_back(key);
        }
        if (swallowing) {
            tell({key, true, canSwallow && !windowsHadIt}, saidAgain);
        }
    }

    // What waits to be told, in its order, and the number that was given last; whether keys fell
    // behind and that is not said yet; and whether it was said that a program typed, since keys
    // are swallowed.
    std::vector<Waiting> m_waits;
    quint64 m_last = 0;
    bool m_unsaidBehind = false;
    bool m_saidTyped = false;
};

} // namespace qkvm::ui
