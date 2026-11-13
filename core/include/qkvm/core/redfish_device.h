#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

#include <QByteArray>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/device_asker.h>
#include <qkvm/core/kvm_times.h>
#include <qkvm/core/own_device.h>
#include <qkvm/core/redfish_answers.h>
#include <qkvm/core/server_live.h>
#include <qkvm/core/server_screen.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/wiped.h>

namespace qkvm::core {

class SetupOverrides;

// Where the sessions of the controllers whose drivers were let go of are given back: a controller
// has only so many (four or so), and one left behind uses one up until it runs out by itself, half
// an hour later. Each is given back on a line of its own, held to the certificate its controller is
// known by, and let go of at the answer or after a few seconds, whichever comes first. And a sign-in
// that was still out when its driver was let go of is waited for here, within its own time: a
// session it brings is given back as any other (a box closed during a slow sign-in, a few times over,
// would otherwise shut the owner out of their own controller's page for half an hour). Nobody waits
// for any of it while the program runs: a driver that is let go of goes at once. At the program's end
// they are waited for, within their time (finish), as the web waits for each device's close at its
// shutdown. And before a session is given back, what a start into BIOS setup under way when its
// driver went may have left on the server is cleared under it (clearSetupOverride, review 4's
// MEDIUM 1). On the interface thread.
class ControllerGoodbyes : public QObject {
    Q_OBJECT

public:
    // The way a goodbye comes from another thread (a console way's done, from whichever thread its screen
    // lives on): handed to these goodbyes' own loop while they are there, under a lock their going takes
    // too. (A pointer to them, read on another thread, could be to goodbyes that were going that very
    // moment.) Where they are gone, or the program's end was waited for already (finish), it says so: that
    // goodbye may never be said.
    class Door {
    public:
        // false: they are gone, and nothing was handed on.
        bool leaveWebPage(const DeviceAsker::Where& where, int connectWithinMs, const QByteArray& session,
                          int withinMs);

    private:
        friend class ControllerGoodbyes;
        std::mutex m_lock;
        ControllerGoodbyes* m_goodbyes = nullptr; // none: gone
        bool m_finished = false;                  // the program's end was waited for (finish)
    };

    explicit ControllerGoodbyes(QObject* parent = nullptr);
    ~ControllerGoodbyes() override;
    ControllerGoodbyes(const ControllerGoodbyes&) = delete;
    ControllerGoodbyes& operator=(const ControllerGoodbyes&) = delete;
    ControllerGoodbyes(ControllerGoodbyes&&) = delete;
    ControllerGoodbyes& operator=(ControllerGoodbyes&&) = delete;

    // The program's own, for every driver that is given no other: gone with the program's
    // application object.
    static ControllerGoodbyes& program();

    // The door to these, for any thread.
    std::shared_ptr<Door> door() const { return m_door; }

    // Gives back the session `token` at `place` (a path on the controller at `where`), within
    // `withinMs` in all.
    void giveBack(const DeviceAsker::Where& where, int connectWithinMs, const QString& place, const QByteArray& token,
                  int withinMs);
    // ...and a session of its own web page (AMI's: DELETE /api/session), its cookies and the token beside them as
    // DeviceAsker::Carried::In::CookiesAndHeader has them.
    void leaveWebPage(const DeviceAsker::Where& where, int connectWithinMs, const QByteArray& session, int withinMs);
    // Takes over a sign-in's line that is still out (`line`, made for that sign-in alone): it is this
    // one's from now on, and goes when its asking is over (whoever made it lets it go then) or with this.
    void waitFor(DeviceAsker* line);
    // What a start into BIOS setup that was under way when its driver was let go of may have left (review
    // 4's MEDIUM 1): the server's one-time boot override cleared again under the driver's session, on a
    // line of its own, and read back, all within `withinMs`; then that session given back (giveBack),
    // where `sessionPlace` names one. Nothing is changed where the computer read is not the server the
    // driver knew by its number, or shows another tool's override. `then`, told once on this thread:
    // whether the override was seen off ("Disabled" in so many words, or another tool's in its place),
    // and whether a read before the clearing showed this program's own set (the change that was out had
    // been made, and none of it can come after the clearing).
    struct SetupClearing {
        DeviceAsker::Where where;
        int connectWithinMs = 0;
        QString system; // the computer's place
        QByteArray token;
        QString machine; // the mark the driver knew the server by (RedfishDevice::Where::machine)
        int withinMs = 0;
        int againEveryMs = 0; // read back this often while it does not show off
        QString sessionPlace;
        int signOutWithinMs = 0;
        std::function<void(bool off, bool oursFirst)> then;
    };
    void clearSetupOverride(SetupClearing clearing);
    // Waits, the loop running, until nothing is out any more, or `withinMs` has passed (longer where a
    // clearing is out: its time, and the goodbye of its session after it): for the program's end, while
    // its loop still runs (main.cpp, at aboutToQuit). At once where nothing is out. What came through the
    // door before it is said first. A goodbye after it says that it may never be said (the loop is
    // ending), and goes all the same (a test's loop runs on).
    void finish(int withinMs);
    // How many sessions were handed to this to be given back, and how many lines are still out (the
    // sign-ins waited for among them).
    int given() const { return m_given; }
    int underWay() const { return m_underWay; }

signals:
    // Nothing is out any more.
    void allGone();

private:
    struct Clearing;
    void counted(QObject* line);
    // Whether the program's end was waited for already (finish).
    bool finished() const;
    // leaveWebPage, from the door or from here, without a word.
    void leave(const DeviceAsker::Where& where, int connectWithinMs, const QByteArray& session, int withinMs);
    // A clearing in steps: the computer read and judged (`meant`: the change goes as meant whatever the state); the
    // change; read back; its end, the session given back after it. The computer as an answer shows it, where it is
    // the server the clearing is for.
    void readBeforeClearing(const std::shared_ptr<Clearing>& state, bool meant);
    void sendClearing(const std::shared_ptr<Clearing>& state, const QByteArray& mark, bool meant);
    void readClearingBack(const std::shared_ptr<Clearing>& state);
    void clearingOver(const std::shared_ptr<Clearing>& state, bool off);
    static std::optional<QJsonObject> systemIn(const Clearing& state, const base::Result<DeviceAsker::Reply>& reply);

    int m_given = 0;
    int m_underWay = 0;
    std::shared_ptr<Door> m_door;
    // When the last of the clearings out ends, the goodbye of its session after it included (by m_clock); and whether
    // the program's end waits for what is out just now (finish: what is handed in then is waited for too).
    QElapsedTimer m_clock;
    qint64 m_endsBy = 0;
    bool m_waiting = false;
};

// The driver of a server's management controller (BMC) on this PC's own wall (M7), over Redfish, in
// the same words as a kvmd's (kvmd_device.h) and a JetKVM's (jetkvm_device.h) wherever they say the
// same thing. Its reference is the server's driver (../app/qkvm/redfish.py).
//
// * It is asked over HTTPS or plain HTTP, as its address is: held to the certificate it is known by,
//   straight to it whatever proxy this PC uses, with a limit on every answer (DeviceAsker). A
//   controller is a small, often slow computer: it is waited for longer than a KVM (Pace).
// * It is signed in to with a Redfish session, and in no other way: POST
//   /redfish/v1/SessionService/Sessions with the user and the password as JSON. 200 or 201 with a
//   token (X-Auth-Token, the whole of it one that may go back in a header) is in, and that token goes
//   back in that header with every question from then on: never a cookie, and never an
//   Authorization (Basic would send the password with every question). Where to sign out again is the
//   path of the place it names for the session (Location), where that is a place of its own
//   (redfish::link); one elsewhere is not signed out of at all.
// * A refused password (401, 403) is offered again as SignInPacing says, as a kvmd's is: a
//   controller locks an account after a few. A sign-in that came to neither yes nor no (every
//   session it has is given out: 503; it is no controller; a token that is none) is said as what
//   stands ("no_sign_in"), and not sent again at every look either. With no password saved
//   nothing is sent at all ("no_password").
// * A question under the session answered 401: signed in once more (the session ran out, or the
//   controller restarted); answered 401 again: "keeps rejecting the session", and not signed in to
//   again for a while, by any driver of it, longer each time it does it again (SignInPacing, the road
//   "session"). A session let go of so is given back all the same. A read that is not vital (its
//   sensors, its version) answered 401 under the session the read before was answered under is that
//   resource refused, and not the session (an older board keeps its sensors from an operator): the
//   session is kept, and that place is not asked again until where it keeps things is found anew.
// * Let go of, it gives its session back (ControllerGoodbyes), unless it has none, or its controller
//   was away the last time it was asked; where a start into BIOS setup under way may have set the
//   server's boot override, that is cleared under the session first, on the way out (redfish_setup.cpp).
//   A sign-in it has out then goes on without it (each goes on a
//   line of its own): what it comes to is told to SignInPacing as this driver would have told it, and a
//   session it brings is given back.
// * Which server answers (the web's `_judge`): a controller's maker may have put one certificate, and
//   one key, on every board, and a fleet often has one password. So a server is known by the numbers it
//   gives for itself, kept as digests (Where::machine, redfish::marksOf): its computer's (its UUID),
//   asked under the session before anything is done to it, and its controller's (its service root's
//   UUID, which anybody may read), asked before the password goes. Another server at its address, or
//   one that says none where one was kept, is "another_machine" in the web's words, and nothing is sent
//   to it; the password not either, where the controller's number tells it. Where nothing is kept, the
//   numbers seen are taken (trust on first use, as a certificate is), and said (knownAs); so is the
//   controller's number where only the computer's was kept, learnt under the session the computer was
//   found to be this one under.
// * What it is (identify, for a person who adds it): the model of its computer, the version of the
//   controller's firmware, the name the computer goes by, and the mark it is known by after the asking
//   (what was kept, or what was taken then), to know it by at any address.
// * How it is (look, the web's `_read_status`): its computer, read and judged at every look; its
//   sensors in either of Redfish's shapes (redfish_answers.h); its controller's version, asked once in
//   ten minutes. A look that fails for one of the controller's moments leaves the last good one standing
//   for a while (hiccupMs), and only such a look is followed sooner (retryLookMs); never one that is not
//   let in, another certificate, or another server. What was read under a session other than the one
//   its server was judged under is not taken; another server found during a look ends it as that.
//   A critical fault that went away is still told of for a while
//   (faultHoldMs). While a refused password waits, its root is asked with no session, now and then, to
//   tell "away" from "refused" (the web's `_gone_meanwhile`).
// * Its server's own screen, from the controller's console (AMI's firmware: ServerScreen, M7 1c), one console
//   for all of it (the owner's decision, 2026-10-29): the card's still (askSnapshot, made smaller as the web
//   makes its JPEGs: base/screen_still.h), the screen live for whoever shows it large (makeLive: ServerLive, with
//   keyboard and mouse as the console allows), and keys and a text from the menu (act: "keys", "text"; never
//   the keyboard's Power key). Its status says `screen` and `live` where it is not known to have no console, and
//   `keys`, `text` and `control` once its console is known (the web's `can`); why its screen is not shown, for
//   as long as that stands (somebody else at the console, its server's monitor going dark while a console is
//   open: ServerReadings::noScreen, the web's no_screen). Nothing of it goes to another server at its address.
//   Its frames are read, and the card's still made smaller, on a thread of their own (ServerScreen's, review 3's
//   HIGH 1): this driver's thread (the interface's) only takes them off the console's line. It wakes no computer.
// * Its power buttons and its controller's restart (act, the web's `act`): by what is wanted (power_on
//   and power_off leave alone a server that is so already, "already on"), the kinds the controller offers
//   (a power cycle stands in for the reset button, nothing else for another), the computer judged first and
//   the command sent under that very session; the controller's verdict (only a 4xx is a no); a switch on or
//   a cut of the power confirmed by seeing it (CONFIRM_WITHIN); a controller's restart whose line drops is
//   done; looked at again at the moments after (FOLLOW_UP). Anything else is "invalid" before anything goes.
// * Its light to find the server by in a rack (locate_on, locate_off, the web's `_locate`): lit rather than
//   set blinking (redfish::locator), changed under the session the computer was judged under, the controller
//   shown the mark it put on the state the change was decided on (If-Match), and told the change is meant
//   whatever the state ("*") where it will not take that mark back, or gave none and wants one (412, 428).
//   One that has no light it says how to switch: "action_failed", and nothing sent.
//
// On the interface thread, as every driver is (own_device.h).
class RedfishDevice : public OwnDevice {
    Q_OBJECT

public:
    struct Where {
        QUrl address; // http or https, a host, a port where one was typed, and nothing else
        QString user = QStringLiteral("admin");
        bool knownByCertificate = false;
        QByteArray certificate; // its SHA-256
        // The mark the server is kept by: the numbers its computer and its controller gave for
        // themselves (redfish::marksOf); empty where none is kept.
        QString machine;
    };

    // What it is given, made short in a test: the web's times for a controller (redfish.py).
    struct Pace {
        int connectWithinMs = kvm::letInWithinMs; // to be let in at all
        // The longest nothing may come while an answer is waited for: an older board took nine to
        // twelve seconds to show the root of its Redfish service (the web's BMC_WAITS).
        int quietMs = 20'000;
        int transferMs = kvm::transferMs; // and how much longer than that a whole answer may take
        // The longest nothing may come while a sign-in's answer is waited for: as for every family's
        // (the web's WAITS x SIGN_IN_PATIENCE, 8 s x 2.5), some devices hold a no back on purpose.
        int signInQuietMs = 20'000;
        // Its service root, read with no session before a sign-in: twice the quiet (ROOT_WAIT_TIMES).
        int rootUnsignedMs = 40'000;
        int holdMs = 1000; // after a failure it is left alone this long; doubled while it stays away
        int longestHoldMs = 15'000;
        // As a kvmd's (KvmdDevice::Pace): the first wait after a no, and after a sign-in that came to
        // neither yes nor no; SignInPacing keeps them, outside this driver. The second is also how long
        // a session it keeps refusing is not signed in to again (the web's LOGIN_RETRY).
        int refusedHoldMs = 60'000;
        int signInRetryMs = 10'000;
        // Where they are kept: none, the program's own (SignInPacing::program()).
        std::shared_ptr<SignInPacing> pacing;
        // How long what a look found counts as how it is (LOOK_EVERY), and after a look that failed
        // (RETRY_LOOK); how long a look that fails for a moment leaves the last good one standing
        // (HICCUP); how long a critical fault that went away is still told of (FAULT_HOLD).
        int freshMs = 10'000;
        int retryLookMs = 4000;
        int hiccupMs = 45'000;
        int faultHoldMs = 300'000;
        // After something was done to it, it is looked at again at these moments (FOLLOW_UP); a power
        // command is confirmed within this, looked for this often (CONFIRM_WITHIN, CONFIRM_EVERY).
        QList<int> followUpMs{2000, 4000, 6000, 8000, 11'000, 15'000};
        int confirmWithinMs = 25'000;
        int confirmEveryMs = 2000;
        // Its session given back within this, all in all (SIGN_OUT_WITHIN).
        int signOutWithinMs = 5000;
        // Where it keeps things (its computer, chassis and controller) is asked again after this (ABOUT_TTL);
        // and its controller's version, after this where it could not be read (ABOUT_RETRY).
        int placesFreshMs = 600'000;
        int aboutRetryMs = 60'000;
        // In the newer shape every fan is a read of its own: the sensors get this much of one look
        // (SENSORS_WITHIN), and a fan not read in it keeps what was last read of it.
        int sensorsWithinMs = 15'000;
        // Where they are given back: none, the program's own (ControllerGoodbyes::program()).
        std::shared_ptr<ControllerGoodbyes> goodbyes;
        // While a password waits to be offered again, how often it is asked whether it is there at all.
        int stillThereEveryMs = 30'000;
        // Which server answers, asked for its console: where a number is kept, asked through one of its moments so
        // often, so far apart (QUESTION_TRIES, QUESTION_AGAIN).
        int questionTries = 3;
        int questionAgainMs = 1000;
        // Its firmware inventory, where its version is looked for when its manager says none: no read of it begins
        // after this, from the first (IMAGES_WITHIN).
        int imagesWithinMs = 10'000;
        // Its console, for the card's still and for whoever has it live: what its screen and each live are held to
        // (the web's CONSOLE_IDLE and the rest: ServerScreen::Limits; ServerLive::Limits).
        ServerScreen::Limits screen;
        ServerLive::Limits live;
        // The seats of the controllers' consoles, one for every driver of a controller: none, the program's own
        // (ConsoleSeats::program()).
        std::shared_ptr<ConsoleSeats> seats;
        // Start into BIOS setup (redfish_setup.cpp; the web's SETUP_WITHIN, the page's SETUP_PATIENCE, BOOT_WAITS): its
        // restart goes out within this of the press, or not at all; the press is given this long in all (four minutes,
        // as the page waits for it), and an override the server has not taken by then is cleared again; the change of
        // the override, the restart and the clearing each wait this long for their answers (a real board took 19.8 s
        // over the change, and answered no read for a minute after it).
        int setupWithinMs = 120'000;
        int setupPatienceMs = 240'000;
        int bootQuietMs = 60'000;
        // A start into BIOS setup under way when this driver is let go of (the program's end among it, review 4's
        // MEDIUM 1): its override cleared by the goodbyes and read back within this in all, before the session is given
        // back (the change took a real board 19.8 s); and where which servers' override may be set is kept beside the
        // settings: none, the program's own (SetupOverrides::program()).
        int setupClearWithinMs = 30'000;
        std::shared_ptr<SetupOverrides> overrides;
    };

    // The way into one session of the controller's console (AMI's firmware), as ServerScreen::WayIn takes it
    // (server_screen.h), field for field: where the controller is and what it is known by; its web page's session,
    // as the Cookie header says it; the pass its web page handed out for one console session; this PC's address and
    // the controller's as it names them, and the number of the web page's session; whether its firmware lets a
    // console reconnect (none: not known). `done`, called once, from any thread, when the console session is over:
    // the web page's session is given back then (ControllerGoodbyes::leaveWebPage).
    struct ConsoleWay {
        QUrl address;
        bool knownByCertificate = false;
        QByteArray certificate;
        QByteArray cookie;
        QByteArray pass;
        QByteArray ownAddress;
        QByteArray controllerAddress;
        QByteArray webSession;
        std::optional<bool> completeFirst;
        std::function<void()> done;
    };

    // What it says it is.
    struct Identity {
        QString model;    // its computer's, or its maker's; empty where nobody filled it in
        QString version;  // of the controller's firmware, as it says it; empty where it says none
        QString hostname; // the name its computer goes by; empty where it says none
        // Its computer's own mark ("server:" and a digest of its number: the web's `ident`); empty where it
        // gives none and the server is known by its controller alone, or by nothing; none where it gives none
        // now though one is kept (what it is known by stays as it is: machine()).
        std::optional<QString> number;
    };

    RedfishDevice(Where where, QString password, Pace pace, QObject* parent = nullptr);
    ~RedfishDevice() override;
    RedfishDevice(const RedfishDevice&) = delete;
    RedfishDevice& operator=(const RedfishDevice&) = delete;
    RedfishDevice(RedfishDevice&&) = delete;
    RedfishDevice& operator=(RedfishDevice&&) = delete;

    // A person has typed this password for the controller at this place just now: it is tried as for the
    // first time, whatever was refused before (SignInPacing::forget).
    static void tryAfresh(const Where& where, const QString& password, const Pace& pace);
    // Whether a command that ended so may have been done all the same: it went out, and what came of it
    // is not known ("timeout", "dropped": the web's server counts both so).
    static bool mayHaveBeenDone(const base::Error& error);

    // What it is, for a person who adds it: signs in to ask.
    void identify(std::function<void(base::Result<Identity>)> done);
    // A way into the controller's console (part 3a; the web's `_hold_console` up to its console's line). Its own web
    // page is signed in to with the same password: a second road the password takes, paced on its own (SignInPacing's
    // "web"), and taken only once the controller has let this program in the other way. Which server answers is asked
    // before the password goes there, and again before the pass is asked for: another server's web page is not
    // offered the password, and its console's pass is not handed on. Its web page's session is given back on every
    // road where no way is handed (and by the way's `done` where one is). Answered once, a turn of the loop later at
    // the soonest; "stopped" where this driver is let go of first. "no_picture" where asking again does not change it
    // soon (no console here; its web page refused the password; its server's monitor goes dark while a console is
    // open).
    void consoleWay(std::function<void(base::Result<ConsoleWay>)> done);
    // The mark the server is known by now: what it was made with, or what it took since (knownAs).
    QString machine() const { return m_machine; }
    // Its server's screen live, for one who asks (what makeLive() hands out does): over the driver's one screen.
    // Refused where another server answers at its address, or it has no console here.
    std::unique_ptr<Live> openLive(Live::Need need);
    // Every live of it ends in these words, a turn of the loop later, what is down let go of first (OwnLive::end).
    void endLives(const base::Error& why);

    Status status() const override;
    bool isStale() const override;
    void look() override;
    void afterLook(int withinMs, std::function<void(bool stillLooking)> then) override;
    void askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done) override;
    void act(const Action& what, std::function<void(base::Result<Done>)> done) override;
    void askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done) override;
    void tidy() override;
    std::unique_ptr<OwnLive> makeLive() override;

    // The words of what is said where another server answers at its address (the web's).
    static base::Error anotherServer();
    static base::Error noServerNumber();
    static base::Error anotherController();
    static base::Error noControllerNumber();
    // What is said of its picture: a controller has none here.
    static base::Error noPicture();

signals:
    // The server at its address was taken for this driver's (nothing was kept of it), or its controller's
    // number learnt beside its computer's: the mark it is known by from now on (`now`), and the one it was
    // known by before (`before`; empty where none was). Only ever of this driver's own server.
    void knownAs(const QString& before, const QString& now);

private:
    using Reply = DeviceAsker::Reply;
    // A question as every family's driver asks one (device_asker.h), and what this driver does with it
    // beside that.
    struct Ask : DeviceAsker::Ask {
        Ask();
        bool vital = true; // unanswered, the controller counts as away
        // A detail of how it is (its sensors, its version, where it keeps them): one the account may be refused while
        // the session is honoured. Never its computer itself, nor its root.
        bool detail = false;
        // It changes something on the server: it goes only under the session under which the computer was last
        // found to be this one's (another server at the address does not know that session).
        bool command = false;
    };
    struct Tries {
        bool signedInAgain = false; // signed in anew after the controller refused the session
        int asked = 0;              // times which server answers was asked before a command (MOST_ASKED)
        bool refused = false;       // a command the controller answered 401 under its session (not done)
    };
    struct Pressing;
    struct Setup;
    struct WayAsking;
    struct WebSignIn;
    class HeldWebSession;
    // The session it was given: its token, and where to sign out of it again (none: nowhere).
    struct Session {
        QByteArray token;
        QString place;
    };
    struct Identifying;
    struct SignInOut;
    struct FanWalk;
    struct ImageWalk;
    // Where this controller keeps the server, its chassis and itself (the web's `_find`), and its root.
    struct Places {
        QJsonObject root;
        QString system;
        QString chassis;
        QString manager;
        // Where its chassis keeps its sensors: the older shape (thermal), or the newer (metrics, fans).
        QString thermal;
        QString metrics;
        QString fans;
        // Its UpdateService, where its firmware inventory is (none: it names none).
        QString update;
    };
    // A fan of the newer shape as last read: when (this driver's clock), and the fan (none: what is there is
    // no fan that is fitted).
    struct FanRead {
        qint64 at = 0;
        std::optional<redfish::Fan> fan;
    };
    // What a sign-in came to, as SignInPacing was told it: in, refused, neither yes nor no (an answer of
    // another kind, or none after it went out), or not sent at all.
    struct SignInCame {
        enum class Kind { In, Refused, Neither, NotSent };
        Kind kind = Kind::NotSent;
        Session session;
        base::Error why;
        bool compared = false; // the controller's number was compared just before it
        QJsonObject root;      // its root, read on the sign-in's own line just before it (none: not read)
    };
    // Reads what came of a sign-in that was out (`out`), and tells the pacing so: for this driver, and for
    // one that was let go of while it was out.
    // `late`: its driver was let go of meanwhile. What came to nothing then is noted only while something
    // still stands for that password: another driver may have been let in with it since.
    static SignInCame cameOf(const SignInOut& out, const base::Result<Reply>& reply, bool late);
    // What came of a sign-in that was out when its driver was let go of: told to the pacing, and a
    // session it brought given back.
    static void cameAfterwards(const std::shared_ptr<SignInOut>& out, const base::Result<Reply>& reply);

    qint64 now() const { return m_clock.elapsed(); }
    void later(std::function<void()> what);

    // The session to ask with: signs in where that is needed and allowed.
    void withToken(std::function<void(base::Result<QByteArray>)> done);
    void signIn();
    // Another driver of this controller has the same password out, and nothing else stands: what it comes to is
    // waited for, and then looked at again; and whether that may be done now.
    void afterTheOtherSignIn();
    bool mayWaitForTheOther() const;
    // Where the controller's number is kept: its service root read with no session, and its number compared,
    // before the password goes (the web's `_same_controller`), on the line the sign-in then goes on: whether it
    // was compared.
    void sameController(DeviceAsker& line, std::function<void(base::Result<bool>)> done);
    void sendSignIn(DeviceAsker* line, bool compared);
    // Its service root, which anybody may read, read on `line` with no session; "bad_response" where what
    // answered is no service root just then.
    void askRoot(DeviceAsker& line, bool vital, std::function<void(base::Result<QJsonObject>)> done);
    void signInCame(const SignInCame& came);
    void signedIn(const base::Result<QByteArray>& token);
    SignInPacing& pacing() const;
    SignInPacing::Mark offered() const;
    // The sessions it gives to this password, as the pacing keeps a controller's refusing them (road "session").
    SignInPacing::Mark sessionsGiven() const;
    SignInPacing::Steps steps() const;
    // What stands against offering the password now, noted by any driver of this controller: a no, a sign-in that
    // came to nothing; and the sessions it gives refused again and again.
    std::optional<base::Error> heldSignIn() const;
    std::optional<base::Error> heldSessions() const;
    // A sign-in came to neither yes nor no (`why`), and was held back as such: said as what stands.
    void heldBackAfter(const SignInPacing::Mark& mark, const base::Error& why);
    // The controller showed the certificate it is known by, on one of this driver's lines: said once.
    void showed(const QByteArray& der);

    // One question under the session, signed in once more where the controller refused it.
    void request(const Ask& question, std::function<void(base::Result<Reply>)> done);
    void request(const Ask& question, std::function<void(base::Result<Reply>)> done, Tries tries);
    // A resource of the controller's at `path`: a detail where it is not vital (`detail`, Ask), or as said.
    void read(const QString& path, bool vital, std::function<void(base::Result<QJsonObject>)> done);
    void read(const QString& path, bool vital, bool detail, std::function<void(base::Result<QJsonObject>)> done);
    // The first member of a collection the controller points to; empty where it lists none of its own.
    void firstOf(const QJsonValue& collection, bool vital, std::function<void(base::Result<QString>)> done);
    void finish(const std::shared_ptr<Identifying>& asking, const base::Result<Identity>& result);

    // Where it keeps things, found once every so often (placesFreshMs).
    void find(std::function<void(base::Result<Places>)> done);
    void placesOf(const QJsonObject& root, const QString& system, std::function<void(base::Result<Places>)> done);
    void sensorPlaces(const std::shared_ptr<Places>& places, std::function<void()> done);
    // Every temperature and every fan, in whichever shape this controller has them: a detail of how it is.
    void sensors(const Places& places, std::function<void(QList<redfish::Temperature>, QList<redfish::Fan>)> done);
    void nextFan(const std::shared_ptr<FanWalk>& walk);
    // What rarely changes about the controller: its version (the web's `_itself`).
    void itself(const Places& places, std::function<void(QString version)> done);
    // Its version as its manager (read: `manager`, at `own`) says it; where it says none at all, out of its firmware
    // inventory (the web's `_inventory_version`). `mine`: whether what was just read came under the session its
    // server was judged under (what came under another is no answer).
    void versionOf(const QJsonObject& manager, const QString& own, const QString& update,
                   const std::function<bool()>& mine, std::function<void(QString version)> done);
    void nextImage(const std::shared_ptr<ImageWalk>& walk);
    // How it is when a look found no server to show: in what words, and whether the last good look stands.
    void lookFailed(const base::Error& why);
    void lookFailedAs(const base::Error& why);
    // `hiccup`: what is said is the last good look, left standing for one of its moments (looked at again sooner).
    void lookOver(Status status, bool hiccup);
    // Whether it is there at all, while nothing that needs a sign-in is asked (the web's `_gone_meanwhile`); then
    // the look is said as `why`, or as what found it gone.
    void stillThere(const base::Error& why);
    std::optional<base::Error> heldBack() const;
    // What comes with a computer read and judged: the session it was judged under (whatever was signed in since),
    // and the mark the controller put on its state (redfish::markOf; empty: none).
    struct Checked {
        QByteArray token;
        QByteArray mark;
    };
    // The computer at `place`, read and judged by the number it gives for itself under that very session.
    void checkedRead(const QString& place, bool vital, std::function<void(base::Result<QJsonObject>, Checked)> done);
    // Whether the server that answered under `token` is the one this driver is for, by the number its computer
    // gave there (`seen`, empty: none) and its controller's (the web's `_judge`).
    void judge(const QString& seen, const QByteArray& token, std::function<void(base::Result<void>)> done);
    // The controller's number as the controller that made the session `token` says it: its root's, where its
    // manager, read under that session, names the same; and whether that could be asked under it.
    void controllerUnder(const QByteArray& token, std::function<void(const QString& found, bool asked)> done);
    // Another server answers at its address: said from now on, until this one answers again.
    base::Error another(const base::Error& why);
    // Whether the answer just handed over came under the session its server was last judged under: what was read
    // under another (signed in anew meanwhile, maybe at another controller) is not taken for this server's.
    bool underJudged() const { return !m_checkedToken.isEmpty() && m_answeredToken == m_checkedToken; }

    // The press of a button: the kinds of it that would do what is wanted, as the resource (its computer, or its
    // controller) offers them; the command; what came of it; whether it was carried out.
    void offer(const std::shared_ptr<Pressing>& pressing, const QJsonObject& resource, QLatin1StringView action,
               const QString& place, const QStringList& wanted);
    void press(const std::shared_ptr<Pressing>& pressing, const QString& target, const QString& kind);
    void carriedOut(const std::shared_ptr<Pressing>& pressing, qint64 until, const QString& seen);
    void pressed(const std::shared_ptr<Pressing>& pressing, const base::Result<Done>& result);
    // Its light switched: the change, shown the mark of the state it was decided on, or (`meant`) said to be meant
    // whatever the state.
    void light(const std::shared_ptr<Pressing>& pressing, const QString& place, const QJsonObject& change,
               const QByteArray& mark, bool meant);
    // Something was done to it: what its card says is old now, and it is looked at again at the moments after.
    void followUp();

    // Start into BIOS setup, in steps (redfish_setup.cpp): the computer read and judged, and what goes out planned
    // (`meant`: the change is sent as meant whatever the state); the override set, and seen set; the restart; the
    // start watched until the server has taken the override, or the press's time is over; the override cleared again
    // (`left`: nothing where it was, else what is to be said of it); the press's end.
    void startIntoSetup(const std::shared_ptr<Pressing>& pressing);
    void setupRead(const std::shared_ptr<Setup>& setup, bool meant);
    void setTheOverride(const std::shared_ptr<Setup>& setup, const QByteArray& mark, bool meant);
    void lookForTheOverride(const std::shared_ptr<Setup>& setup, qint64 until);
    void restartIntoSetup(const std::shared_ptr<Setup>& setup);
    void watchTheStart(const std::shared_ptr<Setup>& setup);
    void setupEnds(const std::shared_ptr<Setup>& setup);
    void clearTheOverride(const std::shared_ptr<Setup>& setup, bool meant,
                          std::function<void(std::optional<QString> left)> then);
    void clearShown(const std::shared_ptr<Setup>& setup, qint64 until,
                    std::function<void(std::optional<QString> left)> then);
    void setupOver(const std::shared_ptr<Setup>& setup, const base::Result<Done>& result);
    // From here on what comes of `pressing` may have been done: a driver let go of says so.
    static void sentFor(const std::shared_ptr<Pressing>& pressing);
    // Whether a start into BIOS setup under way may have set the override.
    bool anOverrideMayBeSet() const;
    // What may be left of a start into BIOS setup, kept beside the settings (setup_overrides.h, review 4's MEDIUM 1):
    // where; the moment `ms` of this driver's clock on the PC's; the change can go out now, so noted until the press
    // can do nothing more; at a press's end, let go of where it was seen off with nothing of it still on its way, else
    // left standing until a change of its can no longer come; a look that shows the override off.
    SetupOverrides& overrides() const;
    QDateTime wallAt(qint64 ms) const;
    void noteMayBeSet(const std::shared_ptr<Setup>& setup);
    void settleTheNote(const std::shared_ptr<Setup>& setup);
    void lookedAtTheOverride(const QJsonObject& system);
    // This driver is let go of: each start into BIOS setup under way is over, its press told that the override may be
    // set where it may, and that override handed to the goodbyes to be cleared on the way out. True: one was, and this
    // driver's session is given back after it.
    bool handTheSetupsOver();

    // The way into its console, in steps (redfish_console_way.cpp): whether it has one; which server answers (asked
    // through `asked` of its moments); its web page's sign-in, and what came of it, also where its driver was let go
    // of while it was out; the pass; and an asking that ends without a way, its web page left on the way out.
    SignInPacing::Mark webOffered() const;
    void hasConsole(const std::shared_ptr<WayAsking>& asking);
    void whichServerForTheConsole(int asked, const std::function<void(std::optional<base::Error>)>& then);
    void webSignIn(const std::shared_ptr<WayAsking>& asking);
    // Its web page's sign-in with this password is out, another driver's or another asking's of this one, and nothing
    // else stands: what it comes to is waited for, and then the asking is asked through again; and whether that may be
    // done now.
    void afterTheOtherWebSignIn(const std::shared_ptr<WayAsking>& asking);
    bool mayWaitForTheOtherWeb() const;
    void webSignInCame(const std::shared_ptr<WayAsking>& asking, const base::Result<QByteArray>& session);
    static base::Result<QByteArray> webCameOf(const WebSignIn& out, const base::Result<Reply>& reply, bool late);
    void consolePass(const std::shared_ptr<WayAsking>& asking);
    void askThePass(const std::shared_ptr<WayAsking>& asking);
    void wayHanded(const std::shared_ptr<WayAsking>& asking, ConsoleWay way);
    void noWay(const std::shared_ptr<WayAsking>& asking, const base::Error& why);
    // This driver is let go of: every asking under way is told "stopped", and what it holds of its web page given back.
    void letGoOfTheWays();
    // A session this driver lets go of (it is let go of itself, or the controller refused it): given back.
    void giveBack(const Session& session);
    void answered();
    base::Error failed(const base::Error& error);

    Where m_where;
    // Its password, for as long as it signs in with it: its own, written over when it goes (wiped.h; from the outside
    // review, F5).
    SecretText m_password;
    Pace m_pace;
    DeviceAsker m_asker;
    QElapsedTimer m_clock;

    Session m_session;
    bool m_signingIn = false;
    // The sign-in that is out, on its own line; none when none is.
    std::shared_ptr<SignInOut> m_signInOut;
    // Which server answers: the mark it is known by; the session of the last answer, and the one under which
    // its computer was last found to be this one; the sessions signed in right after its controller's number
    // was compared (or under which it was taken), the last few; the controller's number as read under a
    // session, and that session; another server found at its address, said until this one answers again.
    QString m_machine;
    QByteArray m_answeredToken;
    QByteArray m_checkedToken;
    QList<QByteArray> m_controllerTokens;
    std::optional<std::pair<QString, QByteArray>> m_rootSeen;
    // Its root as read on a sign-in's own line just before the password went (for the sign-in to take along), and
    // with the session that sign-in brought: what its controller's number is taken from under that session.
    QJsonObject m_rootBeforeSignIn;
    std::optional<std::pair<QJsonObject, QByteArray>> m_rootOfSession;
    std::optional<base::Error> m_other;
    std::optional<Places> m_places;
    qint64 m_placesAt = 0;
    bool m_toldItsCertificate = false;
    QList<std::function<void(base::Result<QByteArray>)>> m_waitingForToken;
    // What came of the last sign-in of this driver's that let it nowhere, in its own words, and the hold
    // it noted then as the pacing kept it: heldSignIn says those words while that very hold stands.
    std::optional<base::Error> m_signInError;
    std::optional<SignInPacing::Kept> m_heldAs;
    // The hold this driver noted when the controller refused the session it had just given, as the pacing kept it.
    std::optional<SignInPacing::Kept> m_sessionsHeldAs;
    // The places it refused this account under a session it honours: not asked again until where it keeps things is
    // found anew.
    QSet<QString> m_refusedPlaces;

    // How it was found: the last look that worked, and when (said again, for a while, by a look that fails
    // for one of its moments: hiccup); a critical fault it reported, when and where; its version, and when it
    // was asked; the fans of the newer shape as last read, by place, in the controller's order.
    std::optional<Status> m_good;
    qint64 m_goodAt = 0;
    bool m_hiccup = false;
    qint64 m_statusAt = 0;
    std::optional<qint64> m_faultAt;
    QString m_faultWhere;
    QString m_version;
    std::optional<qint64> m_versionAt;
    QList<QString> m_fanOrder;
    QHash<QString, FanRead> m_fansRead;
    qint64 m_heardAt = -1;

    int m_failures = 0; // attempts in a row that found it away
    qint64 m_downUntil = 0;
    std::optional<base::Error> m_downError;
    std::optional<qint64> m_lastOkAt;

    std::optional<Status> m_status;
    bool m_looking = false;
    // The presses of buttons under way, and the moments it is to be looked at again after one (FOLLOW_UP).
    QList<std::shared_ptr<Pressing>> m_pressing;
    // The starts into BIOS setup under way.
    QList<std::shared_ptr<Setup>> m_setups;
    QList<qint64> m_followUpAt;
    // The askings for a way into its console under way; whether it has a console (none: not asked yet); whether its
    // firmware lets a console reconnect (none: not asked yet); its web page's sign-in that is out, one a driver, and
    // the askings that wait for what it comes to.
    QList<std::shared_ptr<WayAsking>> m_wayAsked;
    std::optional<bool> m_hasConsole;
    std::optional<bool> m_completeFirst;
    std::shared_ptr<WebSignIn> m_webSignInOut;
    QList<std::shared_ptr<WayAsking>> m_afterTheWebSignIn;

    // Its server's screen (redfish_screen.cpp): made when it is first wanted, and what it showed handed to every
    // live of it; why it is not shown, and until when. It goes first as the driver goes, its console's goodbye
    // said on a line the program keeps until it has gone: at the program's end while the loop still runs
    // (main.cpp sends the deferred deletions before it waits for the goodbyes; plan-m7 1b-i, item 14).
    ServerScreen& screen();
    void shownOnTheScreen(const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed);
    void notedOnTheScreen(const base::Error& why);
    std::optional<base::Error> noScreen() const;
    bool hasScreen() const;
    void typeOnTheScreen(const Action& what, std::function<void(base::Result<Done>)> done);
    std::unique_ptr<ServerScreen> m_screen;
    QList<QPointer<ServerLive>> m_lives;
    std::optional<base::Error> m_noScreen;
    qint64 m_noScreenUntil = 0;
    // The frames it showed, and the card's still last made: of which frame, at which width.
    quint64 m_framesShown = 0;
    struct MadeStill {
        quint64 frames = 0;
        int tier = 0;
        QByteArray picture;
    };
    std::optional<MadeStill> m_still;
};

} // namespace qkvm::core
