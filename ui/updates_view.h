#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/updates.h>

namespace qkvm::ui {

class UpdatesView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY changed)
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(QString notes READ notes NOTIFY changed)
    Q_PROPERTY(QString details READ details NOTIFY changed)
    Q_PROPERTY(bool automatic READ automatic WRITE setAutomatic NOTIFY changed)
    Q_PROPERTY(bool canCheck READ canCheck NOTIFY changed)
    Q_PROPERTY(bool canDownload READ canDownload NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canRecover READ canRecover NOTIFY changed)
    Q_PROPERTY(bool showsFile READ showsFile CONSTANT)
    Q_PROPERTY(bool restarts READ restarts CONSTANT)
    Q_PROPERTY(QString afterSetup READ afterSetup NOTIFY changed)
    Q_PROPERTY(bool afterSetupWentWrong READ afterSetupWentWrong NOTIFY changed)
public:
    explicit UpdatesView(core::Updates& updates, QObject* parent = nullptr);
    QString currentVersion() const { return m_updates.currentVersion(); }
    QString status() const;
    QString problem() const { return m_problem.isEmpty() ? m_updates.problem() : m_problem; }
    QString lastChecked() const;
    QString version() const;
    QString notes() const;
    QString details() const;
    bool automatic() const { return m_updates.automatic(); }
    void setAutomatic(bool on) { m_updates.setAutomatic(on); }
    bool canCheck() const { return m_updates.configured() && !busy() && !ready(); }
    bool canDownload() const { return m_updates.state() == core::Updates::State::Available; }
    bool ready() const { return m_updates.state() == core::Updates::State::Ready; }
    bool busy() const { return m_updates.busy(); }
    bool canRecover() const { return static_cast<bool>(m_recover) && !busy(); }
    // A portable copy's verified update is shown in its folder; an MSIX copy's is handed to its installer.
    bool showsFile() const { return m_updates.kind() == core::UpdateKind::Portable; }
    // A Setup copy's is installed by Setup, run verified, once qKVM has closed: "Install and restart".
    bool restarts() const { return m_updates.kind() == core::UpdateKind::Setup; }
    // What this start found of the run before's hand-over to Setup (core::Updates::afterSetup).
    QString afterSetup() const { return m_updates.afterSetup(); }
    bool afterSetupWentWrong() const { return m_updates.afterSetupWentWrong(); }
    // Both are supplied by the application; absent handlers never permit installation.
    void installWith(std::function<bool()> idle, std::function<bool(const QString&)> open);
    // Supplied by the application: what keeps it from installing now, in the person's words (empty: nothing), said
    // where `idle` says no.
    void holdWith(std::function<QString()> holds);
    // Supplied by the application: once Setup has started, closes every window and ends the program within 10 s
    // (Setup waits for that). Absent, Setup is never started.
    void handOverWith(std::function<void()> handOver);
    void recoverWith(std::function<bool(bool)> recover);
    // Supplied by the application: shows a file in its folder, never opening or running it.
    void showWith(std::function<bool(const QString&)> show);
    Q_INVOKABLE void check();
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void install();
    Q_INVOKABLE void showFile();
    Q_INVOKABLE void recover(bool previous);
signals:
    void changed();

private:
    core::Updates& m_updates;
    QString m_problem;
    bool m_openedInstaller = false;
    std::function<bool()> m_idle;
    std::function<bool(const QString&)> m_open;
    std::function<bool(bool)> m_recover;
    std::function<bool(const QString&)> m_show;
    std::function<void()> m_handOver;
    std::function<QString()> m_holds;
    // What keeps it from installing, where something does: what `holds` says, or what is always said.
    QString held() const;
    bool m_shownFile = false;
    bool m_handedOver = false;
};

} // namespace qkvm::ui
