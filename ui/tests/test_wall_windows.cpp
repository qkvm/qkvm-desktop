#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QJsonObject>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/exchange.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/setup_overrides.h>
#include <qkvm/engine/gpu.h>
#include <qkvm/engine/types.h>

#include "alerts_view.h"
#include "files_view.h"
#include "keys_by_hand.h"
#include "made_up_kvm.h"
#include "media_view.h"
#include "memory_secrets.h"
#include "own_files_view.h"
#include "own_wall.h"

#include "pretend_tray.h"
#include "server_session.h"
#include "session_view.h"
#include "stand_in.h"
#include "tray_keeper.h"
#include "wall_pace.h"
#include "wall_view.h"
#include "wall_windows.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::MadeUpKvm;
using core::testing::StandIn;

namespace {

// Two monitors side by side, each by the part of it that is free of the taskbar.
QList<QRect> twoScreens()
{
    return {QRect(0, 0, 1920, 1040), QRect(1920, 0, 1280, 984)};
}

core::WindowPlace at(int x, int y, int width, int height)
{
    core::WindowPlace place;
    place.x = x;
    place.y = y;
    place.width = width;
    place.height = height;
    return place;
}

// A window as the settings hold one, said in one line: where it was, or nowhere yet, and of its
// wall the group, the order, the columns and whether the groups take turns.
core::WallWindow windowAt(const std::optional<core::WindowPlace>& place, const QString& group = {},
                          const QString& order = {}, int columns = 0, bool rotates = false)
{
    core::WallWindow window;
    window.place = place;
    window.group = group;
    window.order = order;
    window.columns = columns;
    window.rotates = rotates;
    return window;
}

// One of which only the columns are other than a new window's.
core::WallWindow windowIn(int columns)
{
    return windowAt(std::nullopt, {}, {}, columns);
}

// A window that can ask for another, as the program's screens can.
class AskingWindow : public QQuickWindow {
    Q_OBJECT

signals:
    void windowAsked();
};

// What the keeper of the windows is given, for one test: settings in a file of the test's, an icon
// that is only pretended, a session that speaks to nobody until it is told an address, and windows
// without screens in them.
struct Desk {
    explicit Desk(const QString& file, bool independent = false)
        : m_file(file), independentSources(independent), settings(file), keeper(tray, view, all, settings),
          pace(settings), session(view, all, settings, secrets, 100),
          windows(makers(), all, settings, pace, keeper, session)
    {
        pace.add(all);
    }

    ui::WallWindows::Makers makers()
    {
        ui::WallWindows::Makers how;
        how.independentSources = independentSources;
        how.adds = [this](ui::AddKvmView& box) {
            served.append(&box);
            if (own != nullptr) {
                own->serve(box);
            }
        };
        how.edits = [this](ui::EditKvmView& box) {
            servedToChange.append(&box);
            if (own != nullptr) {
                own->serve(box);
            }
        };
        how.ownFiles = [this](ui::OwnFilesView& boxes) {
            servedWithFiles.append(&boxes);
            if (own != nullptr) {
                own->serve(boxes);
            }
        };
        how.window = [this](ui::SessionView&, ui::WorkspacesView&, ui::WallView&, ui::ActionsView&, ui::FilesView&,
                            ui::OwnFilesView&, ui::AddKvmView&, ui::EditKvmView&, ui::ControlView&) {
            std::unique_ptr<AskingWindow> window;
            if (makes) {
                window = std::make_unique<AskingWindow>();
                // (Of a size a window can have: where it is, is written down with it.)
                window->resize(640, 400);
            }
            return window;
        };
        // Windows' software adapter, in a test that asks for a graphics device.
        how.gpu = [this]() -> std::unique_ptr<engine::Gpu> {
            if (!makesGpus) {
                return nullptr;
            }
            base::Result<std::unique_ptr<engine::Gpu>> made = engine::Gpu::create({.software = true});
            return made ? std::move(made).value() : nullptr;
        };
        // Where a file would be chosen from a window: over which window it was asked is noted.
        how.door = [this](QWindow* over) {
            return ui::FileDoor{[this, over](const QString&) {
                                    askedOver.append(over);
                                    if (whileAsked) {
                                        whileAsked(over);
                                    }
                                    return base::Result<QString>(savesTo);
                                },
                                [this] { return base::Result<QString>(opens); }};
        };
        how.screens = twoScreens;
        how.restMs = 60;
        // The display is never kept awake by a test: what would have been asked is noted.
        how.keepAwake = [this](bool awake) { keptAwake.append(awake); };
        // No key of this PC is listened for by a test: each window's keys are pressed by hand.
        how.keys = [this](QWindow& window) -> std::unique_ptr<ui::KeySource> {
            auto keys = std::make_unique<ui::KeysByHand>();
            keySources.append(QPointer<ui::KeysByHand>(keys.get()));
            keyWindows.append(&window);
            return keys;
        };
        return how;
    }

    // What the file holds of the windows just now.
    QList<core::WallWindow> kept() const { return core::Settings(file()).remembered().windows; }
    QString file() const { return m_file; }

    // As somebody would ask in this window for another.
    void askIn(qsizetype index) { QMetaObject::invokeMethod(windows.window(index), "windowAsked"); }
    // A machine's key on the wall, by the server's id for it.
    static QString key(const QString& machine)
    {
        return ui::wallKey(ui::ServerSession::serverId(), machine).value_or(QString());
    }

    QString m_file;
    bool independentSources;
    core::Settings settings;
    core::testing::MemorySecrets secrets;
    ui::SessionView view;
    ui::WallView all;
    ui::PretendedTray tray;
    ui::TrayKeeper keeper;
    ui::WallPace pace;
    ui::ServerSession session;
    // Whether a window can be made at all.
    bool makes = true;
    bool makesGpus = false;
    // Each time the display was to be kept awake, or let go of.
    QList<bool> keptAwake;
    // The boxes that add a KVM, as whoever adds KVMs was handed them: one for each window.
    QList<ui::AddKvmView*> served;
    // And the boxes that change one, as whoever changes KVMs was handed them.
    QList<ui::EditKvmView*> servedToChange;
    // And those for this PC's wall as a file, as whoever makes and reads the files was handed them.
    QList<ui::OwnFilesView*> servedWithFiles;
    // The windows over which somebody was asked where a file is to be kept, and what they said.
    QList<QWindow*> askedOver;
    QString savesTo;
    // And the file somebody chooses to be opened; none: nobody chooses one.
    QString opens;
    // What happens behind the box while somebody is asked, as it can behind one of Windows'.
    std::function<void(QWindow*)> whileAsked;
    // This PC's own wall as the program holds it, where a test makes one before it opens its
    // windows: the boxes of each window are then served by it, as the program's are.
    std::unique_ptr<ui::OwnWall> own;
    // Where each window's keys come from, in the order the windows were opened in; nothing where
    // a window went, and its keys with it.
    QList<QPointer<ui::KeysByHand>> keySources;
    // And the window each was made for.
    QList<QPointer<QWindow>> keyWindows;
    ui::WallWindows windows;
};

// A QKVM where somebody is signed in already, with two machines.
Answered signedIn(const Asked& asked, const QByteArray& status)
{
    if (asked.path == "/healthz") {
        return {200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"], "page": "3f2a9c", "locked": false})"};
    }
    if (asked.path == "/api/v1/session") {
        return {200, R"({"initialized": true, "authenticated": true, "ask_name": false,
                         "user": {"id": "u1", "name": "anna", "role": "admin", "token": false}})"};
    }
    if (asked.path == "/api/v1/wall") {
        return {200, R"({"interval": 0.25, "devices": [
            {"id": "a", "name": "Build server", "host": "192.168.1.21", "kind": "kvmd", "picture": true},
            {"id": "b", "name": "NAS", "host": "192.168.1.22", "kind": "kvmd", "picture": true}]})"};
    }
    if (asked.path == "/api/v1/status") {
        return {200, status};
    }
    return {200, core::testing::aPicture(), "image/jpeg"};
}

// A QKVM where somebody is signed in already, with one rack server that can start into its BIOS setup; an action
// sent to it is answered 2.5 s later, and counted.
Answered withARackServer(const Asked& asked, int& acted)
{
    if (asked.path == "/api/v1/wall") {
        return {200, R"({"interval": 60, "devices": [{"id": "srv", "name": "Rack server", "host": "192.168.1.60",
                         "kind": "redfish", "open_url": "https://192.168.1.60/", "picture": false}]})"};
    }
    if (asked.path == "/api/v1/status") {
        return {200, R"({"srv": {"state": "online", "picture": false, "console": true,
                         "atx": {"enabled": true, "power": true}, "server": {"health": "OK", "locator": false},
                         "can": ["power", "reset", "power_state", "locator", "bios_setup"]}})"};
    }
    if (asked.path == "/api/v1/actions") {
        return {200, R"({"actions": []})"};
    }
    if (asked.method == "POST" && asked.path.endsWith("/actions")) {
        ++acted;
        Answered later{200, R"({"ok": true, "note": null})"};
        later.afterMs = 2500;
        return later;
    }
    return signedIn(asked, "{}");
}

// A QKVM's virtual media of one KVM (app/qkvm/server.py, media_*): one image, and a mount answered 2 s later.
Answered mediaOfAKvm(const Asked& asked)
{
    if (asked.path == "/healthz") {
        return {200, R"({"status": "ok", "version": "0.3.0", "api": ["v1"]})"};
    }
    if (asked.method == "GET" && asked.path == "/api/v1/devices/kvm/media/upload") {
        return {200, R"({"uploading": null})"};
    }
    if (asked.method == "GET" && asked.path == "/api/v1/devices/kvm/media") {
        return {200, R"({"images": [{"id": "debian-13.iso", "name": "debian-13.iso", "where": null, "size": 700000000,
                         "partial": false, "sha256": null}], "unseen": [], "left_out": 0, "notes": [],
                         "space": {"free": 9000000000, "size": 32000000000}, "mounted": null,
                         "offers": {"drives": ["cd", "flash"], "boot": [], "restart": true, "upload": true,
                         "where": "device", "writable": ["flash"], "delete": true, "drops_input": false},
                         "uploading": null})"};
    }
    if (asked.method == "POST" && asked.path.startsWith("/api/v1/devices/kvm/media")) {
        Answered later{200, R"({"ok": true, "note": null, "mounted": null})"};
        later.afterMs = 2000;
        return later;
    }
    return {404, R"({"code": "not_found", "message": "Unknown"})"};
}

} // namespace

// The program's windows: as many as were open, each as it was; one more when asked; and what
// becomes of one that is closed.
class TestWallWindows : public QObject {
    Q_OBJECT

private:
    // A settings file in a folder of the test's own, with these windows remembered in it.
    static QString fileWith(const QTemporaryDir& folder, const QList<core::WallWindow>& windows)
    {
        const QString file = folder.filePath(u"settings.json"_s);
        if (!windows.isEmpty()) {
            core::Settings settings(file);
            core::Remembered remembered;
            remembered.windows = windows;
            static_cast<void>(settings.remember(remembered));
        }
        return file;
    }

    // The times a real KVM is given, made short: for this PC's own wall, where a test has one.
    static core::KvmAdding::Pace quick()
    {
        core::KvmAdding::Pace pace;
        pace.probe.connectWithinMs = 1500;
        pace.probe.quietMs = 1500;
        pace.probe.withinMs = 3000;
        pace.driver.connectWithinMs = 1500;
        pace.driver.quietMs = 1500;
        pace.driver.transferMs = 1500;
        pace.driver.holdMs = 400;
        pace.driver.freshMs = 200;
        return pace;
    }

    static core::testing::Tls tls(const char* name)
    {
        const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
        return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
    }
    static QByteArray sha256Of(const char* name)
    {
        return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
    }

    // A KVM that answers the sign-in only after this long; and one that says what it is only
    // after this long (its certificate is read by asking that).
    static void makeSlowToSignIn(MadeUpKvm& kvm, int afterMs)
    {
        Answered late = core::testing::good({{u"token"_s, u"pass1"_s}});
        late.afterMs = afterMs;
        kvm.first["/api/auth/login"_ba].append(late);
    }
    static void makeSlowToSayWhatItIs(MadeUpKvm& kvm, int afterMs)
    {
        Answered late = core::testing::refusal(401, u"UnauthorizedError"_s);
        late.afterMs = afterMs;
        kvm.instead.insert("/api/auth/check"_ba, late);
    }

    // Puts a made-up KVM on a desk's own wall as it stands there once it was added, with the
    // password the made-up KVMs have, known by the certificate of that name where one is named;
    // and gives the wall's name for it.
    static QString put(Desk& desk, const MadeUpKvm& kvm, const QString& name, const char* certificate = nullptr)
    {
        core::OwnKvm known;
        known.address = kvm.stand->address();
        known.name = name;
        known.knownByCertificate = certificate != nullptr;
        known.certificate = certificate != nullptr ? sha256Of(certificate) : QByteArray();
        const base::Result<QString> id = desk.own->wall().add(known, u"secret"_s);
        return id.ok() ? id.value() : QString();
    }
    static core::OwnKvm kvmOf(Desk& desk, const QString& id)
    {
        const QList<core::OwnKvm>& all = desk.own->wall().kvms();
        const auto known = std::ranges::find_if(all, [&id](const core::OwnKvm& one) { return one.id == id; });
        return known == all.end() ? core::OwnKvm{} : *known;
    }

private slots:
    void independentWindowsKeepTheirSourceAndRestoreNamedWorkspaces()
    {
        bool firstGone = false;
        const QByteArray status = R"({"a": {"state": "online"}, "b": {"state": "online"}})";
        StandIn first([&](const Asked& asked) {
            if (firstGone && asked.path == "/api/v1/status")
                return Answered{401, R"({"code":"unauthorized", "message":"Sign in first"})"};
            return signedIn(asked, status);
        });
        StandIn second([&](const Asked& asked) { return signedIn(asked, status); });
        const auto firstAddress = first.address() + u"/"_s;
        const auto secondAddress = second.address() + u"/"_s;
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {windowIn(2), windowIn(4)}), true);
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        desk.session.useOwnWall(desk.own->wall());
        QVERIFY(desk.windows.openAll());
        auto* left = desk.windows.session(0);
        auto* right = desk.windows.session(1);
        QVERIFY(left != right);
        left->chooseSource(firstAddress);
        right->chooseSource(secondAddress);
        QTRY_COMPARE(desk.windows.wall(0)->machines()->rowCount(), 2);
        QTRY_COMPARE(desk.windows.wall(1)->machines()->rowCount(), 2);
        desk.windows.wall(1)->enlarge(Desk::key(u"b"_s));
        QCOMPARE(desk.windows.wall(1)->enlarged(), Desk::key(u"b"_s));
        left->goToOwnWall();
        QVERIFY(left->ownWall());
        QCOMPARE(desk.windows.wall(1)->enlarged(), Desk::key(u"b"_s));
        QCOMPARE(desk.windows.wall(1)->machines()->rowCount(), 2);
        QCOMPARE(right->source(), secondAddress);
        desk.windows.window(0)->setGeometry(90, 110, 900, 600);
        auto& workspaces = desk.windows.workspaces();
        QVERIFY(workspaces.save(u"  Office   monitors  "_s));
        const auto id = desk.settings.remembered().workspaces.first().id;
        QCOMPARE(desk.settings.remembered().workspaces.first().name, u"Office monitors"_s);
        QCOMPARE(desk.settings.remembered().workspaces.first().windows.at(0).source, u"pc"_s);
        QCOMPARE(desk.settings.remembered().workspaces.first().windows.at(1).source, secondAddress);
        QVERIFY(!workspaces.save(u"office monitors"_s));
        QVERIFY(workspaces.rename(id, u"Evening"_s));
        desk.askIn(1);
        QCOMPARE(desk.windows.count(), 3);
        QTRY_COMPARE(desk.windows.session(2)->source(), secondAddress);
        desk.windows.wall(0)->setColumns(6);
        left->chooseSource(firstAddress);
        QTRY_COMPARE(desk.windows.wall(0)->machines()->rowCount(), 2);
        firstGone = true;
        QTRY_VERIFY(desk.windows.wall(0)->machines()->rowCount() == 0);
        QCOMPARE(desk.windows.wall(1)->machines()->rowCount(), 2);
        QCOMPARE(desk.windows.wall(1)->enlarged(), Desk::key(u"b"_s));
        QVERIFY(workspaces.restore(id));
        QTRY_COMPARE(desk.windows.count(), 2);
        QTRY_VERIFY(left->ownWall());
        QCOMPARE(desk.windows.wall(0)->columns(), 2);
        QCOMPARE(desk.windows.wall(1)->columns(), 4);
        QVERIFY(desk.windows.wall(1)->enlarged().isEmpty());
        QCOMPARE(desk.windows.window(0)->geometry(), QRect(90, 110, 900, 600));
        QVERIFY(!desk.windows.control(0)->isOn());
        QVERIFY(!desk.windows.control(1)->isOn());
        QTRY_COMPARE(desk.windows.wall(1)->machines()->rowCount(), 2);
        const auto kept = core::Settings(desk.file()).remembered();
        QCOMPARE(kept.workspaces.first().name, u"Evening"_s);
        QCOMPARE(kept.windows.at(1).source, secondAddress);
        ui::ControlView::Shown controlling;
        controlling.on = true;
        desk.windows.control(0)->show(controlling);
        QVERIFY(!workspaces.restore(id));
        desk.windows.control(0)->show({});
        QVERIFY(workspaces.remove(id));
        QVERIFY(workspaces.entries().isEmpty());
    }

    void rememberedSourcesOverrideTheLastServerAndWorkspaceGrowthRecoversAMissingMonitor()
    {
        const QByteArray status = R"({"a": {"state": "online"}, "b": {"state": "online"}})";
        StandIn server([&](const Asked& asked) { return signedIn(asked, status); });
        const QTemporaryDir folder;
        auto local = windowIn(2);
        local.source = u"pc"_s;
        auto remote = windowAt(at(9000, 9000, 900, 600), {}, u"name"_s, 3);
        remote.source = server.address() + u'/';
        const auto file = fileWith(folder, {local});
        {
            core::Settings settings(file);
            auto saved = settings.remembered();
            saved.server = remote.source;
            saved.showsOwnWall = false;
            saved.workspaces = {{QString(32, u'a'), u"Two monitors"_s, {local, remote}}};
            QVERIFY(settings.remember(saved));
        }
        {
            Desk desk(file, true);
            desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
            desk.session.useOwnWall(desk.own->wall());
            QVERIFY(desk.windows.openAll());
            QVERIFY(desk.windows.session(0)->ownWall());
            QVERIFY(desk.windows.workspaces().restore(QString(32, u'a')));
            QTRY_COMPARE(desk.windows.count(), 2);
            QTRY_COMPARE(desk.windows.wall(1)->machines()->rowCount(), 2);
            QCOMPARE(desk.windows.wall(1)->columns(), 3);
            QCOMPARE(desk.windows.session(1)->source(), remote.source);
            QCOMPARE(desk.windows.window(1)->position(), QPoint(24, 24));
            QVERIFY(!desk.windows.control(1)->isOn());
        }
        Desk restarted(file, true);
        restarted.own = std::make_unique<ui::OwnWall>(restarted.settings, restarted.secrets, quick());
        restarted.session.useOwnWall(restarted.own->wall());
        QVERIFY(restarted.windows.openAll());
        QCOMPARE(restarted.windows.count(), 2);
        QVERIFY(restarted.windows.session(0)->ownWall());
        QTRY_COMPARE(restarted.windows.wall(1)->machines()->rowCount(), 2);
        QCOMPARE(restarted.windows.session(1)->source(), remote.source);
        QVERIFY(restarted.windows.wall(1)->enlarged().isEmpty());
    }

    void workspaceLimitsLockAndWriteFailuresAreVisible()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(
            secrets,
            [](QByteArrayView text, QByteArrayView salt, const std::stop_token&) {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                hash.addData(text);
                hash.addData(salt);
                return base::Result<QByteArray>(hash.result());
            },
            [] { return 1000; });
        lock.submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!lock.busy());
        QVERIFY(!lock.restricted());
        ui::WorkspacesView view(settings);
        view.useAppLock(lock);
        int restored = 0;
        view.useWindows([] { return QList<core::WallWindow>{windowIn(3)}; },
                        [&](const auto& windows) {
                            ++restored;
                            return windows.size() == 1;
                        });
        for (int i = 0; i < core::Remembered::mostWorkspaces; ++i)
            QVERIFY(view.save(QString::number(i)));
        QVERIFY(!view.save(u"One more"_s));
        const auto id = settings.remembered().workspaces.first().id;
        QVERIFY(view.save(u"Updated"_s, id));
        QVERIFY(view.restore(id));
        QCOMPARE(restored, 1);
        lock.lock();
        QVERIFY(view.entries().isEmpty());
        QVERIFY(!view.restore(id));
        QVERIFY(!view.rename(id, u"Changed while locked"_s));
        QVERIFY(!view.remove(id));
        QCOMPARE(restored, 1);
        QCOMPARE(settings.remembered().workspaces.size(), 16);

        const auto blocked = folder.filePath(u"file-in-the-way"_s);
        QFile file(blocked);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        core::Settings unwritable(blocked + u"/settings.json"_s, {1, 0});
        ui::WorkspacesView failing(unwritable);
        failing.useWindows([] { return QList<core::WallWindow>{windowIn(2)}; }, {});
        QVERIFY(!failing.save(u"Unsaved"_s));
        QVERIFY(!failing.problem().isEmpty());
        QCOMPARE(failing.entries().size(), 1);
    }

    void updatesWaitForControlAndActionsInEveryWindow()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {}));
        QVERIFY(desk.windows.openAll());
        QVERIFY(desk.windows.openOne());
        QVERIFY(desk.windows.readyForUpdate());
        ui::ControlView::Shown control;
        control.on = true;
        desk.windows.control(1)->show(control);
        QVERIFY(!desk.windows.readyForUpdate());
        desk.windows.control(1)->show({});
        desk.windows.actions(0)->setBusy(true);
        QVERIFY(!desk.windows.readyForUpdate());
        desk.windows.actions(0)->setBusy(false);
        desk.windows.actions(1)->showRun({{u"running"_s, true}});
        QVERIFY(!desk.windows.readyForUpdate());
        desk.windows.actions(1)->showRun({{u"running"_s, false}});
        QVERIFY(desk.windows.readyForUpdate());
        desk.windows.window(1)->setProperty("screenshotHoldsWindow", true);
        QVERIFY(!desk.windows.readyForUpdate());
        QVERIFY(!desk.windows.window(1)->close());
        desk.windows.window(1)->setProperty("screenshotHoldsWindow", false);
        QVERIFY(desk.windows.readyForUpdate());
    }

    // The program closes for Setup only where its end cuts nothing short, and says what holds it (fix round of the
    // Setup updates): a box with changes that are not saved, virtual media being changed in another window, an action
    // sent from a card that is not answered yet (a start into BIOS setup through a server), and a start into BIOS
    // setup of this PC's own server, or its override that may still be set.
    void anUpdateWaitsForUnsavedBoxesMediaActionsAndBiosSetup()
    {
        int acted = 0;
        StandIn server([&acted](const Asked& asked) { return withARackServer(asked, acted); });
        StandIn mediaServer([](const Asked& asked) { return mediaOfAKvm(asked); });
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {}));
        QVERIFY(desk.windows.openAll());
        QVERIFY(desk.windows.openOne());
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.windows.wall(0)->machines()->rowCount(), 1);
        QTRY_VERIFY(desk.windows.actions(0)->allowed());
        QCOMPARE(desk.windows.whatHoldsAnUpdate(), QString());

        // A box with changes that are not saved, by its title.
        desk.windows.window(1)->setProperty("unsavedEdit", u"Add a KVM"_s);
        QCOMPARE(desk.windows.whatHoldsAnUpdate(),
                 u"\"Add a KVM\" has changes that are not saved. Save them or close it before installing."_s);
        desk.windows.window(1)->setProperty("unsavedEdit", QString());
        QCOMPARE(desk.windows.whatHoldsAnUpdate(), QString());

        // A start into BIOS setup through the server, sent from a window and not answered yet.
        ui::ActionsView& actions = *desk.windows.actions(0);
        actions.open(Desk::key(u"srv"_s));
        actions.choose(u"power:bios_setup"_s);
        QVERIFY(!actions.question().isEmpty());
        actions.answer(true);
        QTRY_COMPARE(acted, 1);
        QCOMPARE(desk.windows.whatHoldsAnUpdate(),
                 u"An action sent to a machine has not been answered yet. Wait for its answer before installing."_s);
        QTRY_COMPARE_WITH_TIMEOUT(desk.windows.whatHoldsAnUpdate(), QString(), 10000);

        // This PC's own server starting into BIOS setup, or its override that may still be set.
        const QUrl controller(u"https://192.168.1.61"_s);
        static_cast<void>(core::SetupOverrides::program().mayBeSet(controller, QString(),
                                                                   QDateTime::currentDateTimeUtc().addSecs(60)));
        QVERIFY(desk.windows.whatHoldsAnUpdate().startsWith(u"A start into BIOS setup is under way"_s));
        core::SetupOverrides::program().drop(controller, QString());
        QCOMPARE(desk.windows.whatHoldsAnUpdate(), QString());

        // Virtual media being changed in the other window, through a server.
        core::ServerClient client;
        static_cast<void>(client.setAddress(mediaServer.address()));
        client.setSession(QByteArray(43, 's'));
        ui::MediaView::UnderWay underWay;
        ui::MediaView& media = *desk.windows.actions(1)->media();
        media.open(&client, u"kvm"_s, u"Build server"_s, u"wall:kvm"_s, &underWay, u"one:kvm"_s);
        QTRY_VERIFY(!media.images().isEmpty());
        media.pick(u"debian-13.iso"_s);
        media.chooseDrive(u"cd"_s);
        media.mount();
        QVERIFY(!media.question().isEmpty());
        media.answer(true);
        QVERIFY(media.busy());
        QCOMPARE(desk.windows.whatHoldsAnUpdate(), u"Virtual media are being uploaded or changed in a qKVM window. Let "
                                                   "that finish before installing."_s);
        QTRY_VERIFY_WITH_TIMEOUT(!media.busy(), 10000);
        QCOMPARE(desk.windows.whatHoldsAnUpdate(), QString());
        media.letGo();
    }

    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    // How video is decoded is told to the graphics device of every window there is, and of every
    // window that comes after.
    void theWayOfDecodingReachesEveryWindowsDevice()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {}));
        desk.makesGpus = true;
        QVERIFY(desk.windows.openAll());
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.windows.count(), 2);
        QVERIFY(desk.windows.gpu(0) != nullptr);
        QVERIFY(desk.windows.gpu(1) != nullptr);
        QVERIFY(desk.windows.gpu(2) == nullptr);
        QCOMPARE(desk.windows.gpu(0)->decoding(), engine::DecodingChoice::Automatic);
        // And each window's wall says what its card calls itself.
        QCOMPARE(desk.windows.wall(0)->graphicsCard(), QString::fromStdString(desk.windows.gpu(0)->adapterName()));
        QVERIFY(!desk.windows.wall(1)->graphicsCard().isEmpty());
        desk.windows.setDecoding(engine::DecodingChoice::Software);
        QCOMPARE(desk.windows.gpu(0)->decoding(), engine::DecodingChoice::Software);
        QCOMPARE(desk.windows.gpu(1)->decoding(), engine::DecodingChoice::Software);
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.windows.gpu(2)->decoding(), engine::DecodingChoice::Software);
        desk.windows.setDecoding(engine::DecodingChoice::GraphicsCard);
        for (qsizetype index = 0; index < 3; ++index) {
            QCOMPARE(desk.windows.gpu(index)->decoding(), engine::DecodingChoice::GraphicsCard);
        }
    }

    void aPcThatNeverRanTheProgramHasOneWindow()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {}));
        // Nobody sees a wall before there is a window.
        QVERIFY(desk.all.isUnseen());
        QVERIFY(desk.windows.openAll());
        QCOMPARE(desk.windows.count(), 1);
        QVERIFY(desk.windows.window(0)->isVisible());
        QVERIFY(desk.windows.window(1) == nullptr);
        QVERIFY(desk.windows.wall(1) == nullptr);
        QVERIFY(desk.windows.actions(-1) == nullptr);
        QVERIFY(!desk.all.isUnseen());
        // Once it has come to rest, it is written down: one window.
        QTRY_COMPARE(desk.kept().size(), 1);
        QVERIFY(desk.kept().constFirst().place.has_value());
    }

    void whereNoWindowCanBeMadeThatIsSaid()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        desk.makes = false;
        QVERIFY(!desk.windows.openAll());
        QVERIFY(!desk.windows.openOne());
        QCOMPARE(desk.windows.count(), 0);
        QVERIFY(desk.all.isUnseen());
    }

    void theWindowsThatWereOpenAreThereAgainEachAsItWas()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {windowAt(at(100, 100, 900, 600), u"Rack B"_s, {}, 2),
                                    windowAt(at(2000, 100, 1000, 700), {}, u"name"_s), core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        QCOMPARE(desk.windows.count(), 3);
        // Each where it was: the second on the second monitor.
        QCOMPARE(desk.windows.window(0)->geometry(), QRect(100, 100, 900, 600));
        QCOMPARE(desk.windows.window(1)->geometry(), QRect(2000, 100, 1000, 700));
        for (qsizetype index = 0; index < 3; ++index) {
            QVERIFY(desk.windows.window(index)->isVisible());
        }
        // And each with its wall as it was shown.
        QCOMPARE(desk.windows.wall(0)->columns(), 2);
        QCOMPARE(desk.windows.wall(0)->shown()->group(), u"Rack B"_s);
        QCOMPARE(desk.windows.wall(1)->shown()->order(), u"name"_s);
        QCOMPARE(desk.windows.wall(1)->columns(), 0);
        QCOMPARE(desk.windows.wall(2)->shown()->order(), QString());
        // What is chosen in one is written down for that one.
        desk.windows.wall(1)->setColumns(4);
        QCOMPARE(desk.kept().at(1).columns, 4);
        QCOMPARE(desk.kept().at(0).columns, 2);
        QCOMPARE(desk.kept().at(2).columns, 0);
        QCOMPARE(desk.kept().size(), 3);
    }

    void aNewWindowIsOneMoreAndThereAreNoMoreThanThereMayBe()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {windowIn(3)}));
        QVERIFY(desk.windows.openAll());
        QCOMPARE(desk.windows.count(), 1);
        // Asked for in the window, as "New window" asks: one more, in sight, and as a new one is.
        // It comes up a step beside the window it was asked in, of that one's size: over it, it
        // would hide it, and be taken for it.
        desk.windows.window(0)->setGeometry(40, 60, 600, 380);
        desk.askIn(0);
        QCOMPARE(desk.windows.count(), 2);
        QVERIFY(desk.windows.window(1)->isVisible());
        QCOMPARE(desk.windows.window(1)->geometry(), QRect(72, 92, 600, 380));
        QCOMPARE(desk.windows.window(0)->geometry(), QRect(40, 60, 600, 380));
        // Asked for in the same window once more: a step further, and not over the one before.
        desk.askIn(0);
        QCOMPARE(desk.windows.count(), 3);
        QCOMPARE(desk.windows.window(2)->geometry(), QRect(104, 124, 600, 380));
        QVERIFY(desk.windows.window(2)->close());
        QTRY_COMPARE(desk.windows.count(), 2);
        QCOMPARE(desk.windows.wall(1)->columns(), 0);
        QCOMPARE(desk.windows.wall(0)->columns(), 3);
        // And the new one can ask for the next. One that would reach out of the screen comes up
        // a step from the screen's corner instead.
        desk.windows.window(1)->setGeometry(150, 400, 600, 380);
        desk.askIn(1);
        QCOMPARE(desk.windows.count(), 3);
        const QRect free = desk.windows.window(1)->screen()->availableGeometry();
        QCOMPARE(desk.windows.window(2)->geometry(), QRect(free.x() + 32, free.y() + 32, 600, 380));
        // Asked for in a window that fills the screen, it is where it would be by itself.
        desk.windows.window(2)->showMaximized();
        QTRY_COMPARE(desk.windows.window(2)->visibility(), QWindow::Maximized);
        desk.askIn(2);
        QCOMPARE(desk.windows.count(), 4);
        QCOMPARE(desk.windows.window(3)->size(), QSize(640, 400));
        while (desk.windows.count() < core::Remembered::mostWindows) {
            QVERIFY(desk.windows.openOne());
        }
        // As many as there may be: no more, and that is said in the window that asked, and in no
        // other.
        QVERIFY(!desk.windows.openOne());
        desk.askIn(1);
        QCOMPARE(desk.windows.count(), core::Remembered::mostWindows);
        const ui::Notices& told = *desk.windows.actions(1)->notices();
        QCOMPARE(told.count(), 1);
        QCOMPARE(told.index(0, 0).data(ui::Notices::TextRole).toString(), u"qKVM opens no more than 8 windows."_s);
        QCOMPARE(desk.windows.actions(0)->notices()->count(), 0);
        // All of them are there again the next time.
        QTRY_COMPARE(desk.kept().size(), core::Remembered::mostWindows);
        QCOMPARE(desk.kept().constFirst().columns, 3);
    }

    void aWindowThatIsRememberedSomewhereComesUpThere()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {windowAt(at(40, 60, 600, 380)), windowAt(at(2000, 100, 1000, 700), {}, {}, 4)}));
        // Only the first of the two could be opened at the start; the second is asked for in it
        // later. It is where it was, and as it was, and not a step beside the first.
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.windows.count(), 1);
        desk.askIn(0);
        QCOMPARE(desk.windows.count(), 2);
        QCOMPARE(desk.windows.window(1)->geometry(), QRect(2000, 100, 1000, 700));
        QCOMPARE(desk.windows.wall(1)->columns(), 4);
    }

    void aWindowThatIsClosedIsGoneAndTheOnesAfterItMoveUp()
    {
        const QTemporaryDir folder;
        Desk desk(
            fileWith(folder, {windowAt(at(100, 100, 900, 600), {}, {}, 1), windowAt(at(200, 150, 900, 600), {}, {}, 2),
                              windowAt(at(300, 200, 900, 600), {}, {}, 3)}));
        QVERIFY(desk.windows.openAll());
        QCOMPARE(desk.windows.count(), 3);
        ui::WallView* third = desk.windows.wall(2);

        // The second is closed: the program goes on with two, and the third is the second now.
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 2);
        QCOMPARE(desk.windows.wall(1), third);
        QCOMPARE(desk.kept().size(), 2);
        QCOMPARE(desk.kept().at(0).columns, 1);
        QCOMPARE(desk.kept().at(1).columns, 3);
        QCOMPARE(desk.kept().at(1).place.value_or(core::WindowPlace{}), at(300, 200, 900, 600));
        // What is chosen in it from now on is written where it is now.
        third->setColumns(5);
        QCOMPARE(desk.kept().size(), 2);
        QCOMPARE(desk.kept().at(1).columns, 5);
        QCOMPARE(desk.kept().at(0).columns, 1);
        // So is where it is moved to.
        desk.windows.window(1)->setGeometry(400, 250, 800, 500);
        QTRY_COMPARE(desk.kept().at(1).place.value_or(core::WindowPlace{}), at(400, 250, 800, 500));
        QCOMPARE(desk.kept().at(0).place.value_or(core::WindowPlace{}), at(100, 100, 900, 600));

        // The first is closed as well: one is left, with what was the third's.
        QVERIFY(desk.windows.window(0)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
        QCOMPARE(desk.kept().size(), 1);
        QCOMPARE(desk.kept().at(0).columns, 5);
        // And one more comes after it, as a new one is.
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.windows.wall(1)->columns(), 0);
    }

    void theLastWindowIsThereAgainTheNextTime()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder,
                           {windowAt(at(100, 100, 900, 600), {}, {}, 1), windowAt(at(200, 150, 900, 600), {}, {}, 2)}));
        QVERIFY(desk.windows.openAll());
        // Both are closed, one right after the other: the one that was closed last is the
        // program's last window, and is kept.
        QVERIFY(desk.windows.window(0)->close());
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
        QTest::qWait(150);
        QCOMPARE(desk.windows.count(), 1);
        QCOMPARE(desk.kept().size(), 1);
        QCOMPARE(desk.kept().at(0).columns, 2);
        QCOMPARE(desk.windows.wall(0)->columns(), 2);
    }

    void whereTheProgramGoesOnTheLastWindowGoesBehindTheIconAndNoOther()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {windowIn(1), windowIn(2)}));
        core::Remembered stays = desk.settings.remembered();
        stays.staysInTray = true;
        QVERIFY(desk.settings.remember(stays).ok());
        QVERIFY(desk.windows.openAll());
        QVERIFY(desk.tray.isThere());
        // One of two is closed, and is gone.
        QVERIFY(desk.windows.window(0)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
        // The last one is not: out of sight, still the program's, and the icon brings it back.
        QQuickWindow* last = desk.windows.window(0);
        QVERIFY(!last->close());
        QVERIFY(!last->isVisible());
        QTest::qWait(100);
        QCOMPARE(desk.windows.count(), 1);
        QCOMPARE(desk.kept().size(), 1);
        QCOMPARE(desk.kept().at(0).columns, 2);
        desk.tray.click();
        QVERIFY(last->isVisible());
        // A window that comes now is one of two again, and is closed like any.
        QVERIFY(desk.windows.openOne());
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
    }

    void whenTheProgramEndsAsAWholeAllItsWindowsAreThereAgain()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {windowIn(1), windowIn(2), windowIn(3)}));
        QVERIFY(desk.windows.openAll());
        QCOMPARE(desk.windows.count(), 3);
        // "Quit" in the icon's menu; and then Qt closes every window, as it does when a program
        // is told to end.
        QVERIFY(desk.tray.choose(u"Quit"_s));
        for (qsizetype index = 0; index < 3; ++index) {
            QVERIFY(desk.windows.window(index)->close());
        }
        QTest::qWait(150);
        QCOMPARE(desk.windows.count(), 3);
        QCOMPARE(desk.kept().size(), 3);
        QCOMPARE(desk.kept().at(2).columns, 3);
    }

    void aWindowThatFillsAScreenKeepsTheDisplayAwakeWhateverTheOthersDo()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        QVERIFY(desk.keptAwake.isEmpty());
        desk.windows.window(1)->showFullScreen();
        QTRY_COMPARE(desk.keptAwake, QList<bool>{true});
        // The window beside it fills its screen and leaves it again: the first still fills its own.
        desk.windows.window(0)->showFullScreen();
        QTRY_COMPARE(desk.windows.window(0)->visibility(), QWindow::FullScreen);
        desk.windows.window(0)->showNormal();
        QTRY_COMPARE(desk.windows.window(0)->visibility(), QWindow::Windowed);
        // (Until the platform has said it all once more, as it does a moment later.)
        QTest::qWait(150);
        QCOMPARE(desk.windows.window(0)->visibility(), QWindow::Windowed);
        QCOMPARE(desk.keptAwake.size(), 1);
        // The one that fills a screen is closed, and gone: nothing holds the display any more.
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
        QCOMPARE(desk.keptAwake, (QList<bool>{true, false}));
        // The one that is left fills its screen: held again, and let go of when it leaves it.
        desk.windows.window(0)->showFullScreen();
        QTRY_COMPARE(desk.keptAwake, (QList<bool>{true, false, true}));
        desk.windows.window(0)->showNormal();
        QTRY_COMPARE(desk.keptAwake, (QList<bool>{true, false, true, false}));
    }

    void nobodySeesTheProgramsOwnWallWhileNobodySeesAWindow()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        QVERIFY(!desk.all.isUnseen());
        // One window is minimized: the other is seen.
        desk.windows.wall(0)->setUnseen(true);
        QVERIFY(!desk.all.isUnseen());
        desk.windows.wall(1)->setUnseen(true);
        QVERIFY(desk.all.isUnseen());
        desk.windows.wall(0)->setUnseen(false);
        QVERIFY(!desk.all.isUnseen());
        // The one that is seen is closed: what is left is the one nobody sees.
        desk.windows.wall(0)->setUnseen(true);
        desk.windows.wall(1)->setUnseen(false);
        QVERIFY(!desk.all.isUnseen());
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
        QVERIFY(desk.all.isUnseen());
        // A new one is seen.
        QVERIFY(desk.windows.openOne());
        QVERIFY(!desk.all.isUnseen());
    }

    void everyWindowHasTheWallAndThePaceOfAll()
    {
        QByteArray status = R"({"a": {"state": "online"}, "b": {"state": "online"}})";
        StandIn server([&status](const Asked& asked) { return signedIn(asked, status); });
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.all.machines()->rowCount(), 2);
        QCOMPARE(desk.windows.wall(0)->machines()->rowCount(), 2);
        QCOMPARE(desk.windows.wall(1)->machines()->rowCount(), 2);
        QVERIFY(desk.windows.actions(1)->allowed());

        // How often pictures are asked for, chosen in one window: in all, and remembered once.
        desk.windows.wall(1)->setPace(2000);
        QCOMPARE(desk.windows.wall(0)->pace(), 2000);
        QCOMPARE(desk.all.pace(), 2000);
        QCOMPARE(core::Settings(desk.file()).remembered().wallPictureEveryMs, 2000);

        // A window that comes while the wall is up has the machines and the pace at once.
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.windows.wall(2)->machines()->rowCount(), 2);
        QCOMPARE(desk.windows.wall(2)->pace(), 2000);
        QVERIFY(desk.windows.actions(2)->allowed());

        // One is closed: the others go on being told how the machines are.
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 2);
        status = R"({"a": {"state": "offline"}, "b": {"state": "online"}})";
        QTRY_COMPARE(desk.all.machines()->attention(), 1);
        QCOMPARE(desk.windows.wall(0)->machines()->attention(), 1);
        QCOMPARE(desk.windows.wall(1)->machines()->attention(), 1);
        // And a pace chosen now reaches those that are left.
        desk.windows.wall(0)->setPace(500);
        QCOMPARE(desk.windows.wall(1)->pace(), 500);
    }

    void theBoxThatAddsAKvmIsEachWindowsOwn()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        ui::AddKvmView* first = desk.windows.adding(0);
        ui::AddKvmView* second = desk.windows.adding(1);
        QVERIFY(first != nullptr && second != nullptr && first != second);
        QCOMPARE(desk.windows.adding(2), nullptr);
        // Whoever adds KVMs was handed each, once; and the one of a window that comes later.
        QCOMPARE(desk.served, (QList<ui::AddKvmView*>{first, second}));
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.served.size(), 3);
        QCOMPARE(desk.served.constLast(), desk.windows.adding(2));
        // What one says is nothing to the others.
        second->began();
        QVERIFY(second->busy());
        QVERIFY(!first->busy());
        QVERIFY(!desk.windows.adding(2)->busy());
    }

    void theBoxThatChangesAKvmIsEachWindowsOwn()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        ui::EditKvmView* first = desk.windows.editing(0);
        ui::EditKvmView* second = desk.windows.editing(1);
        QVERIFY(first != nullptr && second != nullptr && first != second);
        QCOMPARE(desk.windows.editing(2), nullptr);
        QCOMPARE(desk.windows.editing(-1), nullptr);
        // Whoever changes KVMs was handed each, once; and the one of a window that comes later.
        QCOMPARE(desk.servedToChange, (QList<ui::EditKvmView*>{first, second}));
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.servedToChange.size(), 3);
        QCOMPARE(desk.servedToChange.constLast(), desk.windows.editing(2));
        // What one is opened for, asks and is told is nothing to the others.
        const QSignalSpy askedOfTheFirst(first, &ui::EditKvmView::saveAsked);
        const QSignalSpy askedOfTheSecond(second, &ui::EditKvmView::saveAsked);
        ui::EditKvmView::Shown kvm;
        kvm.id = u"0a1b2c3d"_s;
        kvm.called = u"NAS"_s;
        kvm.name = u"NAS"_s;
        kvm.address = u"192.168.1.22"_s;
        kvm.https = true;
        second->show(kvm, false);
        QCOMPARE(second->machine(), u"0a1b2c3d"_s);
        QCOMPARE(first->machine(), QString());
        QCOMPARE(desk.windows.editing(2)->machine(), QString());
        second->save(u"Storage"_s, u"192.168.1.22"_s, u"admin"_s, QString(), QString());
        first->save(u"Storage"_s, u"192.168.1.22"_s, u"admin"_s, QString(), QString());
        QCOMPARE(askedOfTheSecond.count(), 1);
        QCOMPARE(askedOfTheFirst.count(), 0);
        second->began(u"saving"_s);
        QVERIFY(second->busy());
        QVERIFY(!first->busy());
        QVERIFY(!desk.windows.editing(2)->busy());
        second->refused(u"Device is unreachable"_s);
        QCOMPARE(second->problem(), u"Device is unreachable"_s);
        QCOMPARE(first->problem(), QString());
        // A window that is closed takes its own with it, and the others' stay theirs.
        QVERIFY(desk.windows.window(0)->close());
        QTRY_COMPARE(desk.windows.count(), 2);
        QCOMPARE(desk.windows.editing(0), second);
        QCOMPARE(second->machine(), u"0a1b2c3d"_s);
    }

    void theWallAsAFileIsEachWindowsOwn()
    {
        const QByteArray status = R"({"a": {"state": "online"}, "b": {"state": "online"}})";
        StandIn server([&status](const Asked& asked) {
            return asked.path == "/api/v1/export" ? Answered{200, R"({"format": "qkvm-export", "devices": []})"}
                                                  : signedIn(asked, status);
        });
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.all.machines()->rowCount(), 2);
        ui::FilesView* first = desk.windows.files(0);
        ui::FilesView* second = desk.windows.files(1);
        QVERIFY(first != nullptr && second != nullptr && first != second);
        QCOMPARE(desk.windows.files(2), nullptr);

        // Where the file is to be kept is asked over the window whose box asked; the server is
        // asked for that window, and the other one has nothing under way and is told nothing.
        desk.savesTo = folder.filePath(u"wall.json"_s);
        const QSignalSpy done(second, &ui::FilesView::done);
        const QSignalSpy doneInTheOther(first, &ui::FilesView::done);
        second->exportWall({u"devices"_s}, {}, u"my own"_s);
        QCOMPARE(desk.askedOver, (QList<QWindow*>{desk.windows.window(1)}));
        QVERIFY(second->isBusy());
        QVERIFY(!first->isBusy());
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), u"Exported, without passwords"_s);
        QVERIFY(QFile::exists(desk.savesTo));
        QCOMPARE(doneInTheOther.count(), 0);
        // A window that comes later has its own as well.
        QVERIFY(desk.windows.openOne());
        QFile::remove(desk.savesTo);
        const QSignalSpy doneInTheThird(desk.windows.files(2), &ui::FilesView::done);
        desk.windows.files(2)->exportWall({u"devices"_s}, {}, u"my own"_s);
        QCOMPARE(desk.askedOver.last(), desk.windows.window(2));
        QTRY_COMPARE(doneInTheThird.count(), 1);
        QVERIFY(QFile::exists(desk.savesTo));
    }

    void aWindowWithSomethingOfAFileUnderWayIsNotClosed()
    {
        // One of Windows' own boxes may stand over it, and what the server answers is to be told
        // in it: it stays until that is over.
        const QByteArray status = R"({"a": {"state": "online"}, "b": {"state": "online"}})";
        StandIn server([&status](const Asked& asked) {
            if (asked.path == "/api/v1/export") {
                Answered answer{200, R"({"format": "qkvm-export", "devices": []})"};
                answer.afterMs = 500;
                return answer;
            }
            return signedIn(asked, status);
        });
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.all.machines()->rowCount(), 2);
        desk.savesTo = folder.filePath(u"wall.json"_s);
        const QSignalSpy done(desk.windows.files(1), &ui::FilesView::done);
        desk.windows.files(1)->exportWall({u"devices"_s}, {}, u"my own"_s);
        QVERIFY(desk.windows.files(1)->isBusy());
        // Asked to close meanwhile: it stays, and so does what is remembered of it.
        QVERIFY(!desk.windows.window(1)->close());
        QTest::qWait(100);
        QCOMPARE(desk.windows.count(), 2);
        QVERIFY(desk.windows.window(1)->isVisible());
        QCOMPARE(desk.kept().size(), 2);
        // A window with nothing under way goes as ever, while that one waits.
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.windows.count(), 3);
        QVERIFY(desk.windows.files(1)->isBusy());
        QVERIFY(desk.windows.window(2)->close());
        QTRY_COMPARE(desk.windows.count(), 2);
        QTRY_COMPARE(done.count(), 1);

        // One of Windows' own boxes stands over it, and the wall is left behind the box. Nothing
        // is waited for any more, and the window stays all the same until the box is gone: the
        // press that opened the box is still being answered in it.
        bool wasBusy = true;
        bool wasClosed = true;
        desk.whileAsked = [&](QWindow* over) {
            desk.windows.files(1)->reset();
            wasBusy = desk.windows.files(1)->isBusy();
            wasClosed = over->close();
        };
        desk.windows.files(1)->exportWall({u"devices"_s}, {}, u"my own"_s);
        desk.whileAsked = nullptr;
        QVERIFY(!wasBusy);
        QVERIFY(!wasClosed);
        QCOMPARE(desk.windows.count(), 2);
        QVERIFY(!desk.windows.files(1)->isBusy());
        // Over: it closes like any other.
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 1);

        // The program ending as a whole is not held up by a file.
        desk.windows.files(0)->exportWall({u"devices"_s}, {}, u"my own"_s);
        QVERIFY(desk.windows.files(0)->isBusy());
        desk.windows.end();
        QVERIFY(desk.windows.window(0)->close());
    }

    void thisPcsWallAsAFileIsEachWindowsOwnAndAWindowWithOneUnderWayIsNotClosed()
    {
        // Each window has boxes of its own for this PC's wall as a file, served by the wall the
        // program holds, with Windows' own boxes over that window. And a window with such a file
        // under way, or with one of Windows' boxes over it, is not closed.
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        // (At a cost a test can pay.)
        desk.own->sealFilesWith(
            core::exchange::Sealing{core::scrypt::Cost{4096, 8, 1}, core::exchange::windowsDerive()});
        QVERIFY(desk.windows.openAll());
        ui::OwnFilesView* first = desk.windows.ownFiles(0);
        ui::OwnFilesView* second = desk.windows.ownFiles(1);
        QVERIFY(first != nullptr && second != nullptr && first != second);
        QCOMPARE(desk.windows.ownFiles(2), nullptr);
        QCOMPARE(desk.servedWithFiles, (QList<ui::OwnFilesView*>{first, second}));
        // (A KVM at this PC's own address, where nobody is: it is written, and asked nothing.)
        core::OwnKvm kvm;
        kvm.address = u"http://127.0.0.1:20001"_s;
        kvm.name = u"NAS"_s;
        QVERIFY(desk.own->wall().add(kvm, u"secret"_s).ok());

        // The second window makes a file. While it is made, that window is not closed.
        desk.savesTo = folder.filePath(u"own.qkvm"_s);
        second->makeFile(u"a passphrase of mine"_s);
        QVERIFY(second->isBusy());
        QVERIFY(second->holdsItsWindow());
        QVERIFY(!desk.windows.window(1)->close());
        QCOMPARE(desk.windows.count(), 2);
        QVERIFY(desk.windows.window(1)->isVisible());
        QTRY_VERIFY(second->isMade());
        QVERIFY(!first->isMade());
        second->setBesideOnShow(true);
        QTRY_VERIFY_WITH_TIMEOUT(second->maySave(), 5000);
        // Where it is kept is asked over the window whose box asked. And while Windows' own box
        // stands over it, it is not closed: it would be left standing over nothing.
        bool wasClosed = true;
        bool heldItsWindow = false;
        desk.whileAsked = [&](QWindow* over) {
            heldItsWindow = second->holdsItsWindow();
            wasClosed = over->close();
        };
        const QSignalSpy exported(second, &ui::OwnFilesView::exported);
        second->saveFile();
        desk.whileAsked = nullptr;
        QCOMPARE(desk.askedOver, (QList<QWindow*>{desk.windows.window(1)}));
        QVERIFY(heldItsWindow);
        QVERIFY(!wasClosed);
        QCOMPARE(desk.windows.count(), 2);
        QCOMPARE(exported.count(), 1);
        QVERIFY(QFile::exists(desk.savesTo));
        // Over: it closes like any other. And a window that comes later has boxes of its own.
        QVERIFY(!second->holdsItsWindow());
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
        QVERIFY(desk.windows.openOne());
        QVERIFY(desk.windows.ownFiles(1) != nullptr);
        QCOMPARE(desk.servedWithFiles.size(), 3);
        // The program ending as a whole is not held up by a file.
        first->makeFile(u"a passphrase of mine"_s);
        QVERIFY(first->holdsItsWindow());
        desk.windows.end();
        QVERIFY(desk.windows.window(0)->close());
    }

    void aWindowOutOfSightHasItsBoxesForThisPcsWallAsAFileClosedAndWhatWasLaidOutForgotten()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}}));
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        core::Remembered behindTheIcon = desk.settings.remembered();
        behindTheIcon.staysInTray = true;
        QVERIFY(desk.settings.remember(behindTheIcon).ok());
        QVERIFY(desk.windows.openAll());
        ui::OwnFilesView* boxes = desk.windows.ownFiles(0);
        QQuickWindow* last = desk.windows.window(0);
        const QSignalSpy closeAsked(boxes, &ui::OwnFilesView::closeAsked);
        // A file with one KVM in it, at this PC's own address, is chosen in the window's box and
        // laid out; its KVM is ticked, and the list was looked at for its moment.
        core::exchange::Content content;
        core::exchange::Device device;
        device.address = u"https://127.0.0.1:20443"_s;
        device.name = u"From a file"_s;
        device.user = u"admin"_s;
        device.kind = u"kvmd"_s;
        device.tls = u"system"_s;
        content.devices = QList<core::exchange::Device>{device};
        const base::Result<core::exchange::Written> written = core::exchange::write(content, QString());
        QVERIFY(written.ok());
        desk.opens = folder.filePath(u"wall.json"_s);
        {
            QFile file(desk.opens);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(written.value().file(core::exchange::NotInIt::wasLookedAt));
        }
        boxes->chooseFile();
        boxes->readFile(QString());
        QTRY_VERIFY(boxes->isLaidOut());
        // (A tick is taken once the list has stood for its moment.)
        boxes->setLookedAt(true);
        QTRY_VERIFY_WITH_TIMEOUT(boxes->isSettled(), 5000);
        boxes->setTicked(1, true);
        QCOMPARE(boxes->tickedCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(boxes->mayTake(), 5000);
        // (A window that is only made small is in the taskbar, and comes back as it was left.)
        last->showMinimized();
        QTRY_COMPARE(last->visibility(), QWindow::Minimized);
        last->showNormal();
        QTRY_COMPARE(last->visibility(), QWindow::Windowed);
        QVERIFY(boxes->isLaidOut());
        QCOMPARE(closeAsked.count(), 0);

        // The last window is closed: out of sight, and not gone. Its boxes are closed from
        // outside: the screens are told, and nothing of the file is shown, kept or to be taken
        // in a window nobody sees.
        QVERIFY(!last->close());
        QVERIFY(!last->isVisible());
        QCOMPARE(closeAsked.count(), 1);
        QVERIFY(!boxes->isLaidOut());
        QCOMPARE(boxes->fileName(), QString());
        QCOMPARE(boxes->tickedCount(), 0);
        boxes->take();
        QVERIFY(desk.own->wall().kvms().isEmpty());
        // Brought back, it is as a window whose boxes were never opened.
        desk.tray.click();
        QVERIFY(last->isVisible());
        QVERIFY(!boxes->isLaidOut());
        QTest::qWait(ui::OwnFilesView::takeRestsMs + 200);
        boxes->take();
        QVERIFY(desk.own->wall().kvms().isEmpty());
    }

    void aWindowThatGoesEndsWhatItsBoxesHadAsked()
    {
        // Three windows of the program, each with its own boxes, and this PC's own wall as the
        // program holds it. A KVM that is slow to sign in, and one that shows another certificate
        // than it is known by and is slow to say what it is.
        // (What this holds is a rule that was there before a window's boxes were closed from
        // outside, and it passes without that: a box that goes with its window has given up
        // what it asked, which whoever serves it sees by the box's own end. What the closing
        // from outside adds is in the three tests after this one.)
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}, core::WallWindow{}}));
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        QVERIFY(desk.windows.openAll());
        QCOMPARE(desk.windows.count(), 3);
        MadeUpKvm slow;
        MadeUpKvm reset(tls("second"));
        MadeUpKvm slowToAdd;
        const int late = 1200;
        const QString nas = put(desk, slow, u"NAS"_s);
        const QString build = put(desk, reset, u"Build server"_s, "first");
        const core::OwnKvm buildBefore = kvmOf(desk, build);
        const QSignalSpy changed(&desk.own->wall(), &core::LocalWall::changed);
        ui::EditKvmView* stays = desk.windows.editing(0);
        ui::EditKvmView* saving = desk.windows.editing(1);
        ui::AddKvmView* adding = desk.windows.adding(1);
        ui::EditKvmView* trusting = desk.windows.editing(2);

        // The second window's box that changes a KVM has a change under way, with another
        // password typed for it, and its box that adds one a KVM that is being asked; the
        // third's has a yes to a certificate, which it was shown for long enough to give it.
        // (Somebody looks at that box: its screens say so of a window that is the active one.)
        trusting->setLookedAt(true);
        desk.own->openFor(*trusting, build, true);
        QTRY_VERIFY_WITH_TIMEOUT(trusting->isReviewed(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(trusting->mayTrust(), 5000);
        desk.own->openFor(*saving, nas, false);
        makeSlowToSignIn(slow, late);
        saving->save(u"Another name"_s, saving->address(), saving->user(), u"another one"_s, saving->group());
        QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        makeSlowToSignIn(slowToAdd, late);
        adding->add(slowToAdd.stand->address(), QString(), u"secret"_s, QString());
        QTRY_COMPARE(slowToAdd.asked("/api/auth/login"), 1);
        makeSlowToSayWhatItIs(reset, late);
        const qsizetype askedBefore = reset.stand->asked.size();
        trusting->trust();
        QVERIFY(trusting->busy());
        QTRY_VERIFY(reset.stand->asked.size() > askedBefore);

        // Both windows are closed, and gone.
        QVERIFY(desk.windows.window(2)->close());
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
        // The box of the window that is left is not kept waiting for either: its own change is
        // under way at once, and so is its own look at a certificate.
        desk.own->openFor(*stays, nas, false);
        const QSignalSpy done(stays, &ui::EditKvmView::done);
        stays->save(u"Storage"_s, stays->address(), stays->user(), QString(), stays->group());
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(stays->problem(), QString());
        QCOMPARE(changed.count(), 1);
        desk.own->openFor(*stays, build, false);
        stays->review();
        QTRY_VERIFY_WITH_TIMEOUT(stays->isReviewed() || !stays->busy(), 20000);
        QCOMPARE(stays->problem(), QString());
        QVERIFY(stays->isReviewed());
        const MadeUpKvm next;
        ui::AddKvmView* addsNext = desk.windows.adding(0);
        addsNext->add(next.stand->address(), QString(), u"secret"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!addsNext->busy(), 20000);
        QCOMPARE(addsNext->problem(), QString());
        QCOMPARE(addsNext->count(), 1);
        QCOMPARE(changed.count(), 2);
        // And when the KVMs answer at last, nothing is saved, added or trusted behind anybody's
        // back: not the name, not the password, not the KVM, not the certificate.
        QTest::qWait(late + 400);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(desk.own->wall().kvms().size(), 3);
        QCOMPARE(desk.own->wall().kvms().constLast().address, next.stand->address());
        QCOMPARE(kvmOf(desk, nas).name, u"Storage"_s);
        QCOMPARE(desk.secrets.all.value(core::LocalWall::secretOf(nas)),
                 core::testing::savedFor("secret", slow.stand->address()));
        QCOMPARE(kvmOf(desk, build), buildBefore);
    }

    void theLastWindowOutOfSightBehindTheIconHasItsBoxesClosed()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}}));
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        core::Remembered behindTheIcon = desk.settings.remembered();
        behindTheIcon.staysInTray = true;
        QVERIFY(desk.settings.remember(behindTheIcon).ok());
        QVERIFY(desk.windows.openAll());
        MadeUpKvm slow;
        MadeUpKvm toAdd;
        const QString nas = put(desk, slow, u"NAS"_s);
        const core::OwnKvm before = kvmOf(desk, nas);
        const QSignalSpy changed(&desk.own->wall(), &core::LocalWall::changed);
        ui::EditKvmView* editing = desk.windows.editing(0);
        ui::AddKvmView* adding = desk.windows.adding(0);

        const QSignalSpy closeAskedOfTheOneThatChanges(editing, &ui::EditKvmView::closeAsked);
        const QSignalSpy closeAskedOfTheOneThatAdds(adding, &ui::AddKvmView::closeAsked);
        QQuickWindow* last = desk.windows.window(0);

        // Its box that changes a KVM is open. (A window that is only made small is in the
        // taskbar, and comes back as it was left: its boxes stay.)
        desk.own->openFor(*editing, nas, false);
        last->showMinimized();
        QTRY_COMPARE(last->visibility(), QWindow::Minimized);
        last->showNormal();
        QTRY_COMPARE(last->visibility(), QWindow::Windowed);
        QCOMPARE(editing->machine(), nas);
        QCOMPARE(closeAskedOfTheOneThatChanges.count(), 0);
        QCOMPARE(closeAskedOfTheOneThatAdds.count(), 0);
        // It has a change under way, and the box that adds a KVM has one that is being asked.
        makeSlowToSignIn(slow, 700);
        editing->save(u"Another name"_s, editing->address(), editing->user(), u"another one"_s, editing->group());
        QTRY_COMPARE(slow.asked("/api/auth/login"), 1);
        makeSlowToSignIn(toAdd, 700);
        adding->add(toAdd.stand->address(), QString(), u"secret"_s, QString());
        QTRY_COMPARE(toAdd.asked("/api/auth/login"), 1);
        QVERIFY(editing->busy() && adding->busy());

        // The last window is closed: out of sight, and not gone.
        QVERIFY(!last->close());
        QVERIFY(!last->isVisible());
        QCOMPARE(desk.windows.count(), 1);
        // The screens of both boxes are told to close them, which empties what was typed; and
        // neither box shows or waits for anything any more.
        QCOMPARE(closeAskedOfTheOneThatChanges.count(), 1);
        QCOMPARE(closeAskedOfTheOneThatAdds.count(), 1);
        QCOMPARE(editing->machine(), QString());
        QVERIFY(!editing->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!adding->busy(), 400);
        QCOMPARE(adding->count(), 0);
        // And nothing is saved or added with nobody looking, when the KVMs answer at last.
        QTest::qWait(1100);
        QVERIFY(changed.isEmpty());
        QCOMPARE(kvmOf(desk, nas), before);
        QCOMPARE(desk.own->wall().kvms().size(), 1);
        QCOMPARE(desk.secrets.all.size(), 1);
        // Brought back, it is as a window whose boxes were never opened.
        desk.tray.click();
        QVERIFY(last->isVisible());
        QCOMPARE(editing->machine(), QString());
        QCOMPARE(adding->added(), QString());
    }

    void whatWasDoneAsTheLastWindowWentBehindTheIconIsSaidForWhenItComesBack()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}}));
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        core::Remembered behindTheIcon = desk.settings.remembered();
        behindTheIcon.staysInTray = true;
        QVERIFY(desk.settings.remember(behindTheIcon).ok());
        QVERIFY(desk.windows.openAll());
        const MadeUpKvm kvm;
        const MadeUpKvm toAdd(tls("comet"));
        const QString nas = put(desk, kvm, u"NAS"_s);
        ui::EditKvmView* editing = desk.windows.editing(0);
        ui::AddKvmView* adding = desk.windows.adding(0);
        ui::ActionsView* actions = desk.windows.actions(0);
        ui::Notices* notices = actions->notices();
        QQuickWindow* last = desk.windows.window(0);
        const QSignalSpy saved(editing, &ui::EditKvmView::doneAsItClosed);
        const QSignalSpy added(adding, &ui::AddKvmView::addedAsItClosed);
        // (A window of the program's has screens, which make a notice of what a box says as it
        // is closed. Here that is done by hand, as the screens do it.)
        connect(editing, &ui::EditKvmView::doneAsItClosed, this,
                [actions](const QString&, const QString& name) { actions->tellWhenSeen(name + u" saved"_s, true); });
        // (Seen is a window that is looked at: in sight, and the active one, which it is a
        // turn after it was shown. This asked at once, when seen was a window in sight.)
        QTRY_VERIFY(notices->isSeen());

        // Its box has saved a name: on the wall, and the box not told yet. The window is closed
        // in that turn, and goes behind the icon.
        desk.own->openFor(*editing, nas, false);
        editing->save(u"Storage"_s, editing->address(), editing->user(), QString(), editing->group());
        QVERIFY(editing->busy());
        QCOMPARE(kvmOf(desk, nas).name, u"Storage"_s);
        QVERIFY(!last->close());
        QVERIFY(!last->isVisible());
        // The box is closed and empty. What it had saved is said, to be a notice in that window:
        // one that nobody sees for now, and that stays.
        QCOMPARE(editing->machine(), QString());
        QVERIFY(!editing->busy());
        QCOMPARE(saved.count(), 1);
        QCOMPARE(saved.at(0).at(0).toString(), u"saved"_s);
        // (As a sentence calls it: set off.)
        QCOMPARE(saved.at(0).at(1).toString(), u"\"Storage\""_s);
        QVERIFY(!notices->isSeen());
        QCOMPARE(notices->count(), 1);
        QTest::qWait(300);
        // It is there when the window comes back, and has its time from then.
        desk.tray.click();
        QVERIFY(last->isVisible());
        QTRY_VERIFY(notices->isSeen());
        QCOMPARE(notices->count(), 1);
        QCOMPARE(notices->index(0, 0).data(ui::Notices::TextRole).toString(), u"\"Storage\" saved"_s);
        QCOMPARE(saved.count(), 1);

        // So with a KVM that was added as the window went, at the moment the wall says that it
        // has changed: what the box would have said of it, and the certificate with it.
        const auto atTheWallsWord =
            connect(&desk.own->wall(), &core::LocalWall::changed, last, [last] { last->close(); });
        adding->add(toAdd.stand->address(), QString(), u"secret"_s, QString());
        QTRY_COMPARE_WITH_TIMEOUT(added.count(), 1, 20000);
        disconnect(atTheWallsWord);
        QVERIFY(!last->isVisible());
        QVERIFY(added.at(0).at(0).toString().contains(u" added"_s));
        QCOMPARE(added.at(0).at(1).toString(), core::kvm::fingerprint(sha256Of("comet")));
        QCOMPARE(desk.own->wall().kvms().size(), 2);
        QCOMPARE(adding->count(), 0);
        QVERIFY(!adding->busy());
    }

    void whatAWindowsBoxAsksInTheTurnInWhichTheWindowGoesIsNotDone()
    {
        // A window that is not the last one is forgotten a turn of the loop after it was closed:
        // its boxes are there for that long.
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        QVERIFY(desk.windows.openAll());
        const MadeUpKvm kvm;
        const QString nas = put(desk, kvm, u"NAS"_s);
        ui::EditKvmView* editing = desk.windows.editing(1);
        desk.own->openFor(*editing, nas, false);
        QCOMPARE(editing->machine(), nas);
        const QSignalSpy changed(&desk.own->wall(), &core::LocalWall::changed);

        // Something is on its way to its box when the window is closed, as a key that was
        // pressed a moment before: it arrives in that turn, before the window is forgotten.
        QMetaObject::invokeMethod(
            editing,
            [editing] {
                editing->save(u"Renamed"_s, editing->address(), editing->user(), QString(), editing->group());
            },
            Qt::QueuedConnection);
        QVERIFY(desk.windows.window(1)->close());
        QCOMPARE(desk.windows.count(), 2);
        QTRY_COMPARE(desk.windows.count(), 1);
        QTest::qWait(300);
        QVERIFY(changed.isEmpty());
        QCOMPARE(kvmOf(desk, nas).name, u"NAS"_s);
    }

    void whatAWindowThatGoesForGoodStillHadToSayIsSaidInAnotherThatIsOpen()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        QVERIFY(desk.windows.openAll());
        const MadeUpKvm kvm;
        const QString nas = put(desk, kvm, u"NAS"_s);
        ui::EditKvmView* editing = desk.windows.editing(1);
        ui::ActionsView* goes = desk.windows.actions(1);
        ui::Notices* stays = desk.windows.actions(0)->notices();
        // (A window of the program's has screens, which make a notice of what a box says as it
        // is closed. Here that is done by hand, as the screens do it.)
        connect(editing, &ui::EditKvmView::doneAsItClosed, goes,
                [goes](const QString&, const QString& name) { goes->tellWhenSeen(name + u" saved"_s, true); });

        // The second window's box has saved a name: on the wall, and the box not told yet. The
        // window is closed in that turn. What its box had done is a notice of that window's,
        // which nobody sees any more.
        desk.own->openFor(*editing, nas, false);
        editing->save(u"Storage"_s, editing->address(), editing->user(), QString(), editing->group());
        QVERIFY(editing->busy());
        QCOMPARE(kvmOf(desk, nas).name, u"Storage"_s);
        QVERIFY(desk.windows.window(1)->close());
        QCOMPARE(desk.windows.count(), 2);
        QCOMPARE(goes->notices()->count(), 1);
        QCOMPARE(stays->count(), 0);
        // A turn later the window is gone for good: what it still had to say is said in the
        // window that is open.
        QTRY_COMPARE(desk.windows.count(), 1);
        QCOMPARE(stays->count(), 1);
        QCOMPARE(stays->index(0, 0).data(ui::Notices::TextRole).toString(), u"\"Storage\" saved"_s);
        QCOMPARE(stays->index(0, 0).data(ui::Notices::KindRole).toString(), u"ok"_s);
    }

    void aWindowsNoticesAreSeenWhileTheWindowIsLookedAtAndNotWhileItIsOnlyInSight()
    {
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}}));
        QVERIFY(desk.windows.openAll());
        ui::Notices* notices = desk.windows.actions(0)->notices();
        QQuickWindow* last = desk.windows.window(0);
        last->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(last));
        QTRY_VERIFY(notices->isSeen());

        // Another window is the active one: this one is in sight, and nobody looks at it. What
        // is told for when it is seen waits, and has its time from when it is looked at.
        QWindow elsewhere;
        elsewhere.resize(200, 100);
        elsewhere.show();
        elsewhere.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&elsewhere));
        QVERIFY(last->isVisible());
        QTRY_VERIFY(!notices->isSeen());
        last->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(last));
        QTRY_VERIFY(notices->isSeen());
        // Nor is one that is made small looked at.
        last->showMinimized();
        QTRY_COMPARE(last->visibility(), QWindow::Minimized);
        QTRY_VERIFY(!notices->isSeen());
        last->showNormal();
        last->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(last));
        QTRY_VERIFY(notices->isSeen());
    }

    void whatABoxSaysAsItsWindowGoesOutOfSightIsSaidToAWindowThatNobodySees()
    {
        // What a box had done is said as the box is closed, and is a notice that waits only
        // where its window is known to be out of sight by then: that is said first.
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}}));
        desk.own = std::make_unique<ui::OwnWall>(desk.settings, desk.secrets, quick());
        core::Remembered behindTheIcon = desk.settings.remembered();
        behindTheIcon.staysInTray = true;
        QVERIFY(desk.settings.remember(behindTheIcon).ok());
        QVERIFY(desk.windows.openAll());
        const MadeUpKvm kvm;
        const QString nas = put(desk, kvm, u"NAS"_s);
        ui::EditKvmView* editing = desk.windows.editing(0);
        ui::Notices* notices = desk.windows.actions(0)->notices();
        QQuickWindow* last = desk.windows.window(0);
        last->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(last));
        QTRY_VERIFY(notices->isSeen());
        QList<bool> seenWhenItWasSaid;
        connect(editing, &ui::EditKvmView::doneAsItClosed, this,
                [&seenWhenItWasSaid, notices] { seenWhenItWasSaid.append(notices->isSeen()); });

        desk.own->openFor(*editing, nas, false);
        editing->save(u"Storage"_s, editing->address(), editing->user(), QString(), editing->group());
        QVERIFY(editing->busy());
        QVERIFY(!last->close());
        QVERIFY(!last->isVisible());
        QCOMPARE(seenWhenItWasSaid, QList<bool>{false});
    }

    void keyboardAndMouseOfAMachineAreEachWindowsOwn()
    {
        // A QKVM that has its machines live, and gives a pass for a line to whoever asks.
        const QByteArray status = R"({"a": {"state": "online"}, "b": {"state": "online"}})";
        StandIn server([&status](const Asked& asked) {
            if (asked.path == "/api/v1/wall") {
                return Answered{200, R"({"live": 2, "interval": 0.25, "devices": [
                    {"id": "a", "name": "Build server", "host": "192.168.1.21", "kind": "kvmd", "picture": true},
                    {"id": "b", "name": "NAS", "host": "192.168.1.22", "kind": "kvmd", "picture": true}]})"};
            }
            if (asked.method == "POST" && asked.path.endsWith("/live")) {
                return Answered{200, R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})"};
            }
            return signedIn(asked, status);
        });
        const auto says = [](bool on) { return QJsonObject{{u"t"_s, u"control"_s}, {u"on"_s, on}}; };
        const input::Pressed keyA{0x1E, false, 0x41};
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        desk.session.retryLiveAt({10, 150, 600, 5, 20});
        QVERIFY(desk.windows.openAll());
        // Each window has its own view of it, and keys of its own.
        ui::ControlView* first = desk.windows.control(0);
        ui::ControlView* second = desk.windows.control(1);
        QVERIFY(first != nullptr && second != nullptr && first != second);
        QCOMPARE(desk.windows.control(2), nullptr);
        QCOMPARE(desk.windows.control(-1), nullptr);
        QCOMPARE(desk.keySources.size(), 2);
        QVERIFY(desk.keySources[0] != desk.keySources[1]);
        // Each made for its own window, which Windows' own way asks after as keys go down.
        QCOMPARE(desk.keyWindows, (QList<QPointer<QWindow>>{desk.windows.window(0), desk.windows.window(1)}));
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.all.machines()->rowCount(), 2);
        // On the wall there is nothing to take, in either.
        QVERIFY(!first->isPossible());
        QVERIFY(!second->isPossible());

        // The second window shows a machine large, and has it live: its keyboard and mouse can
        // be taken there, and nowhere else.
        desk.windows.wall(1)->setEnlargedPixels(1000);
        desk.windows.wall(1)->enlarge(desk.key(u"a"_s));
        QTRY_VERIFY(second->isPossible());
        QVERIFY(!first->isPossible());
        // Not from a window that does not have the keyboard.
        desk.windows.window(0)->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.windows.window(0)));
        second->take();
        QVERIFY(!second->isAsking());
        QTest::qWait(100);
        QVERIFY(server.live.saidOf(u"control"_s).isEmpty());
        // From the one that has it.
        desk.windows.window(1)->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.windows.window(1)));
        second->take();
        QVERIFY(second->isAsking());
        QVERIFY(!first->isAsking());
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 1);
        server.live.say(says(true));
        QTRY_VERIFY(second->isOn());
        QVERIFY(!first->isOn());
        // That window's keys are listened for and swallowed, the other's are not; and only what
        // is pressed there goes to the machine.
        QVERIFY(desk.keySources[1]->listens && desk.keySources[1]->swallows());
        QVERIFY(!desk.keySources[0]->listens && !desk.keySources[0]->swallows());
        emit desk.keySources[0]->keyChanged(keyA, true, true);
        desk.keySources[1]->press(keyA);
        QTRY_COMPARE(server.live.saidOf(u"k"_s).size(), 1);
        QCOMPARE(server.live.saidOf(u"k"_s).at(0)[u"u"_s].toInt(), 0x04);
        QVERIFY(server.live.saidOf(u"k"_s).at(0)[u"d"_s].toBool());

        // The other window comes to the front: this one has the keyboard no longer, and gives
        // the machine's back, letting go first of the key that is down there.
        QVERIFY(server.live.saidOf(u"up"_s).isEmpty());
        desk.windows.window(0)->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.windows.window(0)));
        QTRY_VERIFY(!second->isOn());
        // (Nothing is swallowed any more. The finger is still on A, which Windows never saw go
        // down: that window's keys are listened for until it was let go, and no longer. This
        // looked for the listening to end with the giving back.)
        QVERIFY(!desk.keySources[1]->swallows());
        QVERIFY(desk.keySources[1]->listens);
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 2);
        QVERIFY(!server.live.saidOf(u"control"_s).at(1)[u"on"_s].toBool());
        QVERIFY(!server.live.saidOf(u"up"_s).isEmpty());
        qsizetype letGoAt = -1;
        qsizetype givenBackAt = -1;
        for (qsizetype at = 0; at < server.live.said.size(); ++at) {
            const QJsonObject& word = server.live.said.at(at);
            if (word[u"t"_s].toString() == u"up"_s && letGoAt < 0) {
                letGoAt = at;
            }
            if (word[u"t"_s].toString() == u"control"_s && !word[u"on"_s].toBool()) {
                givenBackAt = at;
            }
        }
        QVERIFY(letGoAt >= 0 && letGoAt < givenBackAt);
        server.live.say(says(false));
        QTest::qWait(100);
        QCOMPARE(second->problem(), QString());
        // (The finger comes off the key: that is nothing to the machine, and the listening ends.)
        desk.keySources[1]->release(keyA);
        QCOMPARE(server.live.saidOf(u"k"_s).size(), 1);
        QVERIFY(!desk.keySources[1]->listens);

        // Taken again there, and the window is closed over it: they go with the window, and
        // what was down on the machine is let go.
        desk.windows.window(1)->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.windows.window(1)));
        second->take();
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 3);
        server.live.say(says(true));
        QTRY_VERIFY(second->isOn());
        desk.keySources[1]->press(keyA);
        QTRY_COMPARE(server.live.saidOf(u"k"_s).size(), 2);
        const qsizetype letGoBefore = server.live.saidOf(u"up"_s).size();
        QVERIFY(desk.windows.window(1)->close());
        QTRY_COMPARE(desk.windows.count(), 1);
        QTRY_VERIFY(server.live.saidOf(u"up"_s).size() > letGoBefore);
        QVERIFY(desk.keySources[1] == nullptr);
        QVERIFY(!first->isOn());
        QVERIFY(!desk.keySources[0]->listens);
        // A window that comes later has its own as well.
        QVERIFY(desk.windows.openOne());
        QCOMPARE(desk.keySources.size(), 3);
        QVERIFY(desk.windows.control(1) != nullptr && desk.windows.control(1) != first);
        QVERIFY(!desk.windows.control(1)->isPossible());
    }

    void whatIsHeldIsKnownToEveryWindowOfTheProgram()
    {
        const QByteArray status = R"({"a": {"state": "online"}, "b": {"state": "online"}})";
        StandIn server([&status](const Asked& asked) {
            if (asked.path == "/api/v1/wall") {
                return Answered{200, R"({"live": 2, "interval": 0.25, "devices": [
                    {"id": "a", "name": "Build server", "host": "192.168.1.21", "kind": "kvmd", "picture": true},
                    {"id": "b", "name": "NAS", "host": "192.168.1.22", "kind": "kvmd", "picture": true}]})"};
            }
            if (asked.method == "POST" && asked.path.endsWith("/live")) {
                return Answered{200, R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})"};
            }
            return signedIn(asked, status);
        });
        const auto says = [](bool on) { return QJsonObject{{u"t"_s, u"control"_s}, {u"on"_s, on}}; };
        const input::Pressed keyC{0x2E, false, 0x43};
        const input::Pressed leftCtrl{0x1D, false, 0xA2};
        const QTemporaryDir folder;
        Desk desk(fileWith(folder, {core::WallWindow{}, core::WallWindow{}}));
        desk.session.retryLiveAt({10, 150, 600, 5, 20});
        QVERIFY(desk.windows.openAll());
        ui::ControlView* first = desk.windows.control(0);
        ui::ControlView* second = desk.windows.control(1);
        desk.view.connectTo(server.address());
        QTRY_COMPARE(desk.all.machines()->rowCount(), 2);

        // The second window shows a machine large and takes its keyboard and mouse. Ctrl is
        // pressed there: it goes to the machine, and Windows never sees it go down.
        desk.windows.wall(1)->setEnlargedPixels(1000);
        desk.windows.wall(1)->enlarge(desk.key(u"a"_s));
        QTRY_VERIFY(second->isPossible());
        desk.windows.window(1)->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.windows.window(1)));
        second->take();
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 1);
        server.live.say(says(true));
        QTRY_VERIFY(second->isOn());
        desk.keySources[1]->press(leftCtrl);
        QTRY_COMPARE(server.live.saidOf(u"k"_s).size(), 1);

        // The first window shows another machine large. A click into it: the second has the
        // keyboard no longer, and gives the machine's back, with the finger still on Ctrl.
        desk.windows.wall(0)->setEnlargedPixels(1000);
        desk.windows.wall(0)->enlarge(desk.key(u"b"_s));
        QTRY_VERIFY(first->isPossible());
        desk.windows.window(0)->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(desk.windows.window(0)));
        QTRY_VERIFY(!second->isOn());
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 2);
        QVERIFY(desk.keySources[1]->listens);
        // "Control" in the first. Its keys never heard Ctrl go down, and Windows does not know
        // of it: C is no "c" on the machine all the same, and the bar says why nothing arrives.
        first->take();
        QTRY_COMPARE(server.live.saidOf(u"control"_s).size(), 3);
        server.live.say(says(true));
        QTRY_VERIFY(first->isOn());
        const qsizetype keysBefore = server.live.saidOf(u"k"_s).size();
        desk.keySources[0]->press(keyC);
        desk.keySources[0]->release(keyC);
        QTest::qWait(200);
        QCOMPARE(server.live.saidOf(u"k"_s).size(), keysBefore);
        QVERIFY(first->note().contains(u"Left Ctrl"_s));
        // Ctrl is let go. The keys of the window that swallows hear that, and keep it from the
        // other's: it is known to both, and the second window's keys are waited for no longer.
        emit desk.keySources[0]->keyChanged(leftCtrl, false, true);
        QVERIFY(!desk.keySources[1]->listens);
        QCOMPARE(first->note(), QString());
        desk.keySources[0]->press(keyC);
        QTRY_COMPARE(server.live.saidOf(u"k"_s).size(), keysBefore + 1);
        QCOMPARE(server.live.saidOf(u"k"_s).constLast()[u"u"_s].toInt(), 0x06);
    }
};

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    // (Closing the last window of a test is not the end of the tests.)
    QGuiApplication::setQuitOnLastWindowClosed(false);
    TestWallWindows test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_wall_windows.moc"
