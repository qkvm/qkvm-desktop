#pragma once

#include <cstdint>
#include <memory>

namespace qkvm::bench {

// What this program costs the PC, as Windows counts it.
struct Cost {
    double processorPercent = 0; // of all the PC's processors together, since the last time asked
    double decodingPercent = 0;  // of the graphics cards' video decoders, this program's part
    double drawingPercent = 0;   // of the graphics cards' drawing, this program's part
    std::uint64_t committedBytes = 0;
    std::uint64_t videoBytes = 0; // the graphics cards' own memory this program holds (Dedicated Usage), as it stands
    std::uint32_t handles = 0;
    std::uint32_t windowObjects = 0; // what the window system keeps for the program
};

// Asks Windows. One of these, asked from one thread, about once a second or slower: the numbers are
// means since the last asking, and the first asking gives nothing for those.
class Meter {
public:
    Meter();
    ~Meter();
    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;

    Cost ask();

private:
    struct Counters;
    std::unique_ptr<Counters> m_counters;
    std::uint64_t m_lastProcessorTime = 0;
    std::int64_t m_lastAskedUs = 0;
};

} // namespace qkvm::bench
