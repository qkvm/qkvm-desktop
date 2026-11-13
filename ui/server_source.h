#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/connection.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_feed.h>

#include "files_view.h"
#include "session_view.h"
#include "source.h"
#include "wall_screen.h"

namespace qkvm::ui {

// One QKVM server as a place the wall's machines come from: the program's whole conversation with
// it. Where it is with the server is one core::Connection; what an answer means is asked of
// core::after. It has the client that speaks to the server and the feed that keeps the server's
// wall (core::ServerClient, core::WallFeed), and it is the only listener to what the feed says of
// the server itself: contact lost and back, signed out, a certificate refused, a QKVM of another
// version. What the feed says of the wall, its machines and their pictures, the windows' wall
// hears, and nobody else (MergedWall): it tells this when the machines were looked at (looked).
//
// It remembers the server that was signed in to last, and, where whoever signed in asked for it,
// keeps the session over the program's end: the address in the settings, the session with the
// secrets, and nowhere else.
//
// A display link, pasted where the address is typed, signs in a screen nobody sits at, and this PC
// is a wall screen from then on, until a person signs it out here (markScreensIn). Its key is
// kept with the secrets, and it is what the screen comes back in with by itself when its session is
// gone: a session may not outlast the server's restart, the link does. And a screen that starts
// while its server is away goes on asking for it by itself, where the server is named, until one
// of these: the server takes the link, and the wall is up; it says that the link is not good, and
// somebody is asked to sign in; a QKVM answers there that speaks another version of its API;
// a certificate has to be looked at; somebody types there, names a server, or goes to this PC's
// own wall (standAside). Each handing-in of the link that a QKVM does not take, and does not call
// no good, has the next one wait twice as long (handTheLinkInLater). So has one that is taken and
// whose session does not hold: the first look at the machines under it is refused as signed out. A
// link counts as taken, and the waiting begins anew, only once such a look was answered.
//
// What a call's failure says of the server is the client's to tell (ServerClient). The wall says
// that contact is lost only where the look at its machines fails; and it is left only by the
// server's own word that the session is gone, by a certificate that is not the server's, or by
// a QKVM that came back speaking another version of its API, which is said where a server is
// named.
//
// What the windows do with its wall is not its own: it says what happened, each at the moment it
// happens (Source: entered, whoChanged, left, emptied, stateChanged), and whoever has the windows
// does their part there and then; its listeners are direct. What the screens are told of it is
// shown() (changed: it may be another now). Each answer of the server is taken only by the source
// that asked, in the turn it asked in.
//
// On the interface thread. The settings and the secrets outlive it.
class ServerSource final : public Source {
    Q_OBJECT

public:
    // `id`: what its machines' keys on the wall begin with (Source::id). How the machines are is
    // asked as often as the page asks it; a test asks more often. That is also the pace a server
    // that is away is asked again at.
    ServerSource(const QString& id, core::Settings& settings, core::SecretStore& secrets, int statusEveryMs = 4000,
                 QObject* parent = nullptr);
    ~ServerSource() override;
    ServerSource(const ServerSource&) = delete;
    ServerSource& operator=(const ServerSource&) = delete;
    ServerSource(ServerSource&&) = delete;
    ServerSource& operator=(ServerSource&&) = delete;

    QString id() const override { return m_id; }
    // The place its machines come from (the client), and the feed that keeps its wall.
    core::WallSource& place() override { return m_client; }
    core::ServerClient* detailsClient() override { return &m_client; }
    core::WallFeed& feed() override { return m_feed; }
    // Where the server is, as the client speaks to it.
    QUrl address() const { return m_client.address(); }
    // Opaque, memory-only partition for sharing live video; never a session token.
    QString liveIdentity() const;
    // Whether somebody is at its wall: signed in, with or without contact.
    bool isAtTheWall() const override;
    // At its wall, and contact is lost.
    bool isAway() const override { return m_state == core::Connection::Away; }
    // Whether a machine of it may be had live, as far as the server goes: while it answers.
    bool mayBeLive() const override { return m_state == core::Connection::SignedIn; }
    // Whether whoever is signed in is an admin.
    bool manages() const override { return m_manages; }
    // Raised with every question to the server, whenever its wall is left or it stands aside, and
    // where somebody stops it being asked again by itself (stopTryingAgain): an answer is to a
    // question, and an answer to an older one is nobody's any more.
    //
    // That is what guards each answer of the server's here. (Each also holds a QPointer to this,
    // which never finds it gone: the client is a member and ends before this does, and an answer
    // comes only through the client. They are kept as every other answer of the program has one.)
    quint64 turn() const override { return m_turn; }
    // What the screens are to be told of it, where its wall, or where the program is with it, is
    // the one that is shown.
    SessionView::Shown shown() const;

    // What the screens ask of it (SessionView), each as the screens ask it.
    void connectTo(const QString& address);
    // A status-only source uses saved sessions, never a display link or a password prompt.
    // Set before connecting. It does not change the last wall selected by the user.
    void setBackground() { m_background = true; }
    void verifyAccount(std::function<void(const QString&)> done);
    void signIn(const QString& name, const QString& password, const QString& code, bool keepSignedIn);
    // Yes to the certificate the server showed: it is known by it from now on, and asked again.
    void trust();
    // A person here signs out (while somebody is at its wall): and where this PC is a wall screen
    // of this server, this is the one way a person ends that screen (WallScreen). Whatever is to
    // be asked before a screen is signed out is to be asked in front of this.
    void signOut();
    // Somebody types where a server is named, or names one: a server that was being asked
    // again by itself is asked no more, and what it still answers is nobody's.
    void stopTryingAgain();
    // Back to saying which server: the wall is left, and the link that was spoken with stays kept
    // for that server, and is nothing to the next one.
    void leave();
    // This PC's own wall is gone to: away from this server's wall first, and from whatever was
    // being asked of the server: signed in there or not, that is left as it is (a session that was
    // kept stays kept). What the server still answers to a question from before means nothing, and
    // what was asked for a sign-in there (a code) is asked anew the next time.
    void standAside();
    // The machines were looked at, and the wall's cards show what was found (MergedWall): who is
    // signed in is asked with every fifteenth look, and a display link's taking holds.
    void looked() override;
    // What a window's box for the wall as a file asks, asked of the server for whoever is at its
    // wall. Each answer is told under the number it was asked under: the view knows whether it
    // still waits for it.
    void exportWall(const QPointer<FilesView>& asked, quint64 turn, const QStringList& parts, const QString& passphrase,
                    const QString& password);
    void readFile(const QPointer<FilesView>& asked, quint64 turn, const QByteArray& file, const QString& passphrase);
    void takeFile(const QPointer<FilesView>& asked, quint64 turn, const QByteArray& file, const QString& passphrase,
                  const QStringList& parts);
    // What time it is, where a test has a clock of its own: the wall says since when the server
    // does not answer.
    void tellTheTimeWith(std::function<QDateTime()> clock);
    // Where whoever is at this server's wall is a display link's, this PC is a wall screen from
    // then on, of this server; and where a person signs out here (signOut), that screen is over:
    // said to `screen` from now on (WallScreen says what else does not end it). Once; it outlives
    // this. (Without one, nothing is said: a test of the conversation alone.)
    void markScreensIn(WallScreen& screen) { m_screen = &screen; }

signals:
    // What the screens are told of it may be another now (shown). (Where the connection is, which
    // decides whether a machine may be had live, is said after it: Source::stateChanged.)
    void changed();

private:
    // Nobody is at the wall any more: everything that was shown and under way on it goes (left,
    // emptied), and who was signed in is forgotten.
    void emptyTheWall();
    // Whether a call failed because of the server's certificate; if so, everything with this server
    // stops until somebody has looked at it.
    bool stoppedForItsCertificate(const base::Error& error);
    // The first question to a server, before any other: whether a QKVM answers there, and one
    // that speaks this program's version of its API. Only such a one is asked who is signed in.
    void askWhatAnswersThere();
    void askWhoIsSignedIn();
    // The same while the wall is up, now and then: an admin may have made whoever sits here
    // somebody who may do more, or less, or given them another name.
    void askWhoItIsNow();
    // Under which names this server's session and its display link are kept.
    QString sessionName() const;
    QString displayName() const;
    // Signs in with the display link, where the server says nobody is signed in and there is one.
    void openByTheLink();
    // The same while the wall is up and its session is gone: the wall stays, and says that contact
    // is lost, until the link has opened another session or the QKVM has said that the link is
    // not good (whatever else is answered is tried again later). What answers there by then is
    // asked what it is first, and handed the link only if it is a QKVM this program speaks to.
    void comeBackByTheLink();
    void handTheLinkInAgain();
    // The server does not answer any more: said over the wall, with since when.
    void loseContact();
    void forgetTheLink();
    void rememberTheServer();
    // Whether `event` would change anything where the connection is now.
    bool means(core::ConnectionEvent event) const;
    // The event happened: the state that follows, and what the screens show of it.
    void become(core::ConnectionEvent event, const QString& problem = {});
    void enterWall(core::ConnectionEvent event, const core::Account& account);
    bool stopForEnrollment(const core::Account& account);
    void leaveWall(core::ConnectionEvent event, const QString& problem);
    // Whoever is at its wall is this account: where it is a display link's, this PC is a wall
    // screen of this server from now on (markScreensIn).
    void noteWhoIsThere(const core::Account& account);
    // Asks what answers at the server's address now, and runs `then` only where that is a QKVM
    // which speaks this program's version of its API. Before anything that says who asks goes
    // there: a kept session, a display link, a password. Where it is none, or nothing answers,
    // that is said where a server is named, and nothing more is asked.
    void ifAQkvmAnswersThere(std::function<void()> then);
    // Posts what was typed, once what answers there has said that it is a QKVM (signIn asks).
    void sendSignIn(const QString& name, const QString& password, const QString& code, bool keepSignedIn);
    // Whether the link was handed in a moment ago: less long ago than a server that is away is
    // left alone before it is asked again.
    bool handedTheLinkInJustNow() const;
    // The same question as ifAQkvmAnswersThere, while the wall is up: before the wall as a file
    // posts the password of whoever is signed in once more, or hands in a file and its
    // passphrase. Asked beside whatever else is asked of the wall's server, so with no turn of
    // its own, and where the session stands is not changed by its answer: that the server is
    // away, or another, the looks at the machines say. `then` only where a QKVM this program
    // speaks to answers and whoever asked is still at that wall; else `otherwise`, with
    // FilesView::nothingWasSent and the sentence for why.
    void ifAQkvmStillAnswersThere(std::function<void()> then, std::function<void(const base::Error&)> otherwise);
    // The server that was named is not come to: `problem` is said where a server is named.
    // And where it was named by the display link that is kept for it, which is a screen nobody
    // sits at to name it again, it is asked again by itself, from the first question, at the
    // pace a wall's server that is away is asked at (the wall's own: `m_again`), for as long as
    // nobody types there or names a server; and the screens say that it is. `mayComeYet`: not
    // where what answered is a QKVM that cannot be gone on with as it is. `afterMs`: how long
    // until it is asked again, where that is longer than the wall's pace (the link was handed
    // in and not taken: handTheLinkInLater); 0: at that pace.
    void notComeTo(const QString& problem, bool mayComeYet, int afterMs = 0);
    // Whether the link the server was named by is the one that is kept for it: it has opened a
    // session there before. (One that was pasted just now has somebody at it.)
    bool cameByItsKeptLink() const;
    // The display link was handed in, and what came of it is neither a session nor the QKVM's
    // word that the link is not good (no answer, a gateway's error, "too many", what is not
    // understood). How long until it is handed in again: the pace a server that is away is
    // asked at, twice as long for each such handing-in in a row, and five minutes at the most;
    // and never less than the server said to wait, where it said so in its own words, which
    // is five minutes at the most as well (ServerClient::linkWaitSaidMs). A link that is taken,
    // and under whose session a look at the machines was answered, begins that anew; one that
    // is taken and whose session does not hold counts as not taken (`why`: signed out). At the
    // wall's pace for ever, a link that a QKVM would not take was posted to it some twenty
    // thousand times a day.
    // `why`: what came of the handing-in. One that was not sent at all (ServerClient::notSent)
    // counts for nothing, and is tried at the wall's pace.
    int handTheLinkInLater(const base::Error& why);

    QString m_id;
    bool m_background = false;
    core::Settings& m_settings;
    core::SecretStore& m_secrets;
    core::ServerClient m_client;
    core::WallFeed m_feed;
    core::Connection m_state = core::Connection::None;
    // (turn())
    quint64 m_turn = 0;
    // What the screens are told beside the state: none of these decides what happens next.
    QString m_problem;
    QString m_account;
    bool m_manages = false; // whoever is signed in is an admin
    QString m_role;         // and what the server lets them do (SessionView::role)
    bool m_asksName = false;
    bool m_asksCode = false;
    // The server said of itself that it was restarted locked and that nobody has signed in since:
    // told where somebody signs in, until somebody has.
    bool m_locked = false;
    // Since when the server does not answer: of the last time it did not.
    QString m_awaySince;
    std::function<QDateTime()> m_now = [] { return QDateTime::currentDateTime(); };
    // The key of the display link for the server that is being spoken to: pasted with its address,
    // or kept from before. Empty: there is none, and somebody signs in.
    QByteArray m_displayKey;
    QTimer m_again;
    // How often the machines were looked at since the wall was come to: who is signed in is asked
    // with every fifteenth look, as the page asks, which is about once a minute.
    int m_looks = 0;
    QElapsedTimer m_clock;
    // When the display link was last handed in, by that clock; nothing: not yet.
    std::optional<qint64> m_linkHandedInAt;
    // The server that is named was not come to, and is asked again by itself (notComeTo): only
    // where no wall is up and nothing is asked of anybody, which is where a server is named.
    // What the screens are told of it is not "busy": another address can be typed meanwhile.
    bool m_tryingAgain = false;
    // The pace a server that is away is asked again at: the wall's own, how often the machines
    // are looked at. (`m_again` waits for that, or for longer where it is told to.)
    int m_awayEveryMs = 0;
    // How often in a row the display link was handed in and neither taken nor called no good.
    int m_linkNotTaken = 0;
    // The link was taken, and no look at the machines has been answered under its session yet:
    // whether that taking holds is not known.
    bool m_linkTakenUnproven = false;
    // Whom it says that this PC is a wall screen (markScreensIn); none: nobody.
    WallScreen* m_screen = nullptr;
};

} // namespace qkvm::ui
