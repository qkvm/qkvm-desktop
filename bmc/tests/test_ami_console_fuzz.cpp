#include "allocations.h"
#include "aspeed_frames.h"
#include "chance.h"

#include <QTest>
#include <qkvm/bmc/ami_console.h>
#include <qkvm/bmc/aspeed.h>

#include <algorithm>
#include <chrono>
#include <vector>

// What a controller in the wrong hands could send its console, made by chance and the same at every run: streams
// of messages cut anywhere, damaged, with heads that say anything, and frames in pieces whose facts are at their
// edges. Every reading is held to the reader's promises: what comes out is what went in, no more messages at once
// than a console sends, nothing kept beyond one message (or one frame) not yet whole, no more asked for than what
// arrived needs, and no reading longer than its bytes take; the address sanitizer's build looks at every access.
using namespace qkvm::bmc;
using ami::Bytes;
using aspeed::frames::Frame;
using tests::asked;
using tests::Chance;
using tests::Watch;

namespace {

void put16(Bytes& into, std::size_t at, unsigned value)
{
    into[at] = static_cast<std::uint8_t>(value);
    into[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

Bytes someBytes(Chance& chance, std::size_t many)
{
    Bytes found(many);
    for (std::uint8_t& byte : found) {
        byte = static_cast<std::uint8_t>(chance.below(256));
    }
    return found;
}

// A message as a console sends them: mostly small, now and then a large piece of a frame.
ami::Message someMessage(Chance& chance)
{
    static constexpr std::array<std::uint16_t, 8> kinds = {
        ami::kind::video,      ami::kind::stillThere, ami::kind::sharing, ami::kind::passSeen,
        ami::kind::nextHolder, ami::kind::stop,       ami::kind::allowed, 0xFFFF};
    const std::uint16_t kind = kinds[static_cast<std::size_t>(chance.below(static_cast<int>(kinds.size())))];
    const auto status = static_cast<std::uint16_t>(chance.oneIn(4) ? chance.below(65536) : chance.below(8));
    const std::size_t length =
        chance.oneIn(40) ? static_cast<std::size_t>(chance.below(70000)) : static_cast<std::size_t>(chance.below(300));
    return {kind, status, someBytes(chance, length)};
}

// The same stream, cut where chance says (now and then all at once).
std::vector<Bytes> cut(Chance& chance, const Bytes& stream)
{
    std::vector<Bytes> pieces;
    for (std::size_t at = 0; at < stream.size();) {
        const std::size_t step = chance.oneIn(30)  ? stream.size()
                                 : chance.oneIn(3) ? static_cast<std::size_t>(chance.between(1, 16))
                                                   : static_cast<std::size_t>(chance.between(1, 9000));
        const std::size_t to = std::min(stream.size(), at + step);
        pieces.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(at),
                            stream.begin() + static_cast<std::ptrdiff_t>(to));
        at = to;
    }
    return pieces;
}

double msSince(std::chrono::steady_clock::time_point began)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
}

// How long a reading may take: a moment, and a millisecond for every ten thousand bytes it was given and kept.
double mayTakeMs(std::size_t bytes)
{
    return 100 + static_cast<double>(bytes) / 10000;
}

// Feeds one piece and holds the reading to every promise. Empty when it kept them all; `refused` says whether it
// was refused (and then nothing more is fed, as the program reads no further).
QByteArray feedHeld(ami::Messages& arriving, const Bytes& piece, std::vector<ami::Message>& out, bool& refused)
{
    const std::size_t before = arriving.held();
    const auto began = std::chrono::steady_clock::now();
    auto found = [&] {
        const Watch watch;
        return arriving.feed(piece);
    }();
    const double took = msSince(began);
    const std::size_t given = before + piece.size();
    if (took > mayTakeMs(given)) {
        return "a feed of " + QByteArray::number(static_cast<qulonglong>(given)) + " bytes took " +
               QByteArray::number(took) + " ms";
    }
    // What is asked for: the bytes kept (a vector that grows asks for half as much again), the bodies made of
    // them, and the list of them.
    const std::size_t mayAsk = 2 * given + given + std::size_t{64} * 1024;
    if (asked > mayAsk) {
        return "a feed of " + QByteArray::number(static_cast<qulonglong>(given)) + " bytes asked for " +
               QByteArray::number(static_cast<qulonglong>(asked));
    }
    refused = !found.ok();
    if (refused) {
        return found.error().code == "garbled" ? QByteArray() : QByteArray("refused as ") + found.error().code.c_str();
    }
    if (found.value().size() > ami::mostAtOnce) {
        return "more messages at once than a console sends";
    }
    if (arriving.held() >= ami::headBytes + ami::longest) {
        return "more is kept than one message not yet whole";
    }
    for (ami::Message& one : found.value()) {
        if (one.body.size() > ami::longest) {
            return "a message longer than any";
        }
        out.push_back(std::move(one));
    }
    return {};
}

struct Seen {
    int feeds = 0;
    int refused = 0;
    std::size_t messages = 0;
};

void say(const char* part, const Seen& seen)
{
    qInfo("%s: %d feeds, %d refused, %llu messages out", part, seen.feeds, seen.refused,
          static_cast<unsigned long long>(seen.messages));
}

// The facts of a frame's first piece, as a controller writes them.
Bytes factsOf(const aspeed::FrameHeader& header, std::size_t length)
{
    Bytes found(ami::frameFacts);
    put16(found, 4, static_cast<unsigned>(header.width));
    put16(found, 6, static_cast<unsigned>(header.height));
    put16(found, 13, static_cast<unsigned>(header.gridWidth));
    put16(found, 15, static_cast<unsigned>(header.gridHeight));
    found[44] = static_cast<std::uint8_t>(header.strength);
    found[45] = header.lumaForColour ? 1 : 0;
    found[47] = static_cast<std::uint8_t>(header.correctionStrength);
    found[55] = header.halfColour ? 1 : 0;
    found[69] = static_cast<std::uint8_t>(length);
    found[70] = static_cast<std::uint8_t>(length >> 8);
    found[71] = static_cast<std::uint8_t>(length >> 16);
    return found;
}

Frame someFrame(Chance& chance)
{
    const int width = chance.between(1, 160);
    const int height = chance.between(1, 120);
    Frame frame(width, height, {.strength = chance.below(8), .correctionStrength = chance.below(8)});
    for (int block = chance.below(40); block > 0; --block) {
        std::vector<int> pixels(64);
        for (int& pixel : pixels) {
            pixel = chance.below(2);
        }
        frame.colours({{chance.below(4), static_cast<std::uint32_t>(chance.next() & 0xFFFFFF)}, {chance.below(4), {}}},
                      pixels);
    }
    return frame;
}

} // namespace

class TestAmiConsoleFuzz : public QObject {
    Q_OBJECT
private slots:
    void messagesMadeAndCutAnyWayComeOutAsTheyWent()
    {
        Chance chance(20261029);
        Seen seen;
        for (int round = 0; round < 400; ++round) {
            std::vector<ami::Message> sent;
            Bytes stream;
            // Now and then more small ones than a console sends at once, which a piece of them all is refused for.
            const bool flood = chance.oneIn(10);
            const int many = flood ? static_cast<int>(ami::mostAtOnce) + chance.between(1, 40) : chance.between(1, 60);
            for (int one = 0; one < many; ++one) {
                ami::Message message = flood ? ami::Message{ami::kind::stillThere, 0, {}} : someMessage(chance);
                const Bytes written = ami::message(message.kind, message.status, message.body);
                stream.insert(stream.end(), written.begin(), written.end());
                sent.push_back(std::move(message));
            }
            ami::Messages arriving;
            std::vector<ami::Message> out;
            bool refused = false;
            for (const Bytes& piece : cut(chance, stream)) {
                const std::size_t ready = out.size();
                const QByteArray wrong = feedHeld(arriving, piece, out, refused);
                ++seen.feeds;
                QVERIFY2(wrong.isEmpty(), wrong);
                if (refused) {
                    ++seen.refused;
                    // ...only for more at once than a console sends: the whole messages this piece completed.
                    QVERIFY(flood);
                    std::size_t whole = 0;
                    for (std::size_t at = ready, length = 0; at < sent.size(); ++at) {
                        length += ami::headBytes + sent[at].body.size();
                        if (length > arriving.held()) {
                            break;
                        }
                        ++whole;
                    }
                    QVERIFY(whole > ami::mostAtOnce);
                    break;
                }
            }
            seen.messages += out.size();
            if (refused) {
                continue;
            }
            QCOMPARE(out.size(), sent.size());
            for (std::size_t at = 0; at < sent.size(); ++at) {
                QVERIFY(out[at].kind == sent[at].kind && out[at].status == sent[at].status &&
                        out[at].body == sent[at].body);
            }
            QCOMPARE(arriving.held(), std::size_t{0});
        }
        say("messages made and cut", seen);
    }

    void streamsDamagedEveryWhichWay()
    {
        Chance chance(5);
        Seen seen;
        for (int round = 0; round < 700; ++round) {
            Bytes stream;
            for (int one = chance.between(1, 30); one > 0; --one) {
                const ami::Message message = someMessage(chance);
                const Bytes written = ami::message(message.kind, message.status, message.body);
                stream.insert(stream.end(), written.begin(), written.end());
            }
            for (int way = chance.between(1, 4); way > 0 && !stream.empty(); --way) {
                const auto at = static_cast<std::size_t>(chance.below(static_cast<int>(stream.size())));
                switch (chance.below(5)) {
                case 0: // bits flipped
                    stream[at] ^= static_cast<std::uint8_t>(1u << chance.below(8));
                    break;
                case 1: // cut short
                    stream.resize(at);
                    break;
                case 2: { // bytes put in
                    const Bytes more = someBytes(chance, static_cast<std::size_t>(chance.between(1, 40)));
                    stream.insert(stream.begin() + static_cast<std::ptrdiff_t>(at), more.begin(), more.end());
                    break;
                }
                case 3: { // a length that says anything, where a length may be
                    static constexpr std::array<std::uint32_t, 6> lengths = {0,        1,        0x7FFFFF,
                                                                             0x800000, 0x800001, 0xFFFFFFFF};
                    const std::uint32_t length = lengths[static_cast<std::size_t>(chance.below(6))];
                    if (at + 6 <= stream.size()) {
                        for (std::size_t byte = 0; byte < 4; ++byte) {
                            stream[at + 2 + byte] = static_cast<std::uint8_t>(length >> (8 * byte));
                        }
                    }
                    break;
                }
                default: // bytes taken out
                    stream.erase(stream.begin() + static_cast<std::ptrdiff_t>(at),
                                 stream.begin() +
                                     static_cast<std::ptrdiff_t>(std::min(
                                         stream.size(), at + static_cast<std::size_t>(chance.between(1, 40)))));
                    break;
                }
            }
            ami::Messages arriving;
            std::vector<ami::Message> out;
            bool refused = false;
            for (const Bytes& piece : cut(chance, stream)) {
                const QByteArray wrong = feedHeld(arriving, piece, out, refused);
                ++seen.feeds;
                QVERIFY2(wrong.isEmpty(), wrong);
                if (refused) {
                    ++seen.refused;
                    break;
                }
            }
            seen.messages += out.size();
        }
        say("streams damaged", seen);
    }

    void headsAtTheirEdges()
    {
        // A head alone, then a part of its body, then the rest: refused at once if it says more than any message
        // may be, else waited for, and taken in once it is all there.
        Seen seen;
        for (const std::uint32_t length : {0u, 1u, 7u, 8u, 65535u, 65536u, static_cast<std::uint32_t>(ami::longest),
                                           static_cast<std::uint32_t>(ami::longest + 1), 0x7FFFFFFFu, 0xFFFFFFFFu}) {
            for (const std::uint16_t kind : {std::uint16_t{0}, ami::kind::video, std::uint16_t{0xFFFF}}) {
                Bytes head(ami::headBytes);
                put16(head, 0, kind);
                put16(head, 2, length & 0xFFFF);
                put16(head, 4, length >> 16);
                put16(head, 6, 0xFFFF);
                ami::Messages arriving;
                std::vector<ami::Message> out;
                bool refused = false;
                QByteArray wrong = feedHeld(arriving, head, out, refused);
                ++seen.feeds;
                QVERIFY2(wrong.isEmpty(), wrong);
                QCOMPARE(refused, length > ami::longest);
                if (refused) {
                    ++seen.refused;
                    continue;
                }
                const std::size_t body = length;
                if (body && (body <= 65536 || kind == ami::kind::video)) { // (the longest, once a length)
                    wrong = feedHeld(arriving, Bytes(body - 1), out, refused);
                    QVERIFY2(wrong.isEmpty() && !refused && out.empty(), wrong);
                    QCOMPARE(arriving.held(), ami::headBytes + body - 1);
                    wrong = feedHeld(arriving, Bytes(1), out, refused);
                    QVERIFY2(wrong.isEmpty() && !refused, wrong);
                }
                if (body <= 65536 || kind == ami::kind::video) {
                    QCOMPARE(out.size(), std::size_t{1});
                    QVERIFY(out[0].kind == kind && out[0].status == 0xFFFF && out[0].body.size() == body);
                    QCOMPARE(arriving.held(), std::size_t{0});
                }
                seen.messages += out.size();
            }
        }
        say("heads at their edges", seen);
    }

    void framesInPiecesWithFactsAtTheirEdges()
    {
        Chance chance(77);
        int frames = 0;
        int refused = 0;
        for (int round = 0; round < 1500; ++round) {
            const Frame frame = someFrame(chance);
            const Bytes data = frame.data();
            aspeed::FrameHeader header = frame.header();
            std::size_t length = data.size();
            Bytes facts = factsOf(header, length);
            bool shouldRefuse = false;
            switch (chance.below(8)) {
            case 0: // encrypted
                facts[53] = static_cast<std::uint8_t>(chance.between(1, 255));
                shouldRefuse = true;
                break;
            case 1: { // longer than any frame may be, or just as long
                length = chance.oneIn(2) ? ami::longest + static_cast<std::size_t>(chance.between(1, 0x7FFFFF))
                                         : ami::longest;
                facts[69] = static_cast<std::uint8_t>(length);
                facts[70] = static_cast<std::uint8_t>(length >> 8);
                facts[71] = static_cast<std::uint8_t>(length >> 16);
                shouldRefuse = length > ami::longest;
                break;
            }
            case 2: // fields that say anything
                for (const std::size_t at : {4, 6, 13, 15}) {
                    put16(facts, static_cast<std::size_t>(at), static_cast<unsigned>(chance.below(65536)));
                }
                facts[44] = static_cast<std::uint8_t>(chance.below(256));
                facts[47] = static_cast<std::uint8_t>(chance.below(256));
                facts[45] = static_cast<std::uint8_t>(chance.below(3));
                facts[55] = static_cast<std::uint8_t>(chance.below(3));
                break;
            case 3: // shorter than it should be
                length = static_cast<std::size_t>(chance.below(static_cast<int>(data.size()) + 1));
                facts[69] = static_cast<std::uint8_t>(length);
                facts[70] = static_cast<std::uint8_t>(length >> 8);
                facts[71] = 0;
                break;
            default:
                break;
            }
            // The first piece: its two bytes, the facts (now and then too few of them), and some of the data.
            const bool tooFew = chance.oneIn(15);
            Bytes first = someBytes(chance, 2);
            first.insert(first.end(), facts.begin(), facts.end() - (tooFew ? chance.between(1, 86) : 0));
            Bytes rest = data;
            if (length > data.size() && length <= ami::longest) {
                rest.resize(length); // (as long as it said)
            }
            const auto split = static_cast<std::size_t>(chance.below(static_cast<int>(rest.size()) + 1));
            if (!tooFew) {
                first.insert(first.end(), rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(split));
            }
            ami::Frames pieces;
            auto read = [&] {
                const Watch watch;
                return pieces.piece(first);
            }();
            QVERIFY(asked <= 2 * (first.size() + length) + 1024);
            if (tooFew || shouldRefuse) {
                QVERIFY(!read.ok());
                ++refused;
                continue;
            }
            QVERIFY(read.ok());
            std::size_t got = std::min(split, length);
            while (!read.value()) {
                // What is kept is what came of the frame, and never more than the frame said it would be.
                QVERIFY(pieces.held() == got && got < length);
                const auto more = static_cast<std::size_t>(chance.between(1, 5000));
                Bytes next = someBytes(chance, 2);
                const std::size_t to = std::min(rest.size(), got + more);
                next.insert(next.end(), rest.begin() + static_cast<std::ptrdiff_t>(got),
                            rest.begin() + static_cast<std::ptrdiff_t>(to));
                const std::size_t held = pieces.held();
                read = [&] {
                    const Watch watch;
                    return pieces.piece(next);
                }();
                // (A vector that grows asks for half as much again as it had; a frame made whole is handed over.)
                QVERIFY(asked <= 2 * (held + next.size()) + 1024);
                QVERIFY(read.ok());
                got = to;
            }
            const ami::WholeFrame& whole = *read.value();
            QCOMPARE(whole.data.size(), length);
            QVERIFY(std::equal(whole.data.begin(),
                               whole.data.begin() + static_cast<std::ptrdiff_t>(std::min(length, rest.size())),
                               rest.begin()));
            QCOMPARE(pieces.held(), std::size_t{0});
            // ...and the reader takes what was put together, or says why not.
            aspeed::Screen screen;
            const auto picture = screen.read(whole.header, whole.data);
            QVERIFY(picture.ok() || picture.error().code == "garbled");
            ++frames;
        }
        qInfo("frames in pieces: %d whole, %d refused", frames, refused);
    }

    void whatIsWrittenIsReadBackWhole()
    {
        // Whatever the texts are (long, empty, not ASCII, with zeros in them), the pass message is its fields'
        // size, every field ends in a zero, and what is let in is said as it was named.
        Chance chance(3);
        for (int round = 0; round < 600; ++round) {
            auto text = [&chance] {
                std::string found(static_cast<std::size_t>(chance.oneIn(5) ? chance.below(600) : chance.below(40)),
                                  ' ');
                for (char& one : found) {
                    one = static_cast<char>(chance.below(256));
                }
                return found;
            };
            const bool completeFirst = chance.oneIn(2);
            const std::string pass = text();
            const std::string own = text();
            const std::string controller = text();
            const std::string name = text();
            ami::Messages arriving;
            const auto said = arriving.feed(ami::hello(pass, own, controller, name, completeFirst));
            QVERIFY(said.ok() && said.value().size() == (completeFirst ? 3u : 2u));
            const Bytes& body = said.value()[completeFirst ? 1 : 0].body;
            QCOMPARE(body.size(), std::size_t{438});
            for (const std::size_t end : {129, 194, 323, 372, 437}) {
                QCOMPARE(body[end], std::uint8_t{0});
            }
            const Bytes who = someBytes(chance, static_cast<std::size_t>(chance.below(300)));
            const auto letIn = ami::Messages().feed(ami::letIn(who));
            QVERIFY(letIn.ok() && letIn.value().size() == 2 && letIn.value()[0].body == who &&
                    letIn.value()[1].body == who);
        }
    }
};

QTEST_GUILESS_MAIN(TestAmiConsoleFuzz)
#include "test_ami_console_fuzz.moc"
