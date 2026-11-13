#include "wall_pace.h"

namespace qkvm::ui {

WallPace::WallPace(core::Settings& settings, QObject* parent) : QObject(parent), m_settings(settings) {}

void WallPace::add(WallView& wall)
{
    if (m_walls.isEmpty()) {
        wall.setChosenPace(m_settings.remembered().wallPictureEveryMs);
    } else {
        wall.paceAs(*m_walls.constFirst());
    }
    m_walls.append(&wall);
    connect(&wall, &WallView::paceChanged, this, [this, &wall] { follow(wall); });
    connect(&wall, &WallView::chosenPaceChanged, this, [this, &wall] { follow(wall); });
    connect(&wall, &QObject::destroyed, this, [this, &wall] { m_walls.removeAll(&wall); });
}

void WallPace::follow(const WallView& chosenOn)
{
    if (m_following) {
        return;
    }
    m_following = true;
    for (WallView* wall : std::as_const(m_walls)) {
        if (wall != &chosenOn) {
            wall->paceAs(chosenOn);
        }
    }
    m_following = false;
    if (core::Remembered remembered = m_settings.remembered(); remembered.wallPictureEveryMs != chosenOn.chosenPace()) {
        remembered.wallPictureEveryMs = chosenOn.chosenPace();
        // A PC whose settings cannot be written has its pictures at the pace that was chosen all
        // the same, until the program ends.
        static_cast<void>(m_settings.remember(remembered));
    }
}

void WallPace::reload()
{
    m_following = true;
    for (WallView* wall : std::as_const(m_walls)) {
        wall->setChosenPace(m_settings.remembered().wallPictureEveryMs);
    }
    m_following = false;
}

} // namespace qkvm::ui
