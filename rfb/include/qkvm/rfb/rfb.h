#pragma once

#include <qkvm/base/result.h>
#include <qkvm/rfb/inflate.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The RFB protocol, which VNC speaks: a server's screen kept as pixels, and keys and a pointer said to it. As the web's
// app/qkvm/rfb.py, which was typed from RFC 6143 and IANA's registry of its numbers, the Tight encoding from TightVNC's
// own description of it, and the keys from X11's list of keysyms and the USB HID usage tables.
//
// A `Client` is one connection, from the server's first word on, without the connection itself: whoever holds the
// line hands it what came (`feed`, in pieces of any size) and sends what it has to say (`takeOutgoing`, and the
// messages below). It speaks the handshake with the ways of signing in the RFC has (none, and VNC authentication,
// whose answer whoever holds the password makes), one pixel format of this program's choosing (or RGB565 for a server
// that takes no other), and the encodings it reads (Raw, CopyRect, Hextile, ZRLE, Tight with JPEG, and a change of
// the screen's size). What it keeps is the screen, three bytes a pixel, and what the updates changed of it.
//
// Nothing here trusts the server. What it says the screen is, is held to a size; every length it sends is checked
// against that screen before any room is taken for it; a rectangle outside the screen, a stream that inflates past
// what its rectangle can hold, a message of a kind this program does not read, more in a second than a server has
// reason to send: the connection is over, with a named error, never with a guess at what was meant. What is kept of a
// message not yet whole is that message's alone, and no more than its own bounds. Whoever holds the line keeps the
// times (a handshake that takes too long, a message begun and not ended: `messageSince`). Plain C++ on the standard
// library alone; one thread at a time uses a client.
namespace qkvm::rfb {

using Bytes = std::vector<std::uint8_t>;

// Ways of signing in.
inline constexpr std::uint8_t securityInvalid = 0;
inline constexpr std::uint8_t securityNone = 1;
inline constexpr std::uint8_t securityVnc = 2;

// Encodings, and the pseudo-encodings that ask for something other than pixels.
inline constexpr std::int32_t raw = 0;
inline constexpr std::int32_t copyRect = 1;
inline constexpr std::int32_t hextile = 5;
inline constexpr std::int32_t tight = 7;
inline constexpr std::int32_t zrle = 16;
inline constexpr std::int32_t desktopSize = -223;
// -32 is the lowest quality, -23 the highest: Tight sends JPEG only where one is asked for (and OpenBMC's always).
inline constexpr std::int32_t jpegQuality = -24;
// What a server is told this program reads, in order of preference (the web's ENCODINGS).
inline constexpr std::array<std::int32_t, 7> allEncodings{tight, zrle,        hextile,    copyRect,
                                                          raw,   desktopSize, jpegQuality};

// What the server is held to (the web's).
inline constexpr int largestSide = 8192;        // pixels a screen may be wide or tall
inline constexpr int mostPixels = 16'000'000;   // ...and have in all: three bytes each, kept in memory
inline constexpr std::size_t mostName = 1024;   // bytes of the name a server gives itself
inline constexpr std::size_t mostReason = 1024; // ...and of the reason it gives for a refusal
inline constexpr std::size_t mostCutText = std::size_t{1024} * 1024; // clipboard a server may send (read, let go of)
inline constexpr int tightWidest = 2048; // a Tight rectangle may be no wider, by its own description
inline constexpr int mostMessagesASecond = 2000;
inline constexpr int mostRectanglesASecond = 50'000; // a Hextile rectangle counts its tiles, eight to a rectangle
inline constexpr int mostResizesASecond = 10;
inline constexpr int mostScreensAnUpdate = 4; // pixels drawn by one update, in screens
inline constexpr int mostScreensASecond = 16; // ...and in one second, weighted by what each kind costs to draw...
inline constexpr std::int64_t leastWorkASecond = 8'000'000; // ...and never less than this many
inline constexpr std::size_t zlibSlack = 4096;              // bytes a zlib stream may be longer than what it holds
// Deflate blocks the zlib streams of a second's rectangles may begin, all of them together, and those of one
// rectangle's stream: one for every 64 bytes it may inflate to, and 64 more (outside review 2.1). zlib's deflate ends a
// block every 16,383 codes (some 127 at its smallest memory), and a flush adds one or two a rectangle: a server whose
// rectangles come at the most a second (mostRectanglesASecond) is far within both.
inline constexpr std::int64_t mostBlocksASecond = 200'000;
inline constexpr std::size_t bytesABlock = 64;
inline constexpr std::uint64_t blocksARectangle = 64;
// The room kept for what is fed, once no larger piece is under way: a Tight piece's most (its lengths have 22 bits).
inline constexpr std::size_t mostKeptRoom = std::size_t{4} * 1024 * 1024;
// What the updates changed, as rectangles, before they are taken: past this many, the whole screen.
inline constexpr std::size_t mostChanged = 4096;

// Ways a connection ends that whoever holds it turns into what is said (rfb.py's PROTOCOL to LIMIT).
inline constexpr std::string_view protocol = "protocol";       // what arrived is not RFB as this program reads it
inline constexpr std::string_view refused = "refused";         // the server said no, with or without a reason
inline constexpr std::string_view auth = "auth";               // it rejected the password
inline constexpr std::string_view unsupported = "unsupported"; // it speaks what this program does not
inline constexpr std::string_view limit = "limit";             // it sends more than this program takes
// ...and that whoever holds the line says: the server asks for a password, and none is held for it (nothing is sent).
inline constexpr std::string_view noPassword = "no_password";
// ...and that whoever holds the line asked for (Options::stopped): it let go of the line, and nothing more is read.
inline constexpr std::string_view stopped = "stopped";

enum class Pixels {
    Rgb888, // 32 bits a pixel, 24 of them colour, little-endian, red in the lowest byte: what every server is asked for
    Rgb565, // 16 bits, red in the top five: for a server that takes no other, with Raw and CopyRect only
};

// A part of the screen, in pixels from its top left.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const Rect&) const = default;
};

// A JPEG (Tight's) as three bytes a pixel, `width` by `height`; an error where it is not a picture of that size.
using JpegDecoder = std::function<base::Result<Bytes>(std::span<const std::uint8_t> jpeg, int width, int height)>;

struct Options {
    // What the server is told this program reads (Raw always among them: every server may fall back on it). A Tight
    // JPEG is read only with a decoder: without one, JPEG is not asked for, and a server that sends one ends the
    // connection ("protocol").
    std::vector<std::int32_t> encodings{allEncodings.begin(), allEncodings.end()};
    Pixels pixels = Pixels::Rgb888;
    // The ways of signing in a family's server may ask for (VNC authentication only where a password is there).
    std::vector<std::uint8_t> signsIn{securityNone, securityVnc};
    // Greetings beyond version 3 that a family's server sends and means 3.8 by ("RFB 004.000\n", Intel AMT's KVM).
    std::vector<std::string> greetedAs38;
    JpegDecoder jpeg;
    // A monotonic wall clock in milliseconds; empty: a steady clock of this program's. A test gives its own.
    std::function<std::int64_t()> clock;
    // The reader's own processor time in milliseconds (how long it has actually worked, not how long the wall says):
    // empty falls back to `clock`. The host gives the running thread's CPU time, so a thread kept off the core by other
    // work is not charged for the wall it lost. A test gives its own.
    std::function<std::int64_t()> workClock;
    // A share of a drawing thread, as the web holds each server to (DRAWING_SHARE, DRAWING_BURST, LONG_STEP). The time
    // the reader spends drawing is charged against this share (by the clock); a server over its share is shown more
    // slowly (the screen holds its line back until the share earns the debt back, pausedForMs), not refused. Only a
    // server whose single steps take LONG_STEP of a drawing thread's own processor time, more than MOST_LONG_STEPS
    // times within LONG_STEPS_WITHIN, is refused ("the server's pictures take far too long to draw"), on the ladder.
    // longStepMs negative turns the whole share off (a test's wish). And one rectangle's zlib stream that has taken
    // longStepMs of the thread's own processor time is cut off there, the server at fault (outside review 2.1: the
    // stream cannot be read on later, so it ends the connection at once, where the web counts such a step as one of its
    // long steps).
    std::int64_t sharePerMil = 500;         // milliseconds of a drawing thread a second its pictures may take, per 1000
    std::int64_t burstMs = 1000;            // ...and at once, after taking less for a while (a first screen)
    std::int64_t countedFromMs = 50;        // a step whose clock hop is this long is charged its processor time, not...
    std::int64_t longStepMs = 2000;         // ...the clock (short steps read nought of a core); a step this long...
    int mostLongSteps = 3;                  // ...of a core more than this many times...
    std::int64_t longStepsWithinMs = 60000; // ...within this long refuses the server
    // Asked wherever the share is (each costly step: rows, tiles, a JPEG, and as the inflater asks): true, and whoever
    // holds the line has let go of it (its screen closed, its session over), so the read stops where it is
    // ("stopped"), and no work goes on for nobody (outside review 2.1). On the reading thread: what it reads, another
    // thread may set.
    std::function<bool()> stopped;
};

// The keysym for a key by its USB HID usage, as the key is without Shift (and with it) on the US layout where the key
// has a character; none for a key this program has no keysym for.
std::optional<std::uint32_t> keysymFor(int usage, bool shift = false);
// The keysym that types `character`: Latin-1 its own code, the rest Unicode keysyms; none for a control character.
std::optional<std::uint32_t> keysymOf(char32_t character);
inline constexpr std::uint32_t unicodeKeysym = 0x01000000;

// VNC authentication's DES key of a password (rfb.py's vnc_key): its first eight bytes (UTF-8), noughts after a shorter
// one, each byte with its bits the other way round. The RFC leaves the mirroring out (its erratum 4951 puts it in);
// every server does it, from the first one on, in a DES whose key schedule read the bits from the other end. What is
// longer than eight bytes is cut, by every server: the rest is not checked. Written into `key`, the caller's room,
// which the caller writes over once the key is used; nothing else is made of the password here, and nothing is kept.
// The DES itself is whoever holds the password's (Windows' own, in core: vnc_auth.h), as `answer` takes it.
void vncKey(std::span<const std::uint8_t> password, std::span<std::uint8_t, 8> key) noexcept;

// What this program says, as it goes on the wire.
Bytes updateRequest(bool incremental, int x, int y, int width, int height);
Bytes keyEvent(std::uint32_t keysym, bool down);
// The pointer at a place of the screen, with these buttons held: bit 0 the left, 1 the middle, 2 the right, 3 and 4
// the wheel up and down, 5 and 6 left and right.
Bytes pointerEvent(int x, int y, std::uint8_t buttons);

class Client {
public:
    explicit Client(Options options);

    // What came from the server, read as far as it goes. Ok, or why the connection is over (one of the codes above);
    // after that the client takes nothing more.
    base::Result<void> feed(std::span<const std::uint8_t> data, std::int64_t nowMs);
    // What the handshake has to say to the server now, taken (and so said once).
    Bytes takeOutgoing();
    // VNC authentication: the sixteen bytes the server asks to have answered, while it waits for the answer...
    std::optional<std::array<std::uint8_t, 16>> challenge() const;
    // ...and the answer (the DES of them under the password's key, made by whoever holds the password).
    base::Result<void> answer(std::span<const std::uint8_t> response, std::int64_t nowMs);

    // The handshake is over: updates may be asked for, and keys and the pointer said.
    bool ready() const noexcept { return m_stage == Stage::Ready; }
    // The connection is over (feed said why).
    bool over() const noexcept { return m_stage == Stage::Over; }
    int minor() const noexcept { return m_minor; } // the version spoken: 3.3, 3.7 or 3.8 (0 before)
    std::uint8_t security() const noexcept { return m_security; }
    const std::string& name() const noexcept { return m_name; }
    // The screen as it stands: three bytes a pixel, row after row (0 by 0 before the server said its size).
    int width() const noexcept { return m_width; }
    int height() const noexcept { return m_height; }
    const Bytes& rgb() const noexcept { return m_rgb; }
    // Updates that arrived, and those of them that drew something.
    std::uint64_t updates() const noexcept { return m_updates; }
    std::uint64_t frames() const noexcept { return m_frames; }
    // What the updates that arrived changed since the last take, in their order (the whole screen where it changed its
    // size, or where they changed more rectangles than mostChanged); and whether the size changed meanwhile.
    struct Changed {
        std::vector<Rect> rects;
        bool resized = false;
    };
    Changed take();
    // When the message under way began (by the clock `feed` is given), where one has begun and not ended.
    std::optional<std::int64_t> messageSince() const noexcept { return m_since; }
    // What is kept of what was fed, read or not (less than a megabyte of it read, after a feed), and the room kept for
    // it: at the most mostKeptRoom once no larger piece is under way.
    std::size_t keptBytes() const noexcept { return m_held.size(); }
    std::size_t keptRoom() const noexcept { return m_held.capacity(); }
    // ...and the room kept for what a rectangle's stream inflated to: given back the same way (round-6 review LOW 3).
    std::size_t inflatedRoom() const noexcept { return m_plain.capacity(); }
    // Bytes kept for the message under way.
    std::size_t held() const noexcept { return m_held.size() - m_at; }
    // Over its drawing share: how long (ms) whoever reads should hold the server's line back and ask for nothing, so
    // the server is shown more slowly rather than refused; 0 when it is within its share. Brings the share up to date.
    std::int64_t pausedForMs();

    // An update of the whole screen asked for: what changed, or (`incremental` false) all of it.
    Bytes request(bool incremental) const;
    // The pointer, at a place held to the screen.
    Bytes pointer(int x, int y, std::uint8_t buttons) const;

private:
    enum class Stage {
        Version,
        SecurityType,  // 3.3: the server chose
        SecurityCount, // 3.7 and 3.8: it offers
        SecurityList,
        Challenge, // VNC authentication: the answer is waited for
        SecurityResult,
        Reason, // a no's reason, after which the connection is over
        Init,
        Name,
        Ready,
        Over,
    };
    // What a message of a connection that is ready is at.
    enum class At {
        Kind,
        Update, // the head of an update
        Rect,   // the head of one of its rectangles
        RawRows,
        CopyRect,
        Hextile,
        ZrleLength,
        ZrleData,
        Tight,
        ColourMap,
        CutText,
        Drop, // bytes read and let go of
    };

    base::Result<void> run();
    base::Result<void> step(bool& more);
    base::Result<void> handshake(bool& more);
    base::Result<void> message(bool& more);
    base::Result<void> fail(std::string_view code, std::string message);
    base::Result<void> rectangle(bool& more);
    base::Result<void> rawRows(bool& more);
    base::Result<void> hextileTile(bool& more);
    base::Result<void> zrleData(bool& more);
    base::Result<void> tightRect(bool& more);
    base::Result<void> rectDone();
    base::Result<void> count(int messages, int rectangles, int resizes, std::int64_t work, std::int64_t blocks = 0);
    // The second being counted, brought up to now (a new one where the last is over).
    void countFromNow();
    // What a rectangle's zlib stream is read within (outside review 2.1): blocks, by its room (`room`, what it may
    // inflate to) and by what the second has left; table work, by what the second's work has left; and the share's
    // asking, with the cut-off of a stream that has taken a long step of the thread's own time.
    Inflater::Bounds boundsFor(std::size_t room);
    // How a read that took too long ends: the server at fault, or stopped where whoever holds the line let go of it.
    base::Result<void> tooSlowToDraw();
    bool stoppedNow() const;
    base::Result<void> fit(int width, int height);

    // The drawing share: the wall and processor clocks now; `overBudget`, asked at each step (the inflater before every
    // dynamic block and every so many others, the tile and row loops, after a JPEG), and first of all whether whoever
    // holds the line has let go of it (Options::stopped), which charges the drawing since it was last asked against the
    // share and says whether the server has taken too many long steps (so the long loops and the inflater stop and the
    // connection ends, the server at fault); and `pausedForMs`, how long the screen should hold the line back and ask
    // for nothing because the server is over its share (0 when it is not). Not const: they keep the share.
    std::int64_t clockNow() const;
    std::int64_t workNow() const;
    // What a step of `drawnWall` by the clock and `workedThread` of a core is charged to the share (countedFromMs).
    std::int64_t shareCharge(std::int64_t drawnWall, std::int64_t workedThread) const;
    bool overBudget();
    std::span<const std::uint8_t> there() const noexcept;
    void consume(std::size_t bytes) noexcept;
    void say(std::span<const std::uint8_t> bytes);
    void changed(int x, int y, int width, int height);
    void blit(int x, int y, int width, int height, std::span<const std::uint8_t> rgb);
    void fill(int x, int y, int width, int height, std::span<const std::uint8_t> pixel);

    Options m_options;
    std::vector<std::int32_t> m_said; // the encodings the server is told of
    Stage m_stage = Stage::Version;
    std::string m_why; // the code a reason that is being read ends the connection with
    int m_minor = 0;
    std::uint8_t m_security = 0;
    std::size_t m_wanted = 0; // the length of what is being read (a reason, a name, a security list)
    std::optional<std::array<std::uint8_t, 16>> m_challenge;
    std::string m_name;
    int m_width = 0;
    int m_height = 0;
    Bytes m_rgb;
    Bytes m_out;
    // What came and has not been read, from m_at on (round-5 review LOW 3). At the most: the one piece read whole
    // before it is drawn, which is a ZRLE rectangle's tile stream (four bytes a pixel and 64 a tile, as the web holds
    // it: a run of one pixel is four bytes, so a legal stream may be that long; some 64 MB at mostPixels), any other
    // piece being at the most 4 MB (Tight's lengths have 22 bits); less than a megabyte of what was read before it
    // (let go of as each batch ends: run); and the one feed in flight (a line message, 1 MB). Not counted in the
    // screen's hold-back, which would
    // hold the line on a piece not yet whole and still the server's deadlines; its room is given back once a piece
    // larger than mostKeptRoom is done (run).
    Bytes m_held;
    std::size_t m_at = 0;
    std::int64_t m_now = 0;
    std::optional<std::int64_t> m_since;
    // The drawing share (overBudget, pausedForMs): credit in ms (up to burstMs), when it was last brought up to date
    // (wall, spans held-back time), the wall and processor clocks at the last step (both reset each batch so a
    // held-back gap and other readers' time are not charged), when long steps happened (within longStepsWithinMs), and
    // whether the server has been refused. `m_shareStarted`: the credit is set to a burst at the first batch.
    bool m_shareStarted = false;
    bool m_refused = false;
    std::int64_t m_credit = 0;
    std::int64_t m_creditAt = 0; // wall, when the credit was last earned up to (not reset per batch)
    std::int64_t m_lastWall = 0; // wall at the last step (reset each batch: only drawing is charged)
    std::int64_t m_lastWork = 0; // processor time at the last step (reset each batch)
    std::vector<std::int64_t> m_longStepsAt;
    // The message under way.
    At m_part = At::Kind;
    int m_rectsLeft = 0;
    bool m_drew = false;
    std::int64_t m_updatePixels = 0;
    Rect m_rect;
    int m_rowsDone = 0;
    int m_tile = 0; // the next Hextile tile of the rectangle
    std::array<std::uint8_t, 3> m_background{};
    std::array<std::uint8_t, 3> m_foreground{};
    std::size_t m_dataLength = 0; // ZRLE's stream for the rectangle
    std::size_t m_dropLeft = 0;
    // The streams: ZRLE's, and Tight's four.
    Inflater m_zrle;
    std::array<Inflater, 4> m_tight;
    Bytes m_plain; // what a stream inflated to, for the rectangle being drawn
    // The second being counted, and what came in it.
    std::int64_t m_second = -1;
    int m_messages = 0;
    int m_rectangles = 0;
    int m_resizes = 0;
    std::int64_t m_work = 0;
    std::int64_t m_blocks = 0;
    // Whoever holds the line let go of it (Options::stopped), seen.
    bool m_stopped = false;
    std::uint64_t m_updates = 0;
    std::uint64_t m_frames = 0;
    Changed m_changed;
};

} // namespace qkvm::rfb
