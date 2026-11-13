#include <QtTest>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

using namespace Qt::StringLiterals;

class TestCrashReport : public QObject {
    Q_OBJECT
private:
    static QString child() { return QCoreApplication::applicationDirPath() + u"/qkvm-crash-test-child.exe"_s; }
    static QByteArray read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }
    static int run(const QString& directory, const QString& mode)
    {
        QProcess process;
        process.start(child(), {directory, mode});
        if (!process.waitForFinished(20000)) {
            process.kill();
            process.waitForFinished(5000);
            return -1;
        }
        return process.exitCode();
    }
private slots:
    void normalExitLeavesNoCrashRecord()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QCOMPARE(run(directory.path(), u"normal"_s), 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void abnormalExitRecordsOnlyTheBoundedSummary_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"exception", "terminate", "abort", "interrupted"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void abnormalExitRecordsOnlyTheBoundedSummary()
    {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(run(directory.path(), mode) != 0);
        const auto files = QDir(directory.path()).entryList({u"*.crash.txt"_s}, QDir::Files);
        QCOMPARE(files.size(), 1);
        const auto text = read(directory.filePath(files.first()));
        QVERIFY(text.size() < 1024);
        const auto outcome = mode == u"interrupted" ? "unfinished_session"_ba : mode.toLatin1();
        QVERIFY(text.contains("outcome=" + outcome + '\n'));
        QVERIFY(text.contains("build_pe_timestamp="));
        QVERIFY(!text.contains(directory.path().toUtf8()));
        QVERIFY(!text.contains("ticket="));
        QVERIFY(!text.contains("password="));
        QCOMPARE(text.count('\n'), 10);
        // From the second reading of c7ec58c, L3: what the program does as well at a crash (the video's diagnostics
        // lines that wait to be written) runs once its summary is written, and never where nothing ran its handler.
        QCOMPARE(read(directory.filePath(u"at-a-crash.ran"_s)), mode == u"interrupted" ? QByteArray() : "ran"_ba);
    }
    void retentionIsBoundedAndUnrelatedFilesAreKept()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile unrelated(directory.filePath(u"keep.txt"_s));
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        unrelated.write("unrelated");
        unrelated.close();
        for (int n = 0; n < 12; ++n)
            QVERIFY(run(directory.path(), u"interrupted"_s) != 0);
        QCOMPARE(QDir(directory.path()).entryList({u"*.crash.txt"_s}, QDir::Files).size(), 10);
        QCOMPARE(read(unrelated.fileName()), "unrelated"_ba);
        for (const auto& name : QDir(directory.path()).entryList({u"*.crash.txt"_s}, QDir::Files)) {
            QFile old(directory.filePath(name));
            QVERIFY(old.open(QIODevice::ReadWrite));
            QVERIFY(old.setFileTime(QDateTime::currentDateTimeUtc().addDays(-31), QFileDevice::FileModificationTime));
        }
        QCOMPARE(run(directory.path(), u"normal"_s), 0);
        QVERIFY(QDir(directory.path()).entryList({u"*.crash.txt"_s}, QDir::Files).isEmpty());
        QCOMPARE(read(unrelated.fileName()), "unrelated"_ba);
    }
};
QTEST_GUILESS_MAIN(TestCrashReport)
#include "test_crash_report.moc"
