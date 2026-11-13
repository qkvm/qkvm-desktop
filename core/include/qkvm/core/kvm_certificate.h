#pragma once

#include <QByteArray>
#include <QList>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>

// How a KVM's certificate is looked at where Windows objects to it: one rule, for the driver's
// questions (KvmdDevice) and for the lines the program speaks itself (DeviceLine), so that the two
// cannot come to say different things of one KVM. Over HTTPS a KVM is one of two: one whose
// certificate Windows trusts for its name, or one that shows exactly the certificate it is known
// by (SPEC.md section 8).
namespace qkvm::core::kvm {

// How a line is set up on which a KVM that is known by a certificate is to be known by that
// alone: Windows is given nobody to vouch for anybody, objects to every certificate, the KVM's
// own included, and so the look below is made for every line there is to it.
QSslConfiguration knownByItsCertificateAlone(QSslConfiguration line);

// What comes of Windows' objections to the certificate a KVM showed.
enum class Objected {
    PassedOver, // it showed the one it is known by, and every objection names that certificate itself
    BySigner,   // it showed the one it is known by, and an objection names another: its signer's, sent along
    Changed,    // it is known by a certificate, and showed another, or none
    Untrusted,  // it is known by none: Windows has to trust what it shows, and does not
};

// Decides that, from what the KVM showed, what Windows objects to in it, and what the KVM is known
// by (`certificate`: the SHA-256 of the one it is known by). Only `PassedOver` lets a line go on,
// and whoever asks then tells Qt to pass over exactly these objections.
//
// Passed over only where every objection names the shown certificate itself. A certificate that
// somebody else signed is objected to by its signer's name where the signer's is sent along, and
// passing that over would pass it over for every other certificate of that signer's: such a KVM
// is let in where Windows trusts the signer, and by that.
Objected objectedTo(const QSslCertificate& shown, const QList<QSslError>& objections, bool knownByCertificate,
                    const QByteArray& certificate);

} // namespace qkvm::core::kvm
