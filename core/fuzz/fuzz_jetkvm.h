#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QList>
#include <QNetworkCookie>
#include <QString>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/core/kvmd_answers.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../src/jetkvm_words.h"
#include "../src/rtc_sdp.h"

// The harness of what a JetKVM says (fuzz_jetkvm.cpp, libFuzzer's way in; test_core_fuzz_found, what it once found):
// jetkvm_words.cpp, which its connection and its driver read it with (on its signalling line, on `rpc` and `hidrpc`,
// and its answers to the driver), compiled into the fuzz target with the readers behind it (the program's own JSON
// reader, rtc_sdp.cpp for its answer's description and its addresses). An input is words as they come, each one byte
// for where it came (its value modulo 8: 0 the signalling line, 1 `rpc`, 2 `hidrpc`, 3 GET /device, 4 GET
// /device/status, 5 the first word of the line a driver identifies it on, 6 a sign-in's Set-Cookie lines, 7 a 429's
// Retry-After), its length in two bytes (big-endian), and its bytes; the signalling line taken as the connection takes
// it, an answer once and then no more. Held to the readers' promises: its texts one short line, what it says of its
// input within its bounds, its keyboard of the keys there are, a pass one that can go back in a header, a wait within
// its most.
//
// Its canary (fuzz_promise.h): its answer read from the signalling line through its base64 and JSON to a description
// whose video was chosen, and then one of its addresses read on that line and judged sound (no seed has both).
namespace qkvm::fuzz {

namespace jetkvms {

inline void line(const QString& said, qsizetype limit)
{
    promise(said.size() <= limit, "a text of the JetKVM's is longer than its line");
    for (const QChar one : said) {
        promise(one == u' ' || (one.category() != QChar::Other_Control && one.category() != QChar::Separator_Line &&
                                one.category() != QChar::Separator_Paragraph),
                "a text of the JetKVM's holds what does not print");
    }
}

inline void keys(int modifiers, const QList<int>& keys, qsizetype most)
{
    promise(modifiers >= 0 && modifiers <= 255, "a JetKVM's modifiers that are no byte");
    promise(keys.size() <= most, "more keys down than a report holds");
    for (const int key : keys) {
        promise(key >= 1 && key <= 255, "a key down that is none");
    }
}

} // namespace jetkvms

inline void readJetKvm(std::span<const std::uint8_t> input)
{
    namespace words = core::jetkvm_words;
    std::span<const std::uint8_t> rest = input;
    bool answered = false;
    bool chosen = false; // its answer's video chosen
    for (int count = 0; count < 64 && rest.size() >= 3; ++count) {
        const int where = rest[0] % 8;
        const std::size_t length = std::min<std::size_t>((std::size_t{rest[1]} << 8) | rest[2], rest.size() - 3);
        const QByteArray said(reinterpret_cast<const char*>(rest.data()) + 3, static_cast<qsizetype>(length));
        rest = rest.subspan(3 + length);
        switch (where) {
        case 0: {
            const words::Signalling read = words::signalling(said, answered);
            if (read.kind == words::Signalling::Kind::Refused) {
                jetkvms::line(read.refusal, 200);
            } else if (read.kind == words::Signalling::Kind::Answer) {
                const std::optional<QByteArray> sdp = words::answerSdp(read.data);
                if (!sdp) {
                    break;
                }
                promise(!sdp->isEmpty(), "an answer of no description");
                // (As the offering side takes it: its video, its application, its addresses each judged.)
                answered = true;
                chosen = core::chooseVideo(*sdp, {}).has_value();
                static_cast<void>(core::applicationMid(*sdp));
                for (const QByteArray& candidate : core::withoutAddresses(*sdp).candidates) {
                    static_cast<void>(core::judgeAddress(candidate, false));
                }
            } else if (read.kind == words::Signalling::Kind::Address) {
                const core::JudgedAddress judged = core::judgeAddress(read.address, false);
                if (judged.fault == core::AddressFault::None && chosen) {
                    reached("an answer read through its base64 and JSON to a video chosen, and then a sound address");
                }
            }
            break;
        }
        case 1: {
            const words::Rpc read = words::rpc(said);
            static_cast<void>(words::otherSessionConnected(said));
            switch (read.kind) {
            case words::Rpc::Kind::Answer:
                promise(read.id >= 1 && read.id <= 9'000'000'000'000'000, "an answer to a number that is none");
                if (read.refused) {
                    jetkvms::line(read.words, 200);
                }
                break;
            case words::Rpc::Kind::Video:
                promise(read.width >= 0 && read.width <= 16384 && read.height >= 0 && read.height <= 16384 &&
                            std::isfinite(read.fps) && read.fps >= 0 && read.fps <= 1000,
                        "a JetKVM's input said beyond its bounds");
                break;
            case words::Rpc::Kind::Usb:
            case words::Rpc::Kind::Version:
                jetkvms::line(read.words, 40);
                break;
            case words::Rpc::Kind::Keys:
                jetkvms::keys(read.modifiers, read.keys, 16);
                break;
            default:
                break;
            }
            break;
        }
        case 2: {
            const words::Hid read = words::hid(said);
            if (read.kind == words::Hid::Kind::Keys) {
                jetkvms::keys(read.modifiers, read.keys, 6);
            }
            break;
        }
        case 3: {
            const QString number = words::numberFrom(said);
            promise(number.isEmpty() || (number.startsWith(u"jetkvm:") && number.size() == 39),
                    "a JetKVM's number that is no mark");
            break;
        }
        case 4:
            static_cast<void>(words::saysNotSetUp(said));
            break;
        case 5:
            if (const std::optional<QString> version = words::metadataVersion(said)) {
                jetkvms::line(*version, 40);
            }
            break;
        case 6: {
            const QByteArray pass = words::passFrom(QNetworkCookie::parseCookies(said));
            promise(pass.isEmpty() || core::kvmd::isToken(pass), "a pass that cannot go back in a header");
            break;
        }
        default: {
            const qint64 wait = words::comeBackAfterMs(said, 300'000);
            promise(wait >= 0 && wait <= 300'000, "a wait beyond its most");
            break;
        }
        }
    }
}

} // namespace qkvm::fuzz
