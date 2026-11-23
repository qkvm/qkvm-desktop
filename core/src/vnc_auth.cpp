#include <qkvm/core/vnc_auth.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

#include <QCoreApplication>

#include <qkvm/core/wiped.h>
#include <qkvm/rfb/rfb.h>

#include "cng.h"

namespace qkvm::core::vnc {

namespace {

constexpr std::size_t keyBytes = 8;

QString words(const char* text)
{
    return QCoreApplication::translate("Vnc", text);
}

base::Error windowsSaidNo()
{
    return {windowsRefused, words("Windows could not make the answer to the VNC server's challenge.").toStdString()};
}

} // namespace

base::Result<Response> answer(QByteArrayView password, std::span<const std::uint8_t, challengeBytes> challenge)
{
    // The key, in room of its own that is written over when it goes: Windows wants room it may write to for all it
    // reads, and its copy of the key is its own (cng::Key).
    SecretBytes key(static_cast<qsizetype>(keyBytes));
    key.resize(static_cast<qsizetype>(keyBytes));
    rfb::vncKey(
        std::span(reinterpret_cast<const std::uint8_t*>(password.data()), static_cast<std::size_t>(password.size())),
        std::span<std::uint8_t, keyBytes>(key.data(), keyBytes));
    const cng::Algorithm des(BCRYPT_DES_ALGORITHM, 0);
    // ECB, each block of the challenge by itself, as every VNC server does it: in room Windows may write to.
    std::array<wchar_t, sizeof(BCRYPT_CHAIN_MODE_ECB) / sizeof(wchar_t)> mode{};
    std::memcpy(mode.data(), BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB));
    if (!des || !cng::worked(BCryptSetProperty(des.get(), BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode.data()),
                                               static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0))) {
        return windowsSaidNo();
    }
    const cng::Key handle(des, key.data(), static_cast<ULONG>(keyBytes));
    if (!handle) {
        return windowsSaidNo();
    }
    // Encrypted where it stands, in a copy of the challenge: two blocks of eight, no padding, no IV.
    Response made{};
    std::copy(challenge.begin(), challenge.end(), made.begin());
    ULONG done = 0;
    const NTSTATUS status = BCryptEncrypt(handle.get(), made.data(), static_cast<ULONG>(made.size()), nullptr, nullptr,
                                          0, made.data(), static_cast<ULONG>(made.size()), &done, 0);
    if (!cng::worked(status) || done != static_cast<ULONG>(made.size())) {
        return windowsSaidNo();
    }
    return made;
}

base::Result<Response> answer(QStringView password, std::span<const std::uint8_t, challengeBytes> challenge)
{
    // Its UTF-8, in room that is written over when it goes.
    const std::optional<SecretBytes> utf8 = SecretBytes::utf8Of(password);
    if (!utf8) {
        return base::Error{notAPassword, words("The password holds half a character, which no VNC server is "
                                               "sent.")
                                             .toStdString()};
    }
    return answer(utf8->view(), challenge);
}

} // namespace qkvm::core::vnc
