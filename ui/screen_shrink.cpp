#include "screen_shrink.h"

#include <QImage>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/screen_watch.h>

namespace qkvm::ui {

std::optional<QByteArray> shrunkScreen(const QByteArray& jpeg)
{
    // A JPEG is compared, by what it says of its size before a pixel of it is decoded.
    const std::optional<QSize> size = core::kvmd::pictureSize(jpeg);
    if (!jpeg.startsWith("\xff\xd8") || !size || qint64{size->width()} * size->height() <= 0 ||
        qint64{size->width()} * size->height() > core::ScreenWatch::mostPixels) {
        return std::nullopt;
    }
    const QImage picture = QImage::fromData(jpeg, "JPEG");
    if (picture.isNull() || picture.size() != *size) {
        return std::nullopt;
    }
    const QImage rgb = picture.convertToFormat(QImage::Format_RGB888);
    return core::ScreenWatch::fingerprint(rgb.constBits(), rgb.width(), rgb.height(), rgb.bytesPerLine());
}

} // namespace qkvm::ui
