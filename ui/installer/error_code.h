#pragma once
#include <array>
#include <cstdint>
#include <cwchar>
#include <string>
namespace qkvm::installer {
inline std::wstring errorCode(const wchar_t* domain, std::uint32_t code)
{
    std::array<wchar_t, 11> hex{};
    swprintf_s(hex.data(), hex.size(), L"0x%08lX", static_cast<unsigned long>(code));
    return std::wstring(domain) + L' ' + hex.data();
}
} // namespace qkvm::installer