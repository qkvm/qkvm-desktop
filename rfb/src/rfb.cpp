#include <qkvm/rfb/rfb.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace qkvm::rfb {

namespace {

// Client messages.
constexpr std::uint8_t setPixelFormatKind = 0;
constexpr std::uint8_t setEncodingsKind = 2;
constexpr std::uint8_t updateRequestKind = 3;
constexpr std::uint8_t keyEventKind = 4;
constexpr std::uint8_t pointerEventKind = 5;
// Server messages.
constexpr std::uint8_t updateKind = 0;
constexpr std::uint8_t colourMapKind = 1;
constexpr std::uint8_t bellKind = 2;
constexpr std::uint8_t cutTextKind = 3;

// Bytes of Raw pixels read and drawn at a time (whole rows): a screen of them is never held twice.
constexpr std::size_t chunk = std::size_t{64} * 1024;
// What no rectangle costs less than, however few its pixels; and a Hextile tile, whatever of it is on the screen.
constexpr std::int64_t rectangleWork = 256;
constexpr std::int64_t tileWork = 256;
constexpr int tilesARectangle = 8;
constexpr std::int64_t gradientCost = 8; // a Tight gradient costs this many times a plain rectangle
constexpr std::int64_t cheap = 8;        // a fill, or a JPEG, this many times less

// The pixel format this program asks for (rfb.py's PIXEL_FORMAT and PIXEL_FORMAT_565): bits a pixel, depth,
// big-endian, true colour, the three maxima (16 bits each), the three shifts, and three bytes of nothing. On the wire a
// pixel is then R, G, B and a byte of nothing; ZRLE's three-byte pixel and Tight's are R, G, B.
constexpr std::array<std::uint8_t, 16> format888{32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 0, 8, 16, 0, 0, 0};
constexpr std::array<std::uint8_t, 16> format565{16, 16, 0, 1, 0, 31, 0, 63, 0, 31, 11, 5, 0, 0, 0, 0};

std::uint32_t be16(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return (std::uint32_t{bytes[at]} << 8) | bytes[at + 1];
}

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return (be16(bytes, at) << 16) | be16(bytes, at + 2);
}

void put16(Bytes& into, std::uint32_t value)
{
    into.push_back(static_cast<std::uint8_t>(value >> 8));
    into.push_back(static_cast<std::uint8_t>(value));
}

void put32(Bytes& into, std::uint32_t value)
{
    put16(into, value >> 16);
    put16(into, value & 0xFFFF);
}

// What the registry calls the ways of signing in that this program does not speak, for a tile to say so.
std::string named(std::uint32_t kind)
{
    static constexpr std::array<std::pair<std::uint32_t, std::string_view>, 10> names{{
        {5, "RA2"},
        {6, "RA2ne"},
        {16, "Tight"},
        {17, "Ultra"},
        {18, "TLS"},
        {19, "VeNCrypt"},
        {20, "SASL"},
        {21, "MD5"},
        {22, "xvp"},
        {30, "Apple remote desktop"},
    }};
    for (const auto& [number, name] : names) {
        if (number == kind) {
            return std::string(name) + ", type " + std::to_string(kind);
        }
    }
    return "type " + std::to_string(kind);
}

// The reason a server gives for a no, as one short line of what can be shown of it.
std::string oneLine(std::span<const std::uint8_t> said)
{
    std::string line;
    bool blank = false;
    for (const std::uint8_t byte : said) {
        if (byte > 0x20 && byte < 0x7F) {
            if (blank && !line.empty()) {
                line += ' ';
            }
            blank = false;
            line += static_cast<char>(byte);
        } else {
            blank = true;
        }
        if (line.size() >= 200) {
            break;
        }
    }
    return line.empty() ? std::string("no reason given") : line;
}

// A rectangle's bytes as they are taken apart: a take past the end is no take.
class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}
    bool take(std::size_t size, std::span<const std::uint8_t>& taken)
    {
        if (size > m_bytes.size() - m_at) {
            return false;
        }
        taken = m_bytes.subspan(m_at, size);
        m_at += size;
        return true;
    }
    bool byte(std::uint8_t& taken)
    {
        if (m_at >= m_bytes.size()) {
            return false;
        }
        taken = m_bytes[m_at++];
        return true;
    }
    std::size_t left() const noexcept { return m_bytes.size() - m_at; }

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t m_at = 0;
};

// Pixels of four bytes (R, G, B, nothing), as three.
void rgbOf888(std::span<const std::uint8_t> pixels, Bytes& into)
{
    into.resize(pixels.size() / 4 * 3);
    for (std::size_t from = 0, to = 0; from + 3 < pixels.size(); from += 4, to += 3) {
        into[to] = pixels[from];
        into[to + 1] = pixels[from + 1];
        into[to + 2] = pixels[from + 2];
    }
}

// Pixels of two bytes (RGB565, little-endian), as three, each stretched to 0..255 and rounded.
void rgbOf565(std::span<const std::uint8_t> pixels, Bytes& into)
{
    into.resize(pixels.size() / 2 * 3);
    for (std::size_t from = 0, to = 0; from + 1 < pixels.size(); from += 2, to += 3) {
        const std::uint32_t value = pixels[from] | (std::uint32_t{pixels[from + 1]} << 8);
        into[to] = static_cast<std::uint8_t>(((value >> 11) * 255 + 15) / 31);
        into[to + 1] = static_cast<std::uint8_t>((((value >> 5) & 63) * 255 + 31) / 63);
        into[to + 2] = static_cast<std::uint8_t>(((value & 31) * 255 + 15) / 31);
    }
}

// A ZRLE run length: one more than its bytes add up to, a byte of 255 saying another follows.
bool runLength(Cursor& data, std::int64_t& length)
{
    length = 1;
    for (;;) {
        std::uint8_t byte = 0;
        if (!data.byte(byte)) {
            return false;
        }
        length += byte;
        if (byte != 255) {
            return true;
        }
        if (length > std::int64_t{mostPixels}) {
            return false; // (no tile has this many pixels: what follows is cut short as far as it goes)
        }
    }
}

} // namespace

Bytes updateRequest(bool incremental, int x, int y, int width, int height)
{
    Bytes made{updateRequestKind, static_cast<std::uint8_t>(incremental ? 1 : 0)};
    for (const int value : {x, y, width, height}) {
        put16(made, static_cast<std::uint32_t>(std::clamp(value, 0, 0xFFFF)));
    }
    return made;
}

Bytes keyEvent(std::uint32_t keysym, bool down)
{
    Bytes made{keyEventKind, static_cast<std::uint8_t>(down ? 1 : 0), 0, 0};
    put32(made, keysym);
    return made;
}

Bytes pointerEvent(int x, int y, std::uint8_t buttons)
{
    Bytes made{pointerEventKind, buttons};
    put16(made, static_cast<std::uint32_t>(std::clamp(x, 0, 0xFFFF)));
    put16(made, static_cast<std::uint32_t>(std::clamp(y, 0, 0xFFFF)));
    return made;
}

Client::Client(Options options) : m_options(std::move(options))
{
    // What the server is told this program reads: only what it reads, Raw always among it; JPEG only where a decoder
    // is there; and with RGB565, Raw and CopyRect alone (the others carry pixels of their own shapes in that format).
    for (const std::int32_t encoding : m_options.encodings) {
        const bool known = std::ranges::find(allEncodings, encoding) != allEncodings.end();
        const bool fits =
            m_options.pixels == Pixels::Rgb888 || encoding == raw || encoding == copyRect || encoding == desktopSize;
        const bool jpeg = encoding != jpegQuality || m_options.jpeg;
        if (known && fits && jpeg && std::ranges::find(m_said, encoding) == m_said.end()) {
            m_said.push_back(encoding);
        }
    }
    if (std::ranges::find(m_said, raw) == m_said.end()) {
        m_said.push_back(raw);
    }
}

base::Result<void> Client::fail(std::string_view code, std::string message)
{
    m_stage = Stage::Over;
    m_held.clear();
    m_at = 0;
    m_plain.clear();
    return base::Error{std::string(code), std::move(message)};
}

std::int64_t Client::clockNow() const
{
    if (m_options.clock) {
        return m_options.clock();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t Client::workNow() const
{
    return m_options.workClock ? m_options.workClock() : clockNow();
}

bool Client::stoppedNow() const
{
    return m_options.stopped && m_options.stopped();
}

base::Result<void> Client::tooSlowToDraw()
{
    if (m_stopped) {
        return fail(stopped, "whoever held the line let go of it");
    }
    return fail(limit, "the server's pictures take far too long to draw");
}

bool Client::overBudget()
{
    // Whoever holds the line let go of it: what is under way stops here, at the next asking (outside review 2.1).
    if (stoppedNow()) {
        m_stopped = true;
        return true;
    }
    if (m_options.longStepMs < 0) {
        return false; // the share is off (a test's wish)
    }
    if (m_refused) {
        return true;
    }
    const std::int64_t wall = clockNow();
    const std::int64_t work = workNow();
    // The share earns `sharePerMil` of a drawing thread a second, up to a burst; it earns over every second of wall,
    // the held-back seconds between batches among them (m_creditAt is not reset per batch).
    m_credit = std::min(m_options.burstMs, m_credit + (wall - m_creditAt) * m_options.sharePerMil / 1000);
    m_creditAt = wall;
    const std::int64_t drawn = std::max<std::int64_t>(0, wall - m_lastWall);
    const std::int64_t worked = std::max<std::int64_t>(0, work - m_lastWork);
    m_lastWall = wall;
    m_lastWork = work;
    m_credit -= shareCharge(drawn, worked);
    // A long step is the drawing thread's own processor time (not the clock, which a stall or a busy core inflates):
    // a step that truly took LONG_STEP of a core, more than MOST_LONG_STEPS times within LONG_STEPS_WITHIN, refuses a
    // server. Nothing is refused merely for being over its share (round-2 review 2): the screen shows it more slowly.
    if (worked >= m_options.longStepMs) {
        const std::int64_t since = wall - m_options.longStepsWithinMs;
        std::erase_if(m_longStepsAt, [since](std::int64_t at) { return at < since; });
        m_longStepsAt.push_back(wall);
        if (static_cast<int>(m_longStepsAt.size()) > m_options.mostLongSteps) {
            m_refused = true;
        }
    }
    return m_refused;
}

std::int64_t Client::shareCharge(std::int64_t drawnWall, std::int64_t workedThread) const
{
    // What a step is charged to the share (round-2 review 4): its processor time where its clock hop was long enough
    // for a core's counter to read true (countedFromMs or more), else the clock. Short steps read nought of a core
    // (Windows counts it at 15.6 ms ticks), so they are charged by the clock; a long step descheduled off a busy core
    // is charged what the core did, not the wall it lost.
    return drawnWall >= m_options.countedFromMs ? workedThread : drawnWall;
}

std::int64_t Client::pausedForMs()
{
    if (m_options.longStepMs < 0 || m_refused) {
        return 0;
    }
    // Asked at a batch's end (the reader's tell): the share earns over the wall since it was last up to date, and is
    // charged the batch's last drawing (the time since the last step, which no overBudget charged). Over its share
    // (credit below nought), the screen holds the line back and asks for nothing this long, until it is earned back.
    const std::int64_t wall = clockNow();
    const std::int64_t work = workNow();
    m_credit = std::min(m_options.burstMs, m_credit + (wall - m_creditAt) * m_options.sharePerMil / 1000);
    m_creditAt = wall;
    m_credit -= shareCharge(std::max<std::int64_t>(0, wall - m_lastWall), std::max<std::int64_t>(0, work - m_lastWork));
    m_lastWall = wall;
    m_lastWork = work;
    if (m_credit >= 0) {
        return 0;
    }
    return (-m_credit * 1000 + m_options.sharePerMil - 1) / m_options.sharePerMil;
}

std::span<const std::uint8_t> Client::there() const noexcept
{
    return std::span<const std::uint8_t>(m_held).subspan(m_at);
}

void Client::consume(std::size_t bytes) noexcept
{
    m_at += bytes;
}

void Client::say(std::span<const std::uint8_t> bytes)
{
    m_out.insert(m_out.end(), bytes.begin(), bytes.end());
}

Bytes Client::takeOutgoing()
{
    Bytes taken;
    taken.swap(m_out);
    return taken;
}

std::optional<std::array<std::uint8_t, 16>> Client::challenge() const
{
    return m_stage == Stage::Challenge ? m_challenge : std::nullopt;
}

base::Result<void> Client::answer(std::span<const std::uint8_t> response, std::int64_t nowMs)
{
    if (m_stage != Stage::Challenge || !m_challenge || response.size() != 16) {
        return fail(protocol, "an answer to a challenge the server did not ask");
    }
    say(response);
    m_challenge.reset();
    m_stage = Stage::SecurityResult;
    m_now = nowMs;
    return run();
}

base::Result<void> Client::feed(std::span<const std::uint8_t> data, std::int64_t nowMs)
{
    if (m_stage == Stage::Over) {
        return base::Error{std::string(protocol), "the connection to the server is over"};
    }
    m_now = nowMs;
    m_held.insert(m_held.end(), data.begin(),
                  data.end()); // (what was read before is let go of as each batch ends: run)
    return run();
}

base::Result<void> Client::run()
{
    if (!m_shareStarted) {
        // The connection begins with a full burst of share, kept up from here (the first screen may draw a while).
        m_shareStarted = true;
        m_credit = m_options.burstMs;
        m_creditAt = clockNow();
    }
    // This batch's drawing is charged from here: the wall held back between batches, and the processor time the pool
    // thread spent on other readers then, are not this batch's (overBudget charges wall - m_lastWall, work -
    // m_lastWork).
    m_lastWall = clockNow();
    m_lastWork = workNow();
    // What was fed is drawn whole, and the share is settled after it (pausedForMs): a batch is not stopped part way.
    // Accepted (round-3 review LOW 6, coordinator's decision on the round-4 review): stopping between rectangles kept
    // the rest here, unbounded and uncounted by the screen's hold-back, and could strand it after a pause. Drawn whole,
    // a batch is bounded by the reader's queue, which the screen holds back past Limits::heldBackFromBytes (8 MB, plus
    // the one line message in flight, 1 MB): a burst of at the most seconds of one drawing thread, after which the
    // screen holds the line back for the whole debt, so over time the reader has DRAWING_SHARE (half a thread), as on
    // the web; a server whose single steps take LONG_STEP again and again is refused (overBudget).
    for (bool more = true; more;) {
        more = false;
        if (const base::Result<void> stepped = step(more); !stepped) {
            return stepped;
        }
        // A message whole (the reader back at the next one's kind): its stamp goes with it, wherever the batch ends
        // (round-5 review LOW 4: behind a line that cuts messages anywhere, no batch may end between two).
        if (m_part == At::Kind) {
            m_since.reset();
        }
    }
    // A message under way at the batch's end: since the batch it was first seen in.
    if (m_stage == Stage::Ready && (m_part != At::Kind || held() > 0)) {
        if (!m_since) {
            m_since = m_now;
        }
    } else {
        m_since.reset();
    }
    // What was read is let go of (round-5 review LOW 3): all of it once all is read, else once it is a megabyte or the
    // larger part of what is kept, so that less than a megabyte of it is kept before a piece read whole. And the room
    // of a piece larger than mostKeptRoom is given back once it is done, not kept for the rest of the session: what is
    // left, a message begun, moved to room of its own.
    if (m_at == m_held.size()) {
        m_held.clear();
        m_at = 0;
    } else if (m_at >= std::size_t{1024} * 1024 || m_at * 2 >= m_held.size()) {
        m_held.erase(m_held.begin(), m_held.begin() + static_cast<std::ptrdiff_t>(m_at));
        m_at = 0;
    }
    if (m_held.capacity() > mostKeptRoom && held() <= mostKeptRoom / 2) {
        Bytes rest(m_held.begin() + static_cast<std::ptrdiff_t>(m_at), m_held.end());
        m_held = std::move(rest);
        m_at = 0;
    }
    // What a rectangle's stream inflated to is that rectangle's alone, drawn within its step and kept by none between
    // batches: the room of one larger than mostKeptRoom (a whole-screen ZRLE's, some 65 MB at the largest) is given
    // back too (round-6 review LOW 3).
    if (m_plain.capacity() > mostKeptRoom) {
        m_plain = Bytes{};
    }
    return {};
}

base::Result<void> Client::step(bool& more)
{
    return m_stage == Stage::Ready ? message(more) : handshake(more);
}

base::Result<void> Client::fit(int width, int height)
{
    if (width <= 0 || width > largestSide || height <= 0 || height > largestSide ||
        std::int64_t{width} * height > mostPixels) {
        return fail(limit, "a screen of " + std::to_string(width) + " by " + std::to_string(height) +
                               " pixels, which is more than this program shows");
    }
    if (width == m_width && height == m_height) {
        return {};
    }
    // A screen of this size from now on, keeping what was at its top left.
    Bytes made(std::size_t{3} * static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const int keptWidth = std::min(width, m_width);
    const int keptHeight = std::min(height, m_height);
    for (int row = 0; row < keptHeight; ++row) {
        std::memcpy(made.data() + std::size_t{3} * static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                    m_rgb.data() + std::size_t{3} * static_cast<std::size_t>(row) * static_cast<std::size_t>(m_width),
                    std::size_t{3} * static_cast<std::size_t>(keptWidth));
    }
    m_rgb.swap(made);
    // (A change of size is a change of all of it; the size the server begins with is none: nothing is drawn yet.)
    if (m_width > 0) {
        m_changed.resized = true;
        m_changed.rects.assign(1, Rect{0, 0, width, height});
    }
    m_width = width;
    m_height = height;
    return {};
}

void Client::changed(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    const Rect whole{0, 0, m_width, m_height};
    if (m_changed.rects.size() == 1 && m_changed.rects.front() == whole) {
        return;
    }
    if (m_changed.rects.size() >= mostChanged) {
        m_changed.rects.assign(1, whole);
        return;
    }
    m_changed.rects.push_back(Rect{x, y, width, height});
}

Client::Changed Client::take()
{
    Changed taken;
    std::swap(taken, m_changed);
    return taken;
}

Bytes Client::request(bool incremental) const
{
    return updateRequest(incremental, 0, 0, m_width, m_height);
}

Bytes Client::pointer(int x, int y, std::uint8_t buttons) const
{
    return pointerEvent(std::clamp(x, 0, std::max(0, m_width - 1)), std::clamp(y, 0, std::max(0, m_height - 1)),
                        buttons);
}

void Client::blit(int x, int y, int width, int height, std::span<const std::uint8_t> rgb)
{
    const std::size_t row = std::size_t{3} * static_cast<std::size_t>(width);
    for (int line = 0; line < height; ++line) {
        const std::size_t at =
            std::size_t{3} *
            (static_cast<std::size_t>(y + line) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x));
        const std::span<const std::uint8_t> from = rgb.subspan(static_cast<std::size_t>(line) * row, row);
        std::copy(from.begin(), from.end(), m_rgb.begin() + static_cast<std::ptrdiff_t>(at));
    }
}

void Client::fill(int x, int y, int width, int height, std::span<const std::uint8_t> pixel)
{
    for (int line = 0; line < height; ++line) {
        std::size_t at = std::size_t{3} * (static_cast<std::size_t>(y + line) * static_cast<std::size_t>(m_width) +
                                           static_cast<std::size_t>(x));
        for (int across = 0; across < width; ++across, at += 3) {
            m_rgb[at] = pixel[0];
            m_rgb[at + 1] = pixel[1];
            m_rgb[at + 2] = pixel[2];
        }
    }
}

void Client::countFromNow()
{
    if (m_second < 0 || m_now - m_second >= 1000) {
        m_second = m_now;
        m_messages = m_rectangles = m_resizes = 0;
        m_work = 0;
        m_blocks = 0;
    }
}

base::Result<void> Client::count(int messages, int rectangles, int resizes, std::int64_t work, std::int64_t blocks)
{
    countFromNow();
    m_messages += messages;
    m_rectangles += rectangles;
    m_resizes += resizes;
    m_work += work;
    m_blocks += blocks; // (held to mostBlocksASecond as each stream is read: boundsFor)
    if (m_messages > mostMessagesASecond || m_rectangles > mostRectanglesASecond) {
        return fail(limit, "the server sends far more than a server has reason to");
    }
    if (m_resizes > mostResizesASecond) {
        return fail(limit, "the server changes the screen's size far more often than a server has reason to");
    }
    if (m_work > std::max<std::int64_t>(std::int64_t{mostScreensASecond} * m_width * m_height, leastWorkASecond)) {
        return fail(limit, "the server sends more pixels in a second than its screen could change");
    }
    return {};
}

Inflater::Bounds Client::boundsFor(std::size_t room)
{
    countFromNow();
    Inflater::Bounds bounds;
    // The second's work as count() holds it, of which a stream's tables may take what is left: counted before each
    // table is built, so that a run of tiny dynamic blocks is cut off where the second's work runs out, not read whole
    // and refused after (outside review 2.1).
    const std::int64_t work =
        std::max<std::int64_t>(std::int64_t{mostScreensASecond} * m_width * m_height, leastWorkASecond);
    bounds.mostTableWork = static_cast<std::uint64_t>(std::max<std::int64_t>(0, work - m_work));
    bounds.mostBlocks = std::min(room / bytesABlock + blocksARectangle,
                                 static_cast<std::uint64_t>(std::max<std::int64_t>(0, mostBlocksASecond - m_blocks)));
    // The share asked as the stream is read (each dynamic block, and every so many others and codes); and a stream that
    // has taken a long step of the thread's own time since the step before it is cut off there, the server at fault.
    const std::int64_t began = m_lastWork;
    bounds.overBudget = [this, began] {
        return overBudget() || (m_options.longStepMs >= 0 && m_lastWork - began >= m_options.longStepMs);
    };
    return bounds;
}

// --- the handshake ----------------------------------------------------------------------------------------------

base::Result<void> Client::handshake(bool& more)
{
    const std::span<const std::uint8_t> got = there();
    const auto afterSecurity = [this] {
        if (m_security == securityVnc) {
            m_stage = Stage::Challenge;
        } else if (m_minor == 8) {
            m_stage = Stage::SecurityResult;
        } else {
            say(std::array<std::uint8_t, 1>{1}); // ClientInit: shared, so that nobody else at the server is thrown out
            m_stage = Stage::Init;
        }
    };
    const auto refusal = [this](std::string_view code, const std::string& prefix) {
        m_why = std::string(code) + '\n' + prefix;
        m_wanted = 0;
        m_stage = Stage::Reason;
    };
    switch (m_stage) {
    case Stage::Version: {
        if (got.size() < 12) {
            return {};
        }
        const std::string said(got.begin(), got.begin() + 12);
        consume(12);
        const auto digits = [&said](std::size_t from) {
            return std::all_of(said.begin() + static_cast<std::ptrdiff_t>(from),
                               said.begin() + static_cast<std::ptrdiff_t>(from + 3),
                               [](char one) { return one >= '0' && one <= '9'; });
        };
        if (said.compare(0, 4, "RFB ") != 0 || !digits(4) || said[7] != '.' || !digits(8) || said[11] != '\n') {
            return fail(protocol, "what answers is not a VNC server (its greeting is not RFB)");
        }
        int major = std::stoi(said.substr(4, 3));
        int minor = std::stoi(said.substr(8, 3));
        if (std::ranges::find(m_options.greetedAs38, said) != m_options.greetedAs38.end()) {
            major = 3;
            minor = 8;
        }
        if (major != 3) {
            return fail(unsupported, "the server speaks RFB " + std::to_string(major) + "." + std::to_string(minor) +
                                         ", which this program does not");
        }
        // The RFC's three versions; anything else of version 3 is taken for 3.3, as it says.
        m_minor = minor >= 8 ? 8 : minor == 7 ? 7 : 3;
        const std::string answer = "RFB 003.00" + std::to_string(m_minor) + "\n";
        say(std::span(reinterpret_cast<const std::uint8_t*>(answer.data()), answer.size()));
        m_stage = m_minor == 3 ? Stage::SecurityType : Stage::SecurityCount;
        more = true;
        return {};
    }
    case Stage::SecurityType: {
        if (got.size() < 4) {
            return {};
        }
        const std::uint32_t chosen = be32(got, 0);
        consume(4);
        more = true;
        if (chosen == securityInvalid) {
            refusal(refused, "the server refused the connection: ");
            return {};
        }
        if (chosen > 255 ||
            std::ranges::find(m_options.signsIn, static_cast<std::uint8_t>(chosen)) == m_options.signsIn.end()) {
            return fail(unsupported,
                        "the server asks for a way of signing in this program does not speak (" + named(chosen) + ")");
        }
        m_security = static_cast<std::uint8_t>(chosen);
        afterSecurity();
        return {};
    }
    case Stage::SecurityCount: {
        if (got.empty()) {
            return {};
        }
        m_wanted = got[0];
        consume(1);
        more = true;
        if (m_wanted == 0) {
            refusal(refused, "the server refused the connection: ");
            return {};
        }
        m_stage = Stage::SecurityList;
        return {};
    }
    case Stage::SecurityList: {
        if (got.size() < m_wanted) {
            return {};
        }
        const std::span<const std::uint8_t> offered = got.first(m_wanted);
        const auto allows = [this, offered](std::uint8_t kind) {
            return std::ranges::find(offered, kind) != offered.end() &&
                   std::ranges::find(m_options.signsIn, kind) != m_options.signsIn.end();
        };
        // None where it is offered: nothing to send, and nothing of the server's business which other ways it has.
        if (allows(securityNone)) {
            m_security = securityNone;
        } else if (allows(securityVnc)) {
            m_security = securityVnc;
        } else {
            std::vector<std::uint8_t> kinds(offered.begin(), offered.end());
            std::ranges::sort(kinds);
            kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());
            std::string others;
            for (const std::uint8_t kind : kinds) {
                others += (others.empty() ? "" : ", ") + named(kind);
            }
            return fail(unsupported,
                        "the server asks for a way of signing in this program does not speak (" + others + ")");
        }
        consume(m_wanted);
        say(std::array<std::uint8_t, 1>{m_security});
        afterSecurity();
        more = true;
        return {};
    }
    case Stage::Challenge: {
        if (m_challenge) {
            return {}; // (the answer is waited for)
        }
        if (got.size() < 16) {
            return {};
        }
        std::array<std::uint8_t, 16> asked{};
        std::copy_n(got.begin(), 16, asked.begin());
        consume(16);
        m_challenge = asked;
        return {};
    }
    case Stage::SecurityResult: {
        if (got.size() < 4) {
            return {};
        }
        const std::uint32_t result = be32(got, 0);
        consume(4);
        more = true;
        if (result == 0) {
            say(std::array<std::uint8_t, 1>{1});
            m_stage = Stage::Init;
            return {};
        }
        if (result == 1) {
            const std::string_view code = m_security == securityVnc ? auth : refused;
            if (m_minor == 8) {
                refusal(code, "");
                return {};
            }
            return fail(code, "the server rejected the password");
        }
        if (result == 2) {
            // (Not in the RFC; TightVNC and RealVNC say it after some wrong passwords, and shut the address out.)
            return fail(refused,
                        "the server says there were too many failed sign-ins, and shuts this address out for a while");
        }
        return fail(protocol,
                    "the server answered the sign-in with " + std::to_string(result) + ", which means nothing");
    }
    case Stage::Reason: {
        if (m_wanted == 0) {
            if (got.size() < 4) {
                return {};
            }
            const std::uint32_t length = be32(got, 0);
            if (length > mostReason) {
                return fail(limit,
                            "the server gives a reason of " + std::to_string(length) + " bytes, which is no reason");
            }
            consume(4);
            if (length == 0) {
                const std::size_t cut = m_why.find('\n');
                return fail(m_why.substr(0, cut), m_why.substr(cut + 1) + "no reason given");
            }
            m_wanted = length;
            more = true;
            return {};
        }
        if (got.size() < m_wanted) {
            return {};
        }
        const std::string line = oneLine(got.first(m_wanted));
        const std::size_t cut = m_why.find('\n');
        return fail(m_why.substr(0, cut), m_why.substr(cut + 1) + line);
    }
    case Stage::Init: {
        if (got.size() < 24) {
            return {};
        }
        const auto width = static_cast<int>(be16(got, 0));
        const auto height = static_cast<int>(be16(got, 2));
        const std::uint32_t nameLength = be32(got, 20);
        if (nameLength > mostName) {
            return fail(limit,
                        "the server gives itself a name of " + std::to_string(nameLength) + " bytes, which is no name");
        }
        if (const base::Result<void> fitted = fit(width, height); !fitted) {
            return fitted;
        }
        consume(24);
        m_wanted = nameLength;
        m_stage = Stage::Name;
        more = true;
        return {};
    }
    case Stage::Name: {
        if (got.size() < m_wanted) {
            return {};
        }
        m_name.assign(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(m_wanted));
        consume(m_wanted);
        Bytes first{setPixelFormatKind, 0, 0, 0};
        const auto& format = m_options.pixels == Pixels::Rgb565 ? format565 : format888;
        first.insert(first.end(), format.begin(), format.end());
        first.push_back(setEncodingsKind);
        first.push_back(0);
        put16(first, static_cast<std::uint32_t>(m_said.size()));
        for (const std::int32_t encoding : m_said) {
            put32(first, static_cast<std::uint32_t>(encoding));
        }
        say(first);
        m_stage = Stage::Ready;
        m_part = At::Kind;
        more = true;
        return {};
    }
    case Stage::Ready:
    case Stage::Over:
        break;
    }
    return {};
}

// --- what the server sends ---------------------------------------------------------------------------------------

base::Result<void> Client::message(bool& more)
{
    const std::span<const std::uint8_t> got = there();
    switch (m_part) {
    case At::Kind: {
        if (got.empty()) {
            return {};
        }
        const std::uint8_t kind = got[0];
        consume(1);
        more = true;
        if (const base::Result<void> counted = count(1, 0, 0, 0); !counted) {
            return counted;
        }
        if (kind == updateKind) {
            m_part = At::Update;
        } else if (kind == colourMapKind) {
            m_part = At::ColourMap;
        } else if (kind == cutTextKind) {
            m_part = At::CutText;
        } else if (kind != bellKind) {
            return fail(protocol, "a message of a kind this program does not read (type " + std::to_string(kind) + ")");
        }
        return {};
    }
    case At::Update: {
        if (got.size() < 3) {
            return {};
        }
        m_rectsLeft = static_cast<int>(be16(got, 1));
        consume(3);
        more = true;
        if (const base::Result<void> counted = count(0, m_rectsLeft, 0, 0); !counted) {
            return counted;
        }
        m_drew = false;
        m_updatePixels = 0;
        if (m_rectsLeft == 0) {
            ++m_updates;
            m_part = At::Kind;
            return {};
        }
        m_part = At::Rect;
        return {};
    }
    case At::Rect:
        return rectangle(more);
    case At::RawRows:
        return rawRows(more);
    case At::CopyRect: {
        if (got.size() < 4) {
            return {};
        }
        const auto fromX = static_cast<int>(be16(got, 0));
        const auto fromY = static_cast<int>(be16(got, 2));
        consume(4);
        more = true;
        const Rect& rect = m_rect;
        if (fromX + rect.width > m_width || fromY + rect.height > m_height) {
            return fail(protocol, "a rectangle copied from outside the screen");
        }
        // The two may overlap: every row is read before it is written over, from the bottom up where the copy moves
        // down, from the top down otherwise.
        const std::size_t row = std::size_t{3} * static_cast<std::size_t>(rect.width);
        const auto place = [this](int x, int y) {
            return std::size_t{3} *
                   (static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x));
        };
        for (int line = 0; line < rect.height; ++line) {
            const int at = rect.y > fromY ? rect.height - 1 - line : line;
            if (row > 0) {
                std::memmove(m_rgb.data() + place(rect.x, rect.y + at), m_rgb.data() + place(fromX, fromY + at), row);
            }
        }
        changed(rect.x, rect.y, rect.width, rect.height);
        return rectDone();
    }
    case At::Hextile:
        return hextileTile(more);
    case At::ZrleLength: {
        if (got.size() < 4) {
            return {};
        }
        const std::uint32_t length = be32(got, 0);
        const std::int64_t tiles =
            std::int64_t{(m_rect.width + 63) / 64} * static_cast<std::int64_t>((m_rect.height + 63) / 64);
        const std::int64_t area = std::int64_t{m_rect.width} * m_rect.height;
        if (length > 4 * area + 64 * tiles + static_cast<std::int64_t>(zlibSlack)) {
            return fail(limit, "a tile stream of " + std::to_string(length) + " bytes for a rectangle of " +
                                   std::to_string(m_rect.width) + " by " + std::to_string(m_rect.height) +
                                   ", which is more than it could hold");
        }
        consume(4);
        m_dataLength = length;
        m_part = At::ZrleData;
        more = true;
        return {};
    }
    case At::ZrleData:
        return zrleData(more);
    case At::Tight:
        return tightRect(more);
    case At::ColourMap: {
        if (got.size() < 5) {
            return {};
        }
        // (Asked for true colour: a map means nothing here.)
        m_dropLeft = std::size_t{6} * be16(got, 3);
        consume(5);
        m_part = At::Drop;
        more = true;
        return {};
    }
    case At::CutText: {
        if (got.size() < 7) {
            return {};
        }
        const std::uint32_t length = be32(got, 3);
        if (length > mostCutText) {
            return fail(limit, "the server sends a clipboard of " + std::to_string(length) +
                                   " bytes, which is more than this program takes");
        }
        consume(7);
        m_dropLeft = length;
        m_part = At::Drop;
        more = true;
        return {};
    }
    case At::Drop: {
        const std::size_t taken = std::min(m_dropLeft, got.size());
        consume(taken);
        m_dropLeft -= taken;
        if (m_dropLeft == 0) {
            m_part = At::Kind;
            more = true;
        }
        return {};
    }
    }
    return {};
}

base::Result<void> Client::rectDone()
{
    if (--m_rectsLeft > 0) {
        m_part = At::Rect;
        return {};
    }
    ++m_updates;
    if (m_drew) {
        ++m_frames;
    }
    m_part = At::Kind;
    return {};
}

base::Result<void> Client::rectangle(bool& more)
{
    const std::span<const std::uint8_t> got = there();
    if (got.size() < 12) {
        return {};
    }
    const Rect rect{static_cast<int>(be16(got, 0)), static_cast<int>(be16(got, 2)), static_cast<int>(be16(got, 4)),
                    static_cast<int>(be16(got, 6))};
    const auto encoding = static_cast<std::int32_t>(be32(got, 8));
    consume(12);
    more = true;
    if (encoding == desktopSize) {
        if (const base::Result<void> counted = count(0, 0, 1, 0); !counted) {
            return counted;
        }
        if (const base::Result<void> fitted = fit(rect.width, rect.height); !fitted) {
            return fitted;
        }
        return rectDone();
    }
    if (encoding < 0 || std::ranges::find(m_said, encoding) == m_said.end()) {
        return fail(protocol,
                    "a rectangle in an encoding this program did not ask for (" + std::to_string(encoding) + ")");
    }
    if (rect.x + rect.width > m_width || rect.y + rect.height > m_height) {
        return fail(protocol, "a rectangle outside the screen (" + std::to_string(rect.width) + " by " +
                                  std::to_string(rect.height) + " at " + std::to_string(rect.x) + ", " +
                                  std::to_string(rect.y) + " on " + std::to_string(m_width) + " by " +
                                  std::to_string(m_height) + ")");
    }
    const std::int64_t area = std::int64_t{rect.width} * rect.height;
    m_drew = m_drew || area > 0;
    m_updatePixels += area;
    if (m_updatePixels > std::int64_t{mostScreensAnUpdate} * m_width * m_height) {
        return fail(limit, "the server sends more pixels in one update than its screen could change");
    }
    m_rect = rect;
    if (encoding == raw || encoding == copyRect) {
        if (const base::Result<void> counted = count(0, 0, 0, std::max(area, rectangleWork)); !counted) {
            return counted;
        }
        if (encoding == copyRect) {
            m_part = At::CopyRect;
            return {};
        }
        m_rowsDone = 0;
        if (area == 0) {
            return rectDone();
        }
        m_part = At::RawRows;
        return {};
    }
    if (encoding == hextile) {
        if (area == 0) {
            // (No tiles, and nothing of them follows.)
            if (const base::Result<void> counted = count(0, 0, 0, rectangleWork); !counted) {
                return counted;
            }
            return rectDone();
        }
        const int tiles = ((rect.width + 15) / 16) * ((rect.height + 15) / 16);
        if (const base::Result<void> counted =
                count(0, (tiles + tilesARectangle - 1) / tilesARectangle, 0, std::int64_t{tiles} * tileWork);
            !counted) {
            return counted;
        }
        m_tile = 0;
        m_background = {};
        m_foreground = {};
        m_part = At::Hextile;
        return {};
    }
    if (encoding == zrle) {
        if (const base::Result<void> counted = count(0, 0, 0, std::max(area, rectangleWork)); !counted) {
            return counted;
        }
        m_part = At::ZrleLength;
        return {};
    }
    if (rect.width > tightWidest) {
        return fail(protocol, "a Tight rectangle " + std::to_string(rect.width) +
                                  " pixels wide, which its own rules do not allow");
    }
    m_part = At::Tight;
    return {};
}

base::Result<void> Client::rawRows(bool& more)
{
    const std::size_t bytesAPixel = m_options.pixels == Pixels::Rgb565 ? 2 : 4;
    const std::size_t row = bytesAPixel * static_cast<std::size_t>(m_rect.width);
    const std::size_t there = this->there().size();
    const auto rows = static_cast<int>(std::min(
        {std::max<std::size_t>(1, chunk / row), there / row, static_cast<std::size_t>(m_rect.height - m_rowsDone)}));
    if (rows == 0) {
        return {};
    }
    const std::span<const std::uint8_t> pixels = this->there().first(static_cast<std::size_t>(rows) * row);
    if (m_options.pixels == Pixels::Rgb565) {
        rgbOf565(pixels, m_plain);
    } else {
        rgbOf888(pixels, m_plain);
    }
    blit(m_rect.x, m_rect.y + m_rowsDone, m_rect.width, rows, m_plain);
    changed(m_rect.x, m_rect.y + m_rowsDone, m_rect.width, rows);
    consume(pixels.size());
    m_rowsDone += rows;
    more = true;
    if (overBudget()) { // charges this chunk's drawing against the share; ends only on a long step (round-2 review 5)
        return tooSlowToDraw();
    }
    if (m_rowsDone == m_rect.height) {
        return rectDone();
    }
    return {};
}

base::Result<void> Client::hextileTile(bool& more)
{
    const std::span<const std::uint8_t> got = there();
    if (got.empty()) {
        return {};
    }
    const int across = (m_rect.width + 15) / 16;
    const int left = (m_tile % across) * 16;
    const int top = (m_tile / across) * 16;
    const int tileWidth = std::min(16, m_rect.width - left);
    const int tileHeight = std::min(16, m_rect.height - top);
    const std::uint8_t sub = got[0];
    // The tile's length, as far as it says it (it says it piece by piece).
    std::size_t length = 1;
    if ((sub & 1) != 0) {
        length += std::size_t{4} * static_cast<std::size_t>(tileWidth) * static_cast<std::size_t>(tileHeight);
    } else {
        length += ((sub & 2) != 0 ? 4 : 0) + ((sub & 4) != 0 ? 4 : 0);
        if ((sub & 8) != 0) {
            if (got.size() <= length) {
                return {};
            }
            length += 1 + std::size_t{got[length]} * ((sub & 16) != 0 ? 6 : 2);
        }
    }
    if (got.size() < length) {
        return {};
    }
    Cursor data(got.first(length));
    std::span<const std::uint8_t> taken;
    static_cast<void>(data.take(1, taken));
    const int x = m_rect.x + left;
    const int y = m_rect.y + top;
    if ((sub & 1) != 0) {
        static_cast<void>(data.take(length - 1, taken));
        rgbOf888(taken, m_plain);
        blit(x, y, tileWidth, tileHeight, m_plain);
    } else {
        if ((sub & 2) != 0) {
            static_cast<void>(data.take(4, taken));
            std::copy_n(taken.begin(), 3, m_background.begin());
        }
        if ((sub & 4) != 0) {
            static_cast<void>(data.take(4, taken));
            std::copy_n(taken.begin(), 3, m_foreground.begin());
        }
        fill(x, y, tileWidth, tileHeight, m_background);
        std::int64_t subrects = 0; // the pixels the subrectangle fills drew, beyond the tile the rectangle counted
        if ((sub & 8) != 0) {
            std::uint8_t many = 0;
            static_cast<void>(data.byte(many));
            for (int one = 0; one < many; ++one) {
                std::array<std::uint8_t, 3> colour = m_foreground;
                if ((sub & 16) != 0) {
                    static_cast<void>(data.take(4, taken));
                    std::copy_n(taken.begin(), 3, colour.begin());
                }
                std::uint8_t position = 0;
                std::uint8_t size = 0;
                static_cast<void>(data.byte(position));
                static_cast<void>(data.byte(size));
                const int sx = position >> 4;
                const int sy = position & 15;
                const int sw = (size >> 4) + 1;
                const int sh = (size & 15) + 1;
                if (sx + sw > tileWidth || sy + sh > tileHeight) {
                    return fail(protocol, "a Hextile subrectangle outside its tile");
                }
                fill(x + sx, y + sy, sw, sh, colour);
                subrects += std::int64_t{sw} * sh;
            }
        }
        // The rectangle already counted this tile (tileWork, at `hextile` above), as the web counts Hextile. Only the
        // subrectangle fills beyond it are counted here, weighted as a fill (confirm review 4): 255 subrectangles of
        // 16x16 still draw 65,280 pixels from 520 bytes, so a flood is refused, while an honest whole-screen Hextile
        // counts about one screen an update, not two or three.
        if (const base::Result<void> counted = count(0, 0, 0, subrects / cheap); !counted) {
            return counted;
        }
    }
    consume(length);
    more = true;
    if (overBudget()) { // charges this tile's drawing against the share; ends only on a long step (round-2 review 5)
        return tooSlowToDraw();
    }
    if (++m_tile == across * ((m_rect.height + 15) / 16)) {
        changed(m_rect.x, m_rect.y, m_rect.width, m_rect.height);
        return rectDone();
    }
    return {};
}

base::Result<void> Client::zrleData(bool& more)
{
    const std::span<const std::uint8_t> got = there();
    if (got.size() < m_dataLength) {
        return {};
    }
    const int width = m_rect.width;
    const int height = m_rect.height;
    const std::int64_t tiles = std::int64_t{(width + 63) / 64} * static_cast<std::int64_t>((height + 63) / 64);
    const auto most = static_cast<std::size_t>(4 * std::int64_t{width} * height + 385 * tiles + 16);
    m_plain.clear();
    if (const base::Result<void> inflated = m_zrle.inflate(got.first(m_dataLength), most, m_plain, boundsFor(most));
        !inflated) {
        const std::string& code = inflated.error().code;
        if (code == "too_long") {
            return fail(limit, "a tile stream that inflates past what its rectangle can hold");
        }
        if (code == "too_costly") {
            return fail(limit, "a tile stream that takes far more work to unpack than a server has reason to send");
        }
        if (code == "too_slow") {
            return tooSlowToDraw();
        }
        return fail(protocol,
                    code == "ended" ? "a tile stream used after its end" : "a tile stream that does not inflate");
    }
    if (const base::Result<void> counted = count(0, 0, 0, static_cast<std::int64_t>(m_zrle.tableWorkTaken()),
                                                 static_cast<std::int64_t>(m_zrle.blocksTaken()));
        !counted) {
        return counted;
    }
    consume(m_dataLength);
    more = true;
    Cursor plain(m_plain);
    std::span<const std::uint8_t> taken;
    Bytes pixels;
    for (int top = 0; top < height; top += 64) {
        const int tileHeight = std::min(64, height - top);
        // The drawing of a screen of tiles is given a ceiling too (checked a row of tiles at a time): one made slow on
        // purpose ends the connection, the server at fault (finding 1).
        if (overBudget()) {
            return tooSlowToDraw();
        }
        for (int left = 0; left < width; left += 64) {
            const int tileWidth = std::min(64, width - left);
            const std::int64_t count = std::int64_t{tileWidth} * tileHeight;
            const auto bytes = static_cast<std::size_t>(3 * count);
            std::uint8_t sub = 0;
            if (!plain.byte(sub)) {
                return fail(protocol, "a rectangle was cut short");
            }
            const int x = m_rect.x + left;
            const int y = m_rect.y + top;
            if (sub == 0) {
                if (!plain.take(bytes, taken)) {
                    return fail(protocol, "a rectangle was cut short");
                }
                blit(x, y, tileWidth, tileHeight, taken);
                continue;
            }
            if (sub == 1) {
                if (!plain.take(3, taken)) {
                    return fail(protocol, "a rectangle was cut short");
                }
                fill(x, y, tileWidth, tileHeight, taken);
                continue;
            }
            if ((sub > 16 && sub < 128) || sub == 129) {
                return fail(protocol, "a ZRLE tile of a kind there is none of (" + std::to_string(sub) + ")");
            }
            const int colours = sub <= 16 ? sub : sub == 128 ? 0 : sub - 128;
            std::span<const std::uint8_t> palette;
            if (!plain.take(std::size_t{3} * static_cast<std::size_t>(colours), palette)) {
                return fail(protocol, "a rectangle was cut short");
            }
            pixels.resize(bytes);
            if (sub <= 16) {
                const int bits = sub == 2 ? 1 : sub <= 4 ? 2 : 4;
                const int perByte = 8 / bits;
                const auto rowBytes = static_cast<std::size_t>((tileWidth + perByte - 1) / perByte);
                if (!plain.take(rowBytes * static_cast<std::size_t>(tileHeight), taken)) {
                    return fail(protocol, "a rectangle was cut short");
                }
                for (int row = 0; row < tileHeight; ++row) {
                    for (int column = 0; column < tileWidth; ++column) {
                        const std::uint8_t byte = taken[static_cast<std::size_t>(row) * rowBytes +
                                                        static_cast<std::size_t>(column / perByte)];
                        const int index = (byte >> (8 - bits * (column % perByte + 1))) & ((1 << bits) - 1);
                        if (index >= colours) {
                            return fail(protocol, "an index past the palette");
                        }
                        std::copy_n(palette.begin() + std::ptrdiff_t{3} * index, 3,
                                    pixels.begin() + 3 * (static_cast<std::ptrdiff_t>(row) * tileWidth + column));
                    }
                }
                blit(x, y, tileWidth, tileHeight, pixels);
                continue;
            }
            // Runs: of a pixel each (128), or of a colour of the palette (130 and on).
            std::int64_t done = 0;
            while (done < count) {
                std::array<std::uint8_t, 3> pixel{};
                std::int64_t length = 1;
                if (sub == 128) {
                    if (!plain.take(3, taken)) {
                        return fail(protocol, "a rectangle was cut short");
                    }
                    std::copy_n(taken.begin(), 3, pixel.begin());
                    if (!runLength(plain, length)) {
                        return fail(protocol, "a rectangle was cut short");
                    }
                } else {
                    std::uint8_t index = 0;
                    if (!plain.byte(index)) {
                        return fail(protocol, "a rectangle was cut short");
                    }
                    if ((index & 0x80) != 0 && !runLength(plain, length)) {
                        return fail(protocol, "a rectangle was cut short");
                    }
                    index &= 0x7F;
                    if (index >= colours) {
                        return fail(protocol, "an index past the palette");
                    }
                    std::copy_n(palette.begin() + std::ptrdiff_t{3} * index, 3, pixel.begin());
                }
                if (length > count - done) {
                    return fail(protocol, "a run past the end of its tile");
                }
                for (std::int64_t one = 0; one < length; ++one, ++done) {
                    std::copy(pixel.begin(), pixel.end(), pixels.begin() + static_cast<std::ptrdiff_t>(3 * done));
                }
            }
            blit(x, y, tileWidth, tileHeight, pixels);
        }
    }
    if (plain.left() > 0) {
        return fail(protocol, "a tile stream longer than its tiles");
    }
    changed(m_rect.x, m_rect.y, width, height);
    m_plain.clear();
    return rectDone();
}

base::Result<void> Client::tightRect(bool& more)
{
    const std::span<const std::uint8_t> got = there();
    if (got.empty()) {
        return {};
    }
    const int width = m_rect.width;
    const int height = m_rect.height;
    const std::int64_t area = std::int64_t{width} * height;
    const std::uint8_t control = got[0];
    const int method = control >> 4;
    // Tight's length of one to three bytes, seven bits each, the eighth saying another follows: its value, and how
    // many bytes it took (none: they have not all come).
    const auto compact = [&got](std::size_t at, std::size_t& value) -> std::size_t {
        if (got.size() <= at) {
            return 0;
        }
        value = got[at] & 0x7FU;
        if ((got[at] & 0x80) == 0) {
            return 1;
        }
        if (got.size() <= at + 1) {
            return 0;
        }
        value |= std::size_t{got[at + 1] & 0x7FU} << 7;
        if ((got[at + 1] & 0x80) == 0) {
            return 2;
        }
        if (got.size() <= at + 2) {
            return 0;
        }
        value |= std::size_t{got[at + 2]} << 14;
        return 3;
    };
    const auto reset = [this, control] {
        for (std::size_t stream = 0; stream < 4; ++stream) {
            if ((control & (1U << stream)) != 0) {
                m_tight[stream] = Inflater();
            }
        }
    };
    if (method == 0x8) { // fill: one pixel for all of it
        if (got.size() < 4) {
            return {};
        }
        if (const base::Result<void> counted = count(0, 0, 0, std::max(area / cheap, rectangleWork)); !counted) {
            return counted;
        }
        reset();
        fill(m_rect.x, m_rect.y, width, height, got.subspan(1, 3));
        changed(m_rect.x, m_rect.y, width, height);
        consume(4);
        more = true;
        return rectDone();
    }
    if (method == 0x9) { // JPEG
        std::size_t length = 0;
        const std::size_t said = compact(1, length);
        if (said == 0) {
            return {};
        }
        if (length > static_cast<std::size_t>(3 * area) + zlibSlack) {
            return fail(limit, "a JPEG of " + std::to_string(length) + " bytes for a rectangle of " +
                                   std::to_string(width) + " by " + std::to_string(height) +
                                   ", which is more than it could hold");
        }
        if (got.size() < 1 + said + length) {
            return {};
        }
        if (!m_options.jpeg) {
            return fail(protocol, "a Tight JPEG, which this program did not ask for");
        }
        // A JPEG counts a fill's eighth towards the per-second cap, as the web does (a JPEG is decoded in C, far
        // cheaper a byte than a plain rectangle): obmc-ikvm sends every frame as one whole-screen JPEG, about thirty a
        // second, so the cap must not refuse an honest console. The drawing share, charged after the decode below,
        // holds a flood back (round-2 review 1 and 2).
        if (const base::Result<void> counted = count(0, 0, 0, std::max(area / cheap, rectangleWork)); !counted) {
            return counted;
        }
        reset();
        base::Result<Bytes> picture = m_options.jpeg(got.subspan(1 + said, length), width, height);
        if (!picture) {
            // "too_slow" (a decode that ran over its time, or a frame the decoder refuses as too costly) ends the
            // connection as the server's fault; anything else is a picture that is no picture (finding 3).
            if (picture.error().code == "too_slow") {
                return tooSlowToDraw();
            }
            return fail(protocol, "a JPEG that cannot be read (" + picture.error().message + ")");
        }
        if (picture.value().size() != static_cast<std::size_t>(3 * area)) {
            return fail(protocol, "a JPEG that is not of its rectangle's size");
        }
        // The decode's processor time is charged against the drawing share (round-2 review 2): a server that sends
        // JPEGs faster than a thread can decode them is shown more slowly, the screen holding its line back after the
        // batch (pausedForMs); overBudget ends it only where its single decodes take a long step again and again.
        if (overBudget()) {
            return tooSlowToDraw();
        }
        blit(m_rect.x, m_rect.y, width, height, picture.value());
        changed(m_rect.x, m_rect.y, width, height);
        consume(1 + said + length);
        more = true;
        return rectDone();
    }
    if (method > 0x9) {
        return fail(protocol,
                    "a Tight rectangle of a kind this program does not read (method " + std::to_string(method) + ")");
    }
    std::size_t at = 1;
    int filter = 0;
    if ((control & 0x40) != 0) {
        if (got.size() <= at) {
            return {};
        }
        filter = got[at++];
    }
    int colours = 0;
    std::span<const std::uint8_t> palette;
    std::size_t plain = 0;
    if (filter == 1) { // palette
        if (got.size() <= at) {
            return {};
        }
        colours = got[at++] + 1;
        if (got.size() < at + std::size_t{3} * static_cast<std::size_t>(colours)) {
            return {};
        }
        palette = got.subspan(at, std::size_t{3} * static_cast<std::size_t>(colours));
        at += palette.size();
        plain = static_cast<std::size_t>(colours <= 2 ? (width + 7) / 8 : width) * static_cast<std::size_t>(height);
    } else if (filter == 0 || filter == 2) { // copy, gradient
        plain = static_cast<std::size_t>(3 * area);
    } else {
        return fail(protocol, "a Tight filter there is none of (" + std::to_string(filter) + ")");
    }
    std::size_t length = plain;
    if (plain >= 12) {
        const std::size_t said = compact(at, length);
        if (said == 0) {
            return {};
        }
        if (length > plain + zlibSlack) {
            return fail(limit, "a Tight stream of " + std::to_string(length) + " bytes for " + std::to_string(plain) +
                                   " bytes of pixels, which is more than it could hold");
        }
        at += said;
    }
    if (got.size() < at + length) {
        return {};
    }
    const std::int64_t cost = filter == 2 ? area * gradientCost : area;
    if (const base::Result<void> counted = count(0, 0, 0, std::max(cost, rectangleWork)); !counted) {
        return counted;
    }
    reset();
    const std::span<const std::uint8_t> data = got.subspan(at, length);
    std::span<const std::uint8_t> pixels = data;
    if (plain >= 12) {
        m_plain.clear();
        Inflater& stream = m_tight[static_cast<std::size_t>((control >> 4) & 3)];
        if (const base::Result<void> inflated = stream.inflate(data, plain, m_plain, boundsFor(plain)); !inflated) {
            const std::string& code = inflated.error().code;
            if (code == "too_long") {
                return fail(limit, "a Tight stream that inflates past what its rectangle can hold");
            }
            if (code == "too_costly") {
                return fail(limit,
                            "a Tight stream that takes far more work to unpack than a server has reason to send");
            }
            if (code == "too_slow") {
                return tooSlowToDraw();
            }
            return fail(protocol,
                        code == "ended" ? "a Tight stream used after its end" : "a Tight stream that does not inflate");
        }
        if (const base::Result<void> counted = count(0, 0, 0, static_cast<std::int64_t>(stream.tableWorkTaken()),
                                                     static_cast<std::int64_t>(stream.blocksTaken()));
            !counted) {
            return counted;
        }
        if (m_plain.size() < plain) {
            return fail(protocol, "a Tight stream that holds less than its rectangle");
        }
        pixels = m_plain;
    }
    Bytes rgb(static_cast<std::size_t>(3 * area));
    if (filter == 1 && colours <= 2) {
        const auto rowBytes = static_cast<std::size_t>((width + 7) / 8);
        for (int row = 0; row < height; ++row) {
            for (int column = 0; column < width; ++column) {
                const std::uint8_t byte =
                    pixels[static_cast<std::size_t>(row) * rowBytes + static_cast<std::size_t>(column / 8)];
                const int index = (byte >> (7 - column % 8)) & 1;
                if (index >= colours) {
                    return fail(protocol, "an index past the palette");
                }
                std::copy_n(palette.begin() + std::ptrdiff_t{3} * index, 3,
                            rgb.begin() + 3 * (static_cast<std::ptrdiff_t>(row) * width + column));
            }
        }
    } else if (filter == 1) {
        for (std::int64_t pixel = 0; pixel < area; ++pixel) {
            const int index = pixels[static_cast<std::size_t>(pixel)];
            if (index >= colours) {
                return fail(protocol, "an index past the palette");
            }
            std::copy_n(palette.begin() + std::ptrdiff_t{3} * index, 3,
                        rgb.begin() + static_cast<std::ptrdiff_t>(3 * pixel));
        }
    } else if (filter == 2) {
        // Tight's gradient undone: each sample was sent as its difference from a guess made of its neighbours.
        const std::size_t row = std::size_t{3} * static_cast<std::size_t>(width);
        for (int line = 0; line < height; ++line) {
            if ((line & 255) == 0 && overBudget()) { // the gradient is the costly filter: checked as it goes
                return tooSlowToDraw();
            }
            const std::size_t start = static_cast<std::size_t>(line) * row;
            std::array<int, 3> before{};
            for (std::size_t sample = 0; sample < row; sample += 3) {
                for (std::size_t colour = 0; colour < 3; ++colour) {
                    const int up = line > 0 ? rgb[start - row + sample + colour] : 0;
                    const int upLeft = line > 0 && sample > 0 ? rgb[start - row + sample + colour - 3] : 0;
                    const int guess = std::clamp(before[colour] + up - upLeft, 0, 255);
                    const auto value = static_cast<std::uint8_t>(pixels[start + sample + colour] + guess);
                    rgb[start + sample + colour] = value;
                    before[colour] = value;
                }
            }
        }
    } else {
        std::copy_n(pixels.begin(), rgb.size(), rgb.begin());
    }
    if (pixels.size() > plain) {
        return fail(protocol, "a Tight rectangle longer than its pixels");
    }
    if (overBudget()) { // charges this rectangle's drawing (palette, gradient or copy) against the share
        return tooSlowToDraw();
    }
    blit(m_rect.x, m_rect.y, width, height, rgb);
    changed(m_rect.x, m_rect.y, width, height);
    consume(at + length);
    m_plain.clear();
    more = true;
    return rectDone();
}

} // namespace qkvm::rfb
