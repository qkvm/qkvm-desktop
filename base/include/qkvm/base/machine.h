#pragma once

// Which kind of Windows a program of qKVM's is built for, and which kind of Windows the PC is: x64, or Windows on Arm
// (ARM64). On an Arm PC a person ends up with the ARM64 program by every road (the owner, 2026-11-19): an x64 copy that
// Windows runs there under its emulation takes the ARM64 release (core/src/update_release.cpp), and an MSIX is taken
// only of the PC's own kind (ui/installer/main.cpp). Windows' own headers only, so that a program with no Qt (qKVM's
// MSIX installer) shares it.

#include <bit>

#include <windows.h>

namespace qkvm::base {

enum class Architecture { X64, Arm64 };

// This program's own, as it was built.
constexpr Architecture programArchitecture() noexcept
{
#if defined(_M_ARM64)
    return Architecture::Arm64;
#elif defined(_M_X64)
    return Architecture::X64;
#else
#error qKVM is built for x64 and ARM64 Windows only.
#endif
}

// The PC's own kind, from the machine Windows says it is (IsWow64Process2's native machine; `said` false where Windows
// said nothing): ARM64 where Windows says ARM64, whatever this program is; and this program's own for anything else,
// and where nothing was said. So a program takes another kind than its own only as an x64 one on Windows on Arm, never
// an ARM64 one the x64 release, and never by a guess.
constexpr Architecture machineFrom(USHORT nativeMachine, bool said, Architecture program) noexcept
{
    return said && nativeMachine == IMAGE_FILE_MACHINE_ARM64 ? Architecture::Arm64 : program;
}

// The PC's own kind, as Windows says it of this process (IsWow64Process2, looked up at run time: where Windows has no
// such call, or the call fails, this program's own).
inline Architecture machineArchitecture() noexcept
{
    using Ask = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const FARPROC found = kernel == nullptr ? nullptr : GetProcAddress(kernel, "IsWow64Process2");
    USHORT process = 0;
    USHORT native = 0;
    const bool said = found != nullptr && std::bit_cast<Ask>(found)(GetCurrentProcess(), &process, &native) != FALSE;
    return machineFrom(native, said, programArchitecture());
}

// The name each kind goes by in a release's platform and file names: "x64" and "arm64".
constexpr const char* architectureName(Architecture architecture) noexcept
{
    return architecture == Architecture::Arm64 ? "arm64" : "x64";
}

} // namespace qkvm::base
