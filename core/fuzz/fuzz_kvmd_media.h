#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/core/kvmd_media.h>
#include <qkvm/core/server_types.h>

#include <cstdint>
#include <optional>
#include <span>

// The harness of what a KVM that runs kvmd says of its virtual media, its drive and its storage (fuzz_kvmd_media.cpp,
// libFuzzer's way in; test_core_fuzz_found, what it once found): kvmd_media_answers.cpp and what it reads with are
// compiled into the fuzz target. An input is messages on the KVM's line for events, one to a line (a line feed between
// them), each of which is merged into what is known as the driver merges kvmd's `msd` events, and every reader of the
// drive's state is given what is known after each; and each is an answer to a write and a refusal as well. Held to the
// readers' promises: no more images than are listed, each by a name the program hands back, sizes that are sizes.
//
// Its canary (fuzz_promise.h): what two or more `msd` events merged into says of an image on the drive (mounted) beside
// two or more images listed in the storage (the seeds' second event takes the image out).
namespace qkvm::fuzz {

inline void readKvmdMedia(std::span<const std::uint8_t> input)
{
    namespace kvmd = core::kvmd;
    const QByteArray all(reinterpret_cast<const char*>(input.data()), static_cast<qsizetype>(input.size()));
    const QString kvm = QStringLiteral("https://kvm.fuzz");
    kvmd::MediaNotes notes;
    std::optional<QJsonObject> known;
    int lines = 0;
    int merged = 0;
    for (const QByteArray& bytes : all.split('\n')) {
        if (++lines > 64) {
            break;
        }
        static_cast<void>(kvmd::writtenOf(bytes));
        promise(kvmd::errorOf(bytes).size() <= 200, "a KVM's words are longer than their line");
        static_cast<void>(kvmd::msdRefusal(409, bytes, QStringLiteral("connect the image")));
        static_cast<void>(kvmd::msdRefusal(400, bytes, QStringLiteral("remove the image")));
        if (const std::optional<QJsonObject> heard = kvmd::msdHeard(bytes)) {
            known = kvmd::msdMerged(known, *heard);
            ++merged;
        }
        const kvmd::Listed listed = known ? kvmd::imagesOf(*known, notes, kvm) : kvmd::Listed{};
        promise(listed.images.size() <= core::mostMediaImages && listed.leftOut >= 0,
                "more images than are listed, or fewer than none left out");
        for (const core::MediaImage& image : listed.images) {
            promise(!image.name.isEmpty() && kvmd::isDeviceImage(image.id),
                    "an image listed by a name the program does not hand back");
            promise(!image.size || (*image.size >= 0 && *image.size <= qint64{1} << 50), "an image of no size");
            // (What this program carried there, said of it while it is that image.)
            notes.carried(kvm, image.id, QStringLiteral("00"), image.size.value_or(0),
                          known ? kvmd::stampOf(*known, image.id) : std::nullopt);
        }
        if (known) {
            static_cast<void>(kvmd::standsAgainst(*known));
            if (kvmd::mountedOf(*known, notes, kvm) && merged >= 2 && listed.images.size() >= 2) {
                reached("two msd events merged into an image on the drive beside two images in the storage");
            }
            static_cast<void>(kvmd::freeOf(*known));
            static_cast<void>(kvmd::sizeOf(*known));
            static_cast<void>(kvmd::msdKept(*known));
        }
        static_cast<void>(kvmd::offerOf(known, true));
        static_cast<void>(kvmd::offerOf(known, false));
    }
}

} // namespace qkvm::fuzz
