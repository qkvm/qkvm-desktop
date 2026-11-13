#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <qkvm/base/result.h>
#include <qkvm/engine/types.h>

#include "ffmpeg.h"

namespace qkvm::engine {

class GpuCore;

// A decoded picture as the decoder left it: planes in memory, or a slice of a texture on the card.
// Empty when there is none.
struct Picture {
    FramePtr frame;
    std::int64_t arrivedUs = 0;
    // When its decoding began, and when it was done (the stream sets both).
    std::int64_t beganUs = 0;
    std::int64_t decodedUs = 0;

    explicit operator bool() const noexcept { return frame != nullptr; }
    bool onTheCard() const noexcept { return frame != nullptr && frame->format == AV_PIX_FMT_D3D11; }
};

// One way of decoding (DESIGN.md section 3.2). A decoder belongs to one stream and is used by one
// thread at a time.
class Decoder {
public:
    virtual ~Decoder() = default;

    // Decodes one packet. `padded` is the packet followed by AV_INPUT_BUFFER_PADDING_SIZE bytes of
    // zero, which FFmpeg reads into; `bytes` is the packet's own length. Gives the picture, or an
    // empty one when the decoder wants more before it has one, or why it failed.
    virtual base::Result<Picture> decode(std::span<std::byte> padded, std::size_t bytes, std::int64_t arrivedUs) = 0;
    virtual Decoding kind() const noexcept = 0;
};

// The code of a packet a decoder refused before it was handed it: parameters that differ, or a size too
// large (the SPS gate, decoder.cpp). The packet, not the decoder, is at fault: a stream on the card does
// not give up on it for that (2d and the SPS gate, after their reading).
inline constexpr std::string_view refusedByTheGate = "refused_by_the_gate";

// H.264 or stills on the processor.
base::Result<std::unique_ptr<Decoder>> makeSoftwareDecoder(Codec codec);
// H.264 on the graphics card, through Direct3D 11. The picture never leaves the card.
base::Result<std::unique_ptr<Decoder>> makeCardDecoder(GpuCore& gpu);

} // namespace qkvm::engine
