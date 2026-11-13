#pragma once

#include <QObject>

#include <qkvm/core/settings.h>

#include "wall_view.h"

namespace qkvm::ui {

// How a window shows the wall is remembered on this PC, for that window: which group, in which
// order, with how many columns, and whether the groups take turns while the wall is only shown.
// When this is made, the wall is set to what was remembered; from then on every change of one of
// them is written down. (Which state is shown, what was typed into "Find a KVM", and a group that
// is shown because its turn came, are for the moment, and are not. How often a new picture is
// asked for is one choice for all the windows: WallPace.)
//
// On the interface thread. The wall, and its window's part of what is remembered, outlive it.
class WallChoices : public QObject {
    Q_OBJECT

public:
    WallChoices(WallView& wall, core::WindowSlot& kept, QObject* parent = nullptr);
    void restore(const core::WallWindow& remembered);

private:
    void remember();

    WallView& m_wall;
    core::WindowSlot& m_kept;
    // The group that was chosen, which is not always the one that is shown.
    QString m_group;
    bool m_restoring = false;
};

} // namespace qkvm::ui
