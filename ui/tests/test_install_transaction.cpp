#include <QTest>

#include "installer/transaction.h"

using namespace qkvm::installer;

namespace {
const Package first{L"first.msix", L"test_family", 1, 1};
const Package second{L"second.msix", L"test_family", 2, 1};
struct Desk {
    History history{first, std::nullopt, std::nullopt};
    History disk = history;
    std::optional<Package> actual = first;
    bool saveWorks = true;
    bool installWorks = true;
    bool restoreWorks = true;
    bool candidateHealthy = true;
    bool firstHealthy = true;
    bool trusted = true;
    bool failCommit = false;
    int changes = 0;
    Backend backend{[&](const Package&) { return trusted; }, [&] { return actual; },
                    [&](const History& next) {
                        if (!saveWorks || (failCommit && !next.pending))
                            return false;
                        disk = next;
                        return true;
                    },
                    [&](const Package& package, bool rollback) {
                        ++changes;
                        if (!(rollback ? restoreWorks : installWorks))
                            return false;
                        actual = package;
                        return true;
                    },
                    [&](const Package& package) { return package.version == 1 ? firstHealthy : candidateHealthy; }};
};
} // namespace

class TestInstallTransaction : public QObject {
    Q_OBJECT
private slots:
    void upgradeRetainsTheHealthCheckedVersion()
    {
        Desk desk;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::Installed);
        QVERIFY(desk.history.current == second);
        QVERIFY(desk.history.previous == first);
        QVERIFY(!desk.disk.pending);
    }
    void explicitRollbackReturnsToThePreviousVersion()
    {
        Desk desk;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::Installed);
        QCOMPARE(apply(desk.history, first, true, desk.backend), Result::RolledBack);
        QVERIFY(desk.actual == first);
        QCOMPARE(desk.changes, 2);
    }
    // L-2. A rollback whose older version fails its startup check is no rollback: the version the
    // user had is put back, and that is told apart from a rollback as asked. (Before: both gave
    // Restored, and Setup said the rollback was done, with exit code 0.)
    void aFailedRollbackIsToldApartFromARollback()
    {
        Desk desk;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::Installed);
        desk.firstHealthy = false;
        QCOMPARE(apply(desk.history, first, true, desk.backend), Result::PutBack);
        QVERIFY(desk.actual == second);
        QVERIFY(desk.history.current == second);
        QVERIFY(!desk.disk.pending);
        QCOMPARE(setupExitCode(Result::PutBack, false), 1);
        QVERIFY(!informational(Result::PutBack));
    }
    // Recovery that finishes an interrupted rollback says it was a rollback.
    void recoveryFinishingARollbackSaysSo()
    {
        Desk desk;
        desk.history = {second, first, first};
        desk.actual = first;
        QCOMPARE(recover(desk.history, desk.backend), Result::RolledBack);
        QVERIFY(desk.history.current == first);
    }
    void exitCodesAndIconsFollowWhatWasAsked()
    {
        QCOMPARE(setupExitCode(Result::Installed, false), 0);
        QCOMPARE(setupExitCode(Result::RolledBack, false), 0);
        QCOMPARE(setupExitCode(Result::PutBack, false), 1);
        QCOMPARE(setupExitCode(Result::PutBack, true), 1);
        QCOMPARE(setupExitCode(Result::KeptCurrent, false), 1); // an update or rollback that did not happen
        QCOMPARE(setupExitCode(Result::KeptCurrent, true), 0);  // a recovery with nothing to finish
        QCOMPARE(setupExitCode(Result::Refused, false), 1);
        QCOMPARE(setupExitCode(Result::RecoveryNeeded, true), 1);
        QVERIFY(informational(Result::Installed));
        QVERIFY(informational(Result::RolledBack));
        for (const auto result : {Result::PutBack, Result::KeptCurrent, Result::Refused, Result::RecoveryNeeded})
            QVERIFY(!informational(result));
    }
    void failedDeploymentKeepsTheCurrentVersion()
    {
        Desk desk;
        desk.installWorks = false;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::KeptCurrent);
        QVERIFY(desk.actual == first);
        QVERIFY(!desk.disk.pending);
    }
    void failedStartupRestoresThePreviousPackage()
    {
        Desk desk;
        desk.candidateHealthy = false;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::PutBack);
        QVERIFY(desk.actual == first);
        QCOMPARE(desk.changes, 2);
    }
    void failedRestorationKeepsTheJournalForRecovery()
    {
        Desk desk;
        desk.candidateHealthy = false;
        desk.restoreWorks = false;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::RecoveryNeeded);
        QVERIFY(desk.disk.pending == second);
        desk.restoreWorks = true;
        QCOMPARE(recover(desk.history, desk.backend), Result::PutBack);
    }
    void interruptionAfterWindowsCommitCanFinishTheJournal()
    {
        Desk desk;
        desk.history.pending = second;
        desk.actual = second;
        QCOMPARE(recover(desk.history, desk.backend), Result::Installed);
        QVERIFY(desk.history.previous == first);
        QCOMPARE(desk.changes, 0);
    }
    void interruptionBeforeWindowsCommitKeepsCurrent()
    {
        Desk desk;
        desk.history.pending = second;
        QCOMPARE(recover(desk.history, desk.backend), Result::KeptCurrent);
        QCOMPARE(desk.changes, 0);
    }
    void diskFailureNeverStartsAnUnjournalledDeployment()
    {
        Desk desk;
        desk.saveWorks = false;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::Refused);
        QCOMPARE(desk.changes, 0);
    }
    void failedFinalCommitIsRecoverable()
    {
        Desk desk;
        desk.failCommit = true;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::RecoveryNeeded);
        QVERIFY(desk.disk.pending == second);
        desk.failCommit = false;
        QCOMPARE(recover(desk.history, desk.backend), Result::Installed);
    }
    void untrustedOrIncompatiblePackagesNeverDeploy()
    {
        Desk desk;
        Package wrong = second;
        wrong.family = L"another_family";
        QCOMPARE(apply(desk.history, wrong, false, desk.backend), Result::Refused);
        wrong = second;
        wrong.profileSchema = 2;
        QCOMPARE(apply(desk.history, wrong, false, desk.backend), Result::Refused);
        desk.trusted = false;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::Refused);
        QCOMPARE(desk.changes, 0);
    }
    void anUnrecordedWindowsVersionIsNeverOverwritten()
    {
        Desk desk;
        desk.actual = second;
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::Refused);
        desk.history.pending = second;
        desk.actual->version = 3;
        QCOMPARE(recover(desk.history, desk.backend), Result::RecoveryNeeded);
        QCOMPARE(desk.changes, 0);
    }
    void arbitraryDowngradesAndForwardRollbacksAreRefused()
    {
        Desk desk;
        QCOMPARE(apply(desk.history, first, false, desk.backend), Result::Refused);
        QCOMPARE(apply(desk.history, second, true, desk.backend), Result::Refused);
        QCOMPARE(apply(desk.history, second, false, desk.backend), Result::Installed);
        QCOMPARE(apply(desk.history, first, true, desk.backend), Result::RolledBack);
        QCOMPARE(apply(desk.history, second, true, desk.backend), Result::Refused);
    }
    void firstInstallDoesNotInventARollbackPackage()
    {
        Desk desk;
        desk.history = {};
        desk.actual.reset();
        QCOMPARE(apply(desk.history, first, false, desk.backend), Result::Installed);
        QVERIFY(!desk.history.previous);
    }
};

QTEST_GUILESS_MAIN(TestInstallTransaction)
#include "test_install_transaction.moc"
