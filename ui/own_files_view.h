#pragma once

#include <optional>

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <qkvm/base/result.h>
#include <qkvm/core/exchange.h>
#include <qkvm/core/wall_exchange.h>

#include "file_door.h"

namespace qkvm::ui {

// What a file holds, laid out for the box that takes KVMs of it onto this PC's wall: one list,
// which is scrolled as one. First the lines that stand beside the devices (what the reader left
// out of the file, what the file holds that is not this PC's yet), each a row; then every device
// of the file, in the file's order, each a row.
//
// A device is one short row: its name and address (or, where its address cannot be shown,
// which device of the file it is), and the one thing that matters of it in a few words
// (`inShort`). Everything else that a person is to be shown of it
// (core::WallExchange::Offered: its verdict, every sentence that is said of it, the whole
// SHA-256 of the certificate its file names) is the row's too, and is on show once the row is
// opened (`setOpen`). What is the reason a device is not ticked to begin with is never only
// among what is opened: the short line says it. What is so of every device of the list alike
// is said once, above it (saidOfItsCounts), and on no row.
//
// A file may be made by anybody, and so may the lines beside its devices: however few or many
// they are, and however long, they stand folded into one row that says how many they are,
// until somebody opens them (`openNotes`). So a file cannot push its devices out of sight
// under what it had left out of it, not even in the smallest window, where one note of three
// lines would; and nothing is taken while lines are folded away that nobody opened
// (OwnFilesView::mayTake).
//
// The ticks are kept here, and nowhere on the screen: a list of two thousand devices is drawn
// only where it is seen, and a tick is what it was when its row comes back. So is whether a row
// is opened. A device that cannot be taken has no tick, and is given none by anything the
// screens can ask for.
//
// Made by OwnFilesView, and handed to the screens by it; on the interface thread.
class LaidOutList : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")

public:
    enum Role {
        // "beside": a line that stands beside the devices (TextRole). "fold": the row that
        // stands for the lines that are folded away (TextRole: how many they are, in words).
        // "device": a device.
        KindRole = Qt::UserRole + 1,
        TextRole,
        // Of a device. Its place in the file, counted from 1; its name and its address in one
        // piece, as the core hands them over; and where the core hands none (its address cannot
        // be shown), which device of the file it is, as the lines beside the devices count:
        // "Device 7 of the file".
        PlaceRole,
        NameAndAddressRole,
        // The one thing that matters of it, in a few words (inShort()).
        InShortRole,
        // It can be taken, and something about it wants a second look, which is why it was not
        // ticked to begin with: its short line says what, and is a question to whoever reads it.
        WantsALookRole,
        // What stands behind its row: in one sentence whether it can be taken, as what, or why
        // not; everything else that is said of it, each sentence by itself; and the SHA-256 of
        // the certificate its file names, in four lines of eight pairs, as the box that changes
        // a KVM shows one to be compared (empty: the file names none, or it cannot be taken).
        VerdictRole,
        SaysRole,
        FingerprintRole,
        CanBeTakenRole,
        TickedRole,
        // Its row is opened: what stands behind it is on show.
        OpenRole,
        HasTextRole,
    };

    explicit LaidOutList(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // What is listed from now on, in the place of what was. Ticked to begin with: every device
    // that can be taken and has nothing about it that wants a second look (tickedToBeginWith).
    // No row is opened, and the lines beside the devices are folded away into one row.
    void show(const QStringList& beside, const QList<core::WallExchange::Offered>& devices);
    void clear();
    // Ticks a device, by its place in the file, or takes its tick away. True: its tick is
    // another now. A place that is not listed has none, and neither has a device that cannot
    // be taken.
    bool setTicked(qsizetype place, bool ticked);
    // Opens the row of a device, by its place in the file, or closes it. True: it is another now.
    bool setOpen(qsizetype place, bool open);
    // Makes a row of every line beside the devices, in the place of the one that stood for them.
    // True: they were folded away until now.
    bool openNotes();
    // How many lines beside the devices are folded away at this moment.
    int foldedNotes() const { return static_cast<int>(m_beside.size() - m_notesShown); }
    // The places that are ticked, in the file's order; and how many there are.
    QList<qsizetype> tickedPlaces() const;
    int tickedCount() const { return m_ticked; }
    int deviceCount() const { return static_cast<int>(m_devices.size()); }
    bool hasOtherParts() const { return m_otherParts; }
    // Every line beside the devices, folded away or not.
    int besideCount() const { return static_cast<int>(m_beside.size()); }
    // How many devices can be taken, are not ticked, and have something about them that wants
    // a look; and how many cannot be taken at all.
    int toLookAtCount() const;
    int cannotBeTakenCount() const;
    // That, in words, for what stands above the list at all times: how many devices of the file
    // are in this list (the lines beside it number the file's devices, of which those that were
    // left out are not in it; how many the file has is not handed over by its reader), how many
    // are ticked, how many are not ticked and want a look, how many cannot be taken (these two
    // only where there are any); and that the file has no password for any of them, where that
    // is so of every one that can be taken.
    QString saidOfItsCounts() const;
    // Every form of that which may take the most room as it is drawn, whatever is ticked: for
    // the room that what stands above the list keeps for as long as the list is shown, which the
    // screens make as high as the highest of these is drawn at the box's width (so a tick that
    // makes what is said there shorter, or longer, moves nothing under it). Each form of how many
    // are ticked that this list can come to ("None", "1", and of the numbers from 2 on that it
    // can come to, the widest), with each of how many want a look (none, "1", and the widest
    // number there), and the rest as it is. A number is as wide as its digits are drawn one
    // beside the other (`digitWidths`: the ten, from 0 to 9; told nothing of them, the largest
    // number is taken). Of forms that differ by a number alone, the one with the widest number
    // needs the most lines: a wider word never lets a line hold more. The two parts are taken as
    // if each could be at its widest at once, which not every list allows: the room may have a
    // line that no count of the list needs. Of a list of no device, the one thing it says.
    QStringList saidOfItsCountsToMakeRoomFor(const QList<qreal>& digitWidths = {}) const;
    // What a device is called where its row is not there any more: as its row calls it (its name
    // and its address in one piece, or which device of the file it is); empty: no device of that
    // place is listed.
    QString nameAndAddressOf(qsizetype place) const;

    // Whether a device is ticked when it is first shown: it can be taken, and nothing about it
    // wants a second look. Not ticked, and ticked only by whoever wants it: one whose password
    // goes in the clear, one whose address is a name in its "xn--" form, and one whose address
    // is this PC itself or of the kind no router passes on. So what is taken by somebody who
    // reads nothing below the first screen is only what is known by a certificate or by
    // Windows' word, at an address somewhere else.
    static bool tickedToBeginWith(const core::WallExchange::Offered& device);
    // The one thing that matters of a device, in a few words, made of its plain values:
    // * of one that cannot be taken: that it cannot, and why;
    // * of one that can and is not ticked to begin with: every reason for that (its password
    //   goes in the clear; its address is a name in its "xn--" form, this PC itself, or of the
    //   kind no router passes on);
    // * of one that is ticked to begin with: what it will be known by (its certificate, with
    //   the first and the last of its SHA-256, the middle left out here and nowhere else; or
    //   Windows' word, and where the file would have had it known by what it shows first, that
    //   it is not);
    // * and of one that can be taken and comes without a password, that too, where
    //   `saysOfAPassword`: not where the file has no password for any device of the list, which
    //   is said once above it.
    static QString inShort(const core::WallExchange::Offered& device, bool saysOfAPassword = true);

private:
    struct Device {
        core::WallExchange::Offered offered;
        // What its row calls it.
        QString called;
        QString inShort;
        QString fingerprint;
        bool wantsALook = false;
        bool ticked = false;
        bool open = false;
    };

    // Where the devices begin: after the lines beside them where they are opened, and after the
    // row that stands for them where they are folded away.
    qsizetype firstDeviceRow() const { return m_notesShown + (m_fold ? 1 : 0); }
    // The device of a place, and where in `m_devices` it is; nothing: none is listed there.
    Device* deviceAt(qsizetype place, qsizetype* at = nullptr);

    QStringList m_beside;
    // How many of them are rows (none, or all once they are opened); and whether one row stands
    // for them, folded away.
    qsizetype m_notesShown = 0;
    bool m_fold = false;
    QList<Device> m_devices;
    // Where in `m_devices` the device of a place is.
    QHash<qsizetype, qsizetype> m_atPlace;
    int m_ticked = 0;
    // The file has no password for any device of the list that can be taken (and there is one).
    bool m_noPasswordForAny = false;
    bool m_otherParts = false;
};

// This PC's own wall as a file: what the two boxes of one window show and ask (SPEC.md section 4,
// M3c). "Export" writes the wall to a file, "Import" takes KVMs of a file onto it. The program
// itself makes the file and reads one (core::WallExchange, which this view is handed one of: one
// window's laying-out is not another's), where a server does both for a server's wall
// (files_view.h). There is no account here, and no password of one is asked for.
//
// Its manners are those of the server's boxes. Everything that takes time has a number, and an
// answer is taken only for what is waited for just now: what was asked before a box was closed,
// before the wall was left, or behind one of Windows' own boxes while something else happened,
// is nobody's when it comes. Nothing is asked from behind one of Windows' boxes. And closed, a
// box keeps nothing: reset() forgets what was made, what was chosen and what was laid out, here
// and in the core (WallExchange::forget), and waits for nothing that is under way.
//
// What "Cancel" does to work that is under way, and what it does not. reset() comes back at
// once, and from then on what that work comes to is nobody's: a file that was being made is let
// go unseen when it is made, and of a file that was being read nothing is laid out (the core is
// told to forget it, and keeps nothing of it). The work itself is NOT ended: the passphrase's
// work runs to its end (half a second or more: exchange_work.h), with nobody waiting for it.
// Until then the program's one place for a file is taken, and whoever asks for another file in
// that time is told that one is being worked on, which passes (`note`). (Ending it needs a word
// that the core's exchange::FileWork does not have: it stops its work only where it goes
// itself, and waits for it there.)
//
// EXPORT. The file is made first ("Make the file": the passphrase's work is done off the
// interface thread); then every line of what is NOT in it stands in the box (`notInIt`); and
// only with those lines on show (`besideOnShow`, which the screens say), and not in the first
// moment they are (`saveRestsMs`, `maySave`), nor in the first moment after the box changed its
// size or its place (`boxPlace`), does "Save" ask where the file is kept and write it, whole or
// not at all. The file's text is asked of what the core handed back at that
// moment, for the word that the lines were looked at, and at no other. The name it is kept
// under says what it is, whatever was chosen in Windows' box (namedAsWhatItIs, file_door.h):
// ".qkvm" for one that is sealed, ".json" for a plain one; and where that makes it another name
// than was chosen, a file (or a folder) that is there under it is not written over, since nobody
// was asked. The window's corner names the file that was written (`exported`).
//
// IMPORT. A file is chosen, and says of itself whether it is sealed; it is read ("Read the
// file"), and what it holds is laid out (`offered`), with nothing on the wall changed. What is
// so of the list as a whole stands above it at all times, in words (`counts`): how many of the
// file's devices are in the list, how many of them are ticked, how many are not ticked and want
// a look, how many cannot be taken. take() takes the devices that are ticked, and no other: the
// ticks are here, and the screens name no place. It is not to be had by the click that read the
// file: for the first moment the list is looked at (`takeRestsMs`, and from the beginning
// whenever it is looked at again) nothing is taken (`mayTake`). Nor while lines beside the
// devices are folded away that nobody opened (`foldedNotes`, openNotes(); the moment begins
// anew when they are). Nor while nothing is ticked; and where something comes to be ticked
// where nothing was, or a row is opened or closed, the moment begins anew too. (The button that
// takes stands in its place for as long as a list is shown, and takes no click while nothing is
// ticked: so it is never another button that comes to stand where it stood, nor it where
// another stood, and where it comes to take, the click that was on its way to it meanwhile
// takes nothing.) What is in the list takes no press either (a tick, a row's "Details", the row
// that opens the notes) until the list has stood as it is for its moment (`settled`), whoever
// asks: the screens, the keyboard, a reader of the screen; and the list's own moment begins anew
// where the keys scroll it (`rowsMoved`). The box itself has one size and one place for as long as
// its window keeps its own. Where it changes them (its window changed size, moved, or came to a
// screen of another scale: `boxPlace`, as the screens say it), every control of it stands
// somewhere else under a pointer that rests where it rested, and the moments of the list, of what
// takes and of "Save" begin anew. One laying-out, one taking: where every device that was ticked
// is on the wall the box closes (`taken`), and what its window shows under it takes no press for
// a moment (`heldAfterTaking`), since the click that follows the one on "Take" lands there. Where
// one is not on the wall the box stays and says what came of each (`tookSummary`, `notTaken`),
// with nothing left to take; and when that box is closed, by whatever way, what was taken is told
// in its window's corner (`taken` again).
//
// What is kept, and for how long. The passphrase is not kept here: it is handed to the core
// with the press it was typed for, which holds it for as long as that work takes. On the screen
// it stands in its line until its file was made or read (`fileWasMade`, `fileWasRead`), and no
// longer than the file it was typed for is the chosen one (`fileIsAnother`); the screens empty
// the line at each of these and when its box is closed, and the line goes out of sight in no
// other way. The bytes of a file that was chosen are kept until it was laid out, and no longer;
// and a file that is chosen and not laid out (nobody read it, or its passphrase was not the one)
// is kept no longer than what was laid out would be (WallExchange::keptForMs, counted from the
// choosing): then it is let go as a list is, with what was typed for it (`fileIsAnother`), and
// a reading of it that is under way is waited for no more. What was laid out is the core's to
// keep (the passwords of the file are there, and never here), for ten minutes at the most: when
// the core says that it keeps it no longer, the list goes, nothing of the file is kept, and a
// sentence says so and what is to be done (`note`, `keptNoLonger`). (The box on the screen does
// not move for either: its size and its place change with its window's, and with nothing else.)
//
// Made in C++, one for each window, and handed to its screens; on the interface thread.
class OwnFilesView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // Something is under way and has not ended: a file is being made or read, or somebody is
    // being asked where a file is kept or which is opened. Nothing else is asked meanwhile.
    Q_PROPERTY(bool busy READ isBusy NOTIFY changed)
    // What went wrong with what was asked last, as a sentence to show; empty: nothing.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // What is to be known and is nothing wrong, as a sentence to show: that another file is
    // being worked on just now, which passes; that what was read is kept no longer. Empty: nothing.
    Q_PROPERTY(QString note READ note NOTIFY changed)

    // The file is being made.
    Q_PROPERTY(bool making READ isMaking NOTIFY changed)
    // It was made, and is not kept anywhere yet: what is not in it is to be shown, and then it
    // can be saved. With a passphrase, and so with the passwords of the KVMs; or plain, with none.
    Q_PROPERTY(bool made READ isMade NOTIFY changed)
    Q_PROPERTY(bool madeEncrypted READ isMadeEncrypted NOTIFY changed)
    // Every line of what is not in the file that was made, or is in it and may not be read by a
    // QKVM server: as the core hands them over, none left out.
    Q_PROPERTY(QStringList notInIt READ notInIt NOTIFY changed)
    // The screens say whether those lines are on show: the box is open on them. Not, until they
    // say so; and without that nothing is saved.
    Q_PROPERTY(bool besideOnShow READ isBesideOnShow WRITE setBesideOnShow NOTIFY changed)
    // The lines have been on show for their moment (since they came on show, and since the box
    // last changed its size or its place): only now does saveFile() ask where the file is kept.
    Q_PROPERTY(bool maySave READ maySave NOTIFY changed)

    // The screens say where the box that is open stands on the screen, and how large it is: in
    // the screen's own pixels, with the place of its window on the screen counted in. The first
    // place a box comes to once it is opened is no change; and every place after it that is
    // another is: its window changed size, moved, or came to a screen of another scale, and every
    // control of the box stands somewhere else under a pointer that rests where it rested. Then
    // the moments of the list (`settled`), of what takes (`mayTake`) and of "Save" (`maySave`)
    // begin anew. Closed (reset()), a box has no place.
    Q_PROPERTY(QRectF boxPlace READ boxPlace WRITE setBoxPlace NOTIFY changed)

    // The file that was chosen to be read, by its name; empty: none.
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    // It says of itself that it is sealed: there is a passphrase to ask for.
    Q_PROPERTY(bool sealed READ isSealed NOTIFY changed)
    // It is being read.
    Q_PROPERTY(bool reading READ isReading NOTIFY changed)
    // It was read, and what it holds is laid out: `offered` lists it.
    Q_PROPERTY(bool laidOut READ isLaidOut NOTIFY changed)
    Q_PROPERTY(qkvm::ui::LaidOutList* offered READ offered CONSTANT)
    // How many devices are ticked; and what is so of the list as a whole, in words, for what
    // stands above it (LaidOutList::saidOfItsCounts).
    Q_PROPERTY(int tickedCount READ tickedCount NOTIFY tickedChanged)
    Q_PROPERTY(bool hasOtherParts READ hasOtherParts NOTIFY changed)
    Q_PROPERTY(QString counts READ counts NOTIFY tickedChanged)
    // Every form of that which may take the most room as it is drawn, for the room it keeps there
    // (LaidOutList::saidOfItsCountsToMakeRoomFor): the screens draw each, unseen, at the width of
    // what stands above the list, and keep the room of the highest. Empty while no list is shown.
    // Its numbers are as wide as the screens say each digit is drawn there (`digitWidths`, the ten
    // from 0 to 9, in the font the counts are drawn in).
    Q_PROPERTY(QStringList countsToMakeRoomFor READ countsToMakeRoomFor NOTIFY countsRoomChanged)
    Q_PROPERTY(QList<qreal> digitWidths READ digitWidths WRITE setDigitWidths NOTIFY countsRoomChanged)
    // How many lines beside the devices are folded away: nothing is taken until somebody has
    // opened them (openNotes()).
    Q_PROPERTY(int foldedNotes READ foldedNotes NOTIFY changed)
    // The screens say whether the list is looked at: the box is open on it, in a window that is
    // in sight and is the active one. Not, until they say so.
    Q_PROPERTY(bool lookedAt READ isLookedAt WRITE setLookedAt NOTIFY changed)
    // Something is ticked, the list has been looked at for its moment (since it came, since it
    // was looked at again, since its notes were opened, since a row of it was opened or closed,
    // since something came to be ticked where nothing was, and since the box last changed its
    // size or its place), and no line beside it is folded away: only now does take() take.
    Q_PROPERTY(bool mayTake READ mayTake NOTIFY changed)
    // The list has stood as it is, looked at, for its moment: since it came (where the line for
    // the passphrase and the button that reads stood a moment before), since it was looked at
    // again, since its notes were opened, since a row of it was opened or closed by its "Details"
    // (every row under it moves by what it grew or shrank), since the keys scrolled it
    // (rowsMoved()), and since the box last changed its size or its place (`boxPlace`). Until then
    // setTicked(), setOpen() and openNotes() do nothing, whoever asks, and the screens give the
    // list's own buttons (its ticks, its rows' "Details", the row that opens its notes) no click.
    // (A tick does not begin this anew: it moves nothing.)
    Q_PROPERTY(bool settled READ isSettled NOTIFY changed)
    // A taking is over, and not every device that was ticked is on the wall: how many are, as a
    // sentence, and each that is not, {device, why}: what it is called (its name and address in
    // one piece) and the sentence it was not taken with. Nothing is left to take.
    Q_PROPERTY(bool took READ hasTaken NOTIFY changed)
    Q_PROPERTY(QString tookSummary READ tookSummary NOTIFY changed)
    Q_PROPERTY(QVariantList notTaken READ notTaken NOTIFY changed)
    // What was laid out, or a file that was chosen and not laid out, went by itself, its time
    // being over, and no other file was chosen since: `note` says so. The screens do not close
    // the box for a click beside it: the sentence is there to be read.
    Q_PROPERTY(bool keptNoLonger READ isKeptNoLonger NOTIFY changed)
    // A taking took every device that was ticked a moment ago, and its box closed: what its window
    // shows where the box stood is under the pointer that pressed "Take" (a click that follows,
    // the second of a double click, lands there), and takes no press, for as long as the list
    // took none after it came (`takeRestsMs`). The screens lay something over the window for that
    // moment that takes every press. Whatever becomes of the box meanwhile, it runs to its end.
    Q_PROPERTY(bool heldAfterTaking READ isHeldAfterTaking NOTIFY changed)

public:
    // For how long take() takes nothing after the list has come, in milliseconds, as the box
    // that changes a KVM holds back a yes to a certificate (EditKvmView::trustRestsMs, and as
    // long): the list comes when the file was read, which for a plain file is a moment after
    // the press that asked for it, and the second click of a double click is on the screen
    // then. It runs only while the list is looked at, and from the beginning whenever it is
    // looked at again.
    static constexpr int takeRestsMs = 1000;
    // And for how long saveFile() asks nothing after the lines of what is not in the file have
    // come on show: as long, and for the same reason. The lines come when the file was made,
    // which for a plain one is a moment after the press that asked for it; the second click of
    // a double click would open Windows' own box over lines that nobody has read.
    static constexpr int saveRestsMs = 1000;
    // No more of a chosen file than this is read from the disk, in bytes: the most signs a file
    // of QKVM's has (core::exchange::largestSealed), each in as many bytes as a sign can take.
    // The reader has the last word on what is too large.
    static constexpr qsizetype largestFileBytes = core::exchange::largestSealed * 4;

    explicit OwnFilesView(QObject* parent = nullptr);
    ~OwnFilesView() override;

    // Who makes and reads the files: one of its own for each view (OwnWall::serve). It may go
    // before this does, and then nothing is asked of it. Without one, nothing is made or read.
    void use(core::WallExchange* exchange);
    void useAppLock(class AppLock& lock);
    // How a file is chosen on this PC (file_door.h).
    void setDoor(FileDoor door) { m_door = std::move(door); }
    // The two moments of a test's choosing, in milliseconds: long where a test looks at what is
    // not to be had yet, short where it waits for what is. (The program has them as they are.)
    void restFor(int takeMs, int saveMs);

    bool isBusy() const { return m_waits != Waits::ForNothing; }
    // Its window is not to be closed just now: something is under way, or one of Windows' own
    // boxes stands over it, whatever has become of the question that was put in it meanwhile.
    bool holdsItsWindow() const { return isBusy() || m_atTheDoor; }
    QString problem() const { return m_problem; }
    QString note() const { return m_note; }
    bool isMaking() const { return m_waits == Waits::ForTheFile; }
    bool isMade() const { return m_written.has_value(); }
    bool isMadeEncrypted() const { return m_written.has_value() && m_madeEncrypted; }
    QStringList notInIt() const { return m_written ? m_written->notInIt() : QStringList(); }
    bool isBesideOnShow() const { return m_besideOnShow; }
    void setBesideOnShow(bool onShow);
    bool maySave() const { return m_written.has_value() && m_besideOnShow && m_besideRested; }
    QRectF boxPlace() const { return m_boxPlace; }
    void setBoxPlace(const QRectF& place);
    QString fileName() const { return m_fileName; }
    bool isSealed() const { return m_sealed; }
    bool isReading() const { return m_waits == Waits::ForTheReading; }
    bool isLaidOut() const { return m_laidOut != 0; }
    LaidOutList* offered() { return &m_offered; }
    int tickedCount() const { return m_offered.tickedCount(); }
    bool hasOtherParts() const { return m_offered.hasOtherParts(); }
    QString counts() const { return m_laidOut != 0 ? m_offered.saidOfItsCounts() : QString(); }
    QStringList countsToMakeRoomFor() const
    {
        return m_laidOut != 0 ? m_offered.saidOfItsCountsToMakeRoomFor(m_digitWidths) : QStringList();
    }
    QList<qreal> digitWidths() const { return m_digitWidths; }
    void setDigitWidths(const QList<qreal>& widths);
    int foldedNotes() const { return m_offered.foldedNotes(); }
    bool isLookedAt() const { return m_lookedAt; }
    void setLookedAt(bool lookedAt);
    bool mayTake() const
    {
        return m_laidOut != 0 && m_rested && m_offered.foldedNotes() == 0 && m_offered.tickedCount() > 0;
    }
    bool isSettled() const { return m_laidOut != 0 && m_settled; }
    bool hasTaken() const { return m_took; }
    QString tookSummary() const { return m_tookSummary; }
    QVariantList notTaken() const { return m_notTaken; }
    bool isKeptNoLonger() const { return m_keptNoLonger; }
    bool isHeldAfterTaking() const { return m_heldAfterTaking; }

    // A box is opened, or closed, or the wall is left: nothing of before is shown or kept, here
    // or in the core, and nothing that is under way is waited for any more (what it comes to is
    // nobody's; the work itself runs to its end, as is said at the class). What came of a taking,
    // where that was shown, is told once more as it goes (`taken`).
    Q_INVOKABLE void reset();

    // "Export". Makes the file: sealed with the passphrase, and then with the passwords of the
    // KVMs; or plain where it is empty, and then with none. Nothing while something is under
    // way, and nothing once a file was made (that one is saved, or the box is closed).
    Q_INVOKABLE void makeFile(const QString& passphrase);
    Q_INVOKABLE void makeSelectedFile(const QString& passphrase, bool devices, bool actions, bool settings);
    Q_INVOKABLE QString actionText(int place) const;
    // Asks where the file that was made is kept, and writes it there. Nothing unless one was
    // made and the lines beside it have been on show for their moment (maySave).
    Q_INVOKABLE void saveFile();

    // "Import", step by step: a file is chosen, read and laid out, and its ticked KVMs are taken.
    Q_INVOKABLE void chooseFile();
    Q_INVOKABLE void readFile(const QString& passphrase);
    // Ticks a device of what is laid out, by its place in the file, or takes its tick away.
    // One that cannot be taken is not ticked. The first tick where none was begins the moment
    // after which something is taken anew. (This, and the two that follow, do nothing before
    // the list has stood for its moment: `settled`.)
    Q_INVOKABLE void setTicked(int place, bool ticked);
    // Opens the row of a device, by its place in the file: what stands behind it is on show.
    // Or closes it. The rows under it move by what it grew or shrank: the list's moment, and
    // with it the moment after which something is taken, begins anew.
    Q_INVOKABLE void setOpen(int place, bool open);
    // Makes every line beside the devices a row of the list. The moment after which something
    // is taken begins anew, and the list's own: what has just come on show is not passed over by
    // a click, and every row has moved.
    Q_INVOKABLE void openNotes();
    // The screens say that the keys scrolled the list: a step, a page, or to an end, at once,
    // and under a pointer that rests where it rested are other rows. The list's own moment
    // begins anew (`settled`); what takes stands where it stood, outside the list, and is not
    // held for this (where its own moment was still running, that ends with the list's new one).
    // Nothing where no list is shown.
    Q_INVOKABLE void rowsMoved();
    // Takes the devices that are ticked onto this PC's wall. Nothing while none is ticked, and
    // nothing before the list was looked at for its moment, with no line folded away (mayTake).
    Q_INVOKABLE void take();

    // What whoever keeps the windows says: the boxes are to be closed, from outside (their window
    // is out of sight). The screens close them and empty their lines; what they show and wait
    // for is gone there and then, whether or not there are screens.
    void close();

    // How many bytes of a file that was chosen are held here at this moment: all of it until it
    // was laid out, and none from then on, nor once its box is closed.
    qsizetype bytesHeld() const { return m_file.size(); }
    // What the box says once what was laid out is kept no longer, of a time it was kept for: that
    // it is kept so long, that the time is over, and that the file is to be chosen again.
    static QString saidOfWhatIsKeptNoLonger(int keptForMs);

signals:
    void changed();
    // The ticks are others, or the list is: how many are ticked, and what is said of its counts.
    void tickedChanged();
    // What the counts can come to is another: another list, or none; or its digits are drawn
    // another way.
    void countsRoomChanged();
    // A file was made, and a file was read: what was typed as its passphrase is needed no
    // more, and goes out of its line.
    void fileWasMade();
    void fileWasRead();
    // The file a passphrase may have been typed for is not the chosen one any more: another
    // was chosen, it was refused as one that no other try will read, its list went, or its box
    // was closed. What was typed for it goes out of its line.
    void fileIsAnother();
    // For the screens: the boxes are to be closed, with nothing kept of what was typed.
    void closeAsked();
    // The file was kept: its box closes, and this is told in its window's corner.
    void exported(const QString& said);
    // A taking is over, and its box says nothing of it: every device that was ticked is on the
    // wall and the box closes (`all`); or the box was closed in the middle of the taking, or
    // closed over what came of it, and not every one is. Told in the window's corner either way:
    // as something that was done, or as something to be looked at.
    void taken(const QString& said, bool all);

private:
    enum class Waits { ForNothing, ForTheFile, ForAPlace, ForAChoice, ForTheReading, ForTheTaking, ForPassword };
    void makeAuthorized(const QString& passphrase);
    bool mayChange() const;
    AppLock* m_lock = nullptr;

    // Waits for this from now on, under a number of its own.
    quint64 waitFor(Waits what);
    // Whether this is the answer that is waited for; and if so, nothing is waited for any more.
    bool isAnswer(quint64 turn, Waits to);
    void fail(const QString& problem);
    // Why not, as the box says it: what passes (another file is being worked on) as a note,
    // and anything else as what went wrong.
    void say(const base::Error& why);
    void sayNothing();
    void forgetWhatWasMade();
    // The file that was chosen, what was laid out of it and what came of a taking: nothing of
    // them is shown or kept from here on, and the core forgets what it laid out.
    void forgetTheFile();
    void wasWritten(quint64 turn, const base::Result<core::exchange::Written>& written);
    void wasLaidOut(quint64 turn, const base::Result<core::WallExchange::LaidOut>& laidOut);
    // The core keeps what it laid out no longer.
    void theListsTimeIsOver();
    // A file that was chosen and not laid out has been held for as long as a list is kept.
    void theChosenFilesTimeIsOver();
    // What either says once it went: that what is read is kept so long, and that it is over.
    void sayItIsKeptNoLonger();
    // The moment begins anew: for the list and for what takes (restAnew), or for what takes
    // alone (restTakingAnew).
    void restAnew();
    void restTakingAnew();
    void restBesideAnew();

    QPointer<core::WallExchange> m_exchange;
    FileDoor m_door;
    Waits m_waits = Waits::ForNothing;
    quint64 m_turn = 0;
    bool m_atTheDoor = false;
    QString m_problem;
    QString m_note;
    // The file as it was made, with what is not in it; and whether it was made with a passphrase.
    std::optional<core::exchange::Written> m_written;
    core::WallExchange::Parts m_parts;
    bool m_madeEncrypted = false;
    bool m_besideOnShow = false;
    // Runs while the lines of what is not in the file are on show, until they have been for a
    // moment.
    QTimer m_besideRest;
    bool m_besideRested = false;
    int m_saveRestsMs = saveRestsMs;
    // The file that was chosen: its bytes until it was laid out, and its name; and what runs from
    // its choosing for as long as a list is kept.
    QByteArray m_file;
    QString m_fileName;
    bool m_sealed = false;
    QTimer m_chosenFor;
    // Which laying-out of the core's is shown; 0: none.
    quint64 m_laidOut = 0;
    LaidOutList m_offered;
    bool m_lookedAt = false;
    // Runs while the list is looked at, until it has been for a moment: then what takes has
    // rested, and so has the list.
    QTimer m_rest;
    bool m_rested = false;
    bool m_settled = false;
    int m_takeRestsMs = takeRestsMs;
    bool m_took = false;
    QString m_tookSummary;
    QVariantList m_notTaken;
    bool m_keptNoLonger = false;
    // Where the box that is open stands; nothing: it is closed, or has been told no place yet.
    QRectF m_boxPlace;
    QList<qreal> m_digitWidths;
    // Runs from a taking that took every device that was ticked, for a moment: what was under
    // the box takes no press until then.
    QTimer m_afterTaking;
    bool m_heldAfterTaking = false;
};

} // namespace qkvm::ui
