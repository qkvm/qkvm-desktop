#pragma once

#include <QObject>
#include <QTimer>

#include <qkvm/core/settings.h>

#include "session_view.h"
#include "tray.h"
#include "wall_news.h"
#include "wall_view.h"

namespace qkvm::ui {

// Tells whoever is not looking at the wall what became of it, through the program's icon, as a
// message of Windows': a machine went away or came back, QKVM stopped answering or answers again.
//
// Only while nobody can see the wall (its window is minimized, or out of sight behind the icon):
// whoever looks at the wall sees it there. Only where that was not switched off in the settings.
// And never at once: what happens is gathered for a moment, so that a switch that takes twelve
// machines with it is one message and a machine that is gone for a second is none.
//
// On the interface thread. What it is given outlives it.
class WallTeller : public QObject {
    Q_OBJECT

public:
    // `gatherMs`: for how long news is gathered before it is told. A test waits less.
    WallTeller(TrayIcon& tray, SessionView& session, WallView& wall, core::Settings& settings, int gatherMs = 4000,
               QObject* parent = nullptr);

private:
    // The wall's machines are not what they were: what is news of that is put to the rest.
    void lookAtTheWall();
    // Who is signed in, or whether QKVM answers, is not what it was.
    void lookAtTheSession();
    // What was gathered is told, or dropped where nobody is to be told.
    void tell();

    TrayIcon& m_tray;
    SessionView& m_session;
    WallView& m_wall;
    core::Settings& m_settings;
    // Somebody is signed in, and was at the last look: the wall is a QKVM's.
    bool m_atTheWall = false;
    // What is known of the wall's machines; nothing before the first look, and after the wall
    // was left.
    WallLook m_look;
    // What nobody was told yet.
    WallNews m_news;
    // Whether QKVM answered at the last look; and whether somebody was told that it does not,
    // who is then told when it does again.
    bool m_answered = true;
    bool m_toldSilent = false;
    QTimer m_gather;
};

} // namespace qkvm::ui
