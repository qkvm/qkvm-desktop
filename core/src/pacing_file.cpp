#include <qkvm/core/pacing_file.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <span>
#include <system_error>
#include <utility>

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QThreadPool>

#include <qkvm/core/sealing.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// What the sealed part is sealed for: whoever opens it names it again (sealing.h).
constexpr QByteArrayView sealedFor = "qkvm sign-in pacing 1";
// The most of a file read back: far more than SignInPacing keeps.
constexpr qint64 largestFile = qint64{16} * 1024 * 1024;
// The most noes or silent ones in a row read back: more than any pacing counts in a day.
constexpr int mostInARow = 100'000;
constexpr int mostPutAside = 10;

// One as it is written down: its mark, its counts, and until when it waits on the PC's clock.
struct Line {
    SignInPacing::Mark mark;
    int noes = 0;
    int silent = 0;
    qint64 untilMs = 0;
};

QByteArray contentOf(const QList<Line>& lines)
{
    QJsonArray kept;
    for (const Line& one : lines) {
        kept.append(QJsonObject{{u"digest"_s, QString::fromLatin1(one.mark.digest.toHex())},
                                {u"road"_s, one.mark.road},
                                {u"address"_s, one.mark.address},
                                {u"user"_s, one.mark.user},
                                {u"noes"_s, one.noes},
                                {u"silent"_s, one.silent},
                                {u"until"_s, static_cast<double>(one.untilMs)}});
    }
    return QJsonDocument(QJsonObject{{u"kept"_s, kept}}).toJson(QJsonDocument::Compact);
}

// A whole number in a JSON value, within [0, most]; none otherwise.
std::optional<qint64> wholeIn(const QJsonValue& value, qint64 most)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number = value.toDouble();
    if (!(number >= 0) || number > static_cast<double>(most) || number != static_cast<double>(qint64(number))) {
        return std::nullopt;
    }
    return static_cast<qint64>(number);
}

// What was written down, every line of it whole; none where anything of it is not.
std::optional<QList<Line>> linesIn(QByteArrayView content)
{
    QJsonParseError error{};
    const QJsonDocument read = QJsonDocument::fromJson(content.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !read.isObject() || !read.object().value("kept"_L1).isArray()) {
        return std::nullopt;
    }
    QList<Line> lines;
    const QJsonArray all = read.object().value("kept"_L1).toArray();
    for (const QJsonValue each : all) {
        const QJsonObject one = each.toObject();
        const QByteArray digest = QByteArray::fromHex(one.value("digest"_L1).toString().toLatin1());
        const std::optional<qint64> noes = wholeIn(one.value("noes"_L1), mostInARow);
        const std::optional<qint64> silent = wholeIn(one.value("silent"_L1), mostInARow);
        const std::optional<qint64> until = wholeIn(one.value("until"_L1), qint64{1} << 52);
        if (!each.isObject() || digest.size() != 32 || one.value("digest"_L1).toString().size() != 64 || !noes ||
            !silent || !until || !one.value("road"_L1).isString() || !one.value("address"_L1).isString() ||
            !one.value("user"_L1).isString()) {
            return std::nullopt;
        }
        Line line;
        line.mark = SignInPacing::Mark(digest);
        line.mark.road = one.value("road"_L1).toString();
        line.mark.address = one.value("address"_L1).toString();
        line.mark.user = one.value("user"_L1).toString();
        line.noes = static_cast<int>(*noes);
        line.silent = static_cast<int>(*silent);
        line.untilMs = *until;
        lines.append(std::move(line));
    }
    return lines;
}

// What the pacing keeps, as it is written down, `at` being now on the PC's clock.
QList<Line> linesOf(const QList<SignInPacing::Saved>& kept, qint64 at)
{
    QList<Line> lines;
    lines.reserve(kept.size());
    for (const SignInPacing::Saved& one : kept) {
        lines.append(Line{one.mark, one.noes, one.silent, at + one.leftMs});
    }
    return lines;
}

// The file written: sealed under `key`, and put in the place of the one before as a whole (QSaveFile).
void writeDown(const QString& file, const QList<Line>& lines, const SecretKey& key)
{
    std::array<quint8, sealing::nonceBytes> nonce{};
    bool done = false;
    if (sealing::random(nonce)) {
        const QByteArray content = contentOf(lines);
        const base::Result<QByteArray> sealed =
            sealing::seal(key, QByteArrayView(nonce.data(), nonce.size()), content, sealedFor);
        if (sealed) {
            const QJsonObject top{
                {u"version"_s, PacingFile::version},
                {u"nonce"_s,
                 QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(nonce.data()), nonce.size()).toBase64())},
                {u"sealed"_s, QString::fromLatin1(sealed.value().toBase64())}};
            QSaveFile out(file);
            done = out.open(QIODevice::WriteOnly) &&
                   out.write(QJsonDocument(top).toJson(QJsonDocument::Compact)) >= 0 && out.commit();
        }
    }
    if (!done) {
        qWarning("qKVM: the wrong-password counts could not be written down: they are kept in memory, and "
                 "written at the next change");
    }
}

} // namespace

// One writing under way on a thread of the pool, and whether it has ended: waited for as this goes.
struct PacingFile::Writing {
    std::mutex mutex;
    std::condition_variable ended;
    bool done = false;
};

PacingFile::PacingFile(SignInPacing& pacing, QString file, SecretStore& secrets, Pace pace, QObject* parent)
    : QObject(parent), m_pacing(pacing), m_file(std::move(file)), m_secrets(secrets), m_pace(std::move(pace))
{
    m_soon.setSingleShot(true);
    connect(&m_soon, &QTimer::timeout, this, &PacingFile::write);
}

PacingFile::PacingFile(SignInPacing& pacing, QString file, SecretStore& secrets, QObject* parent)
    : PacingFile(pacing, std::move(file), secrets, Pace{}, parent)
{}

PacingFile::~PacingFile()
{
    // (The pacing may outlive this: the program's own.)
    m_pacing.whenChanged(nullptr);
    // At the program's end this goes after the loop has ended (main.cpp): nothing of the loop is run from here, and
    // what was handed to it meanwhile is not run inside this going (from the merged review, 8). The writing under way
    // is waited for, five seconds at the most (a few milliseconds: one small file); what changed since, or is still
    // to write, is then written here, on this thread. (Not where the one under way did not end: the older would be
    // written over the newer.)
    const bool more = m_soon.isActive() || m_again;
    m_soon.stop();
    if (const std::shared_ptr<Writing> writing = std::exchange(m_writingNow, nullptr); writing && m_writing) {
        std::unique_lock<std::mutex> over(writing->mutex);
        if (!writing->ended.wait_for(over, std::chrono::seconds(5), [&writing] { return writing->done; })) {
            qWarning("qKVM: the wrong-password counts were still being written down as the program ended");
            return;
        }
    }
    if (more && m_key) {
        writeDown(m_file, linesOf(m_pacing.saved(), wallNow()), *m_key);
    }
}

qint64 PacingFile::wallNow() const
{
    return m_pace.wallMs ? m_pace.wallMs() : QDateTime::currentMSecsSinceEpoch();
}

void PacingFile::read()
{
    // Its key, from Windows' store of this program's secrets; a new one where none is kept yet.
    base::Result<std::optional<QByteArray>> had = m_secrets.read(QString::fromLatin1(keyName));
    if (!had) {
        qWarning("qKVM: the key of the wrong-password counts kept on disk was not handed over: they are kept in "
                 "memory only in this run");
        return;
    }
    QByteArray bytes = had.value() ? std::move(*had.value()) : QByteArray();
    const WipedOnLeaving overBytes(bytes);
    const bool fitting = bytes.size() == SecretKey::bytes;
    SecretKey key;
    if (fitting) {
        std::copy(bytes.cbegin(), bytes.cend(), key.data());
    } else {
        if (!sealing::random(std::span<quint8>(key.data(), SecretKey::bytes))) {
            qWarning("qKVM: no key could be made for the wrong-password counts kept on disk: they are kept in "
                     "memory only in this run");
            return;
        }
        // (The copy the store is handed is written over once the store has its own: from the merged review, 7.)
        QByteArray handed = key.view().toByteArray();
        const WipedOnLeaving overHanded(handed);
        if (!m_secrets.keep(QString::fromLatin1(keyName), handed)) {
            qWarning("qKVM: the key of the wrong-password counts kept on disk could not be kept: they are kept in "
                     "memory only in this run");
            return;
        }
    }
    m_key = std::move(key);
    // From here on, what changes is written down (also where what is there now cannot be read).
    m_pacing.whenChanged([this] { changed(); });
    QFile file(m_file);
    if (!file.exists()) {
        return;
    }
    if (!fitting) {
        // A file, and no key that opens it (the store was emptied: a reset of the program's own password).
        setAside("no key opens it");
        return;
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > largestFile) {
        setAside("it cannot be read");
        return;
    }
    const QByteArray whole = file.readAll();
    file.close();
    QJsonParseError error{};
    const QJsonDocument outside = QJsonDocument::fromJson(whole, &error);
    const QJsonObject top = outside.object();
    if (error.error != QJsonParseError::NoError || !outside.isObject()) {
        setAside("it is damaged");
        return;
    }
    if (!top.value("version"_L1).isDouble() || top.value("version"_L1).toInteger(-1) != version) {
        setAside("it is of another version");
        return;
    }
    const QByteArray nonce = QByteArray::fromBase64(top.value("nonce"_L1).toString().toLatin1());
    const QByteArray sealed = QByteArray::fromBase64(top.value("sealed"_L1).toString().toLatin1());
    const base::Result<SecretBytes> opened = sealing::open(*m_key, nonce, sealed, sealedFor);
    const std::optional<QList<Line>> lines = opened ? linesIn(opened.value().view()) : std::nullopt;
    if (!lines) {
        setAside("it is damaged, or was sealed under another key");
        return;
    }
    // Back in the pacing, by its clock: what each waits still on the PC's clock, never longer than the longest hold
    // (a clock set back since).
    const qint64 at = wallNow();
    QList<SignInPacing::Saved> kept;
    kept.reserve(lines->size());
    for (const Line& one : *lines) {
        kept.append(SignInPacing::Saved{one.mark, one.noes, one.silent,
                                        std::clamp<qint64>(one.untilMs - at, 0, m_pace.longestHoldMs)});
    }
    m_pacing.restore(kept);
}

void PacingFile::setAside(const char* why)
{
    const QFileInfo was(m_file);
    for (int number = 1; number <= mostPutAside; ++number) {
        const QString aside = was.dir().filePath(number == 1 ? was.fileName() + u".unreadable"_s
                                                             : was.fileName() + u".unreadable-%1"_s.arg(number));
        if (QFile::exists(aside)) {
            continue;
        }
        std::error_code refused;
        std::filesystem::rename(std::filesystem::path(m_file.toStdU16String()),
                                std::filesystem::path(aside.toStdU16String()), refused);
        if (!refused) {
            m_putAside = aside;
            qWarning("qKVM: the wrong-password counts kept on disk could not be read (%s): put aside as %s, and the "
                     "program starts with nothing held",
                     why, qPrintable(QDir::toNativeSeparators(aside)));
            return;
        }
        break;
    }
    // (Not put aside: nothing is written over it either, and nothing of it is held.)
    qWarning("qKVM: the wrong-password counts kept on disk could not be read (%s), nor put aside: they are kept in "
             "memory only in this run",
             why);
    m_pacing.whenChanged(nullptr);
    m_key.reset();
}

void PacingFile::changed()
{
    if (!m_soon.isActive()) {
        m_soon.start(std::max(0, m_pace.writtenAfterMs));
    }
}

void PacingFile::write()
{
    if (!m_key) {
        return;
    }
    if (m_writing) {
        m_again = true;
        return;
    }
    m_writing = true;
    // On this thread, only what is kept is copied out (as many as the pacing keeps); the rest on a thread of the pool.
    auto writing = std::make_shared<Writing>();
    m_writingNow = writing;
    QThreadPool::globalInstance()->start([here = QPointer<PacingFile>(this), file = m_file,
                                          lines = linesOf(m_pacing.saved(), wallNow()),
                                          key = std::make_shared<SecretKey>(m_key->copy()), writing] {
        writeDown(file, lines, *key);
        {
            const std::lock_guard<std::mutex> over(writing->mutex);
            writing->done = true;
        }
        writing->ended.notify_all();
        QThread* on = QThread::currentThread();
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [here, on] {
                if (here != nullptr) {
                    here->wrote(on);
                }
            },
            Qt::QueuedConnection);
    });
}

void PacingFile::wrote(QThread* on)
{
    m_writing = false;
    m_lastWrittenOn = on;
    ++m_writings;
    if (std::exchange(m_again, false)) {
        write();
    }
}

bool PacingFile::waitForWritten(int withinMs)
{
    if (m_soon.isActive()) {
        m_soon.stop();
        write();
    }
    const QDeadlineTimer until(withinMs);
    while ((m_writing || m_again || m_soon.isActive()) && !until.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (m_soon.isActive()) {
            m_soon.stop();
            write();
        }
    }
    return !m_writing && !m_again && !m_soon.isActive();
}

} // namespace qkvm::core
