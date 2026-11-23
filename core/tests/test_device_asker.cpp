#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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
#include <QProcess>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStringList>
#include <QTcpSocket>
#include <QTest>
#include <QThread>

#include <qkvm/core/device_asker.h>
#include <qkvm/core/network_end.h>

#include "heard_of_threads.h"
#include "signer_far_away.h"
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

    void aManagerEndedOffThisThreadEndsItsQuestionsHereAndTheRestElsewhere()
    {
        // (network_end.h.) Its questions are this thread's, and go here and now, as its own end has them go; the rest
        // of it ends on a thread kept for that.
        StandIn silent([](const Asked&) { return silence(); });
        auto* network = new QNetworkAccessManager; // NOLINT(cppcoreguidelines-owning-memory): ended below
        QNetworkReply* question = network->get(QNetworkRequest(QUrl(silent.address() + u"/api/auth/check"_s)));
        QTRY_COMPARE(silent.asked.size(), 1);
        std::atomic<QThread*> questionOn{nullptr};
        std::atomic<QThread*> managerOn{nullptr};
        connect(question, &QObject::destroyed, [&questionOn] { questionOn = QThread::currentThread(); });
        connect(network, &QObject::destroyed, [&managerOn] { managerOn = QThread::currentThread(); });
        qkvm::core::endOffThisThread(network);
        QCOMPARE(questionOn.load(), QThread::currentThread());
        QTRY_VERIFY(managerOn.load() != nullptr);
        QVERIFY(managerOn.load() != QThread::currentThread());
    }

    // (kept_with.h, as test_core_server_client has it of a call to a server.) The question, and whoever is to hear its
    // answer, end on this thread wherever Qt lets go of what listens to its reply: played by another thread inside a
    // signal of the reply, which holds its connections while the asker ends with the question out; Qt lets go of them
    // there, as the signal ends.
    void whatAQuestionKeepsEndsOnThisThreadWhereverQtLetsGoOfItsReply()
    {
        StandIn silent([](const Asked&) { return silence(); });
        // (Times that do not end it meanwhile.)
        auto asker = std::make_unique<DeviceAsker>(at(silent.address()), DeviceAsker::Times{60'000, 60'000, 60'000},
                                                   "auth_token"_ba);
        std::atomic<QThread*> hearerEndedOn{nullptr};
        std::atomic<QThread*> questionEndedOn{nullptr};
        QNetworkReply* reply = nullptr;
        {
            // (Whoever is to hear the answer holds the first, and the question the second; each says where it ended.)
            const std::shared_ptr<void> hearer(nullptr,
                                               [&hearerEndedOn](void*) { hearerEndedOn = QThread::currentThread(); });
            const std::shared_ptr<void> ofTheQuestion(
                nullptr, [&questionEndedOn](void*) { questionEndedOn = QThread::currentThread(); });
            DeviceAsker::Ask question = asking("GET"_ba, u"/api/info"_s);
            question.handed = [&reply, ofTheQuestion](QNetworkReply* handed) { reply = handed; };
            asker->send(question, {}, [hearer](const base::Result<DeviceAsker::Reply>&) {});
        }
        QTRY_COMPARE(silent.asked.size(), 1);
        QVERIFY(reply != nullptr);
        const qkvm::core::testing::HeardOfThreads heard;
        QSemaphore inside;
        QSemaphore ended;
        std::thread elsewhere([reply, &inside, &ended] {
            const QObject here; // (of this thread)
            QObject::connect(
                reply, &QIODevice::channelBytesWritten, &here,
                [&inside, &ended] {
                    inside.release();
                    ended.acquire();
                },
                Qt::DirectConnection);
            emit reply->channelBytesWritten(0, 0);
        });
        if (!inside.tryAcquire(1, 10'000)) {
            ended.release();
            elsewhere.join();
            QFAIL("The other thread never came inside the reply's signal.");
        }
        asker.reset();
        ended.release();
        elsewhere.join();
        QVERIFY2(hearerEndedOn.load() == QThread::currentThread(),
                 "Whoever was to hear the answer ended on another thread.");
        QVERIFY2(questionEndedOn.load() == QThread::currentThread(), "The question ended on another thread.");
        const QStringList said = qkvm::core::testing::HeardOfThreads::said();
        QVERIFY2(said.isEmpty(), qPrintable(said.join(u" | "_s)));
    }

    void aFetchAtASignersPlaceIsThisProgramsOnlyWhereThisProgramMadeIt()
    {
        // (signer_far_away.h.) What a stall case waits for is a fetch of this program's, on the line it judges.
        // Windows' LSASS fetches as well, on behalf of the test's server, and a second run of the same program at once
        // may fetch at the same place: another program's is told at once that there is nothing there, and is not
        // counted. Here Windows' own curl is the other program.
        const qkvm::core::testing::SignerFarAway far(qkvm::core::testing::stallsOfTheAsker);
        QVERIFY2(far.isListening(), qPrintable(far.said()));
        const QString curl = qEnvironmentVariable("SystemRoot") + u"/System32/curl.exe"_s;
        QVERIFY2(QFile::exists(curl), qPrintable(curl));
        QProcess other;
        other.start(curl, {u"--silent"_s, u"--max-time"_s, u"20"_s, u"--output"_s, u"NUL"_s, u"--write-out"_s,
                           u"%{http_code}"_s, u"http://127.0.0.1:%1/signer.crt"_s.arg(far.port())});
        QTRY_VERIFY_WITH_TIMEOUT(other.state() == QProcess::NotRunning, 30'000);
        QCOMPARE(other.readAllStandardOutput(), "404"_ba);
        QCOMPARE(far.asked(), 0);
        QTcpSocket mine;
        mine.connectToHost(QHostAddress::LocalHost, far.port());
        QTRY_COMPARE(far.asked(), 1);
        QCOMPARE(far.waitedOn(), 1);
        qInfo("%s", qPrintable(far.said()));
    }

    void anAskerThatEndsWhileWindowsFetchesASignerDoesNotHoldTheInterfaceThread()
    {
        // (The LOW of beta 2's review.) Every manager of Qt's for HTTP speaks on a thread of its own, and its end
        // waits up to five seconds for that thread. An asker has one (a driver's, and one for each sign-in of a
        // kvmd's or a JetKVM's), and ends with its driver or its sign-in. Where a line's certificate is being judged
        // then, and Windows fetches its signer from an address that does not answer (an AIA fetch on a PC without
        // internet), that thread is held: the interface thread was held with it, as the asker ended. It is not
        // (network_end.h).
        // (This program's own certificate and ports: signer_far_away.h.)
        const auto& stalls = qkvm::core::testing::stallsOfTheAsker;
        const qkvm::core::testing::SignerFarAway far(stalls);
        QVERIFY2(far.isListening(), qPrintable(far.said()));
        const qkvm::core::testing::TlsDoorElsewhere door(tls(stalls.name));
        QVERIFY(door.speaksWindowsOwnTls());
        DeviceAsker::Where where;
        where.address = QUrl(door.address());
        auto asker = std::make_unique<DeviceAsker>(where, DeviceAsker::Times{30'000, 30'000, 30'000}, "auth_token"_ba);
        DeviceAsker::Ask look;
        look.path = u"/api/auth/check"_s;
        look.largest = 4096;
        QVERIFY(asker->sendOut(look, {}).ok());
        // Its line's certificate judged: Windows fetches the signer from where the certificate says, for this program
        // here, and waits on it as the asker ends.
        QVERIFY2(QTest::qWaitFor([&far] { return far.asked() > 0; }, 60'000),
                 qPrintable(u"Windows did not fetch the signer here for this program: "_s + far.said()));
        QVERIFY2(far.waitedOn() > 0, qPrintable(far.said()));
        const qkvm::core::testing::Stillness still;
        asker.reset();
        QTest::qWait(6000);
        QVERIFY2(still.longestMs() < 1000,
                 qPrintable(
                     u"The interface thread was held still for %1 ms (%2)."_s.arg(still.longestMs()).arg(far.said())));
        // (And Windows still waited on that fetch all through: what was measured is an end while its thread was held.)
        QVERIFY2(far.waitedOn() > 0, qPrintable(far.said()));
        qInfo("%s", qPrintable(far.said()));
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
        // (A PUT is one since wave 2: a Proxmox guest's settings are changed with it.)
        QTest::newRow("HEAD") << "HEAD"_ba;
        QTest::newRow("put in small letters") << "put"_ba;
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

    // The words of an answer's status line come with it, as they came and cut to their bound: where a Proxmox says why
    // it refused (review c9b-2, H1), and nowhere else.
    void theWordsOfTheStatusLineComeWithTheAnswer()
    {
        QByteArray words = "Permission check failed (/vms/100, VM.Config.CDROM)";
        StandIn device([&words](const Asked&) {
            Answered no(403, R"({"data":null})");
            no.reason = words;
            return no;
        });
        DeviceAsker asker(at(device.address()), quick(), inAHeader());
        auto reply = answerTo(asker, asking("GET"_ba, u"/api2/json/nodes/pve1/qemu/100/config"_s));
        QVERIFY(reply);
        QCOMPARE(reply.value().status, 403);
        QCOMPARE(reply.value().reason, words);
        QCOMPARE(reply.value().body, R"({"data":null})"_ba);
        // As long as Qt reads them (its own bound, the same as this one: a longer status line is no answer to it, and
        // nothing of it is said).
        words = QByteArray(DeviceAsker::mostReasonBytes, 'x');
        reply = answerTo(asker, asking("GET"_ba, u"/api2/json/version"_s));
        QVERIFY(reply);
        QCOMPARE(reply.value().reason, words);
        words = QByteArray(DeviceAsker::mostReasonBytes + 1, 'x');
        QCOMPARE(codeOf(answerTo(asker, asking("GET"_ba, u"/api2/json/version"_s))), std::string("dropped"));
        words = "";
        reply = answerTo(asker, asking("GET"_ba, u"/api2/json/version"_s));
        QVERIFY(reply);
        QVERIFY(reply.value().reason.isEmpty());
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
        QTest::newRow("PUT") << "PUT"_ba << "ide2=none%2Cmedia%3Dcdrom"_ba;
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
        // power button is not pressed twice, nor a light switched. A PUT is asked once more, as a GET is (HTTP
        // holds it to be idempotent, and Qt takes it so: seen): only what a second one cannot do twice goes as a PUT
        // (a Proxmox guest's settings, by the digest of what was read for the change: proxmox_media.cpp).
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
                QTest::addRow("PUT %s", line.constData())
                    << "PUT"_ba << "ide2=none%2Cmedia%3Dcdrom"_ba << kept << overTls << 2;
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

    void aQuestionThatGoesOnlyOnItsLineIsStoppedWhereANewOneIsMadeForIt_data()
    {
        QTest::addColumn<bool>("silently");
        QTest::newRow("the line closed without a word") << true;
        QTest::newRow("the line said to be closed") << false;
    }

    void aQuestionThatGoesOnlyOnItsLineIsStoppedWhereANewOneIsMadeForIt()
    {
        // (Outside review of 0.2.0, area 5, MEDIUM 5.4.) Over TLS a question that goes only on the line the one before
        // it was answered on (Ask::onItsLine, a sign-in's password) is stopped as the two agree how to speak on a line
        // Qt made anew for it, before a byte of it goes; on the line kept, it goes. An answer that says its line ends
        // with it says so (Reply::lineEnds), one that keeps it does not.
        QFETCH(bool, silently);
        const auto answer = [](const Asked& asked) { return Answered{asked.method == "POST" ? 201 : 200, "{}"}; };
        StandIn device(answer, tls("first"));
        DeviceAsker::Where where = at(device.address());
        where.knownByCertificate = true;
        where.certificate = StandIn::certificateIn(tls("first").certificateFile).digest(QCryptographicHash::Sha256);
        DeviceAsker::Times times{20000, 20000, 20000}; // (TLS handshakes on a busy PC)
        DeviceAsker::Ask post = asking("POST"_ba, u"/redfish/v1/SessionService/Sessions"_s);
        post.body = "{\"Password\": \"the-password\"}"; // (a plain literal: a raw one hides the class from AutoMoc)
        post.bodyIs = "application/json"_ba;
        post.doesSomething = true;
        post.onItsLine = true;
        {
            // Kept: it goes on the line the root was read on.
            device.keepsLines = true;
            DeviceAsker asker(where, times, inAHeader());
            const auto root = answerTo(asker, asking("GET"_ba, u"/redfish/v1/"_s), {}, 90000);
            QVERIFY(root);
            QVERIFY(!root.value().lineEnds);
            QVERIFY(answerTo(asker, post, {}, 90000));
            QCOMPARE(device.lines, 1);
            QCOMPARE(device.count("/redfish/v1/SessionService"), 1);
        }
        device.keepsLines = false;
        device.closesSilently = silently;
        DeviceAsker asker(where, times, inAHeader());
        const auto root = answerTo(asker, asking("GET"_ba, u"/redfish/v1/"_s), {}, 90000);
        QVERIFY(root);
        QCOMPARE(root.value().lineEnds, !silently);
        const int before = device.lines;
        const auto sent = answerTo(asker, post, {}, 90000);
        QCOMPARE(codeOf(sent), std::string(DeviceAsker::lineMadeAnewCode));
        QTest::qWait(300);
        QVERIFY(device.lines <= before + 1); // (a new line made for it, and nothing came on it)
        QCOMPARE(device.count("/redfish/v1/SessionService"), 1);
        for (const Asked& one : std::as_const(device.asked)) {
            QVERIFY(!one.body.contains("the-password") || one.line == 1);
        }
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
        QTest::newRow("PUT") << "PUT"_ba << "ide2=none%2Cmedia%3Dcdrom"_ba << "ok"_ba;
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
        QTest::newRow("PUT") << "PUT"_ba << "ide2=none%2Cmedia%3Dcdrom"_ba;
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

    // Held to one address (onlyTo: one its name was found at), every question goes there, whatever Windows finds the
    // name at, with the name and port of its address in its Host header; over HTTPS held to the certificate it is
    // known by as ever. An address no address of the web is written with is refused, and nothing changes.
    void aQuestionHeldToOneAddressGoesThereByItsName_data()
    {
        QTest::addColumn<bool>("https");
        QTest::newRow("plain HTTP") << false;
        QTest::newRow("HTTPS, known by its certificate") << true;
    }
    void aQuestionHeldToOneAddressGoesThereByItsName()
    {
        QFETCH(bool, https);
        const QHostAddress there(u"127.0.0.3"_s);
        const auto answer = [](const Asked&) { return Answered(200, "{}"); };
        const std::unique_ptr<StandIn> device =
            https ? std::make_unique<StandIn>(answer, tls("first")) : std::make_unique<StandIn>(answer);
        QVERIFY(device->alsoAt(there));
        const quint16 port = device->serverPort();
        DeviceAsker::Where where =
            at(QStringLiteral("%1://localhost:%2").arg(https ? u"https"_s : u"http"_s).arg(port));
        if (https) {
            where.knownByCertificate = true;
            where.certificate = StandIn::certificateIn(tls("first").certificateFile).digest(QCryptographicHash::Sha256);
        }
        DeviceAsker asker(where, DeviceAsker::Times{5000, 5000, 1500}, "authToken"_ba);
        QVERIFY(!asker.onlyTo(QHostAddress(u"fe80::1%1"_s)));
        QVERIFY(!asker.onlyTo(QHostAddress()));
        QVERIFY(asker.onlyTo(there));
        for (int twice = 0; twice < 2; ++twice) {
            const base::Result<DeviceAsker::Reply> reply =
                answerTo(asker, asking("GET"_ba, u"/device"_s), "the-session"_ba);
            QVERIFY2(reply, reply ? "" : reply.error().message.c_str());
        }
        QCOMPARE(device->asked.size(), qsizetype{2});
        for (const Asked& one : std::as_const(device->asked)) {
            QCOMPARE(one.at, there);
            QCOMPARE(one.headers.value("host"), "localhost:" + QByteArray::number(port));
            QCOMPARE(one.headers.value("cookie"), "authToken=the-session"_ba);
        }
        // Not held, it goes by its name, as Windows finds it.
        DeviceAsker byName(where, DeviceAsker::Times{5000, 5000, 1500}, "authToken"_ba);
        QVERIFY(answerTo(byName, asking("GET"_ba, u"/device"_s)));
        QCOMPARE(device->asked.constLast().at, QHostAddress(QHostAddress::LocalHost));
        QCOMPARE(device->asked.constLast().headers.value("host"), "localhost:" + QByteArray::number(port));
    }
};

QTEST_GUILESS_MAIN(TestDeviceAsker)
#include "test_device_asker.moc"
