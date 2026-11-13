#pragma once

#include <QList>

#include "wall_model.h"

namespace qkvm::ui {

// A wall of made-up machines, one for each thing a card can show: the same fifteen the live demo of
// the page has. For the tests of the screens and for the rig that measures the engine under Qt
// Quick; the program itself never shows them.
//
// The machines with a picture have the streams 1 to `pictured()`, in the wall's order: whoever
// shows this wall with pictures feeds those.
QList<WallTile> pretendWall();
int pretendWallPictured();

} // namespace qkvm::ui
