#include "unfinished_update.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <sddl.h>

namespace qkvm::ui {
namespace {

// This process's Windows user, as Windows names it (a SID: "S-1-5-21-..."); empty where it did not say.
std::wstring thisUsersSid()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    std::wstring sid;
    LPWSTR named = nullptr;
    if (size >= sizeof(TOKEN_USER) && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &named) != FALSE) {
        sid = named;
        LocalFree(named);
    }
    CloseHandle(token);
    return sid;
}

// The most of a record that is read: one of the setup's own is some 10 KiB.
constexpr LONGLONG mostRecord = LONGLONG{1024} * 1024;

// A name of ASCII signs alone (the record's names, by tools/build_setup.ps1's rule) as Windows takes it.
std::wstring widened(std::string_view ascii)
{
    return {ascii.begin(), ascii.end()};
}

// Whether anything is at a path, as Windows says: a file, a folder, a redirection, or one it cannot look at for another
// reason than its not being there.
bool somethingAt(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    const DWORD error = GetLastError();
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
}

// One part of a name of the package's: letters, digits, blanks and . _ + -, beginning with a letter, a digit or _,
// ending in neither a dot nor a blank (tools/build_setup.ps1 refuses a package with any other).
bool plainPart(std::string_view part)
{
    if (part.empty() || part.back() == '.' || part.back() == ' ') {
        return false;
    }
    for (std::size_t at = 0; at < part.size(); ++at) {
        const char sign = part[at];
        const bool letterOrDigit =
            (sign >= 'A' && sign <= 'Z') || (sign >= 'a' && sign <= 'z') || (sign >= '0' && sign <= '9');
        if (!letterOrDigit && sign != '_' && (at == 0 || (sign != '.' && sign != '+' && sign != ' ' && sign != '-'))) {
            return false;
        }
    }
    return true;
}

// A name as the record says it ("plugins\platforms\qwindows.dll"): one a package of ours can have.
bool packageName(std::string_view name)
{
    for (;;) {
        const std::size_t at = name.find('\\');
        if (!plainPart(name.substr(0, at))) {
            return false;
        }
        if (at == std::string_view::npos) {
            return true;
        }
        name.remove_prefix(at + 1);
    }
}

// One of the endings a set-aside gives (packaging/qkvm-desktop.iss, SetAsideEndings): .old-update, or .2.old-update to
// .9.old-update.
bool setAsideEndingOfOurs(std::string_view ending)
{
    const std::string_view first = setAsideEnding;
    if (ending == first) {
        return true;
    }
    return ending.size() == first.size() + 2 && ending[0] == '.' && ending[1] >= '2' && ending[1] <= '9' &&
           ending.substr(2) == first;
}

// The bytes of a file of at most mostRecord; nothing where it is no file that can be read whole.
std::optional<std::string> smallFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::optional<std::string> bytes;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 && size.QuadPart <= mostRecord) {
        std::string read(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        if (read.empty() || (ReadFile(file, read.data(), static_cast<DWORD>(read.size()), &got, nullptr) != FALSE &&
                             got == read.size())) {
            bytes = std::move(read);
        }
    }
    CloseHandle(file);
    return bytes;
}

// What a record says the set-aside renamed: its ending and the names, as it says them; and what the copy writes.
struct Record {
    std::string ending;
    std::vector<std::string> asides;
    std::vector<std::string> copies;
};

// A record read, as the setup writes it: nothing where it is not whole, not of this kind, says its ending other than
// once and as one of ours, or names what no package of ours can have.
std::optional<Record> readRecord(const std::string& bytes)
{
    std::vector<std::string_view> lines;
    std::string_view rest = bytes;
    while (!rest.empty()) {
        const std::size_t at = rest.find('\n');
        std::string_view line = rest.substr(0, at);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        rest = at == std::string_view::npos ? std::string_view() : rest.substr(at + 1);
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.size() < 4 || lines[1] != setAsideRecordKind || lines.back() != setAsideRecordEnd) {
        return std::nullopt;
    }
    Record record;
    constexpr std::string_view endingLine = "ending ";
    constexpr std::string_view asideLine = "aside ";
    constexpr std::string_view copyLine = "copy ";
    for (std::size_t at = 2; at + 1 < lines.size(); ++at) {
        const std::string_view line = lines[at];
        if (line.starts_with(endingLine)) {
            const std::string_view ending = line.substr(endingLine.size());
            if (!record.ending.empty() || !setAsideEndingOfOurs(ending)) {
                return std::nullopt;
            }
            record.ending = ending;
        } else if (line.starts_with(asideLine)) {
            const std::string_view name = line.substr(asideLine.size());
            if (!packageName(name)) {
                return std::nullopt;
            }
            record.asides.emplace_back(name);
        } else if (line.starts_with(copyLine)) {
            const std::string_view name = line.substr(copyLine.size());
            if (!packageName(name)) {
                return std::nullopt;
            }
            record.copies.emplace_back(name);
        }
        // (A line of another kind, what an earlier update left for the setup to remove among them, says nothing of
        // whether this copy is whole.)
    }
    if (record.ending.empty()) {
        return std::nullopt;
    }
    return record;
}

// The marker of `folder` judged, as the setup wrote it: Nothing where there is none, Unfinished where its record names
// a file set aside that is there (in `setAside`) or does not read, StaleMarker otherwise. Nothing is changed.
UpdateLook judged(const std::wstring& folder, const std::wstring& marker)
{
    UpdateLook look;
    if (!somethingAt(marker)) {
        return look;
    }
    const std::optional<std::string> bytes = smallFile(marker);
    const std::optional<Record> record = bytes ? readRecord(*bytes) : std::nullopt;
    if (!record) {
        look.found = UpdateLeft::Unfinished;
        return look;
    }
    for (const std::string& name : record->asides) {
        if (somethingAt(folder + L'\\' + widened(name) + widened(record->ending))) {
            look.found = UpdateLeft::Unfinished;
            look.setAside = widened(name);
            return look;
        }
    }
    // (And a file only the copy wrote, which the earlier version had not, still there: a put-back that could not remove
    // it keeps the marker for the next setup, and the earlier version beside it is no whole one; outside review of
    // 0.2.0, 1.5. Names compared as Windows does, whatever their case.)
    for (const std::string& name : record->copies) {
        const bool setAside = std::ranges::any_of(record->asides, [&name](const std::string& aside) {
            return CompareStringOrdinal(widened(aside).c_str(), -1, widened(name).c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (!setAside && somethingAt(folder + L'\\' + widened(name))) {
            look.found = UpdateLeft::Unfinished;
            look.setAside = widened(name);
            return look;
        }
    }
    look.found = UpdateLeft::StaleMarker;
    return look;
}

// The gate `name` taken by this thread, as a setup takes it: made owned, with a list that lets this account alone at
// it; or there already and free, or left by one that ended, and taken. Nothing where another holds it, or where Windows
// does not let this account at it (one another account made under its name: it is not known to be free). The caller
// lets go of it (ReleaseMutex, then CloseHandle).
HANDLE takeTheGate(const std::wstring& name)
{
    const std::wstring user = thisUsersSid();
    if (name.empty() || user.empty()) {
        return nullptr;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring rules = L"D:P(A;;GA;;;" + user + L")";
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(rules.c_str(), SDDL_REVISION_1, &descriptor, nullptr) ==
        FALSE) {
        return nullptr;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE gate =
        CreateMutexExW(&attributes, name.c_str(), CREATE_MUTEX_INITIAL_OWNER, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    const DWORD said = GetLastError();
    LocalFree(descriptor);
    if (gate == nullptr) {
        return nullptr;
    }
    if (said != ERROR_ALREADY_EXISTS) {
        return gate;
    }
    const DWORD waited = WaitForSingleObject(gate, 0);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
        return gate;
    }
    CloseHandle(gate);
    return nullptr;
}

} // namespace

std::wstring setupAccountGate()
{
    const std::wstring sid = thisUsersSid();
    return sid.empty() ? std::wstring() : std::wstring(setupAccountGatePrefix) + sid;
}

UpdateLook lookForUnfinishedUpdate(const std::wstring& folder, const std::wstring& gate)
{
    // (Only a copy the setup installed: the contract's marker beside it. A copy from the ZIP, or one copied out of an
    // installed folder, is no setup's to put right, and is never told to run one; review i020-1, L7.)
    if (!somethingAt(folder + L"\\qkvm-desktop.installed")) {
        return {};
    }
    const std::wstring marker = folder + L'\\' + widened(unfinishedUpdateMarker);
    UpdateLook look = judged(folder, marker);
    if (look.found != UpdateLeft::StaleMarker) {
        return look;
    }
    // A marker whose record names nothing still set aside: what an update left once the earlier version was whole
    // again, or what a setup has just written and not yet acted on. Taken away only while this start holds the
    // account's gate, which every setup and uninstall holds as it works (outside review of 0.2.0, 1.4), and judged
    // again then, a setup that wrote another meanwhile being over; the program starts whether or not it was taken away.
    const HANDLE held = takeTheGate(gate);
    if (held == nullptr) {
        look.setupAtWork = true;
        return look;
    }
    look = judged(folder, marker);
    if (look.found == UpdateLeft::StaleMarker) {
        look.markerRemoved = DeleteFileW(marker.c_str()) != FALSE;
    }
    ReleaseMutex(held);
    CloseHandle(held);
    return look;
}

const wchar_t* unfinishedUpdateWords()
{
    return unfinishedUpdateText;
}

std::wstring ownFolder()
{
    std::vector<wchar_t> name(MAX_PATH);
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
        if (size == 0) {
            return {};
        }
        if (size < name.size()) {
            const std::wstring file(name.data(), size);
            const std::size_t at = file.rfind(L'\\');
            return at != std::wstring::npos && at > 0 ? file.substr(0, at) : std::wstring();
        }
        // (Cut short: a longer name.)
        if (name.size() >= 32768) {
            return {};
        }
        name.resize(name.size() * 2);
    }
}

} // namespace qkvm::ui
