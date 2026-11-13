#include <qkvm/core/update_cache.h>
#include <qkvm/core/updates.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {
namespace {
// The one name a download is kept under in its own folder of the cache, by its kind.
QString fileFor(UpdateKind kind)
{
    switch (kind) {
    case UpdateKind::Msix:
        return u"qkvm.msix"_s;
    case UpdateKind::Setup:
        return u"qkvm-desktop-setup.exe"_s;
    case UpdateKind::Portable:
        break;
    }
    return u"qkvm.zip"_s;
}

std::wstring native(const QString& path)
{
    return QDir::toNativeSeparators(path).toStdWString();
}

// Whoever runs this program, as Windows' token says it (a TOKEN_USER); empty where it does not say.
std::vector<unsigned char> userOfThisProgram()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    if (size == 0 || GetTokenInformation(token, TokenUser, user.data(), size, &size) == FALSE) {
        user.clear();
    }
    CloseHandle(token);
    return user;
}

PSID sidIn(std::vector<unsigned char>& user)
{
    return reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
}

// What a folder that a Setup is downloaded into, run from, or unpacks itself into is kept to (and the cache's Downloads
// folder that holds them): this user, SYSTEM and the administrators, all of it, and nobody else; and nothing taken from
// the folders above (a profile's folders may let a sandbox's users or an app's capability write into everything below
// them: on a PC this was built on they do). What is made in it takes the same. Null where Windows does not say who runs
// this program; freed with LocalFree.
PSECURITY_DESCRIPTOR keptToThisUser()
{
    auto user = userOfThisProgram();
    LPWSTR text = nullptr;
    if (user.empty() || ConvertSidToStringSidW(sidIn(user), &text) == FALSE) {
        return nullptr;
    }
    const std::wstring list = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(text) + L")";
    LocalFree(text);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(list.c_str(), SDDL_REVISION_1, &descriptor, nullptr) !=
                   FALSE
               ? descriptor
               : nullptr;
}

// A folder that is there already (the cache's Downloads) kept so from now on.
bool keepToThisUser(const QString& folder)
{
    PSECURITY_DESCRIPTOR descriptor = keptToThisUser();
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL entries = nullptr;
    std::wstring path = native(folder);
    const bool kept = descriptor != nullptr &&
                      GetSecurityDescriptorDacl(descriptor, &present, &entries, &defaulted) != FALSE &&
                      present != FALSE &&
                      SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                            nullptr, entries, nullptr) == ERROR_SUCCESS;
    LocalFree(descriptor);
    return kept;
}

// A new folder in `cache`, `prefix` and six letters or digits, made by this call (never one that was there), and
// where `kept`, made with the list keptToThisUser gives, in the one call that makes it: there is no moment at which it
// takes what the cache would give it. Its path; empty where none could be made.
QString newCacheFolder(const QString& cache, const QString& prefix, bool kept)
{
    PSECURITY_DESCRIPTOR descriptor = kept ? keptToThisUser() : nullptr;
    if (kept && descriptor == nullptr) {
        return {};
    }
    SECURITY_ATTRIBUTES made{};
    made.nLength = sizeof(made);
    made.lpSecurityDescriptor = descriptor;
    static constexpr char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    QString folder;
    for (int tried = 0; tried < 16 && folder.isEmpty(); ++tried) {
        QString name = prefix;
        for (int at = 0; at < 6; ++at) {
            name += QChar::fromLatin1(letters[QRandomGenerator::system()->bounded(int(sizeof(letters) - 1))]);
        }
        const QString path = QDir(cache).filePath(name);
        if (CreateDirectoryW(native(path).c_str(), kept ? &made : nullptr) != FALSE) {
            folder = path;
        } else if (GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    LocalFree(descriptor);
    return folder;
}

// The cache's Downloads kept so: made so where it is not there yet (its parents as the cache makes them), and given the
// list from now on where it is.
bool keepFolderToThisUser(const QString& folder)
{
    if (QFileInfo(folder).isDir()) {
        return keepToThisUser(folder);
    }
    PSECURITY_DESCRIPTOR descriptor = keptToThisUser();
    SECURITY_ATTRIBUTES made{};
    made.nLength = sizeof(made);
    made.lpSecurityDescriptor = descriptor;
    const bool kept = descriptor != nullptr && QDir().mkpath(QFileInfo(folder).absolutePath()) &&
                      CreateDirectoryW(native(folder).c_str(), &made) != FALSE;
    LocalFree(descriptor);
    return kept;
}

// This program's environment, as Setup is given it: every variable as it is, but TEMP and TMP, which name `temp`.
// Each "NAME=value" ends in a zero and the block in one more, in the order of their names, as Windows asks.
std::vector<wchar_t> environmentWithTemp(const std::wstring& temp)
{
    std::vector<std::wstring> lines;
    if (LPWCH all = GetEnvironmentStringsW(); all != nullptr) {
        for (const wchar_t* at = all; *at != L'\0'; at += std::wcslen(at) + 1) {
            const std::wstring line(at);
            const std::wstring name = line.substr(0, line.find(L'=', 1));
            if (CompareStringOrdinal(name.c_str(), -1, L"TEMP", -1, TRUE) != CSTR_EQUAL &&
                CompareStringOrdinal(name.c_str(), -1, L"TMP", -1, TRUE) != CSTR_EQUAL) {
                lines.push_back(line);
            }
        }
        FreeEnvironmentStringsW(all);
    }
    lines.push_back(L"TEMP=" + temp);
    lines.push_back(L"TMP=" + temp);
    std::ranges::stable_sort(lines, [](const std::wstring& one, const std::wstring& other) {
        const std::wstring first = one.substr(0, one.find(L'=', 1));
        const std::wstring second = other.substr(0, other.find(L'=', 1));
        return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for (const std::wstring& line : lines) {
        block.insert(block.end(), line.begin(), line.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
// Whether nobody but this user, SYSTEM and the administrators may change a folder: one of them owns it, and its list
// of who may do what lets nobody else add to it, take from it, or change the list or the owner (also of what is made in
// it). An entry of a kind this does not read counts against it, and so does no list at all (everybody may do
// everything).
bool onlyThisUserMayChange(const QString& folder)
{
    auto user = userOfThisProgram();
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system{};
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> administrators{};
    DWORD size = static_cast<DWORD>(system.size());
    if (user.empty() || CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &size) == FALSE) {
        return false;
    }
    size = static_cast<DWORD>(administrators.size());
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size) == FALSE) {
        return false;
    }
    const auto trusted = [&](PSID sid) {
        return EqualSid(sid, sidIn(user)) != FALSE || EqualSid(sid, system.data()) != FALSE ||
               EqualSid(sid, administrators.data()) != FALSE;
    };
    PSID owner = nullptr;
    PACL entries = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring path = native(folder);
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &owner, nullptr, &entries, nullptr, &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    constexpr ACCESS_MASK changes = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_WRITE_EA | FILE_DELETE_CHILD |
                                    FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE |
                                    GENERIC_ALL;
    bool only = owner != nullptr && trusted(owner) && entries != nullptr;
    for (DWORD index = 0; only && index < entries->AceCount; ++index) {
        void* entry = nullptr;
        if (GetAce(entries, index, &entry) == FALSE) {
            only = false;
        } else if (const auto* header = static_cast<const ACE_HEADER*>(entry);
                   header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            only = (allowed->Mask & changes) == 0 || trusted(&allowed->SidStart);
        } else {
            only = header->AceType == ACCESS_DENIED_ACE_TYPE;
        }
    }
    LocalFree(descriptor);
    return only;
}

// The whole name of the file a handle holds, through no junction or link: each folder in it holds the file, and none
// of them can be renamed or taken away while the handle is open (a junction on the way could be, and point elsewhere
// at the next opening). Empty where Windows does not say it as a name on a drive.
QString finalPathOf(HANDLE file)
{
    std::vector<wchar_t> name(32768);
    const DWORD length = GetFinalPathNameByHandleW(file, name.data(), static_cast<DWORD>(name.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= name.size()) {
        return {};
    }
    const QString path = QString::fromWCharArray(name.data(), static_cast<qsizetype>(length));
    const QString prefix = u"\\\\?\\"_s;
    if (!path.startsWith(prefix) || path.size() < prefix.size() + 3 || path.at(prefix.size() + 1) != u':') {
        return {};
    }
    return QDir::fromNativeSeparators(path.mid(prefix.size()));
}

// The whole name of a folder that is there, as Windows says it through no junction or link (finalPathOf); empty where
// it is not there or not said so.
QString finalFolderPath(const QString& folder)
{
    const HANDLE held = CreateFileW(native(folder).c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (held == INVALID_HANDLE_VALUE) {
        return {};
    }
    const QString said = finalPathOf(held);
    CloseHandle(held);
    return said;
}

// Whether what a handle holds is `bytes` long and has that SHA-256, read through the handle from its start.
bool readsAs(HANDLE file, qint64 bytes, const QByteArray& sha256)
{
    LARGE_INTEGER size{};
    const LARGE_INTEGER start{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart != bytes ||
        SetFilePointerEx(file, start, nullptr, FILE_BEGIN) == FALSE) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(64LL * 1024, Qt::Uninitialized);
    qint64 read = 0;
    for (;;) {
        DWORD got = 0;
        if (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) == FALSE) {
            return false;
        }
        if (got == 0) {
            break;
        }
        read += got;
        hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(got)));
    }
    return read == bytes && hash.result() == sha256;
}

quint64 whole(const FILETIME& time)
{
    return (static_cast<quint64>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
}

// A path on a drive (C:\...), whole: no quote and no sign below a space in it, and no backslash at its end.
QString quotedForSetup(const QString& path)
{
    const QString spelled = QDir::toNativeSeparators(path);
    const bool onADrive =
        spelled.size() > 3 &&
        ((spelled.at(0) >= u'A' && spelled.at(0) <= u'Z') || (spelled.at(0) >= u'a' && spelled.at(0) <= u'z')) &&
        spelled.at(1) == u':' && spelled.at(2) == u'\\';
    if (!onADrive || spelled.endsWith(u'\\') ||
        std::ranges::any_of(spelled, [](QChar sign) { return sign == u'"' || sign.unicode() < 0x20; })) {
        return {};
    }
    return u'"' + spelled + u'"';
}
} // namespace

// A download's own folder of the cache: taken away with what is in it when let go of, unless it is to be kept.
class DownloadFolder {
public:
    explicit DownloadFolder(QString path) : m_path(std::move(path)) {}
    ~DownloadFolder()
    {
        if (m_remove && !m_path.isEmpty()) {
            QDir(m_path).removeRecursively();
        }
    }
    DownloadFolder(const DownloadFolder&) = delete;
    DownloadFolder& operator=(const DownloadFolder&) = delete;
    DownloadFolder(DownloadFolder&&) = delete;
    DownloadFolder& operator=(DownloadFolder&&) = delete;

    bool isValid() const { return !m_path.isEmpty(); }
    QString path() const { return m_path; }
    QString filePath(const QString& name) const { return QDir(m_path).filePath(name); }
    void setAutoRemove(bool remove) { m_remove = remove; }

private:
    QString m_path;
    bool m_remove = true;
};

QString setupResultIn(const QString& log)
{
    QFile file(log);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    constexpr qint64 tail = 64LL * 1024;
    if (file.size() > tail && !file.seek(file.size() - tail)) {
        return {};
    }
    const QString read = QString::fromUtf8(file.read(tail));
    const QString mark = u"qKVM-RESULT: "_s;
    const QStringList lines = read.split(u'\n');
    for (auto line = lines.crbegin(); line != lines.crend(); ++line) {
        const qsizetype at = line->indexOf(mark);
        if (at < 0) {
            continue;
        }
        // Printable signs only (no control, no sign that turns the writing's direction), at most 200.
        QList<char32_t> kept;
        for (const char32_t sign : line->mid(at + mark.size()).toUcs4()) {
            if (kept.size() < 200 && QChar::isPrint(sign)) {
                kept.append(sign);
            }
        }
        return QString::fromUcs4(kept.constData(), kept.size()).trimmed();
    }
    return {};
}

QString setupCommandLine(const QString& setup, const QString& programFolder, const QString& log)
{
    const QString file = quotedForSetup(setup);
    const QString folder = quotedForSetup(programFolder);
    const QString at = quotedForSetup(log);
    if (file.isEmpty() || folder.isEmpty() || at.isEmpty()) {
        return {};
    }
    return file + u" /SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CURRENTUSER /UPDATE=1 /DIR="_s + folder +
           u" /LOG="_s + at;
}

Updates::Updates(UpdateChannel channel, Settings& settings, QNetworkAccessManager& network, QString cacheFolder,
                 QObject* parent)
    : QObject(parent), m_channel(std::move(channel)), m_settings(settings), m_network(network),
      m_cacheFolder(std::move(cacheFolder))
{
    // Straight to the feed, as every line of the program goes: no proxy the application or Windows names
    // (WPAD, PAC). TLS and the feed's signature hold either way; this keeps the checks from being seen
    // or held by whatever answers proxy discovery on the network.
    m_network.setProxy(QNetworkProxy::NoProxy);
    // Old downloads of every kind (a copy may have been another kind before).
    for (const auto kind : {UpdateKind::Msix, UpdateKind::Portable, UpdateKind::Setup}) {
        pruneUpdateCache(m_cacheFolder, u"update-"_s, fileFor(kind), QDateTime::currentDateTimeUtc());
    }
    // And Setup's own TEMP folders that were left, a week on.
    pruneUpdateCache(m_cacheFolder, u"setup-temp-"_s, QString(), QDateTime::currentDateTimeUtc());
    m_state = configured() ? State::Idle : State::Unavailable;
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this,
            [this] { fail(tr("The update request timed out. Try again later.")); });
    lookAtHandOver();
}

Updates::~Updates()
{
    stopRequest();
    discardPackage();
    if (m_setupProcess != nullptr) {
        m_setupEnd.reset();
        CloseHandle(m_setupProcess);
    }
}

bool Updates::configured() const
{
    return updateAddress(m_channel.feed) && m_channel.publicKey.size() == 64 &&
           !updateVersion(m_channel.currentVersion).isNull();
}

void Updates::setAutomatic(bool on)
{
    auto remembered = m_settings.remembered();
    if (remembered.checkUpdates == on) {
        return;
    }
    remembered.checkUpdates = on;
    if (!m_settings.remember(remembered)) {
        m_problem = tr("This choice could not be saved. It applies until qKVM closes.");
    }
    emit changed();
}

void Updates::checkIfDue(const QDateTime& now)
{
    if (automatic() && now.isValid() && !busy() && m_state != State::Ready &&
        (!m_lastAttempt.isValid() || now < m_lastAttempt || m_lastAttempt.secsTo(now) >= 86400)) {
        check();
        m_lastAttempt = now;
    }
}

void Updates::check()
{
    if (!configured() || busy() || m_state == State::Ready) {
        return;
    }
    m_release.reset();
    m_metadata.clear();
    m_problem.clear();
    m_lastAttempt = QDateTime::currentDateTimeUtc();
    m_state = State::Checking;
    request(m_channel.feed, false);
    emit changed();
}

void Updates::request(const QUrl& url, bool package)
{
    m_package = package;
    m_received = 0;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("User-Agent", "qKVM-Desktop-Updater");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("Accept", package ? "application/octet-stream" : "application/json");
    request.setTransferTimeout(15000);
    m_reply = m_network.get(request);
    m_reply->setReadBufferSize(128LL * 1024);
    connect(m_reply, &QNetworkReply::readyRead, this, &Updates::takeBytes);
    connect(m_reply, &QNetworkReply::finished, this, &Updates::finished);
    m_deadline.start(package ? 15 * 60 * 1000 : 30000);
}

void Updates::download()
{
    if (m_state != State::Available || !m_release) {
        return;
    }
    if (m_release->expires <= QDateTime::currentDateTimeUtc()) {
        fail(tr("This update information has expired. Check for updates again."));
        return;
    }
    discardPackage();
    // A Setup is run from its folder: nobody but this user (and SYSTEM and the administrators) may change it, nor the
    // cache's Downloads that holds it, and the folder is made so.
    const bool kept = m_channel.kind == UpdateKind::Setup;
    if (!(kept ? keepFolderToThisUser(m_cacheFolder) : QDir().mkpath(m_cacheFolder))) {
        fail(tr("The update download folder could not be created."));
        return;
    }
    m_folder = std::make_unique<DownloadFolder>(newCacheFolder(m_cacheFolder, u"update-"_s, kept));
    if (m_folderMade) {
        m_folderMade(m_folder->path());
    }
    if (!m_folder->isValid()) {
        fail(tr("The update download folder could not be created."));
        return;
    }
    m_file.setFileName(m_folder->filePath(fileFor(m_channel.kind)));
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail(tr("The update package could not be saved."));
        return;
    }
    m_hash.reset();
    m_problem.clear();
    m_state = State::Downloading;
    request(m_release->package, true);
    emit changed();
}

void Updates::takeBytes()
{
    if (m_reply == nullptr) {
        return;
    }
    if (m_package && !m_release) {
        fail(tr("The update information is no longer available. Check again."));
        return;
    }
    const qint64 limit = m_package && m_release ? m_release->bytes : mostUpdateMetadata;
    const auto length = m_reply->header(QNetworkRequest::ContentLengthHeader);
    if (length.isValid() && length.toLongLong() > limit) {
        fail(tr("The update download is larger than expected."));
        return;
    }
    while (m_reply != nullptr && m_reply->bytesAvailable() > 0) {
        const QByteArray bytes = m_reply->read(64LL * 1024);
        if (bytes.isEmpty()) {
            break;
        }
        if (m_received > limit - bytes.size()) {
            fail(tr("The update download is larger than expected."));
            return;
        }
        m_received += bytes.size();
        if (m_package) {
            if (m_file.write(bytes) != bytes.size()) {
                fail(tr("The update package could not be saved. Check the free disk space."));
                return;
            }
            m_hash.addData(bytes);
        } else {
            m_metadata.append(bytes);
        }
    }
    emit changed();
}

void Updates::finished()
{
    if (m_reply == nullptr) {
        return;
    }
    if (m_reply->error() != QNetworkReply::NoError ||
        m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
        m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
        fail(tr("The update service could not be reached securely. Try again later."));
        return;
    }
    auto* const finishing = m_reply;
    takeBytes();
    // A progress observer may cancel this request and start another synchronously.
    // Never finish, abort or interpret the replacement on behalf of the old reply.
    if (m_reply != finishing) {
        return;
    }
    stopRequest();
    if (m_package) {
        if (!m_release || m_received != m_release->bytes || m_hash.result() != m_release->sha256 || !m_file.flush()) {
            fail(tr("The downloaded update failed verification. Nothing was installed."));
            return;
        }
        m_file.close();
        m_state = State::Ready;
    } else {
        const auto now = QDateTime::currentDateTimeUtc();
        const auto verified = verifiedUpdate(m_metadata, m_channel.publicKey, now, m_channel.kind);
        m_metadata.clear();
        if (!verified) {
            fail(tr("The update information is invalid, expired or not signed by qKVM."));
            return;
        }
        auto remembered = m_settings.remembered();
        remembered.lastUpdateCheck = now;
        if (!m_settings.remember(remembered)) {
            m_problem = tr("The check succeeded, but its date could not be saved.");
        }
        if (updateVersion(verified.value().version) > updateVersion(m_channel.currentVersion)) {
            m_release = verified.value();
            m_state = State::Available;
            if (m_notifiedVersion != m_release->version) {
                m_notifiedVersion = m_release->version;
                emit available();
            }
        } else {
            m_state = State::Current;
        }
    }
    emit changed();
}

void Updates::stopRequest()
{
    m_deadline.stop();
    if (m_reply != nullptr) {
        auto* reply = std::exchange(m_reply, nullptr);
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void Updates::discardPackage()
{
    m_file.close();
    if (m_packageLock != nullptr) {
        CloseHandle(m_packageLock);
        m_packageLock = nullptr;
    }
    m_folder.reset();
    m_file.setFileName(QString());
}

void Updates::fail(const QString& message)
{
    stopRequest();
    discardPackage();
    m_metadata.clear();
    m_problem = message;
    m_state = State::Error;
    emit changed();
}

void Updates::cancel()
{
    if (!busy() && m_state != State::Ready) {
        return;
    }
    stopRequest();
    discardPackage();
    m_metadata.clear();
    m_problem.clear();
    m_received = 0;
    m_state = m_release ? State::Available : State::Idle;
    emit changed();
}

bool Updates::locked()
{
    // Hold a read handle that denies writing and replacement across verification and handoff.
    if (m_packageLock == nullptr) {
        const auto path = native(m_file.fileName());
        HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION information{};
        if (locked == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(locked, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (locked != INVALID_HANDLE_VALUE) {
                CloseHandle(locked);
            }
            return false;
        }
        m_packageLock = locked;
    }
    return true;
}

bool Updates::verifiedAgain()
{
    if (!m_release) {
        return false;
    }
    if (!locked()) {
        fail(tr("The saved update could not be secured. Download it again."));
        return false;
    }
    // Read through the handle that is held, and so of the file that is then handed over.
    if (m_release->expires <= QDateTime::currentDateTimeUtc() ||
        !readsAs(m_packageLock, m_release->bytes, m_release->sha256)) {
        fail(tr("The saved update failed verification. Check for updates and download it again."));
        return false;
    }
    return true;
}

bool Updates::openInstaller(const std::function<bool(const QString&)>& open)
{
    // An MSIX copy only: a portable copy's ZIP is never handed to an installer.
    if (m_channel.kind != UpdateKind::Msix || m_state != State::Ready || !m_release || !open) {
        return false;
    }
    // Windows' MSIX installer separately verifies the package's own signature and identity.
    if (!verifiedAgain()) {
        return false;
    }
    if (!open(m_file.fileName())) {
        CloseHandle(m_packageLock);
        m_packageLock = nullptr;
        m_problem = tr("qKVM Setup could not be opened. Use the complete installer folder, then try again.");
        emit changed();
        return false;
    }
    // App Installer may still be reading after qKVM exits. Leave this unique folder for it;
    // the next release must not depend on the old process's lifetime to finish installation.
    m_folder->setAutoRemove(false);
    m_problem.clear();
    return true;
}

bool Updates::showFile(const std::function<bool(const QString&)>& show)
{
    // A portable copy only: an MSIX is installed, never handed to the person to unpack.
    if (m_channel.kind != UpdateKind::Portable || m_state != State::Ready || !m_release || !show) {
        return false;
    }
    if (!verifiedAgain()) {
        return false;
    }
    if (!show(m_file.fileName())) {
        CloseHandle(m_packageLock);
        m_packageLock = nullptr;
        m_problem = tr("The folder could not be opened. The verified file is %1")
                        .arg(QDir::toNativeSeparators(m_file.fileName()));
        emit changed();
        return false;
    }
    // The person unpacks it, most likely after qKVM has closed: it stays (the cache's pruning takes it after seven
    // days), held against writing until then.
    m_folder->setAutoRemove(false);
    m_problem.clear();
    return true;
}

bool Updates::runSetup()
{
    // A Setup copy only: a portable copy's ZIP is never run, and an MSIX goes to Windows' installer.
    if (m_channel.kind != UpdateKind::Setup || m_state != State::Ready || !m_release) {
        return false;
    }
    if (!locked() || m_release->expires <= QDateTime::currentDateTimeUtc() ||
        !readsAs(m_packageLock, m_release->bytes, m_release->sha256)) {
        // Not the file that was verified (changed, swapped, or not to be held): removed, and downloaded anew.
        discardPackage();
        m_state = State::Available;
        download();
        if (m_state == State::Downloading) {
            m_problem = tr("The saved update was not the file that was verified. It was removed and is downloaded "
                           "again: install it once that is done.");
            emit changed();
        }
        return false;
    }
    // Setup is started from the file the handle holds, by its whole name through no junction: none of the folders of
    // that name can be renamed or taken away while the handle is open, and so it is the file that was just verified.
    const QString file = finalPathOf(m_packageLock);
    const QString folder = QFileInfo(file).absolutePath();
    const QStringList beside =
        QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    if (file.isEmpty() || beside != QStringList{QFileInfo(file).fileName()} || !onlyThisUserMayChange(folder)) {
        fail(tr("The saved update does not lie alone in a folder that only you can change. Nothing was installed. "
                "Download it again."));
        return false;
    }
    // Setup installs into the folder the program runs from, which must still be the one Setup installed.
    if (updateKindOf(m_channel.programFolder, false) != UpdateKind::Setup) {
        fail(tr("qKVM's folder no longer holds the mark of qKVM Setup. Nothing was installed."));
        return false;
    }
    // Every path Setup is given is the final one, as Windows says it of the folder, through no junction: Setup runs
    // with Inno's RedirectionGuard and crosses no junction the person made (a data folder moved behind one), and would
    // make no log there and give up. Still refused, so resolved, with a quote, a sign below a space or a backslash at
    // its end (setupCommandLine).
    const QString logFolder = QFileInfo(setupLogFor(m_release->version)).absolutePath();
    const QString programFolder = finalFolderPath(m_channel.programFolder);
    const QString logIn = QDir().mkpath(logFolder) ? finalFolderPath(logFolder) : QString();
    const QString log =
        logIn.isEmpty() ? QString() : logIn + u'/' + QFileInfo(setupLogFor(m_release->version)).fileName();
    const QString line = setupCommandLine(file, programFolder, log);
    if (programFolder.isEmpty() || log.isEmpty() || line.isEmpty()) {
        fail(tr("qKVM Setup cannot be told where qKVM is, or where to write its log. Nothing was installed."));
        return false;
    }
    // What came of this attempt is read from its log at the next start: one of an earlier attempt at this version goes
    // first, so that a Setup that ends before it writes its own (its stub could not unpack itself) leaves no earlier
    // word standing. Where it cannot be taken away, Setup is not run.
    if (QFileInfo::exists(log) && !QFile::remove(log)) {
        fail(tr("Setup's log of an earlier attempt could not be removed. Nothing was installed. Close what holds %1, "
                "then try again.")
                 .arg(QDir::toNativeSeparators(log)));
        return false;
    }
    // Setup unpacks itself (Inno Setup's setup.tmp) into its TEMP: a folder of its own, kept as its folder is, and not
    // the person's Temp, which others may be let write into.
    const QString madeTemp = newCacheFolder(m_cacheFolder, u"setup-temp-"_s, true);
    const QString temp = madeTemp.isEmpty() ? QString() : finalFolderPath(madeTemp);
    if (temp.isEmpty() || quotedForSetup(temp).isEmpty()) {
        fail(tr("qKVM Setup's own temporary folder could not be made. Nothing was installed."));
        return false;
    }
    std::vector<wchar_t> environment = environmentWithTemp(native(temp));
    if (m_beforeSetup) {
        m_beforeSetup(file);
    }
    // Suspended until the hand-over is noted; no handle of this program's goes with it.
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION made{};
    const std::wstring program = native(file);
    std::wstring command = line.toStdWString();
    const std::wstring where = native(folder);
    if (CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(), where.c_str(), &start,
                       &made) == FALSE) {
        QDir(m_cacheFolder).rmdir(QFileInfo(temp).fileName());
        fail(tr("qKVM Setup could not be started. Nothing was installed."));
        return false;
    }
    FILETIME created{};
    FILETIME ended{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(made.hProcess, &created, &ended, &kernel, &user) == FALSE ||
        !noteHandOver(made.dwProcessId, whole(created), QFileInfo(temp).fileName()) ||
        ResumeThread(made.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(made.hProcess, 1);
        CloseHandle(made.hThread);
        CloseHandle(made.hProcess);
        QFile::remove(handOverFile());
        fail(tr("The update could not be noted beside qKVM's settings. Nothing was installed."));
        return false;
    }
    CloseHandle(made.hThread);
    CloseHandle(made.hProcess);
    // Setup runs from this folder: it stays (the cache's pruning takes it after seven days), and the file stays held
    // against writing, renaming and deletion until this program ends.
    m_folder->setAutoRemove(false);
    m_problem.clear();
    emit changed();
    return true;
}

QString Updates::setupLogFor(const QString& version) const
{
    return QDir(m_channel.dataFolder).filePath(u"updates/setup-"_s + version + u".log"_s);
}

QString Updates::handOverFile() const
{
    return m_channel.dataFolder.isEmpty() ? QString()
                                          : QDir(m_channel.dataFolder).filePath(u"updates/setup-handover.json"_s);
}

bool Updates::noteHandOver(unsigned long process, quint64 created, const QString& temp)
{
    const QString file = handOverFile();
    if (file.isEmpty() || !m_release) {
        return false;
    }
    const QJsonObject said{{u"schema"_s, 1},
                           {u"from"_s, m_channel.currentVersion},
                           {u"to"_s, m_release->version},
                           {u"process"_s, static_cast<qint64>(process)},
                           {u"created"_s, QString::number(created)},
                           {u"temp"_s, temp},
                           {u"at"_s, QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    QSaveFile note(file);
    return note.open(QIODevice::WriteOnly) && note.write(QJsonDocument(said).toJson(QJsonDocument::Compact)) > 0 &&
           note.commit();
}

void Updates::lookAtHandOver()
{
    const QString file = handOverFile();
    if (m_channel.kind != UpdateKind::Setup || file.isEmpty() || !QFileInfo::exists(file)) {
        return;
    }
    QJsonObject said;
    if (QFile noted(file); noted.open(QIODevice::ReadOnly)) {
        said = QJsonDocument::fromJson(noted.read(4096)).object();
    }
    const QString version = said.value(u"to"_s).toString();
    const qint64 process = said.value(u"process"_s).toInteger(-1);
    bool numbered = false;
    const quint64 created = said.value(u"created"_s).toString().toULongLong(&numbered);
    if (updateVersion(version).isNull() || process <= 0 || process > 0xFFFFFFFFLL || !numbered) {
        QFile::remove(file);
        return;
    }
    // Setup's own TEMP, let go of once Setup is over (only where it is that and empty: Setup takes away what it
    // unpacked).
    m_setupTemp = said.value(u"temp"_s).toString();
    if (updateVersion(version) == updateVersion(m_channel.currentVersion)) {
        QFile::remove(file);
        letGoOfSetupsTemp();
        tellAfterSetup(tr("Updated to qKVM %1.").arg(version), false, false);
        return;
    }
    // Setup may still be at work: the process that was started (by its start time, not only its number), not ended.
    HANDLE setup = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process));
    FILETIME began{};
    FILETIME ended{};
    FILETIME kernel{};
    FILETIME user{};
    if (setup != nullptr && GetProcessTimes(setup, &began, &ended, &kernel, &user) != FALSE &&
        whole(began) == created && WaitForSingleObject(setup, 0) == WAIT_TIMEOUT) {
        m_setupProcess = setup;
        m_setupVersion = version;
        m_setupEnd = std::make_unique<QWinEventNotifier>(setup);
        connect(m_setupEnd.get(), &QWinEventNotifier::activated, this, &Updates::setupEnded);
        tellAfterSetup(
            tr("qKVM Setup is still at work on the update to qKVM %1. What came of it is said here once it has "
               "ended.")
                .arg(version),
            false, false);
        return;
    }
    if (setup != nullptr) {
        CloseHandle(setup);
    }
    QFile::remove(file);
    letGoOfSetupsTemp();
    tellWhatSetupSaid(version);
}

void Updates::setupEnded()
{
    m_setupEnd.reset();
    CloseHandle(m_setupProcess);
    m_setupProcess = nullptr;
    QFile::remove(handOverFile());
    letGoOfSetupsTemp();
    tellWhatSetupSaid(std::exchange(m_setupVersion, QString()));
}

void Updates::tellWhatSetupSaid(const QString& version)
{
    const QString log = QDir::toNativeSeparators(setupLogFor(version));
    const QString said = setupResultIn(setupLogFor(version));
    const QString running = QString::number(setupFoundQkvmRunning);
    const QString unchanged = QString::number(setupChangedNothing);
    const auto is = [&said](const QString& code) {
        return said.startsWith(code) && (said.size() == code.size() || said.at(code.size()) == u' ');
    };
    if (is(running)) {
        tellAfterSetup(tr("The update to qKVM %1 was not installed: qKVM was still running when Setup needed it "
                          "closed, so Setup changed nothing. Choose Install and restart again.")
                           .arg(version),
                       true, true);
    } else if (is(unchanged)) {
        // Setup's words after "nothing was changed:", where it says them (shown as text, nothing else).
        const qsizetype colon = said.indexOf(u':');
        const QString why = colon >= 0 ? said.mid(colon + 1).trimmed() : QString();
        tellAfterSetup(why.isEmpty()
                           ? tr("The update to qKVM %1 was not installed, and nothing was changed. Setup's log: %2")
                                 .arg(version, log)
                           : tr("The update to qKVM %1 was not installed, and nothing was changed: %2. Setup's log: %3")
                                 .arg(version, why, log),
                       true, true);
    } else {
        tellAfterSetup(tr("The update to qKVM %1 was not installed. Setup's log: %2").arg(version, log), true, true);
    }
}

void Updates::letGoOfSetupsTemp()
{
    static const QRegularExpression named(u"\\Asetup-temp-[A-Za-z0-9]{6}\\z"_s);
    if (named.match(m_setupTemp).hasMatch()) {
        QDir(m_cacheFolder).rmdir(m_setupTemp);
    }
    m_setupTemp.clear();
}

void Updates::tellAfterSetup(const QString& text, bool wrong, bool offerAgain)
{
    m_afterSetup = text;
    m_afterSetupWentWrong = wrong;
    emit afterSetupTold();
    emit changed();
    if (offerAgain) {
        // The update is offered again: checked for anew (its download still waits for the person).
        QTimer::singleShot(0, this, [this] { check(); });
    }
}

} // namespace qkvm::core
