#include "display_awake.h"

#include <algorithm>
#include <utility>

#include <QWindow>

// (windows.h before powerbase.h, which needs its types.)
#include <windows.h>

#include <powerbase.h>

namespace qkvm::ui {

void keepDisplayAwake(bool awake)
{
    // ES_CONTINUOUS alone takes back what was asked before.
    SetThreadExecutionState(awake ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

bool displayIsKeptAwake()
{
    ULONG state = 0;
    return CallNtPowerInformation(SystemExecutionState, nullptr, 0, &state, sizeof state) == 0 &&
           (state & ES_DISPLAY_REQUIRED) != 0;
}

AwakeInFullScreen::AwakeInFullScreen(std::function<void(bool)> keeper, QObject* parent)
    : QObject(parent), m_keeper(std::move(keeper))
{}

AwakeInFullScreen::~AwakeInFullScreen()
{
    if (m_kept) {
        m_keeper(false);
    }
}

void AwakeInFullScreen::watch(QWindow& window)
{
    m_windows.append(&window);
    connect(&window, &QWindow::visibilityChanged, this, &AwakeInFullScreen::look);
    look();
}

void AwakeInFullScreen::forget(QWindow& window)
{
    // (What it still says until it ends is a look at the others, and changes nothing.)
    m_windows.removeAll(&window);
    look();
}

void AwakeInFullScreen::look()
{
    const bool wanted = std::ranges::any_of(
        m_windows, [](const QWindow* window) { return window->visibility() == QWindow::FullScreen; });
    if (wanted != m_kept) {
        m_kept = wanted;
        m_keeper(wanted);
    }
}

} // namespace qkvm::ui
