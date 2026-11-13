#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <qkvm/base/result.h>
#include <qkvm/core/exchange.h>
#include <qkvm/core/exchange_work.h>
#include <qkvm/core/local_wall.h>

// This PC's wall and "the wall as a file" (exchange.h), joined: a file's KVMs are taken onto this
// PC's wall, and this PC's wall is written to such a file (SPEC.md section 4, M3c). What the boxes
// on the screen call; nothing here is a box.
namespace qkvm::core {

// Takes devices, saved actions and shared settings from a portable file, and writes selected
// sections back. Server alerts and desktop-only preferences are not portable here.
//
// A file may be made by anybody, and it holds the passwords of KVMs. So:
//
// TAKING is two steps, and the first changes nothing.
// 1. layOut() reads the file and lays out its devices, actions and shared settings: whether each can be
//    taken and as what, or why not, with everything a person has to be shown before saying yes
//    (`Offered`). List metadata holds neither passwords nor saved text; actionText() reveals
//    one action's text only for the matching, still-current preview.
// 2. take() takes, of what was laid out last, the items that are named by their places. A
//    place that was not laid out, and one that was laid out as not to be taken, takes nothing.
//    Each device is taken or is not, and what came of each is handed back; the wall's list is
//    written once for all devices (LocalWall::takeFromAFile). Actions are saved individually
//    and shared settings last, so a failure is reported per item without undoing earlier ones.
// Neither step asks anything of any address, and neither sends anything anywhere.
//
// What is KEPT here between the two steps, and for how long:
// * Of the devices that can be taken, and of no other: what the wall would be handed for each
//   (where it is, who signs in, what it is to be known by, its certificate, its password). A
//   device that cannot be taken (another family, an address that is none, one that is on the
//   wall already) is kept as the reason it was laid out with, and nothing else of it: not its
//   password, which nothing here could ever need.
// * Each password and saved text in memory of its own that is written over before it is given back
//   (SecretBytes), on every way out of here.
// * Until the first of: a taking from it (take() ends the laying-out it takes from, whatever
//   came of each device: one laying-out, one taking, and whoever wants more reads the file
//   again); another laying-out; forget(); this going; and `keptForMs` after it was laid out (ten
//   minutes: long enough to read down a list, and not as long as a box is left open over
//   lunch). After any of those, take() says "not_laid_out": read the file again.
// What is NOT wiped, since its type cannot be: what the reader handed over (exchange::Content,
// whose texts are Qt's: exchange.h says so, and it is let go as soon as it was laid out); the
// text a password is made for the moment the wall is handed it (written over by hand as soon
// as the wall has it, which holds for the one copy that is made here); and whatever the wall
// and the secret store make of it on its way to Windows.
//
// What can be taken, and as what:
// * Only a KVM of a family this program speaks to by itself ("kvmd", "jetkvm"), as that family. A
//   server's management controller is laid out as what it is, and is not taken ("not_yet").
// * Never twice. A device at a place where a KVM of the wall is (LocalWall::atOnePlace), and
//   one whose file names the certificate a KVM of the wall is known by, is laid out as on the
//   wall already, with which one ("duplicate", "same_device": the codes a box that adds a KVM
//   has for the two). Two devices of one file that are the same by either measure: the first
//   that can be taken is the one, and the other says which that is. So a second taking of one
//   file adds nothing.
// * What it is called, who signs in, its group and its password are held to the rules a box
//   that adds a KVM has for what is typed (kvm_adding.h): what this PC's wall could not keep,
//   or read back at the next start, is not taken ("invalid").
// * What a KVM from a file is known by here, once it is on the wall:
//   - Over HTTPS, where the file says "pinned" and names the certificate: by that
//     certificate's fingerprint and by nothing else. The wall keeps the certificate itself too.
//   - Over HTTPS, where the file says Windows vouches ("system"): by Windows' word.
//   - Over HTTPS, where the file says "pinned" and names no certificate, or says that the
//     certificate is not checked ("insecure"): NOT by whatever it shows first, which is what a
//     QKVM server makes of such a device. It is taken as known by Windows' word. A KVM's own
//     certificate does not have that: then its card says that its certificate is not trusted,
//     nothing is sent to it, and the way on is the one the program has for a certificate nobody
//     has said yes to (KvmCertificateReview, kvm_editing.h), which puts it before a person.
//     Where Windows DOES trust what it shows for its name, it is asked what it is and signed in
//     to on that word, the file's password with it, and nobody has looked at any certificate:
//     by Windows' word, which is the rule, and is what is laid out for it.
//   - A device that has no name in its file is taken without one, and is called by where it
//     is. (The reader names such a device by its address, as a server does; kept as a name,
//     that would stay the address it HAD after a move. The box that adds a KVM by hand still
//     does keep the address as the name where none is typed: DESIGN.md section 11.)
//   - Over plain HTTP: by nothing, and its password goes in the clear, which is laid out.
// * Every KVM that is taken is on the wall as "not asked yet" (local_wall.h): the wall asks what
//   answers at its address before its password goes anywhere, on lines that are held to what
//   the KVM is known by.
//
// WRITING hands back the file in the shape the writer has for it (exchange::Written): the text
// only for whoever says that the lines beside it were looked at. Beside the file, on top of the
// writer's own lines: one line that says what of a wall is not in the file (server alerts,
// accounts, app password, layout and desktop preferences); and a line for each KVM whose password
// Windows did not hand out, which is written without. A plain file (no passphrase) has no
// password in it, and none is read from the store for it. A passphrase that is none
// (exchange::passphraseRefused) seals nothing.
//
// A file of QKVM's holds four million signs (exchange::largestPlain), and a certificate may be
// twenty thousand: a wall of many KVMs can be larger than a file may be WITH its certificates,
// and hardly ever is without them (two thousand KVMs with the longest of everything else are
// under two million where that is written in ASCII; a sign beyond ASCII is written as six, so a
// wall of two thousand whose names and passwords are long and of such signs throughout is
// larger than a file may be all the same: its second writing is refused as the first was, and
// nothing is written). So where the writer says that the wall holds more than a file can, and a
// certificate was in it, the wall is written once more without the certificates themselves:
// every KVM is in the file, each that is known by a certificate as "pinned" with none named.
// The first line beside the file says that this was done and why, and the writer's own line
// follows for each such KVM (a server that reads the file takes the first certificate it
// shows; this program has each looked at by a person). All of them or none: which KVMs keep
// theirs is not chosen by their place on the wall.
//
// On the interface thread. The slow work (the passphrase's) is done off it (exchange::FileWork),
// one file at a time for the whole program ("busy"); layOut() and write() come back at once, and
// what came of it is handed to `done` later, on the same thread, exactly once, unless this is
// gone by then, or whoever asked is (`asker`: once that is gone, `done` is not called). Without
// an asker nothing is worked on. take() answers there and then.
//
// The wall says aloud that it has changed while it takes (LocalWall::changed), and whoever hears
// that may end this there and then: take() touches nothing of this after it.
//
// The wall outlives it.
class WallExchange : public QObject {
    Q_OBJECT

public:
    // Unique selection places across the three portable sections. Device places keep their
    // file numbering; actions follow the bounded device range, and settings follow actions.
    static constexpr qsizetype firstActionPlace = exchange::mostDevices + 1;
    static constexpr qsizetype settingsPlace = firstActionPlace + exchange::mostActions;
    struct Parts {
        bool devices = true;
        bool actions = true;
        bool wall = true;
    };
    // Where a device's address is, as far as its signs say. No name is looked up.
    enum class Where {
        Elsewhere,
        // This PC itself: "localhost" or a name that ends in ".localhost", 127.x.x.x, [::1],
        // and the address of no place (0.0.0.0, [::]), which many a system takes for itself.
        ThisPc,
        // Of the kind that no router passes on (link-local): 169.254.x.x, [fe80::...]. Such a
        // number means whatever has it on the network this PC is plugged into.
        NotRouted,
    };
    Q_ENUM(Where)

    // What a KVM that is taken will be known by on this PC.
    enum class KnownBy {
        Nothing,        // it is spoken to over plain HTTP
        ItsCertificate, // the one the file names, by its fingerprint
        WindowsWord,    // a certificate that Windows trusts for its name
    };
    Q_ENUM(KnownBy)

    // One device of a file, laid out for a screen to show as it is: plain values and whole
    // sentences. Everything in a file is text from outside; what stands here was looked at
    // sign for sign, and no password is in any of it.
    struct Offered {
        enum class Kind { Device, Action, Settings };
        Kind kind = Kind::Device;
        // Its place in the file's list of devices, counted from 1, as the sentences of what
        // was left out count: take() names a device by this.
        qsizetype place = 0;
        // Its name and its address, in one line and never the one without the other: the name
        // as exchange::nameSetOff makes it, then the address as below. Where there is no name
        // to show, the address alone; empty where the address cannot be shown.
        QString nameAndAddress;
        // Its address as it will be kept: "https://host", "http://host:port", every sign of it
        // plain ASCII, a name in its "xn--" form left in that form. Empty: it cannot be shown.
        QString address;
        // Its address is a name with a part in its "xn--" form.
        bool nameInXnForm = false;
        Where where = Where::Elsewhere;
        // It is spoken to over plain HTTP: its password goes in the clear.
        bool passwordInTheClear = false;
        // What the file says of how its certificate is checked, over HTTPS: "pinned", "system"
        // or "insecure"; and whether the file names a certificate for it. Over plain HTTP
        // there is nothing to say: empty, and none.
        QString fileSaysOfTls;
        bool fileNamesACertificate = false;
        // What it will be known by here; and, where the file names a certificate, that
        // certificate's SHA-256 as the page writes one (kvm::fingerprint), to be compared with
        // what the KVM's own page shows.
        KnownBy knownBy = KnownBy::Nothing;
        QString fingerprint;
        // The file would have it known by whatever it shows first (no certificate named, or
        // "insecure"), and this program does not take it so: it is taken as known by Windows'
        // word. What Windows does not trust is then put before a person before anything is sent
        // to it; what Windows trusts is signed in to on that word, with nobody having looked.
        bool notByWhatItShowsFirst = false;
        // The file has a password for it. (Without one it is taken all the same, and its
        // password is to be typed on this PC.)
        bool hasPassword = false;
        // Nothing was asked at its address yet. True of every device of every file, and so in no
        // sentence of `says`: whoever shows the list says it once for all of them, with what
        // follows from it (each KVM that is taken is asked what it is before its password goes
        // anywhere).
        bool nothingAskedYet = true;
        // Whether it can be taken. Where not, why: "not_yet" (a family this program does not
        // speak to), "invalid" (something of it that this PC's wall does not keep),
        // "duplicate" or "same_device" (on the wall already, or the same as an earlier device
        // of the file), "too_many" (the wall is full).
        bool canBeTaken = false;
        QString whyNot;
        // Of one that is "same_device": it is so by the number it gives for itself, which the file carries and which
        // is said, not proven (the web: "says it is the same device as"); not by a certificate.
        bool saysItIsTheSame = false;
        // That, in one sentence: as what it can be taken, or why it cannot, and which KVM of
        // the wall or which device of the file is in its way.
        QString verdict;
        // And what else is to be said of it alone, each in a sentence, in this order: where its
        // address is (this PC, not routed) and the "xn--" form; and, of a device that can be
        // taken, how it is spoken to and what it will be known by, and whether the file has
        // its password. One that cannot be taken says why (`verdict`), and nothing of how it
        // would be known or of its password. No sentence holds a certificate's SHA-256, which
        // is `fingerprint`; and none says what is so of every device (`nothingAskedYet`).
        QStringList says;
        bool initiallyTicked = true;
        bool hasText = false;
    };

    // What whoever takes the list of devices says in taking it: that what stands beside the
    // list was looked at first. One word, said by its name and in no other way (as
    // exchange::NotInIt is).
    class BesideTheList {
    public:
        static const BesideTheList wasLookedAt;

    private:
        constexpr BesideTheList() noexcept = default;
    };

    // What a file holds, laid out. The devices and what stands beside them do not come apart
    // by oversight: there is no way to the list that does not say that the lines were looked at.
    class LaidOut {
    public:
        LaidOut() = default;

        // Which laying-out this is: take() takes from this one, and from no other.
        quint64 number() const noexcept { return m_number; }
        // Every sentence of what the reader left out of the file (exchange::Content::leftOut),
        // as it is; unsupported server alerts and unencrypted secrets are also explained here.
        const QStringList& besideTheList() const noexcept { return m_beside; }
        // The devices, in the file's order.
        const QList<Offered>& devices(BesideTheList /*wasLookedAt*/) const noexcept { return m_devices; }
        const QList<Offered>& otherParts() const noexcept { return m_other; }

    private:
        friend class WallExchange;

        quint64 m_number = 0;
        QList<Offered> m_devices;
        QList<Offered> m_other;
        QStringList m_beside;
    };

    // What came of one device that was to be taken: the wall's name for it; or why it is not
    // on the wall, in the code and the sentence it was laid out with, or the wall's own
    // ("not_saved" where Windows would not keep its password or the list could not be written,
    // "not_laid_out" where it was not laid out by the laying-out that was named).
    struct Took {
        qsizetype place = 0;
        base::Result<QString> came;
    };

    // For how long what was laid out is kept, where nothing ends it sooner.
    static constexpr int keptForMsAsItIs = 10 * 60 * 1000;

    explicit WallExchange(LocalWall& wall, QObject* parent = nullptr);
    // With the cost a file is sealed with, and the passphrase's work, of a test's choosing.
    WallExchange(LocalWall& wall, exchange::Sealing sealing, QObject* parent = nullptr);
    // And with the time what was laid out is kept.
    WallExchange(LocalWall& wall, exchange::Sealing sealing, int keptForMs, QObject* parent = nullptr);
    ~WallExchange() override;

    // Lays out what a file holds: `file` is its bytes as they are, `passphrase` what somebody
    // typed for it, empty for none. Refused as exchange::read refuses a file, and "busy". What
    // was laid out before this is forgotten there and then, whatever comes of this.
    void layOut(const QByteArray& file, const QString& passphrase, QObject* asker,
                std::function<void(base::Result<LaidOut>)> done);
    // Takes, of what `laidOut` laid out, the devices at these places: each once, in the
    // file's order, however they were named. What comes back says of each place that was named
    // what came of it. Nothing is taken where `laidOut` is not the laying-out that was made
    // last by this, or that one was forgotten.
    //
    // Only what was laid out as to be taken is taken, however the wall has changed since: a
    // device that was laid out as on the wall already is not taken because that KVM was taken
    // off meanwhile (nobody was shown it as one that would be taken). And each that was is
    // looked at once more against the wall as it is now: one that came onto the wall since is
    // not taken a second time.
    //
    // The laying-out is over with it, whatever came of each device: what was kept of the file
    // is forgotten before the wall is handed anything, and a second taking from it says
    // "not_laid_out". (Not where no place was named: nothing was taken, and nothing is over.)
    QList<Took> take(quint64 laidOut, const QList<qsizetype>& places);
    // Forgets what was laid out, the passwords of the file with it, written over: for when the
    // box that showed it has gone. Nothing can be taken from it after that. A file that is
    // still being read is given up with it: when its reading ends, nothing of it is laid out
    // or kept, and whoever asked is told "given_up".
    void forget();
    // How many passwords of a file are held here at this moment: those of the devices that
    // were laid out as to be taken and came with one. None once what was laid out is forgotten.
    qsizetype passwordsHeld() const;

    // Writes this PC's wall to a file: sealed with `passphrase`, and then with each KVM's
    // password; or plain where the passphrase is empty, and then with none. Refused as
    // exchange::write refuses, "busy", and with a passphrase that is none
    // (exchange::passphraseRefused, "invalid").
    void write(const QString& passphrase, QObject* asker, std::function<void(base::Result<exchange::Written>)> done);
    void writeParts(Parts parts, const QString& passphrase, QObject* asker,
                    std::function<void(base::Result<exchange::Written>)> done);
    // The text of an import action, only on a deliberate reveal while its preview still exists.
    QString actionText(quint64 laidOut, qsizetype place) const;

    // A file is being worked on, by this or by anything else of the program.
    bool isBusy() const { return m_work.isBusy(); }
    // For how long what was laid out is kept here, in milliseconds: `keptForMsAsItIs`, unless a
    // test chose another. For whoever says it to a person.
    int keptForMs() const { return m_keptForMs; }

signals:
    // What was laid out was kept for as long as it is kept, and is forgotten from this moment:
    // for whoever shows it (a box that was left open), to show it no longer. Said of that
    // forgetting alone, and after it. A taking, another laying-out and forget() are the doing
    // of whoever asked for them, who knows.
    void keptNoLonger();

private:
    // One device of what was laid out last that can be taken, as the wall would be handed it:
    // with its password in memory that is written over when this goes (its signs as a text of
    // the program's has them, two bytes each; none: the file had none for it).
    struct Takeable {
        qsizetype place = 0;
        OwnKvm kvm;
        SecretBytes password;
        QString certificatePem;
    };
    // And one that cannot: why, as it was laid out.
    struct Refused {
        QString code;
        QString verdict;
    };
    // What was laid out last: what can be taken of it, in the file's order; and of each place
    // that cannot, why. Nothing else of the file.
    struct Kept {
        quint64 number = 0;
        std::vector<Takeable> takeable;
        QHash<qsizetype, Refused> refused;
        struct Saved {
            qsizetype place = 0;
            exchange::Action action;
            SecretBytes text;
        };
        std::vector<Saved> actions;
        std::optional<exchange::WallSettings> settings;
    };

    LaidOut laidOutFrom(const exchange::Content& content);
    // What was laid out is taken from no more; a reading that is under way goes on.
    void forgetWhatWasLaidOut();

    LocalWall& m_wall;
    std::optional<Kept> m_laidOut;
    // Counted up by every forgetting: a reading that began under another number was given up.
    quint64 m_readings = 0;
    // Forgets what was laid out when it has been kept for as long as it is kept.
    int m_keptForMs = keptForMsAsItIs;
    QTimer m_forgetSoon;
    // Last, so that it goes first: its work is told to stop and waited for before the rest goes.
    exchange::FileWork m_work;
};

inline constexpr WallExchange::BesideTheList WallExchange::BesideTheList::wasLookedAt{};

} // namespace qkvm::core
