#include "workspaces_view.h"

#include <QUuid>
#include <algorithm>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

WorkspacesView::WorkspacesView(core::Settings& settings, QObject* parent) : QObject(parent), m_settings(settings) {}

void WorkspacesView::useAppLock(AppLock& lock)
{
    m_lock = &lock;
    connect(&lock, &AppLock::changed, this, &WorkspacesView::changed);
}

void WorkspacesView::useWindows(std::function<QList<core::WallWindow>()> capture,
                                std::function<bool(const QList<core::WallWindow>&)> restore)
{
    m_capture = std::move(capture);
    m_restore = std::move(restore);
}

QVariantList WorkspacesView::entries() const
{
    QVariantList rows;
    if (allowed()) {
        for (const auto& workspace : m_settings.remembered().workspaces)
            rows.append(QVariantMap{
                {u"value"_s, workspace.id}, {u"label"_s, workspace.name}, {u"windows"_s, workspace.windows.size()}});
    }
    return rows;
}

bool WorkspacesView::fail(const QString& text)
{
    m_problem = text;
    emit changed();
    return false;
}

bool WorkspacesView::validName(const QString& name, const QString& id)
{
    if (name.isEmpty() || name.size() > core::Remembered::longestWorkspaceName || !name.isValidUtf16())
        return fail(tr("Use a workspace name with 1 to 80 characters."));
    if (std::ranges::any_of(m_settings.remembered().workspaces, [&](const auto& saved) {
            return saved.id != id && saved.name.compare(name, Qt::CaseInsensitive) == 0;
        }))
        return fail(tr("A workspace already has this name."));
    return true;
}

bool WorkspacesView::keep(const core::Remembered& remembered)
{
    const auto saved = m_settings.remember(remembered);
    m_problem = saved ? QString() : tr("The change is available now, but could not be saved on this PC.");
    emit changed();
    return static_cast<bool>(saved);
}

bool WorkspacesView::save(const QString& name, const QString& replace)
{
    if (!allowed() || !m_capture)
        return false;
    const auto label = name.simplified();
    if (!validName(label, replace))
        return false;
    // Capture first: keepers may write their latest geometry while doing so.
    const auto windows = m_capture();
    if (windows.isEmpty() || windows.size() > core::Remembered::mostWindows)
        return fail(tr("Finish the current operation before saving a workspace."));
    auto saved = m_settings.remembered();
    if (replace.isEmpty()) {
        if (saved.workspaces.size() >= core::Remembered::mostWorkspaces)
            return fail(tr("You can save up to 16 workspaces. Remove one before adding another."));
        saved.workspaces.append({QUuid::createUuid().toString(QUuid::Id128), label, windows});
    } else {
        auto found = std::ranges::find(saved.workspaces, replace, &core::Workspace::id);
        if (found == saved.workspaces.end())
            return fail(tr("This workspace is no longer saved."));
        found->name = label;
        found->windows = windows;
    }
    return keep(saved);
}

bool WorkspacesView::rename(const QString& id, const QString& name)
{
    if (!allowed() || !validName(name.simplified(), id))
        return false;
    auto saved = m_settings.remembered();
    auto found = std::ranges::find(saved.workspaces, id, &core::Workspace::id);
    if (found == saved.workspaces.end())
        return fail(tr("This workspace is no longer saved."));
    found->name = name.simplified();
    return keep(saved);
}

bool WorkspacesView::remove(const QString& id)
{
    if (!allowed())
        return false;
    auto saved = m_settings.remembered();
    if (saved.workspaces.removeIf([&](const auto& workspace) { return workspace.id == id; }) == 0)
        return fail(tr("This workspace is no longer saved."));
    return keep(saved);
}

bool WorkspacesView::restore(const QString& id)
{
    if (!allowed() || !m_restore)
        return false;
    const auto saved = m_settings.remembered().workspaces;
    const auto found = std::ranges::find(saved, id, &core::Workspace::id);
    if (found == saved.end())
        return fail(tr("This workspace is no longer saved."));
    if (!m_restore(found->windows))
        return fail(tr(
            "The workspace could not be opened. Finish any active control, file or device operation and try again."));
    m_problem.clear();
    emit changed();
    return true;
}

} // namespace qkvm::ui
