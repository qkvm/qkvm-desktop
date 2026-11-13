#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <qkvm/base/result.h>
#include <qkvm/engine/scene.h>

#include "clips.h"

// The made pictures as streams: read once, and handed to the engine at the pace a network would
// hand them over. For the rigs; the program itself is fed by a server.
namespace qkvm::bench {

// One made file or folder, read once and shown by as many tiles as were asked for.
struct Clip {
    engine::Codec codec = engine::Codec::Still;
    double perSecond = 2; // a folder of JPEGs is two a second, as the wall's previews are
    std::vector<std::byte> bytes;
    std::vector<Cut> cuts;
    std::vector<std::vector<std::byte>> jpegs;

    std::size_t pictures() const { return codec == engine::Codec::H264 ? cuts.size() : jpegs.size(); }
};

// Reads `name` in the folder `data`: a file of H.264 ("pattern-1080p30.h264" plays at 30 a second,
// the number between the last "p" and the dot), or a folder of JPEGs.
base::Result<Clip> readClip(const std::filesystem::path& data, const std::string& name);

// One tile's stream and where it is in its clip.
struct Player {
    const Clip* clip = nullptr;
    engine::Stream* stream = nullptr;
    std::size_t next = 0;
    std::int64_t dueUs = 0;
    std::int64_t everyUs = 0;
};

// What the network thread of the program will do: hand each stream its pictures when they are due.
// A stream that wishes for a key frame gets one next, as a server would send it. The clips and the
// streams outlive it.
class Feeder {
public:
    explicit Feeder(std::vector<Player> players);
    ~Feeder();
    Feeder(const Feeder&) = delete;
    Feeder& operator=(const Feeder&) = delete;

private:
    void run();
    static void push(Player& player, std::int64_t now);
    void wait(std::int64_t microseconds) const;

    std::vector<Player> m_players;
    void* m_timer = nullptr;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace qkvm::bench
