#include <functional>
#include <optional>

#include <QHash>
#include <QSignalSpy>
#include <QTest>

#include "bulk_run.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// What a run is given in place of a server: everything that goes out is kept until the test answers
// it, and the test sees what is out, in the order it went.
struct Bench {
    struct Out {
        core::Machine machine;
        core::Action action;
        std::function<void(base::Result<core::Done>)> told;
    };

    QList<Out> out;
    // What is under way where, as whoever owns a run keeps it.
    QHash<QString, QString> acting;
    QStringList doneOn;
    int sent = 0;

    ui::BulkRun::Ways ways()
    {
        ui::BulkRun::Ways ways;
        ways.underWay = [this](const QString& machine) { return acting.value(machine); };
        ways.began = [this](const QString& machine, const QString& title) { acting.insert(machine, title); };
        ways.ended = [this](const QString& machine) { acting.remove(machine); };
        ways.send = [this](const core::Machine& machine, const core::Action& action,
                           std::function<void(base::Result<core::Done>)> told) {
            ++sent;
            out.append(Out{machine, action, std::move(told)});
        };
        ways.mayHaveBeenDone = [](const core::Machine&, const base::Error& error) {
            return error.code == "unreachable";
        };
        ways.done = [this](const core::Machine& machine, const ui::Ask&) { doneOn.append(machine.id); };
        return ways;
    }

    // Answers what went out to this machine: done, done with a word of the server's, or not.
    void answer(const QString& machine, const base::Result<core::Done>& how = core::Done{})
    {
        for (qsizetype at = 0; at < out.size(); ++at) {
            if (out[at].machine.id == machine) {
                const Out one = out.takeAt(at);
                one.told(how);
                return;
            }
        }
        QFAIL(qPrintable(u"nothing is out to "_s + machine));
    }

    QStringList outTo() const
    {
        QStringList ids;
        for (const Out& one : out) {
            ids.append(one.machine.id);
        }
        return ids;
    }
};

QList<core::Machine> machines(int count)
{
    QList<core::Machine> all;
    for (int number = 1; number <= count; ++number) {
        core::Machine machine;
        machine.id = u"m%1"_s.arg(number);
        machine.name = u"Machine %1"_s.arg(number);
        all.append(machine);
    }
    return all;
}

ui::Ask pressing()
{
    ui::Ask ask;
    ask.title = u"Press power button"_s;
    ask.action = core::Action::plain(u"atx_power"_s);
    ask.done = u"power button pressed"_s;
    return ask;
}

// A row of what the screen is given, as "state: text".
QStringList rowsOf(const ui::BulkRun& run)
{
    QStringList rows;
    const QVariantList shown = run.shown().value(u"rows"_s).toList();
    for (const QVariant& row : shown) {
        rows.append(row.toMap().value(u"state"_s).toString() + u": "_s + row.toMap().value(u"text"_s).toString());
    }
    return rows;
}

} // namespace

// One thing done to several machines: how many go out at once, what is counted as what, what is
// offered again, and what a stop leaves unsent.
class TestBulkRun : public QObject {
    Q_OBJECT

private slots:
    void aFewGoOutAtATimeAndEveryOneIsToldOf()
    {
        Bench bench;
        ui::BulkRun run(bench.ways());
        const QSignalSpy finished(&run, &ui::BulkRun::finished);
        QVERIFY(!run.isThere());
        QVERIFY(run.shown().isEmpty());

        run.start(machines(6), pressing(), std::nullopt, u"batch-1"_s);
        QVERIFY(run.isRunning());
        QVERIFY(run.isThere());
        // Four at once, in the order of the wall; the others wait.
        QCOMPARE(bench.outTo(), (QStringList{u"m1"_s, u"m2"_s, u"m3"_s, u"m4"_s}));
        QCOMPARE(run.shown().value(u"title"_s).toString(), u"Press power button · 6 KVMs"_s);
        QCOMPARE(rowsOf(run), (QStringList{u": Sending…"_s, u": Sending…"_s, u": Sending…"_s, u": Sending…"_s,
                                           u": Waiting…"_s, u": Waiting…"_s}));
        QVERIFY(run.shown().value(u"running"_s).toBool());
        QCOMPARE(run.shown().value(u"again"_s).toString(), QString());
        // What goes out is what was asked, with what ties the run together on the server.
        QCOMPARE(bench.out[0].action.kind, u"atx_power"_s);
        QCOMPARE(bench.out[0].action.batch, u"batch-1"_s);
        QCOMPARE(bench.acting.value(u"m1"_s), u"Press power button"_s);

        // One is answered: the next goes out in its place.
        bench.answer(u"m2"_s);
        QCOMPARE(bench.outTo(), (QStringList{u"m1"_s, u"m3"_s, u"m4"_s, u"m5"_s}));
        QCOMPARE(run.shown().value(u"summary"_s).toString(), u"1 of 6 done…"_s);
        QCOMPARE(rowsOf(run).at(1), u"ok: power button pressed"_s);
        QVERIFY(!bench.acting.contains(u"m2"_s));
        QCOMPARE(bench.doneOn, QStringList{u"m2"_s});

        // What the server adds is told in place of the usual words.
        bench.answer(u"m1"_s, core::Done{u"already on"_s});
        QCOMPARE(rowsOf(run).at(0), u"ok: already on"_s);
        bench.answer(u"m3"_s);
        bench.answer(u"m4"_s);
        bench.answer(u"m5"_s);
        QVERIFY(run.isRunning());
        QCOMPARE(finished.count(), 0);
        bench.answer(u"m6"_s);
        QVERIFY(!run.isRunning());
        QCOMPARE(finished.count(), 1);
        QCOMPARE(run.told(), u"Done on all 6 KVMs."_s);
        QVERIFY(run.allDone());
        QCOMPARE(bench.sent, 6);
        QVERIFY(bench.acting.isEmpty());
        // Nothing is left to send once more.
        QCOMPARE(run.shown().value(u"again"_s).toString(), QString());
        QVERIFY(!run.shown().value(u"running"_s).toBool());
        run.retry();
        QCOMPARE(bench.sent, 6);
    }

    void oneAfterTheOtherIsOneAtATime()
    {
        Bench bench;
        ui::BulkRun run(bench.ways());
        run.start(machines(3), pressing(), 0, u"b"_s);
        QCOMPARE(bench.outTo(), QStringList{u"m1"_s});
        bench.answer(u"m1"_s);
        // At once, where no pause was asked for.
        QCOMPARE(bench.outTo(), QStringList{u"m2"_s});
        bench.answer(u"m2"_s);
        QCOMPARE(bench.outTo(), QStringList{u"m3"_s});
        bench.answer(u"m3"_s);
        QCOMPARE(run.told(), u"Done on all 3 KVMs."_s);
    }

    void aPauseIsKeptBetweenOneMachineAndTheNext()
    {
        Bench bench;
        // (A "second" of 20 ms: the pause of ten is a fifth of a second here.)
        ui::BulkRun run(bench.ways(), 20);
        const QSignalSpy finished(&run, &ui::BulkRun::finished);
        run.start(machines(2), pressing(), 10, u"b"_s);
        QCOMPARE(bench.outTo(), QStringList{u"m1"_s});
        bench.answer(u"m1"_s);
        // Not at once: the pause is said, and kept.
        QVERIFY(bench.out.isEmpty());
        QCOMPARE(run.shown().value(u"summary"_s).toString(), u"1 of 2 done… Next in 10 s."_s);
        QTest::qWait(80);
        QVERIFY(bench.out.isEmpty());
        QVERIFY(run.isRunning());
        QTRY_COMPARE_WITH_TIMEOUT(bench.outTo(), QStringList{u"m2"_s}, 2000);
        bench.answer(u"m2"_s);
        // No pause after the last one.
        QCOMPARE(finished.count(), 1);
        QCOMPARE(run.told(), u"Done on all 2 KVMs."_s);
    }

    void whatWasRefusedAndWhatGotNoAnswerAreToldApart()
    {
        Bench bench;
        ui::BulkRun run(bench.ways());
        run.start(machines(4), pressing(), std::nullopt, u"b"_s);
        bench.answer(u"m1"_s);
        bench.answer(u"m2"_s, base::Error{"not_allowed", "This account may not do that"});
        // The line fell dead: nobody knows whether the machine did it.
        bench.answer(u"m3"_s, base::Error{"unreachable", "qKVM could not be reached"});
        bench.answer(u"m4"_s);
        QVERIFY(!run.isRunning());
        QCOMPARE(rowsOf(run),
                 (QStringList{u"ok: power button pressed"_s, u"bad: This account may not do that"_s,
                              u"unknown: No answer. It may have been done."_s, u"ok: power button pressed"_s}));
        QCOMPARE(run.told(), u"2 succeeded, 1 failed, 1 without an answer."_s);
        QVERIFY(!run.allDone());
        QCOMPARE(bench.doneOn, (QStringList{u"m1"_s, u"m4"_s}));

        // Offered again: what was refused. Not what got no answer: a power button pressed a second
        // time is no retry.
        QCOMPARE(run.shown().value(u"again"_s).toString(), u"Retry failed"_s);
        run.retry();
        QVERIFY(run.isRunning());
        QCOMPARE(bench.outTo(), QStringList{u"m2"_s});
        QCOMPARE(rowsOf(run).at(1), u": Sending…"_s);
        // The others keep how they went.
        QCOMPARE(rowsOf(run).at(2), u"unknown: No answer. It may have been done."_s);
        bench.answer(u"m2"_s);
        QCOMPARE(run.told(), u"Done on the KVM."_s);
        QCOMPARE(bench.sent, 5);
    }

    void whatIsNotSentYetIsNotSentAfterAStop()
    {
        Bench bench;
        ui::BulkRun run(bench.ways());
        const QSignalSpy finished(&run, &ui::BulkRun::finished);
        run.start(machines(6), pressing(), std::nullopt, u"b"_s);
        bench.answer(u"m1"_s);
        QCOMPARE(bench.outTo(), (QStringList{u"m2"_s, u"m3"_s, u"m4"_s, u"m5"_s}));
        run.stop();
        // What is on its way cannot be called back: it is waited for, and nothing more goes out.
        QVERIFY(run.isRunning());
        QVERIFY(run.shown().value(u"stopping"_s).toBool());
        QCOMPARE(rowsOf(run).at(5), u"skipped: Not sent"_s);
        bench.answer(u"m2"_s);
        bench.answer(u"m3"_s);
        bench.answer(u"m4"_s);
        QCOMPARE(finished.count(), 0);
        bench.answer(u"m5"_s);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(bench.sent, 5);
        QCOMPARE(run.told(), u"5 succeeded, 1 not sent."_s);
        QCOMPARE(run.shown().value(u"again"_s).toString(), u"Send the rest"_s);
        // A stop of what has ended is nothing.
        run.stop();
        QCOMPARE(finished.count(), 1);

        run.retry();
        QCOMPARE(bench.outTo(), QStringList{u"m6"_s});
        bench.answer(u"m6"_s);
        QCOMPARE(run.told(), u"Done on the KVM."_s);
    }

    void aStopInAPauseEndsTheRunThereAndThen()
    {
        Bench bench;
        ui::BulkRun run(bench.ways(), 20);
        const QSignalSpy finished(&run, &ui::BulkRun::finished);
        run.start(machines(3), pressing(), 120, u"b"_s);
        bench.answer(u"m1"_s, base::Error{"busy", "Something else is being done"});
        QVERIFY(bench.out.isEmpty());
        run.stop();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(run.told(), u"0 succeeded, 1 failed, 2 not sent."_s);
        QCOMPARE(run.shown().value(u"again"_s).toString(), u"Retry failed, send the rest"_s);
        // The pause that was under way sends nothing when it is over.
        QTest::qWait(150);
        QCOMPARE(bench.sent, 1);
        // Once more: all three, in their order, one after the other as before.
        run.retry();
        QCOMPARE(bench.outTo(), QStringList{u"m1"_s});
        run.forget();
    }

    void nothingIsSentOnTopOfWhatIsUnderWayOnAMachine()
    {
        Bench bench;
        // Something went to the second machine from its own menu, and is not answered yet.
        bench.acting.insert(u"m2"_s, u"Reboot KVM"_s);
        ui::BulkRun run(bench.ways());
        run.start(machines(3), pressing(), std::nullopt, u"b"_s);
        QCOMPARE(bench.outTo(), (QStringList{u"m1"_s, u"m3"_s}));
        QCOMPARE(rowsOf(run).at(1), u"bad: \"Reboot KVM\" is still under way here"_s);
        // Whose it is stays whose it was.
        QCOMPARE(bench.acting.value(u"m2"_s), u"Reboot KVM"_s);
        bench.answer(u"m1"_s);
        bench.answer(u"m3"_s);
        QCOMPARE(run.told(), u"2 succeeded, 1 failed."_s);
        // Offered again with what failed; and by then it is free.
        bench.acting.remove(u"m2"_s);
        run.retry();
        QCOMPARE(bench.outTo(), QStringList{u"m2"_s});
        bench.answer(u"m2"_s);
        QVERIFY(run.allDone());

        // Where every machine is busy, the run is over at once, and says so.
        Bench busy;
        busy.acting = {{u"m1"_s, u"Type text"_s}, {u"m2"_s, u"Type text"_s}};
        ui::BulkRun other(busy.ways());
        const QSignalSpy finished(&other, &ui::BulkRun::finished);
        other.start(machines(2), pressing(), 0, u"b"_s);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(other.told(), u"0 succeeded, 2 failed."_s);
        QCOMPARE(busy.sent, 0);
    }

    void aRunThatIsUnderWayIsNotBegunAgain()
    {
        Bench bench;
        ui::BulkRun run(bench.ways());
        run.start(machines(2), pressing(), std::nullopt, u"first"_s);
        run.start(machines(5), pressing(), std::nullopt, u"second"_s);
        run.retry();
        QCOMPARE(bench.sent, 2);
        QCOMPARE(run.shown().value(u"rows"_s).toList().size(), 2);
        // And none is begun for nobody.
        ui::BulkRun empty(bench.ways());
        empty.start({}, pressing(), std::nullopt, u"b"_s);
        QVERIFY(!empty.isRunning());
        QVERIFY(!empty.isThere());
        run.forget();
    }

    void aRunThatIsForgottenTellsNothingMoreAndFreesItsMachines()
    {
        Bench bench;
        ui::BulkRun run(bench.ways());
        const QSignalSpy finished(&run, &ui::BulkRun::finished);
        run.start(machines(6), pressing(), std::nullopt, u"b"_s);
        QCOMPARE(bench.out.size(), 4);
        // Nobody is there any more whose run it was.
        run.forget();
        QVERIFY(!run.isRunning());
        QVERIFY(!run.isThere());
        QVERIFY(run.shown().isEmpty());
        // What was on its way is answered: the machine is free again, and nothing else follows.
        bench.answer(u"m1"_s);
        bench.answer(u"m2"_s, base::Error{"busy", "no"});
        QVERIFY(!bench.acting.contains(u"m1"_s));
        QVERIFY(!bench.acting.contains(u"m2"_s));
        QCOMPARE(bench.sent, 4);
        QCOMPARE(finished.count(), 0);
        QVERIFY(bench.doneOn.isEmpty());
        // And another run can begin, while the old one's last answers are still out.
        run.start(machines(1), pressing(), std::nullopt, u"c"_s);
        QCOMPARE(bench.sent, 5);
        bench.answer(u"m3"_s);
        bench.answer(u"m4"_s);
        QVERIFY(run.isRunning());
        // (The first machine's own answer, of this run.)
        bench.answer(u"m1"_s);
        QCOMPARE(run.told(), u"Done on the KVM."_s);
    }
};

QTEST_GUILESS_MAIN(TestBulkRun)
#include "test_bulk_run.moc"
