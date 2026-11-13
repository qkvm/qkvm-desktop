#include "installer_launcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <vector>

#include "installer/setup_launch.h"
#include "installer_config.h"
#include "win/cache_paths.h"
#include <qkvm/core/update_cache.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {
bool installerConfigured()
{
    return installer::packagePublisher[0] != L'\0';
}

bool startInstaller(const QString& action, const QString& package, const QString& version)
{
    if (action != u"--install"_s && action != u"--rollback"_s && action != u"--recover"_s)
        return false;
    QString root;
    try {
        root = QString::fromStdWString(storage::cache(storage::Area::Helpers).wstring());
    } catch (...) {
        return false;
    }
    core::pruneUpdateCache(root, u"qkvm-setup-"_s, u"qkvm-setup.exe"_s, QDateTime::currentDateTimeUtc());
    QTemporaryDir folder(QDir(root).filePath(u"qkvm-setup-XXXXXX"_s));
    if (!folder.isValid())
        return false;
    const QString helper = folder.filePath(u"qkvm-setup.exe"_s);
    if (!QFile::copy(QDir(QCoreApplication::applicationDirPath()).filePath(u"qkvm-setup.exe"_s), helper))
        return false;
    QStringList arguments{action};
    if (action == u"--install"_s) {
        if (package.isEmpty() || version.isEmpty())
            return false;
        arguments << QFileInfo(package).absoluteFilePath() << u"--version"_s << version;
    }
    std::vector<std::wstring> nativeArguments;
    for (const auto& argument : arguments)
        nativeArguments.push_back(argument.toStdWString());
    if (installer::launchSetup(helper.toStdWString(), nativeArguments, installer::SetupHop::Relay) != 0)
        return false;
    // Setup can outlive this application and Windows can replace its installed copy.
    // A later explicit Setup launch prunes old helper folders once they are no longer in use.
    folder.setAutoRemove(false);
    return true;
}
} // namespace qkvm::ui
