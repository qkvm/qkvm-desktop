#include <qkvm/core/server_details.h>

#include <cmath>
#include <limits>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <qkvm/core/server_types.h>
#include <qkvm/core/strict_json.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {
namespace {
base::Error badDetails()
{
    return {"bad_answer", "The server answered in a way this program does not understand."};
}

std::optional<QJsonObject> objectFrom(const QByteArray& bytes)
{
    if (bytes.size() > largestAnswerBytes || !json::signsIn(bytes)) {
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    return document.isObject() ? std::optional<QJsonObject>(document.object()) : std::nullopt;
}

bool number(const QJsonValue& value, double& into, double least, double most)
{
    if (!value.isDouble()) {
        return false;
    }
    into = value.toDouble();
    return std::isfinite(into) && into >= least && into <= most;
}

bool optionalNumber(const QJsonValue& value, std::optional<double>& into, double most)
{
    if (value.isNull()) {
        into.reset();
        return true;
    }
    double parsed = 0;
    if (!number(value, parsed, 0, most)) {
        return false;
    }
    into = parsed;
    return true;
}

bool count(const QJsonValue& value, int& into)
{
    double parsed = 0;
    if (!number(value, parsed, 0, std::numeric_limits<int>::max()) || std::floor(parsed) != parsed) {
        return false;
    }
    into = static_cast<int>(parsed);
    return true;
}

bool text(const QJsonValue& value, QString& into, qsizetype most, bool required = false)
{
    if (value.isNull() || value.isUndefined()) {
        return !required;
    }
    if (!value.isString()) {
        return false;
    }
    into = value.toString();
    return into.size() <= most && (!required || !into.isEmpty());
}

// What an entry says of a screen watch's alert (b7), where it says anything of it: false where what it
// says is not of the server's shape. A watch this program does not know is one added since, kept as the
// server said it (README, "Clients and versions"); a stamp that is not one of the server's (only digits)
// names no picture: it would go into an address.
bool screenWatchOf(const QJsonObject& entry, ActivityEntry& parsed)
{
    QString watch;
    QString stamp;
    if (!text(entry["watch"_L1], watch, 32) || !text(entry["picture"_L1], stamp, 32)) {
        return false;
    }
    parsed.watch = watch;
    static const QRegularExpression isStamp(u"\\A[0-9]{1,16}\\z"_s);
    parsed.picture = isStamp.match(stamp).hasMatch() ? stamp : QString();
    if (const QJsonValue minutes = entry["minutes"_L1]; !minutes.isUndefined() && !minutes.isNull()) {
        int counted = 0;
        if (!count(minutes, counted) || counted > 1'000'000) {
            return false;
        }
        parsed.minutes = counted;
    }
    if (const QJsonValue back = entry["back"_L1]; !back.isUndefined() && !back.isNull()) {
        if (!back.isBool()) {
            return false;
        }
        parsed.back = back.toBool();
    }
    std::optional<double> at;
    if (!optionalNumber(entry["picture_at"_L1].isUndefined() ? QJsonValue() : entry["picture_at"_L1], at, 1e11)) {
        return false;
    }
    parsed.pictureAt = parsed.picture.isEmpty() ? std::nullopt : at;
    return true;
}

bool window(const QJsonObject& object, double& since, double& until)
{
    return number(object["since"_L1], since, 0, 1e11) && number(object["until"_L1], until, since, 1e11);
}
} // namespace

base::Result<HistorySummary> historySummaryFrom(const QByteArray& bytes)
{
    const auto object = objectFrom(bytes);
    if (!object) {
        return badDetails();
    }
    const auto& value = *object;
    HistorySummary result;
    if (!window(value, result.since, result.until) ||
        !number(value["watched"_L1], result.watched, 0, result.until - result.since + 1) ||
        !number(value["online"_L1], result.online, 0, result.watched + 1) ||
        !optionalNumber(value["share"_L1], result.share, 1) ||
        !optionalNumber(value["first_seen"_L1], result.firstSeen, 1e11) ||
        !optionalNumber(value["last_seen"_L1], result.lastSeen, 1e11) || !text(value["state"_L1], result.state, 100) ||
        !count(value["changes"_L1], result.changes) || !count(value["events"_L1], result.events) ||
        !value["sensors"_L1].isArray()) {
        return badDetails();
    }
    const auto sensors = value["sensors"_L1].toArray();
    if (sensors.size() > 64) {
        return badDetails();
    }
    for (const auto& sensor : sensors) {
        QString name;
        if (!text(sensor, name, 1024, true)) {
            return badDetails();
        }
        result.sensors.append(name);
    }
    return result;
}

base::Result<HistorySamples> historySamplesFrom(const QByteArray& bytes)
{
    const auto object = objectFrom(bytes);
    if (!object) {
        return badDetails();
    }
    const auto& value = *object;
    HistorySamples result;
    double every = 0;
    if (!window(value, result.since, result.until) || !number(value["every"_L1], every, 1, 86400) ||
        std::floor(every) != every || !value["series"_L1].isArray() || !value["more"_L1].isBool()) {
        return badDetails();
    }
    result.more = value["more"_L1].toBool();
    const auto series = value["series"_L1].toArray();
    if (series.size() > 64) {
        return badDetails();
    }
    qsizetype total = 0;
    for (const auto& item : series) {
        if (!item.isObject()) {
            return badDetails();
        }
        const auto one = item.toObject();
        HistorySeries line;
        if (!text(one["sensor"_L1], line.sensor, 1024, true) || !one["unit"_L1].isString() ||
            !text(one["unit"_L1], line.unit, 64) || !one["points"_L1].isArray()) {
            return badDetails();
        }
        const auto points = one["points"_L1].toArray();
        total += points.size();
        if (total > 5000) {
            return badDetails();
        }
        double previous = -1;
        for (const auto& point : points) {
            const auto parts = point.toArray();
            HistoryPoint parsed;
            if (!point.isArray() || parts.size() != 4 || !number(parts[0], parsed.at, result.since, result.until) ||
                parsed.at <= previous || !number(parts[1], parsed.value, -1e12, 1e12) ||
                !number(parts[2], parsed.low, -1e12, parsed.value) ||
                !number(parts[3], parsed.high, parsed.value, 1e12)) {
                return badDetails();
            }
            previous = parsed.at;
            line.points.append(parsed);
        }
        result.series.append(std::move(line));
    }
    return result;
}

base::Result<DeviceActivity> deviceActivityFrom(const QByteArray& bytes)
{
    const auto object = objectFrom(bytes);
    if (!object) {
        return badDetails();
    }
    const auto& value = *object;
    DeviceActivity result;
    if (!value["entries"_L1].isArray() || !value["more"_L1].isBool()) {
        return badDetails();
    }
    const auto missing = value["missing"_L1];
    if (!missing.isNull()) {
        const auto gap = missing.toObject();
        double since = 0;
        QString why;
        if (!missing.isObject() || !count(gap["count"_L1], result.missing) ||
            !number(gap["since"_L1], since, 0, 1e11) || !text(gap["why"_L1], why, 1024, true)) {
            return badDetails();
        }
    }
    result.more = value["more"_L1].toBool();
    const auto entries = value["entries"_L1].toArray();
    if (entries.size() > 100) {
        return badDetails();
    }
    for (const auto& item : entries) {
        if (!item.isObject()) {
            return badDetails();
        }
        const auto entry = item.toObject();
        ActivityEntry parsed;
        if (!number(entry["time"_L1], parsed.at, 0, 1e11) || !text(entry["user"_L1], parsed.user, 256) ||
            !text(entry["event"_L1], parsed.event, 100, true) || !text(entry["name"_L1], parsed.name, 1024) ||
            !text(entry["action"_L1], parsed.action, 256) || !text(entry["state"_L1], parsed.state, 100) ||
            !text(entry["was"_L1], parsed.was, 100) || !text(entry["from"_L1], parsed.from, 256) ||
            !text(entry["by"_L1], parsed.by, 256) || !text(entry["why"_L1], parsed.why, 100)) {
            return badDetails();
        }
        if (entry.contains("on"_L1)) {
            if (!entry["on"_L1].isBool()) {
                return badDetails();
            }
            parsed.on = entry["on"_L1].toBool();
        }
        if (!screenWatchOf(entry, parsed)) {
            return badDetails();
        }
        // (A sentence of the server's, where it is one: no more than a line can say.)
        for (const char* field : {"error", "note"}) {
            if (const QJsonValue sentence = entry[QLatin1StringView(field)];
                parsed.said.isEmpty() && sentence.isString()) {
                parsed.said = sentence.toString().left(600).simplified();
            }
        }
        result.entries.append(std::move(parsed));
    }
    return result;
}
} // namespace qkvm::core
