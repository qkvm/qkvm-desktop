#include <qkvm/core/tls.h>

#include <QSslSocket>
#include <QString>

using namespace Qt::StringLiterals;

namespace qkvm::core {

bool useWindowsOwnTls()
{
    // (Where Qt refuses the choice, TLS is in use already, or Windows' own is not to be had:
    // with what, it says.)
    static const bool windowsOwn =
        QSslSocket::setActiveBackend(u"schannel"_s) || QSslSocket::activeBackend() == "schannel"_L1;
    return windowsOwn;
}

bool refusedForItsTls(bool windowsOwnTls, const QUrl& address)
{
    // (QUrl writes how an address is spoken to in small letters, however it was typed.)
    const QString scheme = address.scheme();
    return !windowsOwnTls && (scheme == "https"_L1 || scheme == "wss"_L1);
}

base::Error damagedTls()
{
    return {tlsDamaged, tlsDamagedSentence};
}

} // namespace qkvm::core
