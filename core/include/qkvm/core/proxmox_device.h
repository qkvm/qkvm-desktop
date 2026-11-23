#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <qkvm/base/result.h>
#include <qkvm/core/device_asker.h>
#include <qkvm/core/kvm_times.h>
#include <qkvm/core/own_device.h>
#include <qkvm/core/own_media.h>
#include <qkvm/core/proxmox_answers.h>
#include <qkvm/core/rfb_screen.h>
#include <qkvm/core/server_console.h>
#include <qkvm/core/server_live.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/wiped.h>

namespace qkvm::core {

// The driver of a virtual machine on Proxmox VE (a guest) on this PC's own wall, through Proxmox's own API, in the
// same words as a server's controller (redfish_device.h) wherever they say the same thing. Its reference is the web's
// driver (../app/qkvm/proxmox.py, the frozen copy of web 0.3.1), which a real Proxmox VE 9.2 was measured against.
//
// * A guest is named by its Proxmox (Where::address: https, its host, its port, 8006 unless another was typed), its
//   node and its number. It is asked over HTTPS alone, held to the certificate its Proxmox is known by, straight to it
//   whatever proxy this PC uses, with a limit on every answer and its time (DeviceAsker). Everything that comes from
//   Proxmox is read by kind and within bounds (proxmox_answers.h).
// * It is signed in to with an API token and in no other way, as the web is: never a person's password and a ticket.
//   The token's ID is the user name, its secret the password; both go in the Authorization header of every question
//   (`PVEAPIToken=<id>=<secret>`), and in no address, no log and no sentence. What is no token's shape is sent nowhere
//   ("invalid"). The first question that carries it is the sign-in (GET /version), counted as one (SignInPacing, the
//   road "login" at the Proxmox, for every guest of it on the wall together: they share one verdict on the token, and
//   one that would sign in while another's sign-in with it is out waits for what that comes to, and goes on without a
//   sign-in of its own where it was taken). Right before every sign-in, and before another's is taken for this guest's,
//   what answers on the very line the token is to go on is asked what it is, with nothing of the token
//   (askWhatAnswers: the unsigned question of its API, proxmox::saysItIsAProxmox), and the token goes only where it
//   says it is a Proxmox: so on every road a card comes by (an add, an edit, a yes to another certificate, a file, a
//   start of the program, a sign-in again after a 401; outside review of 0.2.0, 3.2), whatever the wall says of it, and
//   somebody else there is sent nothing ("bad_response", and asked again only after a hold, as a failure is). The web
//   sends the token with its first question. A refused token (401) is offered again as SignInPacing says. A question
//   answered 401 later is signed in to once more (the token was taken back, or Proxmox restarted); answered 401 again
//   after that, "Proxmox takes the token and refuses the request after it", held back as a sign-in that came to
//   nothing. A question that does something (a command, a console's ticket, a change of its settings) and finds its
//   driver signed out on its way is sent as one answered 401 is sent again: once which virtual machine has the number,
//   and what its press is to find in its settings, were asked again on a reading after that sign-in, within its moment
//   (review cx3-3, L1); never on a reading a sign-in old: so too one decided on a reading that went out under an
//   earlier sign-in than the one it is to go under, its driver signed out and in again while that reading was out (each
//   reading is told the sign-in it went under, and its command held to that: outside review 2, 4.1). The next question
//   after contact with what answers there
//   failed (nothing back at all: nothing there, the line fell before any answer, another certificate; of any question,
//   the unsigned question of its API and the sign-in among them (outside review 2, 4.2), or of its console's line
//   before Proxmox let it in, or after that without Proxmox's goodbye; review cx3-2, L1) signs in once more too, and so
//   does every other guest's driver of that Proxmox (review cx3-3, L3; for a certificate refused, every other known the
//   same way, by that pin or by Windows' trust: fresh review of 4.2, M1): the sign-in stood for what answered then, and
//   what answers next is asked first. A sign-in stands for what answered its unsigned question: where contact was lost
//   while the two were out, it stands for nothing, and the question that waited for it asks again first (outside
//   review 2, 4.2). No answer
//   in time is no such failure: its line was let in by what the Proxmox is known by, and the token went on it; nor is a
//   console Proxmox closed with its goodbye, as it ends the console of a guest that stops (review cx3-3, L2 and L3).
//   Between those, the token goes with every question on what the Proxmox is known by (section 8 of SPEC.md). Proxmox
//   holds every 401 back three seconds: the sign-in's answer, and the unsigned question's, are waited for as any
//   family's sign-in is.
// * How it is (look, the web's `_read_status`): its status (status/current) at every look, at most once in
//   Pace::freshMs; "online" while it runs and "no_signal" while it is stopped; its power, its processors and memory,
//   "VM <number> on <node>", Proxmox's version. A token Proxmox does not take is "auth"; another certificate, an
//   answer that is no answer, a refusal of the token's rights, "error"; anything else (nothing there), "offline".
// * What it is (identify, for a person who adds it): its model ("VM 100 on pve1"), Proxmox's version, its name, and
//   the number it gives for itself (below), taken where none was kept.
// * The guests the token may see (listing, the web's `guests`), for a person adding them: by kind, containers counted.
// * Rule 30, a guest is the one it was added for (the web's `_same_guest` and `_judge`). Proxmox names a guest by its
//   node and its number, and gives a new virtual machine the lowest number that is free: a guest deleted and another
//   made, on its node or on another, has its number. So a guest is known by the number it gives for itself (its
//   SMBIOS UUID, proxmox::ownNumber; Where::machine), kept when it is added, and asked afresh (its settings, `config`)
//   right before anything is decided or sent: before every power command, picture, key, text, saved action and
//   console, before the guest is followed to another node of its cluster (where Proxmox says it is not on the one
//   asked), and at a look once in Pace::sameGuestEveryMs, and at once where a look finds it stopped where it ran,
//   running where it was stopped, or running a process begun since (a virtual machine made under its number runs a
//   process of its own), so that its card says so before anybody presses. Another number, or none where one is kept:
//   "another_machine" in the web's words, nothing is sent to it, nothing read of it is said as this guest's, and that
//   is said until the guest has its number again. None kept (a guest taken from a file without one): the first seen
//   is taken (trust on first use, as a certificate is) and said (knownAs). None given and none kept: no check.
// * Its power buttons (act, the web's `act`): start ("power_on"), ask to shut down ("power_off", Proxmox's shutdown,
//   through ACPI or its agent), the power button ("atx_power": whichever of the two applies), stop at once
//   ("atx_power_long", Proxmox's stop: the plug pulled) and reset ("atx_reset"). Decided by its state read right
//   before, and which virtual machine has its number asked after that read and before anything is decided on it
//   ("already on" included); a state neither running nor stopped decides nothing. By what is wanted: one that is so
//   already is left as it is ("already on", "already off"). The command answered with Proxmox's task (a UPID), and a
//   start or a stop waited for until the guest is seen so, the task asked meanwhile how it stands (Pace::
//   confirmWithinMs from its end, Pace::taskWithinMs while it runs or where Proxmox's high availability carries it
//   out, all of it bounded from the press): a task that ended in an error is said at once in Proxmox's words; one that
//   runs on is "timeout", under way and perhaps done, never "failed". An answer lost on its way back (596, 597, a
//   proxy's 502 to 504) may have been done, and is said so; 595 (the node not reached) was not. Looked at again at the
//   moments after (Pace::followUpMs). A driver let go of while it waits says it may have been done.
// * Its screen, live, keys, typed text and control through the console its Proxmox hands out (proxmox_screen.cpp, the
//   web's ProxmoxConsole): POST vncproxy with websocket=1 makes a ticket, a port and a one-time password for one
//   connection, a task in the node's log each time, which the guest's owner reads; the ticket opens vncwebsocket on
//   this program's own line (DeviceLine, the subprotocol "binary", the token in its Authorization header as in every
//   question, never in its address), RFB over it (RfbScreen), VNC authentication answered with the one-time password
//   and never with the token. The ticket is for one connection and used at once; it stands in the address the
//   console's line asks for, held while that lasts and not written over (its password and the token are). Which
//   virtual machine has the number is asked right before each vncproxy (keys and text go through no other way), and
//   none is asked for a guest that is stopped. The console is held while somebody looks, five minutes after the last
//   look (the owner's standard), and asked for again on a ladder after one that failed (Pace::console, consoleLimits).
//   One console for the card's still and for every live of it, as a server's (ServerLive).
// * Its virtual media (proxmox_media.cpp, the web's "virtual media" of 0.3.0, step 7): an ISO image already on one of
//   Proxmox's storages put in the guest's CD-ROM drive, read-only, and taken out; the images listed from the storages
//   of the node it is on that the token may see, taken by kind (proxmox_answers.h). Nothing moves: never Proxmox's
//   download-url, and no upload; nothing is deleted. Which virtual machine has the number is asked (its settings read
//   and judged) at every question of its media, and again right before each change; and the change is made on the
//   settings read for it, by their digest, so that one made on Proxmox meanwhile (another virtual machine made under
//   the number among them) has Proxmox refuse it, and nothing is changed ("changed"). Only an image listed just then
//   is handed to Proxmox. A drive is added where the guest has none (ide2 first, as Proxmox's own page does; a running
//   guest has it from its next start). And the guest started from it, as the web's media_boot does (review c9b-2,
//   M1): its boot order with the CD-ROM drive first (an order written the old way, or not at all, is changed by
//   nobody: said), "once" restarted at once (stopped where it runs, the plug pulled, and started: a reset begins from
//   the order its process began with) and its order put back after, "until_unmounted" restarted where that is asked
//   and its order put back at the unmount (BootOrderWas, kept with the guest: where it is still the one written
//   then); each change of the
//   order on the settings read right before it, by their digest, and each press of the restart a power button's
//   (act: which virtual machine has the number asked before each). What is in its drive is on its card
//   (Status::media), as its settings said it at the last reading judged to be the guest's: once a minute at a look,
//   and at every question of its media.
//
// On the interface thread, as every driver is (own_device.h).
class ProxmoxDevice : public OwnDevice, public OwnMedia {
    Q_OBJECT

public:
    struct Where {
        QUrl address; // the Proxmox: https, a host, a port where it is not 443, and nothing else
        QString node;
        qint64 vmid = 0;
        QString user; // the API token's ID
        bool knownByCertificate = false;
        QByteArray certificate; // its SHA-256
        // The number it gives for itself, as proxmox::ownNumber keeps it ("proxmox:" and 32 hex); empty: none kept.
        QString machine;
        // The boot order a driver of it replaced and has not put back yet, as it is kept with the guest (OwnKvm:
        // keepBootOrdersBy); one of another guest (another number, or another number it gives for itself) is none.
        std::optional<BootOrderWas> bootOrderWas;
    };
    // Whoever keeps the boot order a driver replaced (BootOrderWas), for the guest: told before the order is written,
    // and again each time it changes (none: put back, or found changed on Proxmox since). A failure to keep it is said,
    // and the order is then not written.
    using BootOrderKeeper = std::function<base::Result<void>(const std::optional<BootOrderWas>&)>;

    // Where the guests of each Proxmox were when its list was last read to find a guest that is not on its node any
    // more (the web's _NODES and _LISTED_FOR): by Proxmox and token, and when each guest last had it read for it. Kept
    // apart from the drivers, which are made anew at every edit: a Proxmox that says "not on this node" at every look
    // does not have its whole list read at every look, but once in a while. Bounded. On the interface thread.
    class Places {
    public:
        static Places& program();
        // The node `vmid` was on at the last reading of the list of `listing`, where that was no longer than `withinMs`
        // ago; and whether its list may be read for `guest` now (no reading for it within `withinMs`), noted as read.
        std::optional<QString> knownNode(const QString& listing, qint64 vmid, qint64 withinMs) const;
        bool mayReadFor(const QString& guest, qint64 withinMs);
        void read(const QString& listing, const QHash<qint64, QString>& nodes);

    private:
        qint64 now() const;
        struct Listed {
            qint64 at = 0;
            QHash<qint64, QString> nodes;
        };
        QHash<QString, Listed> m_listed;
        QHash<QString, qint64> m_readFor;
        QElapsedTimer m_clock;
    };

    // What it is given, made short in a test: the web's times for a guest (proxmox.py, kvm.py).
    struct Pace {
        int connectWithinMs = kvm::letInWithinMs;
        int quietMs = kvm::quietMs;
        int transferMs = kvm::transferMs;
        // A sign-in's answer is waited for longer than another's (the web's WAITS x SIGN_IN_PATIENCE, 8 s x 2.5):
        // Proxmox holds every no back three seconds.
        int signInQuietMs = 20'000;
        int holdMs = 1000; // after a failure it is left alone this long; doubled while it stays away
        int longestHoldMs = 15'000;
        // The first wait after a no, and after a sign-in that came to neither yes nor no (SignInPacing keeps them).
        int refusedHoldMs = 60'000;
        int signInRetryMs = 10'000;
        std::shared_ptr<SignInPacing> pacing; // none: the program's own
        // How long what a look found counts as how it is (LOOK_EVERY: a question that costs Proxmox next to nothing).
        int freshMs = 5000;
        // Rule 30: which virtual machine has the guest's number is asked at a look this often (SAME_GUEST_EVERY), and a
        // process begun this much later than the last look found is one begun again (RESTARTED_BY).
        int sameGuestEveryMs = 60'000;
        int restartedByMs = 10'000;
        // Where a guest went that is not on its node: the list of its Proxmox's guests read again no sooner
        // (RELIST_EVERY); and where those readings are kept: none, the program's own (Places::program()).
        int relistEveryMs = 60'000;
        std::shared_ptr<Places> places;
        // A start or a stop is seen carried out within this from its task's end (CONFIRM_WITHIN), or within this from
        // the press while its task runs or high availability carries it out (TASK_WITHIN), looked for this often
        // (CONFIRM_EVERY); and the guest is looked at again at these moments after a command (FOLLOW_UP). The two
        // together from the press are the wait's bound: no read of it is begun in its last half second or after it
        // (outside review 2, 4.4).
        int confirmWithinMs = 25'000;
        int taskWithinMs = 60'000;
        // The questions a press is decided by (its state, which virtual machine has the number, and a sign-in or a
        // follow to another node of its cluster on their way) are held to this from the press, and its command goes
        // out within it or not at all, a second time after a 401 too (outside review of 0.2.0, 3.8: all bounded from
        // the press, SPEC.md 6.4; the web holds them to no bound); and the command's own answer is waited for until
        // then too, once it went a connection's time and half a second at the least (outside review 2, 4.4). One
        // question's whole.
        int decideWithinMs = kvm::letInWithinMs + kvm::quietMs + kvm::transferMs;
        int confirmEveryMs = 1000;
        QList<int> followUpMs{2000, 5000, 10'000, 20'000, 40'000};
        // Its console (consoleLimits: the web's ProxmoxConsole), and what each live of it is held to.
        RfbScreen::Limits console = consoleLimits();
        ServerLive::Limits live;
    };
    // The web's times for a guest's console (LET_GO_AFTER, UPGRADE_WAIT, CONSOLE_RETRY, MOST_FRAME).
    static RfbScreen::Limits consoleLimits();

    // What it says it is.
    struct Identity {
        QString model;    // "VM 100 on pve1"
        QString version;  // "Proxmox VE 9.2.21"; empty where it said none
        QString hostname; // the guest's name on Proxmox; empty where it has none
        // The number it gives for itself (proxmox::ownNumber; empty where it gives none); none where its settings could
        // not be read just now though one is kept (what it is known by stays as it is: machine()).
        std::optional<QString> number;
    };

    // The words of what is said where another virtual machine has the guest's number (the web's).
    static base::Error anotherGuest();
    static base::Error noNumberGiven();

    ProxmoxDevice(Where where, QString secret, Pace pace, QObject* parent = nullptr);
    ~ProxmoxDevice() override;
    ProxmoxDevice(const ProxmoxDevice&) = delete;
    ProxmoxDevice& operator=(const ProxmoxDevice&) = delete;
    ProxmoxDevice(ProxmoxDevice&&) = delete;
    ProxmoxDevice& operator=(ProxmoxDevice&&) = delete;

    // The mark a list of the guest's images carries (MediaList::listed), of the guest and the node they were listed on:
    // a mount picked from that list names it (MediaMount::listed), and one that names another, or none, is refused,
    // nothing changed (outside review of 0.2.0, 3.5). 32 hex digits, as a server's mark is.
    static QString listedMark(const Where& where, const QString& node);
    // A person has typed this token for this Proxmox just now: it is tried as for the first time, whatever was refused
    // before (SignInPacing::forget), for every guest of that Proxmox.
    static void tryAfresh(const Where& where, const QString& secret, const Pace& pace);
    // Whether a command that ended so may have been done all the same: it went out, and what came of it is not known.
    static bool mayHaveBeenDone(const base::Error& error);
    // What the card says of a token Proxmox does not take (the web's words).
    static base::Error tokenRefused();

    const Where& where() const { return m_where; }
    // What its Proxmox showed the last time a line of this driver's was refused for its certificate (its SHA-256: one
    // that Windows does not trust, or another than it is known by); empty before that. For one who lists its guests.
    QByteArray shownCertificate() const { return m_asker.shownCertificate(); }
    // The number the guest is known by now: what it was made with, or what it took since (knownAs).
    QString machine() const { return m_machine; }
    // Its screen live, for one who asks (what makeLive() hands out does): over the driver's one console. Refused where
    // another virtual machine has the number, or the guest is stopped. Every live of it ends in these words, a turn of
    // the loop later, what is down let go of first (OwnLive::end).
    std::unique_ptr<Live> openLive(Live::Need need);
    void endLives(const base::Error& why);
    // Whether a live of it holds the console's keyboard and mouse now (its turn: ServerLive::hasControl).
    bool keyboardHeld() const;
    // Whoever says the guest is being acted on, and in which words (the wall: a press of a power button, a change of
    // its media): its lives are not given its keyboard and mouse meanwhile (ServerLive::refuseControlWhile; the web's
    // live lines, `lines.busy`: review c9b-4, L1). None: given whenever the console gives them.
    using ActedOn = std::function<std::optional<base::Error>()>;
    void refuseControlWhile(ActedOn actedOn);
    // What it is, for a person who adds it.
    void identify(std::function<void(base::Result<Identity>)> done);
    // The virtual machines the token may see at its Proxmox, for a person who adds them (the web's `listing`); and the
    // number one of them gives for itself (proxmox::ownNumber; empty: none), to tell a card's guest from a new virtual
    // machine under its number (the web's `guests` with numbers_of).
    void listing(std::function<void(base::Result<proxmox::Listing>)> done);
    void numberOfGuest(const QString& node, qint64 vmid, std::function<void(base::Result<QString>)> done);

    Status status() const override;
    bool isStale() const override;
    void look() override;
    void afterLook(int withinMs, std::function<void(bool stillLooking)> then) override;
    void askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done) override;
    void act(const Action& what, std::function<void(base::Result<Done>)> done) override;
    void askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done) override;
    void tidy() override;
    void stopTyping() override;
    std::unique_ptr<OwnLive> makeLive() override;

    // Its virtual media (proxmox_media.cpp; OwnMedia, as LocalWall asks it): a CD-ROM drive, its images on Proxmox.
    // A mount as the web's server and driver take it together (`MediaMount::machine` naming the guest in their words):
    // an image of the storages the token may see on its node, listed just then, picked from a list of that node's
    // (`MediaMount::listed`: listedMark), read-only in its CD-ROM drive; one in
    // it already is said so; and then started from, where that is asked ("once", always restarted; "until_unmounted",
    // restarted where `restart` says so; a restart without a start from it is a power button's, refused): what failed
    // of that is said after "The image is mounted, and <guest>" and how far the start came (not started from it,
    // stopped and not started again, started from it, perhaps), and whether its boot order was put back (review c9b-3,
    // M1). A restart asked is done every time, as the web's (review c9b-4, M1). An unmount names what was in its drive
    // when it was asked: nothing in it, done, with that said; another, refused; and the boot order a start wrote is put
    // back, where it is still that. No image is deleted or uploaded ("invalid").
    //
    // The boot order it replaces is kept with the guest before it is written (BootOrderWas, through the keeper the
    // wall gives: in the guest's stored data), and until it is put back or found changed on Proxmox since: so a start
    // that failed halfway, an answer lost, and a restart of the program leave it to the next unmount, or to the next
    // start "once", to put back. (The web keeps it for "until unmounted" alone, and for as long as it runs.)
    void keepBootOrdersBy(BootOrderKeeper keeper);
    std::optional<BootOrderWas> bootOrderWas() const { return m_bootWas; }
    std::optional<MediaOffer> mediaOffer() const override;
    void askMedia(std::function<void(base::Result<MediaList>)> done) override;
    void mountMedia(const MediaMount& mount, std::function<void(base::Result<MediaDone>)> done) override;
    void unmountMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done) override;
    void deleteMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done) override;
    std::function<void()> uploadMedia(const QString& name, std::unique_ptr<QIODevice> image,
                                      std::function<void(base::Result<MediaUploaded>)> done) override;
    std::optional<MediaUploading> uploading() const override;

signals:
    // The guest was taken for this driver's by the number it gives for itself (none was kept): the number it is known
    // by from now on (`now`), and the one before (`before`, empty). Only ever of this driver's own guest.
    void knownAs(const QString& before, const QString& now);

private:
    using Reply = DeviceAsker::Reply;
    // (What is to be so of the guest's settings, on a reading of them judged the guest's, for a command to go: what is
    // amiss, or nothing. pressButton's, below.)
    using SettingsCheck = std::function<std::optional<base::Error>(const QJsonValue& config)>;
    // A question of Proxmox's API, below /api2/json (`path`), and what this driver does with it beside asking it.
    struct Ask {
        QByteArray method = QByteArrayLiteral("GET");
        QString path;
        // `path` is a step below the guest's place ("status/current"), on the node it is on as each send goes (one sent
        // again after a 401 goes where the guest was followed to meanwhile: review cx3-1, L1).
        bool ofTheGuest = false;
        QUrlQuery query;
        QByteArray body; // a form's fields, where there are any
        qsizetype largest = 0;
        bool vital = true;          // unanswered, the guest counts as away
        bool doesSomething = false; // to the guest: never sent twice, never told "not there" on a guess
        // The moment (this driver's clock) by which its answer is to be in, as far as a question can be held to one:
        // never less than getting a connection may take, never more than its own whole. None: its own whole.
        std::optional<qint64> untilMs;
        // Where it does something and is refused 401, or finds its driver signed out on its way: the moment by which it
        // is sent (again) or not at all (its sign-in and the question which virtual machine has the number held to it;
        // review cx3-3, L1). None: untilMs.
        std::optional<qint64> againUntilMs;
        // ...and what is to be so of the guest's settings for it to be sent again, or after its driver's sign-in on its
        // way, or where it was followed to another node, on the reading of them judged the guest's right before that (a
        // start from the image: that image still in its drive; reviews cx3-2, L2, and cx3-3, L1). None: rule 30 alone.
        SettingsCheck lastCheck;
        // Where it does something: the sign-in (signedInNow's count) the reading of rule 30 it was decided on went out
        // under. It goes only under that very sign-in, or is decided again first on a reading after the one it goes
        // under, as one that finds its driver signed out (a reading a sign-in old decides nothing, also where its
        // driver is signed in again by the time it comes: outside review 2, 4.1). None: no such reading was made.
        std::optional<quint64> decidedUnder;
        // Told the sign-in it goes out under, each time it goes (where it is sent again, the last). None: nobody asks.
        std::shared_ptr<quint64> wentUnder;
    };
    // Told the settings read and judged the guest's, and the sign-in that reading went out under (Ask::decidedUnder).
    using Reading = std::function<void(const QJsonValue& config, quint64 signIn)>;
    struct Pressing;
    struct Tries {
        bool signedInAgain = false; // signed in anew after a 401 to this question
        // ...or, one that does something, on its way, its driver found signed out: asked again after that sign-in
        bool checkedAfterASignIn = false;
    };
    struct SignInOut;

    qint64 now() const { return m_clock.elapsed(); }
    void later(std::function<void()> what);
    SignInPacing& pacing() const;
    SignInPacing::Mark offered() const;
    SignInPacing::Steps steps() const;
    // The token as it goes in the Authorization header, made anew for each question. Nothing of it is written over
    // once a question went (review c9b-1, L1): Qt's request takes the very bytes (setRawHeader shares them, so this
    // driver's copy is no copy of its own to write over), and keeps them, with what Qt's network and Windows' TLS make
    // of it to send it, until Qt lets go of them (SPEC.md section 8: out of reach).
    QByteArray authorization() const;
    // The same for the console's upgrade, as a secret's bytes made in their one place: its way in writes them over as
    // it goes, and the line its own copy once the upgrade went (RfbScreen::WayIn, DeviceLine::Where; after the rebase
    // onto the VNC servers, review c9b-1, L1).
    SecretBytes consoleAuthorization() const;
    // Contact with its Proxmox as the program last found it lost (touchLost): the epoch, the stamp of its address (any
    // failure of contact but a certificate refused, for every guest of it), and the stamp of its address as this
    // driver holds a line there to a certificate (wayOf: its pin, or Windows' trust; a certificate refused, for the
    // guests known the same way alone: fresh review of 4.2, M1).
    struct Touch {
        quint64 epoch = 0;
        quint64 at = 0;
        quint64 way = 0;
        bool operator==(const Touch&) const = default;
    };
    QString wayOf() const;
    Touch touchNow() const;
    // Whether its sign-in stands: the token was taken, and contact with its Proxmox was not lost since what answers
    // there was asked before it (askWhatAnswers: outside review 2, 4.2), by this guest's driver or any other's of that
    // Proxmox (review cx3-3, L3), or for a certificate by one known the same way; and the sign-in noted as standing
    // from now on.
    bool signedIn() const;
    void signedInNow();
    // Signed in, or why not: the sign-in where it is needed and allowed, or another's waited for.
    void signedInFirst(std::function<void(std::optional<base::Error>)> done);
    std::optional<base::Error> heldSignIn() const;
    void signIn();
    void afterTheOtherSignIn();
    void settledByTheOther();
    // A sign-in taken (its own, or another's): over for whoever waits for it, where it stands; where contact was lost
    // while it was out, asked again for them first (outside review 2, 4.2).
    void signedInOrAskedAgain();
    void signInOver(const std::optional<base::Error>& why);
    // What answers on this driver's own line asked what it is, the token in nothing of it (the header's "Right before
    // every sign-in"): `then` once it said it is a Proxmox, which counts for the one sign-in, or the one taking of
    // another's, that follows within a sign-in's time (askedJustNow); else whoever waits for the sign-in is told why
    // not, and where something else answered, it is asked again only once a hold is over (heldAsNoProxmox).
    void askWhatAnswers(std::function<void()> then);
    bool askedJustNow() const;
    std::optional<base::Error> heldAsNoProxmox() const;
    // One question with the token; signed in once more where Proxmox answers it 401.
    void request(const Ask& question, std::function<void(base::Result<Reply>)> done);
    void request(const Ask& question, std::function<void(base::Result<Reply>)> done, Tries tries);
    // A question that does something, about to go after a sign-in it had to wait for (after a 401 to it, or one its
    // driver needed on its way): which virtual machine has the number (rule 30) and what is to be so of its settings
    // (Ask::lastCheck) asked again, on one reading made after that sign-in and within its moment, and the question
    // sent only where both hold; refused otherwise, in the words `notSent` makes of why (reviews cx3-2, L2, and cx3-3,
    // L1).
    void checkedAgain(const Ask& question, std::function<void(base::Result<Reply>)> done, Tries tries,
                      base::Error (*notSent)(const std::string& why));
    // A question of this guest's (Ask::path: its step below the guest's place, "status/current"), its data read
    // (`what`: for the words). Followed once to another node where Proxmox says the guest is not on the one asked, once
    // the virtual machine with its number there is found to be this guest (judge): another one is asked nothing.
    void ofTheGuest(const Ask& question, const QString& what, std::function<void(base::Result<QJsonValue>)> done,
                    bool followed = false);
    // An answer as Proxmox's readers take it: its status, its body and the words of its status line (proxmox::Answer).
    static proxmox::Answer answerIn(const Reply& reply);
    QString guestPath(const QString& step) const;
    QString guestPath(const QString& node, const QString& step) const;
    // Rule 30: which virtual machine has the guest's number, asked afresh and judged (sameGuest); the number the one
    // on `node` gives (numberAt); the judgement itself, on a number seen (judge: nothing where it is this guest);
    // another one noted, said from now on (another).
    // (Each held to `untilMs` where one is given, as Ask::untilMs: a press's, or its wait's.)
    // (And told the settings read and judged the guest's, where it is, with the sign-in that reading went under:
    // `read`; numberAt tells the settings it read, for whoever judges the number in them.)
    void sameGuest(bool vital, std::function<void(base::Result<void>)> done,
                   std::optional<qint64> untilMs = std::nullopt, Reading read = {});
    void numberAt(const QString& node, std::function<void(base::Result<QString>)> done,
                  std::optional<qint64> untilMs = std::nullopt, Reading read = {});
    std::optional<base::Error> judge(const QString& seen);
    base::Error another(const base::Error& words);
    // The node the guest is on, as its Proxmox's list says it; none where it is on none the token can see, or the list
    // was read for it a short while ago.
    void whereNow(std::optional<qint64> untilMs, std::function<void(base::Result<std::optional<QString>>)> done);
    void listingBy(std::optional<qint64> untilMs, std::function<void(base::Result<proxmox::Listing>)> done);
    Places& places() const;
    // Whether a look found it otherwise than the last did in the way a virtual machine made under its number mostly is
    // (the web's `_anew`); and the moment its process began, as a look that found it running tells (`_began_again`).
    bool anew(const proxmox::Current& current) const;
    void beganAgain(const proxmox::Current& current);
    void seen(const proxmox::Current& current);
    static Status anotherOnTheCard(const base::Error& why);
    // A press of a power button, in steps: the state and which virtual machine has the number; the command; the wait
    // for it to be carried out (a look, and its task asked how it stands); what came of it, handed on the program's
    // loop. Looked at again at the moments after a command (followUp).
    // (A press whose command goes only where `lastCheck` finds nothing amiss in the settings the press itself read
    // and judged the guest's, and in those read again before it is sent a second time after a 401, or to the node it
    // was followed to: a start from the image, that image still in its drive; outside review of 0.2.0, 3.4, and reviews
    // cx3-1, L5, and cx3-2, L2. None: a power button's.)
    void pressButton(const QString& kind, SettingsCheck lastCheck, std::function<void(base::Result<Done>)> done);
    void press(const std::shared_ptr<Pressing>& pressing, bool running);
    void carriedOut(const std::shared_ptr<Pressing>& pressing);
    void judgeTheWait(const std::shared_ptr<Pressing>& pressing);
    void taskOf(const std::shared_ptr<Pressing>& pressing, std::function<void(std::optional<QString>)> done);
    void pressed(const std::shared_ptr<Pressing>& pressing, const base::Result<Done>& result);
    void followUp();
    // Its console (proxmox_screen.cpp): made when it is first wanted; the way into one session (a ticket, asked for
    // right after which virtual machine has the number is asked); what it showed, handed to every live of it; why it is
    // not shown, and until when; whether its screen may be had now, as the last look found it; keys and a text.
    ServerConsole& screen();
    void rfbWay(std::function<void(base::Result<RfbScreen::WayIn>)> done);
    void shownOnTheScreen(const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed);
    void notedOnTheScreen(const base::Error& why);
    std::optional<base::Error> noScreen() const;
    bool hasScreen() const;
    std::optional<base::Error> screenAllowed() const;
    void typeOnTheScreen(const Action& what, std::function<void(base::Result<Done>)> done);
    // Its virtual media (proxmox_media.cpp): the guest's settings read now and judged to be its (rule 30), what every
    // question of its media begins with, and what is in its drive taken from them; what is in its drive as the last
    // such reading found it (nothing while another virtual machine has the number); the images of the storages of its
    // node; and a change of its settings, made on those read for it (their digest), never sent twice.
    void thisGuest(std::function<void(base::Result<QJsonObject>)> done);
    void driveFrom(const QJsonValue& config);
    std::optional<MountedMedia> mediaNow() const;
    // The images listed on the node the guest is on now, and that node: one bound for its storages (one question's
    // whole time from their list on), the storages not asked by then said in `unseen` (review c9b-2, L3), one asked
    // before it held to it but given a connection's time and half a second at the least (review c9b-3, L1: SPEC.md 6.4
    // says the whole bound, the questions before the storages included).
    struct Gathering;
    void listMedia(std::function<void(base::Result<MediaList>, const QString& node)> done);
    void imagesOn(const std::shared_ptr<Gathering>& gathering);
    void changeSettings(const QJsonObject& config, const QList<std::pair<QString, QString>>& changes,
                        const QString& what, std::function<void(base::Result<void>)> done);
    // The change of a mount, on the node its images were listed on (`listedOn`), or none (review c9b-2, L2).
    void put(const MediaMount& mount, const QString& listedOn, std::function<void(base::Result<MediaDone>)> done);
    // Starting the guest from the image in its drive (the web's media_boot): its boot order with the drive first, kept
    // first (keepBootOrder), and the rest (bootFrom); what failed of it said with how far it came (bootFailed); the
    // order put back where it is still the one written (orderBack: left as it is where it was changed on Proxmox
    // since); the guest started anew, stopped first where it runs (restartGuest: the web's _restart, each a press of a
    // power button). Only from the image confirmed (Boot::image): another in its drive when the start begins, or right
    // before the guest is started (looked at before the start, and on the start's own reading of its settings:
    // pressButton's check), is not started from (outside review of 0.2.0, 3.4; review cx3-1, L5). The boot order kept
    // for this guest (none: of another, or none kept); and kept anew.
    struct Boot;
    struct Restarted {
        enum class Came { Started, NotDone, MayHaveStopped, Stopped, MayHaveStarted, ByAnother };
        Came came = Came::Started;
        bool stopped = false; // stopped first, by this program
        base::Error why;
    };
    enum class Back { PutBack, ItsOwnAlready, ChangedMeanwhile };
    void mediaBoot(const QString& how, bool restart, const QString& named, const QString& image,
                   std::function<void(base::Result<QString>)> done);
    void bootAnew(const std::shared_ptr<Boot>& boot);
    void bootFrom(const std::shared_ptr<Boot>& boot);
    void bootDone(const std::shared_ptr<Boot>& boot, const Restarted& restarted,
                  const std::optional<base::Result<Back>>& back);
    static void bootFailed(const std::shared_ptr<Boot>& boot, const QString& came, base::Error why,
                           const QString& after);
    void orderBack(const QStringList& order, const QStringList& written, std::function<void(base::Result<Back>)> done);
    void restartGuest(const QString& image, std::function<void(const Restarted&)> done);
    std::optional<BootOrderWas> keptBootOrder() const;
    base::Result<void> keepBootOrder(const std::optional<BootOrderWas>& was);
    void showed(const QByteArray& der);
    void answered();
    base::Error failed(const base::Error& error);
    // Where contact with what answers at its address failed (a question's line, of any question, or its console's):
    // signed in no more, so that the next question with the token asks what answers there first (review cx3-2, L1).
    void outOfTouch(const base::Error& why);
    std::optional<base::Error> heldBack() const;
    void lookFailed(const base::Error& why);
    void lookOver(Status status);

    Where m_where;
    // The token's secret, for as long as it signs in with it: its own, written over when it goes (wiped.h).
    SecretText m_secret;
    Pace m_pace;
    DeviceAsker m_asker;
    QElapsedTimer m_clock;

    // Signed in: the token was taken, and has not been refused since, nor has contact with what answers at its address
    // failed since (outOfTouch); a sign-in under way, and who waits for it; the sign-in that is out (none: none is);
    // what came of the last that let it nowhere, and the hold it noted then.
    bool m_signedIn = false;
    // ...and when contact with its Proxmox was last found lost when it signed in, by every guest's driver of it
    // (signedIn: touchNow as it stood then).
    Touch m_touched;
    // How many times it was signed in (its own sign-in, or another's taken): what a reading went out under, and what a
    // command decided on it is held to (Ask::decidedUnder; outside review 2, 4.1).
    quint64 m_signIns = 0;
    bool m_signingIn = false;
    QList<std::function<void(std::optional<base::Error>)>> m_waitingForSignIn;
    std::shared_ptr<SignInOut> m_signInOut;
    std::optional<base::Error> m_signInError;
    std::optional<SignInPacing::Kept> m_heldAs;
    QString m_version; // Proxmox's own, as it said it at the sign-in
    bool m_toldItsCertificate = false;
    // When what answers on its line last said it is a Proxmox, for the sign-in that follows (none: not since the last
    // one); and where something else answered, its words, how many times in a row, and until when it is not asked
    // again.
    std::optional<qint64> m_askedAt;
    // ...and contact with its Proxmox as it stood when that question went (touchNow): the sign-in after it stands on
    // that, so that contact lost while the two were out has it ask again (outside review 2, 4.2).
    Touch m_askedTouch;
    std::optional<base::Error> m_noProxmox;
    int m_noProxmoxTimes = 0;
    qint64 m_noProxmoxUntil = 0;

    // How it was found, and when; failures in a row that found it away, and until when it is left alone.
    std::optional<Status> m_status;
    qint64 m_statusAt = 0;
    bool m_looking = false;
    int m_failures = 0;
    qint64 m_downUntil = 0;
    std::optional<base::Error> m_downError;

    // Rule 30: the number it is known by; another virtual machine that has its number now (what is said meanwhile);
    // when which one has it was last asked and judged, and how often (a look that asked on its way, following the guest
    // to another node, does not ask twice: counted, not told by the time); what the last look found of its power, and
    // when its process began (this driver's clock); whether its move to another node was said in the log.
    QString m_machine;
    std::optional<base::Error> m_other;
    std::optional<qint64> m_judgedAt;
    int m_judges = 0;
    std::optional<bool> m_powerOn;
    std::optional<qint64> m_beganAt;
    bool m_movedSaid = false;
    // What its CD-ROM drive holds, as the last reading of its settings judged to be the guest's found it; the boot
    // order it replaced and has not put back yet, and who keeps that with the guest (none: this driver alone).
    std::optional<MountedMedia> m_drive;
    std::optional<BootOrderWas> m_bootWas;
    BootOrderKeeper m_keepBoot;

    // The presses under way, and the moments it is to be looked at again after a command.
    QList<std::shared_ptr<Pressing>> m_pressing;
    QList<qint64> m_followUpAt;

    // Its console, the lives of it, why its screen is not shown and until when; the frames it showed, and the card's
    // still last made: of which frame, at which width.
    std::unique_ptr<ServerConsole> m_screen;
    QList<QPointer<ServerLive>> m_lives;
    ActedOn m_actedOn;
    std::optional<base::Error> m_noScreen;
    qint64 m_noScreenUntil = 0;
    quint64 m_framesShown = 0;
    struct MadeStill {
        quint64 frames = 0;
        int tier = 0;
        QByteArray picture;
    };
    std::optional<MadeStill> m_still;
};

} // namespace qkvm::core
