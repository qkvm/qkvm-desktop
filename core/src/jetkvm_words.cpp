#include "jetkvm_words.h"

#include <algorithm>
#include <cmath>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core::jetkvm_words {

namespace {

// What its input may say of its size and rate, at the most (clamped).
constexpr int largestSide = 16384;
constexpr double mostFps = 1000;

// A device's text as one short line, or nothing (kvmd::plain, for what was read by the book).
QString plainOf(const json::Value* value, qsizetype limit)
{
    if (value == nullptr) {
        return {};
    }
    if (value->isText()) {
        return kvmd::plain(QJsonValue(value->text), limit);
    }
    if (value->isNumber()) {
        return kvmd::plain(QJsonValue(value->number), limit);
    }
    return {};
}

// A number it said, within reason: clamped to [0, most]; nothing that is no number is 0.
double clampedNumber(const json::Value* value, double most)
{
    if (value == nullptr || !value->isNumber() || !std::isfinite(value->number)) {
        return 0;
    }
    return std::clamp(value->number, 0.0, most);
}

} // namespace

Signalling signalling(QByteArrayView text, bool answered)
{
    Signalling read;
    // What is no JSON object (a "pong") is let go, as is what is not for this connection.
    const base::Result<json::Value> parsed = json::parse(text);
    if (!parsed || !parsed.value().isObject()) {
        return read;
    }
    const json::Value& message = parsed.value();
    const json::Value* type = message.find(u"type");
    if (!answered) {
        const json::Value* error = message.find(u"error");
        if (type == nullptr && error != nullptr && !(error->kind == json::Value::Kind::Null) &&
            !(error->isFlag() && !error->flag) && !(error->isText() && error->text.isEmpty())) {
            read.kind = Signalling::Kind::Refused;
            read.refusal = plainOf(error, 200);
            return read;
        }
        if (type != nullptr && type->isText() && type->text == u"answer"_s) {
            const json::Value* data = message.find(u"data");
            read.kind = Signalling::Kind::Answer;
            read.data = data != nullptr ? *data : json::Value{};
            return read;
        }
    }
    if (type != nullptr && type->isText() && type->text == u"new-ice-candidate"_s) {
        // Its address; the mid it says is let go (the library takes the bundle's).
        const json::Value* data = message.find(u"data");
        const json::Value* candidate = data != nullptr && data->isObject() ? data->find(u"candidate") : nullptr;
        if (candidate == nullptr || !candidate->isText()) {
            return read;
        }
        read.kind = Signalling::Kind::Address;
        read.address = candidate->text.toUtf8();
    }
    return read;
}

std::optional<QByteArray> answerSdp(const json::Value& data)
{
    if (!data.isText()) {
        return std::nullopt;
    }
    // (Its size before it is decoded: the reading of a9b950b, L4.)
    if (data.text.size() > qsizetype{256} * 1024) {
        return std::nullopt;
    }
    const auto decoded = QByteArray::fromBase64Encoding(data.text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        return std::nullopt;
    }
    const base::Result<json::Value> description = json::parse(decoded.decoded);
    const json::Value* sdp = description && description.value().isObject() ? description.value().find(u"sdp") : nullptr;
    if (sdp == nullptr || !sdp->isText() || sdp->text.isEmpty()) {
        return std::nullopt;
    }
    return sdp->text.toUtf8();
}

Rpc rpc(QByteArrayView bytes)
{
    Rpc read;
    const base::Result<json::Value> parsed = json::parse(bytes);
    if (!parsed || !parsed.value().isObject()) {
        return read;
    }
    const json::Value& said = parsed.value();
    // An answer: only to a number of ours that waits, a whole one (w14 row 1: in any order).
    if (const json::Value* id = said.find(u"id"); id != nullptr && id->isNumber()) {
        if (!id->whole || id->number < 1 || id->number > 9.0e15) {
            return read;
        }
        read.kind = Rpc::Kind::Answer;
        read.id = static_cast<qint64>(id->number);
        const json::Value* error = said.find(u"error");
        if (error != nullptr && error->kind != json::Value::Kind::Null) {
            read.refused = true;
            if (error->isObject()) {
                const json::Value* detail = error->find(u"data");
                read.words = plainOf(detail, 200);
                if (read.words.isEmpty()) {
                    read.words = plainOf(error->find(u"message"), 200);
                }
            } else {
                read.words = plainOf(error, 200);
            }
            return read;
        }
        const json::Value* result = said.find(u"result");
        read.result = result != nullptr ? *result : json::Value{};
        return read;
    }
    if (said.find(u"id") != nullptr && said.find(u"method") == nullptr) {
        return read; // (an answer under a number that is none of ours: nothing)
    }
    const json::Value* method = said.find(u"method");
    if (method == nullptr || !method->isText()) {
        return read;
    }
    const json::Value* params = said.find(u"params");
    if (method->text == u"otherSessionConnected"_s) {
        read.kind = Rpc::Kind::OtherSession;
    } else if (method->text == u"videoInputState"_s && params != nullptr && params->isObject()) {
        read.kind = Rpc::Kind::Video;
        const json::Value* ready = params->find(u"ready");
        if (ready != nullptr && ready->isFlag()) {
            read.ready = ready->flag;
        }
        read.width = static_cast<int>(clampedNumber(params->find(u"width"), largestSide));
        read.height = static_cast<int>(clampedNumber(params->find(u"height"), largestSide));
        read.fps = clampedNumber(params->find(u"fps"), mostFps);
    } else if (method->text == u"usbState"_s && params != nullptr && params->isText()) {
        read.kind = Rpc::Kind::Usb;
        read.words = plainOf(params, 40);
    } else if (method->text == u"atxState"_s && params != nullptr && params->isObject()) {
        read.kind = Rpc::Kind::Atx;
        read.result = *params;
    } else if (method->text == u"localVersion"_s && params != nullptr && params->isObject()) {
        read.kind = Rpc::Kind::Version;
        read.words = plainOf(params->find(u"appVersion"), 40);
    } else if (method->text == u"keysDownState"_s && params != nullptr && params->isObject()) {
        // (Where a JetKVM says its keyboard here rather than on hidrpc: the same word, taken the same way.)
        const json::Value* keys = params->find(u"keys");
        if (keys != nullptr && keys->isList() && keys->list.size() <= 16) {
            read.kind = Rpc::Kind::Keys;
            read.modifiers = static_cast<int>(clampedNumber(params->find(u"modifier"), 255));
            for (const json::Value& key : keys->list) {
                if (const int usage = static_cast<int>(clampedNumber(&key, 255)); usage != 0) {
                    read.keys.append(usage);
                }
            }
        }
    }
    // (The rest is let go.)
    return read;
}

bool otherSessionConnected(QByteArrayView text)
{
    if (!text.contains("otherSessionConnected")) {
        return false;
    }
    const base::Result<json::Value> said = json::parse(text);
    if (!said || !said.value().isObject()) {
        return false;
    }
    const json::Value* method = said.value().find(u"method");
    return method != nullptr && method->isText() && method->text == u"otherSessionConnected"_s;
}

Hid hid(QByteArrayView bytes)
{
    Hid read;
    if (bytes.isEmpty()) {
        return read;
    }
    const auto kind = static_cast<unsigned char>(bytes.at(0));
    if (kind == 0x01 && bytes.size() >= 2) {
        read.kind = bytes.at(1) == hidVersion ? Hid::Kind::Shaken : Hid::Kind::OtherVersion;
        return read;
    }
    if (kind == 0x33 && bytes.size() >= 2) {
        read.kind = Hid::Kind::Keys;
        read.modifiers = static_cast<unsigned char>(bytes.at(1));
        for (qsizetype at = 2; at < std::min<qsizetype>(bytes.size(), 8); ++at) {
            if (const auto key = static_cast<unsigned char>(bytes.at(at)); key != 0) {
                read.keys.append(key);
            }
        }
    }
    return read;
}

QByteArray passFrom(const QList<QNetworkCookie>& cookies)
{
    // (Two of that name are none, as a kvmd's driver has it: which of them is its session cannot be told.
    // The web's own cookie jar would take the last of two with one path; refusing both is the safer.)
    QByteArray pass;
    for (const QNetworkCookie& one : cookies) {
        if (one.name() == "authToken") {
            if (!pass.isEmpty() || !kvmd::isToken(one.value())) {
                return {};
            }
            pass = one.value();
        }
    }
    return pass;
}

qint64 comeBackAfterMs(const QByteArray& said, qint64 atMostMs)
{
    bool number = false;
    const double seconds = QString::fromLatin1(said).trimmed().toDouble(&number);
    if (!number || std::isnan(seconds) || seconds <= 0) {
        return 0;
    }
    if (std::isinf(seconds) || seconds * 1000 >= static_cast<double>(atMostMs)) {
        return atMostMs;
    }
    return static_cast<qint64>(seconds * 1000);
}

QString numberFrom(const QByteArray& answer)
{
    const QJsonValue number = QJsonDocument::fromJson(answer).object().value("deviceId"_L1);
    if (!number.isString()) {
        return {};
    }
    const QString text = number.toString();
    // (Counted in signs, as the web counts them: one beyond the first 65536 is one, not two.)
    const QList<uint> signs = text.toUcs4();
    if (signs.size() < 8 || signs.size() > 64 || QSet<uint>(signs.cbegin(), signs.cend()).size() < 4) {
        return {};
    }
    return u"jetkvm:"_s +
           QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

bool saysNotSetUp(const QByteArray& status)
{
    return QJsonDocument::fromJson(status).object().value("isSetup"_L1) == QJsonValue(false);
}

std::optional<QString> metadataVersion(const QByteArray& text)
{
    const QJsonObject heard = QJsonDocument::fromJson(text).object();
    if (heard.value("type"_L1).toString() != "device-metadata"_L1) {
        return std::nullopt;
    }
    return kvmd::plain(heard.value("data"_L1).toObject().value("deviceVersion"_L1), 40);
}

} // namespace qkvm::core::jetkvm_words
