#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QObject>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/bmc/aspeed.h>
#include <qkvm/core/wiped.h>
#include <qkvm/rfb/rfb.h>

namespace qkvm::core {

// One RFB session's screen, read off the thread its line is on (review 3's HIGH 1, as ScreenReader for AMI's console:
// what a server sends is inflated, its JPEGs decoded and its pixels drawn on a thread of Qt's pool, the program's own,
// one thread at a time for one session). RfbScreen's own (rfb_screen.h).
//
// * What came over the line is read in its order (feed); whoever feeds it holds the line back while more than it
//   allows waits here unread (waiting()).
// * What was read goes back through a picture of its own, the hand-over: the screen as the newest update left it, and
//   what changed of it since it was last taken. Where all of it changed (a server that sends every frame whole, as
//   OpenBMC's does), the screen is handed that picture itself, no copy on its thread; one whose thread was busy is
//   handed the screen as it stands with all that changed meanwhile, nothing of it lost.
// * What the handshake has to say, what the server said of itself, and how the connection ended (the reader's error),
//   go back the same way; after an end nothing more is read.
// * The card's still is made here too, in turn behind what was fed before it was asked for.
// * Detached (the session is over, or its screen goes), nothing more is read or told, and what is being read stops
//   where it is, at its next asking (between two dynamic blocks of a stream, two rows of tiles: outside review 2.1);
//   the reader outlives its screen while its thread is at work, and holds nothing of the screen's meanwhile.
//
// Every function but run() on the screen's thread.
class RfbReader : public std::enable_shared_from_this<RfbReader> {
public:
    struct Still {
        std::uint64_t number = 0;
        QByteArray picture; // empty: no screen to make it of
    };
    struct Taken {
        QByteArray outgoing;              // what the handshake has to say to the server, in its order
        std::optional<base::Error> ended; // the reader's end (rfb.h's codes)
        bool ready = false;               // the handshake is over
        int width = 0;                    // the screen, as the server says it now (0 before)
        int height = 0;
        QString name;                             // the name the server gives itself (as it came, made one short line)
        std::uint64_t updates = 0;                // updates that arrived, in all
        std::uint64_t frames = 0;                 // ...that drew something
        std::optional<std::int64_t> messageSince; // a message under way since then (steadyMs)
        std::int64_t pausedForMs = 0;             // over its share: hold the line back and ask nothing this long
        std::vector<bmc::aspeed::Rect> changed;
        std::vector<Still> stills;
        // The answer to VNC authentication's challenge is in `outgoing` (told once): the password went with it.
        bool passwordAnswered = false;
        int minor = 0; // the RFB version spoken, once the handshake is over
    };

    // `home`: the screen. `ready` is called as its own, on its thread, a turn of its loop after something was read or
    // made; never once `home` is gone, or detach() was said. `password`: what VNC authentication's challenge is
    // answered with, where the options let the server ask for it (vnc_auth.h, on the reader's thread); empty: a server
    // that asks is sent nothing, and the session ends ("no_password"). Let go of (written over) once it has answered,
    // or once the handshake is over or the session has ended without it.
    RfbReader(QObject* home, std::function<void()> ready, rfb::Options options, SecretText password = {});
    ~RfbReader();
    RfbReader(const RfbReader&) = delete;
    RfbReader& operator=(const RfbReader&) = delete;
    RfbReader(RfbReader&&) = delete;
    RfbReader& operator=(RfbReader&&) = delete;

    void feed(QByteArray bytes);
    void still(int asked, std::uint64_t number);
    // Bytes fed and not read yet.
    qint64 waiting() const noexcept { return m_waitingBytes.load(std::memory_order_relaxed); }
    // What was read and made since the last take, with `into` brought up to the screen as it stands (made anew where
    // its size is another).
    Taken take(bmc::aspeed::Picture& into);
    void detach();
    // Whether it still holds the password VNC authentication is answered with: from its making until it has answered,
    // or the handshake is over or the session has ended without that (for a test; on any thread).
    bool holdsAPassword() const noexcept { return m_holdsAPassword.load(std::memory_order_relaxed); }

    // The clock the reader is given (what messageSince is by).
    static std::int64_t steadyMs();

private:
    struct Item {
        QByteArray bytes;
        bool still = false;
        int asked = 0;
        std::uint64_t number = 0;
    };

    void wake();
    void run();
    void readOne(const QByteArray& bytes);
    // The challenge the server waits on answered with the password (or none, where none is held): `said`, once the
    // answer was made and handed to the client to go out, whatever reading on after it comes to.
    base::Result<void> answerTheChallenge(const std::array<std::uint8_t, 16>& challenge, bool& said);
    void letGoOfThePassword();
    void makeStill(const Item& item);
    void tell();

    std::mutex m_in;
    std::deque<Item> m_waiting;
    bool m_running = false;
    bool m_detached = false;
    std::atomic<qint64> m_waitingBytes = 0;
    // Detached: the read under way stops at its next asking (rfb::Options::stopped), so that no worker goes on reading
    // for nobody (outside review 2.1). Set on the screen's thread, read on the running one.
    std::atomic<bool> m_stopping = false;
    // The running thread's.
    rfb::Client m_client;
    bool m_over = false;
    SecretText m_password;
    std::atomic<bool> m_holdsAPassword = false;

    std::mutex m_out;
    QObject* m_home;
    std::function<void()> m_ready;
    bool m_told = false;
    Taken m_taken;
    bmc::aspeed::Picture m_handed;
    bool m_handedStale = false;   // what m_handed holds went to the screen: the next update goes over whole
    bool m_all = false;           // all of the screen is to be told as changed
    std::pair<int, int> m_theirs; // the size of the picture the screen holds, as it last took one
};

} // namespace qkvm::core
