#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/exchange.h>
#include <qkvm/core/jetkvm_kept.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/own_actions.h>
#include <qkvm/core/own_device.h>
#include <qkvm/core/own_sign_in.h>
#include <qkvm/core/screen_watch.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_source.h>

namespace qkvm::core {

// This PC's own wall: the KVMs that were added in the program, which it speaks to itself, with no
// server anywhere (SPEC.md section 4, M3a). It answers what a wall asks of the place its machines
// come from (wall_source.h), in a QKVM server's types and words.
//
// What each KVM is known by is in the settings; its password is in the secret store and nowhere
// else, by the KVM's id. Each KVM is spoken to by a driver of its own, and fails by itself: one
// that is away is "offline" on its card, and the wall of the others is as ever.
//
// A password is kept together with what it was saved for (SPEC.md section 8: it goes to the KVM
// it was given for and to nothing else). One secret per KVM, of four lines, each but the last
// ended by a line feed:
//     QKVM-KVM-2
//     the KVM's address, as the settings have it
//     what it is known by: "none", or "sha256:" and the SHA-256 of its certificate in hex
//     the password
// The first line says what this is, and which form of it. The password is the last, and is all
// that follows the third line feed, whatever is in it. A line's end can be in one: a password is
// taken as it was given, and bounded only in its length (kvm::passwordFrom). None can be in the
// two lines before it: an address is letters, digits and ".-_:/[]" (kvm::addressFrom), and
// anything else that stood in one is written there as "%" and two hex digits; and hex is hex.
// The password's bytes are its signs in UTF-8, and are read back sign for sign: one that begins
// with U+FEFF begins with it when it is read, too (Qt's usual reading of bytes would drop it).
//
// "Known by none" and "known by a certificate" are two lines that nothing reads as each other.
// (The form before this one, QKVM-KVM-1, wrote the hex alone, and so the same empty line for
// "none" and for a certificate that is empty. Nothing of that form is read: the program was not
// handed out with it.) And a KVM of which the list says that it is known by a certificate, and
// names none that is one (not 32 bytes: a list that was written by hand, or damaged), has no
// such line at all. Nothing is saved for it, nothing that is kept is its password, and its
// driver is given none, whatever that driver would do with one: "no_password" on its card,
// until somebody has looked at what it shows and said yes to that (kvm_editing.h).
//
// A secret is used only for the KVM it was saved for, as the list has that KVM now. One of any
// other form (the password alone; nothing at all), and one that was saved for another address or
// another certificate, is as none, never mind how nearly it fits: that KVM has no password
// ("no_password", in its driver's sentence), its card asks for it, and nothing is made of what
// is there. So where the list and the store do not agree (the list could not be written and the
// password before could not be put back, or the program ended between the two writings), what is
// in the store goes to nobody. What Windows cannot read back is as what is not there (secrets.h):
// the KVM has no password then either, and one that is given for it takes its place.
//
// A password is read from the store only to be used: for a KVM's driver (at a start, of one that
// was asked what it is; once an asking found one; where a change made it anew), for a sign-in of
// somebody's change (signerIn), for a change that takes it along or may have to put it back, and
// for a sealed file of the wall (asAFile). Never to say whether one is kept: for that the wall
// keeps a mark beside each KVM in the settings, which is no secret (marksAPasswordFor). What the
// store handed over, and what was made of it, is written over when its use is over.
//
// No password goes to an address before what answers there was asked what it is. A KVM that
// was taken from a file (takeFromAFile; wall_exchange.h is who asks) came with an address, a
// password and a word on what it is, and nobody has looked at what answers there. So it is on
// the wall as "not asked yet" (OwnKvm::askedWhatItIs), and until it was asked:
// * Its driver is given no password, and is asked nothing: no look, no picture, nothing done,
//   nothing live (`notAskedYet`, "not_asked_yet"). Its password stays in the store. That the wall
//   asks its driver nothing is all that holds a JetKVM's driver back: one with no password asks a
//   JetKVM as one that needs none, as a JetKVM is out of its box. (A kvmd's driver with no password
//   sends nothing at all.) Whatever reaches a KVM's driver asks first whether it was asked. (A picture
//   and a live view are things a wall asks for again by itself: while such a KVM is asked for
//   the first time they are refused as of a driver that was only just made, "no_stream",
//   which says "ask again soon", and as "not_asked_yet" once an asking did not find the KVM.)
// * When the wall is asked how its KVMs are (askStatus), it asks what answers at that address,
//   as the box that adds a KVM does (KvmProbe): three questions with nothing in them that says
//   who asks. On lines that are held to what the KVM is known by (KvmProbe::askAsKnown): one
//   that shows another certificate than the one from the file, or one that Windows does not
//   trust where the KVM is known by Windows' word, hears nothing at all, not the questions
//   either. That asking is given what the KVM's driver is given, step for step, and never less
//   (KvmProbe::Pace::beforeASignInWith): a KVM that the driver would reach is one the asking
//   reaches. And it has the wall's own pace: after one that did not find the KVM, the next is
//   a moment later, and later each time, as a driver leaves a KVM alone that is away.
// * Only what answered as a KVM of its family is the KVM (kvmd in its own shape, a JetKVM in its).
//   Then that is noted, a driver with its password takes the place of the one without, and looks
//   at the KVM at once. What answered otherwise, or not at all, is said on its card (the code a
//   box that adds a KVM has for it, and a sentence that says no password was sent), and it is
//   asked again later: a KVM of the other family too, which is another KVM than this one. A
//   gateway that answers in the KVM's place, and says that what is behind it does not answer, is
//   a KVM that is away, and not something else (notTheKvm).
// * While it is asked for the first time, its card says what the card of any KVM says that has
//   not answered yet: "connecting", and nothing else. Nothing is wrong with it.
//
// Who is "not asked yet", then:
// * Every KVM that was taken from a file.
// * Every KVM of a settings file that does not say of it, in so many words, that it was asked:
//   so every KVM of a wall that was made before the program kept this, once, at its next start.
//   (That is a wall that worked the day before, and is why the asking has the driver's times.)
// * A KVM that is spoken to over plain HTTP, once in EVERY run of the program, before its
//   first sign-in: that it was asked is not kept for such a KVM from one run to the next
//   (OwnKvm::askedWhatItIs is never true of it). Over plain HTTP nothing shows who answers, and
//   its password is posted in the clear at every sign-in; that a KVM answered at that address
//   once says little of what answers there in another year. What this is and is not: it keeps
//   a password from a machine that is no KVM at all (the printer, or the router's page, that
//   has the address now). It keeps it from nobody who answers AS one: the three questions are
//   answered by whoever cares to, and whoever sits on the line reads the password anyway.
//   Against those there is HTTPS and a certificate, and nothing else.
// * A KVM whose certificate somebody looked at and said yes to (knowBy), where that is another
//   than the one it was known by: the look at a certificate never asks what kind of thing
//   shows it, so it is "not asked yet" again from that yes on, and is asked under the new
//   certificate before its kept password goes to what shows it.
// A KVM that was added in the program was asked by its box (kvm_adding.h), and one that was
// changed there was asked where that was needed (kvm_editing.h): the wall does not ask those
// again in that run. Whether a KVM was asked is the wall's own to say: change() leaves it as it
// is, whatever it is handed, and knowBy() only ever takes it away.
//
// The askings go out a few at a time, and not all at once: each has a network manager of its
// own, which is a thread of Qt's and up to three lines, and a wall may hold two thousand KVMs.
// There are a few places (`Own::askedAtOnce`), and an asking holds one for a short while only
// (`Own::placeHeldMs`). A KVM that is well has answered by then. One that has not (it is off,
// or slow to let a line in) goes on being asked for as long as an asking may take, without its
// place: the next KVM is asked in it. So the KVMs that are off and stand first in the wall's
// order do not hold every place for seconds while the ones that are well wait behind them,
// which they did at a start: nobody was asked yet then, and nothing says beforehand which of
// them is off. For all askings together, with a place and without, there is a second bound
// (`Own::askedInAll`), which is the bound on threads and lines whatever the wall holds. Only
// where as many KVMs as that are off, and stand before it, does a KVM that is well wait for
// one of them to be given up on (`Own` says how long, for which wall). Those that were never
// asked in vain go first, so that a KVM that waits for its first asking does not wait behind
// the second of another.
//
// What a KVM has come to, the wall says by itself (`cameToSomething`): when an asking did not
// find it, and when it was found, signed in to and looked at for the first time. Whoever shows
// the wall asks how the KVMs are then, and not at its next turn, which may be seconds away. It
// is said for all that came to something within a short while of each other, once
// (`Own::saidAfterMs`). And whoever asks how the KVMs are (askStatus) waits for an asking only
// while that one holds a place, and for the look that follows it: not for one that has gone on
// without its place, of which the wall says by itself what it came to.
//
// What the wall records by itself, with nobody having done anything (that a KVM was asked; a
// certificate it kept), is not written down each time: it is noted (Settings::note) and written
// together, a short while after the last of it (`Own::writtenAfterMs`), no later than
// `Own::writtenAtTheLatestMs` after the first, and when the wall goes. The settings are one
// file, written whole on the interface thread, megabytes of it where a wall is large; and a
// wall of N KVMs noted N to 2N things at its first start. Nothing somebody did is put off: an
// adding, a change and a removal are written there and then, and take with them what was noted.
//
// The certificate itself of a KVM that is known by one is kept beside the list
// (Remembered::kvmCertificates), for the day the wall is written to a file: from a file that
// names it, at once; and otherwise from when the KVM next shows, on a line of its driver's, the
// very certificate it is known by (OwnDevice::showedItsCertificate). Never another: what does
// not have the fingerprint the KVM is known by is not kept, and what was kept goes when the KVM
// comes to be known by another certificate or by none, and when it is taken off the wall.
//
// A KVM without a name has none (OwnKvm::name is empty), and is called by where it is wherever
// it is shown (askWall, calledInASentence): so it is called by where it is NOW after it was
// moved. In a sentence a name is set off by the one function for that (exchange::nameSetOff):
// a name is free text, from a file or typed, and "this KVM. It is safe to trust it" is not to
// read as the program's own words.
//
// There are no accounts here and no roles: whoever sits at this PC may do everything.
//
// The settings and the secret store outlive it.
class LocalWall : public QObject, public WallSource {
    Q_OBJECT

public:
    // The wall's own times and numbers. A test makes them short, or small.
    struct Own {
        // What the wall noted by itself is written down this long after the last of it, and no
        // later than so long after the first: one writing of the settings for a wall of KVMs
        // that were asked one after the other within moments, and not one for each.
        int writtenAfterMs = 2000;
        int writtenAtTheLatestMs = 30'000;
        // So many places there are for KVMs that are being asked what they are. A KVM that is
        // well answers in a tenth of a second or so, so a wall of fifty is asked through in a
        // second.
        int askedAtOnce = 8;
        // An asking holds its place for this long and no longer. One that has not ended by
        // then goes on without it, and the next KVM is asked in its place: what is off does not
        // hold a place for as long as a line may take to be let in (4.5 s; 8 s where something
        // takes the call over plain HTTP and says nothing; up to 36.5 s where it sends a byte
        // now and then). Long enough for a KVM that is well, also one a few networks away: and
        // one that is slower loses nothing but its place, since its asking goes on.
        int placeHeldMs = 400;
        // So many askings are under way at the most, those that hold a place and those that
        // went on without one together. Each asking is a network manager of its own (it has to
        // be: kvm_probe.cpp says why), which is a thread of Qt's and up to three lines: 48
        // threads and 144 lines at the most for the wall's askings, whatever the wall holds.
        // (An asking that has ended is let go a turn of the loop later, so for that turn there
        // may be one more. Beside them, and not counted here: one asking for each box that is
        // open and asks by itself, and the driver of each KVM that is signed in to, which has a
        // network manager of its own.)
        //
        // What a KVM that is well waits for, then, at a start, with K KVMs that are off
        // standing before it in the wall's order: K / 8 short whiles, rounded down (ten that
        // are off: 0.4 s; forty: 2 s), while fewer than 48 are off before it. From 48 on, as
        // many askings are under way as there may be, and a KVM is asked when one of them has
        // ended: every 48 that are off cost as long as the asking of such a KVM takes (4.5 s
        // where nothing is let in: a hundred that are off, 9 s; a wall of two thousand of which
        // every one but the last is off, three minutes; and eight times that where every one
        // of them sends a byte now and then).
        int askedInAll = 48;
        // What the wall has to say by itself of its KVMs (cameToSomething) is said this long
        // after the first of them came to something, for all that have by then: fifty KVMs
        // that come online within a second are a few words, and not fifty.
        int saidAfterMs = 100;
        // Its screen watches' times (a test makes them short).
        ScreenWatch::Times screens;
    };

    LocalWall(Settings& settings, SecretStore& secrets, QObject* parent = nullptr);
    // With the times of the KVMs' drivers of a test's choosing, each family's (DriverPaces); or a
    // kvmd's, and a JetKVM's alike them (DriverPaces::alike).
    LocalWall(Settings& settings, SecretStore& secrets, DriverPaces paces, QObject* parent = nullptr);
    LocalWall(Settings& settings, SecretStore& secrets, KvmdDevice::Pace pace, QObject* parent = nullptr);
    // And with the wall's own.
    LocalWall(Settings& settings, SecretStore& secrets, DriverPaces paces, Own own, QObject* parent = nullptr);
    LocalWall(Settings& settings, SecretStore& secrets, KvmdDevice::Pace pace, Own own, QObject* parent = nullptr);
    // What was noted and not written yet is written down as the wall goes.
    ~LocalWall() override;

    void askWall(std::function<void(base::Result<Wall>)> done) override;
    // A KVM whose state is no longer fresh is looked at for this, and the answer waits for those
    // looks: for a moment and no longer (the kvmd's KvmdDevice::Pace::lookPatienceMs, for the KVMs of
    // both families, once for all of them, since they go out side by side). A KVM that is slower
    // than that, or away, is told as it was last known, and its look goes on by itself. Of a KVM
    // that is being asked what it is, the answer waits for the asking while that holds a place
    // (Own::placeHeldMs), and then for the first look of the driver the KVM has from there on; one
    // whose asking has gone on without its place is told as it was last known ("connecting" while
    // it is asked for the first time, and otherwise what the asking before came to), and the wall
    // says when it has come to something (cameToSomething).
    void askStatus(std::function<void(base::Result<QHash<QString, Status>>)> done) override;
    void askSnapshot(const QString& machine, int width, double mayBeOldSeconds,
                     std::function<void(base::Result<Snapshot>)> done) override;
    // One thing at a time goes to a KVM: a second is refused while the first is out ("busy"), in
    // the server's words. A power button pressed twice undoes the first press.
    //
    // A saved action of this wall (Action::run) is run as it is NOW, and only where that is the
    // state whoever runs it looked at: one that is not there any more, and one that was edited
    // since, are refused in the server's codes and sentences ("not_found", "changed":
    // OwnActions::asLookedAt). Then it is what its kind is, keys pressed or a text typed with
    // Enter or without, by the KVM's driver and under every rule above and below: one thing at a
    // time, what a KVM cannot do, whether an error leaves open that it was done. The text of one
    // that types is read from the secret store for that one run, once nothing else stands in
    // the way of the run, and is written over when the driver has been handed it
    // (OwnActions::forOneRun says how far that reaches); where there is none to read, the
    // action is refused ("no_text") in a sentence that says so and that nothing was typed. One
    // that is marked as dangerous runs as any other: asking first is for whoever shows it.
    void act(const QString& machine, const Action& what, std::function<void(base::Result<Done>)> done) override;
    // Of a KVM of the wall, what its family's driver says (KvmdDevice::mayHaveBeenDone,
    // JetKvmDevice::mayHaveBeenDone, RedfishDevice::mayHaveBeenDone); of one that is not on the wall, yes
    // where the driver of any family says yes.
    bool mayHaveBeenDone(const QString& machine, const base::Error& error) const override;
    // The saved actions of this wall as a server hands its own out, never the text one types: as
    // they are when the answer is handed over, a turn of the loop after the asking.
    void askSavedActions(std::function<void(base::Result<QList<SavedAction>>)> done) override;
    void askWakeTargets(const QString& machine, std::function<void(base::Result<QList<WakeTarget>>)> done) override;
    // None of these KVMs hands out a pass into its console: refused ("no_console"). A KVM's
    // console is entered at its door, where the KVM asks who comes.
    void askConsole(const QString& machine, std::function<void(base::Result<QUrl>)> done) override;
    void arrange(const QStringList& machines, std::function<void(base::Result<QStringList>)> done) override;
    // A KVM of the wall, live: its pictures the best way it has, and keyboard and mouse to it
    // (kvmd_live.h). All who have one KVM live share what is asked of it, each with a Live of
    // its own. Every KVM of this wall can be had live, and the wall says so (Wall::liveForm).
    // A KVM that is taken off the wall, or whose driver is replaced (its address or how it is
    // signed in to changed), ends its live: keyboard and mouse are let go there and then, and
    // whoever had it live is told `closed` a turn of the loop later, with what the wall tells
    // those it owes an answer ("not_found", or "no_stream": ask again).
    void openLive(const QString& machine, Live::Need need,
                  std::function<void(base::Result<std::unique_ptr<Live>>)> done) override;

    // The saved actions of the wall (own_actions.h): the list, and adding to it, changing and
    // removing, for whoever manages them. A change of the list is said as a change of the wall
    // (`changed`), so that it reaches whoever shows the wall the way a KVM that was added does.
    OwnActions& actions() { return m_actions; }
    const OwnActions& actions() const { return m_actions; }
    // Its KVMs' screen watches (screen_watch.h): every picture the wall hands out is shown to them, and each round
    // asks the wall which KVMs are watched, and how each is. Where their pictures are kept is the program's to say
    // (ScreenWatch::keepIn).
    ScreenWatch& screens() { return m_screens; }
    const ScreenWatch& screens() const { return m_screens; }

    // The KVMs of the wall, in its order.
    const QList<OwnKvm>& kvms() const;
    // Puts a KVM on the wall, last, as it is given: what it is known by was found out before
    // (whoever adds one asks what answers at its address first, and signs in once). An id is
    // made for it, whatever stood there, and handed back. Its password is kept, saved for its
    // address and what it is known by; when what was added cannot be written down, nothing is
    // kept and nothing is changed. Refused: an address that is on the wall already ("duplicate"),
    // a wall that is full, and a KVM that is to be known by a certificate that is none
    // ("cert_invalid").
    base::Result<QString> add(OwnKvm kvm, const QString& password);
    // Takes a KVM off the wall and forgets its password. The KVM itself is not touched. Not
    // while something is being done to it. Nothing its driver still answers reaches anybody:
    // whoever asked it for a picture, or for the computers it can wake, and has no answer yet is
    // told by the wall that the KVM is not there ("not_found"), once, a turn of the loop later.
    // (The id by value, here and below: whoever names a KVM by the wall's own text for it,
    // `kvms().first().id`, names something that is gone once the list is another.)
    base::Result<void> remove(QString id);
    // Changes a KVM of the wall to what is given, which was found out before (whoever changes
    // one signs in with what is new first: kvm_editing.h). The wall's name for it stays, whatever
    // stands in `kvm`, and so does its place on the wall. A password that is given is kept in the
    // place of the one before, saved for the KVM as it is from now on. None given: the one that
    // is kept stays its password, and is saved anew where the address is another. What it is
    // known by and its password are saved together or not at all: where either cannot be written
    // down, both are what they were, and nothing is changed ("not_saved"; and should the
    // password before not go back into the store either, the KVM has none: see above).
    //
    // Refused as well, whoever asks:
    // * A KVM that is not on the wall ("not_found"), and an address at which another KVM of the
    //   wall is ("duplicate").
    // * With no password given, anything the kept one may not go to: signerIn()'s rule, which is
    //   kept here too ("password_needed", the same sentence). That is another address, unless
    //   both are HTTPS and the KVM is known by a certificate, which it then keeps; and another
    //   certificate, or none, at any address (for that there is knowBy()).
    // * With no password given, another address where none is kept for the KVM as it is: there
    //   is none to take along, and the KVM is not moved without one ("no_password", its driver's
    //   sentence). So what Windows could not read back this once is not lost by a move.
    // * With a password or without: from HTTPS to plain HTTP at the place it is at
    //   (intoTheClear(), "not_in_the_clear").
    // * With a password given, a KVM that says it is known by a certificate that is none
    //   ("cert_invalid", its driver's sentence, which says what to do): no password is kept that
    //   could never be used.
    //
    // Its driver is made of where it is, who signs in, what it is known by and its password.
    // Where one of those changed, it is replaced by a new one, which signs in anew: not while
    // something is being done to the KVM ("busy"), and whoever waits for a picture of it is told
    // to ask again ("no_stream"). A new name or group leaves the driver as it is, and is taken
    // at any time.
    //
    // The one exception, for a JetKVM over plain HTTP whose owner turned HTTPS on (`overHttps`, what
    // answered at its other way): where `kvm` is exactly ontoHttps(), the kept password goes along to
    // that address and certificate though no password is given; and where none is kept for it (the
    // wall's mark: it runs without one), it goes there without one. One whose kept password Windows
    // could not read back this once is not taken along as one with none ("no_password"). Nothing else
    // of the above gives way.
    base::Result<void> change(QString id, OwnKvm kvm, const std::optional<QString>& password,
                              const std::optional<KvmProbe::Found>& overHttps = std::nullopt);
    // A JetKVM of the wall over plain HTTP, as it is once it is spoken to over HTTPS at its other way
    // (`otherWay`, kvm::otherWayOf) at the same host, where a JetKVM answered there (`found`, with
    // `isSetup`, on a line that was not refused): its address that one, known by what it showed there
    // (kvm::knowByWhatItShowed). Nothing for a KVM of another family, one over HTTPS already, one
    // whose other way is none or at another host, and an answer that is no JetKVM's.
    static std::optional<OwnKvm> ontoHttps(const OwnKvm& known, const KvmProbe::Found& found,
                                           const std::function<std::optional<QUrl>(const QUrl&)>& otherWay);
    // Has a KVM of the wall known by this certificate from now on, by its SHA-256: one that
    // somebody was shown and said yes to (kvm_editing.h). Saved and refused as change() saves and
    // refuses, and its driver replaced, so that the next question goes out under this certificate.
    // The kept password goes with it, saved anew for this certificate: the one way it goes to what
    // shows another certificate than it was saved for, and by that somebody's word alone. (Where
    // none is kept for the KVM as it is, the certificate is remembered all the same, and the KVM
    // has no password under it either.) A KVM that is reached over plain HTTP has none
    // ("no_certificate").
    //
    // That word was for the certificate, and says nothing of what kind of thing shows it: whoever
    // looked, looked at two fingerprints. So where the certificate is another than the KVM was
    // known by, the KVM is "not asked yet" from here on: its new driver has no password, and the
    // wall asks what answers there, on lines that are held to this certificate, before the
    // kept password goes to it. (A yes to the very certificate it is known by changes nothing.)
    //
    // Not by the certificate another KVM of the wall is known by ("same_device", sameDevice()):
    // what shows that one is that KVM, at this one's address. Two of the wall change places when
    // their addresses are handed out anew; said yes to, this one's kept password would go to the
    // other, its card would press the other's buttons, and two entries would hold one KVM, each
    // with its own "one thing at a time". Whoever shows a certificate asks that first
    // (kvm_editing.h); the wall is the last that is asked.
    base::Result<void> knowBy(QString id, const QByteArray& certificate);
    // A driver of its own, of the KVM's family (OneSignIn), that signs in to a KVM of the wall with
    // the password that is kept for it, under a user name or at an address that is not saved yet:
    // what whoever changes one of the two has done once, before anything is saved. (A JetKVM has no
    // user name: it is signed in to as it is anywhere.) The kept password is handed to nobody,
    // and goes where the wall's own driver sends it and nowhere else (SPEC.md section 8): to the
    // KVM's address, as it is known there; or to another address only where both are HTTPS and
    // the KVM is known by a certificate, and then to what shows that certificate and to nothing
    // else. For any other address this is refused ("password_needed", the server's sentence):
    // the password has to be typed for it. (And what is saved afterwards is held to the same
    // rule by change(): the two share it.) Where no password is kept for the KVM as it is, the
    // driver has none: a kvmd's says so ("no_password") before it makes a line, and a JetKVM's asks
    // as one that needs none, and says so only where the JetKVM then refuses.
    //
    // And the one exception, as change() has it: a JetKVM over plain HTTP, at its other way, HTTPS at
    // its own host, where a JetKVM answered there (`overHttps`): its kept password goes there, to what
    // shows what that answer showed (ontoHttps(), by the wall's own otherWay, JetKvmDevice::Pace).
    // (And the sign-in is held to twinCheckFor(id), as the wall's own driver's are.)
    base::Result<std::unique_ptr<OneSignIn>> signerIn(const QString& id, const QUrl& address, const QString& user,
                                                      const DriverPaces& paces,
                                                      const std::optional<KvmProbe::Found>& overHttps = std::nullopt);
    // The mark what answers at a KVM's address is held to by every driver of it, the wall's and every sign-in's
    // (OneSignIn::Where::machine): a JetKVM's, the number it is known by (OwnKvm::number, J5); a server's
    // controller's, the mark its server is kept by (OwnKvm::machine); a kvmd's, none.
    static QString markHeldTo(const OwnKvm& kvm);
    // What every sign-in of a JetKVM over plain HTTP holds its own HTTPS to before its password goes in the clear
    // (JetKvmDevice::lookAtItsHttpsBeforeEverySignIn): judgeItsHttps, of the KVM `id` (none: one that is being added).
    // The wall's own drivers are held to it, and so is every sign-in of a box (OneSignIn::of). It looks at the wall
    // as it is at the moment of the sign-in; a wall that is gone lets nothing go ("stopped").
    JetKvmDevice::TwinCheck twinCheckFor(const QString& id);
    // The one judgement of a look at an HTTPS before a JetKVM's password may go the plain way, whoever looked (the
    // Add and Change boxes at every look they take, every sign-in's own look, the wall's drivers), whatever it
    // answered and as whatever family (from the outside review, F1). `found`: what came of it; `from`: the plain
    // address of the KVM `id` whose own HTTPS that is (its other way, at its own host); empty: an HTTPS elsewhere
    // (an address a box asks it to be moved to, one that is being added). Refused in the wall's words:
    // * where it showed the certificate a KVM of the wall other than `id` is known by (any, for none): that one may be
    //   what answers the plain way too ("same_device", sameDevice()). Seen at its own HTTPS, at the address it has
    //   on the wall, the KVM is held back for that one from then on, as where the wall finds it so, and kept so for
    //   the next start: this is what holds it, on every road.
    // * where it is held back already, and this look shows nothing that lets go of that (no certificate, and
    //   something listens there: silence, an answer past its time, a line that showed nothing): in doubt it stays
    //   held, whatever HTTPS was looked at (heldBackFor()). The hold is let go of only on the signs SPEC.md section 8
    //   names, by the wall's own asking.
    std::optional<base::Error> judgeItsHttps(const QString& id, const QUrl& from, const KvmProbe::Found& found);
    // At the program's end (main.cpp, at aboutToQuit, before ControllerGoodbyes::finish): every driver let go
    // of, a turn of the loop later each (as letGo does; whoever ends the program sends the deferred deletions
    // before it waits), so that a server's controller is handed back its session while the loop still runs; and
    // none made anew from then on: nothing of the wall looks, asks or signs in any more (the KVMs stay in the
    // settings as they are). Whoever is owed an answer is told "stopped".
    void letGoOfEveryDriver();
    // How often a KVM's secret was written since the program started; 0 for one that is not on
    // the wall. Whoever reads a KVM, speaks to it for a while and saves then (kvm_editing.h) sees
    // by this that its password is another than it was: the list does not tell. A writing is
    // counted when it has happened: a change that was refused ("not_saved") with what was kept
    // still kept, or kept again, counts nothing.
    quint64 secretWrites(const QString& id) const;
    // Whether the wall's mark says that a password is kept for a KVM of the wall: what it wrote
    // beside the list (Remembered::kvmPasswordsKept), in the very writing in which it last kept or
    // forgot that KVM's password: add(), change() and knowBy() where they write one or take it
    // away, takeFromAFile(), remove(). Nothing of the store is read for it, and nothing of a
    // password is in it. A mark is not the store: Windows may have lost what it kept, or the
    // password before could not be put back (above); whoever lists the KVMs takes the word of a
    // look that found none over it (foundWithoutAPassword). False for one that is not on the wall,
    // and for each KVM of a wall that was written before the program kept the mark, until its
    // password is given anew.
    bool marksAPasswordFor(const QString& id) const;
    // The KVMs of the wall whose driver said, when it last looked, that it has no password for its
    // KVM: "no_password", what its card says then. Nothing is asked of a KVM for this, and nothing
    // is read from the store: it is what the last look found. One that was not asked yet what it
    // is (its driver looks at nothing), and one whose driver was only just made, is not among them.
    QSet<QString> foundWithoutAPassword() const;
    // Where a KVM is, as a card says it: its host, and its port where it has one of its own.
    static QString hostOf(const OwnKvm& kvm);
    // Whether two are at one place, whichever of the two ways each is spoken to: one host, and
    // one port or none (the port every such address has anyway is not written, so a KVM without
    // one is at one place over HTTPS and over plain HTTP). A KVM that is on the wall is not put
    // there a second time the other way: least of all in the clear where it was added over
    // HTTPS, which would send its password a second way, unencrypted (SPEC.md section 8).
    static bool atOnePlace(const OwnKvm& one, const OwnKvm& other);
    // The KVMs of the settings that this build does not read (Remembered::kvmsItDoesNotRead), each
    // with its id and its address and a name that says what it is: for whoever asks how full the
    // wall is and which places are taken (a wall's file, laid out against the wall).
    QList<OwnKvm> keptAside() const;
    // Whether such a KVM is at the place of this one: then no KVM of this build is put there, or the
    // build that reads it would hold two at one place.
    bool keptAt(const OwnKvm& kvm) const;
    // The same of two places that were read out of their addresses once: for whoever holds
    // many KVMs against many (a file of two thousand against a wall of two thousand) and would
    // otherwise read every address anew for every pair. A place is the host, the port as it is
    // written (-1: none of its own), and the port that is spoken to.
    struct Place {
        QString host;
        int written = -1;
        int spokenTo = 0;
    };
    static Place placeOf(const OwnKvm& kvm);
    static bool atOnePlace(const Place& one, const Place& other);
    // Why a KVM of the wall is not changed from the one to the other, with its password given or
    // without: it is on the wall over HTTPS, and would be spoken to over plain HTTP at the same
    // place ("not_in_the_clear", with what to do instead). Its password would go out unencrypted
    // and its certificate be forgotten, and whoever sees "certificate does not match" on its card
    // is who would try that, on the day somebody sits on the line. Nothing: this is not in the
    // way (the other direction, and another place, are changes like any other). Whoever changes
    // a KVM asks this before anything is asked of the KVM; change() asks it again.
    static std::optional<base::Error> intoTheClear(const OwnKvm& from, const OwnKvm& to);
    // The one of these KVMs that is known by the certificate `kvm` is known by: a KVM makes its
    // certificate itself, so that one is this KVM, by whichever address it is reached. Nothing:
    // none of them is; and a KVM that is known by none (behind somebody's front door, or reached
    // in the clear) is known by no certificate here either. (`kvm` itself is not among `there`: whoever
    // asks for one that is on the wall leaves it out.)
    // And the one that gives the number this one gives for itself (OwnKvm::number, a JetKVM's: over
    // plain HTTP it shows no certificate), where none is known by its certificate: the number is
    // said, the certificate proven. Two that give no number and are known by none are two.
    // A server's management controller is never one by a certificate, whichever of the two it is: its
    // maker may have put one certificate, and one key, on every board (the web's core.identity). A
    // server is known by its computer's number alone (OwnKvm::number), and two that give none are two.
    static std::optional<OwnKvm> twinOf(const QList<OwnKvm>& there, const OwnKvm& kvm);
    // Whether somebody said a KVM is to stay beside its twin by number (OwnKvm::keptBesideItsTwin), as the number
    // goes by: never of a server's controller, whatever a file says (a server has one card on this PC's wall: the
    // owner's word, M7 Q2).
    static bool isKeptBeside(const OwnKvm& kvm);
    // Whether both are known by one certificate, which makes two one: never where either is a
    // server's controller (above).
    static bool sameCertificate(const OwnKvm& one, const OwnKvm& other);
    // Why a KVM is not on the wall beside its twin: "same_device", in the server's sentence, which
    // names the one that is there (calledInASentence), and where it is behind that where it has
    // a name. Whoever adds a KVM, changes one or says yes to a certificate says it in these
    // words (kvm::inTheWay), and so does knowBy().
    // `saidSo`: it is known again by the number it gives for itself, which is said and not proven ("This
    // says it is the same device as ..."); otherwise by the certificate it has the key to. Of a server's
    // controller, that a server has one card on this PC's wall is said with it (the owner's word, M7 Q2).
    static base::Error sameDevice(const OwnKvm& twin, bool saidSo = false);
    // What the card of a JetKVM over plain HTTP says that is held back for its twin (its HTTPS showed the certificate
    // `twin` is known by): "same_device", why, and the deliberate acts there are for it: where it is that device, one
    // of the two goes; where it is another, it is removed and added again (the Add box looks anew), or moved onto its
    // own HTTPS. None sends its password in the clear beside that doubt (from the third reading of the twins).
    static base::Error heldBackFor(const OwnKvm& twin);
    // Whether a refusal is the twin's said by its number (sameDevice's `saidSo`): one a person may answer by
    // keeping both (OwnKvm::keptBesideItsTwin), where a certificate's may not be answered at all, nor a server's.
    static bool saysItIsTheSame(const base::Error& refusal);
    // What a KVM is called in a sentence: its name, set off by the one function for that
    // (exchange::nameSetOff: in double quotes, and so that it cannot end its own setting-off);
    // or where it is (hostOf, which is plain signs), where it has no name, or none to show.
    // Every sentence of the wall's that names a KVM names it so: a name is free text.
    static QString calledInASentence(const OwnKvm& kvm);
    // Whether a KVM of the wall is still the one somebody had before them (a box that was filled
    // with it a while ago): compared without what the wall records by itself, which is whether
    // it was asked what it is (OwnKvm::askedWhatItIs). That is no change anybody made, and comes
    // while a box is open: at the first start, when a KVM that was away comes back, during the
    // box's own asking and sign-in. Whoever asks "was it changed meanwhile" asks this. Nor is the
    // mark a server's controller's driver took for its server (OwnKvm::machine), which the wall
    // notes by itself.
    static bool isStillTheOne(const OwnKvm& now, const OwnKvm& seen);
    // The name its password is kept under.
    static QString secretOf(const QString& id);

    // --- the wall as a file (wall_exchange.h is who asks) ----------------------------------------

    // One KVM as a file named it: what it is to be called and where it is, who signs in, what
    // it is to be known by, its password where the file had one, and the certificate itself
    // where the file had that.
    struct FromAFile {
        OwnKvm kvm;
        QString password;       // empty: none, and it is to be typed
        QString certificatePem; // empty: none
    };
    // Puts KVMs from a file on the wall, last and in their order, each as "not asked yet"
    // (above): nothing is asked of any of them here, and nothing is sent. Each is taken or is
    // not, and what came of each is handed back in the order they were given: the wall's name
    // for it, or why not. The list is written down once, for all that are taken, so that a wall
    // is never half written; where that cannot be done, none of them is taken, nothing is kept,
    // and each is told so ("not_saved").
    //
    // Refused, each by itself and with the others taken: an address that is not written as
    // the program writes one; one that is on the wall already, or was given earlier in this
    // very list ("duplicate"); one that is to be known by the certificate a KVM of the wall, or
    // an earlier one of the list, is known by ("same_device"); one that is to be known by a
    // certificate that is none ("cert_invalid"); what a full wall has no room for ("too_many");
    // and one whose password Windows will not keep ("not_saved"). A password is kept as that of
    // a KVM that was added by hand is, saved for the address and for what the KVM is known by.
    // One without a password is taken without, and its card asks for it once it was asked what
    // it is. The certificate itself is kept only where it has the fingerprint the KVM is to be
    // known by; another is dropped, and the KVM is taken all the same.
    QList<base::Result<QString>> takeFromAFile(const QList<FromAFile>& those);
    // Whether a KVM of the wall was asked what it is (above), in this run of the program or, of
    // one over HTTPS, in an earlier one; false for one that is not on the wall. Whoever would
    // sign in to it with something that was typed asks this first (kvm_editing.h), and says
    // `notAskedYet` where it was not.
    bool wasAskedWhatItIs(const QString& id) const;
    // Whether the wall followed this KVM onto HTTPS by itself (a JetKVM that offered it at its own host),
    // and it is still there.
    bool followedOntoHttps(const QString& id) const;
    // Whoever sits here has seen a KVM's details, where its note said it was followed onto HTTPS: not
    // said again (Remembered::kvmsFollowedOntoHttps; the wall is another from then on: `changed`).
    void sawNote(const QString& machine) override;
    // Why nothing is sent to a KVM that was not asked yet: "not_asked_yet", a code of the
    // program's own, and one sentence. (It speaks of "a password" and not of "its password":
    // a KVM that was taken from a file without one has none.)
    static base::Error notAskedYet();
    // What an asking of what answers at a KVM's address found, where that is not the KVM: for
    // its card, and for whoever asked there before saving an address (kvm_editing.h). In the
    // code a box that adds a KVM has for it, and with what became of a password: none was sent.
    // Not asked of an asking that found the KVM (one of its family, `family`, on a line that was
    // the KVM's). A KVM of the other family that this program speaks to is another KVM, and said
    // to be ("other_family", kvm::ofAnotherFamily), as of the address it was asked at (`at`): where the
    // KVM stands (its card), this one is to be taken off the wall before the other is added.
    //
    // Where all that answered was a gateway's word that what is behind it does not answer
    // (KvmProbe::Found::saidByAGatewayAlone: a reverse proxy before a KVM that restarts, a
    // tunnel whose far end is away), that is a KVM that is away, and is said as one: in the
    // code of an address where nothing answers ("unreachable", so "offline" on its card, and
    // asked again as that is), and in a sentence that says who answered and who did not. It is
    // not "something that is no KVM": that would be untrue of a KVM that is away for a moment,
    // and reads as if somebody had added the wrong thing. No password goes anywhere either way.
    static base::Error notTheKvm(const KvmProbe::Found& found, const QString& family, kvm::AskedAt at);
    // Why the password that is kept for a KVM of the wall does not go with it to `address`
    // unless it is typed anew; nothing: it goes. The rule signerIn() and change() share, asked
    // by itself, and of a KVM that was not asked yet as well: "not_found"; and "password_needed"
    // for another address, unless both are HTTPS and the KVM is known by a certificate.
    std::optional<base::Error> keptPasswordStaysBehind(const QString& id, const QUrl& address) const;
    // The certificate itself that is kept for a KVM of the wall, as a file holds one; empty:
    // none is kept (yet).
    QString certificateOf(const QString& id) const;
    // The wall's KVMs as a file holds devices (exchange.h): each as a device of the family it
    // is, with its name, address, user and group, and what it is known by: over HTTPS by its
    // certificate ("pinned", with the certificate itself where that is kept, and its fingerprint
    // alone where it is not: the writer says so beside the file), or by Windows' word
    // ("system"); over plain HTTP as a file says that. With each password as it is kept for the
    // KVM as it is, where `withPasswords`, which is for a file that is sealed: none is read from
    // the store for a plain one. `withoutPassword`: the KVMs, by their place on the wall counted
    // from 0, whose password the store did not hand out or does not hold; they are written
    // without, and whoever writes the file says so beside it.
    //
    // The passwords are in what is handed back, which is for the writer of a file and for
    // nobody else: it is shown nowhere and written to no log.
    struct AsAFile {
        exchange::Content content;
        QList<qsizetype> withoutPassword;
    };
    AsAFile asAFile(bool withPasswords);
    exchange::WallSettings portableSettings() const;
    base::Result<void> takeSettings(const exchange::WallSettings& settings);

signals:
    void portableSettingsChanged();
    // The wall's KVMs, or their order, are not what they were. (Not said where a KVM was asked
    // what it is, or its certificate itself was kept: the wall is the one it was, and how a KVM
    // is, is asked with askStatus.) Or its saved actions are not what they were
    // (OwnActions::changed): whoever shows the wall asks for those anew as well.
    void changed();
    // A KVM of the wall has come to something with nobody having asked how it is just then:
    // an asking did not find it (or it cannot be asked at all, for want of a certificate to
    // hold a line to), and its card says so from now on, or says another thing than after the
    // asking before; or its driver, which was only just made (with the wall, at a start, for
    // a KVM that was asked in an earlier run; or because the KVM was found by an asking, added,
    // or changed), has looked at it for the first time, whatever that look found. (So an
    // ordinary start says this once or twice for looks that the first answer already had:
    // one asking more of how the KVMs are, which costs no call to any KVM.) The
    // wall is the one it was (`changed` is not said). How the KVMs are is to be asked now, with
    // askStatus, and not at the next turn of whoever shows the wall: that is how a tile comes
    // to say what its KVM came to within a moment of that. Said once for all that came to
    // something within a short while of each other (Own::saidAfterMs), a turn of the loop or
    // more after whatever brought it about, and never from inside a call. Not said of a KVM
    // that is asked again and is away as before, nor of what a later look finds: that is what
    // whoever shows the wall asks at their own pace.
    void cameToSomething();

protected:
    // Keeps the certificate itself that a KVM's driver was shown, where it is the one the KVM
    // is known by as the KVM is on the wall NOW, a file of the wall would hold it, and none is
    // kept yet. What a driver was shown a moment ago may be the certificate of the KVM as it
    // was: the driver says so a turn of the loop before this is asked, and the KVM may have
    // come to be known by another in between. (Not private, so that a test can ask it by
    // itself: that turn of the loop is not one a test can step into.)
    void keepCertificate(const QString& id, const QByteArray& der);
    // A JetKVM of the wall over plain HTTP at `from` offers HTTPS at `at`, its own host (`found`: what
    // answered there, with `isSetup`): the wall moves it there by itself, as the web's `moved` does
    // (server.py), by its own rules and no other: still at `from`, `at` its own HTTPS as it is now
    // (ontoHttps()), asked what it is, the place free and no other KVM of the wall that it is (alter:
    // another KVM there, a kept one, a twin). What is live and what is saved change together; its
    // password goes along, saved anew for that address and what it showed there; it stays asked (that
    // answer is an asking). Where it cannot be saved, nothing is changed, and its driver offers it
    // again at its next due look. True: it was moved. (Not private, so that a test can ask it by
    // itself: the turn of the loop between an offer and this is not one a test can step into.)
    // An error where its HTTPS shows the certificate another KVM of the wall is known by (proven: "same_device"):
    // that is the other one, and this one is held back (heldBackForItsTwin).
    base::Result<bool> followOntoHttps(const QString& id, const QUrl& from, const QUrl& at,
                                       const KvmProbe::Found& found);

private:
    // What the wall's drivers kept of their sign-ins to this address (SignInPacing), cleared: of a KVM taken off it.
    void forgetSignInsAt(const QUrl& address);

    struct Held {
        QString id;
        // Its family's driver (own_device.h; a kvmd's: KvmdDevice).
        std::unique_ptr<OwnDevice> device;
        bool busy = false; // something is being done to it
        // One higher each time what is kept as its secret has become another: with every change
        // that wrote it, and with one that failed after it and could not put back what was kept.
        quint64 secretWrites = 0;
        // Whoever asked its driver something and has no answer yet, each under a number of its
        // own: as what tells that one why no answer comes, should the driver be let go first.
        QHash<quint64, std::function<void(const base::Error&)>> owed;
        // The KVM live, for all who show it large (OwnDevice::makeLive; a kvmd's: kvmd_live.h):
        // made when it is first asked for, kept for as long as this driver is, and ended with it.
        std::unique_ptr<OwnLive> live;
        // What answers at its address was asked what it is, and said that it is a KVM of the
        // family. Until then its driver has no password and is asked nothing.
        bool asked = true;
        // The asking that is under way, by its number; 0: none is.
        quint64 asking = 0;
        // Somebody asked how it is, it is to be asked what it is, and no place is free for that
        // (or as many askings are under way as there may be): it waits for its turn.
        bool waitsToBeAsked = false;
        // What came of the last asking that did not find the KVM, for its card; when the next
        // is due, by the wall's clock; and how many in a row did not find it.
        std::optional<base::Error> saidWhenAsked;
        qint64 askAgainAt = 0;
        int askedInVain = 0;
        // Whoever waits for the asking that is under way: told when it has ended, or when it
        // has gone on without its place, whichever is first.
        QList<std::function<void()>> afterAsking;
        // When its HTTPS was last asked beside its asking (a JetKVM over plain HTTP): at its first
        // asking, and then no more often than its family's wayWhenDownMs, by the wall's clock.
        std::optional<qint64> overHttpsAskedAt;
        // Its HTTPS showed the certificate another KVM of the wall is known by: that other one may be
        // what answers its plain way too, and no password goes there in the clear. Not asked what it is
        // meanwhile, its card saying so; and its HTTPS asked again at every asking, whatever the way's
        // pace, until it shows a certificate no other KVM of the wall is known by, or nothing listens
        // there (in doubt, it stays held, for as long as the one it is held for is on the wall). Kept
        // across a start (heldFor).
        bool heldBackByTwin = false;
        // A JetKVM's: what outlives each of its drivers (design 3.4: that the program stepped aside for
        // another viewer, and its one connection at a time), handed to every driver it has.
        std::shared_ptr<JetKvmKept> jetkvm;
    };

    Held* held(const QString& id);
    // The driver of the KVM's family (OwnKvm::kind: a kvmd's KvmdDevice, a JetKVM's JetKvmDevice, a
    // server's controller's RedfishDevice), made of where it is, who signs in, what it is known by (and of
    // a controller, the mark its server is kept by) and its password, at its family's pace; a JetKVM's with
    // what its record keeps across its drivers (Held::jetkvm, made here where there is none).
    std::unique_ptr<OwnDevice> driverFor(Held& one, const OwnKvm& kvm, const QString& password) const;
    // Holds a KVM from now on, as one that was asked what it is or was not. Its driver is given
    // the password only where it was; of one that was not, nothing is done with what is handed in.
    void hold(const OwnKvm& kvm, bool asked, const QString& password);
    // Listens to a KVM's driver for the certificate the KVM is known by (keepCertificate), for the
    // end of the first look it takes (noteThatOneCameToSomething), of a JetKVM's, for its other
    // way offering HTTPS (followOntoHttps), and of a controller's, for the server it took (knowsItsServer).
    void listenTo(const Held& one);
    // A controller's driver (`by`) took the server that answered for its own (`now`; it had `before`): noted
    // with the KVM by itself, where the KVM is still the one that driver was made for (its driver is that
    // one, and the mark is still `before`), and for no other.
    void knowsItsServer(const QString& id, const OwnDevice* by, const QString& before, const QString& now);

    // Asks what answers at the address of a KVM that was not asked yet, unless that is under
    // way or not due yet, or there is no room for it (then it waits its turn:
    // askThoseThatWait); true: an asking is under way now.
    bool ask(Held& one);
    // Whether one more asking may begin: a place is free, and fewer are under way in all than
    // there may be.
    bool hasRoomToAsk() const;
    // There is room for an asking again: the next that wait are asked. Those first that were
    // never asked in vain, in the wall's order; then those that were, the one first that has
    // been due for the longest, so that each of them is asked again by turns however many
    // stay off.
    void askThoseThatWait();
    // An asking has held its place for as long as one does, and has not ended: it goes on
    // without it. Whoever waited for it is told, and the next that wait are asked.
    void leavesItsPlace(quint64 number);
    // A KVM has come to something: said soon, for all that have by then (cameToSomething).
    void noteThatOneCameToSomething();
    // A KVM over plain HTTP whose HTTPS shows another of the wall (`twin`; its card says heldBackFor()): not asked from
    // here on, its driver one with no password, and asked again later, and later each time. Noted with it, and which
    // one it is held for (Remembered::kvmsHeldForATwin): it is held so from the start of the next run too.
    void heldBackForItsTwin(Held& one, const OwnKvm& twin);
    // The same where its driver found it so (its other way, or the look before a sign-in): that driver let go of
    // (whoever it owed an answer is told heldBackFor()), and one with no password made.
    void heldBackFromItsDriver(Held& one, const OwnKvm& known, const OwnKvm& twin);
    // A look at its own HTTPS (judgeItsHttps) showed the certificate `twin` is known by: held back for that one from
    // now on. Where its driver may hold its password, and may be the very driver whose look that was, the hold is noted
    // at once (every sign-in from then on is held to it) and that driver let go of a turn of the loop later; else as
    // heldBackForItsTwin. Nothing where it is held for that one already.
    void heldBackOnALook(Held& one, const OwnKvm& twin);
    // The KVM of the wall it is held back for, as noted (heldBackForItsTwin): nothing where none is noted, or that one
    // is not on the wall any more.
    std::optional<OwnKvm> heldFor(const QString& id) const;
    // What a look at the HTTPS of a KVM of the wall (`id`; none: one that is being added) holds it back for: the KVM
    // whose certificate it showed; or, where it is held back already and the look shows nothing that lets go of that,
    // the one it is held for. Nothing: neither. (judgeItsHttps, and the wall's own asking: the one rule for both.)
    std::optional<OwnKvm> twinBehind(const QString& id, const KvmProbe::Found& found) const;
    // Notes that it is held back for that one (`twin`, its id), or that it is not (empty), where that is another than
    // was noted: remembered at once, and written down with what else the wall notes by itself.
    void markHeldFor(const QString& id, const QString& twin);
    // The KVM of the wall, other than this one (`id`), that is known by the certificate a line showed (`shown`, its
    // SHA-256; sameCertificate): what showed it is that one, whether Windows would trust that certificate there or
    // not. Nothing: none is, or nothing was shown.
    std::optional<OwnKvm> anotherKnownBy(const QString& id, const QByteArray& shown) const;
    // What an asking found.
    // `overHttps`: what answered at the HTTPS way of a JetKVM over plain HTTP, asked beside it with the
    // JetKVM's question alone; a JetKVM there is followed there before any driver has its password.
    void wasAsked(const QString& id, quint64 number, const OwnKvm& askedAs, const KvmProbe::Found& found,
                  const std::optional<KvmProbe::Found>& overHttps = std::nullopt);
    // Whether a KVM is one of which the wall keeps that it was asked: one over HTTPS.
    static bool keepsThatItWasAsked(const OwnKvm& kvm);
    // Notes these as the wall's KVMs and their certificates, for what the wall records by
    // itself: remembered at once, and written down a short while later (Own), together with
    // whatever else was noted by then.
    void noteByItself(const QList<OwnKvm>& kvms, const QHash<QString, QString>& certificates);
    // How a KVM is that was not asked yet, for its card.
    Status howItIsUnasked(const Held& one) const;
    // Why no picture of such a KVM is to be had, and why it is not had live: "ask again soon"
    // ("no_stream") while it is being asked, or waits to be, for the first time; and that it
    // was not asked yet (`notAskedYet`) once an asking did not find the KVM.
    static base::Error whyNothingYet(const Held& one);
    // What a driver is handed in the place of `done`: it passes the driver's answer on while the
    // answer is still owed, and to nobody once the driver was let go.
    template <typename Answer>
    std::function<void(base::Result<Answer>)> owedBy(Held& one, std::function<void(base::Result<Answer>)> done);
    // Lets a KVM's driver go, and tells whoever it still owes an answer `why` none comes.
    void letGo(Held& one, const base::Error& why);
    // What change() and knowBy() both do; `what` is what could not be saved, in the server's
    // words. `saidYesTo`: the certificate it is to be known by is one somebody was shown and said
    // yes to, and the kept password goes with it on that word, not by signerIn()'s rule.
    base::Result<void> alter(const QString& id, OwnKvm kvm, const std::optional<QString>& password, const char* what,
                             bool saidYesTo, const std::optional<KvmProbe::Found>& overHttps = std::nullopt);
    // Remembers these as the wall's KVMs; where that cannot be written down, the ones before
    // are the wall's again, and why is told. The certificates that are kept beside them, and the
    // marks of those for which a password is kept, stay as they are, or are these: written
    // together with the list, and put back together with it.
    base::Result<void> remember(const QList<OwnKvm>& kvms, const char* what);
    base::Result<void> remember(const QList<OwnKvm>& kvms, const QHash<QString, QString>& certificates,
                                const QSet<QString>& marks, const char* what);
    void later(std::function<void()> what);

    Settings& m_settings;
    SecretStore& m_secrets;
    // The wall's saved actions, which are kept beside its KVMs: in the same settings, their texts
    // in the same store.
    OwnActions m_actions;
    ScreenWatch m_screens;
    // The KVMs that are watched, and how each is now, for a round of the watches.
    QList<ScreenWatch::Watched> watchedScreens() const;
    // The times of the KVMs' drivers, each family's. The wall's own that are a driver's (how long an
    // answer waits for a look, how long a KVM that an asking did not find is left alone) are the
    // kvmd's.
    DriverPaces m_paces;
    Own m_own;
    std::vector<Held> m_held;
    quint64 m_asked = 0; // counts what was asked of the drivers, and the askings of what answers
    // The askings of what answers that are out at this moment, by their numbers: each from when
    // it was begun until its probe has said what it found, whoever that is still for. And those
    // of them that hold a place still (Own::askedAtOnce, Own::placeHeldMs).
    QSet<quint64> m_askingsOut;
    QSet<quint64> m_inAPlace;
    // Says what the wall has to say by itself of its KVMs, a short while after the first of it.
    QTimer m_saySoon;
    QTimer m_tidy;
    // For when a KVM that was not asked yet is asked again, and for when what was noted is
    // written down at the latest.
    QElapsedTimer m_clock;
    // Writes down what the wall noted by itself; and since when something is noted and unwritten.
    QTimer m_writeSoon;
    qint64 m_notedSince = 0;
    // The program is ending (letGoOfEveryDriver): no driver is made any more.
    bool m_ended = false;
    // What asks a KVM that was not asked yet what it is: made when one is first asked, so that a
    // wall whose KVMs were all added by hand makes nothing for it. And for a server's controller, one
    // that waits for its service root as its driver does (KvmProbe::Pace::rootQuietMs), which a KVM's
    // asking does not: one that is off is given up on in a KVM's time. Last, so that they go first,
    // and their askings with them.
    std::unique_ptr<KvmProbe> m_probe;
    std::unique_ptr<KvmProbe> m_serverProbe;
};

} // namespace qkvm::core
