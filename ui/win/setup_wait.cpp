#include "setup_wait.h"

#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <tlhelp32.h>

namespace qkvm::ui {
namespace {

// Whether a mutex of this name is there. (One there that may not be opened, made by Setup run otherwise, is there all
// the same.)
bool isThere(const std::wstring& name)
{
    const HANDLE there = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (there == nullptr) {
        return GetLastError() != ERROR_FILE_NOT_FOUND;
    }
    CloseHandle(there);
    return true;
}

// A token's integrity, as the number of its label (0 where it cannot be read).
unsigned long integrityOf(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<unsigned char> label(size);
    if (size < sizeof(TOKEN_MANDATORY_LABEL) ||
        GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size) == FALSE) {
        return 0;
    }
    const PSID level = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    const UCHAR parts = IsValidSid(level) != FALSE ? *GetSidSubAuthorityCount(level) : 0;
    return parts == 0 ? 0 : *GetSidSubAuthority(level, parts - 1U);
}

// Whether this process runs below medium integrity (or its integrity cannot be read).
bool runsBelowThePerson()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return true;
    }
    const unsigned long integrity = integrityOf(token);
    CloseHandle(token);
    return integrity < SECURITY_MANDATORY_MEDIUM_RID;
}

// Whether a qKVM Setup is at work: its mutex there, and a process that runs now that says it is that Setup, by its own
// event named after the mutex and the process's number (setupRunsName), which Setup makes as it starts and lets go of
// with its end. A handle to the mutex kept elsewhere after Setup has ended keeps the mutex there, and every start would
// end at once for it, the restart after an update among them; no process of that number runs any more, or one that
// runs says nothing of the kind (review R2). Where the processes cannot be told, it is taken to be at work: a start
// below medium integrity (Windows leaves out of its list every process above it, Setup's among them; as
// test_updates_view starts the program), or one whose list cannot be had.
bool setupIsAtWork(const std::wstring& name)
{
    if (!isThere(name)) {
        return false;
    }
    if (runsBelowThePerson()) {
        return true;
    }
    const HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (all == INVALID_HANDLE_VALUE) {
        return true;
    }
    bool atWork = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(all, &entry); more != FALSE && !atWork; more = Process32NextW(all, &entry)) {
        const std::wstring says = setupRunsName(name, entry.th32ProcessID);
        const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, says.c_str());
        if (event != nullptr) {
            CloseHandle(event);
            atWork = true;
        } else if (GetLastError() == ERROR_ACCESS_DENIED) {
            atWork = true;
        }
    }
    CloseHandle(all);
    return atWork;
}

} // namespace

std::wstring setupRunsName(const std::wstring& mutex, unsigned long process)
{
    return mutex + L"-" + std::to_wstring(process);
}

bool aboveThePerson(bool elevated, unsigned long integrity)
{
    return elevated || integrity > SECURITY_MANDATORY_MEDIUM_RID;
}

bool tokenRunsAboveThePerson(void* token)
{
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) == FALSE) {
        return true;
    }
    const unsigned long integrity = integrityOf(token);
    if (integrity == 0) {
        return true;
    }
    return aboveThePerson(elevation.TokenIsElevated != 0, integrity);
}

bool runsAboveThePerson()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return true;
    }
    const bool above = tokenRunsAboveThePerson(token);
    CloseHandle(token);
    return above;
}

WhileSetupWorks whileSetupWorks(const QString& name, bool afterUpdate, std::chrono::milliseconds most,
                                const std::function<bool()>& startAnew, const std::function<bool()>& aboveThePersonNow,
                                void* setup)
{
    const std::wstring named = name.toStdWString();
    if (!afterUpdate) {
        return setupIsAtWork(named) ? WhileSetupWorks::BeingUpdated : WhileSetupWorks::GoOn;
    }
    // Setup's own start never runs the program itself: with Setup's token it could run elevated, if Setup did, or with
    // whatever else Setup's start gave it (review C1).
    if (!aboveThePersonNow || aboveThePersonNow()) {
        return WhileSetupWorks::NotStartedAgain;
    }
    if (setup != nullptr) {
        // Setup's own process: over when it has ended, whoever keeps its mutex.
        if (WaitForSingleObject(setup, static_cast<DWORD>(most.count())) != WAIT_OBJECT_0) {
            return WhileSetupWorks::StillUpdating;
        }
    } else {
        const auto until = std::chrono::steady_clock::now() + most;
        while (setupIsAtWork(named)) {
            if (std::chrono::steady_clock::now() >= until) {
                return WhileSetupWorks::StillUpdating;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    return startAnew && startAnew() ? WhileSetupWorks::StartedAnew : WhileSetupWorks::NotStartedAgain;
}

void* openTheStarter()
{
    const DWORD self = GetCurrentProcessId();
    DWORD starter = 0;
    const HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (all == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(all, &entry); more != FALSE; more = Process32NextW(all, &entry)) {
        if (entry.th32ProcessID == self) {
            starter = entry.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(all);
    if (starter == 0 || starter == self) {
        return nullptr;
    }
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, starter);
    if (process == nullptr) {
        return nullptr;
    }
    // (Its number is its own only where it began before this one: a process that began later took the number of a
    // starter that had ended.)
    FILETIME theirs{};
    FILETIME ours{};
    FILETIME ended{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process, &theirs, &ended, &kernel, &user) == FALSE ||
        GetProcessTimes(GetCurrentProcess(), &ours, &ended, &kernel, &user) == FALSE ||
        CompareFileTime(&theirs, &ours) > 0) {
        CloseHandle(process);
        return nullptr;
    }
    return process;
}

} // namespace qkvm::ui
