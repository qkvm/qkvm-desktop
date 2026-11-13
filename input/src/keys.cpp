#include <qkvm/input/keys.h>

#include <array>

namespace qkvm::input {

namespace {

// Windows' name for Pause, whose scan code alone does not say that it is not Num Lock: listeners
// for every key are told 0x45 for both, with the mark on the one that never had it.
constexpr std::uint32_t virtualPause = 0x13;
// The name Windows gives a key it cannot name.
constexpr std::uint32_t virtualNone = 0xFF;

constexpr std::uint16_t usagePause = 0x48;
constexpr std::uint16_t usageNumLock = 0x53;

// Scan code (set 1) to USB usage, for the keys without the mark. By the table Microsoft
// publishes for it (USB HID to PS/2 scan code translation); 0: no key of the keyboard's page.
constexpr std::array<std::uint16_t, 0x80> plain = {
    0x00, 0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, // 00: none, Esc, 1 2 3 4 5 6
    0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B, // 08: 7 8 9 0 - =, Backspace, Tab
    0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, // 10: Q W E R T Y U I
    0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16, // 18: O P [ ], Enter, left Ctrl, A S
    0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, // 20: D F G H J K L ;
    0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19, // 28: ' `, left Shift, \, Z X C V
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55, // 30: B N M , . /, right Shift, block *
    0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, // 38: left Alt, Space, Caps Lock, F1 to F5
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, // 40: F6 to F10, Num Lock, Scroll Lock, block 7
    0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59, // 48: block 8 9 - 4 5 6 + 1
    0x5A, 0x5B, 0x62, 0x63, 0x46, 0x00, 0x64, 0x44, // 50: block 2 3 0 ., Alt+PrtSc, none, beside left Shift, F11
    0x45, 0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 58: F12, block =
    0x00, 0x00, 0x00, 0x00, 0x68, 0x69, 0x6A, 0x6B, // 60: F13 to F16 from 64
    0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x00, // 68: F17 to F23
    0x88, 0x00, 0x00, 0x87, 0x00, 0x00, 0x73, 0x00, // 70: kana, ro at 73, F24 at 76
    0x00, 0x8A, 0x00, 0x8B, 0x00, 0x89, 0x85, 0x00, // 78: henkan 79, muhenkan 7B, yen 7D, block comma 7E
};

// The same for the keys with the mark.
constexpr std::uint16_t marked(std::uint32_t scanCode) noexcept
{
    switch (scanCode) {
    case 0x1C:
        return 0x58; // Enter on the number block
    case 0x1D:
        return 0xE4; // right Ctrl
    case 0x35:
        return 0x54; // / on the number block
    case 0x37:
        return 0x46; // Print Screen
    case 0x38:
        return 0xE6; // right Alt
    case 0x45:
        return usageNumLock;
    case 0x46:
        return usagePause; // Ctrl+Pause
    case 0x47:
        return 0x4A; // Home
    case 0x48:
        return 0x52; // up
    case 0x49:
        return 0x4B; // Page Up
    case 0x4B:
        return 0x50; // left
    case 0x4D:
        return 0x4F; // right
    case 0x4F:
        return 0x4D; // End
    case 0x50:
        return 0x51; // down
    case 0x51:
        return 0x4E; // Page Down
    case 0x52:
        return 0x49; // Insert
    case 0x53:
        return 0x4C; // Delete
    case 0x5B:
        return 0xE3; // left Windows key
    case 0x5C:
        return 0xE7; // right Windows key
    case 0x5D:
        return 0x65; // the menu key
    default:
        return 0;
    }
}

} // namespace

std::uint16_t usageOf(const Pressed& key) noexcept
{
    if (key.virtualKey == virtualNone) {
        return 0;
    }
    // The mark first: with it, 0x45 is Num Lock whatever Windows calls it (under Ctrl it calls
    // it Pause, and the key that is held is Num Lock all the same). Set, and not shown by a test:
    // that Windows tells a listener of Num Lock with the mark under Ctrl too. Where it does not,
    // the key is read here as Pause while Ctrl is held and as Num Lock once Ctrl was let go: two
    // USB numbers for one key. Nothing here can mend that. Whoever keeps what is down knows the
    // two places of that number as one key's while one of them is down (ui/win/keyboard_hook,
    // `KeysHeld`): the key goes up as what it went down as, whichever of the two Ctrl was let go
    // before. What it went down as may then be the wrong one of the two, for the machine.
    if (key.extended) {
        return marked(key.scanCode);
    }
    if (key.virtualKey == virtualPause) {
        return usagePause;
    }
    // (The left Ctrl that Windows makes up before the right Alt of a layout with AltGr has a
    // number no key has, 0x21D, and so is none.)
    return key.scanCode < plain.size() ? plain.at(key.scanCode) : 0;
}

bool KeysDown::take(std::uint16_t usage, bool down)
{
    if (usage == 0) {
        return false;
    }
    return down ? m_down.insert(usage).second : m_down.erase(usage) > 0;
}

ControlKeys::Step ControlKeys::take(std::uint16_t usage, bool down, bool saidAgain)
{
    if (usage == 0) {
        return {};
    }
    if (usage == m_leaveKey) {
        if (down) {
            // (Windows says a held key again and again: the first time is the one that counts.)
            if (!m_leaveIsDown) {
                m_leaveIsDown = true;
                m_leaveWasUsed = false;
            }
            return {};
        }
        const bool leaves = m_leaveIsDown && !m_leaveWasUsed;
        m_leaveIsDown = false;
        return {leaves ? Does::Leave : Does::Nothing, 0, false};
    }
    // A key that went down with the leave-key held never went down at the far end, and is the
    // leave-key's until it was let go itself: said again (Windows says a held key again and
    // again), with the leave-key up by then or down once more, it is no press, or the machine
    // would hold it bare. And no key going down: it uses the leave-key for nothing.
    if (m_keptBack.contains(usage)) {
        if (!down) {
            m_keptBack.erase(usage);
        }
        return {};
    }
    // (Nor does a key that is down at the far end and is only said again.)
    if (m_leaveIsDown && down && !saidAgain) {
        m_keptBack.insert(usage);
        m_leaveWasUsed = true;
        return {usage == usageDelete ? Does::CtrlAltDel : Does::Nothing, 0, false};
    }
    return {Does::Send, usage, down};
}

bool ControlKeys::keepsBackAModifier() const noexcept
{
    const auto modifier = m_keptBack.lower_bound(usageLeftCtrl);
    return modifier != m_keptBack.end() && isModifier(*modifier);
}

} // namespace qkvm::input
