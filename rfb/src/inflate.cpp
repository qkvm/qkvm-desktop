#include <qkvm/rfb/inflate.h>

#include <algorithm>
#include <cassert>

namespace qkvm::rfb {

namespace {

constexpr int mostBits = 15;
constexpr std::size_t windowBytes = std::size_t{1} << 15;
constexpr std::uint32_t adlerBase = 65521;

// The order the code lengths' own code lengths come in (RFC 1951, 3.2.7).
constexpr std::array<std::uint8_t, 19> lengthCodeOrder{16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                       11, 4,  12, 3, 13, 2, 14, 1, 15};
// Lengths of a copy, and their extra bits, by symbol from 257; distances by symbol (3.2.5).
constexpr std::array<std::uint16_t, 29> lengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> lengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> distanceBase{1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                     33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> distanceExtra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

enum class Kind { LengthCodes, Other };

// The table slots that building the table of codes of these lengths writes (the clear of the whole table, and each
// code's entries), counted before it is built: what `build` writes, at the most.
std::uint64_t slotsOf(std::span<const std::uint8_t> lengths) noexcept
{
    std::uint64_t slots = std::uint64_t{1} << mostBits;
    for (const std::uint8_t length : lengths) {
        if (length != 0 && length <= mostBits) {
            slots += std::uint64_t{1} << (mostBits - length);
        }
    }
    return slots;
}

// The table of codes that `lengths` make, as deflate makes them (canonical, shortest first, then by symbol): false
// where the lengths make no code (more codes of a length than there is room for; a set that leaves room unused, but
// for one code of one bit, or none at all, which RFC 1951 allows of distances and zlib of both; for the code lengths'
// own code, none is allowed to). What it costs was counted before (slotsOf).
bool build(std::span<const std::uint8_t> lengths, Kind kind, Inflater::Codes& codes)
{
    std::array<int, mostBits + 1> many{};
    for (const std::uint8_t length : lengths) {
        ++many[length];
    }
    many[0] = 0;
    int longest = 0;
    for (int length = 1; length <= mostBits; ++length) {
        if (many[length] != 0) {
            longest = length;
        }
    }
    int room = 1;
    for (int length = 1; length <= mostBits; ++length) {
        room = room * 2 - many[length];
        if (room < 0) {
            return false; // over-subscribed
        }
    }
    if (room > 0 && (kind == Kind::LengthCodes || longest > 1)) {
        return false; // incomplete
    }
    // The first code of each length.
    std::array<int, mostBits + 2> next{};
    int code = 0;
    for (int length = 1; length <= mostBits; ++length) {
        code = (code + many[length - 1]) << 1;
        next[length] = code;
    }
    codes.entries.fill(0);
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        const int length = lengths[symbol];
        if (length == 0) {
            continue;
        }
        // Deflate sends a code's first bit first, into the low bits: the table is looked up by the bits reversed.
        int given = next[length]++;
        int reversed = 0;
        for (int bit = 0; bit < length; ++bit) {
            reversed = (reversed << 1) | (given & 1);
            given >>= 1;
        }
        const auto entry = static_cast<std::uint16_t>((symbol << 4) | static_cast<std::size_t>(length));
        for (int at = reversed; at < (1 << mostBits); at += 1 << length) {
            codes.entries[static_cast<std::size_t>(at)] = entry;
        }
    }
    return true;
}

// The fixed codes (3.2.6), made once.
const std::array<Inflater::Codes, 2>& fixedCodes()
{
    static const std::array<Inflater::Codes, 2> made = [] {
        std::array<Inflater::Codes, 2> both{};
        std::array<std::uint8_t, 288> literals{};
        std::fill(literals.begin(), literals.begin() + 144, std::uint8_t{8});
        std::fill(literals.begin() + 144, literals.begin() + 256, std::uint8_t{9});
        std::fill(literals.begin() + 256, literals.begin() + 280, std::uint8_t{7});
        std::fill(literals.begin() + 280, literals.end(), std::uint8_t{8});
        // (Thirty-two distances of five bits, the last two of which mean nothing: a code that is complete.)
        std::array<std::uint8_t, 32> distances{};
        distances.fill(5);
        const bool complete = build(literals, Kind::Other, both[0]) && build(distances, Kind::Other, both[1]);
        static_cast<void>(complete);
        assert(complete);
        return both;
    }();
    return made;
}

} // namespace

// (Its tables and its window are made when the stream needs them: a Tight server's four streams, ZRLE's beside them,
// may never all be used.)
Inflater::Inflater() = default;

bool Inflater::need(int count) noexcept
{
    while (m_count < count && m_in != m_end) {
        m_bits |= std::uint64_t{*m_in} << m_count;
        ++m_in;
        m_count += 8;
    }
    return m_count >= count;
}

std::uint32_t Inflater::take(int count) noexcept
{
    const auto value = static_cast<std::uint32_t>(m_bits & ((std::uint64_t{1} << count) - 1));
    m_bits >>= count;
    m_count -= count;
    return value;
}

bool Inflater::decode(const Codes& codes, int& symbol)
{
    // As many of the next 15 bits as there are: those not there yet are nought here, and a code is prefix-free, so the
    // entry they find is the code's where the code is no longer than the bits that are there.
    static_cast<void>(need(mostBits));
    const std::uint16_t entry = codes.entries[static_cast<std::size_t>(m_bits & ((1U << mostBits) - 1))];
    const int length = entry & 15;
    if (entry == 0 || length > m_count) {
        if (m_count >= mostBits) {
            m_state = State::Failed; // a pattern that begins no code
        }
        return false;
    }
    static_cast<void>(take(length));
    symbol = entry >> 4;
    return true;
}

base::Result<void> Inflater::fail(const char* code, const char* message)
{
    m_state = State::Failed;
    return base::Error{code, message};
}

bool Inflater::put(std::uint8_t byte)
{
    if (m_room == 0) {
        m_tooLong = true;
        return false;
    }
    --m_room;
    m_out->push_back(byte);
    m_window[static_cast<std::size_t>(m_total % windowBytes)] = byte;
    ++m_total;
    m_adlerA += byte;
    if (m_adlerA >= adlerBase) {
        m_adlerA -= adlerBase;
    }
    m_adlerB += m_adlerA;
    if (m_adlerB >= adlerBase) {
        m_adlerB -= adlerBase;
    }
    return true;
}

void Inflater::blockEnded() noexcept
{
    m_state = m_final ? State::Trailer : State::Block;
}

bool Inflater::mayBuild(std::span<const std::uint8_t> lengths) noexcept
{
    const std::uint64_t slots = slotsOf(lengths);
    if (m_readSlots + slots > m_bounds->mostTableWork) {
        return false;
    }
    m_readSlots += slots;
    m_builtSlots += slots;
    return true;
}

bool Inflater::goesOn()
{
    m_blocksSinceAsked = 0;
    m_symbolsSinceAsked = 0;
    return !m_bounds->overBudget || !m_bounds->overBudget();
}

base::Result<void> Inflater::inflate(std::span<const std::uint8_t> data, std::size_t most,
                                     std::vector<std::uint8_t>& out, const Bounds& bounds)
{
    if (m_state == State::Done) {
        return fail("ended", "a zlib stream used after its end");
    }
    if (m_state == State::Failed) {
        return base::Error{"garbled", "a zlib stream that was not read on after an error"};
    }
    m_in = data.data();
    m_end = data.data() + data.size();
    m_out = &out;
    m_room = most;
    m_tooLong = false;
    // The read's bounds, for this call alone (outside review 2.1): its table slots and blocks counted from nothing, and
    // the bounds let go of again whatever it comes to.
    m_bounds = &bounds;
    m_readSlots = 0;
    m_readBlocks = 0;
    base::Result<void> taken = read();
    m_bounds = nullptr;
    return taken;
}

base::Result<void> Inflater::read()
{
    const auto garbled = [this](const char* message) { return fail("garbled", message); };
    const auto tooLong = [this] { return fail("too_long", "a zlib stream that inflates past what there is room for"); };
    const auto tooCostly = [this] { return fail("too_costly", "a zlib stream that costs far more to read than may"); };
    const auto tooSlow = [this] { return fail("too_slow", "a zlib stream that costs far too much time to read"); };
    const Codes* literals = nullptr;
    const Codes* distances = nullptr;
    const auto inUse = [&] {
        literals = m_fixed ? &fixedCodes()[0] : m_tables.empty() ? nullptr : &m_tables[1];
        distances = m_fixed ? &fixedCodes()[1] : m_tables.empty() ? nullptr : &m_tables[2];
    };
    inUse();
    for (;;) {
        switch (m_state) {
        case State::Header: {
            if (!need(16)) {
                return {};
            }
            const std::uint32_t method = take(8);
            const std::uint32_t flags = take(8);
            if ((method & 15) != 8 || (method >> 4) > 7 || ((method << 8) | flags) % 31 != 0) {
                return garbled("a zlib stream whose header is none");
            }
            if ((flags & 0x20) != 0) {
                return garbled("a zlib stream that wants a dictionary");
            }
            m_window.resize(windowBytes);
            m_state = State::Block;
            break;
        }
        case State::Block: {
            if (!need(3)) {
                return {};
            }
            // A block begun: counted, and held to the read's bounds before anything of it is read (outside review
            // 2.1). And the caller is asked whether the thread has had too much of its time before the tables of
            // every dynamic block (each one builds three tables 32,768 entries wide), and after every so many of the
            // other blocks: a stream of tiny blocks engineered to cost a great deal is cut off as it goes, never
            // after it is done.
            if (++m_readBlocks > m_bounds->mostBlocks) {
                return tooCostly();
            }
            ++m_blocksBegun;
            m_final = take(1) != 0;
            const std::uint32_t type = take(2);
            if ((type == 2 || ++m_blocksSinceAsked >= askEveryBlocks) && !goesOn()) {
                return tooSlow();
            }
            if (type == 0) {
                static_cast<void>(take(m_count % 8)); // (a stored block begins at a byte)
                m_state = State::StoredLength;
            } else if (type == 1) {
                m_fixed = true;
                inUse();
                m_state = State::Literal;
            } else if (type == 2) {
                m_fixed = false;
                m_state = State::Counts;
            } else {
                return garbled("a deflate block of a kind there is none of");
            }
            break;
        }
        case State::StoredLength: {
            if (!need(32)) {
                return {};
            }
            const std::uint32_t length = take(16);
            const std::uint32_t check = take(16);
            if (length != (~check & 0xFFFF)) {
                return garbled("a stored block whose length is none");
            }
            m_left = length;
            m_state = State::Stored;
            break;
        }
        case State::Stored: {
            // (What the bits hold first: whole bytes, since the block began at one.)
            while (m_left > 0 && m_count >= 8) {
                if (!put(static_cast<std::uint8_t>(take(8)))) {
                    return tooLong();
                }
                --m_left;
            }
            while (m_left > 0 && m_in != m_end) {
                if (!put(*m_in)) {
                    return tooLong();
                }
                ++m_in;
                --m_left;
            }
            if (m_left > 0) {
                return {};
            }
            blockEnded();
            break;
        }
        case State::Counts: {
            if (!need(14)) {
                return {};
            }
            m_literals = static_cast<int>(take(5)) + 257;
            m_distances = static_cast<int>(take(5)) + 1;
            m_lengthCodes = static_cast<int>(take(4)) + 4;
            if (m_literals > 286 || m_distances > 30) {
                return garbled("a deflate block with more codes than there are");
            }
            m_lengthCodeLengths.fill(0);
            m_lengths.fill(0);
            if (m_tables.empty()) {
                m_tables.resize(3);
            }
            m_index = 0;
            m_repeat = 0;
            m_state = State::LengthCodes;
            break;
        }
        case State::LengthCodes: {
            while (m_index < m_lengthCodes) {
                if (!need(3)) {
                    return {};
                }
                m_lengthCodeLengths[lengthCodeOrder[static_cast<std::size_t>(m_index++)]] =
                    static_cast<std::uint8_t>(take(3));
            }
            if (!mayBuild(m_lengthCodeLengths)) {
                return tooCostly();
            }
            if (!build(m_lengthCodeLengths, Kind::LengthCodes, m_tables[0])) {
                return garbled("a deflate block whose code lengths make no code");
            }
            m_index = 0;
            m_state = State::Lengths;
            break;
        }
        case State::Lengths: {
            const int all = m_literals + m_distances;
            while (m_index < all) {
                if (m_repeat == 0) {
                    int symbol = 0;
                    if (!decode(m_tables[0], symbol)) {
                        return m_state == State::Failed ? garbled("a code length that is none") : base::Result<void>{};
                    }
                    if (symbol < 16) {
                        m_lengths[static_cast<std::size_t>(m_index++)] = static_cast<std::uint8_t>(symbol);
                        continue;
                    }
                    if (symbol == 16 && m_index == 0) {
                        return garbled("a deflate block that repeats a length before the first");
                    }
                    m_repeat = symbol;
                }
                const int extra = m_repeat == 16 ? 2 : m_repeat == 17 ? 3 : 7;
                if (!need(extra)) {
                    return {};
                }
                const int times = static_cast<int>(take(extra)) + (m_repeat == 18 ? 11 : 3);
                const std::uint8_t length = m_repeat == 16 ? m_lengths[static_cast<std::size_t>(m_index - 1)] : 0;
                m_repeat = 0;
                if (m_index + times > all) {
                    return garbled("a deflate block with more code lengths than codes");
                }
                std::fill_n(m_lengths.begin() + m_index, times, length);
                m_index += times;
            }
            if (m_lengths[256] == 0) {
                return garbled("a deflate block with no code for its end");
            }
            const std::span<const std::uint8_t> lengths(m_lengths);
            const std::span<const std::uint8_t> literalLengths = lengths.first(static_cast<std::size_t>(m_literals));
            const std::span<const std::uint8_t> distanceLengths =
                lengths.subspan(static_cast<std::size_t>(m_literals), static_cast<std::size_t>(m_distances));
            if (!mayBuild(literalLengths)) {
                return tooCostly();
            }
            if (!build(literalLengths, Kind::Other, m_tables[1])) {
                return garbled("a deflate block whose lengths make no code");
            }
            if (!mayBuild(distanceLengths)) {
                return tooCostly();
            }
            if (!build(distanceLengths, Kind::Other, m_tables[2])) {
                return garbled("a deflate block whose lengths make no code");
            }
            inUse();
            m_state = State::Literal;
            break;
        }
        case State::Literal: {
            for (;;) {
                // (A block of many codes is asked of as it goes too: what one inflates to is bounded, its time by
                // that.)
                if (++m_symbolsSinceAsked >= askEverySymbols && !goesOn()) {
                    return tooSlow();
                }
                int symbol = 0;
                if (!decode(*literals, symbol)) {
                    return m_state == State::Failed ? garbled("a code that is none") : base::Result<void>{};
                }
                if (symbol < 256) {
                    if (!put(static_cast<std::uint8_t>(symbol))) {
                        return tooLong();
                    }
                    continue;
                }
                if (symbol == 256) {
                    blockEnded();
                    break;
                }
                if (symbol > 285) {
                    return garbled("a length there is none of");
                }
                m_symbol = symbol - 257;
                m_state = State::LengthExtra;
                break;
            }
            break;
        }
        case State::LengthExtra: {
            const int extra = lengthExtra[static_cast<std::size_t>(m_symbol)];
            if (!need(extra)) {
                return {};
            }
            m_length = lengthBase[static_cast<std::size_t>(m_symbol)] + static_cast<int>(take(extra));
            m_state = State::Distance;
            break;
        }
        case State::Distance: {
            int symbol = 0;
            if (!decode(*distances, symbol)) {
                return m_state == State::Failed ? garbled("a distance code that is none") : base::Result<void>{};
            }
            if (symbol > 29) {
                return garbled("a distance there is none of");
            }
            m_symbol = symbol;
            m_state = State::DistanceExtra;
            break;
        }
        case State::DistanceExtra: {
            const int extra = distanceExtra[static_cast<std::size_t>(m_symbol)];
            if (!need(extra)) {
                return {};
            }
            const auto distance = std::uint64_t{distanceBase[static_cast<std::size_t>(m_symbol)]} + take(extra);
            if (distance > m_total) {
                return garbled("a copy from before the stream began");
            }
            for (int copied = 0; copied < m_length; ++copied) {
                if (!put(m_window[static_cast<std::size_t>((m_total - distance) % windowBytes)])) {
                    return tooLong();
                }
            }
            m_state = State::Literal;
            break;
        }
        case State::Trailer: {
            if (m_count % 8 != 0) {
                static_cast<void>(take(m_count % 8));
            }
            if (!need(32)) {
                return {};
            }
            std::uint32_t said = 0;
            for (int at = 0; at < 4; ++at) {
                said = (said << 8) | take(8);
            }
            if (said != ((m_adlerB << 16) | m_adlerA)) {
                return garbled("a zlib stream whose checksum is not its own");
            }
            m_state = State::Done;
            break;
        }
        case State::Done:
            if (m_in != m_end || m_count > 0) {
                return fail("ended", "a zlib stream with bytes after its end");
            }
            return {};
        case State::Failed:
            return m_tooLong ? tooLong() : garbled("a zlib stream that is none");
        }
    }
}

} // namespace qkvm::rfb
