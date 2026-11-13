#include "allocations.h"
#include "aspeed_frames.h"
#include "chance.h"

#include <QElapsedTimer>
#include <QTest>
#include <qkvm/bmc/aspeed.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <vector>

// What a controller in the wrong hands could send, made by chance and the same at every run: frames the tests'
// own writer made, then cut short, with bits flipped, bytes put in and taken out, lengths that say anything, and
// every field of the header at its edges. Every frame read here is held to the reader's promises: it never reads
// or writes outside what it was given or what it holds (the address sanitizer's build looks at every access;
// the debug build at every one of the standard library's), never asks for more room than a screen of its size
// needs, and never takes longer than its screen's worth of the costliest blocks there are.

using namespace qkvm::bmc::aspeed;
using frames::Coefficients;
using frames::Frame;
using qkvm::bmc::tests::asked;
using qkvm::bmc::tests::Chance;
using qkvm::bmc::tests::Watch;

namespace {

bool sayable(const FrameHeader& header)
{
    return 0 < header.width && header.width <= largest && 0 < header.height && header.height <= largest &&
           0 <= header.strength && header.strength < 8 && 0 <= header.correctionStrength &&
           header.correctionStrength < 8;
}

std::int64_t blocks(int length, int side)
{
    return (std::int64_t{length} + side - 1) / side;
}

// The blocks a frame of this header may bring.
std::int64_t allowedFor(const FrameHeader& header)
{
    const int side = header.halfColour ? 16 : 8;
    return 2 * blocks(header.width, side) * blocks(header.height, side) + spare;
}

// What a screen of this header's size may hold: its picture, and 192 bytes an 8x8 block.
std::size_t roomFor(const FrameHeader& header)
{
    return 3 * static_cast<std::size_t>(header.width) * static_cast<std::size_t>(header.height) +
           24 * static_cast<std::size_t>(blocks(header.height, 8)) +
           (header.halfColour ? 0
                              : 192 * static_cast<std::size_t>(blocks(header.width, 8)) *
                                    static_cast<std::size_t>(blocks(header.height, 8)));
}

// A plane of coefficients the writer can write: a first one near the last, and a few others.
Coefficients someCoefficients(Chance& chance, int busy)
{
    Coefficients found{};
    found[0] = chance.between(-300, 300);
    for (std::size_t at = 1; at < 64; ++at) {
        if (chance.below(100) < busy) {
            found[at] = chance.between(-1023, 1023) >> chance.below(9);
        }
    }
    return found;
}

std::uint32_t someColour(Chance& chance)
{
    return static_cast<std::uint32_t>(chance.next() & 0xFFFFFF);
}

frames::Place somePlace(Chance& chance, const FrameHeader& header)
{
    if (chance.below(3)) {
        return {};
    }
    const int side = header.halfColour ? 16 : 8;
    if (chance.oneIn(5)) {
        return std::pair{chance.below(256), chance.below(256)}; // anywhere, the screen or not
    }
    return std::pair{chance.below(static_cast<int>(std::min<std::int64_t>(256, blocks(header.width, side)))),
                     chance.below(static_cast<int>(std::min<std::int64_t>(256, blocks(header.height, side))))};
}

// A frame of every kind of block, of a size and a strength by chance.
Frame someFrame(Chance& chance, int widest, int tallest)
{
    const frames::Options options{chance.oneIn(4), chance.below(8), chance.below(8), chance.oneIn(2)};
    const int width = chance.between(1, widest);
    const int height = chance.between(1, tallest);
    Frame frame(width, height, options);
    const FrameHeader& header = frame.header();
    const auto most = static_cast<int>(std::min<std::int64_t>(allowedFor(header), 600)) + 1;
    const int many = chance.below(most);
    for (int block = 0; block < many; ++block) {
        const frames::Place at = somePlace(chance, header);
        const int kind = chance.below(10);
        const int busy = chance.below(4) ? chance.below(20) : 100;
        if (kind < 3) {
            const bool fine = chance.oneIn(4);
            const bool correction = chance.oneIn(4);
            if (options.halfColour && !correction) {
                std::array<Coefficients, 4> ys{};
                for (Coefficients& y : ys) {
                    y = someCoefficients(chance, busy);
                }
                const Coefficients cb = someCoefficients(chance, busy);
                const Coefficients cr = someCoefficients(chance, busy);
                frame.jpeg(ys, cb, cr, at, fine);
            } else {
                const Coefficients y = someCoefficients(chance, busy);
                const Coefficients cb = someCoefficients(chance, busy);
                const Coefficients cr = someCoefficients(chance, busy);
                frame.jpeg(y, cb, cr, at, fine, correction);
            }
        } else {
            const std::size_t listed = kind < 6 ? 1 : kind < 8 ? 2 : 4;
            std::vector<frames::Listed> colours;
            colours.reserve(listed);
            for (std::size_t one = 0; one < listed; ++one) {
                frames::Listed entry{chance.below(4), std::nullopt};
                if (chance.oneIn(2)) {
                    entry.colour = someColour(chance);
                }
                colours.push_back(entry);
            }
            std::vector<int> pixels;
            pixels.reserve(64);
            if (listed > 1) {
                for (int pixel = 0; pixel < 64; ++pixel) {
                    pixels.push_back(chance.below(static_cast<int>(listed)));
                }
            }
            frame.colours(colours, pixels, at);
        }
    }
    return frame;
}

// What a field of a header may say, at the edges of what it means and of what it can hold.
const std::vector<int> sizesAtEdges = {INT_MIN, -1,   0,    1,    2,    7,    8,     9,     15,     16,
                                       17,      2040, 2047, 2048, 2049, 4096, 65535, 70000, INT_MAX};
const std::vector<int> gridsAtEdges = {INT_MIN, -1, 0, 1, 7, 8, 9, 16, 2048, 65535, INT_MAX};
const std::vector<int> strengthsAtEdges = {INT_MIN, -1, 0, 1, 6, 7, 8, 255, INT_MAX};

FrameHeader someHeaderAtEdges(Chance& chance, FrameHeader header)
{
    switch (chance.below(8)) {
    case 0:
        header.width = sizesAtEdges[static_cast<std::size_t>(chance.below(static_cast<int>(sizesAtEdges.size())))];
        break;
    case 1:
        header.height = sizesAtEdges[static_cast<std::size_t>(chance.below(static_cast<int>(sizesAtEdges.size())))];
        break;
    case 2:
        header.gridWidth = gridsAtEdges[static_cast<std::size_t>(chance.below(static_cast<int>(gridsAtEdges.size())))];
        break;
    case 3:
        header.gridHeight = gridsAtEdges[static_cast<std::size_t>(chance.below(static_cast<int>(gridsAtEdges.size())))];
        break;
    case 4:
        header.strength =
            strengthsAtEdges[static_cast<std::size_t>(chance.below(static_cast<int>(strengthsAtEdges.size())))];
        break;
    case 5:
        header.correctionStrength =
            strengthsAtEdges[static_cast<std::size_t>(chance.below(static_cast<int>(strengthsAtEdges.size())))];
        break;
    case 6:
        header.halfColour = !header.halfColour;
        break;
    default:
        header.lumaForColour = !header.lumaForColour;
        break;
    }
    return header;
}

// The frame, damaged in one way or a few.
std::vector<std::uint8_t> damaged(Chance& chance, std::vector<std::uint8_t> data)
{
    const int ways = chance.between(1, 3);
    for (int way = 0; way < ways; ++way) {
        const auto length = static_cast<int>(data.size());
        switch (chance.below(8)) {
        case 0: // cut short, anywhere
            data.resize(static_cast<std::size_t>(chance.below(length + 1)));
            break;
        case 1: // bits flipped
            for (int flip = chance.between(1, 16); flip > 0 && length; --flip) {
                data[static_cast<std::size_t>(chance.below(length))] ^=
                    static_cast<std::uint8_t>(1u << chance.below(8));
            }
            break;
        case 2: { // a run of bytes made all ones, all zeros, or anything
            if (!length) {
                break;
            }
            const int from = chance.below(length);
            const int run = std::min(length - from, chance.between(1, 64));
            const int fill = chance.below(3);
            for (int at = from; at < from + run; ++at) {
                data[static_cast<std::size_t>(at)] = static_cast<std::uint8_t>(fill == 0   ? 0xFF
                                                                               : fill == 1 ? 0
                                                                                           : chance.below(256));
            }
            break;
        }
        case 3: { // bytes put in
            const int where = chance.below(length + 1);
            std::vector<std::uint8_t> more(static_cast<std::size_t>(chance.between(1, 64)));
            for (std::uint8_t& byte : more) {
                byte = static_cast<std::uint8_t>(chance.below(256));
            }
            data.insert(data.begin() + where, more.begin(), more.end());
            break;
        }
        case 4: { // bytes taken out
            if (!length) {
                break;
            }
            const int from = chance.below(length);
            const int run = std::min(length - from, chance.between(1, 64));
            data.erase(data.begin() + from, data.begin() + from + run);
            break;
        }
        case 5: { // a word that says something huge (where a length or a place would be, if there were one)
            if (length < 4) {
                break;
            }
            static constexpr std::array<std::uint32_t, 5> huge = {0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x00800000,
                                                                  0x0000FFFF};
            const std::uint32_t word = huge[static_cast<std::size_t>(chance.below(5))];
            const auto at = static_cast<std::size_t>(chance.below(length / 4)) * 4;
            for (std::size_t byte = 0; byte < 4; ++byte) {
                data[at + byte] = static_cast<std::uint8_t>(word >> (8 * byte));
            }
            break;
        }
        case 6: // the same over again
            if (data.size() < std::size_t{64} * 1024) {
                const std::vector<std::uint8_t> again = data;
                data.insert(data.end(), again.begin(), again.end());
            }
            break;
        default: // its last words taken off (where the end mark is)
            data.resize(data.size() - std::min(data.size(), static_cast<std::size_t>(chance.between(1, 16))));
            break;
        }
    }
    return data;
}

// How long the costliest block there is takes here, measured once: a frame of 160 by 120 of blocks whose three
// planes have every coefficient there is, as many as the screen may have.
double costliestBlockMs = 0;

Frame costliest(int width, int height)
{
    Frame frame(width, height);
    Coefficients full{};
    for (std::size_t at = 0; at < 64; ++at) {
        full[at] = at % 2 ? 1 : -1;
    }
    for (std::int64_t block = 0; block < allowedFor(frame.header()); ++block) {
        frame.jpeg(full, full, full);
    }
    return frame;
}

// How long a frame may take: its screen's worth of the costliest blocks, eight times over, and a quarter of a second
// for a computer that is busy with other things (a frame of a few blocks once took 60 ms on a loaded PC).
double mayTakeMs(const FrameHeader& header)
{
    return 250 + 8 * costliestBlockMs * static_cast<double>(sayable(header) ? allowedFor(header) : 0);
}

struct Seen {
    int reads = 0;
    int refused = 0;
    double longestMs = 0;
    double worstShare = 0; // of what a frame may take, the most one took
    std::size_t mostAsked = 0;
};

// Reads one frame into `screen` and holds it to every promise. Empty when it kept them all.
QByteArray readHeld(Screen& screen, const FrameHeader& header, std::span<const std::uint8_t> data, Seen& seen)
{
    const auto began = std::chrono::steady_clock::now();
    const auto result = [&] {
        const Watch watch;
        return screen.read(header, data);
    }();
    const double tookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    ++seen.reads;
    seen.longestMs = std::max(seen.longestMs, tookMs);
    seen.worstShare = std::max(seen.worstShare, tookMs / mayTakeMs(header));
    seen.mostAsked = std::max(seen.mostAsked, asked);
    const QByteArray what = "frame of " + QByteArray::number(header.width) + " by " +
                            QByteArray::number(header.height) + ", " +
                            QByteArray::number(static_cast<qsizetype>(data.size())) + " bytes: ";
    if (result.ok()) {
        if (!sayable(header) || data.size() > longest) {
            return what + "read, though its header or its length is impossible";
        }
        if (result.value() < 0 || result.value() > allowedFor(header)) {
            return what + "changed " + QByteArray::number(result.value()) + " blocks";
        }
    } else {
        ++seen.refused;
        if (result.error().code != "garbled" && result.error().code != "given_up") {
            return what + "refused as " + QByteArray::fromStdString(result.error().code);
        }
    }
    const Picture& picture = screen.picture();
    if (picture.width < 0 || picture.width > largest || picture.height < 0 || picture.height > largest ||
        picture.rgb.size() != 3 * static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height)) {
        return what + "the picture is not a whole picture any more";
    }
    // Room: a new screen's picture and what is kept of its blocks, and a sentence of why it was refused. (A screen
    // that stays the size it is asks for none: framesMadeAndDamaged.)
    const std::size_t mayAsk = (sayable(header) ? roomFor(header) : 0) + 1024;
    if (asked > mayAsk) {
        return what + "asked for " + QByteArray::number(static_cast<qulonglong>(asked)) + " bytes, " +
               QByteArray::number(static_cast<qulonglong>(mayAsk)) + " at most";
    }
    // What changed: rows of the screen top to bottom, each on it.
    int below = 0;
    for (const Rect& one : screen.changed()) {
        if (one.width <= 0 || one.height <= 0 || one.x < 0 || one.y < below || one.x + one.width > picture.width ||
            one.y + one.height > picture.height) {
            return what + "it says a part changed that is no part of the screen, or not in order";
        }
        below = one.y + one.height;
    }
    if (screen.held() >
        3 * static_cast<std::size_t>(largest) * largest + std::size_t{192} * 256 * 256 + std::size_t{24} * 256) {
        return what + "the screen holds " + QByteArray::number(static_cast<qulonglong>(screen.held())) + " bytes";
    }
    if (tookMs > mayTakeMs(header)) {
        return what + "took " + QByteArray::number(tookMs) + " ms, " + QByteArray::number(mayTakeMs(header)) +
               " at most";
    }
    return {};
}

void say(const char* part, const Seen& seen)
{
    qInfo("%s: %d frames read, %d refused; the longest took %.1f ms (%.0f%% of what it might); at most %llu bytes "
          "asked for in one",
          part, seen.reads, seen.refused, seen.longestMs, 100 * seen.worstShare,
          static_cast<unsigned long long>(seen.mostAsked));
}

} // namespace

class TestAspeedFuzz : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // The tables of the standard are made at the first frame of the program: not counted against any frame.
        Frame first(8, 8);
        first.colours({{0, 0x108080}});
        QVERIFY(Screen().read(first.header(), first.data()).ok());
        const Frame frame = costliest(160, 120);
        const auto data = frame.data();
        double best = 1e9;
        for (int time = 0; time < 3; ++time) {
            Screen screen;
            QElapsedTimer took;
            took.start();
            const auto read = screen.read(frame.header(), data);
            best = std::min(best, static_cast<double>(took.nsecsElapsed()) / 1e6);
            QVERIFY(read.ok() && read.value() == allowedFor(frame.header()));
        }
        costliestBlockMs = best / static_cast<double>(allowedFor(frame.header()));
        qInfo("the costliest block takes %.4f ms here", costliestBlockMs);
    }

    void everyFieldOfTheHeaderAtItsEdges()
    {
        // One field at a time, the others as a frame made for them says; then two or three at a time by chance.
        Chance chance(20261029);
        Seen seen;
        const Frame made = someFrame(chance, 64, 48);
        const auto madeData = made.data();
        std::vector<std::uint8_t> noise(4096);
        for (std::uint8_t& byte : noise) {
            byte = static_cast<std::uint8_t>(chance.below(256));
        }
        const std::vector<std::vector<std::uint8_t>> datas = {madeData, std::vector<std::uint8_t>(64), noise, {}};
        auto each = [&](const FrameHeader& header) {
            for (const auto& data : datas) {
                Screen fresh;
                QByteArray wrong = readHeld(fresh, header, data, seen);
                QVERIFY2(wrong.isEmpty(), wrong);
                Screen used;
                (void)used.read(made.header(), madeData);
                wrong = readHeld(used, header, data, seen);
                QVERIFY2(wrong.isEmpty(), wrong);
            }
        };
        for (const int size : sizesAtEdges) {
            FrameHeader header = made.header();
            header.width = size;
            each(header);
            if (QTest::currentTestFailed()) {
                return;
            }
            header = made.header();
            header.height = size;
            each(header);
            if (QTest::currentTestFailed()) {
                return;
            }
        }
        for (const int grid : gridsAtEdges) {
            FrameHeader header = made.header();
            header.gridWidth = grid;
            each(header);
            header = made.header();
            header.gridHeight = grid;
            each(header);
            if (QTest::currentTestFailed()) {
                return;
            }
        }
        for (const int strength : strengthsAtEdges) {
            FrameHeader header = made.header();
            header.strength = strength;
            each(header);
            header = made.header();
            header.correctionStrength = strength;
            each(header);
            if (QTest::currentTestFailed()) {
                return;
            }
        }
        for (int time = 0; time < 300; ++time) {
            FrameHeader header = made.header();
            for (int field = chance.between(2, 3); field > 0; --field) {
                header = someHeaderAtEdges(chance, header);
            }
            if (static_cast<std::int64_t>(header.width) * header.height > std::int64_t{512} * 512) {
                header.height = std::min(header.height, 8); // (a large screen at a time is enough: see below)
            }
            each(header);
            if (QTest::currentTestFailed()) {
                return;
            }
        }
        FrameHeader both = made.header();
        both.width = largest;
        both.height = largest;
        each(both);
        say("headers at their edges", seen);
    }

    void lengthsAtTheirEdges()
    {
        Seen seen;
        Chance chance(7);
        // As long as its screen may take, and a byte more; in nothing, and in blocks of nothing much.
        for (const auto& [width, height] : {std::pair{1, 1}, std::pair{8, 8}, std::pair{21, 11}, std::pair{64, 48}}) {
            const std::size_t most =
                2 * (3 * static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) + slack;
            for (const std::size_t length : {most - 1, most, most + 1}) {
                for (const bool half : {false, true}) {
                    const FrameHeader header{width, height, width, height, half, 3, 5, false};
                    std::vector<std::uint8_t> data(length);
                    Screen screen;
                    QByteArray wrong = readHeld(screen, header, data, seen);
                    QVERIFY2(wrong.isEmpty(), wrong);
                    for (std::uint8_t& byte : data) {
                        byte = static_cast<std::uint8_t>(chance.below(256));
                    }
                    wrong = readHeld(screen, header, data, seen);
                    QVERIFY2(wrong.isEmpty(), wrong);
                    const auto read = Screen().read(header, data);
                    QCOMPARE(read.ok() || read.error().message != "a frame longer than its screen could take",
                             length <= most);
                }
            }
        }
        // As long as any frame may be, and a byte more, for the largest screen there is (each ended at once:
        // what it costs to read is another test's).
        const FrameHeader largestScreen{largest, largest, largest, largest, false, 0, 7, true};
        for (const std::size_t length : {longest - 1, longest, longest + 1}) {
            std::vector<std::uint8_t> data(length);
            data[3] = 0x90;
            Screen screen;
            const QByteArray wrong = readHeld(screen, largestScreen, data, seen);
            QVERIFY2(wrong.isEmpty(), wrong);
            QCOMPARE(screen.picture().width, length <= longest ? largest : 0);
        }
        say("lengths at their edges", seen);
    }

    void framesMadeAndDamaged()
    {
        // Frames made by the tests' own writer, read whole (each as the screen it was made for), and damaged
        // every which way, into screens that have seen other frames before (what a screen keeps of earlier
        // frames is what a damaged one finds), and into new ones.
        Chance chance(1);
        Seen seen;
        auto kept = std::make_unique<Screen>();
        for (int round = 0; round < 500; ++round) {
            const Frame frame = someFrame(chance, chance.oneIn(10) ? 640 : 200, chance.oneIn(10) ? 480 : 120);
            const auto whole = frame.data();
            {
                Screen fresh;
                const QByteArray wrong = readHeld(fresh, frame.header(), whole, seen);
                QVERIFY2(wrong.isEmpty(), wrong);
                // A whole frame is read, unless it is longer than its screen could take (a tiny screen of its
                // most blocks, each with every coefficient there is).
                const auto again = Screen().read(frame.header(), whole);
                const std::size_t pixels = static_cast<std::size_t>(frame.header().width) * frame.header().height;
                if (whole.size() > 2 * (3 * pixels) + slack) {
                    QVERIFY(!again.ok() && again.error().message == "a frame longer than its screen could take");
                } else {
                    QVERIFY2(again.ok(), again.ok() ? "" : again.error().message.c_str());
                    // ...and a screen that stays the size it is asks for no room at all.
                    const auto steady = [&] {
                        const Watch watch;
                        return fresh.read(frame.header(), whole);
                    }();
                    QVERIFY(steady.ok());
                    QCOMPARE(asked, std::size_t{0});
                }
            }
            for (int time = 0; time < 6; ++time) {
                FrameHeader header = frame.header();
                if (chance.oneIn(4)) {
                    header = someHeaderAtEdges(chance, header);
                    if (static_cast<std::int64_t>(header.width) * header.height > std::int64_t{1024} * 1024) {
                        header.height = std::min(header.height, 16);
                    }
                }
                const auto data = damaged(chance, whole);
                QByteArray wrong = readHeld(*kept, header, data, seen);
                QVERIFY2(wrong.isEmpty(), wrong);
                Screen fresh;
                wrong = readHeld(fresh, header, data, seen);
                QVERIFY2(wrong.isEmpty(), wrong);
            }
            if (chance.oneIn(50)) {
                kept = std::make_unique<Screen>();
            }
        }
        say("frames made and damaged", seen);
    }

    void noFrameCostsMoreThanItsScreensWorth()
    {
        // The costliest frames there are, for screens of a few sizes: every block with every coefficient, as many
        // as the screen may have, and nothing at all (which reads as the costliest of blocks there are of
        // brightness). Each within its bound; and one block more is not read.
        Seen seen;
        for (const auto& [width, height] : {std::pair{16, 16}, std::pair{101, 59}, std::pair{320, 240}}) {
            const Frame frame = costliest(width, height);
            Screen screen;
            QByteArray wrong = readHeld(screen, frame.header(), frame.data(), seen);
            QVERIFY2(wrong.isEmpty(), wrong);
            const auto all = Screen().read(frame.header(), frame.data());
            QVERIFY(all.ok() && all.value() == allowedFor(frame.header()));
            Frame more = costliest(width, height);
            Coefficients full{};
            full.fill(1);
            more.jpeg(full, full, full);
            const auto refused = screen.read(more.header(), more.data());
            QVERIFY(!refused.ok());
            for (const bool half : {false, true}) {
                const FrameHeader header{width, height, width, height, half, 0, 0, false};
                const std::vector<std::uint8_t> nothing(
                    2 * (3 * static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) + slack);
                wrong = readHeld(screen, header, nothing, seen);
                QVERIFY2(wrong.isEmpty(), wrong);
            }
        }
        say("the costliest frames", seen);
    }
};

QTEST_GUILESS_MAIN(TestAspeedFuzz)
#include "test_aspeed_fuzz.moc"
