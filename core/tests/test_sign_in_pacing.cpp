#include <type_traits>

#include <QElapsedTimer>
#include <QObject>
#include <QTest>

#include <qkvm/core/settings.h>
#include <qkvm/core/sign_in_pacing.h>

using namespace Qt::StringLiterals;
using qkvm::core::SignInPacing;

namespace {

// A pacing on a clock of the test's own.
struct Paced {
    qint64 nowMs = 1'000'000;
    SignInPacing pacing{SignInPacing::mostKept, [this] { return nowMs; }};
};

const QUrl kvm(u"https://192.0.2.20"_s);
// The web's own steps: a minute after a no, ten seconds after a sign-in that came to nothing.
const SignInPacing::Steps steps;

// (What it holds knows its own places in it: a copy would hold the places of the one it was made of.)
static_assert(!std::is_copy_constructible_v<SignInPacing> && !std::is_copy_assignable_v<SignInPacing>);

} // namespace

class TestSignInPacing : public QObject {
    Q_OBJECT

private slots:
    void aPasswordIsKnownByADigestOfWhereWhoAndWhatAndNothingOfItIsKept()
    {
        const SignInPacing::Mark mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        QCOMPARE(mark.digest.size(), 32);
        QVERIFY(!mark.digest.contains("secret"));
        // (Beside it, where it goes and who signs in: what may be kept of it on disk, a9. Never the password.)
        QCOMPARE(mark.road, u"login"_s);
        QCOMPARE(mark.address, u"https://192.0.2.20"_s);
        QCOMPARE(mark.user, u"admin"_s);
        QCOMPARE(SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s), mark);
        // Another of any of the four is another.
        QVERIFY(SignInPacing::what(u"web", kvm, u"admin"_s, u"secret"_s) != mark);
        QVERIFY(SignInPacing::what(u"login", QUrl(u"https://192.0.2.21"_s), u"admin"_s, u"secret"_s) != mark);
        QVERIFY(SignInPacing::what(u"login", kvm, u"root"_s, u"secret"_s) != mark);
        QVERIFY(SignInPacing::what(u"login", kvm, u"admin"_s, u"secret2"_s) != mark);
        // (And no two run together: "ab" + "c" is not "a" + "bc".)
        QVERIFY(SignInPacing::what(u"login", kvm, u"ad"_s, u"minsecret"_s) != mark);
    }

    void aNoInARowWaitsAMinuteFiveFifteenThenAnHour()
    {
        Paced paced;
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 0);
        const QList<qint64> waits{60'000, 300'000, 900'000, 3'600'000, 3'600'000};
        for (qsizetype no = 0; no < waits.size(); ++no) {
            QCOMPARE(paced.pacing.saidNo(mark, steps), waits.at(no));
            QCOMPARE(paced.pacing.times(mark), no + 1);
            QCOMPARE(paced.pacing.waitLeftMs(mark), waits.at(no));
            paced.nowMs += waits.at(no);
            QCOMPARE(paced.pacing.waitLeftMs(mark), 0);
        }
        // Let in: as if it had never been refused.
        paced.pacing.accepted(mark);
        QCOMPARE(paced.pacing.times(mark), 0);
        QCOMPARE(paced.pacing.saidNo(mark, steps), 60'000);
    }

    void aSignInThatCameToNothingIsNoNoAndWaitsTenSecondsDoublingToFiveMinutes()
    {
        Paced paced;
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        const QList<qint64> waits{10'000, 20'000, 40'000, 80'000, 160'000, 300'000, 300'000};
        for (const qint64 wait : waits) {
            paced.pacing.sent(mark, steps);
            QCOMPARE(paced.pacing.unanswered(mark, steps), wait);
            QCOMPARE(paced.pacing.times(mark), 0);
            paced.nowMs += wait;
        }
        // A no after them is the first no.
        QCOMPARE(paced.pacing.saidNo(mark, steps), 60'000);
        QCOMPARE(paced.pacing.times(mark), 1);
        // And what came to nothing after a no counts no further no.
        paced.nowMs += 60'000;
        paced.pacing.sent(mark, steps);
        QCOMPARE(paced.pacing.unanswered(mark, steps), 10'000);
        QCOMPARE(paced.pacing.times(mark), 1);
    }

    void aSignInIsCountedAsItGoesOutAndPutBackWhereItDidNot()
    {
        Paced paced;
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        // Out: until its answer is noted, it stands as one nothing answered (another driver of the KVM,
        // made meanwhile, does not send it again at once).
        QVERIFY(!paced.pacing.standing(mark));
        const SignInPacing::Sent first = paced.pacing.sent(mark, steps);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 10'000);
        QCOMPARE(paced.pacing.silent(mark), 1);
        QVERIFY(paced.pacing.standing(mark) == first.noted);
        // It found no line: put back, as if it had not been.
        paced.pacing.putBack(mark, first);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 0);
        QCOMPARE(paced.pacing.silent(mark), 0);
        // Two out at once: the one hears a no, the other found no line; the no stays.
        const SignInPacing::Sent one = paced.pacing.sent(mark, steps);
        const SignInPacing::Sent two = paced.pacing.sent(mark, steps);
        static_cast<void>(one);
        QCOMPARE(paced.pacing.saidNo(mark, steps), 60'000);
        paced.pacing.putBack(mark, two);
        QCOMPARE(paced.pacing.times(mark), 1);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 60'000);
        // That no waited out, the next one found no line: what stood before it, the no, stands again,
        // and the next no is the second in a row.
        paced.nowMs += 60'000;
        const SignInPacing::Sent three = paced.pacing.sent(mark, steps);
        paced.pacing.putBack(mark, three);
        QCOMPARE(paced.pacing.times(mark), 1);
        QCOMPARE(paced.pacing.saidNo(mark, steps), 5 * 60'000);
    }

    void nothingShortensAWaitThatStands()
    {
        Paced paced;
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        paced.pacing.saidNo(mark, steps);
        paced.pacing.saidNo(mark, steps); // five minutes
        // A sign-in noted as out, and one that came to nothing, wait ten seconds: the five minutes stand.
        paced.pacing.sent(mark, steps);
        QCOMPARE(paced.pacing.unanswered(mark, steps), 300'000);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 300'000);
    }

    void aDevicesOwnComeBackThenIsNoWordOnThePassword()
    {
        Paced paced;
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        const SignInPacing::Sent out = paced.pacing.sent(mark, steps);
        QCOMPARE(paced.pacing.later(mark, 120'000, out), 120'000);
        QCOMPARE(paced.pacing.times(mark), 0);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 120'000);
        // After a no, a later keeps the count of noes as it is.
        paced.nowMs += 120'000;
        paced.pacing.saidNo(mark, steps);
        paced.nowMs += 60'000;
        paced.pacing.later(mark, 90'000);
        QCOMPARE(paced.pacing.times(mark), 1);
    }

    void aComeBackThenNeverShortensAHoldThatStands()
    {
        // Two drivers of one KVM sign in at once: the one hears a no (a minute), the other "come back in ten
        // seconds" (a JetKVM's 429). The minute stands.
        Paced paced;
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        paced.pacing.sent(mark, steps);
        const SignInPacing::Sent other = paced.pacing.sent(mark, steps);
        paced.pacing.saidNo(mark, steps);
        QCOMPARE(paced.pacing.later(mark, 10'000, other), 60'000);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 60'000);
        QCOMPARE(paced.pacing.times(mark), 1);
        // And where no sign-in of its own is answered: the same. A longer one is kept.
        paced.nowMs += 30'000;
        QCOMPARE(paced.pacing.later(mark, 10'000), 30'000);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 30'000);
        QCOMPARE(paced.pacing.later(mark, 120'000), 120'000);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 120'000);
        QCOMPARE(paced.pacing.times(mark), 1);
    }

    void whatASignInNotedAsItWentIsNoWaitToKeepWhenItsDeviceSaysWhen()
    {
        // Three sign-ins in a row came to nothing, and eighty seconds were noted as the fourth went out; the device
        // answers that one with "come back in a minute": a minute from now, and nothing came to nothing since.
        Paced paced;
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        for (int one = 0; one < 3; ++one) {
            paced.pacing.sent(mark, steps);
            paced.pacing.unanswered(mark, steps);
            paced.nowMs += 600'000;
        }
        const SignInPacing::Sent out = paced.pacing.sent(mark, steps);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 80'000);
        QCOMPARE(paced.pacing.later(mark, 60'000, out), 60'000);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 60'000);
        QCOMPARE(paced.pacing.silent(mark), 0);
    }

    void aPasswordAPersonHasJustTypedIsTriedAsForTheFirstTime()
    {
        Paced paced;
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        const auto other = SignInPacing::what(u"login", kvm, u"admin"_s, u"other"_s);
        paced.pacing.saidNo(mark, steps);
        paced.pacing.saidNo(other, steps);
        const auto sessions = SignInPacing::what(u"session", kvm, u"admin"_s, u"wrong"_s);
        paced.pacing.cameToNothingAgain(sessions, steps);
        paced.pacing.forget(kvm, u"admin"_s, u"wrong"_s);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 0);
        QCOMPARE(paced.pacing.times(mark), 0);
        // (And a controller's sessions it kept refusing: RedfishDevice.)
        QCOMPARE(paced.pacing.waitLeftMs(sessions), 0);
        // Only that one.
        QCOMPARE(paced.pacing.times(other), 1);
    }

    void asManyAreKeptAsTheMostAndTheOldestOfWhatProtectsNothingGoFirst()
    {
        // From the outside review (F3): what goes to make room is only what protects nothing any more, a hold that has
        // run out with no sign-in out, and no no that still counts (the merged review, 6); the oldest of that first. A
        // hold that stands and a sign-in that is out are kept, however many there are: one dropped would be a password
        // offered again too soon. (Sign-ins that came to nothing here: what goes, and what stays.)
        qint64 nowMs = 0;
        SignInPacing pacing(3, [&nowMs] { return nowMs; });
        QList<SignInPacing::Mark> marks;
        for (int one = 0; one < 6; ++one) {
            marks.append(SignInPacing::what(u"login", kvm, u"admin"_s, u"secret%1"_s.arg(one)));
        }
        for (int one = 0; one < 4; ++one) {
            pacing.cameToNothingAgain(marks.at(one), steps);
        }
        // Four holds that stand, of three that are kept at the most: all four.
        QCOMPARE(pacing.kept(), qsizetype{4});
        QCOMPARE(pacing.silent(marks.at(0)), 1);
        QVERIFY(pacing.waitLeftMs(marks.at(0)) > 0);
        // Their ten seconds over, one more: the oldest go first, until there are as many as the most.
        nowMs += steps.unansweredMs;
        pacing.cameToNothingAgain(marks.at(4), steps);
        QCOMPARE(pacing.kept(), qsizetype{3});
        QCOMPARE(pacing.silent(marks.at(0)), 0);
        QCOMPARE(pacing.silent(marks.at(1)), 0);
        QCOMPARE(pacing.silent(marks.at(2)), 1);
        QCOMPARE(pacing.silent(marks.at(4)), 1);
        // One that is noted anew is the newest from then on.
        pacing.cameToNothingAgain(marks.at(2), steps);
        pacing.cameToNothingAgain(marks.at(5), steps);
        QCOMPARE(pacing.silent(marks.at(3)), 0);
        QCOMPARE(pacing.silent(marks.at(2)), 2);
        QCOMPARE(pacing.kept(), qsizetype{3});
    }

    void aNoThatStillCountsIsNeverLetGoOfToMakeRoom()
    {
        // The merged review's LOW 6 (2026-11-06): a no whose hold has run out still counts: the next no in a row waits
        // five minutes, not one. Let go of to make room, it was offered as for the first time, and a JetKVM, which
        // shuts an address out after five wrong passwords, was offered five within about five minutes.
        qint64 nowMs = 0;
        SignInPacing pacing(2, [&nowMs] { return nowMs; });
        const SignInPacing::Mark refused = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        QCOMPARE(pacing.saidNo(refused, steps), 60'000);
        nowMs += steps.refusedMs;
        for (int one = 0; one < 5; ++one) {
            pacing.unanswered(SignInPacing::what(u"login", kvm, u"admin"_s, u"other%1"_s.arg(one)), steps);
            nowMs += steps.unansweredMs;
        }
        QCOMPARE(pacing.kept(), qsizetype{2});
        QCOMPARE(pacing.times(refused), 1);
        QCOMPARE(pacing.waitLeftMs(refused), 0);
        QCOMPARE(pacing.saidNo(refused, steps), 300'000);
    }

    void asManyAreKeptAsAWallOfTheMostKvmsHasRoadsFor()
    {
        // Each KVM of the most a wall holds by every road a sign-in goes (a device's own, a controller's sessions and
        // its web page's), with room to spare for passwords changed meanwhile.
        QVERIFY(SignInPacing::mostKept >= 3 * qkvm::core::Remembered::mostKvms);
        QCOMPARE(SignInPacing::mostKept, qsizetype{8192});
    }

    void aNoThatStandsIsKeptWhateverComesAfterIt()
    {
        // From the outside review (F3, its rejectionSurvivesCapacity): a no, and then as many others as are kept at
        // the most, all of them standing. That no stands still: its count, and its wait.
        qint64 nowMs = 0;
        SignInPacing pacing(SignInPacing::mostKept, [&nowMs] { return nowMs; });
        const SignInPacing::Mark first("rejected-mark");
        pacing.saidNo(first, steps);
        for (qsizetype one = 0; one < SignInPacing::mostKept; ++one) {
            pacing.saidNo(QByteArray::number(one), steps);
        }
        QCOMPARE(pacing.times(first), 1);
        QVERIFY(pacing.waitLeftMs(first) > 0);
        QCOMPARE(pacing.kept(), SignInPacing::mostKept + 1);
    }

    void aSignInThatIsOutIsKeptWhateverComesAfterIt()
    {
        // From the outside review (F3, its outstandingSurvivesCapacity): a sign-in that is out, and then as many noes
        // as are kept at the most. It holds still. And past its hold by the clock, its line not ended yet: it is out
        // all the same, and does not go to make room for what comes after (nor is whoever waits for it told it
        // settled).
        qint64 nowMs = 0;
        SignInPacing pacing(SignInPacing::mostKept, [&nowMs] { return nowMs; });
        const SignInPacing::Mark first("outstanding-mark");
        const SignInPacing::Steps slow{60'000, 10'000, 48'500};
        pacing.sent(first, slow);
        QObject waiter;
        int told = 0;
        pacing.whenSettled(first, &waiter, 600'000, [&told] { ++told; });
        for (qsizetype one = 0; one < SignInPacing::mostKept; ++one) {
            pacing.saidNo(QByteArray::number(one), steps);
        }
        QVERIFY(pacing.onlyOut(first));
        nowMs += 2 * slow.outMs + steps.refusedMs;
        for (qsizetype one = 0; one < SignInPacing::mostKept; ++one) {
            pacing.saidNo(QByteArray::number(one) + "-later", steps);
        }
        const std::optional<SignInPacing::Kept> stands = pacing.standing(first);
        QVERIFY(stands && stands->out != 0);
        QTest::qWait(50);
        QCOMPARE(told, 0);
        // What it comes to settles it, as ever.
        pacing.accepted(first);
        QTRY_COMPARE(told, 1);
    }

    void aHealthyKvmIsLetInWhereAllThatIsKeptIsHistory()
    {
        // As many noes as are kept at the most, all of them run out: a KVM that is well, new to this, has nothing
        // standing against it, and signs in as ever. The noes still count (their next no waits longer), and are kept
        // beside it: more than the most, for as long as they do (turned by the merged review, 6: they were let go of
        // to make room, and a no began again at a minute).
        qint64 nowMs = 0;
        SignInPacing pacing(SignInPacing::mostKept, [&nowMs] { return nowMs; });
        for (qsizetype one = 0; one < SignInPacing::mostKept; ++one) {
            pacing.saidNo(QByteArray::number(one), steps);
        }
        nowMs += steps.refusedMs * 60;
        const auto well = SignInPacing::what(u"login", kvm, u"admin"_s, u"right"_s);
        QCOMPARE(pacing.waitLeftMs(well), 0);
        QCOMPARE(pacing.times(well), 0);
        pacing.sent(well, steps);
        QVERIFY(pacing.onlyOut(well));
        QCOMPARE(pacing.kept(), SignInPacing::mostKept + 1);
        pacing.accepted(well);
        QVERIFY(!pacing.standing(well));
        QCOMPARE(pacing.times(QByteArray::number(0)), 1);
        QCOMPARE(pacing.kept(), SignInPacing::mostKept);
    }

    void aHealthyKvmIsLetInWhereAllThatIsKeptStillProtects()
    {
        // The choice where all that is kept still protects (from the outside review, F3): nothing of it goes, and a KVM
        // that is well, new to this, is let in all the same, with nothing standing against it: no "wrong password", and
        // no wait it did not earn. What is kept is then more than the most, for as long as those holds stand: every one
        // of them stands for a sign-in that went out, and the drivers pace each. Once they have run out, their noes
        // still count, and they are kept still (the merged review, 6).
        qint64 nowMs = 0;
        SignInPacing pacing(SignInPacing::mostKept, [&nowMs] { return nowMs; });
        const SignInPacing::Mark first("rejected-mark");
        pacing.saidNo(first, steps);
        for (qsizetype one = 0; one < SignInPacing::mostKept; ++one) {
            pacing.saidNo(QByteArray::number(one), steps);
        }
        const auto well = SignInPacing::what(u"login", kvm, u"admin"_s, u"right"_s);
        QCOMPARE(pacing.waitLeftMs(well), 0);
        QCOMPARE(pacing.times(well), 0);
        pacing.sent(well, steps);
        QVERIFY(pacing.onlyOut(well));
        QCOMPARE(pacing.times(well), 0);
        pacing.accepted(well);
        QVERIFY(!pacing.standing(well));
        QCOMPARE(pacing.times(first), 1);
        QVERIFY(pacing.waitLeftMs(first) > 0);
        QCOMPARE(pacing.kept(), SignInPacing::mostKept + 1);
        nowMs += steps.refusedMs;
        pacing.saidNo(well, steps);
        QCOMPARE(pacing.kept(), SignInPacing::mostKept + 2);
        QCOMPARE(pacing.times(well), 1);
        QCOMPARE(pacing.times(first), 1);
    }

    void theStepsAreTheDriversOwnAndTheLadderTheSame()
    {
        Paced paced;
        const SignInPacing::Steps brief{100, 50};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        for (const qint64 wait : {100, 500, 1500, 6000, 6000}) {
            QCOMPARE(paced.pacing.saidNo(mark, brief), wait);
            paced.nowMs += wait;
        }
        const auto quiet = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        for (const qint64 wait : {50, 100, 200, 400, 800, 1500, 1500}) {
            paced.pacing.sent(quiet, brief);
            QCOMPARE(paced.pacing.unanswered(quiet, brief), wait);
            paced.nowMs += wait;
        }
    }

    void aSignInThatIsOutHoldsAsLongAsItCanBeOut()
    {
        // (The pacing's reading, M1.) A slow controller's no came after twelve seconds; the hold of one that came to
        // nothing is ten: the other driver of it sent the wrong password a second time. Out until what it comes to is
        // told, its line's end: by the clock, twice as long as it can be out, for a line that never ends (the second
        // reading: held as long as it could be out, by the clock alone, a waiter woke as the hold ended and sent while
        // the first was still on its line, given up a moment later).
        Paced paced;
        const SignInPacing::Steps slow{60'000, 10'000, 25'500};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        const SignInPacing::Sent out = paced.pacing.sent(mark, slow);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 51'000);
        QVERIFY(paced.pacing.onlyOut(mark));
        QVERIFY(out.noted.out);
        paced.nowMs += 25'500;
        QVERIFY(paced.pacing.onlyOut(mark)); // (as long as it could be out, and its line not heard to end: out still)
        QCOMPARE(paced.pacing.waitLeftMs(mark), 25'500);
        // Its no replaces it.
        QCOMPARE(paced.pacing.saidNo(mark, slow), 60'000);
        QVERIFY(!paced.pacing.onlyOut(mark));
        // Where what came to nothing waits longer than that, that is the hold.
        const auto quiet = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        for (int one = 0; one < 3; ++one) {
            paced.pacing.unanswered(quiet, slow, paced.pacing.sent(quiet, slow));
            paced.nowMs += 600'000;
        }
        paced.pacing.sent(quiet, slow);
        QCOMPARE(paced.pacing.waitLeftMs(quiet), 80'000);
    }

    void whatASignInComesToReplacesTheHoldItWentOutWith()
    {
        Paced paced;
        const SignInPacing::Steps slow{60'000, 10'000, 25'500};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        // Nothing heard: the wait after one that came to nothing, from now, not what it went out with.
        SignInPacing::Sent out = paced.pacing.sent(mark, slow);
        paced.nowMs += 3000;
        QCOMPARE(paced.pacing.unanswered(mark, slow, out), 10'000);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 10'000);
        QCOMPARE(paced.pacing.silent(mark), 1);
        QVERIFY(!paced.pacing.onlyOut(mark));
        // A device's "come back then": then.
        paced.nowMs += 10'000;
        out = paced.pacing.sent(mark, slow);
        QCOMPARE(paced.pacing.later(mark, 5000, out), 5000);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 5000);
        // Put back: what stood before it.
        paced.nowMs += 5000;
        out = paced.pacing.sent(mark, slow);
        paced.pacing.putBack(mark, out);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 0);
        // In: nothing.
        out = paced.pacing.sent(mark, slow);
        paced.pacing.accepted(mark);
        QVERIFY(!paced.pacing.standing(mark));
        // And where something was noted since (another driver's no), what it comes to shortens nothing.
        out = paced.pacing.sent(mark, slow);
        paced.pacing.saidNo(mark, slow);
        QCOMPARE(paced.pacing.unanswered(mark, slow, out), 60'000);
        QCOMPARE(paced.pacing.times(mark), 1);
    }

    void aPasswordTypedAnewWhileASignInWithItIsOutWaitsForIt()
    {
        // (The pacing's second reading, M2.) The wall's driver has the wrong password out; a person types the same in
        // the Edit box and saves. Its noes and what came to nothing are forgotten; the sign-in that is out is not, and
        // the typed one waits for it. (Dropped, the box sent it beside the one out: two strikes at a JetKVM from one
        // save.)
        Paced paced;
        const SignInPacing::Steps slow{60'000, 10'000, 25'500};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        paced.pacing.saidNo(mark, slow);
        paced.nowMs += 60'000;
        paced.pacing.sent(mark, slow);
        QVERIFY(!paced.pacing.onlyOut(mark)); // (a no before it)
        QObject waiter;
        int told = 0;
        paced.pacing.whenSettled(mark, &waiter, 60'000, [&told] { ++told; });
        paced.pacing.forget(kvm, u"admin"_s, u"wrong"_s);
        QCOMPARE(paced.pacing.times(mark), 0);
        QCOMPARE(paced.pacing.silent(mark), 0);
        QCOMPARE(paced.pacing.waitLeftMs(mark), 51'000);
        QVERIFY(paced.pacing.onlyOut(mark));
        QTest::qWait(50);
        QCOMPARE(told, 0); // (out still: nobody is told)
        // Its no is a no all the same, the first since the password was typed.
        QCOMPARE(paced.pacing.saidNo(mark, slow), 60'000);
        QCOMPARE(paced.pacing.times(mark), 1);
        QTRY_COMPARE(told, 1);
        // Put back after it was typed anew: nothing of what stood before it comes back.
        paced.nowMs += 60'000;
        const SignInPacing::Sent again = paced.pacing.sent(mark, slow);
        paced.pacing.forget(kvm, u"admin"_s, u"wrong"_s);
        paced.pacing.putBack(mark, again);
        QVERIFY(!paced.pacing.standing(mark));
        // Nothing heard after it: the first that came to nothing, from now, and not as long as it went out with.
        paced.pacing.saidNo(mark, slow);
        paced.nowMs += 60'000;
        const SignInPacing::Sent third = paced.pacing.sent(mark, slow);
        paced.pacing.forget(kvm, u"admin"_s, u"wrong"_s);
        paced.nowMs += 1000;
        QCOMPARE(paced.pacing.unanswered(mark, slow, third), 10'000);
        QCOMPARE(paced.pacing.times(mark), 0);
        QCOMPARE(paced.pacing.silent(mark), 1);
        QVERIFY(!paced.pacing.onlyOut(mark));
    }

    void aSignInPutBackOverALeftoverTellsWhoeverWaits()
    {
        // (The pacing's third reading, L3.) What stood before a sign-in that found no line was a leftover of one that
        // never ended (its cap is up): put back, it is out no longer, and whoever waits is told at once.
        Paced paced;
        const SignInPacing::Steps slow{60'000, 10'000, 25'500};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        paced.pacing.sent(mark, slow);
        paced.nowMs += 51'000;
        QVERIFY(!paced.pacing.onlyOut(mark));
        const SignInPacing::Sent again = paced.pacing.sent(mark, slow);
        QObject waiter;
        int told = 0;
        paced.pacing.whenSettled(mark, &waiter, 60'000, [&told] { ++told; });
        paced.pacing.putBack(mark, again);
        QTRY_COMPARE(told, 1);
        QCOMPARE(paced.pacing.standing(mark).value_or(SignInPacing::Kept{}).out, quint64{0});
    }

    void whatIsNoVerdictOnASignInThatIsOutKeepsItOut()
    {
        // (The pacing's third reading, L5.) One that came to nothing, or a device's "come back then", noted where they
        // are no verdict on the sign-in that is out (another's, the session refused once more): it stays out, and a
        // password typed after it keeps its hold (cleared, the typed one went beside it).
        Paced paced;
        const SignInPacing::Steps slow{60'000, 10'000, 25'500};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        for (int write = 0; write < 3; ++write) {
            const SignInPacing::Sent out = paced.pacing.sent(mark, slow);
            if (write == 0) {
                paced.pacing.unanswered(mark, slow);
            } else if (write == 1) {
                paced.pacing.later(mark, 1000);
            } else {
                paced.pacing.cameToNothingAgain(mark, slow);
            }
            QCOMPARE(paced.pacing.standing(mark).value_or(SignInPacing::Kept{}).out, out.noted.out);
            paced.pacing.forget(kvm, u"admin"_s, u"secret"_s);
            QVERIFY(paced.pacing.onlyOut(mark));
            paced.pacing.accepted(mark);
            paced.nowMs += 600'000;
        }
    }

    void onlyASignInThatIsOutAndNoNoBeforeItIsWaitedFor()
    {
        Paced paced;
        const SignInPacing::Steps slow{60'000, 10'000, 25'500};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        QVERIFY(!paced.pacing.onlyOut(mark));
        paced.pacing.saidNo(mark, slow);
        paced.nowMs += 60'000;
        paced.pacing.sent(mark, slow);
        QVERIFY(!paced.pacing.onlyOut(mark)); // (a no before it: that is what is said)
        const auto other = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        paced.pacing.sent(other, slow);
        QVERIFY(paced.pacing.onlyOut(other));
        paced.nowMs += 51'000;
        QVERIFY(!paced.pacing.onlyOut(other)); // (its cap is up: a line that never ended)
        // What a controller's refusing of its sessions holds is nothing that is out, and climbs as one sent and never
        // answered.
        const auto sessions = SignInPacing::what(u"session", kvm, u"admin"_s, u"secret"_s);
        for (const qint64 wait : {10'000, 20'000, 40'000}) {
            QCOMPARE(paced.pacing.cameToNothingAgain(sessions, slow), wait);
            QVERIFY(!paced.pacing.onlyOut(sessions));
            paced.nowMs += wait;
        }
    }

    void whoeverWaitsIsToldOnceWhenItSettlesAndFromTheLoop()
    {
        Paced paced;
        const SignInPacing::Steps slow{60'000, 10'000, 25'500};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        QObject waiter;
        int told = 0;
        paced.pacing.sent(mark, slow);
        paced.pacing.whenSettled(mark, &waiter, 600, [&told] { ++told; });
        QTest::qWait(50);
        QCOMPARE(told, 0);
        // Sent again while out (another, after its password was typed anew): out still, nobody told.
        paced.pacing.sent(mark, slow);
        QTest::qWait(50);
        QCOMPARE(told, 0);
        paced.pacing.saidNo(mark, slow);
        QCOMPARE(told, 0); // (not from within the call that settled it)
        QTRY_COMPARE(told, 1);
        QTest::qWait(800); // (its time up after: not told again)
        QCOMPARE(told, 1);
        // Each verdict tells.
        const auto quiet = SignInPacing::what(u"login", kvm, u"admin"_s, u"other"_s);
        for (int verdict = 0; verdict < 5; ++verdict) {
            const SignInPacing::Sent out = paced.pacing.sent(quiet, slow);
            int heard = 0;
            paced.pacing.whenSettled(quiet, &waiter, 60'000, [&heard] { ++heard; });
            switch (verdict) {
            case 0:
                paced.pacing.unanswered(quiet, slow, out);
                break;
            case 1:
                paced.pacing.later(quiet, 1000, out);
                break;
            case 2:
                paced.pacing.putBack(quiet, out);
                break;
            case 3:
                paced.pacing.accepted(quiet);
                break;
            default:
                paced.pacing.clear();
                break;
            }
            QTRY_COMPARE(heard, 1);
            paced.pacing.accepted(quiet);
            paced.nowMs += 600'000;
        }
        // And never one let go of to make room (from the outside review, F3): kept beside however many come after it,
        // it is what it comes to that tells.
        qint64 nowMs = 0;
        SignInPacing few(1, [&nowMs] { return nowMs; });
        few.sent(quiet, slow);
        int settled = 0;
        few.whenSettled(quiet, &waiter, 60'000, [&settled] { ++settled; });
        few.sent(mark, slow);
        QTest::qWait(50);
        QCOMPARE(settled, 0);
        QVERIFY(few.onlyOut(quiet));
        few.saidNo(quiet, slow);
        QTRY_COMPARE(settled, 1);
    }

    void whoeverWaitsIsToldAfterItsTimeWhereNothingSettlesIt()
    {
        Paced paced;
        const SignInPacing::Steps slow{60'000, 10'000, 25'500};
        const auto mark = SignInPacing::what(u"login", kvm, u"admin"_s, u"secret"_s);
        paced.pacing.sent(mark, slow);
        QObject waiter;
        int told = 0;
        QElapsedTimer watch;
        watch.start();
        paced.pacing.whenSettled(mark, &waiter, 300, [&told] { ++told; });
        QTRY_COMPARE(told, 1);
        QVERIFY(watch.elapsed() >= 290);
        // Nothing out: told at once, from the loop.
        const auto none = SignInPacing::what(u"login", kvm, u"admin"_s, u"none"_s);
        int now = 0;
        paced.pacing.whenSettled(none, &waiter, 60'000, [&now] { ++now; });
        QCOMPARE(now, 0);
        QTRY_COMPARE(now, 1);
        // Never where whoever waits is gone.
        int gone = 0;
        {
            QObject brief;
            paced.pacing.whenSettled(mark, &brief, 200, [&gone] { ++gone; });
        }
        paced.pacing.accepted(mark);
        QTest::qWait(400);
        QCOMPARE(gone, 0);
    }

    void theProgramHasOneOfItsOwn() { QCOMPARE(&SignInPacing::program(), &SignInPacing::program()); }
};

QTEST_GUILESS_MAIN(TestSignInPacing)
#include "test_sign_in_pacing.moc"
