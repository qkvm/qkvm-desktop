#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <qkvm/base/result.h>

namespace qkvm::core {

// A JPEG made into pixels, three bytes a pixel, row after row: what a VNC server's Tight encoding sends of its screen
// (OpenBMC's console sends every frame so), drawn into the screen the RFB reader keeps. Decoded by FFmpeg's own JPEG
// decoder, the one the engine decodes a KVM's stills with, on the processor, in the thread that reads the screen.
//
// A JPEG is held to its rectangle before it is decoded: one that does not say that very size, or says none, is
// refused, and the decoder is told the most pixels it may make (FFmpeg's max_pixels), so that nothing it is given makes
// a larger picture than the rectangle's. One decoder for a connection's JPEGs, used by one thread at a time.
class JpegPixels {
public:
    JpegPixels();
    ~JpegPixels();
    JpegPixels(const JpegPixels&) = delete;
    JpegPixels& operator=(const JpegPixels&) = delete;
    JpegPixels(JpegPixels&&) = delete;
    JpegPixels& operator=(JpegPixels&&) = delete;

    // The JPEG `jpeg` as `width` by `height` pixels; "garbled" where it is not a JPEG of that size, or cannot be read.
    base::Result<std::vector<std::uint8_t>> decode(std::span<const std::uint8_t> jpeg, int width, int height);

    // The size a JPEG says it is (its frame's header), without decoding it; none where it says none.
    struct Size {
        int width = 0;
        int height = 0;
    };
    static std::optional<Size> sizeOf(std::span<const std::uint8_t> jpeg);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace qkvm::core
