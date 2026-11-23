#include <QTest>

#include "server_screen_wall.h"

// A server's own screen from its controller's console: keys and the pointer behind what waits to leave, for a
// console that takes nothing, or takes it slowly (part 4 and its readings), against a console by hand on this PC's
// own address. Keys, the pointer and typings where nothing waits are test_server_screen_input.cpp's.
using namespace qkvm::core::testing::screens;

class TestServerScreenBacklog : public QObject {
    Q_OBJECT
private slots:
    void keysAreNotSaidBehindWhatHasWaitedTooLongAndWhatIsDownIsLetGoOfAfter_data()
    {
        QTest::addColumn<int>("letGoWithinMs");
        QTest::newRow("in time") << 30000;
        QTest::newRow("too late") << 600;
    }

    void keysAreNotSaidBehindWhatHasWaitedTooLongAndWhatIsDownIsLetGoOfAfter()
    {
        // (The desktop's own, for the web's INPUT_SAY and LET_GO_WITHIN; and the reading of part 4, H1.) A console
        // that takes nothing off its line: what is said to it waits, and a key is not said behind what has waited too
        // long (it would be pressed long after it was). The console goes on. A letting go presses nothing, and is
        // said behind it; where it has not left in time while the line goes on, the session is ended, which lets go
        // of it on the server (before: it was dropped without a word, and the key stayed down).
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 300;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.holdMs = 4000;
        limits.goodbyeWithinMs = 500;
        QFETCH(int, letGoWithinMs);
        limits.letGoWithinMs = letGoWithinMs;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const Outcome typed = anOutcome();
        wall.screen->press(u"KeyA"_s, {}, into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(!keysOf(wall.console).empty(), 10000);
        wall.console.stall();
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        qkvm::base::Result<void> said;
        int keys = 0;
        while (said.ok() && keys < 400'000) {
            said = wall.screen->keyboard({0, {0x04}}, turn.value());
            if (++keys % 200 == 0) {
                QCoreApplication::processEvents();
            }
        }
        QVERIFY2(!said.ok(), QByteArray::number(keys).constData());
        QCOMPARE(codeIn(said), std::string("busy")); // (the turn stands: not "dropped", as a turn that went is)
        QCOMPARE(said.error().message, std::string("The server's console stopped taking keys and the mouse"));
        QVERIFY(wall.screen->holdsTheConsole());
        // Nor anything that presses, behind what waits now: a key, or a modifier.
        QVERIFY(!wall.screen->keyboard({0, {0x04, 0x05}}, turn.value()).ok());
        QVERIFY(!wall.screen->keyboard({0x02, {0x04}}, turn.value()).ok());
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        // (What is said, in its order: that something may have stayed down, and the session's end.)
        const auto told = std::make_shared<std::vector<std::string>>();
        QObject::connect(wall.screen.get(), &ServerScreen::leftDown, wall.screen.get(),
                         [told] { told->emplace_back("left_down"); });
        QObject::connect(
            wall.screen.get(), &ServerScreen::ended, wall.screen.get(),
            [told](const QString& code, const QString&) { told->push_back("ended:" + code.toStdString()); });
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 10000);
        QCOMPARE(codeOf(typed), std::string("done")); // (its letting go was said, behind what waits)
        QVERIFY(!lastLetsGo(wall.console));
        if (letGoWithinMs < 1000) {
            // Not taken in time, while the line goes on: the session is ended, which lets go of what it had down, and
            // that is said, just after the word that something may have stayed down.
            QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
            QCOMPARE(*told, (std::vector<std::string>{"left_down", "ended:dropped"}));
            QCOMPARE(ended.at(0).at(0).toString(), u"dropped"_s);
            QVERIFY(ended.at(0).at(1).toString().contains(u"may have stayed down"_s));
            QVERIFY(!wall.screen->holdsTheConsole());
            QTest::qWait(1500); // (its goodbye's time: then its line is reset)
            wall.console.readAgain();
            QTRY_VERIFY_WITH_TIMEOUT(lastLetsGo(wall.console) || wall.console.endedWithSomethingDown > 0, 30000);
            return;
        }
        wall.console.readAgain();
        QTRY_VERIFY_WITH_TIMEOUT(lastLetsGo(wall.console), 30000);
        QVERIFY(wall.console.badHid.isEmpty());
        QVERIFY(wall.screen->holdsTheConsole());
        QCOMPARE(ended.size(), 0);
        QVERIFY(told->empty());
    }

    void aLettingGoGoesBehindWhatWaits_data()
    {
        QTest::addColumn<bool>("button");
        QTest::addColumn<int>("letGoWithinMs");
        QTest::newRow("a key let go of, in time") << false << 5000;
        QTest::newRow("a key let go of, too late") << false << 600;
        QTest::newRow("a button let go of, in time") << true << 5000;
        QTest::newRow("a button let go of, too late") << true << 600;
    }

    void aLettingGoGoesBehindWhatWaits()
    {
        // (The reading of part 4, H1.) A live line's key-up and button-up, while the console takes nothing: said behind
        // what waits (before: refused, and never said again), and the session ended where it has not left in time.
        QFETCH(bool, button);
        QFETCH(int, letGoWithinMs);
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 300;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.goodbyeWithinMs = 500;
        limits.letGoWithinMs = letGoWithinMs;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        const qkvm::core::typed::Report held{0x01, {0x04}};
        if (button) {
            QVERIFY(wall.screen->mouse(0x01, 1000, 2000, 0, turn.value()).ok());
        } else {
            QVERIFY(wall.screen->keyboard(held, turn.value()).ok());
        }
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        qkvm::base::Result<void> said;
        int reports = 0;
        while (said.ok() && reports < 400'000) {
            // (What changes nothing presses nothing new either, and lets go of nothing: it does not go behind.)
            said = button ? wall.screen->keyboard({}, turn.value()) : wall.screen->keyboard(held, turn.value());
            if (++reports % 200 == 0) {
                QCoreApplication::processEvents();
            }
        }
        QVERIFY2(!said.ok(), QByteArray::number(reports).constData());
        QCOMPARE(said.error().message, std::string("The server's console stopped taking keys and the mouse"));
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        QElapsedTimer sinceLettingGo;
        if (button) {
            QVERIFY(!wall.screen->mouse(0x03, 1000, 2000, 0, turn.value()).ok()); // (another button: it presses)
            QVERIFY(!wall.screen->mouse(0x01, 1000, 2000, 0, turn.value()).ok()); // (the same: it lets go of none)
            QVERIFY(wall.screen->mouse(0, 1000, 2000, 0, turn.value()).ok());
        } else {
            // Ctrl let go of, A still down, but B pressed: refused, and what it lets go of goes alone (Ctrl).
            const auto pressing = wall.screen->keyboard({0, {0x04, 0x05}}, turn.value());
            QVERIFY(!pressing.ok() && pressing.error().code == "busy");
            QVERIFY(!wall.screen->keyboard({0, {0x04}}, turn.value()).ok()); // (as it is now: it lets go of none)
            QVERIFY(wall.screen->keyboard({}, turn.value()).ok());
        }
        sinceLettingGo.start();
        const auto letGoOf = [&wall, button] {
            const std::vector<AmiConsoleByHand::Hid> reports = button ? wall.console.mice() : wall.console.keyboards();
            return reports.size() > 1 && reports.back().bits == 0 && reports.back().keys.empty();
        };
        if (letGoWithinMs < 1000) {
            QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
            QVERIFY(ended.at(0).at(1).toString().contains(u"may have stayed down"_s));
            QTest::qWait(1500);
            wall.console.readAgain();
            QTRY_VERIFY_WITH_TIMEOUT(letGoOf() || wall.console.endedWithSomethingDown > 0, 30000);
            return;
        }
        wall.console.readAgain();
        QTRY_VERIFY_WITH_TIMEOUT(letGoOf(), 30000);
        QVERIFY(wall.console.badHid.isEmpty());
        // Left, and so not watched any more: the session goes on beyond the time the letting go had.
        QVERIFY2(sinceLettingGo.elapsed() < letGoWithinMs, QByteArray::number(sinceLettingGo.elapsed()).constData());
        QTest::qWait(static_cast<int>(letGoWithinMs - sinceLettingGo.elapsed()) + 500);
        QVERIFY(wall.screen->holdsTheConsole());
        QCOMPARE(ended.size(), 0);
    }

    void aConsoleThatIsSlowAndWellIsNotEnded()
    {
        // (Part 4, the second reading, M-a.) A board that is slow and well: it takes the button's letting go, said
        // behind what waits, and what came before it, and is slow to take the moves that came after it, so that
        // something waits at every look for longer than letGoWithinMs. The session goes on (before: judged by whether
        // anything at all waited, it was ended, the button "may have stayed down", and the screen gone).
        //
        // Made so, step by step, by what waits and what the console takes, and not by how fast moves are said or the
        // console reads: a release build said so many moves before the first was refused that the console, reading
        // 2 KiB every 20 ms, took more than letGoWithinMs to come to the letting go behind them (gate 48), and where
        // the console read on, what waited came to nothing between two looks now and then.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 2000;
        limits.letGoWithinMs = 4000;
        limits.sayMs = 600000; // (and the line is not ended for what waits: only the watch could end the session)
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.goodbyeWithinMs = 500;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        QVERIFY(wall.screen->mouse(0x01, 1000, 1000, 0, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        int place = 0;
        const auto moves = [&wall, &turn, &place](int many) {
            int taken = 0;
            for (int one = 0; one < many; ++one) {
                place = (place + 7) % 30000;
                taken += wall.screen->mouse(0x01, place, 1000, 0, turn.value()).ok() ? 1 : 0;
            }
            return taken;
        };
        // Moves until something waits to leave this program (what Windows holds for the console is full: it takes
        // nothing now), and two hundred more; the letting go behind them; and five thousand moves behind it, said
        // well within inputSayMs and taken.
        QElapsedTimer filling;
        filling.start();
        while (wall.screen->inputWaitedMs() == 0 && filling.elapsed() < 30000) {
            static_cast<void>(moves(50));
            QCoreApplication::processEvents();
        }
        QVERIFY2(filling.elapsed() < 30000, "nothing came to wait");
        QCOMPARE(moves(200), 200);
        QVERIFY(wall.screen->mouse(0, place, 1000, 0, turn.value()).ok()); // (let go of, behind what waits)
        QCOMPARE(moves(5000), 5000);
        // The console takes 100 KiB more, the letting go and what came before it (some 250 moves of 100 bytes or so,
        // and what Windows held), and then nothing: what came after waits at every look.
        wall.console.readSome(100 * 1024);
        QTest::qWait(limits.letGoWithinMs + 1500);
        QVERIFY(wall.screen->inputWaitedMs() > limits.letGoWithinMs);
        QCOMPARE(ended.size(), 0);
        QVERIFY(wall.screen->holdsTheConsole());
        wall.console.readAgain();
        QTRY_VERIFY_WITH_TIMEOUT(
            std::ranges::any_of(wall.console.mice(), [](const auto& one) { return one.bits == 0; }), 30000);
        QVERIFY(wall.console.badHid.isEmpty());
    }

    void lettingsGoThatKeepComingDoNotPutOffTheJudging_data()
    {
        QTest::addColumn<bool>("askings");
        QTest::newRow("lettings go") << false;
        QTest::newRow("askings for the keyboard") << true;
    }

    void lettingsGoThatKeepComingDoNotPutOffTheJudging()
    {
        // (Part 4, the second reading, M-a.) A console that takes nothing, while one letting go comes after another,
        // or one asking for the keyboard, more often than the ticks that judge them: the first letting go that has not
        // left in time ends the session in its time (before: each restarted the ticks, and none came while they kept
        // coming).
        QFETCH(bool, askings);
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.tickMs = 250;
        limits.inputSayMs = 300;
        limits.letGoWithinMs = 400;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.goodbyeWithinMs = 500;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        const qkvm::core::typed::Report held{0xFF, {0x04, 0x05, 0x06, 0x07, 0x08, 0x09}};
        QVERIFY(wall.screen->keyboard(held, turn.value()).ok());
        QVERIFY(wall.screen->mouse(0x07, 1000, 1000, 0, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{2});
        wall.console.stall();
        qkvm::base::Result<void> said;
        for (int report = 0; said.ok() && report < 400'000; ++report) {
            said = wall.screen->keyboard(held, turn.value());
            if (report % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        QCOMPARE(codeIn(said), std::string("busy"));
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        // Seventeen lettings go, a tenth of a second apart: the modifiers one by one, the keys, the buttons.
        qkvm::core::typed::Report now = held;
        std::uint8_t buttons = 0x07;
        QElapsedTimer sinceFirst;
        sinceFirst.start();
        for (int step = 0; step < 17 && ended.isEmpty(); ++step) {
            if (askings && step > 0) {
                wall.screen->askForInput([](const qkvm::base::Result<ServerScreen::Turn>&) {});
            } else if (step < 8) {
                now.modifiers = static_cast<std::uint8_t>(now.modifiers & ~(1U << step));
                QVERIFY(wall.screen->keyboard(now, turn.value()).ok());
            } else if (step < 14) {
                now.keys.pop_back();
                QVERIFY(wall.screen->keyboard(now, turn.value()).ok());
            } else {
                buttons = static_cast<std::uint8_t>(buttons >> 1);
                QVERIFY(wall.screen->mouse(buttons, 1000, 1000, 0, turn.value()).ok());
            }
            QTest::qWait(100);
        }
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
        QVERIFY2(sinceFirst.elapsed() < 1300, QByteArray::number(sinceFirst.elapsed()).constData());
        QCOMPARE(ended.at(0).at(0).toString(), u"dropped"_s);
        QTest::qWait(1000);
        wall.console.readAgain();
    }

    void aButtonLetGoOfBehindWhatWaitsGoesWhereThePointerStands()
    {
        // (Part 4, the second reading, M-b.) Held at one place, moved while the console still took moves, and let go
        // of far away with a turn of the wheel, behind what has waited too long: it is let go of where the pointer
        // stood, and the wheel does not turn (before: a drag to where it was let go of, and a turn of the wheel,
        // seconds late).
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 300;
        limits.letGoWithinMs = 30000;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        QVERIFY(wall.screen->mouse(0x01, 1000, 2000, 0, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        // (The moves stay between 1001 and 6000 across: where it is let go of, 30000, is a place none of them goes. A
        // release build said sixty thousand moves, one place further each, before the first was refused, and one of
        // them was at 30000, gate 48.)
        int x = 1000;
        qkvm::base::Result<void> said;
        for (int report = 0; said.ok() && report < 400'000; ++report) {
            const int next = 1001 + (report % 5000);
            said = wall.screen->mouse(0x01, next, 2000, 0, turn.value());
            x = said.ok() ? next : x;
            if (report % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        QCOMPARE(codeIn(said), std::string("busy"));
        QVERIFY(wall.screen->mouse(0, 30000, 30000, 3, turn.value()).ok());
        wall.console.readAgain();
        QTRY_VERIFY_WITH_TIMEOUT(!wall.console.mice().empty() && wall.console.mice().back().bits == 0, 30000);
        const AmiConsoleByHand::Hid last = wall.console.mice().back();
        QCOMPARE(last.x, x);
        QCOMPARE(last.y, 2000);
        QCOMPARE(last.wheel, 0);
        QVERIFY(std::ranges::none_of(
            wall.console.mice(), [](const auto& one) { return one.x == 30000 || one.y == 30000 || one.wheel != 0; }));
    }

    void whatAReportLetsGoOfGoesWhereWhatItPressesCannot_data()
    {
        QTest::addColumn<bool>("button");
        QTest::addColumn<int>("letGoWithinMs");
        QTest::newRow("keys") << false << 30000;
        QTest::newRow("buttons") << true << 30000;
        QTest::newRow("keys, not left in time") << false << 600;
        QTest::newRow("buttons, not left in time") << true << 600;
    }

    void whatAReportLetsGoOfGoesWhereWhatItPressesCannot()
    {
        // (Part 4, the second reading, M-c.) Behind what has waited too long, a report that lets go of something and
        // presses something else (a seventh key down lets go of the first; the left button let go of as the right one
        // is pressed): what it presses is refused ("busy", the turn stands), and what it lets go of goes alone (before:
        // refused whole, and what it let go of stayed down while the console went on).
        QFETCH(bool, button);
        QFETCH(int, letGoWithinMs);
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 300;
        limits.letGoWithinMs = letGoWithinMs;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.goodbyeWithinMs = 500;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        const qkvm::core::typed::Report held{0x02, {0x04, 0x05}};
        QVERIFY(button ? wall.screen->mouse(0x01, 1000, 2000, 0, turn.value()).ok()
                       : wall.screen->keyboard(held, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        qkvm::base::Result<void> said;
        for (int report = 0; said.ok() && report < 400'000; ++report) {
            said = button ? wall.screen->mouse(0x01, 1000, 2000, 0, turn.value())
                          : wall.screen->keyboard(held, turn.value());
            if (report % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        QCOMPARE(codeIn(said), std::string("busy"));
        const auto both = button ? wall.screen->mouse(0x02, 1000, 2000, 0, turn.value())
                                 : wall.screen->keyboard({0x02, {0x05, 0x06}}, turn.value());
        QVERIFY(!both.ok());
        QCOMPARE(codeIn(both), std::string("busy"));
        QVERIFY(wall.screen->inTurn(turn.value()).ok());
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        const QSignalSpy leftDown(wall.screen.get(), &ServerScreen::leftDown);
        if (letGoWithinMs < 1000) {
            // What it let go of is watched as every letting go is.
            QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
            QCOMPARE(leftDown.size(), 1);
            QCOMPARE(ended.at(0).at(0).toString(), u"dropped"_s);
            QTest::qWait(1000);
            wall.console.readAgain();
            return;
        }
        wall.console.readAgain();
        if (button) {
            QTRY_VERIFY_WITH_TIMEOUT(wall.console.mice().back().bits == 0, 30000);
            QVERIFY(std::ranges::none_of(wall.console.mice(), [](const auto& one) { return (one.bits & 0x02) != 0; }));
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(keysOf(wall.console).back() == (Keys{0x02, {0x05}}), 30000);
            QVERIFY(std::ranges::none_of(keysOf(wall.console), [](const Keys& one) {
                return std::ranges::find(one.second, 0x06) != one.second.end();
            }));
        }
        // (Let go of in this session, by this program: not by a session's end.)
        QCOMPARE(ended.size(), 0);
        QCOMPARE(wall.console.sessions, 1);
        QVERIFY(wall.screen->holdsTheConsole());
        QVERIFY(wall.console.badHid.isEmpty());
    }

    void aButtonLetGoOfAsTheMouseBecomesRelativeIsWatched()
    {
        // (Part 4, the second reading.) The button let go of as the server's mouse becomes a relative one is watched
        // as every letting go is: where it does not leave in time, the session is ended, and that is said.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 300;
        limits.letGoWithinMs = 600;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.goodbyeWithinMs = 500;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        QVERIFY(wall.screen->mouse(0x01, 1000, 2000, 0, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        qkvm::base::Result<void> said;
        for (int report = 0; said.ok() && report < 400'000; ++report) {
            said = wall.screen->keyboard({0, {0x04}}, turn.value());
            if (report % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        QCOMPARE(codeIn(said), std::string("busy"));
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        const QSignalSpy leftDown(wall.screen.get(), &ServerScreen::leftDown);
        wall.console.says.push_back(ami::message(ami::kind::mouseMode, 0, ami::Bytes{ami::mouse::relative}));
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
        QCOMPARE(leftDown.size(), 1);
        QCOMPARE(ended.at(0).at(0).toString(), u"dropped"_s);
        QTest::qWait(1000);
        wall.console.readAgain();
    }

    void aLettingGoIsWatchedThoughLittleHasWaited_data()
    {
        QTest::addColumn<bool>("button");
        QTest::newRow("a key") << false;
        QTest::newRow("a button") << true;
    }

    void aLettingGoIsWatchedThoughLittleHasWaited()
    {
        // (Part 4, the second reading, M-a.) Said where nothing has waited long enough to hold keys back (here never:
        // inputSayMs is a minute), into a console that takes nothing: watched all the same, and the session ended
        // where it has not left in time.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 60000;
        limits.sayMs = 600000; // (and the line is not ended for what waits: only the watch ends the session)
        limits.letGoWithinMs = 600;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.goodbyeWithinMs = 500;
        QFETCH(bool, button);
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        const qkvm::core::typed::Report held{0, {0x04}};
        QVERIFY(button ? wall.screen->mouse(0x01, 1000, 2000, 0, turn.value()).ok()
                       : wall.screen->keyboard(held, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        // Two megabytes of keys that change nothing: more than Windows keeps for a line that nobody reads.
        for (int report = 0; report < 70'000; ++report) {
            QVERIFY(wall.screen->keyboard(button ? qkvm::core::typed::Report{} : held, turn.value()).ok());
            if (report % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        const QSignalSpy leftDown(wall.screen.get(), &ServerScreen::leftDown);
        QVERIFY(button ? wall.screen->mouse(0, 1000, 2000, 0, turn.value()).ok()
                       : wall.screen->keyboard({}, turn.value()).ok());
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
        QCOMPARE(leftDown.size(), 1);
        QCOMPARE(ended.at(0).at(0).toString(), u"dropped"_s);
        QTest::qWait(1000);
        wall.console.readAgain();
    }

    void aReportThatLetsGoOfOneAndPressesAnotherIsWatched_data()
    {
        QTest::addColumn<bool>("button");
        QTest::newRow("a key let go of, another pressed") << false;
        QTest::newRow("a button let go of, another pressed") << true;
    }

    void aReportThatLetsGoOfOneAndPressesAnotherIsWatched()
    {
        // (Outside review of 0.2.0, area 5, MEDIUM 5.10.) As above, but the report that lets go of A presses B with it
        // ({A} to {B}; the left button let go of, the right one pressed), said whole where nothing has waited long
        // enough to hold it back. What it lets go of is watched as every letting go is (before: only a report that let
        // go of something and pressed nothing was, and A, not taken, would repeat on the server while the line went
        // on).
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 60000;
        limits.sayMs = 600000;
        limits.letGoWithinMs = 600;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.goodbyeWithinMs = 500;
        QFETCH(bool, button);
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        const qkvm::core::typed::Report held{0, {0x04}};
        QVERIFY(button ? wall.screen->mouse(0x01, 1000, 2000, 0, turn.value()).ok()
                       : wall.screen->keyboard(held, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        for (int report = 0; report < 70'000; ++report) {
            QVERIFY(wall.screen->keyboard(button ? qkvm::core::typed::Report{} : held, turn.value()).ok());
            if (report % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        const QSignalSpy leftDown(wall.screen.get(), &ServerScreen::leftDown);
        QVERIFY(button ? wall.screen->mouse(0x02, 1000, 2000, 0, turn.value()).ok()
                       : wall.screen->keyboard({0, {0x05}}, turn.value()).ok());
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
        QCOMPARE(leftDown.size(), 1);
        QCOMPARE(ended.at(0).at(0).toString(), u"dropped"_s);
        QVERIFY(ended.at(0).at(1).toString().contains(u"may have stayed down"_s));
        QTest::qWait(1000);
        wall.console.readAgain();
    }

    void whatIsDownAsTheConsoleIsGivenAwayIsWatched()
    {
        // (Part 4, the second reading.) Somebody at the controller's own page is let in while what was said waits: what
        // this program has down is let go of first, behind what waits, and watched as every letting go is.
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.inputSayMs = 300;
        limits.letGoWithinMs = 600;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        limits.goodbyeWithinMs = 500;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        const qkvm::core::typed::Report held{0, {0x04}};
        QVERIFY(wall.screen->keyboard(held, turn.value()).ok());
        QTRY_COMPARE(wall.console.hid.size(), std::size_t{1});
        wall.console.stall();
        qkvm::base::Result<void> said;
        for (int report = 0; said.ok() && report < 400'000; ++report) {
            said = wall.screen->keyboard(held, turn.value());
            if (report % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        QCOMPARE(codeIn(said), std::string("busy"));
        const QSignalSpy ended(wall.screen.get(), &ServerScreen::ended);
        const QSignalSpy leftDown(wall.screen.get(), &ServerScreen::leftDown);
        wall.console.asks.emplace_back(ami::kind::sharing, somebody);
        QTRY_VERIFY_WITH_TIMEOUT(!wall.screen->inTurn(turn.value()).ok(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
        QCOMPARE(leftDown.size(), 1);
        QCOMPARE(ended.at(0).at(0).toString(), u"dropped"_s);
        QTest::qWait(1000);
        wall.console.readAgain();
    }

    void aTypingWhoseConsoleEndsInTheSayingIsLeftAlone()
    {
        // (The reading of part 4, H2.) A typing's letting go ends the session in the saying (what waited behind it had
        // waited too long), whoever hears that closes the screen, which stops the typing and lets go of it: the typing
        // is looked at no more after that (before: what was let go of was read, and an empty list asked for its first).
        ServerScreen::Limits limits = Wall::shortLimits();
        limits.sayMs = 1500;
        limits.inputSayMs = 200;
        limits.behindMs = 600000;
        limits.holdMs = 4000;
        limits.mostWaitingBytes = qint64{64} * 1024 * 1024;
        Wall wall(limits);
        QVERIFY(wall.shownWithin(5000));
        wall.console.mute = true; // (nothing is said to it but keys: the typing's letting go is the first late word)
        QObject::connect(wall.screen.get(), &ServerScreen::ended, wall.screen.get(), [&wall] { wall.screen->close(); });
        const Outcome typed = anOutcome();
        wall.screen->press(u"KeyA"_s, {}, into(typed));
        QTRY_VERIFY_WITH_TIMEOUT(!keysOf(wall.console).empty(), 10000);
        wall.console.stall();
        const auto turn = turnOf(*wall.screen);
        QVERIFY(turn.ok());
        // Said until Windows takes no more and what waits is refused: then the typing's letting go is the next word.
        qkvm::base::Result<void> said;
        for (int report = 0; said.ok() && report < 400'000; ++report) {
            said = wall.screen->keyboard({0, {0x04}}, turn.value());
            if (report % 200 == 199) {
                QCoreApplication::processEvents();
            }
        }
        QVERIFY(!said.ok());
        QCOMPARE(said.error().message, std::string("The server's console stopped taking keys and the mouse"));
        QVERIFY(wall.screen->holdsTheConsole());
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 10000);
        QCOMPARE(codeOf(typed), std::string("dropped"));
        QVERIFY(!wall.screen->holdsTheConsole());
        QTest::qWait(200);
        wall.console.readAgain();
    }
};

QTEST_GUILESS_MAIN(TestServerScreenBacklog)
#include "test_server_screen_backlog.moc"
