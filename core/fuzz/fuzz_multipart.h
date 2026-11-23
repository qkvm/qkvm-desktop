#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/multipart_pictures.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

// The harness of a kvmd KVM's stream of pictures, "multipart/x-mixed-replace" as ustreamer sends it
// (fuzz_multipart.cpp, libFuzzer's way in; test_core_fuzz_found, what it once found): multipart_pictures.cpp and the
// reader of a picture's stated size are compiled into the fuzz target. An input is one byte that says the size of the
// pieces the stream is fed in (1 to 255, or whole where 0), then the answer's Content-Type up to a line feed, then the
// stream. Held to the reader's promises: it never takes more than it is given, nothing once its reading is over, no
// picture longer than it takes as one, and it holds no more room than a part and a picture.
//
// Its canary (fuzz_promise.h): three pictures handed out of one stream, each part's lines read and its stated length
// taken whole (each seed carries two at the most).
namespace qkvm::fuzz {

inline void readMultipart(std::span<const std::uint8_t> input)
{
    using core::MultipartPictures;
    if (input.empty()) {
        return;
    }
    const qsizetype step = input[0] == 0 ? static_cast<qsizetype>(input.size()) : qsizetype{input[0]};
    const QByteArrayView rest(reinterpret_cast<const char*>(input.data()) + 1,
                              static_cast<qsizetype>(input.size() - 1));
    const qsizetype end = rest.indexOf('\n');
    if (end < 0) {
        return;
    }
    const std::optional<QByteArray> boundary = core::boundaryFrom(rest.first(end));
    if (!boundary) {
        return;
    }
    promise(!boundary->isEmpty() && boundary->size() <= 70, "a boundary that RFC 2046 does not allow");
    MultipartPictures::Limits limits;
    limits.pictureBytes = qsizetype{1} << 20;
    MultipartPictures pictures(*boundary, limits);
    QByteArrayView stream = rest.sliced(end + 1);
    int shown = 0;
    while (!stream.isEmpty()) {
        const QByteArrayView piece = stream.first(std::min(step, stream.size()));
        const MultipartPictures::End before = pictures.end();
        const qsizetype taken = pictures.feed(piece);
        promise(taken >= 0 && taken <= piece.size(), "it took more than it was given");
        promise(before == MultipartPictures::End::None || taken == 0, "it took more once its reading was over");
        promise(pictures.room() <= limits.headerBytes + 2 * limits.pictureBytes + 4096,
                "it holds more than a part and a picture");
        if (const std::optional<QByteArray> picture = pictures.next()) {
            promise(picture->size() <= limits.pictureBytes, "a picture longer than one is taken");
            static_cast<void>(core::kvmd::pictureSize(*picture));
            if (++shown == 3) {
                reached("three pictures handed out of one stream, each part read whole");
            }
        }
        if (pictures.end() != MultipartPictures::End::None) {
            break;
        }
        if (taken == 0 && !pictures.next()) {
            // (Nothing taken, no picture waiting and the reading not over: it would never go on.)
            promise(piece.isEmpty(), "it took nothing and has nothing to hand out");
        }
        stream = stream.sliced(taken);
    }
}

} // namespace qkvm::fuzz
