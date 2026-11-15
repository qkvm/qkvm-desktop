#pragma once

#include <QDir>
#include <QFile>
#include <QString>

#include <windows.h>

#include <shlobj.h>

// Whether this is a machine made for the VM steps (tools/test_upgrade_keeps_vm.ps1, as test_setup_vm.ps1 has it): a
// virtual machine, as its firmware says, and the profile folder of whoever runs this holds qkvm-disposable-vm.txt
// with this machine's name. Both as Windows says
// them (GetComputerNameExW, SHGetKnownFolderPath), never as the environment does: COMPUTERNAME, _CLUSTER_NETWORK_NAME_
// and USERPROFILE are the caller's to set (review 012, L5; review 012 last, R2). Tests only; only programs that would
// write Windows' own Credential Manager ask it, and they ask it before they look at anything.
namespace qkvm::testing {

inline QString disposableMarkerIn(const QString& profile)
{
    return QDir(profile).filePath(QStringLiteral("qkvm-disposable-vm.txt"));
}

// The machine's name as Windows has it (the NetBIOS name, which test_setup_vm.ps1's marker holds); empty where it
// does not say. The physical one: ComputerNameNetBIOS gives _CLUSTER_NETWORK_NAME_ instead where that is set, and the
// caller sets that too (review 012 last, R2).
inline QString machineNameOfWindows()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameExW(ComputerNamePhysicalNetBIOS, name, &size) == FALSE) {
        return {};
    }
    return QString::fromWCharArray(name, static_cast<qsizetype>(size));
}

// The profile folder of whoever runs this, as Windows has it; empty where it does not say.
inline QString profileOfWindows()
{
    PWSTR path = nullptr;
    QString found;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &path)) && path != nullptr) {
        found = QString::fromWCharArray(path);
    }
    CoTaskMemFree(path);
    return found;
}

// Whether the marker in `profile` names the machine `name`.
inline bool markerNames(const QString& profile, const QString& name)
{
    if (name.isEmpty() || profile.isEmpty()) {
        return false;
    }
    QFile marker(disposableMarkerIn(profile));
    return marker.open(QIODevice::ReadOnly) &&
           QString::fromUtf8(marker.read(256)).trimmed().compare(name, Qt::CaseInsensitive) == 0;
}

// What the machine's firmware says it is, as Windows keeps it where a standard user may read it
// (HKLM\HARDWARE\DESCRIPTION\System\BIOS): its maker and its product.
struct Firmware {
    QString maker;
    QString product;
};

inline Firmware firmwareOfWindows()
{
    const auto valueOf = [](const wchar_t* name) {
        wchar_t said[256] = {};
        DWORD size = sizeof(said);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", name, RRF_RT_REG_SZ, nullptr, said,
                         &size) != ERROR_SUCCESS) {
            return QString();
        }
        return QString::fromWCharArray(said).trimmed();
    };
    return {valueOf(L"SystemManufacturer"), valueOf(L"SystemProductName")};
}

// The hypervisor the firmware names, if it names one: QEMU (as Proxmox's guests are), VMware, Hyper-V ("Microsoft
// Corporation" and "Virtual Machine"), VirtualBox ("innotek GmbH"); empty where it names none, as a PC's does.
inline QString hypervisorNamedBy(const Firmware& firmware)
{
    const auto is = [](const QString& said, QStringView word) { return said.compare(word, Qt::CaseInsensitive) == 0; };
    if (firmware.maker.contains(u"QEMU", Qt::CaseInsensitive) ||
        firmware.product.contains(u"QEMU", Qt::CaseInsensitive)) {
        return QStringLiteral("QEMU");
    }
    if (firmware.maker.startsWith(u"VMware", Qt::CaseInsensitive) ||
        firmware.product.startsWith(u"VMware", Qt::CaseInsensitive)) {
        return QStringLiteral("VMware");
    }
    if (is(firmware.maker, u"Microsoft Corporation") && is(firmware.product, u"Virtual Machine")) {
        return QStringLiteral("Hyper-V");
    }
    if (is(firmware.maker, u"innotek GmbH") || is(firmware.product, u"VirtualBox")) {
        return QStringLiteral("VirtualBox");
    }
    return {};
}

// Why this is no machine made for the VM steps, in words; empty where it is one: its firmware names a hypervisor, and
// the profile holds the marker that names the machine (outside review of 0.1.2, T5: a marker alone proves no VM).
inline QString whyNotDisposable(const QString& profile, const QString& machine, const Firmware& firmware)
{
    if (hypervisorNamedBy(firmware).isEmpty()) {
        return QStringLiteral("its firmware names no hypervisor (QEMU, VMware, Hyper-V, VirtualBox): it is no virtual "
                              "machine");
    }
    if (!markerNames(profile, machine)) {
        return QStringLiteral("no qkvm-disposable-vm.txt with its name in the profile Windows names");
    }
    return {};
}

inline bool isDisposableMachine()
{
    return whyNotDisposable(profileOfWindows(), machineNameOfWindows(), firmwareOfWindows()).isEmpty();
}

} // namespace qkvm::testing
