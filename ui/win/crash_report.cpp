#include "crash_report.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <exception>
#include <string>
#include <vector>

#include <windows.h>

#include <objbase.h>

#include "cache_paths.h"

namespace qkvm::ui {
namespace fs = std::filesystem;
namespace {
HANDLE report = INVALID_HANDLE_VALUE;
volatile LONG recorded = 0;
std::array<char, 2048> prefix{};
std::terminate_handler priorTerminate = nullptr;
using SignalHandler = void (*)(int);
SignalHandler priorAbort = SIG_DFL;
std::atomic<CrashReport::AtACrash> also{nullptr};
volatile LONG alsoDone = 0;

// Only a fixed alphabet and integer fields enter the crash path. No allocation,
// Qt calls, locks, symbol loading, settings or network operations are attempted
// (what main adds, alsoAtACrash, is held to the same).
void writeSummary(const char* outcome, DWORD code, const void* address)
{
    if (report == INVALID_HANDLE_VALUE || InterlockedCompareExchange(&recorded, 1, 0) != 0)
        return;
    std::array<char, 4096> text{};
    size_t used = 0;
    const auto add = [&](const char* value) {
        while (*value != '\0' && used + 1 < text.size())
            text[used++] = *value++;
    };
    const auto number = [&](unsigned long long value) {
        constexpr char digits[] = "0123456789abcdef";
        add("0x");
        for (int shift = 60; shift >= 0; shift -= 4)
            text[used++] = digits[(value >> shift) & 15U];
        add("\n");
    };
    add(prefix.data());
    add("outcome=");
    add(outcome);
    add("\nexception=");
    number(code);
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    add("utc_filetime=");
    number((static_cast<unsigned long long>(now.dwHighDateTime) << 32U) | now.dwLowDateTime);
    MEMORY_BASIC_INFORMATION region{};
    const bool image = address != nullptr && VirtualQuery(address, &region, sizeof(region)) == sizeof(region) &&
                       region.Type == MEM_IMAGE;
    add("module=");
    // A path or an arbitrary loaded-module name can contain user information.
    // Identify only the application's image; other faults remain explicitly unknown.
    const bool application = image && region.AllocationBase == GetModuleHandleW(nullptr);
    add(application ? "application\nmodule_offset=" : "other_or_unknown\nmodule_offset=");
    number(application ? reinterpret_cast<ULONG_PTR>(address) - reinterpret_cast<ULONG_PTR>(region.AllocationBase) : 0);
    LARGE_INTEGER beginning{};
    DWORD written = 0;
    if (SetFilePointerEx(report, beginning, nullptr, FILE_BEGIN) &&
        WriteFile(report, text.data(), static_cast<DWORD>(used), &written, nullptr) && written == used) {
        SetEndOfFile(report);
        FlushFileBuffers(report);
    }
}

// After the summary, which is the surest, and once whichever way the crash comes (a terminate goes on to an
// abort): what main added (from the second reading of c7ec58c, L3: the video's diagnostics lines that waited
// to be written, the nearest to the crash, were lost with it).
void alsoNow()
{
    const CrashReport::AtACrash what = also.load();
    if (what != nullptr && InterlockedCompareExchange(&alsoDone, 1, 0) == 0)
        what();
}

LONG WINAPI unhandled(EXCEPTION_POINTERS* exception)
{
    if (exception != nullptr && exception->ExceptionRecord != nullptr)
        writeSummary("exception", exception->ExceptionRecord->ExceptionCode,
                     exception->ExceptionRecord->ExceptionAddress);
    alsoNow();
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminated()
{
    writeSummary("terminate", 0, nullptr);
    alsoNow();
    if (priorTerminate != nullptr)
        priorTerminate();
    std::abort();
}

void aborted(int)
{
    writeSummary("abort", 0, nullptr);
    alsoNow();
}

bool reportName(const std::wstring& name)
{
    return name.size() == 48 && name.front() == L'{' && name.substr(37) == L"}.crash.txt" &&
           name.substr(1, 36).find_first_not_of(L"0123456789abcdefABCDEF-") == std::wstring::npos;
}

void prune(const fs::path& directory)
{
    std::vector<fs::directory_entry> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (reportName(entry.path().filename().wstring()) && attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0)
            files.push_back(entry);
    }
    std::ranges::sort(files, [](const auto& a, const auto& b) { return a.last_write_time() > b.last_write_time(); });
    for (size_t at = 0; at < files.size(); ++at) {
        if (at >= 9 || fs::file_time_type::clock::now() - files[at].last_write_time() > std::chrono::hours(24 * 30))
            DeleteFileW(files[at].path().c_str());
    }
}
} // namespace

struct CrashReport::State {
    fs::path file;
    LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
    bool active = false;
};

CrashReport::CrashReport(const fs::path& directory) : m_state(std::make_unique<State>())
{
    try {
        if (report != INVALID_HANDLE_VALUE)
            return;
        storage::prepareDirectory(directory);
        prune(directory);
        GUID id{};
        wchar_t name[40]{};
        if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, name, 40) == 0)
            return;
        m_state->file = directory / (std::wstring(name) + L".crash.txt");
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew);
        const std::string header = "qKVM local diagnostic\nformat=1\nversion=" QKVM_DESKTOP_VERSION
                                   "\nbuild_pe_timestamp=" +
                                   std::to_string(nt->FileHeader.TimeDateStamp) +
                                   "\nNo memory, keys, text, credentials or device addresses are collected.\n";
        std::copy(header.begin(), header.end(), prefix.begin());
        prefix[header.size()] = '\0';
        report = CreateFileW(m_state->file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (report == INVALID_HANDLE_VALUE)
            return;
        recorded = 0;
        writeSummary("unfinished_session", 0, nullptr);
        recorded = 0;
        m_state->previous = SetUnhandledExceptionFilter(unhandled);
        priorTerminate = std::set_terminate(terminated);
        priorAbort = std::signal(SIGABRT, aborted);
        m_state->active = true;
    } catch (...) {
        // Diagnostics must not prevent the app from starting when storage is unavailable.
        if (report != INVALID_HANDLE_VALUE && !m_state->active) {
            CloseHandle(report);
            report = INVALID_HANDLE_VALUE;
        }
    }
}

void CrashReport::alsoAtACrash(AtACrash what)
{
    also = what;
}

CrashReport::~CrashReport()
{
    if (!m_state->active)
        return;
    SetUnhandledExceptionFilter(m_state->previous);
    std::set_terminate(priorTerminate);
    std::signal(SIGABRT, priorAbort);
    const bool normal = recorded == 0;
    CloseHandle(report);
    report = INVALID_HANDLE_VALUE;
    if (normal)
        DeleteFileW(m_state->file.c_str());
}
} // namespace qkvm::ui
