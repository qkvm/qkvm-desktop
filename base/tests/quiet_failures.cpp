#include <crtdbg.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include <windows.h>

#include <werapi.h>
#pragma comment(lib, "wer.lib")

// Every test program fails quietly, and at once (qkvm_add_test in CMakeLists.txt links this into each, and the
// configuration refuses a test program that has not got it: DESIGN.md section 8). A check of the standard
// library's debug build (an index out of range), an assert of either kind, the debug heap finding a block written
// past its end, a bad argument to the C library, an abort, a pure virtual call or a crash: each ends the program by
// itself, with its words on stderr and an exit code that is not 0, and none puts a window on the screen that
// waits for a click. Without this, a removal of a bound put "Debug Assertion Failed! vector subscript out of
// range" on the owner's screen (2026-10-29), and a gate left to run by itself would have waited at that dialog
// until CTest's limit ended the program, two minutes on, with nothing said of why.
//
// Everything ends here, with exit code 3 (abort()'s), and nothing goes on past a failed check: a report of the
// debug library that only went to stderr would have let the program carry on (an _ASSERTE, a corrupted heap),
// and a test that wrote past what it had passed. Set before anything of the program's own runs (the C library's
// part of starting, not the program's): Qt Test sets some of these again its own way (the debug library's errors
// to a debugger only), and none of its ways shows a window or comes between a report and the hook that ends it.
//
// Compiled twice (CMakeLists.txt): for programs on the C library's DLL, as the tests are, and for those that carry
// the library in them (the installer's own test).

namespace {

// The exit code of a program that failed a check: abort()'s.
constexpr unsigned failedCheck = 3;

[[noreturn]] void endNow() noexcept
{
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), failedCheck);
    std::abort(); // (TerminateProcess does not return to its own process)
}

// A check of the C or C++ library failed; a release build says nothing of where.
void __cdecl failedLibraryCheck(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                                unsigned int line, std::uintptr_t) noexcept
{
    std::fwprintf(stderr, L"QKVM test: a check of the C or C++ library failed: %ls in %ls (%ls, line %u)\n",
                  expression ? expression : L"(no words in this build)", function ? function : L"?", file ? file : L"?",
                  line);
    endNow();
}

void __cdecl pureVirtualCalled() noexcept
{
    std::fputs("QKVM test: a pure virtual function was called\n", stderr);
    endNow();
}

#ifdef _DEBUG
// A report of the debug library: an assert (its own, an _ASSERTE, and the standard library's checks, which report
// as one) or an error (the debug heap's "HEAP CORRUPTION DETECTED" among them) is said on stderr and ends the
// program here; a warning goes its own way. Called before whatever the report's mode says, which Qt Test sets.
// Every report comes to a hook of wide letters, whether it was made in narrow ones (the standard library's
// checks, the heap's) or wide (an assert, an _ASSERTE): one hook of the other kind beside it was seen to change
// nothing.
int __cdecl debugReport(int type, wchar_t* message, int*) noexcept
{
    if (type == _CRT_WARN) {
        return FALSE;
    }
    std::fwprintf(stderr, L"QKVM test: the debug library reports: %ls\n", message ? message : L"(no words)");
    endNow();
}
#endif

struct Quiet {
    Quiet() noexcept
    {
        // No box for a crash, a missing disk or a file that cannot be opened.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        // Nor a window of Windows' error reporting for a program that ends at once.
        WerSetFlags(WER_FAULT_REPORTING_NO_UI);
        // abort(): no box of words, and no report to Windows' error reporting (which may show one).
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        // An assert of a release build writes to stderr, not into a box.
        _set_error_mode(_OUT_TO_STDERR);
#ifdef _DEBUG
        // The debug library's own reports end the program at the hook. Where one would go on to its mode, the
        // mode is stderr and a debugger, never a window.
        _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, debugReport);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
        // A bad argument to the C library, and a pure virtual call, end the program at once.
        _set_invalid_parameter_handler(failedLibraryCheck);
        _set_purecall_handler(pureVirtualCalled);
    }
};

} // namespace

// The mark of a test program. A shipped program defines it too (ui/main.cpp, ui/installer/main.cpp): linking this
// into one of them fails (LNK2005), and nothing of a test's failing quietly goes out to anybody.
extern "C" const int qkvmTestProgram = 1;

#pragma warning(push)
#pragma warning(disable : 4073) // "initializers put in library initialization area": where they are meant to be
#pragma init_seg(lib)
#pragma warning(pop)
namespace {
const Quiet quiet;
} // namespace
