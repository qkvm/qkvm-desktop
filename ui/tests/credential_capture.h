#pragma once

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>

#include <windows.h>

#include <wincred.h>

// Windows Credential Manager's calls, in memory (review 012, M3): what a build's own core/src/windows_secrets.cpp is
// compiled against by windows_secrets_in_memory.h, so that the very CREDENTIALW a version builds is seen (its type,
// persistence, flags, target name, user name, blob), and what it asks of the store (the filter it lists by, the type
// it reads by), and so that records as a published version wrote them can be put where this build looks. Nothing of
// it reaches Windows' own store. Its rules are Windows' as its documentation gives them: a target name is told apart
// by its letters with no regard to their case, and by its type; a filter is a name, or the beginning of one followed by
// an asterisk; and a flag Windows does not take is refused as Windows refuses it (ERROR_INVALID_FLAGS): a read's and an
// erase's flags are reserved and must be zero, a write takes CRED_PRESERVE_CREDENTIAL_BLOB alone, and a list takes
// CRED_ENUMERATE_ALL_CREDENTIALS alone, and that only without a filter (review 012 confirm, N3). Tests only.
namespace qkvm::testing::credentials {

// One record, as the store keeps it.
struct Record {
    std::wstring target;
    DWORD type = 0;
    DWORD persist = 0;
    DWORD flags = 0;
    std::wstring userName;
    std::wstring comment;
    std::wstring alias;
    DWORD attributes = 0;
    QByteArray blob;
};

// What was asked of the store, as it was asked.
struct Asked {
    std::vector<std::wstring> filters;
    std::vector<DWORD> enumerateFlags;
    std::vector<DWORD> readTypes;
    std::vector<DWORD> readFlags;
    std::vector<DWORD> writeFlags;
    std::vector<DWORD> deleteTypes;
    std::vector<DWORD> deleteFlags;
};

inline std::vector<Record>& kept()
{
    static std::vector<Record> all;
    return all;
}

inline Asked& asked()
{
    static Asked all;
    return all;
}

inline void clear()
{
    kept().clear();
    asked() = Asked{};
}

inline bool sameName(const std::wstring& one, const std::wstring& other)
{
    return CompareStringOrdinal(one.c_str(), static_cast<int>(one.size()), other.c_str(),
                                static_cast<int>(other.size()), TRUE) == CSTR_EQUAL;
}

inline std::wstring text(LPCWSTR said)
{
    return said == nullptr ? std::wstring() : std::wstring(said);
}

// What was handed out, by where it is, until CredFree gives it back: the blocks, and the lists with their records.
inline std::map<const void*, std::unique_ptr<unsigned char[]>>& blocks()
{
    static std::map<const void*, std::unique_ptr<unsigned char[]>> all;
    return all;
}

inline std::map<const void*, std::vector<const void*>>& lists()
{
    static std::map<const void*, std::vector<const void*>> all;
    return all;
}

inline unsigned char* block(std::size_t size)
{
    auto made = std::make_unique<unsigned char[]>(size);
    unsigned char* at = made.get();
    blocks().emplace(at, std::move(made));
    return at;
}

// A CREDENTIALW handed out, in one block that CredFree gives back.
inline PCREDENTIALW handedOut(const Record& record)
{
    const size_t strings =
        (record.target.size() + record.userName.size() + record.comment.size() + record.alias.size() + 4) *
        sizeof(wchar_t);
    unsigned char* at = block(sizeof(CREDENTIALW) + strings + static_cast<size_t>(record.blob.size()));
    auto* made = reinterpret_cast<PCREDENTIALW>(at);
    auto* next = reinterpret_cast<wchar_t*>(at + sizeof(CREDENTIALW));
    const auto place = [&next](const std::wstring& said) {
        wchar_t* here = next;
        std::memcpy(here, said.c_str(), (said.size() + 1) * sizeof(wchar_t));
        next += said.size() + 1;
        return here;
    };
    made->Type = record.type;
    made->Persist = record.persist;
    made->Flags = record.flags;
    made->TargetName = place(record.target);
    made->UserName = record.userName.empty() ? nullptr : place(record.userName);
    made->Comment = record.comment.empty() ? nullptr : place(record.comment);
    made->TargetAlias = record.alias.empty() ? nullptr : place(record.alias);
    made->CredentialBlobSize = static_cast<DWORD>(record.blob.size());
    made->CredentialBlob = reinterpret_cast<LPBYTE>(next);
    std::memcpy(made->CredentialBlob, record.blob.constData(), static_cast<size_t>(record.blob.size()));
    return made;
}

// A call refused, as Windows refuses one: FALSE, and why in GetLastError().
inline BOOL refused(DWORD error)
{
    SetLastError(error);
    return FALSE;
}

inline BOOL WINAPI write(PCREDENTIALW credential, DWORD flags)
{
    asked().writeFlags.push_back(flags);
    if ((flags & ~static_cast<DWORD>(CRED_PRESERVE_CREDENTIAL_BLOB)) != 0) {
        return refused(ERROR_INVALID_FLAGS);
    }
    const bool preserving = (flags & CRED_PRESERVE_CREDENTIAL_BLOB) != 0;
    // (Keeping the blob already there: the one handed in must then be empty.)
    if (credential == nullptr || credential->TargetName == nullptr || credential->CredentialBlobSize > 2560 ||
        (preserving && credential->CredentialBlobSize != 0)) {
        return refused(ERROR_INVALID_PARAMETER);
    }
    Record record;
    record.target = credential->TargetName;
    record.type = credential->Type;
    record.persist = credential->Persist;
    record.flags = credential->Flags;
    record.userName = text(credential->UserName);
    record.comment = text(credential->Comment);
    record.alias = text(credential->TargetAlias);
    record.attributes = credential->AttributeCount;
    record.blob = QByteArray(reinterpret_cast<const char*>(credential->CredentialBlob),
                             static_cast<qsizetype>(credential->CredentialBlobSize));
    for (Record& one : kept()) {
        if (one.type == record.type && sameName(one.target, record.target)) {
            if (preserving) {
                record.blob = one.blob;
            }
            one = record;
            return TRUE;
        }
    }
    // (Keeping the blob of none: refused, as CredWriteW's documentation has it, with ERROR_NOT_FOUND.)
    if (preserving) {
        return refused(ERROR_NOT_FOUND);
    }
    kept().push_back(record);
    return TRUE;
}

inline BOOL WINAPI read(LPCWSTR target, DWORD type, DWORD flags, PCREDENTIALW* out)
{
    asked().readTypes.push_back(type);
    asked().readFlags.push_back(flags);
    *out = nullptr;
    if (flags != 0) {
        return refused(ERROR_INVALID_FLAGS);
    }
    for (const Record& one : kept()) {
        if (one.type == type && sameName(one.target, text(target))) {
            *out = handedOut(one);
            return *out != nullptr ? TRUE : FALSE;
        }
    }
    return refused(ERROR_NOT_FOUND);
}

inline bool matches(const std::wstring& filter, const std::wstring& target)
{
    if (!filter.empty() && filter.back() == L'*') {
        const std::wstring start = filter.substr(0, filter.size() - 1);
        return target.size() >= start.size() && sameName(target.substr(0, start.size()), start);
    }
    return sameName(filter, target);
}

inline BOOL WINAPI enumerate(LPCWSTR filter, DWORD flags, DWORD* count, PCREDENTIALW** out)
{
    asked().filters.push_back(text(filter));
    asked().enumerateFlags.push_back(flags);
    *count = 0;
    *out = nullptr;
    const bool all = (flags & CRED_ENUMERATE_ALL_CREDENTIALS) != 0;
    if ((flags & ~static_cast<DWORD>(CRED_ENUMERATE_ALL_CREDENTIALS)) != 0 || (all && filter != nullptr)) {
        return refused(ERROR_INVALID_FLAGS);
    }
    std::vector<Record> found;
    for (const Record& one : kept()) {
        if (filter == nullptr || matches(filter, one.target)) {
            found.push_back(one);
            // Every one, by the name of its kind and its own: a generic record as cmdkey /list shows it. (Only
            // generic records are kept here: they are what every version writes.)
            if (all && one.type == CRED_TYPE_GENERIC) {
                found.back().target = L"LegacyGeneric:target=" + one.target;
            }
        }
    }
    if (found.empty()) {
        return refused(ERROR_NOT_FOUND);
    }
    // The list, and the records behind it: CredFree gives back the list, and so all of it, as Windows' does.
    auto* list = reinterpret_cast<PCREDENTIALW*>(block((found.size() + 1) * sizeof(PCREDENTIALW)));
    std::vector<const void*> records;
    records.reserve(found.size());
    for (size_t index = 0; index < found.size(); ++index) {
        list[index] = handedOut(found[index]);
        records.push_back(list[index]);
    }
    lists().emplace(list, std::move(records));
    *out = list;
    *count = static_cast<DWORD>(found.size());
    return TRUE;
}

inline BOOL WINAPI erase(LPCWSTR target, DWORD type, DWORD flags)
{
    asked().deleteTypes.push_back(type);
    asked().deleteFlags.push_back(flags);
    if (flags != 0) {
        return refused(ERROR_INVALID_FLAGS);
    }
    for (auto one = kept().begin(); one != kept().end(); ++one) {
        if (one->type == type && sameName(one->target, text(target))) {
            kept().erase(one);
            return TRUE;
        }
    }
    return refused(ERROR_NOT_FOUND);
}

inline VOID WINAPI release(PVOID handed)
{
    // A list: its records with it.
    if (const auto list = lists().find(handed); list != lists().end()) {
        for (const void* record : list->second) {
            blocks().erase(record);
        }
        lists().erase(list);
    }
    blocks().erase(handed);
}
} // namespace qkvm::testing::credentials
