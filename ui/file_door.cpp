#include "file_door.h"

#include <QCoreApplication>
#include <QDate>
#include <QFile>
#include <QSaveFile>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Files", text);
}

} // namespace

QString suggestedFileName(bool encrypted)
{
    return u"qkvm-%1.%2"_s.arg(QDate::currentDate().toString(Qt::ISODate), encrypted ? u"qkvm"_s : u"json"_s);
}

QString asWindowsTakesIt(const QString& path)
{
    QString name = path;
    while (name.endsWith(u'.') || name.endsWith(u' ')) {
        name.chop(1);
    }
    return name;
}

QString namedAsWhatItIs(const QString& path, bool encrypted)
{
    const QLatin1StringView right = encrypted ? ".qkvm"_L1 : ".json"_L1;
    const QLatin1StringView other = encrypted ? ".json"_L1 : ".qkvm"_L1;
    // What Windows drops of a name anyway goes first: the dots and blanks at its end. (A name that
    // ends in a dot has no ending: the dot is the one of the ending it is given.)
    QString named = asWindowsTakesIt(path);
    // The other one's ending, where it stands last, is taken off; and then the right one is put
    // on where it does not end so already: "wall.qkvm.json", sealed, is "wall.qkvm".
    if (named.endsWith(other, Qt::CaseInsensitive)) {
        named = asWindowsTakesIt(named.chopped(other.size()));
    }
    return named.endsWith(right, Qt::CaseInsensitive) ? named : named + right;
}

base::Result<QByteArray> readChosenFile(const QString& path, qsizetype atMostBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return base::Error{"unreadable", words("The file could not be read: %1").arg(file.errorString()).toStdString()};
    }
    QByteArray text = file.read(atMostBytes + 1);
    if (text.size() > atMostBytes) {
        return base::Error{"too_large", words("The file is larger than anything qKVM writes.").toStdString()};
    }
    return text;
}

base::Result<void> writeWholeFile(const QString& path, const QByteArray& text)
{
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(text) != text.size() || !out.commit()) {
        return base::Error{"unwritten",
                           words("The file could not be written: %1").arg(out.errorString()).toStdString()};
    }
    return {};
}

QString saidOfAFileThatWasKept(bool encrypted, const QString& name)
{
    if (!name.isEmpty()) {
        return encrypted
                   ? words("Exported to %1. The file opens with its passphrase only: keep the two apart.").arg(name)
                   : words("Exported to %1, without passwords").arg(name);
    }
    return encrypted ? words("Exported. The file opens with its passphrase only: keep the two apart.")
                     : words("Exported, without passwords");
}

} // namespace qkvm::ui
