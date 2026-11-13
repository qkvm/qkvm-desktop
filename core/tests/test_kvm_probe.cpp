#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslError>
#include <QTcpServer>
#include <QTest>

#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvmd_device.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmProbe;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

namespace {

// The three questions a probe asks, each of them the one a family of devices answers to anyone.
constexpr const char* kvmdsQuestion = "/api/auth/check";
constexpr const char* jetKvmsQuestion = "/device/status";
constexpr const char* redfishsQuestion = "/redfish/v1/";

QByteArray written(const QJsonObject& said)
{
    return QJsonDocument(said).toJson(QJsonDocument::Compact);
}

QString sign(char32_t number)
{
    return QString::fromUcs4(&number, 1);
}

// What a kvmd answers to "who is signed in?" when nobody is (401 from one maker's, 403 from the
// other's), and when somebody is (200): in its own shape each time.
Answered asKvmd(int status)
{
    if (status == 200) {
        return {200, written({{u"ok"_s, true}, {u"result"_s, QJsonObject{}}})};
    }
    const QJsonObject why{{u"error"_s, status == 401 ? u"UnauthorizedError"_s : u"ForbiddenError"_s},
                          {u"error_msg"_s, u""_s}};
    return {status, written({{u"ok"_s, false}, {u"result"_s, why}})};
}

// The same when somebody is, made exactly `size` bytes long by something it has no reason to say.
QByteArray kvmdsAnswerOf(qsizetype size)
{
    const auto with = [](qsizetype filling) {
        return written({{u"ok"_s, true}, {u"result"_s, QJsonObject{{u"x"_s, QString(filling, u'x')}}}});
    };
    return with(size - with(0).size());
}

// An answer sent in pieces, each with its length in front, and nothing true said of the whole: the
// length the stand-in states for it is one byte, which Qt does not go by where an answer comes in
// pieces.
Answered inPieces(const QByteArray& whole)
{
    Answered answer(200, QByteArray::number(whole.size(), 16) + "\r\n" + whole + "\r\n0\r\n\r\n");
    answer.more = {"Transfer-Encoding: chunked"_ba};
    answer.shortBy = 1 - static_cast<int>(answer.body.size());
    return answer;
}

// What a JetKVM says of itself, and the root of a controller's Redfish service.
Answered asJetKvm()
{
    return {200, written({{u"isSetup"_s, true}})};
}
Answered asRedfish()
{
    return {200, written({{u"@odata.id"_s, u"/redfish/v1/"_s}, {u"RedfishVersion"_s, u"1.8.0"_s}})};
}

Answered noSuchPage()
{
    return {404, "<html><body><h1>404 Not Found</h1></body></html>"_ba, "text/html"_ba};
}
Answered hungUpOn()
{
    Answered gone;
    gone.hangUp = true;
    return gone;
}
Answered neverAnswered()
{
    Answered silent;
    silent.never = true;
    return silent;
}

// Something at an address, as far as a probe asks it: by question what a test has it answer, and to
// every other question what `otherwise` says, which is "no such page" until a test says otherwise.
class MadeUp {
public:
    explicit MadeUp(const std::optional<core::testing::Tls>& tls = {})
    {
        const auto answer = [this](const Asked& asked) { return says.value(asked.path, otherwise); };
        stand = tls ? std::make_unique<StandIn>(answer, *tls) : std::make_unique<StandIn>(answer);
    }

    QHash<QByteArray, Answered> says;
    Answered otherwise = noSuchPage();
    std::unique_ptr<StandIn> stand;

    QUrl address() const { return QUrl(stand->address()); }
    int asked(const char* question) const { return stand->count(question); }
};

// While one of these is there, the program's lines trust what these makers of certificates signed,
// as they trust what the makers that Windows knows signed.
class VouchedFor {
public:
    explicit VouchedFor(const QString& makersFile) : m_before(QSslConfiguration::defaultConfiguration())
    {
        QSslConfiguration with = m_before;
        with.addCaCertificates(QSslCertificate::fromPath(makersFile, QSsl::Pem));
        QSslConfiguration::setDefaultConfiguration(with);
    }
    ~VouchedFor() { QSslConfiguration::setDefaultConfiguration(m_before); }
    VouchedFor(const VouchedFor&) = delete;
    VouchedFor& operator=(const VouchedFor&) = delete;
    VouchedFor(VouchedFor&&) = delete;
    VouchedFor& operator=(VouchedFor&&) = delete;

private:
    QSslConfiguration m_before;
};

} // namespace

// What a typed address is made into, and what a probe finds at an address, against made-up devices
// of each family and against things that are none.
class TestKvmProbe : public QObject {
    Q_OBJECT

private:
    // The times an address is given, made short.
    static KvmProbe::Pace quick()
    {
        KvmProbe::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.withinMs = 3000;
        return pace;
    }
    // The same over HTTPS, with room for the lines to be let in: on the PC's slow cores, beside a build, under the
    // address sanitizer, a probe's three lines over TLS were not let in within quick()'s 1.5 s, every one (gate 36,
    // and ten runs of ten). The cases over HTTPS look at what was shown and asked, not at how soon.
    static KvmProbe::Pace overTls()
    {
        KvmProbe::Pace pace;
        pace.connectWithinMs = 10000;
        pace.quietMs = 10000;
        pace.withinMs = 20000;
        return pace;
    }
    static KvmProbe::Pace paceFor(const QUrl& address) { return address.scheme() == "https"_L1 ? overTls() : quick(); }

    static core::testing::Tls tls(const char* name)
    {
        const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
        return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
    }
    static QByteArray sha256Of(const char* name)
    {
        return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
    }

    // Asks what answers at an address, lets the program's loop run until the probe says, and gives
    // that. A probe that never says has failed the test, whatever is made of what this gives then.
    static KvmProbe::Found toldBy(KvmProbe& probe, const QUrl& address)
    {
        QList<KvmProbe::Found> told;
        probe.ask(address, [&told](const KvmProbe::Found& found) { told.append(found); });
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 60000)) {
            QTest::qFail("The probe never said what answers there.", __FILE__, __LINE__);
            return {};
        }
        return told.constFirst();
    }
    // The JetKVM's question alone, asked of an address (askForAJetKvm).
    static KvmProbe::Found toldAlone(KvmProbe& probe, const QUrl& address)
    {
        QList<KvmProbe::Found> told;
        probe.askForAJetKvm(address, [&told](const KvmProbe::Found& found) { told.append(found); });
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 60000)) {
            QTest::qFail("The probe never said what answers there.", __FILE__, __LINE__);
            return {};
        }
        return told.constFirst();
    }
    static KvmProbe::Found foundAt(const QUrl& address)
    {
        KvmProbe probe(paceFor(address));
        return toldBy(probe, address);
    }
    // The same of an asking that is held to what a KVM is known by.
    static KvmProbe::Found foundAsKnown(const QUrl& address, const KvmProbe::Known& known)
    {
        KvmProbe probe(paceFor(address));
        QList<KvmProbe::Found> told;
        probe.askAsKnown(address, known, [&told](const KvmProbe::Found& found) { told.append(found); });
        if (!QTest::qWaitFor([&told] { return !told.isEmpty(); }, 60000)) {
            QTest::qFail("The probe never said what answers there.", __FILE__, __LINE__);
            return {};
        }
        return told.constFirst();
    }

private slots:
    void initTestCase()
    {
        // The probe chooses Windows' own TLS, which is refused once TLS is in use: so one is made
        // before anything else touches TLS (the stand-ins do).
        const KvmProbe first;
    }

    void init()
    {
        // Whatever is at an address, a probe says nothing aloud about it: a warning of Qt's about
        // what a probe did, one for every question, would be a flood in somebody's log.
        QTest::failOnWarning();
    }

    void anAddressIsWrittenTheOneWay_data()
    {
        // Each of these beside what the server makes of the same text (device_url, tried with the
        // server's own lines of code).
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("written");
        QTest::newRow("numbers") << u"192.168.1.20"_s << u"https://192.168.1.20"_s;
        QTest::newRow("a name and a port") << u"kvm.lan:8443"_s << u"https://kvm.lan:8443"_s;
        QTest::newRow("pasted from a browser") << u"https://192.168.1.20/#/"_s << u"https://192.168.1.20"_s;
        QTest::newRow("capitals, blanks, a dot at the end") << u"  KVM.Lan.  "_s << u"https://kvm.lan"_s;
        QTest::newRow("more dots at the end") << u"kvm.lan..."_s << u"https://kvm.lan"_s;
        QTest::newRow("said to be plain") << u"http://kvm.lan"_s << u"http://kvm.lan"_s;
        QTest::newRow("all of a page's address") << u"HTTP://KVM.LAN:80/x?y#z"_s << u"http://kvm.lan"_s;
        QTest::newRow("the port https has anyway") << u"https://kvm.lan:443"_s << u"https://kvm.lan"_s;
        QTest::newRow("the port of the other way, plain") << u"http://kvm.lan:443"_s << u"http://kvm.lan:443"_s;
        QTest::newRow("the port of the other way") << u"https://kvm.lan:80"_s << u"https://kvm.lan:80"_s;
        QTest::newRow("a colon and no port") << u"kvm.lan:"_s << u"https://kvm.lan"_s;
        QTest::newRow("noughts before a port") << u"kvm.lan:00080"_s << u"https://kvm.lan:80"_s;
        QTest::newRow("the largest port") << u"https://kvm.lan:65535"_s << u"https://kvm.lan:65535"_s;
        QTest::newRow("an underscore") << u"kvm_1.lan"_s << u"https://kvm_1.lan"_s;
        QTest::newRow("a hyphen and a digit") << u"kvm-2"_s << u"https://kvm-2"_s;
        QTest::newRow("numbers and a dot") << u"1.2.3.4."_s << u"https://1.2.3.4"_s;
        QTest::newRow("a name that begins like numbers") << u"1.2.3.4a"_s << u"https://1.2.3.4a"_s;
        QTest::newRow("a name of digits and a letter") << u"1e3"_s << u"https://1e3"_s;
        QTest::newRow("a name written for another alphabet")
            << u"xn--mnchen-3ya.de"_s << u"https://xn--mnchen-3ya.de"_s;
        QTest::newRow("an at behind the host") << u"kvm.lan/a@b"_s << u"https://kvm.lan"_s;
        QTest::newRow("an at behind a cross") << u"kvm.lan#@x"_s << u"https://kvm.lan"_s;
        QTest::newRow("a way of speaking behind a path")
            << u"https://kvm.lan/x?to=http://other"_s << u"https://kvm.lan"_s;
        // (No "://": what stands before the first colon is the host, and "http" is a name.)
        QTest::newRow("a colon and one stroke") << u"http:/kvm.lan"_s << u"https://http"_s;
        QTest::newRow("this PC, the new way") << u"[::1]"_s << u"https://[::1]"_s;
        QTest::newRow("the same, written out") << u"[0:0:0:0:0:0:0:1]:8443"_s << u"https://[::1]:8443"_s;
        QTest::newRow("capitals in numbers") << u"http://[2001:DB8::1]:80"_s << u"http://[2001:db8::1]"_s;
        QTest::newRow("the longest gap is the one left out") << u"[1:0:0:2:0:0:0:3]"_s << u"https://[1:0:0:2::3]"_s;
        QTest::newRow("the first of two gaps") << u"[2001:db8:0:0:1:0:0:1]"_s << u"https://[2001:db8::1:0:0:1]"_s;
        QTest::newRow("a gap of one is written") << u"[1:2:3:4:5:6:7::]"_s << u"https://[1:2:3:4:5:6:7:0]"_s;
        QTest::newRow("no address at all") << u"[::]"_s << u"https://[::]"_s;
        QTest::newRow("a gap at the end") << u"[1::]"_s << u"https://[1::]"_s;
        QTest::newRow("noughts before a group") << u"[0001::1]"_s << u"https://[1::1]"_s;
        QTest::newRow("a colon and no port, in brackets") << u"[::1]:"_s << u"https://[::1]"_s;
        QTest::newRow("four plain numbers at the end")
            << u"[1:2:3:4:5:6:1.2.3.4]"_s << u"https://[1:2:3:4:5:6:102:304]"_s;
        QTest::newRow("the old way inside the new") << u"[::ffff:1.2.3.4]"_s << u"https://[::ffff:1.2.3.4]"_s;
    }

    void anAddressIsWrittenTheOneWay()
    {
        QFETCH(QString, typed);
        QFETCH(QString, written);
        const base::Result<QUrl> address = core::kvm::addressFrom(typed);
        QVERIFY2(address.ok(), address.ok() ? "" : address.error().message.c_str());
        QCOMPARE(address.value().toString(QUrl::FullyEncoded), written);
        // A scheme, a host, a port where one was typed, and nothing else.
        QVERIFY(address.value().path().isEmpty());
        QVERIFY(!address.value().hasQuery());
        QVERIFY(!address.value().hasFragment());
        QVERIFY(address.value().userInfo().isEmpty());
        // And typed as it is written, it is the same address.
        const base::Result<QUrl> again = core::kvm::addressFrom(written);
        QVERIFY(again.ok());
        QCOMPARE(again.value(), address.value());
    }

    void whatIsNoAddressIsRefusedInTheServersWords_data()
    {
        // The text as the server's sentence shows it: in quotes, and in small letters.
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("shown");
        const auto asItIs = [](const QString& typed) {
            QTest::newRow(qPrintable(typed)) << typed << u'\'' + typed + u'\'';
        };
        QTest::newRow("nothing") << u""_s << u"''"_s;
        QTest::newRow("a blank") << u" "_s << u"''"_s;
        // Only the two ways of speaking, and each written as it is.
        for (const QString& typed : {u"ftp://kvm.lan"_s, u"://kvm.lan"_s, u"//kvm.lan"_s, u"https://"_s,
                                     u"http:kvm.lan"_s, u"https:///kvm.lan"_s, u"kvm.lan?x=http://y"_s}) {
            asItIs(typed);
        }
        // A port is a number from 1 to 65535, and one port.
        for (const QString& typed :
             {u"kvm.lan:0"_s, u"kvm.lan:00000"_s, u"kvm.lan:x"_s, u"kvm.lan:65536"_s, u"kvm.lan:-1"_s, u"kvm.lan:+80"_s,
              u"kvm.lan:80:90"_s, u"kvm.lan:8 0"_s, u"kvm.lan:99999999999999999999"_s, u"[::1]:0"_s, u"[::1]:x"_s}) {
            asItIs(typed);
        }
        // Nobody's name and nobody's password.
        asItIs(u"user@kvm.lan"_s);
        asItIs(u"https://user:pw@kvm.lan"_s);
        asItIs(u"@kvm.lan"_s);
        // What looks like numbers is four of them, written the plain way.
        for (const QString& typed :
             {u"127.1"_s, u"010.1.1.1"_s, u"1.2.3"_s, u"1.2.3.4.5"_s, u"256.1.1.1"_s, u"1.2.3.1000"_s, u"1234"_s,
              u"0"_s, u"0x7f000001"_s, u"0x7f.1"_s, u"kvm.0x1f"_s, u"kvm.2"_s, u"1..2.3"_s}) {
            asItIs(typed);
        }
        // IPv6 stands in brackets, and without a network's name behind it.
        asItIs(u"::1"_s);
        asItIs(u"[fe80::1%eth0]"_s);
        // A host is made of letters, digits, dots, hyphens and underscores.
        for (const QString& typed : {u"kvm lan"_s, u"."_s, u"..."_s, u"kvm,lan"_s, u"kvm.lan;x"_s, u"kvm%2elan"_s}) {
            asItIs(typed);
        }
        QTest::newRow("a letter of another alphabet")
            << u"kvm.l"_s + sign(0xE4) + u"n"_s << u"'kvm.l"_s + sign(0xE4) + u"n'"_s;
        QTest::newRow("capitals") << u"KVM.LAN:x"_s << u"'kvm.lan:x'"_s;
        QTest::newRow("capitals of another alphabet")
            << u"KVM.L"_s + sign(0xC4) + u"N"_s << u"'kvm.l"_s + sign(0xE4) + u"n'"_s;
        // As the server's language writes a text out: in the other quotes where it has a quote in
        // it, a backslash before a backslash, and what does not print by its number.
        QTest::newRow("a quote") << u"It's"_s << u"\"it's\""_s;
        QTest::newRow("both quotes") << u"a\"b'c"_s << u"'a\"b\\'c'"_s;
        QTest::newRow("a backslash") << u"kvm.lan\\x"_s << u"'kvm.lan\\\\x'"_s;
        QTest::newRow("a sign that draws nothing") << u"kvm"_s + sign(0x200B) + u".lan"_s << u"'kvm\\u200b.lan'"_s;
        QTest::newRow("a blank of another kind") << u"kvm"_s + sign(0xA0) + u"lan"_s << u"'kvm\\xa0lan'"_s;
        QTest::newRow("a sign from far up that draws nothing")
            << u"kvm"_s + sign(0xE0001) + u".lan"_s << u"'kvm\\u000e0001.lan'"_s;
        QTest::newRow("a sign from far up that draws") << u"kvm"_s + sign(0x1F600) << u"'kvm"_s + sign(0x1F600) + u'\'';

        // Refused by the server as well, and in other words there: its reader of addresses says
        // of these what it thinks is wrong with them, and the server shows that. Here the one
        // sentence. A bracket that is alone, and something beside the brackets:
        for (const QString& typed : {u"[::1"_s, u"::1]"_s, u"[::1]x"_s, u"[::1]x:80"_s, u"x[::1]"_s}) {
            asItIs(typed);
        }
        // What stands in them and is no IPv6 address:
        for (const QString& typed : {u"[1.2.3.4]"_s, u"[kvm.lan]"_s, u"[]"_s, u"[::g]"_s, u"[ ::1]"_s, u"[::1.]"_s}) {
            asItIs(typed);
        }
        // Too few groups, too many, and a gap that is no gap or stands for nothing:
        for (const QString& typed : {u"[1:2:3:4:5:6:7]"_s, u"[1:2:3:4:5:6:7:8:9]"_s, u"[1::2::3]"_s, u"[:::]"_s,
                                     u"[1:::2]"_s, u"[:1:2:3:4:5:6:7]"_s, u"[1:2:3:4:5:6:7:]"_s}) {
            asItIs(typed);
        }
        for (const QString& typed : {u"[::1:2:3:4:5:6:7:8]"_s, u"[1:2:3:4:5:6:7:8::]"_s}) {
            asItIs(typed);
        }
        // A group that is none, and plain numbers that are none or not at the end.
        for (const QString& typed : {u"[00001::1]"_s, u"[0x1::1]"_s, u"[::+1]"_s, u"[::-1]"_s, u"[::1.2.3]"_s,
                                     u"[::1.2.3.04]"_s, u"[1.2.3.4::]"_s, u"[1:2:3:4:5:6:7:1.2.3.4]"_s}) {
            asItIs(typed);
        }

        // Taken by the server, and not here. Its reader of addresses drops a tab and a line's end
        // wherever they stand and reads on, and a sign that does not print in front of "https":
        // the second of these is another host to it than anybody saw.
        QTest::newRow("a tab in the name") << u"ho\tst.lan"_s << u"'ho\\tst.lan'"_s;
        QTest::newRow("a line's end in the numbers") << u"192.168.1.20\n1"_s << u"'192.168.1.20\\n1'"_s;
        QTest::newRow("a line's end before the port") << u"kvm.lan\r\n:8443"_s << u"'kvm.lan\\r\\n:8443'"_s;
        QTest::newRow("a sign that does not print in front")
            << sign(0x01) + u"https://kvm.lan"_s << u"'\\x01https://kvm.lan'"_s;
        // The sign for Kelvin is a "k" to the server once it has made small letters of the host.
        // (And in the sentence, which is in small letters, it is one here as well.)
        QTest::newRow("a letter that only looks like one") << sign(0x212A) + u"vm.lan"_s << u"'kvm.lan'"_s;
        // A second thing brackets may hold by the book, which no device is at.
        asItIs(u"[v1.a]"_s);
        // And what Qt will not connect to: it has the last word here, as the library that makes
        // the server's lines has it there. (That one was taken to take all four. Asked, the
        // server's own reader refuses the last as well: a name in its xn-- form that is none.)
        for (const QString& typed : {u"a..b"_s, u"-kvm.lan"_s, u"kvm-.lan"_s, u"xn--zzzzzz.de"_s}) {
            asItIs(typed);
        }
        QTest::newRow("a part of a name longer than one may be")
            << QString(64, u'a') + u".lan"_s << u'\'' + QString(64, u'a') + u".lan'"_s;
        // And a name that is numbers to Qt, written to the base of two: Qt would go to 0.0.0.0
        // for the first, and to 1.0.0.3 for the second.
        asItIs(u"0b0"_s);
        asItIs(u"1.0b11"_s);
    }

    void whatIsNoAddressIsRefusedInTheServersWords()
    {
        QFETCH(QString, typed);
        QFETCH(QString, shown);
        const base::Result<QUrl> address = core::kvm::addressFrom(typed);
        QVERIFY2(!address.ok(), qPrintable(address.ok() ? address.value().toString() : QString()));
        QCOMPARE(address.error().code, std::string("invalid"));
        QCOMPARE(QString::fromStdString(address.error().message), u"Not a usable device address: "_s + shown);
    }

    void qtReadsNumbersMoreLooselyThanTheServer()
    {
        // What the function leans on Qt for is the last word, and not the reading: left to read
        // numbers itself, Qt takes these for hosts, the first three for other ones than were
        // typed. Each of them is refused before Qt is asked (the rows above). If one of these
        // fails, Qt has become stricter, and nothing is lost.
        const auto hostToQt = [](const QString& typed) { return QUrl(u"https://"_s + typed, QUrl::StrictMode).host(); };
        QCOMPARE(hostToQt(u"127.1"_s), u"127.0.0.1"_s);
        QCOMPARE(hostToQt(u"010.1.1.1"_s), u"8.1.1.1"_s);
        QCOMPARE(hostToQt(u"0x7f.1"_s), u"127.0.0.1"_s);
        QCOMPARE(hostToQt(u"[0x1::1]"_s), u"1::1"_s);
        QCOMPARE(hostToQt(u"[::+1]"_s), u"::1"_s);
        // And numbers to the base of two, which are a name to the server.
        QCOMPARE(hostToQt(u"0b0"_s), u"0.0.0.0"_s);
        QCOMPARE(hostToQt(u"1.0b11"_s), u"1.0.0.3"_s);
        for (const QString& typed :
             {u"127.1"_s, u"010.1.1.1"_s, u"0x7f.1"_s, u"[0x1::1]"_s, u"[::+1]"_s, u"0b0"_s, u"1.0b11"_s}) {
            QVERIFY2(!core::kvm::addressFrom(typed).ok(), qPrintable(typed));
        }
    }

    void qtWritesOneAddressOtherwiseThanTheServer()
    {
        // The old way inside the new, as it was written before "::ffff:" was made for that. The
        // server writes its four numbers as two groups ("::102:304"), Qt as they were typed, and
        // an address is Qt's to write here. No device is at one of these.
        const base::Result<QUrl> address = core::kvm::addressFrom(u"[::1.2.3.4]"_s);
        QVERIFY(address.ok());
        QCOMPARE(address.value().toString(QUrl::FullyEncoded), u"https://[::1.2.3.4]"_s);
        QCOMPARE(core::kvm::addressFrom(u"[::102:304]"_s).value(), address.value());
    }

    void aTextLongerThanAnAddressIsReadIsRefused()
    {
        // As long as the server's own box takes one, it is read: what follows the host is dropped.
        const QString longest = u"kvm.lan/"_s + QString(core::kvm::longestTypedAddress - 8, u'a');
        QCOMPARE(longest.size(), core::kvm::longestTypedAddress);
        QCOMPARE(core::kvm::addressFrom(longest).value().toString(), u"https://kvm.lan"_s);
        // One sign more and it is not read at all, and the sentence shows as much of it as is.
        const base::Result<QUrl> refused = core::kvm::addressFrom(longest + u'a');
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("invalid"));
        QCOMPARE(QString::fromStdString(refused.error().message),
                 u"Not a usable device address: '"_s + longest + u"...'"_s);
        // (Counted as it was typed, blanks and all, as the server counts it.)
        QVERIFY(!core::kvm::addressFrom(u"kvm.lan"_s + QString(core::kvm::longestTypedAddress, u' ')).ok());
        // And never cut through the middle of a sign that takes two places.
        const QString twoPlaces = QString(core::kvm::longestTypedAddress - 1, u'a') + sign(0x1F600);
        QCOMPARE(QString::fromStdString(core::kvm::addressFrom(twoPlaces).error().message),
                 u"Not a usable device address: '"_s + QString(core::kvm::longestTypedAddress - 1, u'a') + u"...'"_s);
    }

    void whetherAWayOfSpeakingWasTyped()
    {
        QVERIFY(core::kvm::schemeWasTyped(u"https://kvm.lan"_s));
        QVERIFY(core::kvm::schemeWasTyped(u"  http://kvm.lan"_s));
        QVERIFY(core::kvm::schemeWasTyped(u"HTTPS://KVM.LAN"_s));
        QVERIFY(!core::kvm::schemeWasTyped(u"kvm.lan"_s));
        QVERIFY(!core::kvm::schemeWasTyped(u"kvm.lan:8443"_s));
        QVERIFY(!core::kvm::schemeWasTyped(u"http:/kvm.lan"_s));
        QVERIFY(!core::kvm::schemeWasTyped(u""_s));
        // As the server asks it: of the text, and not of what the text turns out to be.
        QVERIFY(core::kvm::schemeWasTyped(u"ftp://kvm.lan"_s));
    }

    void aKvmdIsKnownSignedInOrNot_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("nobody is signed in, as one maker says it") << 401;
        QTest::newRow("nobody is signed in, as the other says it") << 403;
        QTest::newRow("somebody is") << 200;
    }

    // --- a JetKVM's other way (M6 1b-ii) -------------------------------------------------------------

    void theOtherWayOfAnAddressIsAtTheSameHostWhereNoPortOfItsOwnIsWritten_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("other"); // empty: none
        // The web's own (test_the_other_way_of_an_address).
        QTest::newRow("plain, numbers") << u"http://192.168.1.40"_s << u"https://192.168.1.40"_s;
        QTest::newRow("https, a name") << u"https://jet.lan"_s << u"http://jet.lan"_s;
        QTest::newRow("the port plain has anyway") << u"http://192.168.1.40:80"_s << u"https://192.168.1.40"_s;
        QTest::newRow("the port https has anyway, IPv6") << u"https://[fd00::7]:443"_s << u"http://[fd00::7]"_s;
        QTest::newRow("IPv6, plain") << u"http://[fd00::7]"_s << u"https://[fd00::7]"_s;
        QTest::newRow("a port of its own, plain") << u"http://192.168.1.40:8080"_s << QString();
        QTest::newRow("a port of its own, https") << u"https://kvm.example.org:8443"_s << QString();
        QTest::newRow("https's port, plain") << u"http://jet.lan:443"_s << QString();
        QTest::newRow("plain's port, https") << u"https://jet.lan:80"_s << QString();
        QTest::newRow("no host") << u"http://"_s << QString();
        QTest::newRow("another way of speaking") << u"ftp://jet.lan"_s << QString();
    }

    void theOtherWayOfAnAddressIsAtTheSameHostWhereNoPortOfItsOwnIsWritten()
    {
        QFETCH(QString, address);
        QFETCH(QString, other);
        const std::optional<QUrl> found = core::kvm::otherWayOf(QUrl(address));
        QCOMPARE(found ? found->toString(QUrl::FullyEncoded) : QString(), other);
    }

    void aJetKvmIsAskedBesideAnAddressTheWayTheWebAsksIt_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("beside"); // empty: none
        // Typed without a way of speaking: the plain way at the same host, and the port as typed.
        QTest::newRow("a host") << u"jet.lan"_s << u"http://jet.lan"_s;
        QTest::newRow("numbers") << u"192.168.1.40"_s << u"http://192.168.1.40"_s;
        QTest::newRow("https's port, which the address does not write") << u"jet.lan:443"_s << u"http://jet.lan:443"_s;
        QTest::newRow("plain's port") << u"jet.lan:80"_s << u"http://jet.lan"_s;
        QTest::newRow("a port of its own") << u"jet.lan:8080"_s << u"http://jet.lan:8080"_s;
        QTest::newRow("IPv6") << u"[fd00::7]"_s << u"http://[fd00::7]"_s;
        QTest::newRow("IPv6 and a port") << u"[fd00::7]:8443"_s << u"http://[fd00::7]:8443"_s;
        QTest::newRow("blanks and a path") << u"  JET.lan/#/  "_s << u"http://jet.lan"_s;
        // Typed with one: the other way, where no port of its own was typed.
        QTest::newRow("https typed") << u"https://jet.lan"_s << u"http://jet.lan"_s;
        QTest::newRow("plain typed") << u"http://jet.lan"_s << u"https://jet.lan"_s;
        QTest::newRow("https typed with its own port") << u"https://jet.lan:443"_s << u"http://jet.lan"_s;
        QTest::newRow("https typed with a port") << u"https://jet.lan:8443"_s << QString();
        QTest::newRow("plain typed with a port") << u"http://jet.lan:8080"_s << QString();
    }

    void aJetKvmIsAskedBesideAnAddressTheWayTheWebAsksIt()
    {
        QFETCH(QString, typed);
        QFETCH(QString, beside);
        const base::Result<QUrl> address = core::kvm::addressFrom(typed);
        QVERIFY(address.ok());
        const std::optional<QUrl> found = core::kvm::bothWaysOf(typed, address.value());
        QCOMPARE(found ? found->toString(QUrl::FullyEncoded) : QString(), beside);
    }

    void aJetKvmsQuestionAloneIsAskedBesideAnAddress()
    {
        // Its status, and nothing else, with nothing in it that says who asks.
        MadeUp jet;
        jet.says.insert(jetKvmsQuestion, asJetKvm());
        KvmProbe probe(quick());
        const KvmProbe::Found found = toldAlone(probe, jet.address());
        QCOMPARE(found.kind, KvmProbe::Kind::JetKvm);
        QCOMPARE(jet.stand->asked.size(), 1);
        QCOMPARE(jet.stand->asked.constFirst().path, QByteArray(jetKvmsQuestion));
        QCOMPARE(jet.stand->asked.constFirst().method, QByteArray("GET"));
        QVERIFY(jet.stand->asked.constFirst().headers.value("cookie").isEmpty());
        QVERIFY(jet.stand->asked.constFirst().body.isEmpty());
        // What answers otherwise is something, and of no kind; where nothing does, nothing.
        MadeUp page;
        QCOMPARE(toldAlone(probe, page.address()).kind, KvmProbe::Kind::SomethingElse);
        QCOMPARE(page.stand->asked.size(), 1);
        MadeUp kvmd;
        kvmd.says.insert(kvmdsQuestion, asKvmd(401));
        QCOMPARE(toldAlone(probe, kvmd.address()).kind, KvmProbe::Kind::SomethingElse);
        QCOMPARE(kvmd.asked(kvmdsQuestion), 0);
        const core::testing::NobodyThere nobody;
        QCOMPARE(toldAlone(probe, QUrl(nobody.address())).kind, KvmProbe::Kind::Nothing);
        // Over HTTPS its certificate is not judged, and is told.
        MadeUp encrypted(tls("first"));
        encrypted.says.insert(jetKvmsQuestion, asJetKvm());
        KvmProbe overHttps(overTls());
        const KvmProbe::Found shown = toldAlone(overHttps, encrypted.address());
        QCOMPARE(shown.kind, KvmProbe::Kind::JetKvm);
        QCOMPARE(shown.shown, StandIn::certificateIn(tls("first").certificateFile).digest(QCryptographicHash::Sha256));
        QVERIFY(!shown.trustedByWindows);
        QCOMPARE(encrypted.stand->asked.size(), 1);
    }

    void whetherAnythingListensIsReadFromTheQuestionsOwnLines()
    {
        // Refused outright on every line: nothing listens there. (The program's own times: Windows takes
        // a couple of seconds to say that a port on this PC refuses, and a line that is given up on
        // sooner than that, or that fell silent, is no refusal.)
        KvmProbe probe;
        const core::testing::NobodyThere nobody;
        KvmProbe::Found found = toldBy(probe, QUrl(u"https://127.0.0.1:%1"_s.arg(nobody.port())));
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(found.nothingListens);
        QVERIFY(toldBy(probe, QUrl(nobody.address())).nothingListens);
        // Something that takes the line and says nothing listens; so does whatever answers.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        KvmProbe::Pace briefly = quick();
        briefly.connectWithinMs = 500;
        briefly.withinMs = 2000;
        KvmProbe impatient(briefly);
        found = toldBy(impatient, QUrl(u"https://127.0.0.1:%1"_s.arg(silent.serverPort())));
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(!found.nothingListens);
        MadeUp page;
        QVERIFY(!toldBy(probe, page.address()).nothingListens);
        MadeUp gone;
        gone.otherwise = hungUpOn();
        found = toldBy(probe, gone.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(!found.nothingListens);
    }

    void aKvmdIsKnownSignedInOrNot()
    {
        QFETCH(int, status);
        MadeUp kvm;
        kvm.says.insert(kvmdsQuestion, asKvmd(status));
        const KvmProbe::Found found = foundAt(kvm.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
        // In the clear there is no certificate to tell of.
        QVERIFY(found.shown.isEmpty());
        QVERIFY(!found.trustedByWindows);
        // Each of the three questions once, and nothing else.
        QCOMPARE(kvm.stand->asked.size(), 3);
        QCOMPARE(kvm.asked(kvmdsQuestion), 1);
        QCOMPARE(kvm.asked(jetKvmsQuestion), 1);
        QCOMPARE(kvm.asked(redfishsQuestion), 1);
    }

    void aJetKvmIsKnownByWhatItSaysOfItself()
    {
        MadeUp jet;
        jet.says.insert(jetKvmsQuestion, asJetKvm());
        // (To what it does not know it answers with its page, as these do.)
        jet.otherwise = Answered{200, "<!doctype html><html><body>JetKVM</body></html>"_ba, "text/html"_ba};
        const KvmProbe::Found found = foundAt(jet.address());
        QCOMPARE(found.kind, KvmProbe::Kind::JetKvm);
        // Whatever it says under that name: one that is not set up yet is a JetKVM too.
        jet.says.insert(jetKvmsQuestion, Answered{200, written({{u"isSetup"_s, false}})});
        QCOMPARE(foundAt(jet.address()).kind, KvmProbe::Kind::JetKvm);
    }

    void aServersControllerIsKnownByItsRedfishRoot()
    {
        MadeUp controller;
        controller.says.insert(redfishsQuestion, asRedfish());
        // (Everything but that root it shows to nobody who is not signed in.)
        controller.otherwise = Answered{401, "{\"error\": \"unauthorized\"}"_ba};
        const KvmProbe::Found found = foundAt(controller.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Redfish);
        QVERIFY(found.shown.isEmpty());
    }

    void whatAnswersAsTwoIsTakenInTheServersOrder()
    {
        MadeUp both;
        both.says.insert(kvmdsQuestion, asKvmd(200));
        both.says.insert(jetKvmsQuestion, asJetKvm());
        both.says.insert(redfishsQuestion, asRedfish());
        QCOMPARE(foundAt(both.address()).kind, KvmProbe::Kind::JetKvm);
        both.says.remove(jetKvmsQuestion);
        QCOMPARE(foundAt(both.address()).kind, KvmProbe::Kind::Redfish);
        both.says.remove(redfishsQuestion);
        QCOMPARE(foundAt(both.address()).kind, KvmProbe::Kind::Kvmd);
    }

    void whatAnswersOtherwiseIsSomethingElse_data()
    {
        QTest::addColumn<QByteArray>("question");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("answer");
        QTest::addColumn<QByteArray>("kind");
        const QByteArray json = "application/json"_ba;
        const QByteArray page = "<html><body>Sign in</body></html>"_ba;
        const QByteArray kvmds = asKvmd(200).body;
        QTest::newRow("a page") << QByteArray(kvmdsQuestion) << 200 << page << "text/html"_ba;
        QTest::newRow("a page that says it is JSON") << QByteArray(kvmdsQuestion) << 200 << page << json;
        QTest::newRow("nothing in the answer") << QByteArray(kvmdsQuestion) << 200 << QByteArray() << json;
        // kvmd's shape, and not where or how kvmd answers in it.
        QTest::newRow("kvmd's shape, not found") << QByteArray(kvmdsQuestion) << 404 << kvmds << json;
        QTest::newRow("kvmd's shape, an error of its own") << QByteArray(kvmdsQuestion) << 500 << kvmds << json;
        QTest::newRow("kvmd's shape, sent elsewhere") << QByteArray(kvmdsQuestion) << 302 << kvmds << json;
        QTest::newRow("kvmd's shape to a JetKVM's question") << QByteArray(jetKvmsQuestion) << 200 << kvmds << json;
        QTest::newRow("kvmd's shape to a controller's") << QByteArray(redfishsQuestion) << 200 << kvmds << json;
        // Half of the shape is none.
        QTest::newRow("ok alone") << QByteArray(kvmdsQuestion) << 200 << "{\"ok\": true}"_ba << json;
        QTest::newRow("a result alone") << QByteArray(kvmdsQuestion) << 200 << "{\"result\": {}}"_ba << json;
        QTest::newRow("a result that is a list")
            << QByteArray(kvmdsQuestion) << 200 << "{\"ok\": true, \"result\": []}"_ba << json;
        QTest::newRow("the shape in a list")
            << QByteArray(kvmdsQuestion) << 200 << "[{\"ok\": true, \"result\": {}}]"_ba << json;
        QTest::newRow("the shape, broken off")
            << QByteArray(kvmdsQuestion) << 200 << "{\"ok\": true, \"result\": {}"_ba << json;
        // A JetKVM says so with a plain yes, and under a name, not in a text.
        QTest::newRow("a JetKVM's words, refused")
            << QByteArray(jetKvmsQuestion) << 401 << "{\"isSetup\": true}"_ba << json;
        QTest::newRow("a JetKVM's words in a list") << QByteArray(jetKvmsQuestion) << 200 << "[\"isSetup\"]"_ba << json;
        QTest::newRow("a JetKVM's words as a text") << QByteArray(jetKvmsQuestion) << 200 << "\"isSetup\""_ba << json;
        QTest::newRow("a JetKVM's words to kvmd's question")
            << QByteArray(kvmdsQuestion) << 200 << "{\"isSetup\": true}"_ba << json;
        // And a controller with the root of its service.
        QTest::newRow("a root that is refused")
            << QByteArray(redfishsQuestion) << 401 << "{\"RedfishVersion\": \"1.8.0\"}"_ba << json;
        QTest::newRow("a root without its version")
            << QByteArray(redfishsQuestion) << 200 << "{\"@odata.id\": \"/redfish/v1/\"}"_ba << json;
        QTest::newRow("a root's words to a JetKVM's question")
            << QByteArray(jetKvmsQuestion) << 200 << "{\"RedfishVersion\": \"1.8.0\"}"_ba << json;
    }

    void whatAnswersOtherwiseIsSomethingElse()
    {
        QFETCH(QByteArray, question);
        QFETCH(int, status);
        QFETCH(QByteArray, answer);
        QFETCH(QByteArray, kind);
        MadeUp other;
        other.says.insert(question, Answered{status, answer, kind});
        const KvmProbe::Found found = foundAt(other.address());
        QCOMPARE(found.kind, KvmProbe::Kind::SomethingElse);
        QVERIFY(found.shown.isEmpty());
        QVERIFY(!found.trustedByWindows);
    }

    void nobodyThereIsNothing()
    {
        const core::testing::NobodyThere nobody;
        KvmProbe::Found found = foundAt(QUrl(nobody.address()));
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(found.shown.isEmpty());
        QVERIFY(!found.trustedByWindows);
        // Nor where HTTPS was asked for.
        found = foundAt(QUrl(u"https://127.0.0.1:%1"_s.arg(nobody.port())));
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(found.shown.isEmpty());
    }

    void whatHangsUpOnEveryQuestionIsNothing()
    {
        MadeUp gone;
        gone.otherwise = hungUpOn();
        const KvmProbe::Found found = foundAt(gone.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        // And one answer is somebody.
        gone.says.insert(redfishsQuestion, noSuchPage());
        QCOMPARE(foundAt(gone.address()).kind, KvmProbe::Kind::SomethingElse);
    }

    void aGatewaysWordThatWhatIsBehindItIsAwayIsToldApartFromSomethingElse()
    {
        // What stands before a KVM (a reverse proxy, the near end of a tunnel) answers in the
        // KVM's place while the KVM does not: 502, 503 or 504, and a page of its own.
        const auto page = [](int status) {
            return Answered{status, "<html><body><h1>" + QByteArray::number(status) + " Bad Gateway</h1></body></html>",
                            "text/html"_ba};
        };
        const auto found = [](const Answered& kvmds, const Answered& jetKvms, const Answered& redfishs) {
            MadeUp there;
            there.says.insert(kvmdsQuestion, kvmds);
            there.says.insert(jetKvmsQuestion, jetKvms);
            there.says.insert(redfishsQuestion, redfishs);
            return foundAt(there.address());
        };
        // Each of the three numbers, to every question: something answers, and all it says is
        // that what is behind it does not.
        for (const int status : {502, 503, 504}) {
            const KvmProbe::Found said = found(page(status), page(status), page(status));
            QCOMPARE(said.kind, KvmProbe::Kind::SomethingElse);
            QVERIFY2(said.saidByAGatewayAlone, qPrintable(QString::number(status)));
        }
        // The same where the gateway answers one question and nothing comes of the others: all
        // that answered was the gateway.
        QVERIFY(found(page(502), hungUpOn(), hungUpOn()).saidByAGatewayAlone);
        QVERIFY(found(hungUpOn(), hungUpOn(), page(504)).saidByAGatewayAlone);
        // A page that is larger than is taken of an answer, and one that comes packed, are a
        // gateway's all the same: neither is anything kvmd says.
        const Answered large{502, QByteArray(core::kvm::largestProbeAnswerBytes + 1, 'x'), "text/html"_ba};
        Answered packed = page(503);
        packed.more = {"Content-Encoding: gzip"_ba};
        QVERIFY(found(large, page(502), page(502)).saidByAGatewayAlone);
        QVERIFY(found(packed, packed, packed).saidByAGatewayAlone);

        // What is something else stays what it is. One answer of another sort is somebody who
        // speaks for themselves: a page that is not found, a door that wants a sign-in, an error
        // of the thing's own, a page.
        const Answered signIn{401, "<html><body>Sign in</body></html>"_ba, "text/html"_ba};
        const Answered itsOwnPage{200, "<html><body>A printer</body></html>"_ba, "text/html"_ba};
        for (const Answered& other : {noSuchPage(), signIn, page(500), itsOwnPage}) {
            for (const KvmProbe::Found& said : {found(other, page(502), page(502)), found(page(502), other, page(502)),
                                                found(page(502), page(502), other), found(other, other, other)}) {
                QCOMPARE(said.kind, KvmProbe::Kind::SomethingElse);
                QVERIFY2(!said.saidByAGatewayAlone, qPrintable(QString::number(other.status)));
            }
        }
        // In kvmd's own shape it is kvmd that speaks, under whichever number: no gateway's word.
        const Answered kvmdsOwn{
            503, written({{u"ok"_s, false},
                          {u"result"_s, QJsonObject{{u"error"_s, u"UnavailableError"_s}, {u"error_msg"_s, u""_s}}}})};
        const KvmProbe::Found ofKvmd = found(kvmdsOwn, page(502), page(502));
        QCOMPARE(ofKvmd.kind, KvmProbe::Kind::SomethingElse);
        QVERIFY(!ofKvmd.saidByAGatewayAlone);
        // What says that it is a device is that device, whatever a gateway says beside it; and a
        // KVM is a KVM.
        const KvmProbe::Found jet = found(page(502), asJetKvm(), page(502));
        QCOMPARE(jet.kind, KvmProbe::Kind::JetKvm);
        QVERIFY(!jet.saidByAGatewayAlone);
        const KvmProbe::Found kvm = found(asKvmd(401), page(502), page(502));
        QCOMPARE(kvm.kind, KvmProbe::Kind::Kvmd);
        QVERIFY(!kvm.saidByAGatewayAlone);
        // And where nothing answers at all, no gateway said anything.
        const KvmProbe::Found nobody = found(hungUpOn(), hungUpOn(), hungUpOn());
        QCOMPARE(nobody.kind, KvmProbe::Kind::Nothing);
        QVERIFY(!nobody.saidByAGatewayAlone);
    }

    void whatSpeaksPlainlyWhereHttpsWasAskedIsNotAskedThePlainWay()
    {
        // One address, as it was given. A KVM that answers the plain way beside it is not gone
        // to by itself: what was typed with https, or with nothing, stays on HTTPS.
        MadeUp plain;
        plain.otherwise = asKvmd(200);
        const KvmProbe::Found found = foundAt(QUrl(u"https://127.0.0.1:%1"_s.arg(plain.stand->serverPort())));
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(plain.stand->asked.isEmpty());
        QVERIFY(plain.stand->notHttp > 0);
    }

    void whatSendsTheProgramElsewhereIsNotFollowed_data()
    {
        QTest::addColumn<int>("status");
        for (const int status : {301, 302, 303, 307, 308}) {
            QTest::newRow(QByteArray::number(status).constData()) << status;
        }
    }

    void whatSendsTheProgramElsewhereIsNotFollowed()
    {
        QFETCH(int, status);
        // Where it sends to, a kvmd would answer every question.
        MadeUp elsewhere;
        elsewhere.otherwise = asKvmd(200);
        const auto answer = [&elsewhere, status](const Asked& asked) {
            Answered moved(status, QByteArray());
            moved.more = {"Location: " + elsewhere.stand->address().toUtf8() + asked.path};
            return moved;
        };
        StandIn sends(answer);
        const KvmProbe::Found found = foundAt(QUrl(sends.address()));
        // An answer, and none that says what is there.
        QCOMPARE(found.kind, KvmProbe::Kind::SomethingElse);
        QCOMPARE(sends.asked.size(), 3);
        QVERIFY(elsewhere.stand->asked.isEmpty());
        QCOMPARE(elsewhere.stand->lines, 0);
    }

    void nothingThatSaysWhoAsksGoesOut()
    {
        // Whatever answers is handed nothing: no cookie it set a moment ago, and no sign-on, which
        // Qt would make by itself with the account of whoever is signed in to Windows, and send
        // the question again.
        MadeUp asks;
        for (const QByteArray& way : {"NTLM"_ba, "Negotiate"_ba, "Basic realm=kvm"_ba}) {
            Answered refused = asKvmd(401);
            refused.more = {"WWW-Authenticate: " + way, "Set-Cookie: auth_token=abc; Path=/"_ba};
            asks.otherwise = refused;
            KvmProbe probe(quick());
            const qsizetype before = asks.stand->asked.size();
            // Twice by one probe: what the first asking was told to keep, the second does not carry.
            for (int round = 0; round < 2; ++round) {
                const KvmProbe::Found found = toldBy(probe, asks.address());
                // (A kvmd it is all the same: it answered in kvmd's shape.)
                QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
            }
            // The three questions each time, and nothing of Qt's after them.
            QTest::qWait(200);
            QCOMPARE(asks.stand->asked.size(), before + 6);
        }
        // Nor a name and a password that an address carries, should one ever be handed in.
        QUrl carrying = asks.address();
        carrying.setUserName(u"admin"_s);
        carrying.setPassword(u"secret"_s);
        QCOMPARE(foundAt(carrying).kind, KvmProbe::Kind::Kvmd);
        QCOMPARE(asks.stand->asked.size(), 21);
        for (const Asked& one : std::as_const(asks.stand->asked)) {
            QCOMPARE(one.method, "GET"_ba);
            QVERIFY(!one.headers.contains("cookie"));
            QVERIFY(!one.headers.contains("authorization"));
            QVERIFY(!one.headers.contains("proxy-authorization"));
            // And every question asks for an answer that is not packed.
            QCOMPARE(one.headers.value("accept-encoding"), "identity"_ba);
            QVERIFY(one.body.isEmpty());
            QVERIFY(!one.path.contains("admin") && !one.path.contains("secret"));
        }
    }

    void anAnswerLargerThanIsTakenIsSomebodyAndOfNoKind()
    {
        // Nobody answers the other two questions: what is made of this one is all there is.
        MadeUp large;
        large.otherwise = hungUpOn();
        // Up to the limit an answer is read: a kvmd that has a lot to say is a kvmd.
        const QByteArray most = kvmdsAnswerOf(core::kvm::largestProbeAnswerBytes);
        QCOMPARE(most.size(), core::kvm::largestProbeAnswerBytes);
        large.says.insert(kvmdsQuestion, Answered{200, most});
        QCOMPARE(foundAt(large.address()).kind, KvmProbe::Kind::Kvmd);
        // One byte more is not read. Somebody is there all the same.
        large.says.insert(kvmdsQuestion, Answered{200, kvmdsAnswerOf(core::kvm::largestProbeAnswerBytes + 1)});
        QCOMPARE(foundAt(large.address()).kind, KvmProbe::Kind::SomethingElse);
        large.says.insert(kvmdsQuestion, Answered{200, kvmdsAnswerOf(8 * core::kvm::largestProbeAnswerBytes)});
        QCOMPARE(foundAt(large.address()).kind, KvmProbe::Kind::SomethingElse);
        // The same of a JetKVM's answer and a controller's.
        large.says.clear();
        QByteArray filled =
            written({{u"isSetup"_s, true}, {u"x"_s, QString(core::kvm::largestProbeAnswerBytes, u'x')}});
        large.says.insert(jetKvmsQuestion, Answered{200, filled});
        QCOMPARE(foundAt(large.address()).kind, KvmProbe::Kind::SomethingElse);
        large.says.clear();
        filled = written({{u"RedfishVersion"_s, u"1"_s}, {u"x"_s, QString(core::kvm::largestProbeAnswerBytes, u'x')}});
        large.says.insert(redfishsQuestion, Answered{200, filled});
        QCOMPARE(foundAt(large.address()).kind, KvmProbe::Kind::SomethingElse);
    }

    void anAnswerThatDoesNotSayHowLargeItIsIsCutOffAsItArrives()
    {
        // Nothing is known of its size until it is there.
        MadeUp pieces;
        pieces.otherwise = hungUpOn();
        // (An answer in pieces is read like any other.)
        pieces.says.insert(kvmdsQuestion, inPieces(asKvmd(200).body));
        QCOMPARE(foundAt(pieces.address()).kind, KvmProbe::Kind::Kvmd);
        pieces.says.insert(kvmdsQuestion, inPieces(kvmdsAnswerOf(core::kvm::largestProbeAnswerBytes)));
        QCOMPARE(foundAt(pieces.address()).kind, KvmProbe::Kind::Kvmd);
        // One that turns out larger than is taken is somebody, and of no kind: by a byte, by a
        // little, by a lot. And that is known as soon as it is there, not when its time is over.
        KvmProbe::Pace pace = quick();
        pace.quietMs = 3000;
        pace.withinMs = 6000;
        KvmProbe probe(pace);
        for (const qsizetype more : {qsizetype{1}, qsizetype{2}, core::kvm::largestProbeAnswerBytes - 1,
                                     core::kvm::largestProbeAnswerBytes, 7 * core::kvm::largestProbeAnswerBytes}) {
            pieces.says.insert(kvmdsQuestion, inPieces(kvmdsAnswerOf(core::kvm::largestProbeAnswerBytes + more)));
            QElapsedTimer watch;
            watch.start();
            QCOMPARE(toldBy(probe, pieces.address()).kind, KvmProbe::Kind::SomethingElse);
            QVERIFY2(watch.elapsed() < pace.quietMs, qPrintable(QString::number(watch.elapsed())));
        }
    }

    void qtStopsHandingOnAnAnswerThatIsLeftLyingInItsStore()
    {
        // What the probe leans on, and why it takes an answer out of Qt's store as it arrives.
        // Qt is told how much of an answer it may gather while nobody takes it: as the probe tells
        // it, one byte more than an answer may be, so that one that is too large shows itself. Of
        // an answer in pieces that is larger than that it hands on a first part, no larger than
        // its store, and nothing after it: whoever waits to see more than was allowed, to call it
        // too much, waits until the time for an answer is over. (If this ever fails, Qt hands on
        // what does not fit, and nothing is lost.) The first part fills the store to the last
        // byte, or not, as the pieces happen to come.
        const qsizetype allowed = core::kvm::largestProbeAnswerBytes;
        StandIn server([](const Asked&) { return inPieces(QByteArray(3 * allowed, 'y')); });
        QNetworkAccessManager network;
        QNetworkRequest request(QUrl(server.address() + u"/x"_s));
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setTransferTimeout(700);
        QNetworkReply* reply = network.get(request);
        reply->setReadBufferSize(allowed + 1);
        qint64 most = 0;
        QObject::connect(reply, &QNetworkReply::readyRead, reply,
                         [reply, &most] { most = std::max(most, reply->bytesAvailable()); });
        QSignalSpy over(reply, &QNetworkReply::finished);
        QVERIFY(over.wait(5000));
        QVERIFY(most > 0);
        QVERIFY2(most <= allowed + 1, qPrintable(QString::number(most)));
        QCOMPARE(reply->error(), QNetworkReply::TimeoutError);
        reply->deleteLater();
    }

    void aPackedAnswerIsSomebodyAndOfNoKind()
    {
        MadeUp packs;
        packs.otherwise = hungUpOn();
        for (const QByteArray& packing : {"gzip"_ba, "deflate"_ba, "br"_ba, "gzip, gzip"_ba, "x-unknown"_ba}) {
            Answered packed = asKvmd(200);
            packed.more = {"Content-Encoding: " + packing};
            packs.says.insert(kvmdsQuestion, packed);
            const KvmProbe::Found found = foundAt(packs.address());
            // Not unpacked, and so not read: it may be a kvmd, and is not taken for one.
            QCOMPARE(found.kind, KvmProbe::Kind::SomethingElse);
        }
        // (An answer that says it is not packed is not packed.)
        Answered asItIs = asKvmd(200);
        asItIs.more = {"Content-Encoding: identity"_ba};
        packs.says.insert(kvmdsQuestion, asItIs);
        QCOMPARE(foundAt(packs.address()).kind, KvmProbe::Kind::Kvmd);
    }

    void theCertificateShownIsToldAndThatWindowsObjectsToIt()
    {
        // Nobody vouches for it, and that is no reason to stop: nothing secret is sent.
        MadeUp kvm(tls("first"));
        kvm.says.insert(kvmdsQuestion, asKvmd(403));
        KvmProbe::Found found = foundAt(kvm.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
        QCOMPARE(found.shown, sha256Of("first"));
        QVERIFY(!found.trustedByWindows);
        QCOMPARE(kvm.stand->asked.size(), 3);
        // As these KVMs make them: named "localhost", and good from 1970 to 1979.
        MadeUp comet(tls("comet"));
        comet.says.insert(kvmdsQuestion, asKvmd(401));
        found = foundAt(comet.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
        QCOMPARE(found.shown, sha256Of("comet"));
        QVERIFY(!found.trustedByWindows);
        // And of what is no KVM the certificate is told as well.
        MadeUp other(tls("second"));
        found = foundAt(other.address());
        QCOMPARE(found.kind, KvmProbe::Kind::SomethingElse);
        QCOMPARE(found.shown, sha256Of("second"));
        QVERIFY(!found.trustedByWindows);
    }

    void aCertificateWindowsTrustsIsToldAsTrusted()
    {
        MadeUp kvm(tls("signed"));
        kvm.says.insert(kvmdsQuestion, asKvmd(401));
        {
            const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
            const KvmProbe::Found found = foundAt(kvm.address());
            QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
            QCOMPARE(found.shown, sha256Of("signed"));
            QVERIFY(found.trustedByWindows);
        }
        // The same certificate with nobody to vouch for its maker is one Windows objects to.
        const KvmProbe::Found found = foundAt(kvm.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
        QCOMPARE(found.shown, sha256Of("signed"));
        QVERIFY(!found.trustedByWindows);
    }

    void aKvmThatIsKnownByACertificateIsAskedOnlyWhereItShowsThatOne()
    {
        // An asking that is held to what a KVM is known by: for a KVM that came to the wall with
        // nothing asked, before its password goes anywhere.
        MadeUp kvm(tls("first"));
        kvm.says.insert(kvmdsQuestion, asKvmd(401));
        // It shows the one it is known by: asked the three questions, as ever. (Windows was not
        // asked whether it trusts it: it was given nobody to vouch for anybody.)
        KvmProbe::Found found = foundAsKnown(kvm.address(), {true, sha256Of("first")});
        QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
        QVERIFY(!found.refused.has_value());
        QCOMPARE(found.shown, sha256Of("first"));
        QVERIFY(!found.trustedByWindows);
        QCOMPARE(kvm.stand->asked.size(), 3);
        // Known by another, it hears nothing: not one of the three questions. What it showed is told.
        found = foundAsKnown(kvm.address(), {true, sha256Of("second")});
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(found.refused == core::kvm::Objected::Changed);
        QCOMPARE(found.shown, sha256Of("first"));
        QCOMPARE(kvm.stand->asked.size(), 3);
        QCOMPARE(kvm.stand->notHttp, 0);
        // Nor where what is said to be its fingerprint is none: nothing shows that.
        for (const QByteArray& none : {QByteArray(), QByteArray(16, 'c'), sha256Of("first").left(31)}) {
            found = foundAsKnown(kvm.address(), {true, none});
            QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
            QVERIFY(found.refused == core::kvm::Objected::Changed);
        }
        QCOMPARE(kvm.stand->asked.size(), 3);
        // And what is no KVM, and shows the certificate, is told as what it is.
        MadeUp page(tls("second"));
        found = foundAsKnown(page.address(), {true, sha256Of("second")});
        QCOMPARE(found.kind, KvmProbe::Kind::SomethingElse);
        QVERIFY(!found.refused.has_value());
    }

    void aKvmThatIsKnownByWindowsWordIsAskedOnlyWhereWindowsTrustsWhatItShows()
    {
        MadeUp kvm(tls("signed"));
        kvm.says.insert(kvmdsQuestion, asKvmd(401));
        // Nobody vouches for what it shows: where ask() passes that over, this does not, and
        // nothing is said to it.
        KvmProbe::Found found = foundAsKnown(kvm.address(), {false, {}});
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(found.refused == core::kvm::Objected::Untrusted);
        QCOMPARE(found.shown, sha256Of("signed"));
        QVERIFY(!found.trustedByWindows);
        QVERIFY(kvm.stand->asked.isEmpty());
        {
            const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
            found = foundAsKnown(kvm.address(), {false, {}});
            QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
            QVERIFY(!found.refused.has_value());
            QVERIFY(found.trustedByWindows);
            QCOMPARE(kvm.stand->asked.size(), 3);
        }
        // A certificate a KVM made for itself is never one Windows trusts: such a KVM hears
        // nothing until it is known by that certificate.
        MadeUp own(tls("first"));
        own.says.insert(kvmdsQuestion, asKvmd(401));
        found = foundAsKnown(own.address(), {false, {}});
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(found.refused == core::kvm::Objected::Untrusted);
        QCOMPARE(found.shown, sha256Of("first"));
        QVERIFY(own.stand->asked.isEmpty());
    }

    void overPlainHttpThereIsNothingToHoldALineToAndItIsAskedAsEver()
    {
        MadeUp kvm;
        kvm.says.insert(kvmdsQuestion, asKvmd(401));
        for (const KvmProbe::Known& known : {KvmProbe::Known{false, {}}, KvmProbe::Known{true, sha256Of("first")}}) {
            const KvmProbe::Found found = foundAsKnown(kvm.address(), known);
            QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
            QVERIFY(!found.refused.has_value());
            QVERIFY(found.shown.isEmpty());
        }
        QCOMPARE(kvm.stand->asked.size(), 6);
    }

    void whatShowedACertificateAndSaidNothingHasStillShownIt()
    {
        MadeUp silent(tls("first"));
        silent.otherwise = hungUpOn();
        const KvmProbe::Found found = foundAt(silent.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QCOMPARE(found.shown, sha256Of("first"));
        QVERIFY(!found.trustedByWindows);
    }

    void qtSaysNothingOfTheCertificateOnALineThatIsThereAlready()
    {
        // What the probe leans on, and why every asking has lines of its own. On a line that was
        // made for an earlier question and kept, Qt does not say again that Windows objects to
        // what was shown: a probe that asked twice on such lines would take the second silence
        // for trust. (If this ever fails, Qt says it every time, and nothing is lost.)
        MadeUp kept(tls("first"));
        kept.stand->keepsLines = true;
        QNetworkAccessManager network;
        QNetworkRequest request(QUrl(kept.stand->address() + u"/x"_s));
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        int objections = 0;
        int agreements = 0;
        const auto shownTo = [&network, &request, &objections, &agreements] {
            QNetworkReply* reply = network.get(request);
            const auto objected = [&objections, reply](const QList<QSslError>& errors) {
                ++objections;
                reply->ignoreSslErrors(errors);
            };
            QObject::connect(reply, &QNetworkReply::sslErrors, reply, objected);
            QObject::connect(reply, &QNetworkReply::encrypted, reply, [&agreements] { ++agreements; });
            QSignalSpy over(reply, &QNetworkReply::finished);
            const bool ended = over.wait(5000);
            const QByteArray shown = reply->sslConfiguration().peerCertificate().digest(QCryptographicHash::Sha256);
            reply->deleteLater();
            return ended ? shown : QByteArray();
        };
        QCOMPARE(shownTo(), sha256Of("first"));
        // (Qt 6.12 says it twice of one line on Windows' own TLS. That it is said is what counts.)
        const int heardByTheFirst = objections;
        QVERIFY(heardByTheFirst >= 1);
        QCOMPARE(agreements, 1);
        // The questions after it travel on the first one's line, and hear nothing of it. What the
        // line showed can still be asked.
        for (int later = 0; later < 3; ++later) {
            QCOMPARE(shownTo(), sha256Of("first"));
        }
        QCOMPARE(kept.stand->lines, 1);
        QCOMPARE(kept.stand->asked.size(), 4);
        QCOMPARE(objections, heardByTheFirst);
        QCOMPARE(agreements, 1);
    }

    void everyAskingHearsOfTheCertificateAnew()
    {
        // A device that keeps its lines, as real ones do, asked again and again by one probe.
        MadeUp kvm(tls("first"));
        kvm.stand->keepsLines = true;
        kvm.says.insert(kvmdsQuestion, asKvmd(403));
        KvmProbe probe(overTls());
        for (int round = 0; round < 3; ++round) {
            const int lines = kvm.stand->lines;
            const KvmProbe::Found found = toldBy(probe, kvm.address());
            QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
            QCOMPARE(found.shown, sha256Of("first"));
            QVERIFY(!found.trustedByWindows);
            // On lines of its own, and none of the asking before.
            QVERIFY(kvm.stand->lines > lines);
        }
    }

    void anAskingIsGivenWhatTheDriverThatSignsInIsGivenForEachStep()
    {
        // What is not asked is not signed in to: an asking that gave up sooner than the driver
        // would kept a KVM off the wall that the driver reaches. So the two have their times
        // from one place (kvm_times.h), and the ones nobody chose are the same, step for step.
        // (They were two seconds to be let in and five in all once, under a driver with four
        // and a half and more.)
        const core::KvmdDevice::Pace driver;
        const KvmProbe::Pace asking;
        QCOMPARE(asking.connectWithinMs, driver.connectWithinMs);
        QCOMPARE(asking.quietMs, driver.quietMs);
        QCOMPARE(asking.withinMs, driver.connectWithinMs + driver.quietMs + driver.transferMs);
        // A KVM that lets a line in after three seconds is let in within them.
        QVERIFY(asking.connectWithinMs > 3000);
        // And an asking that goes before a sign-in by a driver with other times has those.
        const KvmProbe::Pace before = KvmProbe::Pace::beforeASignInWith(1200, 3400, 5600);
        QCOMPARE(before.connectWithinMs, 1200);
        QCOMPARE(before.quietMs, 3400);
        QCOMPARE(before.withinMs, 1200 + 3400 + 5600);
        QCOMPARE(before.rootQuietMs, 0);
        // (And before a controller's driver, its root's wait as well.)
        QCOMPARE(KvmProbe::Pace::beforeASignInWith(1200, 3400, 5600, 7800).rootQuietMs, 7800);
        // Nothing waits longer for a controller's root than for any answer unless it is told to.
        QCOMPARE(asking.rootQuietMs, 0);
    }

    void aControllersRootIsWaitedForAsItsDriverWaitsForIt()
    {
        // M7 1b-i (W9): a controller's service root that takes longer than an answer may (an older board took nine to
        // twenty seconds) is a controller's where the asking waits for it as long as the controller's driver would
        // (Pace::rootQuietMs); it was "something" otherwise. The other questions wait no longer for that.
        MadeUp slow;
        Answered late = asRedfish();
        late.afterMs = 1200;
        slow.says.insert("/redfish/v1/"_ba, late);
        KvmProbe::Pace pace = quick();
        pace.quietMs = 700;
        pace.withinMs = 2500;
        KvmProbe impatient(pace);
        QCOMPARE(toldBy(impatient, slow.address()).kind, KvmProbe::Kind::SomethingElse);
        pace.rootQuietMs = 2500;
        KvmProbe patient(pace);
        QCOMPARE(toldBy(patient, slow.address()).kind, KvmProbe::Kind::Redfish);
        // Where nothing answers the root at all, it is given up on within that wait.
        MadeUp silent;
        silent.says.insert("/redfish/v1/"_ba, neverAnswered());
        KvmProbe waits(pace);
        QElapsedTimer watch;
        watch.start();
        QCOMPARE(toldBy(waits, silent.address()).kind, KvmProbe::Kind::SomethingElse);
        QVERIFY2(watch.elapsed() >= pace.rootQuietMs - 100, qPrintable(QString::number(watch.elapsed())));
        QVERIFY2(watch.elapsed() < pace.withinMs + pace.rootQuietMs, qPrintable(QString::number(watch.elapsed())));
        // But where nothing at the address answers anything, the root is no slow controller's: given up on in an
        // answer's time, as every question is (a box that asks there is not kept waiting the longer time).
        MadeUp mute;
        mute.otherwise = neverAnswered();
        KvmProbe asksMute(pace);
        watch.restart();
        QCOMPARE(toldBy(asksMute, mute.address()).kind, KvmProbe::Kind::Nothing);
        QVERIFY2(watch.elapsed() < pace.rootQuietMs - 500, qPrintable(QString::number(watch.elapsed())));
    }

    void anAnswerThatNeverComesEndsWithinThePace()
    {
        // The question is taken, and nothing is ever said to it.
        MadeUp silent;
        silent.otherwise = neverAnswered();
        KvmProbe::Pace pace = quick();
        pace.connectWithinMs = 500;
        pace.quietMs = 700;
        pace.withinMs = 2500;
        KvmProbe probe(pace);
        QElapsedTimer watch;
        watch.start();
        const KvmProbe::Found found = toldBy(probe, silent.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        // Given up when nothing had come for too long, and not at once: the questions were out.
        QVERIFY2(watch.elapsed() >= pace.quietMs - 100, qPrintable(QString::number(watch.elapsed())));
        QVERIFY2(watch.elapsed() < pace.withinMs, qPrintable(QString::number(watch.elapsed())));
        QCOMPARE(silent.stand->asked.size(), 3);
    }

    void whatTakesTheLineAndNeverSpeaksIsNothingWithinThePace()
    {
        // Something that takes the line and never speaks, where a KVM would agree on how to speak.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        KvmProbe::Pace pace = quick();
        pace.connectWithinMs = 500;
        pace.quietMs = 2000;
        pace.withinMs = 4000;
        KvmProbe probe(pace);
        QElapsedTimer watch;
        watch.start();
        const KvmProbe::Found found = toldBy(probe, QUrl(u"https://127.0.0.1:%1"_s.arg(silent.serverPort())));
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY(found.shown.isEmpty());
        // Given up when it was not let in, long before the rest of its time was over.
        QVERIFY2(watch.elapsed() >= pace.connectWithinMs - 100, qPrintable(QString::number(watch.elapsed())));
        QVERIFY2(watch.elapsed() < pace.quietMs, qPrintable(QString::number(watch.elapsed())));
    }

    void anAnswerAByteAtATimeEndsWithinThePaceToo()
    {
        // Something always comes, so nothing is ever quiet for too long: and still an address may
        // not keep a question open for as long as it likes.
        MadeUp dripping;
        Answered slowly = asKvmd(200);
        slowly.body += QByteArray(1000, ' ');
        slowly.byteEveryMs = 50;
        dripping.otherwise = slowly;
        KvmProbe::Pace pace = quick();
        pace.connectWithinMs = 500;
        pace.quietMs = 500;
        pace.withinMs = 1200;
        KvmProbe probe(pace);
        QElapsedTimer watch;
        watch.start();
        const KvmProbe::Found found = toldBy(probe, dripping.address());
        // As the server has it: what did not answer in its time has not answered.
        QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        QVERIFY2(watch.elapsed() >= pace.withinMs - 150, qPrintable(QString::number(watch.elapsed())));
        QVERIFY2(watch.elapsed() < pace.withinMs + 1500, qPrintable(QString::number(watch.elapsed())));
    }

    void aKvmThatLeavesTheOtherQuestionsOpenIsStillTheKvm()
    {
        // It takes its time over its own question, within what it is given, and says nothing at
        // all to the two that are not its own.
        MadeUp kvm;
        Answered late = asKvmd(401);
        late.afterMs = 300;
        kvm.says.insert(kvmdsQuestion, late);
        kvm.otherwise = neverAnswered();
        KvmProbe::Pace pace = quick();
        pace.quietMs = 800;
        KvmProbe probe(pace);
        const KvmProbe::Found found = toldBy(probe, kvm.address());
        QCOMPARE(found.kind, KvmProbe::Kind::Kvmd);
    }

    void theAnswerComesLaterAndOnce()
    {
        MadeUp kvm;
        kvm.says.insert(kvmdsQuestion, asKvmd(401));
        KvmProbe probe(quick());
        QList<KvmProbe::Kind> told;
        QList<KvmProbe::Kind> toldOfNone;
        probe.ask(kvm.address(), [&told](const KvmProbe::Found& found) { told.append(found.kind); });
        // Also where there is nothing to wait for: an address that is none.
        probe.ask(QUrl(), [&toldOfNone](const KvmProbe::Found& found) { toldOfNone.append(found.kind); });
        QVERIFY(told.isEmpty());
        QVERIFY(toldOfNone.isEmpty());
        QTRY_COMPARE(told.size(), 1);
        QTRY_COMPARE(toldOfNone.size(), 1);
        QTest::qWait(300);
        QCOMPARE(told, QList<KvmProbe::Kind>{KvmProbe::Kind::Kvmd});
        QCOMPARE(toldOfNone, QList<KvmProbe::Kind>{KvmProbe::Kind::Nothing});
    }

    void anAddressThatIsNoneIsAskedNothing()
    {
        MadeUp kvm;
        kvm.otherwise = asKvmd(200);
        const QString port = QString::number(kvm.stand->serverPort());
        for (const QUrl& none : {QUrl(), QUrl(u"ftp://127.0.0.1:"_s + port), QUrl(u"ws://127.0.0.1:"_s + port),
                                 QUrl(u"127.0.0.1:"_s + port), QUrl(u"//127.0.0.1:"_s + port),
                                 QUrl(u"http://:"_s + port), QUrl(u"http://a b:"_s + port, QUrl::StrictMode)}) {
            const KvmProbe::Found found = foundAt(none);
            QCOMPARE(found.kind, KvmProbe::Kind::Nothing);
        }
        QTest::qWait(200);
        QVERIFY(kvm.stand->asked.isEmpty());
        QCOMPARE(kvm.stand->lines, 0);
        // And of a page's address the place is asked, and nothing of the page.
        QCOMPARE(foundAt(QUrl(kvm.stand->address() + u"/a/page?with=this#there"_s)).kind, KvmProbe::Kind::Kvmd);
        QCOMPARE(kvm.stand->asked.size(), 3);
        QCOMPARE(kvm.asked(kvmdsQuestion), 1);
        for (const Asked& one : std::as_const(kvm.stand->asked)) {
            QVERIFY(!one.path.contains("page") && !one.path.contains('?'));
        }
    }

    void aProbeThatIsGoneTellsNobody()
    {
        MadeUp kvm;
        Answered late = asKvmd(200);
        late.afterMs = 300;
        kvm.otherwise = late;
        int told = 0;
        {
            KvmProbe probe(quick());
            probe.ask(kvm.address(), [&told](const KvmProbe::Found&) { ++told; });
            // Gone while its questions are out.
            QTRY_COMPARE(kvm.stand->asked.size(), 3);
        }
        {
            // And gone before a question was out at all.
            KvmProbe probe(quick());
            probe.ask(kvm.address(), [&told](const KvmProbe::Found&) { ++told; });
            probe.ask(QUrl(), [&told](const KvmProbe::Found&) { ++told; });
        }
        QTest::qWait(800);
        QCOMPARE(told, 0);
        // Whoever is told may end the probe, a turn of the loop later.
        auto probe = std::make_unique<KvmProbe>(quick());
        probe->ask(kvm.address(), [&told, &probe](const KvmProbe::Found&) {
            ++told;
            probe.release()->deleteLater();
        });
        QTRY_COMPARE(told, 1);
        QTest::qWait(200);
        QCOMPARE(told, 1);
        QVERIFY(probe == nullptr);
    }

    void severalAddressesAreAskedAtOnceAndEachIsToldByItself()
    {
        MadeUp kvm;
        kvm.says.insert(kvmdsQuestion, asKvmd(403));
        MadeUp jet;
        jet.says.insert(jetKvmsQuestion, asJetKvm());
        MadeUp secure(tls("first"));
        secure.says.insert(redfishsQuestion, asRedfish());
        MadeUp other;
        const core::testing::NobodyThere nobody;
        KvmProbe probe(overTls()); // (one of them over HTTPS)
        QHash<QString, KvmProbe::Found> told;
        int tellings = 0;
        const auto ask = [&probe, &told, &tellings](const QString& name, const QUrl& address) {
            probe.ask(address, [&told, &tellings, name](const KvmProbe::Found& found) {
                told.insert(name, found);
                ++tellings;
            });
        };
        ask(u"kvm"_s, kvm.address());
        ask(u"jet"_s, jet.address());
        ask(u"secure"_s, secure.address());
        ask(u"other"_s, other.address());
        ask(u"nobody"_s, QUrl(nobody.address()));
        // The same address twice is two askings.
        ask(u"kvm again"_s, kvm.address());
        QTRY_COMPARE_WITH_TIMEOUT(tellings, 6, 60000);
        QCOMPARE(told.value(u"kvm"_s).kind, KvmProbe::Kind::Kvmd);
        QCOMPARE(told.value(u"kvm again"_s).kind, KvmProbe::Kind::Kvmd);
        QCOMPARE(told.value(u"jet"_s).kind, KvmProbe::Kind::JetKvm);
        QCOMPARE(told.value(u"secure"_s).kind, KvmProbe::Kind::Redfish);
        QCOMPARE(told.value(u"other"_s).kind, KvmProbe::Kind::SomethingElse);
        QCOMPARE(told.value(u"nobody"_s).kind, KvmProbe::Kind::Nothing);
        // What one of them showed is told of that one, and of no other.
        QCOMPARE(told.value(u"secure"_s).shown, sha256Of("first"));
        QVERIFY(told.value(u"kvm"_s).shown.isEmpty());
        QVERIFY(told.value(u"other"_s).shown.isEmpty());
        QCOMPARE(kvm.stand->asked.size(), 6);
    }
};

QTEST_GUILESS_MAIN(TestKvmProbe)
#include "test_kvm_probe.moc"
