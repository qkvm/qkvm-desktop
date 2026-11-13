#include "own_files_view.h"
#include "app_lock.h"
#include "files_view.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QFileInfo>
#include <QVariantMap>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("OwnFiles", text);
}

// A certificate's SHA-256 as the page writes one, in four lines of eight pairs: whole, and as
// it is easiest to compare by eye (the box that changes a KVM shows one so). Nothing of nothing.
QString inLinesOfEight(const QString& fingerprint)
{
    constexpr qsizetype inALine = 8;
    const QStringList pairs = fingerprint.split(u':', Qt::SkipEmptyParts);
    QStringList lines;
    for (qsizetype at = 0; at < pairs.size(); at += inALine) {
        lines.append(pairs.mid(at, inALine).join(u':'));
    }
    return lines.join(u'\n');
}

// The first two pairs of a certificate's SHA-256 and the last two, with the sign for what is
// left out between them (three dots in one, made here from its number): for the one line of a
// row, to tell one certificate from another at a glance. The whole of it stands behind the row,
// to be compared. (As it is, where there is no middle to leave out.)
QString shortened(const QString& fingerprint)
{
    constexpr qsizetype atEachEnd = 2;
    const QStringList pairs = fingerprint.split(u':', Qt::SkipEmptyParts);
    if (pairs.size() <= 2 * atEachEnd) {
        return fingerprint;
    }
    return pairs.first(atEachEnd).join(u':') + u' ' + QChar(0x2026) + u' ' + pairs.last(atEachEnd).join(u':');
}

// Why a device cannot be taken, in a few words, by the core's word for it. (Which KVM of the
// wall or which device of the file is in its way is in its verdict, behind its row.) `saidSo`: a
// "same_device" by the number it gives for itself, which is said, as the web says it.
QString cannotBeTakenInShort(const QString& whyNot, bool saidSo)
{
    if (whyNot == "same_device"_L1 && saidSo) {
        return words("Cannot be taken: says it is the same device as another, on the wall or earlier in this file");
    }
    if (whyNot == "not_yet"_L1) {
        return words("Cannot be taken: not a device this program speaks to");
    }
    if (whyNot == "invalid"_L1) {
        return words("Cannot be taken: this PC's wall does not keep it as the file has it");
    }
    if (whyNot == "duplicate"_L1) {
        return words("Cannot be taken: on the wall already, or earlier in this file");
    }
    if (whyNot == "same_device"_L1) {
        return words("Cannot be taken: the same certificate as another, on the wall or earlier in this file");
    }
    if (whyNot == "too_many"_L1) {
        return words("Cannot be taken: this PC's wall is full");
    }
    return words("Cannot be taken");
}

// Three of the parts of what stands above the list, as it says them: how many devices of the
// file are in the list; how many of them are ticked; and how many are not ticked and want a look
// (where there are any).
QString saidOfTheDevices(int devices)
{
    return devices == 1 ? words("1 device of this file is in this list.")
                        : words("%1 devices of this file are in this list.").arg(devices);
}

QString saidOfTheTicked(int ticked)
{
    return ticked == 0   ? words("None is ticked.")
           : ticked == 1 ? words("1 is ticked.")
                         : words("%1 are ticked.").arg(ticked);
}

QString saidOfThoseToLookAt(int toLookAt)
{
    return toLookAt == 1 ? words("1 is not ticked and wants a look.")
                         : words("%1 are not ticked and want a look.").arg(toLookAt);
}

// And the two that stand after them where they are so: how many cannot be taken; and that the
// file has no password for any device of the list, which would otherwise stand on every row.
QString saidOfThoseThatCannotBeTaken(int cannot)
{
    return words("%1 cannot be taken.").arg(cannot);
}

QString saidOfNoPasswordForAny()
{
    return words("The file has no password for any of them.");
}

// Of the numbers from `least` to `most`, the one that is drawn the widest, where each digit is as
// wide as `digitWidths` says (the ten, from 0 to 9) and a number as wide as its digits one beside
// the other. Told nothing of them (or not of all ten), the largest; of several as wide, the
// largest of them.
int widestOf(int least, int most, const QList<qreal>& digitWidths)
{
    constexpr int ten = 10;
    if (digitWidths.size() != ten) {
        return most;
    }
    const auto widthOf = [&digitWidths](int number) {
        qreal width = 0;
        for (int rest = number;; rest /= ten) {
            width += digitWidths.at(rest % ten);
            if (rest < ten) {
                return width;
            }
        }
    };
    int widest = most;
    qreal widestIs = widthOf(most);
    for (int number = most - 1; number >= least; --number) {
        if (const qreal width = widthOf(number); width > widestIs) {
            widest = number;
            widestIs = width;
        }
    }
    return widest;
}

// How many lines beside the devices are folded away, as the row that stands for them says it.
QString saidOfTheFold(int folded)
{
    return folded == 1 ? words("1 note about this file") : words("%1 notes about this file").arg(folded);
}

// For how long something is kept, as a sentence says it.
QString forHowLong(int milliseconds)
{
    constexpr int aSecond = 1000;
    constexpr int aMinute = 60 * aSecond;
    if (milliseconds >= aMinute) {
        const int minutes = milliseconds / aMinute;
        return minutes == 1 ? words("a minute") : words("%1 minutes").arg(minutes);
    }
    if (milliseconds >= aSecond) {
        const int seconds = milliseconds / aSecond;
        return seconds == 1 ? words("a second") : words("%1 seconds").arg(seconds);
    }
    return words("a moment");
}

// What came of a taking, as one sentence: how many of those that were ticked are on the wall.
// (That each of them is asked what it is before a password goes to it stood above the list
// they were chosen from, and is not said a second time.)
QString saidOfATaking(qsizetype taken, qsizetype ticked)
{
    if (taken == 0) {
        return ticked == 1 ? words("The KVM was not taken onto this wall.")
                           : words("None of the %1 KVMs was taken onto this wall.").arg(ticked);
    }
    if (taken < ticked) {
        return words("%1 of %2 KVMs were taken onto this wall.").arg(taken).arg(ticked);
    }
    return taken == 1 ? words("1 KVM was taken onto this wall.")
                      : words("%1 KVMs were taken onto this wall.").arg(taken);
}

// Whether a file that was refused is one that no other try will read: it is not kept then.
bool isNoFileToTryAgain(const std::string& code)
{
    return code == core::exchange::notAFile || code == core::exchange::newer || code == core::exchange::damaged ||
           code == core::exchange::tooLarge;
}

} // namespace

LaidOutList::LaidOutList(QObject* parent) : QAbstractListModel(parent) {}

int LaidOutList::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(firstDeviceRow() + m_devices.size());
}

QVariant LaidOutList::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    // The lines beside the devices first: one row that stands for them all while they are
    // folded away, and each a row once they are opened; then the devices. (Every role has a
    // value of its kind in every row: a row of the screen is told all of them, whichever it is.)
    const qsizetype row = index.row();
    const bool isBeside = row < m_notesShown;
    const bool isFold = m_fold && row == 0;
    static const Device none;
    const Device& device = isBeside || isFold ? none : m_devices.at(row - firstDeviceRow());
    switch (role) {
    case KindRole:
        return isBeside                                                             ? u"beside"_s
               : isFold                                                             ? u"fold"_s
               : device.offered.kind == core::WallExchange::Offered::Kind::Action   ? u"action"_s
               : device.offered.kind == core::WallExchange::Offered::Kind::Settings ? u"settings"_s
                                                                                    : u"device"_s;
    case TextRole:
        return isBeside ? m_beside.at(row) : isFold ? saidOfTheFold(foldedNotes()) : QString();
    case PlaceRole:
        return static_cast<int>(device.offered.place);
    case NameAndAddressRole:
        return device.called;
    case InShortRole:
        return device.inShort;
    case WantsALookRole:
        return device.wantsALook;
    case VerdictRole:
        return device.offered.verdict;
    case SaysRole:
        return device.offered.says;
    case FingerprintRole:
        return device.fingerprint;
    case CanBeTakenRole:
        return device.offered.canBeTaken;
    case TickedRole:
        return device.ticked;
    case OpenRole:
        return device.open;
    case HasTextRole:
        return device.offered.hasText;
    default:
        return {};
    }
}

QHash<int, QByteArray> LaidOutList::roleNames() const
{
    return {{KindRole, "kind"},
            {TextRole, "text"},
            {PlaceRole, "place"},
            {NameAndAddressRole, "nameAndAddress"},
            {InShortRole, "inShort"},
            {WantsALookRole, "wantsALook"},
            {VerdictRole, "verdict"},
            {SaysRole, "says"},
            {FingerprintRole, "fingerprint"},
            {CanBeTakenRole, "canBeTaken"},
            {TickedRole, "ticked"},
            {OpenRole, "open"},
            {HasTextRole, "hasText"}};
}

bool LaidOutList::tickedToBeginWith(const core::WallExchange::Offered& device)
{
    if (device.kind != core::WallExchange::Offered::Kind::Device) {
        return device.canBeTaken && device.initiallyTicked;
    }
    return device.canBeTaken && !device.passwordInTheClear && !device.nameInXnForm &&
           device.where == core::WallExchange::Where::Elsewhere;
}

QString LaidOutList::inShort(const core::WallExchange::Offered& device, bool saysOfAPassword)
{
    if (device.kind != core::WallExchange::Offered::Kind::Device) {
        return device.verdict;
    }
    using KnownBy = core::WallExchange::KnownBy;
    using Where = core::WallExchange::Where;
    if (!device.canBeTaken) {
        return cannotBeTakenInShort(device.whyNot, device.saysItIsTheSame);
    }
    QStringList said;
    if (tickedToBeginWith(device)) {
        // Nothing about it wants a second look: what it will be known by is what matters.
        if (device.knownBy == KnownBy::ItsCertificate) {
            said.append(words("Known by its certificate %1").arg(shortened(device.fingerprint)));
        } else if (device.notByWhatItShowsFirst) {
            said.append(words("Known by Windows' word, not by what it shows first"));
        } else {
            said.append(words("Known by Windows' word"));
        }
    } else {
        // Every reason it is not ticked to begin with, each in a few words: none of them is
        // left for what stands behind the row.
        if (device.passwordInTheClear) {
            said.append(words("Plain HTTP: its password goes in the clear"));
        }
        if (device.nameInXnForm) {
            said.append(words("Its address is a name in xn-- form: compare it sign for sign"));
        }
        if (device.where == Where::ThisPc) {
            said.append(words("Its address is this PC itself"));
        } else if (device.where == Where::NotRouted) {
            said.append(words("Its address is of the kind no router passes on"));
        }
    }
    if (!device.hasPassword && saysOfAPassword) {
        said.append(words("No password in the file"));
    }
    // (Between two of them, the page's own dot in the middle of the line, made here from its
    // number.)
    return said.join(u' ' + QString(QChar(0x00B7)) + u' ');
}

LaidOutList::Device* LaidOutList::deviceAt(qsizetype place, qsizetype* at)
{
    const auto found = m_atPlace.constFind(place);
    if (found == m_atPlace.constEnd()) {
        return nullptr;
    }
    if (at != nullptr) {
        *at = *found;
    }
    return &m_devices[*found];
}

void LaidOutList::show(const QStringList& beside, const QList<core::WallExchange::Offered>& devices)
{
    beginResetModel();
    m_beside = beside;
    // One row stands for the lines beside the devices until somebody opens them.
    m_fold = !beside.isEmpty();
    m_notesShown = 0;
    m_devices.clear();
    m_devices.reserve(devices.size());
    m_atPlace.clear();
    m_ticked = 0;
    m_otherParts = std::ranges::any_of(devices, [](const core::WallExchange::Offered& one) {
        return one.kind != core::WallExchange::Offered::Kind::Device;
    });
    // That the file has no password for a device is said on its row where that is not so of
    // every one that can be taken; where it is (a plain file has none), once above the list.
    const auto takeable = [](const core::WallExchange::Offered& one) {
        return one.canBeTaken && one.kind == core::WallExchange::Offered::Kind::Device;
    };
    m_noPasswordForAny = std::ranges::all_of(devices,
                                             [](const core::WallExchange::Offered& one) {
                                                 return one.kind == core::WallExchange::Offered::Kind::Device;
                                             }) &&
                         std::ranges::any_of(devices, takeable) &&
                         std::ranges::none_of(devices, [](const core::WallExchange::Offered& one) {
                             return one.canBeTaken && one.hasPassword;
                         });
    for (const core::WallExchange::Offered& offered : devices) {
        Device device;
        device.offered = offered;
        // Which device of the file it is, as the lines beside the devices count, where the core
        // hands no name and no address for it (its address cannot be shown).
        device.called = offered.nameAndAddress.isEmpty() ? words("Device %1 of the file").arg(offered.place)
                                                         : offered.nameAndAddress;
        device.inShort = inShort(offered, !m_noPasswordForAny);
        // (Of one that cannot be taken, nothing of how it would be known is shown.)
        device.fingerprint = offered.canBeTaken ? inLinesOfEight(offered.fingerprint) : QString();
        device.ticked = tickedToBeginWith(offered);
        device.wantsALook = offered.canBeTaken && !device.ticked;
        m_ticked += device.ticked ? 1 : 0;
        m_atPlace.insert(offered.place, m_devices.size());
        m_devices.append(std::move(device));
    }
    endResetModel();
}

void LaidOutList::clear()
{
    if (m_beside.isEmpty() && m_devices.isEmpty()) {
        return;
    }
    beginResetModel();
    m_beside.clear();
    m_notesShown = 0;
    m_fold = false;
    m_devices.clear();
    m_atPlace.clear();
    m_ticked = 0;
    m_noPasswordForAny = false;
    m_otherParts = false;
    endResetModel();
}

bool LaidOutList::setTicked(qsizetype place, bool ticked)
{
    qsizetype at = 0;
    Device* const device = deviceAt(place, &at);
    // A tick is to be had only where the device can be taken.
    if (device == nullptr || (ticked && !device->offered.canBeTaken) || device->ticked == ticked) {
        return false;
    }
    device->ticked = ticked;
    m_ticked += ticked ? 1 : -1;
    const QModelIndex row = index(static_cast<int>(firstDeviceRow() + at), 0);
    emit dataChanged(row, row, {TickedRole});
    return true;
}

bool LaidOutList::setOpen(qsizetype place, bool open)
{
    qsizetype at = 0;
    Device* const device = deviceAt(place, &at);
    if (device == nullptr || device->open == open) {
        return false;
    }
    device->open = open;
    const QModelIndex row = index(static_cast<int>(firstDeviceRow() + at), 0);
    emit dataChanged(row, row, {OpenRole});
    return true;
}

bool LaidOutList::openNotes()
{
    if (!m_fold) {
        return false;
    }
    // The row that stood for them goes, and each of them is a row in its place: before the
    // first device. (Gone and come in two steps: whatever shows the list is told each.)
    beginRemoveRows({}, 0, 0);
    m_fold = false;
    endRemoveRows();
    beginInsertRows({}, 0, static_cast<int>(m_beside.size()) - 1);
    m_notesShown = m_beside.size();
    endInsertRows();
    return true;
}

QList<qsizetype> LaidOutList::tickedPlaces() const
{
    QList<qsizetype> places;
    for (const Device& device : m_devices) {
        if (device.ticked) {
            places.append(device.offered.place);
        }
    }
    return places;
}

int LaidOutList::toLookAtCount() const
{
    return static_cast<int>(
        std::ranges::count_if(m_devices, [](const Device& device) { return device.wantsALook && !device.ticked; }));
}

int LaidOutList::cannotBeTakenCount() const
{
    return static_cast<int>(
        std::ranges::count_if(m_devices, [](const Device& device) { return !device.offered.canBeTaken; }));
}

QString LaidOutList::saidOfItsCounts() const
{
    // (The devices that are in this list, as devices of the file: the lines beside it number the
    // file's devices, those that were left out among them, and how many the file has is not
    // handed over by its reader. So nothing here says how many the file has.)
    const int devices = deviceCount();
    if (devices == 0) {
        return words("No device of this file is in this list.");
    }
    QStringList said{m_otherParts ? words("%1 items from this file are listed.").arg(devices)
                                  : saidOfTheDevices(devices),
                     saidOfTheTicked(m_ticked)};
    if (const int toLookAt = toLookAtCount(); toLookAt > 0) {
        said.append(saidOfThoseToLookAt(toLookAt));
    }
    if (const int cannot = cannotBeTakenCount(); cannot > 0) {
        said.append(saidOfThoseThatCannotBeTaken(cannot));
    }
    // What would otherwise stand on every row of a plain file.
    if (m_noPasswordForAny) {
        said.append(saidOfNoPasswordForAny());
    }
    return said.join(u' ');
}

QStringList LaidOutList::saidOfItsCountsToMakeRoomFor(const QList<qreal>& digitWidths) const
{
    const int devices = deviceCount();
    if (devices == 0) {
        return {saidOfItsCounts()};
    }
    // What can come to be ticked here, and what can come to want a look: any number of the
    // devices that can be taken, and any number of those that want a look while they are not
    // ticked. Of each, every form its part can take: none, one, and of the numbers from two on
    // the one drawn the widest (of forms that differ by a number alone, it needs the most lines).
    // How many want a look is said by nothing where none does.
    const int cannot = cannotBeTakenCount();
    const int takeable = devices - cannot;
    const int wantALook =
        static_cast<int>(std::ranges::count_if(m_devices, [](const Device& device) { return device.wantsALook; }));
    QStringList ticked{saidOfTheTicked(0)};
    if (takeable >= 1) {
        ticked.append(saidOfTheTicked(1));
    }
    if (takeable >= 2) {
        ticked.append(saidOfTheTicked(widestOf(2, takeable, digitWidths)));
    }
    QStringList toLookAt{QString()};
    if (wantALook >= 1) {
        toLookAt.append(saidOfThoseToLookAt(1));
    }
    if (wantALook >= 2) {
        toLookAt.append(saidOfThoseToLookAt(widestOf(2, wantALook, digitWidths)));
    }
    QStringList after;
    if (cannot > 0) {
        after.append(saidOfThoseThatCannotBeTaken(cannot));
    }
    if (m_noPasswordForAny) {
        after.append(saidOfNoPasswordForAny());
    }
    // Each with each: the two are taken as if each could be at its widest at once.
    QStringList forms;
    for (const QString& oneOf : std::as_const(ticked)) {
        for (const QString& otherOf : std::as_const(toLookAt)) {
            QStringList said{m_otherParts ? words("%1 items from this file are listed.").arg(devices)
                                          : saidOfTheDevices(devices),
                             oneOf};
            if (!otherOf.isEmpty()) {
                said.append(otherOf);
            }
            said += after;
            forms.append(said.join(u' '));
        }
    }
    return forms;
}

QString LaidOutList::nameAndAddressOf(qsizetype place) const
{
    const auto at = m_atPlace.constFind(place);
    return at == m_atPlace.constEnd() ? QString() : m_devices.at(*at).called;
}

OwnFilesView::OwnFilesView(QObject* parent) : QObject(parent)
{
    m_rest.setSingleShot(true);
    connect(&m_rest, &QTimer::timeout, this, [this] {
        m_rested = true;
        m_settled = true;
        emit changed();
    });
    m_besideRest.setSingleShot(true);
    connect(&m_besideRest, &QTimer::timeout, this, [this] {
        m_besideRested = true;
        emit changed();
    });
    m_chosenFor.setSingleShot(true);
    connect(&m_chosenFor, &QTimer::timeout, this, &OwnFilesView::theChosenFilesTimeIsOver);
    m_afterTaking.setSingleShot(true);
    connect(&m_afterTaking, &QTimer::timeout, this, [this] {
        m_heldAfterTaking = false;
        emit changed();
    });
}

OwnFilesView::~OwnFilesView()
{
    // The window goes: what the core laid out for it is forgotten, and of a file that is still
    // being read nothing is laid out.
    if (m_exchange != nullptr) {
        m_exchange->forget();
    }
}

void OwnFilesView::use(core::WallExchange* exchange)
{
    if (m_exchange != nullptr) {
        disconnect(m_exchange, nullptr, this, nullptr);
    }
    m_exchange = exchange;
    if (exchange != nullptr) {
        connect(exchange, &core::WallExchange::keptNoLonger, this, &OwnFilesView::theListsTimeIsOver);
    }
}

void OwnFilesView::restFor(int takeMs, int saveMs)
{
    m_takeRestsMs = std::max(0, takeMs);
    m_saveRestsMs = std::max(0, saveMs);
}

quint64 OwnFilesView::waitFor(Waits what)
{
    m_waits = what;
    return ++m_turn;
}

bool OwnFilesView::isAnswer(quint64 turn, Waits to)
{
    if (turn != m_turn || m_waits != to) {
        return false;
    }
    m_waits = Waits::ForNothing;
    return true;
}

void OwnFilesView::sayNothing()
{
    m_problem.clear();
    m_note.clear();
}

void OwnFilesView::fail(const QString& problem)
{
    m_problem = problem;
    m_note.clear();
    emit changed();
}

void OwnFilesView::say(const base::Error& why)
{
    if (why.code == "busy") {
        // One file at a time is worked on in the whole program: that is so, and passes. Said as
        // that, and not as something that went wrong.
        m_problem.clear();
        m_note = words("Another file is being worked on just now, and one is worked on at a time. That passes: try "
                       "again in a moment.");
        emit changed();
        return;
    }
    fail(QString::fromStdString(why.message));
}

void OwnFilesView::setBesideOnShow(bool onShow)
{
    if (onShow == m_besideOnShow) {
        return;
    }
    m_besideOnShow = onShow;
    // Whatever the moment had come to: it runs while the lines are on show, and from the
    // beginning each time they are again.
    restBesideAnew();
    // (Said a turn of the loop later: the screens say this while they read what is shown here.)
    QMetaObject::invokeMethod(this, &OwnFilesView::changed, Qt::QueuedConnection);
}

void OwnFilesView::setBoxPlace(const QRectF& place)
{
    if (place == m_boxPlace) {
        return;
    }
    // (The place a box comes to as it opens is where it was not yet: no change of its place.)
    const bool first = m_boxPlace.isNull();
    m_boxPlace = place;
    if (!first) {
        // Every control of the box stands somewhere else now, larger or smaller, under a pointer
        // that rests where it rested: whatever the moments had come to, the list's, that of what
        // takes and that of "Save" begin anew.
        restAnew();
        restBesideAnew();
    }
    // (Said a turn of the loop later: the screens say this while they read what is shown here.)
    QMetaObject::invokeMethod(this, &OwnFilesView::changed, Qt::QueuedConnection);
}

void OwnFilesView::setDigitWidths(const QList<qreal>& widths)
{
    if (widths == m_digitWidths) {
        return;
    }
    m_digitWidths = widths;
    emit countsRoomChanged();
}

void OwnFilesView::restBesideAnew()
{
    m_besideRested = false;
    m_besideRest.stop();
    if (m_besideOnShow && m_written) {
        m_besideRest.start(m_saveRestsMs);
    }
}

void OwnFilesView::setLookedAt(bool lookedAt)
{
    if (lookedAt == m_lookedAt) {
        return;
    }
    m_lookedAt = lookedAt;
    // Whatever the moment had come to: it runs while the list is looked at, and from the
    // beginning each time it is looked at again.
    restAnew();
    // (Said a turn of the loop later. The screens say this while they read what is shown here,
    // having just been told that it changed: told again in the middle of that, they would be
    // reading in a circle. What a taking is held to is as it is from this line on.)
    QMetaObject::invokeMethod(this, &OwnFilesView::changed, Qt::QueuedConnection);
}

void OwnFilesView::restAnew()
{
    m_settled = false;
    restTakingAnew();
}

void OwnFilesView::restTakingAnew()
{
    m_rested = false;
    m_rest.stop();
    if (m_lookedAt && m_laidOut != 0) {
        m_rest.start(m_takeRestsMs);
    }
}

void OwnFilesView::forgetWhatWasMade()
{
    m_written.reset();
    m_madeEncrypted = false;
    restBesideAnew();
}

void OwnFilesView::forgetTheFile()
{
    m_file.clear();
    m_fileName.clear();
    m_sealed = false;
    m_chosenFor.stop();
    m_laidOut = 0;
    m_rest.stop();
    m_rested = false;
    m_settled = false;
    m_took = false;
    m_tookSummary.clear();
    m_notTaken.clear();
    m_keptNoLonger = false;
    const bool hadAList = m_offered.rowCount() > 0;
    m_offered.clear();
    // And the core forgets what it laid out, the passwords of the file with it; of a file that
    // it is still reading it lays nothing out.
    if (m_exchange != nullptr) {
        m_exchange->forget();
    }
    if (hadAList) {
        emit tickedChanged();
    }
    // (No list, nothing to make room for.)
    emit countsRoomChanged();
    // Whatever was typed as this file's passphrase is not another file's: out of its line.
    emit fileIsAnother();
}

void OwnFilesView::reset()
{
    m_parts = {};
    if (m_lock != nullptr) {
        m_lock->cancelApproval(this);
    }
    // What came of a taking that was on show is not lost with its box, whichever way the box was
    // closed: its window's corner says it, as something to be looked at.
    const QString tookShown = m_took ? m_tookSummary : QString();
    // (Nothing is waited for: whatever is answered from now on is nobody's.)
    m_waits = Waits::ForNothing;
    sayNothing();
    forgetWhatWasMade();
    forgetTheFile();
    // A box that is closed stands nowhere: where the next stands once it is opened is no change.
    // (What was under it after a taking is held on for its moment all the same.)
    m_boxPlace = QRectF();
    emit changed();
    if (!tookShown.isEmpty()) {
        emit taken(tookShown, false);
    }
}

void OwnFilesView::close()
{
    // The screens first, which empty what was typed and say that the box was closed; and then
    // the same once more for where there are none, which does nothing a second time.
    emit closeAsked();
    reset();
}

void OwnFilesView::makeFile(const QString& passphrase)
{
    if (!mayChange() || holdsItsWindow() || m_written) {
        return;
    }
    if (m_lock != nullptr && !passphrase.isEmpty()) {
        const auto turn = waitFor(Waits::ForPassword);
        const auto wiped = std::make_shared<ExportPassword>(passphrase);
        if (!m_lock->authorizeExport(this, [this, turn, wiped] {
                if (isAnswer(turn, Waits::ForPassword) && mayChange()) {
                    makeAuthorized(wiped->text);
                }
            })) {
            m_waits = Waits::ForNothing;
            fail(words("Finish the other app password request first."));
        }
        emit changed();
        return;
    }
    makeAuthorized(passphrase);
}

void OwnFilesView::makeSelectedFile(const QString& passphrase, bool devices, bool actions, bool settings)
{
    if (!mayChange() || holdsItsWindow() || m_written) {
        return;
    }
    if (!devices && !actions && !settings) {
        fail(words("Choose at least one part for the file."));
        return;
    }
    m_parts = {devices, actions, settings};
    makeFile(passphrase);
}

QString OwnFilesView::actionText(int place) const
{
    return mayChange() && m_laidOut != 0 && m_exchange != nullptr
               ? FilesView::shownTypedText(m_exchange->actionText(m_laidOut, place))
               : QString();
}

void OwnFilesView::makeAuthorized(const QString& passphrase)
{
    if (!mayChange()) {
        return;
    }
    // (Nor from behind one of Windows' boxes that still stands, whatever became of its question.)
    if (holdsItsWindow() || m_written) {
        return;
    }
    if (m_exchange == nullptr) {
        fail(words("Files of this PC's wall cannot be made or read in this window."));
        return;
    }
    // What is no passphrase seals nothing: said at once, by the core's own rule and in its words.
    if (const std::optional<base::Error> refused =
            passphrase.isEmpty() ? std::nullopt : core::exchange::passphraseRefused(passphrase)) {
        fail(QString::fromStdString(refused->message));
        return;
    }
    sayNothing();
    m_madeEncrypted = !passphrase.isEmpty();
    const quint64 turn = waitFor(Waits::ForTheFile);
    emit changed();
    m_exchange->writeParts(
        m_parts, passphrase, this,
        [this, turn](const base::Result<core::exchange::Written>& written) { wasWritten(turn, written); });
}

bool OwnFilesView::mayChange() const
{
    return m_lock == nullptr || !m_lock->restricted();
}

void OwnFilesView::useAppLock(AppLock& lock)
{
    m_lock = &lock;
    connect(
        &lock, &AppLock::changed, this,
        [this] {
            if (!mayChange()) {
                close();
            }
        },
        Qt::DirectConnection);
    connect(&lock, &AppLock::approvalCancelled, this, [this](QObject* owner) {
        if (owner == this) {
            reset();
        }
    });
    connect(this, &QObject::destroyed, &lock, [&lock, owner = this] { lock.cancelApproval(owner); });
}

void OwnFilesView::wasWritten(quint64 turn, const base::Result<core::exchange::Written>& written)
{
    // Not waited for any more (the box was closed, the wall was left): what was made is nobody's,
    // and goes here unseen and unkept.
    if (!isAnswer(turn, Waits::ForTheFile)) {
        return;
    }
    if (!written) {
        say(written.error());
        return;
    }
    m_written = written.value();
    // (Nothing is saved for the first moment its lines are on show.)
    restBesideAnew();
    emit changed();
    emit fileWasMade();
}

void OwnFilesView::saveFile()
{
    if (!mayChange()) {
        return;
    }
    // Only a file that was made, and only with what is not in it on show, for its moment: not
    // by the click that made the file.
    if (holdsItsWindow() || !maySave()) {
        return;
    }
    sayNothing();
    const QString suggested = suggestedFileName(m_madeEncrypted);
    const quint64 turn = waitFor(Waits::ForAPlace);
    emit changed();
    // (For as long as Windows' box is there, the window under it stays, whatever becomes of the
    // question meanwhile. The program goes on while that box is open.)
    m_atTheDoor = true;
    const QPointer<OwnFilesView> here(this);
    const base::Result<QString> place =
        m_door.whereToSave ? m_door.whereToSave(suggested) : base::Result<QString>(QString());
    if (here == nullptr) {
        return;
    }
    m_atTheDoor = false;
    // Whoever comes back from Windows' box to something else than they left (the box was closed,
    // the wall was left) has asked for nothing: nothing is written.
    if (!isAnswer(turn, Waits::ForAPlace)) {
        return;
    }
    if (!place) {
        fail(QString::fromStdString(place.error().message));
        return;
    }
    // Windows' box was given up, or the lines are on show no longer: the box stays as it is.
    if (place.value().isEmpty() || !m_written || !m_besideOnShow) {
        emit changed();
        return;
    }
    // The name it is kept under says what it is, whatever came back from Windows' box: a sealed
    // file is no ".json", and a plain one no ".qkvm". Where that makes it another file than the
    // one that box asked about, nobody was asked whether a file of that name is to be written
    // over: it is not. (The one it asked about is taken to be the name it handed back as Windows
    // takes a name: "wall.qkvm." is "wall.qkvm", and is written over. What that rests on, and
    // what no test shows: asWindowsTakesIt, file_door.h.)
    const QString keptAs = namedAsWhatItIs(place.value(), m_madeEncrypted);
    const QFileInfo there(keptAs);
    if (keptAs != asWindowsTakesIt(place.value()) && there.exists()) {
        // (A folder is called a folder: nobody writes a file over one, asked or not.)
        fail((there.isDir() ? words("This file is kept under a name that says what it is: %1. A folder of that name is "
                                    "there already. Choose another name.")
                            : words("This file is kept under a name that says what it is: %1. A file of that name is "
                                    "there already, and nobody was asked whether to write over it. Choose another "
                                    "name."))
                 .arg(there.fileName()));
        return;
    }
    // The file's text, for the word that what is not in it was looked at: those lines are on
    // show at this moment, and this is the one place the text is asked for.
    const base::Result<void> kept = writeWholeFile(keptAs, m_written->file(core::exchange::NotInIt::wasLookedAt));
    if (!kept) {
        fail(QString::fromStdString(kept.error().message));
        return;
    }
    const bool encrypted = m_madeEncrypted;
    forgetWhatWasMade();
    emit changed();
    // (By the name it was written under, which may be another than Windows' box was given.)
    emit exported(saidOfAFileThatWasKept(encrypted, there.fileName()));
}

void OwnFilesView::chooseFile()
{
    if (!mayChange()) {
        return;
    }
    // (Nothing is left to choose once a taking is over: the box says what came of it, and closes.)
    if (holdsItsWindow() || m_took) {
        return;
    }
    const quint64 turn = waitFor(Waits::ForAChoice);
    emit changed();
    m_atTheDoor = true;
    const QPointer<OwnFilesView> here(this);
    const base::Result<QString> chosen = m_door.whichToOpen ? m_door.whichToOpen() : base::Result<QString>(QString());
    if (here == nullptr) {
        return;
    }
    m_atTheDoor = false;
    if (!isAnswer(turn, Waits::ForAChoice)) {
        return;
    }
    if (!chosen) {
        fail(QString::fromStdString(chosen.error().message));
        return;
    }
    // Windows' box was given up: the box stays as it is, with what it showed.
    if (chosen.value().isEmpty()) {
        emit changed();
        return;
    }
    // Another file: nothing of the one before, and what was laid out of it is forgotten.
    forgetTheFile();
    sayNothing();
    const base::Result<QByteArray> text = readChosenFile(chosen.value(), largestFileBytes);
    if (!text) {
        fail(QString::fromStdString(text.error().message));
        return;
    }
    // A file says of itself whether it is sealed, in the open: only then is there a passphrase
    // to ask for. What is no file of QKVM's at all says that here, and is not kept.
    const base::Result<bool> sealed = core::exchange::isSealed(text.value());
    if (!sealed) {
        fail(QString::fromStdString(sealed.error().message));
        return;
    }
    m_file = text.value();
    m_fileName = QFileInfo(chosen.value()).fileName();
    m_sealed = sealed.value();
    // Held, with whatever is typed for it, for no longer than what is read of it would be kept.
    m_chosenFor.start(m_exchange != nullptr ? m_exchange->keptForMs() : core::WallExchange::keptForMsAsItIs);
    emit changed();
}

void OwnFilesView::readFile(const QString& passphrase)
{
    if (!mayChange()) {
        return;
    }
    // (What is laid out is taken from, or its box is closed: it is not read over.)
    if (holdsItsWindow() || m_laidOut != 0 || m_took) {
        return;
    }
    if (m_file.isEmpty()) {
        fail(words("Choose a file first."));
        return;
    }
    if (m_exchange == nullptr) {
        fail(words("Files of this PC's wall cannot be made or read in this window."));
        return;
    }
    sayNothing();
    const quint64 turn = waitFor(Waits::ForTheReading);
    emit changed();
    m_exchange->layOut(
        m_file, m_sealed ? passphrase : QString(), this,
        [this, turn](const base::Result<core::WallExchange::LaidOut>& laidOut) { wasLaidOut(turn, laidOut); });
}

void OwnFilesView::wasLaidOut(quint64 turn, const base::Result<core::WallExchange::LaidOut>& laidOut)
{
    // Not waited for any more (the box was closed, the wall was left): nobody's. (The core was
    // told then, by forgetTheFile(), and lays nothing out of such a file: what comes here for
    // it is that it was given up.)
    if (!isAnswer(turn, Waits::ForTheReading)) {
        return;
    }
    if (!laidOut) {
        // What no other try will read is not kept; a file whose passphrase was not the one,
        // or that has to wait its turn, stays chosen.
        if (isNoFileToTryAgain(laidOut.error().code)) {
            forgetTheFile();
        }
        say(laidOut.error());
        return;
    }
    // Laid out, and nothing on the wall has changed. The bytes of the file are let go: what can
    // be taken of it is the core's to keep from here, for as long as it keeps it.
    m_file.clear();
    m_chosenFor.stop();
    m_laidOut = laidOut.value().number();
    // (The lines beside the devices stand in the same list, before the first device: the list
    // is asked for with them.)
    m_offered.show(laidOut.value().besideTheList(),
                   laidOut.value().devices(core::WallExchange::BesideTheList::wasLookedAt) +
                       laidOut.value().otherParts());
    // Nothing is taken for the first moment the list is looked at.
    restAnew();
    emit changed();
    emit tickedChanged();
    emit countsRoomChanged();
    emit fileWasRead();
}

void OwnFilesView::setTicked(int place, bool ticked)
{
    // (Not before the list has stood for its moment, whoever asks: the screens give a tick no
    // click until then, and the keyboard and a reader of the screen are held here as well.)
    if (holdsItsWindow() || !isSettled()) {
        return;
    }
    const bool noneWasTicked = m_offered.tickedCount() == 0;
    if (!m_offered.setTicked(place, ticked)) {
        return;
    }
    // From nothing ticked to something: what takes has just become something that takes, and a
    // click that was on its way to it meanwhile takes nothing. Its moment begins anew, as when
    // the notes are opened. The list's own does not: a tick moves nothing in it, nor over it
    // (what stands over the list keeps the room of the longest it can say). And from something
    // to nothing, there is nothing to take.
    if (noneWasTicked) {
        restTakingAnew();
    }
    if (noneWasTicked || m_offered.tickedCount() == 0) {
        emit changed();
    }
    emit tickedChanged();
}

void OwnFilesView::setOpen(int place, bool open)
{
    if (holdsItsWindow() || !isSettled() || !m_offered.setOpen(place, open)) {
        return;
    }
    // Every row under it has moved by what it grew or shrank, and has something else under the
    // pointer now: the list takes no press for its moment, and what takes takes none either, as
    // when the notes are opened. (The screens keep the row's own first line where it stood.)
    restAnew();
    emit changed();
}

void OwnFilesView::openNotes()
{
    if (holdsItsWindow() || !isSettled() || !m_offered.openNotes()) {
        return;
    }
    // They have just come on show, and every row has moved: nothing is taken in their first
    // moment, and nothing of the list takes a press, as in the list's own first moment.
    restAnew();
    emit changed();
}

void OwnFilesView::rowsMoved()
{
    if (m_laidOut == 0) {
        return;
    }
    // Other rows stand under a pointer that rests where it rested: what is in the list takes no
    // press for its moment. (What takes has not moved, and takes as it did; where its own moment
    // was still running, that ends with this one.)
    m_settled = false;
    m_rest.stop();
    if (m_lookedAt) {
        m_rest.start(m_takeRestsMs);
    }
    emit changed();
}

void OwnFilesView::take()
{
    if (!mayChange()) {
        return;
    }
    // Not by the click that read the file: only once the list was looked at for its moment. And
    // not while lines beside the devices are folded away that nobody opened.
    if (holdsItsWindow() || m_laidOut == 0 || !mayTake() || m_exchange == nullptr) {
        return;
    }
    const QList<qsizetype> places = m_offered.tickedPlaces();
    if (places.isEmpty()) {
        return;
    }
    sayNothing();
    // What each is called, for what is said of one that was not taken: the list goes with the
    // taking.
    QHash<qsizetype, QString> called;
    for (const qsizetype place : places) {
        called.insert(place, m_offered.nameAndAddressOf(place));
    }
    // One laying-out, one taking: whatever comes of each device, nothing is left to take.
    const quint64 number = std::exchange(m_laidOut, 0);
    const quint64 turn = waitFor(Waits::ForTheTaking);
    // (The wall says aloud that it has changed while it takes, and whoever hears that may close
    // this box, or end its window, there and then.)
    const QPointer<OwnFilesView> here(this);
    const QList<core::WallExchange::Took> took = m_exchange->take(number, places);
    if (here == nullptr) {
        return;
    }
    const bool stillShown = isAnswer(turn, Waits::ForTheTaking);
    qsizetype onTheWall = 0;
    QVariantList notTaken;
    for (const core::WallExchange::Took& one : took) {
        if (one.came) {
            ++onTheWall;
        } else {
            notTaken.append(QVariantMap{{u"device"_s, called.value(one.place)},
                                        {u"why"_s, QString::fromStdString(one.came.error().message)}});
        }
    }
    const bool hasOther =
        std::ranges::any_of(places, [](qsizetype place) { return place >= core::WallExchange::firstActionPlace; });
    const QString summary =
        hasOther ? words("Imported %1 of %2 selected devices, actions and settings.").arg(onTheWall).arg(places.size())
                 : saidOfATaking(onTheWall, places.size());
    forgetTheFile();
    if (notTaken.isEmpty() || !stillShown) {
        // All of them are on the wall: the box closes, and its window says how many. (And where
        // the box was closed in the middle of the taking, its window says what came of it: the
        // box says nothing any more.) Either way the box is gone from under the pointer that
        // pressed "Take", and what its window shows there takes no press for a moment.
        m_heldAfterTaking = true;
        m_afterTaking.start(m_takeRestsMs);
        emit changed();
        emit taken(summary, notTaken.isEmpty());
        return;
    }
    // One is not: the box stays, and says what came of each.
    m_took = true;
    m_tookSummary = summary;
    m_notTaken = notTaken;
    emit changed();
}

void OwnFilesView::theListsTimeIsOver()
{
    // (Only what is shown: after a taking, and for a box that was closed, nothing is.)
    if (m_laidOut == 0) {
        return;
    }
    sayItIsKeptNoLonger();
}

void OwnFilesView::theChosenFilesTimeIsOver()
{
    // (Only a file that is chosen and not laid out: what was laid out has the time of its list.)
    if (m_file.isEmpty() || m_laidOut != 0) {
        return;
    }
    // A reading of it that is under way is waited for no more: what it comes to is nobody's, and
    // the core is told to lay nothing out of it (forgetTheFile). (Whoever is in one of Windows'
    // boxes just now is not: what comes back from there is answered as ever.)
    if (m_waits == Waits::ForTheReading) {
        m_waits = Waits::ForNothing;
    }
    sayItIsKeptNoLonger();
}

void OwnFilesView::sayItIsKeptNoLonger()
{
    const int keptFor = m_exchange != nullptr ? m_exchange->keptForMs() : core::WallExchange::keptForMsAsItIs;
    forgetTheFile();
    m_keptNoLonger = true;
    m_problem.clear();
    m_note = saidOfWhatIsKeptNoLonger(keptFor);
    emit changed();
}

QString OwnFilesView::saidOfWhatIsKeptNoLonger(int keptForMs)
{
    return words("What is read from a file is kept for %1, and that time is over. Nothing of the file is kept: "
                 "choose it again.")
        .arg(forHowLong(keptForMs));
}

} // namespace qkvm::ui
