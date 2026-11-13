#pragma once

#include <span>

#include <QByteArray>
#include <QByteArrayView>

#include <qkvm/base/result.h>
#include <qkvm/core/wiped.h>

// Sealing bytes under a key, and opening them again: AES-256-GCM, of Windows' own (CNG), as a
// QKVM server seals with it (../app/qkvm/vault.py). What is sealed is the bytes encrypted and,
// behind them, a tag of sixteen bytes that fits only this key, this nonce, these bytes and what
// they were sealed for.
namespace qkvm::core::sealing {

inline constexpr qsizetype nonceBytes = 12;
inline constexpr qsizetype tagBytes = 16;
// The most that is sealed or opened in one piece: more than any file of a wall holds.
inline constexpr qsizetype largestBytes = qsizetype{64} * 1024 * 1024;

// In codes of the program's own. `doesNotOpen`: the tag does not fit, or what was handed over
// is none that anything opens (a nonce of another length, fewer bytes than a tag has).
inline constexpr const char* doesNotOpen = "desktop:does_not_open";
inline constexpr const char* windowsRefused = "desktop:crypto";

// Bytes nobody can guess, of Windows' own making (BCryptGenRandom). From any thread.
base::Result<void> random(std::span<quint8> into);

// `plain` sealed under `key` with `nonce`, which has twelve bytes and is used for this once
// with this key, for `context`, which whoever opens it has to name again. From any thread.
base::Result<QByteArray> seal(const SecretKey& key, QByteArrayView nonce, QByteArrayView plain, QByteArrayView context);

// What was sealed. Where the tag does not fit, nothing of what was decrypted is handed out:
// `doesNotOpen`, and the room it stood in is wiped. From any thread.
base::Result<SecretBytes> open(const SecretKey& key, QByteArrayView nonce, QByteArrayView sealed,
                               QByteArrayView context);

} // namespace qkvm::core::sealing
