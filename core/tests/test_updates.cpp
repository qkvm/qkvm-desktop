#include <algorithm>
#include <array>
#include <vector>

#include <QDirIterator>
#include <QNetworkProxy>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/data_folder.h>
#include <qkvm/core/update_cache.h>
#include <qkvm/core/updates.h>

#include "update_by_hand.h"

#include <aclapi.h>
#include <sddl.h>

using namespace Qt::StringLiterals;
using namespace qkvm::core;
using namespace qkvm::core::testing;

namespace {
// The stand-in that a Setup copy's updater runs in these tests, in Setup's place (stand_in_setup.cpp): its bytes.
QByteArray standInSetup()
{
    QFile program(QCoreApplication::applicationDirPath() + u"/qkvm-test-stand-in-setup.exe"_s);
    return program.open(QIODevice::ReadOnly) ? program.readAll() : QByteArray();
}

struct Desk {
    explicit Desk(UpdateKind made = UpdateKind::Portable) : kind(made) {}
    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;
    // A Setup that is still running ends first, before its folders go.
    ~Desk()
    {
        for (int waited = 0; waited < 200 && setupStillRuns(); ++waited) {
            QTest::qWait(50);
        }
    }
    UpdateKind kind;
    QTemporaryDir folder;
    Settings settings{folder.filePath(u"settings.json"_s)};
    ReleaseSigner signer;
    UpdateNetwork network;
    QByteArray package{"a made-up package, never executed"};
    // The program runs from "program" (a Setup copy's holds Setup's marker), and keeps its data in "data".
    UpdateChannel channel(const QString& version) const
    {
        return {QUrl(u"https://updates.example.test/stable.json"_s),
                signer.publicKey(),
                version,
                kind,
                folder.filePath(u"program"_s),
                folder.filePath(u"data"_s)};
    }
    Updates updates{channel(u"0.1.0"_s), settings, network, folder.filePath(u"downloads"_s)};
    void check(const QJsonObject& release = {})
    {
        network.next = {
            signer.seal(release.isEmpty() ? releaseFor(package, QDateTime::currentDateTimeUtc(), kind) : release)};
        updates.check();
    }
    void download()
    {
        network.next = {package};
        updates.download();
    }
    QString program() const { return folder.filePath(u"program"_s); }
    // A Setup copy, its marker beside the program, its release (the stand-in) downloaded and verified.
    bool readyToRun()
    {
        package = standInSetup();
        QFile marker(program() + u'/' + QString::fromLatin1(setupMarkerName));
        if (package.isEmpty() || !QDir().mkpath(program()) || !marker.open(QIODevice::WriteOnly) ||
            marker.write(setupMarkerContent) != 6) {
            return false;
        }
        marker.close();
        check();
        if (!QTest::qWaitFor([this] { return updates.state() == Updates::State::Available; }, 5000)) {
            return false;
        }
        download();
        return QTest::qWaitFor([this] { return updates.state() == Updates::State::Ready; }, 5000);
    }
    QString downloaded() const
    {
        QDirIterator files(folder.filePath(u"downloads"_s), {u"qkvm-desktop-setup.exe"_s}, QDir::Files,
                           QDirIterator::Subdirectories);
        return files.hasNext() ? files.next() : QString();
    }
    QString packageHash() const
    {
        return QString::fromLatin1(QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex());
    }
    // What the stand-in wrote in Setup's log once it ran (its command line, its file's hash, its file's name, its TEMP
    // and its TMP), waited for; nothing where it did not run.
    QStringList whatSetupWrote() const
    {
        QFile log(folder.filePath(u"data/updates/setup-0.2.0.log"_s));
        for (int waited = 0; waited < 300; ++waited) {
            if (log.open(QIODevice::ReadOnly)) {
                const QString said = QString::fromUtf8(log.readAll());
                log.close();
                QStringList lines = said.split(u'\n');
                if (lines.size() == 6 && lines.last().isEmpty()) {
                    lines.removeLast();
                    return lines;
                }
            }
            QTest::qWait(50);
        }
        return {};
    }
    QJsonObject handOver() const
    {
        QFile note(folder.filePath(u"data/updates/setup-handover.json"_s));
        const auto said =
            note.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(note.readAll()).object() : QJsonObject();
        if (said.value(u"process"_s).toInteger(0) > 0) {
            started = said.value(u"process"_s).toInteger(0);
        }
        return said;
    }
    // The Setup that the hand-over named, once it was read.
    mutable qint64 started = 0;
    // Whether the Setup that the hand-over names (or named) is still running.
    bool setupStillRuns() const
    {
        handOver();
        const qint64 process = started;
        if (process <= 0) {
            return false;
        }
        const HANDLE setup = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process));
        if (setup == nullptr) {
            return false;
        }
        const bool runs = WaitForSingleObject(setup, 0) == WAIT_TIMEOUT;
        CloseHandle(setup);
        return runs;
    }
};

// An environment variable of this program's, set while it lives (a program it starts has it too).
class Environment {
public:
    Environment(QString name, const QString& value) : m_name(std::move(name))
    {
        qputenv(qPrintable(m_name), value.toUtf8());
    }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    ~Environment() { qunsetenv(qPrintable(m_name)); }

private:
    QString m_name;
};

// A junction at `link` to the folder `target`, made by Windows' own PowerShell.
bool junction(const QString& link, const QString& target)
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"QKVM_TEST_JUNCTION"_s, link);
    environment.insert(u"QKVM_TEST_TARGET"_s, target);
    process.setProcessEnvironment(environment);
    process.start(
        qEnvironmentVariable("SystemRoot") + u"/System32/WindowsPowerShell/v1.0/powershell.exe"_s,
        {u"-NoProfile"_s, u"-NonInteractive"_s, u"-Command"_s,
         u"$ErrorActionPreference='Stop'; New-Item -ItemType Junction -Path $env:QKVM_TEST_JUNCTION -Target $env:QKVM_TEST_TARGET | Out-Null"_s});
    return process.waitForFinished(15000) && process.exitCode() == 0 && QFileInfo(link).isJunction();
}

// Whether a folder's list of who may do what is its own (taking nothing from above), and gives all of it to this user,
// SYSTEM and the administrators, and nothing to anybody else.
bool keptToThisUserAlone(const QString& folder)
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    const bool told = size > 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE;
    CloseHandle(token);
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system{};
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD systemSize = static_cast<DWORD>(system.size());
    DWORD administratorsSize = static_cast<DWORD>(administrators.size());
    if (!told || CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &systemSize) == FALSE ||
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &administratorsSize) == FALSE) {
        return false;
    }
    PACL entries = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto path = QDir::toNativeSeparators(folder).toStdWString();
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &entries,
                              nullptr, &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool kept = GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE &&
                (control & SE_DACL_PROTECTED) != 0 && entries != nullptr && entries->AceCount == 3;
    std::array<bool, 3> seen{};
    for (DWORD index = 0; kept && index < entries->AceCount; ++index) {
        void* entry = nullptr;
        kept = GetAce(entries, index, &entry) != FALSE &&
               static_cast<ACE_HEADER*>(entry)->AceType == ACCESS_ALLOWED_ACE_TYPE;
        if (kept) {
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            PSID sid = &allowed->SidStart;
            const std::array<PSID, 3> whom{reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, system.data(),
                                           administrators.data()};
            const auto at = std::ranges::find_if(whom, [&](PSID one) { return EqualSid(sid, one) != FALSE; });
            kept = at != whom.end() && (allowed->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
            if (kept) {
                seen.at(static_cast<std::size_t>(at - whom.begin())) = true;
            }
        }
    }
    LocalFree(descriptor);
    return kept && std::ranges::all_of(seen, [](bool one) { return one; });
}

// Gives a folder the list a Setup's download folder has: this user, SYSTEM and the administrators, its own.
bool keepToThisUserAsSetupsIs(const QString& folder)
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    LPWSTR text = nullptr;
    const bool told = size > 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE &&
                      ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &text) != FALSE;
    CloseHandle(token);
    if (!told) {
        return false;
    }
    const std::wstring list = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(text) + L")";
    LocalFree(text);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(list.c_str(), SDDL_REVISION_1, &descriptor, nullptr) ==
        FALSE) {
        return false;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL entries = nullptr;
    std::wstring path = QDir::toNativeSeparators(folder).toStdWString();
    const bool kept = GetSecurityDescriptorDacl(descriptor, &present, &entries, &defaulted) != FALSE &&
                      SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                            nullptr, entries, nullptr) == ERROR_SUCCESS;
    LocalFree(descriptor);
    return kept;
}

// Denies this user `what` of a file or folder while it lives (an entry that denies, first in its list); what was there
// before is put back after.
class DenyThisUser {
public:
    DenyThisUser(const QString& path, DWORD what) : m_path(QDir::toNativeSeparators(path).toStdWString())
    {
        PACL entries = nullptr;
        HANDLE token = nullptr;
        if (GetNamedSecurityInfoW(m_path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &entries,
                                  nullptr, &m_kept) != ERROR_SUCCESS ||
            OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
            return;
        }
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> user(size);
        const bool told = size > 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE;
        CloseHandle(token);
        EXPLICIT_ACCESS_W deny{};
        deny.grfAccessPermissions = what;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.ptstrName =
            told ? static_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) : nullptr;
        PACL denied = nullptr;
        m_ok = told && SetEntriesInAclW(1, &deny, entries, &denied) == ERROR_SUCCESS &&
               SetNamedSecurityInfoW(m_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, denied,
                                     nullptr) == ERROR_SUCCESS;
        LocalFree(denied);
    }
    DenyThisUser(const DenyThisUser&) = delete;
    DenyThisUser& operator=(const DenyThisUser&) = delete;
    DenyThisUser(DenyThisUser&&) = delete;
    DenyThisUser& operator=(DenyThisUser&&) = delete;
    ~DenyThisUser()
    {
        if (m_kept != nullptr) {
            BOOL present = FALSE;
            BOOL defaulted = FALSE;
            PACL entries = nullptr;
            if (GetSecurityDescriptorDacl(m_kept, &present, &entries, &defaulted) != FALSE) {
                SetNamedSecurityInfoW(m_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                      entries, nullptr);
            }
            LocalFree(m_kept);
        }
    }
    bool ok() const { return m_ok; }

private:
    std::wstring m_path;
    PSECURITY_DESCRIPTOR m_kept = nullptr;
    bool m_ok = false;
};

// Lets `who` (a SID as text) also do `may` to a folder.
bool letAlsoChange(const QString& folder, const QString& who, DWORD may)
{
    PSID sid = nullptr;
    if (ConvertStringSidToSidW(who.toStdWString().c_str(), &sid) == FALSE) {
        return false;
    }
    std::wstring path = QDir::toNativeSeparators(folder).toStdWString();
    PACL entries = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL widened = nullptr;
    EXPLICIT_ACCESS_W also{};
    also.grfAccessPermissions = may;
    also.grfAccessMode = GRANT_ACCESS;
    also.grfInheritance = NO_INHERITANCE;
    also.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    also.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    const bool let = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                           &entries, nullptr, &descriptor) == ERROR_SUCCESS &&
                     SetEntriesInAclW(1, &also, entries, &widened) == ERROR_SUCCESS &&
                     SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                           nullptr, widened, nullptr) == ERROR_SUCCESS;
    LocalFree(widened);
    LocalFree(descriptor);
    LocalFree(sid);
    return let;
}

// Makes a file or a folder look as if it was last changed `days` ago (Windows' own call: Qt sets no folder's time).
// (`itself`: a junction or link, not what it leads to.)
bool ageBy(const QString& path, int days, bool itself = false)
{
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE item = CreateFileW(native.c_str(), FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | (itself ? FILE_FLAG_OPEN_REPARSE_POINT : 0), nullptr);
    if (item == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER when{};
    when.LowPart = now.dwLowDateTime;
    when.HighPart = now.dwHighDateTime;
    when.QuadPart -= static_cast<ULONGLONG>(days) * 24ULL * 3600ULL * 10000000ULL;
    const FILETIME then{when.LowPart, when.HighPart};
    const bool set = SetFileTime(item, nullptr, nullptr, &then) != FALSE;
    CloseHandle(item);
    return set;
}

// A folder of the download cache with one file in it, as old as asked; its name, or nothing where it was not made.
QString aDownloadIn(const QString& downloads, const QString& folder, const QString& file, int daysOld)
{
    const QString place = downloads + u'/' + folder;
    if (!QDir().mkpath(place)) {
        return {};
    }
    QFile made(place + u'/' + file);
    if (!made.open(QIODevice::WriteOnly) || made.write("test") != 4) {
        return {};
    }
    made.close();
    return daysOld == 0 || (ageBy(made.fileName(), daysOld) && ageBy(place, daysOld)) ? place : QString();
}
} // namespace

class TestUpdates : public QObject {
    Q_OBJECT
private slots:
    void transferCleanupNeverFollowsDirectoryJunctions_data()
    {
        QTest::addColumn<bool>("rootIsJunction");
        QTest::newRow("redirected cache root") << true;
        QTest::newRow("redirected transfer folder") << false;
    }

    void transferCleanupNeverFollowsDirectoryJunctions()
    {
        QFETCH(bool, rootIsJunction);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString cache = folder.filePath(u"cache"_s);
        const QString held = folder.filePath(u"held"_s);
        const QString target = rootIsJunction ? held + u"/update-Ab1234"_s : held;
        QVERIFY(QDir().mkpath(target));
        if (!rootIsJunction)
            QVERIFY(QDir().mkpath(cache));
        const QString link = rootIsJunction ? cache : cache + u"/update-Ab1234"_s;
        const QString kept = target + u"/qkvm.msix"_s;
        QFile file(kept);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("disposable test data"), 20);
        file.close();
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"QKVM_TEST_JUNCTION"_s, link);
        environment.insert(u"QKVM_TEST_TARGET"_s, held);
        process.setProcessEnvironment(environment);
        process.start(
            qEnvironmentVariable("SystemRoot") + u"/System32/WindowsPowerShell/v1.0/powershell.exe"_s,
            {u"-NoProfile"_s, u"-NonInteractive"_s, u"-Command"_s,
             u"$ErrorActionPreference='Stop'; New-Item -ItemType Junction -Path $env:QKVM_TEST_JUNCTION -Target $env:QKVM_TEST_TARGET | Out-Null"_s});
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(QFileInfo(link).isJunction());
        const int removed =
            pruneUpdateCache(cache, u"update-"_s, u"qkvm.msix"_s, QDateTime::currentDateTimeUtc().addDays(8));
        QVERIFY2(QFileInfo::exists(kept), "Cleanup followed a redirected directory and deleted its target file.");
        QCOMPARE(removed, 0);
    }

    void oldTransferCleanupPreservesRecentAndUnrecognizedFiles()
    {
        QTemporaryDir folder;
        const auto make = [&](const QString& directory, const QString& fileName) {
            QVERIFY(QDir(folder.path()).mkpath(directory));
            QFile file(folder.filePath(directory + u'/' + fileName));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("test"), 4);
        };
        make(u"update-Ab1234"_s, u"qkvm.msix"_s);
        make(u"update-Cd1234"_s, u"qkvm.msix"_s);
        make(u"update-Cd1234"_s, u"settings.json"_s);
        make(u"update-Ef1234"_s, u"qkvm.zip"_s);
        make(u"update-Gh1234"_s, u"qkvm.zip"_s);
        make(u"update-Gh1234"_s, u"qkvm"_s);
        make(u"update-Ij1234"_s, u"other.zip"_s);
        make(u"unrelated"_s, u"qkvm.msix"_s);
        const auto now = QDateTime::currentDateTimeUtc();
        QCOMPARE(pruneUpdateCache(folder.path(), u"update-"_s, u"qkvm.msix"_s, now), 0);
        QCOMPARE(pruneUpdateCache(folder.path(), u"update-"_s, u"qkvm.zip"_s, now), 0);
        QCOMPARE(pruneUpdateCache(folder.path(), u"update-"_s, u"qkvm.msix"_s, now.addDays(8)), 1);
        QVERIFY(!QFileInfo::exists(folder.filePath(u"update-Ab1234"_s)));
        QVERIFY(QFileInfo::exists(folder.filePath(u"update-Cd1234/settings.json"_s)));
        QVERIFY(QFileInfo::exists(folder.filePath(u"unrelated/qkvm.msix"_s)));
        QVERIFY(QFileInfo::exists(folder.filePath(u"update-Ef1234/qkvm.zip"_s)));
        // A portable copy's verified file, shown and left for the person: taken the same way, after the same time.
        QCOMPARE(pruneUpdateCache(folder.path(), u"update-"_s, u"qkvm.zip"_s, now.addDays(8)), 1);
        QVERIFY(!QFileInfo::exists(folder.filePath(u"update-Ef1234"_s)));
        // Where it was unpacked beside itself, or a name that is none of the program's, nothing is taken.
        QVERIFY(QFileInfo::exists(folder.filePath(u"update-Gh1234/qkvm.zip"_s)));
        QCOMPARE(pruneUpdateCache(folder.path(), u"update-"_s, u"other.zip"_s, now.addDays(8)), 0);
        QVERIFY(QFileInfo::exists(folder.filePath(u"update-Ij1234/other.zip"_s)));
        QCOMPARE(pruneUpdateCache(folder.path(), u"unrelated"_s, u"qkvm.msix"_s, now.addDays(8)), 0);
    }

    void oldDownloadsOfEitherKindAreClearedWhenTheProgramStarts()
    {
        QTemporaryDir folder;
        const QString downloads = folder.filePath(u"downloads"_s);
        const QString oldPackage = aDownloadIn(downloads, u"update-Old111"_s, u"qkvm.msix"_s, 10);
        const QString oldZip = aDownloadIn(downloads, u"update-Old222"_s, u"qkvm.zip"_s, 10);
        const QString newZip = aDownloadIn(downloads, u"update-New333"_s, u"qkvm.zip"_s, 0);
        // A Setup that was run from the cache, a week and more ago (it ended long since), and one of today.
        const QString oldSetup = aDownloadIn(downloads, u"update-Old444"_s, u"qkvm-desktop-setup.exe"_s, 10);
        const QString newSetup = aDownloadIn(downloads, u"update-New555"_s, u"qkvm-desktop-setup.exe"_s, 0);
        QVERIFY(!oldPackage.isEmpty() && !oldZip.isEmpty() && !newZip.isEmpty());
        QVERIFY(!oldSetup.isEmpty() && !newSetup.isEmpty());
        Settings settings(folder.filePath(u"settings.json"_s));
        UpdateNetwork network;
        const Updates updates({{}, {}, u"0.1.0"_s}, settings, network, downloads);
        QVERIFY(!QFileInfo::exists(oldPackage));
        QVERIFY(!QFileInfo::exists(oldZip));
        QVERIFY(QFileInfo::exists(newZip + u"/qkvm.zip"_s));
        QVERIFY(!QFileInfo::exists(oldSetup));
        QVERIFY(QFileInfo::exists(newSetup + u"/qkvm-desktop-setup.exe"_s));
    }

    void unconfiguredBuildDoesNotClaimCurrentOrContactAnything()
    {
        Desk desk;
        Updates unavailable({{}, {}, u"0.1.0"_s}, desk.settings, desk.network, desk.folder.path());
        unavailable.check();
        unavailable.checkIfDue(QDateTime::currentDateTimeUtc());
        unavailable.download();
        QCOMPARE(unavailable.state(), Updates::State::Unavailable);
        QVERIFY(desk.network.requests.isEmpty());
    }

    void signatureCoversTheExactPayloadAndTrustComesFromTheProgram()
    {
        Desk desk;
        const auto now = QDateTime::currentDateTimeUtc();
        const auto payload = releaseFor(desk.package, now);
        const auto envelope = desk.signer.seal(payload);
        QVERIFY(verifiedUpdate(envelope, desk.signer.publicKey(), now, UpdateKind::Portable));
        const ReleaseSigner stranger;
        QVERIFY(!verifiedUpdate(envelope, stranger.publicKey(), now, UpdateKind::Portable));
        auto tampered = QJsonDocument::fromJson(envelope).object();
        auto changed = payload;
        changed[u"version"_s] = u"99.0.0"_s;
        tampered[u"payload"_s] = QString::fromLatin1(QJsonDocument(changed).toJson().toBase64());
        QVERIFY(!verifiedUpdate(QJsonDocument(tampered).toJson(), desk.signer.publicKey(), now, UpdateKind::Portable));
        QVERIFY(!verifiedUpdate(envelope, QByteArray(64, '\0'), now, UpdateKind::Portable));
        QVERIFY(!verifiedUpdate(QByteArray(mostUpdateMetadata + 1, 'x'), desk.signer.publicKey(), now,
                                UpdateKind::Portable));
    }

    // The signed payload names the kind of copy it is for, and its package ends in that kind; a copy takes only its
    // own. (Before: one platform, "windows-x64", and an MSIX only, for every copy.)
    void eachKindOfCopyTakesOnlyItsOwnKindOfRelease_data()
    {
        QTest::addColumn<UpdateKind>("copy");
        QTest::addColumn<QString>("platform");
        QTest::addColumn<QString>("package");
        QTest::addColumn<bool>("taken");
        const auto portable = UpdateKind::Portable;
        const auto msix = UpdateKind::Msix;
        const QString at = u"https://updates.example.test/"_s;
        const QString portableName = u"windows-x64-portable"_s;
        const QString msixName = u"windows-x64-msix"_s;
        QTest::newRow("portable takes a zip") << portable << portableName << at + u"q-0.2.0.zip"_s << true;
        QTest::newRow("portable, zip in capitals") << portable << portableName << at + u"q-0.2.0.ZIP"_s << true;
        QTest::newRow("portable, msix platform") << portable << msixName << at + u"q-0.2.0.zip"_s << false;
        QTest::newRow("portable, msix file") << portable << portableName << at + u"q-0.2.0.msix"_s << false;
        QTest::newRow("portable, msix release") << portable << msixName << at + u"q-0.2.0.msix"_s << false;
        QTest::newRow("portable, old platform") << portable << u"windows-x64"_s << at + u"q-0.2.0.zip"_s << false;
        QTest::newRow("portable, an exe") << portable << portableName << at + u"q-0.2.0.exe"_s << false;
        QTest::newRow("portable, zip mid-path") << portable << portableName << at + u"q.zip/setup.exe"_s << false;
        QTest::newRow("msix takes an msix") << msix << msixName << at + u"q-0.2.0.msix"_s << true;
        QTest::newRow("msix, portable platform") << msix << portableName << at + u"q-0.2.0.msix"_s << false;
        QTest::newRow("msix, zip file") << msix << msixName << at + u"q-0.2.0.zip"_s << false;
        QTest::newRow("msix, portable release") << msix << portableName << at + u"q-0.2.0.zip"_s << false;
        QTest::newRow("msix, old platform") << msix << u"windows-x64"_s << at + u"q-0.2.0.msix"_s << false;
        // A copy that qKVM's Setup installed takes only Setup's own release (2026-11-08).
        const auto setup = UpdateKind::Setup;
        const QString setupName = u"windows-x64-setup"_s;
        const QString setupFile = at + u"qKVM-Desktop-0.2.0-windows-x64-setup.exe"_s;
        QTest::newRow("setup takes a setup") << setup << setupName << setupFile << true;
        QTest::newRow("setup, in capitals") << setup << setupName << at + u"Q-0.2.0-SETUP.EXE"_s << true;
        QTest::newRow("setup, another exe") << setup << setupName << at + u"q-0.2.0.exe"_s << false;
        QTest::newRow("setup, setup mid-path") << setup << setupName << at + u"q-setup.exe/q.zip"_s << false;
        QTest::newRow("setup, a zip") << setup << setupName << at + u"q-0.2.0.zip"_s << false;
        QTest::newRow("setup, an msix") << setup << setupName << at + u"q-0.2.0.msix"_s << false;
        QTest::newRow("setup, portable platform") << setup << portableName << setupFile << false;
        QTest::newRow("setup, msix platform") << setup << msixName << setupFile << false;
        QTest::newRow("setup, portable release") << setup << portableName << at + u"q-0.2.0.zip"_s << false;
        QTest::newRow("setup, old platform") << setup << u"windows-x64"_s << setupFile << false;
        QTest::newRow("portable, setup release") << portable << setupName << setupFile << false;
        QTest::newRow("portable, setup platform") << portable << setupName << at + u"q-0.2.0.zip"_s << false;
        QTest::newRow("msix, setup release") << msix << setupName << setupFile << false;
    }

    // Which kind of copy runs from a folder: Setup's, by Setup's marker beside the program with exactly its content;
    // else an MSIX copy, by the package's identity; else a portable one.
    void aCopyIsOfSetupsKindByItsMarkerAlone_data()
    {
        QTest::addColumn<QByteArray>("marker");
        QTest::addColumn<bool>("packaged");
        QTest::addColumn<UpdateKind>("kind");
        QTest::newRow("setup's marker") << QByteArray("setup\n") << false << UpdateKind::Setup;
        QTest::newRow("setup's marker, packaged") << QByteArray("setup\n") << true << UpdateKind::Setup;
        QTest::newRow("no marker") << QByteArray() << false << UpdateKind::Portable;
        QTest::newRow("no marker, packaged") << QByteArray() << true << UpdateKind::Msix;
        QTest::newRow("no line feed") << QByteArray("setup") << false << UpdateKind::Portable;
        QTest::newRow("windows line end") << QByteArray("setup\r\n") << false << UpdateKind::Portable;
        QTest::newRow("two line feeds") << QByteArray("setup\n\n") << false << UpdateKind::Portable;
        QTest::newRow("capital") << QByteArray("Setup\n") << false << UpdateKind::Portable;
        QTest::newRow("byte-order mark") << QByteArray("\xEF\xBB\xBFsetup\n") << false << UpdateKind::Portable;
        QTest::newRow("another word") << QByteArray("portable\n") << false << UpdateKind::Portable;
        QTest::newRow("empty, packaged") << QByteArray("") << true << UpdateKind::Msix;
    }

    void aCopyIsOfSetupsKindByItsMarkerAlone()
    {
        QFETCH(QByteArray, marker);
        QFETCH(bool, packaged);
        QFETCH(UpdateKind, kind);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        if (!marker.isNull()) {
            QFile file(folder.filePath(QString::fromLatin1(setupMarkerName)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(marker), marker.size());
        }
        QCOMPARE(updateKindOf(folder.path(), packaged), kind);
    }

    void aFolderNamedAsTheMarkerIsNoMarker()
    {
        QTemporaryDir folder;
        QVERIFY(QDir(folder.path()).mkdir(QString::fromLatin1(setupMarkerName)));
        QCOMPARE(updateKindOf(folder.path(), false), UpdateKind::Portable);
    }

    // The test program has no marker beside it: a portable copy, whose channel names the folder it runs from.
    void theProgramsChannelNamesItsOwnFolder()
    {
        const auto channel = desktopUpdateChannel();
        QCOMPARE(channel.kind, UpdateKind::Portable);
        QCOMPARE(channel.programFolder, QCoreApplication::applicationDirPath());
        QVERIFY(!channel.dataFolder.isEmpty());
    }

    // A Setup copy reads its own feed, beside the built-in one: beta 1's portable copies read stable.json as they do.
    void aSetupCopysFeedLiesBesideTheBuiltInOne()
    {
        const QUrl builtIn(u"https://download.qkvm.com/desktop/stable.json"_s);
        const QByteArray key(64, 'k');
        QTemporaryDir installed;
        QFile marker(installed.filePath(QString::fromLatin1(setupMarkerName)));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write(setupMarkerContent), 6);
        marker.close();
        const auto setup = updateChannelFor(builtIn, key, u"0.1.0"_s, installed.path(), false, u"C:/data"_s);
        QCOMPARE(setup.kind, UpdateKind::Setup);
        QCOMPARE(setup.feed, QUrl(u"https://download.qkvm.com/desktop/stable-setup.json"_s));
        QCOMPARE(setup.programFolder, installed.path());
        QCOMPARE(setup.dataFolder, u"C:/data"_s);
        QCOMPARE(setup.publicKey, key);
        QCOMPARE(setup.currentVersion, u"0.1.0"_s);
        QTemporaryDir unpacked;
        const auto portable = updateChannelFor(builtIn, key, u"0.1.0"_s, unpacked.path(), false, u"C:/data"_s);
        QCOMPARE(portable.kind, UpdateKind::Portable);
        QCOMPARE(portable.feed, builtIn);
        const auto msix = updateChannelFor(builtIn, key, u"0.1.0"_s, unpacked.path(), true, u"C:/data"_s);
        QCOMPARE(msix.kind, UpdateKind::Msix);
        QCOMPARE(msix.feed, builtIn);
        // A development build has no feed, and so none for Setup either.
        QVERIFY(updateChannelFor({}, {}, u"0.1.0"_s, installed.path(), false, {}).feed.isEmpty());
        QCOMPARE(setupFeedBeside(QUrl(u"https://download.qkvm.com/desktop/stable.json"_s)),
                 QUrl(u"https://download.qkvm.com/desktop/stable-setup.json"_s));
        QCOMPARE(setupFeedBeside(QUrl(u"https://updates.example.test/a/b/stable.json"_s)),
                 QUrl(u"https://updates.example.test/a/b/stable-setup.json"_s));
        QVERIFY(setupFeedBeside(QUrl()).isEmpty());
        QVERIFY(setupFeedBeside(QUrl(u"http://download.qkvm.com/desktop/stable.json"_s)).isEmpty());
    }

    void eachKindOfCopyTakesOnlyItsOwnKindOfRelease()
    {
        QFETCH(UpdateKind, copy);
        QFETCH(QString, platform);
        QFETCH(QString, package);
        QFETCH(bool, taken);
        Desk desk;
        const auto now = QDateTime::currentDateTimeUtc();
        auto payload = releaseFor(desk.package, now, copy);
        payload[u"platform"_s] = platform;
        payload[u"package"_s] = package;
        QCOMPARE(static_cast<bool>(verifiedUpdate(desk.signer.seal(payload), desk.signer.publicKey(), now, copy)),
                 taken);
    }

    void releaseToolsFeedIsAcceptedByTheWindowsVerifier_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<UpdateKind>("kind");
        QTest::addColumn<QString>("ending");
        QTest::newRow("msix") << u"data/update-feed-dotnet.json"_s << UpdateKind::Msix << u".msix"_s;
        QTest::newRow("portable") << u"data/update-feed-dotnet-portable.json"_s << UpdateKind::Portable << u".zip"_s;
        // Signed by sign_release.ps1 with a throwaway encrypted key, assembled, then signed again by refresh_feed.ps1.
        QTest::newRow("refreshed") << u"data/update-feed-dotnet-refreshed.json"_s << UpdateKind::Portable << u".zip"_s;
        // prepare_update_payload.ps1 -Setup, assembled for stable-setup.json.
        QTest::newRow("setup") << u"data/update-feed-dotnet-setup.json"_s << UpdateKind::Setup << u"-setup.exe"_s;
    }

    void releaseToolsFeedIsAcceptedByTheWindowsVerifier()
    {
        QFETCH(QString, fixture);
        QFETCH(UpdateKind, kind);
        QFETCH(QString, ending);
        // Prepared/assembled by the PowerShell release tools and signed by .NET,
        // independently of this test's CNG signer. Only public data is retained.
        // From the source folder the build names (a release build trims its folder off __FILE__).
        QFile file(QString::fromUtf8(QKVM_TEST_SOURCES "/") + fixture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto kept = QJsonDocument::fromJson(file.readAll()).object();
        const auto key = QByteArray::fromHex(kept.value(u"public_key"_s).toString().toLatin1());
        const auto envelope = QByteArray::fromBase64(kept.value(u"feed"_s).toString().toLatin1());
        const auto now = QDateTime::fromString(kept.value(u"checked_at"_s).toString(), Qt::ISODate);
        const auto release = verifiedUpdate(envelope, key, now, kind);
        QVERIFY(release);
        QVERIFY(release.value().package.path().endsWith(ending));
        // And every other kind of copy refuses it, signed and current as it is.
        for (const auto other : {UpdateKind::Msix, UpdateKind::Portable, UpdateKind::Setup}) {
            QVERIFY(other == kind || !verifiedUpdate(envelope, key, now, other));
        }
        const QByteArray package("A made-up test package; never installed.");
        QCOMPARE(release.value().version, u"0.2.0"_s);
        QCOMPARE(release.value().notes, u"Test release"_s);
        QCOMPARE(release.value().bytes, package.size());
        QCOMPARE(release.value().sha256, QCryptographicHash::hash(package, QCryptographicHash::Sha256));
    }

    void signedButInvalidReleasesAreRejected_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("other product") << u"product"_s << QJsonValue(u"qkvm-server"_s);
        QTest::newRow("other platform") << u"platform"_s << QJsonValue(u"linux-x64"_s);
        QTest::newRow("other channel") << u"channel"_s << QJsonValue(u"beta"_s);
        QTest::newRow("schema flag") << u"schema"_s << QJsonValue(true);
        QTest::newRow("schema newer") << u"schema"_s << QJsonValue(2);
        QTest::newRow("bad version") << u"version"_s << QJsonValue(u"0.2.0-rc1"_s);
        QTest::newRow("version newline") << u"version"_s << QJsonValue(u"0.2.0\n"_s);
        QTest::newRow("large version") << u"version"_s << QJsonValue(u"65536.0.0"_s);
        QTest::newRow("negative size") << u"bytes"_s << QJsonValue(-1);
        QTest::newRow("too large") << u"bytes"_s << QJsonValue(mostUpdatePackage + 1);
        QTest::newRow("fraction") << u"bytes"_s << QJsonValue(1.5);
        QTest::newRow("bad hash") << u"sha256"_s << QJsonValue(QString(64, u'z'));
        QTest::newRow("http") << u"package"_s << QJsonValue(u"http://updates.example.test/a.zip"_s);
        QTest::newRow("credentials") << u"package"_s << QJsonValue(u"https://user@updates.example.test/a.zip"_s);
        QTest::newRow("exe") << u"package"_s << QJsonValue(u"https://updates.example.test/a.exe"_s);
        QTest::newRow("fragment") << u"package"_s << QJsonValue(u"https://updates.example.test/a.zip#b"_s);
        QTest::newRow("query") << u"package"_s << QJsonValue(u"https://updates.example.test/a.zip?x=1"_s);
        QTest::newRow("expired") << u"expires"_s << QJsonValue(u"2000-01-01T00:00:00Z"_s);
        QTest::newRow("far future") << u"expires"_s << QJsonValue(u"2099-01-01T00:00:00Z"_s);
        QTest::newRow("bad date") << u"published"_s << QJsonValue(u"not a date"_s);
        QTest::newRow("long notes") << u"notes"_s << QJsonValue(QString(16001, u'x'));
    }

    void signedButInvalidReleasesAreRejected()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        Desk desk;
        auto payload = releaseFor(desk.package);
        payload[field] = value;
        QVERIFY(!verifiedUpdate(desk.signer.seal(payload), desk.signer.publicKey(), QDateTime::currentDateTimeUtc(),
                                UpdateKind::Portable));
    }

    void duplicateFieldsAndBrokenUtf8AreRejected()
    {
        Desk desk;
        auto payload = QJsonDocument(releaseFor(desk.package)).toJson(QJsonDocument::Compact);
        payload.insert(1, "\"version\":\"8.0.0\",");
        QVERIFY(!verifiedUpdate(desk.signer.seal(payload), desk.signer.publicKey(), QDateTime::currentDateTimeUtc(),
                                UpdateKind::Portable));
        payload = QByteArray("{\"notes\":\"") + char(-1) + "\"}";
        QVERIFY(!verifiedUpdate(desk.signer.seal(payload), desk.signer.publicKey(), QDateTime::currentDateTimeUtc(),
                                UpdateKind::Portable));
    }

    void automaticChecksNotifyOnceAndNeverDownloadOrInstall()
    {
        Desk desk;
        const QSignalSpy available(&desk.updates, &Updates::available);
        const auto now = QDateTime::currentDateTimeUtc();
        desk.network.next = {desk.signer.seal(releaseFor(desk.package))};
        desk.updates.checkIfDue(now);
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        QCOMPARE(available.count(), 1);
        QCOMPARE(desk.network.requests.size(), 1);
        QVERIFY(desk.updates.lastChecked().isValid());
        desk.updates.checkIfDue(now.addSecs(3600));
        QCOMPARE(desk.network.requests.size(), 1);
        desk.updates.checkIfDue(now.addDays(1));
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        QCOMPARE(desk.network.requests.size(), 2);
        QCOMPARE(available.count(), 1);
        desk.updates.setAutomatic(false);
        desk.updates.checkIfDue(now.addDays(3));
        QCOMPARE(desk.network.requests.size(), 2);
        const Settings reread(desk.folder.filePath(u"settings.json"_s));
        QVERIFY(!reread.remembered().checkUpdates);
        QVERIFY(reread.remembered().lastUpdateCheck.isValid());
    }

    // Round 2 (review L1): update checks go straight to the feed, as every other line of the program
    // does: no proxy the application or Windows names (WPAD, PAC). (Before: the system's proxy rules.)
    void updateChecksUseNoProxy()
    {
        Desk desk;
        QCOMPARE(desk.network.proxy().type(), QNetworkProxy::NoProxy);
    }
    void requestHasNoCookiesCredentialsCacheOrRedirects()
    {
        Desk desk;
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        const auto request = desk.network.requests.first();
        QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), QNetworkRequest::Manual);
        QCOMPARE(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), QNetworkRequest::Manual);
        QCOMPARE(request.attribute(QNetworkRequest::AuthenticationReuseAttribute).toInt(), QNetworkRequest::Manual);
        QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 QNetworkRequest::ManualRedirectPolicy);
        QVERIFY(!request.hasRawHeader("Authorization"));
        QCOMPARE(request.rawHeader("User-Agent"), QByteArray("qKVM-Desktop-Updater"));
    }

    void currentAndOlderReleasesNeverOfferADowngrade_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<UpdateKind>("kind");
        QTest::newRow("equal") << u"0.1.0"_s << UpdateKind::Portable;
        QTest::newRow("older") << u"0.0.9"_s << UpdateKind::Portable;
        // A Setup copy is never run back, nor the same again (and nothing then is downloaded, verified or run).
        QTest::newRow("setup, equal") << u"0.1.0"_s << UpdateKind::Setup;
        QTest::newRow("setup, older") << u"0.0.9"_s << UpdateKind::Setup;
        QTest::newRow("setup, a minor back") << u"0.0.10"_s << UpdateKind::Setup;
    }
    void currentAndOlderReleasesNeverOfferADowngrade()
    {
        QFETCH(QString, version);
        QFETCH(UpdateKind, kind);
        Desk desk(kind);
        auto payload = releaseFor(desk.package, QDateTime::currentDateTimeUtc(), kind);
        payload[u"version"_s] = version;
        desk.check(payload);
        QTRY_COMPARE(desk.updates.state(), Updates::State::Current);
        desk.download();
        QCOMPARE(desk.network.requests.size(), 1);
        QVERIFY(!desk.updates.runSetup());
    }

    void failuresAreNeverReportedAsCurrentAndDoNotChangeSuccessfulDate_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 5; ++kind) {
            QTest::newRow(qPrintable(QString::number(kind))) << kind;
        }
    }
    void failuresAreNeverReportedAsCurrentAndDoNotChangeSuccessfulDate()
    {
        QFETCH(int, kind);
        Desk desk;
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        const auto successful = desk.updates.lastChecked();
        UpdateAnswer answer{desk.signer.seal(releaseFor(desk.package))};
        if (kind == 0) {
            answer.status = 302;
        }
        if (kind == 1) {
            answer.status = 500;
        }
        if (kind == 2) {
            answer.error = QNetworkReply::SslHandshakeFailedError;
        }
        if (kind == 3) {
            answer.bytes = "not json";
        }
        if (kind == 4) {
            answer.declaredBytes = mostUpdateMetadata + 1;
        }
        desk.network.next = answer;
        desk.updates.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Error);
        QCOMPARE(desk.updates.lastChecked(), successful);
        QVERIFY(!desk.updates.problem().isEmpty());
    }

    void verifiedDownloadWaitsForAnExplicitInstallerRequest()
    {
        Desk desk(UpdateKind::Msix);
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        desk.download();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Ready);
        int opened = 0;
        QVERIFY(desk.updates.openInstaller([&](const QString& file) {
            ++opened;
            QFile downloaded(file);
            return downloaded.open(QIODevice::ReadOnly) && downloaded.readAll() == desk.package;
        }));
        QCOMPARE(opened, 1);
        QCOMPARE(desk.updates.state(), Updates::State::Ready);
        desk.updates.cancel();
        QCOMPARE(desk.updates.state(), Updates::State::Available);
        QVERIFY(!desk.updates.openInstaller([&](const QString&) {
            ++opened;
            return true;
        }));
        QCOMPARE(opened, 1);
    }

    void wrongTruncatedAndOversizedDownloadsNeverReachTheInstaller_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<UpdateKind>("copy");
        QTest::newRow("corrupt") << 0 << UpdateKind::Msix;
        QTest::newRow("short") << 1 << UpdateKind::Msix;
        QTest::newRow("long") << 2 << UpdateKind::Msix;
        QTest::newRow("corrupt zip") << 0 << UpdateKind::Portable;
        QTest::newRow("short zip") << 1 << UpdateKind::Portable;
        QTest::newRow("long zip") << 2 << UpdateKind::Portable;
    }
    void wrongTruncatedAndOversizedDownloadsNeverReachTheInstaller()
    {
        QFETCH(int, kind);
        QFETCH(UpdateKind, copy);
        Desk desk(copy);
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        auto bytes = desk.package;
        if (kind == 0) {
            bytes[0] = 'x';
        }
        if (kind == 1) {
            bytes.chop(1);
        }
        if (kind == 2) {
            bytes.append('x');
        }
        desk.network.next = {bytes};
        desk.updates.download();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Error);
        bool opened = false;
        QVERIFY(!desk.updates.openInstaller([&](const QString&) {
            opened = true;
            return true;
        }));
        QVERIFY(!desk.updates.showFile([&](const QString&) {
            opened = true;
            return true;
        }));
        QVERIFY(!opened);
        QDirIterator files(desk.folder.filePath(u"downloads"_s), QDir::Files | QDir::Hidden | QDir::System,
                           QDirIterator::Subdirectories);
        QVERIFY(!files.hasNext());
    }

    void cancelledDownloadAndLateReplyCannotInstall()
    {
        Desk desk;
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        desk.network.next = {desk.package};
        desk.updates.download();
        desk.updates.cancel();
        QCoreApplication::processEvents();
        QCOMPARE(desk.updates.state(), Updates::State::Available);
        QCOMPARE(desk.updates.received(), 0);
    }

    void cancellingAndCheckingAgainFromAProgressNoticeKeepsTheNewRequest()
    {
        Desk desk;
        int progress = 0;
        bool replaced = false;
        connect(&desk.updates, &Updates::changed, &desk.updates, [&] {
            if (!replaced && desk.updates.state() == Updates::State::Checking && desk.updates.received() > 0 &&
                ++progress == 2) {
                replaced = true;
                desk.updates.cancel();
                desk.updates.check();
            }
        });
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        QVERIFY(replaced);
        QCOMPARE(desk.network.requests.size(), 2);
    }

    void packageIsReverifiedAndInstallerFailureIsReported()
    {
        Desk desk(UpdateKind::Msix);
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        desk.download();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Ready);
        QVERIFY(!desk.updates.openInstaller([](const QString&) { return false; }));
        QVERIFY(!desk.updates.problem().isEmpty());
        QDirIterator files(desk.folder.filePath(u"downloads"_s), {u"*.msix"_s}, QDir::Files,
                           QDirIterator::Subdirectories);
        QVERIFY(files.hasNext());
        QFile saved(files.next());
        QVERIFY(saved.open(QIODevice::WriteOnly));
        saved.write("tampered");
        saved.close();
        bool opened = false;
        QVERIFY(!desk.updates.openInstaller([&](const QString&) {
            opened = true;
            return true;
        }));
        QVERIFY(!opened);
        QCOMPARE(desk.updates.state(), Updates::State::Error);
    }

    // A portable copy's update is shown to the person in its folder, never handed to an installer or run; the file
    // stays when the program ends, for the person to unpack, and nothing writes to it while the program runs.
    void aPortableUpdateIsShownInItsFolderAndNeverHandedToAnInstaller()
    {
        QTemporaryDir folder;
        Settings settings(folder.filePath(u"settings.json"_s));
        const ReleaseSigner signer;
        UpdateNetwork network;
        const QByteArray package("a made-up zip, never unpacked or run");
        QString shown;
        {
            Updates updates({QUrl(u"https://updates.example.test/stable.json"_s), signer.publicKey(), u"0.1.0"_s,
                             UpdateKind::Portable},
                            settings, network, folder.filePath(u"downloads"_s));
            QCOMPARE(updates.kind(), UpdateKind::Portable);
            network.next = {signer.seal(releaseFor(package))};
            updates.check();
            QTRY_COMPARE(updates.state(), Updates::State::Available);
            network.next = {package};
            updates.download();
            QTRY_COMPARE(updates.state(), Updates::State::Ready);
            int installers = 0;
            QVERIFY(!updates.openInstaller([&](const QString&) {
                ++installers;
                return true;
            }));
            QCOMPARE(installers, 0);
            QCOMPARE(updates.state(), Updates::State::Ready);
            int shows = 0;
            const auto show = [&](const QString& file) {
                ++shows;
                shown = file;
                return true;
            };
            QVERIFY(updates.showFile(show));
            QCOMPARE(shows, 1);
            QCOMPARE(QFileInfo(shown).fileName(), u"qkvm.zip"_s);
            QFile kept(shown);
            QVERIFY(kept.open(QIODevice::ReadOnly));
            QCOMPARE(kept.readAll(), package);
            kept.close();
            QVERIFY(updates.problem().isEmpty());
            QCOMPARE(updates.state(), Updates::State::Ready);
            // Held against writing while the program runs.
            QFile writer(shown);
            QVERIFY(!writer.open(QIODevice::ReadWrite));
            // And shown again, as often as asked.
            QVERIFY(updates.showFile(show));
            QCOMPARE(shows, 2);
            QCOMPARE(network.requests.size(), 2);
        }
        // Kept for the person after the program has ended, and no longer held.
        QVERIFY(QFileInfo::exists(shown));
        QFile after(shown);
        QVERIFY(after.open(QIODevice::ReadWrite));
    }

    void anMsixUpdateIsNeverShownAsAFile()
    {
        Desk desk(UpdateKind::Msix);
        QCOMPARE(desk.updates.kind(), UpdateKind::Msix);
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        desk.download();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Ready);
        bool shown = false;
        QVERIFY(!desk.updates.showFile([&](const QString&) {
            shown = true;
            return true;
        }));
        QVERIFY(!shown);
        QCOMPARE(desk.updates.state(), Updates::State::Ready);
        QVERIFY(desk.updates.openInstaller([](const QString& file) { return file.endsWith(u"qkvm.msix"_s); }));
    }

    void aShownFileIsVerifiedAgainFirst()
    {
        Desk desk;
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        desk.download();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Ready);
        QDirIterator files(desk.folder.filePath(u"downloads"_s), {u"qkvm.zip"_s}, QDir::Files,
                           QDirIterator::Subdirectories);
        QVERIFY(files.hasNext());
        QFile saved(files.next());
        QVERIFY(saved.open(QIODevice::WriteOnly));
        saved.write("a different file of nearly the same size");
        saved.close();
        bool shown = false;
        QVERIFY(!desk.updates.showFile([&](const QString&) {
            shown = true;
            return true;
        }));
        QVERIFY(!shown);
        QCOMPARE(desk.updates.state(), Updates::State::Error);
        QVERIFY(!desk.updates.problem().isEmpty());
        QVERIFY(!QFileInfo::exists(saved.fileName()));
    }

    void aFileThatCouldNotBeShownIsSaidWithWhereItIs()
    {
        Desk desk;
        desk.check();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
        desk.download();
        QTRY_COMPARE(desk.updates.state(), Updates::State::Ready);
        QString asked;
        QVERIFY(!desk.updates.showFile([&](const QString& file) {
            asked = file;
            return false;
        }));
        QVERIFY(!asked.isEmpty());
        QVERIFY(desk.updates.problem().contains(QDir::toNativeSeparators(asked)));
        QCOMPARE(desk.updates.state(), Updates::State::Ready);
        // Not held where it was not shown.
        QFile writer(asked);
        QVERIFY(writer.open(QIODevice::ReadWrite));
        writer.close();
        // And shown at the next asking.
        QVERIFY(desk.updates.showFile([](const QString&) { return true; }));
        QVERIFY(desk.updates.problem().isEmpty());
    }

    // ---- A Setup copy: "Install and restart" (the contract with Setup, 2026-11-08) ----

    // The command line, whole: each path in Windows' spelling between quotes, and nothing of a path that would not do.
    void setupsCommandLineIsTheContractsAndNothingElse_data()
    {
        QTest::addColumn<QString>("setup");
        QTest::addColumn<QString>("folder");
        QTest::addColumn<QString>("log");
        QTest::addColumn<QString>("line");
        const QString setup = u"D:/Home/a b/AppData/Local/qKVM/Cache/Downloads/update-Ab1234/qkvm-desktop-setup.exe"_s;
        const QString folder = u"D:/Home/a b/AppData/Local/Programs/qKVM Desktop"_s;
        const QString log = u"D:/Home/a b/AppData/Local/qKVM/qKVM Desktop/updates/setup-0.2.0.log"_s;
        QTest::newRow("as the contract says")
            << setup << folder << log
            << u"\"D:\\Home\\a b\\AppData\\Local\\qKVM\\Cache\\Downloads\\update-Ab1234\\qkvm-desktop-setup.exe\" "
               "/SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CURRENTUSER /UPDATE=1 "
               "/DIR=\"D:\\Home\\a b\\AppData\\Local\\Programs\\qKVM Desktop\" "
               "/LOG=\"D:\\Home\\a b\\AppData\\Local\\qKVM\\qKVM Desktop\\updates\\setup-0.2.0.log\""_s;
        QTest::newRow("a quote in the folder") << setup << u"C:/q\" /DIR=\"C:/elsewhere"_s << log << QString();
        QTest::newRow("a quote in the log") << setup << folder << u"C:/l\"og.log"_s << QString();
        QTest::newRow("a quote in the setup") << u"C:/s\"etup.exe"_s << folder << log << QString();
        QTest::newRow("a new line") << setup << u"C:/a\n/b"_s << log << QString();
        QTest::newRow("a backslash at the end") << setup << u"C:/Programs/qKVM Desktop/"_s << log << QString();
        QTest::newRow("a drive's root") << setup << u"C:/"_s << log << QString();
        QTest::newRow("relative") << setup << u"Programs/qKVM Desktop"_s << log << QString();
        QTest::newRow("drive-relative") << setup << u"C:Programs"_s << log << QString();
        QTest::newRow("rooted, no drive") << setup << u"/Programs/qKVM"_s << log << QString();
        QTest::newRow("a share") << setup << u"//server/share/qKVM"_s << log << QString();
        QTest::newRow("no log") << setup << folder << QString() << QString();
    }

    void setupsCommandLineIsTheContractsAndNothingElse()
    {
        QFETCH(QString, setup);
        QFETCH(QString, folder);
        QFETCH(QString, log);
        QFETCH(QString, line);
        QCOMPARE(setupCommandLine(setup, folder, log), line);
    }

    // Verified, then run with exactly the contract's command line, from the very file that was verified; the hand-over
    // noted beside the settings; the file held until the program ends.
    void aSetupCopyRunsTheVerifiedSetupWithTheContractsCommandLine()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        const QString downloaded = desk.downloaded();
        QVERIFY(desk.updates.runSetup());
        const auto ran = desk.whatSetupWrote();
        QCOMPARE(ran.size(), 5);
        const QString log = QDir::toNativeSeparators(desk.updates.setupLogFor(u"0.2.0"_s));
        QCOMPARE(log, QDir::toNativeSeparators(desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s)));
        QCOMPARE(ran[0], u"\""_s + QDir::toNativeSeparators(ran[2]) + u"\" /SILENT /SUPPRESSMSGBOXES /NOCANCEL "_s +
                             u"/NORESTART /CURRENTUSER /UPDATE=1 /DIR=\""_s + QDir::toNativeSeparators(desk.program()) +
                             u"\" /LOG=\""_s + log + u'"');
        QCOMPARE(ran[1], desk.packageHash());
        QVERIFY(QFileInfo(ran[2]) == QFileInfo(downloaded));
        // Setup unpacks itself into a TEMP of its own, kept as its folder is, beside it in the cache's Downloads,
        // which is kept so too.
        const QString downloads = desk.folder.filePath(u"downloads"_s);
        QCOMPARE(ran[3], ran[4]);
        QCOMPARE(QFileInfo(ran[3]).absolutePath().toLower(), QFileInfo(downloads).absoluteFilePath().toLower());
        QVERIFY(QFileInfo(ran[3]).fileName().startsWith(u"setup-temp-"_s));
        QVERIFY(keptToThisUserAlone(ran[3]));
        QVERIFY(keptToThisUserAlone(downloads));
        const auto noted = desk.handOver();
        QCOMPARE(noted.value(u"temp"_s).toString(), QFileInfo(ran[3]).fileName());
        QCOMPARE(noted.value(u"to"_s).toString(), u"0.2.0"_s);
        QCOMPARE(noted.value(u"from"_s).toString(), u"0.1.0"_s);
        QVERIFY(noted.value(u"process"_s).toInteger() > 0);
        // Still held: nobody writes, renames or removes it, or its folder, while this program runs.
        QFile writer(downloaded);
        QVERIFY(!writer.open(QIODevice::ReadWrite));
        QVERIFY(!QFile::rename(downloaded, downloaded + u".old"_s));
        QVERIFY(!QDir().rename(QFileInfo(downloaded).absolutePath(), QFileInfo(downloaded).absolutePath() + u"x"_s));
        QCOMPARE(desk.updates.state(), Updates::State::Ready);
        QVERIFY(desk.updates.problem().isEmpty());
    }

    // Never for another kind of copy: a portable copy's ZIP and an MSIX copy's package are never run (here each would
    // run, a program in its folder kept as a Setup's is, and Setup's marker beside the program, but for its kind).
    void noOtherKindOfCopyRunsSetup()
    {
        for (const auto kind : {UpdateKind::Portable, UpdateKind::Msix}) {
            Desk desk(kind);
            desk.package = standInSetup();
            QVERIFY(!desk.package.isEmpty());
            QVERIFY(QDir().mkpath(desk.program()));
            QFile marker(desk.program() + u'/' + QString::fromLatin1(setupMarkerName));
            QVERIFY(marker.open(QIODevice::WriteOnly));
            QCOMPARE(marker.write(setupMarkerContent), 6);
            marker.close();
            desk.check();
            QTRY_COMPARE(desk.updates.state(), Updates::State::Available);
            desk.download();
            QTRY_COMPARE(desk.updates.state(), Updates::State::Ready);
            QDirIterator files(desk.folder.filePath(u"downloads"_s), QDir::Files, QDirIterator::Subdirectories);
            QVERIFY(files.hasNext());
            QVERIFY(keepToThisUserAsSetupsIs(QFileInfo(files.next()).absolutePath()));
            QVERIFY(!desk.updates.runSetup());
            QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s)));
            QCOMPARE(desk.updates.state(), Updates::State::Ready);
        }
        // Nor before its download is verified.
        Desk early(UpdateKind::Setup);
        early.check();
        QTRY_COMPARE(early.updates.state(), Updates::State::Available);
        QVERIFY(!early.updates.runSetup());
        QCOMPARE(early.network.requests.size(), 1);
    }

    // A file changed after its download (before it was held) is refused, never run, and downloaded anew.
    void aSetupChangedAfterItsDownloadIsNeverRunAndIsDownloadedAnew()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        QFile saved(desk.downloaded());
        QVERIFY(saved.open(QIODevice::ReadWrite));
        QVERIFY(saved.seek(saved.size() - 1));
        QCOMPARE(saved.write("!"), 1);
        saved.close();
        desk.network.next = {desk.package};
        QVERIFY(!desk.updates.runSetup());
        QVERIFY(desk.updates.problem().contains(u"downloaded again"_s));
        QCOMPARE(desk.network.requests.size(), 3);
        QTRY_COMPARE(desk.updates.state(), Updates::State::Ready);
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s)));
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-handover.json"_s)));
        // And the new download is run, verified.
        QVERIFY(desk.updates.runSetup());
        QCOMPARE(desk.whatSetupWrote().value(1), desk.packageHash());
    }

    // Between the check and the start nothing can take the file's place: it is held against writing, renaming and
    // deletion, and so is its folder; where it could be, the file run would be another (the stand-in says its hash).
    void nothingTakesTheSetupsPlaceBetweenItsCheckAndItsStart()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        QStringList took;
        desk.updates.beforeSetupStarts([&](const QString& file) {
            const QByteArray decoy = desk.package + QByteArray("another file");
            if (QFile writer(file); writer.open(QIODevice::ReadWrite)) {
                took << u"written"_s;
                writer.write(decoy);
            }
            if (QFile::rename(file, file + u".old"_s)) {
                took << u"renamed"_s;
                QFile put(file);
                if (put.open(QIODevice::WriteOnly)) {
                    put.write(decoy);
                }
            }
            const QString folder = QFileInfo(file).absolutePath();
            if (QDir().rename(folder, folder + u"-moved"_s)) {
                took << u"folder renamed"_s;
            }
            // Nor a folder above it: the cache's Downloads.
            const QString above = QFileInfo(folder).absolutePath();
            if (QDir().rename(above, above + u"-moved"_s)) {
                took << u"folder above renamed"_s;
            }
        });
        QVERIFY(desk.updates.runSetup());
        QCOMPARE(took, QStringList());
        QCOMPARE(desk.whatSetupWrote().value(1), desk.packageHash());
    }

    // A junction in the way is not followed at the start: Setup is started from the file that is held, by its own
    // whole name, and not from wherever the junction points by then.
    void aJunctionOnTheWayIsNotFollowedToAnotherSetup()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        const QString downloaded = desk.downloaded();
        const QString folder = QFileInfo(downloaded).absolutePath();
        const QString moved = desk.folder.filePath(u"moved"_s);
        const QString decoys = desk.folder.filePath(u"decoys"_s);
        QVERIFY(QDir().rename(folder, moved));
        QVERIFY(junction(folder, moved));
        QVERIFY(QDir().mkpath(decoys));
        QFile decoy(decoys + u'/' + QFileInfo(downloaded).fileName());
        QVERIFY(decoy.open(QIODevice::WriteOnly));
        QVERIFY(decoy.write(desk.package + QByteArray("a decoy")) > 0);
        decoy.close();
        bool swapped = false;
        desk.updates.beforeSetupStarts([&](const QString&) {
            swapped = RemoveDirectoryW(QDir::toNativeSeparators(folder).toStdWString().c_str()) != FALSE &&
                      junction(folder, decoys);
        });
        QVERIFY(desk.updates.runSetup());
        QVERIFY(swapped);
        const auto ran = desk.whatSetupWrote();
        QCOMPARE(ran.value(1), desk.packageHash());
        QCOMPARE(QFileInfo(ran.value(2)).absolutePath().toLower(), QFileInfo(moved).absoluteFilePath().toLower());
    }

    // A library put beside Setup would be loaded by it: anything beside it, and Setup is not run.
    void aSetupWithAnythingBesideItIsNeverRun()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        QFile beside(QFileInfo(desk.downloaded()).absolutePath() + u"/version.dll"_s);
        QVERIFY(beside.open(QIODevice::WriteOnly));
        beside.close();
        QVERIFY(!desk.updates.runSetup());
        QVERIFY(desk.updates.problem().contains(u"alone"_s));
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s)));
    }

    // Its folder is kept to this user, SYSTEM and the administrators from the start, cut from what the folders above
    // it let others do; and one that others may change by then is not run from.
    void aSetupsFolderIsKeptToThisUser()
    {
        Desk desk(UpdateKind::Setup);
        // From the moment it is made, not only after: there is no moment at which it is anybody else's.
        QStringList made;
        desk.updates.whenFolderMade([&made](const QString& folder) {
            if (keptToThisUserAlone(folder)) {
                made << folder;
            }
        });
        QVERIFY(desk.readyToRun());
        const QString folder = QFileInfo(desk.downloaded()).absolutePath();
        QCOMPARE(made, QStringList{folder});
        QVERIFY(keptToThisUserAlone(folder));
        QVERIFY(keptToThisUserAlone(desk.folder.filePath(u"downloads"_s)));
    }

    void aSetupInAFolderOthersMayChangeIsNeverRun_data()
    {
        QTest::addColumn<QString>("who");
        QTest::addColumn<int>("may");
        QTest::newRow("everybody may add a file") << u"S-1-1-0"_s << int(FILE_ADD_FILE);
        QTest::newRow("users may change the list") << u"S-1-5-32-545"_s << int(WRITE_DAC);
        QTest::newRow("a capability may do all") << u"S-1-15-3-1"_s << int(GENERIC_ALL);
    }

    void aSetupInAFolderOthersMayChangeIsNeverRun()
    {
        QFETCH(QString, who);
        QFETCH(int, may);
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        QVERIFY(letAlsoChange(QFileInfo(desk.downloaded()).absolutePath(), who, static_cast<DWORD>(may)));
        QVERIFY(!desk.updates.runSetup());
        QVERIFY(desk.updates.problem().contains(u"only you can change"_s));
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s)));
    }

    // Setup installs into the folder the program runs from: no longer Setup's (its marker gone), and it is not run.
    void aFolderThatIsNoLongerSetupsIsNotUpdatedInPlace()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        QVERIFY(QFile::remove(desk.program() + u'/' + QString::fromLatin1(setupMarkerName)));
        QVERIFY(!desk.updates.runSetup());
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s)));
    }

    // Setup runs with Inno's RedirectionGuard, and crosses no junction the person made: every path it is given, its
    // own, its /DIR, its /LOG and its TEMP and TMP, is the final one, as Windows says it of the folder, through no
    // junction (the program's folder, its data and its cache each behind one here).
    void everyPathSetupIsGivenIsItsFinalOne()
    {
        Desk desk(UpdateKind::Setup);
        const QString realProgram = desk.folder.filePath(u"real-program"_s);
        const QString realData = desk.folder.filePath(u"real-data"_s);
        const QString realDownloads = desk.folder.filePath(u"real-downloads"_s);
        for (const QString& real : {realProgram, realData, realDownloads}) {
            QVERIFY(QDir().mkpath(real));
        }
        QVERIFY(junction(desk.program(), realProgram));
        QVERIFY(junction(desk.folder.filePath(u"data"_s), realData));
        QVERIFY(junction(desk.folder.filePath(u"downloads"_s), realDownloads));
        QVERIFY(desk.readyToRun());
        QVERIFY(desk.updates.runSetup());
        const auto ran = desk.whatSetupWrote();
        QCOMPARE(ran.size(), 5);
        const QString line = ran[0].toLower();
        const auto native = [](const QString& path) { return QDir::toNativeSeparators(path).toLower(); };
        QVERIFY2(line.contains(u"/dir=\""_s + native(realProgram) + u'"'), qPrintable(ran[0]));
        QVERIFY2(line.contains(u"/log=\""_s + native(realData + u"/updates/setup-0.2.0.log"_s) + u'"'),
                 qPrintable(ran[0]));
        QVERIFY2(line.startsWith(u'"' + native(realDownloads)), qPrintable(ran[0]));
        QVERIFY2(native(ran[3]).startsWith(native(realDownloads) + u"\\setup-temp-"_s), qPrintable(ran[3]));
        QCOMPARE(native(ran[4]), native(ran[3]));
    }
    // Setup's own TEMP folders that were left (Setup ended in a way that left what it unpacked, or its outer process
    // still held it at the next start) are taken away a week on, with what is in them, at the program's start; never
    // one that is newer, has anything newer in it, holds a junction or link, or is named otherwise.
    void oldSetupTempFoldersAreTakenAwayAWeekOn()
    {
        QTemporaryDir folder;
        const QString downloads = folder.filePath(u"downloads"_s);
        const QString outside = folder.filePath(u"outside"_s);
        QVERIFY(QDir().mkpath(outside));
        QFile kept(outside + u"/kept.txt"_s);
        QVERIFY(kept.open(QIODevice::WriteOnly));
        QCOMPARE(kept.write("kept"), 4);
        kept.close();
        // A folder with what Inno unpacks in it, all of it ten days old.
        const auto made = [&](const QString& name, int daysOld) {
            const QString place = downloads + u'/' + name;
            const QString inner = place + u"/is-AB12C.tmp"_s;
            if (!QDir().mkpath(inner)) {
                return QString();
            }
            QFile file(inner + u"/setup.tmp"_s);
            if (!file.open(QIODevice::WriteOnly) || file.write("unpacked") != 8) {
                return QString();
            }
            file.close();
            return daysOld == 0 || (ageBy(file.fileName(), daysOld) && ageBy(inner, daysOld) && ageBy(place, daysOld))
                       ? place
                       : QString();
        };
        const QString old = made(u"setup-temp-Old111"_s, 10);
        const QString fresh = made(u"setup-temp-New222"_s, 0);
        const QString newerInside = made(u"setup-temp-Mid333"_s, 10);
        const QString linked = made(u"setup-temp-Lnk444"_s, 10);
        const QString otherName = made(u"setup-temp-Long5555"_s, 10);
        QVERIFY(!old.isEmpty() && !fresh.isEmpty() && !newerInside.isEmpty() && !linked.isEmpty() &&
                !otherName.isEmpty());
        QFile newer(newerInside + u"/is-AB12C.tmp/newer.txt"_s);
        QVERIFY(newer.open(QIODevice::WriteOnly));
        newer.close();
        QVERIFY(ageBy(newerInside + u"/is-AB12C.tmp"_s, 10) && ageBy(newerInside, 10));
        // (All of it old, what the junction leads to as well: only the junction keeps it.)
        QVERIFY(junction(linked + u"/is-AB12C.tmp/away"_s, outside));
        QVERIFY(ageBy(outside + u"/kept.txt"_s, 10) && ageBy(outside, 10) &&
                ageBy(linked + u"/is-AB12C.tmp/away"_s, 10, true));
        QVERIFY(ageBy(linked + u"/is-AB12C.tmp"_s, 10) && ageBy(linked, 10));
        // The program's start.
        Settings settings(folder.filePath(u"settings.json"_s));
        UpdateNetwork network;
        const Updates updates({{}, {}, u"0.1.0"_s}, settings, network, downloads);
        QVERIFY(!QFileInfo::exists(old));
        QVERIFY(QFileInfo::exists(fresh + u"/is-AB12C.tmp/setup.tmp"_s));
        QVERIFY(QFileInfo::exists(newerInside + u"/is-AB12C.tmp/setup.tmp"_s));
        QVERIFY(QFileInfo::exists(linked + u"/is-AB12C.tmp/setup.tmp"_s));
        QVERIFY(QFileInfo::exists(outside + u"/kept.txt"_s));
        QVERIFY(QFileInfo::exists(otherName + u"/is-AB12C.tmp/setup.tmp"_s));
    }
    // A second attempt at the same version starts with no log of the first: a Setup that ends before it writes its
    // own (its stub could not unpack itself) leaves no earlier attempt's word standing at the next start; and where
    // that log cannot be taken away, Setup is not run.
    void anEarlierAttemptsLogIsNeverReadForThisOne()
    {
        const QString stale = u"2026-11-08 20:34:09.306   qKVM-RESULT: 105 the program was still running\r\n"_s;
        {
            // An earlier attempt's log, and this one's Setup ends before it writes any: not read.
            Desk desk(UpdateKind::Setup);
            QVERIFY(desk.readyToRun());
            const QString log = desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s);
            QVERIFY(QDir().mkpath(QFileInfo(log).absolutePath()));
            QFile earlier(log);
            QVERIFY(earlier.open(QIODevice::WriteOnly) && earlier.write(stale.toUtf8()) > 0);
            earlier.close();
            const Environment silent(u"QKVM_STAND_IN_SETUP_SILENT"_s, u"1"_s);
            QVERIFY(desk.updates.runSetup());
            QTRY_VERIFY_WITH_TIMEOUT(!desk.setupStillRuns(), 15000);
            QVERIFY(!QFileInfo::exists(log));
            desk.network.next = {
                desk.signer.seal(releaseFor(desk.package, QDateTime::currentDateTimeUtc(), desk.kind))};
            const Updates after(desk.channel(u"0.1.0"_s), desk.settings, desk.network,
                                desk.folder.filePath(u"downloads"_s));
            QCOMPARE(after.afterSetup(), u"The update to qKVM 0.2.0 was not installed. Setup's log: "_s +
                                             QDir::toNativeSeparators(desk.updates.setupLogFor(u"0.2.0"_s)));
        }
        // That log held open (deletion not shared): Setup is not run.
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        const QString log = desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s);
        QVERIFY(QDir().mkpath(QFileInfo(log).absolutePath()));
        QFile earlier(log);
        QVERIFY(earlier.open(QIODevice::WriteOnly) && earlier.write(stale.toUtf8()) > 0);
        QVERIFY(!desk.updates.runSetup());
        QVERIFY(desk.updates.problem().contains(u"earlier attempt"_s));
        QVERIFY(!desk.setupStillRuns());
    }
    // ---- The start after a hand-over ----

    void theStartAfterAnUpdateSaysItWasInstalled()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        QVERIFY(desk.updates.runSetup());
        QVERIFY(!desk.whatSetupWrote().isEmpty());
        const Updates after(desk.channel(u"0.2.0"_s), desk.settings, desk.network,
                            desk.folder.filePath(u"downloads"_s));
        QCOMPARE(after.afterSetup(), u"Updated to qKVM 0.2.0."_s);
        QVERIFY(!after.afterSetupWentWrong());
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-handover.json"_s)));
        QCOMPARE(desk.network.requests.size(), 2);
    }

    void theStartAfterAFailedUpdateSaysWhereSetupsLogIsAndOffersItAgain()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        QVERIFY(desk.updates.runSetup());
        QVERIFY(!desk.whatSetupWrote().isEmpty());
        QTRY_VERIFY(!desk.setupStillRuns());
        desk.network.next = {desk.signer.seal(releaseFor(desk.package, QDateTime::currentDateTimeUtc(), desk.kind))};
        Updates after(desk.channel(u"0.1.0"_s), desk.settings, desk.network, desk.folder.filePath(u"downloads"_s));
        QVERIFY(after.afterSetup().startsWith(u"The update to qKVM 0.2.0 was not installed."_s));
        QVERIFY(after.afterSetup().contains(QDir::toNativeSeparators(desk.updates.setupLogFor(u"0.2.0"_s))));
        QVERIFY(after.afterSetupWentWrong());
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-handover.json"_s)));
        // Setup's own TEMP, empty once Setup is over, is let go of.
        QVERIFY(QDir(desk.folder.filePath(u"downloads"_s)).entryList({u"setup-temp-*"_s}, QDir::Dirs).isEmpty());
        // Offered again: checked for anew, and its download still waits for the person.
        QTRY_COMPARE(after.state(), Updates::State::Available);
        QCOMPARE(desk.network.requests.size(), 3);
    }

    // No copy of the program runs while Setup does, so what came of an update that changed nothing is read at the next
    // start from Setup's log (the contract with Setup: one line "qKVM-RESULT: 105 ..." or "qKVM-RESULT: 7 nothing was
    // changed: <reason>"): said in plain words, and the update offered again; with no such line, not installed, with
    // the log's place.
    void whatSetupSaidInItsLogIsToldAtTheNextStart_data()
    {
        QTest::addColumn<QString>("result");
        QTest::addColumn<QString>("said");
        QTest::newRow("still running")
            << u"105 the program was still running"_s
            << u"The update to qKVM 0.2.0 was not installed: qKVM was still running when Setup needed it closed, so "
               "Setup changed nothing. Choose Install and restart again."_s;
        QTest::newRow("nothing changed")
            << u"7 nothing was changed: a file of qKVM's was in use"_s
            << u"The update to qKVM 0.2.0 was not installed, and nothing was changed: a file of qKVM's was in use. "
               "Setup's log: "_s;
        QTest::newRow("none") << QString() << u"The update to qKVM 0.2.0 was not installed. Setup's log: "_s;
        QTest::newRow("another word") << u"5 aborted"_s
                                      << u"The update to qKVM 0.2.0 was not installed. Setup's log: "_s;
        QTest::newRow("a number that only begins so")
            << u"70 nothing was changed: no"_s << u"The update to qKVM 0.2.0 was not installed. Setup's log: "_s;
    }

    void whatSetupSaidInItsLogIsToldAtTheNextStart()
    {
        QFETCH(QString, result);
        QFETCH(QString, said);
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        const Environment says(u"QKVM_STAND_IN_SETUP_RESULT"_s, result);
        QVERIFY(desk.updates.runSetup());
        QVERIFY(desk.setupStillRuns() || QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-0.2.0.log"_s)));
        QTRY_VERIFY_WITH_TIMEOUT(!desk.setupStillRuns(), 15000);
        desk.network.next = {desk.signer.seal(releaseFor(desk.package, QDateTime::currentDateTimeUtc(), desk.kind))};
        Updates after(desk.channel(u"0.1.0"_s), desk.settings, desk.network, desk.folder.filePath(u"downloads"_s));
        QVERIFY2(after.afterSetup().startsWith(said), qPrintable(after.afterSetup()));
        if (said.endsWith(u"Setup's log: "_s)) {
            QCOMPARE(after.afterSetup(), said + QDir::toNativeSeparators(desk.updates.setupLogFor(u"0.2.0"_s)));
        }
        QVERIFY(after.afterSetupWentWrong());
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-handover.json"_s)));
        QTRY_COMPARE(after.state(), Updates::State::Available);
    }

    // Setup's process in its last moments (its mutex let go of, the process not ended yet) when the program starts
    // anew: waited for, and then its log read as above.
    void aSetupInItsLastMomentsIsWaitedForAndItsLogRead()
    {
        Desk desk(UpdateKind::Setup);
        QVERIFY(desk.readyToRun());
        const Environment waits(u"QKVM_STAND_IN_SETUP_WAIT_MS"_s, u"1500"_s);
        const Environment says(u"QKVM_STAND_IN_SETUP_RESULT"_s, u"105 the program was still running"_s);
        QVERIFY(desk.updates.runSetup());
        desk.network.next = {desk.signer.seal(releaseFor(desk.package, QDateTime::currentDateTimeUtc(), desk.kind))};
        Updates after(desk.channel(u"0.1.0"_s), desk.settings, desk.network, desk.folder.filePath(u"downloads"_s));
        const QSignalSpy told(&after, &Updates::afterSetupTold);
        QCOMPARE(after.afterSetup(), u"qKVM Setup is still at work on the update to qKVM 0.2.0. What came of it is "
                                     "said here once it has ended."_s);
        QVERIFY(!after.afterSetupWentWrong());
        QTRY_VERIFY_WITH_TIMEOUT(after.afterSetup().contains(u"qKVM was still running when Setup needed it closed"_s),
                                 20000);
        QVERIFY(after.afterSetupWentWrong());
        QCOMPARE(told.count(), 1);
        QVERIFY(!QFileInfo::exists(desk.folder.filePath(u"data/updates/setup-handover.json"_s)));
    }

    // Setup's log is the person's own text, which anybody of theirs may write: of its last 64 KiB, the last line that
    // holds "qKVM-RESULT: ", as one line of printable signs, at most 200.
    void setupsResultIsReadFromItsLogAsUntrustedText()
    {
        QTemporaryDir folder;
        const QString log = folder.filePath(u"setup-0.2.0.log"_s);
        const auto write = [&log](const QByteArray& bytes) {
            QFile file(log);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        QCOMPARE(setupResultIn(log), QString()); // no log
        QVERIFY(write("2026-11-08 20:34:08.940   Log opened.\r\n2026-11-08 20:34:09.306   Deinitializing Setup.\r\n"));
        QCOMPARE(setupResultIn(log), QString());
        QVERIFY(write("a\r\n2026-11-08 20:34:09.306   qKVM-RESULT: 7 nothing was changed: one\r\nb\r\n"
                      "2026-11-08 20:34:09.307   qKVM-RESULT: 105 the program was still running\r\nLog closed.\r\n"));
        QCOMPARE(setupResultIn(log), u"105 the program was still running"_s);
        // Beyond its last 64 KiB: not read.
        QVERIFY(write("qKVM-RESULT: 105 long ago\n" + QByteArray(qsizetype{70} * 1024, 'x') + "\nLog closed.\n"));
        QCOMPARE(setupResultIn(log), QString());
        // Signs that are not printable go (a bell, a turn of the writing's direction), and so does what is past 200.
        QByteArray odd("qKVM-RESULT: 7 nothing was changed: a");
        odd += '\x07';
        odd += QString(QChar(0x202E)).toUtf8();
        odd += QByteArray(300, 'z');
        QVERIFY(write(odd + "\n"));
        const QString read = setupResultIn(log);
        QCOMPARE(read.size(), 200);
        QVERIFY(read.startsWith(u"7 nothing was changed: az"_s));
        QVERIFY(std::ranges::all_of(read, [](QChar sign) { return sign.isPrint(); }));
    }
    void aHandOverNoteThatDoesNotReadIsLetGo_data()
    {
        QTest::addColumn<QByteArray>("note");
        QTest::newRow("no json") << QByteArray("not json");
        QTest::newRow("no version") << QByteArray(R"({"process":4,"created":"1"})");
        QTest::newRow("a version with a path") << QByteArray(R"({"to":"0.2.0/../x","process":4,"created":"1"})");
        QTest::newRow("no process") << QByteArray(R"({"to":"0.2.0","created":"1"})");
        QTest::newRow("no start time") << QByteArray(R"({"to":"0.2.0","process":4})");
    }

    void aHandOverNoteThatDoesNotReadIsLetGo()
    {
        QFETCH(QByteArray, note);
        Desk desk(UpdateKind::Setup);
        const QString file = desk.folder.filePath(u"data/updates/setup-handover.json"_s);
        QVERIFY(QDir().mkpath(QFileInfo(file).absolutePath()));
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        QCOMPARE(written.write(note), note.size());
        written.close();
        const Updates after(desk.channel(u"0.1.0"_s), desk.settings, desk.network,
                            desk.folder.filePath(u"downloads"_s));
        QVERIFY(after.afterSetup().isEmpty());
        QVERIFY(!QFileInfo::exists(file));
        QCOMPARE(desk.network.requests.size(), 0);
    }

    // ---- The data folder (a start after Setup inherits its mitigations: a junction may not be crossed) ----

    // A data folder that is there but cannot be read is never taken for none: what the person is told says that and
    // where; a folder or settings that are plainly not there are a first start, and one that can be read is read.
    void aDataFolderThatCannotBeReadIsNeverTakenForNone()
    {
        QTemporaryDir root;
        const QString data = root.filePath(u"qKVM Desktop"_s);
        const QString settings = data + u"/settings.json"_s;
        QCOMPARE(whyDataCannotBeRead(settings), QString()); // not there: a first start
        QVERIFY(QDir().mkpath(data));
        QCOMPARE(whyDataCannotBeRead(settings), QString()); // there, empty
        QFile file(settings);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("{}"), 2);
        file.close();
        QCOMPARE(whyDataCannotBeRead(settings), QString()); // there, readable
        // Its list shut to this user: not listed.
        {
            const DenyThisUser denied(data, FILE_LIST_DIRECTORY);
            QVERIFY(denied.ok());
            const QString why = whyDataCannotBeRead(settings);
            QVERIFY2(why.startsWith(u"qKVM cannot read its data in "_s + QDir::toNativeSeparators(data)),
                     qPrintable(why));
            QVERIFY(why.contains(u"Nothing was changed"_s));
        }
        QCOMPARE(whyDataCannotBeRead(settings), QString());
        // The settings themselves shut to this user.
        {
            const DenyThisUser denied(settings, FILE_READ_DATA);
            QVERIFY(denied.ok());
            const QString why = whyDataCannotBeRead(settings);
            QVERIFY2(why.startsWith(u"qKVM cannot read its data in "_s + QDir::toNativeSeparators(settings)),
                     qPrintable(why));
        }

        QCOMPARE(whyDataCannotBeRead(settings), QString());
    }
};

QTEST_GUILESS_MAIN(TestUpdates)
#include "test_updates.moc"
