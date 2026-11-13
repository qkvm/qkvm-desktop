#include "aspeed_frames.h"

#include <QCryptographicHash>
#include <QTest>
#include <qkvm/bmc/ami_console.h>
#include <qkvm/bmc/aspeed.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

// The console of a controller with AMI's firmware: what is sent, what arrives, how a frame is put together, and the
// keyboard and mouse reports. The web's tests/test_ami.py, test for test, and a few of the desktop's own where a bound
// of the web's had no test that names it.
using namespace qkvm::bmc;
using ami::Bytes;
using aspeed::frames::Frame;

namespace {

Bytes join(std::initializer_list<Bytes> parts)
{
    Bytes found;
    for (const Bytes& part : parts) {
        found.insert(found.end(), part.begin(), part.end());
    }
    return found;
}

Bytes counting(std::size_t many)
{
    Bytes found(many);
    for (std::size_t at = 0; at < many; ++at) {
        found[at] = static_cast<std::uint8_t>(at);
    }
    return found;
}

std::vector<ami::Message> fed(std::span<const std::uint8_t> data)
{
    auto found = ami::Messages().feed(data);
    if (!found.ok()) {
        return {};
    }
    return std::move(found).value();
}

// The text a field holds: up to the zero that ends it.
std::string field(const Bytes& body, std::size_t from, std::size_t size)
{
    std::string found;
    for (std::size_t at = from; at < from + size && body[at]; ++at) {
        found += static_cast<char>(body[at]);
    }
    return found;
}

void put16(Bytes& into, std::size_t at, int value)
{
    into[at] = static_cast<std::uint8_t>(value);
    into[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

// The facts a frame's first piece begins with, for this frame (as the web's test writes them).
Bytes facts(const Frame& frame, std::size_t length)
{
    const aspeed::FrameHeader& header = frame.header();
    Bytes found(ami::frameFacts);
    put16(found, 4, header.width);
    put16(found, 6, header.height);
    put16(found, 13, header.gridWidth);
    put16(found, 15, header.gridHeight);
    found[44] = static_cast<std::uint8_t>(header.strength);
    found[45] = header.lumaForColour ? 1 : 0;
    found[47] = static_cast<std::uint8_t>(header.correctionStrength);
    found[55] = header.halfColour ? 1 : 0;
    found[69] = static_cast<std::uint8_t>(length);
    found[70] = static_cast<std::uint8_t>(length >> 8);
    found[71] = static_cast<std::uint8_t>(length >> 16);
    return found;
}

// A video message's body: the two bytes every piece begins with, then what it carries.
Bytes piece(std::initializer_list<Bytes> parts)
{
    return join({Bytes(2), join(parts)});
}

// The frame a piece made whole, if it made one.
const ami::WholeFrame* madeWhole(const qkvm::base::Result<std::optional<ami::WholeFrame>>& read)
{
    if (!read.ok()) {
        return nullptr;
    }
    const std::optional<ami::WholeFrame>& made = read.value();
    return made ? &*made : nullptr;
}

Bytes slice(const Bytes& bytes, std::size_t from, std::size_t to)
{
    return {bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(to)};
}

// The one keyboard or mouse message a packet is, as its IUSB header of 32 bytes and the report after its length
// byte (the web's one_hid): empty where it is not one, or the header does not check out.
struct Hid {
    Bytes header;
    Bytes report;
};

Hid oneHid(const Bytes& packet)
{
    const auto found = fed(packet);
    if (found.size() != 1 || found[0].kind != ami::kind::hid || found[0].body.size() < 33) {
        return {};
    }
    const Bytes& body = found[0].body;
    const std::string signature(body.begin(), body.begin() + 8);
    unsigned sum = 0;
    for (std::size_t at = 0; at < 32; ++at) {
        sum += body[at];
    }
    if (signature != "IUSB    " || (sum & 0xFF) != 0 || body[32] != body.size() - 33) {
        return {};
    }
    return {slice(body, 0, 32), slice(body, 33, body.size())};
}

// A mouse report of an absolute mouse, read back: buttons, place across and down, wheel.
std::tuple<int, int, int, int> absolute(const Bytes& report)
{
    if (report.size() != 6) {
        return {-1, -1, -1, -1};
    }
    return {report[0], report[1] | report[2] << 8, report[3] | report[4] << 8, static_cast<std::int8_t>(report[5])};
}

} // namespace

class TestAmiConsole : public QObject {
    Q_OBJECT
private slots:
    void messagesArriveInAnyPiecesAndComeOutWhole()
    {
        const Bytes sent = join({ami::message(ami::kind::allowed), ami::message(ami::kind::passSeen, 0, Bytes{1, 3}),
                                 ami::message(ami::kind::video, 7, counting(40))});
        for (const std::size_t step : {std::size_t{1}, std::size_t{3}, std::size_t{8}, std::size_t{11}, sent.size()}) {
            ami::Messages arriving;
            std::vector<ami::Message> found;
            for (std::size_t start = 0; start < sent.size(); start += step) {
                auto some = arriving.feed(slice(sent, start, std::min(sent.size(), start + step)));
                QVERIFY(some.ok());
                for (ami::Message& one : some.value()) {
                    found.push_back(std::move(one));
                }
            }
            QCOMPARE(found.size(), std::size_t{3});
            QVERIFY(found[0].kind == ami::kind::allowed && found[0].status == 0 && found[0].body.empty());
            QVERIFY(found[1].kind == ami::kind::passSeen && found[1].status == 0 && found[1].body.size() == 2);
            QVERIFY(found[2].kind == ami::kind::video && found[2].status == 7 && found[2].body.size() == 40);
            QCOMPARE(found[2].body, counting(40));
            QCOMPARE(arriving.held(), std::size_t{0});
        }
    }

    void aMessageIsItsHeadAndItsBody()
    {
        // (The desktop's own, as the web's writes it: kind, length and status, least significant byte first.)
        QCOMPARE(ami::message(ami::kind::video, 7, counting(5)),
                 (Bytes{0x19, 0x00, 0x05, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04}));
    }

    void aMessageThatSaysItIsLongerThanAnyIsNotWaitedFor()
    {
        // Believed, it would have the program keep whatever arrives for as long as the controller cares to send.
        Bytes said = {ami::kind::video, 0};
        const auto length = static_cast<std::uint32_t>(ami::longest + 1);
        for (int shift = 0; shift < 32; shift += 8) {
            said.push_back(static_cast<std::uint8_t>(length >> shift));
        }
        said.insert(said.end(), 2, 0);
        const auto refused = ami::Messages().feed(said);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("garbled"));
        // (One of just the longest there may be is waited for, and is taken in once it is all there.)
        Bytes longestOne = said;
        longestOne[2] = static_cast<std::uint8_t>(ami::longest);
        longestOne[3] = static_cast<std::uint8_t>(ami::longest >> 8);
        longestOne[4] = static_cast<std::uint8_t>(ami::longest >> 16);
        longestOne[5] = static_cast<std::uint8_t>(ami::longest >> 24);
        ami::Messages arriving;
        const auto waits = arriving.feed(longestOne);
        QVERIFY(waits.ok() && waits.value().empty());
        const auto whole = arriving.feed(Bytes(ami::longest));
        QVERIFY(whole.ok() && whole.value().size() == 1 && whole.value()[0].body.size() == ami::longest);
    }

    void theAnswerToAllowedIsThePassAndSendThePicture()
    {
        const auto said = fed(ami::hello("a-console-pass", "192.168.1.20", "192.168.1.50"));
        QCOMPARE(said.size(), std::size_t{2});
        const ami::Message& first = said[0];
        QVERIFY(first.kind == ami::kind::pass && first.status == 1 && first.body.size() == 438);
        QVERIFY(said[1].kind == ami::kind::resume && said[1].body.empty());
        QCOMPARE(first.body[0], std::uint8_t{0});
        QCOMPARE(field(first.body, 1, 129), std::string("a-console-pass"));
        QCOMPARE(field(first.body, 130, 65), std::string("192.168.1.20"));
        QCOMPARE(field(first.body, 195, 129), std::string("domain/username"));
        QCOMPARE(field(first.body, 324, 49), std::string("00-00-00-00-00-00"));
        QCOMPARE(field(first.body, 373, 65), std::string("192.168.1.50"));
        // A field is never filled to its last byte: what reads it there looks for the zero that ends it.
        const auto longPass = fed(ami::hello(std::string(500, 'p'), "", ""));
        QCOMPARE(longPass.size(), std::size_t{2});
        QCOMPARE(longPass[0].body[129], std::uint8_t{0});
        QCOMPARE(field(longPass[0].body, 1, 129), std::string(128, 'p'));
    }

    void aCharacterThatIsNotAsciiGoesAsAQuestionMark()
    {
        // (The desktop's own, as the web's writes it: "p", a with two dots, the euro's sign, "ss" become "p??ss".)
        std::string pass = "p";
        for (const int byte : {0xC3, 0xA4, 0xE2, 0x82, 0xAC}) {
            pass += static_cast<char>(byte);
        }
        pass += "ss";
        const auto said = fed(ami::hello(pass, "192.168.1.20", "192.168.1.50"));
        QCOMPARE(said.size(), std::size_t{2});
        QCOMPARE(field(said[0].body, 1, 129), std::string("p??ss"));
    }

    void firmwareThatLetsAConsoleReconnectIsToldTheConnectionIsCompleteFirst()
    {
        // A real controller said "allowed", was shown the pass, and closed without a word: its firmware wants this
        // first.
        const auto said = fed(ami::hello("a-pass", "192.168.1.20", "192.168.1.50", ami::defaultName, true));
        QCOMPARE(said.size(), std::size_t{3});
        QVERIFY(said[0].kind == ami::kind::complete && said[0].status == 1 && said[0].body.empty());
        QVERIFY(said[1].kind == ami::kind::pass && said[1].status == 1 && said[1].body.size() == 438);
        QVERIFY(said[2].kind == ami::kind::resume && said[2].status == 0 && said[2].body.empty());
        const auto bye = fed(ami::goodbye());
        QVERIFY(bye.size() == 1 && bye[0].kind == ami::kind::stop && bye[0].status == 0 && bye[0].body.empty());
        // ...and every byte of it as the web's writes it.
        const Bytes all = ami::hello("a-console-pass", "192.168.1.20", "192.168.1.50", ami::defaultName, true);
        QCOMPARE(QCryptographicHash::hash(QByteArrayView(all.data(), static_cast<qsizetype>(all.size())),
                                          QCryptographicHash::Sha256)
                     .toHex(),
                 "62e9d0ec6f5f1d6fdbcff14596a534933f52675cb20aa42026644c65d974852e");
    }

    void whoeverAsksForTheConsoleIsLetInTheWayTheControllersOwnPageDoesIt()
    {
        // Said twice over, with who is asking as the controller named them: once as the answer, once as "theirs now".
        Bytes who(64 + 70);
        const std::string name = "admin";
        const std::string address = "192.168.1.60";
        std::copy(name.begin(), name.end(), who.begin());
        std::copy(address.begin(), address.end(), who.begin() + 64);
        const auto said = fed(ami::letIn(who));
        QCOMPARE(said.size(), std::size_t{2});
        QVERIFY(said[0].kind == ami::kind::sharing && said[0].status == 1 && said[0].body == who);
        QVERIFY(said[1].kind == ami::kind::nextHolder && said[1].status == 1 && said[1].body == who);
        // What the numbers in a real board's own words were: "the one who held it is coming back" came as 1796.
        QCOMPARE(ami::sharing::answered | ami::answer::holderReturning << 8, 1796u);
    }

    void aLicenceIsAnsweredWithTheWebSession()
    {
        // (The desktop's own, as the web's writes it: its text in 35 bytes, padded, and filled to the last.)
        const auto said = fed(ami::answerToLicence("1234"));
        QCOMPARE(said.size(), std::size_t{1});
        QVERIFY(said[0].kind == ami::kind::webSession && said[0].status == 0 && said[0].body.size() == 35);
        QCOMPARE(field(said[0].body, 0, 35), std::string("1234"));
        const auto full = fed(ami::answerToLicence(std::string(50, 's')));
        QCOMPARE(full[0].body, Bytes(35, 's'));
        QCOMPARE(ami::answerToLicence("12"),
                 ami::message(ami::kind::webSession, 0, join({Bytes{'1', '2'}, Bytes(33)})));
    }

    void moreInOneGoThanAConsoleSendsIsRefusedBeforeItIsKept()
    {
        // (A review of the desktop's.) One message of the WebSocket is no longer than the web's socket takes.
        ami::Messages arriving;
        const auto refused = arriving.feed(Bytes(ami::largestArrival + 1));
        QVERIFY(!refused.ok());
        QCOMPARE(arriving.held(), std::size_t{0});
        // As much as may arrive in one go: the longest message, and seven of nothing.
        Bytes most = ami::message(ami::kind::video, 0, Bytes(ami::longest));
        most.resize(ami::largestArrival);
        const auto taken = ami::Messages().feed(most);
        QVERIFY(taken.ok() && taken.value().size() == 8 && taken.value()[0].body.size() == ami::longest);
    }

    void aKeyboardReportIsTheModifiersAndTheKeysDown()
    {
        const Hid hid = oneHid(ami::keyboard(0x05, Bytes{0x04, 0x05}, 7));
        QCOMPARE(hid.header.size(), std::size_t{32});              // (a keyboard message, whose header checks out)
        QVERIFY(hid.header[17] == 0x30 && hid.header[18] == 0x10); // for the virtual USB keyboard
        // The modifier byte, a reserved zero, then the keys down in six places.
        QCOMPARE(hid.report, (Bytes{0x05, 0, 0x04, 0x05, 0, 0, 0, 0}));
        // The whole keyboard every time: nothing down is an empty report, and no more than six keys go (the first six
        // given; whoever keeps the keys down hands over the six newest, as a keyboard does).
        QCOMPARE(oneHid(ami::keyboard(0, {}, 0)).report, Bytes(8));
        QCOMPARE(slice(oneHid(ami::keyboard(0, Bytes{4, 5, 6, 7, 8, 9, 10}, 0)).report, 2, 8),
                 (Bytes{4, 5, 6, 7, 8, 9}));
        // (The desktop's own: byte for byte what the web's writes, made by its ami.keyboard(0x05, [4, 5], 7) and
        // ami.keyboard(0xFF, [0x1FF], 0x80000005), whose number goes in 31 bits.)
        QCOMPARE(
            ami::keyboard(0x05, Bytes{0x04, 0x05}, 7),
            (Bytes{0x01, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x55, 0x53, 0x42, 0x20, 0x20, 0x20, 0x20, 0x01,
                   0x00, 0x20, 0x5A, 0x09, 0x00, 0x00, 0x00, 0x00, 0x30, 0x10, 0x80, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x05, 0x00, 0x04, 0x05, 0x00, 0x00, 0x00, 0x00}));
        QCOMPARE(
            ami::keyboard(0xFF, Bytes{0xFF}, 0x80000005),
            (Bytes{0x01, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x55, 0x53, 0x42, 0x20, 0x20, 0x20, 0x20, 0x01,
                   0x00, 0x20, 0x5C, 0x09, 0x00, 0x00, 0x00, 0x00, 0x30, 0x10, 0x80, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00}));
    }

    void theSequenceNumberGoesUpAndTheChecksumCatchesAChangedByte()
    {
        // The controller checks that the header sums to zero: a byte changed on the way is caught. And every report
        // carries its number, as the controller's own viewer sends them.
        QVERIFY(ami::keyboard(0, Bytes{4}, 1) != ami::keyboard(0, Bytes{4}, 2));
        Bytes bad = ami::keyboard(0, Bytes{4}, 1);
        QCOMPARE(bad.size(), std::size_t{49});
        bad[8 + 20] ^= 0x01; // a byte of the IUSB header (past the message's own eight-byte head)
        unsigned sum = 0;
        for (std::size_t at = 8; at < 8 + 32; ++at) {
            sum += bad[at];
        }
        QVERIFY((sum & 0xFF) != 0);
        QVERIFY(oneHid(bad).header.empty());
    }

    void anAbsoluteMouseReportCarriesThePlace()
    {
        const Hid hid = oneHid(ami::mouseAbsolute(0x01, 16384, 32767, 0, 3));
        QCOMPARE(hid.header.size(), std::size_t{32});
        QVERIFY(hid.header[17] == 0x31 && hid.header[18] == 0x20); // for the virtual USB mouse
        QCOMPARE(absolute(hid.report), std::make_tuple(0x01, 16384, 32767, 0));
        // A place past the far edge is held at it, and one before the near edge at that.
        QCOMPARE(absolute(oneHid(ami::mouseAbsolute(0, 99999, -5, -1, 0)).report), std::make_tuple(0, 0x7FFF, 0, -1));
        // (The desktop's own: a wheel is held to a signed byte, and the bytes are the web's, made by its
        // ami.mouse_abs(1, 16384, 32767, 0, 3) and ami.mouse_abs(7, -1, 40000, -300, 0xFFFFFFFF).)
        QCOMPARE(absolute(oneHid(ami::mouseAbsolute(0, 0, 0, 300, 0)).report), std::make_tuple(0, 0, 0, 127));
        QCOMPARE(ami::mouseAbsolute(0x01, 16384, 32767, 0, 3),
                 (Bytes{0x01, 0x00, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x55, 0x53, 0x42, 0x20, 0x20, 0x20, 0x20,
                        0x01, 0x00, 0x20, 0x4E, 0x07, 0x00, 0x00, 0x00, 0x00, 0x31, 0x20, 0x80, 0x02, 0x01, 0x00, 0x00,
                        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0x40, 0xFF, 0x7F, 0x00}));
        // A relative report is four bytes of buttons and a signed move (the web's), each held to a signed byte; the
        // bytes are the web's, made by its ami.mouse_rel(4, -3, 7, 1, 9) and ami.mouse_rel(0, 300, -300, -200,
        // 0x80000001).
        const Hid relative = oneHid(ami::mouseRelative(0x04, -3, 7, 1, 9));
        QVERIFY(relative.header[17] == 0x31 && relative.header[18] == 0x20);
        QCOMPARE(relative.report, (Bytes{0x04, 0xFD, 0x07, 0x01}));
        QCOMPARE(ami::mouseRelative(0x04, -3, 7, 1, 9),
                 (Bytes{0x01, 0x00, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x55, 0x53, 0x42, 0x20, 0x20, 0x20,
                        0x20, 0x01, 0x00, 0x20, 0x4A, 0x05, 0x00, 0x00, 0x00, 0x00, 0x31, 0x20, 0x80, 0x02, 0x01,
                        0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0xFD, 0x07, 0x01}));
        QCOMPARE(ami::mouseRelative(0, 300, -300, -200, 0x80000001),
                 (Bytes{0x01, 0x00, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x55, 0x53, 0x42, 0x20, 0x20, 0x20,
                        0x20, 0x01, 0x00, 0x20, 0x52, 0x05, 0x00, 0x00, 0x00, 0x00, 0x31, 0x20, 0x80, 0x02, 0x01,
                        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x7F, 0x81, 0x81}));
        QCOMPARE(ami::mouseAbsolute(7, -1, 40000, -300, 0xFFFFFFFF),
                 (Bytes{0x01, 0x00, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x55, 0x53, 0x42, 0x20, 0x20, 0x20, 0x20,
                        0x01, 0x00, 0x20, 0xD5, 0x07, 0x00, 0x00, 0x00, 0x00, 0x31, 0x20, 0x80, 0x02, 0x01, 0x00, 0x00,
                        0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x06, 0x07, 0x00, 0x00, 0xFF, 0x7F, 0x81}));
    }

    void aSessionEndsInWordsForTheLog()
    {
        // (The desktop's own: the web's table, number for number.)
        QCOMPARE(ami::endedBecause(ami::timeUp), std::string_view("the session's time was up"));
        QCOMPARE(ami::endedBecause(7), std::string_view("its web session was signed out"));
        QCOMPARE(ami::endedBecause(13), std::string_view("the controller was put back to how it came"));
        for (const std::uint16_t unknown : {0, 1, 3, 4, 6, 14, 0xFFFF}) {
            QVERIFY(ami::endedBecause(unknown).empty());
        }
    }

    void aFrameIsPutTogetherFromItsPieces()
    {
        Frame frame(24, 8, {.strength = 2, .correctionStrength = 6, .lumaForColour = true});
        for (const std::uint32_t colour : {0xEB8080u, 0x108080u, 0x515AF0u}) {
            frame.colours({{0, colour}});
        }
        const Bytes data = frame.data();
        ami::Frames frames;
        const auto first = frames.piece(piece({facts(frame, data.size()), slice(data, 0, 5)}));
        QVERIFY(first.ok() && !first.value());
        const auto second = frames.piece(piece({slice(data, 5, 9)}));
        QVERIFY(second.ok() && !second.value());
        QCOMPARE(frames.held(), std::size_t{9});
        const auto third = frames.piece(piece({slice(data, 9, data.size())}));
        QVERIFY(third.ok() && third.value());
        const ami::WholeFrame& whole = *third.value();
        QVERIFY(whole.header == frame.header());
        QCOMPARE(whole.data, data);
        QCOMPARE(frames.held(), std::size_t{0});
        aspeed::Screen screen;
        const auto read = screen.read(whole.header, whole.data);
        QVERIFY(read.ok() && read.value() == 3);
        QCOMPARE(slice(screen.picture().rgb, 0, 3), (Bytes{255, 255, 255}));
        // ...and the next piece begins the next frame.
        const auto again = frames.piece(piece({facts(frame, data.size()), data}));
        QVERIFY(madeWhole(again) && madeWhole(again)->data == data);
    }

    void aPieceThatBringsMoreThanItsFrameNeedsIsCutWhereTheFrameEnds()
    {
        // (The desktop's own, as the web's does it; and the rest is not kept.)
        Frame frame(8, 8);
        frame.colours({{0, 0x108080}});
        const Bytes data = frame.data();
        ami::Frames frames;
        const auto whole = frames.piece(piece({facts(frame, data.size()), data, Bytes(1000, 0xAA)}));
        QVERIFY(madeWhole(whole) && madeWhole(whole)->data == data);
        QCOMPARE(frames.held(), std::size_t{0});
        // A frame of no bytes is whole at once.
        const auto empty = frames.piece(piece({facts(frame, 0)}));
        QVERIFY(madeWhole(empty) && madeWhole(empty)->data.empty());
    }

    void aPictureThatIsSentEncryptedIsSaidToBeSo()
    {
        Frame frame(8, 8);
        frame.colours({{0, 0x108080}});
        const Bytes data = frame.data();
        Bytes locked = facts(frame, data.size());
        locked[53] = 1;
        const auto refused = ami::Frames().piece(piece({locked, data}));
        QVERIFY(!refused.ok());
        QVERIFY(QByteArray::fromStdString(refused.error().message).contains("encrypted"));
        // A first piece too short to say how the frame is to be read.
        QVERIFY(!ami::Frames().piece(piece({Bytes(20)})).ok());
        QVERIFY(!ami::Frames().piece(piece({Bytes(ami::frameFacts - 1)})).ok());
        QVERIFY(!ami::Frames().piece(Bytes{}).ok());
        QVERIFY(!ami::Frames().piece(Bytes{0}).ok());
        QVERIFY(ami::Frames().piece(piece({Bytes(ami::frameFacts)})).ok()); // (just enough: a frame of nothing)
    }

    void moreMessagesAtOnceThanAConsoleSendsAreNotTakenIn()
    {
        // Found by a review of the web's, and measured: one thing sent over the connection may be megabytes, the
        // smallest message is eight bytes, and every one of four million was made into an object, on the thread
        // that served every browser. Nothing was served for nine seconds.
        Bytes many;
        for (std::size_t one = 0; one < ami::mostAtOnce; ++one) {
            const Bytes still = ami::message(ami::kind::stillThere);
            many.insert(many.end(), still.begin(), still.end());
        }
        const auto taken = ami::Messages().feed(many);
        QVERIFY(taken.ok() && taken.value().size() == ami::mostAtOnce);
        const Bytes still = ami::message(ami::kind::stillThere);
        many.insert(many.end(), still.begin(), still.end());
        QVERIFY(!ami::Messages().feed(many).ok());
        // (What is left over from one time is the beginning of the next, as before.)
        ami::Messages arriving;
        const Bytes whole = ami::message(ami::kind::video, 0, counting(50));
        const auto before = arriving.feed(join({ami::message(ami::kind::allowed), slice(whole, 0, 20)}));
        QVERIFY(before.ok() && before.value().size() == 1);
        QCOMPARE(arriving.held(), std::size_t{20});
        const auto after = arriving.feed(slice(whole, 20, whole.size()));
        QVERIFY(after.ok() && after.value().size() == 1 && after.value()[0].body == counting(50));
        // ...and 256 whole ones with the beginning of another are taken in, the beginning kept.
        Bytes more = slice(many, 0, ami::mostAtOnce * ami::headBytes);
        more.insert(more.end(), whole.begin(), whole.begin() + 10);
        ami::Messages last;
        const auto most = last.feed(more);
        QVERIFY(most.ok() && most.value().size() == ami::mostAtOnce);
        QCOMPARE(last.held(), std::size_t{10});
    }

    void aFrameThatSaysItIsLongerThanAnyIsNotCollected()
    {
        // The web's put its pieces together up to sixteen megabytes before anything looked at how long that is.
        Frame frame(24, 8);
        frame.colours({{0, 0xEB8080}});
        const Bytes data = frame.data();
        const auto refused = ami::Frames().piece(piece({facts(frame, ami::longest + 1), data}));
        QVERIFY(!refused.ok());
        QVERIFY(QByteArray::fromStdString(refused.error().message).contains("longer"));
        ami::Frames frames; // (one of just the longest there may be is collected)
        const auto waits = frames.piece(piece({facts(frame, ami::longest), data}));
        QVERIFY(waits.ok() && !waits.value());
        const auto whole = frames.piece(piece({Bytes(ami::longest)}));
        QVERIFY(madeWhole(whole) && madeWhole(whole)->data.size() == ami::longest);
    }

    void theHeaderOfAFrameIsReadFromItsFacts()
    {
        // (The desktop's own, as the web's reads them: the two bytes of a size, and a one where a yes is.)
        Frame frame(300, 200, {.halfColour = true, .strength = 7, .correctionStrength = 0});
        Bytes said = facts(frame, 0);
        said[55] = 2; // (not one: not half the detail)
        said[45] = 1;
        put16(said, 4, 0xFFFF);
        const auto read = ami::Frames().piece(piece({said}));
        QVERIFY(read.ok() && read.value());
        const aspeed::FrameHeader& header = madeWhole(read)->header;
        QVERIFY(header.width == 0xFFFF && header.height == 200 && header.gridWidth == 304 && header.gridHeight == 208);
        QVERIFY(!header.halfColour && header.lumaForColour && header.strength == 7 && header.correctionStrength == 0);
        said[44] = 255;
        const auto strong = ami::Frames().piece(piece({said}));
        QVERIFY(madeWhole(strong) && madeWhole(strong)->header.strength == 255);
        said[55] = 1; // ...and the other way round: half the detail, and not the brightness table for colour
        said[45] = 2;
        const auto other = ami::Frames().piece(piece({said}));
        QVERIFY(madeWhole(other) && madeWhole(other)->header.halfColour && !madeWhole(other)->header.lumaForColour);
    }
};

QTEST_GUILESS_MAIN(TestAmiConsole)
#include "test_ami_console.moc"
