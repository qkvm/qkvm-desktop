#pragma once

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

// What the box that changes a KVM of this PC's own wall is given, and what it asks for (SPEC.md
// section 4, M3a): the KVM as it is on the wall, to be changed or taken off, and the certificate it
// is known by beside the one it shows right now, to be said yes to. The box only passes the wish
// on: whoever changes the KVM hears it, and writes here what came of it. Nothing here speaks to a
// KVM. It keeps nothing but what the box shows, and that only while the box is open; the password
// that is typed is passed on and kept nowhere.
//
// One for each window: what one window's box asks for and is told is nothing to another's.
//
// Made in C++ and handed to the screens; lives on the interface thread.
class EditKvmView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // The KVM the box is open for, by the wall's name for it; empty: for none.
    Q_PROPERTY(QString machine READ machine NOTIFY changed)
    // What its card calls it: its name, or where it is.
    Q_PROPERTY(QString called READ called NOTIFY changed)
    // And what a sentence calls it: its name set off so that it cannot read as the sentence's
    // own words, or where it is (core::LocalWall::calledInASentence, which is who makes it: a
    // name is free text, typed or from a file). Every sentence and every title of the box that
    // names the KVM has this, and never the name as it is.
    Q_PROPERTY(QString calledInASentence READ calledInASentence NOTIFY changed)
    // What stands in the box's lines when it is opened. Its name as it is saved (empty for one that
    // is called by where it is), and its address as it is typed back: "https://" is not written,
    // "http://" is.
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString address READ address NOTIFY changed)
    Q_PROPERTY(QString user READ user NOTIFY changed)
    Q_PROPERTY(QString group READ group NOTIFY changed)
    Q_PROPERTY(bool watchable READ watchable NOTIFY changed)
    Q_PROPERTY(int noSignalMinutes READ noSignalMinutes NOTIFY changed)
    Q_PROPERTY(int unchangedMinutes READ unchangedMinutes NOTIFY changed)
    // Its family (core::OwnKvm::kind): "kvmd", or "jetkvm", which has no user name. Empty while the
    // box is open for none.
    Q_PROPERTY(QString family READ family NOTIFY changed)
    // The certificate it is known by: its SHA-256 as the page writes one. Empty where it is known
    // by none (plain HTTP, or a certificate Windows trusts).
    Q_PROPERTY(QString certificate READ certificate NOTIFY changed)
    // It is spoken to over plain HTTP; it is spoken to over HTTPS, and so shows a certificate.
    // (Neither while the box is open for none.)
    Q_PROPERTY(bool overPlainHttp READ isOverPlainHttp NOTIFY changed)
    Q_PROPERTY(bool https READ isHttps NOTIFY changed)
    // The box was opened for its certificate alone (a card's "Review certificate"): its lines are
    // not shown.
    Q_PROPERTY(bool forReview READ isForReview NOTIFY changed)
    // The box was opened for the question before the KVM is taken off the wall, and for nothing
    // else (a list's "Remove"): it shows that question, and its way back from it closes the box.
    Q_PROPERTY(bool removesOnly READ removesOnly NOTIFY changed)
    // Something that was asked is not answered yet, and what it was: "saving", "removing",
    // "looking" (at its certificate) or "trusting". The last stands until the next is asked.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString asked READ asked NOTIFY changed)
    // While a saving is under way: its sign-in waits for an earlier one to the KVM with the same password to be
    // answered (another window's, the wall's own driver's), and the box says so.
    Q_PROPERTY(bool waitsForAnotherSignIn READ waitsForAnotherSignIn NOTIFY changed)
    // Why not, as a sentence to show; empty where it was done, or nothing was asked, or the KVM went
    // meanwhile (`gone` says that).
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // Refused because it says by its number that it is another KVM of the wall: the person may keep both, as
    // on the page (saveKeepingBoth).
    Q_PROPERTY(bool mayKeepBoth READ mayKeepBoth NOTIFY changed)
    // The KVM it is open for was taken off the wall elsewhere while it was open (wentAway): there is
    // nothing of it to change, to take off or to say yes to any more. Until it is opened anew.
    Q_PROPERTY(bool gone READ isGone NOTIFY changed)
    // The two certificates of a review are there: what it is known by (empty: by none yet) and
    // what it shows right now, each as the page writes one, and whether they are the same.
    Q_PROPERTY(bool reviewed READ isReviewed NOTIFY changed)
    Q_PROPERTY(QString remembered READ remembered NOTIFY changed)
    Q_PROPERTY(QString presented READ presented NOTIFY changed)
    Q_PROPERTY(bool same READ isSame NOTIFY changed)
    // What it shows is the certificate another KVM of the wall is known by: what that one is
    // called. No yes is taken to it (the wall would refuse it). Empty: it is no other's.
    Q_PROPERTY(QString presentedIsCalled READ presentedIsCalled NOTIFY changed)
    // A saving was refused over what answers at the address that was typed: the KVM would be
    // known otherwise there than it is known now. What it is known by and what is shown there
    // are there as the two of a review are (`remembered`, `presented`), and so is the address
    // the second was shown at, as it is typed.
    Q_PROPERTY(bool differs READ differs NOTIFY changed)
    Q_PROPERTY(QString newAddress READ newAddress NOTIFY changed)
    // And: Windows trusts what is shown there for that name. The KVM would be known by Windows'
    // word there, and by no certificate: that is what a yes is said to, and what is shown in
    // the place of a second certificate.
    Q_PROPERTY(bool onWindowsWord READ isOnWindowsWord NOTIFY changed)
    // Or: that address is plain HTTP, and the KVM is spoken to over HTTPS. Nothing is shown
    // there, and no second certificate is there: what stands beside what the KVM is known by
    // is what is given up, and that is what a yes is said to.
    Q_PROPERTY(bool plainHttpThere READ isPlainHttpThere NOTIFY changed)
    // A yes was given, and what is shown now is another than the one it was given to: the two
    // that are there came in the place of the two that were said yes to. Until they go, or a
    // yes is given to them. For an address that was typed that is so for as long as the box is
    // open for this KVM, whatever was asked in between: whoever answers there may show the
    // one to the questions and another only when the sign-in goes out.
    Q_PROPERTY(bool yesWasToAnother READ yesWasToAnother NOTIFY changed)
    // And that yes was to Windows' word for what was shown at that address: no certificate
    // stood there that the one that is shown now could be "another than".
    Q_PROPERTY(bool yesWasOnWindowsWord READ yesWasOnWindowsWord NOTIFY changed)
    // The screens say whether the two are looked at: the box is open on them, in a window that
    // is in sight and is the active one. Not, until they say so.
    Q_PROPERTY(bool lookedAt READ isLookedAt WRITE setLookedAt NOTIFY changed)
    // The two that are there have been looked at for a moment: only now is a yes to the second
    // one passed on (trustRestsMs).
    Q_PROPERTY(bool mayTrust READ mayTrust NOTIFY changed)
    // It was saved, and the box stays open on it: what it is known by, or how it is spoken to, is
    // another than the box showed, and is to be seen (`certificate`, `overPlainHttp`). What a
    // sentence calls the KVM now; empty otherwise, and from the next thing that is asked.
    Q_PROPERTY(QString savedAs READ savedAs NOTIFY changed)
    // And it was saved over HTTPS where it was spoken to over plain HTTP, at its own host: a JetKVM
    // whose owner turned that on (core::KvmEditing::Changed::ontoHttps). Said with `certificate`.
    Q_PROPERTY(bool ontoHttps READ isOntoHttps NOTIFY changed)

public:
    // A KVM as its box shows it.
    struct Shown {
        QString id;
        QString called;
        // What a sentence calls it (`calledInASentence`).
        QString inASentence;
        QString name;
        QString address;
        QString user;
        QString group;
        QString family;
        QString certificate;
        bool overPlainHttp = false;
        bool https = false;
        // Its screen watches, in minutes (0: not watched), where its card shows a screen (not a server's controller).
        bool watchable = false;
        int noSignalMinutes = 0;
        int unchangedMinutes = 0;

        bool operator==(const Shown&) const = default;
    };

    // What a box shows to be said yes to: for which KVM, which certificate (none where the
    // address is plain HTTP), at which address as it is typed (empty: the one the KVM has, in
    // a look at its certificate), and whether the yes would be to Windows' word for that
    // certificate, or to plain HTTP there.
    struct Offer {
        QString machine;
        QByteArray certificate;
        QString at;
        bool onWindowsWord = false;
        bool plainHttp = false;

        bool operator==(const Offer&) const = default;
    };

    // How long a yes takes no click after the two have come, in milliseconds. They come when
    // whatever answers at the KVM's address has answered, which that one chooses: a click that
    // was on its way to something else, or the second of two, is on the screen at that moment.
    // About as long as it takes to see that something else stands there now. It runs only while
    // the two are looked at (`lookedAt`), and from the beginning whenever they are looked at
    // again: a click that brings the window back lands on whatever is under it.
    static constexpr int trustRestsMs = 1000;
    // And after a yes was given to what is not shown any more (`yesWasToAnother`): three times
    // as long. What stands there looks as it did, but for one of two rows of letters: long
    // enough to read the sentence that says so, and to find the row.
    static constexpr int trustRestsAnewMs = 3000;

    explicit EditKvmView(QObject* parent = nullptr);

    QString machine() const { return m_shown.id; }
    QString called() const { return m_shown.called; }
    QString calledInASentence() const { return m_shown.inASentence; }
    QString name() const { return m_shown.name; }
    QString address() const { return m_shown.address; }
    bool watchable() const { return m_shown.watchable; }
    int noSignalMinutes() const { return m_shown.noSignalMinutes; }
    int unchangedMinutes() const { return m_shown.unchangedMinutes; }
    // Its screen watches as they stand in the box, said before it is saved (c7): minutes, empty for off. What is
    // no whole number is none; one longer than a day is a day. Saved with the rest of the change (typedWatches).
    Q_INVOKABLE void typeWatches(const QString& noSignal, const QString& unchanged);
    std::optional<std::pair<int, int>> typedWatches() const { return m_typedWatches; }
    QString user() const { return m_shown.user; }
    QString group() const { return m_shown.group; }
    QString family() const { return m_shown.family; }
    QString certificate() const { return m_shown.certificate; }
    bool isOverPlainHttp() const { return m_shown.overPlainHttp; }
    bool isHttps() const { return m_shown.https; }
    bool isForReview() const { return m_forReview; }
    bool removesOnly() const { return m_removesOnly; }
    bool busy() const { return m_busy; }
    QString asked() const { return m_asked; }
    bool waitsForAnotherSignIn() const { return m_busy && m_waitsForAnother; }
    QString problem() const { return m_problem; }
    bool mayKeepBoth() const { return m_mayKeepBoth; }
    bool isGone() const { return m_gone; }
    bool isReviewed() const { return m_reviewed; }
    QString remembered() const { return m_remembered; }
    QString presented() const { return m_presented; }
    bool isSame() const { return m_reviewed && m_rememberedSha256 == m_presentedSha256; }
    QString presentedIsCalled() const { return m_presentedIsCalled; }
    bool differs() const { return m_differs; }
    QString newAddress() const { return m_newAddress; }
    bool isOnWindowsWord() const { return m_onWindowsWord; }
    bool isPlainHttpThere() const { return m_plainHttpThere; }
    bool yesWasToAnother() const { return m_yesWasToAnother; }
    bool yesWasOnWindowsWord() const { return m_yesWasOnWindowsWord; }
    bool isLookedAt() const { return m_lookedAt; }
    void setLookedAt(bool lookedAt);
    bool mayTrust() const { return (m_reviewed || m_differs) && m_presentedIsCalled.isEmpty() && m_rested; }
    QString savedAs() const { return m_savedAs; }
    bool isOntoHttps() const { return m_ontoHttps; }

    // What the screens ask for. The box for a machine of the wall, by the wall's name for it;
    // `review`: for its certificate alone. Nothing while something is asked.
    Q_INVOKABLE void openFor(const QString& machine, bool review);
    // (What was typed of the watches goes with the box's next saving, and no other.)
    // The box for a machine of the wall, at the question before it is taken off the wall, and for
    // that alone (`removesOnly`). Nothing while something is asked. A machine that is not on the
    // wall opens nothing, and the box that is opened next for another way is that one's.
    Q_INVOKABLE void openToRemove(const QString& machine);
    // Of the KVM the box is open for, each nothing while something is asked: that it be changed to
    // what was typed; that it be taken off the wall; that its certificate be read, to be looked at
    // beside the one it is known by; and that it be known from now on by the certificate the box
    // SHOWS. That very one is handed on, as it was read for the review, and never one that was read
    // again in between: nobody has seen that one. Nor one that has only just come (mayTrust), nor
    // one that is another KVM's of the wall.
    Q_INVOKABLE void save(const QString& name, const QString& address, const QString& user, const QString& password,
                          const QString& group);
    Q_INVOKABLE void remove();
    Q_INVOKABLE void review();
    Q_INVOKABLE void trust();
    // The yes where a saving was refused over what is shown at the new address (`differs`): that
    // the KVM be changed to what was typed after all, and be known there by what the box SHOWS
    // for that address; or, where that address is plain HTTP, be spoken to in the clear there.
    // That very one is handed on with what was typed, by this and by nothing
    // else the screens can ask for. Nothing unless the two are there, and have been looked at
    // for a moment (mayTrust).
    Q_INVOKABLE void saveKnownByWhatIsShown(const QString& name, const QString& address, const QString& user,
                                            const QString& password, const QString& group);
    // What is being asked is not waited for any longer: the box stays, and is told what came of
    // it. Nothing while nothing is asked.
    Q_INVOKABLE void giveUp();
    // A saving as save()'s, to be kept beside the KVM of the wall it says it is: only where that is what it was
    // refused for (mayKeepBoth).
    Q_INVOKABLE void saveKeepingBoth(const QString& name, const QString& address, const QString& user,
                                     const QString& password, const QString& group);
    // The two certificates were looked at: back to the box's lines, and they are shown no longer.
    Q_INVOKABLE void closeReview();
    // The box was closed: nothing of the KVM is shown or kept any more, and what was being asked
    // is given up.
    Q_INVOKABLE void closed();

    // What whoever keeps the windows says: the box is to be closed, from outside (its window is
    // out of sight). The screens close it and empty its lines; what it shows and waits for is
    // gone there and then, whether or not there are screens.
    void close();
    // And whether a KVM may be changed or taken off from here just now: not while the program
    // only shows (a screen nobody sits at: MergedWall::onlyShows). Where it may not, the box is
    // closed as above, and nothing it asks is passed on from then on: it opens for no KVM, saves
    // nothing, takes nothing off, and neither looks at a certificate nor takes one. It may, until
    // it is told otherwise.
    void setMayChange(bool mayChange);
    bool mayChange() const { return m_mayChange; }

    // What whoever changes the KVM says. The box is to be opened for this KVM; `forReview`: for
    // its certificate alone, which is asked for at once.
    void show(const Shown& kvm, bool forReview);
    // And of what the box asked, each but the first only while it is waited for (what comes for a
    // box that gave it up, or was closed, is nobody's): that it is being done, and why it was not.
    void began(const QString& what);
    // What it asked waits for an earlier sign-in to the KVM (core::KvmEditing::signInWaits).
    void signInWaits();
    void refused(const QString& why, bool mayKeepBoth = false);
    // It was not done because the KVM is not what the box showed any more (another window changed
    // it): the box stays, and shows the KVM as it is now, its lines filled anew.
    void refusedOver(const Shown& now, const QString& why);
    // While the box is open for it, the KVM was renamed elsewhere (another window, a file), and is
    // called this now: its card's word for it, and a sentence's. The box's title follows; nothing
    // else of the box changes (its lines keep what stands in them, and a saving over what was
    // renamed is refused as one over a KVM another window changed). Nothing while the box is open
    // for none.
    void renamed(const QString& called, const QString& inASentence);
    // While the box is open for it, the KVM was taken off the wall elsewhere: so it is from then on
    // (`gone`, which the screens read: the box stays, whatever it shows, and says so), also where the
    // box was asking something just then, which ends as it ends. Nothing of it is asked any more:
    // no saving, no taking off, no look at its certificate and no yes to one. Nothing while the box
    // is open for none, and nothing a second time.
    void wentAway();
    // It was changed, and is called this now; it is off the wall; it is known by the certificate
    // that was shown. The box closes on each. `name`, here and wherever this is told what a KVM
    // is called for a sentence of the screens (`isCalled` of reviewed(), savedAsItClosed(),
    // trustedAsItClosed()): as a sentence calls it, set off by whoever knows the KVM
    // (core::LocalWall::calledInASentence), and never its name as it is.
    void saved(const QString& name);
    void removed(const QString& name);
    void trusted(const QString& name);
    // It was changed, and what it is known by, or how it is spoken to, is another than the box
    // showed: the box does not close. It shows the KVM as it is now, its lines filled anew, and
    // says that it was saved (`savedAs`); and, `ontoHttps`, that it answers over HTTPS now.
    void savedAndStays(const Shown& now, bool ontoHttps = false);
    // The certificate it is known by (empty: by none) and the one it shows right now, each by its
    // SHA-256; and what the KVM of the wall is called whose certificate the second is, where it
    // is another one's. Where they come for a yes (it was said to a certificate that is shown no
    // longer), that is said with them (`yesWasToAnother`), and the yes rests anew, for longer.
    void reviewed(const QByteArray& remembered, const QByteArray& presented, const QString& isCalled = {});
    // The saving was refused over what is shown at the new address: what the KVM is known by
    // (empty: by none) and the certificate that is shown there, each by its SHA-256; the address
    // as it is typed; and whether Windows trusts what is shown there, so that the KVM would be
    // known by Windows' word. Where they come for a yes, as above.
    void differing(const QByteArray& remembered, const QByteArray& presented, const QString& at,
                   bool onWindowsWord = false);
    // The saving was refused because the address that was typed is plain HTTP, and the KVM is
    // spoken to over HTTPS: what it is known by (empty: by Windows' word), and that address
    // as it is typed. Nothing was asked there, and nothing is shown of it.
    void givingUpHttps(const QByteArray& remembered, const QString& at);
    // The address that was asked is another than the one typed (a JetKVM, asked both ways): its line
    // is to say this one from here on, as it is typed, so that a yes that is given goes with it.
    void retype(const QString& address);
    // What the box shows to be said yes to right now, whether or not it takes a yes yet;
    // nothing where it shows none, or what it shows is another KVM's of the wall. For whoever
    // changes the KVM: only what the box shows is offered to it.
    std::optional<Offer> offers() const;
    // What the yes that is being passed on this moment was said to: what the box showed when
    // its button was pressed, having been looked at for its moment. Only while the wish that
    // carries it is handed on (saveAsked, trustAsked), and nothing at any other time: a wish
    // that somebody sends by the signal alone comes with no yes.
    const std::optional<Offer>& saysYesTo() const { return m_passedOn; }
    // It was given up, as the box asked: nothing was done, and there is nothing to complain of.
    void gaveUp();
    // What the box had asked was done in the very turn in which the box was closed over it:
    // while the box is being closed, and to be said by its window, since the box says nothing
    // any more. That it was saved, as what; and where the box would have stayed open (above)
    // what it would have stayed for: the certificate the KVM is known by now, as the page writes
    // one (empty: by none), and whether it is spoken to in the clear. Or that it was trusted.
    void savedAsItClosed(const QString& name, bool known, const QString& certificate, bool overPlainHttp);
    void trustedAsItClosed(const QString& name);

signals:
    void changed();
    // For the screens: the box is to be opened, for the KVM that is shown here from now on.
    void opened();
    // The box stays open, and its lines are to be filled anew with the KVM as it is shown here
    // now: what was typed into them is gone, the password too.
    void filledAnew();
    // The box is to be closed, with nothing kept of what was typed.
    void closeAsked();
    // And it is over: "saved", "removed" or "trusted", and what the KVM is called. The box closes.
    void done(const QString& what, const QString& name);
    // It was over as the box closed: "saved" or "trusted", what the KVM is called, and (`known`)
    // what it is known by now where that is to be seen. The window says it as a notice.
    void doneAsItClosed(const QString& what, const QString& name, bool known, const QString& certificate,
                        bool overPlainHttp);
    // The box's line for the address is to say this from here on (retype()).
    void addressRetyped(const QString& address);
    // The saving under way waits for an earlier sign-in to the KVM to be answered: the window says so, once a
    // saving (waitsForAnotherSignIn says it meanwhile).
    void signInWaitsForAnEarlierOne();

    // For whoever changes the KVM. `saidYesTo`: the certificate the box showed for the new address
    // and was told yes to, by its SHA-256; empty: none.
    void openAsked(const QString& machine, bool review);
    void saveAsked(const QString& machine, const QString& name, const QString& address, const QString& user,
                   const QString& password, const QString& group, const QByteArray& saidYesTo, bool keepBoth);
    void removeAsked(const QString& machine);
    void reviewAsked(const QString& machine);
    void trustAsked(const QString& machine, const QByteArray& presented);
    // `closing`: by a box that was closed over what it asked, and hears nothing of it any more;
    // otherwise the box stays, and is told what came of it.
    void giveUpAsked(bool closing);

private:
    // Whether something of the KVM that is shown can be asked now: not of one that is gone from the
    // wall, whatever the screens ask.
    bool canAsk() const { return m_mayChange && !m_busy && !m_gone && !m_shown.id.isEmpty(); }
    void forgetTheReview();
    // And what was said yes to while the box was open for this KVM: it is another from here on.
    void forgetTheKvm();
    void over(const QString& what, const QString& name);
    void showTheTwo(const QByteArray& remembered, const QByteArray& presented);
    void showAtTheNewAddress(const QString& at, bool onWindowsWord, bool plainHttp);
    void restAnew();
    void saidYes();

    // What was said yes to at an address that was typed.
    struct SaidYes {
        QByteArray certificate;
        bool onWindowsWord = false;
    };

    Shown m_shown;
    std::optional<std::pair<int, int>> m_typedWatches;
    bool m_mayChange = true;
    bool m_forReview = false;
    // The box is open for the question alone; and it was asked to be opened so, and has not been yet.
    bool m_removesOnly = false;
    bool m_toRemove = false;
    bool m_busy = false;
    bool m_waitsForAnother = false; // (for the saving under way: `began` puts it back)
    QString m_asked;
    QString m_problem;
    bool m_mayKeepBoth = false;
    bool m_gone = false;
    bool m_reviewed = false;
    bool m_differs = false;
    QString m_newAddress;
    bool m_onWindowsWord = false;
    QString m_presentedIsCalled;
    QString m_savedAs;
    bool m_ontoHttps = false;
    QString m_remembered;
    QString m_presented;
    // The two as they were read: what the box shows is written from these, and what is trusted
    // is the second of them.
    QByteArray m_rememberedSha256;
    QByteArray m_presentedSha256;
    bool m_plainHttpThere = false;
    // A yes to the second of them was passed on, and nothing has come of it yet.
    bool m_saidYes = false;
    bool m_yesWasToAnother = false;
    bool m_yesWasOnWindowsWord = false;
    // What was said yes to last at each address that was typed, since the box was opened for
    // this KVM or filled anew with it: what is shown there at a later asking is held against
    // it. (A yes to plain HTTP is not among them: nothing is shown there that could change.)
    QHash<QString, SaidYes> m_saidYesAt;
    // The yes that is being handed on this moment (saysYesTo()).
    std::optional<Offer> m_passedOn;
    bool m_lookedAt = false;
    // Runs while the two are looked at, until they have been for a moment.
    QTimer m_rest;
    bool m_rested = false;
};

} // namespace qkvm::ui
