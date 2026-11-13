#include "edit_kvm_view.h"

#include <utility>

#include <QPointer>

#include <qkvm/core/kvm_editing.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

EditKvmView::EditKvmView(QObject* parent) : QObject(parent)
{
    m_rest.setSingleShot(true);
    connect(&m_rest, &QTimer::timeout, this, [this] {
        m_rested = true;
        emit changed();
    });
}

void EditKvmView::setLookedAt(bool lookedAt)
{
    if (lookedAt == m_lookedAt) {
        return;
    }
    m_lookedAt = lookedAt;
    // Whatever the moment had come to: it runs while the two are looked at, and from the
    // beginning each time they are looked at again.
    restAnew();
    // (Said a turn of the loop later. The screens say this while they read what is shown here,
    // having just been told that it changed: told again in the middle of that, they would be
    // reading in a circle. What a yes is held to is as it is from this line on.)
    QMetaObject::invokeMethod(this, &EditKvmView::changed, Qt::QueuedConnection);
}

void EditKvmView::openFor(const QString& machine, bool review)
{
    if (m_busy || !m_mayChange) {
        return;
    }
    m_toRemove = false;
    emit openAsked(machine, review);
}

void EditKvmView::typeWatches(const QString& noSignal, const QString& unchanged)
{
    const auto minutes = [](const QString& typed) {
        bool whole = false;
        const qint64 said = typed.trimmed().toLongLong(&whole);
        return typed.trimmed().isEmpty() || !whole ? 0 : core::ScreenWatch::minutesFrom(said);
    };
    m_typedWatches = std::pair<int, int>(minutes(noSignal), minutes(unchanged));
}

void EditKvmView::setMayChange(bool mayChange)
{
    if (mayChange == m_mayChange) {
        return;
    }
    m_mayChange = mayChange;
    if (!mayChange) {
        close();
    }
}

void EditKvmView::openToRemove(const QString& machine)
{
    if (m_busy || !m_mayChange) {
        return;
    }
    // (Whoever opens the box shows the KVM in it while this is said, or not at all: a machine that
    // is not on the wall leaves nothing of the wish behind.)
    m_toRemove = true;
    emit openAsked(machine, false);
    m_toRemove = false;
}

void EditKvmView::save(const QString& name, const QString& address, const QString& user, const QString& password,
                       const QString& group)
{
    // (With no yes to anything: that is said by its own button, and passed on by its own way.)
    if (canAsk()) {
        emit saveAsked(m_shown.id, name, address, user, password, group, QByteArray(), false);
    }
}

void EditKvmView::saveKeepingBoth(const QString& name, const QString& address, const QString& user,
                                  const QString& password, const QString& group)
{
    if (canAsk() && m_mayKeepBoth) {
        emit saveAsked(m_shown.id, name, address, user, password, group, QByteArray(), true);
    }
}

void EditKvmView::remove()
{
    if (canAsk()) {
        emit removeAsked(m_shown.id);
    }
}

void EditKvmView::review()
{
    if (canAsk()) {
        emit reviewAsked(m_shown.id);
    }
}

void EditKvmView::trust()
{
    // Only what is on the screen is said yes to: with no certificate shown, there is none. And
    // not one that has only just come, or that nobody looks at.
    if (canAsk() && m_reviewed && mayTrust()) {
        saidYes();
        // (Whoever hears the wish may end this box's window there and then.)
        const QPointer<EditKvmView> here(this);
        emit trustAsked(m_shown.id, m_presentedSha256);
        if (here != nullptr) {
            m_passedOn.reset();
        }
    }
}

void EditKvmView::saveKnownByWhatIsShown(const QString& name, const QString& address, const QString& user,
                                         const QString& password, const QString& group)
{
    // The certificate is the one that is on the screen, and is this one's to hand on: the screens
    // say what was typed, and never which certificate.
    if (canAsk() && m_differs && mayTrust()) {
        saidYes();
        const QPointer<EditKvmView> here(this);
        emit saveAsked(m_shown.id, name, address, user, password, group, m_presentedSha256, false);
        if (here != nullptr) {
            m_passedOn.reset();
        }
    }
}

std::optional<EditKvmView::Offer> EditKvmView::offers() const
{
    if (m_shown.id.isEmpty() || !(m_reviewed || m_differs) || !m_presentedIsCalled.isEmpty()) {
        return std::nullopt;
    }
    return Offer{m_shown.id, m_presentedSha256, m_newAddress, m_onWindowsWord, m_plainHttpThere};
}

void EditKvmView::saidYes()
{
    // What it is said to is what stands there now, and is said with the wish that carries it.
    m_passedOn = offers();
    m_saidYes = true;
    // (And what stands there is what the last yes was given to, from here on.)
    if (m_differs && !m_plainHttpThere) {
        m_saidYesAt.insert(m_newAddress, SaidYes{m_presentedSha256, m_onWindowsWord});
    }
    m_yesWasOnWindowsWord = false;
    if (std::exchange(m_yesWasToAnother, false)) {
        emit changed();
    }
}

void EditKvmView::giveUp()
{
    if (m_busy) {
        emit giveUpAsked(false);
    }
}

void EditKvmView::closeReview()
{
    if (m_busy || !(m_reviewed || m_differs)) {
        return;
    }
    forgetTheReview();
    m_problem.clear();
    emit changed();
}

void EditKvmView::closed()
{
    // Closed over something that is being asked (the wall was left, the window is out of sight):
    // given up, and nobody is here to be told of it. (Whoever did it says here and now what it
    // had done already, for the window to tell: savedAsItClosed.)
    if (m_busy) {
        emit giveUpAsked(true);
    }
    if (m_shown == Shown{} && !m_forReview && !m_removesOnly && !m_busy && m_asked.isEmpty() && m_problem.isEmpty() &&
        !m_gone && !m_reviewed && !m_differs && m_savedAs.isEmpty()) {
        return;
    }
    m_shown = {};
    m_forReview = false;
    m_removesOnly = false;
    m_busy = false;
    m_asked.clear();
    m_problem.clear();
    m_gone = false;
    m_savedAs.clear();
    m_ontoHttps = false;
    forgetTheKvm();
    emit changed();
}

void EditKvmView::close()
{
    // The screens first, which empty what was typed and say that the box was closed; and then
    // the same once more for where there are none, which does nothing a second time.
    emit closeAsked();
    closed();
}

void EditKvmView::renamed(const QString& called, const QString& inASentence)
{
    if (m_shown.id.isEmpty() || (m_shown.called == called && m_shown.inASentence == inASentence)) {
        return;
    }
    m_shown.called = called;
    m_shown.inASentence = inASentence;
    emit changed();
}

void EditKvmView::wentAway()
{
    if (m_shown.id.isEmpty() || m_gone) {
        return;
    }
    m_gone = true;
    emit changed();
}

void EditKvmView::show(const Shown& kvm, bool forReview)
{
    if (m_busy) {
        return;
    }
    m_shown = kvm;
    m_typedWatches.reset();
    m_forReview = forReview;
    m_removesOnly = m_toRemove && !forReview;
    m_asked.clear();
    m_problem.clear();
    m_gone = false;
    m_savedAs.clear();
    m_ontoHttps = false;
    forgetTheKvm();
    emit changed();
    emit opened();
    if (forReview) {
        review();
    }
}

void EditKvmView::began(const QString& what)
{
    m_busy = true;
    m_asked = what;
    m_waitsForAnother = false;
    m_problem.clear();
    m_mayKeepBoth = false;
    m_savedAs.clear();
    m_ontoHttps = false;
    emit changed();
}

void EditKvmView::signInWaits()
{
    if (!m_busy || std::exchange(m_waitsForAnother, true)) {
        return;
    }
    emit changed();
    emit signInWaitsForAnEarlierOne();
}

void EditKvmView::refused(const QString& why, bool mayKeepBoth)
{
    if (!m_busy) {
        return;
    }
    m_busy = false;
    m_saidYes = false;
    // (Of a KVM that went meanwhile there is nothing to say but that it went: `gone`.)
    m_problem = m_gone ? QString() : why;
    m_mayKeepBoth = mayKeepBoth && !m_problem.isEmpty();
    // A yes to what is shown at a new address is for one saving. Refused, the box is back at its
    // lines, where it is said why; the next saving reads what is shown there anew.
    if (m_differs) {
        forgetTheReview();
    }
    emit changed();
}

void EditKvmView::refusedOver(const Shown& now, const QString& why)
{
    if (!m_busy) {
        return;
    }
    m_busy = false;
    m_problem = why;
    m_shown = now;
    forgetTheKvm();
    emit changed();
    emit filledAnew();
}

void EditKvmView::saved(const QString& name)
{
    over(u"saved"_s, name);
}

void EditKvmView::removed(const QString& name)
{
    over(u"removed"_s, name);
}

void EditKvmView::trusted(const QString& name)
{
    over(u"trusted"_s, name);
}

void EditKvmView::retype(const QString& address)
{
    // (Only of a box that is open for a KVM: what it asked has come back.)
    if (m_shown.id.isEmpty()) {
        return;
    }
    emit addressRetyped(address);
}

void EditKvmView::savedAndStays(const Shown& now, bool ontoHttps)
{
    if (!m_busy) {
        return;
    }
    m_busy = false;
    m_problem.clear();
    m_shown = now;
    m_savedAs = now.inASentence;
    m_ontoHttps = ontoHttps;
    forgetTheKvm();
    emit changed();
    emit filledAnew();
}

void EditKvmView::reviewed(const QByteArray& remembered, const QByteArray& presented, const QString& isCalled)
{
    if (!m_busy) {
        return;
    }
    // They come for a yes, which was to another than this: said with them.
    const bool another = std::exchange(m_saidYes, false) && m_reviewed && presented != m_presentedSha256;
    m_busy = false;
    m_problem.clear();
    m_reviewed = true;
    m_differs = false;
    m_newAddress.clear();
    m_onWindowsWord = false;
    m_presentedIsCalled = isCalled;
    m_yesWasToAnother = another;
    showTheTwo(remembered, presented);
    emit changed();
}

void EditKvmView::differing(const QByteArray& remembered, const QByteArray& presented, const QString& at,
                            bool onWindowsWord)
{
    if (!m_busy) {
        return;
    }
    // A yes was given for this address since the box was opened, and it was to another than
    // this: to another certificate, or to that certificate and not to Windows' word for it,
    // or the other way round. Said with them, whatever was asked in between: what answers
    // there may have shown the one to every question, and this one only to the sign-in.
    const auto last = m_saidYesAt.constFind(at);
    const bool another =
        last != m_saidYesAt.constEnd() && (presented != last->certificate || onWindowsWord != last->onWindowsWord);
    m_saidYes = false;
    m_busy = false;
    m_problem.clear();
    showAtTheNewAddress(at, onWindowsWord, false);
    m_yesWasToAnother = another;
    m_yesWasOnWindowsWord = another && last->onWindowsWord;
    showTheTwo(remembered, presented);
    emit changed();
}

void EditKvmView::givingUpHttps(const QByteArray& remembered, const QString& at)
{
    if (!m_busy) {
        return;
    }
    m_busy = false;
    m_saidYes = false;
    m_problem.clear();
    showAtTheNewAddress(at, false, true);
    // (Nothing is shown there that could be another than a moment ago.)
    m_yesWasToAnother = false;
    m_yesWasOnWindowsWord = false;
    showTheTwo(remembered, QByteArray());
    emit changed();
}

void EditKvmView::showAtTheNewAddress(const QString& at, bool onWindowsWord, bool plainHttp)
{
    m_reviewed = false;
    m_differs = true;
    m_newAddress = at;
    m_onWindowsWord = onWindowsWord;
    m_plainHttpThere = plainHttp;
    m_presentedIsCalled.clear();
}

void EditKvmView::gaveUp()
{
    if (!m_busy) {
        return;
    }
    m_busy = false;
    m_saidYes = false;
    m_problem.clear();
    emit changed();
}

void EditKvmView::savedAsItClosed(const QString& name, bool known, const QString& certificate, bool overPlainHttp)
{
    emit doneAsItClosed(u"saved"_s, name, known, certificate, overPlainHttp);
}

void EditKvmView::trustedAsItClosed(const QString& name)
{
    emit doneAsItClosed(u"trusted"_s, name, false, QString(), false);
}

void EditKvmView::showTheTwo(const QByteArray& remembered, const QByteArray& presented)
{
    m_rememberedSha256 = remembered;
    m_presentedSha256 = presented;
    m_remembered = core::kvm::fingerprint(remembered);
    m_presented = core::kvm::fingerprint(presented);
    // What a yes would be said to is another than a moment ago, or is there for the first time:
    // none is passed on before it has been looked at for a moment.
    restAnew();
}

void EditKvmView::restAnew()
{
    m_rested = false;
    m_rest.stop();
    if (m_lookedAt && (m_reviewed || m_differs)) {
        m_rest.start(m_yesWasToAnother ? trustRestsAnewMs : trustRestsMs);
    }
}

void EditKvmView::forgetTheReview()
{
    m_reviewed = false;
    m_differs = false;
    m_newAddress.clear();
    m_onWindowsWord = false;
    m_plainHttpThere = false;
    m_presentedIsCalled.clear();
    m_remembered.clear();
    m_presented.clear();
    m_rememberedSha256.clear();
    m_presentedSha256.clear();
    m_saidYes = false;
    m_yesWasToAnother = false;
    m_yesWasOnWindowsWord = false;
    m_rest.stop();
    m_rested = false;
}

void EditKvmView::forgetTheKvm()
{
    forgetTheReview();
    m_saidYesAt.clear();
}

void EditKvmView::over(const QString& what, const QString& name)
{
    if (!m_busy) {
        return;
    }
    m_busy = false;
    m_saidYes = false;
    m_problem.clear();
    emit changed();
    emit done(what, name);
}

} // namespace qkvm::ui
