#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

namespace qkvm::core {

// The screen watches of this PC's own wall (the web's screens.py; its 0.3.0 row in FOR-THE-DESKTOP-APP): an alert where
// a KVM's computer has shown no picture for some minutes, or its screen has not changed, set per KVM (minutes, 0 to
// 1440; 0: not watched), each with the picture that set it off kept beside it, and a "back to normal" when it is over.
//
// * No signal for N minutes: by the card's state as the wall sees it ("no_signal"). Where a KVM has this watch, the
//   plain "no video signal" of the alerts is left to the watch (told after N minutes, not after two looks).
// * Unchanged for N minutes: by the pictures the wall gets anyway, as its cards and views ask for them (saw). Nothing
//   is asked of a KVM for it: where nobody has its card on a screen, nothing is compared, and nothing is said. Each
//   picture compared is shrunk to 64 by 36 squares, each the average brightness (0 to 255) of its part of the screen;
//   it counts as changed where at least two squares differ by more than ten from the first picture of the run. So the
//   noise of a capture and of JPEG does not count, and a moving pointer, a ticking clock or a line of text does.
//   "Unchanged" is not "hung": a screen nobody uses looks the same.
//
// What it costs: nothing for a KVM without the watch. With it, one picture every round (Times::roundMs, fifteen
// seconds) at the most, only where a new one came, decoded and shrunk in a thread of the pool, never on this one. The
// decoding is its owner's to hand it (shrinkWith: the program's is ui's, which has Qt's pictures; core has none):
// without one, nothing is compared, and "unchanged" is never told.
//
// What is kept (keepIn: the settings folder's "alert-pictures", the owner's alone): the last alert's picture of each
// KVM, replaced by the next alert's, deleted after Times::keepDays days (looked for at the first round, and every hour
// from then) and when the KVM leaves the wall; and which watches went off and are not over, with the last of what was
// told of each KVM, so that "back to normal" is told after a restart too, and its Details can say it. Read and written
// in a thread of its own, one thing at a time, never on this one (review 4's LOW 5): until what stands is read back,
// no round is made and nothing is written, and it is said (readBack) once it is in. As this goes, a round's comparing
// stops after the picture in hand, and its file thread is waited for Times::filesWithinMs at the most; what is left
// there then is the program's application object's to see to the end of.
//
// On the interface thread.
class ScreenWatch : public QObject {
    Q_OBJECT

public:
    static constexpr int mostMinutes = 1440;
    static constexpr int across = 64;
    static constexpr int down = 36;
    static constexpr int levels = 10;    // how far of 255 a square may move and still be the same
    static constexpr int squares = 2;    // how many squares that moved further make a picture another one
    static constexpr int linesRead = 10; // lines of a row of squares read at the most, spread over it
    static constexpr qint64 mostPixels = qint64{3840} * 2160;
    static constexpr int mostTold = 20; // of what was told, the last of each KVM is kept
    static constexpr qint64 mostPictureBytes = qint64{32} * 1024 * 1024;

    struct Times {
        int roundMs = 15'000;
        qint64 minuteMs = 60'000; // (a test makes it shorter)
        // Where no new picture came for longer than this, the run of "unchanged" starts again with the next one.
        int gapMinutes = 11;
        int keepDays = 30;
        qint64 dayMs = 86'400'000;
        qint64 sweepEveryMs = 3'600'000;
        // Comparing in one round, at the most: what is left waits for the next.
        qint64 roundMostMs = 10'000;
        std::function<qint64()> clockMs;      // steady; none: the program's
        std::function<QDateTime()> wallClock; // none: now, in UTC
        // Told in the thread a round's pictures are compared in, as they are (a test's: which thread that is).
        std::function<void()> whileComparing;
        // Told in its file thread as each thing done there begins (a test's: which thread, or one that takes long).
        std::function<void()> whileFiling;
        // What its file thread still does is waited for this long at the most as this goes.
        int filesWithinMs = 2000;
        // A JPEG as fingerprint gives it; nothing where it is none, or larger than mostPixels (shrinkWith).
        std::function<std::optional<QByteArray>(const QByteArray& jpeg)> shrink;
    };
    // A KVM to watch in a round, as the wall has it.
    struct Watched {
        QString machine;
        QString state; // its card's ("online", "no_signal", "offline", ...); empty before it is known
        int noSignal = 0;
        int unchanged = 0;
    };
    // A watch that went off, or is over (`back`).
    struct Event {
        QString machine;
        QString watch; // "no_signal" | "unchanged"
        bool back = false;
        int minutes = 0;     // what the watch is set to
        qint64 lastedMs = 0; // how long the screen was dark or the same, as far as the wall saw
        QDateTime at;        // when it was told
        QString stamp;       // the picture kept with it, by its number; empty: none
        QDateTime pictureAt; // when the wall was handed that picture
        bool operator==(const Event&) const = default;
    };

    explicit ScreenWatch(QObject* parent = nullptr);
    explicit ScreenWatch(Times times, QObject* parent = nullptr);
    ~ScreenWatch() override;
    ScreenWatch(const ScreenWatch&) = delete;
    ScreenWatch& operator=(const ScreenWatch&) = delete;
    ScreenWatch(ScreenWatch&&) = delete;
    ScreenWatch& operator=(ScreenWatch&&) = delete;

    // Where its pictures, and what stands, are kept; what stands there is read from now, in its thread (readBack once
    // it is in). Empty: nothing kept.
    void keepIn(const QString& folder);
    // Whether what stands in its folder is still being read.
    bool isReading() const { return m_reading; }
    QString folder() const { return m_folder; }
    // How a JPEG is decoded and shrunk (Times::shrink), from now on. Called in a thread of the pool.
    void shrinkWith(std::function<std::optional<QByteArray>(const QByteArray& jpeg)> shrink);
    // Who is watched, asked at the start of each round; and the rounds begin.
    void watch(std::function<QList<Watched>()> watched);
    // A picture of a KVM the wall has just handed out: kept (a reference) where the KVM is watched, for the next
    // round, the smallest of those with a signal since the last.
    void saw(const QString& machine, const QByteArray& picture, bool signal);
    // A KVM left the wall: what is known of it goes, and its picture.
    void forget(const QString& machine);
    // The watches of a KVM that went off and are not over.
    QStringList standing(const QString& machine) const;
    // What was told of a KVM, newest first (the last mostTold).
    QList<Event> told(const QString& machine) const;
    // The picture of a KVM kept under that stamp, read in its thread and handed back on this one; nothing where it
    // is not kept (any more). Not at all where this is gone by then.
    void askPicture(const QString& machine, const QString& stamp, std::function<void(std::optional<QByteArray>)> done);
    // A round now, beside the timer's (a test's).
    void roundNow() { round(); }
    // Whether a round's comparing is under way (a test waits for it).
    bool isComparing() const { return m_comparing; }

    // How long a watch waits, from what a card keeps: 0 (off) for what is no whole number above 0, mostMinutes at
    // the most (the web's `minutes`).
    static int minutesFrom(qint64 value);
    // A picture as across by down squares, each the average brightness of its part of the screen (R, G and B alike).
    // Of a screen's pixels, three bytes each (red, green, blue), `bytesPerLine` apart from row to row.
    static QByteArray fingerprint(const uchar* rgb, int width, int height, qsizetype bytesPerLine);
    // Whether two pictures (as fingerprint gives them) are of screens that look different.
    static bool differs(const QByteArray& one, const QByteArray& other);

signals:
    // A watch went off, or is over. (`told` has it from then on.)
    void alerted(const qkvm::core::ScreenWatch::Event& event);
    // What stands in its folder was read back (keepIn): `standing` and `told` have it from now on.
    void readBack();

private:
    struct Seen {
        QByteArray picture;
        QDateTime at; // when the wall handed it out
        qint64 pixels = 0;
        quint64 number = 0; // which of its KVM's pictures it is
    };
    struct Machine {
        std::optional<Seen> pending; // to compare at the next round
        bool darkSeen = false;       // a picture without a signal came since the last round
        std::optional<Seen> lastLit; // the last picture with a signal that was compared (or seen)
        quint64 lastCompared = 0;    // the number of the last picture compared
        quint64 pictures = 0;        // its pictures counted
        QByteArray reference;        // the first picture of the run of "unchanged", shrunk
        std::optional<qint64> sameSince;
        std::optional<qint64> lastNew;
        std::optional<qint64> darkSince;
        QSet<QString> told;
        QList<Event> said; // newest first
    };

    void round();
    void finishRound(const QList<Watched>& machines, const QHash<QString, std::optional<QByteArray>>& compared,
                     const QHash<QString, Seen>& todo, qint64 now);
    QList<Event> dark(const Watched& each, Machine& machine, qint64 now);
    QList<Event> same(const Watched& each, Machine& machine, const std::optional<Seen>& seen,
                      const std::optional<QByteArray>& found, qint64 now);
    // The events of a round told: their pictures kept first (in its thread), then said, and what stands written.
    void tell(QList<Event> events, QHash<QString, Seen> pictures);
    void toldNow(const QList<Event>& events);
    void write();
    // What stands in `folder`, read in its thread; and taken in on this one.
    static QHash<QString, Machine> stateIn(const QString& folder);
    void takeIn(const QString& folder, QHash<QString, Machine> machines);
    // `task` in its file thread, after Times::whileFiling.
    void filing(std::function<void()> task);
    qint64 now() const;
    QDateTime wallNow() const;

    Times m_times;
    QString m_folder;
    std::function<QList<Watched>()> m_who;
    QHash<QString, Machine> m_machines;
    QSet<QString> m_watched;
    QSet<QString> m_going; // KVMs that left the wall: their pictures go at the next round
    QTimer m_rounds;
    bool m_comparing = false;
    std::optional<qint64> m_sweptAt; // none: not yet in this run (the first round sweeps)
    qint64 m_stamped = 0;
    // While what stands is read: what is to be written once it is in, and the KVMs that left the wall meanwhile.
    bool m_reading = false;
    bool m_writeOnceRead = false;
    QSet<QString> m_forgottenWhileReading;
    // Its files, one thing at a time (its own: handed to the program's application object where it is not done as
    // this goes); and the comparing, in the program's pool, told to stop as this goes.
    std::unique_ptr<QThreadPool> m_files = std::make_unique<QThreadPool>();
    std::shared_ptr<std::atomic<bool>> m_stop = std::make_shared<std::atomic<bool>>(false);
};

} // namespace qkvm::core

Q_DECLARE_METATYPE(qkvm::core::ScreenWatch::Event)
