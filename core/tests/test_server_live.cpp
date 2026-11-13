#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/base/screen_changes.h>
#include <qkvm/core/server_live.h>
#include <qkvm/core/server_screen.h>

#include "ami_console_by_hand.h"

// A server's screen live (ServerLive), against a console by hand on this PC's own address: its pictures as what
// changed of the screen, the whole first, and keyboard and mouse over the same console, with a Live's manners.
using namespace Qt::StringLiterals;
using qkvm::core::Live;
using qkvm::core::ServerLive;
using qkvm::core::ServerScreen;
using qkvm::core::testing::AmiConsoleByHand;
namespace ami = qkvm::bmc::ami;
namespace aspeed = qkvm::bmc::aspeed;
namespace screen = qkvm::base::screen;
using aspeed::frames::Frame;

Q_DECLARE_METATYPE(qkvm::core::Live::Picture)
Q_DECLARE_METATYPE(qkvm::core::Live::Video)

namespace {

constexpr std::uint32_t white = 0xEB8080;
constexpr std::uint32_t black = 0x108080;
constexpr std::uint32_t red = 0x515AF0;

// A screen of 64 by 32: white on the left, black on the right; then one block turns red.
std::vector<Frame> aScreen()
{
    Frame first(64, 32);
    for (int row = 0; row < 4; ++row) {
        for (const std::uint32_t colour : {white, white, white, white, black, black, black, black}) {
            first.colours({{0, colour}});
        }
    }
    Frame second(64, 32);
    second.colours({{2, red}}, {}, std::pair{6, 2});
    return {first, second};
}

// A person at the controller's own page, as the console names them.
const QByteArray somebody =
    QByteArray("admin").leftJustified(64, '\0') + QByteArray("192.168.1.60").leftJustified(70, '\0');

// A console by hand, the screen of it as the driver will make it, and the live over the screen (made now, or later
// by makeLive()).
struct Wall {
    explicit Wall(ServerScreen::Limits limits = shortLimits(), ServerLive::Limits liveLimits = {300},
                  bool liveNow = true)
        : liveLimits(liveLimits)
    {
        console.screen = aScreen();
        // (Each frame a moment after the one before: these are about what each frame came to. Review 3's HIGH 1.)
        console.frameEveryMs = 150;
        screen = std::make_unique<ServerScreen>(
            [this](const ServerScreen::Answer& answer, const ServerScreen::Wanted& /*wanted*/) {
                const QByteArray pass = "pass-" + QByteArray::number(++wayIns);
                console.passes.insert(pass);
                ServerScreen::WayIn way;
                way.address = console.address();
                way.cookie = "QSESSIONID=s3ss10n"_ba;
                way.ownAddress = "192.168.1.20"_ba;
                way.controllerAddress = "192.168.1.50"_ba;
                way.webSession = "1234"_ba;
                way.pass = pass;
                answer(way);
            },
            [this](const aspeed::Picture& picture, std::span<const aspeed::Rect> changed) {
                ++frames;
                if (live != nullptr) {
                    live->shown(picture, changed);
                }
            },
            limits);
        if (liveNow) {
            makeLive();
        }
    }
    void makeLive()
    {
        live = std::make_unique<ServerLive>(screen.get(), liveLimits);
        QObject::connect(live.get(), &Live::pictureCame, live.get(),
                         [this](const Live::Picture& picture) { pictures.push_back(picture); });
        QObject::connect(live.get(), &Live::videoChanged, live.get(),
                         [this](const Live::Video& video) { videos.push_back(video); });
        QObject::connect(live.get(), &Live::controlChanged, live.get(),
                         [this](bool on, const QString& code, const QString&) {
                             words.push_back(on ? std::string("on") : "off:" + code.toStdString());
                         });
        QObject::connect(live.get(), &Live::leftDown, live.get(), [this] { words.emplace_back("left_down"); });
    }
    ~Wall()
    {
        live.reset(); // (before the screen it is over)
    }
    Wall(const Wall&) = delete;
    Wall& operator=(const Wall&) = delete;

    static ServerScreen::Limits shortLimits()
    {
        ServerScreen::Limits limits;
        limits.tickMs = 50;
        return limits;
    }
    // The screen as the pictures so far make it: each applied in turn, as the engine applies them.
    std::vector<std::uint8_t> drawn(int& width, int& height) const
    {
        std::vector<std::uint8_t> rgb;
        width = height = 0;
        for (const Live::Picture& picture : pictures) {
            const auto bytes =
                std::as_bytes(std::span(picture.data.constData(), static_cast<std::size_t>(picture.data.size())));
            const std::optional<screen::Changes> read = screen::read(bytes);
            if (!read) {
                continue;
            }
            if (read->whole()) {
                width = read->width;
                height = read->height;
                rgb.assign(std::size_t{3} * static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
            }
            if (read->width != width || read->height != height) {
                continue;
            }
            std::size_t at = 0;
            for (const screen::Rect& rect : read->rects) {
                for (int y = rect.y; y < rect.y + rect.height; ++y) {
                    for (int x = rect.x; x < rect.x + rect.width; ++x) {
                        const std::size_t to =
                            std::size_t{3} * (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                              static_cast<std::size_t>(x));
                        for (int c = 0; c < 3; ++c) {
                            rgb[to + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(read->pixels[at++]);
                        }
                    }
                }
            }
        }
        return rgb;
    }

    ServerLive::Limits liveLimits;
    AmiConsoleByHand console;
    int wayIns = 0;
    int frames = 0; // the screen showed
    std::vector<Live::Picture> pictures;
    std::vector<Live::Video> videos;
    std::vector<std::string> words;
    std::unique_ptr<ServerScreen> screen;
    std::unique_ptr<ServerLive> live;
};

bool keysLetGo(const AmiConsoleByHand& console)
{
    const std::vector<AmiConsoleByHand::Hid> keys = console.keyboards();
    return !keys.empty() && keys.back().bits == 0 && keys.back().keys.empty();
}

// The turn the screen's keyboard and mouse go in now (the live's own, where it holds them).
qkvm::base::Result<int> turnOf(ServerScreen& screen)
{
    auto got = std::make_shared<std::optional<qkvm::base::Result<int>>>();
    screen.askForInput([got](qkvm::base::Result<int> turn) { *got = std::move(turn); });
    QElapsedTimer waited;
    waited.start();
    while (!got->has_value() && waited.elapsed() < 15000) {
        QTest::qWait(10);
    }
    return got->has_value() ? std::move(**got) : qkvm::base::Error{"never", "Never answered"};
}

// A screen of 128 by 64, each row of blocks a colour of its own.
Frame aLargerScreen()
{
    Frame larger(128, 64);
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 16; ++column) {
            larger.colours({{0, row % 2 == 0 ? white : red}});
        }
    }
    return larger;
}

} // namespace

class TestServerLive : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qRegisterMetaType<Live::Picture>();
        qRegisterMetaType<Live::Video>();
    }

    void theScreenComesWholeAndThenAsWhatChanged()
    {
        // The whole screen first, said with its size, and a key frame; then what each frame changed. Applied in turn,
        // they are the screen the console shows.
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        QVERIFY(wall.live->isOpen());
        QCOMPARE(wall.videos.size(), std::size_t{1});
        QVERIFY(wall.videos.front().changes && !wall.videos.front().h264);
        QCOMPARE(wall.videos.front().width, 64);
        QCOMPARE(wall.videos.front().height, 32);
        const Live::Picture& first = wall.pictures.front();
        QVERIFY(first.changes && first.keyFrame && !first.h264);
        QCOMPARE(first.counter, quint32{1});
        QVERIFY(!wall.pictures[1].keyFrame);
        QCOMPARE(wall.pictures[1].counter, quint32{2});
        // The second frame changed one block: that, and nothing else, came.
        const auto bytes = std::as_bytes(
            std::span(wall.pictures[1].data.constData(), static_cast<std::size_t>(wall.pictures[1].data.size())));
        const screen::Changes changed = screen::read(bytes).value_or(screen::Changes{});
        QCOMPARE(changed.rects, (std::vector<screen::Rect>{{48, 16, 8, 8}}));
        int width = 0;
        int height = 0;
        const std::vector<std::uint8_t> rgb = wall.drawn(width, height);
        const aspeed::Picture& picture = wall.screen->picture();
        QCOMPARE(width, picture.width);
        QCOMPARE(height, picture.height);
        QVERIFY(rgb == picture.rgb);
    }

    void aKeyFrameIsTheWholeScreenAtOnce()
    {
        // Whoever draws it lost its place: the whole screen as it stands, at once, without the console being asked.
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        const std::size_t before = wall.pictures.size();
        const int heard = static_cast<int>(wall.console.heard.size());
        wall.live->askForAKeyFrame();
        QCOMPARE(wall.pictures.size(), before + 1);
        QVERIFY(wall.pictures.back().keyFrame);
        QCOMPARE(wall.videos.size(), std::size_t{1}); // (the same size: not said again)
        QCOMPARE(static_cast<int>(wall.console.heard.size()), heard);
        int width = 0;
        int height = 0;
        QVERIFY(wall.drawn(width, height) == wall.screen->picture().rgb);
    }

    void aNewSessionBeginsWithTheWholeScreen()
    {
        // The console's session ended (its time was up) and was opened again: its first frame goes whole. (A session
        // this short is one that came to nothing: the next is asked for after the first retry, made short here.)
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.retryMs = {200, 200, 200, 200};
        Wall wall(limits);
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        wall.console.hangsUp = true;
        wall.console.says.push_back(ami::message(ami::kind::stop, ami::timeUp));
        QTRY_VERIFY(!wall.screen->holdsTheConsole());
        wall.console.hangsUp = false;
        // The new session's first frame changes one block of a screen of the same size: the screen it makes is a
        // screen of its own all the same (nothing of the last session's is kept), and goes whole.
        wall.console.screen = {aScreen()[1]};
        const std::size_t before = wall.pictures.size();
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() > before && wall.screen->holdsTheConsole(), 15000);
        QVERIFY(wall.pictures[before].keyFrame);
        QCOMPARE(wall.videos.size(), std::size_t{1}); // (the same size)
        int width = 0;
        int height = 0;
        QVERIFY(wall.drawn(width, height) == wall.screen->picture().rgb);
    }

    void moreRectanglesThanAPacketHoldsGoAsTheWholeScreen()
    {
        // (A packet holds 4096 rectangles at the most; a frame of ASPEED's changes 256 at the most, but nothing here
        // rests on that.) As many as a packet holds go as they are; more go as the whole screen, not as nothing.
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        QTest::qWait(200);
        const aspeed::Picture& picture = wall.screen->picture();
        std::vector<aspeed::Rect> changed;
        changed.reserve(static_cast<std::size_t>(screen::mostRects) + 1);
        for (int one = 0; one < screen::mostRects + 1; ++one) {
            changed.push_back({one % picture.width, (one / picture.width) % picture.height, 1, 1});
        }
        const std::size_t before = wall.pictures.size();
        wall.live->shown(picture, std::span(changed).first(static_cast<std::size_t>(screen::mostRects)));
        QCOMPARE(wall.pictures.size(), before + 1);
        QVERIFY(!wall.pictures.back().keyFrame);
        const auto bytes = std::as_bytes(std::span(wall.pictures.back().data.constData(),
                                                   static_cast<std::size_t>(wall.pictures.back().data.size())));
        QCOMPARE(screen::read(bytes).value_or(screen::Changes{}).rects.size(), std::size_t{screen::mostRects});
        wall.live->shown(picture, changed);
        QCOMPARE(wall.pictures.size(), before + 2);
        QVERIFY(wall.pictures.back().keyFrame);
        int width = 0;
        int height = 0;
        QVERIFY(wall.drawn(width, height) == picture.rgb);
    }

    void whatCannotBeSaidAsChangesGoesAsTheWholeScreen()
    {
        // (After their reading, low 4.) What a frame changed cannot be said as changes (a rectangle that is not inside
        // the screen): the whole screen goes, rather than nothing (what it changed would be lost until the next whole).
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        QTest::qWait(200);
        const aspeed::Picture& picture = wall.screen->picture();
        const std::vector<aspeed::Rect> outside{{60, 28, 8, 8}};
        const std::size_t before = wall.pictures.size();
        wall.live->shown(picture, outside);
        QCOMPARE(wall.pictures.size(), before + 1);
        QVERIFY(wall.pictures.back().keyFrame);
        int width = 0;
        int height = 0;
        QVERIFY(wall.drawn(width, height) == picture.rgb);
    }

    void aScreenThrownAwayEndsTheLive()
    {
        // (After their reading, low 5.) The screen the live is over goes: the live says it is over, once, and nothing
        // more is said or sent.
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        const QSignalSpy closed(wall.live.get(), &Live::closed);
        const std::size_t pictures = wall.pictures.size();
        wall.screen.reset();
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).at(0).toString(), u"no_picture"_s);
        QVERIFY(!wall.live->isOpen());
        wall.live->askForAKeyFrame();
        wall.live->askControl(true);
        QTest::qWait(300);
        QCOMPARE(wall.pictures.size(), pictures);
        QVERIFY(wall.words.empty());
        QCOMPARE(closed.size(), 1);
    }

    void aLiveMadeOverAShownScreenBeginsWithItWhole()
    {
        // The driver holds one console for the card's still and the live picture: a live made while the screen is
        // shown already, and stands still (the console sends only what changes), has it whole at once.
        Wall wall(Wall::shortLimits(), {300}, false);
        QTRY_VERIFY_WITH_TIMEOUT(wall.screen->look().ok() && wall.frames >= 2, 10000);
        wall.makeLive();
        QVERIFY(wall.pictures.empty()); // (a turn of the loop later)
        QTRY_COMPARE(wall.pictures.size(), std::size_t{1});
        QVERIFY(wall.pictures.front().keyFrame);
        QCOMPARE(wall.videos.size(), std::size_t{1});
        int width = 0;
        int height = 0;
        QVERIFY(wall.drawn(width, height) == wall.screen->picture().rgb);
        QTest::qWait(300);
        QCOMPARE(wall.pictures.size(), std::size_t{1}); // (and nothing more while nothing changes)
    }

    void aScreenOfAnotherSizeIsSaidWithItsSize()
    {
        // The server's screen changed its size: the new one goes whole, said with its size.
        Wall wall;
        wall.console.screen.push_back(aLargerScreen());
        QTRY_VERIFY_WITH_TIMEOUT(wall.videos.size() >= 2, 10000);
        QCOMPARE(wall.videos.size(), std::size_t{2});
        QVERIFY(wall.videos[1].changes);
        QCOMPARE(wall.videos[1].width, 128);
        QCOMPARE(wall.videos[1].height, 64);
        QVERIFY(wall.pictures.back().keyFrame);
        int width = 0;
        int height = 0;
        QVERIFY(wall.drawn(width, height) == wall.screen->picture().rgb);
        QCOMPARE(width, 128);
    }

    void theLiveKeepsTheConsole()
    {
        // Nobody looks at the screen but the live: the console is kept, and not given back for nobody looking.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.idleMs = 600;
        Wall wall(limits, {150});
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        QTest::qWait(2000);
        QCOMPARE(wall.console.sessions, 1);
        QVERIFY(wall.screen->holdsTheConsole());
        wall.live->close();
        QTRY_COMPARE_WITH_TIMEOUT(wall.console.watching, 0, 5000); // (closed, the screen is looked at no more)
    }

    void keyboardAndMouseGoOnceTheyAreGiven()
    {
        // Asked for and given (one word, a turn of the loop later); keys as the whole keyboard, the pointer as a
        // place, buttons and the wheel; given back, what is down is let go of first, and one word says so.
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        QTRY_VERIFY(wall.screen->holdsTheConsole());
        wall.live->key(0x04, true); // (before keyboard and mouse are given: nothing)
        wall.live->askControl(true);
        wall.live->askControl(true); // (asked again while it is asked for: no second word)
        QVERIFY(wall.words.empty());
        QTRY_COMPARE(wall.words, (std::vector<std::string>{"on"}));
        QTest::qWait(100);
        QCOMPARE(wall.words, (std::vector<std::string>{"on"}));
        QVERIFY(wall.live->hasControl());
        wall.live->askControl(true); // (asked again while held: no second word)
        wall.live->key(0xE0, true);
        wall.live->key(0x06, true);
        wall.live->pointerAt(16384, 8192);
        wall.live->button(1, true);
        wall.live->button(4, true); // (back: no bit there)
        wall.live->wheel(3, -2);
        wall.live->pointerBy(16, 4); // (a quarter of the screen across, an eighth down)
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{7});
        const auto& hid = wall.console.hid;
        QVERIFY(hid[0].keyboard && hid[0].bits == 0x01 && hid[0].keys.empty());
        QVERIFY(hid[1].keyboard && hid[1].bits == 0x01 && hid[1].keys == std::vector<int>{0x06});
        QVERIFY(!hid[2].keyboard && hid[2].bits == 0 && hid[2].x == 16384 && hid[2].y == 8192);
        QVERIFY(!hid[3].keyboard && hid[3].bits == 1);
        QVERIFY(hid[4].wheel == -1 && hid[5].wheel == -1 && hid[4].bits == 1);
        QVERIFY(!hid[6].keyboard && hid[6].bits == 1);
        QCOMPARE(hid[6].x, 16384 + 16 * 32767 / 63);
        QCOMPARE(hid[6].y, 8192 + 4 * 32767 / 31);
        wall.live->pointerBy(-10000, 0); // (no further than the edge)
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{8});
        QVERIFY(hid[7].x == 0 && hid[7].y == 8192 + 4 * 32767 / 31);
        wall.live->pointerBy(16, 0); // (from the edge it stands at)
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{9});
        QCOMPARE(hid[8].x, 16 * 32767 / 63);
        wall.live->wheel(0, 40); // (a report a notch, sixteen at the most)
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{25});
        QVERIFY(hid[9].wheel == 1 && hid[24].wheel == 1);
        QTest::qWait(100);
        QCOMPARE(wall.console.hid.size(), std::size_t{25});
        wall.live->askControl(false);
        QCOMPARE(wall.words, (std::vector<std::string>{"on"})); // (a turn of the loop later)
        QTRY_COMPARE(wall.words, (std::vector<std::string>{"on", "off:"}));
        QVERIFY(!wall.live->hasControl());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{27});
        QVERIFY(keysLetGo(wall.console));
        QVERIFY(wall.console.mice().back().bits == 0);
        wall.live->key(0x04, true); // (given back: nothing more)
        QTest::qWait(200);
        QCOMPARE(wall.console.hid.size(), std::size_t{27});
    }

    void keyboardAndMouseRefusedSayWhy()
    {
        // A console that is somebody else's: no, in the screen's words; and a giving back while it was being asked
        // for: a no for the asking, and the word for the giving back.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.masterWaitMs = 400;
        Wall held(limits);
        held.console.holderReturning = true;
        QTRY_VERIFY_WITH_TIMEOUT(!held.pictures.empty(), 10000);
        held.live->askControl(true);
        QTRY_COMPARE_WITH_TIMEOUT(held.words, (std::vector<std::string>{"off:in_use"}), 10000);
        QVERIFY(!held.live->hasControl());
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(!wall.pictures.empty(), 10000);
        wall.console.firstSession = true; // (not in this session: its keyboard is attached, and it is asked anew)
        wall.live->askControl(true);
        wall.live->askControl(false);
        QTRY_COMPARE_WITH_TIMEOUT(wall.words.size(), std::size_t{2}, 10000);
        QCOMPARE(wall.words, (std::vector<std::string>{"off:given_up", "off:"}));
        QVERIFY(!wall.live->hasControl());
    }

    void keyboardAndMouseGoWhenTheConsoleIsGiven_data()
    {
        QTest::addColumn<bool>("byAKey");
        QTest::newRow("seen at a look") << false;
        QTest::newRow("seen at the next key") << true;
    }

    void keyboardAndMouseGoWhenTheConsoleIsGiven()
    {
        // Somebody at the controller's own page asks for the console: they have it, what was down is let go of first,
        // and the live is told that keyboard and mouse are not its any more ("in_use") as soon as it sees it: at its
        // next look, without a key, or at the next key, which does not go.
        QFETCH(bool, byAKey);
        Wall wall(Wall::shortLimits(), {byAKey ? 60000 : 300});
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        wall.live->askControl(true);
        QTRY_COMPARE(wall.words, (std::vector<std::string>{"on"}));
        wall.live->key(0x04, true);
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.asks.emplace_back(ami::kind::sharing, somebody);
        QTRY_VERIFY_WITH_TIMEOUT(!wall.screen->holdsTheConsole(), 5000);
        QTRY_VERIFY(keysLetGo(wall.console));
        const std::size_t heard = wall.console.hid.size();
        if (byAKey) {
            QTest::qWait(300);
            QCOMPARE(wall.words, (std::vector<std::string>{"on"})); // (not looked at yet)
            wall.live->key(0x05, true);
        }
        QTRY_COMPARE_WITH_TIMEOUT(wall.words, (std::vector<std::string>{"on", "off:in_use"}), 5000);
        QVERIFY(!wall.live->hasControl());
        wall.live->key(0x06, true);
        QTest::qWait(200);
        QCOMPARE(wall.console.hid.size(), heard);
        QCOMPARE(wall.words, (std::vector<std::string>{"on", "off:in_use"}));
        QVERIFY(wall.live->isOpen()); // (the screen is shown on)
    }

    void anAnswerIsDecidedAsTheConsoleIsThen()
    {
        // Keyboard and mouse given, and the console gone before the word for it was said: the word is that they went.
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2 && wall.screen->holdsTheConsole(), 10000);
        wall.live->askControl(true);
        wall.screen->close();
        QTRY_COMPARE(wall.words, (std::vector<std::string>{"off:dropped"}));
        QTest::qWait(700);
        QCOMPARE(wall.words, (std::vector<std::string>{"off:dropped"}));
        QVERIFY(!wall.live->hasControl());
    }

    void nothingIsSaidOrSentOnceClosed_data()
    {
        QTest::addColumn<bool>("held");
        QTest::newRow("closed while asking") << false;
        QTest::newRow("closed while giving back") << true;
    }

    void nothingIsSaidOrSentOnceClosed()
    {
        // Closed from here: no word comes after it, and keyboard and mouse given meanwhile are not this one's.
        QFETCH(bool, held);
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2 && wall.screen->holdsTheConsole(), 10000);
        wall.live->askControl(true);
        if (held) {
            QTRY_COMPARE(wall.words, (std::vector<std::string>{"on"}));
            wall.live->askControl(false);
        }
        wall.live->close();
        QTest::qWait(300);
        QCOMPARE(wall.words, held ? std::vector<std::string>{"on"} : std::vector<std::string>{});
        QVERIFY(!wall.live->hasControl());
        wall.live->key(0x04, true);
        QTest::qWait(200);
        QVERIFY(wall.console.hid.empty());
    }

    void whatMayHaveStayedDownIsSaidBeforeKeyboardAndMouseGo_data()
    {
        QTest::addColumn<int>("how");
        QTest::newRow("let go of in the turn") << 0;
        QTest::newRow("given back") << 1;
        QTest::newRow("closed") << 2;
    }

    void whatMayHaveStayedDownIsSaidBeforeKeyboardAndMouseGo()
    {
        // A console that takes nothing off its line: a key let go of is said behind what waits, and where it has not
        // left in time the screen ends the session (which lets go of it on the server). Whoever held keyboard and
        // mouse is told that something may have stayed down meanwhile, and then that they went; also where the key
        // was let go of as keyboard and mouse were given back, or as the live was closed (after their reading, M3:
        // before, nothing was said then).
        QFETCH(int, how);
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 300;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.letGoWithinMs = 600;
        limits.goodbyeWithinMs = 500;
        Wall wall(limits);
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        wall.live->askControl(true);
        QTRY_COMPARE(wall.words, (std::vector<std::string>{"on"}));
        wall.live->key(0xE1, true);
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        // What is said to the console waits now, until nothing that presses is said behind it.
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        qkvm::base::Result<void> said;
        int keys = 0;
        while (said.ok() && keys < 400'000) {
            said = wall.screen->keyboard({0x02, {0x04}}, turn.value());
            if (++keys % 200 == 0) {
                QCoreApplication::processEvents();
            }
        }
        QVERIFY2(!said.ok(), QByteArray::number(keys).constData());
        QVERIFY(wall.live->hasControl()); // (the turn stands: the console goes on)
        if (how == 0) {
            wall.live->key(0xE1, false);
            QTRY_COMPARE_WITH_TIMEOUT(wall.words, (std::vector<std::string>{"on", "left_down", "off:dropped"}), 10000);
        } else if (how == 1) {
            wall.live->askControl(false);
            QTRY_COMPARE_WITH_TIMEOUT(wall.words, (std::vector<std::string>{"on", "off:", "left_down"}), 10000);
        } else {
            wall.live->close();
            QTRY_COMPARE_WITH_TIMEOUT(wall.words, (std::vector<std::string>{"on", "left_down"}), 10000);
        }
        QTest::qWait(300);
        QCOMPARE(wall.words.size(), how == 2 ? std::size_t{2} : std::size_t{3}); // (and nothing more)
        QVERIFY(!wall.live->hasControl());
        QTest::qWait(1500); // (the goodbye's time: then its line is reset)
        wall.console.readAgain();
    }

    void whatAnotherLeftDownIsNotThisOnes()
    {
        // The same, of a typing's key (an action's, which asks for the keyboard by itself) while the live does not
        // hold keyboard and mouse: the live is told nothing of it.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 300;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.letGoWithinMs = 600;
        limits.goodbyeWithinMs = 500;
        limits.holdMs = 4000;
        Wall wall(limits);
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        const QSignalSpy leftDown(wall.screen.get(), &ServerScreen::leftDown);
        wall.screen->press(u"KeyA"_s, {}, [](const qkvm::base::Result<void>&) {});
        QTRY_VERIFY_WITH_TIMEOUT(!wall.console.hid.empty(), 10000);
        wall.console.stall();
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        qkvm::base::Result<void> said;
        int keys = 0;
        while (said.ok() && keys < 400'000) {
            said = wall.screen->keyboard({0, {0x04}}, turn.value());
            if (++keys % 200 == 0) {
                QCoreApplication::processEvents();
            }
        }
        QVERIFY2(!said.ok(), QByteArray::number(keys).constData());
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 15000);
        QCOMPARE(leftDown.size(), 1);
        QTest::qWait(200);
        QVERIFY(wall.words.empty());
        QTest::qWait(1500);
        wall.console.readAgain();
    }

    void closedOrThrownAwayItLetsGoOfWhatIsDown_data()
    {
        QTest::addColumn<bool>("thrownAway");
        QTest::newRow("closed") << false;
        QTest::newRow("thrown away") << true;
    }

    void closedOrThrownAwayItLetsGoOfWhatIsDown()
    {
        QFETCH(bool, thrownAway);
        Wall wall;
        QTRY_VERIFY_WITH_TIMEOUT(wall.pictures.size() >= 2, 10000);
        wall.live->askControl(true);
        QTRY_COMPARE(wall.words, (std::vector<std::string>{"on"}));
        wall.live->key(0xE2, true);
        wall.live->button(2, true);
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{2});
        if (thrownAway) {
            wall.live.reset();
        } else {
            wall.live->close();
        }
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{4});
        QVERIFY(keysLetGo(wall.console));
        QVERIFY(wall.console.mice().back().bits == 0);
        if (!thrownAway) {
            QTest::qWait(100);
            QCOMPARE(wall.words, (std::vector<std::string>{"on"})); // (nothing is said after close())
            QVERIFY(!wall.live->isOpen());
        }
    }
};

QTEST_GUILESS_MAIN(TestServerLive)
#include "test_server_live.moc"
