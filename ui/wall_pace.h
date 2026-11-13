#pragma once

#include <QList>
#include <QObject>

#include <qkvm/core/settings.h>

#include "wall_view.h"

namespace qkvm::ui {

// How often a new picture of each machine is asked for is one choice for all of the program's
// walls, and is remembered on this PC: the pictures come from one server at one pace, in however
// many windows they are shown. A wall that is added is set to what holds; from then on, what is
// chosen on any of them holds on all: a pace, which is written down, or a pause, which is for now
// and is not.
//
// On the interface thread. The settings outlive it; a wall is let go of when it ends.
class WallPace : public QObject {
    Q_OBJECT

public:
    explicit WallPace(core::Settings& settings, QObject* parent = nullptr);

    // A wall of the program's, from now on until it ends. Once for each wall.
    void add(WallView& wall);
    void reload();

private:
    // Something was chosen on this one: the others follow, and a pace is written down.
    void follow(const WallView& chosenOn);

    core::Settings& m_settings;
    QList<WallView*> m_walls;
    // The others are following one: what that does to them is not a choice made on them.
    bool m_following = false;
};

} // namespace qkvm::ui
