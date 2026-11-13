#pragma once

#include <QObject>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include "app_lock.h"
#include <qkvm/core/own_actions.h>

namespace qkvm::ui {

class OwnActionsView final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application.")
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(bool allowed READ allowed NOTIFY changed)
public:
    OwnActionsView(core::OwnActions& actions, AppLock& lock, QObject* parent = nullptr);
    QVariantList entries() const;
    bool allowed() const { return !m_lock.restricted(); }
    // Empty means success. Only metadata leaves this model, including on a failed edit.
    Q_INVOKABLE QString save(const QString& id, int revision, const QString& name, bool types, const QString& keys,
                             const QString& text, bool enter, bool danger);
    Q_INVOKABLE QString remove(const QString& id, int revision);
signals:
    void changed();

private:
    core::OwnActions& m_actions;
    AppLock& m_lock;
};
} // namespace qkvm::ui
