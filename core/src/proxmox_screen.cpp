// A Proxmox guest's screen on this PC's wall (c9b, wave 2): the console its Proxmox hands out (vncproxy, then
// vncwebsocket: RFB over a WebSocket, RfbScreen) held by its driver, for the card's still and for whoever has it live
// (ServerLive), one console for both as a server's; and keys and a text typed over it. The rest of the driver is in
// proxmox_device.cpp. As the web's proxmox.py of 0.3.1 (ProxmoxConsole, console_ticket, console_socket).
#include <qkvm/core/proxmox_device.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

#include <qkvm/base/screen_still.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/server_live.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

constexpr QLatin1StringView api{"/api2/json"};
// What a ticket is (<password>:PVEVNC:<time>::<signature>, of printable characters) and the VNC password that comes
// with it (eight printable characters, % < @ among them), as the web reads them.
const QRegularExpression& ticketShape()
{
    static const QRegularExpression shape(u"\\A[!-~]{16,1024}\\z"_s);
    return shape;
}
const QRegularExpression& oneTimeShape()
{
    static const QRegularExpression shape(u"\\A[!-~]{1,64}\\z"_s);
    return shape;
}

base::Error guestStopped()
{
    return {"no_picture", "The guest is stopped"};
}

// What a console that does not let this program in is said as: nothing to do with the token, and nothing counted
// against it (the web's ProxmoxConsole._ended): its one-time password refused a moment after it was handed out, or a
// console that asks for what this program does not speak.
base::Error consoleWords(const base::Error& why)
{
    if (why.code == "auth_failed" || why.code == "no_password") {
        return {"bad_response", "The guest's console does not let qKVM in: " + why.message};
    }
    return why;
}

// Why the screen cannot be had now that asking again soon does not change: said on the card until the screen would ask
// again (the web's refused_now, the tile's no_screen).
bool standing(const base::Error& why)
{
    return why.code == "bad_response" || why.code == "auth_failed" || why.code == "no_password";
}

// A guest's live that is not to be had: whoever asks is told why a turn of the loop after they hold it.
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

// What has the guest live for all who show it: its driver's one screen, and a live of its own for each who asks.
// Taken away (end): every live of it ends in those words, and none is opened after.
class ProxmoxOwnLive final : public OwnLive {
public:
    explicit ProxmoxOwnLive(ProxmoxDevice* driver) : m_driver(driver) {}

    std::unique_ptr<Live> open(Live::Need need) override
    {
        if (m_ended || m_driver == nullptr) {
            return std::make_unique<RefusingLive>(m_ended ? m_why : guestStopped());
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

    bool keyboardHeld() const override { return !m_ended && m_driver != nullptr && m_driver->keyboardHeld(); }

private:
    QPointer<ProxmoxDevice> m_driver;
    bool m_ended = false;
    base::Error m_why;
};

} // namespace

RfbScreen::Limits ProxmoxDevice::consoleLimits()
{
    RfbScreen::Limits limits;
    // Held this long after its last viewer (the owner's standard, the web's STANDARD_HOLD): every console opened is a
    // task in the node's log, which the guest's owner reads, and a wall looked at in visits a few minutes apart had
    // one written at each visit.
    limits.idleMs = 300'000;
    // The port waits ten seconds for one connection, then nothing answers: the upgrade has eight (UPGRADE_WAIT).
    limits.openingMs = 8000;
    limits.lookPaceMs = 250; // (VncDevice's LOOK_PACE)
    // Asked again after these after one that failed, one after the other (CONSOLE_RETRY: 15 s, a minute, five, a
    // quarter of an hour; the web's half an hour after that is the last rung here, a quarter of an hour).
    limits.retryMs = {15'000, 60'000, 300'000, 900'000};
    // QEMU's VNC server takes keys from the first frame (no INPUT_AFTER, which is obmc-ikvm's).
    limits.inputAfterMs = 0;
    // Proxmox sends the console in binary frames of at most 128 KiB (MOST_FRAME).
    limits.largestMessageBytes = qint64{128} * 1024 + 64;
    return limits;
}

void ProxmoxDevice::rfbWay(std::function<void(base::Result<RfbScreen::WayIn>)> done)
{
    if (m_other) {
        later([done = std::move(done), why = *m_other] { done(why); });
        return;
    }
    if (m_powerOn == std::optional<bool>(false)) {
        // (A guest that is stopped is asked for no console: Proxmox would hand out a ticket all the same, and its task
        // would say that it failed.)
        later([done = std::move(done)] { done(guestStopped()); });
        return;
    }
    // Asked first whether it runs, as it is now; and which virtual machine has the guest's number right before its
    // console is asked for (another one's screen is not shown, nor are keys or text typed into it: they go through no
    // other way than a console opened here). What the first question found is acted on only once that is known.
    Ask state;
    state.path = u"status/current"_s;
    state.vital = false;
    ofTheGuest(state, u"look at this guest"_s, [this, done](const base::Result<QJsonValue>& found) {
        if (!found) {
            done(found.error());
            return;
        }
        const proxmox::Current current = proxmox::currentOf(found.value());
        // (The sign-in that reading went under: its ticket is asked for under that one, or decided again first. None
        // where none was made. Outside review 2, 4.1.)
        auto under = std::make_shared<std::optional<quint64>>();
        const auto read = [under](const QJsonValue& /*config*/, quint64 signIn) { *under = signIn; };
        const auto judged = [this, current, done, under](const base::Result<void>& same) {
            if (!same) {
                done(same.error());
                return;
            }
            if (current.on != std::optional<bool>(true)) {
                if (current.on == std::optional<bool>(false)) {
                    m_powerOn = false;
                    m_beganAt.reset();
                }
                done(base::Error{"no_picture", "The guest is not running"});
                return;
            }
            beganAgain(current);
            Ask ticket;
            ticket.method = "POST"_ba;
            ticket.path = u"vncproxy"_s;
            ticket.body = "websocket=1"_ba;
            ticket.vital = false;
            ticket.doesSomething = true; // (a task in the node's log: never sent twice)
            ticket.decidedUnder = *under;
            ofTheGuest(ticket, u"open its console"_s, [this, done](const base::Result<QJsonValue>& said) {
                if (!said) {
                    done(said.error());
                    return;
                }
                const QJsonObject handed = said.value().toObject();
                const QJsonValue portSaid = handed.value("port"_L1);
                int port = 0;
                if (portSaid.isString() && portSaid.toString().size() <= 5 &&
                    std::ranges::all_of(portSaid.toString(),
                                        [](QChar sign) { return sign.isDigit() && sign < u'\x7f'; })) {
                    port = portSaid.toString().toInt(); // (Proxmox says it as a text)
                } else if (portSaid.isDouble() && portSaid.toDouble() == static_cast<double>(portSaid.toInt())) {
                    port = portSaid.toInt();
                }
                if (port < 1 || port > 65535) {
                    done(base::Error{"bad_response", "Proxmox handed out a console without a port"});
                    return;
                }
                QString oneTime = handed.value("password"_L1).toString();
                const QString ticketSaid = handed.value("ticket"_L1).toString();
                if (!ticketShape().match(ticketSaid).hasMatch() || !oneTimeShape().match(oneTime).hasMatch()) {
                    wipeInPlace(oneTime);
                    done(base::Error{"bad_response", "Proxmox handed out a console without a ticket and its password"});
                    return;
                }
                // For one connection, used at once: a second use gets no answer from Proxmox at all. (Its ticket stands
                // in the address the console's line asks for, held while the line lasts and not written over; its
                // one-time password is written over, a SecretText, and so are the token's copies.) The token
                // in the upgrade's header, as of every question; the ticket in its address, and its password to RFB
                // alone (never the token's secret: SAVED_PASSWORD_TO_RFB is false behind a door).
                RfbScreen::WayIn way;
                way.address = m_where.address;
                way.knownByCertificate = m_where.knownByCertificate;
                way.certificate = m_where.certificate;
                way.authorization = consoleAuthorization();
                way.protocol = "binary"_ba;
                way.path =
                    (QString(api) + guestPath(u"vncwebsocket"_s) + u"?port=%1&vncticket="_s.arg(port)).toLatin1() +
                    QUrl::toPercentEncoding(ticketSaid);
                way.vncAuthentication = true;
                way.password = SecretText::takenFrom(oneTime);
                done(std::move(way));
            });
        };
        sameGuest(false, judged, std::nullopt, read);
    });
}

ServerConsole& ProxmoxDevice::screen()
{
    if (m_screen == nullptr) {
        RfbScreen::Asker asker = [driver = QPointer<ProxmoxDevice>(this)](const RfbScreen::Answer& answer,
                                                                          const RfbScreen::Wanted& wanted) {
            if (driver == nullptr || !wanted()) {
                answer(base::Error{"stopped", "Nobody waits for the guest's screen any more"});
                return;
            }
            driver->rfbWay(answer);
        };
        auto made = std::make_unique<RfbScreen>(
            std::move(asker),
            [this](const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed) {
                shownOnTheScreen(picture, changed);
            },
            m_pace.console);
        const RfbScreen* const console = made.get();
        m_screen = std::move(made);
        connect(m_screen.get(), &ServerConsole::ended, this,
                [this, console](const QString& code, const QString& message) {
                    const base::Error why{code.toStdString(), message.toStdString()};
                    // (Its line that ended before Proxmox let it in, or that fell after that without Proxmox's
                    // goodbye, for nothing there or another certificate, is a failure of contact as a question's is:
                    // review cx3-2, L1. Not a line let in and ended otherwise: Proxmox's goodbye (its own end of the
                    // console of a guest that stops), what it said, this program's own limits; nor a way in that
                    // failed, whose questions were counted as they failed. Review cx3-3, L3. Once for each session
                    // that ends, and not at each still asked of one that failed.)
                    if (const RfbScreen::Ended ended = console->lastEnded();
                        ended == RfbScreen::Ended::NotLetIn || ended == RfbScreen::Ended::Fell) {
                        outOfTouch(why);
                    }
                    notedOnTheScreen(why);
                });
    }
    return *m_screen;
}

void ProxmoxDevice::notedOnTheScreen(const base::Error& why)
{
    if (standing(why)) {
        m_noScreen = consoleWords(why);
        m_noScreenUntil = now() + m_pace.console.retryMs.front();
    }
}

void ProxmoxDevice::shownOnTheScreen(const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed)
{
    // The screen shows again: nothing stands in its way.
    m_noScreen.reset();
    ++m_framesShown;
    m_lives.removeIf([](const QPointer<ServerLive>& live) { return live == nullptr; });
    for (const QPointer<ServerLive>& live : std::as_const(m_lives)) {
        live->shown(picture, changed);
    }
}

std::optional<base::Error> ProxmoxDevice::noScreen() const
{
    if (m_noScreen && now() < m_noScreenUntil) {
        return m_noScreen;
    }
    return std::nullopt;
}

bool ProxmoxDevice::hasScreen() const
{
    // As the web's: while it runs, and nothing stands in its way for now; never of another virtual machine.
    return m_powerOn == std::optional<bool>(true) && !noScreen() && !m_other;
}

std::optional<base::Error> ProxmoxDevice::screenAllowed() const
{
    // (The web's _screen_allowed: the last look decides.)
    if (m_other) {
        return m_other;
    }
    if (!m_status) {
        return base::Error{"no_stream", "Looking at the guest"};
    }
    if (m_status->state != "online"_L1 && m_status->state != "no_signal"_L1) {
        return base::Error{m_status->errorCode.isEmpty() ? std::string("unreachable")
                                                         : m_status->errorCode.toStdString(),
                           m_status->errorMessage.isEmpty() ? std::string("Device is unreachable")
                                                            : m_status->errorMessage.toStdString()};
    }
    if (m_powerOn == std::optional<bool>(false)) {
        return guestStopped();
    }
    return std::nullopt;
}

void ProxmoxDevice::askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done)
{
    if (const std::optional<base::Error> why = screenAllowed()) {
        later([done = std::move(done), why = *why] { done(why); });
        return;
    }
    const base::Result<void> looked = screen().look();
    if (!looked) {
        notedOnTheScreen(looked.error());
        later([done = std::move(done), why = consoleWords(looked.error())] { done(why); });
        return;
    }
    // (Made once for each frame and width, as a server's: a card that asks again before the screen changed is handed
    // the same.)
    const int tier = base::screen::stillWidthFor(width, m_screen->picture().width);
    if (m_still && m_still->frames == m_framesShown && m_still->tier == tier) {
        later([done = std::move(done), still = Snapshot{m_still->picture}] { done(still); });
        return;
    }
    m_screen->still(width, [here = QPointer<ProxmoxDevice>(this), frames = m_framesShown, tier,
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
                done(consoleWords(made.error()));
            }
        });
    });
}

std::unique_ptr<OwnLive> ProxmoxDevice::makeLive()
{
    return std::make_unique<ProxmoxOwnLive>(this);
}

std::unique_ptr<Live> ProxmoxDevice::openLive(Live::Need need)
{
    if (const std::optional<base::Error> why = screenAllowed(); why && why->code != "no_stream") {
        return std::make_unique<RefusingLive>(*why);
    }
    auto live = std::make_unique<ServerLive>(&screen(), m_pace.live);
    live->setNeed(need);
    // (Asked of this driver as it is then: one let go of meanwhile refuses nothing, its lives ended with it.)
    live->refuseControlWhile([driver = QPointer<ProxmoxDevice>(this)]() -> std::optional<base::Error> {
        return driver != nullptr && driver->m_actedOn ? driver->m_actedOn() : std::nullopt;
    });
    m_lives.append(live.get());
    return live;
}

bool ProxmoxDevice::keyboardHeld() const
{
    return std::ranges::any_of(m_lives,
                               [](const QPointer<ServerLive>& live) { return live != nullptr && live->hasControl(); });
}

void ProxmoxDevice::refuseControlWhile(ActedOn actedOn)
{
    m_actedOn = std::move(actedOn);
}

void ProxmoxDevice::endLives(const base::Error& why)
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

void ProxmoxDevice::typeOnTheScreen(const Action& what, std::function<void(base::Result<Done>)> done)
{
    // Nothing goes to another virtual machine under its number, nor to a guest that is stopped.
    if (const std::optional<base::Error> why = screenAllowed(); why && why->code != "no_stream") {
        later([done = std::move(done), why = *why] { done(why); });
        return;
    }
    // The keyboard's Power key is never sent, whatever else is pressed with it (as on every machine of this wall: a5).
    if (what.kind == "keys"_L1 && kvmd::namesThePowerKey(what.keys)) {
        later([done = std::move(done)] { done(base::Error{"invalid", kvmd::powerKeyRefused}); });
        return;
    }
    const QPointer<ProxmoxDevice> here(this);
    const ServerConsole::Still still = [here] { return here != nullptr && !here->m_other; };
    const auto said = [done](const base::Result<void>& how) {
        if (!how) {
            done(consoleWords(how.error()));
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
