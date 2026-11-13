#include "add_kvm_view.h"

#include <utility>

namespace qkvm::ui {

AddKvmView::AddKvmView(QObject* parent) : QObject(parent) {}

void AddKvmView::add(const QString& address, const QString& user, const QString& password, const QString& group)
{
    if (m_busy) {
        return;
    }
    emit addAsked(address, user, password, group, false);
}

void AddKvmView::addKeepingBoth(const QString& address, const QString& user, const QString& password,
                                const QString& group)
{
    if (m_busy || !m_mayKeepBoth) {
        return;
    }
    emit addAsked(address, user, password, group, true);
}

void AddKvmView::giveUp()
{
    if (m_busy) {
        emit giveUpAsked();
    }
}

void AddKvmView::reset()
{
    if (m_problem.isEmpty() && m_added.isEmpty() && m_certificate.isEmpty() && m_count == 0) {
        return;
    }
    m_problem.clear();
    m_mayKeepBoth = false;
    m_added.clear();
    m_certificate.clear();
    m_count = 0;
    emit changed();
}

void AddKvmView::closed()
{
    // Over a KVM that is being asked: given up, once; and what comes of it is not the box's to
    // say any more.
    if (m_busy && !m_closedOver) {
        m_closedOver = true;
        emit giveUpAsked();
    }
    reset();
}

void AddKvmView::close()
{
    // The screens first, which empty what was typed and say that the box was closed; and then
    // the same once more for where there are none, which does nothing a second time.
    emit closeAsked();
    closed();
}

void AddKvmView::began()
{
    m_busy = true;
    m_closedOver = false;
    m_problem.clear();
    m_mayKeepBoth = false;
    emit changed();
}

void AddKvmView::refused(const QString& why, bool mayKeepBoth)
{
    m_busy = false;
    // (Of a box that was closed meanwhile nothing is complained: it asks nothing any more.)
    m_problem = std::exchange(m_closedOver, false) ? QString() : why;
    m_mayKeepBoth = mayKeepBoth && !m_problem.isEmpty();
    emit changed();
}

void AddKvmView::took(const QString& said, const QString& certificate)
{
    m_busy = false;
    m_problem.clear();
    if (std::exchange(m_closedOver, false)) {
        // Added, in the very turn in which the box was closed over it: the box is closed and
        // says nothing, so its window says it.
        emit changed();
        emit addedAsItClosed(said, certificate);
        return;
    }
    m_added = said;
    m_certificate = certificate;
    ++m_count;
    emit changed();
}

void AddKvmView::gaveUp()
{
    m_busy = false;
    m_closedOver = false;
    m_problem.clear();
    emit changed();
}

} // namespace qkvm::ui
