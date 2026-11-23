#include "rfb_console_by_hand.h"
#include "vnc_by_hand.h"

#include <QTcpServer>

#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QThreadPool>
#include <qkvm/core/rfb_screen.h>
#include <qkvm/core/server_live.h>
#include <qkvm/rfb/rfb.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <qkvm/base/screen_still.h>

#include "../src/jpeg_pixels.h"
#include "../src/rfb_reader.h"

// OpenBMC's console: RFB over bmcweb's WebSocket /kvm/0 (RfbScreen), against a stand-in on this PC's own address. The
// web's tests/test_openbmc_console.py and the DoorScreen's manners of test_vnc.py.
using namespace qkvm::core;
using namespace qkvm::core::testing;
namespace rfb = qkvm::rfb;
namespace rfbt = qkvm::rfb::tests;
namespace aspeed = qkvm::bmc::aspeed;
using namespace Qt::StringLiterals;

namespace {

RfbScreen::Limits quick()
{
    RfbScreen::Limits limits;
    limits.idleMs = 1500;
    limits.openingMs = 3000;
    limits.handshakeMs = 1500;
    limits.lookPaceMs = 200;
    limits.livePaceMs = 10;
    limits.liveForMs = 400;
    limits.heartbeatMs = 400;
    limits.messageWithinMs = 800;
    limits.pieceWithinMs = 600;
    limits.retryMs = {400, 800, 1200, 1600};
    limits.settledMs = 1000;
    limits.tickMs = 20;
    limits.goodbyeWithinMs = 500;
    limits.inputAfterMs = 150;
    limits.inputWaitMs = 2500;
    return limits;
}

// A screen over a console stand-in: its ways in, what it showed and how its sessions ended.
struct Rig {
    explicit Rig(const RfbScreen::Limits& limits = quick())
    {
        screen = std::make_unique<RfbScreen>(
            [this](const RfbScreen::Answer& answer, const RfbScreen::Wanted& /*wanted*/) {
                ++asked;
                RfbScreen::WayIn way;
                way.address = console.address();
                way.token = token.isEmpty() ? console.token : token;
                QString handed = password;
                way.password = SecretText::takenFrom(handed);
                way.vncAuthentication = vncAuthentication;
                way.passwordSent = [count = passwordsSent]() -> qkvm::base::Result<void> {
                    ++*count;
                    return {};
                };
                // (Called once its line has gone, which may be after this rig: it holds a count of its own.)
                way.done = [count = done] { ++*count; };
                answer(std::move(way));
            },
            [this](const aspeed::Picture& picture, std::span<const aspeed::Rect> changed) {
                ++shown;
                last = picture;
                lastChanged.assign(changed.begin(), changed.end());
            },
            limits);
        QObject::connect(screen.get(), &ServerConsole::ended, screen.get(),
                         [this](const QString& code, const QString& message) { ended.emplace_back(code, message); });
    }

    // Looks every 100 ms while it lives (whoever shows a screen asks at least that often).
    void keepLooking()
    {
        looks.start(100);
        QObject::connect(&looks, &QTimer::timeout, screen.get(), [this] { static_cast<void>(screen->look()); });
    }

    RfbConsoleByHand console;
    QByteArray token;
    // The password each way in hands in, and whether its console may ask for VNC authentication; how many times the
    // answer to a challenge went.
    QString password;
    bool vncAuthentication = false;
    std::shared_ptr<int> passwordsSent = std::make_shared<int>(0);
    int asked = 0;
    std::shared_ptr<int> done = std::make_shared<int>(0);
    int shown = 0;
    aspeed::Picture last;
    std::vector<aspeed::Rect> lastChanged;
    std::vector<std::pair<QString, QString>> ended;
    QTimer looks;
    std::unique_ptr<RfbScreen> screen;
};

// A VNC server's family's times: quick(), its own words, an idle pace while nobody watches, keys at once.
RfbScreen::Limits vncQuick()
{
    RfbScreen::Limits limits = quick();
    limits.thing = u"the VNC server"_s;
    limits.idlePaceMs = 1500;
    limits.watchedForMs = 300;
    limits.lookPaceMs = 100;
    limits.inputAfterMs = 0;
    return limits;
}

// A screen over a VNC server by hand: RFB straight over its TCP connection, with VNC authentication and a password.
struct VncRig {
    explicit VncRig(const RfbScreen::Limits& limits = vncQuick())
    {
        screen = std::make_unique<RfbScreen>(
            [this](const RfbScreen::Answer& answer, const RfbScreen::Wanted& /*wanted*/) {
                ++asked;
                RfbScreen::WayIn way;
                way.raw = true;
                way.address = QUrl(u"http://127.0.0.1:%1"_s.arg(port != 0 ? port : server.port()));
                way.vncAuthentication = true;
                QString handed = password;
                way.password = SecretText::takenFrom(handed);
                way.passwordSent = [this]() -> qkvm::base::Result<void> {
                    if (heldBack) {
                        return *heldBack;
                    }
                    ++*passwordsSent;
                    return {};
                };
                answer(std::move(way));
            },
            [this](const aspeed::Picture& picture, std::span<const aspeed::Rect> /*changed*/) {
                ++shown;
                last = picture;
            },
            limits);
        QObject::connect(screen.get(), &ServerConsole::ended, screen.get(),
                         [this](const QString& code, const QString& message) { ended.emplace_back(code, message); });
    }

    // Keeps the session every 100 ms while it lives, as the wall's look at its status does, nobody watching.
    void keepKeeping()
    {
        keeps.start(100);
        QObject::connect(&keeps, &QTimer::timeout, screen.get(), [this] { static_cast<void>(screen->keep()); });
    }

    VncByHand server;
    QString password = u"secret"_s;
    quint16 port = 0; // another port than the server's (one nobody listens at)
    // How many answers to a challenge went; and where the way says one may not go now, why (the screen's
    // passwordSent).
    std::shared_ptr<int> passwordsSent = std::make_shared<int>(0);
    std::optional<qkvm::base::Error> heldBack;
    int asked = 0;
    int shown = 0;
    aspeed::Picture last;
    std::vector<std::pair<QString, QString>> ended;
    QTimer keeps;
    std::unique_ptr<RfbScreen> screen;
};

QByteArray qtBytes(const rfbt::Bytes& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}

quint16 aPortNobodyListensAt()
{
    QTcpServer server;
    server.listen(QHostAddress::LocalHost);
    return server.serverPort();
}

bool showsPicture(const aspeed::Picture& shown, const rfbt::Picture& expected, int x = 0, int y = 0)
{
    if (shown.width < x + expected.width || shown.height < y + expected.height) {
        return false;
    }
    const rfbt::Picture whole{shown.width, shown.height, shown.rgb};
    return rfbt::part(whole, x, y, expected.width, expected.height).rgb == expected.rgb;
}

QByteArray jpegOf(const QImage& image, int quality = 100)
{
    QByteArray made;
    QBuffer buffer(&made);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", quality);
    return made;
}

// An update of `many` (at the most 16: four screens, all one update may draw) quarter-screen JPEGs of a 1920 by 1200
// screen, real ones decoded by FFmpeg: costly enough to draw that a small drawing share pauses between them.
constexpr int costlyWidth = 1920;
constexpr int costlyHeight = 1200;
rfbt::Bytes costlyJpegs(int many)
{
    constexpr int width = costlyWidth / 2;
    constexpr int height = costlyHeight / 2;
    QImage frame(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            frame.setPixel(x, y, qRgb((x + y) & 255, (x * 3) & 255, (y * 5) & 255));
        }
    }
    const QByteArray jpeg = jpegOf(frame, 80);
    const std::vector<std::uint8_t> body(jpeg.begin(), jpeg.end());
    rfbt::Bytes made{0, 0};
    rfbt::be16(made, static_cast<std::uint32_t>(many));
    for (int one = 0; one < many; ++one) {
        const rfbt::Bytes rect = rfbt::tightJpeg(one % 2 * width, one / 2 % 2 * height, width, height, body);
        made.insert(made.end(), rect.begin(), rect.end());
    }
    return made;
}

// What drawing `stream` (a handshake and its updates) costs in this build, on this core, through the reader the screen
// gives it to (an rfb::Client with JpegPixels): the best of two readings, in milliseconds for each of its `many`.
double readingCostMs(const rfbt::Bytes& stream, int many)
{
    auto pixels = std::make_shared<JpegPixels>();
    rfb::Options options;
    options.signsIn = {rfb::securityNone};
    options.longStepMs = -1; // (the share off: the cost itself)
    options.jpeg = [pixels](std::span<const std::uint8_t> data, int width, int height) {
        return pixels->decode(data, width, height);
    };
    qint64 best = std::numeric_limits<qint64>::max();
    for (int reading = 0; reading < 2; ++reading) {
        rfb::Client client(options);
        QElapsedTimer drawing;
        drawing.start();
        static_cast<void>(client.feed(stream, 0));
        best = std::min(best, drawing.elapsed());
    }
    return std::max(1.0, static_cast<double>(best) / many);
}

// What drawing one of costlyJpegs' JPEGs costs here.
double costPerJpegMs()
{
    return readingCostMs(rfbt::join({rfbt::greeting(), rfbt::securityTypes({1}), rfbt::word(0),
                                     rfbt::serverInit(costlyWidth, costlyHeight, "s"), costlyJpegs(8)}),
                         8);
}

// The share of quick() sized by that cost, with no burst to draw a first JPEG on: each JPEG is followed by a pause
// of some half a second in any build (debug, release, the sanitizer's), so the pauses of eight pass a deadline of
// 1.5 s wherever the tests run (round 5's fixed share drew past them too fast in release).
RfbScreen::Limits pausing(RfbScreen::Limits limits)
{
    const double cost = costPerJpegMs();
    limits.drawingSharePerMil = std::clamp(static_cast<int>(cost * 1000 / 500), 1, 1000);
    limits.drawingBurstMs = 1;
    limits.idleMs = 60000;
    qInfo("pausing: a JPEG costs %.1f ms here, so the share is %d per mil", cost, limits.drawingSharePerMil);
    return limits;
}

// Whether the console was asked for one pixel outright (the heartbeat).
bool askedOnePixel(const RfbConsoleByHand& console)
{
    return std::ranges::any_of(console.requests,
                               [](const RfbConsoleByHand::Request& one) { return one.width == 1 && one.height == 1; });
}

// Every thread of the global pool kept busy until what this returns is called (by a scope guard): nothing the pool is
// given meanwhile runs, the screen's reader among it. The semaphore lives as long as the last of its waiters.
std::function<void()> keepThePoolBusy()
{
    QThreadPool* const pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    auto busy = std::make_shared<QSemaphore>();
    for (int one = 0; one < threads; ++one) {
        pool->start([busy] { busy->acquire(); });
    }
    return [busy, threads] { busy->release(threads); };
}

// The turn an asking was answered with: -1 where it was none; and what an answer came to, by its code ("ok").
ServerConsole::Turn turnIn(const std::optional<qkvm::base::Result<ServerConsole::Turn>>& turn)
{
    return turn.has_value() && turn->ok() ? turn->value() : -1;
}

template <typename T>
std::string codeIn(const std::optional<qkvm::base::Result<T>>& answer)
{
    if (!answer.has_value()) {
        return "none";
    }
    return answer->ok() ? std::string("ok") : answer->error().code;
}

std::span<const std::uint8_t> spanOf(const QByteArray& bytes)
{
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<std::size_t>(bytes.size())};
}

} // namespace

class TestRfbScreen : public QObject {
    Q_OBJECT

private slots:
    void aLookOpensASessionWithTheSessionsTokenAndNoPassword()
    {
        Rig rig;
        const auto looked = rig.screen->look();
        QVERIFY(!looked.ok());
        QCOMPARE(looked.error().code, std::string("no_stream"));
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        QVERIFY(showsPicture(rig.last, rig.console.picture));
        QVERIFY(rig.screen->look().ok());
        QCOMPARE(rig.screen->picture().width, 64);
        // Signed in to with the driver's session, no subprotocol; RFB 3.8, no password, the console shared.
        QCOMPARE(rig.console.tokens, QList<QByteArray>{"t0ken"});
        QCOMPARE(rig.console.protocols, QList<QByteArray>{"(none)"});
        QCOMPARE(rig.console.handshake, rfbt::join({rfbt::text("RFB 003.008\n"), rfbt::Bytes{1}, rfbt::Bytes{1}}));
        QCOMPARE(rig.console.formats, 1);
        QVERIFY(std::ranges::find(rig.console.encodings, qkvm::rfb::tight) != rig.console.encodings.end());
        QVERIFY(std::ranges::find(rig.console.encodings, qkvm::rfb::jpegQuality) != rig.console.encodings.end());
        // The whole screen first.
        QVERIFY(!rig.console.requests.empty());
        QVERIFY(!rig.console.requests.front().incremental);
        QCOMPARE(rig.console.requests.front().width, 64);
        QCOMPARE(rig.console.requests.front().height, 48);
        QCOMPARE(rig.screen->serverName(), u"obmc-ikvm"_s);
        QCOMPARE(rig.asked, 1);
    }

    void updatesAreAskedForOneAtATimeAtTheLookingPaceAndAtAViewersWhileLive()
    {
        Rig rig;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        const std::size_t before = rig.console.requests.size();
        QTest::qWait(700);
        const std::size_t looking = rig.console.requests.size() - before;
        QVERIFY2(looking <= 5, qPrintable(QString::number(looking))); // (at most one in 200 ms)
        for (std::size_t at = before; at < rig.console.requests.size(); ++at) {
            QVERIFY(rig.console.requests[at].incremental);
        }
        QTimer live;
        QObject::connect(&live, &QTimer::timeout, rig.screen.get(), [&rig] { rig.screen->liveSeen(); });
        rig.screen->liveSeen();
        live.start(50);
        const std::size_t then = rig.console.requests.size();
        QTest::qWait(700);
        const std::size_t whileLive = rig.console.requests.size() - then;
        QVERIFY2(whileLive >= 2 * looking + 4, qPrintable(u"%1 %2"_s.arg(whileLive).arg(looking)));
    }

    void whatChangedIsShownAsTheRectanglesItChanged()
    {
        Rig rig;
        const rfbt::Picture first = rfbt::pattern(64, 48, 3);
        const rfbt::Picture dot = rfbt::pattern(5, 4, 9);
        rig.console.updates = {rfbt::update({rfbt::rawRect(0, 0, first)}), rfbt::update({rfbt::rawRect(10, 20, dot)})};
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 2, 5000);
        QString said;
        for (const aspeed::Rect& one : rig.lastChanged) {
            said += u"(%1 %2 %3 %4) "_s.arg(one.x).arg(one.y).arg(one.width).arg(one.height);
        }
        QVERIFY2(rig.lastChanged == std::vector<aspeed::Rect>{aspeed::Rect(10, 20, 5, 4)}, qPrintable(said));
        QVERIFY(showsPicture(rig.last, dot, 10, 20));
        QVERIFY(showsPicture(rig.last, rfbt::part(first, 0, 0, 64, 20)));
    }

    void everyEncodingComesThroughIntoThePicture()
    {
        Rig rig;
        rfbt::Deflater zrle;
        rfbt::Deflater tight;
        const rfbt::Picture pixels = rfbt::pattern(64, 48, 5);
        const QImage solid = [] {
            QImage made(32, 16, QImage::Format_RGB888);
            made.fill(QColor(200, 40, 90));
            return made;
        }();
        const QByteArray jpeg = jpegOf(solid);
        rig.console.updates = {
            rfbt::update(
                {rfbt::zrleRect(0, 0, rfbt::part(pixels, 0, 0, 64, 16), rfbt::ZrleTiles::PlainRuns, zrle),
                 rfbt::hextileRect(0, 16, rfbt::part(pixels, 0, 16, 64, 16), true),
                 rfbt::tightRect(0, 32, rfbt::part(pixels, 0, 32, 64, 16), rfbt::TightFilter::Gradient, tight)}),
            rfbt::update({rfbt::tightJpeg(16, 16, 32, 16, spanOf(jpeg))})};
        rig.console.piece = 37; // (in WebSocket messages of a few bytes each)
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        // (Drawn as its rectangles come: what is shown is waited for until the JPEG, the last, is in it.)
        const std::size_t at = (std::size_t{20} * 64 + 20) * 3;
        const auto jpegShown = [&rig] {
            return rig.last.rgb.size() > at + 2 && std::abs(rig.last.rgb[at] - 200) <= 6 &&
                   std::abs(rig.last.rgb[at + 1] - 40) <= 6 && std::abs(rig.last.rgb[at + 2] - 90) <= 6;
        };
        QTRY_VERIFY_WITH_TIMEOUT(jpegShown(), 5000);
        QVERIFY(rig.ended.empty());
        QVERIFY(showsPicture(rig.last, rfbt::part(pixels, 0, 0, 64, 16)));
        QVERIFY(showsPicture(rig.last, rfbt::part(pixels, 0, 32, 64, 16), 0, 32));
    }

    void aJpegIsMadeIntoPixelsAtItsRectanglesSizeAlone()
    {
        QImage image(40, 24, QImage::Format_RGB888);
        image.fill(QColor(10, 220, 130));
        const QByteArray jpeg = jpegOf(image);
        const std::optional<JpegPixels::Size> said = JpegPixels::sizeOf(spanOf(jpeg));
        QVERIFY(said.has_value());
        QCOMPARE(said.value_or(JpegPixels::Size{}).width, 40);
        QCOMPARE(said.value_or(JpegPixels::Size{}).height, 24);
        JpegPixels pixels;
        const auto made = pixels.decode(spanOf(jpeg), 40, 24);
        QVERIFY2(made.ok(), made.ok() ? "" : made.error().message.c_str());
        QCOMPARE(made.value().size(), std::size_t{2880});
        QVERIFY(std::abs(made.value()[0] - 10) <= 6 && std::abs(made.value()[1] - 220) <= 6);
        // Of another size than its rectangle's, cut short, or no JPEG at all: refused.
        QCOMPARE(pixels.decode(spanOf(jpeg), 41, 24).error().code, std::string("garbled"));
        QCOMPARE(pixels.decode(spanOf(jpeg.left(jpeg.size() / 3)), 40, 24).error().code, std::string("garbled"));
        QCOMPARE(pixels.decode(spanOf(QByteArray("not a picture at all")), 4, 4).error().code, std::string("garbled"));
        QVERIFY(!JpegPixels::sizeOf(spanOf(QByteArray("\xFF\xD8\xFF\xDA\x00\x02", 6))).has_value());
        // A grey one.
        QImage grey(16, 8, QImage::Format_Grayscale8);
        grey.fill(QColor(128, 128, 128));
        const auto greyMade = pixels.decode(spanOf(jpegOf(grey)), 16, 8);
        QVERIFY(greyMade.ok());
        QVERIFY(std::abs(greyMade.value()[0] - 128) <= 4);
        theJpegGateIsStrict();
    }

    // A JPEG is held to the shape a real Tight JPEG has, walked whole, before FFmpeg sees a byte: one baseline or
    // extended-sequential frame, its scans and their entropy data, EOI and nothing after (confirm review finding 1).
    void theJpegGateIsStrict()
    {
        // Byte helpers: a marker segment with a 16-bit length, SOI, EOI.
        const auto seg = [](std::uint8_t marker, const QByteArray& payload) {
            QByteArray made("\xFF", 1);
            made.append(static_cast<char>(marker));
            const int length = static_cast<int>(payload.size()) + 2;
            made.append(static_cast<char>((length >> 8) & 0xFF));
            made.append(static_cast<char>(length & 0xFF));
            made.append(payload);
            return made;
        };
        const QByteArray soi("\xFF\xD8", 2);
        const QByteArray eoi("\xFF\xD9", 2);
        const QByteArray entropy("\x12\x34", 2); // a scan's coded data
        const auto sof0 = [&seg](int nf) {
            QByteArray payload("\x08\x00\x10\x00\x10", 5); // precision 8, 16 by 16
            payload.append(static_cast<char>(nf));
            for (int one = 0; one < nf; ++one) {
                payload.append(QByteArray("\x01\x11\x00", 3)); // one component spec
            }
            return seg(0xC0, payload);
        };
        const QByteArray sos = seg(0xDA, QByteArray("\x01\x01\x00\x00\x3F\x00", 6));
        JpegPixels pixels;
        // A well formed baseline shape is read (its size seen); it is only FFmpeg that it never reaches here.
        const QByteArray good = soi + sof0(3) + sos + entropy + eoi;
        QVERIFY(JpegPixels::sizeOf(spanOf(good)).has_value());
        QCOMPARE(JpegPixels::sizeOf(spanOf(good)).value_or(JpegPixels::Size{}).width, 16);
        // The confirm review's input: SOI, SOF0, DHT, SOS with one byte, then SOF2 and a scan. Refused at the second
        // frame, before FFmpeg, where the old walk stopped at the first scan and never saw it.
        const QByteArray dht = seg(0xC4, QByteArray(20, '\x00'));
        const QByteArray sof2 =
            seg(0xC2, QByteArray("\x08\x00\x10\x00\x10\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00", 15));
        const QByteArray review = soi + sof0(3) + dht + seg(0xDA, QByteArray("\x01\x01\x00\x00\x3F\x00", 6)) +
                                  QByteArray("\x00", 1) + sof2 + sos + entropy + eoi;
        QVERIFY(!JpegPixels::sizeOf(spanOf(review)).has_value());
        // Its variants, each refused, each ok on the old walk:
        QVERIFY(!JpegPixels::sizeOf(spanOf(soi + sof0(3) + sos + entropy + sof0(3) + sos + entropy + eoi))
                     .has_value()); // a second SOF0
        QVERIFY(!JpegPixels::sizeOf(spanOf(soi + sof0(1) + sos + entropy + dht + sos + entropy + eoi))
                     .has_value()); // a DHT then a second scan, one component
        QByteArray nine = soi + sof0(3);
        for (int one = 0; one < 9; ++one) {
            nine += sos + entropy;
        }
        nine += eoi;
        QVERIFY(!JpegPixels::sizeOf(spanOf(nine)).has_value());                              // nine scans
        QVERIFY(!JpegPixels::sizeOf(spanOf(good + QByteArray("more data", 9))).has_value()); // data after EOI
        // round-2 review 3: a JPGn marker (FF F0) is refused outright, not skipped by a claimed length, so a frame
        // cannot hide inside a segment FFmpeg reads markers within. The review's input: a scan, then 55 such segments,
        // each carrying an SOF2 and scans.
        QByteArray jpgn = soi + sof0(3) + sos + entropy;
        for (int one = 0; one < 55; ++one) {
            QByteArray hiddenBody = sof2;
            hiddenBody.append(QByteArray(200, '\x11'));
            jpgn += seg(0xF0, hiddenBody);
        }
        jpgn += eoi;
        QVERIFY(!JpegPixels::sizeOf(spanOf(jpgn)).has_value());
        // A single JPGn, before any frame, is refused too (FFmpeg reads no length for it).
        QVERIFY(!JpegPixels::sizeOf(spanOf(soi + seg(0xF0, QByteArray(4, '\x00')) + sof0(3) + sos + entropy + eoi))
                     .has_value());
        // An SOF of a length that is not 8 + 3*Nf, or an SOS not 6 + 2*Ns, is refused (a claimed length cannot differ
        // from the shape).
        QVERIFY(!JpegPixels::sizeOf(spanOf(soi + seg(0xC0, QByteArray("\x08\x00\x10\x00\x10\x03\x01\x11\x00", 9)) +
                                           sos + entropy + eoi))
                     .has_value()); // SOF0 says 3 components but is one short
        // round-2 review 6: a 0xFF fill byte before a restart marker inside the entropy data (FF FF D0), and before
        // the EOI, is accepted, not refused (T.81 B.1.1.2).
        const QByteArray restartFill = soi + sof0(3) + sos + QByteArray("\x12\xFF\xFF\xD0\x34\xFF\xFF", 7) + eoi;
        QVERIFY(JpegPixels::sizeOf(spanOf(restartFill)).has_value());
        // round-3 finding 1 (HIGH): a segment that ends at the last byte must not be read one past it (the hardened
        // build would end the whole program). An SOF0 of length 7, and an SOS of length 2, each at the end: refused,
        // no crash.
        QVERIFY(
            !JpegPixels::sizeOf(spanOf(QByteArray("\xFF\xD8\xFF\xC0\x00\x07\x08\x00\x10\x00\x10", 11))).has_value());
        QVERIFY(!JpegPixels::sizeOf(spanOf(soi + sof0(3) + QByteArray("\xFF\xDA\x00\x02", 4))).has_value());
        QVERIFY(pixels.decode(spanOf(QByteArray("\xFF\xD8\xFF\xC0\x00\x07\x08\x00\x10\x00\x10", 11)), 16, 16)
                    .error()
                    .code == std::string("garbled"));
        // round-3 finding 2: a DQT is read whole, not skipped by a claimed length, so a tail cannot hide a marker. A
        // DQT whose length leaves six bytes past one 65-byte table is refused.
        const QByteArray dqtTail = soi + sof0(3) +
                                   seg(0xDB, QByteArray(65, '\x00') + QByteArray("\xFF\xFE\x00\x40\x11\x22", 6)) + sos +
                                   entropy + eoi;
        QVERIFY(!JpegPixels::sizeOf(spanOf(dqtTail)).has_value());
        // ...while a DQT of exactly one 65-byte table (Pq 0) is read.
        const QByteArray dqtGood = soi + seg(0xDB, QByteArray(65, '\x00')) + sof0(3) + sos + entropy + eoi;
        QVERIFY(JpegPixels::sizeOf(spanOf(dqtGood)).has_value());
        // A DHT whose counts do not fill its length is refused; one that does is read.
        const QByteArray dhtBad = soi + sof0(3) +
                                  seg(0xC4, QByteArray("\x00", 1) + QByteArray(16, '\x00') + QByteArray(10, '\x11')) +
                                  sos + entropy + eoi; // counts all zero, yet ten symbol bytes: a tail
        QVERIFY(!JpegPixels::sizeOf(spanOf(dhtBad)).has_value());
        const QByteArray dhtGood =
            soi + seg(0xC4, QByteArray("\x00", 1) + QByteArray(16, '\x00')) + sof0(3) + sos + entropy + eoi;
        QVERIFY(JpegPixels::sizeOf(spanOf(dhtGood)).has_value());
        // What the gate refuses, decode refuses too (never handed to FFmpeg).
        QCOMPARE(pixels.decode(spanOf(review), 16, 16).error().code, std::string("garbled"));
        QCOMPARE(pixels.decode(spanOf(jpgn), 16, 16).error().code, std::string("garbled"));
    }

    void whatTheServerSendsUnaskedIsShownToo()
    {
        Rig rig;
        rig.console.answersChanges = false;
        rig.console.pushEveryMs = 50;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 5, 5000);
    }

    void nobodyLookingTheSessionIsGivenBack()
    {
        Rig rig;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, QString());
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.watching, 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(*rig.done, 1, 3000);
        QVERIFY(rig.screen->picture().rgb.empty());
        // Looked at again, a session is opened again at once.
        static_cast<void>(rig.screen->look());
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.sessions, 2, 3000);
        QCOMPARE(rig.console.mostWatching, 1);
    }

    void aStillScreenIsAskedForOnePixelAndOneThatAnswersNothingIsLeft()
    {
        Rig rig;
        rig.console.answersChanges = false; // (a screen that does not change)
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        const auto onePixel = [&rig] {
            return std::ranges::any_of(rig.console.requests, [](const RfbConsoleByHand::Request& one) {
                return !one.incremental && one.width == 1 && one.height == 1;
            });
        };
        QTRY_VERIFY_WITH_TIMEOUT(onePixel(), 3000);
        QTest::qWait(1000);
        QVERIFY(rig.ended.empty()); // (answered: the session goes on)

        Rig deaf;
        deaf.console.answersChanges = false;
        deaf.keepLooking();
        static_cast<void>(deaf.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(deaf.shown >= 1, 5000);
        deaf.console.answersWhole = false;
        QTRY_VERIFY_WITH_TIMEOUT(!deaf.ended.empty(), 5000);
        QCOMPARE(deaf.ended.front().first, u"timeout"_s);
        QCOMPARE(deaf.ended.front().second, u"The server's console does not answer requests"_s);
    }

    void whatIsNoRfbIsSaidAndLeftAloneOnTheLadder()
    {
        Rig rig;
        rig.console.garbles = true;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"bad_response"_s);
        QVERIFY2(rig.ended.front().second.startsWith(u"The server's console sent what qKVM does not take: "_s),
                 qPrintable(rig.ended.front().second));
        const auto looked = rig.screen->look();
        QVERIFY(!looked.ok());
        QCOMPARE(looked.error().code, std::string("bad_response"));
        QCOMPARE(rig.console.sessions, 1);
        // ...until the ladder's first rung: then once more.
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.sessions, 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(static_cast<int>(rig.ended.size()), 2, 3000);
        QTest::qWait(500);
        QCOMPARE(rig.console.sessions, 2); // (the second rung is longer)
    }

    void aConsoleThatAsksForAPasswordIsAnsweredNothing()
    {
        Rig rig;
        rig.console.security = {2};
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"bad_response"_s);
        QVERIFY2(rig.ended.front().second.startsWith(u"The server's console does not let qKVM in: "_s),
                 qPrintable(rig.ended.front().second));
        QCOMPARE(rig.console.handshake, rfbt::text("RFB 003.008\n")); // (its version, and not a word more)
    }

    void aConsoleThatAsksForAPasswordIsAnsweredWithTheOneItsWayHandsIn()
    {
        // A door's console (a Proxmox guest's, with the ticket its way in was handed): VNC authentication answered
        // with that password, once, and told the moment before the answer goes (the web's before_password).
        Rig rig;
        rig.console.security = {2};
        rig.console.password = "t1cket-9Qz";
        rig.password = u"t1cket-9Qz"_s;
        rig.vncAuthentication = true;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        QVERIFY(showsPicture(rig.last, rig.console.picture));
        QCOMPARE(rig.console.passwordTries, 1);
        QCOMPARE(*rig.passwordsSent, 1);
        QVERIFY(rig.ended.empty());
        // Its version, VNC authentication chosen, the answer, and the console shared.
        QCOMPARE(rig.console.handshake.size(), std::size_t{12 + 1 + 1});
        QCOMPARE(rig.console.handshake.at(12), std::uint8_t{2});
    }

    void aRejectedPasswordIsSaidAsARejectedLogin()
    {
        Rig rig;
        rig.console.security = {2};
        rig.console.password = "t1cket-9Qz";
        rig.password = u"another-one"_s;
        rig.vncAuthentication = true;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"auth_failed"_s);
        QCOMPARE(rig.ended.front().second, u"Login rejected - check the username and password"_s);
        QCOMPARE(rig.console.passwordTries, 1);
        QCOMPARE(*rig.passwordsSent, 1);
        QVERIFY(!rig.ended.front().second.contains(u"another-one"_s));
    }

    void aConsoleThatAsksForAPasswordNobodyHandedInIsSentNone()
    {
        Rig rig;
        rig.console.security = {2};
        rig.console.password = "t1cket-9Qz";
        rig.vncAuthentication = true;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"no_password"_s);
        QCOMPARE(rig.ended.front().second, u"No password saved for this device"_s);
        QTest::qWait(200);
        QCOMPARE(rig.console.passwordTries, 0);
        QCOMPARE(*rig.passwordsSent, 0);
    }

    void aWayThatTakesNoVncAuthenticationSendsNoneOfItsPassword()
    {
        // (OpenBMC's way: no VNC authentication, whatever password it was handed by mistake.)
        Rig rig;
        rig.console.security = {2};
        rig.console.password = "t1cket-9Qz";
        rig.password = u"t1cket-9Qz"_s;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"bad_response"_s);
        QCOMPARE(rig.console.handshake, rfbt::text("RFB 003.008\n"));
        QCOMPARE(rig.console.passwordTries, 0);
        QCOMPARE(*rig.passwordsSent, 0);
    }

    // --- a VNC server's screen: RFB straight over its own connection -------------------------------------------------

    void aVncServerIsReadStraightOverItsOwnConnection()
    {
        VncRig rig;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        QVERIFY(showsPicture(rig.last, rig.server.picture));
        QVERIFY(rig.screen->letIn());
        QCOMPARE(rig.screen->width(), 160);
        QCOMPARE(rig.screen->height(), 100);
        QVERIFY(rig.server.versions == std::vector<std::string>{"RFB 003.008\n"});
        QCOMPARE(rig.server.passwordTries, 1);
        QVERIFY(rig.server.shared == std::vector<int>{1}); // (nobody else at the server is thrown out)
        QCOMPARE(rig.screen->serverName(), u"stand-in VNC"_s);
        QVERIFY(rig.ended.empty());
    }

    void aVncServerSaysWhatEndedItInItsOwnWords()
    {
        // (Windows takes a couple of seconds to give up on a port nobody listens at, more on a busy PC: the line is
        // given longer than that to be let in, so that what ends it is the refusal, and not its time, which is
        // "timeout".)
        RfbScreen::Limits patient = vncQuick();
        patient.openingMs = 10000;
        VncRig nobody(patient);
        nobody.port = aPortNobodyListensAt();
        nobody.keepKeeping();
        static_cast<void>(nobody.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!nobody.ended.empty(), 12000);
        QCOMPARE(nobody.ended.front().first, u"unreachable"_s);
        QCOMPARE(nobody.ended.front().second, u"Device is unreachable"_s);
        QCOMPARE(nobody.screen->standing().value_or(qkvm::base::Error{}).code, std::string("unreachable"));
        VncRig garbled;
        garbled.server.garbles = true;
        static_cast<void>(garbled.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!garbled.ended.empty(), 5000);
        QCOMPARE(garbled.ended.front().first, u"bad_response"_s);
        QVERIFY2(garbled.ended.front().second.startsWith(u"The VNC server sent what qKVM does not take: "_s),
                 qPrintable(garbled.ended.front().second));
        VncRig refused;
        refused.server.offers = std::vector<std::uint8_t>{19, 16};
        static_cast<void>(refused.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!refused.ended.empty(), 5000);
        QCOMPARE(refused.ended.front().first, u"bad_response"_s);
        QVERIFY2(refused.ended.front().second.startsWith(u"The VNC server does not let qKVM in: "_s),
                 qPrintable(refused.ended.front().second));
        QCOMPARE(refused.server.bytesHeard, qint64{12}); // (its version, and not a word more)
    }

    void aStreamOfTinyDynamicBlocksEndsTheSessionInItsTimeAndFreesItsWorker()
    {
        // Outside review 2.1: a VNC server sends one ZRLE rectangle of the whole screen whose stream is 150,000 empty
        // dynamic blocks (each about twelve bytes, and each has the reader build its tables anew: read whole, it held a
        // worker for seconds, tens of them in the debug build). The second's work is counted before each table is
        // built, so the stream is cut off within a few hundred blocks: the session ends at once, in plain words, the
        // server at fault, and the reader's worker is free again.
        QVERIFY(QThreadPool::globalInstance()->waitForDone(30'000)); // (nothing of an earlier case's still at work)
        VncRig rig;
        rig.server.width = 1920;
        rig.server.height = 1080;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        rfbt::Deflater stream;
        const rfbt::Bytes hostile = rfbt::update({rfbt::zrleOf(0, 0, 1920, 1080, stream.emptyDynamicStream(150'000))});
        QElapsedTimer since;
        since.start();
        rig.server.sendNow(hostile);
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 120'000);
        const qint64 tookMs = since.elapsed();
        QCOMPARE(rig.ended.front().first, u"bad_response"_s);
        QVERIFY2(rig.ended.front().second.startsWith(u"The VNC server sent what qKVM does not take: "_s),
                 qPrintable(rig.ended.front().second));
        QVERIFY2(tookMs < 1500, qPrintable(u"The session ended %1 ms after the stream was sent"_s.arg(tookMs)));
        QVERIFY2(QThreadPool::globalInstance()->waitForDone(500),
                 qPrintable(QString::number(QThreadPool::globalInstance()->activeThreadCount())));
    }

    void aReaderWhoseScreenGoesStopsTheWorkUnderWay()
    {
        // Outside review 2.1: a reader detached (its session over, its screen closed) stops a decode under way at the
        // next asking (before the next dynamic block's tables), and its worker is free at once: it does not read the
        // rest of a stream for nobody. A slow machine is made of the reader's processor clock (each reading of it takes
        // two milliseconds), so that even what the second's work allows of the stream lasts long.
        QVERIFY(QThreadPool::globalInstance()->waitForDone(30'000));
        QObject home;
        rfb::Options options;
        options.workClock = [] {
            QThread::msleep(2);
            return RfbReader::steadyMs();
        };
        const auto reader = std::make_shared<RfbReader>(&home, [] {}, std::move(options));
        rfbt::Deflater stream;
        reader->feed(qtBytes(rfbt::join({rfbt::greeting(), rfbt::securityTypes({rfb::securityNone}), rfbt::word(0),
                                         rfbt::serverInit(1920, 1080, "slow")})));
        reader->feed(qtBytes(rfbt::update({rfbt::zrleOf(0, 0, 1920, 1080, stream.emptyDynamicStream(100'000))})));
        QTest::qWait(150); // (the decode under way)
        QVERIFY(QThreadPool::globalInstance()->activeThreadCount() > 0);
        reader->detach();
        QVERIFY2(QThreadPool::globalInstance()->waitForDone(300),
                 qPrintable(QString::number(QThreadPool::globalInstance()->activeThreadCount())));
    }

    void whileNobodyWatchesAVncServerIsAskedForWhatChangedAtTheIdlePace()
    {
        VncRig rig;
        rig.keepKeeping();
        static_cast<void>(rig.screen->keep());
        QTRY_VERIFY_WITH_TIMEOUT(rig.screen->letIn(), 5000);
        QVERIFY(rig.screen->keep().ok());
        QTest::qWait(1000);
        const std::size_t before = rig.server.requests.size();
        QTest::qWait(1200);
        QVERIFY2(rig.server.requests.size() - before <= 1, qPrintable(QString::number(rig.server.requests.size())));
        // Somebody looks, as the wall's view of a watched screen does (every 100 ms): the looking pace, at once, and
        // for as long as it is looked at. (Looked at once alone, the looking pace lasts watchedForMs, 300 ms here,
        // which holds three requests only where each answer comes back within some 50 ms: the line's socket on a thread
        // of its own, outside review 2.2, added a few ms to each, and the case failed there in nearly every whole run.)
        static_cast<void>(rig.screen->look());
        const std::size_t looked = rig.server.requests.size();
        QTimer looking;
        QObject::connect(&looking, &QTimer::timeout, rig.screen.get(),
                         [&rig] { static_cast<void>(rig.screen->look()); });
        looking.start(100);
        // (At once: well before the idle pace, 1.5 s, would ask.)
        QVERIFY2(QTest::qWaitFor([&rig, looked] { return rig.server.requests.size() > looked; }, 500),
                 qPrintable(QString::number(rig.server.requests.size())));
        QTest::qWait(1000);
        looking.stop();
        QVERIFY2(rig.server.requests.size() - looked >= 3, qPrintable(QString::number(rig.server.requests.size())));
        // Kept, it is not let go of for want of anybody looking (Limits::idleMs is 1.5 s here).
        QTest::qWait(2500);
        QCOMPARE(rig.server.connections, 1);
        QVERIFY(rig.ended.empty());
    }

    void aFamilysOwnLadderSaysWhenTheNextSessionIsAskedFor()
    {
        VncRig rig;
        rig.server.garbles = true;
        std::vector<std::pair<QString, bool>> holds;
        rig.screen->setHold([&holds](const QString& code, bool served, qint64 /*lastedMs*/) {
            holds.emplace_back(code, served);
            return qint64{1500};
        });
        rig.keepKeeping();
        static_cast<void>(rig.screen->keep());
        QTRY_COMPARE_WITH_TIMEOUT(holds.size(), std::size_t{1}, 5000);
        QCOMPARE(holds.front().first, u"bad_response"_s);
        QVERIFY(!holds.front().second);
        // (The screen's own first rung is 400 ms here: not that.)
        QTest::qWait(900);
        QCOMPARE(rig.server.connections, 1);
        QTRY_COMPARE_WITH_TIMEOUT(rig.server.connections, 2, 3000);
    }

    void aSessionTheControllerDoesNotLetInIsSaid()
    {
        Rig rig;
        rig.token = "s0mebody-else";
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"bad_response"_s);
        QCOMPARE(rig.console.sessions, 0);
        QTRY_COMPARE_WITH_TIMEOUT(*rig.done, 1, 3000);
    }

    void aTokenThatCouldSplitTheAskingIsNotSent()
    {
        // (What a controller hands out goes into a header: one with a blank, a quote, a comma or a semicolon in it is
        // not sent at all.)
        for (const QByteArray& token : {QByteArray("two words"), QByteArray("a;b"), QByteArray("a\"b")}) {
            Rig rig;
            rig.token = token;
            static_cast<void>(rig.screen->look());
            QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
            QCOMPARE(rig.ended.front().first, u"no_picture"_s);
            QCOMPARE(rig.console.askings, 0);
        }
    }

    void aHandshakeThatDoesNotFinishEndsTheSession()
    {
        Rig rig;
        rig.console.silent = true;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"timeout"_s);
        QCOMPARE(rig.ended.front().second, u"The server's console did not finish the handshake in time"_s);
    }

    void aMessageBegunAndNotEndedEndsTheSession()
    {
        RfbScreen::Limits limits = quick();
        limits.heartbeatMs = 4000; // (no pixel is asked for meanwhile: its answer would come after the half)
        Rig rig(limits);
        // An update that says one rectangle of Raw and sends half of it.
        rfbt::Bytes half = rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(64, 48))});
        half.resize(half.size() / 2);
        rig.console.updates = {rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(64, 48))}), half};
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"timeout"_s);
    }

    void aConsoleThatRefusesTheAccountIsSaid()
    {
        // bmcweb answers the upgrade 403 where this account may not open the console: said as a refusal, and left on
        // the ladder (finding 6; m7r's DeviceLine::End::Refused).
        Rig rig;
        rig.console.refuses = true;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"bad_response"_s);
        QCOMPARE(rig.ended.front().second, u"The controller does not let this account open its console (HTTP 403)"_s);
        QCOMPARE(rig.console.sessions, 0); // (no WebSocket was opened)
    }

    void whileTheLineIsHeldBackThePieceTimerDoesNotRun()
    {
        // A reader slowed by a large update needs time to drain to 4 MiB; while this program holds the line back of its
        // own accord, the 8 s piece timer (the server's silence) must not end the session as the server's fault
        // (finding 5). A whole 1080p screen in Raw, in small pieces, holds the line back well past a short piece time.
        RfbScreen::Limits limits = quick();
        // The piece clock larger than any resume gap, smaller than the one long held span below.
        limits.pieceWithinMs = 200;
        limits.messageWithinMs = 40000;
        limits.heartbeatMs = 40000;
        limits.idleMs = 40000;
        // Held while over 20 MiB waits, let go at 10 MiB: with the whole screen queued, one long span of draining 10
        // MiB, far longer than the piece time, during which the line is held and (at its end) the clock is stale.
        limits.heldBackFromBytes = qint64{20} * 1024 * 1024;
        Rig rig(limits);
        // A screen of millions of pixels: its whole Raw update (about 37 MiB on the wire) holds the line back in one
        // span and keeps the reader mid-message for well over the 200 ms piece time.
        rig.console.width = 4096;
        rig.console.height = 2304;
        rig.console.picture = rfbt::pattern(4096, 2304, 2);
        rig.console.updates = {rfbt::update({rfbt::rawRect(0, 0, rig.console.picture)})};
        rig.console.piece = 64 * 1024;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        // The screen is shown, and in the held-back minutes the piece timer never ended the session as the server's
        // fault (it fires without the fix: the removal's test).
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 40000);
        QCOMPARE(rig.screen->picture().width, 4096);
        QVERIFY2(rig.ended.empty(), rig.ended.empty() ? "" : qPrintable(rig.ended.front().second));
    }

    void anUpdateSlicedOverPausesPastItsDeadlineIsNotEnded()
    {
        // round-4 review LOW 3: the time this program holds the line back (its reader over its drawing share) is taken
        // out of the server's deadlines, as the web's _now and _away (rfb.py:1108-1122). One honest update of eight
        // JPEGs, read at three hundredths of a thread, is sliced over pauses whose wall time passes its 1.5 s message
        // deadline. The server sends it a piece every 20 ms, so the reader is part way through the message at every
        // pause (sent at once, the line may hand it over in one, drawn before any pause), and each piece comes in its
        // time whenever the line is let go: it is not ended. Nor is a pixel asked for: the time since the update was
        // asked for is the server's only where its line was free.
        RfbScreen::Limits limits = pausing(quick());
        limits.messageWithinMs = 1500;
        limits.pieceWithinMs = 1000;
        limits.heartbeatMs = 1500;
        Rig rig(limits);
        rig.console.width = costlyWidth;
        rig.console.height = costlyHeight;
        rig.console.updates = {costlyJpegs(8)};
        rig.console.piece = 16 * 1024;
        rig.console.trickleEveryMs = 20;
        rig.keepLooking();
        QElapsedTimer wall;
        wall.start();
        static_cast<void>(rig.screen->look());
        // Whole when the screen asks again.
        QTRY_VERIFY_WITH_TIMEOUT(rig.console.requests.size() >= 2 || !rig.ended.empty(), 60000);
        const qint64 took = wall.elapsed();
        QVERIFY2(rig.ended.empty(), rig.ended.empty() ? "" : qPrintable(rig.ended.front().second));
        QVERIFY2(took > limits.messageWithinMs,
                 QByteArray::number(took)); // (the case: its wall time past the deadline)
        QVERIFY(!askedOnePixel(rig.console));
        QCOMPARE(rig.screen->picture().width, costlyWidth);
        qInfo("an update sliced over pauses: whole after %lld ms, its deadline %d ms", static_cast<long long>(took),
              limits.messageWithinMs);
    }

    void aHeartbeatAnsweredOverPausesIsNotEnded()
    {
        // ...and the answer to the heartbeat's pixel has its time the same way: a still screen is asked for one pixel,
        // and the server answers it with eight JPEGs, drawn over pauses past the 1.5 s it has to answer.
        RfbScreen::Limits limits = pausing(quick());
        limits.messageWithinMs = 1500;
        limits.pieceWithinMs = 1000;
        limits.heartbeatMs = 300;
        Rig rig(limits);
        rig.console.width = costlyWidth;
        rig.console.height = costlyHeight;
        rig.console.answersChanges = false; // a still screen: what changed is never answered
        rig.console.updates = {rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(16, 16))}), costlyJpegs(8)};
        rig.console.piece = 16 * 1024;
        rig.console.trickleEveryMs = 20; // the answer a piece every 20 ms: part way through it at every pause
        rig.console.trickleFrom = 1;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(askedOnePixel(rig.console) || !rig.ended.empty(), 20000);
        QElapsedTimer wall;
        wall.start();
        const std::size_t asked = rig.console.requests.size();
        // Answered whole when the screen asks again after it.
        QTRY_VERIFY_WITH_TIMEOUT(rig.console.requests.size() > asked || !rig.ended.empty(), 60000);
        const qint64 took = wall.elapsed();
        QVERIFY2(rig.ended.empty(), rig.ended.empty() ? "" : qPrintable(rig.ended.front().second));
        QVERIFY2(took > limits.messageWithinMs, QByteArray::number(took));
        // ...and only that time: from then on the screen answers nothing, not even a pixel, and is ended on its own
        // time since it was asked again, its heartbeat and the pixel's answer not lengthened by the pauses before.
        rig.console.answersWhole = false;
        qint64 endedAt = -1;
        QObject::connect(rig.screen.get(), &ServerConsole::ended, rig.screen.get(), [&endedAt, &rig] {
            if (endedAt < 0) {
                endedAt = rig.console.sinceMs();
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 60000);
        QCOMPARE(rig.ended.front().second, u"The server's console does not answer requests"_s);
        const qint64 quiet = endedAt - rig.console.requests[asked].atMs; // since the update asked for after the answer
        QVERIFY2(quiet <= limits.heartbeatMs + limits.messageWithinMs + 1000, QByteArray::number(quiet));
    }

    void aServerThatSendsAMessageTooSlowlyIsEndedInItsTime()
    {
        // ...and the message under way the same: after pauses, a server that sends a message a piece every 300 ms
        // (each piece in its time) is ended when the whole message has taken its 2 s of the server's own time, never
        // later for the time away before it.
        RfbScreen::Limits limits = pausing(quick());
        limits.messageWithinMs = 2000;
        limits.pieceWithinMs = 800;
        limits.heartbeatMs = 60000;
        Rig rig(limits);
        rig.console.width = costlyWidth;
        rig.console.height = costlyHeight;
        rig.console.updates = {costlyJpegs(6), rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(256, 256))})};
        rig.console.piece = 16 * 1024;
        rig.console.trickleEveryMs = 300; // the second update's 256 KiB in sixteen pieces: some 5 s
        rig.console.trickleFrom = 1;
        qint64 endedAt = -1;
        QObject::connect(rig.screen.get(), &ServerConsole::ended, rig.screen.get(), [&endedAt, &rig] {
            if (endedAt < 0) {
                endedAt = rig.console.sinceMs();
            }
        });
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 60000);
        QCOMPARE(rig.ended.front().second, u"The server's console did not send a whole message in time"_s);
        QVERIFY(rig.console.requests.size() >= 2);
        const qint64 took = endedAt - rig.console.requests[1].atMs; // the slow one went in answer to the second request
        QVERIFY2(took >= limits.messageWithinMs - 50 && took <= limits.messageWithinMs + 1000,
                 QByteArray::number(took));
    }

    void aServerThatStopsSendingIsStillEndedInItsTime()
    {
        // ...and only that time: a server that stops part way through a message while nothing holds its line back is
        // ended within its piece time of its silence, however long this program was away before. The time away is
        // taken out of the deadlines that ran through it, never credited to a later one.
        RfbScreen::Limits limits = pausing(quick());
        limits.messageWithinMs = 3000;
        limits.pieceWithinMs = 800;
        limits.heartbeatMs = 60000;
        Rig rig(limits);
        rig.console.width = costlyWidth;
        rig.console.height = costlyHeight;
        rfbt::Bytes half = rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(16, 16))});
        half.resize(half.size() / 2);
        rig.console.updates = {costlyJpegs(6), half};
        rig.console.piece = 16 * 1024;
        qint64 endedAt = -1;
        QObject::connect(rig.screen.get(), &ServerConsole::ended, rig.screen.get(), [&endedAt, &rig] {
            if (endedAt < 0) {
                endedAt = rig.console.sinceMs();
            }
        });
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 60000);
        QCOMPARE(rig.ended.front().first, u"timeout"_s);
        QVERIFY2(rig.ended.front().second.contains(u"did not finish"_s), qPrintable(rig.ended.front().second));
        QVERIFY(rig.console.requests.size() >= 2);
        const qint64 took = endedAt - rig.console.requests[1].atMs; // the half went in answer to the second request
        QVERIFY2(took >= limits.pieceWithinMs - 50 && took <= limits.pieceWithinMs + 1000, QByteArray::number(took));
    }

    void aReaderThatTellsNothingWhileTheLineIsHeldEndsTheSession()
    {
        // round-5 review LOW 2: a reader that stops telling (out of memory twice, or a pool that never runs it) while
        // its queue holds the line back would hold it, and still the server's deadlines, for good. Once it has told
        // nothing for the message deadline with the line held (not paused), the session is ended. Here every thread of
        // the pool is kept busy after the first screen, so the reader never runs while a large update fills its queue.
        RfbScreen::Limits limits = quick();
        limits.heldBackFromBytes = qint64{128} * 1024;
        limits.readerQuietMs = 800;
        Rig rig(limits);
        rig.console.width = 256;
        rig.console.height = 256;
        rig.console.updates = {rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(16, 16))}),
                               rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(256, 256))})};
        rig.console.piece = 16 * 1024;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        QTest::qWait(30); // (the reader done with the first screen, and back in the pool)
        const auto letGo = qScopeGuard(keepThePoolBusy());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 4 * limits.readerQuietMs + 2000);
        QCOMPARE(rig.ended.front().second, u"The server's screen was not read in time"_s);
        QVERIFY(rig.console.requests.size() >= 2); // (the large update was asked for, and came)
    }

    void aReaderThatTellsAsItDrainsALongHoldIsWaitedFor()
    {
        // ...and a reader that tells as it goes is waited for, however long its queue holds the line: its quiet is
        // timed from its last telling. A console pushing whole-screen JPEGs faster than they are drawn, with a whole
        // thread for its share (no pause), so that its queue holds the line. Sized by this build's own cost of a
        // frame (round-6 review LOW 1: fixed sizes reached the case in Debug alone): the quiet time some eight
        // frames' drawing, the holds four times that at the least, and the pushes three times the drawing's pace.
        // The console sees a hold as its pushes standing still: once at least for longer than the quiet time, and
        // the session goes on.
        // And its screen tall enough for the reader's pace: the reader counts each frame as it reads it, at its own
        // pace while its queue holds the line, against what the screen could change in a second (rfb.h,
        // mostScreensASecond, a JPEG counting an eighth of its pixels: rfb.cpp's `cheap`, mirrored here as
        // jpegCounts; were that changed, the guard below would end the session, loudly). Where a frame took 7.8 ms or
        // less to draw (the build box, in release), a screen of the frame's own size was more than that, and the
        // session was ended for it. The frames fill its top; the screen is as much taller as keeps twice this pace
        // within it. (So a fast PC reads by another path than a slow one: above 2400 rows a read of one frame is less
        // than half the screen, and is handed over by its rectangles, rfb_reader.cpp mostOf, where at 1200 every read
        // is the whole screen. Both are the reader's; the case, the pushes standing still, is asserted on both.)
        QImage frame(1920, 1200, QImage::Format_RGB888);
        for (int y = 0; y < 1200; ++y) {
            for (int x = 0; x < 1920; ++x) {
                frame.setPixel(x, y, qRgb(x / 8, y / 5, (x + y) / 13)); // (smooth: few bytes, a whole screen to decode)
            }
        }
        const QByteArray jpeg = jpegOf(frame, 50);
        const rfbt::Bytes one =
            rfbt::update({rfbt::tightJpeg(0, 0, 1920, 1200, std::vector<std::uint8_t>(jpeg.begin(), jpeg.end()))});
        rfbt::Bytes four =
            rfbt::join({rfbt::greeting(), rfbt::securityTypes({1}), rfbt::word(0), rfbt::serverInit(1920, 1200, "s")});
        for (int frames = 0; frames < 4; ++frames) {
            four.insert(four.end(), one.begin(), one.end());
        }
        const double cost = readingCostMs(four, 4);
        const int quiet = std::max(250, static_cast<int>(8 * cost));
        const int drained = std::max(48, static_cast<int>(std::ceil(4 * quiet / cost)));
        const int perPush = std::max(1, static_cast<int>(std::ceil(30 / cost)));
        constexpr double jpegCounts = 1.0 / 8;                                     // (rfb.cpp, cheap)
        const double counted = 2 * (1000 / cost) * (1920.0 * 1200.0 * jpegCounts); // twice the pixels a second, counted
        const int height = std::max(1200, static_cast<int>(std::ceil(counted / rfb::mostScreensASecond / 1920)));
        // (Below some 2.3 ms a frame, no screen the program takes is large enough: nothing is wrong with the program
        // there, and the case cannot be made on that PC.)
        if (height > rfb::largestSide || qint64{1920} * height > rfb::mostPixels) {
            QSKIP(qPrintable(u"a frame drawn in %1 ms: no screen the program takes is large enough"_s.arg(cost)));
        }
        qInfo(
            "long hold: a frame of %lld bytes costs %.1f ms here: the quiet time %d ms, holds of %d frames, %d frames "
            "a push, a screen of 1920 by %d",
            static_cast<long long>(jpeg.size()), cost, quiet, drained, perPush, height);
        RfbScreen::Limits limits = quick();
        limits.drawingSharePerMil = 1000;
        limits.readerQuietMs = quiet;
        limits.heldBackFromBytes = qint64{2} * drained * static_cast<qint64>(one.size());
        // (The server's own deadlines long: a pixel asked for meanwhile waits behind dozens of frames, in drawing
        // that counts as the server's time while the line is free.)
        limits.heartbeatMs = 30000;
        limits.messageWithinMs = 30000;
        limits.pieceWithinMs = 30000;
        Rig rig(limits);
        rig.console.width = 1920;
        rig.console.height = height;
        rig.console.answersChanges = false;
        rfbt::Bytes push;
        for (int frames = 0; frames < perPush; ++frames) {
            push.insert(push.end(), one.begin(), one.end());
        }
        rig.console.updates = {push};
        rig.console.piece = 256 * 1024; // (line messages of at the most a quarter of a megabyte)
        rig.console.pushEveryMs = 10;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.console.longestStallMs > qint64{2} * quiet || !rig.ended.empty(), 60000);
        QVERIFY2(rig.ended.empty(), rig.ended.empty() ? "" : qPrintable(rig.ended.front().second));
        QVERIFY2(rig.console.longestStallMs > quiet, QByteArray::number(rig.console.longestStallMs)); // (the case)
        qInfo("long hold: the pushes stood still %lld ms at the longest, the session going on",
              static_cast<long long>(rig.console.longestStallMs));
    }

    void aReaderQuietBeforeTheLineIsHeldIsTimedFromTheHold()
    {
        // ...and timed from the hold, not from before it: a still screen, nothing to tell for twice the reader's quiet
        // time; then a large update the server sends unasked while the pool is busy for 300 ms of its 2 s, so that it
        // holds the line before the reader can tell. It is read once the pool is free, and the session goes on
        // (with 1.7 s to spare for a busy machine: round-6 review LOW 2).
        RfbScreen::Limits limits = quick();
        limits.heldBackFromBytes = qint64{128} * 1024;
        limits.readerQuietMs = 2000;
        limits.heartbeatMs = 30000; // (a still screen: no pixel asked for meanwhile)
        limits.idleMs = 30000;
        Rig rig(limits);
        rig.console.width = 256;
        rig.console.height = 256;
        rig.console.answersChanges = false;
        rig.console.updates = {rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(16, 16))})};
        rig.console.piece = 16 * 1024;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        QTest::qWait(2 * limits.readerQuietMs);
        const int shown = rig.shown;
        {
            const auto letGo = qScopeGuard(keepThePoolBusy());
            rig.console.sendNow(rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(256, 256))}));
            QTest::qWait(300);
        }
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown > shown || !rig.ended.empty(), 5000);
        QVERIFY2(rig.ended.empty(), rig.ended.empty() ? "" : qPrintable(rig.ended.front().second));
    }

    void backToBackMessagesBehindALineThatCutsAnywhereAreNotEnded()
    {
        // round-5 review LOW 4, as a screen meets it: a console pushing small updates whose line's messages cut them
        // part way (six hundred updates sent in messages of 401 bytes, which never end where an update does), so no
        // batch of the reader's ends between messages. Each update is whole within milliseconds: none is ended as not
        // whole in time, over more than twice the message deadline. Its deadlines are 1.6 s, so a machine that is busy
        // for a while has more than a second to spare (round-6 review LOW 2), and the pieces last some 4 s.
        RfbScreen::Limits limits = quick();
        limits.messageWithinMs = 1600;
        limits.pieceWithinMs = 1600;
        Rig rig(limits);
        rig.console.width = 16;
        rig.console.height = 16;
        rfbt::Bytes stream;
        for (int one = 0; one < 600; ++one) {
            const rfbt::Bytes said = rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(8, 8, one))});
            stream.insert(stream.end(), said.begin(), said.end());
        }
        std::vector<rfbt::Bytes> pieces;
        for (std::size_t at = 0; at < stream.size(); at += 401) {
            const std::size_t end = std::min(at + 401, stream.size());
            pieces.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(at),
                                stream.begin() + static_cast<std::ptrdiff_t>(end));
        }
        pieces.push_back(rfbt::update({rfbt::rawRect(0, 0, rfbt::pattern(8, 8))})); // (then whole ones, while it runs)
        rig.console.updates = pieces;
        rig.console.answersChanges = false;
        rig.console.pushEveryMs = 10;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        QTest::qWait(2 * limits.messageWithinMs + 200);
        QVERIFY2(rig.ended.empty(), rig.ended.empty() ? "" : qPrintable(rig.ended.front().second));
        QVERIFY2(rig.shown > 50, QByteArray::number(rig.shown));
    }

    void anHonestFirstScreenIsDrawnNotRefused()
    {
        // confirm review 5: an honest first screen (a whole ZRLE screen, inflated and drawn in one batch through the
        // real reader) is drawn, not refused, in the debug build on the slow cores. The share is kept by this reader's
        // real processor time (not the wall it may lose to other work), and one screen is well within its first burst.
        Rig rig;
        rig.console.width = 640;
        rig.console.height = 480;
        rig.console.picture = rfbt::pattern(640, 480, 5);
        rfbt::Deflater stream;
        rig.console.updates = {rfbt::update({rfbt::zrleRect(0, 0, rig.console.picture, rfbt::ZrleTiles::Raw, stream)})};
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 20000);
        QCOMPARE(rig.screen->picture().width, 640);
        QVERIFY2(rig.ended.empty(), rig.ended.empty() ? "" : qPrintable(rig.ended.front().second));
    }

    void anHonestObmcStreamIsShownNotRefused()
    {
        // round-2 review, the mandated test: a realistic obmc-ikvm stream (a whole-screen baseline JPEG at 1920x1200,
        // pushed about thirty a second) read by the real reader on these slow cores. It is never refused; it is shown
        // more slowly where it costs more than its share of a drawing thread. Frames shown a second are reported.
        QImage frame(1920, 1200, QImage::Format_RGB888);
        for (int y = 0; y < 1200; ++y) {
            for (int x = 0; x < 1920; ++x) {
                frame.setPixel(x, y, qRgb((x + y) & 255, (x * 3) & 255, (y * 5) & 255));
            }
        }
        const QByteArray jpeg = jpegOf(frame, 50);
        QVERIFY2(jpeg.size() < qsizetype{2} * 1024 * 1024, QByteArray::number(jpeg.size())); // (fits Tight's length)
        Rig rig;
        rig.console.width = 1920;
        rig.console.height = 1200;
        rig.console.answersChanges = false;
        rig.console.updates = {
            rfbt::update({rfbt::tightJpeg(0, 0, 1920, 1200, std::vector<std::uint8_t>(jpeg.begin(), jpeg.end()))})};
        rig.console.pushEveryMs = 33; // about thirty a second
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        constexpr int seconds = 60;
        for (int one = 0; one < seconds; ++one) {
            QTest::qWait(1000);
            QVERIFY2(rig.ended.empty(), rig.ended.empty() ? "" : qPrintable(rig.ended.front().second));
        }
        qInfo("honest obmc stream: %d frames shown in %d s (%.1f a second), ended %d", rig.shown, seconds,
              static_cast<double>(rig.shown) / seconds, static_cast<int>(rig.ended.size()));
        QVERIFY(rig.shown > 0);
        QVERIFY(rig.ended.empty());
    }

    void aSessionThatServedAndEndedIsOpenedAgainAtOnceOneThatDidNotWaits()
    {
        Rig served;
        served.console.hangsUpAfterMs = 1300; // (past settledMs, 1 s)
        served.keepLooking();
        static_cast<void>(served.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!served.ended.empty(), 5000);
        QCOMPARE(served.ended.front().first, u"dropped"_s);
        QTRY_COMPARE_WITH_TIMEOUT(served.console.sessions, 2, 300); // (the ladder's first rung is 400 ms)

        Rig cut;
        cut.console.hangsUpAfterMs = 100;
        cut.keepLooking();
        static_cast<void>(cut.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!cut.ended.empty(), 5000);
        QTest::qWait(200);
        QCOMPARE(cut.console.sessions, 1);
        QTRY_COMPARE_WITH_TIMEOUT(cut.console.sessions, 2, 3000);
    }

    void aCardsStillIsMadeOfTheScreen()
    {
        Rig rig;
        rig.keepLooking();
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        std::optional<qkvm::base::Result<QByteArray>> still;
        rig.screen->still(480, [&still](qkvm::base::Result<QByteArray> made) { still = std::move(made); });
        QTRY_VERIFY_WITH_TIMEOUT(still.has_value(), 3000);
        const qkvm::base::Result<QByteArray> made = still.value_or(qkvm::base::Error{"none", "never answered"});
        QVERIFY(made.ok());
        QCOMPARE(made.value(), qkvm::base::screen::still<QByteArray>(64, 48, rig.console.picture.rgb, 480));
    }

    void closedNothingMoreIsOpened()
    {
        Rig rig;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(rig.shown >= 1, 5000);
        rig.screen->close();
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.watching, 0, 3000);
        QCOMPARE(rig.screen->look().error().code, std::string("no_picture"));
        QTest::qWait(300);
        QCOMPARE(rig.console.sessions, 1);
    }

    // --- keys and the pointer ------------------------------------------------------------------------------------

    void keyboardAndMouseAreGivenOnceObmcIkvmTakesThem()
    {
        Rig rig;
        rig.keepLooking();
        std::optional<qkvm::base::Result<ServerConsole::Turn>> turn;
        qint64 givenAt = -1;
        rig.screen->askForInput([&turn, &givenAt, &rig](qkvm::base::Result<ServerConsole::Turn> given) {
            turn = std::move(given);
            givenAt = rig.console.sinceMs();
        });
        QTRY_VERIFY_WITH_TIMEOUT(turn.has_value(), 5000);
        QVERIFY(turnIn(turn) >= 0);
        // Not in a client's first frames: 150 ms (here) after the handshake ended.
        QVERIFY(rig.console.initAtMs >= 0);
        QVERIFY2(givenAt - rig.console.initAtMs >= 140, qPrintable(QString::number(givenAt - rig.console.initAtMs)));
        QVERIFY(rig.screen->inTurn(turnIn(turn)).ok());
        // Asked again in the same session: the same turn, at once.
        std::optional<qkvm::base::Result<ServerConsole::Turn>> again;
        rig.screen->askForInput([&again](qkvm::base::Result<ServerConsole::Turn> given) { again = std::move(given); });
        QTRY_VERIFY_WITH_TIMEOUT(again.has_value(), 1000);
        QCOMPARE(turnIn(again), turnIn(turn));
    }

    void keysGoAsTheirKeysymsAndTheWholeKeyboardIsKeptInStep()
    {
        Rig rig;
        rig.keepLooking();
        std::optional<qkvm::base::Result<ServerConsole::Turn>> turn;
        rig.screen->askForInput([&turn](qkvm::base::Result<ServerConsole::Turn> given) { turn = std::move(given); });
        QTRY_VERIFY_WITH_TIMEOUT(turn.has_value() && turn->ok(), 5000);
        const ServerConsole::Turn mine = turnIn(turn);
        // Left Ctrl, then the A key with it; the Windows key (left), and Power, which has no keysym and goes nowhere.
        QVERIFY(rig.screen->keyboard({0x01, {}}, mine).ok());
        QVERIFY(rig.screen->keyboard({0x01, {0x04}}, mine).ok());
        QVERIFY(rig.screen->keyboard({0x09, {0x04, 0x66}}, mine).ok());
        QVERIFY(rig.screen->keyboard({}, mine).ok());
        using Key = std::pair<std::uint32_t, bool>;
        const std::vector<Key> expected{{0xFFE3, true}, {0x61, true},    {0xFFEB, true},
                                        {0x61, false},  {0xFFE3, false}, {0xFFEB, false}};
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.keys.size(), expected.size(), 3000);
        QVERIFY(rig.console.keys == expected);
    }

    void thePointerGoesAtItsPlaceInTheScreensPixelsWithRfbsButtons()
    {
        Rig rig;
        rig.keepLooking();
        std::optional<qkvm::base::Result<ServerConsole::Turn>> turn;
        rig.screen->askForInput([&turn](qkvm::base::Result<ServerConsole::Turn> given) { turn = std::move(given); });
        QTRY_VERIFY_WITH_TIMEOUT(turn.has_value() && turn->ok(), 5000);
        const ServerConsole::Turn mine = turnIn(turn);
        // (The line's buttons: 1 left, 2 right, 4 middle; RFB's: 1 left, 2 middle, 4 right, 8 and 16 the wheel.)
        QVERIFY(rig.screen->mouse(0, 32767, 0, 0, mine).ok());
        QVERIFY(rig.screen->mouse(1, 16383, 32767, 0, mine).ok());
        QVERIFY(rig.screen->mouse(2, 0, 0, 0, mine).ok());
        QVERIFY(rig.screen->mouse(4, 0, 0, 0, mine).ok());
        QVERIFY(rig.screen->mouse(0, 0, 0, 1, mine).ok());
        QVERIFY(rig.screen->mouse(0, 0, 0, -1, mine).ok());
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.pointers.size(), std::size_t{8}, 3000);
        const auto& said = rig.console.pointers;
        QCOMPARE(said[0].x, 63);
        QCOMPARE(said[0].y, 0);
        QCOMPARE(said[0].buttons, 0);
        QCOMPARE(said[1].x, 31);
        QCOMPARE(said[1].y, 47);
        QCOMPARE(said[1].buttons, 1);
        QCOMPARE(said[2].buttons, 4);
        QCOMPARE(said[3].buttons, 2);
        QCOMPARE(said[4].buttons, 8);  // a notch up: pressed...
        QCOMPARE(said[5].buttons, 0);  // ...and let go of
        QCOMPARE(said[6].buttons, 16); // and down
        QCOMPARE(said[7].buttons, 0);
    }

    void whatIsDownIsLetGoOfWhenTheConsoleIsClosed()
    {
        Rig rig;
        rig.keepLooking();
        std::optional<qkvm::base::Result<ServerConsole::Turn>> turn;
        rig.screen->askForInput([&turn](qkvm::base::Result<ServerConsole::Turn> given) { turn = std::move(given); });
        QTRY_VERIFY_WITH_TIMEOUT(turn.has_value() && turn->ok(), 5000);
        QVERIFY(rig.screen->keyboard({0x02, {0x05}}, turnIn(turn)).ok());
        QVERIFY(rig.screen->mouse(1, 100, 100, 0, turnIn(turn)).ok());
        rig.screen->close();
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.watching, 0, 3000);
        using Key = std::pair<std::uint32_t, bool>;
        const std::vector<Key> expected{{0xFFE1, true}, {0x62, true}, {0x62, false}, {0xFFE1, false}};
        QVERIFY(rig.console.keys == expected);
        QCOMPARE(rig.console.pointers.back().buttons, 0);
        // The turn is over with it.
        QCOMPARE(rig.screen->inTurn(turnIn(turn)).error().code, std::string("dropped"));
        QCOMPARE(rig.screen->keyboard({0, {0x04}}, turnIn(turn)).error().code, std::string("dropped"));
    }

    void aTurnEndsWithItsSession()
    {
        Rig rig;
        rig.console.hangsUpAfterMs = 400;
        rig.keepLooking();
        std::optional<qkvm::base::Result<ServerConsole::Turn>> turn;
        rig.screen->askForInput([&turn](qkvm::base::Result<ServerConsole::Turn> given) { turn = std::move(given); });
        QTRY_VERIFY_WITH_TIMEOUT(turn.has_value() && turn->ok(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 3000);
        QVERIFY(!rig.screen->inTurn(turnIn(turn)).ok());
        QVERIFY(!rig.screen->mouse(0, 1, 1, 0, turnIn(turn)).ok());
    }

    void keysWaitForAConsoleAtTheMostTheirTime()
    {
        RfbScreen::Limits limits = quick();
        limits.inputWaitMs = 600;
        Rig rig(limits);
        rig.console.silent = true; // (a handshake that never ends: no session that takes keys)
        std::optional<qkvm::base::Result<ServerConsole::Turn>> turn;
        rig.screen->askForInput([&turn](qkvm::base::Result<ServerConsole::Turn> given) { turn = std::move(given); });
        QTRY_VERIFY_WITH_TIMEOUT(turn.has_value(), 3000);
        QVERIFY(turnIn(turn) < 0);
        QCOMPARE(codeIn(turn), std::string("no_stream"));
    }

    void aTextIsTypedAsTheWebTypesItForObmcIkvm()
    {
        // Each character by its keysym, Shift down around those the US layout types with it (obmc-ikvm makes a keysym
        // into its key, and holds Shift only between a Shift key's own events).
        Rig rig;
        rig.keepLooking();
        std::optional<qkvm::base::Result<void>> typed;
        rig.screen->type(
            u"aB!\n"_s, [] { return true; }, [&typed](const qkvm::base::Result<void>& how) { typed = how; });
        QTRY_VERIFY_WITH_TIMEOUT(typed.has_value(), 5000);
        QCOMPARE(codeIn(typed), std::string("ok"));
        using Key = std::pair<std::uint32_t, bool>;
        const std::vector<Key> expected{{0x61, true},  {0x61, false},   {0xFFE1, true}, {0x42, true},
                                        {0x42, false}, {0xFFE1, false}, {0xFFE1, true}, {0x21, true},
                                        {0x21, false}, {0xFFE1, false}, {0xFF0D, true}, {0xFF0D, false}};
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.keys.size(), expected.size(), 3000);
        QVERIFY(rig.console.keys == expected);
        // A combination: pressed in its order, let go of in the other.
        rig.console.keys.clear();
        std::optional<qkvm::base::Result<void>> pressed;
        rig.screen->press(
            u"ControlLeft,AltLeft,Delete"_s, [] { return true; },
            [&pressed](const qkvm::base::Result<void>& how) { pressed = how; });
        QTRY_VERIFY_WITH_TIMEOUT(pressed.has_value(), 5000);
        QCOMPARE(codeIn(pressed), std::string("ok"));
        const std::vector<Key> chord{{0xFFE3, true},  {0xFFE9, true},  {0xFFFF, true},
                                     {0xFFFF, false}, {0xFFE9, false}, {0xFFE3, false}};
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.keys.size(), chord.size(), 3000);
        QVERIFY(rig.console.keys == chord);
    }

    void aTypingWhoseAskerMayNoLongerIsStoppedAndWhatCannotBeTypedIsRefused()
    {
        Rig rig;
        rig.keepLooking();
        std::optional<qkvm::base::Result<void>> typed;
        rig.screen->type(
            u"abc"_s, [] { return false; }, [&typed](const qkvm::base::Result<void>& how) { typed = how; });
        QTRY_VERIFY_WITH_TIMEOUT(typed.has_value(), 5000);
        QCOMPARE(codeIn(typed), std::string("stopped"));
        std::optional<qkvm::base::Result<void>> refused;
        rig.screen->type(
            QString(QChar(0x4E2D)), [] { return true; },
            [&refused](const qkvm::base::Result<void>& how) { refused = how; });
        QTRY_VERIFY_WITH_TIMEOUT(refused.has_value(), 5000);
        QCOMPARE(codeIn(refused), std::string("invalid"));
        QTest::qWait(200);
        QVERIFY(rig.console.keys.empty());
    }

    void aLongTextIsAskedForAgainBeforeEveryPieceAndStopsWhereItMayNoLonger()
    {
        // Forty characters are eighty key events, three pieces of at most 32 that leave nothing down: whoever asked is
        // asked before each (and before the keyboard is asked for); a no after the first piece stops it there.
        Rig rig;
        rig.keepLooking();
        int asked = 0;
        std::optional<qkvm::base::Result<void>> typed;
        rig.screen->type(
            QString(40, u'a'), [&asked] { return ++asked <= 2; },
            [&typed](const qkvm::base::Result<void>& how) { typed = how; });
        QTRY_VERIFY_WITH_TIMEOUT(typed.has_value(), 5000);
        QCOMPARE(codeIn(typed), std::string("stopped"));
        QTest::qWait(200);
        QCOMPARE(rig.console.keys.size(), std::size_t{32});
        QCOMPARE(rig.console.keys.back(), (std::pair<std::uint32_t, bool>{0x61, false}));
    }

    void aTypingStoppedWhileItWaitsForTheKeyboardKeepsNoConsole()
    {
        // Review cx6-1, L2: a typing stopped (the program locked, or made a wall screen: 6.18) while it waits for the
        // keyboard takes its asking along, as a server's screen does: nothing keeps the console wanted for it
        // afterwards (before: up to the time keys wait, a session made and held for nobody), and nobody looking, the
        // console is given back in its time. A typing asked for after it is typed, once.
        RfbScreen::Limits limits = quick();
        limits.idleMs = 300;
        limits.inputAfterMs = 4'000; // (the keyboard is given that long after a session is ready)
        limits.inputWaitMs = 20'000;
        Rig rig(limits);
        std::optional<qkvm::base::Result<void>> stopped;
        rig.screen->type(
            u"abc"_s, [] { return true; }, [&stopped](const qkvm::base::Result<void>& how) { stopped = how; });
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.watching, 1, 5000);
        rig.screen->stopTyping();
        QCOMPARE(codeIn(stopped), std::string("stopped"));
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.watching, 0, 2000);
        QTest::qWait(500);
        QCOMPARE(rig.console.watching, 0);
        QVERIFY(rig.console.keys.empty());
        // (And one asked for afterwards is typed whole, on its own asking.)
        std::optional<qkvm::base::Result<void>> typed;
        rig.screen->type(u"de"_s, [] { return true; }, [&typed](const qkvm::base::Result<void>& how) { typed = how; });
        QTRY_VERIFY_WITH_TIMEOUT(typed.has_value(), 10'000);
        QCOMPARE(codeIn(typed), std::string("ok"));
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.keys.size(), std::size_t{4}, 3000);
        QVERIFY(rig.console.keys.front() == (std::pair<std::uint32_t, bool>{0x64, true}));
    }

    void aLiveOfItHasKeyboardAndMouseThroughTheServersLive()
    {
        Rig rig;
        ServerLive live(rig.screen.get(), ServerLive::Limits{200});
        QObject::connect(&live, &Live::pictureCame, &live, [&rig](const Live::Picture&) { ++rig.asked; });
        std::vector<bool> control;
        QObject::connect(&live, &Live::controlChanged, &live,
                         [&control](bool on, const QString&, const QString&) { control.push_back(on); });
        live.askControl(true);
        QTRY_VERIFY_WITH_TIMEOUT(!control.empty(), 5000);
        QVERIFY(control.front());
        live.key(0x04, true);
        live.key(0x04, false);
        live.pointerAt(32767, 32767);
        live.button(1, true);
        live.button(1, false);
        using Key = std::pair<std::uint32_t, bool>;
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.keys.size(), std::size_t{2}, 3000);
        QVERIFY(rig.console.keys == (std::vector<Key>{{0x61, true}, {0x61, false}}));
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.pointers.size(), std::size_t{3}, 3000);
        QCOMPARE(rig.console.pointers[0].x, 63);
        QCOMPARE(rig.console.pointers[1].buttons, 1);
        QCOMPARE(rig.console.pointers[2].buttons, 0);
        live.close();
    }

    void theReaderLetsGoOfItsPasswordOnceItIsOfNoMoreUse()
    {
        // Outside review L5: the reader's own copy of the password is let go of once it answered the challenge, once
        // the handshake is over without one, and once the session ended before one; held until then.
        QObject home;
        aspeed::Picture picture;
        const auto bytesOf = [](const rfb::Bytes& bytes) {
            return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
        };
        // The reader, fed what a server says, and what it has to say back once it has read all of it.
        const auto readerWith = [&home](int& readies) {
            rfb::Options options;
            options.signsIn = {rfb::securityNone, rfb::securityVnc};
            QString password = u"secret"_s;
            return std::make_shared<RfbReader>(
                &home, [&readies] { ++readies; }, std::move(options), SecretText::takenFrom(password));
        };
        const auto saidAfter = [&picture](RfbReader& reader, int& readies, qsizetype bytes) {
            QByteArray said;
            static_cast<void>(QTest::qWaitFor(
                [&] {
                    if (readies > 0) {
                        readies = 0;
                        said += reader.take(picture).outgoing;
                    }
                    return said.size() >= bytes;
                },
                5000));
            return said;
        };
        {
            // Answered: held up to the challenge, and let go of with the answer.
            int readies = 0;
            const std::shared_ptr<RfbReader> reader = readerWith(readies);
            QVERIFY(reader->holdsAPassword());
            reader->feed(bytesOf(rfbt::join({rfbt::greeting(), rfbt::securityTypes({rfb::securityVnc})})));
            QCOMPARE(saidAfter(*reader, readies, 13).size(), qsizetype{13}); // (its version, and VNC authentication)
            QVERIFY(reader->holdsAPassword());
            reader->feed(QByteArray(16, '\x5a'));
            QCOMPARE(saidAfter(*reader, readies, 16).size(), qsizetype{16}); // (the answer)
            QVERIFY(!reader->holdsAPassword());
        }
        {
            // A server that asks for none: held until the handshake is over, and let go of then.
            int readies = 0;
            const std::shared_ptr<RfbReader> reader = readerWith(readies);
            reader->feed(
                bytesOf(rfbt::join({rfbt::greeting(), rfbt::securityTypes({rfb::securityNone}), rfbt::word(0)})));
            QCOMPARE(saidAfter(*reader, readies, 14).size(), qsizetype{14}); // (its version, None, ClientInit)
            QVERIFY(reader->holdsAPassword());
            reader->feed(bytesOf(rfbt::serverInit(160, 100, "stand-in")));
            QTRY_VERIFY(!reader->holdsAPassword());
        }
        {
            // A session that ended before a challenge: let go of with it.
            int readies = 0;
            const std::shared_ptr<RfbReader> reader = readerWith(readies);
            reader->feed(bytesOf(rfbt::greeting()));
            QCOMPARE(saidAfter(*reader, readies, 12).size(), qsizetype{12});
            QVERIFY(reader->holdsAPassword());
            reader->feed(bytesOf(rfbt::join({rfbt::securityTypes({}), rfbt::reason("not you")})));
            QTRY_VERIFY(!reader->holdsAPassword());
        }
    }

    void aChallengeAnsweredIsToldAsAPasswordThatWentWhateverCameAfterIt()
    {
        // Outside review 2.5: a server sends its challenge and, in the same breath, bytes that are no RFB (a result
        // that means nothing). The answer was made, and goes out, as the web's does (the challenge read, the answer
        // written, the result read after it): so it is told as a password that went, for whoever counts the sign-ins.
        QObject home;
        aspeed::Picture picture;
        int readies = 0;
        rfb::Options options;
        options.signsIn = {rfb::securityNone, rfb::securityVnc};
        QString password = u"secret"_s;
        const auto reader = std::make_shared<RfbReader>(
            &home, [&readies] { ++readies; }, std::move(options), SecretText::takenFrom(password));
        reader->feed(qtBytes(rfbt::join({rfbt::greeting(), rfbt::securityTypes({rfb::securityVnc})})));
        QTRY_VERIFY(readies > 0);
        readies = 0;
        static_cast<void>(reader->take(picture));
        reader->feed(qtBytes(rfbt::join({rfbt::Bytes(16, 0x5a), rfbt::word(7)})));
        QTRY_VERIFY(readies > 0);
        const RfbReader::Taken taken = reader->take(picture);
        QVERIFY(taken.ended.has_value());
        QCOMPARE(taken.ended.value_or(qkvm::base::Error{}).code, std::string(rfb::protocol));
        QCOMPARE(taken.outgoing.size(), qsizetype{16});
        QVERIFY(taken.passwordAnswered);
        // ...and the screen counts it before it goes, as every answer of a password.
        VncRig rig;
        rig.server.garblesAfterChallenge = true;
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"bad_response"_s);
        QCOMPARE(*rig.passwordsSent, 1);
        QTRY_COMPARE_WITH_TIMEOUT(rig.server.passwordTries, 1, 3000);
    }

    void anAnswerTheWayHoldsBackAsItGoesIsNotSent()
    {
        // Outside review 2.4: whether the answer to the challenge may go is asked as it goes (another sign-in with the
        // password may have gone out since the way was made). Where it may not, it is thrown away with the session,
        // which ends in the way's words; the server hears no answer.
        VncRig rig;
        rig.heldBack = qkvm::base::Error{"no_stream", "Waiting for another sign-in to the VNC server to be answered"};
        static_cast<void>(rig.screen->look());
        QTRY_VERIFY_WITH_TIMEOUT(!rig.ended.empty(), 5000);
        QCOMPARE(rig.ended.front().first, u"no_stream"_s);
        QCOMPARE(rig.ended.front().second, u"Waiting for another sign-in to the VNC server to be answered"_s);
        QTest::qWait(300);
        QCOMPARE(rig.server.passwordTries, 0);
        QCOMPARE(rig.server.connections, 1);
    }
};

QTEST_GUILESS_MAIN(TestRfbScreen)
#include "test_rfb_screen.moc"
