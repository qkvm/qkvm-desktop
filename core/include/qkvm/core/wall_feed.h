#pragma once

#include <functional>
#include <optional>

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <qkvm/core/server_types.h>
#include <qkvm/core/wall_source.h>

namespace qkvm::core {

// Keeps one server's wall up to date, as the page does: which machines there are, how each is doing,
// and a new picture of each machine that is being looked at, at the pace the server names.
//
// What it asks, it asks the place the wall's machines come from (wall_source.h), and cannot tell
// which of the two that is: where "the server" is said here, this PC's own KVMs are meant as well.
//
// A picture is asked for only for a machine that is shown, at the width it is shown at; and never
// while the last one asked of that machine is still out: it is skipped, not queued (DESIGN.md
// section 4). A machine that has a picture is a KVM, or a server whose own screen the QKVM server
// reads from its controller.
//
// Four pictures are on their way at once and no more, each machine in its turn: of the six
// questions a program has out to one server at a time, two stay free, so that what somebody does to
// a machine never waits behind pictures. A machine whose picture could not be had is asked again
// after three seconds and not at the wall's pace; one whose stream is only being woken, a second
// later at the latest. As the page keeps all of it. On the interface thread.
class WallFeed : public QObject {
    Q_OBJECT

public:
    // How the machines are is asked as often as the page asks it, every four seconds; a test asks
    // more often.
    explicit WallFeed(WallSource& source, int statusEveryMs = 4000, QObject* parent = nullptr);

    // From a signed-in client. start() asks at once and then keeps asking; stop() ends that, and what
    // is still out is let run and ignored.
    void start();
    void stop();
    bool isRunning() const { return m_running; }
    // The wall of another place from now on. Only for a feed that is stopped: one that is running
    // does nothing here, and goes on asking the place it has. After it nothing is known of the wall
    // that was: not its machines, not how they were, not what it named (the pace, the form of the
    // live connection), and nothing that was asked of it counts; where it had machines, that it
    // has none now is told as any other change of the wall. What whoever looks has chosen stays:
    // which machines are shown, the pace, which are looked at closely. The place outlives the
    // feed, as the one it was made with does, unless another is set here before it ends.
    void setSource(WallSource& source);

    // Which machines are on the screen, and how wide each is drawn, in the monitor's own pixels.
    // Pictures are asked for these, and for no others.
    void setShown(const QHash<QString, int>& widths);
    // How often a new picture of each shown machine is asked for, where whoever looks at the wall
    // has chosen that: so many milliseconds apart. Nothing: at the pace the server names. 0: not
    // at all for now ("Paused"): the cards keep the pictures they have, and how the machines are
    // is asked as ever. A machine that has had no picture yet since the feed was started is given
    // one all the same: its card would otherwise show nothing. Ending a pause asks for pictures
    // at once.
    void setPictureEvery(std::optional<int> everyMs);
    // The pace the server names, in milliseconds; a second until it has named one.
    int serverPaceMs() const;
    // Which form of the live connection the server says it speaks; 0: none.
    int liveForm() const { return m_liveForm; }
    // The machines that are shown large and looked at alone, each in a window of the program's:
    // their pictures are asked for at least four times a second, as the page does for its
    // enlarged view, however slow the wall's own pace is, and also while the wall is paused. The
    // others', in a window beside, come at the wall's pace as ever.
    void setLookedAtClosely(const QSet<QString>& machines);
    // Something shows the wall that did not before, a window more: every shown machine is given a
    // picture now, also while the wall is paused, as if it had not had one yet.
    void showAnew();
    // The wall's order was just set from here, and this is the server's word for it: the machines
    // are in that order from now on, without the wall being asked for again. (A machine the list
    // does not name stays after those it names.)
    void takeOrder(const QStringList& machines);
    // Something was just done to a machine. What it did shows once the machine has got round to it
    // and been asked again, which on a real server's controller takes seconds: so the machines are
    // asked how they are about 1, 3, 6 and 10 seconds from now, as the page does, instead of at the
    // usual pace alone. However many machines it was done to meanwhile, that is one round of looks.
    void lookAgainSoon();
    // The place the machines come from says that its list is another now (a KVM was added on this
    // PC, or taken off): the list, and how the machines are, are asked for at once and not at the
    // feed's next turn. Where the list is being asked for this moment, it is asked for once
    // more when that answer has come: it may have been made before the change. Nothing while
    // the feed is stopped.
    void askAnew();
    // The place the machines come from says that one of them has come to something with nobody
    // having asked (a KVM of this PC's was asked what it is, signed in to and looked at, or was
    // not found: LocalWall::cameToSomething): how the machines are is asked at once, and not at
    // the feed's next turn, so that a tile says so within a moment. The list is not asked for:
    // it is the one it was. Where how the machines are is being asked this moment, it is asked
    // once more when that answer has come: the one on its way may have been made before, and
    // however often this is said meanwhile, that is one asking more. Nothing while the feed is
    // stopped.
    void lookNow();

    const QList<Machine>& machines() const { return m_machines; }
    const QHash<QString, Status>& status() const { return m_status; }
    // The last picture of this machine that came is a still kept between sessions (a PC with Intel
    // AMT whose tile is only shown: Snapshot::still); and how old it is now, in seconds: its age as
    // it came, and the time since. Nothing: no still, or one that said no age.
    bool isStill(const QString& machine) const { return m_stills.contains(machine); }
    std::optional<int> stillAgeOf(const QString& machine) const;
    // Whether the server answered the last time it was asked how the machines are.
    bool isInContact() const { return m_inContact; }

signals:
    // The machines of the wall, or how they are, are not what they were.
    void wallChanged();
    void statusChanged();
    // Each completed fresh status request, including a transport failure; never a cached picture.
    void statusObserved(bool inContact);
    // A picture of a machine came, or could not be had and why (the server's code and sentence).
    void pictureCame(const QString& machine, const QByteArray& picture, bool signal);
    void pictureFailed(const QString& machine, const QString& code, const QString& message);
    // Contact with the server was lost, or is there again.
    void contactChanged();
    // The server says nobody is signed in any more: whoever owns the feed asks for a sign-in.
    void signedOut();
    // The server showed another certificate than the one it is known by, and was told nothing:
    // whoever owns the feed asks about the certificate.
    void certificateRefused();
    // Contact was lost, and what answers at the server's address now is a QKVM that speaks
    // another version of its API (ServerClient::otherVersion): it was told nothing, and asking
    // it again changes nothing. The feed has stopped; `sentence` says which versions it
    // speaks, for whoever owns the feed to say where a server is named.
    void otherVersion(const QString& sentence);

private:
    void askWall();
    void askStatus();
    // Lets the machines whose turn has come wait for a question that is free; and sends as many
    // of those that wait as there are free.
    void askPictures();
    void sendPictures();
    // The answer to a picture that was asked for at `sent`, by the feed's clock.
    void pictureAnswered(const QString& machine, qint64 sent, const base::Result<Snapshot>& answer);
    // How often a look is taken at whose turn it is: at the pace of the machine that is asked
    // most often, and every second where that is slower, for a stream that is being woken.
    int tickMs() const;
    // How often this machine's picture is asked for just now.
    int paceMs(const QString& machine) const;
    // Everybody's turn comes now: the pace is another, or somebody has begun to look closely.
    void turnsBeginAnew();
    void lookAgain();
    void noteContact(bool inContact);
    bool isGone(const base::Error& error);
    template <typename Answer, typename Then>
    std::function<void(base::Result<Answer>)> whileAsking(Then then);

    // Never null: the place that is asked, until another is set.
    WallSource* m_source;
    bool m_running = false;
    // Raised by start(): an answer to a question that was asked before the last start is not this run's.
    quint64 m_run = 0;
    bool m_inContact = true;
    bool m_wallAsked = false;
    // The list was asked for anew while it was being asked for: it is asked for once more when
    // the answer that is on its way has come.
    bool m_wallAskedAnew = false;
    bool m_statusAsked = false;
    // The place said that a machine has come to something while how they are was being asked:
    // that is asked once more when the answer that is on its way has come.
    bool m_statusAskedAnew = false;
    QList<Machine> m_machines;
    QHash<QString, Status> m_status;
    QHash<QString, int> m_shown;
    QSet<QString> m_pictureAsked;
    // The machines whose turn has come and for whom no question was free, in the order it came.
    QStringList m_pictureWaiting;
    // When each machine's next turn comes, by the feed's clock; none: now.
    QHash<QString, qint64> m_pictureDue;
    QElapsedTimer m_clock;
    // The machines a picture has come of since the feed was started.
    QSet<QString> m_pictured;
    // Of those whose last picture was a still: its age as it came, and when that was by the clock.
    struct Still {
        std::optional<int> ageSeconds;
        qint64 cameAtMs = 0;
    };
    QHash<QString, Still> m_stills;
    double m_everySeconds = 1;
    int m_liveForm = 0;
    std::optional<int> m_ownEveryMs;
    QSet<QString> m_closely;
    // How often pictures are asked for just now: the wall's, and this machine's.
    double wallSeconds() const;
    double paceSeconds(const QString& machine) const;
    QTimer m_statusTimer;
    QTimer m_pictureTimer;
    // The looks that are still to come of a round begun by lookAgainSoon(); 0: none is under way.
    int m_looksLeft = 0;
    QTimer m_lookTimer;
};

} // namespace qkvm::core
