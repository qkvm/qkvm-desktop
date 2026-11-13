#include <qkvm/core/server_screen.h>

#include <algorithm>
#include <atomic>
// Built with the standard library's own checks in every build (core/CMakeLists.txt): a build without them is refused
// here, and never loses them without a word.
#if !_MSVC_STL_HARDENING_ARRAY || !_MSVC_STL_HARDENING_SPAN || !_MSVC_STL_HARDENING_VECTOR
#error "This reads what a management controller sends, and is built with _MSVC_STL_HARDENING (core/CMakeLists.txt)"
#endif
#include <cmath>
#include <deque>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QPointer>
#include <QThread>

#include <qkvm/core/device_line.h>
#include <qkvm/core/tls.h>

#include "screen_reader.h"

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace ami = bmc::ami;
namespace aspeed = bmc::aspeed;

namespace {

// Who is asking for the console, as the controller names them: more than this is not a name, and is not said back.
constexpr qsizetype mostOfWho = 512;
// The console pass: what may travel in a message of the console's own (the web's CONSOLE_PASS).
constexpr qsizetype longestPass = 128;
// What may arrive in one go: a frame's piece at the most, and its message's head and WebSocket's around it.
constexpr qint64 largestMessage = static_cast<qint64>(ami::largestArrival);
// What a console may send in a second, and in how many messages: a busy screen, in pieces of any size.
constexpr qint64 mostBytesASecond = 4 * static_cast<qint64>(aspeed::longest);
constexpr int mostMessagesASecond = 20000;
// Frames that may wait to be read (the one being read and the next): past that, nothing more is taken off the line.
constexpr int framesAhead = 2;

bool fitPass(const QByteArray& pass)
{
    return !pass.isEmpty() && pass.size() <= longestPass &&
           std::ranges::all_of(pass, [](char one) { return one >= '!' && one <= '~'; });
}

// What the console's answer to the pass says, where it is no.
QString passRefused(int answer)
{
    switch (answer) {
    case 0:
        return u"its console did not accept the pass it had given out"_s;
    case 2:
        return u"this user is not allowed to use its console"_s;
    case 3:
        return u"its console did not accept what this program said of itself"_s;
    case 8:
        return u"its console does not know the session any more"_s;
    default:
        return u"its console answered %1"_s.arg(answer);
    }
}

// Why a line ended by itself, in the driver's codes and words.
std::pair<QString, QString> saidOf(DeviceLine::End why)
{
    switch (why) {
    case DeviceLine::End::NotAsked:
        // (What the controller handed out is not fit to be sent: that is not changed by asking again soon.)
        return {u"no_picture"_s, u"The way into the server's console is not fit to be sent"_s};
    case DeviceLine::End::TlsDamaged:
        return {QString::fromLatin1(tlsDamaged), QString::fromLatin1(tlsDamagedSentence)};
    case DeviceLine::End::CertificateUnread:
        return {u"cert_invalid"_s,
                u"The certificate saved for this device cannot be read. Look at its certificate and trust it again."_s};
    case DeviceLine::End::CertificateChanged:
        return {u"cert_mismatch"_s, u"Certificate does not match the one saved for this device"_s};
    case DeviceLine::End::CertificateUntrusted:
        return {u"cert_invalid"_s, u"The device's certificate is not trusted"_s};
    case DeviceLine::End::CertificateOfUnknownSigner:
        return {u"cert_invalid"_s, u"The device's certificate was signed by a maker of certificates that Windows does "
                                   "not know. Have Windows trust that maker, and the device is let in by that."_s};
    case DeviceLine::End::Silent:
        return {u"timeout"_s, u"The controller's console did not answer in time"_s};
    case DeviceLine::End::NotReached:
    case DeviceLine::End::SignedOut:
    case DeviceLine::End::NotLetIn:
        return {u"dropped"_s, u"The controller's console could not be opened"_s};
    case DeviceLine::End::Fell:
        return {u"dropped"_s, u"The controller closed its console"_s};
    case DeviceLine::End::Misbehaved:
        return {u"bad_response"_s, u"The controller's console sent what this program does not accept"_s};
    case DeviceLine::End::TakesNothing:
    case DeviceLine::End::Late:
        return {u"dropped"_s, u"The controller's console stopped taking what is said to it"_s};
    }
    return {u"dropped"_s, u"The controller closed its console"_s};
}

base::Error error(const QString& code, const QString& message)
{
    return {code.toStdString(), message.toStdString()};
}

// Turns of the console's keyboard and mouse, numbered for the whole program: a turn of one screen's is no other's.
std::atomic<int> turns{0};

int aNewTurn() noexcept
{
    return turns.fetch_add(1, std::memory_order_relaxed) + 1;
}

// Whether a keyboard report only lets go: of a key or a modifier, and presses none.
bool onlyLetsGo(const typed::Report& before, const typed::Report& after) noexcept
{
    if ((after.modifiers & ~before.modifiers) != 0) {
        return false;
    }
    for (const std::uint8_t key : after.keys) {
        if (std::ranges::find(before.keys, key) == before.keys.end()) {
            return false;
        }
    }
    return after.modifiers != before.modifiers || after.keys.size() < before.keys.size();
}

// What a turn of the console's keyboard and mouse that ended is told, by why it ended.
base::Error handedOver()
{
    return {"in_use", "Somebody at the server's own console has its keyboard and mouse now"};
}

base::Error consoleClosed()
{
    return {"dropped", "The server's console was closed"};
}

QByteArray bytesOf(const ami::Bytes& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}

} // namespace

// The seat of one controller's console, as oneSeatAtATime and ConsoleSeats hand it out: whether a way into it is out
// (asked for, or handed out and not done yet), and who waits for one, each with the asker of its own driver.
struct ConsoleSeats::Seat {
    struct Waiting {
        ServerScreen::Asker asker;
        ServerScreen::Answer answer;
        ServerScreen::Wanted wanted;
    };
    bool out = false;
    std::deque<Waiting> waiting;
};

// One console session: its line, and what was said on it so far.
struct ServerScreen::Session : QObject {
    Session(DeviceLine::Where where, DeviceLine::Limits limits) : line(std::move(where), limits) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Gone, its line with it: the way in it was made of is given back (WayIn::done).
    ~Session() override
    {
        if (done) {
            done();
        }
    }

    std::function<void()> done;
    DeviceLine line;
    std::uint64_t number = 0; // (the frames' session, ScreenReader's)
    ami::Messages arriving;
    ami::Frames frames;
    int waiting = 0;       // frames handed over to be read, and not taken back yet
    bool greeted = false;  // the pass was said
    bool accepted = false; // ...and taken
    bool readAny = false;  // a frame was read
    bool mine = false;     // "the console is yours", and given to nobody since
    bool over = false;
    QByteArray who;   // whoever else is at the console, as the controller last named them
    int letIn = 0;    // times somebody asking for the console was let in
    int unread = 0;   // frames in a row that could not be read
    int shown = 0;    // frames that showed the screen
    qint64 since = 0; // when the first of them came
    qint64 heardAt = 0;
    qint64 saidAt = 0;
    QByteArray webSession;
    // Keys and the pointer: when the pass was taken (-1: not yet), from when keys may go (a first session's
    // keyboard is attached only a while after), whether the console was given to somebody at the controller's own
    // page, which mouse the server shows, and what this program has down there as far as it has said.
    qint64 acceptedAt = -1;
    qint64 keysFrom = 0;
    bool handedOver = false;
    bool relative = false;
    std::uint32_t reports = 0;
    typed::Report keysThere; // the last keyboard report said
    std::uint8_t buttonsDown = 0;

    bool keysDown() const noexcept { return keysThere.modifiers != 0 || !keysThere.keys.empty(); }
    std::pair<int, int> place{ami::farthest / 2, ami::farthest / 2};
};

// A typing: its reports, how long each is held, and how far it has got.
struct ServerScreen::Typing {
    std::uint64_t number = 0; // (what its asking for the keyboard finds it by)
    std::vector<typed::Report> reports;
    int heldMs = 0;
    Still still;
    Typed done;
    bool asked = false;   // its keyboard was asked for
    int turn = -1;        // ...and given, in this turn
    std::size_t next = 0; // the report that goes next
    bool down = false;    // ...is down, and not let go of yet
};

namespace {

using Seat = ConsoleSeats::Seat;

void next(const std::shared_ptr<Seat>& seat);

// The seat is free again: the next who waits is asked for at the next turn of the loop, and not from inside the
// answer or the ending of a session (whoever answers is not asked again from inside its own answer).
void freed(const std::weak_ptr<Seat>& kept)
{
    QTimer::singleShot(0, [kept] {
        if (const std::shared_ptr<Seat> seat = kept.lock()) {
            seat->out = false;
            next(seat);
        }
    });
}

// Asks for the next way in, where nobody holds the seat and somebody waits for it.
void next(const std::shared_ptr<Seat>& seat)
{
    // Whoever no longer waits (a screen closed, or thrown away, while it waited for the seat) is let go of: nothing is
    // asked of the controller for it, no sign-in, no pass.
    while (!seat->out && !seat->waiting.empty() && seat->waiting.front().wanted && !seat->waiting.front().wanted()) {
        seat->waiting.pop_front();
    }
    if (seat->out || seat->waiting.empty()) {
        return;
    }
    auto [asker, answer, wanted] = std::move(seat->waiting.front());
    seat->waiting.pop_front();
    seat->out = true;
    asker(
        [kept = std::weak_ptr<Seat>(seat), answer = std::move(answer)](base::Result<ServerScreen::WayIn> way) {
            if (!way) {
                freed(kept); // (no way, and so no seat taken)
                answer(std::move(way));
                return;
            }
            ServerScreen::WayIn given = std::move(way).value();
            // Free when the session it was for is over and its line has gone: then the next is asked for.
            given.done = [kept, done = std::move(given.done)] {
                if (done) {
                    done();
                }
                freed(kept);
            };
            answer(std::move(given));
        },
        std::move(wanted));
}

} // namespace

ServerScreen::Asker ServerScreen::oneSeatAtATime(Asker asker)
{
    auto seat = std::make_shared<Seat>();
    return [seat, asker = std::move(asker)](Answer answer, Wanted wanted) {
        seat->waiting.push_back({asker, std::move(answer), std::move(wanted)});
        next(seat);
    };
}

// --- the seats of the program's controllers -----------------------------------------------------------------------

ConsoleSeats::ConsoleSeats(qsizetype most) : m_most(std::max<qsizetype>(1, most)) {}

ConsoleSeats::~ConsoleSeats() = default;

const std::shared_ptr<ConsoleSeats>& ConsoleSeats::program()
{
    static const std::shared_ptr<ConsoleSeats> kept = std::make_shared<ConsoleSeats>();
    return kept;
}

namespace {

bool taken(const Seat& seat)
{
    return seat.out || !seat.waiting.empty();
}

} // namespace

void ConsoleSeats::makeRoom()
{
    if (m_kept.size() < m_most) {
        return;
    }
    m_kept.removeIf([](const auto& kept) { return !taken(*kept.value()); });
}

std::shared_ptr<Seat> ConsoleSeats::seatOf(const Of& of)
{
    const QString address = u"a "_s + of.address.toString(QUrl::FullyEncoded);
    const QString mark = of.controller.isEmpty() ? QString() : u"c "_s + of.controller;
    std::shared_ptr<Seat> mine = mark.isEmpty() ? nullptr : m_kept.value(mark);
    const std::shared_ptr<Seat> there = m_kept.value(address);
    // Found under its address alone, or the controller's number at an address another seat was kept under: one
    // controller, and the seat that is held or waited for is its seat. (Both held: each goes on with what it has, its
    // own way out freeing it alone, and the next asking finds the one kept under both keys.)
    if (there != nullptr && (mine == nullptr || (there != mine && !taken(*mine) && taken(*there)))) {
        mine = there;
    } else if (mine == nullptr) {
        makeRoom();
        mine = std::make_shared<Seat>();
    }
    if (!mark.isEmpty()) {
        m_kept.insert(mark, mine);
    }
    m_kept.insert(address, mine);
    return mine;
}

ServerScreen::Asker ConsoleSeats::asker(std::function<std::optional<Of>()> of, ServerScreen::Asker asker)
{
    return [seats = shared_from_this(), of = std::move(of), asker = std::move(asker)](ServerScreen::Answer answer,
                                                                                      ServerScreen::Wanted wanted) {
        const std::optional<Of> where = of();
        if (!where) {
            answer(base::Error{"stopped", "Nobody waits for the server's screen any more"});
            return;
        }
        const std::shared_ptr<Seat> seat = seats->seatOf(*where);
        seat->waiting.push_back({asker, std::move(answer), std::move(wanted)});
        next(seat);
    };
}

ServerScreen::ServerScreen(Asker asker, Shown shown, Limits limits, QObject* parent)
    : QObject(parent), m_asker(std::move(asker)), m_shown(std::move(shown)), m_limits(limits),
      m_reader(std::make_shared<ScreenReader>(this, [this] { takeReads(); }, limits.mostResizesASecond))
{
    m_clock.start();
    m_turn = aNewTurn();
    connect(&m_ticks, &QTimer::timeout, this, [this] { tick(); });
    connect(&m_inputTicks, &QTimer::timeout, this, [this] { inputTick(); });
    m_typingSteps.setSingleShot(true);
    m_typingSteps.setTimerType(Qt::PreciseTimer);
    connect(&m_typingSteps, &QTimer::timeout, this, [this] { typingStep(); });
}

ServerScreen::~ServerScreen()
{
    m_closed = true;
    // (Its thread lets go of the frame under way within 64 blocks; nothing of it is told to this any more.)
    m_reader->detach();
    leave();
}

base::Result<void> ServerScreen::look()
{
    if (m_closed) {
        return error(u"no_picture"_s, u"This server's screen is not looked at any more"_s);
    }
    m_wantedAt = now();
    if (m_asking) {
        return error(u"no_stream"_s, u"Opening the server's console"_s);
    }
    if (m_session != nullptr) {
        if (m_session->shown > 0) {
            return {};
        }
        return error(u"no_stream"_s, u"Waiting for the server's screen"_s);
    }
    // What the last session came to stands until the next may be asked for, while its line goes too.
    if (m_refusal && now() < m_retryAt) {
        return *m_refusal;
    }
    if (m_leaving != nullptr) {
        // The last session's line has not gone yet: its seat is not free, and somebody who opens the console now
        // would be told that every seat is taken.
        return error(u"no_stream"_s, u"Giving the server's console back"_s);
    }
    open();
    return error(u"no_stream"_s, u"Opening the server's console"_s);
}

const aspeed::Picture& ServerScreen::picture() const noexcept
{
    return m_picture;
}

int ServerScreen::framesWaiting() const noexcept
{
    return m_session != nullptr ? m_session->waiting : 0;
}

bool ServerScreen::holdsTheConsole() const noexcept
{
    return m_session != nullptr && m_session->mine;
}

void ServerScreen::close()
{
    m_closed = true;
    m_asking = false; // (an answer that comes after this finds nobody waiting for it, and is given back at once)
    leave();
    m_picture = {};
    m_greeting.clear();
    m_ticks.stop();
    // Whoever waits for keys, or for a typing, is told: nothing more goes.
    m_inputTicks.stop();
    m_typingSteps.stop();
    m_owed.reset();
    std::vector<Asking> askings = std::exchange(m_askings, {});
    std::deque<std::unique_ptr<Typing>> typings = std::exchange(m_typings, {});
    const QPointer<ServerScreen> self(this);
    stillsUnmade(consoleClosed());
    if (self == nullptr) {
        return;
    }
    for (Asking& asking : askings) {
        asking.answer(consoleClosed());
        if (self == nullptr) {
            return;
        }
    }
    for (const std::unique_ptr<Typing>& typing : typings) {
        typing->done(consoleClosed());
        if (self == nullptr) {
            return;
        }
    }
}

void ServerScreen::open()
{
    m_asking = true;
    // Answered now or later, on this thread; an answer that is no longer waited for finds nothing.
    const QPointer<ServerScreen> self(this);
    m_asker(
        [self](base::Result<WayIn> way) {
            if (self == nullptr || !self->m_asking) {
                // Nobody waits for it any more (the screen was closed, or is gone): its web session is given back at
                // once.
                if (way && way.value().done) {
                    way.value().done();
                }
                return;
            }
            self->m_asking = false;
            if (!way) {
                self->finish(QString::fromStdString(way.error().code), QString::fromStdString(way.error().message));
                return;
            }
            self->entered(std::move(way).value());
        },
        [self] { return self != nullptr && self->m_asking; });
}

void ServerScreen::entered(WayIn way)
{
    if (!fitPass(way.pass)) {
        if (way.done) {
            way.done(); // (no line was opened with it)
        }
        finish(u"no_picture"_s, u"The controller gave no pass for its console"_s);
        return;
    }
    if (!m_completeFirst) {
        m_completeFirst = way.completeFirst.value_or(false);
    }
    DeviceLine::Where where;
    where.address = way.address;
    where.asking = "/kvm"_ba;
    where.cookie = way.cookie;
    where.knownByCertificate = way.knownByCertificate;
    where.certificate = way.certificate;
    DeviceLine::Limits limits;
    limits.letInWithinMs = m_limits.openingMs;
    limits.lateAfterMs = m_limits.sayMs;
    limits.goodbyeWithinMs = m_limits.goodbyeWithinMs;
    limits.largestFrameBytes = largestMessage;
    limits.mostBytesASecond = mostBytesASecond;
    limits.mostMessagesASecond = mostMessagesASecond;
    limits.mostUnsentBytes = m_limits.mostWaitingBytes;
    m_session = std::make_unique<Session>(where, limits);
    m_picture = {};
    Session& session = *m_session;
    session.number = ++m_sessions;
    session.done = std::move(way.done);
    session.heardAt = session.saidAt = now();
    session.webSession = way.webSession;
    // (Kept for the greeting: the pass and the two addresses, as the controller's own page says them.)
    const ami::Bytes greeting = ami::hello(way.pass.toStdString(), way.ownAddress.toStdString(),
                                           way.controllerAddress.toStdString(), ami::defaultName, *m_completeFirst);
    session.line.receiveBytes("binary"_ba);
    connect(&session.line, &DeviceLine::bytesCame, this, [this](const QByteArray& bytes) { heard(bytes); });
    connect(&session.line, &DeviceLine::ended, this, [this](DeviceLine::End why) { lineEnded(why); });
    m_greeting = QByteArray(reinterpret_cast<const char*>(greeting.data()), static_cast<qsizetype>(greeting.size()));
    session.line.open();
    m_ticks.start(m_limits.tickMs);
}

void ServerScreen::heard(const QByteArray& bytes)
{
    if (m_session == nullptr) {
        return;
    }
    // What is done with a message may end this session, open another (whoever hears `ended` may look again), or
    // throw this object away (whoever hears `ended`, or is shown the screen): looked at after every one.
    const QPointer<ServerScreen> self(this);
    Session* const session = m_session.get();
    m_session->heardAt = now();
    const auto messages = m_session->arriving.feed(
        {reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())});
    if (!messages) {
        finish(u"bad_response"_s,
               u"The controller's console sent "_s + QString::fromStdString(messages.error().message));
        return;
    }
    for (const ami::Message& one : messages.value()) {
        take(one);
        if (self == nullptr || m_session.get() != session) {
            return; // (it ended at that one)
        }
    }
    if (session->waiting >= framesAhead) {
        session->line.holdBack(true); // (let go when what was read is taken back: takeReads)
    }
    behind();
}

void ServerScreen::take(const ami::Message& one)
{
    Session& session = *m_session;
    switch (one.kind) {
    case ami::kind::allowed:
        if (say({reinterpret_cast<const std::uint8_t*>(m_greeting.constData()),
                 static_cast<std::size_t>(m_greeting.size())})) {
            session.greeted = true;
        }
        return;
    case ami::kind::passSeen: {
        const int answer = one.body.empty() ? 0 : one.body.front();
        if (answer != 1) {
            finish(u"no_picture"_s, u"The controller refused: "_s + passRefused(answer));
            return;
        }
        if (session.accepted) {
            // Said a second time in one session, which a controller does not: what it means is not known.
            finish(u"bad_response"_s, u"The server's console said its pass was taken a second time"_s);
            return;
        }
        session.accepted = true;
        // A session of this program's, and nothing down in it yet; its reports are numbered from one. Keys wait for
        // "the console is yours" (sharing), and in a first session for its keyboard to be attached.
        session.acceptedAt = now();
        session.keysFrom = now() + (one.status == ami::firstSession ? m_limits.keysSettleMs : 0);
        return;
    }
    case ami::kind::mouseMode:
        if (!one.body.empty() &&
            (one.body.front() == ami::mouse::absolute || one.body.front() == ami::mouse::relative)) {
            session.relative = one.body.front() == ami::mouse::relative;
            if (session.relative && session.mine && session.buttonsDown != 0) {
                // A button held as the server's mouse became a relative one is let go of in the form it takes now
                // (as the web's lets go of buttons where it has no place for them).
                session.buttonsDown = 0;
                say(ami::mouseRelative(0, 0, 0, 0, ++session.reports));
            }
        }
        return;
    case ami::kind::stillThere:
        say(ami::message(ami::kind::stillThere));
        return;
    case ami::kind::others:
        say(ami::message(ami::kind::fullScreen, 1));
        return;
    case ami::kind::licence:
        say(ami::answerToLicence(session.webSession.toStdString()));
        return;
    case ami::kind::tooMany:
        finish(u"in_use"_s, u"Every console seat of this controller is taken"_s);
        return;
    case ami::kind::sharing:
    case ami::kind::nextHolder:
        sharing(one);
        return;
    case ami::kind::stop:
    case ami::kind::refused: {
        const bool timeUp = one.kind == ami::kind::stop && one.status == ami::timeUp;
        const std::string_view words = ami::endedBecause(one.status);
        const QString why = one.kind == ami::kind::refused ? u"refused, its reason: %1"_s.arg(one.status)
                            : words.empty()                ? u"its reason: %1"_s.arg(one.status)
                                            : QString::fromLatin1(words.data(), static_cast<qsizetype>(words.size()));
        finish(timeUp ? u"console_time_up"_s : u"dropped"_s,
               u"The controller ended the console session (%1)"_s.arg(why));
        return;
    }
    case ami::kind::video:
        video(one.body);
        return;
    default:
        return;
    }
}

void ServerScreen::sharing(const ami::Message& one)
{
    Session& session = *m_session;
    const unsigned about = one.status & 0xFF;
    const unsigned answer = one.status >> 8;
    if (!one.body.empty() && static_cast<qsizetype>(one.body.size()) <= mostOfWho) {
        session.who =
            QByteArray(reinterpret_cast<const char*>(one.body.data()), static_cast<qsizetype>(one.body.size()));
    }
    if (about == ami::sharing::becameMaster && one.kind == ami::kind::nextHolder && answer == ami::answer::letIn &&
        session.accepted && !session.mine) {
        // "The console is yours" (a lone session is greeted so, and one that gets it back): a new turn of its keyboard
        // and mouse (the one after the turn that ended when the console went, or when the last session ended).
        // Whoever held them in an earlier one takes them again, from a keyboard with nothing down. (Said again while it
        // is this program's already, it changes nothing: a new turn then would leave what is down with nobody to let
        // go of it.)
        session.mine = true;
        session.handedOver = false;
        m_lost = {"dropped", "The server's console changed hands: take its keyboard again"};
        if (!m_askings.empty()) {
            m_inputTicks.start(0); // (answered from the loop, not from the middle of what the console says)
        }
    } else if (about == ami::sharing::becameMaster && one.kind == ami::kind::sharing && session.mine) {
        // It was switched to another session: what this program has down is let go of at once, and nothing more of
        // its goes there.
        if (!giveUpInput()) {
            return;
        }
    }
    if (about == ami::sharing::asked && !session.who.isEmpty() && session.letIn < m_limits.letInAtMost) {
        // Somebody opened the controller's own console while this program has it, and it is asked whether they may.
        // Nobody sits here to answer, and unanswered they would wait half a minute: they have it at once, keys and
        // all, and the screen goes on being looked at. (A person asks once; a controller that asks without end is
        // not answered without end.) What this program has down there is let go of first, and its turn is over.
        ++session.letIn;
        const ami::Bytes who(session.who.begin(), session.who.end());
        if (giveUpInput()) {
            say(ami::letIn(who));
        }
        return;
    }
    if (about == ami::sharing::waiting ||
        (about == ami::sharing::answered && (answer == ami::answer::turnedAway || answer == ami::answer::shutOut))) {
        // Somebody had the console first, and is being asked whether this program may look: a question on the
        // screen of somebody at work, and unanswered the console would pass to the wall. It leaves at once, and
        // looks again much later.
        const auto minutes = std::max<qint64>(1, std::llround(m_limits.heldMs / 60000.0));
        const QString again = minutes == 1 ? u"in a minute"_s : u"in %1 minutes"_s.arg(minutes);
        finish(u"console_held"_s,
               u"Somebody is using this server's console. The wall keeps out of their way, and looks again %1"_s.arg(
                   again));
    }
}

void ServerScreen::video(const ami::Bytes& body)
{
    Session& session = *m_session;
    // (A line brings what it has in one go, frames by the thousand; between two of them nobody may look any more.)
    if (now() - m_wantedAt > m_limits.idleMs) {
        finish({}, u"Nobody looks at the server's screen: its console was given back"_s);
        return;
    }
    auto whole = session.frames.piece(body);
    if (!whole) {
        session.frames = ami::Frames();
        unreadable(QString::fromStdString(whole.error().message));
        return;
    }
    std::optional<ami::WholeFrame>& frame = whole.value();
    if (!frame) {
        return;
    }
    // Read on a thread of its own, after the ones before it, none thrown away unread; what it came to is taken back a
    // turn of this loop later (takeReads). While two wait, nothing more is taken off the line (heard).
    ++session.waiting;
    m_reader->read(session.number, std::move(*frame));
}

void ServerScreen::takeReads()
{
    Session* const session = m_session.get();
    ScreenReader::Taken taken = m_reader->take(session != nullptr ? session->number : 0, m_picture);
    // What is told may do anything (close this, throw it away): looked at after each.
    const QPointer<ServerScreen> self(this);
    for (ScreenReader::Still& made : taken.stills) {
        const auto asked = std::ranges::find(m_stillsAsked, made.number, &StillAsked::number);
        if (asked == m_stillsAsked.end()) {
            continue;
        }
        const StillAnswer done = std::move(asked->done);
        m_stillsAsked.erase(asked);
        if (made.picture.isEmpty()) {
            done(error(u"no_stream"_s, u"The server's screen is not shown yet"_s));
        } else {
            done(std::move(made.picture));
        }
        if (self == nullptr || m_session.get() != session) {
            return;
        }
    }
    if (session == nullptr || taken.reads.empty()) {
        return;
    }
    session->waiting = std::max(0, session->waiting - static_cast<int>(taken.reads.size()));
    // (Nobody looking any more is seen by the console's tick, the loop turning between two takings; and no frame is
    // handed over after it, video().)
    bool show = false;
    std::optional<std::pair<QString, QString>> ending;
    for (const ScreenReader::Read& read : taken.reads) {
        if (read.resizedTooOften) {
            // (A screen that changes its size too often is not read on: what counts is a picture made anew.)
            ending = {u"no_picture"_s, u"The controller's screen changes its size more often than a server's does"_s};
            break;
        }
        if (!read.changed) {
            // What it drew before it was refused is on the picture.
            show = show || (session->shown > 0 && !taken.changed.empty());
            if (++session->unread >= m_limits.unreadable) {
                ending = {u"no_picture"_s, u"The controller sends a picture this program does not read yet (%1)"_s.arg(
                                               QString::fromStdString(read.changed.error().message))};
                break;
            }
            continue;
        }
        session->unread = 0;
        session->readAny = true;
        if (read.changed.value() == 0 && session->shown == 0) {
            // A console opened again begins with a frame that says "nothing changed"; the whole screen follows when
            // asked for. Until it does there is no picture, and a black one is not shown in its place.
            continue;
        }
        if (++session->shown == 1) {
            session->since = now();
        }
        show = true;
    }
    if (show) {
        m_shown(m_picture, taken.changed);
        if (self == nullptr || m_session.get() != session) {
            return; // (whoever was shown it closed the screen, or threw it away)
        }
    }
    if (ending) {
        finish(ending->first, ending->second);
        return;
    }
    if (session->waiting < framesAhead) {
        session->line.holdBack(false);
    }
    behind();
}

void ServerScreen::unreadable(const QString& why)
{
    Session& session = *m_session;
    if (++session.unread >= m_limits.unreadable) {
        finish(u"no_picture"_s, u"The controller sends a picture this program does not read yet (%1)"_s.arg(why));
    }
}

void ServerScreen::still(int width, StillAnswer done)
{
    if (m_closed || m_session == nullptr || m_session->shown == 0) {
        // NOLINTNEXTLINE(bugprone-exception-escape)
        QTimer::singleShot(0, this, [done = std::move(done)] {
            done(error(u"no_stream"_s, u"The server's screen is not shown yet"_s));
        });
        return;
    }
    const std::uint64_t number = ++m_stillsMade;
    m_stillsAsked.push_back({number, std::move(done)});
    m_reader->still(m_session->number, width, number);
}

void ServerScreen::stillsUnmade(const base::Error& why)
{
    const std::vector<StillAsked> asked = std::exchange(m_stillsAsked, {});
    for (const StillAsked& one : asked) {
        one.done(why); // (each asker holds what it needs: this may be gone after any of them)
    }
}

void ServerScreen::behind()
{
    if (m_session != nullptr && m_session->readAny && now() - m_session->saidAt > m_limits.behindMs) {
        m_session->saidAt = now();
        say(ami::message(ami::kind::behind)); // "every frame so far has been drawn"
    }
}

void ServerScreen::tick()
{
    if (m_session == nullptr) {
        m_ticks.stop();
        return;
    }
    const qint64 at = now();
    if (at - m_wantedAt > m_limits.idleMs) {
        finish({}, u"Nobody looks at the server's screen: its console was given back"_s);
        return;
    }
    if (at - m_session->heardAt > m_limits.quietMs) {
        finish(u"dropped"_s, u"The controller's console went quiet"_s);
        return;
    }
    behind();
}

bool ServerScreen::say(std::span<const std::uint8_t> what)
{
    if (m_session == nullptr) {
        return false;
    }
    const QPointer<ServerScreen> self(this);
    Session* const session = m_session.get();
    if (session->line.sayBytes(
            QByteArray(reinterpret_cast<const char*>(what.data()), static_cast<qsizetype>(what.size())))) {
        return true;
    }
    // (Where the line ended in the saying, that was said already, and may have thrown this away.)
    if (self != nullptr && m_session.get() == session) {
        finish(u"dropped"_s, u"The controller's console stopped taking what is said to it"_s);
    }
    return false;
}

void ServerScreen::lineEnded(DeviceLine::End why)
{
    if (m_session == nullptr) {
        return;
    }
    if (why == DeviceLine::End::Fell && m_session->greeted && !m_session->accepted && m_completeFirst) {
        // It closed on the greeting, without a word about the pass: it wants to be greeted the other way.
        m_completeFirst = !*m_completeFirst;
    }
    const auto [code, message] = saidOf(why);
    m_session->over = true; // (the line is over by itself: nothing more is said on it)
    finish(code, message);
}

void ServerScreen::leave()
{
    if (m_session == nullptr) {
        return;
    }
    std::unique_ptr<Session> session = std::move(m_session);
    disconnect(&session->line, nullptr, this, nullptr);
    // Nothing more of its frames is read, the one under way is given up, and its pictures go.
    m_reader->end(session->number);
    endTurn(consoleClosed());
    if (!session->over && session->line.isIn()) {
        // What this program has down on the server (a typing cut off as the screen is closed) is let go of first.
        // (Down only while the console is this session's: what was down when it went to somebody else was let go of
        // then, and into a console that is somebody else's nothing goes.)
        if (session->keysDown()) {
            session->line.sayBytes(bytesOf(ami::keyboard(0, {}, ++session->reports)));
        }
        if (session->buttonsDown != 0) {
            session->line.sayBytes(
                bytesOf(ami::mouseAbsolute(0, session->place.first, session->place.second, 0, ++session->reports)));
        }
        // Said as the controller's own page says it when its window is closed: a console that just goes away is
        // waited for by the controller, which is then slow to answer anybody. Said from the greeting on.
        if (session->greeted) {
            const ami::Bytes bye = ami::goodbye();
            session->line.sayBytes(
                QByteArray(reinterpret_cast<const char*>(bye.data()), static_cast<qsizetype>(bye.size())));
        }
        session->line.close();
        // Kept until it has gone (the controller's own goodbye, or its time), beyond this object if need be, on the
        // thread it lives on: by the program where that is the program's own, else by that thread until it ends.
        Session* const left = session.release();
        connect(&left->line, &DeviceLine::gone, left, [left] { left->deleteLater(); });
        if (QCoreApplication::instance() != nullptr &&
            QThread::currentThread() == QCoreApplication::instance()->thread()) {
            left->setParent(QCoreApplication::instance());
        } else {
            connect(QThread::currentThread(), &QThread::finished, left, &QObject::deleteLater);
        }
        m_leaving = left;
        return;
    }
    if (!session->over) {
        session->line.cut();
    }
    session.release()->deleteLater();
}

void ServerScreen::finish(const QString& code, const QString& message)
{
    const int shown = m_session != nullptr ? m_session->shown : 0;
    const qint64 since = m_session != nullptr ? m_session->since : 0;
    leave();
    m_picture = {};     // (the screen, megabytes of it, is not kept beyond the line it came over)
    m_greeting.clear(); // (nor the pass: it was good for that session alone)
    m_ticks.stop();
    if (code.isEmpty()) {
        m_refusal.reset();
        m_failures = 0;
    } else {
        m_refusal = error(code, message);
        if (shown > 0) {
            m_failures = 0; // it worked until now: this is the first failure, not one more
        }
        if (code == u"console_time_up"_s && shown > 0 && now() - since >= m_limits.settledMs) {
            // It ran its time, as a console in which no key is pressed does: no failure, and opened again at once.
            m_retryAt = now();
        } else {
            ++m_failures;
            const bool standing = code == u"no_picture"_s || code == u"console_held"_s;
            const int wait = code == u"console_held"_s ? m_limits.heldMs
                             : standing
                                 ? m_limits.giveUpMs
                                 : m_limits.retryMs[static_cast<std::size_t>(
                                       std::min<int>(m_failures, static_cast<int>(m_limits.retryMs.size())) - 1)];
            m_retryAt = now() + wait;
        }
    }
    // Whoever waits for a still is told after the session's end is (whoever hears that may throw this away).
    const std::vector<StillAsked> stills = std::exchange(m_stillsAsked, {});
    emit ended(code, message);
    const base::Error why =
        code.isEmpty() ? error(u"no_stream"_s, u"The server's console was given back"_s) : error(code, message);
    for (const StillAsked& one : stills) {
        one.done(why);
    }
}

// --- keys and the pointer ---------------------------------------------------------------------------------------

void ServerScreen::askForInput(InputAnswer answer)
{
    askForInput(std::move(answer), 0);
}

void ServerScreen::askForInput(InputAnswer answer, std::uint64_t typing)
{
    m_askings.push_back({std::move(answer), now() + m_limits.inputWaitMs, typing});
    answerInput();
}

void ServerScreen::answerInput()
{
    if (m_askings.empty()) {
        return;
    }
    // Looking keeps the console (and opens one): what it says counts for every asking. It may end a session, and
    // whoever hears that may throw this away.
    const QPointer<ServerScreen> self(this);
    const base::Result<void> looked = look();
    if (self == nullptr) {
        return;
    }
    // Each answer may do anything (ask again, close this, throw it away): the answered are taken out first.
    std::vector<std::pair<InputAnswer, base::Result<int>>> answered;
    for (auto asking = m_askings.begin(); asking != m_askings.end();) {
        std::optional<base::Result<int>> said = inputNow(asking->until, looked);
        if (said) {
            answered.emplace_back(std::move(asking->answer), std::move(*said));
            asking = m_askings.erase(asking);
        } else {
            ++asking;
        }
    }
    if (!m_askings.empty() || m_owed) {
        m_inputTicks.start(m_limits.tickMs);
    } else {
        m_inputTicks.stop();
    }
    for (auto& [answer, said] : answered) {
        answer(std::move(said));
        if (self == nullptr) {
            return;
        }
    }
}

std::optional<base::Result<int>> ServerScreen::inputNow(qint64 until, const base::Result<void>& looked) const
{
    const qint64 at = now();
    if (m_session != nullptr && m_session->handedOver) {
        return handedOver();
    }
    if (m_session != nullptr && m_session->mine) {
        if (at >= m_session->keysFrom) {
            return m_turn;
        }
        if (at >= until) {
            return error(u"no_stream"_s, u"Waiting for the server's keyboard and mouse to be attached"_s);
        }
        return std::nullopt;
    }
    if (!looked && looked.error().code != "no_stream") {
        return looked.error(); // (somebody holds it, the controller refused, it is no screen this program reads)
    }
    if (m_session != nullptr && m_session->acceptedAt >= 0 && at - m_session->acceptedAt > m_limits.masterWaitMs) {
        return error(u"in_use"_s, u"Another session holds this server's console: this program shows its screen and "
                                  "types nothing into it"_s);
    }
    if (at >= until) {
        return error(u"no_stream"_s, u"Waiting for the server's console"_s);
    }
    return std::nullopt;
}

base::Result<void> ServerScreen::mayGo(int turn, bool letsGo)
{
    if (m_session == nullptr || !m_session->mine || turn != m_turn) {
        return m_lost;
    }
    if (now() < m_session->keysFrom) {
        return error(u"no_stream"_s, u"Waiting for the server's keyboard and mouse to be attached"_s);
    }
    // A key from then would be pressed now: nothing that presses is said behind what has waited too long to leave.
    // A letting go presses nothing, and goes.
    if (!letsGo && m_session->line.waitedMs() > m_limits.inputSayMs) {
        return error(u"dropped"_s, u"The server's console stopped taking keys and the mouse"_s);
    }
    return {};
}

base::Result<void> ServerScreen::inTurn(int turn) const
{
    if (m_session == nullptr || !m_session->mine || turn != m_turn) {
        return m_lost;
    }
    return {};
}

base::Result<void> ServerScreen::sayInput(int turn, const ami::Bytes& report)
{
    const QPointer<ServerScreen> self(this);
    if (!say(report)) {
        // (The session ended in the saying, which may have thrown this away.)
        return self != nullptr && turn != m_turn ? m_lost : consoleClosed();
    }
    return {};
}

base::Result<void> ServerScreen::keyboard(const typed::Report& keys, int turn)
{
    const bool letsGo = m_session != nullptr && onlyLetsGo(m_session->keysThere, keys);
    if (auto may = mayGo(turn, letsGo); !may) {
        return may;
    }
    Session& session = *m_session;
    const bool behind = session.line.waitedMs() > 0;
    if (auto said = sayInput(turn, ami::keyboard(keys.modifiers, keys.keys, ++session.reports)); !said) {
        return said;
    }
    session.keysThere = keys;
    if (letsGo && behind) {
        watchLettingGo();
    }
    return {};
}

base::Result<void> ServerScreen::mouse(std::uint8_t buttons, int x, int y, int wheel, int turn)
{
    const bool letsGo = m_session != nullptr && m_session->buttonsDown != 0 &&
                        (buttons & ~m_session->buttonsDown) == 0 && buttons != m_session->buttonsDown;
    if (auto may = mayGo(turn, letsGo); !may) {
        return may;
    }
    Session& session = *m_session;
    const bool behind = session.line.waitedMs() > 0;
    // (Nothing is held where it is relative: what was held was let go of as it became so.)
    if (session.relative) {
        return error(u"no_input"_s,
                     u"This server's mouse is a relative one, and this program moves an absolute one only, so far"_s);
    }
    const std::pair<int, int> place{std::clamp(x, 0, ami::farthest), std::clamp(y, 0, ami::farthest)};
    if (auto said = sayInput(turn, ami::mouseAbsolute(buttons, place.first, place.second, wheel, ++session.reports));
        !said) {
        return said;
    }
    session.buttonsDown = buttons;
    session.place = place;
    if (letsGo && behind) {
        watchLettingGo();
    }
    return {};
}

bool ServerScreen::giveUpInput()
{
    Session& session = *m_session;
    const bool mine = session.mine;
    session.handedOver = true;
    session.mine = false;
    endTurn(handedOver());
    // The one thing of this program's that goes into a console that is about to be, or just became, somebody else's:
    // a key left down would repeat on the server, while letting go can at most lift for a moment a key the newcomer
    // holds, which their next report puts down again.
    if (mine && !letGoOfWhatIsDown()) {
        return false;
    }
    session.keysThere = {};
    session.buttonsDown = 0;
    return true;
}

bool ServerScreen::letGoOfWhatIsDown()
{
    Session& session = *m_session;
    if (session.keysDown()) {
        session.keysThere = {};
        if (!say(ami::keyboard(0, {}, ++session.reports))) {
            return false;
        }
    }
    if (session.buttonsDown != 0) {
        session.buttonsDown = 0;
        if (!say(ami::mouseAbsolute(0, session.place.first, session.place.second, 0, ++session.reports))) {
            return false;
        }
    }
    return true;
}

void ServerScreen::endTurn(base::Error why)
{
    m_turn = aNewTurn();
    m_lost = std::move(why);
    // A typing under way hears of it at its next step, now: what it had down was let go of with the turn.
    if (!m_typings.empty() && m_typings.front()->turn >= 0) {
        m_typingSteps.start(0);
    }
}

void ServerScreen::watchLettingGo()
{
    if (!m_owed) {
        m_owed = now() + m_limits.letGoWithinMs;
    }
    m_inputTicks.start(m_limits.tickMs);
}

void ServerScreen::letGoOwed()
{
    if (!m_owed) {
        return;
    }
    if (m_session == nullptr || m_session->line.waitedMs() == 0) {
        // It has left (all that waited has: it was behind that), or the session is over (and a controller lets go of
        // what a session had down as it ends; this program let go of it before its goodbye).
        m_owed.reset();
        return;
    }
    if (now() > *m_owed) {
        // The console takes nothing, and a key, a modifier or a button it was told to let go of may still be down
        // there, repeating on the server, while the line goes on: the session is ended, which lets go of it.
        m_owed.reset();
        const QPointer<ServerScreen> self(this);
        emit leftDown();
        if (self == nullptr || m_session == nullptr) {
            return;
        }
        finish(u"dropped"_s, u"The server's console took nothing for so long that a key or a button it was told to "
                             "let go of may have stayed down: its session was ended, which lets go of it"_s);
    }
}

void ServerScreen::inputTick()
{
    const QPointer<ServerScreen> self(this);
    letGoOwed();
    if (self == nullptr) {
        return;
    }
    answerInput();
    if (self != nullptr && m_askings.empty() && !m_owed) {
        m_inputTicks.stop();
    }
}

// --- typings ------------------------------------------------------------------------------------------------------

void ServerScreen::press(const QString& keys, Still still, Typed done)
{
    base::Result<typed::Report> report = typed::combination(keys, u"This server's console"_s);
    auto typing = std::make_unique<Typing>();
    typing->still = std::move(still);
    typing->done = std::move(done);
    typing->heldMs = m_limits.holdMs;
    if (report) {
        typing->reports.push_back(std::move(report).value());
    } else {
        // (Answered later, as every typing is: never from inside the asking.)
        QTimer::singleShot(0, this, [done = std::move(typing->done), why = report.error()] { done(why); });
        return;
    }
    queueTyping(std::move(typing));
}

void ServerScreen::type(const QString& text, Still still, Typed done)
{
    base::Result<std::vector<typed::Report>> reports = typed::text(text);
    if (!reports) {
        QTimer::singleShot(0, this, [done = std::move(done), why = reports.error()] { done(why); });
        return;
    }
    auto typing = std::make_unique<Typing>();
    typing->reports = std::move(reports).value();
    typing->heldMs = m_limits.keyPauseMs;
    typing->still = std::move(still);
    typing->done = std::move(done);
    queueTyping(std::move(typing));
}

void ServerScreen::queueTyping(std::unique_ptr<Typing> typing)
{
    typing->number = ++m_typingsMade;
    m_typings.push_back(std::move(typing));
    if (m_typings.size() == 1) {
        m_typingSteps.start(0);
    }
}

void ServerScreen::stopTyping()
{
    m_typingSteps.stop();
    std::deque<std::unique_ptr<Typing>> typings = std::exchange(m_typings, {});
    // Their askings for the keyboard go with them: nobody waits for those, and they would keep the console.
    std::erase_if(m_askings, [](const Asking& asking) { return asking.typing != 0; });
    const QPointer<ServerScreen> self(this);
    if (!typings.empty() && typings.front()->down) {
        // Stopped between its press and its letting go: let go of now (behind what waits, where something does).
        static_cast<void>(keyboard({}, typings.front()->turn));
        if (self == nullptr) {
            return;
        }
    }
    for (const std::unique_ptr<Typing>& typing : typings) {
        typing->done(error(u"stopped"_s, u"Typing was stopped"_s));
        if (self == nullptr) {
            return;
        }
    }
}

void ServerScreen::typingStep()
{
    if (m_typings.empty()) {
        return;
    }
    Typing* const typing = m_typings.front().get();
    // Copied before anything is said: what is said may end the session, and whoever hears that may stop every typing
    // or throw this away; the typing is looked for again by its number after.
    const std::uint64_t number = typing->number;
    const int turn = typing->turn;
    const QPointer<ServerScreen> self(this);
    const auto stillThere = [this, &self, number] {
        return self != nullptr && !m_typings.empty() && m_typings.front()->number == number;
    };
    // A report of the typing's: what came of it, or nothing where the typing is gone in the saying.
    const auto said = [this, turn, &stillThere](const typed::Report& keys) -> std::optional<base::Result<void>> {
        base::Result<void> result = keyboard(keys, turn);
        if (!stillThere()) {
            return std::nullopt;
        }
        return result;
    };
    if (!typing->asked) {
        typing->asked = true;
        askForInput(
            [self, number](base::Result<int> given) {
                if (self != nullptr && !self->m_typings.empty() && self->m_typings.front()->number == number) {
                    self->typingTurn(number, std::move(given));
                }
            },
            number);
        return;
    }
    if (turn < 0) {
        return; // (its keyboard is still asked for)
    }
    if (typing->down) {
        // Nothing stays down: a key held repeats on the server until it is let go of.
        std::optional<base::Result<void>> letGo = said({});
        if (!letGo) {
            return;
        }
        if (!*letGo) {
            endTyping(std::move(*letGo)); // (the turn is over, and it was let go of with it; or the console is gone)
            return;
        }
        typing->down = false;
        ++typing->next;
        m_typingSteps.start(m_limits.keyPauseMs);
        return;
    }
    if (typing->next == typing->reports.size()) {
        endTyping({});
        return;
    }
    // A long text takes a while, all of it this program pressing keys: the console is kept meanwhile, and whoever
    // asked is asked now and then whether they may still have it done.
    m_wantedAt = now();
    const auto sent = static_cast<int>(typing->next);
    if (sent > 0 && sent % m_limits.askAgainEvery == 0 && typing->still) {
        const bool may = typing->still();
        if (!stillThere()) {
            return; // (whoever was asked stopped it, or threw this away)
        }
        if (!may) {
            endTyping(error(u"stopped"_s, u"Typing was stopped after %1 of %2 keys: whoever asked for it may no longer "
                                          "do that"_s.arg(sent)
                                              .arg(typing->reports.size())));
            return;
        }
    }
    std::optional<base::Result<void>> pressed = said(typing->reports[typing->next]);
    if (!pressed) {
        return;
    }
    if (!*pressed) {
        endTyping(std::move(*pressed));
        return;
    }
    typing->down = true;
    m_typingSteps.start(typing->heldMs);
}

void ServerScreen::typingTurn(std::uint64_t number, base::Result<int> turn)
{
    if (!turn) {
        endTyping(turn.error());
        return;
    }
    Typing* const typing = m_typings.front().get();
    const QPointer<ServerScreen> self(this);
    const bool may = !typing->still || typing->still();
    if (self == nullptr || m_typings.empty() || m_typings.front()->number != number) {
        return; // (whoever was asked stopped it, or threw this away)
    }
    if (!may) {
        endTyping(error(u"stopped"_s, u"It was not done: whoever asked for it may no longer do that"_s));
        return;
    }
    typing->turn = turn.value();
    m_typingSteps.start(0);
}

void ServerScreen::endTyping(base::Result<void> how)
{
    m_typingSteps.stop();
    std::unique_ptr<Typing> typing = std::move(m_typings.front());
    m_typings.pop_front();
    if (!m_typings.empty()) {
        m_typingSteps.start(0);
    }
    typing->done(std::move(how));
}

} // namespace qkvm::core
