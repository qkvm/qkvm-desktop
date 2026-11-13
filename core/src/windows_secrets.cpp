#include <qkvm/core/secrets.h>

#include <memory>
#include <string>
#include <utility>

#include <qkvm/core/wiped.h>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <wincred.h>

namespace qkvm::core {

namespace {

class WindowsSecrets final : public SecretStore {
public:
    explicit WindowsSecrets(QString under) : m_under(std::move(under)) {}

    base::Result<void> keep(const QString& name, const QByteArray& secret) override
    {
        if (secret.isEmpty() || secret.size() > largestSecretBytes) {
            return base::Error{"secrets", "What was to be kept is not of a size Windows keeps."};
        }
        std::wstring target = targetOf(name);
        // Windows wants the bytes as its own to read, not to change: a copy of them, which is
        // written over when this is left.
        QByteArray bytes = secret;
        const WipedOnLeaving over(bytes);
        CREDENTIALW entry{};
        entry.Type = CRED_TYPE_GENERIC;
        entry.TargetName = target.data();
        entry.CredentialBlobSize = static_cast<DWORD>(bytes.size());
        entry.CredentialBlob = reinterpret_cast<LPBYTE>(bytes.data());
        // On this PC, and not on the others the same person signs in to.
        entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (CredWriteW(&entry, 0) == FALSE) {
            return base::Error{"secrets", "Windows did not keep it (error " + std::to_string(GetLastError()) + ")."};
        }
        return {};
    }

    QByteArray kept(const QString& name) override
    {
        PCREDENTIALW entry = nullptr;
        if (CredReadW(targetOf(name).c_str(), CRED_TYPE_GENERIC, 0, &entry) == FALSE || entry == nullptr) {
            return {};
        }
        // Given back to Windows on every way out of here.
        const std::unique_ptr<CREDENTIALW, decltype(&CredFree)> handedOver(entry, &CredFree);
        // Copied out where it is of a size this program keeps, and written over in Windows' memory
        // before that is given back, whatever it is (one that is too long too): what CredFree frees
        // holds nothing of the secret.
        return takenOut(entry->CredentialBlob, entry->CredentialBlobSize, largestSecretBytes);
    }

    base::Result<std::optional<QByteArray>> read(const QString& name) override
    {
        PCREDENTIALW entry = nullptr;
        if (CredReadW(targetOf(name).c_str(), CRED_TYPE_GENERIC, 0, &entry) == FALSE || entry == nullptr) {
            const DWORD error = GetLastError();
            if (error == ERROR_NOT_FOUND) {
                return std::optional<QByteArray>();
            }
            return base::Error{"secrets",
                               "Windows did not hand over a saved secret (error " + std::to_string(error) + ")."};
        }
        const std::unique_ptr<CREDENTIALW, decltype(&CredFree)> handedOver(entry, &CredFree);
        // Found, even where it holds nothing or more than is kept: that is for the reader to judge.
        return std::optional<QByteArray>(
            takenOut(entry->CredentialBlob, entry->CredentialBlobSize, largestSecretBytes));
    }

    void forget(const QString& name) override { CredDeleteW(targetOf(name).c_str(), CRED_TYPE_GENERIC, 0); }

    base::Result<void> erase(const QString& name) override
    {
        if (CredDeleteW(targetOf(name).c_str(), CRED_TYPE_GENERIC, 0) != FALSE || GetLastError() == ERROR_NOT_FOUND) {
            return {};
        }
        return base::Error{"secrets", "Windows could not erase a saved secret."};
    }

    base::Result<QStringList> names() override
    {
        const QString prefix = m_under + QStringLiteral(": ");
        const std::wstring filter = (prefix + u'*').toStdWString();
        DWORD count = 0;
        PCREDENTIALW* entries = nullptr;
        if (CredEnumerateW(filter.c_str(), 0, &count, &entries) == FALSE) {
            if (GetLastError() == ERROR_NOT_FOUND) {
                return QStringList();
            }
            return base::Error{"secrets", "Windows could not inspect the saved secrets."};
        }
        const std::unique_ptr<PCREDENTIALW, decltype(&CredFree)> release(entries, &CredFree);
        QStringList found;
        for (DWORD index = 0; index < count; ++index) {
            const PCREDENTIALW entry = entries[index];
            const QString target = QString::fromWCharArray(entry->TargetName);
            if (entry->Type == CRED_TYPE_GENERIC && target.startsWith(prefix, Qt::CaseInsensitive)) {
                found.append(target.mid(prefix.size()));
            }
            if (entry->CredentialBlob != nullptr) {
                wipe(entry->CredentialBlob, entry->CredentialBlobSize);
            }
        }
        return found;
    }

private:
    std::wstring targetOf(const QString& name) const { return (m_under + QStringLiteral(": ") + name).toStdWString(); }

    QString m_under;
};

} // namespace

std::unique_ptr<SecretStore> windowsSecrets(const QString& under)
{
    return std::make_unique<WindowsSecrets>(under);
}

} // namespace qkvm::core
