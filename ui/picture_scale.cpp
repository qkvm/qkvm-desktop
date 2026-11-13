#include "picture_scale.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

QStringList pictureScales()
{
    return {u"fit"_s, u"50"_s, u"75"_s, u"100"_s, u"125"_s, u"140"_s, u"200"_s};
}

double scaleOf(const QString& choice)
{
    if (choice == u"fit"_s || !pictureScales().contains(choice)) {
        return 0;
    }
    return choice.toDouble() / 100;
}

QSizeF drawnAt(QSize picture, double scale, double ratio)
{
    if (scale <= 0 || ratio <= 0 || picture.width() <= 0 || picture.height() <= 0) {
        return {};
    }
    // (On whole pixels of the monitor, as the page's canvas is: a picture between two is blurred.)
    const double width = std::max(1.0, std::round(picture.width() * scale));
    const double height = std::max(1.0, std::round(picture.height() * scale));
    return {width / ratio, height / ratio};
}

QRectF pictureIn(QSizeF view, QSize picture, double scale, double ratio, QPointF scrolled)
{
    if (view.width() <= 0 || view.height() <= 0 || picture.width() <= 0 || picture.height() <= 0) {
        return {};
    }
    if (scale <= 0) {
        const double fits = std::min(view.width() / picture.width(), view.height() / picture.height());
        const QSizeF size(picture.width() * fits, picture.height() * fits);
        return {(view.width() - size.width()) / 2, (view.height() - size.height()) / 2, size.width(), size.height()};
    }
    const QSizeF size = drawnAt(picture, scale, ratio);
    if (size.isEmpty()) {
        return {};
    }
    // Each way: in the middle where it is smaller, scrolled where it is larger (and never beyond its ends).
    const auto along = [](double room, double length, double moved) {
        return length <= room ? (room - length) / 2 : -std::clamp(moved, 0.0, length - room);
    };
    return {along(view.width(), size.width(), scrolled.x()), along(view.height(), size.height(), scrolled.y()),
            size.width(), size.height()};
}

QPointF placeIn(QPointF at, const QRectF& picture)
{
    if (picture.width() <= 0 || picture.height() <= 0) {
        return {-1, -1};
    }
    return {(at.x() - picture.left()) / picture.width(), (at.y() - picture.top()) / picture.height()};
}

} // namespace qkvm::ui
