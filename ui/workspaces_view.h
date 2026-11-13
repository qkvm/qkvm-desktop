#pragma once

#include "app_lock.h"
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <qkvm/core/settings.h>

namespace qkvm::ui {

class WorkspacesView final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application.")
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(bool allowed READ allowed NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
public:
    explicit WorkspacesView(core::Settings& settings, QObject* parent = nullptr);
    void useAppLock(AppLock& lock);
    void useWindows(std::function<QList<core::WallWindow>()> capture,
                    std::function<bool(const QList<core::WallWindow>&)> restore);
    QVariantList entries() const;
    bool allowed() const { return m_lock == nullptr || !m_lock->restricted(); }
    QString problem() const { return allowed() ? m_problem : QString(); }
    Q_INVOKABLE bool save(const QString& name, const QString& replace = {});
    Q_INVOKABLE bool rename(const QString& id, const QString& name);
    Q_INVOKABLE bool remove(const QString& id);
    Q_INVOKABLE bool restore(const QString& id);
    void reportProblem(const QString& text)
    {
        fail(text);
        emit problemReported(text);
    }
signals:
    void changed();
    void problemReported(const QString& text);

private:
    bool validName(const QString& name, const QString& id);
    bool keep(const core::Remembered& remembered);
    bool fail(const QString& text);
    core::Settings& m_settings;
    QPointer<AppLock> m_lock;
    QString m_problem;
    std::function<QList<core::WallWindow>()> m_capture;
    std::function<bool(const QList<core::WallWindow>&)> m_restore;
};

} // namespace qkvm::ui
