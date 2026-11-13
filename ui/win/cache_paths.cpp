#include "cache_paths.h"

#include <stdexcept>
#include <vector>

#include <windows.h>

#include <appmodel.h>
#include <shlobj.h>

#include "installer_config.h"

namespace qkvm::storage {
namespace fs = std::filesystem;

fs::path localData()
{
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_NO_PACKAGE_REDIRECTION, nullptr, &value)))
        throw std::runtime_error("Windows could not locate the local application cache.");
    const fs::path path(value);
    CoTaskMemFree(value);
    return path;
}

std::wstring currentFamily()
{
    UINT32 count = 0;
    const LONG result = GetCurrentPackageFamilyName(&count, nullptr);
    if (result == APPMODEL_ERROR_NO_PACKAGE)
        return {};
    if (result != ERROR_INSUFFICIENT_BUFFER || count > PACKAGE_FAMILY_NAME_MAX_LENGTH + 1)
        throw std::runtime_error("Windows could not identify this application package.");
    std::vector<wchar_t> value(count);
    if (GetCurrentPackageFamilyName(&count, value.data()) != ERROR_SUCCESS)
        throw std::runtime_error("Windows could not read this application package identity.");
    return value.data();
}

std::wstring familyFor(std::wstring name, std::wstring publisher)
{
    PACKAGE_ID id{};
    id.name = name.data();
    id.publisher = publisher.data();
    id.processorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    UINT32 count = 0;
    if (PackageFamilyNameFromId(&id, &count, nullptr) != ERROR_INSUFFICIENT_BUFFER ||
        count > PACKAGE_FAMILY_NAME_MAX_LENGTH + 1)
        throw std::runtime_error("The configured package identity is invalid.");
    std::vector<wchar_t> value(count);
    if (PackageFamilyNameFromId(&id, &count, value.data()) != ERROR_SUCCESS)
        throw std::runtime_error("Windows could not derive the configured package family.");
    return value.data();
}

fs::path cachePath(const fs::path& base, const std::wstring& family, Area area)
{
    if (!base.is_absolute() || (!family.empty() && VerifyPackageFamilyName(family.c_str()) != ERROR_SUCCESS))
        throw std::runtime_error("The application cache location is invalid.");
    // Package data survives an upgrade and belongs to Windows for uninstall cleanup.
    const fs::path root =
        family.empty() ? base / L"qKVM" / L"Cache" : base / L"Packages" / family / L"LocalCache" / L"qKVM";
    switch (area) {
    case Area::Downloads:
        return root / L"Downloads";
    case Area::Helpers:
        return root / L"Helpers";
    case Area::Installer:
        return root / L"Installer";
    case Area::Diagnostics:
        return root / L"Diagnostics";
    }
    throw std::runtime_error("The application cache area is invalid.");
}

void prepareDirectory(const fs::path& path)
{
    if (!path.is_absolute())
        throw std::runtime_error("The application cache must have an absolute path.");
    fs::path at = path.root_path();
    for (const auto& part : path.relative_path()) {
        if (part == L".." || part == L".")
            throw std::runtime_error("The application cache path is invalid.");
        at /= part;
        const DWORD prior = GetFileAttributesW(at.c_str());
        if (prior == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(at.c_str(), nullptr))
            throw std::runtime_error("The application cache could not be created.");
        const DWORD attributes = GetFileAttributesW(at.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            throw std::runtime_error("The application cache cannot use redirected folders.");
    }
}

fs::path cache(Area area)
{
    auto family = currentFamily();
    const auto expected = installer::packagePublisher[0] == L'\0'
                              ? familyFor(L"qKVM.Desktop.Development", L"CN=qKVM Development")
                              : familyFor(installer::packageName, installer::packagePublisher);
    // A development executable launched by another packaged program can inherit
    // that program's identity. Its cache must not belong to the host application.
    if (family != expected)
        family.clear();
    const auto path = cachePath(localData(), family, area);
    prepareDirectory(path);
    return path;
}
} // namespace qkvm::storage
