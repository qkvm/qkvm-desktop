#pragma once

#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/incidents.h>
#include <qkvm/core/screen_watch.h>
#include <qkvm/core/server_types.h>

#include "app_lock.h"
#include "quick_access.h"

namespace qkvm::ui {
class AlertsView final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application.")
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(bool allowed READ allowed NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(QString pauseEnds READ pauseEnds NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(bool damaged READ damaged NOTIFY changed)
    Q_PROPERTY(int attention READ attention NOTIFY changed)
    Q_PROPERTY(bool history READ history WRITE setHistory NOTIFY changed)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed)
    Q_PROPERTY(QString scope READ scope NOTIFY changed)
    Q_PROPERTY(QString sourceStatus READ sourceStatus NOTIFY changed)
public:
    AlertsView(core::Incidents& incidents, AppLock& lock, QObject* parent = nullptr);
    // Each scope is a local wall or a server/account identity, never a transient session token.
    void context(const QString& scope, bool local);
    void inventory(const QList<core::Machine>& machines);
    void observe(const QHash<QString, core::Status>& status, bool inContact);
    void watch(const QString& scope, bool local, const QString& name);
    void suspend(const QString& scope);
    void unavailable(const QString& scope, const QString& name, const QString& reason);
    void forgetSource(const QString& scope);
    void inventoryFor(const QString& scope, const QList<core::Machine>& machines);
    void observeFor(const QString& scope, const QHash<QString, core::Status>& status, bool inContact);
    // A screen watch of this PC's own wall went off, or is over (c7): an entry of its own, told as a change is.
    void screenAlertFor(const QString& scope, const core::ScreenWatch::Event& event);
    // A screen watch's alert in the page's words (app.js, watchText): "Screen unchanged for 30 minutes", "Back to
    // normal: a picture again".
    static QString watchText(const QString& watch, bool back, int minutes);
    QVariantList sources() const;
    QVariantMap treeSource(const QString& scope) const;
    QList<MachineChoice> availableMachines() const;
    QString scope() const { return !m_lock.restricted() ? m_scope : QString(); }
    QString sourceStatus() const;
    Q_INVOKABLE void choose(const QString& scope);
    Q_INVOKABLE void openSource();
    QVariantList entries() const;
    bool allowed() const
    {
        return m_channels.contains(m_scope) && m_channels.value(m_scope).active && !m_lock.restricted();
    }
    bool paused() const;
    QString pauseEnds() const;
    QString problem() const { return allowed() ? m_incidents.problem() : QString(); }
    bool damaged() const { return allowed() && m_incidents.damaged(); }
    int attention() const;
    bool history() const { return m_history; }
    void setHistory(bool history);
    Q_INVOKABLE void acknowledge(const QString& id);
    Q_INVOKABLE void pause(bool paused);
    Q_INVOKABLE void pauseDevice(const QString& id, bool paused);
    Q_INVOKABLE void repair();
    static QString label(const QString& state);
signals:
    void changed();
    void openAsked(const QString& scope);
    void notification(const QString& title, const QString& text, bool warning, const QString& scope,
                      const QString& machine);

private:
    bool visible(const core::Incidents::Entry& entry) const;
    core::Incidents& m_incidents;
    AppLock& m_lock;
    QString m_scope;
    bool m_history = false;
    struct Channel {
        QString name;
        QString problem;
        bool local = false;
        bool active = true;
        bool inContact = true;
        QHash<QString, core::Machine> machines;
        QHash<QString, QString> states;
    };
    QHash<QString, Channel> m_channels;
    QTimer m_expiry;
    QSet<QString> m_lastMuted;
};
} // namespace qkvm::ui
