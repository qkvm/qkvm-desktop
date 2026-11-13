#pragma once

#include <qkvm/bmc/aspeed.h>

#include <cstdint>
#include <span>
#include <vector>

// What a fuzz target of ASPEED's frames takes in: up to four frames, each its header in twelve bytes (width,
// height, grid width and grid height as signed 16-bit numbers; a byte of flags, 1 colour at half the detail and
// 2 the brightness table for colour; the two strengths as signed bytes; a byte to spare), the length of its data
// in four, and its data. Written by make_seeds.cpp from the tests' own frames, read by the fuzz targets.
namespace qkvm::bmc::fuzz {

inline constexpr std::size_t headerBytes = 16;
inline constexpr int mostFrames = 4;

inline std::int16_t signed16(std::span<const std::uint8_t> at)
{
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(at[0] | (at[1] << 8)));
}

inline aspeed::FrameHeader headerOf(std::span<const std::uint8_t> at)
{
    return {signed16(at.subspan(0, 2)),
            signed16(at.subspan(2, 2)),
            signed16(at.subspan(4, 2)),
            signed16(at.subspan(6, 2)),
            (at[8] & 1) != 0,
            static_cast<std::int8_t>(at[9]),
            static_cast<std::int8_t>(at[10]),
            (at[8] & 2) != 0};
}

inline std::uint32_t lengthOf(std::span<const std::uint8_t> at)
{
    return std::uint32_t{at[0]} | (std::uint32_t{at[1]} << 8) | (std::uint32_t{at[2]} << 16) |
           (std::uint32_t{at[3]} << 24);
}

inline void put16(std::vector<std::uint8_t>& into, int value)
{
    into.push_back(static_cast<std::uint8_t>(value));
    into.push_back(static_cast<std::uint8_t>(value >> 8));
}

// One frame as a fuzz target takes it.
inline void putFrame(std::vector<std::uint8_t>& into, const aspeed::FrameHeader& header,
                     std::span<const std::uint8_t> data)
{
    put16(into, header.width);
    put16(into, header.height);
    put16(into, header.gridWidth);
    put16(into, header.gridHeight);
    into.push_back(static_cast<std::uint8_t>((header.halfColour ? 1 : 0) | (header.lumaForColour ? 2 : 0)));
    into.push_back(static_cast<std::uint8_t>(header.strength));
    into.push_back(static_cast<std::uint8_t>(header.correctionStrength));
    into.push_back(0);
    const auto length = static_cast<std::uint32_t>(data.size());
    for (int shift = 0; shift < 32; shift += 8) {
        into.push_back(static_cast<std::uint8_t>(length >> shift));
    }
    into.insert(into.end(), data.begin(), data.end());
}

} // namespace qkvm::bmc::fuzz
