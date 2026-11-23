#pragma once

#include <qkvm/base/fuzz_promise.h>
#include <qkvm/rfb/rfb.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "../src/jpeg_pixels.h"

// The harness of the RFB reader (fuzz_rfb.cpp, libFuzzer's way in; test_core_fuzz_found, what it once found), with the
// inflater and the JPEG walk and decode behind it: rfb.cpp, inflate.cpp, keysyms.cpp and jpeg_pixels.cpp are compiled
// into the fuzz target, so libFuzzer sees their branches. An input is what a server sends from its greeting on (Intel
// AMT's "RFB 004.000" among the greetings read as 3.8). It is read twice: fed whole, and fed in two at its middle (what
// a feed leaves unread taken up again by the next). The decoder is the screen's own (JpegPixels), and the clocks the
// share is kept by advance a millisecond each time they are read, the same for every run of an input.
//
// Its canary (fuzz_promise.h): a stream read whole whose one update drew one Tight JPEG, which the screen's decoder
// made into pixels, away from the screen's top left corner (drawn in at an offset of rows and columns; each seed's
// Tight JPEG is at the corner).
namespace qkvm::fuzz {

namespace rfbs {

inline rfb::Options optionsFor(core::JpegPixels& pixels, std::int64_t& wall, std::int64_t& work, int& decoded)
{
    rfb::Options options;
    options.signsIn = {rfb::securityNone};
    options.greetedAs38 = {"RFB 004.000\n"};
    options.jpeg = [&pixels, &decoded](std::span<const std::uint8_t> jpeg, int width, int height) {
        base::Result<rfb::Bytes> made = pixels.decode(jpeg, width, height);
        if (made.ok()) {
            ++decoded;
        }
        return made;
    };
    options.clock = [&wall] { return ++wall; };
    options.workClock = [&work] { return ++work; };
    return options;
}

inline rfb::Client::Changed readAll(rfb::Client& client)
{
    static_cast<void>(client.takeOutgoing());
    rfb::Client::Changed changed = client.take();
    static_cast<void>(client.pausedForMs());
    static_cast<void>(client.messageSince());
    promise(client.rgb().size() ==
                std::size_t{3} * static_cast<std::size_t>(client.width()) * static_cast<std::size_t>(client.height()),
            "the screen is not a whole screen");
    return changed;
}

} // namespace rfbs

inline void readRfb(std::span<const std::uint8_t> input)
{
    static core::JpegPixels pixels; // (one decoder, as a connection has: used by one thread at a time)
    {
        std::int64_t wall = 0;
        std::int64_t work = 0;
        int decoded = 0;
        rfb::Client client(rfbs::optionsFor(pixels, wall, work, decoded));
        const bool read = client.feed(input, 0).ok();
        const rfb::Client::Changed changed = rfbs::readAll(client);
        // (A Tight JPEG is one change of the screen: the one change of an update of nothing else.)
        if (read && decoded == 1 && client.updates() == 1 && client.frames() == 1 && changed.rects.size() == 1 &&
            (changed.rects.front().x > 0 || changed.rects.front().y > 0)) {
            reached("a Tight JPEG decoded into pixels and drawn away from the screen's top left corner");
        }
    }
    {
        std::int64_t wall = 0;
        std::int64_t work = 0;
        int decoded = 0;
        rfb::Client client(rfbs::optionsFor(pixels, wall, work, decoded));
        if (client.feed(input.first(input.size() / 2), 0).ok()) {
            rfbs::readAll(client);
            static_cast<void>(client.feed(input.subspan(input.size() / 2), 1));
            rfbs::readAll(client);
        }
    }
}

} // namespace qkvm::fuzz
