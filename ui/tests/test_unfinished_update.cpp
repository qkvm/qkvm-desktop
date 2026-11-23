#include <QtTest>

#include <string>
#include <thread>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "../win/unfinished_update.h"

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <sddl.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

// What a start of the program finds beside its own file where an update of the installed copy did not finish
// (packaging/qkvm-desktop.iss, the staged install): a folder of the test's own stands for the program's, with the
// setup's marker, the record in it of what it set aside, and the files it set aside, each its name and the record's
// ending. Nothing is shown and nothing is started: ui/main.cpp says what it finds in a box of Windows' own, which
// tools/test_setup_vm.ps1 sees on a VM.
class TestUnfinishedUpdate : public QObject {
    Q_OBJECT

private:
    // A file of the folder, made with the given bytes (its folders made first).
    static void put(const QString& folder, const QString& name, const QByteArray& bytes = "x")
    {
        const QString path = folder + u'/' + name;
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    static bool there(const QString& folder, const QString& name) { return QFileInfo::exists(folder + u'/' + name); }
    static std::wstring native(const QString& folder) { return QDir::toNativeSeparators(folder).toStdWString(); }
    // A record as the setup writes it (packaging/qkvm-desktop.iss, SetPackageAside): what it is in words, its kind,
    // its ending, one line for each file it renames, the lines given, and its end.
    static QByteArray recordOf(const QByteArray& ending, const QList<QByteArray>& asides,
                               const QList<QByteArray>& more = {})
    {
        QByteArray text = "qKVM Desktop 0.2.0 is being installed here; the version before it is set aside beside "
                          "this file.\r\nqKVM Desktop set-aside record 1\r\nending " +
                          ending + "\r\n";
        for (const QByteArray& name : asides) {
            text += "aside " + name + "\r\n";
        }
        for (const QByteArray& line : more) {
            text += line + "\r\n";
        }
        return text + "end of the record\r\n";
    }
    // The contract's marker of a copy the setup installed (packaging/qkvm-desktop.iss): only there is anything looked
    // at.
    static void installedHere(const QString& folder)
    {
        put(folder, u"qkvm-desktop.installed"_s, QByteArray("setup") + '\x0A');
    }
    static QString marker() { return QString::fromLatin1(ui::unfinishedUpdateMarker); }
    static QString aside(const QString& name, const QString& ending = u".old-update"_s) { return name + ending; }
    // (Under a gate of the test's own name, never the account's: a setup of this account at work would be stopped.)
    std::wstring m_gate;
    int m_made = 0;
    ui::UpdateLook look(const QString& folder) const { return ui::lookForUnfinishedUpdate(native(folder), m_gate); }

    // The gate held as a setup holds it, by another thread (a mutex is its owner thread's: this one would take it
    // again), from `held` until `letGo` is set; or, where `leave`, left by one that ends without letting go of it.
    struct Holder {
        HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE letGo = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread thread;
        ~Holder()
        {
            SetEvent(letGo);
            if (thread.joinable()) {
                thread.join();
            }
            CloseHandle(held);
            CloseHandle(letGo);
        }
    };
    static void holdTheGate(Holder& holder, const std::wstring& gate, bool leave)
    {
        // (The name the test's own member, which outlives the thread.)
        holder.thread = std::thread([&holder, &gate, leave]() noexcept {
            const HANDLE mutex = CreateMutexW(nullptr, TRUE, gate.c_str());
            SetEvent(holder.held);
            if (leave) {
                // (Its handle kept by the test's own, below, so that the gate stays, left by an owner that ended.)
                return;
            }
            WaitForSingleObject(holder.letGo, INFINITE);
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        });
        QCOMPARE(WaitForSingleObject(holder.held, 10'000), DWORD{WAIT_OBJECT_0});
    }
    // A stale marker: its record names a file set aside that is not there.
    void staleIn(const QString& folder)
    {
        installedHere(folder);
        put(folder, marker(), recordOf(".old-update", {"Qt6Core.dll"}));
        put(folder, u"Qt6Core.dll"_s);
    }

private slots:
    void init()
    {
        m_gate = L"Global\\qKVM-Desktop-Setup-Account-test-" + std::to_wstring(GetCurrentProcessId()) + L'-' +
                 std::to_wstring(++m_made);
    }

    void theGateIsTheSetupsOfThisAccount()
    {
        // (tools/test_setup.ps1 holds packaging/qkvm-desktop.iss's AccountGatePrefix to this.)
        QCOMPARE(QString::fromWCharArray(ui::setupAccountGatePrefix), u"Global\\qKVM-Desktop-Setup-Account-"_s);
        HANDLE token = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE);
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> user(size);
        QVERIFY(GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE);
        CloseHandle(token);
        LPWSTR named = nullptr;
        QVERIFY(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &named) != FALSE);
        const std::wstring sid = named;
        LocalFree(named);
        QCOMPARE(QString::fromStdWString(ui::setupAccountGate()),
                 QString::fromWCharArray(ui::setupAccountGatePrefix) + QString::fromStdWString(sid));
    }

    void aStaleMarkerStaysWhileASetupHoldsTheAccountsGate()
    {
        // (Outside review of 0.2.0, 1.4: a setup in another Windows session of the account, between writing its marker
        // and holding it, would otherwise lose it to this start.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        staleIn(folder.path());
        {
            Holder holder;
            holdTheGate(holder, m_gate, false);
            const ui::UpdateLook found = look(folder.path());
            QCOMPARE(found.found, ui::UpdateLeft::StaleMarker);
            QVERIFY(!found.markerRemoved);
            QVERIFY(found.setupAtWork);
            QVERIFY(there(folder.path(), marker()));
        }
        // Once the setup has let go of it, the next start takes the marker away.
        const ui::UpdateLook later = look(folder.path());
        QCOMPARE(later.found, ui::UpdateLeft::StaleMarker);
        QVERIFY(later.markerRemoved);
        QVERIFY(!later.setupAtWork);
        QVERIFY(!there(folder.path(), marker()));
    }

    void aGateLeftByASetupThatEndedIsTaken()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        staleIn(folder.path());
        Holder holder;
        holdTheGate(holder, m_gate, true);
        holder.thread.join();
        // (Kept, so that the gate is there, left by its owner.)
        const HANDLE kept = OpenMutexW(SYNCHRONIZE, FALSE, m_gate.c_str());
        QVERIFY(kept != nullptr);
        const ui::UpdateLook found = look(folder.path());
        CloseHandle(kept);
        QCOMPARE(found.found, ui::UpdateLeft::StaleMarker);
        QVERIFY(found.markerRemoved);
        QVERIFY(!there(folder.path(), marker()));
    }

    void aGateThatCannotBeToldFreeKeepsTheMarker()
    {
        // (One of its name that another made, with a list that lets this account at none of it.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        staleIn(folder.path());
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor,
                                                                     nullptr) != FALSE);
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const HANDLE squatter = CreateMutexW(&attributes, FALSE, m_gate.c_str());
        LocalFree(descriptor);
        QVERIFY(squatter != nullptr);
        const ui::UpdateLook found = look(folder.path());
        CloseHandle(squatter);
        QCOMPARE(found.found, ui::UpdateLeft::StaleMarker);
        QVERIFY(!found.markerRemoved);
        QVERIFY(found.setupAtWork);
        QVERIFY(there(folder.path(), marker()));
    }

    void theNamesAreTheSetups()
    {
        // (tools/test_setup.ps1 holds packaging/qkvm-desktop.iss to these.)
        QCOMPARE(QString::fromLatin1(ui::unfinishedUpdateMarker), u"qkvm-desktop.update-unfinished"_s);
        QCOMPARE(QString::fromLatin1(ui::setAsideEnding), u".old-update"_s);
        QCOMPARE(QString::fromLatin1(ui::setAsideRecordKind), u"qKVM Desktop set-aside record 1"_s);
        QCOMPARE(QString::fromLatin1(ui::setAsideRecordEnd), u"end of the record"_s);
    }

    void theWordsSayWhatToDo()
    {
        // ("Choose OK" first: the box keeps the program's libraries in use while it is open, review i020-1 L6.)
        QCOMPARE(QString::fromWCharArray(ui::unfinishedUpdateWords()),
                 u"An update of qKVM did not finish. Choose OK, then run the qKVM setup again: it puts the "
                 u"earlier version back, or finishes the update."_s);
    }

    void nothingIsLookedAtWithoutTheMarker()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), aside(u"qkvm-desktop.exe"_s));
        put(folder.path(), aside(u"Qt6Core.dll"_s));
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::Nothing);
        // (What a setup set aside is the setup's to put right: never touched here.)
        QVERIFY(there(folder.path(), aside(u"qkvm-desktop.exe"_s)));
        QVERIFY(there(folder.path(), aside(u"Qt6Core.dll"_s)));
    }

    void whatTheRecordSetAsideIsAnUnfinishedUpdate()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), recordOf(".old-update", {"qkvm-desktop.exe", "Qt6Core.dll"}));
        put(folder.path(), aside(u"qkvm-desktop.exe"_s));
        const ui::UpdateLook found = look(folder.path());
        QCOMPARE(found.found, ui::UpdateLeft::Unfinished);
        QCOMPARE(QString::fromStdWString(found.setAside), u"qkvm-desktop.exe"_s);
        // (Nothing changed: the marker stays for the setup that puts it right.)
        QVERIFY(there(folder.path(), marker()));
        QVERIFY(there(folder.path(), aside(u"qkvm-desktop.exe"_s)));
    }

    void aFileInAFolderTheRecordSetAsideToo()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(),
            recordOf(".old-update", {"qkvm-desktop.exe", "Qt6Core.dll", "plugins\\platforms\\qwindows.dll"}));
        put(folder.path(), u"qkvm-desktop.exe"_s);
        put(folder.path(), u"Qt6Core.dll"_s);
        put(folder.path(), aside(u"plugins/platforms/qwindows.dll"_s));
        const ui::UpdateLook found = look(folder.path());
        QCOMPARE(found.found, ui::UpdateLeft::Unfinished);
        QCOMPARE(QString::fromStdWString(found.setAside), u"plugins\\platforms\\qwindows.dll"_s);
        QVERIFY(there(folder.path(), marker()));
    }

    void theRecordsOwnEndingDecides()
    {
        // (A set-aside gives the first ending no file of the package's has beside it: here a file of the first ending
        // was there before, which is not the setup's, and the record says the second.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), recordOf(".2.old-update", {"Qt6Core.dll"}));
        put(folder.path(), u"Qt6Core.dll"_s);
        put(folder.path(), aside(u"Qt6Core.dll"_s));
        const ui::UpdateLook stale = look(folder.path());
        QCOMPARE(stale.found, ui::UpdateLeft::StaleMarker);
        QVERIFY(there(folder.path(), aside(u"Qt6Core.dll"_s)));
        put(folder.path(), marker(), recordOf(".2.old-update", {"Qt6Core.dll"}));
        put(folder.path(), aside(u"Qt6Core.dll"_s, u".2.old-update"_s));
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::Unfinished);
        put(folder.path(), marker(), recordOf(".9.old-update", {"Qt6Core.dll"}));
        put(folder.path(), aside(u"Qt6Core.dll"_s, u".9.old-update"_s));
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::Unfinished);
    }

    void aFileOfThatEndingTheRecordDoesNotNameIsNotLookedAt()
    {
        // (Never by the ending alone: a person's file of that ending, one of a name of the package's that the record
        // does not name, one a later version set aside: never a reason to stay shut, and never touched.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), recordOf(".old-update", {"qkvm-desktop.exe"}));
        put(folder.path(), u"qkvm-desktop.exe"_s);
        put(folder.path(), aside(u"notes.txt"_s));
        put(folder.path(), aside(u"Qt6Core.dll"_s));
        put(folder.path(), aside(u"plugins/platforms/other.dll"_s));
        const ui::UpdateLook found = look(folder.path());
        QCOMPARE(found.found, ui::UpdateLeft::StaleMarker);
        QVERIFY(found.markerRemoved);
        QVERIFY(!there(folder.path(), marker()));
        QVERIFY(there(folder.path(), aside(u"notes.txt"_s)));
        QVERIFY(there(folder.path(), aside(u"Qt6Core.dll"_s)));
        QVERIFY(there(folder.path(), aside(u"plugins/platforms/other.dll"_s)));
    }

    void aMarkerWhoseRecordWasPutRightIsStaleAndTakenAway()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), recordOf(".old-update", {"qkvm-desktop.exe", "Qt6Core.dll"}));
        put(folder.path(), u"Qt6Core.dll"_s);
        put(folder.path(), u"qkvm-desktop.exe"_s);
        const ui::UpdateLook found = look(folder.path());
        QCOMPARE(found.found, ui::UpdateLeft::StaleMarker);
        QVERIFY(found.markerRemoved);
        QVERIFY(!there(folder.path(), marker()));
        // (Only the marker: the program's files are as they were.)
        QVERIFY(there(folder.path(), u"Qt6Core.dll"_s));
        QVERIFY(there(folder.path(), u"qkvm-desktop.exe"_s));
        // And the next start finds nothing.
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::Nothing);
    }

    void aFileOnlyTheCopyMadeStillThereIsAnUnfinishedUpdate()
    {
        // (Outside review of 0.2.0, 1.5: a put-back that could not remove what only the copy made, a plugin of the
        // newer version's, keeps its marker, and the earlier version beside that file is no whole one: never taken for
        // a stale marker, so the record stays for the next setup.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(),
            recordOf(".old-update", {"qkvm-desktop.exe", "Qt6Core.dll"},
                     {"copy qkvm-desktop.exe", "copy QT6CORE.dll", "copy plugins\\platforms\\qwindows.dll"}));
        put(folder.path(), u"Qt6Core.dll"_s);
        put(folder.path(), u"qkvm-desktop.exe"_s);
        put(folder.path(), u"plugins/platforms/qwindows.dll"_s);
        const ui::UpdateLook found = look(folder.path());
        QCOMPARE(found.found, ui::UpdateLeft::Unfinished);
        QCOMPARE(QString::fromStdWString(found.setAside), u"plugins\\platforms\\qwindows.dll"_s);
        QVERIFY(there(folder.path(), marker()));
        // Once it is gone, the marker is stale: what the copy writes and the earlier version had too is no sign.
        QVERIFY(QFile::remove(folder.path() + u"/plugins/platforms/qwindows.dll"_s));
        const ui::UpdateLook stale = look(folder.path());
        QCOMPARE(stale.found, ui::UpdateLeft::StaleMarker);
        QVERIFY(stale.markerRemoved);
    }

    void linesOfAnotherKindAreNotLookedAt()
    {
        // (What an earlier update left, carried in the record for the setup to remove, and lines a later version may
        // add: neither says that this copy is unfinished.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(),
            recordOf(".2.old-update", {"Qt6Core.dll"}, {"left .old-update Qt6Core.dll", "copy Qt6Core.dll"}));
        put(folder.path(), u"Qt6Core.dll"_s);
        put(folder.path(), aside(u"Qt6Core.dll"_s));
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::StaleMarker);
        QVERIFY(there(folder.path(), aside(u"Qt6Core.dll"_s)));
    }

    void aMarkerThatCannotBeTakenAwayKeepsNobodyOut()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), recordOf(".old-update", {"Qt6Core.dll"}));
        const std::wstring path = native(folder.path() + u'/' + marker());
        QVERIFY(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
        const ui::UpdateLook found = look(folder.path());
        QVERIFY(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);
        QCOMPARE(found.found, ui::UpdateLeft::StaleMarker);
        QVERIFY(!found.markerRemoved);
    }

    void aRecordThatDoesNotReadKeepsTheProgramShut_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const QByteArray head = "qKVM Desktop 0.2.0 is being installed here.\r\n";
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("words alone") << head;
        QTest::newRow("no end") << recordOf(".old-update", {"Qt6Core.dll"}).chopped(19);
        QTest::newRow("another kind")
            << QByteArray(recordOf(".old-update", {"Qt6Core.dll"})).replace("set-aside record 1", "set-aside record 2");
        QTest::newRow("no ending")
            << QByteArray(recordOf(".old-update", {"Qt6Core.dll"})).replace("ending .old-update\r\n", "");
        QTest::newRow("two endings") << recordOf(".old-update", {"Qt6Core.dll"}, {"ending .2.old-update"});
        QTest::newRow("an ending of none") << recordOf(".old", {"Qt6Core.dll"});
        QTest::newRow("an ending of ten") << recordOf(".10.old-update", {"Qt6Core.dll"});
        QTest::newRow("an ending of one") << recordOf(".1.old-update", {"Qt6Core.dll"});
        QTest::newRow("an ending of a letter") << recordOf(".a.old-update", {"Qt6Core.dll"});
        QTest::newRow("a name outside") << recordOf(".old-update", {"..\\outside.dll"});
        QTest::newRow("a name of a drive") << recordOf(".old-update", {"C:\\outside.dll"});
        QTest::newRow("a name from the root") << recordOf(".old-update", {"\\outside.dll"});
        QTest::newRow("a name ending in a dot") << recordOf(".old-update", {"plugins.\\x.dll"});
        QTest::newRow("a name ending in a blank") << recordOf(".old-update", {"x.dll "});
        QTest::newRow("a name of a slash") << recordOf(".old-update", {"plugins/x.dll"});
        QTest::newRow("an empty part") << recordOf(".old-update", {"plugins\\\\x.dll"});
        QTest::newRow("a hidden name") << recordOf(".old-update", {".hidden.dll"});
        QTest::newRow("an empty name") << recordOf(".old-update", {""});
        QByteArray large = recordOf(".old-update", {"Qt6Core.dll"});
        large.prepend(QByteArray(qsizetype{1024} * 1024, ' '));
        QTest::newRow("too large") << large;
    }

    void aRecordThatDoesNotReadKeepsTheProgramShut()
    {
        // (The setup writes its record whole before anything is renamed, and renames it into the marker in one step:
        // a record that does not read is not one of this setup's, or a later version's of another kind. Whatever is
        // set aside beside it cannot be told: the setup is the one to look at it, and the program starts nothing.)
        QFETCH(QByteArray, bytes);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), u"Qt6Core.dll"_s);
        put(folder.path(), marker(), bytes);
        const ui::UpdateLook found = look(folder.path());
        QCOMPARE(found.found, ui::UpdateLeft::Unfinished);
        QVERIFY(found.setAside.empty());
        QVERIFY(there(folder.path(), marker()));
    }

    void aMarkerThatIsAFolderKeepsTheProgramShut()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        QVERIFY(QDir(folder.path()).mkdir(marker()));
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::Unfinished);
        QVERIFY(there(folder.path(), marker()));
    }

    void aFolderAtANameSetAsideIsSomethingSetAside()
    {
        // (Anything Windows shows at that name: whatever it is, the setup is the one to look at it.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), recordOf(".old-update", {"Qt6Core.dll"}));
        QVERIFY(QDir(folder.path()).mkdir(aside(u"Qt6Core.dll"_s)));
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::Unfinished);
    }

    void aNameWithBlanksAndSignsAPackageHasIsLookedAt()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), recordOf(".old-update", {"qml\\Qt Quick\\x_y+z-1.0.qmldir"}));
        put(folder.path(), aside(u"qml/Qt Quick/x_y+z-1.0.qmldir"_s));
        const ui::UpdateLook found = look(folder.path());
        QCOMPARE(found.found, ui::UpdateLeft::Unfinished);
        QCOMPARE(QString::fromStdWString(found.setAside), u"qml\\Qt Quick\\x_y+z-1.0.qmldir"_s);
    }

    void aRecordWithLineFeedsAloneReadsAlike()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), QByteArray(recordOf(".old-update", {"Qt6Core.dll"})).replace("\r\n", "\n"));
        put(folder.path(), aside(u"Qt6Core.dll"_s));
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::Unfinished);
    }

    void aRecordWithBlankLinesAfterItsEndReadsAlike()
    {
        // (As the setup's own reader takes it, packaging/qkvm-desktop.iss ReadRecord: blank lines after the last are
        // nothing.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        installedHere(folder.path());
        put(folder.path(), marker(), recordOf(".old-update", {"Qt6Core.dll"}) + "\r\n\r\n");
        put(folder.path(), u"Qt6Core.dll"_s);
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::StaleMarker);
    }

    void aCopyTheSetupDidNotInstallIsNeverLookedAt()
    {
        // (A copy from the ZIP, or one copied out of an installed folder: no setup acts on it, so nothing there is an
        // update of the setup's, and "run the setup again" would be wrong advice; review i020-1, L7.)
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        put(folder.path(), marker(), recordOf(".old-update", {"Qt6Core.dll"}));
        put(folder.path(), aside(u"Qt6Core.dll"_s));
        QCOMPARE(look(folder.path()).found, ui::UpdateLeft::Nothing);
        QVERIFY(there(folder.path(), marker()));
        QVERIFY(there(folder.path(), aside(u"Qt6Core.dll"_s)));
    }

    void theFolderIsTheProgramsOwn()
    {
        QCOMPARE(QDir::fromNativeSeparators(QString::fromStdWString(ui::ownFolder())).toLower(),
                 QDir::fromNativeSeparators(QCoreApplication::applicationDirPath()).toLower());
    }
};

QTEST_GUILESS_MAIN(TestUnfinishedUpdate)
#include "test_unfinished_update.moc"
