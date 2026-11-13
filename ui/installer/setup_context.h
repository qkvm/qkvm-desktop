#pragma once
#include <functional>
#include <string>
#include <windows.h>
namespace qkvm::installer {
DWORD processElevation(bool& elevated);
LONG processPackageStatus();
struct SetupContextApi {
    std::function<LONG()> packageStatus = processPackageStatus;
    std::function<DWORD(bool&)> elevation = processElevation;
};
std::wstring setupContextProblem(const SetupContextApi& api = {});
} // namespace qkvm::installer