#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qkvm::engine {

// Sizes and places are in physical pixels, never in the scaled ones of an interface toolkit.
struct SizePx {
    int width = 0;
    int height = 0;
    bool operator==(const SizePx&) const = default;
};

struct RectPx {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const RectPx&) const = default;
};

// What a stream's packets are. A still is a whole picture by itself, a JPEG or a PNG, told apart by
// how it begins: one stream may send now one and now the other. A key-frame still is a still that may
// also be an H.264 key frame beginning with its SPS (a JetKVM's card, design 4 of M6): only a stream
// made so takes one, since its source says what it sends; how a packet begins never decides that.
// A still may also be a whole screen of the kind below (a server's card).
//
// Rgb is a screen sent as what changed of it (base/screen_changes.h: a server's screen, from its
// controller's console): a whole screen first, and then the rectangles that changed, each applied to
// the screen as it stands, in order. Only what changed is copied to the graphics card. A packet is a
// key frame where it is the whole screen: one that is lost (decoding fell behind) or that cannot be
// applied loses what it changed, and the stream waits for a whole screen again and wishes for one.
enum class Codec { H264, Still, KeyFrameStill, Rgb };

// How much a stream matters to whoever looks at the window. It decides who gets the graphics card
// when there is not enough of it for all (DESIGN.md section 3.5).
enum class Importance { Preview, ShownLarge, Controlled };

// How a stream's pictures are being decoded just now.
enum class Decoding { NotYet, OnTheCard, InSoftware };

// What the owner chose in the settings: let the program decide, or one way for every stream. A
// stream the card cannot decode is decoded in software whatever was chosen.
enum class DecodingChoice { Automatic, GraphicsCard, Software };

// One picture as it came: an H.264 access unit in Annex B form, or a JPEG.
struct Packet {
    std::span<const std::byte> data;
    std::int64_t arrivedUs = 0; // on this PC's steady clock
    bool keyFrame = false;
};

using StreamId = std::uint32_t;

// What comes from outside is checked against these before memory is taken for it.
inline constexpr int maxPictureWidth = 4096;
inline constexpr int maxPictureHeight = 2304;
inline constexpr std::size_t maxPacketBytes = std::size_t{8} * 1024 * 1024;

// What a stream knows of itself, for the diagnostics line and for the measurements. The counts are
// since the stream was made; the times are means over about the last second.
struct Stats {
    Decoding decoding = Decoding::NotYet;
    SizePx picture;
    std::uint64_t pushed = 0;        // packets handed in
    std::uint64_t droppedBehind = 0; // packets thrown away because decoding had fallen behind
    std::uint64_t refused = 0;       // packets that were too large, or that the decoder could not read
    std::uint64_t decoded = 0;       // pictures that came out of the decoder
    std::uint64_t replaced = 0;      // pictures a newer one took the place of before they were drawn
    std::uint64_t shown = 0;         // pictures that were drawn and reached the screen
    std::uint64_t stillsSame = 0;    // stills let go as byte for byte the last decoded (also in pushed)
    std::uint64_t uploadedBytes = 0; // copied to the card from this program's memory (what software decoded)
    double decodeMs = 0;             // packet in, picture out
    double uploadMs = 0;             // a software picture copied to the card
    double arrivedToShownMs = 0;     // packet arrived, picture on the screen
    double arrivedToShownMaxMs = 0;  // the worst of those
    // How many pictures took how long from arriving to being on the screen: up to each of
    // arrivedToShownStepsMs, and in the last place everything slower.
    std::array<std::uint64_t, 8> arrivedToShownCounts{};
    // The time between two new pictures, as their packets arrived and as they reached the screen:
    // up to each of gapStepsMs, and in the last place everything slower. Even steps on the screen
    // are what looks smooth; steps on the screen that are uneven where the arrivals' are not are
    // this program's own. (On the screen means: the presentation of the frame returned. Steps on
    // the screen are counted afresh after a frame the stream was not drawn in, and after the
    // device was lost; a window that draws nothing at all, made small, is not told apart.)
    std::array<std::uint64_t, 8> arrivalGapCounts{};
    std::array<std::uint64_t, 8> shownGapCounts{};
    double shownGapMaxMs = 0; // the longest of those on the screen
    // How much longer or shorter a picture took from arriving to the screen than the one shown
    // before it: what this program adds to the unevenness of the source, whatever its pace. Up to
    // each of unevenStepsMs, and in the last place everything more.
    std::array<std::uint64_t, 8> unevenCounts{};
    // Where the time from arriving to the screen goes, for the pictures shown: waiting for the
    // decoder to come to the packet; decoding it (decodeMs); the decoded picture waiting for the
    // frame that draws it (the host's own pace: in a Qt window its next frame); and that frame,
    // from the picture taken to draw to the presentation returned (with a picture decoded in
    // software its copy to the card). Running means, and the last two counted up to each of
    // arrivedToShownStepsMs as well.
    double waitedMs = 0;
    double readyToDrawnMs = 0;
    double drawnToShownMs = 0;
    std::array<std::uint64_t, 8> readyToDrawnCounts{};
    std::array<std::uint64_t, 8> drawnToShownCounts{};
};
inline constexpr std::array<double, 7> arrivedToShownStepsMs = {2, 4, 8, 12, 16, 33, 100};
// Each a pace with a little wobble: 120 and 60 pictures a second, 30, 20, 15 and 10, and slow.
inline constexpr std::array<double, 7> gapStepsMs = {12, 21, 38, 54, 71, 104, 250};
// Within a refresh of a 120 Hz screen is up to 8, of a 60 Hz one up to 17.
inline constexpr std::array<double, 7> unevenStepsMs = {2, 4, 8, 12, 17, 25, 34};

// What a host is told when something went wrong that it should know of.
enum class Fault {
    StreamFailed,   // a stream's decoder gave up; the stream starts again at the next key frame
    DeviceLost,     // Windows took the graphics device away; nothing is drawn until it is back
    DeviceRestored, // it is back, and every stream needs a key frame
};

} // namespace qkvm::engine
