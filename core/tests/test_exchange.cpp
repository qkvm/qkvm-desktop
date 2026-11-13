#include <algorithm>
#include <optional>
#include <string>
#include <type_traits>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUrl>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/strict_json.h>

#include "exchange_by_hand.h"
#include "windows_gcm_by_hand.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using namespace qkvm::core::testing;
using core::SecretBytes;
using core::exchange::Action;
using core::exchange::Content;
using core::exchange::Device;
using core::exchange::LinkAsWritten;

namespace {

// The server's sentences (app/qkvm/exchange.py), word for word.
const char* const notAFile = "not_a_file: This is not a file that qKVM made";
const char* const tooLarge = "too_large: The file is larger than anything qKVM writes";
const char* const newer = "newer: This file was made by a later version of qKVM than this one. Update this one first.";
const char* const passphraseNeeded =
    "passphrase_needed: This file is encrypted. Enter the passphrase it was made with.";
const char* const wrongPassphrase = "wrong_passphrase: That is not the passphrase this file was made with";
const char* const doesNotOpen = "damaged: The file is damaged: what is in it does not open with its own key";
const char* const notWhatIsWritten = "damaged: The file is damaged: what is in it is not what qKVM writes";

base::Result<Content> readPlain(const QByteArray& file)
{
    return core::exchange::read(file, QString());
}

// What a file with these parts is read to.
Content contentOf(const QJsonObject& parts)
{
    const base::Result<Content> read = readPlain(plainFile(parts));
    Content none;
    none.leftOut = QStringList{u"the file was refused: "_s + saidOf(read)};
    return read ? read.value() : none;
}

// An entry of a file's list of devices that is in order, with these fields added or made another.
QJsonObject deviceWith(const QJsonObject& fields)
{
    QJsonObject entry{{u"url"_s, u"https://192.168.1.20"_s}, {u"kind"_s, u"kvmd"_s}};
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        entry[field.key()] = field.value();
    }
    return entry;
}

Content devicesOf(const QJsonArray& entries)
{
    return contentOf({{u"devices"_s, entries}});
}

// The one device of a file that has one entry in order, with these fields.
Device deviceOf(const QJsonObject& fields)
{
    const Content content = devicesOf({deviceWith(fields)});
    return content.devices && devicesIn(content).size() == 1 && content.leftOut.isEmpty() ? devicesIn(content).value(0)
                                                                                          : Device();
}

// Why the one entry of a file was left out; nothing where it was not.
QString leftOutOf(const QJsonValue& entry, const QString& part = u"devices"_s)
{
    const Content content = contentOf({{part, QJsonArray{entry}}});
    return content.leftOut.size() == 1 ? content.leftOut.first() : content.leftOut.join(u" | "_s);
}

QJsonObject actionWith(const QJsonObject& fields)
{
    QJsonObject entry{{u"name"_s, u"Lock"_s}, {u"kind"_s, u"keys"_s}, {u"keys"_s, u"MetaLeft,KeyL"_s}};
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        entry[field.key()] = field.value();
    }
    return entry;
}

Action actionOf(const QJsonObject& fields)
{
    const Content content = contentOf({{u"actions"_s, QJsonArray{actionWith(fields)}}});
    return content.actions && actionsIn(content).size() == 1 && content.leftOut.isEmpty() ? actionsIn(content).value(0)
                                                                                          : Action();
}

// The certificate of the server's example files, as it stands in the plain one: read out of it
// by Qt, and not by the program.
QString theCertificate()
{
    const QJsonObject file = QJsonDocument::fromJson(exampleFile("plain.json")).object();
    return file[u"devices"_s].toArray().first().toObject()[u"cert_pem"_s].toString();
}

// Its bytes, by Qt's own reading of base64, which is lenient where the program's is not.
QByteArray derOf(const QString& pem)
{
    QString between = pem;
    between.remove(u"-----BEGIN CERTIFICATE-----"_s).remove(u"-----END CERTIFICATE-----"_s);
    return QByteArray::fromBase64(between.toLatin1());
}

// Bytes as a certificate is written: in base64, in lines of 64, between its two lines.
QString pemOf(const QByteArray& der)
{
    const QByteArray all = der.toBase64();
    QString pem = u"-----BEGIN CERTIFICATE-----\n"_s;
    for (qsizetype at = 0; at < all.size(); at += 64) {
        pem += QString::fromLatin1(all.mid(at, 64)) + u'\n';
    }
    return pem + u"-----END CERTIFICATE-----\n"_s;
}

// What the server's example files hold (make_export_examples.py), as the program hands it back:
// with their secrets, or as a plain file has them, without; and all parts, or the devices alone.
Content theExample(bool secrets, bool allParts)
{
    const QString pem = theCertificate();
    Device first;
    first.name = u"Rack 1, top"_s;
    first.address = u"https://192.168.1.21"_s;
    first.user = u"admin"_s;
    first.password = u"made-up-password-1"_s;
    first.kind = u"kvmd"_s;
    first.group = u"Rack 1"_s;
    first.tls = u"pinned"_s;
    first.certificatePem = pem;
    first.certificate = QCryptographicHash::hash(derOf(pem), QCryptographicHash::Sha256);
    Device second;
    second.name = u"Rack 1, plain"_s;
    second.address = u"http://192.168.1.22:8080"_s;
    second.user = u"operator"_s;
    second.password = u"made up, with a space and an umlaut: "_s + QChar(0xfc);
    second.kind = u"kvmd"_s;
    second.group = u"Rack 1"_s;
    second.tls = u"pinned"_s;
    second.consoleLinkAsWritten = LinkAsWritten(u"https://192.168.1.22/kvm/"_s);
    second.alerts = false;
    Device third;
    third.name = u"By Windows"_s;
    third.address = u"https://kvm.example.net"_s;
    third.user = u"admin"_s;
    third.password = u"made-up-password-3"_s;
    third.kind = u"kvmd"_s;
    third.tls = u"system"_s;
    Device fourth;
    fourth.name = u"A small one"_s;
    fourth.address = u"http://192.168.1.30"_s;
    fourth.user = u"admin"_s;
    fourth.password = u"made-up-password-4"_s;
    fourth.kind = u"jetkvm"_s;
    fourth.group = u"Desk"_s;
    fourth.tls = u"pinned"_s;
    fourth.ident = u"jetkvm:0123456789abcdef"_s;
    Device fifth;
    fifth.name = u"A server"_s;
    fifth.address = u"https://192.168.1.40"_s;
    fifth.user = u"root"_s;
    fifth.password = u"made-up-password-5"_s;
    fifth.kind = u"redfish"_s;
    fifth.group = u"Rack 2"_s;
    fifth.tls = u"insecure"_s;
    fifth.keepBoth = true;
    Content content;
    content.sealed = secrets;
    content.program = u"0.1.0"_s;
    QList<Device> devices{first, second, third, fourth, fifth};
    if (!secrets) {
        for (Device& device : devices) {
            device.password.clear();
        }
    }
    content.devices = devices;
    if (!allParts) {
        return content;
    }
    Action keys;
    keys.name = u"Ctrl+Alt+Del"_s;
    keys.keys = u"ControlLeft,AltLeft,Delete"_s;
    keys.danger = true;
    Action text;
    text.name = u"Sign in"_s;
    text.types = true;
    text.text = u"made-up text to type"_s;
    text.enter = true;
    // A saved text without its text is nothing to keep: a plain file does not have it at all.
    content.actions = secrets ? QList<Action>{keys, text} : QList<Action>{keys};
    content.alerts = core::exchange::Alerts();
    core::exchange::WallSettings wall;
    wall.pictureEverySeconds = 1.0;
    wall.asksForNewerVersions = true;
    content.wall = wall;
    return content;
}

// A file that is what QKVM writes but for one thing that stands in its head.
QByteArray fileWith(const QByteArray& head)
{
    return "{" + head + R"(, "devices": []})";
}

} // namespace

// The program's own reader of a wall as a file: that it reads what a QKVM server wrote to
// exactly what is in it, refuses what the server's reader refuses in the server's words, and
// takes no entry the server would leave out.
class TestExchange : public QObject {
    Q_OBJECT

private slots:
    // --- the files a QKVM server's own writer made --------------------------------------------------

    void aPlainFileOfTheServersIsReadToWhatIsInIt()
    {
        const QByteArray file = exampleFile("plain.json");
        QVERIFY(!file.isEmpty());
        QCOMPARE(saidOf(core::exchange::isSealed(file)), u"read"_s);
        QVERIFY(!core::exchange::isSealed(file).value());
        const base::Result<Content> read = core::exchange::read(file, QString());
        QCOMPARE(saidOf(read), u"read"_s);
        Content expected = theExample(false, true);
        expected.made = 1791832242;
        QCOMPARE(read.value().devices, expected.devices);
        QCOMPARE(read.value().actions, expected.actions);
        QCOMPARE(read.value().alerts, expected.alerts);
        QCOMPARE(read.value().wall, expected.wall);
        QCOMPARE(read.value().leftOut, QStringList());
        QVERIFY(read.value() == expected);
        QVERIFY(!read.value().hasSecrets());
        // And with a passphrase that it has no use for, it is the same file.
        QVERIFY(core::exchange::read(file, thePassphrase()).value() == expected);
    }

    void aSealedFileOfTheServersIsReadToWhatIsInIt_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<bool>("allParts");
        QTest::newRow("all of a wall") << QByteArray("sealed-quick.qkvm") << true;
        QTest::newRow("its devices alone") << QByteArray("sealed-devices-only.qkvm") << false;
    }

    void aSealedFileOfTheServersIsReadToWhatIsInIt()
    {
        QFETCH(QByteArray, name);
        QFETCH(bool, allParts);
        const QByteArray file = exampleFile(name.constData());
        QVERIFY(!file.isEmpty());
        QVERIFY(core::exchange::isSealed(file).value());
        // Nothing of the wall can be read off the file.
        QVERIFY(!file.contains("192.168.1.") && !file.contains("made-up") && !file.contains("CERTIFICATE"));
        const base::Result<Content> read = core::exchange::read(file, thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        Content expected = theExample(true, allParts);
        // (When it was made is the one thing the script that made it does not say.)
        QVERIFY2(read.value().made > 1.7e9 && read.value().made < 2e9, qPrintable(QString::number(read.value().made)));
        expected.made = read.value().made;
        QCOMPARE(read.value().devices, expected.devices);
        QCOMPARE(read.value().actions, expected.actions);
        QCOMPARE(read.value().alerts, expected.alerts);
        QCOMPARE(read.value().wall, expected.wall);
        QCOMPARE(read.value().leftOut, QStringList());
        QVERIFY(read.value() == expected);
        QVERIFY(read.value().hasSecrets());
        QCOMPARE(read.value().actions.has_value(), allParts);
    }

    void aFileSealedAtTheFullCostIsRead()
    {
        // As a QKVM server writes one: N = 2^17, which is 128 MiB and the one time in these tests
        // that it is paid.
        const QByteArray file = exampleFile("sealed-as-written.qkvm");
        QVERIFY(!file.isEmpty());
        QVERIFY(QJsonDocument::fromJson(file).object()[u"lock"_s].toObject()[u"kdf"_s].toObject()[u"n"_s].toInt() ==
                131072);
        QElapsedTimer watch;
        watch.start();
        const base::Result<Content> read = core::exchange::read(file, thePassphrase());
        qInfo("a file sealed at the full cost (N 2^17, r 8, p 1) was read in %lld ms",
              static_cast<long long>(watch.elapsed()));
        QCOMPARE(saidOf(read), u"read"_s);
        Content expected = theExample(true, true);
        expected.made = read.value().made;
        QVERIFY(read.value() == expected);
    }

    void aFileOfTheServersWhoseAlertsAreWrittenForOneServiceIsReadToWhatIsInIt()
    {
        // sealed-alerts-ntfy.qkvm (make_export_odd.py): alerts on, written as "ntfy" and not left
        // to the server, with an address; and the wall's settings. Nothing else.
        const QByteArray file = exampleFile("sealed-alerts-ntfy.qkvm");
        QVERIFY(!file.isEmpty());
        const base::Result<Content> read = core::exchange::read(file, thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        QCOMPARE(alertsIn(read.value()).format, u"ntfy"_s);
        core::exchange::Alerts alerts;
        alerts.enabled = true;
        alerts.format = u"ntfy"_s;
        alerts.offline = true;
        alerts.noSignal = false;
        alerts.problem = true;
        alerts.recovered = false;
        alerts.address = u"https://ntfy.example.net/qkvm-wall"_s;
        core::exchange::WallSettings wall;
        wall.pictureEverySeconds = 2.5;
        wall.asksForNewerVersions = false;
        Content expected;
        expected.sealed = true;
        expected.program = u"0.1.0"_s;
        expected.made = read.value().made;
        expected.alerts = alerts;
        expected.wall = wall;
        QCOMPARE(alertsIn(read.value()).address, alerts.address);
        QCOMPARE(read.value().alerts, expected.alerts);
        QCOMPARE(read.value().wall, expected.wall);
        QCOMPARE(read.value().leftOut, QStringList());
        QVERIFY(!read.value().devices.has_value() && !read.value().actions.has_value());
        QVERIFY(read.value() == expected);
    }

    void aFileOfTheServersWithDevicesAtTheEdgesIsReadDeviceByDevice()
    {
        // sealed-odd-devices.qkvm (make_export_odd.py): thirteen devices, one oddity each. The
        // server's own reader takes nine of them, and so does this: the same nine, the one at a
        // name in its "xn--" form among them.
        const QByteArray file = exampleFile("sealed-odd-devices.qkvm");
        QVERIFY(!file.isEmpty());
        const base::Result<Content> read = core::exchange::read(file, thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        const QChar umlaut(0xfc);
        // What is left out, each with its sentence: the server's, but for the two whose address
        // is none. There the server's sentence shows the address (the second with the password
        // that was written into it), and this one says whose it was and what is wrong with it.
        QCOMPARE(
            read.value().leftOut,
            (QStringList{
                u"Device 4 (\"an address with a name in letters beyond ASCII\"): its address has a sign in it that "
                "no name of a host has"_s,
                u"Device 7 (\"an address with a name and a password in it\"): its address has a user name or a "
                "password in it"_s,
                u"Device 11: the console link must not have a user name or password in it"_s,
                u"Device 13: its name, user name or group is not a short line of text"_s,
            }));
        for (const QString& sentence : read.value().leftOut) {
            QVERIFY2(!sentence.contains(u"made-up"_s) && !sentence.contains(u"nchen"_s), qPrintable(sentence));
        }
        // What is taken, each as the server takes it: with its password, and with its console
        // link as it was written.
        const auto device = [](const QString& name, const QString& address, const QString& tls) {
            Device made;
            made.name = name;
            made.address = address;
            made.user = u"admin"_s;
            made.password = u"made-up-password"_s;
            made.kind = u"kvmd"_s;
            made.tls = tls;
            return made;
        };
        const QString byWindows = u"system"_s;
        Device linkBeyondAscii =
            device(u"a console link with a name in letters beyond ASCII"_s, u"https://192.168.1.51"_s, byWindows);
        linkBeyondAscii.consoleLinkAsWritten = LinkAsWritten(u"https://b"_s + umlaut + u"ro.example/"_s);
        Device linkToNoPort =
            device(u"a console link with a port no computer has"_s, u"https://192.168.1.52"_s, byWindows);
        linkToNoPort.consoleLinkAsWritten = LinkAsWritten(u"https://console.example:99999/"_s);
        const QList<Device> taken{
            linkBeyondAscii,
            linkToNoPort,
            device(u"an address in the short form of a name beyond ASCII"_s, u"https://xn--mnchen-3ya.example"_s,
                   byWindows),
            device(u"an address of the new kind"_s, u"https://[fd00::1]:8443"_s, byWindows),
            device(u"a name with a tab in it"_s, u"https://192.168.1.56"_s, byWindows),
            device(u"an address in capitals"_s, u"https://kvm.example.net"_s, byWindows),
            // Pinned without a certificate: taken as pinned, with none.
            device(u"a pinned one without its certificate"_s, u"https://192.168.1.59"_s, u"pinned"_s),
            // Over plain HTTP, whatever the file says of its certificate: "pinned".
            device(u"a plain one that says insecure"_s, u"http://192.168.1.60"_s, u"pinned"_s),
            device(u"an address with a path"_s, u"https://192.168.1.62"_s, byWindows),
        };
        const QList<Device> found = devicesIn(read.value());
        QStringList names;
        for (const Device& one : found) {
            names.append(one.name);
        }
        QStringList expectedNames;
        for (const Device& one : taken) {
            expectedNames.append(one.name);
        }
        QCOMPARE(names, expectedNames);
        for (qsizetype at = 0; at < taken.size(); ++at) {
            QCOMPARE(found.value(at).address, taken.at(at).address);
            QCOMPARE(linkOf(found.value(at)), linkOf(taken.at(at)));
            QCOMPARE(found.value(at).tls, taken.at(at).tls);
            QCOMPARE(found.value(at).password, taken.at(at).password);
            QVERIFY2(found.value(at) == taken.at(at), qPrintable(taken.at(at).name));
        }
    }

    // --- what is refused, in the server's words -----------------------------------------------------

    void whatIsNoFileOfQkvmsIsRefused_data()
    {
        QTest::addColumn<QByteArray>("file");
        QTest::newRow("nothing") << QByteArray();
        QTest::newRow("no JSON") << QByteArray("not json at all");
        QTest::newRow("a list") << QByteArray("[]");
        QTest::newRow("a text") << QByteArray(R"("qkvm-export")");
        QTest::newRow("an object with nothing in it") << QByteArray("{}");
        QTest::newRow("lists a hundred thousand deep") << QByteArray(100000, '[');
        QTest::newRow("no format") << QByteArray(R"({"version": 1, "devices": []})");
        QTest::newRow("another format") << fileWith(R"("format": "something-else", "version": 1)");
        QTest::newRow("the format in capitals") << fileWith(R"("format": "QKVM-EXPORT", "version": 1)");
        QTest::newRow("a number as the format") << fileWith(R"("format": 1, "version": 1)");
        QTest::newRow("a list as the format") << fileWith(R"("format": ["qkvm-export"], "version": 1)");
        QTest::newRow("JSON cut off") << QByteArray(R"({"format": "qkvm-export", "version": 1, "devices": [)");
        QTest::newRow("JSON with something behind it") << fileWith(R"("format": "qkvm-export", "version": 1)") + "x";
        QTest::newRow("the mark some programs put at a file's head")
            << "\xEF\xBB\xBF" + fileWith(R"("format": "qkvm-export", "version": 1)");
        QTest::newRow("bytes that are no text")
            << fileWith("\"format\": \"qkvm-export\", \"version\": 1, \"x\": \"" + QByteArray("\xC3\x28") + "\"");
        // A version is a whole number from 1 on, written as one.
        QTest::newRow("no version") << fileWith(R"("format": "qkvm-export")");
        QTest::newRow("a version in quotes") << fileWith(R"("format": "qkvm-export", "version": "1")");
        QTest::newRow("version nought") << fileWith(R"("format": "qkvm-export", "version": 0)");
        QTest::newRow("a version below nought") << fileWith(R"("format": "qkvm-export", "version": -1)");
        QTest::newRow("a yes as the version") << fileWith(R"("format": "qkvm-export", "version": true)");
        QTest::newRow("nothing as the version") << fileWith(R"("format": "qkvm-export", "version": null)");
        QTest::newRow("a version with a point") << fileWith(R"("format": "qkvm-export", "version": 1.0)");
        QTest::newRow("a version with an exponent") << fileWith(R"("format": "qkvm-export", "version": 1e0)");
        QTest::newRow("a version and a half") << fileWith(R"("format": "qkvm-export", "version": 1.5)");
        QTest::newRow("a later version with a point") << fileWith(R"("format": "qkvm-export", "version": 2.0)");
        QTest::newRow("a list as the version") << fileWith(R"("format": "qkvm-export", "version": [1])");
        // The second of two is the one that counts, as for the server.
        QTest::newRow("a format, and then another")
            << fileWith(R"("format": "qkvm-export", "version": 1, "format": "x")");
    }

    void whatIsNoFileOfQkvmsIsRefused()
    {
        QFETCH(QByteArray, file);
        QCOMPARE(saidOf(readPlain(file)), QLatin1StringView(notAFile));
        QCOMPARE(saidOf(core::exchange::read(file, thePassphrase())), QLatin1StringView(notAFile));
        QCOMPARE(saidOf(core::exchange::isSealed(file)), QLatin1StringView(notAFile));
    }

    void aFileOfALaterQkvmIsSaidToBeOne_data()
    {
        QTest::addColumn<QByteArray>("version");
        QTest::newRow("the next version") << QByteArray("2");
        QTest::newRow("a far later one") << QByteArray("1000");
        QTest::newRow("a version longer than a number of this PC") << QByteArray("99999999999999999999");
    }

    void aFileOfALaterQkvmIsSaidToBeOne()
    {
        QFETCH(QByteArray, version);
        const QByteArray plain = fileWith(R"("format": "qkvm-export", "version": )" + version);
        QCOMPARE(saidOf(readPlain(plain)), QLatin1StringView(newer));
        if (version.size() > 9) {
            // (Qt's writer, which the sealed file below is made with, writes so long a number
            // with an exponent, and then it is no whole number any more.)
            return;
        }
        // A sealed one says so before it asks for a passphrase, and before any work.
        SealedByHand sealed = sealedByHand(R"({"devices": []})");
        sealed.head[u"version"_s] = QJsonValue(version.toDouble());
        QCOMPARE(saidOf(readPlain(sealed.text())), QLatin1StringView(newer));
        WatchedWork work;
        QCOMPARE(saidOf(core::exchange::readWith(sealed.text(), SecretBytes::utf8Of(thePassphrase()), work.derive())),
                 QLatin1StringView(newer));
        QCOMPARE(work.asked(), 0);
    }

    void aFileWithItsFirstFormatAsTheLastIsRead()
    {
        // (The other way round from the row above: what is written last counts.)
        const base::Result<Content> read =
            readPlain(fileWith(R"("format": "x", "version": 7, "format": "qkvm-export", "version": 1)"));
        QCOMPARE(saidOf(read), u"read"_s);
        QVERIFY(read.value().devices.has_value());
    }

    void aSealedFileNeedsItsPassphrase()
    {
        const QByteArray file = exampleFile("sealed-quick.qkvm");
        QCOMPARE(saidOf(readPlain(file)), QLatin1StringView(passphraseNeeded));
        // And says so before anything else of it is looked at: its lock may be none.
        SealedByHand sealed = sealedByHand(R"({"devices": []})");
        sealed.kdf = QJsonObject();
        QCOMPARE(saidOf(readPlain(sealed.text())), QLatin1StringView(passphraseNeeded));
        // Sealed is what has something under "sealed", whatever it is.
        QCOMPARE(saidOf(readPlain(fileWith(R"("format": "qkvm-export", "version": 1, "sealed": null)"))),
                 QLatin1StringView(passphraseNeeded));
    }

    void aWrongPassphraseIsSaidToBeWrong_data()
    {
        QTest::addColumn<QString>("passphrase");
        QTest::newRow("another passphrase") << u"another passphrase"_s;
        QTest::newRow("the passphrase with a blank behind it") << thePassphrase() + u' ';
        QTest::newRow("the passphrase in capitals") << thePassphrase().toUpper();
        QTest::newRow("one letter") << u"a"_s;
        // What is no text at all: half a character. No file was sealed with it.
        QTest::newRow("the passphrase and half a character") << thePassphrase() + QChar(0xd83d);
    }

    void aWrongPassphraseIsSaidToBeWrong()
    {
        QFETCH(QString, passphrase);
        QCOMPARE(saidOf(core::exchange::read(exampleFile("sealed-quick.qkvm"), passphrase)),
                 QLatin1StringView(wrongPassphrase));
    }

    void aPlainFileIsReadWhateverWasTypedAsItsPassphrase()
    {
        const QByteArray file = plainFile({{u"devices"_s, QJsonArray{deviceWith({})}}});
        QCOMPARE(saidOf(core::exchange::read(file, thePassphrase() + QChar(0xd83d))), u"read"_s);
    }

    void aLockThatAsksForMoreThanAFileMayIsNotWorkedOn_data()
    {
        QTest::addColumn<double>("n");
        QTest::addColumn<double>("r");
        QTest::addColumn<double>("p");
        // Each bound by itself, with the others as a file is written.
        QTest::newRow("N below 2^10") << 512.0 << 8.0 << 1.0;
        QTest::newRow("N above 2^20") << 2097152.0 << 1.0 << 1.0;
        QTest::newRow("N that is no power of two") << 4097.0 << 8.0 << 1.0;
        QTest::newRow("N that is no power of two, and even") << 6144.0 << 8.0 << 1.0;
        QTest::newRow("N of nought") << 0.0 << 8.0 << 1.0;
        QTest::newRow("N below nought") << -4096.0 << 8.0 << 1.0;
        QTest::newRow("r of nought") << 4096.0 << 0.0 << 1.0;
        QTest::newRow("r above 16") << 4096.0 << 17.0 << 1.0;
        QTest::newRow("p of nought") << 4096.0 << 8.0 << 0.0;
        QTest::newRow("p above 4") << 4096.0 << 8.0 << 5.0;
        // All three within their bounds, and more work together than twice what is written.
        QTest::newRow("N times r of 2^23") << 1048576.0 << 8.0 << 1.0;
        QTest::newRow("N times r of 2^22") << 131072.0 << 16.0 << 2.0;
        QTest::newRow("N times r times p of 2^22") << 131072.0 << 8.0 << 4.0;
        // What the work itself insists on: N below 2^(16r).
        QTest::newRow("N of 2^16 with r of one") << 65536.0 << 1.0 << 1.0;
        // Far beyond all of it.
        QTest::newRow("N of 2^40") << 1099511627776.0 << 8.0 << 1.0;
        QTest::newRow("r of 2^40") << 4096.0 << 1099511627776.0 << 1.0;
        QTest::newRow("N too large for any number") << 1e300 << 8.0 << 1.0;
        // Numbers that are not whole. (The server drops what is behind the point.)
        QTest::newRow("N and a half") << 4096.5 << 8.0 << 1.0;
        QTest::newRow("r and a half") << 4096.0 << 8.5 << 1.0;
        QTest::newRow("p and a half") << 4096.0 << 8.0 << 1.5;
    }

    void aLockThatAsksForMoreThanAFileMayIsNotWorkedOn()
    {
        QFETCH(double, n);
        QFETCH(double, r);
        QFETCH(double, p);
        SealedByHand sealed = sealedByHand(R"({"devices": []})");
        sealed.kdf[u"n"_s] = n;
        sealed.kdf[u"r"_s] = r;
        sealed.kdf[u"p"_s] = p;
        WatchedWork work;
        const base::Result<Content> read =
            core::exchange::readWith(sealed.text(), SecretBytes::utf8Of(thePassphrase()), work.derive());
        // Refused as a wrong passphrase is (it opens nothing), and before the work was asked
        // for at all: no memory is taken on such a file's word.
        QCOMPARE(saidOf(read), QLatin1StringView(wrongPassphrase));
        QCOMPARE(work.asked(), 0);
    }

    void whatAFileMayAskOfThePassphrasesWorkIsBounded()
    {
        using core::exchange::mayBeAsked;
        using core::scrypt::Cost;
        // As a file is written, as the server's tests write one, and the most there is.
        QVERIFY(mayBeAsked(core::exchange::costAsWritten));
        QCOMPARE(core::exchange::costAsWritten, (Cost{131072, 8, 1}));
        QVERIFY(mayBeAsked(quickCost));
        QVERIFY(mayBeAsked(Cost{1024, 1, 1}));
        QVERIFY(mayBeAsked(Cost{1048576, 2, 1}));
        QVERIFY(mayBeAsked(Cost{131072, 16, 1}));
        QVERIFY(mayBeAsked(Cost{131072, 4, 4}));
        QVERIFY(mayBeAsked(Cost{32768, 1, 4}));
        // Nothing a file may ask for takes more than 257 MiB, or is refused by the work itself.
        for (quint64 n = 1024; n <= 1048576; n *= 2) {
            for (quint32 r = 1; r <= 16; ++r) {
                for (quint32 p = 1; p <= 4; ++p) {
                    const quint64 asked = core::scrypt::bytesAskedFor(Cost{n, r, p});
                    if (mayBeAsked(Cost{n, r, p})) {
                        QVERIFY2(asked > 0 && asked <= (quint64{257} << 20),
                                 qPrintable(u"N %1 r %2 p %3: %4 bytes"_s.arg(n).arg(r).arg(p).arg(asked)));
                    }
                }
            }
        }
    }

    void aLockThatIsNotWrittenAsTheServerWritesOneOpensNothing_data()
    {
        QTest::addColumn<QString>("where"); // "lock", "kdf" or "key"
        QTest::addColumn<QString>("field"); // of that; nothing: all of it
        QTest::addColumn<QJsonValue>("value");
        const QJsonValue gone(QJsonValue::Undefined);
        QTest::newRow("no lock") << u"lock"_s << QString() << gone;
        QTest::newRow("nothing as the lock") << u"lock"_s << QString() << QJsonValue();
        QTest::newRow("a text as the lock") << u"lock"_s << QString() << QJsonValue(u"scrypt"_s);
        QTest::newRow("a list as the lock") << u"lock"_s << QString() << QJsonValue(QJsonArray{1});
        QTest::newRow("an object with nothing in it as the lock")
            << u"lock"_s << QString() << QJsonValue(QJsonObject());
        QTest::newRow("no kdf") << u"kdf"_s << QString() << gone;
        QTest::newRow("a list as the kdf") << u"kdf"_s << QString() << QJsonValue(QJsonArray{4096, 8, 1});
        QTest::newRow("a kdf with nothing in it") << u"kdf"_s << QString() << QJsonValue(QJsonObject());
        QTest::newRow("no name") << u"kdf"_s << u"name"_s << gone;
        QTest::newRow("another name") << u"kdf"_s << u"name"_s << QJsonValue(u"pbkdf2"_s);
        QTest::newRow("the name in capitals") << u"kdf"_s << u"name"_s << QJsonValue(u"SCRYPT"_s);
        QTest::newRow("no N") << u"kdf"_s << u"n"_s << gone;
        // The server makes a number of each of these. Here a number is a number.
        QTest::newRow("N in quotes") << u"kdf"_s << u"n"_s << QJsonValue(u"4096"_s);
        QTest::newRow("r in quotes") << u"kdf"_s << u"r"_s << QJsonValue(u"8"_s);
        QTest::newRow("a yes as p") << u"kdf"_s << u"p"_s << QJsonValue(true);
        QTest::newRow("nothing as r") << u"kdf"_s << u"r"_s << QJsonValue();
        QTest::newRow("a list as N") << u"kdf"_s << u"n"_s << QJsonValue(QJsonArray{4096});
        QTest::newRow("no salt") << u"kdf"_s << u"salt"_s << gone;
        QTest::newRow("a number as the salt") << u"kdf"_s << u"salt"_s << QJsonValue(7);
        QTest::newRow("a salt longer than any of qKVM's")
            << u"kdf"_s << u"salt"_s << QJsonValue(base64Of(QByteArray(core::exchange::longestSalt + 1, 's')));
        QTest::newRow("no key") << u"key"_s << QString() << gone;
        QTest::newRow("a text as the key") << u"key"_s << QString() << QJsonValue(u"AAAA"_s);
        QTest::newRow("a key without its nonce") << u"key"_s << u"nonce"_s << gone;
        QTest::newRow("a key without its data") << u"key"_s << u"data"_s << gone;
        QTest::newRow("a number as the key's data") << u"key"_s << u"data"_s << QJsonValue(7);
        QTest::newRow("a key's nonce of eight bytes") << u"key"_s << u"nonce"_s << QJsonValue(base64Of("12345678"));
        QTest::newRow("a key's nonce of sixteen bytes")
            << u"key"_s << u"nonce"_s << QJsonValue(base64Of("1234567890123456"));
        QTest::newRow("a key's nonce of no bytes") << u"key"_s << u"nonce"_s << QJsonValue(QString());
        QTest::newRow("a key's data shorter than a tag")
            << u"key"_s << u"data"_s << QJsonValue(base64Of("123456789012345"));
    }

    void aLockThatIsNotWrittenAsTheServerWritesOneOpensNothing()
    {
        QFETCH(QString, where);
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        SealedByHand sealed = sealedByHand(R"({"devices": []})");
        QJsonObject file = sealed.head;
        file[u"sealed"_s] = sealed.sealed;
        QJsonObject kdf = sealed.kdf;
        QJsonObject key = sealed.key;
        QJsonValue lock;
        if (where == u"lock"_s) {
            lock = value;
        } else {
            QJsonObject& changed = where == u"kdf"_s ? kdf : key;
            QJsonValue whole = changed;
            if (field.isEmpty()) {
                whole = value;
            } else if (value.isUndefined()) {
                changed.remove(field);
                whole = changed;
            } else {
                changed[field] = value;
                whole = changed;
            }
            QJsonObject made{{u"kdf"_s, kdf}, {u"key"_s, key}};
            if (whole.isUndefined()) {
                made.remove(where);
            } else {
                made[where] = whole;
            }
            lock = made;
        }
        if (!lock.isUndefined()) {
            file[u"lock"_s] = lock;
        }
        WatchedWork work;
        const base::Result<Content> read = core::exchange::readWith(
            QJsonDocument(file).toJson(QJsonDocument::Compact), SecretBytes::utf8Of(thePassphrase()), work.derive());
        QCOMPARE(saidOf(read), QLatin1StringView(wrongPassphrase));
        // And none of them costs the work of a passphrase.
        QCOMPARE(work.asked(), 0);
    }

    void base64IsReadByTheBook_data()
    {
        QTest::addColumn<QString>("how"); // what is done to the text that stands there
        QTest::newRow("a line's end behind it") << u"end"_s;
        QTest::newRow("a line's end in the middle") << u"middle"_s;
        QTest::newRow("a blank in front") << u"blank"_s;
        QTest::newRow("a sign of the other alphabet (for addresses)") << u"dash"_s;
        QTest::newRow("a sign that is in no alphabet") << u"star"_s;
        QTest::newRow("its filling left off") << u"unfilled"_s;
        QTest::newRow("more filling than it needs") << u"overfilled"_s;
        QTest::newRow("a bit set that belongs to no byte") << u"bit"_s;
    }

    void base64IsReadByTheBook()
    {
        QFETCH(QString, how);
        // The server's reader of base64 passes over what is not of the alphabet, and takes each
        // of these but the two with wrong filling. Here each is no base64: in the lock that is a
        // lock that opens nothing, and in the content a file that is damaged.
        const auto changed = [&how](const QString& text) {
            // (Sixteen bytes and twelve are written with filling at the end, or with none; the
            // rows about filling are made on a text that has some.)
            QString other = text;
            if (how == u"end"_s) {
                other += u'\n';
            } else if (how == u"middle"_s) {
                other.insert(other.size() / 2, u'\n');
            } else if (how == u"blank"_s) {
                other.prepend(u' ');
            } else if (how == u"dash"_s) {
                other[1] = u'-';
            } else if (how == u"star"_s) {
                other[1] = u'*';
            } else if (how == u"unfilled"_s) {
                other = base64Of("12345678901234567").chopped(1);
            } else if (how == u"overfilled"_s) {
                other = base64Of("123456789012345678") + u"===="_s;
            } else {
                // Seventeen bytes: the last sign before the filling says two bits that are part
                // of no byte, and are noughts. The sign behind it in the alphabet has one set.
                other = base64Of("12345678901234567");
                other[other.size() - 2] = QChar(other.at(other.size() - 2).unicode() + 1);
            }
            return other;
        };
        for (const QString& field : {u"salt"_s, u"key nonce"_s, u"key data"_s, u"sealed nonce"_s, u"sealed data"_s}) {
            SealedByHand sealed = sealedByHand(R"({"devices": []})");
            if (field == u"salt"_s) {
                sealed.kdf[u"salt"_s] = changed(sealed.kdf[u"salt"_s].toString());
            } else {
                QJsonObject& part = field.startsWith(u"key"_s) ? sealed.key : sealed.sealed;
                const QString name = field.section(u' ', 1);
                part[name] = changed(part[name].toString());
            }
            WatchedWork work;
            const base::Result<Content> read =
                core::exchange::readWith(sealed.text(), SecretBytes::utf8Of(thePassphrase()), work.derive());
            const bool inTheLock = !field.startsWith(u"sealed"_s);
            QVERIFY2(saidOf(read) == QLatin1StringView(inTheLock ? wrongPassphrase : doesNotOpen),
                     qPrintable(field + u": "_s + saidOf(read)));
            // What is wrong with a lock is seen before the passphrase is worked on.
            QCOMPARE(work.asked(), inTheLock ? 0 : 1);
        }
    }

    void aFileKeyOfAnotherLengthIsNotOpened_data()
    {
        QTest::addColumn<int>("bytes");
        QTest::newRow("sixteen bytes, which the server would go on with") << 16;
        QTest::newRow("twenty-four bytes, which the server would go on with") << 24;
        QTest::newRow("five bytes") << 5;
        QTest::newRow("thirty-one bytes") << 31;
        QTest::newRow("thirty-three bytes") << 33;
        QTest::newRow("sixty-four bytes") << 64;
        QTest::newRow("no bytes") << 0;
    }

    void aFileKeyOfAnotherLengthIsNotOpened()
    {
        QFETCH(int, bytes);
        const SealedByHand sealed =
            sealedByHand(R"({"devices": []})", thePassphrase(), quickCost, QByteArray(bytes, 'k'));
        // The passphrase is the right one, and the lock opens: what is in it is no key of a file.
        QCOMPARE(saidOf(core::exchange::read(sealed.text(), thePassphrase())), QLatin1StringView(doesNotOpen));
        // With a wrong passphrase it is a wrong passphrase, as for any file.
        QCOMPARE(saidOf(core::exchange::read(sealed.text(), u"another passphrase"_s)),
                 QLatin1StringView(wrongPassphrase));
    }

    void whatWasChangedAfterItWasSealedDoesNotOpen_data()
    {
        QTest::addColumn<QString>("part"); // "key" or "sealed"
        QTest::addColumn<QString>("field");
        QTest::addColumn<int>("turnedBit"); // counted from the end where it is below nought
        QTest::newRow("the first bit of the content") << u"sealed"_s << u"data"_s << 0;
        QTest::newRow("a bit in the middle of the content") << u"sealed"_s << u"data"_s << 100;
        QTest::newRow("the last bit of the content's tag") << u"sealed"_s << u"data"_s << -1;
        QTest::newRow("the first bit of the content's tag") << u"sealed"_s << u"data"_s << -128;
        QTest::newRow("a bit of the content's nonce") << u"sealed"_s << u"nonce"_s << 3;
        QTest::newRow("the first bit of the key") << u"key"_s << u"data"_s << 0;
        QTest::newRow("the last bit of the key's tag") << u"key"_s << u"data"_s << -1;
        QTest::newRow("a bit of the key's nonce") << u"key"_s << u"nonce"_s << 3;
    }

    void whatWasChangedAfterItWasSealedDoesNotOpen()
    {
        QFETCH(QString, part);
        QFETCH(QString, field);
        QFETCH(int, turnedBit);
        SealedByHand sealed = sealedByHand("{\"devices\": [{\"url\": \"https://192.168.1.20\", \"kind\": \"kvmd\"}]}");
        QCOMPARE(saidOf(core::exchange::read(sealed.text(), thePassphrase())), u"read"_s);
        QJsonObject& changed = part == u"key"_s ? sealed.key : sealed.sealed;
        QByteArray bytes = QByteArray::fromBase64(changed[field].toString().toLatin1());
        const int bit = turnedBit >= 0 ? turnedBit : static_cast<int>(bytes.size()) * 8 + turnedBit;
        bytes[bit / 8] = static_cast<char>(bytes.at(bit / 8) ^ (1 << (bit % 8)));
        changed[field] = base64Of(bytes);
        // The key does not open: a wrong passphrase, as far as anybody can tell. The content does
        // not open with its own key: damaged.
        QCOMPARE(saidOf(core::exchange::read(sealed.text(), thePassphrase())),
                 QLatin1StringView(part == u"key"_s ? wrongPassphrase : doesNotOpen));
    }

    void whatWasSealedWithANonceOfAnotherLengthIsNotRead_data()
    {
        QTest::addColumn<QString>("part"); // "key" or "sealed"
        QTest::addColumn<int>("bytes");
        QTest::newRow("the file's key under a nonce of eight bytes") << u"key"_s << 8;
        QTest::newRow("the file's key under a nonce of sixteen bytes") << u"key"_s << 16;
        QTest::newRow("the content under a nonce of eight bytes") << u"sealed"_s << 8;
        QTest::newRow("the content under a nonce of sixteen bytes") << u"sealed"_s << 16;
    }

    void whatWasSealedWithANonceOfAnotherLengthIsNotRead()
    {
        QFETCH(QString, part);
        QFETCH(int, bytes);
        // Sealed for real with such a nonce, by Windows asked by hand. A QKVM server would read
        // this file: its sealing takes nonces of eight bytes to a hundred and twenty-eight. QKVM
        // writes twelve, and so nothing else is read here: in the lock that is a lock that opens
        // nothing, and in the content a file that is damaged.
        const QByteArray inside("{\"devices\": [{\"url\": \"https://192.168.1.20\", \"kind\": \"kvmd\"}]}");
        SealedByHand sealed = sealedByHand(inside);
        QCOMPARE(saidOf(core::exchange::read(sealed.text(), thePassphrase())), u"read"_s);
        const QByteArray nonce(bytes, 'n');
        const QByteArray data = part == u"key"_s
                                    ? sealedByWindows(sealed.derived, nonce, sealed.fileKey, "kvm-wall data key")
                                    : sealedByWindows(sealed.fileKey, nonce, inside, "export 1");
        QVERIFY2(!data.isEmpty(), "Windows itself does not seal with a nonce of this length");
        const QJsonObject other{{u"nonce"_s, base64Of(nonce)}, {u"data"_s, base64Of(data)}};
        (part == u"key"_s ? sealed.key : sealed.sealed) = other;
        QCOMPARE(saidOf(core::exchange::read(sealed.text(), thePassphrase())),
                 QLatin1StringView(part == u"key"_s ? wrongPassphrase : doesNotOpen));
    }

    void whatIsSealedForSomethingElseDoesNotOpenAsAFile()
    {
        SealedByHand sealed = sealedByHand(R"({"devices": []})");
        // The file's own key, put where the content goes.
        SealedByHand keyAsContent = sealed;
        keyAsContent.sealed = sealed.key;
        QCOMPARE(saidOf(core::exchange::read(keyAsContent.text(), thePassphrase())), QLatin1StringView(doesNotOpen));
        // Content that was sealed for something else: a device's password of a wall, as it were.
        SealedByHand forADevice = sealed;
        forADevice.sealed = SealedByHand::sealedUnder(sealed.fileKey, R"({"devices": []})", "a1b2c3d4");
        QCOMPARE(saidOf(core::exchange::read(forADevice.text(), thePassphrase())), QLatin1StringView(doesNotOpen));
        // A key that was sealed for something else: the content of a file, as it were.
        SealedByHand keyForAFile = sealed;
        keyForAFile.key = SealedByHand::sealedUnder(sealed.derived, sealed.fileKey, "export 1");
        QCOMPARE(saidOf(core::exchange::read(keyForAFile.text(), thePassphrase())), QLatin1StringView(wrongPassphrase));
    }

    void aSealedPartThatIsNotWrittenAsTheServerWritesOneIsDamaged_data()
    {
        QTest::addColumn<QJsonValue>("sealed");
        const SealedByHand made = sealedByHand(R"({"devices": []})");
        const auto with = [&made](const QString& field, const QJsonValue& value) {
            QJsonObject sealed = made.sealed;
            if (value.isUndefined()) {
                sealed.remove(field);
            } else {
                sealed[field] = value;
            }
            return QJsonValue(sealed);
        };
        QTest::newRow("nothing") << QJsonValue();
        QTest::newRow("a text") << QJsonValue(u"AAAA"_s);
        QTest::newRow("a list") << QJsonValue(QJsonArray{u"AAAA"_s, u"AAAA"_s});
        QTest::newRow("an object with nothing in it") << QJsonValue(QJsonObject());
        QTest::newRow("no nonce") << with(u"nonce"_s, QJsonValue(QJsonValue::Undefined));
        QTest::newRow("no data") << with(u"data"_s, QJsonValue(QJsonValue::Undefined));
        QTest::newRow("a number as the nonce") << with(u"nonce"_s, 7);
        QTest::newRow("a list as the data") << with(u"data"_s, QJsonArray{1, 2});
        // The server would go on with a nonce of eight bytes to a hundred and twenty-eight, had
        // the content been sealed with it. QKVM seals with twelve, and so is nothing else read.
        QTest::newRow("a nonce of eight bytes") << with(u"nonce"_s, base64Of("12345678"));
        QTest::newRow("a nonce of eleven bytes") << with(u"nonce"_s, base64Of("12345678901"));
        QTest::newRow("a nonce of thirteen bytes") << with(u"nonce"_s, base64Of("1234567890123"));
        QTest::newRow("a nonce of sixteen bytes") << with(u"nonce"_s, base64Of("1234567890123456"));
        QTest::newRow("no bytes as the nonce") << with(u"nonce"_s, QString());
        QTest::newRow("data shorter than a tag") << with(u"data"_s, base64Of("123456789012345"));
        QTest::newRow("no bytes as the data") << with(u"data"_s, QString());
    }

    void aSealedPartThatIsNotWrittenAsTheServerWritesOneIsDamaged()
    {
        QFETCH(QJsonValue, sealed);
        // The same lock for every row would do; a new one each time is what a test can afford.
        SealedByHand made = sealedByHand(R"({"devices": []})");
        QJsonObject file = made.head;
        file[u"lock"_s] = made.lock();
        file[u"sealed"_s] = sealed;
        QCOMPARE(saidOf(core::exchange::read(QJsonDocument(file).toJson(QJsonDocument::Compact), thePassphrase())),
                 QLatin1StringView(doesNotOpen));
    }

    void whatASealedFileHoldsHasToBeWhatQkvmWrites_data()
    {
        QTest::addColumn<QByteArray>("inside");
        QTest::addColumn<QString>("said");
        const QString opens = QLatin1StringView(doesNotOpen);
        const QString isNotIt = QLatin1StringView(notWhatIsWritten);
        QTest::newRow("nothing") << QByteArray() << opens;
        QTest::newRow("no JSON") << QByteArray("not json at all") << opens;
        QTest::newRow("JSON cut off") << QByteArray(R"({"devices": [)") << opens;
        QTest::newRow("bytes that are no text") << QByteArray("{\"x\": \"\xC3\x28\"}") << opens;
        QTest::newRow("the mark some programs put at a file's head") << QByteArray("\xEF\xBB\xBF{}") << opens;
        QTest::newRow("half a character, spelled out")
            << QByteArray("{\"actions\": [{\"name\": \"type\", \"kind\": \"text\", \"text\": \"x") +
                   QByteArray(1, '\\') + "ud800\"}]}"
            << opens;
        QTest::newRow("nested deeper than anything of qKVM's")
            << QByteArray(1000, '[') + QByteArray(1000, ']') << opens;
        QTest::newRow("a list") << QByteArray("[1, 2]") << isNotIt;
        QTest::newRow("a number") << QByteArray("7") << isNotIt;
        QTest::newRow("a text") << QByteArray(R"("qkvm-export")") << isNotIt;
        QTest::newRow("nothing, spelled out") << QByteArray("null") << isNotIt;
    }

    void whatASealedFileHoldsHasToBeWhatQkvmWrites()
    {
        QFETCH(QByteArray, inside);
        QFETCH(QString, said);
        QCOMPARE(saidOf(core::exchange::read(sealedByHand(inside).text(), thePassphrase())), said);
    }

    void whatASealedFileSaysOfItselfInsideIsNotAskedAgain()
    {
        // The server reads what is inside without asking again what it calls itself: an object
        // is enough. So it is here, to the letter.
        const base::Result<Content> read = core::exchange::read(
            sealedByHand(R"({"format": "x", "version": 99, "sealed": 1, "wall": {"interval": 2}})").text(),
            thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        QVERIFY(read.value().sealed);
        QCOMPARE(wallIn(read.value()).pictureEverySeconds, std::optional<double>(2.0));
        // And nothing in it at all is a file with no part.
        const base::Result<Content> none = core::exchange::read(sealedByHand("{}").text(), thePassphrase());
        QCOMPARE(saidOf(none), u"read"_s);
        QVERIFY(!none.value().devices && !none.value().actions && !none.value().alerts && !none.value().wall);
    }

    void aFileLargerThanAnyOfQkvmsIsRefused()
    {
        using core::exchange::largestPlain;
        using core::exchange::largestSealed;
        QCOMPARE(largestPlain, qsizetype{4000000});
        QCOMPARE(largestSealed, qsizetype{5337429});
        // A plain file of exactly as many signs as one may have, and of one more.
        const QByteArray plain = plainFile({{u"devices"_s, QJsonArray{deviceWith({})}}});
        QCOMPARE(saidOf(readPlain(plain + QByteArray(largestPlain - plain.size(), ' '))), u"read"_s);
        QCOMPARE(saidOf(readPlain(plain + QByteArray(largestPlain - plain.size() + 1, ' '))),
                 QLatin1StringView(tooLarge));
        // Counted in signs, and not in bytes: with signs of two bytes and of four in it, such a
        // file has many more bytes than that, and is read.
        const QString twoBytes(100000, QChar(0xfc));
        const QByteArray wide =
            plainFile({{u"devices"_s, QJsonArray{deviceWith({})}}, {u"x"_s, twoBytes + wideSigns(100000)}});
        const qsizetype signs = wide.size() - 100000 - 300000;
        QCOMPARE(core::json::signsIn(wide), std::optional<qsizetype>(signs));
        QVERIFY((wide + QByteArray(largestPlain - signs, ' ')).size() == largestPlain + 400000);
        QCOMPARE(saidOf(readPlain(wide + QByteArray(largestPlain - signs, ' '))), u"read"_s);
        QCOMPARE(saidOf(readPlain(wide + QByteArray(largestPlain - signs + 1, ' '))), QLatin1StringView(tooLarge));
        // Whatever it is: too long is said before anything of it is read.
        QCOMPARE(saidOf(readPlain(QByteArray(largestSealed + 1, ' '))), QLatin1StringView(tooLarge));
        QCOMPARE(saidOf(readPlain(QByteArray(largestSealed * 5, 'x'))), QLatin1StringView(tooLarge));
        QCOMPARE(saidOf(core::exchange::isSealed(QByteArray(largestSealed + 1, ' '))), QLatin1StringView(tooLarge));
        // What is no file at all, and no longer than a sealed one may be, is no file.
        QCOMPARE(saidOf(readPlain(QByteArray(largestSealed, ' '))), QLatin1StringView(notAFile));

        // A sealed file may be longer than a plain one, by what sealing adds: as long as
        // largestSealed. One sign more is too long, before its passphrase is worked on.
        const SealedByHand sealed = sealedByHand(R"({"devices": []})");
        const QByteArray text = sealed.text();
        WatchedWork work;
        const auto readSealed = [&work](const QByteArray& file) {
            return core::exchange::readWith(file, SecretBytes::utf8Of(thePassphrase()), work.derive());
        };
        QCOMPARE(saidOf(readSealed(text + QByteArray(largestSealed - text.size(), ' '))), u"read"_s);
        QCOMPARE(work.asked(), 1);
        QCOMPARE(saidOf(readSealed(text + QByteArray(largestSealed - text.size() + 1, ' '))),
                 QLatin1StringView(tooLarge));
        QCOMPARE(work.asked(), 1);
        // That is no way to hand over a plain file of that length.
        QCOMPARE(saidOf(readPlain(plain + QByteArray(largestSealed - plain.size(), ' '))), QLatin1StringView(tooLarge));
    }

    void whatASealedFileHoldsIsNoLargerThanAPlainFile()
    {
        using core::exchange::largestPlain;
        // Inside a sealed file that is itself no longer than one may be: exactly as much as a
        // plain file may hold, and one sign more.
        const QByteArray inside("{\"devices\": [{\"url\": \"https://192.168.1.20\", \"kind\": \"kvmd\"}]}");
        const QByteArray fits = sealedByHand(inside + QByteArray(largestPlain - inside.size(), ' ')).text();
        const QByteArray tooMuch = sealedByHand(inside + QByteArray(largestPlain - inside.size() + 1, ' ')).text();
        QVERIFY(tooMuch.size() <= core::exchange::largestSealed);
        const base::Result<Content> read = core::exchange::read(fits, thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        QCOMPARE(devicesIn(read.value()).size(), qsizetype{1});
        QCOMPARE(saidOf(core::exchange::read(tooMuch, thePassphrase())), QLatin1StringView(tooLarge));
    }

    // --- the entries: devices -----------------------------------------------------------------------

    void aDeviceIsReadWithWhatAnEntryMayLeaveOut()
    {
        const Content content = devicesOf({deviceWith({})});
        QCOMPARE(content.leftOut, QStringList());
        QVERIFY(content.devices.has_value());
        QCOMPARE(devicesIn(content).size(), qsizetype{1});
        Device expected;
        expected.name = u"192.168.1.20"_s; // no name given: its address, as when it is added by hand
        expected.address = u"https://192.168.1.20"_s;
        expected.user = u"admin"_s;
        expected.kind = u"kvmd"_s;
        expected.tls = u"pinned"_s;
        QCOMPARE(devicesIn(content).value(0), expected);
        QVERIFY(!content.sealed && !content.hasSecrets());
        QCOMPARE(content.made, 0.0);
        QCOMPARE(content.program, QString());
        // A file made by hand may hold passwords in plain text, and that is seen.
        const Content byHand = devicesOf({deviceWith({{u"password"_s, u"typed by hand"_s}})});
        QCOMPARE(devicesIn(byHand).value(0).password, u"typed by hand"_s);
        QVERIFY(byHand.hasSecrets() && !byHand.sealed);
    }

    void whatAnEntrySaysBeyondWhatADeviceIsMadeOfIsNotLookedAt()
    {
        const Device device = deviceOf({{u"id"_s, u"action:33334444"_s},
                                        {u"secret"_s, QJsonObject{{u"nonce"_s, u"x"_s}, {u"data"_s, u"y"_s}}},
                                        {u"changed"_s, 5},
                                        {u"users"_s, QJsonArray{u"x"_s}},
                                        {u"name"_s, u"  Spaced  "_s},
                                        {u"ident"_s, u"own:a1b2c3d4"_s},
                                        {u"alerts"_s, u"yes"_s}});
        QCOMPARE(device.name, u"Spaced"_s);
        QCOMPARE(device.ident, QString());
        QCOMPARE(device.alerts, true);
        QCOMPARE(device.password, QString());
        QCOMPARE(device.user, u"admin"_s);
    }

    void aDevicesNameIsAShortLineOfText_data()
    {
        QTest::addColumn<QJsonValue>("name");
        QTest::addColumn<QString>("taken"); // nothing: the device is left out
        const QString zeroWidth(QChar(0x200b));
        QTest::newRow("eighty signs") << QJsonValue(QString(80, u'x')) << QString(80, u'x');
        QTest::newRow("eighty-one signs") << QJsonValue(QString(81, u'x')) << QString();
        // Counted in signs: eighty that take two places each are eighty.
        QTest::newRow("eighty signs of two places") << QJsonValue(wideSigns(80)) << wideSigns(80);
        QTest::newRow("eighty-one signs of two places") << QJsonValue(wideSigns(81)) << QString();
        QTest::newRow("eighty signs and the blanks around them")
            << QJsonValue(u"  \t"_s + QString(80, u'x') + u" \n"_s) << QString(80, u'x');
        QTest::newRow("a tab inside is a blank") << QJsonValue(u"Rack\tKVM"_s) << u"Rack KVM"_s;
        QTest::newRow("a blank of no width is nothing") << QJsonValue(u"Rack"_s + zeroWidth + u"KVM"_s) << u"RackKVM"_s;
        QTest::newRow("eighty signs and blanks of no width")
            << QJsonValue(zeroWidth + QString(40, u'x') + QChar(0x2060) + QString(40, u'y') + QChar(0xfeff))
            << QString(40, u'x') + QString(40, u'y');
        QTest::newRow("a blank that a blank of no width hid")
            << QJsonValue(zeroWidth + u" Rack KVM "_s + zeroWidth) << u"Rack KVM"_s;
        QTest::newRow("a no-break space inside") << QJsonValue(u"Serveur n"_s + QChar(0xb0) + QChar(0xa0) + u'3')
                                                 << u"Serveur n"_s + QChar(0xb0) + QChar(0xa0) + u'3';
        QTest::newRow("the wide blank of Japanese text")
            << QJsonValue(QString(QChar(0x6771)) + QChar(0x4eac) + QChar(0x3000) + u"KVM"_s)
            << QString(QChar(0x6771)) + QChar(0x4eac) + QChar(0x3000) + u"KVM"_s;
        QTest::newRow("a joiner inside, as an emoji that is two has")
            << QJsonValue(u"Build box "_s + QChar(0x200d) + u'x') << u"Build box "_s + QChar(0x200d) + u'x';
        QTest::newRow("a mark that says which way text runs")
            << QJsonValue(u"KVM "_s + QChar(0x200f) + u'x') << u"KVM "_s + QChar(0x200f) + u'x';
        QTest::newRow("a line's end behind it, of a kind Qt does not call a blank")
            << QJsonValue(u"Rack KVM"_s + QChar(0x1c)) << u"Rack KVM"_s;
        QTest::newRow("no text in it") << QJsonValue(QString()) << u"192.168.1.20"_s;
        QTest::newRow("blanks only") << QJsonValue(u"   "_s) << u"192.168.1.20"_s;
        // What is no line.
        QTest::newRow("two lines") << QJsonValue(u"two\nlines"_s) << QString();
        QTest::newRow("a sign that shows nothing and ends nothing")
            << QJsonValue(u"Rack"_s + QChar(0) + u"KVM"_s) << QString();
        QTest::newRow("an escape sign") << QJsonValue(u"Rack"_s + QChar(0x1b) + u"KVM"_s) << QString();
        QTest::newRow("a control sign of the second set")
            << QJsonValue(u"Rack"_s + QChar(0x85) + u"KVM"_s) << QString();
        QTest::newRow("a delete sign") << QJsonValue(u"Rack"_s + QChar(0x7f) + u"KVM"_s) << QString();
        QTest::newRow("a line separator") << QJsonValue(u"Rack"_s + QChar(0x2028) + u"KVM"_s) << QString();
        QTest::newRow("a paragraph separator") << QJsonValue(u"Rack"_s + QChar(0x2029) + u"KVM"_s) << QString();
        QTest::newRow("a turn of direction") << QJsonValue(u"Rack "_s + QChar(0x202e) + u"MVK"_s) << QString();
        QTest::newRow("the first of the marks that turn")
            << QJsonValue(u"Rack "_s + QChar(0x202a) + u"KVM"_s) << QString();
        QTest::newRow("an isolate") << QJsonValue(u"Rack "_s + QChar(0x2066) + u"KVM"_s + QChar(0x2069)) << QString();
        QTest::newRow("the last of the isolates") << QJsonValue(u"Rack"_s + QChar(0x2069)) << QString();
        // What is no text.
        QTest::newRow("a number") << QJsonValue(7) << QString();
        QTest::newRow("a list") << QJsonValue(QJsonArray{u"a"_s}) << QString();
        QTest::newRow("nothing, spelled out") << QJsonValue() << QString();
        QTest::newRow("a yes") << QJsonValue(true) << QString();
    }

    void aDevicesNameIsAShortLineOfText()
    {
        QFETCH(QJsonValue, name);
        QFETCH(QString, taken);
        const QJsonObject entry = deviceWith({{u"name"_s, name}});
        if (taken.isEmpty()) {
            QCOMPARE(leftOutOf(entry), u"Device 1: its name, user name or group is not a short line of text"_s);
            QVERIFY(devicesIn(devicesOf({entry})).isEmpty());
        } else {
            QCOMPARE(leftOutOf(entry), QString());
            QCOMPARE(deviceOf({{u"name"_s, name}}).name, taken);
        }
    }

    void aDevicesUserAndGroupAreShortLinesToo()
    {
        const QString notALine = u"Device 1: its name, user name or group is not a short line of text"_s;
        // The user: sixty-four signs, and "admin" where there is none.
        QCOMPARE(deviceOf({{u"username"_s, wideSigns(64)}}).user, wideSigns(64));
        QCOMPARE(leftOutOf(deviceWith({{u"username"_s, wideSigns(65)}})), notALine);
        QCOMPARE(deviceOf({{u"username"_s, u"  root "_s}}).user, u"root"_s);
        QCOMPARE(deviceOf({{u"username"_s, QString()}}).user, u"admin"_s);
        QCOMPARE(deviceOf({{u"username"_s, u"   "_s}}).user, u"admin"_s);
        QCOMPARE(leftOutOf(deviceWith({{u"username"_s, u"ro\not"_s}})), notALine);
        QCOMPARE(leftOutOf(deviceWith({{u"username"_s, 7}})), notALine);
        QCOMPARE(leftOutOf(deviceWith({{u"username"_s, QJsonValue()}})), notALine);
        // The group: forty signs once its blanks, of whatever kind, are one blank each; and no
        // more than two hundred before that.
        QCOMPARE(deviceOf({{u"group"_s, wideSigns(40)}}).group, wideSigns(40));
        QCOMPARE(leftOutOf(deviceWith({{u"group"_s, wideSigns(41)}})), u"Device 1: its group's name is too long"_s);
        QCOMPARE(deviceOf({{u"group"_s, u"  rack   1 \t of"_s + QChar(0xa0) + QChar(0x3000) + u"two "_s}}).group,
                 u"rack 1 of two"_s);
        QCOMPARE(deviceOf({{u"group"_s, QString(20, u'x') + QString(161, u' ') + QString(19, u'y')}}).group,
                 QString(20, u'x') + u' ' + QString(19, u'y'));
        QCOMPARE(leftOutOf(deviceWith({{u"group"_s, QString(20, u'x') + QString(162, u' ') + QString(19, u'y')}})),
                 notALine);
        QCOMPARE(leftOutOf(deviceWith({{u"group"_s, QString(21, u'x') + u' ' + QString(20, u'y')}})),
                 u"Device 1: its group's name is too long"_s);
        QCOMPARE(deviceOf({{u"group"_s, u"rack"_s + QChar(0x200b) + u'1'}}).group, u"rack1"_s);
        QCOMPARE(leftOutOf(deviceWith({{u"group"_s, u"rack"_s + QChar(0x202e) + u'1'}})), notALine);
        QCOMPARE(leftOutOf(deviceWith({{u"group"_s, QJsonArray{u"rack 1"_s}}})), notALine);
    }

    void aDevicesPasswordIsTakenAsItStands()
    {
        // Whatever is in it, blanks and ends of lines too, and up to 256 signs of it.
        const QString odd = u"  two\nlines\tand a "_s + QChar(0x202e) + QChar(0) + u" "_s;
        QCOMPARE(deviceOf({{u"password"_s, odd}}).password, odd);
        QCOMPARE(deviceOf({{u"password"_s, wideSigns(256)}}).password, wideSigns(256));
        QCOMPARE(leftOutOf(deviceWith({{u"password"_s, wideSigns(257)}})), u"Device 1: its password is not one"_s);
        QCOMPARE(leftOutOf(deviceWith({{u"password"_s, QString(257, u'p')}})), u"Device 1: its password is not one"_s);
        QCOMPARE(leftOutOf(deviceWith({{u"password"_s, 12345}})), u"Device 1: its password is not one"_s);
        QCOMPARE(leftOutOf(deviceWith({{u"password"_s, QJsonValue()}})), u"Device 1: its password is not one"_s);
        QCOMPARE(leftOutOf(deviceWith({{u"password"_s, QJsonObject{{u"nonce"_s, u"AAAA"_s}}}})),
                 u"Device 1: its password is not one"_s);
    }

    void aDeviceIsOfAKindThisVersionKnowsAndSaysHowItsCertificateIsChecked()
    {
        const QString notAKind = u"Device 1: it is not a kind of device this version knows"_s;
        const QString notAWay = u"Device 1: how its certificate is checked is not said in a way this version knows"_s;
        // The three families, of which this program speaks to one: all three are read, and
        // handed back as what they are.
        for (const QString& kind : {u"kvmd"_s, u"jetkvm"_s, u"redfish"_s}) {
            QCOMPARE(deviceOf({{u"kind"_s, kind}}).kind, kind);
        }
        QJsonObject noKind = deviceWith({});
        noKind.remove(u"kind"_s);
        QCOMPARE(leftOutOf(noKind), notAKind);
        QCOMPARE(leftOutOf(deviceWith({{u"kind"_s, u"ipmi"_s}})), notAKind);
        QCOMPARE(leftOutOf(deviceWith({{u"kind"_s, u"KVMD"_s}})), notAKind);
        QCOMPARE(leftOutOf(deviceWith({{u"kind"_s, u" kvmd"_s}})), notAKind);
        QCOMPARE(leftOutOf(deviceWith({{u"kind"_s, QJsonArray{u"kvmd"_s}}})), notAKind);
        QCOMPARE(leftOutOf(deviceWith({{u"kind"_s, QJsonValue()}})), notAKind);
        for (const QString& tls : {u"pinned"_s, u"system"_s, u"insecure"_s}) {
            QCOMPARE(deviceOf({{u"tls"_s, tls}}).tls, tls);
        }
        QCOMPARE(deviceOf({}).tls, u"pinned"_s);
        QCOMPARE(leftOutOf(deviceWith({{u"tls"_s, u"trust-everything"_s}})), notAWay);
        QCOMPARE(leftOutOf(deviceWith({{u"tls"_s, u"Pinned"_s}})), notAWay);
        QCOMPARE(leftOutOf(deviceWith({{u"tls"_s, QJsonValue()}})), notAWay);
        QCOMPARE(leftOutOf(deviceWith({{u"tls"_s, true}})), notAWay);
        // Over plain HTTP there is no certificate to check, whatever the file says about one;
        // but what it says has to be one of the three all the same.
        const Device plain =
            deviceOf({{u"url"_s, u"http://192.168.1.40"_s}, {u"tls"_s, u"insecure"_s}, {u"cert_pem"_s, u"x"_s}});
        QCOMPARE(plain.tls, u"pinned"_s);
        QCOMPARE(plain.certificatePem, QString());
        QCOMPARE(plain.certificate, QByteArray());
        QCOMPARE(leftOutOf(deviceWith({{u"url"_s, u"http://192.168.1.40"_s}, {u"tls"_s, u"trust-everything"_s}})),
                 notAWay);
    }

    void aDevicesAddressIsWrittenOneWay_data()
    {
        QTest::addColumn<QString>("written");
        QTest::addColumn<QString>("taken");
        QTest::newRow("as it is kept") << u"https://192.168.1.20"_s << u"https://192.168.1.20"_s;
        QTest::newRow("in capitals") << u"HTTPS://KVM.Example"_s << u"https://kvm.example"_s;
        QTest::newRow("the port it has anyway") << u"https://kvm.example:443"_s << u"https://kvm.example"_s;
        QTest::newRow("a colon and no port") << u"https://kvm.example:"_s << u"https://kvm.example"_s;
        QTest::newRow("a dot behind its name, noughts before its port, and a path")
            << u"https://kvm.example.:0443/"_s << u"https://kvm.example"_s;
        QTest::newRow("plain, the port it has anyway") << u"http://kvm.example:80"_s << u"http://kvm.example"_s;
        QTest::newRow("a port of its own") << u"https://kvm.example:8443"_s << u"https://kvm.example:8443"_s;
        QTest::newRow("plain, at the other's port") << u"http://kvm.example:443"_s << u"http://kvm.example:443"_s;
        QTest::newRow("numbers of the longer kind") << u"http://[2001:DB8:0:0::7]"_s << u"http://[2001:db8::7]"_s;
        QTest::newRow("blanks around it") << u"  https://192.168.1.20 \n"_s << u"https://192.168.1.20"_s;
        QTest::newRow("a pasted page") << u"https://192.168.1.20/#/kvm?x=1"_s << u"https://192.168.1.20"_s;
        QTest::newRow("an underscore, as names at home have") << u"https://my_kvm.lan"_s << u"https://my_kvm.lan"_s;
        // A name in its "xn--" form (punycode) is made of letters, digits and hyphens, and is one
        // this program speaks to and the server takes (aNameInItsXnFormIsTakenWhereItIsOne).
        QTest::newRow("a name in its xn-- form") << u"https://xn--mnchen-3ya.de"_s << u"https://xn--mnchen-3ya.de"_s;
        QTest::newRow("a name in its xn-- form, in capitals, with a port and a path")
            << u"https://XN--MNCHEN-3ya.de:8443/kvm?x=1"_s << u"https://xn--mnchen-3ya.de:8443"_s;
    }

    void aDevicesAddressIsWrittenOneWay()
    {
        QFETCH(QString, written);
        QFETCH(QString, taken);
        const Device device = deviceOf({{u"url"_s, written}});
        QCOMPARE(device.address, taken);
        // Named by its address where it has no name: the host, and the port it has of its own.
        QCOMPARE(device.name, taken.mid(taken.indexOf(u"://"_s) + 3));
    }

    void whatIsNoAddressIsNoDevice_data()
    {
        QTest::addColumn<QJsonValue>("url");
        QTest::addColumn<QString>("why");
        // What is said of an address that is none is said in words, and the address is in none
        // of them (anAddressThatWasRefusedIsNeverShown). The first two sentences are the
        // server's own, which show nothing.
        const QString none = u"Device 1: it has no address"_s;
        const QString otherWay = u"Device 1: its address does not begin with http:// or https://"_s;
        const QString somebody = u"Device 1: its address has a user name or a password in it"_s;
        const QString noPort = u"Device 1: what stands behind the colon in its address is no port"_s;
        const QString noHost = u"Device 1: its address names no host"_s;
        const QString noName = u"Device 1: its address has a sign in it that no name of a host has"_s;
        const QString noNumbers = u"Device 1: the numbers in its address are not an address"_s;
        const QString inBrackets = u"Device 1: what stands in brackets in its address is no address of numbers"_s;
        const QString behindBrackets = u"Device 1: what stands behind the brackets in its address is no port"_s;
        const QString notToQt = u"Device 1: its address is not one this program can connect to"_s;
        QTest::newRow("none at all") << QJsonValue(QJsonValue::Undefined) << none;
        QTest::newRow("a number") << QJsonValue(7) << none;
        QTest::newRow("nothing, spelled out") << QJsonValue() << none;
        QTest::newRow("a list") << QJsonValue(QJsonArray{u"https://192.168.1.20"_s}) << none;
        // Written by QKVM, an address says how it is spoken to.
        QTest::newRow("no way of speaking") << QJsonValue(u"192.168.1.21"_s) << none;
        QTest::newRow("more than 255 signs") << QJsonValue(u"https://192.168.1.20/"_s + QString(236, u'x')) << none;
        QTest::newRow("another way of speaking") << QJsonValue(u"ftp://192.168.1.23"_s) << otherWay;
        QTest::newRow("a file") << QJsonValue(u"file:///etc/passwd"_s) << otherWay;
        QTest::newRow("blanks around what is none") << QJsonValue(u"  ftp://192.168.1.23 "_s) << otherWay;
        // The long s counts as an "s" to Qt, and to the server "http", a long s and "://" is no
        // way of speaking: nor is it one here. What this row holds is the SENTENCE: that the
        // device is left out is the program's own rule for an address, which never compared by
        // Qt's folding; with the way of speaking compared by that folding where the sentence
        // is made, the device is left out all the same and the sentence is "its address is not
        // one this program can connect to" (tried: build/mine/x24_folding_test_core_exchange.log).
        QTest::newRow("a long s where the s of https stands")
            << QJsonValue(u"http"_s + QChar(0x17f) + u"://192.168.1.23"_s) << otherWay;
        QTest::newRow("a name and a password in it") << QJsonValue(u"https://user:pw@192.168.1.22"_s) << somebody;
        QTest::newRow("an at sign and nothing before it") << QJsonValue(u"https://@kvm.example"_s) << somebody;
        QTest::newRow("port nought") << QJsonValue(u"https://kvm.example:0"_s) << noPort;
        QTest::newRow("a port above the last") << QJsonValue(u"https://kvm.example:99999"_s) << noPort;
        QTest::newRow("a port that is words") << QJsonValue(u"https://kvm.example:https"_s) << noPort;
        QTest::newRow("a dot and nothing else") << QJsonValue(u"https://."_s) << noHost;
        QTest::newRow("numbers the system reads otherwise") << QJsonValue(u"http://10.01.000.001"_s) << noNumbers;
        QTest::newRow("five numbers") << QJsonValue(u"https://1.2.3.4.5"_s) << noNumbers;
        QTest::newRow("a number above 255") << QJsonValue(u"https://999.1.1.1"_s) << noNumbers;
        QTest::newRow("a blank inside") << QJsonValue(u"https://kvm example"_s) << noName;
        QTest::newRow("a name in letters beyond ASCII")
            << QJsonValue(u"https://m"_s + QChar(0xfc) + u"nchen.example"_s) << noName;
        // The sign for Kelvin counts as a "k" to Qt, and to the server too, which makes small
        // letters of a host by its language's rule and takes this one as "kvm.example". Here no
        // name of a host has it (kvm::addressFrom): less is taken. What this row holds is that
        // difference, and that this reader puts no folding of its own in front of the
        // program's rule. It shows nothing of how a way of speaking is compared ("http" and
        // "https" have no k), and passes with that compared either way.
        QTest::newRow("the sign for Kelvin where a k stands")
            << QJsonValue(u"https://"_s + QChar(0x212a) + u"vm.example"_s) << noName;
        QTest::newRow("brackets around what is no address") << QJsonValue(u"https://[kvm]"_s) << inBrackets;
        QTest::newRow("numbers of the longer kind as Qt alone reads them")
            << QJsonValue(u"https://[0x1::1]"_s) << inBrackets;
        QTest::newRow("something behind the brackets") << QJsonValue(u"https://[::1]x"_s) << behindBrackets;
        // What Qt, which would have to connect to it, will not have as a name.
        QTest::newRow("a name in its xn-- form that is none") << QJsonValue(u"https://xn--"_s) << notToQt;
        QTest::newRow("a part of a name that begins with a hyphen") << QJsonValue(u"https://-kvm.example"_s) << notToQt;
    }

    void whatIsNoAddressIsNoDevice()
    {
        QFETCH(QJsonValue, url);
        QFETCH(QString, why);
        QJsonObject entry = deviceWith({});
        entry.remove(u"url"_s);
        if (!url.isUndefined()) {
            entry[u"url"_s] = url;
        }
        QCOMPARE(leftOutOf(entry), why);
        QCOMPARE(devicesIn(devicesOf({entry})).size(), qsizetype{0});
    }

    void aNameInItsXnFormIsTakenWhereItIsOne_data()
    {
        // A device at a name in letters beyond ASCII, written in its short form ("xn--",
        // punycode). Three rules meet there: the program's own for an address
        // (kvm::addressFrom, which is what is read here), Qt's for names, which has the last
        // word in it, and the server's (device_url), in which the library its lines are made
        // with has the last word, asked only where the whole host begins with "xn--". Beside
        // each kind of row: what the server's own reader made of the same addresses (asked of
        // the server's code as it ran on Python 3.14.4: build/mine/x21_odd.py and x21_odd.log).
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("taken");
        // Taken here, and by the server.
        QTest::newRow("a name of a town") << u"https://xn--mnchen-3ya.example"_s << true;
        QTest::newRow("the same in capitals") << u"https://XN--MNCHEN-3YA.EXAMPLE"_s << true;
        QTest::newRow("a capital in what is spelled out") << u"https://xn--Mnchen-3ya.example"_s << true;
        QTest::newRow("another, with a port") << u"https://xn--bcher-kva.example:8443"_s << true;
        QTest::newRow("a sharp s, which older rules made two letters of") << u"https://xn--fa-hia.example"_s << true;
        QTest::newRow("a part of a name that is not its first") << u"https://kvm.xn--mnchen-3ya.example"_s << true;
        QTest::newRow("an underscore before a part in that form")
            << u"https://my_host.xn--mnchen-3ya.example"_s << true;
        // None here, and none to the server.
        QTest::newRow("nothing behind the mark") << u"https://xn--"_s << false;
        QTest::newRow("what spells out no name") << u"https://xn--zzzzzz.de"_s << false;
        // None here, and taken by the server, which asks nobody about a part that is not the
        // first: here less is taken.
        QTest::newRow("what spells out no name, as a part that is not the first")
            << u"https://kvm.xn--zzzzzz.de"_s << false;
        // Taken here, and left out by the server ("not a usable device address"): the one place
        // where this reader takes more. It is a KVM this program speaks to; and whoever writes
        // a file with it is told that a server may leave it out (test_exchange_written.cpp).
        QTest::newRow("a heart in the name") << u"https://xn--i-7iq.example"_s << true;
        QTest::newRow("a picture for a name") << u"https://xn--ls8h.example"_s << true;
        QTest::newRow("an underscore in another part of a name that begins in that form")
            << u"https://xn--mnchen-3ya.my_host"_s << true;
    }

    void aNameInItsXnFormIsTakenWhereItIsOne()
    {
        QFETCH(QString, address);
        QFETCH(bool, taken);
        const QJsonObject entry =
            deviceWith({{u"url"_s, address}, {u"name"_s, u"Rack 3"_s}, {u"password"_s, u"made-up-password"_s}});
        const Content content = devicesOf({entry});
        // What is read here is what the program itself takes for a KVM's address, to the sign.
        const base::Result<QUrl> typed = core::kvm::addressFrom(address);
        QCOMPARE(typed.ok(), taken);
        if (!taken) {
            QCOMPARE(content.leftOut,
                     QStringList{u"Device 1 (\"Rack 3\"): its address is not one this program can connect "
                                 "to"_s});
            QCOMPARE(devicesIn(content).size(), qsizetype{0});
            return;
        }
        QCOMPARE(content.leftOut, QStringList());
        QCOMPARE(devicesIn(content).size(), qsizetype{1});
        QCOMPARE(devicesIn(content).value(0).address, typed.value().toString(QUrl::FullyEncoded));
        QCOMPARE(devicesIn(content).value(0).address, address.toLower());
        QCOMPARE(devicesIn(content).value(0).password, u"made-up-password"_s);
    }

    void aDeviceAtANameInItsXnFormIsLeftOutOnlyForWhatAnyDeviceIs()
    {
        const QString atSuchAName = u"https://xn--mnchen-3ya.de"_s;
        QCOMPARE(leftOutOf(deviceWith({{u"url"_s, atSuchAName}, {u"kind"_s, u"ipmi"_s}})),
                 u"Device 1: it is not a kind of device this version knows"_s);
        QCOMPARE(leftOutOf(deviceWith({{u"url"_s, atSuchAName}, {u"open_url"_s, u"https://u:p@console.example/"_s}})),
                 u"Device 1: the console link must not have a user name or password in it"_s);
        // Named by its address where it has no name, as any device is.
        QCOMPARE(deviceOf({{u"url"_s, atSuchAName + u":8443/kvm"_s}}).name, u"xn--mnchen-3ya.de:8443"_s);
    }

    void whatIsTakenOfANameWithAPartInItsXnFormIsShownNameByNameAndEndingByEnding_data()
    {
        // Whether Qt, which has the last word in the program's rule for an address, reads a part
        // in its "xn--" form at all under an ending that is not on its own list of endings
        // (.lan, .example, no ending) was shown by no test. As far as this shows, it does not:
        // a part that Qt refuses under the endings on its list (.de, .com) is taken under the
        // others. This is the table, name by name: that part ("xn--zzzzzz", which the server's
        // own reader did not take either when it was asked under ".de"), a town, and a picture
        // (which that reader did not take under ".example"), each with no ending, under two
        // endings that are not on Qt's list and under two that are. It shows what IS, and no
        // mending:
        // the mending is that a KVM at ANY such name has its line beside a file that is written
        // (test_exchange_written.cpp), since what a server takes of them cannot be told here.
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("taken");
        const QList<QByteArray> endings{QByteArray(), ".lan", ".example", ".de", ".com"};
        for (const QByteArray& ending : endings) {
            const QByteArray noneWhereQtReadsIt = "xn--zzzzzz" + ending;
            const bool onQtsList = ending == ".de" || ending == ".com";
            QTest::newRow(noneWhereQtReadsIt.constData())
                << u"https://"_s + QString::fromLatin1(noneWhereQtReadsIt) << !onQtsList;
        }
        for (const QByteArray& name : {QByteArray("xn--mnchen-3ya"), QByteArray("xn--ls8h")}) {
            for (const QByteArray& ending : endings) {
                const QByteArray host = name + ending;
                QTest::newRow(host.constData()) << u"https://"_s + QString::fromLatin1(host) << true;
            }
        }
        // And names of digits and hyphens, which the library some servers connect with may read
        // as numbers that are no address: names here, since their last part is not digits alone.
        QTest::newRow("192-168-1-20") << u"https://192-168-1-20"_s << true;
        QTest::newRow("1.2.3-4") << u"https://1.2.3-4"_s << true;
    }

    void whatIsTakenOfANameWithAPartInItsXnFormIsShownNameByNameAndEndingByEnding()
    {
        QFETCH(QString, address);
        QFETCH(bool, taken);
        const base::Result<QUrl> typed = core::kvm::addressFrom(address);
        QCOMPARE(typed.ok(), taken);
        const Content content = devicesOf({deviceWith({{u"url"_s, address}, {u"name"_s, u"Rack 3"_s}})});
        QCOMPARE(devicesIn(content).size(), qsizetype{taken ? 1 : 0});
        if (taken) {
            QCOMPARE(content.leftOut, QStringList());
            QCOMPARE(devicesIn(content).value(0).address, address);
        } else {
            QCOMPARE(content.leftOut, QStringList{u"Device 1 (\"Rack 3\"): its address is not one this program can "
                                                  "connect to"_s});
        }
    }

    void aNameFromAFileCannotEndItsOwnSettingOffInASentence_data()
    {
        // A name is the file's free text, and it stands in a sentence about its device. Set off
        // in brackets alone, it could close them and go on as the sentence: "Device 1 (x): taken.
        // Device 2 (Rack 1): its address has a user name or a password in it" was said of ONE
        // device, whose name was all of "x): taken. Device 2 (Rack 1". A name stands in double
        // quotes, and nothing that is shown of it is a double quote.
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("shown"); // nothing: the device is named by its number alone
        const QString turnsLeft(QChar(0x200e));
        const QString turnsRight(QChar(0x200f));
        const QString turnsAsArabic(QChar(0x061c));
        QTest::newRow("a name") << u"Rack 9"_s << u"\"Rack 9\""_s;
        QTest::newRow("a name that reads like the end of its sentence and the beginning of the next")
            << u"x): taken. Device 2 (Rack 1"_s << u"\"x): taken. Device 2 (Rack 1\""_s;
        QTest::newRow("the same with the quotes it would be set off in")
            << u"x\"): taken. Device 2 (\"Rack 1"_s << u"\"x'): taken. Device 2 ('Rack 1\""_s;
        QTest::newRow("the same with quotes that curl")
            << u"x"_s + QChar(0x201d) + u"): taken. Device 2 ("_s + QChar(0x201c) + u"Rack 1"_s
            << u"\"x'): taken. Device 2 ('Rack 1\""_s;
        // Each of these is shown as an apostrophe; and of apostrophes side by side one is shown,
        // since two of them look like a double quote.
        QTest::newRow("quotes of full width, low quotes and double primes, each between two letters")
            << u"a"_s + QChar(0xff02) + u"b"_s + QChar(0x201e) + u"c"_s + QChar(0x2033) + u"d"_s + QChar(0x301e) +
                   u"e"_s
            << u"\"a'b'c'd'e\""_s;
        QTest::newRow("two apostrophes where a quote would close the name")
            << u"x''): taken. Device 2 (''Rack 1"_s << u"\"x'): taken. Device 2 ('Rack 1\""_s;
        QTest::newRow("quotes and apostrophes side by side") << u"x'\""_s + QChar(0x201d) + u"'y"_s << u"\"x'y\""_s;
        // What turns the direction of writing without being refused as a turn of it (the rule
        // for a line lets these three pass, as the server's does): shown without them.
        QTest::newRow("marks that say which way the writing runs")
            << u"Rack"_s + turnsRight + u" 9"_s + turnsLeft + turnsAsArabic << u"\"Rack 9\""_s;
        // A name of nothing to see is no name.
        QTest::newRow("a hyphen that shows only where a line breaks") << QString(QChar(0xad)) << QString();
        QTest::newRow("a filler that is a letter and shows nothing") << QString(QChar(0x3164)) << QString();
        QTest::newRow("signs nobody sees, of several kinds")
            << QString(QChar(0xad)) + QChar(0x3164) + QChar(0x200d) + QChar(0xa0) + turnsLeft + QChar(0x115f) +
                   QChar(0x2800)
            << QString();
        QTest::newRow("one letter among signs nobody sees") << QString(QChar(0xad)) + u"x"_s + QChar(0x3164)
                                                            << u"\""_s + QChar(0xad) + u"x"_s + QChar(0x3164) + u"\""_s;
    }

    void aNameFromAFileCannotEndItsOwnSettingOffInASentence()
    {
        QFETCH(QString, name);
        QFETCH(QString, shown);
        const QString inBrackets = shown.isEmpty() ? QString() : u" ("_s + shown + u")"_s;
        // In the sentence for a device whose address was refused,
        const QJsonObject refused = deviceWith({{u"url"_s, u"https://admin:hunter2@192.168.1.5"_s}, {u"name"_s, name}});
        QCOMPARE(leftOutOf(refused),
                 u"Device 1"_s + inBrackets + u": its address has a user name or a password in it"_s);
        // and in the one for a device that a file has twice.
        const Content twice = devicesOf({deviceWith({}), deviceWith({{u"name"_s, name}})});
        QCOMPARE(twice.leftOut, QStringList{u"Device 2"_s + inBrackets + u": the file has that address twice"_s});
        // By the one function that shows a name, which is what whoever shows a device of a
        // file shows its name by.
        QCOMPARE(core::exchange::nameSetOff(name), shown);
        // What is kept of the name is what the server keeps of it: the sentence alone is another.
        QCOMPARE(deviceOf({{u"name"_s, name}}).name, name);
    }

    void whatIsNoShortLineOfTextIsNoNameToShow()
    {
        using core::exchange::nameSetOff;
        // The rule a file has for a name: a line, of eighty signs at the most, with the blanks
        // around it gone and a tab made a blank.
        const auto inQuotes = [](const QString& name) -> QString { return u"\""_s + name + u"\""_s; };
        QCOMPARE(nameSetOff(u"  Rack\t9 \n"_s), u"\"Rack 9\""_s);
        QCOMPARE(nameSetOff(QString(80, u'n')), inQuotes(QString(80, u'n')));
        QCOMPARE(nameSetOff(wideSigns(80)), inQuotes(wideSigns(80)));
        QCOMPARE(nameSetOff(QString(81, u'n')), QString());
        QCOMPARE(nameSetOff(u"two\nlines"_s), QString());
        QCOMPARE(nameSetOff(u"Rack "_s + QChar(0x202e) + u"9"_s), QString());
        QCOMPARE(nameSetOff(u"Rack"_s + QChar(0xd83d)), QString());
        QCOMPARE(nameSetOff(QString()), QString());
        QCOMPARE(nameSetOff(u"   "_s), QString());
        // A mark that stands on a letter is shown with its letter, and is nothing to see alone.
        QCOMPARE(nameSetOff(u"Cafe"_s + QChar(0x301)), inQuotes(u"Cafe"_s + QChar(0x301)));
        QCOMPARE(nameSetOff(QString(QChar(0x301))), QString());
        // An apostrophe is shown as it is, and so are brackets: the quotes are what sets off.
        QCOMPARE(nameSetOff(u"Joe's rack (top)"_s), u"\"Joe's rack (top)\""_s);
    }

    void anAddressOfExactly255SignsIsOne()
    {
        // (What follows the host is dropped, and counts all the same.)
        const QString longest = u"https://192.168.1.20/"_s + QString(234, u'x');
        QCOMPARE(longest.size(), qsizetype{255});
        QCOMPARE(deviceOf({{u"url"_s, longest}}).address, u"https://192.168.1.20"_s);
    }

    void anAddressThatWasRefusedIsNeverShown_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QJsonValue>("name");
        QTest::addColumn<QString>("why");
        const QString somebody = u"its address has a user name or a password in it"_s;
        const QString otherWay = u"its address does not begin with http:// or https://"_s;
        const QString noPort = u"what stands behind the colon in its address is no port"_s;
        const QString noName = u"its address has a sign in it that no name of a host has"_s;
        const QString written = u"https://admin:hunter2@192.168.1.5"_s;
        const QJsonValue rack(u"Rack 9"_s);
        // Which device it was is said by its number, and by its name where that is a short line.
        QTest::newRow("a name and a password") << written << rack << u"Device 1 (\"Rack 9\"): "_s + somebody;
        QTest::newRow("of a device without a name")
            << written << QJsonValue(QJsonValue::Undefined) << u"Device 1: "_s + somebody;
        QTest::newRow("of a device whose name is no line")
            << written << QJsonValue(u"Rack\n9"_s) << u"Device 1: "_s + somebody;
        QTest::newRow("of a device whose name is longer than one may be")
            << written << QJsonValue(QString(81, u'n')) << u"Device 1: "_s + somebody;
        QTest::newRow("blanks around it")
            << u"  "_s + written + u" \n"_s << rack << u"Device 1 (\"Rack 9\"): "_s + somebody;
        QTest::newRow("a name alone") << u"https://admin@192.168.1.5"_s << rack
                                      << u"Device 1 (\"Rack 9\"): "_s + somebody;
        QTest::newRow("before a name in its xn-- form")
            << u"https://admin:hunter2@xn--mnchen-3ya.de"_s << rack << u"Device 1 (\"Rack 9\"): "_s + somebody;
        QTest::newRow("another way of speaking")
            << u"ftp://admin:hunter2@192.168.1.5"_s << rack << u"Device 1 (\"Rack 9\"): "_s + otherWay;
        QTest::newRow("in front of the way of speaking")
            << u"admin:hunter2@https://192.168.1.5"_s << rack << u"Device 1 (\"Rack 9\"): "_s + otherWay;
        // What the rule for an "@" could not see, and what was shown for that until this was
        // mended: the "@" spelled out, as whatever reads the address next would read it back;
        QTest::newRow("before an at sign that is spelled out")
            << u"https://admin:hunter2%40192.168.1.5"_s << rack << u"Device 1 (\"Rack 9\"): "_s + noPort;
        // a password where a port stands;
        QTest::newRow("a password where a port stands")
            << u"https://192.168.1.5:hunter2"_s << rack << u"Device 1 (\"Rack 9\"): "_s + noPort;
        // a token behind a question mark, in an address that is refused for its host;
        QTest::newRow("a token behind a question mark")
            << u"https://kvm example/?token=hunter2&user=admin"_s << rack << u"Device 1 (\"Rack 9\"): "_s + noName;
        QTest::newRow("a password in a path, behind a port that is none")
            << u"https://192.168.1.5:x/admin:hunter2@y"_s << rack << u"Device 1 (\"Rack 9\"): "_s + noPort;
        // and an "@" that only counts as one: the one of full width, which a keyboard for other
        // letters writes.
        QTest::newRow("before an at sign of full width")
            << u"https://admin:hunter2"_s + QChar(0xff20) + u"192.168.1.5"_s << rack
            << u"Device 1 (\"Rack 9\"): "_s + noPort;
        // An address that is none for nothing secret is not shown either: nobody can tell.
        QTest::newRow("words where an address stands")
            << u"https://hunter2 is the password of admin"_s << rack << u"Device 1 (\"Rack 9\"): "_s + noName;
    }

    void anAddressThatWasRefusedIsNeverShown()
    {
        // The server's sentence for what is no address shows the address, password and all.
        // Here an address that was refused is in no sentence: whether it holds a secret cannot
        // be told by its signs. What is said is which device it was, and what was wrong.
        QFETCH(QString, url);
        QFETCH(QJsonValue, name);
        QFETCH(QString, why);
        QJsonObject entry = deviceWith({{u"url"_s, url}});
        if (!name.isUndefined()) {
            entry[u"name"_s] = name;
        }
        const QString said = leftOutOf(entry);
        QVERIFY2(!said.contains(u"hunter2"_s) && !said.contains(u"admin"_s) && !said.contains(u"192.168"_s) &&
                     !said.contains(u"token"_s),
                 qPrintable(said));
        QCOMPARE(said, why);
        // (An @ behind the host is no name and no password: that device is taken, as the server takes it.)
        QCOMPARE(deviceOf({{u"url"_s, u"https://192.168.1.5/admin@x"_s}}).address, u"https://192.168.1.5"_s);
    }

    void aDeviceThatAFileHasTwiceIsTakenOnce()
    {
        const Content content = devicesOf({
            deviceWith({{u"name"_s, u"The first"_s}}),
            deviceWith({{u"name"_s, u"The same again"_s}}),
            deviceWith({{u"url"_s, u"HTTPS://192.168.1.20:443/"_s}, {u"name"_s, u"Written another way"_s}}),
            deviceWith({{u"url"_s, u"http://192.168.1.20"_s}, {u"name"_s, u"Spoken to another way"_s}}),
            deviceWith({{u"url"_s, u"https://192.168.1.20:8443"_s}}),
        });
        QVERIFY(content.devices.has_value());
        QCOMPARE(devicesIn(content).size(), qsizetype{3});
        QCOMPARE(devicesIn(content).at(0).name, u"The first"_s);
        QCOMPARE(devicesIn(content).at(1).name, u"Spoken to another way"_s);
        QCOMPARE(devicesIn(content).at(2).address, u"https://192.168.1.20:8443"_s);
        QCOMPARE(content.leftOut,
                 (QStringList{u"Device 2 (\"The same again\"): the file has that address twice"_s,
                              u"Device 3 (\"Written another way\"): the file has that address twice"_s}));
    }

    void aDevicesCertificateIsKeptAsTheServerKeepsOne()
    {
        const QString pem = theCertificate();
        const QByteArray der = derOf(pem);
        QVERIFY(pem.startsWith(u"-----BEGIN CERTIFICATE-----\n"_s) && der.size() > 200);
        const QByteArray sha256 = QCryptographicHash::hash(der, QCryptographicHash::Sha256);
        // As the server writes it; in one line; with Windows' ends of lines; with blanks around it.
        QString oneLine = pem;
        oneLine.remove(u'\n');
        QString windows = pem;
        windows.replace(u"\n"_s, u"\r\n"_s);
        for (const QString& written : {pem, oneLine, windows, u"\n  "_s + pem + u" \n\n"_s}) {
            const Device device = deviceOf({{u"cert_pem"_s, written}});
            QCOMPARE(device.certificatePem, pem);
            QCOMPARE(device.certificate, sha256);
        }
        // None: nothing, an empty text, and no word of one.
        QCOMPARE(deviceOf({{u"cert_pem"_s, QJsonValue()}}).certificate, QByteArray());
        QCOMPARE(deviceOf({{u"cert_pem"_s, QString()}}).certificate, QByteArray());
        QCOMPARE(deviceOf({}).certificatePem, QString());
        // Only a device that is known by a certificate has one: for one that Windows vouches
        // for, or that is not checked, what the file says of one is not looked at.
        QCOMPARE(deviceOf({{u"tls"_s, u"system"_s}, {u"cert_pem"_s, pem}}).certificatePem, QString());
        QCOMPARE(deviceOf({{u"tls"_s, u"insecure"_s}, {u"cert_pem"_s, u"not one"_s}}).certificate, QByteArray());
    }

    void whatIsNoCertificateIsNoDevice_data()
    {
        QTest::addColumn<QJsonValue>("written");
        const QString pem = theCertificate();
        const QByteArray der = derOf(pem);
        QTest::newRow("words between the two lines")
            << QJsonValue(u"-----BEGIN CERTIFICATE-----\nnot one\n-----END CERTIFICATE-----\n"_s);
        QTest::newRow("a letter") << QJsonValue(u"x"_s);
        QTest::newRow("a number") << QJsonValue(7);
        QTest::newRow("a no") << QJsonValue(false);
        QTest::newRow("a list") << QJsonValue(QJsonArray{pem});
        QTest::newRow("longer than a certificate is") << QJsonValue(pem + QString(20000, u' '));
        QTest::newRow("its bytes without the two lines") << QJsonValue(QString::fromLatin1(der.toBase64()));
        QTest::newRow("its first line only") << QJsonValue(pem.left(pem.indexOf(u"-----END"_s)));
        QTest::newRow("something before it") << QJsonValue(u"a certificate:\n"_s + pem);
        QTest::newRow("something behind it") << QJsonValue(pem + u"and more"_s);
        // The server takes the first of two and drops the second. Here two are none.
        QTest::newRow("the same one twice") << QJsonValue(pem + pem);
        QTest::newRow("another kind of thing between such lines")
            << QJsonValue(QString(pem).replace(u"CERTIFICATE"_s, u"PRIVATE KEY"_s));
        QTest::newRow("a sign of no alphabet among its bytes") << QJsonValue(QString(pem).replace(40, 1, u"*"_s));
        QTest::newRow("a letter that is not of the twenty-six among its bytes")
            << QJsonValue(QString(pem).replace(40, 1, QChar(0xfc)));
        // Bytes that are no certificate, and bytes that are one with something behind it.
        QTest::newRow("bytes that are none") << QJsonValue(pemOf(QByteArray(300, 'x')));
        QTest::newRow("a list of numbers, as its bytes are written")
            << QJsonValue(pemOf(QByteArray::fromHex("3003020101")));
        QTest::newRow("its bytes and one more") << QJsonValue(pemOf(der + QByteArray(1, '\0')));
        QTest::newRow("its bytes twice") << QJsonValue(pemOf(der + der));
        QTest::newRow("its bytes but for the last") << QJsonValue(pemOf(der.chopped(1)));
    }

    void whatIsNoCertificateIsNoDevice()
    {
        QFETCH(QJsonValue, written);
        QCOMPARE(leftOutOf(deviceWith({{u"cert_pem"_s, written}})), u"Device 1: its certificate is not one"_s);
    }

    void aCertificateWhoseLengthIsWrittenTheLongWayIsNone()
    {
        // A length has one spelling, in as few bytes as it takes. Written in more, it is a
        // certificate to some readers and none to others, and not what the device presents.
        const QByteArray der = derOf(theCertificate());
        // The whole of it is "30 82 01 1f ...": a sequence of 287 bytes, its length in two bytes.
        QCOMPARE(der.left(4).toHex(), QByteArray("3082011f"));
        const QByteArray longWay = QByteArray::fromHex("3083") + QByteArray(1, '\0') + der.mid(2);
        QCOMPARE(leftOutOf(deviceWith({{u"cert_pem"_s, pemOf(longWay)}})), u"Device 1: its certificate is not one"_s);
        // And the same inside it: what is signed is "30 81 c6 ...", a sequence of 198 bytes.
        QCOMPARE(der.mid(4, 3).toHex(), QByteArray("3081c6"));
        QByteArray inside = QByteArray::fromHex("30820120") + QByteArray::fromHex("3082") + QByteArray(1, '\0') +
                            QByteArray::fromHex("c6") + der.mid(7);
        QCOMPARE(inside.size(), der.size() + 1);
        QCOMPARE(leftOutOf(deviceWith({{u"cert_pem"_s, pemOf(inside)}})), u"Device 1: its certificate is not one"_s);
    }

    void ofTheCertificatesOfOneFileThatWindowsWillNotHave256AreAskedAndNoMore()
    {
        // The server has such a limit (MOST_ODD in its exchange.py) on the certificates of one
        // file that it has to ask its second reader about. Here: once Windows has refused 256
        // different ones, a certificate not seen before is none, unasked, in the server's
        // sentence; and one that was read before is what it was.
        const QString pem = theCertificate();
        const QByteArray sha256 = QCryptographicHash::hash(derOf(pem), QCryptographicHash::Sha256);
        // The same certificate, written another way: another text, and so looked at anew.
        QString windows = pem;
        windows.replace(u"\n"_s, u"\r\n"_s);
        const auto withSoMany = [&pem, &windows](int none) {
            QJsonArray entries;
            entries.append(deviceWith({{u"url"_s, u"https://first.example.net"_s}, {u"cert_pem"_s, pem}}));
            for (int number = 0; number < none; ++number) {
                // A list of one number: one piece, and no certificate. Each of them another.
                const QByteArray der = QByteArray::fromHex("30040202") + static_cast<char>(0x10 + number / 256) +
                                       static_cast<char>(number % 256);
                entries.append(
                    deviceWith({{u"url"_s, u"https://none%1.example.net"_s.arg(number)}, {u"cert_pem"_s, pemOf(der)}}));
            }
            entries.append(deviceWith({{u"url"_s, u"https://again.example.net"_s}, {u"cert_pem"_s, pem}}));
            entries.append(deviceWith({{u"url"_s, u"https://anew.example.net"_s}, {u"cert_pem"_s, windows}}));
            return devicesOf(entries);
        };
        // One fewer than the limit: every device that has the certificate is taken.
        const Content within = withSoMany(255);
        QCOMPARE(within.leftOut.size(), qsizetype{255});
        QCOMPARE(within.leftOut.first(), u"Device 2: its certificate is not one"_s);
        QCOMPARE(devicesIn(within).size(), qsizetype{3});
        QCOMPARE(devicesIn(within).value(2).address, u"https://anew.example.net"_s);
        QCOMPARE(devicesIn(within).value(2).certificate, sha256);
        // At the limit: the one that is looked at anew is none, in the server's sentence.
        const Content beyond = withSoMany(256);
        QCOMPARE(beyond.leftOut.size(), qsizetype{257});
        QCOMPARE(beyond.leftOut.last(), u"Device 259: its certificate is not one"_s);
        QCOMPARE(devicesIn(beyond).size(), qsizetype{2});
        QCOMPARE(devicesIn(beyond).value(1).address, u"https://again.example.net"_s);
        QCOMPARE(devicesIn(beyond).value(1).certificate, sha256);
        // One that many devices share counts once, however many they are.
        QJsonArray shared;
        const QString none = pemOf(QByteArray::fromHex("3003020101"));
        for (int number = 0; number < 300; ++number) {
            shared.append(
                deviceWith({{u"url"_s, u"https://shared%1.example.net"_s.arg(number)}, {u"cert_pem"_s, none}}));
        }
        shared.append(deviceWith({{u"url"_s, u"https://anew.example.net"_s}, {u"cert_pem"_s, windows}}));
        const Content many = devicesOf(shared);
        QCOMPARE(many.leftOut.size(), qsizetype{300});
        QCOMPARE(devicesIn(many).size(), qsizetype{1});
        QCOMPARE(devicesIn(many).value(0).certificate, sha256);
    }

    void aDevicesConsoleLinkIsAnAddressOfTheWebWithNobodysNameInIt_data()
    {
        QTest::addColumn<QJsonValue>("written");
        QTest::addColumn<QString>("taken"); // or why it is not, behind "Device 1: "
        const QString mustStart = u"Device 1: the console link must start with http:// or https://"_s;
        const QString noName = u"Device 1: the console link must not have a user name or password in it"_s;
        const QString notOne = u"Device 1: its console link is not one"_s;
        QTest::newRow("an address") << QJsonValue(u"https://192.168.1.22/kvm/"_s) << u"https://192.168.1.22/kvm/"_s;
        QTest::newRow("a plain one, with a port and a question")
            << QJsonValue(u"http://bench.example:8080/a?b=c#d"_s) << u"http://bench.example:8080/a?b=c#d"_s;
        QTest::newRow("kept as it is written, capitals and all")
            << QJsonValue(u"HTTPS://Bench.Example/Console"_s) << u"HTTPS://Bench.Example/Console"_s;
        QTest::newRow("blanks around it")
            << QJsonValue(u"  https://bench.example/ \n"_s) << u"https://bench.example/"_s;
        // What stands behind the host is kept as it is written, as the server keeps it: also
        // what an address by the book would spell out. (Whoever hands a link to a browser asks
        // whether it is one by the book; that is not the reader's to ask.)
        QTest::newRow("a blank and signs that the book wants spelled out, behind the host")
            << QJsonValue(u"https://bench.example/a b|{c}?d=\"e\""_s) << u"https://bench.example/a b|{c}?d=\"e\""_s;
        QTest::newRow("a port") << QJsonValue(u"https://192.168.1.22:8443"_s) << u"https://192.168.1.22:8443"_s;
        QTest::newRow("none") << QJsonValue(QString()) << QString();
        QTest::newRow("blanks only") << QJsonValue(u"   "_s) << QString();
        QTest::newRow("255 signs") << QJsonValue(u"https://bench.example/"_s + wideSigns(233))
                                   << u"https://bench.example/"_s + wideSigns(233);
        QTest::newRow("256 signs") << QJsonValue(u"https://bench.example/"_s + wideSigns(234)) << notOne;
        QTest::newRow("a number") << QJsonValue(7) << notOne;
        QTest::newRow("nothing, spelled out") << QJsonValue() << notOne;
        QTest::newRow("a script") << QJsonValue(u"javascript:alert(1)"_s) << mustStart;
        QTest::newRow("a script, in letters small and large, behind a blank")
            << QJsonValue(u" JaVaScRiPt:alert(1)"_s) << mustStart;
        QTest::newRow("data") << QJsonValue(u"data:text/html,<script>alert(1)</script>"_s) << mustStart;
        QTest::newRow("a file") << QJsonValue(u"file:///etc/passwd"_s) << mustStart;
        QTest::newRow("no host") << QJsonValue(u"https:///console"_s) << mustStart;
        QTest::newRow("one stroke") << QJsonValue(u"https:/bench.example/"_s) << mustStart;
        QTest::newRow("no way of speaking") << QJsonValue(u"bench.example/console"_s) << mustStart;
        QTest::newRow("a name and a password") << QJsonValue(u"https://u:p@console.example/"_s) << noName;
        QTest::newRow("an at sign and nothing before it") << QJsonValue(u"https://@console.example/"_s) << noName;
        // A device is not lost over a link the server keeps. These the server keeps as they are
        // written, and so does this: whether one can be opened is asked by whoever opens it.
        const QString umlaut = u"https://b"_s + QChar(0xfc) + u"ro.example/"_s;
        QTest::newRow("a letter that is not of the twenty-six in its host") << QJsonValue(umlaut) << umlaut;
        QTest::newRow("a port no computer has")
            << QJsonValue(u"https://console.example:99999/"_s) << u"https://console.example:99999/"_s;
        QTest::newRow("a port that is none")
            << QJsonValue(u"https://bench.example:port/"_s) << u"https://bench.example:port/"_s;
        QTest::newRow("a blank in its host") << QJsonValue(u"https://bench example/"_s) << u"https://bench example/"_s;
        QTest::newRow("signs no name has in its host")
            << QJsonValue(u"https://be<n>ch%zz.example/"_s) << u"https://be<n>ch%zz.example/"_s;
        const QString wideBlank = u"https://bench"_s + QChar(0x3000) + u"example/"_s;
        QTest::newRow("a blank of full width in its host") << QJsonValue(wideBlank) << wideBlank;
        // The sign for Kelvin counts as a "K" to the server's reader, which lets it pass. What
        // this row holds: a sign beyond ASCII that counts as a plain letter, and not as a sign
        // an address is cut at, does not cost the device (passesAsTheServersReaderHasIt). It
        // shows nothing of how a way of speaking is compared, and passes with that either way.
        const QString kelvin = u"https://"_s + QChar(0x212a) + u"vm.example/"_s;
        QTest::newRow("the sign for Kelvin in its host") << QJsonValue(kelvin) << kelvin;
        // These the server keeps too, as they are written, and so does this. What is kept is a
        // text for the next file, and no address: nothing of it is shown or opened but through
        // consoleAddress(), which takes none of these.
        const auto kept = [](const char* row, const QString& link) { QTest::newRow(row) << QJsonValue(link) << link; };
        kept("a line's end inside", u"https://bench.example/a\nb"_s);
        kept("a line's end in its host", u"https://bench.\nexample/"_s);
        kept("a tab in its way of speaking", u"ht\ttps://bench.example/"_s);
        kept("a turn of direction inside", u"https://bench.example/"_s + QChar(0x202e) + u"gpj.exe"_s);
        kept("a sign that moves the cursor in its host", u"https://bench"_s + QChar(0x1b) + u".example/"_s);
        kept("a line's end of the other kind in its host", u"https://bench"_s + QChar(0x2028) + u".example/"_s);
        kept("a backslash, which a browser reads as a stroke", u"https://one.example\\two.example/"_s);
        kept("a host that is none before a port", u"http://:80/"_s);
        kept("quotes and what a command line reads", u"https://bench.example/\"a\"&b|c<d>e^f%g'h'"_s);
        kept("signs that print nothing", u"https://bench.example/a"_s + QChar(0x200b) + QChar(0x200f) + QChar(0x2060) +
                                             QChar(0xfeff) + QChar(0xad) + u"b"_s);
        // Numbers of the new kind in brackets, by the rule that is written in this program.
        kept("numbers of the new kind", u"https://[fd00::1]/kvm"_s);
        kept("numbers of the new kind, in capitals, with a port", u"https://[FD00::A1]:8443/"_s);
        kept("numbers of the new kind with a port no computer has", u"https://[::1]:99999/"_s);
        kept("the old way inside the new", u"https://[::ffff:192.168.1.9]/"_s);
        // And these the server refuses, in words of its language's own: the device is left out.
        QTest::newRow("a host that is none") << QJsonValue(u"https://[::1/"_s) << notOne;
        QTest::newRow("a bracket that is alone, the other way") << QJsonValue(u"https://::1]/"_s) << notOne;
        // (Qt reads a host out of these two; the server's reader of numbers refuses both.)
        QTest::newRow("a group of numbers with 0x in front") << QJsonValue(u"https://[0x1::1]/"_s) << notOne;
        QTest::newRow("a group of numbers with a plus in front") << QJsonValue(u"https://[::+1]/"_s) << notOne;
        QTest::newRow("a group of five digits") << QJsonValue(u"https://[00001::1]/"_s) << notOne;
        QTest::newRow("two gaps in the numbers") << QJsonValue(u"https://[1::2::3]/"_s) << notOne;
        QTest::newRow("four plain numbers in brackets") << QJsonValue(u"https://[192.168.1.9]/"_s) << notOne;
        QTest::newRow("a stroke of full width in its host")
            << QJsonValue(u"https://bench.example"_s + QChar(0xff0f) + u"x/"_s) << notOne;
        QTest::newRow("the sign for care of in its host")
            << QJsonValue(u"https://bench"_s + QChar(0x2105) + u".example/"_s) << notOne;
        // With a tab or a line's end taken out, as the server's reader takes them out first.
        QTest::newRow("nothing but a tab where the host stands") << QJsonValue(u"https://\t/x"_s) << mustStart;
        QTest::newRow("a name and a password, with a tab among them")
            << QJsonValue(u"https://u:p\t@console.example/"_s) << noName;
        // The long s counts as an "s" where Qt compares without regard to small and large. To
        // the server's reader "http", a long s and "://" is no way of speaking.
        QTest::newRow("a long s where the s of https stands")
            << QJsonValue(u"http"_s + QChar(0x17f) + u"://console.example/"_s) << mustStart;
        QTest::newRow("a long s where the s of HTTPS stands")
            << QJsonValue(u"HTTP"_s + QChar(0x17f) + u"://console.example/"_s) << mustStart;
        // Stricter than the server, of whose reader one version takes this and another does not.
        QTest::newRow("a sign that prints nothing in front of it")
            << QJsonValue(QString(QChar(0x01)) + u"https://bench.example/"_s) << mustStart;
        // And stricter where brackets stand that not every version of that reader takes, or that
        // hold what the program's rule for numbers of the new kind does not take.
        QTest::newRow("brackets around what is no address") << QJsonValue(u"https://[bench]/"_s) << notOne;
        QTest::newRow("brackets in the middle of a name") << QJsonValue(u"https://be[n]ch.example/"_s) << notOne;
        QTest::newRow("something before the brackets") << QJsonValue(u"https://x[::1]/"_s) << notOne;
        QTest::newRow("something behind the brackets") << QJsonValue(u"https://[::1]x/"_s) << notOne;
        QTest::newRow("numbers of the new kind with a zone") << QJsonValue(u"https://[fe80::1%25eth0]/"_s) << notOne;
        QTest::newRow("the other thing brackets may hold") << QJsonValue(u"https://[v1.bench]/"_s) << notOne;
    }

    void aDevicesConsoleLinkIsAnAddressOfTheWebWithNobodysNameInIt()
    {
        QFETCH(QJsonValue, written);
        QFETCH(QString, taken);
        const QJsonObject entry = deviceWith({{u"open_url"_s, written}});
        if (taken.startsWith(u"Device 1: "_s)) {
            // The device is left out.
            QCOMPARE(leftOutOf(entry), taken);
            QCOMPARE(devicesIn(devicesOf({entry})).size(), qsizetype{0});
        } else {
            QCOMPARE(leftOutOf(entry), QString());
            QCOMPARE(linkOf(deviceOf({{u"open_url"_s, written}})), taken);
        }
    }

    void theOneWayFromAConsoleLinkToAnAddressIsTheStrictOne_data()
    {
        QTest::addColumn<QString>("link");
        QTest::addColumn<QString>("address"); // as it is to be opened; nothing: it is none
        const auto none = [](const char* row, const QString& link) { QTest::newRow(row) << link << QString(); };
        const auto one = [](const char* row, const QString& link, const QString& address) {
            QTest::newRow(row) << link << address;
        };
        one("an address", u"https://192.168.1.22/kvm/"_s, u"https://192.168.1.22/kvm/"_s);
        one("a plain one, with a port, a question and a cross", u"http://bench.example:8080/a?b=c#d"_s,
            u"http://bench.example:8080/a?b=c#d"_s);
        one("in capitals: the host in small letters, the path as it is", u"HTTPS://Bench.Example/Console"_s,
            u"https://bench.example/Console"_s);
        one("numbers of the new kind", u"https://[FD00::A1]:8443/x"_s, u"https://[fd00::a1]:8443/x"_s);
        one("a name in its xn-- form", u"https://xn--mnchen-3ya.example/"_s, u"https://xn--mnchen-3ya.example/"_s);
        one("no path at all", u"https://bench.example"_s, u"https://bench.example"_s);
        // Written the one way: the port it has anyway is not written, nor a colon with nothing
        // behind it.
        one("the port it has anyway", u"https://bench.example:443/kvm"_s, u"https://bench.example/kvm"_s);
        one("a colon and no port", u"http://bench.example:/kvm"_s, u"http://bench.example/kvm"_s);
        // An "@" behind the host names nobody: it is a sign of a path, of a question or of
        // what stands behind a cross, and the host is the one in front.
        one("an at sign in its path", u"https://bench.example/a@other.example"_s,
            u"https://bench.example/a@other.example"_s);
        one("an at sign behind a question mark", u"https://bench.example?x@other.example"_s,
            u"https://bench.example?x@other.example"_s);
        one("an at sign behind a cross", u"https://bench.example#@other.example"_s,
            u"https://bench.example#@other.example"_s);
        // A cross and what stands behind it are part of what is opened.
        one("a cross right behind the host", u"https://bench.example#tile"_s, u"https://bench.example#tile"_s);
        one("a cross behind a path, as a wall's own links have one", u"https://192.168.1.22/#/kvm"_s,
            u"https://192.168.1.22/#/kvm"_s);
        // This PC's own addresses are handed out, and said to be that (the next test).
        one("this PC by its numbers", u"http://127.0.0.1:8080/"_s, u"http://127.0.0.1:8080/"_s);
        one("this PC by its name", u"http://localhost:8080/"_s, u"http://localhost:8080/"_s);
        none("no link", QString());
        // The way of speaking: the two, compared sign for sign. (A way of speaking with a sign
        // beyond ASCII in it, the long s among them, is none by the first rule of all, and so
        // shows nothing of how the two are compared: the reader's rows for a console link and
        // for where alerts go show that.)
        none("a script", u"javascript:alert(1)"_s);
        none("a file", u"file:///C:/Windows/System32/calc.exe"_s);
        none("another way of speaking", u"ftp://bench.example/"_s);
        none("one stroke", u"https:/bench.example/"_s);
        // A host by the program's rule, a port that is one, and nobody's name.
        none("no host", u"https:///console"_s);
        none("a host that is none before a port", u"http://:80/"_s);
        none("a letter that is not of the twenty-six in its host", u"https://b"_s + QChar(0xfc) + u"ro.example/"_s);
        // The program's rule drops a dot at a name's end and Qt keeps it: what would be opened
        // is not what was written, and nothing is handed out.
        none("a dot at its name's end", u"https://bench.example./kvm"_s);
        none("a name in its xn-- form that is none under an ending Qt has a list of", u"https://xn--zzzzzz.com/"_s);
        none("numbers that are no address", u"https://999.1.1.1/"_s);
        none("numbers as Qt alone reads them", u"https://127.1/"_s);
        none("numbers of the new kind as Qt alone reads them", u"https://[0x1::1]/"_s);
        none("a port no computer has", u"https://console.example:99999/"_s);
        none("port nought", u"https://console.example:0/"_s);
        none("a port that is none", u"https://bench.example:port/"_s);
        none("a name and a password", u"https://u:p@console.example/"_s);
        none("an at sign and nothing before it", u"https://@console.example/"_s);
        // None of the signs that a command line or a browser reads as more than a sign.
        none("a blank in its path", u"https://bench.example/a b"_s);
        none("a blank in its host", u"https://bench example/"_s);
        none("a blank before it", u" https://bench.example/"_s);
        none("a tab", u"https://bench.example/a\tb"_s);
        none("a line's end", u"https://bench.example/a\nb"_s);
        none("a double quote", u"https://bench.example/\"a\""_s);
        none("a single quote", u"https://bench.example/'a'"_s);
        none("a quote that leans back", u"https://bench.example/`a`"_s);
        none("an and", u"https://bench.example/?a=1&b=2"_s);
        none("a bar", u"https://bench.example/a|calc"_s);
        none("an arrow to the left", u"https://bench.example/a<b"_s);
        none("an arrow to the right", u"https://bench.example/a>b"_s);
        none("a caret", u"https://bench.example/a^b"_s);
        none("a percent sign", u"https://bench.example/%TEMP%"_s);
        none("a sign spelled out by a percent sign", u"https://bench.example/a%20b"_s);
        none("a backslash, which a browser reads as a stroke", u"https://one.example\\two.example/"_s);
        none("a backslash in its path", u"https://bench.example/a\\b"_s);
        // Nothing that prints nothing, and no letter beyond ASCII anywhere.
        for (const int unseen : {0x200b, 0x200c, 0x200d, 0x200e, 0x200f, 0x2060, 0xfeff, 0xad, 0x202e, 0x7f}) {
            const QByteArray row = "the sign " + QByteArray::number(unseen, 16) + " in its path";
            QTest::newRow(row.constData()) << u"https://bench.example/a"_s + QChar(unseen) + u"b"_s << QString();
        }
        none("a sign that prints nothing in its host", u"https://bench"_s + QChar(0x200b) + u".example/"_s);
        none("a letter that is not of the twenty-six in its path", u"https://bench.example/t"_s + QChar(0xfc) + u"r"_s);
        none("a sign of two places in its path", u"https://bench.example/"_s + wideSigns(1));
        none("longer than a link is", u"https://bench.example/"_s + QString(234, u'x'));
    }

    void theOneWayFromAConsoleLinkToAnAddressIsTheStrictOne()
    {
        // What a file's console link is kept as is a text (LinkAsWritten). The one way from it to
        // something that may be opened or shown hands back an address by the strict rule, or
        // nothing.
        QFETCH(QString, link);
        QFETCH(QString, address);
        const std::optional<core::exchange::ConsoleAddress> made = core::exchange::consoleAddress(LinkAsWritten(link));
        QCOMPARE(made.has_value() ? made->toOpen() : QString(), address);
        if (made.has_value()) {
            // Every sign of every word of it is one of the plain ones, from behind the blank
            // to the tilde; and its host is what stands in the address behind how it is spoken to.
            const auto isPlain = [](const QString& word) {
                return !word.isEmpty() && std::ranges::all_of(word, [](QChar sign) {
                    return sign.unicode() > 0x20 && sign.unicode() < 0x7f;
                });
            };
            QVERIFY2(isPlain(made->toOpen()) && isPlain(made->host()), qPrintable(link));
            QVERIFY(made->toOpen().startsWith((made->isEncrypted() ? u"https://"_s : u"http://"_s) + made->host()));
            QVERIFY(made->port() >= 1 && made->port() <= 65535);
            // And it is what a device of a file has, read from one: kept as written, and the
            // same address by the one way.
            const Device device = deviceOf({{u"open_url"_s, link}});
            QCOMPARE(linkOf(device), link);
            QCOMPARE(core::exchange::consoleAddress(device.consoleLinkAsWritten), made);
        }
    }

    void everyWordOfAnAddressThatIsHandedOutIsAscii_data()
    {
        // What is handed out of a console link was a QUrl. Under the endings Qt has a list of
        // (.com, .de), a QUrl's own words show a name in its "xn--" form in the letters it
        // stands for: the first of these read as the Cyrillic look-alike of a well-known name
        // in host(), toString() and toDisplayString(). Now it is a type of this piece's own,
        // and every word of it is compared here, sign for sign.
        QTest::addColumn<QString>("link");
        QTest::addColumn<QString>("toOpen");
        QTest::addColumn<QString>("host");
        QTest::addColumn<int>("port");
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<bool>("thisPcsOwn");
        const auto row = [](const char* name, const QString& link, const QString& toOpen, const QString& host, int port,
                            bool encrypted, bool thisPcsOwn = false) {
            QTest::newRow(name) << link << toOpen << host << port << encrypted << thisPcsOwn;
        };
        row("a look-alike of a well-known name, under .com", u"https://xn--80ak6aa92e.com/console"_s,
            u"https://xn--80ak6aa92e.com/console"_s, u"xn--80ak6aa92e.com"_s, 443, true);
        row("the same in capitals, plain, with a port, a question and a cross",
            u"HTTP://XN--80AK6AA92E.COM:8080/Console?b=c#d"_s, u"http://xn--80ak6aa92e.com:8080/Console?b=c#d"_s,
            u"xn--80ak6aa92e.com"_s, 8080, false);
        row("a town, under .de", u"https://xn--mnchen-3ya.de"_s, u"https://xn--mnchen-3ya.de"_s, u"xn--mnchen-3ya.de"_s,
            443, true);
        row("a part that is not the first, under .com", u"https://kvm.xn--mnchen-3ya.com:8443/"_s,
            u"https://kvm.xn--mnchen-3ya.com:8443/"_s, u"kvm.xn--mnchen-3ya.com"_s, 8443, true);
        row("a town, under an ending Qt has no list for", u"https://xn--mnchen-3ya.example/"_s,
            u"https://xn--mnchen-3ya.example/"_s, u"xn--mnchen-3ya.example"_s, 443, true);
        row("a name", u"http://Bench.Example/Console"_s, u"http://bench.example/Console"_s, u"bench.example"_s, 80,
            false);
        row("four numbers and the port they have anyway", u"https://192.168.1.22:443/kvm/"_s,
            u"https://192.168.1.22/kvm/"_s, u"192.168.1.22"_s, 443, true);
        row("numbers of the new kind, in brackets and the short way", u"https://[FD00:0:0::A1]:8443/x"_s,
            u"https://[fd00::a1]:8443/x"_s, u"[fd00::a1]"_s, 8443, true);
        // This PC's own addresses: handed out, and said to be that. A console is often reached
        // through a tunnel that ends at this PC; and a file from somebody else can point "Open
        // console" at whatever listens here, so whoever shows the link says so.
        row("this PC by its name", u"http://localhost:8080/"_s, u"http://localhost:8080/"_s, u"localhost"_s, 8080,
            false, true);
        row("a name that ends in .localhost", u"https://kvm.LocalHost/"_s, u"https://kvm.localhost/"_s,
            u"kvm.localhost"_s, 443, true, true);
        row("this PC by its numbers", u"http://127.0.0.1/"_s, u"http://127.0.0.1/"_s, u"127.0.0.1"_s, 80, false, true);
        row("any of the numbers that are this PC", u"https://127.8.9.10:8443/"_s, u"https://127.8.9.10:8443/"_s,
            u"127.8.9.10"_s, 8443, true, true);
        row("this PC by its numbers of the new kind", u"https://[0:0:0:0:0:0:0:1]/"_s, u"https://[::1]/"_s, u"[::1]"_s,
            443, true, true);
        row("this PC, the old way inside the new", u"https://[::ffff:127.0.0.1]/"_s, u"https://[::ffff:127.0.0.1]/"_s,
            u"[::ffff:127.0.0.1]"_s, 443, true, true);
        row("the address of no place", u"http://0.0.0.0:8080/"_s, u"http://0.0.0.0:8080/"_s, u"0.0.0.0"_s, 8080, false,
            true);
        row("the address of no place, of the new kind", u"http://[::]/"_s, u"http://[::]/"_s, u"[::]"_s, 80, false,
            true);
        // Not this PC's own: a name that only has the word in it, and numbers beside its own.
        row("a name with localhost in it", u"https://localhost.example/"_s, u"https://localhost.example/"_s,
            u"localhost.example"_s, 443, true, false);
        row("numbers of a home network", u"https://192.168.1.22/"_s, u"https://192.168.1.22/"_s, u"192.168.1.22"_s, 443,
            true, false);
        row("numbers a network gives itself", u"https://169.254.1.22/"_s, u"https://169.254.1.22/"_s, u"169.254.1.22"_s,
            443, true, false);
    }

    void everyWordOfAnAddressThatIsHandedOutIsAscii()
    {
        QFETCH(QString, link);
        QFETCH(QString, toOpen);
        QFETCH(QString, host);
        QFETCH(int, port);
        QFETCH(bool, encrypted);
        QFETCH(bool, thisPcsOwn);
        const std::optional<core::exchange::ConsoleAddress> made = core::exchange::consoleAddress(LinkAsWritten(link));
        if (!made.has_value()) {
            QFAIL("nothing was handed out");
        }
        // (QCOMPARE of two texts compares them sign for sign, and prints what is beyond ASCII
        // by its number.)
        QCOMPARE(made->toOpen(), toOpen);
        QCOMPARE(made->host(), host);
        QCOMPARE(made->port(), port);
        QCOMPARE(made->isEncrypted(), encrypted);
        QCOMPARE(made->isThisPcsOwn(), thisPcsOwn);
        for (const QString& word : {made->toOpen(), made->host()}) {
            for (const QChar sign : word) {
                QVERIFY2(sign.unicode() > 0x20 && sign.unicode() < 0x7f, qPrintable(link));
            }
        }
        // There is no way from it to a name in other letters: it is no QUrl and hands out none.
        QVERIFY((!std::is_convertible_v<core::exchange::ConsoleAddress, QUrl>));
        QVERIFY((!std::is_constructible_v<QUrl, core::exchange::ConsoleAddress>));
        // And nobody but the one function makes one: not out of nothing, and not out of words.
        QVERIFY(!std::is_default_constructible_v<core::exchange::ConsoleAddress>);
        QVERIFY((!std::is_constructible_v<core::exchange::ConsoleAddress, QString>));
        QVERIFY((!std::is_constructible_v<core::exchange::ConsoleAddress, QString, QString, int, bool, bool>));
    }

    void aLinkThatAFileKeepsAndThatIsNoAddressIsNeverHandedOutAsOne()
    {
        // Each of these is kept, since the server keeps it and may be handed the file again; and
        // of each the one way to an address hands back nothing.
        const QStringList keptAndNone{
            u"https://one.example\\two.example/"_s,
            u"http://:80/"_s,
            u"https://bench.example/a b"_s,
            u"https://bench.example/\"a\"&b|c<d>e^f%g"_s,
            u"https://bench.example/a\nb"_s,
            u"https://bench.example/"_s + QChar(0x202e) + u"gpj.exe"_s,
            u"https://bench.example/a"_s + QChar(0x200b) + u"b"_s,
            u"https://b"_s + QChar(0xfc) + u"ro.example/"_s,
            u"https://console.example:99999/"_s,
        };
        for (const QString& link : keptAndNone) {
            const Device device = deviceOf({{u"open_url"_s, link}, {u"password"_s, u"made-up-password"_s}});
            QCOMPARE(linkOf(device), link);
            QCOMPARE(device.password, u"made-up-password"_s);
            QVERIFY2(!core::exchange::consoleAddress(device.consoleLinkAsWritten).has_value(), qPrintable(link));
        }
    }

    void whatADeviceSaysItIsIsTakenAndAMarkOfAQkvmsOwnIsNot_data()
    {
        QTest::addColumn<QJsonValue>("written");
        QTest::addColumn<QString>("taken");
        const QString digits(16, u'a');
        QTest::newRow("as a driver gives it")
            << QJsonValue(u"jetkvm:0123456789abcdef"_s) << u"jetkvm:0123456789abcdef"_s;
        QTest::newRow("two letters and sixteen digits") << QJsonValue(u"aa:"_s + digits) << u"aa:"_s + digits;
        QTest::newRow("twelve letters and sixty-four digits")
            << QJsonValue(QString(12, u'z') + u':' + QString(64, u'0')) << QString(12, u'z') + u':' + QString(64, u'0');
        QTest::newRow("a word that begins as a mark of a qKVM's own does")
            << QJsonValue(u"owner:"_s + digits) << u"owner:"_s + digits;
        // The marks a QKVM makes itself are worked out anew on the wall that takes a file in.
        QTest::newRow("somebody's decision on another wall") << QJsonValue(u"own:"_s + digits) << QString();
        QTest::newRow("a certificate's mark") << QJsonValue(u"cert:"_s + QString(64, u'a')) << QString();
        QTest::newRow("one letter") << QJsonValue(u"a:"_s + digits) << QString();
        QTest::newRow("thirteen letters") << QJsonValue(QString(13, u'a') + u':' + digits) << QString();
        QTest::newRow("fifteen digits") << QJsonValue(u"aa:"_s + QString(15, u'a')) << QString();
        QTest::newRow("sixty-five digits") << QJsonValue(u"aa:"_s + QString(65, u'a')) << QString();
        QTest::newRow("capitals in its family") << QJsonValue(u"JetKVM:"_s + digits) << QString();
        QTest::newRow("capitals in its digits") << QJsonValue(u"aa:"_s + QString(16, u'A')) << QString();
        QTest::newRow("a digit that is none") << QJsonValue(u"aa:"_s + QString(16, u'g')) << QString();
        QTest::newRow("a line's end behind it") << QJsonValue(u"aa:"_s + digits + u'\n') << QString();
        QTest::newRow("no colon") << QJsonValue(u"aa"_s + digits) << QString();
        QTest::newRow("two colons") << QJsonValue(u"aa:bb:"_s + digits) << QString();
        QTest::newRow("a number") << QJsonValue(7) << QString();
        QTest::newRow("nothing, spelled out") << QJsonValue() << QString();
    }

    void whatADeviceSaysItIsIsTakenAndAMarkOfAQkvmsOwnIsNot()
    {
        QFETCH(QJsonValue, written);
        QFETCH(QString, taken);
        // (The device is taken either way: a mark that is none is no mark.)
        QCOMPARE(leftOutOf(deviceWith({{u"ident"_s, written}})), QString());
        QCOMPARE(deviceOf({{u"ident"_s, written}}).ident, taken);
    }

    void theNumbersItsMachineGaveForItselfAreReadAsADriverGivesThem_data()
    {
        // M7 1b-ii: a server's `machine`, as the server's reader has it (exchange.py, _devices): as a driver gives
        // one, or none in so many words; anything else there leaves the device out, since it is no "none".
        QTest::addColumn<QJsonValue>("written");
        QTest::addColumn<bool>("leftOut");
        QTest::addColumn<QString>("taken");
        const QString both = u"server:"_s + QString(32, u'a') + QString(32, u'b');
        QTest::newRow("both of a server's numbers") << QJsonValue(both) << false << both;
        QTest::newRow("its computer's alone")
            << QJsonValue(u"server:"_s + QString(32, u'c')) << false << u"server:"_s + QString(32, u'c');
        QTest::newRow("none, said so") << QJsonValue(u""_s) << false << QString();
        QTest::newRow("a mark of a qKVM's own") << QJsonValue(u"own:"_s + QString(16, u'a')) << true << QString();
        QTest::newRow("of no such shape") << QJsonValue(u"server:xyz"_s) << true << QString();
        QTest::newRow("capitals") << QJsonValue(u"SERVER:"_s + QString(32, u'a')) << true << QString();
        QTest::newRow("a number") << QJsonValue(7) << true << QString();
        QTest::newRow("nothing, spelled out") << QJsonValue() << true << QString();
    }

    void theNumbersItsMachineGaveForItselfAreReadAsADriverGivesThem()
    {
        QFETCH(QJsonValue, written);
        QFETCH(bool, leftOut);
        QFETCH(QString, taken);
        const QJsonObject entry = deviceWith({{u"kind"_s, u"redfish"_s}, {u"machine"_s, written}});
        if (leftOut) {
            QCOMPARE(leftOutOf(entry), u"Device 1: the number its machine gave for itself is not one"_s);
            return;
        }
        QCOMPARE(leftOutOf(entry), QString());
        QCOMPARE(deviceOf({{u"kind"_s, u"redfish"_s}, {u"machine"_s, written}}).machine, taken);
    }

    void aServersNumberAFileOfBeforeKeptAsWhatItIsIsItsMachine()
    {
        // A file of 0.1.x has no `machine`: a server's tile kept the number its computer gives as what the server
        // is (`ident`). That is its machine, as at an upgrade (store.machine_from_before); and only where the field is
        // not there at all, only of a server, and only of that shape.
        const QString number = u"server:"_s + QString(32, u'd');
        QCOMPARE(deviceOf({{u"kind"_s, u"redfish"_s}, {u"ident"_s, number}}).machine, number);
        QCOMPARE(deviceOf({{u"kind"_s, u"redfish"_s}, {u"ident"_s, number}}).ident, number);
        QCOMPARE(deviceOf({{u"kind"_s, u"redfish"_s}, {u"ident"_s, number}, {u"machine"_s, u""_s}}).machine, QString());
        QCOMPARE(deviceOf({{u"kind"_s, u"kvmd"_s}, {u"ident"_s, number}}).machine, QString());
        QCOMPARE(deviceOf({{u"kind"_s, u"redfish"_s}, {u"ident"_s, number + u'e'}}).machine, QString());
        QCOMPARE(deviceOf({{u"kind"_s, u"redfish"_s}, {u"ident"_s, u"jetkvm:"_s + QString(32, u'd')}}).machine,
                 QString());
    }

    void whatIsNeitherYesNorNoIsWhatADeviceIsWithoutIt()
    {
        QCOMPARE(deviceOf({{u"alerts"_s, false}}).alerts, false);
        QCOMPARE(deviceOf({{u"alerts"_s, true}}).alerts, true);
        QCOMPARE(deviceOf({{u"alerts"_s, 0}}).alerts, true);
        QCOMPARE(deviceOf({{u"alerts"_s, u"no"_s}}).alerts, true);
        QCOMPARE(deviceOf({{u"alerts"_s, QJsonValue()}}).alerts, true);
        QCOMPARE(deviceOf({{u"keep_both"_s, true}}).keepBoth, true);
        QCOMPARE(deviceOf({{u"keep_both"_s, false}}).keepBoth, false);
        QCOMPARE(deviceOf({{u"keep_both"_s, 1}}).keepBoth, false);
        QCOMPARE(deviceOf({{u"keep_both"_s, u"true"_s}}).keepBoth, false);
        QCOMPARE(deviceOf({}).keepBoth, false);
    }

    void whatIsNoEntryIsLeftOutAndTheRestIsRead()
    {
        const Content content = devicesOf({
            deviceWith({}),
            u"not an entry"_s,
            7,
            QJsonArray{deviceWith({})},
            QJsonValue(),
            deviceWith({{u"url"_s, u"https://192.168.1.32"_s}}),
        });
        QCOMPARE(devicesIn(content).size(), qsizetype{2});
        QCOMPARE(devicesIn(content).at(1).address, u"https://192.168.1.32"_s);
        QCOMPARE(content.leftOut, (QStringList{u"Device 2: not an entry"_s, u"Device 3: not an entry"_s,
                                               u"Device 4: not an entry"_s, u"Device 5: not an entry"_s}));
        // And what is no list of devices is none, said once; the file has such a part all the same.
        for (const QJsonValue& notAList :
             {QJsonValue(QJsonObject()), QJsonValue(u"x"_s), QJsonValue(7), QJsonValue()}) {
            const Content none = contentOf({{u"devices"_s, notAList}});
            QVERIFY(none.devices.has_value() && devicesIn(none).isEmpty());
            QCOMPARE(none.leftOut, QStringList{u"The list of devices is not a list"_s});
        }
        QVERIFY(!contentOf({}).devices.has_value());
    }

    void moreDevicesThanAFileHoldsAreNotReadToTheirEnd()
    {
        QJsonArray many;
        for (int number = 0; number < 2005; ++number) {
            many.append(deviceWith({{u"url"_s, u"https://kvm%1.example"_s.arg(number)}}));
        }
        const Content content = devicesOf(many);
        QCOMPARE(devicesIn(content).size(), qsizetype{2000});
        QCOMPARE(devicesIn(content).last().address, u"https://kvm1999.example"_s);
        QCOMPARE(content.leftOut, QStringList{u"More than 2000 devices: the ones after that"_s});
        // Exactly as many as a file holds are all read, and nothing is said.
        many = QJsonArray();
        for (int number = 0; number < 2000; ++number) {
            many.append(deviceWith({{u"url"_s, u"https://kvm%1.example"_s.arg(number)}}));
        }
        const Content all = devicesOf(many);
        QCOMPARE(devicesIn(all).size(), qsizetype{2000});
        QCOMPARE(all.leftOut, QStringList());
        // The ones after that are not looked at, whatever they are.
        many.append(u"not an entry"_s);
        QCOMPARE(devicesOf(many).leftOut, QStringList{u"More than 2000 devices: the ones after that"_s});
    }

    // --- the entries: saved actions -----------------------------------------------------------------

    void aSavedActionPressesKeysOrTypesAText()
    {
        Action keys;
        keys.name = u"Lock"_s;
        keys.keys = u"MetaLeft,KeyL"_s;
        QCOMPARE(actionOf({}), keys);
        QCOMPARE(actionOf({{u"danger"_s, true}}).danger, true);
        // Enter belongs to a text; for keys it is not taken, and neither is a text.
        QCOMPARE(actionOf({{u"enter"_s, true}, {u"text"_s, u"not typed"_s}}), keys);
        Action text;
        text.name = u"Hello"_s;
        text.types = true;
        text.text = u" hello\tyou\n"_s;
        text.enter = true;
        QCOMPARE(actionOf({{u"name"_s, u"  Hello "_s},
                           {u"kind"_s, u"text"_s},
                           {u"text"_s, u" hello\tyou\n"_s},
                           {u"enter"_s, true},
                           {u"keys"_s, u"KeyA"_s},
                           {u"id"_s, u"11112222"_s},
                           {u"changed"_s, 99}}),
                 text);
        QCOMPARE(actionOf({{u"kind"_s, u"text"_s}, {u"text"_s, u"x"_s}, {u"enter"_s, u"yes"_s}}).enter, false);
        QVERIFY(contentOf({{u"actions"_s, QJsonArray{actionWith({{u"kind"_s, u"text"_s}, {u"text"_s, u"x"_s}})}}})
                    .hasSecrets());
    }

    void whatIsNoSavedActionIsLeftOut_data()
    {
        QTest::addColumn<QJsonValue>("entry");
        QTest::addColumn<QString>("why"); // nothing: it is taken
        const QString noName = u"Saved action 1: it has no name"_s;
        const QString notKeys = u"Saved action 1: its keys are not keys this version knows"_s;
        const QString noText = u"Saved action 1: it has no text to type, or more than can be typed at once"_s;
        const QString notAKind = u"Saved action 1: it is not a kind of action this version knows"_s;
        const auto text = [](const QJsonValue& typed) {
            return QJsonValue(actionWith({{u"kind"_s, u"text"_s}, {u"text"_s, typed}}));
        };
        const auto keys = [](const QJsonValue& pressed) { return QJsonValue(actionWith({{u"keys"_s, pressed}})); };
        const auto named = [](const QJsonValue& name) { return QJsonValue(actionWith({{u"name"_s, name}})); };
        QJsonObject noNameAtAll = actionWith({});
        noNameAtAll.remove(u"name"_s);
        QTest::newRow("a text") << QJsonValue(u"not an entry"_s) << u"Saved action 1: not an entry"_s;
        QTest::newRow("a list") << QJsonValue(QJsonArray{actionWith({})}) << u"Saved action 1: not an entry"_s;
        QTest::newRow("no name") << QJsonValue(noNameAtAll) << noName;
        QTest::newRow("a name with nothing in it") << named(QString()) << noName;
        QTest::newRow("a name of blanks") << named(u"  \t "_s) << noName;
        QTest::newRow("a name of two lines") << named(u"two\nlines"_s) << noName;
        QTest::newRow("a number as its name") << named(7) << noName;
        QTest::newRow("a name of sixty signs") << named(wideSigns(60)) << QString();
        QTest::newRow("a name of sixty-one signs") << named(wideSigns(61)) << noName;
        // Its name is asked for before its kind.
        QTest::newRow("no name and no kind") << QJsonValue(QJsonObject{{u"kind"_s, u"script"_s}}) << noName;
        QTest::newRow("another kind") << QJsonValue(actionWith({{u"kind"_s, u"script"_s}, {u"text"_s, u"rm -rf /"_s}}))
                                      << notAKind;
        QTest::newRow("a kind in capitals") << QJsonValue(actionWith({{u"kind"_s, u"Keys"_s}})) << notAKind;
        QTest::newRow("no kind") << QJsonValue(QJsonObject{{u"name"_s, u"Lock"_s}, {u"keys"_s, u"KeyA"_s}}) << notAKind;
        QTest::newRow("one key") << keys(u"KeyA"_s) << QString();
        QTest::newRow("eight keys") << keys(u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG,KeyH"_s) << QString();
        QTest::newRow("nine keys") << keys(u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG,KeyH,KeyI"_s) << notKeys;
        QTest::newRow("a key of two signs") << keys(u"F1"_s) << QString();
        QTest::newRow("a key of one sign") << keys(u"F"_s) << notKeys;
        QTest::newRow("a key of sixteen signs") << keys(u"K"_s + QString(15, u'a')) << QString();
        QTest::newRow("a key of seventeen signs") << keys(u"K"_s + QString(16, u'a')) << notKeys;
        QTest::newRow("words that are no key") << keys(u"not a key name"_s) << notKeys;
        QTest::newRow("a key that begins small") << keys(u"keyA"_s) << notKeys;
        QTest::newRow("a key that begins with a digit") << keys(u"1Key"_s) << notKeys;
        // The keyboard's Power key, as the web's reader says it (exchange.py, POWER_KEY_REFUSED).
        const QString power =
            u"Saved action 1: the keyboard's Power key is not sent. A machine's power is under Power."_s;
        QTest::newRow("the keyboard's Power key") << keys(u"Power"_s) << power;
        QTest::newRow("the keyboard's Power key held with others") << keys(u"ControlLeft,AltLeft,Power"_s) << power;
        QTest::newRow("a name that begins as the Power key's") << keys(u"PowerOff"_s) << QString();
        QTest::newRow("a comma and nothing behind it") << keys(u"KeyA,"_s) << notKeys;
        QTest::newRow("a blank behind a comma") << keys(u"KeyA, KeyB"_s) << notKeys;
        QTest::newRow("a line's end behind the keys") << keys(u"KeyA\n"_s) << notKeys;
        QTest::newRow("a letter that is not of the twenty-six") << keys(u"Key"_s + QChar(0xc4)) << notKeys;
        QTest::newRow("no keys at all") << keys(QString()) << notKeys;
        QTest::newRow("a list of keys") << keys(QJsonArray{u"KeyA"_s}) << notKeys;
        QTest::newRow("a command where keys belong") << keys(u"rm -rf"_s) << notKeys;
        QTest::newRow("a text of one sign") << text(u"x"_s) << QString();
        QTest::newRow("a text of two thousand signs") << text(wideSigns(2000)) << QString();
        QTest::newRow("a text of two thousand and one signs") << text(wideSigns(2001)) << noText;
        QTest::newRow("a text of two thousand and one letters") << text(QString(2001, u'x')) << noText;
        QTest::newRow("a text with nothing in it") << text(QString()) << noText;
        QTest::newRow("a number as its text") << text(7) << noText;
        QTest::newRow("a text without its text")
            << QJsonValue(QJsonObject{{u"name"_s, u"Type"_s}, {u"kind"_s, u"text"_s}}) << noText;
    }

    void whatIsNoSavedActionIsLeftOut()
    {
        QFETCH(QJsonValue, entry);
        QFETCH(QString, why);
        QCOMPARE(leftOutOf(entry, u"actions"_s), why);
        const Content content = contentOf({{u"actions"_s, QJsonArray{entry}}});
        QCOMPARE(actionsIn(content).size(), why.isEmpty() ? qsizetype{1} : qsizetype{0});
    }

    void moreSavedActionsThanAFileHoldsAreNotReadToTheirEnd()
    {
        QJsonArray many;
        for (int number = 0; number < 503; ++number) {
            many.append(actionWith({{u"name"_s, u"Action %1"_s.arg(number)}}));
        }
        const Content content = contentOf({{u"actions"_s, many}});
        QCOMPARE(actionsIn(content).size(), qsizetype{500});
        QCOMPARE(actionsIn(content).last().name, u"Action 499"_s);
        QCOMPARE(content.leftOut, QStringList{u"More than 500 saved actions: the ones after that"_s});
        // Two of one name are two: whoever takes them in says what an action is here already.
        const Content twice = contentOf({{u"actions"_s, QJsonArray{actionWith({}), actionWith({})}}});
        QCOMPARE(actionsIn(twice).size(), qsizetype{2});
        QCOMPARE(twice.leftOut, QStringList());
        for (const QJsonValue& notAList : {QJsonValue(QJsonObject()), QJsonValue(u"x"_s), QJsonValue()}) {
            const Content none = contentOf({{u"actions"_s, notAList}});
            QVERIFY(none.actions.has_value() && actionsIn(none).isEmpty());
            QCOMPARE(none.leftOut, QStringList{u"The list of saved actions is not a list"_s});
        }
    }

    // --- the settings -------------------------------------------------------------------------------

    void alertSettingsAreReadWithWhatTheyAreWithoutASay()
    {
        const core::exchange::Alerts usual;
        QCOMPARE(usual.enabled, false);
        QCOMPARE(usual.format, u"auto"_s);
        QVERIFY(usual.offline && usual.noSignal && usual.problem && usual.recovered);
        QCOMPARE(contentOf({{u"alerts"_s, QJsonObject()}}).alerts, std::optional<core::exchange::Alerts>(usual));
        // What is neither yes nor no is what it is without a say; a link and a pause are a
        // wall's own, and not looked at.
        const Content odd = contentOf({{u"alerts"_s, QJsonObject{{u"enabled"_s, u"yes"_s},
                                                                 {u"offline"_s, 0},
                                                                 {u"no_signal"_s, QJsonValue()},
                                                                 {u"problem"_s, u"false"_s},
                                                                 {u"recovered"_s, QJsonArray()},
                                                                 {u"link"_s, u"not an address"_s},
                                                                 {u"paused_until"_s, 9e9}}}});
        QCOMPARE(odd.alerts, std::optional<core::exchange::Alerts>(usual));
        QCOMPARE(odd.leftOut, QStringList());
        core::exchange::Alerts said;
        said.enabled = true;
        said.offline = false;
        said.noSignal = false;
        said.problem = false;
        said.recovered = false;
        said.address = u"https://hooks.example/T0KEN"_s;
        const Content content =
            contentOf({{u"alerts"_s, QJsonObject{{u"enabled"_s, true},
                                                 {u"offline"_s, false},
                                                 {u"no_signal"_s, false},
                                                 {u"problem"_s, false},
                                                 {u"recovered"_s, false},
                                                 {u"webhook"_s, u" https://hooks.example/T0KEN "_s}}}});
        QCOMPARE(content.alerts, std::optional<core::exchange::Alerts>(said));
        QVERIFY(content.hasSecrets());
        // What is no settings at all is none, and said.
        for (const QJsonValue& notSettings :
             {QJsonValue(QJsonArray()), QJsonValue(7), QJsonValue(u"x"_s), QJsonValue()}) {
            const Content none = contentOf({{u"alerts"_s, notSettings}});
            QVERIFY(!none.alerts.has_value());
            QCOMPARE(none.leftOut, QStringList{u"The alert settings are not settings"_s});
        }
    }

    void whereAlertsGoIsAnAddressOfTheWebOrNone_data()
    {
        QTest::addColumn<QJsonValue>("written");
        QTest::addColumn<QString>("taken");
        QTest::addColumn<QString>("said"); // of an address that is not kept; nothing: no word is said
        // What is said is what is so. Where the server's rule says in so many words that it is
        // no web address, the server's sentence; where a server may keep it and this takes
        // less, that. (Of all of these the server's sentence was said, which was not so of the
        // second kind: a server's own file holds an address at a name in its "xn--" form.)
        const QString notOne = u"Alert settings: the address alerts go to is not a web address"_s;
        const QString atAnXnName = u"Alert settings: the address alerts go to is at a name in its xn-- form, which "
                                   "this program does not take from a file (a qKVM server may)"_s;
        const QString aServerMay = u"Alert settings: the address alerts go to is one this program does not take from "
                                   "a file (a qKVM server may)"_s;
        QTest::newRow("an address") << QJsonValue(u"https://hooks.example/T0KEN"_s) << u"https://hooks.example/T0KEN"_s
                                    << QString();
        QTest::newRow("a plain one at this PC")
            << QJsonValue(u"http://127.0.0.1:8099/hook"_s) << u"http://127.0.0.1:8099/hook"_s << QString();
        // Nothing there, in each of the ways the server counts as nothing: no word is said of it.
        QTest::newRow("a text with nothing in it") << QJsonValue(QString()) << QString() << QString();
        QTest::newRow("nothing, spelled out") << QJsonValue() << QString() << QString();
        QTest::newRow("a no") << QJsonValue(false) << QString() << QString();
        QTest::newRow("nought") << QJsonValue(0) << QString() << QString();
        QTest::newRow("a list with nothing in it") << QJsonValue(QJsonArray()) << QString() << QString();
        // Something there that is no address of the web: left out, and said.
        QTest::newRow("a file") << QJsonValue(u"file:///etc/passwd"_s) << QString() << notOne;
        QTest::newRow("a script") << QJsonValue(u"javascript:alert(1)"_s) << QString() << notOne;
        QTest::newRow("no host") << QJsonValue(u"https:///hook"_s) << QString() << notOne;
        QTest::newRow("words") << QJsonValue(u"not an address"_s) << QString() << notOne;
        QTest::newRow("two lines") << QJsonValue(u"https://hooks.example/a\nb"_s) << QString() << notOne;
        // (A server keeps this one where its library reads the host as a name, and so it keeps
        // one with the sign for Kelvin in its host: here no host in other letters is kept.)
        QTest::newRow("a letter that is not of the twenty-six in its host")
            << QJsonValue(u"https://h"_s + QChar(0xf6) + u"oks.example/T0KEN"_s) << QString() << aServerMay;
        QTest::newRow("a blank in its host") << QJsonValue(u"https://hooks example/T0KEN"_s) << QString() << notOne;
        QTest::newRow("a number") << QJsonValue(7) << QString() << notOne;
        QTest::newRow("a yes") << QJsonValue(true) << QString() << notOne;
        QTest::newRow("something sealed, as a wall keeps it")
            << QJsonValue(QJsonObject{{u"nonce"_s, u"x"_s}, {u"data"_s, u"y"_s}}) << QString() << notOne;
        QTest::newRow("longer than any address")
            << QJsonValue(u"https://hooks.example/"_s + QString(2000, u'x')) << QString() << notOne;

        // The server's rule (check_url in its alerts.py), row by row as its own reader was asked
        // (make_export_odd.py): what it takes is taken as it is written, and nothing else is.
        const auto kept = [](const char* row, const QString& address) {
            QTest::newRow(row) << QJsonValue(address) << address << QString();
        };
        const auto notKept = [&notOne](const char* row, const QString& address) {
            QTest::newRow(row) << QJsonValue(address) << QString() << notOne;
        };
        const auto notKeptHere = [&aServerMay](const char* row, const QString& address) {
            QTest::newRow(row) << QJsonValue(address) << QString() << aServerMay;
        };
        const QString hook = u"https://hooks.example/"_s;
        kept("500 signs", hook + QString(478, u'x'));
        notKept("501 signs", hook + QString(479, u'x'));
        // (Counted in signs, as the server counts: one that takes two places here is one.)
        kept("500 signs that take two places each", hook + wideSigns(478));
        notKept("501 signs that take two places each", hook + wideSigns(479));
        QTest::newRow("500 signs, and blanks around them")
            << QJsonValue(u"  "_s + hook + QString(478, u'x') + u"\n"_s) << hook + QString(478, u'x') << QString();
        notKept("a blank behind its host", hook + u"a b"_s);
        notKept("a blank that breaks no line behind its host", hook + u"a"_s + QChar(0xa0) + u"b"_s);
        notKept("a blank of full width behind its host", hook + u"a"_s + QChar(0x3000) + u"b"_s);
        notKept("a tab behind its host", hook + u"a\tb"_s);
        notKept("a sign below 32 behind its host", hook + u"a"_s + QChar(0x1f) + u"b"_s);
        notKept("the sign 127 behind its host", hook + u"a"_s + QChar(0x7f) + u"b"_s);
        notKept("numbers no address has", u"https://999.1.1.1/h"_s);
        notKept("numbers with a nought in front", u"https://192.168.01.9/h"_s);
        kept("four numbers", u"https://192.168.1.9/h"_s);
        kept("three numbers, which are a name", u"https://1.2.3/h"_s);
        kept("five numbers, which are a name", u"https://1.2.3.4.5/h"_s);
        kept("plain", u"http://hooks.example/h"_s);
        notKept("another way of speaking", u"ftp://hooks.example/h"_s);
        kept("in capitals", u"HTTPS://Hooks.Example/T0KEN"_s);
        // The long s counts as an "s" where Qt compares without regard to small and large. To
        // the server "http", a long s and "://" is no way of speaking. (These two rows fail
        // where the way of speaking is compared by Qt's folding: tried,
        // build/mine/x24_folding_test_core_exchange.log.)
        notKept("a long s where the s of https stands", u"http"_s + QChar(0x17f) + u"://hooks.example/h"_s);
        notKept("a long s where the s of HTTPS stands", u"HTTP"_s + QChar(0x17f) + u"://hooks.example/h"_s);
        kept("a name and a password in it", u"https://user:made-up@hooks.example/h"_s);
        kept("a port", u"https://hooks.example:8443/h"_s);
        kept("a port no computer has", u"https://hooks.example:99999/h"_s);
        kept("a colon and no port", u"https://hooks.example:/h"_s);
        notKept("no host before its port", u"https://:8443/h"_s);
        notKept("no host behind a name", u"https://user@/h"_s);
        notKept("a bracket that is alone", u"http://[2001:db8::7/hook"_s);
        kept("signs no name has in its host", u"https://ho<o>ks_1%zz.example/h"_s);
        kept("numbers of the longer kind", u"http://[2001:db8::7]:8099/hook"_s);
        kept("a part of its name in punycode, which the server asks nobody about",
             u"https://www.xn--mnchen-3ya.example/h"_s);
        kept("signs beyond ASCII behind its host", hook + u"t"_s + QChar(0xfc) + u"r/"_s + wideSigns(1));
        // Stricter than the server, where it asks a library that is not here: a server may
        // keep each of these, and that is what is said. First the one a server's own file holds.
        QTest::newRow("a name in its xn-- form")
            << QJsonValue(u"https://xn--mnchen-3ya.example/h"_s) << QString() << atAnXnName;
        QTest::newRow("a name in its xn-- form, in capitals")
            << QJsonValue(u"https://XN--mnchen-3ya.example/h"_s) << QString() << atAnXnName;
        QTest::newRow("a name in its xn-- form, with a name and a password before it and a port behind it")
            << QJsonValue(u"https://user:made-up@xn--mnchen-3ya.example:8443/h"_s) << QString() << atAnXnName;
        notKeptHere("a port that is none", u"https://hooks.example:port/h"_s);
        notKeptHere("a plus before its port", u"https://hooks.example:+8443/h"_s);
        notKeptHere("an underscore in its port", u"https://hooks.example:8_443/h"_s);
        notKeptHere("a name in letters beyond ASCII before the at",
                    u"https://us"_s + QChar(0xe9) + u"r@hooks.example/h"_s);
        notKeptHere("something behind the brackets", u"http://[2001:db8::7]x/hook"_s);
        notKeptHere("brackets in the middle of a name", u"http://ho[o]ks.example/hook"_s);
        notKeptHere("brackets around what is no address", u"http://[hooks]/hook"_s);
        notKeptHere("the other thing brackets may hold", u"http://[v1.hooks]/hook"_s);
        notKeptHere("numbers of the longer kind with a zone", u"http://[fe80::1%25eth0]/hook"_s);
        // And where the server's sender of alerts had another rule for years than it has now:
        // digits with any three signs among them looked like four numbers to it.
        notKeptHere("digits and hyphens that look like four numbers", u"https://192-168-1-9/h"_s);
        notKeptHere("seven digits", u"https://1234567/h"_s);
        kept("five digits, which never looked like four numbers", u"https://12345/h"_s);
        // And what turns the writing round behind its host, which the server takes.
        notKeptHere("a turn of direction behind its host", hook + QChar(0x202e) + u"gpj.exe"_s);
    }

    void whereAlertsGoIsAnAddressOfTheWebOrNone()
    {
        QFETCH(QJsonValue, written);
        QFETCH(QString, taken);
        QFETCH(QString, said);
        const Content content = contentOf({{u"alerts"_s, QJsonObject{{u"webhook"_s, written}}}});
        QVERIFY(content.alerts.has_value());
        QCOMPARE(alertsIn(content).address, taken);
        QCOMPARE(content.leftOut, said.isEmpty() ? QStringList() : QStringList{said});
    }

    void howAlertsAreWrittenIsAWordOfTheServersListOrAuto()
    {
        // The server keeps a word of its own list (FORMATS in its alerts.py) and makes "auto" of
        // any other ("carrier-pigeon", in its own tests). So does this: a wall whose alerts go to
        // a service that wants them in its own form has them go out in that form after the file.
        const auto formatOf = [](const QJsonValue& written) {
            return alertsIn(contentOf({{u"alerts"_s, QJsonObject{{u"format"_s, written}}}})).format;
        };
        for (const QString& word : {u"auto"_s, u"json"_s, u"ntfy"_s, u"slack"_s, u"discord"_s}) {
            QCOMPARE(formatOf(word), word);
        }
        // Anything else is "auto": a word in other letters, a word of no list, one with
        // something around it, and what is no word.
        QCOMPARE(formatOf(u"NTFY"_s), u"auto"_s);
        QCOMPARE(formatOf(u"teams"_s), u"auto"_s);
        QCOMPARE(formatOf(u"carrier-pigeon"_s), u"auto"_s);
        QCOMPARE(formatOf(u"Carrier Pigeon"_s), u"auto"_s);
        QCOMPARE(formatOf(u"json\n"_s), u"auto"_s);
        QCOMPARE(formatOf(u" ntfy"_s), u"auto"_s);
        QCOMPARE(formatOf(QString()), u"auto"_s);
        QCOMPARE(formatOf(7), u"auto"_s);
        QCOMPARE(formatOf(true), u"auto"_s);
        QCOMPARE(formatOf(QJsonValue()), u"auto"_s);
        QCOMPARE(formatOf(QJsonArray{u"json"_s}), u"auto"_s);
        // And nothing is said of it, as the server says nothing.
        QCOMPARE(contentOf({{u"alerts"_s, QJsonObject{{u"format"_s, u"carrier-pigeon"_s}}}}).leftOut, QStringList());
        QCOMPARE(contentOf({{u"alerts"_s, QJsonObject{{u"format"_s, u"ntfy"_s}}}}).leftOut, QStringList());
    }

    void theWallsSettingsAreReadWhereTheyAreWhatAWallAllows_data()
    {
        QTest::addColumn<QJsonValue>("interval");
        QTest::addColumn<bool>("taken");
        QTest::newRow("a fifth of a second") << QJsonValue(0.2) << true;
        QTest::newRow("a minute") << QJsonValue(60) << true;
        QTest::newRow("a second, written whole") << QJsonValue(1) << true;
        QTest::newRow("two and a half seconds") << QJsonValue(2.5) << true;
        QTest::newRow("less than a fifth of a second") << QJsonValue(0.19) << false;
        QTest::newRow("a thousandth of a second") << QJsonValue(0.001) << false;
        QTest::newRow("more than a minute") << QJsonValue(60.5) << false;
        QTest::newRow("nought") << QJsonValue(0) << false;
        QTest::newRow("less than nothing") << QJsonValue(-1) << false;
        QTest::newRow("a number in quotes") << QJsonValue(u"1"_s) << false;
        QTest::newRow("a yes") << QJsonValue(true) << false;
        QTest::newRow("nothing, spelled out") << QJsonValue() << false;
    }

    void theWallsSettingsAreReadWhereTheyAreWhatAWallAllows()
    {
        QFETCH(QJsonValue, interval);
        QFETCH(bool, taken);
        const Content content = contentOf({{u"wall"_s, QJsonObject{{u"interval"_s, interval}}}});
        QVERIFY(content.wall.has_value());
        QCOMPARE(wallIn(content).pictureEverySeconds,
                 taken ? std::optional<double>(interval.toDouble()) : std::optional<double>());
        QCOMPARE(
            content.leftOut,
            taken ? QStringList()
                  : QStringList{u"Wall settings: how often pictures are fetched is not a time this version allows"_s});
    }

    void ofAWallOnlyItsPaceAndItsAskingForNewerVersionsAreTaken()
    {
        const Content content = contentOf({{u"wall"_s, QJsonObject{{u"interval"_s, 2},
                                                                   {u"update_check"_s, false},
                                                                   {u"bind"_s, QJsonArray{u"all"_s}},
                                                                   {u"port"_s, 1},
                                                                   {u"allowed_hosts"_s, QJsonArray{u"evil.example"_s}},
                                                                   {u"require_totp"_s, QString()}}},
                                           {u"users"_s, QJsonArray{QJsonObject{{u"name"_s, u"intruder"_s}}}},
                                           {u"tokens"_s, QJsonArray{QJsonObject{{u"name"_s, u"x"_s}}}},
                                           {u"server"_s, QJsonObject{{u"port"_s, 1}}},
                                           {u"recovery"_s, QJsonObject()},
                                           {u"unlock"_s, QJsonObject()}});
        core::exchange::WallSettings wall;
        wall.pictureEverySeconds = 2.0;
        wall.asksForNewerVersions = false;
        QCOMPARE(content.wall, std::optional<core::exchange::WallSettings>(wall));
        QCOMPARE(content.leftOut, QStringList());
        QVERIFY(!content.devices && !content.actions && !content.alerts);
        // What is neither yes nor no says nothing of newer versions, and nothing is said of it.
        const Content odd = contentOf({{u"wall"_s, QJsonObject{{u"update_check"_s, u"no"_s}}}});
        QCOMPARE(odd.wall, std::optional<core::exchange::WallSettings>(core::exchange::WallSettings()));
        QCOMPARE(odd.leftOut, QStringList());
        for (const QJsonValue& notSettings : {QJsonValue(QJsonArray()), QJsonValue(7), QJsonValue()}) {
            const Content none = contentOf({{u"wall"_s, notSettings}});
            QVERIFY(!none.wall.has_value());
            QCOMPARE(none.leftOut, QStringList{u"The wall's settings are not settings"_s});
        }
    }

    void whenAFileWasMadeAndByWhatIsReadWhereItSaysSo()
    {
        const auto madeOf = [](const QJsonValue& made) { return contentOf({{u"made"_s, made}}).made; };
        QCOMPARE(madeOf(1791832242), 1791832242.0);
        QCOMPARE(madeOf(1791832242.5), 1791832242.5);
        QCOMPARE(madeOf(0), 0.0);
        QCOMPARE(madeOf(99999999999.0), 99999999999.0);
        QCOMPARE(madeOf(1e11), 0.0);
        QCOMPARE(madeOf(-1), 0.0);
        QCOMPARE(madeOf(u"1791832242"_s), 0.0);
        QCOMPARE(madeOf(true), 0.0);
        const auto programOf = [](const QJsonValue& program) { return contentOf({{u"program"_s, program}}).program; };
        QCOMPARE(programOf(u" 0.1.0 "_s), u"0.1.0"_s);
        QCOMPARE(programOf(wideSigns(20)), wideSigns(20));
        QCOMPARE(programOf(wideSigns(21)), QString());
        QCOMPARE(programOf(u"0.1\n.0"_s), QString());
        QCOMPARE(programOf(7), QString());
    }

    void whatWasLeftOutIsSaidInTheOrderOfTheFilesParts()
    {
        // Whatever order they stand in: devices, saved actions, alerts, the wall.
        const QByteArray file = R"({"wall": 7, "alerts": [], "actions": [1], "devices": [1],)"
                                R"( "version": 1, "format": "qkvm-export"})";
        const base::Result<Content> read = readPlain(file);
        QCOMPARE(saidOf(read), u"read"_s);
        QCOMPARE(read.value().leftOut,
                 (QStringList{u"Device 1: not an entry"_s, u"Saved action 1: not an entry"_s,
                              u"The alert settings are not settings"_s, u"The wall's settings are not settings"_s}));
    }
};

QTEST_GUILESS_MAIN(TestExchange)
#include "test_exchange.moc"
