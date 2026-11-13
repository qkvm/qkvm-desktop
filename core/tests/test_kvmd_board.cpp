#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <qkvm/core/board_holds.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/sign_in_pacing.h>

#include "made_up_kvm.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::BoardHolds;
using core::KvmdDevice;
using core::testing::Answered;
using core::testing::good;
using core::testing::MadeUpKvm;
using core::testing::NobodyThere;
using core::testing::refusal;
using core::testing::StandIn;

// A kvmd KVM's power board, as a real Comet's showed it (rule 23 of the web's FOR-THE-DESKTOP-APP, its tests
// test_a_comets_power_board.py): whether the computer is on, and the board kept through an answer that says it is not
// there; the machine held as one thing for the whole of a press, a press that must change the power answered once the
// board shows the change, and the board left alone after a press that came to no clear answer, also by a driver made
// anew (a power button pressed twice undoes itself). Against a made-up KVM, and the hold itself against a clock of the
// test's own.
class TestKvmdBoard : public QObject {
    Q_OBJECT

private:
    // The times a real KVM is given, made short.
    static KvmdDevice::Pace quick()
    {
        KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.holdMs = 400;
        pace.longestHoldMs = 1600;
        pace.refusedHoldMs = 1200;
        pace.signInRetryMs = 800;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.restAfterMs = 900;
        // (And a board's own, as short as a test can tell them apart.)
        pace.pressHeldMs = 250;
        pace.longPressHeldMs = 700;
        pace.boardAnswerMs = 1200;
        pace.seenWithinMs = 1500;
        pace.seenEveryMs = 100;
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.boards = std::make_shared<BoardHolds>();
        return pace;
    }

    static core::testing::Tls tls(const char* name)
    {
        const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
        return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
    }
    static QByteArray sha256Of(const char* name)
    {
        return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
    }

    static base::Result<core::Done> doneBy(KvmdDevice& device, const QString& kind)
    {
        std::optional<base::Result<core::Done>> answer;
        device.act(core::Action::plain(kind), [&](base::Result<core::Done> came) { answer = std::move(came); });
        if (!QTest::qWaitFor([&] { return answer.has_value(); }, 20000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return *answer;
    }

    static core::Status lookedAt(KvmdDevice& device)
    {
        QSignalSpy over(&device, &KvmdDevice::looked);
        device.look();
        if (!over.wait(20000)) {
            core::Status never;
            never.state = u"the look never ended"_s;
            return never;
        }
        return device.status();
    }
    // Has the driver hold the KVM's line for events, as it does while somebody watches its picture (the line that
    // keeps its video awake: /api/ws?stream=1), and waits until that line is in and a picture has come.
    static bool heldAwake(KvmdDevice& device, MadeUpKvm& kvm)
    {
        kvm.restsUntilWatched = true;
        std::optional<base::Result<core::Snapshot>> first;
        device.askSnapshot(400, [&first](base::Result<core::Snapshot> came) { first = std::move(came); });
        return QTest::qWaitFor([&first] { return first.has_value(); }, 10000) &&
               QTest::qWaitFor([&] { return kvm.stand->live.isConnected() && device.isKeptAwake(); }, 10000);
    }
    // What a power board says of itself on that line, as kvmd's /api/ws says it: the whole of it, at every change.
    static QString atxEvent(const QString& word, bool enabled = true, bool busy = false)
    {
        const QJsonObject event{{u"enabled"_s, enabled},
                                {u"busy"_s, busy},
                                {u"power"_s, word},
                                {u"leds"_s, QJsonObject{{u"power"_s, false}, {u"hdd"_s, false}}}};
        return QString::fromUtf8(QJsonDocument(QJsonObject{{u"event_type"_s, u"atx"_s}, {u"event"_s, event}})
                                     .toJson(QJsonDocument::Compact));
    }
    static QString note(const base::Result<core::Done>& done) { return done ? done.value().note : u"(failed)"_s; }
    static std::string code(const base::Result<core::Done>& done)
    {
        return done ? std::string("(ok)") : done.error().code;
    }

private slots:
    void initTestCase()
    {
        // The device chooses Windows' own TLS, which is refused once TLS is in use: so one is made before anything
        // else touches TLS (the stand-ins do).
        const KvmdDevice first(KvmdDevice::Where{}, QString());
    }

    void theComputersPowerIsGlkvmsWord()
    {
        // A running PC whose screen sleeps, behind a real Comet: "power": "on", and its light off (the web's
        // power_said). Read by the light, its card said "Power off", and a press on that word shuts it down.
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.powerWord = u"on"_s;
        kvm.powerLight = false;
        kvm.signal = false;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        const core::Status status = lookedAt(device);
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(true));
        kvm.powerWord = u"sleep"_s;
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        kvm.powerWord = u"off"_s;
        kvm.powerLight = true;
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        // Stock kvmd says no word: the light.
        kvm.powerWord.reset();
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
    }

    void aBoardThatSaysOnceItIsNotThereIsNotTakenAway()
    {
        // A Comet's board answers one asker at a time and tells whoever asks meanwhile "enabled": false and "off": the
        // card's power buttons went with each such answer, and its "Power on" to "Power off" and back. Kept for a while
        // (the web's BOARD_KEPT and POWER_KEPT): the buttons for longer, what it said of the computer for a moment.
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.powerWord = u"on"_s;
        KvmdDevice::Pace pace = quick();
        pace.powerKeptMs = 2500;
        pace.boardKeptMs = 5000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        // (The board said it is there at some moment of the first look: no sooner than `begun`, no later than `since`.)
        QElapsedTimer begun;
        begun.start();
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        QElapsedTimer since;
        since.start();
        const QJsonObject none{{u"enabled"_s, false},
                               {u"busy"_s, false},
                               {u"power"_s, u"off"_s},
                               {u"leds"_s, QJsonObject{{u"power"_s, false}}}};
        kvm.instead.insert("/api/atx"_ba, good(none));
        core::Status status = lookedAt(device);
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(true));
        // A board that does not answer at all is kept the same way.
        kvm.instead.insert("/api/atx"_ba, Answered{500, "oops"_ba});
        status = lookedAt(device);
        QVERIFY(begun.elapsed() < pace.powerKeptMs); // (else the PC was too slow for what is tried here)
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(true));
        // What it said of the computer is old after a moment: not known, and the buttons stay.
        QTest::qWait(static_cast<int>(std::max<qint64>(0, pace.powerKeptMs + 50 - since.elapsed())));
        status = lookedAt(device);
        QVERIFY(begun.elapsed() < pace.boardKeptMs);
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::nullopt);
        // And a board that has said for long enough that it is not there is not.
        QTest::qWait(static_cast<int>(std::max<qint64>(0, pace.boardKeptMs + 50 - since.elapsed())));
        status = lookedAt(device);
        QVERIFY(!status.powerIsKnown);
        QCOMPARE(status.power, std::nullopt);
        // Back: as it says.
        kvm.instead.remove("/api/atx"_ba);
        status = lookedAt(device);
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(true));
    }

    // --- the hold, kept apart from the drivers ---

    void aClickIsHeldUntilItsAnswerSettlesIt()
    {
        qint64 nowMs = 1000;
        BoardHolds holds(16, [&nowMs] { return nowMs; });
        const BoardHolds::Of kvm{QUrl(u"http://10.0.0.1"_s), {}};
        QCOMPARE(holds.heldMs(kvm), 0);
        // Out: held for as long as its answer may take and the button's hold, as one that may be pressing.
        const quint64 click = holds.out(kvm, 8500);
        QCOMPARE(holds.heldMs(kvm), 8500);
        QVERIFY(holds.unclear(kvm));
        // A yes: the button's own hold, from then.
        nowMs += 100;
        holds.pressed(kvm, click, 500);
        QCOMPARE(holds.heldMs(kvm), 500);
        QVERIFY(!holds.unclear(kvm));
        nowMs += 500;
        QCOMPARE(holds.heldMs(kvm), 0);
        // A no: nothing.
        const quint64 refused = holds.out(kvm, 8500);
        holds.notPressed(kvm, refused);
        QCOMPARE(holds.heldMs(kvm), 0);
        QVERIFY(!holds.unclear(kvm));
        // Neither: as long again from when the answer was given up on.
        const quint64 unknown = holds.out(kvm, 8500);
        nowMs += 3000;
        holds.mayBePressing(kvm, 8500);
        QCOMPARE(holds.heldMs(kvm), 8500);
        QVERIFY(holds.unclear(kvm));
        // And what a click that was settled so says after that shortens nothing.
        holds.pressed(kvm, unknown, 500);
        holds.notPressed(kvm, unknown);
        QCOMPARE(holds.heldMs(kvm), 8500);
        QVERIFY(holds.unclear(kvm));
        // A click sent a second time (the KVM had forgotten the session) is still the one press: its no lets go of
        // nothing that held before the first went out, and of nothing it held itself.
        nowMs += 8500;
        const quint64 once = holds.out(kvm, 8500);
        nowMs += 200;
        const quint64 again = holds.out(kvm, 8500);
        QVERIFY(once != again);
        holds.notPressed(kvm, again);
        QCOMPARE(holds.heldMs(kvm), 0);
    }

    void aClickStillOutKeepsTheHoldPastItsTime()
    {
        // The merged review's LOW 9 (2026-11-06): a click whose answer is slower than its answer time and the button's
        // hold (a line that trickles) is out still: what holds the board is that click, until its answer settles it,
        // and not the clock; for twice its time at the most (a line that never ends).
        qint64 nowMs = 1000;
        BoardHolds holds(16, [&nowMs] { return nowMs; });
        const BoardHolds::Of kvm{QUrl(u"http://10.0.0.5"_s), {}};
        const quint64 click = holds.out(kvm, 8500);
        QVERIFY(holds.pressing(kvm));
        nowMs += 9000;
        QVERIFY(holds.heldMs(kvm) > 0);
        QVERIFY(holds.pressing(kvm));
        nowMs += 7000;
        QVERIFY(holds.heldMs(kvm) > 0);
        nowMs += 1000;
        QCOMPARE(holds.heldMs(kvm), 0);
        // Its answer settles it, as ever.
        nowMs = 20'000;
        const quint64 again = holds.out(kvm, 8500);
        nowMs += 9000;
        QVERIFY(holds.heldMs(kvm) > 0);
        holds.pressed(kvm, again, 500);
        QCOMPARE(holds.heldMs(kvm), 500);
        QVERIFY(!holds.pressing(kvm));
        static_cast<void>(click);
    }

    void aSecondDriverOfTheKvmIsRefusedWhileTheFirstAsksItsBoard()
    {
        // The merged review's LOW 9: the board is held from the first driver's look at its press (nothing held, nothing
        // out) to its click's going out: the board is asked first, and while that answer is on its way another driver
        // of the same KVM (the wall's and a box's, or one made anew) found nothing held and pressed too. One press
        // goes.
        MadeUpKvm kvm;
        kvm.atx = true;
        const KvmdDevice::Pace pace = quick();
        KvmdDevice first(kvm.where(), u"secret"_s, pace);
        KvmdDevice second(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(first).state, u"online"_s);
        QCOMPARE(lookedAt(second).state, u"online"_s);
        // (Its board answers the first one's question slowly.)
        Answered slowly = good({{u"enabled"_s, true},
                                {u"busy"_s, false},
                                {u"leds"_s, QJsonObject{{u"power"_s, true}, {u"hdd"_s, false}}}});
        slowly.afterMs = 600;
        kvm.first["/api/atx"_ba].append(slowly);
        std::optional<base::Result<core::Done>> pressed;
        first.act(core::Action::plain(u"atx_power"_s),
                  [&pressed](base::Result<core::Done> came) { pressed = std::move(came); });
        QTest::qWait(200);
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
        QCOMPARE(code(doneBy(second, u"atx_reset"_s)), std::string("busy"));
        QVERIFY(QTest::qWaitFor([&pressed] { return pressed.has_value(); }, 20000));
        QCOMPARE(code(*pressed), std::string("(ok)"));
        QCOMPARE(kvm.asked("/api/atx/click"), 1);
        QCOMPARE(kvm.did.count("atx power"_ba), 1);
        QCOMPARE(kvm.did.count("atx reset"_ba), 0);
    }

    void aKvmIsFoundByItsAddressAndByItsCertificate()
    {
        qint64 nowMs = 0;
        BoardHolds holds(16, [&nowMs] { return nowMs; });
        const QByteArray mark = QCryptographicHash::hash("first", QCryptographicHash::Sha256);
        const BoardHolds::Of known{QUrl(u"https://10.0.0.2"_s), mark};
        holds.mayBePressing(known, 5000);
        // The same KVM at another of its addresses, by its certificate.
        QCOMPARE(holds.heldMs(BoardHolds::Of{QUrl(u"https://kvm.lan"_s), mark}), 5000);
        // At its address, whatever it is known by.
        QCOMPARE(holds.heldMs(BoardHolds::Of{QUrl(u"https://10.0.0.2"_s), {}}), 5000);
        // Elsewhere, with nothing that tells it from another: afresh.
        QCOMPARE(holds.heldMs(BoardHolds::Of{QUrl(u"https://10.0.0.3"_s), {}}), 0);
        QCOMPARE(holds.heldMs(BoardHolds::Of{QUrl(u"http://10.0.0.2"_s), {}}), 0);
        // A card edited to the other address keeps it there, then removed and added again at that address.
        holds.mayBePressing(BoardHolds::Of{QUrl(u"https://kvm.lan"_s), mark}, 6000);
        QCOMPARE(holds.heldMs(BoardHolds::Of{QUrl(u"https://kvm.lan"_s), {}}), 6000);
        // Two holds that turn out to be one KVM's: the longer stands for both.
        holds.mayBePressing(BoardHolds::Of{QUrl(u"http://10.0.0.9"_s), {}}, 9000);
        holds.out(BoardHolds::Of{QUrl(u"http://10.0.0.9"_s), mark}, 100);
        QCOMPARE(holds.heldMs(known), 9000);
        QCOMPARE(holds.heldMs(BoardHolds::Of{QUrl(u"http://10.0.0.9"_s), {}}), 9000);
    }

    void roomIsMadeOnlyOfWhatHoldsNothing()
    {
        qint64 nowMs = 0;
        BoardHolds holds(4, [&nowMs] { return nowMs; });
        const auto at = [](int number) { return BoardHolds::Of{QUrl(u"http://10.0.1.%1"_s.arg(number)), {}}; };
        holds.mayBePressing(at(0), 60'000);
        for (int number = 1; number < 4; ++number) {
            holds.notPressed(at(number), holds.out(at(number), 8000));
        }
        for (int number = 4; number < 40; ++number) {
            holds.notPressed(at(number), holds.out(at(number), 8000));
        }
        QCOMPARE(holds.heldMs(at(0)), 60'000);
        // Where all of them hold, none is let go of to make room: more are kept.
        for (int number = 40; number < 50; ++number) {
            holds.mayBePressing(at(number), 30'000);
        }
        for (int number = 40; number < 50; ++number) {
            QCOMPARE(holds.heldMs(at(number)), 30'000);
        }
        QCOMPARE(holds.heldMs(at(0)), 60'000);
        QVERIFY(holds.unclear(at(0)));
    }

    // --- the driver ---

    void aPressIsAnsweredOnceTheBoardHasLetGoOfTheButton()
    {
        MadeUpKvm kvm;
        kvm.atx = true; // its light says the computer is on, and it sends a picture
        const KvmdDevice::Pace pace = quick();
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        QElapsedTimer took;
        took.start();
        // A short press on a computer that is on changes nothing that is looked for: answered once it is let go of.
        const auto first = doneBy(device, u"atx_power"_s);
        QCOMPARE(code(first), std::string("(ok)"));
        QVERIFY(took.elapsed() >= pace.pressHeldMs);
        // Meanwhile the machine is one thing: a second press, of the board's other button too, is refused, and the KVM
        // is not asked.
        std::optional<base::Result<core::Done>> reset;
        device.act(core::Action::plain(u"atx_reset"_s),
                   [&](base::Result<core::Done> came) { reset = std::move(came); });
        QTRY_COMPARE(kvm.asked("/api/atx/click"), 2);
        const auto meanwhile = doneBy(device, u"atx_power"_s);
        QCOMPARE(code(meanwhile), std::string("busy"));
        QCOMPARE(kvm.asked("/api/atx/click"), 2);
        QTRY_VERIFY(reset.has_value());
        QCOMPARE(code(*reset), std::string("(ok)"));
        QCOMPARE(kvm.did, (QList<QByteArray>{"atx power"_ba, "atx reset"_ba}));
    }

    void aPressThatMustChangeThePowerIsAnsweredOnceTheBoardShowsIt()
    {
        // A computer that is off, by GLKVM's word, and sends no picture: a short press must bring it on. On the real
        // one its word followed the computer three or four seconds late.
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.powerWord = u"off"_s;
        kvm.powerLight = false;
        kvm.signal = false;
        const KvmdDevice::Pace pace = quick();
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        const int late = pace.pressHeldMs + 400;
        kvm.whenPressed = [&kvm, late](const QByteArray&) {
            QTimer::singleShot(late, [&kvm] { kvm.powerWord = u"on"_s; });
        };
        QElapsedTimer took;
        took.start();
        std::optional<base::Result<core::Done>> first;
        device.act(core::Action::plain(u"atx_power"_s),
                   [&](base::Result<core::Done> came) { first = std::move(came); });
        // While the change is waited for, after the button's hold, the machine is still being done something to.
        QTest::qWait(pace.pressHeldMs + 150);
        QVERIFY(!first.has_value());
        QCOMPARE(code(doneBy(device, u"atx_reset"_s)), std::string("busy"));
        QTRY_VERIFY_WITH_TIMEOUT(first.has_value(), 5000);
        auto done = *first;
        QCOMPARE(note(done), QString());
        QVERIFY2(took.elapsed() >= late, qPrintable(QString::number(took.elapsed())));
        QVERIFY(took.elapsed() < pace.pressHeldMs + pace.seenWithinMs);
        QCOMPARE(kvm.asked("/api/atx/click"), 1);

        // The long press on a computer that is on, answered once it is off.
        kvm.signal = true;
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        const int down = pace.longPressHeldMs + 300;
        kvm.whenPressed = [&kvm, down](const QByteArray&) {
            QTimer::singleShot(down, [&kvm] { kvm.powerWord = u"off"_s; });
        };
        took.restart();
        done = doneBy(device, u"atx_power_long"_s);
        QCOMPARE(note(done), QString());
        QVERIFY2(took.elapsed() >= down, qPrintable(QString::number(took.elapsed())));
        QCOMPARE(kvm.did, (QList<QByteArray>{"atx power"_ba, "atx power_long"_ba}));
    }

    void aChangeThatIsNotSeenIsDoneAndSaysWhatWasSeen()
    {
        // Never a failure, whose retry presses the button a second time on a computer that may be on its way.
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.powerWord = u"off"_s;
        kvm.signal = false;
        const KvmdDevice::Pace pace = quick();
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        QElapsedTimer took;
        took.start();
        auto done = doneBy(device, u"atx_power"_s);
        QCOMPARE(note(done), u"pressed, but the computer is still off"_s);
        QVERIFY(took.elapsed() >= pace.pressHeldMs + pace.seenWithinMs);
        // The board that says nothing of the computer after the press.
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        kvm.whenPressed = [&kvm](const QByteArray&) { kvm.instead.insert("/api/atx"_ba, Answered{500, "oops"_ba}); };
        done = doneBy(device, u"atx_power"_s);
        QCOMPARE(note(done), u"pressed; its board has not said since"_s);
        kvm.instead.remove("/api/atx"_ba);
        kvm.whenPressed = nullptr;
        // The long press on one that stays on.
        kvm.powerWord = u"on"_s;
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        done = doneBy(device, u"atx_power_long"_s);
        QCOMPARE(note(done), u"held, but the computer is still on"_s);
        // A light that says "off" beside a picture is not wired up: no witness of what a press does, and nothing is
        // waited for.
        kvm.powerWord.reset();
        kvm.powerLight = false;
        kvm.signal = true;
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        took.restart();
        done = doneBy(device, u"atx_power"_s);
        QCOMPARE(note(done), QString());
        QVERIFY(took.elapsed() < pace.pressHeldMs + pace.seenWithinMs / 2);
    }

    void whatSaysNeitherYesNorNoHoldsTheBoard_data()
    {
        QTest::addColumn<int>("row");
        QTest::addColumn<std::string>("said");
        QTest::newRow("no answer in time") << 0 << std::string("timeout");
        QTest::newRow("an answer that broke off") << 1 << std::string("dropped");
        QTest::newRow("kvmd's own 500") << 500 << std::string("action_unknown");
        QTest::newRow("its web server's 502 while kvmd restarts") << 502 << std::string("action_unknown");
        QTest::newRow("503") << 503 << std::string("action_unknown");
        QTest::newRow("504") << 504 << std::string("action_unknown");
    }

    void whatSaysNeitherYesNorNoHoldsTheBoard()
    {
        // The board may take such a click as late as its answer is given up on, and hold the button: it is left alone,
        // whoever asks, for as long as the answer may take and the hold (the web's BOARD_ANSWER and HELD), from then.
        QFETCH(int, row);
        QFETCH(std::string, said);
        MadeUpKvm kvm;
        kvm.atx = true;
        const KvmdDevice::Pace pace = quick();
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        Answered answer = row >= 500 ? Answered{row, "<html>gateway</html>"_ba, "text/html"_ba} : Answered{};
        answer.never = row == 0;
        answer.hangUp = row == 1;
        kvm.instead.insert("/api/atx/click"_ba, answer);
        const auto first = doneBy(device, u"atx_power"_s);
        QElapsedTimer since;
        since.start();
        QCOMPARE(code(first), said);
        QVERIFY(KvmdDevice::mayHaveBeenDone(first.error()));
        QVERIFY2(first.error().message.find("look at the machine before pressing again") != std::string::npos,
                 first.error().message.c_str());
        kvm.instead.remove("/api/atx/click"_ba);
        for (const QString& kind : {u"atx_power"_s, u"atx_reset"_s, u"atx_power_long"_s}) {
            const auto again = doneBy(device, kind);
            QCOMPARE(code(again), std::string("busy"));
            QVERIFY2(again.error().message.starts_with("Its power board may still be pressing a button: its last press "
                                                       "got no clear answer."),
                     again.error().message.c_str());
        }
        QCOMPARE(kvm.asked("/api/atx/click"), 1);
        QTest::qWait(
            static_cast<int>(std::max<qint64>(0, pace.boardAnswerMs + pace.pressHeldMs - 150 - since.elapsed())));
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("busy"));
        QTest::qWait(
            static_cast<int>(std::max<qint64>(0, pace.boardAnswerMs + pace.pressHeldMs + 100 - since.elapsed())));
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("(ok)"));
        QCOMPARE(kvm.asked("/api/atx/click"), 2);
    }

    void aNoHoldsNothing()
    {
        // Only a 4xx is a no: nothing was pressed, and the next press goes at once.
        MadeUpKvm kvm; // without a power board: "ATX is not available", 400
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("action_failed"));
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("action_failed"));
        kvm.atx = true;
        kvm.instead.insert("/api/atx/click"_ba, refusal(409, u"IsBusyError"_s, u"Performing another ATX operation"_s));
        const auto busy = doneBy(device, u"atx_reset"_s);
        QCOMPARE(code(busy), std::string("action_failed"));
        QCOMPARE(busy.error().message, std::string("Performing another ATX operation"));
        kvm.instead.remove("/api/atx/click"_ba);
        QCOMPARE(code(doneBy(device, u"atx_reset"_s)), std::string("(ok)"));
        QCOMPARE(kvm.asked("/api/atx/click"), 4);
    }

    void theHoldIsKeptForADriverMadeAnew()
    {
        // A card edited meanwhile makes its driver anew: the hold is not the driver's, and stands for the new one, at
        // the KVM's address and by its certificate at another of its addresses. Another KVM is another matter.
        MadeUpKvm kvm(tls("first"));
        MadeUpKvm elsewhere(tls("first")); // (the same certificate, at another port: the same KVM, to this program)
        MadeUpKvm other;
        kvm.atx = elsewhere.atx = other.atx = true;
        KvmdDevice::Where where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = sha256Of("first");
        KvmdDevice::Where there = elsewhere.where();
        there.knownByCertificate = true;
        there.certificate = sha256Of("first");
        const KvmdDevice::Pace pace = quick();
        auto first = std::make_unique<KvmdDevice>(where, u"secret"_s, pace);
        QCOMPARE(lookedAt(*first).state, u"online"_s);
        Answered gone;
        gone.hangUp = true;
        kvm.instead.insert("/api/atx/click"_ba, gone);
        QCOMPARE(code(doneBy(*first, u"atx_power_long"_s)), std::string("dropped"));
        QElapsedTimer since;
        since.start();
        first.reset();
        kvm.instead.remove("/api/atx/click"_ba);
        KvmdDevice again(where, u"secret"_s, pace);
        QCOMPARE(code(doneBy(again, u"atx_power"_s)), std::string("busy"));
        KvmdDevice atAnother(there, u"secret"_s, pace);
        QCOMPARE(code(doneBy(atAnother, u"atx_reset"_s)), std::string("busy"));
        QCOMPARE(kvm.asked("/api/atx/click") + elsewhere.asked("/api/atx/click"), 1);
        KvmdDevice beside(other.where(), u"secret"_s, pace);
        QCOMPARE(code(doneBy(beside, u"atx_power"_s)), std::string("(ok)"));
        // (Held for the long press's own hold.)
        const qint64 heldFor = pace.boardAnswerMs + pace.longPressHeldMs;
        QTest::qWait(static_cast<int>(std::max<qint64>(0, heldFor - 250 - since.elapsed())));
        QCOMPARE(code(doneBy(again, u"atx_power"_s)), std::string("busy"));
        QTest::qWait(static_cast<int>(std::max<qint64>(0, heldFor + 100 - since.elapsed())));
        QCOMPARE(code(doneBy(again, u"atx_power"_s)), std::string("(ok)"));
    }

    void aDriverMadeAnewWhileTheBoardHoldsTheButtonWaitsForIt()
    {
        // The board said yes and holds the button on: a driver made anew meanwhile does not press beside it.
        MadeUpKvm kvm;
        kvm.atx = true;
        const KvmdDevice::Pace pace = quick();
        auto first = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, pace);
        first->act(core::Action::plain(u"atx_power_long"_s), [](const base::Result<core::Done>&) {});
        QTRY_COMPARE(kvm.did, QList<QByteArray>{"atx power_long"_ba});
        QElapsedTimer since;
        since.start();
        QTest::qWait(100); // (its yes heard by the driver: answered at once, on this PC's own address)
        first.reset();
        KvmdDevice again(kvm.where(), u"secret"_s, pace);
        const auto meanwhile = doneBy(again, u"atx_reset"_s);
        QCOMPARE(code(meanwhile), std::string("busy"));
        QVERIFY2(meanwhile.error().message.starts_with("Its power board is still holding a button"),
                 meanwhile.error().message.c_str());
        QVERIFY(since.elapsed() < pace.longPressHeldMs); // (else the PC was too slow for what is tried here)
        QTest::qWait(static_cast<int>(std::max<qint64>(0, pace.longPressHeldMs + 100 - since.elapsed())));
        QCOMPARE(code(doneBy(again, u"atx_reset"_s)), std::string("(ok)"));
        QCOMPARE(kvm.asked("/api/atx/click"), 2);
    }

    void aDriverLetGoOfWhileItsPressIsOutLeavesTheHoldBehind()
    {
        // From when it was let go of: the KVM may go on with a question whose asker has gone.
        MadeUpKvm kvm;
        kvm.atx = true;
        const KvmdDevice::Pace pace = quick();
        auto first = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(*first).state, u"online"_s);
        Answered silent;
        silent.never = true;
        kvm.instead.insert("/api/atx/click"_ba, silent);
        bool answered = false;
        first->act(core::Action::plain(u"atx_power"_s),
                   [&answered](const base::Result<core::Done>&) { answered = true; });
        QTRY_COMPARE(kvm.asked("/api/atx/click"), 1);
        QTest::qWait(400);
        first.reset();
        QElapsedTimer since;
        since.start();
        kvm.instead.remove("/api/atx/click"_ba);
        KvmdDevice again(kvm.where(), u"secret"_s, pace);
        QCOMPARE(code(doneBy(again, u"atx_power"_s)), std::string("busy"));
        QTest::qWait(
            static_cast<int>(std::max<qint64>(0, pace.boardAnswerMs + pace.pressHeldMs - 200 - since.elapsed())));
        QCOMPARE(code(doneBy(again, u"atx_power"_s)), std::string("busy"));
        QTest::qWait(
            static_cast<int>(std::max<qint64>(0, pace.boardAnswerMs + pace.pressHeldMs + 100 - since.elapsed())));
        QCOMPARE(code(doneBy(again, u"atx_power"_s)), std::string("(ok)"));
        QVERIFY(!answered); // (its answer reached nobody: the driver had gone)
    }

    void aPressThatNeverWentOutHoldsNothing()
    {
        // Nothing reached the KVM: nothing was pressed, and nothing is held.
        const NobodyThere nobody;
        KvmdDevice::Where where;
        where.address = QUrl(nobody.address());
        KvmdDevice device(where, u"secret"_s, quick());
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("unreachable"));
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("unreachable"));
        // Nor where it never signed in, or refused the session it gave.
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.password = u"another"_s;
        KvmdDevice refused(kvm.where(), u"secret"_s, quick());
        QCOMPARE(code(doneBy(refused, u"atx_power"_s)), std::string("auth_failed"));
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
        MadeUpKvm forgets;
        forgets.atx = true;
        const KvmdDevice::Pace pace = quick();
        KvmdDevice device2(forgets.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device2).state, u"online"_s);
        forgets.instead.insert("/api/atx/click"_ba, refusal(401, u"UnauthorizedError"_s));
        QCOMPARE(code(doneBy(device2, u"atx_power"_s)), std::string("auth_failed"));
        QCOMPARE(forgets.asked("/api/atx/click"), 2);
        forgets.instead.remove("/api/atx/click"_ba);
        QTest::qWait(pace.signInRetryMs + 100);
        QCOMPARE(code(doneBy(device2, u"atx_power"_s)), std::string("(ok)"));
    }

    // --- the board asked before a press, and heard on the line held anyway (a4) ---

    void aPressAsksTheBoardFirstAndIsRefusedWhileItIsBusy()
    {
        // A Comet's board says `busy` for the first half second of a press, from its own page or another program's:
        // pressed meanwhile, the press is taken as well. Asked before every press, and refused where it says so.
        MadeUpKvm kvm;
        kvm.atx = true;
        const KvmdDevice::Pace pace = quick();
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        kvm.boardBusy = true;
        const int asked = kvm.asked("/api/atx");
        const auto refused = doneBy(device, u"atx_power"_s);
        QCOMPARE(code(refused), std::string("busy"));
        QCOMPARE(refused.error().message, std::string("The KVM's power board is pressing a button at this moment (from "
                                                      "its own page, or another program). Wait for it to finish."));
        QVERIFY(!KvmdDevice::mayHaveBeenDone(refused.error()));
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
        QVERIFY(kvm.asked("/api/atx") > asked);
        // A board that says once that it is not there (it answers somebody else) is asked a second time.
        kvm.boardBusy = false;
        Answered none = good({{u"enabled"_s, false}, {u"busy"_s, false}});
        QJsonObject busy{{u"enabled"_s, true}, {u"busy"_s, true}, {u"leds"_s, QJsonObject{{u"power"_s, true}}}};
        kvm.first["/api/atx"_ba] = {none, good(busy)};
        QCOMPARE(code(doneBy(device, u"atx_reset"_s)), std::string("busy"));
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
        // And whether the computer was on before is what the board says now, not what the last look found: off since.
        kvm.powerWord = u"on"_s;
        kvm.signal = false;
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        kvm.powerWord = u"off"_s;
        const int late = pace.pressHeldMs + 300;
        kvm.whenPressed = [&kvm, late](const QByteArray&) {
            QTimer::singleShot(late, [&kvm] { kvm.powerWord = u"on"_s; });
        };
        QElapsedTimer took;
        took.start();
        QCOMPARE(note(doneBy(device, u"atx_power"_s)), QString());
        QVERIFY2(took.elapsed() >= late, qPrintable(QString::number(took.elapsed())));
        QCOMPARE(kvm.did, QList<QByteArray>{"atx power"_ba});
    }

    void aBoardThatSaysItselfOnTheLineHeldAnywayIsNotAsked()
    {
        // While somebody watches its picture the driver holds the KVM's line for events (/api/ws, which keeps its video
        // awake): the board says its whole state there at every change, and the look reads it there. One asker less
        // on a board that answers one at a time (the web's _heard_on_the_line).
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.powerWord = u"off"_s; // (what the board answers when it is asked)
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 30'000;
        pace.eventsPingMs = 300;
        pace.eventsQuietMs = 1500;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QVERIFY(heldAwake(device, kvm));
        kvm.stand->live.sayRaw(atxEvent(u"on"_s));
        QTest::qWait(150);
        int asked = kvm.asked("/api/atx");
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        QCOMPARE(kvm.asked("/api/atx"), asked);
        // It is asked whether it is there in kvmd's own words, and nothing else is said on the line.
        QTRY_VERIFY(std::ranges::any_of(kvm.stand->live.said, [](const QJsonObject& word) {
            return word == QJsonObject{{u"event_type"_s, u"ping"_s}, {u"event"_s, QJsonObject{}}};
        }));
        QVERIFY(std::ranges::all_of(kvm.stand->live.said, [](const QJsonObject& word) {
            return word.value("event_type"_L1) == QJsonValue(u"ping"_s);
        }));
        // A "no board" there says nothing of the board (a Comet's line says it too while the board answers somebody
        // else): the board is asked.
        kvm.stand->live.sayRaw(atxEvent(u"on"_s, false));
        QTest::qWait(150);
        asked = kvm.asked("/api/atx");
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        QCOMPARE(kvm.asked("/api/atx"), asked + 1);
        kvm.stand->live.sayRaw(atxEvent(u"on"_s));
        QTest::qWait(150);
        asked = kvm.asked("/api/atx");
        QCOMPARE(lookedAt(device).power, std::optional<bool>(true));
        QCOMPARE(kvm.asked("/api/atx"), asked);
        // A line that has said nothing for a while is no witness: asked.
        QTest::qWait(pace.eventsQuietMs + 200);
        asked = kvm.asked("/api/atx");
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        QCOMPARE(kvm.asked("/api/atx"), asked + 1);
        // Nor what it said before it ended.
        kvm.stand->live.sayRaw(atxEvent(u"on"_s));
        QTest::qWait(150);
        kvm.stand->live.fallOver();
        QTRY_VERIFY(!device.isKeptAwake());
        asked = kvm.asked("/api/atx");
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        QCOMPARE(kvm.asked("/api/atx"), asked + 1);
    }

    void aPressIsRefusedWhereTheLineSaysTheBoardIsBusy()
    {
        // The board is asked all the same before a press, and refused where either says busy: its line says what it
        // read last, about every second and a quarter on a real one.
        MadeUpKvm kvm;
        kvm.atx = true;
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 30'000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QVERIFY(heldAwake(device, kvm));
        kvm.stand->live.sayRaw(atxEvent(u"on"_s, true, true));
        QTest::qWait(150);
        const int asked = kvm.asked("/api/atx");
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("busy"));
        QCOMPARE(kvm.asked("/api/atx"), asked + 1);
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
        kvm.stand->live.sayRaw(atxEvent(u"on"_s));
        QTest::qWait(150);
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("(ok)"));
    }

    void theChangeIsHeardOnTheLine()
    {
        // After the hold, the board's next word on its line answers the press as soon as it says the change, rather
        // than the next asking. What the line said before the press is no witness of what the press did.
        MadeUpKvm kvm;
        kvm.atx = true;
        kvm.powerWord = u"off"_s;
        kvm.signal = false;
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 30'000;
        pace.seenEveryMs = 2500;
        pace.seenWithinMs = 4000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        QVERIFY(heldAwake(device, kvm));
        kvm.stand->live.sayRaw(atxEvent(u"off"_s));
        QTest::qWait(150);
        QCOMPARE(lookedAt(device).power, std::optional<bool>(false));
        const int after = pace.pressHeldMs + 300;
        kvm.whenPressed = [&kvm, after](const QByteArray&) {
            QTimer::singleShot(after, [&kvm] { kvm.stand->live.sayRaw(atxEvent(u"on"_s)); });
        };
        const int asked = kvm.asked("/api/atx");
        QElapsedTimer took;
        took.start();
        QCOMPARE(note(doneBy(device, u"atx_power"_s)), QString());
        QVERIFY2(took.elapsed() >= after, qPrintable(QString::number(took.elapsed())));
        QVERIFY2(took.elapsed() < pace.pressHeldMs + pace.seenEveryMs, qPrintable(QString::number(took.elapsed())));
        // (Asked before the press, and once after the hold, the line having said nothing since the click.)
        QCOMPARE(kvm.asked("/api/atx"), asked + 2);
    }

    void aBoardPressReservesTheBoardForAsLongAsItsRoadRuns()
    {
        // Review 4's LOW 3: the board is held from a press's look to its click, however long the road between takes (a
        // sign-in answered after three seconds, past twice the click's own time and its answer's).
        MadeUpKvm kvm;
        kvm.atx = true;
        KvmdDevice::Pace pace = quick();
        pace.boardAnswerMs = 500; // (its click and its answer: 750 ms, twice that 1.5 s)
        KvmdDevice first(kvm.where(), u"secret"_s, pace);
        QCOMPARE(lookedAt(first).state, u"online"_s);
        kvm.signInAfterMs = 3000;
        KvmdDevice anew(kvm.where(), u"secret"_s, pace);
        std::optional<base::Result<core::Done>> pressed;
        anew.act(core::Action::plain(u"atx_power"_s),
                 [&pressed](base::Result<core::Done> came) { pressed = std::move(came); });
        QTest::qWait(2200);
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
        QCOMPARE(code(doneBy(first, u"atx_reset"_s)), std::string("busy"));
        QVERIFY(QTest::qWaitFor([&pressed] { return pressed.has_value(); }, 20000));
        QCOMPARE(kvm.asked("/api/atx/click"), 1);
        QCOMPARE(kvm.did.count("atx reset"_ba), 0);
    }
};

QTEST_GUILESS_MAIN(TestKvmdBoard)
#include "test_kvmd_board.moc"
