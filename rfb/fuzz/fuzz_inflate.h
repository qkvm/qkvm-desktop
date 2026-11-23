#pragma once

#include <QByteArray>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/rfb/inflate.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The harness of the reader's own zlib inflater (fuzz_inflate.cpp, libFuzzer's way in; test_core_fuzz_found, what it
// once found), which the owner kept on the condition that it is fuzzed (2026-11-14). An input is three bytes that say
// how it is read, then a zlib stream: the room each part may inflate to ((first + 1) * 1024 bytes), the size of the
// parts it is fed in (1 to 256, or whole where 0), and after how many questions the budget says it is spent (never
// where 0). Held to its promises: no part inflates past its room, nothing is taken once it has refused or ended, and a
// stream it reads to its end is what Qt's own zlib makes of it, byte for byte.
//
// Its canary (fuzz_promise.h): a stream of a block with codes of its own (a dynamic block: its tables built), fed in
// two parts or more, so taken up again where a part ended, read to its checksum and found to be what zlib makes of it
// (every seed of such a block is fed whole).
namespace qkvm::fuzz {

inline void readInflate(std::span<const std::uint8_t> input)
{
    if (input.size() < 3) {
        return;
    }
    const std::size_t most = (std::size_t{input[0]} + 1) * 1024;
    const std::size_t step = input[1] == 0 ? input.size() : std::size_t{input[1]};
    const int budget = input[2];
    const std::span<const std::uint8_t> stream = input.subspan(3);
    rfb::Inflater inflater;
    int asked = 0;
    // (Its bounds as a reader's of a whole stream: no limit on its tables or blocks, only the budget asked.)
    rfb::Inflater::Bounds bounds;
    bounds.overBudget = [&asked, budget] { return budget != 0 && ++asked >= budget; };
    std::vector<std::uint8_t> out;
    bool refused = false;
    std::uint64_t tables = 0;
    for (std::size_t at = 0; at < stream.size(); at += step) {
        const std::size_t before = out.size();
        const base::Result<void> read =
            inflater.inflate(stream.subspan(at, std::min(step, stream.size() - at)), most, out, bounds);
        tables += inflater.tableWorkTaken();
        promise(out.size() - before <= most, "a part inflated past its room");
        if (!read.ok()) {
            const std::string& code = read.error().code;
            promise(code == "garbled" || code == "too_long" || code == "too_slow" || code == "ended",
                    "a stream was refused for no reason there is");
            refused = true;
            break;
        }
    }
    if (refused) {
        std::vector<std::uint8_t> more;
        promise(!inflater.inflate(stream.first(std::min<std::size_t>(stream.size(), 1)), most, more).ok() &&
                    more.empty(),
                "a stream refused once was read on");
        return;
    }
    if (!inflater.ended()) {
        return;
    }
    // Read to its end: Qt's zlib, told the size, makes the same of it.
    QByteArray framed;
    const auto size = static_cast<std::uint32_t>(out.size());
    for (const int shift : {24, 16, 8, 0}) {
        framed.append(static_cast<char>((size >> shift) & 0xFF));
    }
    framed.append(reinterpret_cast<const char*>(stream.data()), static_cast<qsizetype>(stream.size()));
    const QByteArray theirs = qUncompress(framed);
    promise(theirs.size() == static_cast<qsizetype>(out.size()) &&
                std::equal(out.begin(), out.end(), reinterpret_cast<const std::uint8_t*>(theirs.constData())),
            "a stream read to its end is not what zlib makes of it");
    if (tables > 0 && input[1] != 0 && step < stream.size() && !out.empty()) {
        reached("a stream of a dynamic block, fed in two parts or more, read to its end as zlib reads it");
    }
}

} // namespace qkvm::fuzz
