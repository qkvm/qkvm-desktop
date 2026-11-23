#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include <QByteArray>
#include <QTest>

#include <qkvm/core/vnc_auth.h>

using namespace Qt::StringLiterals;
using namespace qkvm;
namespace vnc = core::vnc;

namespace {

std::array<std::uint8_t, vnc::challengeBytes> challengeOf(const char* hex)
{
    const QByteArray bytes = QByteArray::fromHex(hex);
    std::array<std::uint8_t, vnc::challengeBytes> made{};
    if (bytes.size() != static_cast<qsizetype>(made.size())) {
        qFatal("a challenge is sixteen bytes");
    }
    std::copy(bytes.begin(), bytes.end(), made.begin());
    return made;
}

QByteArray hexOf(const base::Result<vnc::Response>& answered)
{
    if (!answered.ok()) {
        return "error " + QByteArray::fromStdString(answered.error().code);
    }
    return QByteArray(reinterpret_cast<const char*>(answered.value().data()),
                      static_cast<qsizetype>(answered.value().size()))
        .toHex()
        .toUpper();
}

} // namespace

// The answer to a VNC server's challenge, as other implementations make it: the web's
// test_the_des_key_and_response_are_what_other_implementations_make, against Windows' own DES.
class TestVncAuth : public QObject {
    Q_OBJECT

private slots:
    void theAnswersAreJohnTheRippersPublishedVectors()
    {
        // (challenge, response, password) from John the Ripper's published test vectors for VNC authentication, which
        // neither this program nor the web had a hand in.
        struct Vector {
            const char* challenge;
            const char* response;
            const char* password;
        };
        const std::array<Vector, 5> vectors{{
            {"2F7532B3EFD17EEA5DD3A0949FFDF1D8", "0EB42D4D9AC1EF1B6EF6647B9594A621", "12345678"},
            {"7963F9BB7BA6A42A085763808156F570", "475B10D05648E4110D77F03916106F98", "123"},
            {"84076F040550EEA9341967633B5F3855", "DD96D21781A70DA49443279975404DD0", "pass1234"},
            {"0805B790B58E967F2A350A0C99DE3881", "AECB26FAEAAA62D79636A5934BAC1078", "Password"},
            {"ADDC021F444F999B8E27144C0DCE7389", "AFAF1BB57588784333962A124668A2C6", "openwall"},
        }};
        for (const Vector& one : vectors) {
            const auto challenge = challengeOf(one.challenge);
            QCOMPARE(hexOf(vnc::answer(QByteArrayView(one.password), challenge)), QByteArray(one.response));
            QCOMPARE(hexOf(vnc::answer(QString::fromLatin1(one.password), challenge)), QByteArray(one.response));
        }
    }

    void theDesIsTheStandardsOwnInEcbMode()
    {
        // FIPS 81's example of ECB: the key 0123456789ABCDEF, "Now is the time " in, 3FA40E8A984D4815 6A271787AB8883F9
        // out, each block by itself. The password whose key that is: each of its bytes with its bits mirrored.
        const QByteArray password = QByteArray::fromHex("80C4A2E691D5B3F7");
        const QByteArray plain = "Now is the time ";
        std::array<std::uint8_t, vnc::challengeBytes> challenge{};
        std::copy(plain.begin(), plain.end(), challenge.begin());
        QCOMPARE(hexOf(vnc::answer(QByteArrayView(password), challenge)),
                 QByteArray("3FA40E8A984D48156A271787AB8883F9"));
        // ECB: the same block in each half is the same block out (no chaining from the first to the second).
        std::copy_n(plain.begin(), 8, challenge.begin() + 8);
        QCOMPARE(hexOf(vnc::answer(QByteArrayView(password), challenge)),
                 QByteArray("3FA40E8A984D48153FA40E8A984D4815"));
    }

    void aPasswordCountsByItsFirstEightBytesOfUtf8()
    {
        const auto challenge = challengeOf("0805B790B58E967F2A350A0C99DE3881");
        // Longer than eight: the rest is not checked, by every server.
        QCOMPARE(hexOf(vnc::answer(u"Password and more"_s, challenge)), QByteArray("AECB26FAEAAA62D79636A5934BAC1078"));
        // A text as its UTF-8, cut where the eighth byte ends, in the middle of a character too.
        const QString accented = u"p"_s + QChar(0xE4) + u"sswo"_s + QChar(0xF6) + u"rd"_s;
        QCOMPARE(hexOf(vnc::answer(accented, challenge)),
                 hexOf(vnc::answer(QByteArrayView(accented.toUtf8().first(8)), challenge)));
        const QString cut = u"abcdefg"_s + QChar(0xE4);
        QCOMPARE(hexOf(vnc::answer(cut, challenge)),
                 hexOf(vnc::answer(QByteArrayView(QByteArray::fromHex("61626364656667C3")), challenge)));
        QVERIFY(hexOf(vnc::answer(u"Passwore"_s, challenge)) != QByteArray("AECB26FAEAAA62D79636A5934BAC1078"));
    }

    void aTextWithHalfACharacterIsNoPasswordAndIsNotSaid()
    {
        const auto challenge = challengeOf("0805B790B58E967F2A350A0C99DE3881");
        const QString half = u"Qx7"_s + QChar(0xD800) + u"zK9"_s;
        const base::Result<vnc::Response> answered = vnc::answer(half, challenge);
        QVERIFY(!answered.ok());
        QCOMPARE(answered.error().code, std::string(vnc::notAPassword));
        QVERIFY(!QString::fromStdString(answered.error().message).contains(u"Qx7"_s));
        QVERIFY(!QString::fromStdString(answered.error().message).contains(u"zK9"_s));
    }
};

QTEST_GUILESS_MAIN(TestVncAuth)
#include "test_vnc_auth.moc"
