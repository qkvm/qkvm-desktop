#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/own_sign_in.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wiped.h>

// Adding a KVM ends here (SPEC.md section 4, M3a): what was typed into the box is read by the
// server's rules, the address is asked what answers there, the KVM is signed in to once, and only
// then is it on this PC's wall.
namespace qkvm::core {

namespace kvm {

// The most that is kept of what is typed beside an address. The server's own limits: what this
// PC's wall takes, a QKVM server's takes too. Counted as the settings count what they read back
// (settings.cpp), by the places a text takes here; a sign from far up, many an emoji among them,
// takes two, and is one to the server. So a name of such signs is too long here at half the length
// the server takes, and what is taken here is read back whole at the next start.
inline constexpr qsizetype longestName = 80;
inline constexpr qsizetype longestGroup = 40;
inline constexpr qsizetype longestUser = 64;
inline constexpr qsizetype longestPassword = 256;

// What is kept of a name, a group and a user name as each was typed, by the server's rule for such
// a line (_a_line in ../app/qkvm/server.py, as_a_line in ../app/qkvm/store.py):
// * The blanks around it are not kept, and a tab is a blank. A blank is what the server's language
//   takes for one: a no-break blank and a wide one among them, and what ends a line where it
//   stands at an end.
// * A blank of no width (U+200B, U+2060, U+FEFF) is nothing: typed without knowing, pasted
//   without seeing.
// * What ends a line inside it, what moves the cursor, half of a sign that takes two places, and
//   what turns the direction of the writing round (a name could be made to read as another one
//   with those) make it no line, and it is refused.
// * A name keeps the blanks it was typed with. In a group a run of blanks is one blank: two groups
//   are not told apart by how they were spaced.
// * No user name is "admin", the one every such KVM comes with.
// * What is kept is no longer than the limit above; and what was typed, blanks and all, no longer
//   than four times that (the server's own bound on what it reads at all).
// What is refused is "invalid", with the server's sentence as its page shows it: the field as the
// server calls it ("name", "group", "username"), a colon, and the rule. Never what was typed.
base::Result<QString> nameFrom(const QString& typed);
base::Result<QString> groupFrom(const QString& typed);
base::Result<QString> userFrom(const QString& typed);
// The same rule for a line of another kind that has to be there (the name of a saved action:
// own_actions.h): by what the server calls the field and by the most that is kept of it. Where
// nothing is kept of what was typed (nothing was, or blanks alone), it is refused in the server's
// words for that ("a name is needed"), as the three above are refused.
base::Result<QString> neededLineFrom(const QString& typed, QLatin1StringView called, qsizetype longest);

// The name a KVM gets where none was typed: what it calls itself (`hostname`, as
// KvmdDevice::identify read it), unless that is nothing (a JetKVM calls itself nothing) or what
// every KVM of its make is called ("glkvm", "pikvm", "localhost", "localhost.localdomain", in
// letters small or large). Then it is where the KVM is: its host, and its port where it has one of
// its own. No longer than a name may be.
QString nameWhereNoneWasTyped(const QString& hostname, const QUrl& address);

// What whoever adds a KVM and whoever changes one (kvm_editing.h) both go by, so that each rule
// is written once:
//
// A password is taken as it was typed, the blanks around it too (they may be part of it), and
// only no longer than the server takes one. Refused like a line above ("invalid", "password"),
// and never with anything of it in the sentence.
base::Result<void> passwordFrom(const QString& typed);
// Why what a probe found at an address is not signed in to: nothing ("unreachable"), something
// that is no KVM this program speaks to ("not_a_kvm"). A device of a family this program speaks to
// by itself (one that runs kvmd, a JetKVM, a server's management controller: familyOf) is signed in
// to, and of one this is not asked.
base::Error whyNotSignedInTo(KvmProbe::Kind kind);
// What the asking of what answers at an address finds of a KVM of a family (OwnKvm::kind); nothing
// for a family this build does not speak to by itself, which nothing found is. Of every family that
// OwnKvm::namesAFamily names, something.
std::optional<KvmProbe::Kind> foundAs(const QString& family);
// And the other way round: the family (OwnKvm::kind) of what the asking found, where it is one this
// program speaks to by itself ("kvmd", "jetkvm", "redfish"); empty for anything else, which is signed
// in to by nobody (whyNotSignedInTo). What a KVM that is added is kept as.
QString familyOf(KvmProbe::Kind kind);
// Why what answers at a KVM's address is not that KVM, where it is a KVM of a family this program
// speaks to all the same, and not the KVM's (`family`): "other_family", in a sentence that names
// both, and says that the one that answers is added as a KVM of its own. Nothing is signed in to
// there for the KVM: its password is for a KVM of its own family. (A KVM is never made one of
// another family by a change: LocalWall::change.) Of the address the KVM stands at (`at`), it says
// to take this one off the wall first: the other is not added at a place where a KVM of the wall is.
base::Error ofAnotherFamily(KvmProbe::Kind found, const QString& family, AskedAt at);
// What was typed for a KVM that is added, and what came of asking it both ways (kvm::bothWaysOf):
// for wayOf, which asks nothing itself.
struct Ways {
    QUrl address;              // what was typed, made an address (addressFrom): HTTPS where nothing was typed
    bool schemeTyped = false;  // http:// or https:// was typed
    bool portTyped = false;    // nothing was typed before it, and a port was typed (443 and 80 too)
    KvmProbe::Found atAddress; // the three questions at `address`
    std::optional<QUrl> twin;  // the way beside it, where there is one
    KvmProbe::Found atTwin;    // and the JetKVM's question alone there
};
// What is asked for a KVM that is added (or a JetKVM that is moved, kvm_editing.h) at `address`, as
// `typed`: the way beside it (`otherWay` of it where http:// or https:// was typed, bothWaysOf's plain
// way where neither was; none where that is `address` itself), whether a scheme was typed, and
// whether a port was typed where none was (443 and 80 too, which `address` does not write). Nothing
// is asked here, and nothing was found yet: that is wayOf's to weigh.
Ways waysFor(const QString& typed, const QUrl& address,
             const std::function<std::optional<QUrl>(const QUrl&)>& otherWay);
// The way a KVM is added: at which address, and what was found there (the line whose answer and
// certificate count from here on).
struct Way {
    QUrl address;
    KvmProbe::Found found;
};
// Which way a KVM is added, as the web decides it (detect_kind in ../app/qkvm/server.py), HTTPS first:
// 1. A JetKVM that answers over HTTPS, at the address or beside it: there, also where "http://" was
//    typed.
// 2. Else a JetKVM that answers only the plain way, beside an HTTPS address:
//    * something answered at the address itself: that is what was meant, and it is taken as what it
//      is; the JetKVM beside it hears no password;
//    * "https://" was typed: "plain_only", in the web's sentence, which says to type "http://...";
//    * nothing was typed, no port was typed, and something listens on the address's port (it did not
//      answer in time, or did not refuse): "plain_only", in the web's second sentence;
//    * otherwise (nothing typed; a port typed, or nothing listens there): the plain way.
//    And a JetKVM at an address typed "http://": the plain way.
// 3. Else what answers at the address, as it was typed.
// An HTTPS address that was not asked (TLS is not Windows' own) is that, whatever answered beside it.
// Never the plain way where "https://" was typed.
// "plain_only" says what to type in the box that asks: the adding box's words are the web's ("To add");
// the change box's (`changing`: the address of the KVM that is changed) keep it over plain HTTP, where
// the plain address is the one it has, or move it there so.
base::Result<Way> wayOf(const Ways& ways, const std::optional<QUrl>& changing = std::nullopt);
// What a KVM at `address` is known by from now on, by what it has just shown a probe (SPEC.md
// section 8). Over plain HTTP that is nothing. Over HTTPS it is the certificate it showed; unless
// Windows trusts that one for the address as it is, and then it is known by none, and by Windows'
// word each time.
void knowByWhatItShowed(OwnKvm& kvm, const QUrl& address, const KvmProbe::Found& found);
// Why a KVM may not be on the wall beside these; nothing: it may. One of them is at its place
// (LocalWall::atOnePlace): "duplicate". Or one of them is known by the certificate it is known
// by, and so is this KVM under another address: "same_device", in the server's sentence, which
// names the one that is there. (A KVM that is being changed is not one of `there`: whoever asks
// for it leaves it out.) `shown`: what the line to it showed (a certificate's SHA-256; empty:
// nothing, or nothing asked yet): one of `there` that is known by that certificate is this KVM
// too, also where Windows trusts that certificate at its address and this KVM would be known
// there by Windows' word (from the third reading of the twins).
std::optional<base::Error> inTheWay(const QList<OwnKvm>& there, const OwnKvm& kvm, const QByteArray& shown = {});

} // namespace kvm

// Puts one KVM on this PC's own wall, as the server does it when a device is added (create_device
// in ../app/qkvm/server.py). Step by step, and each step can end it, with the server's own code
// and sentence where the server has one:
// 1. What was typed beside the address, by the rules above; and a password is no longer than the
//    server takes one ("invalid").
// 2. The address (kvm::addressFrom, "invalid"). One that is on the wall already is asked nothing:
//    "duplicate".
// 3. What answers there (KvmProbe), asked with nothing in the question that says who asks; and,
//    at the same time, the way beside it where a JetKVM may answer (kvm::bothWaysOf: the plain
//    way at the same host and port where nothing was typed before the address, the other way
//    where a scheme and no port of its own were typed), with the JetKVM's question alone. Which
//    of the two it is added at is the web's rule (kvm::wayOf): a JetKVM over HTTPS first, also
//    where "http://" was typed; the plain way only where nothing else answers over HTTPS, and
//    never where "https://" was typed ("plain_only"). Then what answers at that one: nothing,
//    "unreachable"; something that is no KVM this program speaks to, "not_a_kvm". Only a device
//    of a family this program speaks to by itself goes on: one that runs kvmd, a JetKVM, or a
//    server's management controller (M7). It is kept as that family (kvm::familyOf), at that
//    address. A controller's service root is waited for as its driver waits for it (Pace::probe).
// 4. What it is known by from now on (SPEC.md section 8). Over plain HTTP that is nothing. Over
//    HTTPS it is the certificate it showed the probe; unless Windows trusts that one for the
//    address as it is, and then it is known by none, and by Windows' word each time. A KVM of the
//    wall that is known by the same certificate is this KVM under another address: "same_device",
//    in the server's sentence, which names the one that is there.
// 5. One sign-in, by a driver of its family that is made for it and ended after it (OneSignIn: a
//    kvmd's KvmdDevice::identify, a JetKVM's JetKvmDevice::identify). What that driver says passes
//    through as it is ("auth_failed", "unreachable", "cert_mismatch", "no_sign_in" for a KVM that is
//    there and did not let it in, with what went wrong in its sentence, a JetKVM's "not_set_up", and
//    the rest). With no password, a kvmd's says so ("no_password") before any line is made; a
//    JetKVM's asks as one that needs none, as a JetKVM is out of its box, and says "no_password"
//    only where the JetKVM then refuses. A JetKVM over plain HTTP has its HTTPS looked at on that
//    sign-in first, as the wall's own driver has (LocalWall::twinCheckFor): where it shows the
//    certificate a KVM of the wall is known by, whatever it answers, nothing goes ("same_device").
// 6. Its name where none was typed (kvm::nameWhereNoneWasTyped). A JetKVM calls itself nothing, and
//    is named by where it is.
// 7. The wall (LocalWall::add), whose refusals pass through as they are ("not_saved", "too_many").
//    What was looked at before the KVM was spoken to is looked at once more first: the wall may
//    have changed meanwhile.
//
// The password (SPEC.md section 8) goes to the driver of step 5 and to the wall. Over HTTPS that
// driver sends it to what shows the certificate of step 4 (or, where the KVM is known by none, one
// that Windows trusts) and to nothing else; over plain HTTP it goes in the clear, which `Added`
// says. It is in no sentence and in no log, and nothing of it is kept here once the adding is
// over. Before the probe has said that a KVM of a family it speaks to answers, nothing but the
// probe's three questions is sent to the address at all, and nothing but the JetKVM's question to
// the way beside it.
//
// Otherwise than the server in these:
// * There is no "add anyway" and no "keep both". What could not be checked is not added, and one
//   KVM is not put on the wall twice.
// * Only what answered as a family this program speaks to is signed in to: kvmd in its own shape,
//   a JetKVM in its. The server takes everything that is neither a JetKVM nor a controller for a
//   kvmd, silence too, and finds out with the password.
// * That it is the same KVM as one of the wall is said before the password goes out, since the
//   probe has seen its certificate. The server sees the certificate when it signs in.
// * The KVM is signed in to twice: here, and once more by the wall's own driver when that first
//   looks at it. The server hands the driver that signed in on to its wall.
//
// On the interface thread. add() comes back at once; the result is handed to `done` later, on the
// same thread, exactly once, unless this is gone by then: an adding that is ended in the middle
// adds nothing and tells nobody. Whoever is handed a result may begin the next adding there and
// then, and does not end this there and then, but a turn of the loop later (deleteLater). Whoever
// hears the wall say that it has changed may end it there and then: the KVM is on the wall by
// then, and the wall has said so.
//
// The wall outlives it.
class KvmAdding : public QObject {
    Q_OBJECT

public:
    // What was typed into the box.
    struct Typed {
        QString address;
        QString user; // empty: "admin"
        QString password;
        QString name;  // empty: what the KVM calls itself, or where it is
        QString group; // empty: none
        // Where it says by its number that it is a KVM of the wall, it is to have a card of its own beside
        // that one all the same (the web's keep_both; LocalWall::saysItIsTheSame). Not by a certificate, and
        // not of a server's controller: a server has one card on this PC's wall (the owner's word, M7 Q2).
        bool keepBoth = false;
    };

    // The KVM that is on the wall now.
    struct Added {
        QString id;   // the wall's own name for it
        QString name; // as typed, or the one it got
        // What it says it is; empty where it says nothing (one maker's does not).
        QString model;
        QString version;
        // It is spoken to in the clear: what is typed and seen there, and its password, are not
        // encrypted on the way. Whoever shows that it was added says so.
        bool overPlainHttp = false;
        // The SHA-256 of the certificate it is known by from now on: the one it showed when it
        // was asked what it is. Whoever shows that it was added shows this, to be compared with
        // what the KVM says of itself. Empty where it is known by none (plain HTTP, or a
        // certificate that Windows trusts for its name).
        QByteArray certificate;
    };

    // The times the probe and the driver that signs in keep, of each family. A test makes them short.
    struct Pace {
        // The program's own, and a controller's service root waited for as its driver waits for it before a
        // sign-in (KvmProbe::Pace::rootQuietMs): an older board takes nine to twenty seconds to show it.
        KvmProbe::Pace probe = [] {
            KvmProbe::Pace program;
            program.rootQuietMs = RedfishDevice::Pace{}.rootUnsignedMs;
            return program;
        }();
        KvmdDevice::Pace driver;
        // A JetKVM's; none: alike the kvmd's (DriverPaces::alike).
        std::optional<JetKvmDevice::Pace> jetkvm;
        // A server's controller's; none: alike the kvmd's (DriverPaces::alike).
        std::optional<RedfishDevice::Pace> redfish;
        // Where a JetKVM's other way is (kvm::otherWayOf). A test has two stand-ins at two ports be
        // one JetKVM's two ways.
        std::function<std::optional<QUrl>(const QUrl&)> otherWay = kvm::otherWayOf;
        // The drivers' of every family, as a wall and a sign-in are handed them.
        DriverPaces drivers() const;
    };

    explicit KvmAdding(LocalWall& wall, QObject* parent = nullptr);
    KvmAdding(LocalWall& wall, Pace pace, QObject* parent = nullptr);
    ~KvmAdding() override;

    // Adds the KVM that was typed, or says why not. One at a time: while an adding is under way a
    // second is refused ("busy"), and the first goes on as if nothing had been asked.
    void add(Typed typed, std::function<void(base::Result<Added>)> done);
    // From add() until its result has been handed over.
    bool isUnderWay() const { return m_underWay; }
    // Gives up the adding that is under way: a KVM that answers a byte at a time is not waited
    // for to the end. Nothing is on the wall and nothing is kept; whoever asked is told so
    // ("given_up"), later and once, as of any other end. What the KVM was asked up to then it was
    // asked (it may have been signed in to); nothing more goes to it. Nothing where no adding is
    // under way, or one whose result is on its way already, or one that the wall is being told of
    // this moment: whoever hears the wall say that it has changed and gives up then is told that
    // the KVM was added, which it was.
    void giveUp();

private:
    // One of the two ways has answered; once both have, the way is chosen (kvm::wayOf).
    void askedOneWay();
    void probed(const KvmProbe::Found& found);
    void identified(const base::Result<OneSignIn::Said>& who);
    void letTheDriverGo();
    void finish(base::Result<Added> result);
    void later(std::function<void()> what);

    LocalWall& m_wall;
    Pace m_pace;
    bool m_underWay = false;
    // The adding is not given up any more: it has ended and its result is on its way to whoever
    // asked, or the wall is being told of the KVM this moment.
    bool m_over = false;
    // One higher with every end of an adding: what one that is over was still to be told (an
    // answer of the probe's, of the driver's) reaches nothing.
    quint64 m_turn = 0;
    std::function<void(base::Result<Added>)> m_done;
    // What is being added, as far as it is known, where it is, and its password: only while an
    // adding is under way.
    OwnKvm m_kvm;
    QUrl m_address;
    // (Written over when it goes, or is given another: from the merged review, 3.)
    SecretText m_password;
    // It is to be kept beside a twin it says by its number that it is (Typed::keepBoth).
    bool m_keepBoth = false;
    // What was asked both ways and what came of it, and how many of the two are still out.
    kvm::Ways m_ways;
    int m_waysOut = 0;
    // What the line to the way chosen showed (kvm::inTheWay's `shown`), for the look once more before it is added.
    QByteArray m_shown;
    // The driver that signs in once, of the KVM's family.
    std::unique_ptr<OneSignIn> m_device;
    KvmProbe m_probe;
};

} // namespace qkvm::core
