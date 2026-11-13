// JetKVMs by hand for measuring (M6 2i): so many on this PC's loopback, each on a port of its own, each the tests'
// JetKVM by hand with its one viewer (core/tests/jetkvm_by_hand.h, jetkvm_viewer_by_hand.h), sending the film
// jetkvm-1080p60.h264 (the desktop, 1080p60, a key frame every 30 pictures, 8000k) from a thread of its own, and
// answering the status questions as the real one does. For the program's own window (qkvm-session-rig, "own")
// with a wall of such JetKVMs (tools/measure_live.ps1 -JetKvm), and for qkvm-jetkvm-rig.
//
//     qkvm-jetkvm-stand-in [--count N] [--first-picture-after <ms>] [--seconds S]
//
// Says each one's address as a line "jetkvm: http://127.0.0.1:<port>", then "ready", and runs for so many seconds
// (0, as by default: until it is ended). Their password is "secret", as the tests' (no real one's). With
// --first-picture-after, each connection's first picture comes so long after its offer (a JetKVM that rested:
// 5500 ms on a real one after a minute, 15000 after a long rest).

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QTimer>

#include "jetkvm_by_hand.h"
#include "jetkvm_viewer_by_hand.h"

using namespace Qt::StringLiterals;
using qkvm::core::testing::JetKvmByHand;
using qkvm::core::testing::JetKvmViewerByHand;

namespace {
struct One {
    std::unique_ptr<JetKvmByHand> jet;
    std::unique_ptr<JetKvmViewerByHand> viewer;
};
int run(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    int count = 1;
    int firstPictureAfterMs = 0;
    int seconds = 0;
    for (qsizetype at = 1; at < args.size(); ++at) {
        if (args.at(at) == u"--count"_s && at + 1 < args.size()) {
            count = std::clamp(args.at(++at).toInt(), 1, 128);
        } else if (args.at(at) == u"--first-picture-after"_s && at + 1 < args.size()) {
            firstPictureAfterMs = std::max(0, args.at(++at).toInt());
        } else if (args.at(at) == u"--seconds"_s && at + 1 < args.size()) {
            seconds = std::max(0, args.at(++at).toInt());
        } else {
            std::fprintf(stderr, "not understood: %s\n", qPrintable(args.at(at)));
            return 2;
        }
    }
    if (!qkvm::core::testing::JetKvmPeerByHand::filmIsThere()) {
        std::fprintf(stderr, "%s\n", qkvm::core::testing::JetKvmPeerByHand::noFilm);
        return 2;
    }
    std::vector<One> all;
    all.reserve(static_cast<std::size_t>(count));
    for (int each = 0; each < count; ++each) {
        One one;
        one.jet = std::make_unique<JetKvmByHand>();
        one.viewer = std::make_unique<JetKvmViewerByHand>(*one.jet);
        one.viewer->firstPictureAfterMs = firstPictureAfterMs;
        std::printf("jetkvm: %s\n", qPrintable(one.jet->address().toString()));
        all.push_back(std::move(one));
    }
    std::printf("ready\n");
    std::fflush(stdout);
    if (seconds > 0) {
        QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    }
    const int code = QCoreApplication::exec();
    // (Each viewer before its JetKVM: its peers stop before the line they answered on goes.)
    for (One& one : all) {
        one.viewer.reset();
    }
    all.clear();
    return code;
}

} // namespace

int main(int argc, char* argv[])
{
    // (What can throw is memory running out, or a port that cannot be had: said, and the end.)
    try {
        return run(argc, argv);
    } catch (...) {
        std::fputs("the stand-in could not go on\n", stderr);
        return 1;
    }
}
