#include <qkvm/core/discovery.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

#include <QHostAddress>
#include <QNetworkInterface>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {
namespace {

// Several windows may each have a discovery dialog. One search at a time keeps the stated
// request bound true for the whole application, including while another window is hidden.
std::atomic<Discovery*> activeSearch{nullptr};

void releaseSearch(Discovery* owner)
{
    auto* expected = owner;
    activeSearch.compare_exchange_strong(expected, nullptr);
}

base::Error invalid(const QString& message)
{
    return {"invalid", message.toStdString()};
}

// QHostAddress accepts shortened, octal and hexadecimal IPv4. Discovery does not: a scope must
// mean precisely the dotted-decimal address the person saw in its preview.
std::optional<quint32> ipv4(const QString& text)
{
    const QStringList parts = text.split(u'.');
    if (parts.size() != 4) {
        return std::nullopt;
    }
    quint32 result = 0;
    for (const QString& part : parts) {
        if (part.isEmpty() || part.size() > 3 || (part.size() > 1 && part.front() == u'0')) {
            return std::nullopt;
        }
        int number = 0;
        for (const QChar digit : part) {
            if (digit < u'0' || digit > u'9') {
                return std::nullopt;
            }
            number = number * 10 + digit.unicode() - u'0';
        }
        if (number > 255) {
            return std::nullopt;
        }
        result = (result << 8) | static_cast<quint32>(number);
    }
    return result;
}

int privateBlock(quint32 address)
{
    if ((address & 0xff000000U) == 0x0a000000U) {
        return 1;
    }
    if ((address & 0xfff00000U) == 0xac100000U) {
        return 2;
    }
    if ((address & 0xffff0000U) == 0xc0a80000U) {
        return 3;
    }
    if ((address & 0xffff0000U) == 0xa9fe0000U) {
        return 4;
    }
    return 0;
}

quint32 maskFor(int prefix)
{
    return prefix == 0 ? 0U : 0xffffffffU << (32 - prefix);
}

quint64 hostCountFor(int prefix)
{
    const quint64 all = quint64{1} << (32 - prefix);
    return prefix < 31 ? all - 2 : all;
}

bool recognized(KvmProbe::Kind kind)
{
    return kind == KvmProbe::Kind::Kvmd || kind == KvmProbe::Kind::JetKvm || kind == KvmProbe::Kind::Redfish;
}

} // namespace

QList<Discovery::Network> Discovery::networks()
{
    QList<Network> result;
    QSet<QString> seen;
    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : interface.addressEntries()) {
            const int prefix = entry.prefixLength();
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || prefix < 0 || prefix > 32 ||
                privateBlock(entry.ip().toIPv4Address()) == 0) {
                continue;
            }
            const QString address = entry.ip().toString();
            const QString cidr =
                QHostAddress(entry.ip().toIPv4Address() & maskFor(prefix)).toString() + u'/' + QString::number(prefix);
            const QString key = interface.name() + u'\n' + cidr;
            if (seen.contains(key)) {
                continue;
            }
            seen.insert(key);
            const QString label =
                interface.humanReadableName().isEmpty() ? interface.name() : interface.humanReadableName();
            result.append({interface.name(), label, address, cidr, prefix, hostCountFor(prefix)});
        }
    }
    std::sort(result.begin(), result.end(), [](const Network& left, const Network& right) {
        if (left.label != right.label) {
            return left.label < right.label;
        }
        return left.cidr < right.cidr;
    });
    return result;
}

base::Result<QList<Discovery::Target>> Discovery::targetsFor(const QStringList& scopes)
{
    QList<Target> targets;
    QHash<QString, int> positions;
    const auto append = [&](const QUrl& address, bool tryHttp) -> base::Result<void> {
        const QString key = address.toString(QUrl::FullyEncoded);
        const auto existing = positions.constFind(key);
        if (existing != positions.cend()) {
            // An explicit HTTPS entry always wins over the anonymous fallback of an overlapping
            // range. No spelling of a second scope can quietly undo that choice.
            targets[*existing].tryHttp = targets[*existing].tryHttp && tryHttp;
            return {};
        }
        if (targets.size() >= maxTargets) {
            return invalid(u"A search can include at most %1 addresses. Choose a smaller range."_s.arg(maxTargets));
        }
        positions.insert(key, static_cast<int>(targets.size()));
        targets.append({address, tryHttp});
        return {};
    };
    const auto appendRange = [&](quint32 first, quint32 last) -> base::Result<void> {
        if (first > last) {
            return invalid(u"The first address must come before the last address."_s);
        }
        if (privateBlock(first) == 0 || privateBlock(first) != privateBlock(last)) {
            return invalid(u"Search only private IPv4 or link-local addresses on networks you can access."_s);
        }
        // Widen before subtraction, and check before allocating or iterating. A /0 or an
        // inverted range must never wrap into a small-looking search.
        const quint64 count = quint64{last} - first + 1;
        if (count > maxTargets) {
            return invalid(u"A search can include at most %1 addresses. Choose a smaller range."_s.arg(maxTargets));
        }
        for (quint64 number = first; number <= last; ++number) {
            QUrl address;
            address.setScheme(u"https"_s);
            address.setHost(QHostAddress(static_cast<quint32>(number)).toString());
            auto added = append(address, true);
            if (!added) {
                return added;
            }
        }
        return {};
    };

    for (const QString& input : scopes) {
        const QString scope = input.trimmed();
        if (scope.isEmpty()) {
            continue;
        }
        if (scope.size() > kvm::longestTypedAddress) {
            return invalid(u"A search address is too long."_s);
        }
        const bool schemeTyped = kvm::schemeWasTyped(scope);
        if (!schemeTyped && scope.contains(u'/')) {
            const QStringList parts = scope.split(u'/');
            if (parts.size() != 2 || parts[1].isEmpty() || parts[1].size() > 2 ||
                !std::all_of(parts[1].cbegin(), parts[1].cend(),
                             [](QChar digit) { return digit >= u'0' && digit <= u'9'; })) {
                return invalid(u"Enter a network as an IPv4 address and prefix, for example 192.168.1.0/24."_s);
            }
            const auto host = ipv4(parts[0]);
            const int prefix = parts[1].toInt();
            if (!host || prefix > 32) {
                return invalid(u"Enter a valid IPv4 address and a prefix from 20 to 32."_s);
            }
            if (prefix < 20) {
                return invalid(u"Choose a /20 or smaller network, with at most 4096 addresses."_s);
            }
            const quint32 mask = maskFor(prefix);
            const quint32 network = *host & mask;
            const quint32 last = network | ~mask;
            auto added = appendRange(prefix < 31 ? network + 1 : network, prefix < 31 ? last - 1 : last);
            if (!added) {
                return added.error();
            }
        } else if (!schemeTyped && scope.contains(u'-')) {
            const QStringList parts = scope.split(u'-');
            const auto first = parts.size() == 2 ? ipv4(parts[0].trimmed()) : std::nullopt;
            const auto last = parts.size() == 2 ? ipv4(parts[1].trimmed()) : std::nullopt;
            if (!first || !last) {
                return invalid(u"Enter two complete IPv4 addresses, for example 192.168.1.20-192.168.1.40."_s);
            }
            auto added = appendRange(*first, *last);
            if (!added) {
                return added.error();
            }
        } else {
            auto parsed = kvm::addressFrom(scope);
            if (!parsed) {
                return parsed.error();
            }
            const auto host = ipv4(parsed.value().host());
            if (!host || privateBlock(*host) == 0) {
                return invalid(
                    u"Enter a numeric private IPv4 or link-local address. Host names and IPv6 are not supported yet."_s);
            }
            auto added = append(parsed.value(), !schemeTyped);
            if (!added) {
                return added.error();
            }
        }
    }
    if (targets.isEmpty()) {
        return invalid(u"Choose a network or enter an address range to search."_s);
    }
    return targets;
}

base::Result<Discovery::Plan> Discovery::planFor(const QStringList& scopes)
{
    auto targets = targetsFor(scopes);
    if (!targets) {
        return targets.error();
    }
    const int total = static_cast<int>(targets.value().size());
    return Plan{total, total > confirmationThreshold};
}

Discovery::Discovery(QObject* parent)
    : Discovery(
          [](const QUrl& address, QObject* lifetime, std::function<void(KvmProbe::Found)> done) {
              // Discovery asks less of a quiet host than an interactive sign-in. Its results are
              // candidates, never a prerequisite for manual addition or evidence a device is down.
              // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): owned by the attempt's Qt lifetime
              auto* probe = new KvmProbe(KvmProbe::Pace{1500, 1500, 3000}, lifetime);
              probe->ask(address, std::move(done));
          },
          parent)
{}

Discovery::Discovery(Probe probe, QObject* parent) : QObject(parent), m_probe(std::move(probe)) {}

Discovery::~Discovery()
{
    // No completion signals while an owner or a dialog is being destroyed.
    m_running = false;
    ++m_generation;
    m_run.reset();
    releaseSearch(this);
}

base::Result<void> Discovery::start(const QStringList& scopes, bool confirmedLarge)
{
    if (m_running) {
        return base::Error{"busy", "A search is already running. Stop it before starting another."};
    }
    auto targets = targetsFor(scopes);
    if (!targets) {
        return targets.error();
    }
    if (targets.value().size() > confirmationThreshold && !confirmedLarge) {
        return base::Error{"confirmation_required",
                           "This search includes more than 256 addresses. Confirm the range to continue."};
    }
    Discovery* expected = nullptr;
    if (!activeSearch.compare_exchange_strong(expected, this)) {
        return base::Error{"busy",
                           "A search is already running in another window. Stop it there before starting another."};
    }
    m_targets = std::move(targets).value();
    m_next = 0;
    m_completed = 0;
    m_run = std::make_unique<QObject>();
    m_running = true;
    const quint64 generation = ++m_generation;
    // No request, result or progress signal arrives until start() has returned.
    QTimer::singleShot(0, this, [this, generation] {
        if (m_running && m_generation == generation) {
            QPointer<Discovery> alive(this);
            emit progress(0, static_cast<int>(m_targets.size()));
            if (alive) {
                pump(generation);
            }
        }
    });
    return {};
}

void Discovery::cancel()
{
    if (m_running) {
        stop(true);
    }
}

void Discovery::stop(bool cancelled)
{
    m_running = false;
    ++m_generation;
    m_active.clear();
    m_run.reset();
    releaseSearch(this);
    emit finished(cancelled);
}

void Discovery::pump(quint64 generation)
{
    while (m_running && generation == m_generation && m_active.size() < maxConcurrent && m_next < m_targets.size()) {
        const int index = m_next++;
        ask(index, m_targets[index].address);
    }
}

void Discovery::ask(int index, const QUrl& address)
{
    auto* attempt = new QObject(m_run.get()); // NOLINT(cppcoreguidelines-owning-memory): owned by the run's Qt lifetime
    m_active.insert(index, attempt);
    QPointer<Discovery> scanner(this);
    const QPointer<QObject> lifetime(attempt);
    const quint64 generation = m_generation;
    m_probe(address, attempt, [scanner, lifetime, generation, index, address](KvmProbe::Found answer) {
        if (!scanner || !lifetime) {
            return;
        }
        // KvmProbe must not be destroyed inside its own callback. Queue the result on the
        // scanner, and retain a guarded attempt so cancellation/restart also invalidates it.
        QTimer::singleShot(0, scanner, [scanner, lifetime, generation, index, address, answer = std::move(answer)] {
            if (scanner && lifetime) {
                scanner->answered(generation, index, lifetime, address, answer);
            }
        });
    });
}

void Discovery::answered(quint64 generation, int index, QObject* attempt, const QUrl& address,
                         const KvmProbe::Found& answer)
{
    if (!m_running || generation != m_generation || m_active.value(index) != attempt) {
        return;
    }
    m_active.remove(index);
    delete attempt; // NOLINT(cppcoreguidelines-owning-memory): finish this run-owned Qt lifetime and its probes now
    if (answer.kind == KvmProbe::Kind::NotAsked) {
        // The installation's TLS error is not evidence that a device only supports HTTP.
        // End all traffic before informing the UI, preserving a clear failure explanation.
        m_running = false;
        ++m_generation;
        m_active.clear();
        m_run.reset();
        releaseSearch(this);
        QPointer<Discovery> alive(this);
        const quint64 stoppedGeneration = m_generation;
        emit problem(QString::fromLatin1(tlsDamagedSentence));
        if (alive && stoppedGeneration == m_generation) {
            emit finished(false);
        }
        return;
    }
    if (!recognized(answer.kind) && m_targets[index].tryHttp && address.scheme() == "https"_L1) {
        QUrl plain = address;
        plain.setScheme(u"http"_s);
        ask(index, plain);
        return;
    }
    ++m_completed;
    QPointer<Discovery> alive(this);
    if (recognized(answer.kind)) {
        // A shared certificate is not an identity: two IPs may be different cloned devices.
        // We never join them or change a saved device merely because their fingerprints match.
        emit found(Device{address, answer.kind, answer.shown, answer.trustedByWindows});
        if (!alive || !m_running || generation != m_generation) {
            return;
        }
    }
    emit progress(m_completed, static_cast<int>(m_targets.size()));
    if (!alive || !m_running || generation != m_generation) {
        return;
    }
    if (m_completed == m_targets.size()) {
        stop(false);
    } else {
        pump(generation);
    }
}

} // namespace qkvm::core
