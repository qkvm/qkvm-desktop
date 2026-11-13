#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/jetkvm_connection.h>
#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/jetkvm_kept.h>
#include <qkvm/core/jetkvm_viewer.h>
#include <qkvm/core/live.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>

#include "jetkvm_by_hand.h"
#include "jetkvm_viewer_by_hand.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using qkvm::core::JetKvmConnection;
using qkvm::core::JetKvmDevice;
using qkvm::core::JetKvmKept;
using qkvm::core::Live;
using qkvm::core::SignInPacing;
using qkvm::core::testing::JetKvmByHand;
using qkvm::core::testing::JetKvmViewerByHand;
using Report = JetKvmConnection::Report;
using Road = JetKvmConnection::Road;

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
    pace.freshMs = 0;
    pace.pacing = std::make_shared<SignInPacing>();
    pace.viewer.connection.watchEveryMs = 100;
    pace.viewer.unwatchedEveryMs = 100;
    pace.viewer.retryPauseMs = 300;
    return pace;
}

JetKvmDevice::Where at(const JetKvmByHand& jet)
{
    JetKvmDevice::Where where;
    where.address = jet.address();
    return where;
}

// A JetKVM by hand with its viewer, and a driver of it.
struct Rig {
    explicit Rig(const JetKvmDevice::Pace& pace = quick()) : device(at(jet), u"secret"_s, pace) {}
    JetKvmByHand jet;
    JetKvmViewerByHand viewer{jet};
    JetKvmDevice device;
};

JetKvmConnection::Pace watchedOften()
{
    JetKvmConnection::Pace pace;
    pace.watchEveryMs = 100;
    return pace;
}

// A connection of its own to the rig's JetKVM, opened, and its keys ready (or why not).
struct Connected {
    explicit Connected(Rig& rig, std::shared_ptr<JetKvmKept> kept = {}, JetKvmConnection::Pace pace = watchedOften())
        : connection(std::make_unique<JetKvmConnection>(rig.device, 1, pace, std::move(kept)))
    {
        QObject::connect(connection.get(), &JetKvmConnection::ended, connection.get(),
                         [this](const QString& code, const QString& message) {
                             codes.append(code);
                             messages.append(message);
                         });
        clock.start();
        QSignalSpy opened(connection.get(), &JetKvmConnection::opened);
        connection->start();
        if (opened.wait(20'000)) {
            openedAt = clock.elapsed();
        }
    }
    bool ready(int withinMs = 15'000)
    {
        if (!connection->keysReady()) {
            QSignalSpy readied(connection.get(), &JetKvmConnection::keysReadied);
            if (!readied.wait(withinMs)) {
                return false;
            }
        }
        readyAt = clock.elapsed();
        return true;
    }
    std::unique_ptr<JetKvmConnection> connection;
    QElapsedTimer clock;
    qint64 openedAt = -1;
    qint64 readyAt = -1;
    QStringList codes;
    QStringList messages;
};

Report keyboard(int modifiers, QList<int> keys)
{
    Report report;
    report.keyboard = JetKvmConnection::Keyboard{modifiers, std::move(keys)};
    return report;
}
Report place(int x, int y, int buttons)
{
    return Report{Report::Kind::Place, {}, x, y, buttons};
}

// What a window is told of keyboard and mouse.
struct Window {
    explicit Window(Rig& rig) : live(rig.device.makeLive()), handle(live->open({}))
    {
        QObject::connect(handle.get(), &Live::controlChanged, handle.get(),
                         [this](bool on, const QString& code, const QString&) { said.append({on, code}); });
        QObject::connect(handle.get(), &Live::leftDown, handle.get(), [this] { ++leftDown; });
    }
    // Keyboard and mouse asked for and given (or not, within 20 s).
    bool take()
    {
        const qsizetype before = said.size();
        handle->askControl(true);
        return QTest::qWaitFor([&] { return said.size() > before; }, 20'000) && said.last().first;
    }
    std::unique_ptr<qkvm::core::OwnLive> live;
    std::unique_ptr<Live> handle;
    QList<std::pair<bool, QString>> said;
    int leftDown = 0;
};

// What an action that types comes to (20 s at most).
base::Result<qkvm::core::Done> typed(Rig& rig, const qkvm::core::Action& action)
{
    std::optional<base::Result<qkvm::core::Done>> answer;
    rig.device.act(action, [&answer](const base::Result<qkvm::core::Done>& what) { answer = what; });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 20'000)) {
        return base::Error{"never", "Never answered."};
    }
    return *answer;
}

QByteArray hex(const char* bytes)
{
    return QByteArray::fromHex(bytes);
}

// The messages that came on hidrpc over one connection, as bytes.
QList<QByteArray> bytesOf(const QList<JetKvmViewerByHand::HidMessage>& heard)
{
    QList<QByteArray> said;
    for (const auto& one : heard) {
        said.append(one.bytes);
    }
    return said;
}

// What came on rpc over the newest connection that is a call of `method`.
QList<QJsonObject> callsOf(JetKvmViewerByHand& viewer, const QString& method)
{
    QList<QJsonObject> calls;
    qkvm::core::testing::JetKvmPeerByHand* peer = viewer.current();
    if (peer == nullptr) {
        return calls;
    }
    for (const auto& [bytes, text] : peer->heard("rpc")) {
        const QJsonObject call = QJsonDocument::fromJson(bytes).object();
        if (call.value(u"method"_s).toString() == method) {
            calls.append(call);
        }
    }
    return calls;
}

} // namespace

// Keys and the mouse to this PC's own JetKVMs over its binary `hidrpc` channel, as its own page sends them (rule 28 of
// the web's FOR-THE-DESKTOP-APP), against a JetKVM by hand on 127.0.0.1 whose keyboard and mice are kept as the
// computer behind it has them.
class TestJetKvmKeys : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(qkvm::core::useWindowsOwnTls());
        QVERIFY2(qkvm::core::testing::JetKvmPeerByHand::filmIsThere(), qkvm::core::testing::JetKvmPeerByHand::noFilm);
    }

    // 28a: `01 01` every quarter of a second until it is answered; keys go over hidrpc.
    void theHandshakeIsAskedEveryQuarterSecondUntilAnswered()
    {
        Rig rig;
        rig.viewer.hidDeafForMs = 600;
        Connected jet(rig);
        QVERIFY(jet.openedAt >= 0);
        QVERIFY(jet.ready());
        QCOMPARE(jet.connection->road(), Road::Hid);
        const int shakes = rig.viewer.hidCount(0x01);
        QVERIFY2(shakes >= 3 && shakes <= 5, qPrintable(QString::number(shakes)));
        QVERIFY2(jet.readyAt - jet.openedAt < 1800, qPrintable(QString::number(jet.readyAt - jet.openedAt)));
        // (Answered: asked no more.)
        QTest::qWait(600);
        QCOMPARE(rig.viewer.hidCount(0x01), shakes);
        for (const QByteArray& said : bytesOf(rig.viewer.hidHeard())) {
            if (said.at(0) == 0x01) {
                QCOMPARE(said, hex("0101"));
            }
        }
    }

    // 28a: no answer within two seconds of the channel's opening (an older firmware): JSON-RPC, decided by then, and
    // asked every quarter of a second meanwhile.
    void noAnswerWithinTwoSecondsIsJsonRpc()
    {
        Rig rig;
        rig.viewer.hidrpc = false;
        Connected jet(rig);
        QVERIFY(jet.ready());
        QCOMPARE(jet.connection->road(), Road::Rpc);
        const qint64 took = jet.readyAt - jet.openedAt;
        QVERIFY2(took >= 1800 && took < 3000, qPrintable(QString::number(took)));
        const int shakes = rig.viewer.hidCount(0x01);
        QVERIFY2(shakes >= 7 && shakes <= 10, qPrintable(QString::number(shakes)));
    }

    // Another version: JSON-RPC at once, and asked no more.
    void anotherVersionIsJsonRpc()
    {
        Rig rig;
        rig.viewer.hidVersion = 2;
        Connected jet(rig);
        QVERIFY(jet.ready());
        QCOMPARE(jet.connection->road(), Road::Rpc);
        QVERIFY2(jet.readyAt - jet.openedAt < 1000, qPrintable(QString::number(jet.readyAt - jet.openedAt)));
        const int shakes = rig.viewer.hidCount(0x01);
        QTest::qWait(1500);
        QCOMPARE(rig.viewer.hidCount(0x01), shakes);
    }

    // The late road: no answer in time, then asked again once a second; an answer then moves keys onto hidrpc.
    void aLateAnswerMovesKeysOntoHidrpc()
    {
        Rig rig;
        rig.viewer.hidDeafForMs = 2400;
        Connected jet(rig);
        QVERIFY(jet.ready());
        QCOMPARE(jet.connection->road(), Road::Rpc);
        QTRY_COMPARE_WITH_TIMEOUT(jet.connection->road(), Road::Hid, 4000);
    }

    // No key goes before the road is decided.
    void nothingGoesBeforeTheRoadIsDecided()
    {
        Rig rig;
        rig.viewer.hidrpc = false;
        Connected jet(rig);
        QVERIFY(!jet.connection->keysReady());
        const std::optional<base::Error> refused = jet.connection->say(keyboard(0, {0x04}), 1);
        QCOMPARE(refused.value_or(base::Error{}).code, std::string("dropped"));
        QCOMPARE(rig.viewer.hidCount(0x05), 0);
        QCOMPARE(rig.viewer.hidCount(0x02), 0);
    }

    // 28g: the hidrpc channel closing, where keys go over it, ends the connection.
    void theHidrpcChannelClosingEndsIt()
    {
        Rig rig;
        Connected jet(rig);
        QVERIFY(jet.ready());
        QCOMPARE(jet.connection->road(), Road::Hid);
        QVERIFY(rig.viewer.current()->closeChannel("hidrpc"));
        QTRY_COMPARE_WITH_TIMEOUT(jet.codes.size(), 1, 10000);
        QCOMPARE(jet.codes.first(), u"dropped"_s);
        QCOMPARE(jet.messages.first(), u"The JetKVM closed the channel keys go over"_s);
    }

    // 28e: `33`, and `keysDownState` on rpc, is the JetKVM's keyboard as it is.
    void itsKeyboardAsItSaysItIsRead()
    {
        Rig rig;
        Connected jet(rig);
        QVERIFY(jet.ready());
        QVERIFY(!jet.connection->say(keyboard(0x02, {0x04}), 1));
        QTRY_COMPARE_WITH_TIMEOUT(jet.connection->keyboardSaid(),
                                  std::optional<JetKvmConnection::Keyboard>(JetKvmConnection::Keyboard{0x02, {0x04}}),
                                  5000);
        QVERIFY(rig.viewer.sayOnRpc(
            R"({"jsonrpc":"2.0","method":"keysDownState","params":{"modifier":1,"keys":[5,0,0,0,0,0]}})"));
        QTRY_COMPARE_WITH_TIMEOUT(jet.connection->keyboardSaid(),
                                  std::optional<JetKvmConnection::Keyboard>(JetKvmConnection::Keyboard{1, {5}}), 5000);
        QVERIFY(rig.viewer.sayOnHid(hex("33000607000000")));
        QTRY_COMPARE_WITH_TIMEOUT(jet.connection->keyboardSaid(),
                                  std::optional<JetKvmConnection::Keyboard>(JetKvmConnection::Keyboard{0, {6, 7}}),
                                  5000);
    }

    // 28b: keys as keypress messages (`05`), kept alive every 30 ms while one is held (the JetKVM lets go of it 100 ms
    // after the last), modifiers as their own keys, and `02 00` where nothing is down.
    void keysGoAsKeypressesAndAreKeptAlive()
    {
        Rig rig;
        Window window(rig);
        QVERIFY(window.take());
        window.handle->key(0x04, true);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.keyboard().down.contains(0x04), 5000);
        QTest::qWait(700);
        QCOMPARE(rig.viewer.keyboard().down, QList<int>{0x04});
        QVERIFY(rig.viewer.autoReleased().isEmpty());
        QVERIFY2(rig.viewer.keepAlives() >= 12, qPrintable(QString::number(rig.viewer.keepAlives())));
        window.handle->key(0xE1, true); // Shift
        window.handle->key(0x04, false);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.keyboard(), (JetKvmViewerByHand::Keyboard{0x02, {}}), 5000);
        const int kept = rig.viewer.keepAlives();
        QTest::qWait(300);
        QVERIFY2(rig.viewer.keepAlives() - kept <= 1, "kept alive with no key held");
        window.handle->key(0x05, true);
        window.handle->releaseAll();
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.keyboard(), JetKvmViewerByHand::Keyboard{}, 5000);
        // (The mouse's letting go comes behind it.)
        QTRY_COMPARE_WITH_TIMEOUT(bytesOf(rig.viewer.hidHeard(rig.viewer.currentNumber())).last(), hex("06000000"),
                                  5000);
        const QList<QByteArray> said = bytesOf(rig.viewer.hidHeard(rig.viewer.currentNumber()));
        QVERIFY(said.contains(hex("050401")));
        QVERIFY(said.contains(hex("05e101")));
        QVERIFY(said.contains(hex("050400")));
        QVERIFY(said.contains(hex("050501")));
        // (Behind it the mouse's letting go, as the web's line says it: a step of nothing with no button.)
        QCOMPARE(said.mid(said.size() - 2), (QList<QByteArray>{hex("0200"), hex("06000000")}));
    }

    // The keyboard's Power key is never sent.
    void thePowerKeyIsNeverSent()
    {
        Rig rig;
        Window window(rig);
        QVERIFY(window.take());
        window.handle->key(0x66, true);
        window.handle->key(0x04, true);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.keyboard().down.contains(0x04), 5000);
        QVERIFY(!rig.viewer.keyboard().down.contains(0x66));
        QVERIFY(!bytesOf(rig.viewer.hidHeard()).contains(hex("056601")));
    }

    // 28c: the absolute mouse `03 x4 y4 b`, the relative `06 dx dy b`; a button let go of on one is let go of on the
    // other; the wheel over JSON-RPC.
    void theMouseGoesAsItsOwnPageSendsIt()
    {
        Rig rig;
        Window window(rig);
        QVERIFY(window.take());
        window.handle->pointerAt(1000, 2000);
        window.handle->button(1, true);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.absButtons(), 1, 5000);
        QCOMPARE(rig.viewer.place(), QPoint(1000, 2000));
        // A drag begun at a place and ended by steps.
        window.handle->pointerBy(5, -3);
        window.handle->button(1, false);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.buttons(), 0, 5000);
        const QList<QByteArray> said = bytesOf(rig.viewer.hidHeard(rig.viewer.currentNumber()));
        QVERIFY(said.contains(hex("03000003e8000007d000")));
        QVERIFY(said.contains(hex("03000003e8000007d001")));
        QVERIFY(said.contains(hex("0605fd01")));
        QVERIFY(said.contains(hex("06000000")));
        // (The absolute one let go of at its place, behind the relative one's letting go.)
        QCOMPARE(said.last(), hex("03000003e8000007d000"));
        window.handle->wheel(0, 3);
        QTRY_COMPARE_WITH_TIMEOUT(callsOf(rig.viewer, u"wheelReport"_s).size(), 1, 5000);
        const QJsonObject turn = callsOf(rig.viewer, u"wheelReport"_s).first().value(u"params"_s).toObject();
        QCOMPARE(turn.value(u"wheelY"_s).toInt(), 3);
        QCOMPARE(turn.value(u"wheelX"_s).toInt(), 0);
        QCOMPARE(rig.viewer.hidCount(0x04), 0);
    }

    // 28d: no macro (`07`, `08`) goes, whatever is sent.
    void macrosAreNeverSent()
    {
        Rig rig;
        Window window(rig);
        QVERIFY(window.take());
        for (const int usage : {0x04, 0x05, 0x06, 0xE0, 0x28}) {
            window.handle->key(usage, true);
            window.handle->key(usage, false);
        }
        window.handle->pointerAt(5, 5);
        window.handle->button(2, true);
        window.handle->button(2, false);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.hidCount(0x03) >= 3, 5000);
        QCOMPARE(rig.viewer.hidCount(0x07), 0);
        QCOMPARE(rig.viewer.hidCount(0x08), 0);
    }

    // 28f: at a hand-over, `02 00` and each mouse's buttons up, behind everything; no keep-alive after it, and nothing
    // else ever.
    void atAHandOverTheKeyboardAndTheButtonsAreLetGoOf()
    {
        Rig rig;
        Window window(rig);
        QVERIFY(window.take());
        const int number = rig.viewer.currentNumber();
        window.handle->key(0x04, true);
        window.handle->pointerAt(300, 400);
        window.handle->button(1, true);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.absButtons(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.keepAlives() > 2, 5000);
        rig.viewer.handOver();
        // (A JetKVM keeps a mouse button across a hand-over: only the program's letting go lets go of it.)
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.buttons(), 0, 3000);
        QList<QByteArray> after;
        for (const auto& one : rig.viewer.hidHeard(number)) {
            if (one.handed && one.bytes != hex("09")) {
                after.append(one.bytes);
            }
        }
        QCOMPARE(after, (QList<QByteArray>{hex("0200"), hex("030000012c0000019000")}));
        const int kept = rig.viewer.keepAlives();
        const qsizetype heard = rig.viewer.hidHeard(number).size();
        window.handle->key(0x05, true);
        window.handle->button(2, true);
        QTest::qWait(500);
        QVERIFY2(rig.viewer.keepAlives() - kept <= 1, "kept alive after the hand-over");
        QCOMPARE(rig.viewer.hidHeard(number).size(), heard);
        QVERIFY(rig.viewer.keyboard().down.isEmpty());
    }

    // 28f: before this program ends a connection itself, what it has down is let go of and given its time to leave.
    void beforeAnEndItLetsGoAndWaits()
    {
        Rig rig;
        const auto kept = std::make_shared<JetKvmKept>();
        Connected jet(rig, kept);
        QVERIFY(jet.ready());
        const int number = rig.viewer.currentNumber();
        QVERIFY(!jet.connection->say(place(100, 200, 0), 1));
        QVERIFY(!jet.connection->say(place(100, 200, 3), 1));
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.absButtons(), 3, 5000);
        const QPointer<JetKvmConnection> going(jet.connection.get());
        JetKvmConnection::letGo(std::move(jet.connection));
        QTRY_VERIFY_WITH_TIMEOUT(going == nullptr, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.goneAt(number) > 0, 5000);
        QCOMPARE(rig.viewer.absButtons(), 0);
        QCOMPARE(bytesOf(rig.viewer.hidHeard()).last(), hex("0300000064000000c800"));
        // Given its time to leave and be done before the connection went (letGoSettleMs at the least).
        const qint64 gapUs = rig.viewer.goneAt(number) - rig.viewer.lastHidAt(number);
        QVERIFY2(gapUs >= 40'000, qPrintable(QString::number(gapUs)));
        // (And owed to the next connection all the same: it may not have arrived.)
        QVERIFY(kept->leftover.down);
    }

    // 28f: the same where the driver goes while a window holds a button (the KVM changed, or taken off the wall): its
    // viewer gives the connection back, letting go first.
    void aDriverThatGoesLetsGoFirst()
    {
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        auto device = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, quick());
        const std::unique_ptr<qkvm::core::OwnLive> live = device->makeLive();
        const std::unique_ptr<Live> window = live->open({});
        QSignalSpy control(window.get(), &Live::controlChanged);
        window->askControl(true);
        QTRY_COMPARE_WITH_TIMEOUT(control.size(), 1, 20000);
        QVERIFY(control.first().at(0).toBool());
        window->pointerAt(900, 900);
        window->button(3, true);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.absButtons(), 4, 5000);
        device.reset();
        QTRY_COMPARE_WITH_TIMEOUT(viewer.absButtons(), 0, 3000);
    }

    // 28f: a connection after one that ended with something down begins with "nothing down": the keyboard, the
    // relative mouse, and the absolute one at its last place.
    void theNextConnectionBeginsWithNothingDown()
    {
        Rig rig;
        const auto kept = std::make_shared<JetKvmKept>();
        {
            Connected jet(rig, kept);
            QVERIFY(jet.ready());
            QVERIFY(!jet.connection->say(place(700, 800, 1), 1));
            QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.absButtons(), 1, 5000);
            // The JetKVM's connection goes without a word: nothing could be let go of over it.
            rig.viewer.dropCurrent();
            QTRY_COMPARE_WITH_TIMEOUT(jet.codes.size(), 1, 15000);
        }
        QCOMPARE(rig.viewer.absButtons(), 1);
        QVERIFY(kept->leftover.down);
        Connected next(rig, kept);
        QVERIFY(next.ready());
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.buttons(), 0, 5000);
        QList<QByteArray> first;
        for (const QByteArray& said : bytesOf(rig.viewer.hidHeard(rig.viewer.currentNumber()))) {
            if (said.at(0) != 0x01) {
                first.append(said);
            }
        }
        QCOMPARE(first, (QList<QByteArray>{hex("0200"), hex("06000000"), hex("03000002bc0000032000")}));
        QVERIFY(!kept->leftover.down);
    }

    // One window holds keyboard and mouse at a time.
    void aSecondWindowIsToldInUse()
    {
        Rig rig;
        Window first(rig);
        QVERIFY(first.take());
        const std::unique_ptr<Live> second = first.live->open({});
        QSignalSpy control(second.get(), &Live::controlChanged);
        second->askControl(true);
        QTRY_COMPARE(control.size(), 1);
        QCOMPARE(control.first().at(0).toBool(), false);
        QCOMPARE(control.first().at(1).toString(), u"in_use"_s);
        QVERIFY(first.handle->hasControl());
    }

    // Keyboard and mouse are held on one connection: where it ends, whoever held them is told, and holds nothing.
    void keyboardAndMouseGoWithTheirConnection()
    {
        Rig rig;
        Window window(rig);
        QVERIFY(window.take());
        window.handle->key(0x04, true);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.keyboard().down.contains(0x04), 5000);
        rig.viewer.dropCurrent();
        QTRY_COMPARE_WITH_TIMEOUT(window.said.size(), 2, 15000);
        QCOMPARE(window.said.last(), (std::pair<bool, QString>{false, u"dropped"_s}));
        QVERIFY(!window.handle->hasControl());
        QCOMPARE(window.leftDown, 1);
        // (The JetKVM let go of its keyboard as the connection ended.)
        QVERIFY(rig.viewer.keyboard().down.isEmpty());
    }

    // --- JSON-RPC, where hidrpc does not answer (rule 24)
    // ---------------------------------------------------------------

    // 24a: one report at a time, each once the one before was answered; moves that wait are folded.
    void overJsonRpcOneReportAtATimeAndMovesFolded()
    {
        Rig rig;
        rig.viewer.hidrpc = false;
        rig.viewer.reportsAnswerAfterMs = 60;
        Window window(rig);
        QVERIFY(window.take());
        for (int step = 1; step <= 40; ++step) {
            window.handle->pointerAt(step * 100, step * 50);
        }
        window.handle->key(0x04, true);
        window.handle->key(0x04, false);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.keyboard(), JetKvmViewerByHand::Keyboard{}, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.reports().size() >= 3, 10000);
        QTest::qWait(300);
        QCOMPARE(rig.viewer.mostReportsInFlight(), 1);
        int places = 0;
        QJsonObject last;
        for (const auto& [method, params] : rig.viewer.reports()) {
            if (method == u"absMouseReport"_s) {
                ++places;
                last = params;
            }
        }
        QVERIFY2(places <= 4, qPrintable(QString::number(places)));
        QCOMPARE(last.value(u"x"_s).toInt(), 4000);
        QCOMPARE(last.value(u"y"_s).toInt(), 2000);
        // (The keys over JSON-RPC as well, as whole keyboards.)
        QCOMPARE(rig.viewer.hidCount(0x05), 0);
    }

    // 24a: a report not answered within its time ends the connection on purpose, and a new one is made; whoever held
    // the keyboard is told, and the JetKVM is not taken for away.
    void aReportNotAnsweredInTimeMakesTheConnectionAnew()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.takeWithinMs = 800;
        Rig rig(pace);
        rig.viewer.hidrpc = false;
        Window window(rig);
        QVERIFY(window.take());
        const qsizetype offers = rig.jet.offersHeard.size();
        rig.viewer.answersReports = false;
        window.handle->key(0x04, true);
        QTRY_COMPARE_WITH_TIMEOUT(window.said.size(), 2, 10000);
        QCOMPARE(window.said.last(), (std::pair<bool, QString>{false, u"dropped"_s}));
        QTRY_VERIFY_WITH_TIMEOUT(rig.jet.offersHeard.size() > offers, 5000);
        rig.viewer.answersReports = true;
        QVERIFY(window.take());
    }

    // 24a: more waiting than it takes ends the connection on purpose too.
    void tooManyWaitingMakesTheConnectionAnew()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.mostWaiting = 8;
        Rig rig(pace);
        rig.viewer.hidrpc = false;
        rig.viewer.reportsAnswerAfterMs = 2000;
        Window window(rig);
        QVERIFY(window.take());
        const qsizetype offers = rig.jet.offersHeard.size();
        for (int usage = 0x04; usage < 0x04 + 12; ++usage) {
            window.handle->key(usage, true);
            window.handle->key(usage, false);
        }
        QTRY_COMPARE_WITH_TIMEOUT(window.said.size(), 2, 10000);
        QCOMPARE(window.said.last().second, u"dropped"_s);
        QTRY_VERIFY_WITH_TIMEOUT(rig.jet.offersHeard.size() > offers, 10000);
    }

    // 24c: a connection made anew lets go of what it has down only once the presses in the JetKVM's hands are answered:
    // sent beside one, the letting go could be done before it, and the button stay down.
    void aConnectionMadeAnewWaitsForThePressesInItsHands()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.mostWaiting = 4;
        Rig rig(pace);
        rig.viewer.hidrpc = false;
        Window window(rig);
        QVERIFY(window.take());
        window.handle->pointerAt(70, 80);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.place() == QPoint(70, 80), 5000);
        rig.viewer.pressesAnswerAfterMs = 1500;
        window.handle->button(1, true);
        // The press in the JetKVM's hands before the keys come. (The JetKVM is seen to have put the pointer there a
        // moment before that report's answer is back here: a press said in that moment waits its turn behind it, goes
        // with what waits when the keys make the connection anew, never sent, and there is no press to wait for.)
        const auto pressInItsHands = [&rig] {
            const QList<std::pair<QString, QJsonObject>> reports = rig.viewer.reports();
            return std::ranges::any_of(reports, [](const std::pair<QString, QJsonObject>& one) {
                return one.first == u"absMouseReport"_s && one.second.value(u"buttons"_s).toInt() == 1;
            });
        };
        QTRY_VERIFY_WITH_TIMEOUT(pressInItsHands(), 5000);
        for (int usage = 0x04; usage < 0x04 + 4; ++usage) {
            window.handle->key(usage, true);
            window.handle->key(usage, false);
        }
        QTRY_COMPARE_WITH_TIMEOUT(window.said.size(), 2, 10000);
        QCOMPARE(window.said.last().second, u"dropped"_s);
        // (The press done, and let go of after it over this connection: not only by the next one, which says "nothing
        // down" first some two seconds later, once its road is decided.)
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.absButtonsSeen().contains(1), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.absButtons(), 0, 400);
        QTest::qWait(300);
        QCOMPARE(rig.viewer.absButtons(), 0);
    }

    // 24c: a press answered after the JetKVM said its own page took it was done after the hand-over: let go of at once.
    void aPressAnsweredAfterTheHandOverIsLetGoOf()
    {
        Rig rig;
        rig.viewer.hidrpc = false;
        rig.viewer.reportsAnswerAfterMs = 300;
        rig.viewer.handOverCloseMs = 1500;
        Window window(rig);
        QVERIFY(window.take());
        window.handle->pointerAt(500, 600);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.place() == QPoint(500, 600), 5000);
        rig.viewer.handOverAtPress = 1;
        window.handle->button(1, true);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.absButtons(), 1, 5000);
        // (A JetKVM keeps a button across a hand-over: only the program's letting go lets go of it.)
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.absButtons(), 0, 3000);
        QCOMPARE(rig.viewer.place(), QPoint(500, 600));
    }

    // 24c: after a hand-over the program waits up to two seconds for what is due before it closes the connection, so
    // that a letting go of a late press comes after it. Where it is closed sooner, the press is down for good.
    void theAnswersDueAreWaitedForBeforeTheClose()
    {
        Rig rig;
        rig.viewer.hidrpc = false;
        rig.viewer.reportsAnswerAfterMs = 1200;
        rig.viewer.handOverCloseMs = 1900;
        Window window(rig);
        QVERIFY(window.take());
        rig.viewer.handOverAtPress = 1;
        window.handle->key(0xE0, true); // Ctrl
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.keyboard().modifier, 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.keyboard().modifier, 0, 3000);
    }

    // 25: a press whose own wait was given up and that is answered after all is let go of again, as that answer comes.
    void aPressGivenUpAndAnsweredAfterAllIsLetGoOfAgain()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.takeWithinMs = 600;
        Rig rig(pace);
        rig.viewer.hidrpc = false;
        Window window(rig);
        QVERIFY(window.take());
        window.handle->pointerAt(50, 60);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.place() == QPoint(50, 60), 5000);
        // (The press done a second after it came, and its letting go at once: done before it, as a JetKVM may.)
        rig.viewer.pressesAnswerAfterMs = 1000;
        window.handle->button(2, true);
        QTRY_VERIFY_WITH_TIMEOUT(rig.viewer.absButtonsSeen().contains(2), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.absButtons(), 0, 3000);
        QTest::qWait(300);
        QCOMPARE(rig.viewer.absButtons(), 0);
    }

    // 24e: an action that types waits up to five seconds for the live line's reports on their way, then says busy.
    void typingWaitsForTheLiveLineThenSaysBusy()
    {
        JetKvmDevice::Pace pace = quick();
        pace.typing.liveKeysWaitMs = 700;
        Rig rig(pace);
        rig.viewer.hidrpc = false;
        rig.viewer.reportsAnswerAfterMs = 1500;
        Window window(rig);
        QVERIFY(window.take());
        window.handle->wheel(0, 1);
        window.handle->wheel(1, 0);
        QTest::qWait(100);
        const base::Result<qkvm::core::Done> busy = typed(rig, qkvm::core::Action::press(u"KeyA"_s));
        QVERIFY(!busy);
        QCOMPARE(busy.error().code, std::string("busy"));
        QTest::qWait(3500);
        const base::Result<qkvm::core::Done> done = typed(rig, qkvm::core::Action::press(u"KeyA"_s));
        QVERIFY2(done, done ? "" : done.error().message.c_str());
    }

    // An action that types, over JSON-RPC: each key a whole keyboard, then nothing down, each once the one before was
    // answered.
    void typingOverJsonRpc()
    {
        Rig rig;
        rig.viewer.hidrpc = false;
        const base::Result<qkvm::core::Done> done = typed(rig, qkvm::core::Action::type(u"Ab"_s, false));
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QList<JetKvmViewerByHand::Keyboard> said;
        for (const auto& [method, params] : rig.viewer.reports()) {
            if (method == u"keyboardReport"_s) {
                JetKvmViewerByHand::Keyboard one{params.value(u"modifier"_s).toInt(), {}};
                for (const auto& key : params.value(u"keys"_s).toArray()) {
                    one.down.append(key.toInt());
                }
                said.append(one);
            }
        }
        QCOMPARE(said, (QList<JetKvmViewerByHand::Keyboard>{{2, {0x04}}, {0, {}}, {0, {0x05}}, {0, {}}}));
        QCOMPARE(rig.viewer.mostReportsInFlight(), 1);
    }

    // An action that types, over hidrpc: each key a whole keyboard (`02`), its `33` waited for, then `02 00`.
    void typingOverHidrpc()
    {
        Rig rig;
        const base::Result<qkvm::core::Done> done =
            typed(rig, qkvm::core::Action::press(u"ControlLeft,AltLeft,Delete"_s));
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QList<QByteArray> whole;
        for (const QByteArray& said : bytesOf(rig.viewer.hidHeard())) {
            if (said.at(0) == 0x02) {
                whole.append(said);
            }
        }
        QCOMPARE(whole, (QList<QByteArray>{hex("02054c"), hex("0200")}));
        QCOMPARE(rig.viewer.keyboard(), JetKvmViewerByHand::Keyboard{});
        QCOMPARE(rig.viewer.hidCount(0x07), 0);
    }

    // --- text in the JetKVM's layout (W14d)
    // ---------------------------------------------------------------------------

    // Review 3's LOW 3 (2026-11-06): a long text typed with nobody watching (no window, no card asks) is typed to its
    // end. Every key keeps the JetKVM's one connection wanted, as the web's _wanted_at before every key; it was given
    // back for nobody watching in the middle of the text ("stopped after N of M keys").
    void aLongTextWithNobodyWatchingIsTypedToItsEnd()
    {
        // (Longer than the connection takes to come about, and its layout to be asked: no key goes before those.)
        JetKvmDevice::Pace pace = quick();
        pace.viewer.unwatchedMs = 3000;
        pace.typing.keyPauseMs = 60;
        Rig rig(pace);
        const QString text = u"abcdefghijklmnopqrstuvwxabcdefghijklmnopqrstuvwx"_s; // (48 keys of 120 ms and more)
        QElapsedTimer took;
        took.start();
        const base::Result<qkvm::core::Done> done = typed(rig, qkvm::core::Action::type(text, false));
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QVERIFY2(took.elapsed() > pace.viewer.unwatchedMs + 1000, QByteArray::number(took.elapsed()));
        QList<QByteArray> pressed;
        for (const QByteArray& said : bytesOf(rig.viewer.hidHeard())) {
            if (said.at(0) == 0x02 && said.size() > 2) {
                pressed.append(said);
            }
        }
        QCOMPARE(pressed.size(), text.size());
        QCOMPARE(rig.viewer.keyboard(), JetKvmViewerByHand::Keyboard{});
    }

    // Review 3's LOW 4 (2026-11-06): at the program's end, a JetKVM's letting go of what was down (a button held in a
    // drag in a live window) is given its time, whether or not a server's goodbye is waited for: its driver let go of,
    // JetKvmConnection::finishLettingGo runs the loop until its connection has let go and gone, and the button is up
    // by then. (Before, nothing ran the loop then: the letting go went out with the close, and a JetKVM never lets go
    // of a mouse button by itself.)
    void atTheProgramsEndTheLettingGoIsGivenItsTime()
    {
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        std::optional<JetKvmDevice> device;
        device.emplace(at(jet), u"secret"_s, quick());
        std::unique_ptr<qkvm::core::OwnLive> live = device->makeLive();
        std::unique_ptr<Live> handle = live->open({});
        QList<bool> said;
        QObject::connect(handle.get(), &Live::controlChanged, handle.get(),
                         [&said](bool on, const QString& /*code*/, const QString& /*message*/) { said.append(on); });
        handle->askControl(true);
        QVERIFY(QTest::qWaitFor([&said] { return !said.isEmpty(); }, 20'000) && said.last());
        handle->pointerAt(16000, 16000);
        handle->button(1, true); // (a drag, held)
        QTRY_VERIFY_WITH_TIMEOUT(viewer.absButtonsSeen().contains(1), 5000);
        QCOMPARE(JetKvmConnection::lettingGo(), 0);
        // The program ends: its drivers let go of (as LocalWall::letGoOfEveryDriver), the window still there.
        device.reset();
        QCOMPARE(JetKvmConnection::lettingGo(), 1);
        QElapsedTimer took;
        took.start();
        JetKvmConnection::finishLettingGo(5000);
        QCOMPARE(JetKvmConnection::lettingGo(), 0);
        QVERIFY2(took.elapsed() < 4000, QByteArray::number(took.elapsed()));
        QCOMPARE(viewer.absButtons(), 0);
        // (Nothing to wait for: at once.)
        took.restart();
        JetKvmConnection::finishLettingGo(5000);
        QVERIFY(took.elapsed() < 100);
        handle.reset();
        live.reset();
    }

    // A text goes in the keys of the layout the JetKVM says its computer uses, asked once a connection.
    void aTextIsTypedInTheLayoutItsComputerUses()
    {
        Rig rig;
        rig.viewer.keyboardLayout = "de-DE";
        const base::Result<qkvm::core::Done> done = typed(rig, qkvm::core::Action::type(u"zy"_s, false));
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        const auto pressed = [&rig] {
            QList<QByteArray> whole;
            for (const QByteArray& said : bytesOf(rig.viewer.hidHeard())) {
                if (said.at(0) == 0x02 && said.size() > 2) {
                    whole.append(said);
                }
            }
            return whole;
        };
        // (German: z where a US keyboard has Y, and y where it has Z.)
        QCOMPARE(pressed(), (QList<QByteArray>{hex("02001c"), hex("02001d")}));
        QVERIFY(typed(rig, qkvm::core::Action::type(u"z"_s, false)));
        QCOMPARE(rig.viewer.layoutsAsked.load(), 1);
    }

    // A character the layout cannot type: refused before anything is typed.
    void aTextTheLayoutCannotTypeTypesNothing()
    {
        Rig rig;
        rig.viewer.keyboardLayout = "de-DE";
        const base::Result<qkvm::core::Done> done =
            typed(rig, qkvm::core::Action::type(u"ab"_s + QChar(0x0436), false));
        QVERIFY(!done);
        QCOMPARE(done.error().code, std::string("invalid"));
        QVERIFY2(done.error().message.find("character 3") != std::string::npos, done.error().message.c_str());
        QCOMPARE(rig.viewer.hidCount(0x02), 0);
        QCOMPARE(rig.viewer.hidCount(0x05), 0);
    }

    // One that has no such question (an older JetKVM), or never chose one, types in US English, as its own page does.
    void withoutALayoutItIsUsEnglish_data()
    {
        QTest::addColumn<bool>("asked");
        QTest::addColumn<QString>("said");
        QTest::newRow("no question") << false << QString();
        QTest::newRow("none chosen") << true << QString();
        QTest::newRow("an older one's name") << true << u"en_US"_s;
    }
    void withoutALayoutItIsUsEnglish()
    {
        QFETCH(bool, asked);
        QFETCH(QString, said);
        Rig rig;
        rig.viewer.saysItsLayout = asked;
        rig.viewer.keyboardLayout = said.toStdString();
        const base::Result<qkvm::core::Done> done = typed(rig, qkvm::core::Action::type(u"z"_s, false));
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QVERIFY(bytesOf(rig.viewer.hidHeard()).contains(hex("02001d")));
    }

    // 24c: what the end of a connection cut off went out, and may have been done.
    void whatTheEndCutOffMayHaveBeenDone()
    {
        Rig rig;
        rig.viewer.hidrpc = false;
        rig.viewer.answersReports = false;
        std::optional<base::Result<qkvm::core::Done>> answer;
        rig.device.act(qkvm::core::Action::press(u"KeyQ"_s),
                       [&answer](const base::Result<qkvm::core::Done>& what) { answer = what; });
        QTRY_VERIFY_WITH_TIMEOUT(!rig.viewer.reports().isEmpty(), 15000);
        rig.viewer.dropCurrent();
        QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 15000);
        const base::Result<qkvm::core::Done> came = answer.value_or(qkvm::core::Done{});
        QVERIFY(!came);
        QCOMPARE(came.error().code, std::string("timeout"));
        QVERIFY2(came.error().message.find("may have been done") != std::string::npos, came.error().message.c_str());
    }

    // 28a: keys move onto a hidrpc that answered late only at a moment nothing of the program's is down or on its way
    // over JSON-RPC.
    void theLateRoadWaitsForAQuietMoment()
    {
        Rig rig;
        rig.viewer.hidDeafForMs = 2400;
        Connected jet(rig);
        QVERIFY(jet.ready());
        QCOMPARE(jet.connection->road(), Road::Rpc);
        QVERIFY(!jet.connection->say(keyboard(0, {0x04}), 1));
        QTest::qWait(2500);
        QCOMPARE(jet.connection->road(), Road::Rpc);
        QVERIFY(!jet.connection->say(keyboard(0, {}), 1));
        QTRY_COMPARE_WITH_TIMEOUT(jet.connection->road(), Road::Hid, 3000);
        QCOMPARE(rig.viewer.keyboard(), JetKvmViewerByHand::Keyboard{});
    }

    // 24e: typing stops with in_use where the JetKVM's own page takes it, and nothing more of it goes.
    void typingStopsWhereItsOwnPageTakesIt()
    {
        Rig rig;
        rig.viewer.hidrpc = false;
        rig.viewer.handOverCloseMs = 1500;
        rig.viewer.handOverAtPress = 3; // (the third key's press)
        const base::Result<qkvm::core::Done> done = typed(rig, qkvm::core::Action::type(u"abcdef"_s, false));
        QVERIFY(!done);
        QCOMPARE(done.error().code, std::string("in_use"));
        QVERIFY2(done.error().message.find("of 6") != std::string::npos, done.error().message.c_str());
        int pressed = 0;
        for (const auto& [method, params] : rig.viewer.reports()) {
            pressed += method == u"keyboardReport"_s && !params.value(u"keys"_s).toArray().isEmpty() ? 1 : 0;
        }
        QCOMPARE(pressed, 3);
        QTRY_COMPARE_WITH_TIMEOUT(rig.viewer.keyboard(), JetKvmViewerByHand::Keyboard{}, 3000);
    }
};

QTEST_GUILESS_MAIN(TestJetKvmKeys)
#include "test_jetkvm_keys.moc"
