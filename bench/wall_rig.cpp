// The engine underneath Qt Quick (SPEC.md, step M1, the sixth part): the program's own window and
// its own QML, with a wall of made-up machines whose pictures are the clips tools/make_testdata.ps1
// makes. What the bare rig shows of the engine alone, this shows of the engine with an interface
// drawn over it.
//
//   qkvm-wall-rig <folder of the made pictures> [seconds to run; 0 or nothing: until closed]

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>

#include <qkvm/engine/gpu.h>
#include <qkvm/engine/scene.h>

#include <qkvm/core/settings.h>

#include "about_model.h"
#include "actions_view.h"
#include "add_kvm_view.h"
#include "application.h"
#include "control_view.h"
#include "edit_kvm_view.h"
#include "feeder.h"
#include "files_view.h"
#include "look.h"
#include "own_files_view.h"
#include "own_kvms_view.h"
#include "pretend_start.h"
#include "pretend_wall.h"
#include "session_view.h"
#include "settings_view.h"
#include "video_host.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// What the made-up machines show, in turn: mostly the still desktop as small pictures twice a
// second, which is what a wall is made of, and two machines live.
const char* const sources[] = {
    "desktop-preview-640-422", "desktop-720p15.h264",  "desktop-preview-480",
    "pattern-preview-480",     "desktop-1080p30.h264",
};

int run(int argc, char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    ui::prepareApplication();
    if (argc < 2) {
        std::fputs("qkvm-wall-rig <folder of the made pictures> [seconds]\n", stderr);
        return 2;
    }
    const std::filesystem::path data = argv[1];
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 0;

    base::Result<std::unique_ptr<engine::Gpu>> made = engine::Gpu::create({});
    if (!made) {
        std::fprintf(stderr, "%s\n", made.error().message.c_str());
        return 1;
    }
    const std::unique_ptr<engine::Gpu> gpu = std::move(made).value();

    std::map<std::string, bench::Clip> clips;
    for (const char* name : sources) {
        base::Result<bench::Clip> clip = bench::readClip(data, name);
        if (!clip) {
            std::fprintf(stderr, "%s\n", clip.error().message.c_str());
            return 1;
        }
        clips.emplace(name, std::move(clip).value());
    }

    ui::AboutModel about;
    ui::WallView wall;
    wall.machines()->setTiles(ui::pretendWall());
    // As if somebody were signed in: the window shows its wall.
    ui::SessionView session;
    ui::SessionView::Shown signedIn;
    signedIn.stage = u"wall"_s;
    signedIn.address = u"192.168.1.10:8787"_s;
    signedIn.account = u"rig"_s;
    session.show(signedIn);
    // Nobody does anything to a made-up machine: the rig is for looking.
    ui::ActionsView actions;

    // The rig looks as the program does when nothing was chosen, and remembers no other choice.
    const QTemporaryDir lookFolder;
    core::Settings lookSettings(lookFolder.filePath(u"settings.json"_s));
    ui::Look look(lookSettings);
    // Whether it starts with Windows is only pretended: the rig is never on Windows' list.
    ui::PretendedStart start;
    ui::SettingsView ownSettings(start, lookSettings);
    ui::AddKvmView adding;
    ui::EditKvmView editing;
    // Nobody takes the keyboard and mouse of a made-up machine either.
    ui::ControlView control;
    // A server's wall: no KVMs of this PC's are listed in its settings.
    ui::OwnKvmsView kvms;
    // Nor is a wall written to a file, or read from one: the boxes for that are there, with nobody
    // behind them (a made-up server writes no file, and this PC's wall is not shown).
    ui::FilesView files;
    ui::OwnFilesView ownFiles;
    QQmlApplicationEngine engine;
    ui::keepOffTheNetwork(engine);
    engine.setInitialProperties({{u"about"_s, QVariant::fromValue(&about)},
                                 {u"session"_s, QVariant::fromValue(&session)},
                                 {u"wall"_s, QVariant::fromValue(&wall)},
                                 {u"actions"_s, QVariant::fromValue(&actions)},
                                 {u"look"_s, QVariant::fromValue(&look)},
                                 {u"settings"_s, QVariant::fromValue(&ownSettings)},
                                 {u"files"_s, QVariant::fromValue(&files)},
                                 {u"ownFiles"_s, QVariant::fromValue(&ownFiles)},
                                 {u"adding"_s, QVariant::fromValue(&adding)},
                                 {u"editing"_s, QVariant::fromValue(&editing)},
                                 {u"control"_s, QVariant::fromValue(&control)},
                                 {u"kvms"_s, QVariant::fromValue(&kvms)}});
    engine.loadFromModule("Qkvm", "Main");
    auto* window =
        engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (window == nullptr) {
        std::fputs("The window could not be made.\n", stderr);
        return 1;
    }

    const std::unique_ptr<ui::VideoHost> video = std::make_unique<ui::VideoHost>(*gpu, window);
    std::vector<bench::Player> players;
    for (int stream = 1; stream <= ui::pretendWallPictured(); ++stream) {
        const bench::Clip& clip = clips.at(sources[static_cast<std::size_t>(stream - 1) % std::size(sources)]);
        engine::Stream& added = video->scene().add(static_cast<engine::StreamId>(stream), clip.codec);
        // Pictures decoded for a card on a wall, not for one machine shown large.
        added.shownAt({}, engine::Importance::Preview);
        // Not all from their first picture: the pointers of two machines are not in the same place.
        players.push_back({&clip, &added, static_cast<std::size_t>(stream) * 3 % clip.pictures()});
    }
    // A stream of H.264 begins at a key frame.
    for (bench::Player& player : players) {
        while (player.clip->codec == engine::Codec::H264 && !player.clip->cuts[player.next].keyFrame) {
            player.next = (player.next + 1) % player.clip->pictures();
        }
    }
    const bench::Feeder feeder(std::move(players));

    window->show();
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
        std::fprintf(stderr, "qkvm-wall-rig ended: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("qkvm-wall-rig ended on something that was thrown.\n", stderr);
        return 1;
    }
}
