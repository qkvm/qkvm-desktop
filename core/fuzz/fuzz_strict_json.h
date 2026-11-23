#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonDocument>
#include <QJsonParseError>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/core/strict_json.h>

#include <cmath>
#include <cstdint>
#include <span>

// The harness of the program's own JSON reader, read by the book (fuzz_strict_json.cpp, libFuzzer's way in;
// test_core_fuzz_found, what it once found): strict_json.cpp is compiled into the fuzz target. It reads every word a
// JetKVM says (its signalling, JSON-RPC on `rpc`, the description in its answer) and every wall as a file: an input
// is one such text. Held to its promises: nothing nested deeper than it takes, no more values than it takes, every
// number one a PC's own numbers hold, no half a character in a text; and what it reads as an object or a list, Qt's own
// lenient reader reads too, with as many members.
//
// Its canary (fuzz_promise.h): a text holding a character beyond the first plane (its two halves, of escapes or of
// UTF-8), nested three deep or more (the seeds' is one list deep).
namespace qkvm::fuzz {

namespace jsons {

struct Walked {
    qsizetype values = 0;
    bool deepPair = false; // a character beyond the first plane in a text three deep or more
};

inline void walk(const core::json::Value& value, int depth, Walked& walked)
{
    qsizetype& values = walked.values;
    using Kind = core::json::Value::Kind;
    promise(depth <= core::json::deepestNesting, "a value nested deeper than is read");
    promise(++values <= core::json::mostValues, "more values than are read");
    switch (value.kind) {
    case Kind::Number:
        promise(std::isfinite(value.number), "a number no PC's numbers hold");
        break;
    case Kind::Text:
        for (qsizetype at = 0; at < value.text.size(); ++at) {
            const QChar one = value.text.at(at);
            if (one.isHighSurrogate()) {
                promise(at + 1 < value.text.size() && value.text.at(at + 1).isLowSurrogate(),
                        "half a character in a text");
                walked.deepPair = walked.deepPair || depth >= 3;
                ++at;
            } else {
                promise(!one.isLowSurrogate(), "half a character in a text");
            }
        }
        break;
    case Kind::List:
        for (const core::json::Value& one : value.list) {
            walk(one, depth + 1, walked);
        }
        break;
    case Kind::Object:
        for (const core::json::Member& member : value.members) {
            walk(member.value, depth + 1, walked);
            promise(value.find(member.name) != nullptr, "a member that cannot be found by its name");
        }
        break;
    default:
        break;
    }
}

} // namespace jsons

inline void readStrictJson(std::span<const std::uint8_t> input)
{
    const QByteArrayView text(reinterpret_cast<const char*>(input.data()), static_cast<qsizetype>(input.size()));
    const std::optional<qsizetype> signs = core::json::signsIn(text);
    const base::Result<core::json::Value> read = core::json::parse(text);
    if (!read.ok()) {
        return;
    }
    promise(signs.has_value(), "a text read whose bytes are no UTF-8");
    jsons::Walked walked;
    jsons::walk(read.value(), 1, walked);
    if (!read.value().isObject() && !read.value().isList()) {
        return;
    }
    QJsonParseError error{};
    const QJsonDocument theirs = QJsonDocument::fromJson(text.toByteArray(), &error);
    promise(error.error == QJsonParseError::NoError, "a text read by the book that Qt's reader refuses");
    if (walked.deepPair) {
        reached("a text holding a character beyond the first plane, nested three deep or more");
    }
}

} // namespace qkvm::fuzz
