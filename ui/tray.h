#pragma once

#include <memory>
#include <optional>

#include <QList>
#include <QObject>
#include <QString>

#include "mark.h"

namespace qkvm::ui {

// The program's icon in the notification area of Windows' taskbar, as far as the program deals
// with it: the program's mark with or without a dot, what it says to a pointer that rests on it, a
// menu, and what somebody does with it. Behind it Windows' own (ui/win/tray.cpp), or in a test one
// that is only pretended.
class TrayIcon : public QObject {
    Q_OBJECT

public:
    // A row of its menu; `id` 0 is a line between rows.
    struct Row {
        int id = 0;
        QString text;
        QString scope = {};
        QString machine = {};
        QList<Row> children = {};
        bool enabled = true;

        bool operator==(const Row&) const = default;
    };

    using QObject::QObject;

    // Puts it there, or changes what is there. Where Windows has no place for it just now (no
    // taskbar is running yet, as it can be right after somebody signed in to Windows), it is put
    // there when the taskbar comes, with what it was told last.
    virtual void show(MarkDot dot, const QString& says) = 0;
    virtual void hide() = 0;
    // Whether Windows has it now.
    virtual bool isThere() const = 0;
    // A message from the icon, as Windows shows such: a heading, a line under it, and whether it
    // is a warning. Where and for how long is Windows' to decide, and whoever asked Windows for
    // quiet is not shown it at all.
    void tell(const QString& title, const QString& text, bool isWarning);
    virtual void setMenu(const QList<Row>& rows) = 0;
    // Keep the target with its notification until Windows finishes that notification.
    // A later status change must never replace the target of a visible message.
    void tellAbout(const QString& title, const QString& text, bool warning, const QString& scope,
                   const QString& machine);
    void clearNotifications();
    void clearWaitingNotifications() { m_waiting.clear(); }

signals:
    // Somebody clicked it, or chose it with the keyboard.
    void opened();
    // A row of its menu was chosen.
    void chosen(int id);
    void notificationOpened(const QString& scope, const QString& machine);
    void shortcutChosen(const QString& scope, const QString& machine);

protected:
    void chooseMenuRow(QList<Row> snapshot, int id);
    virtual void showMessage(const QString& title, const QString& text, bool isWarning) = 0;
    void notificationFinished(bool clicked);

private:
    struct Notification {
        QString title;
        QString text;
        bool warning = false;
        QString scope;
        QString machine;
    };
    void tellNext();
    QList<Notification> m_waiting;
    std::optional<Notification> m_notification;
};

// What the icon shows of the wall: a dot beside the mark, and a line for the pointer that rests on it.
struct TrayState {
    MarkDot dot = MarkDot::None;
    QString says;

    bool operator==(const TrayState&) const = default;
};

// `stage` and `inContact` as SessionView has them; the counts as the bar shows them. Something
// that wants attention outweighs a machine without a signal; a QKVM that does not answer
// outweighs both, since nothing on the wall is known to be so any more.
TrayState trayStateOf(const QString& stage, bool inContact, int all, int online, int noSignal, int attention);

// Windows' own notification area.
std::unique_ptr<TrayIcon> windowsTray();

} // namespace qkvm::ui
