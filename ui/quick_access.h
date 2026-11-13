#pragma once

#include <QObject>

#include <qkvm/core/settings.h>

namespace qkvm::ui {
class AlertsView;
class AppLock;

struct MachineChoice {
    core::MachineReference reference;
    QString name;
    QString source;
    bool operator==(const MachineChoice&) const = default;
};

// Only identities are saved. Names and availability come from authenticated inventories.
class QuickAccess final : public QObject {
    Q_OBJECT
public:
    QuickAccess(core::Settings& settings, AlertsView& sources, AppLock& lock, QObject* parent = nullptr);
    bool allowed() const;
    QList<MachineChoice> favorites() const;
    QList<MachineChoice> recent() const;
    bool isFavorite(const core::MachineReference& reference) const;
    void toggleFavorite(const core::MachineReference& reference);
    void opened(const core::MachineReference& reference);
    void clearRecent();
    void clearFavorites();
signals:
    void changed();
    void problem(const QString& text);

private:
    QList<MachineChoice> resolve(const QList<core::MachineReference>& references) const;
    bool known(const core::MachineReference& reference) const;
    void save(const core::Remembered& remembered);
    void refresh();
    core::Settings& m_settings;
    AlertsView& m_sources;
    AppLock& m_lock;
    QList<MachineChoice> m_known;
    bool m_allowed = false;
};
} // namespace qkvm::ui
