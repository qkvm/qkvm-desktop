#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>
#include <QVersionNumber>

#include <qkvm/base/result.h>

namespace qkvm::core {

// Which kind of copy of the program this is, and so the one kind of release it takes: a copy that Windows installed
// from an MSIX package, the folder of a portable ZIP, or a copy that qKVM's own Setup installed. The signed payload's
// platform names the kind ("windows-x64-msix", "windows-x64-portable", "windows-x64-setup") and the package's name
// ends in it (.msix, .zip, -setup.exe); a release of another kind is refused as any wrong release is. A portable
// copy's update is shown to the person in its folder, verified, and never run, unpacked or handed to an installer by
// the program. A Setup copy's is run, verified, when the person asks for it (Updates::runSetup).
enum class UpdateKind { Msix, Portable, Setup };

// What Setup writes beside the program it installs, and its whole content (the Contract with Setup, 2026-11-08). A
// portable copy has no such file.
inline constexpr char setupMarkerName[] = "qkvm-desktop.installed";
inline constexpr char setupMarkerContent[] = "setup\n";

// The release address and verification key are built into the program, never supplied by a
// server or by a settings file. An unconfigured development build makes no update requests.
struct UpdateChannel {
    QUrl feed;
    QByteArray publicKey; // P-256 public X || Y, 32 bytes each, big endian.
    QString currentVersion;
    UpdateKind kind = UpdateKind::Portable;
    // The folder the program runs from (a Setup copy: the one with Setup's marker, which Setup installs into again),
    // and the program's own data folder (Setup's log and the note of a hand-over to Setup go to its "updates").
    QString programFolder = {};
    QString dataFolder = {};
};

// The channel of a copy that runs from `programFolder`: its kind (updateKindOf), and the feed for that kind, which for
// a Setup copy lies beside `builtInFeed` (setupFeedBeside) and for every other copy is `builtInFeed` itself.
UpdateChannel updateChannelFor(const QUrl& builtInFeed, const QByteArray& publicKey, const QString& currentVersion,
                               const QString& programFolder, bool packaged, const QString& dataFolder);
// The program's own: the channel for the folder it runs from, with the feed and key built into it.
UpdateChannel desktopUpdateChannel();
// Setup's when its marker lies in `programFolder` with exactly its content; else an MSIX copy where Windows says the
// program is `packaged`; else portable.
UpdateKind updateKindOf(const QString& programFolder, bool packaged);
// The feed a Setup copy reads: stable-setup.json, in the folder of the built-in feed (none where that is none).
QUrl setupFeedBeside(const QUrl& feed);
bool updateAddress(const QUrl& address);
QVersionNumber updateVersion(const QString& text);

struct UpdateRelease {
    QString version;
    QString notes;
    QDateTime published;
    QDateTime expires;
    QUrl package;
    qint64 bytes = 0;
    QByteArray sha256;
};

inline constexpr qint64 mostUpdateMetadata = 96LL * 1024;
inline constexpr qint64 mostUpdatePackage = 1024LL * 1024 * 1024;

// The envelope holds base64 payload and signature. Verify the exact payload bytes before
// interpreting them. ECDSA P-256 / SHA-256, signature as fixed-width r || s (64 bytes).
// Only this product's stable Windows x64 releases of the given kind are accepted. No executable URLs,
// command arguments, scripts or settings migrations come from the feed.
base::Result<UpdateRelease> verifiedUpdate(QByteArrayView envelope, const QByteArray& publicKey, const QDateTime& now,
                                           UpdateKind kind);

} // namespace qkvm::core
