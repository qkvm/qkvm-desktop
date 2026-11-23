#include <qkvm/core/vnc_device.h>

#include <algorithm>
#include <utility>

#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkProxy>
#include <QPromise>
#include <QSslCertificate>
#include <QSslSocket>
#include <QThread>
#include <QTimer>

#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/tls.h>

#include "line_threads.h"

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace vnc {

namespace {

base::Error noAddress()
{
    // (What was typed is not said back: it may be anything, a password typed into the wrong box among it.)
    return {"invalid", "Not the address of a VNC server"};
}

} // namespace

base::Result<QString> addressFrom(const QString& typed)
{
    if (schemeAmiss(typed)) {
        return schemeAmissError();
    }
    QString text = typed.trimmed();
    QString scheme = u"vnc"_s;
    for (const QString& prefix : {u"vncs://"_s, u"vnc://"_s}) {
        if (text.startsWith(prefix, Qt::CaseInsensitive)) {
            scheme = prefix.chopped(3);
            text = text.sliced(prefix.size());
            break;
        }
    }
    // A question mark or a cross is no part of a VNC server's address anywhere: it has no question to be asked and no
    // place in a page. Where Qt and the web end a host and its port at one, a colon before it was a host with no port
    // ("vncs:" of "vnc://vncs:?/10.0.0.6", a server at a host called "vncs"), and a port behind it was dropped
    // ("10.0.0.6?:5901" was 10.0.0.6 at 5900): refused, as the web does not (outside review fin-1, A-L1).
    if (text.isEmpty() || text.contains(u"://"_s) || text.contains(u'@') || text.contains(u'?') ||
        text.contains(u'#')) {
        return noAddress();
    }
    text = text.section(u'/', 0, 0); // (a path means nothing to a VNC server)
    // A colon with no port behind it is no port: it is what a scheme written with one slash leaves of what was typed
    // ("vncs:" of "vnc://vncs:/10.0.0.6"), which would be a server at a host called "vncs" (outside review c9a-4, L1).
    if (text.endsWith(u':')) {
        return noAddress();
    }
    const QUrl parts(u"//"_s + text, QUrl::StrictMode);
    if (!parts.isValid() || parts.host().isEmpty()) {
        return noAddress();
    }
    const int port = parts.port(defaultPort);
    if (port < 1 || port > 65535) {
        return noAddress();
    }
    // Its host as it was typed, written as a KVM's is (kvm::addressFrom), as the web's vnc_url writes it by its
    // device_url: letters of the twenty-six in small, digits, dots, hyphens and the underscore, no dot at its end, or
    // numbers written the one way, IPv6 in brackets and short. And Qt read that host and no other. A sign that Qt drops
    // from a name, or makes another letter of, as it reads one (a soft hyphen, a sign of no width, a mark of the byte
    // order, letters of the full width) is no sign of a host's: Qt made "vncs" of one before "vncs:", where the
    // refusal of a scheme written amiss, which reads the text as typed, did not see it (outside review c9a-4, L1).
    const QString typedHost = text.startsWith(u'[') ? text.left(text.indexOf(u']') + 1) : text.section(u':', 0, 0);
    const base::Result<QUrl> asAKvmsIs = kvm::addressFrom(u"https://"_s + typedHost);
    if (typedHost.isEmpty() || !asAKvmsIs) {
        return noAddress();
    }
    QString host = asAKvmsIs.value().host(QUrl::FullyEncoded);
    QString read = parts.host(QUrl::FullyEncoded).toLower();
    while (read.endsWith(u'.')) {
        read.chop(1);
    }
    if (host.isEmpty() || host != read) {
        return noAddress();
    }
    if (host.contains(u':')) {
        host = u'[' + host + u']';
    }
    return scheme + u"://"_s + host + (port == defaultPort ? QString() : u':' + QString::number(port));
}

bool namesOne(const QString& typed)
{
    const QString text = typed.trimmed();
    return text.startsWith(u"vnc://"_s, Qt::CaseInsensitive) || text.startsWith(u"vncs://"_s, Qt::CaseInsensitive);
}

bool schemeAmiss(const QString& typed)
{
    const QString text = typed.trimmed();
    for (const QString& scheme : {u"vncs"_s, u"vnc"_s}) {
        if (text.size() > scheme.size() && text.startsWith(scheme, Qt::CaseInsensitive)) {
            const QChar next = text.at(scheme.size());
            if (next == u':' || next == u'/') {
                return !text.startsWith(scheme + u"://"_s, Qt::CaseInsensitive);
            }
        }
    }
    return false;
}

base::Error schemeAmissError()
{
    // (What was typed is not said back, as noAddress.)
    return {"invalid", "This begins as a VNC server's address does, with vnc or vncs, but not with vnc:// or vncs://. "
                       "Write either in full, with both slashes."};
}

Place placeOf(const QString& address)
{
    const QUrl url(address);
    return {url.host(), url.port(defaultPort), url.scheme() == u"vncs"_s};
}

QUrl lineAddress(const QString& address)
{
    const Place place = placeOf(address);
    const QString host = place.host.contains(u':') ? u'[' + place.host + u']' : place.host;
    return QUrl(u"%1://%2:%3"_s.arg(place.tls ? u"https"_s : u"http"_s, host).arg(place.port));
}

namespace {

// What a handshake on that thread came to, and the promise kept when it did (Qt's, safe from thread to thread).
struct Told {
    QMutex mutex;
    std::optional<base::Result<Certificate>> came;
    QPromise<void> done;
};

// The handshake itself, on that thread: what it comes to is told, once, and the line is let go of, and the thread with
// it (whenever Windows lets it go: its own time runs there too).
void askThere(QObject* home, const Place& place, int withinMs, const std::shared_ptr<Told>& told)
{
    auto* socket = new QSslSocket(home); // NOLINT(cppcoreguidelines-owning-memory): `home`'s, let go of here
    socket->setProxy(QNetworkProxy::NoProxy);
    auto objected = std::make_shared<bool>(false); // Windows objected to what was shown
    auto over = std::make_shared<bool>(false);
    const auto tell = [told, socket, home, over](base::Result<Certificate> said) {
        if (std::exchange(*over, true)) {
            return;
        }
        {
            const QMutexLocker locked(&told->mutex);
            told->came = std::move(said);
        }
        told->done.finish();
        socket->abort();
        home->deleteLater();
        home->thread()->quit();
    };
    QObject::connect(socket, &QSslSocket::sslErrors, socket, [objected, socket](const QList<QSslError>& errors) {
        // What Windows objects to is noted, and the handshake is let finish so that the certificate can be read:
        // nothing is written on this line, whatever is shown.
        *objected = true;
        socket->ignoreSslErrors(errors);
    });
    QObject::connect(socket, &QSslSocket::encrypted, socket, [objected, socket, tell] {
        const QSslCertificate shown = socket->peerCertificate();
        if (shown.isNull()) {
            tell(base::Error{"no_certificate", "The VNC server showed no certificate"});
            return;
        }
        tell(Certificate{shown.digest(QCryptographicHash::Sha256), shown.toDer(), !*objected});
    });
    QObject::connect(socket, &QSslSocket::errorOccurred, socket,
                     [tell] { tell(base::Error{"unreachable", "Device is unreachable"}); });
    QTimer::singleShot(withinMs, socket, [tell] { tell(base::Error{"unreachable", "Device is unreachable"}); });
    socket->connectToHostEncrypted(place.host, static_cast<quint16>(place.port));
}

} // namespace

void askCertificate(const QString& address, int withinMs, QObject* context,
                    std::function<void(base::Result<Certificate>)> done)
{
    struct Asking {
        std::function<void(base::Result<Certificate>)> done;
        bool over = false;
    };
    auto asking = std::make_shared<Asking>();
    asking->done = std::move(done);
    const auto answer = [asking](base::Result<Certificate> said) {
        if (!std::exchange(asking->over, true)) {
            asking->done(std::move(said));
        }
    };
    const Place place = placeOf(address);
    if (!place.tls || place.host.isEmpty()) {
        QTimer::singleShot(0, context, [answer] {
            answer(base::Error{"no_certificate", "A VNC server spoken to in the clear shows no certificate"});
        });
        return;
    }
    // Whoever judges what it shows is Windows, as on every line of the program's (tls.h).
    if (refusedForItsTls(useWindowsOwnTls(), lineAddress(address))) {
        QTimer::singleShot(0, context, [answer] { answer(base::Error{tlsDamaged, tlsDamagedSentence}); });
        return;
    }
    // On a thread of its own (line_threads.h), which ends with the asking: one Windows holds holds no other asking, and
    // the program's end does not wait for it.
    QThread* const thread =
        lines::aThreadOfItsOwn(u"qKVM certificate asking"_s, place.host + u':' + QString::number(place.port));
    if (thread == nullptr) {
        // (The program is ending, or Windows holds so many earlier askings' threads for this server: nothing more is
        // asked of it.)
        QTimer::singleShot(0, context, [answer] { answer(base::Error{"unreachable", "Device is unreachable"}); });
        return;
    }
    auto* const home = new QObject; // NOLINT(cppcoreguidelines-owning-memory): the thread's, let go of in askThere
    home->moveToThread(thread);
    // What the handshake there comes to comes back here through a future (Qt's, safe from thread to thread), watched
    // by an object of `context`'s, which goes with it; and the asker is answered in `withinMs` whatever that thread
    // still waits for.
    auto told = std::make_shared<Told>();
    told->done.start();
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): `context`'s, let go of once it has answered
    auto* watcher = new QFutureWatcher<void>(context);
    // (Let go of as the asker is answered, or goes: from then on a thread Windows still holds counts as held,
    // lines::letGo. Heard only while the thread is there: it goes by itself once it has ended.)
    QObject::connect(watcher, &QObject::destroyed, thread, [thread] { lines::letGo(thread); });
    QObject::connect(watcher, &QFutureWatcherBase::finished, watcher, [watcher, answer, told] {
        std::optional<base::Result<Certificate>> came;
        {
            const QMutexLocker locked(&told->mutex);
            came = told->came;
        }
        answer(came.value_or(base::Error{"unreachable", "Device is unreachable"}));
        watcher->deleteLater();
    });
    QTimer::singleShot(withinMs, watcher, [watcher, answer] {
        answer(base::Error{"unreachable", "Device is unreachable"});
        watcher->deleteLater();
    });
    watcher->setFuture(told->done.future());
    QMetaObject::invokeMethod(home, [home, place, withinMs, told] { askThere(home, place, withinMs, told); });
}

} // namespace vnc

namespace {

// The web's NO_SAID: what a card says of a saved password the server does not take.
constexpr const char* rejected = "Login rejected - check the username and password";

// A text cut to `most` UTF-16 units before a character that takes two, and never through it (as the reader cuts a
// server's name: outside review c9a-2, L5).
QString wholeLeft(QString text, qsizetype most)
{
    if (text.size() > most) {
        text.truncate(text.at(most - 1).isHighSurrogate() ? most - 1 : most);
    }
    return text;
}

// What a line that ended for its certificate says, as every line of the program's says it (the web's _connect_error);
// nothing for any other end.
std::optional<base::Error> certificateEnd(DeviceLine::End why)
{
    if (why == DeviceLine::End::CertificateChanged) {
        return base::Error{"cert_mismatch", "Certificate does not match the one saved for this device"};
    }
    if (why == DeviceLine::End::CertificateUntrusted || why == DeviceLine::End::CertificateOfUnknownSigner) {
        return base::Error{"cert_invalid", "The device's certificate is not trusted"};
    }
    if (why == DeviceLine::End::CertificateUnread) {
        return base::Error{"cert_invalid", "The certificate saved for this device cannot be read. Look at its "
                                           "certificate and trust it again."};
    }
    return std::nullopt;
}

// A live that is refused at once (no screen to show), as the server's driver has one.
class RefusedLive final : public Live {
public:
    explicit RefusedLive(base::Error why) : m_why(std::move(why))
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

} // namespace

// What has the VNC server live for all who show it: its driver's one screen, and a live of its own for each who asks
// (ServerLive over it). Taken away (end): every live of it ends in those words, and none is opened after.
class VncOwnLive final : public OwnLive {
public:
    VncOwnLive(VncDevice* driver, QList<QPointer<ServerLive>>* lives, ServerLive::Limits limits)
        : m_driver(driver), m_lives(lives), m_limits(limits)
    {}

    std::unique_ptr<Live> open(Live::Need need) override
    {
        if (m_ended || m_driver == nullptr) {
            return std::make_unique<RefusedLive>(
                m_ended ? m_why : base::Error{"no_picture", "The VNC server is not on the wall"});
        }
        // (During a wait between ends in a row, whoever watches it live is let in at once: the web's enlarged view.)
        m_driver->watchedEnlarged();
        auto live = std::make_unique<ServerLive>(&m_driver->screen(), m_limits);
        live->setNeed(need);
        m_lives->append(live.get());
        return live;
    }

    void end(const base::Error& why) override
    {
        m_ended = true;
        m_why = why;
        if (m_driver == nullptr) {
            return;
        }
        for (const QPointer<ServerLive>& live : std::as_const(*m_lives)) {
            if (live == nullptr) {
                continue;
            }
            live->close(); // (what is down is let go of first)
            // NOLINTNEXTLINE(bugprone-exception-escape)
            QTimer::singleShot(0, live.data(), [live, why] {
                if (live != nullptr) {
                    emit live->closed(QString::fromStdString(why.code), QString::fromStdString(why.message));
                }
            });
        }
        m_lives->clear();
    }

private:
    QPointer<VncDevice> m_driver;
    QList<QPointer<ServerLive>>* m_lives;
    ServerLive::Limits m_limits;
    bool m_ended = false;
    base::Error m_why;
};

RfbScreen::Limits VncDevice::screenLimits()
{
    RfbScreen::Limits limits;
    limits.thing = u"the VNC server"_s;
    limits.openingMs = 5000;     // CONNECT_WAIT, TLS included
    limits.handshakeMs = 20000;  // HANDSHAKE_WAIT
    limits.lookPaceMs = 250;     // LOOK_PACE
    limits.livePaceMs = 40;      // LIVE_PACE
    limits.liveForMs = 2000;     // LIVE_FOR
    limits.idlePaceMs = 10000;   // IDLE_PACE
    limits.watchedForMs = 30000; // KEEPALIVE_IDLE
    limits.heartbeatMs = 60000;  // HEARTBEAT
    // Held while kept (every look at its status keeps it), and let go of a minute after the last: the wall's looks come
    // every few seconds for as long as it is on the wall (the web holds it for that long: LET_GO_AFTER None).
    limits.idleMs = 60000;
    // A connection that answered and lasted KEEPALIVE_LASTS served (the web's _stood: a frame and five seconds).
    limits.settledMs = 5000;
    limits.inputAfterMs = 0;    // INPUT_AFTER: a VNC server takes keys at once
    limits.inputWaitMs = 10000; // ACT_WAIT, LIVE_WAIT
    limits.keysAtOnce = 32;     // KEYS_AT_ONCE
    limits.usShift = false;     // US_SHIFT: a server's keysyms are its own
    return limits;
}

VncDevice::VncDevice(Where where, const QString& password, Pace pace, QObject* parent)
    : OwnDevice(parent), m_where(std::move(where)), m_pace(std::move(pace))
{
    m_clock.start();
    if (!password.isEmpty()) {
        // What the password is counted by, as every sign-in: the road, the address and the password (no user).
        m_mark = SignInPacing::what(u"login", QUrl(m_where.address), QStringView(), password);
    }
    QString handed(password.constData(), password.size()); // (a copy of its own, written over as it is taken)
    m_password = SecretText::takenFrom(handed);
    m_screen = std::make_unique<RfbScreen>(
        [self = QPointer<VncDevice>(this)](const RfbScreen::Answer& answer, const RfbScreen::Wanted& wanted) {
            if (self == nullptr || !wanted()) {
                answer(base::Error{"stopped", "Nobody waits for the VNC server any more"});
                return;
            }
            self->way(answer);
        },
        [this](const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed) {
            m_lives.removeIf([](const QPointer<ServerLive>& live) { return live == nullptr; });
            for (const QPointer<ServerLive>& live : std::as_const(m_lives)) {
                live->shown(picture, changed);
            }
        },
        m_pace.screen);
    m_screen->setHold(
        [this](const QString& code, bool served, qint64 lastedMs) { return hold(code, served, lastedMs); });
    connect(m_screen.get(), &RfbScreen::handshakeOver, this, [this] {
        // Let in: a password that went is one the server took; refusals in a row count from nothing again. And it was
        // heard (the web's _heard_at).
        if (m_out) {
            m_out.reset();
            pacing().accepted(m_mark);
        }
        m_bad = 0;
        m_gone = false;
        m_greetingRefused.reset();
        m_greetedAt = now();
    });
    connect(m_screen.get(), &RfbScreen::certificateShown, this, [this](const QByteArray& der) {
        // The certificate itself it is known by, the first time it is shown, for the wall as a file (as every driver
        // tells it); never another.
        if (!m_toldCertificate && m_where.knownByCertificate &&
            QCryptographicHash::hash(der, QCryptographicHash::Sha256) == m_where.certificate) {
            m_toldCertificate = true;
            emit showedItsCertificate(der);
        }
    });
    m_lookWait.setInterval(50);
    connect(&m_lookWait, &QTimer::timeout, this, [this] { lookedEnough(); });
    // A greeting that has not come in a message's time after the line's: nobody there either.
    m_greetingWait.setSingleShot(true);
    connect(&m_greetingWait, &QTimer::timeout, this, [this] { greetingCame(false); });
}

VncDevice::~VncDevice()
{
    m_lookWait.stop();
    m_greetingWait.stop();
    if (m_greeting != nullptr) {
        disconnect(m_greeting.get(), nullptr, this, nullptr);
        m_greeting->cut();
    }
    // (Nothing more is told: its screen goes with it, letting go of what is down.)
    disconnect(m_screen.get(), nullptr, this, nullptr);
    m_screen->close();
}

SignInPacing& VncDevice::pacing() const
{
    return m_pace.pacing != nullptr ? *m_pace.pacing : SignInPacing::program();
}

template <typename Done>
void VncDevice::later(Done done)
{
    QTimer::singleShot(0, this, std::move(done));
}

void VncDevice::watchedEnlarged()
{
    if (m_cuts > 1 && now() < m_cutWaitUntil) {
        m_cutWaitUntil = -1;
        m_screen->retryNow();
        static_cast<void>(m_screen->look());
    }
}

void VncDevice::identify(std::function<void(base::Result<Identity>)> done)
{
    // A session of its own, held as every look holds one, and waited for: its handshake, or what ended it (the web's
    // identify, CONNECT_WAIT + HANDSHAKE_WAIT and a second).
    const qint64 within = qint64{m_pace.screen.openingMs} + m_pace.screen.handshakeMs + 1000;
    const qint64 started = now();
    static_cast<void>(m_screen->keep());
    auto* waiting = new QTimer(this); // NOLINT(cppcoreguidelines-owning-memory): this driver's, let go of when over
    waiting->setInterval(50);
    connect(waiting, &QTimer::timeout, this, [this, waiting, started, within, done = std::move(done)] {
        static_cast<void>(m_screen->keep());
        std::optional<base::Result<Identity>> said;
        if (m_screen->letIn()) {
            const QString name = m_screen->serverName();
            const int minor = m_screen->rfbMinor();
            said = Identity{name, minor > 0 ? u"RFB 3.%1"_s.arg(minor) : QString(), name};
        } else if (const std::optional<base::Error> standing = m_screen->standing();
                   standing && standing->code != "no_stream") {
            said = *standing; // (one that waits for another sign-in to be answered is waited for)
        } else if (now() - started > within) {
            said = base::Error{"timeout", "The VNC server did not finish the handshake in time"};
        }
        if (said) {
            waiting->stop();
            waiting->deleteLater();
            done(std::move(*said));
        }
    });
    waiting->start();
}

void VncDevice::tryAfresh(const Where& where, const QString& password, const Pace& pace)
{
    (pace.pacing != nullptr ? *pace.pacing : SignInPacing::program()).forget(QUrl(where.address), QString(), password);
}

QStringList VncDevice::can()
{
    return {u"screen"_s, u"keys"_s, u"text"_s, u"live"_s, u"control"_s};
}

bool VncDevice::mayHaveBeenDone(const base::Error& error)
{
    // Keys that went part of the way (whoever asked was stopped meanwhile, or the session ended under them).
    return error.code == "dropped" || error.code == "stopped";
}

QString VncDevice::stateOf(const QString& code)
{
    if (code == u"auth_failed"_s || code == u"no_password"_s) {
        return u"auth"_s;
    }
    // It is there, and something is wrong: not "offline", which an alert would report as gone.
    if (code == u"no_sign_in"_s || code == u"bad_response"_s || code == u"cert_mismatch"_s ||
        code == u"cert_invalid"_s) {
        return u"error"_s;
    }
    return u"offline"_s;
}

std::optional<base::Error> VncDevice::passwordWaits() const
{
    if (pacing().onlyOut(m_mark)) {
        return base::Error{"no_stream", "Waiting for another sign-in to the VNC server to be answered"};
    }
    if (pacing().waitLeftMs(m_mark) > 0) {
        return pacing().times(m_mark) > 0
                   ? base::Error{"auth_failed", rejected}
                   : base::Error{"timeout", "The VNC server did not answer its password: it is asked again later"};
    }
    return std::nullopt;
}

void VncDevice::way(const RfbScreen::Answer& answer)
{
    // (Until it is answered with a way in: a refusal of its own opens no line, and hears nothing of the server.)
    m_lineOpened = false;
    RfbScreen::WayIn way;
    way.raw = true;
    way.address = vnc::lineAddress(m_where.address);
    way.knownByCertificate = m_where.knownByCertificate;
    way.certificate = m_where.certificate;
    way.vncAuthentication = true;
    if (!m_password.isEmpty()) {
        // One password at a time to the server, and a refused one not again before its wait (SignInPacing).
        if (const std::optional<base::Error> notYet = passwordWaits()) {
            answer(*notYet);
            return;
        }
        QString copy(m_password.view().data(), m_password.view().size());
        way.password = SecretText::takenFrom(copy);
        // ...held to when the answer goes, too: another driver of this server (the wall's, the Change box's) may have
        // made its way in meanwhile, and sent its own (outside review 2.4). Looked at and noted in one go, on this
        // thread: of two, the one whose challenge comes first sends, and the other nothing.
        way.passwordSent = [self = QPointer<VncDevice>(this)]() -> base::Result<void> {
            if (self == nullptr) {
                return base::Error{"stopped", "Nobody waits for the VNC server any more"};
            }
            if (const std::optional<base::Error> notYet = self->passwordWaits()) {
                return *notYet;
            }
            self->m_out = self->pacing().sent(self->m_mark, self->m_pace.signIns);
            return {};
        };
    }
    m_lineOpened = true;
    answer(std::move(way));
}

qint64 VncDevice::hold(const QString& code, bool served, qint64 lastedMs)
{
    // A password that went with the session: what it came to is noted, as every sign-in's.
    qint64 signInWait = 0;
    if (m_out) {
        const SignInPacing::Sent sent = *std::exchange(m_out, std::nullopt);
        signInWait = code == u"auth_failed"_s ? pacing().saidNo(m_mark, m_pace.signIns)
                                              : pacing().unanswered(m_mark, m_pace.signIns, sent);
    }
    // A session that opened its line and came as far as the password was answered by the server: it was there just now,
    // and its greeting is asked a whole Pace::stillThereEveryMs after this, not at the next look. (One that way()
    // refused by itself, a pacing's wait, heard nothing: the second reading of c9a, on difference 2.)
    const bool lineOpened = std::exchange(m_lineOpened, false);
    if ((code == u"auth_failed"_s || code == u"no_password"_s) && lineOpened) {
        m_greetedAt = now();
        m_gone = false;
        m_greetingRefused.reset();
    }
    if (code == u"auth_failed"_s) {
        return std::max(signInWait, pacing().waitLeftMs(m_mark));
    }
    if (code == u"no_password"_s) {
        return m_pace.noPasswordHoldMs;
    }
    if (code == u"bad_response"_s || code == u"no_picture"_s) {
        // Left alone for a while, twice as long each time it does so again (the web's _left_alone).
        ++m_bad;
        return std::min(m_pace.badHoldMostMs, m_pace.badHoldMs << std::min(m_bad - 1, 5));
    }
    if (code == u"no_stream"_s || code == u"stopped"_s) {
        return std::max<qint64>(1000, signInWait);
    }
    // It is down, or a connection of it ended.
    if (served && code != u"timeout"_s) {
        // One that served ended: connected to again at once the first time, and a rung later for each end after that
        // until one has served long (the web's _cut and CUT_LADDER); an hour without one forgets the run.
        const qint64 at = now();
        if ((m_cutAt >= 0 && at - m_cutAt >= m_pace.cutsForgottenAfterMs) || lastedMs >= m_pace.servedLongMs) {
            m_cuts = 0;
            m_cutSaid.clear();
        }
        m_cutAt = at;
        ++m_cuts;
        m_failures = 0;
        if (m_cuts <= 1) {
            return std::max<qint64>(0, signInWait);
        }
        const auto rung =
            static_cast<std::size_t>(std::min<int>(m_cuts - 1, static_cast<int>(m_pace.cutLadderMs.size())));
        const qint64 wait = std::max(signInWait, m_pace.cutLadderMs.at(rung - 1));
        // Said from its second end in a row on, in the web's words, with how long its connections served.
        m_cutWaitUntil = at + wait;
        m_cutSaid =
            u"The VNC server ends every connection after about %1 s: qKVM connects to it less often meanwhile, "
            u"and at once while somebody watches it enlarged"_s.arg((std::max<qint64>(0, lastedMs) + 500) / 1000);
        return wait;
    }
    ++m_failures;
    return std::max(signInWait, std::min(m_pace.backoffMostMs, m_pace.errorHoldMs << std::min(m_failures - 1, 10)));
}

Status VncDevice::status() const
{
    Status said;
    said.hasPicture = true;
    said.capabilities = can();
    // In a run of ends (from its third end in a row on), the connection between two waits says what the waits say:
    // "error" with its sentence, once for the run, and not "online" and "offline" by turns, each an alert (the web's
    // in_a_run). From the second end on, a wait says so.
    const bool inARun = m_cuts > 2 && !m_cutSaid.isEmpty();
    if (m_screen->letIn()) {
        said.state = inARun ? u"error"_s : u"online"_s;
        if (inARun) {
            said.errorCode = u"bad_response"_s;
            said.errorMessage = m_cutSaid;
        }
        said.signal = true;
        said.width = m_screen->width();
        said.height = m_screen->height();
        said.model = wholeLeft(m_screen->serverName(), 40);
        if (const int minor = m_screen->rfbMinor(); minor > 0) {
            said.version = u"RFB 3.%1"_s.arg(minor);
        }
        return said;
    }
    if (const std::optional<base::Error> standing = m_screen->standing()) {
        const QString code = QString::fromStdString(standing->code);
        // A password that waits, or a wait between ends in a row, of a server that is not there any more: offline; or
        // whose greeting's line showed another certificate than it is known by: that certificate's error, "error"
        // (the web's _gone_meanwhile_vnc, outside review c9a-2, L1 and L2).
        if (code == u"auth_failed"_s || code == u"no_password"_s || inACutWait(code)) {
            if (m_greetingRefused) {
                said.state = stateOf(QString::fromStdString(m_greetingRefused->code));
                said.errorCode = QString::fromStdString(m_greetingRefused->code);
                said.errorMessage = QString::fromStdString(m_greetingRefused->message);
                return said;
            }
            if (m_gone) {
                said.state = u"offline"_s;
                said.errorCode = u"unreachable"_s;
                said.errorMessage = u"Device is unreachable"_s;
                return said;
            }
        }
        if (inACutWait(code)) {
            said.state = u"error"_s;
            said.errorCode = u"bad_response"_s;
            said.errorMessage = m_cutSaid;
            return said;
        }
        said.state = code == u"no_stream"_s ? u"connecting"_s : stateOf(code);
        said.errorCode = code;
        said.errorMessage = QString::fromStdString(standing->message);
        return said;
    }
    said.state = u"connecting"_s;
    return said;
}

bool VncDevice::inACutWait(const QString& code) const
{
    return m_cuts > 1 && !m_cutSaid.isEmpty() && now() < m_cutWaitUntil && code == u"dropped"_s;
}

bool VncDevice::isStale() const
{
    return m_lookedAt < 0 || now() - m_lookedAt > m_pace.freshMs;
}

void VncDevice::look()
{
    if (m_looking) {
        return;
    }
    m_lookedAt = now();
    // A connection that has served long ends a run of ends: what was said of it says nothing any more (the web's
    // _pace).
    if (m_cuts > 0 && m_screen->letInForMs() >= m_pace.servedLongMs) {
        m_cuts = 0;
        m_cutSaid.clear();
    }
    // While a password waits to be offered again (or none is saved), and during a wait between ends in a row, whether
    // it is there at all is asked by its greeting.
    if (const std::optional<base::Error> standing = m_screen->standing()) {
        const QString code = QString::fromStdString(standing->code);
        if (code == u"auth_failed"_s || code == u"no_password"_s) {
            askWhetherItIsThere(m_pace.stillThereEveryMs);
        } else if (inACutWait(code)) {
            askWhetherItIsThere(m_pace.stillThereInAWaitMs);
        }
    }
    // The session is held (kept) by every look, and opened where none is and none stands in its way.
    const base::Result<void> kept = m_screen->keep();
    if (kept || m_screen->standing()) {
        later([this] { emit looked(); });
        return;
    }
    // A handshake under way: waited for a moment, as the web's status waits (STATUS_WAIT).
    m_looking = true;
    m_lookStartedAt = now();
    m_lookWait.start();
}

void VncDevice::askWhetherItIsThere(qint64 everyMs)
{
    if (m_greeting != nullptr || (m_greetedAt >= 0 && now() - m_greetedAt < everyMs)) {
        return;
    }
    m_greetedAt = now();
    m_greeted.clear();
    DeviceLine::Where where;
    where.address = vnc::lineAddress(m_where.address);
    where.knownByCertificate = m_where.knownByCertificate;
    where.certificate = m_where.certificate;
    DeviceLine::Limits limits;
    limits.letInWithinMs = m_pace.screen.openingMs;
    limits.goodbyeWithinMs = m_pace.screen.goodbyeWithinMs;
    limits.largestFrameBytes = 4096;
    limits.mostBytesASecond = qint64{64} * 1024;
    m_greeting = std::make_unique<DeviceLine>(where, limits);
    m_greeting->receiveRaw();
    connect(m_greeting.get(), &DeviceLine::bytesCame, this, [this](const QByteArray& bytes) {
        m_greeted += bytes.left(12 - m_greeted.size());
        if (m_greeted.size() >= 12) {
            greetingCame(m_greeted.startsWith("RFB "));
        }
    });
    connect(m_greeting.get(), &DeviceLine::ended, this, [this](DeviceLine::End why) { greetingCame(false, why); });
    m_greetingWait.start(m_pace.screen.openingMs + m_pace.screen.handshakeMs);
    m_greeting->open();
}

void VncDevice::greetingCame(bool there, std::optional<DeviceLine::End> why)
{
    if (m_greeting == nullptr) {
        return;
    }
    m_greetingWait.stop();
    // A line that ended for its certificate: something answers there, and shows another than the server is known by
    // (or one Windows does not trust). Said as the screen says it, and not as gone (outside review c9a-2, L1).
    m_greetingRefused = why ? certificateEnd(*why) : std::nullopt;
    m_gone = !there && !m_greetingRefused;
    // (Let go of later, never from inside what it says: closed with a goodbye where it greeted, and kept until it has
    // gone; cut where it said nothing that counts.)
    DeviceLine* const line = m_greeting.release();
    disconnect(line, nullptr, this, nullptr);
    if (there) {
        connect(line, &DeviceLine::gone, line, &QObject::deleteLater);
        if (!line->close()) {
            line->deleteLater();
        }
    } else {
        line->cut();
        line->deleteLater();
    }
}

void VncDevice::lookedEnough()
{
    if (!m_looking) {
        m_lookWait.stop();
        return;
    }
    if (m_screen->letIn() || m_screen->standing() || now() - m_lookStartedAt >= m_pace.statusWaitMs) {
        m_looking = false;
        m_lookWait.stop();
        emit looked();
    }
}

void VncDevice::afterLook(int withinMs, std::function<void(bool stillLooking)> then)
{
    if (!m_looking) {
        later([then = std::move(then)] { then(false); });
        return;
    }
    // Whichever of the two comes first, and that one alone.
    auto over = std::make_shared<bool>(false);
    auto heard = std::make_shared<QMetaObject::Connection>();
    auto run = std::make_shared<std::function<void()>>([this, over, heard, then = std::move(then)] {
        if (std::exchange(*over, true)) {
            return;
        }
        disconnect(*heard);
        then(m_looking);
    });
    *heard = connect(this, &OwnDevice::looked, this, [run] { (*run)(); });
    QTimer::singleShot(withinMs, this, [run] { (*run)(); });
}

void VncDevice::askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done)
{
    // Asked for, the screen is watched: asked for what changed at the looking pace.
    const base::Result<void> looked = m_screen->look();
    if (!looked) {
        later([done = std::move(done), why = looked.error()] { done(why); });
        return;
    }
    m_screen->still(width, [here = QPointer<VncDevice>(this), done = std::move(done)](base::Result<QByteArray> made) {
        if (here == nullptr) {
            return; // (the driver went with its screen: the wall tells whoever asked)
        }
        here->later([done, made = std::move(made)] {
            if (made) {
                done(Snapshot{made.value()});
            } else {
                done(made.error());
            }
        });
    });
}

void VncDevice::act(const Action& what, std::function<void(base::Result<Done>)> done)
{
    const QPointer<VncDevice> here(this);
    const ServerConsole::Still still = [here] { return here != nullptr; };
    const auto said = [done](const base::Result<void>& how) {
        if (!how) {
            done(how.error());
            return;
        }
        done(Done{});
    };
    // Keys pressed together, Ctrl+Alt+Delete; nothing of power or of a KVM (the web's VncDevice.act).
    if (what.kind == u"keys"_s) {
        m_screen->press(what.keys, still, said);
        return;
    }
    if (what.kind == u"ctrl_alt_del"_s) {
        m_screen->press(u"ControlLeft,AltLeft,Delete"_s, still, said);
        return;
    }
    // A text by the server's own keysyms, and Enter after it where that is asked: in the one typing, in the text's turn
    // of the keyboard (outside review 2.3), as the web types the text with a "\n" after it.
    if (what.kind == u"text"_s) {
        m_screen->type(what.text, still, said, what.enter);
        return;
    }
    later([done = std::move(done)] { done(base::Error{"invalid", "A VNC server cannot do that"}); });
}

void VncDevice::askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done)
{
    later([done = std::move(done)] { done(QList<WakeTarget>{}); });
}

void VncDevice::tidy() {}

void VncDevice::stopTyping()
{
    // (Its screen answers each typing it holds "stopped", and an Enter that was to follow one is not pressed: act().)
    m_screen->stopTyping();
}

std::unique_ptr<OwnLive> VncDevice::makeLive()
{
    return std::make_unique<VncOwnLive>(this, &m_lives, m_pace.live);
}

} // namespace qkvm::core
