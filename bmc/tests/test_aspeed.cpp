#include "aspeed_frames.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTest>
#include <qkvm/bmc/aspeed.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <vector>

// The picture a management controller sends: ASPEED's video format, read. The web's tests/test_aspeed.py,
// test for test, and a few of the desktop's own where a bound of the web's had no test that names it.
using namespace qkvm::bmc::aspeed;
using frames::Coefficients;
using frames::Frame;
using frames::Numbers;

namespace {

// Colours as video has them: brightness, blue difference, red difference, in one number.
constexpr std::uint32_t white = 0xEB8080;
constexpr std::uint32_t black = 0x108080;
constexpr std::uint32_t red = 0x515AF0;
constexpr std::uint32_t green = 0x913622;
constexpr std::uint32_t blue = 0x29F06E;
const Coefficients flat{};

using Rgb = std::array<int, 3>;

Rgb at(const Screen& screen, int x, int y)
{
    const Picture& picture = screen.picture();
    const std::size_t start =
        3 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(picture.width) + static_cast<std::size_t>(x));
    return {picture.rgb[start], picture.rgb[start + 1], picture.rgb[start + 2]};
}

bool near(const Rgb& colour, const Rgb& wanted, int within = 3)
{
    for (std::size_t one = 0; one < 3; ++one) {
        if (std::abs(colour[one] - wanted[one]) > within) {
            return false;
        }
    }
    return true;
}

QByteArray said(const Rgb& colour)
{
    return QByteArray::number(colour[0]) + ',' + QByteArray::number(colour[1]) + ',' + QByteArray::number(colour[2]);
}

// The coefficients of a plane that is one shade all over.
Coefficients shade(int level)
{
    Coefficients found{};
    found[0] = level;
    return found;
}

std::unique_ptr<Screen> read(std::initializer_list<const Frame*> frames)
{
    auto screen = std::make_unique<Screen>();
    for (const Frame* frame : frames) {
        const auto data = frame->data();
        (void)screen->read(frame->header(), data);
    }
    return screen;
}

QByteArray sha256(const Picture& picture)
{
    return QCryptographicHash::hash(QByteArrayView(picture.rgb.data(), static_cast<qsizetype>(picture.rgb.size())),
                                    QCryptographicHash::Sha256)
        .toHex();
}

// Three frames with every kind of block in them, many times over (the web's a_mixed_screen, number for number:
// every number is drawn in the order the web draws it).
std::vector<Frame> aMixedScreen()
{
    Numbers below(2026);
    auto plane = [&below](int most, int busy) {
        Coefficients found{};
        found[0] = below(2 * most + 1) - most;
        for (std::size_t position = 1; position < 64; ++position) {
            if (below(100) < busy) {
                found[position] = below(61) - 30;
            }
        }
        return found;
    };
    auto colour = [&below] {
        const auto y = static_cast<std::uint32_t>(below(256));
        const auto cb = static_cast<std::uint32_t>(below(256));
        const auto cr = static_cast<std::uint32_t>(below(256));
        return (y << 16) | (cb << 8) | cr;
    };
    auto listed = [&below, &colour](int many, int newOneIn) {
        std::vector<frames::Listed> found;
        for (int one = 0; one < many; ++one) {
            frames::Listed entry{below(4), std::nullopt};
            if (below(newOneIn)) {
                entry.colour = colour();
            }
            found.push_back(entry);
        }
        return found;
    };
    std::vector<Frame> made;
    for (int number = 0; number < 3; ++number) {
        const int strength = below(8);
        const int correctionStrength = below(8);
        Frame frame(101, 59,
                    {.strength = strength, .correctionStrength = correctionStrength, .lumaForColour = number == 1});
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 13; ++x) {
                if (number && below(3)) {
                    continue; // a later frame leaves most of the screen as it is
                }
                frames::Place where;
                if (number || below(4) == 0) {
                    where = std::pair{x, y};
                }
                const int kind = below(10);
                if (kind < 4) {
                    const int busyY = below(30);
                    const Coefficients py = plane(80, busyY);
                    const int busyCb = below(12);
                    const Coefficients pcb = plane(40, busyCb);
                    const int busyCr = below(12);
                    const Coefficients pcr = plane(40, busyCr);
                    const bool fine = below(5) == 0;
                    frame.jpeg(py, pcb, pcr, where, fine);
                } else if (kind == 4 && number) {
                    const int busyY = below(10);
                    const Coefficients py = plane(15, busyY);
                    const int busyCb = below(5);
                    const Coefficients pcb = plane(8, busyCb);
                    const int busyCr = below(5);
                    const Coefficients pcr = plane(8, busyCr);
                    frame.jpeg(py, pcb, pcr, where, false, true);
                } else if (kind < 7) {
                    frame.colours(listed(1, 3), {}, where);
                } else if (kind < 9) {
                    const auto two = listed(2, 2);
                    std::vector<int> pixels;
                    pixels.reserve(64);
                    for (int pixel = 0; pixel < 64; ++pixel) {
                        pixels.push_back(below(2));
                    }
                    frame.colours(two, pixels, where);
                } else {
                    const auto four = listed(4, 2);
                    std::vector<int> pixels;
                    pixels.reserve(64);
                    for (int pixel = 0; pixel < 64; ++pixel) {
                        pixels.push_back(below(4));
                    }
                    frame.colours(four, pixels, where);
                }
            }
        }
        made.push_back(frame);
    }
    return made;
}

std::vector<Frame> aCoarseScreen()
{
    Numbers below(7);
    const int strength = below(8);
    Frame frame(96, 48, {.halfColour = true, .strength = strength});
    for (int block = 0; block < 18; ++block) {
        std::array<Coefficients, 4> ys{};
        for (Coefficients& y : ys) {
            y[0] = below(121) - 60;
            for (std::size_t position = 1; position < 64; ++position) {
                y[position] = below(10) == 0 ? below(41) - 20 : 0;
            }
        }
        Coefficients cb{};
        cb[0] = below(61) - 30;
        Coefficients cr{};
        cr[0] = below(61) - 30;
        frame.jpeg(ys, cb, cr);
    }
    return {frame};
}

FrameHeader usual()
{
    return {64, 48, 64, 48, false, 3, 5, false};
}

} // namespace

Q_DECLARE_METATYPE(FrameHeader)

class TestAspeed : public QObject {
    Q_OBJECT
private slots:
    void aBlockOfOneColourIsThatColour()
    {
        Frame frame(40, 8);
        for (const std::uint32_t colour : {white, black, red, green, blue}) {
            frame.colours({{0, colour}});
        }
        const auto screen = read({&frame});
        QCOMPARE(screen->picture().width, 40);
        QCOMPARE(screen->picture().height, 8);
        const std::array<Rgb, 5> wanted = {{{255, 255, 255}, {0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}}};
        for (int block = 0; block < 5; ++block) {
            QVERIFY2(near(at(*screen, 8 * block + 3, 4), wanted[static_cast<std::size_t>(block)]),
                     said(at(*screen, 8 * block + 3, 4)));
            QCOMPARE(at(*screen, 8 * block, 0), at(*screen, 8 * block + 7, 7)); // the same all over
        }
    }

    void twoAndFourColoursSayWhichOneEveryPixelIs()
    {
        Frame frame(16, 8);
        std::vector<int> stripes;
        std::vector<int> quarters;
        stripes.reserve(64);
        quarters.reserve(64);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                stripes.push_back(x % 2);
                quarters.push_back((y / 4) * 2 + x / 4);
            }
        }
        frame.colours({{0, white}, {1, black}}, stripes);
        frame.colours({{0, red}, {1, green}, {2, blue}, {3, white}}, quarters);
        const auto screen = read({&frame});
        QVERIFY(near(at(*screen, 0, 5), {255, 255, 255}));
        QVERIFY(near(at(*screen, 1, 5), {0, 0, 0}));
        QVERIFY(near(at(*screen, 6, 0), {255, 255, 255}));
        QVERIFY(near(at(*screen, 9, 1), {255, 0, 0}));
        QVERIFY(near(at(*screen, 14, 1), {0, 255, 0}));
        QVERIFY(near(at(*screen, 9, 6), {0, 0, 255}));
        QVERIFY(near(at(*screen, 14, 6), {255, 255, 255}));
    }

    void aColourThatIsKnownIsNamedAndNotSentAgain()
    {
        // A screen of text is two colours, thousands of times: the sender says each once and then only which.
        Frame first(24, 8);
        first.colours({{2, red}});
        first.colours({{2, std::nullopt}});
        std::vector<int> stripes;
        stripes.reserve(64);
        for (int pixel = 0; pixel < 64; ++pixel) {
            stripes.push_back(pixel % 8 % 2);
        }
        first.colours({{1, blue}, {2, std::nullopt}}, stripes);
        Frame second(24, 8);
        second.colours({{1, std::nullopt}}); // ...and it is still known when the next frame comes
        const auto screen = read({&first, &second});
        QVERIFY(near(at(*screen, 4, 4), {0, 0, 255})); // the second frame's block
        QVERIFY(near(at(*screen, 12, 4), {255, 0, 0}));
        QVERIFY(near(at(*screen, 16, 4), {0, 0, 255}));
        QVERIFY(near(at(*screen, 17, 4), {255, 0, 0}));
    }

    void blocksFollowOneAnotherUnlessTheySayWhereTheyAre()
    {
        Frame frame(24, 16);
        for (const std::uint32_t colour :
             {red, green, blue, white}) { // three across, and the fourth begins the next row
            frame.colours({{0, colour}});
        }
        frame.colours({{0, blue}}, {}, std::pair{2, 1}); // one that says where
        frame.colours({{0, red}});                       // ...and the one after it follows from there, round to the top
        const auto screen = read({&frame});
        QVERIFY(near(at(*screen, 4, 12), {255, 255, 255}));
        QVERIFY(near(at(*screen, 20, 12), {0, 0, 255}));
        QVERIFY(near(at(*screen, 4, 4), {255, 0, 0}));
        QVERIFY(near(at(*screen, 12, 4), {0, 255, 0}));
    }

    void aFrameBringsOnlyWhatChanged()
    {
        Frame whole(16, 16);
        for (int block = 0; block < 4; ++block) {
            whole.colours({{0, white}});
        }
        Frame change(16, 16);
        change.colours({{0, black}}, {}, std::pair{1, 0});
        Screen screen;
        const auto first = screen.read(whole.header(), whole.data());
        QVERIFY(first.ok() && first.value() == 4);
        const auto second = screen.read(change.header(), change.data());
        QVERIFY(second.ok() && second.value() == 1);
        QVERIFY(near(at(screen, 12, 3), {0, 0, 0}));
        for (const auto& [x, y] : {std::pair{3, 3}, std::pair{3, 12}, std::pair{12, 12}}) {
            QVERIFY(near(at(screen, x, y), {255, 255, 255}));
        }
    }

    void aJpegBlockOfOneShadeAndOneWithDetail()
    {
        Frame frame(24, 8, {.strength = 4});
        frame.jpeg(shade(0), flat, flat);  // the middle of the range
        frame.jpeg(shade(40), flat, flat); // brighter (each plane counts on from the one before it)
        Coefficients across = flat;
        across[0] = 0;
        across[1] = 30; // brightness falling from left to right, the same down every column
        frame.jpeg(across, flat, flat);
        const auto screen = read({&frame});
        const Rgb grey = at(*screen, 3, 3);
        QVERIFY(grey[0] == grey[1] && grey[1] == grey[2] && 120 <= grey[0] && grey[0] <= 136);
        QCOMPARE(at(*screen, 0, 0), at(*screen, 7, 7));
        QVERIFY(at(*screen, 11, 3)[0] > grey[0] + 30);
        std::vector<int> row;
        std::vector<int> lower;
        for (int x = 16; x < 24; ++x) {
            row.push_back(at(*screen, x, 2)[0]);
            lower.push_back(at(*screen, x, 6)[0]);
        }
        QVERIFY(std::is_sorted(row.rbegin(), row.rend()) && row[0] > row[7] + 40);
        QCOMPARE(lower, row);
    }

    void colourAtHalfTheDetailComesInBlocksOfSixteen()
    {
        Frame frame(32, 16, {.halfColour = true});
        frame.jpeg(std::array{shade(-60), shade(-20), shade(20), shade(60)}, flat, flat); // four shades, one a quarter
        frame.jpeg(std::array{shade(0), shade(0), shade(0), shade(0)}, shade(0), shade(60)); // reddish all over
        const auto screen = read({&frame});
        const std::vector<int> quarters = {at(*screen, 3, 3)[0], at(*screen, 12, 3)[0], at(*screen, 3, 12)[0],
                                           at(*screen, 12, 12)[0]};
        QVERIFY(std::is_sorted(quarters.begin(), quarters.end()));
        QVERIFY(quarters[0] != quarters[1] && quarters[1] != quarters[2] && quarters[2] != quarters[3]);
        const Rgb reddish = at(*screen, 24, 8);
        QVERIFY(reddish[0] > reddish[1] + 40 && reddish[0] > reddish[2] + 20);
        QCOMPARE(at(*screen, 17, 1), at(*screen, 30, 14));
    }

    void aCorrectionIsAddedToWhatTheBlockWasSentAs()
    {
        Frame sent(8, 8, {.strength = 4, .correctionStrength = 4});
        sent.jpeg(shade(0), flat, flat);
        Frame brighter(8, 8, {.strength = 4, .correctionStrength = 4});
        brighter.jpeg(shade(10), flat, flat, {}, false, true);
        const auto screen = read({&sent});
        const int was = at(*screen, 3, 3)[0];
        (void)screen->read(brighter.header(), brighter.data());
        const int once = at(*screen, 3, 3)[0];
        QVERIFY(once > was + 5);
        // The same correction again: to the block as sent, not to the corrected one.
        (void)screen->read(brighter.header(), brighter.data());
        QCOMPARE(at(*screen, 3, 3)[0], once);
    }

    void aScreenNeedNotBeAWholeNumberOfBlocks()
    {
        Frame frame(21, 11);
        for (int block = 0; block < 6; ++block) {
            frame.colours({{0, white}});
        }
        const auto screen = read({&frame});
        QCOMPARE(screen->picture().rgb.size(), std::size_t{3} * 21 * 11);
        QVERIFY(near(at(*screen, 20, 10), {255, 255, 255}));
        QVERIFY(near(at(*screen, 0, 0), {255, 255, 255}));
    }

    void aBlockThatSaysItIsOffTheScreenIsNotDrawn()
    {
        Frame frame(16, 8);
        frame.colours({{0, white}});
        frame.colours({{0, red}}, {}, std::pair{200, 3});
        frame.colours({{0, red}}, {}, std::pair{1, 150});
        Screen screen;
        const auto changed = screen.read(frame.header(), frame.data());
        QVERIFY(changed.ok() && changed.value() == 1);
        QVERIFY(near(at(screen, 3, 3), {255, 255, 255}));
        QCOMPARE(at(screen, 12, 3), (Rgb{0, 0, 0}));
    }

    void norIsACorrectionOrAJpegBlockOffTheScreen()
    {
        // (The desktop's own: what a correction off the screen is added to is looked up nowhere.)
        Frame frame(16, 8, {.correctionStrength = 4});
        frame.colours({{0, white}});
        frame.jpeg(shade(10), flat, flat, std::pair{200, 3}, false, true);
        frame.jpeg(shade(10), flat, flat, std::pair{1, 150}, false, true);
        frame.jpeg(shade(10), flat, flat, std::pair{255, 255});
        Frame half(32, 16, {.halfColour = true});
        half.jpeg(std::array{shade(1), shade(2), shade(3), shade(4)}, flat, flat, std::pair{255, 0});
        half.jpeg(shade(10), flat, flat, std::pair{0, 255}, false, true);
        Screen screen;
        const auto changed = screen.read(frame.header(), frame.data());
        QVERIFY(changed.ok() && changed.value() == 1);
        QCOMPARE(at(screen, 12, 3), (Rgb{0, 0, 0}));
        const auto large = screen.read(half.header(), half.data());
        QVERIFY(large.ok() && large.value() == 0);
    }

    void aNewSizeOfScreenIsANewPicture()
    {
        Frame small(8, 8);
        Frame large(16, 8);
        small.colours({{0, white}});
        large.colours({{0, red}}, {}, std::pair{1, 0});
        const auto screen = read({&small, &large});
        QCOMPARE(screen->picture().width, 16);
        QCOMPARE(screen->picture().height, 8);
        QCOMPARE(at(*screen, 3, 3), (Rgb{0, 0, 0}));
        QVERIFY(near(at(*screen, 12, 3), {255, 0, 0}));
    }

    void colourAtHalfTheDetailOrNotIsANewPictureToo()
    {
        // (The desktop's own, as the web's reads it.) A screen of the same size that changes to colour at half the
        // detail and back is a new picture each time, and what is kept of its blocks is made anew with it.
        Frame half(32, 16, {.halfColour = true});
        half.jpeg(std::array{shade(1), shade(2), shade(3), shade(4)}, flat, flat);
        Frame plain(32, 16);
        for (int block = 0; block < 8; ++block) {
            plain.colours({{0, white}});
        }
        Screen screen;
        QVERIFY(screen.read(half.header(), half.data()).ok());
        QCOMPARE(screen.held(), std::size_t{3} * 32 * 16 + std::size_t{24} * 2);
        const auto read = screen.read(plain.header(), plain.data());
        QVERIFY(read.ok() && read.value() == 8);
        QCOMPARE(screen.held(), std::size_t{3} * 32 * 16 + std::size_t{8} * 3 * 64 + std::size_t{24} * 2);
        QVERIFY(near(at(screen, 31, 15), {255, 255, 255}));
        QVERIFY(screen.read(half.header(), half.data()).ok());
        QCOMPARE(screen.held(), std::size_t{3} * 32 * 16 + std::size_t{24} * 2);
        QCOMPARE(at(screen, 31, 15), (Rgb{0, 0, 0})); // (a new picture: the block drawn was the first only)
    }

    void aFrameSaysWhichRectanglesItChanged()
    {
        // (The desktop's own: for whoever shows the screen to take only what changed.) Row by row of eight pixels,
        // from the leftmost block drawn to the rightmost, rows of the same span joined.
        Frame first(24, 16); // three blocks across, two down
        first.colours({{0, white}}, {}, std::pair{1, 0});
        first.colours({{0, white}}); // (the next one: 2, 0)
        first.colours({{0, red}}, {}, std::pair{0, 1});
        Screen screen;
        QVERIFY(screen.read(first.header(), first.data()).ok());
        QCOMPARE(std::vector(screen.changed().begin(), screen.changed().end()),
                 (std::vector<Rect>{{8, 0, 16, 8}, {0, 8, 8, 8}}));
        Frame column(24, 16);
        column.colours({{0, blue}}, {}, std::pair{1, 0});
        column.colours({{0, blue}}, {}, std::pair{1, 1});
        column.colours({{0, blue}}, {}, std::pair{200, 1}); // (off the screen: nothing changed there)
        QVERIFY(screen.read(column.header(), column.data()).ok());
        QCOMPARE(std::vector(screen.changed().begin(), screen.changed().end()), (std::vector<Rect>{{8, 0, 8, 16}}));
        // A screen that is no whole number of blocks: what changed stops where it does.
        Frame odd(21, 11);
        odd.colours({{0, white}}, {}, std::pair{2, 1});
        QVERIFY(screen.read(odd.header(), odd.data()).ok());
        QCOMPARE(std::vector(screen.changed().begin(), screen.changed().end()), (std::vector<Rect>{{16, 8, 5, 3}}));
        // A block of sixteen at half the detail is two rows of eight.
        Frame half(32, 16, {.halfColour = true});
        half.jpeg(std::array{shade(1), shade(2), shade(3), shade(4)}, flat, flat, std::pair{1, 0});
        QVERIFY(screen.read(half.header(), half.data()).ok());
        QCOMPARE(std::vector(screen.changed().begin(), screen.changed().end()), (std::vector<Rect>{{16, 0, 16, 16}}));
        // A frame that changes nothing says so, and one refused part of the way through says what it drew.
        Frame nothing(32, 16, {.halfColour = true});
        QVERIFY(screen.read(nothing.header(), nothing.data()).ok());
        QVERIFY(screen.changed().empty());
        Frame tooMany(16, 8);
        for (int block = 0; block < 2 * 2 + 64 + 1; ++block) {
            tooMany.colours({{0, white}});
        }
        QVERIFY(!screen.read(tooMany.header(), tooMany.data()).ok());
        QCOMPARE(std::vector(screen.changed().begin(), screen.changed().end()), (std::vector<Rect>{{0, 0, 16, 8}}));
        QVERIFY(!screen.read(tooMany.header(), std::vector<std::uint8_t>(longest + 1)).ok());
        QVERIFY(screen.changed().empty());
    }

    void thePictureIsTheOneTheControllersOwnReaderMakes()
    {
        // The web's: these frames were also given to the reader in a real controller's own web page (with the
        // two places put right where that one is wrong: the green that the red difference gives, and a
        // correction that goes below zero). The pictures were the same, byte for byte, as were those of a
        // thousand frames made by chance. The web's reader makes these two; this one makes the same.
        const auto mixed = std::make_unique<Screen>();
        for (const Frame& frame : aMixedScreen()) {
            QVERIFY(mixed->read(frame.header(), frame.data()).ok());
        }
        const auto coarse = std::make_unique<Screen>();
        for (const Frame& frame : aCoarseScreen()) {
            QVERIFY(coarse->read(frame.header(), frame.data()).ok());
        }
        QCOMPARE(sha256(mixed->picture()), "f19a96cac01b1c48099f5cf0d232465f9957ce48b68b520d2bf15641d00b2386");
        QCOMPARE(sha256(coarse->picture()), "50dbb5c74894e251f2c81a773d90d621fda79562cd0b301a53f8430ea947a095");
    }

    void pastItsEndAFrameReadsAsNothing()
    {
        // (The desktop's own.) Twelve bytes of nothing: a JPEG block whose planes run on past the frame's end,
        // where the frame reads as zeros, and the picture is the one the web's reader makes of it.
        Screen screen;
        const std::vector<std::uint8_t> nothing(12);
        const auto changed = screen.read({8, 8, 8, 8, false, 4, 7, false}, nothing);
        QVERIFY(changed.ok() && changed.value() == 1);
        QCOMPARE(sha256(screen.picture()), "fca2eb0cd40cbc3d33fb37ffc7c12f86ce4db2090e391be113d9868847b718cc");
    }

    void noBlockBeginsInAFramesLastTwoWords()
    {
        // (The desktop's own, as the web's reads it.) Blocks of one colour, seven bits each, four to a word and
        // no end mark: in a frame of two words none is read, in one of three the five that begin in the first.
        const std::uint32_t fourBlocks = 0x50A14280;
        auto words = [](int many, std::uint32_t word) {
            std::vector<std::uint8_t> found;
            for (int one = 0; one < many; ++one) {
                for (int shift = 0; shift < 32; shift += 8) {
                    found.push_back(static_cast<std::uint8_t>(word >> shift));
                }
            }
            return found;
        };
        const FrameHeader header{16, 8, 16, 8, false, 4, 7, false};
        const auto two = Screen().read(header, words(2, fourBlocks));
        QVERIFY(two.ok() && two.value() == 0);
        const auto three = Screen().read(header, words(3, fourBlocks));
        QVERIFY(three.ok() && three.value() == 5);
    }

    void aCodeThatStandsForNothingIsRefused()
    {
        // (The desktop's own, as the web's reads it.) Sixteen bits that begin no code of the standard's table where
        // they stand, though they would be one of another table: where a block's first coefficient of brightness
        // is to be (nine ones begin none of its codes; these are a code of the coefficients after it), and where
        // the last plane's second coefficient is (sixteen ones begin no code of any table).
        Frame first(16, 8);
        first.put(0x0, 4);     // a JPEG block
        first.put(0xFF82, 16); // its first coefficient: no code
        Frame last(16, 8);
        last.put(0x0, 4);
        last.put(0b00, 2);    // brightness: its first coefficient the one before it,
        last.put(0b1010, 4);  // and the rest of it nothing
        last.put(0b0000, 4);  // blue difference: the same
        last.put(0b00, 2);    // red difference: its first coefficient the one before it,
        last.put(0xFFFF, 16); // and then no code
        for (const Frame* frame : {&first, &last}) {
            const auto refused = Screen().read(frame->header(), frame->data());
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error().message, std::string("a code that stands for nothing"));
        }
    }

    void whatIsNotAFrameIsRefusedOrRunsOutAndIsNeverLongAboutIt()
    {
        // What a controller sends is not believed: it may be damaged, another version's, or somebody's idea of a joke.
        Numbers below(99);
        const FrameHeader header{640, 480, 640, 480, false, 3, 5, false};
        Screen screen;
        QElapsedTimer began;
        began.start();
        for (const int length : {0, 1, 3, 4, 8, 12, 40, 400, 4000, 40000}) {
            for (int time = 0; time < 4; ++time) {
                std::vector<std::uint8_t> noise;
                noise.reserve(static_cast<std::size_t>(length));
                for (int byte = 0; byte < length; ++byte) {
                    noise.push_back(static_cast<std::uint8_t>(below(256)));
                }
                (void)screen.read(header, noise);
            }
        }
        QVERIFY(began.elapsed() < 20000);
        QCOMPARE(screen.picture().rgb.size(), std::size_t{3} * 640 * 480); // ...and the picture is still a picture
        const Frame real = aMixedScreen()[0];
        const auto data = real.data();
        for (const std::size_t cut : {std::size_t{5}, std::size_t{37}, data.size() / 2}) { // a real frame, cut short
            Screen cutShort;
            (void)cutShort.read(real.header(), std::span(data).first(cut));
        }
    }

    void aFrameThatSaysSomethingImpossibleOfItselfIsRefused_data()
    {
        QTest::addColumn<FrameHeader>("header");
        FrameHeader header = usual();
        header.width = 0;
        QTest::newRow("width 0") << header;
        header = usual();
        header.height = 0;
        QTest::newRow("height 0") << header;
        header = usual();
        header.width = 70000;
        QTest::newRow("width 70000") << header;
        header = usual();
        header.height = 5000;
        QTest::newRow("height 5000") << header;
        header = usual();
        header.strength = 8;
        QTest::newRow("strength 8") << header;
        header = usual();
        header.correctionStrength = 200;
        QTest::newRow("correction strength 200") << header;
        header = usual();
        header.strength = -1;
        QTest::newRow("strength -1") << header;
        header = usual();
        header.correctionStrength = -1;
        QTest::newRow("correction strength -1") << header;
    }
    void aFrameThatSaysSomethingImpossibleOfItselfIsRefused()
    {
        QFETCH(FrameHeader, header);
        Screen screen;
        const auto refused = screen.read(header, std::vector<std::uint8_t>(64));
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("garbled"));
        QCOMPARE(screen.picture().width, 0); // and nothing was made ready for it
        QCOMPARE(screen.picture().rgb.size(), std::size_t{0});
    }

    // --- what a frame may cost --------------------------------------------------------------------------------
    // Found by a review of the web's that fed the reader what a controller in the wrong hands could send. Nothing
    // made it fall over or write where it should not; these are about how long and how much.

    void noFrameCostsMoreThanDrawingItsScreenTwice()
    {
        // Sixteen megabytes of blocks that are each in order, for a screen of four, kept one of the web's threads
        // at it for seven minutes.
        Frame frame(16, 16);
        for (int block = 0; block < 2 * 4 + 64 + 1; ++block) {
            frame.colours({{0, white}});
        }
        const auto refused = Screen().read(frame.header(), frame.data());
        QVERIFY(!refused.ok());
        QVERIFY(QByteArray::fromStdString(refused.error().message).contains("more blocks"));
        Frame fits(16, 16);
        for (int block = 0; block < 8; ++block) {
            fits.colours({{0, white}});
        }
        const auto read = Screen().read(fits.header(), fits.data());
        QVERIFY(read.ok() && read.value() == 8);
        Frame most(16, 16); // (the most there may be: two for every block and the spare ones)
        for (int block = 0; block < 2 * 4 + 64; ++block) {
            most.colours({{0, white}});
        }
        const auto all = Screen().read(most.header(), most.data());
        QVERIFY(all.ok() && all.value() == 2 * 4 + 64);
    }

    void noFrameIsLongerThanItsScreenCouldTake()
    {
        Frame small(16, 16);
        small.colours({{0, white}});
        auto data = small.data();
        data.resize(data.size() + std::size_t{2} * 3 * 16 * 16 + 4096);
        const auto refused = Screen().read(small.header(), data);
        QVERIFY(!refused.ok());
        QVERIFY(QByteArray::fromStdString(refused.error().message).contains("longer"));
        data.resize(std::size_t{2} * 3 * 16 * 16 + 4096); // (and one that is just as long as it may be is read)
        const auto read = Screen().read(small.header(), data);
        QVERIFY(read.ok() && read.value() == 1);
    }

    void aFrameLongerThanAnyScreenIsRefusedBeforeAnythingChanges()
    {
        // (The desktop's own.) A screen as large as there are could take more than eight megabytes; no frame may.
        Frame first(64, 32);
        first.colours({{0, white}});
        Screen screen;
        QVERIFY(screen.read(first.header(), first.data()).ok());
        std::vector<std::uint8_t> ended(longest + 1);
        ended[3] = 0x90; // an end mark at once, and nothing after it
        const FrameHeader largestScreen{largest, largest, largest, largest, false, 3, 5, false};
        const auto refused = screen.read(largestScreen, ended);
        QVERIFY(!refused.ok());
        QVERIFY(QByteArray::fromStdString(refused.error().message).contains("longer than a screen can be"));
        QCOMPARE(screen.picture().width, 64); // nothing was made ready for it
        ended.pop_back();
        const auto read = screen.read(largestScreen, ended);
        QVERIFY(read.ok() && read.value() == 0);
        QCOMPARE(screen.picture().width, largest);
    }

    void aScreenIsNoLargerThanTheOnesAControllerHas()
    {
        // Kept for every block: what it was last. At 4096 by 4096 that came to half a gigabyte for one of the web's.
        Frame wide(2048, 8);
        wide.colours({{0, white}});
        const auto read = Screen().read(wide.header(), wide.data());
        QVERIFY(read.ok() && read.value() == 1);
        Frame wider(2056, 8);
        wider.colours({{0, white}});
        QVERIFY(!Screen().read(wider.header(), wider.data()).ok());
        Frame tall(8, 2048);
        tall.colours({{0, white}});
        QVERIFY(Screen().read(tall.header(), tall.data()).ok());
        Frame taller(8, 2049);
        taller.colours({{0, white}});
        QVERIFY(!Screen().read(taller.header(), taller.data()).ok());
    }

    void whatIsKeptOfABlockIsKeptSmall()
    {
        // The web's kept a plane that is one shade all over as one number, and the others as 64 bytes: a list of 64
        // numbers each was ten times that. Here every plane is 64 bytes, and nothing else is kept of a block.
        Frame frame(16, 8);
        Coefficients across = flat;
        across[1] = 30;
        frame.jpeg(across, flat, flat);
        frame.jpeg(shade(40), flat, flat);
        const auto screen = read({&frame});
        QCOMPARE(screen->held(), std::size_t{3} * 16 * 8 + std::size_t{2} * 3 * 64 + std::size_t{24});
        Frame large(2048, 2048);
        large.colours({{0, white}});
        const auto largeScreen = read({&large});
        QCOMPARE(largeScreen->held(),
                 std::size_t{3} * 2048 * 2048 + std::size_t{256} * 256 * 3 * 64 + std::size_t{24} * 256); // 24 MiB
        Frame half(32, 16, {.halfColour = true});
        half.jpeg(std::array{shade(1), shade(2), shade(3), shade(4)}, flat, flat);
        const auto halfScreen = read({&half});
        // (Nothing is corrected at half the detail.)
        QCOMPARE(halfScreen->held(), std::size_t{3} * 32 * 16 + std::size_t{24} * 2);
    }

    void thePictureAndItsSizeAreHandedOutTogether()
    {
        // In the web's a picture was made of the screen by one thread while another read the next frame into it.
        // Its width, its height and its bytes were three things looked at one after the other: with a new size of
        // screen in between they were of two screens. Here they are one.
        Frame first(64, 32);
        Frame second(32, 16);
        first.colours({{0, white}});
        second.colours({{0, black}});
        Screen screen;
        QVERIFY(screen.read(first.header(), first.data()).ok());
        const Picture before = screen.picture();
        QVERIFY(screen.read(second.header(), second.data()).ok());
        const Picture& after = screen.picture();
        QVERIFY(before.width == 64 && before.height == 32 && before.rgb.size() == std::size_t{3} * 64 * 32);
        QVERIFY(after.width == 32 && after.height == 16 && after.rgb.size() == std::size_t{3} * 32 * 16);
    }

    void aScreenSaysWhetherAFrameMakesItsPictureAnew()
    {
        // (The desktop's own, for the console's limit on screens that change their size: what counts is a picture
        // made anew, and a header the screen refuses makes none.)
        Frame first(64, 32);
        first.colours({{0, white}});
        Screen screen;
        QVERIFY(!screen.resizedBy(first.header())); // (nothing held yet: the first picture is no change)
        QVERIFY(screen.read(first.header(), first.data()).ok());
        QVERIFY(!screen.resizedBy(first.header()));
        QVERIFY(screen.resizedBy(Frame(32, 32).header()));
        QVERIFY(screen.resizedBy(Frame(64, 16).header()));
        QVERIFY(screen.resizedBy(Frame(64, 32, {.halfColour = true}).header()));
        QVERIFY(!screen.resizedBy(Frame(64, 32, {.strength = 7, .correctionStrength = 2}).header()));
        for (const auto& [width, height] : {std::pair{0, 0}, std::pair{0, 32}, std::pair{64, 0},
                                            std::pair{largest + 1, 32}, std::pair{64, largest + 1}}) {
            QVERIFY(!screen.resizedBy(Frame(width, height).header()));
            QVERIFY(!screen.read(Frame(width, height).header(), {}).ok()); // (refused, and nothing made anew)
            QCOMPARE(screen.picture().width, 64);
        }
        for (const int strength : {-1, 8}) {
            FrameHeader odd = Frame(32, 16).header();
            odd.strength = strength;
            QVERIFY(!screen.resizedBy(odd));
            odd = Frame(32, 16).header();
            odd.correctionStrength = strength;
            QVERIFY(!screen.resizedBy(odd));
        }
        QVERIFY(screen.resizedBy(Frame(largest, largest).header()));
    }

    void aFrameThatNobodyWaitsForAnyMoreIsLetGoOf()
    {
        // A thread cannot be told to stop. One of the web's that was reading a long frame for a console that had
        // been closed read it to its end, and the program waited for it on its way out.
        Frame frame(640, 480);
        for (int block = 0; block < 300; ++block) {
            frame.colours({{0, white}});
        }
        Screen screen;
        screen.giveUp();
        const auto refused = screen.read(frame.header(), frame.data());
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("given_up"));
        Frame short63(640, 480); // (looked at every 64 blocks: a frame of fewer is read)
        for (int block = 0; block < 63; ++block) {
            short63.colours({{0, white}});
        }
        const auto read = screen.read(short63.header(), short63.data());
        QVERIFY(read.ok() && read.value() == 63);
    }

    void theConsoleScreenOfTheTestsIsTheWebs()
    {
        // The tests' own screen of a text console (as the web's demo shows): the same bytes as the web's writer
        // makes, and the same picture as its reader makes of them.
        const Frame frame = frames::consoleScreen(640, 480, 14, 5);
        const auto data = frame.data();
        QCOMPARE(QCryptographicHash::hash(QByteArrayView(data.data(), static_cast<qsizetype>(data.size())),
                                          QCryptographicHash::Sha256)
                     .toHex(),
                 "be994c80973e120fe4f21b38b7bc45600c135f7165f6de05ed79c601094cf6ee");
        Screen screen;
        const auto read = screen.read(frame.header(), data);
        QVERIFY(read.ok() && read.value() == 80 * 60);
        QCOMPARE(sha256(screen.picture()), "9ef4498813df5d53219f6bc59c3166a9f23838d3cadcc15bb7a800cb3b73fcc7");
    }
};

QTEST_GUILESS_MAIN(TestAspeed)
#include "test_aspeed.moc"
