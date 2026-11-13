#include <qkvm/core/screen_watch.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <span>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>

#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

const QString noSignalWatch = u"no_signal"_s;
const QString unchangedWatch = u"unchanged"_s;
const QString stateFile = u"screens.json"_s;
constexpr qint64 mostStateBytes = 1'000'000;
constexpr int mostMachinesKept = 10'000;

bool isAnId(const QString& id)
{
    static const QRegularExpression shape(u"\\A[A-Za-z0-9_-]{1,64}\\z"_s);
    return shape.match(id).hasMatch();
}

bool isAStamp(const QString& stamp)
{
    static const QRegularExpression shape(u"\\A[0-9]{1,16}\\z"_s);
    return shape.match(stamp).hasMatch();
}

bool isAWatch(const QString& watch)
{
    return watch == noSignalWatch || watch == unchangedWatch;
}

// The pictures kept of a KVM in `folder`: "<id>-<stamp>.jpg".
QStringList keptOf(const QString& folder, const QString& machine)
{
    QStringList found;
    const QStringList names = QDir(folder).entryList({machine + u"-*.jpg"_s}, QDir::Files);
    for (const QString& name : names) {
        if (isAStamp(name.mid(machine.size() + 1).chopped(4))) {
            found.append(name);
        }
    }
    return found;
}

QJsonObject written(const ScreenWatch::Event& event)
{
    return {
        {u"watch"_s, event.watch},
        {u"back"_s, event.back},
        {u"minutes"_s, event.minutes},
        {u"lasted"_s, static_cast<double>(event.lastedMs)},
        {u"at"_s, event.at.toUTC().toString(Qt::ISODateWithMs)},
        {u"stamp"_s, event.stamp},
        {u"pictureAt"_s, event.pictureAt.isValid() ? event.pictureAt.toUTC().toString(Qt::ISODateWithMs) : QString()}};
}

std::optional<ScreenWatch::Event> eventFrom(const QString& machine, const QJsonValue& value)
{
    const QJsonObject said = value.toObject();
    ScreenWatch::Event event;
    event.machine = machine;
    event.watch = said.value("watch"_L1).toString();
    event.back = said.value("back"_L1).toBool();
    event.minutes = ScreenWatch::minutesFrom(said.value("minutes"_L1).toInteger());
    event.lastedMs = std::max<qint64>(0, said.value("lasted"_L1).toInteger());
    event.at = QDateTime::fromString(said.value("at"_L1).toString(), Qt::ISODateWithMs);
    event.stamp = said.value("stamp"_L1).toString();
    event.pictureAt = QDateTime::fromString(said.value("pictureAt"_L1).toString(), Qt::ISODateWithMs);
    if (!isAWatch(event.watch) || !event.at.isValid() || (!event.stamp.isEmpty() && !isAStamp(event.stamp))) {
        return std::nullopt;
    }
    return event;
}

} // namespace

ScreenWatch::ScreenWatch(QObject* parent) : ScreenWatch(Times{}, parent) {}

ScreenWatch::ScreenWatch(Times times, QObject* parent) : QObject(parent), m_times(std::move(times))
{
    m_files->setMaxThreadCount(1);
    m_rounds.setInterval(std::max(50, m_times.roundMs));
    connect(&m_rounds, &QTimer::timeout, this, &ScreenWatch::round);
}

ScreenWatch::~ScreenWatch()
{
    // A round's comparing in the pool stops after the picture in hand: the program's end does not wait for the rest of
    // it (review 4's LOW 5).
    m_stop->store(true);
    // What its thread still has to do is a file or two of a few kilobytes and one picture: waited for, so long at the
    // most, on this thread (a disk that hangs holds it no longer). What is left then is the program's application
    // object's to see to the end of, after everything else of it is gone; its answers find nobody here.
    if (m_files->waitForDone(std::max(0, m_times.filesWithinMs))) {
        return;
    }
    qWarning(
        "qKVM: the screen watches' files were still being read or written as they went: left to end by themselves");
    if (QCoreApplication* const program = QCoreApplication::instance(); program != nullptr) {
        m_files.release()->setParent(program);
        return;
    }
    m_files->waitForDone(); // (no program to hand them to)
}

void ScreenWatch::filing(std::function<void()> task)
{
    m_files->start([told = m_times.whileFiling, task = std::move(task)] {
        if (told) {
            told();
        }
        task();
    });
}

qint64 ScreenWatch::now() const
{
    if (m_times.clockMs) {
        return m_times.clockMs();
    }
    static const QElapsedTimer clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return clock.elapsed();
}

QDateTime ScreenWatch::wallNow() const
{
    return m_times.wallClock ? m_times.wallClock() : QDateTime::currentDateTimeUtc();
}

int ScreenWatch::minutesFrom(qint64 value)
{
    return value <= 0 ? 0 : static_cast<int>(std::min<qint64>(value, mostMinutes));
}

QByteArray ScreenWatch::fingerprint(const uchar* rgb, int width, int height, qsizetype bytesPerLine)
{
    QByteArray squaresOf(qsizetype{across} * down, '\0');
    if (rgb == nullptr || width <= 0 || height <= 0 || bytesPerLine < qsizetype{width} * 3) {
        return squaresOf;
    }
    std::array<int, across + 1> edges{};
    for (int x = 0; x <= across; ++x) {
        edges.at(static_cast<std::size_t>(x)) = (x * width / across) * 3;
    }
    for (int row = 0; row < down; ++row) {
        const int top = row * height / down;
        const int bottom = std::max((row + 1) * height / down, top + 1);
        const int step = std::max(1, (bottom - top) / linesRead);
        std::array<qint64, across> sums{};
        int read = 0;
        for (int y = top; y < std::min(bottom, height); y += step) {
            ++read;
            const std::span<const uchar> line(rgb + (qsizetype{y} * bytesPerLine), static_cast<std::size_t>(width) * 3);
            for (int x = 0; x < across; ++x) {
                qint64 sum = 0;
                for (int at = edges.at(static_cast<std::size_t>(x)); at < edges.at(static_cast<std::size_t>(x) + 1);
                     ++at) {
                    sum += line[static_cast<std::size_t>(at)];
                }
                sums.at(static_cast<std::size_t>(x)) += sum;
            }
        }
        for (int x = 0; x < across; ++x) {
            const qint64 values =
                qint64{read} * (edges.at(static_cast<std::size_t>(x) + 1) - edges.at(static_cast<std::size_t>(x)));
            squaresOf[(row * across) + x] =
                static_cast<char>(values > 0 ? sums.at(static_cast<std::size_t>(x)) / values : 0);
        }
    }
    return squaresOf;
}

bool ScreenWatch::differs(const QByteArray& one, const QByteArray& other)
{
    if (one.size() != other.size()) {
        return true;
    }
    int moved = 0;
    for (qsizetype at = 0; at < one.size(); ++at) {
        if (std::abs(static_cast<uchar>(one.at(at)) - static_cast<uchar>(other.at(at))) > levels &&
            ++moved >= squares) {
            return true;
        }
    }
    return false;
}

void ScreenWatch::shrinkWith(std::function<std::optional<QByteArray>(const QByteArray& jpeg)> shrink)
{
    m_times.shrink = std::move(shrink);
}

void ScreenWatch::keepIn(const QString& folder)
{
    m_folder = folder;
    m_reading = !folder.isEmpty();
    if (folder.isEmpty()) {
        return;
    }
    // Read in its thread, and taken in on this one (review 4's LOW 5: up to a megabyte of JSON at a start).
    const QPointer<ScreenWatch> self(this);
    filing([self, folder] {
        QHash<QString, Machine> machines = stateIn(folder);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, folder, machines = std::move(machines)]() mutable {
                if (self != nullptr) {
                    self->takeIn(folder, std::move(machines));
                }
            },
            Qt::QueuedConnection);
    });
}

void ScreenWatch::takeIn(const QString& folder, QHash<QString, Machine> machines)
{
    if (!m_reading || folder != m_folder) {
        return; // (kept elsewhere since)
    }
    m_reading = false;
    for (const QString& gone : std::exchange(m_forgottenWhileReading, {})) {
        machines.remove(gone);
    }
    for (auto one = machines.begin(); one != machines.end(); ++one) {
        if (!m_machines.contains(one.key())) {
            m_machines.insert(one.key(), std::move(one.value()));
        }
    }
    if (std::exchange(m_writeOnceRead, false)) {
        write();
    }
    emit readBack();
}

void ScreenWatch::watch(std::function<QList<Watched>()> watched)
{
    m_who = std::move(watched);
    m_rounds.start();
}

void ScreenWatch::saw(const QString& machine, const QByteArray& picture, bool signal)
{
    if (!m_watched.contains(machine)) {
        return;
    }
    Machine& one = m_machines[machine];
    if (!signal) {
        one.darkSeen = true;
        return;
    }
    const std::optional<QSize> size = kvmd::pictureSize(picture);
    const qint64 pixels = size ? qint64{size->width()} * size->height() : 0;
    if (pixels <= 0 || !picture.startsWith("\xff\xd8")) {
        return;
    }
    const quint64 number = ++one.pictures;
    // (The smallest of those since the last round: the quickest to compare.)
    if (!one.pending || pixels <= one.pending->pixels) {
        one.pending = Seen{picture, wallNow(), pixels, number};
    }
}

void ScreenWatch::forget(const QString& machine)
{
    m_machines.remove(machine);
    m_watched.remove(machine);
    m_going.insert(machine);
    if (m_reading) {
        m_forgottenWhileReading.insert(machine); // (what is read back of it goes too)
    }
    write();
}

QStringList ScreenWatch::standing(const QString& machine) const
{
    QStringList all = m_machines.value(machine).told.values();
    all.sort();
    return all;
}

QList<ScreenWatch::Event> ScreenWatch::told(const QString& machine) const
{
    return m_machines.value(machine).said;
}

void ScreenWatch::round()
{
    // (Not before what stands is read back: what is told then is told of what was, and written over it.)
    if (m_comparing || !m_who || m_reading) {
        return;
    }
    const qint64 at = now();
    QList<Watched> machines = m_who();
    machines.removeIf([](const Watched& each) { return each.noSignal <= 0 && each.unchanged <= 0; });
    m_watched.clear();
    for (const Watched& each : std::as_const(machines)) {
        m_watched.insert(each.machine);
    }
    // Not watched any more (its watches switched off, or off the wall): nothing of it stands, but what was told.
    for (auto one = m_machines.begin(); one != m_machines.end(); ++one) {
        if (!m_watched.contains(one.key())) {
            QList<Event> said = std::move(one->said);
            *one = Machine{};
            one->said = std::move(said);
        }
    }
    QHash<QString, Seen> todo;
    for (const Watched& each : std::as_const(machines)) {
        Machine& machine = m_machines[each.machine];
        if (each.unchanged <= 0) {
            // (Watched for no signal alone: the last picture with a signal is kept in mind, for its alert.)
            if (machine.pending) {
                machine.lastLit = machine.pending;
            }
            machine.pending.reset();
            machine.reference.clear();
            machine.sameSince.reset();
            machine.lastNew.reset();
            machine.darkSeen = false;
            machine.told.remove(unchangedWatch);
        } else if (machine.pending && machine.pending->number > machine.lastCompared) {
            todo.insert(each.machine, *machine.pending);
        }
        if (each.noSignal <= 0) {
            machine.darkSince.reset();
            machine.told.remove(noSignalWatch);
        }
    }
    // At the first round, every so often from then, and for a KVM that left the wall: the pictures kept too long, or of
    // nobody, deleted. (At the first round too: a program that never runs for an hour kept them for ever, review 4's
    // LOW 4.)
    const QSet<QString> going = std::exchange(m_going, {});
    const bool sweep = !m_sweptAt || at - *m_sweptAt >= m_times.sweepEveryMs;
    if (!m_folder.isEmpty() && (sweep || !going.isEmpty())) {
        if (sweep) {
            m_sweptAt = at;
        }
        filing([folder = m_folder, going, sweep, oldest = wallNow().addMSecs(-m_times.keepDays * m_times.dayMs)] {
            const QStringList names = QDir(folder).entryList({u"*.jpg"_s}, QDir::Files);
            for (const QString& name : names) {
                const QString whose = name.section(u'-', 0, 0);
                const QFileInfo file(QDir(folder).filePath(name));
                if (going.contains(whose) || (sweep && file.lastModified().toUTC() < oldest)) {
                    QFile::remove(file.filePath());
                }
            }
        });
    }
    if (todo.isEmpty()) {
        finishRound(machines, {}, {}, at);
        return;
    }
    // Decoded and shrunk beside the interface, which goes on meanwhile; told back on it.
    m_comparing = true;
    const QPointer<ScreenWatch> self(this);
    QThreadPool::globalInstance()->start([self, todo, machines, at, most = m_times.roundMostMs,
                                          told = m_times.whileComparing, shrink = m_times.shrink, stop = m_stop] {
        if (told) {
            told();
        }
        QHash<QString, std::optional<QByteArray>> compared;
        QElapsedTimer took;
        took.start();
        for (auto one = todo.cbegin(); one != todo.cend(); ++one) {
            if (took.elapsed() > most) {
                break; // (what is left waits for the next round)
            }
            if (stop->load()) {
                break; // (this is gone: nobody waits for the rest, and the program's end does not either)
            }
            compared.insert(one.key(), shrink ? shrink(one->picture) : std::nullopt);
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, machines, compared, todo, at] {
                if (self != nullptr) {
                    self->m_comparing = false;
                    self->finishRound(machines, compared, todo, at);
                }
            },
            Qt::QueuedConnection);
    });
}

void ScreenWatch::finishRound(const QList<Watched>& machines, const QHash<QString, std::optional<QByteArray>>& compared,
                              const QHash<QString, Seen>& todo, qint64 at)
{
    QList<Event> events;
    QHash<QString, Seen> pictures;
    for (const Watched& each : machines) {
        const auto found = m_machines.find(each.machine);
        if (found == m_machines.end() || !m_watched.contains(each.machine)) {
            continue; // (taken off the wall while the pictures were compared)
        }
        if (each.noSignal > 0) {
            const QList<Event> darkOnes = dark(each, *found, at);
            for (const Event& one : darkOnes) {
                if (!one.back && found->lastLit) {
                    pictures.insert(each.machine + u'\n' + one.watch, *found->lastLit);
                }
            }
            events.append(darkOnes);
        }
        if (each.unchanged > 0) {
            const auto seen = todo.constFind(each.machine);
            const auto shrunkOne = compared.constFind(each.machine);
            const bool wasCompared = seen != todo.constEnd() && shrunkOne != compared.constEnd();
            const QList<Event> sameOnes = same(each, *found, wasCompared ? std::optional<Seen>(*seen) : std::nullopt,
                                               wasCompared ? *shrunkOne : std::nullopt, at);
            for (const Event& one : sameOnes) {
                if (!one.back && wasCompared) {
                    pictures.insert(each.machine + u'\n' + one.watch, *seen);
                }
            }
            events.append(sameOnes);
        }
    }
    if (!events.isEmpty()) {
        tell(std::move(events), std::move(pictures));
    }
}

QList<ScreenWatch::Event> ScreenWatch::dark(const Watched& each, Machine& machine, qint64 at)
{
    const auto event = [&](bool back, qint64 lasted) {
        return Event{each.machine, noSignalWatch, back, each.noSignal, lasted, wallNow(), {}, {}};
    };
    if (each.state == "no_signal"_L1) {
        if (!machine.darkSince) {
            machine.darkSince = at;
        }
        const qint64 lasted = at - *machine.darkSince;
        if (!machine.told.contains(noSignalWatch) && lasted >= each.noSignal * m_times.minuteMs) {
            machine.told.insert(noSignalWatch);
            return {event(false, lasted)};
        }
        return {};
    }
    if (each.state.isEmpty() || each.state == "offline"_L1 || each.state == "auth"_L1 || each.state == "error"_L1 ||
        each.state == "connecting"_L1) {
        // Not known to have a signal or none (it does not answer): the run of no signal is over, and what went off
        // stands.
        machine.darkSince.reset();
        return {};
    }
    const qint64 lasted = machine.darkSince ? at - *machine.darkSince : 0;
    machine.darkSince.reset();
    if (each.state == "online"_L1 && machine.told.remove(noSignalWatch)) {
        return {event(true, lasted)};
    }
    return {};
}

QList<ScreenWatch::Event> ScreenWatch::same(const Watched& each, Machine& machine, const std::optional<Seen>& seen,
                                            const std::optional<QByteArray>& found, qint64 at)
{
    const auto event = [&](bool back, qint64 lasted) {
        return Event{each.machine, unchangedWatch, back, each.unchanged, lasted, wallNow(), {}, {}};
    };
    const qint64 gap = m_times.gapMinutes * m_times.minuteMs;
    if (std::exchange(machine.darkSeen, false)) {
        // A picture without a signal: no picture of the screen. The run starts again with the next one that has it.
        machine.reference.clear();
        machine.sameSince.reset();
        machine.lastNew.reset();
    }
    if (!seen) {
        if (machine.lastNew && at - *machine.lastNew > gap) {
            // Nobody looked for a long while.
            machine.reference.clear();
            machine.sameSince.reset();
            machine.lastNew.reset();
        }
        return {};
    }
    if (machine.pending && machine.pending->number == seen->number) {
        machine.pending.reset();
    }
    machine.lastCompared = std::max(machine.lastCompared, seen->number);
    if (!found) {
        return {}; // (no JPEG, or one that could not be decoded: not compared)
    }
    machine.lastLit = seen;
    if (machine.reference.isEmpty() || !machine.sameSince || (machine.lastNew && at - *machine.lastNew > gap)) {
        machine.reference = *found;
        machine.sameSince = at;
        machine.lastNew = at;
        return {};
    }
    if (differs(machine.reference, *found)) {
        const qint64 lasted = at - *machine.sameSince;
        machine.reference = *found;
        machine.sameSince = at;
        machine.lastNew = at;
        if (machine.told.remove(unchangedWatch)) {
            return {event(true, lasted)};
        }
        return {};
    }
    machine.lastNew = at;
    const qint64 lasted = at - *machine.sameSince;
    if (!machine.told.contains(unchangedWatch) && lasted >= each.unchanged * m_times.minuteMs) {
        machine.told.insert(unchangedWatch);
        return {event(false, lasted)};
    }
    return {};
}

void ScreenWatch::tell(QList<Event> events, QHash<QString, Seen> pictures)
{
    if (m_folder.isEmpty() || pictures.isEmpty()) {
        toldNow(events);
        return;
    }
    // Stamps given here, each never one given before: milliseconds since 1970.
    QHash<QString, QString> stamps;
    for (auto one = pictures.cbegin(); one != pictures.cend(); ++one) {
        m_stamped = std::max(wallNow().toMSecsSinceEpoch(), m_stamped + 1);
        stamps.insert(one.key(), QString::number(m_stamped));
    }
    const QPointer<ScreenWatch> self(this);
    filing([self, folder = m_folder, events = std::move(events), pictures = std::move(pictures), stamps]() mutable {
        QDir().mkpath(folder);
        for (Event& event : events) {
            const QString key = event.machine + u'\n' + event.watch;
            if (event.back || !pictures.contains(key) || !isAnId(event.machine)) {
                continue;
            }
            const QString stamp = stamps.value(key);
            const QString name = event.machine + u'-' + stamp + u".jpg"_s;
            QSaveFile file(QDir(folder).filePath(name));
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(pictures.value(key).picture) != pictures.value(key).picture.size() || !file.commit()) {
                continue; // (not kept: the alert is told without it)
            }
            event.stamp = stamp;
            event.pictureAt = pictures.value(key).at;
            // In place of the one kept before.
            for (const QString& other : keptOf(folder, event.machine)) {
                if (other != name) {
                    QFile::remove(QDir(folder).filePath(other));
                }
            }
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, events] {
                if (self != nullptr) {
                    self->toldNow(events);
                }
            },
            Qt::QueuedConnection);
    });
}

void ScreenWatch::toldNow(const QList<Event>& events)
{
    for (const Event& event : events) {
        // (Taken off the wall meanwhile: nobody's.)
        if (!m_machines.contains(event.machine)) {
            continue;
        }
        QList<Event>& said = m_machines[event.machine].said;
        said.prepend(event);
        while (said.size() > mostTold) {
            said.removeLast();
        }
    }
    write();
    for (const Event& event : events) {
        if (m_machines.contains(event.machine)) {
            emit alerted(event);
        }
    }
}

void ScreenWatch::write()
{
    if (m_folder.isEmpty()) {
        return;
    }
    if (m_reading) {
        // (Written now, it would put what is known so far over what is still being read.)
        m_writeOnceRead = true;
        return;
    }
    QJsonObject machines;
    for (auto one = m_machines.cbegin(); one != m_machines.cend(); ++one) {
        if (one->told.isEmpty() && one->said.isEmpty()) {
            continue;
        }
        QJsonArray told;
        QStringList standing = one->told.values();
        standing.sort();
        for (const QString& watch : std::as_const(standing)) {
            told.append(watch);
        }
        QJsonArray said;
        for (const Event& event : one->said) {
            said.append(written(event));
        }
        machines.insert(one.key(), QJsonObject{{u"told"_s, told}, {u"said"_s, said}});
    }
    const QByteArray bytes =
        QJsonDocument(QJsonObject{{u"version"_s, 1}, {u"machines"_s, machines}}).toJson(QJsonDocument::Compact);
    filing([path = QDir(m_folder).filePath(stateFile), folder = m_folder, bytes] {
        QDir().mkpath(folder);
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) {
            file.commit();
        }
    });
}

QHash<QString, ScreenWatch::Machine> ScreenWatch::stateIn(const QString& folder)
{
    QHash<QString, Machine> machines;
    QFile file(QDir(folder).filePath(stateFile));
    if (!file.open(QIODevice::ReadOnly) || file.size() > mostStateBytes) {
        return machines;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.read(mostStateBytes)).object();
    if (root.value("version"_L1) != QJsonValue(1)) {
        return machines;
    }
    const QJsonObject kept = root.value("machines"_L1).toObject();
    int taken = 0;
    for (auto one = kept.begin(); one != kept.end() && taken < mostMachinesKept; ++one) {
        if (!isAnId(one.key())) {
            continue;
        }
        ++taken;
        Machine machine;
        for (const QJsonValue watch : one.value().toObject().value("told"_L1).toArray()) {
            if (isAWatch(watch.toString())) {
                machine.told.insert(watch.toString());
            }
        }
        for (const QJsonValue said : one.value().toObject().value("said"_L1).toArray()) {
            if (machine.said.size() >= mostTold) {
                break;
            }
            if (const std::optional<Event> event = eventFrom(one.key(), said)) {
                machine.said.append(*event);
            }
        }
        machines.insert(one.key(), machine);
    }
    return machines;
}

void ScreenWatch::askPicture(const QString& machine, const QString& stamp,
                             std::function<void(std::optional<QByteArray>)> done)
{
    if (m_folder.isEmpty() || !isAnId(machine) || !isAStamp(stamp)) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), [done = std::move(done)] { done(std::nullopt); }, Qt::QueuedConnection);
        return;
    }
    const QPointer<ScreenWatch> self(this);
    filing([self, path = QDir(m_folder).filePath(machine + u'-' + stamp + u".jpg"_s), done = std::move(done)] {
        std::optional<QByteArray> read;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= mostPictureBytes) {
            read = file.readAll();
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, done, read] {
                if (self != nullptr) {
                    done(read);
                }
            },
            Qt::QueuedConnection);
    });
}

} // namespace qkvm::core
