#pragma once

#include <cstdint>
#include <set>

namespace qkvm::input {

// A key as Windows tells of it to whoever listens for every key: the keyboard's own number for
// the key (its scan code, set 1), whether it is one of the keys that came later than the first
// PC's and are told apart by a mark (Enter on the number block, the arrows, right Ctrl), and
// Windows' name for what the key means.
struct Pressed {
    std::uint32_t scanCode = 0;
    bool extended = false;
    std::uint32_t virtualKey = 0;
};

// USB's numbers for some keys (HID usage page 7, the keyboard's).
inline constexpr std::uint16_t usageDelete = 0x4C;
inline constexpr std::uint16_t usageLeftCtrl = 0xE0;
inline constexpr std::uint16_t usageLeftAlt = 0xE2;
inline constexpr std::uint16_t usageLeftWindows = 0xE3;
inline constexpr std::uint16_t usageRightCtrl = 0xE4;
inline constexpr std::uint16_t usageRightWindows = 0xE7;

// One of the eight modifiers: Ctrl, Shift, Alt and the Windows key, left and right.
constexpr bool isModifier(std::uint16_t usage) noexcept
{
    return usage >= usageLeftCtrl && usage <= usageRightWindows;
}

// The USB number of a physical key, which is what a KVM types with: a key is where it is on the
// keyboard, whatever is printed on it, and the machine at the far end makes a letter of it with
// its own layout. 0 for a key that has none there (the keys for sound and the like are on
// another page), and for what Windows makes up without a finger pressing anything: the left
// Ctrl it puts before the right Alt of a layout that has AltGr.
//
// By the keyboard's number and its mark wherever those say which key it is, and by Windows' name
// only where they do not: Windows names some keys by what else is held (Num Lock is "Pause" to
// it under Ctrl), and the key is the same key all the while.
std::uint16_t usageOf(const Pressed& key) noexcept;

// The keys that are down on the machine at the far end, as far as this end has said.
class KeysDown {
public:
    // A key went down or up here. True where that is to be said: not for a key that is down
    // there already (Windows repeats a held key, the far machine repeats by itself), and not
    // for a key going up that never went down there (it was pressed before the machine was
    // taken, or it was kept back).
    bool take(std::uint16_t usage, bool down);
    bool any() const noexcept { return !m_down.empty(); }
    // Everything is let go at the far end, by whoever said so there.
    void clear() noexcept { m_down.clear(); }

private:
    std::set<std::uint16_t> m_down;
};

// What a key does while a machine is controlled. Every key goes to the machine, but one: the
// leave-key, which is kept back (SPEC.md, M3). Pressed and let go by itself, it gives keyboard
// and mouse back. Held with Del, it is Ctrl+Alt+Del, which no program can be handed by Windows.
// Held with any other key it does nothing, and that key does nothing either: such pairs are
// kept for what the program may do later. One of these is for one time of controlling a machine:
// the next time begins with a new one.
//
// A key that went down while the leave-key was down is the leave-key's until it was let go
// itself: nothing of it is ever sent. Not when Windows says it again, as it does of a held key,
// and not after the leave-key was let go, which whoever uses the right Ctrl as a Ctrl does first
// as often as not: sent then, the machine would hold that key bare. And so with the Del that
// made Ctrl+Alt+Del. The rule lives here, where the leave-key is known.
//
// A key that is only said again is no key going down: one that was down before the leave-key,
// and is said again under it (a second keyboard goes on saying its key), uses the leave-key for
// nothing, and letting the leave-key go leaves.
class ControlKeys {
public:
    explicit ControlKeys(std::uint16_t leaveKey = usageRightCtrl) : m_leaveKey(leaveKey) {}

    enum class Does {
        Nothing,    // kept back
        Send,       // `usage` goes to the machine, down or up
        Leave,      // keyboard and mouse are given back
        CtrlAltDel, // Ctrl+Alt+Del is pressed on the machine
    };
    struct Step {
        Does does = Does::Nothing;
        std::uint16_t usage = 0;
        bool down = false;

        bool operator==(const Step&) const = default;
    };

    // A key went down or up while the machine is controlled. `saidAgain`, of a key that goes
    // down: it is down at the far end already, and is only said again.
    Step take(std::uint16_t usage, bool down, bool saidAgain = false);

    std::uint16_t leaveKey() const noexcept { return m_leaveKey; }
    // One of the keys that are kept back so, and not let go yet, is a modifier: at the far end
    // it is up, and a key that is pressed under it here would arrive there bare.
    bool keepsBackAModifier() const noexcept;
    // The keys that are kept back so: for whoever says which.
    const std::set<std::uint16_t>& keptBack() const noexcept { return m_keptBack; }

private:
    std::uint16_t m_leaveKey;
    bool m_leaveIsDown = false;
    // Another key was pressed while the leave-key was down: letting it go leaves nothing.
    bool m_leaveWasUsed = false;
    // The keys that went down while the leave-key was held, each until it was let go: nothing
    // of them is sent, going up either.
    std::set<std::uint16_t> m_keptBack;
};

} // namespace qkvm::input
