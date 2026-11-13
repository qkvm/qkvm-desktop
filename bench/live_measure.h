#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QObject>
#include <QQuickWindow>
#include <QString>
#include <QVariantMap>

#include "meter.h"
#include "wall_view.h"

namespace qkvm::bench {

// How smooth the machine shown large is, measured from inside the program's window and written to
// a file: for holding the live console against its targets.
//
// Once its first live picture has reached the screen and `settleSeconds` have passed, its own
// numbers (LivePicture's diagnostics) are taken each time they are told, about once a second, for
// `seconds`: how many pictures arrived and how many reached the screen, how evenly (the steps
// between them as they arrived and on the screen, and how much longer or shorter each took to the
// screen than the one before), and how long from arriving to the screen. Beside them the frames
// the window drew and the steps between those, and what the program cost the PC meanwhile (the
// processor, the card's decoder and drawing). Then the report is written to `file` and `done` is
// called; also where the numbers stop coming (the machine is no longer shown large), at the latest
// half a minute after the measuring was to end.
class LiveMeasure : public QObject {
    Q_OBJECT

public:
    LiveMeasure(QQuickWindow& window, ui::WallView& wall, QString file, int settleSeconds, int seconds,
                std::function<void()> done, QObject* parent = nullptr);

private:
    void begin();
    void sample();
    void end();

    QQuickWindow& m_window;
    ui::WallView& m_wall;
    QString m_file;
    int m_settleSeconds;
    int m_seconds;
    std::function<void()> m_done;
    bool m_settling = false;
    bool m_settled = false;
    bool m_begun = false;
    bool m_ended = false;
    QElapsedTimer m_since;
    QVariantMap m_first;
    QVariantMap m_last;
    qint64 m_lastAtMs = 0;
    quint32 m_stream = 0;
    bool m_streamChanged = false;
    QJsonArray m_samples;
    Meter m_meter;

    // When the window's frames were shown, taken down by the thread that draws while measuring. Its
    // own, and held by what Qt calls on that thread as well: such a call may still be under way
    // while this is destroyed.
    struct Frames {
        std::mutex guard;
        bool measuring = false;
        std::vector<std::int64_t> us;
    };
    std::shared_ptr<Frames> m_frames = std::make_shared<Frames>();
};

} // namespace qkvm::bench
