#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QThread>

#include <qkvm/core/exchange.h>
#include <qkvm/core/exchange_work.h>
#include <qkvm/core/strict_json.h>

#include "exchange_by_hand.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using namespace qkvm::core::testing;
using core::SecretBytes;
using core::exchange::Action;
using core::exchange::Content;
using core::exchange::Device;
using core::exchange::LinkAsWritten;
using core::exchange::Sealing;
using core::exchange::Written;

namespace {

// Sealing at a cost that a test can pay, with the work watched.
Sealing quickly(WatchedWork& work)
{
    return Sealing{quickCost, work.derive()};
}

// What the writer hands back: the file, and what of the wall is not in it.
base::Result<Written> writtenWith(const Content& content, const QString& passphrase, WatchedWork& work)
{
    return core::exchange::writeWith(content, SecretBytes::utf8Of(passphrase), quickly(work));
}

// The file's text alone, for a test that is about the text. (What is not in the file is looked
// at by the tests that are about that; and that nobody gets the text without a word of it, by
// whoeverTakesAFilesTextHasToSayThatWhatIsNotInItWasLookedAt.)
base::Result<QByteArray> written(const Content& content, const QString& passphrase, WatchedWork& work)
{
    const base::Result<Written> whole = writtenWith(content, passphrase, work);
    if (!whole) {
        return whole.error();
    }
    return textOf(whole.value());
}

// Whether a type hands out a file's text to whoever asks for it and says nothing: as a member
// that is there to be read, or by a function that takes nothing.
template <typename File>
concept HasItsTextAsAMember = requires { requires std::is_member_object_pointer_v<decltype(&File::file)>; };
template <typename File>
concept HandsOutItsTextUnasked = requires(const File& file) { QByteArray(file.file()); };
template <typename File>
concept HandsOutItsTextWithoutAWord = HasItsTextAsAMember<File> || HandsOutItsTextUnasked<File>;
template <typename File>
concept HandsOutItsTextForTheWord =
    requires(const File& file) { QByteArray(file.file(core::exchange::NotInIt::wasLookedAt)); };
// Whether the word can be said without saying it: by empty braces, which make the first word
// of a list of words (as "it was looked at" was one), or by a number that is made one.
template <typename File>
concept HandsOutItsTextForEmptyBraces = requires(const File& file) { QByteArray(file.file({})); };
template <typename Word>
concept IsSaidByEmptyBraces = requires { Word{}; };
template <typename Word>
concept IsMadeOfANumber = requires { static_cast<Word>(0); } || requires { Word{0}; };

// The certificate of the server's example files, read out of the plain one by Qt.
QString theCertificate()
{
    const QJsonObject file = QJsonDocument::fromJson(exampleFile("plain.json")).object();
    return file[u"devices"_s].toArray().first().toObject()[u"cert_pem"_s].toString();
}

// Its bytes, by Qt's own reading of base64.
QByteArray theCertificatesBytes()
{
    return QByteArray::fromBase64(QString(theCertificate())
                                      .remove(u"-----BEGIN CERTIFICATE-----"_s)
                                      .remove(u"-----END CERTIFICATE-----"_s)
                                      .toLatin1());
}

Device deviceAt(const QString& address)
{
    Device device;
    device.name = address.mid(address.indexOf(u"://"_s) + 3);
    device.address = address;
    device.user = u"admin"_s;
    device.kind = u"kvmd"_s;
    device.tls = u"pinned"_s;
    return device;
}

// A wall with one of everything in it, and with the signs a file has to spell out: quotes and
// backslashes, ends of lines, a letter that is not of the twenty-six, a sign of two places.
Content aWall()
{
    Content content;
    content.sealed = true;
    content.made = 1791832242;
    content.program = u"0.1.0"_s;
    Device first = deviceAt(u"https://192.168.1.21"_s);
    first.name = u"Rack 1, t"_s + QChar(0xfc) + u"r "_s + wideSigns(1);
    first.password = u" a \"password\" with a \\, a tab\t, two\nlines, "_s + QChar(0xe4) + wideSigns(2) + QChar(0x7f);
    first.group = u"Rack 1"_s;
    first.certificatePem = theCertificate();
    first.certificate = QCryptographicHash::hash(QByteArray::fromBase64(QString(first.certificatePem)
                                                                            .remove(u"-----BEGIN CERTIFICATE-----"_s)
                                                                            .remove(u"-----END CERTIFICATE-----"_s)
                                                                            .toLatin1()),
                                                 QCryptographicHash::Sha256);
    Device second = deviceAt(u"http://192.168.1.22:8080"_s);
    second.name = u"Rack 1, plain"_s;
    second.user = u"operator"_s;
    second.password = u"made-up-password-2"_s;
    second.consoleLinkAsWritten = LinkAsWritten(u"https://192.168.1.22/kvm/"_s);
    second.alerts = false;
    Device third = deviceAt(u"https://kvm.example.net"_s);
    third.tls = u"system"_s;
    Device fourth = deviceAt(u"http://192.168.1.30"_s);
    fourth.kind = u"jetkvm"_s;
    fourth.ident = u"jetkvm:0123456789abcdef"_s;
    Device fifth = deviceAt(u"https://[2001:db8::7]:8443"_s);
    fifth.user = u"root"_s;
    fifth.password = u"made-up-password-5"_s;
    fifth.kind = u"redfish"_s;
    fifth.tls = u"insecure"_s;
    fifth.keepBoth = true;
    // (A server's: the numbers its computer and its controller gave for themselves, M7 1b-ii.)
    fifth.machine = u"server:"_s + QString(32, u'a') + QString(32, u'b');
    content.devices = QList<Device>{first, second, third, fourth, fifth};
    Action keys;
    keys.name = u"Ctrl+Alt+Del"_s;
    keys.keys = u"ControlLeft,AltLeft,Delete"_s;
    keys.danger = true;
    Action text;
    text.name = u"Sign in"_s;
    text.types = true;
    text.text = u"made-up text to type\r\nin two lines "_s + QChar(0);
    text.enter = true;
    content.actions = QList<Action>{keys, text};
    core::exchange::Alerts alerts;
    alerts.enabled = true;
    // (Written for one service, and not left to the server: a choice a file has to carry.)
    alerts.format = u"slack"_s;
    alerts.recovered = false;
    alerts.address = u"https://hooks.example.net/made-up-token"_s;
    content.alerts = alerts;
    core::exchange::WallSettings wall;
    wall.pictureEverySeconds = 2.5;
    wall.asksForNewerVersions = false;
    content.wall = wall;
    return content;
}

// The same wall as a plain file hands it back: with no secret in it.
Content withoutSecrets(Content content)
{
    content.sealed = false;
    QList<Device> devices = devicesIn(content);
    for (Device& device : devices) {
        device.password.clear();
    }
    content.devices = devices;
    QList<Action> actions = actionsIn(content);
    actions.removeIf([](const Action& action) { return action.types; });
    content.actions = actions;
    core::exchange::Alerts alerts = alertsIn(content);
    alerts.address.clear();
    content.alerts = alerts;
    return content;
}

// The lines the writer hands back beside a file (Written::notInIt), each of one device of the
// wall: by its place there, and by its name in quotes where it has one that can be shown.
QString ofTheWall(int place, const QString& name)
{
    return name.isEmpty() ? u"Device %1 of this wall"_s.arg(place)
                          : u"Device %1 of this wall (\"%2\")"_s.arg(QString::number(place), name);
}

QString atANameInItsXnForm(const QString& which)
{
    return which + u": its address is a name with a part in its xn-- form. This program speaks to it and reads it "
                   "back from this file; a qKVM server may not take a name in its xn-- form: read the file there and "
                   "look."_s;
}

QString atANameThatReadsAsNumbers(const QString& which)
{
    return which + u": its address is a name of digits with single signs between them, which a qKVM server may read "
                   "as numbers that are no address, and not take: read the file there and look."_s;
}

QString withALinkThisReaderLeavesItOutFor(const QString& which)
{
    return which + u": its console link is written as it is. Reading this file, this program leaves this device out "
                   "for it, and a qKVM server may too: read the file there and look."_s;
}

QString knownByAFingerprintAlone(const QString& which)
{
    return which + u": only the fingerprint of its certificate is known here, and a file holds a certificate itself "
                   "or none. A qKVM server that reads this file will take the first certificate this device shows."_s;
}

// A sign spelled out by its number, as a file spells one: put together here, and written
// nowhere in this file as it stands in a file.
QByteArray spelled(uint unit)
{
    return QByteArray(1, '\\') + 'u' + QByteArray::number(unit, 16).rightJustified(4, '0');
}

} // namespace

// The program's own writer of a wall as a file, and the doing of both off the interface thread:
// what is written is read back whole, is written as the server writes one, and nothing is written
// that would not be read back.
class TestExchangeWritten : public QObject {
    Q_OBJECT

private slots:
    // --- writing ------------------------------------------------------------------------------------

    void whatIsWrittenSealedIsReadBackWhole()
    {
        WatchedWork work;
        const Content content = aWall();
        const base::Result<QByteArray> file = written(content, thePassphrase(), work);
        QCOMPARE(saidOf(file), u"read"_s);
        QCOMPARE(work.asked(), 1);
        const base::Result<Content> read = core::exchange::read(file.value(), thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        QCOMPARE(read.value().devices, content.devices);
        QCOMPARE(read.value().actions, content.actions);
        QCOMPARE(read.value().alerts, content.alerts);
        QCOMPARE(read.value().wall, content.wall);
        QCOMPARE(read.value().leftOut, QStringList());
        QVERIFY(read.value() == content);
        // And with another passphrase it is not read.
        QCOMPARE(saidOf(core::exchange::read(file.value(), u"another passphrase"_s)),
                 u"wrong_passphrase: That is not the passphrase this file was made with"_s);
        // Nothing of the wall can be read off the file: not a password, and not a name or an
        // address either.
        for (const char* word :
             {"192.168.1.", "Rack 1", "password", "made-up", "hooks.example", "CERTIFICATE", "redfish"}) {
            QVERIFY2(!file.value().contains(word), word);
        }
    }

    void aFileIsSealedAsTheServerSealsOne()
    {
        // With the work watched and not done: what the program writes a file with is the full
        // cost, and this test looks only at what it is written as.
        WatchedWork work;
        const base::Result<Written> whole = core::exchange::writeWith(
            aWall(), SecretBytes::utf8Of(thePassphrase()), Sealing{core::exchange::costAsWritten, work.derive()});
        QCOMPARE(saidOf(whole), u"read"_s);
        const base::Result<QByteArray> file = textOf(whole.value());
        QCOMPARE(work.asked(), 1);
        QCOMPARE(work.lastCost(), (core::scrypt::Cost{131072, 8, 1}));
        // What the program seals with where nothing else is said is that cost.
        QCOMPARE(Sealing().cost, (core::scrypt::Cost{131072, 8, 1}));
        const QJsonObject said = QJsonDocument::fromJson(file.value()).object();
        QCOMPARE(said.keys(), (QStringList{u"format"_s, u"lock"_s, u"sealed"_s, u"version"_s}));
        QCOMPARE(said[u"format"_s].toString(), u"qkvm-export"_s);
        QCOMPARE(said[u"version"_s].toInt(), 1);
        const QJsonObject lock = said[u"lock"_s].toObject();
        QCOMPARE(lock.keys(), (QStringList{u"kdf"_s, u"key"_s}));
        const QJsonObject kdf = lock[u"kdf"_s].toObject();
        QCOMPARE(kdf.keys(), (QStringList{u"n"_s, u"name"_s, u"p"_s, u"r"_s, u"salt"_s}));
        QCOMPARE(kdf[u"name"_s].toString(), u"scrypt"_s);
        QCOMPARE(kdf[u"n"_s].toInt(), 131072);
        QCOMPARE(kdf[u"r"_s].toInt(), 8);
        QCOMPARE(kdf[u"p"_s].toInt(), 1);
        const auto bytesOf = [](const QJsonValue& value) {
            return QByteArray::fromBase64(value.toString().toLatin1());
        };
        // A salt of sixteen bytes, nonces of twelve, and the file's own key of 32 with its tag.
        QCOMPARE(bytesOf(kdf[u"salt"_s]).size(), qsizetype{16});
        QCOMPARE(lock[u"key"_s].toObject().keys(), (QStringList{u"data"_s, u"nonce"_s}));
        QCOMPARE(bytesOf(lock[u"key"_s].toObject()[u"nonce"_s]).size(), qsizetype{12});
        QCOMPARE(bytesOf(lock[u"key"_s].toObject()[u"data"_s]).size(), qsizetype{48});
        QCOMPARE(said[u"sealed"_s].toObject().keys(), (QStringList{u"data"_s, u"nonce"_s}));
        QCOMPARE(bytesOf(said[u"sealed"_s].toObject()[u"nonce"_s]).size(), qsizetype{12});
        QVERIFY(bytesOf(said[u"sealed"_s].toObject()[u"data"_s]).size() > 1000);
        // Laid out as the server lays one out: spread over lines, two blanks deeper each time,
        // in the server's order, and with a line's end behind it.
        QVERIFY2(file.value().startsWith("{\n  \"format\": \"qkvm-export\",\n  \"version\": 1,\n  \"lock\": {\n"
                                         "    \"kdf\": {\n      \"name\": \"scrypt\",\n      \"n\": 131072,\n"
                                         "      \"r\": 8,\n      \"p\": 1,\n      \"salt\": \""),
                 file.value().left(200).constData());
        QVERIFY(file.value().endsWith("\"\n  }\n}\n"));
        QVERIFY(file.value().contains("\n    },\n    \"key\": {\n      \"nonce\": \""));
        QVERIFY(file.value().contains("\n    }\n  },\n  \"sealed\": {\n    \"nonce\": \""));
    }

    void everySealingIsNew()
    {
        WatchedWork work;
        const Content content = aWall();
        const QJsonObject one = QJsonDocument::fromJson(written(content, thePassphrase(), work).value()).object();
        const QJsonObject other = QJsonDocument::fromJson(written(content, thePassphrase(), work).value()).object();
        const auto in = [](const QJsonObject& file, const char* part, const char* field) {
            const QJsonObject lock = file[u"lock"_s].toObject();
            const QLatin1StringView where(part);
            const QJsonObject whole = where == "kdf"_L1   ? lock[u"kdf"_s].toObject()
                                      : where == "key"_L1 ? lock[u"key"_s].toObject()
                                                          : file[u"sealed"_s].toObject();
            return whole[QLatin1StringView(field)].toString();
        };
        for (const auto& [part, field] : {std::pair{"kdf", "salt"}, std::pair{"key", "nonce"}, std::pair{"key", "data"},
                                          std::pair{"sealed", "nonce"}, std::pair{"sealed", "data"}}) {
            QVERIFY2(!in(one, part, field).isEmpty() && in(one, part, field) != in(other, part, field), field);
        }
        // And within one file the two nonces are two.
        QVERIFY(in(one, "key", "nonce") != in(one, "sealed", "nonce"));
        // The file's own key is another each time too: opened here with the passphrase's key,
        // made by hand, it is 32 bytes that the other file does not have.
        const auto fileKeyOf = [&in](const QJsonObject& file) {
            const auto bytesOf = [&in, &file](const char* part, const char* field) {
                return QByteArray::fromBase64(in(file, part, field).toLatin1());
            };
            QByteArray derived(32, '\0');
            const base::Result<void> made =
                core::scrypt::derive(thePassphrase().toUtf8(), bytesOf("kdf", "salt"), quickCost,
                                     std::span<quint8>(reinterpret_cast<quint8*>(derived.data()), 32));
            const base::Result<SecretBytes> opened = core::sealing::open(keyFrom(derived), bytesOf("key", "nonce"),
                                                                         bytesOf("key", "data"), "kvm-wall data key");
            return made && opened ? opened.value().view().toByteArray() : QByteArray();
        };
        const QByteArray firstKey = fileKeyOf(one);
        QCOMPARE(firstKey.size(), qsizetype{32});
        QVERIFY(firstKey != QByteArray(32, '\0'));
        QVERIFY(firstKey != fileKeyOf(other));
    }

    void aPlainFileIsWrittenAsTheServerWritesOne()
    {
        // Sign for sign what the server's writer makes of the same wall (its language's own
        // writer of JSON, spread over lines).
        Content content;
        content.made = 1791832242;
        content.program = u"0.1.0"_s;
        Device device = deviceAt(u"https://192.168.1.21"_s);
        device.name = u"Rack 1, top"_s;
        device.password = u"never written"_s;
        device.group = u"Rack 1"_s;
        Device second = deviceAt(u"http://192.168.1.22:8080"_s);
        second.keepBoth = true;
        second.alerts = false;
        content.devices = QList<Device>{device, second};
        content.actions = QList<Action>();
        core::exchange::WallSettings wall;
        wall.pictureEverySeconds = 1.0;
        wall.asksForNewerVersions = true;
        content.wall = wall;
        WatchedWork work;
        const base::Result<QByteArray> file = written(content, QString(), work);
        QCOMPARE(saidOf(file), u"read"_s);
        QCOMPARE(work.asked(), 0);
        const QByteArray expected = "{\n"
                                    "  \"format\": \"qkvm-export\",\n"
                                    "  \"version\": 1,\n"
                                    "  \"made\": 1791832242,\n"
                                    "  \"program\": \"0.1.0\",\n"
                                    "  \"devices\": [\n"
                                    "    {\n"
                                    "      \"name\": \"Rack 1, top\",\n"
                                    "      \"url\": \"https://192.168.1.21\",\n"
                                    "      \"username\": \"admin\",\n"
                                    "      \"kind\": \"kvmd\",\n"
                                    "      \"group\": \"Rack 1\",\n"
                                    "      \"tls\": \"pinned\",\n"
                                    "      \"cert_pem\": null,\n"
                                    "      \"open_url\": \"\",\n"
                                    "      \"alerts\": true,\n"
                                    "      \"ident\": \"\",\n"
                                    "      \"machine\": \"\"\n"
                                    "    },\n"
                                    "    {\n"
                                    "      \"name\": \"192.168.1.22:8080\",\n"
                                    "      \"url\": \"http://192.168.1.22:8080\",\n"
                                    "      \"username\": \"admin\",\n"
                                    "      \"kind\": \"kvmd\",\n"
                                    "      \"group\": \"\",\n"
                                    "      \"tls\": \"pinned\",\n"
                                    "      \"cert_pem\": null,\n"
                                    "      \"open_url\": \"\",\n"
                                    "      \"alerts\": false,\n"
                                    "      \"ident\": \"\",\n"
                                    "      \"keep_both\": true,\n"
                                    "      \"machine\": \"\"\n"
                                    "    }\n"
                                    "  ],\n"
                                    "  \"actions\": [],\n"
                                    "  \"wall\": {\n"
                                    "    \"interval\": 1.0,\n"
                                    "    \"update_check\": true\n"
                                    "  }\n"
                                    "}\n";
        QCOMPARE(QString::fromLatin1(file.value()), QString::fromLatin1(expected));
    }

    void aPlainFileHoldsNoSecretAndIsReadWithoutAPassphrase()
    {
        WatchedWork work;
        const Content content = aWall();
        const base::Result<QByteArray> file = written(content, QString(), work);
        QCOMPARE(saidOf(file), u"read"_s);
        // No passphrase, no work.
        QCOMPARE(work.asked(), 0);
        // Not a secret, and not even a place for one; a saved text without its text is left out.
        for (const char* word :
             {"password", "webhook", "Sign in", "made-up text", "made-up-password", "hooks.example"}) {
            QVERIFY2(!file.value().contains(word), word);
        }
        // Readable, and meant to be.
        QVERIFY(file.value().contains("\"name\": \"Rack 1, plain\""));
        QVERIFY(!core::exchange::isSealed(file.value()).value());
        const base::Result<Content> read = core::exchange::read(file.value(), QString());
        QCOMPARE(saidOf(read), u"read"_s);
        const Content expected = withoutSecrets(content);
        QCOMPARE(read.value().devices, expected.devices);
        QCOMPARE(read.value().actions, expected.actions);
        QCOMPARE(read.value().alerts, expected.alerts);
        QVERIFY(read.value() == expected);
        QVERIFY(!read.value().hasSecrets());
    }

    void howAlertsAreWrittenIsWrittenAndReadBackAndNoWordIsWrittenThatTheServerDoesNotHave()
    {
        // Each word of the server's list is written as it is and read back as it is, plain and
        // sealed: a wall whose alerts go to a service in that service's own form keeps that.
        WatchedWork work;
        Content content;
        content.program = u"0.1.0"_s;
        core::exchange::Alerts alerts;
        for (const char* word : {"auto", "json", "ntfy", "slack", "discord"}) {
            alerts.format = QString::fromLatin1(word);
            content.alerts = alerts;
            const base::Result<QByteArray> plain = written(content, QString(), work);
            QCOMPARE(saidOf(plain), u"read"_s);
            QVERIFY2(plain.value().contains(QByteArray("\"format\": \"") + word + "\""), plain.value().constData());
            QCOMPARE(alertsIn(core::exchange::read(plain.value(), QString()).value()).format, alerts.format);
            const base::Result<QByteArray> sealed = written(content, thePassphrase(), work);
            QCOMPARE(saidOf(sealed), u"read"_s);
            QCOMPARE(alertsIn(core::exchange::read(sealed.value(), thePassphrase()).value()).format, alerts.format);
        }
        // Any other word would be read back as "auto", by the server and here, and nobody would
        // have been told: it is not written.
        const int asked = work.asked();
        for (const char* word : {"NTFY", "teams", "", " ntfy", "carrier-pigeon"}) {
            alerts.format = QString::fromLatin1(word);
            content.alerts = alerts;
            const QString refused = u"desktop:not_written: Nothing was written: what this wall has set for alerts "
                                    "cannot be put into a file of qKVM's: how alerts are written is not said in a way "
                                    "qKVM knows"_s;
            QCOMPARE(saidOf(written(content, thePassphrase(), work)), refused);
            QCOMPARE(saidOf(written(content, QString(), work)), refused);
        }
        QCOMPARE(work.asked(), asked);
    }

    void whatWasReadOfTheServersFileWithAlertsForOneServiceIsWrittenBackWithThem()
    {
        // The server's own file whose alerts are written as "ntfy": read, written again, and
        // read again, it holds what it held.
        const base::Result<Content> read =
            core::exchange::read(exampleFile("sealed-alerts-ntfy.qkvm"), thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        QCOMPARE(alertsIn(read.value()).format, u"ntfy"_s);
        WatchedWork work;
        const base::Result<Written> again = writtenWith(read.value(), thePassphrase(), work);
        QCOMPARE(saidOf(again), u"read"_s);
        QCOMPARE(again.value().notInIt(), QStringList());
        const base::Result<Content> back = core::exchange::read(textOf(again.value()), thePassphrase());
        QCOMPARE(saidOf(back), u"read"_s);
        QCOMPARE(alertsIn(back.value()).format, u"ntfy"_s);
        QCOMPARE(alertsIn(back.value()).address, u"https://ntfy.example.net/qkvm-wall"_s);
        QVERIFY(back.value() == read.value());
    }

    void whatWasReadOfTheServersFileWithDevicesAtTheEdgesIsWrittenBackAsItWasRead()
    {
        // Every device this reader took of the server's file is one the writer writes: the nine
        // the server takes, the one at a name in its "xn--" form among them, each with its
        // console link as it was written, also where that is one this program could not open.
        const base::Result<Content> read =
            core::exchange::read(exampleFile("sealed-odd-devices.qkvm"), thePassphrase());
        QCOMPARE(saidOf(read), u"read"_s);
        QCOMPARE(devicesIn(read.value()).size(), qsizetype{9});
        Content taken = read.value();
        taken.leftOut.clear();
        WatchedWork work;
        const base::Result<Written> again = writtenWith(taken, thePassphrase(), work);
        QCOMPARE(saidOf(again), u"read"_s);
        // (The one at a name in its "xn--" form has its line, as every such one has.)
        QCOMPARE(again.value().notInIt(), QStringList{atANameInItsXnForm(
                                              ofTheWall(3, u"an address in the short form of a name beyond ASCII"_s))});
        const base::Result<Content> back = core::exchange::read(textOf(again.value()), thePassphrase());
        QCOMPARE(saidOf(back), u"read"_s);
        QCOMPARE(back.value().leftOut, QStringList());
        QCOMPARE(back.value().devices, taken.devices);
        QCOMPARE(linkOf(devicesIn(back.value()).value(0)), u"https://b"_s + QChar(0xfc) + u"ro.example/"_s);
        QCOMPARE(linkOf(devicesIn(back.value()).value(1)), u"https://console.example:99999/"_s);
        QCOMPARE(devicesIn(back.value()).value(2).address, u"https://xn--mnchen-3ya.example"_s);
        QCOMPARE(devicesIn(back.value()).value(2).password, u"made-up-password"_s);
    }

    void aWallWithAKvmAtANameInItsXnFormIsWritten()
    {
        // A KVM at a name in its "xn--" form is one this program speaks to (it is taken where a
        // KVM is added) and one a server takes. It was left out when a file was read, for one
        // round; and since nothing is written that is not read back, one such KVM on a wall
        // meant that the wall could not be written at all.
        Device atSuchAName = deviceAt(u"https://xn--mnchen-3ya.example"_s);
        atSuchAName.name = u"Rack 3"_s;
        atSuchAName.password = u"made-up-password-3"_s;
        Content content = aWall();
        QList<Device> devices = devicesIn(content);
        devices.insert(1, atSuchAName);
        content.devices = devices;
        WatchedWork work;
        for (const QString& passphrase : {thePassphrase(), QString()}) {
            const base::Result<Written> whole = writtenWith(content, passphrase, work);
            QCOMPARE(saidOf(whole), u"read"_s);
            // (Written, and with its line: whether a server takes a name in this form is not
            // guessed, the next test says.)
            QCOMPARE(whole.value().notInIt(), QStringList{atANameInItsXnForm(ofTheWall(2, u"Rack 3"_s))});
            const base::Result<Content> back = core::exchange::read(textOf(whole.value()), passphrase);
            QCOMPARE(saidOf(back), u"read"_s);
            QCOMPARE(back.value().leftOut, QStringList());
            QCOMPARE(devicesIn(back.value()).size(), qsizetype{6});
            QCOMPARE(devicesIn(back.value()).value(1).address, u"https://xn--mnchen-3ya.example"_s);
            QCOMPARE(devicesIn(back.value()).value(1).name, u"Rack 3"_s);
            const QString itsPassword = passphrase.isEmpty() ? QString() : atSuchAName.password;
            QCOMPARE(devicesIn(back.value()).value(1).password, itsPassword);
        }
    }

    void aKvmAtANameAServerMayNotTakeIsWrittenAndThatIsHandedBackBesideTheFile_data()
    {
        // Names this program speaks to, and whether a line is handed back beside the file for a
        // KVM at each. By two rules that are said, and by no guess at what a server's library
        // makes of a name: a part of the name begins with "xn--"; or the whole name reads as
        // digits, a sign, digits, a sign, digits, a sign, digits, and is not four plain numbers.
        // (The first rule was a guess for one round, by the letters a name stands for: then the
        // rows marked so had no line.)
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("line"); // 'x': a name in its xn-- form; 'n': reads as numbers; nothing: none
        const QString inItsXnForm = u"x"_s;
        const QString asNumbers = u"n"_s;
        // Left out by the server's own reader when it was asked (build/mine/x21_odd.log).
        QTest::newRow("a heart in the name") << u"https://xn--i-7iq.example"_s << inItsXnForm;
        QTest::newRow("a picture for a name") << u"https://xn--ls8h.example:8443"_s << inItsXnForm;
        QTest::newRow("an underscore in another part of the name")
            << u"https://xn--mnchen-3ya.my_host"_s << inItsXnForm;
        QTest::newRow("the same, as somebody typed it") << u"HTTPS://XN--mnchen-3ya.My_Host:443/kvm"_s << inItsXnForm;
        // Taken by that reader when it was asked, and with a line all the same (no line before).
        QTest::newRow("a name of a town") << u"https://xn--mnchen-3ya.example"_s << inItsXnForm;
        QTest::newRow("a sharp s") << u"https://xn--fa-hia.example"_s << inItsXnForm;
        // A part in that form that is not the first (no line before).
        QTest::newRow("an underscore before a part in that form")
            << u"https://my_host.xn--mnchen-3ya.example"_s << inItsXnForm;
        QTest::newRow("a part of a name that is not its first")
            << u"https://kvm.xn--mnchen-3ya.example"_s << inItsXnForm;
        QTest::newRow("the last part of a name") << u"https://kvm.example.xn--mnchen-3ya"_s << inItsXnForm;
        // Names of digits with single signs between them (no line before).
        QTest::newRow("digits and hyphens") << u"https://192-168-1-20"_s << asNumbers;
        QTest::newRow("numbers with a hyphen where the last dot would be") << u"https://1.2.3-4"_s << asNumbers;
        QTest::newRow("digits and hyphens, with a port") << u"https://10-0-0-5:8443"_s << asNumbers;
        QTest::newRow("digits with letters between them") << u"https://1a2b3c4"_s << asNumbers;
        // No line: no part begins with "xn--", and the name does not read as numbers.
        QTest::newRow("a name with an underscore and nothing in that form") << u"https://my_kvm.example"_s << QString();
        QTest::newRow("four plain numbers") << u"https://192.168.1.20"_s << QString();
        QTest::newRow("digits and hyphens before an ending") << u"https://10-0-0-5.lan"_s << QString();
        QTest::newRow("digits and hyphens behind a letter") << u"https://ip-10-0-0-5"_s << QString();
        QTest::newRow("the mark in the middle of a part") << u"https://axn--b.example"_s << QString();
    }

    void aKvmAtANameAServerMayNotTakeIsWrittenAndThatIsHandedBackBesideTheFile()
    {
        // Where this program takes more than a server: a name with a part in its "xn--" form,
        // which Qt reads by one rule and the server's library by another; and a name that
        // library may read as numbers. A KVM at one is written, and read back here; a server
        // that reads the file may leave it out. So it is said beside the file, where whoever
        // asked for the file has to look: for every such name, and not for those of which a
        // guess says that a server refuses them.
        QFETCH(QString, address);
        QFETCH(QString, line);
        Device atSuchAName = deviceAt(address);
        atSuchAName.name = u"Rack 4"_s;
        atSuchAName.tls = u"system"_s;
        atSuchAName.password = u"made-up-password-4"_s;
        Content content;
        content.program = u"0.1.0"_s;
        content.devices = QList<Device>{deviceAt(u"https://192.168.1.21"_s), atSuchAName};
        WatchedWork work;
        const base::Result<Written> whole = writtenWith(content, thePassphrase(), work);
        QCOMPARE(saidOf(whole), u"read"_s);
        const QString which = ofTheWall(2, u"Rack 4"_s);
        QStringList expected;
        if (line == u"x"_s) {
            expected.append(atANameInItsXnForm(which));
        } else if (line == u"n"_s) {
            expected.append(atANameThatReadsAsNumbers(which));
        }
        QCOMPARE(whole.value().notInIt(), expected);
        // Written all the same, and read back here, password and all.
        const base::Result<Content> back =
            core::exchange::read(whole.value().file(core::exchange::NotInIt::wasLookedAt), thePassphrase());
        QCOMPARE(saidOf(back), u"read"_s);
        QCOMPARE(back.value().leftOut, QStringList());
        QCOMPARE(devicesIn(back.value()).size(), qsizetype{2});
        QCOMPARE(devicesIn(back.value()).value(1).name, u"Rack 4"_s);
        QCOMPARE(devicesIn(back.value()).value(1).password, u"made-up-password-4"_s);
    }

    void aDeviceWhoseConsoleLinkIsNoAddressIsWrittenWithItsLinkAsItWas()
    {
        // What the server keeps of a console link, this keeps and writes: a wall is not lost to
        // a file over a link, whatever is in it. Each of these was read from a server's file as
        // a device without its link once, and then such a device could not be written.
        const QStringList links{
            u"https://bench.example/a\nb"_s,
            u"ht\ttps://bench.example/"_s,
            u"https://bench.example/"_s + QChar(0x202e) + u"gpj.exe"_s,
            u"https://bench"_s + QChar(0x1b) + u".example/"_s,
            u"https://one.example\\two.example/\"a\"&b|c<d>e^f%g"_s,
            u"http://:80/"_s,
            u"https://bench.example/a"_s + QChar(0x200b) + QChar(0xfeff) + QChar(0xad) + u"b"_s,
        };
        QList<Device> devices;
        for (qsizetype at = 0; at < links.size(); ++at) {
            Device device = deviceAt(u"https://192.168.1.%1"_s.arg(at + 30));
            device.password = u"made-up-password"_s;
            device.consoleLinkAsWritten = LinkAsWritten(links.at(at));
            devices.append(device);
        }
        Content content;
        content.program = u"0.1.0"_s;
        content.devices = devices;
        WatchedWork work;
        const base::Result<Written> whole = writtenWith(content, thePassphrase(), work);
        QCOMPARE(saidOf(whole), u"read"_s);
        const base::Result<Content> back = core::exchange::read(textOf(whole.value()), thePassphrase());
        QCOMPARE(saidOf(back), u"read"_s);
        QCOMPARE(back.value().leftOut, QStringList());
        QCOMPARE(devicesIn(back.value()).size(), links.size());
        for (qsizetype at = 0; at < links.size(); ++at) {
            QCOMPARE(linkOf(devicesIn(back.value()).value(at)), links.at(at));
            QCOMPARE(devicesIn(back.value()).value(at).password, u"made-up-password"_s);
            // And none of them is an address to the one way there is to one.
            const LinkAsWritten kept = devicesIn(back.value()).value(at).consoleLinkAsWritten;
            QVERIFY(!core::exchange::consoleAddress(kept).has_value());
        }
    }

    void aConsoleLinkIsWrittenAsItWasWhateverThisProgramMakesOfIt_data()
    {
        // A console link is a text as a file had it. The writer read it as the reader does, and
        // where the reader leaves a device out for its link, nothing was written: also for links
        // that a server keeps (the first three, by the word of the server's own reader), so that
        // one device stopped every file. Now a link is written as it was; and where this reader
        // would leave the device out for it, a line beside the file says so, and that a server
        // may too. Whether a server does is not guessed.
        QTest::addColumn<QString>("link");
        QTest::addColumn<bool>("thisReaderLeavesTheDeviceOut");
        QTest::newRow("numbers of the new kind with a zone") << u"https://[fe80::1%25eth0]/"_s << true;
        QTest::newRow("the other thing brackets may hold") << u"https://[v1.x]/"_s << true;
        QTest::newRow("a sign that prints nothing in front of it")
            << QString(QChar(0x01)) + u"https://bench.example/"_s << true;
        // And what a server leaves a device out for as well, on some versions.
        QTest::newRow("numbers in brackets that are none") << u"https://[0x1::1]/"_s << true;
        // (What a server refuses for certain is not written at all: the test below.)
        // No line: this reader reads the device back with its link as it was.
        QTest::newRow("an address") << u"https://bench.example/kvm"_s << false;
        QTest::newRow("a letter that is not of the twenty-six in its host")
            << u"https://b"_s + QChar(0xfc) + u"ro.example/"_s << false;
        QTest::newRow("a line's end inside") << u"https://bench.example/a\nb"_s << false;
        QTest::newRow("255 signs") << u"https://bench.example/"_s + wideSigns(233) << false;
        QTest::newRow("none") << QString() << false;
    }

    void aConsoleLinkIsWrittenAsItWasWhateverThisProgramMakesOfIt()
    {
        QFETCH(QString, link);
        QFETCH(bool, thisReaderLeavesTheDeviceOut);
        Device withIt = deviceAt(u"https://192.168.1.22"_s);
        withIt.name = u"Rack 2"_s;
        withIt.password = u"made-up-password-2"_s;
        withIt.consoleLinkAsWritten = LinkAsWritten(link);
        Content content;
        content.program = u"0.1.0"_s;
        content.devices = QList<Device>{deviceAt(u"https://192.168.1.21"_s), withIt};
        WatchedWork work;
        for (const QString& passphrase : {thePassphrase(), QString()}) {
            const base::Result<Written> whole = writtenWith(content, passphrase, work);
            QCOMPARE(saidOf(whole), u"read"_s);
            QCOMPARE(whole.value().notInIt(),
                     thisReaderLeavesTheDeviceOut
                         ? QStringList{withALinkThisReaderLeavesItOutFor(ofTheWall(2, u"Rack 2"_s))}
                         : QStringList());
            const QByteArray file = textOf(whole.value());
            const base::Result<Content> back = core::exchange::read(file, passphrase);
            QCOMPARE(saidOf(back), u"read"_s);
            if (thisReaderLeavesTheDeviceOut) {
                // What the line says is so: this reader leaves the device out, and names it.
                QCOMPARE(devicesIn(back.value()).size(), qsizetype{1});
                QCOMPARE(back.value().leftOut.size(), qsizetype{1});
                QVERIFY2(back.value().leftOut.first().startsWith(u"Device 2: "_s),
                         qPrintable(back.value().leftOut.first()));
            } else {
                QCOMPARE(back.value().leftOut, QStringList());
                QCOMPARE(linkOf(devicesIn(back.value()).value(1)), link);
            }
            if (passphrase.isEmpty()) {
                // And the file has the link as it was, to the sign: read out of it by Qt.
                const QJsonArray devices = QJsonDocument::fromJson(file).object()[u"devices"_s].toArray();
                QCOMPARE(devices.size(), qsizetype{2});
                QCOMPARE(devices.at(1).toObject()[u"open_url"_s].toString(), link);
            }
        }
    }

    void aConsoleLinkThatAServerRefusesForCertainStopsTheFile_data()
    {
        QTest::addColumn<QString>("link");
        QTest::addColumn<QString>("why");
        const QString noNameOrPassword = u"the console link must not have a user name or password in it"_s;
        const QString noAddressOfTheWeb = u"the console link must start with http:// or https://"_s;
        QTest::newRow("a name and a password") << u"https://admin:hunter2@console.example/"_s << noNameOrPassword;
        QTest::newRow("a name alone") << u"https://admin@console.example/"_s << noNameOrPassword;
        QTest::newRow("a long s where the s of https stands")
            << u"http"_s + QChar(0x17f) + u"://console.example/"_s << noAddressOfTheWeb;
        QTest::newRow("a script") << u"javascript:alert(1)"_s << noAddressOfTheWeb;
        QTest::newRow("nothing where a host stands") << u"https:///kvm"_s << noAddressOfTheWeb;
        QTest::newRow("a blank in front of what is no address of the web")
            << u" ftp://console.example/"_s << noAddressOfTheWeb;
    }

    void aConsoleLinkThatAServerRefusesForCertainStopsTheFile()
    {
        // A link that a server refuses by its own rule, whatever version it is: no program would
        // read the device back, and nothing is written. (For one round such a link was written
        // as it was, with a line beside the file that said a server "may" leave the device
        // out: and a link with a name and a password in it stood in a plain file, which is
        // promised to hold no secret.)
        QFETCH(QString, link);
        QFETCH(QString, why);
        Device withIt = deviceAt(u"https://192.168.1.22"_s);
        withIt.name = u"Rack 2"_s;
        withIt.password = u"made-up-password-2"_s;
        withIt.consoleLinkAsWritten = LinkAsWritten(link);
        Content content;
        content.program = u"0.1.0"_s;
        content.devices = QList<Device>{deviceAt(u"https://192.168.1.21"_s), withIt};
        WatchedWork work;
        for (const QString& passphrase : {thePassphrase(), QString()}) {
            const base::Result<Written> whole = writtenWith(content, passphrase, work);
            QVERIFY2(!whole.ok(), "a file was written with a link that no program reads the device back for");
            const QString said = saidOf(whole);
            QVERIFY2(said.startsWith(u"desktop:not_written: Nothing was written: "_s), qPrintable(said));
            QVERIFY2(said.endsWith(why), qPrintable(said));
            // The link itself is in no sentence, and so no password that stands in one.
            QVERIFY2(!said.contains(u"hunter2"_s) && !said.contains(u"console.example"_s), qPrintable(said));
        }
    }

    void aConsoleLinkLongerThanAFileHoldsIsNotWritten()
    {
        // The one rule that keeps a file a file: 255 signs, which is what a server reads of a
        // link before it looks at it. With one more a server leaves the device out, for certain.
        Device withIt = deviceAt(u"https://192.168.1.22"_s);
        withIt.name = u"Rack 2"_s;
        withIt.consoleLinkAsWritten = LinkAsWritten(u"https://bench.example/"_s + wideSigns(234));
        Content content;
        content.devices = QList<Device>{deviceAt(u"https://192.168.1.21"_s), withIt};
        WatchedWork work;
        QCOMPARE(saidOf(writtenWith(content, thePassphrase(), work)),
                 u"desktop:not_written: Nothing was written: device 2 of this wall (\"Rack 2\") cannot be put into a "
                 "file of qKVM's: its console link is longer than a file holds"_s);
        QCOMPARE(work.asked(), 0);
    }

    void everyLineBesideAFileNamesItsDeviceByItsPlaceAndByItsNameWhereThatCanBeShown()
    {
        // The lines beside a file had the name as it was handed in before them: a device
        // without a name was ": only the fingerprint ...", naming nothing; a name with a line's
        // end behind it broke its line in two; and a name could close its own setting-off.
        const QByteArray fingerprint(32, '\x5a');
        Device noName = deviceAt(u"https://192.168.1.21"_s);
        noName.name.clear();
        noName.certificate = fingerprint;
        Device endsALine = deviceAt(u"https://192.168.1.22"_s);
        endsALine.name = u"Rack 3\n"_s;
        endsALine.certificate = fingerprint;
        Device likeASentence = deviceAt(u"https://xn--mnchen-3ya.example"_s);
        likeASentence.name = u"x\"): all of it is in the file. Device 9 of this wall (\"y"_s;
        likeASentence.tls = u"system"_s;
        Device nothingToSee = deviceAt(u"https://192.168.1.24"_s);
        nothingToSee.name = QString(QChar(0x3164)) + QChar(0xad);
        nothingToSee.certificate = fingerprint;
        Content content;
        content.devices = QList<Device>{noName, endsALine, likeASentence, nothingToSee};
        WatchedWork work;
        const base::Result<Written> whole = writtenWith(content, QString(), work);
        QCOMPARE(saidOf(whole), u"read"_s);
        QCOMPARE(whole.value().notInIt(),
                 (QStringList{
                     knownByAFingerprintAlone(ofTheWall(1, QString())),
                     knownByAFingerprintAlone(ofTheWall(2, u"Rack 3"_s)),
                     atANameInItsXnForm(ofTheWall(3, u"x'): all of it is in the file. Device 9 of this wall ('y"_s)),
                     knownByAFingerprintAlone(ofTheWall(4, QString())),
                 }));
        for (const QString& line : whole.value().notInIt()) {
            QVERIFY2(!line.contains(u'\n') && line.startsWith(u"Device "_s), qPrintable(line));
        }
    }

    void aDeviceThatIsReadWithItsCertificateIsOneThatIsWrittenAgain_data()
    {
        // One measure for how long a certificate may be: 20,000 signs of its text as a file
        // holds it, which is what the server asks of the text it reads. Asked of the text that
        // was read alone, a certificate written in ONE line passed that is longer than that once
        // it is kept in lines of 64, as it is written into the next file: its device was taken,
        // and from then on no file could be written of the wall it was on.
        QTest::addColumn<int>("bytes");
        QTest::addColumn<QString>("leftOut"); // why its device is; nothing: it is taken
        const QString asKept = u"Device 1: its certificate is longer than a file holds once it is written in lines "
                               "of 64, as qKVM keeps one"_s;
        QTest::newRow("the longest that fits a file in lines of 64") << 14727 << QString();
        QTest::newRow("one byte more: 20,001 signs once it is kept") << 14728 << asKept;
        QTest::newRow("the longest that fits a file in one line: 20,000 signs as read") << 14961 << asKept;
        QTest::newRow("one byte more: 20,004 signs as read, which a server refuses")
            << 14962 << u"Device 1: its certificate is not one"_s;
    }

    void aDeviceThatIsReadWithItsCertificateIsOneThatIsWrittenAgain()
    {
        QFETCH(int, bytes);
        QFETCH(QString, leftOut);
        const QByteArray der = certificateOfSoManyBytes(theCertificatesBytes(), bytes);
        QCOMPARE(der.size(), qsizetype{bytes});
        const QString oneLine = inOneLine(der);
        const QJsonObject entry{{u"url"_s, u"https://192.168.1.20"_s},
                                {u"kind"_s, u"kvmd"_s},
                                {u"name"_s, u"Rack 1"_s},
                                {u"cert_pem"_s, oneLine}};
        const base::Result<Content> read =
            core::exchange::read(plainFile({{u"devices"_s, QJsonArray{entry}}}), QString());
        QCOMPARE(saidOf(read), u"read"_s);
        // Whatever the reader took, the writer writes: no device that was read stops a file.
        Content taken = read.value();
        taken.leftOut.clear();
        WatchedWork work;
        const base::Result<Written> again = writtenWith(taken, QString(), work);
        QCOMPARE(saidOf(again), u"read"_s);
        const base::Result<Content> back = core::exchange::read(textOf(again.value()), QString());
        QCOMPARE(saidOf(back), u"read"_s);
        QCOMPARE(back.value().leftOut, QStringList());
        QCOMPARE(back.value().devices, taken.devices);
        // And what it took: the device with its certificate, where that fits a file as it is kept.
        QCOMPARE(read.value().leftOut, leftOut.isEmpty() ? QStringList() : QStringList{leftOut});
        QCOMPARE(devicesIn(read.value()).size(), qsizetype{leftOut.isEmpty() ? 1 : 0});
        if (leftOut.isEmpty()) {
            const Device device = devicesIn(read.value()).value(0);
            QCOMPARE(device.certificate, QCryptographicHash::hash(der, QCryptographicHash::Sha256));
            QVERIFY(device.certificatePem.size() <= core::exchange::longestCertificate);
            QCOMPARE(device.certificatePem.size(), qsizetype{19997});
        }
    }

    void whereAlertsGoIsWrittenByTheRuleItIsReadBy_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("refused"); // why nothing is written; nothing: it is written
        // What a server's reader does not keep either: no file of QKVM's holds it.
        const QString notOne = u"what this wall has set for alerts cannot be put into a file of qKVM's: the address "
                               "alerts go to is not a web address"_s;
        // What a server may keep, and this reader does not, since it cannot tell whether a server
        // does. "Not a web address that a file holds" was said of these too, and was not so: a
        // server's own file holds the first of them.
        const QString onlyHere = u"this program does not write what this wall has set for alerts into a file: "_s;
        const QString atAnXnName = onlyHere + u"the address alerts go to is at a name in its xn-- form: a qKVM server "
                                              "may take such an address, and this program cannot tell whether it "
                                              "does"_s;
        const QString aServerMay = onlyHere + u"the address alerts go to is one that a qKVM server may take, and "
                                              "this program cannot tell whether it does"_s;
        const QString hook = u"https://hooks.example/"_s;
        QTest::newRow("500 signs") << hook + QString(478, u'x') << QString();
        QTest::newRow("501 signs") << hook + QString(479, u'x') << notOne;
        QTest::newRow("a blank behind its host") << hook + u"a b"_s << notOne;
        QTest::newRow("a blank that breaks no line behind its host") << hook + u"a"_s + QChar(0xa0) + u"b"_s << notOne;
        QTest::newRow("numbers no address has") << u"https://999.1.1.1/h"_s << notOne;
        QTest::newRow("plain") << u"http://hooks.example/h"_s << QString();
        QTest::newRow("another way of speaking") << u"ftp://hooks.example/h"_s << notOne;
        QTest::newRow("a name in its xn-- form, which a server's own file holds")
            << u"https://xn--mnchen-3ya.example/hook"_s << atAnXnName;
        QTest::newRow("a name in letters beyond ASCII")
            << u"https://h"_s + QChar(0xf6) + u"oks.example/h"_s << aServerMay;
        QTest::newRow("digits and hyphens that look like four numbers") << u"https://192-168-1-9/h"_s << aServerMay;
        QTest::newRow("a name and a password in it") << u"https://user:made-up@hooks.example/h"_s << QString();
        QTest::newRow("numbers of the longer kind") << u"http://[2001:db8::7]:8099/hook"_s << QString();
        // What only counts as "https" where Qt compares without regard to small and large: the
        // writer's own check let it pass, and a server that read the file did not keep it.
        QTest::newRow("a long s where the s of https stands")
            << u"http"_s + QChar(0x17f) + u"://hooks.example/h"_s << notOne;
    }

    void whereAlertsGoIsWrittenByTheRuleItIsReadBy()
    {
        // What this reader would not keep is not sealed into a file: the writer's check is the
        // reader's rule, which is the server's and in a few places takes less
        // (test_exchange.cpp has it row by row). What is said of it is what is so: that no file
        // holds it, or that this program does not write it.
        QFETCH(QString, address);
        QFETCH(QString, refused);
        const bool isWritten = refused.isEmpty();
        Content content;
        content.program = u"0.1.0"_s;
        core::exchange::Alerts alerts;
        alerts.enabled = true;
        alerts.address = address;
        content.alerts = alerts;
        WatchedWork work;
        const base::Result<QByteArray> file = written(content, thePassphrase(), work);
        if (isWritten) {
            QCOMPARE(saidOf(file), u"read"_s);
            const base::Result<Content> back = core::exchange::read(file.value(), thePassphrase());
            QCOMPARE(saidOf(back), u"read"_s);
            QCOMPARE(alertsIn(back.value()).address, address);
            QCOMPARE(back.value().leftOut, QStringList());
        } else {
            QCOMPARE(saidOf(file), u"desktop:not_written: Nothing was written: "_s + refused);
            // Before anything is made of the passphrase.
            QCOMPARE(work.asked(), 0);
        }
    }

    void aDeviceKnownByItsFingerprintAloneIsWrittenWithoutACertificateAndThatIsHandedBack()
    {
        // This PC's wall knows a KVM by its certificate's SHA-256, and not by the certificate. A
        // file holds a certificate or none: such a device is written as pinned with none, which
        // a server reads as "known by whatever it shows first". The file is written, and what
        // is not in it is handed back beside it: a line for each such device, by its name.
        const QByteArray fingerprint(32, '\x5a');
        Device byFingerprint = deviceAt(u"https://192.168.1.21"_s);
        byFingerprint.name = u"Rack 1, top"_s;
        byFingerprint.password = u"made-up-password-1"_s;
        byFingerprint.certificate = fingerprint;
        // With the certificate itself: all of it is in the file.
        Device byCertificate = deviceAt(u"https://192.168.1.22"_s);
        byCertificate.certificatePem = theCertificate();
        byCertificate.certificate =
            QCryptographicHash::hash(QByteArray::fromBase64(QString(byCertificate.certificatePem)
                                                                .remove(u"-----BEGIN CERTIFICATE-----"_s)
                                                                .remove(u"-----END CERTIFICATE-----"_s)
                                                                .toLatin1()),
                                     QCryptographicHash::Sha256);
        // Known by none, and so written as what it is.
        const Device byNone = deviceAt(u"https://192.168.1.23"_s);
        // Not known by a certificate at all: Windows vouches for it, it is not checked, or it is
        // spoken to in the plain. What is remembered of one beside that is not a pin.
        Device byWindows = deviceAt(u"https://kvm.example.net"_s);
        byWindows.tls = u"system"_s;
        byWindows.certificate = fingerprint;
        Device unchecked = deviceAt(u"https://192.168.1.24"_s);
        unchecked.tls = u"insecure"_s;
        unchecked.certificate = fingerprint;
        Device plain = deviceAt(u"http://192.168.1.25"_s);
        plain.certificate = fingerprint;
        Device second = deviceAt(u"https://192.168.1.26:8443"_s);
        second.name = u"Rack 2, t"_s + QChar(0xfc) + u"r"_s;
        second.certificate = QByteArray(32, '\x11');
        Content content;
        content.program = u"0.1.0"_s;
        content.devices = QList<Device>{byFingerprint, byCertificate, byNone, byWindows, unchecked, plain, second};
        const QStringList expected{knownByAFingerprintAlone(ofTheWall(1, u"Rack 1, top"_s)),
                                   knownByAFingerprintAlone(ofTheWall(7, second.name))};
        WatchedWork work;
        for (const QString& passphrase : {thePassphrase(), QString()}) {
            const base::Result<Written> whole = writtenWith(content, passphrase, work);
            QCOMPARE(saidOf(whole), u"read"_s);
            QCOMPARE(whole.value().notInIt().size(), qsizetype{2});
            QCOMPARE(whole.value().notInIt(), expected);
            // The file itself: read back, the device is pinned and known by nothing; the one
            // with its certificate is known by it.
            const base::Result<Content> back =
                core::exchange::read(whole.value().file(core::exchange::NotInIt::wasLookedAt), passphrase);
            QCOMPARE(saidOf(back), u"read"_s);
            QCOMPARE(back.value().leftOut, QStringList());
            QCOMPARE(devicesIn(back.value()).size(), qsizetype{7});
            QCOMPARE(devicesIn(back.value()).value(0).tls, u"pinned"_s);
            QCOMPARE(devicesIn(back.value()).value(0).certificate, QByteArray());
            QCOMPARE(devicesIn(back.value()).value(1).certificate, byCertificate.certificate);
        }
        // And a wall of which a file says everything has nothing beside its file.
        content.devices = QList<Device>{byCertificate, byNone, byWindows, unchecked, plain};
        const base::Result<Written> all = writtenWith(content, thePassphrase(), work);
        QCOMPARE(saidOf(all), u"read"_s);
        QCOMPARE(all.value().notInIt(), QStringList());
        QCOMPARE(saidOf(writtenWith(aWall(), thePassphrase(), work)), u"read"_s);
        QCOMPARE(writtenWith(aWall(), thePassphrase(), work).value().notInIt(), QStringList());
    }

    void whateverIsHandedInAsAnAddressIsTakenByTheProgramsRuleAndWrittenTheOneWay()
    {
        // An address in capitals, with the port it has anyway and a path, of a device that is
        // pinned and known by its fingerprint alone. "Is it over HTTPS?" was asked of the text
        // as it was handed in, in small letters: such a device got no line, and its pin became
        // trust at first sight at a server with nobody told.
        Device inCapitals = deviceAt(u"HTTPS://KVM.Example.NET:443/kvm/"_s);
        inCapitals.name = u"Rack 1, top"_s;
        inCapitals.password = u"made-up-password-1"_s;
        inCapitals.certificate = QByteArray(32, '\x5a');
        // And plain, in capitals: no certificate is checked there, and nothing is said.
        Device plain = deviceAt(u"HTTP://192.168.1.23:80"_s);
        plain.name = u"Rack 1, plain"_s;
        plain.certificate = QByteArray(32, '\x22');
        Content content;
        content.program = u"0.1.0"_s;
        content.devices = QList<Device>{inCapitals, plain};
        const auto line = [](int place, const QString& name) {
            return knownByAFingerprintAlone(ofTheWall(place, name));
        };
        WatchedWork work;
        for (const QString& passphrase : {thePassphrase(), QString()}) {
            const base::Result<Written> whole = writtenWith(content, passphrase, work);
            QCOMPARE(saidOf(whole), u"read"_s);
            QCOMPARE(whole.value().notInIt(), QStringList{line(1, u"Rack 1, top"_s)});
            const base::Result<Content> back =
                core::exchange::read(whole.value().file(core::exchange::NotInIt::wasLookedAt), passphrase);
            QCOMPARE(saidOf(back), u"read"_s);
            QCOMPARE(back.value().leftOut, QStringList());
            QCOMPARE(devicesIn(back.value()).size(), qsizetype{2});
            QCOMPARE(devicesIn(back.value()).value(0).address, u"https://kvm.example.net"_s);
            QCOMPARE(devicesIn(back.value()).value(0).tls, u"pinned"_s);
            QCOMPARE(devicesIn(back.value()).value(1).address, u"http://192.168.1.23"_s);
        }
        // The file itself has the address the one way, and no other.
        const base::Result<QByteArray> plainFile = written(content, QString(), work);
        QCOMPARE(saidOf(plainFile), u"read"_s);
        QVERIFY(plainFile.value().contains("\"url\": \"https://kvm.example.net\""));
        QVERIFY(!plainFile.value().contains("HTTPS") && !plainFile.value().contains("Example"));
        // What was handed in is nobody's to change: it is as it was.
        QCOMPARE(devicesIn(content).value(0).address, u"HTTPS://KVM.Example.NET:443/kvm/"_s);

        // Typed without a way of speaking, which is HTTPS to this program wherever an address
        // is typed: written as that, and known by its fingerprint over HTTPS like the first.
        Device asTyped = deviceAt(u"192.168.1.22:8443"_s);
        asTyped.name = u"Rack 1, second"_s;
        asTyped.certificate = QByteArray(32, '\x11');
        content.devices = QList<Device>{inCapitals, asTyped, plain};
        const base::Result<Written> three = writtenWith(content, QString(), work);
        QCOMPARE(saidOf(three), u"read"_s);
        QCOMPARE(three.value().notInIt(), (QStringList{line(1, u"Rack 1, top"_s), line(2, u"Rack 1, second"_s)}));
        const base::Result<Content> back =
            core::exchange::read(three.value().file(core::exchange::NotInIt::wasLookedAt), QString());
        QCOMPARE(saidOf(back), u"read"_s);
        QCOMPARE(devicesIn(back.value()).value(1).address, u"https://192.168.1.22:8443"_s);
    }

    void whoeverTakesAFilesTextHasToSayThatWhatIsNotInItWasLookedAt()
    {
        // A file and what is not in it come back together, and the second is easily dropped: a
        // caller that wrote `write(...).value().file` had the text and had seen no line. Now
        // there is no way to the text that does not say that the lines were looked at: whoever
        // forgets does not compile.
        QVERIFY2(!HandsOutItsTextWithoutAWord<Written>, "the text of a file can be had without a word of what is "
                                                        "not in it");
        QVERIFY(HandsOutItsTextForTheWord<Written>);
        // Nor is the word said by accident: `written.file({})` compiled while the word was the
        // first of a list of words, and so did the word made of a nought.
        QVERIFY2(!HandsOutItsTextForEmptyBraces<Written>, "written.file({}) compiles: the word is said by empty "
                                                          "braces");
        QVERIFY2(!IsSaidByEmptyBraces<core::exchange::NotInIt>, "NotInIt{} compiles");
        QVERIFY2(!IsMadeOfANumber<core::exchange::NotInIt>, "a number can be made the word");
        // And the two are what was written, and what is not in it.
        Device byFingerprint = deviceAt(u"https://192.168.1.21"_s);
        byFingerprint.certificate = QByteArray(32, '\x5a');
        Content content;
        content.devices = QList<Device>{byFingerprint};
        WatchedWork work;
        const base::Result<Written> whole = writtenWith(content, QString(), work);
        QCOMPARE(saidOf(whole), u"read"_s);
        QCOMPARE(whole.value().notInIt().size(), qsizetype{1});
        const QByteArray text = whole.value().file(core::exchange::NotInIt::wasLookedAt);
        QVERIFY(text.startsWith("{\n  \"format\": \"qkvm-export\""));
    }

    void anErrorOfThePassphrasesWorkIsPassedOnAsItIsAndNothingIsReadOrWritten()
    {
        // The passphrase's work is stood in for here by something that hands back an error, in
        // the code and the sentence scrypt.cpp has for "Windows said no", and then in those for
        // "no memory". What this shows is that the reader and the writer pass such an error on
        // as it is: not as "wrong passphrase", and with no file. It does NOT show that a call of
        // Windows' own cryptography that fails says so: no seam makes one fail, and that rests
        // on reading (scrypt.cpp: pbkdf2 and windowsSaidNo; windows_sealing.cpp).
        int asked = 0;
        Sealing refusing;
        refusing.cost = quickCost;
        refusing.derive = [&asked](QByteArrayView, QByteArrayView, const core::scrypt::Cost&,
                                   const std::stop_token&) -> base::Result<core::SecretKey> {
            ++asked;
            return base::Error{core::scrypt::windowsRefused, "Windows could not do the work a passphrase takes."};
        };
        const QString refused = u"desktop:crypto: Windows could not do the work a passphrase takes."_s;
        QCOMPARE(saidOf(core::exchange::readWith(exampleFile("sealed-quick.qkvm"), SecretBytes::utf8Of(thePassphrase()),
                                                 refusing.derive)),
                 refused);
        QCOMPARE(saidOf(core::exchange::writeWith(aWall(), SecretBytes::utf8Of(thePassphrase()), refusing)), refused);
        QCOMPARE(asked, 2);
        // And where this PC has no memory for it, the same way.
        refusing.derive = [](QByteArrayView, QByteArrayView, const core::scrypt::Cost&,
                             const std::stop_token&) -> base::Result<core::SecretKey> {
            return base::Error{core::scrypt::noMemory, "This PC does not have the memory free."};
        };
        QCOMPARE(saidOf(core::exchange::readWith(exampleFile("sealed-quick.qkvm"), SecretBytes::utf8Of(thePassphrase()),
                                                 refusing.derive)),
                 u"desktop:no_memory: This PC does not have the memory free."_s);
        QCOMPARE(saidOf(core::exchange::writeWith(aWall(), SecretBytes::utf8Of(thePassphrase()), refusing)),
                 u"desktop:no_memory: This PC does not have the memory free."_s);
    }

    void aFileIsWrittenInPlainSignsOnly()
    {
        // Every sign that is not one from the blank to the tilde is spelled out by its number,
        // as the server's writer does it: a file's length in signs is then its length in bytes.
        WatchedWork work;
        const base::Result<QByteArray> file = written(aWall(), QString(), work);
        QCOMPARE(saidOf(file), u"read"_s);
        QVERIFY(std::all_of(file.value().cbegin(), file.value().cend(),
                            [](char sign) { return sign == '\n' || (sign >= ' ' && sign <= '~'); }));
        // The name of the first device: a letter with two dots, and a sign of two places as its halves.
        QVERIFY2(file.value().contains("\"name\": \"Rack 1, t" + spelled(0xfc) + "r " + spelled(0xd83d) +
                                       spelled(0xde00) + "\""),
                 file.value().left(300).constData());
        QCOMPARE(core::json::signsIn(file.value()), std::optional<qsizetype>(file.value().size()));
    }

    void whenAFileWasMadeIsWrittenAndNowWhereNothingIsSaid()
    {
        WatchedWork work;
        Content content;
        content.program = u"0.1.0"_s;
        content.devices = QList<Device>{deviceAt(u"https://192.168.1.21"_s)};
        const qint64 before = QDateTime::currentSecsSinceEpoch();
        const base::Result<Content> now = core::exchange::read(written(content, QString(), work).value(), QString());
        const qint64 after = QDateTime::currentSecsSinceEpoch();
        QVERIFY(now.value().made >= static_cast<double>(before) && now.value().made <= static_cast<double>(after));
        content.made = 1700000000.4;
        QCOMPARE(core::exchange::read(written(content, QString(), work).value(), QString()).value().made, 1700000000.0);
        QCOMPARE(core::exchange::read(written(content, QString(), work).value(), QString()).value().program,
                 u"0.1.0"_s);
    }

    void moreDevicesOrActionsThanAFileHoldsAreNotWritten()
    {
        WatchedWork work;
        QList<Device> devices;
        for (int number = 0; number < 2001; ++number) {
            devices.append(deviceAt(u"https://kvm%1.example"_s.arg(number)));
        }
        Content content;
        content.devices = devices;
        const QString tooMany = u"too_large: This wall has 2001 devices, and a file of qKVM's holds 2000. Nothing was "
                                "written: leave that part out, or have fewer."_s;
        QCOMPARE(saidOf(written(content, thePassphrase(), work)), tooMany);
        QCOMPARE(saidOf(written(content, QString(), work)), tooMany);
        // Exactly as many as a file holds are written, and read back.
        devices.removeLast();
        content.devices = devices;
        QElapsedTimer watch;
        watch.start();
        const base::Result<QByteArray> full = written(content, QString(), work);
        const qint64 writing = watch.elapsed();
        QCOMPARE(saidOf(full), u"read"_s);
        watch.restart();
        QCOMPARE(devicesIn(core::exchange::read(full.value(), QString()).value()).size(), qsizetype{2000});
        // (None of this is cut short where a work is told to stop: exchange_work.h.)
        qInfo("as many devices as a file holds: written in %lld ms, read in %lld ms", static_cast<long long>(writing),
              static_cast<long long>(watch.elapsed()));

        QList<Action> actions;
        for (int number = 0; number < 501; ++number) {
            Action action;
            action.name = u"Action %1"_s.arg(number);
            action.keys = u"KeyA"_s;
            actions.append(action);
        }
        Content withActions;
        withActions.actions = actions;
        QCOMPARE(saidOf(written(withActions, thePassphrase(), work)),
                 u"too_large: This wall has 501 saved actions, and a file of qKVM's holds 500. Nothing was written: "
                 "leave that part out, or have fewer."_s);
        actions.removeLast();
        withActions.actions = actions;
        QCOMPARE(saidOf(written(withActions, QString(), work)), u"read"_s);
        // Said before anything is made of the passphrase.
        QCOMPARE(work.asked(), 0);
    }

    void moreThanAFilesLengthIsNotWritten()
    {
        const QString tooMuch = u"too_large: This wall holds more than a file of qKVM's can. Nothing was written."_s;
        WatchedWork work;
        // Five hundred texts of two thousand signs, each of which a file spells out in six: six
        // million signs, where a file holds four.
        QList<Action> texts;
        for (int number = 0; number < 500; ++number) {
            Action action;
            action.name = u"Action %1"_s.arg(number);
            action.types = true;
            action.text = QString(2000, QChar(0xfc));
            texts.append(action);
        }
        Content withTexts;
        withTexts.actions = texts;
        QCOMPARE(saidOf(written(withTexts, thePassphrase(), work)), tooMuch);
        QCOMPARE(work.asked(), 0);

        // A plain file too: two thousand devices, each with a console link of signs that are
        // spelled out in twelve.
        QList<Device> links;
        for (int number = 0; number < 2000; ++number) {
            Device device = deviceAt(u"https://kvm%1.example"_s.arg(number));
            device.consoleLinkAsWritten = LinkAsWritten(u"https://bench.example/"_s + wideSigns(233));
            links.append(device);
        }
        Content withLinks;
        withLinks.devices = links;
        QCOMPARE(saidOf(written(withLinks, QString(), work)), tooMuch);
        QCOMPARE(work.asked(), 0);
    }

    void aFileIsWrittenUpToItsLengthToTheSign()
    {
        // What a sealed file holds is exactly as many signs as a file may hold, and is written
        // and read back; with one sign more, nothing is written. The length is made up of saved
        // texts: most of them of signs that are spelled out in six, and the last ones of plain
        // letters, as many as it takes.
        using core::exchange::largestPlain;
        const QString tooMuch = u"too_large: This wall holds more than a file of qKVM's can. Nothing was written."_s;
        WatchedWork work;
        QList<Action> texts;
        const auto add = [&texts](const QString& text) {
            Action action;
            action.name = u"Action %1"_s.arg(texts.size(), 3, 10, QChar(u'0'));
            action.types = true;
            action.text = text;
            texts.append(action);
        };
        const auto asAWall = [&texts] {
            Content content;
            content.program = u"0.1.0"_s;
            content.made = 1791832242;
            content.actions = texts;
            return content;
        };
        // How many signs a file holds inside: what is sealed is that, and a tag of sixteen bytes.
        const auto holds = [&work](const Content& wall) -> qsizetype {
            const base::Result<QByteArray> file = written(wall, thePassphrase(), work);
            if (!file) {
                return -1;
            }
            const QJsonObject sealed = QJsonDocument::fromJson(file.value()).object()[u"sealed"_s].toObject();
            return QByteArray::fromBase64(sealed[u"data"_s].toString().toLatin1()).size() - 16;
        };
        for (int number = 0; number < 320; ++number) {
            add(QString(2000, QChar(0xfc)));
        }
        const qsizetype bulk = holds(asAWall());
        add(u"x"_s);
        // What a saved text takes beside its letters.
        const qsizetype beside = holds(asAWall()) - bulk - 1;
        texts.removeLast();
        QVERIFY2(bulk > 3800000 && bulk < largestPlain - 10000 && beside > 50 && beside < 200,
                 qPrintable(u"%1 and %2"_s.arg(bulk).arg(beside)));
        qsizetype room = largestPlain - bulk;
        while (room > 2 * beside + 2002) {
            add(QString(2000, u'x'));
            room -= beside + 2000;
        }
        if (room > beside + 2000) {
            // Two more, of which the second has one letter at the least.
            const qsizetype first = std::min<qsizetype>(2000, room - 2 * beside - 1);
            add(QString(first, u'x'));
            room -= beside + first;
        }
        add(QString(room - beside, u'x'));
        QVERIFY(texts.size() <= 500);
        QElapsedTimer watch;
        watch.start();
        const base::Result<QByteArray> full = written(asAWall(), thePassphrase(), work);
        const qint64 writing = watch.elapsed();
        QCOMPARE(saidOf(full), u"read"_s);
        QCOMPARE(holds(asAWall()), largestPlain);
        QVERIFY(full.value().size() <= core::exchange::largestSealed);
        watch.restart();
        const base::Result<Content> read = core::exchange::read(full.value(), thePassphrase());
        // What exchange_work.h says of a work that is told to stop: nothing but the passphrase's
        // work is cut short, so this, the passphrase's work aside, is what such a work may
        // still take (and the wiping of its memory, which test_scrypt.cpp says).
        qInfo("a file of the largest size, at a cost of N 2^12 (some milliseconds of it are the passphrase's work): "
              "written in %lld ms, read in %lld ms",
              static_cast<long long>(writing), static_cast<long long>(watch.elapsed()));
        QCOMPARE(saidOf(read), u"read"_s);
        QCOMPARE(actionsIn(read.value()).size(), texts.size());
        QCOMPARE(read.value().leftOut, QStringList());
        QVERIFY(actionsIn(read.value()) == texts);
        // One sign more: said before anything is made of the passphrase.
        const int asked = work.asked();
        texts.last().text += u'x';
        QCOMPARE(saidOf(written(asAWall(), thePassphrase(), work)), tooMuch);
        QCOMPARE(work.asked(), asked);
    }

    void whatWasReadOfTheServersPlainFileIsWrittenBackSignForSign()
    {
        // The plain file a QKVM server's own writer made, read and written again: the same file,
        // to the last sign. (When it was made is in it, and is written as it was read.) But for
        // what the server's writer has written since its 0.2 and its 0.1 did not, which this
        // example is of: every device's `machine`, last, "" where none is kept (M7 1b-ii; none of
        // these keeps one: its server's 0.1 `ident` is empty).
        const QByteArray file = exampleFile("plain.json");
        const base::Result<Content> read = core::exchange::read(file, QString());
        QCOMPARE(saidOf(read), u"read"_s);
        WatchedWork work;
        const base::Result<QByteArray> again = written(read.value(), QString(), work);
        QCOMPARE(saidOf(again), u"read"_s);
        const QString asMade = QString::fromLatin1(file);
        const qsizetype actions = asMade.indexOf(u"\"actions\""_s);
        QVERIFY(actions > 0);
        const QString expected =
            QString(asMade.first(actions)).replace(u"\n    }"_s, u",\n      \"machine\": \"\"\n    }"_s) +
            asMade.sliced(actions);
        QCOMPARE(QString::fromLatin1(again.value()), expected);
    }

    void whatNoFileHoldsIsNotWrittenAndIsSaidToSomebodyWhoWritesAFile_data()
    {
        // What stops a file is said to somebody who has a wall and wants a file: which thing of
        // the wall it is, by its place there and its name, and what of it. Not in the reader's
        // sentences, which speak of a file that is there ("the file has that address twice",
        // "takes no device ... from a file").
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("why");
        const QString device = u"device 2 of this wall (\"Rack 2\") cannot be put into a file of qKVM's: "_s;
        const QString action = u"saved action 1 of this wall (\"Lock\") cannot be put into a file of qKVM's: "_s;
        QTest::newRow("a device of a kind no version knows")
            << u"kind"_s << device + u"it is not a kind of device this version knows"_s;
        QTest::newRow("a password longer than one may be")
            << u"password"_s << device + u"its password is longer than a file holds"_s;
        // (A name that is no short line of text is not shown either.)
        QTest::newRow("a name longer than one may be")
            << u"name"_s
            << u"device 2 of this wall cannot be put into a file of qKVM's: its name, user name or group is not a "
               "short line of text"_s;
        // (A console link stops no file but by its length: aConsoleLinkIsWrittenAsItWas...)
        QTest::newRow("a certificate that is none") << u"certificate"_s << device + u"its certificate is not one"_s;
        // (By the one measure of a certificate: aDeviceThatIsReadWithItsCertificate... has it.)
        QTest::newRow("a certificate in one line that is over the measure once it is kept in lines")
            << u"long certificate"_s
            << device + u"its certificate is longer than a file holds once it is written in lines of 64, as qKVM "
                        "keeps one"_s;
        QTest::newRow("two devices at one address")
            << u"twice"_s << device + u"another device of this wall is at the same address"_s;
        QTest::newRow("a saved action without a name")
            << u"action name"_s << u"saved action 1 of this wall cannot be put into a file of qKVM's: it has no name"_s;
        QTest::newRow("keys that are none") << u"keys"_s << action + u"its keys are not keys this version knows"_s;
        QTest::newRow("a text longer than is typed at once")
            << u"text"_s << action + u"it has no text to type, or more than can be typed at once"_s;
        QTest::newRow("an address for alerts that is none")
            << u"alerts"_s
            << u"what this wall has set for alerts cannot be put into a file of qKVM's: the address alerts go to is "
               "not a web address"_s;
        QTest::newRow("a pace no wall allows")
            << u"wall"_s
            << u"what this wall has set for itself cannot be put into a file of qKVM's: how often pictures are "
               "fetched is not a time a file holds (a fifth of a second to a minute)"_s;
        QTest::newRow("half a character in a name") << u"half"_s << u"it holds what no file can"_s;
        // An address that is none is not written, and is not shown in saying so: not with a
        // name and a password in front of an "@", and not where no "@" gives them away.
        QTest::newRow("an address with a name and a password in it")
            << u"password in address"_s << device + u"its address has a user name or a password in it"_s;
        QTest::newRow("an address with a password where a port stands")
            << u"password as port"_s << device + u"what stands behind the colon in its address is no port"_s;
        QTest::newRow("an address with a password before an at sign that is spelled out")
            << u"password before a spelled at"_s
            << device + u"what stands behind the colon in its address is no port"_s;
        QTest::newRow("an address with a token behind a question mark, refused for its host")
            << u"token in address"_s << device + u"its address has a sign in it that no name of a host has"_s;
        QTest::newRow("an address that speaks another way")
            << u"another way"_s << device + u"its address does not begin with http:// or https://"_s;
    }

    void whatNoFileHoldsIsNotWrittenAndIsSaidToSomebodyWhoWritesAFile()
    {
        QFETCH(QString, what);
        QFETCH(QString, why);
        Content content;
        content.program = u"0.1.0"_s;
        Device odd = deviceAt(u"https://192.168.1.22"_s);
        odd.name = u"Rack 2"_s;
        odd.password = u"made-up-password-2"_s;
        Action action;
        action.name = u"Lock"_s;
        action.keys = u"MetaLeft,KeyL"_s;
        core::exchange::Alerts alerts;
        core::exchange::WallSettings wall;
        if (what == u"kind"_s) {
            odd.kind = u"ipmi"_s;
        } else if (what == u"password"_s) {
            odd.password = QString(257, u'p');
        } else if (what == u"name"_s) {
            odd.name = QString(81, u'n');
        } else if (what == u"certificate"_s) {
            odd.certificatePem = u"-----BEGIN CERTIFICATE-----\nnot one\n-----END CERTIFICATE-----\n"_s;
        } else if (what == u"long certificate"_s) {
            odd.certificatePem = inOneLine(certificateOfSoManyBytes(theCertificatesBytes(), 14800));
        } else if (what == u"twice"_s) {
            odd.address = u"HTTPS://192.168.1.21:443/"_s;
        } else if (what == u"action name"_s) {
            action.name = u"   "_s;
        } else if (what == u"keys"_s) {
            action.keys = u"not a key name"_s;
        } else if (what == u"text"_s) {
            action.types = true;
            action.keys.clear();
            action.text = QString(2001, u't');
        } else if (what == u"alerts"_s) {
            alerts.address = u"file:///etc/passwd"_s;
        } else if (what == u"wall"_s) {
            wall.pictureEverySeconds = 100.0;
        } else if (what == u"password in address"_s) {
            odd.address = u"https://admin:hunter2@192.168.1.22"_s;
        } else if (what == u"password as port"_s) {
            odd.address = u"https://192.168.1.22:hunter2"_s;
        } else if (what == u"password before a spelled at"_s) {
            odd.address = u"https://admin:hunter2%40192.168.1.22"_s;
        } else if (what == u"token in address"_s) {
            odd.address = u"https://kvm example/?token=hunter2"_s;
        } else if (what == u"another way"_s) {
            odd.address = u"ftp://admin:hunter2@192.168.1.22"_s;
        } else {
            odd.name = u"Rack"_s + QChar(0xd83d);
        }
        content.devices = QList<Device>{deviceAt(u"https://192.168.1.21"_s), odd};
        content.actions = QList<Action>{action};
        content.alerts = alerts;
        content.wall = wall;
        WatchedWork work;
        const base::Result<Written> whole = writtenWith(content, thePassphrase(), work);
        // Nothing is written: no file that is short of the entry, with a line beside it that
        // may be overlooked.
        QVERIFY(!whole.ok());
        const QString said = saidOf(whole);
        // No address that was refused is in what is said of it, and so no password that stands
        // in one.
        QVERIFY2(!said.contains(u"hunter2"_s) && !said.contains(u"admin"_s) && !said.contains(u"192.168"_s),
                 qPrintable(said));
        QCOMPARE(said, u"desktop:not_written: Nothing was written: "_s + why);
        // The same for a plain file, where the thing is one a plain file has.
        if (what != u"text"_s && what != u"alerts"_s && what != u"password"_s) {
            QCOMPARE(saidOf(written(content, QString(), work)), said);
        }
        // Before anything is made of the passphrase.
        QCOMPARE(work.asked(), 0);
        // And in order, the same wall is written.
        content.devices = QList<Device>{deviceAt(u"https://192.168.1.21"_s)};
        action = Action();
        action.name = u"Lock"_s;
        action.keys = u"MetaLeft,KeyL"_s;
        content.actions = QList<Action>{action};
        content.alerts = core::exchange::Alerts();
        content.wall = core::exchange::WallSettings();
        QCOMPARE(saidOf(written(content, thePassphrase(), work)), u"read"_s);
    }

    void aFileIsNotSealedAtACostAFileMayNotAskFor()
    {
        WatchedWork work;
        for (const core::scrypt::Cost& cost :
             {core::scrypt::Cost{512, 8, 1}, core::scrypt::Cost{quint64{1} << 21, 8, 1}, core::scrypt::Cost{4096, 8, 5},
              core::scrypt::Cost{4097, 8, 1}}) {
            const base::Result<Written> file =
                core::exchange::writeWith(aWall(), SecretBytes::utf8Of(thePassphrase()), Sealing{cost, work.derive()});
            QVERIFY(!file.ok());
            QCOMPARE(file.error().code, std::string(core::scrypt::notACost));
        }
        QCOMPARE(work.asked(), 0);
    }

    void aPassphraseIsTenToTwoHundredAndFiftySixSigns()
    {
        using core::exchange::passphraseRefused;
        QVERIFY(!passphraseRefused(thePassphrase()).has_value());
        QVERIFY(!passphraseRefused(QString(10, u'x')).has_value());
        QVERIFY(!passphraseRefused(wideSigns(10)).has_value());
        QVERIFY(!passphraseRefused(wideSigns(256)).has_value());
        QVERIFY(!passphraseRefused(u"  blanks count  "_s).has_value());
        const auto saidOfIt = [](const QString& passphrase) {
            const std::optional<base::Error> refused = passphraseRefused(passphrase);
            return refused ? QString::fromStdString(refused->code + ": " + refused->message) : u"a passphrase"_s;
        };
        QCOMPARE(saidOfIt(QString(9, u'x')), u"invalid: A passphrase has at least 10 characters."_s);
        QCOMPARE(saidOfIt(wideSigns(9)), u"invalid: A passphrase has at least 10 characters."_s);
        QCOMPARE(saidOfIt(QString()), u"invalid: A passphrase has at least 10 characters."_s);
        QCOMPARE(saidOfIt(wideSigns(257)), u"invalid: A passphrase has at most 256 characters."_s);
        QCOMPARE(saidOfIt(QString(257, u'x')), u"invalid: A passphrase has at most 256 characters."_s);
        const QString half = thePassphrase() + QChar(0xd83d);
        QCOMPARE(saidOfIt(half), u"invalid: A passphrase cannot have half a character in it."_s);
        // And none of the sentences says anything of what was typed.
        QVERIFY(!saidOfIt(u"secret!"_s).contains(u"secret"_s));
        // Nothing is written with what is no text.
        WatchedWork work;
        QCOMPARE(saidOf(written(aWall(), half, work)), u"invalid: A passphrase cannot have half a character in it."_s);
        QCOMPARE(saidOf(core::exchange::write(aWall(), half)),
                 u"invalid: A passphrase cannot have half a character in it."_s);
        QCOMPARE(work.asked(), 0);
    }

    // --- off the interface thread -------------------------------------------------------------------

    void aFileIsReadOffTheInterfaceThreadAndToldOnIt()
    {
        WatchedWork work;
        core::exchange::FileWork files(quickly(work));
        base::Result<Content> answer = base::Error{"test", "nothing was told yet"};
        QThread* toldOn = nullptr;
        int told = 0;
        files.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this, [&](const base::Result<Content>& read) {
            answer = read;
            toldOn = QThread::currentThread();
            ++told;
        });
        // It comes back at once, with nothing told yet.
        QVERIFY(files.isBusy());
        QCOMPARE(told, 0);
        QTRY_COMPARE(told, 1);
        QCOMPARE(toldOn, QThread::currentThread());
        QVERIFY(!files.isBusy());
        QCOMPARE(saidOf(answer), u"read"_s);
        QCOMPARE(devicesIn(answer.value()).size(), qsizetype{5});
        QCOMPARE(devicesIn(answer.value()).value(0).password, u"made-up-password-1"_s);
        QCOMPARE(work.asked(), 1);
        // And told once: nothing more comes.
        QTest::qWait(50);
        QCOMPARE(told, 1);
    }

    void aFileIsWrittenOffTheInterfaceThreadAndTheNextMayFollowFromWhereItIsTold()
    {
        WatchedWork work;
        core::exchange::FileWork files(quickly(work));
        const Content content = aWall();
        base::Result<Content> readBack = base::Error{"test", "nothing was told yet"};
        bool told = false;
        QStringList notInIt{u"nothing was told yet"_s};
        files.write(content, thePassphrase(), this, [&](const base::Result<Written>& file) {
            QVERIFY(file.ok());
            notInIt = file.value().notInIt();
            // From inside the telling: this file is done with, and the next is taken.
            QVERIFY(!files.isBusy());
            files.read(file.value().file(core::exchange::NotInIt::wasLookedAt), thePassphrase(), this,
                       [&](const base::Result<Content>& read) {
                           readBack = read;
                           told = true;
                       });
        });
        QTRY_VERIFY(told);
        QCOMPARE(saidOf(readBack), u"read"_s);
        QVERIFY(readBack.value() == content);
        QCOMPARE(work.asked(), 2);
        // What is not in the file comes with it, here as from write() itself.
        QCOMPARE(notInIt, QStringList());
        Content known = content;
        QList<Device> devices = devicesIn(known);
        devices[2].tls = u"pinned"_s;
        devices[2].certificate = QByteArray(32, '\x5a');
        known.devices = devices;
        told = false;
        files.write(known, thePassphrase(), this, [&](const base::Result<Written>& file) {
            QVERIFY(file.ok());
            notInIt = file.value().notInIt();
            told = true;
        });
        QTRY_VERIFY(told);
        QCOMPARE(notInIt, QStringList{knownByAFingerprintAlone(ofTheWall(3, u"kvm.example.net"_s))});
    }

    void refusalsAreToldTheSameWay()
    {
        WatchedWork work;
        core::exchange::FileWork files(quickly(work));
        QString said;
        files.read(exampleFile("sealed-quick.qkvm"), u"another passphrase"_s, this,
                   [&](const base::Result<Content>& read) { said = saidOf(read); });
        QTRY_COMPARE(said, u"wrong_passphrase: That is not the passphrase this file was made with"_s);
        said.clear();
        files.read("not json at all", QString(), this, [&](const base::Result<Content>& read) { said = saidOf(read); });
        QVERIFY(said.isEmpty());
        QTRY_COMPARE(said, u"not_a_file: This is not a file that qKVM made"_s);
        said.clear();
        files.write(aWall(), thePassphrase() + QChar(0xd83d), this,
                    [&](const base::Result<Written>& file) { said = saidOf(file); });
        QTRY_COMPARE(said, u"invalid: A passphrase cannot have half a character in it."_s);
        // Without anybody whose answer it would be, nothing is worked on at all.
        const int asked = work.asked();
        files.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), nullptr,
                   [&](const base::Result<Content>& read) { said = u"told to nobody: "_s + saidOf(read); });
        QVERIFY(!files.isBusy());
        QTest::qWait(50);
        QCOMPARE(work.asked(), asked);
        QCOMPARE(said, u"invalid: A passphrase cannot have half a character in it."_s);
    }

    void oneFileAtATime()
    {
        // The work of the first is held until the test lets it go.
        const auto held = std::make_shared<std::atomic<bool>>(true);
        const auto entered = std::make_shared<std::atomic<int>>(0);
        Sealing slow;
        slow.cost = quickCost;
        slow.derive = [held, entered](QByteArrayView passphrase, QByteArrayView salt, const core::scrypt::Cost& cost,
                                      const std::stop_token& stop) {
            ++*entered;
            while (held->load() && !stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return core::exchange::windowsDerive()(passphrase, salt, cost, stop);
        };
        core::exchange::FileWork files(slow);
        QStringList said;
        files.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                   [&](const base::Result<Content>& read) { said.append(u"first: "_s + saidOf(read)); });
        QTRY_COMPARE(entered->load(), 1);
        // A second, and a third that is to be written: refused while the first is worked on, in
        // the server's word for it, a turn of the loop later and not there and then.
        files.read(exampleFile("plain.json"), QString(), this,
                   [&](const base::Result<Content>& read) { said.append(u"second: "_s + saidOf(read)); });
        files.write(aWall(), QString(), this,
                    [&](const base::Result<Written>& file) { said.append(u"third: "_s + saidOf(file)); });
        QCOMPARE(said, QStringList());
        QTRY_COMPARE(said.size(), qsizetype{2});
        QCOMPARE(said, (QStringList{u"second: busy: Another file is being worked on. Try again in a moment."_s,
                                    u"third: busy: Another file is being worked on. Try again in a moment."_s}));
        QVERIFY(files.isBusy());
        QCOMPARE(entered->load(), 1);
        held->store(false);
        QTRY_COMPARE(said.size(), qsizetype{3});
        QCOMPARE(said.last(), u"first: read"_s);
        QVERIFY(!files.isBusy());
    }

    void oneFileAtATimeInTheWholeProgram()
    {
        // Two that work on files, as two boxes of the program would have one each: while the
        // first works on one, the second is refused too. The work of two passphrases at once
        // is twice the memory, and nothing lets that pile up.
        const auto held = std::make_shared<std::atomic<bool>>(true);
        const auto entered = std::make_shared<std::atomic<int>>(0);
        Sealing slow;
        slow.cost = quickCost;
        slow.derive = [held, entered](QByteArrayView passphrase, QByteArrayView salt, const core::scrypt::Cost& cost,
                                      const std::stop_token& stop) {
            ++*entered;
            while (held->load() && !stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return core::exchange::windowsDerive()(passphrase, salt, cost, stop);
        };
        core::exchange::FileWork first(slow);
        auto second = std::make_unique<core::exchange::FileWork>(slow);
        QStringList said;
        QVERIFY(!first.isBusy() && !second->isBusy());
        first.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                   [&](const base::Result<Content>& read) { said.append(u"first: "_s + saidOf(read)); });
        QTRY_COMPARE(entered->load(), 1);
        // Busy is what the program is, whichever of them is asked.
        QVERIFY(first.isBusy() && second->isBusy());
        second->read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                     [&](const base::Result<Content>& read) { said.append(u"second: "_s + saidOf(read)); });
        second->write(aWall(), thePassphrase(), this,
                      [&](const base::Result<Written>& file) { said.append(u"second, writing: "_s + saidOf(file)); });
        QCOMPARE(said, QStringList());
        QTRY_COMPARE(said.size(), qsizetype{2});
        QCOMPARE(said,
                 (QStringList{u"second: busy: Another file is being worked on. Try again in a moment."_s,
                              u"second, writing: busy: Another file is being worked on. Try again in a moment."_s}));
        // The passphrase's work was begun once: the second's never was.
        QCOMPARE(entered->load(), 1);
        // One that was refused holds no place: it may go, and the first still has its own.
        second.reset();
        QVERIFY(first.isBusy());
        held->store(false);
        QTRY_COMPARE(said.size(), qsizetype{3});
        QCOMPARE(said.last(), u"first: read"_s);
        QVERIFY(!first.isBusy());
        // And when the first is done, another's file is worked on.
        core::exchange::FileWork third(slow);
        third.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                   [&](const base::Result<Content>& read) { said.append(u"third: "_s + saidOf(read)); });
        QTRY_COMPARE(said.size(), qsizetype{4});
        QCOMPARE(said.last(), u"third: read"_s);
        QCOMPARE(entered->load(), 2);
    }

    void oneThatGoesWhileItWorksGivesThePlaceBack()
    {
        // The place a file is worked on in is the program's one: whoever has it and goes hands
        // it back, or no file could be worked on again.
        const auto entered = std::make_shared<std::atomic<int>>(0);
        Sealing endless;
        endless.cost = quickCost;
        endless.derive = [entered](QByteArrayView, QByteArrayView, const core::scrypt::Cost&,
                                   const std::stop_token& stop) -> base::Result<core::SecretKey> {
            ++*entered;
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return base::Error{core::scrypt::stopped, "Nobody waits for this any more."};
        };
        auto going = std::make_unique<core::exchange::FileWork>(endless);
        going->read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this, [](const base::Result<Content>&) {});
        QTRY_COMPARE(entered->load(), 1);
        WatchedWork work;
        core::exchange::FileWork next(quickly(work));
        QVERIFY(next.isBusy());
        going.reset();
        QVERIFY(!next.isBusy());
        QString said;
        next.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                  [&](const base::Result<Content>& read) { said = saidOf(read); });
        QTRY_COMPARE(said, u"read"_s);
    }

    void whoseAnswerItIsMayBeGoneAndIsThenNotCalled()
    {
        // What is to be told belongs to somebody: a box that asked, and may be closed before the
        // answer is there. Once that is gone, what it handed in is not called: it may hold on to
        // what went with it.
        const auto held = std::make_shared<std::atomic<bool>>(true);
        const auto entered = std::make_shared<std::atomic<int>>(0);
        Sealing slow;
        slow.cost = quickCost;
        slow.derive = [held, entered](QByteArrayView passphrase, QByteArrayView salt, const core::scrypt::Cost& cost,
                                      const std::stop_token& stop) {
            ++*entered;
            while (held->load() && !stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return core::exchange::windowsDerive()(passphrase, salt, cost, stop);
        };
        core::exchange::FileWork files(slow);
        QStringList said;
        auto asker = std::make_unique<QObject>();
        files.read(
            exampleFile("sealed-quick.qkvm"), thePassphrase(), asker.get(),
            [&](const base::Result<Content>& read) { said.append(u"to the one that is gone: "_s + saidOf(read)); });
        QTRY_COMPARE(entered->load(), 1);
        // A refusal is somebody's answer too, and is not told once they are gone.
        auto other = std::make_unique<QObject>();
        files.read(exampleFile("plain.json"), QString(), other.get(), [&](const base::Result<Content>& read) {
            said.append(u"a refusal to the one that is gone: "_s + saidOf(read));
        });
        other.reset();
        asker.reset();
        held->store(false);
        // The work runs to its end all the same, and the place is free again.
        QTRY_VERIFY(!files.isBusy());
        QTest::qWait(50);
        QCOMPARE(said, QStringList());
        // And the next one, asked by somebody who stays, is told.
        files.read(exampleFile("plain.json"), QString(), this, [&](const base::Result<Content>& read) {
            said.append(u"to the one that stays: "_s + saidOf(read));
        });
        QTRY_COMPARE(said, QStringList{u"to the one that stays: read"_s});
    }

    void whereTheWorkCannotBeBegunThatIsToldAndTheNextFileIsWorkedOn_data()
    {
        QTest::addColumn<bool>("noMemory");
        QTest::addColumn<QString>("told");
        QTest::newRow("Windows has no thread for it")
            << false << u"desktop:failed: Windows could not begin the work on the file. Try again in a moment."_s;
        QTest::newRow("this PC has no memory for it")
            << true
            << u"desktop:no_memory: This PC does not have the memory free to work on the file. Close another "
               "program and try again."_s;
    }

    void whereTheWorkCannotBeBegunThatIsToldAndTheNextFileIsWorkedOn()
    {
        // What the beginning of a thread throws where there is none to be had, thrown here just
        // before the thread would be begun: once, and the next time not.
        QFETCH(bool, noMemory);
        QFETCH(QString, told);
        WatchedWork work;
        int fails = 1;
        core::exchange::FileWork files(quickly(work), [&fails, noMemory] {
            if (fails > 0) {
                --fails;
                if (noMemory) {
                    throw std::bad_alloc();
                }
                throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            }
        });
        QStringList said;
        files.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                   [&](const base::Result<Content>& read) { said.append(saidOf(read)); });
        // Told like everything else: a turn of the loop later. And nothing is busy over it.
        QCOMPARE(said, QStringList());
        QVERIFY(!files.isBusy());
        QTRY_COMPARE(said, QStringList{told});
        QCOMPARE(work.asked(), 0);
        // The next file is worked on as if nothing had been, by this and by any other.
        files.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                   [&](const base::Result<Content>& read) { said.append(saidOf(read)); });
        QTRY_COMPARE(said, (QStringList{told, u"read"_s}));
        fails = 1;
        files.write(aWall(), thePassphrase(), this,
                    [&](const base::Result<Written>& file) { said.append(saidOf(file)); });
        QTRY_COMPARE(said, (QStringList{told, u"read"_s, told}));
        core::exchange::FileWork other(quickly(work));
        other.write(aWall(), thePassphrase(), this,
                    [&](const base::Result<Written>& file) { said.append(saidOf(file)); });
        QTRY_COMPARE(said, (QStringList{told, u"read"_s, told, u"read"_s}));
    }

    void whateverIsThrownWhereTheWorkIsBegunGivesThePlaceBackAndIsTold_data()
    {
        QTest::addColumn<int>("thrown");
        QTest::newRow("something of the library's that is neither of the two it is known to throw") << 0;
        QTest::newRow("something that is no error of the library's at all") << 1;
    }

    void whateverIsThrownWhereTheWorkIsBegunGivesThePlaceBackAndIsTold()
    {
        // Two things are known to be thrown where a thread is begun: no memory, and no thread.
        // Anything else that was thrown there left read() flying and the program's one place
        // taken: no file could be worked on again until the program was started anew.
        QFETCH(int, thrown);
        WatchedWork work;
        int fails = 1;
        core::exchange::FileWork files(quickly(work), [&fails, thrown] {
            if (fails > 0) {
                --fails;
                if (thrown == 0) {
                    throw std::length_error("made up for a test");
                }
                throw 7;
            }
        });
        QStringList said;
        bool flew = false;
        try {
            files.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                       [&](const base::Result<Content>& read) { said.append(saidOf(read)); });
        } catch (...) {
            flew = true;
        }
        QVERIFY2(!flew, "what was thrown where the work is begun left read() flying");
        QVERIFY2(!files.isBusy(), "the program's one place for a file was left taken");
        // Told like everything else: a turn of the loop later.
        QCOMPARE(said, QStringList());
        const QString told = u"desktop:failed: The file could not be worked on."_s;
        QTRY_COMPARE(said, QStringList{told});
        QCOMPARE(work.asked(), 0);
        // And the next file is worked on, by this and by any other.
        files.read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                   [&](const base::Result<Content>& read) { said.append(saidOf(read)); });
        QTRY_COMPARE(said, (QStringList{told, u"read"_s}));
        core::exchange::FileWork other(quickly(work));
        other.write(aWall(), thePassphrase(), this,
                    [&](const base::Result<Written>& file) { said.append(saidOf(file)); });
        QTRY_COMPARE(said, (QStringList{told, u"read"_s, u"read"_s}));
    }

    void whoeverAskedMayBeGoneAndTheWorkStopsAndNothingIsTold()
    {
        // Work that goes on for as long as anybody waits for it.
        const auto entered = std::make_shared<std::atomic<int>>(0);
        const auto stoppedAt = std::make_shared<std::atomic<int>>(0);
        Sealing endless;
        endless.cost = quickCost;
        endless.derive = [entered, stoppedAt](QByteArrayView, QByteArrayView, const core::scrypt::Cost&,
                                              const std::stop_token& stop) -> base::Result<core::SecretKey> {
            ++*entered;
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ++*stoppedAt;
            return base::Error{core::scrypt::stopped, "Nobody waits for this any more."};
        };
        int told = 0;
        auto files = std::make_unique<core::exchange::FileWork>(endless);
        files->read(exampleFile("sealed-quick.qkvm"), thePassphrase(), this,
                    [&told](const base::Result<Content>&) { ++told; });
        QTRY_COMPARE(entered->load(), 1);
        QElapsedTimer watch;
        watch.start();
        files.reset();
        // It was told to stop, and was waited for: nothing of it runs on behind the test.
        QCOMPARE(stoppedAt->load(), 1);
        QVERIFY2(watch.elapsed() < 2000, qPrintable(QString::number(watch.elapsed())));
        // And nothing is told to whoever is gone, now or later.
        QTest::qWait(100);
        QCOMPARE(told, 0);
    }

    void workOnTheRealThingEndsWhenWhoeverDoesItGoesAndNotByItself()
    {
        // The passphrase's own work, at the cost a file is written with, done again and again:
        // work that does not end by itself, so that no clock is asked how long it would have
        // taken. (A clock was asked once, and a busy PC gave it another answer.) When the
        // FileWork goes, the work that is under way is told so through the real thing's own
        // way of being told, ends as "stopped", and is waited for; had the telling not reached
        // it, it would have done all its rounds.
        constexpr int rounds = 12;
        struct Seen {
            std::atomic<int> entered{0};
            std::atomic<int> done{0};        // rounds done to their end
            std::atomic<int> stopped{0};     // ended as "stopped"
            std::atomic<int> cutUnderWay{0}; // of those: begun before anybody said stop
            std::atomic<int> otherwise{0};   // ended in any other way
        };
        const auto seen = std::make_shared<Seen>();
        Sealing again;
        again.cost = core::exchange::costAsWritten;
        again.derive = [seen](QByteArrayView passphrase, QByteArrayView salt, const core::scrypt::Cost& cost,
                              const std::stop_token& stop) -> base::Result<core::SecretKey> {
            ++seen->entered;
            base::Result<core::SecretKey> made = base::Error{"test", "no round was done"};
            for (int round = 0; round < rounds; ++round) {
                const bool begunBeforeTheStop = !stop.stop_requested();
                made = core::exchange::windowsDerive()(passphrase, salt, cost, stop);
                if (made.ok()) {
                    ++seen->done;
                    continue;
                }
                const bool wasStopped = made.error().code == core::scrypt::stopped;
                seen->stopped += wasStopped ? 1 : 0;
                seen->cutUnderWay += wasStopped && begunBeforeTheStop ? 1 : 0;
                seen->otherwise += wasStopped ? 0 : 1;
                break;
            }
            return made;
        };
        int told = 0;
        auto files = std::make_unique<core::exchange::FileWork>(again);
        files->read(exampleFile("sealed-as-written.qkvm"), thePassphrase(), this,
                    [&told](const base::Result<Content>&) { ++told; });
        QTRY_COMPARE(seen->entered.load(), 1);
        files.reset();
        // It was waited for: what it came to is known here, and nothing of it runs on.
        QCOMPARE(seen->stopped.load(), 1);
        QCOMPARE(seen->otherwise.load(), 0);
        QVERIFY2(seen->done.load() < rounds, "the work did all its rounds: nobody told it to stop");
        qInfo("work at the full cost that would have gone on for %d rounds ended as stopped after %d of them%s", rounds,
              seen->done.load(), seen->cutUnderWay.load() == 1 ? ", in the middle of one" : ", between two of them");
        // And nothing is told to whoever is gone, now or later; and the place is free again.
        QTest::qWait(50);
        QCOMPARE(told, 0);
        WatchedWork work;
        const core::exchange::FileWork next(quickly(work));
        QVERIFY(!next.isBusy());
    }
};

QTEST_GUILESS_MAIN(TestExchangeWritten)
#include "test_exchange_written.moc"
