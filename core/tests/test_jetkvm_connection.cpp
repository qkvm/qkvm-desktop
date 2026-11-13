#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/jetkvm_connection.h>
#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/video_diagnostics.h>

#include "jetkvm_by_hand.h"
#include "jetkvm_viewer_by_hand.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
namespace json = qkvm::core::json;
using qkvm::core::JetKvmConnection;
using qkvm::core::JetKvmDevice;
using qkvm::core::SignInPacing;
using qkvm::core::testing::JetKvmByHand;
using qkvm::core::testing::JetKvmViewerByHand;

namespace {

JetKvmDevice::Pace quick()
{
    JetKvmDevice::Pace pace;
    pace.connectWithinMs = 3000;
    pace.quietMs = 3000;
    pace.transferMs = 3000;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.refusedHoldMs = 1200;
    pace.signInRetryMs = 800;
    pace.lineWithinMs = 4000;
    pace.pacing = std::make_shared<SignInPacing>();
    return pace;
}

JetKvmDevice::Where at(const JetKvmByHand& jet)
{
    JetKvmDevice::Where where;
    where.address = jet.address();
    return where;
}

JetKvmConnection::Pace watchedOften()
{
    JetKvmConnection::Pace pace;
    pace.watchEveryMs = 100;
    return pace;
}

// A JetKVM by hand with its viewer, and its driver.
struct Rig {
    explicit Rig(const JetKvmDevice::Pace& pace = quick()) : device(at(jet), u"secret"_s, pace) {}
    JetKvmByHand jet;
    JetKvmViewerByHand viewer{jet};
    JetKvmDevice device;
};

// What a connection said of its end, kept.
struct Ending {
    explicit Ending(JetKvmConnection& connection)
    {
        QObject::connect(&connection, &JetKvmConnection::ended, &connection,
                         [this](const QString& code, const QString& message) {
                             codes.append(code);
                             messages.append(message);
                             at.append(clock.elapsed());
                         });
        clock.start();
    }
    QStringList codes;
    QStringList messages;
    QList<qint64> at;
    QElapsedTimer clock;
};

bool opens(JetKvmConnection& connection)
{
    QSignalSpy opened(&connection, &JetKvmConnection::opened);
    connection.start();
    return opened.wait(20'000);
}

QByteArray rpc(const QJsonObject& said)
{
    QJsonObject whole = said;
    whole.insert(u"jsonrpc"_s, u"2.0"_s);
    return QJsonDocument(whole).toJson(QJsonDocument::Compact);
}

base::Result<json::Value> callOf(JetKvmConnection& connection, const QString& method, int withinMs = 20'000)
{
    std::optional<base::Result<json::Value>> answer;
    connection.call(method, {}, [&answer](base::Result<json::Value> what) { answer = std::move(what); });
    if (answer) {
        return base::Error{"at_once", "The answer came before the call came back."};
    }
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, withinMs)) {
        return base::Error{"never", "The call was never answered."};
    }
    return *answer;
}

} // namespace

// One WebRTC session to a JetKVM (M6 2f), against a JetKVM by hand on 127.0.0.1: its signalling line, the
// offer and the answer, the addresses, JSON-RPC and the events, and how long each may take.
class TestJetKvmConnection : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(qkvm::core::useWindowsOwnTls());
        QVERIFY2(qkvm::core::testing::JetKvmPeerByHand::filmIsThere(), qkvm::core::testing::JetKvmPeerByHand::noFilm);
    }

    // The offer goes as the JetKVM's own page sends it, on its signalling line with the session the driver
    // signed in for: {"type":"offer","data":{"sd": base64 of JSON {type, sdp}}}, with the two H.264 formats
    // and the application.
    void theOfferGoesAsTheJetKvmTakesIt()
    {
        Rig rig;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        QSignalSpy went(&connection, &JetKvmConnection::offerWent);
        connection.start();
        QTRY_COMPARE_WITH_TIMEOUT(rig.jet.offerMessages.size(), 1, 15000);
        QTRY_COMPARE(went.size(), 1);
        QCOMPARE(rig.jet.lineCookies, QList<QByteArray>{"authToken=jet.1"_ba});
        const QJsonObject message = QJsonDocument::fromJson(rig.jet.offerMessages.first()).object();
        QCOMPARE(message.keys(), (QStringList{u"data"_s, u"type"_s}));
        QCOMPARE(message.value(u"type"_s).toString(), u"offer"_s);
        const QJsonObject data = message.value(u"data"_s).toObject();
        QCOMPARE(data.keys(), QStringList{u"sd"_s});
        const QJsonObject offer =
            QJsonDocument::fromJson(QByteArray::fromBase64(data.value(u"sd"_s).toString().toLatin1())).object();
        QCOMPARE(offer.value(u"type"_s).toString(), u"offer"_s);
        const QByteArray sdp = offer.value(u"sdp"_s).toString().toUtf8();
        QVERIFY(sdp.contains("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"));
        QVERIFY(sdp.contains("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"));
        QVERIFY(sdp.contains("a=recvonly") && sdp.contains("m=application "));
    }

    // Its answer, bare, and its addresses one by one after it (with whichever mid: let go) open it; what it
    // says first on its line (`otaState`, `device-metadata`) is let go; both channels open, pictures come.
    void itsAnswerAndAddressesOpenIt_data()
    {
        QTest::addColumn<int>("mid");
        QTest::newRow("sdpMid 0") << static_cast<int>(JetKvmByHand::Mid::Zero);
        QTest::newRow("sdpMid empty") << static_cast<int>(JetKvmByHand::Mid::Empty);
        QTest::newRow("no sdpMid") << static_cast<int>(JetKvmByHand::Mid::None);
    }
    void itsAnswerAndAddressesOpenIt()
    {
        QFETCH(int, mid);
        Rig rig;
        rig.jet.midSaid = static_cast<JetKvmByHand::Mid>(mid);
        rig.jet.saysAWordFirst = true;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        const Ending ending(connection);
        int pictures = 0;
        int keys = 0;
        connect(&connection, &JetKvmConnection::pictureCame, this, [&](const QByteArray&, bool key, qint64 arrivedUs) {
            ++pictures;
            keys += key ? 1 : 0;
            QVERIFY(arrivedUs > 0);
        });
        QVERIFY(opens(connection));
        QVERIFY(connection.isOpen());
        QTRY_VERIFY_WITH_TIMEOUT(pictures >= 30 && keys >= 1, 15000);
        QCOMPARE(rig.viewer.current()->labels(), (QList<QByteArray>{"rpc", "hidrpc"}));
        QVERIFY(ending.codes.isEmpty());
    }

    void anErrorInsteadOfAnAnswerEndsIt()
    {
        Rig rig;
        rig.jet.saysInsteadOfAnAnswer = R"({"error":"busy\nwith\tanother"})";
        JetKvmConnection connection(rig.device, 1, watchedOften());
        const Ending ending(connection);
        connection.start();
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 15000);
        QCOMPARE(ending.codes.first(), u"bad_response"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM refused the request for video: busy with another"_s);
    }

    void noAnswerInTimeEndsIt()
    {
        Rig rig;
        rig.jet.answersNothing = true;
        JetKvmConnection::Pace pace = watchedOften();
        pace.answerWithinMs = 1500;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        QSignalSpy went(&connection, &JetKvmConnection::offerWent);
        connection.start();
        QVERIFY(went.wait(15000));
        const qint64 offered = ending.clock.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 15000);
        QCOMPARE(ending.codes.first(), u"timeout"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM did not answer the request for video"_s);
        QVERIFY2(ending.at.first() - offered >= 1300, qPrintable(QString::number(ending.at.first() - offered)));
    }

    // An answer that is none: not base64, base64 of what is no JSON, JSON with no description, one that is no
    // text, or one the offering side does not take.
    void anAnswerThatCannotBeReadEndsIt_data()
    {
        QTest::addColumn<QByteArray>("said");
        // (Escaped, not raw: Qt's moc does not read a raw text that ends in a quote and a brace.)
        const auto packed = [](const QByteArray& inner) {
            return QByteArray("{\"type\":\"answer\",\"data\":\"") + inner.toBase64() + QByteArray("\"}");
        };
        QTest::newRow("no base64") << QByteArray(R"({"type":"answer","data":"%%%"})");
        QTest::newRow("data no text") << QByteArray(R"({"type":"answer","data":{"sdp":"v=0"}})");
        QTest::newRow("no JSON in it") << packed("v=0 and nothing more");
        QTest::newRow("no sdp") << packed(R"({"type":"answer"})");
        QTest::newRow("sdp no text") << packed(R"({"type":"answer","sdp":17})");
        QTest::newRow("sdp empty") << packed(R"({"type":"answer","sdp":""})");
        QTest::newRow("no video in it") << packed(R"({"type":"answer","sdp":"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\n"})");
    }
    void anAnswerThatCannotBeReadEndsIt()
    {
        QFETCH(QByteArray, said);
        Rig rig;
        rig.jet.saysInsteadOfAnAnswer = said;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        const Ending ending(connection);
        connection.start();
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 15000);
        QCOMPARE(ending.codes.first(), u"bad_response"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM sent an answer that cannot be read"_s);
    }

    // Somebody signed in at its own page since: its line refuses the session, and the driver signs in once
    // more on the same road (JetKvmDevice::openSignalling), as for what it is.
    void theLineAsksOnceMoreAfterA401()
    {
        Rig rig;
        {
            JetKvmConnection first(rig.device, 1, watchedOften());
            QVERIFY(opens(first));
            first.end();
        }
        // (Its line refuses the session the question of which JetKVM it is was answered under, J5.)
        rig.jet.refuseLines = 1;
        {
            JetKvmConnection second(rig.device, 2, watchedOften());
            QVERIFY(opens(second));
            second.end();
        }
        QCOMPARE(rig.jet.signIns, 2);
        QCOMPARE(rig.jet.lineCookies,
                 (QList<QByteArray>{"authToken=jet.1"_ba, "authToken=jet.1"_ba, "authToken=jet.2"_ba}));
        // And since J5 a session replaced at its own page is refused first by that question, which goes under it: the
        // line is asked for under the new one alone.
        rig.jet.signInElsewhere();
        JetKvmConnection third(rig.device, 3, watchedOften());
        QVERIFY(opens(third));
        QCOMPARE(rig.jet.signIns, 3);
        QCOMPARE(rig.jet.lineCookies.last(), "authToken=jet.4"_ba);
        QCOMPARE(rig.jet.lineCookies.size(), 4);
    }

    // Its addresses before its answer (pion gathers as it answers): kept, and taken after the answer, as the
    // web's _await_answer takes them in either order.
    void addressesBeforeTheAnswerAreTakenAfterIt()
    {
        Rig rig;
        rig.jet.saysItsAddressesFirst = true;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        QVERIFY(opens(connection));
        QVERIFY(rig.jet.addressesSaid >= 1);
    }

    // The reading of a9b950b (L4): of its addresses before its answer, only one that may be taken later (2048
    // bytes at most) is kept for it, forty at the most: forty that are larger, said first, keep no place from
    // its own. (Before: forty of up to a line's largest each were kept, and its own found no place.)
    void addressesBeforeTheAnswerOverTheLargestAreNotKept()
    {
        Rig rig;
        rig.jet.saysItsAddressesFirst = true;
        for (int each = 0; each < 40; ++each) {
            rig.jet.firstAddresses.append("candidate:9 1 UDP 1 127.0.0.1 9 typ host " + QByteArray(3000, 'x'));
        }
        JetKvmConnection connection(rig.device, 1, watchedOften());
        QVERIFY(opens(connection));
        QVERIFY(rig.jet.addressesSaid >= 41);
    }

    // Its line ends before it answered: over, at once, not at the end of the wait for the answer.
    void theLineEndingBeforeTheAnswerEndsIt()
    {
        Rig rig;
        rig.jet.answersNothing = true;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        const Ending ending(connection);
        QSignalSpy went(&connection, &JetKvmConnection::offerWent);
        connection.start();
        QVERIFY(went.wait(15000));
        rig.jet.closeLines();
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 5000);
        QCOMPARE(ending.codes.first(), u"dropped"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM closed the connection"_s);
    }

    // The JetKVM closes its signalling line once it has said its addresses: the connection lives on.
    void theLineEndingAfterTheAnswerEndsNothing()
    {
        Rig rig;
        rig.jet.closesLineAfterItsAddresses = true;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        const Ending ending(connection);
        int pictures = 0;
        connect(&connection, &JetKvmConnection::pictureCame, this,
                [&](const QByteArray&, bool, qint64) { ++pictures; });
        QVERIFY(opens(connection));
        QTest::qWait(1500);
        const int before = pictures;
        QTRY_VERIFY_WITH_TIMEOUT(pictures > before + 30, 10000);
        QVERIFY(ending.codes.isEmpty());
        QVERIFY(connection.isOpen());
    }

    // Only a whole number of its own that waits answers a call: a stranger's number, the same number as
    // text, or one written half, answer nothing; the right one, after them, answers it.
    void callsAreMatchedByOurNumbersOnly()
    {
        Rig rig;
        rig.viewer.answersInstead = [](const QByteArray& call) {
            const qint64 id = QJsonDocument::fromJson(call).object().value(u"id"_s).toInteger();
            const QByteArray number = QByteArray::number(id);
            // (Escaped, not raw: Qt's moc loses the rest of the class after raw texts such as these.)
            const QByteArray begin = "{\"jsonrpc\":\"2.0\",\"id\":";
            return QList<QByteArray>{
                begin + "999,\"result\":\"a stranger's\"}",
                begin + '"' + number + "\",\"result\":\"an id that is text\"}",
                begin + number + ".0,\"result\":\"a number written half\"}",
                begin + number + ",\"result\":\"ours\"}",
            };
        };
        JetKvmConnection connection(rig.device, 1, watchedOften());
        QVERIFY(opens(connection));
        const base::Result<json::Value> answer = callOf(connection, u"getUSBState"_s);
        QVERIFY2(answer, answer ? "" : answer.error().message.c_str());
        QVERIFY(answer.value().isText());
        QCOMPARE(answer.value().text, u"ours"_s);
        // And its number again: nothing waits under it any more.
        const base::Result<json::Value> next = callOf(connection, u"getUSBState"_s);
        QVERIFY(next);
        QCOMPARE(next.value().text, u"ours"_s);
    }

    // Two calls answered the other way round (as a real one does now and then: row 1) are each their own.
    void twoCallsAnsweredTheOtherWayRoundAreEachTheirOwn()
    {
        Rig rig;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        QVERIFY(opens(connection));
        rig.viewer.answersTwoTheOtherWayRound = true;
        std::optional<base::Result<json::Value>> video;
        std::optional<base::Result<json::Value>> usb;
        connection.call(u"getVideoState"_s, {}, [&video](base::Result<json::Value> what) { video = std::move(what); });
        connection.call(u"getUSBState"_s, {}, [&usb](base::Result<json::Value> what) { usb = std::move(what); });
        QTRY_VERIFY_WITH_TIMEOUT(video && usb, 10000);
        if (!video.has_value() || !usb.has_value()) {
            QFAIL("not answered");
        }
        QVERIFY(*video && *usb);
        QVERIFY(video->value().isObject() && video->value().find(u"ready") != nullptr);
        QCOMPARE(usb->value().text, u"configured"_s);
    }

    // A call not answered in time fails as one that may have been done; one the JetKVM refuses fails in its
    // words.
    void aCallNotAnsweredInTimeFails()
    {
        Rig rig;
        JetKvmConnection::Pace pace = watchedOften();
        pace.callWithinMs = 800;
        JetKvmConnection connection(rig.device, 1, pace);
        QVERIFY(opens(connection));
        const base::Result<json::Value> refused = callOf(connection, u"noSuchMethod"_s);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code, std::string("action_failed"));
        QCOMPARE(refused.error().message, std::string("Method not found"));
        rig.viewer.answersCalls = false;
        QElapsedTimer waited;
        waited.start();
        const base::Result<json::Value> silent = callOf(connection, u"getUSBState"_s);
        QVERIFY(!silent);
        QCOMPARE(silent.error().code, std::string("timeout"));
        QCOMPARE(silent.error().message, std::string("The JetKVM did not answer in time"));
        QVERIFY2(waited.elapsed() >= 700, qPrintable(QString::number(waited.elapsed())));
    }

    // What it says of itself as it changes: its input (clamped), its USB, its power board, its version (in
    // words made plain).
    void itsEventsAreHeard()
    {
        Rig rig;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        QVERIFY(opens(connection));
        QTRY_COMPARE(connection.video().width, 1920);
        QCOMPARE(connection.video().height, 1080);
        QCOMPARE(connection.video().fps, 60.0);
        QCOMPARE(connection.video().ready, std::optional<bool>(true));
        QTRY_COMPARE(connection.usb(), std::optional<QString>(u"configured"_s));
        QTRY_COMPARE(connection.version(), u"0.5.9"_s);
        QSignalSpy videoSaid(&connection, &JetKvmConnection::videoSaid);
        QVERIFY(rig.viewer.sayOnRpc(
            rpc({{u"method"_s, u"videoInputState"_s},
                 {u"params"_s,
                  QJsonObject{{u"ready"_s, true}, {u"width"_s, 999'999}, {u"height"_s, -5}, {u"fps"_s, u"many"_s}}}})));
        QVERIFY(videoSaid.wait(5000));
        QCOMPARE(connection.video().width, 16384);
        QCOMPARE(connection.video().height, 0);
        QCOMPARE(connection.video().fps, 0.0);
        QVERIFY(rig.viewer.sayOnRpc(rpc({{u"method"_s, u"usbState"_s}, {u"params"_s, u"attached"_s}})));
        QVERIFY(rig.viewer.sayOnRpc(
            rpc({{u"method"_s, u"atxState"_s}, {u"params"_s, QJsonObject{{u"power"_s, true}, {u"hdd"_s, false}}}})));
        QVERIFY(rig.viewer.sayOnRpc(
            rpc({{u"method"_s, u"localVersion"_s}, {u"params"_s, QJsonObject{{u"appVersion"_s, u"0.6\n1"_s}}}})));
        QTRY_COMPARE(connection.version(), u"0.6 1"_s);
        QCOMPARE(connection.usb(), std::optional<QString>(u"attached"_s));
        QVERIFY(connection.atx() && connection.atx()->isObject());
        QVERIFY(connection.atx()->find(u"power") != nullptr && connection.atx()->find(u"power")->flag);
        // A ready that is no yes or no changes nothing of it.
        QVERIFY(rig.viewer.sayOnRpc(
            rpc({{u"method"_s, u"videoInputState"_s}, {u"params"_s, QJsonObject{{u"ready"_s, u"no"_s}}}})));
        QVERIFY(videoSaid.wait(5000));
        QCOMPARE(connection.video().ready, std::optional<bool>(true));
    }

    void theRpcChannelClosingEndsIt()
    {
        Rig rig;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        const Ending ending(connection);
        QVERIFY(opens(connection));
        QVERIFY(rig.viewer.current()->closeChannel("rpc"));
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 10000);
        QCOMPARE(ending.codes.first(), u"dropped"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM closed the channel it is asked over"_s);
        QVERIFY(!connection.isOpen());
    }

    // FRAME_STALE: no whole picture for so long while it does not say "no signal" ends it; while it says
    // there is none, nothing coming is how it is.
    void noPictureForSoLongEndsIt()
    {
        Rig rig;
        JetKvmConnection::Pace pace = watchedOften();
        pace.frameStaleMs = 1500;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        int pictures = 0;
        connect(&connection, &JetKvmConnection::pictureCame, this,
                [&](const QByteArray&, bool, qint64) { ++pictures; });
        QVERIFY(opens(connection));
        QTRY_VERIFY_WITH_TIMEOUT(pictures >= 10, 15000);
        rig.viewer.pictures(false);
        const qint64 stopped = ending.clock.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 10000);
        QCOMPARE(ending.codes.first(), u"dropped"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM stopped sending video"_s);
        QVERIFY2(ending.at.first() - stopped >= 1300, qPrintable(QString::number(ending.at.first() - stopped)));
    }
    void noPictureWhileItSaysNoSignalEndsNothing()
    {
        Rig rig;
        JetKvmConnection::Pace pace = watchedOften();
        pace.frameStaleMs = 1000;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        QVERIFY(opens(connection));
        QSignalSpy videoSaid(&connection, &JetKvmConnection::videoSaid);
        rig.viewer.saySignal(false);
        QVERIFY(videoSaid.wait(5000));
        rig.viewer.pictures(false);
        QTest::qWait(3000);
        QVERIFY(ending.codes.isEmpty());
    }

    // 10e: a new connection's first picture is waited for so long from its opening; where none comes by then, it is
    // ended (its viewer connects again); while it says there is no signal, nothing.
    void aConnectionWhoseFirstPictureNeverComesIsEnded()
    {
        Rig rig;
        rig.viewer.everyMs = 0;
        JetKvmConnection::Pace pace = watchedOften();
        pace.pictureAfterSignalMs = 1500;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        QVERIFY(opens(connection));
        const qint64 opened = ending.clock.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 10000);
        QCOMPARE(ending.codes.first(), u"dropped"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM sent no picture"_s);
        QVERIFY2(ending.at.first() - opened >= 1400, qPrintable(QString::number(ending.at.first() - opened)));
    }
    void noFirstPictureWhileItSaysNoSignalEndsNothing()
    {
        Rig rig;
        rig.viewer.everyMs = 0;
        rig.viewer.signal = false;
        JetKvmConnection::Pace pace = watchedOften();
        pace.pictureAfterSignalMs = 1000;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        QVERIFY(opens(connection));
        QTest::qWait(2500);
        QVERIFY(ending.codes.isEmpty());
    }

    // PICTURE_AFTER_SIGNAL: once it says its input is back, the first picture of it is waited for so long, and
    // a picture that came within the margin after the word is not that one (it may be from before).
    void afterItsInputCameBackItsPictureIsWaitedFor()
    {
        Rig rig;
        JetKvmConnection::Pace pace = watchedOften();
        pace.frameStaleMs = 10'000;
        pace.pictureAfterSignalMs = 1500;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        QSignalSpy videoSaid(&connection, &JetKvmConnection::videoSaid);
        QVERIFY(opens(connection));
        QTest::qWait(500);
        rig.viewer.saySignal(false);
        QVERIFY(videoSaid.wait(5000));
        rig.viewer.pictures(false);
        QTest::qWait(300);
        // Back, and a picture or two within the margin after the word, then none.
        rig.viewer.saySignal(true);
        rig.viewer.pictures(true);
        QTest::qWait(15);
        rig.viewer.pictures(false);
        const qint64 back = ending.clock.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 6000);
        QCOMPARE(ending.codes.first(), u"dropped"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM stopped sending video"_s);
        QVERIFY2(ending.at.first() - back >= 1200, qPrintable(QString::number(ending.at.first() - back)));
    }
    void aPictureAfterItsInputCameBackKeepsIt()
    {
        Rig rig;
        JetKvmConnection::Pace pace = watchedOften();
        pace.frameStaleMs = 10'000;
        pace.pictureAfterSignalMs = 1500;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        QSignalSpy videoSaid(&connection, &JetKvmConnection::videoSaid);
        QVERIFY(opens(connection));
        rig.viewer.saySignal(false);
        QVERIFY(videoSaid.wait(5000));
        rig.viewer.pictures(false);
        QTest::qWait(300);
        rig.viewer.saySignal(true);
        QTest::qWait(400);
        rig.viewer.pictures(true);
        QTest::qWait(3000);
        QVERIFY(ending.codes.isEmpty());
    }

    // A JetKVM that rested sends its first picture some seconds after its channel opens (5.5 on a real one
    // after a minute): nothing ends it meanwhile, its pictures come.
    void aRestedJetKvmStillConnects()
    {
        Rig rig;
        rig.viewer.firstPictureAfterMs = 3000;
        JetKvmConnection::Pace pace = watchedOften();
        pace.frameStaleMs = 1000;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        int pictures = 0;
        connect(&connection, &JetKvmConnection::pictureCame, this,
                [&](const QByteArray&, bool, qint64) { ++pictures; });
        QVERIFY(opens(connection));
        QTest::qWait(1500);
        QCOMPARE(pictures, 0);
        QTRY_VERIFY_WITH_TIMEOUT(pictures >= 10, 10000);
        QVERIFY(ending.codes.isEmpty());
    }

    // Another viewer took it: said once, and nothing more of ours goes to it; its end, when the JetKVM closes
    // it a second later, is "in use".
    void otherSessionConnectedSaysTaken()
    {
        Rig rig;
        JetKvmConnection connection(rig.device, 1, watchedOften());
        const Ending ending(connection);
        QSignalSpy taken(&connection, &JetKvmConnection::takenByAnother);
        QVERIFY(opens(connection));
        rig.viewer.handOver();
        QVERIFY(taken.wait(5000));
        QVERIFY(connection.taken());
        const int calls = rig.viewer.calls;
        const base::Result<json::Value> after = callOf(connection, u"getUSBState"_s);
        QVERIFY(!after);
        QCOMPARE(after.error().code, std::string("in_use"));
        QTest::qWait(300);
        QCOMPARE(rig.viewer.calls.load(), calls);
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 10000);
        QCOMPARE(ending.codes.first(), u"in_use"_s);
        QCOMPARE(taken.size(), 1);
    }

    // An answer that opens no way to it (no address): it ends in its time, said as taking too long.
    void notOpenInTimeEndsIt()
    {
        Rig rig;
        rig.jet.addresses = [] { return QList<QByteArray>{}; };
        JetKvmConnection::Pace pace = watchedOften();
        pace.openWithinMs = 2500;
        JetKvmConnection connection(rig.device, 1, pace);
        const Ending ending(connection);
        connection.start();
        QTRY_COMPARE_WITH_TIMEOUT(ending.codes.size(), 1, 10000);
        QCOMPARE(ending.codes.first(), u"timeout"_s);
        QCOMPARE(ending.messages.first(), u"The JetKVM is taking too long to connect"_s);
        QVERIFY2(ending.at.first() >= 2300, qPrintable(QString::number(ending.at.first())));
    }

    // Forty of its addresses at the most, counted beyond that.
    void aJetKvmIsTakenFortyAddressesAtMost()
    {
        Rig rig;
        for (int port = 0; port < 45; ++port) {
            rig.jet.moreAddresses.append("candidate:9 1 UDP 1 127.0.0.1 " + QByteArray::number(21000 + port) +
                                         " typ host");
        }
        JetKvmConnection connection(rig.device, 1, watchedOften());
        QVERIFY(opens(connection));
        QTRY_VERIFY_WITH_TIMEOUT(rig.jet.addressesSaid >= 46, 10000);
        QTRY_COMPARE(connection.refused().candidatesBeyond, quint64(rig.jet.addressesSaid - 40));
    }

    // Where a connection's time goes (plan 4, for M1): the video's diagnostics say when it was asked for, and the
    // offer, the answer, its channel open and its first picture each once, with the ms since it was asked for, in
    // that order. Numbers only: nothing of its address, its session or its offer.
    void itsStepsAreInTheVideoDiagnostics()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        {
            const qkvm::core::VideoDiagnostics log(path);
            QVERIFY(log.isOpen());
            Rig rig;
            rig.viewer.firstPictureAfterMs = 600;
            JetKvmConnection connection(rig.device, 1, watchedOften());
            int pictures = 0;
            connect(&connection, &JetKvmConnection::pictureCame, this,
                    [&pictures](const QByteArray&, bool, qint64) { ++pictures; });
            QVERIFY(opens(connection));
            QTRY_VERIFY_WITH_TIMEOUT(pictures >= 20, 15000);
        }
        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray all = written.readAll();
        QStringList events;
        QList<qint64> ms;
        for (const QByteArray& line : all.split('\n')) {
            const QJsonObject one = QJsonDocument::fromJson(line).object();
            if (one.value(u"event"_s).toString().startsWith(u"jetkvm_"_s)) {
                events.append(one.value(u"event"_s).toString());
                ms.append(one.value(u"values"_s).toObject().value(u"ms"_s).toInteger(-1));
            }
        }
        QCOMPARE(events, (QStringList{u"jetkvm_asked"_s, u"jetkvm_offer_sent"_s, u"jetkvm_answer"_s, u"jetkvm_open"_s,
                                      u"jetkvm_first_picture"_s}));
        QCOMPARE(ms.first(), qint64{0});
        for (qsizetype at = 1; at < ms.size(); ++at) {
            QVERIFY2(ms.at(at) >= ms.at(at - 1), qPrintable(QString::number(ms.at(at))));
        }
        // (Its first picture waited for as the rested one sent it: from the offer, 600 ms.)
        QVERIFY2(ms.last() >= 550, qPrintable(QString::number(ms.last())));
        QVERIFY(!all.contains("127.0.0.1") && !all.contains("authToken") && !all.contains("candidate"));
    }
};

QTEST_GUILESS_MAIN(TestJetKvmConnection)
#include "test_jetkvm_connection.moc"
