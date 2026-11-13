#include "feeder.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <span>
#include <string_view>

#include <windows.h>

#include <qkvm/base/clock.h>

namespace qkvm::bench {

namespace {

// "pattern-1080p30.h264" plays at 30 a second: the number between the last "p" and the dot.
double perSecondOf(const std::string& name)
{
    const std::size_t dot = name.rfind('.');
    const std::size_t p = name.rfind('p', dot);
    int rate = 0;
    if (dot == std::string::npos || p == std::string::npos) {
        return 30;
    }
    const std::string_view digits = std::string_view(name).substr(p + 1, dot - p - 1);
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), rate);
    return error == std::errc{} && end == digits.data() + digits.size() && rate > 0 ? rate : 30;
}

} // namespace

base::Result<Clip> readClip(const std::filesystem::path& data, const std::string& name)
{
    Clip clip;
    const std::filesystem::path path = data / name;
    if (std::filesystem::is_directory(path)) {
        for (const std::filesystem::path& file : jpegsIn(path)) {
            base::Result<std::vector<std::byte>> bytes = readFile(file);
            if (!bytes) {
                return bytes.error();
            }
            clip.jpegs.push_back(std::move(bytes).value());
        }
    } else {
        base::Result<std::vector<std::byte>> bytes = readFile(path);
        if (!bytes) {
            return bytes.error();
        }
        clip.codec = engine::Codec::H264;
        clip.perSecond = perSecondOf(name);
        clip.bytes = std::move(bytes).value();
        clip.cuts = cutIntoPictures(clip.bytes);
    }
    if (clip.pictures() == 0) {
        return base::Error{"not_found", path.string() + " has no pictures in it. tools\\make_testdata.ps1 makes them."};
    }
    return clip;
}

Feeder::Feeder(std::vector<Player> players) : m_players(std::move(players))
{
    // A timer that keeps to tenths of a millisecond: Windows' ordinary sleep is too coarse to pace
    // sixty pictures a second.
    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    m_thread = std::thread([this] { run(); });
    SetThreadDescription(m_thread.native_handle(), L"qkvm feeder");
}

Feeder::~Feeder()
{
    m_stop.store(true);
    m_thread.join();
    if (m_timer != nullptr) {
        CloseHandle(m_timer);
    }
}

void Feeder::run()
{
    // Spread over their first interval, so that sixty-four streams do not all come due at once.
    const std::int64_t start = base::steadyNowUs();
    for (std::size_t index = 0; index < m_players.size(); ++index) {
        Player& player = m_players[index];
        player.everyUs = static_cast<std::int64_t>(1'000'000.0 / player.clip->perSecond);
        player.dueUs =
            start + player.everyUs * static_cast<std::int64_t>(index) / static_cast<std::int64_t>(m_players.size());
    }
    while (!m_stop.load()) {
        const std::int64_t now = base::steadyNowUs();
        std::int64_t soonest = now + 50'000;
        for (Player& player : m_players) {
            if (player.dueUs <= now) {
                push(player, now);
                player.dueUs += player.everyUs;
                if (player.dueUs < now - 1'000'000) {
                    player.dueUs = now; // far behind (the PC slept): go on from now
                }
            }
            soonest = std::min(soonest, player.dueUs);
        }
        wait(soonest - base::steadyNowUs());
    }
}

void Feeder::push(Player& player, std::int64_t now)
{
    const Clip& clip = *player.clip;
    if (clip.codec == engine::Codec::Still) {
        player.stream->push({clip.jpegs[player.next], now, true});
    } else {
        if (player.stream->takeKeyFrameWish()) {
            while (!clip.cuts[player.next].keyFrame) {
                player.next = (player.next + 1) % clip.cuts.size();
            }
        }
        const Cut& cut = clip.cuts[player.next];
        player.stream->push({std::span<const std::byte>(clip.bytes).subspan(cut.from, cut.size), now, cut.keyFrame});
    }
    player.next = (player.next + 1) % clip.pictures();
}

void Feeder::wait(std::int64_t microseconds) const
{
    if (microseconds <= 0) {
        return;
    }
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
