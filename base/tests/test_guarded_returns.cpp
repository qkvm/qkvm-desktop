#include <QCoreApplication>
#include <QProcess>
#include <QTest>

#include <windows.h>

// A program of ours whose return address is overwritten on its stack is ended by Windows, and never goes where the
// overwriting points, wherever the processor guards returns (SPEC.md section 3): on ARM64 by its pointer
// authentication, with which every function of ours signs its return address and checks it (/guard:signret, in every
// build: CMakeLists.txt), and on x64 by the shadow stack a release of ours is marked fit for (/CETCOMPAT). The child
// (forged_return.cpp) is built as our programs are; its twin without that guard shows that the overwriting itself
// works, so that the guarded child's end is the guard's doing. Seen 2026-11-21: on the ARM test PC (whose processor has
// pointer authentication) and on the build box (whose processor has a shadow stack), the guarded child ended 0xC0000409
// and the unguarded one landed.
using namespace Qt::StringLiterals;

class TestGuardedReturns : public QObject {
    Q_OBJECT
private:
    // How the forged return ends where nothing guards it (forged_return.cpp), and Windows' fast fail, which is how a
    // guarded one ended where it was seen.
    static constexpr quint32 landedThere = 42;
    static constexpr quint32 fastFail = 0xC0000409;

    struct Ended {
        bool byItself = false;
        quint32 code = 0;
        QByteArray said;
    };
    static Ended run(bool guarded, const QString& mode)
    {
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() +
                          (guarded ? u"/qkvm-test-forged-return.exe"_s : u"/qkvm-test-forged-return-unguarded.exe"_s),
                      {mode});
        if (!process.waitForStarted(10000)) {
            return {};
        }
        const bool byItself = process.waitForFinished(15000);
        if (!byItself) {
            process.kill();
            process.waitForFinished(5000);
        }
        return {byItself, static_cast<quint32>(process.exitCode()),
                process.readAllStandardOutput() + process.readAllStandardError()};
    }

#if defined(_M_ARM64)
    // Whether this processor has pointer authentication, as Windows says of its first core's ID registers
    // (ID_AA64ISAR1_EL1, "CP 4031": APA and API; ID_AA64ISAR2_EL1, "CP 4032": APA3).
    static bool processorSignsReturns()
    {
        const auto readRegister = [](const wchar_t* name) {
            ULONGLONG value = 0;
            DWORD size = sizeof value;
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", name,
                             RRF_RT_QWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
                return ULONGLONG{0};
            }
            return value;
        };
        const ULONGLONG isar1 = readRegister(L"CP 4031");
        const ULONGLONG isar2 = readRegister(L"CP 4032");
        return ((isar1 >> 4) & 0xF) != 0 || ((isar1 >> 8) & 0xF) != 0 || ((isar2 >> 12) & 0xF) != 0;
    }
#endif

private slots:
    void theChildReturnsAsAnyProgramDoesUntilItsReturnIsOverwritten()
    {
        for (const bool guarded : {false, true}) {
            const Ended child = run(guarded, u"plain"_s);
            QVERIFY(child.byItself);
            QVERIFY2(child.code == 0 && child.said.contains("RETURNED 1"), child.said.constData());
        }
    }

    void anOverwrittenReturnGoesWhereItPointsWhereNothingGuardsIt()
    {
        // (Else the guarded child's end below would prove nothing: the overwriting is what it is said to be.)
        const Ended child = run(false, u"forge"_s);
        QVERIFY(child.byItself);
        QVERIFY2(child.code == landedThere && !child.said.contains("RETURNED"),
                 qPrintable(
                     u"ended 0x%1: %2"_s.arg(child.code, 8, 16, QChar(u'0')).arg(QString::fromLocal8Bit(child.said))));
    }

    void anOverwrittenReturnEndsAProgramOfOursWhereTheProcessorGuardsReturns()
    {
        const Ended guards = run(true, u"guards"_s);
        QVERIFY(guards.byItself);
        QCOMPARE(guards.code, 0u);
#if defined(_M_ARM64)
        if (!processorSignsReturns()) {
            QSKIP("This processor has no pointer authentication: its signing instructions do nothing here.");
        }
        // Where the processor has it, Windows turns it on for a program of ours (as it is by default).
        QVERIFY2(guards.said.contains("pointer-auth 1"), guards.said.constData());
#else
        if (!guards.said.contains("shadow-stack 1")) {
            QSKIP("Windows put no shadow stack on the child: this processor has none, or this build is not a release "
                  "(only a release's link marks a program fit for one).");
        }
#endif
        const Ended child = run(true, u"forge"_s);
        QVERIFY(child.byItself);
        QVERIFY2(child.code == fastFail && !child.said.contains("RETURNED"),
                 qPrintable(
                     u"ended 0x%1: %2"_s.arg(child.code, 8, 16, QChar(u'0')).arg(QString::fromLocal8Bit(child.said))));
    }
};

QTEST_GUILESS_MAIN(TestGuardedReturns)
#include "test_guarded_returns.moc"
