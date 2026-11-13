#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/base/screen_changes.h>
#include <qkvm/base/screen_still.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/redfish_device.h>

#include "ami_console_by_hand.h"
#include "aspeed_frames.h"
#include "memory_secrets.h"
#include "redfish_by_hand.h"
#include "stand_in.h"
#include "stillness.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::LocalWall;
using core::OwnKvm;
using core::testing::AmiConsoleByHand;
using core::testing::Elsewhere;
using core::testing::MemorySecrets;
using core::testing::RedfishByHand;
using core::testing::StandIn;
using core::testing::Stillness;
namespace aspeed = qkvm::bmc::aspeed;
using aspeed::frames::Frame;

Q_DECLARE_METATYPE(qkvm::core::Live::Picture)
Q_DECLARE_METATYPE(qkvm::core::Live::Video)

namespace {

core::testing::Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

QByteArray sha256Of(const char* name)
{
    return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
}

constexpr std::uint32_t white = 0xEB8080;
constexpr std::uint32_t black = 0x108080;

// A screen of 800 by 600, white on the left and black on the right, in one frame of blocks of eight by eight.
std::vector<Frame> aScreen()
{
    Frame first(800, 600);
    for (int row = 0; row < 600 / 8; ++row) {
        for (int block = 0; block < 800 / 8; ++block) {
            first.colours({{0, block < 50 ? white : black}});
        }
    }
    return {first};
}

// A busy screen: every block JPEG with detail, as a moving picture is (the bench's, smaller).
Frame aBusyScreen(int width, int height, std::uint64_t seed)
{
    Frame frame(width, height);
    aspeed::frames::Numbers below(seed);
    for (int block = 0; block < (width / 8) * (height / 8); ++block) {
        aspeed::frames::Coefficients y{};
        y[0] = below(200) - 100;
        for (std::size_t at = 1; at < 64; ++at) {
            if (below(100) < 30) {
                y[at] = below(61) - 30;
            }
        }
        frame.jpeg(y, {}, {});
    }
    return frame;
}

// A person at the controller's own page, as the console names them.
const QByteArray somebody =
    QByteArray("admin").leftJustified(64, '\0') + QByteArray("192.168.1.60").leftJustified(70, '\0');

core::KvmdDevice::Pace quickKvmd()
{
    core::KvmdDevice::Pace pace;
    pace.connectWithinMs = 5000;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.freshMs = 200;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    pace.pacing = std::make_shared<core::SignInPacing>();
    return pace;
}

// A server's controller whose own web page hands out the way into its console, and that console at /kvm on the same
// address (AMI's firmware): the console lets in the sessions of that page, with the passes it handed out.
struct Server {
    explicit Server(const char* certificate = "first") : bmc(RedfishByHand::Layout::Ami, tls(certificate))
    {
        console.screen = aScreen();
        console.piece = 4096;
        console.servesAt(
            *bmc.stand,
            [this](const QByteArray& cookie) {
                return cookie.startsWith("QSESSIONID=") && bmc.webSessions.contains(cookie.mid(11));
            },
            [this](const QByteArray& pass) { return bmc.consolePasses.remove(pass); });
    }
    RedfishByHand bmc;
    AmiConsoleByHand console;
};

// This PC's own wall for one test, its controllers' goodbyes its own, to be counted; and short times for its
// screens.
struct OwnWall {
    // (`change`: what a test has otherwise.)
    explicit OwnWall(const std::function<void(core::DriverPaces&)>& change = {})
        : goodbyes(std::make_shared<core::ControllerGoodbyes>()), drivers(driversWith(goodbyes, change)),
          settings(folder.filePath(u"settings.json"_s)), wall(settings, secrets, drivers)
    {}

    static core::DriverPaces driversWith(const std::shared_ptr<core::ControllerGoodbyes>& goodbyes,
                                         const std::function<void(core::DriverPaces&)>& change)
    {
        core::DriverPaces paces = core::DriverPaces::alike(quickKvmd());
        paces.redfish.goodbyes = goodbyes;
        // (The seats of this wall's controllers' consoles its own: a test's do not wait for another's.)
        paces.redfish.seats = std::make_shared<core::ConsoleSeats>();
        paces.redfish.signOutWithinMs = 1000;
        paces.redfish.hiccupMs = 0;
        paces.redfish.questionAgainMs = 100;
        core::ServerScreen::Limits& screen = paces.redfish.screen;
        screen.tickMs = 50;
        screen.idleMs = 3000;
        screen.retryMs = {300, 600, 900, 1200};
        screen.giveUpMs = 1500;
        screen.heldMs = 1500;
        screen.settledMs = 500;
        screen.keysSettleMs = 0;
        screen.masterWaitMs = 1500;
        screen.goodbyeWithinMs = 500;
        paces.redfish.live.lookEveryMs = 300;
        if (change) {
            change(paces);
        }
        return paces;
    }

    // A controller on the wall, as one that was added in the Add box is: asked what it is, known by its certificate.
    QString add(const Server& server) { return add(server.bmc.address().toString()); }
    QString add(const QString& address)
    {
        OwnKvm kvm;
        kvm.address = address;
        kvm.kind = u"redfish"_s;
        kvm.name = u"db-01"_s;
        kvm.knownByCertificate = true;
        kvm.certificate = sha256Of("first");
        const base::Result<QString> id = wall.add(kvm, u"secret"_s);
        return id.ok() ? id.value() : QString();
    }

    QTemporaryDir folder;
    std::shared_ptr<core::ControllerGoodbyes> goodbyes;
    core::DriverPaces drivers;
    core::Settings settings;
    MemorySecrets secrets;
    LocalWall wall;
};

template <typename Answer>
base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask,
                              int withinMs = 30000)
{
    QList<base::Result<Answer>> answers;
    ask([&](base::Result<Answer> answer) { answers.append(std::move(answer)); });
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, withinMs)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

template <typename T>
std::string codeOf(const base::Result<T>& result)
{
    return result.ok() ? std::string("(ok)") : result.error().code;
}

core::Status statusOf(LocalWall& wall, const QString& id)
{
    const base::Result<QHash<QString, core::Status>> all =
        answerTo<QHash<QString, core::Status>>([&](auto done) { wall.askStatus(std::move(done)); });
    return all.ok() ? all.value().value(id) : core::Status{};
}

// How it is once it has been looked at: asked until `holds`, or for a while.
core::Status cameTo(LocalWall& wall, const QString& id, const std::function<bool(const core::Status&)>& holds)
{
    core::Status status;
    static_cast<void>(QTest::qWaitFor(
        [&] {
            status = statusOf(wall, id);
            return holds(status);
        },
        30000));
    return status;
}

// The card's still, asked for until one comes (the first opens the console), or why none came at the last.
base::Result<core::Snapshot> stillOf(LocalWall& wall, const QString& id, int width = 320)
{
    base::Result<core::Snapshot> last = base::Error{"never", "No still was asked for."};
    static_cast<void>(QTest::qWaitFor(
        [&] {
            last = answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(id, width, 0, std::move(done)); });
            return last.ok() || last.error().code != "no_stream";
        },
        30000));
    return last;
}

// The server live, as its large view has it (LocalWall::openLive): what the wall hands over, or why not.
base::Result<std::unique_ptr<core::Live>> liveOf(LocalWall& wall, const QString& id)
{
    std::optional<base::Result<std::unique_ptr<core::Live>>> given;
    wall.openLive(id, core::Live::Need{},
                  [&given](base::Result<std::unique_ptr<core::Live>> live) { given.emplace(std::move(live)); });
    if (!QTest::qWaitFor([&given] { return given.has_value(); }, 30000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return std::move(*given);
}

std::optional<OwnKvm> kvmOf(const LocalWall& wall, const QString& id)
{
    for (const OwnKvm& kvm : wall.kvms()) {
        if (kvm.id == id) {
            return kvm;
        }
    }
    return std::nullopt;
}

bool can(const core::Status& status, const QString& word)
{
    return status.capabilities && status.capabilities->contains(word);
}

} // namespace

// A server's own screen on this PC's wall (M7 1c), from its controller's console (AMI's firmware) as its driver holds
// it: the card's still, the live window and its keys, keys and a text from the menu, and the console's manners (the
// server's monitor going dark, somebody else at the console, the session the controller ends after a while). Against
// RedfishByHand with AmiConsoleByHand at its /kvm. (Its own program: these wait out the console's times.)
class TestWallServerScreens : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<core::Live::Picture>();
        qRegisterMetaType<core::Live::Video>();
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void theCardShowsAStillOfTheServersScreen()
    {
        // The web's tile: `screen` and `live` where its console can be had, the still of the screen made smaller for
        // the card (the web's tiers: 480 for a card 320 wide), and keys, text and control once the console is known
        // to be AMI's. Never Basic, never a cookie to Redfish.
        Server server;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(!id.isEmpty());
        const core::Status first = cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; });
        QVERIFY(first.hasScreen);
        QVERIFY(can(first, u"screen"_s) && can(first, u"live"_s));
        // (Keys only once its console is known to be one that takes them.)
        QVERIFY(!can(first, u"keys"_s) && !can(first, u"control"_s));
        const base::Result<core::Snapshot> still = stillOf(own.wall, id, 320);
        QVERIFY2(still.ok(), still.ok() ? "" : still.error().message.c_str());
        const auto bytes = std::as_bytes(
            std::span(still.value().picture.constData(), static_cast<std::size_t>(still.value().picture.size())));
        const std::optional<base::screen::Changes> read = base::screen::read(bytes);
        QVERIFY(read.has_value());
        const base::screen::Changes made = read.value_or(base::screen::Changes{});
        QVERIFY(made.whole());
        QCOMPARE(made.width, 480);
        QCOMPARE(made.height, 360);
        // A wider card: a wider still, of the same screen.
        const base::Result<core::Snapshot> wider = stillOf(own.wall, id, 640);
        QVERIFY(wider.ok());
        const std::optional<base::screen::Head> head = base::screen::headOf(std::as_bytes(
            std::span(wider.value().picture.constData(), static_cast<std::size_t>(wider.value().picture.size()))));
        QVERIFY(head.has_value());
        QVERIFY(head.value_or(base::screen::Head{}).whole);
        QCOMPARE(head.value_or(base::screen::Head{}).width, 640);
        QCOMPARE(server.console.sessions, 1);
        const core::Status known = cameTo(own.wall, id, [](const core::Status& s) { return can(s, u"keys"_s); });
        QVERIFY(can(known, u"text"_s) && can(known, u"control"_s));
        QCOMPARE(known.width, 800);
        QCOMPARE(known.height, 600);
        QCOMPARE(server.bmc.authorizations, 0);
    }

    void theCardsStillOfABusyScreenIsMadeOffTheInterfaceThread()
    {
        // Review 3's HIGH 1 (2026-11-06): a card that shows a busy server (a scrolling log, an installer) asks for its
        // still again and again, and so keeps the console open: the frames, and each still made smaller from the
        // screen of 1920 by 1080 (every pixel of it read), are made on a thread of their own. The interface thread
        // works for no longer than about one frame of a monitor (16 ms) at a time meanwhile. (The controller on a
        // thread of its own too, so that what it does to send them is not counted.)
        Elsewhere elsewhere;
        const Elsewhere::Kept<Server> server(elsewhere);
        QString address;
        elsewhere.there([&] {
            server->console.screen.clear();
            for (std::uint64_t seed = 301; seed < 307; ++seed) {
                server->console.screen.push_back(aBusyScreen(1920, 1080, seed));
            }
            server->console.piece = 256 * 1024;
            address = server->bmc.address().toString();
        });
        OwnWall own;
        const QString id = own.add(address);
        QVERIFY(!id.isEmpty());
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        const base::Result<core::Snapshot> first = stillOf(own.wall, id, 320);
        QVERIFY2(first.ok(), first.ok() ? "" : first.error().message.c_str());
        // A card that asks every tenth of a second, while the frames come; until its still has stood for a while.
        QByteArray last = first.value().picture;
        int stills = 0;
        int changed = 0;
        QElapsedTimer stood;
        stood.start();
        QTimer card;
        connect(&card, &QTimer::timeout, this, [&] {
            own.wall.askSnapshot(id, 320, 0, [&](const base::Result<core::Snapshot>& still) {
                if (!still.ok()) {
                    return;
                }
                ++stills;
                if (still.value().picture != last) {
                    last = still.value().picture;
                    ++changed;
                    stood.restart();
                }
            });
        });
        Stillness stillness;
        stillness.start();
        card.start(100);
        QElapsedTimer took;
        took.start();
        while (took.elapsed() < 120000 && stood.elapsed() < 4000) {
            core::testing::runFor(20);
        }
        card.stop();
        stillness.stop();
        const QByteArray said = stillness.said();
        qInfo("A card of a busy 1080p screen: %d stills, %d of them new, in %lld ms; the interface thread was %s",
              stills, changed, static_cast<long long>(took.elapsed()), said.constData());
        // What the interface thread did itself in any one wait: within a monitor's frame (under the address sanitizer,
        // within what tells a frame read on it; before, the frames and the stills read on it, a wait of 612 ms). And
        // how long it was still at all, what it waited for and what Windows still takes from a thread above the PC's
        // other work among it (stillness.h says why the two are apart).
        QVERIFY2(stillness.longestWorkedMs() < core::testing::mostWorkInOneWaitMs, said.constData());
        QVERIFY2(stillness.longestMs() < core::testing::mostStillMs, said.constData());
        QVERIFY(changed >= 1);
        QVERIFY(stills >= 10);
        // The last still is the last frame's screen, made smaller.
        aspeed::Screen reference;
        elsewhere.there([&] {
            for (const Frame& frame : server->console.screen) {
                (void)reference.read(frame.header(), frame.data());
            }
        });
        QCOMPARE(last, (base::screen::still<QByteArray>(reference.picture().width, reference.picture().height,
                                                        reference.picture().rgb, 320)));
        own.wall.letGoOfEveryDriver();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        own.goodbyes->finish(own.drivers.redfish.signOutWithinMs + 2000);
    }

    void aDriverMadeAnewWaitsForTheOldOnesGoodbyeBeforeItOpensTheConsole()
    {
        // Review 3's MEDIUM 2 (2026-11-06): one seat of a controller's console for every driver of it in the program.
        // The card's password typed again makes its driver anew while its console says its goodbye, slowly (the
        // controller takes nothing more off the line, the goodbye neither: the line goes when the goodbye's time is
        // up). The new driver waits for the old one's line to go: it signs in to the controller's web page only after
        // the old one's session there was given back (as the line went), never two at once, and opens its console
        // session only then; and it is never told that somebody else has the console (that somebody would be this
        // program).
        Server server;
        OwnWall own([](core::DriverPaces& paces) { paces.redfish.screen.goodbyeWithinMs = 2000; });
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        QVERIFY(stillOf(own.wall, id).ok());
        QCOMPARE(server.console.sessions, 1);
        QCOMPARE(server.bmc.webLogins, 1);
        // The web page's sign-ins and sign-outs from here on, in the order they came, and how many of its sessions
        // were open at once.
        const auto web = std::make_shared<QList<QByteArray>>(); // (kept by the stand-in's hook too)
        qsizetype mostAtOnce = server.bmc.webSessions.size();
        server.bmc.whenAsked = [web](const core::testing::Asked& asked) {
            if (asked.path.startsWith("/api/session")) {
                web->append(asked.method + ' ' + asked.path);
            }
        };
        server.console.stall();
        const std::optional<OwnKvm> kvm = kvmOf(own.wall, id);
        QVERIFY(kvm.has_value());
        QVERIFY(own.wall.change(id, kvm.value_or(OwnKvm{}), u"secret"_s).ok());
        std::set<std::string> said;
        QElapsedTimer waited;
        waited.start();
        while (server.console.sessions < 2 && waited.elapsed() < 20000) {
            own.wall.askSnapshot(id, 320, 0, [&said](const base::Result<core::Snapshot>& still) {
                said.insert(still.ok() ? std::string("(ok)") : still.error().code);
            });
            QTest::qWait(50);
            mostAtOnce = std::max(mostAtOnce, server.bmc.webSessions.size());
        }
        server.console.readAgain();
        QCOMPARE(server.console.sessions, 2);
        QVERIFY2(waited.elapsed() >= 1500, QByteArray::number(waited.elapsed())); // (not before the old line went)
        QCOMPARE(*web, (QList<QByteArray>{"DELETE /api/session", "POST /api/session"}));
        QCOMPARE(mostAtOnce, qsizetype{1});
        const base::Result<core::Snapshot> still = stillOf(own.wall, id);
        QVERIFY2(still.ok(), still.ok() ? "" : still.error().message.c_str());
        QCOMPARE(server.bmc.webLogins, 2);
        for (const std::string& code : said) {
            QVERIFY2(code != "console_held" && code != "in_use", code.c_str());
        }
    }

    void aConsoleThatDarkensTheServersMonitorIsLeftAlone()
    {
        // Rule 15: where the pass says the server's own monitor goes dark while a console is open, the console is not
        // opened; the card says why (the web's no_screen) and offers no screen for that while.
        Server server;
        server.bmc.automaticOff = 1;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        const base::Result<core::Snapshot> still = stillOf(own.wall, id);
        QCOMPARE(codeOf(still), std::string("no_picture"));
        QCOMPARE(server.console.askings, 0);
        const core::Status said = statusOf(own.wall, id);
        QVERIFY(!said.hasScreen);
        QVERIFY(!can(said, u"screen"_s) && !can(said, u"keys"_s));
        QVERIFY(said.server.has_value());
        QCOMPARE(said.server.value_or(core::ServerReadings{}).noScreen, QString::fromStdString(still.error().message));
    }

    void somebodyElseAtTheConsoleIsNotAskedForIt()
    {
        // Rule 16: whoever had the console first is not asked for it: the session is left at once ("console_held"),
        // said on the card, and not asked for again for a while.
        Server server;
        server.console.heldBy = somebody;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        const base::Result<core::Snapshot> still = stillOf(own.wall, id);
        QCOMPARE(codeOf(still), std::string("console_held"));
        const int askings = server.console.askings;
        const core::Status said = cameTo(own.wall, id, [](const core::Status& s) { return !s.hasScreen; });
        QVERIFY(!said.hasScreen);
        QVERIFY(said.server && !said.server->noScreen.isEmpty());
        QCOMPARE(
            codeOf(answerTo<core::Snapshot>([&](auto done) { own.wall.askSnapshot(id, 320, 0, std::move(done)); })),
            std::string("console_held"));
        QCOMPARE(server.console.askings, askings);
    }

    void theLiveWindowShowsTheScreenAndTakesKeysAndTheMouse()
    {
        // Shown large: the server's screen live (ServerLive, as what changed of it), keyboard and mouse given once the
        // console is this program's, a key and a click arriving at the console, and let go of as the window closes.
        Server server;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        const base::Result<std::unique_ptr<core::Live>> opened = liveOf(own.wall, id);
        QVERIFY2(opened.ok(), opened.ok() ? "" : opened.error().message.c_str());
        core::Live& live = *opened.value();
        QSignalSpy videos(&live, &core::Live::videoChanged);
        QSignalSpy pictures(&live, &core::Live::pictureCame);
        QSignalSpy control(&live, &core::Live::controlChanged);
        QTRY_VERIFY_WITH_TIMEOUT(!pictures.isEmpty(), 20000);
        QVERIFY(!videos.isEmpty());
        QVERIFY(videos.constLast().constFirst().value<core::Live::Video>().changes);
        QCOMPARE(videos.constLast().constFirst().value<core::Live::Video>().width, 800);
        live.askControl(true);
        QTRY_VERIFY_WITH_TIMEOUT(!control.isEmpty(), 20000);
        QVERIFY2(control.constFirst().at(0).toBool(), qPrintable(control.constFirst().at(2).toString()));
        live.key(0x04, true); // A
        live.key(0x04, false);
        live.pointerAt(16383, 16383);
        live.button(1, true);
        live.button(1, false);
        QTRY_VERIFY_WITH_TIMEOUT(server.console.keyboards().size() >= 2, 10000);
        QVERIFY(std::ranges::any_of(server.console.keyboards(), [](const AmiConsoleByHand::Hid& one) {
            return one.keys == std::vector<int>{0x04};
        }));
        QVERIFY(server.console.keyboards().back().keys.empty());
        QTRY_VERIFY_WITH_TIMEOUT(!server.console.mice().empty(), 10000);
        live.close();
        QCOMPARE(server.console.badHid, QList<QByteArray>{});
        // Taken off the wall while it is live: the live ends in the wall's words for that (OwnLive::end).
        const base::Result<std::unique_ptr<core::Live>> again = liveOf(own.wall, id);
        QVERIFY(again.ok());
        QSignalSpy closed(again.value().get(), &core::Live::closed);
        QTRY_VERIFY_WITH_TIMEOUT(server.console.watching >= 1, 10000);
        QVERIFY(own.wall.remove(id).ok());
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 5000);
        QCOMPARE(closed.constFirst().constFirst().toString(), u"not_found"_s);
        QTRY_COMPARE_WITH_TIMEOUT(server.console.watching, 0, 5000);
    }

    void keysAndATextFromTheMenuGoOverTheConsole()
    {
        // The menu's keys and its typed text (an action of the wall's), as the web's `keys` and `text` over the
        // console; the keyboard's Power key never (as on every KVM of this wall).
        Server server;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        const base::Result<core::Done> pressed = answerTo<core::Done>([&](auto done) {
            own.wall.act(id, core::Action::press(u"ControlLeft,AltLeft,Delete"_s), std::move(done));
        });
        QVERIFY2(pressed.ok(), pressed.ok() ? "" : pressed.error().message.c_str());
        QVERIFY(!server.console.keyboards().empty());
        QCOMPARE(server.console.keyboards().front().bits, 0x05); // left Ctrl and left Alt
        QCOMPARE(server.console.keyboards().front().keys, std::vector<int>{0x4C});
        const auto keysBefore = server.console.keyboards().size();
        const base::Result<core::Done> power = answerTo<core::Done>(
            [&](auto done) { own.wall.act(id, core::Action::press(u"Power"_s), std::move(done)); });
        QCOMPARE(codeOf(power), std::string("invalid"));
        QCOMPARE(power.error().message, std::string(core::kvmd::powerKeyRefused));
        QCOMPARE(server.console.keyboards().size(), keysBefore);
        // Ctrl+Alt+Del as the web's own action for it.
        const base::Result<core::Done> salute = answerTo<core::Done>(
            [&](auto done) { own.wall.act(id, core::Action::plain(u"ctrl_alt_del"_s), std::move(done)); });
        QVERIFY2(salute.ok(), salute.ok() ? "" : salute.error().message.c_str());
        QVERIFY(server.console.keyboards().size() >= keysBefore + 2);
        QCOMPARE(server.console.keyboards().at(keysBefore).bits, 0x05);
        QCOMPARE(server.console.keyboards().at(keysBefore).keys, std::vector<int>{0x4C});
        const base::Result<core::Done> typed = answerTo<core::Done>(
            [&](auto done) { own.wall.act(id, core::Action::type(u"ab"_s, true), std::move(done)); });
        QVERIFY2(typed.ok(), typed.ok() ? "" : typed.error().message.c_str());
        // a, b, and Enter after them, each let go of.
        std::vector<int> down;
        for (const AmiConsoleByHand::Hid& one : server.console.keyboards()) {
            if (!one.keys.empty()) {
                down.push_back(one.keys.front());
            }
        }
        QCOMPARE(std::vector<int>(down.end() - 3, down.end()), (std::vector<int>{0x04, 0x05, 0x28}));
        QVERIFY(server.console.keyboards().back().keys.empty());
    }

    void aSessionTheControllerEndsIsOpenedAgain()
    {
        // Rule 16: the controller ends a console session after a while (about every half hour on a real board): a
        // session that showed the screen is opened again at once, and that is no failure on the card.
        Server server;
        server.console.sessionLastsMs = 1500;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        QVERIFY(stillOf(own.wall, id).ok());
        QVERIFY(QTest::qWaitFor(
            [&] {
                static_cast<void>(stillOf(own.wall, id));
                return server.console.sessions >= 2;
            },
            20000));
        QVERIFY(stillOf(own.wall, id).ok());
        QVERIFY(statusOf(own.wall, id).hasScreen);
    }

    void atTheProgramsEndTheConsoleSaysGoodbyeAndItsPageIsLeft()
    {
        // Item 14 of the plan: a screen that is looked at is closed before the drivers are let go of, saying its
        // console's goodbye while the loop runs; its web page's session given back within the goodbyes' time, and the
        // controller's Redfish session too; nothing asks the console afterwards.
        Server server;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        QVERIFY(stillOf(own.wall, id).ok());
        QCOMPARE(server.console.watching, 1);
        own.wall.letGoOfEveryDriver();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        own.goodbyes->finish(own.drivers.redfish.signOutWithinMs + 2000);
        QTRY_COMPARE_WITH_TIMEOUT(server.console.watching, 0, 3000);
        QVERIFY(server.bmc.webSessions.isEmpty());
        QVERIFY(server.bmc.sessions.isEmpty());
        const int askings = server.console.askings;
        QTest::qWait(500);
        QCOMPARE(server.console.askings, askings);
    }

    void aControllerWithNoConsoleOfItsOwnHasNoScreenForGood()
    {
        // The web's has_screen: offered until the controller is known to have no console that qKVM reads; from then on
        // never for this driver, also once what was said on the card has had its while (here 1.5 s), and its console
        // is not asked for again.
        Server server;
        server.bmc.ownConsole = false;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        QCOMPARE(codeOf(stillOf(own.wall, id)), std::string("no_picture"));
        const int asked = server.bmc.tokenAsks;
        QTest::qWait(std::max(own.drivers.redfish.screen.heldMs, own.drivers.redfish.screen.giveUpMs) + 500);
        const core::Status later = statusOf(own.wall, id);
        QVERIFY(!later.hasScreen);
        QVERIFY(!can(later, u"screen"_s) && !can(later, u"live"_s));
        QCOMPARE(
            codeOf(answerTo<core::Snapshot>([&](auto done) { own.wall.askSnapshot(id, 320, 0, std::move(done)); })),
            std::string("no_picture"));
        QCOMPARE(server.bmc.tokenAsks, asked);
        QCOMPARE(server.console.askings, 0);
    }

    void anotherServerAtItsAddressIsShownNothingOf()
    {
        // As every command (rule 20): another server answering at its address is shown nothing of, and its console is
        // not even asked for.
        Server server;
        OwnWall own;
        const QString id = own.add(server);
        QVERIFY(cameTo(own.wall, id, [](const core::Status& s) { return s.state == "online"_L1; }).hasScreen);
        server.bmc.anotherServer();
        const core::Status other =
            cameTo(own.wall, id, [](const core::Status& s) { return s.errorCode == "another_machine"_L1; });
        QCOMPARE(other.errorCode, u"another_machine"_s);
        QVERIFY(!other.hasScreen);
        QCOMPARE(
            codeOf(answerTo<core::Snapshot>([&](auto done) { own.wall.askSnapshot(id, 320, 0, std::move(done)); })),
            std::string("another_machine"));
        QCOMPARE(server.console.askings, 0);
        QCOMPARE(server.bmc.webLogins, 0);
    }
};

QTEST_GUILESS_MAIN(TestWallServerScreens)
#include "test_wall_server_screens.moc"
