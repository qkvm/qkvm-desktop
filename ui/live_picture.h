#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <qkvm/core/live.h>
#include <qkvm/core/wall_source.h>
#include <qkvm/engine/scene.h>

#include "wall_machines.h"

namespace qkvm::ui {

// One window's live picture: the machine that is shown large in the window, had live from the
// place its key on the wall reaches (core::WallSource: a QKVM server, SPEC.md section 6.2, or
// this PC's own KVMs, section 6.3) for as long as it is shown large there and may be had live.
//
// It asks that place for the machine live (a server for a pass and then the line; a KVM of this
// PC's own for its pictures). The pictures that come go into a stream of their own in the
// window's engine. Once the engine has made a picture of them, that stream is what is to be
// drawn (`stream()`), and the machine's snapshots are not needed (`wantsSnapshots()`). Until
// then they are what there is.
//
// What is drawn never goes back in time. When the line is gone, its last picture stays up until
// a snapshot has come that is newer, or another line has a picture: the snapshot from before the
// machine was had live, which may be an hour old, is not shown again.
//
// A place that says in its own words that it has no live picture of the machine is not asked
// again while the machine stays shown; nor is one that this copy of the program does not speak
// to at all (tls.h, "desktop:tls_damaged"). One that could not be reached, or refused without such a
// word, and a line that ended, are tried again after a while, and after a longer while each time
// in a row, until a line has held for long. A machine is asked for only once it has stayed shown
// for a moment: whoever steps through the machines with a key held down asks for none of those
// passed.
//
// The place a machine is had live from is what its key reaches when it comes to be shown, and it
// stays that place for as long as the machine stays shown: its line is never one place's and the
// next another's. A key that reaches nothing then is asked of nobody while it stays shown.
//
// On the interface thread. The places its keys reach outlive it.
class LivePicture : public QObject {
    Q_OBJECT

public:
    // What it needs of its surroundings. `scene`: the window's engine as it is just now, or
    // nothing where the window has none (asked each time: an engine may end before this does).
    // `number`: a number that no other stream of the program has. `place`: which place, and which
    // machine of it, a key on the wall reaches (WallMachines::reach).
    using OpenLive = std::function<void(const Reached&, core::Live::Need,
                                        std::function<void(base::Result<std::unique_ptr<core::Live>>)>)>;
    struct Around {
        std::function<engine::Scene*()> scene;
        std::function<quint32()> number;
        std::function<Reached(const QString& key)> place;
        OpenLive open;
    };
    // How long a machine has to stay shown before it is asked for. How long it waits before it
    // asks again, the first time, and never longer than `longestMs`; a line that held for that
    // long begins the waiting anew. How often the engine is asked whether a stream's first
    // picture is made. And how long a snapshot is given to be drawn before the last live picture
    // makes way for it. Statistics are sampled at their own slower pace. A test waits less.
    struct Pace {
        int beginAfterMs = 150;
        int againAfterMs = 2000;
        int longestMs = 30'000;
        int lookEveryMs = 15;
        int makeWayAfterMs = 80;
        int statisticsEveryMs = 1000;
    };

    LivePicture(Around around, Pace pace, QObject* parent = nullptr);
    ~LivePicture() override;
    LivePicture(const LivePicture&) = delete;
    LivePicture& operator=(const LivePicture&) = delete;
    LivePicture(LivePicture&&) = delete;
    LivePicture& operator=(LivePicture&&) = delete;

    // Which machine is shown large now, by its key on the wall, how wide its picture is drawn, in
    // the monitor's own pixels, and whether it may be had live just now (somebody can see the
    // window, the place its machines come from answers and has live pictures); no machine: none
    // is shown. Said whenever anything of it may have changed. While a machine may not be had
    // live its line is ended, and its last picture stays.
    void show(const QString& machine, int pixels, bool live);

    // The number of the engine's stream that is to be drawn for the machine; 0: its card's own,
    // which its snapshots go into.
    quint32 stream() const { return m_fedIsDrawn ? m_fed.number : m_held.number; }
    // No live picture comes just now: the machine's snapshots are asked for.
    bool wantsSnapshots() const { return !m_fedIsDrawn; }
    // A snapshot of the machine has come and gone to its card's stream: a last live picture that
    // was still up makes way for it.
    void snapshotCame();
    // The machine may be had live again (M6 2h: "Show here" took a JetKVM's picture back): where it is the
    // machine shown, and its line was stopped by a no (refused so, or ended for good), it is asked for at
    // once. Of any other machine, or one whose line was not stopped so, nothing.
    void openAgain(const QString& machine);
    // The machine as it is had live just now, for whoever sends keyboard and mouse to it; nothing
    // while it is not had live. This one's own: it ends when this says so.
    core::Live* live() const { return m_link.get(); }
    // Measurements of the current live connection and decoder, sampled once a second. Receive
    // age uses this PC's steady clock, never the source clock. Rate is compressed picture
    // payload handed to this window, not wire bitrate or the device's capture/display rate.
    // Empty when no machine is enlarged; unknown values stay unknown until observed.
    QVariantMap diagnostics() const { return m_diagnostics; }

signals:
    // `stream()` or `wantsSnapshots()` is not what it was, and not because of what `show` was
    // just told.
    void changed();
    // `live()` is another one, or none. Where one goes, that is said while it still stands, and
    // before it is ended: whoever sent keys through it lets go of them there first.
    void liveChanged();
    // The line the machine was had live by ended by itself, and this is why, in the words of
    // whoever gave it. Said just before `liveChanged` says that there is none: whoever had asked
    // that live for something hears why no answer comes any more.
    void lineEndedBecause(const QString& message);
    void diagnosticsChanged();

private:
    // A stream in the window's engine: the scene it is in (where the window has one), the
    // stream itself, and the number it is drawn by; number 0: none.
    struct Kept {
        engine::Scene* scene = nullptr;
        engine::Stream* stream = nullptr;
        quint32 number = 0;
    };

    core::Live::Need need() const;
    // The engine's own stream of one that is kept; nothing where the engine has ended.
    engine::Stream* streamOf(const Kept& kept) const;
    void tellTheSize();
    void ask();
    void hold(std::unique_ptr<core::Live> live);
    void take(const core::Live::Picture& picture);
    // Whether the engine has made a picture of what was fed: from then on it is what is drawn.
    void look();
    // The line ended by itself, for this reason, said in this sentence; and it is ended from here.
    void lineEnded(const QString& code, const QString& message);
    void endLine();
    // A line that goes: what it let go is kept with the machine's earlier lines' (Live::picturesLetGo).
    void keepWhatItLetGo(const core::Live& line);
    // What was fed is fed no more: its last picture stays up where it had one, or it goes.
    void holdWhatWasFed(bool keep);
    void drop(Kept& kept);
    void tryAgainLater();
    void resetDiagnostics();
    // What whoever decodes wished for, passed on to where the pictures come from.
    void passOnAWish();
    void publishDiagnostics();

    Around m_around;
    Pace m_pace;
    // The machine that is shown large, and the one the line is for: the same, or none while the
    // machine may not be had live.
    QString m_machine;
    QString m_lineFor;
    // Where the machine that is shown large is had live from: what its key reached when it came to
    // be shown; nothing while none is, or where the key reached nothing.
    Reached m_at;
    int m_pixels = 0;
    // Raised whenever the line is for another machine, or for none: an answer is to a question,
    // and the answer for a machine that is not asked for any more is nobody's.
    quint64 m_turn = 0;
    // The line for the machine shown was stopped by a no, and is asked for again only when a person does
    // something (openAgain), or another machine is shown.
    bool m_stoppedOnANo = false;
    std::unique_ptr<core::Live> m_link;
    // The stream the line's pictures go into, of which kind it is, and whether the engine has
    // made a picture of them. And a stream that is fed no more, whose last picture is still up.
    Kept m_fed;
    bool m_fedIsH264 = false;
    bool m_fedIsChanges = false; // (a server's screen as what changed of it: core::Live::Picture::changes)
    bool m_fedIsDrawn = false;
    Kept m_held;
    // Since when the line's pictures are drawn; and how often the engine was asked in vain.
    QElapsedTimer m_drawnSince;
    QElapsedTimer m_decodeProgress;
    quint64 m_lastDecoded = 0;
    bool m_reportedDecodeFailure = false;
    int m_looks = 0;
    int m_waitMs;
    QTimer m_again;
    QTimer m_looking;
    QTimer m_makingWay;
    QTimer m_statistics;
    QVariantMap m_diagnostics;
    core::Live::Video m_video;
    bool m_hasReceived = false;
    bool m_receivedH264 = false;
    bool m_receivedChanges = false;
    quint64 m_receivedPictures = 0;
    quint64 m_receivedBytes = 0;
    quint64 m_ratePictures = 0;
    quint64 m_rateBytes = 0;
    double m_receivedFps = 0;
    double m_receivedMbps = 0;
    // Of which stream of the engine the count the rate is reckoned from is, by its number (which no
    // other stream has after it, where an address may be had again); 0: none.
    quint32 m_rateNumber = 0;
    quint64 m_rateShown = 0;
    // What the lines of the machine shown let go before they were handed over (Live::picturesLetGo):
    // those that went, and the most the one now said. From nought where another machine is shown.
    quint64 m_letGoEarlier = 0;
    quint64 m_letGoOfLine = 0;
    double m_shownFps = 0;
    bool m_shownRateKnown = false;
    bool m_rateKnown = false;
    QElapsedTimer m_rateSince;
    QElapsedTimer m_lastReceived;
    QElapsedTimer m_diagnosticSince;
};

} // namespace qkvm::ui
