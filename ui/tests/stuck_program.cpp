#include <chrono>
#include <cwchar>

#include <windows.h>

#include <shellapi.h>

#include "../win/hand_over_end.h"

// The child of test_ui_updates_view's case of a program that has handed over to Setup and whose quit is held up before
// its goodbyes are even over: it holds a mutex of the name it is given (as the program holds its single-instance one),
// begins its hand-over's end with the milliseconds it is given as the whole bound (ui::HandOverEnd, as the program does
// once Setup has started), and then waits for ever, as a quit that does not end would. Only that end ends it, and lets
// go of the mutex. A Windows program: it shows nothing.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr || count != 3) {
        return 90;
    }
    const HANDLE held = CreateMutexW(nullptr, TRUE, arguments[1]);
    if (held == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 91;
    }
    const std::chrono::milliseconds whole(std::wcstol(arguments[2], nullptr, 10));
    LocalFree(static_cast<void*>(arguments));
    qkvm::ui::HandOverEnd end({whole, whole});
    end.begin();
    Sleep(INFINITE);
    return 92;
}
