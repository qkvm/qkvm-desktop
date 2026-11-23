#pragma once

#include "rfb_by_hand.h"

#include <vector>

// The streams the reader's exhaustive reading sweeps (test_rfb_sweeps.cpp), which are also the first inputs of its fuzz
// target (core/fuzz/make_seeds.cpp): every encoding this program reads, and every way of each it has (ZRLE's five kinds
// of tile, Tight's filters, its fill and its JPEG, Hextile's raw tiles and subrectangles), each the handshake and one
// update, small enough to be read whole a few hundred thousand times (round-4 review LOW 4).
namespace qkvm::rfb::tests {

// The handshake every sweep's stream begins with: a server of 3.8, no password, a screen of 16 by 16.
inline Bytes sweepHandshake()
{
    return join({greeting(), securityTypes({1}), word(0), serverInit(16, 16, "s")});
}

inline std::vector<Bytes> sweepStreams()
{
    const auto whole = [](const Bytes& rect) { return join({sweepHandshake(), update({rect})}); };
    std::vector<Bytes> streams;
    streams.push_back(whole(rawRect(0, 0, pattern(8, 8))));
    streams.push_back(whole(copyRectOf(0, 0, 8, 8, 8, 8)));
    streams.push_back(whole(desktopSizeRect(12, 10)));
    streams.push_back(whole(hextileRect(0, 0, quarters(16, 16), true)));
    streams.push_back(whole(hextileRect(0, 0, pattern(8, 8), false)));
    streams.push_back(whole(hextileAllSubrects(0, 0, 16, 16, 5)));
    for (const ZrleTiles way :
         {ZrleTiles::Raw, ZrleTiles::Solid, ZrleTiles::Packed, ZrleTiles::PlainRuns, ZrleTiles::PaletteRuns}) {
        Deflater stream;
        const Picture pixels = way == ZrleTiles::Solid ? plain(8, 8, {1, 2, 3})
                               : way == ZrleTiles::Raw ? pattern(8, 8)
                                                       : quarters(8, 8);
        streams.push_back(whole(zrleRect(0, 0, pixels, way, stream)));
    }
    {
        Deflater stream;
        streams.push_back(whole(tightRect(0, 0, pattern(8, 8), TightFilter::Copy, stream)));
    }
    {
        Deflater stream;
        streams.push_back(whole(tightRect(0, 0, quarters(8, 8), TightFilter::Palette, stream)));
    }
    {
        Deflater stream;
        streams.push_back(whole(tightRect(0, 0, halves(6, 3), TightFilter::Palette, stream))); // two colours: bits
    }
    {
        Deflater stream;
        streams.push_back(whole(tightRect(0, 0, pattern(8, 8), TightFilter::Gradient, stream)));
    }
    streams.push_back(whole(tightFill(0, 0, 16, 16, {4, 5, 6})));
    streams.push_back(whole(tightJpeg(0, 0, 8, 8, madeUpJpeg(pattern(8, 8)))));
    return streams;
}

} // namespace qkvm::rfb::tests
