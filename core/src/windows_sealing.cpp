#include <qkvm/core/sealing.h>

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include <QCoreApplication>

#include "cng.h"
#include "windows_certificate.h"

// (After windows.h, which cng.h has brought.)
#include <wincrypt.h>

namespace qkvm::core {

bool windowsReadsAsCertificate(QByteArrayView der) noexcept
{
    if (der.isEmpty()) {
        return false;
    }
    const PCCERT_CONTEXT read =
        CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, reinterpret_cast<const BYTE*>(der.data()),
                                     static_cast<DWORD>(der.size()));
    if (read == nullptr) {
        return false;
    }
    CertFreeCertificateContext(read);
    return true;
}

namespace sealing {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Exchange", text);
}

// What Windows says where a tag does not fit (STATUS_AUTH_TAG_MISMATCH).
constexpr NTSTATUS tagDoesNotFit = static_cast<NTSTATUS>(0xC000A002L);

base::Error windowsSaidNo()
{
    return {windowsRefused, words("Windows could not do the encryption.").toStdString()};
}

base::Error opensNothing()
{
    return {doesNotOpen, words("This does not open with that key.").toStdString()};
}

// AES-256 in GCM under one key, for one sealing or one opening.
class Cipher {
public:
    explicit Cipher(const SecretKey& secret) : m_secret(secret.copy()), m_aes(BCRYPT_AES_ALGORITHM, 0)
    {
        // Which way of chaining is meant, in room Windows may write to (it wants that of all
        // it reads).
        std::array<wchar_t, sizeof(BCRYPT_CHAIN_MODE_GCM) / sizeof(wchar_t)> mode{};
        std::memcpy(mode.data(), BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM));
        if (m_aes &&
            cng::worked(BCryptSetProperty(m_aes.get(), BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode.data()),
                                          static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0))) {
            m_key.emplace(m_aes, m_secret.data(), static_cast<ULONG>(SecretKey::bytes));
        }
    }

    explicit operator bool() const noexcept { return m_key.has_value() && static_cast<bool>(*m_key); }
    BCRYPT_KEY_HANDLE key() const noexcept { return m_key.has_value() ? m_key->get() : nullptr; }

private:
    // A copy of the key that Windows may write to, wiped when this goes.
    SecretKey m_secret;
    cng::Algorithm m_aes;
    std::optional<cng::Key> m_key;
};

// What goes with the bytes: the nonce, what it is sealed for, and room for the tag. All of it
// here, where Windows may write to it, and where it stays: Windows is told where each stands.
struct Beside {
    std::array<quint8, nonceBytes> nonce{};
    std::array<quint8, tagBytes> tag{};
    QByteArray context;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};

    Beside(QByteArrayView theNonce, QByteArrayView theContext) : context(theContext.data(), theContext.size())
    {
        std::memcpy(nonce.data(), theNonce.data(), nonce.size());
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data();
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = context.isEmpty() ? nullptr : reinterpret_cast<PUCHAR>(context.data());
        info.cbAuthData = static_cast<ULONG>(context.size());
        info.pbTag = tag.data();
        info.cbTag = static_cast<ULONG>(tag.size());
    }
    Beside(const Beside&) = delete;
    Beside& operator=(const Beside&) = delete;
};

} // namespace

base::Result<void> random(std::span<quint8> into)
{
    if (into.empty()) {
        return {};
    }
    if (into.size() > static_cast<std::size_t>(largestBytes) ||
        !cng::worked(
            BCryptGenRandom(nullptr, into.data(), static_cast<ULONG>(into.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return windowsSaidNo();
    }
    return {};
}

base::Result<QByteArray> seal(const SecretKey& key, QByteArrayView nonce, QByteArrayView plain, QByteArrayView context)
{
    if (nonce.size() != nonceBytes || plain.size() > largestBytes || context.size() > largestBytes) {
        return windowsSaidNo();
    }
    const Cipher cipher(key);
    if (!cipher) {
        return windowsSaidNo();
    }
    Beside beside(nonce, context);
    // Encrypted where it stands: in a copy, with room for the tag behind it.
    QByteArray sealed(plain.size() + tagBytes, Qt::Uninitialized);
    if (!plain.isEmpty()) {
        std::memcpy(sealed.data(), plain.data(), static_cast<std::size_t>(plain.size()));
    }
    auto* bytes = reinterpret_cast<PUCHAR>(sealed.data());
    ULONG done = 0;
    const NTSTATUS status = BCryptEncrypt(cipher.key(), bytes, static_cast<ULONG>(plain.size()), &beside.info, nullptr,
                                          0, bytes, static_cast<ULONG>(plain.size()), &done, 0);
    if (!cng::worked(status) || done != static_cast<ULONG>(plain.size())) {
        // (What was to be sealed stands in it, or half of it.)
        wipe(sealed.data(), static_cast<std::size_t>(sealed.size()));
        return windowsSaidNo();
    }
    std::memcpy(sealed.data() + plain.size(), beside.tag.data(), beside.tag.size());
    return sealed;
}

base::Result<SecretBytes> open(const SecretKey& key, QByteArrayView nonce, QByteArrayView sealed,
                               QByteArrayView context)
{
    if (nonce.size() != nonceBytes || sealed.size() < tagBytes || sealed.size() > largestBytes + tagBytes ||
        context.size() > largestBytes) {
        return opensNothing();
    }
    const Cipher cipher(key);
    if (!cipher) {
        return windowsSaidNo();
    }
    Beside beside(nonce, context);
    const qsizetype count = sealed.size() - tagBytes;
    std::memcpy(beside.tag.data(), sealed.data() + count, beside.tag.size());
    // Decrypted where it stands, in room that is wiped when it goes: Windows may have written
    // to it before it found that the tag does not fit.
    SecretBytes plain = SecretBytes::copyOf(sealed.first(count));
    ULONG done = 0;
    const NTSTATUS status = BCryptDecrypt(cipher.key(), plain.data(), static_cast<ULONG>(count), &beside.info, nullptr,
                                          0, plain.data(), static_cast<ULONG>(count), &done, 0);
    if (status == tagDoesNotFit) {
        return opensNothing();
    }
    if (!cng::worked(status) || done != static_cast<ULONG>(count)) {
        return windowsSaidNo();
    }
    return base::Result<SecretBytes>(std::move(plain));
}

} // namespace sealing

} // namespace qkvm::core
