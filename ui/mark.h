#pragma once

#include <QImage>

namespace qkvm::ui {

// What the program's mark says beside itself where it stands for a whole wall: nothing, that
// something wants looking at, or that something is wrong.
enum class MarkDot { None, Warn, Bad };

// The official qKVM SVG, rendered at the monitor's icon size, with an optional tray status badge.
// The interface and Windows icons share the same bundled mark.
QImage markImage(int size, MarkDot dot = MarkDot::None);

} // namespace qkvm::ui
