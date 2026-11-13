#pragma once

#include <functional>
#include <optional>
#include <thread>

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/secrets.h>
#include <qkvm/core/wiped.h>

namespace qkvm::ui {

// A pending export owns this copy. It is wiped even if its password prompt is cancelled.
struct ExportPassword {
    explicit ExportPassword(const QString& value) : text(value) { text.detach(); }
    QString text;
    core::WipedOnLeaving<QString> wipe{text};
};

// One authority for all windows. Password work runs off the interface thread. An
// approval belongs to one request and is discarded on cancellation, lock or reset.
class AppLock final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application.")
    Q_PROPERTY(bool restricted READ restricted NOTIFY changed)
    Q_PROPERTY(bool locked READ locked NOTIFY changed)
    Q_PROPERTY(bool wallScreen READ wallScreen NOTIFY changed)
    Q_PROPERTY(bool automatic READ automatic NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(int retryAfter READ retryAfter NOTIFY changed)

public:
    using Derive = std::function<base::Result<QByteArray>(QByteArrayView, QByteArrayView, const std::stop_token&)>;
    // The production marker is <settings file>.locked; inventory/incidents are prior-use evidence.
    AppLock(core::SecretStore& secrets, const QString& lockFile, QObject* parent = nullptr);
    // A test supplies inexpensive work and its own clocks; production always uses scrypt. `now` is
    // the PC's clock in seconds; `steady` a clock in seconds that nobody sets (M-D2), the same as
    // `now` where a test gives none.
    AppLock(core::SecretStore& secrets, Derive derive, std::function<qint64()> now, QObject* parent = nullptr,
            QString lockFile = {}, std::function<qint64()> steady = {});
    ~AppLock() override;

    bool restricted() const { return m_locked || m_screen || m_recovery || m_setup || m_unavailable; }
    bool locked() const { return m_locked || m_recovery || m_setup || m_unavailable; }
    bool wallScreen() const { return m_screen; }
    bool automatic() const { return m_automatic; }
    bool busy() const { return m_busy; }
    QString mode() const { return m_mode; }
    QString problem() const { return m_problem; }
    int retryAfter() const;
    bool canStartSources() const { return !m_setup && !m_recovery && !m_unavailable; }

    Q_INVOKABLE void ask(const QString& purpose);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void lock();
    Q_INVOKABLE void submit(const QString& current, const QString& replacement, const QString& confirmation,
                            bool automatic = true);
    Q_INVOKABLE void disableAutomatic();
    Q_INVOKABLE void forgot();
    Q_INVOKABLE void resetSecrets(bool understood);
    // Where Windows did not hand over what qKVM keeps (mode "unavailable"): ask it again, or quit.
    Q_INVOKABLE void tryAgain();
    Q_INVOKABLE void quit();

    void becameWallScreen();
    void endScreenWith(std::function<bool()> end) { m_endScreen = std::move(end); }
    bool authorizeExport(QObject* owner, std::function<void()> approved);
    void cancelApproval(QObject* owner);
    static QString recordName();
    static base::Result<QByteArray> derivePassword(QByteArrayView password, QByteArrayView salt,
                                                   const std::stop_token& stop);

signals:
    void changed();
    void clearFields();
    void endWallScreen();
    void resetStarting();
    void resetFinished();
    void approvalCancelled(QObject* owner);
    void quitAsked();

private:
    bool persist();
    bool latch(bool resetComplete = false);
    bool unlatch();
    void load();
    bool quietForADay() const;
    // The wait the current count of wrong tries owes: 1, 2, 4 ... seconds, at most 300.
    qint64 owedWait() const;
    void becomeUnavailable();
    void invalidate();
    void finish(quint64 turn, const QString& purpose, bool matches, bool worked, const QByteArray& salt,
                const QByteArray& verifier, bool automatic);
    core::SecretStore& m_secrets;
    QString m_lockFile;
    Derive m_derive;
    std::function<qint64()> m_now;
    QJsonObject m_record;
    QString m_mode;
    QString m_problem;
    bool m_setup = false;
    bool m_passwordSaved = false;
    bool m_marked = false;
    bool m_recovery = false;
    // Windows did not hand over the list of what qKVM keeps, or a record that list names: nothing is
    // known to be lost, so nothing is written and no reset is offered until it answers.
    bool m_unavailable = false;
    bool m_locked = true;
    bool m_screen = false;
    bool m_automatic = true;
    bool m_busy = false;
    int m_failures = 0;
    // When the next try may come, on the PC's clock (kept in the record), and within this run on the
    // steady clock too; the later of the two holds. Mutable: a clock seen to have gone back brings
    // the first down to the wait the count owes, from then (retryAfter()).
    mutable qint64 m_next = 0;
    std::optional<qint64> m_steadyNext;
    // When the last wrong try was, on each clock: a day without one forgets the count.
    qint64 m_last = 0;
    std::optional<qint64> m_steadyLast;
    std::function<qint64()> m_steady;
    quint64 m_turn = 0;
    QPointer<QObject> m_owner;
    QMetaObject::Connection m_ownerDied;
    std::function<void()> m_approved;
    std::function<bool()> m_endScreen;
    QTimer m_tick;
    std::jthread m_worker;
};

} // namespace qkvm::ui
