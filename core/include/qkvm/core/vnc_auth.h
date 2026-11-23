#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <QByteArrayView>
#include <QStringView>

#include <qkvm/base/result.h>

// VNC authentication (RFB, RFC 6143 section 7.2.2): the server's challenge of sixteen bytes, answered with their DES
// under a key made of the password (rfb::vncKey: its first eight bytes, each with its bits mirrored), as the web's
// rfb.vnc_response. The DES is Windows' own (CNG, BCRYPT_DES_ALGORITHM in ECB mode, the two blocks of the challenge
// each by itself), as scrypt and the sealing of a file take theirs from it: no library is added for it, and none is
// written here. Single DES under at the most eight bytes of a password is weak, and the RFC says so: it is what VNC
// servers ask, and it is answered only where a server asks it and a password is held for that server.
//
// What is made of the password on the way (its UTF-8, the key) stands in room of its own that is written over when it
// goes, on every way out (SecretBytes); Windows' own copy of the key, for as long as it holds the key, is Windows' to
// say (cng::Key). Nothing of the password, its key or the answer is in an error. From any thread.
namespace qkvm::core::vnc {

inline constexpr std::size_t challengeBytes = 16;
using Response = std::array<std::uint8_t, challengeBytes>;

// What cannot be answered, in codes of the program's own (written with a colon, so that no server can say them).
inline constexpr const char* notAPassword = "desktop:not_a_password"; // a text that holds half a character
inline constexpr const char* windowsRefused = "desktop:crypto";       // Windows did not do the DES

// The sixteen bytes that answer a server's challenge: for a password as its bytes (UTF-8, of which the first eight
// count), or as a text (its UTF-8; `notAPassword` where it holds a surrogate by itself, which no bytes say).
base::Result<Response> answer(QByteArrayView password, std::span<const std::uint8_t, challengeBytes> challenge);
base::Result<Response> answer(QStringView password, std::span<const std::uint8_t, challengeBytes> challenge);

} // namespace qkvm::core::vnc
