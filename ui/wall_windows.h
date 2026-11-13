#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QList>
#include <QObject>
#include <QQuickWindow>
#include <QRect>

#include <qkvm/core/settings.h>
#include <qkvm/engine/gpu.h>
#include <qkvm/engine/types.h>

#include "actions_view.h"
#include "add_kvm_view.h"
#include "control_view.h"
#include "edit_kvm_view.h"
#include "files_view.h"
#include "key_source.h"
#include "keys_kept.h"
#include "own_files_view.h"
#include "server_session.h"
#include "tray_keeper.h"
#include "wall_pace.h"
#include "wall_view.h"
#include "win/display_awake.h"
#include "workspaces_view.h"

namespace qkvm::ui {

// The program's windows. Each shows the wall by itself: laid out and narrowed as was chosen in
// it, and where it was the last time, on the monitor it was on. At the start as many are opened
// as were open when the program was last closed, and one where it never ran; "New window" in a
// window opens another, up to core::Remembered::mostWindows of them.
//
// A window that is closed while another is open is gone, the next time too, and the windows
// after it move up in what is remembered. The last one is the program's: closing it ends the
// program, or puts it behind its icon where that was chosen (TrayKeeper), and it is there again
// the next time. So are all the windows that are open when the program is ended as a whole: from
// its icon, or by Windows.
//
// Each window has a graphics device of its own. Qt draws every window on a thread of that
// window's, and what two threads draw at once cannot go through one device.
//
// On the interface thread. Made after everything it is given, and ended before any of it.
class WallWindows : public QObject {
    Q_OBJECT

public:
    // How a window is made, by whoever knows: the program makes its screens from QML and a
    // graphics device as Windows gives one; a test makes plain windows, and none.
    struct Makers {
        // The window for this wall, for what is done to machines from it, for a server's wall as
        // a file and for this PC's own, for the boxes that add a KVM and change one, and for the
        // keyboard and mouse of the machine it shows large, not shown yet; none where it cannot
        // be made.
        std::function<std::unique_ptr<QQuickWindow>(SessionView& session, WorkspacesView& workspaces, WallView& wall,
                                                    ActionsView& actions, FilesView& files, OwnFilesView& ownFiles,
                                                    AddKvmView& adding, EditKvmView& editing, ControlView& control)>
            window;
        // Fixtures may deliberately exercise several views of one shared conversation.
        bool independentSources = true;
        // Who adds the KVM that a window's box asks for. Without one, the box asks nobody.
        std::function<void(AddKvmView& adding)> adds;
        // And who changes the KVM that a window's other box is open for. Without one, that box
        // is never opened.
        std::function<void(EditKvmView& editing)> edits;
        // And who writes this PC's wall to a file and reads one, for a window's two boxes.
        // Without one, those boxes make and read nothing.
        std::function<void(OwnFilesView& ownFiles)> ownFiles;
        // How a file is chosen from a window: Windows' own boxes over that window in the program.
        // Without one, nobody can be asked, and no file is kept or opened.
        std::function<FileDoor(QWindow* over)> door;
        // And how an image is chosen for a machine's drive (MediaView): Windows' own box over that window. Without
        // one, nobody is asked, and no image is uploaded.
        std::function<std::function<base::Result<QString>()>(QWindow* over)> imageDoor;
        // A device for its pictures. Without one, or where none can be made, the window comes up
        // and shows no pictures.
        std::function<std::unique_ptr<engine::Gpu>()> gpu;
        // For the windows' keepers (see WindowKeeper): which screens there are, where a test has
        // its own, and how soon a window has come to rest.
        std::function<QList<QRect>()> screens;
        int restMs = 400;
        // How the display is kept awake while a window fills a screen: Windows' own way, unless a
        // test hands in its own.
        std::function<void(bool)> keepAwake = keepDisplayAwake;
        // Where a window's keys come from while the machine it shows large is controlled: Windows'
        // own way of hearing every key in the program, one that is fed by hand in a test. One for
        // each window, and made for it: Windows' own way asks, as each key goes down, whether that
        // window is the one in front. Without one, no machine is controlled from a window.
        std::function<std::unique_ptr<KeySource>(QWindow&)> keys;
    };

    // `all` is the program's own wall, which no window shows: nobody sees it while nobody sees a
    // window.
    WallWindows(Makers makers, WallView& all, core::Settings& settings, WallPace& pace, TrayKeeper& tray,
                ServerSession& server, QObject* parent = nullptr);
    ~WallWindows() override;
    WallWindows(const WallWindows&) = delete;
    WallWindows& operator=(const WallWindows&) = delete;
    WallWindows(WallWindows&&) = delete;
    WallWindows& operator=(WallWindows&&) = delete;

    // The windows that were open the last time, each as it was; one, where there were none. False
    // where not one could be made.
    bool openAll();
    // One more, as a new one is. False where it could not be made, or there are as many as there
    // may be.
    bool openOne() { return open(nullptr); }
    // The program is ending as a whole: windows that are closed from now on are there again the
    // next time.
    void end();
    bool readyForUpdate() const;
    // What keeps the program from closing for an update to be installed, in the person's words; empty where nothing
    // does. All that readyForUpdate waits for, and: a virtual media upload or change in any window, an action sent to a
    // machine from any window and not answered yet (a start into BIOS setup among them), a start into BIOS setup of
    // this PC's own servers under way or an override of one that may still be set (core::SetupOverrides: the program's
    // end waits for its clearing), and a box with changes that are not saved (its window's "unsavedEdit", its title).
    QString whatHoldsAnUpdate() const;
    void openIncident(const QString& scope, const QString& machine);
    void incidentUnavailable();
    void showProblem(const QString& text);

    qsizetype count() const { return static_cast<qsizetype>(m_windows.size()); }
    // The windows in the order they were opened in, and what each was given: for whoever does
    // something to one of them from outside (a test, a rig).
    QQuickWindow* window(qsizetype index) const;
    WallView* wall(qsizetype index) const;
    ActionsView* actions(qsizetype index) const;
    FilesView* files(qsizetype index) const;
    OwnFilesView* ownFiles(qsizetype index) const;
    AddKvmView* adding(qsizetype index) const;
    EditKvmView* editing(qsizetype index) const;
    ControlView* control(qsizetype index) const;
    SessionView* session(qsizetype index) const;
    engine::Gpu* gpu(qsizetype index) const;
    WorkspacesView& workspaces() { return m_workspaces; }

    // How video is decoded from now on, by the graphics device of every window that has one
    // (engine::Gpu::setDecoding), and of every window opened after this.
    void setDecoding(engine::DecodingChoice decoding);

    // A window that has something of a file under way is not closed.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // A window asked for another ("New window"): opened, or said in that window why not.
    void openAnother();

private:
    struct One;

    // One more. Where nothing is remembered of where it was, and it was asked for in a window that
    // is of its own size: a step down and to the right of that one, of its size, where both are
    // seen; and otherwise where Windows puts it.
    bool open(const QWindow* beside);
    // A window is being closed, and nothing held it back.
    void closing(One* which);
    // It is gone: what it had is ended, what was remembered of it is forgotten, and the windows
    // after it move up. A notice of it that still waited for somebody to see the window is told
    // in a window that is open, and waits there for that one to be seen.
    void forget(One* which);
    // Whether anybody sees a window is told to the program's own wall.
    void look();
    // Nobody sees it any more (it was closed, or is behind the program's icon): its boxes that
    // add and change a KVM are closed, and those for this PC's wall as a file. What they were
    // asking is given up and what was typed into them is emptied, there and then: nothing is
    // typed into, saved from, taken from a file or said yes to in a window nobody sees, what
    // was laid out of a file is forgotten, and nothing of a window that goes is done in the
    // turn of the loop it is still there for. What a box had done in that very turn is said by the window, as
    // a notice that stays until somebody sees the window again (Notices::tellWhenSeen): so
    // the window's notices are told that nobody sees it before its boxes are closed. Seen is
    // a window that is in sight, not made small, and the active one.
    static void closeBoxes(One& of);
    QList<core::WallWindow> captureWorkspace();
    bool requestWorkspace(const QList<core::WallWindow>& windows);
    void restoreWorkspace(const QList<core::WallWindow>& windows);
    ServerSession* connection(qsizetype index) const;

    Makers m_makers;
    WallView& m_all;
    core::Settings& m_settings;
    WallPace& m_pace;
    TrayKeeper& m_tray;
    ServerSession& m_server;
    WorkspacesView m_workspaces;
    // A wall that fills a screen is looked at without anybody touching the PC.
    AwakeInFullScreen m_awake;
    // The keys that were kept from Windows while a machine was controlled and are still held:
    // what is physically down is one thing for the whole program, so the controls of all the
    // windows have this one memory of it. (Before the windows: it ends after them.)
    KeysKept m_kept;
    std::vector<std::unique_ptr<One>> m_windows;
    std::optional<engine::DecodingChoice> m_decoding;
    bool m_ending = false;
};

} // namespace qkvm::ui
