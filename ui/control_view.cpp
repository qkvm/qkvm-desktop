#include "control_view.h"

#include <algorithm>

#include <QCoreApplication>

#include <qkvm/core/exchange.h>
#include <qkvm/core/settings.h>
#include <qkvm/input/keys.h>

#include "picture_scale.h"

namespace qkvm::ui {

QPointF inPicture(QPointF at, QSizeF tile, QSize picture)
{
    if (picture.width() <= 0 || picture.height() <= 0 || tile.width() <= 0 || tile.height() <= 0) {
        // (Over no picture at all.)
        return {-1, -1};
    }
    const double scale = std::min(tile.width() / picture.width(), tile.height() / picture.height());
    const double width = picture.width() * scale;
    const double height = picture.height() * scale;
    return {(at.x() - (tile.width() - width) / 2) / width, (at.y() - (tile.height() - height) / 2) / height};
}

ControlView::ControlView(QObject* parent) : QObject(parent) {}

QString ControlView::leaveKeyName() const
{
    if (m_releaseKey == QStringLiteral("f12")) {
        return QCoreApplication::translate("Control", "F12");
    }
    if (m_releaseKey == QStringLiteral("scroll-lock")) {
        return QCoreApplication::translate("Control", "Scroll Lock");
    }
    return QCoreApplication::translate("Control", "Right Ctrl");
}

QString ControlView::inASentence(const QString& name)
{
    const QString setOff = core::exchange::nameSetOff(name);
    return setOff.isEmpty() ? QCoreApplication::translate("Control", "this machine") : setOff;
}

std::uint16_t ControlView::leaveKey() const
{
    return m_releaseKey == QStringLiteral("f12")           ? 0x45
           : m_releaseKey == QStringLiteral("scroll-lock") ? 0x47
                                                           : input::usageRightCtrl;
}

void ControlView::setReleaseKey(const QString& key)
{
    if (key == m_releaseKey || !core::Remembered::namesAReleaseKey(key)) {
        return;
    }
    // End the old capture before the new key is advertised, including a pending taking.
    emit giveBackAsked();
    m_releaseKey = key;
    emit releaseKeyChanged();
}

void ControlView::take()
{
    if (m_shown.takeoverPossible)
        emit takeoverAsked();
    else
        emit takeAsked();
}

void ControlView::answerTakeover(bool yes)
{
    emit takeoverAnswered(yes);
}

void ControlView::giveBack()
{
    emit giveBackAsked();
}

void ControlView::ctrlAltDel()
{
    emit ctrlAltDelAsked();
}

void ControlView::setLocalInput(bool local)
{
    emit localInputAsked(local);
}

void ControlView::tapKeys(const QList<int>& usages)
{
    emit keysTapped(usages);
}

void ControlView::wentOn(bool byAKey)
{
    emit wentOnSaid(byAKey);
}

void ControlView::pointerAt(qreal x, qreal y, qreal width, qreal height)
{
    const QPointF place = inPicture({x, y}, {width, height}, m_shown.picture);
    emit pointerMoved(place.x(), place.y());
}

void ControlView::pointerOver(qreal x, qreal y, qreal left, qreal top, qreal width, qreal height)
{
    const QPointF place = placeIn({x, y}, QRectF(left, top, width, height));
    emit pointerMoved(place.x(), place.y());
}

void ControlView::button(int button, bool down)
{
    switch (button) {
    case Qt::LeftButton:
        emit buttonChanged(1, down);
        break;
    case Qt::RightButton:
        emit buttonChanged(2, down);
        break;
    case Qt::MiddleButton:
        emit buttonChanged(3, down);
        break;
    case Qt::BackButton:
        emit buttonChanged(4, down);
        break;
    case Qt::ForwardButton:
        emit buttonChanged(5, down);
        break;
    default:
        break;
    }
}

void ControlView::wheel(int angleX, int angleY)
{
    emit wheelTurned(angleX, angleY);
}

void ControlView::show(const Shown& shown)
{
    if (shown == m_shown) {
        return;
    }
    m_shown = shown;
    emit changed();
}

void ControlView::setRelative(bool relative)
{
    emit relativeAsked(relative);
}

void ControlView::pointerBy(int dx, int dy)
{
    emit pointerShifted(dx, dy);
}

void ControlView::captureReady()
{
    emit captureBegan();
}

void ControlView::captureLost()
{
    emit captureEnded();
}

} // namespace qkvm::ui
