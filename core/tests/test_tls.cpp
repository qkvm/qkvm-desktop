#include <memory>
#include <optional>

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTest>
#include <QUrl>

#include <qkvm/core/kvmd_console_line.h>
#include <qkvm/core/tls.h>

#include "kvm_by_hand.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmdConsoleLine;
using core::testing::KvmByHand;
using core::testing::StandIn;
using core::testing::Tls;

namespace {

Tls tlsOf(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

} // namespace

// The choice of TLS (tls.h), in a program in which a line to a KVM is the first thing made that
// could speak TLS: the other tests make a driver first, which chooses for everything after it.
// And what is decided by that choice, as the plain function it is.
class TestTls : public QObject {
    Q_OBJECT

private slots:
    // (No driver is made before the tests here: the first of them makes the first thing.)

    void aLineThatIsTheFirstThingMadeSpeaksWindowsOwnTls()
    {
        // A line's socket is a member of it, and a socket takes Qt's own pick of TLS when it is
        // made: OpenSSL, where one happens to be on this PC. Chosen after that, Windows' own is
        // refused, and Qt says so aloud.
        QTest::failOnWarning(QRegularExpression(u"another backend is already in use"_s));
        KvmdConsoleLine::Where nowhere;
        nowhere.address = QUrl(u"https://127.0.0.1:9"_s);
        nowhere.cookie = "auth_token=pass1";
        {
            // (Made and thrown away: it asks for its line a turn of the loop later, which does
            // not come.)
            const KvmdConsoleLine first(nowhere);
            QCOMPARE(QSslSocket::activeBackend(), u"schannel"_s);
        }
        QVERIFY(core::useWindowsOwnTls());

        // And a line is let in over TLS, by a KVM that is known by its certificate.
        KvmByHand kvm(tlsOf("first"));
        KvmdConsoleLine::Where where;
        where.address = kvm.address();
        where.query = "stream=0";
        where.cookie = "auth_token=pass1";
        where.knownByCertificate = true;
        where.certificate = StandIn::certificateIn(tlsOf("first").certificateFile).digest(QCryptographicHash::Sha256);
        KvmdConsoleLine line(where);
        const QSignalSpy opened(&line, &KvmdConsoleLine::opened);
        const QSignalSpy closed(&line, &KvmdConsoleLine::closed);
        QTRY_VERIFY(opened.count() == 1 || closed.count() == 1);
        QVERIFY2(closed.isEmpty(), qPrintable(closed.value(0).value(0).toString()));
        QVERIFY(line.isOpen());
    }

    void whetherAnAddressIsSpokenToIsDecidedByItsKindAndByWhoseTlsItIs_data()
    {
        QTest::addColumn<bool>("windowsOwn");
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("refused");
        // Windows' own TLS: nothing is refused for it.
        QTest::newRow("Windows' own, HTTPS") << true << u"https://192.168.1.20"_s << false;
        QTest::newRow("Windows' own, a live line over TLS")
            << true << u"wss://qkvm.example/api/v1/devices/a/live"_s << false;
        QTest::newRow("Windows' own, plain HTTP") << true << u"http://192.168.1.20"_s << false;
        // Another, or none: nothing is spoken to over TLS.
        QTest::newRow("not Windows' own, HTTPS") << false << u"https://192.168.1.20"_s << true;
        QTest::newRow("not Windows' own, HTTPS at a port and a place")
            << false << u"https://kvm.lan:8443/api/ws?stream=0"_s << true;
        QTest::newRow("not Windows' own, HTTPS in capitals") << false << u"HTTPS://KVM.LAN"_s << true;
        QTest::newRow("not Windows' own, HTTPS at an IPv6 address") << false << u"https://[::1]:8443"_s << true;
        QTest::newRow("not Windows' own, a live line over TLS")
            << false << u"wss://qkvm.example/api/v1/devices/a/live"_s << true;
        // Plain HTTP is none of this.
        QTest::newRow("not Windows' own, plain HTTP") << false << u"http://192.168.1.20"_s << false;
        QTest::newRow("not Windows' own, plain HTTP at a port") << false << u"http://kvm.lan:8080/"_s << false;
        QTest::newRow("not Windows' own, a live line in the clear") << false << u"ws://127.0.0.1:8787/x"_s << false;
        // (What is no address of either kind is each speaker's own to refuse.)
        QTest::newRow("not Windows' own, no address") << false << QString() << false;
        QTest::newRow("not Windows' own, a host that is called https") << false << u"http://https"_s << false;
    }

    void whetherAnAddressIsSpokenToIsDecidedByItsKindAndByWhoseTlsItIs()
    {
        QFETCH(bool, windowsOwn);
        QFETCH(QString, address);
        QFETCH(bool, refused);
        QCOMPARE(core::refusedForItsTls(windowsOwn, QUrl(address)), refused);
    }

    void whatIsSaidOfAnAddressThatIsRefusedForItIsOneCodeOfTheProgramsOwnAndOneSentence()
    {
        const base::Error said = core::damagedTls();
        QCOMPARE(said.code, std::string("desktop:tls_damaged"));
        QCOMPARE(said.code, std::string(core::tlsDamaged));
        // (With a colon, which no word of a server's or a KVM's has.)
        QVERIFY(said.code.find(':') != std::string::npos);
        QCOMPARE(said.message,
                 std::string("This copy of qKVM is damaged and speaks to nothing over HTTPS: install qKVM "
                             "anew."));
        QCOMPARE(said.message, std::string(core::tlsDamagedSentence));
    }
};

QTEST_GUILESS_MAIN(TestTls)

#include "test_tls.moc"
