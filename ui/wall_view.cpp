#include "wall_view.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTime>

#include "picture_scale.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// A card narrower than this cannot be read: the wall scrolls instead of shrinking further, with
// cards of about the second width.
constexpr qreal smallestFittedCard = 260;
constexpr qreal cardWhenScrolling = 300;
// And where the number of columns was chosen, a card is at least this wide.
constexpr qreal narrowestChosenCard = 240;
constexpr int mostColumns = 6;
// How often a new picture can be asked for, in milliseconds, as the page offers it: beside the
// pace the server names.
constexpr std::array pagesPaces{500, 1000, 2000, 5000, 10000};

} // namespace

WallView::WallView(QObject* parent, int groupStaysMs) : QObject(parent)
{
    m_turn.setInterval(groupStaysMs);
    connect(&m_turn, &QTimer::timeout, this, &WallView::turnToTheNextGroup);
    m_shown.setSourceModel(&m_machines);
    connect(&m_machines, &WallModel::groupsChanged, &m_shown, &WallFilter::lookAgain);
    // Which cards the bar's choices leave decides, with the screen, which are in sight.
    connect(&m_shown, &WallFilter::countChanged, this, &WallView::inSightChanged);
    connect(&m_shown, &QAbstractItemModel::rowsMoved, this, &WallView::inSightChanged);
    // The machine that is shown large changes with the wall: what its card says, and whether it
    // is there at all.
    connect(&m_machines, &QAbstractItemModel::dataChanged, this, &WallView::enlargedCardChanged);
    connect(&m_machines, &QAbstractItemModel::modelReset, this, &WallView::lookAtTheEnlargedOne);
    connect(&m_machines, &WallModel::pickedChanged, this, &WallView::pickedChanged);
    // While the wall's order is being made: a machine that comes is last in it, and one that
    // leaves is in it no more; and which cards are in sight is by that order.
    connect(&m_machines, &QAbstractItemModel::modelReset, this, [this] {
        if (arranging()) {
            emit arrangementChanged();
        }
    });
    connect(this, &WallView::arrangementChanged, this, &WallView::inSightChanged);
    // The clock goes on when the second does: a moment after each one begins. (Every second
    // from whenever the wall was made is another thing: where that falls just before a
    // second ends, the clock shows one second twice and the next never.)
    m_everySecond.setSingleShot(true);
    m_everySecond.setTimerType(Qt::PreciseTimer);
    connect(&m_everySecond, &QTimer::timeout, this, [this] {
        emit clockChanged();
        waitForTheNextSecond();
    });
    waitForTheNextSecond();
}

void WallView::waitForTheNextSecond()
{
    constexpr int afterItBegan = 20;
    m_everySecond.start(1000 - QTime::currentTime().msec() + afterItBegan);
}

QString WallView::clock() const
{
    // With the seconds, as the page shows it, and without the time zone Windows' long form adds.
    static const QRegularExpression zone(u"\\s*t+"_s);
    QString form = QLocale::system().timeFormat(QLocale::LongFormat);
    form.remove(zone);
    return QLocale::system().toString(QTime::currentTime(), form);
}

void WallView::setFavorites(const QStringList& keys, bool available)
{
    if (m_favoriteKeys != keys || m_shortcutsAvailable != available) {
        m_favoriteKeys = keys;
        m_shortcutsAvailable = available;
        emit favoritesChanged();
    }
}

void WallView::setInSight(int first, int last)
{
    const std::pair<int, int> range{first, last};
    if (m_inSight != range) {
        m_inSight = range;
        emit inSightChanged();
    }
}

QStringList WallView::inSight() const
{
    // While the wall's order is being made, a card is where that order has it.
    const QStringList shown = arranging() ? arrangement() : m_shown.keys();
    const qsizetype first = m_inSight ? std::max(0, m_inSight->first) : 0;
    const qsizetype last = m_inSight ? std::min<qsizetype>(shown.size() - 1, m_inSight->second) : shown.size() - 1;
    return first <= last ? shown.mid(first, last - first + 1) : QStringList();
}

int WallView::rowOf(const QString& machine) const
{
    for (int row = 0; row < m_machines.rowCount(); ++row) {
        if (m_machines.index(row, 0).data(WallModel::KeyRole).toString() == machine) {
            return row;
        }
    }
    return -1;
}

void WallView::lookAtTheEnlargedOne()
{
    if (m_enlarged.isEmpty()) {
        return;
    }
    if (rowOf(m_enlarged) < 0) {
        // It has left the wall: there is nothing to show large.
        closeEnlarged();
    } else {
        emit enlargedCardChanged();
    }
}

QVariantMap WallView::enlargedCard() const
{
    const int row = m_enlarged.isEmpty() ? -1 : rowOf(m_enlarged);
    if (row < 0) {
        return {};
    }
    QVariantMap card;
    const QHash<int, QByteArray> roles = m_machines.roleNames();
    for (auto role = roles.constBegin(); role != roles.constEnd(); ++role) {
        card.insert(QString::fromLatin1(role.value()), m_machines.index(row, 0).data(role.key()));
    }
    return card;
}

void WallView::setDisplaying(bool displaying)
{
    // (A wall whose order is being made has its bar, where the way out of that is.)
    if (displaying == m_displaying || (displaying && arranging())) {
        return;
    }
    m_displaying = displaying;
    if (displaying) {
        // A wall that is only shown is not one machines are picked on, as on the page.
        setSelecting(false);
    }
    // A group's turn is a whole one, counted from when the wall was begun to be shown.
    if (m_displaying && m_rotating) {
        m_turn.start();
    } else {
        m_turn.stop();
    }
    emit displayingChanged();
}

void WallView::setRotating(bool rotating)
{
    if (rotating == m_rotating) {
        return;
    }
    m_rotating = rotating;
    if (m_displaying && m_rotating) {
        m_turn.start();
    } else {
        m_turn.stop();
    }
    emit rotatingChanged();
}

void WallView::setCovered(bool covered)
{
    if (covered != m_covered) {
        m_covered = covered;
        emit coveredChanged();
    }
}

void WallView::setUnseen(bool unseen)
{
    if (unseen != m_unseen) {
        m_unseen = unseen;
        emit unseenChanged();
    }
}

void WallView::turnToTheNextGroup()
{
    // Not while somebody is looking at one machine, or has a box or a menu open over the wall;
    // and one group takes no turns.
    const QStringList names = m_machines.groups();
    if (names.size() < 2 || !m_enlarged.isEmpty() || m_covered) {
        return;
    }
    // After the one that is shown; from "all of them", or a group the wall does not have, the first.
    m_turning = true;
    m_shown.setGroup(names.at((names.indexOf(m_shown.group()) + 1) % names.size()));
    m_turning = false;
}

void WallView::setSelecting(bool selecting)
{
    if (selecting == m_selecting || (selecting && arranging())) {
        return;
    }
    m_selecting = selecting;
    if (!selecting) {
        m_machines.pickNone();
    }
    emit selectingChanged();
}

QStringList WallView::arrangement() const
{
    if (!arranging()) {
        return {};
    }
    // (Of the source it is made for alone.)
    QStringList there;
    QSet<QString> all;
    for (int row = 0; row < m_machines.rowCount(); ++row) {
        const QModelIndex card = m_machines.index(row, 0);
        if (card.data(WallModel::SourceRole).toString() != m_arrangingOf) {
            continue;
        }
        there.append(card.data(WallModel::KeyRole).toString());
        all.insert(there.constLast());
    }
    // What was made, of the machines that are still there; and after them those that came since,
    // as they came.
    QStringList order;
    QSet<QString> placed;
    for (const QString& machine : m_made) {
        if (all.contains(machine) && !placed.contains(machine)) {
            order.append(machine);
            placed.insert(machine);
        }
    }
    for (const QString& machine : std::as_const(there)) {
        if (!placed.contains(machine)) {
            order.append(machine);
        }
    }
    return order;
}

void WallView::arrange()
{
    const auto isOfIt = [this](int row) {
        return m_machines.index(row, 0).data(WallModel::SourceRole).toString() == m_orderOf;
    };
    bool any = false;
    for (int row = 0; row < m_machines.rowCount() && !any; ++row) {
        any = isOfIt(row);
    }
    if (arranging() || !any) {
        return;
    }
    closeEnlarged();
    setSelecting(false);
    setDisplaying(false);
    // For the source it is begun for, whatever is said afterwards: what it holds is that one's.
    m_arrangingOf = m_orderOf;
    m_shown.setArranging(true, m_arrangingOf);
    emit arrangingChanged();
    emit arrangementChanged();
}

void WallView::placeAt(const QString& machine, int place)
{
    if (!arranging() || m_savingOrder) {
        return;
    }
    QStringList order = arrangement();
    const qsizetype from = order.indexOf(machine);
    const qsizetype to = std::clamp<qsizetype>(place, 0, order.size() - 1);
    if (from < 0 || from == to) {
        return;
    }
    order.move(from, to);
    m_made = order;
    emit arrangementChanged();
}

void WallView::stopArranging()
{
    if (!arranging()) {
        return;
    }
    if (m_savingOrder) {
        m_savingOrder = false;
        emit savingOrderChanged();
    }
    m_made.clear();
    m_shown.setArranging(false);
    m_arrangingOf.clear();
    emit arrangingChanged();
    emit arrangementChanged();
}

void WallView::saveOrder()
{
    if (!arranging() || m_savingOrder) {
        return;
    }
    m_savingOrder = true;
    emit savingOrderChanged();
    emit orderAsked(arrangement());
}

void WallView::orderSaved()
{
    if (!m_savingOrder) {
        return;
    }
    // An order that was just made is the one to look at.
    m_shown.setOrder(QString());
    stopArranging();
}

void WallView::orderRefused()
{
    if (m_savingOrder) {
        m_savingOrder = false;
        emit savingOrderChanged();
    }
}

void WallView::togglePicked(const QString& machine)
{
    if (m_selecting) {
        m_machines.setPicked(machine, !m_machines.isPicked(machine));
    }
}

void WallView::pickShown()
{
    if (!m_selecting) {
        return;
    }
    QStringList shown;
    for (int row = 0; row < m_shown.rowCount(); ++row) {
        shown.append(m_shown.index(row, 0).data(WallModel::KeyRole).toString());
    }
    m_machines.pick(shown);
}

void WallView::pickNone()
{
    m_machines.pickNone();
}

void WallView::enlarge(const QString& machine)
{
    // (While the wall's order is being made, a card is something to move and nothing to open.)
    if (machine == m_enlarged || machine.isEmpty() || rowOf(machine) < 0 || arranging()) {
        return;
    }
    // One machine is looked at now: picking several is over, as on the page.
    setSelecting(false);
    m_enlarged = machine;
    // (At "fit" until whoever keeps its size says otherwise, as it hears of this one.)
    setEnlargedScale(u"fit"_s);
    emit enlargedChanged();
    emit enlargedCardChanged();
}

void WallView::closeEnlarged()
{
    if (m_enlarged.isEmpty()) {
        return;
    }
    m_enlarged.clear();
    setEnlargedScale(u"fit"_s);
    emit enlargedChanged();
    emit enlargedCardChanged();
}

void WallView::chooseScale(const QString& choice)
{
    if (m_enlarged.isEmpty() || !pictureScales().contains(choice)) {
        return;
    }
    setEnlargedScale(choice);
    emit scaleChosen(m_enlarged, choice);
}

void WallView::setEnlargedScale(const QString& choice)
{
    const QString taken = pictureScales().contains(choice) ? choice : u"fit"_s;
    if (taken == m_enlargedScale) {
        return;
    }
    m_enlargedScale = taken;
    emit enlargedScaleChanged();
}

double WallView::scaleOf(const QString& choice)
{
    return ui::scaleOf(choice);
}

QRectF WallView::pictureIn(QSizeF view, QSizeF picture, double scale, double ratio, QPointF scrolled)
{
    return ui::pictureIn(view, picture.toSize(), scale, ratio, scrolled);
}

void WallView::stepEnlarged(int by)
{
    const int count = m_shown.rowCount();
    if (m_enlarged.isEmpty() || count == 0) {
        return;
    }
    // Where it is among the cards the bar's choices leave; one that they do not leave counts as the
    // one before the first.
    int at = -1;
    for (int row = 0; row < count && at < 0; ++row) {
        at = m_shown.index(row, 0).data(WallModel::KeyRole).toString() == m_enlarged ? row : -1;
    }
    const int next = at < 0 ? 0 : ((at + by) % count + count) % count;
    enlarge(m_shown.index(next, 0).data(WallModel::KeyRole).toString());
}

void WallView::setEnlargedPixels(int pixels)
{
    if (pixels != m_enlargedPixels) {
        m_enlargedPixels = pixels;
        emit enlargedPixelsChanged();
    }
}

void WallView::setLiveStream(quint32 stream)
{
    if (stream != m_liveStream) {
        m_liveStream = stream;
        emit liveStreamChanged();
    }
}

void WallView::setLiveDiagnostics(const QVariantMap& diagnostics)
{
    if (diagnostics != m_liveDiagnostics) {
        m_liveDiagnostics = diagnostics;
        emit liveDiagnosticsChanged();
    }
}

void WallView::setGraphicsCard(const QString& card)
{
    if (card != m_graphicsCard) {
        m_graphicsCard = card;
        emit graphicsCardChanged();
    }
}

void WallView::setLive(bool live)
{
    if (live != m_live) {
        m_live = live;
        emit liveChanged();
    }
}

void WallView::setCardPixels(int pixels)
{
    if (pixels != m_cardPixels) {
        m_cardPixels = pixels;
        emit cardPixelsChanged();
    }
}

QList<int> WallView::offeredPaces() const
{
    QList<int> offered(pagesPaces.begin(), pagesPaces.end());
    // Beside the page's: the server's, and each other source's.
    for (const int named : QList<int>{m_serverPace} + m_otherPaces) {
        if (named > 0 && !offered.contains(named)) {
            offered.append(named);
        }
    }
    std::ranges::sort(offered);
    return offered;
}

QVariantList WallView::paces() const
{
    QVariantList all;
    for (const int every : offeredPaces()) {
        all.append(every);
    }
    all.append(0);
    return all;
}

std::optional<int> WallView::ownPace() const
{
    if (m_paused) {
        return 0;
    }
    return m_chosenPace > 0 && offeredPaces().contains(m_chosenPace) ? std::optional<int>(m_chosenPace) : std::nullopt;
}

template <typename Change>
void WallView::changePace(Change change)
{
    const std::pair before{pace(), ownPace()};
    change();
    if (std::pair{pace(), ownPace()} != before) {
        emit paceChanged();
    }
}

void WallView::setPace(int everyMs)
{
    if (everyMs != 0 && !offeredPaces().contains(everyMs)) {
        return;
    }
    changePace([&] {
        m_paused = everyMs == 0;
        if (everyMs != 0 && everyMs != m_chosenPace) {
            m_chosenPace = everyMs;
            emit chosenPaceChanged();
        }
    });
}

void WallView::setServerPace(int everyMs)
{
    if (everyMs <= 0 || everyMs == m_serverPace) {
        return;
    }
    changePace([&] {
        m_serverPace = everyMs;
        emit pacesChanged();
    });
}

void WallView::setOtherPaces(const QList<int>& everyMs)
{
    if (everyMs == m_otherPaces) {
        return;
    }
    changePace([&] {
        m_otherPaces = everyMs;
        emit pacesChanged();
    });
}

void WallView::setChosenPace(int everyMs)
{
    changePace([&] { m_chosenPace = std::max(0, everyMs); });
}

void WallView::paceAs(const WallView& other)
{
    changePace([&] {
        m_paused = other.m_paused;
        if (m_chosenPace != other.m_chosenPace) {
            m_chosenPace = other.m_chosenPace;
            emit chosenPaceChanged();
        }
    });
}

void WallView::setColumns(int columns)
{
    const int known = std::clamp(columns, 0, mostColumns);
    if (known != m_columns) {
        m_columns = known;
        emit columnsChanged();
    }
}

QVariantMap WallView::fit(int count, qreal width, qreal height, qreal gap, int wanted)
{
    if (count <= 0 || width <= 0 || height <= 0) {
        return {{u"columns"_s, 1}, {u"cardWidth"_s, qreal(0)}};
    }
    if (wanted > 0) {
        // A chosen number is the most there are: a narrow window gets fewer.
        const int room = static_cast<int>(std::floor((width + gap) / (narrowestChosenCard + gap)));
        const int columns = std::max(1, std::min(wanted, room));
        return {{u"columns"_s, columns}, {u"cardWidth"_s, std::floor((width - gap * (columns - 1)) / columns)}};
    }
    int columns = 1;
    qreal best = 0;
    for (int tried = 1; tried <= count; ++tried) {
        const int rows = (count + tried - 1) / tried;
        const qreal byWidth = (width - gap * (tried - 1)) / tried;
        const qreal byHeight = ((height - gap * (rows - 1)) / rows - underThePicture) * 16 / 9;
        const qreal size = std::min(byWidth, byHeight);
        if (size > best) {
            best = size;
            columns = tried;
        }
    }
    if (best >= smallestFittedCard) {
        return {{u"columns"_s, columns}, {u"cardWidth"_s, std::floor(best)}};
    }
    // Too many machines for this window: readable cards, and scrolling.
    columns = std::max(1, static_cast<int>(std::floor((width + gap) / (cardWhenScrolling + gap))));
    return {{u"columns"_s, columns}, {u"cardWidth"_s, std::floor((width - gap * (columns - 1)) / columns)}};
}

} // namespace qkvm::ui
