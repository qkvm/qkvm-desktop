#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/device_line.h>
#include <qkvm/core/own_device.h>
#include <qkvm/core/rfb_screen.h>
#include <qkvm/core/server_live.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/wiped.h>

namespace qkvm::core {

// The addresses of VNC servers, as the web writes them (app/qkvm/vncdevice.py: vnc_url, address_of, is_vnc).
namespace vnc {

inline constexpr int defaultPort = 5900;

// An address as somebody typed it ("192.168.1.20", "192.168.1.20:5901", "vnc://Box.lan.:5902", "vncs://box", "[::1]")
// as it is kept: vnc://host[:port] (vncs:// for TLS from the first byte, as some controllers' VNC servers do it), the
// host in small letters without a dot at its end, an IPv6 address in brackets, a path left out, and the port left out
// where it is 5900. The host is held to what was typed as a KVM's is (kvm::addressFrom), as the web's vnc_url holds it
// to its device_url: letters of the twenty-six, digits, dots, hyphens and the underscore (a name of other letters in
// its xn-- form), or numbers written the one way. An error for what is no such address (another scheme, a user, no
// host, a host of other signs, a colon with no port behind it, a port that is none).
base::Result<QString> addressFrom(const QString& typed);
// Whether an address as somebody typed it names a VNC server by its scheme.
bool namesOne(const QString& typed);
// Whether an address as somebody typed it begins as a VNC server's does but with its scheme written amiss: vnc or vncs,
// in any case, then a colon or a slash, and not "://" ("vncs:/10.0.0.6:5901", "vncs//box", "vnc:/box", "vnc:5901").
// Read as an address without a scheme, it would be one at a host called "vncs" or "vnc", which anyone on the network
// may answer for. Both rules for an address refuse it, a VNC server's (addressFrom, with schemeAmissError()) and a
// KVM's (kvm::addressFrom, in a sentence that also names https:// for a KVM at a host of that name), whatever is chosen
// in the box that adds one (outside review c9a-3, L1; c9a-4, L2). The web reads these as such a host.
bool schemeAmiss(const QString& typed);
base::Error schemeAmissError();
// Where an address as addressFrom writes it goes: its host (an IPv6 address without brackets), its port, and whether
// over TLS.
struct Place {
    QString host;
    int port = defaultPort;
    bool tls = false;
};
Place placeOf(const QString& address);
// The connection's own address, for its line (DeviceLine::receiveRaw): http or https, the host and the port.
QUrl lineAddress(const QString& address);

// The certificate a vncs:// server shows (the web's fetch_certificate at a VNC server's own host and port,
// certificate_address): its SHA-256, the certificate itself (DER), and whether Windows trusts it for the address's name
// as it is. Read by a TLS handshake alone, with nothing written on the line before or after it, not a byte of RFB:
// whatever it is, it is looked at and the line is let go of. "unreachable" where nothing answers in `withinMs` or
// nothing listens, "no_certificate" for an address that is no vncs:// one, tls.h's damaged TLS where TLS is not
// Windows' own. Handed to `done` later, once, on `context`'s thread; never where `context` is gone by then.
// The handshake is spoken on a thread of the asking's own, never on `context`'s (outside review 2.2): Windows
// judges what is shown on the thread that speaks TLS, and where the certificate names a signer to be fetched from an
// address that does not answer (an AIA fetch on a PC without internet), it waits there for as long as it gives that.
// The asker is answered in `withinMs` all the same; what that thread still waits for ends by itself, and the program's
// end does not wait for it (line_threads.h).
inline constexpr int certificateWithinMs = 5000; // (CONNECT_WAIT, TLS included)
struct Certificate {
    QByteArray sha256;
    QByteArray der;
    bool trustedByWindows = false;
};
void askCertificate(const QString& address, int withinMs, QObject* context,
                    std::function<void(base::Result<Certificate>)> done);

} // namespace vnc

// A VNC server of this PC's own wall (kind "vnc"): any machine that runs one (a libvirt guest's, TigerVNC, QEMU, a
// controller's), by its address and password, read with the RFB reader (RfbScreen over the server's own connection),
// as the web's VncDevice. Its screen, live with keys and the pointer, typed text and keys; nothing of power, nothing of
// a KVM's (can()).
//
// * A session is held for as long as the server is on the wall: kept by every look at its status, asked for what
//   changed at the idle pace while nobody looks, at the looking pace while a card or a view asks for its picture, at a
//   viewer's while it is live (the web's IDLE_PACE, LOOK_PACE, LIVE_PACE). Nobody at the server is thrown out by it
//   (shared).
// * VNC authentication with the saved password, or None where the server asks for none; a server that asks where none
//   is saved is sent nothing ("no_password", and asked again after Pace::noPasswordHoldMs). While a password waits so
//   (rejected, or none), whether the server is there at all is asked by its greeting every Pace::stillThereEveryMs,
//   with nothing sent: gone, it is "offline"; showing another certificate on that line, it says that certificate's
//   error (the web's _gone_meanwhile_vnc). The same during a wait between ends in a row, every
//   Pace::stillThereInAWaitMs. The server counts as heard at a handshake, and where a session opened its line and
//   reached the password's question; a session refused before its line (the pacing's wait) heard nothing. The password
//   is counted as
//   every sign-in is (SignInPacing, road "login", no user): a rejected one is not offered again before its wait, and a
//   password that is out is not sent again beside it.
// * vncs://: the certificate is held as a kvmd KVM's HTTPS one (knownByCertificate and its SHA-256, or Windows' trust
//   for its name): another one ends the connection before a byte of RFB, the password with it, goes.
// * What ended a session, and when the next is asked for, as the web's ladder: a refusal or what RFB as this program
//   reads it is not, Pace::badHoldMs doubling up to Pace::badHoldMostMs; a server that is down, Pace::errorHoldMs
//   doubling up to Pace::backoffMostMs; a server that ends every connection that served, at once the first time and
//   then Pace::cutLadderMs rung by rung, until one has served Pace::servedLongMs.
//
// On the interface thread.
class VncDevice : public OwnDevice {
    Q_OBJECT

public:
    struct Where {
        QString address; // as vnc::addressFrom writes it
        bool knownByCertificate = false;
        QByteArray certificate; // its SHA-256
    };
    // The web's times; a test makes them short.
    struct Pace {
        RfbScreen::Limits screen = screenLimits();
        ServerLive::Limits live;
        qint64 statusWaitMs = 2000;      // a look waits this long for a handshake under way (STATUS_WAIT)
        qint64 freshMs = 4000;           // a look is due after this long (STATUS_TTL)
        qint64 noPasswordHoldMs = 30000; // a server that wants a password nobody saved (NO_PASSWORD_HOLD)
        qint64 stillThereEveryMs =
            30000; // ...and how often its greeting is asked while a password waits (STILL_THERE_EVERY)
        qint64 stillThereInAWaitMs = 120000; // ...and during a wait between ends in a row (STILL_THERE_IN_A_WAIT)
        qint64 badHoldMs = 15000;            // one that sent what is not taken, or refused (BAD_HOLD)...
        qint64 badHoldMostMs = 240000;       // ...twice as long each time, up to this (BAD_HOLD_MAX)
        qint64 errorHoldMs = 1000;           // one that is down (ERROR_HOLD)...
        qint64 backoffMostMs = 15000;        // ...twice as long each time, up to this (BACKOFF_MAX)
        std::array<qint64, 3> cutLadderMs = {15000, 60000, 300000}; // ends in a row of connections that served
        qint64 servedLongMs = 240000; // a connection that served this long ends a run of them (SERVED_LONG)
        qint64 cutsForgottenAfterMs = 3600000;
        SignInPacing::Steps signIns;          // a rejected password's waits, and one that came to nothing
        std::shared_ptr<SignInPacing> pacing; // none: the program's own
    };
    // The web's VNC paces for its screen (LOOK_PACE 0.25 s, LIVE_PACE 0.04 s, IDLE_PACE 10 s, HEARTBEAT 60 s, keys at
    // once: INPUT_AFTER 0), in the family's words ("the VNC server").
    static RfbScreen::Limits screenLimits();

    VncDevice(Where where, const QString& password, Pace pace, QObject* parent = nullptr);
    ~VncDevice() override;
    VncDevice(const VncDevice&) = delete;
    VncDevice& operator=(const VncDevice&) = delete;
    VncDevice(VncDevice&&) = delete;
    VncDevice& operator=(VncDevice&&) = delete;

    Status status() const override;
    bool isStale() const override;
    void look() override;
    void afterLook(int withinMs, std::function<void(bool stillLooking)> then) override;
    void askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done) override;
    void act(const Action& what, std::function<void(base::Result<Done>)> done) override;
    void askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done) override;
    void tidy() override;
    void stopTyping() override;
    std::unique_ptr<OwnLive> makeLive() override;

    // What it says of itself, read by a session of its own (the web's identify): the name it gives itself, as its model
    // and hostname, and the RFB version it speaks; why not where that cannot be had (its status's error, or
    // "timeout" where the handshake did not end in Pace::screen.openingMs and handshakeMs).
    struct Identity {
        QString model;
        QString version;
        QString hostname;
    };
    void identify(std::function<void(base::Result<Identity>)> done);
    // A person has typed this password for the VNC server at this address just now: tried as for the first time,
    // whatever was refused before (SignInPacing::forget, as every family's).
    static void tryAfresh(const Where& where, const QString& password, const Pace& pace);

    // What a VNC server can be asked (the web's `can`): its screen, keys, text, live and control.
    static QStringList can();
    // Whether an error of act() leaves open that it was done all the same.
    static bool mayHaveBeenDone(const base::Error& error);
    // What the web calls a status's state of a session that ended for this (VncDevice._state_of).
    static QString stateOf(const QString& code);

    // The screen, for a live and for a test.
    RfbScreen& screen() { return *m_screen; }
    // Somebody watches it enlarged or live (VncOwnLive): during a wait between ends in a row it is connected to at
    // once (the web's _picture with big), whatever the rung; not during any other hold.
    void watchedEnlarged();

private:
    void way(const RfbScreen::Answer& answer);
    // Why the saved password may not go to the server now: another sign-in with it is out (one at a time), or a
    // refused one's wait has not run out. Asked as the way is made, and again as the answer goes (outside review 2.4).
    std::optional<base::Error> passwordWaits() const;
    qint64 hold(const QString& code, bool served, qint64 lastedMs);
    void lookedEnough();
    // While a password waits (rejected, or none saved), or during a wait between ends in a row: whether the server is
    // there at all, by its RFB greeting read on a line of its own (vncs:// held to its certificate as every line), with
    // nothing sent; at most every `everyMs` after it was last heard.
    void askWhetherItIsThere(qint64 everyMs);
    // What came of it: its greeting (`there`), or the line's end (`why`).
    void greetingCame(bool there, std::optional<DeviceLine::End> why = std::nullopt);
    // A wait after ends in a row stands, for a session that ended with `code`.
    bool inACutWait(const QString& code) const;
    qint64 now() const { return m_clock.elapsed(); }
    SignInPacing& pacing() const;
    template <typename Done>
    void later(Done done);

    Where m_where;
    SecretText m_password;
    SignInPacing::Mark m_mark; // what the password is counted by
    Pace m_pace;
    QElapsedTimer m_clock;
    std::unique_ptr<RfbScreen> m_screen;
    // A password that went with a session's answer and has not been answered yet.
    std::optional<SignInPacing::Sent> m_out;
    int m_bad = 0;      // refusals in a row
    int m_failures = 0; // sessions in a row that found it down
    int m_cuts = 0;     // ends in a row of sessions that served
    qint64 m_cutAt = -1;
    // What it is said to do from its second end in a row on (the web's _cut_said), and until when the wait after the
    // last of them stands (its _cut_error's hold).
    QString m_cutSaid;
    qint64 m_cutWaitUntil = -1;
    bool m_looking = false;
    // The greeting asked while a password waits: its line, what came of it so far, when the server was last heard (or
    // its greeting last asked), its time, whether nothing answered there the last time, and the certificate's error
    // where its line showed another than the server is known by.
    std::unique_ptr<DeviceLine> m_greeting;
    QByteArray m_greeted;
    qint64 m_greetedAt = -1;
    QTimer m_greetingWait;
    bool m_gone = false;
    std::optional<base::Error> m_greetingRefused;
    // The last answer of way() opened a line (and was no refusal of its own, as the pacing's wait).
    bool m_lineOpened = false;
    bool m_toldCertificate = false;
    qint64 m_lookedAt = -1;
    qint64 m_lookStartedAt = 0;
    QTimer m_lookWait;
    QList<QPointer<ServerLive>> m_lives;
};

} // namespace qkvm::core
