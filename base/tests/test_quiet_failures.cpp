#include <QCoreApplication>
#include <QProcess>
#include <QTest>

#include <crtdbg.h>
#include <cstdlib>

#include <windows.h>

#include <werapi.h>
#pragma comment(lib, "wer.lib")

// Every test program fails quietly and at once (quiet_failures.cpp): what it sets is in place in this one, and a
// child that fails each way ends by itself, says why on stderr, and exits with a code that is not 0.
using namespace Qt::StringLiterals;

class TestQuietFailures : public QObject {
    Q_OBJECT
private:
    struct Ended {
        bool byItself = false;
        int code = 0;
        QByteArray said;
    };
    // The child on the C library's DLL, as the tests are, or the one that carries the library in it.
    static Ended run(const QString& mode, bool carried = false)
    {
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() +
                          (carried ? u"/qkvm-test-quiet-child-static.exe"_s : u"/qkvm-test-quiet-child.exe"_s),
                      {mode});
        if (!process.waitForStarted(10000)) {
            return {};
        }
        // A window that waits for a click would hold it here: it would not end by itself.
        const bool byItself = process.waitForFinished(15000);
        if (!byItself) {
            process.kill();
            process.waitForFinished(5000);
        }
        return {byItself, process.exitCode(), process.readAllStandardError()};
    }

private slots:
    void whatEveryTestProgramSetsIsInPlace()
    {
        // In a program of nothing else, of either kind (Qt Test sets some of it again its own way here)...
        for (const bool carried : {false, true}) {
            const Ended child = run(u"settings"_s, carried);
            QVERIFY(child.byItself);
            QVERIFY2(child.code == 0 && child.said.isEmpty(), child.said.constData());
        }
        // ...and in this one, as Qt Test leaves it.
        // (Each is read back, nothing is set: the program goes on as it was.)
        const UINT errors = GetErrorMode();
        QVERIFY(errors & SEM_FAILCRITICALERRORS);
        QVERIFY(errors & SEM_NOGPFAULTERRORBOX);
        QVERIFY(errors & SEM_NOOPENFILEERRORBOX);
        DWORD reporting = 0;
        QVERIFY(SUCCEEDED(WerGetFlags(GetCurrentProcess(), &reporting)));
        QVERIFY(reporting & WER_FAULT_REPORTING_NO_UI);
        QCOMPARE(_set_abort_behavior(0, 0) & (_WRITE_ABORT_MSG | _CALL_REPORTFAULT), 0u);
        QCOMPARE(_set_error_mode(_REPORT_ERRMODE), _OUT_TO_STDERR);
        QVERIFY(_get_invalid_parameter_handler() != nullptr);
        QVERIFY(_get_purecall_handler() != nullptr);
#ifdef _DEBUG
        for (const int type : {_CRT_ASSERT, _CRT_ERROR}) {
            QCOMPARE(_CrtSetReportMode(type, _CRTDBG_REPORT_MODE) & _CRTDBG_MODE_WNDW, 0);
        }
        QVERIFY(_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_REPORT_MODE) & _CRTDBG_MODE_FILE);
        QCOMPARE(_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_REPORT_FILE), GetStdHandle(STD_ERROR_HANDLE));
#endif
    }

    void aChildThatFailsEndsByItselfAndSaysWhy_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QByteArray>("words");
        QTest::addColumn<bool>("carried");
#ifdef __SANITIZE_ADDRESS__
        const QByteArray overrun = "heap-buffer-overflow"_ba; // (the address sanitizer finds it at the write)
#else
        const QByteArray overrun = "HEAP CORRUPTION DETECTED"_ba;
#endif
#ifdef _DEBUG
        const QByteArray parameter = "Buffer is too small"_ba; // (the debug library reports it as an assert first)
#else
        const QByteArray parameter = "a check of the C or C++ library failed"_ba;
#endif
        for (const bool carried : {false, true}) {
            const char* kind = carried ? " (the C library carried in)" : "";
            auto row = [&](const char* name, const QString& mode, const QByteArray& words) {
                QTest::newRow((QByteArray(name) + kind).constData()) << mode << words << carried;
            };
            row("an index out of range", u"subscript"_s, "vector subscript out of range"_ba);
            row("an assert", u"assert"_s, "never"_ba);
            row("an _ASSERTE", u"asserte"_s, "never"_ba);
            row("a heap written past its end", u"heap"_s, overrun);
            row("a bad argument", u"parameter"_s, parameter);
            row("abort", u"abort"_s, QByteArray());
            row("a crash", u"crash"_s, QByteArray());
        }
    }
    void aChildThatFailsEndsByItselfAndSaysWhy()
    {
        QFETCH(QString, mode);
        QFETCH(QByteArray, words);
        QFETCH(bool, carried);
        const Ended ended = run(mode, carried);
        if (ended.byItself && ended.code == 77) {
            QSKIP("This build has no such check.");
        }
        QVERIFY2(ended.byItself, "it did not end by itself: something waits for a click");
        QVERIFY2(ended.code != 0, ended.said.constData());
        QVERIFY2(ended.said.contains(words), ended.said.constData());
        // Ended at the failed check, not past it: at the hook or the handler every test program has, or by abort
        // (the address sanitizer ends a program its own way).
        const bool sanitizer = ended.said.contains("AddressSanitizer");
        if (mode != u"crash"_s && !sanitizer) {
            QCOMPARE(ended.code, 3);
        }
    }

    void aChildThatDoesNotFailEndsWell()
    {
        for (const bool carried : {false, true}) {
            const Ended ended = run(u"normal"_s, carried);
            QVERIFY(ended.byItself);
            QCOMPARE(ended.code, 0);
            QVERIFY(ended.said.isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(TestQuietFailures)
#include "test_quiet_failures.moc"
