#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <QByteArray>
#include <QCryptographicHash>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/device_asker.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using qkvm::core::DeviceAsker;
using qkvm::core::testing::Answered;
using qkvm::core::testing::Asked;
using qkvm::core::testing::NobodyThere;
using qkvm::core::testing::StandIn;
using qkvm::core::testing::Tls;

namespace {

// The times a real device is given, made short.
DeviceAsker::Times quick()
{
    return DeviceAsker::Times{1500, 1500, 1500};
}

DeviceAsker::Where at(const QString& address)
{
    DeviceAsker::Where where;
    where.address = QUrl(address);
    return where;
}

// A session in a header of its own, as a server's management controller has it (Redfish's X-Auth-Token).
DeviceAsker::Carried inAHeader()
{
    return DeviceAsker::Carried{DeviceAsker::Carried::In::Header, "X-Auth-Token"_ba};
}

DeviceAsker::Ask asking(const QByteArray& method, const QString& path)
{
    DeviceAsker::Ask question;
    question.method = method;
    question.path = path;
    question.largest = qsizetype{64} * 1024;
    return question;
}

// Asks, lets the program's loop run until the answer is there, and gives it.
base::Result<DeviceAsker::Reply> answerTo(DeviceAsker& asker, const DeviceAsker::Ask& question,
                                          const QByteArray& token = {}, int withinMs = 20000)
{
    QList<base::Result<DeviceAsker::Reply>> answers;
    asker.send(question, token, [&](base::Result<DeviceAsker::Reply> reply) { answers.append(std::move(reply)); });
    if (!answers.isEmpty()) {
        return base::Error{"at_once", "The answer came before the call came back."};
    }
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, withinMs)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

std::string codeOf(const base::Result<DeviceAsker::Reply>& reply)
{
    return reply ? std::string("ok") : reply.error().code;
}

Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

// Reproduce a received request whose send notification was not recorded (as test_kvmd_device.cpp has it):
// only the reply's own note of it is undone; the loopback exchange still occurs.
class UnreportedSend final : public QObject {
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::DynamicPropertyChange &&
            static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "wentOut"_ba &&
            watched->property("wentOut").toBool()) {
            watched->setProperty("wentOut", false);
        }
        return false;
    }
};

Answered silence()
{
    Answered never;
    never.never = true;
    return never;
}

} // namespace

// How the driver of a KVM or a server's controller of this PC's own asks it a question (device_asker.h),
// beyond what the kvmd's and the JetKVM's tests try of it: the questions a controller is asked (PATCH and
// DELETE), the headers a question may send, the session carried in a header of its own, and the headers of
// an answer read as they were sent.
class TestDeviceAsker : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The asker chooses Windows' own TLS, which is refused once TLS is in use: so one is made before
        // anything else touches TLS.
        const DeviceAsker first(DeviceAsker::Where{}, quick(), "auth_token"_ba);
    }

    void qtSaysWhenADeleteOrAPatchHasGoneOut()
    {
        // What the asker leans on, as for a GET and a POST (the kvmd's tests try those): whether a question
        // has gone out decides between "the device is not there" and "it may have been done".
        StandIn silent([](const Asked&) { return silence(); });
        QNetworkAccessManager network;
        QNetworkRequest request(QUrl(silent.address() + u"/redfish/v1/SessionService/Sessions/1"_s));
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QNetworkReply* gone = network.deleteResource(request);
        QSignalSpy goneWent(gone, &QNetworkReply::requestSent);
        const bool deleteSaid = goneWent.wait(3000);
        request.setRawHeader("Content-Type"_ba, "application/json"_ba);
        QNetworkReply* changed = network.sendCustomRequest(request, "PATCH"_ba, R"({"IndicatorLED":"Lit"})"_ba);
        QSignalSpy changedWent(changed, &QNetworkReply::requestSent);
        const bool patchSaid = changedWent.wait(3000);
        QVERIFY(deleteSaid);
        QVERIFY(patchSaid);
        QTRY_COMPARE(silent.asked.size(), 2);
        QCOMPARE(silent.asked.at(0).method, "DELETE"_ba);
        QCOMPARE(silent.asked.at(1).method, "PATCH"_ba);

        // A PATCH with nothing to send along, as a POST with nothing: Qt does not say it went, although it
        // did. So the asker puts no limit on such a question's being let in. (If this ever fails, Qt has
        // mended it.)
        request.setRawHeader("Content-Type"_ba, "application/octet-stream"_ba);
        QNetworkReply* bare = network.sendCustomRequest(request, "PATCH"_ba, QByteArray());
        QSignalSpy bareWent(bare, &QNetworkReply::requestSent);
        QVERIFY(!bareWent.wait(1500));
        QCOMPARE(silent.asked.size(), 3);
        QCOMPARE(silent.asked.at(2).method, "PATCH"_ba);
    }

    void aDeleteAndAPatchAreHeardAsSuch()
    {
        StandIn device([](const Asked&) { return Answered{204, {}}; });
        DeviceAsker asker(at(device.address()), quick(), inAHeader());
        QCOMPARE(answerTo(asker, asking("DELETE"_ba, u"/redfish/v1/SessionService/Sessions/7"_s)).value().status, 204);
        DeviceAsker::Ask light = asking("PATCH"_ba, u"/redfish/v1/Systems/Self"_s);
        light.body = R"({"IndicatorLED":"Lit"})"_ba;
        light.bodyIs = "application/json"_ba;
        QVERIFY(answerTo(asker, light));
        DeviceAsker::Ask shutDown = asking("POST"_ba, u"/redfish/v1/Systems/Self/Actions/ComputerSystem.Reset"_s);
        shutDown.body = R"({"ResetType":"GracefulShutdown"})"_ba;
        shutDown.bodyIs = "application/json"_ba;
        QVERIFY(answerTo(asker, shutDown));
        QVERIFY(answerTo(asker, asking("GET"_ba, u"/redfish/v1"_s)));
        QCOMPARE(device.asked.size(), 4);
        QCOMPARE(device.asked.at(0).method, "DELETE"_ba);
        QCOMPARE(device.asked.at(0).path, "/redfish/v1/SessionService/Sessions/7"_ba);
        QVERIFY(device.asked.at(0).body.isEmpty());
        QCOMPARE(device.asked.at(1).method, "PATCH"_ba);
        QCOMPARE(device.asked.at(1).path, "/redfish/v1/Systems/Self"_ba);
        QCOMPARE(device.asked.at(1).body, R"({"IndicatorLED":"Lit"})"_ba);
        QCOMPARE(device.asked.at(1).headers.value("content-type"), "application/json"_ba);
        QCOMPARE(device.asked.at(2).method, "POST"_ba);
        QCOMPARE(device.asked.at(2).body, R"({"ResetType":"GracefulShutdown"})"_ba);
        QCOMPARE(device.asked.at(3).method, "GET"_ba);
    }

    void aPathGoesAsItIsWritten()
    {
        // A place a controller points to is written with its escapes already: sent as it is, and not as
        // the place whose name has a "%" in it.
        StandIn device([](const Asked&) { return Answered{200, "{}"}; });
        DeviceAsker asker(at(device.address()), quick(), inAHeader());
        QVERIFY(answerTo(asker, asking("GET"_ba, u"/redfish/v1/Systems/a%20b/"_s)));
        QVERIFY(answerTo(asker, asking("DELETE"_ba, u"/redfish/v1/SessionService/Sessions/x%3Fy"_s)));
        QCOMPARE(device.asked.size(), 2);
        QCOMPARE(device.asked.at(0).path, "/redfish/v1/Systems/a%20b/"_ba);
        QCOMPARE(device.asked.at(1).path, "/redfish/v1/SessionService/Sessions/x%3Fy"_ba);
    }

    void aWayOfAskingThisProgramDoesNotUseIsRefusedBeforeAnythingGoes_data()
    {
        QTest::addColumn<QByteArray>("method");
        QTest::newRow("PUT") << "PUT"_ba;
        QTest::newRow("HEAD") << "HEAD"_ba;
        QTest::newRow("OPTIONS") << "OPTIONS"_ba;
        QTest::newRow("in small letters") << "delete"_ba;
        QTest::newRow("none") << QByteArray();
        QTest::newRow("a line of its own") << "GET /x HTTP/1.1\r\nX-A: b"_ba;
    }

    void aWayOfAskingThisProgramDoesNotUseIsRefusedBeforeAnythingGoes()
    {
        QFETCH(QByteArray, method);
        StandIn device([](const Asked&) { return Answered{200, "{}"}; });
        DeviceAsker asker(at(device.address()), quick(), inAHeader());
        const auto reply = answerTo(asker, asking(method, u"/redfish/v1"_s), "the-pass"_ba);
        QCOMPARE(codeOf(reply), std::string("invalid"));
        // (Given a moment, whatever had gone would have arrived.)
        QTest::qWait(300);
        QCOMPARE(device.asked.size(), 0);
        QCOMPARE(device.lines, 0);
    }

    void theSessionGoesInItsOwnHeaderAndNoCookieGoesWithIt()
    {
        StandIn device([](const Asked&) { return Answered{200, "{}"}; });
        DeviceAsker asker(at(device.address()), quick(), inAHeader());
        QVERIFY(answerTo(asker, asking("GET"_ba, u"/redfish/v1/Systems"_s), "AbC+/9="_ba));
        DeviceAsker::Ask gone = asking("DELETE"_ba, u"/redfish/v1/SessionService/Sessions/1"_s);
        QVERIFY(answerTo(asker, gone, "AbC+/9="_ba));
        // Without a session, none.
        QVERIFY(answerTo(asker, asking("GET"_ba, u"/redfish/v1"_s)));
        QCOMPARE(device.asked.size(), 3);
        for (const Asked& one : std::as_const(device.asked).first(2)) {
            QCOMPARE(one.headers.value("x-auth-token"), "AbC+/9="_ba);
            QVERIFY(!one.headers.contains("cookie"));
            QVERIFY(!one.headers.contains("authorization"));
        }
        QVERIFY(!device.asked.at(2).headers.contains("x-auth-token"));
        QVERIFY(!device.asked.at(2).headers.contains("cookie"));
    }

    void theSessionGoesInItsCookieAsBefore()
    {
        // A kvmd's and a JetKVM's: the cookie the family names, and no header of the controllers'.
        StandIn device([](const Asked&) { return Answered{200, "{}"}; });
        DeviceAsker asker(at(device.address()), quick(), "authToken"_ba);
        QVERIFY(answerTo(asker, asking("GET"_ba, u"/device"_s), "jet.1"_ba));
        QCOMPARE(device.asked.size(), 1);
        QCOMPARE(device.asked.at(0).headers.value("cookie"), "authToken=jet.1"_ba);
        QVERIFY(!device.asked.at(0).headers.contains("x-auth-token"));
    }

    void aWebPagesSessionGoesInItsCookiesAndTheHeaderBesideThem()
    {
        // A controller's own web page (AMI's): its cookies as a Cookie header says them, and its token in a header of
        // its own; handed over as two lines, each one that prints, or nothing goes.
        StandIn device([](const Asked&) { return Answered{200, "{}"}; });
        DeviceAsker asker(at(device.address()), quick(),
                          DeviceAsker::Carried{DeviceAsker::Carried::In::CookiesAndHeader, "X-CSRFTOKEN"_ba});
        QVERIFY(answerTo(asker, asking("GET"_ba, u"/api/kvm/token"_s), "QSESSIONID=ab12; other=cd\nt0k3n"_ba));
        QCOMPARE(device.asked.size(), 1);
        QCOMPARE(device.asked.at(0).headers.value("cookie"), "QSESSIONID=ab12; other=cd"_ba);
        QCOMPARE(device.asked.at(0).headers.value("x-csrftoken"), "t0k3n"_ba);
        const QList<QByteArray> none{"QSESSIONID=ab12"_ba, "QSESSIONID=ab12\n"_ba, "\nt0k3n"_ba,
                                     "QSESSIONID=ab12\nt0k\r3n"_ba, "QSESSIONID=a\tb\nt0k3n"_ba};
        for (const QByteArray& session : none) {
            const auto refused = answerTo(asker, asking("GET"_ba, u"/api/kvm/token"_s), session);
            QVERIFY(!refused);
            QCOMPARE(refused.error().code, std::string("invalid"));
        }
        QCOMPARE(device.asked.size(), 1);
    }

    void theHeadersAQuestionMaySendArrive()
    {
        StandIn device([](const Asked&) { return Answered{204, {}}; });
        DeviceAsker asker(at(device.address()), quick(), inAHeader());
        DeviceAsker::Ask light = asking("PATCH"_ba, u"/redfish/v1/Systems/Self"_s);
        light.body = R"({"IndicatorLED":"Off"})"_ba;
        light.bodyIs = "application/json"_ba;
        light.sent = {{"If-Match"_ba, R"(W/"1700000000")"_ba}, {"accept"_ba, "application/json"_ba}};
        QVERIFY(answerTo(asker, light, "the-pass"_ba));
        QCOMPARE(device.asked.size(), 1);
        QCOMPARE(device.asked.at(0).headers.value("if-match"), R"(W/"1700000000")"_ba);
        QCOMPARE(device.asked.at(0).headers.value("accept"), "application/json"_ba);
        QCOMPARE(device.asked.at(0).headers.value("x-auth-token"), "the-pass"_ba);
    }

    void noOtherHeaderGoesAndTheQuestionIsRefused_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QByteArray>("value");
        // What would sign this program on, or have a secret leave in a header of the caller's.
        QTest::newRow("Authorization") << "Authorization"_ba << "Basic YWRtaW46c2VjcmV0"_ba;
        QTest::newRow("authorization") << "authorization"_ba << "Basic YWRtaW46c2VjcmV0"_ba;
        QTest::newRow("Proxy-Authorization") << "Proxy-Authorization"_ba << "Basic YWRtaW46c2VjcmV0"_ba;
        QTest::newRow("Cookie") << "Cookie"_ba << "QSESSIONID=abc"_ba;
        QTest::newRow("the session's own") << "X-Auth-Token"_ba << "another"_ba;
        // What says where it goes, or what it is.
        QTest::newRow("Host") << "Host"_ba << "elsewhere.example"_ba;
        QTest::newRow("Content-Type") << "Content-Type"_ba << "text/plain"_ba;
        QTest::newRow("Content-Length") << "Content-Length"_ba << "0"_ba;
        QTest::newRow("any other") << "X-Something"_ba << "1"_ba;
        QTest::newRow("no name") << QByteArray() << "1"_ba;
        // One of the list, whose value would end the line and begin another.
        QTest::newRow("a line of its own") << "If-Match"_ba << "*\r\nAuthorization: Basic YWRtaW46c2VjcmV0"_ba;
        QTest::newRow("a line feed") << "If-Match"_ba << "*\nX-A: b"_ba;
        QTest::newRow("a nought") << "Accept"_ba << QByteArray("a\0b", 3);
        QTest::newRow("what does not print") << "If-Match"_ba << "\"\x7f\""_ba;
    }

    void noOtherHeaderGoesAndTheQuestionIsRefused()
    {
        QFETCH(QByteArray, name);
        QFETCH(QByteArray, value);
        StandIn device([](const Asked&) { return Answered{200, "{}"}; });
        DeviceAsker asker(at(device.address()), quick(), inAHeader());
        DeviceAsker::Ask question = asking("GET"_ba, u"/redfish/v1/Systems"_s);
        question.sent = {{"Accept"_ba, "application/json"_ba}, {name, value}};
        QCOMPARE(codeOf(answerTo(asker, question, "the-pass"_ba)), std::string("invalid"));
        QTest::qWait(300);
        QCOMPARE(device.asked.size(), 0);
        QCOMPARE(device.lines, 0);
    }

    void aHeaderWhoseCaseMattersComesBackAsSent()
    {
        // A controller's session, where to sign out of it, and the mark it puts on a state are all compared
        // as they were sent: in small letters each would be another.
        StandIn device([](const Asked&) {
            Answered in(201, R"({"Id":"AbC"})");
            in.more = {"X-Auth-Token:   AbC+/9xYz=  "_ba,
                       "Location: https://127.0.0.1/redfish/v1/SessionService/Sessions/AbC"_ba,
                       R"(ETag: W/"Mark-1")"_ba};
            return in;
        });
        DeviceAsker asker(at(device.address()), quick(), inAHeader());
        DeviceAsker::Ask login = asking("POST"_ba, u"/redfish/v1/SessionService/Sessions"_s);
        login.body = R"({"UserName":"admin","Password":"secret"})"_ba;
        login.bodyIs = "application/json"_ba;
        login.headersAsSent = {"X-Auth-Token"_ba, "Location"_ba, "ETag"_ba, "Not-There"_ba};
        login.headers = {"ETag"_ba};
        const auto reply = answerTo(asker, login);
        QVERIFY(reply);
        QCOMPARE(reply.value().status, 201);
        QCOMPARE(reply.value().headersAsSent.value("x-auth-token"), "AbC+/9xYz="_ba);
        QCOMPARE(reply.value().headersAsSent.value("location"),
                 "https://127.0.0.1/redfish/v1/SessionService/Sessions/AbC"_ba);
        QCOMPARE(reply.value().headersAsSent.value("etag"), R"(W/"Mark-1")"_ba);
        QVERIFY(reply.value().headersAsSent.value("not-there").isEmpty());
        // And those asked for as before: in small letters, as the kvmd's and the JetKVM's readers take them.
        QCOMPARE(reply.value().headers.value("etag"), R"(w/"mark-1")"_ba);
        QVERIFY(!reply.value().headers.contains("x-auth-token"));
    }

    void aQuestionThatIsNeverAnsweredMayHaveBeenDone_data()
    {
        QTest::addColumn<QByteArray>("method");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("DELETE") << "DELETE"_ba << QByteArray();
        QTest::newRow("PATCH") << "PATCH"_ba << R"({"IndicatorLED":"Lit"})"_ba;
        // (Qt does not say when this one went: it is not given up on as one that was never let in.)
        QTest::newRow("PATCH with nothing to send") << "PATCH"_ba << QByteArray();
        QTest::newRow("POST") << "POST"_ba << R"({"ResetType":"On"})"_ba;
    }

    void aQuestionThatIsNeverAnsweredMayHaveBeenDone()
    {
        QFETCH(QByteArray, method);
        QFETCH(QByteArray, body);
        StandIn device([](const Asked&) { return silence(); });
        // To be let in it has a short while, and to answer as long as a question is given: told apart.
        DeviceAsker asker(at(device.address()), DeviceAsker::Times{300, 1500, 300}, inAHeader());
        DeviceAsker::Ask question = asking(method, u"/redfish/v1/SessionService/Sessions/3"_s);
        question.body = body;
        question.bodyIs = "application/json"_ba;
        const auto reply = answerTo(asker, question, "the-pass"_ba);
        QCOMPARE(codeOf(reply), std::string("timeout"));
        QCOMPARE(device.asked.size(), 1);
        QCOMPARE(device.asked.at(0).method, method);
    }

    void whatQtAsksAgainByItselfWhenTheLineFalls_data()
    {
        QTest::addColumn<QByteArray>("method");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("kept");
        QTest::addColumn<bool>("overTls");
        QTest::addColumn<int>("times");
        // The device takes the question and hangs up without a word, on a new line or on one kept from a
        // question before (a controller closes one that sat unused). Qt asks a GET once more by itself, and
        // nothing else (tried): a DELETE, a POST and a PATCH (which goes as a way of Qt's own) go once. A
        // power button is not pressed twice, nor a light switched.
        // (The same over HTTPS: a power button of a server's controller goes over it.)
        for (const bool overTls : {false, true}) {
            for (const bool kept : {false, true}) {
                const QByteArray line =
                    QByteArray(kept ? "on a kept line" : "on a new line") + (overTls ? " over TLS" : "");
                QTest::addRow("GET %s", line.constData()) << "GET"_ba << QByteArray() << kept << overTls << 2;
                QTest::addRow("DELETE %s", line.constData()) << "DELETE"_ba << QByteArray() << kept << overTls << 1;
                QTest::addRow("POST %s", line.constData())
                    << "POST"_ba << R"({"ResetType":"On"})"_ba << kept << overTls << 1;
                QTest::addRow("PATCH %s", line.constData())
                    << "PATCH"_ba << R"({"IndicatorLED":"Lit"})"_ba << kept << overTls << 1;
            }
        }
    }

    void whatQtAsksAgainByItselfWhenTheLineFalls()
    {
        QFETCH(QByteArray, method);
        QFETCH(QByteArray, body);
        QFETCH(bool, kept);
        QFETCH(bool, overTls);
        QFETCH(int, times);
        const auto answer = [](const Asked& asked) {
            if (asked.path == "/redfish/v1") {
                return Answered{200, "{}"};
            }
            Answered gone;
            gone.hangUp = true;
            return gone;
        };
        const auto device =
            overTls ? std::make_unique<StandIn>(answer, tls("first")) : std::make_unique<StandIn>(answer);
        device->keepsLines = kept;
        DeviceAsker::Where where = at(device->address());
        if (overTls) {
            where.knownByCertificate = true;
            where.certificate = StandIn::certificateIn(tls("first").certificateFile).digest(QCryptographicHash::Sha256);
        }
        // (Room for the lines to be let in, and for the quiet while Qt makes its second one: what is looked at is
        // what is asked and what it is called, not how soon. Over TLS on the slow cores beside a build, a GET's two
        // lines took longer than 4 s to be let in, and longer than 7 s in all: "unreachable" and "timeout".)
        DeviceAsker asker(where, DeviceAsker::Times{20000, 20000, 20000}, inAHeader());
        if (kept) {
            QVERIFY(answerTo(asker, asking("GET"_ba, u"/redfish/v1"_s), {}, 90000));
        }
        DeviceAsker::Ask question = asking(method, u"/redfish/v1/Systems/Self"_s);
        question.body = body;
        question.bodyIs = "application/json"_ba;
        QCOMPARE(codeOf(answerTo(asker, question, "the-pass"_ba, 90000)), std::string("dropped"));
        QTest::qWait(300);
        QCOMPARE(device->count("/redfish/v1/Systems/Self"), times);
    }

    void aPatchOrADeleteIsNeverToldAsNotThereOnAGuess_data()
    {
        QTest::addColumn<QByteArray>("method");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("code");
        // Let in only after longer than being let in is given, which Qt's word that a question went comes a
        // moment after: a look is given up on as "unreachable" (it is a look); a change or a goodbye is
        // waited for, whatever its caller said of it, and is not told as not there while it may have gone.
        QTest::newRow("GET") << "GET"_ba << QByteArray() << "unreachable"_ba;
        QTest::newRow("PATCH") << "PATCH"_ba << R"({"IndicatorLED":"Lit"})"_ba << "ok"_ba;
        QTest::newRow("DELETE") << "DELETE"_ba << QByteArray() << "ok"_ba;
    }

    void aPatchOrADeleteIsNeverToldAsNotThereOnAGuess()
    {
        QFETCH(QByteArray, method);
        QFETCH(QByteArray, body);
        QFETCH(QByteArray, code);
        StandIn device([](const Asked&) { return Answered{204, {}}; }, tls("first"));
        QVERIFY(device.letInOnlyAfter(1200));
        DeviceAsker::Where where = at(device.address());
        where.knownByCertificate = true;
        where.certificate = StandIn::certificateIn(tls("first").certificateFile).digest(QCryptographicHash::Sha256);
        DeviceAsker asker(where, DeviceAsker::Times{400, 3000, 3000}, inAHeader());
        DeviceAsker::Ask question = asking(method, u"/redfish/v1/Systems/Self"_s);
        question.body = body;
        question.bodyIs = "application/json"_ba;
        QCOMPARE(QByteArray::fromStdString(codeOf(answerTo(asker, question, "the-pass"_ba))), code);
    }

    void aLineThatWasLetInIsNeverToldAsNotThere_data()
    {
        QTest::addColumn<int>("afterMs");
        QTest::addColumn<QByteArray>("code");
        // Over HTTPS the two agreed on how to speak: the line was let in, whatever Qt says (or does not say) of the
        // question going out. Answered late: answered. Never answered: it may have been done, not "not there".
        QTest::newRow("answered after being let in is waited for") << 700 << "ok"_ba;
        QTest::newRow("never answered") << -1 << "timeout"_ba;
    }

    void aLineThatWasLetInIsNeverToldAsNotThere()
    {
        QFETCH(int, afterMs);
        QFETCH(QByteArray, code);
        StandIn device(
            [afterMs](const Asked&) {
                Answered late(200, "{}");
                late.afterMs = std::max(afterMs, 0);
                late.never = afterMs < 0;
                return late;
            },
            tls("first"));
        DeviceAsker::Where where = at(device.address());
        where.knownByCertificate = true;
        where.certificate = StandIn::certificateIn(tls("first").certificateFile).digest(QCryptographicHash::Sha256);
        DeviceAsker asker(where, DeviceAsker::Times{300, 1200, 300}, inAHeader());
        DeviceAsker::Ask login = asking("POST"_ba, u"/redfish/v1/SessionService/Sessions"_s);
        login.body = R"({"UserName":"admin","Password":"secret"})"_ba;
        login.bodyIs = "application/json"_ba;
        const base::Result<QNetworkReply*> out = asker.sendOut(login, {});
        QVERIFY(out);
        UnreportedSend unreported;
        out.value()->installEventFilter(&unreported);
        QSignalSpy over(out.value(), &QNetworkReply::finished);
        QVERIFY(over.wait(10000));
        const std::optional<base::Error> why = DeviceAsker::whyNoAnswer(out.value());
        QCOMPARE(why ? QByteArray::fromStdString(why->code) : "ok"_ba, code);
        out.value()->deleteLater();
    }

    void aQuestionToNobodyIsUnreachable_data()
    {
        QTest::addColumn<QByteArray>("method");
        QTest::addColumn<QByteArray>("body");
        // (Not a PATCH with nothing to send: of that one Qt does not say when it went, so it is not given up
        // on as one that was never let in, and Windows takes longer to say that nobody is there than such a
        // question is waited for. It is told as one that may have been done, as a POST with nothing is.)
        QTest::newRow("DELETE") << "DELETE"_ba << QByteArray();
        QTest::newRow("PATCH") << "PATCH"_ba << R"({"IndicatorLED":"Lit"})"_ba;
    }

    void aQuestionToNobodyIsUnreachable()
    {
        QFETCH(QByteArray, method);
        QFETCH(QByteArray, body);
        const NobodyThere nobody;
        DeviceAsker asker(at(nobody.address()), quick(), inAHeader());
        DeviceAsker::Ask question = asking(method, u"/redfish/v1/SessionService/Sessions/3"_s);
        question.body = body;
        question.bodyIs = "application/json"_ba;
        QCOMPARE(codeOf(answerTo(asker, question, "the-pass"_ba)), std::string("unreachable"));
    }
};

QTEST_GUILESS_MAIN(TestDeviceAsker)
#include "test_device_asker.moc"
