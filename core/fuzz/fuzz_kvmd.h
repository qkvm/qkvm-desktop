#pragma once

#include <QByteArray>
#include <QChar>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QNetworkCookie>
#include <QString>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/core/kvmd_answers.h>

#include <cstdint>
#include <span>

#include "fuzz_status.h"

// The harness of what a KVM that runs kvmd answers (PiKVM, GL.iNet's Comet), and what it says on its line for events
// (fuzz_kvmd.cpp, libFuzzer's way in; test_core_fuzz_found, what it once found): kvmd_answers.cpp and what it reads
// with are compiled into the fuzz target. An input is one byte that says the answer's HTTP status (fuzz_status.h: 0 is
// 200) and the longest line a text of it is cut to (its value, 1 to 256), then the answer: every reader of the driver
// is given it, as an answer, as the result in it, as a message on the line for events, as a picture, as a cookie's
// value. Held to the readers' promises: a text of the KVM's is one line that can be shown and no longer than it may
// be, a pass is one that can go back in a header, a computer's list is of its most.
//
// Its canary (fuzz_promise.h): a KVM switch's computers read from the result of an answer, the one shown the third or a
// later one, with a computer on it (the seeds' switch shows its second).
namespace qkvm::fuzz {

namespace kvmds {

// What kvmd::plain makes of a text: one line of what prints, a blank between words, no longer than `limit` (cut there,
// so that a blank may end it, as the web's plain cuts what it joined).
inline void line(const QString& said, qsizetype limit)
{
    promise(said.size() <= limit, "a text of the KVM's is longer than its line");
    const QString uncut = said.endsWith(u' ') ? said.chopped(1) : said;
    promise(uncut == uncut.simplified(), "a text of the KVM's holds blanks that are not one blank");
    for (const QChar one : said) {
        promise(one == u' ' || (one.category() != QChar::Other_Control && one.category() != QChar::Separator_Line &&
                                one.category() != QChar::Separator_Paragraph),
                "a text of the KVM's holds what does not print");
    }
}

inline void pass(const QByteArray& token)
{
    promise(token.isEmpty() || core::kvmd::isToken(token), "a pass that cannot go back in a header");
}

} // namespace kvmds

inline void readKvmd(std::span<const std::uint8_t> input)
{
    namespace kvmd = core::kvmd;
    if (input.empty()) {
        return;
    }
    const int status = statusOf(input[0]);
    const qsizetype limit = qsizetype{input[0]} + 1;
    const QByteArray answer(reinterpret_cast<const char*>(input.data()) + 1, static_cast<qsizetype>(input.size() - 1));
    const QString text = QString::fromUtf8(answer);
    kvmds::line(kvmd::plain(QJsonValue(text), limit), limit);
    static_cast<void>(kvmd::isKvmd(answer));
    kvmds::pass(kvmd::tokenFrom(answer, {}));
    QNetworkCookie cookie(QByteArrayLiteral("auth_token"), answer.left(600));
    kvmds::pass(kvmd::tokenFrom(answer, {cookie}));
    kvmds::pass(kvmd::tokenFrom(answer, {cookie, cookie}));
    const base::Error refusal = kvmd::refusalOf(status, answer);
    promise(refusal.message.size() <= 4096, "a refusal longer than any sentence");
    if (const std::optional<QSize> size = kvmd::pictureSize(answer)) {
        static_cast<void>(kvmd::isShown(*size));
    }
    if (const std::optional<QJsonObject> heard = kvmd::boardHeard(answer)) {
        static_cast<void>(kvmd::powerSaid(*heard));
        static_cast<void>(kvmd::boardIsThere(*heard));
    }
    static_cast<void>(kvmd::fingerbotHeard(answer));
    const base::Result<QJsonObject> result = kvmd::resultFrom(answer);
    if (!result.ok()) {
        return;
    }
    const QJsonObject& said = result.value();
    const kvmd::About about = kvmd::aboutFrom(said);
    kvmds::line(about.model, 40);
    kvmds::line(about.version, 40);
    static_cast<void>(kvmd::hostnameFrom(said));
    const std::optional<kvmd::Ports> ports = kvmd::portsFrom(said);
    promise(!ports || (ports->list.size() <= kvmd::mostPorts && ports->shown < ports->list.size()),
            "a KVM's computers are more than it may have, or the one shown is none of them");
    if (ports && ports->shown >= 2 && !ports->list.at(ports->shown).empty) {
        reached("a switch's computers read, the one shown the third or later, a computer on it");
    }
    static_cast<void>(kvmd::powerSaid(said));
    static_cast<void>(kvmd::boardIsThere(said));
    if (const std::optional<int> battery = kvmd::batterySaid(said)) {
        promise(*battery >= 0 && *battery <= 255, "a battery of no reading");
    }
    static_cast<void>(kvmd::sizeFor(320, kvmd::sourceWidthFrom(said)));
    static_cast<void>(kvmd::wakeTargetsFrom(said));
    kvmd::Looked looked;
    looked.streamer = said;
    static_cast<void>(kvmd::statusFrom(looked));
    looked.info = said;
    looked.hid = said;
    looked.atx = said;
    looked.about = about;
    looked.ports = ports;
    static_cast<void>(kvmd::statusFrom(looked));
    // Each of its parts as the part it would be of an answer: its streamer, its hardware, its keyboard, its board.
    for (auto at = said.begin(); at != said.end(); ++at) {
        if (!at.value().isObject()) {
            continue;
        }
        const QJsonObject part = at.value().toObject();
        looked.streamer = part;
        looked.info = part;
        looked.hid = part;
        looked.atx = part;
        static_cast<void>(kvmd::statusFrom(looked));
        static_cast<void>(kvmd::portsFrom(part));
        static_cast<void>(kvmd::wakeTargetsFrom(part));
    }
}

} // namespace qkvm::fuzz
