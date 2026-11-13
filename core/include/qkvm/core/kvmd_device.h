#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QSslConfiguration>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <qkvm/base/result.h>
#include <qkvm/core/awake_line.h>
#include <qkvm/core/board_holds.h>
#include <qkvm/core/device_asker.h>
#include <qkvm/core/kvm_times.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/kvmd_console_line.h>
#include <qkvm/core/kvmd_stream.h>
#include <qkvm/core/own_device.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/wiped.h>

class QNetworkReply;

namespace qkvm::core {

// One KVM of this PC's own wall that runs kvmd (PiKVM, GL.iNet Comet), spoken to by the program
// itself (SPEC.md section 6.3): signing in, how it is, a picture of its screen, its live picture,
// and what is done to it. What the server's driver does for a QKVM server (../app/qkvm/kvm.py),
// this does here, and answers in the same words: a card cannot tell which of the two it shows.
//
// On the interface thread. A call comes back at once; what it asked for is handed to `done` later,
// on the same thread, exactly once, unless the device is gone by then. Whoever is handed something
// does not end the device there and then: it is ended a turn of the loop later (deleteLater), since
// what handed it over is in the middle of its work.
//
// Towards the KVM (SPEC.md section 8):
// * Over HTTPS it is one of two: one whose certificate Windows trusts for its name, or one that
//   shows exactly the certificate it is known by. That is looked at when the two have agreed on
//   how to speak and before a byte of a question goes out. A KVM that is neither hears nothing:
//   no password, no session.
// * It is spoken to straight, whatever proxy this PC uses for the web, and is followed nowhere: a
//   KVM that answers "go there" has given an answer that is none.
// * Every answer has a limit on its size and on its time (a sign-in's is longer: some KVMs hold a
//   no back on purpose). A KVM that does not answer is asked less and less often while it stays
//   away, and a password it refused is not sent again for a minute, then longer after each no in
//   a row (SignInPacing): a KVM locks an account out.
// * TLS is Windows' own (tls.h). Where that is not what the program speaks, nothing goes out to
//   a KVM at an HTTPS address: every question ends with the one code and sentence there is for
//   that (damagedTls), and how the KVM is, is told as something to look at ("error"), not as a
//   KVM that is away.
class KvmdDevice : public OwnDevice {
    Q_OBJECT

public:
    // Where the KVM is and what it is known by.
    struct Where {
        QUrl address; // http or https, a host, a port where one was typed, and nothing else
        QString user = QStringLiteral("admin");
        // Over HTTPS: whether it is known by a certificate, and that certificate's SHA-256. One
        // that is known by none has to be trusted by Windows for its name.
        bool knownByCertificate = false;
        QByteArray certificate;
    };

    // The times it keeps. A test makes them short. (The first three are also what an asking of
    // what answers at a KVM's address is given, which goes before every sign-in: kvm_times.h has
    // them for both, and KvmProbe::Pace::beforeASignInWith makes an asking's of a driver's.)
    struct Pace {
        int connectWithinMs = kvm::letInWithinMs; // to be let in at all
        int quietMs = kvm::quietMs;               // the longest nothing may come while an answer is waited for
        int transferMs = kvm::transferMs;         // and how much longer than that a whole answer may take
        int holdMs = 1000; // after a failure it is left alone this long; doubled while it stays away
        int longestHoldMs = 15'000;
        // After a refused password, the first wait: longer for each no in a row after it (five,
        // fifteen, sixty times as long); and after a sign-in that came to neither yes nor no, the
        // first, twice as long each time (thirty times as long at the most). SignInPacing keeps
        // them, outside this driver, which is made anew whenever its KVM is changed.
        int refusedHoldMs = 60'000;
        int signInRetryMs = 10'000;
        // Where they are kept: none, the program's own (SignInPacing::program()). A test hands the
        // drivers of one of its walls one of their own.
        std::shared_ptr<SignInPacing> pacing;
        // While a password waits to be offered again, how often the KVM is asked whether it is there
        // at all (with no password and no session): nothing that needs a sign-in is asked meanwhile,
        // and a KVM that went away went on saying that its sign-in failed.
        int stillThereEveryMs = 30'000;
        // How long what is known of it counts as how it is. Less than the four seconds at which a
        // wall asks (WallFeed), by the longest an asking waits for a look: so that every asking
        // of a wall finds it old and looks. At four seconds, what a look had found a moment after
        // one asking was still fresh at the next, and a KVM was looked at every eight. (It is
        // also for how long a picture that came says that the KVM's video is awake, where a
        // gateway answers for its stream: kvmd_stream.h, "no_stream_here". And so for how long
        // a picture that came and that nobody was handed is new to whoever asks for a new one:
        // askNewSnapshot.)
        int freshMs = 2500;
        int lookPatienceMs = 1500;  // how long whoever asks how it is waits for the look that asking began
        int aboutFreshMs = 600'000; // what it is, and that it has one computer, change rarely
        int firstContactMs = 600;   // how long a picture waits for a KVM nothing is known of yet
        int patienceMs = 1500;      // how long a picture waits for the KVM before its card asks again
        int restAfterMs = 30'000;   // nobody has asked for a picture this long: it is not kept awake
        int streamQuietMs = 10'000; // the longest nothing may come on its stream of pictures, which never ends
        // The longest one picture of that stream may take to come whole, from the first byte
        // behind the picture before it: as long as a whole answer may take, a picture that is
        // asked for by itself among them (quietMs and transferMs). So a picture of a megabyte
        // is still shown over a way that carries thirty-two kilobytes a second, and the largest
        // that is taken at all over one that carries a megabyte a second.
        int streamPictureMs = 32'000;
        // A power board the KVM said it has, this long ago or less, is not taken away by a look that says otherwise or
        // says nothing; and what it said of the computer is kept this long through such answers, which say nothing of
        // it (the web's BOARD_KEPT and POWER_KEPT). A Comet's board answers one asker at a time and tells whoever asks
        // meanwhile that there is none, and "off": the card's power buttons went with each such answer.
        int boardKeptMs = 60'000;
        int powerKeptMs = 10'000;
        // How long the board holds a button after it has answered its click (the web's HELD: a Comet's own page says
        // half a second, and six and a half for the long press); until it has let go, the machine is being done
        // something to. How long the click's answer is waited for, and after one that came to no clear answer, how
        // long the board is left alone beside the button's hold, from then (BOARD_ANSWER). And a press that must change
        // the computer's power is answered once the board shows the change, looked for this long at the most, this
        // often (SEEN_WITHIN, SEEN_EVERY: a Comet's word followed the computer three or four seconds late).
        int pressHeldMs = 500;
        int longPressHeldMs = 6500;
        int boardAnswerMs = 8000;
        int seenWithinMs = 20'000;
        int seenEveryMs = 1000;
        // While its line for events is held (the one that keeps its video awake), the KVM is asked on it this often
        // whether it is there, in kvmd's own words; and what its board said there counts while the line was heard from
        // within the second (the web's EVENTS_PING and EVENTS_QUIET).
        int eventsPingMs = 5000;
        int eventsQuietMs = 12'000;
        // Where the board's holds are kept: none, the program's own (BoardHolds::program()), which outlives every
        // driver. A test hands the drivers of one of its walls one of their own.
        std::shared_ptr<BoardHolds> boards;
        // A Fingerbot paired with the KVM (rule 29 of the web's FOR-THE-DESKTOP-APP; kvm.py FINGERBOT_*): how long its
        // two presses hold the power button (FINGERBOT_PRESSES: a short press, and the eight seconds that force a
        // computer off), how long the Comet's answer is waited for beside that (FINGERBOT_ANSWER), and the arm's way
        // back after it (FINGERBOT_BACK); how long a line opened to ask whether one is paired waits for the word
        // (FINGERBOT_ASK_WITHIN), and how long a "none", or nothing said, stands before it is asked again
        // (FINGERBOT_ASK_AGAIN); its battery asked this often while one is paired (FINGERBOT_BATTERY_EVERY), this soon
        // again after one that went unanswered (FINGERBOT_BATTERY_RETRY), and its answer waited for this long
        // (FINGERBOT_BATTERY_WAIT).
        int fingerbotPressMs = 500;
        int fingerbotLongPressMs = 8000;
        int fingerbotAnswerMs = 8000;
        int fingerbotBackMs = 1000;
        int fingerbotAskWithinMs = 10'000;
        int fingerbotAskAgainMs = 120'000;
        int batteryEveryMs = 3'600'000;
        int batteryRetryMs = 600'000;
        int batteryWaitMs = 3000;
    };

    KvmdDevice(Where where, QString password, QObject* parent = nullptr);
    KvmdDevice(Where where, QString password, Pace pace, QObject* parent = nullptr);
    ~KvmdDevice() override;

    const Where& where() const { return m_where; }
    // The times it keeps: for whoever stands beside it and keeps a time of its own by one of them.
    const Pace& pace() const { return m_pace; }

    // Signs in and reads what the KVM says of itself: what adding one begins with.
    struct Identity {
        QString model;
        QString version;
        QString hostname;
    };
    void identify(std::function<void(base::Result<Identity>)> done);

    // How it is, as last known: "connecting" until it has answered or failed once. Read anew by
    // look(), which does nothing while a look is under way; isStale() says when one is due.
    //
    // Where the KVM's state says nothing of its video (one maker's says none of it while the
    // video rests), what the last picture said stands in, and is said at once (statusChanged):
    // whether the computer sends a picture, by a picture that came with kvmd's word on that;
    // and how large that is, by the computer's picture as the KVM captured it, since one that
    // the KVM made smaller for a card has another size than the computer's. Never the other way
    // round: what the state says, is how it is. Each of the two stands in at a look only where
    // the picture that said it is no older than Pace::restAfterMs by then: a size that one
    // picture as captured said is not kept new by the small pictures that come after it. And
    // only of the computer the picture showed: what was seen is dropped when this driver puts
    // another on the KVM's screen, and when a look finds another there than this driver knew of.
    // A Fingerbot paired with it (Status::fingerbot) where the KVM said so on its line for events, kept through the
    // line's end and for a driver made anew; its battery asked by a look once an hour, low below kvmd::lowBattery.
    Status status() const override;
    bool isStale() const override;
    void look() override;
    // Runs `then` when the look that is under way is over, or after `withinMs`, whichever is
    // first; `then` is told whether a look is still under way. A turn of the loop later where
    // none is under way. Not at all once the device is gone.
    void afterLook(int withinMs, std::function<void(bool stillLooking)> then) override;

    // A picture of its screen for a card `width` pixels wide. Nobody waits long for it: a KVM
    // that is away, or slow, is said at once ("no_stream" where asking again soon is the thing
    // to do), and what a slow KVM sends after that goes to whoever asks next. An asking that
    // follows closely on another is answered with the picture that one brought: two windows
    // that show one card are one question to the KVM.
    void askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done) override;
    // The same, for whoever has the KVM live and takes its pictures one after the other: never
    // answered with a picture that somebody was handed before. It is a question to the KVM
    // each time, unless one for that size is under way, which it joins (the KVM is not asked
    // twice at once for one size: a card's question for another size runs beside it), or a
    // picture has come that nobody was handed yet. That one only while it is new: asked of the
    // KVM after this asking began, or come no longer ago than Pace::freshMs. One that is older
    // is thrown away, and the KVM is asked. How often is the caller's to bound.
    void askNewSnapshot(int width, std::function<void(base::Result<Snapshot>)> done);
    // Its live picture, as its stream of JPEGs (kvmd_stream.h says what the stream promises).
    // Asked as every question is: signed in first, the certificate looked at before a byte goes
    // out, on a line of this driver's own. The stream is the caller's, and is over when this
    // device is. While one is open the KVM's video is kept awake, and for a moment after it has
    // ended (Pace::restAfterMs), since whoever was told to ask again does.
    //
    // One at a time. While a stream of this device is open or being opened, a second is refused:
    // what comes back then is a stream that asks the KVM nothing and says so a turn of the loop
    // later (ended, "busy"). Each open stream holds one of the six lines Qt keeps to a place
    // for as long as it is open, and six of them left none for a look or a button. Where several
    // windows show one KVM live, they share one stream: KvmdLive (kvmd_live.h) asks for it, and
    // hands its pictures to each of them.
    std::unique_ptr<KvmdStream> watch();
    // Has the KVM do something. The answer comes when it has done it.
    //
    // A press of its power board (atx_power, atx_power_long, atx_reset; rule 23 of the web's FOR-THE-DESKTOP-APP)
    // is answered once the board has let go of the button (Pace::pressHeldMs, longPressHeldMs), so that whoever
    // holds the machine as one thing meanwhile (the wall) holds it for the whole press; and a press that must change
    // the computer's power (a short one on a computer that is off, the long one on one that is on, by the last look)
    // once the board shows the change, Pace::seenWithinMs at the most: where it does not, the press was made all the
    // same, and is done with a note of what was seen. Only a 4xx is a no. An answer that says neither (none in time,
    // one that broke off, a 5xx), or this driver let go of while its click is out, holds the board for
    // Pace::boardAnswerMs and the button's hold from then, in BoardHolds, whoever asks, a driver made anew too: a
    // press meanwhile is refused ("busy") and the KVM is not asked. One press of this driver's at a time.
    //
    // A press of a Fingerbot paired with the KVM (fingerbot_power, fingerbot_power_long; rule 29, kvmd_fingerbot.cpp)
    // is held by the same rules, in the same hold: a Fingerbot's press and the board's on one machine hold each other.
    // It is answered once the Comet has said the Fingerbot took it, the press's own time has passed after that, and
    // the arm's way back (Pace::fingerbotBackMs). Pressed only where the KVM said one is paired, on the line for its
    // events this driver holds anyway while its picture is watched; where it has not said either way of late, a line
    // is opened before the press to ask it (Pace::fingerbotAskWithinMs); where it said "none", or nothing when it was
    // asked, a moment ago (Pace::fingerbotAskAgainMs), or it is stock kvmd, refused with no line opened (a line opened
    // to a Comet can wake its sleeping computer). One question to the Fingerbot at a time: a press waits for a
    // question of its battery that is under way (Pace::batteryWaitMs at the most).
    void act(const Action& what, std::function<void(base::Result<Done>)> done) override;
    // Whether an action of that kind presses the KVM's power board.
    static bool pressesTheBoard(QStringView kind);
    // Whether it is pressed by a Fingerbot paired with the KVM.
    static bool pressesTheFingerbot(QStringView kind);
    // Whether an error of act() leaves open that the KVM did it all the same: the question went
    // out and no answer came back. That is no refusal, and nothing is sent a second time on the
    // strength of it.
    static bool mayHaveBeenDone(const base::Error& error);
    // The computers the KVM has saved to wake over the network; none where it keeps no such list.
    void askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done) override;
    // The line that carries keyboard and mouse to the KVM (kvmd_console_line.h), for whoever
    // controls its computer: signed in first, as for every question, and made to know the KVM by
    // what this driver knows it by. Handed over as it is made: it opens by itself a turn later,
    // and says when it is in. Nothing is said on it here, and it keeps no video awake: what goes
    // to the machine is the caller's alone. It is the caller's, and outlives this where the caller
    // lets it. None is handed out, and the KVM is asked nothing, under a pass under which it
    // has refused the line although it had only just given that pass ("not_allowed": see
    // passWasRefused), until that has been a while.
    void openConsole(std::function<void(base::Result<std::unique_ptr<KvmdConsoleLine>>)> done);

    // Whether the KVM's mouse takes places (absolute) or steps (relative), as its last look found it (/api/hid,
    // `mouse.absolute`; nothing: not said, or never looked at). kvmd drops a move of the other kind. Followed, and
    // never switched by this program: the KVM's `mouse_output` is a setting of the device that its other users share
    // (rule 26 of the web's FOR-THE-DESKTOP-APP, `pointer_kind`). A look whose /api/hid did not come leaves it as it
    // was.
    std::optional<bool> mouseAbsolute() const { return m_mouseAbsolute; }

    // A view-only Janus signaling line, with the same identity and sign-in rules as
    // snapshots. A rejected video session can refresh authentication at most once per minute.
    void openVideoLine(std::function<void(base::Result<std::unique_ptr<DeviceLine>>)> done);

    // How a line is set up on which a KVM that is known by a certificate is to be known by
    // that alone: Windows is given nobody to vouch for anybody, objects to every certificate,
    // the KVM's own included, and so the program's own look at it is made for every line, the
    // ones Qt makes with no question waiting for them among them.
    static QSslConfiguration knownByItsCertificateAlone(QSslConfiguration line);

    // Whether the line that keeps its video awake is there, or being made.
    bool isKeptAwake() const { return m_awake != nullptr; }

    // What its power board said last on the line that keeps its video awake (its line for events, kvmd's /api/ws:
    // GLKVM on a real Comet says its board's whole state there at every change), where that line is held, was heard
    // from within Pace::eventsQuietMs, and has said it; nothing otherwise (kvmd::boardHeard has its shape). A look
    // reads the board there where it says it is there, instead of asking it (the web's `_board_heard`): one asker less
    // on a board that answers one asker at a time. No line is opened for the board alone.
    std::optional<QJsonObject> boardOnTheLine() const;

    // What it showed the last time it was refused for its certificate, on this driver's own line or on a
    // sign-in's; empty before that.
    QByteArray shownCertificate() const
    {
        return m_asker.shownCertificate().isEmpty() ? m_signInLineShowed : m_asker.shownCertificate();
    }
    // Lets go of what is only kept for whoever looks, once nobody has for a while.
    void tidy() override;
    // A person has just typed this password for the KVM at `where`: it is tried as for the first time,
    // whatever was refused before (SignInPacing::forget). For the boxes that sign in with one.
    static void tryAfresh(const Where& where, const QString& password, const Pace& pace);
    // What has it live for all who show it (kvmd_live.h).
    std::unique_ptr<OwnLive> makeLive() override;
    // The KVM refused this pass somewhere this driver cannot see: on a line it handed out
    // (openConsole), which ended "auth_failed" and says which pass it had (KvmdConsoleLine::pass).
    // Whoever holds such a line says so here before asking for the next one. The pass is
    // forgotten only if it is still the one this driver holds: the next question signs in anew,
    // under the rules for that (a refused password is not sent again for a while). A pass that
    // was replaced since is not thrown away for an older one's sake.
    //
    // Nor is one the KVM has only just given: the first that it gave after a line's refusal had
    // the one before forgotten, where no line was let in under it since; or one under which the
    // line was refused in this way before. Then it is not the pass that the KVM refuses but the
    // line: whoever is signed in may not use keyboard and mouse there. The pass is kept, since
    // every other question is served under it, and no password is posted for a line again;
    // openConsole hands out no line under that pass for a while (Pace::signInRetryMs, as long as
    // a stream that was refused so is left alone: kvmd_stream.h). What comes back says which of
    // the two it was: nothing, where the pass was forgotten or was not this driver's; or the
    // code and sentence for whoever asked for keyboard and mouse ("not_allowed").
    std::optional<base::Error> passWasRefused(const QByteArray& pass);

signals:
    // What status() says is not what it said.
    void statusChanged();
    // (And `looked` and `showedItsCertificate`, as every driver of a KVM of the wall says them:
    // own_device.h.)
    // The KVM has signed this driver in and given it a pass: the first time, and anew whenever
    // the pass before was given up (the KVM had forgotten the session, as where kvmd was started
    // anew; or a line said that it was refused). For whoever remembers something of the KVM that
    // may be otherwise after that.
    void passGiven();
    // Its power board said something new on its line for events (boardOnTheLine), or that line ended: for whoever
    // waits for the board's next word.
    void boardSaid();

protected:
    // Asks for the line on which a KVM's video is kept awake, as a browser that watches it has
    // one. (Not private, so that a test can have it asked for by itself: the pass goes out with
    // the asking, and where it goes is what the test is about.)
    void keepAwake();

private:
    // A stream is this driver's question, read as it comes: it asks through what is private here.
    friend class KvmdStream;

    // A question as every family's driver asks one (device_asker.h), and what this driver does with
    // it beside that.
    struct Ask : DeviceAsker::Ask {
        Ask()
        {
            largest = kvmd::largestReplyBytes;
            // (And kvmd's word on whether the computer sends a picture, which its answers carry:
            // Reply::online.)
            headers.append(QByteArrayLiteral("X-UStreamer-Online"));
        }
        bool urgent = false; // somebody has just asked for it: not held back
        bool vital = true;   // unanswered, the KVM counts as away
        // Told each time the question is handed to the line to the KVM (it may reach the KVM from then on): once
        // more where it is sent a second time, signed in anew.
        std::function<void()> goingOut;
    };
    struct Reply {
        int status = 0;
        QByteArray body;
        QByteArray kind;   // what it says its body is, in small letters and without what follows a ";"
        QByteArray online; // kvmd's header that says whether the computer sends a picture
        QList<QNetworkCookie> cookies;
    };
    struct Tries {
        bool relogged = false; // signed in anew after the KVM refused the session
    };
    struct Frame {
        Snapshot picture;
        qint64 askedAt = 0; // when the asking that had it fetched began
        qint64 cameAt = 0;  // when it came
    };

    // Sends one question out, as every question goes: straight to the KVM, within its limits,
    // the certificate looked at first. The answer is the caller's to read, and to end
    // (deleteLater) when it is over. Not sent at all: what is saved as its certificate is none;
    // or the KVM's address is an HTTPS one and TLS is not Windows' own (tls.h).
    base::Result<QNetworkReply*> sendOut(const Ask& question, const QByteArray& token);
    // Why a question that is over got no answer of the KVM's own, in the driver's words: the line,
    // the certificate, or a limit that was marked on it. Nothing where the KVM answered,
    // whatever it said.
    static std::optional<base::Error> whyNoAnswer(const QNetworkReply* reply);
    // One question and its whole answer.
    void send(const Ask& question, const QByteArray& token, std::function<void(base::Result<Reply>)> done);
    // One question of somebody who is signed in: signs in where that is needed, and once more
    // where the KVM has forgotten the session.
    void request(const Ask& question, std::function<void(base::Result<Reply>)> done);
    void request(const Ask& question, std::function<void(base::Result<Reply>)> done, Tries tries);
    // The same, and the result of a good answer: what most questions want.
    void ask(const Ask& question, std::function<void(base::Result<QJsonObject>)> done);
    void withToken(std::function<void(base::Result<QByteArray>)> done);
    // Where this driver's sign-ins are counted, and the mark of the one it would offer now.
    SignInPacing& pacing() const;
    SignInPacing::Mark offered() const;
    SignInPacing::Steps steps() const;
    // What stands against offering the password now, noted by any driver of this KVM, this one or
    // one before it: a no (said in its words) or a sign-in that came to nothing.
    std::optional<base::Error> heldSignIn() const;
    // The KVM refused the session `used`. Nothing: it is forgotten, and the question is asked
    // once more, signed in afresh. Or why not: that was done already, and it refuses still.
    std::optional<base::Error> sessionRefused(const QByteArray& used, Tries& tries);
    // The KVM says that its video rests: the line that keeps it awake is asked for, and whoever
    // wanted a picture is told to ask again soon.
    base::Error resting();
    // A sign-in goes on a line of its own (`line`), made for it, the question asked first with it: one that
    // outlives this driver where it is let go of while the sign-in is out (~KvmdDevice), so that what it comes
    // to is heard all the same and told to the pacing (cameOf), which ends its hold.
    struct SignInOut;
    struct SignInCame;
    void signIn();
    void sendSignIn(DeviceAsker* line, const Ask& login);
    static SignInCame cameOf(const SignInOut& out, const base::Result<DeviceAsker::Reply>& reply);
    void signInCame(const SignInCame& came);
    // Another driver of this KVM has the same password out, and nothing else stands: what it comes to
    // is waited for, and then looked at again; and whether that may be done now.
    void afterTheOtherSignIn();
    bool mayWaitForTheOther() const;
    void signedIn(const base::Result<QByteArray>& token);
    // The KVM showed the certificate it is known by, on one of this driver's lines: said once.
    void showed(const QByteArray& der);

    void answered();
    base::Error failed(const base::Error& error, bool inPassing);
    std::optional<base::Error> heldBack() const;

    void lookFurther(const QJsonObject& streamer);
    // What an answer to GET /api/upgrade/version says (the web's `_glinet_version`): GL.iNet's account of the KVM
    // where it is GL.iNet's firmware, nothing where it is not or says neither; and the KVM's family, noted on the way
    // (m_glinet). An error where nothing answered: then nothing is noted, and it is asked again.
    base::Result<std::optional<QJsonObject>> familyFrom(const base::Result<Reply>& reply);
    // A look whose board said it is not there, or said nothing, keeps the board as it was a moment ago
    // (Pace::boardKeptMs, powerKeptMs); one whose board said it is there is noted for that.
    void keepTheBoard(Status& status);
    void lookOver(Status status);
    // A look found that it could not ask: said as what that is (a KVM that is there and does not sign
    // this program in is no KVM that is away).
    void lookFailed(const base::Error& why);
    // Whether the KVM is there at all, asked with no password and no session where it was not heard
    // from of late: the error where it is gone, nothing where it is there.
    void stillThere(std::function<void(std::optional<base::Error>)> then);
    // A picture has come: what it says of the computer's video. `signal`: whether the computer
    // sends a picture, where the picture came with kvmd's word on that. `size`: of the computer's
    // own picture, where this one came as captured and is the computer's; else none.
    void pictureSays(std::optional<bool> signal, QSize size);
    // Puts what the last picture said where the state of a look said nothing; true: it did.
    bool standIn(Status& status) const;
    // Another computer is on the KVM's screen than what was seen showed: the pictures that are
    // kept, what the last one said, and what a picture that is on its way will bring.
    void forgetWhatWasSeen();
    // Whether a picture of the KVM has come within the last moments, so that its video is awake:
    // no longer ago than what is known of a KVM counts as how it is (Pace::freshMs). For a
    // stream's question that a gateway answered in the KVM's place (KvmdStream).
    bool sawItsVideoAwake() const;
    void expectUp(const std::function<void(std::optional<base::Error>)>& then);
    // `anew`: for askNewSnapshot, which is not handed a picture somebody had before.
    void askPicture(int width, bool anew, std::function<void(base::Result<Snapshot>)> done);
    void picture(const QString& size, qint64 arrived, bool anew,
                 const std::function<void(base::Result<Snapshot>)>& done);
    void fetch(const QString& size, qint64 asked);
    void landed(const QString& size, const base::Result<Snapshot>& result);
    void later(std::function<void()> what);
    qint64 now() const { return m_clock.elapsed(); }

    // The power board (kvmd_board.cpp). Where its holds are kept, and what this KVM is found by there.
    BoardHolds& boards() const;
    BoardHolds::Of boardOf() const;
    // A press, from the asking to the answer; and after the board answered yes and the button's hold is over, the
    // change it must bring looked for (`before`: whether the computer was on before it, where that is known).
    void pressBoard(const QString& kind, Ask click, std::function<void(base::Result<Done>)> done);
    // The click itself, once the board said it is not busy: `before`, whether the computer was on before it.
    void sendTheClick(const QString& kind, Ask click, std::optional<bool> before,
                      const std::function<void(base::Result<Done>)>& over);
    // `then`, once the board's hold as BoardHolds keeps it is over.
    void afterTheButtonsHold(const BoardHolds::Of& board, const std::function<void()>& then);
    // What the board says at this moment, before a press: asked all the same where its line says it (a Comet's line
    // says its board as it read it last, and `busy`, half a second long, was never seen there), a second time where
    // the first answer says there is no board; busy where either says so; the computer's power by the answer, else by
    // the line. (The web's `_board_now(ask=True)`.)
    void askTheBoard(const std::function<void(std::optional<bool> on, bool busy)>& then);
    void askTheBoardAgain(int left, const std::optional<QJsonObject>& heard,
                          const std::function<void(std::optional<bool> on, bool busy)>& then);
    // `words`: how many words the board had said on its line when the click was answered.
    void afterTheHold(const QString& kind, std::optional<bool> before, quint64 words,
                      const std::function<void(base::Result<Done>)>& over);
    // One look for the change a press must bring, and the answer once it is seen or the time is up.
    struct Watch {
        bool wanted = true;   // the computer's power, as it is to be after the press
        bool said = false;    // the board has said whether the computer is on since
        bool settled = false; // the press was answered
        quint64 words = 0;    // the board's words on its line when the click was answered: what it said before is no
                              // witness of what the press did
        std::function<void(base::Result<Done>)> over;
        void settle(const base::Result<Done>& result);
    };
    void lookForTheChange(const std::shared_ptr<Watch>& watch);
    void sawTheBoard(const std::shared_ptr<Watch>& watch, std::optional<bool> on);
    // A message on the line for events: kept where it is the board's state and says something new (boardSaid).
    void heardOnTheLine(const QByteArray& message);

    // A Fingerbot paired with the KVM (kvmd_fingerbot.cpp). What the KVM said of it, kept in BoardHolds beside the
    // hold.
    BoardHolds::Fingerbot fingerbotSaid() const { return boards().fingerbot(boardOf()); }
    // The KVM said whether one is paired, on a line for its events.
    void fingerbotHeard(bool paired);
    // A Fingerbot as the card says it: where the KVM said one is paired, and is not stock kvmd.
    std::optional<core::Fingerbot> fingerbotNow() const;
    // What stands against a Fingerbot's press now: a hold of either button, or a word of the KVM's of a moment ago
    // that none is paired, or nothing said when it was asked. `mine`: the press's own reservation
    // (m_fingerbotReserved), which stands against nothing while it is still the one out; 0: none yet.
    std::optional<base::Error> fingerbotNotNow(quint64 mine = 0) const;
    void pressFingerbot(const QString& kind, std::function<void(base::Result<Done>)> done);
    // `then` once no question to the Fingerbot is out (its battery's, of any driver of the KVM).
    void afterTheFingerbotsTurn(const std::function<void()>& then);
    // Asks, on a line of its own to the KVM's events, whether one is paired: what it said, or nothing where it said
    // nothing within Pace::fingerbotAskWithinMs; an error where the line could not be had.
    void askWhetherOneIsPaired(std::function<void(base::Result<std::optional<bool>>)> then);
    void clickTheFingerbot(const QString& kind, const std::function<void(base::Result<Done>)>& over);
    // Its battery asked in the background, where one is paired and it is time (Pace::batteryEveryMs).
    void batterySoon();

    Where m_where;
    // Its password, for as long as it signs in with it: its own, written over when it goes (wiped.h; from the outside
    // review, F5).
    SecretText m_password;
    Pace m_pace;
    // How it asks the KVM a question, as every family's driver does (device_asker.h).
    DeviceAsker m_asker;
    QElapsedTimer m_clock;

    QByteArray m_token;
    qint64 m_videoAuthRetryAt = 0;
    QList<QPointer<DeviceLine>> m_videoLines;
    bool m_signingIn = false;
    std::shared_ptr<SignInOut> m_signInOut; // the sign-in that is out, on its own line; none when none is
    bool m_toldItsCertificate = false;
    // When a sign-in's line last heard the KVM (by m_clock; -1: never), and what one refused for its certificate
    // was shown: as this driver's own line's are.
    qint64 m_signInLineHeardAt = -1;
    QByteArray m_signInLineShowed;
    // Whether the KVM was heard just now, on this driver's own line or on a sign-in's.
    bool heardJustNow() const;
    // What a sign-in's line that is over heard and was shown.
    void heardOn(const DeviceAsker& line, bool heard);
    QList<std::function<void(base::Result<QByteArray>)>> m_waitingForToken;
    // What came of the last sign-in of this driver's that let it nowhere, in its own words, and the
    // hold it noted then as the pacing kept it: heldSignIn says those words while that very hold
    // stands. Held here, until m_signInRetryAt, is only a session it was given and refuses.
    std::optional<base::Error> m_signInError;
    std::optional<SignInPacing::Kept> m_heldAs;
    qint64 m_signInRetryAt = 0;
    qint64 m_heardAt = -1; // by m_clock: when the KVM last answered anything

    int m_failures = 0; // attempts in a row that found it away
    qint64 m_downUntil = 0;
    std::optional<base::Error> m_downError;
    bool m_inPassing = false; // the failure came of something a person asked for, not of looking
    std::optional<qint64> m_lastOkAt;

    std::optional<Status> m_status;
    qint64 m_statusAt = 0;
    // What the last picture said of the computer's video, and when: for where a look's state
    // says nothing of it (status()). The size has a time of its own: of the last picture as
    // captured, which is the only kind that says one. And whether the last look's state said
    // each of the two.
    struct Sight {
        std::optional<bool> signal; // nothing: the picture came without kvmd's word on that
        qint64 at = 0;              // when the last picture came, whatever it said
        QSize size;                 // of the computer's own picture; none: no picture as captured has said
        qint64 sizeAt = 0;          // when a picture as captured last said so
    };
    std::optional<Sight> m_sight;
    bool m_stateSaysSignal = false;
    bool m_stateSaysSize = false;
    bool m_looking = false;
    quint64 m_look = 0;        // counts the looks
    quint64 m_awaitedLook = 0; // the look a picture last waited for
    int m_sourceWidth = 0;
    kvmd::About m_about;
    std::optional<qint64> m_aboutAt;
    std::optional<qint64> m_onePortAt; // when it last said that it has one computer
    // Which computer is on its screen as far as this driver knows: by its last look, or by having
    // put another there itself since.
    std::optional<int> m_portSeen;
    bool m_usbUnsure = false;            // another one is than was, and it has not found its keyboard there yet
    std::optional<bool> m_mouseAbsolute; // mouseAbsolute()
    // Whether it is GL.iNet's firmware (it answered /api/upgrade/version in GL.iNet's shape: true), stock kvmd (it
    // answered 404 there: false), or not known (not asked yet, or what it answered said neither, which leaves what was
    // known as it was). Only to a GL.iNet KVM does its session go in the address of a line to /api/ws as well as in
    // the cookie, as GL.iNet's own page sends it (SPEC.md section 8): stock kvmd writes the address of every request
    // into its log, where the session would stand for whoever reads it.
    std::optional<bool> m_glinet;
    // When its power board last said that it is there, and what it said then of the computer (keepTheBoard).
    std::optional<qint64> m_boardAt;
    std::optional<bool> m_boardPower;
    // A press of its board is under way (pressBoard); its click is out, by its number in BoardHolds (0: none is), and
    // the button's hold it asked for.
    bool m_pressing = false;
    quint64 m_pressOut = 0;
    quint64 m_pressReserved = 0; // the board held for a press of this one's from its look to its click's going out
    qint64 m_pressOutHeldMs = 0;
    // What the board said last on the line that keeps the video awake, and when that line was last heard from (by
    // m_clock; -1: never); how many new words the board said there, all lines together; and that count when the look
    // under way began (a word that comes meanwhile has the look's end leave the card due for another).
    std::optional<QJsonObject> m_lineBoard;
    qint64 m_eventsHeardAt = -1;
    quint64 m_boardWords = 0;
    quint64 m_boardWordsAtLook = 0;
    // A Fingerbot's press whose click is out, by its number in BoardHolds (0: none is), and how long it may hold the
    // button from when it went; a line opened to ask whether one is paired; a question of its battery of this driver's.
    quint64 m_fingerbotOut = 0;
    quint64 m_fingerbotReserved =
        0; // the buttons held for a press of this one's from its look to its click's going out
    qint64 m_fingerbotMayTakeMs = 0;
    std::unique_ptr<AwakeLine> m_fingerbotLine;
    bool m_askingBattery = false;

    QHash<QString, Frame> m_frames;
    QSet<QString> m_unseen; // the sizes whose picture nobody was handed yet
    QHash<QString, std::pair<qint64, base::Error>> m_frameErrors;
    QHash<QString, qint64> m_fetching; // the sizes a picture is on its way for, and since when
    QHash<QString, QList<std::function<void(const base::Result<Snapshot>&)>>> m_waitingForFrame;
    quint64 m_screen = 0; // counts the times another computer came onto its screen (forgetWhatWasSeen)
    qint64 m_wantedAt = 0;
    qint64 m_awakeSince = 0; // when the line that keeps it awake was asked for
    std::unique_ptr<AwakeLine> m_awake;
    QTimer m_awakeTimer;
    // The stream of pictures that is open or being opened, which is its caller's: one at the
    // most (watch()).
    QList<KvmdStream*> m_streams;
    // The session under which the KVM refused its stream although it had only just given that
    // session, and until when no stream is asked for under it again (KvmdStream).
    QByteArray m_streamRefusedUnder;
    qint64 m_streamRefusedUntil = 0;
    // The same for the line for keyboard and mouse (passWasRefused). A line's refusal has had
    // the pass forgotten, and the next that is given is the one "only just given"; which pass
    // that is, until a line was let in under it; and the pass under which the line itself is
    // what the KVM refuses, and until when no line is handed out under it.
    bool m_lineHadThePassForgotten = false;
    QByteArray m_givenAfterALinesRefusal;
    QByteArray m_lineRefusedUnder;
    qint64 m_lineRefusedUntil = 0;
};

} // namespace qkvm::core
