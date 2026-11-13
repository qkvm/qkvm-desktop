#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <qkvm/core/settings.h>

#include "pretend_tray.h"
#include "session_view.h"
#include "tray_keeper.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

Q_DECLARE_METATYPE(qkvm::ui::MarkDot)

namespace {

// A window, a pretended icon, and what the keeper is given beside them.
struct Desk {
    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    ui::SessionView session;
    ui::WallView wall;
    ui::PretendedTray tray;
    QWindow window;

    // Whoever sits at the PC chose in the settings that the program goes on when its window is closed.
    void staysInTray(bool stays)
    {
        core::Remembered remembered = settings.remembered();
        remembered.staysInTray = stays;
        QVERIFY(settings.remember(remembered).ok());
    }

    // Somebody is signed in, and the wall has these machines.
    void atTheWall(const QStringList& states)
    {
        ui::SessionView::Shown shown;
        shown.stage = u"wall"_s;
        session.show(shown);
        QList<ui::WallTile> tiles;
        for (const QString& state : states) {
            ui::WallTile tile;
            tile.key = u"m%1"_s.arg(tiles.size());
            tile.name = tile.key;
            tile.state = state;
            tiles.append(tile);
        }
        wall.machines()->setTiles(tiles);
    }
};

} // namespace

// The icon in the notification area: what it says of the wall, and the program going on behind it.
class TestTrayKeeper : public QObject {
    Q_OBJECT

private slots:
    void anUpdateNoticeCannotBorrowAMachineAlertsTarget()
    {
        ui::PretendedTray tray;
        tray.show(ui::MarkDot::None, {});
        QSignalSpy opened(&tray, &ui::TrayIcon::notificationOpened);
        tray.tellAbout(u"Machine offline"_s, u"First server"_s, true, u"one"_s, u"a"_s);
        tray.tell(u"Update available"_s, u"Review it in Settings"_s, false);
        tray.tellAbout(u"Machine recovered"_s, u"Other server"_s, false, u"two"_s, u"a"_s);
        QCOMPARE(tray.messages.size(), 1);
        tray.finishMessage(true);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.front().at(0).toString(), u"one"_s);
        QTRY_COMPARE(tray.messages.size(), 2);
        QCOMPARE(tray.messages.back().title, u"Update available"_s);
        tray.finishMessage(true);
        QCOMPARE(opened.size(), 1); // An update has no machine to open.
        QTRY_COMPARE(tray.messages.size(), 3);
        QCOMPARE(tray.messages.back().title, u"Machine recovered"_s);
        tray.finishMessage(true);
        QCOMPARE(opened.size(), 2);
        QCOMPARE(opened.back().at(0).toString(), u"two"_s);
    }

    void notificationTargetsStayWithTheirMessagesAndCanBeCancelled()
    {
        ui::PretendedTray tray;
        tray.show(ui::MarkDot::None, {});
        QSignalSpy opened(&tray, &ui::TrayIcon::notificationOpened);
        tray.tellAbout(u"First"_s, u"First machine"_s, true, u"one"_s, u"a"_s);
        tray.tellAbout(u"Second"_s, u"Same machine ID, other server"_s, true, u"two"_s, u"a"_s);
        QCOMPARE(tray.messages.size(), 1);
        tray.finishMessage(true);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.front().at(0).toString(), u"one"_s);
        QTRY_COMPARE(tray.messages.size(), 2);
        tray.finishMessage(true);
        QCOMPARE(opened.back().at(0).toString(), u"two"_s);
        tray.finishMessage(true); // An old or duplicate callback has no target.
        QCOMPARE(opened.size(), 2);
        tray.tellAbout(u"Third"_s, u"Private"_s, true, u"three"_s, u"b"_s);
        tray.tellAbout(u"Fourth"_s, u"Queued"_s, true, u"four"_s, u"c"_s);
        tray.clearNotifications();
        tray.finishMessage(true);
        QCoreApplication::processEvents();
        QCOMPARE(opened.size(), 2);
        QCOMPARE(tray.messages.size(), 3);
    }

    void theIconSaysHowTheWallIs_data()
    {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<bool>("inContact");
        QTest::addColumn<QStringList>("states");
        QTest::addColumn<ui::MarkDot>("dot");
        QTest::addColumn<QString>("says");
        const QStringList well{u"online"_s, u"online"_s, u"connecting"_s};
        QTest::newRow("nobody is signed in")
            << u"signIn"_s << true << QStringList() << ui::MarkDot::None << u"qKVM\nNot signed in"_s;
        QTest::newRow("all is well") << u"wall"_s << true << well << ui::MarkDot::None
                                     << u"qKVM\nKVMs: 3 · Online: 2 · No signal: 0 · Attention: 0"_s;
        QTest::newRow("a machine without a signal")
            << u"wall"_s << true << QStringList{u"online"_s, u"no_signal"_s} << ui::MarkDot::Warn
            << u"qKVM\nKVMs: 2 · Online: 1 · No signal: 1 · Attention: 0"_s;
        QTest::newRow("a machine that wants attention")
            << u"wall"_s << true << QStringList{u"online"_s, u"no_signal"_s, u"offline"_s, u"auth"_s}
            << ui::MarkDot::Bad << u"qKVM\nKVMs: 4 · Online: 1 · No signal: 1 · Attention: 2"_s;
        QTest::newRow("a wall without machines") << u"wall"_s << true << QStringList() << ui::MarkDot::None
                                                 << u"qKVM\nKVMs: 0 · Online: 0 · No signal: 0 · Attention: 0"_s;
        QTest::newRow("qKVM does not answer")
            << u"wall"_s << false << well << ui::MarkDot::Bad << u"qKVM\nLost contact with qKVM"_s;
    }

    void theIconSaysHowTheWallIs()
    {
        QFETCH(QString, stage);
        QFETCH(bool, inContact);
        QFETCH(QStringList, states);
        QFETCH(ui::MarkDot, dot);
        QFETCH(QString, says);
        Desk desk;
        desk.atTheWall(states);
        ui::SessionView::Shown shown;
        shown.stage = stage;
        shown.inContact = inContact;
        desk.session.show(shown);
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        QVERIFY(desk.tray.there);
        QCOMPARE(desk.tray.dot, dot);
        QCOMPARE(desk.tray.says, says);
        // Windows takes so many letters and no more.
        QVERIFY(desk.tray.says.size() <= 127);
    }

    void theIconFollowsTheWallAndIsToldOnlyWhatIsNew()
    {
        Desk desk;
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        QCOMPARE(desk.tray.says, u"qKVM\nNot signed in"_s);
        QCOMPARE(desk.tray.told, 1);

        desk.atTheWall({u"online"_s, u"online"_s});
        QCOMPARE(desk.tray.dot, ui::MarkDot::None);
        QCOMPARE(desk.tray.says, u"qKVM\nKVMs: 2 · Online: 2 · No signal: 0 · Attention: 0"_s);
        const int told = desk.tray.told;

        // A machine goes away.
        desk.atTheWall({u"online"_s, u"offline"_s});
        QCOMPARE(desk.tray.dot, ui::MarkDot::Bad);
        QCOMPARE(desk.tray.says, u"qKVM\nKVMs: 2 · Online: 1 · No signal: 0 · Attention: 1"_s);
        QCOMPARE(desk.tray.told, told + 1);
        // The wall is filled again with what it had: the icon is left alone.
        desk.atTheWall({u"online"_s, u"offline"_s});
        ui::SessionView::Shown same = desk.session.shown();
        same.account = u"anna"_s;
        desk.session.show(same);
        QCOMPARE(desk.tray.told, told + 1);

        // QKVM stops answering, and answers again.
        ui::SessionView::Shown away = desk.session.shown();
        away.inContact = false;
        desk.session.show(away);
        QCOMPARE(desk.tray.says, u"qKVM\nLost contact with qKVM"_s);
        away.inContact = true;
        desk.session.show(away);
        QCOMPARE(desk.tray.says, u"qKVM\nKVMs: 2 · Online: 1 · No signal: 0 · Attention: 1"_s);
    }

    void theMenuOpensTheWindowAndThePageAndEndsTheProgram()
    {
        Desk desk;
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        const QSignalSpy pageAsked(&desk.session, &ui::SessionView::pageAsked);
        const QSignalSpy quitAsked(&keeper, &ui::TrayKeeper::quitAsked);
        // Nobody is signed in: there is no QKVM whose page it would be.
        QCOMPARE(desk.tray.rows(), (QStringList{u"Open qKVM"_s, u"-"_s, u"Quit"_s}));

        desk.atTheWall({u"online"_s});
        QCOMPARE(desk.tray.rows(), (QStringList{u"Open qKVM"_s, u"Open the qKVM page"_s, u"-"_s, u"Quit"_s}));
        QVERIFY(desk.tray.choose(u"Open the qKVM page"_s));
        QCOMPARE(pageAsked.count(), 1);
        QCOMPARE(quitAsked.count(), 0);

        QVERIFY(!desk.window.isVisible());
        QVERIFY(desk.tray.choose(u"Open qKVM"_s));
        QVERIFY(desk.window.isVisible());
        QCOMPARE(pageAsked.count(), 1);

        QVERIFY(desk.tray.choose(u"Quit"_s));
        QCOMPARE(quitAsked.count(), 1);

        // Signed out: the page's row is gone again.
        desk.session.show({});
        QCOMPARE(desk.tray.rows(), (QStringList{u"Open qKVM"_s, u"-"_s, u"Quit"_s}));
    }

    void atThisPcsOwnWallThereIsNoPageToOpen()
    {
        Desk desk;
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        // No QKVM server is used: there is no page of one. What the icon says of the wall is as ever.
        ui::SessionView::Shown own;
        own.stage = u"wall"_s;
        own.ownWall = true;
        desk.session.show(own);
        QCOMPARE(desk.tray.rows(), (QStringList{u"Open qKVM"_s, u"-"_s, u"Quit"_s}));

        // On to a server's wall, and back.
        desk.atTheWall({u"online"_s});
        QCOMPARE(desk.tray.rows(), (QStringList{u"Open qKVM"_s, u"Open the qKVM page"_s, u"-"_s, u"Quit"_s}));
        desk.session.show(own);
        QCOMPARE(desk.tray.rows(), (QStringList{u"Open qKVM"_s, u"-"_s, u"Quit"_s}));
        QCOMPARE(desk.tray.says, u"qKVM\nKVMs: 1 · Online: 1 · No signal: 0 · Attention: 0"_s);
    }

    void aClickOnTheIconBringsTheWindowBackAsItWas()
    {
        Desk desk;
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        desk.window.showMaximized();
        QVERIFY(QTest::qWaitForWindowExposed(&desk.window));

        // Minimized, as its button in the title bar does it: back to what it was, and not to a
        // window of its own size.
        desk.window.setWindowStates(desk.window.windowStates() | Qt::WindowMinimized);
        QTRY_VERIFY((desk.window.windowStates() & Qt::WindowMinimized) != 0);
        desk.tray.click();
        QTRY_VERIFY((desk.window.windowStates() & Qt::WindowMinimized) == 0);
        QVERIFY((desk.window.windowStates() & Qt::WindowMaximized) != 0);
        QVERIFY(desk.window.isVisible());

        // Out of sight: the same.
        desk.window.hide();
        QVERIFY(!desk.window.isVisible());
        desk.tray.click();
        QVERIFY(desk.window.isVisible());
        QVERIFY((desk.window.windowStates() & Qt::WindowMaximized) != 0);
    }

    void closingTheWindowClosesItUnlessTheProgramIsToGoOn()
    {
        Desk desk;
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        desk.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&desk.window));
        // Nothing was chosen in the settings: the window closes, as any window does.
        QVERIFY(desk.window.close());
        QVERIFY(!desk.window.isVisible());
    }

    void whereTheProgramIsToGoOnTheWindowGoesOutOfSightAndTheIconBringsItBack()
    {
        Desk desk;
        desk.staysInTray(true);
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        desk.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&desk.window));

        // Not closed: whoever asked is told so, and the window is still there to be shown again.
        QVERIFY(!desk.window.close());
        QVERIFY(!desk.window.isVisible());
        QVERIFY(desk.window.handle() != nullptr);
        desk.tray.click();
        QVERIFY(desk.window.isVisible());

        // Switched off in the settings while the program runs: the next close closes.
        desk.staysInTray(false);
        QVERIFY(desk.window.close());
        QVERIFY(!desk.window.isVisible());
    }

    void withoutAnIconTheWindowClosesWhateverWasChosen()
    {
        Desk desk;
        desk.staysInTray(true);
        // Windows has no place for the icon: nothing would bring the window back.
        desk.tray.hasRoom = false;
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        QVERIFY(!desk.tray.there);
        desk.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&desk.window));
        QVERIFY(desk.window.close());

        // The taskbar comes, and the icon with it, with nothing new on the wall: from then on
        // the window goes out of sight and is not closed.
        desk.tray.taskbarCame();
        desk.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&desk.window));
        QVERIFY(!desk.window.close());
        QVERIFY(!desk.window.isVisible());
        desk.tray.click();
        QVERIFY(desk.window.isVisible());
    }

    void ofSeveralWindowsOnlyTheLastGoesBehindTheIcon()
    {
        Desk desk;
        desk.staysInTray(true);
        QWindow other;
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        keeper.keep(other);
        desk.window.show();
        other.show();
        QVERIFY(QTest::qWaitForWindowExposed(&other));
        // One of two: closed, as any window is. Whoever keeps the windows lets go of it.
        QVERIFY(other.close());
        QVERIFY(!other.isVisible());
        keeper.letGo(other);
        // The last one: out of sight, and not closed.
        QVERIFY(!desk.window.close());
        QVERIFY(!desk.window.isVisible());
        // The icon brings back what is kept, and not a window that was let go of.
        desk.tray.click();
        QVERIFY(desk.window.isVisible());
        QVERIFY(!other.isVisible());
        // And the one that was let go of closes like any, while one is kept: it is not the
        // program's last window, and the last window is not put away for it.
        other.show();
        QVERIFY(other.close());
        QVERIFY(!other.isVisible());
        QVERIFY(desk.window.isVisible());

        // Two again, and neither in sight: the icon's menu brings both back.
        keeper.keep(other);
        // (Kept twice, it is one window.)
        keeper.keep(other);
        desk.window.hide();
        QVERIFY(desk.tray.choose(u"Open qKVM"_s));
        QVERIFY(desk.window.isVisible());
        QVERIFY(other.isVisible());
        // And of two, the one that is closed first is closed, whichever it is.
        QVERIFY(desk.window.close());
        QVERIFY(!desk.window.isVisible());
        keeper.letGo(desk.window);
        QVERIFY(!other.close());
        QVERIFY(!other.isVisible());
        QVERIFY(other.handle() != nullptr);
        // A window that was let go of is nobody's to hold back.
        keeper.letGo(other);
        other.show();
        QVERIFY(other.close());
        QVERIFY(!other.isVisible());
    }

    void quitEndsTheProgramAlsoWhereItWasToGoOn()
    {
        Desk desk;
        desk.staysInTray(true);
        ui::TrayKeeper keeper(desk.tray, desk.session, desk.wall, desk.settings);
        keeper.keep(desk.window);
        desk.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&desk.window));
        QVERIFY(desk.tray.choose(u"Quit"_s));
        // The program closes its windows as it ends: this one lets itself be closed now.
        QVERIFY(desk.window.close());
        QVERIFY(!desk.window.isVisible());
    }
};

QTEST_MAIN(TestTrayKeeper)
#include "test_tray_keeper.moc"
