#pragma once

#include <qkvm/base/result.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// The picture a server's management controller sends, decoded: ASPEED's video format (aspeed_format.h).
//
// Most management controllers are built on a chip by ASPEED, and that chip compresses the server's screen in
// a format of its own before the controller sends it to its console. This reads that format and keeps the
// screen as it stands, as plain RGB. A frame only brings the blocks that changed: everything else stays as
// it was. As the web's app/qkvm/aspeed.py, picture for picture.
//
// Nothing here trusts what it is given: a frame that makes no sense is refused or runs out, a block that says
// it is outside the screen is not drawn, and no frame takes longer than its own length and its screen allow.
// Plain C++ on the standard library alone (no Qt, no Windows): where it runs, in the program's own process or
// in a worker of its own, is decided by whoever hosts it.
namespace qkvm::bmc::aspeed {

// What a frame may cost. A controller is not trusted, and one in the wrong hands sends what it likes, for as
// long as somebody looks at the wall: nothing it sends takes more time or room than a screen of its size could.
// No screen is wider or taller than this (the chip that makes these pictures stops at 1920 by 1200).
inline constexpr int largest = 2048;
// Nor does one screenful take more bytes than this, and none more than twice what it is unpacked.
inline constexpr std::size_t longest = std::size_t{8} * 1024 * 1024;
// Blocks a frame may carry beyond two for every one its screen has.
inline constexpr int spare = 64;
// What a frame may say beyond twice its unpacked screen (a header's worth, as the web allows it).
inline constexpr std::size_t slack = 4096;

// What comes with a frame's data and says how to read it.
struct FrameHeader {
    int width = 0; // the screen
    int height = 0;
    int gridWidth = 0; // the same, as the blocks are counted (rounded up by the sender)
    int gridHeight = 0;
    bool halfColour = false;    // colour at half the detail (4:2:0): blocks of 16x16
    int strength = 0;           // which of the eight quantization strengths, 0 the coarsest
    int correctionStrength = 0; // the same for blocks that are a correction
    bool lumaForColour = false; // the colour planes use the brightness plane's table
    bool operator==(const FrameHeader&) const = default;
};

// The screen as it stands: three bytes a pixel (red, green, blue), row after row, `width` by `height`. Its
// size and its bytes are one thing: whatever holds a picture holds them together.
struct Picture {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

// A part of the screen, in pixels from its top left.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const Rect&) const = default;
};

// A server's screen, kept up to date frame by frame. One thread at a time reads into it and looks at it;
// `giveUp` alone may be called from any other.
class Screen {
public:
    Screen();
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    // Takes in one frame: how many blocks of the screen it changed, or why it was refused ("garbled", or
    // "given_up" once `giveUp` was called). A frame refused part of the way through leaves what it drew so
    // far, as the web's does; the screen is a whole picture all the same.
    base::Result<int> read(const FrameHeader& header, std::span<const std::uint8_t> data);

    // Empty (0 by 0) until a frame has said what size the screen is.
    const Picture& picture() const noexcept { return m_picture; }

    // Whether a frame of this header would be read into a picture made anew in place of the one this screen holds:
    // a size, or a detail of colour, other than that one's, and a header this screen takes. False before the first
    // picture, and for a header it refuses (that frame changes nothing it holds).
    bool resizedBy(const FrameHeader& header) const noexcept;

    // What the last frame changed of the picture, also where it was refused part of the way through: for each
    // row of eight pixels it drew in, from the leftmost of the blocks it drew there to the rightmost (with what lies
    // between), rows one under the other with the same span joined into one; top to bottom, none outside the
    // screen, none for a frame that changed nothing. Good until the next read.
    std::span<const Rect> changed() const noexcept { return std::span(m_changed).first(m_changes); }

    // The bytes this screen holds: 3 a pixel for its picture, 192 an 8x8 block for what the block was last sent as
    // (none at half the detail, where nothing is corrected), and 24 a row of eight pixels for what a frame changed.
    // 24 MiB for the largest screen there may be.
    std::size_t held() const noexcept;

    // Whoever no longer waits for the frame being read says so: it is let go of within 64 blocks, and so is
    // every frame after it.
    void giveUp() noexcept { m_givenUp.store(true, std::memory_order_relaxed); }

    using Samples = std::array<std::uint8_t, 64>;
    // What a block was last sent as, for a correction that may come: brightness, blue and red difference.
    struct Kept {
        Samples y;
        Samples cb;
        Samples cr;
    };
    using Quantizers = std::array<std::int32_t, 64>;

private:
    base::Result<void> fit(const FrameHeader& header);
    int draw(int tx, int ty, const Samples& y, const Samples& cb, const Samples& cr, bool keep);
    int drawLarge(int tx, int ty, const std::array<Samples, 4>& ys, const Samples& cb, const Samples& cr);
    const Kept& was(int tx, int ty) const noexcept;
    // A block drawn: its rows of eight pixels changed from `left` to `left + wide`.
    void drawn(int top, int high, int left, int wide) noexcept;
    // What the rows that changed come to, as rectangles (changed()).
    void collect() noexcept;

    bool m_fitted = false;
    FrameHeader m_header;
    int m_across = 0; // blocks, as the sender counts them
    int m_down = 0;
    std::array<Quantizers, 4> m_quant{};
    std::array<std::uint32_t, 4> m_colours;
    std::array<int, 4> m_chosen = {0, 1, 2, 3};
    int m_keptAcross = 0;
    std::vector<Kept> m_kept; // what each 8x8 block on the screen was last sent as
    // For each row of eight pixels, the span the frame being read changed (from, to), and what that came to.
    std::vector<std::pair<int, int>> m_rows;
    std::vector<Rect> m_changed; // as many as there are rows; the first m_changes of them
    std::size_t m_changes = 0;
    Picture m_picture;
    std::atomic<bool> m_givenUp = false;
};

} // namespace qkvm::bmc::aspeed
