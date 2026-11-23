#include "server_feeder.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <utility>

#include <windows.h>

#include <qkvm/base/clock.h>
#include <qkvm/base/screen_changes.h>

#include "aspeed_frames.h"

namespace qkvm::bench {

namespace {

using bmc::aspeed::frames::Coefficients;
using bmc::aspeed::frames::Frame;
using bmc::aspeed::frames::Numbers;

// One plane of a JPEG block: its first coefficient up to `most` either way, and each of the others one in a hundred
// times `busy` (as the tests' busy screens draw them).
Coefficients somePlane(Numbers& below, int most, int busy)
{
    Coefficients found{};
    found[0] = below(2 * most + 1) - most;
    for (std::size_t position = 1; position < found.size(); ++position) {
        if (below(100) < busy) {
            found[position] = below(61) - 30;
        }
    }
    return found;
}

// Every block of the screen a JPEG block, nothing the same as in the frame before.
Frame aBusyScreen(int width, int height, int seed)
{
    Numbers below(static_cast<std::uint64_t>(seed));
    Frame frame(width, height);
    const int blocks = (frame.header().gridWidth / 8) * (frame.header().gridHeight / 8);
    for (int block = 0; block < blocks; ++block) {
        const Coefficients y = somePlane(below, 80, 30);
        const Coefficients cb = somePlane(below, 40, 12);
        const Coefficients cr = somePlane(below, 40, 12);
        frame.jpeg(y, cb, cr);
    }
    return frame;
}

// A clock of six blocks at the top right of a text console's screen (aspeed_frames.h's consoleScreen), at its
// `tick`: light strokes on dark, each block in its place.
Frame aClock(int width, int height, int tick)
{
    constexpr std::uint32_t dark = 0x108080;
    constexpr std::uint32_t ink = 0xD08080;
    Numbers below(static_cast<std::uint64_t>(tick) + 1000);
    Frame frame(width, height);
    const int right = width / 8 - 8;
    for (int place = 0; place < 6; ++place) {
        std::vector<int> pixels;
        pixels.reserve(64);
        for (int pixel = 0; pixel < 64; ++pixel) {
            const int row = pixel / 8;
            const int column = pixel % 8;
            pixels.push_back(row >= 1 && row <= 6 && column < 6 ? below(2) : 0);
        }
        frame.colours({{0, dark}, {2, ink}}, pixels, std::pair{right + place, 1});
    }
    return frame;
}

double towards(double mean, double value) noexcept
{
    return mean == 0 ? value : mean + (value - mean) / 10.0;
}

} // namespace

bool serverScreenKind(std::string_view name, ServerScreenKind& kind)
{
    if (name == "busy-1080p") {
        kind = ServerScreenKind::Busy1080;
    } else if (name == "busy-2048") {
        kind = ServerScreenKind::Busy2048;
    } else if (name == "clock-1080p") {
        kind = ServerScreenKind::Clock1080;
    } else {
        return false;
    }
    return true;
}

ServerFeeder::ServerFeeder(ServerScreenKind kind, double perSecond, engine::Stream& stream)
    : m_stream(stream), m_perSecond(perSecond)
{
    const auto made = [](const Frame& frame) { return Made{frame.header(), frame.data()}; };
    if (kind == ServerScreenKind::Clock1080) {
        m_width = 1920;
        m_height = 1080;
        m_first = made(bmc::aspeed::frames::consoleScreen(m_width, m_height, 40, 7));
        for (int tick = 0; tick < 10; ++tick) {
            m_after.push_back(made(aClock(m_width, m_height, tick)));
        }
    } else {
        m_width = kind == ServerScreenKind::Busy1080 ? 1920 : 2048;
        m_height = kind == ServerScreenKind::Busy1080 ? 1080 : 2048;
        m_first = made(aBusyScreen(m_width, m_height, 1));
        for (int seed = 2; seed < 8; ++seed) {
            m_after.push_back(made(aBusyScreen(m_width, m_height, seed)));
        }
    }
    // A timer that keeps to tenths of a millisecond, as the other feeder's.
    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    m_thread = std::thread([this] { run(); });
    SetThreadDescription(m_thread.native_handle(), L"qkvm console");
}

ServerFeeder::~ServerFeeder()
{
    m_stop.store(true);
    m_thread.join();
    if (m_timer != nullptr) {
        CloseHandle(m_timer);
    }
}

ServerCounts ServerFeeder::counts() const
{
    const std::lock_guard guard(m_countsLock);
    return m_counts;
}

void ServerFeeder::run()
{
    const auto every = static_cast<std::int64_t>(1'000'000.0 / m_perSecond);
    std::int64_t due = base::steadyNowUs();
    std::size_t next = 0;
    bool first = true;
    while (!m_stop.load()) {
        if (const std::int64_t now = base::steadyNowUs(); due > now) {
            wait(due - now);
            continue;
        }
        const Made& frame = first ? m_first : m_after[next];
        if (!first) {
            next = (next + 1) % m_after.size();
        }
        // Taken off the console's line: what the engine counts as its arrival.
        const std::int64_t arrived = base::steadyNowUs();
        const bool resized = m_screen.resizedBy(frame.header);
        const base::Result<int> read = m_screen.read(frame.header, frame.data);
        const std::int64_t readDone = base::steadyNowUs();
        if (!read) {
            const std::lock_guard guard(m_countsLock);
            ++m_counts.refused;
            due += every;
            continue;
        }
        // As ServerLive hands it on: the whole screen first, after a new size, and when the engine wishes for it;
        // otherwise what the frame changed (nothing for a frame that changed nothing).
        const bmc::aspeed::Picture& picture = m_screen.picture();
        const base::screen::Rect all{0, 0, picture.width, picture.height};
        bool whole = first || resized || m_stream.takeKeyFrameWish();
        std::vector<base::screen::Rect> rects;
        if (!whole) {
            for (const bmc::aspeed::Rect& rect : m_screen.changed()) {
                rects.push_back({rect.x, rect.y, rect.width, rect.height});
            }
            whole = rects.size() > static_cast<std::size_t>(base::screen::mostRects) ||
                    (rects.size() == 1 && rects.front() == all);
        }
        std::vector<std::byte> packet;
        if (whole) {
            packet = base::screen::whole(picture.width, picture.height, picture.rgb);
        } else if (!rects.empty()) {
            packet = base::screen::write(picture.width, picture.height, picture.rgb, rects);
        }
        const std::int64_t packed = base::steadyNowUs();
        if (!packet.empty()) {
            m_stream.push({packet, arrived, whole});
        }
        first = false;
        {
            const std::lock_guard guard(m_countsLock);
            ++m_counts.frames;
            m_counts.frameBytes += frame.data.size();
            m_counts.packetBytes += packet.size();
            m_counts.wholes += whole && !packet.empty() ? 1 : 0;
            m_counts.readMs = towards(m_counts.readMs, static_cast<double>(readDone - arrived) / 1000.0);
            m_counts.packMs = towards(m_counts.packMs, static_cast<double>(packed - readDone) / 1000.0);
        }
        due += every;
        if (const std::int64_t now = base::steadyNowUs(); due < now) {
            // Read later than the next was due: the controller waited meanwhile, and the next comes now.
            const std::lock_guard guard(m_countsLock);
            ++m_counts.behind;
            due = now;
        }
    }
}

void ServerFeeder::wait(std::int64_t microseconds) const
{
    if (m_timer == nullptr) {
        std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
        return;
    }
    LARGE_INTEGER due{};
    due.QuadPart = -microseconds * 10; // negative: from now, in tenths of a microsecond
    SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(m_timer, 1000);
}

} // namespace qkvm::bench
