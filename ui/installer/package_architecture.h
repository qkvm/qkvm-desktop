#pragma once

// The one MSIX package qKVM's installer takes: the package for the PC's own kind of Windows (base::machineArchitecture:
// x64, or ARM64 on Windows on Arm), never one that Windows would run under its emulation and never a neutral one. (No
// MSIX is published; the owner, 2026-11-19: a person on Windows on Arm ends up with the ARM64 program by every road.)

#include <appxpackaging.h>

#include <qkvm/base/machine.h>

namespace qkvm::installer {

constexpr APPX_PACKAGE_ARCHITECTURE packageArchitectureOf(base::Architecture architecture) noexcept
{
    return architecture == base::Architecture::Arm64 ? APPX_PACKAGE_ARCHITECTURE_ARM64 : APPX_PACKAGE_ARCHITECTURE_X64;
}

constexpr bool packageFitsThePc(APPX_PACKAGE_ARCHITECTURE package, base::Architecture pc) noexcept
{
    return package == packageArchitectureOf(pc);
}

} // namespace qkvm::installer
