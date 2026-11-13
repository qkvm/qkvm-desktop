#pragma once

#include <functional>
#include <memory>

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/kvm_probe.h>

namespace qkvm::core {

// Discovery is explicitly started by a person. Listing interfaces and planning a search send
// nothing. Each target is asked anonymously by KvmProbe; adding and trusting it remain separate.
// This first slice accepts numeric private IPv4 addresses, CIDRs and inclusive address ranges.
// Names, IPv6, announcements and Tailscale peer enumeration come later.
class Discovery : public QObject {
    Q_OBJECT

public:
    static constexpr int maxTargets = 4096;
    static constexpr int confirmationThreshold = 256;
    // A KvmProbe asks three family endpoints together, so this is at most 48 requests at once.
    static constexpr int maxConcurrent = 16;

    struct Network {
        QString interfaceName;
        QString label;
        QString address;
        QString cidr;
        int prefixLength = 0;
        quint64 hostCount = 0;
    };
    struct Plan {
        int total = 0;
        bool needsConfirmation = false;
    };
    struct Device {
        QUrl address;
        KvmProbe::Kind kind = KvmProbe::Kind::Nothing;
        QByteArray certificate;
        bool trustedByWindows = false;
    };

    // The adapter name, actual address and actual prefix; no /24 assumption. Unusable, down,
    // loopback and nonprivate IPv4 interfaces are omitted. Networks larger than the scan limit
    // are still listed so the person can enter a smaller range. Nothing is selected here.
    static QList<Network> networks();
    // Every nonblank item is one scope: 192.168.1.20, http(s)://192.168.1.20:8443,
    // 192.168.1.0/24 or 192.168.1.20-192.168.1.40. CIDRs exclude network/broadcast
    // addresses except /31 and /32. Only RFC1918 and 169.254/16 are accepted. CGNAT
    // requires a future Tailscale peer source and cannot be swept. A network broader than
    // /20 is refused, including one whose overlap would otherwise fit the target limit.
    static base::Result<Plan> planFor(const QStringList& scopes);

    // Tests replace only the anonymous probe. The callback may be synchronous or asynchronous;
    // its result is processed on a later event-loop turn. Deleting lifetime must stop all
    // work, including network requests. A late callback cannot revive a cancelled search.
    using Probe = std::function<void(const QUrl&, QObject* lifetime, std::function<void(KvmProbe::Found)>)>;

    explicit Discovery(QObject* parent = nullptr);
    explicit Discovery(Probe probe, QObject* parent = nullptr);
    ~Discovery() override;

    // Validates before changing any existing run. Only one search may run across all windows. Searches
    // over 256 targets require a separate explicit confirmation. Traffic begins after return.
    base::Result<void> start(const QStringList& scopes, bool confirmedLarge = false);
    // Stops pending and active work immediately; emits finished(true) only for an active run.
    void cancel();
    bool running() const { return m_running; }

signals:
    void progress(int completed, int total);
    void found(const qkvm::core::Discovery::Device& device);
    // A damaged TLS installation stops the search instead of falling back to plain HTTP.
    void problem(const QString& message);
    void finished(bool cancelled);

private:
    struct Target {
        QUrl address;
        bool tryHttp = false;
    };
    static base::Result<QList<Target>> targetsFor(const QStringList& scopes);
    void pump(quint64 generation);
    void ask(int index, const QUrl& address);
    void answered(quint64 generation, int index, QObject* attempt, const QUrl& address, const KvmProbe::Found& answer);
    void stop(bool cancelled);

    Probe m_probe;
    QList<Target> m_targets;
    QHash<int, QObject*> m_active;
    std::unique_ptr<QObject> m_run;
    quint64 m_generation = 0;
    int m_next = 0;
    int m_completed = 0;
    bool m_running = false;
};

} // namespace qkvm::core

Q_DECLARE_METATYPE(qkvm::core::Discovery::Device)
