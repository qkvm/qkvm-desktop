#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>

#include <qkvm/core/server_client.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

namespace {

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

// Stands between the program and a server, and passes on what each says to the other. While it
// holds, whoever calls is let in and gets no further: the line to the server is made, and what the
// caller said meanwhile is passed on, only when the test lets it through.
class SlowWay : public QObject {
public:
    SlowWay()
    {
        m_door.listen(QHostAddress::LocalHost, 0);
        connect(&m_door, &QTcpServer::pendingConnectionAvailable, this, [this] { take(); });
    }

    QString address() const { return QStringLiteral("https://127.0.0.1:%1").arg(m_door.serverPort()); }

    // Lets through whoever was held up, and says how many of them were still waiting.
    int letThrough()
    {
        holds = false;
        int waiting = 0;
        for (const auto& way : m_ways) {
            if (!way->open && way->caller->state() == QAbstractSocket::ConnectedState) {
                goOn(*way);
                ++waiting;
            }
        }
        return waiting;
    }

    // The port of the server behind it, for whoever calls from now on.
    quint16 to = 0;
    bool holds = false;

private:
    struct Way {
        QTcpSocket* caller = nullptr; // the door's own
        QTcpSocket onward;
        bool open = false;
    };

    void goOn(Way& way) const
    {
        way.open = true;
        way.onward.connectToHost(QHostAddress::LocalHost, to);
        way.onward.write(way.caller->readAll());
    }

    void take()
    {
        while (QTcpSocket* caller = m_door.nextPendingConnection()) {
            Way& way = *m_ways.emplace_back(std::make_unique<Way>());
            way.caller = caller;
            connect(caller, &QTcpSocket::readyRead, &way.onward, [&way] {
                if (way.open) {
                    way.onward.write(way.caller->readAll());
                }
            });
            connect(&way.onward, &QTcpSocket::readyRead, caller, [&way] { way.caller->write(way.onward.readAll()); });
            connect(caller, &QTcpSocket::disconnected, &way.onward, [&way] { way.onward.disconnectFromHost(); });
            connect(&way.onward, &QTcpSocket::disconnected, caller, [caller] { caller->disconnectFromHost(); });
            if (!holds) {
                goOn(way);
            }
        }
    }

    QTcpServer m_door;
    std::vector<std::unique_ptr<Way>> m_ways;
};

} // namespace

// The calls to a server, against a stand-in that answers what each test tells it to.
class TestServerClient : public QObject {
    Q_OBJECT

private:
    static constexpr const char* signedInAs =
        R"({"ok": true, "user": {"id": "u1", "name": "anna", "role": "admin", "token": false}})";

    static Answered emptyWall(const Asked& /*asked*/) { return {200, R"({"interval": 1, "devices": []})"}; }

    // One of the two certificates tools/make_testdata.ps1 makes, with its key.
    static core::testing::Tls tls(const char* name)
    {
        const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
        return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
    }

    static QByteArray sha256Of(const char* name)
    {
        return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
    }

    // Asks, lets the program's loop run until the answer is there, and gives it. A call that is never
    // answered gives an error of its own.
    template <typename Answer>
    static base::Result<Answer> answerTo(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
    {
        QList<base::Result<Answer>> answers;
        ask([&](base::Result<Answer> answer) { answers.append(std::move(answer)); });
        if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 20000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return answers.takeFirst();
    }

    // What a QKVM says of itself; and a picture, as far as it has to be one here: how a JPEG begins.
    static constexpr const char* aQkvm = R"({"status": "ok", "version": "0.1.0", "api": ["v1"]})";
    static QByteArray aJpeg() { return QByteArray::fromHex("ffd8ffe000104a464946") + QByteArray(2000, 'x'); }

private slots:
    // M-D3: the server client goes straight to the server, as the live line does: no proxy the
    // application names, none Windows names (WPAD, PAC, the system's own), and so no proxy that
    // could ask Windows to sign on. 0.0.0.0 is an address Qt would send through a proxy (127.0.0.1
    // it never does); each control shows that it would, here. (Before: every call went to the proxy.)
    void serverCallsNeverGoThroughAProxy()
    {
        const auto before = QNetworkProxy::applicationProxy();
        const auto restore = qScopeGuard([&] {
            QNetworkProxyFactory::setApplicationProxyFactory(nullptr);
            QNetworkProxy::setApplicationProxy(before);
        });
        int proxyCalls = 0;
        StandIn proxy([&](const Asked&) {
            ++proxyCalls;
            return Answered{502, {}, "text/plain"};
        });
        const QNetworkProxy throughIt(QNetworkProxy::HttpProxy, u"127.0.0.1"_s, proxy.serverPort());
        const core::testing::NobodyThere nobody;
        const QString address = u"http://0.0.0.0:%1"_s.arg(nobody.port());
        const auto everyCall = [&] {
            core::ServerClient client;
            QVERIFY(client.setAddress(address).ok());
            client.setSession("synthetic-session-for-the-proxy-test");
            int answered = 0;
            client.askHealth([&](const auto&) { ++answered; });
            client.askSession([&](const auto&) { ++answered; });
            client.signIn(u"name"_s, u"synthetic password"_s, {}, false, [&](const auto&) { ++answered; });
            client.askWall([&](const auto&) { ++answered; });
            client.askStatus([&](const auto&) { ++answered; });
            client.askLive(u"a"_s, [&](const auto&) { ++answered; });
            client.exportWall(u"synthetic password"_s, {}, {u"devices"_s}, [&](const auto&) { ++answered; });
            client.readFile("{}", {}, [&](const auto&) { ++answered; });
            client.takeFile("{}", {}, {u"devices"_s}, [&](const auto&) { ++answered; });
            QTRY_COMPARE_WITH_TIMEOUT(answered, 9, 30000);
        };
        const auto plainCall = [&] {
            QNetworkAccessManager plain;
            std::unique_ptr<QNetworkReply> reply(plain.get(QNetworkRequest(QUrl(address + u"/healthz"_s))));
            QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 10000);
        };
        // A proxy the application names.
        QNetworkProxy::setApplicationProxy(throughIt);
        plainCall();
        QVERIFY2(proxyCalls > 0, "control: Qt sends 0.0.0.0 through the application's proxy");
        proxyCalls = 0;
        everyCall();
        QCOMPARE(proxyCalls, 0);
        // A proxy that is asked for each address, as Windows' own (WPAD, PAC) is.
        class Counting final : public QNetworkProxyFactory {
        public:
            explicit Counting(const QNetworkProxy& proxy) : m_proxy(proxy) {}
            QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery&) override
            {
                ++asked;
                return {m_proxy};
            }
            int asked = 0;

        private:
            QNetworkProxy m_proxy;
        };
        auto made = std::make_unique<Counting>(throughIt);
        Counting* factory = made.get();
        QNetworkProxyFactory::setApplicationProxyFactory(made.release()); // Qt takes it, and the guard above frees it
        plainCall();
        QVERIFY2(factory->asked > 0 && proxyCalls > 0, "control: Qt asks the factory, and goes its way");
        factory->asked = 0;
        proxyCalls = 0;
        everyCall();
        QCOMPARE(factory->asked, 0);
        QCOMPARE(proxyCalls, 0);
    }
    void serverPathPrefixesAreRefusedBeforeAnyRequest()
    {
        StandIn server(emptyWall);
        for (const auto& suffix : {u"/qkvm/"_s, u"/a%2Fb"_s, u"/./"_s}) {
            core::ServerClient client;
            QVERIFY(!client.setAddress(server.address() + suffix));
            QVERIFY(client.address().isEmpty());
        }
        QCOMPARE(server.asked.size(), 0);
    }

    void anAddressIsTakenAsTyped_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("is");
        QTest::newRow("a name and a port") << u"qkvm.lan:8787"_s << u"http://qkvm.lan:8787/"_s;
        QTest::newRow("numbers") << u" 192.168.1.10:8787 "_s << u"http://192.168.1.10:8787/"_s;
        QTest::newRow("with https") << u"https://qkvm.example"_s << u"https://qkvm.example/"_s;
        QTest::newRow("with a root fragment") << u"http://qkvm.lan:8787/?x=1#top"_s << u"http://qkvm.lan:8787/"_s;
    }

    void anAddressIsTakenAsTyped()
    {
        QFETCH(QString, typed);
        QFETCH(QString, is);
        core::ServerClient client;
        QVERIFY(client.setAddress(typed).ok());
        QCOMPARE(client.address().toString(), is);
    }

    void whatWasTypedWithHttpsStaysEncrypted()
    {
        core::ServerClient client;
        QVERIFY(client.setAddress(u"https://qkvm.example"_s).ok());
        QVERIFY(client.isEncrypted());
        QVERIFY(client.setAddress(u"qkvm.example"_s).ok());
        QVERIFY(!client.isEncrypted());
    }

    void whatIsNoAddressOfAServerIsRefused_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::newRow("nothing") << QString();
        QTest::newRow("spaces") << u"   "_s;
        QTest::newRow("a file") << u"file:///C:/Windows/win.ini"_s;
        QTest::newRow("another kind of server") << u"ftp://qkvm.lan"_s;
        QTest::newRow("a password in it") << u"http://admin:secret@qkvm.lan:8787"_s;
        QTest::newRow("no host") << u"http://"_s;
    }

    void whatIsNoAddressOfAServerIsRefused()
    {
        QFETCH(QString, typed);
        core::ServerClient client;
        const base::Result<void> set = client.setAddress(typed);
        QVERIFY(!set.ok());
        QCOMPARE(set.error().code, std::string("invalid"));
    }

    void whatAQkvmSaysOfItselfIsAskedWithoutTheSession()
    {
        // As a QKVM answers; and with it, what no QKVM sends: a session nobody asked for.
        StandIn server([](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{
                    200,
                    R"({"status": "ok", "version": "0.1.0", "api": ["v1"], "page": "3f2a9c", "locked": true})",
                    "application/json",
                    {"Set-Cookie: qkvm_session=handed-in-unasked; Path=/; HttpOnly"}};
            }
            return emptyWall(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        // Somebody stayed signed in to the server that was here last. Who answers now is not known
        // yet: the session does not go with this question.
        client.setSession("kept-one");
        const base::Result<core::Health> health =
            answerTo<core::Health>([&](const auto& done) { client.askHealth(done); });
        QVERIFY2(health.ok(), health.ok() ? "" : health.error().message.c_str());
        QCOMPARE(health.value().version, u"0.1.0"_s);
        QCOMPARE(health.value().apis, QStringList{u"v1"_s});
        QVERIFY(health.value().locked);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("GET"));
        QCOMPARE(server.asked[0].path, QByteArray("/healthz"));
        QVERIFY(!server.asked[0].headers.contains("cookie"));
        QVERIFY(!server.asked[0].headers.contains("authorization"));
        // The session is the one it was, whatever came with the answer, and goes with what is
        // asked next.
        QCOMPARE(client.session(), QByteArray("kept-one"));
        QVERIFY(answerTo<core::Wall>([&](const auto& done) { client.askWall(done); }).ok());
        QCOMPARE(server.asked[1].headers.value("cookie"), QByteArray("qkvm_session=kept-one"));
    }

    void whatAnswersAndIsNoQkvmIsSaidInOneWay_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<bool>("withoutALength");
        const QByteArray none;
        QTest::newRow("a page") << 200 << "<html>A router</html>"_ba << none << false;
        QTest::newRow("nothing at that place") << 404 << "<html>Not Found</html>"_ba << none << false;
        QTest::newRow("a refusal in a qKVM's own shape")
            << 404 << R"({"code": "not_found", "message": "As some server says"})"_ba << none << false;
        QTest::newRow("something that wants a sign-in first")
            << 401 << R"({"code": "unauthorized", "message": "Sign in first"})"_ba << none << false;
        QTest::newRow("a refusal written as this program writes its own findings")
            << 400 << R"({"code": "desktop:unreachable", "message": "As some server says"})"_ba << none << false;
        QTest::newRow("something that sends elsewhere")
            << 302 << QByteArray() << "Location: http://127.0.0.1:1/healthz"_ba << false;
        QTest::newRow("something that fell over") << 500 << "Internal Server Error"_ba << none << false;
        QTest::newRow("a qKVM from before its API had versions")
            << 200 << R"({"status": "ok", "version": "0.0.9", "locked": false})"_ba << none << false;
        QTest::newRow("something that is not ok")
            << 200 << R"({"status": "starting", "api": ["v1"]})"_ba << none << false;
        const QByteArray large = R"({"status": "ok", "api": ["v1"]})"_ba + QByteArray(core::largestHealthBytes, ' ');
        QTest::newRow("more than a qKVM says of itself") << 200 << large << none << false;
        QTest::newRow("more than that, without saying how much") << 200 << large + large << none << true;
    }

    void whatAnswersAndIsNoQkvmIsSaidInOneWay()
    {
        QFETCH(const int, status);
        QFETCH(const QByteArray, body);
        QFETCH(const QByteArray, header);
        QFETCH(const bool, withoutALength);
        StandIn server([&](const Asked&) {
            Answered answer{status, body, "application/json", header.isEmpty() ? QList<QByteArray>() : QList{header}};
            answer.streams = withoutALength;
            return answer;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("kept-one");
        const base::Result<core::Health> health =
            answerTo<core::Health>([&](const auto& done) { client.askHealth(done); });
        QVERIFY(!health.ok());
        // One word and one sentence for all of them, and none of what the other end said.
        QCOMPARE(health.error().code, std::string("bad_answer"));
        QCOMPARE(health.error().message, std::string("The server answered in a way this program does not understand."));
        // Asked once, followed nowhere, and nobody is signed out by it.
        QTest::qWait(200);
        QCOMPARE(server.asked.size(), 1);
        QVERIFY(!server.asked[0].headers.contains("cookie"));
        QCOMPARE(client.session(), QByteArray("kept-one"));
    }

    void aQkvmThatSpeaksOtherVersionsIsSaidWithTheOnesItSpeaks_data()
    {
        QTest::addColumn<QByteArray>("listed");
        QTest::addColumn<QString>("sentence");
        const QString ending = u", and this program speaks v1. One of the two has to be updated."_s;
        QTest::newRow("another one") << R"(["v2"])"_ba << u"This qKVM speaks v2 of its API"_s + ending;
        QTest::newRow("two others") << R"(["v2", "v3"])"_ba << u"This qKVM speaks v2 and v3 of its API"_s + ending;
        QTest::newRow("several others") << R"(["v2", "v3", "v10"])"_ba
                                        << u"This qKVM speaks v2, v3 and v10 of its API"_s + ending;
        QTest::newRow("none") << "[]"_ba << u"This qKVM names no version of its API"_s + ending;
        // What is not written as a version is not said after it, and is not taken for v1.
        QTest::newRow("none that can be read")
            << R"(["V1", 1, "v1.0", " v1", "<b>v1</b>"])"_ba << u"This qKVM names no version of its API"_s + ending;
    }

    void aQkvmThatSpeaksOtherVersionsIsSaidWithTheOnesItSpeaks()
    {
        QFETCH(const QByteArray, listed);
        QFETCH(const QString, sentence);
        StandIn server([&](const Asked&) {
            return Answered{200, R"({"status": "ok", "version": "3.0.0", "api": )" + listed + "}"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Health> health =
            answerTo<core::Health>([&](const auto& done) { client.askHealth(done); });
        QVERIFY(!health.ok());
        QCOMPARE(health.error().code, std::string(core::ServerClient::otherVersion));
        QCOMPARE(QString::fromStdString(health.error().message), sentence);
        // It answered, and nothing was asked of a machine: no "maybe it was done".
        QVERIFY(!client.mayHaveBeenDone(u"any"_s, health.error()));
    }

    void whatAServerSaysOfItselfIsNotAskedBeforeItsCertificateWasLookedAt()
    {
        StandIn server(
            [](const Asked&) { return Answered{200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"]})"}; },
            tls("first"));
        QVERIFY(server.isListening());
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Health> unknown =
            answerTo<core::Health>([&](const auto& done) { client.askHealth(done); });
        QVERIFY(!unknown.ok());
        QCOMPARE(unknown.error().code, std::string(core::ServerClient::certificateUnknown));
        QCOMPARE(client.shownCertificate(), sha256Of("first"));
        QCOMPARE(server.asked.size(), 0);

        // Known by the certificate it shows: asked, and answered.
        client.setKnownCertificate(sha256Of("first"));
        const base::Result<core::Health> health =
            answerTo<core::Health>([&](const auto& done) { client.askHealth(done); });
        QVERIFY2(health.ok(), health.ok() ? "" : health.error().message.c_str());
        QCOMPARE(server.asked.size(), 1);

        // Known by another: not asked.
        client.setKnownCertificate(sha256Of("second"));
        const base::Result<core::Health> changed =
            answerTo<core::Health>([&](const auto& done) { client.askHealth(done); });
        QVERIFY(!changed.ok());
        QCOMPARE(changed.error().code, std::string(core::ServerClient::certificateChanged));
        QCOMPARE(server.asked.size(), 1);
    }

    void aServerThatIsNotThereOrSaysNothingIsNotSaidToBeNoQkvm()
    {
        // A port nothing listens on.
        const core::testing::NobodyThere nobody;
        core::ServerClient client;
        QVERIFY(client.setAddress(nobody.address()).ok());
        const base::Result<core::Health> none =
            answerTo<core::Health>([&](const auto& done) { client.askHealth(done); });
        QVERIFY(!none.ok());
        QCOMPARE(none.error().code, std::string(core::ServerClient::unreachable));

        // One that takes the question and never answers: given up on after ten seconds, as every
        // ordinary question is.
        StandIn silent([](const Asked&) {
            Answered nothing;
            nothing.never = true;
            return nothing;
        });
        QVERIFY(client.setAddress(silent.address()).ok());
        const base::Result<core::Health> late =
            answerTo<core::Health>([&](const auto& done) { client.askHealth(done); });
        QVERIFY(!late.ok());
        QCOMPARE(late.error().code, std::string(core::ServerClient::unreachable));
        QCOMPARE(silent.asked.size(), 1);
    }

    void theSessionIsAsked()
    {
        StandIn server([](const Asked&) {
            return Answered{200, R"({"initialized": true, "authenticated": false, "user": null, "ask_name": true})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Session> answer =
            answerTo<core::Session>([&](const auto& done) { client.askSession(done); });
        QVERIFY(answer.ok());
        QVERIFY(answer.value().asksName);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("GET"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/session"));
    }

    void aSignInSendsWhatWasTypedAndKeepsTheSession()
    {
        StandIn server([](const Asked& asked) {
            if (asked.path == "/api/v1/login") {
                return Answered{
                    200, signedInAs, "application/json", {"Set-Cookie: qkvm_session=abc123; Path=/; HttpOnly"}};
            }
            return Answered{200, R"({"interval": 1, "devices": []})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());

        const base::Result<core::Account> account = answerTo<core::Account>(
            [&](const auto& done) { client.signIn(u"anna"_s, u"correct horse"_s, u"123456"_s, true, done); });
        QVERIFY(account.ok());
        QCOMPARE(account.value().name, u"anna"_s);
        QCOMPARE(account.value().role, u"admin"_s);

        const Asked& login = server.asked[0];
        QCOMPARE(login.method, QByteArray("POST"));
        // The server's guard against other sites' pages.
        QCOMPARE(login.headers.value("x-qkvm"), QByteArray("1"));
        const QJsonObject sent = QJsonDocument::fromJson(login.body).object();
        QCOMPARE(sent["name"_L1].toString(), u"anna"_s);
        QCOMPARE(sent["password"_L1].toString(), u"correct horse"_s);
        QCOMPARE(sent["code"_L1].toString(), u"123456"_s);
        QCOMPARE(sent["remember"_L1].toBool(), true);

        // The next call carries the session the sign-in was given.
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QCOMPARE(server.asked[1].headers.value("cookie"), QByteArray("qkvm_session=abc123"));
    }

    void aSessionIsNotCarriedToAnotherServer()
    {
        const auto signsIn = [](const Asked& asked) {
            if (asked.path == "/api/v1/login") {
                return Answered{
                    200, signedInAs, "application/json", {"Set-Cookie: qkvm_session=abc123; Path=/; HttpOnly"}};
            }
            return Answered{200, R"({"interval": 1, "devices": []})"};
        };
        StandIn first(signsIn);
        // On the same computer, at another port: to a cookie by itself that is the same place.
        StandIn second(signsIn);
        core::ServerClient client;
        QVERIFY(client.setAddress(first.address()).ok());
        QCOMPARE(client.session(), QByteArray());
        const base::Result<core::Account> account = answerTo<core::Account>(
            [&](const auto& done) { client.signIn(u"anna"_s, u"correct horse"_s, {}, false, done); });
        QVERIFY(account.ok());
        QCOMPARE(client.session(), QByteArray("abc123"));

        QVERIFY(client.setAddress(second.address()).ok());
        QCOMPARE(client.session(), QByteArray());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QCOMPARE(second.asked.size(), 1);
        QVERIFY(!second.asked[0].headers.contains("cookie"));

        // Naming the same server again is not naming another.
        QVERIFY(client.setAddress(second.address()).ok());
        client.setSession("kept-one");
        QVERIFY(client.setAddress(second.address()).ok());
        QCOMPARE(client.session(), QByteArray("kept-one"));
    }

    void aKeptSessionIsCarriedOn()
    {
        StandIn server([](const Asked&) { return Answered{200, R"({"interval": 1, "devices": []})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("kept-one");
        QCOMPARE(client.session(), QByteArray("kept-one"));
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=kept-one"));
    }

    void whoeverSignsOutIsSignedOutHereBeforeTheServerAnswers()
    {
        // A server that never answers the sign-out: the cable was pulled, say.
        StandIn server([](const Asked& asked) {
            Answered answer{200, R"({"interval": 1, "devices": []})"};
            answer.never = asked.path == "/api/v1/logout";
            return answer;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("the-old-one");
        client.signOut([](const base::Result<void>&) {});
        // Here nobody is signed in from this moment on.
        QCOMPARE(client.session(), QByteArray());
        // The question itself names the session that is to be ended.
        QTRY_COMPARE(server.count("/api/v1/logout"), 1);
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=the-old-one"));
        // And what is asked next carries none, also where the same server is named anew.
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QCOMPARE(server.asked.size(), 2);
        QVERIFY(!server.asked[1].headers.contains("cookie"));
    }

    void aSignOutThatIsAnsweredLateTakesNoLaterSessionAway()
    {
        // As a QKVM answers a sign-out: it takes its cookie back. Here that answer is on its way for
        // a while, and somebody has signed in again before it is there.
        StandIn server([](const Asked& asked) {
            if (asked.path == "/api/v1/logout") {
                Answered late{
                    200, R"({"ok": true})", "application/json", {"Set-Cookie: qkvm_session=\"\"; Max-Age=0; Path=/"}};
                late.afterMs = 400;
                return late;
            }
            if (asked.path == "/api/v1/login") {
                return Answered{
                    200, signedInAs, "application/json", {"Set-Cookie: qkvm_session=the-new-one; Path=/; HttpOnly"}};
            }
            return Answered{200, R"({"interval": 1, "devices": []})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("the-old-one");
        bool signedOut = false;
        client.signOut([&](const base::Result<void>& answer) { signedOut = answer.ok(); });
        const base::Result<core::Account> account = answerTo<core::Account>(
            [&](const auto& done) { client.signIn(u"anna"_s, u"correct horse"_s, {}, false, done); });
        QVERIFY(account.ok());
        QVERIFY(!signedOut);
        QCOMPARE(client.session(), QByteArray("the-new-one"));
        QTRY_VERIFY(signedOut);
        QCOMPARE(client.session(), QByteArray("the-new-one"));
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QCOMPARE(server.asked.constLast().headers.value("cookie"), QByteArray("qkvm_session=the-new-one"));
        // What the sign-out named was the session before it, and the sign-in named none.
        for (const Asked& asked : std::as_const(server.asked)) {
            if (asked.path == "/api/v1/logout") {
                QCOMPARE(asked.headers.value("cookie"), QByteArray("qkvm_session=the-old-one"));
            } else if (asked.path == "/api/v1/login") {
                QVERIFY(!asked.headers.contains("cookie"));
            }
        }
    }

    void aSignInThatIsAnsweredAfterAnotherServerWasNamedLeavesNoSession()
    {
        // The first server takes its time over the sign-in, and meanwhile another is named: on the
        // same computer at another port, which to a cookie by itself is the same place.
        StandIn first([](const Asked&) {
            Answered late{
                200, signedInAs, "application/json", {"Set-Cookie: qkvm_session=the-first-ones; Path=/; HttpOnly"}};
            late.afterMs = 300;
            return late;
        });
        StandIn second(emptyWall);
        core::ServerClient client;
        QVERIFY(client.setAddress(first.address()).ok());
        client.signIn(u"anna"_s, u"correct horse"_s, {}, false, [](const base::Result<core::Account>&) {});
        QTRY_COMPARE(first.asked.size(), 1);
        QVERIFY(client.setAddress(second.address()).ok());
        // Whether the first one's answer still comes or not: its session is nothing to the second.
        QTest::qWait(700);
        QCOMPARE(client.session(), QByteArray());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QCOMPARE(second.asked.size(), 1);
        QVERIFY(!second.asked[0].headers.contains("cookie"));
    }

    void aSignInThatIsAnsweredAfterASignOutLeavesNoSession()
    {
        StandIn server([](const Asked& asked) {
            if (asked.path == "/api/v1/login") {
                Answered late{
                    200, signedInAs, "application/json", {"Set-Cookie: qkvm_session=too-late; Path=/; HttpOnly"}};
                late.afterMs = 300;
                return late;
            }
            return Answered{200, R"({"ok": true})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        bool answered = false;
        client.signIn(u"anna"_s, u"correct horse"_s, {}, false,
                      [&](const base::Result<core::Account>& account) { answered = account.ok(); });
        QTRY_COMPARE(server.count("/api/v1/login"), 1);
        client.signOut([](const base::Result<void>&) {});
        // The sign-in's answer comes after that: whoever signed out stays signed out.
        QTRY_VERIFY(answered);
        QCOMPARE(client.session(), QByteArray());
    }

    void whereAProxyVouchesForSomebodyTheSessionQuestionSignsThemIn()
    {
        // As a QKVM answers behind a proxy it trusts: the question whether somebody is signed in
        // comes back with a session for whom the proxy named.
        StandIn server([](const Asked& asked) {
            if (asked.path == "/api/v1/session") {
                return Answered{200,
                                R"({"initialized": true, "authenticated": true, "ask_name": false,
                        "user": {"id": "u1", "name": "anna", "role": "admin", "token": false}})",
                                "application/json",
                                {"Set-Cookie: qkvm_session=vouched-for; Path=/; HttpOnly"}};
            }
            return Answered{200, R"({"interval": 1, "devices": []})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Session> session =
            answerTo<core::Session>([&](const auto& done) { client.askSession(done); });
        QVERIFY(session.ok());
        QCOMPARE(client.session(), QByteArray("vouched-for"));
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QCOMPARE(server.asked[1].headers.value("cookie"), QByteArray("qkvm_session=vouched-for"));
    }

    void whatAServerGivesForASessionAndIsNotOfTheShapeOfOneIsNotTaken()
    {
        StandIn server([](const Asked& asked) {
            if (asked.path == "/api/v1/login") {
                return Answered{200,
                                signedInAs,
                                "application/json",
                                {"Set-Cookie: qkvm_session=" + QByteArray(600, 'a') + "; Path=/; HttpOnly",
                                 "Set-Cookie: other=1; Path=/"}};
            }
            return Answered{200, R"({"interval": 1, "devices": []})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Account> account = answerTo<core::Account>(
            [&](const auto& done) { client.signIn(u"anna"_s, u"correct horse"_s, {}, false, done); });
        QVERIFY(account.ok());
        QCOMPARE(client.session(), QByteArray());
        // And nothing else a server hands out with its answers is carried back to it.
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QVERIFY(!server.asked[1].headers.contains("cookie"));
    }

    void whatIsNotOfTheShapeOfASessionIsNotSent_data()
    {
        QTest::addColumn<QByteArray>("kept");
        QTest::newRow("nothing") << QByteArray();
        QTest::newRow("a second cookie behind it") << QByteArray("abc; other=1");
        QTest::newRow("a line break and a header") << QByteArray("abc\r\nX-Other: 1");
        QTest::newRow("a space") << QByteArray("abc def");
        QTest::newRow("letters that are none") << QByteArray("abc\x01\xff");
        QTest::newRow("longer than a session is") << QByteArray(600, 'a');
    }

    void whatIsNotOfTheShapeOfASessionIsNotSent()
    {
        QFETCH(QByteArray, kept);
        StandIn server([](const Asked&) { return Answered{200, R"({"interval": 1, "devices": []})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession(kept);
        QCOMPARE(client.session(), QByteArray());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(wall.ok());
        QVERIFY(!server.asked[0].headers.contains("cookie"));
        QVERIFY(!server.asked[0].headers.contains("x-other"));
    }

    void anAddressIsWrittenAsItWouldBeTypedAgain_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("again");
        QTest::newRow("numbers and a port") << u"192.168.1.10:8787"_s << u"192.168.1.10:8787"_s;
        QTest::newRow("said to be http") << u"http://qkvm.lan:8787/"_s << u"qkvm.lan:8787"_s;
        QTest::newRow("https") << u"https://qkvm.example/"_s << u"https://qkvm.example"_s;
        QTest::newRow("https and a port") << u"https://qkvm.example:8443"_s << u"https://qkvm.example:8443"_s;
    }

    void anAddressIsWrittenAsItWouldBeTypedAgain()
    {
        QFETCH(QString, typed);
        QFETCH(QString, again);
        const base::Result<QUrl> address = core::ServerClient::addressFrom(typed);
        QVERIFY(address.ok());
        QCOMPARE(core::ServerClient::asTyped(address.value()), again);
        // And typed again, it is the same server.
        QCOMPARE(core::ServerClient::addressFrom(again).value(), address.value());
    }

    void aCertificateWindowsDoesNotKnowIsRefusedAndShown()
    {
        StandIn server(emptyWall, tls("first"));
        QVERIFY(server.isListening());
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        QVERIFY(client.isEncrypted());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string(core::ServerClient::certificateUnknown));
        // Nothing was asked of it: the certificate is looked at before a question goes out.
        QCOMPARE(server.asked.size(), 0);
        // And what it showed is there to be shown to whoever has to decide.
        QCOMPARE(client.shownCertificate(), sha256Of("first"));
        QCOMPARE(client.shownCertificate().size(), 32);
    }

    void theCertificateAServerIsKnownByIsTakenAndOnlyThat()
    {
        StandIn server(emptyWall, tls("first"));
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("kept-one");
        client.setKnownCertificate(sha256Of("first"));
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY2(wall.ok(), wall.ok() ? "" : wall.error().message.c_str());
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=kept-one"));

        // Known by another: refused as changed, with what it showed; and the session stays here.
        client.setKnownCertificate(sha256Of("second"));
        const base::Result<core::Wall> again = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, std::string(core::ServerClient::certificateChanged));
        QCOMPARE(client.shownCertificate(), sha256Of("first"));
        QCOMPARE(server.asked.size(), 1);
    }

    void aServerThatComesBackWithAnotherCertificateIsRefused()
    {
        // The same address, one certificate and then another: as after somebody put a server of
        // their own where the QKVM was.
        auto server = std::make_unique<StandIn>(emptyWall, tls("first"));
        const quint16 port = server->serverPort();
        core::ServerClient client;
        QVERIFY(client.setAddress(server->address()).ok());
        client.setKnownCertificate(sha256Of("first"));
        QVERIFY(answerTo<core::Wall>([&](const auto& done) { client.askWall(done); }).ok());
        server.reset();

        int askedOfTheOther = 0;
        QSslServer other;
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(StandIn::certificateIn(tls("second").certificateFile));
        QFile key(tls("second").keyFile);
        QVERIFY(key.open(QIODevice::ReadOnly));
        configuration.setPrivateKey(QSslKey(key.readAll(), QSsl::Rsa, QSsl::Pem));
        other.setSslConfiguration(configuration);
        if (!other.listen(QHostAddress::LocalHost, port)) {
            QSKIP("Windows has given the port to somebody else in the meantime.");
        }
        connect(&other, &QTcpServer::pendingConnectionAvailable, &other, [&] {
            while (QTcpSocket* line = other.nextPendingConnection()) {
                connect(line, &QTcpSocket::readyRead, line,
                        [&askedOfTheOther, line] { askedOfTheOther += line->readAll().isEmpty() ? 0 : 1; });
            }
        });
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string(core::ServerClient::certificateChanged));
        QCOMPARE(client.shownCertificate(), sha256Of("second"));
        QTest::qWait(200);
        QCOMPARE(askedOfTheOther, 0);
    }

    void theCertificateOneServerIsKnownByIsNotAnothers()
    {
        // Two servers that show the same certificate. What was said of the one is not said of the
        // other: whoever has to decide is asked about each.
        StandIn first(emptyWall, tls("first"));
        StandIn second(emptyWall, tls("first"));
        core::ServerClient client;
        QVERIFY(client.setAddress(first.address()).ok());
        client.setKnownCertificate(sha256Of("first"));
        QVERIFY(answerTo<core::Wall>([&](const auto& done) { client.askWall(done); }).ok());

        QVERIFY(client.setAddress(second.address()).ok());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string(core::ServerClient::certificateUnknown));
        QCOMPARE(second.asked.size(), 0);
    }

    void anotherCertificateOfTheSameSignerIsNotTheServer()
    {
        // A certificate that somebody else signed, whom Windows does not know. What Windows
        // objects to in it names the certificate itself (its signer is not sent along): so it is
        // one to be known by, and what is passed over for it holds for no other.
        auto server = std::make_unique<StandIn>(emptyWall, tls("twin-one"));
        core::ServerClient client;
        QVERIFY(client.setAddress(server->address()).ok());
        client.setKnownCertificate(sha256Of("twin-one"));
        QVERIFY(answerTo<core::Wall>([&](const auto& done) { client.askWall(done); }).ok());

        // Something else at its address, with another certificate of the same signer's, good for
        // the same name: that is not the server, and hears nothing.
        const quint16 port = server->serverPort();
        server.reset();
        StandIn other(emptyWall, tls("twin-two"), port);
        QVERIFY(other.isListening());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string(core::ServerClient::certificateChanged));
        QCOMPARE(client.shownCertificate(), sha256Of("twin-two"));
        QVERIFY(other.asked.isEmpty());
        // (Where a signer's own certificate is sent along, Windows' objection names the signer,
        // and such a certificate is not one to be known by at all: the client says so
        // (certificateSignerUnknown) and lets nothing through. The tests' own server sends none
        // along, so that is not tried here.)
    }

    void aCertificateWindowsTrustsIsNotTheOneAServerIsKnownByEither()
    {
        // Known by a certificate, a server is whatever shows that one and nothing else: what answers
        // at its address with another hears nothing, also where Windows trusts that other one for
        // the name.
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        StandIn server(emptyWall, tls("signed"));
        QVERIFY(server.isListening());
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("kept-one");
        client.setKnownCertificate(sha256Of("first"));
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string(core::ServerClient::certificateChanged));
        QCOMPARE(client.shownCertificate(), sha256Of("signed"));
        QCOMPARE(server.asked.size(), 0);

        // (Known by none, the same one is let in: it is one that Windows trusts, which is what
        // this was tried with.)
        client.setKnownCertificate({});
        const base::Result<core::Wall> trusted = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY2(trusted.ok(), trusted.ok() ? "" : trusted.error().message.c_str());
        QCOMPARE(server.asked.size(), 1);
    }

    void aLineThatComesUpWithNoQuestionWaitingCarriesNoneLater()
    {
        // Where the earlier shape of the look failed. Something else stands where the server was,
        // with a certificate of its own. It is met once, and refused. Then it is slow: the line
        // to it is still coming up when the question that wanted the line is given up, and comes
        // up after that, when no question is waiting that could look at it. What is asked next
        // finds that line there, and must not go out on it.
        StandIn server(emptyWall, tls("first"));
        StandIn other(emptyWall, tls("second"));
        QVERIFY(server.isListening());
        QVERIFY(other.isListening());
        SlowWay way;
        way.to = server.serverPort();
        core::ServerClient client;
        QVERIFY(client.setAddress(way.address()).ok());
        client.setSession("kept-one");
        client.setKnownCertificate(sha256Of("first"));
        QVERIFY(answerTo<core::Wall>([&](const auto& done) { client.askWall(done); }).ok());
        QCOMPARE(server.asked.size(), 1);

        way.to = other.serverPort();
        const base::Result<core::Wall> met = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!met.ok());
        QCOMPARE(met.error().code, std::string(core::ServerClient::certificateChanged));

        way.holds = true;
        const base::Result<core::Wall> givenUp = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!givenUp.ok());
        QCOMPARE(givenUp.error().code, std::string(core::ServerClient::unreachable));
        // (Qt has kept the line coming up though its question is gone. Should it stop doing that,
        // nothing is tried here any more, and this says so.)
        QCOMPARE(way.letThrough(), 1);
        QTest::qWait(1000); // (the line is up by now, where it comes up at all)

        const base::Result<core::Wall> next = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!next.ok());
        QCOMPARE(next.error().code, std::string(core::ServerClient::certificateChanged));
        QCOMPARE(client.shownCertificate(), sha256Of("second"));
        QTest::qWait(200);
        QCOMPARE(other.asked.size(), 0);
    }

    void aCertificateIsWrittenAsTheServerPrintsIt()
    {
        // The server prints the SHA-256 of the certificate as it goes over the wire: of what stands
        // between the two lines of its file, decoded. Worked out here by hand, it is the same.
        QFile file(tls("first").certificateFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray between;
        const QList<QByteArray> lines = file.readAll().split('\n');
        for (const QByteArray& line : lines) {
            between += line.startsWith("-----") ? QByteArray() : line.trimmed();
        }
        QCOMPARE(sha256Of("first"),
                 QCryptographicHash::hash(QByteArray::fromBase64(between), QCryptographicHash::Sha256));

        QCOMPARE(core::ServerClient::written(QByteArray::fromHex("00ab10ff")), u"00:AB:10:FF"_s);
        QCOMPARE(core::ServerClient::written(sha256Of("first")).size(), 32 * 3 - 1);
    }

    void aDisplayLinksKeyIsFoundWhereThereIsOne_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QByteArray>("key");
        const QByteArray key = "qkvm_" + QByteArray(43, 'k');
        const QString link = u"192.168.1.10:8787/#display&key="_s + QString::fromLatin1(key);
        QTest::newRow("a display link as the page makes it") << u"http://"_s + link << key;
        QTest::newRow("without the http") << link << key;
        QTest::newRow("with spaces around it") << u"  "_s + link + u"\n"_s << key;
        QTest::newRow("the key alone behind the cross")
            << u"https://qkvm.example/#key="_s + QString::fromLatin1(key) << key;
        QTest::newRow("an address") << u"192.168.1.10:8787"_s << QByteArray();
        QTest::newRow("a wall screen's address without a key")
            << u"http://192.168.1.10:8787/#display"_s << QByteArray();
        QTest::newRow("a key in front of the cross")
            << u"http://192.168.1.10:8787/?key="_s + QString::fromLatin1(key) << QByteArray();
        QTest::newRow("too short for a key") << u"http://192.168.1.10:8787/#display&key=qkvm_short"_s << QByteArray();
        QTest::newRow("not of the server's making")
            << u"http://192.168.1.10:8787/#display&key="_s + QString(43, u'k') << QByteArray();
        QTest::newRow("with what no key has in it")
            << u"http://192.168.1.10:8787/#display&key=qkvm_"_s + QString(30, u'k') + u"\"; rm"_s << QByteArray();
        QTest::newRow("longer than a key is")
            << u"http://192.168.1.10:8787/#display&key=qkvm_"_s + QString(200, u'k') << QByteArray();
    }

    void aDisplayLinksKeyIsFoundWhereThereIsOne()
    {
        QFETCH(QString, typed);
        QFETCH(QByteArray, key);
        QCOMPARE(core::ServerClient::displayKeyIn(typed), key);
        // And the address is read from the same text, key or none.
        if (!key.isEmpty()) {
            const base::Result<QUrl> address = core::ServerClient::addressFrom(typed);
            QVERIFY(address.ok());
            QVERIFY(!address.value().toString().contains(u"qkvm_"_s));
            QVERIFY(!address.value().hasFragment());
        }
    }

    void aDisplayLinkOpensASession()
    {
        const QByteArray key = "qkvm_" + QByteArray(43, 'k');
        StandIn server([](const Asked& asked) {
            if (asked.path == "/api/v1/display") {
                return Answered{
                    200,
                    R"({"ok": true, "user": {"id": "token:t1", "name": "Lobby screen", "role": "viewer", "token": true}})",
                    "application/json",
                    {"Set-Cookie: qkvm_session=shown1; Path=/; HttpOnly"}};
            }
            return Answered{200, R"({"interval": 1, "devices": []})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Account> screen =
            answerTo<core::Account>([&](const auto& done) { client.openDisplay(key, done); });
        QVERIFY(screen.ok());
        QCOMPARE(screen.value().name, u"Lobby screen"_s);
        QCOMPARE(screen.value().role, u"viewer"_s);
        QVERIFY(screen.value().isDisplay);
        QCOMPARE(server.asked[0].method, QByteArray("POST"));
        QCOMPARE(server.asked[0].headers.value("x-qkvm"), QByteArray("1"));
        QCOMPARE(QJsonDocument::fromJson(server.asked[0].body).object()["key"_L1].toString().toLatin1(), key);
        // And what is asked afterwards carries the session it was given.
        QVERIFY(answerTo<core::Wall>([&](const auto& done) { client.askWall(done); }).ok());
        QCOMPARE(server.asked[1].headers.value("cookie"), QByteArray("qkvm_session=shown1"));
    }

    void aDisplayLinkThatWasTakenBackIsToldInTheServersWords()
    {
        StandIn server([](const Asked&) {
            return Answered{
                401,
                R"({"code": "bad_key", "message": "This display link is not valid any more. Ask an admin for a new one."})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Account> screen =
            answerTo<core::Account>([&](const auto& done) { client.openDisplay("qkvm_" + QByteArray(43, 'k'), done); });
        QVERIFY(!screen.ok());
        QCOMPARE(screen.error().code, std::string("bad_key"));
        QCOMPARE(screen.error().message,
                 std::string("This display link is not valid any more. Ask an admin for a new one."));
    }

    void aWrongPasswordIsToldInTheServersWords()
    {
        StandIn server(
            [](const Asked&) { return Answered{401, R"({"code": "bad_password", "message": "Wrong password"})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Account> account =
            answerTo<core::Account>([&](const auto& done) { client.signIn({}, u"wrong"_s, {}, false, done); });
        QVERIFY(!account.ok());
        QCOMPARE(account.error().code, std::string("bad_password"));
        QCOMPARE(account.error().message, std::string("Wrong password"));
    }

    void aSessionThatIsGoneIsToldAsThat()
    {
        StandIn server(
            [](const Asked&) { return Answered{401, R"({"code": "unauthorized", "message": "Sign in first"})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string(core::ServerClient::signedOut));
    }

    void aServerThatAsksForAWindowsSignOnGetsNone_data()
    {
        QTest::addColumn<QByteArray>("way");
        QTest::addColumn<bool>("keepsLines");
        QTest::newRow("NTLM") << "NTLM"_ba << false;
        QTest::newRow("NTLM, on a line that is kept") << "NTLM"_ba << true;
        QTest::newRow("Negotiate") << "Negotiate"_ba << false;
        QTest::newRow("Negotiate, on a line that is kept") << "Negotiate"_ba << true;
        QTest::newRow("a name and a password") << "Basic realm=qkvm"_ba << true;
    }

    void aServerThatAsksForAWindowsSignOnGetsNone()
    {
        // Asked to sign on in some ways, Qt would answer by itself with the account of whoever is
        // signed in to Windows, and send the question again, the session it carries included.
        QFETCH(QByteArray, way);
        QFETCH(bool, keepsLines);
        StandIn server([&way](const Asked& asked) {
            return Answered{401,
                            asked.path == "/api/v1/login" ? R"({"code": "bad_password", "message": "Wrong password"})"
                                                          : R"({"code": "unauthorized", "message": "Sign in first"})",
                            "application/json",
                            {"WWW-Authenticate: " + way}};
        });
        server.keepsLines = keepsLines;
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        // An ordinary call is asked once, and ends as the 401 it is: the session is gone.
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QTest::qWait(300); // (a second asking would have arrived by now)
        QCOMPARE(server.count("/api/v1/wall"), 1);
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string(core::ServerClient::signedOut));
        // So is a sign-in, which ends as the server's refusal of it, in the server's words.
        const base::Result<core::Account> account =
            answerTo<core::Account>([&](const auto& done) { client.signIn(u"anna"_s, u"wrong"_s, {}, false, done); });
        QTest::qWait(300);
        QCOMPARE(server.count("/api/v1/login"), 1);
        QVERIFY(!account.ok());
        QCOMPARE(account.error().code, std::string("bad_password"));
        QCOMPARE(account.error().message, std::string("Wrong password"));
        // And nothing that went out carried a sign-on of Qt's making.
        QCOMPARE(server.asked.size(), 2);
        for (const Asked& one : std::as_const(server.asked)) {
            QVERIFY(!one.headers.contains("authorization"));
        }
    }

    void aPictureComesWithWhetherTheMachineSendsOne()
    {
        const QByteArray jpeg = QByteArray::fromHex("ffd8ffe000104a464946") + QByteArray(2000, 'x');
        StandIn server([&](const Asked&) { return Answered{200, jpeg, "image/jpeg", {"X-KVM-Signal: 0"}}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Snapshot> picture =
            answerTo<core::Snapshot>([&](const auto& done) { client.askSnapshot(u"a b/c"_s, 480, 0.5, done); });
        QVERIFY(picture.ok());
        QCOMPARE(picture.value().picture, jpeg);
        QVERIFY(!picture.value().signal);
        // The machine's name cannot reach into the address, whatever is in it.
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/devices/a%20b%2Fc/snapshot?w=480&age=0.50"));
    }

    // b5: a still kept between sessions comes with its age, which the tile says.
    void aStillComesWithHowOldItIs()
    {
        const QByteArray jpeg = aJpeg();
        StandIn server([&](const Asked& asked) {
            return asked.path.contains("/devices/amt/")
                       ? Answered{200, jpeg, "image/jpeg", {"X-KVM-Signal: 1", "X-KVM-Still: 1", "X-KVM-Age: 137"}}
                       : Answered{200, jpeg, "image/jpeg", {"X-KVM-Signal: 1"}};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Snapshot> still =
            answerTo<core::Snapshot>([&](const auto& done) { client.askSnapshot(u"amt"_s, 480, 0.5, done); });
        QVERIFY(still.ok());
        QVERIFY(still.value().still);
        QCOMPARE(still.value().ageSeconds, std::optional<int>(137));
        const base::Result<core::Snapshot> live =
            answerTo<core::Snapshot>([&](const auto& done) { client.askSnapshot(u"kvm"_s, 480, 0.5, done); });
        QVERIFY(live.ok());
        QVERIFY(!live.value().still);
        QVERIFY(!live.value().ageSeconds.has_value());
    }

    // b5: a PC with Intel AMT is put in another view by its own call, and the server's word for the
    // view it is in now is what comes back; a token's 403 is the server's refusal, said as it is.
    void aPcIsPutInAnotherViewByItsOwnCall()
    {
        StandIn server([](const Asked& asked) {
            if (asked.path.contains("/devices/tok/")) {
                return Answered{403,
                                R"({"code": "not_allowed", "message": "A token does not change how a tile is set."})"};
            }
            return Answered{200, R"({"ok": true, "amt_view": "health"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<QString> done =
            answerTo<QString>([&](const auto& told) { client.setView(u"a b/c"_s, u"health"_s, told); });
        QVERIFY(done.ok());
        QCOMPARE(done.value(), u"health"_s);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("PUT"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/devices/a%20b%2Fc/view"));
        QCOMPARE(server.asked[0].headers.value("x-qkvm"), QByteArray("1"));
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=s1"));
        QCOMPARE(QJsonDocument::fromJson(server.asked[0].body).object(), (QJsonObject{{u"view"_s, u"health"_s}}));
        const base::Result<QString> refused =
            answerTo<QString>([&](const auto& told) { client.setView(u"tok"_s, u"live"_s, told); });
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("not_allowed"));
        QCOMPARE(refused.error().message, std::string("A token does not change how a tile is set."));
    }

    // b7: the picture that set off an alert is asked for by its machine and its stamp, and is one only
    // where it says it is a picture and begins as one; once it is kept no more, the server says so; a
    // stamp that is not of the server's shape is not asked for at all.
    void anAlertsPictureIsAskedForByItsStamp()
    {
        StandIn server([&](const Asked& asked) {
            if (asked.path.endsWith("/alert-picture/17")) {
                return Answered{200, aJpeg(), "image/jpeg"};
            }
            if (asked.path.endsWith("/alert-picture/18")) {
                return Answered{200, "<html>a page</html>", "text/html"};
            }
            return Answered{404, R"({"code": "no_picture", "message": "That picture is no longer kept"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const auto ask = [&](const QString& stamp) {
            return answerTo<QByteArray>([&](const auto& done) { client.askAlertPicture(u"a b/c"_s, stamp, done); });
        };
        const base::Result<QByteArray> picture = ask(u"17"_s);
        QVERIFY(picture.ok());
        QCOMPARE(picture.value(), aJpeg());
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("GET"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/devices/a%20b%2Fc/alert-picture/17"));
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=s1"));
        const base::Result<QByteArray> gone = ask(u"19"_s);
        QVERIFY(!gone.ok());
        QCOMPARE(gone.error().code, std::string("no_picture"));
        for (const QString& stamp : {u"../17"_s, u"17?w=1"_s, u""_s, u"12345678901234567"_s, u"1 7"_s}) {
            const base::Result<QByteArray> refused = ask(stamp);
            QVERIFY2(!refused.ok(), qPrintable(stamp));
        }
        QCOMPARE(server.asked.size(), 2);
        const base::Result<QByteArray> page = ask(u"18"_s);
        QVERIFY(!page.ok());
        QCOMPARE(page.error().code, std::string("bad_answer"));
    }

    void somethingIsAskedOfAMachine()
    {
        StandIn server([](const Asked&) { return Answered{200, R"({"ok": true, "note": "already on"})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::Done> done = answerTo<core::Done>([&](const auto& told) {
            client.act(u"a b/c"_s, core::Action::press(u"ControlLeft,AltLeft,Delete"_s), told);
        });
        QVERIFY(done.ok());
        QCOMPARE(done.value().note, u"already on"_s);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("POST"));
        // The machine's name cannot reach into the address, whatever is in it.
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/devices/a%20b%2Fc/actions"));
        // What makes the server take a change at all, and who asks.
        QCOMPARE(server.asked[0].headers.value("x-qkvm"), QByteArray("1"));
        QCOMPARE(server.asked[0].headers.value("content-type"), QByteArray("application/json"));
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=s1"));
        QCOMPARE(QJsonDocument::fromJson(server.asked[0].body).object(),
                 (QJsonObject{{u"action"_s, u"keys"_s}, {u"keys"_s, u"ControlLeft,AltLeft,Delete"_s}}));
    }

    // b11: a line the server kept is not used once it has been idle long enough for the server to be closing it: the
    // server's own keep-alive is five seconds (uvicorn's, which qKVM keeps), and an action sent on such a line just as
    // the server closed it never reached it (the joint run, 0.3.0: "qKVM could not be reached", a press lost). The
    // stand-in here closes a kept line on the next question once it was idle three seconds, which is the race made
    // sure of. What only asks is kept on one line, and after that idle time goes on another, as a question that
    // arrives: not taken for contact lost (no look at what answers there).
    void aLineKeptIdleUntilTheServerMayBeClosingItIsNotUsed()
    {
        StandIn server(emptyWall);
        server.keepsLines = true;
        server.forgetsIdleAfterMs = 3000;
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const auto look = [&] { return answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }); };
        QVERIFY(look().ok());
        QVERIFY(look().ok());
        QCOMPARE(server.lines, 1);
        QTest::qWait(3500);
        const base::Result<core::Wall> after = look();
        QVERIFY2(after.ok(), after ? "" : after.error().message.c_str());
        QCOMPARE(server.forgotten, 0);
        QCOMPARE(server.count("/healthz"), 0);
        QCOMPARE(server.count("/api/v1/wall"), 3);
        QCOMPARE(server.lines, 2);
    }

    // b11: what changes something goes on a line of its own, every time: never on a line the client kept (one idle
    // past the server's keep-alive among them), and on none another question comes on. Each arrives once.
    void aChangeGoesOnALineOfItsOwnEveryTime()
    {
        StandIn server([](const Asked& asked) {
            return asked.path == "/api/v1/wall" ? emptyWall(asked) : Answered{200, R"({"ok": true, "note": null})"};
        });
        server.keepsLines = true;
        server.forgetsIdleAfterMs = 3000;
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const auto press = [&] {
            return answerTo<core::Done>(
                [&](const auto& told) { client.act(u"pc"_s, core::Action::plain(u"atx_power"_s), told); });
        };
        QVERIFY(answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        QVERIFY(press().ok());
        QVERIFY(press().ok());
        QTest::qWait(3500);
        const base::Result<core::Done> after = press();
        QVERIFY2(after.ok(), after ? "" : after.error().message.c_str());
        QCOMPARE(server.forgotten, 0);
        QCOMPARE(server.count("/api/v1/devices/pc/actions"), 3);
        QSet<int> lines;
        for (const Asked& one : std::as_const(server.asked)) {
            lines.insert(one.line);
        }
        QCOMPARE(lines.size(), server.asked.size());
    }

    // b11: a change on a line of its own is held to the server's certificate as every question is: known by the one it
    // shows, it arrives; known by another, or by none Windows trusts, nothing of it is sent, and why is said.
    void aChangeOnALineOfItsOwnKeepsTheCertificateRule()
    {
        StandIn server([](const Asked&) { return Answered{200, R"({"ok": true, "note": null})"}; }, tls("first"));
        QVERIFY(server.isListening());
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const auto press = [&] {
            return answerTo<core::Done>(
                [&](const auto& told) { client.act(u"pc"_s, core::Action::plain(u"atx_power"_s), told); });
        };
        const base::Result<core::Done> unknown = press();
        QVERIFY(!unknown.ok());
        QCOMPARE(unknown.error().code, std::string(core::ServerClient::certificateUnknown));
        QCOMPARE(server.asked.size(), 0);
        client.setKnownCertificate(sha256Of("first"));
        const base::Result<core::Done> known = press();
        QVERIFY2(known.ok(), known ? "" : known.error().message.c_str());
        QCOMPARE(server.asked.size(), 1);
        client.setKnownCertificate(sha256Of("second"));
        const base::Result<core::Done> other = press();
        QVERIFY(!other.ok());
        QCOMPARE(other.error().code, std::string(core::ServerClient::certificateChanged));
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.notHttp, 0);
    }

    // b11: and it carries the session, as every question does: the server takes it as whoever is signed in here.
    void aChangeOnALineOfItsOwnCarriesTheSession()
    {
        StandIn server([](const Asked& asked) {
            return asked.headers.value("cookie") == "qkvm_session=s1"
                       ? Answered{200, R"({"ok": true, "note": null})"}
                       : Answered{401, R"({"code": "unauthorized", "message": "Sign in first"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::Done> done = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"pc"_s, core::Action::plain(u"atx_power"_s), told); });
        QVERIFY2(done.ok(), done ? "" : done.error().message.c_str());
        QCOMPARE(server.asked.size(), 1);
    }

    // b11: nor is it sent somewhere else on a server's word: an answer that says "go there" is not followed.
    void aChangeOnALineOfItsOwnFollowsNoRedirect()
    {
        StandIn elsewhere([](const Asked&) { return Answered{200, R"({"ok": true, "note": null})"}; });
        const QByteArray there = "Location: " + elsewhere.address().toLatin1() + "/api/v1/devices/pc/actions";
        StandIn server([&there](const Asked&) { return Answered{307, {}, "text/plain", {there}}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::Done> done = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"pc"_s, core::Action::plain(u"atx_power"_s), told); });
        QVERIFY(!done.ok());
        QCOMPARE(server.asked.size(), 1);
        QTest::qWait(300);
        QCOMPARE(elsewhere.asked.size(), 0);
    }

    // b11: a run on many machines at once, each on a line of its own: each machine is asked exactly once.
    void manyChangesAtOnceAreEachSentOnce()
    {
        StandIn server([](const Asked&) { return Answered{200, R"({"ok": true, "note": null})"}; });
        server.keepsLines = true;
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        QList<base::Result<core::Done>> answers;
        constexpr int machines = 12;
        for (int at = 0; at < machines; ++at) {
            client.act(u"m%1"_s.arg(at), core::Action::plain(u"atx_reset"_s),
                       [&answers](base::Result<core::Done> done) { answers.append(std::move(done)); });
        }
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), machines, 20'000);
        for (const auto& done : std::as_const(answers)) {
            QVERIFY2(done.ok(), done ? "" : done.error().message.c_str());
        }
        QTest::qWait(300);
        QCOMPARE(server.asked.size(), machines);
        for (int at = 0; at < machines; ++at) {
            QCOMPARE(server.count("/api/v1/devices/m" + QByteArray::number(at) + "/actions"), 1);
        }
    }

    // The same where the client has more than one line to the server, and keeps one of them busy: the other, idle as
    // long, is not used for a question that changes something either, which goes out on a line of its own. (Qt lets go
    // of a whole pool, never of one idle line while another is busy: a GET can still meet such a line, and is asked
    // anew.)
    void aLineOfSeveralKeptIdleUntilTheServerMayBeClosingItIsNotUsed()
    {
        StandIn server([](const Asked& asked) {
            Answered answer{200, R"({"ok": true, "note": null})"};
            if (asked.path.startsWith("/api/v1/devices/slow/")) {
                answer.afterMs = 700;
            }
            return answer;
        });
        server.keepsLines = true;
        server.forgetsIdleAfterMs = 3000;
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const auto act = [&](const QString& machine, QList<base::Result<core::Done>>& into) {
            client.act(machine, core::Action::plain(u"atx_power"_s),
                       [&into](base::Result<core::Done> done) { into.append(std::move(done)); });
        };
        // Two at once: two lines.
        QList<base::Result<core::Done>> first;
        act(u"slow"_s, first);
        act(u"pc"_s, first);
        QTRY_COMPARE_WITH_TIMEOUT(first.size(), 2, 10'000);
        QCOMPARE(server.lines, 2);
        // One line kept busy for longer than the server keeps an idle one, the other idle meanwhile.
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 3500) {
            QList<base::Result<core::Done>> one;
            act(u"pc"_s, one);
            QTRY_COMPARE_WITH_TIMEOUT(one.size(), 1, 10'000);
            QVERIFY(one.first().ok());
            QTest::qWait(300);
        }
        // Then two at once again: the second goes beside the first, and not on a line the server may be closing.
        QList<base::Result<core::Done>> last;
        act(u"slow"_s, last);
        act(u"pc"_s, last);
        QTRY_COMPARE_WITH_TIMEOUT(last.size(), 2, 10'000);
        for (const auto& done : std::as_const(last)) {
            QVERIFY2(done.ok(), done ? "" : done.error().message.c_str());
        }
        QCOMPARE(server.forgotten, 0);
    }

    void theWallsOrderIsToldToTheServerWhichHandsItBackWhole()
    {
        // (The server puts the machines that were not named after those that were.)
        StandIn server([](const Asked&) { return Answered{200, R"({"ok": true, "ids": ["b", "a", "s"]})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<QStringList> done =
            answerTo<QStringList>([&](const auto& told) { client.arrange({u"b"_s, u"a"_s}, told); });
        QVERIFY(done.ok());
        QCOMPARE(done.value(), (QStringList{u"b"_s, u"a"_s, u"s"_s}));
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("PUT"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/wall/order"));
        // What makes the server take a change at all, and who asks.
        QCOMPARE(server.asked[0].headers.value("x-qkvm"), QByteArray("1"));
        QCOMPARE(server.asked[0].headers.value("content-type"), QByteArray("application/json"));
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=s1"));
        QCOMPARE(QJsonDocument::fromJson(server.asked[0].body).object(),
                 (QJsonObject{{u"ids"_s, QJsonArray{u"b"_s, u"a"_s}}}));
    }

    void aRefusalToArrangeIsToldInTheServersWords()
    {
        StandIn server([](const Asked&) {
            return Answered{403, R"({"code": "forbidden", "message": "Only an admin can do that"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<QStringList> done =
            answerTo<QStringList>([&](const auto& told) { client.arrange({u"a"_s}, told); });
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, std::string("forbidden"));
        QCOMPARE(done.error().message, std::string("Only an admin can do that"));
    }

    void aRefusalToDoItIsToldInTheServersWords()
    {
        StandIn server([](const Asked&) {
            return Answered{
                409,
                R"({"code": "busy", "message": "Something else is being done to NAS at this moment. Wait for it to finish."})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Done> done = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, std::string("busy"));
        QCOMPARE(done.error().message,
                 std::string("Something else is being done to NAS at this moment. Wait for it to finish."));
        // The server said no: nothing was done.
        QVERIFY(!client.mayHaveBeenDone(u"any"_s, done.error()));
    }

    void aMachineTheServerCannotReachIsARefusalAndNotAMissingAnswer()
    {
        // The server answers, and says that the machine did not: nothing was done there. The server's
        // word for that is not this program's finding that the server itself could not be reached.
        StandIn server([](const Asked&) {
            return Answered{502, R"({"code": "unreachable", "message": "Device is unreachable"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Done> done = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
        QVERIFY(!done.ok());
        QCOMPARE(done.error().message, std::string("Device is unreachable"));
        QVERIFY(done.error().code != std::string(core::ServerClient::unreachable));
        QVERIFY(!client.mayHaveBeenDone(u"any"_s, done.error()));
    }

    void aServersWordIsNotTakenForWhatThisProgramFindsItself_data()
    {
        QTest::addColumn<QString>("word");
        QTest::newRow("a machine that is not reached") << u"unreachable"_s;
        QTest::newRow("signed out, in a word") << u"signed_out"_s;
        QTest::newRow("another certificate, in a word") << u"certificate_changed"_s;
        QTest::newRow("an unknown certificate, in a word") << u"certificate_unknown"_s;
        QTest::newRow("as this program writes: not reached") << QString::fromLatin1(core::ServerClient::unreachable);
        QTest::newRow("as this program writes: signed out") << QString::fromLatin1(core::ServerClient::signedOut);
        QTest::newRow("as this program writes: another certificate")
            << QString::fromLatin1(core::ServerClient::certificateChanged);
        QTest::newRow("as this program writes: an unknown certificate")
            << QString::fromLatin1(core::ServerClient::certificateUnknown);
    }

    void aServersWordIsNotTakenForWhatThisProgramFindsItself()
    {
        // That the server was not reached, that the session is gone, that the certificate is not the
        // server's: the program finds these itself, and much hangs on each. A refusal that merely
        // says one of them in its answer is a refusal like any other.
        QFETCH(QString, word);
        StandIn server([&word](const Asked&) {
            const QJsonObject said{{u"code"_s, word}, {u"message"_s, u"As the server says"_s}};
            return Answered{400, QJsonDocument(said).toJson(QJsonDocument::Compact)};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().message, std::string("As the server says"));
        for (const char* own : {core::ServerClient::unreachable, core::ServerClient::signedOut,
                                core::ServerClient::certificateChanged, core::ServerClient::certificateUnknown}) {
            QVERIFY2(wall.error().code != std::string(own), own);
        }
        QVERIFY(!client.mayHaveBeenDone(u"any"_s, wall.error()));
    }

    void noAnswerToWhatWasAskedIsNoRefusal()
    {
        // The line falls dead when the question has gone out: whether the machine did it nobody knows.
        StandIn silent([](const Asked&) {
            Answered none;
            none.hangUp = true;
            return none;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(silent.address()).ok());
        const base::Result<core::Done> done = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
        QVERIFY(!done.ok());
        QVERIFY(client.mayHaveBeenDone(u"any"_s, done.error()));
        // And it was asked once: a power button is not pressed again because the first press was not
        // answered.
        QTest::qWait(300);
        QCOMPARE(silent.asked.size(), 1);

        // The server's own word for having lost sight of it on the way to the machine.
        StandIn lost([](const Asked&) {
            return Answered{504,
                            R"({"code": "timeout", "message": "NAS was being changed while this was under way."})"};
        });
        QVERIFY(client.setAddress(lost.address()).ok());
        const base::Result<core::Done> unknown = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
        QVERIFY(!unknown.ok());
        QVERIFY(client.mayHaveBeenDone(u"any"_s, unknown.error()));
        QCOMPARE(lost.asked.size(), 1);

        // And its word for an answer of the machine's that broke off, or a broken one: what it sent
        // went out, and the machine may have done it (FOR-THE-DESKTOP-APP, "timeout and dropped").
        StandIn broken([](const Asked&) {
            return Answered{502, R"({"code": "dropped", "message": "The connection to the JetKVM ended"})"};
        });
        QVERIFY(client.setAddress(broken.address()).ok());
        const base::Result<core::Done> cut = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
        QVERIFY(!cut.ok());
        QCOMPARE(cut.error().code, std::string("dropped"));
        QVERIFY(client.mayHaveBeenDone(u"any"_s, cut.error()));
        QTest::qWait(300);
        QCOMPARE(broken.asked.size(), 1);

        // A session that is gone and a certificate that is not the server's are neither: nothing went
        // out to a machine.
        QVERIFY(!client.mayHaveBeenDone(u"any"_s, {core::ServerClient::signedOut, "You are signed out."}));
        QVERIFY(!client.mayHaveBeenDone(u"any"_s, {core::ServerClient::certificateChanged, "Another one."}));
    }

    void whateverBreaksOffAfterTheServerSaidYesMayHaveBeenDone_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("otherwise");
        // (An answer that says it is longer than what comes is in a test of its own, below.)
        QTest::newRow("its answer is larger than is taken, and is cut off")
            << QByteArray(core::largestAnswerBytes + 1000, ' ') << u"too_large"_s;
        QTest::newRow("its answer is not what a qKVM says of something done")
            << "<html>done</html>"_ba << u"bad_answer"_s;
    }

    void whateverBreaksOffAfterTheServerSaidYesMayHaveBeenDone()
    {
        // The server answered 200 to what was to be done to a machine: it said yes. That what it
        // said with the yes was cut off here, or is not understood, is no refusal.
        QFETCH(QByteArray, body);
        QFETCH(QString, otherwise);
        StandIn server([&body](const Asked& asked) {
            return asked.path == "/healthz" ? Answered{200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"]})"}
                                            : Answered{200, body};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Done> done = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
        QVERIFY(!done.ok());
        QVERIFY2(client.mayHaveBeenDone(u"any"_s, done.error()), done.error().code.c_str());
        QTest::qWait(200);
        QCOMPARE(server.count("/api/v1/devices/nas/actions"), 1);
        // Of a question that does nothing, the same answer is what it was: there is nothing
        // that may have been done.
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& told) { client.askWall(told); });
        QVERIFY(!wall.ok());
        QCOMPARE(QString::fromStdString(wall.error().code), otherwise);
        QVERIFY(!client.mayHaveBeenDone(u"any"_s, wall.error()));
    }

    void whatAGatewaySaysOfSomethingThatWasToBeDoneIsNoRefusal_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("mayBeDone");
        const QByteArray page = "<html><head><title>502 Bad Gateway</title></head><body>502</body></html>"_ba;
        // What stands before the server, in the middle of something that takes the machine long.
        QTest::newRow("a gateway's page: nothing behind it") << 502 << page << true;
        QTest::newRow("a gateway's page: it gave up waiting") << 504 << page << true;
        QTest::newRow("an error with nothing said") << 500 << QByteArray() << true;
        QTest::newRow("an error in other words than a qKVM's") << 503 << R"({"error": "upstream"})"_ba << true;
        // The server's own words for the device are its own: a no, where they say that nothing was sent.
        QTest::newRow("the server's word for a machine it cannot reach")
            << 502 << R"({"code": "unreachable", "message": "Device is unreachable"})"_ba << false;
        QTest::newRow("the server's word for a picture it has not")
            << 503 << R"({"code": "no_stream", "message": "Waiting for the first picture"})"_ba << false;
        // But not its own 500, whatever its word: an error inside the server, which its middleware answers for any
        // error of a handler, one raised after a press went out among them (the web's server.py, `guard`). It may have
        // been done. (Turned from "a no" by the merged review, 5, 2026-11-06.)
        QTest::newRow("the server's own error")
            << 500 << R"({"code": "failed", "message": "The machine refused"})"_ba << true;
        QTest::newRow("the server's own error, as its middleware says it")
            << 500
            << R"({"code": "internal", "message": "Something went wrong inside qKVM. The details are in its log."})"_ba
            << true;
        // Except where they say that it went out and no answer came, or a broken one.
        QTest::newRow("the server's word for a machine that lost what it was sent")
            << 504 << R"({"code": "timeout", "message": "NAS was being changed"})"_ba << true;
        QTest::newRow("the server's word for a machine's answer that broke off")
            << 502 << R"({"code": "dropped", "message": "The connection to the JetKVM ended"})"_ba << true;
        // And what was not passed on at all: a no of whatever answered.
        QTest::newRow("a page that says there is no such place") << 404 << page << false;
        // An answer that is cut off here for its size was read by nobody: no refusal, whatever
        // its number.
        const QByteArray large = page + QByteArray(core::largestAnswerBytes + 1000, ' ');
        QTest::newRow("a gateway's page that is larger than is read") << 502 << large << true;
        QTest::newRow("a page of another number that is larger than is read") << 404 << large << true;
    }

    void whatAGatewaySaysOfSomethingThatWasToBeDoneIsNoRefusal()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(bool, mayBeDone);
        StandIn server([&](const Asked&) { return Answered{status, body}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Done> done = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"nas"_s, core::Action::type(u"a long text"_s, true), told); });
        QVERIFY(!done.ok());
        QCOMPARE(client.mayHaveBeenDone(u"any"_s, done.error()), mayBeDone);
        QTest::qWait(200);
        QCOMPARE(server.asked.size(), 1);
    }

    void afterContactWasLostNothingCarriesTheSessionBeforeAQkvmHasSaidWhatItIs()
    {
        const auto asAQkvm = [](const Asked& asked) {
            return asked.path == "/healthz" ? Answered{200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"]})"}
                                            : emptyWall(asked);
        };
        const auto wallOf = [](core::ServerClient& client) {
            return answerTo<core::Wall>([&](const auto& told) { client.askWall(told); });
        };
        auto server = std::make_unique<StandIn>(asAQkvm);
        const quint16 port = server->serverPort();
        core::ServerClient client;
        QVERIFY(client.setAddress(server->address()).ok());
        client.setSession("s1");
        // While the server answers, nothing is asked beside what is asked.
        QVERIFY(wallOf(client).ok());
        QVERIFY(wallOf(client).ok());
        QCOMPARE(server->asked.size(), 2);
        // The server goes away: a call gets no answer.
        server.reset();
        const base::Result<core::Wall> none = wallOf(client);
        QVERIFY(!none.ok());
        QCOMPARE(none.error().code, std::string(core::ServerClient::unreachable));
        {
            // Something else answers at its address, whatever it is asked. Each call that would
            // carry the session has it asked what it is first, with nothing that says who asks;
            // it is no QKVM, and the call is not sent.
            StandIn other([](const Asked&) { return Answered{200, "<html>It works!</html>"_ba, "text/html"_ba}; },
                          port);
            QVERIFY(other.isListening());
            for (int round = 0; round < 2; ++round) {
                const base::Result<core::Wall> wall = wallOf(client);
                QVERIFY(!wall.ok());
                QCOMPARE(wall.error().code, std::string(core::ServerClient::notSent));
                const base::Result<core::Snapshot> picture =
                    answerTo<core::Snapshot>([&](const auto& told) { client.askSnapshot(u"nas"_s, 480, 0.5, told); });
                QVERIFY(!picture.ok());
                QCOMPARE(picture.error().code, std::string(core::ServerClient::notSent));
                // What was to be done was not sent either: that is no "may have been done".
                const base::Result<core::Done> done = answerTo<core::Done>(
                    [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
                QVERIFY(!done.ok());
                QCOMPARE(done.error().code, std::string(core::ServerClient::notSent));
                QVERIFY(!client.mayHaveBeenDone(u"any"_s, done.error()));
            }
            QTest::qWait(200);
            QVERIFY(!other.asked.isEmpty());
            for (const Asked& one : std::as_const(other.asked)) {
                QCOMPARE(one.path, QByteArray("/healthz"));
                QVERIFY(!one.headers.contains("cookie"));
            }
        }
        // The QKVM is back. The next call has it asked what it is, and goes out behind its
        // answer, with the session; from then on nothing is asked first.
        StandIn back(asAQkvm, port);
        QVERIFY(back.isListening());
        QVERIFY(wallOf(client).ok());
        QCOMPARE(back.asked.size(), 2);
        QCOMPARE(back.asked[0].path, QByteArray("/healthz"));
        QVERIFY(!back.asked[0].headers.contains("cookie"));
        QCOMPARE(back.asked[1].path, QByteArray("/api/v1/wall"));
        QCOMPARE(back.asked[1].headers.value("cookie"), QByteArray("qkvm_session=s1"));
        QVERIFY(wallOf(client).ok());
        QCOMPARE(back.asked.size(), 3);
    }

    void whatIsOutWhenContactIsLostIsNotGivenUpAndWhatIsAskedNextWaitsForWhatAnswers()
    {
        // The question for how the machines are is out when another call gets a gateway's page:
        // contact is lost. What is out has left, and its session with it: it is answered. And the
        // next call that carries the session waits until what answers there has said what it is.
        // (This test had a picture that was out given up at that moment, so that Qt would not
        // ask it a second time of whatever takes the server's place. Given up with it was the
        // wall's own question, and the wall went away for one machine's picture. What Qt asks
        // again by itself has a test of its own, below.)
        StandIn server([](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            if (asked.path == "/api/v1/status") {
                Answered late{200, R"({"a": {"state": "online"}})"};
                late.afterMs = 2500;
                return late;
            }
            if (asked.path.contains("/snapshot")) {
                return Answered{404, "<html>Not Found</html>"_ba, "text/html"_ba};
            }
            return emptyWall(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        QList<base::Result<QHash<QString, core::Status>>> looks;
        client.askStatus([&looks](const base::Result<QHash<QString, core::Status>>& look) { looks.append(look); });
        QTRY_COMPARE(server.count("/api/v1/status"), 1);
        const base::Result<core::Snapshot> picture =
            answerTo<core::Snapshot>([&](const auto& told) { client.askSnapshot(u"nas"_s, 480, 0.5, told); });
        QVERIFY(!picture.ok());
        QCOMPARE(picture.error().code, std::string("refused"));
        QVERIFY(looks.isEmpty());
        QList<base::Result<core::Wall>> walls;
        client.askWall([&walls](const base::Result<core::Wall>& wall) { walls.append(wall); });
        QTRY_COMPARE_WITH_TIMEOUT(looks.size(), 1, 10'000);
        QVERIFY2(looks[0].ok(), looks[0].ok() ? "" : looks[0].error().message.c_str());
        QTRY_COMPARE(walls.size(), 1);
        QVERIFY2(walls[0].ok(), walls[0].ok() ? "" : walls[0].error().message.c_str());
        // The wall was asked for behind the question what answers there, which carried nothing.
        QCOMPARE(server.count("/healthz"), 1);
        qsizetype whatItIs = -1;
        qsizetype theWall = -1;
        for (qsizetype at = 0; at < server.asked.size(); ++at) {
            whatItIs = server.asked[at].path == "/healthz" ? at : whatItIs;
            theWall = server.asked[at].path == "/api/v1/wall" ? at : theWall;
        }
        QVERIFY(whatItIs >= 0 && whatItIs < theWall);
        QVERIFY(!server.asked[whatItIs].headers.contains("cookie"));
        QCOMPARE(server.asked[theWall].headers.value("cookie"), QByteArray("qkvm_session=s1"));
    }

    void aPictureThatIsTooLargeOrTooSlowFailsAloneAndContactIsNotLost_data()
    {
        QTest::addColumn<bool>("slow");
        QTest::newRow("larger than is taken") << false;
        QTest::newRow("longer on its way than a picture may be") << true;
    }

    void aPictureThatIsTooLargeOrTooSlowFailsAloneAndContactIsNotLost()
    {
        // One machine's picture is larger than is taken, or the server is still fetching it from
        // the machine when a picture's time is up. Neither says anything of who answers: that
        // picture fails, the question for how the machines are, which is out, is answered, and
        // what is asked next goes straight out.
        QFETCH(const bool, slow);
        StandIn server([slow](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            if (asked.path == "/api/v1/status") {
                Answered late{200, R"({"a": {"state": "online"}})"};
                late.afterMs = 4000;
                return late;
            }
            if (asked.path.contains("/snapshot")) {
                Answered picture{200, aJpeg(), "image/jpeg"_ba};
                if (slow) {
                    picture.never = true;
                } else {
                    picture.moreToCome = static_cast<int>(core::largestPictureBytes) + 1;
                }
                return picture;
            }
            return emptyWall(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        QList<base::Result<core::Snapshot>> pictures;
        QList<base::Result<QHash<QString, core::Status>>> looks;
        const auto look = [&client, &looks] {
            client.askStatus([&looks](const base::Result<QHash<QString, core::Status>>& one) { looks.append(one); });
        };
        if (!slow) {
            look();
            QTRY_COMPARE(server.count("/api/v1/status"), 1);
        }
        client.askSnapshot(u"nas"_s, 480, 0.5,
                           [&pictures](const base::Result<core::Snapshot>& picture) { pictures.append(picture); });
        if (slow) {
            // (A picture is given fifteen seconds: the look is out when that time is up.)
            QTest::qWait(13'500);
            look();
        }
        QTRY_COMPARE_WITH_TIMEOUT(pictures.size(), 1, 10'000);
        QVERIFY(!pictures[0].ok());
        QCOMPARE(pictures[0].error().code, std::string(slow ? core::ServerClient::unreachable : "too_large"));
        if (slow) {
            QCOMPARE(pictures[0].error().message, std::string("qKVM did not answer in time."));
        }
        QVERIFY(looks.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(looks.size(), 1, 10'000);
        QVERIFY2(looks[0].ok(), looks[0].ok() ? "" : looks[0].error().message.c_str());
        // Contact is not lost: what answers there is not asked what it is first.
        QVERIFY(answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        QCOMPARE(server.count("/healthz"), 0);
    }

    void whatHadNotLeftWhenContactWasLostIsTakenBackAndNotHandedToWhateverAnswersThereNext_data()
    {
        QTest::addColumn<bool>("aQkvmAnswersNext");
        QTest::addColumn<bool>("doesSomething");
        QTest::newRow("another program answers there next, and the question only asks") << false << false;
        QTest::newRow("the qKVM answers there again, and the question only asks") << true << false;
        QTest::newRow("another program answers there next, and something was to be done") << false << true;
        QTest::newRow("the qKVM answers there again, and something was to be done") << true << true;
    }

    void whatHadNotLeftWhenContactWasLostIsTakenBackAndNotHandedToWhateverAnswersThereNext()
    {
        // Nothing answers at the server's address: Windows goes on trying to reach it for a second
        // or two on behalf of a question. The first question fails; while that waits to be heard
        // (the interface thread is busy), a second is asked, and is on its way to a line of its
        // own. Then the first is heard: contact is lost, and at that moment something begins to
        // answer there. The second carries the session and has not left: it is taken back.
        // Something that was to be done is given up with that, whatever answers there next. A
        // question that only asks waits behind the question for what answers there, which goes
        // with nothing that says who asks: it goes out behind a QKVM's yes, and is answered,
        // once; to another program it never goes. (It did: Windows reached the newcomer for it,
        // and Qt sent it, session and all.)
        QFETCH(const bool, aQkvmAnswersNext);
        QFETCH(const bool, doesSomething);
        // (A port that is held, so that Windows gives it to nobody else, until the newcomer has it.)
        auto nobody = std::make_unique<core::testing::NobodyThere>();
        const quint16 port = nobody->port();
        const QString address = nobody->address();
        // How long a question to an address where nothing answers is tried for, before it fails:
        // found out with a client of its own, so that the one below has not lost contact by it.
        QElapsedTimer clock;
        clock.start();
        {
            core::ServerClient before;
            QVERIFY(before.setAddress(address).ok());
            bool heard = false;
            before.askStatus([&heard](const base::Result<QHash<QString, core::Status>>&) { heard = true; });
            QTRY_VERIFY_WITH_TIMEOUT(heard, 30'000);
        }
        const int triedFor = static_cast<int>(clock.elapsed());
        core::ServerClient client;
        QVERIFY(client.setAddress(address).ok());
        client.setSession("s1");
        std::unique_ptr<StandIn> other;
        int firstTold = 0;
        QTimer comes;
        comes.setSingleShot(true);
        connect(&comes, &QTimer::timeout, &comes, [&] {
            nobody.reset();
            other = std::make_unique<StandIn>(
                [aQkvmAnswersNext](const Asked& asked) {
                    if (!aQkvmAnswersNext) {
                        return Answered{200, "<html>It works!</html>"_ba, "text/html"_ba};
                    }
                    if (asked.path == "/healthz") {
                        return Answered{200, aQkvm};
                    }
                    return Answered{200, R"({"a": {"state": "online"}})"};
                },
                port);
        });
        client.askStatus([&](const base::Result<QHash<QString, core::Status>>&) {
            ++firstTold;
            // The first is heard: a moment later something answers there. (Not in this very
            // moment: what Qt gets a line for before the taking back has reached it, it sends,
            // and that much is not closed. On a busy PC the newcomer's first moment and a try
            // for the second fell together once, and the second went out twice.)
            comes.start(150);
        });
        // (Busy for longer than Windows tries: what became of the first is not heard meanwhile.)
        QThread::msleep(triedFor + 500);
        int secondTold = 0;
        bool secondCame = false;
        base::Error secondFailed;
        if (doesSomething) {
            client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), [&](const base::Result<core::Done>& done) {
                ++secondTold;
                secondCame = done.ok();
                secondFailed = done.ok() ? base::Error{} : done.error();
            });
        } else {
            client.askStatus([&](const base::Result<QHash<QString, core::Status>>& look) {
                ++secondTold;
                secondCame = look.ok();
                secondFailed = look.ok() ? base::Error{} : look.error();
            });
        }
        QTRY_VERIFY_WITH_TIMEOUT(other != nullptr, 20'000);
        QVERIFY2(other->isListening(), "the port that was held went to somebody else all the same");
        QTRY_COMPARE_WITH_TIMEOUT(secondTold, 1, 20'000);
        // (As long as Windows would have gone on trying for the second, and then some.)
        QTest::qWait(triedFor + 500);
        QCOMPARE(firstTold, 1);
        QCOMPARE(secondTold, 1);
        int looks = 0;
        for (const Asked& one : std::as_const(other->asked)) {
            if (one.path == "/healthz") {
                QVERIFY(!one.headers.contains("cookie"));
                continue;
            }
            // Nothing else but a look, only to the QKVM, and only once.
            QVERIFY2(aQkvmAnswersNext && !doesSomething, one.path.constData());
            QCOMPARE(one.path, QByteArray("/api/v1/status"));
            QCOMPARE(one.headers.value("cookie"), QByteArray("qkvm_session=s1"));
            ++looks;
        }
        if (doesSomething) {
            // Given up here, which its sentence says: it had not failed by itself. Told as a
            // server that did not answer, and so as what may have been done: Qt's word that a
            // question left comes a moment after it left.
            QVERIFY(!secondCame);
            QCOMPARE(secondFailed.code, std::string(core::ServerClient::unreachable));
            QCOMPARE(secondFailed.message, std::string("qKVM did not answer: contact with it was lost meanwhile."));
            QCOMPARE(looks, 0);
        } else if (aQkvmAnswersNext) {
            // Behind the QKVM's word of itself, and whoever asked heard nothing of the taking back.
            QVERIFY2(secondCame, secondFailed.message.c_str());
            QCOMPARE(looks, 1);
            QCOMPARE(other->asked.constFirst().path, QByteArray("/healthz"));
        } else {
            // Asked what it is, which says that the second was taken back and waited behind that
            // question: had it failed with the first, nothing would be shown here.
            QVERIFY2(!other->asked.isEmpty(), "the second question was not on its way when the first was heard");
            QVERIFY(!secondCame);
            QCOMPARE(secondFailed.code, std::string(core::ServerClient::notSent));
            QCOMPARE(looks, 0);
        }
    }

    void whatQtAsksAgainByItselfWhereItsLineFellBeforeAnAnswer_data()
    {
        QTest::addColumn<QByteArray>("method");
        QTest::addColumn<bool>("again");
        QTest::newRow("a question that only asks") << "GET"_ba << true;
        QTest::newRow("the wall's order") << "PUT"_ba << true;
        QTest::newRow("something to be done to a machine") << "POST"_ba << false;
    }

    void whatQtAsksAgainByItselfWhereItsLineFellBeforeAnAnswer()
    {
        // For the record, and so that it is noticed when another Qt does otherwise. Where the
        // line falls before an answer, Qt sends a question that only asks (GET), and one that
        // puts something where it belongs (PUT), again by itself, session and all, to whatever
        // listens at the address by then; this program hears of the last failure only. What
        // is not the same done twice (POST) it sends once. Qt's QNetworkRequest and
        // QNetworkAccessManager offer nothing that turns this off (server_client.h).
        QFETCH(const QByteArray, method);
        QFETCH(const bool, again);
        StandIn server([](const Asked&) {
            Answered none;
            none.hangUp = true;
            return none;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        std::string code;
        if (method == "GET") {
            const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& told) { client.askWall(told); });
            code = wall.ok() ? std::string() : wall.error().code;
        } else if (method == "PUT") {
            const base::Result<QStringList> order =
                answerTo<QStringList>([&](const auto& told) { client.arrange({u"a"_s}, told); });
            code = order.ok() ? std::string() : order.error().code;
        } else {
            const base::Result<core::Done> done = answerTo<core::Done>(
                [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
            code = done.ok() ? std::string() : done.error().code;
        }
        QCOMPARE(code, std::string(core::ServerClient::unreachable));
        QTest::qWait(500);
        QVERIFY2(again ? server.asked.size() > 1 : server.asked.size() == 1,
                 QByteArray::number(server.asked.size()).constData());
        for (const Asked& one : std::as_const(server.asked)) {
            QCOMPARE(one.method, method);
            QCOMPARE(one.headers.value("cookie"), QByteArray("qkvm_session=s1"));
        }
    }

    void whatQtSendsAgainByItselfToWhatIsNotThereIsTakenBackWhereContactIsLost_data()
    {
        QTest::addColumn<bool>("lostFirst");
        QTest::newRow("contact was lost before the line fell") << true;
        QTest::newRow("contact is lost while Qt tries to make the new line") << false;
    }

    void whatQtSendsAgainByItselfToWhatIsNotThereIsTakenBackWhereContactIsLost()
    {
        // A picture is out to the server, which is still fetching it from the machine: it has
        // left, with the session. Then the server goes, and the picture's line with it, before
        // an answer: Qt sends the question again by itself, on a new line, and Windows goes on
        // trying for a second or two to make that one where nothing listens any more. Contact
        // is lost, before the server goes (a gateway's page in its place) or while Windows
        // tries (a look that finds nobody there). Then something else begins to answer at the
        // address. Nothing that carries the session reaches it: what Qt was sending again had
        // not left again, and is taken back as such. (Before, it went to the newcomer, session
        // and all: seen in a full run of the session's tests, as the first question the newcomer
        // was asked.)
        QFETCH(const bool, lostFirst);
        // How long a question to an address where nothing answers is tried for, as Windows tries
        // it on this PC: found out with a client of its own.
        int triedFor = 0;
        {
            const core::testing::NobodyThere nobody;
            core::ServerClient before;
            QVERIFY(before.setAddress(nobody.address()).ok());
            QElapsedTimer clock;
            clock.start();
            bool heard = false;
            before.askStatus([&heard](const base::Result<QHash<QString, core::Status>>&) { heard = true; });
            QTRY_VERIFY_WITH_TIMEOUT(heard, 30'000);
            triedFor = static_cast<int>(clock.elapsed());
        }
        if (triedFor < 400) {
            QSKIP("Windows does not go on trying to reach an address where nothing listens on this PC.");
        }
        auto server = std::make_unique<StandIn>([](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            if (asked.path.contains("/snapshot")) {
                Answered fetching;
                fetching.never = true;
                return fetching;
            }
            if (asked.path == "/api/v1/status") {
                return Answered{502, "<html>Bad Gateway</html>"_ba, "text/html"_ba};
            }
            return emptyWall(asked);
        });
        const quint16 port = server->serverPort();
        core::ServerClient client;
        QVERIFY(client.setAddress(server->address()).ok());
        client.setSession("s1");
        QList<base::Result<core::Snapshot>> pictures;
        client.askSnapshot(u"nas"_s, 480, 0.5,
                           [&pictures](const base::Result<core::Snapshot>& picture) { pictures.append(picture); });
        QTRY_COMPARE(server->count("/api/v1/devices/"), 1);
        if (lostFirst) {
            // A gateway's page in the server's place: contact is lost while the picture is out.
            const base::Result<QHash<QString, core::Status>> look =
                answerTo<QHash<QString, core::Status>>([&](const auto& told) { client.askStatus(told); });
            QVERIFY(!look.ok());
            QCOMPARE(look.error().code, std::string("refused"));
            // The server goes, its lines with it; and a moment later, while Windows still tries
            // for the picture once more, something else answers at the address.
            server.reset();
            QTest::qWait(triedFor / 4);
        } else {
            // Nobody new is let in at the server's address from now on, and a look finds nobody
            // there: Windows tries for it for its second or two. Half way, the server goes, and
            // the picture's line with it: Qt tries for the picture again, and is still trying when
            // the look fails and contact is lost. A moment later something else answers there.
            server->takeNoMore();
            QList<base::Result<QHash<QString, core::Status>>> looks;
            client.askStatus([&looks](const base::Result<QHash<QString, core::Status>>& look) { looks.append(look); });
            QTest::qWait(triedFor / 2);
            QVERIFY(looks.isEmpty());
            server.reset();
            QTRY_COMPARE_WITH_TIMEOUT(looks.size(), 1, 30'000);
            QVERIFY(!looks[0].ok());
            QCOMPARE(looks[0].error().code, std::string(core::ServerClient::unreachable));
            QVERIFY(pictures.isEmpty());
        }
        // Something else answers there now, whatever it is asked.
        StandIn other([](const Asked&) { return Answered{200, "<html>It works!</html>"_ba, "text/html"_ba}; }, port);
        if (!other.isListening()) {
            QSKIP("Windows has given the port to somebody else in the meantime.");
        }
        QTRY_COMPARE_WITH_TIMEOUT(pictures.size(), 1, 30'000);
        QVERIFY(!pictures[0].ok());
        // (As long as Windows would have gone on trying, and then some.)
        QTest::qWait(triedFor + 500);
        QCOMPARE(pictures.size(), 1);
        for (const Asked& one : std::as_const(other.asked)) {
            QCOMPARE(one.path, QByteArray("/healthz"));
            QVERIFY(!one.headers.contains("cookie"));
        }
    }

    void a401InAnotherShapeThanTheServersIsNotTheNewsThatASessionIsGone()
    {
        // Something that has taken the server's place and wants a sign-in of its own for
        // whatever it is asked. That is no QKVM's word that the session is gone: nobody is
        // signed out by it, contact is lost, and what answers there is asked what it is.
        StandIn server([](const Asked& asked) {
            return asked.path == "/healthz" ? Answered{200, aQkvm}
                                            : Answered{401, "<html>Sign in to go on</html>"_ba, "text/html"_ba};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& told) { client.askWall(told); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string("refused"));
        QCOMPARE(client.session(), QByteArray("s1"));
        QVERIFY(!answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        QCOMPARE(server.asked.size(), 3);
        QCOMPARE(server.asked[1].path, QByteArray("/healthz"));
        QVERIFY(!server.asked[1].headers.contains("cookie"));
    }

    void whatIsNoPictureOfAKindThisProgramDrawsIsNoQkvmsAnswer_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("calledItself");
        QTest::addColumn<bool>("isOne");
        const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a") + QByteArray(200, 'x');
        QTest::newRow("a JPEG") << aJpeg() << "image/jpeg"_ba << true;
        QTest::newRow("a PNG") << png << "image/png"_ba << true;
        QTest::newRow("a JPEG, with more said of its kind") << aJpeg() << "image/jpeg; q=1"_ba << true;
        QTest::newRow("a page") << "<html>Welcome</html>"_ba << "text/html"_ba << false;
        QTest::newRow("what calls itself a picture and begins as none") << "{}"_ba << "image/jpeg"_ba << false;
        QTest::newRow("a picture that does not say it is one") << aJpeg() << "application/octet-stream"_ba << false;
        QTest::newRow("a picture of a kind that is not drawn")
            << "GIF89a"_ba + QByteArray(200, 'x') << "image/gif"_ba << false;
    }

    void whatIsNoPictureOfAKindThisProgramDrawsIsNoQkvmsAnswer()
    {
        QFETCH(const QByteArray, body);
        QFETCH(const QByteArray, calledItself);
        QFETCH(const bool, isOne);
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            return asked.path.contains("/snapshot") ? Answered{200, body, calledItself} : emptyWall(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Snapshot> picture =
            answerTo<core::Snapshot>([&](const auto& told) { client.askSnapshot(u"nas"_s, 480, 0.5, told); });
        QCOMPARE(picture.ok(), isOne);
        if (!isOne) {
            QCOMPARE(picture.error().code, std::string("bad_answer"));
        }
        // What is none was not a QKVM's answer: contact is lost, and the next call asks first.
        QVERIFY(answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        QCOMPARE(server.count("/healthz"), isOne ? 0 : 1);
    }

    void whatAQkvmSaidOfItselfBeforeContactWasLostDoesNotBringContactBack()
    {
        // What answers there was asked what it is, by whoever keeps the wall, and is slow to
        // say. Before it has, a call gets a gateway's page: contact is lost. The answer that
        // comes then was to a question from before that, and says nothing of what answers now.
        int saidOfItself = 0;
        bool gateway = true;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/healthz") {
                Answered itself{200, aQkvm};
                itself.afterMs = ++saidOfItself == 1 ? 600 : 0;
                return itself;
            }
            return gateway ? Answered{502, "<html>Bad Gateway</html>"_ba, "text/html"_ba} : emptyWall(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        QList<base::Result<core::Health>> early;
        client.askHealth([&early](const base::Result<core::Health>& health) { early.append(health); });
        QTRY_COMPARE(server.count("/healthz"), 1);
        QVERIFY(!answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        QVERIFY(early.isEmpty());
        QTRY_COMPARE(early.size(), 1);
        QVERIFY(early[0].ok());
        gateway = false;
        QVERIFY(answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        // Asked anew, since contact was lost; and the wall behind that answer.
        QCOMPARE(server.count("/healthz"), 2);
        QCOMPARE(server.asked.constLast().path, QByteArray("/api/v1/wall"));
        QCOMPARE(server.asked[server.asked.size() - 2].path, QByteArray("/healthz"));

        // Whoever asks it after contact was lost has asked for everybody: the next call goes out
        // behind that answer, and it is not asked a second time.
        gateway = true;
        QVERIFY(!answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        gateway = false;
        QVERIFY(answerTo<core::Health>([&](const auto& told) { client.askHealth(told); }).ok());
        QCOMPARE(server.count("/healthz"), 3);
        QVERIFY(answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        QCOMPARE(server.count("/healthz"), 3);
    }

    void aQkvmThatComesBackSpeakingAnotherVersionIsToldAsThat()
    {
        // Contact is lost, and what answers there afterwards is a QKVM that speaks another
        // version of its API: one of the two has to be updated. Told as that, with the versions
        // it speaks, and not as a call that was merely not sent.
        QByteArray apis = R"(["v1"])";
        bool gateway = false;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, R"({"status": "ok", "version": "3.0.0", "api": )" + apis + "}"};
            }
            return gateway ? Answered{502, "<html>Bad Gateway</html>"_ba, "text/html"_ba} : emptyWall(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        gateway = true;
        QVERIFY(!answerTo<core::Wall>([&](const auto& told) { client.askWall(told); }).ok());
        gateway = false;
        apis = R"(["v2"])";
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& told) { client.askWall(told); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string(core::ServerClient::otherVersion));
        QCOMPARE(wall.error().message,
                 std::string("This qKVM speaks v2 of its API, and this program speaks v1. One of the two has to be "
                             "updated."));
        // Nothing went to it with the session, and nothing may have been done.
        QCOMPARE(server.count("/api/v1/wall"), 1);
        QVERIFY(!client.mayHaveBeenDone(u"any"_s, wall.error()));
    }

    void howLongAServerAsksToBeLeftAloneCountsOnlyInItsOwnAnswerAndNotForLongerThanFiveMinutes_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("says");
        QTest::addColumn<int>("waitMs");
        const QByteArray tooMany =
            R"({"code": "rate_limited", "message": "Too many attempts. Try again in a minute."})";
        const QByteArray page = "<html><head><title>503 Service Unavailable</title></head></html>";
        QTest::newRow("the server's own refusal") << 429 << tooMany << "7"_ba << 7000;
        QTest::newRow("the server's own refusal, for an hour") << 429 << tooMany << "3600"_ba << 300'000;
        QTest::newRow("a gateway's page") << 503 << page << "3600"_ba << 0;
        QTest::newRow("a refusal in other words than a qKVM's") << 503 << R"({"error": "upstream"})"_ba << "30"_ba << 0;
        QTest::newRow("what is no number") << 429 << tooMany << "Wed, 21 Oct 2026 07:28:00 GMT"_ba << 0;
    }

    void howLongAServerAsksToBeLeftAloneCountsOnlyInItsOwnAnswerAndNotForLongerThanFiveMinutes()
    {
        QFETCH(const int, status);
        QFETCH(const QByteArray, body);
        QFETCH(const QByteArray, says);
        QFETCH(const int, waitMs);
        StandIn server([&](const Asked& asked) {
            return asked.path == "/healthz" ? Answered{200, aQkvm}
                                            : Answered{status, body, "application/json", {"Retry-After: " + says}};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Account> screen =
            answerTo<core::Account>([&](const auto& done) { client.openDisplay("qkvm_" + QByteArray(43, 'k'), done); });
        QVERIFY(!screen.ok());
        QCOMPARE(client.linkWaitSaidMs(), waitMs);
    }

    void theLimitOnAWholeAnswerGoesWithItsAnswer()
    {
        // Every call has a limit on its whole, twice as long as nothing may come: half a minute
        // for a picture. What keeps that time is let go with the answer, and does not live it out
        // for every picture of a wall.
        const QByteArray jpeg = QByteArray::fromHex("ffd8ffe000104a464946") + QByteArray(2000, 'x');
        StandIn server([&jpeg](const Asked&) { return Answered{200, jpeg, "image/jpeg"_ba}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        QCOMPARE(client.limitsAlive(), 0);
        int came = 0;
        for (int one = 0; one < 12; ++one) {
            client.askSnapshot(u"nas"_s, 480, 0.5,
                               [&came](const base::Result<core::Snapshot>& picture) { came += picture.ok() ? 1 : 0; });
        }
        QCOMPARE(client.limitsAlive(), 12);
        QTRY_COMPARE(came, 12);
        QTRY_COMPARE_WITH_TIMEOUT(client.limitsAlive(), 0, 3000);
    }

    void whatIsAskedOfAMachineIsGivenLongerThanAnOrdinaryCall()
    {
        // An ordinary call is given ten seconds. A machine that types a text, or has its power button
        // held, takes its time: giving up on it would invite a second try.
        StandIn server([](const Asked&) {
            Answered late{200, R"({"ok": true, "note": null})"};
            late.afterMs = 12'000;
            return late;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Done> done = answerTo<core::Done>(
            [&](const auto& told) { client.act(u"nas"_s, core::Action::type(u"a long text"_s, true), told); });
        QVERIFY2(done.ok(), done.ok() ? "" : done.error().message.c_str());
        QCOMPARE(server.asked.size(), 1);
    }

    void theWayIntoAMachinesConsoleIsAsked()
    {
        const QByteArray way =
            QJsonDocument(QJsonObject{{u"url"_s, u"https://192.168.1.60/console?pass=abc"_s}}).toJson();
        StandIn server([&](const Asked&) { return Answered{200, way}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<QUrl> entered =
            answerTo<QUrl>([&](const auto& done) { client.askConsole(u"a b/c"_s, done); });
        QVERIFY(entered.ok());
        QCOMPARE(entered.value(), QUrl(u"https://192.168.1.60/console?pass=abc"_s));
        QCOMPARE(server.asked.size(), 1);
        // Making a pass out changes something on the machine: asked as a change is.
        QCOMPARE(server.asked[0].method, QByteArray("POST"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/devices/a%20b%2Fc/console"));
        QCOMPARE(server.asked[0].headers.value("x-qkvm"), QByteArray("1"));
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=s1"));
    }

    void aPassThatIsRefusedOrNamesNoWebAddressIsNone()
    {
        // (Built, not written out: what moc reads of a test may hold neither two slashes in a row nor
        // an apostrophe by itself.)
        const QByteArray no =
            QJsonDocument(QJsonObject{{u"code"_s, u"not_allowed"_s},
                                      {u"message"_s, u"A way into a console is for a person's browser"_s}})
                .toJson();
        StandIn refusing([&](const Asked&) { return Answered{403, no}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(refusing.address()).ok());
        const base::Result<QUrl> refused = answerTo<QUrl>([&](const auto& done) { client.askConsole(u"s"_s, done); });
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, std::string("not_allowed"));
        QCOMPARE(refused.error().message, std::string("A way into a console is for a person's browser"));

        const QByteArray program =
            QJsonDocument(QJsonObject{{u"url"_s, u"file:///C:/Windows/System32/calc.exe"_s}}).toJson();
        StandIn odd([&](const Asked&) { return Answered{200, program}; });
        QVERIFY(client.setAddress(odd.address()).ok());
        const base::Result<QUrl> none = answerTo<QUrl>([&](const auto& done) { client.askConsole(u"s"_s, done); });
        QVERIFY(!none.ok());
        QCOMPARE(none.error().code, std::string("bad_answer"));
    }

    void theSavedActionsAreAsked()
    {
        StandIn server([](const Asked&) {
            return Answered{
                200,
                R"({"actions": [{"id": "a1b2", "name": "Lock", "kind": "keys", "keys": "MetaLeft,KeyL", "enter": false,
                                 "danger": false, "changed": 1}]})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<QList<core::SavedAction>> saved =
            answerTo<QList<core::SavedAction>>([&](const auto& done) { client.askSavedActions(done); });
        QVERIFY(saved.ok());
        QCOMPARE(saved.value().size(), 1);
        QCOMPARE(saved.value().constFirst().name, u"Lock"_s);
        QCOMPARE(server.asked[0].method, QByteArray("GET"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/actions"));
    }

    void theComputersAKvmCanWakeAreAsked()
    {
        StandIn server([](const Asked&) {
            return Answered{200, R"({"targets": [{"mac": "00:11:22:aa:bb:cc", "name": "NAS"}]})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<QList<core::WakeTarget>> targets =
            answerTo<QList<core::WakeTarget>>([&](const auto& done) { client.askWakeTargets(u"a b/c"_s, done); });
        QVERIFY(targets.ok());
        QCOMPARE(targets.value().size(), 1);
        QCOMPARE(targets.value().constFirst().mac, u"00:11:22:aa:bb:cc"_s);
        QCOMPARE(server.asked[0].method, QByteArray("GET"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/devices/a%20b%2Fc/wol"));
    }

    void anAnswerThatIsTooLargeIsCutOff()
    {
        StandIn server([](const Asked&) { return Answered{200, QByteArray(core::largestAnswerBytes + 1000, ' ')}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string("too_large"));
    }

    void brokenJsonIsRefused()
    {
        StandIn server([](const Asked&) { return Answered{200, R"({"interval": 1, "devices": [)"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(wall.error().code, std::string("bad_answer"));
    }

    void anAnswerThatBreaksOffIsNoAnswer()
    {
        // The server says how long its answer is, sends less, and the line falls under it. What
        // came is read as nothing: half a picture is no picture, and of something that was
        // asked of a machine nobody knows whether it was done.
        const QByteArray jpeg = QByteArray::fromHex("ffd8ffe000104a464946") + QByteArray(20'000, 'x');
        const QByteArray done = R"({"ok": true, "note": null})";
        const QByteArray file = "{\"format\": \"qkvm-export\", \"sealed\": {\"v\": 1}}";
        // How much of each answer comes before the line falls: all but the last of it; or all
        // of it, with more said to follow (the one cut after which what came still reads whole).
        bool allOfIt = false;
        StandIn server([&](const Asked& asked) {
            // (After an answer that broke off, what answers there is asked what it is before
            // anything else goes to it: a QKVM, here.)
            if (asked.path == "/healthz") {
                return Answered{200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"]})"};
            }
            const bool picture = asked.path.contains("/snapshot");
            const QByteArray whole = picture ? jpeg : asked.path == "/api/v1/export" ? file : done;
            const qsizetype missing = allOfIt ? 0 : whole.size() / 4;
            Answered cut(200, whole.first(whole.size() - missing), picture ? "image/jpeg"_ba : "application/json"_ba);
            cut.shortBy = static_cast<int>(missing) + (allOfIt ? 9 : 0);
            return cut;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        for (const bool whole : {false, true}) {
            allOfIt = whole;
            const base::Result<core::Snapshot> picture =
                answerTo<core::Snapshot>([&](const auto& told) { client.askSnapshot(u"nas"_s, 480, 0.5, told); });
            QVERIFY2(!picture.ok(), "a picture that broke off was handed on as a picture");
            QCOMPARE(picture.error().code, std::string(core::ServerClient::unreachable));
            QCOMPARE(picture.error().message, std::string("qKVM's answer broke off before all of it had come."));
            const base::Result<core::Done> acted = answerTo<core::Done>(
                [&](const auto& told) { client.act(u"nas"_s, core::Action::plain(u"atx_power"_s), told); });
            QVERIFY2(!acted.ok(), "an answer that broke off was taken for the machine's word that it is done");
            QCOMPARE(acted.error().code, std::string(core::ServerClient::unreachable));
            // That is no refusal: it may have been done, and is not asked a second time.
            QVERIFY(client.mayHaveBeenDone(u"any"_s, acted.error()));
            const base::Result<core::WallFile> written = answerTo<core::WallFile>(
                [&](const auto& told) { client.exportWall(u"my own"_s, u"for the file"_s, {u"devices"_s}, told); });
            QVERIFY2(!written.ok(), "a file that broke off was handed on to be written");
            QCOMPARE(written.error().code, std::string(core::ServerClient::unreachable));
        }
        QTest::qWait(300);
        QCOMPARE(server.count("/api/v1/devices/nas/actions"), 2);
        QCOMPARE(server.asked.size() - server.count("/healthz"), 6);
    }

    void aServerThatSendsElsewhereIsNotFollowed()
    {
        StandIn server(
            [](const Asked&) { return Answered{302, {}, "text/plain", {"Location: http://127.0.0.1:1/elsewhere"}}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const base::Result<core::Wall> wall = answerTo<core::Wall>([&](const auto& done) { client.askWall(done); });
        QVERIFY(!wall.ok());
        QCOMPARE(server.asked.size(), 1);
    }

    void aFileOfTheWallIsAskedForWithThePasswordTypedAgain()
    {
        StandIn server([](const Asked&) {
            return Answered{
                200,
                "{\"format\": \"qkvm-export\", \"sealed\": {\"v\": 1}}",
                "application/json",
                {"Content-Disposition: attachment; filename=\"qkvm-2026-10-11.qkvm\"", "X-QKVM-Not-Opened: 1"}};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::WallFile> file = answerTo<core::WallFile>([&](const auto& done) {
            client.exportWall(u"my own"_s, u"for the file"_s, {u"devices"_s, u"actions"_s}, done);
        });
        QVERIFY(file.ok());
        QCOMPARE(file.value().text, QByteArray("{\"format\": \"qkvm-export\", \"sealed\": {\"v\": 1}}"));
        QCOMPARE(file.value().notOpened, 1);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("POST"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/export"));
        QCOMPARE(server.asked[0].headers.value("x-qkvm"), QByteArray("1"));
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=s1"));
        QCOMPARE(QJsonDocument::fromJson(server.asked[0].body).object(),
                 (QJsonObject{{u"password"_s, u"my own"_s},
                              {u"passphrase"_s, u"for the file"_s},
                              {u"parts"_s, QJsonArray{u"devices"_s, u"actions"_s}}}));
    }

    void aFileAndWhatIsSaidOfOneMayBeLargerThanAnyOtherAnswer()
    {
        // A wall of two thousand machines, with a certificate each, is megabytes.
        const QByteArray large =
            "{\"format\": \"qkvm-export\", \"devices\": [" + QByteArray(core::largestAnswerBytes + 1000, ' ') + "]}";
        QJsonArray leftOut;
        for (int number = 0; number < 3000; ++number) {
            leftOut.append(QString(1000, u'a'));
        }
        const QByteArray told = QJsonDocument(QJsonObject{{u"encrypted"_s, false},
                                                          {u"devices"_s, QJsonValue::Null},
                                                          {u"actions"_s, QJsonValue::Null},
                                                          {u"alerts"_s, QJsonValue::Null},
                                                          {u"wall"_s, QJsonValue::Null},
                                                          {u"left_out"_s, leftOut}})
                                    .toJson(QJsonDocument::Compact);
        QVERIFY(told.size() > core::largestAnswerBytes);
        StandIn server(
            [&](const Asked& asked) { return Answered{200, asked.path == "/api/v1/export" ? large : told}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::WallFile> file = answerTo<core::WallFile>(
            [&](const auto& done) { client.exportWall(u"my own"_s, {}, {u"devices"_s}, done); });
        QVERIFY(file.ok());
        QCOMPARE(file.value().text.size(), large.size());
        const base::Result<core::FileContents> read =
            answerTo<core::FileContents>([&](const auto& done) { client.readFile(large, {}, done); });
        QVERIFY(read.ok());
        QCOMPARE(read.value().leftOut.size(), 200);
        // (And the file went to the server whole.)
        QCOMPARE(QJsonDocument::fromJson(server.asked[1].body).object()["file"_L1].toString().size(), large.size());
    }

    void aWrongPasswordForAFileIsTheServersWordAndNotTheSessionsEnd()
    {
        StandIn server(
            [](const Asked&) { return Answered{401, R"({"code": "bad_password", "message": "Wrong password"})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::WallFile> file = answerTo<core::WallFile>(
            [&](const auto& done) { client.exportWall(u"not it"_s, {}, {u"devices"_s}, done); });
        QVERIFY(!file.ok());
        QCOMPARE(file.error().code, std::string("bad_password"));
        QCOMPARE(file.error().message, std::string("Wrong password"));
    }

    void aFileIsReadAndTakenInByTheServer()
    {
        const QByteArray told =
            R"({"encrypted": false, "secrets": false, "made": 1791676800, "program": "0.1.0", "devices": [], "actions": null, "alerts": null, "wall": {}, "left_out": []})";
        StandIn server([&](const Asked& asked) {
            return asked.path == "/api/v1/import/read"
                       ? Answered{200, told}
                       : Answered{200,
                                  R"({"devices": 2, "actions": 0, "alerts": false, "wall": false, "left_out": []})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const QByteArray file = "{\"devices\": []}";
        const base::Result<core::FileContents> read =
            answerTo<core::FileContents>([&](const auto& done) { client.readFile(file, u"of the file"_s, done); });
        QVERIFY(read.ok());
        QCOMPARE(read.value().program, u"0.1.0"_s);
        QVERIFY(read.value().devices.has_value());
        QCOMPARE(server.asked[0].method, QByteArray("POST"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/import/read"));
        QCOMPARE(QJsonDocument::fromJson(server.asked[0].body).object(),
                 (QJsonObject{{u"file"_s, u"{\"devices\": []}"_s}, {u"passphrase"_s, u"of the file"_s}}));

        const base::Result<core::Taken> taken = answerTo<core::Taken>(
            [&](const auto& done) { client.takeFile(file, u"of the file"_s, {u"devices"_s}, done); });
        QVERIFY(taken.ok());
        QCOMPARE(taken.value().devices, 2);
        QCOMPARE(server.asked[1].path, QByteArray("/api/v1/import"));
        QCOMPARE(QJsonDocument::fromJson(server.asked[1].body).object(),
                 (QJsonObject{{u"file"_s, u"{\"devices\": []}"_s},
                              {u"passphrase"_s, u"of the file"_s},
                              {u"take"_s, QJsonArray{u"devices"_s}}}));
    }

    void aFileThatIsNotOneIsRefusedInTheServersWords()
    {
        StandIn server([](const Asked& asked) {
            return asked.path == "/api/v1/import/read"
                       ? Answered{422,
                                  R"({"code": "passphrase_needed", "message": "This file is encrypted. Enter the passphrase it was made with."})"}
                       : Answered{401, R"({"code": "unauthorized", "message": "Sign in first"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::FileContents> read =
            answerTo<core::FileContents>([&](const auto& done) { client.readFile("{}", {}, done); });
        QVERIFY(!read.ok());
        QCOMPARE(read.error().code, std::string("passphrase_needed"));
        // A session that is gone is that, as for every call but the one that asks for a password.
        const base::Result<core::Taken> taken =
            answerTo<core::Taken>([&](const auto& done) { client.takeFile("{}", {}, {u"devices"_s}, done); });
        QVERIFY(!taken.ok());
        QCOMPARE(taken.error().code, std::string(core::ServerClient::signedOut));
    }

    void aServerThatIsNotThereIsToldAsThat()
    {
        // A port nothing listens on.
        const core::testing::NobodyThere nobody;
        core::ServerClient client;
        QVERIFY(client.setAddress(nobody.address()).ok());
        const base::Result<core::Session> session =
            answerTo<core::Session>([&](const auto& done) { client.askSession(done); });
        QVERIFY(!session.ok());
        QCOMPARE(session.error().code, std::string(core::ServerClient::unreachable));
    }

    void nothingIsHandedToACallerThatIsGone()
    {
        StandIn server([](const Asked&) { return Answered{200, R"({"interval": 1, "devices": []})"}; });
        bool called = false;
        {
            core::ServerClient client;
            QVERIFY(client.setAddress(server.address()).ok());
            client.askWall([&](const base::Result<core::Wall>&) { called = true; });
        }
        QTest::qWait(300);
        QVERIFY(!called);
    }

    void everyQuestionGoesToTheVersionOfTheApiThisProgramSpeaks()
    {
        // The stand-in says yes to whatever comes: what counts here is where each question went.
        // (Of itself it says what a QKVM says: an answer that is none is contact lost, and what
        // had not left by then is given up, which is not what this is about.)
        StandIn server([](const Asked& asked) {
            return asked.path == "/healthz" ? Answered{200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"]})"}
                                            : Answered{200, "{}"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        // What each asking is given to tell its answer to: the asking is counted, and the answer
        // when it comes.
        int asked = 0;
        int answered = 0;
        const auto noted = [&asked, &answered] {
            ++asked;
            return [&answered](const auto&) { ++answered; };
        };

        // Every asking the client has, each once, and each when the one before was answered: a
        // "{}" is no QKVM's answer to most of them, which is contact lost, and what has not left
        // by then is given up (it would never be seen where it goes). One that is added to the
        // client is added here: only then is it seen where it goes.
        const auto came = [&asked, &answered] {
            return QTest::qWaitFor([&asked, &answered] { return answered == asked; }, 20000);
        };
        client.askHealth(noted());
        QVERIFY(came());
        client.askSession(noted());
        QVERIFY(came());
        client.signIn(u"anna"_s, u"correct horse"_s, u"123456"_s, true, noted());
        QVERIFY(came());
        client.openDisplay("qkvm_" + QByteArray(43, 'k'), noted());
        QVERIFY(came());
        client.askWall(noted());
        QVERIFY(came());
        client.arrange({u"a b/c"_s}, noted());
        QVERIFY(came());
        client.askStatus(noted());
        QVERIFY(came());
        client.askSnapshot(u"a b/c"_s, 480, 0.5, noted());
        QVERIFY(came());
        client.exportWall(u"my own"_s, u"for the file"_s, core::wallFileParts(), noted());
        QVERIFY(came());
        client.readFile("{}", u"of the file"_s, noted());
        QVERIFY(came());
        client.takeFile("{}", u"of the file"_s, core::wallFileParts(), noted());
        QVERIFY(came());
        client.askLive(u"a b/c"_s, noted());
        QVERIFY(came());
        // (The stand-in's yes holds no pass: the question goes out, and no line is opened for it.)
        client.openLive(u"a b/c"_s, core::Live::Need{1280, 20}, noted());
        QVERIFY(came());
        client.act(u"a b/c"_s, core::Action::press(u"ShiftLeft"_s), noted());
        QVERIFY(came());
        client.askSavedActions(noted());
        QVERIFY(came());
        client.askConsole(u"a b/c"_s, noted());
        QVERIFY(came());
        client.askWakeTargets(u"a b/c"_s, noted());
        QVERIFY(came());
        client.signOut(noted());
        QVERIFY(came());
        // (Every one of them reached the server, and beside them only what the server says of
        // itself, which is asked anew after an answer that was none: said with where each went.)
        QByteArrayList went;
        for (const Asked& one : std::as_const(server.asked)) {
            went.append(one.path);
        }
        QVERIFY2(server.asked.size() - server.count("/healthz") == asked - 1, went.join(' ').constData());

        // What the server says of itself has one place, whatever version it speaks. Everything
        // else is under the version this program speaks, and nowhere else.
        const QByteArray versioned = "/api/" + QByteArray(core::spokenApi.data(), core::spokenApi.size()) + '/';
        QCOMPARE(versioned, QByteArray("/api/v1/"));
        int ofItself = 0;
        for (const Asked& one : std::as_const(server.asked)) {
            if (one.path == "/healthz") {
                ++ofItself;
                continue;
            }
            QVERIFY2(one.path.startsWith(versioned) && one.path.size() > versioned.size(), one.path.constData());
        }
        QVERIFY(ofItself >= 1);

        // And so is a machine's live line, which is no question like the others.
        const std::unique_ptr<core::LiveLink> line =
            client.openLive(u"a b/c"_s, "p4ss-for-one-line_0123456789", {1280, 20});
        QTRY_COMPARE(server.live.lines, 1);
        QVERIFY2(server.live.path.startsWith(versioned), server.live.path.constData());
    }

    // --- a machine live, as a wall asks the place its machines come from for it ---

    void aMachineIsHadLiveByThePassAndThenTheLine()
    {
        StandIn server(
            [](const Asked&) { return Answered{200, R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        core::WallSource& source = client;
        std::unique_ptr<core::Live> live;
        QList<base::Error> refused;
        source.openLive(u"a b"_s, {1280, 20}, [&](base::Result<std::unique_ptr<core::Live>> made) {
            if (made) {
                live = std::move(made).value();
            } else {
                refused.append(made.error());
            }
        });
        // Nothing is handed over before the server has answered.
        QVERIFY(live == nullptr);
        QTRY_VERIFY(live != nullptr);
        QVERIFY(refused.isEmpty());
        // The pass was asked for as everything is asked, with the session.
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("POST"));
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/devices/a%20b/live"));
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=s1"));
        // And the line opened with it: the pass is its first word, with what was asked for, and
        // the session is not on it.
        QList<QByteArray> pictures;
        connect(live.get(), &core::Live::pictureCame, live.get(),
                [&](const core::Live::Picture& picture) { pictures.append(picture.data); });
        QTRY_COMPARE(server.live.lines, 1);
        QCOMPARE(server.live.path, QByteArray("/api/v1/devices/a%20b/live"));
        QVERIFY(!server.live.headers.contains("cookie"));
        QTRY_COMPARE(server.live.said.size(), 1);
        QCOMPARE(server.live.said.at(0)[u"ticket"_s].toString(), u"p4ss-for-one-line_0123456789"_s);
        QCOMPARE(server.live.said.at(0)[u"max_w"_s].toInt(), 1280);
        QCOMPARE(server.live.said.at(0)[u"max_fps"_s].toInt(), 20);
        QTRY_VERIFY(live->isOpen());
        // What comes over the line comes out of what was handed over, and that ends the line.
        server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_COMPARE(pictures, (QList<QByteArray>{"a JPEG"}));
        live->close();
        QTRY_VERIFY(!server.live.isConnected());
        QCOMPARE(server.asked.size(), 1);
    }

    void aServerThatHasNoLivePictureOfTheMachineOpensNoLine()
    {
        StandIn server([](const Asked&) {
            return Answered{404, R"({"code": "no_live", "message": "This machine has no live picture"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        QList<base::Error> refused;
        int handedOver = 0;
        client.openLive(u"a"_s, {800, 30}, [&](const base::Result<std::unique_ptr<core::Live>>& made) {
            if (made) {
                ++handedOver;
            } else {
                refused.append(made.error());
            }
        });
        QTRY_COMPARE(refused.size(), 1);
        // The server's own words, as for every question.
        QCOMPARE(refused[0].code, std::string("no_live"));
        QCOMPARE(refused[0].message, std::string("This machine has no live picture"));
        QTest::qWait(200);
        QCOMPARE(handedOver, 0);
        QCOMPARE(server.live.lines, 0);
        QCOMPARE(server.asked.size(), 1);
    }

    void aPassIsNotSaidToAnotherServerThanTheOneThatGaveIt()
    {
        // The first is slow to give its pass, and another server is named meanwhile.
        StandIn first([](const Asked&) {
            Answered late{200, R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})"};
            late.afterMs = 300;
            return late;
        });
        StandIn second([](const Asked&) { return Answered{200, "{}"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(first.address()).ok());
        client.setSession("s1");
        QList<base::Error> refused;
        int handedOver = 0;
        client.openLive(u"a"_s, {800, 30}, [&](const base::Result<std::unique_ptr<core::Live>>& made) {
            if (made) {
                ++handedOver;
            } else {
                refused.append(made.error());
            }
        });
        QTRY_COMPARE(first.asked.size(), 1);
        QVERIFY(client.setAddress(second.address()).ok());
        // Whoever asked is told once, and no line is opened: not to the server that was named
        // since, which would hear the first one's pass, and not to the one that was left. (Qt
        // gives up a question that is out when another server is named, so the pass never comes;
        // where it came all the same, the client's own rule is in its way.)
        QTRY_COMPARE(refused.size(), 1);
        QCOMPARE(refused[0].code, std::string(core::ServerClient::unreachable));
        QTest::qWait(400);
        QCOMPARE(handedOver, 0);
        QCOMPARE(second.live.lines, 0);
        QCOMPARE(first.live.lines, 0);
        QVERIFY(second.asked.isEmpty());
    }

    void anAnswerThatComesAByteAtATimeIsGivenUpOn()
    {
        // What answers begins at once, and then sends a byte every half second, without end:
        // it never falls silent for as long as an answer may be waited for. The whole of an
        // answer has a limit of its own, twice that long: for what a server says of itself,
        // which is asked of whatever stands at an address, and for every other call.
        StandIn dripping([](const Asked&) {
            Answered slow{200, QByteArray(60'000, ' ')};
            slow.byteEveryMs = 500;
            return slow;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(dripping.address()).ok());
        QElapsedTimer waited;
        waited.start();
        QList<base::Result<core::Health>> ofItself;
        QList<base::Result<QHash<QString, core::Status>>> ofTheMachines;
        client.askHealth([&](const base::Result<core::Health>& health) { ofItself.append(health); });
        client.askStatus(
            [&](const base::Result<QHash<QString, core::Status>>& status) { ofTheMachines.append(status); });
        QVERIFY2(QTest::qWaitFor([&] { return !ofItself.isEmpty() && !ofTheMachines.isEmpty(); }, 35'000),
                 "what dribbles was never given up on");
        // Not before an answer has had its time (ten seconds without a byte would have ended it
        // sooner, and there were none), and told as a server that did not answer: not as one
        // that is no QKVM, and not as a refusal.
        QVERIFY2(waited.elapsed() >= 15'000, QByteArray::number(waited.elapsed()).constData());
        QVERIFY(!ofItself.constFirst().ok());
        QCOMPARE(ofItself.constFirst().error().code, std::string(core::ServerClient::unreachable));
        QCOMPARE(ofItself.constFirst().error().message, std::string("qKVM did not answer in time."));
        QVERIFY(!ofTheMachines.constFirst().ok());
        QCOMPARE(ofTheMachines.constFirst().error().code, std::string(core::ServerClient::unreachable));
        QCOMPARE(dripping.asked.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestServerClient)
#include "test_server_client.moc"
