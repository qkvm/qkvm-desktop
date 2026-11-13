#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>

// How large a machine's picture is in its enlarged view, as the page offers it (app.js, SCALES): fitted
// into the view, or at a scale of the machine's own pixels, so many of this screen's to each of the
// machine's (100 %: one to one, whatever Windows' scaling). And where the pointer is in the picture at
// every one of them, which is what is sent to the machine.
namespace qkvm::ui {

// The choices, in the page's order: "fit", "50", "75", "100", "125", "140", "200".
QStringList pictureScales();
// The scale of a choice: 0 for "fit", 0.5 for "50" and so on; 0 for what is none of them.
double scaleOf(const QString& choice);
// How large the picture is drawn at a scale, in the view's own (logical) pixels, with `ratio` of the
// monitor's pixels to each of those: on whole pixels of the monitor. Nothing at "fit" (scale 0), or
// where the picture's size is not known.
QSizeF drawnAt(QSize picture, double scale, double ratio);
// Where the picture lies in a view of this size, in the view's own pixels. At "fit" (scale 0): as large
// as fits, in the middle (as the engine draws it). At a scale: as large as drawnAt says, in the middle
// where it is smaller than the view each way, and otherwise from the view's corner, moved by how far
// the view is scrolled (`scrolled`, from 0 to how much larger it is). Nothing where its size is not known.
QRectF pictureIn(QSizeF view, QSize picture, double scale, double ratio, QPointF scrolled);
// Where a place in the view is in the picture that lies there: each way 0.0 to 1.0 over it, and less or
// more beside it. (-1, -1) where there is no picture.
QPointF placeIn(QPointF at, const QRectF& picture);

} // namespace qkvm::ui
