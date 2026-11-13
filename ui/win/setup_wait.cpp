#include "setup_wait.h"

#include <functional>
#include <string>
#include <thread>

#include <windows.h>

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

} // namespace

WhileSetupWorks whileSetupWorks(const QString& name, bool afterUpdate, std::chrono::milliseconds most,
                                const std::function<bool()>& startAnew)
{
    const std::wstring named = name.toStdWString();
    if (!afterUpdate) {
        return isThere(named) ? WhileSetupWorks::BeingUpdated : WhileSetupWorks::GoOn;
    }
    const auto until = std::chrono::steady_clock::now() + most;
    while (isThere(named)) {
        if (std::chrono::steady_clock::now() >= until) {
            return WhileSetupWorks::StillUpdating;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return startAnew && startAnew() ? WhileSetupWorks::StartedAnew : WhileSetupWorks::GoOn;
}

} // namespace qkvm::ui
