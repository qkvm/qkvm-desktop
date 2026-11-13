#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <windows.h>

#include "../win/crash_report.h"

namespace {
// Where what is done as well at a crash says that it ran (its name made before anything crashes: nothing is
// made then).
std::array<wchar_t, 1024> ranAt{};

void sayItRan() noexcept
{
    const HANDLE file =
        CreateFileW(ranAt.data(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, "ran", 3, &written, nullptr);
    CloseHandle(file);
}
} // namespace

// Dedicated disposable child, never the application or an owner's profile.
int wmain(int count, wchar_t* arguments[])
{
    if (count != 3)
        return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    const std::wstring ran = std::wstring(arguments[1]) + L"\\at-a-crash.ran";
    if (ran.size() >= ranAt.size())
        return 2;
    std::ranges::copy(ran, ranAt.begin());
    const qkvm::ui::CrashReport report{std::filesystem::path(arguments[1])};
    qkvm::ui::CrashReport::alsoAtACrash(&sayItRan);
    const std::wstring mode(arguments[2]);
    if (mode == L"exception")
        RaiseException(0xe0424b56, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    if (mode == L"terminate")
        std::terminate();
    if (mode == L"abort")
        std::abort();
    if (mode == L"interrupted")
        TerminateProcess(GetCurrentProcess(), 3);
    return mode == L"normal" ? 0 : 2;
}
