#include "own_temp.h"

#include <cstdlib>
#include <cwctype>
#include <vector>

#include <windows.h>

#include "cache_paths.h"

namespace qkvm::ui {
namespace {

std::wstring variable(const wchar_t* name)
{
    std::vector<wchar_t> value(32768);
    const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    return length > 0 && length < value.size() ? std::wstring(value.data(), length) : std::wstring();
}

std::wstring lowered(std::wstring text)
{
    for (wchar_t& sign : text) {
        sign = static_cast<wchar_t>(std::towlower(sign));
    }
    return text;
}

// A path as it is compared: whole, its parts as they are, no separator at its end, in small letters.
std::wstring comparable(const std::filesystem::path& path)
{
    std::wstring said = path.lexically_normal().wstring();
    while (said.size() > 3 && (said.back() == L'\\' || said.back() == L'/')) {
        said.pop_back();
    }
    return lowered(said);
}

// A folder as Windows says it of a handle to it: through no junction, in long names (as the TEMP Setup was given is
// said: Updates::runSetup). As it is named where it is not there or not said so.
std::filesystem::path finalOf(const std::filesystem::path& folder)
{
    const HANDLE held =
        CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (held == INVALID_HANDLE_VALUE) {
        return folder;
    }
    std::vector<wchar_t> name(32768);
    const DWORD length = GetFinalPathNameByHandleW(held, name.data(), static_cast<DWORD>(name.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(held);
    if (length == 0 || length >= name.size()) {
        return folder;
    }
    std::wstring said(name.data(), length);
    const std::wstring prefix = L"\\\\?\\";
    if (said.compare(0, prefix.size(), prefix) == 0 && said.size() > prefix.size() + 2 &&
        said[prefix.size() + 1] == L':') {
        said.erase(0, prefix.size());
    }
    return said;
}

bool isSetupsTemp(const std::wstring& value, const std::filesystem::path& downloads)
{
    if (value.empty()) {
        return false;
    }
    const std::filesystem::path at(comparable(std::filesystem::path(value)));
    const std::wstring leaf = at.filename().wstring();
    const std::wstring prefix = L"setup-temp-";
    if (leaf.size() != prefix.size() + 6 || leaf.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    for (std::size_t index = prefix.size(); index < leaf.size(); ++index) {
        if (std::iswalnum(leaf[index]) == 0 || leaf[index] > L'z') {
            return false;
        }
    }
    // Both as Windows says them of the folders (the cache may be named behind a junction, or in short names).
    return comparable(finalOf(at.parent_path())) == comparable(finalOf(downloads));
}

} // namespace

std::wstring ownTemp(const wchar_t* name)
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", name, RRF_RT_REG_SZ, nullptr, nullptr, &size) ==
            ERROR_SUCCESS &&
        size > sizeof(wchar_t)) {
        std::vector<wchar_t> value(size / sizeof(wchar_t) + 1);
        size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", name, RRF_RT_REG_SZ, nullptr, value.data(), &size) ==
            ERROR_SUCCESS) {
            return value.data();
        }
    }
    try {
        return (storage::localData() / L"Temp").wstring();
    } catch (...) {
        return {};
    }
}

bool takeBackOwnTemp(const std::filesystem::path& downloads)
{
    bool changed = false;
    for (const wchar_t* name : {L"TEMP", L"TMP"}) {
        if (!isSetupsTemp(variable(name), downloads)) {
            continue;
        }
        const std::wstring own = ownTemp(name);
        if (!own.empty() && SetEnvironmentVariableW(name, own.c_str()) != FALSE) {
            static_cast<void>(_wputenv_s(name, own.c_str()));
            changed = true;
        }
    }
    return changed;
}

bool takeBackOwnTemp()
{
    try {
        return takeBackOwnTemp(storage::cache(storage::Area::Downloads));
    } catch (...) {
        return false; // (No cache: nothing of Setup's was given out there.)
    }
}

} // namespace qkvm::ui
