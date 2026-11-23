#pragma once

#include <qkvm/base/result.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

// One zlib stream (RFC 1950 around RFC 1951's deflate), read as it comes: what RFB's ZRLE and Tight encodings carry
// their pixels in. A server keeps one stream going for the whole connection (four, for Tight), and ends each
// rectangle's part of it where it likes; so a stream is taken up where it was left at any bit, and nothing of what was
// given is kept but the bits not yet read (at most a few bytes) and the last 32 KiB it inflated to, which a later
// part may copy from.
//
// Nothing here trusts what it is given: a code or a distance that is none, a table of codes that is not one, a
// copy from before the stream began, a checksum that is not the stream's, or more than the caller has room for, ends
// the stream with a named error, never with a guess. Plain C++ on the standard library alone; one thread at a time.
namespace qkvm::rfb {

// What one read of an Inflater (one call of inflate) is held to, so that a stream engineered to cost a great deal of
// processor time (a run of tiny dynamic blocks, each of which builds its tables anew) cannot hold the thread for long
// (outside review 2.1). Each is looked at before the work it bounds is done, never after.
struct InflateBounds {
    // The table slots the read may build in all: each table's are counted before it is built (tableWorkTaken's count),
    // and one that would go past this ends the read ("too_costly") with nothing of it built.
    std::uint64_t mostTableWork = std::numeric_limits<std::uint64_t>::max();
    // The blocks the read may begin: one past this ends it ("too_costly").
    std::uint64_t mostBlocks = std::numeric_limits<std::uint64_t>::max();
    // Asked as the stream is read, often enough that a read never runs long between two askings: before the tables of
    // every dynamic block, after every Inflater::askEveryBlocks other blocks, and after every askEverySymbols codes
    // read. True ends the read ("too_slow").
    std::function<bool()> overBudget;
};

class Inflater {
public:
    Inflater();

    using Bounds = InflateBounds;
    static constexpr std::uint64_t askEveryBlocks = 64;
    static constexpr std::uint64_t askEverySymbols = 65536;

    // Takes all of `data`, and adds what it inflates to at the end of `out`: never more than `most` bytes of it, within
    // `bounds`. Errors (after one, the stream is not read on): "garbled", what is no zlib stream of deflate's;
    // "too_long", it inflates past `most`; "too_costly", it would build more tables or begin more blocks than its
    // bounds allow; "too_slow", `overBudget` said so; "ended", the stream had ended before (its last block and its
    // checksum came), or bytes came after its end.
    base::Result<void> inflate(std::span<const std::uint8_t> data, std::size_t most, std::vector<std::uint8_t>& out,
                               const Bounds& bounds = {});
    // Its last block and its checksum have come: nothing more may.
    bool ended() const noexcept { return m_state == State::Done; }
    // The table slots built since this was last asked, and reset to nothing: what a dynamic block's tables cost, for
    // the caller to count towards a server's work (each slot is a write of a table 32,768 entries wide).
    std::uint64_t tableWorkTaken() noexcept { return std::exchange(m_builtSlots, std::uint64_t{0}); }
    // ...and the blocks begun since this was last asked, the same way.
    std::uint64_t blocksTaken() noexcept { return std::exchange(m_blocksBegun, std::uint64_t{0}); }

    // A table of codes, as deflate makes them from their lengths: for each pattern of the next 15 bits, the symbol
    // it begins with and that code's length (symbol << 4 | length), or 0 where none does.
    struct Codes {
        std::array<std::uint16_t, 1U << 15> entries{};
    };

private:
    enum class State {
        Header,
        Block,
        StoredLength,
        Stored,
        Counts,
        LengthCodes,
        Lengths,
        Literal,
        LengthExtra,
        Distance,
        DistanceExtra,
        Trailer,
        Done,
        Failed,
    };

    // The next `count` bits (at most 32) are there, taking from what was given as far as needed.
    bool need(int count) noexcept;
    std::uint32_t take(int count) noexcept;
    // The next symbol of `codes`: true with it; false where more bits are wanted (`m_error` set where none can come).
    bool decode(const Codes& codes, int& symbol);
    base::Result<void> fail(const char* code, const char* message);
    // One byte inflated: false where that is more than there is room for.
    bool put(std::uint8_t byte);
    // What a block's end leads to: the next block, or the checksum after the last.
    void blockEnded() noexcept;
    // A table of codes of these lengths may be built within the read's bounds: its slots counted (tableWorkTaken's
    // count and the read's) before it is built; false, and nothing counted, where they would go past the bounds.
    bool mayBuild(std::span<const std::uint8_t> lengths) noexcept;
    // The read's bounds say it may go on: asked of overBudget (false: "too_slow").
    bool goesOn();
    // What inflate reads, once it has taken what it was given and its bounds.
    base::Result<void> read();

    State m_state = State::Header;
    std::uint64_t m_bits = 0;
    int m_count = 0;
    const std::uint8_t* m_in = nullptr;
    const std::uint8_t* m_end = nullptr;
    std::vector<std::uint8_t>* m_out = nullptr;
    std::size_t m_room = 0;
    bool m_final = false;
    // A stored block: the bytes still to come.
    std::uint32_t m_left = 0;
    // A table of codes being read: how many of each, the code lengths' own codes, and the lengths so far.
    int m_literals = 0;
    int m_distances = 0;
    int m_lengthCodes = 0;
    int m_index = 0;
    int m_repeat = 0; // a repeat code read whose extra bits have not come yet (16, 17, 18)
    std::array<std::uint8_t, 19> m_lengthCodeLengths{};
    std::array<std::uint8_t, 320> m_lengths{};
    // The codes in use, and a copy being read: its length's symbol, its length, its distance's symbol.
    std::vector<Codes> m_tables; // [0] code lengths, [1] literals and lengths, [2] distances
    bool m_fixed = false;
    int m_symbol = 0;
    int m_length = 0;
    // The last 32 KiB inflated, how many bytes in all, and the checksum of them so far.
    std::vector<std::uint8_t> m_window;
    std::uint64_t m_total = 0;
    std::uint32_t m_adlerA = 1;
    std::uint32_t m_adlerB = 0;
    bool m_tooLong = false;
    // Table slots built and blocks begun since tableWorkTaken and blocksTaken; the read's bounds, the table slots and
    // blocks of this read, and the blocks and codes since overBudget was last asked.
    std::uint64_t m_builtSlots = 0;
    std::uint64_t m_blocksBegun = 0;
    const Bounds* m_bounds = nullptr;
    std::uint64_t m_readSlots = 0;
    std::uint64_t m_readBlocks = 0;
    std::uint64_t m_blocksSinceAsked = 0;
    std::uint64_t m_symbolsSinceAsked = 0;
};

} // namespace qkvm::rfb
