#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/incidents.h>

using namespace Qt::StringLiterals;
using qkvm::core::Incidents;

class TestIncidents : public QObject {
    Q_OBJECT
private slots:
    void malformedStoredFieldsArePreservedInsteadOfBecomingActiveIncidents_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("invalid-close") << u"closed"_s << QJsonValue(u"not a date"_s);
        QTest::newRow("wrong-name-type") << u"name"_s << QJsonValue(123);
    }
    void malformedStoredFieldsArePreservedInsteadOfBecomingActiveIncidents()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QTemporaryDir dir;
        const QString path = dir.filePath(u"history"_s);
        {
            Incidents history(path);
            history.observe(u"pc"_s, {{u"one"_s, u"Test machine"_s, u"offline"_s}});
            history.observe(u"pc"_s, {{u"one"_s, u"Test machine"_s, u"offline"_s}});
            QVERIFY(history.flush());
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonArray entries = root[u"entries"_s].toArray();
        QJsonObject entry = entries[0].toObject();
        entry[field] = value;
        entries[0] = entry;
        root[u"entries"_s] = entries;
        const QByteArray malformed = QJsonDocument(root).toJson();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(malformed), malformed.size());
        file.close();
        Incidents restored(path);
        QVERIFY(restored.damaged());
        QVERIFY(restored.entries(u"pc"_s).isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), malformed);
    }

    void settledEpisodesAreAcknowledgedAndRecoveredAcrossRestarts()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(u"incidents.json"_s);
        QDateTime now = QDateTime::fromString(u"2026-01-01T12:00:00Z"_s, Qt::ISODate);
        QString id;
        {
            Incidents history(file, [&] { return now; });
            history.begin(u"pc"_s);
            const auto see = [&](const QString& state) {
                return history.observe(u"pc"_s, {{u"one"_s, u"Test machine"_s, state}});
            };
            QVERIFY(see(u"online"_s).isEmpty());
            QVERIFY(see(u"online"_s).isEmpty());
            QVERIFY(see(u"offline"_s).isEmpty());
            QVERIFY(see(u"connecting"_s).isEmpty());
            QVERIFY(see(u"offline"_s).isEmpty());
            QCOMPARE(see(u"offline"_s).size(), 1);
            QCOMPARE(history.entries(u"pc"_s).size(), 1);
            id = history.entries(u"pc"_s).front().id;
            QVERIFY(history.acknowledge(u"pc"_s, id));
            QVERIFY(!history.acknowledge(u"other"_s, id));
            QVERIFY(history.flush());
        }
        Incidents history(file, [&] { return now; });
        history.begin(u"pc"_s);
        QCOMPARE(history.entries(u"pc"_s).front().id, id);
        QVERIFY(history.entries(u"pc"_s).front().acknowledged);
        QCOMPARE(history.observe(u"pc"_s, {{u"one"_s, u"Test machine"_s, u"offline"_s}}).size(), 0);
        now = now.addSecs(5);
        history.observe(u"pc"_s, {{u"one"_s, u"Test machine"_s, u"auth"_s}});
        QCOMPARE(history.observe(u"pc"_s, {{u"one"_s, u"Test machine"_s, u"auth"_s}}).size(), 1);
        QVERIFY(!history.entries(u"pc"_s).front().acknowledged);
        QCOMPARE(history.entries(u"pc"_s).front().id, id);
        history.observe(u"pc"_s, {{u"one"_s, u"Test machine"_s, u"online"_s}});
        QCOMPARE(history.observe(u"pc"_s, {{u"one"_s, u"Test machine"_s, u"online"_s}}).size(), 1);
        QVERIFY(history.entries(u"pc"_s).front().closed.isValid());
        QVERIFY(!history.entries(u"pc"_s).front().removed);
        QVERIFY(history.flush());
        Incidents restored(file, [&] { return now; });
        QVERIFY(restored.entries(u"pc"_s).front().closed.isValid());
    }

    void restartGraceAndFirstObservationsDoNotFloodNotifications()
    {
        QTemporaryDir dir;
        QDateTime now = QDateTime::currentDateTimeUtc();
        const QString file = dir.filePath(u"history"_s);
        {
            Incidents history(file, [&] { return now; });
            for (int i = 0; i < 2; ++i)
                history.observe(u"pc"_s, {{u"a"_s, u"A"_s, u"online"_s}});
            QVERIFY(history.flush());
        }
        Incidents history(file, [&] { return now; });
        history.begin(u"pc"_s);
        for (int i = 0; i < 4; ++i)
            QVERIFY(history.observe(u"pc"_s, {{u"a"_s, u"A"_s, u"offline"_s}}).isEmpty());
        QVERIFY(history.entries(u"pc"_s).isEmpty());
        now = now.addSecs(121);
        QVERIFY(history.observe(u"pc"_s, {{u"a"_s, u"A"_s, u"offline"_s}}).isEmpty());
        QCOMPARE(history.observe(u"pc"_s, {{u"a"_s, u"A"_s, u"offline"_s}}).size(), 1);
        for (int i = 0; i < 2; ++i)
            QVERIFY(history.observe(u"server"_s, {{u"a"_s, u"A"_s, u"auth"_s}}).isEmpty());
        QCOMPARE(history.entries(u"server"_s).size(), 1);
        QCOMPARE(history.entries(u"pc"_s).size(), 1);
    }

    void maintenanceRecordsHistoryAndReportsOnlyTheNetChange()
    {
        QTemporaryDir dir;
        QDateTime now = QDateTime::currentDateTimeUtc();
        Incidents history(dir.filePath(u"history"_s), [&] { return now; });
        const auto settled = [&](const QString& state) {
            history.observe(u"pc"_s, {{u"one"_s, u"One"_s, state}});
            return history.observe(u"pc"_s, {{u"one"_s, u"One"_s, state}});
        };
        settled(u"online"_s);
        history.pause(u"pc"_s, {}, true);
        QVERIFY(settled(u"offline"_s).isEmpty());
        QVERIFY(settled(u"online"_s).isEmpty());
        history.pause(u"pc"_s, {}, false);
        QVERIFY(settled(u"online"_s).isEmpty());
        QCOMPARE(history.entries(u"pc"_s).size(), 1);
        history.pause(u"pc"_s, u"one"_s, true);
        QVERIFY(settled(u"error"_s).isEmpty());
        QVERIFY(history.flush());
        Incidents restored(dir.filePath(u"history"_s), [&] { return now; });
        restored.begin(u"pc"_s);
        QVERIFY(restored.pausedUntil(u"pc"_s, u"one"_s) > now);
        now = now.addSecs(3601);
        QCOMPARE(restored.observe(u"pc"_s, {{u"one"_s, u"One"_s, u"error"_s}}).size(), 1);
        QVERIFY(restored.observe(u"pc"_s, {{u"one"_s, u"One"_s, u"error"_s}}).isEmpty());
    }

    void duplicateAndMissingSamplesDoNotCountAsConsecutiveLooks()
    {
        QTemporaryDir dir;
        Incidents history(dir.filePath(u"history"_s));
        const Incidents::Observation bad{u"one"_s, u"One"_s, u"offline"_s};
        history.observe(u"pc"_s, {bad, bad});
        QVERIFY(history.entries(u"pc"_s).isEmpty());
        history.observe(u"pc"_s, {});
        history.observe(u"pc"_s, {bad});
        QVERIFY(history.entries(u"pc"_s).isEmpty());
        history.observe(u"pc"_s, {bad});
        QCOMPARE(history.entries(u"pc"_s).size(), 1);
        history.reconcile(u"pc"_s, {});
        QVERIFY(history.entries(u"pc"_s).front().removed);
        QVERIFY(history.entries(u"pc"_s).front().closed.isValid());
    }

    void corruptHistoryIsPreservedUntilAnExplicitBackupAndRestart()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"history"_s);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not json");
        file.close();
        Incidents history(path);
        QVERIFY(history.damaged());
        history.observe(u"pc"_s, {{u"one"_s, u"One"_s, u"offline"_s}});
        history.observe(u"pc"_s, {{u"one"_s, u"One"_s, u"offline"_s}});
        QVERIFY(!history.flush());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("not json"));
        file.close();
        QVERIFY(history.repair());
        QCOMPARE(QDir(dir.path()).entryList({u"history.damaged-*"_s}, QDir::Files).size(), 1);
        Incidents restored(path);
        QVERIFY(!restored.damaged());
        QCOMPARE(restored.entries(u"pc"_s).size(), 1);
    }

    void failedSavingIsVisibleAndCanBeRetriedWithoutLosingEpisodes()
    {
        QTemporaryDir dir;
        QFile blocking(dir.filePath(u"folder"_s));
        QVERIFY(blocking.open(QIODevice::WriteOnly));
        blocking.close();
        Incidents history(dir.filePath(u"folder/history"_s));
        history.observe(u"pc"_s, {{u"one"_s, u"One"_s, u"offline"_s}});
        history.observe(u"pc"_s, {{u"one"_s, u"One"_s, u"offline"_s}});
        QVERIFY(!history.flush());
        QVERIFY(!history.problem().isEmpty());
        QVERIFY(blocking.remove());
        QVERIFY(history.flush());
        QVERIFY(history.problem().isEmpty());
        Incidents restored(dir.filePath(u"folder/history"_s));
        QCOMPARE(restored.entries(u"pc"_s).size(), 1);
    }

    void closedHistoryIsBoundedWithoutEvictingAnActiveIncident()
    {
        QTemporaryDir dir;
        QDateTime now = QDateTime::currentDateTimeUtc();
        Incidents history(dir.filePath(u"history"_s), [&] { return now; });
        for (int i = 0; i < Incidents::mostClosed + 3; ++i) {
            now = now.addSecs(1);
            for (int j = 0; j < 2; ++j)
                history.observe(u"pc"_s, {{u"one"_s, u"One"_s, u"offline"_s}, {u"active"_s, u"Active"_s, u"error"_s}});
            for (int j = 0; j < 2; ++j)
                history.observe(u"pc"_s, {{u"one"_s, u"One"_s, u"online"_s}, {u"active"_s, u"Active"_s, u"error"_s}});
        }
        QCOMPARE(history.entries(u"pc"_s).size(), Incidents::mostClosed + 1);
        QVERIFY(!history.entries(u"pc"_s).front().closed.isValid());
        QVERIFY(history.flush());
    }
};
QTEST_GUILESS_MAIN(TestIncidents)
#include "test_incidents.moc"
