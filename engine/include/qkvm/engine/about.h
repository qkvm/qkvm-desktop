#pragma once

#include <string>

namespace qkvm::engine {

// The library that decodes the pictures, as it describes itself. For the About box, for the first
// line of the log, and for refusing to run with a build that must not ship.
struct DecoderLibrary {
    std::string version;       // as its builder named it
    std::string licence;       // in its own words
    std::string configuration; // the switches it was built with
};

// What that library can do of what the engine needs.
struct DecoderAbilities {
    bool h264 = false;          // H.264 at all
    bool h264OnTheCard = false; // H.264 on the graphics card, through Direct3D 11
    bool jpeg = false;          // the pictures a wall's cards are made of
    bool png = false;           // and what a machine sends in their place when it has none
};

// Both may be called from any thread, and cost next to nothing.
DecoderLibrary decoderLibrary();
DecoderAbilities decoderAbilities();

} // namespace qkvm::engine
