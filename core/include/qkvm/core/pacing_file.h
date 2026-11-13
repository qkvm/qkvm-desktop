#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

#include <qkvm/core/secrets.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/wiped.h>

namespace qkvm::core {

// What SignInPacing holds, kept beside the settings (a9, the owner's decision of 2026-11-05): the noes and the waits of
// every road, address, user and password mark outlive the program, so that a restart does not offer a refused password
// again before its time (a JetKVM shuts an address out after five wrong ones, a controller an account after a few).
//
// * What the file holds: for each mark, its digest (SignInPacing::what, the one SignInPacing knows it by: a digest of
//   the road, the address, the user and the password, made as the outside review's F5 makes it), the road, the address,
//   the user, the noes and the sign-ins that came to nothing in a row, and until when it may not be offered (on the
//   PC's clock). No password. And since that digest is a fast hash of what a password is tried against, it is never
//   written in the clear: the whole is sealed (AES-256-GCM, sealing.h) under a key of its own that is kept in Windows'
//   store of this program's secrets (`keyName`), beside the KVMs' passwords themselves. The file alone gives nothing to
//   try a password against; whoever can read that store reads the passwords there and needs no digest. The key is made
//   at a first start before the program's lock looks at that store, and the lock does not take it for a sign of a
//   program used before (AppLock); a reset of the saved secrets erases it with the rest, and the file it opened is put
//   aside at the next start.
// * Read at start, before any sign-in can go out (read(), on the interface thread: one small file, once). A file that
//   is damaged, is of another version, or does not open under the key, is put aside beside it (`.unreadable`), and the
//   program starts with nothing held, which is said in the log (and by putAside()). Where Windows does not hand over
//   the key, or keeps no new one, nothing is read or written in this run, as before a9: the pacing holds in memory.
// * Written a while after the first change (`writtenAfterMs`, for all that changes meanwhile), and OFF the interface
//   thread: on the interface thread only what is kept is copied out (SignInPacing::saved: as many as are kept, at most
//   SignInPacing::mostKept beside those that still protect, each a few short texts); the JSON, the sealing and the
//   writing (QSaveFile) are done on a thread of the pool, one write at a time, the next one after it where more changed
//   meanwhile. At the program's end (this going, after the loop has ended) the writing under way is waited for, five
//   seconds at the most, and what is still to write is written then, on this thread: nothing of the loop is run from
//   inside this going (from the merged review, 8).
// * A hold read back is never longer than `longestHoldMs` (a clock set back since), and one that has run out is kept
//   for its counts: the next no after it still waits longer.
// * A KVM taken off the wall has its entries cleared (SignInPacing::forgetPlace, LocalWall::remove), and so are they in
//   the file at its next writing.
//
// On the interface thread.
class PacingFile : public QObject {
    Q_OBJECT

public:
    static constexpr const char* keyName = "sign-in-pacing";
    static constexpr int version = 1;

    struct Pace {
        int writtenAfterMs = 1000;
        // Milliseconds since the epoch on the PC's clock (a test's own); none: the PC's.
        std::function<qint64()> wallMs;
        qint64 longestHoldMs = 3'600'000;
    };

    PacingFile(SignInPacing& pacing, QString file, SecretStore& secrets, Pace pace, QObject* parent = nullptr);
    PacingFile(SignInPacing& pacing, QString file, SecretStore& secrets, QObject* parent = nullptr);
    ~PacingFile() override;
    PacingFile(const PacingFile&) = delete;
    PacingFile& operator=(const PacingFile&) = delete;
    PacingFile(PacingFile&&) = delete;
    PacingFile& operator=(PacingFile&&) = delete;

    // What the file keeps, taken back into the pacing; and from then on, what changes there is written down.
    void read();
    // Where a file that could not be read was put; empty where none was.
    const QString& putAside() const { return m_putAside; }
    // Whether it is kept on disk in this run (the key was had, or made).
    bool keeps() const { return m_key.has_value(); }
    // Waits, the loop running, until what changed is written (`withinMs` at most): for a test, and for this going.
    bool waitForWritten(int withinMs);
    // The thread the last writing ran on; none before one did.
    QThread* lastWrittenOn() const { return m_lastWrittenOn; }
    // How many writings have ended.
    int writings() const { return m_writings; }
    // Whether a writing is under way, off the interface thread.
    bool isWriting() const { return m_writing; }

private:
    struct Writing;

    qint64 wallNow() const;
    void changed();
    void write();
    void wrote(QThread* on);
    // The file put aside, beside where it was (`why`: said in the log).
    void setAside(const char* why);

    SignInPacing& m_pacing;
    QString m_file;
    SecretStore& m_secrets;
    Pace m_pace;
    std::optional<SecretKey> m_key;
    QTimer m_soon;
    std::shared_ptr<Writing> m_writingNow; // the writing under way, or the last one
    bool m_writing = false;
    bool m_again = false;
    QString m_putAside;
    QThread* m_lastWrittenOn = nullptr;
    int m_writings = 0;
};

} // namespace qkvm::core
