#pragma once

#include <memory>
#include <optional>
#include <unordered_map>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_exchange.h>

#include "add_kvm_view.h"
#include "edit_kvm_view.h"
#include "own_files_view.h"

namespace qkvm::ui {

class AppLock;

// This PC's own wall as the program holds it (SPEC.md section 4, M3a and M3c): the KVMs that were
// added here, the adding of one, and the changing of one, which a window's boxes ask for and are
// told of; and the wall as a file, which a window's boxes write and take KVMs from. Whatever
// shows a wall asks the wall that is in here (core::LocalWall, a core::WallSource).
//
// On the interface thread. The settings and the secrets outlive it.
class OwnWall : public QObject {
    Q_OBJECT

public:
    OwnWall(core::Settings& settings, core::SecretStore& secrets, QObject* parent = nullptr);
    // With the times of a test's choosing for what speaks to the KVMs.
    OwnWall(core::Settings& settings, core::SecretStore& secrets, const core::KvmAdding::Pace& pace,
            QObject* parent = nullptr);

    core::LocalWall& wall() { return m_wall; }
    void useAppLock(AppLock& lock);

    // The box of one window that adds a KVM: what it asks for is done, and it is told what came
    // of it, and no other box is. One KVM is added at a time, whichever window asks: a second box
    // is told to wait. Only the box whose KVM is being asked can give that up; and a box that
    // goes while its KVM is being asked has given it up. The view may go before this does.
    void serve(AddKvmView& view);
    // The box of one window that changes a KVM of the wall: what it asks for is done, and it is
    // told what came of it, and no other box is. One KVM is changed at a time, and one
    // certificate is looked at at a time, whichever window asks: a second box is told to wait
    // (core::KvmEditing and core::KvmCertificateReview say so themselves). A password that was
    // typed is passed on to the one change it was typed for, and kept nowhere.
    //
    // What a box saves, it saves over the KVM as the box was filled with it: this keeps that for
    // each box (the KVM as the wall had it then, and how often its secret had been written), and
    // hands it on with the change. The screens are never handed it, and say nothing of it. A KVM
    // that another window changed since is not saved over: the box is told so, and is filled
    // anew with the KVM as it is. So is a box that stays open after a saving: where the KVM is
    // known by another certificate than the box showed, or is spoken to in the clear now and was
    // not, the box does not close, and shows what it is known by now.
    //
    // A saving that is refused over what is shown at a new address hands the box the two
    // (core::KvmEditing, step 4), and the box's yes to the second comes back with what was typed.
    // So does one that is refused because that address is plain HTTP and the KVM is spoken to
    // over HTTPS: the box is handed what is given up there. And so does a look at a
    // certificate, and a yes that was to one the KVM shows no longer. A look, and the yes to
    // what it showed, are asked over the KVM as the box was filled with it, as a saving is: of
    // that KVM and of no other, whatever the wish names, and not once it is another.
    //
    // A yes travels on a signal, which the screens can send as they can call a function. It is
    // one only for what this keeps as offered to that box (as it keeps what the box was filled
    // with): the very thing, for the KVM and at the address, that it last told this box to
    // show. What is offered is what is shown: this keeps it only where the box shows it once
    // it was told (EditKvmView::offers), and takes a yes only where the box says, with the
    // wish that carries it, that this very thing is what its button was pressed on, looked at
    // for its moment (EditKvmView::saysYesTo); and once. Anything else is no yes: a saving
    // goes on as one without, and nothing is trusted.
    //
    // Only the box whose change or whose look is under way can give that up. With its "Cancel"
    // the one that does it is told to give up (core::KvmEditing::giveUp), and the box is told
    // what came of it: that it was given up, or, where it was done in that very turn, that it
    // was done. A box that is closed or goes meanwhile has given it up too, and hears nothing:
    // the one that does it is ended there and then, and a new one is there. From then on nothing
    // is changed and nothing is trusted, behind nobody's back, whatever the KVM answers
    // afterwards, and the next box does not wait for it. What had been done by then (the wall
    // took it in that very turn) is said to the box as it closes, for its window to tell: the
    // one that did it would tell nobody. (Taking a KVM off the wall is done at once, and is not
    // given up.) The view may go before this does.
    //
    // A box that is open follows the wall: where its KVM is renamed elsewhere, its title says what
    // the KVM is called now; and where it is taken off elsewhere, the box is told so
    // (EditKvmView::wentAway), and stays, saying so, whatever it shows. (A box that asks something
    // hears what came of that first.)
    void serve(EditKvmView& view);
    // Opens a window's box for a KVM of the wall, filled with the KVM as it is on the wall now;
    // `withReview`: for its certificate alone, which is read at once (only for a KVM that is
    // spoken to over HTTPS: another has none, and gets the box as it is). A machine that is not
    // on the wall opens nothing; nor is a box opened anew while something it asked is under way.
    void openFor(EditKvmView& view, const QString& machine, bool withReview);
    // The boxes of one window for this PC's wall as a file ("Export" and "Import"): the view is
    // handed one that makes and reads files for it (core::WallExchange), of its own. What one
    // window laid out of a file is not another's to take, and is forgotten with its window.
    // The view may go before this does.
    void serve(OwnFilesView& view);
    // The cost a file is sealed with, of a test's choosing: for the views that are served from
    // here on.
    void sealFilesWith(core::exchange::Sealing sealing) { m_sealing = std::move(sealing); }
    // At the program's end (main.cpp, at aboutToQuit, before the controllers' goodbyes are waited for): the
    // adding and the change under way are given up, and every driver of theirs and of the wall let go of, a turn
    // of the loop later each (whoever ends the program sends the deferred deletions before it waits), none made
    // anew from then on (core::LocalWall::letGoOfEveryDriver). So a server's controller is handed back every
    // session of this PC's while the loop still runs: the wall's driver's and a box's sign-in.
    void letGoOfEverything();

    // What the box says of a KVM that was added, as the page says it: what it is called, what
    // it says it is, and that it is spoken to in the clear where it is. `called`: as a sentence
    // calls it (calledInASentence), and never its name as it is.
    static QString saidOf(const core::KvmAdding::Added& added, const QString& called);
    // What a sentence of the screens calls a KVM of the wall, by the wall's name for it: as the
    // wall's own sentences call one (core::LocalWall::calledInASentence: its name set off so
    // that it cannot read as the sentence's own words, or where it is). The one way the screens
    // have a KVM's name for a sentence: a name is free text, typed or from a file, and "this
    // KVM. It is safe to trust it" is not to read as the program's words. For one that is not
    // on the wall (any more): `otherwise`, which is what its card called it, set off the same
    // way; and "this KVM" where that shows nothing.
    QString calledInASentence(const QString& machine, const QString& otherwise) const;
    // What the first window says where the file of what the program remembers could not be read
    // when it started: the list of this PC's own KVMs was in it. The file is put aside here, and
    // what is said is what came of that. Nothing where the file was read, or there was none.
    static QString saidOfSettingsThatCouldNotBeRead(core::Settings& settings);

private:
    bool mayChange() const;
    AppLock* m_lock = nullptr;
    QList<QPointer<AddKvmView>> m_addBoxes;
    // What a box that changes a KVM asked of one of the two that do it, from when it was asked
    // until that box has been told: whose wish is under way, and which boxes are still to be
    // told that they wait for it. (Compared, and never looked into: a box may be gone.)
    struct Asked {
        const QObject* underWayFor = nullptr;
        QList<QPointer<EditKvmView>> toldToWait;

        // A box asks: its wish is under way where none was, and otherwise it is told to wait.
        // True: it is this box's from now on.
        bool begin(bool isUnderWay, EditKvmView& box);
        // The answer for a box has come. False: that box gave it up meanwhile, or is gone, and
        // the answer is nobody's.
        bool end(bool wasThisOnes, const QPointer<EditKvmView>& box);
        // A box waits for nothing any more. True: its own wish was under way, and is to be ended.
        bool letGo(const QObject* box);
    };

    // What a box was last told to show for a yes, and shows: in the words the box has for it.
    using Offered = EditKvmView::Offer;

    void giveUpFor(const QObject* box);
    // What a box that changes a KVM had asked is given up, and the box is told what came of it;
    // or it is ended there and then, for a box that hears nothing any more: see
    // serve(EditKvmView&).
    void giveUpChangingFor(const QObject* box);
    void endChangingFor(const QObject* box);
    // A box is being closed over what it asked: what of that was done already is said to it.
    void sayWhatWasDoneFor(EditKvmView& box);
    // The two certificates of a KVM, as a reading or a refused yes hands them over, are a
    // box's to show from here on, and the second is offered to it for a yes.
    void showTheTwoTo(EditKvmView& box, const QString& machine, const core::KvmCertificateReview::Shown& shown);
    // A box was told to show this for a yes: kept as offered to it where the box shows it
    // now, and otherwise nothing is.
    void keepAsOffered(const EditKvmView& box, const Offered& what);
    // What is offered to a box for a yes, taken out of what is kept (it is offered once);
    // nothing, unless the box says that this is what the yes it is passing on was said to.
    std::optional<Offered> takeOfferedTo(const EditKvmView& box);
    // The KVM a box was filled with, as it was then, where a wish of that box names that KVM;
    // nothing: it names another, or the box was filled with none.
    std::optional<core::KvmEditing::Seen> filledWith(const QObject* box, const QString& machine) const;
    // A box asked over a KVM that is not what it was filled with any more ("conflict"): it is
    // filled anew with the KVM as it is, told why, and asks over that from now on. False: it
    // was refused for another reason, or the KVM is not on the wall, and nothing was told.
    bool fillAnewOver(EditKvmView& box, const QString& machine, const base::Error& why);
    // A KVM of the wall as it is now; nothing: it is not on the wall. And as a box shows one.
    std::optional<core::KvmEditing::Seen> seenNow(const QString& machine) const;
    static EditKvmView::Shown shownOf(const core::OwnKvm& kvm);
    // The wall changed: each box that is open for one of its KVMs is told what became of it.
    void followTheWallInTheBoxes();

    core::LocalWall m_wall;
    core::KvmAdding m_adding;
    // The box whose KVM is being asked; nothing: none is. (Compared, and never looked into: the
    // box may be gone.)
    const QObject* m_asking = nullptr;
    // The one that changes a KVM, and the one that reads a certificate and has it trusted: one of
    // each at any time. What a box gives up and waits to hear of, each gives up itself. What a
    // box that is closed or gone had asked is ended with the one that did it (which changes
    // nothing and tells nobody: kvm_editing.h), and a new one is there for the next box: that
    // box does not wait a turn of the loop for the old one to say that it is over, and a box
    // that was told to wait for it is not told "busy" of what nobody waits for any more.
    core::KvmEditing::Pace m_pace;
    std::unique_ptr<core::KvmEditing> m_editing;
    std::unique_ptr<core::KvmCertificateReview> m_review;
    Asked m_changing;
    Asked m_looking;
    // What each box was filled with: see serve(EditKvmView&). (By the box, which is compared and
    // never looked into: it may be gone.)
    QHash<const QObject*, core::KvmEditing::Seen> m_seen;
    // And what each was last told to show for a yes, until a yes was taken to it, the box was
    // told something else, or the box was filled anew, closed or gone.
    QHash<const QObject*, Offered> m_offered;
    // The boxes that change a KVM, of every window, for as long as each is there.
    QList<QPointer<EditKvmView>> m_editBoxes;
    // What makes and reads files of the wall, one for each window's boxes: by the view, which is
    // compared and never looked into (it may be gone). Each goes with its view, and all of them
    // before the wall does (they are named last).
    core::exchange::Sealing m_sealing;
    std::unordered_map<const QObject*, std::unique_ptr<core::WallExchange>> m_exchanges;
};

} // namespace qkvm::ui
