#pragma once

#include <QtTypes>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <bcrypt.h>

// Windows' own cryptography (CNG, bcrypt.dll), as scrypt and the sealing of a file use it: what
// Windows hands out is given back when these go.
namespace qkvm::core::cng {

inline bool worked(NTSTATUS status) noexcept
{
    return status >= 0;
}

// One of Windows' algorithms, by its name.
class Algorithm {
public:
    Algorithm(LPCWSTR name, ULONG flags) noexcept
    {
        if (!worked(BCryptOpenAlgorithmProvider(&m_handle, name, nullptr, flags))) {
            m_handle = nullptr;
        }
    }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
    ~Algorithm()
    {
        if (m_handle != nullptr) {
            BCryptCloseAlgorithmProvider(m_handle, 0);
        }
    }

    explicit operator bool() const noexcept { return m_handle != nullptr; }
    BCRYPT_ALG_HANDLE get() const noexcept { return m_handle; }

private:
    BCRYPT_ALG_HANDLE m_handle = nullptr;
};

// A key as Windows holds one for an algorithm. Windows keeps a copy of the bytes that is its
// own, for as long as the key is held: what becomes of that copy afterwards is Windows' to say.
class Key {
public:
    Key(const Algorithm& algorithm, quint8* bytes, ULONG count) noexcept
    {
        if (!algorithm ||
            !worked(BCryptGenerateSymmetricKey(algorithm.get(), &m_handle, nullptr, 0, bytes, count, 0))) {
            m_handle = nullptr;
        }
    }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    ~Key()
    {
        if (m_handle != nullptr) {
            BCryptDestroyKey(m_handle);
        }
    }

    explicit operator bool() const noexcept { return m_handle != nullptr; }
    BCRYPT_KEY_HANDLE get() const noexcept { return m_handle; }

private:
    BCRYPT_KEY_HANDLE m_handle = nullptr;
};

} // namespace qkvm::core::cng
