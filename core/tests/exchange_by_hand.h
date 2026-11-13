#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <qkvm/core/exchange.h>
#include <qkvm/core/scrypt.h>
#include <qkvm/core/sealing.h>
#include <qkvm/core/wiped.h>

// A wall as a file, put together by hand: what the tests of the program's own reader and writer
// hand in and compare with. A sealed file is made here from its parts, each of which a test may
// make another before the file is written out; and plain files are written by Qt's own writer of
// JSON, which is not the one the program writes with.
namespace qkvm::core::testing {

// The passphrase of the server's example files (core/tests/data/export), and of the tests.
inline QString thePassphrase()
{
    return QStringLiteral("a passphrase for the file");
}

// A cost that a test can pay a hundred times, as the server's own tests have it.
inline constexpr scrypt::Cost quickCost{4096, 8, 1};

// One of the files a QKVM server's own writer made.
inline QByteArray exampleFile(const char* name)
{
    QFile file(QStringLiteral(QKVM_EXPORT_EXAMPLES "/") + QLatin1StringView(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A sign that takes two places in a text of this program's and is one to the server (the
// grinning face), so many times over.
inline QString wideSigns(qsizetype count)
{
    const char32_t wide = 0x1F600;
    return QString::fromUcs4(&wide, 1).repeated(count);
}

// How a reading or a writing ended: "read", or its code and its sentence.
template <typename Answer>
QString saidOf(const base::Result<Answer>& answer)
{
    return answer ? QStringLiteral("read")
                  : QString::fromStdString(answer.error().code) + QStringLiteral(": ") +
                        QString::fromStdString(answer.error().message);
}

// A part of what a file holds, or one with nothing in it where the file has no such part: for a
// test that has looked whether the part is there, or does not care.
inline QList<exchange::Device> devicesIn(const exchange::Content& content)
{
    return content.devices.value_or(QList<exchange::Device>());
}

inline QList<exchange::Action> actionsIn(const exchange::Content& content)
{
    return content.actions.value_or(QList<exchange::Action>());
}

inline exchange::Alerts alertsIn(const exchange::Content& content)
{
    return content.alerts.value_or(exchange::Alerts());
}

inline exchange::WallSettings wallIn(const exchange::Content& content)
{
    return content.wall.value_or(exchange::WallSettings());
}

// A device's console link as the file had it: the text, for a test to compare.
inline QString linkOf(const exchange::Device& device)
{
    return device.consoleLinkAsWritten.asAFileHadIt();
}

// The text of a file that was written, for a test that looks at the text: what is not in the
// file is looked at by the tests that are about that.
inline QByteArray textOf(const exchange::Written& written)
{
    return written.file(exchange::NotInIt::wasLookedAt);
}

// A certificate of so many bytes: the one of the server's example files (`der`), with what
// stands where its signature does made as long as it takes. It is a certificate to Windows,
// which reads one without asking whether it was signed; and it is nobody's.
inline QByteArray certificateOfSoManyBytes(const QByteArray& der, qsizetype bytes)
{
    // The example: "30 82 01 1f", then what is signed (201 bytes), how (12 bytes), and the
    // signature. Here: the same but for the signature, which is so many bytes of nothing.
    constexpr qsizetype head = 4;
    constexpr qsizetype signedAndHow = 201 + 12;
    const qsizetype signature = bytes - head - signedAndHow - head;
    const auto twoBytes = [](qsizetype length) {
        return QByteArray(1, static_cast<char>((length >> 8) & 0xFF)) + QByteArray(1, static_cast<char>(length & 0xFF));
    };
    return QByteArray::fromHex("3082") + twoBytes(bytes - head) + der.mid(head, signedAndHow) +
           QByteArray::fromHex("0382") + twoBytes(signature) + QByteArray(1, '\0') + QByteArray(signature - 1, '\x5a');
}

// A certificate's bytes as a text of one line: between its two lines, with no line's end at all.
inline QString inOneLine(const QByteArray& der)
{
    return QStringLiteral("-----BEGIN CERTIFICATE-----") + QString::fromLatin1(der.toBase64()) +
           QStringLiteral("-----END CERTIFICATE-----");
}

inline SecretKey keyFrom(const QByteArray& bytes)
{
    SecretKey key;
    std::copy_n(bytes.constData(), std::min<qsizetype>(bytes.size(), SecretKey::bytes), key.data());
    return key;
}

// The passphrase's work, watched: how often it was asked for, and what was asked the last time.
// A cost that is not the quick one is not worked on at all (a key of noughts is handed back):
// what a test shows of such a file is that this was never asked.
class WatchedWork {
public:
    exchange::Derive derive()
    {
        return [seen = m_seen](QByteArrayView passphrase, QByteArrayView salt, const scrypt::Cost& cost,
                               const std::stop_token& stop) -> base::Result<SecretKey> {
            ++seen->asked;
            seen->n = cost.n;
            seen->r = cost.r;
            seen->p = cost.p;
            if (cost != quickCost) {
                return base::Result<SecretKey>(SecretKey());
            }
            return exchange::windowsDerive()(passphrase, salt, cost, stop);
        };
    }

    int asked() const { return m_seen->asked.load(); }
    scrypt::Cost lastCost() const { return {m_seen->n.load(), m_seen->r.load(), m_seen->p.load()}; }

private:
    struct Seen {
        std::atomic<int> asked{0};
        std::atomic<quint64> n{0};
        std::atomic<quint32> r{0};
        std::atomic<quint32> p{0};
    };
    std::shared_ptr<Seen> m_seen = std::make_shared<Seen>();
};

inline QByteArray randomBytes(qsizetype count)
{
    QByteArray bytes(count, '\0');
    const base::Result<void> made =
        sealing::random(std::span<quint8>(reinterpret_cast<quint8*>(bytes.data()), static_cast<std::size_t>(count)));
    return made ? bytes : QByteArray();
}

inline QString base64Of(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toBase64());
}

// A sealed file in its parts, as the server writes one: the lock (how the passphrase is made a
// key, and the file's own key sealed under that) and the content sealed under the file's key.
struct SealedByHand {
    QJsonObject head{{QStringLiteral("format"), QStringLiteral("qkvm-export")}, {QStringLiteral("version"), 1}};
    QJsonObject kdf;
    QJsonObject key;
    QJsonObject sealed;
    // What went into it, for a test that seals something of it anew.
    QByteArray salt;
    QByteArray derived;
    QByteArray fileKey;

    QJsonObject lock() const { return {{QStringLiteral("kdf"), kdf}, {QStringLiteral("key"), key}}; }

    QByteArray text() const
    {
        QJsonObject file = head;
        file[QStringLiteral("lock")] = lock();
        file[QStringLiteral("sealed")] = sealed;
        return QJsonDocument(file).toJson(QJsonDocument::Compact);
    }

    // Something sealed under a key for a context, as a file writes it.
    static QJsonObject sealedUnder(const QByteArray& under, const QByteArray& plain, const QByteArray& context,
                                   const QByteArray& nonce = randomBytes(sealing::nonceBytes))
    {
        const base::Result<QByteArray> data = sealing::seal(keyFrom(under), nonce, plain, context);
        return {{QStringLiteral("nonce"), base64Of(nonce)},
                {QStringLiteral("data"), data ? base64Of(data.value()) : QString()}};
    }
};

// A file that holds `inside`, sealed with `passphrase` at `cost`, under a file key of these
// bytes (of 32, as a file's own key has them, unless a test wants another).
inline SealedByHand sealedByHand(const QByteArray& inside, const QString& passphrase = thePassphrase(),
                                 const scrypt::Cost& cost = quickCost, const QByteArray& fileKey = QByteArray(32, 'k'))
{
    // The passphrase's work is done once for a test program, under one salt: a test hands in
    // some hundreds of these files, and what they are looked at for is never their salt. (The
    // tests that make them run on one thread.)
    struct Worked {
        QByteArray salt;
        QByteArray derived;
    };
    static QHash<QString, Worked> worked;
    const QString under = QStringLiteral("%1 %2 %3 ").arg(cost.n).arg(cost.r).arg(cost.p) + passphrase;
    if (!worked.contains(under)) {
        Worked once{randomBytes(16), QByteArray(32, '\0')};
        const base::Result<void> derived =
            scrypt::derive(passphrase.toUtf8(), once.salt, cost,
                           std::span<quint8>(reinterpret_cast<quint8*>(once.derived.data()), 32));
        if (!derived) {
            return {};
        }
        worked.insert(under, once);
    }
    SealedByHand made;
    made.salt = worked.value(under).salt;
    made.derived = worked.value(under).derived;
    made.fileKey = fileKey;
    made.kdf = QJsonObject{{QStringLiteral("name"), QStringLiteral("scrypt")},
                           {QStringLiteral("n"), static_cast<qint64>(cost.n)},
                           {QStringLiteral("r"), static_cast<qint64>(cost.r)},
                           {QStringLiteral("p"), static_cast<qint64>(cost.p)},
                           {QStringLiteral("salt"), base64Of(made.salt)}};
    made.key = SealedByHand::sealedUnder(made.derived, fileKey, "kvm-wall data key");
    // (The content under the first 32 bytes of the file key, padded with noughts: a key of
    // another length is one the program is to stop at before it looks at the content.)
    made.sealed = SealedByHand::sealedUnder(fileKey.leftJustified(32, '\0', true), inside, "export 1");
    return made;
}

// A plain file with these parts, as Qt writes it.
inline QByteArray plainFile(const QJsonObject& parts)
{
    QJsonObject file{{QStringLiteral("format"), QStringLiteral("qkvm-export")}, {QStringLiteral("version"), 1}};
    for (auto part = parts.begin(); part != parts.end(); ++part) {
        file[part.key()] = part.value();
    }
    return QJsonDocument(file).toJson(QJsonDocument::Compact);
}

} // namespace qkvm::core::testing
