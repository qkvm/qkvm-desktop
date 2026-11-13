#pragma once

#include <cstddef>
#include <span>
#include <stop_token>

#include <QByteArray>
#include <QByteArrayView>

#include <qkvm/base/result.h>

// scrypt (RFC 7914): the work that turns a passphrase into a key, made to cost memory as well as
// time, so that guessing is dear. Written here from the RFC, since neither Windows nor Qt has
// it and no library is added for it; the one part that is Windows' own is PBKDF2 with
// HMAC-SHA-256 (CNG's BCryptDeriveKeyPBKDF2), which stands at its beginning and its end. A QKVM
// server derives the key of a file of its wall this way (../app/qkvm/vault.py), and a wrong
// scrypt here opens none of them.
//
// Slow by design, and not for the interface thread: as a file of the wall is written it takes
// about 128 MiB and, on the PC this was written on, almost half a second in the build that is
// worked with and a little over a third of a second in the one that is handed out
// (core/tests/test_scrypt.cpp says it at every run; scrypt.cpp says what of that is the price
// of wiping the words it works on).
namespace qkvm::core::scrypt {

// What the work costs, in the RFC's letters: N (how much memory, and how long), r (how large a
// block of it is) and p (how many times over).
struct Cost {
    quint64 n = 0;
    quint32 r = 0;
    quint32 p = 0;

    bool operator==(const Cost&) const = default;
};

// More than this is asked of no PC in one piece, whoever asks and whatever was checked before.
inline constexpr quint64 largestBlockBytes = quint64{512} * 1024 * 1024;
// The longest passphrase and the longest salt that are worked on, and the longest key made.
inline constexpr qsizetype longestInputBytes = qsizetype{64} * 1024;
inline constexpr std::size_t longestKeyBytes = 64;

// How many bytes the work asks for, in one piece; 0 where the cost is none that this does at
// all: N a power of two above 1 and below 2^(16r), r and p at least 1, and nothing larger than
// the limits above.
quint64 bytesAskedFor(const Cost& cost) noexcept;

// Where the one large piece of memory comes from. A test hands in its own, to see what was asked
// for and to have none.
class Memory {
public:
    virtual ~Memory() = default;
    // So many bytes, aligned as memory for anything is, or nothing where there is none.
    virtual void* take(std::size_t bytes) = 0;
    virtual void give(void* block, std::size_t bytes) = 0;
};
// Windows itself (VirtualAlloc): it says no where it has none, and nothing falls over that.
Memory& windowsMemory();

// What cannot be worked on, and why the work did not end: in codes of the program's own, written
// with a colon so that no file and no server can say them.
inline constexpr const char* notACost = "desktop:not_a_cost";
inline constexpr const char* noMemory = "desktop:no_memory";
inline constexpr const char* stopped = "desktop:stopped";
inline constexpr const char* windowsRefused = "desktop:crypto";

// The key for a passphrase, a salt and a cost, written into `key`, which says by its size how
// long a key is wanted.
//
// The cost is looked at before anything is asked for: one that is none (bytesAskedFor) is
// refused, `notACost`, with no memory taken. Then the memory is asked for once; a PC that does
// not have it is told so in `noMemory`. `stop`: whoever asked has gone, and the work ends where
// it stands, `stopped`. Whatever comes of it, the memory is wiped before it is given back, and
// so is this function's own copy of the passphrase. Nothing of the passphrase or the key is in
// any error.
//
// From any thread; on the interface thread never.
base::Result<void> derive(QByteArrayView passphrase, QByteArrayView salt, const Cost& cost, std::span<quint8> key,
                          const std::stop_token& stop = {}, Memory& memory = windowsMemory());

// The parts scrypt is made of, each as the RFC names it, for the tests against the RFC's own
// examples. Nothing but those tests calls them.
namespace parts {

// Salsa20/8 Core: 64 bytes in, 64 out.
QByteArray salsa20Core8(QByteArrayView block);
// scryptBlockMix: 128 * r bytes in and out.
QByteArray blockMix(QByteArrayView blocks, quint32 r);
// scryptROMix: 128 * r bytes in and out, with N of them remembered meanwhile.
QByteArray roMix(QByteArrayView block, quint64 n, quint32 r);
// PBKDF2 with HMAC-SHA-256, of Windows.
base::Result<QByteArray> pbkdf2(QByteArrayView passphrase, QByteArrayView salt, quint64 rounds, qsizetype length);

} // namespace parts

} // namespace qkvm::core::scrypt
