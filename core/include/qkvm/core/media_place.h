#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QIODevice>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/core/server_types.h>

namespace qkvm::core {

// Where a machine's virtual media are asked for and changed (0.3.0): a QKVM server, for its machines, an admin's,
// signed in in person (ServerClient); or this PC's own wall, for its kvmd KVMs, which it speaks to itself (LocalWall,
// GAPS A2). Whoever shows the media box (ui/media_view.h) asks this, and cannot tell which of the two answers: the
// types and the words are the same. `machine` is the machine's id there; `named` what the question named it, which
// both hold to (409 "changed" where it is called otherwise by then: nothing was done); `listed` the mark of the list
// the change was picked from (MediaList::listed), which a server 0.3.2 holds to (409 "moved" where the tile's machine
// is another by then: nothing was done), and which is empty where the list said none (sent none, as before).
//
// On the interface thread. A call comes back at once; what it asked for is handed to `done` later, on the same thread,
// once.
class MediaPlace {
public:
    virtual ~MediaPlace() = default;
    MediaPlace(const MediaPlace&) = delete;
    MediaPlace& operator=(const MediaPlace&) = delete;
    MediaPlace(MediaPlace&&) = delete;
    MediaPlace& operator=(MediaPlace&&) = delete;

    // An upload was stopped by whoever began it (the stop uploadMedia hands back), and nothing of it is left: said by
    // nobody, as whoever stopped it knows.
    static constexpr const char* uploadStopped = "desktop:stopped";
    // A server's word (0.3.2, 409) for a change picked from a list of a machine its tile is no longer at (`listed`):
    // nothing was done, and the box that listed it closes.
    static constexpr const char* moved = "moved";

    // The images that can be mounted on it and what is mounted, asked of the machine now.
    virtual void askMedia(const QString& machine, std::function<void(base::Result<MediaList>)> done) = 0;
    // (The mark of the list the image was picked from is the mount's own: MediaMount::listed.)
    virtual void mountMedia(const QString& machine, const MediaMount& mount,
                            std::function<void(base::Result<MediaDone>)> done) = 0;
    virtual void unmountMedia(const QString& machine, const QString& image, const QString& named, const QString& listed,
                              std::function<void(base::Result<MediaDone>)> done) = 0;
    virtual void deleteMedia(const QString& machine, const QString& image, const QString& named, const QString& listed,
                             std::function<void(base::Result<MediaDone>)> done) = 0;
    // An image carried to the machine's device as `name`, from `image` as it is read (a file, read at a place), with
    // its length said first, 16 GB at the most (mostUploadBytes), kept nowhere on the way. What is handed back stops
    // it.
    virtual std::function<void()> uploadMedia(const QString& machine, const QString& name, const QString& named,
                                              const QString& listed, std::unique_ptr<QIODevice> image,
                                              std::function<void(base::Result<MediaUploaded>)> done) = 0;
    // How far the upload under way to it has come; nothing: none is.
    virtual void askUpload(const QString& machine,
                           std::function<void(base::Result<std::optional<MediaUploading>>)> done) = 0;
    // Who answers: an answer that comes when this is another (another server, another account there) is nobody's.
    virtual QByteArray mediaIdentity() const = 0;
    // Whether an error of a change is no answer of the place's own (nothing came back from it: it may have been done),
    // which the box says as that; otherwise its words are said as they are.
    virtual bool saidNothing(const base::Error& error) const = 0;
    // Why a box that listed the machine as `bound` (MediaList::boundTo, as it first listed it) changes nothing of it
    // any more: the machine is gone, or its address, or what it is known by, is another since ("changed"). Nothing: it
    // is the machine it listed, or the place keeps no such word (a server). Asked before every change a box sends.
    virtual std::optional<base::Error> movedSince(const QString& machine, const QString& bound) const = 0;

protected:
    MediaPlace() = default;
};

} // namespace qkvm::core
