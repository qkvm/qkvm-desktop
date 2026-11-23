#include <qkvm/core/setup_overrides.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>

#include <qkvm/core/redfish_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

constexpr int fileVersion = 1;
// As many files as this are put aside, and no more (as the sign-in counts': a PC on which every start finds one that
// cannot be read has another trouble, and is not filled with them).
constexpr int mostPutAside = 10;

// The computer's mark of what is kept, where it is a number (and not the mark of one that gave none).
QString numberIn(const QString& machine)
{
    const QString server = redfish::marksOf(machine).server;
    return server.startsWith(u"server:"_s) && server != redfish::saidNone ? server : QString();
}

QString addressOf(const QUrl& address)
{
    return address.toString(QUrl::FullyEncoded);
}

QJsonObject recordOf(const SetupOverrides::Left& left)
{
    QJsonObject record{{u"address"_s, left.address},
                       {u"machine"_s, left.machine},
                       {u"since"_s, left.since.toUTC().toString(Qt::ISODateWithMs)},
                       {u"doubtUntil"_s, left.doubtUntil.toUTC().toString(Qt::ISODateWithMs)}};
    // (Only where it is marked queued: a version before reads the rest as ever.)
    if (left.queued) {
        record.insert(u"queued"_s, true);
    }
    return record;
}

std::optional<SetupOverrides::Left> leftFrom(const QJsonValue& value)
{
    const QJsonObject said = value.toObject();
    SetupOverrides::Left left;
    left.address = said.value("address"_L1).toString();
    left.machine = said.value("machine"_L1).toString();
    left.since = QDateTime::fromString(said.value("since"_L1).toString(), Qt::ISODateWithMs);
    left.doubtUntil = QDateTime::fromString(said.value("doubtUntil"_L1).toString(), Qt::ISODateWithMs);
    left.queued = said.value("queued"_L1).toBool();
    const QUrl address(left.address, QUrl::StrictMode);
    if (!address.isValid() || address.host().isEmpty() || left.address.size() > 2048 || left.machine.size() > 256 ||
        !left.since.isValid() || !left.doubtUntil.isValid()) {
        return std::nullopt;
    }
    return left;
}

} // namespace

SetupOverrides::SetupOverrides(QString file, std::function<QDateTime()> wallClock, std::function<qint64()> steadyClock,
                               Settings::Patience patience)
    : m_file(std::move(file)), m_wallClock(std::move(wallClock)), m_steadyClock(std::move(steadyClock)),
      m_patience(patience)
{
    read(m_patience.tries);
}

SetupOverrides& SetupOverrides::program()
{
    static SetupOverrides kept;
    return kept;
}

QDateTime SetupOverrides::wallNow() const
{
    return m_wallClock ? m_wallClock() : QDateTime::currentDateTimeUtc();
}

qint64 SetupOverrides::steadyNow() const
{
    if (m_steadyClock) {
        return m_steadyClock();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

qint64 SetupOverrides::steadyEndOf(const QDateTime& doubtUntil) const
{
    return steadyNow() + std::clamp(wallNow().msecsTo(doubtUntil), qint64{0}, longestDoubtMs);
}

bool SetupOverrides::doubtIsOver(const Kept& kept, const QDateTime& at) const
{
    // Noted in this run: by the steady clock alone. Of a run before: by the wall clock (a look's own time).
    return kept.steadyUntil ? steadyNow() >= *kept.steadyUntil : at >= kept.left.doubtUntil;
}

bool SetupOverrides::markQueued(const QUrl& address, const QString& machine)
{
    if (const qsizetype at = indexOf(address, machine); at >= 0) {
        const bool marked = !std::exchange(m_left[at].left.queued, true);
        write();
        return marked;
    }
    return false;
}

void SetupOverrides::seenMade(const QUrl& address, const QString& machine)
{
    if (const qsizetype at = indexOf(address, machine); at >= 0 && m_left.at(at).left.queued) {
        m_left[at].left.queued = false;
        write();
    }
}

void SetupOverrides::forget(const QUrl& address, const QString& machine)
{
    if (const qsizetype at = indexOf(address, machine); at >= 0) {
        m_left.removeAt(at);
        write();
    }
}

QList<SetupOverrides::Left> SetupOverrides::all() const
{
    QList<Left> lefts;
    lefts.reserve(m_left.size());
    for (const Kept& kept : m_left) {
        lefts.append(kept.left);
    }
    return lefts;
}

bool SetupOverrides::mayStillCome(const Kept& kept, const QDateTime& now, qint64 steady)
{
    // Noted in this run: until its end on the steady clock, whatever the wall clock does meanwhile.
    if (kept.steadyUntil) {
        return steady < *kept.steadyUntil;
    }
    // Of a run before: not one whose doubt reaches further than any press notes (noted by a clock that ran ahead).
    return kept.left.doubtUntil > now && now.msecsTo(kept.left.doubtUntil) <= longestDoubtMs;
}

bool SetupOverrides::changeMayStillCome() const
{
    if (m_underWay->load() > 0) {
        return true;
    }
    const QDateTime now = wallNow();
    const qint64 steady = steadyNow();
    return std::ranges::any_of(m_left, [&now, steady](const Kept& kept) { return mayStillCome(kept, now, steady); });
}

bool SetupOverrides::pruneBy(const QList<OwnKvm>& kvms)
{
    const QDateTime now = wallNow();
    const qint64 steady = steadyNow();
    const qsizetype gone = m_left.removeIf([&kvms, &now, steady](const Kept& kept) {
        // (Never one marked queued, whatever the list says: only its card taken off lets it go, forget. A list read
        // whole may be one a run wrote after its settings could not be read, with none of the old cards: outside review
        // of the prune, round 2, M1.)
        if (kept.left.queued) {
            return false;
        }
        const bool onACard = std::ranges::any_of(kvms, [&kept](const OwnKvm& kvm) {
            return kvm.kind == "redfish"_L1 && isOf(kept.left, QUrl(kvm.address), kvm.machine);
        });
        return !onACard && !mayStillCome(kept, now, steady);
    });
    if (gone == 0) {
        return false;
    }
    write();
    return true;
}

bool SetupOverrides::prune(const Settings& settings)
{
    if (!settings.listReadWhole()) {
        return false; // (a wall that began as new, or a list read in part, says nothing of which servers are on no
                      // card)
    }
    // Nor a list read whole while settings that could not be read stand put aside beside it: it may be the one a run
    // wrote after them, with none of their cards, which come back where the kept file is put back, each with its note
    // (outside review of the fuzz work, round 3, M2).
    if (settings.keepsAsideWhatCouldNotBeRead()) {
        return false;
    }
    return pruneBy(settings.remembered().kvms);
}

std::shared_ptr<void> SetupOverrides::pressUnderWay()
{
    // (It points at the count it holds up, so that whoever keeps it can tell it has one; the count goes with the last
    // one of them, never with this alone.)
    ++*m_underWay;
    return {m_underWay.get(), [underWay = m_underWay](const void*) { --*underWay; }};
}

void SetupOverrides::keepIn(const QString& file)
{
    const QList<Kept> before = std::exchange(m_left, {});
    m_file = file;
    read(m_patience.tries);
    takeIn(before);
    if (!before.isEmpty()) {
        write();
    }
}

void SetupOverrides::takeIn(const QList<Kept>& others)
{
    for (const Kept& one : others) {
        if (const qsizetype at = indexOf(QUrl(one.left.address), one.left.machine); at >= 0) {
            m_left[at].left.doubtUntil = std::max(m_left.at(at).left.doubtUntil, one.left.doubtUntil);
            if (one.steadyUntil) {
                m_left[at].steadyUntil = std::max(*one.steadyUntil, steadyEndOf(m_left.at(at).left.doubtUntil));
            }
            // (Marked queued where either was.)
            m_left[at].left.queued = m_left.at(at).left.queued || one.left.queued;
        } else {
            m_left.append(one);
        }
    }
}

bool SetupOverrides::isOf(const Left& left, const QUrl& address, const QString& machine)
{
    const QString kept = numberIn(left.machine);
    const QString asked = numberIn(machine);
    if (!kept.isEmpty() && !asked.isEmpty()) {
        return kept == asked;
    }
    return left.address == addressOf(address);
}

qsizetype SetupOverrides::indexOf(const QUrl& address, const QString& machine) const
{
    for (qsizetype at = 0; at < m_left.size(); ++at) {
        if (isOf(m_left.at(at).left, address, machine)) {
            return at;
        }
    }
    return -1;
}

SetupOverrides::Noted SetupOverrides::mayBeSet(const QUrl& address, const QString& machine, const QDateTime& doubtUntil)
{
    std::optional<Earlier> before;
    if (const qsizetype at = indexOf(address, machine); at >= 0) {
        before = Earlier{m_left.at(at).left.doubtUntil, m_left.at(at).steadyUntil};
        Kept& kept = m_left[at];
        kept.left.doubtUntil = std::max(kept.left.doubtUntil, doubtUntil);
        // (Its doubt in this run: the longest of this one, one noted before in it, and one of a run before that still
        // runs, on the steady clock.)
        kept.steadyUntil = std::max(kept.steadyUntil.value_or(0), steadyEndOf(kept.left.doubtUntil));
        // (As it is now: the card may have moved to another address of the same server.)
        kept.left.address = addressOf(address);
        if (!numberIn(machine).isEmpty()) {
            kept.left.machine = numberIn(machine);
        }
    } else {
        if (!roomForOneMore()) {
            // Every one kept must stay: nothing is noted, written or let go of, and the press does not go (the owner's
            // rule: a record marked queued is never let go of to make room; the merged review of 0.2.0, A-L1).
            return {false, before, true};
        }
        m_left.append(
            Kept{Left{addressOf(address), numberIn(machine), wallNow(), doubtUntil}, steadyEndOf(doubtUntil)});
    }
    write();
    // (In memory alone, with no file: what there is to write down is written.)
    return {m_file.isEmpty() || m_written.value_or(false), before};
}

bool SetupOverrides::roomForOneMore() const
{
    // (At most mostKept, and none is let go of to make room: each stands for a server whose override may be set, and
    // its card says so, marked queued or not. The owner's rule that a queued record is never pruned, the merged review
    // of 0.2.0, A-L1, and round 2's L4: an unmarked one whose card still said "may be set" went to the cap. Records go
    // by their own roads alone: a look that shows the override off, a press that ends knowing it cleared it, a start
    // into BIOS setup that ends as it should, the card's removal, the prune of servers on no card.)
    return m_left.size() < mostKept;
}

void SetupOverrides::restore(const QUrl& address, const QString& machine, const Earlier& earlier)
{
    if (const qsizetype at = indexOf(address, machine); at >= 0) {
        Kept& kept = m_left[at];
        kept.steadyUntil = earlier.steadyUntil;
        if (kept.left.doubtUntil != earlier.doubtUntil) {
            kept.left.doubtUntil = earlier.doubtUntil;
            write();
        }
    }
}

void SetupOverrides::settle(const QUrl& address, const QString& machine, const QDateTime& doubtUntil)
{
    if (const qsizetype at = indexOf(address, machine); at >= 0) {
        Kept& kept = m_left[at];
        kept.steadyUntil = steadyEndOf(doubtUntil);
        if (kept.left.doubtUntil != doubtUntil) {
            kept.left.doubtUntil = doubtUntil;
            write();
        }
    }
}

bool SetupOverrides::drop(const QUrl& address, const QString& machine)
{
    // (One marked queued stays: D1.)
    const qsizetype at = indexOf(address, machine);
    if (at < 0 || m_left.at(at).left.queued) {
        return false;
    }
    m_left.removeAt(at);
    write();
    return true;
}

bool SetupOverrides::seenOff(const QUrl& address, const QString& machine, const QDateTime& at)
{
    const qsizetype found = indexOf(address, machine);
    // (Nor does a look let one marked queued go, however late: D1.)
    if (found < 0 || m_left.at(found).left.queued || !doubtIsOver(m_left.at(found), at)) {
        return false;
    }
    m_left.removeAt(found);
    write();
    return true;
}

std::optional<SetupOverrides::Left> SetupOverrides::standing(const QUrl& address, const QString& machine) const
{
    const qsizetype at = indexOf(address, machine);
    return at >= 0 ? std::optional<Left>(m_left.at(at).left) : std::nullopt;
}

QString SetupOverrides::onTheCard(bool queued)
{
    if (queued) {
        return u"The controller queued a change qKVM could not follow; check the boot override in its BMC. Its next "
               u"boot may land in the BIOS setup. Said until a start into BIOS setup here ends as it should."_s;
    }
    return u"Its BIOS setup boot override may be set, so its next boot may land in the BIOS setup: a start into BIOS "
           u"setup did not end. Look at its boot override, or start into BIOS setup again."_s;
}

QStringList SetupOverrides::toldAtStart(const QList<OwnKvm>& kvms) const
{
    QStringList told;
    for (const OwnKvm& kvm : kvms) {
        const std::optional<Left> left =
            kvm.kind == "redfish"_L1 ? standing(QUrl(kvm.address), kvm.machine) : std::nullopt;
        if (!left) {
            continue;
        }
        const QString name = kvm.name.isEmpty() ? kvm.address : kvm.name;
        if (left->queued) {
            told.append(u"%1: the controller queued a change of its BIOS setup boot override that qKVM could not "
                        u"follow, so the server's next boot may land in the BIOS setup. Check the boot override in its "
                        u"BMC; this is said until a start into BIOS setup here ends as it should."_s.arg(name));
            continue;
        }
        told.append(u"%1: its BIOS setup boot override may still be set from a start into BIOS setup that did not end, "
                    u"so the server's next boot may land in the BIOS setup. Look at its boot override, or start into "
                    u"BIOS setup again."_s.arg(name));
    }
    return told;
}

SetupOverrides::Reading SetupOverrides::readFile(int tries) const
{
    Reading reading;
    // (What is there and is no file, a folder of that name, is not one that could not be read: nothing is lost with
    // it, and it is in the way of the writing, which says so.)
    if (m_file.isEmpty() || !QFileInfo(m_file).isFile()) {
        return reading;
    }
    reading.whole = false;
    QFile file(m_file);
    // Tried as the settings are: another program (one that looks for viruses, one that makes copies) holds a file for
    // a moment, and rarely for longer.
    bool open = file.open(QIODevice::ReadOnly);
    for (int tried = 1; tried < tries && !open; ++tried) {
        QThread::msleep(static_cast<unsigned long>(std::max(0, m_patience.betweenMs)));
        open = file.open(QIODevice::ReadOnly);
    }
    if (!open || file.size() > mostFileBytes) {
        return reading;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.read(mostFileBytes));
    const QJsonObject root = document.object();
    if (!document.isObject() || root.value("version"_L1) != QJsonValue(fileVersion) ||
        !root.value("servers"_L1).isArray()) {
        return reading;
    }
    const QJsonArray servers = root.value("servers"_L1).toArray();
    reading.whole = servers.size() <= mostKept;
    for (const QJsonValue one : servers) {
        if (reading.kept.size() >= mostKept) {
            break;
        }
        if (const std::optional<Left> left = leftFrom(one)) {
            // One whose doubt still runs, by the wall clock now and within the longest doubt, runs on from here on the
            // steady clock, for what is left of it: a wall clock put right later in this run does not shorten it.
            const QDateTime now = wallNow();
            const bool runs = left->doubtUntil > now && now.msecsTo(left->doubtUntil) <= longestDoubtMs;
            reading.kept.append(
                Kept{*left, runs ? std::optional<qint64>(steadyEndOf(left->doubtUntil)) : std::nullopt});
        } else {
            reading.whole = false; // (a record that is none: its bytes are kept aside, never dropped)
        }
    }
    return reading;
}

void SetupOverrides::read(int tries)
{
    Reading reading = readFile(tries);
    m_wasUnreadable = !reading.whole;
    m_inTheWay = !reading.whole;
    m_putAside.clear();
    takeIn(reading.kept);
}

base::Result<void> SetupOverrides::putAsideWhatCouldNotBeRead()
{
    if (!m_inTheWay) {
        return {};
    }
    const QFileInfo was(m_file);
    // (Gone meanwhile, moved by whoever looks after the PC: nothing is in the way any more.)
    if (!was.exists()) {
        m_inTheWay = false;
        return {};
    }
    for (int number = 1; number <= mostPutAside; ++number) {
        const QString aside = was.dir().filePath(number == 1 ? was.fileName() + u".unreadable"_s
                                                             : was.fileName() + u".unreadable-%1"_s.arg(number));
        if (QFile::exists(aside)) {
            continue;
        }
        // A plain move, which Windows refuses for a file that something holds (Qt's own would copy it then).
        std::error_code refused;
        std::filesystem::rename(std::filesystem::path(m_file.toStdU16String()),
                                std::filesystem::path(aside.toStdU16String()), refused);
        if (refused) {
            break;
        }
        m_putAside = aside;
        m_inTheWay = false;
        qWarning("qKVM: which servers' BIOS setup boot override may be set could not be read whole: put aside as %s",
                 qPrintable(QDir::toNativeSeparators(aside)));
        return {};
    }
    return base::Error{"settings", "qKVM's note of which servers' BIOS setup boot override may be set could not be "
                                   "read when it started, and could not be put aside. Nothing is written over it."};
}

QString SetupOverrides::toldOfItsFile() const
{
    if (!m_putAside.isEmpty()) {
        return u"qKVM could not read all of its note of which of this PC's servers may still boot into their BIOS "
               u"setup (%1). It is kept as it was, as %2 beside the settings, and a server it names may say nothing "
               u"of it on its card. Look at the boot override of this PC's servers in their BMCs."_s.arg(
                   QFileInfo(m_file).fileName(), QFileInfo(m_putAside).fileName());
    }
    if (m_inTheWay) {
        return u"qKVM could not read all of its note of which of this PC's servers may still boot into their BIOS "
               u"setup (%1), and could not put it aside: another program may hold it. Nothing is written over it, "
               u"and no start into BIOS setup is sent until it can be read or put aside. Look at the boot override "
               u"of this PC's servers in their BMCs, or start qKVM again."_s.arg(QFileInfo(m_file).fileName());
    }
    return {};
}

void SetupOverrides::write()
{
    if (m_file.isEmpty()) {
        return;
    }
    if (m_inTheWay) {
        // A file that could not be read whole is never written over: read again first (one that another program held
        // may be free now, and what it holds is taken in beside what was noted meanwhile), else put aside, else
        // nothing is written (outside review of the fuzz work, round 3, M1).
        const Reading now = readFile(1);
        takeIn(now.kept);
        if (now.whole) {
            m_inTheWay = false;
        } else if (const base::Result<void> aside = putAsideWhatCouldNotBeRead(); !aside) {
            m_written = false;
            qWarning("qKVM: which servers' BIOS setup boot override may be set could not be written down: its file "
                     "could not be read, and could not be put aside");
            return;
        }
    }
    QJsonArray servers;
    for (const Kept& kept : std::as_const(m_left)) {
        servers.append(recordOf(kept.left));
    }
    const QByteArray bytes =
        QJsonDocument(QJsonObject{{u"version"_s, fileVersion}, {u"servers"_s, servers}}).toJson(QJsonDocument::Compact);
    QSaveFile file(m_file);
    m_written = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    if (!*m_written) {
        qWarning("qKVM: which servers' BIOS setup boot override may be set could not be written down");
    }
}

} // namespace qkvm::core
