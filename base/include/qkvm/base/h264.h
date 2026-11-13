#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// What an H.264 stream says of itself before its pictures are decoded: its parameter sets (the size of its pictures)
// and the units of a packet in Annex B (each behind a start code). Read where the engine judges a packet before a
// decoder is handed it (the SPS gate, engine/src/decoder.cpp), and where a live line says how large its pictures are
// (a JetKVM's: core/jetkvm_live.cpp): one reading for both. Plain C++, as the rest of base is.
namespace qkvm::base::h264 {

// The id an SPS or a PPS has, and the coded size an SPS says (its unit, header byte first), as H.264's
// 7.3.2.1.1 and 7.3.2.2 have them, the emulation bytes taken out first: nothing where it cannot be read.
// It takes nothing by the size it reads (2d and the SPS gate, after their reading: every SPS of a packet
// is read, not only the one its first slice names). And what of that size is shown, the frame's cropping
// taken off (7.4.2.1.1): what a viewer's pointer is placed over.
struct ParameterSet {
    std::uint32_t id = 0;
    std::uint32_t refersTo = 0; // a PPS's SPS
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    // What is shown of it; 0 by 0 where the cropping after the size cannot be read (which does not make the set
    // unread: nothing beyond the size was read before, and what is refused stays what it was).
    std::uint64_t shownWidth = 0;
    std::uint64_t shownHeight = 0;
};
// One reading of a parameter set's bytes after its header, as they are handed.
inline std::optional<ParameterSet> readFrom(const std::vector<std::uint8_t>& rbsp, bool sps)
{
    std::size_t bit = 0;
    bool bad = false;
    const auto u = [&](int count) {
        std::uint32_t value = 0;
        for (int each = 0; each < count; ++each) {
            if (bit >= rbsp.size() * 8) {
                bad = true;
                return std::uint32_t{0};
            }
            value = (value << 1) | ((rbsp[bit / 8] >> (7 - bit % 8)) & 1U);
            ++bit;
        }
        return value;
    };
    const auto ue = [&]() -> std::uint64_t {
        int zeros = 0;
        while (!bad && u(1) == 0) {
            if (++zeros > 31) {
                bad = true;
            }
        }
        return bad ? 0 : ((std::uint64_t{1} << zeros) - 1) + u(zeros);
    };
    const auto se = [&]() -> std::int64_t {
        const std::uint64_t code = ue();
        return (code & 1U) != 0 ? static_cast<std::int64_t>((code + 1) / 2) : -static_cast<std::int64_t>(code / 2);
    };
    ParameterSet set;
    if (!sps) {
        set.id = static_cast<std::uint32_t>(ue());
        set.refersTo = static_cast<std::uint32_t>(ue());
        return bad || set.id > 255 || set.refersTo > 31 ? std::nullopt : std::optional(set);
    }
    const std::uint32_t profile = u(8);
    u(16); // the constraint flags and the level
    set.id = static_cast<std::uint32_t>(ue());
    // Only profiles that this reading and FFmpeg's read alike (2e and the SPS reader, after their reading): the
    // book and FFmpeg differ on which carry chroma fields (FFmpeg reads them for 144, an old High 4:4:4, which
    // this build of it was seen to do: anSpsOfAProfileReadTwoWaysReachesNoDecoder; the book has them for 139,
    // 134 and 135, which FFmpeg's source is said not to), and where the two read an SPS differently its size
    // here says nothing of the size the decoder takes its tables by. Baseline, main and extended have none,
    // the four high profiles have them, in both; what a KVM sends is among these. Any other is no SPS here.
    constexpr std::array<std::uint32_t, 3> withoutChroma{66, 77, 88};
    constexpr std::array<std::uint32_t, 4> withChroma{100, 110, 122, 244};
    const bool chromaFields = std::ranges::find(withChroma, profile) != withChroma.end();
    if (!chromaFields && std::ranges::find(withoutChroma, profile) == withoutChroma.end()) {
        return std::nullopt;
    }
    std::uint64_t chromaFormat = 1; // (4:2:0 where the profile says none)
    if (chromaFields) {
        const std::uint64_t chroma = ue();
        chromaFormat = chroma;
        // (FFmpeg refuses colour planes coded apart, and two depths: one it refuses on its first reading it
        // reads a second way, 2e, the second reading.)
        if (chroma == 3 && u(1) != 0) {
            return std::nullopt;
        }
        const std::uint64_t lumaDepth = ue();
        const std::uint64_t chromaDepth = ue();
        u(1);
        if (chroma > 3 || lumaDepth > 6 || chromaDepth > 6 || lumaDepth != chromaDepth) {
            return std::nullopt;
        }
        if (u(1) != 0) {
            for (int list = 0; list < (chroma != 3 ? 8 : 12) && !bad; ++list) {
                if (u(1) == 0) {
                    continue;
                }
                std::int64_t last = 8;
                std::int64_t next = 8;
                for (int entry = 0; entry < (list < 6 ? 16 : 64) && !bad; ++entry) {
                    if (next != 0) {
                        const std::int64_t delta = se();
                        if (delta < -128 || delta > 127) {
                            return std::nullopt;
                        }
                        next = (last + delta + 256) % 256;
                    }
                    last = next == 0 ? last : next;
                }
            }
        }
    }
    if (ue() > 12) { // log2_max_frame_num_minus4
        return std::nullopt;
    }
    const std::uint64_t order = ue();
    if (order == 0) {
        if (ue() > 12) {
            return std::nullopt;
        }
    } else if (order == 1) {
        u(1);
        se();
        se();
        const std::uint64_t cycle = ue();
        if (cycle > 255) {
            return std::nullopt;
        }
        for (std::uint64_t each = 0; each < cycle && !bad; ++each) {
            se();
        }
    } else if (order != 2) {
        return std::nullopt;
    }
    // max_num_ref_frames: FFmpeg refuses more than 16 (2e, the second reading, as above).
    if (ue() > 16) {
        return std::nullopt;
    }
    u(1);
    const std::uint64_t widthMbs = ue() + 1;
    const std::uint64_t heightUnits = ue() + 1;
    const std::uint32_t framesOnly = u(1);
    if (bad || set.id > 31) {
        return std::nullopt;
    }
    set.width = widthMbs * 16;
    set.height = heightUnits * 16 * (framesOnly != 0 ? 1 : 2);
    // What of it is shown: mb_adaptive_frame_field_flag where fields are coded, direct_8x8_inference_flag, and the
    // frame's cropping, in units of two pixels across where colour is at half the detail across (4:2:0, 4:2:2), and of
    // two rows down where it is at half the detail down (4:2:0), twice that where fields are coded.
    if (framesOnly == 0) {
        u(1);
    }
    u(1);
    std::uint64_t left = 0;
    std::uint64_t right = 0;
    std::uint64_t top = 0;
    std::uint64_t bottom = 0;
    if (u(1) != 0) {
        left = ue();
        right = ue();
        top = ue();
        bottom = ue();
    }
    const std::uint64_t acrossUnit = chromaFormat == 1 || chromaFormat == 2 ? 2 : 1;
    const std::uint64_t downUnit = (chromaFormat == 1 ? std::uint64_t{2} : std::uint64_t{1}) *
                                   (framesOnly != 0 ? std::uint64_t{1} : std::uint64_t{2});
    const std::uint64_t cutAcross = acrossUnit * (left + right);
    const std::uint64_t cutDown = downUnit * (top + bottom);
    if (!bad && cutAcross < set.width && cutDown < set.height) {
        set.shownWidth = set.width - cutAcross;
        set.shownHeight = set.height - cutDown;
    }
    return set;
}

// Whether an SPS read says a coded picture of a size no larger than `widest` by `tallest`.
inline bool saysASize(const ParameterSet& set, std::uint64_t widest, std::uint64_t tallest)
{
    return set.width > 0 && set.height > 0 && set.width <= widest && set.height <= tallest;
}

// A parameter set read from its unit (header byte first), as a decoder that shows pictures up to `widest` by
// `tallest` would take it (FFmpeg's, below).
inline std::optional<ParameterSet> readParameters(std::span<const std::byte> unit, bool sps, std::uint64_t widest,
                                                  std::uint64_t tallest)
{
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(std::min<std::size_t>(unit.size(), 4096));
    bool escaped = false;
    int zeroes = 0;
    for (std::size_t at = 1; at < unit.size() && rbsp.size() < 4096; ++at) {
        const auto byte = static_cast<std::uint8_t>(unit[at]);
        if (zeroes >= 2 && byte == 3) {
            zeroes = 0;
            escaped = true;
            continue;
        }
        rbsp.push_back(byte);
        zeroes = byte == 0 ? zeroes + 1 : 0;
    }
    const std::optional<ParameterSet> read = readFrom(rbsp, sps);
    if (!read || !sps || !escaped) {
        return read;
    }
    // FFmpeg reads an SPS a second way where its first reading fails: the unit as it came, its emulation
    // bytes left in ("SPS decoding failure, trying again with the complete NAL"). So where the unit has any,
    // it is read that way too, and refused unless that reading is none or says a size this program shows
    // (2e, the second reading: a size read here as small could be a huge one to that second reading).
    std::vector<std::uint8_t> raw;
    raw.reserve(std::min<std::size_t>(unit.size(), 4096));
    for (std::size_t at = 1; at < unit.size() && raw.size() < 4096; ++at) {
        raw.push_back(static_cast<std::uint8_t>(unit[at]));
    }
    if (const std::optional<ParameterSet> second = readFrom(raw, true);
        second && !saysASize(*second, widest, tallest)) {
        return std::nullopt;
    }
    return read;
}

// The units of a packet in Annex B, in their order, each without its start code (three bytes, or four: a four-byte
// start code's first zero is its own, not the end of the unit before).
inline std::vector<std::span<const std::byte>> units(std::span<const std::byte> packet)
{
    std::vector<std::span<const std::byte>> found;
    const auto nextStart = [&packet](std::size_t from) {
        for (std::size_t look = from; look + 3 <= packet.size(); ++look) {
            if (packet[look] == std::byte{0} && packet[look + 1] == std::byte{0} && packet[look + 2] == std::byte{1}) {
                return look;
            }
        }
        return packet.size();
    };
    std::size_t at = nextStart(0);
    while (at < packet.size()) {
        const std::size_t begins = at + 3;
        std::size_t ends = nextStart(begins);
        const std::size_t next = ends;
        while (ends > begins && packet[ends - 1] == std::byte{0} && next < packet.size()) {
            --ends;
        }
        if (ends > begins) {
            found.push_back(packet.subspan(begins, ends - begins));
        }
        at = next;
    }
    return found;
}

// What is shown of the pictures of a packet (a key frame, its SPS in it), by its first SPS that reads as a size no
// larger than `widest` by `tallest`; nothing where it has none, or that says nothing of what is shown.
inline std::optional<std::pair<int, int>> shownSize(std::span<const std::byte> packet, std::uint64_t widest,
                                                    std::uint64_t tallest)
{
    for (const std::span<const std::byte> unit : units(packet)) {
        if ((static_cast<std::uint8_t>(unit[0]) & 0x1F) != 7) {
            continue;
        }
        const std::optional<ParameterSet> set = readParameters(unit, true, widest, tallest);
        if (set && saysASize(*set, widest, tallest) && set->shownWidth > 0 && set->shownHeight > 0) {
            return std::pair{static_cast<int>(set->shownWidth), static_cast<int>(set->shownHeight)};
        }
    }
    return std::nullopt;
}

} // namespace qkvm::base::h264
