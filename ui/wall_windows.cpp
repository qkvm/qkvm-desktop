#include "wall_windows.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>

#include <qkvm/core/setup_overrides.h>

#include "control.h"
#include "media_view.h"
#include "notices.h"
#include "video_host.h"
#include "wall_choices.h"
#include "window_keeper.h"

namespace qkvm::ui {

void WallWindows::openIncident(const QString& scope, const QString& machine)
{
    if (count() == 0)
        return;
    qsizetype chosen = 0;
    for (qsizetype i = 0; i < count(); ++i) {
        if (window(i)->isActive())
            chosen = i;
    }
    // Prefer an existing window of this source, without changing a different source's window.
    if (connection(chosen)->selectedScope() != scope) {
        for (qsizetype i = 0; i < count(); ++i) {
            if (connection(i)->selectedScope() == scope) {
                chosen = i;
                break;
            }
        }
    }
    bringBack(*window(chosen));
    connection(chosen)->openIncident(scope, machine, *wall(chosen));
}

void WallWindows::incidentUnavailable()
{
    showProblem(tr("This machine is no longer available to this account."));
}

void WallWindows::showProblem(const QString& text)
{
    if (count() == 0)
        return;
    qsizetype chosen = 0;
    for (qsizetype i = 0; i < count(); ++i) {
        if (window(i)->isActive())
            chosen = i;
    }
    actions(chosen)->tell(text, false);
}

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Windows", text);
}

// How far a new window is from the one it was asked for in: its title bar stays in sight under it.
constexpr int step = 32;

// A step down and to the right of this place, of the same size; or, where that would reach out
// of the screen, a step from the screen's corner.
QRect aStepFrom(const QRect& from, const QScreen* screen)
{
    QRect place = from.translated(step, step);
    if (screen != nullptr) {
        if (const QRect free = screen->availableGeometry(); !free.contains(place)) {
            place.moveTo(free.topLeft() + QPoint(step, step));
        }
    }
    return place;
}

} // namespace

// One window and what belongs to it. What is named later ends first: the keepers, the engine in
// the window, the window, and only then its graphics device and what its screens were handed.
struct WallWindows::One {
    One(core::Settings& settings, qsizetype index) : kept(settings, index) {}

    core::WindowSlot kept;
    WallView wall;
    SessionView session;
    ActionsView actions;
    FilesView files;
    OwnFilesView ownFiles;
    AddKvmView adding;
    EditKvmView editing;
    ControlView control;
    std::unique_ptr<ServerSession> source;
    std::unique_ptr<engine::Gpu> gpu;
    std::unique_ptr<QQuickWindow> window;
    std::unique_ptr<VideoHost> video;
    std::unique_ptr<WindowKeeper> keeper;
    std::unique_ptr<WallChoices> choices;
    // Keyboard and mouse of the machine it shows large: where the keys come from, and who sends
    // them. Ends first of all, and gives them back as it does.
    std::unique_ptr<KeySource> keys;
    std::unique_ptr<MachineControl> machine;
    // Closed by somebody who wanted it gone, and about to be forgotten.
    bool closed = false;
};

bool WallWindows::readyForUpdate() const
{
    return std::ranges::all_of(m_windows, [](const std::unique_ptr<One>& one) {
        return !one->control.isOn() && !one->control.isAsking() && !one->control.isBusy() && !one->actions.busy() &&
               !one->actions.run().value(QStringLiteral("running")).toBool() && !one->files.holdsItsWindow() &&
               !one->ownFiles.holdsItsWindow() && !one->adding.busy() && !one->editing.busy() &&
               !one->window->property("screenshotHoldsWindow").toBool();
    });
}

QString WallWindows::whatHoldsAnUpdate() const
{
    if (!readyForUpdate()) {
        return words("Finish active control, actions and file operations in all qKVM windows before installing.");
    }
    for (const auto& one : m_windows) {
        if (const QString unsaved = one->window->property("unsavedEdit").toString(); !unsaved.isEmpty()) {
            return words("\"%1\" has changes that are not saved. Save them or close it before installing.")
                .arg(unsaved);
        }
    }
    if (std::ranges::any_of(m_windows, [](const auto& one) { return one->actions.media()->busy(); })) {
        return words(
            "Virtual media are being uploaded or changed in a qKVM window. Let that finish before installing.");
    }
    if (m_server.actionsUnderWay()) {
        return words("An action sent to a machine has not been answered yet. Wait for its answer before installing.");
    }
    if (!core::SetupOverrides::program().all().isEmpty()) {
        return words("A start into BIOS setup is under way, or a server's BIOS setup boot override may still be set "
                     "(its card says so). Install once its card no longer says that.");
    }
    return {};
}

WallWindows::WallWindows(Makers makers, WallView& all, core::Settings& settings, WallPace& pace, TrayKeeper& tray,
                         ServerSession& server, QObject* parent)
    : QObject(parent), m_makers(std::move(makers)), m_all(all), m_settings(settings), m_pace(pace), m_tray(tray),
      m_server(server), m_workspaces(settings), m_awake(m_makers.keepAwake)
{
    if (m_server.appLock() != nullptr)
        m_workspaces.useAppLock(*m_server.appLock());
    m_workspaces.useWindows([this] { return captureWorkspace(); },
                            [this](const auto& windows) { return requestWorkspace(windows); });
    connect(&m_workspaces, &WorkspacesView::problemReported, this, &WallWindows::showProblem);
    // "Quit" in the icon's menu, and Windows signing its user out: Qt closes every window then,
    // and none of them was closed by somebody who wanted it gone.
    connect(&m_tray, &TrayKeeper::quitAsked, this, &WallWindows::end);
    connect(qGuiApp, &QGuiApplication::commitDataRequest, this, &WallWindows::end, Qt::DirectConnection);
    look();
}

WallWindows::~WallWindows()
{
    // Each window is taken from whoever was told of it, before it ends.
    while (!m_windows.empty()) {
        One& last = *m_windows.back();
        closeBoxes(last);
        last.files.reset();
        (last.source != nullptr ? *last.source : m_server).remove(last.wall);
        m_tray.letGo(*last.window);
        m_awake.forget(*last.window);
        // (A window that ends goes out of sight in the middle of ending: its screens are told
        // nothing then.)
        disconnect(last.window.get(), nullptr, this, nullptr);
        m_windows.pop_back();
    }
    look();
}

void WallWindows::end()
{
    m_ending = true;
}

QQuickWindow* WallWindows::window(qsizetype index) const
{
    return index >= 0 && index < count() ? m_windows[static_cast<std::size_t>(index)]->window.get() : nullptr;
}

WallView* WallWindows::wall(qsizetype index) const
{
    return index >= 0 && index < count() ? &m_windows[static_cast<std::size_t>(index)]->wall : nullptr;
}

ActionsView* WallWindows::actions(qsizetype index) const
{
    return index >= 0 && index < count() ? &m_windows[static_cast<std::size_t>(index)]->actions : nullptr;
}

FilesView* WallWindows::files(qsizetype index) const
{
    return index >= 0 && index < count() ? &m_windows[static_cast<std::size_t>(index)]->files : nullptr;
}

OwnFilesView* WallWindows::ownFiles(qsizetype index) const
{
    return index >= 0 && index < count() ? &m_windows[static_cast<std::size_t>(index)]->ownFiles : nullptr;
}

AddKvmView* WallWindows::adding(qsizetype index) const
{
    return index >= 0 && index < count() ? &m_windows[static_cast<std::size_t>(index)]->adding : nullptr;
}

EditKvmView* WallWindows::editing(qsizetype index) const
{
    return index >= 0 && index < count() ? &m_windows[static_cast<std::size_t>(index)]->editing : nullptr;
}

ControlView* WallWindows::control(qsizetype index) const
{
    return index >= 0 && index < count() ? &m_windows[static_cast<std::size_t>(index)]->control : nullptr;
}

engine::Gpu* WallWindows::gpu(qsizetype index) const
{
    return index >= 0 && index < count() ? m_windows[static_cast<std::size_t>(index)]->gpu.get() : nullptr;
}

void WallWindows::setDecoding(engine::DecodingChoice decoding)
{
    m_decoding = decoding;
    for (const std::unique_ptr<One>& one : m_windows) {
        if (one->gpu != nullptr) {
            one->gpu->setDecoding(decoding);
        }
    }
}

ServerSession* WallWindows::connection(qsizetype index) const
{
    if (index < 0 || index >= count())
        return nullptr;
    const auto& one = m_windows[static_cast<std::size_t>(index)];
    return one->source != nullptr ? one->source.get() : &m_server;
}

SessionView* WallWindows::session(qsizetype index) const
{
    auto* source = connection(index);
    return source != nullptr ? &source->view() : nullptr;
}

QList<core::WallWindow> WallWindows::captureWorkspace()
{
    if (m_ending || !m_workspaces.allowed() || !readyForUpdate() ||
        std::ranges::any_of(m_windows, [](const auto& one) { return one->closed; }))
        return {};
    QList<core::WallWindow> saved;
    for (qsizetype at = 0; at < count(); ++at) {
        auto& one = *m_windows[static_cast<std::size_t>(at)];
        one.keeper->flush();
        auto layout = one.kept.remembered();
        layout.source = connection(at)->selectedSource();
        saved.append(layout);
    }
    return saved;
}

bool WallWindows::requestWorkspace(const QList<core::WallWindow>& windows)
{
    if (m_ending || !m_workspaces.allowed() || !readyForUpdate() || windows.isEmpty() ||
        windows.size() > core::Remembered::mostWindows ||
        std::ranges::any_of(m_windows, [](const auto& one) { return one->closed; }))
        return false;
    // A QML button may be on a window that will be removed. Let its handler finish first.
    QMetaObject::invokeMethod(this, [this, windows] { restoreWorkspace(windows); }, Qt::QueuedConnection);
    return true;
}

void WallWindows::restoreWorkspace(const QList<core::WallWindow>& windows)
{
    if (m_ending || !m_workspaces.allowed() || !readyForUpdate() ||
        std::ranges::any_of(m_windows, [](const auto& one) { return one->closed; })) {
        m_workspaces.reportProblem(words("The workspace was not opened because another operation started."));
        return;
    }
    const auto before = captureWorkspace();
    const auto previousCount = count();
    // Make additional windows before changing existing ones. Failure leaves the old layout usable.
    while (count() < windows.size()) {
        auto saved = m_settings.remembered();
        while (saved.windows.size() <= count())
            saved.windows.append(core::WallWindow{});
        saved.windows[count()] = windows.at(count());
        m_settings.note(saved);
        if (!openOne()) {
            while (count() > previousCount)
                forget(m_windows.back().get());
            saved = m_settings.remembered();
            saved.windows = before;
            static_cast<void>(m_settings.remember(saved));
            m_workspaces.reportProblem(
                words("qKVM could not create every window. Your previous workspace is still open."));
            return;
        }
    }
    while (count() > windows.size())
        forget(m_windows.back().get());
    for (qsizetype at = 0; at < count(); ++at) {
        auto& one = *m_windows[static_cast<std::size_t>(at)];
        closeBoxes(one);
        one.actions.close();
        one.actions.closeForm();
        one.actions.answer(false);
        one.files.reset();
        const auto& layout = windows.at(at);
        connection(at)->selectSource(layout.source.isEmpty() ? QStringLiteral("pc") : layout.source);
        one.choices->restore(layout);
        one.keeper->restore(layout.place, m_makers.screens);
    }
    auto saved = m_settings.remembered();
    saved.windows = captureWorkspace();
    if (!m_settings.remember(saved))
        m_workspaces.reportProblem(
            words("The workspace is open, but the current layout could not be saved on this PC."));
    m_tray.bringAllBack();
}

bool WallWindows::openAll()
{
    const qsizetype were =
        std::clamp<qsizetype>(m_settings.remembered().windows.size(), 1, core::Remembered::mostWindows);
    for (qsizetype opened = 0; opened < were; ++opened) {
        if (!openOne()) {
            break;
        }
    }
    return count() > 0;
}

bool WallWindows::open(const QWindow* beside)
{
    if (count() >= core::Remembered::mostWindows) {
        return false;
    }
    auto one = std::make_unique<One>(m_settings, count());
    if (m_makers.independentSources && beside != nullptr && one->kept.remembered().source.isEmpty()) {
        for (qsizetype at = 0; at < count(); ++at) {
            if (window(at) == beside) {
                auto saved = one->kept.remembered();
                saved.source = connection(at)->selectedSource();
                static_cast<void>(one->kept.remember(saved));
                break;
            }
        }
    }
    if (m_makers.independentSources)
        one->source = m_server.makeWindow(one->session, one->wall, one->kept);
    auto& source = one->source != nullptr ? *one->source : m_server;
    one->window = m_makers.window(source.view(), m_workspaces, one->wall, one->actions, one->files, one->ownFiles,
                                  one->adding, one->editing, one->control);
    if (one->window == nullptr) {
        return false;
    }
    if (m_makers.adds) {
        m_makers.adds(one->adding);
    }
    if (m_makers.edits) {
        m_makers.edits(one->editing);
    }
    if (m_makers.ownFiles) {
        m_makers.ownFiles(one->ownFiles);
    }
    QQuickWindow& window = *one->window;
    connect(&window, &QWindow::activeChanged, this, [this, shown = &window, source = &source, wall = &one->wall] {
        if (shown->isActive())
            m_tray.follow(source->view(), *wall);
    });
    if (m_makers.imageDoor) {
        one->actions.media()->setImageDoor(m_makers.imageDoor(&window));
    }
    if (m_makers.door) {
        one->files.setDoor(m_makers.door(&window));
        one->ownFiles.setDoor(m_makers.door(&window));
    }
    // The engine is given to the window before it is shown for the first time: Qt Quick takes its
    // graphics device only then. Without a device the window still comes up, and shows no pictures.
    if (m_makers.gpu) {
        one->gpu = m_makers.gpu();
    }
    if (one->gpu != nullptr && m_decoding) {
        one->gpu->setDecoding(*m_decoding);
    }
    if (one->gpu != nullptr) {
        one->wall.setGraphicsCard(QString::fromStdString(one->gpu->adapterName()));
    }
    if (one->gpu != nullptr) {
        one->video = std::make_unique<VideoHost>(*one->gpu, &window);
    }
    m_awake.watch(window);
    // The icon's keeper before the window's own, which is then asked first when it is closed.
    m_tray.keep(window);
    // Where the window was, and how its wall was shown: as it was, or as a new one is.
    const bool wasSomewhere = one->kept.remembered().place.has_value();
    one->keeper = std::make_unique<WindowKeeper>(window, one->kept, m_makers.screens, m_makers.restMs);
    if (!wasSomewhere && beside != nullptr && beside->visibility() == QWindow::Windowed) {
        // Beside the one it was asked for in, and not exactly over another that is there already
        // (asked for twice in the same window): a window that lies over another hides it, and is
        // taken for it.
        QRect place = beside->geometry();
        for (qsizetype tries = 0; tries <= count(); ++tries) {
            place = aStepFrom(place, beside->screen());
            if (std::ranges::none_of(m_windows, [&](const std::unique_ptr<One>& other) {
                    return other->window->geometry().topLeft() == place.topLeft();
                })) {
                break;
            }
        }
        window.setGeometry(place);
    }
    // (After the keepers, and so asked before them when the window is to be closed.)
    window.installEventFilter(this);
    one->choices = std::make_unique<WallChoices>(one->wall, one->kept);
    m_pace.add(one->wall);
    source.add(one->wall, one->actions, one->video.get());
    source.serve(one->files);
    // (Its box that changes a KVM of this PC's own wall changes none while the program only shows.)
    source.serve(one->editing);
    // Keyboard and mouse of the machine it shows large, where keys can be had for it. Only a
    // window that has the keyboard and can be seen sends anything: the moment it is neither,
    // what it held is given back.
    if (m_makers.keys) {
        one->keys = m_makers.keys(window);
    }
    if (one->keys != nullptr) {
        // (With the one memory of what is held that all the windows have: a key that was kept
        // from Windows in one window is known in the next one that takes keyboard and mouse.)
        one->machine = std::make_unique<MachineControl>(one->control, *one->keys, m_kept);
        source.drive(one->wall, *one->machine);
        const auto look = [shown = &window, machine = one->machine.get()] {
            machine->setInFront(shown->isActive() && shown->visibility() != QWindow::Hidden &&
                                shown->visibility() != QWindow::Minimized);
        };
        connect(&window, &QWindow::activeChanged, one->machine.get(), look);
        connect(&window, &QWindow::visibilityChanged, one->machine.get(), look);
        look();
    }

    One* which = one.get();
    connect(&one->wall, &WallView::unseenChanged, this, &WallWindows::look);
    connect(&window, &QQuickWindow::closing, this, [this, which] { closing(which); });
    // (A window that has no way of asking for another does not ask.)
    if (window.metaObject()->indexOfSignal("windowAsked()") >= 0) {
        connect(&window, SIGNAL(windowAsked()), this, SLOT(openAnother()));
    }
    // The one rule for both boxes, and for every way out of sight: a window that was closed is
    // out of sight before it is forgotten, and the last one goes behind the icon without being
    // closed at all. (A window that is only made small is in the taskbar, and comes back as it
    // was left.)
    // Whether somebody sees what this window tells in its corner: it is in sight, not made
    // small, and the active one. A notice that waits for that has its time from when the
    // window is looked at: one that is only in sight again, behind another program's, is
    // read by nobody.
    const auto seen = [which, shown = &window] {
        which->actions.notices()->setSeen(shown->isVisible() && shown->isActive() &&
                                          shown->visibility() != QWindow::Minimized);
    };
    connect(&window, &QWindow::visibleChanged, this, [which, seen](bool isVisible) {
        // (First: what a box says as it is closed is a notice of this window's, which stays
        // until somebody sees the window again.)
        seen();
        if (!isVisible) {
            closeBoxes(*which);
        }
    });
    connect(&window, &QWindow::activeChanged, this, seen);
    connect(&window, &QWindow::visibilityChanged, this, seen);
    m_windows.push_back(std::move(one));
    which->keeper->show();
    source.beginWindow();
    look();
    return true;
}

void WallWindows::openAnother()
{
    if (open(qobject_cast<QWindow*>(sender()))) {
        return;
    }
    for (const std::unique_ptr<One>& one : m_windows) {
        if (one->window.get() == sender()) {
            one->actions.notices()->tell(
                Notices::Kind::Info, words("qKVM opens no more than %1 windows.").arg(core::Remembered::mostWindows));
        }
    }
}

bool WallWindows::eventFilter(QObject* watched, QEvent* event)
{
    // A window that has something of a file under way stays: one of Windows' own boxes may stand
    // over it and would be left standing over nothing, and what the server answers would be told
    // to nobody. It is closed when that is over. (Not held: the program ending as a whole.) So
    // for a file of a server's wall, and so for one of this PC's own.
    if (event->type() == QEvent::Close && !m_ending) {
        const bool isBusy = std::ranges::any_of(m_windows, [&](const std::unique_ptr<One>& one) {
            return one->window.get() == watched && (one->files.holdsItsWindow() || one->ownFiles.holdsItsWindow() ||
                                                    one->window->property("screenshotHoldsWindow").toBool());
        });
        if (isBusy) {
            event->ignore();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

void WallWindows::closing(One* which)
{
    // The program as a whole is ending, or this is the last window that is open, which ends it:
    // there again the next time, as it is now.
    // (What a box of it saved in this very turn is told to nobody then: its notice is in a
    // window that no one sees again, there is no other to say it in, and it is not kept for
    // the next start. That is borne: whoever saved had said so in the box, nothing is saved
    // without that, and the KVM's card shows how it is the next time.)
    const auto open = std::ranges::count_if(m_windows, [](const std::unique_ptr<One>& one) { return !one->closed; });
    if (m_ending || open <= 1) {
        return;
    }
    which->closed = true;
    // Once the window is done with being closed: it ends with what is forgotten of it.
    QMetaObject::invokeMethod(this, [this, which] { forget(which); }, Qt::QueuedConnection);
}

void WallWindows::forget(One* which)
{
    const auto found =
        std::ranges::find_if(m_windows, [&](const std::unique_ptr<One>& one) { return one.get() == which; });
    if (found == m_windows.end()) {
        return;
    }
    const auto index = static_cast<qsizetype>(found - m_windows.begin());
    closeBoxes(*which);
    which->files.reset();
    (which->source != nullptr ? *which->source : m_server).remove(which->wall);
    m_tray.letGo(*which->window);
    m_awake.forget(*which->window);
    disconnect(which->window.get(), nullptr, this, nullptr);
    // What it still had to say and nobody saw (its box had saved something in the turn in
    // which the window was closed) goes with it, and is said in a window that is open: one
    // was, when this one was closed, or this one would not be forgotten.
    const QList<Notices::Told> unsaid = which->actions.notices()->takeWhatWaits();
    m_windows.erase(found);
    const auto open = std::ranges::find_if(m_windows, [](const std::unique_ptr<One>& one) { return !one->closed; });
    if (open != m_windows.end()) {
        for (const Notices::Told& one : unsaid) {
            (*open)->actions.notices()->tellWhenSeen(one.kind, one.text);
        }
    }
    // What was remembered of it goes, and the windows after it are each one earlier from now on.
    if (core::Remembered remembered = m_settings.remembered(); index < remembered.windows.size()) {
        remembered.windows.removeAt(index);
        // (A PC whose settings cannot be written has the window back the next time.)
        static_cast<void>(m_settings.remember(remembered));
    }
    for (qsizetype later = index; later < count(); ++later) {
        m_windows[static_cast<std::size_t>(later)]->kept.moveTo(later);
    }
    look();
}

void WallWindows::look()
{
    m_all.setUnseen(
        std::ranges::all_of(m_windows, [](const std::unique_ptr<One>& one) { return one->wall.isUnseen(); }));
}

void WallWindows::closeBoxes(One& of)
{
    of.adding.close();
    of.editing.close();
    // (And what was laid out of a file is forgotten, the passwords of the file with it.)
    of.ownFiles.close();
}

} // namespace qkvm::ui
