#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <qkvm/core/server_details.h>

using namespace Qt::StringLiterals;
using namespace qkvm::core;

class TestServerDetails : public QObject {
    Q_OBJECT
private:
    static QJsonObject summary()
    {
        return {{u"since"_s, 100},
                {u"until"_s, 200},
                {u"watched"_s, 80},
                {u"online"_s, 60},
                {u"share"_s, 0.75},
                {u"first_seen"_s, 110},
                {u"last_seen"_s, 199},
                {u"state"_s, u"online"_s},
                {u"changes"_s, 2},
                {u"events"_s, 3},
                {u"sensors"_s, QJsonArray{u"CPU"_s}}};
    }

private slots:
    void oversizedOrIllFormedAnswersAreRefusedBeforeTheyReachAView()
    {
        QVERIFY(!historySummaryFrom(QByteArray(2'097'153, ' ')));
        QVERIFY(!historySummaryFrom(QByteArray("{\"state\":\"") + QByteArray::fromHex("ff") + "\"}"));
        QJsonArray entries;
        for (int i = 0; i < 101; ++i)
            entries.append(QJsonObject{{u"time"_s, i}, {u"event"_s, u"action"_s}});
        QVERIFY(!deviceActivityFrom(
            QJsonDocument(QJsonObject{{u"entries"_s, entries}, {u"more"_s, false}, {u"missing"_s, QJsonValue::Null}})
                .toJson()));
        entries.removeLast();
        QVERIFY(deviceActivityFrom(
            QJsonDocument(QJsonObject{{u"entries"_s, entries}, {u"more"_s, false}, {u"missing"_s, QJsonValue::Null}})
                .toJson()));
        QJsonArray points;
        for (int i = 0; i < 5001; ++i)
            points.append(QJsonArray{i, 40, 39, 41});
        const QJsonObject line{{u"sensor"_s, u"CPU"_s}, {u"unit"_s, u"C"_s}, {u"points"_s, points}};
        const QJsonObject samples{{u"since"_s, 0},
                                  {u"until"_s, 6000},
                                  {u"every"_s, 900},
                                  {u"series"_s, QJsonArray{line}},
                                  {u"more"_s, false}};
        QVERIFY(!historySamplesFrom(QJsonDocument(samples).toJson()));
    }

    void knownAndUnobservedHistoryHaveDifferentMeaning()
    {
        auto object = summary();
        auto found = historySummaryFrom(QJsonDocument(object).toJson());
        QVERIFY(found);
        QCOMPARE(found.value().share, std::optional<double>(0.75));
        QCOMPARE(found.value().lastSeen, std::optional<double>(199));
        object[u"share"_s] = QJsonValue::Null;
        object[u"first_seen"_s] = QJsonValue::Null;
        object[u"last_seen"_s] = QJsonValue::Null;
        object[u"state"_s] = QJsonValue::Null;
        object[u"watched"_s] = 0;
        object[u"online"_s] = 0;
        found = historySummaryFrom(QJsonDocument(object).toJson());
        QVERIFY(found);
        QVERIFY(!found.value().share);
        QVERIFY(!found.value().lastSeen);
    }

    void invalidHistoryDoesNotBecomeAnEmptySuccessfulHistory()
    {
        auto object = summary();
        object[u"share"_s] = 1.5;
        QVERIFY(!historySummaryFrom(QJsonDocument(object).toJson()));
        object = summary();
        object[u"until"_s] = 50;
        QVERIFY(!historySummaryFrom(QJsonDocument(object).toJson()));
        object = summary();
        object[u"sensors"_s] = QJsonArray{123};
        QVERIFY(!historySummaryFrom(QJsonDocument(object).toJson()));
        QVERIFY(!historySummaryFrom("{}"));
        QVERIFY(!historySummaryFrom("[]"));
    }

    void graphPointsRetainTimeAverageAndRange()
    {
        const QByteArray good =
            R"({"since":100,"until":200,"every":900,"more":false,"series":[{"sensor":"CPU","unit":"C","points":[[100,40,38,42],[150,41,40,44]]}]})";
        const auto found = historySamplesFrom(good);
        QVERIFY(found);
        QCOMPARE(found.value().series.size(), 1);
        QCOMPARE(found.value().series[0].points[1].value, 41.0);
        auto invalid = good;
        invalid.replace("[150,41,40,44]", "[90,41,40,44]");
        QVERIFY(!historySamplesFrom(invalid));
        invalid = good;
        invalid.replace("[150,41,40,44]", "[150,80,40,44]");
        QVERIFY(!historySamplesFrom(invalid));
    }

    void anUnknownUnitIsValidButAMissingOrNonStringUnitIsNot()
    {
        const QByteArray good =
            R"({"since":100,"until":200,"every":900,"more":false,"series":[{"sensor":"Fan","unit":"","points":[[100,40,38,42]]}]})";
        const auto found = historySamplesFrom(good);
        QVERIFY(found);
        QCOMPARE(found.value().series.size(), 1);
        QVERIFY(found.value().series[0].unit.isEmpty());
        for (const auto& replacement : {QByteArray("null"), QByteArray("123"), QByteArray("[]")}) {
            auto invalid = good;
            invalid.replace("\"unit\":\"\"", "\"unit\":" + replacement);
            QVERIFY(!historySamplesFrom(invalid));
        }
        auto invalid = good;
        invalid.replace("\"unit\":\"\",", "");
        QVERIFY(!historySamplesFrom(invalid));
    }

    void activityPreservesCompletenessAndOnlyItsDisplayFields()
    {
        const auto found = deviceActivityFrom(
            R"({"entries":[{"time":123,"user":"operator","event":"action","name":"Restart","action":"reset","password":"must never reach a view"}],"more":true,"missing":{"count":2,"since":100,"why":"disk full"}})");
        QVERIFY(found);
        QCOMPARE(found.value().entries.size(), 1);
        QCOMPARE(found.value().entries[0].name, u"Restart"_s);
        QVERIFY(found.value().more);
        QCOMPARE(found.value().missing, 2);
        const auto empty = deviceActivityFrom(R"({"entries":[],"more":false,"missing":null})");
        QVERIFY(empty);
        QCOMPARE(empty.value().missing, 0);
        QVERIFY(!deviceActivityFrom(R"({"entries":[],"more":false,"missing":0})"));
        QVERIFY(
            !deviceActivityFrom(R"({"entries":[],"more":false,"missing":{"count":-1,"since":100,"why":"disk full"}})"));
        QVERIFY(
            !deviceActivityFrom(R"({"entries":[{"time":"yesterday","event":"action"}],"more":false,"missing":null})"));
    }

    // b7: an alert of a machine's screen watch says which watch, after how many minutes, whether it
    // is over, and the stamp of the picture that set it off where one was kept: only a stamp of the
    // server's shape, which goes into an address. A watch this program does not know is kept as the server
    // said it, one added since (README, "Clients and versions"), with the sentence the server wrote of the entry
    // (its error, else its note, as the page shows them).
    void aScreenWatchsAlertSaysWhatWentOffAndWhereItsPictureIs()
    {
        const auto found = deviceActivityFrom(R"({"entries":[
            {"time":300,"user":"","event":"alert","state":"online","was":"online","watch":"unchanged","minutes":30,
             "picture":"17","picture_at":290.5},
            {"time":310,"event":"alert","state":"no_signal","was":"online","watch":"no_signal","minutes":1,"back":true},
            {"time":320,"event":"alert","state":"online","was":"no_signal"},
            {"time":330,"event":"alert","watch":"hung","minutes":5,"picture":"../../x",
             "note":"The screen stood still, and the lights of its keyboard did not answer."},
            {"time":340,"event":"live_control","minutes":15}],"more":false,"missing":null})");
        QVERIFY(found);
        const auto& entries = found.value().entries;
        QCOMPARE(entries.size(), 5);
        QCOMPARE(entries[0].watch, u"unchanged"_s);
        QCOMPARE(entries[0].minutes, std::optional<int>(30));
        QVERIFY(!entries[0].back);
        QCOMPARE(entries[0].picture, u"17"_s);
        QCOMPARE(entries[0].pictureAt, std::optional<double>(290.5));
        QCOMPARE(entries[1].watch, u"no_signal"_s);
        QVERIFY(entries[1].back);
        QVERIFY(entries[1].picture.isEmpty());
        QVERIFY(!entries[1].pictureAt.has_value());
        QVERIFY(entries[2].watch.isEmpty());
        QVERIFY(!entries[2].minutes.has_value());
        QVERIFY(entries[2].said.isEmpty());
        QCOMPARE(entries[3].watch, u"hung"_s);
        QCOMPARE(entries[3].said, u"The screen stood still, and the lights of its keyboard did not answer."_s);
        QVERIFY(entries[3].picture.isEmpty());
        QCOMPARE(entries[4].minutes, std::optional<int>(15));
        // What is not of the server's shape is refused whole, as every other field is.
        for (const QByteArray& entry :
             {QByteArray(R"({"time":1,"event":"alert","minutes":"thirty"})"),
              QByteArray(R"({"time":1,"event":"alert","minutes":2.5})"),
              QByteArray(R"({"time":1,"event":"alert","back":"yes"})"),
              QByteArray(R"({"time":1,"event":"alert","watch":7})"),
              QByteArray(R"({"time":1,"event":"alert","picture":17})"),
              QByteArray(R"({"time":1,"event":"alert","picture":"17","picture_at":"then"})")}) {
            QVERIFY2(!deviceActivityFrom(R"({"entries":[)" + entry + R"(],"more":false,"missing":null})"),
                     entry.constData());
        }
    }
};

QTEST_GUILESS_MAIN(TestServerDetails)
#include "test_server_details.moc"
