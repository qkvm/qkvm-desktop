#include <optional>
#include <string>

#include <QStringList>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <qkvm/core/proxmox_answers.h>

using namespace Qt::StringLiterals;
namespace proxmox = qkvm::core::proxmox;

namespace {

QJsonValue parsed(const QByteArray& json)
{
    const QJsonDocument read = QJsonDocument::fromJson(json);
    return read.isArray() ? QJsonValue(read.array()) : QJsonValue(read.object());
}

// The guest an address names, as "origin node vmid", or the code of its refusal: as it is typed, or as a wall keeps it.
QString named(const QString& typed, bool asKept = false)
{
    const qkvm::base::Result<proxmox::Address> found =
        asKept ? proxmox::keptAddressOf(typed) : proxmox::addressOf(typed);
    if (!found) {
        return QString::fromStdString(found.error().code);
    }
    return found.value().origin.toString(QUrl::FullyEncoded) + u' ' + found.value().node + u' ' +
           (found.value().vmid ? QString::number(*found.value().vmid) : u"-"_s);
}

const QString tokenId = u"wall@pve!qkvm"_s;
const QString secret = u"0b9e4f2a-5c1d-4e8f-9a3b-7d6c5e4f3a2b"_s;

} // namespace

// What a Proxmox says through its API, and what is typed for one (proxmox_answers.h), against what a real Proxmox VE
// 9.2 answered as the web's tests have it (../app/tests/test_proxmox.py of web 0.3.1).
class TestProxmoxAnswers : public QObject {
    Q_OBJECT

private slots:
    // The web's test_how_a_guests_address_is_read_and_written.
    void aGuestsAddressIsReadAsTheWebReadsIt()
    {
        QCOMPARE(named(u"pve.lan"_s), u"https://pve.lan:8006  -"_s);
        QCOMPARE(named(u" PVE.lan:8006/ "_s), u"https://pve.lan:8006  -"_s);
        QCOMPARE(named(u"https://192.168.1.5:443/"_s), u"https://192.168.1.5  -"_s);
        QCOMPARE(named(u"pve.lan/100"_s), u"https://pve.lan:8006  100"_s);
        QCOMPARE(named(u"pve.lan:8006/pve1/101"_s), u"https://pve.lan:8006 pve1 101"_s);
        QCOMPARE(named(u"https://pve.lan:8006/nodes/pve1/qemu/100"_s), u"https://pve.lan:8006 pve1 100"_s);
        // The addresses Proxmox's own page shows: a guest picked in its tree, and its console.
        QCOMPARE(named(u"https://pve.lan:8006/#v1:0:=qemu%2F105:4:::::::"_s), u"https://pve.lan:8006  105"_s);
        QCOMPARE(named(u"https://pve.lan:8006/?console=kvm&novnc=1&vmid=107&vmname=x&node=pve2&resize=off&cmd="_s),
                 u"https://pve.lan:8006 pve2 107"_s);
        QCOMPARE(named(u"[::1]:8006/100"_s), u"https://[::1]:8006  100"_s);
        for (const QString& bad :
             {u""_s, u"http://pve.lan"_s, u"pve.lan:x"_s, u"user@pve.lan"_s, u"pve.lan/99"_s, u"pve.lan/abc"_s,
              u"pve.lan/pve_1/100"_s, u"pve.lan/a/b/c"_s, u"pve.lan/nodes/pve1/qemu/1000000000"_s,
              u"https://pve.lan:8006/nodes/pve1/lxc/200"_s, u"https://pve.lan:8006/#v1:0:=lxc%2F200:4"_s,
              u"https://pve.lan:8006/?console=lxc&vmid=200&node=pve1"_s}) {
            QVERIFY2(named(bad) == u"invalid"_s, qPrintable(bad + u": "_s + named(bad)));
        }
        QCOMPARE(proxmox::guestUrl(QUrl(u"https://pve.lan:8006"_s), u"pve1"_s, 100),
                 u"https://pve.lan:8006/nodes/pve1/qemu/100"_s);
        QCOMPARE(proxmox::consolePage(QUrl(u"https://pve.lan:8006"_s), u"pve1"_s, 100).toString(QUrl::FullyEncoded),
                 u"https://pve.lan:8006/?console=kvm&novnc=1&vmid=100&node=pve1&resize=scale"_s);
    }

    // Review cx3-3, M1: a guest's address as a wall keeps one (a wall's file, the web's and this program's) is at the
    // port it is written with, and one without a port at 443, as both writers mean it: each keeps 443 unwritten, a
    // Proxmox behind a proxy at 443 among them. What is typed without one is at 8006, as before.
    void aGuestsAddressAsAWallKeepsItIsAtThePortItIsWrittenWith()
    {
        const QString at443 = proxmox::guestUrl(QUrl(u"https://pve.lan"_s), u"pve1"_s, 100);
        QCOMPARE(at443, u"https://pve.lan/nodes/pve1/qemu/100"_s);
        QCOMPARE(named(at443, true), u"https://pve.lan pve1 100"_s);
        QCOMPARE(named(u"https://pve.lan:443/nodes/pve1/qemu/100"_s, true), u"https://pve.lan pve1 100"_s);
        QCOMPARE(named(u"https://pve.lan:8006/nodes/pve1/qemu/100"_s, true), u"https://pve.lan:8006 pve1 100"_s);
        QCOMPARE(named(u"https://PVE.lan:8443/nodes/pve2/qemu/101"_s, true), u"https://pve.lan:8443 pve2 101"_s);
        QCOMPARE(named(u"https://[::1]/nodes/pve1/qemu/100"_s, true), u"https://[::1] pve1 100"_s);
        // (The same guest typed into the Add box is at Proxmox's own port.)
        QCOMPARE(named(at443), u"https://pve.lan:8006 pve1 100"_s);
        // By the same rules otherwise: HTTPS alone, a guest's number, no container.
        for (const QString& bad :
             {u"http://pve.lan/nodes/pve1/qemu/100"_s, u"https://pve.lan/nodes/pve1/qemu/99"_s,
              u"https://pve.lan/nodes/pve1/lxc/200"_s, u"https://user@pve.lan/nodes/pve1/qemu/100"_s,
              u"https://pve.lan:x/nodes/pve1/qemu/100"_s}) {
            QVERIFY2(named(bad, true) == u"invalid"_s, qPrintable(bad + u": "_s + named(bad, true)));
        }
    }

    // The web's TOKEN_ID and TOKEN_SECRET, and test_what_is_no_token_is_sent_nowhere's shapes.
    void aTokenIsOfItsShapeAlone()
    {
        QVERIFY(proxmox::isTokenId(tokenId));
        QVERIFY(proxmox::isTokenSecret(secret));
        QVERIFY(proxmox::isTokenSecret(secret.toUpper()));
        for (const QString& notAnId : {u"root@pam"_s, u"wall@pve!qkvm\r\nX: y"_s, u"wall@pve!"_s, u"@pve!qkvm"_s,
                                       u"wall@pve!q kvm"_s, u""_s, u"wall@1pve!qkvm"_s}) {
            QVERIFY2(!proxmox::isTokenId(notAnId), qPrintable(notAnId));
        }
        for (const QString& notASecret :
             {u"a password"_s, secret + u"\r\n"_s, secret.left(35), u""_s, secret.chopped(1) + u'g'}) {
            QVERIFY2(!proxmox::isTokenSecret(notASecret), qPrintable(notASecret));
        }
    }

    // The web's test_what_a_proxmox_lists_is_taken_by_kind.
    void whatAProxmoxListsIsTakenByKind()
    {
        const proxmox::Listing listing = proxmox::guestsIn(parsed(
            R"([{"type": "qemu", "vmid": 101, "node": "pve1", "name": "db", "status": "stopped"}, )"
            R"({"type": "qemu", "vmid": 100, "node": "pve2", "name": "web\nforged line", "status": "running"}, )"
            R"({"type": "qemu", "vmid": 100, "node": "pve1", "name": "twice"}, )"
            R"({"type": "qemu", "vmid": 102, "node": "pve1", "template": 1}, )"
            R"({"type": "lxc", "vmid": 200, "node": "pve1"}, )"
            R"({"type": "qemu", "vmid": "103", "node": "pve1"}, {"type": "qemu", "vmid": 104, "node": "bad node"}, )"
            R"({"type": "qemu", "vmid": 99, "node": "pve1"}, {"type": "qemu", "vmid": true, "node": "pve1"}, )"
            R"({"type": "qemu", "vmid": 105.5, "node": "pve1"}, )"
            R"({"type": "node", "node": "pve1"}, "nothing", null, [1, 2]])"));
        const QList<proxmox::Listed> expected{{100, u"pve2"_s, u"web forged line"_s, u"running"_s},
                                              {101, u"pve1"_s, u"db"_s, u"stopped"_s}};
        QCOMPARE(listing.guests, expected);
        QCOMPARE(listing.containers, 1);
        QVERIFY(proxmox::guestsIn(parsed(R"({"data": "not a list"})")).guests.isEmpty());
        // One with no name is named by its number; a status of a kind of its own is none.
        const proxmox::Listing unnamed =
            proxmox::guestsIn(parsed(R"([{"type": "qemu", "vmid": 300, "node": "pve1", "status": []}])"));
        QCOMPARE(unnamed.guests.value(0).name, u"VM 300"_s);
        QCOMPARE(unnamed.guests.value(0).status, QString());
    }

    // No more than mostGuests are listed, whatever a list holds.
    void aListHoldsNoMoreGuestsThanAreListed()
    {
        QJsonArray many;
        for (int one = 0; one < proxmox::mostGuests + 50; ++one) {
            many.append(QJsonObject{{u"type"_s, u"qemu"_s}, {u"vmid"_s, 1000 + one}, {u"node"_s, u"pve1"_s}});
        }
        QCOMPARE(proxmox::guestsIn(many).guests.size(), proxmox::mostGuests);
    }

    // What Proxmox says with a no, in the web's words (data_of): its words in the body, as its other format writes
    // them.
    void aNoIsSaidInTheWebsWords()
    {
        const auto said = [](int status, const QByteArray& body) {
            const qkvm::base::Result<QJsonValue> got =
                proxmox::dataOf(proxmox::Answer{status, body, {}}, u"look at this guest", tokenId, secret);
            return got ? std::string("ok") : got.error().code + ": " + got.error().message;
        };
        QCOMPARE(said(403, R"({"data": null, "message": "Permission check failed (/vms/100, VM.Audit)\n"})"),
                 std::string("bad_response: Proxmox does not let the token look at this guest: Permission check failed "
                             "(/vms/100, VM.Audit)"));
        QCOMPARE(said(500, R"({"message": "Configuration file 'nodes/pve2/qemu-server/100.conf' does not exist\n"})"),
                 std::string("bad_response: Proxmox has no such guest there: Configuration file "
                             "'nodes/pve2/qemu-server/100.conf' does not exist"));
        QCOMPARE(said(595, R"({"data": null})"),
                 std::string("unreachable: Proxmox cannot reach the node the guest is on: HTTP 595"));
        QCOMPARE(
            said(500, R"({"message": "unable to read file: busy"})"),
            std::string("bad_response: Proxmox answered look at this guest with HTTP 500: unable to read file: busy"));
        QCOMPARE(said(200, R"({"nodata": 1})"),
                 std::string("bad_response: Proxmox answered look at this guest with what is not its API's answer"));
        QCOMPARE(said(200, QByteArray(100'000, '[')),
                 std::string("bad_response: Proxmox answered look at this guest with what is not its API's answer"));
        QCOMPARE(said(200, R"({"data": {"status": "running"}})"), std::string("ok"));
        // How each stands beside its words.
        QCOMPARE(proxmox::faultOf({500, R"({"message": "hostname lookup 'pve9' failed"})", {}}),
                 proxmox::Fault::WrongNode);
        QCOMPARE(proxmox::faultOf({500, R"({"message": "unable to read file: busy"})", {}}), proxmox::Fault::Unwell);
        for (const int lost : {502, 503, 504, 596, 597}) {
            QCOMPARE(proxmox::faultOf({lost, {}, {}}), proxmox::Fault::AnswerLost);
        }
        QCOMPARE(proxmox::faultOf({595, {}, {}}), proxmox::Fault::None);
        QCOMPARE(proxmox::faultOf({403, {}, {}}), proxmox::Fault::None);
    }

    // Outside review of 0.2.0, 3.2: what answers the unsigned question of its API (GET /version with no token) is a
    // Proxmox where it refuses it 401, as a real one does (Proxmox VE 9.2: "401 No ticket", a body of none), or says
    // its version as Proxmox's API says it; anything else is somebody else, in words that say the token did not go. And
    // a 401 that asks for a sign-in of its own (WWW-Authenticate) is somebody else's (review cx3-1, L2).
    void whatAnswersTheUnsignedQuestionIsAProxmoxByItsAnswerAlone_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("challenged");
        QTest::addColumn<bool>("aProxmox");
        QTest::newRow("refused") << 401 << QByteArray() << false << true;
        QTest::newRow("refused-asking-for-a-sign-in-of-its-own") << 401 << QByteArray() << true << false;
        QTest::newRow("its-version") << 200 << QByteArray(R"({"data":{"version":"9.2.21","release":"9.2"}})") << false
                                     << true;
        QTest::newRow("a-version-of-another-kind")
            << 200 << QByteArray(R"({"data":{"version":9.2}})") << false << false;
        QTest::newRow("no-version") << 200 << QByteArray(R"({"data":{"release":"9.2"}})") << false << false;
        QTest::newRow("a-page") << 200 << QByteArray("<!doctype html><form>Sign in</form>") << false << false;
        QTest::newRow("not-found") << 404 << QByteArray("Not Found") << false << false;
        QTest::newRow("a-redirect") << 302 << QByteArray() << false << false;
        QTest::newRow("forbidden") << 403 << QByteArray(R"({"data":null})") << false << false;
        QTest::newRow("an-error") << 500 << QByteArray(R"({"data":null})") << false << false;
    }
    void whatAnswersTheUnsignedQuestionIsAProxmoxByItsAnswerAlone()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(bool, challenged);
        QFETCH(bool, aProxmox);
        QCOMPARE(proxmox::saysItIsAProxmox(proxmox::Answer{status, body, "No ticket"}, challenged), aProxmox);
        const qkvm::base::Error why = proxmox::notAProxmoxThere(status, challenged);
        QCOMPARE(why.code, std::string("bad_response"));
        const std::string how =
            challenged ? std::string("HTTP 401, asking for a sign-in of its own") : "HTTP " + std::to_string(status);
        QVERIFY2(why.message.find("does not say it is Proxmox VE (" + how + ")") != std::string::npos,
                 why.message.c_str());
        QVERIFY(why.message.find("the token was not sent to it") != std::string::npos);
    }

    // Review c9b-2, H1: Proxmox's API server says why it refused in its status line, its body {"data":null} (and
    // `errors` for a parameter): said from there where the body says nothing, as the web's `_said` does (its
    // reason_phrase), for the wrong node as well; the body's message first where there is one.
    void aNoInTheStatusLineIsSaidInItsWords()
    {
        const auto said = [](int status, const QByteArray& reason, const QByteArray& body = R"({"data":null})") {
            const qkvm::base::Result<QJsonValue> got =
                proxmox::dataOf(proxmox::Answer{status, body, reason}, u"look at this guest", tokenId, secret);
            return got ? std::string("ok") : got.error().code + ": " + got.error().message;
        };
        QCOMPARE(said(403, "Permission check failed (/vms/100, VM.Config.CDROM)"),
                 std::string("bad_response: Proxmox does not let the token look at this guest: Permission check failed "
                             "(/vms/100, VM.Config.CDROM)"));
        QCOMPARE(said(500, "Configuration file 'nodes/pve2/qemu-server/100.conf' does not exist"),
                 std::string("bad_response: Proxmox has no such guest there: Configuration file "
                             "'nodes/pve2/qemu-server/100.conf' does not exist"));
        QCOMPARE(said(400, "Parameter verification failed.",
                      R"({"data":null,"errors":{"boot":"invalid format - format error"}})"),
                 std::string("bad_response: Proxmox answered look at this guest with HTTP 400: Parameter verification "
                             "failed."));
        QCOMPARE(said(596, "Connection timed out"),
                 std::string("unreachable: Proxmox cannot reach the node the guest is on: Connection timed out"));
        QCOMPARE(said(596, ""), std::string("unreachable: Proxmox cannot reach the node the guest is on: HTTP 596"));
        // (The body's message, where its other format writes one, before the status line's words.)
        QCOMPARE(said(403, "Forbidden", R"({"data":null,"message":"Permission check failed (/vms/100, VM.Audit)\n"})"),
                 std::string("bad_response: Proxmox does not let the token look at this guest: Permission check failed "
                             "(/vms/100, VM.Audit)"));
        // How each stands, by the words of its status line.
        QCOMPARE(proxmox::faultOf({500, R"({"data":null})", "hostname lookup 'pve9' failed - failed to get address"}),
                 proxmox::Fault::WrongNode);
        QCOMPARE(proxmox::faultOf({500, R"({"data":null})", "no such cluster node 'pve9'"}), proxmox::Fault::WrongNode);
        QCOMPARE(proxmox::faultOf({500, R"({"data":null})", "unable to read file: busy"}), proxmox::Fault::Unwell);
        QCOMPARE(proxmox::faultOf({403, R"({"data":null})", "Configuration file does not exist"}),
                 proxmox::Fault::None);
        // Review c9b-3, L4: of what "does not exist", only the guest's settings file says it is on another node; a
        // storage's, a volume's, a boot device's and another file's are no wrong node, and are said in their own words.
        QCOMPARE(proxmox::faultOf(
                     {500, R"({"data":null})", "Configuration file 'nodes/pve2/qemu-server/100.conf' does not exist"}),
                 proxmox::Fault::WrongNode);
        for (const QByteArray& other :
             {QByteArray("storage 'nfs-isos' does not exist"), QByteArray("volume 'local:iso/gone.iso' does not exist"),
              QByteArray("invalid bootorder: device 'scsi1' does not exist"),
              QByteArray("Configuration file 'nodes/pve1/lxc/100.conf' does not exist"),
              QByteArray("Configuration file 'qemu-server/100.conf.tmp' does not exist")}) {
            QCOMPARE(proxmox::faultOf({500, R"({"data":null})", other}), proxmox::Fault::Unwell);
            QCOMPARE(said(500, other),
                     "bad_response: Proxmox answered look at this guest with HTTP 500: " + other.toStdString());
        }
        // As one line of 160, of ASCII alone (as the web's reader decodes the status line), and never the token.
        const QString sent = u"PVEAPIToken="_s + tokenId + u'=' + secret;
        QCOMPARE(proxmox::saidBy({503, {}, "upstream failed; the request had Authorization: " + sent.toLatin1()},
                                 tokenId, secret),
                 u"upstream failed; the request had Authorization: (the token)"_s);
        QCOMPARE(proxmox::saidBy({503, {}, "upstream failed for " + QString(secret).remove(u'-').toLatin1()}, tokenId,
                                 secret),
                 QString(proxmox::saidTheToken));
        QCOMPARE(proxmox::saidBy({500, {}, QByteArray(900, 'x')}, tokenId, secret).size(), 160);
        QCOMPARE(proxmox::saidBy({500, {}, "busy\tnow \xc3\xa9t\xe9  "}, tokenId, secret), u"busy now t"_s);
        QCOMPARE(proxmox::saidBy({500, {}, "  "}, tokenId, secret), u"HTTP 500"_s);
    }

    // The web's test_the_token_said_back_in_another_spelling_is_not_said_at_all, and the token said back as it went.
    void theTokenSaidBackIsNeverSaid()
    {
        const auto words = [](const QString& message) {
            const QByteArray body = QJsonDocument(QJsonObject{{u"message"_s, message}}).toJson();
            return proxmox::saidBy({503, body, {}}, tokenId, secret);
        };
        const QString sent = u"PVEAPIToken="_s + tokenId + u'=' + secret;
        QCOMPARE(words(u"upstream failed; the request had Authorization: "_s + sent),
                 u"upstream failed; the request had Authorization: (the token)"_s);
        QCOMPARE(words(u"bad secret "_s + secret.toUpper()), u"bad secret (the token)"_s);
        QCOMPARE(words(u"it had "_s + sent), u"it had (the token)"_s);
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(tokenId));
        for (const QString& spelt :
             {secret.left(10) + u'\n' + secret.mid(10), secret.left(10) + u'\t' + secret.mid(10),
              QString(secret).remove(u'-'), secret.toUpper().replace(u'-', u' '), encoded,
              QString::fromLatin1(QUrl::toPercentEncoding(sent)), QString::fromLatin1(QUrl::toPercentEncoding(encoded)),
              u"half "_s + secret.mid(9, 9) + u" and the rest"_s}) {
            QCOMPARE(words(u"upstream failed for "_s + spelt), QString(proxmox::saidTheToken));
        }
        // What holds nothing of it is said as it is, one line, cut.
        QCOMPARE(words(u"unable to read file:\nbusy"_s), u"unable to read file: busy"_s);
        QCOMPARE(words(QString(500, u'x')).size(), 160);
        QCOMPARE(proxmox::saidBy({503, {}, {}}, tokenId, secret), u"HTTP 503"_s);
    }

    // The web's test_a_guests_state_said_in_a_kind_of_its_own_is_no_state, and a status as Proxmox says it.
    void aGuestsStateIsTakenByKind()
    {
        const proxmox::Current odd = proxmox::currentOf(
            parsed(R"({"status": [], "qmpstatus": {}, "name": ["x"], "cpus": "2", "maxmem": null})"));
        QVERIFY(!odd.on.has_value());
        QCOMPARE(odd.cpus, 0);
        QVERIFY(!odd.memoryGib.has_value());
        QVERIFY(odd.name.isEmpty());
        QVERIFY(!proxmox::currentOf(QJsonValue::Null).on.has_value());
        const proxmox::Current running = proxmox::currentOf(
            parsed(R"({"vmid": 100, "name": "web", "status": "running", "qmpstatus": "running", "cpus": 2, )"
                   R"("maxmem": 2147483648, "mem": 536870912, "cpu": 0.03, "uptime": 1234})"));
        QCOMPARE(running.on, std::optional<bool>(true));
        QCOMPARE(running.cpus, 2);
        QCOMPARE(running.memoryGib, std::optional<double>(2.0));
        QCOMPARE(running.uptime, std::optional<qint64>(1234));
        const qkvm::core::Status status = proxmox::statusOf(running, 100, u"pve1"_s, u"9.2.21"_s);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.hasPicture);
        QCOMPARE(status.power, std::optional<bool>(true));
        QCOMPARE(status.model, u"VM 100 on pve1"_s);
        QCOMPARE(status.version, u"Proxmox VE 9.2.21"_s);
        const qkvm::core::ServerReadings readings = status.server.value_or(qkvm::core::ServerReadings{});
        QCOMPARE(readings.processors, 2);
        QCOMPARE(readings.processor, u"vCPU"_s);
        const qkvm::core::Status stopped =
            proxmox::statusOf(proxmox::currentOf(parsed(R"({"status": "stopped", "cpus": 4})")), 101, u"pve1"_s, {});
        QCOMPARE(stopped.state, u"no_signal"_s);
        QCOMPARE(stopped.power, std::optional<bool>(false));
        QVERIFY(stopped.version.isEmpty());
        // Numbers out of reason are none.
        const proxmox::Current wild =
            proxmox::currentOf(parsed(R"({"status": "running", "cpus": 99999, "maxmem": -5, "uptime": 1.5})"));
        QCOMPARE(wild.cpus, 0);
        QVERIFY(!wild.memoryGib.has_value());
        QVERIFY(!wild.uptime.has_value());
    }

    // The web's test_a_drive_and_a_boot_order_are_read_as_proxmox_writes_them (its virtual media, step 7).
    void aDriveAndABootOrderAreReadAsProxmoxWritesThem()
    {
        const auto drive = [](const QJsonValue& setting) {
            const std::optional<proxmox::Drive> found = proxmox::driveOf(setting);
            if (!found) {
                return u"-"_s;
            }
            QStringList options;
            for (auto one = found->options.cbegin(); one != found->options.cend(); ++one) {
                options.append(one.key() + u'=' + one.value());
            }
            options.sort();
            return found->disc + u' ' + options.join(u',');
        };
        QCOMPARE(drive(u"local:iso/debian-13.iso,media=cdrom,size=700M"_s),
                 u"local:iso/debian-13.iso media=cdrom,size=700M"_s);
        QCOMPARE(drive(u"file=local:iso/a.iso,media=cdrom"_s), u"local:iso/a.iso media=cdrom"_s);
        QCOMPARE(drive(u"none,media=cdrom"_s), u"none media=cdrom"_s);
        for (const QJsonValue& odd : {QJsonValue(), QJsonValue(5), QJsonValue(u""_s), QJsonValue(u"a,b"_s),
                                      QJsonValue(QString(3000, u'x')), QJsonValue(QJsonArray{u"none"_s})}) {
            QCOMPARE(drive(odd), u"-"_s);
        }
        const auto cd = [](const char* config) {
            const std::optional<proxmox::Cd> found = proxmox::cdOf(parsed(config));
            return found ? found->slot + u' ' + found->disc : u"-"_s;
        };
        QCOMPARE(cd(R"({"ide2": "none,media=cdrom"})"), u"ide2 none"_s);
        // (The one that holds something.)
        QCOMPARE(cd(R"({"ide2": "none,media=cdrom", "sata1": "local:iso/debian-13.iso,media=cdrom"})"),
                 u"sata1 local:iso/debian-13.iso"_s);
        // (Disks, and no CD-ROM drive.)
        QCOMPARE(cd(R"({"ide2": "local-lvm:vm-100-disk-1,size=4G", "scsi0": "local-lvm:vm-100-disk-0"})"), u"-"_s);
        QCOMPARE(proxmox::cdSlots().size(), 41);
        QCOMPARE(proxmox::newCdSlots(), (QStringList{u"ide2"_s, u"ide0"_s, u"ide1"_s, u"ide3"_s, u"sata0"_s, u"sata1"_s,
                                                     u"sata2"_s, u"sata3"_s, u"sata4"_s, u"sata5"_s}));
        QCOMPARE(proxmox::bootOrderOf(parsed(R"({"boot": "order=scsi0;ide2;net0"})")),
                 std::optional<QStringList>({u"scsi0"_s, u"ide2"_s, u"net0"_s}));
        for (const char* odd :
             {"{}", R"({"boot": "cdn"})", R"({"boot": "order=scsi0;scsi0"})", R"({"boot": "order=scsi0,legacy=cdn"})",
              R"({"boot": "order=../x"})", R"({"boot": 5})"}) {
            QVERIFY2(!proxmox::bootOrderOf(parsed(odd)), odd);
        }
        // What is handed back to Proxmox as an image: a name of the characters Proxmox leaves in one, and no comma,
        // slash or space.
        QVERIFY(proxmox::isoVolumeOf(u"local:iso/debian-13.iso"));
        QCOMPARE(proxmox::isoVolumeOf(u"nas-1:iso/Win11_24H2_English_x64.iso").value_or(proxmox::IsoVolume{}).name,
                 u"Win11_24H2_English_x64.iso"_s);
        QCOMPARE(proxmox::isoVolumeOf(u"nas-1:iso/Win11_24H2_English_x64.iso").value_or(proxmox::IsoVolume{}).storage,
                 u"nas-1"_s);
        for (const QString& odd :
             {u"local:iso/a.iso,media=disk"_s, u"local:iso/../a.iso"_s, u"local:iso/a b.iso"_s,
              u"local:images/vm-100-disk-0.raw"_s, u"http://x/a.iso"_s, u"local:iso/.iso"_s, u"/dev/sr0"_s,
              // (Review c9b-2, L5: a line break at its end, and what could begin an option, an escape or another field
              // of a drive's setting or of a form; and letters a case-blind match would take for k and s, the Kelvin
              // sign and the long s, which are no ASCII: none is the name of an image uploaded to Proxmox.)
              u"local:iso/a.iso\n"_s, u"local:iso/a.iso\r"_s, u"local:iso/a%2Cb.iso"_s, u"local:iso/a=b.iso"_s,
              u"local:iso/a;b.iso"_s, u"local:iso/a&b.iso"_s, u"loc"_s + QChar(0x212A) + u"al:iso/a.iso"_s,
              u"local:iso/"_s + QChar(0x212A) + u".iso"_s, u"local:iso/a.i"_s + QChar(0x017F) + u"o"_s}) {
            QVERIFY2(!proxmox::isoVolumeOf(odd), qPrintable(odd));
        }
        // (Its ending in either case, as Proxmox takes one.)
        QVERIFY(proxmox::isoVolumeOf(u"local:iso/A.ISO"));
        QVERIFY(proxmox::isoVolumeOf(u"local:iso/b.Img"));
        // What is in the drive, for the card: by its image's name and storage, read-only, first in the boot order or
        // not.
        const std::optional<qkvm::core::MountedMedia> in = proxmox::mountedIn(
            parsed(R"({"boot": "order=ide2;scsi0", "ide2": "local:iso/debian-13.iso,media=cdrom,size=700M"})"));
        QVERIFY(in);
        QCOMPARE(in.value_or(qkvm::core::MountedMedia{}).drive, u"cd"_s);
        QCOMPARE(in.value_or(qkvm::core::MountedMedia{}).image, u"local:iso/debian-13.iso"_s);
        QCOMPARE(in.value_or(qkvm::core::MountedMedia{}).name, u"debian-13.iso"_s);
        QCOMPARE(in.value_or(qkvm::core::MountedMedia{}).where, u"local"_s);
        QVERIFY(in.value_or(qkvm::core::MountedMedia{}).readOnly);
        QCOMPARE(in.value_or(qkvm::core::MountedMedia{}).first, std::optional<bool>(true));
        QCOMPARE(
            proxmox::mountedIn(parsed(R"({"ide2": "cdrom,media=cdrom"})")).value_or(qkvm::core::MountedMedia{}).name,
            u"the host's own CD drive"_s);
        QCOMPARE(proxmox::mountedIn(parsed(R"({"ide2": "/mnt/pve/x/y.iso,media=cdrom"})"))
                     .value_or(qkvm::core::MountedMedia{})
                     .name,
                 u"y.iso"_s);
        QVERIFY(!proxmox::mountedIn(parsed(R"({"ide2": "none,media=cdrom"})")));
        QVERIFY(!proxmox::mountedIn(parsed("{}")));
        // The digest of the settings, as Proxmox writes one.
        QCOMPARE(proxmox::digestOf(parsed(R"({"digest": "0123456789abcdef0123456789abcdef01234567"})")),
                 u"0123456789abcdef0123456789abcdef01234567"_s);
        for (const char* odd : {"{}", R"({"digest": "x"})", R"({"digest": 5})",
                                R"({"digest": "0123456789ABCDEF0123456789ABCDEF01234567"})"}) {
            QVERIFY2(proxmox::digestOf(parsed(odd)).isEmpty(), odd);
        }
    }

    // The web's media_images, its reading: the storages of a node that hold ISO images, and the images on one, each
    // taken by kind.
    void aNodesStoragesAndTheirImagesAreTakenByKind()
    {
        const proxmox::Storages storages = proxmox::storagesIn(parsed(
            R"([{"storage": "local", "content": "iso,vztmpl,backup", "active": 1}, null, "local", {"storage": 5},)"
            R"( {"storage": "../x", "content": "iso", "active": 1}, {"storage": "zz", "content": "iso", "active": 0},)"
            R"( {"storage": "local", "content": "iso", "active": 1}, {"storage": "backups", "content": "backup"},)"
            R"( {"storage": "nas", "content": "iso", "active": false}, {"storage": "noword", "active": 1}])"));
        QCOMPARE(storages.names, QStringList{u"local"_s});
        QCOMPARE(storages.unseen,
                 (QStringList{u"zz: Proxmox says it is not active"_s, u"nas: Proxmox says it is not active"_s}));
        QVERIFY(proxmox::storagesIn(parsed(R"({"storage": "local"})")).names.isEmpty());
        const proxmox::Images images = proxmox::imagesIn(
            parsed(R"([{"volid": "local:iso/debian-13.iso", "content": "iso", "size": 700000000}, null, "x",)"
                   R"( {"volid": 5}, {"volid": "other:iso/local.iso", "content": "iso"},)"
                   R"( {"volid": "local:images/vm-100-disk-0.raw", "content": "images"},)"
                   R"( {"volid": "local:iso/../../etc/passwd.iso", "content": "iso"},)"
                   R"( {"volid": "local:iso/two.iso,media=disk", "content": "iso"},)"
                   R"( {"volid": "local:iso/odd.iso", "content": "iso", "size": "big"},)"
                   R"( {"volid": "local:iso/plain.iso"}, {"volid": "local:iso/vz.iso", "content": "vztmpl"}])"),
            u"local"_s, 2000);
        QCOMPARE(images.images.size(), 3);
        QCOMPARE(images.images.at(0).id, u"local:iso/debian-13.iso"_s);
        QCOMPARE(images.images.at(0).name, u"debian-13.iso"_s);
        QCOMPARE(images.images.at(0).where, u"local"_s);
        QCOMPARE(images.images.at(0).size, std::optional<qint64>(700'000'000));
        QCOMPARE(images.images.at(1).id, u"local:iso/odd.iso"_s);
        QVERIFY(!images.images.at(1).size); // (a size that is no number is none)
        QCOMPARE(images.images.at(2).id, u"local:iso/plain.iso"_s);
        QCOMPARE(images.leftOut, 8);
        // No more than there is room for.
        QCOMPARE(
            proxmox::imagesIn(parsed(R"([{"volid": "local:iso/a.iso"}, {"volid": "local:iso/b.iso"}])"), u"local"_s, 1)
                .leftOut,
            1);
    }
};

QTEST_GUILESS_MAIN(TestProxmoxAnswers)
#include "test_proxmox_answers.moc"
