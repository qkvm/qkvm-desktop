#pragma once

#include <QtGlobal>
#include <qkvm/base/result.h>
#include <qkvm/rfb/rfb.h>

#include <cstdint>
#include <span>

#include "rfb_by_hand.h"

// The reader's side of the tests' server (rfb_by_hand.h): a decoder of its made-up JPEGs, a client's options with
// it, and what went wrong said in the test's output. Used by the reader's tests and its sweeps.
namespace qkvm::rfb::tests {

// A decoder of the tests' made-up JPEGs ("JPG", the size, the pixels).
inline base::Result<Bytes> madeUpDecoder(std::span<const std::uint8_t> data, int width, int height)
{
    if (data.size() < 7 || data[0] != 'J' || data[1] != 'P' || data[2] != 'G') {
        return base::Error{"garbled", "no JPEG"};
    }
    const int saidWidth = (data[3] << 8) | data[4];
    const int saidHeight = (data[5] << 8) | data[6];
    // (A picture of the size it says, which may be another than its rectangle's.)
    static_cast<void>(width);
    static_cast<void>(height);
    if (data.size() !=
        7 + std::size_t{3} * static_cast<std::size_t>(saidWidth) * static_cast<std::size_t>(saidHeight)) {
        return base::Error{"garbled", "a JPEG cut short"};
    }
    return Bytes(data.begin() + 7, data.end());
}

inline Options withJpeg()
{
    Options options;
    options.jpeg = madeUpDecoder;
    return options;
}

// Whether it went through; where not, why, in the test's output.
inline bool okay(const base::Result<void>& result)
{
    if (!result.ok()) {
        qWarning("%s: %s", result.error().code.c_str(), result.error().message.c_str());
    }
    return result.ok();
}

} // namespace qkvm::rfb::tests
