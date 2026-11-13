#pragma once

#include <QByteArrayView>

namespace qkvm::core {

// Whether Windows reads these bytes as a certificate: what it would make of them if a KVM
// showed them on a line (windows_sealing.cpp). Windows' own reader is the one that will be asked
// then, and is asked here, with nothing of Qt's between.
bool windowsReadsAsCertificate(QByteArrayView der) noexcept;

} // namespace qkvm::core
