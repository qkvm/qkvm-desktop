#pragma once

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrlQuery>
#include <QUuid>

#include "stand_in.h"

namespace qkvm::core::testing {

// A JetKVM's own image storage and its virtual drive, by hand, as the web's stand-in has them (fake_jetkvm.py, from the
// firmware's jsonrpc.go and usb_mass_storage.go at dev d646be8, release 0.5.9): getVirtualMediaState, getStorageSpace,
// listStorageFiles, deleteStorageFile (a base name: a path is cut to its last part), startStorageFileUpload (refused
// where the file is there; `<name>.incomplete` opened to add to, its size said back as `alreadyUploadedBytes`; a start
// for a file under way already ends that one), mountWithStorage ("another virtual media is already mounted"; a CD-ROM
// drive takes 300 sectors at least, else the kernel's errno 525; a mode other than the drive's binds its USB device
// anew: `rebinds`) and unmountImage. What qKVM never asks is noted (`never`): mountWithHTTP, checkMountUrl,
// rpcMountBuiltInImage, setMassStorageMode. Its calls come on the library's thread, an upload on the interface thread:
// everything is under its lock.
//
// Handed to a JetKvmViewerByHand (its `storage`), which answers its calls with this; without one, a JetKVM is one of
// before these methods, and answers each "Method not found". And to a JetKvmByHand (its `storage`), which hands it
// POST /storage/upload (upload).
class JetKvmStorageByHand {
public:
    struct File {
        qint64 size = 0;
        QString createdAt = QStringLiteral("2026-10-30T09:12:44.000000+00:00");
        QByteArray sha256; // of what was uploaded
    };
    struct Open {
        QString filename;
        qint64 size = 0;
        qint64 already = 0;
        bool claimed = false;
    };

    // Its state, under the lock (lock() first, or the helpers below).
    std::map<QString, File> files;
    qint64 storageSize = 7'000'000'000;
    std::optional<QJsonObject> mounted; // what getVirtualMediaState says: source, mode, filename, size
    bool cdromMode = true;
    int rebinds = 0;
    QHash<QString, Open> uploadsOpen; // by the upload's id
    QStringList calls;                // every call of its storage and drive, as "method params"
    QStringList never;                // what qKVM is never to ask, where it was asked
    // Switches: a mount, an unmount and a deletion that answer yes and do nothing; what the start of an upload answers
    // instead.
    bool mountsNothing = false;
    bool unmountsNothing = false;
    bool deletesNothing = false;
    std::optional<QJsonObject> startSaysInstead;
    // What listStorageFiles says as its `files` in place of its list, from the `filesSayInsteadAfter`th list on (0:
    // every one); and what getVirtualMediaState says in place of its state.
    std::optional<QJsonValue> filesSayInstead;
    int filesSayInsteadAfter = 0;
    int listsSaid = 0;
    std::optional<QJsonValue> stateSaysInstead;

    // Every POST /storage/upload, as it came: its id, how much of the image it took, and whether that was all of it.
    struct Upload {
        QString id;
        qint64 received = 0;
        bool complete = false;
    };
    QList<Upload> uploads;
    // Switches of an upload: what it answers in its place (a refusal, a line hung up on); and so much of a body taken
    // at the most, the rest let go of, answered "Upload completed" all the same (a body that ended early).
    std::optional<Answered> uploadInstead;
    qint64 takesOnly = -1;
    // What came of a body is put under the image's own name whatever its length (a JetKVM that is none: a real one
    // keeps a body that ended early as `.incomplete`).
    bool keepsWhatCame = false;
    // An upload refused with a 500 whose body and status line name its id.
    bool namesTheIdInItsRefusal = false;

    int openUploads()
    {
        const auto held = lock();
        return static_cast<int>(uploadsOpen.size());
    }

    // POST /storage/upload?uploadId=, from whoever holds its session (`signedIn`), its body as it came: claimed once,
    // within a minute of its start, renamed into place where every byte came, answered "Upload completed" also where
    // the body ended early (fake_jetkvm.py storage_upload). On the interface thread.
    Answered upload(const Asked& asked, bool signedIn)
    {
        if (!signedIn) {
            return {401, R"({"error":"Unauthorized"})"};
        }
        const auto held = lock();
        const QUrlQuery query(QString::fromLatin1(asked.path.mid(asked.path.indexOf('?') + 1)));
        const QString id = query.queryItemValue(QStringLiteral("uploadId"));
        Upload record{id, 0, false};
        if (uploadInstead) {
            uploads.append(record);
            return *uploadInstead;
        }
        if (namesTheIdInItsRefusal) {
            uploads.append(record);
            Answered refused(500, R"({"error":"upload )" + id.toLatin1() + R"( failed"})");
            refused.reason = "Upload " + id.toLatin1() + " failed";
            return refused;
        }
        const auto open = uploadsOpen.find(id);
        if (open == uploadsOpen.end() || open->claimed) {
            uploads.append(record);
            return {404, R"({"error":"Upload not found"})"};
        }
        open->claimed = true;
        const QString name = open->filename;
        File& partial = files[name + QStringLiteral(".incomplete")];
        const QByteArray body =
            takesOnly >= 0 ? asked.body.first(std::min<qsizetype>(takesOnly, asked.body.size())) : asked.body;
        const qint64 written = open->already + body.size();
        partial.size = written;
        record.received = body.size();
        const qint64 size = open->size;
        uploadsOpen.erase(open);
        if (written == size || keepsWhatCame) {
            files.erase(name + QStringLiteral(".incomplete"));
            files[name] = File{written, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                               QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex()};
            record.complete = written == size;
        }
        uploads.append(record);
        return {200, R"({"message":"Upload completed"})"};
    }

    std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(m_lock); }
    void add(const QString& name, qint64 size)
    {
        const auto held = lock();
        files[name] = File{size, File{}.createdAt, {}};
    }
    bool has(const QString& name)
    {
        const auto held = lock();
        return files.contains(name);
    }
    std::optional<QJsonObject> mountedNow()
    {
        const auto held = lock();
        return mounted;
    }
    int rebound()
    {
        const auto held = lock();
        return rebinds;
    }
    QStringList called()
    {
        const auto held = lock();
        return calls;
    }
    QStringList neverAsked()
    {
        const auto held = lock();
        return never;
    }

    // A call of its storage or drive: what it answers (`result`, or `error`), or nothing where it is no such call. On
    // any thread.
    std::optional<QJsonObject> call(const QString& method, const QJsonObject& params)
    {
        static const QStringList ours{QStringLiteral("getVirtualMediaState"),   QStringLiteral("getStorageSpace"),
                                      QStringLiteral("listStorageFiles"),       QStringLiteral("deleteStorageFile"),
                                      QStringLiteral("startStorageFileUpload"), QStringLiteral("mountWithStorage"),
                                      QStringLiteral("unmountImage"),           QStringLiteral("mountWithHTTP"),
                                      QStringLiteral("checkMountUrl"),          QStringLiteral("rpcMountBuiltInImage"),
                                      QStringLiteral("setMassStorageMode")};
        if (!ours.contains(method)) {
            return std::nullopt;
        }
        const auto held = lock();
        calls.append(params.isEmpty()
                         ? method
                         : method + u' ' + QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact)));
        const auto result = [](const QJsonValue& value) { return QJsonObject{{QStringLiteral("result"), value}}; };
        const auto refused = [](const QString& message) {
            return QJsonObject{
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32603},
                                                      {QStringLiteral("message"), QStringLiteral("Internal error")},
                                                      {QStringLiteral("data"), message}}}};
        };
        if (method == QStringLiteral("mountWithHTTP") || method == QStringLiteral("checkMountUrl") ||
            method == QStringLiteral("rpcMountBuiltInImage") || method == QStringLiteral("setMassStorageMode")) {
            never.append(method);
            return refused(QStringLiteral("not in this stand-in"));
        }
        if (method == QStringLiteral("getVirtualMediaState")) {
            if (stateSaysInstead) {
                return result(*stateSaysInstead);
            }
            return result(mounted ? QJsonValue(*mounted) : QJsonValue());
        }
        if (method == QStringLiteral("getStorageSpace")) {
            return result(QJsonObject{{QStringLiteral("bytesUsed"), storageSize - free()},
                                      {QStringLiteral("bytesFree"), free()}});
        }
        if (method == QStringLiteral("listStorageFiles")) {
            if (filesSayInstead && listsSaid++ >= filesSayInsteadAfter) {
                return result(QJsonObject{{QStringLiteral("files"), *filesSayInstead}});
            }
            QJsonArray listed;
            for (const auto& [name, about] : files) {
                listed.append(QJsonObject{{QStringLiteral("filename"), name},
                                          {QStringLiteral("size"), about.size},
                                          {QStringLiteral("createdAt"), about.createdAt}});
            }
            return result(QJsonObject{{QStringLiteral("files"), listed}});
        }
        if (method == QStringLiteral("unmountImage")) {
            if (!unmountsNothing) {
                mounted.reset();
            }
            return result(QJsonValue());
        }
        const std::optional<QString> name = baseName(params.value(QStringLiteral("filename")));
        if (!name) {
            return refused(QStringLiteral("invalid filename"));
        }
        if (method == QStringLiteral("deleteStorageFile")) {
            if (!files.contains(*name)) {
                return refused(QStringLiteral("file does not exist: ") +
                               params.value(QStringLiteral("filename")).toString());
            }
            if (!deletesNothing) {
                files.erase(*name);
            }
            return result(QJsonValue());
        }
        if (method == QStringLiteral("startStorageFileUpload")) {
            if (files.contains(*name)) {
                return refused(QStringLiteral("file already exists: ") + *name);
            }
            for (auto open = uploadsOpen.begin(); open != uploadsOpen.end();) {
                open = open->filename == *name ? uploadsOpen.erase(open) : std::next(open); // (superseded)
            }
            const File& partial = files.try_emplace(*name + QStringLiteral(".incomplete")).first->second;
            const QString id = QStringLiteral("upload_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            uploadsOpen.insert(id, Open{*name, params.value(QStringLiteral("size")).toInteger(), partial.size, false});
            if (startSaysInstead) {
                return result(*startSaysInstead);
            }
            return result(QJsonObject{{QStringLiteral("alreadyUploadedBytes"), partial.size},
                                      {QStringLiteral("dataChannel"), id}});
        }
        // mountWithStorage
        if (mounted) {
            return refused(QStringLiteral("another virtual media is already mounted"));
        }
        if (!files.contains(*name)) {
            return refused(QStringLiteral("failed to get file info: stat /userdata/jetkvm/images/") + *name +
                           QStringLiteral(": no such file or directory"));
        }
        const QString mode = params.value(QStringLiteral("mode")).toString();
        if (mode != QStringLiteral("CDROM") && mode != QStringLiteral("Disk")) {
            return refused(QStringLiteral("invalid mode: ") + mode);
        }
        if (mode == QStringLiteral("CDROM") && files.at(*name).size < qint64{300} * 2048) {
            return refused(QStringLiteral("failed to set mass storage image: write "
                                          "/sys/kernel/config/usb_gadget/jetkvm/functions/mass_storage.usb0/lun.0/"
                                          "file: errno 525"));
        }
        if (mountsNothing) {
            return result(QJsonValue());
        }
        if ((mode == QStringLiteral("CDROM")) != cdromMode) {
            cdromMode = mode == QStringLiteral("CDROM");
            ++rebinds;
        }
        mounted = QJsonObject{{QStringLiteral("source"), QStringLiteral("Storage")},
                              {QStringLiteral("mode"), mode},
                              {QStringLiteral("filename"), *name},
                              {QStringLiteral("size"), files.at(*name).size}};
        return result(QJsonValue());
    }

private:
    // sanitizeFilename: a path cut to its last part; nothing for what is refused.
    static std::optional<QString> baseName(const QJsonValue& filename)
    {
        if (!filename.isString() || filename.toString().isEmpty() || filename.toString().startsWith(u'/') ||
            filename.toString().contains(QStringLiteral(".."))) {
            return std::nullopt;
        }
        QString base = filename.toString();
        base.replace(u'\\', u'/');
        base = base.section(u'/', -1);
        return base.isEmpty() || base == QStringLiteral(".") ? std::nullopt : std::optional<QString>(base);
    }
    qint64 free() const
    {
        qint64 used = 0;
        for (const auto& [name, about] : files) {
            used += about.size;
        }
        return storageSize - used;
    }

    std::mutex m_lock;
};

} // namespace qkvm::core::testing
