#pragma once

#include <string>

#include <QByteArray>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <bcrypt.h>

// AES-256-GCM asked of Windows by hand, and not through the program's own sealing: with a nonce
// of whatever length a test names. The program seals with twelve bytes and opens nothing else; a
// QKVM server would open what was sealed with eight to a hundred and twenty-eight. What is made
// here is what such a file would hold.
namespace qkvm::core::testing {

// The bytes encrypted and the tag of sixteen behind them; nothing where Windows will not.
inline QByteArray sealedByWindows(QByteArray key, QByteArray nonce, const QByteArray& plain, QByteArray context)
{
    BCRYPT_ALG_HANDLE aes = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    QByteArray sealed;
    std::wstring mode = BCRYPT_CHAIN_MODE_GCM;
    QByteArray tag(16, '\0');
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = context.isEmpty() ? nullptr : reinterpret_cast<PUCHAR>(context.data());
    info.cbAuthData = static_cast<ULONG>(context.size());
    info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    info.cbTag = static_cast<ULONG>(tag.size());
    // (With room for one byte where there is nothing to seal: Windows is handed something to
    // point at.)
    QByteArray bytes = plain + QByteArray(1, '\0');
    auto* at = reinterpret_cast<PUCHAR>(bytes.data());
    const auto count = static_cast<ULONG>(plain.size());
    ULONG done = 0;
    if (BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode.data()),
                          static_cast<ULONG>((mode.size() + 1) * sizeof(wchar_t)), 0) >= 0 &&
        BCryptGenerateSymmetricKey(aes, &handle, nullptr, 0, reinterpret_cast<PUCHAR>(key.data()),
                                   static_cast<ULONG>(key.size()), 0) >= 0 &&
        BCryptEncrypt(handle, at, count, &info, nullptr, 0, at, count, &done, 0) >= 0 && done == count) {
        sealed = bytes.left(plain.size()) + tag;
    }
    if (handle != nullptr) {
        BCryptDestroyKey(handle);
    }
    if (aes != nullptr) {
        BCryptCloseAlgorithmProvider(aes, 0);
    }
    return sealed;
}

} // namespace qkvm::core::testing
