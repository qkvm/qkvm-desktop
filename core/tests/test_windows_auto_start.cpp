#include <memory>
#include <string>

#include <QCoreApplication>
#include <QTest>

#include <qkvm/core/auto_start.h>

// Windows' own header.
#include <windows.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

// Windows' registry itself, of whoever runs the test: under keys of the test's own, which nothing
// of Windows reads, and with everything taken out again at the end. The list Windows starts
// programs from is never touched.
class TestWindowsAutoStart : public QObject {
    Q_OBJECT

private:
    QString m_root = u"Software\\QKVM Desktop tests\\%1"_s.arg(QCoreApplication::applicationPid());
    core::StartKeys m_keys{m_root + u"\\Run"_s, m_root + u"\\Switched off"_s};
    QString m_program = u"C:/Program Files/QKVM Desktop/qkvm-desktop.exe"_s;

    std::unique_ptr<core::AutoStart> start(const QString& program = {}) const
    {
        return core::windowsAutoStart(program.isEmpty() ? m_program : program, u"QKVM Desktop"_s, m_keys);
    }

    // What the list holds under a name, as Windows would read it; nothing where there is none.
    QString listed(const QString& name = u"QKVM Desktop"_s) const
    {
        std::wstring held(2048, L'\0');
        auto size = static_cast<DWORD>(held.size() * sizeof(wchar_t));
        if (RegGetValueW(HKEY_CURRENT_USER, m_keys.list.toStdWString().c_str(), name.toStdWString().c_str(),
                         RRF_RT_REG_SZ, nullptr, held.data(), &size) != ERROR_SUCCESS) {
            return {};
        }
        return QString::fromWCharArray(held.c_str());
    }

    void list(const QString& name, const QString& line) const
    {
        const std::wstring text = line.toStdWString();
        QCOMPARE(RegSetKeyValueW(HKEY_CURRENT_USER, m_keys.list.toStdWString().c_str(), name.toStdWString().c_str(),
                                 REG_SZ, text.c_str(), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))),
                 ERROR_SUCCESS);
    }

    // As Windows notes it when somebody switches an entry off or on in its settings.
    void note(const QByteArray& bytes) const
    {
        QCOMPARE(RegSetKeyValueW(HKEY_CURRENT_USER, m_keys.switchedOff.toStdWString().c_str(), L"QKVM Desktop",
                                 REG_BINARY, bytes.constData(), static_cast<DWORD>(bytes.size())),
                 ERROR_SUCCESS);
    }

    bool isNoted() const
    {
        DWORD size = 0;
        return RegGetValueW(HKEY_CURRENT_USER, m_keys.switchedOff.toStdWString().c_str(), L"QKVM Desktop",
                            RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS;
    }

private slots:
    void packagedStartupDoesNotCreateAnUnmanagedRunEntry()
    {
        // This test executable has no package identity: Windows must refuse the task change.
        QVERIFY(core::windowsPackageFamily().isEmpty());
        const auto packaged = core::windowsAutoStart(m_program, u"QKVM Desktop"_s, m_keys, u"test_family"_s);
        QVERIFY(!packaged->set(true).ok());
        QVERIFY(!packaged->isOn());
        QVERIFY(listed().isEmpty());
    }

    void cleanup()
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, m_root.toStdWString().c_str());
        // The folder the tests' keys are in, where no other test has one in it just now.
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\QKVM Desktop tests");
    }

    void theProgramIsNotOnTheListUntilItIsPutThere()
    {
        const std::unique_ptr<core::AutoStart> start = this->start();
        QVERIFY(!start->isOn());
        QCOMPARE(listed(), QString());

        QVERIFY(start->set(true).ok());
        QVERIFY(start->isOn());
        // The place of the program's file as Windows writes one, and in quotes: it has a space in it.
        QCOMPARE(listed(), u"\"C:\\Program Files\\QKVM Desktop\\qkvm-desktop.exe\""_s);
        // And for whoever asks anew: the next run of the program.
        QVERIFY(this->start()->isOn());
        // Put there twice, it is there once and the same.
        QVERIFY(start->set(true).ok());
        QCOMPARE(listed(), u"\"C:\\Program Files\\QKVM Desktop\\qkvm-desktop.exe\""_s);
    }

    void takenOffTheListItIsGoneAndWhatElseIsOnTheListStays()
    {
        list(u"Something else"_s, u"C:\\Windows\\notepad.exe"_s);
        const std::unique_ptr<core::AutoStart> start = this->start();
        QVERIFY(start->set(true).ok());
        QVERIFY(start->set(false).ok());
        QVERIFY(!start->isOn());
        QCOMPARE(listed(), QString());
        QCOMPARE(listed(u"Something else"_s), u"C:\\Windows\\notepad.exe"_s);
        // Taken off where it is not on: nothing.
        QVERIFY(start->set(false).ok());
        QVERIFY(!start->isOn());
    }

    void anEntryForAFileSomewhereElseIsNotThisPrograms()
    {
        // A copy of the program that was somewhere else put itself on the list, and is gone.
        list(u"QKVM Desktop"_s, u"\"D:\\Old\\qkvm-desktop.exe\""_s);
        const std::unique_ptr<core::AutoStart> start = this->start();
        QVERIFY(!start->isOn());
        // Put on the list, this one takes the entry's place.
        QVERIFY(start->set(true).ok());
        QVERIFY(start->isOn());
        QCOMPARE(listed(), u"\"C:\\Program Files\\QKVM Desktop\\qkvm-desktop.exe\""_s);
    }

    void theSamePlaceWrittenWithOtherCapitalsIsThisPrograms()
    {
        QVERIFY(start()->set(true).ok());
        QVERIFY(start(u"c:/program files/qkvm desktop/QKVM-DESKTOP.EXE"_s)->isOn());
        QVERIFY(!start(u"C:/Program Files/QKVM Desktop/qkvm-desktop.exe.bak"_s)->isOn());
        QVERIFY(!start(u"C:/Program Files/QKVM Desktop/qkvm-desktop"_s)->isOn());
    }

    void whatWasSwitchedOffInWindowsSettingsIsOffAndTickingTheBoxSwitchesItOn_data()
    {
        QTest::addColumn<QByteArray>("noted");
        QTest::addColumn<bool>("on");
        // As Windows writes them: a first byte of 2 or 6 where it is on, of 3 or 7 where it is off,
        // and behind it when that was done.
        QTest::newRow("switched on again") << QByteArray::fromHex("020000000000000000000000") << true;
        QTest::newRow("on, another way") << QByteArray::fromHex("060000000000000000000000") << true;
        QTest::newRow("switched off") << QByteArray::fromHex("03000000a1b2c3d4e5f60708") << false;
        QTest::newRow("off, another way") << QByteArray::fromHex("07000000a1b2c3d4e5f60708") << false;
    }

    void whatWasSwitchedOffInWindowsSettingsIsOffAndTickingTheBoxSwitchesItOn()
    {
        QFETCH(QByteArray, noted);
        QFETCH(bool, on);
        const std::unique_ptr<core::AutoStart> start = this->start();
        QVERIFY(start->set(true).ok());
        note(noted);
        QCOMPARE(start->isOn(), on);
        // Whoever ticks the box means it: what Windows noted is taken away, and with it the "off".
        QVERIFY(start->set(true).ok());
        QVERIFY(start->isOn());
        QVERIFY(!isNoted());
    }

    void takenOffTheListNothingIsLeftNotedEither()
    {
        const std::unique_ptr<core::AutoStart> start = this->start();
        QVERIFY(start->set(true).ok());
        note(QByteArray::fromHex("03000000a1b2c3d4e5f60708"));
        QVERIFY(start->set(false).ok());
        QCOMPARE(listed(), QString());
        QVERIFY(!isNoted());
    }

    void whatIsOnTheListAndIsNoLineIsNotThisPrograms()
    {
        // Bytes under the program's name, and the very ones its line would be: somebody else's
        // doing, and nothing Windows starts a program from.
        const std::wstring line = L"\"C:\\Program Files\\QKVM Desktop\\qkvm-desktop.exe\"";
        QCOMPARE(RegSetKeyValueW(HKEY_CURRENT_USER, m_keys.list.toStdWString().c_str(), L"QKVM Desktop", REG_BINARY,
                                 line.c_str(), static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t))),
                 ERROR_SUCCESS);
        QVERIFY(!start()->isOn());
    }

    void whatIsNotedBesideTheListAndIsNoNoteOfWindowsSwitchesNothingOff()
    {
        const std::unique_ptr<core::AutoStart> start = this->start();
        QVERIFY(start->set(true).ok());
        // A text where Windows notes bytes, and one that begins with an odd byte at that.
        QCOMPARE(RegSetKeyValueW(HKEY_CURRENT_USER, m_keys.switchedOff.toStdWString().c_str(), L"QKVM Desktop", REG_SZ,
                                 L"3", static_cast<DWORD>(2 * sizeof(wchar_t))),
                 ERROR_SUCCESS);
        QVERIFY(start->isOn());
    }

    void whereWindowsDoesNotTakeItThatIsSaid()
    {
        // No key of the registry has a name this long.
        core::StartKeys keys = m_keys;
        keys.list = m_root + u'\\' + QString(300, u'x');
        const std::unique_ptr<core::AutoStart> start = core::windowsAutoStart(m_program, u"QKVM Desktop"_s, keys);
        const base::Result<void> put = start->set(true);
        QVERIFY(!put.ok());
        QCOMPARE(put.error().code, std::string("auto_start"));
        QVERIFY(!start->isOn());
    }
};

QTEST_GUILESS_MAIN(TestWindowsAutoStart)
#include "test_windows_auto_start.moc"
