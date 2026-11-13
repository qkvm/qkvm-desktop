#include <qkvm/core/setup_overrides.h>

#include <algorithm>
#include <utility>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <qkvm/core/redfish_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

constexpr int fileVersion = 1;

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
    return {{u"address"_s, left.address},
            {u"machine"_s, left.machine},
            {u"since"_s, left.since.toUTC().toString(Qt::ISODateWithMs)},
            {u"doubtUntil"_s, left.doubtUntil.toUTC().toString(Qt::ISODateWithMs)}};
}

std::optional<SetupOverrides::Left> leftFrom(const QJsonValue& value)
{
    const QJsonObject said = value.toObject();
    SetupOverrides::Left left;
    left.address = said.value("address"_L1).toString();
    left.machine = said.value("machine"_L1).toString();
    left.since = QDateTime::fromString(said.value("since"_L1).toString(), Qt::ISODateWithMs);
    left.doubtUntil = QDateTime::fromString(said.value("doubtUntil"_L1).toString(), Qt::ISODateWithMs);
    const QUrl address(left.address, QUrl::StrictMode);
    if (!address.isValid() || address.host().isEmpty() || left.address.size() > 2048 || left.machine.size() > 256 ||
        !left.since.isValid() || !left.doubtUntil.isValid()) {
        return std::nullopt;
    }
    return left;
}

} // namespace

SetupOverrides::SetupOverrides(QString file, std::function<QDateTime()> wallClock)
    : m_file(std::move(file)), m_wallClock(std::move(wallClock))
{
    read();
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

void SetupOverrides::keepIn(const QString& file)
{
    const QList<Left> before = std::exchange(m_left, {});
    m_file = file;
    read();
    for (const Left& one : before) {
        if (const qsizetype at = indexOf(QUrl(one.address), one.machine); at >= 0) {
            m_left[at].doubtUntil = std::max(m_left.at(at).doubtUntil, one.doubtUntil);
        } else {
            m_left.append(one);
        }
    }
    if (!before.isEmpty()) {
        write();
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
        if (isOf(m_left.at(at), address, machine)) {
            return at;
        }
    }
    return -1;
}

std::optional<QDateTime> SetupOverrides::mayBeSet(const QUrl& address, const QString& machine,
                                                  const QDateTime& doubtUntil)
{
    std::optional<QDateTime> before;
    if (const qsizetype at = indexOf(address, machine); at >= 0) {
        before = m_left.at(at).doubtUntil;
        Left& left = m_left[at];
        left.doubtUntil = std::max(left.doubtUntil, doubtUntil);
        // (As it is now: the card may have moved to another address of the same server.)
        left.address = addressOf(address);
        if (!numberIn(machine).isEmpty()) {
            left.machine = numberIn(machine);
        }
    } else {
        m_left.append(Left{addressOf(address), numberIn(machine), wallNow(), doubtUntil});
        while (m_left.size() > mostKept) {
            m_left.removeFirst();
        }
    }
    write();
    return before;
}

void SetupOverrides::settle(const QUrl& address, const QString& machine, const QDateTime& doubtUntil)
{
    if (const qsizetype at = indexOf(address, machine); at >= 0 && m_left.at(at).doubtUntil != doubtUntil) {
        m_left[at].doubtUntil = doubtUntil;
        write();
    }
}

void SetupOverrides::drop(const QUrl& address, const QString& machine)
{
    if (const qsizetype at = indexOf(address, machine); at >= 0) {
        m_left.removeAt(at);
        write();
    }
}

bool SetupOverrides::seenOff(const QUrl& address, const QString& machine, const QDateTime& at)
{
    const qsizetype found = indexOf(address, machine);
    if (found < 0 || at < m_left.at(found).doubtUntil) {
        return false;
    }
    m_left.removeAt(found);
    write();
    return true;
}

std::optional<SetupOverrides::Left> SetupOverrides::standing(const QUrl& address, const QString& machine) const
{
    const qsizetype at = indexOf(address, machine);
    return at >= 0 ? std::optional<Left>(m_left.at(at)) : std::nullopt;
}

QString SetupOverrides::onTheCard()
{
    return u"Its BIOS setup boot override may be set, so its next boot may land in the BIOS setup: a start into BIOS "
           u"setup did not end. Look at its boot override, or start into BIOS setup again."_s;
}

QStringList SetupOverrides::toldAtStart(const QList<OwnKvm>& kvms) const
{
    QStringList told;
    for (const OwnKvm& kvm : kvms) {
        if (kvm.kind != "redfish"_L1 || !standing(QUrl(kvm.address), kvm.machine)) {
            continue;
        }
        const QString name = kvm.name.isEmpty() ? kvm.address : kvm.name;
        told.append(u"%1: its BIOS setup boot override may still be set from a start into BIOS setup that did not end, "
                    u"so the server's next boot may land in the BIOS setup. Look at its boot override, or start into "
                    u"BIOS setup again."_s.arg(name));
    }
    return told;
}

void SetupOverrides::read()
{
    if (m_file.isEmpty()) {
        return;
    }
    QFile file(m_file);
    if (!file.open(QIODevice::ReadOnly) || file.size() > mostFileBytes) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.read(mostFileBytes)).object();
    if (root.value("version"_L1) != QJsonValue(fileVersion)) {
        return;
    }
    for (const QJsonValue one : root.value("servers"_L1).toArray()) {
        if (m_left.size() >= mostKept) {
            break;
        }
        if (const std::optional<Left> left = leftFrom(one)) {
            m_left.append(*left);
        }
    }
}

void SetupOverrides::write()
{
    if (m_file.isEmpty()) {
        return;
    }
    QJsonArray servers;
    for (const Left& left : std::as_const(m_left)) {
        servers.append(recordOf(left));
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
