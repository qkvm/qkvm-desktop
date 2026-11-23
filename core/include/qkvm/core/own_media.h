#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QIODevice>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/core/server_types.h>

namespace qkvm::core {

// What a driver of this PC's own wall that has virtual media does with them, of whichever family: a kvmd's
// (KvmdDevice, kvmd_media.cpp) and a Proxmox guest's (ProxmoxDevice, proxmox_media.cpp). The wall (LocalWall, the
// place of its machines' media: local_wall_media.cpp) does what the web's server does before its driver is asked, and
// asks the driver the rest through this alone. Each method is as KvmdDevice's says it; a family that cannot do one
// (a guest's images are Proxmox's: none is uploaded or deleted from here) refuses it, and says so in what it offers.
//
// On the interface thread, as every driver is (own_device.h): a call comes back at once; what it asked for is handed
// to `done` later, on the same thread, once.
class OwnMedia {
public:
    virtual ~OwnMedia() = default;
    OwnMedia(const OwnMedia&) = delete;
    OwnMedia& operator=(const OwnMedia&) = delete;
    OwnMedia(OwnMedia&&) = delete;
    OwnMedia& operator=(OwnMedia&&) = delete;

    // What it can do now; nothing: it has no virtual media now.
    virtual std::optional<MediaOffer> mediaOffer() const = 0;
    virtual void askMedia(std::function<void(base::Result<MediaList>)> done) = 0;
    virtual void mountMedia(const MediaMount& mount, std::function<void(base::Result<MediaDone>)> done) = 0;
    // `image`: what was in its drive when it was asked.
    virtual void unmountMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done) = 0;
    virtual void deleteMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done) = 0;
    // What is handed back stops it.
    virtual std::function<void()> uploadMedia(const QString& name, std::unique_ptr<QIODevice> image,
                                              std::function<void(base::Result<MediaUploaded>)> done) = 0;
    // The image on its way to it, and how far it has come; nothing: none is.
    virtual std::optional<MediaUploading> uploading() const = 0;

protected:
    OwnMedia() = default;
};

} // namespace qkvm::core
