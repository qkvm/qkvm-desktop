#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QObject>
#include <QQuickWindow>
#include <QString>

#include "meter.h"
#include "wall_view.h"

namespace qkvm::bench {

// What the program's window costs while it shows its wall, measured from inside it and written to
// a file: for holding a large wall against the targets of SPEC.md section 9.
//
// From the start of the program: when the window drew its first frame, and when the wall had its
// cards. Then, after the wall was left to settle: how much of the PC's processors the program
// takes and how much memory; how many frames the window draws, how long each waits for the
// monitor before its work begins (Windows' pacing), how long the work takes (from the moment the
// interface thread is held for it, or for a frame the drawing thread asks for itself without it,
// a new picture's, from its rendering, until the frame is ready to be shown), for how much of that the
// interface thread is held, and how long the finished frame takes to hand over.
//
// And how long the interface thread takes to answer, which is what somebody at the PC feels as a
// hitch: a second thread asks it every few milliseconds, and where the answer is late, looks at
// what the interface thread is doing at that moment and takes it down by name. (That thread gets
// ready while the wall settles: finding the names is slow the first time, and the interface
// thread is held still for each look.)
//
// On the interface thread, but for the moments of each frame, which Qt Quick says on the thread
// that draws, and for the second thread.
class WallMeasure : public QObject {
    Q_OBJECT

public:
    // A moment of the program's start, by name: how many milliseconds after Windows started the
    // program, and how much memory the program had committed by then, in MB.
    struct Moment {
        QString what;
        qint64 ms = 0;
        double committedMb = 0;
    };
    // This moment, under a name.
    static Moment now(const QString& what);
    // Milliseconds since Windows started this program.
    static qint64 sinceTheStart();

    // After the wall has its machines, `settleSeconds` pass, then `seconds` are measured, the
    // report is written to `file`, and `done` is called. `start`: the moments before there was a
    // window, which whoever made the window took down; they are written with the rest.
    // `machines`: how many the wall is to have (the KVMs the rig adds, one after the other): the wall settles from
    // when it has them all (a moment of its own, with the memory committed then), and not from its first, which on a
    // loaded PC came 30 s and more before its last; where they have not all come within three times the settling, it
    // is measured with those it has, which the report says. 0: from the first.
    WallMeasure(QQuickWindow& window, ui::WallView& wall, QString file, int settleSeconds, int seconds,
                std::function<void()> done, std::vector<Moment> start = {}, int machines = 0,
                QObject* parent = nullptr);
    ~WallMeasure() override;

    WallMeasure(const WallMeasure&) = delete;
    WallMeasure& operator=(const WallMeasure&) = delete;
    WallMeasure(WallMeasure&&) = delete;
    WallMeasure& operator=(WallMeasure&&) = delete;

    // Called as the measuring begins, after the wall settled (the rig: a window dragged meanwhile).
    void whenItBegins(std::function<void()> then) { m_atBegin = std::move(then); }
    // More lines of the report, of whoever made the window: asked as the measuring begins (`atTheEnd` false, for
    // what counts since then; what it says then is not written) and at its end. (The rig: the card stills' line.)
    void addToReport(std::function<QString(bool atTheEnd)> lines) { m_more = std::move(lines); }

    // What the program's committed memory is made of just now, as lines of the report (PERF-2): its heaps walked
    // (what is committed for them, what is in use there, and that by the size of each piece), its threads and their
    // stacks, and the rest of what is its own (what the graphics driver and the libraries take for themselves
    // outside a heap). The heaps are held still while they are walked, which takes moments: never while measuring.
    static QString memoryParts();

private:
    // The wall has its machines (all that it is to have, or the first: `machines`), or has waited long enough.
    void cardsCame();
    void begin();
    void end();
    // The second thread: asks, waits for the answer, and looks where it is late.
    void watch();
    // What the interface thread is calling at this moment, innermost first.
    std::string whereTheInterfaceThreadIs() const;

    QQuickWindow& m_window;
    ui::WallView& m_wall;
    QString m_file;
    int m_seconds;
    std::function<void()> m_done;
    std::vector<Moment> m_start;
    Meter m_meter;
    qint64 m_firstFrameMs = -1;
    qint64 m_firstCardMs = -1;
    qint64 m_cardsMs = -1;
    int m_machines = 0;
    int m_settleSeconds = 0;
    double m_cardsCommittedMb = 0;
    double m_beganCommittedMb = 0;
    double m_compactedMb = -1; // (QKVM_RIG_COMPACT: as the heaps were compacted as the measuring began)
    // What the memory was made of as the window was shown, and when the wall had its cards (memoryParts).
    QString m_partsAtFirst;
    QString m_partsWithCards;
    std::function<void()> m_atBegin;
    std::function<QString(bool)> m_more;
    bool m_inFrontAtFirst = false;

    // Taken down by the thread that draws, read at the end by the interface thread.
    std::mutex m_guard;
    bool m_measuring = false;
    std::vector<double> m_waitedMs;   // for the monitor, before a frame's work
    std::vector<double> m_workMs;     // a frame's work
    std::vector<double> m_heldMs;     // the part of it with the interface thread held
    std::vector<double> m_handOverMs; // the finished frame handed over
    qint64 m_frameAskedUs = 0;
    qint64 m_frameBeganUs = 0;
    qint64 m_frameReadyUs = 0;

    // The second thread's.
    std::thread m_watcher;
    std::atomic<bool> m_watching{false};
    std::atomic<qint64> m_answeredUs{0};
    void* m_interfaceThread = nullptr;
    std::vector<double> m_answerMs;
    // The late answers: how late, at which second of the measuring, and what the interface thread
    // was doing meanwhile.
    struct Late {
        double tookMs = 0;
        double atSecond = 0;
        std::string doing;
    };
    std::vector<Late> m_caught;
    qint64 m_beganUs = 0;
};

} // namespace qkvm::bench
