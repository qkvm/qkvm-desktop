#pragma once

#include <functional>

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/discovery.h>

namespace qkvm::ui {

// A discovery belongs to one open Add KVM dialog. Nothing is asked on construction or when the
// dialog opens: the person chooses the networks and presses Search. A result only supplies an
// address to the existing add workflow, which checks the device and its certificate again before
// signing in. No credentials, remembered trust, or wall changes belong to discovery.
class DiscoveryView : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList networks READ networks NOTIFY networksChanged)
    Q_PROPERTY(QString manualScope READ manualScope WRITE setManualScope NOTIFY changed)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(bool confirmationNeeded READ confirmationNeeded NOTIFY changed)
    Q_PROPERTY(int planned READ planned NOTIFY changed)

public:
    enum Role { AddressRole = Qt::UserRole + 1, FamilyRole, SupportedRole, EncryptedRole };

    explicit DiscoveryView(QObject* parent = nullptr);
    // Tests inject a scanner that never uses the network, and a fixed interface inventory.
    DiscoveryView(core::Discovery& scanner, const QList<core::Discovery::Network>& networks, QObject* parent = nullptr);
    ~DiscoveryView() override;

    QVariantList networks() const;
    QString manualScope() const { return m_manualScope; }
    void setManualScope(const QString& scope);
    bool active() const { return m_active; }
    void setActive(bool active);
    bool busy() const { return m_busy; }
    int completed() const { return m_completed; }
    int total() const { return m_total; }
    int count() const { return static_cast<int>(m_devices.size()); }
    QString state() const { return m_state; }
    QString problem() const { return m_problem; }
    bool confirmationNeeded() const { return m_confirmationNeeded; }
    int planned() const { return m_planned; }

    Q_INVOKABLE void prepare();
    Q_INVOKABLE void closed();
    Q_INVOKABLE void selectNetwork(int index, bool selected);
    Q_INVOKABLE void start(bool confirmed = false);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void choose(int index);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void changed();
    void networksChanged();
    // A result's address, for the box that adds a KVM, and the family of what answered there ("kvmd",
    // "jetkvm": OwnKvm::kind), for the box to show what that family has (a JetKVM no user name).
    void addressChosen(const QString& address, const QString& family);

private:
    void listen();
    void forgetResults();
    void forgetConfirmation();
    QStringList scopes() const;

    QPointer<core::Discovery> m_scanner;
    std::function<QList<core::Discovery::Network>()> m_networksProvider;
    QList<core::Discovery::Network> m_networks;
    QSet<int> m_selected;
    QList<core::Discovery::Device> m_devices;
    QString m_manualScope;
    QString m_state = QStringLiteral("idle");
    QString m_problem;
    bool m_active = false;
    bool m_busy = false;
    int m_completed = 0;
    int m_total = 0;
    bool m_confirmationNeeded = false;
    int m_planned = 0;
};

} // namespace qkvm::ui
