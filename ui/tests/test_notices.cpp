#include <QSignalSpy>
#include <QTest>

#include "notices.h"

using namespace Qt::StringLiterals;
using qkvm::ui::Notices;

// The notices in the corner of the window: what is told, for how long, and how many at a time.
class TestNotices : public QObject {
    Q_OBJECT

private:
    static QString kindAt(const Notices& notices, int row)
    {
        return notices.index(row, 0).data(Notices::KindRole).toString();
    }

    static QString textAt(const Notices& notices, int row)
    {
        return notices.index(row, 0).data(Notices::TextRole).toString();
    }

private slots:
    void whatIsToldIsThereInThePagesWordsForItsKind()
    {
        Notices notices;
        const QSignalSpy counted(&notices, &Notices::countChanged);
        notices.tell(Notices::Kind::Done, u"NAS: power button pressed"_s);
        notices.tell(Notices::Kind::Failed, u"NAS: Wrong password"_s);
        notices.tell(Notices::Kind::Info, u"NAS: wait"_s);
        const int number = notices.underWay(u"NAS: Reboot KVM…"_s);
        QCOMPARE(notices.count(), 4);
        QCOMPARE(notices.rowCount(), 4);
        QCOMPARE(counted.count(), 4);
        // The newest last, as they are drawn: the newest lowest.
        QCOMPARE(kindAt(notices, 0), u"ok"_s);
        QCOMPARE(textAt(notices, 0), u"NAS: power button pressed"_s);
        QCOMPARE(kindAt(notices, 1), u"error"_s);
        QCOMPARE(kindAt(notices, 2), u"info"_s);
        QCOMPARE(kindAt(notices, 3), u"working"_s);
        QCOMPARE(notices.index(3, 0).data(Notices::NumberRole).toInt(), number);
    }

    void aNoticeStaysLongEnoughToBeRead()
    {
        // As the page has it: four seconds, a failure eight, a long sentence longer, none over twelve.
        QCOMPARE(Notices::staysMs(Notices::Kind::Done, u"short"_s), 4000);
        QCOMPARE(Notices::staysMs(Notices::Kind::Info, u"short"_s), 4000);
        QCOMPARE(Notices::staysMs(Notices::Kind::Failed, u"short"_s), 8000);
        QCOMPARE(Notices::staysMs(Notices::Kind::Done, QString(100, u'x')), 7000);
        QCOMPARE(Notices::staysMs(Notices::Kind::Failed, QString(100, u'x')), 8000);
        QCOMPARE(Notices::staysMs(Notices::Kind::Failed, QString(150, u'x')), 10500);
        QCOMPARE(Notices::staysMs(Notices::Kind::Done, QString(5000, u'x')), 12000);
    }

    void aNoticeGoesByItself()
    {
        // (At a fiftieth of its time: 80 ms for the one, 160 for the other.)
        Notices notices(nullptr, 2);
        QStringList gone;
        connect(&notices, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [&](const QModelIndex&, int first, int) { gone.append(textAt(notices, first)); });
        // The failure is told first, and still goes last: it stays longer.
        notices.tell(Notices::Kind::Failed, u"failed"_s);
        notices.tell(Notices::Kind::Done, u"done"_s);
        QCOMPARE(notices.count(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 0, 3000);
        QCOMPARE(gone, (QStringList{u"done"_s, u"failed"_s}));
    }

    void whatIsUnderWayStaysUntilItIsOver()
    {
        Notices notices(nullptr, 1);
        const int first = notices.underWay(u"first"_s);
        const int second = notices.underWay(u"second"_s);
        QVERIFY(first != second);
        // Far longer than any notice stays at this pace.
        QTest::qWait(400);
        QCOMPARE(notices.count(), 2);
        notices.over(first);
        QCOMPARE(notices.count(), 1);
        QCOMPARE(textAt(notices, 0), u"second"_s);
        // Over twice, or one that never was: nothing.
        notices.over(first);
        notices.over(12345);
        QCOMPARE(notices.count(), 1);
        notices.over(second);
        QCOMPARE(notices.count(), 0);
        // Told as something under way without a number to end it by, it would stay for good: it is
        // told as something worth knowing, and goes.
        notices.tell(Notices::Kind::UnderWay, u"nobody ends this"_s);
        QCOMPARE(kindAt(notices, 0), u"info"_s);
        QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 0, 2000);
    }

    void notMoreThanAFewAtATimeAndNeverWithoutWhatIsUnderWay()
    {
        Notices notices;
        const int underWay = notices.underWay(u"under way"_s);
        for (int number = 1; number <= Notices::most + 3; ++number) {
            notices.tell(Notices::Kind::Done, u"done %1"_s.arg(number));
        }
        QCOMPARE(notices.count(), Notices::most);
        // The oldest went, but not the only sign of what is still under way.
        QCOMPARE(textAt(notices, 0), u"under way"_s);
        QCOMPARE(textAt(notices, 1), u"done %1"_s.arg(5));
        QCOMPARE(textAt(notices, Notices::most - 1), u"done %1"_s.arg(Notices::most + 3));
        notices.over(underWay);
        QCOMPARE(notices.count(), Notices::most - 1);

        // Nothing but things under way: none of them goes for a newer one.
        Notices busy;
        for (int number = 0; number < Notices::most + 2; ++number) {
            busy.underWay(u"under way"_s);
        }
        QCOMPARE(busy.count(), Notices::most + 2);
    }

    void whatIsToldForWhenTheWindowIsSeenStaysUntilSomebodySeesTheWindow()
    {
        // (At a fiftieth of its time: 80 ms.)
        Notices notices(nullptr, 2);
        QVERIFY(notices.isSeen());
        // Nobody sees the window: what is told for when somebody does stays, far longer than
        // any notice stays at this pace. What is told as ever goes as ever.
        notices.setSeen(false);
        QVERIFY(!notices.isSeen());
        notices.tellWhenSeen(Notices::Kind::Done, u"saved while nobody looked"_s);
        notices.tell(Notices::Kind::Done, u"done"_s);
        QCOMPARE(notices.count(), 2);
        QTest::qWait(600);
        QCOMPARE(notices.count(), 1);
        QCOMPARE(textAt(notices, 0), u"saved while nobody looked"_s);
        QCOMPARE(kindAt(notices, 0), u"ok"_s);
        // Nor does it go for newer ones: it is the only word of what was done.
        for (int number = 1; number <= Notices::most + 1; ++number) {
            notices.tell(Notices::Kind::Info, u"told %1"_s.arg(number));
        }
        QCOMPARE(textAt(notices, 0), u"saved while nobody looked"_s);
        QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 1, 3000);
        // Somebody sees the window: it has its time from then, and goes.
        notices.setSeen(true);
        QCOMPARE(notices.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 0, 3000);
        // Told while somebody sees the window, it is a notice like any other.
        notices.tellWhenSeen(Notices::Kind::Done, u"saved"_s);
        QCOMPARE(notices.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 0, 3000);
    }

    void whatStillWaitsIsTakenOutToBeToldSomewhereElse()
    {
        // (At a fiftieth of its time.)
        Notices notices(nullptr, 2);
        notices.tellWhenSeen(Notices::Kind::Done, u"told while somebody looked"_s);
        notices.setSeen(false);
        notices.tellWhenSeen(Notices::Kind::Done, u"saved while nobody looked"_s);
        notices.tell(Notices::Kind::Info, u"told as ever"_s);
        const int underWay = notices.underWay(u"under way"_s);
        notices.tellWhenSeen(Notices::Kind::Failed, u"refused while nobody looked"_s);
        QCOMPARE(notices.count(), 5);
        const QSignalSpy counted(&notices, &Notices::countChanged);
        // The two that wait, each as it was told and in that order; the others stay.
        const QList<Notices::Told> waited = notices.takeWhatWaits();
        QCOMPARE(waited.size(), 2);
        QCOMPARE(waited.at(0).kind, Notices::Kind::Done);
        QCOMPARE(waited.at(0).text, u"saved while nobody looked"_s);
        QCOMPARE(waited.at(1).kind, Notices::Kind::Failed);
        QCOMPARE(waited.at(1).text, u"refused while nobody looked"_s);
        QCOMPARE(notices.count(), 3);
        QCOMPARE(counted.count(), 2);
        QCOMPARE(textAt(notices, 0), u"told while somebody looked"_s);
        QCOMPARE(textAt(notices, 2), u"under way"_s);
        // Taken once: nothing waits any more, and nothing comes back when the window is seen.
        QVERIFY(notices.takeWhatWaits().isEmpty());
        notices.setSeen(true);
        notices.over(underWay);
        QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 0, 3000);
    }

    void allOfThemGoWhenTheWallIsLeft()
    {
        Notices notices(nullptr, 2);
        notices.tell(Notices::Kind::Done, u"done"_s);
        notices.underWay(u"under way"_s);
        const QSignalSpy counted(&notices, &Notices::countChanged);
        notices.clear();
        QCOMPARE(notices.count(), 0);
        QCOMPARE(counted.count(), 1);
        notices.clear();
        QCOMPARE(counted.count(), 1);
        // The time of one that is gone already runs out: nothing happens.
        QTest::qWait(300);
        QCOMPARE(notices.count(), 0);
    }
};

QTEST_GUILESS_MAIN(TestNotices)
#include "test_notices.moc"
