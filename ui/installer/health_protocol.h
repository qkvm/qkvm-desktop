#pragma once
#include <cstdint>
#include <windows.h>
namespace qkvm::installer::detail {
inline constexpr std::uint32_t healthForm = 0x514B4801;
struct HealthReply {
    std::uint32_t version = healthForm;
    std::uint32_t pid = 0;
    std::uint32_t code = ERROR_INVALID_DATA;
    LONG claimed = 0;
    wchar_t family[128]{};
    wchar_t executable[32768]{};
};
} // namespace qkvm::installer::detail