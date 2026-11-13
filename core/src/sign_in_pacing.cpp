#include <qkvm/core/sign_in_pacing.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <qkvm/core/wiped.h>

#include "cng.h"

namespace qkvm::core {

// One that waits for what a sign-in that is out comes to (whenSettled): told once, whichever comes first.
struct SignInPacing::Waiter {
    QPointer<QObject> context;
    std::function<void()> then;
    bool told = false;

    void tell()
    {
        if (!std::exchange(told, true)) {
            then();
        }
    }
};

namespace {

// The roads a sign-in to a KVM of this PC's wall goes by: a device's own sign-in; and (a server's controller) the
// sessions it gives, which it may refuse after it signed this program in, and its own web page's sign-in, the way to
// its console (RedfishDevice).
constexpr std::array<const char*, 3> roads{"login", "session", "web"};

qint64 steadyMs()
{
    static const QElapsedTimer since = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return since.elapsed();
}

} // namespace

SignInPacing::SignInPacing(qsizetype most, std::function<qint64()> clockMs)
    : m_most(most), m_clockMs(clockMs ? std::move(clockMs) : std::function<qint64()>(steadyMs))
{}

SignInPacing& SignInPacing::program()
{
    static SignInPacing own;
    return own;
}

SignInPacing::Mark SignInPacing::what(QStringView road, const QUrl& address, QStringView user, QStringView password)
{
    const QString where = address.toString(QUrl::FullyEncoded);
    SecretBytes said = secretUtf8({road, where, user, password});
    QByteArray mark(32, Qt::Uninitialized);
    // (Windows' own SHA-256, at once: what it keeps of the bytes while it works is Windows' to write over. And Qt's
    // where Windows does not make one, which never comes about on a Windows this program runs on.)
    if (!cng::worked(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, said.data(), static_cast<ULONG>(said.size()),
                                reinterpret_cast<PUCHAR>(mark.data()), 32))) {
        mark = QCryptographicHash::hash(said.view(), QCryptographicHash::Sha256);
    }
    // (And where it goes and who signs in, beside it: what may be kept of it on disk, and what a KVM taken off the wall
    // is known by. Never the password.)
    Mark made(std::move(mark));
    made.road = road.toString();
    made.address = where;
    made.user = user.toString();
    return made;
}

qint64 SignInPacing::now() const
{
    return m_clockMs();
}

int SignInPacing::times(const Mark& what) const
{
    const auto found = m_known.constFind(what);
    return found == m_known.constEnd() ? 0 : found->kept.noes;
}

qint64 SignInPacing::waitLeftMs(const Mark& what) const
{
    const auto found = m_known.constFind(what);
    return found == m_known.constEnd() ? 0 : std::max<qint64>(0, found->kept.untilMs - now());
}

void SignInPacing::drop(const Mark& what)
{
    if (const auto found = m_known.find(what); found != m_known.end()) {
        m_order.erase(found->place);
        m_known.erase(found);
        touched();
    }
}

void SignInPacing::touched()
{
    if (m_changed) {
        m_changed();
    }
}

qint64 SignInPacing::keep(const Mark& what, int noes, int silent, qint64 waitMs, std::optional<quint64> out)
{
    // (Never sooner than what stands already: nothing here shortens a wait. And a write that is no verdict on the
    // sign-in that is out keeps it out: cleared, a password typed after it forgot its hold, and went beside it.)
    const qint64 at = now();
    qint64 standing = 0;
    quint64 wasOut = 0;
    if (const auto found = m_known.constFind(what); found != m_known.constEnd()) {
        standing = found->kept.untilMs;
        wasOut = found->kept.out;
    }
    drop(what);
    const qint64 until = std::max(at + waitMs, standing);
    m_order.push_back(what); // (the newest last)
    m_known.insert(what, Held{Kept{noes, silent, until, out.value_or(wasOut)}, std::prev(m_order.end())});
    makeRoom();
    settled(what);
    touched();
    return std::max(waitMs, standing - at);
}

void SignInPacing::makeRoom()
{
    if (m_known.size() <= m_most) {
        return;
    }
    // (Of the oldest first; a sign-in that is out, or a hold that stands, is passed over: from the outside review, F3.
    // Dropped, the oldest of those was a no forgotten before its time, or a sign-in on the wire that a second went
    // beside. And so is a no that still counts, whose hold has run out: its next no waits longer, five minutes and not
    // one, and a JetKVM shuts an address out after five; dropped, it began again at a minute, from the merged
    // review, 6.)
    const qint64 at = now();
    for (auto place = m_order.begin(); place != m_order.end() && m_known.size() > m_most;) {
        const auto found = m_known.constFind(*place);
        if (found == m_known.constEnd() || found->kept.out != 0 || found->kept.untilMs > at || found->kept.noes > 0) {
            ++place;
            continue;
        }
        const Mark gone = *place;
        place = m_order.erase(place);
        m_known.erase(found);
        settled(gone);
        touched();
    }
}

void SignInPacing::settled(const Mark& what)
{
    const auto found = m_waiting.find(what);
    if (found == m_waiting.end()) {
        return;
    }
    if (const auto held = m_known.constFind(what); held != m_known.constEnd() && held->kept.out != 0) {
        return; // (out still, or out anew)
    }
    const QList<std::shared_ptr<Waiter>> waiting = std::move(found.value());
    m_waiting.erase(found);
    for (const std::shared_ptr<Waiter>& waiter : waiting) {
        if (!waiter->told && !waiter->context.isNull()) {
            // (From the loop: whoever is told may send at once, and the one that settled it is in the middle of
            // hearing its answer.)
            QMetaObject::invokeMethod(waiter->context.data(), [waiter] { waiter->tell(); }, Qt::QueuedConnection);
        }
    }
}

bool SignInPacing::onlyOut(const Mark& what) const
{
    const auto found = m_known.constFind(what);
    return found != m_known.constEnd() && found->kept.out != 0 && found->kept.noes == 0 && found->kept.untilMs > now();
}

bool SignInPacing::stillOut(const Mark& what, const Sent& answering) const
{
    // (By its number: a password typed anew meanwhile forgot the counts it went out with, and it is that one all the
    // same.)
    const auto found = m_known.constFind(what);
    return found != m_known.constEnd() && found->kept.out != 0 && found->kept.out == answering.noted.out;
}

void SignInPacing::whenSettled(const Mark& what, QObject* context, qint64 atMostMs, std::function<void()> then)
{
    auto waiter = std::make_shared<Waiter>();
    waiter->context = context;
    waiter->then = std::move(then);
    QList<std::shared_ptr<Waiter>>& waiting = m_waiting[what];
    waiting.removeIf([](const std::shared_ptr<Waiter>& one) { return one->told || one->context.isNull(); });
    waiting.append(waiter);
    // (A precise timer: a coarse one may come a little early, to a hold that has not quite run out.)
    QTimer::singleShot(std::chrono::milliseconds(std::max<qint64>(0, atMostMs)), Qt::PreciseTimer, context,
                       [waiter] { waiter->tell(); });
    settled(what); // (where it is out no longer already: told at once, from the loop)
}

qint64 SignInPacing::quiet(int silent, const Steps& steps)
{
    const int doublings = std::min(std::max(silent, 1) - 1, 10);
    return std::min(steps.unansweredMs << doublings, steps.unansweredMs * 30);
}

qint64 SignInPacing::saidNo(const Mark& what, const Steps& steps)
{
    static constexpr std::array<qint64, 4> ladder{1, 5, 15, 60};
    const int noes = times(what) + 1;
    // (A verdict: what `sent` noted of this very sign-in is not a wait to be kept.)
    drop(what);
    const std::size_t step = std::min<std::size_t>(static_cast<std::size_t>(noes), ladder.size()) - 1;
    return keep(what, noes, 0, steps.refusedMs * ladder.at(step), 0);
}

SignInPacing::Sent SignInPacing::sent(const Mark& what, const Steps& steps)
{
    Sent mark;
    if (const auto found = m_known.constFind(what); found != m_known.constEnd()) {
        mark.before = found->kept;
    }
    const Kept was = mark.before.value_or(Kept{});
    // Out until what it comes to is told (the verdicts below), which its line's end tells: by the clock only twice as
    // long as it can be out, for a line that never ends. (Held only as one that came to nothing, ten seconds, a no that
    // came after twelve was not there yet when the hold ran out; held as long as it could be out, by the clock alone,
    // a waiter that woke as the hold ended sent while the first was still on its line, given up a moment later.)
    keep(what, was.noes, was.silent + 1, std::max(quiet(was.silent + 1, steps), 2 * steps.outMs), ++m_sent);
    mark.noted = m_known.value(what).kept;
    return mark;
}

void SignInPacing::putBack(const Mark& what, const Sent& mark)
{
    // Only where it is the one that is out still: two drivers of one KVM can both be signing in, and the one that hears
    // a no is not undone by the other that found no line to be had.
    if (!stillOut(what, mark)) {
        return;
    }
    // (Its password typed anew since, what was kept before it is forgotten too.)
    const bool forgotten = !(m_known.value(what).kept == mark.noted);
    drop(what);
    if (mark.before && !forgotten) {
        // (As it stood, but out no longer: a sign-in it held is over, and whoever waits is told.)
        Kept restored = *mark.before;
        restored.out = 0;
        m_order.push_back(what);
        m_known.insert(what, Held{restored, std::prev(m_order.end())});
        touched();
    }
    settled(what);
}

qint64 SignInPacing::unanswered(const Mark& what, const Steps& steps)
{
    Kept was;
    if (const auto found = m_known.constFind(what); found != m_known.constEnd()) {
        was = found->kept;
    }
    return keep(what, was.noes, std::max(was.silent, 1), quiet(was.silent, steps));
}

qint64 SignInPacing::unanswered(const Mark& what, const Steps& steps, const Sent& answering)
{
    if (!stillOut(what, answering)) {
        return unanswered(what, steps); // (something was noted since: it stands, and this is never sooner)
    }
    // What `sent` noted of this very sign-in, the time it might be out, is no wait to be kept: what stood before it
    // is, and the wait after one that came to nothing, from now. (Counted as it stands: where its password was typed
    // anew meanwhile, this one is the first that came to nothing.)
    const Kept was = m_known.value(what).kept;
    const int silent = std::max(was.silent, 1);
    putBack(what, answering);
    return keep(what, was.noes, silent, quiet(silent, steps), 0);
}

qint64 SignInPacing::cameToNothingAgain(const Mark& what, const Steps& steps)
{
    const Kept was = standing(what).value_or(Kept{});
    return keep(what, was.noes, was.silent + 1, quiet(was.silent + 1, steps));
}

int SignInPacing::silent(const Mark& what) const
{
    const auto found = m_known.constFind(what);
    return found == m_known.constEnd() ? 0 : found->kept.silent;
}

std::optional<SignInPacing::Kept> SignInPacing::standing(const Mark& what) const
{
    const auto found = m_known.constFind(what);
    return found == m_known.constEnd() ? std::nullopt : std::optional<Kept>(found->kept);
}

qint64 SignInPacing::later(const Mark& what, qint64 waitMs)
{
    // (Never sooner than what stands: `keep` shortens nothing. Another driver's no, an hour at the fourth, is not
    // cut to the minute a device asks of this one.)
    return keep(what, times(what), 0, waitMs);
}

qint64 SignInPacing::later(const Mark& what, qint64 waitMs, const Sent& answering)
{
    // (What `sent` noted of this very sign-in is not a wait to be kept: put back, where nothing was noted since.)
    putBack(what, answering);
    return later(what, waitMs);
}

void SignInPacing::accepted(const Mark& what)
{
    drop(what);
    settled(what);
}

void SignInPacing::forget(const QUrl& address, const QString& user, const QString& password)
{
    for (const char* road : roads) {
        const Mark mark = what(QString::fromLatin1(road), address, user, password);
        if (const auto found = m_known.find(mark); found != m_known.end() && found->kept.out != 0) {
            // A sign-in with it is out: its noes and what came to nothing are forgotten, and it holds still. Dropped,
            // the typed one went beside it: two strikes at a JetKVM from one save.
            found->kept.noes = 0;
            found->kept.silent = 0;
            touched();
            continue;
        }
        drop(mark);
        settled(mark);
    }
}

void SignInPacing::clear()
{
    m_known.clear();
    m_order.clear();
    for (const Mark& mark : m_waiting.keys()) {
        settled(mark);
    }
    touched();
}

QList<SignInPacing::Saved> SignInPacing::saved() const
{
    QList<Saved> all;
    all.reserve(static_cast<qsizetype>(m_order.size()));
    const qint64 at = now();
    for (const Mark& mark : m_order) {
        const Kept& kept = m_known.value(mark).kept;
        all.append(Saved{mark, kept.noes, kept.silent, std::max<qint64>(0, kept.untilMs - at)});
    }
    return all;
}

void SignInPacing::restore(const QList<Saved>& kept)
{
    const qint64 at = now();
    for (const Saved& one : kept) {
        if (one.mark.digest.isEmpty() || (one.noes <= 0 && one.silent <= 0 && one.leftMs <= 0)) {
            continue;
        }
        // (Never shorter than what stands, and nothing of it out: no line of the run before is there to end it.)
        Kept was;
        if (const auto found = m_known.constFind(one.mark); found != m_known.constEnd()) {
            was = found->kept;
        }
        const Kept taken{std::max(was.noes, std::max(0, one.noes)), std::max(was.silent, std::max(0, one.silent)),
                         std::max(was.untilMs, at + std::max<qint64>(0, one.leftMs)), was.out};
        if (const auto found = m_known.find(one.mark); found != m_known.end()) {
            found->kept = taken;
            continue;
        }
        m_order.push_back(one.mark);
        m_known.insert(one.mark, Held{taken, std::prev(m_order.end())});
    }
    makeRoom();
    touched();
}

void SignInPacing::forgetPlace(const QUrl& address)
{
    const QString where = address.toString(QUrl::FullyEncoded);
    QList<Mark> gone;
    for (const Mark& mark : m_order) {
        // (A sign-in that is out stays held: its line's end settles it, and nothing goes beside it meanwhile.)
        if (mark.address == where && m_known.value(mark).kept.out == 0) {
            gone.append(mark);
        }
    }
    for (const Mark& mark : gone) {
        drop(mark);
        settled(mark);
    }
}

void SignInPacing::whenChanged(std::function<void()> changed)
{
    m_changed = std::move(changed);
}

} // namespace qkvm::core
