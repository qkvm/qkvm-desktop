// VNC authentication's DES key of a password (rfb.py's _mirrored and vnc_key).
#include <qkvm/rfb/rfb.h>

#include <algorithm>
#include <cstddef>

namespace qkvm::rfb {

namespace {

// A byte with its bits the other way round: what VNC does to each byte of the password.
std::uint8_t mirrored(std::uint8_t byte) noexcept
{
    std::uint8_t made = 0;
    for (int bit = 0; bit < 8; ++bit) {
        made = static_cast<std::uint8_t>((made << 1) | ((byte >> bit) & 1));
    }
    return made;
}

} // namespace

void vncKey(std::span<const std::uint8_t> password, std::span<std::uint8_t, 8> key) noexcept
{
    // Noughts after a shorter password: the room is the caller's, and what stood in it before is not left there.
    std::fill(key.begin(), key.end(), std::uint8_t{0});
    const std::size_t taken = std::min(password.size(), key.size());
    for (std::size_t at = 0; at < taken; ++at) {
        key[at] = mirrored(password[at]);
    }
}

} // namespace qkvm::rfb
