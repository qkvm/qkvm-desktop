#include "health_exchange.h"
#include "health_protocol.h"
#include <algorithm>
#include <array>
#include <objbase.h>
#include <stdexcept>
#include <windows.h>

#include <appmodel.h>
namespace qkvm::installer {
namespace {
using Reply = detail::HealthReply;
constexpr auto form = detail::healthForm;
struct Handle {
    HANDLE value = nullptr;
    ~Handle()
    {
        if (value != nullptr)
            CloseHandle(value);
    }
};
struct View {
    Reply* value = nullptr;
    ~View()
    {
        if (value != nullptr)
            UnmapViewOfFile(value);
    }
};
bool validId(const std::wstring& id)
{
    if (id.size() != 38 || id.front() != L'{' || id.back() != L'}')
        return false;
    for (std::size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (id[i] != L'-')
                return false;
        } else if (!((id[i] >= L'0' && id[i] <= L'9') || (id[i] >= L'A' && id[i] <= L'F') ||
                     (id[i] >= L'a' && id[i] <= L'f'))) {
            return false;
        }
    }
    return true;
}
std::wstring objectName(const std::wstring& id)
{
    return L"Local\\qKVM.Health." + id;
}
bool identityMatches(const Reply& reply, const HealthIdentityApi& api)
{
    std::array<wchar_t, 128> family{};
    UINT32 length = static_cast<UINT32>(family.size());
    const LONG status = api.family(&length, family.data());
    if (status != ERROR_SUCCESS && status != APPMODEL_ERROR_NO_PACKAGE)
        return false;
    std::array<wchar_t, 32768> executable{};
    const DWORD count = api.image(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    return count > 0 && count < executable.size() && reply.family[127] == L'\0' && reply.executable[32767] == L'\0' &&
           wcscmp(reply.family, family.data()) == 0 && _wcsicmp(reply.executable, executable.data()) == 0;
}
} // namespace
struct HealthExchange::Impl {
    std::wstring id;
    Handle mapping;
    Handle ready;
    Handle release;
    View view;
    ~Impl()
    {
        if (release.value != nullptr)
            SetEvent(release.value);
    }
};
HealthExchange::HealthExchange(const std::wstring& family, const std::wstring& executable)
    : m_impl(std::make_unique<Impl>())
{
    if (family.size() >= 128 || executable.empty() || executable.size() >= 32768)
        throw std::runtime_error("The startup-check identity is invalid.");
    GUID guid{};
    std::array<wchar_t, 40> text{};
    if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) == 0)
        throw std::runtime_error("A startup-check identity could not be created.");
    m_impl->id = text.data();
    const auto name = objectName(m_impl->id);
    m_impl->mapping.value =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Reply), name.c_str());
    const DWORD mappingStatus = GetLastError();
    m_impl->ready.value = CreateEventW(nullptr, TRUE, FALSE, (name + L".ready").c_str());
    const DWORD eventStatus = GetLastError();
    m_impl->release.value = CreateEventW(nullptr, TRUE, FALSE, (name + L".release").c_str());
    const DWORD releaseStatus = GetLastError();
    if (m_impl->mapping.value == nullptr || m_impl->ready.value == nullptr || m_impl->release.value == nullptr ||
        mappingStatus == ERROR_ALREADY_EXISTS || eventStatus == ERROR_ALREADY_EXISTS ||
        releaseStatus == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("The startup-check exchange could not be created.");
    m_impl->view.value =
        static_cast<Reply*>(MapViewOfFile(m_impl->mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Reply)));
    if (m_impl->view.value == nullptr)
        throw std::runtime_error("The startup-check exchange could not be opened.");
    Reply& reply = *m_impl->view.value;
    reply = Reply{};
    std::copy(family.begin(), family.end(), reply.family);
    std::copy(executable.begin(), executable.end(), reply.executable);
}
HealthExchange::~HealthExchange() = default;
const std::wstring& HealthExchange::id() const
{
    return m_impl->id;
}
std::wstring HealthExchange::arguments() const
{
    return L"--check-package --health-check " + m_impl->id;
}
std::uint32_t HealthExchange::result(std::uint32_t pid, std::uint32_t timeoutMs) const
{
    return wait(pid, nullptr, timeoutMs).code;
}
HealthResult HealthExchange::wait(std::uint32_t pid, HANDLE process, std::uint32_t timeoutMs) const
{
    const std::array<HANDLE, 2> events{m_impl->ready.value, process};
    const DWORD waiting = WaitForMultipleObjects(process == nullptr ? 1 : 2, events.data(), FALSE, timeoutMs);
    const DWORD error = GetLastError();
    SetEvent(m_impl->release.value);
    if (waiting == WAIT_OBJECT_0 + 1 && process != nullptr) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process, &exitCode))
            return {HealthResult::Kind::Win32, GetLastError()};
        return {HealthResult::Kind::ProcessExit, exitCode};
    }
    if (waiting != WAIT_OBJECT_0)
        return {HealthResult::Kind::Win32, waiting == WAIT_TIMEOUT ? ERROR_TIMEOUT : error};
    const Reply snapshot = *m_impl->view.value;
    if (snapshot.version != form || snapshot.pid != pid || pid == 0 || snapshot.claimed != 1)
        return {HealthResult::Kind::Win32, ERROR_INVALID_DATA};
    return {HealthResult::Kind::Reply, snapshot.code};
}
bool reportHealth(const std::wstring& id, std::uint32_t result, const HealthIdentityApi& api)
{
    if (!validId(id))
        return false;
    const auto name = objectName(id);
    const Handle mapping{OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str())};
    const Handle ready{OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L".ready").c_str())};
    const Handle release{OpenEventW(SYNCHRONIZE, FALSE, (name + L".release").c_str())};
    if (mapping.value == nullptr || ready.value == nullptr || release.value == nullptr)
        return false;
    const View view{
        static_cast<Reply*>(MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Reply)))};
    if (view.value == nullptr)
        return false;
    const Reply snapshot = *view.value;
    if (snapshot.version != form || !identityMatches(snapshot, api) ||
        InterlockedCompareExchange(&view.value->claimed, 1, 0) != 0)
        return false;
    view.value->pid = GetCurrentProcessId();
    view.value->code = result;
    // Keep the activated process alive until Setup has opened its process handle.
    // If Setup disappears, the diagnostic still leaves within a bounded time.
    return SetEvent(ready.value) != FALSE && WaitForSingleObject(release.value, 30000) == WAIT_OBJECT_0;
}
bool hasNoPackageIdentity()
{
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) == APPMODEL_ERROR_NO_PACKAGE;
}
} // namespace qkvm::installer
