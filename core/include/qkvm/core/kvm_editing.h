#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/own_sign_in.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wiped.h>

// A KVM of this PC's own wall is changed here (SPEC.md section 4, M3a: the box that adds one
// "changes a KVM's address, name, group and password"), and the certificate it is known by is
// looked at and said yes to anew (section 8).
namespace qkvm::core {

namespace kvm {

// A certificate's SHA-256 as the page shows it, to be compared with the one a KVM's own page
// shows: capitals, a colon between every two. (As a server's is written: ServerClient::written.)
// Nothing where there is none.
QString fingerprint(const QByteArray& sha256);

} // namespace kvm

// Changes one KVM of this PC's own wall, as the server does it when a device is edited (the
// editing of a device in ../app/qkvm/server.py), by the server's rules and in its sentences. Step
// by step, and each step can end it:
// 1. The KVM, by the wall's name for it. One that is not on the wall: "not_found". And where
//    whoever asks says what they had before them when they typed (Typed::seen: the KVM as a box
//    showed it when it was opened, and how often its secret had been written by then), a KVM
//    that is not that any more: "conflict", before anything else is looked at. What was typed
//    stood over something else: saved, it would put back what another window changed
//    meanwhile, and nobody would be told.
// 2. What was typed, by the rules of adding (kvm_adding.h; "invalid"). No name typed keeps the
//    name. No group typed clears the group. No user name typed is "admin". No password typed
//    keeps the one that is kept.
// 3. The address (kvm::addressFrom, "invalid"). Another KVM of the wall at it: "duplicate", and
//    nothing is asked there. Plain HTTP at the place where the KVM is on the wall over HTTPS:
//    "not_in_the_clear" (LocalWall::intoTheClear), with its password typed or without, and
//    nothing is asked there either. The other direction goes on as a new address does.
// 4. What is asked of the KVM before anything is saved, by what was changed:
//    * The name or the group, and nothing else: nothing.
//    * A JetKVM spoken to over plain HTTP that is to be signed in to anew (as the web has it: with
//      whatever it is signed in to with): its password typed at the address it has; or the one kept,
//      under another user name, or moved with nothing typed to its own HTTPS. Its other way (HTTPS
//      at the same host, Pace::otherWay, as LocalWall::ontoHttps has it) is asked the JetKVM's
//      question alone first. Where a JetKVM answers there, it is signed in to there, known by what
//      it showed on that first contact, and saved there (Changed::ontoHttps). That answer is the
//      asking of what it is, of one that was not asked yet too where its password was typed; the
//      kept one goes to no KVM that was not asked yet ("not_asked_yet", LocalWall::signerIn).
//      Otherwise on as below.
//    * A JetKVM moved with a password typed, or with none where it has none kept (one out of its
//      box: it is signed in to as one that needs none, and saved with none): asked both ways, as
//      when one is added (kvm::wayOf, "plain_only" in the change box's words), and the address
//      chosen is held to all that a typed address is held to below (another KVM at its place,
//      HTTPS to the plain way at its place, plain HTTP for HTTPS only with the yes to that address,
//      and what it would be known by there). Chosen at its own HTTPS, where a JetKVM answered, it
//      is known there by what it showed on that first contact, as above; any other address chosen
//      is a new one.
//    * A KVM that was not asked yet what it is (LocalWall::wasAskedWhatItIs: one that was taken
//      from a file, and every KVM of an older settings file, until the wall has asked what
//      answers at its address; one whose new certificate somebody said yes to). Nobody signs in
//      to it before it was asked, so:
//      - Anything that would sign in to it at the address it has (another user name; a
//        password that was typed for that address): "not_asked_yet", and nothing goes to it.
//      - Another address, with the password left as it is kept: moved as the wall moves it
//        (LocalWall::change). Only where the kept password may go along
//        (LocalWall::keptPasswordStaysBehind: both addresses HTTPS, and the KVM known by a
//        certificate; "password_needed" otherwise). What answers at the new address is asked
//        what it is, on lines that are held to that certificate (KvmProbe::askAsKnown), and
//        nothing else goes there: what shows another certificate hears nothing
//        ("password_needed", in the server's sentence for a device that could not be checked
//        at its new address), and what is no KVM, or nothing, is said in the wall's words for
//        that (LocalWall::notTheKvm) and not saved. The KVM itself is saved there with nothing
//        signed in to (Changed::signedIn is false), its password saved anew for that address;
//        and where none is kept for it, it is not moved without one ("no_password").
//      - Another address, with a password typed for it: asked there as below.
//      Its name and its group are changed all the same, and its certificate is looked at and
//      said yes to as any other's (below). Whichever it was, it stays one that was not asked:
//      the wall asks it as it is from then on, and its password goes to it only after that.
//    * The user name, or a password was typed, at the address it has: one sign-in with the new
//      ones, by a driver of its family that is made for it and ended after it (OneSignIn), to the
//      KVM as it is known (by its certificate, or by Windows' word). What that driver says passes
//      through as it is ("auth_failed", "unreachable", "cert_mismatch" and the rest). Every
//      sign-in of the box, with the password typed or the one kept, at the address it has or
//      another, holds a JetKVM over plain HTTP to what the wall's own driver is held to: its HTTPS
//      looked at on that sign-in, and nothing sent where that shows the certificate another KVM of
//      the wall is known by ("same_device", LocalWall::twinCheckFor).
//    * The address, and a password was typed: it is asked what answers there, as when a KVM
//      is added (KvmAdding, step 3), with nothing in the question that says who asks: only a KVM
//      of its family goes on, and one of the other family that this program speaks to is another
//      KVM ("other_family", kvm::ofAnotherFamily). And what the KVM would be known by there is
//      found anew (kvm::knowByWhatItShowed; what shows the very certificate the KVM is known by
//      stays known by that one, whoever else vouches for it there). What shows the certificate
//      another KVM of the wall is known by is that KVM: "same_device", in the sentence an adding
//      has for it (kvm::inTheWay). Then, before the password goes anywhere:
//      - Over HTTPS it is signed in to at once only where it will be known as it is known
//        now. It shows the certificate the KVM is known by: the same KVM, reached another
//        way. Or the KVM is known by Windows' word where it is, and Windows trusts what is
//        shown there for the name that was typed: Windows vouches there as it does here.
//      - Known otherwise there: "certificate_differs", and nothing but the three questions
//        has gone there. That is another certificate than the one it is known by; a
//        certificate, where it is known by none (it is spoken to in the clear, or on Windows'
//        word); and Windows' word, where it is known by a certificate or is spoken to in the
//        clear. Whoever asked is handed what it is known by now and what is shown there
//        (Outcome::remembered, presented, onWindowsWord), and shows the two side by side, as
//        a look at a certificate does (KvmCertificateReview, below): somebody compares them,
//        and says yes or does not.
//      - It is the very thing somebody was shown for this change and said yes to
//        (Typed::saidYesTo, saidYesOnWindowsWord): signed in to once, and saved as known that
//        way. By a certificate: on lines that are known by that certificate and by no other.
//        On Windows' word: on lines that Windows trusts, and the KVM is known by no
//        certificate from then on, not by the one it had either. It was read once more for
//        this, as a yes is everywhere: what is shown there by now, and is not what was said
//        yes to, is refused as above, with what is shown now. And what shows another only
//        when the sign-in goes out hears nothing: the driver's lines are held to what was
//        said yes to ("cert_mismatch", in the sentence a yes to a certificate has for it; or
//        the driver's "cert_invalid", where it is Windows that does not trust it).
//      - What answers there over HTTPS in kvmd's own shape and shows no certificate at all (no
//        line that Qt makes does): there is nothing to put before anybody. Where a yes would
//        be needed, that is "unreachable", and nothing more goes there. A KVM that is known
//        by Windows' word where it is would be known by that word there too, so it goes on
//        to the sign-in as above, and its driver's lines refuse what Windows does not trust
//        ("cert_invalid") before a byte goes out on them.
//      The new address is plain HTTP, and the KVM is spoken to over HTTPS where it is (another
//      place than the one it is at: that is refused in 3). Nothing can be shown of a place
//      that speaks in the clear; what can be said is what is given up there. So this is
//      held to the same rule, before the three questions too:
//      - Without a yes: "plain_http_there", and nothing at all has gone to that address.
//        Whoever asked is handed what the KVM is known by now and that address
//        (Outcome::remembered, presentedAt), and shows the first beside what is given up:
//        nothing shows who answers there, and its password, and what is seen and typed, can
//        be read on the way. Somebody says yes to that or does not.
//      - With the yes to that very address (Typed::saidYesToPlainHttpAt), and to no other:
//        asked what answers there, as above, signed in to once, and saved as spoken to in
//        the clear and known by nothing. Changed::overPlainHttp says so for whoever shows
//        the save.
//      A KVM that is spoken to over plain HTTP already, at another plain-HTTP address, gives
//      nothing up: it is asked what answers there and signed in to, as before.
//      Adding a KVM anew is not held to this (kvm_adding.h): there is nothing it was known by
//      to put beside what it shows, and what it showed is shown once it is on the wall.
//    * The address, and no password was typed (of a KVM that was asked what it is; of one that
//      was not, above): the kept password goes to another address only
//      where the KVM can be known there (LocalWall::signerIn): both addresses HTTPS, and the KVM
//      known by a certificate. It keeps that certificate, and what shows another one at the new
//      address hears nothing, whatever was said yes to: where a kept password goes is the
//      wall's rule and nobody else's, and a yes of either kind is for a password that was typed.
//      Anywhere else: "password_needed", in the server's sentence. And where it is the
//      certificate shown there, or the kept password, that is not the KVM's: "password_needed"
//      too, in the server's sentence for a device that could not be checked at its new address,
//      with the driver's word in it. Typed, the password is held to the rules above.
// 5. The wall (LocalWall::change): what it is known by and its password are saved together or
//    not at all ("not_saved"), and its driver is replaced where what that is made of changed.
//    What was looked at when the change began (1) is looked at once more first: a KVM that
//    was changed meanwhile, in another window's box, is not saved over ("conflict"), and one
//    that was taken off is "not_found". Changed is also one whose password alone was saved
//    anew meanwhile (LocalWall::secretWrites): the list shows nothing of that, and what this
//    signed in with was never tried together with it. NOT changed, here and in (1), is one of
//    which the wall recorded something by itself meanwhile (that it was asked what it is:
//    LocalWall::isStillTheOne): a box that was open while the wall asked is not refused for it.
//    And the KVM it turned out to be at a new address may have come onto the wall meanwhile
//    ("same_device").
//    The wall keeps the rules for a password by itself, whoever asks it (local_wall.h): what it
//    refuses passes through as it is.
// Where a step refuses, nothing is changed: the KVM is on the wall as it was, with the sign-in
// it had.
//
// The password (SPEC.md section 8): one that was typed goes to the driver of step 4 and to the
// wall, and nothing of it is kept here once the change is over. The one that is kept is never
// handed to this at all: the wall makes the driver that signs in with it, and says where it may
// go. Neither is in a sentence or in a log.
//
// Otherwise than the server in these:
// * A change of the name or the group alone asks nothing of the KVM. The server signs in at
//   every edit. Here a KVM that is away (switched off, or on a network this PC is not on today)
//   can still be renamed and put into another group: what stands on its card is this PC's own,
//   and nothing a sign-in guards is changed by it. Where its password goes stays what it was.
// * There is no "save without checking" yet, which the server's page offers for a device that
//   is away. What could not be signed in to is not saved. So where the kept password could not
//   be tried at a new address because nothing answers there, the driver's own word is given, and
//   not the server's "type its password to save it there all the same": typing it would not.
// * A KVM is not moved onto another one of the wall. The server says "same_device" where a
//   device is added, and never where one is edited. Here two entries for one KVM are two
//   drivers, and "one thing at a time to a KVM" is kept by each for itself: a button could be
//   pressed through both at once.
// * A KVM is not changed from HTTPS to plain HTTP at the place it is at ("not_in_the_clear", a
//   code of this program's own). Whoever means that takes it off the wall and adds it anew:
//   there "http://" is typed for it, and whoever shows that it was added says what that means.
// * A KVM is known at another address as it is known where it is, until somebody has seen what
//   it would be known by there beside that and said yes ("certificate_differs", step 4). The
//   server forgets the certificate with the address, and knows the device by what it shows
//   there. So with plain HTTP at another address for a KVM that is spoken to over HTTPS: until
//   somebody has seen what is given up there and said yes, nothing goes to that address
//   ("plain_http_there", a code of this program's own).
// * There is one way a KVM is known and none to choose between (section 8), so none is typed.
// * At a new address only what answered as a KVM of its family is signed in to, as when adding.
// * Where it is signed in to first, it is signed in to twice, as when adding: here, and once
//   more by the wall's new driver when that first looks at it.
//
// On the interface thread. change() comes back at once; what came of it is handed to `done` later,
// on the same thread, exactly once, unless this is gone by then: a change that is ended in the
// middle changes nothing and tells nobody. (A change of the name or the group alone is on the
// wall before change() comes back; its result comes later all the same.) Whoever is handed a
// result may begin the next change there and then, and does not end this there and then, but a
// turn of the loop later (deleteLater). Whoever hears the wall say that it has changed may end it
// there and then: the change is on the wall by then, and the wall has said so.
//
// The wall outlives it.
class KvmEditing : public QObject {
    Q_OBJECT

public:
    // A KVM as somebody had it before them: as it was on the wall when their box was filled, and
    // how often the wall had written its secret by then (LocalWall::secretWrites: a password
    // that another window saved meanwhile leaves the KVM in the list as it stood).
    struct Seen {
        OwnKvm kvm;
        quint64 secretWrites = 0;
    };

    // What stands in the box when it is saved.
    struct Typed {
        QString name; // empty: the name it has
        QString address;
        QString user;     // empty: "admin"
        QString password; // empty: the one that is kept
        QString group;    // empty: none
        // What somebody was shown for this change, beside what the KVM is known by, and said
        // yes to: the certificate that was shown at the new address, by its SHA-256; and
        // whether the yes was to the KVM's being known by Windows' word there, which trusts
        // that certificate, and not by the certificate itself. Empty: no yes. (Step 4.)
        QByteArray saidYesTo;
        bool saidYesOnWindowsWord = false;
        // And the yes where the KVM is spoken to over HTTPS and the address that was typed is
        // plain HTTP: somebody was shown what is given up there, and said yes to that. The
        // address it was said for, as it was handed over (Outcome::presentedAt); it is a yes
        // for that address alone. Empty: no yes. (Step 4.)
        QString saidYesToPlainHttpAt;
        // What all of this was typed over, where whoever asks had the KVM before them for a
        // while (a box that was open): a KVM that is not that any more is not changed (step 1).
        // Nothing: the KVM as it is on the wall when the change begins.
        std::optional<Seen> seen;
        // As the Add box's (KvmAdding::Typed::keepBoth).
        bool keepBoth = false;
        // Its screen watches, in minutes (0: not watched); nothing: as they are.
        std::optional<int> noSignalMinutes;
        std::optional<int> unchangedMinutes;
    };

    // The KVM as it is on the wall now.
    struct Changed {
        QString id;
        QString name; // as its card has it: its name, or where it is
        // It was signed in to before this was saved. (Not for a name or a group alone.)
        bool signedIn = false;
        // It is spoken to in the clear: what is typed and seen there, and its password, are not
        // encrypted on the way. Whoever shows that it was saved says so.
        bool overPlainHttp = false;
        // The SHA-256 of the certificate it is known by now: the one it showed at a new address,
        // or the one it was known by and still is. Whoever shows that it was saved shows this, to
        // be compared with what the KVM says of itself. Empty where it is known by none (plain
        // HTTP, or a certificate that Windows trusts for its name).
        QByteArray certificate;
        // It was spoken to over plain HTTP, and answers over HTTPS now at the same host: a JetKVM
        // whose owner turned that on, which the change was saved over (LocalWall::ontoHttps). Whoever
        // shows that it was saved says so, with `certificate`.
        bool ontoHttps = false;
    };

    // What a change came to, as whoever asked is handed it.
    struct Outcome {
        // The KVM as it is on the wall now; or why it is as it was.
        base::Result<Changed> result;
        // With "certificate_differs", and with nothing else: the certificate the KVM is known
        // by (empty: by none) and the one that is shown at the new address right now, each by
        // its SHA-256; and that address as it is typed (its host, and its port where it has
        // one of its own). Whoever asked shows the two side by side, and says where the second
        // was shown. They are here and in no sentence: a sentence is read, and these are
        // compared.
        QByteArray remembered;
        QByteArray presented;
        QString presentedAt;
        // And whether Windows trusts what is shown there for the name that was typed: the KVM
        // would be known by Windows' word there, and by no certificate. Whoever asked says so
        // in the place of a second certificate to compare.
        bool onWindowsWord = false;
        // With "plain_http_there": `remembered` and `presentedAt` as above, and nothing was
        // presented: nothing has gone to that address.
    };

    // The times the probe and the driver that signs in keep: the ones adding keeps.
    using Pace = KvmAdding::Pace;

    // What stands against the address chosen for a JetKVM asked both ways (step 4), as against an
    // address that was typed: another KVM of the wall at its place ("duplicate"), HTTPS to the plain
    // way at the place it is at ("not_in_the_clear"), and plain HTTP where it is spoken to over HTTPS
    // without the yes to that very address (`saidYesToPlainHttpAt`; "plain_http_there", with what it
    // is known by and that address). Nothing: none stands. `before` is the KVM as it is on `wall`.
    static std::optional<Outcome> refusalAtTheWayChosen(const LocalWall& wall, const OwnKvm& before,
                                                        const OwnKvm& after, const QString& saidYesToPlainHttpAt);

    explicit KvmEditing(LocalWall& wall, QObject* parent = nullptr);
    KvmEditing(LocalWall& wall, Pace pace, QObject* parent = nullptr);
    ~KvmEditing() override;

    // Changes the KVM to what was typed, or says why not. One at a time: while a change is under
    // way a second is refused ("busy"), and the first goes on as if nothing had been asked.
    void change(const QString& id, Typed typed, std::function<void(Outcome)> done);
    // From change() until its result has been handed over.
    bool isUnderWay() const { return m_underWay; }
    // Gives up the change that is under way: a KVM that answers a byte at a time is not waited
    // for to the end. Nothing is changed; whoever asked is told so ("given_up"), later and once,
    // as of any other end. What the KVM was asked up to then it was asked (it may have been
    // signed in to); nothing more goes to it. Nothing where no change is under way, or one
    // whose result is on its way already, or one that the wall is being told of this moment:
    // whoever gives up then is told what came of it, and that may be that it was saved.
    void giveUp();
    // What the change that is under way has come to, where that is settled and whoever asked
    // has not been told yet: in the turn of the loop between the two, and while the wall says
    // aloud that it has changed. For whoever ends this there and then, which tells nobody:
    // what was saved is theirs to say from then on. Nothing at any other time.
    const std::optional<Outcome>& cameTo() const { return m_cameTo; }

signals:
    // The sign-in of the change under way waits for another's with the same password to that KVM to be
    // answered (another window's, the wall's own driver's): said as it begins to wait, for the box to say.
    void signInWaits();

private:
    // Hears what the sign-in of this change says while it is out (signInWaits).
    void heedItsSignIn();
    void probed(const KvmProbe::Found& found);
    // What an asking found that was held to what the KVM is known by: of a KVM that was not
    // asked yet, at the new address it is moved to with its kept password.
    void askedAsKnown(const KvmProbe::Found& found);
    // What is asked of the KVM, by what was changed (step 4). `moved`: by the address it is to have,
    // which, for a JetKVM asked both ways, is the address chosen (askedOneWay asks this anew with that).
    void goOn(bool moved);
    // A JetKVM moved with a password typed (or with none, where it has none: `m_typedNone`) is asked both
    // ways, as when one is added; once both have answered, the way is chosen (kvm::wayOf), and the
    // address chosen is held to every rule a typed one is held to (refusalAtTheWayChosen).
    void askBothWays();
    void askedOneWay();
    // What each HTTPS of both ways showed, judged by the wall (LocalWall::judgeItsHttps), whatever it answered: its own
    // (`own`, ownHttps()) as its own, any other as one elsewhere. Why nothing goes on; nothing where nothing stands.
    std::optional<base::Error> judged(const kvm::Ways& ways, const std::optional<QUrl>& own);
    // A JetKVM spoken to the plain way that is about to be signed in to: what answered at its other way,
    // over HTTPS at its own host (`at`), judged first (judgeItsHttps), whatever it answered, but where it is
    // the way chosen of both (judged in askedOneWay). A JetKVM there: signed in to there, with the password
    // typed, or with none where none was typed and none is kept (`m_typedNone`: as one that needs none), or
    // with the one kept (that one by the wall's own rule), and saved there, known by what it showed. Otherwise
    // on as where it is; or, where `at` is the way chosen of both (`chosen`, askedOneWay), asked as an
    // address it is moved to (probed).
    void askedOverHttps(const QUrl& at, const KvmProbe::Found& found, bool moved, bool chosen);
    // Pace::otherWay, where there is one; nothing where there is none.
    std::optional<QUrl> otherWayOf(const QUrl& address) const;
    // A JetKVM spoken to the plain way: its other way, where that is HTTPS at the same host (as
    // LocalWall::ontoHttps has it). Nothing for any other KVM.
    std::optional<QUrl> ownHttps() const;
    // What signs in to it at the address it has, or moves it with the password that is kept.
    void signInWhereItIs(bool moved);
    // The one sign-in with the password typed (or none, `m_typedNone`) at `where`: tried as for the first time, and
    // held to what the wall's own driver is held to before a password goes in the clear (LocalWall::twinCheckFor).
    base::Result<std::unique_ptr<OneSignIn>> signInWithWhatWasTyped(const OneSignIn::Where& where,
                                                                    const DriverPaces& paces) const;
    void identified(const base::Result<OneSignIn::Said>& who);
    void save(bool signedIn);
    void letTheDriverGo();
    void finish(base::Result<Changed> result);
    void finish(Outcome outcome);
    void later(std::function<void()> what);

    LocalWall& m_wall;
    Pace m_pace;
    bool m_underWay = false;
    // The change is not given up any more: it has ended and its result is on its way to whoever
    // asked, or the wall is being told of it this moment.
    bool m_over = false;
    // One higher with every end of a change: what one that is over was still to be told (an
    // answer of the probe's, of the driver's) reaches nothing.
    quint64 m_turn = 0;
    std::function<void(Outcome)> m_done;
    // What it came to, until whoever asked has been told (cameTo()).
    std::optional<Outcome> m_cameTo;
    // The KVM as it was when the change began and as it is to be, where it is to be, and the
    // password that was typed: only while a change is under way.
    OwnKvm m_before;
    OwnKvm m_after;
    QUrl m_address;
    // (Written over when it goes, or is given another: from the merged review, 3.)
    SecretText m_password;
    // What was said yes to for this change (Typed::saidYesTo); empty: nothing.
    QByteArray m_saidYesTo;
    bool m_saidYesOnWindowsWord = false;
    // It is being signed in to on that yes: known otherwise there than it is known now.
    bool m_heldToAYes = false;
    // The kept password is being tried at an address it was not saved for.
    bool m_keptElsewhere = false;
    // What the KVM is known by was found anew, at another address: by what it showed there.
    bool m_knownAnew = false;
    // The address as it was typed, and the yes to plain HTTP at an address that was typed
    // (Typed::saidYesToPlainHttpAt).
    QString m_typed;
    QString m_saidYesToPlainHttpAt;
    // Its other way, over HTTPS, was asked by this change already, and answered no JetKVM.
    bool m_overHttpsAsked = false;
    // A JetKVM with no password kept, moved with none typed: it is signed in to as one that needs none,
    // and saved with none.
    bool m_typedNone = false;
    // It is to be kept beside a twin it says by its number that it is (Typed::keepBoth).
    bool m_keepBoth = false;
    // What was asked both ways and what came of it, and how many of the two are still out.
    kvm::Ways m_ways;
    int m_waysOut = 0;
    // What answered over HTTPS at the other way of a JetKVM spoken to the plain way, where it is saved
    // there (LocalWall::change).
    std::optional<KvmProbe::Found> m_overHttps;
    // What the line to the address it is moved to showed (kvm::inTheWay's `shown`), for the look once more at its save.
    QByteArray m_shown;
    // How often the wall had written its secret when the change began (LocalWall::secretWrites).
    quint64 m_secretWrites = 0;
    // The driver that signs in once, of the KVM's family.
    std::unique_ptr<OneSignIn> m_device;
    KvmProbe m_probe;
};

// The certificate a KVM of this PC's own wall is known by, beside the one it shows right now, and
// the saying yes to the new one (SPEC.md section 8): what a card's "Review certificate" opens. A
// KVM that shows another certificate than the one it is known by hears nothing from its driver,
// which says "cert_mismatch"; here somebody looks at both, and the wall is told only what that
// person saw.
//
// What the KVM shows is read as it is read when a KVM is added: by a probe (KvmProbe), on lines of
// its own, which asks its three questions with nothing in them that says who asks, and tells
// which certificate was shown without trusting it. No password and no session travels on such a
// line, here or ever.
//
// Beside the wall, and not in it: the wall speaks to a KVM by its driver alone, on whose lines
// nothing passes that the KVM is not known by. What reads a certificate unjudged stays outside
// it, so that no line of the wall's is one that was let up unlooked-at.
//
// On the interface thread. A call comes back at once; what it asked for is handed to `done`
// later, on the same thread, exactly once, unless this is gone by then: then nothing is trusted
// and nobody is told. One at a time: while one is under way a second is refused ("busy").
// Whoever is handed something may ask the next there and then, and does not end this there and
// then, but a turn of the loop later (deleteLater). Whoever hears the wall say that it has
// changed may end it there and then: the certificate is the KVM's by then, and the wall has
// said so.
//
// The wall outlives it.
class KvmCertificateReview : public QObject {
    Q_OBJECT

public:
    // Two certificates, each by its SHA-256.
    struct Shown {
        QByteArray remembered; // what the KVM is known by; empty: by none (by Windows' word)
        QByteArray presented;  // what it shows right now
        // What it shows is the certificate another KVM of the wall is known by: that KVM answers
        // at this one's address (the two changed places, as addresses that are handed out do).
        // The wall's name for that KVM, and what its card calls it; both empty: no other is known
        // by it. Whoever shows the two says "this is <name>" then, before anybody compares: saying
        // yes is refused (trust(), "same_device").
        QString presentedIsOf;
        QString presentedIsCalled;
    };

    // What a yes came to, as whoever asked is handed it.
    struct Outcome {
        // The KVM is known by the certificate from now on; or why not.
        base::Result<void> result;
        // With "cert_mismatch", and with nothing else: what the KVM is known by and what it
        // shows now, in the place of what was said yes to. Whoever asked shows these two from
        // then on, and says that the second is another than a moment ago: nobody has seen it,
        // and no yes was given to it.
        Shown shownNow;
    };

    explicit KvmCertificateReview(LocalWall& wall, QObject* parent = nullptr);
    KvmCertificateReview(LocalWall& wall, KvmProbe::Pace pace, QObject* parent = nullptr);
    ~KvmCertificateReview() override;

    // What a KVM of the wall is known by, and what it shows right now; and where what it shows
    // is another KVM of the wall's, which one (Shown::presentedIsOf). Refused: a KVM that is
    // not on the wall ("not_found"), one that is reached over plain HTTP and has no certificate
    // ("no_certificate"), and one that shows none because nothing answers ("unreachable").
    // `seen`, here and for a yes: the KVM as whoever asks had it before them (a box that was
    // filled with it a while ago). One that is not that any more is neither read nor trusted,
    // and nothing is asked of it: "conflict", as a change says it (KvmEditing, step 1), and
    // again where it became another while it was read. What stands beside the two in such a
    // box is the KVM as it was, and a yes is said over that.
    void askCertificates(const QString& id, std::function<void(base::Result<Shown>)> done,
                         const std::optional<KvmEditing::Seen>& seen = std::nullopt);
    // Has the KVM known, from now on, by the certificate somebody was shown and said yes to. It
    // is read once more first: where the KVM does not show that very one now, nothing is saved
    // ("cert_mismatch", the server's sentence), since nobody has seen what it shows instead;
    // and what it shows instead is handed over with that, to be shown (Outcome::shownNow).
    // Nor where it is the certificate another KVM of the wall is known by ("same_device", in the
    // sentence that adding has for it, which names that KVM: kvm::inTheWay). What shows that one
    // is that KVM, at this one's address, and whoever compared the two found them the same for
    // that reason; this one's kept password does not go to it. The wall refuses it as well.
    // Otherwise the wall remembers it (LocalWall::knowBy) and the KVM's driver is replaced, so
    // that the next question to it goes out under the new certificate.
    void trust(const QString& id, const QByteArray& presentedAsShown, std::function<void(Outcome)> done,
               const std::optional<KvmEditing::Seen>& seen = std::nullopt);
    // From a call until its result has been handed over.
    bool isUnderWay() const { return m_underWay; }
    // Gives up the reading, or the yes, that is under way: a KVM that is slow to say what it is
    // is not waited for to the end. Nothing is trusted; whoever asked is told so ("given_up"),
    // later and once, as of any other end. Nothing where none is under way, or one whose result
    // is on its way already, or a yes that the wall is being told of this moment: whoever gives
    // up then is told what came of it, and that may be that the certificate is trusted.
    void giveUp();
    // What the yes that is under way has come to, where that is settled and whoever asked has
    // not been told yet: in the turn of the loop between the two, and while the wall says aloud
    // that it has changed. For whoever ends this there and then, which tells nobody: what was
    // trusted is theirs to say from then on. Nothing at any other time, and of a reading.
    const std::optional<Outcome>& cameTo() const { return m_cameTo; }

private:
    // Reads what the KVM shows right now, and hands it to `then` with the KVM as the wall has it
    // by then; or says to `refuse` why there is nothing to read.
    void read(const QString& id, const std::optional<KvmEditing::Seen>& seen,
              std::function<void(const base::Error&)> refuse,
              std::function<void(const OwnKvm&, const QByteArray&)> then);
    // The two as a reading hands them over: what a KVM is known by, what it shows, and whose
    // that is where it is another KVM of the wall's.
    Shown theTwo(const OwnKvm& kvm, const QByteArray& presented) const;
    void finish(std::function<void()> tell);
    // The same for a yes: what it came to is kept until whoever asked has been told.
    void finishYes(Outcome outcome, std::shared_ptr<std::function<void(Outcome)>> tell);
    void later(std::function<void()> what);

    LocalWall& m_wall;
    KvmProbe::Pace m_pace;
    bool m_underWay = false;
    // The call is not given up any more: its result is on its way to whoever asked, or the wall
    // is being told of the certificate this moment.
    bool m_over = false;
    // One higher with every end of a call: what its probe still hears reaches nothing.
    quint64 m_turn = 0;
    // What tells whoever asked why the call that is under way came to nothing.
    std::function<void(const base::Error&)> m_refuse;
    // What a yes came to, until whoever asked has been told (cameTo()).
    std::optional<Outcome> m_cameTo;
    // The probe of the call that is under way: made for it, and let go before the wall is told
    // anything. (One that was given up stays until the next call, or the end of this.)
    std::unique_ptr<KvmProbe> m_probe;
};

} // namespace qkvm::core
