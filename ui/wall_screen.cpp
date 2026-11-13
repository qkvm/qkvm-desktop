#include "wall_screen.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

WallScreen::WallScreen(core::SecretStore& secrets, QObject* parent) : QObject(parent), m_secrets(secrets)
{
    // Whatever stands under the name makes this PC a screen, whether it reads as addresses or not:
    // what is no address of a server here is one nobody can sign out, and that is the safe side.
    m_servers = QString::fromUtf8(m_secrets.kept(keptAs())).split(u'\n', Qt::SkipEmptyParts);
}

QString WallScreen::keptAs()
{
    return u"wall screen"_s;
}

void WallScreen::becameOneFor(const QString& server)
{
    if (server.isEmpty() || m_servers.contains(server)) {
        return;
    }
    const bool was = isOne();
    m_servers.append(server);
    keep();
    if (!was) {
        emit changed();
    }
}

void WallScreen::signedOutAt(const QString& server)
{
    if (!m_servers.removeOne(server)) {
        return;
    }
    keep();
    if (!isOne()) {
        emit changed();
    }
}

void WallScreen::keep()
{
    if (m_servers.isEmpty()) {
        m_secrets.forget(keptAs());
        return;
    }
    // Where Windows does not keep it, this PC is a screen all the same while the program runs.
    static_cast<void>(m_secrets.keep(keptAs(), m_servers.join(u'\n').toUtf8()));
}

base::Result<void> WallScreen::clearLocally()
{
    const auto names = m_secrets.names();
    if (!names) {
        return names.error();
    }
    // Include links whose screen marker was lost; no server needs to answer.
    for (const QString& name : names.value()) {
        if (name.startsWith(u"display link at ")) {
            const auto session = m_secrets.erase(u"session at "_s + name.mid(16));
            const auto link = m_secrets.erase(name);
            if (!session || !link) {
                return base::Error{"secrets", "Windows could not forget the display links."};
            }
        }
    }
    for (const QString& server : m_servers) {
        if (!m_secrets.erase(u"session at "_s + server)) {
            return base::Error{"secrets", "Windows could not forget a display session."};
        }
    }
    const auto cleared = m_secrets.erase(keptAs());
    if (!cleared) {
        return cleared.error();
    }
    m_servers.clear();
    emit changed();
    return {};
}

} // namespace qkvm::ui
