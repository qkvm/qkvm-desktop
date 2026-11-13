#pragma once

#include <cstring>
#include <memory>
#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

#include <qkvm/core/update_release.h>

#include <qt_windows.h>

#include <bcrypt.h>

namespace qkvm::core::testing {

// A disposable test key, never a product signing key. No private bytes leave this object.
class ReleaseSigner {
public:
    ReleaseSigner()
    {
        if (BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptGenerateKeyPair(m_algorithm, &m_key, 256, 0) >= 0 && BCryptFinalizeKeyPair(m_key, 0) >= 0) {
            QByteArray blob(72, '\0');
            ULONG size = 0;
            if (BCryptExportKey(m_key, nullptr, BCRYPT_ECCPUBLIC_BLOB, reinterpret_cast<PUCHAR>(blob.data()), 72, &size,
                                0) >= 0 &&
                size == 72) {
                m_public = blob.mid(8);
            }
        }
    }
    ReleaseSigner(const ReleaseSigner&) = delete;
    ReleaseSigner& operator=(const ReleaseSigner&) = delete;
    ~ReleaseSigner()
    {
        if (m_key != nullptr) {
            BCryptDestroyKey(m_key);
        }
        if (m_algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
        }
    }
    QByteArray publicKey() const { return m_public; }
    QByteArray seal(const QByteArray& payload) const
    {
        QByteArray digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
        QByteArray signature(64, '\0');
        ULONG size = 0;
        if (m_public.isEmpty() ||
            BCryptSignHash(m_key, nullptr, reinterpret_cast<PUCHAR>(digest.data()), 32,
                           reinterpret_cast<PUCHAR>(signature.data()), 64, &size, 0) < 0 ||
            size != 64) {
            return {};
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("payload"), QString::fromLatin1(payload.toBase64())},
                                         {QStringLiteral("signature"), QString::fromLatin1(signature.toBase64())}})
            .toJson(QJsonDocument::Compact);
    }
    QByteArray seal(const QJsonObject& payload) const
    {
        return seal(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
    BCRYPT_KEY_HANDLE m_key = nullptr;
    QByteArray m_public;
};

// A release of the given kind, as the release tools write it (spelled out here, not taken from the program).
inline QJsonObject releaseFor(const QByteArray& package, const QDateTime& now = QDateTime::currentDateTimeUtc(),
                              UpdateKind kind = UpdateKind::Portable)
{
    using namespace Qt::StringLiterals;
    const QString platform = kind == UpdateKind::Portable ? u"windows-x64-portable"_s
                             : kind == UpdateKind::Msix   ? u"windows-x64-msix"_s
                                                          : u"windows-x64-setup"_s;
    const QString address = kind == UpdateKind::Portable ? u"https://updates.example.test/qkvm-0.2.0.zip"_s
                            : kind == UpdateKind::Msix
                                ? u"https://updates.example.test/qkvm-0.2.0.msix"_s
                                : u"https://updates.example.test/qKVM-Desktop-0.2.0-windows-x64-setup.exe"_s;
    return {{u"schema"_s, 1},
            {u"product"_s, u"qkvm-desktop"_s},
            {u"platform"_s, platform},
            {u"channel"_s, u"stable"_s},
            {u"version"_s, u"0.2.0"_s},
            {u"notes"_s, u"New features\nBug fixes"_s},
            {u"published"_s, now.addSecs(-60).toString(Qt::ISODate)},
            {u"expires"_s, now.addDays(7).toString(Qt::ISODate)},
            {u"package"_s, address},
            {u"bytes"_s, package.size()},
            {u"sha256"_s, QString::fromLatin1(QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex())}};
}

struct UpdateAnswer {
    QByteArray bytes;
    int status = 200;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    bool hold = false;
    qint64 declaredBytes = -1;
    // Where it sends the asker instead, as a server's redirect does (with the status, say 302); none: no redirect.
    QUrl redirect{};
    // Answered this long after the question (all of it at once then), as a slow server answers.
    int afterMs = 0;
    // What it has is said, and it never ends: a server that keeps sending.
    bool endless = false;
};

class UpdateReply : public QNetworkReply {
public:
    UpdateReply(const QNetworkRequest& request, UpdateAnswer answer, QObject* parent)
        : QNetworkReply(parent), m_answer(std::move(answer))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_answer.status);
        if (!m_answer.redirect.isEmpty()) {
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, m_answer.redirect);
            setRawHeader("Location", m_answer.redirect.toEncoded());
        }
        if (m_answer.declaredBytes >= 0) {
            setHeader(QNetworkRequest::ContentLengthHeader, m_answer.declaredBytes);
        }
        open(QIODevice::ReadOnly);
        if (!m_answer.hold) {
            QTimer::singleShot(m_answer.afterMs, this, [this] {
                if (m_answer.error != NoError) {
                    setError(m_answer.error, QStringLiteral("Test failure"));
                }
                emit readyRead();
                if (!isFinished() && !m_answer.endless) {
                    setFinished(true);
                    emit finished();
                }
            });
        }
    }
    void abort() override { setFinished(true); }
    qint64 bytesAvailable() const override { return m_answer.bytes.size() - m_read + QNetworkReply::bytesAvailable(); }

protected:
    qint64 readData(char* destination, qint64 maximum) override
    {
        const qint64 count = qMin(maximum, m_answer.bytes.size() - m_read);
        if (count <= 0) {
            return -1;
        }
        std::memcpy(destination, m_answer.bytes.constData() + m_read, static_cast<std::size_t>(count));
        m_read += count;
        return count;
    }

private:
    UpdateAnswer m_answer;
    qint64 m_read = 0;
};

class UpdateNetwork : public QNetworkAccessManager {
public:
    UpdateAnswer next;
    // Answers given first, one to each request in turn, before `next` answers all the rest.
    QList<UpdateAnswer> first;
    QList<QNetworkRequest> requests;

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice* outgoing) override
    {
        Q_UNUSED(operation)
        Q_UNUSED(outgoing)
        requests.append(request);
        const UpdateAnswer answer = first.isEmpty() ? next : first.takeFirst();
        return std::make_unique<UpdateReply>(request, answer, this).release(); // The network manager owns it.
    }
};

} // namespace qkvm::core::testing
