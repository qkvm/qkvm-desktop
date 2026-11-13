#include <qkvm/core/update_cache.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <windows.h>

using namespace Qt::StringLiterals;
namespace qkvm::core {
namespace {
bool plainEntry(const QFileInfo& file)
{
    // Qt's NoSymLinks/isSymLink do not exclude NTFS junctions. Refuse every Windows
    // reparse point before entering a cache directory or removing its single file.
    const auto path = QDir::toNativeSeparators(file.absoluteFilePath()).toStdWString();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

// What is in a folder, all of it, depth first (a folder after what is in it), where nothing in it is a link or a
// junction and nothing is newer than a week; nothing where anything is.
bool allOfItAWeekOld(const QFileInfo& folder, const QDateTime& now, QList<QFileInfo>& all)
{
    const auto entries = QDir(folder.absoluteFilePath())
                             .entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (!plainEntry(entry) || entry.lastModified().daysTo(now) < 7)
            return false;
        if (entry.isDir() && !allOfItAWeekOld(entry, now, all))
            return false;
        all.append(entry);
    }
    return true;
}

// Setup's own TEMP folders a week on (Updates::runSetup made them; Setup takes away what it unpacked, but not always):
// each taken away whole, only where all of it is a week old and nothing in it is a link or junction. What is in use
// cannot be taken, and stays with its folder.
int pruneSetupsTemp(const QString& root, const QDateTime& now)
{
    static const QRegularExpression names(u"\\Asetup-temp-[A-Za-z0-9]{6}\\z"_s);
    int removed = 0;
    const QDir cache(root);
    const auto folders =
        cache.entryInfoList({u"setup-temp-*"_s}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const auto& folder : folders) {
        QList<QFileInfo> all;
        if (!names.match(folder.fileName()).hasMatch() || !plainEntry(folder) ||
            folder.lastModified().daysTo(now) < 7 || !allOfItAWeekOld(folder, now, all))
            continue;
        bool gone = true;
        for (const auto& entry : all) {
            gone = (entry.isDir() ? QDir().rmdir(entry.absoluteFilePath()) : QFile::remove(entry.absoluteFilePath())) &&
                   gone;
        }
        if (gone && cache.rmdir(folder.fileName()))
            ++removed;
    }
    return removed;
}
} // namespace

int pruneUpdateCache(const QString& root, const QString& prefix, const QString& fileName, const QDateTime& now)
{
    if (!now.isValid() || !plainEntry(QFileInfo(root)) || !QFileInfo(root).isDir())
        return 0;
    if (prefix == u"setup-temp-"_s && fileName.isEmpty())
        return pruneSetupsTemp(root, now);
    if (!((prefix == u"update-"_s &&
           (fileName == u"qkvm.msix"_s || fileName == u"qkvm.zip"_s || fileName == u"qkvm-desktop-setup.exe"_s)) ||
          (prefix == u"qkvm-setup-"_s && fileName == u"qkvm-setup.exe"_s)))
        return 0;
    const QRegularExpression names(u"\\A"_s + QRegularExpression::escape(prefix) + u"[A-Za-z0-9]{6}\\z"_s);
    int removed = 0;
    const QDir cache(root);
    const auto folders = cache.entryInfoList({prefix + u'*'}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const auto& folder : folders) {
        if (!names.match(folder.fileName()).hasMatch() || !plainEntry(folder) || folder.lastModified().daysTo(now) < 7)
            continue;
        const QDir directory(folder.absoluteFilePath());
        const auto files =
            directory.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        if (files.size() != 1 || files[0].fileName() != fileName || !files[0].isFile() || !plainEntry(files[0]) ||
            files[0].lastModified().daysTo(now) < 7)
            continue;
        if (QFile::remove(files[0].absoluteFilePath()) && cache.rmdir(folder.fileName()))
            ++removed;
    }
    return removed;
}
} // namespace qkvm::core
