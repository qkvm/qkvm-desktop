#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <qkvm/core/kvmd_device.h>

#include "stand_in.h"

// A KVM for the tests of the program's own, and what is said of and to it (DESIGN.md section 8):
// for the driver that speaks to one KVM, and for the wall of this PC's own KVMs.
namespace qkvm::core::testing {

using namespace Qt::StringLiterals;

inline QByteArray written(const QJsonObject& said)
{
    return QJsonDocument(said).toJson(QJsonDocument::Compact);
}

inline Answered good(const QJsonObject& result = {})
{
    return {200, written({{u"ok"_s, true}, {u"result"_s, result}})};
}

inline Answered refusal(int status, const QString& name, const QString& sentence = {})
{
    return {status,
            written({{u"ok"_s, false}, {u"result"_s, QJsonObject{{u"error"_s, name}, {u"error_msg"_s, sentence}}}})};
}

inline QString sign(char32_t number)
{
    return QString::fromUcs4(&number, 1);
}

// The beginning of a JPEG, as far as where it says its size, and `more` bytes after it.
inline QByteArray jpeg(int width, int height, qsizetype more = 64)
{
    QByteArray data("\xff\xd8\xff\xe0\x00\x10", 6);
    data += QByteArray(14, 'x');
    data += QByteArray("\xff\xc0\x00\x11\x08", 5);
    data += static_cast<char>((height >> 8) & 0xFF);
    data += static_cast<char>(height & 0xFF);
    data += static_cast<char>((width >> 8) & 0xFF);
    data += static_cast<char>(width & 0xFF);
    return data + QByteArray(more, 'p');
}

// A KVM that runs kvmd, as far as the program asks one: it is what a test sets it to be, answers
// as a real one does, and notes what it was asked. (The server's own tests have one in Python,
// tests/fake_kvm.py in ../app, which the driver is tried against as well: tools/stand_in_kvm.py.)
class MadeUpKvm {
public:
    // At a port of the test's choosing where one KVM is to take the place of another.
    explicit MadeUpKvm(const std::optional<core::testing::Tls>& tls = {}, quint16 port = 0)
    {
        const auto answer = [this](const Asked& asked) { return answered(asked); };
        stand = tls ? std::make_unique<StandIn>(answer, *tls, port) : std::make_unique<StandIn>(answer, port);
    }

    // What it is.
    QString password = u"secret"_s;
    bool passAsCookie = false; // as the other maker hands it over
    int signInStatus = 200;
    int signInAfterMs = 0; // its sign-ins are answered only after this long, a yes and a no alike
    // Told of every sign-in as it comes, before it is answered (and before signInAfterMs begins): for a test that does
    // something while one is out, whatever holds the test up meanwhile.
    std::function<void()> whenSignedInTo;
    bool signal = true;
    int width = 1920;
    int height = 1080;
    // Its state (/api/streamer) is answered only after this long: a KVM busy for a moment, whose look takes longer than
    // an asking waits for one. Taken as the question comes.
    int stateAfterMs = 0;
    bool atx = false;
    // Its power board, as GLKVM says it: a word of its own for the computer ("on", "off", "sleep") beside a light
    // that stays off on a real Comet whatever the computer does. None: as stock kvmd says it, the light alone. And
    // the light itself (none: as `atx`), and whether the board is pressing a button at the moment.
    std::optional<QString> powerWord;
    std::optional<bool> powerLight;
    bool diskLight = false; // its disk light (leds.hdd), lit while the computer's disk works

    bool boardBusy = false;
    // Told of every press its board takes, with the button, before it answers: for a test whose computer comes on or
    // goes off a while after a press.
    std::function<void(const QByteArray& button)> whenPressed;
    // A GL.iNet Fingerbot paired with it, as a real Comet answers for one: a press is taken ("success") and its battery
    // said (a real one said 101). Whether one is paired, a Comet says only on a line to its events
    // (LiveStandIn::greeting).
    bool fingerbot = false;
    int battery = 101;
    bool keyboard = true;
    QList<bool> ports; // the computers behind it, and whether one is on each; none: it has one
    int shown = 0;
    // GL.iNet's firmware, with its own routes beside kvmd's; else stock kvmd, which answers 404 for those before it
    // asks who asks (kvmd 4.226).
    bool glinet = true;
    bool restsUntilWatched = false;
    bool restsThoughWatched = false; // as one that was started anew under a line that never heard of it
    // While its video rests it says nothing of it in its state, as one maker's does: "streamer": null.
    bool saysNothingOfAVideoThatRests = false;
    QByteArray picture;
    QByteArray pictureKind = "image/jpeg"_ba;
    int pictureAfterMs = 0;
    QList<QJsonObject> toWake;
    // Its virtual media, as kvmd's /api/msd says them (kvmd's msd: what kvm.py reads of it), where it has a drive
    // (`hasDrive`); without one it answers 404 there, as a KVM without them. Its images by name, each {size, complete,
    // mod_ts}; the one chosen for its drive (set_params), and whether that is in it (set_connected); GL.iNet's File
    // Sharing; its storage's room. A write (POST /api/msd/write) is taken whole, and listed at once, complete, of the
    // length that came.
    struct Msd {
        bool enabled = true;
        bool online = true;
        bool busy = false;
        bool fileSharing = false;
        QJsonObject images;
        QString chosen;
        bool cdrom = true;
        bool rw = false;
        bool connected = false;
        qint64 free = 8'000'000'000;
        qint64 size = 16'000'000'000;
        // Mounted otherwise than it was asked: always as a flash drive, writable.
        bool mountsOtherwise = false;
        // Its watch on the storage: a deleted image is listed this many questions longer.
        int listedLonger = 0;
        // What its state says as `storage.images` in place of its images; and that from the next remove on.
        std::optional<QJsonValue> imagesSayInstead;
        bool imagesUnreadAfterRemove = false;
        // What its state says as its `drive` in place of its own.
        std::optional<QJsonValue> driveSaysInstead;
        // Another client of the KVM chooses this image for its drive, as a USB flash drive and writable: once, right
        // after a set_params of this program's is taken, or just before its connect is (each then forgotten); or
        // chooses it and connects it, just before this program's connect (which kvmd then refuses); or connects what
        // this program chose, right after its set_params. And a disconnect answered that does nothing, and one refused
        // as busy.
        QString othersChoiceAfterParams;
        QString othersChoiceAtConnect;
        QString othersMountAtConnect;
        bool othersConnectAfterParams = false;
        bool disconnectsNothing = false;
        bool disconnectRefused = false;
        // What the question of its state (GET /api/msd) right after a connect it takes, or a disconnect it takes, is
        // answered with in the place of the state, once (each then forgotten): a read-back that fails, or that finds
        // its drive otherwise.
        std::optional<Answered> stateAfterConnect;
        std::optional<Answered> stateAfterDisconnect;
    };
    bool hasDrive = false;
    Msd msd;
    // Its state as /api/msd says it, and as its `msd` event says it whole on a line to its events.
    QJsonObject msdState() const
    {
        if (!hasDrive) {
            return {};
        }
        const Msd& kept = msd;
        QJsonObject drive{{u"connected"_s, kept.connected}, {u"cdrom"_s, kept.cdrom}, {u"rw"_s, kept.rw}};
        if (!kept.chosen.isEmpty()) {
            QJsonObject image = kept.images.value(kept.chosen).toObject();
            image.insert(u"name"_s, kept.chosen);
            drive.insert(u"image"_s, image);
        } else {
            drive.insert(u"image"_s, QJsonValue::Null);
        }
        QJsonObject images = kept.images;
        for (const QString& gone : std::as_const(m_goneButListed)) {
            images.insert(gone, QJsonObject{{u"size"_s, 1}, {u"complete"_s, true}});
        }
        return {{u"enabled"_s, kept.enabled},
                {u"online"_s, kept.online},
                {u"busy"_s, kept.busy},
                {u"drive"_s, kept.driveSaysInstead.value_or(drive)},
                {u"drive_partition"_s, QJsonObject{{u"connected"_s, kept.fileSharing}}},
                {u"storage"_s, QJsonObject{{u"images"_s, kept.imagesSayInstead.value_or(images)},
                                           {u"parts"_s, QJsonObject{{u""_s, QJsonObject{{u"size"_s, kept.size},
                                                                                        {u"free"_s, kept.free}}}}}}},
                {u"features"_s, QJsonObject{{u"multi"_s, true}}}};
    }
    QString msdEvent() const
    {
        return QString::fromUtf8(written({{u"event_type"_s, u"msd"_s}, {u"event"_s, msdState()}}));
    }
    // A value behind the question mark as kvmd reads it (aiohttp): a plus is a blank, the rest percent-decoded.
    static QString asKvmdReadsIt(const QByteArray& path, const QByteArray& key)
    {
        const QByteArray query = path.mid(path.indexOf('?') + 1);
        for (const QByteArray& pair : query.split('&')) {
            const qsizetype equals = pair.indexOf('=');
            QByteArray name = pair.left(equals);
            QByteArray value = equals < 0 ? QByteArray() : pair.mid(equals + 1);
            name.replace('+', ' ');
            value.replace('+', ' ');
            if (QByteArray::fromPercentEncoding(name) == key) {
                return QString::fromUtf8(QByteArray::fromPercentEncoding(value));
            }
        }
        return {};
    }
    // Where set, its plain address sends every question on there (a web server before kvmd once HTTPS is its only way
    // in: 301, Location this and the question's path), or every question `sendsOn` says of by its path; none: it
    // answers as kvmd.
    QString httpsAt;
    std::function<bool(const QByteArray& path)> sendsOn;
    // By path: what it answers there instead of the usual, the first time or every time.
    QHash<QByteArray, QList<Answered>> first;
    QHash<QByteArray, Answered> instead;
    // By path: what it answers there until a picture was asked of it, since a test last said
    // that none was (`pictured`): as what stands before a KVM answers for its stream until
    // somebody has woken its video by taking a picture.
    QHash<QByteArray, Answered> untilPictured;
    bool pictured = false;

    // What it was asked.
    QSet<QByteArray> passes;
    int signIns = 0;
    QList<QByteArray> did;
    QList<QUrlQuery> pictureAsks;
    // The most questions for a picture that were with it at one time: one that came while the
    // answer to one before it had not been written whole yet (it is written `pictureAfterMs` after
    // its question came) is a second at once. A KVM that answers at once never sees two: only one
    // that takes a while over a picture can tell.
    int mostPicturesAtOnce = 0;
    std::unique_ptr<StandIn> stand;

    QUrl address() const { return QUrl(stand->address()); }
    KvmdDevice::Where where() const
    {
        KvmdDevice::Where where;
        where.address = address();
        return where;
    }
    // How often something at this path was asked for, whatever stood behind its question mark.
    int asked(const QByteArray& path) const
    {
        int found = 0;
        for (const Asked& one : stand->asked) {
            found += one.path == path || one.path.startsWith(path + '?') ? 1 : 0;
        }
        return found;
    }
    const Asked* last(const QByteArray& path) const
    {
        for (auto one = stand->asked.crbegin(); one != stand->asked.crend(); ++one) {
            if (one->path == path || one->path.startsWith(path + '?')) {
                return &*one;
            }
        }
        return nullptr;
    }

private:
    // Its video rests: nobody is on a line that watches it, or it rests under one all the same.
    bool rests() const { return restsThoughWatched || (restsUntilWatched && !stand->live.isConnected()); }

    Answered signIn(const Asked& asked)
    {
        if (signInStatus != 200) {
            return refusal(signInStatus, u"ServerError"_s);
        }
        const QUrlQuery form(QString::fromUtf8(asked.body));
        if (form.queryItemValue(u"user"_s, QUrl::FullyDecoded) != u"admin"_s ||
            form.queryItemValue(u"passwd"_s, QUrl::FullyDecoded) != password) {
            return refusal(403, u"ForbiddenError"_s);
        }
        const QByteArray pass = "pass" + QByteArray::number(signIns);
        passes.insert(pass);
        if (passAsCookie) {
            Answered answer = good();
            answer.more = {"Set-Cookie: auth_token=" + pass + "; Path=/"};
            return answer;
        }
        return good({{u"token"_s, QString::fromLatin1(pass)}});
    }

    Answered answered(const Asked& asked)
    {
        const qsizetype mark = asked.path.indexOf('?');
        const QByteArray path = mark < 0 ? asked.path : asked.path.left(mark);
        const QUrlQuery query(mark < 0 ? QString() : QString::fromUtf8(asked.path.mid(mark + 1)));
        if (!httpsAt.isEmpty() && (!sendsOn || sendsOn(path))) {
            return Answered{301,
                            "<html><body>301 Moved Permanently</body></html>"_ba,
                            "text/html"_ba,
                            {"Location: "_ba + httpsAt.toUtf8() + asked.path}};
        }
        if (QList<Answered>& once = first[path]; !once.isEmpty()) {
            return once.takeFirst();
        }
        if (const auto resting = untilPictured.constFind(path); resting != untilPictured.constEnd() && !pictured) {
            return *resting;
        }
        if (const auto other = instead.constFind(path); other != instead.constEnd()) {
            return *other;
        }
        if (path == "/api/auth/login") {
            ++signIns;
            if (whenSignedInTo) {
                whenSignedInTo();
            }
            Answered said = signIn(asked);
            said.afterMs = std::max(said.afterMs, signInAfterMs);
            return said;
        }
        if (!glinet && (path == "/api/upgrade/version" || path == "/api/system/get_hostname")) {
            return refusal(404, u"NotFoundError"_s);
        }
        const QByteArray cookie = asked.headers.value("cookie");
        if (!cookie.startsWith("auth_token=") || !passes.contains(cookie.mid(11))) {
            return refusal(401, u"UnauthorizedError"_s);
        }
        if (path == "/api/auth/check") {
            return good();
        }
        if (path == "/api/streamer") {
            if (saysNothingOfAVideoThatRests && rests()) {
                Answered nothing = good({{u"streamer"_s, QJsonValue::Null}});
                nothing.afterMs = stateAfterMs;
                return nothing;
            }
            const QJsonObject source{{u"online"_s, signal},
                                     {u"captured_fps"_s, 60},
                                     {u"resolution"_s, QJsonObject{{u"width"_s, width}, {u"height"_s, height}}}};
            Answered state = good(
                {{u"streamer"_s, QJsonObject{{u"hdmi"_s, QJsonObject{{u"signal"_s, signal}}}, {u"source"_s, source}}}});
            state.afterMs = stateAfterMs;
            return state;
        }
        if (path == "/api/info") {
            return good({{u"health"_s, QJsonObject{{u"temp"_s, QJsonObject{{u"cpu"_s, 42.5}}}}}});
        }
        if (path == "/api/hid") {
            return good({{u"keyboard"_s, QJsonObject{{u"online"_s, keyboard}}},
                         {u"mouse"_s, QJsonObject{{u"online"_s, keyboard}, {u"absolute"_s, true}}}});
        }
        if (path == "/api/atx") {
            QJsonObject board{{u"enabled"_s, atx},
                              {u"busy"_s, boardBusy},
                              {u"leds"_s, QJsonObject{{u"power"_s, powerLight.value_or(atx)}, {u"hdd"_s, diskLight}}}};
            if (powerWord) {
                board.insert(u"power"_s, *powerWord);
            }
            return good(board);
        }
        if (path == "/api/upgrade/version") {
            return good({{u"model"_s, u"RM10"_s}, {u"version"_s, u"V1.10.1 release2"_s}});
        }
        if (path == "/api/system/get_hostname") {
            return good({{u"hostname"_s, u"glkvm"_s}, {u"success"_s, true}});
        }
        if (path == "/api/switch") {
            if (ports.isEmpty()) {
                return refusal(503, u"UnavailableError"_s);
            }
            QJsonArray list;
            QJsonArray links;
            for (const bool plugged : std::as_const(ports)) {
                list.append(QJsonObject{{u"name"_s, u""_s}});
                links.append(plugged);
            }
            return good({{u"model"_s, QJsonObject{{u"ports"_s, list}}},
                         {u"summary"_s, QJsonObject{{u"active_port"_s, shown}}},
                         {u"video"_s, QJsonObject{{u"links"_s, links}}}});
        }
        if (path == "/api/streamer/snapshot") {
            pictureAsks.append(query);
            // With it until its answer has been written (or its line has gone): told by the stand-in's writing, not
            // guessed by a clock of its own. (A Windows timer asked for 30 ms wrote one 15 ms after its question, and
            // the driver's next question, which came after the answer, was counted a second at once, 2026-11-19.)
            ++*m_picturesOut;
            mostPicturesAtOnce = std::max(mostPicturesAtOnce, *m_picturesOut);
            const auto over = [out = m_picturesOut] { --*out; };
            if (rests()) {
                Answered resting = refusal(503, u"UnavailableError"_s);
                resting.whenOver = over;
                return resting;
            }
            pictured = true;
            Answered answer(200, picture.isEmpty() ? jpeg(width, height) : picture, pictureKind);
            answer.more = {QByteArray("X-UStreamer-Online: ") + (signal ? "true" : "false")};
            answer.afterMs = pictureAfterMs;
            answer.whenOver = over;
            return answer;
        }
        if (path == "/api/fingerbot/battery" && asked.method == "GET") {
            if (!fingerbot) {
                return refusal(400, u"BadRequestError"_s, u"Fingerbot is not connected"_s);
            }
            did.append("battery");
            return good({{u"battery"_s, battery}});
        }
        if (path == "/api/fingerbot/click" && asked.method == "POST") {
            if (!fingerbot) {
                return refusal(400, u"BadRequestError"_s, u"Failed to click"_s);
            }
            did.append("fingerbot " + query.queryItemValue(u"press_time"_s).toUtf8() + " " +
                       query.queryItemValue(u"angle_enum"_s).toUtf8());
            return good({{u"result"_s, u"success"_s}});
        }
        if (path == "/api/wol/list") {
            QJsonArray devices;
            for (const QJsonObject& one : std::as_const(toWake)) {
                devices.append(one);
            }
            return good({{u"devices"_s, devices}});
        }
        if (path == "/api/msd" && asked.method == "GET" && hasDrive) {
            const QJsonObject state = msdState();
            for (auto gone = m_goneButListed.begin(); gone != m_goneButListed.end();) {
                gone = --m_listedFor[*gone] <= 0 ? m_goneButListed.erase(gone) : std::next(gone);
            }
            return good(state);
        }
        if (path.startsWith("/api/msd/") && asked.method == "POST" && hasDrive) {
            return msdChanged(path, asked);
        }
        if (asked.method != "POST") {
            return refusal(404, u"NotFoundError"_s);
        }
        if (path == "/api/atx/click") {
            if (!atx) {
                return refusal(400, u"BadRequestError"_s, u"ATX is not available"_s);
            }
            did.append("atx " + query.queryItemValue(u"button"_s).toUtf8());
            if (whenPressed) {
                whenPressed(query.queryItemValue(u"button"_s).toUtf8());
            }
            return good();
        }
        if (path == "/api/hid/events/send_shortcut") {
            did.append("keys " + query.queryItemValue(u"keys"_s).toUtf8());
            return good();
        }
        if (path == "/api/hid/print") {
            did.append("type " + asked.body);
            return good();
        }
        if (path == "/api/hid/set_params") {
            did.append("jiggler " + query.queryItemValue(u"jiggler"_s).toUtf8());
            return good();
        }
        if (path == "/api/switch/set_active") {
            const int port = query.queryItemValue(u"port"_s).toInt();
            if (port < 0 || port >= ports.size()) {
                return refusal(400, u"BadRequestError"_s, u"No such port"_s);
            }
            shown = port;
            did.append("port " + QByteArray::number(port));
            return good();
        }
        if (path == "/api/wol/wake") {
            did.append("wake " + query.queryItemValue(u"mac"_s).toUtf8());
            return good();
        }
        if (path == "/api/upgrade/reboot") {
            did.append("reboot");
            return good();
        }
        return refusal(404, u"NotFoundError"_s);
    }

    // A change of its drive or storage, as kvmd takes it (its msd API: none while it is busy with another; set_params
    // only while disconnected, an image it has; set_connected, a connect only while disconnected and a disconnect only
    // while connected, as stock kvmd and GL.iNet's take them, the web's stand-in too; remove; write).
    Answered msdChanged(const QByteArray& path, const Asked& asked)
    {
        Msd& drive = msd;
        const QString image = asKvmdReadsIt(asked.path, "image");
        if (drive.busy) {
            return refusal(409, u"MsdIsBusyError"_s, u"Performing another MSD operation, please try again"_s);
        }
        if (path == "/api/msd/set_params") {
            if (drive.connected) {
                return refusal(400, u"MsdConnectedError"_s,
                               u"Performing this operation is not allowed while the "
                               u"drive is connected"_s);
            }
            if (!drive.images.contains(image)) {
                return refusal(400, u"MsdUnknownImageError"_s, u"The image is not found in the storage"_s);
            }
            drive.chosen = image;
            drive.cdrom = !drive.mountsOtherwise && asKvmdReadsIt(asked.path, "cdrom") == "1"_L1;
            drive.rw = drive.mountsOtherwise || asKvmdReadsIt(asked.path, "rw") == "1"_L1;
            did.append("msd params " + image.toUtf8() + " cdrom " + asKvmdReadsIt(asked.path, "cdrom").toUtf8() +
                       " rw " + asKvmdReadsIt(asked.path, "rw").toUtf8());
            if (!drive.othersChoiceAfterParams.isEmpty()) {
                chooseForAnother(std::exchange(drive.othersChoiceAfterParams, {}));
            }
            if (std::exchange(drive.othersConnectAfterParams, false)) {
                drive.connected = true;
            }
            return good();
        }
        if (path == "/api/msd/set_connected") {
            const bool in = asKvmdReadsIt(asked.path, "connected") == "1"_L1;
            if (in && !drive.othersChoiceAtConnect.isEmpty()) {
                chooseForAnother(std::exchange(drive.othersChoiceAtConnect, {}));
            }
            if (in && !drive.othersMountAtConnect.isEmpty()) {
                chooseForAnother(std::exchange(drive.othersMountAtConnect, {}));
                drive.connected = true;
            }
            if (in && drive.connected) {
                return refusal(400, u"MsdConnectedError"_s,
                               u"MSD is connected to Server, but shouldn't for this operation"_s);
            }
            if (!in && !drive.connected) {
                return refusal(400, u"MsdDisconnectedError"_s,
                               u"MSD is disconnected from Server, but should be for this operation"_s);
            }
            if (!in && drive.disconnectRefused) {
                return refusal(409, u"MsdIsBusyError"_s, u"Performing another MSD operation, please try again"_s);
            }
            if (in || !drive.disconnectsNothing) {
                drive.connected = in;
            }
            did.append("msd connected " + asKvmdReadsIt(asked.path, "connected").toUtf8());
            if (std::optional<Answered>& readBack = in ? drive.stateAfterConnect : drive.stateAfterDisconnect;
                readBack) {
                first["/api/msd"_ba].prepend(*std::exchange(readBack, std::nullopt));
            }
            return good();
        }
        if (path == "/api/msd/remove") {
            if (!drive.images.contains(image)) {
                return refusal(400, u"MsdUnknownImageError"_s, u"The image is not found in the storage"_s);
            }
            drive.images.remove(image);
            if (drive.imagesUnreadAfterRemove) {
                drive.imagesSayInstead = QJsonValue(u"none"_s);
            }
            if (drive.listedLonger > 0) {
                m_goneButListed.append(image);
                m_listedFor[image] = drive.listedLonger;
            }
            did.append("msd remove " + image.toUtf8());
            return good();
        }
        if (path == "/api/msd/write") {
            drive.images.insert(
                image,
                QJsonObject{{u"size"_s, asked.body.size()}, {u"complete"_s, true}, {u"mod_ts"_s, 1'763'000'000.25}});
            did.append("msd write " + image.toUtf8() + " remove_incomplete " +
                       asKvmdReadsIt(asked.path, "remove_incomplete").toUtf8());
            return good(
                {{u"image"_s,
                  QJsonObject{{u"name"_s, image}, {u"size"_s, asked.body.size()}, {u"written"_s, asked.body.size()}}}});
        }
        return refusal(404, u"NotFoundError"_s);
    }

    // Another client of the KVM's choice of an image for its drive: as a USB flash drive, writable.
    void chooseForAnother(const QString& image)
    {
        msd.chosen = image;
        msd.cdrom = false;
        msd.rw = true;
    }

    // How many questions for a picture are with it now, unanswered (shared with the answers' ends, which the stand-in
    // may tell as it goes, after this KVM).
    std::shared_ptr<int> m_picturesOut = std::make_shared<int>(0);
    // Images it removed and lists a while longer (Msd::listedLonger), and for how many questions more.
    QList<QString> m_goneButListed;
    QHash<QString, int> m_listedFor;
};

// The line that wakes a resting KVM's video (kvmd's /api/ws?stream=1), as the cases that wake one wait for it. The
// driver asks for that line only where an asking finds the video resting, once it is signed in (resting(), which begins
// one where it keeps none, or keeps one asked for longer ago than its quietMs, as it answers "Video stream is idle -
// waking it up"), and a line not let in within its time, or ended, is asked for anew only at the next asking: a case
// that asked once found nothing woken where its first picture was answered "Connecting to the KVM" by its patience, or
// the line's first try was given up, as on the loaded build box in test_core_kvmd_device, test_core_kvmd_board,
// test_core_kvmd_fingerbot and test_core_kvmd_media. A card that shows the picture asks again; these ask again so, and
// count what may have begun a line, so that a case still holds "one line for one waking" (oneLineAWaking).

// Whether a picture was answered that the video rests and is being woken (resting()).
inline bool saysItWakes(const base::Result<Snapshot>& answer)
{
    return !answer && answer.error().code == "no_stream" &&
           answer.error().message == "Video stream is idle - waking it up";
}

// What an asking again came to, said as it is answered.
enum class Woke {
    nothing,     // nothing that can have begun a line: a picture came, or another answer
    maybe,       // it may have begun one: a picture answered that the video is being woken, or the line asked for
                 // where the driver kept none
    saidButNone, // a picture answered that the video is being woken while the driver kept no line for it: said, and
                 // nothing begun (resting() begins one as it says so)
};
// Asks once more, and says what that came to as it is answered.
using Again = std::function<void(std::function<void(Woke)> answered)>;

// What a wait for that line came to.
struct Waited {
    // Whether it came in its time.
    bool came = false;
    // How many askings may have begun a line, the case's own before the wait counted as one where the wait followed
    // it: one line for each at the most.
    int wakings = 0;
    // How many pictures said that the video is being woken while the driver kept no line for it: none, for a driver
    // that begins one as it says so (the askings again would hide one that began none).
    int saidButNone = 0;
    // What was seen, where it did not come.
    QString seen;

    explicit operator bool() const { return came && saidButNone == 0; }
    // Empty where it came, and every waking said began a line; what did not otherwise.
    QString why() const
    {
        if (saidButNone > 0) {
            return u"%1 pictures said the video is being woken while the driver kept no line for it"_s.arg(saidButNone);
        }
        return came ? QString() : seen;
    }
};

// One line for one waking: the lines the stand-in took for the line that wakes the video, against the askings that may
// have begun one (Waited::wakings): exactly one where at most one may have, as in nearly every run, and no more than
// one for each where more may have (a first try not let in in time, or a first picture answered by its patience, and
// asked again). Empty where it holds; what does not otherwise.
inline QString oneLineAWaking(int lines, int wakings)
{
    if (lines >= 1 && lines <= std::max(1, wakings)) {
        return {};
    }
    return u"%1 lines taken for the line that wakes the video, for %2 askings that may have begun one"_s.arg(lines).arg(
        wakings);
}

// A picture of the driver's asked again (an Again), and what its answer may have begun.
inline Again aPictureOf(KvmdDevice& device, int width = 400)
{
    return [&device, width](std::function<void(Woke)> answered) {
        device.askSnapshot(width, [&device, answered = std::move(answered)](const base::Result<Snapshot>& answer) {
            // (resting() begins the line before the answer is handed on: whether the driver keeps one is read here.)
            Woke woke = Woke::nothing;
            if (saysItWakes(answer)) {
                woke = device.isKeptAwake() ? Woke::maybe : Woke::saidButNone;
            }
            answered(woke);
        });
    };
}

// A case's own first picture of a KVM whose video rests, asked and waited for: its answer, and whether it said that
// the video is being woken while the driver kept no line for it (read as it was answered: a try is given up only its
// connectWithinMs later). What is asked again after it would hide a first waking that began no line.
struct FirstPicture {
    std::optional<base::Result<Snapshot>> answer;
    bool saidButNone = false;

    base::Result<Snapshot> said() const { return answer.value_or(base::Error{"never", "Never answered."}); }
};
inline FirstPicture firstPictureOf(KvmdDevice& device, int width = 400)
{
    // (Of its answer, which may come after this has ended: kept by it.)
    const auto first = std::make_shared<FirstPicture>();
    device.askSnapshot(width, [&device, first](const base::Result<Snapshot>& answer) {
        first->answer = answer;
        first->saidButNone = saysItWakes(answer) && !device.isKeptAwake();
    });
    static_cast<void>(QTest::qWaitFor([&first] { return first->answer.has_value(); }, 30'000));
    return *first;
}

// Asked again (`again`) a tenth of a second after the last asking again was answered, while `keepsALine` says the
// driver keeps no line that wakes the video, until `came` holds (the line asked for, or there), and nothing more once
// it holds, so that what a case counts after it is what the askings gave. After the case's own asking, counted as one
// that may have begun a line.
inline Waited askedAgainUntil(const std::function<bool()>& came, const std::function<bool()>& keepsALine,
                              const Again& again, int withinMs = 20'000)
{
    struct Asking {
        bool out = false;
        int wakings = 1;
        int saidButNone = 0;
    };
    // (Of the askings' answers, which may come after this has ended: kept by them.)
    const auto asking = std::make_shared<Asking>();
    Waited waited;
    waited.came = QTest::qWaitFor(
        [&] {
            if (came()) {
                return true;
            }
            if (!asking->out && !keepsALine()) {
                asking->out = true;
                again([asking](Woke woke) {
                    asking->wakings += woke == Woke::maybe ? 1 : 0;
                    asking->saidButNone += woke == Woke::saidButNone ? 1 : 0;
                    QTimer::singleShot(100, [asking] { asking->out = false; });
                });
            }
            return false;
        },
        withinMs);
    waited.wakings = asking->wakings;
    waited.saidButNone = asking->saidButNone;
    waited.seen =
        u"not come in %1 ms, after %2 askings that may have begun a line"_s.arg(withinMs).arg(asking->wakings);
    return waited;
}

// The same, until the stand-in's line (`live`) is the driver's own: in, with the driver keeping a line (`keepsALine`).
// That is so at once where nothing asked again may have begun a line: the case's asking made the one try there is.
// Where something asked again did, a try the driver gave up on may have been taken by the stand-in late, and stay there
// while the next is being made (a case's count or picture after it then went by that one): then only once the same
// line has stood so for `connectWithinMs` and two seconds more, by heldAwake's rule below. Meanwhile nothing is asked
// while the driver keeps a line, and the case's rest (restAfterMs) is to be longer than that; or, `keptWanted`, it is
// asked again a tenth of a second after each answer, as a card asks (a picture asked while the line is in is answered
// with one and begins nothing; a line asked for that is kept is not asked for anew). Done once no asking is out.
inline Waited theDriversOwnLine(const LiveStandIn& live, const std::function<bool()>& keepsALine, const Again& again,
                                int connectWithinMs, bool keptWanted = false, int withinMs = 20'000)
{
    struct Asking {
        bool out = false;
        bool unanswered = false;
        int wakings = 1;
        int saidButNone = 0;
    };
    const auto asking = std::make_shared<Asking>();
    int line = 0; // the stand-in's line, by its number, and how long it has stood so
    QElapsedTimer lineFor;
    Waited waited;
    waited.came = QTest::qWaitFor(
        [&] {
            const bool kept = keepsALine();
            const bool in = live.isConnected() && kept;
            if (in && !asking->unanswered) {
                if (asking->wakings <= 1) {
                    return true;
                }
                if (line != live.lines) {
                    line = live.lines;
                    lineFor.start();
                }
                if (lineFor.elapsed() > connectWithinMs + 2000) {
                    return true;
                }
            } else if (!in) {
                line = 0;
            }
            if (!asking->out && (!kept || (keptWanted && asking->wakings > 1))) {
                asking->out = true;
                asking->unanswered = true;
                again([asking](Woke woke) {
                    asking->unanswered = false;
                    asking->wakings += woke == Woke::maybe ? 1 : 0;
                    asking->saidButNone += woke == Woke::saidButNone ? 1 : 0;
                    QTimer::singleShot(100, [asking] { asking->out = false; });
                });
            }
            return false;
        },
        withinMs);
    waited.wakings = asking->wakings;
    waited.saidButNone = asking->saidButNone;
    waited.seen = u"not the driver's own in %1 ms, after %2 askings that may have begun a line: its line %3 (%4 "
                  u"taken), the driver %5"_s.arg(withinMs)
                      .arg(asking->wakings)
                      .arg(live.isConnected() ? u"in"_s : u"not in"_s)
                      .arg(live.lines)
                      .arg(keepsALine() ? u"keeping one"_s : u"keeping none"_s);
    return waited;
}

// The driver holds the KVM's line for events, as it does while somebody watches its picture: asked for one a tenth of a
// second after the last was answered, as a card that shows the picture asks, until it holds it, and so for as long as
// that takes (a rest of the pace's restAfterMs after the last picture lets the line go). Held means the stand-in's line
// is the driver's own, let in: the driver says it keeps a line where it is there or being made (isKeptAwake), and the
// stand-in speaks on the line it took last. Asked for again, a try the driver gave up on may be taken by the stand-in
// late, and stay there while the next is being made: what a case then says on the line goes to the one given up on. The
// driver lets a try in or gives it up within its `connectWithinMs` of beginning it, before the stand-in takes it: so
// the stand-in's line, the same one for that long and two seconds more with the driver keeping its line, is the
// driver's. Whether it holds it within 20 s, what was seen otherwise, and how many of its pictures may have begun a
// line (the first among them: no asking of the case's is before it). (Asked once, it held it only where its line came
// up at the first try, and a case's words on the line could go to a try given up on: test_core_kvmd_media,
// test_core_kvmd_board and test_core_kvmd_fingerbot failed so on the loaded build box.)
inline Waited heldAwake(KvmdDevice& device, MadeUpKvm& kvm, int connectWithinMs = 1500)
{
    kvm.restsUntilWatched = true;
    struct Asking {
        int asked = 0;
        int answered = 0;
        bool out = false;
        int wakings = 0;
        int saidButNone = 0;
    };
    const auto asking = std::make_shared<Asking>();
    const int decidedWithinMs = connectWithinMs + 2000;
    int line = 0; // the stand-in's line, by its number, and how long it has been there
    QElapsedTimer lineFor;
    Waited waited;
    waited.came = QTest::qWaitFor(
        [&] {
            if (kvm.stand->live.isConnected() && device.isKeptAwake()) {
                if (line != kvm.stand->live.lines) {
                    line = kvm.stand->live.lines;
                    lineFor.start();
                }
                if (lineFor.elapsed() > decidedWithinMs) {
                    return true;
                }
            } else {
                line = 0;
            }
            if (!asking->out) {
                asking->out = true;
                ++asking->asked;
                device.askSnapshot(400, [asking, &device](const base::Result<Snapshot>& answer) {
                    ++asking->answered;
                    // (resting() begins the line before the answer is handed on: whether the driver keeps one is read
                    // here.)
                    if (saysItWakes(answer) && device.isKeptAwake()) {
                        ++asking->wakings;
                    } else if (saysItWakes(answer)) {
                        ++asking->saidButNone;
                    }
                    QTimer::singleShot(100, [asking] { asking->out = false; });
                });
            }
            return false;
        },
        20'000);
    waited.wakings = asking->wakings;
    waited.saidButNone = asking->saidButNone;
    waited.seen = u"not held awake in 20 s: %1 pictures asked, %2 answered; its line %3 (%4 taken), the driver %5"_s
                      .arg(asking->asked)
                      .arg(asking->answered)
                      .arg(kvm.stand->live.isConnected() ? u"in"_s : u"not in"_s)
                      .arg(kvm.stand->live.lines)
                      .arg(device.isKeptAwake() ? u"keeping it awake"_s : u"keeping nothing awake"_s);
    return waited;
}

} // namespace qkvm::core::testing
