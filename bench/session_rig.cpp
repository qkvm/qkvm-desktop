// The program's own window against a QKVM server, signed in without anybody typing: for looking at
// the wall with the stand-in server of the app's tests (python -m tests.demo, in ../app).
//
//   qkvm-session-rig <address of the server> [seconds to run; 0: until closed] [folder to remember in]
//
// The password is read from the environment, QKVM_RIG_PASSWORD, and a user name from QKVM_RIG_NAME
// where the server has several accounts. They are the stand-in's: this is never given a real one.
//
// With QKVM_RIG_ENLARGE set to a machine's name, that machine is shown large once the wall has it,
// as a double click on its card would. With QKVM_RIG_VIEW set to "order,columns,group" (as in
// "name,3,Rack A"; the group may be left out), the wall is shown so, as the bar's menus would set it.
//
// With QKVM_RIG_MENU set to a machine's name, that machine's menu is opened once the wall has it, as
// a click in the corner of its card would. With QKVM_RIG_DO set to the id of a row of that menu (as
// in "keys:ShiftLeft"), the row is chosen a moment later, and with QKVM_RIG_YES set as well, the
// question that follows is answered with yes. Where the row leads to the box for a text or for keys,
// QKVM_RIG_TEXT or QKVM_RIG_KEYS is what is put into it. That does something to a machine, so the rig does it
// only to the stand-in server's made-up ones: the address has to be this PC's, at port 8788, where
// the stand-in listens. With QKVM_RIG_SIZE set (as in "1000x400") the window is that large.
//
// With QKVM_RIG_PICK set to machines' names with commas between them (or to "all"), those machines
// are picked on the wall once it has them, as "Select" and a click on each card would. With
// QKVM_RIG_MENU set to "picked" as well, the menu of the picked machines is opened a moment later, and
// QKVM_RIG_DO, QKVM_RIG_YES, QKVM_RIG_TEXT and QKVM_RIG_KEYS do with it what they do with one machine's.
// The yes to a question about several is given with its tick, and with QKVM_RIG_PACE as the seconds
// between one machine and the next (without it: a few at a time).
//
// With QKVM_RIG_EVERY set to a number of milliseconds, that is how often a new picture of each
// machine is asked for, as the View menu would set it once the wall is there; 0 is its "Paused".
//
// With QKVM_RIG_DISPLAY set, the wall is only shown once it is there, as the View menu's "Display
// mode" would have it; set to "rotate", its groups take turns as well. (An address that is a display
// link needs neither a password nor this: such a screen shows the wall and nothing else by itself.)
//
// With QKVM_RIG_CONSOLE set to a machine's name, the way into that machine's own console is asked for
// as a click on its card's door would. The rig opens no browser: it says where one would have been
// sent, without the pass that may be in the address.
//
// With QKVM_RIG_THEME set to "light" or "system" the rig looks so, as the bar's button would make it.
//
// Without a folder the rig keeps nothing. With one it does what the program does: the server is
// remembered in a settings file there, the sign-in asks to stay signed in, and the session is kept
// in Windows Credential Manager under the rig's own name ("QKVM Desktop rig"). Started again with
// the same folder it goes straight to the wall, and needs no password for that. Its window is then
// where it was when the rig was last closed, as the program's is; with QKVM_RIG_PLACE set to
// "x,y,width,height" it is put there two seconds after it came up, and with QKVM_RIG_WINDOW set to
// "maximized" or "normal" it is made that a second later, as somebody at the PC would.
//
// With QKVM_RIG_BOX set to the name of one of the window's boxes ("settingsBox", "aboutBox"), that
// box is opened three seconds after the window came up, as its button would open it.
//
// With QKVM_RIG_TRAY set, the rig has the program's icon in Windows' own notification area, goes
// on behind it when its window is closed, and tells through it what becomes of the wall, as the
// program does where that was chosen. Set to a number, "Quit" is said after so many seconds as the
// icon's menu would say it. With QKVM_RIG_TOLD set to a file, every message the icon is told to
// show is written there as well: a rig is looked at without a picture of anybody's screen.
//
// With QKVM_RIG_SINGLE set to a name, one rig of that name runs at a time, as one copy of the
// program does: started again, the rig brings the running one's window back, and ends.
//
// With QKVM_RIG_MEASURE set to "file,seconds to settle,seconds to measure", the rig measures what
// its window costs while it shows the wall (bench/wall_measure.h), writes that to the file, and
// ends: for a large wall (tools/stand_in_wall.py) against the targets of SPEC.md section 9.
//
// With QKVM_RIG_LIVE_MEASURE set to "file,seconds to settle,seconds to measure" as well as
// QKVM_RIG_ENLARGE, the rig measures how smooth that machine's live picture is once its first one
// is on the screen (bench/live_measure.h), writes that to the file, and ends: for a server with
// live pictures (tools/stand_in_live.py, with a film of a known pace).
//
// With QKVM_RIG_DECODING set to "automatic", "on" or "off", video is decoded so, as the settings'
// Video section would have it; with QKVM_RIG_CARD set to "most-powerful", "power-saving" or
// "windows", the rig draws and decodes on that graphics card. (Neither is remembered.)
//
// With QKVM_RIG_ARRANGE set to "name,place" (as in "NAS,1"), the wall's own order is made by hand
// once the wall has that machine, as "Arrange the tiles" in the View menu begins it: the machine is
// put at that place, counted from 1, and three seconds later the order is saved, unless
// QKVM_RIG_ARRANGE ends in ",look", which leaves the wall as it is while it is being arranged.
// That changes the server's wall for everybody, so the rig does it to the stand-in only.
//
// With QKVM_RIG_WINDOWS set to a number, the rig has that many windows: three seconds after the
// first came up it asks for the others one after the other, as "New window" in the first would.
// The rig's other switches are about the first window. (With a folder to remember in, the windows
// that were open when the rig was last closed are there again by themselves.)
//
// The wall as a file. No box of Windows' is opened by the rig: where a file is kept and which is
// opened are named by QKVM_RIG_SAVE and QKVM_RIG_OPEN; unless QKVM_RIG_DIALOGS is set, which has
// the rig ask Windows' own boxes as the program does, to see that they come up. With QKVM_RIG_EXPORT set, the export
// box is opened three seconds after the window came up and the wall is asked for as a file a moment later, with that
// passphrase, or as a plain file where it is "plain". With QKVM_RIG_IMPORT set (to the file's passphrase, or to
// "plain"), the import box is opened, the file is chosen and read; and with QKVM_RIG_TAKE set to parts with commas
// between them (as in "devices,actions"), those are taken in six seconds after the file was read. A file is made with
// the password of whoever is signed in, and taking one in changes the server's wall for everybody: both are done to a
// stand-in only.
//
// A stand-in is what listens on this PC at port 8788, where tests.demo does; or at the port
// With "own" in the place of the address, the rig shows this PC's own wall, with no server: KVMs
// that the rig speaks to itself, kept in the rig's own folder and in no secret store of Windows'.
// QKVM_RIG_ADD names KVMs to add to it, as the box that adds one would ("address" or
// "address;group", with commas between several), with the password in QKVM_RIG_KVM_PASSWORD: made-up
// ones on this PC's own address only (tools/stand_in_kvm.py). A real KVM is added only where its
// owner named it for this run: its host stands in QKVM_RIG_NAMED_BY_THE_OWNER (several with commas
// between), and what is not named there, and is not this PC's own address, is passed by.
// QKVM_RIG_STAND_IN names, for one that was started elsewhere. Never at 8787, where a QKVM that
// somebody uses listens unless it was told otherwise.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>

#include <qkvm/core/secrets.h>
#include <qkvm/core/settings.h>
#include <qkvm/engine/gpu.h>

#include "about_model.h"
#include "actions_view.h"
#include "add_kvm_view.h"
#include "application.h"
#include "edit_kvm_view.h"
#include "files_view.h"
#include "live_measure.h"
#include "look.h"
#include "own_kvms_view.h"
#include "own_wall.h"
#include "pretend_start.h"
#include "pretend_tray.h"
#include "server_session.h"
#include "session_view.h"
#include "settings_view.h"
#include "tray.h"
#include "tray_keeper.h"
#include "wall_measure.h"
#include "wall_pace.h"
#include "wall_teller.h"
#include "wall_view.h"
#include "wall_windows.h"
#include "win/file_dialogs.h"
#include "win/keyboard_hook.h"
#include "win/single_instance.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// Secrets that are kept for one run of the rig and no longer: the passwords of the made-up KVMs
// of the rig's own wall.
class KeptForTheRun final : public core::SecretStore {
public:
    base::Result<void> keep(const QString& name, const QByteArray& secret) override
    {
        m_all.insert(name, secret);
        return {};
    }
    QByteArray kept(const QString& name) override { return m_all.value(name); }
    void forget(const QString& name) override { m_all.remove(name); }

private:
    QHash<QString, QByteArray> m_all;
};

// The rig keeps nothing: not in the Credential Manager of whoever runs it, and not in their settings.
class NoSecrets final : public core::SecretStore {
public:
    base::Result<void> keep(const QString& /*name*/, const QByteArray& /*secret*/) override { return {}; }
    QByteArray kept(const QString& /*name*/) override { return {}; }
    void forget(const QString& /*name*/) override {}
};

// Windows' own icon, with every message it is told to show written to a file as well: what the
// rig tells Windows is looked at there, and not in a picture of somebody's screen.
class NotedTray final : public ui::TrayIcon {
public:
    NotedTray(std::unique_ptr<ui::TrayIcon> real, QString file) : m_real(std::move(real)), m_file(std::move(file))
    {
        connect(m_real.get(), &ui::TrayIcon::opened, this, &ui::TrayIcon::opened);
        connect(m_real.get(), &ui::TrayIcon::chosen, this, &ui::TrayIcon::chosen);
    }

    void show(ui::MarkDot dot, const QString& says) override { m_real->show(dot, says); }
    void hide() override { m_real->hide(); }
    bool isThere() const override { return m_real->isThere(); }
    void setMenu(const QList<Row>& rows) override { m_real->setMenu(rows); }

private:
    void showMessage(const QString& title, const QString& text, bool isWarning) override
    {
        if (QFile noted(m_file); noted.open(QIODevice::Append)) {
            noted.write((isWarning ? "warning: " : "message: ") + title.toUtf8() + " | " + text.toUtf8() + "\n");
        }
        m_real->tell(title, text, isWarning);
        // The rig sends untargeted notices; the real icon owns their display queue.
        notificationFinished(false);
    }

    std::unique_ptr<ui::TrayIcon> m_real;
    QString m_file;
};

int run(int argc, char** argv)
{
    // Where the time from the start to the window goes, for the measure (QKVM_RIG_MEASURE).
    std::vector<bench::WallMeasure::Moment> moments;
    const auto reached = [&moments](const char* what) {
        moments.push_back(bench::WallMeasure::now(QString::fromLatin1(what)));
    };
    reached("the program's own code began");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    ui::prepareApplication();
    reached("Qt was ready");
    // Where one copy at a time is to be looked at: under a name of the rig's own, never the program's.
    std::unique_ptr<ui::SingleInstance> only;
    if (const QString name = qEnvironmentVariable("QKVM_RIG_SINGLE"); !name.isEmpty()) {
        only = std::make_unique<ui::SingleInstance>(u"QKVM Desktop rig "_s + name);
        if (!only->isFirst()) {
            return 0;
        }
    }
    if (argc < 2) {
        std::fputs("qkvm-session-rig <address of the server> [seconds] [folder to remember in]\n", stderr);
        return 2;
    }
    const QString address = QString::fromLocal8Bit(argv[1]);
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 0;
    const QString rememberIn = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    const QString name = qEnvironmentVariable("QKVM_RIG_NAME");
    const QString password = qEnvironmentVariable("QKVM_RIG_PASSWORD");

    ui::AboutModel about;
    // The rig's own wall, which no window shows, as the program has one.
    ui::WallView all;
    ui::SessionView session;

    // The rig looks as the program does when nothing was chosen, and remembers no other choice.
    const QTemporaryDir lookFolder;
    core::Settings lookSettings(lookFolder.filePath(u"settings.json"_s));
    ui::Look look(lookSettings);
    for (const QString wanted = qEnvironmentVariable("QKVM_RIG_THEME");
         (wanted == "light"_L1 || wanted == "system"_L1) && look.theme() != wanted;) {
        look.nextTheme();
    }
    // Whether it starts with Windows is only pretended: the rig is never on Windows' list.
    ui::PretendedStart start;
    ui::SettingsView ownSettings(start, lookSettings);
    ui::AddKvmView adding;
    QQmlApplicationEngine engine;
    ui::keepOffTheNetwork(engine);
    QQmlComponent screens(&engine);
    screens.loadFromModule("Qkvm", "Main");
    reached("the screens were read");
    const QTemporaryDir folder;
    const bool remembers = !rememberIn.isEmpty();
    core::Settings settings(QDir(remembers ? rememberIn : folder.path()).filePath(u"settings.json"_s));
    ui::WallPace paceOfAll(settings);
    paceOfAll.add(all);
    NoSecrets none;
    const std::unique_ptr<core::SecretStore> ofWindows = core::windowsSecrets(u"QKVM Desktop rig"_s);
    core::SecretStore& secrets = remembers ? *ofWindows : static_cast<core::SecretStore&>(none);

    // The program's icon in Windows' own notification area, where that is to be looked at; and
    // then the rig goes on behind it when its last window is closed, as the program does where
    // that was chosen. Otherwise an icon that is only pretended, and closing closes.
    std::unique_ptr<ui::TrayIcon> tray = std::make_unique<ui::PretendedTray>();
    std::unique_ptr<ui::WallTeller> teller;
    const bool hasTray = qEnvironmentVariableIsSet("QKVM_RIG_TRAY");
    if (hasTray) {
        core::Remembered stays = settings.remembered();
        stays.staysInTray = true;
        static_cast<void>(settings.remember(stays));
        tray = ui::windowsTray();
        if (const QString file = qEnvironmentVariable("QKVM_RIG_TOLD"); !file.isEmpty()) {
            tray = std::make_unique<NotedTray>(std::move(tray), file);
        }
    }
    ui::TrayKeeper presence(*tray, session, all, settings);
    QObject::connect(&presence, &ui::TrayKeeper::quitAsked, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    if (only != nullptr) {
        QObject::connect(only.get(), &ui::SingleInstance::askedToShow, &presence, &ui::TrayKeeper::bringAllBack);
    }
    if (hasTray) {
        teller = std::make_unique<ui::WallTeller>(*tray, session, all, settings);
        // With a number: after so many seconds, as if "Quit" had been chosen in the icon's menu.
        if (const int after = qEnvironmentVariable("QKVM_RIG_TRAY").toInt(); after > 0) {
            QTimer::singleShot(after * 1000, tray.get(),
                               [icon = tray.get()] { emit icon->chosen(ui::TrayKeeper::Quit); });
        }
    }

    // This PC's own wall, where the rig is to show that: what was added to it is kept for the run.
    const bool showsOwn = address == "own"_L1;
    KeptForTheRun keptForTheRun;
    ui::OwnWall own(settings, showsOwn ? static_cast<core::SecretStore&>(keptForTheRun) : secrets);
    // (The rig's own view: what QKVM_RIG_ADD names is added through it, as a box would.)
    own.serve(adding);
    // Its KVMs as the settings list them.
    ui::OwnKvmsView kvms;
    kvms.use(&own.wall());
    ui::ServerSession server(session, all, settings, secrets);
    if (showsOwn) {
        server.useOwnWall(own.wall());
    }
    // The windows as the program has them: each from the screens, with a graphics device of its own.
    ui::WallWindows::Makers makers;
    // This diagnostic rig drives one shared session through its command-line switches.
    makers.independentSources = false;
    makers.adds = [&own](ui::AddKvmView& box) { own.serve(box); };
    makers.edits = [&own](ui::EditKvmView& box) { own.serve(box); };
    makers.ownFiles = [&own](ui::OwnFilesView& boxes) { own.serve(boxes); };
    makers.window = [&](ui::SessionView& windowSession, ui::WorkspacesView& workspaces, ui::WallView& shown,
                        ui::ActionsView& from, ui::FilesView& files, ui::OwnFilesView& ownFiles, ui::AddKvmView& box,
                        ui::EditKvmView& editing, ui::ControlView& control) {
        std::unique_ptr<QObject> made(
            screens.createWithInitialProperties({{u"about"_s, QVariant::fromValue(&about)},
                                                 {u"session"_s, QVariant::fromValue(&windowSession)},
                                                 {u"workspaces"_s, QVariant::fromValue(&workspaces)},
                                                 {u"wall"_s, QVariant::fromValue(&shown)},
                                                 {u"actions"_s, QVariant::fromValue(&from)},
                                                 {u"look"_s, QVariant::fromValue(&look)},
                                                 {u"settings"_s, QVariant::fromValue(&ownSettings)},
                                                 {u"files"_s, QVariant::fromValue(&files)},
                                                 {u"ownFiles"_s, QVariant::fromValue(&ownFiles)},
                                                 {u"adding"_s, QVariant::fromValue(&box)},
                                                 {u"editing"_s, QVariant::fromValue(&editing)},
                                                 {u"control"_s, QVariant::fromValue(&control)},
                                                 {u"kvms"_s, QVariant::fromValue(&kvms)}}));
        reached("the screens were made");
        std::unique_ptr<QQuickWindow> window;
        if (qobject_cast<QQuickWindow*>(made.get()) != nullptr) {
            window.reset(qobject_cast<QQuickWindow*>(made.release()));
        }
        return window;
    };
    // The wall as a file. Where a file is saved and which is opened are named when the rig is
    // started, or are nowhere.
    makers.door = [](QWindow* over) {
        if (qEnvironmentVariableIsSet("QKVM_RIG_DIALOGS")) {
            return ui::windowsFileDoor(over);
        }
        return ui::FileDoor{[](const QString&) { return base::Result<QString>(qEnvironmentVariable("QKVM_RIG_SAVE")); },
                            [] { return base::Result<QString>(qEnvironmentVariable("QKVM_RIG_OPEN")); }};
    };
    const QString card = qEnvironmentVariable("QKVM_RIG_CARD");
    engine::Gpu::Options onTheCard;
    onTheCard.choice = card == "power-saving"_L1 ? engine::AdapterChoice::PowerSaving
                       : card == "windows"_L1    ? engine::AdapterChoice::AsWindowsChooses
                                                 : engine::AdapterChoice::MostPowerful;
    makers.gpu = [&] {
        base::Result<std::unique_ptr<engine::Gpu>> made = engine::Gpu::create(onTheCard);
        if (!made) {
            std::fprintf(stderr, "%s\n", made.error().message.c_str());
            return std::unique_ptr<engine::Gpu>();
        }
        reached("the engine had its graphics device");
        return std::move(made).value();
    };
    // Keyboard and mouse of a machine, as the program has them: every key of this PC's keyboard,
    // from when somebody at the rig takes them until they are given back. No switch of the rig
    // takes them.
    makers.keys = [](QWindow& window) { return ui::windowsKeyboard(window); };
    ui::WallWindows windows(std::move(makers), all, settings, paceOfAll, presence, server);
    if (const QString decoding = qEnvironmentVariable("QKVM_RIG_DECODING"); !decoding.isEmpty()) {
        windows.setDecoding(decoding == "on"_L1    ? engine::DecodingChoice::GraphicsCard
                            : decoding == "off"_L1 ? engine::DecodingChoice::Software
                                                   : engine::DecodingChoice::Automatic);
    }
    if (!windows.openAll()) {
        std::fputs("The window could not be made.\n", stderr);
        return 1;
    }
    // What the rig's switches are about: the first window, its wall, and what is done from it.
    QQuickWindow* window = windows.window(0);
    ui::WallView& wall = *windows.wall(0);
    ui::ActionsView& actions = *windows.actions(0);
    ui::FilesView& files = *windows.files(0);
    if (const QStringList view = qEnvironmentVariable("QKVM_RIG_VIEW").split(u','); view.size() >= 2) {
        wall.shown()->setOrder(view.at(0));
        wall.setColumns(view.at(1).toInt());
        wall.shown()->setGroup(view.value(2));
    }
    if (const int wanted = qEnvironmentVariable("QKVM_RIG_WINDOWS").toInt(); wanted > 1) {
        QTimer::singleShot(3000, &app, [&windows, wanted] {
            for (qsizetype more = windows.count(); more < wanted; ++more) {
                QMetaObject::invokeMethod(windows.window(0), "windowAsked");
            }
        });
    }

    // What a person would do at the screen, once each: name the server, then sign in. Whatever goes
    // wrong is said and ends the rig: it is not tried again.
    bool signInSent = false;
    QObject::connect(&session, &ui::SessionView::changed, &app, [&] {
        if (!session.problem().isEmpty()) {
            std::fprintf(stderr, "%s\n", qPrintable(session.problem()));
            QCoreApplication::exit(1);
        } else if (session.stage() == u"signIn"_s && !session.busy() && !signInSent) {
            signInSent = true;
            session.signIn(name, password, QString(), remembers);
        }
    });
    // Doing things: only to the stand-in's machines.
    const base::Result<QUrl> typed = core::ServerClient::addressFrom(address);
    const QUrl where = typed ? typed.value() : QUrl();
    const int standInAt = qEnvironmentVariable("QKVM_RIG_STAND_IN").toInt();
    const bool isTheStandIn = (where.host() == "127.0.0.1"_L1 || where.host() == "localhost"_L1) &&
                              (where.port() == 8788 || (where.port() == standInAt && standInAt != 8787));
    const QString menuOf = qEnvironmentVariable("QKVM_RIG_MENU");
    const QString rowToChoose = isTheStandIn ? qEnvironmentVariable("QKVM_RIG_DO") : QString();
    const bool saysYes = qEnvironmentVariableIsSet("QKVM_RIG_YES");
    bool menuOpened = false;
    bool rowChosen = false;
    QObject::connect(wall.machines(), &QAbstractItemModel::modelReset, &app, [&] {
        for (int row = 0; row < wall.machines()->rowCount() && !menuOf.isEmpty() && !menuOpened; ++row) {
            const QModelIndex machine = wall.machines()->index(row, 0);
            if (machine.data(ui::WallModel::NameRole).toString() == menuOf) {
                menuOpened = true;
                actions.open(machine.data(ui::WallModel::KeyRole).toString());
            }
        }
    });
    QObject::connect(&actions, &ui::ActionsView::menuChanged, &app, [&] {
        if (!actions.menu().isEmpty() && !rowToChoose.isEmpty() && !rowChosen) {
            rowChosen = true;
            QTimer::singleShot(1500, &app, [&] { actions.choose(rowToChoose); });
        }
    });
    // No browser is opened by the rig. Where one would go is said, and of the address only the place:
    // a pass into a console is in the rest of it.
    server.openPagesWith([](const QUrl& address) {
        std::fprintf(stderr, "a browser would be sent to %s\n",
                     qPrintable(address.toString(QUrl::RemovePath | QUrl::RemoveQuery)));
        return true;
    });
    const QString consoleOf = isTheStandIn ? qEnvironmentVariable("QKVM_RIG_CONSOLE") : QString();
    bool consoleAsked = false;
    QObject::connect(wall.machines(), &QAbstractItemModel::modelReset, &app, [&] {
        for (int row = 0; row < wall.machines()->rowCount() && !consoleOf.isEmpty() && !consoleAsked; ++row) {
            const QModelIndex machine = wall.machines()->index(row, 0);
            if (machine.data(ui::WallModel::NameRole).toString() == consoleOf) {
                consoleAsked = true;
                const QString key = machine.data(ui::WallModel::KeyRole).toString();
                // (Once the server has said how the machine is: whether it hands out a pass.)
                QTimer::singleShot(2500, &app, [&actions, key] { actions.openConsole(key); });
            }
        }
    });
    const QString textToType = isTheStandIn ? qEnvironmentVariable("QKVM_RIG_TEXT") : QString();
    const QString keysToSend = isTheStandIn ? qEnvironmentVariable("QKVM_RIG_KEYS") : QString();
    QObject::connect(&actions, &ui::ActionsView::formChanged, &app, [&] {
        const QString kind = actions.form().value(u"kind"_s).toString();
        if (kind == "text"_L1 && !textToType.isEmpty()) {
            QTimer::singleShot(800, &app, [&] { actions.typeText(textToType, true); });
        } else if (kind == "keys"_L1 && !keysToSend.isEmpty()) {
            QTimer::singleShot(800, &app, [&] { actions.sendKeys(keysToSend); });
        }
    });
    bool paceGiven = false;
    const int paceAsked = qEnvironmentVariable("QKVM_RIG_PACE").toInt(&paceGiven);
    const int pace = paceGiven ? paceAsked : -1;
    QObject::connect(&actions, &ui::ActionsView::questionChanged, &app, [&] {
        if (!actions.question().isEmpty() && saysYes) {
            // (About one machine there is neither a pace nor a tick, and neither is looked at.)
            QTimer::singleShot(1000, &app, [&actions, pace] { actions.answerWith(true, pace, true); });
        }
    });
    const QStringList toPick = qEnvironmentVariable("QKVM_RIG_PICK").split(u',', Qt::SkipEmptyParts);
    bool picked = false;
    QObject::connect(wall.machines(), &QAbstractItemModel::modelReset, &app, [&] {
        if (toPick.isEmpty() || picked || wall.machines()->rowCount() == 0) {
            return;
        }
        picked = true;
        wall.setSelecting(true);
        for (int row = 0; row < wall.machines()->rowCount(); ++row) {
            const QModelIndex machine = wall.machines()->index(row, 0);
            if (toPick.contains(u"all"_s) || toPick.contains(machine.data(ui::WallModel::NameRole).toString())) {
                wall.togglePicked(machine.data(ui::WallModel::KeyRole).toString());
            }
        }
        if (menuOf == "picked"_L1) {
            // (Once the server has said how the machines are: their menu is of what they can do.)
            QTimer::singleShot(2500, &app, [&actions] { actions.openPicked(); });
        }
    });
    if (const QStringList size = qEnvironmentVariable("QKVM_RIG_SIZE").split(u'x'); size.size() == 2) {
        window->resize(size.at(0).toInt(), size.at(1).toInt());
    }
    bool everyGiven = false;
    const int every = qEnvironmentVariable("QKVM_RIG_EVERY").toInt(&everyGiven);
    bool paceSet = false;
    QObject::connect(wall.machines(), &QAbstractItemModel::modelReset, &app, [&] {
        // (Once the server has named its own pace: the choices are known from then on.)
        if (everyGiven && !paceSet && wall.machines()->rowCount() > 0) {
            paceSet = true;
            wall.setPace(every);
        }
    });
    const QString display = qEnvironmentVariable("QKVM_RIG_DISPLAY");
    bool displaySet = false;
    QObject::connect(wall.machines(), &QAbstractItemModel::modelReset, &app, [&] {
        if (!display.isEmpty() && !displaySet && wall.machines()->rowCount() > 0) {
            displaySet = true;
            wall.setRotating(display == "rotate"_L1);
            wall.setDisplaying(true);
        }
    });
    const QStringList toArrange =
        isTheStandIn ? qEnvironmentVariable("QKVM_RIG_ARRANGE").split(u',', Qt::SkipEmptyParts) : QStringList();
    bool arrangeBegun = false;
    QObject::connect(wall.machines(), &QAbstractItemModel::modelReset, &app, [&] {
        for (int row = 0; row < wall.machines()->rowCount() && toArrange.size() >= 2 && !arrangeBegun; ++row) {
            const QModelIndex machine = wall.machines()->index(row, 0);
            if (machine.data(ui::WallModel::NameRole).toString() == toArrange.at(0)) {
                arrangeBegun = true;
                const QString key = machine.data(ui::WallModel::KeyRole).toString();
                QTimer::singleShot(1500, &app, [&wall, &toArrange, key] {
                    wall.arrange();
                    wall.placeAt(key, toArrange.at(1).toInt() - 1);
                });
                if (toArrange.value(2) != "look"_L1) {
                    QTimer::singleShot(4500, &app, [&wall] { wall.saveOrder(); });
                }
            }
        }
    });
    const QString enlarge = qEnvironmentVariable("QKVM_RIG_ENLARGE");
    QObject::connect(wall.machines(), &QAbstractItemModel::modelReset, &app, [&] {
        for (int row = 0; row < wall.machines()->rowCount() && !enlarge.isEmpty(); ++row) {
            const QModelIndex machine = wall.machines()->index(row, 0);
            if (machine.data(ui::WallModel::NameRole).toString() == enlarge) {
                wall.enlarge(machine.data(ui::WallModel::KeyRole).toString());
            }
        }
    });
    if (showsOwn) {
        session.goToOwnWall();
        // KVMs that are added as the box would add them: one after the other, each when the last
        // has been told of. Only what is on this PC's own address: a made-up KVM.
        auto toAdd =
            std::make_shared<QStringList>(qEnvironmentVariable("QKVM_RIG_ADD").split(u',', Qt::SkipEmptyParts));
        const auto next = [&adding, toAdd] {
            while (!toAdd->isEmpty() && !adding.busy()) {
                const QStringList parts = toAdd->takeFirst().split(u';');
                // (Read as the box reads an address, and then only this PC's own, or a KVM its owner
                // named for this run: a password goes nowhere else.)
                const base::Result<QUrl> where = core::kvm::addressFrom(parts.at(0));
                const QStringList named =
                    qEnvironmentVariable("QKVM_RIG_NAMED_BY_THE_OWNER").split(u',', Qt::SkipEmptyParts);
                if (!where || (where.value().host() != "127.0.0.1"_L1 && !named.contains(where.value().host()))) {
                    std::fputs("the rig adds only made-up KVMs on this PC's own address, and what the owner named\n",
                               stderr);
                    continue;
                }
                adding.add(parts.at(0), QString(), qEnvironmentVariable("QKVM_RIG_KVM_PASSWORD"), parts.value(1));
            }
        };
        QObject::connect(&adding, &ui::AddKvmView::changed, &app, [&adding, next] {
            if (!adding.busy()) {
                std::fprintf(stderr, "added: %s%s\n", qPrintable(adding.added()), qPrintable(adding.problem()));
                next();
            }
        });
        QTimer::singleShot(1000, &app, next);
    } else if (remembers && !settings.remembered().server.isEmpty()) {
        server.begin();
    } else {
        session.connectTo(address);
    }

    if (const QStringList place = qEnvironmentVariable("QKVM_RIG_PLACE").split(u','); place.size() == 4) {
        QTimer::singleShot(2000, &app, [window, place] {
            window->setGeometry(place.at(0).toInt(), place.at(1).toInt(), place.at(2).toInt(), place.at(3).toInt());
        });
    }
    if (const QString how = qEnvironmentVariable("QKVM_RIG_WINDOW"); !how.isEmpty()) {
        QTimer::singleShot(3000, &app, [window, how] {
            if (how == "maximized"_L1) {
                window->showMaximized();
            } else {
                window->showNormal();
            }
        });
    }
    // What the window costs while it shows the wall, measured and written down; and then the end.
    std::unique_ptr<bench::WallMeasure> measure;
    if (const QStringList how = qEnvironmentVariable("QKVM_RIG_MEASURE").split(u','); how.size() == 3) {
        reached("the window was shown");
        measure = std::make_unique<bench::WallMeasure>(
            *window, wall, how.at(0), how.at(1).toInt(), how.at(2).toInt(), [] { QCoreApplication::quit(); }, moments);
    }
    std::unique_ptr<bench::LiveMeasure> liveMeasure;
    if (const QStringList how = qEnvironmentVariable("QKVM_RIG_LIVE_MEASURE").split(u','); how.size() == 3) {
        liveMeasure = std::make_unique<bench::LiveMeasure>(*window, wall, how.at(0), how.at(1).toInt(),
                                                           how.at(2).toInt(), [] { QCoreApplication::quit(); });
    }
    // A box of the window's own, opened as its button would open it: "settingsBox", "aboutBox".
    if (const QString box = qEnvironmentVariable("QKVM_RIG_BOX"); !box.isEmpty()) {
        QTimer::singleShot(3000, &app, [window, box] {
            if (QObject* found = window->findChild<QObject*>(box)) {
                QMetaObject::invokeMethod(found, "open");
            }
        });
    }
    // The wall as a file, as a person at the two boxes would have it.
    const QString exportWith = isTheStandIn ? qEnvironmentVariable("QKVM_RIG_EXPORT") : QString();
    if (!exportWith.isEmpty()) {
        QTimer::singleShot(3000, &app, [&, window] {
            if (QObject* box = window->findChild<QObject*>(u"exportBox"_s)) {
                QMetaObject::invokeMethod(box, "open");
            }
            QTimer::singleShot(2500, &app, [&] {
                files.exportWall(core::wallFileParts(), exportWith == "plain"_L1 ? QString() : exportWith, password);
            });
        });
    }
    const QString importWith = isTheStandIn ? qEnvironmentVariable("QKVM_RIG_IMPORT") : QString();
    const QString fileOpensWith = importWith == "plain"_L1 ? QString() : importWith;
    const QStringList toTake = qEnvironmentVariable("QKVM_RIG_TAKE").split(u',', Qt::SkipEmptyParts);
    bool takingAsked = false;
    if (!importWith.isEmpty()) {
        QTimer::singleShot(3000, &app, [&, window] {
            if (QObject* box = window->findChild<QObject*>(u"importBox"_s)) {
                QMetaObject::invokeMethod(box, "open");
            }
            QTimer::singleShot(1500, &app, [&] {
                files.chooseFile();
                files.readFile(fileOpensWith);
            });
        });
        QObject::connect(&files, &ui::FilesView::changed, &app, [&] {
            if (files.wasRead() && !toTake.isEmpty() && !takingAsked) {
                takingAsked = true;
                QTimer::singleShot(6000, &app, [&] { files.takeIn(toTake, fileOpensWith); });
            }
        });
    }
    if (seconds > 0) {
        QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    }
    return QGuiApplication::exec();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "qkvm-session-rig ended: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("qkvm-session-rig ended on something that was thrown.\n", stderr);
        return 1;
    }
}
