#include <qkvm/core/device_line.h>
#include <qkvm/core/video_diagnostics.h>

#include <algorithm>
#include <array>
#include <utility>

#include <QAbstractEventDispatcher>
#include <QAbstractNativeEventFilter>
#include <QCryptographicHash>
#include <QEvent>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QStringDecoder>
#include <QThread>
#include <QTimer>

#include <qkvm/core/kvm_certificate.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/wiped.h>

#include "line_threads.h"

#include <winsock2.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The room Windows gets for what this line has said and the KVM has not taken yet: some fifty
// keys' worth. (Windows' own choice is sixty-four kilobytes and more.)
constexpr int roomWithWindowsBytes = 4096;

// The most that is read of an answer to the asking, and the most messages a second a KVM has
// reason to send.
constexpr qsizetype longestHead = 8192;
// What may wait to be read: a whole message's frame, and never less than the longest answer that
// lets a line in, which is read through the same buffer (a smaller one never held all of it).
qint64 readBufferFor(const DeviceLine::Limits& limits)
{
    return std::max<qint64>(limits.largestFrameBytes + 14, longestHead + 4);
}
// What says "this is a WebSocket" in the answer, by the rule of WebSocket itself.
constexpr const char* webSocketMark = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// The kinds of message that are sent from here, by WebSocket's numbers for them, and its number
// for a goodbye with nothing wrong.
constexpr quint8 kindText = 1;
constexpr quint8 kindBytes = 2;
constexpr quint8 kindGoodbye = 8;
constexpr quint8 kindAreYouThere = 9;
constexpr quint8 kindIAm = 10;
constexpr quint16 nothingWrong = 1000;

// How many bytes a message of this length is on the line: how it begins, the four bytes it is
// hidden with, and itself.
constexpr qint64 onTheLine(qsizetype length)
{
    return length + (length <= 125 ? 6 : length <= 0xFFFF ? 8 : 14);
}

// One whole message the way a caller has to send it: hidden with four bytes that nobody could
// have known, new ones for each.
QByteArray message(quint8 kind, const QByteArray& what)
{
    const auto length = static_cast<quint64>(what.size());
    QByteArray whole;
    whole.reserve(onTheLine(what.size()));
    whole += static_cast<char>(0x80 | kind);
    if (length <= 125) {
        whole += static_cast<char>(0x80 | length);
    } else if (length <= 0xFFFF) {
        whole += static_cast<char>(0x80 | 126);
        whole += static_cast<char>((length >> 8) & 0xFF);
        whole += static_cast<char>(length & 0xFF);
    } else {
        whole += static_cast<char>(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            whole += static_cast<char>((length >> shift) & 0xFF);
        }
    }
    const quint32 hide = QRandomGenerator::system()->generate();
    for (int shift = 24; shift >= 0; shift -= 8) {
        whole += static_cast<char>((hide >> shift) & 0xFF);
    }
    for (qsizetype index = 0; index < what.size(); ++index) {
        const auto mask = static_cast<quint8>((hide >> (24 - 8 * (index % 4))) & 0xFF);
        whole += static_cast<char>(static_cast<quint8>(what.at(index)) ^ mask);
    }
    return whole;
}

// Whether the asking can carry these as they are (DeviceLine::Where says what that takes). Both
// are written into it raw: a line's end in either would make a header of somebody else's choosing
// out of it, or a second asking.
bool fitToSend(const DeviceLine::Where& where, bool live)
{
    const auto visible = [](char one) { return one > ' ' && one < '\x7F'; };
    const auto ofAName = [](char one) {
        return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') || one == '_' ||
               one == '-';
    };
    const qsizetype sign = where.cookie.indexOf('=');
    if (live && !where.cookie.isEmpty()) {
        return false; // Server authentication is the first message, never a header.
    }
    const bool askingFits = where.asking.startsWith('/') && std::ranges::all_of(where.asking, visible);
    if (!where.authorization.isEmpty()) {
        return !live && !where.withoutCookie && where.cookie.isEmpty() && where.authToken.isEmpty() &&
               where.authorization.size() <= 512 && std::ranges::all_of(where.authorization, visible) && askingFits;
    }
    if (!where.authToken.isEmpty()) {
        const auto handed = [](char one) {
            return one > ' ' && one < '\x7F' && one != '"' && one != ',' && one != ';' && one != '\\';
        };
        return !live && !where.withoutCookie && where.cookie.isEmpty() && where.authToken.size() <= 512 &&
               std::ranges::all_of(where.authToken, handed) && askingFits;
    }
    if (where.withoutCookie) {
        // (Nothing to say who asks, and nothing in its place either.)
        return where.cookie.isEmpty() && askingFits;
    }
    const bool cookieFits = live || (sign > 0 && std::ranges::all_of(where.cookie.first(sign), ofAName) &&
                                     kvmd::isToken(where.cookie.sliced(sign + 1)));
    return askingFits && cookieFits;
}

} // namespace

// --- the socket's thread ------------------------------------------------------------------------------------------

// What the line and its socket's thread share, each touching it under its lock alone, and never longer than it takes
// to copy what it hands over: nothing of Qt's socket is called with it held (any call may judge a certificate).
struct DeviceLine::Shared {
    Shared() = default;
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;
    Shared(Shared&&) = delete;
    Shared& operator=(Shared&&) = delete;
    // (What says who asks and never went, where both let go of this with it still here, is written over as it goes.)
    ~Shared()
    {
        for (Said& one : out) {
            if (one.wiped) {
                wipeInPlace(one.bytes);
            }
        }
    }

    QMutex mutex;
    // The line, for what its socket's thread hands it; none once it is gone (its destructor), and nothing is handed
    // to it after that.
    DeviceLine* line = nullptr;
    // What was said and not taken by the socket's thread yet, in its order; each with whether that thread writes it
    // over once Qt has made a copy of its own (writeCopied: who asks), and whether it is held to Limits::lateAfterMs:
    // all but the asking, which is no input and is bounded by Limits::letInWithinMs (outside review of tls, L1: it
    // waits behind a judgement of the certificate after the handshake on that thread, and was ended Late before the
    // line was let in).
    struct Said {
        QByteArray bytes;
        bool wiped = false;
        qint64 atMs = 0; // when it was said, by `clock`
        bool timed = true;
    };
    QList<Said> out;
    // A clock both read (started at open(); only read after that).
    QElapsedTimer clock;
    // Of all that was ever said, what that thread handed to its socket; and what the socket still held of what it was
    // handed when it last looked (in the clear and, over TLS, hidden already). What waits in this program is the rest
    // of what was said, and that.
    qint64 taken = 0;
    qint64 inSocket = 0;
    bool writeAsked = false; // that thread is told to take what was said, and has not yet
    // The line was reset from here: nothing more of what was said goes, and that thread lets go of the socket at once,
    // also of what Windows holds for it (or the moment Windows lets that thread go, if it holds it).
    bool resetAsked = false;
    // That thread found something it was handed waiting longer than Limits::lateAfterMs, and reset the socket for it
    // there and then: the line is late, whatever its own thread was doing meanwhile (and is told so).
    bool lateFound = false;
    // What came: taken off the socket by that thread, not by the line yet; what the line took of it and has not read
    // yet; the most the two hold together (more waits in the socket, and with Windows); whether the line is told that
    // something came and has not taken it yet; and whether that thread left something in the socket for want of room
    // (said only where, under this lock, there was none: Wire::take; the line, giving room back, asks for more where it
    // finds this said: gaveRoom).
    QByteArray in;
    qint64 lineHolds = 0;
    qint64 room = 0;
    bool cameTold = false;
    bool starved = false;
    // How the socket stood when that thread last looked, and what was found wrong with the certificate it was shown.
    QAbstractSocket::SocketState state = QAbstractSocket::UnconnectedState;
    QAbstractSocket::SocketError error = QAbstractSocket::UnknownSocketError;
    std::optional<End> objection;
};

// The line's socket, on the line's own thread for its whole life (lines::aThreadOfItsOwn): its TCP and its TLS, every
// judgement of the certificate the device shows, and the driver's rule on what Windows objects to, are there. It is
// told what to do by posted calls, in the order the line made them, and hands what it comes to to the line the same
// way; it ends when the line tells it to (the line's destructor), and its thread with it.
class DeviceLine::Wire : public QObject {
public:
    struct Setup {
        QString host;
        quint16 port = 0;
        bool encrypted = false;
        bool known = false;              // over TLS, known by a certificate whose SHA-256 the line holds
        bool knownByCertificate = false; // Where's own, for the driver's rule
        QByteArray certificate;          // that SHA-256
        qint64 readBuffer = 0;           // what the socket may hold of what came (readBufferFor), on every line
        qint64 mostBytesASecond = 0;     // what is taken off a line thrown away after its goodbye, before it goes
        int lateAfterMs = 0;             // Limits::lateAfterMs
    };

    Wire(std::shared_ptr<Shared> shared, Setup setup) : m_shared(std::move(shared)), m_setup(std::move(setup)) {}

    // The socket made, and the line asked for.
    void begin();
    // What was said, handed to the socket and pushed out at once.
    void write();
    // Let go of at once, Windows' part too (SO_LINGER 0, as the line has always reset).
    void reset();
    // RFB's end (a raw line closed from here): behind what was said.
    void disconnectFromHost();
    // The line has room for what waits in the socket again.
    void room();
    // The last: what was said goes, and the socket with it (a line thrown away after its goodbye has what came taken
    // off it first, `drain`), this, and the thread.
    void end(bool drain);

private:
    // What was written up to a place in all this thread handed its socket, and when it was said.
    struct Written {
        qint64 upTo = 0;
        qint64 atMs = 0;
    };

    // One taking of what was said: false where nothing waited (or nothing may go any more).
    bool writeOnce();
    // What was taken is given to the socket, one message at a time and only while the socket holds nothing, each
    // looked at first; and the socket's own count told to the line.
    void handOn();
    // What was taken and not given to the socket yet, let go of (written over where it says who asks).
    void letGoPending();
    // What came, into what is shared, as far as there is room.
    void take();
    // How the socket stands, into what is shared.
    void look();
    void fell();
    // How much of all that was said this thread has handed Windows, for a test (lines::whatWasHanded): all it gave the
    // socket, less what the socket still holds; said no more once the socket was let go of (m_letGo).
    void sayHanded();
    // Whether something has waited to leave for longer than may: what waits here, what the socket holds, and what
    // left after it had waited so (what has left is forgotten); and the watch on that, set for when the oldest of what
    // waits would be late.
    bool late();
    void watch();
    // The oldest of what waits here that is held to the limit (Shared::Said::timed); none where none is.
    const Shared::Said* oldestTimed() const;
    // Late: the socket is reset at once, nothing more goes, and the line is told.
    void foundLate();
    // Before each message this thread's loop takes from Windows (a socket's, a timer's: BeforeQt), and before each call
    // posted to the socket (Qt's own, as its flush of what the socket holds: eventFilter), where anything waits: Qt
    // hands what a socket holds to Windows from inside such a one, the moment Windows has room, and what has waited too
    // long is found late here first; a line reset from its own thread is reset here. (Qt's loop hands out calls posted
    // to this thread without asking the native filter first: outside review of tls, L2. This Wire's own posted calls
    // look for themselves.) `message`: Windows' (an MSG), none for a posted call.
    void lookBeforeQt(const void* message);
    bool eventFilter(QObject* watched, QEvent* event) override;
    class BeforeQt;
    bool resetAsked() const;
    // Has `heard` run on the line's thread, with the line, where the line is still there. (With the lock held.)
    template <typename Heard>
    void tellLocked(Heard heard);
    template <typename Heard>
    void tell(Heard heard)
    {
        const QMutexLocker locked(&m_shared->mutex);
        tellLocked(std::move(heard));
    }

    std::shared_ptr<Shared> m_shared;
    Setup m_setup;
    std::unique_ptr<QSslSocket> m_socket;
    bool m_fell = false;
    bool m_letGo = false; // the socket was reset or ended here: what it held was thrown away
    // What was taken of what was said and not given to the socket yet, in its order (handOn).
    QList<Shared::Said> m_pending;
    bool m_handing = false;         // handOn under way: what the socket says from inside it is looked at, no more
    bool m_disconnectAsked = false; // a raw line closed from here: its connection ended once all was given over
    // All this thread ever handed its socket; and of what may not have left yet, up to where and when it was said.
    qint64 m_handed = 0;
    QList<Written> m_waiting;
    std::unique_ptr<QTimer> m_lateWatch;  // made on this thread, in begin()
    std::unique_ptr<BeforeQt> m_beforeQt; // this thread's loop's own, from begin() to end()
};

// What this thread's loop takes from Windows is looked at before Qt has it (lookBeforeQt): installed on the loop of
// this thread alone, as each line has a thread of its own.
class DeviceLine::Wire::BeforeQt final : public QAbstractNativeEventFilter {
public:
    explicit BeforeQt(Wire& wire) : m_wire(wire) {}
    BeforeQt(const BeforeQt&) = delete;
    BeforeQt& operator=(const BeforeQt&) = delete;
    BeforeQt(BeforeQt&&) = delete;
    BeforeQt& operator=(BeforeQt&&) = delete;
    ~BeforeQt() override = default;

    bool nativeEventFilter(const QByteArray& /*kind*/, void* message, qintptr* /*result*/) override
    {
        if (lines::looksBeforeQt()) { // (always, but in a test of what is behind it)
            m_wire.lookBeforeQt(message);
        }
        return false; // (Qt has it all the same)
    }

private:
    Wire& m_wire;
};

template <typename Heard>
void DeviceLine::Wire::tellLocked(Heard heard)
{
    // (Posted to the line, which lives on: one gone takes what was posted to it with it, unheard, and nothing is posted
    // after its destructor took the lock to say it is gone.)
    if (DeviceLine* const line = m_shared->line) {
        QMetaObject::invokeMethod(line, [line, heard = std::move(heard)] { heard(*line); }, Qt::QueuedConnection);
    }
}

bool DeviceLine::Wire::resetAsked() const
{
    const QMutexLocker locked(&m_shared->mutex);
    return m_shared->resetAsked;
}

void DeviceLine::Wire::begin()
{
    m_socket = std::make_unique<QSslSocket>();
    QSslSocket* const socket = m_socket.get();
    socket->setProxy(QNetworkProxy::NoProxy);
    if (QAbstractEventDispatcher* const loop = QAbstractEventDispatcher::instance()) {
        m_beforeQt = std::make_unique<BeforeQt>(*this);
        loop->installNativeEventFilter(m_beforeQt.get());
    }
    socket->installEventFilter(this); // (Qt's own calls posted to it: eventFilter)
    // (To the millisecond, as the line's own: what waits is found late at lateAfterMs, not a twentieth of it later,
    // when Qt may have handed it on.)
    m_lateWatch = std::make_unique<QTimer>();
    m_lateWatch->setSingleShot(true);
    m_lateWatch->setTimerType(Qt::PreciseTimer);
    connect(m_lateWatch.get(), &QTimer::timeout, this, [this] {
        if (late()) {
            foundLate();
        } else {
            watch();
        }
    });
    // Every line's socket holds no more of what came than a message's worth, whatever the line reads or throws away:
    // without a bound Qt takes all Windows has, whether or not the line takes it, and a device that sends faster than
    // the line takes grows this program's memory at the link's rate for as long as the interface thread is busy
    // (outside review of tls, M1: kvmd's console line and awake line). Bounded, Windows' window fills and the device is
    // held back by TCP, as it was while the socket was on the interface thread. (Over TLS the bound holds for what came
    // hidden as well, and Qt does not always take from Windows again by itself once it is reached: take says when.)
    socket->setReadBufferSize(m_setup.readBuffer);
    if (m_setup.encrypted && m_setup.known) {
        // Known by its certificate alone, as the driver has it for its questions: Windows
        // vouches for nobody on this line and objects to whatever is shown, the KVM's own
        // included.
        socket->setSslConfiguration(kvm::knownByItsCertificateAlone(QSslConfiguration::defaultConfiguration()));
    }
    connect(socket, &QSslSocket::sslErrors, this, [this](const QList<QSslError>& errors) {
        // Windows objects to what the KVM showed: at the handshake, and at every judgement after it (a session ticket
        // of TLS 1.3, a key update, a renegotiation: Qt judges anew at each). What comes of that is the driver's rule,
        // by the driver's code: passed over for the certificate the KVM is known by where every objection names that
        // one itself, and for nothing else. The line then ends by itself. (Nothing is passed over on a line reset from
        // here: it is let go of.)
        if (resetAsked()) {
            return;
        }
        std::optional<End> objection;
        switch (kvm::objectedTo(m_socket->peerCertificate(), errors, m_setup.knownByCertificate, m_setup.certificate)) {
        case kvm::Objected::PassedOver:
            m_socket->ignoreSslErrors(errors);
            return;
        case kvm::Objected::BySigner:
            objection = End::CertificateOfUnknownSigner;
            break;
        case kvm::Objected::Changed:
            objection = End::CertificateChanged;
            break;
        case kvm::Objected::Untrusted:
            objection = End::CertificateUntrusted;
            break;
        }
        const QMutexLocker locked(&m_shared->mutex);
        m_shared->objection = objection;
    });
    connect(socket, &QSslSocket::encrypted, this, [this] {
        // The two have agreed on how to speak, and nothing has been written: here the look is
        // made for every line, also for one whose certificate Windows had nothing against.
        const QSslCertificate shown = m_socket->peerCertificate();
        const QByteArray digest = shown.isNull() ? QByteArray() : shown.digest(QCryptographicHash::Sha256);
        const bool changed = m_setup.known && digest != m_setup.certificate;
        const QByteArray der = changed ? QByteArray() : shown.toDer();
        look();
        tell([der, changed](DeviceLine& line) { line.wireEncrypted(der, changed); });
    });
    connect(socket, &QSslSocket::connected, this, [this] {
        // Each message goes out when it is said. Left to itself, Windows holds a short one back
        // until the KVM has said that it got the one before: a key that comes late, a pointer
        // that moves in jumps. (Set, and not tried: on this PC's own address nothing is held
        // back either way, so no test here can tell.)
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        // And Windows is given little room to hold what was said: what it holds is out of this
        // program's sight (see say()), and a key that waits there is one that may arrive late.
        m_socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, roomWithWindowsBytes);
        look();
        if (!m_setup.encrypted) {
            tell([](DeviceLine& line) { line.wireConnected(); });
        }
    });
    connect(socket, &QSslSocket::readyRead, this, [this] { take(); });
    // Windows took something of what the socket held (in the clear, or hidden): what left is looked at as it leaves
    // (late), and the socket is given the next of what waits here, if it holds nothing now.
    const auto written = [this] {
        look();
        if (m_handing) {
            watch(); // (from inside handOn, which goes on itself)
        } else {
            handOn();
        }
    };
    connect(socket, &QSslSocket::bytesWritten, this, written);
    connect(socket, &QSslSocket::encryptedBytesWritten, this, written);
    connect(socket, &QSslSocket::stateChanged, this, [this] { look(); });
    connect(socket, &QSslSocket::disconnected, this, [this] { fell(); });
    connect(socket, &QSslSocket::errorOccurred, this, [this] { fell(); });
    {
        const QMutexLocker locked(&m_shared->mutex);
        if (m_shared->resetAsked || m_shared->line == nullptr) {
            return; // (ended, or gone, before it was begun: nothing is asked for)
        }
    }
    if (m_setup.encrypted) {
        socket->connectToHostEncrypted(m_setup.host, m_setup.port);
    } else {
        socket->connectToHost(m_setup.host, m_setup.port);
    }
}

void DeviceLine::Wire::write()
{
    // What was said is taken a few times over while more comes, and the line is told to hand more over (writeAsked
    // false) only once nothing waits: a line said much to in a breath posts this once, not once for each word. After
    // so many takings this goes back to its loop for the rest (the line may say faster than it is taken, and what comes
    // is read in between), and asks itself to come back.
    constexpr int takingsAtATime = 8;
    for (int taking = 0; taking < takingsAtATime; ++taking) {
        if (!writeOnce()) {
            return;
        }
    }
    QMetaObject::invokeMethod(this, [this] { write(); }, Qt::QueuedConnection);
}

bool DeviceLine::Wire::writeOnce()
{
    bool came = false;
    bool wasReset = false;
    {
        const QMutexLocker locked(&m_shared->mutex);
        // (No socket: this is ending, or never began: Wire::end. What waited went with the line, or nothing waits: the
        // line hands the next over itself.)
        wasReset = m_shared->resetAsked;
        if (m_socket == nullptr || m_shared->resetAsked || m_shared->lateFound || m_shared->out.isEmpty()) {
            m_shared->writeAsked = false;
        } else {
            m_pending.append(std::move(m_shared->out));
            m_shared->out.clear();
            came = true;
        }
    }
    if (wasReset) {
        reset(); // (here and now: outside review of tls, L2)
        return false;
    }
    handOn();
    return came;
}

void DeviceLine::Wire::handOn()
{
    if (m_handing || m_socket == nullptr) {
        return;
    }
    m_handing = true;
    qint64 handed = 0;
    bool foundLateHere = false;
    while (!m_pending.isEmpty()) {
        if (resetAsked()) {
            reset(); // (here and now, not at the call posted behind that: outside review of tls, L2)
            break;
        }
        // A socket that has ended (the device closed the line, and the line has not heard of it yet) is given nothing:
        // what waits goes with it, none of it counted as taken, as none of it left (outside review of tls, L6: written
        // into a closed socket, each was "taken", and a key let go of then counted as gone).
        if (const QAbstractSocket::SocketState state = m_socket->state();
            m_fell || (state != QAbstractSocket::ConnectedState && state != QAbstractSocket::ClosingState)) {
            letGoPending();
            break;
        }
        // Nothing is given over behind what has waited too long, nor that: what waits here, what the socket holds, and
        // what left after it had waited so (where this thread was held past it, by Windows judging a certificate).
        if (late()) {
            foundLateHere = true;
            break;
        }
        // The socket is given more only once it holds nothing. What it holds, Qt hands to Windows by itself the moment
        // Windows has room again, at whatever turn of this thread's loop comes first, with no look at how long it
        // waited: before the watch on what waits has had its turn, where this thread was held past it. So it is never
        // given more than one message beyond what Windows takes, and of what waited too long no more than that one can
        // leave (and is found late as it leaves: late()). Each message goes as it is said, as it always went.
        if (m_socket->bytesToWrite() + m_socket->encryptedBytesToWrite() > 0) {
            break;
        }
        Shared::Said one = m_pending.takeFirst();
        if (one.wiped) {
            // (A pointer and a length: Qt copies them into its own buffer, and keeps nothing of this, which is written
            // over at once.)
            m_socket->write(one.bytes.constData(), one.bytes.size());
            wipeInPlace(one.bytes);
        } else {
            m_socket->write(one.bytes);
        }
        handed += one.bytes.size();
        m_handed += one.bytes.size();
        if (!one.timed) {
            // (The asking: not looked at as it leaves. Nothing timed is given the socket behind it before it left.)
        } else if (!m_waiting.isEmpty() && m_waiting.last().atMs == one.atMs) {
            m_waiting.last().upTo = m_handed;
        } else {
            m_waiting.append({m_handed, one.atMs});
        }
        // Handed to Windows now, and not at the next turn of the loop. (What Qt says of it from inside this is looked
        // at there, and may find this late, and the socket reset, and what waited here let go of.)
        m_socket->flush();
    }
    m_handing = false;
    if (foundLateHere) {
        foundLate();
    }
    // What was taken and what the socket still holds of it are told in one breath: told one after the other, the line
    // would for that moment count what the socket holds as having left (hasLeft, a key let go of that has not). (What a
    // socket found late held never left: it is not counted as gone.)
    const qint64 waiting = m_socket->bytesToWrite() + m_socket->encryptedBytesToWrite();
    {
        const QMutexLocker locked(&m_shared->mutex);
        if (!m_shared->resetAsked) {
            m_shared->taken += handed;
            if (!m_shared->lateFound) {
                m_shared->inSocket = waiting;
            }
        }
    }
    // A raw line closed from here ends its connection behind all that was said, once all of it was given over.
    if (m_disconnectAsked && m_pending.isEmpty() && !resetAsked() &&
        m_socket->state() == QAbstractSocket::ConnectedState) {
        m_disconnectAsked = false;
        m_socket->disconnectFromHost();
    }
    look();
    watch();
}

void DeviceLine::Wire::letGoPending()
{
    for (Shared::Said& one : m_pending) {
        if (one.wiped) {
            wipeInPlace(one.bytes);
        }
    }
    m_pending.clear();
}

bool DeviceLine::Wire::late()
{
    if (m_socket == nullptr) {
        return false;
    }
    const qint64 now = m_shared->clock.elapsed();
    const auto tooLong = [this, now](qint64 atMs) { return now - atMs > m_setup.lateAfterMs; };
    // All it was handed, less what its socket still holds (in the clear and hidden), has left: as the line counts it,
    // less is taken to have left than has over TLS, never more. What has left is looked at as it leaves (this is said
    // from where Qt says that Windows took something): where it had waited too long, Qt handed it on by itself, at a
    // turn of this thread's loop that came before the watch's own (this thread held past it). It cannot be called back:
    // the line is late for it, and reset at once, which has Windows throw away whatever of it has not gone.
    const qint64 left = m_handed - (m_socket->bytesToWrite() + m_socket->encryptedBytesToWrite());
    bool leftLate = false;
    while (!m_waiting.isEmpty() && m_waiting.first().upTo <= left) {
        leftLate = leftLate || tooLong(m_waiting.first().atMs);
        m_waiting.removeFirst();
    }
    const Shared::Said* const oldest = oldestTimed();
    return leftLate || (!m_waiting.isEmpty() && tooLong(m_waiting.first().atMs)) ||
           (oldest != nullptr && tooLong(oldest->atMs));
}

const DeviceLine::Shared::Said* DeviceLine::Wire::oldestTimed() const
{
    // (The asking alone is not, and it is the first that is ever said: this looks no further than the second.)
    const auto found = std::ranges::find_if(m_pending, [](const Shared::Said& one) { return one.timed; });
    return found == m_pending.end() ? nullptr : &*found;
}

void DeviceLine::Wire::watch()
{
    if (m_lateWatch == nullptr) {
        return;
    }
    if (late()) {
        foundLate();
        return;
    }
    // (What the socket holds was said before what waits here.)
    const Shared::Said* const timed = oldestTimed();
    const std::optional<qint64> oldest = !m_waiting.isEmpty() ? std::optional(m_waiting.first().atMs)
                                         : timed != nullptr   ? std::optional(timed->atMs)
                                                              : std::nullopt;
    if (!oldest) {
        m_lateWatch->stop();
        return;
    }
    m_lateWatch->start(
        static_cast<int>(std::max<qint64>(1, *oldest + m_setup.lateAfterMs + 1 - m_shared->clock.elapsed())));
}

void DeviceLine::Wire::foundLate()
{
    bool resetThere = false;
    {
        const QMutexLocker locked(&m_shared->mutex);
        if (m_shared->resetAsked) {
            resetThere = true;
        } else if (std::exchange(m_shared->lateFound, true)) {
            return;
        } else {
            // (Let go of here: none of it is handed on.)
            for (Shared::Said& one : m_shared->out) {
                if (one.wiped) {
                    wipeInPlace(one.bytes);
                }
            }
            m_shared->out.clear();
        }
    }
    if (resetThere) {
        // Reset from its own thread meanwhile: reset here at once, and not at the call that says so, which may wait
        // behind what Qt does next (outside review of tls, L2). The line has ended: nothing is told.
        reset();
        return;
    }
    // Left to Qt, what waited would be handed on the moment the way came back, a key from then pressed now: the
    // socket is reset there and then (and what Windows holds for it with it), whatever the line's own thread is doing.
    m_waiting.clear();
    if (m_lateWatch != nullptr) {
        m_lateWatch->stop();
    }
    reset();
    tell([](DeviceLine& line) { line.wireLate(); });
}

void DeviceLine::Wire::lookBeforeQt(const void* message)
{
    // (Not from inside handOn, which looks itself.)
    if (m_socket == nullptr || m_handing || m_socket->state() == QAbstractSocket::UnconnectedState) {
        return;
    }
    lines::lookingBeforeQt(message); // (a test's, where one put this thread aside here: lines::beforeALook)
    // A line reset from its own thread is reset before anything more of it is done here: the call that says so may
    // wait behind this message.
    if (resetAsked()) {
        reset();
        return;
    }
    if ((!m_waiting.isEmpty() || !m_pending.isEmpty()) && late()) {
        foundLate();
        return;
    }
    lines::lookedBeforeQt(message); // (a test's, where one put this thread aside here: lines::afterALook)
}

bool DeviceLine::Wire::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_socket.get() && event->type() == QEvent::MetaCall && lines::looksBeforeQt()) {
        lookBeforeQt(nullptr);
    }
    return false; // (Qt has it all the same)
}

void DeviceLine::Wire::reset()
{
    letGoPending();
    m_waiting.clear();
    if (m_lateWatch != nullptr) {
        m_lateWatch->stop();
    }
    if (m_socket == nullptr) {
        return;
    }
    // What waits in this program to leave goes with the line. What Windows had taken would not:
    // left to itself, Windows goes on sending it for as long as the far end takes it, and a key
    // typed into a line that stalled would be played when the way came back. So the line is
    // reset, not closed: what Windows holds for it is thrown away with it. (That it is reset a
    // test shows, by what the far end's Windows says of the line, and that on this PC nothing
    // more arrives after it. Set, and not shown by a test: a way that itself stalls, which no
    // test on one PC can make. There the far end hears of no reset, and what its own system
    // has taken is its own: a KVM whose program reads again later may play that, and only the
    // watch on the line bounds how much it is.)
    if (const qintptr socket = m_socket->socketDescriptor(); socket != -1) {
        const linger atOnce{.l_onoff = 1, .l_linger = 0};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Winsock takes every option as bytes
        ::setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&atOnce),
                     static_cast<int>(sizeof(atOnce)));
    }
    // (What it handed Windows, said the last time: what it holds goes now.)
    sayHanded();
    m_letGo = true;
    m_socket->abort();
    look();
}

void DeviceLine::Wire::disconnectFromHost()
{
    // (Behind what was said: whatever was said before this was taken by a write posted before this, and is given to the
    // socket as Windows takes it; the connection is ended once all of it has been: handOn. Not at once: Qt's socket in
    // its closing state ends the connection the moment it holds nothing, which is after each message Windows takes
    // whole, before the next is given it.)
    if (resetAsked()) {
        reset(); // (here and now: outside review of tls, L2)
    } else if (m_socket != nullptr) {
        m_disconnectAsked = true;
        handOn();
    }
}

void DeviceLine::Wire::room()
{
    // (What waits in the socket is taken as far as there is room for it now; and where Qt has stopped taking from
    // Windows and would not begin again by itself, it is told to: take.)
    take();
}

void DeviceLine::Wire::take()
{
    if (m_socket == nullptr) {
        return;
    }
    // Whether something is left in the socket for want of room (Shared::starved) is said under the same lock as the
    // room is looked at again then, and is said only where there is none; the line gives room back under that lock,
    // and asks for more only where it finds that said (gaveRoom). So whichever of the two comes last sees the other,
    // and no room the line gives back meanwhile is missed: where there is room by then, more is taken here at once
    // (outside review 2, 2.1: said on the strength of the room looked at before the read, the line could give back all
    // its room in between, find nothing said yet and ask for nothing, and this thread then said it waited and was told
    // nothing more. Qt, its buffer full, takes no more from Windows then, and a console stalled until its watch ended
    // it.)
    while (true) {
        qint64 room = 0;
        {
            const QMutexLocker locked(&m_shared->mutex);
            if (m_shared->resetAsked) {
                return;
            }
            room = m_shared->room - (m_shared->in.size() + m_shared->lineHolds);
        }
        lines::foundRoom(room); // (a test's, where one put this thread aside here: lines::afterTheRoom)
        // (Only this thread adds to what is shared, and the line only takes from it: the room is no less by now.)
        const qint64 there = m_socket->bytesAvailable();
        const QByteArray taken = room > 0 && there > 0 ? m_socket->read(std::min(room, there)) : QByteArray();
        const bool more = m_socket->bytesAvailable() > 0;
        bool again = false;
        {
            const QMutexLocker locked(&m_shared->mutex);
            if (m_shared->resetAsked) {
                return;
            }
            if (!taken.isEmpty()) {
                m_shared->in += taken;
                if (!std::exchange(m_shared->cameTold, true)) {
                    tellLocked([](DeviceLine& line) { line.wireCame(); });
                }
            }
            const bool roomNow = m_shared->in.size() + m_shared->lineHolds < m_shared->room;
            m_shared->starved = more && !roomNow;
            // (Room came back while this read: taken again, each time at least a byte, until the socket holds nothing
            // or the room is full.)
            again = more && roomNow;
        }
        if (again) {
            continue;
        }
        // Qt stops taking from Windows once what came fills its socket's buffer to the bound (begin), and takes again
        // only once a read empties that buffer, or once it is told the buffer's size anew. In the clear the reads here
        // empty it. Over TLS the bound holds as well for what came hidden, in Qt's own socket below the one read here,
        // which Qt's TLS reads a record at a time: where all that is left there is a piece of a record, it waits for
        // the rest of that record, never takes it from Windows, and nothing more comes, with room to spare here (seen
        // under load on the Windows on Arm test PC: a line held back and let go stood still, 14 hidden bytes in that
        // socket). So where nothing is left here to read and something hidden waits, Qt is told the size anew, room
        // here or none: it takes from Windows again, as far as the bound, and what it makes of that comes here as
        // anything does, and waits for room where there is none (starved). Where that socket is full, the telling
        // changes nothing, and need not: Qt's own call to read on, which the read that emptied what it had decrypted
        // asked for, either empties it, or decrypts something, which comes here next and is looked at again. (Not so
        // at a bound above 16 KiB by fewer bytes than a record has besides what it carries, which no line has. While
        // the socket was on the line's thread, the line told Qt so as it was let go.) Shown by test_device_line's
        // aLineOverTlsReadsOnWhereQtKeptAPieceOfARecordAtItsBound.
        if (!more && m_socket->encryptedBytesAvailable() > 0) {
            m_socket->setReadBufferSize(m_setup.readBuffer + 1);
            m_socket->setReadBufferSize(m_setup.readBuffer);
        }
        return;
    }
}

void DeviceLine::Wire::look()
{
    if (m_socket == nullptr) {
        return;
    }
    sayHanded(); // (a test's: lines::whatWasHanded)
    // Over TLS what was written leaves the socket's own count a turn of the loop later, hidden
    // and handed on, whether or not the KVM takes it: it is counted where it waits then, too.
    const qint64 waiting = m_socket->bytesToWrite() + m_socket->encryptedBytesToWrite();
    const QAbstractSocket::SocketState state = m_socket->state();
    const QAbstractSocket::SocketError error = m_socket->error();
    const QMutexLocker locked(&m_shared->mutex);
    // (What a socket that was found late held never left: it is not counted as gone.)
    if (!m_shared->lateFound) {
        m_shared->inSocket = m_shared->resetAsked ? 0 : waiting;
    }
    m_shared->state = state;
    m_shared->error = error;
}

void DeviceLine::Wire::sayHanded()
{
    if (m_socket != nullptr && !m_letGo) {
        lines::handed(m_handed - (m_socket->bytesToWrite() + m_socket->encryptedBytesToWrite()));
    }
}

void DeviceLine::Wire::fell()
{
    take(); // (what came before the end, as far as there is room for it)
    look();
    if (!std::exchange(m_fell, true)) {
        tell([](DeviceLine& line) { line.fell(); });
    }
}

void DeviceLine::Wire::end(bool drain)
{
    if (m_socket != nullptr) {
        // (What was said went before this: a write posted before this one handed it over, unless the line was reset.)
        // Thrown away after its goodbye, before the KVM has ended it: the socket goes with this, and
        // Windows closes the line. Over what had come and was not read it would reset the line
        // instead (close() says what that costs): so that is taken off the line first, as much as a
        // KVM has reason to send in a second and no more. (Off the line itself, as it lies there:
        // over TLS that is what came hidden, which nobody reads. Tried both ways: without this, a
        // far end on this PC lost what was said last, in the clear and over TLS.)
        if (const qintptr socket = m_socket->socketDescriptor(); drain && !resetAsked() && socket != -1) {
            std::array<char, 4096> some{};
            for (qint64 taken = 0; taken < m_setup.mostBytesASecond;) {
                const int came = ::recv(static_cast<SOCKET>(socket), some.data(), static_cast<int>(some.size()), 0);
                if (came <= 0) {
                    break;
                }
                taken += came;
            }
        }
        // (Nothing more is heard of it: what it says as it goes is nobody's. What it handed Windows is said the last
        // time.)
        sayHanded();
        m_letGo = true;
        QObject::disconnect(m_socket.get(), nullptr, this, nullptr);
        m_socket.reset();
    }
    // (What waited here for the socket goes with it, as what the socket held does.)
    letGoPending();
    m_lateWatch.reset();
    if (m_beforeQt != nullptr) {
        if (QAbstractEventDispatcher* const loop = QAbstractEventDispatcher::instance()) {
            loop->removeNativeEventFilter(m_beforeQt.get());
        }
        m_beforeQt.reset();
    }
    deleteLater();
    thread()->quit();
}

// --- the line -----------------------------------------------------------------------------------------------------

DeviceLine::DeviceLine(Where where, Limits limits, QObject* parent)
    : QObject(parent), m_where(std::move(where)), m_limits(limits)
{
    m_clock.start();
    // (To the millisecond: what waits is found late at lateAfterMs, not a twentieth of it later, when Qt may have
    // handed it on.)
    m_lateWatch.setSingleShot(true);
    m_lateWatch.setTimerType(Qt::PreciseTimer);
    connect(&m_lateWatch, &QTimer::timeout, this, [this] { lookWhetherLate(); });
}

DeviceLine::~DeviceLine()
{
    // Nothing is heard of the socket's own end, and nothing said of this one's.
    m_over = true;
    // Who asks, where it never went (a line that was not let in, nor asked for): written over as it goes.
    wipeInPlace(m_where.authorization);
    if (m_shared == nullptr) {
        return;
    }
    // Thrown away neither closed nor cut (deleteLater, or its owner's end: kvmd's AwakeLine goes so, and a JetKVM
    // driver's line still open when the driver goes), it goes as a cut one does: what was said and not handed to
    // Windows goes with it, the asking that says who asks among it, written over, and the socket is reset there, at
    // once or the moment Windows lets that thread go (outside review 2, 2.2: that thread handed all of it over then,
    // with no time limit, and closed the line behind it). But for the one message that thread may be handing over at
    // this very instant (SPEC.md section 8): it goes once, the reset right behind it. A line closed first goes behind
    // what was said before its close and its goodbye (close()).
    if (!m_leaving) {
        reset();
    }
    // Nothing more is handed here; and the socket's thread ends the socket, and itself, once it gets to that: at once,
    // or the moment Windows lets it go. Nothing here waits for it.
    {
        const QMutexLocker locked(&m_shared->mutex);
        m_shared->line = nullptr;
    }
    Wire* const wire = std::exchange(m_wire, nullptr);
    const bool drain = m_leaving && !m_gone;
    // (Let go of before its end is posted: until that end, the thread runs.)
    lines::letGo(std::exchange(m_thread, nullptr));
    QMetaObject::invokeMethod(wire, [wire, drain] { wire->end(drain); }, Qt::QueuedConnection);
}

void DeviceLine::open()
{
    // Once. And not on a line that was ended from here before it was begun: nobody holds that
    // one, and the pass is not sent for nobody.
    if (m_opened || m_over) {
        return;
    }
    m_opened = true;
    // (A raw line asks nothing and carries no pass: its address alone is taken.)
    if (m_raw ? m_where.address.host().isEmpty() : !fitToSend(m_where, m_live)) {
        QTimer::singleShot(0, this, [this] { end(End::NotAsked); });
        return;
    }
    if (refusedForItsTls(m_windowsOwnTls, m_where.address)) {
        // Whoever would judge its certificate is not Windows: the asking, which carries the
        // pass, does not go out.
        QTimer::singleShot(0, this, [this] { end(End::TlsDamaged); });
        return;
    }
    const bool encrypted = m_where.address.scheme() == "https"_L1;
    const bool known = m_where.knownByCertificate && m_where.certificate.size() == 32;
    if (encrypted && m_where.knownByCertificate && !known) {
        QTimer::singleShot(0, this, [this] { end(End::CertificateUnread); });
        return;
    }
    // The socket on a thread of its own, for the line's whole life (the class's own words say why).
    const quint16 port = static_cast<quint16>(m_where.address.port(encrypted ? 443 : 80));
    QThread* const thread =
        lines::aThreadOfItsOwn(u"qKVM device line"_s, m_where.address.host() + u':' + QString::number(port));
    if (thread == nullptr) {
        // (No application, or one that is ending; or Windows holds so many earlier lines' threads to this place:
        // nothing more is asked for, lines::aThreadOfItsOwn.)
        QTimer::singleShot(0, this, [this] { end(End::NotReached); });
        return;
    }
    m_thread = thread;
    m_shared = std::make_shared<Shared>();
    m_shared->line = this;
    m_shared->room = readBufferFor(m_limits);
    m_shared->clock.start();
    Wire::Setup setup;
    setup.host = m_where.address.host();
    setup.port = port;
    setup.encrypted = encrypted;
    setup.known = encrypted && known;
    setup.knownByCertificate = m_where.knownByCertificate;
    setup.certificate = m_where.certificate;
    setup.readBuffer = readBufferFor(m_limits);
    setup.mostBytesASecond = m_limits.mostBytesASecond;
    setup.lateAfterMs = m_limits.lateAfterMs;
    auto* const wire = new Wire(m_shared, std::move(setup)); // NOLINT(cppcoreguidelines-owning-memory): ends itself
    wire->moveToThread(thread);
    m_wire = wire;
    QMetaObject::invokeMethod(wire, [wire] { wire->begin(); }, Qt::QueuedConnection);
    // (A raw line not let in in its time, its TCP connection or its TLS not done, did not answer in time: as the web
    // says of a VNC server's connection that timed out.) Here, whatever the socket's thread still waits for.
    QTimer::singleShot(m_limits.letInWithinMs, this, [this] {
        if (!m_in) {
            end(objection().value_or(m_asked || m_raw ? End::Silent : End::NotReached));
        }
    });
}

void DeviceLine::wireConnected()
{
    if (!m_over) {
        ask();
    }
}

void DeviceLine::wireEncrypted(const QByteArray& der, bool changed)
{
    if (m_over) {
        return;
    }
    if (changed) {
        end(End::CertificateChanged);
        return;
    }
    emit certificateShown(der);
    if (m_over) {
        return;
    }
    ask();
}

void DeviceLine::wireLate()
{
    // (Its socket was reset there already, the moment what waited was late: see Wire::foundLate.)
    if (!m_over) {
        end(End::Late);
    }
}

void DeviceLine::wireCame()
{
    takeWhatCame();
    read();
}

void DeviceLine::takeWhatCame()
{
    if (m_shared == nullptr) {
        return;
    }
    const QMutexLocker locked(&m_shared->mutex);
    m_shared->cameTold = false;
    if (m_reset) {
        m_shared->in.clear();
        return;
    }
    if (!m_shared->in.isEmpty()) {
        if (m_cameAt > 0) {
            m_came.remove(0, m_cameAt);
            m_cameAt = 0;
        }
        if (m_came.isEmpty()) {
            m_came = std::exchange(m_shared->in, {});
        } else {
            m_came += m_shared->in;
            m_shared->in.clear();
        }
    }
    m_shared->lineHolds = available();
}

void DeviceLine::gaveRoom()
{
    if (m_shared == nullptr || m_reset) {
        return;
    }
    // (Under the lock that thread says it waits for room under, and looks at the room again under: Wire::take. Where it
    // has not said so yet, it sees this room itself.)
    const QMutexLocker locked(&m_shared->mutex);
    m_shared->lineHolds = available();
    if (m_shared->starved && m_shared->in.size() + m_shared->lineHolds < m_shared->room) {
        m_shared->starved = false;
        Wire* const wire = m_wire;
        QMetaObject::invokeMethod(wire, [wire] { wire->room(); }, Qt::QueuedConnection);
    }
}

QByteArray DeviceLine::peekCame(qint64 most) const
{
    return m_came.mid(m_cameAt, static_cast<qsizetype>(std::min(most, available())));
}

QByteArray DeviceLine::readCame(qint64 most)
{
    const auto taken = static_cast<qsizetype>(std::min(most, available()));
    QByteArray bytes = m_came.mid(m_cameAt, taken);
    skipCame(taken);
    return bytes;
}

qint64 DeviceLine::skipCame(qint64 most)
{
    const auto skipped = static_cast<qsizetype>(std::clamp<qint64>(most, 0, available()));
    m_cameAt += skipped;
    if (m_cameAt == m_came.size()) {
        m_came.clear();
        m_cameAt = 0;
    }
    return skipped;
}

std::optional<DeviceLine::End> DeviceLine::objection() const
{
    if (m_shared == nullptr) {
        return std::nullopt;
    }
    const QMutexLocker locked(&m_shared->mutex);
    return m_shared->objection;
}

bool DeviceLine::connectedNow() const
{
    if (m_shared == nullptr) {
        return false;
    }
    const QMutexLocker locked(&m_shared->mutex);
    return !m_shared->resetAsked && m_shared->state == QAbstractSocket::ConnectedState;
}

void DeviceLine::receiveText(QByteArray protocol)
{
    if (!m_opened && !protocol.isEmpty() && protocol.size() <= 64 && std::ranges::all_of(protocol, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        })) {
        m_protocol = std::move(protocol);
        m_receivesText = true;
    }
}

void DeviceLine::receiveBytes(QByteArray protocol)
{
    if (!m_opened && !m_live && !m_receivesText && !protocol.isEmpty() && protocol.size() <= 64 &&
        std::ranges::all_of(protocol,
                            [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; })) {
        m_protocol = std::move(protocol);
        m_receivesBytes = true;
    }
}

void DeviceLine::receiveBytes()
{
    if (!m_opened && !m_live && !m_receivesText) {
        m_protocol.clear();
        m_receivesBytes = true;
    }
}

void DeviceLine::receiveRaw()
{
    if (!m_opened && !m_live && !m_receivesText && !m_receivesBytes) {
        m_raw = true;
        m_receivesBytes = true;
    }
}

std::optional<DeviceLine::End> DeviceLine::takeRaw()
{
    while (available() > 0) {
        if (m_heldBack && !m_over) {
            return std::nullopt; // (held back from inside what was handed out last: the rest waits)
        }
        const QByteArray piece = readCame(m_limits.largestFrameBytes);
        if (piece.isEmpty()) {
            return std::nullopt;
        }
        // Counted as a message each, as it came: more in a second than a server has reason to send ends the line.
        if (tooMuch(1, piece.size())) {
            return End::Misbehaved;
        }
        emit bytesCame(piece);
        if (m_over) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void DeviceLine::receiveText()
{
    if (!m_opened && !m_live && !m_receivesBytes) {
        m_receivesText = true;
    }
}

void DeviceLine::hearShortTexts(qint64 mostBytes)
{
    if (!m_opened && !m_live && !m_receivesText && !m_receivesBytes && mostBytes > 0) {
        m_hearsUpTo = std::min(mostBytes, m_limits.largestFrameBytes);
    }
}

void DeviceLine::receiveLive()
{
    if (!m_opened && !m_receivesText && !m_receivesBytes) {
        m_live = true;
    }
}

void DeviceLine::holdBack(bool held)
{
    if (std::exchange(m_heldBack, held) == held || held) {
        return;
    }
    QTimer::singleShot(0, this, [this] {
        // What waited is read; and the socket's thread, where it left something on the line for want of room here,
        // is told there is room again (read() tells it: gaveRoom, Wire::room).
        if (!m_heldBack && m_in && !m_over) {
            takeWhatCame();
            read();
        }
    });
}

bool DeviceLine::ping()
{
    if (m_raw) {
        return false; // (RFB has no "are you there")
    }
    return sayAs(kindAreYouThere, {});
}

void DeviceLine::ask()
{
    if (m_raw) {
        // Nothing to ask: RFB begins with the server's own word, which is read from here on.
        m_asked = true;
        m_in = true;
        emit letIn(); // (whoever hears that may close the line there and then)
        takeWhatCame();
        if (!m_over && available() > 0) {
            read();
        }
        return;
    }
    // Sixteen bytes that nobody could have known, for the mark the answer has to carry.
    std::array<quint32, 4> random{};
    QRandomGenerator::system()->fillRange(random.data(), random.size());
    QByteArray sixteen;
    for (const quint32 four : random) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            sixteen += static_cast<char>((four >> shift) & 0xFF);
        }
    }
    m_key = sixteen.toBase64();
    const bool encrypted = m_where.address.scheme() == "https"_L1;
    QByteArray host = m_where.address.host(QUrl::FullyEncoded).toLatin1();
    if (host.contains(':')) {
        host = '[' + host + ']';
    }
    if (const int port = m_where.address.port(-1); port != -1 && port != (encrypted ? 443 : 80)) {
        host += ':' + QByteArray::number(port);
    }
    // Who asks: a server's live line says it in its first message, after the line is in; a line asked
    // for with nothing to say who asks says nothing (Where::withoutCookie).
    const auto [who, saidAs] =
        m_live                             ? std::pair{"X-QKVM: "_ba, QByteArrayView("1")}
        : !m_where.authorization.isEmpty() ? std::pair{"Authorization: "_ba, QByteArrayView(m_where.authorization)}
        : !m_where.authToken.isEmpty()     ? std::pair{"X-Auth-Token: "_ba, QByteArrayView(m_where.authToken)}
        : m_where.withoutCookie            ? std::pair{QByteArray(), QByteArrayView()}
                                           : std::pair{"Cookie: "_ba, QByteArrayView(m_where.cookie)};
    // The page it is asked from, where it is one of the KVM's own (a management controller's console).
    const QByteArray from =
        m_receivesBytes ? "Origin: " + m_where.address.scheme().toLatin1() + "://" + host + "\r\n" : QByteArray();
    const QByteArray head = "GET " + m_where.asking + " HTTP/1.1\r\nHost: " + host +
                            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + m_key +
                            "\r\nSec-WebSocket-Version: 13\r\n";
    const QByteArray after =
        from + (m_protocol.isEmpty() ? QByteArray{} : "Sec-WebSocket-Protocol: " + m_protocol + "\r\n") + "\r\n";
    // The asking, made once at its size with who asks in it: what says so (a Proxmox guest's API token among it)
    // stands in this one place, which Qt copies and which is written over once Qt has its copy; and the token in this
    // line's Where is written over then, as it is needed for this asking alone (the console line's copy of the token,
    // review c9b-1, L1). What Qt makes of it to send it is Qt's (SPEC.md section 8).
    QByteArray asking;
    const qsizetype said = who.isEmpty() ? 0 : who.size() + saidAs.size() + 2;
    asking.reserve(head.size() + said + after.size());
    asking.append(QByteArrayView(head));
    if (!who.isEmpty()) {
        asking.append(QByteArrayView(who)).append(saidAs).append(QByteArrayView("\r\n"));
    }
    asking.append(QByteArrayView(after));
    m_asked = true;
    writeCopied(asking);
    wipeInPlace(asking);
    wipeInPlace(m_where.authorization);
    m_where.authorization = QByteArray();
}

std::optional<DeviceLine::End> DeviceLine::refusedBy(const QByteArray& head) const
{
    // "You are in", in WebSocket's own words, and with the mark that only something which read
    // this asking can make. Whatever else it says (go there, sign on, no) is "no". Of a "no"
    // only this much is looked at: whether it is "not signed in" (401) or "not you" (403), which
    // whoever made the line can do something about, each in its own way (the web's lines: a kvmd
    // KVM signs in anew on either, a JetKVM on a 401 alone).
    const QList<QByteArray> lines = head.split('\n');
    const QByteArray first = lines.value(0);
    const bool signalling = !m_protocol.isEmpty() && !m_receivesBytes;
    if (signalling) {
        VideoDiagnostics::record("signaling_http", {{"status", first.split(' ').value(1).toInt()}});
    }
    if (!first.startsWith("HTTP/1.1 101")) {
        if (first.startsWith("HTTP/1.1 401")) {
            return End::SignedOut;
        }
        return first.startsWith("HTTP/1.1 403") ? End::Refused : End::NotLetIn;
    }
    if (!m_protocol.isEmpty()) {
        int protocols = 0;
        for (const QByteArray& line : lines) {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0 && line.first(colon).trimmed().toLower() == "sec-websocket-protocol") {
                if (line.sliced(colon + 1).trimmed() != m_protocol) {
                    if (signalling) {
                        VideoDiagnostics::record("signaling_wrong_subprotocol");
                    }
                    return End::NotLetIn;
                }
                ++protocols;
            }
        }
        if (protocols != 1) {
            if (signalling) {
                VideoDiagnostics::record("signaling_missing_subprotocol", {{"count", protocols}});
            }
            return End::NotLetIn;
        }
    }
    // A server's live line, and a line that reads what a device says with no subprotocol: "you are in"
    // in WebSocket's own words, and nothing that nobody asked for.
    if (m_live || ((m_receivesText || m_receivesBytes) && m_protocol.isEmpty())) {
        bool upgrade = false;
        bool connection = false;
        int accepts = 0;
        for (const QByteArray& line : lines) {
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.first(colon).trimmed().toLower();
            const QByteArray value = line.sliced(colon + 1).trimmed().toLower();
            if (name == "sec-websocket-extensions" || name == "sec-websocket-protocol")
                return End::NotLetIn;
            if (name == "upgrade") {
                if (upgrade || value != "websocket")
                    return End::NotLetIn;
                upgrade = true;
            }
            if (name == "connection") {
                for (const auto& token : value.split(','))
                    connection = connection || token.trimmed() == "upgrade";
            }
            if (name == "sec-websocket-accept")
                ++accepts;
        }
        if (!upgrade || !connection || accepts != 1 || first.split(' ').value(1).trimmed() != "101")
            return End::NotLetIn;
    }
    const QByteArray mark = QCryptographicHash::hash(m_key + webSocketMark, QCryptographicHash::Sha1).toBase64();
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const qsizetype colon = lines.at(index).indexOf(':');
        if (colon > 0 && lines.at(index).left(colon).trimmed().toLower() == "sec-websocket-accept") {
            if (lines.at(index).mid(colon + 1).trimmed() == mark) {
                return std::nullopt;
            }
            break;
        }
    }
    return End::NotLetIn;
}

void DeviceLine::read()
{
    // (However it ends: how much of what came waits here is told to the socket's thread, which takes more off the line
    // where it left some for want of room.)
    const auto told = qScopeGuard([this] { gaveRoom(); });
    if (m_raw && !m_in && !m_over) {
        return; // (what came before the line is in waits: over TLS nothing does)
    }
    if (!m_in && !m_over) {
        // The answer to the asking, read to its end and not a byte further: what comes after it
        // in the same breath is a message already.
        const QByteArray seen = peekCame(longestHead + 4);
        const qsizetype stop = seen.indexOf("\r\n\r\n");
        if (stop < 0) {
            if (seen.size() > longestHead) {
                end(End::NotLetIn);
            }
            return;
        }
        if (const std::optional<End> refused = refusedBy(readCame(stop + 4))) {
            end(*refused);
            return;
        }
        m_in = true;
        emit letIn(); // (whoever hears that may close the line there and then)
    }
    if (m_over) {
        // Closed from here and not gone yet: what still comes is nobody's, and is thrown away.
        // Looked at for one thing, while the KVM's ending of the line is waited for: its own
        // goodbye, the one sign that it got what was said (noted where it is read). Until that
        // has come, or something that is no message: after either, nothing more is looked at.
        if (m_reset) {
            return;
        }
        if (m_awaitsGoodbye && throwAway().has_value()) {
            m_awaitsGoodbye = false;
        }
        if (!m_awaitsGoodbye) {
            skipCame(available());
        }
        return;
    }
    const qint64 before = available();
    if (const std::optional<End> why = m_raw ? takeRaw() : throwAway()) {
        end(*why);
    } else if (unsent() > m_limits.mostUnsentBytes) {
        end(End::TakesNothing);
    } else if (available() < before) {
        emit heard(); // (whoever hears that may end the line there and then: nothing is done after it)
    }
}

std::optional<DeviceLine::End> DeviceLine::throwAway()
{
    while (available() > 0) {
        if (m_heldBack && !m_over) {
            return std::nullopt; // (held back from inside what was handed out last: the rest waits)
        }
        if (m_toSkip > 0) {
            const qint64 skipped = skipCame(m_toSkip);
            m_toSkip -= skipped;
            if (tooMuch(0, skipped)) {
                return End::Misbehaved; // more in a second than a KVM has reason to send
            }
            if (m_toSkip > 0) {
                return std::nullopt; // the rest of it has not come yet
            }
            continue;
        }
        // How a message begins: what it is, and how long.
        const QByteArray begun = peekCame(10);
        if (begun.size() < 2) {
            return std::nullopt;
        }
        const auto first = static_cast<quint8>(begun.at(0));
        const auto second = static_cast<quint8>(begun.at(1));
        const int kind = first & 0x0F;
        const int said = second & 0x7F;
        const qsizetype beginning = said == 126 ? 4 : said == 127 ? 10 : 2;
        // Nothing of what this program never agreed to, and nothing hidden as a caller hides it.
        if ((first & 0x70) != 0 || (second & 0x80) != 0 || (kind > 2 && kind < 8) || kind > 10) {
            return End::Misbehaved;
        }
        if (begun.size() < beginning) {
            return std::nullopt;
        }
        qint64 length = said;
        if (said >= 126) {
            length = 0;
            for (qsizetype index = 2; index < beginning; ++index) {
                length = (length << 8) | static_cast<quint8>(begun.at(index));
            }
            if (length < 0) {
                return End::Misbehaved;
            }
        }
        // Refused by what it says of itself, before any of it is waited for: thrown away at the
        // pace it came, a message without end would keep this thread reading for as long.
        if (length > m_limits.largestFrameBytes) {
            return End::Misbehaved;
        }
        if (kind >= kindGoodbye) {
            // "Are you there", its answer, and "goodbye": short by their own rule.
            if (said > 125 || (first & 0x80) == 0) {
                return End::Misbehaved;
            }
            if (available() < beginning + said) {
                return std::nullopt;
            }
            skipCame(beginning);
            const QByteArray with = readCame(said);
            if (kind == kindGoodbye) {
                // It said goodbye. Behind the goodbye from here, that is its answer to it: it
                // got that one, and all that was said before it.
                m_answered = m_leaving;
                m_saidGoodbye = true;
                return End::Fell;
            }
            if (tooMuch(1, beginning + said)) {
                return End::Misbehaved;
            }
            if (kind == kindAreYouThere && !m_leaving) {
                // Answered with what it sent, hidden the way a caller has to hide what it sends.
                // (Not behind the goodbye from here, after which nothing is said.)
                write(message(kindIAm, with));
            }
            continue;
        }
        if ((m_live || m_receivesText || m_receivesBytes) && !m_over) {
            const bool final = (first & 0x80) != 0;
            const int messageKind = kind == 0 ? m_fragmentedKind : kind;
            const qint64 limit = m_live && messageKind == kindText ? qint64{64} * 1024 : m_limits.largestFrameBytes;
            // What each line takes: a console's bytes alone, a server's live line both, any other text alone.
            const bool taken =
                kind == 0 || (m_receivesBytes ? kind == kindBytes : kind == kindText || (m_live && kind == kindBytes));
            if (!taken || (kind == 0) != (m_fragmentedKind != 0) || m_text.size() + length > limit) {
                return End::Misbehaved;
            }
            if (available() < beginning + length) {
                return std::nullopt;
            }
            skipCame(beginning);
            m_text.append(readCame(length));
            if (tooMuch(1, beginning + length)) {
                return End::Misbehaved;
            }
            m_fragmentedKind = final ? 0 : messageKind;
            if (final) {
                QByteArray complete = std::exchange(m_text, {});
                if (messageKind == kindBytes) {
                    emit bytesCame(complete);
                } else {
                    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
                    const QString checked = utf8.decode(complete);
                    if (utf8.hasError() || checked.contains(QChar::Null)) {
                        return End::Misbehaved;
                    }
                    emit textCame(complete);
                }
                if (m_over) {
                    return std::nullopt;
                }
            }
            continue;
        }
        if (m_hearsUpTo > 0 && kind == kindText && (first & 0x80) != 0 && length <= m_hearsUpTo) {
            // A short text in one part, on a line that hears those (hearShortTexts): handed out whole, as it came.
            if (available() < beginning + length) {
                return std::nullopt;
            }
            skipCame(beginning);
            const QByteArray whole = readCame(length);
            if (tooMuch(1, beginning + length)) {
                return End::Misbehaved;
            }
            emit textCame(whole);
            if (m_over) {
                return std::nullopt;
            }
            continue;
        }
        skipCame(beginning);
        if (tooMuch(1, beginning)) {
            return End::Misbehaved; // more messages in a second than a KVM has reason to send
        }
        m_toSkip = length;
    }
    return std::nullopt;
}

bool DeviceLine::tooMuch(int messages, qint64 bytes)
{
    const qint64 second = m_clock.elapsed() / 1000;
    if (second != m_second) {
        m_second = second;
        m_heard = 0;
        m_heardBytes = 0;
    }
    m_heard += messages;
    m_heardBytes += bytes;
    return m_heard > m_limits.mostMessagesASecond || m_heardBytes > m_limits.mostBytesASecond;
}

void DeviceLine::write(const QByteArray& bytes)
{
    hand(bytes, false);
}

void DeviceLine::writeCopied(QByteArrayView copied)
{
    // (A copy of its own for the socket's thread, which writes it over once Qt has made its own of it: of whoever's
    // these are, nothing is kept.)
    hand(QByteArray(copied.data(), copied.size()), true);
}

void DeviceLine::hand(QByteArray bytes, bool wiped)
{
    forgetWhatLeft();
    if (m_shared == nullptr || m_reset) {
        if (wiped) {
            wipeInPlace(bytes);
        }
        return;
    }
    // Counted before the socket's thread can take it: what waits is never counted short. What is written before the
    // line is let in (the asking alone: neither in nor over, the goodbye being said on a line that is over) is not held
    // to Limits::lateAfterMs, here or there: it is no input, and Limits::letInWithinMs bounds it (Shared::Said::timed).
    m_written += bytes.size();
    const bool timed = m_in || m_over;
    {
        const QMutexLocker locked(&m_shared->mutex);
        m_shared->out.append(Shared::Said{std::move(bytes), wiped, m_shared->clock.elapsed(), timed});
        if (!std::exchange(m_shared->writeAsked, true)) {
            Wire* const wire = m_wire;
            QMetaObject::invokeMethod(wire, [wire] { wire->write(); }, Qt::QueuedConnection);
        }
    }
    if (timed) {
        noteWritten();
    }
}

void DeviceLine::noteWritten()
{
    const qint64 now = m_clock.elapsed();
    if (!m_waiting.isEmpty() && m_waiting.last().atMs == now) {
        m_waiting.last().upTo = m_written;
    } else {
        m_waiting.append({m_written, now});
    }
    // Watched from now on, whether or not anything is said after it (outside review 2.6): the watch was already set
    // for something older where it runs.
    if (!m_lateWatch.isActive()) {
        m_lateWatch.start(m_limits.lateAfterMs + 1);
    }
}

void DeviceLine::lookWhetherLate()
{
    if (m_over) {
        return;
    }
    // What has waited too long ends the line there and then, and goes with it (reset): left to Qt, it would be handed
    // on, a key from then pressed now, as soon as the way came back. Else the next look is when the oldest of what
    // still waits would be late; none where nothing waits.
    if (late()) {
        end(End::Late);
        return;
    }
    if (!m_waiting.isEmpty()) {
        m_lateWatch.start(static_cast<int>(
            std::max<qint64>(1, m_waiting.first().atMs + m_limits.lateAfterMs + 1 - m_clock.elapsed())));
    }
}

void DeviceLine::forgetWhatLeft()
{
    // All that was written, less what still waits, has left. (Over TLS what waits is counted
    // hidden, which is more bytes than it was written as: so less is taken to have left than
    // has, and a line is found late a little sooner for it, never later.)
    const qint64 left = m_written - unsent();
    while (!m_waiting.isEmpty() && m_waiting.first().upTo <= left) {
        m_waiting.removeFirst();
    }
}

bool DeviceLine::late()
{
    // (Found so by the socket's thread already, whatever this thread was doing: Wire::foundLate.)
    if (m_shared != nullptr) {
        const QMutexLocker locked(&m_shared->mutex);
        if (m_shared->lateFound) {
            return true;
        }
    }
    forgetWhatLeft();
    return !m_waiting.isEmpty() && m_clock.elapsed() - m_waiting.first().atMs > m_limits.lateAfterMs;
}

qint64 DeviceLine::waitedMs()
{
    forgetWhatLeft();
    return m_waiting.isEmpty() ? 0 : m_clock.elapsed() - m_waiting.first().atMs;
}

bool DeviceLine::hasLeft(qint64 mark) const
{
    // (Counted as forgetWhatLeft counts: over TLS less is taken to have left than has, never more.)
    return m_written - unsent() >= mark;
}

qint64 DeviceLine::unsent() const
{
    // What the socket's thread has not taken yet, and what its socket still held of what it took when it last looked
    // (over TLS hidden already: Wire::look).
    if (m_shared == nullptr) {
        return 0;
    }
    const QMutexLocker locked(&m_shared->mutex);
    return m_written - m_shared->taken + m_shared->inSocket;
}

bool DeviceLine::say(const QByteArray& text)
{
    return sayAs(kindText, text);
}

bool DeviceLine::sayBytes(const QByteArray& bytes)
{
    return sayAs(kindBytes, bytes);
}

bool DeviceLine::sayAs(quint8 kind, const QByteArray& text)
{
    if (!m_in || m_over) {
        return false;
    }
    // Nothing is said behind what has waited too long, and none of that is handed on first either:
    // all of it would arrive at once when the way comes back, a key from then pressed now. So this
    // is looked at before what waits is offered to Windows (outside review 2.6: offered first, what
    // waited went out the moment the way came back, and was judged on time after that).
    if (late()) {
        end(End::Late);
        return false;
    }
    // (What waits, and has not waited too long, is offered to Windows by the socket's thread as it takes this: only
    // what Windows does not take is waiting.) Looked at before the text is taken in: what waits never becomes more
    // than may.
    if (unsent() + (m_raw ? text.size() : onTheLine(text.size())) > m_limits.mostUnsentBytes) {
        end(End::TakesNothing);
        return false;
    }
    // (On a raw line as it is: RFB's own messages, with nothing around them.) Handed to Windows by the socket's thread
    // at once, and not at the next turn of this loop.
    write(m_raw ? text : message(kind, text));
    return !m_over;
}

bool DeviceLine::close()
{
    if (std::exchange(m_closed, true)) {
        return false;
    }
    if (m_over) {
        // It ended by itself, or was cut, before this: there is nothing to wait for.
        QTimer::singleShot(0, this, [this] { sayGone(); });
        return false;
    }
    const bool wasIn = std::exchange(m_in, false);
    m_over = true;
    // Nothing was agreed on it yet, and there is nobody to say goodbye to. Or what was said has
    // waited too long already: it is not pushed out now, to arrive when the way comes back.
    if (!wasIn || late()) {
        reset();
        QTimer::singleShot(0, this, [this] { sayGone(); });
        return false;
    }
    // (Standing as far as the socket's thread last saw, looked at before it is told anything: told, it may have begun
    // to end the connection before this looks again.)
    const bool stood = connectedNow();
    if (m_raw) {
        // RFB has no goodbye: what was said is pushed out by the socket's thread, and the connection ended behind it
        // there (`gone` is never said from inside this). A server that does not end its side in time is reset.
        m_leaving = true;
        Wire* const wire = m_wire;
        QMetaObject::invokeMethod(wire, [wire] { wire->disconnectFromHost(); }, Qt::QueuedConnection);
        QTimer::singleShot(m_limits.goodbyeWithinMs, this, [this] {
            if (!m_gone) {
                reset();
                sayGone();
            }
        });
        return stood;
    }
    QByteArray why;
    why += static_cast<char>(nothingWrong >> 8);
    why += static_cast<char>(nothingWrong & 0xFF);
    // Pushed out by the socket's thread at once, behind what was said before it: whoever closes a line may throw it
    // away in the same breath, and what was said on it last is to have left by then (it does: what was said is handed
    // over by a write posted before the end, which the socket's thread takes first).
    write(message(kindGoodbye, why));
    // The KVM ends a line it was said goodbye on, as the book has it, and that is waited for: the
    // end of the line does not travel with the last that was said on it. (Ended from here in
    // the same breath, a line on which something of the KVM's had come and was not read is
    // reset by Windows, and a far end that is Windows too throws away what it had not taken:
    // the tests' made-up KVMs lost the last "up" that way, the one that reads its line by hand
    // as the one that is Qt's own WebSocket, each time it was tried.) A KVM that does not end
    // it, or takes nothing off the line, is not waited for longer: what has not gone by then
    // has waited too long, and goes with the line.
    m_leaving = true;
    m_awaitsGoodbye = true;
    QTimer::singleShot(m_limits.goodbyeWithinMs, this, [this] {
        if (!m_gone) {
            reset();
            sayGone();
        }
    });
    return !m_gone && stood;
}

void DeviceLine::fell()
{
    if (m_leaving) {
        // The KVM has ended the line that was closed from here: all of it has gone.
        sayGone();
        return;
    }
    end(fallen());
}

void DeviceLine::sayGone()
{
    if (!std::exchange(m_gone, true)) {
        emit gone(m_answered);
    }
}

void DeviceLine::cut()
{
    if (m_over) {
        return;
    }
    m_over = true;
    m_in = false;
    reset();
}

void DeviceLine::reset()
{
    // What waits in this program to leave goes with the line, here and now: none of it is handed to the socket's thread
    // any more, and what that thread had not taken is let go of (written over where it said who asks). The socket is
    // reset there (Wire::reset, which says why it is reset and not closed): at once, or, where Windows holds that
    // thread, the moment Windows lets it go, and nothing more goes to Windows meanwhile.
    m_came.clear();
    m_cameAt = 0;
    m_waiting.clear();
    if (std::exchange(m_reset, true) || m_shared == nullptr) {
        return;
    }
    {
        const QMutexLocker locked(&m_shared->mutex);
        for (Shared::Said& one : m_shared->out) {
            if (one.wiped) {
                wipeInPlace(one.bytes);
            }
        }
        m_shared->out.clear();
        m_shared->resetAsked = true;
        // (Nothing waits any more: what was said was let go of, here or by Windows.)
        m_shared->taken = m_written;
        m_shared->inSocket = 0;
        m_shared->in.clear();
        m_shared->lineHolds = 0;
    }
    Wire* const wire = m_wire;
    QMetaObject::invokeMethod(wire, [wire] { wire->reset(); }, Qt::QueuedConnection);
}

DeviceLine::End DeviceLine::fallen() const
{
    if (const std::optional<End> found = objection()) {
        return *found;
    }
    // Reset by its socket's thread for what had waited there too long (Wire::foundLate): the socket's own end, which
    // that reset brings about, may be heard here before the word that it was late.
    if (m_shared != nullptr) {
        const QMutexLocker locked(&m_shared->mutex);
        if (m_shared->lateFound) {
            return End::Late;
        }
    }
    return m_asked ? End::Fell : End::NotReached;
}

void DeviceLine::end(End why)
{
    if (m_over) {
        return;
    }
    if (!m_protocol.isEmpty() && !m_receivesBytes) {
        QAbstractSocket::SocketError error = QAbstractSocket::UnknownSocketError;
        if (m_shared != nullptr) {
            const QMutexLocker locked(&m_shared->mutex);
            error = m_shared->error;
        }
        VideoDiagnostics::record("signaling_failure",
                                 {{"reason", static_cast<int>(why)}, {"socket_error", static_cast<int>(error)}});
    }
    m_over = true;
    m_in = false;
    // As a line that is cut: nothing of what was said on it arrives after it has ended, the
    // keys that waited too long least of all.
    reset();
    emit ended(why);
}

} // namespace qkvm::core
