#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <windows.h>

#include <appmodel.h>
namespace qkvm::installer {
struct HealthResult {
    enum class Kind { Reply, ProcessExit, Win32 };
    Kind kind;
    std::uint32_t code;
};
// A single, bounded, local Windows object exchange. No socket, profile or result file.
// The parent keeps it alive even if the activated diagnostic exits before OpenProcess.
class HealthExchange {
public:
    HealthExchange(const std::wstring& family, const std::wstring& executable);
    ~HealthExchange();
    HealthExchange(const HealthExchange&) = delete;
    HealthExchange& operator=(const HealthExchange&) = delete;
    std::wstring arguments() const;
    const std::wstring& id() const;
    std::uint32_t result(std::uint32_t pid, std::uint32_t timeoutMs) const;
    HealthResult wait(std::uint32_t pid, HANDLE process, std::uint32_t timeoutMs) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
struct HealthIdentityApi {
    std::function<LONG(UINT32*, PWSTR)> family = GetCurrentPackageFamilyName;
    std::function<DWORD(HMODULE, LPWSTR, DWORD)> image = GetModuleFileNameW;
};
// The diagnostic verifies its actual package family and executable before replying.
bool reportHealth(const std::wstring& id, std::uint32_t result, const HealthIdentityApi& api = {});
bool hasNoPackageIdentity();
} // namespace qkvm::installer
