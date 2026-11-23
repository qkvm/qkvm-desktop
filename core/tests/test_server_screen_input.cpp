#include <QTest>

#include "server_screen_wall.h"

// A server's own screen from its controller's console: keys, the pointer and typings over it (part 4; the web's
// redfish.py 1310-1512 and its live.RedfishHid), against a console by hand on this PC's own address. The session
// and its manners are test_server_screen.cpp's; keys behind what waits to leave, test_server_screen_backlog.cpp's.
using namespace qkvm::core::testing::screens;

class TestServerScreenInput : public QObject {
    Q_OBJECT
private slots:
    void keysTypedTextAndCtrlAltDelReachTheServersConsole()
    {
        // The web's: a server whose controller is AMI's takes the keyboard over the same console its screen comes on.
        // Each report is the whole keyboard; a combination goes down together, a typed character is pressed and let go
        // of, and nothing is left down. Asked for keys, the console is opened for them.
        Wall wall;
        const Outcome ctrlAltDel = anOutcome();
        const Outcome ctrlC = anOutcome();
        const Outcome text = anOutcome();
        wall.screen->press(u"ControlLeft,AltLeft,Delete"_s, {}, into(ctrlAltDel));
        wall.screen->press(u"ControlLeft,KeyC"_s, {}, into(ctrlC));
        wall.screen->type(u"Hi"_s, {}, into(text));
        QTRY_VERIFY_WITH_TIMEOUT(text->has_value(), 15000);
        QCOMPARE(codeOf(ctrlAltDel), std::string("done"));
        QCOMPARE(codeOf(ctrlC), std::string("done"));
        QCOMPARE(codeOf(text), std::string("done"));
        // (Read by the stand-in a moment after they were said.)
        QTRY_COMPARE(keysOf(wall.console).size(), std::size_t{8});
        QCOMPARE(keysOf(wall.console),
                 (std::vector<Keys>{
                     {0x05, {0x4C}}, letGo, {0x01, {0x06}}, letGo, {0x02, {0x0B}}, letGo, {0, {0x0C}}, letGo}));
        QVERIFY(wall.console.badHid.isEmpty()); // every report checked out
        QCOMPARE(wall.console.hidLost, 0);
        // (The desktop's own.) Numbered from one in the session, one higher each.
        for (std::size_t at = 0; at < wall.console.hid.size(); ++at) {
            QCOMPARE(wall.console.hid[at].number, static_cast<std::uint32_t>(at + 1));
        }
        for (const auto& one : wall.console.heard) {
            QVERIFY2(looking.contains(one.kind) || one.kind == ami::kind::hid,
                     QByteArray::number(one.kind).constData());
        }
    }

    void aCombinationIsHeldAndATextsKeysGoApart()
    {
        // (The desktop's own.) A combination is held as long as a person would; a text's keys a few milliseconds
        // apart, each let go of before the next.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.holdMs = 300;
        limits.keyPauseMs = 100;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const Outcome held = anOutcome();
        const Outcome text = anOutcome();
        wall.screen->press(u"ControlLeft,KeyC"_s, {}, into(held));
        wall.screen->type(u"ab"_s, {}, into(text));
        QTRY_VERIFY_WITH_TIMEOUT(text->has_value(), 10000);
        QCOMPARE(codeOf(text), std::string("done"));
        QTRY_COMPARE(wall.console.keyboards().size(), std::size_t{6});
        const std::vector<AmiConsoleByHand::Hid> keys = wall.console.keyboards();
        // (What the stand-in hears comes on a loaded PC late or early by some milliseconds: bounds below only.)
        QVERIFY2(keys[1].atMs - keys[0].atMs >= 200, QByteArray::number(keys[1].atMs - keys[0].atMs).constData());
        for (std::size_t at = 2; at < keys.size(); ++at) {
            QVERIFY2(keys[at].atMs - keys[at - 1].atMs >= 60,
                     QByteArray::number(keys[at].atMs - keys[at - 1].atMs).constData());
        }
        QCOMPARE(keysOf(wall.console),
                 (std::vector<Keys>{{0x01, {0x06}}, letGo, {0, {0x04}}, letGo, {0, {0x05}}, letGo}));
    }

    void anEnterAfterATextIsPressedInTheTextsOwnTyping()
    {
        // Outside review 2.3: a text with Enter after it is one typing, in one turn of the keyboard (as the web types
        // the text with a "\n" after it): its Enter never waits behind a typing asked for after it, nor asks for the
        // keyboard anew.
        Wall wall;
        const Outcome text = anOutcome();
        const Outcome after = anOutcome();
        wall.screen->type(u"a"_s, {}, into(text), true);
        wall.screen->press(u"KeyB"_s, {}, into(after));
        QTRY_VERIFY_WITH_TIMEOUT(after->has_value(), 15000);
        QCOMPARE(codeOf(text), std::string("done"));
        QCOMPARE(codeOf(after), std::string("done"));
        QTRY_COMPARE(keysOf(wall.console).size(), std::size_t{6});
        QCOMPARE(keysOf(wall.console), (std::vector<Keys>{{0, {0x04}}, letGo, {0, {0x28}}, letGo, {0, {0x05}}, letGo}));
    }

    void keysWaitForTheConsoleToBeThisProgramsAndGoNowhereElse()
    {
        // The web's (found by a review there): keys went once the pass was taken, before the controller said "the
        // console is yours". A console whose last holder may come back is shown, and its holder is another session:
        // nothing is typed into it.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.masterWaitMs = 500;
        Wall wall(limits);
        wall.console.holderReturning = true;
        QVERIFY(wall.shownWithin(5000));
        const Outcome typed = anOutcome();
        wall.screen->press(u"KeyA"_s, {}, into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 10000);
        QCOMPARE(codeOf(typed), std::string("in_use"));
        QCOMPARE(messageOf(typed),
                 std::string("Another session holds this server's console: this program shows its screen and types "
                             "nothing into it"));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(!turn.ok() && turn.error().code == "in_use");
        // (No turn was handed out, and none is taken: the first is the first "the console is yours".)
        QVERIFY(!wall.screen->keyboard({0, {0x04}}, 0).ok());
        QVERIFY(wall.console.hid.empty() && wall.console.badHid.isEmpty());
        QCOMPARE(wall.console.heardOf(ami::kind::hid), 0);
        QVERIFY(wall.screen->look().ok()); // (and the screen is shown all the while)
    }

    void keysWaitForTheKeyboardOfAFirstSessionToBeAttached()
    {
        // The web's (seen on a real board): a letter typed the moment a first console session was the wall's never
        // arrived, the virtual keyboard not being attached to the server yet. They wait.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.keysSettleMs = 1200;
        Wall wall(limits);
        wall.console.firstSession = true;
        wall.console.hidSettlesMs = 1000;
        const Outcome typed = anOutcome();
        wall.screen->press(u"KeyA"_s, {}, into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 10000);
        QCOMPARE(codeOf(typed), std::string("done"));
        QTRY_COMPARE(wall.console.hid.size() + static_cast<std::size_t>(wall.console.hidLost), std::size_t{2});
        QCOMPARE(wall.console.hidLost, 0);
        QCOMPARE(keysOf(wall.console), (std::vector<Keys>{{0, {0x04}}, letGo}));
    }

    void keysWaitSoLongAndNoLonger()
    {
        // (The desktop's own.) For a console that is open and this program's, and for a first session's keyboard to
        // be attached: within the time keys wait, and then they are told why not.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.masterWaitMs = 60000;
        limits.inputWaitMs = 500;
        Wall held(limits);
        held.console.holderReturning = true;
        QElapsedTimer waited;
        waited.start();
        const auto notYet = turnOf(*held.screen);
        QVERIFY(!notYet.ok());
        QCOMPARE(codeIn(notYet), std::string("no_stream"));
        QCOMPARE(notYet.error().message, std::string("Waiting for the server's console"));
        QVERIFY2(waited.elapsed() >= 450 && waited.elapsed() < 5000, QByteArray::number(waited.elapsed()).constData());
        limits.keysSettleMs = 60000;
        Wall first(limits);
        first.console.firstSession = true;
        const auto unattached = turnOf(*first.screen);
        QVERIFY(!unattached.ok());
        QCOMPARE(codeIn(unattached), std::string("no_stream"));
        QCOMPARE(unattached.error().message, std::string("Waiting for the server's keyboard and mouse to be attached"));
        QVERIFY(first.screen->holdsTheConsole());
        QCOMPARE(first.console.heardOf(ami::kind::hid), 0);
    }

    void aTypingStoppedWithAKeyDownLetsItGo()
    {
        // The web's (found by a review there): stopped between a key's press and its letting go, the key stayed
        // down on the server, and a key held down repeats. It is let go of at once.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.holdMs = 3000;
        Wall wall(limits);
        const Outcome typed = anOutcome();
        wall.screen->press(u"ControlLeft,KeyC"_s, {}, into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(!keysOf(wall.console).empty(), 10000);
        QCOMPARE(keysOf(wall.console).front(), (Keys{0x01, {0x06}}));
        wall.screen->stopTyping();
        QCOMPARE(codeOf(typed), std::string("stopped"));
        QTRY_VERIFY_WITH_TIMEOUT(lastLetsGo(wall.console), 1000); // (well before its hold was over)
        QCOMPARE(keysOf(wall.console).size(), std::size_t{2});
    }

    void aServerTakenOffTheWallInTheMiddleOfATypingHasItsKeyLetGoOf_data()
    {
        QTest::addColumn<bool>("thrownAway");
        QTest::newRow("closed") << false;
        QTest::newRow("thrown away") << true;
    }

    void aServerTakenOffTheWallInTheMiddleOfATypingHasItsKeyLetGoOf()
    {
        // The web's (found by its second review): the screen closed while a typing held a key, the console said
        // goodbye with the key still down. It is let go of before the goodbye.
        QFETCH(bool, thrownAway);
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.holdMs = 6000; // (the typing's own letting go must not come first, on a slow machine either)
        Wall wall(limits);
        const Outcome typed = anOutcome();
        wall.screen->press(u"ControlLeft,AltLeft,Delete"_s, {}, into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(!keysOf(wall.console).empty(), 10000);
        QCOMPARE(keysOf(wall.console).front(), (Keys{0x05, {0x4C}}));
        // (The desktop's own: a button held by a live line meanwhile, let go of where it was.)
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        QVERIFY(wall.screen->mouse(0x02, 500, 600, 0, turn.value()).ok());
        QTRY_COMPARE(wall.console.mice().size(), std::size_t{1});
        if (thrownAway) {
            wall.screen.reset();
            QVERIFY(!typed->has_value()); // (whoever asked threw it away)
        } else {
            wall.screen->close();
            QCOMPARE(codeOf(typed), std::string("dropped"));
        }
        QTRY_VERIFY(!wall.console.heard.empty() && wall.console.heard.back().kind == ami::kind::stop);
        QVERIFY(lastLetsGo(wall.console));
        const AmiConsoleByHand::Hid button = wall.console.mice().back();
        QVERIFY(button.bits == 0 && button.x == 500 && button.y == 600);
        // The letting go came before the goodbye: keys, then the button.
        const auto& heard = wall.console.heard;
        QCOMPARE(heard[heard.size() - 2].kind, ami::kind::hid);
        QCOMPARE(heard[heard.size() - 3].kind, ami::kind::hid);
        QVERIFY(!wall.console.hid.back().keyboard);
    }

    void whatIsDownIsLetGoOfBeforeTheConsoleIsGiven_data()
    {
        QTest::addColumn<bool>("asked");
        QTest::newRow("somebody at its own page asks for it") << true;
        QTest::newRow("it is switched to another session") << false;
    }

    void whatIsDownIsLetGoOfBeforeTheConsoleIsGiven()
    {
        // The web's (its fourth review): a person let in was given the console with a key of the wall's down. What
        // this program has down, keys and buttons, is let go of first; after that nothing of its goes there, and the
        // turn is over ("in_use").
        QFETCH(bool, asked);
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        QVERIFY(wall.screen->keyboard({0x01, {0x04}}, turn.value()).ok());
        QVERIFY(wall.screen->mouse(0x01, 1000, 2000, 0, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{2});
        if (asked) {
            wall.console.asks.emplace_back(ami::kind::sharing, somebody);
            QTRY_VERIFY(!wall.console.answers.empty());
        } else {
            wall.console.says.push_back(ami::message(ami::kind::sharing, ami::sharing::becameMaster));
        }
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{4});
        const auto& hid = wall.console.hid;
        QVERIFY(hid[2].keyboard && hid[2].bits == 0 && hid[2].keys.empty());
        QVERIFY(!hid[3].keyboard && hid[3].bits == 0 && hid[3].x == 1000 && hid[3].y == 2000);
        if (asked) {
            // ...before the answer that lets them in.
            std::size_t reports = 0;
            for (const auto& one : wall.console.heard) {
                if (one.kind == ami::kind::sharing) {
                    break;
                }
                reports += one.kind == ami::kind::hid ? 1 : 0;
            }
            QCOMPARE(reports, std::size_t{4});
        }
        QVERIFY(!wall.screen->holdsTheConsole());
        const auto refused = wall.screen->keyboard({0, {0x05}}, turn.value());
        QVERIFY(!refused.ok());
        QCOMPARE(codeIn(refused), std::string("in_use"));
        QCOMPARE(refused.error().message,
                 std::string("Somebody at the server's own console has its keyboard and mouse now"));
        QVERIFY(!wall.screen->mouse(0, 0, 0, 0, turn.value()).ok());
        const auto again = turnOf(*wall.screen);
        QVERIFY(!again.ok());
        QCOMPARE(again.error().message, refused.error().message);
        QTest::qWait(300);
        QCOMPARE(wall.console.hid.size(), std::size_t{4});
        QVERIFY(wall.screen->look().ok()); // (and the screen is looked at on)
    }

    void aTurnThatEndedIsNotTheNext()
    {
        // (The desktop's own, as the web's eras.) The console comes back: a new turn, from a keyboard with nothing
        // down, and whoever held the keyboard in the one before takes it again.
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        const auto first = turnOf(*wall.screen);
        QVERIFY(first.ok());
        wall.console.says.push_back(ami::message(ami::kind::sharing, ami::sharing::becameMaster));
        QTRY_VERIFY(!wall.screen->holdsTheConsole());
        wall.console.says.push_back(ami::message(ami::kind::nextHolder, ami::sharing::becameMaster));
        QTRY_VERIFY(wall.screen->holdsTheConsole());
        const auto second = turnOf(*wall.screen);
        QVERIFY(second.ok());
        QVERIFY(second.value() != first.value());
        const auto old = wall.screen->keyboard({0, {0x04}}, first.value());
        QVERIFY(!old.ok());
        QCOMPARE(old.error().message, std::string("The server's console changed hands: take its keyboard again"));
        QVERIFY(wall.screen->keyboard({0, {0x04}}, second.value()).ok());
        QVERIFY(wall.screen->keyboard({}, second.value()).ok());
        QTRY_COMPARE(keysOf(wall.console), (std::vector<Keys>{{0, {0x04}}, letGo}));
    }

    void aKeyIntoAConsoleThatHasClosedIsNotSaidToBeSent()
    {
        // The web's (found by a review there): a key sent over a connection that had closed was taken for delivered.
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        wall.console.hangsUp = true;
        wall.console.says.push_back(ami::message(ami::kind::stillThere));
        QTRY_VERIFY(!wall.screen->holdsTheConsole());
        const auto refused = wall.screen->keyboard({0, {0x04}}, turn.value());
        QVERIFY(!refused.ok());
        QCOMPARE(codeIn(refused), std::string("dropped"));
        QCOMPARE(refused.error().message, std::string("The server's console was closed"));
    }

    void aKeyboardReportHoldsSixKeysAtTheMostEachOnce()
    {
        // (Part 4, the second reading.) As a keyboard makes them: more than six keys, or one twice, is no report.
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        const auto seven = wall.screen->keyboard({0, {0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A}}, turn.value());
        QVERIFY(!seven.ok());
        QCOMPARE(codeIn(seven), std::string("invalid"));
        const auto twice = wall.screen->keyboard({0, {0x04, 0x05, 0x04}}, turn.value());
        QVERIFY(!twice.ok());
        QCOMPARE(codeIn(twice), std::string("invalid"));
        QVERIFY(wall.screen->keyboard({0, {0x04, 0x05, 0x06, 0x07, 0x08, 0x09}}, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        QTest::qWait(100);
        QCOMPARE(wall.console.hid.size(), std::size_t{1});
        QVERIFY(wall.console.badHid.isEmpty());
    }

    void aStillThatStopsTheTypingIsLeftAlone_data()
    {
        QTest::addColumn<bool>("first");
        QTest::newRow("before its first key") << true;
        QTest::newRow("asked again on the way") << false;
    }

    void aStillThatStopsTheTypingIsLeftAlone()
    {
        // (Part 4, the second reading.) Whoever is asked whether they may still have it typed stops every typing
        // from inside the asking, which takes the typing, and what it asks with, away: the asking that runs is a
        // copy of the screen's own, not what was let go of under it (before, it ran on in freed memory: ASAN saw
        // it). Each copy of what it asks with knows whether it was let go of (ItsOwn), and the one that runs looks,
        // after stopping every typing, through what it kept on its stack alone: told here, not a crash.
        struct ItsOwn {
            std::shared_ptr<bool> gone = std::make_shared<bool>(false);
            ItsOwn() = default;
            ItsOwn(const ItsOwn& /*other*/) : gone(std::make_shared<bool>(false)) {}
            ItsOwn(ItsOwn&& other) noexcept : gone(std::exchange(other.gone, nullptr)) {} // (it, moved)
            ItsOwn& operator=(const ItsOwn&) = delete;
            ItsOwn& operator=(ItsOwn&&) = delete;
            ~ItsOwn()
            {
                if (gone != nullptr) {
                    *gone = true;
                }
            }
        };
        bool goneWhileItRan = false; // (before the wall: it outlives every asking)
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.askAgainEvery = 2;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        QFETCH(bool, first);
        auto asked = std::make_shared<int>(0);
        const Outcome typed = anOutcome();
        ServerScreen* const screen = wall.screen.get();
        wall.screen->type(
            u"abcdef"_s,
            [screen, asked, first, said = &goneWhileItRan, own = ItsOwn{}] {
                const bool stop = first || ++*asked > 1;
                // (Kept here: nothing of the asking itself is looked at after the typings are stopped.)
                const std::shared_ptr<bool> mine = own.gone;
                bool* const told = said;
                if (stop) {
                    screen->stopTyping();
                    *told = *told || *mine;
                }
                return true;
            },
            into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 10000);
        QCOMPARE(codeOf(typed), std::string("stopped"));
        QVERIFY2(!goneWhileItRan, "What the typing asks with was let go of while it ran");
        QTest::qWait(200);
        QVERIFY(!wall.console.keyboards().empty() || first);
        QVERIFY(lastLetsGo(wall.console) || first);
    }

    void aTurnIsOneScreensOnlyAndKeysWaitForTheKeyboard()
    {
        // (The reading of part 4, M1.) Turns are numbered for the whole program: one screen's turn is no turn of
        // another's. And a key does not go before a first session's keyboard is attached, whatever turn it is said in.
        {
            Wall one;
            Wall other;
            QVERIFY(one.shownWithin(5000));
            QVERIFY(other.shownWithin(5000));
            const auto ones = turnOf(*one.screen);
            const auto others = turnOf(*other.screen);
            QVERIFY(ones.ok() && others.ok());
            QVERIFY(ones.value() != others.value());
            QVERIFY(!other.screen->keyboard({0, {0x04}}, ones.value()).ok());
            QVERIFY(!one.screen->keyboard({0, {0x04}}, others.value()).ok());
            QVERIFY(other.console.hid.empty() && one.console.hid.empty());
        }
        // A turn that nobody was handed: the next one after the session ends, in a first session that follows. (The
        // two walls above are gone: no other screen numbers a turn meanwhile.)
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.retryMs = {100, 100, 100, 100};
        limits.keysSettleMs = 2000;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto first = turnOf(*wall.screen);
        QVERIFY(first.ok());
        wall.console.hangsUp = true;
        wall.console.says.push_back(ami::message(ami::kind::stillThere));
        QTRY_VERIFY(!wall.screen->holdsTheConsole());
        wall.console.hangsUp = false;
        wall.console.firstSession = true;
        wall.console.hidSettlesMs = 2000;
        QTRY_VERIFY_WITH_TIMEOUT((wall.lookOnce(), wall.screen->holdsTheConsole()), 10000);
        const auto early = wall.screen->keyboard({0, {0x04}}, first.value() + 1);
        QVERIFY(!early.ok());
        QCOMPARE(early.error().message, std::string("Waiting for the server's keyboard and mouse to be attached"));
        const auto settled = turnOf(*wall.screen);
        QVERIFY(settled.ok());
        QCOMPARE(settled.value(), first.value() + 1);
        QVERIFY(wall.screen->keyboard({0, {0x04}}, settled.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        QCOMPARE(wall.console.hidLost, 0);
    }

    void aButtonHeldAsTheMouseBecomesRelativeIsLetGoOf()
    {
        // (The reading of part 4, M2.) The console says the server's mouse is a relative one now, while a button is
        // held: it is let go of at once, as a relative report (the web's way where it has no place for it); after
        // that the pointer goes nowhere, and keys still go.
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        QVERIFY(wall.screen->mouse(0x01, 500, 600, 0, turn.value()).ok());
        QTRY_COMPARE(wall.console.mice().size(), std::size_t{1});
        wall.console.says.push_back(ami::message(ami::kind::mouseMode, 0, ami::Bytes{ami::mouse::relative}));
        QTRY_COMPARE(wall.console.mice().size(), std::size_t{2});
        const AmiConsoleByHand::Hid letGoOf = wall.console.mice().back();
        QVERIFY(letGoOf.relative && letGoOf.bits == 0 && letGoOf.x == 0 && letGoOf.y == 0);
        QVERIFY(wall.console.badHid.isEmpty());
        QCOMPARE(codeIn(wall.screen->mouse(0, 500, 600, 0, turn.value())), std::string("no_input"));
        QCOMPARE(codeIn(wall.screen->mouse(0x01, 500, 600, 0, turn.value())), std::string("no_input"));
        QVERIFY(wall.screen->keyboard({0, {0x04}}, turn.value()).ok());
        QTRY_COMPARE(wall.console.keyboards().size(), std::size_t{1});
    }

    void aStoppedTypingKeepsNoConsole()
    {
        // (The reading of part 4, L1.) A typing stopped while it waited for the keyboard: its asking goes with it, and
        // nothing holds the console for it (before: up to the time keys wait, looked at for nobody).
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.idleMs = 300;
        limits.masterWaitMs = 60000;
        limits.inputWaitMs = 20000;
        limits.goodbyeWithinMs = 500;
        Wall wall(limits);
        wall.console.holderReturning = true;
        const Outcome typed = anOutcome();
        wall.screen->press(u"KeyA"_s, {}, into(typed));
        QTRY_COMPARE_WITH_TIMEOUT(wall.console.watching, 1, 10000);
        wall.screen->stopTyping();
        QCOMPARE(codeOf(typed), std::string("stopped"));
        QTRY_COMPARE_WITH_TIMEOUT(wall.console.watching, 0, 5000);
    }

    void aLongTypingKeepsTheConsole()
    {
        // The web's: a long text takes a while, all of it this program pressing keys; the console is not given back
        // meanwhile for nobody looking at the screen.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.idleMs = 400;
        limits.keyPauseMs = 15;
        Wall wall(limits);
        const Outcome typed = anOutcome();
        wall.screen->type(QString(40, u'a'), {}, into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 15000);
        QCOMPARE(codeOf(typed), std::string("done"));
        QCOMPARE(wall.console.sessions, 1);
        QTRY_COMPARE(keysOf(wall.console).size(), std::size_t{80});
    }

    void whoeverWaitsForKeysIsToldWhenTheScreenCloses()
    {
        // (The desktop's own.) Closed, the screen answers whoever waits for its keyboard, and whoever waits for a
        // typing, from inside the closing; thrown away, nobody (whoever asked threw it away).
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.masterWaitMs = 60000;
        Wall wall(limits);
        wall.console.holderReturning = true;
        auto got = std::make_shared<std::optional<qkvm::base::Result<ServerScreen::Turn>>>();
        wall.screen->askForInput([got](qkvm::base::Result<ServerScreen::Turn> turn) { *got = std::move(turn); });
        const Outcome typed = anOutcome();
        wall.screen->press(u"KeyA"_s, {}, into(typed));
        QVERIFY(wall.shownWithin(5000));
        QVERIFY(!got->has_value() && !typed->has_value());
        wall.screen->close();
        QVERIFY(got->has_value());
        const qkvm::base::Result<ServerScreen::Turn> answered = got->value_or(qkvm::base::Error{"none", "none"});
        QVERIFY(!answered.ok());
        QCOMPARE(codeIn(answered), std::string("dropped"));
        QCOMPARE(codeOf(typed), std::string("dropped"));
        Wall thrown(limits);
        thrown.console.holderReturning = true;
        auto never = std::make_shared<std::optional<qkvm::base::Result<ServerScreen::Turn>>>();
        thrown.screen->askForInput([never](qkvm::base::Result<ServerScreen::Turn> turn) { *never = std::move(turn); });
        QVERIFY(thrown.shownWithin(5000));
        thrown.screen.reset();
        QTest::qWait(100);
        QVERIFY(!never->has_value());
    }

    void aTypingAsksWhetherItMayStillBeDone()
    {
        // The web's: before its first key, and every so many keys of a long text, whoever asked is asked whether
        // they may still have it done.
        Wall refused;
        const Outcome never = anOutcome();
        refused.screen->press(u"KeyA"_s, [] { return false; }, into(never));
        QTRY_VERIFY_WITH_TIMEOUT(never->has_value(), 10000);
        QCOMPARE(codeOf(never), std::string("stopped"));
        QCOMPARE(messageOf(never), std::string("It was not done: whoever asked for it may no longer do that"));
        QCOMPARE(refused.console.heardOf(ami::kind::hid), 0);
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.keyPauseMs = 1;
        Wall wall(limits);
        auto asked = std::make_shared<int>(0);
        const Outcome typed = anOutcome();
        wall.screen->type(QString(60, u'a'), [asked] { return ++*asked < 2; }, into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 15000);
        QCOMPARE(codeOf(typed), std::string("stopped"));
        QCOMPARE(messageOf(typed),
                 std::string("Typing was stopped after 25 of 60 keys: whoever asked for it may no longer do that"));
        QCOMPARE(*asked, 2);
        QTRY_COMPARE(keysOf(wall.console).size(), std::size_t{50});
        QTest::qWait(200);
        QCOMPARE(keysOf(wall.console).size(), std::size_t{50});
        QVERIFY(lastLetsGo(wall.console));
    }

    void whatCannotBeTypedIsRefusedBeforeAnythingIsAsked()
    {
        // The web's: an invalid key combination, in the console's words; and a text that cannot be typed, by where it
        // went wrong and not by what is there (it may be a saved secret). Neither asks the controller for anything.
        Wall wall;
        const Outcome keys = anOutcome();
        const Outcome text = anOutcome();
        const QChar odd(0xE9);
        wall.screen->press(u"NoSuchKey"_s, {}, into(keys));
        wall.screen->type(u"secret"_s + odd + u"x"_s, {}, into(text));
        QVERIFY(!keys->has_value()); // (answered later, never from inside the asking)
        QTRY_VERIFY(keys->has_value() && text->has_value());
        QCOMPARE(std::pair(codeOf(keys), messageOf(keys)),
                 (std::pair{std::string("invalid"), std::string("This server's console has no NoSuchKey key")}));
        QCOMPARE(codeOf(text), std::string("invalid"));
        QVERIFY(QString::fromStdString(messageOf(text)).contains(u"Character 7"_s));
        QVERIFY(!QString::fromStdString(messageOf(text)).contains(odd));
        QCOMPARE(wall.wayIns, 0);
        QCOMPARE(wall.console.askings, 0);
    }

    void aStoppedTypingsAskingDrivesNothing()
    {
        // (The desktop's own.) A typing stopped while it waited for the keyboard, and another asked for after it: what
        // the first one's asking comes to (here, its time is up first) is nothing to the second, and when the keyboard
        // comes, the second is typed, once and as it should be, and the first not at all.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.masterWaitMs = 60000;
        limits.inputWaitMs = 3000;
        limits.holdMs = 400;
        Wall wall(limits);
        wall.console.holderReturning = true;
        QVERIFY(wall.shownWithin(5000));
        const Outcome stopped = anOutcome();
        wall.screen->press(u"KeyA"_s, {}, into(stopped));
        QTest::qWait(1500); // (its keyboard is asked for)
        wall.screen->stopTyping();
        QCOMPARE(codeOf(stopped), std::string("stopped"));
        const Outcome typed = anOutcome();
        wall.screen->press(u"KeyB"_s, {}, into(typed));
        QTest::qWait(2000); // (the first one's asking is over: its time was up)
        QVERIFY2(!typed->has_value(), codeOf(typed).c_str());
        wall.console.says.push_back(ami::message(ami::kind::nextHolder, ami::sharing::becameMaster));
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 10000);
        QCOMPARE(codeOf(typed), std::string("done"));
        QTRY_COMPARE(keysOf(wall.console), (std::vector<Keys>{{0, {0x05}}, letGo}));
        const std::vector<AmiConsoleByHand::Hid> keys = wall.console.keyboards();
        QVERIFY2(keys[1].atMs - keys[0].atMs >= 300, QByteArray::number(keys[1].atMs - keys[0].atMs).constData());
        QTest::qWait(300);
        QCOMPARE(keysOf(wall.console).size(), std::size_t{2});
    }

    void typingsGoOneAfterTheOther()
    {
        // The web's lock: two typings at once would interleave on the one keyboard.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.holdMs = 200;
        Wall wall(limits);
        const Outcome first = anOutcome();
        const Outcome second = anOutcome();
        wall.screen->press(u"ShiftLeft,KeyA"_s, {}, into(first));
        wall.screen->press(u"KeyB"_s, {}, into(second));
        QTRY_VERIFY_WITH_TIMEOUT(second->has_value(), 10000);
        QCOMPARE(codeOf(first), std::string("done"));
        QCOMPARE(codeOf(second), std::string("done"));
        QTRY_COMPARE(keysOf(wall.console), (std::vector<Keys>{{0x02, {0x04}}, letGo, {0, {0x05}}, letGo}));
    }

    void thePointerGoesAsAnAbsolutePlace()
    {
        // The web's mouse reports, through the console: the buttons held, the place over the screen, the wheel; a
        // place beyond the screen is at its edge. And the keys a live line says one by one, as the whole keyboard.
        Wall wall;
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        QVERIFY(wall.screen->mouse(qkvm::core::typed::buttonBit(1), 16384, 32767, 0, turn.value()).ok());
        QVERIFY(wall.screen->mouse(0, 99999, -5, -1, turn.value()).ok());
        qkvm::core::typed::Keyboard keyboard;
        keyboard.take(0xE1, true);
        keyboard.take(0x04, true);
        QVERIFY(wall.screen->keyboard(keyboard.now(), turn.value()).ok());
        keyboard.clear();
        QVERIFY(wall.screen->keyboard(keyboard.now(), turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{4});
        const std::vector<AmiConsoleByHand::Hid> mice = wall.console.mice();
        QVERIFY(mice[0].bits == 1 && mice[0].x == 16384 && mice[0].y == 32767 && mice[0].wheel == 0);
        QVERIFY(mice[1].bits == 0 && mice[1].x == 0x7FFF && mice[1].y == 0 && mice[1].wheel == -1);
        QCOMPARE(keysOf(wall.console), (std::vector<Keys>{{0x02, {0x04}}, letGo}));
        // A server whose mouse is a relative one is not moved by an absolute place; its keyboard still takes keys.
        Wall relative;
        relative.console.mouseMode = ami::mouse::relative;
        QVERIFY(relative.shownWithin(5000));
        const auto its = turnOf(*relative.screen);
        QVERIFY(its.ok());
        const auto moved = relative.screen->mouse(1, 100, 100, 0, its.value());
        QVERIFY(!moved.ok());
        QCOMPARE(codeIn(moved), std::string("no_input"));
        QVERIFY(relative.screen->keyboard({0, {0x04}}, its.value()).ok());
        QTRY_COMPARE(relative.console.hid.size(), std::size_t{1});
        QVERIFY(relative.console.hid[0].keyboard);
    }
};

QTEST_GUILESS_MAIN(TestServerScreenInput)
#include "test_server_screen_input.moc"
