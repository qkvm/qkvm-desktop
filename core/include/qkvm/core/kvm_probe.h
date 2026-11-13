#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/kvm_certificate.h>
#include <qkvm/core/kvm_times.h>

// Adding a KVM begins here (SPEC.md section 4, M3a): what somebody typed as its address is made
// the address that is used, and that address is asked what answers there, before any password
// goes anywhere.
namespace qkvm::core {

namespace kvm {

// The longest text that is read as an address at all. The server's own box takes no longer one.
inline constexpr qsizetype longestTypedAddress = 255;
// The most that is taken of an answer to "what are you?": it is asked of an address nothing is
// known about, and what a KVM says to it is a few hundred bytes. As the server has it.
inline constexpr qsizetype largestProbeAnswerBytes = qsizetype{64} * 1024;

// The address somebody typed ("192.168.1.20", "kvm.lan:8443", a pasted "https://192.168.1.20/#/"),
// written the one way. It is what the server makes of the same text (device_url, in
// ../app/qkvm/store.py), so that one KVM has one address here and there, and whether it "is on the
// wall already" can be asked by that address:
// * Typed with neither http:// nor https:// in front, it is HTTPS. Nothing but those two is taken.
// * What follows the host and its port is dropped: a path, and what stands behind a question
//   mark or a cross.
// * The host is in small letters, without the dot a name may end in, and made of letters, digits,
//   dots, hyphens and underscores. One that looks like numbers is four numbers written the plain
//   way, or none: "127.1" and "010.1.1.1" are 127.0.0.1 and 8.1.1.1 to Qt, and no address at all
//   to the server. IPv6 stands in brackets, and is written the short way.
// * The port every such address has anyway (443, 80) is not written. Port 0, a port that is no
//   number, and anything with an "@" in it are refused.
// What is refused is "invalid", with the server's sentence as its page shows it. What is taken has
// a scheme, a host, a port where one was typed, and nothing else; written out with
// toString(QUrl::FullyEncoded) it is the server's text.
//
// Not the server's rule to the letter. Stricter than the server in a few places, each said where
// it is done (kvm_probe.cpp): a tab or a line's end inside the text is not dropped and read over,
// brackets hold an IPv6 address and nothing else, and a host that Qt will not have as one is none
// here either. And in two it takes what a server may not, since the last word on a name is Qt's
// here and a library's there: a name with a part in its "xn--" form, which Qt reads by its own
// rule under the endings it has a list of (.de, .com), and takes under any other (.lan, .example,
// none) also where it refuses the same part under those; and a name of digits with single signs
// between them ("192-168-1-20"), which the library some servers connect with reads as numbers
// that are no address. (The tests of core/exchange show both, name by name; whoever writes a wall
// as a file is told of such a KVM.)
base::Result<QUrl> addressFrom(const QString& typed);
// Whether whoever typed this said how the KVM is to be spoken to, http:// or https://, or left it
// to the program. (The server asks the same of the same text, and treats the two differently
// for a JetKVM.)
bool schemeWasTyped(const QString& typed);

// Where what answers was asked, for whoever says what came of it: at the address a KVM of the wall
// stands at (which its card says), or at one it is to be moved to (which the box that changes it says).
enum class AskedAt { ItsAddress, ANewAddress };

// The same JetKVM's address the other way, as the web has it (other_way in ../app/qkvm/kvm.py): over
// HTTPS for one that is spoken to the plain way, and the plain way for one over HTTPS, at the same
// host. A JetKVM serves plain HTTP on port 80 and, once its owner turns HTTPS on, HTTPS on port 443
// beside it; it sends nobody from the one to the other. Nothing for an address with a port of its own
// (`address` writes none of the port its way has anyway): that is somebody's own arrangement, a
// forwarded port or a front door, and where the other way would be there is not known.
std::optional<QUrl> otherWayOf(const QUrl& address);
// Where a JetKVM is asked beside `address` (addressFrom(typed)) when it is added, as the web asks
// (detect_kind in ../app/qkvm/server.py): typed with neither http:// nor https://, the plain way at the
// same host and the same port as was typed, read from the text as typed ("kvm.lan:443" is
// http://kvm.lan:443, though `address` does not write the 443; "kvm.lan" is http://kvm.lan); typed
// with one of them, otherWayOf. Nothing where there is no such address.
std::optional<QUrl> bothWaysOf(const QString& typed, const QUrl& address);

} // namespace kvm

// Asks an address what answers there, with nothing that says who asks: no password, no session,
// no name. What the server does when a device is added (detect_kind in ../app/qkvm/server.py)
// and when it looks for devices (_what_is in ../app/qkvm/discover.py), this does here: three
// questions that each family of device answers to anyone, asked together, so that an address
// where nothing answers costs one wait and not three.
//
// Otherwise than the server in two things. There, what is neither a JetKVM nor a controller is
// taken to speak kvmd, and is found out when it is signed in to; here it is a kvmd only if it
// answered in kvmd's shape, and "something else" if it did not: a password is not sent to
// something to find out what it is. And an address is asked as it was given: the way beside it,
// where a JetKVM may answer, is asked by itself (askForAJetKvm), by whoever adds or changes one,
// and the two are weighed there (kvm::wayOf).
//
// On the interface thread. ask() comes back at once; the answer is handed to `done` later, on the
// same thread, exactly once, unless the probe is gone by then. Whoever is handed it does not end
// the probe there and then, but a turn of the loop later (deleteLater).
//
// Towards the address (SPEC.md section 8), as KvmdDevice keeps it: straight to it, whatever proxy
// this PC uses for the web; followed nowhere; Qt's store of cookies neither written nor read;
// signed on to nothing; the answer asked for unpacked, and a packed one not unpacked; every answer
// limited in size and in time.
//
// The one thing that is otherwise: over HTTPS a certificate Windows does not trust is no reason to
// stop. Nothing secret is sent, and the answer only says what is there. What was shown is told,
// and whether Windows objected to it: the certificate a KVM shows at this first contact is what
// it is known by from then on, unless Windows trusts it for its name as it is.
class KvmProbe : public QObject {
    Q_OBJECT

public:
    enum class Kind {
        Kvmd,          // a KVM that runs kvmd (PiKVM, GL.iNet Comet): it answered in kvmd's own shape
        JetKvm,        // a JetKVM: it says so itself
        Redfish,       // a server's management controller: it shows the root of its Redfish service
        SomethingElse, // something answers there, and is none of these
        Nothing,       // nothing answers there, in the time it is given
        // Nothing was asked there: the address is an HTTPS one, and what this copy of the program
        // speaks over TLS is not Windows' own (tls.h). Whoever asked for somebody tells them so
        // in the one code and sentence there is for that (damagedTls).
        NotAsked,
    };
    Q_ENUM(Kind)

    struct Found {
        Kind kind = Kind::Nothing;
        // Over HTTPS: the SHA-256 of the certificate shown on the line by which the answer came
        // that said what is there. Where none said so: on the first line that answered at all,
        // or, where none did, that showed one. Empty: plain HTTP, or no certificate was shown.
        QByteArray shown;
        // Whether Windows trusts that certificate for the address's name as it is. False as soon as
        // it objected to anything shown at this address while it was asked.
        bool trustedByWindows = false;
        // Of an asking that was held to what a KVM is known by (askAsKnown), and of no other: a
        // line showed what the KVM is not known by, and how (kvm_certificate.h). Nothing was
        // said on such a line, `kind` is Nothing whatever another line answered, and `shown` is
        // what that line showed. Nothing: every line that came up was the KVM's.
        std::optional<kvm::Objected> refused;
        // All that answered there was a gateway, which said that what stands behind it does not
        // answer: every question that was answered at all was answered with 502, 503 or 504, in
        // anything but kvmd's own shape. That is the rule the driver has for what a gateway says
        // of a KVM (kvmd_stream.h): in kvmd's own shape it is kvmd that speaks, and anything else
        // under one of those three numbers is the page of what stands before it (a reverse proxy
        // before a KVM that restarts, a tunnel whose far end is away). `kind` is SomethingElse
        // then, as of anything that answers and is none of the three: a gateway is something.
        // But its word says nothing of what is behind it, and whoever tells a person what was
        // found there does not say "this is no KVM" of it (LocalWall::notTheKvm). False wherever
        // one question was answered otherwise: with a page, a 404, a 401, a 500, by a device.
        bool saidByAGatewayAlone = false;
        // Every question to the address was refused outright as it called (connection refused):
        // nothing listens on its port. Read from the questions' own lines, and from nothing else. A
        // line that was let in, that took too long, or that failed in any other way, counts as one to
        // something that listens: where in doubt, something listens. (Whether a JetKVM beside it is
        // signed in to the plain way rests on this: kvm::wayOf.)
        bool nothingListens = false;
    };

    // What a KVM of the wall is known by, for an asking that is held to it: by a certificate,
    // and that certificate's SHA-256; or by none, and then by Windows' word for its name.
    struct Known {
        bool byCertificate = false;
        QByteArray certificate;
    };

    // The times an address is given: for each step what the driver that signs in afterwards gives
    // the KVM for it, from the one place both have them from (kvm_times.h). Never less: what is
    // not asked is not signed in to, so an asking that gave up sooner than the driver would kept
    // a KVM off the wall that the driver could have reached. (They were the server's once, two
    // seconds to be let in and five in all, under a driver with four and a half and more.) A test
    // makes them short.
    struct Pace {
        int connectWithinMs = kvm::letInWithinMs; // to be let in at all
        int quietMs = kvm::quietMs;               // the longest nothing may come while an answer is waited for
        // And all of it, from the asking to the last byte: as long as a driver's question may take.
        int withinMs = kvm::letInWithinMs + kvm::quietMs + kvm::transferMs;
        // The longest nothing may come while a server's controller is asked for its service root, where that is
        // longer than `quietMs` (0: it is not): as long as a controller's driver waits for it before a sign-in
        // (RedfishDevice::Pace::rootUnsignedMs, the web's ROOT_WAIT_TIMES). An older board took nine to twenty
        // seconds to show it, and is no "nothing" for that. Its question may take as much longer in all.
        int rootQuietMs = 0;

        // The times of an asking that goes before a sign-in by a driver with these times of its
        // own (KvmdDevice::Pace: connectWithinMs, quietMs, transferMs): the same, step for step;
        // and, before a controller's, its root's (`driversRootMs`, as above).
        static constexpr Pace beforeASignInWith(int driversConnectMs, int driversQuietMs, int driversTransferMs,
                                                int driversRootMs = 0)
        {
            return Pace{driversConnectMs, driversQuietMs, driversConnectMs + driversQuietMs + driversTransferMs,
                        driversRootMs};
        }
    };

    explicit KvmProbe(QObject* parent = nullptr);
    explicit KvmProbe(Pace pace, QObject* parent = nullptr);

    // What answers at `address`: one that addressFrom has made. Of an address that is none (no
    // host, another scheme than the two) nothing is asked, and nothing answers. Nor is anything
    // asked of an HTTPS address where TLS is not Windows' own (tls.h): Kind::NotAsked. Several
    // addresses may be asked at once; each is told by itself.
    void ask(const QUrl& address, std::function<void(Found)> done);
    // The same three questions, asked of a KVM that is known already, and of nothing else that
    // answers at its address: for a KVM that came to the wall with nothing asked (from a file),
    // before its password goes anywhere (local_wall.h). Its lines are held as the lines of the
    // KVM's driver are (KvmdDevice, kvm_certificate.h), which is the one thing that is otherwise
    // than in ask(): over HTTPS a line that shows another certificate than the one the KVM is
    // known by, or, where it is known by none, one that Windows does not trust for its name, is
    // not passed over. That is looked at when the two have agreed on how to speak and before a
    // byte of a question goes out: what is not the KVM hears nothing at all, not even the three
    // questions. `Found::refused` says so, and then nothing that answered counts. Over plain
    // HTTP there is nothing to hold a line to, and it is asked as ask() asks.
    void askAsKnown(const QUrl& address, const Known& known, std::function<void(Found)> done);
    // A JetKVM's question alone (`GET /device/status`), asked as ask() asks it: for the way beside an
    // address a JetKVM may answer at (kvm::bothWaysOf, kvm::otherWayOf), where nothing else is asked.
    // Its certificate is not judged, as ask() does not, and is told (`shown`, `trustedByWindows`): only
    // `kind` JetKvm, of an answer that says `isSetup`, counts; anything else that answered is
    // SomethingElse. Nothing travels on its line but that question.
    void askForAJetKvm(const QUrl& address, std::function<void(Found)> done);

private:
    void asked(const QUrl& address, const std::optional<Known>& heldTo, bool jetKvmAlone,
               std::function<void(Found)> done);

    Pace m_pace;
    // Whether TLS is Windows' own: chosen when the probe is made, before any of its lines is.
    bool m_windowsOwnTls;
};

} // namespace qkvm::core
