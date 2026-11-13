#pragma once

#include <utility>

#include <QList>
#include <QString>
#include <QStringList>

#include "tray.h"

namespace qkvm::ui {

// An icon in a notification area that is only pretended: it notes what it was told, and a test
// does with it what somebody at the PC would. For the tests and the rigs; the program itself puts
// its icon into Windows' own.
class PretendedTray final : public TrayIcon {
public:
    using TrayIcon::TrayIcon;

    void show(MarkDot withDot, const QString& saying) override
    {
        ++told;
        dot = withDot;
        says = saying;
        there = hasRoom;
    }
    void hide() override { there = false; }
    bool isThere() const override { return there; }
    void setMenu(const QList<Row>& rows) override { menu = rows; }

    // The taskbar has come, and the icon has put itself there with what it was told last.
    void taskbarCame()
    {
        hasRoom = true;
        there = true;
    }
    // Somebody clicks the icon; somebody chooses the row of its menu that says this.
    void click() { emit opened(); }
    void finishMessage(bool clicked) { notificationFinished(clicked); }
    void chooseSnapshot(const QList<Row>& snapshot, int id) { chooseMenuRow(snapshot, id); }
    bool choose(const QString& text)
    {
        for (const Row& row : std::as_const(menu)) {
            if (row.id != 0 && row.text == text) {
                chooseMenuRow(menu, row.id);
                return true;
            }
        }
        return false;
    }
    // What its menu says, row by row; a line between rows is "-".
    QStringList rows() const
    {
        QStringList texts;
        for (const Row& row : menu) {
            texts.append(row.id == 0 ? QStringLiteral("-") : row.text);
        }
        return texts;
    }

    // A message it was told to show.
    struct Said {
        QString title;
        QString text;
        bool isWarning = false;
    };

    bool hasRoom = true;           // false: as on a PC whose taskbar is not running
    bool finishesMessages = false; // legacy teller tests model banners expiring without a click
    bool there = false;
    MarkDot dot = MarkDot::None;
    QString says;
    QList<Row> menu;
    int told = 0; // how often it was told what to show
    // Every message it was told to show, in order.
    QList<Said> messages;

private:
    void showMessage(const QString& title, const QString& text, bool isWarning) override
    {
        messages.append({title, text, isWarning});
        if (finishesMessages)
            notificationFinished(false);
    }
};

} // namespace qkvm::ui
