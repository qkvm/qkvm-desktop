#pragma once

#include "clips.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <rtc/rtc.h>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

namespace qkvm::core::testing {
// A film's pictures sent on a track of a far end's peer from a thread of their own, each at its time,
// with an RTP time that steps by one picture's length on a 90 kHz clock: so that the Qt loop of the
// test, stalled on purpose, does not stall the sending too, as it does where a far end sends from that
// loop (JanusByHand by itself). What is sent, and when, is counted. Nothing of this program's is
// called from here: it is the far end.
//
// Paced by a timer that keeps to tenths of a millisecond, as the measuring rig's feeder is
// (bench/feeder.cpp): Windows' ordinary waits run long by up to its timer's 15.6 ms, and a sender
// late by that much sends two pictures at once. A picture is sent only while the track is open and
// `sending` is set; one more than a picture's length late is not made up for (the next goes at its
// own time, not in a burst).
class FilmSender {
public:
    FilmSender() = default;
    ~FilmSender() { stop(); }
    FilmSender(const FilmSender&) = delete;
    FilmSender& operator=(const FilmSender&) = delete;
    FilmSender(FilmSender&&) = delete;
    FilmSender& operator=(FilmSender&&) = delete;

    // The film, read and cut by the caller (bench::readFile, bench::cutIntoPictures). Kept as given.
    void film(std::vector<std::byte> bytes, std::vector<bench::Cut> cuts)
    {
        stop();
        m_bytes = std::move(bytes);
        m_cuts = std::move(cuts);
    }
    // Begins sending on `track`, a picture every `every`; whatever was sending stops first.
    void start(int track, std::chrono::microseconds every)
    {
        stop();
        if (m_cuts.empty() || track < 0 || every.count() <= 0) {
            return;
        }
        m_stopping = false;
        m_thread = std::thread([this, track, every] { run(track, every); });
    }
    // Stops sending and waits for the thread: the track may be deleted after this.
    void stop()
    {
        m_stopping = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    bool running() const { return m_thread.joinable(); }
    // When each picture was handed to the library, on this PC's steady clock, in microseconds.
    std::vector<std::int64_t> sentAt() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_sentAt;
    }

    std::atomic<bool> sending{true};
    std::atomic<int> sent{0};
    std::atomic<bool> failed{false};
    // The next picture sent is the film's next key frame (a far end that answers a request for one).
    std::atomic<bool> keyFrameNext{false};

private:
    static std::int64_t nowUs()
    {
        using namespace std::chrono;
        return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    }
    // Until `dueUs`, in waits of 20 ms at most, so that a stop is seen soon. False once stopping.
    bool waitUntil(HANDLE timer, std::int64_t dueUs) const
    {
        for (;;) {
            if (m_stopping) {
                return false;
            }
            const std::int64_t left = dueUs - nowUs();
            if (left <= 0) {
                return true;
            }
            const std::int64_t slice = left < 20'000 ? left : 20'000;
            if (timer == nullptr) {
                std::this_thread::sleep_for(std::chrono::microseconds(slice));
                continue;
            }
            LARGE_INTEGER due{};
            due.QuadPart = -slice * 10; // negative: from now, in tenths of a microsecond
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
        }
    }
    void run(int track, std::chrono::microseconds every)
    {
        HANDLE timer =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const std::int64_t everyUs = every.count();
        const auto step = static_cast<std::uint32_t>(90'000LL * everyUs / 1'000'000);
        std::size_t index = 0;
        std::int64_t dueUs = nowUs();
        for (;;) {
            dueUs += everyUs;
            if (!waitUntil(timer, dueUs)) {
                break;
            }
            if (nowUs() - dueUs > everyUs) {
                dueUs = nowUs();
            }
            if (!sending || !rtcIsOpen(track)) {
                continue;
            }
            if (keyFrameNext.exchange(false)) {
                for (std::size_t looked = 0; looked < m_cuts.size() && !m_cuts[index % m_cuts.size()].keyFrame;
                     ++looked) {
                    ++index;
                }
            }
            const bench::Cut cut = m_cuts[index % m_cuts.size()];
            ++index;
            rtcSetTrackRtpTimestamp(track, static_cast<std::uint32_t>(index) * step);
            std::vector<char> picture(cut.size);
            std::memcpy(picture.data(), m_bytes.data() + cut.from, cut.size);
            const std::int64_t at = nowUs();
            if (rtcSendMessage(track, picture.data(), static_cast<int>(picture.size())) < 0) {
                failed = true;
                continue;
            }
            ++sent;
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_sentAt.push_back(at);
        }
        if (timer != nullptr) {
            CloseHandle(timer);
        }
    }

    std::vector<std::byte> m_bytes;
    std::vector<bench::Cut> m_cuts;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_stopping{false};
    std::vector<std::int64_t> m_sentAt;
};
} // namespace qkvm::core::testing
