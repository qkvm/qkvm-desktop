#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <QByteArray>
#include <QTest>

#include <qkvm/base/screen_changes.h>
#include <qkvm/base/screen_still.h>

// What changed of a screen, as one packet (base/screen_changes.h): written as said there, read back, and nothing
// taken that is not so to the byte.
namespace screen = qkvm::base::screen;
using screen::Rect;

namespace {

// A screen whose every pixel says where it is: red its column, green its row, blue the two together.
std::vector<std::uint8_t> numbered(int width, int height)
{
    std::vector<std::uint8_t> rgb;
    rgb.reserve(std::size_t{3} * static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            rgb.push_back(static_cast<std::uint8_t>(x));
            rgb.push_back(static_cast<std::uint8_t>(y));
            rgb.push_back(static_cast<std::uint8_t>(x + 7 * y));
        }
    }
    return rgb;
}

std::vector<std::byte> bytes(std::initializer_list<int> values)
{
    std::vector<std::byte> found;
    for (const int value : values) {
        found.push_back(static_cast<std::byte>(value));
    }
    return found;
}

// SplitMix64: the same at every run, on every computer.
struct Chance {
    std::uint64_t state;
    std::uint64_t next()
    {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    int below(int limit) { return static_cast<int>(next() % static_cast<std::uint64_t>(limit)); }
};

} // namespace

class TestScreenChanges : public QObject {
    Q_OBJECT

private slots:
    void aPacketIsTheScreensSizeItsRectanglesAndTheirPixels()
    {
        const std::vector<std::uint8_t> rgb = numbered(4, 3);
        const Rect rects[] = {{1, 0, 2, 1}, {3, 2, 1, 1}};
        const std::vector<std::byte> packet = screen::write(4, 3, rgb, rects);
        // As the header says, to the byte.
        QCOMPARE(packet, bytes({'Q', 'K', 'S', 'C', 4, 0, 3, 0, 2, 0, 0, 0, //
                                1,   0,   0,   0,   2, 0, 1, 0,             // the first rectangle
                                3,   0,   2,   0,   1, 0, 1, 0,             // the second
                                1,   0,   1,   2,   0, 2,                   // its two pixels: (1, 0) and (2, 0)
                                3,   2,   17}));                            // (3, 2)
        QVERIFY(screen::read(packet).has_value());
        const screen::Changes read = screen::read(packet).value_or(screen::Changes{});
        QCOMPARE(read.width, 4);
        QCOMPARE(read.height, 3);
        QCOMPARE(read.rects, (std::vector<Rect>{{1, 0, 2, 1}, {3, 2, 1, 1}}));
        QCOMPARE(read.pixels.size(), std::size_t{9});
        QVERIFY(!read.whole());
        const screen::Head head = screen::headOf(packet).value_or(screen::Head{});
        QVERIFY(head.width == 4 && head.height == 3 && !head.whole);
    }

    void theWholeScreenIsOneRectangle()
    {
        const std::vector<std::uint8_t> rgb = numbered(5, 2);
        const std::vector<std::byte> packet = screen::whole(5, 2, rgb);
        const screen::Changes read = screen::read(packet).value_or(screen::Changes{});
        QVERIFY(read.whole());
        QCOMPARE(read.pixels.size(), rgb.size());
        QVERIFY(std::memcmp(read.pixels.data(), rgb.data(), rgb.size()) == 0);
        QVERIFY(screen::headOf(packet).value_or(screen::Head{}).whole);
        // The whole screen and more is not the whole screen in one rectangle; nor is one that is not all of it.
        const Rect twice[] = {{0, 0, 5, 2}, {0, 0, 1, 1}};
        QVERIFY(screen::headOf(screen::write(5, 2, rgb, twice)).has_value());
        QVERIFY(!screen::headOf(screen::write(5, 2, rgb, twice)).value_or(screen::Head{0, 0, true}).whole);
        for (const Rect& part : {Rect{1, 0, 4, 2}, Rect{0, 1, 5, 1}, Rect{0, 0, 4, 2}, Rect{0, 0, 5, 1}}) {
            QVERIFY(!screen::headOf(screen::write(5, 2, rgb, std::span(&part, 1)))
                         .value_or(screen::Head{0, 0, true})
                         .whole);
        }
        // As large as the largest screen there is, and its packet as long as any is.
        const std::vector<std::uint8_t> largest(std::size_t{3} * screen::widest * screen::tallest, 9);
        const std::vector<std::byte> most = screen::whole(screen::widest, screen::tallest, largest);
        QVERIFY(screen::read(most).has_value());
        QVERIFY(most.size() <= screen::longest);
    }

    void aPacketIsWrittenIntoBytesOfTheCallersKind()
    {
        // (So that it is handed on in what it was written in: a server's live line writes it into a QByteArray.)
        const std::vector<std::uint8_t> rgb = numbered(4, 3);
        const Rect rects[] = {{1, 0, 2, 1}, {3, 2, 1, 1}};
        const std::vector<std::byte> bytes = screen::write(4, 3, rgb, rects);
        const QByteArray same = screen::write<QByteArray>(4, 3, rgb, rects);
        QCOMPARE(static_cast<std::size_t>(same.size()), bytes.size());
        QVERIFY(std::memcmp(same.constData(), bytes.data(), bytes.size()) == 0);
        const QByteArray all = screen::whole<QByteArray>(4, 3, rgb);
        const std::vector<std::byte> whole = screen::whole(4, 3, rgb);
        QCOMPARE(static_cast<std::size_t>(all.size()), whole.size());
        QVERIFY(std::memcmp(all.constData(), whole.data(), whole.size()) == 0);
        const Rect beyond{3, 2, 2, 1};
        QVERIFY(screen::write<QByteArray>(4, 3, rgb, std::span(&beyond, 1)).isEmpty());
    }

    void whatIsNotAPacketToTheByteIsNotTaken()
    {
        const std::vector<std::uint8_t> rgb = numbered(4, 3);
        const Rect rects[] = {{1, 0, 2, 1}, {3, 2, 1, 1}};
        const std::vector<std::byte> good = screen::write(4, 3, rgb, rects);
        QVERIFY(screen::read(good).has_value());
        const auto changed = [&good](std::size_t at, int value) {
            std::vector<std::byte> bad = good;
            bad[at] = static_cast<std::byte>(value);
            return bad;
        };
        QVERIFY(!screen::read(changed(0, 'X')).has_value()); // not what it is
        QVERIFY(!screen::read(changed(4, 0)).has_value());   // a screen nought wide
        QVERIFY(!screen::read(changed(6, 0)).has_value());   // ...nought high
        QVERIFY(!screen::read(changed(8, 0)).has_value());   // no rectangle
        QVERIFY(!screen::read(changed(8, 3)).has_value());   // more than there are
        QVERIFY(!screen::read(changed(10, 1)).has_value());  // the nought that is not
        QVERIFY(!screen::read(changed(12, 3)).has_value());  // a rectangle beyond the right edge
        QVERIFY(!screen::read(changed(14, 3)).has_value());  // ...beyond the bottom
        QVERIFY(!screen::read(changed(16, 0)).has_value());  // ...empty
        QVERIFY(!screen::read(changed(18, 0)).has_value());  // ...empty the other way
        QVERIFY(!screen::read(changed(17, 1)).has_value());  // ...far beyond (a high byte)
        std::vector<std::byte> shorter = good;
        shorter.pop_back();
        QVERIFY(!screen::read(shorter).has_value()); // a pixel short
        std::vector<std::byte> longer = good;
        longer.push_back(std::byte{0});
        QVERIFY(!screen::read(longer).has_value());                    // something after
        QVERIFY(!screen::read(std::span(good).first(11)).has_value()); // less than a head
        QVERIFY(!screen::read({}).has_value());
        // A head and nothing more: no rectangle, and so no pixels.
        QVERIFY(!screen::read(bytes({'Q', 'K', 'S', 'C', 4, 0, 3, 0, 0, 0, 0, 0})).has_value());
        // One rectangle more than a packet holds, each of them right.
        std::vector<std::byte> many = bytes({'Q', 'K', 'S', 'C', 4, 0, 3, 0});
        many.push_back(static_cast<std::byte>((screen::mostRects + 1) & 0xFF));
        many.push_back(static_cast<std::byte>((screen::mostRects + 1) >> 8));
        many.insert(many.end(), 2, std::byte{0});
        for (int one = 0; one <= screen::mostRects; ++one) {
            const std::vector<std::byte> rect = bytes({0, 0, 0, 0, 1, 0, 1, 0});
            many.insert(many.end(), rect.begin(), rect.end());
        }
        many.insert(many.end(), std::size_t{3} * (screen::mostRects + 1), std::byte{7});
        QVERIFY(!screen::read(many).has_value());
        many[8] = static_cast<std::byte>(screen::mostRects & 0xFF); // (as many as it holds, and three bytes too many)
        many[9] = static_cast<std::byte>(screen::mostRects >> 8);
        QVERIFY(!screen::read(many).has_value());
        const auto lastRect = static_cast<std::ptrdiff_t>(12 + std::size_t{8} * screen::mostRects);
        many.erase(many.begin() + lastRect, many.begin() + lastRect + 8);
        many.resize(many.size() - 3);
        QVERIFY(screen::read(many).has_value());
        // A head that says a screen nought wide, or nought high, is no head.
        QVERIFY(!screen::headOf(changed(4, 0)).has_value());
        QVERIFY(!screen::headOf(changed(6, 0)).has_value());
        // A screen larger than there is room for.
        std::vector<std::byte> wide = good;
        wide[4] = static_cast<std::byte>((screen::widest + 1) & 0xFF);
        wide[5] = static_cast<std::byte>((screen::widest + 1) >> 8);
        QVERIFY(!screen::read(wide).has_value());
        QVERIFY(!screen::headOf(wide).has_value());
        std::vector<std::byte> tall = good;
        tall[6] = static_cast<std::byte>((screen::tallest + 1) & 0xFF);
        tall[7] = static_cast<std::byte>((screen::tallest + 1) >> 8);
        QVERIFY(!screen::read(tall).has_value());
    }

    void nothingIsWrittenOfWhatIsNotAScreenOrNotInIt()
    {
        const std::vector<std::uint8_t> rgb = numbered(4, 3);
        const Rect inside{0, 0, 4, 3};
        QVERIFY(!screen::write(4, 3, rgb, std::span(&inside, 1)).empty());
        for (const Rect& outside : {Rect{-1, 0, 2, 2}, Rect{0, -1, 2, 2}, Rect{3, 0, 2, 1}, Rect{0, 2, 1, 2},
                                    Rect{0, 0, 0, 1}, Rect{0, 0, 1, 0}, Rect{1, 1, 4, 3}}) {
            QVERIFY(screen::write(4, 3, rgb, std::span(&outside, 1)).empty());
        }
        QVERIFY(screen::write(4, 3, rgb, {}).empty()); // no rectangle
        QVERIFY(screen::write(4, 3, std::span(rgb).first(rgb.size() - 1), std::span(&inside, 1)).empty());
        QVERIFY(screen::write(0, 3, rgb, std::span(&inside, 1)).empty());
        QVERIFY(screen::write(screen::widest + 1, 1, std::vector<std::uint8_t>(std::size_t{3} * (screen::widest + 1)),
                              std::span(&inside, 1))
                    .empty());
        const std::vector<Rect> tooMany(screen::mostRects + 1, Rect{0, 0, 1, 1});
        QVERIFY(screen::write(4, 3, rgb, tooMany).empty());
        const std::vector<Rect> most(screen::mostRects, Rect{0, 0, 1, 1});
        QVERIFY(screen::read(screen::write(4, 3, rgb, most)).has_value());
    }

    void whateverComesIsReadOrRefusedAndNeverReadBeyond()
    {
        // (Seeded, the same at every run.) Packets written, then cut and changed anywhere: what is taken is a packet
        // as said, its rectangles inside its screen and its pixels within the packet.
        Chance chance{2026102914};
        int taken = 0;
        for (int round = 0; round < 20000; ++round) {
            const int width = 1 + chance.below(40);
            const int height = 1 + chance.below(30);
            const std::vector<std::uint8_t> rgb = numbered(width, height);
            std::vector<Rect> rects;
            for (int one = 1 + chance.below(5); one > 0; --one) {
                const int x = chance.below(width);
                const int y = chance.below(height);
                rects.push_back({x, y, 1 + chance.below(width - x), 1 + chance.below(height - y)});
            }
            std::vector<std::byte> packet = screen::write(width, height, rgb, rects);
            QVERIFY(!packet.empty());
            switch (chance.below(4)) {
            case 0:
                packet.resize(static_cast<std::size_t>(chance.below(static_cast<int>(packet.size()) + 1)));
                break;
            case 1:
                for (int times = 1 + chance.below(4); times > 0; --times) {
                    packet[static_cast<std::size_t>(chance.below(static_cast<int>(packet.size())))] =
                        static_cast<std::byte>(chance.below(256));
                }
                break;
            case 2:
                packet.insert(packet.begin() + chance.below(static_cast<int>(packet.size()) + 1),
                              static_cast<std::byte>(chance.below(256)));
                break;
            default:
                break;
            }
            const std::optional<screen::Changes> read = screen::read(packet);
            if (!read) {
                continue;
            }
            ++taken;
            std::size_t pixels = 0;
            for (const Rect& rect : read->rects) {
                QVERIFY(rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0);
                QVERIFY(rect.x + rect.width <= read->width && rect.y + rect.height <= read->height);
                pixels += std::size_t{3} * static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height);
            }
            QCOMPARE(read->pixels.size(), pixels);
            QVERIFY(read->pixels.data() + read->pixels.size() == packet.data() + packet.size());
        }
        QVERIFY(taken > 5000); // (every packet left as it was, and those a change left a packet)
    }

    // --- a card's still of a server's screen (base/screen_still.h): the whole screen as one packet, made smaller as
    // the web makes its JPEGs of the same screen (kvm.py TIERS, tier_for), each pixel the mean of what falls in it.

    void aStillIsAsWideAsTheWebsTierForTheCardAndNeverWiderThanTheScreen()
    {
        QCOMPARE(screen::stillWidthFor(320, 1920), 480);
        QCOMPARE(screen::stillWidthFor(480, 1920), 480);
        QCOMPARE(screen::stillWidthFor(481, 1920), 640);
        QCOMPARE(screen::stillWidthFor(900, 1920), 960);
        QCOMPARE(screen::stillWidthFor(1200, 1920), 1920);
        QCOMPARE(screen::stillWidthFor(320, 400), 400);
        QCOMPARE(screen::stillWidthFor(640, 640), 640);
        // As one packet of the whole of it, read back so: 1024 by 768 for a card 320 wide is 480 by 360.
        const std::vector<std::uint8_t> rgb = numbered(1024, 768);
        const std::vector<std::byte> still = screen::still(1024, 768, rgb, 320);
        const std::optional<screen::Changes> read = screen::read(still);
        QVERIFY(read.has_value());
        const screen::Changes made = read.value_or(screen::Changes{});
        QVERIFY(made.whole());
        QCOMPARE(made.width, 480);
        QCOMPARE(made.height, 360);
        // A screen no wider than the tier: whole, as it is.
        const std::vector<std::uint8_t> small = numbered(400, 300);
        const std::vector<std::byte> whole = screen::still(400, 300, small, 320);
        const std::optional<screen::Changes> readWhole = screen::read(whole);
        QVERIFY(readWhole.has_value());
        const screen::Changes asItIs = readWhole.value_or(screen::Changes{});
        QCOMPARE(asItIs.width, 400);
        QCOMPARE(asItIs.pixels.size(), small.size());
        QCOMPARE(std::memcmp(asItIs.pixels.data(), small.data(), small.size()), 0);
    }

    void eachPixelOfAStillIsTheMeanOfWhatFallsInIt()
    {
        // Two pixels made one: their mean, rounded; four made two, two each.
        const std::vector<std::uint8_t> two{0, 10, 255, 255, 11, 0};
        const screen::Smaller one = screen::smaller(2, 1, two, 1);
        QCOMPARE(one.width, 1);
        QCOMPARE(one.height, 1);
        QCOMPARE(one.rgb, (std::vector<std::uint8_t>{128, 11, 128}));
        const std::vector<std::uint8_t> square{0, 0, 0, 100, 100, 100, 200, 200, 200, 50, 50, 50};
        const screen::Smaller half = screen::smaller(2, 2, square, 1);
        QCOMPARE(half.rgb, (std::vector<std::uint8_t>{88, 88, 88}));
        // Not made of what is not a screen, nor wider than it is.
        QVERIFY(screen::smaller(2, 1, two, 3).rgb.empty());
        QVERIFY(screen::smaller(2, 1, two, 0).rgb.empty());
        QVERIFY(screen::smaller(3, 1, two, 1).rgb.empty());
        QVERIFY(screen::still(3, 1, two, 320).empty());
    }

    void aStillOfTheLargestScreenIsNoLongerThanAStillMayBe()
    {
        // The whole of a screen of 2048 by 2048 (ASPEED's largest) is twelve megabytes: made smaller until it fits.
        const std::vector<std::uint8_t> rgb(std::size_t{3} * 2048 * 2048, 0x40);
        const std::vector<std::byte> still = screen::still(2048, 2048, rgb, 4000);
        QVERIFY(!still.empty());
        QVERIFY(still.size() <= screen::longestStill);
        const std::optional<screen::Head> read = screen::headOf(still);
        QVERIFY(read.has_value());
        const screen::Head head = read.value_or(screen::Head{});
        QVERIFY(head.whole);
        QVERIFY(head.width < 2048 && head.width > 1600);
        QCOMPARE(head.height, head.width);
    }
};

QTEST_GUILESS_MAIN(TestScreenChanges)
#include "test_screen_changes.moc"
