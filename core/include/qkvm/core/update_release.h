#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>
#include <QVersionNumber>

#include <qkvm/base/machine.h>
#include <qkvm/base/result.h>

namespace qkvm::core {

// Which kind of copy of the program this is, and so the one kind of release it takes: a copy that Windows installed
// from an MSIX package, the folder of a portable ZIP, or a copy that qKVM's own Setup installed. The signed payload's
// platform names the kind and the kind of Windows the release is for ("windows-x64-portable", "windows-arm64-setup":
// windows-<x64 or arm64>-<msix, portable or setup>) and the package's name ends in the kind (.msix, .zip, -setup.exe);
// a release of another kind, or for another kind of Windows, is refused as any wrong release is. A portable copy's
// update is shown to the person in its folder, verified, and never run, unpacked or handed to an installer by the
// program. A Setup copy's is run, verified, when the person asks for it (Updates::runSetup).
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
    // The kind of Windows whose release the copy takes: the PC's own (base::machineArchitecture), whatever the program
    // is; and the program's own. They differ only for an x64 copy on Windows on Arm, which then takes the ARM64 release
    // of its own version as well as of a later one (Updates).
    base::Architecture machine = base::programArchitecture();
    base::Architecture program = base::programArchitecture();
};

// The channel of a copy that runs from `programFolder` on a PC of the kind `machine`: its kind (updateKindOf), and the
// feed for that kind and that PC (feedFor).
UpdateChannel updateChannelFor(const QUrl& builtInFeed, const QByteArray& publicKey, const QString& currentVersion,
                               const QString& programFolder, bool packaged, const QString& dataFolder,
                               base::Architecture machine);
// The program's own: the channel for the folder it runs from, on the PC it runs on, with the feed and key built into
// it.
UpdateChannel desktopUpdateChannel();
// Setup's when its marker lies in `programFolder` with exactly its content; else an MSIX copy where Windows says the
// program is `packaged`; else portable.
UpdateKind updateKindOf(const QString& programFolder, bool packaged);
// The feed a copy of the kind reads on a PC of the kind `machine`, in the folder of the built-in one (none where that
// is none): on x64 the built-in feed itself (stable.json), and stable-setup.json for a Setup copy, as 0.1.0 to 0.1.2
// read them; on Windows on Arm stable-arm64.json, and stable-arm64-setup.json for a Setup copy.
QUrl feedFor(const QUrl& builtInFeed, UpdateKind kind, base::Architecture machine);
bool updateAddress(const QUrl& address);
// The one redirect an update package may follow (0.1.1, the owner): from a GitHub release download, the address
// the signed feed names (https://github.com/<owner>/<repository>/releases/download/<tag>/<file>), to GitHub's own
// file host, over https, at its usual port and with no name in the address. What comes is checked by its size and
// SHA-256 as ever: this only says where it may come from.
bool updatePackageRedirect(const QUrl& from, const QUrl& to);
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
// The most a redirect that is followed may say of its own (GitHub's says a line or two).
inline constexpr qint64 mostRedirectAnswer = 64LL * 1024;
inline constexpr qint64 mostUpdatePackage = 1024LL * 1024 * 1024;

// The envelope holds base64 payload and signature. Verify the exact payload bytes before
// interpreting them. ECDSA P-256 / SHA-256, signature as fixed-width r || s (64 bytes).
// Only this product's stable Windows releases of the given kind, for the given kind of Windows, are accepted. No
// executable URLs, command arguments, scripts or settings migrations come from the feed.
base::Result<UpdateRelease> verifiedUpdate(QByteArrayView envelope, const QByteArray& publicKey, const QDateTime& now,
                                           UpdateKind kind, base::Architecture machine);

} // namespace qkvm::core
