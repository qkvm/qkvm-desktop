#include <qkvm/core/kvm_certificate.h>

#include <algorithm>

#include <QCryptographicHash>

namespace qkvm::core::kvm {

QSslConfiguration knownByItsCertificateAlone(QSslConfiguration line)
{
    line.setCaCertificates({});
    return line;
}

Objected objectedTo(const QSslCertificate& shown, const QList<QSslError>& objections, bool knownByCertificate,
                    const QByteArray& certificate)
{
    if (!knownByCertificate) {
        return Objected::Untrusted;
    }
    // (No certificate at all is never the one, whatever is saved as the one.)
    if (shown.isNull() || shown.digest(QCryptographicHash::Sha256) != certificate) {
        return Objected::Changed;
    }
    const bool allNameIt = std::ranges::all_of(
        objections, [&shown](const QSslError& objection) { return objection.certificate() == shown; });
    return allNameIt ? Objected::PassedOver : Objected::BySigner;
}

} // namespace qkvm::core::kvm
