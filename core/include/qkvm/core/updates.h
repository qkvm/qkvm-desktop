#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QWinEventNotifier>

#include <qkvm/core/settings.h>
#include <qkvm/core/update_release.h>

namespace qkvm::core {

// The command line Setup is started with (the Contract with Setup, 2026-11-08), the program's own file first:
//   "<setup>" /SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CURRENTUSER /UPDATE=1 /DIR="<folder>" /LOG="<log>"
// Each path whole and in Windows' spelling, between quotes. None where a path is not absolute, holds a quote or a sign
// below a space, or ends in a backslash (which would take the quote after it for one of its own): nothing is made of
// such a name, and nothing of the feed's is ever in it but a version, of three numbers, in the log's name.
QString setupCommandLine(const QString& setup, const QString& programFolder, const QString& log);

// What Setup says where an update changed nothing (the contract with Setup, as added on 2026-11-08): one line in its
// log, written by Inno's Log(), "qKVM-RESULT: " and then 105 (the program's single-instance mutex was still held after
// its minute) or 7 (it refused before changing anything) and words. No copy of the program runs while Setup does, so
// its exit code reaches nobody: the next start reads this line.
inline constexpr unsigned long setupFoundQkvmRunning = 105;
inline constexpr unsigned long setupChangedNothing = 7;
// Of the log at `log` (its last 64 KiB: it is the person's own text, which anything of theirs may write), the last line
// that holds "qKVM-RESULT: ", as what follows that: one line of printable signs only, at most 200. Empty where none.
QString setupResultIn(const QString& log);

class DownloadFolder;
// One updater for all windows. Checks may be automatic; downloads, and opening Windows' installer (an MSIX copy),
// showing the verified file (a portable copy) or running Setup (a Setup copy), are separate explicit requests. No path
// from a timer to any of them. The dedicated network manager must outlive this object and carries no device sessions.
class Updates : public QObject {
    Q_OBJECT
public:
    enum class State { Unavailable, Idle, Checking, Current, Available, Downloading, Ready, Error };
    Updates(UpdateChannel channel, Settings& settings, QNetworkAccessManager& network, QString cacheFolder,
            QObject* parent = nullptr);
    ~Updates() override;

    State state() const { return m_state; }
    bool configured() const;
    bool busy() const { return m_state == State::Checking || m_state == State::Downloading; }
    QString currentVersion() const { return m_channel.currentVersion; }
    QString problem() const { return m_problem; }
    const std::optional<UpdateRelease>& release() const { return m_release; }
    qint64 received() const { return m_received; }
    bool automatic() const { return m_settings.remembered().checkUpdates; }
    QDateTime lastChecked() const { return m_settings.remembered().lastUpdateCheck; }
    void setAutomatic(bool on);
    // Called at startup and periodically. Failed attempts also wait a day, without changing
    // the last successful check. A clock moving backwards causes a fresh check.
    void checkIfDue(const QDateTime& now);
    void check();
    void download();
    void cancel();
    UpdateKind kind() const { return m_channel.kind; }
    // An MSIX copy only: verifies the file again immediately before handing it to the installer. Does not quit
    // the program. The UI must first check all windows for control or unfinished work.
    bool openInstaller(const std::function<bool(const QString&)>& open);
    // A portable copy only: verifies the file again, as for the installer, and hands its name to `show`, which shows
    // it in its folder and never opens or runs it. The file is then kept after the program ends, for the person to
    // unpack (the cache's pruning takes it after seven days), and held against writing while the program runs.
    bool showFile(const std::function<bool(const QString&)>& show);
    // A Setup copy only (the contract with Setup): the release is newer than this program, and its downloaded Setup
    // is opened through a handle that lets nobody write, rename or delete it while it is held, and verified again
    // (size and SHA-256) through that handle. It must lie alone in its own folder of the update cache, which nobody
    // but this user, SYSTEM and the administrators may change (the folder was made so, in the call that made it, and
    // the cache's Downloads that holds it was made or kept so), and the folder the program runs from must still hold
    // Setup's marker. The hand-over is noted beside the settings, and Setup is started from that same file, the
    // handle still held (and until the program ends), with setupCommandLine (the program's own folder and
    // setupLogFor(the version)) and a TEMP and TMP of its own (a "setup-temp-" folder of Downloads, made as its folder
    // is, let go of at the next start). True: Setup has started, and the caller closes every window and ends the
    // program, for Setup waits for that (it gives up after 60 s, changing nothing). A file that is not the one verified
    // is removed and downloaded anew. The UI must first check that nothing the program's end would cut is under way.
    bool runSetup();
    // Where Setup writes its log for a version: "updates/setup-<version>.log" in the program's data folder.
    QString setupLogFor(const QString& version) const;
    // What this start found of a hand-over to Setup that the run before made, in plain words: the update installed;
    // or not, with the place of Setup's log, and then the update offered again (checked for anew); or Setup still at
    // work, which is told again when it ends (afterSetupTold). Empty where there was none.
    QString afterSetup() const { return m_afterSetup; }
    bool afterSetupWentWrong() const { return m_afterSetupWentWrong; }
    // Tests only: called with the file once it is verified, just before Setup is started from it.
    void beforeSetupStarts(std::function<void(const QString&)> what) { m_beforeSetup = std::move(what); }
    // Tests only: called with a download's folder the moment it is made.
    void whenFolderMade(std::function<void(const QString&)> what) { m_folderMade = std::move(what); }

signals:
    void changed();
    void available();
    void afterSetupTold();

private:
    void request(const QUrl& url, bool package);
    void takeBytes();
    void finished();
    void fail(const QString& message);
    void stopRequest();
    void discardPackage();
    // The downloaded file, locked against writing and replacement, and hashed again against the signed release.
    // False (after saying why, and letting go of the file) where it is not that file any more.
    bool verifiedAgain();
    // The downloaded file held by a handle that lets nobody write, rename or delete it (opened once, then kept until
    // the package is let go of); false where it could not be.
    bool locked();
    QString handOverFile() const;
    bool noteHandOver(unsigned long process, quint64 created, const QString& temp);
    void lookAtHandOver();
    void setupEnded();
    void tellWhatSetupSaid(const QString& version);
    void letGoOfSetupsTemp();
    void tellAfterSetup(const QString& text, bool wrong, bool offerAgain);

    UpdateChannel m_channel;
    Settings& m_settings;
    QNetworkAccessManager& m_network;
    QString m_cacheFolder;
    State m_state = State::Unavailable;
    QString m_problem;
    QDateTime m_lastAttempt;
    QString m_notifiedVersion;
    std::optional<UpdateRelease> m_release;
    QNetworkReply* m_reply = nullptr;
    QTimer m_deadline;
    bool m_package = false;
    QByteArray m_metadata;
    qint64 m_received = 0;
    std::unique_ptr<DownloadFolder> m_folder;
    QFile m_file;
    // Windows read handle held across verification and installer handoff. Denies replacement
    // and writing while this program is using the verified package.
    void* m_packageLock = nullptr;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    std::function<void(const QString&)> m_beforeSetup;
    std::function<void(const QString&)> m_folderMade;
    QString m_afterSetup;
    bool m_afterSetupWentWrong = false;
    // A Setup of the run before that was still at work when this one started: its process, and the version it is to
    // install, until it ends.
    void* m_setupProcess = nullptr;
    QString m_setupVersion;
    // The name of Setup's own TEMP folder in the cache, as the hand-over note says it.
    QString m_setupTemp;
    std::unique_ptr<QWinEventNotifier> m_setupEnd;
};

} // namespace qkvm::core
