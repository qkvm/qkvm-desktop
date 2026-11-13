#include "app_lock.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtEndian>

#include <qkvm/core/pacing_file.h>
#include <qkvm/core/scrypt.h>
#include <qkvm/core/sealing.h>
#include <qkvm/core/settings.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {
namespace {
// Credential Manager's text editor and cmdkey write UTF-16: always an even byte count.
// This strictly framed, odd-sized binary record cannot be supplied through either tool.
// It is not encryption against another program running as the same Windows user.
constexpr QByteArrayView recordMagic("\0QKVM\1", 6);
QByteArray recordBytes(const QJsonObject& record)
{
    QByteArray json = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const core::WipedOnLeaving wipe(json);
    QByteArray bytes(recordMagic.data(), recordMagic.size());
    bytes.resize(10);
    qToBigEndian<quint32>(static_cast<quint32>(json.size()), bytes.data() + 6);
    bytes.append(json);
    bytes.append(json.size() % 2 == 0 ? 1 : 2, '\0');
    return bytes;
}
QJsonObject readRecord(const QByteArray& bytes)
{
    if (bytes.size() < 13 || bytes.size() % 2 != 1 || !bytes.startsWith(recordMagic)) {
        return {};
    }
    const qsizetype length = qFromBigEndian<quint32>(bytes.constData() + 6);
    const qsizetype padding = length % 2 == 0 ? 1 : 2;
    if (length > core::largestSecretBytes || bytes.size() != 10 + length + padding ||
        bytes.last(padding) != QByteArray(padding, '\0')) {
        return {};
    }
    return QJsonDocument::fromJson(QByteArray::fromRawData(bytes.constData() + 10, length)).object();
}

QString setupMarkerName()
{
    return u"app password established"_s;
}

bool same(QByteArrayView left, QByteArrayView right)
{
    if (left.size() != 32 || right.size() != 32) {
        return false;
    }
    unsigned int difference = 0;
    for (qsizetype index = 0; index < 32; ++index) {
        difference |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
    }
    return difference == 0;
}

bool passwordFits(const QString& text)
{
    return text.isValidUtf16() && text.toUcs4().size() >= 10 && text.toUcs4().size() <= 256;
}

// What the program keeps in the store for itself, with nobody's password in it: the key its wrong-password counts are
// sealed under on disk (PacingFile), which is made at every start before the lock looks (main.cpp). No sign of a
// program used before, and so never taken for a password record that is missing; a reset erases it with the rest.
bool theProgramsOwn(const QString& name)
{
    return name.compare(QLatin1StringView(core::PacingFile::keyName), Qt::CaseInsensitive) == 0;
}
} // namespace

QString AppLock::recordName()
{
    return u"app password"_s;
}

AppLock::AppLock(core::SecretStore& secrets, const QString& lockFile, QObject* parent)
    : AppLock(
          secrets, derivePassword, [] { return QDateTime::currentSecsSinceEpoch(); }, parent, lockFile,
          [] {
              return static_cast<qint64>(
                  std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
                      .count());
          })
{}

AppLock::AppLock(core::SecretStore& secrets, Derive derive, std::function<qint64()> now, QObject* parent,
                 QString lockFile, std::function<qint64()> steady)
    : QObject(parent), m_secrets(secrets), m_lockFile(std::move(lockFile)), m_derive(std::move(derive)),
      m_now(std::move(now))
{
    m_steady = steady ? std::move(steady) : m_now;
    load();
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        // (Looked at every second: a clock that went back is seen within a second.)
        if (retryAfter() > 0 || m_next != 0) {
            emit changed();
        }
    });
    m_tick.start();
}

AppLock::~AppLock()
{
    m_worker.request_stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void AppLock::load()
{
    const auto names = m_secrets.names();
    // A list Windows does not hand over says nothing about what is kept (M-D1).
    if (!names) {
        becomeUnavailable();
        return;
    }
    m_passwordSaved = names.value().contains(recordName(), Qt::CaseInsensitive);
    m_marked = names.value().contains(setupMarkerName(), Qt::CaseInsensitive);
    const bool latched = !m_lockFile.isEmpty() && QFileInfo::exists(m_lockFile);
    QByteArray lockState;
    if (latched) {
        QFile marker(m_lockFile);
        if (marker.open(QIODevice::ReadOnly)) {
            lockState = marker.read(32);
        }
    }
    // Window placement and update checks are written during first setup. Only actual
    // inventory or incident history is prior-use evidence; any credential still blocks setup.
    // A completed explicit reset permits retaining that inventory with an empty prefix.
    bool priorProfile = false;
    if (m_lockFile.endsWith(u".locked")) {
        const QFileInfo profile(m_lockFile.chopped(7));
        const QStringList remnants{profile.fileName(), profile.fileName() + u".*"_s,
                                   profile.completeBaseName() + u".unreadable*."_s + profile.suffix()};
        for (const QString& name :
             profile.dir().entryList(remnants, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
            if (name.startsWith(profile.fileName() + u".incidents"_s)) {
                priorProfile = true;
                break;
            }
            const core::Settings settings(profile.dir().filePath(name), {1, 0});
            const auto& kept = settings.remembered();
            // (A file that holds KVMs this build does not read held a wall all the same.)
            if (settings.heldKvmsItDoesNotRead() || !kept.kvms.isEmpty() || !kept.actions.isEmpty() ||
                !kept.server.isEmpty() || !kept.servers.isEmpty()) {
                priorProfile = true;
                break;
            }
        }
    }
    const bool resetComplete = lockState == "setup\n";
    if (std::ranges::all_of(names.value(), theProgramsOwn) && ((!latched && !priorProfile) || resetComplete)) {
        m_setup = true;
        m_mode = u"setup"_s;
        return;
    }
    QByteArray bytes;
    const core::WipedOnLeaving wipe(bytes);
    if (m_passwordSaved) {
        auto read = m_secrets.read(recordName());
        // Listed, and not handed over (refused, or gone since the list): asked again, not damage.
        if (!read || !read.value()) {
            becomeUnavailable();
            return;
        }
        bytes = std::move(*read.value());
    }
    m_record = readRecord(bytes);
    const QByteArray salt = QByteArray::fromBase64(m_record.value(u"salt"_s).toString().toLatin1());
    const QByteArray verifier = QByteArray::fromBase64(m_record.value(u"verifier"_s).toString().toLatin1());
    const QJsonValue failures = m_record.value(u"failures"_s);
    const QJsonValue next = m_record.value(u"next"_s);
    // When the last wrong try was: a record written before it was kept has none.
    const QJsonValue last = m_record.value(u"last"_s);
    const bool lastFits = last.isUndefined() || (last.isDouble() && last.toInteger(-1) >= 0);
    const bool resetting = lockState == "reset\n" || m_record.value(u"reset"_s).toBool();
    if (!m_passwordSaved || m_record.value(u"v"_s).toInt() != 1 || resetting || salt.size() != 16 ||
        verifier.size() != 32 || !m_record.value(u"automatic"_s).isBool() || !m_record.value(u"locked"_s).isBool() ||
        !m_record.value(u"screen"_s).isBool() || !failures.isDouble() || failures.toInt(-1) < 0 ||
        failures.toInt(-1) > 30 || !next.isDouble() || next.toInteger(-1) < 0 || !lastFits) {
        m_recovery = true;
        m_mode = u"recovery"_s;
        m_problem = resetting
                        ? tr("A saved-secret reset is unfinished. Finish the reset to continue.")
                        : tr("The app password record is missing or cannot be read. Reset saved secrets to continue.");
        return;
    }
    m_automatic = m_record.value(u"automatic"_s).toBool();
    m_screen = m_record.value(u"screen"_s).toBool();
    // An unreadable marker takes the stricter state. No password or address is in it.
    m_screen = m_screen || (latched && lockState != "locked\n" && !resetComplete);
    m_locked = m_record.value(u"locked"_s).toBool() || !m_automatic || !m_marked || latched;
    m_failures = failures.toInt();
    // A previously fast clock imposes no longer wait than the count owes, and no later last try than now.
    m_next = std::min(next.toInteger(), m_now() + owedWait());
    m_last = std::min(last.toInteger(0), m_now());
    m_mode = m_locked ? u"unlock"_s : QString();
}

qint64 AppLock::owedWait() const
{
    return m_failures > 0 ? std::min(300, 1 << std::min(m_failures - 1, 9)) : 0;
}

bool AppLock::quietForADay() const
{
    // A day by the PC's clock, and within this run by the steady clock as well: setting the clock a
    // day ahead forgets nothing until the program starts again (the handoff's "day of tries").
    constexpr qint64 day = 86400;
    return m_failures > 0 && m_last > 0 && m_now() - m_last >= day &&
           (!m_steadyLast || m_steady() - *m_steadyLast >= day);
}

void AppLock::becomeUnavailable()
{
    m_unavailable = true;
    m_locked = true;
    m_mode = u"unavailable"_s;
    m_problem = tr("Windows did not hand over what qKVM keeps on this PC. Nothing was changed. Try again, or quit "
                   "and open qKVM later. If this keeps happening, Windows Credential Manager is not available to "
                   "this Windows account; qKVM erases nothing while it is not.");
}

void AppLock::tryAgain()
{
    if (!m_unavailable || m_busy) {
        return;
    }
    invalidate();
    m_unavailable = false;
    m_setup = false;
    m_recovery = false;
    m_locked = true;
    m_screen = false;
    m_automatic = true;
    m_passwordSaved = false;
    m_marked = false;
    m_failures = 0;
    m_next = 0;
    m_steadyNext.reset();
    m_last = 0;
    m_steadyLast.reset();
    m_record = {};
    m_mode.clear();
    m_problem.clear();
    load();
    emit changed();
}

void AppLock::quit()
{
    if (m_unavailable) {
        emit quitAsked();
    }
}

int AppLock::retryAfter() const
{
    const qint64 now = m_now();
    // A clock put back holds a wait no longer than the one the count owes, counted from when it is seen.
    m_next = std::min(m_next, now + owedWait());
    const qint64 byClock = std::clamp<qint64>(m_next - now, 0, 300);
    // Within this run the steady clock holds it too: a clock set ahead does not shorten it.
    const qint64 bySteady = m_steadyNext ? std::clamp<qint64>(*m_steadyNext - m_steady(), 0, 300) : 0;
    return static_cast<int>(std::max(byClock, bySteady));
}

bool AppLock::persist()
{
    m_record.insert(u"v"_s, 1);
    m_record.insert(u"automatic"_s, m_automatic);
    m_record.insert(u"locked"_s, m_locked);
    m_record.insert(u"screen"_s, m_screen);
    m_record.insert(u"failures"_s, m_failures);
    m_record.insert(u"next"_s, m_next);
    m_record.insert(u"last"_s, m_last);
    QByteArray bytes = recordBytes(m_record);
    const core::WipedOnLeaving wipe(bytes);
    const auto saved = m_secrets.keep(recordName(), bytes);
    m_passwordSaved = m_passwordSaved || static_cast<bool>(saved);
    // Save the valid password first: a failed first write remains retryable.
    // The independent marker remains if
    // just the password record is subsequently lost or damaged.
    if (!saved || (!m_marked && !m_secrets.keep(setupMarkerName(), QByteArray("1")))) {
        m_locked = true;
        const bool latched = m_passwordSaved && latch();
        m_problem = !m_passwordSaved
                        ? tr("Windows could not save the app password. Your existing secrets are unchanged. Try again.")
                    : latched
                        ? tr("Windows could not save the app password settings. qKVM stays locked. Try again.")
                        : tr("The lock could not be saved. This session is locked; keep qKVM open and try again.");
        emit changed();
        return false;
    }
    m_marked = true;
    return true;
}

bool AppLock::latch(bool resetComplete)
{
    // No secret is in this marker. It is a second durable path for a lock when
    // Credential Manager refuses writes; the credential record is still authoritative.
    if (m_lockFile.isEmpty()) {
        return false;
    }
    if (!QDir().mkpath(QFileInfo(m_lockFile).absolutePath())) {
        return false;
    }
    QSaveFile file(m_lockFile);
    const QByteArray state = resetComplete ? QByteArray("setup\n")
                             : m_recovery  ? QByteArray("reset\n")
                             : m_screen    ? QByteArray("screen\n")
                                           : QByteArray("locked\n");
    return file.open(QIODevice::WriteOnly) && file.write(state) == state.size() && file.commit();
}

bool AppLock::unlatch()
{
    if (m_lockFile.isEmpty() || !QFileInfo::exists(m_lockFile)) {
        return true;
    }
    if (QFile::remove(m_lockFile)) {
        return true;
    }
    m_locked = true;
    m_problem = tr("The saved lock marker could not be cleared. qKVM stays locked.");
    emit changed();
    return false;
}

void AppLock::invalidate()
{
    disconnect(m_ownerDied);
    ++m_turn;
    m_worker.request_stop();
    m_busy = false;
    m_approved = {};
    const QPointer<QObject> owner = m_owner;
    m_owner = nullptr;
    if (owner != nullptr) {
        emit approvalCancelled(owner);
    }
    emit clearFields();
}

void AppLock::ask(const QString& purpose)
{
    if (m_busy) {
        return;
    }
    if (m_unavailable) {
        becomeUnavailable();
        emit changed();
        return;
    }
    if (m_recovery || m_setup) {
        m_mode = m_setup ? u"setup"_s : u"recovery"_s;
        emit changed();
        return;
    }
    if ((purpose == u"unlock" && m_locked) || (purpose == u"endScreen" && m_screen) ||
        (!restricted() && (purpose == u"password" || purpose == u"automatic"))) {
        invalidate();
        m_mode = purpose;
        m_problem.clear();
        emit changed();
    }
}

void AppLock::cancel()
{
    invalidate();
    m_mode.clear();
    m_problem.clear();
    emit changed();
}

void AppLock::lock()
{
    invalidate();
    if (m_unavailable) {
        // Locked already, and nothing is written while Windows does not answer.
        becomeUnavailable();
        emit changed();
        return;
    }
    m_locked = true;
    m_mode = m_setup ? u"setup"_s : m_recovery ? u"recovery"_s : u"unlock"_s;
    m_problem.clear();
    if (!m_setup && !m_recovery) {
        static_cast<void>(latch());
        static_cast<void>(persist());
    }
    emit changed();
}

void AppLock::becameWallScreen()
{
    if (m_screen || m_unavailable) {
        return;
    }
    invalidate();
    m_screen = true;
    m_mode = m_setup ? u"setup"_s : m_recovery ? u"recovery"_s : QString();
    if (!m_setup && !m_recovery) {
        static_cast<void>(persist());
    }
    emit changed();
}

void AppLock::disableAutomatic()
{
    if (restricted() || m_busy) {
        return;
    }
    m_automatic = false;
    static_cast<void>(persist());
    emit changed();
}

base::Result<QByteArray> AppLock::derivePassword(QByteArrayView password, QByteArrayView salt,
                                                 const std::stop_token& stop)
{
    std::array<quint8, 32> bytes{};
    const auto derived = core::scrypt::derive(password, salt, {131072, 8, 1}, bytes, stop);
    QByteArray result;
    if (derived) {
        result = QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    }
    core::wipe(bytes.data(), bytes.size());
    return derived ? base::Result<QByteArray>(std::move(result)) : derived.error();
}

void AppLock::submit(const QString& current, const QString& replacement, const QString& confirmation, bool automatic)
{
    const QString purpose = m_mode;
    const bool creating = purpose == u"setup";
    const bool changing = purpose == u"password";
    if (m_busy || m_recovery || m_unavailable || retryAfter() > 0 || purpose.isEmpty() || purpose == u"reset" ||
        (creating && !m_setup) || (!creating && m_setup)) {
        return;
    }
    if ((creating || changing) && (!passwordFits(replacement) || replacement != confirmation)) {
        m_problem = tr("Use 10 to 256 characters and enter the same new password twice.");
        emit changed();
        return;
    }
    if (!creating && (current.size() > 512 || !current.isValidUtf16())) {
        m_problem = tr("The password is not valid.");
        emit changed();
        return;
    }
    // Charge an attempt BEFORE starting work. Restarting or cancelling cannot evade the delay.
    if (!creating) {
        if (quietForADay()) {
            m_failures = 0;
        }
        m_failures = std::min(m_failures + 1, 30);
        const qint64 wait = owedWait();
        m_next = m_now() + wait;
        m_steadyNext = m_steady() + wait;
        m_last = m_now();
        m_steadyLast = m_steady();
        if (!persist()) {
            return;
        }
    }
    const QByteArray oldSalt = QByteArray::fromBase64(m_record.value(u"salt"_s).toString().toLatin1());
    const QByteArray oldVerifier = QByteArray::fromBase64(m_record.value(u"verifier"_s).toString().toLatin1());
    auto currentBytes = core::SecretBytes::utf8Of(current);
    auto newBytes = core::SecretBytes::utf8Of(replacement);
    if (!currentBytes || !newBytes) {
        return;
    }
    const quint64 turn = ++m_turn;
    m_busy = true;
    m_problem.clear();
    emit clearFields();
    emit changed();
    m_worker = std::jthread([this, turn, purpose, creating, changing, automatic, oldSalt, oldVerifier,
                             currentBytes = std::move(*currentBytes),
                             newBytes = std::move(*newBytes)](const std::stop_token& stop) {
        bool matches = creating;
        bool worked = true;
        QByteArray salt = oldSalt;
        QByteArray verifier = oldVerifier;
        const core::WipedOnLeaving wipe(verifier);
        if (!creating) {
            auto found = m_derive(currentBytes.view(), oldSalt, stop);
            worked = static_cast<bool>(found);
            if (found) {
                matches = same(found.value(), oldVerifier);
                core::wipeInPlace(found.value());
            }
        }
        if (matches && (creating || changing) && !stop.stop_requested()) {
            std::array<quint8, 16> random{};
            worked = static_cast<bool>(core::sealing::random(random));
            if (worked) {
                salt = QByteArray(reinterpret_cast<const char*>(random.data()), static_cast<qsizetype>(random.size()));
                auto made = m_derive(newBytes.view(), salt, stop);
                worked = static_cast<bool>(made);
                if (made) {
                    verifier = std::move(made.value());
                }
            }
        }
        if (!stop.stop_requested()) {
            QMetaObject::invokeMethod(
                this,
                [this, turn, purpose, matches, worked, salt, verifier, automatic] {
                    finish(turn, purpose, matches, worked, salt, verifier, automatic);
                },
                Qt::QueuedConnection);
        }
    });
}

void AppLock::finish(quint64 turn, const QString& purpose, bool matches, bool worked, const QByteArray& salt,
                     const QByteArray& verifier, bool automatic)
{
    if (turn != m_turn) {
        return;
    }
    m_busy = false;
    if (!worked || !matches) {
        m_problem = worked ? tr("The password is incorrect. Wait before trying again.")
                           : tr("The password could not be checked. Try again after the delay.");
        emit changed();
        return;
    }
    if (purpose == u"setup") {
        const auto names = m_secrets.names();
        if (!names) {
            // No secret is known to have appeared, and none is saved: setup stays, to be tried again.
            m_problem = tr("Windows did not answer when qKVM checked what it keeps. Nothing was saved. Try again.");
            emit changed();
            return;
        }
        const bool onlyOurPartialSetup = m_passwordSaved && std::ranges::all_of(names.value(), [](const QString& name) {
                                             return name.compare(recordName(), Qt::CaseInsensitive) == 0 ||
                                                    name.compare(setupMarkerName(), Qt::CaseInsensitive) == 0 ||
                                                    theProgramsOwn(name);
                                         });
        if (!std::ranges::all_of(names.value(), theProgramsOwn) && !onlyOurPartialSetup) {
            m_setup = false;
            m_recovery = true;
            m_locked = true;
            m_mode = u"recovery"_s;
            m_problem = tr("Saved secrets appeared during setup. Reset saved secrets to continue.");
            emit changed();
            return;
        }
    }
    m_failures = 0;
    m_next = 0;
    m_steadyNext.reset();
    m_record.insert(u"salt"_s, QString::fromLatin1(salt.toBase64()));
    m_record.insert(u"verifier"_s, QString::fromLatin1(verifier.toBase64()));
    if (purpose == u"setup" || purpose == u"unlock") {
        m_locked = false;
    }
    if (purpose == u"setup") {
        m_automatic = automatic;
    } else if (purpose == u"automatic") {
        m_automatic = true;
    } else if (purpose == u"endScreen") {
        if (!m_endScreen || !m_endScreen()) {
            m_problem = tr("The wall screen could not be signed out. Try again.");
            emit changed();
            return;
        }
        m_screen = false;
        m_locked = false;
    }
    if (!persist()) {
        return;
    }
    if (!m_locked && !unlatch()) {
        return;
    }
    m_setup = false;
    const QPointer<QObject> owner = m_owner;
    auto approved = std::exchange(m_approved, {});
    m_owner = nullptr;
    disconnect(m_ownerDied);
    m_mode.clear();
    m_problem.clear();
    emit clearFields();
    if (purpose == u"endScreen") {
        emit endWallScreen();
    }
    emit changed();
    if (turn == m_turn && purpose == u"export" && owner != nullptr && approved && !restricted()) {
        approved();
    }
}

bool AppLock::authorizeExport(QObject* owner, std::function<void()> approved)
{
    if (restricted() || m_busy || !m_mode.isEmpty() || owner == nullptr) {
        return false;
    }
    m_owner = owner;
    m_ownerDied = connect(owner, &QObject::destroyed, this, &AppLock::cancel);
    m_approved = std::move(approved);
    m_mode = u"export"_s;
    m_problem.clear();
    emit clearFields();
    emit changed();
    return true;
}

void AppLock::cancelApproval(QObject* owner)
{
    if (m_owner == owner) {
        cancel();
    }
}

void AppLock::forgot()
{
    // Never a reset while Windows does not hand over what is kept: nothing is known to be lost.
    if (m_unavailable) {
        return;
    }
    invalidate();
    m_mode = u"reset"_s;
    m_problem.clear();
    emit changed();
}

void AppLock::resetSecrets(bool understood)
{
    if (!understood || m_mode != u"reset" || m_busy || m_unavailable) {
        return;
    }
    invalidate();
    m_recovery = true;
    m_locked = true;
    emit changed();
    emit resetStarting();
    // Leave a recovery marker until EVERY other secret is gone. A partial reset can
    // never accept the previous password or auto sign-in on the next start.
    const bool latched = latch();
    const auto marked = m_secrets.keep(recordName(), recordBytes(QJsonObject{{u"v"_s, 1}, {u"reset"_s, true}}));
    const auto names = m_secrets.names();
    bool erased = static_cast<bool>(marked) && static_cast<bool>(names);
    if (erased) {
        for (const QString& name : names.value()) {
            if (name.compare(recordName(), Qt::CaseInsensitive) != 0) {
                erased = static_cast<bool>(m_secrets.erase(name)) && erased;
            }
        }
        const auto remaining = m_secrets.names();
        erased = erased && remaining && remaining.value().size() == 1 &&
                 remaining.value().first().compare(recordName(), Qt::CaseInsensitive) == 0;
    }
    if (erased && m_secrets.erase(recordName())) {
        const auto remaining = m_secrets.names();
        erased = remaining && remaining.value().isEmpty();
    } else {
        erased = false;
    }
    if (erased && (m_lockFile.isEmpty() || latch(true))) {
        emit resetFinished();
    } else {
        m_mode = u"recovery"_s;
        m_problem = erased ? tr("Saved secrets were erased, but the completed reset could not be saved. qKVM stays "
                                "locked. Try the reset again.")
                    : !latched && !marked
                        ? tr("The reset could not be saved. This session is locked; keep qKVM open and try again.")
                        : tr("Some saved secrets could not be erased. qKVM stays locked. Try the reset again.");
        emit changed();
    }
}

} // namespace qkvm::ui
