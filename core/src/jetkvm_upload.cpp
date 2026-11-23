#include <qkvm/core/jetkvm_device.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <QHostInfo>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>

#include <qkvm/core/carried_image.h>
#include <qkvm/core/jetkvm_media.h>
#include <qkvm/core/kept_with.h>
#include <qkvm/core/kvmd_media.h>
#include <qkvm/core/media_place.h>
#include <qkvm/core/read_ahead.h>

// An image carried from this PC to a JetKVM's own storage (the web's 0.3.0, A3: jetkvm.py media_upload, _carry,
// _cut_said and media_discard, in their words): opened over its connection (startStorageFileUpload), its bytes by POST
// /storage/upload?uploadId= on a line of their own, after the JetKVM that answers at its address was asked again which
// it is under the very session the bytes go with, the asking and the bytes at the one address its name was found at;
// streamed from the file as it is read, counted and hashed as it goes, kept nowhere; and what an upload that ended
// otherwise left (`<name>.incomplete`) taken off again.

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The most of its answer to the bytes (the web's MAX_REPLY).
constexpr qsizetype largestReplyBytes = qsizetype{2} * 1024 * 1024;

QString nothingLeft()
{
    return u"Nothing of it is left on the JetKVM."_s;
}

base::Error sessionNotTaken()
{
    // (It has one session for everybody: the sign-in anew that a try again makes ends the one of whoever signed in at
    // its own page, which is said. Review cx4-1 L6.)
    return {"action_failed", "The JetKVM did not take qKVM's session for the upload (somebody signed in to it "
                             "meanwhile): trying again signs qKVM in to it anew, which signs out whoever signed in at "
                             "its own page."};
}

base::Error stoppedBefore()
{
    return {MediaPlace::uploadStopped, "The upload was stopped before any of it went."};
}

// Why a part of an upload may be left where it is not found in a list of its storage that was not read whole: that
// list's not being read whole is not "nothing left" (outside review 4.6).
base::Error listNotReadWhole()
{
    return {"bad_response", "its list of its own storage could not be read whole"};
}

} // namespace

// The image on its way, from its asking to its answer: one at a time.
struct JetKvmDevice::Upload {
    QString name;
    qint64 size = 0;
    qint64 sent = 0;
    bool stopped = false;
    std::shared_ptr<CarriedImage> body;
    QPointer<QNetworkReply> reply;
    QByteArray token; // the session its bytes went under
    std::function<void(base::Result<MediaUploaded>)> done;
};

std::optional<MediaUploading> JetKvmDevice::uploading() const
{
    if (!m_upload) {
        return std::nullopt;
    }
    // (Nobody else uploads to this PC's own KVMs: whoever did, did it here.)
    return MediaUploading{m_upload->name, m_upload->size, m_upload->sent, u"this PC"_s};
}

std::function<void()> JetKvmDevice::uploadMedia(const QString& name, std::unique_ptr<QIODevice> image,
                                                std::function<void(base::Result<MediaUploaded>)> done)
{
    // Only from a file that is read at a place, whose length is known before a byte goes; and read on a thread of its
    // own, never on this one (ReadAhead), as a kvmd's.
    std::unique_ptr<QIODevice> file;
    if (qobject_cast<ReadAhead*>(image.get()) != nullptr) {
        file = std::move(image);
    } else {
        file = ReadAhead::of(std::move(image));
    }
    const bool readable = file != nullptr && file->isOpen() && file->isReadable() && !file->isSequential();
    const qint64 size = readable ? file->size() - file->pos() : 0;
    std::optional<base::Error> refused;
    if (!readable) {
        refused = base::Error{"invalid", "An image is carried from a file."};
    } else if (size <= 0) {
        refused = base::Error{"invalid", "An empty file is no image."};
    } else if (size > mostUploadBytes) {
        refused = base::Error{"too_large", "An image carried through qKVM may be 16 GB at the most."};
    } else if (!isImageName(name)) {
        refused = base::Error{"invalid", imageNameRule};
    } else if (m_upload) {
        refused = base::Error{
            "busy", u"%1 is being uploaded to this KVM at this moment. Wait for it to finish."_s.arg(m_upload->name)
                        .toStdString()};
    }
    if (refused) {
        later([done = std::move(done), why = *refused] { done(why); });
        return [] {};
    }
    auto upload = std::make_shared<Upload>();
    upload->name = name;
    upload->size = size;
    upload->body = std::make_shared<CarriedImage>(std::move(file));
    upload->done = std::move(done);
    m_upload = upload;
    withMediaConnection([self = QPointer<JetKvmDevice>(this), upload](const base::Result<Connected>& on) {
        if (self == nullptr) {
            return;
        }
        if (upload->stopped) {
            self->uploadOver(upload, stoppedBefore());
            return;
        }
        if (!on) {
            self->uploadOver(upload, on.error());
            return;
        }
        self->listFiles(on.value(), [self, on = on.value(), upload](const base::Result<jetkvm::Files>& files) {
            if (self == nullptr) {
                return;
            }
            if (!files) {
                self->uploadOver(upload, files.error());
                return;
            }
            const auto listed = [&files](const QString& id) {
                return std::ranges::any_of(files.value().images, [&id](const MediaImage& one) { return one.id == id; });
            };
            if (listed(upload->name)) {
                self->uploadOver(
                    upload, base::Error{"duplicate", u"%1 is on the JetKVM's own storage already: delete "
                                                     u"it first, or upload it under another name."_s.arg(upload->name)
                                                         .toStdString()});
                return;
            }
            if (listed(upload->name + jetkvm::partialEnd)) {
                // (Resumed, the JetKVM would add to it: not this image whole, and not the hash of it.)
                self->uploadOver(
                    upload, base::Error{"duplicate",
                                        u"A part of an earlier upload of %1 is on the JetKVM: delete "
                                        u"it in the list first, or upload it under another name."_s.arg(upload->name)
                                            .toStdString()});
                return;
            }
            // (Neither found in a list that was not read whole: either may be there all the same. Outside review 4.6.)
            if (!files.value().whole) {
                self->uploadOver(upload,
                                 jetkvm::notReadWhole(u"whether %1, or a part of an earlier upload of it, is there "
                                                      u"already is not known"_s.arg(upload->name)));
                return;
            }
            callOver(on, u"getStorageSpace"_s, {}, [self, on, upload](const base::Result<json::Value>& space) {
                if (self == nullptr) {
                    return;
                }
                if (!space) {
                    self->uploadOver(upload, space.error());
                    return;
                }
                if (const std::optional<qint64> free = jetkvm::spaceOf(space.value()).free;
                    free && upload->size > *free) {
                    self->uploadOver(
                        upload,
                        base::Error{"no_space", u"The JetKVM's own storage has %1 bytes free, "
                                                u"and the image is %2: delete an image there "
                                                u"first."_s.arg(kvmd::withCommas(*free), kvmd::withCommas(upload->size))
                                                    .toStdString()});
                    return;
                }
                if (upload->stopped) {
                    self->uploadOver(upload, stoppedBefore());
                    return;
                }
                const QJsonObject asked{{u"filename"_s, upload->name}, {u"size"_s, upload->size}};
                callOver(on, u"startStorageFileUpload"_s, asked,
                         [self, upload](const base::Result<json::Value>& started) {
                             if (self == nullptr) {
                                 return;
                             }
                             if (!started) {
                                 self->notStarted(upload, started.error());
                                 return;
                             }
                             self->opened(upload, jetkvm::startedOf(started.value()));
                         });
            });
        });
    });
    return [self = QPointer<JetKvmDevice>(this), held = std::weak_ptr<Upload>(upload)] {
        const std::shared_ptr<Upload> upload = held.lock();
        if (self == nullptr || upload == nullptr || upload->stopped) {
            return;
        }
        upload->stopped = true;
        if (upload->reply != nullptr && !upload->reply->isFinished()) {
            upload->reply->abort();
        }
    };
}

void JetKvmDevice::notStarted(const std::shared_ptr<Upload>& upload, const base::Error& why)
{
    if (!mayHaveBeenDone(why)) {
        uploadOver(upload, why);
        return;
    }
    // No answer in time: it may have opened it all the same (its `<name>.incomplete`, made as the start is taken),
    // which would have the next upload of that name refused for a part of an earlier one. Taken off as what an upload
    // that ended otherwise left is, and said where it cannot be (the web says the timeout alone, and leaves it).
    // NOLINTNEXTLINE(bugprone-exception-escape): (the words of an answer out of memory: nothing of ours throws)
    discard(upload->name, [this, upload, why](const std::optional<QString>& left) {
        uploadOver(upload, base::Error{why.code, why.message + (left ? " " + left->toStdString() : std::string())});
    });
}

void JetKvmDevice::opened(const std::shared_ptr<Upload>& upload, const jetkvm::Started& started)
{
    if (started.id) {
        carry(upload, *started.id);
        return;
    }
    // Not opened as it was asked (another id, or added to what was there): what it opened anew is taken off again, and
    // what was there before stays (the web's: a discard only where it says it began with nothing).
    const auto said = [this, upload](const std::optional<QString>& left) {
        uploadOver(upload, base::Error{"bad_response", (u"The JetKVM did not open the upload as it was asked."_s +
                                                        (left ? u' ' + *left : QString()))
                                                           .toStdString()});
    };
    if (!started.anew) {
        said(std::nullopt);
        return;
    }
    discard(upload->name, said);
}

void JetKvmDevice::carry(const std::shared_ptr<Upload>& upload, const QString& id)
{
    // Its bytes go under the session this driver keeps, after the JetKVM that answers at its address was asked which
    // it is under that very session (the web's _carry and _same_jetkvm): another one is sent none of it. Its name is
    // found once, and the asking and the bytes both go to the one address it was found at: two JetKVMs that answer to
    // one name (two out of their boxes, `jetkvm.local`) never have the asking go to one and the bytes, with the
    // session, to the other (stricter than the web, whose two connections each find the name anew).
    withToken([self = QPointer<JetKvmDevice>(this), upload, id](const base::Result<QByteArray>& token) {
        if (self == nullptr) {
            return;
        }
        if (upload->stopped || !token) {
            self->cut(upload, token ? base::Error{} : token.error());
            return;
        }
        self->foundAt([self, upload, id, token = token.value()](const QList<QHostAddress>& at) {
            if (self == nullptr) {
                return;
            }
            if (upload->stopped) {
                self->cut(upload, base::Error{});
                return;
            }
            self->askWhichAt(upload, id, token, at);
        });
    });
}

void JetKvmDevice::foundAt(std::function<void(QList<QHostAddress>)> then)
{
    const QString name = m_where.address.host();
    if (const QHostAddress address(name); !address.isNull()) {
        later([then = std::move(then), address] { then({address}); });
        return;
    }
    // What is done next holds the upload, its file and whoever is to hear its end, all of this thread: kept by a child
    // of this driver's (kept_with.h), on which the lookup's answer is heard, and the function the lookup is handed
    // holds only a pointer to it. Qt lets go of that function on its own lookup thread where what the answer is heard
    // on has ended before the answer came (QHostInfo: it is the lookup's own until the answer is handed to an object of
    // this thread), which this driver's end before then would have had the upload end there with it.
    auto& waiting = keeperOf(*this, std::move(then));
    const auto found = [waiting = &waiting](const QList<QHostAddress>& at) {
        const std::function<void(QList<QHostAddress>)> next = std::exchange(waiting->value, nullptr);
        waiting->deleteLater();
        if (next) {
            next(at);
        }
    };
    if (m_pace.lookUp) {
        m_pace.lookUp(name, &waiting, found);
        return;
    }
    QHostInfo::lookupHost(name, &waiting, [found](const QHostInfo& info) { found(info.addresses()); });
}

DeviceAsker* JetKvmDevice::uploadLine(const QHostAddress& at)
{
    // Made for it and let go of after it, whatever comes (a long upload cut off is no state for the line every look
    // uses), held to what the JetKVM is known by as every line of this driver is.
    auto* line = new DeviceAsker(
        DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
        DeviceAsker::Times{m_pace.connectWithinMs, m_pace.uploadQuietMs, m_pace.transferMs}, "authToken"_ba,
        this); // NOLINT(cppcoreguidelines-owning-memory): this driver's, let go of once it is answered
    if (!line->onlyTo(at)) {
        delete line; // NOLINT(cppcoreguidelines-owning-memory): made just now, and asked nothing
        return nullptr;
    }
    connect(line, &DeviceAsker::showedItsCertificate, this, &JetKvmDevice::showed);
    return line;
}

void JetKvmDevice::askWhichAt(const std::shared_ptr<Upload>& upload, const QString& id, const QByteArray& token,
                              QList<QHostAddress> at)
{
    // (A few of the addresses it was found at, in the order they were found, as Windows tries them: one where nothing
    // answers is passed over, as a line of Windows' own is.)
    constexpr qsizetype mostAddresses = 4;
    at = at.mid(0, mostAddresses);
    DeviceAsker* line = nullptr;
    QHostAddress to;
    while (line == nullptr && !at.isEmpty()) {
        to = at.takeFirst();
        line = uploadLine(to);
    }
    if (line == nullptr) {
        // Its name was found at no address a line goes to.
        cut(upload, DeviceAsker::unreachable());
        return;
    }
    Ask device;
    device.path = u"/device"_s;
    device.largest = largestReplyBytes;
    device.quietMs = m_pace.quietMs;
    line->send(
        device, token,
        [self = QPointer<JetKvmDevice>(this), upload, id, token, at, to, line](const base::Result<Reply>& reply) {
            line->deleteLater();
            if (self == nullptr) {
                return;
            }
            if (reply && reply.value().status == 401) {
                self->uploadSessionRefused(token);
                self->cut(upload, sessionNotTaken());
                return;
            }
            if (!reply && reply.error().code == "unreachable" && !at.isEmpty() && !upload->stopped) {
                self->askWhichAt(upload, id, token, at);
                return;
            }
            std::optional<QString> seen;
            if (const std::optional<base::Error> other = self->judged(reply, false, seen)) {
                self->cut(upload, *other);
                return;
            }
            if (upload->stopped) {
                self->cut(upload, base::Error{});
                return;
            }
            self->post(upload, id, token, to);
        });
}

void JetKvmDevice::post(const std::shared_ptr<Upload>& upload, const QString& id, const QByteArray& token,
                        const QHostAddress& at)
{
    // On a line of its own, to the very address the JetKVM was asked which it is at (askWhichAt), and to no other.
    DeviceAsker* line = uploadLine(at);
    if (line == nullptr) {
        cut(upload, DeviceAsker::unreachable()); // (never: a line went to that address just now)
        return;
    }
    Ask bytes;
    bytes.method = "POST"_ba;
    bytes.path = u"/storage/upload"_s;
    bytes.query.addQueryItem(u"uploadId"_s, id);
    bytes.streamed = upload->body.get();
    bytes.bodyIs = "application/octet-stream"_ba;
    bytes.largest = largestReplyBytes;
    bytes.quietMs = m_pace.uploadQuietMs;
    bytes.wholeMs = m_pace.uploadMostMs;
    bytes.doesSomething = true;
    bytes.handed = [self = QPointer<JetKvmDevice>(this), held = std::weak_ptr<Upload>(upload)](QNetworkReply* reply) {
        const std::shared_ptr<Upload> upload = held.lock();
        if (upload == nullptr) {
            reply->abort();
            return;
        }
        upload->reply = reply;
        // (The file is the reply's for as long as it is: Qt may read it until it lets go. It ends with the reply, on
        // its thread: kept_with.h.)
        keptWith(*reply, upload->body);
        QObject::connect(reply, &QNetworkReply::uploadProgress, reply, [self, held](qint64 sent, qint64 total) {
            // (Qt says 0 of 0 as the reply finishes where it kept no count of its own: that says nothing of how far it
            // came, and is not taken for it.)
            if (const std::shared_ptr<Upload> upload = held.lock(); upload != nullptr && total > 0) {
                upload->sent = std::clamp<qint64>(sent, 0, upload->size);
            }
            // (Its connection, which the listing after it goes over, is in use meanwhile: not given back for nobody
            // watching, as a key that goes keeps it.)
            if (self != nullptr) {
                self->m_viewer->keyGoes();
            }
        });
        if (upload->stopped) {
            QMetaObject::invokeMethod(reply, &QNetworkReply::abort, Qt::QueuedConnection);
        }
    };
    upload->token = token;
    line->send(bytes, token, [self = QPointer<JetKvmDevice>(this), upload, line](const base::Result<Reply>& reply) {
        line->deleteLater();
        upload->reply = nullptr;
        if (self != nullptr) {
            self->written(upload, reply);
        }
    });
}

void JetKvmDevice::uploadSessionRefused(const QByteArray& used)
{
    // Somebody signed in to it meanwhile (its own page), which ended this program's session while its connection
    // stayed up: let go of here, so that the upload tried again goes under a session given anew, as a kvmd's does
    // (outside review 4.8: every one was refused until the connection was made anew). The web keeps it.
    if (m_token == used) {
        m_token.clear();
    }
    m_openAccess = false;
}

void JetKvmDevice::written(const std::shared_ptr<Upload>& upload, const base::Result<Reply>& reply)
{
    if (!reply) {
        // It ended before its answer: the sender's doing (stopped, or its file could not be read on the way), or the
        // JetKVM's. What came of it there is taken off, and said.
        cut(upload, reply.error());
        return;
    }
    if (const int status = reply.value().status; status != 200) {
        if (status == 401) {
            uploadSessionRefused(upload->token);
        }
        discard(upload->name, [this, upload, status](const std::optional<QString>& left) {
            const QString rest = left.value_or(nothingLeft());
            const QString said = status == 401 ? QString::fromStdString(sessionNotTaken().message) + u' '
                                               : u"The JetKVM did not take the image (HTTP %1). "_s.arg(status);
            uploadOver(upload, base::Error{"action_failed", (said + rest).toStdString()});
        });
        return;
    }
    // Answered before every byte of it went, and was read and hashed for that: not this image whole, whatever it lists
    // (the web asserts its hash is there; review c8b-1 L2).
    if (!jetkvm::wentWhole(upload->sent, upload->size, upload->body->sha256())) {
        discard(upload->name, [this, upload](const std::optional<QString>& left) {
            uploadOver(upload,
                       base::Error{"action_failed", (u"The JetKVM answered before all of the image had gone. "_s +
                                                     left.value_or(nothingLeft()))
                                                        .toStdString()});
        });
        return;
    }
    // It answers "Upload completed" to a body that ended early too, and keeps what came as `.incomplete`: held to its
    // listing it whole, as asked anew over its connection.
    withMediaConnection([self = QPointer<JetKvmDevice>(this), upload](const base::Result<Connected>& on) {
        if (self == nullptr) {
            return;
        }
        if (!on) {
            self->uploadOver(upload, on.error());
            return;
        }
        self->listFiles(on.value(), [self, upload](const base::Result<jetkvm::Files>& files) {
            if (self == nullptr) {
                return;
            }
            if (!files) {
                self->uploadOver(upload, files.error());
                return;
            }
            // (A list not read whole says nothing of whether a part of it was left beside it: not done, and nothing of
            // it taken off. Outside review 4.6.)
            if (!files.value().whole) {
                self->uploadOver(upload,
                                 jetkvm::notReadWhole(
                                     u"whether %1 is there whole after its upload is not known"_s.arg(upload->name)));
                return;
            }
            const QList<MediaImage>& images = files.value().images;
            const auto found =
                std::ranges::find_if(images, [&upload](const MediaImage& one) { return one.id == upload->name; });
            const bool partLeft = std::ranges::any_of(
                images, [&upload](const MediaImage& one) { return one.id == upload->name + jetkvm::partialEnd; });
            const std::optional<qint64> listedSize = found != images.end() ? found->size : std::nullopt;
            const bool listedOtherwise = found != images.end() && listedSize != std::optional<qint64>(upload->size);
            if (found == images.end() || listedOtherwise || partLeft) {
                // (What it lists under the image's own name is said as it is, and stays: it is not "nothing", nor is it
                // this image.)
                const QString listedAs =
                    listedOtherwise
                        ? u"It lists %1 at %2 bytes, not %3: delete it there."_s.arg(
                              upload->name, listedSize ? kvmd::withCommas(*listedSize) : u"an unknown number of"_s,
                              kvmd::withCommas(upload->size))
                        : QString();
                self->discard(upload->name, [self, upload, listedAs](const std::optional<QString>& left) {
                    if (self == nullptr) {
                        return;
                    }
                    QString said = u"The JetKVM does not list the image whole after its upload."_s;
                    for (const QString& more :
                         {listedAs, left.value_or(listedAs.isEmpty() ? nothingLeft() : QString())}) {
                        if (!more.isEmpty()) {
                            said += u' ' + more;
                        }
                    }
                    self->uploadOver(upload, base::Error{"action_failed", said.toStdString()});
                });
                return;
            }
            const QString sha = upload->body->sha256();
            kvmd::MediaNotes::program().carried(self->mediaKey(), upload->name, sha, upload->size,
                                                files.value().listed.value(upload->name).stamp);
            self->uploadOver(upload, MediaUploaded{sha});
        });
    });
}

void JetKvmDevice::cut(const std::shared_ptr<Upload>& upload, const base::Error& why)
{
    // (What it left taken off first.)
    // NOLINTNEXTLINE(bugprone-exception-escape): (the words of an answer out of memory: nothing of ours throws)
    discard(upload->name, [this, upload, why, failed = upload->body->failure()](const std::optional<QString>& left) {
        cutSaid(upload, why, failed, left);
    });
}

void JetKvmDevice::cutSaid(const std::shared_ptr<Upload>& upload, const base::Error& why, const QString& failed,
                           const std::optional<QString>& left)
{
    // (The web's _cut_said.)
    const QString rest = left.value_or(nothingLeft());
    if (upload->stopped) {
        uploadOver(upload, base::Error{left ? "action_failed" : MediaPlace::uploadStopped,
                                       (u"The upload was stopped before all of it had gone. "_s + rest).toStdString()});
    } else if (!failed.isEmpty()) {
        uploadOver(upload, base::Error{"disconnected", (u"The image could not be read on the way, and nothing more of "
                                                        u"it went: "_s +
                                                        failed + u' ' + rest)
                                                           .toStdString()});
    } else {
        const bool kept = why.code == "timeout" || why.code == "dropped" || why.code == "unreachable" ||
                          why.code == "another_machine";
        uploadOver(upload, base::Error{kept ? why.code : std::string("action_failed"),
                                       "The upload did not get through to the JetKVM (" + why.message + "). " +
                                           rest.toStdString()});
    }
}

void JetKvmDevice::discard(const QString& name, std::function<void(std::optional<QString>)> then)
{
    // What an upload that ended otherwise left (`<name>.incomplete`), taken off over its connection (the web's
    // media_discard): nothing said where nothing is left, else what is.
    const QString part = name + jetkvm::partialEnd;
    const auto mayBe = [part](const base::Error& why) {
        return std::optional<QString>(
            u"What was written of it may be on the JetKVM still, as %1 (%2): delete it there."_s.arg(
                part, QString::fromStdString(why.message)));
    };
    const auto partIn = [part](const jetkvm::Files& files) {
        return std::ranges::any_of(files.images, [&part](const MediaImage& one) { return one.id == part; });
    };
    withMediaConnection([self = QPointer<JetKvmDevice>(this), part, mayBe, partIn,
                         then = std::move(then)](const base::Result<Connected>& on) {
        if (self == nullptr) {
            return;
        }
        if (!on) {
            then(mayBe(on.error()));
            return;
        }
        self->listFiles(
            on.value(), [self, on = on.value(), part, mayBe, partIn, then](const base::Result<jetkvm::Files>& files) {
                if (self == nullptr) {
                    return;
                }
                if (!files) {
                    then(mayBe(files.error()));
                    return;
                }
                if (!partIn(files.value())) {
                    then(files.value().whole ? std::nullopt : mayBe(listNotReadWhole()));
                    return;
                }
                callOver(on, u"deleteStorageFile"_s, QJsonObject{{u"filename"_s, part}},
                         [self, on, part, mayBe, partIn, then](const base::Result<json::Value>& gone) {
                             if (self == nullptr) {
                                 return;
                             }
                             if (!gone) {
                                 then(mayBe(gone.error()));
                                 return;
                             }
                             self->listFiles(on, [part, mayBe, partIn, then](const base::Result<jetkvm::Files>& after) {
                                 if (!after) {
                                     then(mayBe(after.error()));
                                 } else if (partIn(after.value())) {
                                     then(u"What was written of it is on the JetKVM still, as %1: delete it "
                                          u"there."_s.arg(part));
                                 } else if (!after.value().whole) {
                                     then(mayBe(listNotReadWhole()));
                                 } else {
                                     then(std::nullopt);
                                 }
                             });
                         });
            });
    });
}

void JetKvmDevice::uploadOver(const std::shared_ptr<Upload>& upload, const base::Result<MediaUploaded>& result)
{
    if (m_upload == upload) {
        m_upload.reset();
    }
    if (auto done = std::exchange(upload->done, {})) {
        done(result);
    }
}

} // namespace qkvm::core
