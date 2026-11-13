#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include <windows.h>

#include <bcrypt.h>

// A stand-in for qKVM's Setup, built by the tests and started by the program's updater in them in Setup's place: no
// setup is ever run by a test. It writes down how it was started and from which file, and ends: in the file its
// command line names after /LOG= (where the real Setup writes its log), three lines, each ending in a line feed:
//   the whole command line, as Windows handed it over (UTF-8);
//   the SHA-256 of the file it was started from, in lowercase hexadecimal;
//   that file's name, whole (UTF-8);
//   its TEMP and its TMP, one line each (UTF-8);
// and, where QKVM_STAND_IN_SETUP_RESULT names one, the line Setup writes where an update changed nothing (the contract
// with Setup), as Inno's Log() writes a line: a time, three spaces, "qKVM-RESULT: " and what it names.
// QKVM_STAND_IN_SETUP_SILENT=1 has it write nothing (a Setup that ended before it opened its log).
// QKVM_STAND_IN_SETUP_WAIT_MS has it wait that long first (at most 60 s), and QKVM_STAND_IN_SETUP_EXIT names its exit
// code (0 where it names none). A Windows program, never a console one: it shows nothing.
namespace {

std::string utf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string made(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), made.data(), size, nullptr, nullptr);
    return made;
}

// The SHA-256 of a file, read whole; empty where it could not be read.
std::string sha256Of(const std::wstring& file)
{
    const HANDLE read = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (read == INVALID_HANDLE_VALUE)
        return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string said;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<unsigned char> buffer(std::size_t{64} * 1024);
        DWORD got = 0;
        bool whole = true;
        while (ReadFile(read, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) != FALSE && got > 0) {
            if (BCryptHashData(hash, buffer.data(), got, 0) < 0) {
                whole = false;
                break;
            }
        }
        unsigned char digest[32] = {};
        if (whole && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0) {
            char hex[3] = {};
            for (const unsigned char byte : digest) {
                std::snprintf(hex, sizeof hex, "%02x", byte);
                said += hex;
            }
        }
    }
    if (hash != nullptr)
        BCryptDestroyHash(hash);
    if (algorithm != nullptr)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(read);
    return said;
}

std::wstring variable(const wchar_t* name)
{
    std::vector<wchar_t> value(32768);
    const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    return length > 0 && length < value.size() ? std::wstring(value.data(), length) : std::wstring();
}

unsigned long number(const wchar_t* name, unsigned long otherwise, unsigned long most)
{
    wchar_t value[32] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, 32);
    if (length == 0 || length >= 32)
        return otherwise;
    const unsigned long read = std::wcstoul(value, nullptr, 10);
    return read > most ? most : read;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::wstring line = GetCommandLineW();
    // Where the log goes: what stands between /LOG=" and the next quote.
    const std::wstring opening = L"/LOG=\"";
    const std::size_t from = line.find(opening);
    if (from == std::wstring::npos)
        return 90;
    const std::size_t start = from + opening.size();
    const std::size_t end = line.find(L'"', start);
    if (end == std::wstring::npos)
        return 91;
    const std::wstring log = line.substr(start, end - start);
    std::vector<wchar_t> self(32768);
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (length == 0 || length >= self.size())
        return 92;
    const std::wstring program(self.data(), length);
    Sleep(number(L"QKVM_STAND_IN_SETUP_WAIT_MS", 0, 60000));
    // As a Setup that ends before it opens its log (its stub could not unpack itself): nothing written at all.
    if (number(L"QKVM_STAND_IN_SETUP_SILENT", 0, 1) == 1) {
        return 95;
    }
    const std::string record =
        utf8(line) + "\n" + sha256Of(program) + "\n" + utf8(program) + "\n" + utf8(variable(L"TEMP")) + "\n" +
        utf8(variable(L"TMP")) + "\n" +
        (variable(L"QKVM_STAND_IN_SETUP_RESULT").empty()
             ? std::string()
             : "2026-11-08 20:34:09.306   qKVM-RESULT: " + utf8(variable(L"QKVM_STAND_IN_SETUP_RESULT")) + "\r\n");
    const HANDLE file =
        CreateFileW(log.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 93;
    DWORD written = 0;
    const bool wrote = WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) != FALSE &&
                       written == record.size();
    CloseHandle(file);
    if (!wrote)
        return 94;
    return static_cast<int>(number(L"QKVM_STAND_IN_SETUP_EXIT", 0, 255));
}
