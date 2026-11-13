#pragma once

#include <cstdint>

#include <qkvm/input/keys.h>

namespace qkvm::ui {

// Windows virtual-key names for the supported physical release keys. Shared by the
// keyboard listener and the independent mouse watchdog; no operating-system calls.
constexpr int leaveKeyOfWindows(std::uint16_t usage)
{
    return usage == 0x45 ? 0x7B : usage == 0x47 ? 0x91 : 0xA3; // F12, Scroll Lock, right Ctrl.
}

constexpr std::uint16_t supportedLeaveKey(std::uint16_t usage)
{
    return usage == 0x45 || usage == 0x47 ? usage : input::usageRightCtrl;
}

} // namespace qkvm::ui
