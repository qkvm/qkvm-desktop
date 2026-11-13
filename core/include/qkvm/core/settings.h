#pragma once

#include <optional>

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <qkvm/base/result.h>

namespace qkvm::core {

// Where the program's window was: its place on the desktop while it was of its own size, and
// whether it filled the desktop or a screen instead. (`maximized` also under a window that filled
// a screen: it is what the window goes back to from there.)
struct WindowPlace {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
    bool fullScreen = false;

    bool operator==(const WindowPlace&) const = default;
};

// One of the program's windows, as it was: where, and how its wall was shown in it.
struct WallWindow {
    // "pc", "connect", or a canonical server address. Empty uses the last global choice.
    // Display-link keys and credentials are never kept here.
    QString source;
    // Where it was when it, or the program, was last closed; nothing: it has not been anywhere yet.
    std::optional<WindowPlace> place;
    // Its wall: one group or all (nothing), the order of the cards (nothing, "name", "group" or
    // "state"), and how many columns (0: as many as fit).
    QString group;
    QString order;
    int columns = 0;
    // While its wall is only shown (display mode), the groups take turns.
    bool rotates = false;

    bool operator==(const WallWindow&) const = default;
};

struct Workspace {
    QString id;
    QString name;
    QList<WallWindow> windows;
    bool operator==(const Workspace&) const = default;
};

// One KVM of this PC's own wall, as the program knows it (SPEC.md section 4, M3a). Nothing of it
// is secret: its password is kept elsewhere (secrets.h), by its `id`.
struct OwnKvm {
    // The program's own name for it, made when it is added: eight of 0-9 and a-f. It never changes,
    // whatever else of the KVM does.
    QString id;
    QString name;
    // Where it is: "https://host", "http://host:port". Nothing else is in it.
    QString address;
    QString user = QStringLiteral("admin");
    QString group;
    // Which family of KVM it is, by the name a wall's file gives each (exchange.h): "kvmd" (PiKVM,
    // GL.iNet Comet), "jetkvm", or "redfish" (a server's management controller, M7), the families this
    // program speaks to by itself (namesAFamily).
    // The asking of what answers at its address takes its family and no other (local_wall.h). In the
    // settings' file it is so only where the file says a family the program speaks to in so many
    // words: a KVM that says none is none this program wrote, and is not read (there is no older
    // shape of the file to read).
    QString kind = QStringLiteral("kvmd");
    static bool namesAFamily(const QString& kind);
    // Over HTTPS: whether it is known by a certificate, and that certificate's SHA-256. One that
    // is known by none has to be trusted by Windows for its name.
    bool knownByCertificate = false;
    QByteArray certificate;
    // Whether what answers at its address was asked what it is, with nothing in the question that
    // says who asks, and said that it is a KVM of this family (kvm_probe.h). A KVM that somebody
    // added in the program was: the box that adds one asks before any password goes. One that was
    // taken from a file was not, and no password goes to it before the wall has asked
    // (local_wall.h). It is the wall's own to say: whatever stands here in a KVM that is handed
    // to the wall is not looked at, and the wall says "not asked" of every KVM it takes from a
    // file. (So one that is made here is as a box of the program adds one.) In the settings'
    // file it is so only where the file says so in so many words ("asked": true): anything else
    // there, and nothing, is "not asked yet", which costs one asking and sends nothing that
    // says who asks.
    //
    // What stands here is what is KEPT of it, from one run of the program to the next. The wall
    // keeps it only of a KVM that is spoken to over HTTPS, where a certificate says each time
    // who answers; of one over plain HTTP it never says so here, and asks once in every run
    // (local_wall.h says why). Whether a KVM was asked in this run is the wall's to answer
    // (LocalWall::wasAskedWhatItIs). And since the wall records it by itself, it is no change
    // that somebody made: whoever asks whether a KVM "is still the one" somebody saw asks
    // without it (LocalWall::isStillTheOne).
    bool askedWhatItIs = true;
    // The number it gives for itself, as its driver gave it (a JetKVM's: "jetkvm:" and a digest of its
    // own, JetKvmDevice::Identity; the web's `ident`): what tells it from another whatever its address
    // (LocalWall::twinOf). Empty: it gave none fit to be one, or was not asked. Kept as given, and in the
    // settings' file only where it is one a driver gives (exchange::isADevicesOwnNumber).
    QString number;
    // Somebody said it is to have a card of its own beside the one whose number it gives (the web's
    // keep_both): the number makes no twin of it, nor of another with it. A certificate still does.
    bool keptBesideItsTwin = false;
    // Of a server's management controller ("redfish"): the mark the server behind it is kept by, the numbers its
    // computer and its controller gave for themselves (RedfishDevice::Where::machine, redfish::marksOf; the web's
    // `machine`). Its driver asks which server answers by it before anything is sent there, the password included
    // where the controller's number is in it. Empty: none kept yet, and the first server that answers is taken
    // (trust on first use, as a certificate is); the wall notes what its driver took (LocalWall), and a change keeps
    // it unless a password was typed for it (LocalWall::change). Of a KVM, empty. In the settings' file as a driver
    // gives one (exchange::isADevicesOwnNumber): one that stands there in any other shape is no "none", and the
    // entry is one this build does not read, kept as it stands.
    QString machine;
    // Its screen watches (screen_watch.h; the web's `no_signal_minutes`, `unchanged_minutes`): an alert where its
    // computer has shown no picture for so many minutes, or its screen has not changed; 0 (the most 1440): not watched.
    // In the settings' file only where one is set.
    int noSignalMinutes = 0;
    int unchangedMinutes = 0;

    bool operator==(const OwnKvm&) const = default;
};

// One saved action of this PC's own wall (SPEC.md section 4, M3a): something that was saved once
// to be done to any KVM of that wall, keys pressed together or a text that is typed. Nothing of it
// is secret: the text an action types is kept elsewhere (secrets.h, by the action's `id`:
// own_actions.h says in what form), and never here.
struct OwnAction {
    // The program's own name for it, made when it is added: eight of 0-9 and a-f. It never changes,
    // whatever else of the action does.
    QString id;
    QString name;
    // It types its text; otherwise it presses `keys`.
    bool types = false;
    // Pressed together, as "ControlLeft,AltLeft,Delete". None of one that types.
    QString keys;
    // Enter after its text. Never of one that presses.
    bool enter = false;
    // It is asked about before it runs. (The asking is the screen's: a run is a run.)
    bool danger = false;
    // How often it was edited. Whoever runs it, changes it or removes it says which of its states
    // they looked at, and is refused where it is in another by now.
    int changed = 0;
    // Of one that types: which writing of its text is the one it has, as the secret store holds
    // texts (own_actions.h): the place that writing is kept in ("a" or "b"), a colon, and the
    // writing's own mark, sixteen of 0-9 and a-f, which is made anew each time a text is saved.
    // Nothing of the text is in it. What the store holds under this action's id in another form,
    // for another action or of another writing is not its text. Empty: no text is kept for it,
    // and it says so when it is run.
    QString writing;

    bool operator==(const OwnAction&) const = default;
};

// What the program remembers from one run to the next. Nothing of it is secret: secrets are kept
// elsewhere (secrets.h), and nothing here says what one is.
struct MachineReference {
    QString scope; // "pc", or the hash of a server address and account id
    QString machine;
    bool operator==(const MachineReference&) const = default;
};

// The size a machine was last shown at in its enlarged view, where that was not "fit" (b10): one of the
// page's scales, "50", "75", "100", "125", "140" or "200" (ui/picture_scale.h).
struct MachineScale {
    MachineReference machine;
    QString scale;
    bool operator==(const MachineScale&) const = default;
};

struct Remembered {
    // The server that was signed in to last, as ServerClient writes an address; empty: none yet.
    QString server;
    // Servers whose sign-in was saved on this PC. Credentials remain in SecretStore.
    QStringList servers;
    static constexpr qsizetype mostMonitoredServers = 32;
    QList<MachineReference> favorites;
    QList<MachineReference> recentMachines; // most recently opened first
    static constexpr qsizetype mostFavorites = 32;
    static constexpr qsizetype mostRecentMachines = 10;
    // The size each machine was last shown at large (MachineScale), the one chosen last first: of this
    // PC's own machines and of each server's, by the same references as the favorites. No more than
    // `mostPictureScales`, once for each machine; a machine shown at "fit" has none.
    QList<MachineScale> pictureScales;
    static constexpr qsizetype mostPictureScales = 200;
    // The names of the servers that were reached by HTTPS once. None of them is spoken to in the
    // clear again (SPEC.md section 8).
    QStringList reachedEncrypted;
    // The certificate each server is known by, for those that show one Windows does not trust by
    // itself: the server's name and port, and the certificate's SHA-256. Whoever sits at the PC
    // said yes to each of them after seeing it.
    QHash<QString, QByteArray> certificates;
    // The program's windows as they were, in the order they were opened in; none: the program has
    // not had one yet. As many as were open when the program was last closed, and no more than
    // `mostWindows`.
    QList<WallWindow> windows;
    QList<Workspace> workspaces;
    static constexpr qsizetype mostWorkspaces = 16;
    static constexpr qsizetype longestWorkspaceName = 80;
    // How often a new picture of each machine is asked for where that was chosen on this PC, in
    // milliseconds; 0: as often as the server names. One choice for all the windows.
    int wallPictureEveryMs = 0;
    // How the program looks: "dark", "light" or "system" (as Windows is set); nothing: not chosen.
    QString theme;
    // The program goes on behind its icon in the notification area when its window is closed.
    bool staysInTray = false;
    // While nobody can see the wall, Windows shows a message when a machine goes away or comes
    // back. Until somebody switches it off.
    bool tellsNews = true;
    // Unknown file values use Right Ctrl, so a damaged setting cannot remove the way out.
    QString releaseKey = QStringLiteral("right-ctrl");
    static bool namesAReleaseKey(const QString& key);
    // How video is decoded: "automatic" (where the engine finds it best), "on" (on the graphics
    // card, for every picture it has room for) or "off" (on the processor). Anything else in the
    // file is "automatic".
    QString hardwareDecoding = QStringLiteral("automatic");
    static bool namesAHardwareDecoding(const QString& word);
    // Which graphics adapter the program draws and decodes on: "most-powerful", "power-saving" or
    // "windows" (the one Windows gives it). The choice, and never an adapter's name. Anything else
    // in the file is "most-powerful".
    QString graphicsCard = QStringLiteral("most-powerful");
    static bool namesAGraphicsCard(const QString& word);
    // Automatic checks only. Downloading and installation always require a person's choice.
    bool checkUpdates = true;
    QDateTime lastUpdateCheck;
    // The wall that was shown last is this PC's own, and not a QKVM server's: the program starts
    // into it. (It does so as well where no server was ever connected to.)
    bool showsOwnWall = false;
    // The section of the settings box that was shown when the box was last closed, by its key
    // (namesASettingsSection); nothing: none yet. The box opens there, in whichever window lists
    // it. In the file only a key that names a section is taken; anything else is none.
    QString settingsSection;
    // Whether this is the key of one of the settings box's sections: "kvms", "actions", "files",
    // "general", "video", "alerts", "security", "updates" or "about" (ui/settings_view.h lists them
    // for each wall).
    static bool namesASettingsSection(const QString& key);

    // This PC's own KVMs, in the order of their wall. No more than `mostKvms`, and no two by
    // one `id`. (Of the file's list, the first `mostKvms` entries are looked at, whatever they
    // are, and no entry behind them: a list this program wrote has no more, and one of millions
    // of entries that are no KVMs is not gone through to its end.)
    QList<OwnKvm> kvms;
    static constexpr qsizetype mostKvms = 2000;
    // The KVMs of the file that this build does not read: of the program's shape (an id and an
    // address it writes) and of a family it does not speak to (OwnKvm::kind), as another build of
    // the program wrote them, or an older one (a KVM that says no family at all). Kept as they
    // stood, never on the wall, and written back as they stood, before the wall's own: a build that
    // is gone back to (an update undone) does not lose the KVMs of a family it does not know, and
    // the build after it finds them again. Their ids are taken: no KVM is given one of them. With
    // the wall's own, no more than `mostKvms`.
    QJsonArray kvmsItDoesNotRead;
    // The certificate itself of a KVM that is known by one, by the KVM's `id`: its text as a file
    // of the wall holds one (PEM, in lines of 64; exchange.h), for the day the wall is written to
    // a file. Beside the list and not in it: what a KVM is known by is the fingerprint in the
    // list, and this is never more than the certificate that has that fingerprint. Of a KVM
    // that has not shown its certificate since it was known by it, there is none here yet. In the
    // file it stands with its KVM ("certificate_pem"), and is believed only where it is a
    // certificate as a file of the wall holds one and has the fingerprint the KVM is known by:
    // a damaged one, one that is too long, and another KVM's are as none, and the fingerprint
    // stays what the KVM is known by. Nothing of it is secret.
    QHash<QString, QString> kvmCertificates;
    // The wall's mark beside each of those KVMs of whether a password is kept for it: the KVMs, by
    // their `id`, for which the wall kept one when it last kept or forgot that KVM's password (as
    // it was added, changed, said yes to under another certificate, taken from a file or taken
    // off: local_wall.h). Written by the wall in the same writing as the list, and by nothing
    // else. It is no secret: it says that a password was given, and nothing of it. Whoever lists
    // the KVMs says "no password saved" by it, and reads no password for that. In the file it
    // stands with its KVM ("secret_kept"), so only where it says true as a flag does; of a KVM
    // that says nothing of it (one of a wall that was written before the program kept the mark)
    // it is not so. A mark for an `id` that is no KVM of the list is not written down.
    QSet<QString> kvmPasswordsKept;
    // The KVMs the wall followed onto HTTPS by itself (LocalWall::followOntoHttps), each with the
    // address it was moved to: said in its details until whoever sits here has seen that
    // (LocalWall::sawNote), also after a start. In the file it stands with its KVM
    // ("followed_onto_https": that address), only where that is the KVM's address still.
    QHash<QString, QString> kvmsFollowedOntoHttps;
    // The JetKVMs over plain HTTP that the wall holds back for a twin (LocalWall: their HTTPS showed the certificate
    // another KVM of the wall is known by), each with the id of that other one: held so from the start of the next run
    // too, until a sign lets go of it (SPEC.md section 8). In the file it stands with its KVM ("held_for_twin": that
    // id), only of a JetKVM over plain HTTP, and only where the id is another KVM of the list.
    QHash<QString, QString> kvmsHeldForATwin;

    // The saved actions of this PC's own wall, in the order they were added in. No more than
    // `mostSavedActions` (server_types.h), and no two by one `id`. Of the file's list, the first
    // `mostSavedActions` entries are looked at, whatever they are, and no entry behind them, as
    // of the KVMs above. Each entry is held to the rules an action is saved by
    // (action::isOne in own_actions.h: an id of the program's shape, a name as it is kept of what
    // was typed, keys that are keys for one that presses, Enter only with a text, flags that are
    // flags, a count of its edits that is one): one that does not hold is left out, as a KVM is
    // whose address is none. A file that says an action types, and names no writing of its text
    // that could be one, has that action without a text. Never in the file: the text itself.
    QList<OwnAction> actions;

    // More windows than this are not opened, and not remembered: each is a wall drawn by itself.
    static constexpr qsizetype mostWindows = 8;

    bool operator==(const Remembered&) const = default;
};

// The one file under the user's profile that holds what is remembered (DESIGN.md section 4). It is
// read once, when this is made; a file that cannot be read or is not what this writes counts as
// none, and nothing of it is used. It is written whole into a new file that then takes the place of
// the old one: a PC that is switched off in the middle leaves the old file or the new, and never
// half of one.
//
// A file that was there and could not be read is not written over: the list of this PC's own KVMs
// is in it, with what each is known by, and a file that another program held for a moment, or
// that was damaged in one place, is not to cost all of that. Before the first writing it is put
// aside, beside where it was (`putAside`); and where that cannot be done, nothing is written.
class Settings {
public:
    // For how long a file that is there and cannot be opened is tried when the program starts:
    // another program (one that looks for viruses, one that makes copies) holds a file for a
    // moment, and rarely for longer. A test waits less.
    struct Patience {
        int tries = 40;
        int betweenMs = 50;
    };

    explicit Settings(QString file);
    Settings(QString file, Patience patience);

    // Where the file is for whoever is signed in to Windows.
    static QString usualFile();

    const Remembered& remembered() const { return m_remembered; }
    // There was a file when the program started, and it could not be read or is not what this
    // writes: the program began as one that remembers nothing.
    bool wasUnreadable() const { return m_wasUnreadable; }
    // The file holds KVMs this build does not read (Remembered::kvmsItDoesNotRead): a wall was
    // kept in it, whatever this build can make of it (the app lock asks: app_lock.h).
    bool heldKvmsItDoesNotRead() const { return !m_remembered.kvmsItDoesNotRead.isEmpty(); }
    // Where that file is now; empty while it is where it was, and where there was none.
    QString putAside() const { return m_putAside; }
    // Puts the file that could not be read aside, where it still is where it was: beside it,
    // under the first name of its kind that is free. Moved, and never copied: a file that cannot
    // be moved (something holds it) stays what and where it is, that is told, and nothing is
    // written until it can be. Whoever starts the program does this before anything is
    // remembered, and says what came of it. Nothing where no such file is in the way.
    base::Result<void> putAsideWhatCouldNotBeRead();
    // Remembers this from now on, and writes it down. What is remembered changes even when the
    // writing fails; the failure is told. A file that could not be read is put aside first.
    base::Result<void> remember(const Remembered& what);
    // Remembers this from now on, and does not write it down yet: for what the program notes by
    // itself, with nobody having done anything (this PC's wall: that a KVM was asked what it is,
    // and a certificate it kept), which may come a hundred times in a second where a wall of a
    // hundred KVMs starts. It is written with whatever is written next (remember() writes all
    // that is remembered), or when whoever noted it says so (writeDown()). Lost with a program
    // that ends before that, and noted again at the next start: so nothing is noted this way
    // that somebody did, or that costs more than a question to find out again.
    void note(const Remembered& what);
    // Writes down what is remembered, where something was noted, or could not be written, since
    // the file was last written; nothing otherwise, and nothing is told then.
    base::Result<void> writeDown();
    // Whether something is remembered that the file does not say yet.
    bool hasUnwritten() const { return m_unwritten; }
    // How often the file was written since this was made: for whoever has to know that many
    // notes were one writing (a test among them).
    quint64 writings() const { return m_writings; }

private:
    QString m_file;
    Remembered m_remembered;
    // Something is remembered that the file does not say: it was noted, or a writing failed.
    bool m_unwritten = false;
    quint64 m_writings = 0;
    bool m_wasUnreadable = false;
    // The file that could not be read is still where it was, and in the way of what is written.
    bool m_inTheWay = false;
    QString m_putAside;
};

// One window's own part of what is remembered. Which of the windows it is, counted from 0 in the
// order they were opened in, changes while the program runs when a window before it is closed:
// whoever keeps the windows says so.
//
// The settings outlive it.
class WindowSlot {
public:
    WindowSlot(Settings& settings, qsizetype index) : m_settings(settings), m_index(index) {}

    // What is remembered of this window; where nothing is yet, a window as a new one is.
    WallWindow remembered() const;
    // Remembers this of it from now on, and writes it down where it is not what was remembered.
    // As Settings::remember: remembered even when the writing fails, which is told.
    base::Result<void> remember(const WallWindow& what);

    qsizetype index() const { return m_index; }
    void moveTo(qsizetype index) { m_index = index; }

private:
    Settings& m_settings;
    qsizetype m_index;
};

} // namespace qkvm::core
