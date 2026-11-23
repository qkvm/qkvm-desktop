#include <qkvm/core/rfb_screen.h>

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QThread>

#include <qkvm/core/tls.h>
#include <qkvm/rfb/rfb.h>

#include "jpeg_pixels.h"
#include "rfb_reader.h"

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// What a console may send in a second, and in how many messages: a busy screen in Raw at a viewer's pace, which the
// reader holds to its own bounds of what a screen can change.
constexpr qint64 mostBytesASecond = qint64{160} * 1024 * 1024;
constexpr int mostMessagesASecond = 20000;

base::Error error(const QString& code, const QString& message)
{
    return {code.toStdString(), message.toStdString()};
}

QByteArray bytesOf(const rfb::Bytes& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}

// How the reader's end is said (the web's VncDevice._ended and OpenBmcScreen._ended), and whether it is a refusal (left
// alone on the ladder) rather than a session that fell.
// "The VNC server" of "the VNC server": how a sentence begins with what the screen is called.
QString capitalised(const QString& thing)
{
    return thing.isEmpty() ? thing : thing.first(1).toUpper() + thing.sliced(1);
}

std::pair<QString, QString> saidOfTheReader(const base::Error& why, const QString& thing)
{
    const QString message = QString::fromStdString(why.message);
    const std::string& code = why.code;
    // A password that was answered and rejected, and one that was asked for where none was handed in: said as every
    // family's sign-in says them (the web's NO_SAID, and "no_password"), nothing of the password in either.
    if (code == rfb::auth) {
        return {u"auth_failed"_s, u"Login rejected - check the username and password"_s};
    }
    if (code == rfb::noPassword) {
        return {u"no_password"_s, u"No password saved for this device"_s};
    }
    // (In the family's own words where it has them: the web's VncDevice._ended.)
    const QString the = thing.isEmpty() ? u"The server's console"_s : capitalised(thing);
    if (code == rfb::refused || code == rfb::unsupported) {
        return {u"bad_response"_s, the + u" does not let qKVM in: "_s + message};
    }
    if (code == rfb::protocol || code == rfb::limit) {
        return {u"bad_response"_s, the + u" sent what qKVM does not take: "_s + message};
    }
    return {u"dropped"_s, the + u": "_s + message};
}

// Why the line ended by itself, in the driver's codes and words (as ServerScreen's, and the web's rfb_socket).
std::pair<QString, QString> saidOfTheLine(DeviceLine::End why, bool ready, const QString& thing)
{
    if (!thing.isEmpty()) {
        // A server's own connection (a VNC server's), in the web's words for a device's (VncDevice._connect_error and
        // _ended); a certificate as every line says it.
        const QString the = capitalised(thing);
        switch (why) {
        case DeviceLine::End::NotAsked:
            return {u"no_picture"_s, the + u"'s address is not one to connect to"_s};
        case DeviceLine::End::NotReached:
            return {u"unreachable"_s, u"Device is unreachable"_s};
        case DeviceLine::End::Silent:
            return {u"timeout"_s, u"Device did not respond in time"_s};
        case DeviceLine::End::SignedOut:
        case DeviceLine::End::Refused:
        case DeviceLine::End::NotLetIn:
            return {u"bad_response"_s, the + u" does not let qKVM in"_s};
        case DeviceLine::End::Fell:
            return {u"dropped"_s, ready ? the + u": the connection to the server ended"_s
                                        : the + u": the connection ended before the handshake was over"_s};
        case DeviceLine::End::Misbehaved:
            return {u"bad_response"_s, the + u" sent what qKVM does not take: more than a server has reason to send"_s};
        case DeviceLine::End::TakesNothing:
        case DeviceLine::End::Late:
            return {u"dropped"_s, the + u" stopped taking what is said to it"_s};
        case DeviceLine::End::TlsDamaged:
        case DeviceLine::End::CertificateUnread:
        case DeviceLine::End::CertificateChanged:
        case DeviceLine::End::CertificateUntrusted:
        case DeviceLine::End::CertificateOfUnknownSigner:
            break;
        }
    }
    switch (why) {
    case DeviceLine::End::NotAsked:
        return {u"no_picture"_s, u"The way into the server's console is not fit to be sent"_s};
    case DeviceLine::End::TlsDamaged:
        return {QString::fromLatin1(tlsDamaged), QString::fromLatin1(tlsDamagedSentence)};
    case DeviceLine::End::CertificateUnread:
        return {u"cert_invalid"_s,
                u"The certificate saved for this device cannot be read. Look at its certificate and trust it again."_s};
    case DeviceLine::End::CertificateChanged:
        return {u"cert_mismatch"_s, u"Certificate does not match the one saved for this device"_s};
    case DeviceLine::End::CertificateUntrusted:
    case DeviceLine::End::CertificateOfUnknownSigner:
        return {u"cert_invalid"_s, u"The device's certificate is not trusted"_s};
    case DeviceLine::End::Silent:
        return {u"timeout"_s, u"The controller's console did not answer in time"_s};
    case DeviceLine::End::SignedOut:
        return {u"bad_response"_s, u"The controller does not let this account open its console (HTTP 401)"_s};
    case DeviceLine::End::Refused:
        return {u"bad_response"_s, u"The controller does not let this account open its console (HTTP 403)"_s};
    case DeviceLine::End::NotLetIn:
        return {u"bad_response"_s, u"The controller did not open its console"_s};
    case DeviceLine::End::NotReached:
        return {u"dropped"_s, u"The controller's console could not be opened"_s};
    case DeviceLine::End::Fell:
        return {u"dropped"_s, ready ? u"The server's console: the connection to the server ended"_s
                                    : u"The controller closed its console"_s};
    case DeviceLine::End::Misbehaved:
        return {u"bad_response"_s, u"The controller's console sent what this program does not accept"_s};
    case DeviceLine::End::TakesNothing:
    case DeviceLine::End::Late:
        return {u"dropped"_s, u"The controller's console stopped taking what is said to it"_s};
    }
    return {u"dropped"_s, u"The controller closed its console"_s};
}

// The farthest place of a live line's pointer, each way (as ServerLive counts it).
constexpr int farthest = 32767;

base::Error waitingForTheConsole()
{
    return {"no_stream", "Waiting for the server's console"};
}

base::Error stoppedTyping()
{
    return {"stopped", "It was not done: whoever asked for it may no longer do that"};
}

using Event = std::pair<std::uint32_t, bool>;

// What a key's USB number goes as (rfb::keysymFor, without Shift): none for a key that has no keysym.
std::optional<std::uint32_t> keysymOfKey(int usage)
{
    return rfb::keysymFor(usage);
}

// The key events that make the keyboard `after` of `before`: what is let go of first, keys before modifiers, then what
// goes down, modifiers before keys (a key with no keysym goes nowhere).
std::vector<Event> changesOf(const typed::Report& before, const typed::Report& after)
{
    std::vector<Event> events;
    const auto add = [&events](int usage, bool down) {
        if (const std::optional<std::uint32_t> keysym = keysymOfKey(usage)) {
            events.emplace_back(*keysym, down);
        }
    };
    for (const std::uint8_t key : before.keys) {
        if (std::ranges::find(after.keys, key) == after.keys.end()) {
            add(key, false);
        }
    }
    for (int bit = 0; bit < 8; ++bit) {
        if ((before.modifiers >> bit & 1) != 0 && (after.modifiers >> bit & 1) == 0) {
            add(0xE0 + bit, false);
        }
    }
    for (int bit = 0; bit < 8; ++bit) {
        if ((after.modifiers >> bit & 1) != 0 && (before.modifiers >> bit & 1) == 0) {
            add(0xE0 + bit, true);
        }
    }
    for (const std::uint8_t key : after.keys) {
        if (std::ranges::find(before.keys, key) == before.keys.end()) {
            add(key, true);
        }
    }
    return events;
}

QByteArray eventsOf(std::span<const Event> events)
{
    QByteArray said;
    for (const auto& [keysym, down] : events) {
        said += bytesOf(rfb::keyEvent(keysym, down));
    }
    return said;
}

// A live line's buttons (typed::buttonBit: 1 left, 2 right, 4 middle) as RFB's (1 left, 2 middle, 4 right).
std::uint8_t rfbButtons(std::uint8_t buttons) noexcept
{
    return static_cast<std::uint8_t>((buttons & 1) | ((buttons & 4) != 0 ? 2 : 0) | ((buttons & 2) != 0 ? 4 : 0));
}

} // namespace

// One console session: its line, its reader, and how far it has got.
struct RfbScreen::Session : QObject {
    Session(DeviceLine::Where where, DeviceLine::Limits limits) : line(std::move(where), limits) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Gone, its line with it: the way in it was made of is given back (WayIn::done).
    ~Session() override
    {
        if (reader != nullptr) {
            reader->detach();
        }
        if (done) {
            done();
        }
    }

    // The line held back (nothing more is taken off it), and let go: the time between is time away.
    void holdBack(qint64 at)
    {
        if (!held) {
            held = true;
            line.holdBack(true);
            awayFrom = at;
            quietSince = at;
        }
    }
    void letGo(qint64 at)
    {
        if (held) {
            held = false;
            line.holdBack(false);
            away += std::max<qint64>(0, at - awayFrom);
            awayFrom = -1;
        }
    }
    // The time away by `at` (now, or a moment of the hold under way: a span that ended after `at` is counted as before
    // it, which only shortens the server's time), and the server's own time from a deadline's start to `at`: the clock
    // less the time away meanwhile.
    qint64 awayBy(qint64 at) const { return away + (awayFrom >= 0 ? std::max<qint64>(0, at - awayFrom) : 0); }
    qint64 serverSince(qint64 at, qint64 since, qint64 awayThen) const
    {
        return (at - since) - (awayBy(at) - awayThen);
    }
    void hear(qint64 at)
    {
        heardAt = at;
        heardAway = awayBy(at);
    }

    std::function<void()> done;
    std::function<base::Result<void>()> passwordSent;
    DeviceLine line;
    std::shared_ptr<RfbReader> reader;
    bool over = false; // the line ended by itself
    // The line fell while what came before its end was not read yet (fedSinceTake): the reader is heard out first,
    // and the fall is the reason only where it said none (takeReads; tick, after a second at the most).
    bool fedSinceTake = false;
    std::optional<DeviceLine::End> fell;
    qint64 fellAt = -1;
    bool held = false;      // nothing more is taken off the line (the reader has its fill, or the share is spent)
    qint64 inAt = -1;       // let in
    qint64 readyAt = -1;    // the handshake over
    qint64 shareUntil = -1; // over its share: the line is held back and nothing is asked until now() reaches this
    // The time this program was away from the server's bytes, holding its line back of its own accord: not the
    // server's, so taken out of its deadlines, as the web's _now and _away (rfb.py:1108-1122). The held spans that have
    // ended, and when the one held now began (-1: none).
    qint64 away = 0;
    qint64 awayFrom = -1;
    // The reader's silence since: its last telling, or the line held back, whichever is later (round-5 review LOW 2:
    // one that tells nothing while its queue holds the line is not waited for: tick).
    qint64 quietSince = 0;
    // Each deadline's start, with the time away by then (serverSince): the last bytes heard (the next piece), the
    // message under way (the reader's stamp, and the moment on this clock), the update asked for, and the pixel asked
    // for outright after a while of none.
    qint64 heardAt = 0;
    qint64 heardAway = 0;
    QString name;
    int minor = 0;
    int width = 0;
    int height = 0;
    std::uint64_t updates = 0;
    int shown = 0; // frames that showed the screen
    std::optional<std::int64_t> messageSince;
    qint64 messageAt = 0;
    qint64 messageAway = 0;
    bool asked = false;
    qint64 askedAt = 0;
    qint64 askedAway = 0;
    bool heartbeat = false;
    qint64 heartbeatAt = 0;
    qint64 heartbeatAway = 0;
    bool full = true; // the next request asks for the whole screen
    // Keys and the pointer: the turn they go in, and what this program has down there as far as it has said.
    Turn turn = 0;
    typed::Report keysThere;
    std::uint8_t buttonsDown = 0; // (RFB's bits)
    std::pair<int, int> place{0, 0};
};

// A typing: its key events, how far it has got, and who asked.
struct RfbScreen::Typing {
    std::uint64_t number = 0; // which, of all this screen was asked to type: what its asking's answer is told by
    std::vector<Event> events;
    Still still;
    Typed done;
    bool asked = false;   // its keyboard was asked for
    Turn turn = -1;       // ...and given, in this turn
    std::size_t next = 0; // the event that goes next
};

RfbScreen::RfbScreen(Asker asker, Shown shown, Limits limits, QObject* parent)
    : ServerConsole(parent), m_asker(std::move(asker)), m_shown(std::move(shown)), m_limits(std::move(limits))
{
    m_clock.start();
    connect(&m_ticks, &QTimer::timeout, this, [this] { tick(); });
    m_paces.setSingleShot(true);
    connect(&m_paces, &QTimer::timeout, this, [this] { pace(); });
    m_shareWait.setSingleShot(true);
    connect(&m_shareWait, &QTimer::timeout, this, [this] { resumeShare(); });
    connect(&m_inputTicks, &QTimer::timeout, this, [this] {
        if (m_askings.empty()) {
            m_inputTicks.stop();
            return;
        }
        static_cast<void>(look()); // (keys that wait keep the console wanted)
        answerInput();
    });
    m_typingSteps.setSingleShot(true);
    connect(&m_typingSteps, &QTimer::timeout, this, [this] { typingStep(); });
}

RfbScreen::~RfbScreen()
{
    m_closed = true;
    leave();
}

QString RfbScreen::serverName() const
{
    return m_session != nullptr ? m_session->name : QString();
}

base::Result<void> RfbScreen::keep()
{
    return want(false);
}

bool RfbScreen::letIn() const
{
    return m_session != nullptr && m_session->readyAt >= 0;
}

int RfbScreen::width() const
{
    return m_session != nullptr ? m_session->width : 0;
}

int RfbScreen::height() const
{
    return m_session != nullptr ? m_session->height : 0;
}

qint64 RfbScreen::letInForMs() const
{
    return m_session != nullptr && m_session->readyAt >= 0 ? now() - m_session->readyAt : -1;
}

void RfbScreen::retryNow()
{
    if (m_refusal) {
        m_retryAt = std::min(m_retryAt, now());
    }
}

int RfbScreen::rfbMinor() const
{
    return m_session != nullptr && m_session->readyAt >= 0 ? m_session->minor : 0;
}

std::optional<base::Error> RfbScreen::standing() const
{
    if (m_session == nullptr && !m_asking && m_refusal && now() < m_retryAt) {
        return m_refusal;
    }
    return std::nullopt;
}

QString RfbScreen::said(const QString& ofAConsole, const QString& reason) const
{
    return m_limits.thing.isEmpty() ? ofAConsole : capitalised(m_limits.thing) + u": "_s + reason;
}

int RfbScreen::paceNow() const
{
    if (live()) {
        return m_limits.livePaceMs;
    }
    // (Nobody has looked for a while: the idle pace, where the family has one. A session held only while somebody
    // looks has none.)
    if (m_limits.idlePaceMs > 0 && now() - m_wantedAt >= m_limits.watchedForMs) {
        return m_limits.idlePaceMs;
    }
    return m_limits.lookPaceMs;
}

bool RfbScreen::live() const
{
    return m_liveAt >= 0 && now() - m_liveAt < m_limits.liveForMs;
}

void RfbScreen::liveSeen()
{
    m_liveAt = now();
}

base::Result<void> RfbScreen::look()
{
    return want(true);
}

base::Result<void> RfbScreen::want(bool watching)
{
    if (m_closed) {
        return error(u"no_picture"_s, u"This server's screen is not looked at any more"_s);
    }
    if (watching) {
        m_wantedAt = now();
        // Somebody looks again while the next update waits out the idle pace: it is asked for at the looking pace.
        if (m_session != nullptr && m_session->readyAt >= 0 && !m_session->asked && m_paces.isActive() &&
            m_paces.remainingTime() > paceNow()) {
            m_paces.start(paceNow());
        }
    } else {
        m_keptAt = now();
    }
    const QString opening =
        m_limits.thing.isEmpty() ? u"Opening the server's console"_s : u"Connecting to "_s + m_limits.thing;
    if (m_asking) {
        return error(u"no_stream"_s, opening);
    }
    if (m_session != nullptr) {
        if (watching ? m_session->shown > 0 : m_session->readyAt >= 0) {
            return {};
        }
        // (Watched: its screen is waited for, as ever; kept: its handshake.)
        return watching ? error(u"no_stream"_s, u"Waiting for the server's screen"_s) : error(u"no_stream"_s, opening);
    }
    if (m_refusal && now() < m_retryAt) {
        return *m_refusal;
    }
    if (m_leaving != nullptr) {
        // (The last session's line has not gone: its seat is not free yet.)
        return error(u"no_stream"_s, u"Giving the server's console back"_s);
    }
    open();
    return error(u"no_stream"_s, opening);
}

void RfbScreen::open()
{
    m_asking = true;
    const QPointer<RfbScreen> self(this);
    m_asker(
        [self](base::Result<WayIn> way) {
            if (self == nullptr || !self->m_asking) {
                // Nobody waits for it any more: its session is given back at once.
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

void RfbScreen::entered(WayIn way)
{
    DeviceLine::Where where;
    where.address = way.address;
    where.asking = way.path;
    where.authToken = way.token;
    // (A copy of its own for the line, the one copy of it there is: the line writes it over once its upgrade went, or
    // when it goes without one. The way's is written over as the way goes.)
    where.authorization = QByteArray(way.authorization.view().data(), way.authorization.view().size());
    where.knownByCertificate = way.knownByCertificate;
    where.certificate = way.certificate;
    DeviceLine::Limits limits;
    limits.letInWithinMs = m_limits.openingMs;
    limits.goodbyeWithinMs = m_limits.goodbyeWithinMs;
    limits.largestFrameBytes = m_limits.largestMessageBytes;
    limits.mostBytesASecond = mostBytesASecond;
    limits.mostMessagesASecond = mostMessagesASecond;
    limits.mostUnsentBytes = m_limits.mostWaitingBytes;
    m_session = std::make_unique<Session>(std::move(where), limits);
    Session& session = *m_session;
    session.done = std::move(way.done);
    session.passwordSent = std::move(way.passwordSent);
    session.hear(now());
    // The reader: VNC authentication only where the way lets the server ask for it, with the password the way handed
    // in (or none), Tight's JPEGs decoded by FFmpeg's decoder, one of its own for the session.
    rfb::Options options;
    options.signsIn = way.vncAuthentication ? std::vector<std::uint8_t>{rfb::securityNone, rfb::securityVnc}
                                            : std::vector<std::uint8_t>{rfb::securityNone};
    options.sharePerMil = m_limits.drawingSharePerMil;
    options.burstMs = m_limits.drawingBurstMs;
    auto jpeg = std::make_shared<JpegPixels>();
    options.jpeg = [jpeg](std::span<const std::uint8_t> data, int width, int height) {
        return jpeg->decode(data, width, height);
    };
    session.reader = std::make_shared<RfbReader>(
        this, [this] { takeReads(); }, std::move(options),
        way.vncAuthentication ? std::move(way.password) : SecretText());
    // RFB on a WebSocket (a controller's console, a Proxmox guest's with its subprotocol), or straight on the
    // connection (a VNC server's).
    if (way.raw) {
        session.line.receiveRaw();
    } else if (way.protocol.isEmpty()) {
        session.line.receiveBytes();
    } else {
        session.line.receiveBytes(way.protocol);
    }
    connect(&session.line, &DeviceLine::letIn, this, [this] {
        if (m_session != nullptr) {
            m_session->inAt = now();
            m_session->hear(now());
        }
    });
    connect(&session.line, &DeviceLine::bytesCame, this, [this](const QByteArray& bytes) { heard(bytes); });
    connect(&session.line, &DeviceLine::certificateShown, this, &RfbScreen::certificateShown);
    connect(&session.line, &DeviceLine::ended, this, [this](DeviceLine::End why) { lineEnded(why); });
    m_ticks.start(m_limits.tickMs);
    session.line.open();
}

void RfbScreen::heard(const QByteArray& bytes)
{
    if (m_session == nullptr) {
        return;
    }
    Session& session = *m_session;
    session.hear(now());
    session.fedSinceTake = true;
    session.reader->feed(bytes);
    if (session.reader->waiting() > m_limits.heldBackFromBytes) {
        session.holdBack(now()); // (let go when the reader has read its fill: takeReads)
    }
}

void RfbScreen::takeReads()
{
    if (m_session == nullptr) {
        return;
    }
    Session* const session = m_session.get();
    RfbReader::Taken taken = session->reader->take(m_picture);
    session->quietSince = now();
    session->fedSinceTake = false;
    const QPointer<RfbScreen> self(this);
    if (taken.passwordAnswered && session->passwordSent) {
        // The moment before the answer to the challenge goes (the web's before_password), and whether it may go now:
        // where it may not (another sign-in with that password is out), it is thrown away with the session, unsent
        // (outside review 2.4).
        const base::Result<void> mayGo = session->passwordSent();
        if (self == nullptr || m_session.get() != session) {
            return;
        }
        if (!mayGo) {
            finish(QString::fromStdString(mayGo.error().code), QString::fromStdString(mayGo.error().message));
            return;
        }
    }
    if (!session->fell && !taken.outgoing.isEmpty() && !say(taken.outgoing)) {
        return;
    }
    if (taken.ended) {
        const auto [code, message] = saidOfTheReader(*taken.ended, m_limits.thing);
        finish(code, message);
        return;
    }
    if (session->fell) {
        // What came before the line fell is read, and said nothing of why: the fall is the reason.
        finishForTheLine(*session->fell);
        return;
    }
    session->width = taken.width;
    session->height = taken.height;
    if (taken.messageSince != session->messageSince) {
        session->messageSince = taken.messageSince;
        if (taken.messageSince) {
            // When the reader first saw it under way (stamped by its own steady clock), on this clock, and the time
            // away by then.
            session->messageAt = now() - std::max<qint64>(0, RfbReader::steadyMs() - *taken.messageSince);
            session->messageAway = session->awayBy(session->messageAt);
        }
    }
    if (taken.ready && session->readyAt < 0) {
        session->readyAt = now();
        session->name = taken.name;
        session->minor = taken.minor;
        emit handshakeOver();
        if (self == nullptr || m_session.get() != session) {
            return;
        }
        session->turn = aNewTurn();
        // (Keys are given once obmc-ikvm takes them: looked at again then.)
        QTimer::singleShot(m_limits.inputAfterMs, this, [this] { answerInput(); });
        request(false, false); // (the whole screen first)
        if (self == nullptr || m_session.get() != session) {
            return;
        }
    }
    if (taken.updates != session->updates) {
        // Answered (or sent unasked): the next is asked for at the pace that is wanted now.
        session->updates = taken.updates;
        if (session->asked) {
            session->asked = false;
            session->heartbeat = false;
            m_paces.start(paceNow());
        }
    }
    if (taken.pausedForMs > 0) {
        // The reader drew past its share: show this server more slowly (round-2 review 2). Hold its line back and ask
        // for nothing until the share has earned the debt back; a single-shot lets it go then, so a short pause costs
        // no more than it is (round-3 finding 4). Never refused for this.
        session->shareUntil = now() + taken.pausedForMs;
        session->holdBack(now());
        m_paces.stop();
        m_shareWait.start(static_cast<int>(taken.pausedForMs));
    }
    for (const RfbReader::Still& still : taken.stills) {
        const auto found = std::ranges::find(m_stillsAsked, still.number, &StillAsked::number);
        if (found == m_stillsAsked.end()) {
            continue;
        }
        const StillAnswer done = std::move(found->done);
        m_stillsAsked.erase(found);
        if (still.picture.isEmpty()) {
            done(error(u"no_stream"_s, u"Waiting for the server's screen"_s));
        } else {
            done(still.picture);
        }
        if (self == nullptr || m_session.get() != session) {
            return;
        }
    }
    if (!taken.changed.empty()) {
        ++session->shown;
        m_shown(m_picture, taken.changed);
        if (self == nullptr || m_session.get() != session) {
            return;
        }
    }
    const bool paused = session->shareUntil >= 0 && now() < session->shareUntil;
    if (!paused && session->reader->waiting() <= m_limits.heldBackFromBytes / 2) {
        // The server may send again now. The time it was held back is not its own: taken out of its deadlines
        // (Session::away), its clocks go on from where they stood (finding 5, round-4 review LOW 3).
        session->letGo(now());
    }
}

void RfbScreen::pace()
{
    if (m_session == nullptr || m_session->readyAt < 0 || m_session->asked) {
        return;
    }
    if (m_session->shareUntil >= 0 && now() < m_session->shareUntil) {
        return; // over its share: ask for nothing until it is earned back (resumeShare)
    }
    request(true, false);
}

void RfbScreen::resumeShare()
{
    if (m_session == nullptr || m_session->shareUntil < 0) {
        return;
    }
    Session& session = *m_session;
    session.shareUntil = -1;
    if (session.reader->waiting() <= m_limits.heldBackFromBytes / 2) {
        session.letGo(now());
    }
    // Ask again: a pushing server flows once the line is free; a paced one is asked at its pace.
    if (session.readyAt >= 0 && !session.asked) {
        m_paces.start(paceNow());
    }
}

void RfbScreen::request(bool incremental, bool onePixel)
{
    if (m_session == nullptr || m_session->width <= 0) {
        return;
    }
    Session& session = *m_session;
    const bool whole = session.full || !incremental;
    const rfb::Bytes asked = onePixel ? rfb::updateRequest(false, 0, 0, 1, 1)
                                      : rfb::updateRequest(!whole, 0, 0, session.width, session.height);
    if (!onePixel) {
        session.full = false;
        session.asked = true;
        session.askedAt = now();
        session.askedAway = session.awayBy(session.askedAt);
    }
    static_cast<void>(say(bytesOf(asked)));
}

bool RfbScreen::say(const QByteArray& bytes)
{
    if (m_session == nullptr) {
        return false;
    }
    const QPointer<RfbScreen> self(this);
    Session* const session = m_session.get();
    if (session->line.sayBytes(bytes)) {
        return true;
    }
    // (Where the line ended in the saying, that was said already, and may have thrown this away.)
    if (self != nullptr && m_session.get() == session) {
        finish(u"dropped"_s, said(u"The controller's console stopped taking what is said to it"_s,
                                  u"it stopped taking what is said to it"_s));
    }
    return false;
}

void RfbScreen::tick()
{
    if (m_session == nullptr) {
        m_ticks.stop();
        return;
    }
    Session& session = *m_session;
    const qint64 at = now();
    if (session.fell && at - session.fellAt > 1000) {
        // (The reader said nothing in a second of what came before the fall: the fall is the reason.)
        finishForTheLine(*session.fell);
        return;
    }
    // (Kept, as a VNC server's family keeps its session for as long as its machine is on the wall, is wanted too.)
    if (!live() && at - std::max(m_wantedAt, m_keptAt) > m_limits.idleMs) {
        finish({}, u"Nobody looks at the server's screen: its console was given back"_s);
        return;
    }
    if (session.shareUntil >= 0 && at >= session.shareUntil) {
        resumeShare(); // (the single-shot usually does this first; this is its safety net)
    }
    if (session.inAt >= 0 && session.readyAt < 0 && at - session.inAt > m_limits.handshakeMs) {
        finish(u"timeout"_s, said(u"The server's console did not finish the handshake in time"_s,
                                  u"the server did not finish the handshake in time"_s));
        return;
    }
    // The reader itself (round-5 review LOW 2): the line held back for its queue and the reader telling nothing for
    // readerQuietMs (the message deadline's length; out of memory twice, or a pool that never runs it) would hold the
    // line, and still the server's deadlines (the time held is away), for good. Not waited for: the session is ended.
    // In a share pause an idle reader rightly tells nothing (and once it ends, the line is let go or the reader still
    // drains, telling as it goes).
    if (session.held && !(session.shareUntil >= 0 && at < session.shareUntil) &&
        at - session.quietSince > m_limits.readerQuietMs) {
        finish(u"dropped"_s, u"The server's screen was not read in time"_s);
        return;
    }
    // The server's deadlines run on its own time: the clock less the time this program held its line back of its own
    // accord meanwhile (its reader over its drawing share, or with its fill), as the web's _now and _away
    // (rfb.py:1108-1122; round-4 review LOW 3, finding 5). Taken out, not skipped: they are looked at throughout, and
    // the server's own time before a hold still counts after it.
    if (session.messageSince) {
        if (session.serverSince(at, session.messageAt, session.messageAway) > m_limits.messageWithinMs) {
            finish(u"timeout"_s, said(u"The server's console did not send a whole message in time"_s,
                                      u"the server did not send a whole message in time"_s));
            return;
        }
        if (session.serverSince(at, session.heardAt, session.heardAway) > m_limits.pieceWithinMs) {
            finish(u"timeout"_s, said(u"The server's console began a message and did not finish it"_s,
                                      u"the server began a message and did not finish it"_s));
            return;
        }
    }
    if (session.readyAt >= 0 && session.asked) {
        // A still screen answers nothing for as long as it is still: one pixel is asked for outright, which any server
        // answers; one that answers that neither has stopped answering.
        if (!session.heartbeat && session.serverSince(at, session.askedAt, session.askedAway) > m_limits.heartbeatMs) {
            session.heartbeat = true;
            session.heartbeatAt = at;
            session.heartbeatAway = session.awayBy(at);
            request(false, true);
        } else if (session.heartbeat &&
                   session.serverSince(at, session.heartbeatAt, session.heartbeatAway) > m_limits.messageWithinMs) {
            finish(u"timeout"_s,
                   said(u"The server's console does not answer requests"_s, u"the server does not answer requests"_s));
        }
    }
}

void RfbScreen::lineEnded(DeviceLine::End why)
{
    if (m_session == nullptr) {
        return;
    }
    m_session->over = true;
    // A server that says why it ends a connection and then ends it (a rejected password, a refusal, with its reason):
    // what it said is read before the end of the line is taken for the reason.
    if (why == DeviceLine::End::Fell && m_session->fedSinceTake) {
        m_session->fell = why;
        m_session->fellAt = now();
        return;
    }
    finishForTheLine(why);
}

void RfbScreen::finishForTheLine(DeviceLine::End why)
{
    const auto [code, message] = saidOfTheLine(why, m_session != nullptr && m_session->readyAt >= 0, m_limits.thing);
    m_forTheLine = why;
    finish(code, message);
}

void RfbScreen::leave()
{
    if (m_session == nullptr) {
        return;
    }
    std::unique_ptr<Session> session = std::move(m_session);
    disconnect(&session->line, nullptr, this, nullptr);
    session->reader->detach();
    m_paces.stop();
    m_shareWait.stop();
    if (!session->over && session->line.isIn()) {
        // What this program has down on the server is let go of first.
        letGoOfWhatIsDown(*session);
        // RFB has no goodbye: the WebSocket's own is said, and the line kept until it has gone (its seat is not free
        // before), beyond this object if need be, on the thread it lives on.
        session->line.close();
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

void RfbScreen::finish(const QString& code, const QString& message)
{
    // A session that served for a while and then ended is no failure: what failed before counts no more, and the next
    // is asked for at once (not one that stopped answering, which is a server that hangs).
    const bool served = m_session != nullptr && m_session->readyAt >= 0 && m_session->updates > 0 &&
                        now() - m_session->readyAt >= m_limits.settledMs;
    const qint64 lasted = m_session != nullptr && m_session->readyAt >= 0 ? now() - m_session->readyAt : -1;
    // How it ended, for whoever hears `ended` (lastEnded): a line that fell by itself after it was let in is told from
    // one the server closed with its goodbye (DeviceLine::saidGoodbye) and from every other end.
    const std::optional<DeviceLine::End> forTheLine = std::exchange(m_forTheLine, std::nullopt);
    m_lastEnded =
        m_session == nullptr  ? Ended::NoLine
        : m_session->inAt < 0 ? Ended::NotLetIn
        : forTheLine == std::optional<DeviceLine::End>(DeviceLine::End::Fell) && !m_session->line.saidGoodbye()
            ? Ended::Fell
            : Ended::Otherwise;
    leave();
    m_picture = {}; // (the screen is not kept beyond the line it came over)
    m_ticks.stop();
    if (code.isEmpty()) {
        m_refusal.reset();
        m_failures = 0;
    } else {
        m_refusal = error(code, message);
        if (m_hold) {
            // The family's own ladder.
            m_retryAt = now() + std::max<qint64>(0, m_hold(code, served, lasted));
        } else if (served && code != u"timeout"_s) {
            m_failures = 0;
            m_retryAt = now();
        } else {
            ++m_failures;
            const auto rung =
                static_cast<std::size_t>(std::min<int>(m_failures, static_cast<int>(m_limits.retryMs.size())) - 1);
            m_retryAt = now() + m_limits.retryMs[rung];
        }
    }
    m_lost = code.isEmpty() ? base::Error{"dropped", "The server's console was given back"} : error(code, message);
    const std::vector<StillAsked> stills = std::exchange(m_stillsAsked, {});
    emit ended(code, message);
    const base::Error why =
        code.isEmpty() ? error(u"no_stream"_s, u"The server's console was given back"_s) : error(code, message);
    for (const StillAsked& one : stills) {
        one.done(why);
    }
}

void RfbScreen::still(int width, StillAnswer done)
{
    if (m_session == nullptr || m_session->shown == 0) {
        QTimer::singleShot(
            0, this, [done = std::move(done)] { done(error(u"no_stream"_s, u"Waiting for the server's screen"_s)); });
        return;
    }
    const std::uint64_t number = ++m_stillsMade;
    m_stillsAsked.push_back({number, std::move(done)});
    m_session->reader->still(width, number);
}

void RfbScreen::close()
{
    m_closed = true;
    m_asking = false; // (an answer that comes after this finds nobody waiting for it, and is given back at once)
    leave();
    m_picture = {};
    m_ticks.stop();
    m_inputTicks.stop();
    m_typingSteps.stop();
    m_lost = base::Error{"dropped", "The server's console was closed"};
    const std::vector<StillAsked> stills = std::exchange(m_stillsAsked, {});
    const std::vector<Asking> askings = std::exchange(m_askings, {});
    std::deque<std::unique_ptr<Typing>> typings = std::exchange(m_typings, {});
    for (const StillAsked& one : stills) {
        one.done(m_lost);
    }
    for (const Asking& asking : askings) {
        asking.answer(m_lost);
    }
    for (const std::unique_ptr<Typing>& typing : typings) {
        typing->done(m_lost);
    }
}

// --- keys and the pointer -------------------------------------------------------------------------------------------

void RfbScreen::askForInput(InputAnswer answer)
{
    askForInput(std::move(answer), 0);
}

void RfbScreen::askForInput(InputAnswer answer, std::uint64_t typing)
{
    if (m_closed) {
        QTimer::singleShot(0, this, [answer = std::move(answer), why = m_lost] { answer(why); });
        return;
    }
    static_cast<void>(look()); // (a session is opened for it where none is)
    m_askings.push_back({std::move(answer), now() + m_limits.inputWaitMs, typing});
    m_inputTicks.start(m_limits.tickMs);
    // (Answered a turn of the loop later at the soonest: whoever asks hears it from outside the asking.)
    QTimer::singleShot(0, this, [this] { answerInput(); });
}

void RfbScreen::answerInput()
{
    const qint64 at = now();
    const bool taking =
        m_session != nullptr && m_session->readyAt >= 0 && at >= m_session->readyAt + m_limits.inputAfterMs;
    // A session that came to nothing, and none asked for before its time: what it came to is the answer.
    const bool refused = m_session == nullptr && !m_asking && m_leaving == nullptr && m_refusal && at < m_retryAt;
    std::vector<std::pair<InputAnswer, base::Result<Turn>>> answers;
    for (auto asking = m_askings.begin(); asking != m_askings.end();) {
        if (taking) {
            answers.emplace_back(std::move(asking->answer), m_session->turn);
        } else if (refused) {
            answers.emplace_back(std::move(asking->answer), *m_refusal);
        } else if (at >= asking->until) {
            answers.emplace_back(std::move(asking->answer), waitingForTheConsole());
        } else {
            ++asking;
            continue;
        }
        asking = m_askings.erase(asking);
    }
    const QPointer<RfbScreen> self(this);
    for (auto& [answer, turn] : answers) {
        answer(std::move(turn));
        if (self == nullptr) {
            return;
        }
    }
}

base::Result<void> RfbScreen::inTurn(Turn turn) const
{
    if (m_session != nullptr && m_session->readyAt >= 0 && m_session->turn == turn) {
        return {};
    }
    return m_lost;
}

base::Result<void> RfbScreen::keyboard(const typed::Report& keys, Turn turn)
{
    if (const base::Result<void> still = inTurn(turn); !still) {
        return still;
    }
    m_wantedAt = now();
    Session& session = *m_session;
    const QByteArray said = eventsOf(changesOf(session.keysThere, keys));
    session.keysThere = keys;
    if (!said.isEmpty() && !say(said)) {
        return m_lost;
    }
    return {};
}

base::Result<void> RfbScreen::mouse(std::uint8_t buttons, int x, int y, int wheel, Turn turn)
{
    if (const base::Result<void> still = inTurn(turn); !still) {
        return still;
    }
    m_wantedAt = now();
    Session& session = *m_session;
    // At its place in the screen's pixels (as the web's RfbHid works it out), with RFB's buttons.
    const auto pixel = [](int at, int side) {
        return static_cast<int>(static_cast<long long>(std::clamp(at, 0, farthest)) * std::max(side - 1, 0) / farthest);
    };
    session.place = {pixel(x, session.width), pixel(y, session.height)};
    session.buttonsDown = rfbButtons(buttons);
    QByteArray said;
    if (wheel != 0) {
        // A notch: its button pressed and let go of.
        const auto notch = static_cast<std::uint8_t>(session.buttonsDown | (wheel > 0 ? 8 : 16));
        said += bytesOf(rfb::pointerEvent(session.place.first, session.place.second, notch));
    }
    said += bytesOf(rfb::pointerEvent(session.place.first, session.place.second, session.buttonsDown));
    if (!say(said)) {
        return m_lost;
    }
    return {};
}

void RfbScreen::letGoOfWhatIsDown(Session& session)
{
    QByteArray said = eventsOf(changesOf(session.keysThere, {}));
    if (session.buttonsDown != 0) {
        said += bytesOf(rfb::pointerEvent(session.place.first, session.place.second, 0));
    }
    session.keysThere = {};
    session.buttonsDown = 0;
    if (!said.isEmpty()) {
        static_cast<void>(session.line.sayBytes(said));
    }
}

void RfbScreen::press(const QString& keys, Still still, Typed done)
{
    auto typing = std::make_unique<Typing>();
    typing->still = std::move(still);
    typing->done = std::move(done);
    // (A VNC server's refusal of a key there is none of in the web's words, its key_events'.)
    const base::Result<typed::Report> combination =
        typed::combination(keys, u"The server's console"_s,
                           m_limits.thing.isEmpty() ? QString() : u"There is no %1 key to send to a VNC server"_s);
    std::vector<Event> down;
    if (combination) {
        const typed::Report& report = combination.value();
        std::vector<int> usages;
        for (int bit = 0; bit < 8; ++bit) {
            if ((report.modifiers >> bit & 1) != 0) {
                usages.push_back(0xE0 + bit);
            }
        }
        usages.insert(usages.end(), report.keys.begin(), report.keys.end());
        for (const int usage : usages) {
            const std::optional<std::uint32_t> keysym = keysymOfKey(usage);
            if (!keysym) {
                QTimer::singleShot(0, this, [done = std::move(typing->done)] {
                    done(base::Error{"invalid", "There is no such key to send to the server's console"});
                });
                return;
            }
            down.emplace_back(*keysym, true);
        }
    }
    if (!combination || down.empty()) {
        QTimer::singleShot(
            0, this,
            [done = std::move(typing->done), why = combination ? base::Error{"invalid", "Not a valid key combination"}
                                                               : combination.error()] { done(why); });
        return;
    }
    // Pressed in its order, and let go of in the other.
    typing->events = down;
    for (auto key = down.rbegin(); key != down.rend(); ++key) {
        typing->events.emplace_back(key->first, false);
    }
    queueTyping(std::move(typing));
}

void RfbScreen::type(const QString& text, Still still, Typed done, bool thenEnter)
{
    auto typing = std::make_unique<Typing>();
    typing->still = std::move(still);
    typing->done = std::move(done);
    // Enter after the text, in the text's own typing and turn (outside review 2.3): its keysym, Return, pressed and let
    // go of after the text's last character, as the web's text_events types the "\n" it puts there; nothing held.
    static constexpr std::uint32_t returnKey = 0xFF0D;
    const auto enterAfter = [thenEnter](Typing& made) {
        if (thenEnter) {
            made.events.emplace_back(returnKey, true);
            made.events.emplace_back(returnKey, false);
        }
    };
    if (!m_limits.usShift) {
        // The server's own keysyms (the web's text_events, us_shift False): counted as the web counts a text, a "\r\n"
        // one Enter, each character pressed and let go of by its keysym. Where, and never what: the text may be a
        // saved secret. Half of a character by itself is one that is typed by no keysym (the web's keysym_of): kept
        // as it is (Qt's toUcs4 would make it U+FFFD, which is typed), and a pair of halves one character.
        QList<char32_t> characters;
        for (qsizetype at = 0; at < text.size(); ++at) {
            const QChar one = text.at(at);
            if (one.isHighSurrogate() && at + 1 < text.size() && text.at(at + 1).isLowSurrogate()) {
                characters.append(QChar::surrogateToUcs4(one, text.at(at + 1)));
                ++at;
            } else {
                characters.append(one.unicode());
            }
        }
        if (characters.isEmpty() || characters.size() > typed::mostCharacters) {
            QTimer::singleShot(0, this, [done = std::move(typing->done)] {
                done(base::Error{"invalid",
                                 u"Text must be 1 to %1 characters"_s.arg(typed::mostCharacters).toStdString()});
            });
            return;
        }
        int place = 0;
        for (qsizetype at = 0; at < characters.size(); ++at) {
            if (characters[at] == U'\r' && at + 1 < characters.size() && characters[at + 1] == U'\n') {
                continue; // (the "\n" that follows is its Enter)
            }
            ++place;
            const std::optional<std::uint32_t> keysym = rfb::keysymOf(characters[at]);
            if (!keysym) {
                QTimer::singleShot(0, this, [done = std::move(typing->done), place] {
                    done(base::Error{
                        "invalid",
                        u"Character %1 of the text cannot be typed on a VNC server"_s.arg(place).toStdString()});
                });
                return;
            }
            typing->events.emplace_back(*keysym, true);
            typing->events.emplace_back(*keysym, false);
        }
        enterAfter(*typing);
        queueTyping(std::move(typing));
        return;
    }
    const base::Result<std::vector<typed::Report>> reports = typed::text(text);
    if (!reports) {
        QTimer::singleShot(0, this, [done = std::move(typing->done), why = reports.error()] { done(why); });
        return;
    }
    // Each character by its keysym on the US layout, Shift down around it where that layout types it with Shift.
    constexpr std::uint32_t shiftLeft = 0xFFE1;
    for (const typed::Report& report : reports.value()) {
        const bool shifted = (report.modifiers & 0x22) != 0;
        const std::optional<std::uint32_t> keysym =
            report.keys.empty() ? std::nullopt : rfb::keysymFor(report.keys.front(), shifted);
        if (!keysym) {
            QTimer::singleShot(0, this, [done = std::move(typing->done)] {
                done(base::Error{"invalid", "A character of the text cannot be typed on a US keyboard layout"});
            });
            return;
        }
        if (shifted) {
            typing->events.emplace_back(shiftLeft, true);
        }
        typing->events.emplace_back(*keysym, true);
        typing->events.emplace_back(*keysym, false);
        if (shifted) {
            typing->events.emplace_back(shiftLeft, false);
        }
    }
    enterAfter(*typing);
    queueTyping(std::move(typing));
}

void RfbScreen::queueTyping(std::unique_ptr<Typing> typing)
{
    typing->number = ++m_typingsMade;
    m_typings.push_back(std::move(typing));
    if (m_typings.size() == 1) {
        QTimer::singleShot(0, this, [this] { typingStep(); });
    }
}

void RfbScreen::typingStep()
{
    if (m_typings.empty()) {
        return;
    }
    Typing& typing = *m_typings.front();
    if (!typing.asked) {
        if (!typing.still()) {
            endTyping(stoppedTyping());
            return;
        }
        typing.asked = true;
        // (Told by its number, not by where it stands: one made after it was stopped may stand there by then. Review
        // cx6-1, L2.)
        const std::uint64_t asking = typing.number;
        askForInput(
            [this, asking](base::Result<Turn> turn) {
                if (m_typings.empty() || m_typings.front()->number != asking) {
                    return; // (it ended meanwhile)
                }
                if (!turn) {
                    endTyping(turn.error());
                    return;
                }
                m_typings.front()->turn = turn.value();
                typingStep();
            },
            asking);
        return;
    }
    if (typing.turn < 0) {
        return; // (its keyboard is waited for)
    }
    if (const base::Result<void> still = inTurn(typing.turn); !still) {
        endTyping(still);
        return;
    }
    // Asked before every piece: whoever asked may no longer have it typed by now. A piece is whole presses and
    // releases, and leaves nothing down.
    if (!typing.still()) {
        endTyping(stoppedTyping());
        return;
    }
    std::vector<std::uint32_t> held;
    std::size_t end = typing.next;
    while (end < typing.events.size()) {
        if (held.empty() && end - typing.next >= static_cast<std::size_t>(m_limits.keysAtOnce)) {
            break;
        }
        const auto& [keysym, down] = typing.events[end];
        if (down) {
            held.push_back(keysym);
        } else {
            std::erase(held, keysym);
        }
        ++end;
    }
    const QByteArray said = eventsOf(std::span<const Event>(typing.events).subspan(typing.next, end - typing.next));
    typing.next = end;
    m_wantedAt = now();
    if (!say(said)) {
        endTyping(m_lost);
        return;
    }
    if (typing.next >= typing.events.size()) {
        endTyping({});
        return;
    }
    m_typingSteps.start(m_limits.piecePauseMs);
}

void RfbScreen::endTyping(const base::Result<void>& how)
{
    if (m_typings.empty()) {
        return;
    }
    const std::unique_ptr<Typing> ended = std::move(m_typings.front());
    m_typings.pop_front();
    const QPointer<RfbScreen> self(this);
    ended->done(how);
    if (self != nullptr && !m_typings.empty()) {
        QTimer::singleShot(0, this, [this] { typingStep(); });
    }
}

void RfbScreen::stopTyping()
{
    m_typingSteps.stop();
    std::deque<std::unique_ptr<Typing>> typings = std::exchange(m_typings, {});
    // Their askings for the keyboard go with them, as a server's screen has it: nobody waits for those, and they would
    // keep a console made and held for nobody, up to the time keys wait (review cx6-1, L2).
    std::erase_if(m_askings, [](const Asking& asking) { return asking.typing != 0; });
    if (m_askings.empty()) {
        m_inputTicks.stop();
    }
    for (const std::unique_ptr<Typing>& typing : typings) {
        typing->done(stoppedTyping());
    }
}

} // namespace qkvm::core
