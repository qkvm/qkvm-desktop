#include "rfb_by_hand.h"
#include "rfb_client_by_hand.h"

#include <QTest>
#include <qkvm/rfb/rfb.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Windows' own header.
#include <windows.h>

// The RFB protocol as the reader speaks it: the handshake, every encoding it reads, what it says, and what a server is
// held to. The web's tests/test_rfb.py and test_rfb_hostile.py, against the tests' own server (rfb_by_hand.h).
using namespace qkvm::rfb;
using namespace qkvm::rfb::tests;
using qkvm::rfb::tests::Bytes;
namespace base = qkvm::base;
using namespace Qt::StringLiterals;

namespace {

// Clocks the test drives, and a JPEG decoder that charges `perJpeg` of processor time to the work clock each time it is
// asked (the decode itself is instant). `wall` and `work` are advanced by the test to drive the drawing share.
struct Share {
    std::int64_t wall = 0;
    std::int64_t work = 0;
    std::int64_t perJpeg = 0; // wall and work a JPEG decode takes
    std::int64_t perRead = 0; // ...and that every reading of the clock advances (to drive the loop checks)
    Options options()
    {
        Options made;
        // A decoder that charges `perJpeg` of time and returns a blank picture of the rectangle's size, whatever the
        // bytes: a real whole-screen JPEG is tens of kilobytes, far smaller than its raw pixels, so the tests frame a
        // tiny body and let the decoder stand in for the real one.
        made.jpeg = [this](std::span<const std::uint8_t> data, int w, int h) -> base::Result<Bytes> {
            static_cast<void>(data);
            wall += perJpeg;
            work += perJpeg;
            return Bytes(std::size_t{3} * static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
        };
        made.clock = [this] {
            wall += perRead;
            return wall;
        };
        made.workClock = [this] {
            work += perRead;
            return work;
        };
        return made;
    }
};

// A tiny Tight JPEG body: the Share decoder ignores the bytes and returns a picture of the rectangle's size.
const Bytes tinyJpeg{1, 2, 3, 4};

std::string codeOf(const base::Result<void>& result)
{
    return result.ok() ? std::string("ok") : result.error().code;
}

// A client let in by a server of version 3.8 with no password, its screen `width` by `height`.
Client connected(int width, int height, Options options = withJpeg(), std::int64_t now = 0)
{
    Client client(std::move(options));
    const auto fed =
        client.feed(join({greeting(), securityTypes({1}), word(0), serverInit(width, height, "srv")}), now);
    if (!fed.ok() || !client.ready()) {
        qFatal("the handshake did not go through");
    }
    static_cast<void>(client.takeOutgoing());
    static_cast<void>(client.take());
    return client;
}

// The client's screen, or a part of it, as a picture.
Picture screenOf(const Client& client)
{
    return Picture{client.width(), client.height(), client.rgb()};
}

bool shows(const Client& client, int x, int y, const Picture& expected)
{
    return part(screenOf(client), x, y, expected.width, expected.height).rgb == expected.rgb;
}

} // namespace

class TestRfb : public QObject {
    Q_OBJECT

private slots:
    // --- the handshake -----------------------------------------------------------------------------------------

    void aServerOf38WithNoPasswordLetsTheClientIn()
    {
        Client client(withJpeg());
        QVERIFY(okay(client.feed(greeting(), 0)));
        QCOMPARE(client.takeOutgoing(), text("RFB 003.008\n"));
        QVERIFY(okay(client.feed(securityTypes({2, 1}), 0)));
        QCOMPARE(client.takeOutgoing(), Bytes{1}); // None, where it is offered
        QVERIFY(okay(client.feed(word(0), 0)));
        QCOMPARE(client.takeOutgoing(), Bytes{1}); // shared: nobody else at the server is thrown out
        QVERIFY(!client.ready());
        QVERIFY(okay(client.feed(serverInit(640, 480, "a server"), 0)));
        QVERIFY(client.ready());
        QCOMPARE(client.minor(), 8);
        QCOMPARE(client.security(), securityNone);
        QCOMPARE(client.name(), std::string("a server"));
        QCOMPARE(client.width(), 640);
        QCOMPARE(client.height(), 480);
        QCOMPARE(client.rgb().size(), std::size_t{921'600});
        // Its pixel format and its encodings, in order of preference, JPEG asked for as a decoder is there.
        QCOMPARE(client.takeOutgoing(), firstWords({tight, zrle, hextile, copyRect, raw, desktopSize, jpegQuality}));
    }

    void withoutAJpegDecoderNoJpegIsAskedFor()
    {
        const Client client = connected(8, 8, Options{});
        Client again{Options{}};
        QVERIFY(okay(again.feed(join({greeting(), securityTypes({1}), word(0), serverInit(8, 8, "")}), 0)));
        QCOMPARE(again.takeOutgoing(), join({text("RFB 003.008\n"), Bytes{1}, Bytes{1},
                                             firstWords({tight, zrle, hextile, copyRect, raw, desktopSize})}));
        QVERIFY(client.ready());
    }

    void rgb565IsAskedForWithRawAndCopyRectAlone()
    {
        Options options;
        options.pixels = Pixels::Rgb565;
        options.encodings = {zrle, copyRect, raw, desktopSize};
        Client client(options);
        QVERIFY(okay(client.feed(join({greeting("RFB 003.007\n"), securityTypes({1}), serverInit(4, 2, "")}), 0)));
        QCOMPARE(client.takeOutgoing(),
                 join({text("RFB 003.007\n"), Bytes{1}, Bytes{1}, firstWords({copyRect, raw, desktopSize}, true)}));
        const Picture pixels = pattern(4, 2, 3);
        QVERIFY(okay(client.feed(update({raw565Rect(0, 0, pixels)}), 0)));
        QCOMPARE(screenOf(client).rgb, as565(pixels).rgb);
    }

    void theVersionsAreTheRfcsThree_data()
    {
        QTest::addColumn<QByteArray>("greeting");
        QTest::addColumn<int>("minor");
        QTest::newRow("3.3") << QByteArray("RFB 003.003\n") << 3;
        QTest::newRow("3.7") << QByteArray("RFB 003.007\n") << 7;
        QTest::newRow("3.8") << QByteArray("RFB 003.008\n") << 8;
        QTest::newRow("3.889 (Apple's)") << QByteArray("RFB 003.889\n") << 8;
        QTest::newRow("3.5: taken for 3.3") << QByteArray("RFB 003.005\n") << 3;
    }
    void theVersionsAreTheRfcsThree()
    {
        QFETCH(QByteArray, greeting);
        QFETCH(int, minor);
        Client client(withJpeg());
        QVERIFY(okay(client.feed(text(greeting.toStdString()), 0)));
        QCOMPARE(client.minor(), minor);
        QCOMPARE(client.takeOutgoing(), text("RFB 003.00" + std::to_string(minor) + "\n"));
        // 3.3: the server chooses; it says so in four bytes.
        if (minor == 3) {
            QVERIFY(okay(client.feed(join({word(1), serverInit(2, 2, "x")}), 0)));
        } else {
            QVERIFY(
                client.feed(join({securityTypes({1}), minor == 8 ? word(0) : Bytes{}, serverInit(2, 2, "x")}), 0).ok());
        }
        QVERIFY(client.ready());
    }

    void aGreetingAFamilyMeans38ByIsTaken()
    {
        Options options = withJpeg();
        options.greetedAs38 = {"RFB 004.000\n"};
        Client amt(options);
        QVERIFY(okay(amt.feed(text("RFB 004.000\n"), 0)));
        QCOMPARE(amt.minor(), 8);
        Client other(withJpeg());
        const auto fed = other.feed(text("RFB 004.000\n"), 0);
        QCOMPARE(codeOf(fed), std::string(unsupported));
        QVERIFY(other.over());
    }

    void whatIsNoGreetingIsRefused()
    {
        for (const char* said : {"HTTP/1.1 200", "RFB 03.008\n ", "RFB 003.008\r"}) {
            Client client(withJpeg());
            QCOMPARE(codeOf(client.feed(text(said), 0)), std::string(protocol));
        }
    }

    void aNoIsReadWithItsReason()
    {
        // 3.8 and 3.7: no ways of signing in, and the reason; 3.3: the way "invalid", and the reason.
        Client client(withJpeg());
        const auto fed = client.feed(join({greeting(), Bytes{0}, reason("too many\x01 clients\n")}), 0);
        QCOMPARE(codeOf(fed), std::string(refused));
        QCOMPARE(fed.error().message, std::string("the server refused the connection: too many clients"));
        Client old(withJpeg());
        const auto oldFed = old.feed(join({greeting("RFB 003.003\n"), word(0), reason("")}), 0);
        QCOMPARE(codeOf(oldFed), std::string(refused));
        QCOMPARE(oldFed.error().message, std::string("the server refused the connection: no reason given"));
        // A reason longer than a reason is refused before it is read.
        Client longer(withJpeg());
        QCOMPARE(codeOf(longer.feed(join({greeting(), Bytes{0}, word(1025)}), 0)), std::string(limit));
    }

    void aWayOfSigningInThisProgramDoesNotSpeakIsSaidByName()
    {
        Options options = withJpeg();
        options.signsIn = {securityNone}; // (OpenBMC's: the controller's password goes nowhere but its sign-in)
        Client client(options);
        const auto fed = client.feed(join({greeting(), securityTypes({19, 2, 19})}), 0);
        QCOMPARE(codeOf(fed), std::string(unsupported));
        QCOMPARE(fed.error().message,
                 std::string("the server asks for a way of signing in this program does not speak (type 2, VeNCrypt, "
                             "type 19)"));
        QVERIFY(client.takeOutgoing() == text("RFB 003.008\n")); // (and nothing was chosen)
        Client old(options);
        QCOMPARE(codeOf(old.feed(join({greeting("RFB 003.003\n"), word(2)}), 0)), std::string(unsupported));
    }

    void vncAuthenticationIsAnsweredByWhoeverHoldsThePassword()
    {
        Client client(withJpeg());
        Bytes challenge;
        for (int at = 0; at < 16; ++at) {
            challenge.push_back(static_cast<std::uint8_t>(at * 9));
        }
        QVERIFY(okay(client.feed(join({greeting(), securityTypes({2}), challenge}), 0)));
        QCOMPARE(client.takeOutgoing(), join({text("RFB 003.008\n"), Bytes{2}}));
        const std::optional<std::array<std::uint8_t, 16>> asked = client.challenge();
        QVERIFY(asked.has_value());
        QVERIFY(std::equal(challenge.begin(), challenge.end(), asked.value_or(std::array<std::uint8_t, 16>{}).begin()));
        // Nothing more is read while the answer is waited for.
        QVERIFY(okay(client.feed(word(0), 0)));
        QVERIFY(!client.ready());
        const Bytes response(16, 0xAB);
        QVERIFY(okay(client.answer(response, 0)));
        QCOMPARE(client.takeOutgoing(), join({response, Bytes{1}}));
        QVERIFY(!client.challenge().has_value());
        QVERIFY(okay(client.feed(serverInit(2, 2, ""), 0)));
        QVERIFY(client.ready());
        QCOMPARE(client.security(), securityVnc);
        // An answer nobody asked for ends it.
        QCOMPARE(codeOf(client.answer(response, 0)), std::string(protocol));
    }

    void aRejectedPasswordIsAuth()
    {
        Client client(withJpeg());
        QVERIFY(okay(client.feed(join({greeting(), securityTypes({2}), Bytes(16)}), 0)));
        const auto answered = client.answer(Bytes(16), 0);
        QVERIFY(okay(answered));
        const auto fed = client.feed(join({word(1), reason("Authentication failed")}), 0);
        QCOMPARE(codeOf(fed), std::string(auth));
        QCOMPARE(fed.error().message, std::string("Authentication failed"));
        // 3.7 says no reason.
        Client old(withJpeg());
        QVERIFY(okay(old.feed(join({greeting("RFB 003.007\n"), securityTypes({2}), Bytes(16)}), 0)));
        QVERIFY(okay(old.answer(Bytes(16), 0)));
        const auto oldFed = old.feed(word(1), 0);
        QCOMPARE(codeOf(oldFed), std::string(auth));
        QCOMPARE(oldFed.error().message, std::string("the server rejected the password"));
        // Too many tries (TightVNC's 2), and what means nothing.
        Client many(withJpeg());
        QVERIFY(okay(many.feed(join({greeting(), securityTypes({1})}), 0)));
        QCOMPARE(codeOf(many.feed(word(2), 0)), std::string(refused));
        Client nothing(withJpeg());
        QVERIFY(okay(nothing.feed(join({greeting(), securityTypes({1})}), 0)));
        QCOMPARE(codeOf(nothing.feed(word(7), 0)), std::string(protocol));
    }

    void theDesKeyOfAPasswordIsItsFirstEightBytesEachWithItsBitsMirrored()
    {
        // The key rule from two accounts of d3des.c and the RFC's erratum 4951, as the web's
        // test_the_des_key_and_response_are_what_other_implementations_make has it.
        const auto keyOf = [](std::string_view password) {
            std::array<std::uint8_t, 8> key{};
            key.fill(0xFF); // (what stood in the room before is not what is left in it)
            vncKey(std::span(reinterpret_cast<const std::uint8_t*>(password.data()), password.size()), key);
            return Bytes(key.begin(), key.end());
        };
        QCOMPARE(keyOf("12345678"), (Bytes{0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C}));
        QCOMPARE(keyOf("COW"), (Bytes{0b11000010, 0b11110010, 0b11101010, 0, 0, 0, 0, 0}));
        QCOMPARE(keyOf("a password longer than eight"), keyOf("a passwo"));
        QCOMPARE(keyOf(""), Bytes(8, 0));
        // Each of the 256 bytes, its bits read from the other end one by one.
        for (int byte = 0; byte < 256; ++byte) {
            int mirrored = 0;
            for (int bit = 0; bit < 8; ++bit) {
                if ((byte & (1 << bit)) != 0) {
                    mirrored |= 1 << (7 - bit);
                }
            }
            const std::string one(1, static_cast<char>(byte));
            QCOMPARE(keyOf(one), (Bytes{static_cast<std::uint8_t>(mirrored), 0, 0, 0, 0, 0, 0, 0}));
        }
    }

    void whatTheServerSaysOfItselfIsHeldToItsBounds()
    {
        Client named(withJpeg());
        Bytes init = serverInit(8, 8, "");
        init[20] = 0;
        init[21] = 0;
        init[22] = 4;
        init[23] = 1; // 1025 bytes
        QCOMPARE(codeOf(named.feed(join({greeting(), securityTypes({1}), word(0), init}), 0)), std::string(limit));
        QVERIFY(named.held() == 0); // (nothing of it kept)
        for (const auto& [width, height] :
             {std::pair{8193, 10}, std::pair{10, 8193}, std::pair{5000, 5000}, std::pair{0, 10}, std::pair{10, 0}}) {
            Client client(withJpeg());
            QCOMPARE(
                codeOf(client.feed(join({greeting(), securityTypes({1}), word(0), serverInit(width, height, "")}), 0)),
                std::string(limit));
        }
        Client largest(withJpeg());
        QVERIFY(okay(largest.feed(join({greeting(), securityTypes({1}), word(0), serverInit(8192, 1953, "")}), 0)));
    }

    void inPiecesOfOneByteItIsTheSame()
    {
        Deflater zrleStream;
        Deflater tightStream;
        const Picture pixels = pattern(70, 40, 4);
        const Bytes all = join(
            {greeting(), securityTypes({1}), word(0), serverInit(70, 40, "x"),
             update({rawRect(0, 0, part(pixels, 0, 0, 70, 10)), hextileRect(0, 10, part(pixels, 0, 10, 70, 10), true),
                     zrleRect(0, 20, part(pixels, 0, 20, 70, 10), ZrleTiles::PlainRuns, zrleStream),
                     tightRect(0, 30, part(pixels, 0, 30, 70, 10), TightFilter::Gradient, tightStream)})});
        Client client(withJpeg());
        for (const std::uint8_t byte : all) {
            QVERIFY(okay(client.feed(std::span(&byte, 1), 0)));
        }
        QCOMPARE(client.updates(), std::uint64_t{1});
        QCOMPARE(screenOf(client).rgb, pixels.rgb);
        QCOMPARE(client.held(), std::size_t{0});
    }

    // --- the encodings ------------------------------------------------------------------------------------------

    void rawPixelsAreDrawnWhereTheySay()
    {
        Client client = connected(40, 30);
        const Picture pixels = pattern(13, 7, 1);
        QVERIFY(okay(client.feed(update({rawRect(5, 9, pixels)}), 0)));
        QVERIFY(shows(client, 5, 9, pixels));
        QCOMPARE(client.frames(), std::uint64_t{1});
        const Client::Changed changed = client.take();
        QVERIFY(changed.rects == std::vector<Rect>{Rect(5, 9, 13, 7)});
        QVERIFY(!changed.resized);
        // A large one is read a few rows at a time: what is kept of it stays under a chunk and a row.
        Client wide = connected(1920, 1080);
        const Picture screen = pattern(1920, 1080, 2);
        const Bytes said = update({rawRect(0, 0, screen)});
        for (std::size_t at = 0; at < said.size(); at += 50'000) {
            QVERIFY(okay(wide.feed(std::span(said).subspan(at, std::min<std::size_t>(50'000, said.size() - at)), 0)));
            QVERIFY(wide.held() < 64 * 1024 + 50'000);
        }
        QCOMPARE(screenOf(wide).rgb, screen.rgb);
    }

    void copyRectCopiesWhatIsOnTheScreenEitherWay()
    {
        Client client = connected(20, 20);
        const Picture first = pattern(20, 20, 3);
        QVERIFY(okay(client.feed(update({rawRect(0, 0, first)}), 0)));
        // Down and to the right, over itself; then up and to the left, over itself.
        QVERIFY(okay(client.feed(update({copyRectOf(3, 4, 10, 10, 1, 1)}), 0)));
        QVERIFY(shows(client, 3, 4, part(first, 1, 1, 10, 10)));
        const Picture now = screenOf(client);
        QVERIFY(okay(client.feed(update({copyRectOf(0, 0, 12, 12, 4, 5)}), 0)));
        QVERIFY(shows(client, 0, 0, part(now, 4, 5, 12, 12)));
        QCOMPARE(codeOf(client.feed(update({copyRectOf(0, 0, 12, 12, 9, 0)}), 0)), std::string(protocol));
    }

    void hextileTilesAreDrawn()
    {
        for (const bool subrects : {false, true}) {
            Client client = connected(50, 40);
            // (A width and a height that are no multiple of 16; a picture of few colours for the subrectangles.)
            Picture pixels = plain(37, 21, {10, 20, 30});
            const Picture dots = pattern(37, 21, 5);
            for (std::size_t at = 0; at < pixels.rgb.size(); at += std::size_t{21}) {
                std::copy_n(dots.rgb.begin() + static_cast<std::ptrdiff_t>(at), 3,
                            pixels.rgb.begin() + static_cast<std::ptrdiff_t>(at));
            }
            QVERIFY(okay(client.feed(update({hextileRect(6, 11, pixels, subrects)}), 0)));
            QVERIFY2(shows(client, 6, 11, pixels), subrects ? "subrects" : "raw");
        }
    }

    void aHextileSubrectangleOutsideItsTileIsRefused()
    {
        Client client = connected(32, 32);
        Bytes said = head(0, 0, 16, 16, hextile);
        said.insert(said.end(), {2 | 8, 1, 2, 3, 0, 1, 0xF0, 0x10}); // background, one subrectangle: 2 wide at x 15
        QCOMPARE(codeOf(client.feed(update({said}), 0)), std::string(protocol));
    }

    void zrleTilesAreDrawnInEveryWay()
    {
        Client client = connected(150, 100);
        Deflater stream;
        const Picture busy = pattern(150, 70, 6);
        QVERIFY(okay(client.feed(update({zrleRect(0, 0, busy, ZrleTiles::Raw, stream)}), 0)));
        QVERIFY(shows(client, 0, 0, busy));
        // Tiles of few colours: packed (2, 4 and 16 colours), and runs of either kind.
        const auto few = [](int colours) {
            Picture made = pattern(70, 30, colours);
            for (std::size_t at = 0; at < made.rgb.size(); at += 3) {
                const auto index = static_cast<std::uint8_t>((at / 3 / 5) % static_cast<std::size_t>(colours));
                made.rgb[at] = static_cast<std::uint8_t>(index * 16);
                made.rgb[at + 1] = static_cast<std::uint8_t>(255 - index);
                made.rgb[at + 2] = 7;
            }
            return made;
        };
        for (const int colours : {2, 3, 16}) {
            const Picture pixels = few(colours);
            QVERIFY(okay(client.feed(update({zrleRect(10, 60, pixels, ZrleTiles::Packed, stream)}), 0)));
            QVERIFY2(shows(client, 10, 60, pixels), qPrintable(QString::number(colours)));
        }
        for (const ZrleTiles way : {ZrleTiles::PlainRuns, ZrleTiles::PaletteRuns}) {
            const Picture pixels = few(5);
            QVERIFY(okay(client.feed(update({zrleRect(80, 10, pixels, way, stream)}), 0)));
            QVERIFY(shows(client, 80, 10, pixels));
        }
        const Picture one = plain(150, 100, {9, 8, 7});
        QVERIFY(okay(client.feed(update({zrleRect(0, 0, one, ZrleTiles::Solid, stream)}), 0)));
        QVERIFY(shows(client, 0, 0, one));
        // A rectangle of no pixels still has its part of the stream (a flush), read to keep it in step.
        QVERIFY(okay(client.feed(update({zrleOf(0, 0, 0, 0, stream.part({}))}), 0)));
        QVERIFY(okay(client.feed(update({zrleRect(0, 0, busy, ZrleTiles::Raw, stream)}), 0)));
        QVERIFY(shows(client, 0, 0, busy));
    }

    void whatIsNoZrleIsRefused_data()
    {
        const QByteArray protocolWord(protocol.data(), static_cast<qsizetype>(protocol.size()));
        QTest::addColumn<QByteArray>("tiles");
        QTest::addColumn<QByteArray>("code");
        // Of a 4 by 1 rectangle.
        QTest::newRow("a kind there is none of") << QByteArray("\x11", 1) << protocolWord;
        QTest::newRow("129") << QByteArray("\x81", 1) << protocolWord;
        QTest::newRow("cut short") << QByteArray("\x00\x01\x02", 3) << protocolWord;
        QTest::newRow("longer than its tiles") << QByteArray("\x01\x01\x02\x03\x04", 5) << protocolWord;
        QTest::newRow("a run past its tile") << QByteArray("\x80\x01\x02\x03\x04", 5) << protocolWord;
        QTest::newRow("an index past the palette")
            << QByteArray("\x82\x01\x02\x03\x04\x05\x06\x02\x01\x00\x01", 11) << protocolWord;
        QTest::newRow("a packed index past the palette")
            << QByteArray("\x03\x01\x02\x03\x04\x05\x06\x07\x08\x09\xFF", 11) << protocolWord;
    }
    void whatIsNoZrleIsRefused()
    {
        QFETCH(QByteArray, tiles);
        QFETCH(QByteArray, code);
        Client client = connected(8, 8);
        Deflater stream;
        const Bytes deflated = stream.part(
            std::span(reinterpret_cast<const std::uint8_t*>(tiles.data()), static_cast<std::size_t>(tiles.size())));
        QCOMPARE(QByteArray::fromStdString(codeOf(client.feed(update({zrleOf(0, 0, 4, 1, deflated)}), 0))), code);
    }

    void aZrleStreamIsHeldToItsRectangleBeforeItIsRead()
    {
        // Said longer than its rectangle could ever take: refused at its length, before a byte of it is kept.
        Client client = connected(64, 64);
        Bytes said = head(0, 0, 64, 64, zrle);
        be32(said, 4 * 64 * 64 + 64 + 4096 + 1);
        QCOMPARE(codeOf(client.feed(update({said}), 0)), std::string(limit));
        QCOMPARE(client.held(), std::size_t{0});
        // Inflating past what its rectangle can hold.
        Client more = connected(64, 64);
        Deflater stream(Deflater::Way::Stored);
        const Bytes big(4 * 4 * 4 + 385 + 16 + 1, 0);
        QCOMPARE(codeOf(more.feed(update({zrleOf(0, 0, 4, 4, stream.part(big))}), 0)), std::string(limit));
        // A stream that is no zlib stream, and one used after its end.
        Client garbled = connected(64, 64);
        QCOMPARE(codeOf(garbled.feed(update({zrleOf(0, 0, 4, 4, Bytes{1, 2, 3, 4})}), 0)), std::string(protocol));
        Client ended = connected(64, 64);
        Deflater finished;
        const Bytes ending = join({finished.part(zrleTiles(plain(4, 4, {1, 1, 1}), ZrleTiles::Solid)), finished.end()});
        QVERIFY(okay(ended.feed(update({zrleOf(0, 0, 4, 4, ending)}), 0)));
        QCOMPARE(codeOf(ended.feed(update({zrleOf(0, 0, 0, 0, Bytes{})}), 0)), std::string(protocol));
    }

    void tightRectanglesAreDrawnInEveryWay()
    {
        Client client = connected(100, 60);
        std::array<Deflater, 4> streams;
        const Picture busy = pattern(100, 40, 8);
        QVERIFY(okay(client.feed(update({tightRect(0, 0, busy, TightFilter::Copy, streams[0])}), 0)));
        QVERIFY(shows(client, 0, 0, busy));
        QVERIFY(okay(client.feed(update({tightRect(0, 0, busy, TightFilter::Gradient, streams[1], 1)}), 0)));
        QVERIFY(shows(client, 0, 0, busy));
        // A palette of two (a bit a pixel), and of more (a byte a pixel).
        for (const int colours : {2, 5}) {
            Picture pixels = plain(45, 20, {0, 0, 0});
            for (std::size_t at = 0; at < pixels.rgb.size(); at += 3) {
                pixels.rgb[at] = static_cast<std::uint8_t>(((at / 3) % static_cast<std::size_t>(colours)) * 40);
            }
            QVERIFY(okay(client.feed(update({tightRect(50, 30, pixels, TightFilter::Palette, streams[2], 2)}), 0)));
            QVERIFY(shows(client, 50, 30, pixels));
        }
        // Under 12 bytes: sent as they are, not deflated.
        const Picture small = pattern(3, 1, 9);
        QVERIFY(okay(client.feed(update({tightRect(1, 50, small, TightFilter::Copy, streams[3], 3)}), 0)));
        QVERIFY(shows(client, 1, 50, small));
        // A fill, and a JPEG.
        QVERIFY(okay(client.feed(update({tightFill(10, 41, 30, 9, {200, 100, 50})}), 0)));
        QVERIFY(shows(client, 10, 41, plain(30, 9, {200, 100, 50})));
        const Picture photo = pattern(64, 32, 10);
        QVERIFY(okay(client.feed(update({tightJpeg(20, 20, 64, 32, madeUpJpeg(photo))}), 0)));
        QVERIFY(shows(client, 20, 20, photo));
        // A stream reset by the control byte begins again (the next part with a zlib header of its own).
        Deflater fresh;
        QVERIFY(okay(client.feed(update({tightRect(0, 0, busy, TightFilter::Copy, fresh, 0, 1)}), 0)));
        QVERIFY(shows(client, 0, 0, busy));
        QCOMPARE(client.frames(), std::uint64_t{8});
    }

    void whatIsNoTightIsRefused()
    {
        const auto refusedAs = [](const Bytes& rect, Options options = withJpeg()) {
            Client client = connected(3000, 20, std::move(options));
            return codeOf(client.feed(update({rect}), 0));
        };
        Deflater stream;
        QCOMPARE(refusedAs(tightRect(0, 0, pattern(2049, 1), TightFilter::Copy, stream)), std::string(protocol));
        QCOMPARE(refusedAs(join({head(0, 0, 4, 4, tight), Bytes{0xA0}})), std::string(protocol));
        QCOMPARE(refusedAs(join({head(0, 0, 4, 4, tight), Bytes{0x40, 3}})), std::string(protocol));
        // A stream said longer than its pixels could be, before it is read.
        Bytes longer = join({head(0, 0, 4, 4, tight), Bytes{0x00}});
        compact(longer, 48 + 4096 + 1);
        QCOMPARE(refusedAs(longer), std::string(limit));
        // Inflating to less than its rectangle.
        Deflater less;
        Bytes fewerBytes = join({head(0, 0, 4, 4, tight), Bytes{0x00}});
        const Bytes fewer = less.part(Bytes(40, 1));
        compact(fewerBytes, fewer.size());
        fewerBytes.insert(fewerBytes.end(), fewer.begin(), fewer.end());
        QCOMPARE(refusedAs(fewerBytes), std::string(protocol));
        // A JPEG with no decoder, of another size, that cannot be read, and too long to be one.
        QCOMPARE(refusedAs(tightJpeg(0, 0, 4, 4, madeUpJpeg(pattern(4, 4))), Options{}), std::string(protocol));
        QCOMPARE(refusedAs(tightJpeg(0, 0, 4, 4, madeUpJpeg(pattern(5, 4)))), std::string(protocol));
        QCOMPARE(refusedAs(tightJpeg(0, 0, 4, 4, Bytes(20, 1))), std::string(protocol));
        Bytes jpeg = join({head(0, 0, 4, 4, tight), Bytes{0x90}});
        compact(jpeg, 48 + 4096 + 1);
        QCOMPARE(refusedAs(jpeg), std::string(limit));
        // A palette index past the palette (of one colour, a bit a pixel).
        Bytes palette = join({head(0, 0, 4, 1, tight), Bytes{0x40, 1, 0, 1, 1, 1, 0x80}});
        QCOMPARE(refusedAs(palette), std::string(protocol));
        // ...and of three colours, a byte a pixel.
        Bytes three = join({head(0, 0, 4, 1, tight), Bytes{0x40, 1, 2, 1, 1, 1, 2, 2, 2, 3, 3, 3, 0, 1, 2, 3}});
        QCOMPARE(refusedAs(three), std::string(protocol));
    }

    void aChangeOfSizeKeepsTheTopLeft()
    {
        Client client = connected(20, 10);
        const Picture first = pattern(20, 10, 11);
        QVERIFY(okay(client.feed(update({rawRect(0, 0, first)}), 0)));
        static_cast<void>(client.take());
        QVERIFY(okay(client.feed(update({desktopSizeRect(30, 6)}), 0)));
        QCOMPARE(client.width(), 30);
        QCOMPARE(client.height(), 6);
        QVERIFY(shows(client, 0, 0, part(first, 0, 0, 20, 6)));
        const Client::Changed changed = client.take();
        QVERIFY(changed.resized);
        QVERIFY(changed.rects == std::vector<Rect>{Rect(0, 0, 30, 6)});
        QCOMPARE(codeOf(client.feed(update({desktopSizeRect(9000, 6)}), 0)), std::string(limit));
    }

    void whatAServerSaysBesideItsScreenIsReadAndLetGoOf()
    {
        Client client = connected(10, 10);
        Bytes map{1, 0, 0, 0, 0, 2};
        map.insert(map.end(), 12, 7);
        Bytes cut{3, 0, 0, 0};
        be32(cut, 5);
        cut.insert(cut.end(), {'h', 'e', 'l', 'l', 'o'});
        QVERIFY(okay(client.feed(join({map, Bytes{2}, cut, update({rawRect(0, 0, pattern(2, 2))})}), 0)));
        QCOMPARE(client.updates(), std::uint64_t{1});
        // A clipboard larger than this program takes is refused at its length.
        Bytes huge{3, 0, 0, 0};
        be32(huge, 1024 * 1024 + 1);
        QCOMPARE(codeOf(client.feed(huge, 0)), std::string(limit));
        // A message of a kind this program does not read.
        Client other = connected(10, 10);
        QCOMPARE(codeOf(other.feed(Bytes{4}, 0)), std::string(protocol));
        // And nothing after an end.
        QCOMPARE(codeOf(other.feed(update({}), 0)), std::string(protocol));
    }

    void aRectangleOutsideTheScreenOrInAnEncodingNotAskedForIsRefused()
    {
        Client client = connected(10, 10);
        QCOMPARE(codeOf(client.feed(update({rawRect(5, 5, pattern(6, 1))}), 0)), std::string(protocol));
        Client other = connected(10, 10);
        QCOMPARE(codeOf(other.feed(update({head(0, 0, 1, 1, 2)}), 0)), std::string(protocol)); // RRE
        Client negative = connected(10, 10);
        QCOMPARE(codeOf(negative.feed(update({head(0, 0, 1, 1, -224)}), 0)), std::string(protocol));
        Options noTight = withJpeg();
        noTight.encodings = {raw};
        Client rawOnly = connected(10, 10, noTight);
        QCOMPARE(codeOf(rawOnly.feed(update({tightFill(0, 0, 1, 1, {1, 2, 3})}), 0)), std::string(protocol));
    }

    void aServerIsHeldToWhatAServerHasReasonToSend()
    {
        // More than four screens in one update.
        Client client = connected(10, 10);
        QCOMPARE(codeOf(client.feed(update({tightFill(0, 0, 10, 10, {1, 1, 1}), tightFill(0, 0, 10, 10, {1, 1, 1}),
                                            tightFill(0, 0, 10, 10, {1, 1, 1}), tightFill(0, 0, 10, 10, {1, 1, 1}),
                                            tightFill(0, 0, 1, 1, {1, 1, 1})}),
                                    0)),
                 std::string(limit));
        // More messages in a second than 2000; the next second counts anew.
        Client bells = connected(10, 10);
        QVERIFY(okay(bells.feed(Bytes(2000, 2), 0)));
        QVERIFY(okay(bells.feed(Bytes(2000, 2), 1000)));
        QCOMPARE(codeOf(bells.feed(Bytes{2}, 1999)), std::string(limit));
        // More changes of size than ten.
        Client sizes = connected(10, 10);
        for (int one = 0; one < 10; ++one) {
            QVERIFY(okay(sizes.feed(update({desktopSizeRect(10 + one % 2, 10)}), 0)));
        }
        QCOMPARE(codeOf(sizes.feed(update({desktopSizeRect(10, 10)}), 0)), std::string(limit));
        // More pixels in a second than sixteen screens (and never fewer than eight million).
        Client busy = connected(1000, 1000);
        const Bytes screen = update({rawRect(0, 0, plain(1000, 1000, {1, 2, 3}))});
        for (int one = 0; one < 16; ++one) {
            QVERIFY(okay(busy.feed(screen, 5000)));
        }
        QCOMPARE(codeOf(busy.feed(screen, 5999)), std::string(limit));
        // Rectangles: fifty thousand a second.
        Client many = connected(10, 10);
        Bytes empty{0, 0, 0xC3, 0x51}; // 50001 rectangles
        QCOMPARE(codeOf(many.feed(empty, 0)), std::string(limit));
    }

    void aZrleStreamOfTinyDynamicBlocksIsCountedAsItsWork()
    {
        // finding 1 / confirm review 3: a ZRLE stream of empty dynamic blocks (each ~12 bytes) builds three
        // 32,768-entry tables, cleared and filled, for nothing. Every part of that is counted: the code-length table
        // and the clears, not only the literal table. Five hundred blocks against a 2048x2048 screen (cap 67 M a
        // second) is over the cap counting all of it (about 147,000 a block), and under it counting only the literal
        // table (16,384) or leaving the clears out: so the count must be whole for this to be refused.
        // And it is counted before each table is built, not once the stream is read (outside review 2.1): the stream
        // is cut off where the second's work runs out, in the inflater's own words, before its last blocks are read.
        Client client = connected(2048, 2048);
        Deflater stream;
        const Bytes blocks = stream.emptyDynamicStream(500);
        const auto fed = client.feed(update({zrleOf(0, 0, 2048, 2048, blocks)}), 0);
        QCOMPARE(codeOf(fed), std::string(limit));
        QVERIFY2(fed.error().message.find("far more work to unpack") != std::string::npos, fed.error().message.c_str());
        // A Tight stream the same way.
        Client tightClient = connected(64, 64);
        const Bytes tightBlocks = stream.emptyDynamicStream(350);
        Bytes rect = head(0, 0, 4, 4, 7);
        rect.push_back(0x00); // stream 0, the copy filter: 48 bytes of pixels, deflated
        compact(rect, tightBlocks.size());
        rect.insert(rect.end(), tightBlocks.begin(), tightBlocks.end());
        const auto tightFed = tightClient.feed(update({rect}), 0);
        QCOMPARE(codeOf(tightFed), std::string(limit));
        QVERIFY2(tightFed.error().message.find("far more work to unpack") != std::string::npos,
                 tightFed.error().message.c_str());
    }

    void aZrleStreamThatHoldsTheReaderForALongStepIsCutOff()
    {
        // Outside review 2.1: the inflater asks whether the reader has had too much of its thread before the tables of
        // every dynamic block (not every 1024 blocks), and a rectangle whose stream has taken a long step (LONG_STEP,
        // 2 s) of the thread's own time is cut off there, the server at fault. Here every reading of the clocks says
        // 50 ms went by, and three hundred empty dynamic blocks (well within the second's work) are read: cut off at
        // the fortieth asking.
        Share share;
        share.perRead = 50;
        Client client = connected(2048, 2048, share.options());
        Deflater stream;
        const auto fed = client.feed(update({zrleOf(0, 0, 2048, 2048, stream.emptyDynamicStream(300))}), share.wall);
        QCOMPARE(codeOf(fed), std::string(limit));
        QVERIFY2(fed.error().message.find("far too long") != std::string::npos, fed.error().message.c_str());
        QVERIFY2(share.work < 3000, QByteArray::number(static_cast<qlonglong>(share.work)));
    }

    void aRectanglesStreamIsHeldToItsBlocks()
    {
        // Outside review 2.1: a rectangle's stream may begin no more blocks than its room allows (one for every 64
        // bytes it may inflate to, and 64 more): a 1x1 ZRLE rectangle's at the most 70. Empty stored blocks are the
        // cheapest there are, and still each is counted.
        Deflater fits(Deflater::Way::Stored);
        Client client = connected(8, 8);
        Bytes stream = fits.part(Bytes{1, 9, 8, 7}); // a solid tile: one stored block and its flush
        const Bytes more = Deflater::emptyStoredBlocks(60);
        stream.insert(stream.end(), more.begin(), more.end());
        QVERIFY(okay(client.feed(update({zrleOf(0, 0, 1, 1, stream)}), 0)));
        QVERIFY(shows(client, 0, 0, plain(1, 1, {9, 8, 7})));
        Deflater over(Deflater::Way::Stored);
        Client overClient = connected(8, 8);
        Bytes tooMany = over.part(Bytes{1, 9, 8, 7});
        const Bytes many = Deflater::emptyStoredBlocks(200);
        tooMany.insert(tooMany.end(), many.begin(), many.end());
        const auto fed = overClient.feed(update({zrleOf(0, 0, 1, 1, tooMany)}), 0);
        QCOMPARE(codeOf(fed), std::string(limit));
        QVERIFY2(fed.error().message.find("far more work to unpack") != std::string::npos, fed.error().message.c_str());
    }

    void aSecondsStreamsAreHeldToTheirBlocks()
    {
        // ...and in a second, all its rectangles' streams may begin no more than 200,000 blocks: rectangles of 60
        // blocks each, within their own bound, are refused at the 3,334th within one second, and taken in the next.
        Deflater stream(Deflater::Way::Stored);
        const Bytes padding = Deflater::emptyStoredBlocks(58);
        const auto oneRect = [&stream, &padding] {
            Bytes part = stream.part(Bytes{1, 9, 8, 7}); // two blocks: the tile's, and the flush
            part.insert(part.end(), padding.begin(), padding.end());
            return zrleOf(0, 0, 1, 1, part);
        };
        // (One update of many rectangles: a second's 2,000 messages are not what is counted here.)
        Bytes many = update({});
        many[2] = 0x0D; // 3,333 rectangles
        many[3] = 0x05;
        for (int one = 0; one < 3333; ++one) {
            const Bytes rect = oneRect();
            many.insert(many.end(), rect.begin(), rect.end());
        }
        const Bytes next = update({oneRect()});
        Client client = connected(64, 64);
        QVERIFY(okay(client.feed(many, 0)));
        const auto fed = client.feed(next, 999);
        QCOMPARE(codeOf(fed), std::string(limit));
        QVERIFY2(fed.error().message.find("a tile stream that takes far more work to unpack") != std::string::npos,
                 fed.error().message.c_str());
        Client later = connected(64, 64);
        QVERIFY(okay(later.feed(many, 0)));
        QVERIFY(okay(later.feed(next, 1000))); // (a new second)
    }

    void aHextileFloodOfSubrectanglesIsCountedByThePixelsItDraws()
    {
        // finding 2: 255 subrectangles of 16x16 draw 65,280 pixels from 520 bytes; a screen of such tiles draws far
        // more than its area, which the per-second work cap refuses.
        Client client = connected(1024, 1024);
        const auto fed = client.feed(update({hextileAllSubrects(0, 0, 1024, 1024, 255)}), 0);
        QCOMPARE(codeOf(fed), std::string(limit));
        QVERIFY2(fed.error().message.find("more pixels") != std::string::npos, fed.error().message.c_str());
    }

    void anHonestFullScreenHextileIsCountedOnce()
    {
        // confirm review 4: a Hextile server whose whole screen changes counts about one screen an update (as the web
        // counts Hextile, tiles only), not two or three. Fifteen whole-screen updates in a second, under the sixteen-
        // screen cap, pass; the old double count refused such a server at five to eight.
        Client client = connected(1024, 768);
        const Picture screen = plain(1024, 768, {20, 40, 60}); // each tile a background, no subrectangles
        for (int one = 0; one < 15; ++one) {
            QVERIFY(okay(client.feed(update({hextileRect(0, 0, screen, true)}), 0)));
        }
    }

    void aServerOverItsShareIsAskedToPauseNotRefused()
    {
        // round-2 review 2: JPEGs that take longer to decode than their share allows (250 ms each, sent back to back)
        // are never refused; the client asks to be shown more slowly (pausedForMs grows), and the connection stays.
        Share share;
        share.perJpeg = 250;
        Client client = connected(64, 64, share.options());
        for (int one = 0; one < 60; ++one) {
            QVERIFY(okay(client.feed(update({tightJpeg(0, 0, 64, 64, tinyJpeg)}), share.wall)));
        }
        QVERIFY2(client.pausedForMs() > 0, QByteArray::number(client.pausedForMs()));
    }

    void manyWholeScreenJpegsASecondAreNotRefusedByTheCap()
    {
        // round-2 review 1: a JPEG counts a fill's eighth towards the per-second cap, so an honest console that sends
        // every frame as one whole-screen JPEG (obmc-ikvm, about thirty a second) is not refused by the cap. The share
        // (not exercised here: a hair of work each) holds a flood back instead.
        Share share;
        share.perJpeg = 1;
        Client client = connected(1920, 1200, share.options());
        for (int one = 0; one < 30; ++one) {
            QVERIFY(okay(client.feed(update({tightJpeg(0, 0, 1920, 1200, tinyJpeg)}), share.wall)));
        }
    }

    void anHonestJpegPaceIsNotRefusedNorPaused()
    {
        // round-2 review 2: an honest live pace of simple JPEGs (each a few ms, 25 a second) is well within the share:
        // never refused, never asked to pause.
        Share share;
        share.perJpeg = 5;
        Client client = connected(64, 64, share.options());
        for (int one = 0; one < 60; ++one) {
            share.wall += 35; // the gap between frames: 5 ms of decode and 35 of wall is 25 a second
            QVERIFY(okay(client.feed(update({tightJpeg(0, 0, 64, 64, tinyJpeg)}), share.wall)));
            QCOMPARE(client.pausedForMs(), std::int64_t{0});
        }
    }

    void anHonestFirstScreenThatTakesAWhileIsNotRefused()
    {
        // confirm review 5: one honest first screen of over a second of drawing is not refused (it may be shown more
        // slowly after); only a server whose steps take LONG_STEP of a core again and again is. No single wall strike.
        Share share;
        share.perJpeg = 1800;
        Client client = connected(64, 64, share.options());
        QVERIFY(okay(client.feed(update({tightJpeg(0, 0, 64, 64, tinyJpeg)}), share.wall)));
        // A little while later, a quiet update is fine too.
        share.perJpeg = 2;
        share.wall += 4000;
        QVERIFY(okay(client.feed(update({tightJpeg(0, 0, 64, 64, tinyJpeg)}), share.wall)));
    }

    void theTileAndRowLoopsChargeTheirDrawing()
    {
        // round-2 review 5: hextileTile, the Raw rows and Tight's drawing all ask overBudget, so their work is charged
        // and a run of long steps there refuses, as the comment claims. Driven so each such step reads a core's second:
        // after more than three, the connection is refused.
        {
            Share share;
            share.perRead = 2100; // each step (a tile) is a long one, of a core
            Client client = connected(64, 64, share.options());
            const auto fed = client.feed(update({hextileRect(0, 0, pattern(64, 64), false)}), share.wall);
            QCOMPARE(codeOf(fed), std::string(limit)); // (a 64x64 Hextile is 16 tiles: the fourth long one refuses)
            QVERIFY2(fed.error().message.find("far too long") != std::string::npos, fed.error().message.c_str());
        }
        {
            Share share;
            share.perRead = 2100;
            Client client = connected(64, 64, share.options());
            base::Result<void> fed = base::Result<void>{};
            for (int one = 0; one < 10 && fed.ok(); ++one) {
                fed =
                    client.feed(update({rawRect(0, 0, pattern(64, 64))}), share.wall); // one chunk, one long step each
            }
            QCOMPARE(codeOf(fed), std::string(limit)); // the fourth Raw update's long step refuses
        }
        {
            Share share;
            share.perRead = 2100;
            Client client = connected(64, 64, share.options());
            Deflater stream;
            base::Result<void> fed = base::Result<void>{};
            for (int one = 0; one < 10 && fed.ok(); ++one) {
                fed = client.feed(update({tightRect(0, 0, pattern(64, 64), TightFilter::Copy, stream)}), share.wall);
            }
            QCOMPARE(codeOf(fed), std::string(limit)); // the fourth Tight rectangle's long step refuses
        }
    }

    void aPushedBacklogIsDrawnWholeNotKept()
    {
        // round-4 review HIGH: a console pushing costly frames unasked must not grow what the reader keeps. What is fed
        // is drawn whole, then the screen pauses; nothing of a whole message is kept past its feed, so the only backlog
        // is the reader's queue, which the screen's hold-back counts (8 MB).
        Share share;
        share.perJpeg = 1600; // each over what the share allows at once
        Client client = connected(64, 64, share.options());
        const std::uint64_t before = client.updates();
        for (int push = 1; push <= 10; ++push) {
            QVERIFY(okay(client.feed(update({tightJpeg(0, 0, 64, 64, tinyJpeg), tightJpeg(0, 0, 64, 64, tinyJpeg),
                                             tightJpeg(0, 0, 64, 64, tinyJpeg)}),
                                     share.wall)));
            QVERIFY2(client.held() == 0, QByteArray::number(static_cast<qulonglong>(client.held()))); // nothing kept
            QCOMPARE(client.updates(), before + static_cast<std::uint64_t>(push));                    // drawn whole
            share.wall += client.pausedForMs(); // (the screen's pause)
        }
    }

    void aBatchAfterAPauseIsNotStranded()
    {
        // round-4 review MEDIUM: after a pause, an update whose first rectangle costs nothing (a Tight fill, which
        // charges no share) is drawn whole, not stopped and left with no pause to end it.
        Share share;
        share.perJpeg = 1600;
        Client client = connected(64, 64, share.options());
        QVERIFY(okay(client.feed(update({tightJpeg(0, 0, 64, 64, tinyJpeg)}), share.wall)));
        const std::int64_t pause = client.pausedForMs();
        QVERIFY(pause > 0);
        share.wall += pause; // the screen waits the pause out, earning the debt back
        const std::uint64_t before = client.updates();
        QVERIFY(okay(client.feed(update({tightFill(0, 0, 64, 64, {1, 2, 3}), tightJpeg(0, 0, 64, 64, tinyJpeg),
                                         tightJpeg(0, 0, 64, 64, tinyJpeg)}),
                                 share.wall)));
        QCOMPARE(client.updates(), before + 1); // the whole update drawn
        QVERIFY2(client.held() == 0, QByteArray::number(static_cast<qulonglong>(client.held())));
    }

    void stepsThatTakeLongAgainAndAgainAreRefused()
    {
        // confirm review 5 / the web's LONG_STEP: a server whose single steps each take LONG_STEP (2 s) or more of a
        // drawing thread's processor time, more than MOST_LONG_STEPS times, is refused even where its share is kept
        // full by long gaps between them.
        Share share;
        share.perJpeg = 2100; // over LONG_STEP, of a core
        Client client = connected(64, 64, share.options());
        base::Result<void> fed = base::Result<void>{};
        int sent = 0;
        for (; sent < 10 && fed.ok(); ++sent) {
            share.wall += 10000; // long gaps: the share is full each time, so only the long-step count can refuse
            fed = client.feed(update({tightJpeg(0, 0, 64, 64, tinyJpeg)}), share.wall);
        }
        QCOMPARE(codeOf(fed), std::string(limit));
        QVERIFY2(fed.error().message.find("far too long") != std::string::npos, fed.error().message.c_str());
        QCOMPARE(sent, 4); // the fourth long step (more than three) refuses
    }

    void aMessageUnderWaySaysSinceWhen()
    {
        Client client = connected(10, 10);
        QVERIFY(!client.messageSince().has_value());
        const Bytes said = update({rawRect(0, 0, pattern(10, 10))});
        QVERIFY(okay(client.feed(std::span(said).first(20), 100)));
        QCOMPARE(client.messageSince().value_or(-1), std::int64_t{100});
        QVERIFY(okay(client.feed(std::span(said).subspan(20, 30), 900)));
        QCOMPARE(client.messageSince().value_or(-1), std::int64_t{100});
        QVERIFY(okay(client.feed(std::span(said).subspan(50), 1000)));
        QVERIFY(!client.messageSince().has_value());
    }

    void aLargePieceGivesItsRoomBackOnceItIsDone()
    {
        // round-5 review LOW 3: a ZRLE rectangle's tile stream is read whole before it is drawn, so what the reader
        // keeps grows to it (bounded by its rectangle: four bytes a pixel, as the web); once it is drawn, the room is
        // given back rather than kept for the rest of the session, and what came after it is read on.
        // (Drawn under the drawing share as the program runs it: its work clock is the drawing thread's own processor
        // time, as the program's reader gives it (core's RfbReader, threadWorkMs), not connected()'s default, which is
        // the wall: on ARM64's debug build beside three other programs a megabyte of these tiles took over 2 s of wall,
        // a "long step" the share then refused the server for, 2026-11-19.)
        Options options = withJpeg();
        options.workClock = [] {
            FILETIME created{};
            FILETIME ended{};
            FILETIME kernel{};
            FILETIME user{};
            static_cast<void>(GetThreadTimes(GetCurrentThread(), &created, &ended, &kernel, &user));
            const auto hundredNs = [](const FILETIME& time) {
                return (static_cast<std::int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
            };
            return (hundredNs(kernel) + hundredNs(user)) / 10000;
        };
        Client client = connected(2048, 1536, std::move(options));
        // (Raw tiles, three bytes a pixel, stored: some 9 MB. Of four colours, as the tiles' maker looks for a palette
        // in every tile, slowly where every pixel is a colour of its own.)
        Deflater stream(Deflater::Way::Stored);
        const Bytes big = update({zrleRect(0, 0, quarters(2048, 1536), ZrleTiles::Raw, stream)});
        const Bytes next = update({rawRect(0, 0, pattern(8, 8, 3))});
        Bytes all = big;
        all.insert(all.end(), next.begin(), next.begin() + 10); // (the next update begun)
        constexpr std::size_t line = std::size_t{1024} * 1024;  // as a line's messages come
        std::size_t most = 0;
        for (std::size_t at = 0; at < all.size(); at += line) {
            QVERIFY(okay(client.feed(std::span(all).subspan(at, std::min(line, all.size() - at)), 0)));
            most = std::max(most, client.keptRoom());
        }
        QCOMPARE(client.updates(), std::uint64_t{1});
        QVERIFY2(most >= big.size() - line, QByteArray::number(static_cast<qulonglong>(most))); // (read whole)
        QVERIFY2(client.keptRoom() <= mostKeptRoom, QByteArray::number(static_cast<qulonglong>(client.keptRoom())));
        // (And what its stream inflated to, some 9 MB too: round-6 review LOW 3.)
        QVERIFY2(client.inflatedRoom() <= mostKeptRoom,
                 QByteArray::number(static_cast<qulonglong>(client.inflatedRoom())));
        QVERIFY(okay(client.feed(std::span(next).subspan(10), 0)));
        QCOMPARE(client.updates(), std::uint64_t{2});
        QVERIFY(shows(client, 0, 0, pattern(8, 8, 3)));
    }

    void lessThanAMegabyteOfWhatWasReadIsKeptBeforeAPieceReadWhole()
    {
        // ...and while such a piece comes, less than a megabyte of what was read before it is kept beside it (round-5
        // review LOW 3: not up to as much again). A whole Raw update of some 1.9 MB, then a ZRLE rectangle of some
        // 9 MB, fed 4.5 MB at a time: the first feed reads the update and begins the rectangle with more than it read
        // (so that letting go of what was read only once it is the larger part would keep it).
        Client client = connected(2048, 1536);
        Deflater stream(Deflater::Way::Stored);
        Bytes all = update({rawRect(0, 0, quarters(800, 600))});
        const Bytes big = update({zrleRect(0, 0, quarters(2048, 1536), ZrleTiles::Raw, stream)});
        all.insert(all.end(), big.begin(), big.end());
        constexpr std::size_t feed = std::size_t{9} * 512 * 1024;
        for (std::size_t at = 0; at < all.size(); at += feed) {
            QVERIFY(okay(client.feed(std::span(all).subspan(at, std::min(feed, all.size() - at)), 0)));
            const std::size_t read = client.keptBytes() - client.held();
            QVERIFY2(read < std::size_t{1024} * 1024, QByteArray::number(static_cast<qulonglong>(read)));
        }
        QCOMPARE(client.updates(), std::uint64_t{2});
    }

    void eachMessageIsStampedFromItsOwnStartBehindALineThatCutsAnywhere()
    {
        // round-5 review LOW 4: the stamp is cleared when a message is whole, not only when a batch ends between
        // messages. Back-to-back updates behind a line that cuts each part way: every batch ends inside a message, and
        // the one under way is stamped when it was first seen (this batch or the last), never when the first one was.
        Client client = connected(16, 16);
        Bytes stream;
        for (int one = 0; one < 20; ++one) {
            const Bytes said = update({rawRect(0, 0, pattern(4, 4, one))});
            stream.insert(stream.end(), said.begin(), said.end());
        }
        // (Half again a message, and one: the cuts never fall where a message ends.)
        const std::size_t cut = update({rawRect(0, 0, pattern(4, 4))}).size() * 3 / 2 + 1;
        std::int64_t now = 0;
        int underWay = 0;
        for (std::size_t at = 0; at < stream.size(); at += cut, now += 1000) {
            const std::size_t length = std::min(cut, stream.size() - at);
            QVERIFY(okay(client.feed(std::span(stream).subspan(at, length), now)));
            if (const std::optional<std::int64_t> since = client.messageSince()) {
                QVERIFY2(*since >= now - 1000, qPrintable(u"%1 at %2"_s.arg(*since).arg(now)));
                ++underWay;
            }
        }
        QCOMPARE(client.updates(), std::uint64_t{20});
        QVERIFY(underWay > 5); // (the line did cut inside messages)
    }

    void whatTheClientSaysGoesOnTheWire()
    {
        const Client client = connected(800, 600);
        QCOMPARE(client.request(true), (Bytes{3, 1, 0, 0, 0, 0, 0x03, 0x20, 0x02, 0x58}));
        QCOMPARE(client.request(false), (Bytes{3, 0, 0, 0, 0, 0, 0x03, 0x20, 0x02, 0x58}));
        QCOMPARE(updateRequest(false, 0, 0, 1, 1), (Bytes{3, 0, 0, 0, 0, 0, 0, 1, 0, 1}));
        QCOMPARE(keyEvent(0xFFE3, true), (Bytes{4, 1, 0, 0, 0, 0, 0xFF, 0xE3}));
        QCOMPARE(keyEvent(0x61, false), (Bytes{4, 0, 0, 0, 0, 0, 0, 0x61}));
        // The pointer is held to the screen.
        QCOMPARE(client.pointer(900, -5, 5), (Bytes{5, 5, 0x03, 0x1F, 0, 0}));
        QCOMPARE(client.pointer(10, 20, 0), (Bytes{5, 0, 0, 10, 0, 20}));
    }

    void keysAreSaidAsX11Keysyms()
    {
        QCOMPARE(keysymFor(0x04).value_or(0), 0x61U);       // a
        QCOMPARE(keysymFor(0x04, true).value_or(0), 0x41U); // A
        QCOMPARE(keysymFor(0x1E, true).value_or(0), std::uint32_t{'!'});
        QCOMPARE(keysymFor(0x27).value_or(0), std::uint32_t{'0'});
        QCOMPARE(keysymFor(0x38, true).value_or(0), std::uint32_t{'?'});
        QCOMPARE(keysymFor(0x64, true).value_or(0), std::uint32_t{'|'});
        QCOMPARE(keysymFor(0x28).value_or(0), 0xFF0DU); // Return
        QCOMPARE(keysymFor(0x4C).value_or(0), 0xFFFFU); // Delete
        QCOMPARE(keysymFor(0x3A).value_or(0), 0xFFBEU); // F1
        QCOMPARE(keysymFor(0x45).value_or(0), 0xFFC9U); // F12
        QCOMPARE(keysymFor(0x73).value_or(0), 0xFFD5U); // F24
        QCOMPARE(keysymFor(0x61).value_or(0), 0xFFB9U); // KP_9
        QCOMPARE(keysymFor(0xE3).value_or(0), 0xFFEBU); // Super_L: the Windows key
        QCOMPARE(keysymFor(0xE6).value_or(0), 0xFFEAU); // Alt_R
        QVERIFY(!keysymFor(0x66).has_value());          // Power: no keysym
        QVERIFY(!keysymFor(0x00).has_value());
        QCOMPARE(keysymOf(U'a').value_or(0), 0x61U);
        QCOMPARE(keysymOf(U'\n').value_or(0), 0xFF0DU);
        QCOMPARE(keysymOf(char32_t{0xE9}).value_or(0), 0xE9U);
        QCOMPARE(keysymOf(char32_t{0x20AC}).value_or(0), 0x010020ACU);
        QVERIFY(!keysymOf(U'\x01').has_value());
        QVERIFY(!keysymOf(char32_t{0xD800}).has_value());
    }
};

QTEST_GUILESS_MAIN(TestRfb)
#include "test_rfb.moc"
