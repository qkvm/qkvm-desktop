#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <qkvm/bmc/aspeed.h>
#include <qkvm/engine/scene.h>

// A server's own screen as the program will show it (M7 3d), for the measuring rig: frames in ASPEED's video format,
// made here as a management controller's chip would send them (bmc/tests/aspeed_frames.h), read by the program's own
// decoder (bmc/) on a thread of their own, as a console's frames are, and handed to the engine as what changed of the
// screen (base/screen_changes.h, Codec::Rgb), the whole screen first and whenever the engine wishes for it. What the
// program's ServerScreen and ServerLive do, without Qt and without a console's line.
namespace qkvm::bench {

enum class ServerScreenKind {
    Busy1080, // 1920 by 1080, every block of every frame a JPEG block: a film, or a desktop that scrolls
    Busy2048, // the same at 2048 by 2048, the largest screen the decoder takes
    Clock1080 // 1920 by 1080, a text console that stands still but for a clock of six blocks at its top right
};

// The kind a name on the command line means ("busy-1080p", "busy-2048", "clock-1080p"); false where it is none.
bool serverScreenKind(std::string_view name, ServerScreenKind& kind);

// What the console's thread did, since it began.
struct ServerCounts {
    std::uint64_t frames = 0;      // frames read and handed on
    std::uint64_t frameBytes = 0;  // ...their bytes, as the controller sends them
    std::uint64_t packetBytes = 0; // what was handed to the engine
    std::uint64_t wholes = 0;      // packets that were the whole screen
    std::uint64_t refused = 0;     // frames the decoder refused (none, from frames made right)
    std::uint64_t behind = 0;      // frames that were due before the last was read
    double readMs = 0;             // reading a frame, a mean over about the last ten
    double packMs = 0;             // making its packet, the same
};

class ServerFeeder {
public:
    // Makes the frames (a few seconds for the busy ones), then hands them to `stream` at `perSecond`, as a console
    // sends them: never more than one waits, and a frame read late holds the next back (the controller waits while
    // its console is not read). The stream outlives this.
    ServerFeeder(ServerScreenKind kind, double perSecond, engine::Stream& stream);
    ~ServerFeeder();
    ServerFeeder(const ServerFeeder&) = delete;
    ServerFeeder& operator=(const ServerFeeder&) = delete;
    ServerFeeder(ServerFeeder&&) = delete;
    ServerFeeder& operator=(ServerFeeder&&) = delete;

    ServerCounts counts() const;
    int width() const noexcept { return m_width; }
    int height() const noexcept { return m_height; }

private:
    struct Made {
        bmc::aspeed::FrameHeader header;
        std::vector<std::uint8_t> data;
    };

    void run();
    void wait(std::int64_t microseconds) const;

    engine::Stream& m_stream;
    double m_perSecond;
    int m_width = 0;
    int m_height = 0;
    Made m_first;              // the screen, whole
    std::vector<Made> m_after; // then these, in turn, again and again
    bmc::aspeed::Screen m_screen;
    void* m_timer = nullptr;
    mutable std::mutex m_countsLock;
    ServerCounts m_counts;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace qkvm::bench
