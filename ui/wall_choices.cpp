#include "wall_choices.h"

namespace qkvm::ui {

WallChoices::WallChoices(WallView& wall, core::WindowSlot& kept, QObject* parent)
    : QObject(parent), m_wall(wall), m_kept(kept)
{
    const core::WallWindow remembered = m_kept.remembered();
    m_group = remembered.group;
    m_wall.setRotating(remembered.rotates);
    m_wall.shown()->setGroup(remembered.group);
    m_wall.shown()->setOrder(remembered.order);
    m_wall.setColumns(remembered.columns);

    connect(m_wall.shown(), &WallFilter::groupChanged, this, [this] {
        if (!m_wall.isTurningNow()) {
            m_group = m_wall.shown()->group();
            remember();
        }
    });
    connect(&m_wall, &WallView::rotatingChanged, this, &WallChoices::remember);
    connect(m_wall.shown(), &WallFilter::orderChanged, this, &WallChoices::remember);
    connect(&m_wall, &WallView::columnsChanged, this, &WallChoices::remember);
}

void WallChoices::remember()
{
    if (m_restoring)
        return;
    core::WallWindow remembered = m_kept.remembered();
    remembered.group = m_group;
    remembered.rotates = m_wall.rotating();
    remembered.order = m_wall.shown()->order();
    remembered.columns = m_wall.columns();
    // A PC whose settings cannot be written shows its wall all the same, as it was chosen, until
    // the program ends.
    static_cast<void>(m_kept.remember(remembered));
}

void WallChoices::restore(const core::WallWindow& remembered)
{
    m_restoring = true;
    m_wall.stopArranging();
    m_wall.closeEnlarged();
    m_wall.setSelecting(false);
    m_wall.setDisplaying(false);
    m_wall.shown()->setFind({});
    m_wall.shown()->setShow({});
    m_group = remembered.group;
    m_wall.shown()->setGroup(remembered.group);
    m_wall.shown()->setOrder(remembered.order);
    m_wall.setColumns(remembered.columns);
    m_wall.setRotating(remembered.rotates);
    m_restoring = false;
    remember();
}

} // namespace qkvm::ui
