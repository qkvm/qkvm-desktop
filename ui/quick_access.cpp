#include "quick_access.h"

#include <algorithm>

#include "alerts_view.h"
#include "app_lock.h"

namespace qkvm::ui {
QuickAccess::QuickAccess(core::Settings& settings, AlertsView& sources, AppLock& lock, QObject* parent)
    : QObject(parent), m_settings(settings), m_sources(sources), m_lock(lock)
{
    connect(&sources, &AlertsView::changed, this, &QuickAccess::refresh);
    connect(&lock, &AppLock::changed, this, &QuickAccess::refresh);
    refresh();
}

bool QuickAccess::allowed() const
{
    return !m_lock.restricted();
}

void QuickAccess::refresh()
{
    const auto choices = m_sources.availableMachines();
    if (choices != m_known || m_allowed != allowed()) {
        m_known = choices;
        m_allowed = allowed();
        emit changed();
    }
}

bool QuickAccess::known(const core::MachineReference& reference) const
{
    return allowed() &&
           std::ranges::any_of(m_known, [&reference](const auto& choice) { return choice.reference == reference; });
}

QList<MachineChoice> QuickAccess::resolve(const QList<core::MachineReference>& references) const
{
    QList<MachineChoice> result;
    if (!allowed())
        return result;
    for (const auto& reference : references) {
        const auto found =
            std::ranges::find_if(m_known, [&reference](const auto& choice) { return choice.reference == reference; });
        if (found != m_known.cend())
            result.append(*found);
    }
    return result;
}

QList<MachineChoice> QuickAccess::favorites() const
{
    return resolve(m_settings.remembered().favorites);
}
QList<MachineChoice> QuickAccess::recent() const
{
    return resolve(m_settings.remembered().recentMachines);
}
bool QuickAccess::isFavorite(const core::MachineReference& reference) const
{
    return known(reference) && m_settings.remembered().favorites.contains(reference);
}

void QuickAccess::save(const core::Remembered& remembered)
{
    if (remembered == m_settings.remembered())
        return;
    const auto saved = m_settings.remember(remembered);
    emit changed();
    if (!saved)
        emit problem(
            tr("Quick access changed for this run, but could not be saved. Check that settings can be written."));
}

void QuickAccess::toggleFavorite(const core::MachineReference& reference)
{
    if (!known(reference))
        return;
    auto remembered = m_settings.remembered();
    if (!remembered.favorites.removeAll(reference)) {
        if (remembered.favorites.size() >= core::Remembered::mostFavorites) {
            emit problem(tr("You can keep up to %1 favorites. Remove a favorite before adding another.")
                             .arg(core::Remembered::mostFavorites));
            return;
        }
        remembered.favorites.append(reference);
    }
    save(remembered);
}

void QuickAccess::opened(const core::MachineReference& reference)
{
    if (!known(reference))
        return;
    auto remembered = m_settings.remembered();
    remembered.recentMachines.removeAll(reference);
    remembered.recentMachines.prepend(reference);
    remembered.recentMachines = remembered.recentMachines.first(
        std::min(remembered.recentMachines.size(), core::Remembered::mostRecentMachines));
    save(remembered);
}

void QuickAccess::clearRecent()
{
    if (!allowed())
        return;
    auto remembered = m_settings.remembered();
    remembered.recentMachines.clear();
    save(remembered);
}

void QuickAccess::clearFavorites()
{
    if (!allowed())
        return;
    auto remembered = m_settings.remembered();
    remembered.favorites.clear();
    save(remembered);
}
} // namespace qkvm::ui
