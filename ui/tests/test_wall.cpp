#include <memory>
#include <optional>

#include <QFile>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTime>

#include <qkvm/core/settings.h>

#include "pretend_wall.h"
#include "wall_choices.h"
#include "wall_pace.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using qkvm::ui::WallFilter;
using qkvm::ui::WallModel;
using qkvm::ui::WallView;
namespace core = qkvm::core;

namespace {

// A window as the settings hold one, said in one line: where it was, or nowhere yet, and of its
// wall the group, the order, the columns and whether the groups take turns.
core::WallWindow windowAt(const std::optional<core::WindowPlace>& place, const QString& group = {},
                          const QString& order = {}, int columns = 0, bool rotates = false)
{
    core::WallWindow window;
    window.place = place;
    window.group = group;
    window.order = order;
    window.columns = columns;
    window.rotates = rotates;
    return window;
}

// One that has not been anywhere yet.
core::WallWindow windowShowing(const QString& group, const QString& order = {}, int columns = 0)
{
    return windowAt(std::nullopt, group, order, columns);
}

} // namespace

// The wall's models, without a screen: what the bar counts, what its choices leave, and how the
// cards are laid out.
class TestWall : public QObject {
    Q_OBJECT

private:
    static qkvm::ui::WallTile tile(const QString& key, const QString& name, const QString& group, const QString& state)
    {
        qkvm::ui::WallTile made;
        made.key = key;
        made.name = name;
        made.group = group;
        made.state = state;
        return made;
    }

    // A small wall: five machines, "a" to "e" in the wall's own order, their names the other way
    // round by the alphabet, in two groups and none.
    static QList<qkvm::ui::WallTile> five()
    {
        return {tile(u"a"_s, u"Zebra"_s, u"Rack B"_s, u"online"_s), tile(u"b"_s, u"Yak"_s, u"Rack A"_s, u"offline"_s),
                tile(u"c"_s, u"Xerus"_s, u"Rack B"_s, u"online"_s), tile(u"d"_s, u"Wolf"_s, QString(), u"no_signal"_s),
                tile(u"e"_s, u"Vole"_s, u"Rack A"_s, u"online"_s)};
    }

    static QStringList namesShown(WallFilter& shown)
    {
        QStringList names;
        for (int row = 0; row < shown.rowCount(); ++row) {
            names.append(shown.index(row, 0).data(WallModel::NameRole).toString());
        }
        return names;
    }

private slots:
    void theBarCountsAsThePageDoes()
    {
        WallView wall;
        QCOMPARE(wall.machines()->all(), 0);
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        QCOMPARE(wall.machines()->all(), 15);
        // The nine with a picture, and the two servers that are on.
        QCOMPARE(wall.machines()->online(), 11);
        // The computer without a signal, and the server that is switched off.
        QCOMPARE(wall.machines()->noSignal(), 2);
        // Offline, and the one whose login was refused.
        QCOMPARE(wall.machines()->attention(), 2);
    }

    void everyCardIsInSightUntilAScreenSaysWhichAre()
    {
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        const auto key = [&](int row) { return wall.machines()->index(row, 0).data(WallModel::KeyRole).toString(); };
        QVERIFY(!key(0).isEmpty());
        QCOMPARE(wall.inSight().size(), 15);

        const QSignalSpy changed(&wall, &WallView::inSightChanged);
        wall.setInSight(4, 7);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(wall.inSight(), (QStringList{key(4), key(5), key(6), key(7)}));
        // The same said again is not news.
        wall.setInSight(4, 7);
        QCOMPARE(changed.count(), 1);
        // More than there is, is all there is; and past the end there is nothing.
        wall.setInSight(-3, 100);
        QCOMPARE(wall.inSight().size(), 15);
        wall.setInSight(20, 30);
        QCOMPARE(wall.inSight().size(), 0);
    }

    void whatTheBarTakesOffTheWallIsNotInSight()
    {
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        wall.setInSight(0, 100);
        const QSignalSpy changed(&wall, &WallView::inSightChanged);
        wall.shown()->setShow(u"attention"_s);
        QVERIFY(changed.count() >= 1);
        // The two that are left, in the wall's order; and of those, the screen shows the first.
        const QStringList left = wall.inSight();
        QStringList wanting;
        for (int row = 0; row < wall.machines()->rowCount(); ++row) {
            const QModelIndex machine = wall.machines()->index(row, 0);
            if (WallModel::countedAs(machine.data(WallModel::StateRole).toString()) == u"attention"_s) {
                wanting.append(machine.data(WallModel::KeyRole).toString());
            }
        }
        QCOMPARE(left, wanting);
        QCOMPARE(left.size(), 2);
        wall.setInSight(0, 0);
        QCOMPARE(wall.inSight(), QStringList{left.first()});
        wall.shown()->setFind(u"no machine is called this"_s);
        QCOMPARE(wall.inSight().size(), 0);
    }

    void oneMachineIsShownLargeAndTheOnesBesideItAreAStepAway()
    {
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        const auto key = [&](int row) { return wall.machines()->index(row, 0).data(WallModel::KeyRole).toString(); };
        const QSignalSpy changed(&wall, &WallView::enlargedChanged);
        QCOMPARE(wall.enlarged(), QString());
        QVERIFY(wall.enlargedCard().isEmpty());

        wall.enlarge(key(3));
        QCOMPARE(wall.enlarged(), key(3));
        QCOMPARE(changed.count(), 1);
        // Its card, as the wall's cards are given theirs.
        const QVariantMap card = wall.enlargedCard();
        QCOMPARE(card[u"name"_s].toString(), wall.machines()->index(3, 0).data(WallModel::NameRole).toString());
        QCOMPARE(card[u"key"_s].toString(), key(3));
        QCOMPARE(card[u"status"_s].toString(), u"online"_s);
        QCOMPARE(card[u"label"_s].toString(), u"Online"_s);
        QVERIFY(card.contains(u"stream"_s) && card.contains(u"ports"_s) && card.contains(u"server"_s));

        // A machine that is not on the wall is not shown, and neither is none.
        wall.enlarge(u"no machine has this name"_s);
        wall.enlarge(QString());
        QCOMPARE(wall.enlarged(), key(3));
        QCOMPARE(changed.count(), 1);

        // The one after, the one before; and round again at either end.
        wall.stepEnlarged(1);
        QCOMPARE(wall.enlarged(), key(4));
        wall.stepEnlarged(-1);
        wall.stepEnlarged(-1);
        QCOMPARE(wall.enlarged(), key(2));
        wall.enlarge(key(14));
        wall.stepEnlarged(1);
        QCOMPARE(wall.enlarged(), key(0));
        wall.stepEnlarged(-1);
        QCOMPARE(wall.enlarged(), key(14));

        wall.closeEnlarged();
        QCOMPARE(wall.enlarged(), QString());
        QVERIFY(wall.enlargedCard().isEmpty());
        // And a step from the wall goes nowhere.
        wall.stepEnlarged(1);
        QCOMPARE(wall.enlarged(), QString());
    }

    void steppingWalksTheCardsTheBarLeaves()
    {
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        wall.shown()->setShow(u"attention"_s);
        const QStringList left = wall.inSight();
        QCOMPARE(left.size(), 2);
        wall.enlarge(left[0]);
        wall.stepEnlarged(1);
        QCOMPARE(wall.enlarged(), left[1]);
        wall.stepEnlarged(1);
        QCOMPARE(wall.enlarged(), left[0]);
        // One that the bar's choices no longer leave: a step from it goes to the first that they do.
        wall.shown()->setShow(u"no_signal"_s);
        wall.stepEnlarged(1);
        QCOMPARE(wall.enlarged(), wall.inSight().constFirst());
    }

    void theEnlargedCardFollowsTheWallAndGoesWithItsMachine()
    {
        WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        const QString machine = tiles[1].key;
        wall.enlarge(machine);
        const QSignalSpy cardChanged(&wall, &WallView::enlargedCardChanged);

        // What the server says of it changes: so does what is shown large.
        tiles[1].state = u"no_signal"_s;
        tiles[1].label = u"No signal"_s;
        tiles[1].title = u"No signal"_s;
        wall.machines()->setTiles(tiles);
        QVERIFY(cardChanged.count() >= 1);
        QCOMPARE(wall.enlargedCard()[u"label"_s].toString(), u"No signal"_s);
        QCOMPARE(wall.enlarged(), machine);

        // The machine is taken off the wall: there is nothing to show large, and the wall is back.
        tiles.removeAt(1);
        wall.machines()->setTiles(tiles);
        QCOMPARE(wall.enlarged(), QString());
    }

    void theWallsGroupsAreListedByName()
    {
        WallView wall;
        const QSignalSpy changed(wall.machines(), &WallModel::groupsChanged);
        QVERIFY(wall.machines()->groups().isEmpty());
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        QCOMPARE(wall.machines()->groups(), (QStringList{u"Office"_s, u"Rack A"_s, u"Rack B"_s}));
        QCOMPARE(changed.count(), 1);
        // The same wall again is not news.
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        QCOMPARE(changed.count(), 1);
    }

    void oneGroupsMachinesAreShownOrAllOfThem()
    {
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        const QSignalSpy changed(wall.shown(), &WallFilter::groupChanged);
        wall.shown()->setGroup(u"Rack A"_s);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(wall.shown()->count(), 4);
        for (int row = 0; row < 4; ++row) {
            QCOMPARE(wall.shown()->index(row, 0).data(WallModel::GroupRole).toString(), u"Rack A"_s);
        }
        // With the bar's other choices, not instead of them.
        wall.shown()->setGroup(u"Office"_s);
        QCOMPARE(wall.shown()->count(), 3);
        wall.shown()->setShow(u"attention"_s);
        QCOMPARE(wall.shown()->count(), 2);
        wall.shown()->setShow(u"all"_s);
        wall.shown()->setGroup(QString());
        QCOMPARE(wall.shown()->count(), 15);
    }

    void aMachineIsFoundByItsGroupToo()
    {
        // As on the page, where what is typed is looked for in the name, the address and the group.
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        wall.shown()->setFind(u"rack b"_s);
        QCOMPARE(wall.shown()->count(), 5);
        for (int row = 0; row < 5; ++row) {
            QCOMPARE(wall.shown()->index(row, 0).data(WallModel::GroupRole).toString(), u"Rack B"_s);
        }
        // A machine that has the word in its name is found with the group's.
        wall.shown()->setFind(u"RACK"_s);
        QCOMPARE(wall.shown()->count(), 9);
        wall.shown()->setFind(u"office"_s);
        QCOMPARE(wall.shown()->count(), 3);
    }

    void aGroupTheWallDoesNotHaveNarrowsNothingUntilItHasIt()
    {
        // As a choice that was remembered from another wall, or for machines that are not there yet.
        WallView wall;
        wall.shown()->setGroup(u"Rack C"_s);
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        QCOMPARE(wall.shown()->count(), 15);

        // A machine is put into that group on the server: now it is the one that is shown.
        tiles[2].group = u"Rack C"_s;
        wall.machines()->setTiles(tiles);
        QCOMPARE(wall.shown()->count(), 1);
        QCOMPARE(wall.shown()->index(0, 0).data(WallModel::KeyRole).toString(), tiles[2].key);
        // And taken out of it again: all of them are back.
        tiles[2].group = u"Rack A"_s;
        wall.machines()->setTiles(tiles);
        QCOMPARE(wall.shown()->count(), 15);
    }

    void theCardsAreInTheOrderThatWasChosen()
    {
        const auto tile = [](const QString& name, const QString& group, const QString& state) {
            qkvm::ui::WallTile made;
            made.key = name;
            made.name = name;
            made.group = group;
            made.state = state;
            return made;
        };
        WallView wall;
        wall.machines()->setTiles({
            tile(u"Server 10"_s, u"Rack B"_s, u"online"_s),
            tile(u"server 2"_s, QString(), u"offline"_s),
            tile(u"Alpha"_s, u"Rack A"_s, u"no_signal"_s),
            tile(u"Server 1"_s, u"Rack B"_s, u"connecting"_s),
            tile(u"beta"_s, QString(), u"online"_s),
        });
        const auto names = [&] {
            QStringList in;
            for (int row = 0; row < wall.shown()->rowCount(); ++row) {
                in.append(wall.shown()->index(row, 0).data(WallModel::NameRole).toString());
            }
            return in;
        };
        const QStringList asArranged{u"Server 10"_s, u"server 2"_s, u"Alpha"_s, u"Server 1"_s, u"beta"_s};
        QCOMPARE(names(), asArranged);

        // By name: as a person reads them, two before ten, and capitals no matter.
        wall.shown()->setOrder(u"name"_s);
        QCOMPARE(names(), (QStringList{u"Alpha"_s, u"beta"_s, u"Server 1"_s, u"server 2"_s, u"Server 10"_s}));
        // By group: the groups by name, each by its machines' names, and those in no group last.
        wall.shown()->setOrder(u"group"_s);
        QCOMPARE(names(), (QStringList{u"Alpha"_s, u"Server 1"_s, u"Server 10"_s, u"beta"_s, u"server 2"_s}));
        // Problems first: what needs attention, what gives no picture, what is there, what is not known yet.
        wall.shown()->setOrder(u"state"_s);
        QCOMPARE(names(), (QStringList{u"server 2"_s, u"Alpha"_s, u"beta"_s, u"Server 10"_s, u"Server 1"_s}));
        // And back to how the wall was arranged; a word that is no order is that too.
        wall.shown()->setOrder(QString());
        QCOMPARE(names(), asArranged);
        wall.shown()->setOrder(u"name"_s);
        wall.shown()->setOrder(u"by colour"_s);
        QCOMPARE(wall.shown()->order(), QString());
        QCOMPARE(names(), asArranged);
    }

    void anOrderByStateFollowsTheMachines()
    {
        WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        wall.shown()->setOrder(u"state"_s);
        const QString firstBefore = wall.shown()->index(0, 0).data(WallModel::KeyRole).toString();
        // A machine that was simply there stops answering: it is among the first now.
        QCOMPARE(tiles[0].state, u"online"_s);
        QVERIFY(tiles[0].key != firstBefore);
        tiles[0].state = u"offline"_s;
        wall.machines()->setTiles(tiles);
        QStringList firstThree;
        for (int row = 0; row < 3; ++row) {
            firstThree.append(wall.shown()->index(row, 0).data(WallModel::KeyRole).toString());
        }
        QVERIFY(firstThree.contains(tiles[0].key));
    }

    void aChosenNumberOfColumnsIsTheMostThereAre()
    {
        // Three columns in a wide window: three, as wide as the window lets them be.
        QVariantMap laid = WallView::fit(15, 1576, 828, 12, 3);
        QCOMPARE(laid.value(u"columns"_s).toInt(), 3);
        QCOMPARE(laid.value(u"cardWidth"_s).toInt(), 517);
        // Six in a narrow one: as many as leave a card 240 wide.
        laid = WallView::fit(15, 500, 800, 12, 6);
        QCOMPARE(laid.value(u"columns"_s).toInt(), 2);
        QCOMPARE(laid.value(u"cardWidth"_s).toInt(), 244);
        // Never none.
        QCOMPARE(WallView::fit(15, 100, 800, 12, 4).value(u"columns"_s).toInt(), 1);
        // And nought is "Fit", as before.
        QCOMPARE(WallView::fit(15, 1576, 828, 12, 0), WallView::fit(15, 1576, 828, 12));

        WallView wall;
        const QSignalSpy changed(&wall, &WallView::columnsChanged);
        wall.setColumns(4);
        QCOMPARE(wall.columns(), 4);
        wall.setColumns(40);
        QCOMPARE(wall.columns(), 6);
        wall.setColumns(-2);
        QCOMPARE(wall.columns(), 0);
        QCOMPARE(changed.count(), 3);
    }

    void aNewPictureIsAskedForAsOftenAsWasChosen()
    {
        WallView wall;
        const QSignalSpy paceChanged(&wall, &WallView::paceChanged);
        const QSignalSpy pacesChanged(&wall, &WallView::pacesChanged);
        const QSignalSpy chosenChanged(&wall, &WallView::chosenPaceChanged);
        // Until a server has named its pace: a second. The choices are the page's, with the pause
        // at their end.
        QCOMPARE(wall.pace(), 1000);
        QCOMPARE(wall.ownPace(), std::optional<int>());
        QCOMPARE(wall.paces(), (QVariantList{500, 1000, 2000, 5000, 10000, 0}));

        // The server names three seconds: that holds, and is one of the choices, in its place.
        wall.setServerPace(3000);
        QCOMPARE(wall.pace(), 3000);
        QCOMPARE(wall.ownPace(), std::optional<int>());
        QCOMPARE(wall.paces(), (QVariantList{500, 1000, 2000, 3000, 5000, 10000, 0}));
        QCOMPARE(paceChanged.count(), 1);
        QCOMPARE(pacesChanged.count(), 1);
        wall.setServerPace(3000);
        wall.setServerPace(0);
        wall.setServerPace(-250);
        QCOMPARE(wall.pace(), 3000);
        QCOMPARE(pacesChanged.count(), 1);

        // One of them is chosen: that holds, whoever asks for the pictures is told, and it is the
        // one to remember.
        wall.setPace(500);
        QCOMPARE(wall.pace(), 500);
        QCOMPARE(wall.ownPace(), std::optional<int>(500));
        QCOMPARE(wall.chosenPace(), 500);
        QCOMPARE(paceChanged.count(), 2);
        QCOMPARE(chosenChanged.count(), 1);
        // What is not among the choices is not taken.
        wall.setPace(1234);
        wall.setPace(-1);
        QCOMPARE(wall.pace(), 500);
        QCOMPARE(paceChanged.count(), 2);

        // A pause is for now: what was chosen stays the chosen one.
        wall.setPace(0);
        QCOMPARE(wall.pace(), 0);
        QCOMPARE(wall.ownPace(), std::optional<int>(0));
        QCOMPARE(wall.chosenPace(), 500);
        QCOMPARE(paceChanged.count(), 3);
        QCOMPARE(chosenChanged.count(), 1);
        // It ends with a choice: the same again, or another.
        wall.setPace(500);
        QCOMPARE(wall.pace(), 500);
        QCOMPARE(paceChanged.count(), 4);
        QCOMPARE(chosenChanged.count(), 1);

        // The server's own pace, chosen in so many words. When the server names another, that one
        // is no longer among the choices: the server's new one holds, and the old is kept.
        wall.setPace(3000);
        QCOMPARE(wall.ownPace(), std::optional<int>(3000));
        QCOMPARE(chosenChanged.count(), 2);
        wall.setServerPace(250);
        QCOMPARE(wall.paces(), (QVariantList{250, 500, 1000, 2000, 5000, 10000, 0}));
        QCOMPARE(wall.pace(), 250);
        QCOMPARE(wall.ownPace(), std::optional<int>());
        QCOMPARE(wall.chosenPace(), 3000);
        // Whoever asks for the pictures is told when what they are told changes, also where the
        // pace is the same number: the server's two seconds are not this PC's two seconds.
        wall.setPace(2000);
        const qsizetype told = paceChanged.count();
        wall.setServerPace(2000);
        QCOMPARE(wall.pace(), 2000);
        QCOMPARE(wall.ownPace(), std::optional<int>(2000));
        QCOMPARE(paceChanged.count(), told);
        wall.setChosenPace(0);
        QCOMPARE(wall.pace(), 2000);
        QCOMPARE(wall.ownPace(), std::optional<int>());
        QCOMPARE(paceChanged.count(), told + 1);

        // A pace that was remembered: used where it is among the choices, and otherwise not.
        WallView other;
        other.setChosenPace(5000);
        QCOMPARE(other.pace(), 5000);
        other.setChosenPace(7000);
        QCOMPARE(other.pace(), 1000);
        QCOMPARE(other.chosenPace(), 7000);
        other.setServerPace(7000);
        QCOMPARE(other.pace(), 7000);
        QCOMPARE(other.ownPace(), std::optional<int>(7000));
    }

    void aWallThatIsOnlyShownLetsItsGroupsTakeTurns()
    {
        // (A group's turn is over after 60 ms here, and not after twenty seconds.)
        WallView wall(nullptr, 60);
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        QCOMPARE(wall.machines()->groups(), (QStringList{u"Office"_s, u"Rack A"_s, u"Rack B"_s}));
        QSignalSpy groupChanged(wall.shown(), &qkvm::ui::WallFilter::groupChanged);
        QVERIFY(!wall.displaying());
        QVERIFY(!wall.rotating());

        // Display mode ends the picking of machines, and by itself changes nothing of what is shown.
        wall.setSelecting(true);
        const QSignalSpy displayingChanged(&wall, &WallView::displayingChanged);
        wall.setDisplaying(true);
        QVERIFY(wall.displaying());
        QVERIFY(!wall.selecting());
        QCOMPARE(displayingChanged.count(), 1);
        wall.setDisplaying(true);
        QCOMPARE(displayingChanged.count(), 1);
        QTest::qWait(200);
        QCOMPARE(groupChanged.count(), 0);

        // The groups take turns: from all of them to the first, and round again after the last.
        wall.setRotating(true);
        QVERIFY(wall.rotating());
        QCOMPARE(wall.shown()->group(), QString());
        QTRY_COMPARE(wall.shown()->group(), u"Office"_s);
        QTRY_COMPARE(wall.shown()->group(), u"Rack A"_s);
        QTRY_COMPARE(wall.shown()->group(), u"Rack B"_s);
        QTRY_COMPARE(wall.shown()->group(), u"Office"_s);
        // (One at a time, in their order: nothing was shown out of its turn.)
        QVERIFY(groupChanged.count() >= 4);

        // While one machine is looked at, they wait; and go on afterwards.
        wall.enlarge(wall.shown()->index(0, 0).data(qkvm::ui::WallModel::KeyRole).toString());
        QVERIFY(!wall.enlarged().isEmpty());
        qsizetype turns = groupChanged.count();
        QTest::qWait(300);
        QCOMPARE(groupChanged.count(), turns);
        wall.closeEnlarged();
        QTRY_VERIFY(groupChanged.count() > turns);
        // The same while something lies over the wall.
        wall.setCovered(true);
        QVERIFY(wall.covered());
        turns = groupChanged.count();
        QTest::qWait(300);
        QCOMPARE(groupChanged.count(), turns);
        wall.setCovered(false);
        QTRY_VERIFY(groupChanged.count() > turns);

        // Not chosen any more: the group that is shown stays.
        wall.setRotating(false);
        const QString stays = wall.shown()->group();
        QTest::qWait(250);
        QCOMPARE(wall.shown()->group(), stays);
        // Chosen, and the wall is not only shown: nothing takes turns on a wall somebody works at.
        // Not when the wall stops being only shown, and not when it is chosen while the wall is
        // not, as it is when the program starts with what it remembers.
        wall.setRotating(true);
        wall.setDisplaying(false);
        QTest::qWait(250);
        QCOMPARE(wall.shown()->group(), stays);
        wall.setRotating(false);
        wall.setRotating(true);
        QTest::qWait(250);
        QCOMPARE(wall.shown()->group(), stays);
        wall.setDisplaying(true);
        QTRY_VERIFY(wall.shown()->group() != stays);

        // A group that the wall does not have is followed by the first; one group takes no turns.
        wall.shown()->setGroup(u"Cellar"_s);
        QTRY_COMPARE(wall.shown()->group(), u"Office"_s);
        QList<qkvm::ui::WallTile> oneGroup = qkvm::ui::pretendWall();
        for (qkvm::ui::WallTile& tile : oneGroup) {
            tile.group = tile.group.isEmpty() ? QString() : u"Office"_s;
        }
        wall.machines()->setTiles(oneGroup);
        QCOMPARE(wall.machines()->groups(), QStringList{u"Office"_s});
        wall.shown()->setGroup(QString());
        QTest::qWait(250);
        QCOMPARE(wall.shown()->group(), QString());
    }

    void howTheWallIsShownIsRememberedOnThisPc()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        {
            core::Settings settings(file);
            core::WindowSlot kept(settings, 0);
            WallView wall;
            wall.machines()->setTiles(qkvm::ui::pretendWall());
            const qkvm::ui::WallChoices choices(wall, kept);
            qkvm::ui::WallPace pace(settings);
            pace.add(wall);
            // A PC that never ran the program: all groups, as arranged, as many columns as fit.
            QCOMPARE(wall.shown()->group(), QString());
            QCOMPARE(wall.shown()->order(), QString());
            QCOMPARE(wall.columns(), 0);
            QVERIFY(!QFile::exists(file));

            // Each choice is written down when it is made, by itself.
            wall.shown()->setGroup(u"Rack B"_s);
            QCOMPARE(core::Settings(file).remembered().windows.value(0).group, u"Rack B"_s);
            wall.shown()->setOrder(u"name"_s);
            QCOMPARE(core::Settings(file).remembered().windows.value(0).order, u"name"_s);
            wall.setColumns(3);
            QCOMPARE(core::Settings(file).remembered().windows.value(0).columns, 3);
            QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 0);
            wall.setPace(2000);
            QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 2000);
            // What is for the moment is not written down: a pause of the pictures no more than
            // what was typed into "Find a KVM".
            wall.shown()->setShow(u"attention"_s);
            wall.shown()->setFind(u"nas"_s);
            wall.setPace(0);
            QCOMPARE(wall.pace(), 0);
            QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 2000);
            // That the groups take turns on a wall that is only shown.
            QVERIFY(!core::Settings(file).remembered().windows.value(0).rotates);
            wall.setRotating(true);
            QVERIFY(core::Settings(file).remembered().windows.value(0).rotates);
        }
        core::Settings settings(file);
        QCOMPARE(settings.remembered().windows.value(0).group, u"Rack B"_s);
        QCOMPARE(settings.remembered().windows.value(0).order, u"name"_s);
        QCOMPARE(settings.remembered().windows.value(0).columns, 3);

        // The next start: the wall is shown as it was left.
        core::WindowSlot kept(settings, 0);
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        const qkvm::ui::WallChoices choices(wall, kept);
        qkvm::ui::WallPace pace(settings);
        pace.add(wall);
        QCOMPARE(wall.shown()->group(), u"Rack B"_s);
        QCOMPARE(wall.shown()->order(), u"name"_s);
        QCOMPARE(wall.columns(), 3);
        QCOMPARE(wall.shown()->count(), 5);
        QCOMPARE(wall.shown()->show(), u"all"_s);
        QCOMPARE(wall.shown()->find(), QString());
        // A picture every two seconds, and no pause.
        QCOMPARE(wall.pace(), 2000);
        QVERIFY(wall.rotating());
        // The wall itself is not only shown: that is for whoever chooses it, each time.
        QVERIFY(!wall.displaying());
        // What was remembered stays so when something else is chosen now.
        wall.setColumns(4);
        QCOMPARE(core::Settings(file).remembered().windows.value(0).columns, 4);
        QCOMPARE(core::Settings(file).remembered().windows.value(0).group, u"Rack B"_s);
        QCOMPARE(core::Settings(file).remembered().windows.value(0).order, u"name"_s);
        QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 2000);
        QVERIFY(core::Settings(file).remembered().windows.value(0).rotates);
    }

    void aGroupWhoseTurnCameIsNotTheGroupThatWasChosen()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        core::WindowSlot kept(settings, 0);
        WallView wall(nullptr, 60);
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        const qkvm::ui::WallChoices choices(wall, kept);
        wall.shown()->setGroup(u"Rack B"_s);
        QCOMPARE(core::Settings(file).remembered().windows.value(0).group, u"Rack B"_s);

        // Shown, with the groups taking turns: what is remembered is the group that was chosen.
        wall.setDisplaying(true);
        wall.setRotating(true);
        QTRY_COMPARE(wall.shown()->group(), u"Office"_s);
        QTRY_COMPARE(wall.shown()->group(), u"Rack A"_s);
        QCOMPARE(core::Settings(file).remembered().windows.value(0).group, u"Rack B"_s);
        // Also when something else is written down meanwhile.
        wall.setColumns(2);
        QCOMPARE(core::Settings(file).remembered().windows.value(0).columns, 2);
        QCOMPARE(core::Settings(file).remembered().windows.value(0).group, u"Rack B"_s);
        // A group that somebody chooses is the chosen one, whatever is taking turns.
        wall.setRotating(false);
        wall.shown()->setGroup(u"Office"_s);
        QCOMPARE(core::Settings(file).remembered().windows.value(0).group, u"Office"_s);
    }

    void eachWindowsWallIsRememberedByItself()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        {
            core::Settings settings(file);
            core::WindowSlot firstKept(settings, 0);
            core::WindowSlot secondKept(settings, 1);
            WallView first;
            WallView second;
            first.machines()->setTiles(qkvm::ui::pretendWall());
            second.machines()->setTiles(qkvm::ui::pretendWall());
            const qkvm::ui::WallChoices firstChoices(first, firstKept);
            const qkvm::ui::WallChoices secondChoices(second, secondKept);
            // What is chosen in one window is that window's: the other's wall stays as it is.
            second.shown()->setGroup(u"Rack B"_s);
            second.setColumns(2);
            first.shown()->setOrder(u"name"_s);
            QCOMPARE(first.shown()->group(), QString());
            QCOMPARE(first.columns(), 0);
            QCOMPARE(second.shown()->order(), QString());
        }
        core::Settings settings(file);
        QCOMPARE(settings.remembered().windows.size(), 2);
        QCOMPARE(settings.remembered().windows.at(0), (windowShowing({}, u"name"_s)));
        QCOMPARE(settings.remembered().windows.at(1), (windowShowing(u"Rack B"_s, {}, 2)));
        // The next start: each window's wall as it was left.
        core::WindowSlot secondKept(settings, 1);
        WallView second;
        second.machines()->setTiles(qkvm::ui::pretendWall());
        const qkvm::ui::WallChoices secondChoices(second, secondKept);
        QCOMPARE(second.shown()->group(), u"Rack B"_s);
        QCOMPARE(second.columns(), 2);
        QCOMPARE(second.shown()->order(), QString());
    }

    void howOftenPicturesAreAskedForIsOneChoiceForAllTheWalls()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        core::Remembered before;
        before.wallPictureEveryMs = 5000;
        QVERIFY(settings.remember(before).ok());

        qkvm::ui::WallPace pace(settings);
        WallView first;
        WallView second;
        // The first wall takes what was remembered; the next takes what holds.
        pace.add(first);
        QCOMPARE(first.pace(), 5000);
        pace.add(second);
        QCOMPARE(second.pace(), 5000);
        QCOMPARE(second.chosenPace(), 5000);

        // Chosen on one, it holds on the other, and is written down.
        const QSignalSpy secondChanged(&second, &WallView::paceChanged);
        first.setPace(2000);
        QCOMPARE(second.pace(), 2000);
        QCOMPARE(secondChanged.count(), 1);
        QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 2000);
        second.setPace(500);
        QCOMPARE(first.pace(), 500);
        QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 500);

        // A pause on one is a pause on all, and is not written down: it is for now.
        second.setPace(0);
        QCOMPARE(first.pace(), 0);
        QCOMPARE(first.ownPace(), std::optional<int>(0));
        QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 500);
        // A wall that comes while the pictures are paused is paused, and knows the pace under it.
        auto third = std::make_unique<WallView>();
        pace.add(*third);
        QCOMPARE(third->pace(), 0);
        QCOMPARE(third->chosenPace(), 500);
        // The pause ends on the one that came last: on all.
        third->setPace(500);
        QCOMPARE(first.pace(), 500);
        QCOMPARE(second.pace(), 500);
        QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 500);

        // A wall whose window was closed is let go of: the others go on choosing.
        third.reset();
        first.setPace(1000);
        QCOMPARE(second.pace(), 1000);
        QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 1000);
        // Import changes the stored choice. Existing windows follow it without ending a pause.
        core::Remembered imported = settings.remembered();
        imported.wallPictureEveryMs = 2750;
        QVERIFY(settings.remember(imported));
        first.setServerPace(2750);
        second.setServerPace(2750);
        pace.reload();
        QCOMPARE(first.pace(), 2750);
        QCOMPARE(second.pace(), 2750);
        first.setPace(0);
        imported.wallPictureEveryMs = 5000;
        QVERIFY(settings.remember(imported));
        pace.reload();
        QCOMPARE(first.pace(), 0);
        QCOMPARE(second.pace(), 0);
        QCOMPARE(second.chosenPace(), 5000);
    }

    void theClockGoesOnWhenTheSecondDoes()
    {
        // (A second after whenever the wall was made is another moment: a clock that goes on
        // just before a second ends shows that second twice, and the next one never.)
        WallView wall;
        QList<int> intoTheSecond;
        QStringList said;
        connect(&wall, &WallView::clockChanged, this, [&] {
            intoTheSecond.append(QTime::currentTime().msec());
            said.append(wall.clock());
        });
        QTRY_COMPARE_WITH_TIMEOUT(intoTheSecond.size(), 3, 5000);
        for (const int ms : std::as_const(intoTheSecond)) {
            QVERIFY2(ms < 400, qPrintable(QString::number(ms)));
        }
        // Three times, three other seconds.
        QCOMPARE(QSet<QString>(said.cbegin(), said.cend()).size(), 3);
    }

    void theWallsOwnOrderIsMadeByHand()
    {
        WallView wall;
        QList<qkvm::ui::WallTile> tiles = five();
        wall.machines()->setTiles(tiles);
        // The bar's choices: one group, by name; and a machine is picked.
        wall.shown()->setGroup(u"Rack B"_s);
        wall.shown()->setOrder(u"name"_s);
        wall.setSelecting(true);
        wall.togglePicked(u"a"_s);
        QCOMPARE(wall.shown()->keys(), (QStringList{u"c"_s, u"a"_s}));
        QCOMPARE(wall.arrangement(), QStringList());

        const QSignalSpy changed(&wall, &WallView::arrangingChanged);
        const QSignalSpy orderSaid(&wall, &WallView::arrangementChanged);
        wall.arrange();
        QVERIFY(wall.arranging());
        QCOMPARE(changed.count(), 1);
        // The order it begins with is said to whoever draws the cards; and nothing is picked any
        // more.
        QCOMPARE(orderSaid.count(), 1);
        QVERIFY(!wall.selecting());
        QCOMPARE(wall.picked(), 0);
        // (Begun with a machine shown large, or on a wall that is only shown: neither is so any
        // more. The cards are what is worked on, and the way out is at the foot of the wall.)
        wall.stopArranging();
        wall.enlarge(u"c"_s);
        wall.arrange();
        QCOMPARE(wall.enlarged(), QString());
        wall.stopArranging();
        wall.setDisplaying(true);
        wall.arrange();
        QVERIFY(!wall.displaying());
        QVERIFY(wall.arranging());
        QCOMPARE(changed.count(), 5);
        // Every machine, in the wall's own order, whatever the bar had chosen; and none is picked.
        QCOMPARE(wall.arrangement(), (QStringList{u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
        QCOMPARE(wall.shown()->keys(), (QStringList{u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
        QCOMPARE(wall.shown()->count(), 5);
        QVERIFY(!wall.selecting());
        QCOMPARE(wall.picked(), 0);
        // Begun twice, it is begun once.
        wall.arrange();
        QCOMPARE(changed.count(), 5);
        // Nothing else is done on the wall meanwhile: a card is something to move.
        wall.enlarge(u"b"_s);
        QCOMPARE(wall.enlarged(), QString());
        wall.setSelecting(true);
        QVERIFY(!wall.selecting());
        wall.setDisplaying(true);
        QVERIFY(!wall.displaying());

        // A machine is put at a place: the others close up behind it and make room for it.
        wall.placeAt(u"d"_s, 0);
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"a"_s, u"b"_s, u"c"_s, u"e"_s}));
        wall.placeAt(u"a"_s, 3);
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"b"_s, u"c"_s, u"a"_s, u"e"_s}));
        // Before the first is the first, after the last the last; and nobody is nowhere.
        wall.placeAt(u"e"_s, -5);
        QCOMPARE(wall.arrangement(), (QStringList{u"e"_s, u"d"_s, u"b"_s, u"c"_s, u"a"_s}));
        wall.placeAt(u"e"_s, 99);
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"b"_s, u"c"_s, u"a"_s, u"e"_s}));
        wall.placeAt(u"nobody"_s, 1);
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"b"_s, u"c"_s, u"a"_s, u"e"_s}));

        // The wall changes meanwhile: machines that come are last, in the order they came in, one
        // that left is gone, and what was made of the others holds.
        tiles.removeAt(1);
        tiles.append(tile(u"g"_s, u"Tapir"_s, QString(), u"online"_s));
        tiles.append(tile(u"f"_s, u"Unau"_s, QString(), u"online"_s));
        const QSignalSpy cameOrWent(&wall, &WallView::arrangementChanged);
        wall.machines()->setTiles(tiles);
        QVERIFY(cameOrWent.count() >= 1);
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"c"_s, u"a"_s, u"e"_s, u"g"_s, u"f"_s}));
        tiles.removeAt(4);
        wall.machines()->setTiles(tiles);
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"c"_s, u"a"_s, u"e"_s, u"f"_s}));
        wall.placeAt(u"f"_s, 1);
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"f"_s, u"c"_s, u"a"_s, u"e"_s}));

        // "Save this order": whoever speaks to the server is asked once, with what is shown.
        // Which cards are in sight, for their pictures, is by the order that is being made.
        wall.setInSight(0, 1);
        QCOMPARE(wall.inSight(), (QStringList{u"d"_s, u"f"_s}));
        wall.setInSight(0, 99);
        // And each move is said once, to whoever draws the cards.
        const QSignalSpy moved(&wall, &WallView::arrangementChanged);
        wall.placeAt(u"e"_s, 3);
        wall.placeAt(u"e"_s, 3);
        QCOMPARE(moved.count(), 1);
        wall.placeAt(u"e"_s, 4);
        QCOMPARE(moved.count(), 2);
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"f"_s, u"c"_s, u"a"_s, u"e"_s}));

        const QSignalSpy asked(&wall, &WallView::orderAsked);
        const QSignalSpy saving(&wall, &WallView::savingOrderChanged);
        wall.saveOrder();
        QCOMPARE(asked.count(), 1);
        QCOMPARE(asked.at(0).at(0).toStringList(), (QStringList{u"d"_s, u"f"_s, u"c"_s, u"a"_s, u"e"_s}));
        QVERIFY(wall.isSavingOrder());
        QCOMPARE(saving.count(), 1);
        // While it is on its way nothing is moved, and it is not sent a second time.
        wall.placeAt(u"a"_s, 0);
        wall.saveOrder();
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"f"_s, u"c"_s, u"a"_s, u"e"_s}));
        QCOMPARE(asked.count(), 1);
        // Refused: as it was made, to be sent again.
        wall.orderRefused();
        QVERIFY(!wall.isSavingOrder());
        QVERIFY(wall.arranging());
        QCOMPARE(wall.arrangement(), (QStringList{u"d"_s, u"f"_s, u"c"_s, u"a"_s, u"e"_s}));
        wall.saveOrder();
        QCOMPARE(asked.count(), 2);

        // Taken: the wall is given its machines in that order, and is left in its own order, which
        // is the one just made. The group is the bar's again; the order by name is not.
        wall.machines()->setTiles({tiles.at(2), tiles.at(4), tiles.at(1), tiles.at(0), tiles.at(3)});
        wall.orderSaved();
        QVERIFY(!wall.arranging());
        QVERIFY(!wall.isSavingOrder());
        QCOMPARE(changed.count(), 6);
        QCOMPARE(wall.shown()->order(), QString());
        QCOMPARE(wall.shown()->group(), u"Rack B"_s);
        QCOMPARE(wall.shown()->keys(), (QStringList{u"c"_s, u"a"_s}));
        wall.shown()->setGroup(QString());
        QCOMPARE(wall.shown()->keys(), (QStringList{u"d"_s, u"f"_s, u"c"_s, u"a"_s, u"e"_s}));
        // What the server says later of an order that is not on its way is nothing.
        wall.shown()->setOrder(u"name"_s);
        wall.orderSaved();
        wall.orderRefused();
        QCOMPARE(wall.shown()->order(), u"name"_s);
    }

    void anOrderThatIsGivenUpLeavesTheWallAsItWas()
    {
        WallView wall;
        const QSignalSpy changed(&wall, &WallView::arrangingChanged);
        // A wall without a machine has nothing to arrange, and nothing is said to begin.
        wall.arrange();
        QVERIFY(!wall.arranging());
        QCOMPARE(changed.count(), 0);
        wall.machines()->setTiles(five());
        wall.shown()->setOrder(u"name"_s);
        wall.shown()->setShow(u"online"_s);
        const QStringList before = wall.shown()->keys();
        QCOMPARE(before, (QStringList{u"e"_s, u"c"_s, u"a"_s}));

        // Not begun: nothing to move, to save or to leave.
        wall.placeAt(u"a"_s, 4);
        wall.saveOrder();
        wall.stopArranging();
        QCOMPARE(wall.shown()->keys(), before);
        QCOMPARE(changed.count(), 0);

        wall.arrange();
        wall.placeAt(u"e"_s, 0);
        QCOMPARE(wall.arrangement(), (QStringList{u"e"_s, u"a"_s, u"b"_s, u"c"_s, u"d"_s}));
        // (Every card is there meanwhile, as the machines came: where each is drawn is by the
        // order being made, and a card that is moved is still the card it was. An order that is
        // chosen for the bar meanwhile is the bar's afterwards, and not before.)
        QCOMPARE(wall.shown()->keys(), (QStringList{u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
        wall.shown()->setOrder(u"group"_s);
        QCOMPARE(wall.shown()->keys(), (QStringList{u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
        wall.shown()->setOrder(u"name"_s);
        wall.stopArranging();
        QVERIFY(!wall.arranging());
        QCOMPARE(changed.count(), 2);
        // As the bar had it: by name, and only what is online.
        QCOMPARE(wall.shown()->keys(), before);
        QCOMPARE(wall.shown()->order(), u"name"_s);
        QCOMPARE(wall.shown()->show(), u"online"_s);

        // Given up while it was on its way: an answer that comes after is nothing to this wall.
        wall.arrange();
        wall.placeAt(u"d"_s, 0);
        wall.saveOrder();
        QVERIFY(wall.isSavingOrder());
        wall.stopArranging();
        QVERIFY(!wall.isSavingOrder());
        wall.orderSaved();
        QCOMPARE(wall.shown()->order(), u"name"_s);
        QCOMPARE(wall.shown()->keys(), before);
    }

    void machinesArePickedOnlyWhileTheyAreBeingPicked()
    {
        WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        const QSignalSpy selecting(&wall, &WallView::selectingChanged);
        const QSignalSpy picked(&wall, &WallView::pickedChanged);
        QVERIFY(!wall.selecting());
        // Not while the wall is only looked at.
        wall.togglePicked(tiles[1].key);
        wall.pickShown();
        QCOMPARE(wall.picked(), 0);
        QCOMPARE(picked.count(), 0);

        wall.setSelecting(true);
        QCOMPARE(selecting.count(), 1);
        wall.togglePicked(tiles[3].key);
        wall.togglePicked(tiles[1].key);
        QCOMPARE(wall.picked(), 2);
        QCOMPARE(picked.count(), 2);
        // In the wall's order, whatever order they were picked in; and each card says it of itself.
        QCOMPARE(wall.pickedMachines(), (QStringList{tiles[1].key, tiles[3].key}));
        QVERIFY(wall.machines()->index(1, 0).data(WallModel::PickedRole).toBool());
        QVERIFY(!wall.machines()->index(2, 0).data(WallModel::PickedRole).toBool());
        // Again lets go of it. A machine the wall does not have is not picked.
        wall.togglePicked(tiles[3].key);
        wall.togglePicked(u"nobody"_s);
        QCOMPARE(wall.pickedMachines(), QStringList{tiles[1].key});

        // Leaving lets go of all of them.
        wall.setSelecting(false);
        QCOMPARE(wall.picked(), 0);
        QVERIFY(!wall.machines()->index(1, 0).data(WallModel::PickedRole).toBool());
        wall.setSelecting(true);
        QCOMPARE(wall.picked(), 0);
        QCOMPARE(selecting.count(), 3);
    }

    void allThatAreShownArePickedAndNoOthers()
    {
        WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        wall.setSelecting(true);
        // One that is picked already stays picked, also when the bar then narrows the wall.
        wall.togglePicked(tiles[0].key);
        wall.shown()->setShow(u"attention"_s);
        QCOMPARE(wall.shown()->count(), 2);
        wall.pickShown();
        QCOMPARE(wall.picked(), 3);
        // "Select all" means all that are shown: a search narrows it.
        wall.pickNone();
        QCOMPARE(wall.picked(), 0);
        wall.shown()->setShow(u"all"_s);
        wall.shown()->setFind(u"hypervisor"_s);
        wall.pickShown();
        QCOMPARE(wall.pickedMachines(), (QStringList{tiles[2].key, tiles[3].key}));
        wall.shown()->setFind(QString());
        wall.pickShown();
        QCOMPARE(wall.picked(), 15);
        // None again, told once.
        const QSignalSpy picked(&wall, &WallView::pickedChanged);
        wall.pickNone();
        wall.pickNone();
        QCOMPARE(picked.count(), 1);
    }

    void aMachineThatLeavesTheWallIsPickedNoLonger()
    {
        WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        wall.setSelecting(true);
        wall.togglePicked(tiles[1].key);
        wall.togglePicked(tiles[2].key);
        const QString gone = tiles[1].key;
        const QString stays = tiles[2].key;
        const QSignalSpy picked(&wall, &WallView::pickedChanged);
        // How a machine is changes: it is still the machine that was picked.
        tiles[2].state = u"offline"_s;
        wall.machines()->setTiles(tiles);
        QCOMPARE(wall.picked(), 2);
        QCOMPARE(picked.count(), 0);
        // One is taken off the wall on the server.
        tiles.removeAt(1);
        wall.machines()->setTiles(tiles);
        QCOMPARE(wall.pickedMachines(), QStringList{stays});
        QCOMPARE(picked.count(), 1);
        // And comes back: it is not picked by having been once.
        tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        QCOMPARE(wall.pickedMachines(), QStringList{stays});
        QVERIFY(!wall.pickedMachines().contains(gone));
    }

    void aCardNobodySaysMayBeActedOnIsNotPicked()
    {
        // A card as it is made, with nothing said of whether it may be acted on, beside one that
        // says it may: only that one is picked.
        qkvm::ui::WallTile unstamped;
        unstamped.key = u"u"_s;
        unstamped.name = u"Unstamped"_s;
        qkvm::ui::WallTile stamped = unstamped;
        stamped.key = u"s"_s;
        stamped.actable = true;
        WallView wall;
        wall.machines()->setTiles({unstamped, stamped});
        QVERIFY(!wall.machines()->index(0, 0).data(WallModel::ActableRole).toBool());
        wall.setSelecting(true);
        wall.togglePicked(u"u"_s);
        QCOMPARE(wall.picked(), 0);
        wall.pickShown();
        QCOMPARE(wall.pickedMachines(), QStringList{u"s"_s});
    }

    void lookingAtOneMachineEndsThePickingOfSeveral()
    {
        WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        wall.setSelecting(true);
        wall.togglePicked(tiles[1].key);
        wall.enlarge(tiles[4].key);
        QVERIFY(!wall.selecting());
        QCOMPARE(wall.picked(), 0);
        QCOMPARE(wall.enlarged(), tiles[4].key);
    }

    void aStateIsCountedUnderOneOfThree_data()
    {
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("countedAs");
        QTest::newRow("online") << u"online"_s << u"online"_s;
        QTest::newRow("no signal") << u"no_signal"_s << u"no_signal"_s;
        QTest::newRow("offline") << u"offline"_s << u"attention"_s;
        QTest::newRow("login failed") << u"auth"_s << u"attention"_s;
        QTest::newRow("problem") << u"error"_s << u"attention"_s;
        QTest::newRow("in use elsewhere") << u"busy"_s << u"attention"_s;
        // A machine that is still connecting is only counted under All.
        QTest::newRow("connecting") << u"connecting"_s << QString();
        QTest::newRow("a word the program does not know") << u"something_new"_s << QString();
    }

    void aStateIsCountedUnderOneOfThree()
    {
        QFETCH(QString, state);
        QFETCH(QString, countedAs);
        QCOMPARE(WallModel::countedAs(state), countedAs);
    }

    void theBarsChoicesNarrowTheWall()
    {
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        WallFilter& shown = *wall.shown();
        QCOMPARE(shown.count(), 15);

        shown.setShow(u"attention"_s);
        QCOMPARE(namesShown(shown), (QStringList{u"Old server"_s, u"Backup box"_s}));

        shown.setShow(u"all"_s);
        shown.setFind(u"192.168.1.7"_s);
        QCOMPARE(namesShown(shown), (QStringList{u"Database server"_s, u"Render node"_s, u"Spare node"_s}));

        // By a part of the name, whatever the case, and with what was typed around it left out.
        shown.setFind(u"  nas "_s);
        QCOMPARE(namesShown(shown), (QStringList{u"NAS"_s}));

        // Both at once.
        shown.setShow(u"online"_s);
        shown.setFind(u"node"_s);
        QCOMPARE(namesShown(shown), (QStringList{u"Render node"_s}));

        shown.setFind(u"no such machine"_s);
        QCOMPARE(shown.count(), 0);
    }

    void theScreenIsToldWhenTheNumberShownChanges()
    {
        WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        int told = 0;
        connect(wall.shown(), &WallFilter::countChanged, this, [&] { ++told; });
        wall.shown()->setShow(u"no_signal"_s);
        QVERIFY(told > 0);
        told = 0;
        wall.shown()->setShow(u"no_signal"_s);
        QCOMPARE(told, 0); // the same choice again changes nothing
    }

    void theCardsAreAsLargeAsLetsAllBeSeen()
    {
        // Fifteen machines in a window of 1600 by 900, less the bar and the wall's own edge: five
        // across and three down, as the page lays them out there.
        QVariantMap laid = WallView::fit(15, 1576, 828, 12);
        QCOMPARE(laid.value(u"columns"_s).toInt(), 5);
        QCOMPARE(laid.value(u"cardWidth"_s).toInt(), 305);

        // One machine takes the width.
        laid = WallView::fit(1, 1000, 800, 12);
        QCOMPARE(laid.value(u"columns"_s).toInt(), 1);
        QCOMPARE(laid.value(u"cardWidth"_s).toInt(), 1000);

        // A window that is higher than wide stacks them.
        laid = WallView::fit(4, 500, 1600, 12);
        QCOMPARE(laid.value(u"columns"_s).toInt(), 1);
    }

    void tooManyToBeSeenAtOnceStayReadableAndScroll()
    {
        const QVariantMap laid = WallView::fit(64, 800, 600, 12);
        QCOMPARE(laid.value(u"columns"_s).toInt(), 2);
        QCOMPARE(laid.value(u"cardWidth"_s).toInt(), 394);
    }

    void nothingToLayOutIsNoMistake()
    {
        QCOMPARE(WallView::fit(0, 800, 600, 12).value(u"columns"_s).toInt(), 1);
        QCOMPARE(WallView::fit(5, 0, 0, 12).value(u"columns"_s).toInt(), 1);
    }
};

QTEST_GUILESS_MAIN(TestWall)
#include "test_wall.moc"
