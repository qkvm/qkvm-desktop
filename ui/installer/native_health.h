#pragma once
#include "transaction.h"
#include <functional>
#include <windows.h>

#include <appmodel.h>
#include <shobjidl.h>
namespace qkvm::installer {
struct HealthRegistration {
    std::optional<Package> package;
    std::wstring executable;
};
// Only OS boundaries are replaceable. The compiled product implementation still
// opens, identifies, waits for and terminates the actual diagnostic process.
struct NativeHealthApi {
    std::function<HealthRegistration()> registered;
    std::function<HRESULT(LPCWSTR, LPCWSTR, ACTIVATEOPTIONS, DWORD*)> activate;
    std::function<LONG(HANDLE, UINT32*, PWSTR)> family = GetPackageFamilyName;
    DWORD replyTimeoutMs = 30000;
    DWORD exitTimeoutMs = 5000;
};
bool healthy(const Package& package, std::wstring& problem, const NativeHealthApi& api);
} // namespace qkvm::installer