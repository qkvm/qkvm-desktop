#include "setup_context.h"
#include "error_code.h"
#include <appmodel.h>
namespace qkvm::installer {
LONG processPackageStatus()
{
    UINT32 size = 0;
    return GetCurrentPackageFullName(&size, nullptr);
}
DWORD processElevation(bool& elevated)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return GetLastError();
    TOKEN_ELEVATION value{};
    DWORD size = 0;
    const BOOL read = GetTokenInformation(token, TokenElevation, &value, sizeof(value), &size);
    const DWORD error = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    elevated = value.TokenIsElevated != 0;
    return error;
}
std::wstring setupContextProblem(const SetupContextApi& api)
{
    if (api.packageStatus() != APPMODEL_ERROR_NO_PACKAGE)
        return L"Setup must run outside the installed package. Open the separate Setup folder and try again.";
    bool elevated = false;
    const DWORD error = api.elevation(elevated);
    if (error != ERROR_SUCCESS)
        return L"Setup could not check its Windows token (" + errorCode(L"Win32", error) +
               L"). Try again from a normal Windows session.";
    if (elevated)
        return L"Setup cannot run as administrator. Close it and open it normally from your Windows account.";
    return {};
}
} // namespace qkvm::installer