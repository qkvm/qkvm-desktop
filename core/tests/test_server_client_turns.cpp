#include <algorithm>
#include <functional>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <qkvm/core/server_client.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

// How long a server asks to be left alone (`Retry-After`), kept where it says so in its own words: a sign-in that
// was refused for too many attempts, a live picture refused for too many lines, a history read refused for being
// busy (the web's W2b, W4b; "Retry-After is meant"). Nothing of that kind goes to it sooner. The history is read
// one at a time, whoever asks for it on this PC (two windows of the same server among them), as the server reads
// it for each account. And a picture may be as large as the server's are. These wait a second or two each: a
// program of their own, beside test_core_server_client.
class TestServerClientTurns : public QObject {
    Q_OBJECT

private:
    static constexpr const char* aQkvm = R"({"status": "ok", "version": "0.3.0", "api": ["v1"]})";
    static constexpr const char* signedInAs =
        R"({"ok": true, "user": {"id": "u1", "name": "anna", "role": "admin", "token": false}})";

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

    static Answered refusal(int status, const char* code, const char* message, const QByteArray& wait)
    {
        const QByteArray body = QJsonDocument(QJsonObject{{u"code"_s, QString::fromLatin1(code)},
                                                          {u"message"_s, QString::fromLatin1(message)}})
                                    .toJson(QJsonDocument::Compact);
        return {status, body, "application/json", {"Retry-After: " + wait}};
    }

    static const char* summary()
    {
        return R"({"since": 1000, "until": 2000, "first_seen": 1000, "last_seen": 2000, "watched": 1000,
                   "online": 1000, "share": 1, "changes": 0, "events": 0, "sensors": []})";
    }

private slots:
    void aSignInRefusedForTooManyAttemptsIsNotTriedAgainBeforeTheServerSaid()
    {
        bool refuses = true;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            return refuses ? refusal(429, "rate_limited", "Too many attempts. Try again in 2 s.", "2")
                           : Answered{200, signedInAs, "application/json", {"Set-Cookie: qkvm_session=s1"}};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const auto signIn = [&] {
            return answerTo<core::Account>(
                [&](const auto& done) { client.signIn(u"anna"_s, u"wrong"_s, QString(), false, done); });
        };
        QElapsedTimer since;
        since.start();
        base::Result<core::Account> account = signIn();
        QVERIFY(!account.ok());
        QCOMPARE(account.error().code, std::string("rate_limited"));
        QCOMPARE(server.count("/api/v1/login"), 1);
        // Again at once, and the server has stopped refusing meanwhile: nothing goes to it, and the person is told
        // how long is left, as the server says it.
        refuses = false;
        account = signIn();
        QVERIFY(!account.ok());
        QCOMPARE(account.error().code, std::string("rate_limited"));
        QVERIFY2(QString::fromStdString(account.error().message).startsWith(u"Too many attempts. Try again in "_s),
                 account.error().message.c_str());
        QCOMPARE(server.count("/api/v1/login"), 1);
        // Once that time is over, it goes.
        QTest::qWait(static_cast<int>(std::max<qint64>(0, 2100 - since.elapsed())));
        account = signIn();
        QVERIFY2(account.ok(), account.ok() ? "" : account.error().message.c_str());
        QCOMPARE(server.count("/api/v1/login"), 2);
    }

    void onlyTheServersOwnWordsHoldASignInBackAndAnotherServerIsNotHeldByIt_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("says");
        QTest::addColumn<bool>("held");
        const QByteArray tooMany = R"({"code": "rate_limited", "message": "Too many attempts. Try again in 60 s."})";
        QTest::newRow("the server's own refusal") << 429 << tooMany << "60"_ba << true;
        QTest::newRow("a gateway's page") << 429 << "<html>Too Many Requests</html>"_ba << "60"_ba << false;
        QTest::newRow("what is no number") << 429 << tooMany << "Wed, 21 Oct 2026 07:28:00 GMT"_ba << false;
        QTest::newRow("no time at all") << 429 << tooMany << "0"_ba << false;
    }

    void onlyTheServersOwnWordsHoldASignInBackAndAnotherServerIsNotHeldByIt()
    {
        QFETCH(const int, status);
        QFETCH(const QByteArray, body);
        QFETCH(const QByteArray, says);
        QFETCH(const bool, held);
        const auto answer = [&](const Asked& asked) {
            return asked.path == "/healthz" ? Answered{200, aQkvm}
                                            : Answered{status, body, "application/json", {"Retry-After: " + says}};
        };
        StandIn server(answer);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        const auto signIn = [&] {
            return answerTo<core::Account>(
                [&](const auto& done) { client.signIn(u"anna"_s, u"wrong"_s, QString(), false, done); });
        };
        QVERIFY(!signIn().ok());
        QVERIFY(!signIn().ok());
        QCOMPARE(server.count("/api/v1/login"), held ? 1 : 2);
        // Another server is another lock-out's.
        StandIn other(answer);
        QVERIFY(client.setAddress(other.address()).ok());
        QVERIFY(!signIn().ok());
        QCOMPARE(other.count("/api/v1/login"), 1);
    }

    void aLivePictureRefusedForTooManyLinesIsNotAskedForAgainBeforeTheServerSaid()
    {
        // One account has 32 lines at the most (W2b): beyond, 429 busy, and a pass runs out within half a minute.
        bool refuses = true;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            return refuses ? refusal(429, "busy", "This account has as many live pictures open as it may.", "2")
                           : Answered{200, R"({"v": 2, "ticket": "abcdefghijklmnopqrstuvwxyz012345"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        QElapsedTimer since;
        since.start();
        const auto pass = [&](const QString& machine) {
            return answerTo<QByteArray>([&](const auto& done) { client.askLive(machine, done); });
        };
        base::Result<QByteArray> given = pass(u"nas"_s);
        QVERIFY(!given.ok());
        QCOMPARE(given.error().code, std::string("busy"));
        QCOMPARE(server.count("/api/v1/devices/"), 1);
        // Of any machine: the lines are counted per account. Refused here, in the server's words, and not sent.
        refuses = false;
        given = pass(u"rack"_s);
        QVERIFY(!given.ok());
        QCOMPARE(given.error().code, std::string("busy"));
        QCOMPARE(given.error().message, std::string("This account has as many live pictures open as it may."));
        QCOMPARE(server.count("/api/v1/devices/"), 1);
        QTest::qWait(static_cast<int>(std::max<qint64>(0, 2100 - since.elapsed())));
        QVERIFY(pass(u"rack"_s).ok());
        QCOMPARE(server.count("/api/v1/devices/"), 2);
    }

    void theHistoryIsReadOneAtATimeWhoeverAsks()
    {
        // Two windows' Details of one server ask through one client: the second read goes once the first was answered,
        // as the server takes them of one account (a third while one waits would be 503 busy at once).
        QElapsedTimer clock;
        clock.start();
        QList<qint64> came;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            came.append(clock.elapsed());
            Answered answer{200, summary()};
            answer.afterMs = 400;
            return answer;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        int done = 0;
        client.askHistory(u"nas"_s, [&](const base::Result<core::HistorySummary>& read) { done += read.ok() ? 1 : 0; });
        client.askHistory(u"rack"_s,
                          [&](const base::Result<core::HistorySummary>& read) { done += read.ok() ? 1 : 0; });
        client.askHistorySamples(u"rack"_s, [&](const base::Result<core::HistorySamples>&) { ++done; });
        QTRY_COMPARE_WITH_TIMEOUT(done, 3, 10000);
        QCOMPARE(came.size(), 3);
        // (Each answer is given 400 ms after its question came; a timer of that length may end a little early.)
        QVERIFY2(came[1] - came[0] >= 300, qPrintable(u"%1, %2"_s.arg(came[0]).arg(came[1])));
        QVERIFY2(came[2] - came[1] >= 300, qPrintable(u"%1, %2"_s.arg(came[1]).arg(came[2])));
    }

    void aHistoryReadRefusedAsBusyIsNotAskedAgainBeforeTheServerSaid()
    {
        bool refuses = true;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/healthz") {
                return Answered{200, aQkvm};
            }
            return refuses ? refusal(503, "busy",
                                     "This account or token is reading the history already. Try again "
                                     "in a moment.",
                                     "2")
                           : Answered{200, summary()};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        QElapsedTimer since;
        since.start();
        const auto read = [&] {
            return answerTo<core::HistorySummary>([&](const auto& done) { client.askHistory(u"nas"_s, done); });
        };
        QVERIFY(!read().ok());
        QCOMPARE(server.count("/api/v1/devices/"), 1);
        refuses = false;
        // Asked again at once: it waits its time, and goes then.
        const base::Result<core::HistorySummary> again = read();
        QVERIFY2(again.ok(), again.ok() ? "" : again.error().message.c_str());
        QVERIFY2(since.elapsed() >= 1900, qPrintable(QString::number(since.elapsed())));
        QCOMPARE(server.count("/api/v1/devices/"), 2);
    }

    void aPictureMayBeAsLargeAsTheServersAre()
    {
        // The server takes a picture of up to 32 MiB from a machine (a large PNG of a big screen), and hands it on.
        const QByteArray large =
            QByteArray::fromHex("ffd8ffe000104a464946") + QByteArray(qsizetype{20} * 1024 * 1024, 'x');
        StandIn server([&](const Asked& asked) {
            return asked.path == "/healthz" ? Answered{200, aQkvm} : Answered{200, large, "image/jpeg"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        const base::Result<core::Snapshot> picture =
            answerTo<core::Snapshot>([&](const auto& done) { client.askSnapshot(u"nas"_s, 3840, 1, done); });
        QVERIFY2(picture.ok(), picture.ok() ? "" : picture.error().message.c_str());
        QCOMPARE(picture.value().picture.size(), large.size());
        QCOMPARE(core::largestPictureBytes, qsizetype{32} * 1024 * 1024);
    }
};

QTEST_GUILESS_MAIN(TestServerClientTurns)
#include "test_server_client_turns.moc"
