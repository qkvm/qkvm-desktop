#pragma once

#include <memory>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <qkvm/base/result.h>

namespace qkvm::core {

// The one way a secret is kept from one run of the program to the next (SPEC.md section 8): by a
// name, in Windows Credential Manager, where it is encrypted for whoever is signed in to Windows.
// Nothing else in the program writes a secret anywhere: not into the settings, not into a log.
class SecretStore {
public:
    virtual ~SecretStore() = default;

    virtual base::Result<void> keep(const QString& name, const QByteArray& secret) = 0;
    // What is kept under the name; empty when nothing is, or when it cannot be read.
    virtual QByteArray kept(const QString& name) = 0;
    // The same, told apart: nothing kept under the name (an empty optional) from what Windows did not
    // hand over (an error). A store that cannot tell them apart says "nothing kept" for both, as
    // kept() does.
    virtual base::Result<std::optional<QByteArray>> read(const QString& name)
    {
        QByteArray bytes = kept(name);
        if (bytes.isEmpty()) {
            return std::optional<QByteArray>();
        }
        return std::optional<QByteArray>(std::move(bytes));
    }
    virtual void forget(const QString& name) = 0;
    // Recovery must distinguish an empty store from an unreadable one, and erase even
    // orphaned entries. Unsupported stores fail closed; ordinary callers keep the old API.
    virtual base::Result<QStringList> names()
    {
        return base::Error{"secrets", "The secret store cannot be inspected."};
    }
    virtual base::Result<void> erase(const QString&)
    {
        return base::Error{"secrets", "The secret store cannot confirm erasure."};
    }
};

// The largest secret that is kept: Windows takes no more for one entry.
inline constexpr qsizetype largestSecretBytes = 2560;

// What the program's own secrets are kept under in Windows Credential Manager: every profile from 0.1.0 on has its
// secrets there by this (SPEC.md section 11.4), so it changes only by a migration.
inline constexpr const char* programsSecretsUnder = "QKVM Desktop";

// The name Windows keeps a secret of this name under: `under`, a colon and a blank, and the name.
QString windowsTargetOf(const QString& name, const QString& under = QString::fromLatin1(programsSecretsUnder));

// Windows Credential Manager, for this PC only: what is kept does not travel to the user's other
// PCs. Every name is kept under `under`, which a test sets to something of its own.
std::unique_ptr<SecretStore> windowsSecrets(const QString& under = QString::fromLatin1(programsSecretsUnder));

} // namespace qkvm::core
