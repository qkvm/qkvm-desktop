#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <functional>

#include <qkvm/core/secrets.h>

// Secrets kept in memory, for tests of what keeps and forgets them: nothing of a test reaches the
// Credential Manager of whoever runs it.
namespace qkvm::core::testing {

class MemorySecrets final : public SecretStore {
public:
    base::Result<void> keep(const QString& name, const QByteArray& secret) override
    {
        ++keeps;
        if (refuses || keeps == refusesTheKeep) {
            return base::Error{"secrets", "Windows did not keep it."};
        }
        all.insert(name, secret);
        return {};
    }
    // (A read Windows refuses comes back empty from kept(), as from Windows' own store.)
    QByteArray kept(const QString& name) override
    {
        if (onRead)
            onRead(name);
        return refusesReads ? QByteArray() : all.value(name);
    }
    base::Result<std::optional<QByteArray>> read(const QString& name) override
    {
        ++reads;
        if (onRead)
            onRead(name);
        if (refusesReads || reads == refusesTheRead) {
            return base::Error{"secrets", "Reading failed."};
        }
        if (!all.contains(name)) {
            return std::optional<QByteArray>();
        }
        return std::optional<QByteArray>(all.value(name));
    }
    base::Result<QStringList> names() override
    {
        ++inspections;
        if (onInspection)
            onInspection(inspections);
        if (refusesInspection || inspections == refusesTheInspection) {
            return base::Error{"secrets", "Inspection failed."};
        }
        return all.keys();
    }
    base::Result<void> erase(const QString& name) override
    {
        if (forgetsNothing) {
            return base::Error{"secrets", "Erasure failed."};
        }
        all.remove(name);
        if (onErased)
            onErased(name);
        return {};
    }
    void forget(const QString& name) override
    {
        if (!forgetsNothing) {
            all.remove(name);
        }
    }

    QHash<QString, QByteArray> all;
    bool refuses = false;
    bool refusesInspection = false;
    int inspections = 0;
    int refusesTheInspection = 0;
    std::function<void(int)> onInspection;
    std::function<void(const QString&)> onErased;
    // How often one secret was read by read(), the one of those that fails (counted from 1; 0: none),
    // whether every one fails, and what happens just before a read.
    int reads = 0;
    int refusesTheRead = 0;
    bool refusesReads = false;
    std::function<void(const QString&)> onRead;
    // How often something was to be kept, and the one of those times that fails, counted from 1;
    // 0: none of them does.
    int keeps = 0;
    int refusesTheKeep = 0;
    // What is to be forgotten stays where it is: Windows did not take it out.
    bool forgetsNothing = false;
};

// A KVM's password as this PC's own wall keeps it (local_wall.h): one secret, with the address and
// the certificate it was saved for; no certificate: it is known by none. Written out here and not
// asked of the wall: what stands in the store is what a test looks at.
inline QByteArray savedFor(const QByteArray& password, const QString& address, const QByteArray& certificate = {})
{
    const QByteArray knownBy = certificate.isEmpty() ? QByteArray("none") : "sha256:" + certificate.toHex();
    return "QKVM-KVM-2\n" + address.toUtf8() + '\n' + knownBy + '\n' + password;
}

} // namespace qkvm::core::testing
