#include <cassert>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include <windows.h>

#include <werapi.h>
#pragma comment(lib, "wer.lib")

// The child of test_quiet_failures (linked with what every test program is: quiet_failures.cpp). It fails in the
// way it is told to, and must end by itself, quietly. 77: this build has no such check.
namespace {

// What quiet_failures.cpp set, read back in a program that has nothing else that sets any of it (Qt Test sets
// some of it again in a test program). Each missing is said; 1 if any is.
int settings()
{
    int missing = 0;
    auto expect = [&missing](bool there, const char* what) {
        if (!there) {
            std::fprintf(stderr, "not set: %s\n", what);
            missing = 1;
        }
    };
    const UINT errors = GetErrorMode();
    expect(errors & SEM_FAILCRITICALERRORS, "no box for a missing disk");
    expect(errors & SEM_NOGPFAULTERRORBOX, "no box for a crash");
    expect(errors & SEM_NOOPENFILEERRORBOX, "no box for a file that cannot be opened");
    DWORD reporting = 0;
    expect(SUCCEEDED(WerGetFlags(GetCurrentProcess(), &reporting)) && (reporting & WER_FAULT_REPORTING_NO_UI),
           "no window of error reporting");
    const unsigned int aborting = _set_abort_behavior(0, 0);
    expect(!(aborting & _WRITE_ABORT_MSG), "abort says nothing in a box");
    expect(!(aborting & _CALL_REPORTFAULT), "abort reports nothing to Windows");
    expect(_set_error_mode(_REPORT_ERRMODE) == _OUT_TO_STDERR, "an assert writes to stderr");
    expect(_get_invalid_parameter_handler() != nullptr, "a failed check ends at the handler");
    expect(_get_purecall_handler() != nullptr, "a pure virtual call ends at the handler");
#ifdef _DEBUG
    for (const int type : {_CRT_ASSERT, _CRT_ERROR}) {
        expect(_CrtSetReportMode(type, _CRTDBG_REPORT_MODE) == (_CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG),
               "the debug library reports to stderr and a debugger only");
        expect(_CrtSetReportFile(type, _CRTDBG_REPORT_FILE) == GetStdHandle(STD_ERROR_HANDLE), "...to stderr itself");
    }
#endif
    return missing;
}

int fail(std::string_view mode)
{
    if (mode == "settings") {
        return settings();
    }
    if (mode == "subscript") {
#if _ITERATOR_DEBUG_LEVEL == 0
        return 77;
#else
        std::vector<int> some(1);
        volatile std::size_t beyond = 5; // (not "far": windows.h has taken it)
        return some[beyond];
#endif
    }
    if (mode == "assert") {
#ifdef NDEBUG
        return 77;
#else
        volatile bool never = false;
        assert(never);
        return 0;
#endif
    }
    if (mode == "asserte") {
#ifndef _DEBUG
        return 77;
#else
        volatile bool never = false;
        _ASSERTE(never);
        return 0; // (where a report goes on, it would come here: exit 0)
#endif
    }
    if (mode == "heap") {
#if !defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
        return 77;
#else
        // One byte past what was asked for, then given back: the debug heap finds it then (the address sanitizer
        // at the write).
        {
            const auto bytes = std::make_unique<char[]>(8);
            volatile std::size_t past = 8;
            bytes[past] = 1; // NOLINT: the overrun it is told to have
        }
        return 0;
#endif
    }
    if (mode == "parameter") {
        char small[2] = {};
        strcpy_s(small, sizeof small, "far too long for it");
        return small[0];
    }
    if (mode == "abort") {
        std::abort();
    }
    if (mode == "crash") {
        volatile int* nowhere = nullptr;
        *nowhere = 1; // NOLINT: the crash it is told to have
        return 0;
    }
    return mode == "normal" ? 0 : 2;
}

} // namespace

int main(int count, char** arguments)
{
    if (count != 2) {
        return 2;
    }
    try {
        return fail(arguments[1]);
    } catch (...) {
        return 2;
    }
}
