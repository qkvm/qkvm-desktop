#include "tray.h"

#include <QCoreApplication>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Tray", text);
}

} // namespace

// The owned snapshot is required: signal handlers can replace the caller's menu synchronously.
void TrayIcon::chooseMenuRow(QList<Row> snapshot, int id) // NOLINT(performance-unnecessary-value-param)
{
    // A chosen action may synchronously replace the current menu. Keep its rows alive.
    for (const auto& row : snapshot) {
        if (!row.enabled)
            continue;
        if (!row.children.isEmpty()) {
            chooseMenuRow(row.children, id);
        } else if (row.id == id && id != 0) {
            if (!row.scope.isEmpty())
                emit shortcutChosen(row.scope, row.machine);
            else
                emit chosen(id);
            return;
        }
    }
}

void TrayIcon::tell(const QString& title, const QString& text, bool isWarning)
{
    tellAbout(title, text, isWarning, {}, {});
}

void TrayIcon::tellAbout(const QString& title, const QString& text, bool warning, const QString& scope,
                         const QString& machine)
{
    if (!isThere() || m_waiting.size() >= 20)
        return;
    m_waiting.append({title, text, warning, scope, machine});
    tellNext();
}

void TrayIcon::tellNext()
{
    if (m_notification || m_waiting.isEmpty())
        return;
    if (!isThere()) {
        m_waiting.clear();
        return;
    }
    m_notification = m_waiting.takeFirst();
    // Keep copies alive even when a backend finishes the message synchronously.
    const auto notification = *m_notification;
    showMessage(notification.title, notification.text, notification.warning);
}

void TrayIcon::clearNotifications()
{
    m_waiting.clear();
    if (m_notification) {
        m_notification->scope.clear();
        m_notification->machine.clear();
    }
}

void TrayIcon::notificationFinished(bool clicked)
{
    if (!m_notification)
        return;
    const auto notification = *m_notification;
    m_notification.reset();
    if (clicked && !notification.scope.isEmpty())
        emit notificationOpened(notification.scope, notification.machine);
    QTimer::singleShot(0, this, &TrayIcon::tellNext);
}

TrayState trayStateOf(const QString& stage, bool inContact, int all, int online, int noSignal, int attention)
{
    // The program's name first: it is what Windows shows where it lists the icons.
    const QString name = u"qKVM"_s;
    if (stage != "wall"_L1) {
        return {MarkDot::None, name + u'\n' + words("Not signed in")};
    }
    if (!inContact) {
        return {MarkDot::Bad, name + u'\n' + words("Lost contact with qKVM")};
    }
    // The bar's own counts, in the bar's words.
    const QString counts = words("KVMs: %1 · Online: %2 · No signal: %3 · Attention: %4")
                               .arg(all)
                               .arg(online)
                               .arg(noSignal)
                               .arg(attention);
    return {attention > 0 ? MarkDot::Bad : noSignal > 0 ? MarkDot::Warn : MarkDot::None, name + u'\n' + counts};
}

} // namespace qkvm::ui
