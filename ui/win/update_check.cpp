// qKVM Setup's update check (packaging/qkvm-desktop.iss, its staged install, i020): a program of Windows' own calls
// alone, with the C runtime linked into it, so that it runs where nothing else of qKVM is (no library of the package's,
// nothing of Qt's). The setup carries it and never installs it under a name of its own: while the earlier version is
// set aside for a copy, the setup puts this program at the program's own name ({app}\qkvm-desktop.exe), so that
// whatever starts qKVM then (the Start menu, the start with Windows, a desktop entry) starts this one; the copy puts
// the program in its place last, and a setup that puts the earlier version back puts this one there first and takes it
// away last. A cut at any moment in between (the power gone, the setup ended) leaves it there: a start of qKVM then
// says that an update did not finish, in a box of Windows' own, instead of finding no program at all.
//
// While a setup or an uninstall of qKVM is at work (its mutex, as the program honours it, ui/win/setup_wait.h), it ends
// at once and says nothing: a box that waited for a click would keep this file in use, and the setup could not put the
// program in its place, nor take this one away.

#include "unfinished_update.h"

// Windows' own header.
#include <windows.h>

namespace {

// The mutex qKVM's Setup holds while it runs (its SetupMutex), and its uninstall too, in this session's names: as
// ui/win/setup_wait.h names it (setupMutexName) for the program, which holds Qt's strings and is not for this one;
// tools/test_setup.ps1 holds the three alike.
constexpr wchar_t setupMutex[] = L"Local\\qKVM-Desktop-Setup";

bool setupAtWork()
{
    HANDLE const mutex = OpenMutexW(SYNCHRONIZE, FALSE, setupMutex);
    if (mutex == nullptr) {
        // (Not there: no setup at work. Or there and not this user's to open, which no setup of this user's makes.)
        return false;
    }
    CloseHandle(mutex);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*arguments*/, int /*show*/)
{
    if (setupAtWork()) {
        return 0;
    }
    MessageBoxW(nullptr, qkvm::ui::unfinishedUpdateText, L"qKVM", MB_OK | MB_ICONERROR);
    return 1;
}
