// A server's own screen on this PC's wall (M7 1c): its controller's console (ServerScreen) held by its driver, for
// the card's still and for whoever has it live (ServerLive), one console for both (the owner's decision,
// 2026-10-29); and keys and a text typed over it. The rest of the driver is in redfish_device.cpp and
// redfish_console_way.cpp.
#include <qkvm/core/redfish_device.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <QTimer>

#include <qkvm/base/screen_still.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/server_live.h>
#include <qkvm/core/server_screen.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// Why the screen cannot be had now that asking again soon does not change (the web's STANDING): said on the card
// until the screen would ask again (the web's no_screen, until its console_retry_at).
bool standing(const base::Error& why)
{
    return why.code == "no_picture" || why.code == "console_held";
}

// A server live that is not to be had: whoever asks is told why a turn of the loop after they hold it.
class RefusingLive final : public Live {
public:
    explicit RefusingLive(base::Error why) : m_why(std::move(why))
    {
        QTimer::singleShot(0, this, [this] {
            if (!m_closed) {
                emit closed(QString::fromStdString(m_why.code), QString::fromStdString(m_why.message));
            }
        });
    }
    bool isOpen() const override { return false; }
    void setNeed(Need /*need*/) override {}
    void askForAKeyFrame() override {}
    void askControl(bool /*on*/) override {}
    bool hasControl() const override { return false; }
    void key(int /*usage*/, bool /*down*/) override {}
    void pointerAt(int /*x*/, int /*y*/) override {}
    void pointerBy(int /*dx*/, int /*dy*/) override {}
    void button(int /*number*/, bool /*down*/) override {}
    void wheel(int /*dx*/, int /*dy*/) override {}
    void releaseAll() override {}
    void close() override { m_closed = true; }

private:
    base::Error m_why;
    bool m_closed = false;
};

// What has the server live for all who show it: its driver's one screen (RedfishDevice::openLive), and a live of its
// own for each who asks. Taken away (end): every live of it ends in those words, and none is opened after.
class RedfishOwnLive final : public OwnLive {
public:
    explicit RedfishOwnLive(RedfishDevice* driver) : m_driver(driver) {}

    std::unique_ptr<Live> open(Live::Need need) override
    {
        if (m_ended || m_driver == nullptr) {
            return std::make_unique<RefusingLive>(m_ended ? m_why : RedfishDevice::noPicture());
        }
        return m_driver->openLive(need);
    }

    void end(const base::Error& why) override
    {
        m_ended = true;
        m_why = why;
        if (m_driver != nullptr) {
            m_driver->endLives(why);
        }
    }

private:
    QPointer<RedfishDevice> m_driver;
    bool m_ended = false;
    base::Error m_why;
};

} // namespace

void RedfishDevice::controllerSaid(const QJsonObject* manager)
{
    const QString console = manager != nullptr ? redfish::rfbConsole(*manager) : QString();
    m_controllerKnown = true;
    m_isOpenBmc = manager != nullptr && redfish::isOpenBmc(*manager);
    if (m_isOpenBmc) {
        // Its console is the one it announces, or none: it has no web page of AMI's, whose door it answers 401 as AMI's
        // does, and whose sign-in would be sent the controller's password (the web's _itself sets _has_console False).
        m_hasConsole = false;
    }
    if (console == m_rfbConsole) {
        return;
    }
    m_rfbConsole = console;
    // A screen of the other console is let go of: every live of it ends, and the next is of the console there is now.
    if (m_screen != nullptr && m_screenIsRfb != !m_rfbConsole.isEmpty()) {
        endLives(base::Error{"no_input", "This server's controller has changed since: ask for its screen again"});
        m_screen.reset();
        m_still.reset();
        m_noScreen.reset();
    }
}

std::optional<base::Error> RedfishDevice::notAmisConsole() const
{
    // An OpenBMC controller is never offered the password on AMI's web page, whether it announces a console (RFB, shown
    // another way) or not (its KVM off): it answers AMI's door 401 as AMI's does, so the probe's 401 is no proof
    // (finding 4, the web's _openbmc_known). Known OpenBMC closes the window the probe's answer would open.
    if (m_isOpenBmc || !m_rfbConsole.isEmpty()) {
        return base::Error{"no_picture", "This server's controller runs OpenBMC"};
    }
    // Never on the strength of nothing: a console is opened only once the controller is positively known what it is
    // (its manager read), or its service root says it is AMI's.
    if (!m_controllerKnown && !(m_places && redfish::amiRoot(m_places->root))) {
        return base::Error{"no_stream", "Waiting for the controller to say what it is"};
    }
    return std::nullopt;
}

bool RedfishDevice::noConsoleHere() const
{
    // An OpenBMC with no announced console shows no console the AMI way, whatever the unsigned probe's 401 made of
    // m_hasConsole (finding 4): what the look learned (it runs OpenBMC) stands.
    return m_rfbConsole.isEmpty() && (m_hasConsole == std::optional<bool>(false) || m_isOpenBmc);
}

void RedfishDevice::rfbWay(std::function<void(base::Result<RfbScreen::WayIn>)> done)
{
    if (const std::optional<base::Error> why = keptFromTheScreen()) {
        later([done = std::move(done), why = *why] { done(why); });
        return;
    }
    if (m_rfbConsole.isEmpty()) {
        later([done = std::move(done)] { done(base::Error{"no_picture", "This controller announces no console"}); });
        return;
    }
    // Opened only under the session its computer was found to be this server's under, as a command goes: another
    // server's screen is not shown here (the web's rfb_socket).
    whichServerForTheConsole(1, [this, done = std::move(done)](const std::optional<base::Error>& why) {
        if (why) {
            done(*why);
            return;
        }
        if (m_session.token.isEmpty() || (!m_machine.isEmpty() && m_session.token != m_checkedToken)) {
            done(base::Error{"no_stream",
                             "The controller signed this program in anew while it was asked which server it is"});
            return;
        }
        RfbScreen::WayIn way;
        way.address = m_where.address;
        way.knownByCertificate = m_where.knownByCertificate;
        way.certificate = m_where.certificate;
        way.token = m_session.token;
        way.path = m_rfbConsole.toLatin1();
        done(std::move(way));
    });
}

ServerConsole& RedfishDevice::screen()
{
    if (m_screen == nullptr && !m_rfbConsole.isEmpty()) {
        // OpenBMC's console: RFB at /kvm/0, under the driver's own session (no web page, no pass, no seat of AMI's).
        RfbScreen::Asker asker = [driver = QPointer<RedfishDevice>(this)](const RfbScreen::Answer& answer,
                                                                          const RfbScreen::Wanted& wanted) {
            if (driver == nullptr || !wanted()) {
                answer(base::Error{"stopped", "Nobody waits for the server's screen any more"});
                return;
            }
            driver->rfbWay(answer);
        };
        m_screen = std::make_unique<RfbScreen>(
            std::move(asker),
            [this](const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed) {
                shownOnTheScreen(picture, changed);
            },
            m_pace.rfb);
        m_screenIsRfb = true;
        connect(m_screen.get(), &ServerConsole::ended, this, [this](const QString& code, const QString& message) {
            notedOnTheScreen(base::Error{code.toStdString(), message.toStdString()});
        });
    }
    if (m_screen == nullptr) {
        m_screenIsRfb = false;
        // One seat of the controller's console at a time, for every driver of it in the program (a driver made anew
        // waits for the old one's goodbye: review 3's MEDIUM 2), found by its address and its own number as this
        // driver knows them when it asks; asked through this driver's own way in (consoleWay: which server answers
        // asked first, its web page signed in to, the pass for one session).
        const std::shared_ptr<ConsoleSeats>& seats = m_pace.seats ? m_pace.seats : ConsoleSeats::program();
        const auto of = [driver = QPointer<RedfishDevice>(this)]() -> std::optional<ConsoleSeats::Of> {
            if (driver == nullptr) {
                return std::nullopt;
            }
            return ConsoleSeats::Of{driver->m_where.address, redfish::marksOf(driver->m_machine).controller};
        };
        ServerScreen::Asker asker =
            seats->asker(of, [driver = QPointer<RedfishDevice>(this)](const ServerScreen::Answer& answer,
                                                                      const ServerScreen::Wanted& wanted) {
                // (Nothing is asked of the controller for a screen that no longer waits; and the seat is freed by an
                // answer all the same.)
                if (driver == nullptr || !wanted()) {
                    answer(base::Error{"stopped", "Nobody waits for the server's screen any more"});
                    return;
                }
                driver->consoleWay([answer](base::Result<ConsoleWay> way) {
                    if (!way) {
                        answer(way.error());
                        return;
                    }
                    ConsoleWay& given = way.value();
                    ServerScreen::WayIn in;
                    in.address = given.address;
                    in.knownByCertificate = given.knownByCertificate;
                    in.certificate = given.certificate;
                    in.cookie = std::move(given.cookie);
                    in.pass = std::move(given.pass);
                    in.ownAddress = std::move(given.ownAddress);
                    in.controllerAddress = std::move(given.controllerAddress);
                    in.webSession = std::move(given.webSession);
                    in.completeFirst = given.completeFirst;
                    in.done = std::move(given.done);
                    answer(std::move(in));
                });
            });
        m_screen = std::make_unique<ServerScreen>(
            std::move(asker),
            [this](const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed) {
                shownOnTheScreen(picture, changed);
            },
            m_pace.screen);
        // A session that ended for a reason that stands for a while: said on the card for that while.
        connect(m_screen.get(), &ServerConsole::ended, this, [this](const QString& code, const QString& message) {
            notedOnTheScreen(base::Error{code.toStdString(), message.toStdString()});
        });
    }
    return *m_screen;
}

void RedfishDevice::notedOnTheScreen(const base::Error& why)
{
    if (standing(why)) {
        m_noScreen = why;
        m_noScreenUntil = now() + std::max(m_pace.screen.heldMs, m_pace.screen.giveUpMs);
    }
}

void RedfishDevice::shownOnTheScreen(const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed)
{
    // The screen shows again: nothing stands in its way.
    m_noScreen.reset();
    ++m_framesShown;
    m_lives.removeIf([](const QPointer<ServerLive>& live) { return live == nullptr; });
    for (const QPointer<ServerLive>& live : std::as_const(m_lives)) {
        live->shown(picture, changed);
    }
}

std::optional<base::Error> RedfishDevice::noScreen() const
{
    if (m_noScreen && now() < m_noScreenUntil) {
        return m_noScreen;
    }
    return std::nullopt;
}

std::optional<base::Error> RedfishDevice::keptFromTheScreen() const
{
    // Another server at its address; and a computer its card asks about (the owner's decision of 2026-11-22: nothing
    // but reads goes to it, its picture, keys and text not either; review cx2-2, M1).
    if (m_other) {
        return m_other;
    }
    if (m_checkedAsking) {
        return asksFirst();
    }
    return std::nullopt;
}

void RedfishDevice::askingFromNow()
{
    // A console open from before goes, with every live of it and whatever is being typed (review cx2-2, M1: it went on
    // while the card asked).
    letGoOfTheScreen();
}

void RedfishDevice::letGoOfTheScreen()
{
    // A turn of the loop later: the judge that turned to asking may run inside the console's own asking for its way in.
    // And only where something keeps the screen from it still: the card asks, or was told No (notItsServer: review
    // cx2-4, L1).
    later([this] {
        const std::optional<base::Error> why = keptFromTheScreen();
        if (!why) {
            return;
        }
        stopTyping();
        endLives(*why);
        m_screen.reset();
        m_still.reset();
    });
}

bool RedfishDevice::hasScreen() const
{
    // As the web's: where it is not known to have no console, and nothing stands in its way for now; never of another
    // server at its address.
    return !noConsoleHere() && !noScreen() && !keptFromTheScreen();
}

void RedfishDevice::askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done)
{
    // Another server at its address is shown nothing of (its console is not even asked for); a controller with no
    // console here has no picture.
    if (const std::optional<base::Error> why = keptFromTheScreen()) {
        later([done = std::move(done), why = *why] { done(why); });
        return;
    }
    if (noConsoleHere()) {
        later([done = std::move(done)] { done(noPicture()); });
        return;
    }
    const base::Result<void> looked = screen().look();
    if (!looked) {
        notedOnTheScreen(looked.error());
        later([done = std::move(done), why = looked.error()] { done(why); });
        return;
    }
    // (Made once for each frame and width: a card that asks again before the screen changed is handed the same, as
    // the web's encoded stills are.)
    const int tier = base::screen::stillWidthFor(width, m_screen->picture().width);
    if (m_still && m_still->frames == m_framesShown && m_still->tier == tier) {
        later([done = std::move(done), still = Snapshot{m_still->picture}] { done(still); });
        return;
    }
    // Made smaller where the frames are read, off this thread (every pixel of the screen is read for it: review 3's
    // HIGH 1), from the screen as it stands then: at least as new as the frame it is kept for.
    // (Answered a turn of the loop later, as everything a card asks of this: never from inside the screen's own end.)
    m_screen->still(width, [here = QPointer<RedfishDevice>(this), frames = m_framesShown, tier,
                            done = std::move(done)](base::Result<QByteArray> made) mutable {
        if (here == nullptr) {
            return; // (the driver went with its screen: the wall tells whoever asked)
        }
        if (made) {
            here->m_still = MadeStill{frames, tier, made.value()};
        }
        here->later([done = std::move(done), made = std::move(made)] {
            if (made) {
                done(Snapshot{made.value()});
            } else {
                done(made.error());
            }
        });
    });
}

std::unique_ptr<OwnLive> RedfishDevice::makeLive()
{
    return std::make_unique<RedfishOwnLive>(this);
}

std::unique_ptr<Live> RedfishDevice::openLive(Live::Need need)
{
    if (const std::optional<base::Error> why = keptFromTheScreen()) {
        return std::make_unique<RefusingLive>(*why);
    }
    if (noConsoleHere()) {
        return std::make_unique<RefusingLive>(noPicture());
    }
    auto live = std::make_unique<ServerLive>(&screen(), m_pace.live);
    live->setNeed(need);
    m_lives.append(live.get());
    return live;
}

void RedfishDevice::endLives(const base::Error& why)
{
    for (const QPointer<ServerLive>& live : std::as_const(m_lives)) {
        if (live == nullptr) {
            continue;
        }
        // What is down is let go of first (ServerLive::close), and the one who has it told why a turn later.
        live->close();
        // NOLINTNEXTLINE(bugprone-exception-escape)
        QTimer::singleShot(0, live.data(), [live, why] {
            if (live != nullptr) {
                emit live->closed(QString::fromStdString(why.code), QString::fromStdString(why.message));
            }
        });
    }
    m_lives.clear();
}

void RedfishDevice::typeOnTheScreen(const Action& what, std::function<void(base::Result<Done>)> done)
{
    // Nothing goes to another server at its address, nor to a computer its card asks about, and nothing where there is
    // no console here.
    if (const std::optional<base::Error> why = keptFromTheScreen()) {
        later([done = std::move(done), why = *why] { done(why); });
        return;
    }
    if (noConsoleHere()) {
        later([done = std::move(done)] { done(noPicture()); });
        return;
    }
    // The keyboard's Power key is never sent, whatever else is pressed with it (as on every KVM of this wall: a5).
    if (what.kind == "keys"_L1 && kvmd::namesThePowerKey(what.keys)) {
        later([done = std::move(done)] { done(base::Error{"invalid", kvmd::powerKeyRefused}); });
        return;
    }
    const QPointer<RedfishDevice> here(this);
    // (Nor does a text typed go on once its card asks, or once anything else keeps the screen from it: another server
    // found at its address meanwhile, or a No its card was told: review cx2-2, M1; cx2-4, L1 and L3.)
    const ServerConsole::Still still = [here] { return here != nullptr && !here->keptFromTheScreen(); };
    const auto said = [done](const base::Result<void>& how) {
        if (!how) {
            done(how.error());
            return;
        }
        done(Done{});
    };
    if (what.kind == "keys"_L1) {
        screen().press(what.keys, still, said);
        return;
    }
    if (what.kind == "ctrl_alt_del"_L1) {
        screen().press(u"ControlLeft,AltLeft,Delete"_s, still, said);
        return;
    }
    // A text, and Enter after it where that is asked: in the one typing, in the text's turn of the keyboard (outside
    // review 2.3), as the web types the text with a "\n" after it.
    screen().type(what.text, still, said, what.enter);
}

} // namespace qkvm::core
