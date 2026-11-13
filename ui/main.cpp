#include <memory>
#include <qkvm/core/video_diagnostics.h>

#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QStandardPaths>

#include <windows.h>

#include <qkvm/core/auto_start.h>
#include <qkvm/core/data_folder.h>
#include <qkvm/core/jetkvm_connection.h>
#include <qkvm/core/pacing_file.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/screen_watch.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/setup_overrides.h>
#include <qkvm/engine/gpu.h>

#include "about_model.h"
#include "actions_view.h"
#include "add_kvm_view.h"
#include "application.h"
#include "control_view.h"
#include "edit_kvm_view.h"
#include "files_view.h"
#include "installer/health_exchange.h"
#include "installer/package_environment.h"
#include "installer_launcher.h"
#include "look.h"
#include "own_files_view.h"
#include "own_kvms_view.h"
#include "own_wall.h"
#include "package_check.h"
#include "program_end.h"
#include "quick_access.h"
#include "screen_shrink.h"
#include "server_session.h"
#include "session_view.h"
#include "settings_view.h"
#include "tray.h"
#include "tray_keeper.h"
#include "wall_pace.h"
#include "wall_view.h"
#include "wall_windows.h"
#include "win/cache_paths.h"
#include "win/crash_report.h"
#include "win/file_dialogs.h"
#include "win/hand_over_end.h"
#include "win/keyboard_hook.h"
#include "win/own_temp.h"
#include "win/setup_wait.h"
#include "win/shell_start.h"
#include "win/show_file.h"
#include "win/single_instance.h"
#include <cstring>

using namespace Qt::StringLiterals;

// A shipped program: base/tests/quiet_failures.cpp, which is for test programs alone, defines this too, and cannot
// be linked into this one.
extern "C" const int qkvmTestProgram = 0;

int main(int argc, char* argv[])
{
    bool checksThePackage = false;
    bool afterUpdate = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--check-package") == 0) {
            checksThePackage = true;
        } else if (std::strcmp(argv[index], qkvm::ui::afterUpdateArgument) == 0) {
            afterUpdate = true;
        }
    }
    // While qKVM's Setup is at work, a start that is not Setup's own (with --after-update) ends at once, before any
    // window and before the single-instance mutex, saying nothing: a program that runs while Setup replaces its files
    // keeps them from being replaced. Setup's own waits for it to be over, and starts the program anew through the
    // person's shell (no child of Setup's, with none of its mitigations), and ends; where that fails, it goes on
    // itself.
    if (!checksThePackage) {
        switch (qkvm::ui::whileSetupWorks(QString::fromLatin1(qkvm::ui::setupMutexName), afterUpdate,
                                          std::chrono::minutes(2), &qkvm::ui::startAnewThroughTheShell)) {
        case qkvm::ui::WhileSetupWorks::GoOn:
            break;
        case qkvm::ui::WhileSetupWorks::StartedAnew:
        case qkvm::ui::WhileSetupWorks::BeingUpdated:
        case qkvm::ui::WhileSetupWorks::StillUpdating:
            // Started anew through the shell, or Setup at work: at once and without a word: a box that waits for a
            // click would keep this program and its libraries loaded, and Setup could not replace them (it tries a file
            // in use for four seconds, then rolls back).
            return 0;
        }
    } else {
        qkvm::installer::preparePackageCheckEnvironment();
    }
    // A program that qKVM's Setup started at its end has Setup's own TEMP and TMP: the person's own again, first.
    qkvm::ui::takeBackOwnTemp();
    std::unique_ptr<qkvm::ui::CrashReport> crashReport;
    // The engine draws underneath the QML scene with Direct3D 11 (DESIGN.md section 3.6), so Qt Quick
    // has to draw with it too, whatever Qt would choose by itself.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication app(argc, argv);
    qkvm::ui::prepareApplication();
    if (app.arguments().contains(u"--check-package"_s)) {
        const int result = qkvm::ui::checkPackage();
        const qsizetype health = app.arguments().indexOf(u"--health-check"_s);
        if (health >= 0 && (health + 1 >= app.arguments().size() ||
                            !qkvm::installer::reportHealth(app.arguments().at(health + 1).toStdWString(),
                                                           static_cast<std::uint32_t>(result))))
            return 2;
        return result;
    }

    // One running copy: started again, the program shows the copy that runs, and ends here.
    const qkvm::ui::SingleInstance only(u"QKVM Desktop"_s);
    if (!only.isFirst()) {
        return 0;
    }
    // Its data, in a folder that is there but cannot be read (a list that shuts the person out, a junction this start
    // may not cross), is never taken for none: no first start, and nothing written. Said, and the program ends.
    if (const QString why = qkvm::core::whyDataCannotBeRead(qkvm::core::Settings::usualFile()); !why.isEmpty()) {
        MessageBoxW(nullptr, why.toStdWString().c_str(), L"qKVM", MB_OK | MB_ICONERROR);
        return 1;
    }

    QString diagnosticDirectory;
    QString downloadDirectory;
    try {
        diagnosticDirectory = QString::fromStdWString(qkvm::storage::cache(qkvm::storage::Area::Diagnostics).wstring());
        downloadDirectory = QString::fromStdWString(qkvm::storage::cache(qkvm::storage::Area::Downloads).wstring());
        crashReport =
            std::make_unique<qkvm::ui::CrashReport>(std::filesystem::path(diagnosticDirectory.toStdWString()));
    } catch (...) {
        MessageBoxW(nullptr, L"qKVM could not open its local cache. Check folder permissions and try again.", L"qKVM",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    const qkvm::core::VideoDiagnostics diagnostics(diagnosticDirectory + u"/video.jsonl"_s);
    // At a crash, once its summary is written, the lines that wait to be written are given a while: the nearest to it.
    qkvm::ui::CrashReport::alsoAtACrash(&qkvm::core::VideoDiagnostics::writeWhatWaitsAtACrash);
    qkvm::core::VideoDiagnostics::record("application_start", {{"schema", 1}});

    qkvm::ui::AboutModel about;
    about.useDiagnostics(
        [diagnosticDirectory] { return QDesktopServices::openUrl(QUrl::fromLocalFile(diagnosticDirectory)); });
    // The program's own wall, which no window shows: the machines as the server gives them, for
    // the icon and the messages. Empty until somebody is signed in to a QKVM.
    qkvm::ui::WallView wall;
    qkvm::ui::SessionView session;
    // What is remembered on this PC, read before the first window: how the program looks is part of it.
    qkvm::core::Settings settings(qkvm::core::Settings::usualFile());
    qkvm::ui::Look look(settings);
    // Whether the program starts with Windows is asked of Windows, and changed there, for the file
    // this program was started from.
    const std::unique_ptr<qkvm::core::AutoStart> autoStart = qkvm::core::windowsAutoStart(
        QCoreApplication::applicationFilePath(), u"QKVM Desktop"_s, {}, qkvm::core::windowsPackageFamily());
    qkvm::ui::SettingsView ownSettings(*autoStart, settings);
    QNetworkAccessManager updateNetwork;
    qkvm::core::Updates updates(qkvm::core::desktopUpdateChannel(), settings, updateNetwork, downloadDirectory);
    qkvm::ui::UpdatesView updateView(updates);
    ownSettings.useUpdates(updateView);
    // Each choice of graphics card is shown with what the card it comes to here calls itself.
    ownSettings.nameAdaptersWith([](qkvm::engine::AdapterChoice choice) {
        return QString::fromStdString(qkvm::engine::Gpu::adapterFor(choice));
    });
    ownSettings.seeMonitorsWith(&qkvm::engine::Gpu::adapterHasMonitor);

    QQmlApplicationEngine engine;
    qkvm::ui::keepOffTheNetwork(engine);
    // The screens of one window: made once, and a window from them for each wall that is shown.
    QQmlComponent screens(&engine);
    screens.loadFromModule("Qkvm", "Main");

    // The program's icon in the notification area, and the program going on behind it where that
    // was chosen.
    const std::unique_ptr<qkvm::ui::TrayIcon> tray = qkvm::ui::windowsTray();
    qkvm::ui::TrayKeeper presence(*tray, session, wall, settings);
    QObject::connect(&presence, &qkvm::ui::TrayKeeper::quitAsked, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    QObject::connect(&only, &qkvm::ui::SingleInstance::askedToShow, &presence, &qkvm::ui::TrayKeeper::bringAllBack);
    // How often a new picture is asked for is one choice for every wall there is.
    qkvm::ui::WallPace pace(settings);
    pace.add(wall);
    const std::unique_ptr<qkvm::core::SecretStore> secrets = qkvm::core::windowsSecrets();
    // What the sign-ins of the run before were told (the wrong-password counts and how long each waits), read back
    // before anything of this run can sign in anywhere, and kept beside the settings from now on (a9).
    qkvm::core::PacingFile signIns(qkvm::core::SignInPacing::program(),
                                   qkvm::core::Settings::usualFile() + u".sign-ins"_s, *secrets);
    signIns.read();
    qkvm::ui::AppLock security(*secrets, qkvm::core::Settings::usualFile() + u".locked"_s);
    // Which servers' BIOS setup boot override may be set, a start into BIOS setup of the run before not having ended
    // (review 4's MEDIUM 1): read before any of their drivers is made, and kept beside the settings from now on.
    qkvm::core::SetupOverrides::program().keepIn(qkvm::core::Settings::usualFile() + u".setup-overrides"_s);
    // This PC's own wall: the KVMs that were added in the program, and the adding of one.
    qkvm::ui::OwnWall own(settings, *secrets);
    own.useAppLock(security);
    // At the program's end, while its loop can still run: the goodbyes (servers' controllers, a start into BIOS setup
    // under way, a JetKVM letting go), and only then, where it handed over to Setup, the count to its forced end
    // (ui/program_end.h).
    qkvm::ui::HandOverEnd handOverEnd;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &own,
                     [&own, &handOverEnd] { qkvm::ui::endTheProgram(own, handOverEnd); });
    QObject::connect(&own.wall(), &qkvm::core::LocalWall::portableSettingsChanged, &pace, &qkvm::ui::WallPace::reload);
    QObject::connect(&own.wall(), &qkvm::core::LocalWall::portableSettingsChanged, &updates,
                     &qkvm::core::Updates::changed);
    qkvm::ui::OwnActionsView savedActions(own.wall().actions(), security);
    ownSettings.useSecurity(security, savedActions);
    // Its KVMs as the settings list them, for every window.
    qkvm::ui::OwnKvmsView kvms;
    kvms.use(&own.wall());
    qkvm::ui::ServerSession server(session, wall, settings, *secrets);
    server.useOwnWall(own.wall());
    server.useAppLock(security);
    // Persistent incidents follow completed observations; hidden windows can notify through the icon.
    qkvm::core::Incidents incidents(qkvm::core::Settings::usualFile() + u".incidents"_s);
    qkvm::ui::AlertsView alerts(incidents, security);
    ownSettings.useAlerts(alerts);
    // This PC's own screen watches (c7): their pictures kept beside the settings, and their alerts told as the changes
    // of state are.
    own.wall().screens().shrinkWith(&qkvm::ui::shrunkScreen);
    own.wall().screens().keepIn(QFileInfo(qkvm::core::Settings::usualFile()).absolutePath() + u"/alert-pictures"_s);
    QObject::connect(&own.wall().screens(), &qkvm::core::ScreenWatch::alerted, &alerts,
                     [&alerts](const qkvm::core::ScreenWatch::Event& event) { alerts.screenAlertFor(u"pc"_s, event); });
    server.useAlerts(alerts);
    qkvm::ui::QuickAccess shortcuts(settings, alerts, security);
    server.useQuickAccess(shortcuts);
    presence.useQuickAccess(shortcuts);
    QObject::connect(&alerts, &qkvm::ui::AlertsView::notification, tray.get(),
                     [&wall, &settings, &tray](const QString& title, const QString& text, bool warning,
                                               const QString& scope, const QString& machine) {
                         if (wall.isUnseen() && settings.remembered().tellsNews)
                             tray->tellAbout(title, text, warning, scope, machine);
                     });
    QObject::connect(&security, &qkvm::ui::AppLock::changed, tray.get(), [&] {
        if (security.restricted())
            tray->clearWaitingNotifications();
    });
    QObject::connect(&wall, &qkvm::ui::WallView::unseenChanged, tray.get(), [&] {
        if (!wall.isUnseen())
            tray->clearWaitingNotifications();
    });
    QObject::connect(&ownSettings, &qkvm::ui::SettingsView::changed, tray.get(), [&] {
        if (!settings.remembered().tellsNews)
            tray->clearWaitingNotifications();
    });
    QObject::connect(&server, &qkvm::ui::ServerSession::wallWasLeft, tray.get(),
                     &qkvm::ui::TrayIcon::clearWaitingNotifications);
    // A machine's own console opens in the browser Windows has for web addresses. Only such an
    // address gets this far (core::webAddress).
    server.openPagesWith([](const QUrl& address) { return QDesktopServices::openUrl(address); });

    // The windows, last of all: they end first, each taken from the session before its screens and
    // its engine go.
    qkvm::ui::WallWindows::Makers makers;
    makers.window = [&](qkvm::ui::SessionView& windowSession, qkvm::ui::WorkspacesView& workspaces,
                        qkvm::ui::WallView& shown, qkvm::ui::ActionsView& actions, qkvm::ui::FilesView& files,
                        qkvm::ui::OwnFilesView& ownFiles, qkvm::ui::AddKvmView& adding, qkvm::ui::EditKvmView& editing,
                        qkvm::ui::ControlView& control) {
        std::unique_ptr<QObject> made(
            screens.createWithInitialProperties({{u"about"_s, QVariant::fromValue(&about)},
                                                 {u"session"_s, QVariant::fromValue(&windowSession)},
                                                 {u"workspaces"_s, QVariant::fromValue(&workspaces)},
                                                 {u"wall"_s, QVariant::fromValue(&shown)},
                                                 {u"actions"_s, QVariant::fromValue(&actions)},
                                                 {u"look"_s, QVariant::fromValue(&look)},
                                                 {u"settings"_s, QVariant::fromValue(&ownSettings)},
                                                 {u"files"_s, QVariant::fromValue(&files)},
                                                 {u"ownFiles"_s, QVariant::fromValue(&ownFiles)},
                                                 {u"adding"_s, QVariant::fromValue(&adding)},
                                                 {u"editing"_s, QVariant::fromValue(&editing)},
                                                 {u"control"_s, QVariant::fromValue(&control)},
                                                 {u"kvms"_s, QVariant::fromValue(&kvms)}}));
        std::unique_ptr<QQuickWindow> window;
        if (qobject_cast<QQuickWindow*>(made.get()) != nullptr) {
            window.reset(qobject_cast<QQuickWindow*>(made.release()));
        }
        return window;
    };
    // A KVM that a window's box asks for is added to this PC's own wall.
    makers.adds = [&own](qkvm::ui::AddKvmView& adding) { own.serve(adding); };
    // And a KVM of that wall is changed, or taken off it, from a window's other box.
    makers.edits = [&own](qkvm::ui::EditKvmView& editing) { own.serve(editing); };
    // And that wall is written to a file, and KVMs of a file are taken onto it, from a window's
    // two boxes for that.
    makers.ownFiles = [&own](qkvm::ui::OwnFilesView& ownFiles) { own.serve(ownFiles); };
    // The wall as a file is kept, and chosen, in Windows' own boxes over the window that asked.
    makers.door = [](QWindow* over) { return qkvm::ui::windowsFileDoor(over); };
    makers.imageDoor = [](QWindow* over) { return qkvm::ui::windowsImageDoor(over); };
    // Every window's graphics device is on the card chosen when the program started; how it decodes
    // is the windows' to tell it (below).
    makers.gpu = [choice = ownSettings.startedOn()] {
        qkvm::engine::Gpu::Options options;
        options.choice = choice;
        qkvm::base::Result<std::unique_ptr<qkvm::engine::Gpu>> made = qkvm::engine::Gpu::create(options);
        return made ? std::move(made).value() : std::unique_ptr<qkvm::engine::Gpu>();
    };
    // While a machine is controlled from a window, its keys are every key of this PC's keyboard,
    // heard Windows' own way.
    makers.keys = [](QWindow& window) { return qkvm::ui::windowsKeyboard(window); };
    qkvm::ui::WallWindows windows(std::move(makers), wall, settings, pace, presence, server);
    // How video is decoded, as chosen: in every window, from the first one on, and at once when it is
    // chosen anew.
    windows.setDecoding(ownSettings.decoding());
    QObject::connect(&ownSettings, &qkvm::ui::SettingsView::decodingChosen, &windows,
                     [&] { windows.setDecoding(ownSettings.decoding()); });
    QObject::connect(tray.get(), &qkvm::ui::TrayIcon::notificationOpened, &windows,
                     &qkvm::ui::WallWindows::openIncident);
    QObject::connect(tray.get(), &qkvm::ui::TrayIcon::shortcutChosen, &windows, &qkvm::ui::WallWindows::openIncident);
    QObject::connect(&shortcuts, &qkvm::ui::QuickAccess::problem, &windows, &qkvm::ui::WallWindows::showProblem);
    QObject::connect(&server, &qkvm::ui::ServerSession::incidentDetailsAsked, &ownSettings,
                     &qkvm::ui::SettingsView::openIncidents);
    QObject::connect(&alerts, &qkvm::ui::AlertsView::openAsked, &windows,
                     [&](const QString& scope) { windows.openIncident(scope, {}); });
    QObject::connect(&server, &qkvm::ui::ServerSession::incidentUnavailable, &windows,
                     &qkvm::ui::WallWindows::incidentUnavailable);
    // Installing waits for every window to be idle, and, before the program closes for Setup, for what the program's
    // end would cut (WallWindows::whatHoldsAnUpdate), which it then says.
    updateView.holdWith([&windows] { return windows.whatHoldsAnUpdate(); });
    updateView.installWith(
        [&windows, &security] { return !security.restricted() && windows.whatHoldsAnUpdate().isEmpty(); },
        [&updates](const QString& file) {
            return updates.release() && qkvm::ui::startInstaller(u"--install"_s, file, updates.release()->version);
        });
    // A Setup copy's: once Setup has started, the windows close and the program ends, Setup waiting for its single-
    // instance mutex to go before it installs. Where the quit is held up, the program is ended by force 9 s after its
    // goodbyes, and within 50 s in any case (Setup gives up after 60 s): ui::HandOverEnd.
    updateView.handOverWith([&windows, &handOverEnd] {
        handOverEnd.begin();
        windows.end();
        QCoreApplication::quit();
    });
    // A portable copy's verified update is shown in its folder, for the person to unpack: never opened or run here.
    updateView.showWith(
        [&security](const QString& file) { return !security.restricted() && qkvm::ui::showFileInItsFolder(file); });
    if (qkvm::ui::installerConfigured()) {
        updateView.recoverWith(
            [](bool previous) { return qkvm::ui::startInstaller(previous ? u"--rollback"_s : u"--recover"_s); });
    }
    if (!windows.openAll()) {
        return 1;
    }
    // A file of what the program remembered that could not be read is kept, and said: the list of
    // this PC's own KVMs was in it.
    if (const QString said = qkvm::ui::OwnWall::saidOfSettingsThatCouldNotBeRead(settings); !said.isEmpty()) {
        windows.actions(0)->tell(said, false);
    }
    // And a server whose BIOS setup boot override may still be set from the run before: told, until somebody sees it
    // (its card says it too, until a look shows the override off).
    for (const QString& said : qkvm::core::SetupOverrides::program().toldAtStart(settings.remembered().kvms)) {
        windows.actions(0)->tellWhenSeen(said, false);
    }
    // And what became of the run before's hand-over to Setup: installed, or not (with Setup's log), or Setup still
    // waiting; told again when such a Setup ends.
    const auto tellAfterSetup = [&windows, &updates] {
        if (!updates.afterSetup().isEmpty()) {
            windows.actions(0)->tellWhenSeen(updates.afterSetup(), !updates.afterSetupWentWrong());
        }
    };
    tellAfterSetup();
    QObject::connect(&updates, &qkvm::core::Updates::afterSetupTold, &app, tellAfterSetup);
    // It goes at once to the wall that was shown last: this PC's own, or the server's that was
    // used last.
    bool sourcesStarted = false;
    const auto startSources = [&] {
        if (!sourcesStarted && security.canStartSources()) {
            sourcesStarted = true;
            server.begin();
        }
    };
    QObject::connect(&security, &qkvm::ui::AppLock::changed, &app, startSources);
    QObject::connect(&security, &qkvm::ui::AppLock::resetFinished, &app, [&] {
        windows.end();
        app.quit();
    });
    // Quit, chosen where Windows did not hand over what qKVM keeps.
    QObject::connect(&security, &qkvm::ui::AppLock::quitAsked, &app, [&] {
        windows.end();
        app.quit();
    });
    startSources();

    QObject::connect(&updates, &qkvm::core::Updates::available, &app, [&] {
        const auto& release = updates.release();
        if (!release) {
            return;
        }
        const QString message =
            QCoreApplication::translate("Updates", "qKVM %1 is available. Open Settings > Updates to review it.")
                .arg(release->version);
        tray->tell(u"qKVM"_s, message, false);
        for (qsizetype index = 0; index < windows.count(); ++index) {
            windows.actions(index)->tell(message, true);
        }
    });
    QTimer updateClock;
    QObject::connect(&updateClock, &QTimer::timeout, &updates,
                     [&updates] { updates.checkIfDue(QDateTime::currentDateTimeUtc()); });
    updateClock.start(60000);
    QTimer::singleShot(15000, &updates, [&updates] { updates.checkIfDue(QDateTime::currentDateTimeUtc()); });

    return QGuiApplication::exec();
}
