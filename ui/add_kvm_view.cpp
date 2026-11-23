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

void AddKvmView::listGuests(const QString& address, const QString& tokenId, const QString& secret, const QString& group)
{
    if (m_busy) {
        return;
    }
    emit listGuestsAsked(address, tokenId, secret, group);
}

void AddKvmView::addGuests(const QString& address, const QString& tokenId, const QString& secret, const QString& group,
                           const QVariantList& vmids)
{
    if (m_busy || vmids.isEmpty()) {
        return;
    }
    QList<qint64> numbers;
    for (const QVariant& vmid : vmids) {
        numbers.append(vmid.toLongLong());
    }
    emit addGuestsAsked(address, tokenId, secret, group, numbers);
}

void AddKvmView::listed(const QVariantList& guests, int containers)
{
    m_busy = false;
    m_problem.clear();
    m_guests = guests;
    m_containers = containers;
    m_listed = true;
    emit changed();
}

void AddKvmView::beganARun()
{
    m_added.clear();
    m_certificate.clear();
    began();
}

void AddKvmView::tookGuest(const QString& said, const QString& certificate)
{
    m_added = m_added.isEmpty() ? said : m_added + u'\n' + said;
    m_certificate = certificate;
    ++m_count;
    emit changed();
}

void AddKvmView::refusedGuest(const QString& why)
{
    m_problem = m_problem.isEmpty() ? why : m_problem + u'\n' + why;
    emit changed();
}

void AddKvmView::guestsDone()
{
    m_busy = false;
    emit changed();
}

void AddKvmView::giveUp()
{
    if (m_busy) {
        emit giveUpAsked();
    }
}

void AddKvmView::reset()
{
    if (m_problem.isEmpty() && m_added.isEmpty() && m_certificate.isEmpty() && m_count == 0 && m_guests.isEmpty() &&
        !m_listed) {
        return;
    }
    m_problem.clear();
    m_mayKeepBoth = false;
    m_useInstead.clear();
    m_added.clear();
    m_certificate.clear();
    m_count = 0;
    m_guests.clear();
    m_containers = 0;
    m_listed = false;
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
    m_useInstead.clear();
    emit changed();
}

void AddKvmView::refused(const QString& why, bool mayKeepBoth, const QString& useInstead)
{
    m_busy = false;
    // (Of a box that was closed meanwhile nothing is complained: it asks nothing any more.)
    m_problem = std::exchange(m_closedOver, false) ? QString() : why;
    m_mayKeepBoth = mayKeepBoth && !m_problem.isEmpty();
    m_useInstead = m_problem.isEmpty() ? QString() : useInstead;
    emit changed();
}

void AddKvmView::took(const QString& said, const QString& certificate)
{
    m_busy = false;
    m_problem.clear();
    m_useInstead.clear();
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
    m_useInstead.clear();
    emit changed();
}

} // namespace qkvm::ui
