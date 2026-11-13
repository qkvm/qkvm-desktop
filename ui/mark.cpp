#include "mark.h"

#include <algorithm>

#include <QColor>
#include <QPainter>
#include <QPointF>
#include <QSvgRenderer>

namespace qkvm::ui {

namespace {

// The tray badge keeps its 16-unit grid independently of the shared SVG.
constexpr double grid = 16.0;
constexpr QColor ground{0x13, 0x18, 0x22};
// The page's colours for "no signal" and for what wants attention.
constexpr QColor warn{0xf5, 0x9e, 0x0b};
constexpr QColor bad{0xef, 0x44, 0x44};

} // namespace

QImage markImage(int size, MarkDot dot)
{
    const int side = std::max(1, size);
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    QSvgRenderer mark(QStringLiteral(":/qkvm/brand/mark.svg"));
    mark.render(&painter);
    painter.scale(side / grid, side / grid);

    if (dot != MarkDot::None) {
        // A dark rim separates the status dot from the brand mark.
        painter.setBrush(ground);
        painter.drawEllipse(QPointF(11.5, 11.5), 4.5, 4.5);
        painter.setBrush(dot == MarkDot::Bad ? bad : warn);
        painter.drawEllipse(QPointF(11.5, 11.5), 3.5, 3.5);
    }
    return image;
}

} // namespace qkvm::ui
