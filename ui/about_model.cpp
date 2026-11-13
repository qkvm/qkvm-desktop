#include "about_model.h"

#include <QLibraryInfo>
#include <QVersionNumber>

#include <qkvm/engine/about.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

AboutModel::AboutModel(QObject* parent) : QAbstractListModel(parent)
{
    // Qt as it is running, which is the Qt that ships. Its sources stay at this address for every version.
    const QVersionNumber qt = QLibraryInfo::version();
    m_libraries.append({
        u"Qt"_s,
        qt.toString(),
        u"LGPL-3.0-only"_s,
        u"https://download.qt.io/archive/qt/%1.%2/%3/"_s.arg(qt.majorVersion())
            .arg(qt.minorVersion())
            .arg(qt.toString()),
    });

    // Pinned sources used to build the native transport DLL (experiments/webrtc/dependencies.json).
    m_libraries.append(
        {u"libdatachannel"_s, u"0.24.6"_s, u"MPL-2.0"_s,
         u"https://github.com/paullouisageneau/libdatachannel/tree/6b1e2e620f1e37f0eafeee702eaea0043cb305fd"_s});
    m_libraries.append({u"Mbed TLS"_s, u"3.6.7"_s, u"Apache-2.0"_s,
                        u"https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7"_s});
    m_libraries.append(
        {u"libjuice"_s, u"b89c792"_s, u"MPL-2.0"_s,
         u"https://github.com/paullouisageneau/libjuice/tree/b89c792e3612faf2f12cf35bcc56857313a06be3"_s});
    m_libraries.append({u"libsrtp"_s, u"24b3bf8"_s, u"BSD-3-Clause"_s,
                        u"https://github.com/cisco/libsrtp/tree/24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5"_s});
    m_libraries.append(
        {u"usrsctp"_s, u"fec583d"_s, u"BSD-3-Clause"_s,
         u"https://github.com/paullouisageneau/usrsctp/tree/fec583d54493f879d2ae44a743423bf8a04371ab"_s});
    m_libraries.append({u"plog"_s, u"94899e0"_s, u"MIT"_s,
                        u"https://github.com/SergiusTheBest/plog/tree/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef"_s});

    // FFmpeg in its own words: the licence of a build depends on how it was built. The project builds its own
    // (tools/build_ffmpeg.ps1), with zlib inside it, and their sources are a file of every release.
    const QString release = u"https://github.com/qkvm/qkvm-desktop/releases/tag/v%1"_s.arg(version());
    const engine::DecoderLibrary ffmpeg = engine::decoderLibrary();
    m_libraries.append({
        u"FFmpeg"_s,
        QString::fromStdString(ffmpeg.version),
        QString::fromStdString(ffmpeg.licence),
        release,
    });
    m_libraries.append({u"zlib"_s, u"1.3.2"_s, u"Zlib"_s, release});
}

QString AboutModel::version() const
{
    return QStringLiteral(QKVM_DESKTOP_VERSION);
}

QString AboutModel::licence() const
{
    return u"AGPL-3.0-only"_s;
}

QString AboutModel::source() const
{
    // Every release is a tag of the public repository, v and its three numbers.
    return u"https://github.com/qkvm/qkvm-desktop/tree/v%1"_s.arg(version());
}

void AboutModel::useDiagnostics(std::function<bool()> open)
{
    m_diagnostics = std::move(open);
    m_diagnosticsProblem.clear();
    emit diagnosticsChanged();
}

void AboutModel::openDiagnostics()
{
    if (!m_diagnostics)
        return;
    m_diagnosticsProblem = m_diagnostics() ? QString() : tr("The diagnostics folder could not be opened.");
    emit diagnosticsChanged();
}

int AboutModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_libraries.size());
}

QVariant AboutModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_libraries.size()) {
        return {};
    }
    const Library& library = m_libraries.at(index.row());
    switch (role) {
    case NameRole:
        return library.name;
    case VersionRole:
        return library.version;
    case LicenceRole:
        return library.licence;
    case SourceRole:
        return library.source;
    default:
        return {};
    }
}

QHash<int, QByteArray> AboutModel::roleNames() const
{
    return {
        {NameRole, "name"_ba},
        {VersionRole, "version"_ba},
        {LicenceRole, "licence"_ba},
        {SourceRole, "source"_ba},
    };
}

} // namespace qkvm::ui
