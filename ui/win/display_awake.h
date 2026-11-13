#pragma once

#include <functional>

#include <QList>
#include <QObject>

class QWindow;

namespace qkvm::ui {

// Keeps the display from going dark and the PC from going to sleep, for as long as it is asked to;
// and lets Windows have its way again. A wall that fills a screen is looked at without anybody
// touching the PC. Always from the interface thread: Windows keeps this by the thread that asked.
void keepDisplayAwake(bool awake);

// Whether anything on this PC is holding the display awake just now: this program, or another.
bool displayIsKeptAwake();

// Has the display kept awake while one of the program's windows is in full screen, and no longer
// than that: a wall on one monitor that fills it holds the display, whatever the windows beside
// it do. `keeper` is what does the keeping: Windows' own way, unless a test hands in its own.
//
// On the interface thread. A window is forgotten before it ends.
class AwakeInFullScreen : public QObject {
    Q_OBJECT

public:
    explicit AwakeInFullScreen(std::function<void(bool)> keeper = keepDisplayAwake, QObject* parent = nullptr);
    // Lets go, where it was holding.
    ~AwakeInFullScreen() override;
    AwakeInFullScreen(const AwakeInFullScreen&) = delete;
    AwakeInFullScreen& operator=(const AwakeInFullScreen&) = delete;
    AwakeInFullScreen(AwakeInFullScreen&&) = delete;
    AwakeInFullScreen& operator=(AwakeInFullScreen&&) = delete;

    void watch(QWindow& window);
    void forget(QWindow& window);

private:
    void look();

    std::function<void(bool)> m_keeper;
    QList<QWindow*> m_windows;
    bool m_kept = false;
};

} // namespace qkvm::ui
