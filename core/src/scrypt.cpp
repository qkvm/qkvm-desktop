#include <qkvm/core/scrypt.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QtEndian>

#include <qkvm/core/wiped.h>

#include "cng.h"

namespace qkvm::core::scrypt {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Exchange", text);
}

// A block of Salsa20 is sixteen words of four bytes, and scrypt's own block is 2r of those.
constexpr std::size_t wordsOfABlock = 16;
constexpr std::size_t bytesOfABlock = 64;
// The rounds of Salsa20/8, two at a time.
constexpr int doubleRounds = 4;
// What the RFC says of N: below 2^(128 * r / 8). From r = 4 on that is more than a number here
// can be. (What it says of r and p together, p at most (2^32 - 1) * 32 / (128 * r), is far above
// what the limit on memory leaves of them, and so is not asked by itself.)
constexpr quint32 bitsOfNPerR = 16;
constexpr quint32 bitsOfANumber = 64;

base::Error notOne()
{
    return {notACost, words("These are not settings a key can be made with.").toStdString()};
}

base::Error windowsSaidNo()
{
    return {windowsRefused, words("Windows could not do the work a passphrase takes.").toStdString()};
}

// One step of a round: `into` is changed by the sum of two others, turned round by so many bits.
// (Written out where it is used, and no function: the build that is worked with calls every
// function it is given, and this line runs some hundred million times for one passphrase.)
#define QKVM_SALSA_STEP(into, one, other, by)                                                                          \
    sum = (one) + (other);                                                                                             \
    (into) = (into) ^ ((sum << (by)) | (sum >> (32 - (by))))

// Salsa20/8 Core (RFC 7914 section 3), on sixteen words where they stand.
//
// The sixteen words the rounds work on, and the sum of a step, are made of the passphrase like
// all else here, and they stand on the stack, where nothing else wipes them: so each is written
// over with nought on the one way out there is. And each is `volatile`, which is what makes
// that true in the build that is handed out. Without it the compiler works on copies of its
// own, in registers and in places on the stack that have no name here, and what is written
// over at the end is not where the words stood: seen in that build's machine code, with the
// words written over one by one through a pointer, and with a struct of them that wipes itself
// when it goes. With it every look at a word and every change of one is done in the word's own
// place, and those seventeen places are all there is of the words. (It costs: about two thirds
// more time in the build that is handed out, 0.34 s for 0.2 s where it was measured, and
// nothing in the one that is worked with. What a register held for a moment is not this code's
// to wipe.) They are not all that is made of the passphrase outside the piece of memory that
// is wiped: roMix has a number and an address that are, and says there what becomes of them.
void salsa20Core8(quint32* block) noexcept
{
    volatile quint32 x0 = block[0];
    volatile quint32 x1 = block[1];
    volatile quint32 x2 = block[2];
    volatile quint32 x3 = block[3];
    volatile quint32 x4 = block[4];
    volatile quint32 x5 = block[5];
    volatile quint32 x6 = block[6];
    volatile quint32 x7 = block[7];
    volatile quint32 x8 = block[8];
    volatile quint32 x9 = block[9];
    volatile quint32 x10 = block[10];
    volatile quint32 x11 = block[11];
    volatile quint32 x12 = block[12];
    volatile quint32 x13 = block[13];
    volatile quint32 x14 = block[14];
    volatile quint32 x15 = block[15];
    volatile quint32 sum = 0;
    for (int round = 0; round < doubleRounds; ++round) {
        // Down the columns.
        QKVM_SALSA_STEP(x4, x0, x12, 7);
        QKVM_SALSA_STEP(x8, x4, x0, 9);
        QKVM_SALSA_STEP(x12, x8, x4, 13);
        QKVM_SALSA_STEP(x0, x12, x8, 18);
        QKVM_SALSA_STEP(x9, x5, x1, 7);
        QKVM_SALSA_STEP(x13, x9, x5, 9);
        QKVM_SALSA_STEP(x1, x13, x9, 13);
        QKVM_SALSA_STEP(x5, x1, x13, 18);
        QKVM_SALSA_STEP(x14, x10, x6, 7);
        QKVM_SALSA_STEP(x2, x14, x10, 9);
        QKVM_SALSA_STEP(x6, x2, x14, 13);
        QKVM_SALSA_STEP(x10, x6, x2, 18);
        QKVM_SALSA_STEP(x3, x15, x11, 7);
        QKVM_SALSA_STEP(x7, x3, x15, 9);
        QKVM_SALSA_STEP(x11, x7, x3, 13);
        QKVM_SALSA_STEP(x15, x11, x7, 18);
        // Along the rows.
        QKVM_SALSA_STEP(x1, x0, x3, 7);
        QKVM_SALSA_STEP(x2, x1, x0, 9);
        QKVM_SALSA_STEP(x3, x2, x1, 13);
        QKVM_SALSA_STEP(x0, x3, x2, 18);
        QKVM_SALSA_STEP(x6, x5, x4, 7);
        QKVM_SALSA_STEP(x7, x6, x5, 9);
        QKVM_SALSA_STEP(x4, x7, x6, 13);
        QKVM_SALSA_STEP(x5, x4, x7, 18);
        QKVM_SALSA_STEP(x11, x10, x9, 7);
        QKVM_SALSA_STEP(x8, x11, x10, 9);
        QKVM_SALSA_STEP(x9, x8, x11, 13);
        QKVM_SALSA_STEP(x10, x9, x8, 18);
        QKVM_SALSA_STEP(x12, x15, x14, 7);
        QKVM_SALSA_STEP(x13, x12, x15, 9);
        QKVM_SALSA_STEP(x14, x13, x12, 13);
        QKVM_SALSA_STEP(x15, x14, x13, 18);
    }
    block[0] += x0;
    block[1] += x1;
    block[2] += x2;
    block[3] += x3;
    block[4] += x4;
    block[5] += x5;
    block[6] += x6;
    block[7] += x7;
    block[8] += x8;
    block[9] += x9;
    block[10] += x10;
    block[11] += x11;
    block[12] += x12;
    block[13] += x13;
    block[14] += x14;
    block[15] += x15;
    x0 = 0;
    x1 = 0;
    x2 = 0;
    x3 = 0;
    x4 = 0;
    x5 = 0;
    x6 = 0;
    x7 = 0;
    x8 = 0;
    x9 = 0;
    x10 = 0;
    x11 = 0;
    x12 = 0;
    x13 = 0;
    x14 = 0;
    x15 = 0;
    sum = 0;
}

#undef QKVM_SALSA_STEP

// scryptBlockMix (RFC 7914 section 4): `in` and `out` are 2r blocks each and do not overlap;
// `one` is room for one block, which the mixing goes through. (Handed in, so that it lies in
// the memory that is wiped, and not on the stack.)
void blockMix(const quint32* in, quint32* out, quint32* one, std::size_t r) noexcept
{
    std::memcpy(one, in + (2 * r - 1) * wordsOfABlock, bytesOfABlock);
    for (std::size_t index = 0; index < 2 * r; ++index) {
        const quint32* block = in + index * wordsOfABlock;
        for (std::size_t word = 0; word < wordsOfABlock; ++word) {
            one[word] ^= block[word];
        }
        salsa20Core8(one);
        // The even ones first, then the odd ones.
        const std::size_t place = (index % 2 == 0) ? index / 2 : r + index / 2;
        std::memcpy(out + place * wordsOfABlock, one, bytesOfABlock);
    }
}

// scryptROMix (RFC 7914 section 5). `x` is the block, 32r words, and is where the result
// stands; `other` is room for a second one, `one` for a block of Salsa20, and `remembered` for
// N blocks. False where it was stopped; what stands in `x` then is of no use.
bool roMix(quint32* x, quint32* other, quint32* one, quint32* remembered, quint64 n, std::size_t r,
           const std::stop_token& stop) noexcept
{
    const std::size_t count = 2 * r * wordsOfABlock;
    const std::size_t bytes = count * sizeof(quint32);
    // The block goes to and fro between the two rooms. N is even, so it ends where it began.
    quint32* from = x;
    quint32* to = other;
    for (quint64 index = 0; index < n; ++index) {
        if (stop.stop_requested()) {
            return false;
        }
        std::memcpy(remembered + index * count, from, bytes);
        blockMix(from, to, one, r);
        std::swap(from, to);
    }
    for (quint64 index = 0; index < n; ++index) {
        if (stop.stop_requested()) {
            return false;
        }
        // Integerify: the block's last 64 bytes as a number, of which N (a power of two) leaves
        // the lowest bits.
        //
        // `which` is made of the passphrase as the words of Salsa are (up to twenty bits of the
        // block, at every step), and so is the address made of it, which says the same. Neither
        // is wiped, and that is said here as what it is. In the build that is handed out neither
        // ever stands on the stack: both are held in registers, which the next lines write over
        // (seen in that build's machine code: `which` in one that is cleared before the block
        // is mixed, the address in one that the call to blockMix is handed something else in).
        // In the build that is worked with, which is handed to nobody, both stand on the stack
        // until something else is written there. They are not made `volatile` and written over
        // as the words are: `which` alone would be half of it, since the address says the same,
        // and an address that is read from its place for every word stops the compiler from
        // taking the block several words at a time.
        const quint32* last = from + (2 * r - 1) * wordsOfABlock;
        const quint64 which = ((quint64{last[1]} << 32) | last[0]) & (n - 1);
        const quint32* picked = remembered + which * count;
        for (std::size_t word = 0; word < count; ++word) {
            from[word] ^= picked[word];
        }
        blockMix(from, to, one, r);
        std::swap(from, to);
    }
    return true;
}

// The bytes of the RFC are words whose lowest byte comes first, on any PC.
void wordsFrom(const quint8* bytes, quint32* into, std::size_t count) noexcept
{
    for (std::size_t index = 0; index < count; ++index) {
        into[index] = qFromLittleEndian<quint32>(bytes + index * sizeof(quint32));
    }
}

void bytesFrom(const quint32* from, quint8* bytes, std::size_t count) noexcept
{
    for (std::size_t index = 0; index < count; ++index) {
        qToLittleEndian<quint32>(from[index], bytes + index * sizeof(quint32));
    }
}

// PBKDF2 with HMAC-SHA-256, by Windows. It wants to be able to write to what it only reads, so
// all of it is ours to be written to. Never handed nothing to point at: an empty passphrase or
// salt is a pointer to something and a length of nought.
bool pbkdf2(quint8* passphrase, std::size_t passphraseBytes, quint8* salt, std::size_t saltBytes, quint64 rounds,
            quint8* key, std::size_t keyBytes) noexcept
{
    const cng::Algorithm hmac(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!hmac) {
        return false;
    }
    quint8 nothing = 0;
    return cng::worked(BCryptDeriveKeyPBKDF2(hmac.get(), passphrase != nullptr ? passphrase : &nothing,
                                             static_cast<ULONG>(passphraseBytes), salt != nullptr ? salt : &nothing,
                                             static_cast<ULONG>(saltBytes), rounds, key, static_cast<ULONG>(keyBytes),
                                             0));
}

class WindowsMemory final : public Memory {
public:
    void* take(std::size_t bytes) override
    {
        return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    void give(void* block, std::size_t /*bytes*/) override { VirtualFree(block, 0, MEM_RELEASE); }
};

// The one piece of memory, for as long as the work lasts: wiped, and then given back.
class Taken {
public:
    Taken(Memory& memory, std::size_t bytes) : m_memory(memory), m_block(memory.take(bytes)), m_bytes(bytes) {}
    Taken(const Taken&) = delete;
    Taken& operator=(const Taken&) = delete;
    ~Taken()
    {
        if (m_block != nullptr) {
            wipe(m_block, m_bytes);
            m_memory.give(m_block, m_bytes);
        }
    }

    void* block() const noexcept { return m_block; }

private:
    Memory& m_memory;
    void* m_block;
    std::size_t m_bytes;
};

} // namespace

quint64 bytesAskedFor(const Cost& cost) noexcept
{
    const bool powerOfTwo = cost.n >= 2 && (cost.n & (cost.n - 1)) == 0;
    if (!powerOfTwo || cost.r < 1 || cost.p < 1) {
        return 0;
    }
    if (cost.r < bitsOfANumber / bitsOfNPerR && cost.n >= (quint64{1} << (cost.r * bitsOfNPerR))) {
        return 0;
    }
    // (Each division before its multiplication: none of these can run over.)
    const quint64 block = quint64{2} * bytesOfABlock * cost.r;
    if (block > largestBlockBytes || cost.n > largestBlockBytes / block || cost.p > largestBlockBytes / block) {
        return 0;
    }
    // N blocks that are remembered; the block itself, where it goes to and fro, twice; a block
    // of Salsa20; and p blocks for what PBKDF2 makes of the passphrase.
    const quint64 all = block * (cost.n + 2 + cost.p) + bytesOfABlock;
    return all <= largestBlockBytes ? all : 0;
}

Memory& windowsMemory()
{
    static WindowsMemory memory;
    return memory;
}

base::Result<void> derive(QByteArrayView passphrase, QByteArrayView salt, const Cost& cost, std::span<quint8> key,
                          const std::stop_token& stop, Memory& memory)
{
    const quint64 asked = bytesAskedFor(cost);
    if (asked == 0 || key.empty() || key.size() > longestKeyBytes || passphrase.size() > longestInputBytes ||
        salt.size() > longestInputBytes) {
        return notOne();
    }
    const Taken taken(memory, static_cast<std::size_t>(asked));
    if (taken.block() == nullptr) {
        const quint64 megabytes = (asked + (quint64{1} << 20) - 1) >> 20;
        return base::Error{noMemory, words("This PC does not have the memory free that a passphrase's work takes "
                                           "(%1 MB). Close another program and try again.")
                                         .arg(megabytes)
                                         .toStdString()};
    }
    const std::size_t r = cost.r;
    const std::size_t count = 2 * r * wordsOfABlock;
    const std::size_t blockBytes = count * sizeof(quint32);
    // How the piece is laid out: what is remembered, the block twice, a block of Salsa20, and
    // the bytes PBKDF2 makes, which the mixing reads and writes block by block.
    auto* remembered = static_cast<quint32*>(taken.block());
    quint32* x = remembered + cost.n * count;
    quint32* other = x + count;
    quint32* one = other + count;
    auto* made = reinterpret_cast<quint8*>(one + wordsOfABlock);
    const std::size_t madeBytes = blockBytes * cost.p;

    // A copy of the passphrase that Windows may write to, and that is wiped with all else.
    SecretBytes secret = SecretBytes::copyOf(passphrase);
    QByteArray seasoning(salt.data(), salt.size());
    if (!pbkdf2(secret.data(), static_cast<std::size_t>(secret.size()), reinterpret_cast<quint8*>(seasoning.data()),
                static_cast<std::size_t>(seasoning.size()), 1, made, madeBytes)) {
        return windowsSaidNo();
    }
    for (quint32 turn = 0; turn < cost.p; ++turn) {
        quint8* block = made + turn * blockBytes;
        wordsFrom(block, x, count);
        if (!roMix(x, other, one, remembered, cost.n, r, stop)) {
            return base::Error{stopped, words("Nobody waits for this any more.").toStdString()};
        }
        bytesFrom(x, block, count);
    }
    if (!pbkdf2(secret.data(), static_cast<std::size_t>(secret.size()), made, madeBytes, 1, key.data(), key.size())) {
        wipe(key.data(), key.size());
        return windowsSaidNo();
    }
    return {};
}

namespace parts {

QByteArray salsa20Core8(QByteArrayView block)
{
    if (block.size() != static_cast<qsizetype>(bytesOfABlock)) {
        return {};
    }
    std::vector<quint32> one(wordsOfABlock);
    wordsFrom(reinterpret_cast<const quint8*>(block.data()), one.data(), wordsOfABlock);
    scrypt::salsa20Core8(one.data());
    QByteArray mixed(block.size(), Qt::Uninitialized);
    bytesFrom(one.data(), reinterpret_cast<quint8*>(mixed.data()), wordsOfABlock);
    return mixed;
}

QByteArray blockMix(QByteArrayView blocks, quint32 r)
{
    // (For the RFC's small examples only, as the next is.)
    constexpr quint32 largestR = 8;
    const std::size_t count = 2 * std::size_t{r} * wordsOfABlock;
    if (r < 1 || r > largestR || blocks.size() != static_cast<qsizetype>(count * sizeof(quint32))) {
        return {};
    }
    std::vector<quint32> in(count);
    std::vector<quint32> out(count);
    std::vector<quint32> one(wordsOfABlock);
    wordsFrom(reinterpret_cast<const quint8*>(blocks.data()), in.data(), count);
    scrypt::blockMix(in.data(), out.data(), one.data(), r);
    QByteArray mixed(blocks.size(), Qt::Uninitialized);
    bytesFrom(out.data(), reinterpret_cast<quint8*>(mixed.data()), count);
    return mixed;
}

QByteArray roMix(QByteArrayView block, quint64 n, quint32 r)
{
    // (What is remembered is asked of the ordinary heap here.)
    constexpr quint32 largestR = 8;
    constexpr quint64 mostRemembered = 1024;
    const std::size_t count = 2 * std::size_t{r} * wordsOfABlock;
    const bool powerOfTwo = n >= 2 && (n & (n - 1)) == 0;
    if (r < 1 || r > largestR || !powerOfTwo || n > mostRemembered ||
        block.size() != static_cast<qsizetype>(count * sizeof(quint32))) {
        return {};
    }
    std::vector<quint32> x(count);
    std::vector<quint32> other(count);
    std::vector<quint32> one(wordsOfABlock);
    std::vector<quint32> remembered(static_cast<std::size_t>(n) * count);
    wordsFrom(reinterpret_cast<const quint8*>(block.data()), x.data(), count);
    if (!scrypt::roMix(x.data(), other.data(), one.data(), remembered.data(), n, r, std::stop_token())) {
        return {};
    }
    QByteArray mixed(block.size(), Qt::Uninitialized);
    bytesFrom(x.data(), reinterpret_cast<quint8*>(mixed.data()), count);
    return mixed;
}

base::Result<QByteArray> pbkdf2(QByteArrayView passphrase, QByteArrayView salt, quint64 rounds, qsizetype length)
{
    constexpr qsizetype longest = 1024;
    if (rounds < 1 || length < 1 || length > longest || passphrase.size() > longestInputBytes ||
        salt.size() > longestInputBytes) {
        return notOne();
    }
    QByteArray secret(passphrase.data(), passphrase.size());
    QByteArray seasoning(salt.data(), salt.size());
    QByteArray key(length, Qt::Uninitialized);
    if (!scrypt::pbkdf2(reinterpret_cast<quint8*>(secret.data()), static_cast<std::size_t>(secret.size()),
                        reinterpret_cast<quint8*>(seasoning.data()), static_cast<std::size_t>(seasoning.size()), rounds,
                        reinterpret_cast<quint8*>(key.data()), static_cast<std::size_t>(key.size()))) {
        return windowsSaidNo();
    }
    return key;
}

} // namespace parts

} // namespace qkvm::core::scrypt
