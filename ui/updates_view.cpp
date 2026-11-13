#include "updates_view.h"

#include <utility>

#include <QLocale>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

UpdatesView::UpdatesView(core::Updates& updates, QObject* parent) : QObject(parent), m_updates(updates)
{
    connect(&updates, &core::Updates::changed, this, &UpdatesView::changed);
}

QString UpdatesView::status() const
{
    const auto& release = m_updates.release();
    using State = core::Updates::State;
    switch (m_updates.state()) {
    case State::Unavailable:
        return tr("Updates are not available for this development build.");
    case State::Idle:
        return tr("No update check has completed in this session.");
    case State::Checking:
        return tr("Checking for updates...");
    case State::Current:
        return tr("You have the latest available version.");
    case State::Available:
        return tr("qKVM %1 is available.").arg(version());
    case State::Downloading:
        return tr("Downloading update: %1 of %2")
            .arg(QLocale().formattedDataSize(m_updates.received()),
                 QLocale().formattedDataSize(release ? release->bytes : 0));
    case State::Ready:
        if (showsFile()) {
            return m_shownFile ? tr("The verified file is shown in its folder.")
                               : tr("qKVM %1 is downloaded and verified.").arg(version());
        }
        if (restarts()) {
            return m_handedOver ? tr("qKVM Setup has started. qKVM closes now, and starts again once the update is "
                                     "installed.")
                                : tr("qKVM %1 is downloaded and verified.").arg(version());
        }
        return m_openedInstaller ? tr("Continue in qKVM Setup. qKVM has not been closed.")
                                 : tr("The update is verified and ready to install.");
    case State::Error:
        return tr("The update could not be completed.");
    }
    return {};
}

QString UpdatesView::lastChecked() const
{
    return m_updates.lastChecked().isValid()
               ? QLocale().toString(m_updates.lastChecked().toLocalTime(), QLocale::ShortFormat)
               : tr("Never");
}

QString UpdatesView::version() const
{
    const auto& release = m_updates.release();
    return release ? release->version : QString();
}

QString UpdatesView::notes() const
{
    const auto& release = m_updates.release();
    return release ? release->notes : QString();
}

QString UpdatesView::details() const
{
    const auto& release = m_updates.release();
    return release ? tr("Released %1 | %2")
                         .arg(QLocale().toString(release->published.toLocalTime().date(), QLocale::ShortFormat),
                              QLocale().formattedDataSize(release->bytes))
                   : QString();
}

void UpdatesView::installWith(std::function<bool()> idle, std::function<bool(const QString&)> open)
{
    m_idle = std::move(idle);
    m_open = std::move(open);
}

void UpdatesView::handOverWith(std::function<void()> handOver)
{
    m_handOver = std::move(handOver);
}

void UpdatesView::holdWith(std::function<QString()> holds)
{
    m_holds = std::move(holds);
}

QString UpdatesView::held() const
{
    const QString said = m_holds ? m_holds() : QString();
    return said.isEmpty()
               ? tr("Finish active control, actions and file operations in all qKVM windows before installing.")
               : said;
}

void UpdatesView::recoverWith(std::function<bool(bool)> recover)
{
    m_recover = std::move(recover);
    emit changed();
}

void UpdatesView::recover(bool previous)
{
    m_problem.clear();
    if (busy() || !m_idle || !m_recover || !m_idle()) {
        m_problem = tr("Finish active control, actions and file operations in all qKVM windows before recovery.");
    } else if (!m_recover(previous)) {
        m_problem = tr("qKVM Setup could not be opened. Use the complete installer folder to recover.");
    }
    emit changed();
}

void UpdatesView::check()
{
    m_problem.clear();
    m_openedInstaller = false;
    m_shownFile = false;
    m_handedOver = false;
    m_updates.check();
    emit changed();
}

void UpdatesView::download()
{
    m_problem.clear();
    m_openedInstaller = false;
    m_shownFile = false;
    m_handedOver = false;
    m_updates.download();
    emit changed();
}

void UpdatesView::cancel()
{
    m_problem.clear();
    m_openedInstaller = false;
    m_shownFile = false;
    m_handedOver = false;
    m_updates.cancel();
    emit changed();
}

void UpdatesView::install()
{
    if (!ready()) {
        return;
    }
    m_problem.clear();
    if (restarts()) {
        // Setup is started only where every window is idle and the program can then close them and end: Setup waits
        // for that before it installs.
        if (!m_handOver) {
            m_problem = tr("This copy of qKVM cannot close itself for qKVM Setup.");
        } else if (!m_idle || !m_idle()) {
            m_problem = held();
        } else if (m_updates.runSetup()) {
            m_handedOver = true;
            emit changed();
            if (m_handOver) {
                m_handOver();
            }
            return;
        }
        emit changed();
        return;
    }
    if (!m_idle || !m_open || !m_idle()) {
        m_problem = held();
    } else {
        m_openedInstaller = m_updates.openInstaller(m_open);
    }
    emit changed();
}

void UpdatesView::showWith(std::function<bool(const QString&)> show)
{
    m_show = std::move(show);
}

void UpdatesView::showFile()
{
    if (!ready() || !showsFile()) {
        return;
    }
    m_problem.clear();
    if (!m_show) {
        m_problem = tr("This copy of qKVM cannot show the file.");
    } else {
        m_shownFile = m_updates.showFile(m_show);
    }
    emit changed();
}

} // namespace qkvm::ui
