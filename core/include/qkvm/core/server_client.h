#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QElapsedTimer>
#include <QIODevice>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/live_link.h>
#include <qkvm/core/server_details.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/wall_source.h>

class QNetworkReply;
class QNetworkRequest;

namespace qkvm::core {

// Every call to a QKVM server, in one place: nothing else in the program builds an address or reads
// an answer (DESIGN.md section 4). Each call has a limit on its time and on the size of its answer,
// follows no redirect, and gives its answer as a type or the reason there is none.
//
// On the interface thread. A call comes back at once; what it asked for is handed to `done` later,
// on the same thread, exactly once, unless the client is gone by then. The session is the client's:
// a sign-in leaves it holding the server's cookie, and the calls after it carry it. A session goes
// to the server it came from and to no other: naming another server lets go of it. The client
// holds it itself and takes it from the answer to a sign-in alone: Qt's store of cookies is not
// used, since any answer that is still on its way writes into that one, a sign-out's that comes
// after the next sign-in among them.
//
// Over HTTPS the server has to be one of two: one whose certificate Windows itself trusts for that
// name, or one that shows exactly the certificate it is known by here (setKnownCertificate). The
// certificate is looked at when the two have agreed on how to speak and before a byte of a question
// goes out; a server that is neither hears nothing, and what it showed can be asked for.
//
// After contact was lost, the session goes to nothing that has not said what it is. Contact is
// lost by what says something of who answers, and by nothing else. That is no answer at all:
// the line refused, fell (also under an answer that had begun), or nothing came in a call's
// time. And an answer that is no QKVM's where a QKVM's always has a shape: a refusal that is
// not in the server's own shape (a gateway's page, another program's, a 401 among them); an
// answer other than a picture that is cut off for its size, or that dribbles for longer than a
// whole call may take; a wall, a list of how the machines are, or a session that is none; a
// picture that does not say it is a JPEG or a PNG, or does not begin as one; a yes to something
// that was to be done, said with what is not understood; and what is said at the address of what
// answers there that is no QKVM's word about itself. It is not lost by a picture that is larger
// than is taken, or that the server is too slow to give: a machine's screen may be that large,
// and the server fetches the picture from the machine while it is asked. That picture fails,
// and nothing else.
// From then on every call that would carry the session first has what answers at the address
// asked what it is, as askHealth asks, with nothing that says who asks; calls that come while
// that is asked wait for the one answer. Behind the yes of a QKVM that speaks this program's
// version they go out: a yes to a question that was asked after contact was lost, whoever asked
// it. (One that was asked before that says nothing of what answers now.) Otherwise they are not
// sent (notSent; a certificate that is not the server's, and a QKVM that speaks another version
// of its API, are told as that), and the next call asks again: so a server that is away is
// asked what it is as often as it was asked anything before, and no oftener. The rule is here
// because the session is: the wall's looks and pictures, a button, the wall's order and every
// other call pass this class, and none of them has to remember to ask. (What answers as a QKVM
// answers is taken for one: who it is, its certificate says.)
// What has left when contact is lost, and is answered already or is a POST, went with its
// session: it is not given up, and is answered or fails by itself. What has not left this PC
// yet is taken back (lostContact): Windows goes on trying to reach a place for seconds on behalf
// of a question, and Qt keeps what it has no line for, and either would hand it to whatever
// answers there by then. So is what has left and is not answered yet, where it is of what Qt
// sends again by itself (below): Qt's line for that once more may be one Windows is still trying
// to make when something else begins to listen there. Of all that, what does something is
// given up, and what only asks is asked anew behind the question for what answers there now,
// so that whoever asked hears nothing of it. So the rule does not keep back: whatever had been
// answered or was a POST when contact was lost, the call that found it lost among them; what Qt
// gets a line for before the taking back has reached it (a turn of the loop, and then the way to
// Qt's own thread), or sends in the moment before it says that a question left; and what Qt asks
// again by itself before contact is seen to be lost. Where a line falls before an answer, Qt
// sends a GET, and a PUT (the wall's order), again inside itself, and this program hears nothing
// of it; a POST it sends once (seen, and held by a test). Nothing of QNetworkRequest's or
// QNetworkAccessManager's turns that off in the Qt this is built with (6.12: no attribute of a
// request and no setting of the manager names it; it is the reconnecting of its private
// channel; nor does Qt say that it begins to make the new line, as it says of the first one), so
// those questions reach whatever listens at the address by then, with the session, where nothing
// had said by then that contact is lost. A sender of this program's own, as the line to a KVM has
// one (device_line.h), would not ask twice: that is not built. Over plain HTTP only that would
// keep the session from what is not the server; over HTTPS its certificate does.
// One more thing Qt does by itself (seen): where the server hangs up under an answer that had
// begun, Qt fails with it every question to that server that still waits for a line of its own,
// each as a line the server hung up on, though none of them had left. That is contact lost here,
// as the answer that broke off is; the one case in which the answer itself is not (a picture
// that this program cut off for its size, with the server hanging up as well) loses contact by
// those others all the same.
//
// It is one of the two places a wall's machines come from (wall_source.h): what a wall asks of
// such a place, it asks a server through this.
class ServerClient : public QObject, public WallSource {
    Q_OBJECT

public:
    explicit ServerClient(QObject* parent = nullptr);

    // Which server: what was typed, with "http://" put before it where no scheme was. An address
    // that is typed with "https://" is never spoken to in the clear. Refused: anything that is not
    // an address of a web server, and one with a name and password in it ("invalid"); and an
    // HTTPS address where TLS is not Windows' own (tls.h, with the code and the sentence there).
    // A server that is refused is not named: the one before stays the one that is spoken to.
    base::Result<void> setAddress(const QString& typed);
    QUrl address() const { return m_address; }
    // The same reading of what was typed, for whoever has to look at an address before it is used.
    static base::Result<QUrl> addressFrom(const QString& typed);
    // An address as somebody would type it again: without "http://", with "https://".
    static QString asTyped(const QUrl& address);
    // Whether what goes to this server is encrypted on the way.
    bool isEncrypted() const { return m_address.scheme() == QStringLiteral("https"); }

    // What the server says of itself, to anybody: asked first of all, before the session is asked
    // about or a password goes anywhere (SPEC.md section 6.1). It needs no session and carries
    // none: who answers at the address is not known yet. What answers and is no QKVM gives
    // "bad_answer", whatever it sent. A QKVM that does not speak this program's version of the API
    // (spokenApi) gives otherVersion, and nothing more is to be asked of it.
    void askHealth(std::function<void(base::Result<Health>)> done);
    void askSession(std::function<void(base::Result<Session>)> done);
    // Where the server refused a sign-in in its own words and said how long to wait (429 rate_limited, Retry-After:
    // the address is locked out for that long), none goes to it before that time, fifteen minutes at the most: it is
    // refused here, "rate_limited", saying how long is left. (Asking sooner would only be refused again.)
    void signIn(const QString& name, const QString& password, const QString& code, bool keepSignedIn,
                std::function<void(base::Result<Account>)> done);
    // Signed out here at once, whatever comes of it: the question names the session that is to be
    // ended, and nothing asked after it carries that session.
    void signOut(std::function<void(base::Result<void>)> done);
    // Hands in the key of a display link for a session, as the page on a wall screen does: the
    // screen is then signed in as what the link is for, and may look. A link that was taken back
    // or has run out gives the server's refusal, "bad_key".
    void openDisplay(const QByteArray& key, std::function<void(base::Result<Account>)> done);
    // How long the server asked to be left alone when it last refused a display link's key, in
    // milliseconds: what `Retry-After` said in whole seconds, on a refusal in the server's own
    // shape, and never more than longestLinkWaitMs, whatever it said. 0: it named no time, or
    // took the key, or what answered was not the server (a gateway's page says nothing of when
    // the QKVM behind it is back). For whoever hands the key in again by itself.
    int linkWaitSaidMs() const { return m_linkWaitSaidMs; }
    // The longest that a display link's key waits to be handed in again, whoever says so: five
    // minutes.
    static constexpr int longestLinkWaitMs = 300'000;
    // The key in a display link, where what was typed is one (an address that ends in
    // "#display&key=qkvm_..."); empty where it is none. The address is read from the same text
    // with addressFrom.
    static QByteArray displayKeyIn(const QString& typed);

    // The certificate this server is known by, as its SHA-256; empty: none, and only a server that
    // Windows trusts is spoken to. Naming another server forgets it.
    void setKnownCertificate(const QByteArray& sha256);
    // What the server showed the last time it was refused for its certificate; empty before that.
    QByteArray shownCertificate() const { return m_shownCertificate; }
    // A certificate's SHA-256 as the server prints it when it starts: capitals, a colon between
    // every two.
    static QString written(const QByteArray& sha256);

    // The session a sign-in left, for whoever keeps it over the program's end; empty while there
    // is none.
    QByteArray session() const { return m_session; }
    // Carries on with a session that was kept. What is not of the shape of one is not taken.
    void setSession(const QByteArray& session);
    void askWall(std::function<void(base::Result<Wall>)> done) override;
    // A machine's history. The server reads it one at a time of each account (its W4b), and so does this: each read
    // waits for the one before it to be answered, whoever on this PC asked (two windows of the same server among
    // them); and where the server refused one in its own words with a time (503 busy, Retry-After), none goes before
    // that time, a minute at the most. Where another server was named, or whoever was signed in was signed out,
    // while one waited, it is not sent (notSent).
    void askHistory(const QString& machine, std::function<void(base::Result<HistorySummary>)> done);
    void askHistorySamples(const QString& machine, std::function<void(base::Result<HistorySamples>)> done);
    void askDeviceActivity(const QString& machine, std::function<void(base::Result<DeviceActivity>)> done);
    // Puts the wall's machines in an order: those named first, as named, and the rest after them
    // as they stood. It is the wall's own order, for everybody who looks at it; only an admin is
    // let. What comes back is the order of all the wall's machines as the server has it now.
    void arrange(const QStringList& machines, std::function<void(base::Result<QStringList>)> done) override;
    void askStatus(std::function<void(base::Result<QHash<QString, Status>>)> done) override;
    // A picture at least `width` pixels wide where the machine has that many, that may be up to
    // `mayBeOldSeconds` old.
    void askSnapshot(const QString& machine, int width, double mayBeOldSeconds,
                     std::function<void(base::Result<Snapshot>)> done) override;

    // The wall as a file, by the server's own three calls (see server_types.h). A file is made
    // for an admin who types their own password once more: `password` is that one, and a wrong
    // one is refused in the server's words, which is not the end of the session. `passphrase`:
    // what the file is encrypted with; empty: a plain file without secrets. `parts`: of
    // wallFileParts(). Sealing and opening a file takes the server a while: these calls are
    // given a minute, and taking a file in two.
    void exportWall(const QString& password, const QString& passphrase, const QStringList& parts,
                    std::function<void(base::Result<WallFile>)> done);
    // What a file holds, and what of it would be new on this wall. Changes nothing.
    void readFile(const QByteArray& file, const QString& passphrase,
                  std::function<void(base::Result<FileContents>)> done);
    // Takes in the named parts of a file: what is not on the wall yet is added, nothing that is
    // there is changed or removed, and the alert and wall settings are replaced where named.
    void takeFile(const QByteArray& file, const QString& passphrase, const QStringList& parts,
                  std::function<void(base::Result<Taken>)> done);

    // A machine live (SPEC.md section 6.2), in two steps. First the server is asked for a pass,
    // as it is asked everything: with the session, over a line whose certificate was looked at.
    // A server that has no live pictures, or none of this machine, refuses in its own words.
    // Then the line itself is opened, and the pass is its first word: the session never goes
    // over that line. The line is the caller's from then on, and says itself what becomes of it.
    // An account has as many lines at the most (32, the server's W2b): where the server refused a pass in its own
    // words with a time (429 busy, Retry-After), none is asked for before that time, of any machine, five minutes at
    // the most: refused here in the words it was refused in.
    // (Virtual as a test seam only: the live-link tests hand a pass in by overriding it. Nothing in the
    // program derives from ServerClient.)
    virtual void askLive(const QString& machine, std::function<void(base::Result<QByteArray>)> done);
    std::unique_ptr<LiveLink> openLive(const QString& machine, QByteArray pass, LiveLink::Need need,
                                       LiveLink::Watch watch = {});
    // The two steps in one, as a wall asks for a machine live: the pass, and then the line. A
    // pass goes to the server that gave it and to no other: where another server was named
    // before the pass came, no line is opened, and that is told as a server that was not reached.
    void openLive(const QString& machine, Live::Need need,
                  std::function<void(base::Result<std::unique_ptr<Live>>)> done) override;

    // Has something done to a machine. The answer comes when the machine has done it, and that can
    // be long: a power button is held for seconds, a text is typed key by key. So this call is
    // given far longer than the others before the server counts as not answering: two and a half minutes, and Start
    // into BIOS setup ("bios_setup", whose controller may be slow to come to its restart) four, as the page waits.
    // Only the server's own no is a no here. Told as no answer of the server's (`unreachable`):
    // an error from what stands before it (500 and above, in anything but the server's own
    // shape: a gateway's 502 or 504 in the middle of a long action), as the driver for a KVM has
    // it ("action_unknown"); an answer that was cut off here for its size, whatever its number,
    // since nobody read whether it was a no; and a yes (200) said with what is not understood,
    // which is contact lost as well.
    void act(const QString& machine, const Action& what, std::function<void(base::Result<Done>)> done) override;
    // Whether an error of act() leaves open that the machine did it all the same: no answer came
    // from the server, or the server lost sight of it on the way (its own "timeout"), or the
    // machine's answer to it broke off there (its own "dropped"), or the server failed inside after
    // it may have gone out (its own 500, "internal": from the merged review, 5). That is no refusal, and what was
    // asked is not asked a second time on the strength of it: a power button pressed twice undoes
    // the first press. (A machine the server could not reach at all did nothing: a refusal. So
    // did one that was never asked: notSent.)
    // (The same of every machine: the server's own words say it.)
    bool mayHaveBeenDone(const QString& machine, const base::Error& error) const override;
    // The actions saved on the server for any machine. For those who may act.
    void askSavedActions(std::function<void(base::Result<QList<SavedAction>>)> done) override;
    // Where a browser enters a machine's own console, signed in: a pass the machine's controller
    // makes out, good for one use. For those who may act, and only for a machine that says it hands
    // such passes out. Making one out is written down on the server.
    void askConsole(const QString& machine, std::function<void(base::Result<QUrl>)> done) override;
    // The computers a KVM has saved to wake over the network.
    void askWakeTargets(const QString& machine, std::function<void(base::Result<QList<WakeTarget>>)> done) override;
    // A PC with Intel AMT put in another of its three views: PUT .../view {view}, an operator's or
    // an admin's, signed in in person (the server refuses a token, 403 "not_allowed"). In effect at
    // once, and written in the server's activity log. What comes back is the view it is in now.
    void setView(const QString& machine, const QString& view, std::function<void(base::Result<QString>)> done) override;
    // The picture that set off an alert of a machine's screen watch (b7), by the stamp its entry in the
    // activity log names: GET .../alert-picture/{stamp}, an admin's. A JPEG or a PNG within the
    // picture limit (largestPictureBytes), as a snapshot is; "no_picture" (404) once the server keeps
    // it no more. A stamp that is not of the server's shape (only digits) is not asked for.
    void askAlertPicture(const QString& machine, const QString& stamp,
                         std::function<void(base::Result<QByteArray>)> done);

    // A machine's virtual media (0.3.0): an admin's, signed in in person. The server refuses a token, and anybody
    // else, in its own words, as it refuses a machine that has none ("no_virtual_media"). `machine` is the machine's
    // id; `named` what the question named it, which the server holds to (409 "changed" where it is called otherwise
    // by then: nothing was done).
    // The images that can be mounted on it and what is mounted, asked of the machine now.
    void askMedia(const QString& machine, std::function<void(base::Result<MediaList>)> done);
    // Mounted, taken out, deleted: answered as act() is (only the server's own no is a no, and whatever else leaves
    // open whether it was done is told as no answer of the server's, `unreachable`), and given as long as the page
    // gives them: seven minutes, for a server's mount waits for its drive to show, its answer and its task.
    void mountMedia(const QString& machine, const MediaMount& mount, std::function<void(base::Result<MediaDone>)> done);
    void unmountMedia(const QString& machine, const QString& image, const QString& named,
                      std::function<void(base::Result<MediaDone>)> done);
    void deleteMedia(const QString& machine, const QString& image, const QString& named,
                     std::function<void(base::Result<MediaDone>)> done);
    // An image carried to the machine's device as `name`, from `image` as it is read: a piece at a time as the line
    // takes it, kept nowhere on the way, with its length said first, 16 GB at the most (mostUploadBytes). `image` is
    // open for reading, and is read at a place (a file): what has no length of its own before it is read is not
    // taken (Qt would read all of it in first). Refused here, and nothing sent: an empty one, a larger one, and a
    // name the server does not take (isImageName). It is given as long as something comes or goes, six minutes of
    // nothing at the most (the server waits five for the device to say it has it all, once its last byte is out),
    // however long the whole takes; and it is answered as act() is. Driven by the network's own events on this
    // thread, which goes on meanwhile. What is handed back stops it: called before the answer, nothing more of it
    // goes, its line is closed, and `done` is told uploadStopped (what came of it the server takes off).
    std::function<void()> uploadMedia(const QString& machine, const QString& name, const QString& named,
                                      std::unique_ptr<QIODevice> image,
                                      std::function<void(base::Result<MediaUploaded>)> done);
    // How far the upload under way to it has come, by the server's count; nothing: none is.
    void askUpload(const QString& machine, std::function<void(base::Result<std::optional<MediaUploading>>)> done);
    // For a test: how long an upload may go without a byte either way before it is cut off.
    void setUploadQuietMs(int ms) { m_uploadQuietMs = ms; }

    // What this program finds itself, as the code of the error a call gives. Much hangs on each
    // (whether something may have been done, whether the wall is left, whether a certificate is
    // asked about), so none of them can be said by a server: they are written with a colon, and a
    // server's answer that is written so is not taken as a word at all (refusalFrom). The server
    // has words of its own that read alike ("unreachable" is its word for a machine it cannot
    // reach), and those are refusals like any other.
    //
    // The server could not be reached at all, did not answer in time, or its answer broke off;
    // and, of something that was to be done to a machine, whatever else is no answer of the
    // server's own (act). Everything else a call gives is one of those below, the server's own
    // refusal, "refused" (a refusal that is not in the server's shape), "too_large" or
    // "bad_answer".
    static constexpr const char* unreachable = "desktop:unreachable";
    // The session is gone: the server answered 401, in its own shape, to a call that is no
    // sign-in. (A 401 in any other shape was said by something else: "refused", contact is lost,
    // and nobody is signed out by it.)
    static constexpr const char* signedOut = "desktop:signed_out";
    // The server showed a certificate that Windows does not trust, and none is known for it here.
    static constexpr const char* certificateUnknown = "desktop:certificate_unknown";
    // The server showed another certificate than the one it is known by.
    static constexpr const char* certificateChanged = "desktop:certificate_changed";
    // The server showed the certificate it is known by, and somebody else signed that one, whom
    // Windows does not trust. A server is known by a certificate only where it made the
    // certificate for itself; one that somebody signed is let in where Windows trusts the signer.
    static constexpr const char* certificateSignerUnknown = "desktop:certificate_signer_unknown";
    // A QKVM answered, and the version of its API that this program speaks is not among those it
    // speaks: one of the two is older than the other.
    static constexpr const char* otherVersion = "desktop:other_version";
    // The call was not sent. Contact with the server had been lost, and nothing answers at its
    // address now, or what does is no QKVM; or the session the call was to carry was let go of
    // while that was asked. Nothing went anywhere with the session: this is no refusal of a
    // server's, and nothing that may have been done. (A QKVM that speaks another version of its
    // API is told as otherVersion, and a certificate that is not the server's as that: nothing
    // was sent then either.)
    static constexpr const char* notSent = "desktop:not_sent";
    // An upload was stopped by whoever began it (the stop uploadMedia hands back): nothing more of it went.
    static constexpr const char* uploadStopped = "desktop:stopped";
    // An upload's file could not be read on the way (core::ReadAhead::failure, in the message): nothing more of it
    // went, and what the server has of it is a part.
    static constexpr const char* imageUnreadable = "desktop:image_unreadable";

    // How many limits on a whole answer are kept at this moment: one for each call that is out,
    // and none that outlives its answer. (For the tests, which look that this is so.)
    int limitsAlive() const { return *m_limits; }

private:
    // `withSession`: false for the one question that is asked before it is known who answers.
    QNetworkRequest request(const QString& path, int limitMs, bool withSession = true) const;
    // How every line to a server is made, the client's own (m_network) and each one made for a question that changes
    // something (onALineOfItsOwn), from here and nowhere else, so that the two never differ: never through a proxy,
    // never somewhere else by itself. (All the rest is the question's own, the same for both: its session, its
    // certificate rule, its time and its HTTP, in request(); its certificate looked at and its size held, in finish().)
    static void prepare(QNetworkAccessManager& network);
    // A question that changes something (anything but a GET: an action, a sign-in, a pass, media) goes out on a line
    // made for it alone (b11), by a manager of its own made as m_network is (prepare) and gone with its answer. Never
    // on a line the client kept: a server closes one it kept idle (qKVM's own after five seconds), and a question
    // written on it just as it closes never reaches it; a GET is asked anew where that happens (sendOut). This program
    // never sends such a question twice; Qt sends a POST once, and a PUT (a view, the wall's order) again inside itself
    // if its new line falls before an answer, which is the same order again (see above). Every line it makes is let
    // go of after idleLineSeconds besides (request), which
    // keeps a line that was idle all that while from being used; one line of several, idle as long while another is
    // busy, would be used all the same (Qt lets go of a whole pool, not of one of its lines).
    QNetworkReply* onALineOfItsOwn(const std::function<QNetworkReply*(QNetworkAccessManager&)>& send);
    // The session a good answer to a sign-in brings, where the answer is to a question of this
    // `turn`: nobody has signed out and no other server was named since it was asked.
    void takeSessionFrom(const QNetworkReply& reply, quint64 turn);
    // Nobody is signed in here from now on, whatever answer is still on its way.
    void letGoOfTheSession();
    // The path of something of one machine's: "api/v1/devices/<the machine>/<what>".
    static QString ofMachine(const QString& machine, const QString& what);
    // Has the reply look at the server's certificate before anything is sent.
    void watchCertificate(QNetworkReply* reply);
    // What a call's answer is handed to: the body of a good one or why not, and the reply it came
    // by (none: the call was not sent).
    using Answer = std::function<void(base::Result<QByteArray>, QNetworkReply*)>;
    // Waits for the reply, within `largest` bytes, and hands over the body of a good answer or why not.
    void finish(QNetworkReply* reply, qsizetype largest, bool signInCall, Answer done);
    // Sends a call that carries the session, and waits for its answer as finish() does. `send`
    // makes the question and hands it to the network: at once while the server is in contact;
    // after contact was lost, only behind the word of what answers there now (see the class).
    // `takenBack`: how often this question was taken back before it left (sendOut).
    void ask(std::function<QNetworkReply*()> send, qsizetype largest, bool signInCall, Answer done, int takenBack = 0);
    // Something said that it may not be the QKVM that answers (see the class): contact is lost.
    // `of`: the server the call was for, by `m_serverTurn` as it was when the call was made;
    // what a call to a server before this one came to says nothing of this one. What has left
    // and is answered already, or is a POST, stays out: its session went with it, and giving it
    // up would take nothing back and end what a QKVM is answering. Taken back, a turn later, is
    // what has not left this PC yet (Windows goes on trying to reach a place for seconds), and
    // what has left unanswered that Qt would send again by itself: what does something is given
    // up, so that no password and no action follows a server that was replaced, and what only
    // asks is asked anew behind the question for what answers there (sendOut), so that no
    // session does.
    void lostContact(quint64 of);
    // Asks what answers at the address now, for every call that waits for that.
    void askWhatAnswersNow();
    // Hands a question that carries the session to the network, notes that it is out, and waits
    // for its answer (finish). A question that only asks and is taken back before it left
    // (lostContact) is asked anew from here, mostTakenBack times at the most: after that it is
    // told as given up, so that nobody waits without end on a server that comes and goes.
    void sendOut(const std::function<QNetworkReply*()>& send, qsizetype largest, bool signInCall, Answer done,
                 int takenBack = 0);
    static constexpr int mostTakenBack = 3;
    // What came back to something that was to be done, read as act() reads it: the body of a yes that is
    // `understood`, or why there is none. A yes that is not understood is contact lost (`of`: the server's turn).
    base::Result<QByteArray> doneOrNot(base::Result<QByteArray> answer, const QNetworkReply* reply, quint64 of,
                                       const std::function<bool(const QByteArray&)>& understood);
    // The server's own 500 to something that was to be done, as it is told: "internal", whatever its word, with its
    // sentence (mayHaveBeenDone).
    static base::Error itsOwnError(const base::Error& said);
    // A mount, an unmount or a deletion: posted to `path`, and answered as act() is.
    void changeMedia(const QString& path, const QByteArray& body, std::function<void(base::Result<MediaDone>)> done);

    // Windows' own TLS, on every PC: Qt would take OpenSSL where it happens to find one lying
    // about, and the program would then trust and refuse differently from one PC to the next.
    // Chosen once, by whichever of the program's speakers is made first (tls.h), and before
    // anything of Qt's that speaks is made, which is why it stands before that here. Where it
    // is not what is in use, no HTTPS address is taken (setAddress).
    const bool m_windowsOwnTls = useWindowsOwnTls();
    QNetworkAccessManager m_network;
    QUrl m_address;
    // The session the calls carry; empty: nobody is signed in here.
    QByteArray m_session;
    // Raised whenever the session is let go of: an answer to a sign-in that was asked before that
    // brings none any more.
    quint64 m_sessionTurn = 0;
    QByteArray m_knownCertificate;
    QByteArray m_shownCertificate;
    // Contact was lost, and what answers at the address has not said since that it is a QKVM
    // this program speaks to; that is being asked; and the calls that wait for its answer, each
    // told why it is not sent, or nothing. And a number that is another whenever another server
    // is named: what is said of the one before says nothing of this one.
    bool m_unsure = false;
    // How often contact was lost: what a QKVM says of itself brings contact back only where it
    // was asked after the last time (askHealth).
    quint64 m_lost = 0;
    bool m_askingWhatAnswers = false;
    QList<std::function<void(const std::optional<base::Error>&)>> m_waiting;
    quint64 m_serverTurn = 0;
    // The questions that carry the session and are out (and some that are over, until the next
    // one goes).
    QList<QPointer<QNetworkReply>> m_out;
    // How many limits on a whole answer are alive: counted where each goes, which may be after
    // this has gone.
    std::shared_ptr<int> m_limits = std::make_shared<int>(0);
    int m_linkWaitSaidMs = 0;
    // How long an upload may go without a byte either way (uploadMedia): longer than the server waits for its device
    // to say it has it all once the last byte is out (its UPLOAD_ANSWER, five minutes). A test makes it shorter.
    int m_uploadQuietMs = 360'000;

    // Where the server asked to be left alone (Retry-After), until when by m_clock: a sign-in, a live picture's pass
    // and its refusal then, and a history read. Of the server spoken to now: naming another lets go of them.
    QElapsedTimer m_clock;
    qint64 m_signInHeldUntilMs = 0;
    qint64 m_liveHeldUntilMs = 0;
    base::Error m_liveHeld;
    // Reads the server takes one at a time of each account: each waits for the one before it to be answered, and for
    // the time the server last named. `go` sends one, and is handed what to call with its reply when it is answered.
    struct InTurn {
        struct Waiting {
            std::function<void(std::function<void(QNetworkReply*)>)> go;
            std::function<void(const base::Error&)> unsent;
            quint64 serverTurn = 0;
            quint64 sessionTurn = 0;
        };
        QList<Waiting> waiting;
        bool out = false;
        qint64 heldUntilMs = 0;
        QTimer later;
    };
    InTurn m_historyReads;
    void readInTurn(InTurn& reads, std::function<void(std::function<void(QNetworkReply*)>)> go,
                    std::function<void(const base::Error&)> unsent);
    void nextInTurn(InTurn& reads);
    // Whoever waits there is told that nothing went, a turn later, and nothing of the server before is waited for.
    void forgetTheTurns(InTurn& reads);
};

} // namespace qkvm::core
