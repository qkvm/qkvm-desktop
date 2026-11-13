#include "own_actions_view.h"
#include "machine_menu.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {
OwnActionsView::OwnActionsView(core::OwnActions& actions, AppLock& lock, QObject* parent)
    : QObject(parent), m_actions(actions), m_lock(lock)
{
    connect(&actions, &core::OwnActions::changed, this, &OwnActionsView::changed);
    connect(&lock, &AppLock::changed, this, &OwnActionsView::changed);
}

QVariantList OwnActionsView::entries() const
{
    QVariantList result;
    for (const auto& action : m_actions.handedOut()) {
        result.append(QVariantMap{{u"id"_s, action.id},
                                  {u"revision"_s, action.changed},
                                  {u"name"_s, action.name},
                                  {u"types"_s, action.types},
                                  {u"keys"_s, action.keys},
                                  {u"keysLabel"_s, keysLabel(action.keys)},
                                  {u"enter"_s, action.enter},
                                  {u"danger"_s, action.danger}});
    }
    return result;
}

QString OwnActionsView::save(const QString& id, int revision, const QString& name, bool types, const QString& keys,
                             const QString& text, bool enter, bool danger)
{
    if (!allowed()) {
        return tr("Unlock qKVM and sign out of wall-screen mode before changing saved actions.");
    }
    core::OwnActions::Typed typed{name, types, keys, text, enter, danger};
    const core::WipedOnLeaving wipe(typed.text);
    const auto saved = id.isEmpty() ? m_actions.add(typed) : m_actions.change(id, revision, typed);
    return saved ? QString() : QString::fromStdString(saved.error().message);
}

QString OwnActionsView::remove(const QString& id, int revision)
{
    if (!allowed()) {
        return tr("Unlock qKVM and sign out of wall-screen mode before changing saved actions.");
    }
    const auto removed = m_actions.remove(id, revision);
    return removed ? QString() : QString::fromStdString(removed.error().message);
}
} // namespace qkvm::ui
