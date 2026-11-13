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
        m_clock.start();
        const auto answer = [this](const Asked& asked) { return answered(asked); };
        stand = tls ? std::make_unique<StandIn>(answer, *tls, port) : std::make_unique<StandIn>(answer, port);
    }

    // What it is.
    QString password = u"secret"_s;
    bool passAsCookie = false; // as the other maker hands it over
    int signInStatus = 200;
    int signInAfterMs = 0; // its sign-ins are answered only after this long, a yes and a no alike
    bool signal = true;
    int width = 1920;
    int height = 1080;
    bool atx = false;
    // Its power board, as GLKVM says it: a word of its own for the computer ("on", "off", "sleep") beside a light
    // that stays off on a real Comet whatever the computer does. None: as stock kvmd says it, the light alone. And
    // the light itself (none: as `atx`), and whether the board is pressing a button at the moment.
    std::optional<QString> powerWord;
    std::optional<bool> powerLight;
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
    // answer to one before it had not been sent yet (it is sent `pictureAfterMs` after its
    // question came) is a second at once. A KVM that answers at once never sees two: only one
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
                return good({{u"streamer"_s, QJsonValue::Null}});
            }
            const QJsonObject source{{u"online"_s, signal},
                                     {u"captured_fps"_s, 60},
                                     {u"resolution"_s, QJsonObject{{u"width"_s, width}, {u"height"_s, height}}}};
            return good(
                {{u"streamer"_s, QJsonObject{{u"hdmi"_s, QJsonObject{{u"signal"_s, signal}}}, {u"source"_s, source}}}});
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
                              {u"leds"_s, QJsonObject{{u"power"_s, powerLight.value_or(atx)}, {u"hdd"_s, false}}}};
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
            const qint64 now = m_clock.elapsed();
            m_picturesOutUntil.removeIf([now](qint64 until) { return until <= now; });
            m_picturesOutUntil.append(now + (rests() ? 0 : pictureAfterMs));
            mostPicturesAtOnce = std::max(mostPicturesAtOnce, static_cast<int>(m_picturesOutUntil.size()));
            if (rests()) {
                return refusal(503, u"UnavailableError"_s);
            }
            pictured = true;
            Answered answer(200, picture.isEmpty() ? jpeg(width, height) : picture, pictureKind);
            answer.more = {QByteArray("X-UStreamer-Online: ") + (signal ? "true" : "false")};
            answer.afterMs = pictureAfterMs;
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

    // For telling how many questions for a picture are with it at once: its own clock, and until
    // when each such question is unanswered.
    QElapsedTimer m_clock;
    QList<qint64> m_picturesOutUntil;
};

} // namespace qkvm::core::testing
