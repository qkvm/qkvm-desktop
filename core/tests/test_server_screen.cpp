#include <QTest>

#include "server_screen_wall.h"

// A server's own screen from its controller's console (AMI's), against a console by hand on this PC's own address:
// the web's tests of it (app/tests/test_redfish.py, the console's part), the ones that are the console's and not its
// web page's sign-in, test for test where they apply, and the desktop's own where the web's tested a thing through
// its tile. Here: the console's session, and its manners towards whoever else is at it; keys, the
// pointer and typings over it are test_server_screen_input.cpp's and test_server_screen_backlog.cpp's.
using namespace qkvm::core::testing::screens;

class TestServerScreen : public QObject {
    Q_OBJECT
private slots:
    void aServersScreenIsShown()
    {
        // A wall of screens showed a server as a list of temperatures. Its screen is in its controller's own console,
        // and that is where it is taken from.
        Wall wall;
        // (The second frame once the first was shown: two read before the screen is shown are shown as one, with what
        // both changed. Review 3's HIGH 1. 150 ms after the first was not enough on the loaded slow cores, where the
        // first was not shown within it once: one picture of both, and its change the whole screen.)
        wall.console.sendsFrameWhen = [&wall](std::size_t frame) { return wall.shown >= static_cast<int>(frame); };
        const auto opening = wall.screen->look();
        QVERIFY(!opening.ok());
        QCOMPARE(codeIn(opening), std::string("no_stream")); // asked again, as any picture not there yet
        QVERIFY(wall.shownWithin(5000));
        QTRY_VERIFY(wall.shown >= 2);
        const aspeed::Picture& picture = wall.screen->picture();
        QCOMPARE(picture.width, 64);
        QCOMPARE(picture.height, 32);
        const auto left = at(picture, 8, 8);
        const auto right = at(picture, 40, 8);
        const auto turned = at(picture, 52, 20);
        QVERIFY(*std::min_element(left.begin(), left.end()) > 225 &&
                *std::max_element(right.begin(), right.end()) < 30);
        QVERIFY(turned[0] > 150 && turned[1] < 90 && turned[2] < 90);
        // What the last frame changed: its one block.
        QCOMPARE(wall.rects, (std::vector<aspeed::Rect>{{48, 16, 8, 8}}));
        // One way in and one console seat, and nothing said but what looking takes, as from a page of its own.
        QCOMPARE(wall.wayIns, 1);
        QCOMPARE(wall.console.sessions, 1);
        QCOMPARE(wall.console.watching, 1);
        QVERIFY(wall.heardKinds().contains(ami::kind::pass));
        for (const std::uint16_t kind : wall.heardKinds()) {
            QVERIFY2(looking.contains(kind), QByteArray::number(kind));
        }
        QCOMPARE(wall.console.origins, QList<QByteArray>{wall.console.address().toString().toLatin1()});
        // The console is this program's ("the console is yours"), though nothing is typed into it here.
        QVERIFY(wall.screen->holdsTheConsole());
        // Closed, it says that it is leaving and gives the seat back; the screen is not kept.
        wall.screen->close();
        QTRY_COMPARE(wall.console.watching, 0);
        QCOMPARE(wall.console.heard.back().kind, ami::kind::stop);
        QCOMPARE(wall.screen->picture().width, 0);
        QCOMPARE(wall.code(), std::string("no_picture"));
    }

    void whatIsSaidToTheConsoleIsWhatItsOwnPageSays()
    {
        // (The desktop's own.) The licence is answered with the web session, "who else is there" with "the whole
        // screen", "still there?" with itself, and frames that came with "every one is drawn".
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        QTRY_VERIFY(wall.console.heardOf(ami::kind::stillThere) >= 1 && wall.console.heardOf(ami::kind::behind) >= 1);
        QCOMPARE(wall.console.heardOf(ami::kind::webSession), 1);
        QCOMPARE(wall.console.heardOf(ami::kind::fullScreen), 1);
        for (const auto& one : wall.console.heard) {
            if (one.kind == ami::kind::webSession) {
                QCOMPARE(one.bytes, qsizetype{35});
            }
            if (one.kind == ami::kind::fullScreen) {
                QCOMPARE(one.status, std::uint16_t{1});
            }
            if (one.kind == ami::kind::pass) {
                QCOMPARE(one.bytes, qsizetype{438});
            }
        }
    }

    void theConsoleIsGivenBackWhenNobodyLooks()
    {
        // A controller has two console seats. The wall takes one while somebody looks at it, and not for good.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.idleMs = 400;
        Wall wall(limits);
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        QVERIFY(wall.shownWithin(5000));
        QCOMPARE(wall.console.watching, 1);
        QTRY_COMPARE(wall.console.watching, 0);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.first().first().toString(), QString()); // no fault
        QCOMPARE(wall.console.heard.back().kind, ami::kind::stop);
        QCOMPARE(wall.screen->picture().width, 0); // (nor the screen, beyond the line it came over)
        // ...and whoever looks again gets it again, at once.
        QVERIFY(wall.shownWithin(5000));
        QCOMPARE(wall.wayIns, 2);
    }

    void withEverySeatTakenItIsSaidSoAndAskedForLessAndLessOften()
    {
        // A controller is a small computer. The real one was slow to answer anybody for some seconds after every
        // console that came and went, and one came and went every thirty seconds.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.retryMs = {200, 30000, 30000, 30000};
        Wall wall(limits);
        wall.console.seats = 0;
        QTRY_VERIFY((wall.lookOnce(), wall.wayIns == 2));
        const std::set<std::string> seen = wall.lookFor(1200);
        QVERIFY(seen.size() <= 2 && seen.contains("in_use")); // (and "no_stream" while the second try was under way)
        QCOMPARE(wall.wayIns, 2);                             // the third time is half a minute off
        QCOMPARE(wall.code(), std::string("in_use"));
    }

    void firmwareThatLetsAConsoleReconnectIsGreetedTheWayItWants()
    {
        // A real board (a Gigabyte one) said "allowed", was shown the pass, and closed without a word, every thirty
        // seconds, for as long as its tile was looked at. Its firmware wants "connection complete" said first, and says
        // so in the list of what it can do.
        Wall wall;
        wall.console.reconnects = true;
        wall.wayIn.completeFirst = true;
        QVERIFY(wall.shownWithin(5000));
        QCOMPARE(wall.console.heard.front().kind, ami::kind::complete);
        QCOMPARE(wall.wayIns, 1);
    }

    void aConsoleThatClosesOnTheGreetingIsGreetedTheOtherWayNextTime()
    {
        // ...and where the list does not say, it is found out by trying: once each way, not the same way for ever.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.retryMs = {200, 200, 200, 200};
        Wall wall(limits);
        wall.console.reconnects = true;
        QVERIFY(wall.shownWithin(8000));
        std::vector<std::uint16_t> greetings;
        for (const auto& one : wall.console.heard) {
            if (one.kind == ami::kind::complete || one.kind == ami::kind::pass) {
                greetings.push_back(one.kind);
            }
        }
        QCOMPARE(greetings, (std::vector<std::uint16_t>{ami::kind::pass, ami::kind::complete, ami::kind::pass}));
        QCOMPARE(wall.wayIns, 2);
    }

    void aConsoleThatBeginsWithNothingChangedIsNotShownAsABlackScreen()
    {
        // A real board, every time its console was opened again: "the first frame, 16 bytes". That frame says that
        // nothing changed, the whole screen comes when it is asked for, and for the moment between the two the tile
        // of a running server showed a black picture.
        Wall wall;
        wall.console.beginsWithNothing = true;
        QVERIFY(wall.shownWithin(5000));
        const auto left = at(wall.screen->picture(), 8, 8);
        QVERIFY(*std::min_element(left.begin(), left.end()) > 225); // the screen, the first time anything is shown
        QVERIFY(wall.shown >= 1);
    }

    void somebodyWhoOpensTheServersOwnConsoleIsLetInAtOnce_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("asked whether they may") << int{ami::kind::sharing};
        QTest::newRow("asked for the keys") << int{ami::kind::nextHolder};
    }

    void somebodyWhoOpensTheServersOwnConsoleIsLetInAtOnce()
    {
        // The controller asks whoever has the console whether a newcomer may have it, and waits half a minute for the
        // answer. Whoever opened the console of a server to work on it waited that half minute, every time. They are
        // let in at once (the wall only looks, and goes on looking).
        QFETCH(int, kind);
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        QVERIFY(wall.screen->holdsTheConsole());
        wall.console.asks.emplace_back(static_cast<std::uint16_t>(kind), somebody);
        QTRY_COMPARE(wall.console.answers.size(), std::size_t{2});
        QVERIFY(wall.console.answers[0].kind == ami::kind::sharing &&
                wall.console.answers[0].status == ami::sharing::asked && wall.console.answers[0].who == somebody);
        QVERIFY(wall.console.answers[1].kind == ami::kind::nextHolder &&
                wall.console.answers[1].status == ami::sharing::asked && wall.console.answers[1].who == somebody);
        QVERIFY(wall.console.answers[0].afterMs < 2000 && wall.console.answers[1].afterMs < 2000);
        QVERIFY(wall.screen->look().ok());
        QCOMPARE(wall.console.watching, 1);
        QVERIFY(!wall.screen->holdsTheConsole()); // theirs now
    }

    void theWallDoesNotMakeSomebodyAtTheConsoleAnswerForIt_data()
    {
        QTest::addColumn<bool>("shutOut");
        QTest::newRow("asked") << false;
        QTest::newRow("shut out") << true;
    }

    void theWallDoesNotMakeSomebodyAtTheConsoleAnswerForIt()
    {
        // The other way round: somebody is working in the console, and then the wall is looked at. They would get a
        // question on their screen with half a minute to answer it, and unanswered the console passes to whoever asked.
        // It leaves the moment it is told to wait, says so, and looks again later.
        QFETCH(bool, shutOut);
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.heldMs = 1500;
        Wall wall(limits);
        wall.console.heldBy = somebody;
        wall.console.shutOut = shutOut;
        QTRY_COMPARE(wall.code(), std::string("console_held"));
        QTRY_COMPARE(wall.console.watching, 0);
        QCOMPARE(wall.console.heard.back().kind, ami::kind::stop);
        for (const std::uint16_t kind : wall.heardKinds()) {
            QVERIFY2(looking.contains(kind), QByteArray::number(kind)); // it says that it is leaving, and nothing else
        }
        QVERIFY(wall.console.answers.empty());
        QVERIFY(wall.message().contains("Somebody is using"));
        QCOMPARE(wall.wayIns, 1); // ...and does not come back every few seconds to be told again
        // Later it looks again. They have left, and the screen is back.
        wall.console.heldBy.clear();
        QVERIFY(wall.shownWithin(5000));
        QCOMPARE(wall.wayIns, 2);
    }

    void aConsoleWhoseLastHolderMayComeBackShowsTheScreenAllTheSame()
    {
        // Seen on a real board after a console that had dropped: the next one was told "the one who held it is coming
        // back", and was sent the picture. Nobody is asked anything then, and the wall stays.
        Wall wall;
        wall.console.holderReturning = true;
        QVERIFY(wall.shownWithin(5000));
        for (const std::uint16_t kind : wall.heardKinds()) {
            QVERIFY2(looking.contains(kind), QByteArray::number(kind));
        }
        QVERIFY(wall.console.answers.empty());
        QVERIFY(!wall.screen->holdsTheConsole()); // (it was never said to be this program's)
    }

    void aConsoleSessionWhoseTimeIsUpIsOpenedAgainAtOnce()
    {
        // Read in the log of a wall that was left open overnight on two real servers: each controller ended its console
        // session every half hour (its rule for one in which no key is pressed), the wall took that for a failure,
        // waited thirty seconds, and each server's picture was gone for that long, twice an hour.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.settledMs = 300;
        Wall wall(limits);
        wall.console.sessionLastsMs = 1500;
        QVERIFY(wall.shownWithin(5000));
        std::set<std::string> seen;
        QElapsedTimer waited;
        waited.start();
        while (wall.console.sessions < 2 && waited.elapsed() < 15000) {
            const auto looked = wall.screen->look();
            seen.insert(looked ? std::string("shown") : looked.error().code);
            QTest::qWait(30);
        }
        QVERIFY(wall.console.sessions >= 2); // opened again when its time was up, and not half a minute after that
        for (const std::string& one : seen) {
            QVERIFY2(one == "shown" || one == "no_stream", one.c_str()); // (nothing a tile would show as a fault)
        }
        QVERIFY(wall.shownWithin(5000));
    }

    void aConsoleThatSaysItsTimeIsUpAsSoonAsItIsOpenedIsNotOpenedInACircle()
    {
        // "Again at once" is for a session that has run its time. One that is ended within its first minute is a
        // console that cannot be had, and is asked for less and less often, as any other.
        Wall wall;
        wall.console.sessionLastsMs = 400;
        QVERIFY(wall.shownWithin(5000));
        wall.lookFor(2500);
        QCOMPARE(wall.console.sessions, 1);
    }

    void whatTheControllerSaidLastCountsThoughItHasHungUp()
    {
        // Found by a test of the web's that failed one time in ten: the controller had hung up in the very moment it
        // said why. "Its time was up", which is no fault and is put right at once, became "it closed its console".
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.settledMs = 300;
        Wall wall(limits);
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        QVERIFY(wall.shownWithin(5000));
        QTest::qWait(400);
        wall.console.hangsUp = true;
        wall.console.says.push_back(ami::message(ami::kind::stop, ami::timeUp));
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(ended.first().first().toString(), u"console_time_up"_s);
        wall.console.hangsUp = false;
        QVERIFY(wall.shownWithin(5000)); // opened again at once
        QCOMPARE(wall.wayIns, 2);
    }

    void aConsoleThatGoesQuietIsTakenToBeGone()
    {
        // (The desktop's own: the web's CONSOLE_QUIET.) A controller that says nothing for so long, not even "still
        // there?", has a connection that is dead; another may be had.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.quietMs = 600;
        limits.retryMs = {200, 200, 200, 200};
        Wall wall(limits);
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        QVERIFY(wall.shownWithin(5000));
        wall.lookFor(1000); // (one that asks "still there?" is not quiet, however long it shows the same screen)
        QVERIFY(ended.isEmpty());
        wall.console.mute = true;
        QTRY_VERIFY_WITH_TIMEOUT((wall.lookOnce(), ended.size() == 1), 5000);
        QCOMPARE(ended.first().first().toString(), u"dropped"_s);
        QVERIFY(ended.first().last().toString().contains(u"quiet"_s));
    }

    void aControllerThatAsksForTheConsoleOverAndOverIsNotAnsweredForEver()
    {
        // Every "may this one have the console?" was answered with two messages, as often as it was asked.
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        for (int asked = 0; asked < 60; ++asked) {
            wall.console.asks.emplace_back(ami::kind::sharing, somebody);
        }
        QTRY_VERIFY(wall.console.asks.empty());
        QTRY_VERIFY(wall.console.answers.size() >= 2);
        QTest::qWait(1000);
        QVERIFY(wall.console.answers.size() <= std::size_t{2} * 8);
        QVERIFY(wall.screen->look().ok()); // and the screen goes on being shown
    }

    void aPassTheConsoleDoesNotTakeIsNoPictureForAWhile()
    {
        // (The desktop's own, with the web's words.) The controller's console did not take the pass its own page gave
        // out: asking again soon does not change that.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.retryMs = {200, 200, 200, 200}; // (what asking again changes is asked again soon; this is not)
        Wall wall(limits);
        wall.wayIn.pass = "not-one-it-made"_ba;
        QTRY_COMPARE(wall.code(), std::string("no_picture"));
        QVERIFY(wall.message().contains("its console did not accept the pass it had given out"));
        wall.lookFor(500);
        QCOMPARE(wall.wayIns, 1);
    }

    void aPassThatCannotBeSaidIsNotSaid()
    {
        // (The desktop's own: the web's CONSOLE_PASS.) What travels in the console's own message is visible ASCII.
        Wall wall;
        wall.wayIn.pass = "a pass"_ba;
        QTRY_COMPARE(wall.code(), std::string("no_picture"));
        QCOMPARE(wall.console.askings, 0);
    }

    void aConsoleAskedForWithoutItsWebSessionIsNotOpened_data()
    {
        // (The web's _watch_console: a console that is not let in is said so, whatever its no.)
        QTest::addColumn<int>("refusal");
        QTest::newRow("not signed in (401)") << 401;
        QTest::newRow("not allowed (403)") << 403;
    }

    void aConsoleAskedForWithoutItsWebSessionIsNotOpened()
    {
        // (The desktop's own.) Its web page's session is what lets a console in.
        QFETCH(int, refusal);
        Wall wall;
        wall.console.refusesWith = refusal;
        wall.wayIn.cookie = "QSESSIONID=another"_ba;
        QTRY_COMPARE(wall.code(), std::string("dropped"));
        QCOMPARE(wall.message(), "The controller's console could not be opened"_ba);
        QCOMPARE(wall.console.sessions, 0);
    }

    void noWayInIsWhatTheDriverSaysItIs()
    {
        // (The desktop's own.) Where its web page gives no way in, that is what stands, and for as long as it says.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.retryMs = {200, 200, 200, 200};
        Wall wall(limits);
        wall.refuseWayIn = qkvm::base::Error{"no_picture", "The controller's web page did not sign this user in"};
        QTRY_COMPARE(wall.code(), std::string("no_picture"));
        wall.lookFor(500);
        QCOMPARE(wall.wayIns, 1);
        QCOMPARE(wall.console.askings, 0);
    }

    void aPictureThatCannotBeReadIsGivenUpOn()
    {
        // Another version of the firmware, another chip: what arrives is then not shown as a screen full of noise.
        Wall wall;
        wall.console.screen.clear();
        for (int frame = 0; frame < 4; ++frame) {
            Frame unknown(64, 32, {.strength = 9}); // a quantization strength there is none of
            unknown.colours({{0, white}});
            wall.console.screen.push_back(unknown);
        }
        QTRY_COMPARE(wall.code(), std::string("no_picture"));
        QVERIFY(wall.message().contains("does not read"));
        QTRY_COMPARE(wall.console.watching, 0);
        QCOMPARE(wall.shown, 0);
    }

    void whateverAConsoleSaysIsMetWithItsManners()
    {
        // (The desktop's own, by seeded chance, the same at every run.) Once the screen is shown, the console says
        // what it likes: every kind there is and some there are not, any number, any body, frames damaged and whole,
        // questions for the console without end. Whatever it says, nothing is said to it but what looking takes and
        // the letting in of whoever asks (at most as often as may), a session that ends ends for a reason there is,
        // and the screen handed out is a whole picture with what changed on it.
        static const std::set<std::string> reasons = {"",       "dropped",      "bad_response",    "no_picture",
                                                      "in_use", "console_held", "console_time_up", "timeout"};
        qkvm::bmc::tests::Chance chance(29);
        int ends = 0;
        for (int round = 0; round < 12; ++round) {
            Wall wall;
            bool whole = true;
            wall.screen.reset(); // (made anew, to look at every picture handed out)
            wall.screen = std::make_unique<ServerScreen>(
                [&wall](const ServerScreen::Answer& answer, const ServerScreen::Wanted& /*wanted*/) {
                    ++wall.wayIns;
                    const QByteArray pass = "pass-" + QByteArray::number(wall.wayIns);
                    wall.console.passes.insert(pass);
                    ServerScreen::WayIn way = wall.wayIn;
                    way.address = wall.console.address();
                    way.pass = pass;
                    answer(way);
                },
                [&whole](const aspeed::Picture& picture, std::span<const aspeed::Rect> changed) {
                    whole = whole && picture.rgb.size() == std::size_t{3} * static_cast<std::size_t>(picture.width) *
                                                               static_cast<std::size_t>(picture.height);
                    for (const aspeed::Rect& one : changed) {
                        whole = whole && one.x >= 0 && one.y >= 0 && one.x + one.width <= picture.width &&
                                one.y + one.height <= picture.height;
                    }
                },
                Wall::shortLimits());
            const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
            QVERIFY(wall.shownWithin(5000));
            static constexpr std::array<std::uint16_t, 16> kinds = {ami::kind::video,
                                                                    ami::kind::video,
                                                                    ami::kind::stillThere,
                                                                    ami::kind::sharing,
                                                                    ami::kind::nextHolder,
                                                                    ami::kind::others,
                                                                    ami::kind::licence,
                                                                    ami::kind::mouseMode,
                                                                    ami::kind::passSeen,
                                                                    ami::kind::allowed,
                                                                    ami::kind::stop,
                                                                    ami::kind::refused,
                                                                    ami::kind::tooMany,
                                                                    ami::kind::webSession,
                                                                    0,
                                                                    0xFFFF};
            for (int said = chance.between(5, 80); said > 0; --said) {
                const std::uint16_t kind =
                    kinds[static_cast<std::size_t>(chance.below(static_cast<int>(kinds.size())))];
                const auto status =
                    static_cast<std::uint16_t>(chance.oneIn(2) ? chance.below(65536) : chance.below(10));
                ami::Bytes body(static_cast<std::size_t>(chance.oneIn(3) ? chance.below(700) : chance.below(12)));
                for (std::uint8_t& byte : body) {
                    byte = static_cast<std::uint8_t>(chance.below(256));
                }
                if (kind == ami::kind::video && chance.oneIn(2) && body.size() >= 2 + ami::frameFacts) {
                    body[2 + 53] = 0; // (not encrypted: read on)
                    body[2 + 71] = 0; // (not longer than it may be)
                }
                if (kind == ami::kind::passSeen && chance.oneIn(2) && !body.empty()) {
                    body[0] = 1;
                }
                wall.console.says.push_back(ami::message(kind, status, body));
            }
            QTest::qWait(300);
            wall.lookFor(200);
            QVERIFY(whole);
            for (const auto& one : wall.console.heard) {
                const bool lettingIn = (one.kind == ami::kind::sharing || one.kind == ami::kind::nextHolder) &&
                                       one.status == ami::sharing::asked;
                QVERIFY2(looking.contains(one.kind) || lettingIn, QByteArray::number(one.kind));
            }
            QVERIFY(wall.console.answers.size() <= std::size_t{2} * 8 * static_cast<std::size_t>(wall.wayIns));
            for (const auto& one : ended) {
                QVERIFY2(reasons.contains(one.first().toString().toStdString()), qPrintable(one.first().toString()));
            }
            const aspeed::Picture& picture = wall.screen->picture();
            QCOMPARE(picture.rgb.size(), std::size_t{3} * static_cast<std::size_t>(picture.width) *
                                             static_cast<std::size_t>(picture.height));
            ends += static_cast<int>(ended.size());
        }
        qInfo("12 consoles said what they liked; %d sessions ended", ends);
    }

    void aConsoleThatWorkedAndThenFailedIsAskedForAgainSoon()
    {
        // (The desktop's own, as the web's counts.) What came to nothing before a session that showed the screen is
        // not held against the next: its first failure is a first one.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.retryMs = {200, 30000, 30000, 30000};
        limits.quietMs = 600;
        Wall wall(limits);
        wall.console.seats = 0;
        QTRY_VERIFY((wall.lookOnce(), wall.code() == "in_use"));
        wall.console.seats = 2;
        QVERIFY(wall.shownWithin(5000));
        wall.console.mute = true;
        QTRY_VERIFY_WITH_TIMEOUT((wall.lookOnce(), wall.code() == "dropped"), 5000);
        wall.console.mute = false;
        QVERIFY(wall.shownWithin(3000)); // asked again after the first wait, not the second
        QCOMPARE(wall.wayIns, 3);
    }

    void aFrameReadWellBetweenUnreadableOnesKeepsTheConsole()
    {
        // (The desktop's own, as the web's counts: in a row.) Two that cannot be read, one that can, two more: no more
        // than two in a row, and the screen is shown on.
        Wall wall;
        Frame unknown(64, 32, {.strength = 9});
        unknown.colours({{0, white}});
        wall.console.screen = {unknown, unknown, aScreen().front(), unknown, unknown};
        QVERIFY(wall.shownWithin(5000));
        wall.lookFor(500);
        QVERIFY(wall.screen->look().ok());
    }

    void aPassTakenTwiceInOneSessionEndsIt()
    {
        // Said a second time in one session, which a controller does not: what it means is not known.
        Wall wall;
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        QVERIFY(wall.shownWithin(5000));
        wall.console.says.push_back(ami::message(ami::kind::passSeen, 0, ami::Bytes{1, 0}));
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(ended.first().first().toString(), u"bad_response"_s);
    }

    void whatIsLongerThanANameIsNotSaidBack()
    {
        // (The web's WHO.) Who asks for the console, as the controller names them, is said back to let them in; more
        // than a name is not.
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        wall.console.asks.emplace_back(ami::kind::sharing, QByteArray(513, 'a'));
        wall.lookFor(500);
        QVERIFY(wall.console.answers.empty());
        wall.console.asks.emplace_back(ami::kind::sharing, QByteArray(512, 'a'));
        QTRY_COMPARE(wall.console.answers.size(), std::size_t{2});
        QCOMPARE(wall.console.answers[0].who, QByteArray(512, 'a'));
    }

    void theConsoleIsNotThisProgramsBeforeItsPassIsTakenNorOnceSwitchedAway()
    {
        // "The console is yours" counts once the pass is taken; and a console switched to another session is theirs.
        Wall early;
        early.console.holderReturning = true; // (it says nothing of whose the console is after the pass)
        early.console.saysFirst.push_back(ami::message(ami::kind::nextHolder, ami::sharing::becameMaster));
        QVERIFY(early.shownWithin(5000));
        QVERIFY(!early.screen->holdsTheConsole());
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        QVERIFY(wall.screen->holdsTheConsole());
        wall.console.says.push_back(ami::message(ami::kind::sharing, ami::sharing::becameMaster));
        QTRY_VERIFY(!wall.screen->holdsTheConsole());
        QVERIFY(wall.screen->look().ok()); // (and the screen is looked at on)
    }

    void aWayInThatComesAfterTheScreenWasClosedOpensNothing()
    {
        Wall wall;
        wall.deferWayIn = true;
        QCOMPARE(wall.code(), std::string("no_stream"));
        QVERIFY(wall.pending);
        wall.screen->close();
        ServerScreen::WayIn way = wall.wayIn;
        way.address = wall.console.address();
        way.pass = "a-console-pass"_ba;
        wall.pending(way);
        QTest::qWait(300);
        QCOMPARE(wall.console.askings, 0);
    }

    void aScreenThatChangesItsSizeTooOftenIsLeft()
    {
        // (A review of the desktop's.) A new size is a new picture of up to 24 MiB, for a message of a hundred bytes:
        // three changes in a second are a server that starts, more are not a screen being shown.
        Wall wall;
        wall.console.screen.clear();
        for (int frame = 0; frame < 6; ++frame) {
            Frame one(frame % 2 ? 32 : 64, frame % 2 ? 16 : 32);
            one.colours({{0, white}});
            wall.console.screen.push_back(one);
        }
        QTRY_COMPARE(wall.code(), std::string("no_picture"));
        QVERIFY(wall.message().contains("changes its size"));
        Wall starting;
        starting.console.screen.clear();
        for (const auto& [width, height] :
             {std::pair{64, 32}, std::pair{32, 16}, std::pair{64, 32}, std::pair{48, 24}}) {
            Frame one(width, height);
            one.colours({{0, white}});
            starting.console.screen.push_back(one);
        }
        QVERIFY(starting.shownWithin(5000));
        starting.lookFor(300);
        QVERIFY(starting.screen->look().ok());
        QCOMPARE(starting.screen->picture().width, 48);
        // ...and as many again more than a second later: it is the changes in one second that count.
        QTest::qWait(1100);
        for (const auto& [width, height] : {std::pair{64, 32}, std::pair{32, 16}, std::pair{64, 32}}) {
            Frame one(width, height);
            one.colours({{0, white}});
            for (const ami::Bytes& part : starting.console.pieces(one)) {
                starting.console.says.push_back(part);
            }
        }
        QTRY_VERIFY(starting.console.says.empty());
        starting.lookFor(300);
        QVERIFY(starting.screen->look().ok());
        QCOMPARE(starting.screen->picture().width, 64);
    }

    void aMessageLongerThanTheConsoleSendsEndsTheLine()
    {
        // (A review of the desktop's.) The line is held to what one message of the console may be.
        Wall wall;
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        QVERIFY(wall.shownWithin(5000));
        wall.console.says.push_back(
            ami::message(ami::kind::video, 0, ami::Bytes(ami::largestArrival - ami::headBytes + 1)));
        QTRY_VERIFY_WITH_TIMEOUT(ended.size() == 1, 15000);
        QCOMPARE(ended.first().first().toString(), u"bad_response"_s);
        // (The line's own words: it is not taken whole before it is refused.)
        QVERIFY(ended.first().last().toString().contains(u"does not accept"_s));
    }

    // --- after the reading of 3b ------------------------------------------------------------------------------------

    void aWayInIsGivenBackOnEveryRoad()
    {
        // (A review of the desktop's, and the web's test_a_device_taken_off_the_wall_in_the_moment_it_is_signed_in_to
        // _leaves_no_session_behind.) Every way in carries a session of the controller's web page, one of its few:
        // it is given back once, after the line has gone, however the session ends, and also where it was never
        // opened. Closed:
        {
            Wall wall;
            QVERIFY(wall.shownWithin(5000));
            wall.screen->close();
            QTRY_COMPARE(*wall.givenBack, 1);
            QCOMPARE(wall.console.watching, 0);
        }
        // ...thrown away with its session open (and the goodbye said all the same):
        {
            Wall wall;
            QVERIFY(wall.shownWithin(5000));
            wall.screen.reset();
            QTRY_COMPARE(*wall.givenBack, 1);
            QTRY_COMPARE(wall.console.watching, 0);
            QCOMPARE(wall.console.heard.back().kind, ami::kind::stop);
        }
        // ...ended by the controller, and given back when nobody looks, one way each:
        {
            ServerScreen::Limits limits = Wall::shortLimits();
            limits.idleMs = 300;
            Wall wall(limits);
            wall.console.seats = 0;
            QTRY_VERIFY((wall.lookOnce(), wall.code() == "in_use"));
            QTRY_COMPARE(*wall.givenBack, 1);
            wall.screen->close();
        }
        // ...a pass that cannot be said: no line, and given back at once:
        {
            Wall wall;
            wall.wayIn.pass = "a pass"_ba;
            QTRY_COMPARE(wall.code(), std::string("no_picture"));
            QCOMPARE(*wall.givenBack, 1);
        }
        // ...and the way that comes after nobody waits for it any more: the screen closed, or thrown away, while its
        // web page was signing in. No console is opened with it, and it is given back.
        for (const bool thrownAway : {false, true}) {
            Wall wall;
            wall.deferWayIn = true;
            QCOMPARE(wall.code(), std::string("no_stream"));
            QVERIFY(wall.pending);
            if (thrownAway) {
                wall.screen.reset();
            } else {
                wall.screen->close();
            }
            ServerScreen::WayIn way = wall.wayIn;
            way.address = wall.console.address();
            way.pass = "a-console-pass"_ba;
            way.done = [count = wall.givenBack] { ++*count; };
            wall.pending(way);
            QCOMPARE(*wall.givenBack, 1);
            QTest::qWait(200);
            QCOMPARE(wall.console.askings, 0);
        }
    }

    void nobodyLookingIsSeenBetweenFrames()
    {
        // (A review of the desktop's.) The line brings what it has in one go: thousands of frames that are each read
        // and shown. Nobody looking any more is seen between two of them, and the rest are not read.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.idleMs = 300;
        Wall wall(limits);
        std::vector<ami::Bytes> burst(1); // (messages of the line of a hundred of the console's each, as it may send)
        int inThisOne = 0;
        for (int frame = 0; frame < 3000; ++frame) {
            Frame some(64, 32);
            for (int block = 0; block < 32; ++block) {
                some.colours({{0, frame % 2 ? white : black}});
            }
            for (const ami::Bytes& part : wall.console.pieces(some)) {
                if (++inThisOne > 100) {
                    burst.emplace_back();
                    inThisOne = 1;
                }
                burst.back().insert(burst.back().end(), part.begin(), part.end());
            }
        }
        QVERIFY(wall.shownWithin(5000));
        const int before = wall.shown;
        // The first of them takes its time to be shown, as a busy screen does: nobody looks meanwhile, and what came
        // with it is on the line already.
        wall.onShown = [&wall, before] {
            if (wall.shown == before + 1) {
                QThread::msleep(400);
            }
        };
        wall.console.says.insert(wall.console.says.end(), burst.begin(), burst.end());
        QTRY_COMPARE(wall.console.watching, 0);
        QCOMPARE(wall.shown - before, 1); // (nothing after it was read)
    }

    void aBurstOfSizesDoesNotHoldTheThread()
    {
        // (A review of the desktop's.) Frames of nothing that each say another size, of the largest screens there
        // are, a few hundred in one message of the line: each would make a new picture of 24 MiB. It is left after
        // a few, at once, and does not come back soon.
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        ami::Bytes burst;
        for (int frame = 0; frame < 256; ++frame) {
            const Frame nothing(frame % 2 ? 2040 : 2048, frame % 2 ? 2040 : 2048);
            for (const ami::Bytes& part : wall.console.pieces(nothing)) {
                burst.insert(burst.end(), part.begin(), part.end());
            }
        }
        QElapsedTimer took;
        took.start();
        wall.console.says.push_back(burst);
        QTRY_COMPARE(wall.code(), std::string("no_picture"));
        QVERIFY(took.elapsed() < 3000);
        QTRY_COMPARE(wall.console.watching, 0);
    }

    void aSecondSeatIsNotTakenWhileTheFirstIsGivenBack()
    {
        // (A review of the desktop's.) The console's seat is free when its line has gone, not when the goodbye was
        // said: a console that is slow to take it holds the seat until then, and no other session is opened meanwhile
        // (somebody opening the console then would be told every seat is taken).
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.idleMs = 300;
        limits.goodbyeWithinMs = 1000;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        wall.console.deaf = true;
        QTest::qWait(500); // (nobody looks: given back, and the goodbye waits)
        int mostWatching = 0;
        QElapsedTimer waited;
        waited.start();
        std::set<std::string> said;
        while (wall.console.sessions < 2 && waited.elapsed() < 8000) {
            said.insert(wall.code());
            mostWatching = std::max(mostWatching, wall.console.watching);
            QTest::qWait(20);
        }
        QCOMPARE(mostWatching, 1);
        QVERIFY(said.contains("no_stream"));
        QCOMPARE(*wall.givenBack, 1);
        wall.console.deaf = false;
    }

    void aConsoleThatTakesNothingIsGivenBackAllTheSame()
    {
        // The web's test_a_controller_that_stops_listening_does_not_keep_the_console_open_for_ever and
        // test_a_server_taken_off_the_wall_while_its_console_takes_its_time_over_the_goodbye_gives_the_console_back:
        // a controller that stops taking what is said to it, the goodbye too. Given back when nobody looks, and when
        // the screen is thrown away, within the goodbye's time.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.idleMs = 400;
        limits.goodbyeWithinMs = 800;
        {
            Wall wall(limits);
            QVERIFY(wall.shownWithin(5000));
            wall.console.deaf = true;
            QTRY_VERIFY_WITH_TIMEOUT(wall.console.watching == 0, 8000);
            QTRY_COMPARE(*wall.givenBack, 1);
        }
        {
            Wall wall(limits);
            QVERIFY(wall.shownWithin(5000));
            wall.console.deaf = true;
            QTest::qWait(100);
            wall.screen.reset();
            QTRY_VERIFY_WITH_TIMEOUT(wall.console.watching == 0, 8000);
            QTRY_COMPARE(*wall.givenBack, 1);
        }
    }

    void theConsoleIsOpenedOnlyWithTheControllerItIsKnownBy()
    {
        // (A review of the desktop's.) Over TLS, the controller is the one its certificate says before the cookie of
        // its web page and the pass go out: with another certificate nothing is asked of it at all.
        {
            Wall wall(Wall::shortLimits(), tlsOf("first"));
            wall.wayIn.knownByCertificate = true;
            wall.wayIn.certificate = digestOf("first");
            QVERIFY(wall.shownWithin(8000));
        }
        Wall wall(Wall::shortLimits(), tlsOf("first"));
        wall.wayIn.knownByCertificate = true;
        wall.wayIn.certificate = digestOf("second");
        QTRY_COMPARE(wall.code(), std::string("cert_mismatch"));
        QCOMPARE(wall.console.askings, 0);
        QVERIFY(wall.console.heard.empty());
        QCOMPARE(*wall.givenBack, 1);
    }

    void aScreenInLargePiecesOrTinyOnesIsShown()
    {
        // (A review of the desktop's: the line's limits for a console.) A busy screen in pieces of a quarter of a
        // megabyte, over a megabyte in a moment; and one in pieces of four bytes, thousands of messages in a moment.
        Wall large;
        large.console.screen = {aBusyScreen(1920, 1080, 1), aBusyScreen(1920, 1080, 2)};
        large.console.piece = 256 * 1024;
        std::size_t bytes = 0;
        for (const Frame& one : large.console.screen) {
            bytes += one.data().size();
        }
        QVERIFY2(bytes > std::size_t{2} * 1024 * 1024, QByteArray::number(static_cast<qulonglong>(bytes)));
        QVERIFY(large.shownWithin(20000));
        QTRY_VERIFY(large.shown >= 2);
        Wall tiny;
        tiny.console.screen = {aBusyScreen(256, 192, 3)};
        tiny.console.piece = 4;
        QVERIFY2(tiny.console.screen.front().data().size() / 4 > 5000,
                 QByteArray::number(static_cast<qulonglong>(tiny.console.screen.front().data().size())));
        QVERIFY(tiny.shownWithin(15000));
    }

    void aBusyScreenIsReadOffTheInterfaceThread()
    {
        // Review 3's HIGH 1 (2026-11-06). A server's screen that is busy (a scrolling log, an installer) is read frame
        // after frame on a thread of its own: the interface thread, which has the console's line, the screen and its
        // live, works for no longer than about one frame of a monitor (16 ms) at a time while the frames come. (It
        // worked for as long as each frame took to be read: about 46 ms on a slow core in the build that is handed
        // out, and longer in this one.) What the live hands on, packet after packet, makes the screen the frames make
        // when they are read in their order: none lost and none out of turn. And no more than two frames wait to be
        // read meanwhile: the console is held back, as it was.
        std::vector<Frame> frames;
        for (std::uint64_t seed = 101; seed < 107; ++seed) {
            frames.push_back(aBusyScreen(1920, 1080, seed));
        }
        aspeed::Screen reference;
        for (const Frame& frame : frames) {
            QVERIFY(reference.read(frame.header(), frame.data()).ok());
        }
        Elsewhere elsewhere;
        const Elsewhere::Kept<AmiConsoleByHand> console(elsewhere);
        QUrl address;
        elsewhere.there([&] {
            console->screen = frames;
            console->piece = 256 * 1024;
            console->passes.insert("busy-pass"_ba);
            address = console->address();
        });
        int shown = 0;
        int mostWaiting = 0;
        qkvm::core::ServerLive* live = nullptr;
        auto screen = std::make_unique<ServerScreen>(
            [&address](const ServerScreen::Answer& answer, const ServerScreen::Wanted& /*wanted*/) {
                ServerScreen::WayIn way;
                way.address = address;
                way.cookie = "QSESSIONID=s3ss10n"_ba;
                way.pass = "busy-pass"_ba;
                way.ownAddress = "192.168.1.20"_ba;
                way.controllerAddress = "192.168.1.50"_ba;
                way.webSession = "1234"_ba;
                answer(way);
            },
            [&shown, &live](const aspeed::Picture& picture, std::span<const aspeed::Rect> changed) {
                ++shown;
                if (live != nullptr) {
                    live->shown(picture, changed);
                }
            },
            Wall::shortLimits());
        qkvm::core::ServerLive serverLive(screen.get(), {});
        live = &serverLive;
        std::vector<QByteArray> packets;
        connect(&serverLive, &qkvm::core::Live::pictureCame, this,
                [&packets](const qkvm::core::Live::Picture& picture) { packets.push_back(picture.data); });
        QTimer looks;
        connect(&looks, &QTimer::timeout, this, [&screen, &mostWaiting] {
            (void)screen->look();
            mostWaiting = std::max(mostWaiting, screen->framesWaiting());
        });
        (void)screen->look();
        looks.start(20);
        Stillness stillness;
        stillness.start();
        // Until every frame has been read and shown: nothing waits, and nothing new was shown for a while.
        QElapsedTimer took;
        took.start();
        QElapsedTimer quiet;
        quiet.start();
        int seen = 0;
        while (took.elapsed() < 120000 && (shown == 0 || screen->framesWaiting() > 0 || quiet.elapsed() < 3000)) {
            qkvm::core::testing::runFor(20);
            if (shown != seen) {
                seen = shown;
                quiet.restart();
            }
        }
        stillness.stop();
        looks.stop();
        const QByteArray said = stillness.said();
        qInfo("A busy 1080p screen, %d frames shown of %zu, in %lld ms: the interface thread was %s; %d frames waited "
              "at the most",
              shown, frames.size(), static_cast<long long>(took.elapsed()), said.constData(), mostWaiting);
        // What the interface thread did itself in any one wait: within a monitor's frame (under the address sanitizer,
        // within what tells a frame read on it). And how long it was still at all, what it waited for and what Windows
        // still takes from a thread above the PC's other work among it (stillness.h says why the two are apart).
        QVERIFY2(stillness.longestWorkedMs() < qkvm::core::testing::mostWorkInOneWaitMs, said.constData());
        QVERIFY2(stillness.longestMs() < qkvm::core::testing::mostStillMs, said.constData());
        QVERIFY2(mostWaiting <= 2, QByteArray::number(mostWaiting));
        // The screen as the frames make it, and as the live's packets make it.
        QCOMPARE(screen->picture().width, 1920);
        QCOMPARE(screen->picture().height, 1080);
        QVERIFY(screen->picture().rgb == reference.picture().rgb);
        std::vector<std::uint8_t> made;
        int width = 0;
        int height = 0;
        for (const QByteArray& packet : packets) {
            const auto read = qkvm::base::screen::read(
                std::as_bytes(std::span(packet.constData(), static_cast<std::size_t>(packet.size()))));
            QVERIFY(read.has_value());
            const qkvm::base::screen::Changes changes = read.value_or(qkvm::base::screen::Changes{});
            if (changes.width != width || changes.height != height) {
                QVERIFY(changes.whole());
                width = changes.width;
                height = changes.height;
                made.assign(std::size_t{3} * static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
            }
            std::size_t from = 0;
            for (const qkvm::base::screen::Rect& rect : changes.rects) {
                const auto across = std::size_t{3} * static_cast<std::size_t>(rect.width);
                for (int y = rect.y; y < rect.y + rect.height; ++y) {
                    std::memcpy(made.data() +
                                    std::size_t{3} * (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                      static_cast<std::size_t>(rect.x)),
                                changes.pixels.data() + from, across);
                    from += across;
                }
            }
        }
        QVERIFY(made == reference.picture().rgb);
        screen->close();
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                int watching = -1;
                elsewhere.there([&] { watching = console->watching; });
                return watching == 0;
            }(),
            5000);
        screen.reset();
    }

    void aBusyInterfaceThreadIsHandedTheNewestPicture()
    {
        // (Review 3's HIGH 1.) Where the interface thread is held up while frames are read, it is handed the screen as
        // it stands when it comes back, with all that changed since the last it was shown: one picture for the frames
        // read meanwhile, never a pile of them (no more than two frames wait to be read, and the console is held back
        // by that). What the live hands on still makes the screen the frames make.
        std::vector<Frame> frames;
        for (std::uint64_t seed = 201; seed < 225; ++seed) {
            frames.push_back(aBusyScreen(320, 240, seed));
        }
        aspeed::Screen reference;
        for (const Frame& frame : frames) {
            QVERIFY(reference.read(frame.header(), frame.data()).ok());
        }
        Wall wall;
        wall.console.screen = frames;
        wall.console.piece = 256 * 1024;
        int mostWaiting = 0;
        int waitingAtTheFirst = -1;
        wall.onShown = [&wall, &mostWaiting, &waitingAtTheFirst] {
            mostWaiting = std::max(mostWaiting, wall.screen->framesWaiting());
            if (wall.shown == 1) {
                waitingAtTheFirst = wall.screen->framesWaiting();
            }
        };
        // The reader kept from reading until two frames wait for it: Qt's pool, which it runs on, held to one thread,
        // and that one busy until it is let go. (Before, the interface thread was held 600 ms at its first picture, and
        // two frames were to be read meanwhile; on the loaded slow cores the reader was not given the processor within
        // it once, and every frame was shown by itself.)
        QThreadPool* const pool = QThreadPool::globalInstance();
        const int threads = pool->maxThreadCount();
        QSemaphore readerMayGoOn;
        pool->setMaxThreadCount(1);
        pool->start([&readerMayGoOn] { readerMayGoOn.acquire(); });
        const auto restore = qScopeGuard([&] {
            readerMayGoOn.release();
            static_cast<void>(pool->waitForDone(30000));
            pool->setMaxThreadCount(threads);
        });
        QVERIFY(QTest::qWaitFor(
            [&wall] {
                wall.lookOnce();
                return wall.screen->framesWaiting() >= 2;
            },
            30000));
        QCOMPARE(wall.shown, 0);
        // The interface thread is busy with something else (this test) while the reader reads both and hands them over:
        // its run is over when the pool has nothing more to do. Its next turn is handed the two as one picture.
        readerMayGoOn.release();
        QVERIFY(pool->waitForDone(30000));
        QCOMPARE(wall.shown, 0);
        QTRY_VERIFY(wall.shown >= 1);
        QCOMPARE(waitingAtTheFirst, 0);
        QElapsedTimer quiet;
        quiet.start();
        int seen = 0;
        QElapsedTimer took;
        took.start();
        while (took.elapsed() < 60000 &&
               (wall.shown == 0 || wall.screen->framesWaiting() > 0 || quiet.elapsed() < 2000)) {
            wall.lookOnce();
            mostWaiting = std::max(mostWaiting, wall.screen->framesWaiting());
            QTest::qWait(10);
            if (wall.shown != seen) {
                seen = wall.shown;
                quiet.restart();
            }
        }
        QVERIFY2(mostWaiting <= 2, QByteArray::number(mostWaiting));
        QVERIFY2(wall.shown < static_cast<int>(frames.size()), QByteArray::number(wall.shown));
        // A card asks for its still once nothing changes any more: what it is handed back with it changes nothing of
        // the screen as it stands.
        bool stillCame = false;
        wall.screen->still(320, [&stillCame](const qkvm::base::Result<QByteArray>& still) { stillCame = still.ok(); });
        QTRY_VERIFY(stillCame);
        QVERIFY(wall.screen->picture().rgb == reference.picture().rgb);
    }

    void aScreenChangedBitByBitIsShownBitByBit()
    {
        // (Review 3's HIGH 1: what is read goes back through a picture of its own.) A screen, and then one block of it
        // after another, each frame a moment after the one before: each is shown as what it changed, and the screen
        // that is shown is the one the frames make.
        Wall wall;
        std::vector<Frame> frames = aScreen();
        Frame third(64, 32);
        third.colours({{2, black}}, {}, std::pair{1, 1});
        Frame fourth(64, 32);
        fourth.colours({{2, white}}, {}, std::pair{5, 0});
        frames.push_back(third);
        frames.push_back(fourth);
        aspeed::Screen reference;
        for (const Frame& frame : frames) {
            QVERIFY(reference.read(frame.header(), frame.data()).ok());
        }
        wall.console.screen = frames;
        wall.console.frameEveryMs = 150;
        QVERIFY(wall.shownWithin(5000));
        QTRY_VERIFY(wall.shown >= 4);
        QCOMPARE(wall.rects, (std::vector<aspeed::Rect>{{40, 0, 8, 8}}));
        QVERIFY(wall.screen->picture().rgb == reference.picture().rgb);
    }

    void aScreenThatChangedWholeAndThenAtTheEndsOfARowIsShownAsItIs()
    {
        // (Review 3's HIGH 1.) A screen, then all of it red (handed to the screen whole, twice now), then a block at
        // each end of its top row, in one message of the line (shown as one picture): shown as the frames make it, the
        // blocks between the two red, not as they were before.
        Wall wall;
        Frame allRed(64, 32);
        for (int block = 0; block < 32; ++block) {
            allRed.colours({{0, red}});
        }
        Frame left(64, 32);
        left.colours({{2, black}}, {}, std::pair{0, 0});
        Frame right(64, 32);
        right.colours({{2, black}}, {}, std::pair{7, 0});
        const std::vector<Frame> frames = {aScreen().front(), allRed, left, right};
        aspeed::Screen reference;
        for (const Frame& frame : frames) {
            QVERIFY(reference.read(frame.header(), frame.data()).ok());
        }
        wall.console.screen = {frames[0], frames[1]};
        wall.console.frameEveryMs = 150;
        QVERIFY(wall.shownWithin(5000));
        QTRY_VERIFY(wall.shown >= 2);
        ami::Bytes both;
        for (const Frame* frame : {&left, &right}) {
            for (const ami::Bytes& part : wall.console.pieces(*frame)) {
                both.insert(both.end(), part.begin(), part.end());
            }
        }
        wall.console.says.push_back(both);
        QTRY_VERIFY(wall.shown >= 3 && wall.screen->framesWaiting() == 0);
        QTest::qWait(200);
        QVERIFY(wall.screen->picture().rgb == reference.picture().rgb);
    }

    void aScreenOnAThreadOfItsOwnClosesItsLineThere()
    {
        // (A review of the desktop's.) The screen lives on a thread that may wait for a frame; a line it gives back is
        // kept until it has gone on that thread, and given back there, and nothing is asked of another thread's.
        QTest::failOnWarning(QRegularExpression(u"different thread|Cannot set parent|Cannot create children"_s));
        AmiConsoleByHand console;
        console.screen = aScreen();
        for (int pass = 1; pass <= 3; ++pass) {
            console.passes.insert("thread-pass-" + QByteArray::number(pass));
        }
        const QUrl address = console.address();
        std::atomic<int> asked = 0;
        std::atomic<int> givenBack = 0;
        QThread worker;
        worker.start();
        QObject host;
        host.moveToThread(&worker);
        std::unique_ptr<ServerScreen> screen;
        const auto there = [&host](const std::function<void()>& what) {
            QMetaObject::invokeMethod(&host, what, Qt::BlockingQueuedConnection);
        };
        there([&] {
            screen = std::make_unique<ServerScreen>(
                [&](const ServerScreen::Answer& answer, const ServerScreen::Wanted& /*wanted*/) {
                    ServerScreen::WayIn way;
                    way.address = address;
                    way.cookie = "QSESSIONID=s3ss10n"_ba;
                    way.pass = "thread-pass-" + QByteArray::number(++asked);
                    way.done = [&givenBack] { ++givenBack; };
                    answer(way);
                },
                [](const aspeed::Picture&, std::span<const aspeed::Rect>) {}, Wall::shortLimits());
        });
        bool shown = false;
        QElapsedTimer waited;
        waited.start();
        while (!shown && waited.elapsed() < 8000) {
            there([&] { shown = screen->look().ok(); });
            QTest::qWait(30);
        }
        QVERIFY(shown);
        there([&] { screen->close(); });
        QTRY_COMPARE(console.watching, 0);
        QTRY_COMPARE(givenBack.load(), 1);
        there([&] { screen.reset(); });
        worker.quit();
        QVERIFY(worker.wait(5000));
    }

    void whoeverIsShownTheScreenOrHearsItsEndMayCloseItOrThrowItAway()
    {
        // (A review of the desktop's.) Shown what a refused frame drew, the callback closes the screen; told that a
        // session ended, whoever heard it throws the screen away: in the middle of what the line brought, and nothing
        // of the screen is touched after that.
        {
            Wall wall;
            QVERIFY(wall.shownWithin(5000));
            wall.onShown = [&wall] { wall.screen->close(); };
            Frame tooMany(64, 32);
            for (int block = 0; block < 2 * 8 * 4 + 64 + 2; ++block) {
                tooMany.colours({{0, red}});
            }
            ami::Bytes all;
            for (const ami::Bytes& part : wall.console.pieces(tooMany)) {
                all.insert(all.end(), part.begin(), part.end());
            }
            wall.console.says.push_back(all);
            QTRY_COMPARE(*wall.givenBack, 1);
        }
        {
            Wall wall;
            QVERIFY(wall.shownWithin(5000));
            connect(wall.screen.get(), &ServerScreen::ended, this, [&wall] { wall.screen.reset(); });
            ami::Bytes both = ami::message(ami::kind::stop, ami::timeUp);
            const ami::Bytes more = ami::message(ami::kind::stillThere);
            both.insert(both.end(), more.begin(), more.end());
            wall.console.says.push_back(both);
            QTRY_VERIFY(wall.screen == nullptr);
            QTRY_COMPARE(*wall.givenBack, 1);
        }
    }

    void aWayThatIsNotFitToBeSentIsNoPictureForAWhile()
    {
        // (A review of the desktop's.) What the controller handed out cannot be said in the asking for its console:
        // asking again soon would not change that, and each time its web page would be signed in to anew.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.retryMs = {200, 200, 200, 200};
        Wall wall(limits);
        wall.wayIn.cookie = "QSESSIONID=a b"_ba;
        QTRY_COMPARE(wall.code(), std::string("no_picture"));
        wall.lookFor(500);
        QCOMPARE(wall.wayIns, 1);
        QCOMPARE(wall.console.askings, 0);
    }

    void aSizeTheScreenRefusesIsNoChangeOfItsSize()
    {
        // (The reading of 3b's follow-ups.) What counts against a screen that changes its size is a picture made anew:
        // a frame of a size no screen is (0 by 0) is refused, and the picture held is the one it was before. (Five of
        // them, between six of the screen: more than a screen may change its size in a second, counted either way.)
        Wall wall;
        wall.console.screen.clear();
        for (int frame = 0; frame < 11; ++frame) {
            Frame one(frame % 2 ? 0 : 64, frame % 2 ? 0 : 32);
            if (frame % 2 == 0) {
                one.colours({{0, white}});
            }
            wall.console.screen.push_back(one);
        }
        QVERIFY(wall.shownWithin(5000));
        const std::set<std::string> seen = wall.lookFor(500);
        QVERIFY(!seen.contains("no_picture"));
        QVERIFY(wall.screen->look().ok());
        QCOMPARE(wall.screen->picture().width, 64);
        QCOMPARE(wall.console.sessions, 1);
    }

    void twoScreensOfOneControllerHoldOneSeat()
    {
        // (The reading of 3b's follow-ups.) The console's seat is the controller's, not a screen's: one screen thrown
        // away while its goodbye is slow, another made for the same controller is given no way in (no web session,
        // no pass) until the first one's line has gone.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.goodbyeWithinMs = 1500;
        AmiConsoleByHand console;
        console.screen = aScreen();
        auto givenBack = std::make_shared<int>(0);
        std::vector<int> givenBackWhenAsked;
        const ServerScreen::Asker oneSeat = ServerScreen::oneSeatAtATime(
            [&console, &givenBackWhenAsked, givenBack](const ServerScreen::Answer& answer,
                                                       const ServerScreen::Wanted& /*wanted*/) {
                givenBackWhenAsked.push_back(*givenBack);
                const QByteArray pass = "pass-" + QByteArray::number(static_cast<qsizetype>(givenBackWhenAsked.size()));
                console.passes.insert(pass);
                ServerScreen::WayIn way = Wall().wayIn;
                way.address = console.address();
                way.pass = pass;
                way.done = [givenBack] { ++*givenBack; };
                answer(way);
            });
        const auto shownNowhere = [](const aspeed::Picture&, std::span<const aspeed::Rect>) {};
        auto first = std::make_unique<ServerScreen>(oneSeat, shownNowhere, limits);
        QTRY_VERIFY(first->look().ok());
        console.deaf = true; // (its goodbye is never taken: its line goes when its time for that is up)
        first.reset();
        ServerScreen second(oneSeat, shownNowhere, limits);
        std::set<std::string> said;
        QElapsedTimer waited;
        waited.start();
        while (console.sessions < 2 && waited.elapsed() < 8000) {
            const auto looked = second.look();
            said.insert(looked ? std::string("shown") : looked.error().code);
            QTest::qWait(20);
        }
        console.deaf = false;
        QCOMPARE(console.sessions, 2);
        QCOMPARE(console.mostWatching, 1);
        QCOMPARE(givenBackWhenAsked, (std::vector<int>{0, 1}));
        QCOMPARE(said, (std::set<std::string>{"no_stream"}));
    }

    void oneControllersSeatIsOneForEveryDriverOfIt()
    {
        // Review 3's MEDIUM 2 (2026-11-06): the program's seats of its controllers' consoles. Two askers of one
        // controller, made apart (two drivers of it, each with its own way in), hold one seat between them: the second
        // asks its way in only once the first's way is done. The controller is found by its address (also where its
        // number is not known), and by its own number at another address of it; never by a certificate (none is asked
        // for). Another controller at another address, or the same address over another port, is another seat. Those
        // who wait are asked in turn. An asker whose driver is gone is answered "stopped", no seat taken.
        const auto seats = std::make_shared<qkvm::core::ConsoleSeats>();
        using Of = qkvm::core::ConsoleSeats::Of;
        std::vector<std::string> asked;
        // (Each by a name that lives as long as the test: no closure here owns anything that could throw being copied.)
        const auto wayOf = [&asked](const char* who) {
            return [&asked, who](const ServerScreen::Answer& answer, const ServerScreen::Wanted& /*wanted*/) {
                asked.emplace_back(who);
                answer(ServerScreen::WayIn{});
            };
        };
        std::vector<std::string> answered;
        const auto take = [&answered](const char* who, std::vector<ServerScreen::WayIn>& into) {
            return [&answered, &into, who](qkvm::base::Result<ServerScreen::WayIn> way) {
                answered.push_back(std::string(who) + (way ? std::string() : ":" + way.error().code));
                if (way) {
                    into.push_back(std::move(way).value());
                }
            };
        };
        const QUrl one(u"https://10.0.0.5"_s);
        const QUrl other(u"https://10.0.0.6"_s);
        const auto old = seats->asker([one] { return std::optional<Of>(Of{one, u"c0ffee"_s}); }, wayOf("old"));
        const auto anew = seats->asker([one] { return std::optional<Of>(Of{one, u"c0ffee"_s}); }, wayOf("new"));
        const auto plain = seats->asker([one] { return std::optional<Of>(Of{one, {}}); }, wayOf("plain"));
        const auto moved = seats->asker([other] { return std::optional<Of>(Of{other, u"c0ffee"_s}); }, wayOf("moved"));
        const auto elsewhere = seats->asker(
            [] { return std::optional<Of>(Of{QUrl(u"https://10.0.0.7"_s), u"beef00"_s}); }, wayOf("elsewhere"));
        const auto port =
            seats->asker([] { return std::optional<Of>(Of{QUrl(u"https://10.0.0.5:8443"_s), {}}); }, wayOf("port"));
        const auto gone = seats->asker([] { return std::optional<Of>(); }, wayOf("gone"));
        const auto always = [] { return true; };
        std::vector<ServerScreen::WayIn> ways;
        old(take("old", ways), always);
        anew(take("new", ways), always);
        plain(take("plain", ways), always);
        moved(take("moved", ways), always);
        elsewhere(take("elsewhere", ways), always);
        port(take("port", ways), always);
        gone(take("gone", ways), always);
        QTest::qWait(50);
        QCOMPARE(asked, (std::vector<std::string>{"old", "elsewhere", "port"}));
        QCOMPARE(answered, (std::vector<std::string>{"old", "elsewhere", "port", "gone:stopped"}));
        ways.front().done(); // (the old one's line has gone)
        QTRY_COMPARE(asked.size(), std::size_t{4});
        QCOMPARE(asked.back(), std::string("new"));
        QTest::qWait(50);
        QCOMPARE(asked.size(), std::size_t{4}); // (the others wait for the new one's)
        ways.back().done();
        QTRY_COMPARE(asked.size(), std::size_t{5});
        QCOMPARE(asked.back(), std::string("plain"));
        ways.back().done();
        QTRY_COMPARE(asked.size(), std::size_t{6});
        QCOMPARE(asked.back(), std::string("moved"));
        // (Kept under one, its number, other, 10.0.0.7 and its number, and one over another port.)
        QCOMPARE(seats->kept(), qsizetype{6});
    }

    void aSeatForWhichNoWayCameIsFree()
    {
        // (The reading of 3b's follow-ups.) Where the driver gives one screen no way in, no seat was taken: the next
        // screen of that controller is given its way.
        AmiConsoleByHand console;
        console.screen = aScreen();
        int asked = 0;
        const ServerScreen::Asker oneSeat = ServerScreen::oneSeatAtATime(
            [&console, &asked](const ServerScreen::Answer& answer, const ServerScreen::Wanted& /*wanted*/) {
                if (++asked == 1) {
                    answer(qkvm::base::Error{"no_picture", "The controller's web page refused"});
                    return;
                }
                console.passes.insert("pass-2");
                ServerScreen::WayIn way = Wall().wayIn;
                way.address = console.address();
                way.pass = "pass-2";
                answer(way);
            });
        const auto shownNowhere = [](const aspeed::Picture&, std::span<const aspeed::Rect>) {};
        ServerScreen first(oneSeat, shownNowhere, Wall::shortLimits());
        ServerScreen second(oneSeat, shownNowhere, Wall::shortLimits());
        (void)first.look();
        QCOMPARE(codeIn(first.look()), std::string("no_picture"));
        QTRY_VERIFY(second.look().ok());
        QCOMPARE(asked, 2);
    }

    void aRefusalInForceIsSaidWhileTheConsoleIsGivenBack()
    {
        // (The reading of 3b's follow-ups.) A console that said every seat is taken, and then took no goodbye: what it
        // said is what looking says while its line goes, and not that the console is being given back.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.goodbyeWithinMs = 2000;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        wall.console.deafAfterSaying = true;
        wall.console.says.push_back(ami::message(ami::kind::tooMany));
        QTRY_VERIFY(wall.code() != "shown");
        QCOMPARE(wall.lookFor(600), (std::set<std::string>{"in_use"}));
        QCOMPARE(wall.console.watching, 1); // (its line has not gone yet)
        wall.console.deaf = false;
    }

    // --- keys and the pointer (part 4) ---------------------------------------------------------------------------

    void aScreenClosedWhileItWaitsForTheSeatAsksForNothing()
    {
        // (The reading of part 4, L2.) A screen that waits for a controller's seat, and is closed or thrown away
        // meanwhile: when the seat is free, nothing is asked for it (no sign-in at the controller's web page, no pass).
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.goodbyeWithinMs = 1000;
        AmiConsoleByHand console;
        console.screen = aScreen();
        int asked = 0;
        const ServerScreen::Asker oneSeat = ServerScreen::oneSeatAtATime(
            [&console, &asked](const ServerScreen::Answer& answer, const ServerScreen::Wanted& wanted) {
                QVERIFY(wanted && wanted());
                ++asked;
                const QByteArray pass = "pass-" + QByteArray::number(asked);
                console.passes.insert(pass);
                ServerScreen::WayIn way = Wall().wayIn;
                way.address = console.address();
                way.pass = pass;
                answer(way);
            });
        const auto shownNowhere = [](const aspeed::Picture&, std::span<const aspeed::Rect>) {};
        auto first = std::make_unique<ServerScreen>(oneSeat, shownNowhere, limits);
        QTRY_VERIFY(first->look().ok());
        console.deaf = true; // (its goodbye is never taken: its line goes when its time for that is up)
        first.reset();
        ServerScreen closed(oneSeat, shownNowhere, limits);
        auto thrown = std::make_unique<ServerScreen>(oneSeat, shownNowhere, limits);
        (void)closed.look();
        (void)thrown->look();
        closed.close();
        thrown.reset();
        QTest::qWait(2500); // (the first one's line has gone, and the seat is free)
        console.deaf = false;
        QCOMPARE(asked, 1);
        QCOMPARE(console.askings, 1);
    }
};

QTEST_GUILESS_MAIN(TestServerScreen)
#include "test_server_screen.moc"
