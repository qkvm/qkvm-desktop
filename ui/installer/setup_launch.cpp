#include "setup_launch.h"
#include <filesystem>
#include <vector>
namespace qkvm::installer {
namespace {
std::wstring quoted(const std::wstring& value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else {
            result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            result += c;
        }
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
bool allowed(const std::vector<std::wstring>& args, bool check)
{
    for (const auto& arg : args)
        if (arg.find(L'\0') != std::wstring::npos)
            return false;
    if (check)
        return args == std::vector<std::wstring>{L"--check-helper"};
    return (args.size() == 1 && (args[0] == L"--recover" || args[0] == L"--rollback")) ||
           (args.size() == 4 && args[0] == L"--install" && !args[1].empty() && args[2] == L"--version" &&
            !args[3].empty());
}
struct Attributes {
    std::vector<unsigned char> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~Attributes()
    {
        if (value != nullptr)
            DeleteProcThreadAttributeList(value);
    }
};
} // namespace
std::uint32_t launchSetup(const std::wstring& executable, const std::vector<std::wstring>& arguments, SetupHop hop,
                          bool check, const SetupLaunchApi& api)
{
    if (executable.empty() || executable.find(L'\0') != std::wstring::npos || !allowed(arguments, check))
        return ERROR_INVALID_PARAMETER;
    std::wstring command = quoted(executable);
    if (hop == SetupHop::Relay)
        command += L" --relay";
    for (const auto& arg : arguments)
        command += L' ' + quoted(arg);
    Attributes attributes;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    DWORD flags = check ? CREATE_NO_WINDOW : DETACHED_PROCESS;
    DWORD policy = PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE;
    if (hop == SetupHop::Relay) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes.bytes.resize(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.data());
        if (size == 0 || !InitializeProcThreadAttributeList(list, 1, 0, &size))
            return GetLastError();
        attributes.value = list;
        if (!api.attribute(list, 0, PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY, &policy, sizeof(policy), nullptr,
                           nullptr))
            return GetLastError();
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = list;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    PROCESS_INFORMATION child{};
    const auto directory = std::filesystem::path(executable).parent_path();
    if (!api.create(executable.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, directory.c_str(),
                    &startup.StartupInfo, &child))
        return GetLastError();
    CloseHandle(child.hThread);
    DWORD result = 0;
    if (check) {
        const DWORD wait = WaitForSingleObject(child.hProcess, 10000);
        if (wait != WAIT_OBJECT_0) {
            result = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            // Only our freshly created, read-only helper check, never a user session.
            TerminateProcess(child.hProcess, result);
            WaitForSingleObject(child.hProcess, 5000);
        } else if (!GetExitCodeProcess(child.hProcess, &result)) {
            result = GetLastError();
        }
    }
    CloseHandle(child.hProcess);
    return result;
}
} // namespace qkvm::installer