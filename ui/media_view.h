#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/media_place.h>
#include <qkvm/core/read_ahead.h>

#include <qkvm/base/result.h>

namespace qkvm::ui {

// The box for a machine's virtual media (0.3.0), as the page's Actions > Media... has it (app.js, mediaDialog): through
// a QKVM server, or of a kvmd KVM of this PC's own wall, which the program speaks to itself (core::MediaPlace says
// which answers, and the box cannot tell: GAPS A2). The images, what is in the drive and who put it there, and mount,
// unmount, upload and delete, each asked first with a question that names the machine and the image, and sent with
// both: the place does nothing where the machine is called otherwise by then (409 changed), and that is said, and
// nothing more is sent. Through a server 0.3.2 each is sent with the mark of the list it was picked from as well: where
// the tile's machine is another by then (its tile edited to another KVM), the server does nothing (409 moved), and the
// box closes and says so in its words; an upload asks for the list again first, and sends nothing where its mark is
// another by then (web 0.3.2's page). And the box is held to the mark of the first list it was opened with, as this
// PC's own wall's box is to its first listing: a later look that lists the machine by another mark closes it, unshown
// (stricter than the page). Through a server it is an admin's, signed in in person (whoever opens it sees to
// that: MachineActions). The box holds still: one size and place whatever it shows (its screen's), and while something
// of it is out nothing in it takes a click.
//
// What the box does is told in it, as the page tells it, and what was done in the window's corner as well (`told`).
// One thing at a time for each machine, with whatever else is done to it from any window (`underWay`, by the machine's
// key for that). An upload is carried from the file as it is read, a piece at a time, through the server or straight
// to the KVM (MediaPlace::uploadMedia), with how far it has come as the place counts it, asked every second; closing
// the box stops it. The file is held to stay as it was while it is read (core::ReadAhead::unchangedFileAt). A mount,
// an unmount or a deletion goes on when the box is closed, and is told in the corner.
//
// On the interface thread. The place may go first: the box is then of nothing.
class MediaView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("A window's actions have its media box.")
    Q_PROPERTY(bool opened READ opened NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString where READ where NOTIFY changed)
    Q_PROPERTY(QString now READ now NOTIFY changed)
    // The images, each {id, name, hint, picked}; and what is said where there are none.
    Q_PROPERTY(QVariantList images READ images NOTIFY changed)
    Q_PROPERTY(QString nothing READ nothing NOTIFY changed)
    // What an image goes in as, and how the machine is to start from it: each {value, label, enabled}.
    Q_PROPERTY(QVariantList drives READ drives NOTIFY changed)
    Q_PROPERTY(QString drive READ drive NOTIFY changed)
    Q_PROPERTY(QVariantList boots READ boots NOTIFY changed)
    Q_PROPERTY(QString boot READ boot NOTIFY changed)
    Q_PROPERTY(bool restart READ restart NOTIFY changed)
    Q_PROPERTY(bool restartEnabled READ restartEnabled NOTIFY changed)
    Q_PROPERTY(bool canMount READ canMount NOTIFY changed)
    Q_PROPERTY(bool canDelete READ canDelete NOTIFY changed)
    Q_PROPERTY(bool canUpload READ canUpload NOTIFY changed)
    Q_PROPERTY(bool canUnmount READ canUnmount NOTIFY changed)
    // The one line of what is said, whether that is something amiss, and, while something is out, how long it has been.
    Q_PROPERTY(QString said READ said NOTIFY changed)
    Q_PROPERTY(bool saidBad READ saidBad NOTIFY changed)
    Q_PROPERTY(bool working READ working NOTIFY changed)
    Q_PROPERTY(QString waited READ waited NOTIFY changed)
    Q_PROPERTY(QString foot READ foot NOTIFY changed)
    // Something of the box's is out: it holds still.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // The question before something is done: {title, text, yes, danger}; empty: none is asked.
    Q_PROPERTY(QVariantMap question READ question NOTIFY changed)

public:
    // What is under way on which machine from any window, by its key for that (MachineActions' UnderWay).
    using UnderWay = QHash<QString, QString>;

    explicit MediaView(QObject* parent = nullptr);
    ~MediaView() override;
    MediaView(const MediaView&) = delete;
    MediaView& operator=(const MediaView&) = delete;
    MediaView(MediaView&&) = delete;
    MediaView& operator=(MediaView&&) = delete;

    // Opened for this machine of this place (a server's client, or this PC's own wall, each a QObject): `machine` is
    // its id there, `name` what it is called now. `key`: its key on the wall, told with `done`; `underWayKey`: its key
    // in `underWay`, which outlives the box, or is let go of first (letGo). The images are asked for at once.
    void open(core::MediaPlace* place, const QString& machine, const QString& name, const QString& key,
              UnderWay* underWay, const QString& underWayKey);
    // The machine is called this now: the box says so, and its next question names it so.
    void rename(const QString& name);
    // Whoever may have the box may no longer, or the place is gone: it closes, an upload is stopped, and nothing that
    // is out is anybody's any more (what it held in `underWay` is given back now).
    void letGo();
    // The machine is said to be at another address than when the box was opened for it: the box closes (as close()),
    // and that is said in the window's corner. It is opened again from the machine's menu.
    void movedElsewhere();
    // How an image is chosen on this PC: Windows' own box in the program (win/file_dialogs.h), a test's own in a
    // test. The file's whole name; nothing where whoever was asked said no; why not, where they could not be asked.
    void setImageDoor(std::function<base::Result<QString>()> door) { m_whichImage = std::move(door); }
    // How the image chosen there is read: the file at its path, held to stay as it was
    // (core::ReadAhead::unchangedFileAt) in the program, a test's own in a test.
    void setImageSource(std::function<core::ReadAhead::Source(const QString& path)> source)
    {
        m_source = std::move(source);
    }
    // The key on the wall of the machine the box is open for; empty: it is closed.
    QString key() const { return m_opened ? m_key : QString(); }
    // The place it is open for; none: it is closed, or the place went.
    core::MediaPlace* place() const { return m_alive != nullptr ? m_place : nullptr; }

    bool opened() const { return m_opened; }
    QString title() const;
    QString where() const { return m_where; }
    QString now() const { return m_now; }
    QVariantList images() const;
    QString nothing() const { return m_nothing; }
    QVariantList drives() const;
    QString drive() const { return m_drive; }
    QVariantList boots() const;
    QString boot() const { return m_boot; }
    bool restart() const { return m_restart; }
    bool restartEnabled() const;
    bool canMount() const;
    bool canDelete() const;
    bool canUpload() const;
    bool canUnmount() const;
    QString said() const { return m_said; }
    bool saidBad() const { return m_saidBad; }
    bool working() const { return m_carrying != 0 && m_carryTurn == m_turn; }
    QString waited() const { return m_waited; }
    QString foot() const { return m_foot; }
    bool busy() const { return working(); }
    QVariantMap question() const;

    // What the screen asks. Closed: an upload under way is stopped (what came of it is taken off by the server).
    Q_INVOKABLE void close();
    Q_INVOKABLE void pick(const QString& id);
    Q_INVOKABLE void chooseDrive(const QString& value);
    Q_INVOKABLE void chooseBoot(const QString& value);
    Q_INVOKABLE void chooseRestart(bool restart);
    Q_INVOKABLE void mount();
    Q_INVOKABLE void unmount();
    Q_INVOKABLE void remove();
    Q_INVOKABLE void upload();
    Q_INVOKABLE void answer(bool yes);

    // A count of bytes as the page writes one: "614 kB", "2.4 GB".
    static QString bytesText(qint64 bytes);

signals:
    void changed();
    // Something done from the box is over: the machine of this key on the wall may show it.
    void done(const QString& key);
    // Told in the window's corner: what was done, or what is to be looked at.
    void told(const QString& said, bool good);

private:
    struct Asked {
        enum class What { Mount, Unmount, Delete, Upload } what = What::Mount;
        QString title;
        QString text;
        QString yes;
        bool danger = true;
        QString named; // the machine as the question named it
        // The mark of the list shown when it was asked (MediaList::listed: the page's `listed`, taken when the image is
        // picked and asked for), sent with it; a mount's is its own.
        QString listed;
        core::MediaMount mount;
        QString image;
    };

    bool isCurrent(quint64 turn) const;
    // The box is open, and its place answers as somebody else now (signed in again, as another account or to another
    // server through the same client): what it shows and what was asked in it are the other sign-in's, and no look of
    // it is answered any more. It closes, says why in the corner, and says so (true).
    bool signedInOtherwise();
    // Asks what can be mounted and what is; what came of that.
    void load(bool retried = false);
    void listed(const base::Result<core::MediaList>& answer, bool retried);
    void show(const core::MediaList& list);
    void fit();
    void tell(const QString& text, bool bad = false);
    const core::MediaImage* picked() const;
    // What a list says stands against an upload, said here and nothing sent; nothing where nothing does. Of the list
    // shown when it is asked, and of the fresh one of the look before it goes.
    QString refusedAtOnce(const core::MediaList& list, const QString& name, qint64 size) const;
    // Sends what was asked, holding the box still meanwhile: `send` hands what came back (a sentence to say, or why
    // not) to the function it is given. `inTheCorner`: that sentence is told in the window's corner too, after "...
    // done" (the server's note of a mount, say); else only in the box (an upload's SHA-256), as the page tells them.
    using Answer = std::function<void(base::Result<QString>)>;
    void carry(const QString& what, const QString& note, const std::function<void(Answer)>& send,
               bool inTheCorner = true);
    // The upload asked of the file chosen, once it is open; and sent with its yes.
    void askUpload(const QString& name, std::unique_ptr<core::ReadAhead> file);
    void sendUpload(const Asked& asked, std::unique_ptr<core::ReadAhead> file, const Answer& answered);
    // The upload itself, once the look before it (where there is one) found the machine the box listed.
    void carryUpload(const Asked& asked, std::unique_ptr<core::ReadAhead> file, const Answer& answered);
    // How the upload of the thing out from the box as `carrying` is stopped, held until it is over; and let go of then,
    // where that is still the one held (a late answer of one stopped before is nobody's).
    void holdStop(quint64 carrying, std::function<void()> stop);
    void letGoOfStop(quint64 carrying);
    void countUp();

    // The place, and the QObject it is (gone with it).
    core::MediaPlace* m_place = nullptr;
    QPointer<QObject> m_alive;
    QByteArray m_identity;
    QString m_machine;
    QString m_name;
    QString m_key;
    UnderWay* m_underWay = nullptr;
    QString m_underWayKey;
    bool m_opened = false;
    quint64 m_turn = 0;
    quint64 m_look = 0;
    QTimer m_again;
    std::optional<core::MediaList> m_shown;
    // What the place said the machine is bound to when the box first listed it (MediaList::boundTo); and whether it is
    // bound to another since: then the box changes nothing more, and is to be opened again.
    std::optional<QString> m_bound;
    bool m_moved = false;
    // The mark of that first list (MediaList::listed, a server 0.3.2's): a later list with another, or with none, is of
    // another machine than the one the box was opened for, and closes it.
    QString m_mark;
    QString m_picked;
    QString m_drive = QStringLiteral("cd");
    QString m_boot = QStringLiteral("none");
    bool m_restart = false;
    QString m_where;
    QString m_now;
    QString m_nothing;
    QString m_foot;
    QString m_said;
    bool m_saidBad = false;
    QString m_waited;
    std::optional<Asked> m_asked;
    // The one thing out from the box, by its number (0: none), the opening it was sent from, and what it holds in
    // `underWay`, by whose key.
    quint64 m_carrying = 0;
    quint64 m_carryTurn = 0;
    quint64 m_carries = 0;
    QString m_carryWhat;
    // What the last thing done from the box said, to be kept where the look after it says nothing.
    std::optional<std::pair<QString, bool>> m_keep;
    UnderWay* m_carryUnderWay = nullptr;
    QString m_carryKey;
    QElapsedTimer m_since;
    QTimer m_counting;
    // The upload under way: how it is stopped (that of the thing out as m_stopOf), and the look at how far it has come.
    std::function<void()> m_stopUpload;
    quint64 m_stopOf = 0;
    QTimer m_progress;
    QString m_uploading;
    std::function<base::Result<QString>()> m_whichImage;
    std::function<core::ReadAhead::Source(const QString& path)> m_source;
    // The file of the upload that is asked, from its being open until its answer; the opening under way (by its
    // number: another choice, or the box closed, makes it nobody's), what it is called, and when that is said.
    std::unique_ptr<core::ReadAhead> m_file;
    quint64 m_openings = 0;
    QString m_openingName;
    QTimer m_opening;
};

} // namespace qkvm::ui
