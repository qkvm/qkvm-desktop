#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/settings.h>

#include "pretend_tray.h"
#include "session_view.h"
#include "wall_teller.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// How long the teller under test gathers news before it tells; and long enough after that for
// it to have told.
constexpr int gatherMs = 400;
constexpr int afterwardsMs = 900;

// A pretended icon, and what the teller is given beside it. Nobody can see the wall, and messages
// are wanted, until a test says otherwise.
struct Desk {
    Desk()
    {
        wall.setUnseen(true);
        tray.show(ui::MarkDot::None, {});
        tray.finishesMessages = true;
    }

    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    ui::SessionView session;
    ui::WallView wall;
    ui::PretendedTray tray;
    ui::WallTeller teller{tray, session, wall, settings, gatherMs};

    // Somebody is signed in; QKVM answers, or does not.
    void atTheWall(bool answers = true)
    {
        ui::SessionView::Shown shown;
        shown.stage = u"wall"_s;
        shown.inContact = answers;
        session.show(shown);
    }

    // The wall's machines, each as "name state": the machine is known by its name, and what its
    // card says of the state in a word is the state with a capital.
    void machines(const QStringList& named)
    {
        QList<ui::WallTile> tiles;
        for (const QString& one : named) {
            const QStringList parts = one.split(u' ');
            ui::WallTile tile;
            tile.key = parts.value(0);
            tile.name = parts.value(0);
            tile.state = parts.value(1);
            tile.label = parts.value(1).left(1).toUpper() + parts.value(1).mid(1);
            tiles.append(tile);
        }
        wall.machines()->setTiles(tiles);
    }

    void wantsMessages(bool wants)
    {
        core::Remembered remembered = settings.remembered();
        remembered.tellsNews = wants;
        QVERIFY(settings.remember(remembered).ok());
    }

    // What was told so far, each as "heading | line" with "!" in front of a warning.
    QStringList told() const
    {
        QStringList all;
        for (const ui::PretendedTray::Said& said : tray.messages) {
            all.append((said.isWarning ? u"!"_s : QString()) + said.title + u" | "_s + said.text);
        }
        return all;
    }
};

} // namespace

// What is told to whoever is not looking at the wall, and when.
class TestWallTeller : public QObject {
    Q_OBJECT

private slots:
    void aMachineThatGoesAwayAndOneThatComesBackAreTold()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build offline"_s});
        QTest::qWait(afterwardsMs);
        // The wall as it is when it is first filled is no news.
        QCOMPARE(desk.told(), QStringList());

        desk.machines({u"NAS offline"_s, u"Build offline"_s});
        // Not at once.
        QCOMPARE(desk.told(), QStringList());
        QTRY_COMPARE(desk.told(), QStringList{u"!NAS needs attention | Offline"_s});

        desk.machines({u"NAS online"_s, u"Build offline"_s});
        QTRY_COMPARE(desk.told().size(), 2);
        QCOMPARE(desk.told().constLast(), u"NAS is back | Online"_s);
        // And each once.
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told().size(), 2);
    }

    void whatHappensWithinAMomentIsOneMessage()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build online"_s, u"Lab online"_s, u"Old offline"_s});
        // A switch goes, and the machines behind it are missed one after the other.
        desk.machines({u"NAS offline"_s, u"Build online"_s, u"Lab online"_s, u"Old offline"_s});
        QTest::qWait(20);
        desk.machines({u"NAS offline"_s, u"Build offline"_s, u"Lab online"_s, u"Old offline"_s});
        QTest::qWait(20);
        desk.machines({u"NAS offline"_s, u"Build offline"_s, u"Lab error"_s, u"Old offline"_s});
        QTRY_COMPARE(desk.told(), QStringList{u"!3 KVMs need attention | NAS, Build, Lab"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told().size(), 1);
    }

    void aMachineThatIsGoneForAMomentIsNoNews()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build online"_s});
        desk.machines({u"NAS offline"_s, u"Build online"_s});
        QTest::qWait(20);
        desk.machines({u"NAS online"_s, u"Build online"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
    }

    void whoeverCanSeeTheWallIsToldNothingNorLaterWhatHappenedThen()
    {
        Desk desk;
        desk.wall.setUnseen(false);
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build online"_s});
        desk.machines({u"NAS offline"_s, u"Build online"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());

        // The window goes out of sight: what happened before is old news, what happens now is told.
        desk.wall.setUnseen(true);
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
        desk.machines({u"NAS offline"_s, u"Build auth"_s});
        QTRY_COMPARE(desk.told(), QStringList{u"!Build needs attention | Auth"_s});
    }

    void whoeverSwitchedTheMessagesOffIsToldNothing()
    {
        Desk desk;
        desk.wantsMessages(false);
        desk.atTheWall();
        desk.machines({u"NAS online"_s});
        desk.machines({u"NAS offline"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
        // Not even that QKVM does not answer.
        desk.atTheWall(false);
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
        desk.atTheWall(true);
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());

        // Switched on again while the program runs: from then on.
        desk.wantsMessages(true);
        desk.machines({u"NAS online"_s});
        QTRY_COMPARE(desk.told(), QStringList{u"NAS is back | Online"_s});
    }

    void aQkvmThatDoesNotAnswerIsToldAndThatItAnswersAgain()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build online"_s});
        desk.atTheWall(false);
        QCOMPARE(desk.told(), QStringList());
        QTRY_COMPARE(desk.told(), QStringList{u"!Lost contact with qKVM | Trying again. The wall is as it was."_s});
        // Once, however long it lasts.
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told().size(), 1);

        desk.atTheWall(true);
        QTRY_COMPARE(desk.told().size(), 2);
        QCOMPARE(desk.told().constLast(), u"qKVM answers again | The wall is up to date."_s);
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told().size(), 2);
        // That was said once: what happens next is told as what it is.
        desk.machines({u"NAS offline"_s, u"Build online"_s});
        QTRY_COMPARE(desk.told().size(), 3);
        QCOMPARE(desk.told().constLast(), u"!NAS needs attention | Offline"_s);
    }

    void aMachineThatGoesAwayWhileAnotherIsPutOnTheWallIsTold()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build online"_s});
        // Somebody adds a machine on the QKVM's page, and at the same look another is gone.
        desk.machines({u"NAS offline"_s, u"Build online"_s, u"New offline"_s});
        QTRY_COMPARE(desk.told(), QStringList{u"!NAS needs attention | Offline"_s});
    }

    void whileNobodyIsSignedInNothingIsTold()
    {
        Desk desk;
        // Whatever the wall's machines are while the program asks which QKVM, or who.
        desk.machines({u"NAS online"_s});
        desk.machines({u"NAS offline"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
        // Nor is that what the first look at the wall is held against.
        desk.atTheWall();
        desk.machines({u"NAS online"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
    }

    void whenQkvmAnswersAgainWhatBecameOfTheMachinesIsToldWithIt()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build online"_s});
        desk.atTheWall(false);
        QTRY_COMPARE(desk.told().size(), 1);
        // It answers, and the wall is filled with what is so now.
        desk.atTheWall(true);
        desk.machines({u"NAS offline"_s, u"Build online"_s});
        QTRY_COMPARE(desk.told().size(), 2);
        QCOMPARE(desk.told().constLast(), u"!qKVM answers again | NAS needs attention"_s);
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told().size(), 2);
    }

    void aQkvmThatIsSilentForAMomentIsNoNews()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s});
        desk.atTheWall(false);
        QTest::qWait(20);
        desk.atTheWall(true);
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
    }

    void whoeverWasToldThatQkvmIsSilentIsToldThatItAnswersAlsoWithTheWallInSight()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s});
        desk.atTheWall(false);
        QTRY_COMPARE(desk.told().size(), 1);
        // Somebody opens the window, and then it answers: the warning that still stands in
        // Windows' list is followed by its end.
        desk.wall.setUnseen(false);
        desk.atTheWall(true);
        QTRY_COMPARE(desk.told().size(), 2);
        QCOMPARE(desk.told().constLast(), u"qKVM answers again | The wall is up to date."_s);

        // Lost while the wall is in sight: said by the wall, and by nothing else; nor is its end.
        desk.atTheWall(false);
        QTest::qWait(afterwardsMs);
        desk.atTheWall(true);
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told().size(), 2);
    }

    void whileQkvmIsSilentTheWallSaysNothingNewOfAnyMachine()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build online"_s});
        desk.atTheWall(false);
        QTRY_COMPARE(desk.told().size(), 1);
        // Whatever the wall is filled with meanwhile is not QKVM's word: nothing is told of it,
        // and it is not what the wall is held against when QKVM answers again.
        desk.machines({u"NAS offline"_s, u"Build online"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told().size(), 1);
        desk.atTheWall(true);
        desk.machines({u"NAS online"_s, u"Build online"_s});
        QTRY_COMPARE(desk.told().size(), 2);
        QCOMPARE(desk.told().constLast(), u"qKVM answers again | The wall is up to date."_s);
    }

    void afterQkvmWasStartedAgainMachinesThatAreAsBeforeAreNoNews()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build offline"_s, u"Lab no_signal"_s});
        QTest::qWait(afterwardsMs);
        // Every machine is connected to anew, for longer than news is gathered, and is then
        // what it was.
        desk.machines({u"NAS connecting"_s, u"Build connecting"_s, u"Lab connecting"_s});
        QTest::qWait(afterwardsMs);
        desk.machines({u"NAS online"_s, u"Build connecting"_s, u"Lab no_signal"_s});
        QTest::qWait(afterwardsMs);
        desk.machines({u"NAS online"_s, u"Build offline"_s, u"Lab no_signal"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());

        // One that is not: told as what it is.
        desk.machines({u"NAS connecting"_s, u"Build connecting"_s, u"Lab connecting"_s});
        QTest::qWait(afterwardsMs);
        desk.machines({u"NAS offline"_s, u"Build online"_s, u"Lab no_signal"_s});
        QTRY_COMPARE(desk.told(), QStringList{u"!1 need attention, 1 back | Need attention: NAS. Back: Build."_s});
    }

    void ofAWallThatWasLeftNothingIsTold()
    {
        Desk desk;
        desk.atTheWall();
        desk.machines({u"NAS online"_s});
        desk.machines({u"NAS offline"_s});
        // Signed out before anybody was told.
        desk.session.show({});
        desk.machines({});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
        // Whatever the wall's machines are from then on is no QKVM's word.
        desk.machines({u"NAS online"_s});
        desk.machines({u"NAS offline"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
        desk.machines({});

        // Signed in again, to a wall where the machine is back: its first fill is no news.
        desk.atTheWall();
        desk.machines({u"NAS online"_s, u"Build online"_s});
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told(), QStringList());
        // And what is told of this wall has nothing of the last one in it.
        desk.machines({u"NAS online"_s, u"Build offline"_s});
        QTRY_COMPARE(desk.told(), QStringList{u"!Build needs attention | Offline"_s});
        desk.tray.messages.clear();
        // And QKVM that did not answer before somebody signed out is not "answering again".
        desk.atTheWall(false);
        QTRY_COMPARE(desk.told().size(), 1);
        desk.session.show({});
        desk.machines({});
        desk.atTheWall(true);
        QTest::qWait(afterwardsMs);
        QCOMPARE(desk.told().size(), 1);
        // Nor later: what becomes of a machine of the new wall is told as what it is.
        desk.machines({u"NAS online"_s, u"Build offline"_s});
        desk.machines({u"NAS error"_s, u"Build offline"_s});
        QTRY_COMPARE(desk.told().size(), 2);
        QCOMPARE(desk.told().constLast(), u"!NAS needs attention | Error"_s);
    }
};

QTEST_MAIN(TestWallTeller)
#include "test_wall_teller.moc"
