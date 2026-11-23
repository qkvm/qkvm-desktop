#pragma once

#include <qkvm/base/fuzz_promise.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "../src/jpeg_pixels.h"

// The harness of the JPEG walk (JpegPixels::sizeOf and the strict marker walk behind it) and the decode the walk lets
// through (fuzz_jpeg.cpp, libFuzzer's way in; test_core_fuzz_found, what it once found): jpeg_pixels.cpp is compiled
// into the fuzz target, so libFuzzer sees its branches. What the walk takes is decoded at the size it says, where that
// is a million pixels or fewer (FFmpeg is told the most pixels it may make: no larger picture than that), and is held
// to it: three bytes a pixel, whole.
//
// Its canary (fuzz_promise.h): a JPEG the walk took to its scan and the decoder made into pixels, at a side that is no
// multiple of eight (every seed's sides are).
namespace qkvm::fuzz {

inline void readJpeg(std::span<const std::uint8_t> jpeg)
{
    static core::JpegPixels pixels;
    const auto said = core::JpegPixels::sizeOf(jpeg);
    if (said.has_value() && std::int64_t{said->width} * said->height <= std::int64_t{1} << 20) {
        // (A frame's header may say nought for a side: the decoder is then asked for no pixels, and makes none.)
        promise(said->width >= 0 && said->height >= 0, "a JPEG of a size below nought was taken");
        const auto made = pixels.decode(jpeg, said->width, said->height);
        promise(!made.ok() || made.value().size() == std::size_t{3} * static_cast<std::size_t>(said->width) *
                                                         static_cast<std::size_t>(said->height),
                "a JPEG was made into another size than it said");
        if (made.ok() && said->width > 0 && said->height > 0 && (said->width % 8 != 0 || said->height % 8 != 0)) {
            reached("a JPEG the walk took, decoded whole at a side that is no multiple of eight");
        }
    }
}

} // namespace qkvm::fuzz
