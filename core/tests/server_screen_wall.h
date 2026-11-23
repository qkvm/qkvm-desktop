#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QTest>
#include <QThread>
#include <QThreadPool>

#include <qkvm/base/screen_changes.h>
#include <qkvm/core/server_live.h>
#include <qkvm/core/server_screen.h>

#include "ami_console_by_hand.h"
#include "chance.h"
#include "stillness.h"

// What the tests of a server's screen share (test_server_screen.cpp, its session and manners;
// test_server_screen_input.cpp, keys, the pointer and typings over it; test_server_screen_backlog.cpp, the same
// behind what waits to leave): the console by hand, a screen of it as the driver will make it, the frames they
// show, and what a typing was answered.
namespace qkvm::core::testing::screens {

using namespace Qt::StringLiterals;
using qkvm::core::ServerScreen;
using qkvm::core::testing::AmiConsoleByHand;
using qkvm::core::testing::Elsewhere;
using qkvm::core::testing::Stillness;
namespace ami = qkvm::bmc::ami;
namespace aspeed = qkvm::bmc::aspeed;
using aspeed::frames::Frame;

using qkvm::core::testing::Tls;

inline Tls tlsOf(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

inline QByteArray digestOf(const char* name)
{
    return QSslCertificate::fromPath(tlsOf(name).certificateFile, QSsl::Pem)
        .value(0)
        .digest(QCryptographicHash::Sha256);
}

// A busy screen: every block JPEG with detail, as a moving picture is (the bench's, smaller).
inline Frame aBusyScreen(int width, int height, std::uint64_t seed)
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

inline constexpr std::uint32_t white = 0xEB8080;
inline constexpr std::uint32_t black = 0x108080;
inline constexpr std::uint32_t red = 0x515AF0;

// A screen of 64 by 32: white on the left, black on the right; then one block of it turns red (the web's a_screen).
inline std::vector<Frame> aScreen()
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

// What looking at a screen says to a console, and nothing else (the web's LOOKING).
inline const std::set<std::uint16_t> looking = {ami::kind::complete,   ami::kind::pass,       ami::kind::resume,
                                                ami::kind::fullScreen, ami::kind::webSession, ami::kind::stillThere,
                                                ami::kind::behind,     ami::kind::stop};

// A person at the controller's own page, as the console names them.
inline const QByteArray somebody =
    QByteArray("admin").leftJustified(64, '\0') + QByteArray("192.168.1.60").leftJustified(70, '\0');

inline std::array<int, 3> at(const aspeed::Picture& picture, int x, int y)
{
    const auto start =
        3 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(picture.width) + static_cast<std::size_t>(x));
    return {picture.rgb[start], picture.rgb[start + 1], picture.rgb[start + 2]};
}

// The console by hand, and a screen that asks it for its way in as the driver will: a new pass for every session.
struct Wall {
    explicit Wall(ServerScreen::Limits limits = shortLimits(), const std::optional<Tls>& tls = {}) : console(tls)
    {
        console.screen = aScreen();
        screen = std::make_unique<ServerScreen>(
            [this](const ServerScreen::Answer& answer, const ServerScreen::Wanted& /*wanted*/) {
                ++wayIns;
                if (deferWayIn) {
                    pending = answer;
                    return;
                }
                if (refuseWayIn) {
                    answer(*refuseWayIn);
                    return;
                }
                const QByteArray pass = "pass-" + QByteArray::number(wayIns);
                console.passes.insert(pass);
                ServerScreen::WayIn way = wayIn;
                way.address = console.address();
                if (way.pass.isEmpty()) {
                    way.pass = pass;
                }
                way.done = [count = givenBack] { ++*count; }; // (counted beyond the wall, which may be gone)
                answer(way);
            },
            [this](const aspeed::Picture& picture, std::span<const aspeed::Rect> changed) {
                ++shown;
                lastSize = {picture.width, picture.height};
                rects.assign(changed.begin(), changed.end());
                if (onShown) {
                    onShown();
                }
            },
            limits);
    }

    static ServerScreen::Limits shortLimits()
    {
        ServerScreen::Limits limits;
        limits.tickMs = 50;
        return limits;
    }

    // Looks until the screen is shown, as a wall that is looked at does, every so often.
    bool shownWithin(int ms)
    {
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < ms) {
            if (screen->look().ok()) {
                return true;
            }
            QTest::qWait(30);
        }
        return false;
    }
    // Looks for so long, and says every code the screen was answered with meanwhile.
    std::set<std::string> lookFor(int ms)
    {
        std::set<std::string> seen;
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < ms) {
            const auto looked = screen->look();
            seen.insert(looked ? std::string("shown") : looked.error().code);
            QTest::qWait(30);
        }
        return seen;
    }
    void lookOnce() { (void)screen->look(); }
    // What looking says now: "shown", or why not.
    std::string code()
    {
        const auto looked = screen->look();
        return looked ? std::string("shown") : looked.error().code;
    }
    QByteArray message()
    {
        const auto looked = screen->look();
        return looked ? QByteArray() : QByteArray::fromStdString(looked.error().message);
    }
    std::set<std::uint16_t> heardKinds() const
    {
        std::set<std::uint16_t> found;
        for (const auto& one : console.heard) {
            found.insert(one.kind);
        }
        return found;
    }

    AmiConsoleByHand console;
    ServerScreen::WayIn wayIn = [] {
        ServerScreen::WayIn way;
        way.cookie = "QSESSIONID=s3ss10n"_ba;
        way.ownAddress = "192.168.1.20"_ba;
        way.controllerAddress = "192.168.1.50"_ba;
        way.webSession = "1234"_ba;
        return way;
    }();
    std::optional<qkvm::base::Error> refuseWayIn;
    bool deferWayIn = false;
    ServerScreen::Answer pending;
    std::function<void()> onShown;
    // Ways in whose web session was given back (WayIn::done): kept beyond the wall, where the last is given back after.
    std::shared_ptr<int> givenBack = std::make_shared<int>(0);
    int wayIns = 0;
    int shown = 0;
    std::pair<int, int> lastSize;
    std::vector<aspeed::Rect> rects;
    std::unique_ptr<ServerScreen> screen;
};

// What a typing was answered, once it was: "done", or the code and the sentence it was refused with (kept beyond
// whoever waits for it, which may be gone first).
using Outcome = std::shared_ptr<std::optional<std::pair<std::string, std::string>>>;

inline Outcome anOutcome()
{
    return std::make_shared<std::optional<std::pair<std::string, std::string>>>();
}

inline ServerScreen::Typed into(const Outcome& outcome)
{
    return [outcome](const qkvm::base::Result<void>& how) {
        *outcome =
            how ? std::pair{std::string("done"), std::string()} : std::pair{how.error().code, how.error().message};
    };
}

inline std::string codeOf(const Outcome& outcome)
{
    return outcome->value_or(std::pair{std::string("(not answered)"), std::string()}).first;
}

inline std::string messageOf(const Outcome& outcome)
{
    return outcome->value_or(std::pair{std::string(), std::string()}).second;
}

// What an answer was refused with, "(ok)" where it was not refused: never read off a good one (Result::error() of a
// good result is no error, and a release build crashed on it where the test should have failed, gate 48).
template <typename T>
std::string codeIn(const qkvm::base::Result<T>& answer)
{
    return answer.ok() ? std::string("(ok)") : answer.error().code;
}

// The console's keyboard and mouse, asked for and waited for: the turn, or why not.
inline qkvm::base::Result<ServerScreen::Turn> turnOf(ServerScreen& screen, int withinMs = 15000)
{
    auto got = std::make_shared<std::optional<qkvm::base::Result<ServerScreen::Turn>>>();
    screen.askForInput([got](qkvm::base::Result<ServerScreen::Turn> turn) { *got = std::move(turn); });
    QElapsedTimer waited;
    waited.start();
    while (!got->has_value() && waited.elapsed() < withinMs) {
        QTest::qWait(10);
    }
    return got->has_value() ? std::move(**got) : qkvm::base::Error{"never", "Never answered"};
}

// The keyboard reports a console took: the modifiers and the keys down of each.
using Keys = std::pair<int, std::vector<int>>;

inline std::vector<Keys> keysOf(const AmiConsoleByHand& console)
{
    std::vector<Keys> found;
    for (const AmiConsoleByHand::Hid& one : console.keyboards()) {
        found.emplace_back(one.bits, one.keys);
    }
    return found;
}

inline bool lastLetsGo(const AmiConsoleByHand& console)
{
    const std::vector<Keys> keys = keysOf(console);
    return !keys.empty() && keys.back() == Keys{0, {}};
}

inline const Keys letGo{0, {}};

} // namespace qkvm::core::testing::screens
