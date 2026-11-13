#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/device_asker.h>
#include <qkvm/core/jetkvm_kept.h>
#include <qkvm/core/jetkvm_typing.h>
#include <qkvm/core/jetkvm_viewer.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvm_times.h>
#include <qkvm/core/own_device.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/wiped.h>

namespace qkvm::core {

class DeviceLine;

// The driver of a JetKVM of this PC's own wall (M6), in the same words as a kvmd's (kvmd_device.h)
// wherever the two say the same thing.
//
// * It is asked over plain HTTP or HTTPS, as its address is: held to the certificate it is known by,
//   straight to it whatever proxy this PC uses, with a limit on every answer (DeviceAsker). Its
//   signalling line (a WebSocket) is held to the same (DeviceLine).
// * A JetKVM has one password and no user name, and one session for everybody: a sign-in (POST
//   /auth/login-local, the password as JSON) signs out whoever was signed in to it before, its own
//   page included. So the driver signs in once and keeps the session (cookie "authToken") for as long
//   as the JetKVM takes it, and signs in once more where its signalling line says that somebody has
//   since. A JetKVM that runs without a password answers a sign-in 400: nothing is needed then, and
//   nothing is sent to say who asks. With no password saved nothing is sent to sign in, and it is
//   asked as one that needs none, as a JetKVM is out of its box; only where it then refuses is that
//   "no_password". (A kvmd with no password saved hears nothing at all: kvmd_device.h.)
// * A password it refused is offered again as SignInPacing says, as a kvmd's is. A JetKVM shuts an
//   address out after five wrong ones (429) and says when to come back: then, and not sooner than the
//   first wait after a no, and an hour at the most whatever it says. A sign-in that came to neither yes
//   nor no is said as what stands ("no_sign_in"), never as a question that may have been done.
// * What it is (identify, for a person who adds it): its version, which it says first on its
//   signalling line (`device-metadata`), and its number (`GET /device`'s `deviceId`), kept only as a
//   digest, to know it by at another address. The signalling line is the one a picture is arranged
//   over; being let in on it takes nobody's picture.
// * Which JetKVM answers (the web's _same_jetkvm and _judge, jetkvm.py): a JetKVM takes its address from
//   DHCP, and a fleet often has one password. So before its signalling line is asked for (every video
//   connection: a card's, a window's, "Show here"; and identify), its number is asked under the very
//   session the line is then asked with, after the look at its HTTPS and the sign-in, and judged against
//   the one it is known by (Where::machine). Another number, or none where one is known: "another_machine"
//   in the web's words, no line is asked for, no offer goes and no viewer is taken; said by every card and
//   look from then on, until the one it is known by answers again. Where it could not be asked and one is
//   known: the asking's error. Where none is known, the first one seen is taken (trust on first use, as a
//   certificate is) and said (knownAs); identify, which is the taking of an addition or of an edit with
//   the password typed, never takes one it could not read. Over plain HTTP its number is asked once more before its
//   password goes, with no session (the web's _number_to_anybody): a JetKVM without a password says it to anybody, and
//   is judged so before anything is sent (another one gets no password in the clear); one with a password answers 401
//   there, and is signed in to as before.
// * A look never connects and never counts as somebody watching: `GET /device/status`, which anybody
//   may ask. It answers: "online"; it does not: "offline"; a certificate that is not the one: "error".
//   It holds to its own count of looks that found the JetKVM away, which its own answer ends: nothing of
//   the road that connects holds it, and it holds nothing of that road (the web's probe, which has no
//   _check_backoff and calls no _answered: from the second reading of c7ec58c, A).
// * Its other way (Pace::otherWay, HTTPS and plain HTTP at the same host), as the web asks it
//   (jetkvm.py, _look_the_other_way): the JetKVM's question alone, with its certificate not judged. One
//   spoken to over plain HTTP is asked at a look, at most every wayEveryMs, whether it offers HTTPS by
//   now: a JetKVM there is told to the wall (offersHttps), which decides whether it follows it. One that
//   has not answered for wayPatienceMs is asked at most every wayWhenDownMs: one over HTTPS whether it
//   answers over plain HTTP, and a JetKVM there is said on its card ("plain_only"), never followed; one
//   over plain HTTP whether it offers HTTPS. Answering again ends all that was said of it.
// * Its picture, and how it is while somebody watches it: its one viewer (JetKvmViewer, M6 2g), whose one
//   connection its cards and windows share (JetKvmLive), and which steps aside for another viewer until
//   act("resume") ("Show here"). A card that asks connects; a look never does. That it stepped aside
//   outlives this driver (JetKvmKept, which the wall hands to each driver of the KVM).
// * Its keyboard and mouse live: JetKvmLive, over its connection (JetKvmConnection: hidrpc, or JSON-RPC). Keys pressed
//   together and a text (act "keys", "ctrl_alt_del", "text"): over the connection whose keys are ready, one action at
//   a time (jetkvm_typing.h), a text in the keyboard layout the JetKVM says its computer uses (jetkvm_layouts.h).
//   Power and reset by its ATX extension, a restart, Wake-on-LAN and its jiggler: one call each over its connection
//   (press). What it can be asked is said in its status (abilities).
//
// On the interface thread, as every driver is (own_device.h).
class JetKvmDevice : public OwnDevice {
    Q_OBJECT

public:
    struct Where {
        QUrl address; // http or https, a host, a port where one was typed, and nothing else
        bool knownByCertificate = false;
        QByteArray certificate; // its SHA-256
        // The mark of the number it gave for itself (OwnKvm::number: "jetkvm:" and a digest); empty: none is known.
        QString machine;
    };

    // What it is given, made short in a test.
    struct Pace {
        int connectWithinMs = kvm::letInWithinMs; // to be let in at all
        int quietMs = kvm::quietMs;               // the longest nothing may come while an answer is waited for
        int transferMs = kvm::transferMs;         // and how much longer than that a whole answer may take
        int holdMs = 1000; // after a failure it is left alone this long; doubled while it stays away
        int longestHoldMs = 15'000;
        // As a kvmd's (KvmdDevice::Pace): the first wait after a no, and after a sign-in that came to
        // neither yes nor no; SignInPacing keeps them, outside this driver.
        int refusedHoldMs = 60'000;
        int signInRetryMs = 10'000;
        // A JetKVM's own "come back then" (429) is kept this long at the most, whatever it says.
        int laterAtMostMs = 3'600'000;
        // Where they are kept: none, the program's own (SignInPacing::program()).
        std::shared_ptr<SignInPacing> pacing;
        // How long what a look found counts as how it is.
        int freshMs = 2500;
        // Its signalling line: let in within this, and once in, its version said within this.
        int lineWithinMs = 6000;
        int versionWithinMs = 3000;
        // Where a JetKVM's other way is (kvm::otherWayOf): for the wall, which takes a JetKVM onto HTTPS
        // at its own host only there (LocalWall::ontoHttps). A test has two stand-ins at two ports be one
        // JetKVM's two ways.
        std::function<std::optional<QUrl>(const QUrl&)> otherWay = kvm::otherWayOf;
        // How often its other way is asked whether a JetKVM answers there (the web's WAY_EVERY, WAY_WHEN_DOWN
        // and WAY_PATIENCE, jetkvm.py): one spoken to over plain HTTP at most every wayEveryMs, whether it
        // offers HTTPS by now; one that has not answered for wayPatienceMs (a JetKVM that is starting up
        // serves plain HTTP at once and HTTPS a little later), at most every wayWhenDownMs.
        int wayEveryMs = 600'000;
        int wayWhenDownMs = 60'000;
        int wayPatienceMs = 120'000;
        // While another JetKVM answers at its address, a card's connection asks again which one it is no more often
        // than this (the web's SAME_JETKVM_EVERY); "Show here" asks at once.
        int sameJetKvmEveryMs = 60'000;
        // Its one viewer's (JetKvmViewer::Pace): the connection's, UNWATCHED, the wait for the one before.
        JetKvmViewer::Pace viewer;
        // An action that types (jetkvm_typing.h): its waits and pauses.
        jetkvm::TypingPace typing;
    };

    // What it says it is.
    struct Identity {
        QString model;   // "JetKVM"
        QString version; // as it says it; empty where it said none in time
        // "jetkvm:" and a digest of its own number; empty where it gives none (none fit to be one, or an older one with
        // no GET /device). A number that could not be read is no identity: identify says why.
        std::optional<QString> number;
    };

    JetKvmDevice(Where where, QString password, Pace pace, QObject* parent = nullptr);
    // With what the KVM keeps across its drivers (the wall's: JetKvmKept); none: its own.
    JetKvmDevice(Where where, QString password, Pace pace, std::shared_ptr<JetKvmKept> kept, QObject* parent = nullptr);
    ~JetKvmDevice() override;
    JetKvmDevice(const JetKvmDevice&) = delete;
    JetKvmDevice& operator=(const JetKvmDevice&) = delete;
    JetKvmDevice(JetKvmDevice&&) = delete;
    JetKvmDevice& operator=(JetKvmDevice&&) = delete;

    // A person has typed this password for the JetKVM at this place just now: it is tried as for the
    // first time, whatever was refused before (SignInPacing::forget).
    static void tryAfresh(const Where& where, const QString& password, const Pace& pace);
    // Whether a question that ended so may have been done all the same: it went out, and what came of
    // it is not known ("timeout"; a JetKVM's "dropped" was not sent).
    static bool mayHaveBeenDone(const base::Error& error);

    // Its other way was asked just now by another (the wall's asking of what it is): asked again by this
    // driver at its own pace from now on, not at its first look.
    void otherWayAskedJustNow();
    // Before every sign-in of one spoken to over plain HTTP whose other way is HTTPS at its own host, that HTTPS is
    // asked the JetKVM's question, and what came of it (`found`: what its line showed, whether anything listens there)
    // is handed to `check` with where this driver speaks to it (`from`), whatever it answered. Where `check` says why
    // not (the wall's, LocalWall::judgeItsHttps: another KVM of the wall is known by that certificate, and what answers
    // here may be that one; or the KVM is held back for that one, and this look shows nothing that lets go of it), no
    // password goes: the sign-in comes to that refusal, and httpsShowed says what was shown, where anything was. None
    // set: signed in as ever (from the reading of 2g/2h, M1; and the outside review, F1).
    using TwinCheck = std::function<std::optional<base::Error>(const QUrl& from, const KvmProbe::Found& found)>;
    void lookAtItsHttpsBeforeEverySignIn(TwinCheck check);

    // Its version and its number, for a person who adds it: signs in where that is needed.
    void identify(std::function<void(base::Result<Identity>)> done);

    // Its signalling line, let in: the one road every asking that needs it takes (what it is, and a picture's
    // connection, JetKvmConnection), with the session this driver keeps; signed in to where that is needed
    // and allowed, and once more where the line refuses a session that somebody has since replaced (M6 2f).
    // Handed over from inside the line's "you are in", before anything the JetKVM says next is read: whoever
    // takes it connects to it there and then, and from then on it is theirs (a child of this driver until it
    // is given another parent). What stands against it, or why it ended before it was let in, a turn of the
    // loop later.
    void openSignalling(std::function<void(base::Result<DeviceLine*>)> done);
    // Where it is spoken to.
    const QUrl& address() const { return m_where.address; }
    // The mark it is known by now: what it was made with, or what it took since (knownAs).
    const QString& machine() const { return m_machine; }
    // Another JetKVM answers at its address (or one that gives no number, where one is known): what is said, until the
    // one it is known by answers again. None otherwise.
    const std::optional<base::Error>& another() const { return m_other; }

    // The words of what is said where another JetKVM answers at its address (the web's ANOTHER_JETKVM and
    // NO_JETKVM_NUMBER, a card where the web has a tile).
    static base::Error anotherJetKvm();
    static base::Error noJetKvmNumber();
    // What a JetKVM can be asked (the web's `can` of one, jetkvm.py): power and reset only where its ATX extension is
    // on. Said in its status, which is all its menu offers.
    static QStringList abilities(bool atx);
    // Its one viewer (the cards' and the windows' connection).
    JetKvmViewer& viewer() { return *m_viewer; }

    Status status() const override;
    bool isStale() const override;
    void look() override;
    void afterLook(int withinMs, std::function<void(bool stillLooking)> then) override;
    void askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done) override;
    void act(const Action& what, std::function<void(base::Result<Done>)> done) override;
    void askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done) override;
    void tidy() override;
    std::unique_ptr<OwnLive> makeLive() override;

signals:
    // Its other way, HTTPS at `at`, answered as a JetKVM does (`found`): for the wall, which decides
    // whether it follows it there (LocalWall). `from`: where this driver speaks to it.
    void offersHttps(const QUrl& from, const QUrl& at, const qkvm::core::KvmProbe::Found& found);
    // Its other way, HTTPS at `at`, showed this certificate (`shown`, its SHA-256) and answered otherwise than a
    // JetKVM does (an error, an answer past its time, a page that is none): for the wall, which holds it back where
    // that is the certificate another KVM of the wall is known by (LocalWall).
    void httpsShowed(const QUrl& from, const QUrl& at, const QByteArray& shown);
    // The JetKVM at its address was taken for this driver's (no number was known of it): the mark it is known by from
    // now on (`now`), and the one before (`before`, empty). Said a turn of the loop later; only ever of this driver's
    // own JetKVM, at `from`, where it speaks to it.
    void knownAs(const QUrl& from, const QString& before, const QString& now);

private:
    // Its viewer reads the hold below before it connects, notes a connection that found the JetKVM away as an attempt
    // that did, and has it tried at once where a person asks ("Show here"): the web's _connected (jetkvm.py, its
    // _check_backoff, _failed and urgent), which is one driver there.
    friend class JetKvmViewer;
    using Ask = DeviceAsker::Ask;
    using Reply = DeviceAsker::Reply;
    struct Identifying;
    struct Road;

    qint64 now() const { return m_clock.elapsed(); }
    void later(std::function<void()> what);

    // The session to ask with: the pass it was given, or nothing where it needs none (it runs without
    // a password, or none is saved); signs in where that is needed and allowed.
    void withToken(std::function<void(base::Result<QByteArray>)> done);
    // Its HTTPS looked at first where that is asked for (lookAtItsHttpsBeforeEverySignIn), then signInNow.
    void signIn();
    // A sign-in goes on a line of its own (line), made for it, the question asked first with it: one that
    // outlives this driver where it is let go of while the sign-in is out (~JetKvmDevice), so that what it
    // comes to is heard all the same and told to the pacing (cameOf), which ends its hold (as a kvmd's).
    struct SignInOut;
    struct SignInCame;
    void signInNow();
    // Its other way where that is HTTPS at its own host and it is spoken to over plain HTTP; nothing otherwise.
    std::optional<QUrl> itsOwnHttps() const;
    void sendSignIn(DeviceAsker* line, const Ask& login);
    static SignInCame cameOf(const SignInOut& out, const base::Result<Reply>& reply);
    void signInCame(const SignInCame& came);
    void signedIn(const base::Result<QByteArray>& token);
    // Another driver of this JetKVM has the same password out, and nothing else stands: what it comes
    // to is waited for, and then looked at again; and whether that may be done now.
    void afterTheOtherSignIn();
    bool mayWaitForTheOther() const;
    // The JetKVM showed the certificate it is known by, on one of this driver's lines: said once.
    void showed(const QByteArray& der);
    // Where this driver's sign-ins are counted, and the mark of the one it would offer now.
    SignInPacing& pacing() const;
    SignInPacing::Mark offered() const;
    SignInPacing::Steps steps() const;
    // What stands against offering the password now, noted by any driver of this JetKVM.
    std::optional<base::Error> heldSignIn() const;
    // Whether nobody has chosen yet, at its own page, how it is signed in to (`isSetup` false).
    void freshFromItsBox(std::function<void(bool fresh)> then);
    // Why its signalling line refuses the session it was given: in that sentence.
    void rejected(std::function<void(base::Error why)> then);

    void openRoad(const std::shared_ptr<Road>& road);
    void askForTheLine(const std::shared_ptr<Road>& road);
    // Which JetKVM answers, asked under `token` (the session the line is then asked with) and judged; then the line.
    void askWhichItIs(const std::shared_ptr<Road>& road, const QByteArray& token);
    void openTheLine(const std::shared_ptr<Road>& road, const QByteArray& token);
    // The session `used` was refused (by its line, or by the question of which it is): signed in to once more, or why
    // not.
    void sessionRefused(const std::shared_ptr<Road>& road, const QByteArray& used);
    // Whether what gives this number for itself (`seen`; empty: none) is the JetKVM it is known by (the web's _judge):
    // what stands against it, where it is not.
    std::optional<base::Error> judge(const QString& seen);
    // Another JetKVM answers at its address: noted, and said from now on.
    base::Error noteAnother(const base::Error& why);
    void arrive(const std::shared_ptr<Road>& road, const base::Error& why);
    // What it is, once its line said its version (or did not in time, or ended first).
    void said(const std::shared_ptr<Identifying>& asking);
    void finish(const std::shared_ptr<Identifying>& asking, const base::Result<Identity>& result);

    // Attempts in a row that found it away, the hold they grew (holdMs, doubled while it stays away, longestHoldMs at
    // the most), and what found it so: the web's _failures, _down_until and _down_error, kept apart for each of its
    // two roads (the one that connects, and the plain look).
    struct Away {
        int failures = 0;
        qint64 until = 0;
        std::optional<base::Error> error;
        // An attempt that found it away (questions that were out together fail together: one, not several).
        void note(const base::Error& why, qint64 at, const Pace& pace);
        std::optional<base::Error> held(qint64 at) const;
        qint64 heldForMs(qint64 at) const;
        void clear();
    };

    // It can be used: both roads' counts are over (a sign-in, what it is, the first thing it answers over a
    // connection).
    void answered();
    // The road that connects: an attempt that found it away, where the error says so.
    base::Error failed(const base::Error& error);
    // While that road found it away just now (the web's _check_backoff): what found it so; and for how many ms more
    // (0: it may be asked).
    std::optional<base::Error> heldBack() const;
    qint64 heldBackForMs() const;
    // A person is waiting: tried now, whatever hold stands (the web's urgent; how many times in a row it was
    // found away is kept).
    void tryNow();
    // An action that types: its reports, once the connection's keys are ready, one action at a time (two would
    // interleave), as the web's act has them under its _typing lock.
    // `text`: a text, made into keys once the JetKVM has said its computer's layout (`reports` none).
    void typeOn(std::vector<typed::Report> reports, std::optional<QString> text,
                std::function<void(base::Result<Done>)> done);
    void typeNext();
    QList<std::function<void()>> m_typingWaits;
    bool m_typingNow = false;
    // What else is done to it (power and reset by its ATX extension, a restart, Wake-on-LAN, the jiggler): one call
    // over its connection, once that is open.
    void press(const Action& what, std::function<void(base::Result<Done>)> done);
    static void pressAtx(const QPointer<JetKvmConnection>& on, const QString& kind,
                         const std::function<void(base::Result<Done>)>& done);
    static void pressOnce(const QPointer<JetKvmConnection>& on, const QString& method, const QJsonObject& params,
                          const std::function<void(base::Result<Done>)>& done);
    // Whether an end means "the JetKVM is not there, or is not the JetKVM" (the web's DOWN_CODES).
    static bool isAway(const std::string& code);
    // Asks its other way, where that is due (`down`: it has not answered just now).
    void lookTheOtherWay(bool down);
    // Its status, which anybody may ask, and what its viewer last showed or stands for (busy while aside).
    void lookThePlainWay();
    void lookOver(Status status);

    Where m_where;
    // Its password, for as long as it signs in with it: its own, written over when it goes (wiped.h; from the outside
    // review, F5).
    SecretText m_password;
    Pace m_pace;
    DeviceAsker m_asker;
    QElapsedTimer m_clock;

    QByteArray m_token;
    bool m_openAccess = false; // it answered a sign-in that it runs without a password
    bool m_signingIn = false;
    std::shared_ptr<SignInOut> m_signInOut; // the sign-in that is out, on its own line; none when none is
    bool m_toldItsCertificate = false;
    qint64 m_signInLineHeardAt = -1; // when a sign-in's line last heard the JetKVM (by m_clock; -1: never)
    // Whether the JetKVM was heard just now, on this driver's own line or on a sign-in's.
    bool heardJustNow() const;
    QList<std::function<void(base::Result<QByteArray>)>> m_waitingForToken;
    // What came of the last sign-in of this driver's that let it nowhere, in its own words, and the
    // hold it noted then as the pacing kept it: heldSignIn says those words while that very hold
    // stands. Every hold is the pacing's, a session its signalling line refuses among them.
    std::optional<base::Error> m_signInError;
    std::optional<SignInPacing::Kept> m_heldAs;

    Away m_road; // the road that connects: what it is, a sign-in, a picture's connection
    Away m_look; // the plain look's own, which its own answer ends
    std::optional<qint64> m_lastOkAt;

    // Its other way: when it was last asked, whether that asking is out, since when it has not answered,
    // and where a JetKVM answers over plain HTTP only, where it was spoken to over HTTPS. Its own probe,
    // made when it is first needed.
    std::optional<qint64> m_wayAskedAt;
    bool m_wayAsking = false;
    std::optional<qint64> m_downSince;
    std::optional<QUrl> m_plainOnly;
    std::unique_ptr<KvmProbe> m_wayProbe;
    // What its HTTPS is held to before every sign-in (lookAtItsHttpsBeforeEverySignIn); none: nothing.
    TwinCheck m_twinCheck;

    // The mark it is known by (Where::machine, or the first one seen since), and another that answers at its address.
    QString m_machine;
    std::optional<base::Error> m_other;

    QString m_version; // as it last said it
    std::optional<Status> m_status;
    qint64 m_statusAt = 0;
    bool m_looking = false;

    // (Last: it goes first, and gives its connection back while the rest of this driver stands.)
    std::unique_ptr<JetKvmViewer> m_viewer;
};

} // namespace qkvm::core
