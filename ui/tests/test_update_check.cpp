#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "../win/unfinished_update.h"

// Windows' own header.
#include <windows.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

// qKVM Setup's update check (ui/win/update_check.cpp), which the setup puts at the program's own name while the earlier
// version is set aside for its copy (packaging/qkvm-desktop.iss): a program of Windows' own calls and a static C
// runtime alone, so that it runs where nothing else of qKVM is. Each start of it here is on a desktop of this test's
// own, which nobody sees: a box it shows is read there and answered there, and nothing is ever shown on the person's
// screen.
class TestUpdateCheck : public QObject {
    Q_OBJECT

private:
    static QString program() { return QCoreApplication::applicationDirPath() + u"/qkvm-update-check.exe"_s; }

    // The names of the libraries a program file asks Windows for at its start, read from its import table.
    static QSet<QString> importsOf(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        const QByteArray image = file.readAll();
        const auto within = [&image](qsizetype offset, qsizetype size) {
            return offset >= 0 && size >= 0 && offset + size <= image.size();
        };
        IMAGE_DOS_HEADER dos{};
        if (!within(0, sizeof dos)) {
            return {};
        }
        std::memcpy(&dos, image.constData(), sizeof dos);
        IMAGE_NT_HEADERS64 nt{};
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || !within(dos.e_lfanew, sizeof nt)) {
            return {};
        }
        std::memcpy(&nt, image.constData() + dos.e_lfanew, sizeof nt);
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return {};
        }
        const qsizetype sections =
            dos.e_lfanew + qsizetype{offsetof(IMAGE_NT_HEADERS64, OptionalHeader)} + nt.FileHeader.SizeOfOptionalHeader;
        // (A place in the image, by its address once loaded, as a place in the file.)
        const auto inFile = [&](DWORD address) -> qsizetype {
            for (WORD index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
                IMAGE_SECTION_HEADER section{};
                const qsizetype where = sections + (qsizetype{index} * qsizetype{sizeof section});
                if (!within(where, sizeof section)) {
                    return -1;
                }
                std::memcpy(&section, image.constData() + where, sizeof section);
                if (address >= section.VirtualAddress && address < section.VirtualAddress + section.SizeOfRawData) {
                    return qsizetype{section.PointerToRawData} + (address - section.VirtualAddress);
                }
            }
            return -1;
        };
        const IMAGE_DATA_DIRECTORY imports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        QSet<QString> names;
        for (qsizetype where = inFile(imports.VirtualAddress); where >= 0;
             where += qsizetype{sizeof(IMAGE_IMPORT_DESCRIPTOR)}) {
            IMAGE_IMPORT_DESCRIPTOR descriptor{};
            if (!within(where, sizeof descriptor)) {
                return {};
            }
            std::memcpy(&descriptor, image.constData() + where, sizeof descriptor);
            if (descriptor.Name == 0) {
                break;
            }
            const qsizetype name = inFile(descriptor.Name);
            if (name < 0 || !within(name, 1)) {
                return {};
            }
            names.insert(QString::fromLatin1(image.constData() + name).toLower());
        }
        return names;
    }

    struct Seen {
        bool started = false;
        DWORD ended = STILL_ACTIVE;
        // The top-level windows it showed: each one's title, and the texts in it.
        QStringList titles;
        QStringList texts;
        bool answered = false;
    };

    static BOOL CALLBACK textInside(HWND window, LPARAM into)
    {
        wchar_t text[1024] = {};
        if (GetWindowTextW(window, text, 1024) > 0) {
            reinterpret_cast<QStringList*>(into)->append(QString::fromWCharArray(text));
        }
        return TRUE;
    }

    static BOOL CALLBACK buttonInside(HWND window, LPARAM into)
    {
        wchar_t kind[64] = {};
        GetClassNameW(window, kind, 64);
        if (std::wstring(kind) == L"Button") {
            reinterpret_cast<QList<HWND>*>(into)->append(window);
        }
        return TRUE;
    }

    struct Looking {
        DWORD process = 0;
        QList<HWND> windows;
    };

    // The windows Windows' own text input puts into a process that has a window of its own: its input indicator,
    // which it shows on a PC with a touch screen (two visible windows without a title), and the hidden ones of its
    // input methods and of its text services (Cicero). Windows', not the program's: left out of what it is seen to
    // show. Every other window of its process still counts, as before. (Seen on the ARM test PC, which has a touch
    // screen, 2026-11-19: the update check's box, and beside it UAC_InputIndicatorOverlayWnd and UAC Input Indicator,
    // visible, which the test took for a second window of the program's.)
    static bool isWindowsTextInput(HWND window)
    {
        static constexpr std::array<std::wstring_view, 7> classes{L"UAC_InputIndicatorOverlayWnd",
                                                                  L"UAC Input Indicator",
                                                                  L"Touch Tooltip Window",
                                                                  L"CicLoaderWndClass",
                                                                  L"CiceroUIWndFrame",
                                                                  L"MSCTFIME UI",
                                                                  L"IME"};
        wchar_t kind[128] = {};
        const int length = GetClassNameW(window, kind, 128);
        return length > 0 &&
               std::ranges::find(classes, std::wstring_view(kind, static_cast<std::size_t>(length))) != classes.end();
    }

    static BOOL CALLBACK windowOf(HWND window, LPARAM into)
    {
        auto* looking = reinterpret_cast<Looking*>(into);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == looking->process) {
            looking->windows.append(window);
        }
        return TRUE;
    }

    // The update check started on a desktop of this test's own, with or without a setup's mutex held meanwhile; its
    // windows there looked at for up to 10 s (or until it ends), each box's OK pressed once it is read, and the program
    // ended by force where it has not ended 10 s after that.
    static Seen runHidden(bool setupAtWork)
    {
        Seen seen;
        HDESK const desktop = CreateDesktopW(L"qkvm-update-check-test", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (desktop == nullptr) {
            return seen;
        }
        HANDLE const setup = setupAtWork ? CreateMutexW(nullptr, FALSE, L"Local\\qKVM-Desktop-Setup") : nullptr;
        std::wstring line = L"\"" + QDir::toNativeSeparators(program()).toStdWString() + L"\"";
        std::wstring where = L"qkvm-update-check-test";
        STARTUPINFOW start{};
        start.cb = sizeof start;
        start.lpDesktop = where.data();
        PROCESS_INFORMATION started{};
        seen.started =
            (setup != nullptr || !setupAtWork) && CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0,
                                                                 nullptr, nullptr, &start, &started) != FALSE;
        if (seen.started) {
            QList<HWND> read;
            for (int look = 0; look < 100 && WaitForSingleObject(started.hProcess, 100) == WAIT_TIMEOUT; ++look) {
                Looking looking{started.dwProcessId, {}};
                EnumDesktopWindows(desktop, &windowOf, reinterpret_cast<LPARAM>(&looking));
                for (HWND window : looking.windows) {
                    if (!IsWindowVisible(window) || read.contains(window) || isWindowsTextInput(window)) {
                        continue;
                    }
                    read.append(window);
                    wchar_t title[256] = {};
                    GetWindowTextW(window, title, 256);
                    seen.titles.append(QString::fromWCharArray(title));
                    EnumChildWindows(window, &textInside, reinterpret_cast<LPARAM>(&seen.texts));
                    // (Its one button, OK, pressed as a person presses it.)
                    QList<HWND> buttons;
                    EnumChildWindows(window, &buttonInside, reinterpret_cast<LPARAM>(&buttons));
                    if (buttons.size() == 1) {
                        PostMessageW(buttons.front(), BM_CLICK, 0, 0);
                        seen.answered = true;
                    }
                }
                if (seen.answered) {
                    break;
                }
            }
            if (WaitForSingleObject(started.hProcess, 10000) != WAIT_OBJECT_0) {
                TerminateProcess(started.hProcess, 99);
                WaitForSingleObject(started.hProcess, 5000);
            }
            GetExitCodeProcess(started.hProcess, &seen.ended);
            CloseHandle(started.hThread);
            CloseHandle(started.hProcess);
        }
        if (setup != nullptr) {
            CloseHandle(setup);
        }
        CloseDesktop(desktop);
        return seen;
    }

private slots:
    void itAsksWindowsForNothingOfQKvmsOwn()
    {
        // (No library of the package's, none of Qt's, no C runtime's of its own: Windows' kernel and its windows. An
        // address sanitizer's build adds that sanitizer's runtime alone.)
        const QSet<QString> imports = importsOf(program());
        QVERIFY2(!imports.isEmpty(), qPrintable(program()));
        QVERIFY(imports.contains(u"kernel32.dll"_s));
        QVERIFY(imports.contains(u"user32.dll"_s));
        for (const QString& name : imports) {
            QVERIFY2(name == u"kernel32.dll"_s || name == u"user32.dll"_s || name.startsWith(u"clang_rt.asan"_s),
                     qPrintable(name));
        }
    }

    void itEndsAtOnceSayingNothingWhileASetupIsAtWork()
    {
        const Seen seen = runHidden(true);
        QVERIFY(seen.started);
        QVERIFY2(seen.titles.isEmpty(), qPrintable(seen.titles.join(u" | "_s)));
        QCOMPARE(seen.ended, DWORD{0});
    }

    void itSaysThatAnUpdateDidNotFinishWhereNoSetupIsAtWork()
    {
        const Seen seen = runHidden(false);
        QVERIFY(seen.started);
        QCOMPARE(seen.titles, QStringList{u"qKVM"_s});
        QVERIFY2(seen.texts.contains(QString::fromWCharArray(ui::unfinishedUpdateText)),
                 qPrintable(seen.texts.join(u" | "_s)));
        QVERIFY(seen.answered);
        QCOMPARE(seen.ended, DWORD{1});
    }

    void itSaysWhatTheProgramSays()
    {
        // (One text for both: the program's box where a copy was whole but not finished, and the update check's.)
        QCOMPARE(QString::fromWCharArray(ui::unfinishedUpdateWords()),
                 QString::fromWCharArray(ui::unfinishedUpdateText));
        QCOMPARE(QString::fromWCharArray(ui::unfinishedUpdateText),
                 u"An update of qKVM did not finish. Choose OK, then run the qKVM setup again: it puts the earlier "
                 u"version back, or finishes the update."_s);
    }
};

QTEST_GUILESS_MAIN(TestUpdateCheck)
#include "test_update_check.moc"
