#pragma once

#include <QChar>
#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/bmc/aspeed.h>
#include <qkvm/core/wiped.h>
#include <qkvm/rfb/rfb.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "../src/jpeg_pixels.h"
#include "../src/rfb_reader.h"

// The harness of a VNC server's session as this program reads it (fuzz_vnc.cpp, libFuzzer's way in;
// test_core_fuzz_found, what it once found): the RFB reader behind the session's own reader (RfbReader, on a thread of
// Qt's pool, as a VNC server's and a controller's console's screen has it), VNC authentication allowed and a password
// held, as a VNC server of the wall has them (RfbScreen), so that the server's greeting, its ways of signing in, its
// challenge and what it says of the answer, its name and its screen are all read as they come. An input is one byte
// that says where the stream is cut in two (its share of 127 of the stream; with 128, a card's still is asked between
// the two), then what the server sends. Held to its promises: the name it gives itself one short line of what can be
// shown, the password let go of once it has answered or the session is over, and the screen handed over whole.
//
// Its canary (fuzz_promise.h): a session signed in by VNC authentication (the challenge answered with the password, and
// the answer taken) onto a screen wider or taller than sixteen, into which an update then drew, the session not over
// (every seed's screen is of sixteen by sixteen).
namespace qkvm::fuzz {

namespace vncs {

// The reader posts that something was read to its home's thread: an application there for it, once.
inline void haveAnApplication()
{
    if (QCoreApplication::instance() == nullptr) {
        static int count = 1;
        static char name[] = "fuzz_vnc";
        static char* words[] = {name, nullptr};
        static QCoreApplication application(count, words);
    }
}

} // namespace vncs

inline void readVnc(std::span<const std::uint8_t> input)
{
    if (input.empty()) {
        return;
    }
    vncs::haveAnApplication();
    const std::uint8_t how = input[0];
    const std::span<const std::uint8_t> stream = input.subspan(1);
    const std::size_t cut = stream.size() * (how & 0x7F) / 127;
    auto wall = std::make_shared<std::atomic<std::int64_t>>(0);
    auto work = std::make_shared<std::atomic<std::int64_t>>(0);
    auto pixels = std::make_shared<core::JpegPixels>();
    rfb::Options options;
    options.signsIn = {rfb::securityNone, rfb::securityVnc};
    options.jpeg = [pixels](std::span<const std::uint8_t> jpeg, int width, int height) {
        return pixels->decode(jpeg, width, height);
    };
    options.clock = [wall] { return ++*wall; };
    options.workClock = [work] { return ++*work; };
    QString password = QStringLiteral("a password of the fuzz target's");
    {
        QObject home;
        const auto reader =
            std::make_shared<core::RfbReader>(&home, [] {}, std::move(options), core::SecretText::takenFrom(password));
        const auto bytesOf = [](std::span<const std::uint8_t> part) {
            return QByteArray(reinterpret_cast<const char*>(part.data()), static_cast<qsizetype>(part.size()));
        };
        reader->feed(bytesOf(stream.first(cut)));
        if ((how & 0x80) != 0) {
            reader->still(320, 1);
        }
        reader->feed(bytesOf(stream.subspan(cut)));
        QThreadPool::globalInstance()->waitForDone();
        bmc::aspeed::Picture screen;
        const core::RfbReader::Taken taken = reader->take(screen);
        promise(taken.name.size() <= 80, "a server's name is longer than a line");
        for (const QChar one : taken.name) {
            promise(one == u' ' || one.isPrint() || one.isSurrogate(), "a server's name holds what cannot be shown");
        }
        // (Cut at its line's end once made one line, as the web's plain cuts what it joined: a blank may end it.)
        const QString uncut = taken.name.endsWith(u' ') ? taken.name.chopped(1) : taken.name;
        promise(uncut == uncut.simplified(), "a server's name holds blanks that are not one blank");
        promise(!reader->holdsAPassword() || (!taken.ready && !taken.ended && !taken.passwordAnswered),
                "the password is still held after it answered or the session was over");
        promise(screen.rgb.size() ==
                    std::size_t{3} * static_cast<std::size_t>(screen.width) * static_cast<std::size_t>(screen.height),
                "the screen was handed over in part");
        for (const bmc::aspeed::Rect& changed : taken.changed) {
            promise(changed.x >= 0 && changed.y >= 0 && changed.width >= 0 && changed.height >= 0 &&
                        changed.x + changed.width <= screen.width && changed.y + changed.height <= screen.height,
                    "a change was told beyond the screen");
        }
        if (taken.passwordAnswered && taken.ready && !taken.ended && (taken.width > 16 || taken.height > 16) &&
            taken.frames >= 1) {
            reached("a session signed in by VNC authentication onto a screen larger than the seeds', an update drawn");
        }
        reader->detach();
    }
}

} // namespace qkvm::fuzz
