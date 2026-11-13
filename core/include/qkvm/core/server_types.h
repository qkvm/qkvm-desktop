#pragma once

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QUrl>

#include <qkvm/base/result.h>

// What a QKVM server says, as types. The server's answers are JSON; nothing of an answer is used
// before it has been read into one of these, and an answer that is not what these expect is refused
// whole (SPEC.md section 8: a reply is never trusted).
namespace qkvm::core {

// Which version of the server's API this program speaks: every call but the first goes to
// "/api/v1/..." (ServerClient).
inline constexpr QLatin1StringView spokenApi{"v1"};

// What a server says of itself to anybody, before anything else is asked of it: GET /healthz, which
// needs no sign-in (SPEC.md section 6.1).
struct Health {
    QString version; // of the server's program, as it says it; empty: it does not say
    // The versions of its API that it speaks, each under "/api/<version>/": "v1", "v2".
    QStringList apis;
    // It was restarted locked and nobody has signed in since: it reaches no machine until somebody does.
    bool locked = false;
};

// Whoever is signed in. A role is "viewer", "operator" or "admin"; the server enforces it, and the
// program only leaves out what would be refused.
struct Account {
    QString id;
    QString name;
    QString role;
    bool isDisplay = false;  // not a person: a screen opened with a display link, which only looks
    bool mustEnroll = false; // only the server's MFA setup page is available until completed
};

// What a server says of itself before anybody is signed in, and who is.
struct Session {
    bool setUp = false;    // a server that is not set up yet is set up on its page
    bool asksName = false; // with one account, its password alone is enough
    std::optional<Account> account;
};

// One machine of the wall, as the wall lists it.
struct Machine {
    QString id;
    QString name;
    QString host;
    QString kind;
    QString group;
    bool hasPicture = true; // false: a server seen through its controller, whose card shows how it is doing
    // Where a browser reaches the machine's own console; empty: nowhere a browser could be sent.
    QUrl page;
    // Whoever keeps the wall speaks to this machine without encryption: its password, and what
    // is seen and typed there, can be read on the way. (A QKVM server does not say how it reaches
    // its machines; this PC's own wall does.)
    bool plain = false;
    // A KVM of this PC's own wall: whoever sits here can change it and take it off. (Only that
    // wall says so: what a QKVM server lists is changed on the server's page.)
    bool own = false;
    // What this PC's own wall has to say of it in its details: that it followed it onto HTTPS by
    // itself, with the certificate it is known by there. Empty: nothing.
    QString note;
    // Missing: an older server/direct adapter. Present, including empty: advertised abilities.
    std::optional<QStringList> capabilities;
    // A PC with Intel AMT (kind "amt"): which of its three views its tile is in, as the wall lists
    // it ("health", "stills" or "live"; a word of a view this program does not have is "stills", its
    // first). Empty for every other machine, and where the wall says none (an older server: 0.2.2 has no
    // views, nor a way to change one). How it is now, the status says the fresher (amtViewOf).
    QString amtView;
    // A KVM of this PC's own wall: its screen watches, in minutes (0: not watched; screen_watch.h).
    int noSignalMinutes = 0;
    int unchangedMinutes = 0;
};

// Which form of the live connection this program speaks (SPEC.md section 6.2): a server that has
// live pictures says which form it speaks, and a pass is for a line of that form.
inline constexpr int spokenLiveForm = 2;

struct Wall {
    double everySeconds = 1; // the pace the server names for pictures
    // Which form of the live connection the server speaks (SPEC.md section 6.2); 0: it has no
    // live pictures, and is asked for none.
    int liveForm = 0;
    QList<Machine> machines;
};

// One of the computers behind a KVM that has several.
struct Port {
    QString name;       // empty: the server knows it only by its number
    bool empty = false; // nothing is connected to it
};

// How a server is doing, as its controller tells it.
struct ServerReadings {
    QString health; // "OK", "Warning", "Critical", or nothing when it does not say
    // The light to find it by in a rack: on, off, or nothing where it has none.
    std::optional<bool> locator;
    int fans = 0;
    int fansRunning = 0;
    int fansNotWell = 0;
    QString processor;
    int processors = 0;
    std::optional<double> memoryGib;
    // Why its screen is not shown where it could be (`no_screen`), in the server's words; empty:
    // nothing to say (it is shown, or the machine is simply off). A QKVM server's status says it for a PC with
    // Intel AMT in health only; this PC's own servers say it themselves (somebody else at the console, its
    // monitor going dark while a console is open).
    QString noScreen;
    // This PC's own servers: a start into BIOS setup that did not end may have left its boot override set, so its next
    // boot may land in the BIOS setup (setup_overrides.h, review 4's MEDIUM 1): what its card says of that, until a
    // look shows the override off. Empty: nothing to say. (A QKVM server's status says no such thing.)
    QString setupLeft;
    // A PC with Intel AMT: its tile's view as the status says it (`amt.view`): "health", "stills" or
    // "live"; empty where it says none of those.
    QString amtView;
};

// A Fingerbot paired with a KVM, which presses the computer's power button with an arm (`fingerbot` in `can`): its
// battery in percent, where it said so, and whether that is low. It says nothing of the computer's power.
struct Fingerbot {
    std::optional<int> battery;
    bool low = false; // only where its battery is known
};

// What is in a machine's virtual drive, as the server last found it (0.3.0): the status's `media`, which everybody who
// sees the machine's tile is told, and `mounted` in its list of images (MediaList). Its texts are the device's to say,
// and each is one plain line here.
struct MountedMedia {
    QString drive;         // "cd", "flash" or "disk"; a word this program does not know is kept, and said as "Media"
    QString image;         // its id, as the list names it: what an unmount names
    QString name;          // never empty: a drive whose image is not named is not taken for one
    QString where;         // where it is kept (a storage of a Proxmox); empty: on the device itself
    bool readOnly = false; // only where the server says so plainly: otherwise it is said to be writable
    std::optional<bool> first; // it is first in the machine's boot order; nothing: nothing here can tell
    QString by;                // who mounted it from qKVM; empty: not known
    std::optional<qint64> at;  // and when, in seconds since 1970
    QString sha256;            // where qKVM carried it there, and it is still that image
};

// How one machine is, as GET /api/v1/status says it.
struct Status {
    QString state = QStringLiteral("connecting");
    QString errorCode;
    QString errorMessage;
    std::optional<int> lastSeenSecondsAgo;
    bool hasPicture = true;
    // A server, seen through its controller, whose screen the QKVM server can read from that
    // controller's console: a picture of it can be asked for as a KVM's can.
    bool hasScreen = false;
    std::optional<bool> signal;
    int width = 0;
    int height = 0;
    bool powerIsKnown = false; // the KVM is wired to the computer's power light
    std::optional<bool> power;
    std::optional<bool> keyboard;
    std::optional<double> temperature;
    QString model;
    QString version; // of the software on the KVM or the controller, as it says it
    // A server's controller hands out a pass that leads straight into its console, signed in.
    bool opensSignedIn = false;
    QList<Port> ports;
    int shownPort = -1;
    std::optional<ServerReadings> server;
    std::optional<QStringList> capabilities;
    // Nothing: the KVM has no Fingerbot paired with it, or the server does not say.
    std::optional<Fingerbot> fingerbot;
    // Nothing: nothing is in its virtual drive, it has none, or the server does not say.
    std::optional<MountedMedia> media;
};

// Fresh status takes precedence over inventory. An absent list preserves older-server behavior.
bool supports(const Machine& machine, const Status* status, const QString& ability);

// Which of its three views a PC with Intel AMT is in, as the page has it (app.js, amtView): the
// status's word where it says one (another window or browser may have changed it since the wall
// was listed), else the wall's. Empty for a machine that is no such PC, and for one its server says no
// view of: what a server does not have is not shown (no View rows, no view on its card, b11).
QString amtViewOf(const Machine& machine, const Status* status);
// Whether this is one of the three views' words: "health", "stills", "live".
bool isAmtView(const QString& word);
// Whether a picture of this machine's screen is asked for at all: a KVM's, and a server's whose
// screen the QKVM server reads (Status::hasScreen); never of a PC with Intel AMT in health only,
// whatever else its status says, since no KVM session is to be opened to it (the page's showsScreen).
bool hasAScreenToShow(const Machine& machine, const Status* status);

// A picture of a machine's screen, as GET /api/v1/devices/{id}/snapshot gives it: a JPEG or a PNG,
// whichever the machine gave the server.
struct Snapshot {
    QByteArray picture;
    bool signal = true; // false: the machine sends no picture, and this is the last one it sent
    // A still kept between sessions (a PC with Intel AMT whose tile is only shown: X-KVM-Still), and
    // how old it is in seconds (X-KVM-Age), where that is a whole number; nothing: not said.
    bool still = false;
    std::optional<int> ageSeconds{};
};
// What a snapshot's answer says of a still: its X-KVM-Still and X-KVM-Age, read into it. An age is
// taken only of a still, and only where it is a whole number of seconds that is not below 0.
void readStill(Snapshot& snapshot, const QByteArray& still, const QByteArray& age);

// One of the actions an admin saved on the server for any machine, as the server lets it be seen:
// never the text it types.
struct SavedAction {
    QString id;
    QString name;
    bool types = false; // it types its text; otherwise it presses `keys`
    QString keys;       // pressed together, as "ControlLeft,AltLeft,Delete"
    bool enter = false; // Enter after its text
    bool danger = false;
    int changed = 0; // how often it was edited: whoever runs it says which of its states they looked at
};

// A computer that a KVM can wake over the network.
struct WakeTarget {
    QString name; // empty: it is called by its address
    QString mac;
};

// What is asked of a machine: one of the server's actions, and what that one needs. Made by the
// functions here, one for each kind, so that none goes out half filled in; the server checks every
// one of them again, and whether whoever asks may.
struct Action {
    // Those that need nothing else, by the server's word for them: "reboot", "atx_power",
    // "atx_power_long", "atx_reset", "power_on", "power_off", "jiggler_on", "jiggler_off",
    // "locate_on", "locate_off".
    static Action plain(const QString& kind);
    // Keys pressed together and let go: "ControlLeft,AltLeft,Delete".
    static Action press(const QString& keys);
    // Text typed as on the machine's keyboard, and Enter after it where that is asked.
    static Action type(const QString& text, bool enter);
    // The computer a KVM with several shows: counted from 0.
    static Action show(int port);
    static Action wake(const QString& mac);
    // A saved action, in the state it was looked at: one that was changed since is refused.
    static Action run(const SavedAction& saved);

    QString kind;
    QString keys;
    QString text;
    bool enter = false;
    int port = 0;
    QString mac;
    QString savedId;
    int savedChanged = 0;
    // Where the same thing is done to several machines in one go: what ties the entries of that run
    // together in the server's activity log. Letters, digits and "-", not more than forty.
    QString batch;
    bool savedTypes = false; // local metadata for capability checks; never serialized
    // Asked from the machine's large view, as the page says it (`watching`): a PC with Intel AMT then opens a session
    // before it switches on, to show the start. (A run on several says nothing of it.)
    bool watching = false;
};

// What the server answers when it was done: sometimes more than that it was ("already on").
struct Done {
    QString note;
};

// The wall as a file, to keep or to take to another QKVM: the server makes it and the server reads
// it (app/qkvm/exchange.py), and this program only writes it out and hands it in. With a passphrase
// it is encrypted and holds the devices' passwords, the saved texts and the address alerts go to
// (.qkvm); without one it is plain and holds none of those (.json).
//
// The parts of a wall that a file can hold, by the server's words for them and in the page's order:
// "devices", "actions", "alerts", "wall".
QStringList wallFileParts();

// A file as the server made it (POST /api/v1/export): its text, written out as it is; whether it
// says of itself that it is encrypted; and how many saved passwords or texts are not in it
// because they do not open any more.
struct WallFile {
    QByteArray text;
    bool sealed = false;
    int notOpened = 0;
};

// What a file holds, as the server read it (POST /api/v1/import/read). Nothing is taken in by that.
struct FileDevice {
    QString name;
    QString host; // where it is: the name and the port of its address
    QString group;
    bool hasPassword = false;
    QString tls;        // how its certificate is checked: "", "pinned", "insecure" or "system"
    QString openUrl;    // where "Open console" would send a browser, by the file's saying
    bool isNew = false; // this wall does not have it, and would take it
    QString sameAs;     // left out as the same device as this tile, which is here at another address
};
struct FileAction {
    QString name;
    bool types = false; // it types its text; otherwise it presses `keys`
    QString keys;
    QString text; // what it types, whole, to be read before it is taken: of the new ones only
    bool enter = false;
    bool danger = false;
    bool isNew = false;
};
struct FileAlerts {
    bool enabled = false;
    bool hasAddress = false; // the file names where alerts go, and `host` is that address' host
    QString host;
    QStringList kinds; // of "offline", "no_signal", "problem", "recovered"
};
struct FileWallSettings {
    std::optional<double> pictureEverySeconds;
    std::optional<bool> asksForNewerVersions;
};
struct FileContents {
    bool encrypted = false;
    bool hasSecrets = false; // a plain file with passwords in it: somebody wrote it by hand
    qint64 madeAt = 0;       // seconds since 1970; 0: the file does not say
    QString program;         // the version of QKVM that made it
    // Each part the file has; one it does not have is not there.
    std::optional<QList<FileDevice>> devices;
    std::optional<QList<FileAction>> actions;
    std::optional<FileAlerts> alerts;
    std::optional<FileWallSettings> wall;
    // What the server did not take from the file, entry by entry, with why.
    QStringList leftOut;
};
// What was taken in (POST /api/v1/import).
struct Taken {
    int devices = 0;
    int actions = 0;
    bool alerts = false;
    bool wall = false;
};

// Limits on what a server may send: checked before anything is taken from an answer.
inline constexpr qsizetype largestAnswerBytes = qsizetype{2} * 1024 * 1024;
// What a server says of itself is a line or two, and is asked of whatever stands at an address
// somebody typed: more than this is no QKVM's.
inline constexpr qsizetype largestHealthBytes = qsizetype{64} * 1024;
inline constexpr qsizetype longestVersion = 40;
inline constexpr int mostApis = 16;
// A file of the wall, either way: the server writes none larger than about five million
// characters, and the page hands none in that is larger than this.
inline constexpr qsizetype largestFileBytes = 8'000'000;
inline constexpr int mostLeftOut = 200;
// A picture as large as the server takes one from a machine (its MAX_PICTURE: a large PNG of a big screen), on the
// line of live pictures as well as a snapshot.
inline constexpr qsizetype largestPictureBytes = qsizetype{32} * 1024 * 1024;
inline constexpr int mostMachines = 2000;
inline constexpr int mostSavedActions = 500;
inline constexpr int mostWakeTargets = 200;
inline constexpr qsizetype longestText = 300;
inline constexpr qsizetype longestAddress = 2000;
// The most one action types, as the server has it: in characters, of which some take two places
// in a text of this program's.
inline constexpr qsizetype longestTypedText = 2000;

// What a QKVM says of itself: GET /healthz. Whatever does not say "status": "ok" and list its
// versions is no QKVM, and gives "bad_answer". Of the list, the first sixteen entries are looked
// at, and what is not written as a version ("v" and one to four digits) is left out.
base::Result<Health> healthFrom(const QByteArray& answer);
base::Result<Session> sessionFrom(const QByteArray& answer);
base::Result<Account> accountFromSignIn(const QByteArray& answer);
base::Result<Wall> wallFrom(const QByteArray& answer);
base::Result<QHash<QString, Status>> statusFrom(const QByteArray& answer);
// A machine's picture, from the answer, the kind the answer says it is (its Content-Type) and
// what it says of the machine's signal (X-KVM-Signal): GET /api/v1/devices/{id}/snapshot. A
// picture only where the answer says that it is one of a kind this program draws (image/jpeg,
// image/png) and begins as one of those; anything else is no QKVM's answer ("bad_answer").
base::Result<Snapshot> snapshotFrom(QByteArray answer, const QByteArray& kind, const QByteArray& signal);
base::Result<QList<SavedAction>> savedActionsFrom(const QByteArray& answer);
base::Result<QList<WakeTarget>> wakeTargetsFrom(const QByteArray& answer);
base::Result<Done> doneFrom(const QByteArray& answer);
// The pass for one live line to one machine (SPEC.md section 6.2): POST /api/v1/devices/{id}/live.
// Good for a short while and for one line. An answer in another form of the live connection than
// this program speaks is refused under the code "live_form".
base::Result<QByteArray> livePassFrom(const QByteArray& answer);
// The wall's machines in the order the server has them, after it was told one: PUT /api/v1/wall/order.
base::Result<QStringList> orderFrom(const QByteArray& answer);
// The view a PC with Intel AMT is in after it was told one: PUT /api/v1/devices/{id}/view answers
// {"ok": true, "amt_view": ...}. What says no view of the three is no answer of the server's.
base::Result<QString> amtViewFrom(const QByteArray& answer);
// A file the server made, from its answer and the header that says what is not in it: POST
// /api/v1/export. What does not say of itself that it is a file of QKVM's is none: the answer of
// something that stands before the server is not written out as the wall.
base::Result<WallFile> wallFileFrom(const QByteArray& answer, const QByteArray& notOpened);
// Whether a file says of itself that it is encrypted, and so has a passphrase to be asked for. What
// is no file of QKVM's says nothing of the kind: the server is the one that says what it is.
bool isSealedFile(const QByteArray& file);
base::Result<FileContents> fileContentsFrom(const QByteArray& answer);
base::Result<Taken> takenFrom(const QByteArray& answer);
// What the three calls are sent: the bodies of POST /api/v1/export, /api/v1/import/read and /api/v1/import.
QByteArray exportBody(const QString& password, const QString& passphrase, const QStringList& parts);
QByteArray importBody(const QByteArray& file, const QString& passphrase, const QStringList* take);
// Where the server says a machine's console is entered, signed in: POST /api/v1/devices/{id}/console.
base::Result<QUrl> consoleFrom(const QByteArray& answer);
// An address a browser may be sent to: http or https, with a host, and with no name or password in
// it. Anything else gives an empty one: an address comes from a server or a device, and what is
// handed to Windows to be opened is opened by whatever Windows finds for its kind.
QUrl webAddress(const QString& written);
// Whether two such addresses are the same place: the same scheme, host and port.
bool sameSite(const QUrl& one, const QUrl& other);
// An action as POST /api/v1/devices/{id}/actions takes it: the server's word for it, and of the rest
// only what that kind needs.
QByteArray bodyOf(const Action& action);

// --- Virtual media through a server (0.3.0): an image in a machine's drive, and the machine started from it ----------
// An admin's, signed in in person: whoever mounts an image on a machine and starts it from that is root on it. The
// server refuses a token, and anybody else (`not_allowed`).

// One image that can be mounted on a machine, as its list names it.
struct MediaImage {
    QString id;                 // what a mount and a deletion name: as the list says it, never made up here
    QString name;               // never empty
    QString where;              // where it is kept (a storage of a Proxmox); empty: on the device itself
    std::optional<qint64> size; // in bytes; nothing: not said
    bool partial = false;       // what an upload that was cut off left there: only to delete
    QString sha256;             // where qKVM carried it there, and it is still that image
};

// What a machine's virtual media can do, as the server's driver for it says (its MediaOffer). Of each list, only the
// server's own words for it are kept: "cd", "flash", "disk"; "once", "until_unmounted".
struct MediaOffer {
    QStringList drives;      // what an image can be mounted as
    QStringList boot;        // how it can be told to start from it; none: it cannot (a KVM is no firmware)
    bool restart = false;    // it can be restarted from here
    bool upload = false;     // an image can be carried through qKVM into the device's own storage
    QString where;           // where its images are: "proxmox", "device" or "qkvm"
    QStringList writable;    // of `drives`: those an image may be mounted writable in, where that is chosen
    bool canDelete = false;  // an image can be deleted from where it is
    bool dropsInput = false; // a switch of its drive between CD-ROM and disk drops its keyboard and mouse a moment
};

// An image on its way to a machine's own storage: whose, and how far it has come, by the server's count.
struct MediaUploading {
    QString name;
    qint64 size = 0;
    qint64 sent = 0;
    QString by;
};

// GET /api/v1/devices/{id}/media: what can be mounted on the machine and what is mounted, asked of it now.
struct MediaList {
    QList<MediaImage> images;
    QStringList unseen; // a sentence for each place whose images could not be listed
    int leftOut = 0;    // entries there that are no image qKVM hands on
    QStringList notes;  // what stands against a change just now
    // The device's own storage, in bytes: what is free of how much, where it says.
    std::optional<qint64> free;
    std::optional<qint64> size;
    std::optional<MountedMedia> mounted;
    MediaOffer offers;
    std::optional<MediaUploading> uploading;
    QString servedFrom; // where a server's controller reads its image from qKVM; empty: not said
};

// What is asked of a machine's drive (POST .../media). `machine` is the machine's name as the question named it: the
// server does nothing where it is called otherwise by then (409 changed).
struct MediaMount {
    QString image;
    QString machine;
    QString drive = QStringLiteral("cd");
    bool writable = false;
    QString boot = QStringLiteral("none"); // or "once", "until_unmounted"
    bool restart = false;
};

// What a mount, an unmount and a deletion answer once done: a sentence where there is more to say than that, and what
// is in the drive now.
struct MediaDone {
    QString note;
    std::optional<MountedMedia> mounted;
};

// An image that was carried through whole: its SHA-256 as the server counted it on its way.
struct MediaUploaded {
    QString sha256;
};

// The largest image the server carries through to a device (its MOST_UPLOAD): 16 GB.
inline constexpr qint64 mostUploadBytes = 16'000'000'000;
// The most images a list holds (the server's MOST_DEVICE_IMAGES).
inline constexpr int mostMediaImages = 2000;

base::Result<MediaList> mediaListFrom(const QByteArray& answer);
// GET /api/v1/devices/{id}/media/upload: the upload under way, or nothing.
base::Result<std::optional<MediaUploading>> uploadingFrom(const QByteArray& answer);
base::Result<MediaDone> mediaDoneFrom(const QByteArray& answer);
base::Result<MediaUploaded> uploadedFrom(const QByteArray& answer);
QByteArray mountBody(const MediaMount& mount);
// The body of an unmount and of a deletion: the image as the question named it, and the machine.
QByteArray imageBody(const QString& image, const QString& machine);
// Whether the server takes this as the name of an image carried to a device (kvm.IMAGE_NAME): letters, digits, blanks
// and . _ + - ( ) [ ] ~ , (200 at the most), neither beginning nor ending with a blank or a dot, and not ending in
// ".incomplete" (what a JetKVM calls an image while it comes).
bool isImageName(const QString& name);

// Why the server refused, from its answer {"code": ..., "message": ...}: the message is written to be
// shown to a person as it is. An answer that is not of that shape gives a sentence of the program's own.
base::Error refusalFrom(int httpStatus, const QByteArray& answer);
// Whether an answer is a refusal of that shape, with a code and a message that say something:
// the server's own words. What is not (a gateway's page, nothing, another program's JSON) was
// said by something else in the server's place, or by nobody.
bool isRefusalOfAServers(const QByteArray& answer);

} // namespace qkvm::core
