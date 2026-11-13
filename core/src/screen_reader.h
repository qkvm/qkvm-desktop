#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QObject>

#include <qkvm/base/result.h>
#include <qkvm/bmc/ami_console.h>
#include <qkvm/bmc/aspeed.h>

namespace qkvm::core {

// A server's screen, read off the thread its console's line is on (review 3's HIGH 1, 2026-11-06: a busy 1080p frame
// took about 46 ms on a slow core in the build that is handed out, and the card's still read every pixel of the
// screen, both on the interface thread, and with them a JetKVM's keep-alive and every time of the console waited).
// ServerScreen's own (server_screen.h).
//
// * Frames are read in their order, a session's into one picture, on a thread of Qt's pool (the program's own,
//   QThreadPool::globalInstance, which the application waits for as it ends): one thread at a time for one screen, as
//   aspeed::Screen is used.
// * Whoever hands frames over bounds what waits here (ServerScreen holds its console's line back while two wait), and
//   no frame is thrown away unread: each brings only what changed since the one before.
// * What was read goes back through a picture of its own, the hand-over: the screen as the newest frame left it, and
//   what changed of it since it was last taken, row of eight pixels by row. Where all of it changed, the screen is
//   handed that picture itself (no copy on its thread), and a picture that goes over whole is copied before the lock
//   the screen takes with. A screen whose thread was busy is handed,
//   when it takes, the screen as it stands and all that changed since it last took it: the frames read meanwhile are
//   one picture to it (the older pictures are dropped for the newest, as the engine's streams drop them), and nothing
//   that changed is lost, so that what its live hands on stays whole and in order.
// * A picture made anew is up to 24 MiB: past `mostResizesASecond` a second, nothing more of that session is read, and
//   that is said (Read::resizedTooOften).
// * The card's still (base/screen_still.h: every pixel of the screen read) is made here too, in turn behind the frames
//   handed over before it was asked for.
// * A session that ends (end), or a screen that goes (detach), has the frame under way given up within 64 blocks
//   (aspeed::Screen::giveUp), nothing more of it read, its pictures let go of, and what comes of it after told to
//   nobody. The reader outlives its screen while its thread is still at a frame, and holds nothing of the screen's
//   meanwhile; what it tells, it tells as the screen's own (a turn of the screen's loop, never after the screen went).
//
// Every function but run() on the screen's thread.
class ScreenReader : public std::enable_shared_from_this<ScreenReader> {
public:
    // What came of one frame.
    struct Read {
        base::Result<int> changed = 0; // the blocks it changed, or why it was refused (what it drew before, it drew)
        bool resizedTooOften = false;  // refused unread: the screen changes its size more often than it may
    };
    // A still asked for (still()), by its number: empty where there was no picture of that session to make it of.
    struct Still {
        std::uint64_t number = 0;
        QByteArray picture;
    };
    // What was read since the last take: what came of each frame, in their order; the stills made; and what changed of
    // the picture, as rectangles (rows of eight pixels, those of the same span one under the other joined).
    struct Taken {
        std::vector<Read> reads;
        std::vector<Still> stills;
        std::vector<bmc::aspeed::Rect> changed;
    };

    // `home`: the screen. `ready` is called as its own, on its thread, a turn of its loop after something was read or
    // made; never once `home` is gone, or detach() was said.
    ScreenReader(QObject* home, std::function<void()> ready, int mostResizesASecond);
    ~ScreenReader();
    ScreenReader(const ScreenReader&) = delete;
    ScreenReader& operator=(const ScreenReader&) = delete;
    ScreenReader(ScreenReader&&) = delete;
    ScreenReader& operator=(ScreenReader&&) = delete;

    // A frame of session `session` (numbered from 1, higher with each session), to be read after the ones before it.
    void read(std::uint64_t session, bmc::ami::WholeFrame frame);
    // The still of session `session`'s screen for a card `asked` pixels wide, made once the frames handed over before
    // it have been read.
    void still(std::uint64_t session, int asked, std::uint64_t number);
    // Session `session`, and every one before it, is over.
    void end(std::uint64_t session);
    // What was read and made for session `session` since the last take, with `into` brought up to the screen as it
    // stands (made anew where its size is another). Nothing of an earlier session.
    Taken take(std::uint64_t session, bmc::aspeed::Picture& into);
    // The screen goes: nothing more is read, made or told.
    void detach();

private:
    struct Item {
        std::uint64_t session = 0;
        bool still = false;
        bmc::ami::WholeFrame frame;
        int asked = 0;
        std::uint64_t number = 0;
    };

    // With m_in held: a thread of the pool runs what waits, where none does.
    void wake();
    // On a thread of the pool, until nothing waits.
    void run();
    void readOne(Item& item);
    void makeStill(const Item& item);
    // What came of a frame, handed over with what it changed of `screen` (none: nothing of it changed).
    void handOver(std::uint64_t session, Read read, const bmc::aspeed::Screen* screen, bool anew);
    // With m_out held: the hand-over is of `session` from now on; and the screen is told, once until it takes.
    void handOverOf(std::uint64_t session);
    void tell();

    const int m_mostResizes;

    // What waits to be read, and whether a thread runs it: under m_in.
    std::mutex m_in;
    std::deque<Item> m_waiting;
    bool m_running = false;
    bool m_detached = false;
    std::uint64_t m_endedUpTo = 0;
    // What frames are read into, and of which session: the running thread's while it runs (replaced only under m_in),
    // and, while none runs, let go of under m_in. Given up (giveUp) under m_in.
    std::unique_ptr<bmc::aspeed::Screen> m_screen;
    std::uint64_t m_screenOf = 0;
    // (The running thread's: when the screen was last made anew in another size, and whether the session was left.)
    std::vector<std::int64_t> m_resized;
    bool m_refused = false;

    // What was read, for the screen to take: under m_out.
    std::mutex m_out;
    QObject* m_home;
    std::function<void()> m_ready;
    bool m_told = false;
    std::uint64_t m_endedOut = 0;
    std::uint64_t m_handedOf = 0;
    bmc::aspeed::Picture m_handed;
    // What is in m_handed is what the screen had: the newest went to it whole (take), and the next goes over whole.
    bool m_handedStale = false;
    // The size of the picture the screen holds, as it last took one (none: it holds none of this session's).
    std::pair<int, int> m_theirs;
    // For each row of eight pixels of the hand-over, the span that changed since it was taken (empty: from > to).
    std::vector<std::pair<int, int>> m_spans;
    std::vector<Read> m_reads;
    std::vector<Still> m_stills;
};

} // namespace qkvm::core
