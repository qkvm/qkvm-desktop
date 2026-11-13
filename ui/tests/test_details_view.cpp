#include <memory>

#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include "../../core/tests/stand_in.h"
#include "details_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

namespace {
struct Fixture {
    int auditDelay = 0;
    int historyDelay = 0;
    bool unavailable = false;
    bool sensors = false;
    bool unknownUnit = false;
    bool auditUnavailable = false;
    bool forbidden = false;
    QString deniedCode = u"not_allowed"_s;
    // When each question for a history summary came, by this clock.
    QElapsedTimer clock;
    QList<qint64> summaryCame;
    // What the activity log says, where a test says it; and what the picture of an alert is answered with.
    QByteArray audit;
    Answered alertPicture{404, R"({"code":"no_picture","message":"That picture is no longer kept"})"};
    StandIn server{[this](const Asked& asked) {
        if (asked.path == "/healthz")
            return Answered(200, R"({"status":"ok","version":"test","api":["v1"]})");
        if (asked.path.contains("/alert-picture/"))
            return alertPicture;
        if (asked.path.startsWith("/api/v1/audit")) {
            if (auditUnavailable)
                return Answered(503, R"({"code":"busy","message":"Activity unavailable"})");
            Answered answer(
                200,
                !audit.isEmpty()
                    ? audit
                    : QByteArray(
                          R"({"entries":[{"time":150,"user":"administrator","event":"action","name":"Restart","action":"reset"}],"more":false,"missing":null})"));
            answer.afterMs = auditDelay;
            return answer;
        }
        if (unavailable)
            return Answered(503, R"({"code":"history_unavailable","message":"History unavailable"})");
        if (forbidden)
            return Answered(
                403, QJsonDocument(QJsonObject{{u"code"_s, deniedCode}, {u"message"_s, u"Access changed"_s}}).toJson());
        if (asked.path.contains("what=samples")) {
            QByteArray samples =
                R"({"since":100,"until":200,"every":900,"series":[{"sensor":"CPU","unit":"C","points":[[100,40,38,42],[150,41,40,44]]}],"more":false})";
            if (unknownUnit)
                samples.replace("\"unit\":\"C\"", "\"unit\":\"\"");
            return Answered(200, samples);
        }
        summaryCame.append(clock.elapsed());
        QByteArray body =
            R"({"since":100,"until":200,"first_seen":110,"last_seen":199,"state":"online","watched":80,"online":60,"share":0.75,"changes":2,"events":3,"sensors":[]})";
        if (sensors)
            body.replace("\"sensors\":[]", "\"sensors\":[\"CPU\"]");
        Answered answer(200, body);
        answer.afterMs = historyDelay;
        return answer;
    }};
    core::ServerClient client;
    Fixture()
    {
        clock.start();
        static_cast<void>(client.setAddress(server.address()));
        client.setSession(QByteArray(43, 'a'));
    }
};
} // namespace

class TestDetailsView : public QObject {
    Q_OBJECT
private slots:
    void twoWindowsOfOneServerReadTheHistoryOneAtATime()
    {
        // The server reads the history one at a time of each account (its W4b): the Details of two windows of the same
        // server ask through its one client, which sends the second read once the first was answered.
        Fixture fixture;
        fixture.historyDelay = 400;
        ui::DetailsView first;
        ui::DetailsView second;
        first.bind(&fixture.client, u"a"_s, u"account-a"_s, false, true);
        second.bind(&fixture.client, u"b"_s, u"account-a"_s, false, true);
        first.setOpened(true);
        second.setOpened(true);
        QTRY_VERIFY_WITH_TIMEOUT(!first.busy() && !second.busy(), 10000);
        QCOMPARE(fixture.summaryCame.size(), 2);
        // (Each answer is given 400 ms after its question came; a timer of that length may end a little early.)
        QVERIFY2(fixture.summaryCame[1] - fixture.summaryCame[0] >= 300,
                 qPrintable(u"%1, %2"_s.arg(fixture.summaryCame[0]).arg(fixture.summaryCame[1])));
        QCOMPARE(first.historyProblem(), QString());
        QCOMPARE(second.historyProblem(), QString());
        QVERIFY(!first.summary().isEmpty());
        QVERIFY(!second.summary().isEmpty());
    }

    // b7: a screen watch's alert says in the page's words what went off, or that it is over; an admin
    // may look at the picture that set it off, which is asked for only then, and which is said to be
    // gone where the server keeps it no more. What is no picture is not shown as one.
    void aScreenAlertSaysWhatWentOffAndItsPictureIsAskedForOnlyWhenLookedAt()
    {
        Fixture fixture;
        fixture.audit = R"({"entries":[
            {"time":300,"user":"","event":"alert","state":"online","was":"online","watch":"unchanged","minutes":30,
             "picture":"17","picture_at":290},
            {"time":290,"event":"alert","state":"online","was":"online","watch":"no_signal","minutes":1,"back":true},
            {"time":280,"event":"alert","state":"no_signal","was":"online"},
            {"time":270,"event":"alert","watch":"no_signal","minutes":1},
            {"time":260,"event":"alert","state":"online","was":"online","watch":"hung","minutes":5,
             "error":"The screen stood still, and the lights of its keyboard did not answer."},
            {"time":250,"event":"alert","state":"online","was":"online","watch":"hung","picture":"18"}],
            "more":false,"missing":null})";
        QImage red(64, 36, QImage::Format_RGB32);
        red.fill(Qt::red);
        QByteArray png;
        QBuffer into(&png);
        QVERIFY(into.open(QIODevice::WriteOnly));
        QVERIFY(red.save(&into, "PNG"));
        fixture.alertPicture = Answered{200, png, "image/png"};
        ui::DetailsView view;
        view.bind(&fixture.client, u"m 1"_s, u"account-a"_s, true, true);
        view.setOpened(true);
        QTRY_VERIFY(!view.busy());
        QCOMPARE(view.activity().size(), 6);
        const auto row = [&](int at) { return view.activity().at(at).toMap(); };
        QCOMPARE(row(0)[u"what"_s].toString(), u"Alert: Screen unchanged for 30 minutes"_s);
        QCOMPARE(row(1)[u"what"_s].toString(), u"Alert: Back to normal: a picture again"_s);
        QCOMPARE(row(2)[u"what"_s].toString(), u"Alert: Online to No signal"_s);
        QCOMPARE(row(3)[u"what"_s].toString(), u"Alert: No signal for 1 minute"_s);
        // A watch this program does not know (one added since): in the server's own word, with its sentence where it
        // wrote one; never as a change of state.
        QCOMPARE(row(4)[u"what"_s].toString(),
                 u"Alert: hung · The screen stood still, and the lights of its keyboard did not answer."_s);
        QCOMPARE(row(5)[u"what"_s].toString(), u"Alert: hung"_s);
        QVERIFY(row(5)[u"picture"_s].toBool());
        QVERIFY(row(0)[u"picture"_s].toBool());
        QVERIFY(!row(1)[u"picture"_s].toBool());
        // Not asked for until it is looked at; and a row without one has none.
        QCOMPARE(fixture.server.count("/api/v1/devices/m%201/alert-picture/"), 0);
        view.showPicture(1);
        QVERIFY(view.picture().isEmpty());
        view.showPicture(0);
        QCOMPARE(view.picture()[u"state"_s].toString(), u"loading"_s);
        QCOMPARE(view.picture()[u"title"_s].toString(), u"Screen unchanged for 30 minutes"_s);
        QVERIFY(view.picture()[u"at"_s].toString().startsWith(u"Seen by the wall "_s));
        QTRY_COMPARE(view.picture()[u"state"_s].toString(), u"shown"_s);
        QCOMPARE(view.pictureImage().size(), QSize(64, 36));
        QCOMPARE(fixture.server.count("/api/v1/devices/m%201/alert-picture/17"), 1);
        // Gone from the server.
        fixture.alertPicture = Answered{404, R"({"code":"no_picture","message":"That picture is no longer kept"})"};
        view.showPicture(0);
        QTRY_COMPARE(view.picture()[u"state"_s].toString(), u"gone"_s);
        QVERIFY(view.pictureImage().isNull());
        // Said to be a picture, and none.
        fixture.alertPicture = Answered{200, QByteArray("\x89PNG\r\n\x1a\n") + QByteArray(40, 'x'), "image/png"};
        view.showPicture(0);
        QTRY_COMPARE(view.picture()[u"state"_s].toString(), u"failed"_s);
        QVERIFY(view.pictureImage().isNull());
        view.closePicture();
        QVERIFY(view.picture().isEmpty());
        QCOMPARE(fixture.server.count("/api/v1/devices/m%201/alert-picture/"), 3);
    }

    // b7: a picture larger than a monitor's worth of pixels is not decoded at all.
    void anAlertsPictureIsDecodedOnlyWithinItsLimits()
    {
        QImage small(32, 18, QImage::Format_RGB32);
        small.fill(Qt::blue);
        QByteArray png;
        QBuffer into(&png);
        QVERIFY(into.open(QIODevice::WriteOnly));
        QVERIFY(small.save(&into, "PNG"));
        QCOMPARE(ui::DetailsView::pictureFrom(png).size(), QSize(32, 18));
        // (Its 576 pixels, against a limit of so many.)
        QCOMPARE(ui::DetailsView::pictureFrom(png, 576).size(), QSize(32, 18));
        QVERIFY(ui::DetailsView::pictureFrom(png, 575).isNull());
        QByteArray gif;
        QBuffer other(&gif);
        QVERIFY(other.open(QIODevice::WriteOnly));
        QVERIFY(small.save(&other, "BMP"));
        QVERIFY(!gif.isEmpty());
        QVERIFY(ui::DetailsView::pictureFrom(gif).isNull());
        QVERIFY(ui::DetailsView::pictureFrom({}).isNull());
    }

    void graphDataRemainsNestedAndLimitedToTheAskedMachine()
    {
        Fixture fixture;
        fixture.sensors = true;
        ui::DetailsView view;
        view.bind(&fixture.client, u"a/b?c&d"_s, u"account-a"_s, true, true);
        view.setOpened(true);
        QTRY_VERIFY(!view.busy());
        QCOMPARE(view.graphs().size(), 1);
        const auto graph = view.graphs().first().toMap();
        const auto points = graph[u"lines"_s].toList().first().toMap()[u"points"_s].toList();
        QCOMPARE(points.size(), 2);
        QCOMPARE(points[1].toList(), QVariantList({150.0, 41.0, 40.0, 44.0}));
        QCOMPARE(fixture.server.count("/api/v1/devices/a%2Fb%3Fc%26d/history?what=samples&every=900&limit=5000"), 1);
        QCOMPARE(fixture.server.count("/api/v1/audit?device=a%2Fb%3Fc%26d&limit=100"), 1);
    }

    void anUnknownUnitIsDisclosedWithoutLosingHistoryOrReportingAFailedRead()
    {
        Fixture fixture;
        fixture.sensors = true;
        fixture.unknownUnit = true;
        ui::DetailsView view;
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, false, true);
        view.setOpened(true);
        QTRY_VERIFY(!view.busy());
        QVERIFY(!view.summary().isEmpty());
        QVERIFY(view.historyProblem().isEmpty());
        QVERIFY(view.graphProblem().isEmpty());
        QVERIFY(view.graphs().isEmpty());
        QVERIFY(!view.graphNote().isEmpty());
        QCOMPARE(view.refreshAfterMs(), 60000);
    }

    void retargetingCoalescesRequestsAndShowsOnlyTheLatestMachine()
    {
        Fixture fixture;
        fixture.historyDelay = 200;
        ui::DetailsView view;
        view.bind(&fixture.client, u"first"_s, u"account-a"_s, false, true);
        view.setOpened(true);
        QTRY_COMPARE(fixture.server.count("/api/v1/devices/first/history"), 1);
        for (int i = 0; i < 30; ++i)
            view.bind(&fixture.client, QString::number(i), u"account-a"_s, false, true);
        QVERIFY(view.summary().isEmpty());
        QTRY_VERIFY(!view.busy());
        QCOMPARE(fixture.server.count("/api/v1/devices/first/history"), 1);
        QCOMPARE(fixture.server.count("/api/v1/devices/29/history"), 1);
        QCOMPARE(fixture.server.count("/api/v1/devices/15/history"), 0);
        QVERIFY(!view.summary().isEmpty());
    }

    void anEarlierActivityFailureKeepsBackoffAfterHistorySucceeds()
    {
        Fixture fixture;
        fixture.auditUnavailable = true;
        fixture.historyDelay = 200;
        ui::DetailsView view;
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, true, true);
        view.setOpened(true);
        QTRY_VERIFY(!view.busy());
        QVERIFY(!view.activityProblem().isEmpty());
        QVERIFY(!view.summary().isEmpty());
        QCOMPARE(view.refreshAfterMs(), 5000);
    }

    void anAccessRefusalClearsEverythingAndLateActivityCannotRestoreIt_data()
    {
        QTest::addColumn<QString>("code");
        for (const auto& code : {"not_allowed", "unauthorized", "not_found", "totp_required", "suspended", "forbidden"})
            QTest::newRow(code) << QString::fromLatin1(code);
    }

    void anAccessRefusalClearsEverythingAndLateActivityCannotRestoreIt()
    {
        QFETCH(QString, code);
        Fixture fixture;
        fixture.deniedCode = code;
        ui::DetailsView view;
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, true, true);
        view.setOpened(true);
        QTRY_VERIFY(!view.busy());
        QCOMPARE(view.activity().size(), 1);
        fixture.forbidden = true;
        fixture.auditDelay = 200;
        view.refresh();
        QTRY_VERIFY(!view.busy());
        QVERIFY(!view.historyProblem().isEmpty());
        QVERIFY(view.summary().isEmpty());
        QVERIFY(view.activity().isEmpty());
    }

    void viewersAskForHistoryOnlyWhileThePanelIsOpen()
    {
        Fixture fixture;
        ui::DetailsView view;
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, false, true);
        QTest::qWait(60);
        QVERIFY(fixture.server.asked.isEmpty());
        view.setOpened(true);
        QTRY_VERIFY(!view.summary().isEmpty());
        QVERIFY(!view.busy());
        QCOMPARE(fixture.server.count("/api/v1/audit"), 0);
        QCOMPARE(view.summary()[u"online"_s].toString(), u"75 %"_s);
        view.setOpened(false);
        QVERIFY(view.summary().isEmpty());
        QVERIFY(view.activity().isEmpty());
    }

    void closingReopeningAndDestroyingTheSourceLeaveNoOldBatchAlive()
    {
        auto fixture = std::make_unique<Fixture>();
        fixture->historyDelay = 200;
        ui::DetailsView view;
        view.bind(&fixture->client, u"machine"_s, u"account-a"_s, false, true);
        view.setOpened(true);
        QTRY_COMPARE(fixture->server.count("/api/v1/devices/machine/history"), 1);
        view.setOpened(false);
        view.setOpened(true);
        QVERIFY(view.summary().isEmpty());
        QTRY_VERIFY(!view.busy());
        QCOMPARE(fixture->server.count("/api/v1/devices/machine/history"), 2);
        QVERIFY(!view.summary().isEmpty());
        view.refresh();
        QVERIFY(view.busy());
        fixture.reset();
        QVERIFY(!view.available());
        QVERIFY(!view.busy());
        QVERIFY(view.summary().isEmpty());
        Fixture replacement;
        view.bind(&replacement.client, u"machine"_s, u"account-b"_s, false, true);
        QTRY_VERIFY(!view.busy());
        QVERIFY(!view.summary().isEmpty());
    }

    void roleLossDropsLateAdminRepliesAndClearsPreviousActivity()
    {
        Fixture fixture;
        fixture.auditDelay = 200;
        ui::DetailsView view;
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, true, true);
        view.setOpened(true);
        QTRY_COMPARE(fixture.server.count("/api/v1/audit"), 1);
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, false, true);
        QTest::qWait(300);
        QVERIFY(view.activity().isEmpty());
        QVERIFY(!view.administrator());
        QCOMPARE(fixture.server.count("/api/v1/audit"), 1);
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, true, true);
        QTRY_COMPARE(view.activity().size(), 1);
        view.bind(&fixture.client, u"machine"_s, u"account-b"_s, false, true);
        QVERIFY(view.activity().isEmpty());
        QVERIFY(view.summary().isEmpty());
    }

    void losingTheSourceOrLockingTheAppInvalidatesPendingReads()
    {
        Fixture fixture;
        fixture.auditDelay = 200;
        ui::DetailsView view;
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, true, true);
        view.setOpened(true);
        QTRY_COMPARE(fixture.server.count("/api/v1/audit"), 1);
        view.bind(nullptr, {}, {}, false, false);
        QTest::qWait(300);
        QVERIFY(view.summary().isEmpty());
        QVERIFY(view.activity().isEmpty());
        QVERIFY(!view.available());
        QVERIFY(!view.busy());
    }

    void historyFailureIsVisibleWithoutInventingAnEmptyHistory()
    {
        Fixture fixture;
        fixture.unavailable = true;
        ui::DetailsView view;
        view.bind(&fixture.client, u"machine"_s, u"account-a"_s, false, true);
        view.setOpened(true);
        QTRY_VERIFY(!view.historyProblem().isEmpty());
        QVERIFY(view.summary().isEmpty());
        QCOMPARE(view.refreshAfterMs(), 5000);
    }
};

QTEST_GUILESS_MAIN(TestDetailsView)
#include "test_details_view.moc"
