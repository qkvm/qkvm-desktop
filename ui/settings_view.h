#pragma once

#include <functional>

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/auto_start.h>
#include <qkvm/core/settings.h>
#include <qkvm/engine/gpu.h>
#include <qkvm/engine/types.h>

#include "alerts_view.h"
#include "app_lock.h"
#include "own_actions_view.h"
#include "updates_view.h"

namespace qkvm::ui {

// What the settings box shows and changes of how the program behaves on this PC, beside what the
// wall and the look keep themselves. Whether the program starts when its user signs in to Windows:
// that is Windows' own to know (it is switched in Windows' settings as well), so it is asked of
// Windows whenever the box is opened, and never kept here. And whether the program goes on behind
// its icon in the notification area when its window is closed, and whether Windows shows a message
// of the wall while nobody sees it: both remembered on this PC. And how video is decoded, which
// every window takes up at once (decodingChosen), and on which graphics adapter, which the program
// takes up when it starts.
//
// And the box's sections: which there are for what a window shows, in which groups, and which one
// the box opens at. The box is one for every window, in the page's shape (SettingsBox.qml): this
// PC's wall has sections of its own there, which are listed only where the window shows that wall;
// the program's are always there.
//
// Made in C++ and handed to the screens; on the interface thread. Whatever it is given outlives it.
class SettingsView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    Q_PROPERTY(bool startsWithWindows READ startsWithWindows WRITE setStartsWithWindows NOTIFY changed)
    Q_PROPERTY(bool staysInTray READ staysInTray WRITE setStaysInTray NOTIFY changed)
    Q_PROPERTY(bool tellsNews READ tellsNews WRITE setTellsNews NOTIFY changed)
    Q_PROPERTY(QString releaseKey READ releaseKey WRITE setReleaseKey NOTIFY changed)
    // How video is decoded, as core::Remembered::hardwareDecoding says: "automatic", "on" or "off".
    Q_PROPERTY(QString hardwareDecoding READ hardwareDecoding WRITE setHardwareDecoding NOTIFY changed)
    // Which graphics adapter, as core::Remembered::graphicsCard says: "most-powerful",
    // "power-saving" or "windows". Taken up when the program starts again.
    Q_PROPERTY(QString graphicsCard READ graphicsCard WRITE setGraphicsCard NOTIFY changed)
    // The choices of adapter in their order, each with what the adapter it comes to on this PC
    // calls itself, and whether a monitor is on it: [{"value": the choice, "adapter": the name, or
    // nothing where none is known, "monitor": false only where it is known that none is}]. Asked
    // when the box is opened (lookAgain), and never remembered.
    Q_PROPERTY(QVariantList graphicsCards READ graphicsCards NOTIFY changed)
    // The adapter chosen is not the choice the program started with.
    Q_PROPERTY(bool cardChangesAtTheNextStart READ cardChangesAtTheNextStart NOTIFY changed)
    // What came of the last change where Windows did not take it, as a sentence to show; empty
    // when nothing went wrong.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // The section of the box that was shown last, by its key; empty before one was (also in an
    // earlier run of the program: core::Remembered::settingsSection). Where the box opens the next
    // time, in whichever window, where that window lists it (opensAt).
    Q_PROPERTY(QString section READ section NOTIFY sectionChanged)
    Q_PROPERTY(qkvm::ui::AlertsView* alerts READ alerts NOTIFY changed)
    Q_PROPERTY(qkvm::ui::UpdatesView* updates READ updates NOTIFY changed)
    Q_PROPERTY(qkvm::ui::AppLock* security READ security NOTIFY changed)
    Q_PROPERTY(qkvm::ui::OwnActionsView* savedActions READ savedActions NOTIFY changed)

public:
    SettingsView(core::AutoStart& start, core::Settings& settings, QObject* parent = nullptr);
    // The program ends, also with a box open: the section it was left at is written down, as when
    // the box is closed (closed()), and nothing where that is written already.
    ~SettingsView() override;

    bool startsWithWindows() const { return m_startsWithWindows; }
    void setStartsWithWindows(bool on);
    bool staysInTray() const { return m_settings.remembered().staysInTray; }
    void setStaysInTray(bool stays);
    bool tellsNews() const { return m_settings.remembered().tellsNews; }
    void setTellsNews(bool tells);
    QString releaseKey() const { return m_settings.remembered().releaseKey; }
    void setReleaseKey(const QString& key);
    QString hardwareDecoding() const { return m_settings.remembered().hardwareDecoding; }
    void setHardwareDecoding(const QString& word);
    // The same for the engine.
    engine::DecodingChoice decoding() const;
    QString graphicsCard() const { return m_settings.remembered().graphicsCard; }
    void setGraphicsCard(const QString& word);
    QVariantList graphicsCards() const;
    bool cardChangesAtTheNextStart() const { return graphicsCard() != m_cardAtStart; }
    // The choice of adapter the program started with, for the engine: what it draws on until it
    // starts again.
    engine::AdapterChoice startedOn() const;
    // Where the adapters' names are asked: engine::Gpu::adapterFor in the program, made up in tests;
    // and whether a monitor is on each: engine::Gpu::adapterHasMonitor.
    void nameAdaptersWith(std::function<QString(engine::AdapterChoice)> names);
    void seeMonitorsWith(std::function<bool(engine::AdapterChoice)> monitors);
    QString problem() const { return m_problem; }
    QString section() const { return m_section; }
    AlertsView* alerts() const { return m_alerts; }
    void useAlerts(AlertsView& alerts)
    {
        m_alerts = &alerts;
        emit changed();
    }
    void openIncidents();
    UpdatesView* updates() const { return m_updates; }
    AppLock* security() const { return m_security; }
    OwnActionsView* savedActions() const { return m_savedActions; }
    void useSecurity(AppLock& security, OwnActionsView& actions)
    {
        m_security = &security;
        m_savedActions = &actions;
        emit changed();
    }
    void useUpdates(UpdatesView& updates)
    {
        m_updates = &updates;
        emit changed();
    }

    // The box is opened: how things are is asked anew, and what went wrong before is not said again.
    Q_INVOKABLE void lookAgain();

    // The sections the box lists where its window shows `wall`: "pc", this PC's own wall; "server",
    // a QKVM server's; "managed", a QKVM server's that whoever is signed in manages; anything
    // else, no wall (nobody is signed in yet). In their groups and in their order, each group as
    // {"group": its key, "sections": the keys of its sections}: "pc" (this PC's wall: "kvms",
    // "actions", "files"), only where the window shows that wall; "server" ("files": the server's
    // wall as a file), only where it shows a managed one; and "program" ("general", "video",
    // "alerts", "security", "updates", "about"), always. The rest of a server's settings stay on
    // that server's page.
    Q_INVOKABLE static QVariantList sectionsFor(const QString& wall);
    // The section the box opens at where its window shows `wall`: the one shown last, where it is
    // listed there; and otherwise the first that is.
    Q_INVOKABLE QString opensAt(const QString& wall) const;
    // A section is shown: the box opens there the next time, in this run of the program.
    Q_INVOKABLE void show(const QString& section);
    // The box was closed: the section it was left at is written down (core::Remembered), and the
    // box opens there after the program was started again too. Written then, once, and not at each
    // section that is shown on the way: the settings are one file, megabytes of it where the wall
    // is large. Nothing where it is what is written down already. (And as the program ends, with
    // a box open or not: ~SettingsView.)
    Q_INVOKABLE void closed();

signals:
    void incidentsAsked();
    // How video is decoded was chosen anew.
    void decodingChosen();
    // Said after every change that was asked for, also one that came to nothing: whatever shows
    // a tick shows what is so.
    void changed();
    void sectionChanged();

private:
    // One yes-or-no of what is remembered, chosen: written down, and said where it is new.
    void choose(bool core::Remembered::* which, bool chosen);

    // What the adapters are called, asked anew.
    void nameAdapters();

    core::AutoStart& m_start;
    core::Settings& m_settings;
    bool m_startsWithWindows = false;
    const QString m_cardAtStart;
    std::function<QString(engine::AdapterChoice)> m_names;
    QHash<QString, QString> m_adapterNames;
    std::function<bool(engine::AdapterChoice)> m_monitors;
    QHash<QString, bool> m_adapterMonitors;
    QString m_problem;
    QString m_section;
    AlertsView* m_alerts = nullptr;
    UpdatesView* m_updates = nullptr;
    AppLock* m_security = nullptr;
    OwnActionsView* m_savedActions = nullptr;
};

} // namespace qkvm::ui
