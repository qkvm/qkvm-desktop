#include "native_health.h"
#include "error_code.h"
#include "health_exchange.h"
#include <array>
#include <winrt/base.h>
#include <wrl/client.h>
namespace qkvm::installer {
namespace {
struct Handle {
    HANDLE value;
    ~Handle()
    {
        if (value != nullptr)
            CloseHandle(value);
    }
};
bool failed(std::wstring& problem, const wchar_t* domain, std::uint32_t code)
{
    // (What became of the installation record is the result's to say: after a put-back it holds no
    // pending installation any more.)
    problem = L"Startup check failed (" + errorCode(domain, code) + L"). No user session was terminated.";
    return false;
}
} // namespace
bool healthy(const Package& package, std::wstring& problem, const NativeHealthApi& api)
{
    // A successful restore must keep the failed candidate's diagnostic for Setup's dialog.
    try {
        const HealthRegistration registration = api.registered();
        if (!sameVersion(registration.package, package)) {
            problem = L"Startup check: the registered version does not match the candidate.";
            return false;
        }
        HealthExchange exchange(package.family, registration.executable);
        Microsoft::WRL::ComPtr<IApplicationActivationManager> activation;
        if (!api.activate) {
            const HRESULT result = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                                    IID_PPV_ARGS(&activation));
            if (FAILED(result))
                return failed(problem, L"HRESULT", static_cast<std::uint32_t>(result));
        }
        FILETIME activationBegan{};
        GetSystemTimeAsFileTime(&activationBegan);
        const auto appId = package.family + L"!App";
        const auto arguments = exchange.arguments();
        DWORD pid = 0;
        const HRESULT activated =
            api.activate ? api.activate(appId.c_str(), arguments.c_str(), AO_NOERRORUI, &pid)
                         : activation->ActivateApplication(appId.c_str(), arguments.c_str(), AO_NOERRORUI, &pid);
        if (FAILED(activated))
            return failed(problem, L"HRESULT", static_cast<std::uint32_t>(activated));
        if (pid == 0)
            return failed(problem, L"Win32", ERROR_INVALID_PARAMETER);
        const Handle child{
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid)};
        if (child.value == nullptr)
            return failed(problem, L"Win32", GetLastError());
        std::array<wchar_t, 128> family{};
        std::array<wchar_t, 32768> image{};
        UINT32 familyLength = static_cast<UINT32>(family.size());
        DWORD imageLength = static_cast<DWORD>(image.size());
        FILETIME createdAt{}, exitedAt{}, kernel{}, user{};
        const LONG familyStatus = api.family(child.value, &familyLength, family.data());
        if (familyStatus != ERROR_SUCCESS)
            return failed(problem, L"Win32", static_cast<std::uint32_t>(familyStatus));
        if (!QueryFullProcessImageNameW(child.value, 0, image.data(), &imageLength) ||
            !GetProcessTimes(child.value, &createdAt, &exitedAt, &kernel, &user))
            return failed(problem, L"Win32", GetLastError());
        if (family.back() != L'\0' || package.family != family.data() ||
            _wcsicmp(registration.executable.c_str(), image.data()) != 0 ||
            CompareFileTime(&createdAt, &activationBegan) < 0)
            return failed(problem, L"process identity", ERROR_INVALID_DATA);
        // Only this verified, newly activated diagnostic may be waited on or stopped.
        const auto stop = [&] {
            if (!TerminateProcess(child.value, ERROR_TIMEOUT))
                return GetLastError();
            const DWORD waited = WaitForSingleObject(child.value, api.exitTimeoutMs);
            return waited == WAIT_FAILED ? GetLastError() : DWORD(ERROR_TIMEOUT);
        };
        const HealthResult result = exchange.wait(pid, child.value, api.replyTimeoutMs);
        if (result.kind == HealthResult::Kind::ProcessExit) {
            failed(problem, L"process exit", result.code);
            problem += L" No health reply was received.";
            return false; // Even exit zero without a reply is a failure.
        }
        if (result.kind == HealthResult::Kind::Win32) {
            if (result.code == ERROR_TIMEOUT)
                return failed(problem, L"Win32", stop());
            return failed(problem, L"Win32", result.code);
        }
        const DWORD exited = WaitForSingleObject(child.value, api.exitTimeoutMs);
        if (exited != WAIT_OBJECT_0)
            return failed(problem, L"Win32", exited == WAIT_TIMEOUT ? stop() : GetLastError());
        DWORD exitCode = 1;
        if (!GetExitCodeProcess(child.value, &exitCode))
            return failed(problem, L"Win32", GetLastError());
        if (result.code != 0)
            return failed(problem, L"diagnostic", result.code);
        if (exitCode != 0)
            return failed(problem, L"process exit", exitCode);
        return true;
    } catch (const winrt::hresult_error& error) {
        return failed(problem, L"HRESULT", static_cast<std::uint32_t>(error.code().value));
    } catch (...) {
        problem = L"Startup check failed (exception while reading registration or checking the diagnostic).";
        return false;
    }
}
} // namespace qkvm::installer