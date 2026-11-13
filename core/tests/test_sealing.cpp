#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <QTest>

#include <qkvm/core/sealing.h>
#include <qkvm/core/wiped.h>

#include "windows_gcm_by_hand.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::SecretBytes;
using core::SecretKey;
using core::testing::sealedByWindows;

namespace {

SecretKey keyOf(const QByteArray& bytes)
{
    SecretKey key;
    std::copy_n(bytes.constData(), std::min<qsizetype>(bytes.size(), SecretKey::bytes), key.data());
    return key;
}

// What came of an opening: the bytes, or the code it was refused in.
QByteArray openedOr(const base::Result<SecretBytes>& opened)
{
    return opened ? opened.value().view().toByteArray() : QByteArray("refused: ") + opened.error().code.c_str();
}

QByteArray theKey()
{
    return QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

QByteArray theNonce()
{
    return QByteArray::fromHex("a0a1a2a3a4a5a6a7a8a9aaab");
}

QByteArray theContext()
{
    return "export 1";
}

QByteArray thePlain()
{
    return "what a file of the wall holds, made up for a test";
}

} // namespace

// Sealing with Windows' own AES-256-GCM: that it is that (the examples of the paper that
// described GCM, written into this file from memory, and each seen to be what Windows makes), and
// that what does not fit is not opened, whichever part of it is another.
class TestSealing : public QObject {
    Q_OBJECT

private slots:
    void aKeyOfNoughtsSealsNothingToTheKnownTag()
    {
        // McGrew and Viega, "The Galois/Counter Mode of Operation", test case 13: AES-256, a key
        // and a nonce of noughts, nothing to seal and nothing it is sealed for.
        const base::Result<QByteArray> sealed =
            core::sealing::seal(keyOf(QByteArray(32, '\0')), QByteArray(12, '\0'), QByteArray(), QByteArray());
        QVERIFY(sealed.ok());
        QCOMPARE(sealed.value().toHex(), QByteArray("530f8afbc74536b9a963b4f1c4cb738b"));
        // And it opens to nothing, which is not the same as not opening.
        const base::Result<SecretBytes> opened =
            core::sealing::open(keyOf(QByteArray(32, '\0')), QByteArray(12, '\0'), sealed.value(), QByteArray());
        QVERIFY(opened.ok());
        QCOMPARE(opened.value().size(), qsizetype{0});
    }

    void aKeyOfNoughtsSealsABlockOfNoughtsToTheKnownBytes()
    {
        // The same paper, test case 14: sixteen bytes of noughts.
        const base::Result<QByteArray> sealed =
            core::sealing::seal(keyOf(QByteArray(32, '\0')), QByteArray(12, '\0'), QByteArray(16, '\0'), QByteArray());
        QVERIFY(sealed.ok());
        QCOMPARE(sealed.value().toHex(), QByteArray("cea7403d4d606b6e074ec5d3baf39d18"
                                                    "d0d1c8a799996bf0265b98b5d48ab919"));
    }

    void whatItIsSealedForIsPartOfTheTag()
    {
        // The same paper, test case 16: AES-256 with a key, a nonce, sixty bytes, and twenty bytes
        // that it is sealed for.
        const QByteArray key = QByteArray::fromHex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
        const QByteArray nonce = QByteArray::fromHex("cafebabefacedbaddecaf888");
        const QByteArray plain = QByteArray::fromHex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                                                     "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
        const QByteArray context = QByteArray::fromHex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        const base::Result<QByteArray> sealed = core::sealing::seal(keyOf(key), nonce, plain, context);
        QVERIFY(sealed.ok());
        QCOMPARE(sealed.value().toHex(), QByteArray("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
                                                    "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"
                                                    "76fc6ece0f4e1768cddf8853bb2d551b"));
        QCOMPARE(openedOr(core::sealing::open(keyOf(key), nonce, sealed.value(), context)), plain);
    }

    void whatIsSealedOpensWithTheSameKeyNonceAndContext()
    {
        const base::Result<QByteArray> sealed =
            core::sealing::seal(keyOf(theKey()), theNonce(), thePlain(), theContext());
        QVERIFY(sealed.ok());
        // The bytes encrypted, and the tag of sixteen behind them.
        QCOMPARE(sealed.value().size(), thePlain().size() + core::sealing::tagBytes);
        QVERIFY(!sealed.value().contains(thePlain().left(8)));
        QCOMPARE(openedOr(core::sealing::open(keyOf(theKey()), theNonce(), sealed.value(), theContext())), thePlain());
    }

    void whatDoesNotFitHandsOutNothing_data()
    {
        QTest::addColumn<QByteArray>("key");
        QTest::addColumn<QByteArray>("nonce");
        QTest::addColumn<int>("turnedBit"); // of what was sealed; -1: none
        QTest::addColumn<QByteArray>("context");
        QByteArray otherKey = theKey();
        otherKey[31] = static_cast<char>(otherKey.at(31) ^ 1);
        QByteArray otherNonce = theNonce();
        otherNonce[0] = static_cast<char>(otherNonce.at(0) ^ 1);
        const int bitsOfThePlain = static_cast<int>(thePlain().size()) * 8;
        QTest::newRow("another key") << otherKey << theNonce() << -1 << theContext();
        QTest::newRow("another nonce") << theKey() << otherNonce << -1 << theContext();
        QTest::newRow("sealed for something else") << theKey() << theNonce() << -1 << QByteArray("export 2");
        QTest::newRow("sealed for nothing") << theKey() << theNonce() << -1 << QByteArray();
        QTest::newRow("its first bit turned") << theKey() << theNonce() << 0 << theContext();
        QTest::newRow("a bit in the middle turned") << theKey() << theNonce() << bitsOfThePlain / 2 << theContext();
        QTest::newRow("the last bit before the tag turned")
            << theKey() << theNonce() << bitsOfThePlain - 1 << theContext();
        QTest::newRow("the first bit of the tag turned") << theKey() << theNonce() << bitsOfThePlain << theContext();
        QTest::newRow("the last bit of the tag turned")
            << theKey() << theNonce() << bitsOfThePlain + 127 << theContext();
    }

    void whatDoesNotFitHandsOutNothing()
    {
        QFETCH(QByteArray, key);
        QFETCH(QByteArray, nonce);
        QFETCH(int, turnedBit);
        QFETCH(QByteArray, context);
        const base::Result<QByteArray> made =
            core::sealing::seal(keyOf(theKey()), theNonce(), thePlain(), theContext());
        QVERIFY(made.ok());
        QByteArray sealed = made.value();
        if (turnedBit >= 0) {
            sealed[turnedBit / 8] = static_cast<char>(sealed.at(turnedBit / 8) ^ (1 << (turnedBit % 8)));
        }
        const base::Result<SecretBytes> opened = core::sealing::open(keyOf(key), nonce, sealed, context);
        QVERIFY(!opened.ok());
        QCOMPARE(opened.error().code, std::string(core::sealing::doesNotOpen));
        // Nothing of what was sealed is in what is said of it.
        QVERIFY(!QByteArray::fromStdString(opened.error().message).contains(thePlain().left(8)));
    }

    void whatIsNoneThatAnythingOpensIsRefusedBeforeWindowsIsAsked_data()
    {
        QTest::addColumn<QByteArray>("nonce");
        QTest::addColumn<int>("bytesOfTheSealed"); // how much of it is handed over
        const int all = static_cast<int>(thePlain().size()) + 16;
        QTest::newRow("a nonce of eight bytes") << theNonce().left(8) << all;
        QTest::newRow("a nonce of eleven bytes") << theNonce().left(11) << all;
        QTest::newRow("a nonce of thirteen bytes") << theNonce() + QByteArray(1, 'x') << all;
        QTest::newRow("a nonce of sixteen bytes") << theNonce() + QByteArray(4, 'x') << all;
        QTest::newRow("no nonce") << QByteArray() << all;
        QTest::newRow("fewer bytes than a tag has") << theNonce() << 15;
        QTest::newRow("no bytes") << theNonce() << 0;
    }

    void whatIsNoneThatAnythingOpensIsRefusedBeforeWindowsIsAsked()
    {
        QFETCH(QByteArray, nonce);
        QFETCH(int, bytesOfTheSealed);
        const base::Result<QByteArray> made =
            core::sealing::seal(keyOf(theKey()), theNonce(), thePlain(), theContext());
        QVERIFY(made.ok());
        const base::Result<SecretBytes> opened =
            core::sealing::open(keyOf(theKey()), nonce, made.value().left(bytesOfTheSealed), theContext());
        QVERIFY(!opened.ok());
        QCOMPARE(opened.error().code, std::string(core::sealing::doesNotOpen));
    }

    void theProgramSealsAsWindowsDoesWhenItIsAskedByHand()
    {
        // The same key, nonce, bytes and context, sealed by the program and by a test that asks
        // Windows itself: the same bytes, and each opens what the other sealed.
        const QByteArray byHand = sealedByWindows(theKey(), theNonce(), thePlain(), theContext());
        QVERIFY(!byHand.isEmpty());
        QCOMPARE(core::sealing::seal(keyOf(theKey()), theNonce(), thePlain(), theContext()).value().toHex(),
                 byHand.toHex());
        QCOMPARE(openedOr(core::sealing::open(keyOf(theKey()), theNonce(), byHand, theContext())), thePlain());
    }

    void whatWasSealedWithANonceOfAnotherLengthIsNotOpened_data()
    {
        QTest::addColumn<int>("bytes");
        QTest::newRow("a nonce of eight bytes") << 8;
        QTest::newRow("a nonce of thirteen bytes") << 13;
        QTest::newRow("a nonce of sixteen bytes") << 16;
    }

    void whatWasSealedWithANonceOfAnotherLengthIsNotOpened()
    {
        QFETCH(int, bytes);
        // Sealed for real with such a nonce, by Windows asked by hand: it would open, to whoever
        // opened with nonces of any length. QKVM seals with twelve bytes, and nothing else is opened.
        const QByteArray nonce(bytes, 'n');
        const QByteArray sealed = sealedByWindows(theKey(), nonce, thePlain(), theContext());
        QVERIFY2(!sealed.isEmpty(), "Windows itself does not seal with a nonce of this length");
        QCOMPARE(openedOr(core::sealing::open(keyOf(theKey()), nonce, sealed, theContext())),
                 QByteArray("refused: ") + core::sealing::doesNotOpen);
    }

    void nothingIsSealedWithANonceOfAnotherLength()
    {
        for (const qsizetype length : {0, 8, 11, 13, 16}) {
            QVERIFY(!core::sealing::seal(keyOf(theKey()), QByteArray(length, 'n'), thePlain(), theContext()).ok());
        }
    }

    void theTagAloneStillHasToFit()
    {
        // Nothing sealed: sixteen bytes that are a tag, for this key, nonce and context only.
        const base::Result<QByteArray> made =
            core::sealing::seal(keyOf(theKey()), theNonce(), QByteArray(), theContext());
        QVERIFY(made.ok());
        QCOMPARE(made.value().size(), core::sealing::tagBytes);
        QCOMPARE(openedOr(core::sealing::open(keyOf(theKey()), theNonce(), made.value(), theContext())), QByteArray());
        QCOMPARE(openedOr(core::sealing::open(keyOf(theKey()), theNonce(), made.value(), QByteArray("export 2"))),
                 QByteArray("refused: ") + core::sealing::doesNotOpen);
    }

    void randomBytesAreNewEachTime()
    {
        QByteArray one(32, '\0');
        QByteArray other(32, '\0');
        QVERIFY(core::sealing::random(std::span<quint8>(reinterpret_cast<quint8*>(one.data()), 32)).ok());
        QVERIFY(core::sealing::random(std::span<quint8>(reinterpret_cast<quint8*>(other.data()), 32)).ok());
        QVERIFY(one != other);
        QVERIFY(one != QByteArray(32, '\0'));
        // And none are asked for where there is no room for any.
        QVERIFY(core::sealing::random(std::span<quint8>()).ok());
    }

    void aKeyThatIsMovedIsInOnePlaceAfterwards()
    {
        SecretKey first = keyOf(theKey());
        const SecretKey second = std::move(first);
        QCOMPARE(second.view().toByteArray(), theKey());
        // NOLINTNEXTLINE(bugprone-use-after-move): what is left behind is what is looked at
        QCOMPARE(first.view().toByteArray(), QByteArray(32, '\0'));
        QCOMPARE(second.copy().view().toByteArray(), theKey());
    }

    void aKeyIsWipedWhenItGoes()
    {
        // The room a key stood in, looked at once the key is gone: it is the test's own room
        // (an optional's), and still there.
        std::optional<SecretKey> key;
        SecretKey& held = key.emplace(keyOf(theKey()));
        const quint8* room = held.data();
        QVERIFY(std::equal(room, room + SecretKey::bytes, theKey().cbegin(),
                           [](quint8 byte, char expected) { return byte == static_cast<quint8>(expected); }));
        key.reset();
        QVERIFY(std::all_of(room, room + SecretKey::bytes, [](quint8 byte) { return byte == 0; }));
    }

    void secretBytesHoldWhatFitsAndNoMore()
    {
        SecretBytes bytes(8);
        QVERIFY(bytes.append("12345"));
        QVERIFY(!bytes.append("6789"));
        QCOMPARE(bytes.view().toByteArray(), QByteArray("12345"));
        QVERIFY(bytes.append("678"));
        QCOMPARE(bytes.view().toByteArray(), QByteArray("12345678"));
        SecretBytes moved = std::move(bytes);
        QCOMPARE(moved.view().toByteArray(), QByteArray("12345678"));
        // NOLINTNEXTLINE(bugprone-use-after-move): what is left behind is what is looked at
        QCOMPARE(bytes.size(), qsizetype{0});
    }

    void whatHoldsASecretIsWrittenOverOnEveryWayOutButTheOneThatHandsItOn()
    {
        // Each looked at once what held it has gone, where it is the test's own and still there.
        QByteArray bytes("a password, as the store hands it over");
        const qsizetype length = bytes.size();
        {
            const core::WipedOnLeaving over(bytes);
        }
        QCOMPARE(bytes, QByteArray(length, '\0'));
        // So on the way an exception takes.
        // (Made at run time, as one read is: a text written in the program is nobody's to write over.)
        QString text = QString::fromLatin1("a password, as it was read");
        const qsizetype signs = text.size();
        bool caught = false;
        try {
            const core::WipedOnLeaving over(text);
            throw std::runtime_error("on its way out");
        } catch (const std::runtime_error&) {
            caught = true;
        }
        QVERIFY(caught);
        QCOMPARE(text, QString(signs, QChar(0)));
        // What is handed on is the receiver's, whole; what held it holds nothing then.
        QString held = u"handed on"_s;
        QString handed;
        {
            core::WipedOnLeaving over(held);
            handed = over.handOn();
        }
        QCOMPARE(handed, u"handed on"_s);
        QVERIFY(held.isEmpty());
        // What another holds as well is that one's: it is not written over, and not copied to be.
        const QByteArray theStores("what a store in memory holds");
        QByteArray handedOver = theStores;
        {
            const core::WipedOnLeaving over(handedOver);
        }
        QCOMPARE(theStores, QByteArray("what a store in memory holds"));
        QVERIFY(handedOver.isSharedWith(theStores));
    }

    void whatWindowsHandsOverIsCopiedOutAndWrittenOverWhereItIs()
    {
        // Memory that is somebody else's to free, as Windows hands an entry over: here the test's.
        std::array<char, 16> blob{};
        const QByteArray secret("sixteen bytes!!!");
        std::copy(secret.cbegin(), secret.cend(), blob.begin());
        QCOMPARE(core::takenOut(blob.data(), blob.size(), 2560), secret);
        QVERIFY(std::all_of(blob.cbegin(), blob.cend(), [](char byte) { return byte == 0; }));
        // Longer than what is kept: nothing is copied, and it is written over all the same.
        std::copy(secret.cbegin(), secret.cend(), blob.begin());
        QCOMPARE(core::takenOut(blob.data(), blob.size(), 15), QByteArray());
        QVERIFY(std::all_of(blob.cbegin(), blob.cend(), [](char byte) { return byte == 0; }));
        // Nowhere, or nothing: nothing.
        QCOMPARE(core::takenOut(nullptr, 16, 2560), QByteArray());
        QCOMPARE(core::takenOut(blob.data(), 0, 2560), QByteArray());
    }

    void aTextIsItsBytesInUtf8AndHalfACharacterIsNone()
    {
        const char32_t wide = 0x1F600;
        const QString text = u"a "_s + QChar(0xfc) + QChar(0x20ac) + QString::fromUcs4(&wide, 1);
        const auto bytesOf = [](const QString& written) {
            const std::optional<SecretBytes> bytes = SecretBytes::utf8Of(written);
            return bytes ? bytes->view().toByteArray() : QByteArray("no text");
        };
        QCOMPARE(bytesOf(text), text.toUtf8());
        QCOMPARE(bytesOf(QString()), QByteArray());
        QVERIFY(SecretBytes::utf8Of(QString()).has_value());
        // A surrogate by itself, first or second of the two: no text, and no bytes.
        QVERIFY(!SecretBytes::utf8Of(u"a"_s + QChar(0xd83d)).has_value());
        QVERIFY(!SecretBytes::utf8Of(QChar(0xde00) + u"a"_s).has_value());
        QVERIFY(!SecretBytes::utf8Of(QString(QChar(0xd83d)) + QChar(0xd83d)).has_value());
    }
};

QTEST_GUILESS_MAIN(TestSealing)
#include "test_sealing.moc"
