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
    WallMeasure(QQuickWindow& window, ui::WallView& wall, QString file, int settleSeconds, int seconds,
                std::function<void()> done, std::vector<Moment> start = {}, QObject* parent = nullptr);
    ~WallMeasure() override;

    WallMeasure(const WallMeasure&) = delete;
    WallMeasure& operator=(const WallMeasure&) = delete;
    WallMeasure(WallMeasure&&) = delete;
    WallMeasure& operator=(WallMeasure&&) = delete;

private:
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
    qint64 m_cardsMs = -1;
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
