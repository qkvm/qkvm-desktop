#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/base/clock.h>
#include <qkvm/core/incidents.h>
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
using qkvm::core::JetKvmDevice;
using qkvm::core::JetKvmKept;
using qkvm::core::Live;
using qkvm::core::SignInPacing;
using qkvm::core::Snapshot;
using qkvm::core::Status;
using qkvm::core::testing::JetKvmByHand;
using qkvm::core::testing::JetKvmPeerByHand;
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

template <class T>
base::Result<T> answerTo(const std::function<void(std::function<void(base::Result<T>)>)>& ask)
{
    std::optional<base::Result<T>> answer;
    ask([&answer](base::Result<T> what) { answer = std::move(what); });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 20'000)) {
        return base::Error{"never", "Never answered."};
    }
    return *answer;
}

base::Result<Snapshot> cardOf(JetKvmDevice& device)
{
    return answerTo<Snapshot>([&](auto done) { device.askSnapshot(320, std::move(done)); });
}

// A card that asks until it is answered a picture (or 20 s).
base::Result<Snapshot> pictureOf(JetKvmDevice& device)
{
    base::Result<Snapshot> card = cardOf(device);
    QElapsedTimer waited;
    waited.start();
    while (!card && card.error().code == "no_stream" && waited.elapsed() < 20'000) {
        QTest::qWait(100);
        card = cardOf(device);
    }
    return card;
}

Status lookedAt(JetKvmDevice& device)
{
    QSignalSpy over(&device, &JetKvmDevice::looked);
    device.look();
    if (!over.wait(20'000)) {
        Status never;
        never.state = u"the look never ended"_s;
        return never;
    }
    return device.status();
}

// A JetKVM by hand with its viewer, and a driver of it.
struct Rig {
    explicit Rig(const JetKvmDevice::Pace& pace = quick(), std::shared_ptr<JetKvmKept> kept = {})
        : device(at(jet), u"secret"_s, pace, std::move(kept))
    {}
    JetKvmByHand jet;
    JetKvmViewerByHand viewer{jet};
    JetKvmDevice device;
};

// A JetKVM that is rebooting, as this program meets it: its port takes every connection and drops it at once.
// When each attempt came, counted from the first: connections within 100 ms of the one before are one attempt's (since
// J5 an attempt asks which JetKVM it is before its line, a question Qt's network asks once more where the line it
// held is closed under it). And every connection, counted.
struct Refusing {
    Refusing()
    {
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (QTcpSocket* one = server.nextPendingConnection()) {
                ++connections;
                if (cameAt.isEmpty()) {
                    clock.start();
                    cameAt.append(0);
                } else if (clock.elapsed() - last > 100) {
                    cameAt.append(clock.elapsed());
                }
                last = clock.elapsed();
                one->abort();
                one->deleteLater();
            }
        });
    }
    QUrl address() const { return QUrl(u"http://127.0.0.1:%1"_s.arg(server.serverPort())); }
    // The steps between one connection and the next.
    QList<qint64> steps() const
    {
        QList<qint64> between;
        for (qsizetype at = 1; at < cameAt.size(); ++at) {
            between.append(cameAt.at(at) - cameAt.at(at - 1));
        }
        return between;
    }
    // Both, as a line for a failure.
    QString said() const
    {
        QStringList each;
        for (const qint64 ms : steps()) {
            each.append(QString::number(ms));
        }
        return u"%1 connections, the steps between them %2 ms"_s.arg(cameAt.size()).arg(each.join(u", "));
    }
    QTcpServer server;
    QElapsedTimer clock;
    QList<qint64> cameAt;
    qint64 last = 0;
    int connections = 0;
};

// A pace whose own pause after a refusal is short, so that only the driver's growing hold (200 ms, doubled while it
// stays away, 1600 at the most) keeps it from asking at that pause.
JetKvmDevice::Pace heldWhileAway()
{
    JetKvmDevice::Pace pace = quick();
    pace.holdMs = 200;
    pace.longestHoldMs = 1600;
    pace.viewer.retryPauseMs = 100;
    return pace;
}

// One found away is held 3 s (and its answer waited for 300 ms): long enough to be seen standing.
JetKvmDevice::Pace heldLong()
{
    JetKvmDevice::Pace pace = heldWhileAway();
    pace.holdMs = 3000;
    pace.longestHoldMs = 3000;
    pace.viewer.connection.answerWithinMs = 300;
    return pace;
}

// Whether a card is told now that the JetKVM was found away (its line dropped, or its answer never came), as the
// driver's hold stands; or that its offer came to nothing (`not_through`: its answer never came).
bool foundAway(JetKvmDevice& device)
{
    const base::Result<Snapshot> card = cardOf(device);
    return !card && (card.error().code == "dropped" || card.error().code == "unreachable" ||
                     card.error().code == "timeout" || card.error().code == "not_through");
}

// Whether a card is told now that an offer came to nothing, and no card makes another.
bool toldNotThrough(JetKvmDevice& device)
{
    const base::Result<Snapshot> card = cardOf(device);
    return !card && card.error().code == "not_through";
}

// A pace a tenth of the driver's and the wall's own (its hold 100 ms, doubled while it stays away, 1500 at the most;
// an offer's answer waited for 1500; a card every 100 ms and a look every 400): what two minutes of the wall's come to,
// in twelve seconds.
JetKvmDevice::Pace aTenthOfTheWalls()
{
    JetKvmDevice::Pace pace = quick();
    pace.holdMs = 100;
    pace.longestHoldMs = 1500;
    pace.viewer.connection.answerWithinMs = 1500;
    pace.viewer.connection.openWithinMs = 1500;
    pace.viewer.retryPauseMs = 300;
    return pace;
}

QString digestOf(const QString& number)
{
    return u"jetkvm:"_s +
           QString::fromLatin1(QCryptographicHash::hash(number.toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// What a window is told.
struct Window {
    explicit Window(std::unique_ptr<Live> held) : live(std::move(held))
    {
        QObject::connect(live.get(), &Live::pictureCame, live.get(),
                         [this](const Live::Picture& picture) { pictures.append(picture); });
        QObject::connect(live.get(), &Live::closed, live.get(),
                         [this](const QString& code, const QString&) { closed.append(code); });
    }
    std::unique_ptr<Live> live;
    QList<Live::Picture> pictures;
    QStringList closed;
};

} // namespace

// A JetKVM's one viewer (M6 2g): its cards, its windows, one connection across its drivers, giving the viewer back,
// how it is, and stepping aside; against a JetKVM by hand and its viewer on 127.0.0.1.
class TestJetKvmLive : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(qkvm::core::useWindowsOwnTls());
        QVERIFY2(qkvm::core::testing::JetKvmPeerByHand::filmIsThere(), qkvm::core::testing::JetKvmPeerByHand::noFilm);
    }

    // (The review of the fitted view, 2026-11-06.) A window is told the JetKVM's size with its first key frame, as that
    // frame's SPS says what is shown: 1920 by 1080 of a film coded 1920 by 1088 (its cropping taken off). Where the
    // screen behind changes its size, it is told again, once; nothing is said again while the size stays.
    void aWindowIsToldWhatTheJetKvmShowsAndWhenThatChanges()
    {
        if (!JetKvmPeerByHand::filmIsThere() || !JetKvmPeerByHand::filmIsThere("desktop-720p15.h264")) {
            QSKIP(JetKvmPeerByHand::noFilm);
        }
        Rig rig;
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        const std::unique_ptr<Live> handle = live->open({});
        QList<Live::Video> videos;
        int pictures = 0;
        connect(handle.get(), &Live::videoChanged, this, [&videos](const Live::Video& video) { videos.append(video); });
        connect(handle.get(), &Live::pictureCame, this, [&pictures](const Live::Picture&) { ++pictures; });
        QTRY_VERIFY_WITH_TIMEOUT(!videos.isEmpty(), 20'000);
        QCOMPARE(videos.first(), (Live::Video{true, 1920, 1080}));
        QTRY_VERIFY_WITH_TIMEOUT(pictures > 60, 20'000); // (more than a key frame: nothing said again)
        QCOMPARE(videos.size(), 1);
        QVERIFY(rig.viewer.filmAnew("desktop-720p15.h264"));
        QTRY_VERIFY_WITH_TIMEOUT(videos.size() >= 2, 20'000);
        QCOMPARE(videos.at(1), (Live::Video{true, 1280, 720}));
        const int after = pictures;
        QTRY_VERIFY_WITH_TIMEOUT(pictures > after + 30, 20'000);
        QCOMPARE(videos.size(), 2);
    }

    void aLookNeverConnects()
    {
        Rig rig;
        for (int each = 0; each < 3; ++each) {
            QCOMPARE(lookedAt(rig.device).state, u"online"_s);
        }
        QTest::qWait(300);
        QCOMPARE(rig.jet.offersHeard.size(), 0);
        QCOMPARE(rig.jet.lineAskings, 0);
    }

    // --- which JetKVM answers (J5, the web's _same_jetkvm before every video connection) --------------------------

    void anotherJetKvmAtItsAddressIsOfferedNothing()
    {
        // After a new lease another JetKVM answers at the address. A card's connection, "Show here" and a window ask
        // which it is under the session, and go no further: no signalling line, no offer, no viewer taken. The card, a
        // look and the window say so, and a card asks again only at its pace (the web's SAME_JETKVM_EVERY).
        JetKvmDevice::Pace pace = quick();
        pace.sameJetKvmEveryMs = 2500;
        JetKvmByHand jet;
        const JetKvmViewerByHand viewer{jet};
        JetKvmDevice::Where where = at(jet);
        where.machine = digestOf(u"the-one-it-was-added-for"_s);
        JetKvmDevice device(where, u"secret"_s, pace);
        QVERIFY(QTest::qWaitFor(
            [&] {
                const base::Result<Snapshot> card = cardOf(device);
                return !card && card.error().code == "another_machine";
            },
            10000));
        QCOMPARE(cardOf(device).error().message, JetKvmDevice::anotherJetKvm().message);
        const Status looked = lookedAt(device);
        QCOMPARE(looked.state, u"error"_s);
        QCOMPARE(looked.errorCode, u"another_machine"_s);
        // (Before its sign-in over plain HTTP its number was asked with no session too: the merged review, 4.)
        QCOMPARE(jet.asked("/device"), 2);
        QElapsedTimer watch;
        watch.start();
        while (watch.elapsed() < 1200) {
            QCOMPARE(cardOf(device).error().code, std::string("another_machine"));
            QTest::qWait(100);
        }
        QCOMPARE(jet.asked("/device"), 2);
        // "Show here" asks at once, and is refused so.
        const base::Result<qkvm::core::Done> shown = answerTo<qkvm::core::Done>(
            [&](auto then) { device.act(qkvm::core::Action::plain(u"resume"_s), std::move(then)); });
        QVERIFY(!shown);
        QCOMPARE(shown.error().code, std::string("another_machine"));
        QCOMPARE(jet.asked("/device"), 3);
        // A window that is opened asks too, at once (not at the cards' pace), and is told so for good.
        const std::unique_ptr<qkvm::core::OwnLive> live = device.makeLive();
        Window window(live->open({}));
        QTRY_COMPARE_WITH_TIMEOUT(window.closed, QStringList{u"another_machine"_s}, 1500);
        QVERIFY(window.live->endsForGood(u"another_machine"_s));
        QCOMPARE(jet.asked("/device"), 4);
        // Past the pace a card's connection asks again, and every card says so meanwhile too.
        watch.restart();
        while (watch.elapsed() < 3500) {
            QCOMPARE(cardOf(device).error().code, std::string("another_machine"));
            QTest::qWait(100);
        }
        QVERIFY(jet.asked("/device") >= 5);
        QCOMPARE(jet.lineAskings, 0);
        QCOMPARE(jet.offersHeard.size(), 0);
        QVERIFY(!device.viewer().aside());
        QCOMPARE(device.machine(), where.machine);
    }

    void aShowHereThatFindsAnotherJetKvmSaysSoWhileTheProgramStandsAside()
    {
        // The program stood aside for another viewer, and "Show here" finds another JetKVM at the address: it stands
        // aside again (L3), and its card and its look say what was found, not that somebody has this JetKVM's console.
        JetKvmByHand jet;
        auto kept = std::make_shared<JetKvmKept>();
        kept->aside = true;
        JetKvmDevice::Where where = at(jet);
        where.machine = digestOf(u"the-one-it-was-added-for"_s);
        JetKvmDevice device(where, u"secret"_s, quick(), kept);
        const base::Result<qkvm::core::Done> shown = answerTo<qkvm::core::Done>(
            [&](auto then) { device.act(qkvm::core::Action::plain(u"resume"_s), std::move(then)); });
        QCOMPARE(shown ? std::string("ok") : shown.error().code, std::string("another_machine"));
        QVERIFY(kept->aside);
        QCOMPARE(cardOf(device).error().code, std::string("another_machine"));
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
        QCOMPARE(jet.lineAskings, 0);
    }

    void aConnectionTakesTheFirstNumberItSeesAndGoesOn()
    {
        // Where none is known, the first one seen is taken, and the connection goes on; asked before its line, under
        // the session the line is asked with. Where it could not be read, a connection (which takes nothing) goes on
        // too.
        Rig rig;
        QSignalSpy known(&rig.device, &JetKvmDevice::knownAs);
        QVERIFY(pictureOf(rig.device));
        QCOMPARE(rig.device.machine(), digestOf(rig.jet.deviceId.toString()));
        QCOMPARE(known.size(), 1);
        // (And before its sign-in over plain HTTP with no session, which one with a password refuses: the merged
        // review, 4.)
        QCOMPARE(rig.jet.asked("/device"), 2);
        qsizetype number = -1;
        qsizetype line = -1;
        for (qsizetype at = 0; at < rig.jet.stand->asked.size(); ++at) {
            const auto& one = rig.jet.stand->asked.at(at);
            if (one.path == "/device" && !one.headers.value("cookie").isEmpty()) {
                number = at;
                QCOMPARE(one.headers.value("cookie"), rig.jet.lineCookies.constFirst());
            }
            if (one.path.startsWith("/webrtc/signaling/client") && line < 0) {
                line = at;
            }
        }
        QVERIFY(number >= 0);
        QVERIFY(line < 0 || number < line);
        JetKvmByHand dumb;
        dumb.deviceFails = true;
        const JetKvmViewerByHand itsViewer{dumb};
        JetKvmDevice device(at(dumb), u"secret"_s, quick());
        QVERIFY(pictureOf(device));
        QVERIFY(device.machine().isEmpty());
    }

    // A card that asks is somebody watching: it connects, is told "Connecting" meanwhile, and is answered the
    // newest key frame, an access unit that begins with its SPS (a still the engine decodes: 2c).
    void aCardThatAsksConnectsAndIsAnsweredTheNewestKeyFrame()
    {
        Rig rig;
        const base::Result<Snapshot> first = cardOf(rig.device);
        QVERIFY(!first);
        QCOMPARE(first.error().code, std::string("no_stream"));
        QCOMPARE(first.error().message, std::string("Connecting to the JetKVM"));
        const base::Result<Snapshot> card = pictureOf(rig.device);
        QVERIFY2(card, card ? "" : card.error().message.c_str());
        QVERIFY(card.value().signal);
        const QByteArray& still = card.value().picture;
        QVERIFY(still.startsWith(QByteArray::fromHex("00000001")));
        QCOMPARE(static_cast<int>(still.at(4)) & 0x1F, 7);
        QCOMPARE(rig.jet.offersHeard.size(), 1);
        QCOMPARE(rig.viewer.sessions, 1);
    }

    // Once open, and before its first picture (a JetKVM that rested): "Waiting for the JetKVM's video".
    void aCardIsAnsweredWaitingWhileNoPictureCame()
    {
        Rig rig;
        rig.viewer.firstPictureAfterMs = 4000;
        QVERIFY(!cardOf(rig.device));
        QTRY_VERIFY_WITH_TIMEOUT(rig.device.viewer().isOpen(), 15000);
        const base::Result<Snapshot> card = cardOf(rig.device);
        QVERIFY(!card);
        QCOMPARE(card.error().message, std::string("Waiting for the JetKVM's video"));
    }

    // A key frame from before the JetKVM said its input came back is not the screen; nor is one older than
    // FRAME_STALE. One that came after the word is.
    void aStaleOrEarlierKeyFrameIsNotTheScreen()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.frameStaleMs = 20'000; // (the connection is not ended for it meanwhile)
        Rig rig(pace);
        QVERIFY(pictureOf(rig.device));
        rig.viewer.saySignal(false);
        QTRY_VERIFY(cardOf(rig.device) && !cardOf(rig.device).value().signal);
        rig.viewer.pictures(false);
        QTest::qWait(300);
        rig.viewer.saySignal(true);
        QTRY_VERIFY(!cardOf(rig.device));
        QCOMPARE(cardOf(rig.device).error().message, std::string("Waiting for the JetKVM's video"));
        QTest::qWait(300);
        rig.viewer.pictures(true);
        QVERIFY(pictureOf(rig.device));
    }
    void aKeyFrameOlderThanFrameStaleIsNotTheScreen()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.frameStaleMs = 1000;
        pace.viewer.connection.watchEveryMs = 60'000; // (the connection is not ended for it meanwhile)
        Rig rig(pace);
        QVERIFY(pictureOf(rig.device));
        rig.viewer.pictures(false);
        QTest::qWait(1500);
        const base::Result<Snapshot> card = cardOf(rig.device);
        QVERIFY(!card);
        QCOMPARE(card.error().message, std::string("Waiting for the JetKVM's video"));
    }

    // With no signal, the last key frame, said as no signal.
    void noSignalAnswersTheLastWithSignalFalse()
    {
        Rig rig;
        const base::Result<Snapshot> before = pictureOf(rig.device);
        QVERIFY(before);
        rig.viewer.pictures(false);
        rig.viewer.saySignal(false);
        QTRY_VERIFY(cardOf(rig.device) && !cardOf(rig.device).value().signal);
        QVERIFY(!cardOf(rig.device).value().picture.isEmpty());
    }

    // Without a window, the connection's pictures that are no key frames are let go in its box, uncounted as
    // losses; nothing is handed on.
    void withoutAWindowOnlyKeyFramesAreDrained()
    {
        Rig rig;
        QVERIFY(pictureOf(rig.device));
        QTest::qWait(2000);
        const qkvm::core::PictureBox::Counts counts = rig.device.viewer().counts();
        QVERIFY2(counts.letGoAsCardOnly > 30, qPrintable(QString::number(counts.letGoAsCardOnly)));
        QVERIFY2(counts.handed < counts.letGoAsCardOnly, qPrintable(QString::number(counts.handed)));
    }

    // A window has every picture, from the next key frame that arrives after it came, numbered without a gap.
    void aWindowGetsEveryPictureFromTheNextKeyFrame()
    {
        Rig rig;
        QVERIFY(pictureOf(rig.device));
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        Window window(live->open({}));
        QTRY_VERIFY_WITH_TIMEOUT(window.pictures.size() >= 90, 15000);
        QVERIFY(window.pictures.first().keyFrame && window.pictures.first().h264);
        for (qsizetype at = 0; at < window.pictures.size(); ++at) {
            QCOMPARE(window.pictures[at].counter, quint32(at + 1));
            QVERIFY(window.pictures[at].arrivedUs > 0);
        }
        QVERIFY(std::ranges::count_if(window.pictures, [](const Live::Picture& one) { return !one.keyFrame; }) > 60);
        QCOMPARE(rig.viewer.sessions, 1);
    }

    // Two windows share the one connection; one that comes while the other is shown has every picture from the
    // next key frame (it came between two: the film has one every thirty pictures).
    void twoWindowsShareOneConnection()
    {
        Rig rig;
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        Window one(live->open({}));
        QTRY_VERIFY_WITH_TIMEOUT(
            one.pictures.size() >= 40 && one.pictures.size() % 30 >= 5 && one.pictures.size() % 30 <= 20, 20000);
        Window two(live->open({}));
        QTRY_VERIFY_WITH_TIMEOUT(one.pictures.size() >= 30 && two.pictures.size() >= 30, 20000);
        QVERIFY(two.pictures.first().keyFrame);
        QCOMPARE(two.pictures.first().counter, quint32{1});
        QCOMPARE(rig.viewer.sessions, 1);
        QCOMPARE(rig.jet.offersHeard.size(), 1);
        one.live->close();
        const qsizetype had = two.pictures.size();
        QTRY_VERIFY(two.pictures.size() > had + 30);
    }

    // A key frame asked for goes once a second at the most, and nothing held is thrown away for it.
    void aKeyFrameAskedLosesNothingAndGoesOnceASecond()
    {
        Rig rig;
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        Window window(live->open({}));
        QTRY_VERIFY_WITH_TIMEOUT(window.pictures.size() >= 10, 15000);
        const int plis = rig.viewer.current()->plis;
        QElapsedTimer asking;
        asking.start();
        while (asking.elapsed() < 2000) {
            window.live->askForAKeyFrame();
            QTest::qWait(20);
        }
        QVERIFY2(rig.viewer.current()->plis - plis <= 3,
                 qPrintable(QString::number(rig.viewer.current()->plis - plis)));
        QCOMPARE(rig.device.viewer().counts().runsLost, quint64{0});
        for (qsizetype at = 0; at < window.pictures.size(); ++at) {
            QCOMPARE(window.pictures[at].counter, quint32(at + 1));
        }
    }

    // One connection at a time for the KVM, across its drivers: a driver made anew (its password changed while
    // live) does not begin its own while the one before lives, and does once it is gone.
    void oneConnectionAtATimeAcrossDrivers()
    {
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        auto kept = std::make_shared<JetKvmKept>();
        auto first = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, quick(), kept);
        QVERIFY(pictureOf(*first));
        auto next = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, quick(), kept);
        QVERIFY(!cardOf(*next));
        QTest::qWait(600);
        QCOMPARE(jet.offersHeard.size(), 1);
        // The driver before is let go of (as the wall does, a turn later): then the next one connects.
        first.release()->deleteLater();
        QVERIFY(pictureOf(*next));
        QCOMPARE(jet.offersHeard.size(), 2);
        QVERIFY(!kept->aside);
    }

    // One that is not gone within its wait fails the asking, as taking too long (`not_connected`, which was not
    // sent), and nothing is begun beside it.
    void aConnectionThatDoesNotEndInTimeFailsTheAsking()
    {
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        auto kept = std::make_shared<JetKvmKept>();
        const auto first = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, quick(), kept);
        QVERIFY(pictureOf(*first));
        JetKvmDevice::Pace pace = quick();
        pace.viewer.endingWaitMs = 800;
        JetKvmDevice next(at(jet), u"secret"_s, pace, kept);
        QVERIFY(!cardOf(next));
        QElapsedTimer waited;
        waited.start();
        base::Result<Snapshot> card = cardOf(next);
        while (!card && card.error().code == "no_stream" && waited.elapsed() < 5000) {
            QTest::qWait(50);
            card = cardOf(next);
        }
        QVERIFY(!card);
        QCOMPARE(card.error().code, std::string("not_connected"));
        QCOMPARE(card.error().message, std::string("The JetKVM is taking too long to connect"));
        QCOMPARE(jet.offersHeard.size(), 1);
        QVERIFY(!JetKvmDevice::mayHaveBeenDone(card.error()));
    }

    // UNWATCHED: no window and no card asking for so long gives the JetKVM's one viewer back; a window keeps
    // it; a look does not.
    void unwatchedGivesTheViewerBack()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.unwatchedMs = 1500;
        Rig rig(pace);
        QVERIFY(pictureOf(rig.device));
        QVERIFY(rig.device.viewer().isOpen());
        // Looked at all the while, well beyond UNWATCHED: given back all the same, within it and a tick.
        QElapsedTimer looking;
        looking.start();
        while (looking.elapsed() < 3000) {
            QCOMPARE(lookedAt(rig.device).state, u"online"_s);
            QTest::qWait(150);
        }
        QVERIFY(!rig.device.viewer().isOpen());
        QCOMPARE(rig.jet.offersHeard.size(), 1);
    }
    void aWindowKeepsIt()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.unwatchedMs = 1000;
        Rig rig(pace);
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        Window window(live->open({}));
        QTRY_VERIFY_WITH_TIMEOUT(window.pictures.size() >= 10, 15000);
        QTest::qWait(2500);
        QVERIFY(rig.device.viewer().isOpen());
        window.live->close();
        QTRY_VERIFY_WITH_TIMEOUT(!rig.device.viewer().isOpen(), 5000);
    }

    // How it is, while a connection is open: asked over rpc (its picture's size, its keyboard, its version, and
    // the power light where its ATX extension is on); with none, a look of its status, which never connects.
    void statusIsAskedOverRpcWhileOpen()
    {
        Rig rig;
        rig.viewer.extension = "atx-power";
        QVERIFY(pictureOf(rig.device));
        const int calls = rig.viewer.calls;
        const Status status = lookedAt(rig.device);
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(status.width, 1920);
        QCOMPARE(status.height, 1080);
        QCOMPARE(status.keyboard, std::optional<bool>(true));
        QCOMPARE(status.version, u"0.5.9"_s);
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(false));
        QCOMPARE(rig.viewer.calls - calls, 5);
        rig.viewer.saySignal(false);
        QTRY_COMPARE(lookedAt(rig.device).state, u"no_signal"_s);
    }

    // 10f: an answer to getVideoState asked before a videoInputState event that came since is older than the event:
    // what the event said stands.
    void aVideoStateAskedBeforeAnEventIsOlderThanIt()
    {
        Rig rig;
        QVERIFY(pictureOf(rig.device));
        QCOMPARE(lookedAt(rig.device).state, u"online"_s);
        rig.viewer.videoStateAfterMs = 800;
        QSignalSpy over(&rig.device, &JetKvmDevice::looked);
        rig.device.look();
        QTest::qWait(250);
        rig.viewer.saySignal(false);
        QVERIFY(over.wait(10000));
        QCOMPARE(rig.device.status().state, u"no_signal"_s);
    }

    // Keyboard and mouse are given, and given back, each by one word (test_jetkvm_keys has what goes to the JetKVM).
    void askControlIsAnsweredOnceItsKeysAreReady()
    {
        Rig rig;
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        const std::unique_ptr<Live> window = live->open({});
        QSignalSpy control(window.get(), &Live::controlChanged);
        window->askControl(true);
        QTRY_COMPARE_WITH_TIMEOUT(control.size(), 1, 20000);
        QCOMPARE(control.first().at(0).toBool(), true);
        QVERIFY(window->hasControl());
        window->askControl(false);
        QTRY_COMPARE(control.size(), 2);
        QCOMPARE(control.last().at(0).toBool(), false);
        QCOMPARE(control.last().at(1).toString(), QString());
        QVERIFY(!window->hasControl());
    }

    // Another viewer took it: each window is told "in_use" once, and from then on a window, a card, a look and
    // a retry make no offer; a look says "busy". Only "Show here" (act "resume") takes it back.
    void otherSessionConnectedStepsAsideForGood()
    {
        Rig rig;
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        Window window(live->open({}));
        QTRY_VERIFY_WITH_TIMEOUT(window.pictures.size() >= 10, 15000);
        rig.viewer.handOver();
        QTRY_COMPARE(window.closed, QStringList{u"in_use"_s});
        QVERIFY(window.live->endsForGood(u"in_use"_s));
        const qsizetype offers = rig.jet.offersHeard.size();
        const base::Result<Snapshot> card = cardOf(rig.device);
        QVERIFY(!card);
        QCOMPARE(card.error().code, std::string("in_use"));
        const Status looked = lookedAt(rig.device);
        QCOMPARE(looked.state, u"busy"_s);
        QCOMPARE(looked.errorCode, u"in_use"_s);
        Window again(live->open({}));
        QTRY_COMPARE(again.closed, QStringList{u"in_use"_s});
        QTest::qWait(1500); // (beyond the retry pause, and the old connection's hand-over)
        QCOMPARE(rig.jet.offersHeard.size(), offers);
        QCOMPARE(window.closed.size(), 1);
        QVERIFY(rig.device.viewer().aside());
    }

    // Who another viewer's word is about, as design 3.4 has it.
    void theTableOfJudge()
    {
        JetKvmKept kept;
        kept.newest = 5;
        for (const bool offerWent : {false, true}) {
            for (const bool over : {false, true}) {
                kept.newestOfferWent = offerWent;
                kept.newestOver = over;
                // Over the newest: it steps aside, whatever else.
                QCOMPARE(kept.judge(5), JetKvmKept::Word::StepAside);
                // Over an older one: the program's own new one only where the newest's offer went and it is not over.
                QCOMPARE(kept.judge(4), offerWent && !over ? JetKvmKept::Word::OwnNewOne : JetKvmKept::Word::StepAside);
            }
        }
    }

    void resumeTakesItBack()
    {
        Rig rig;
        QVERIFY(pictureOf(rig.device));
        rig.viewer.handOver();
        QTRY_VERIFY(rig.device.viewer().aside());
        QTest::qWait(1200);
        const qsizetype offers = rig.jet.offersHeard.size();
        const base::Result<qkvm::core::Done> done = answerTo<qkvm::core::Done>(
            [&](auto then) { rig.device.act(qkvm::core::Action::plain(u"resume"_s), std::move(then)); });
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QCOMPARE(rig.jet.offersHeard.size(), offers + 1);
        QVERIFY(!rig.device.viewer().aside());
        QVERIFY(pictureOf(rig.device));
        QCOMPARE(lookedAt(rig.device).state, u"online"_s);
    }

    void aShowHereIsAnsweredWhereItsDriverGoesFirst()
    {
        // From the reading of 2g/2h (M2): "Show here" waits for its connection (here: a signalling line taken and never
        // answered); where its driver goes first (the KVM changed, or held back for its twin), whoever pressed it is
        // told so, once.
        JetKvmByHand jet;
        jet.linesSayNothing = true;
        auto device = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, quick());
        QList<qkvm::base::Result<qkvm::core::Done>> told;
        device->act(qkvm::core::Action::plain(u"resume"_s),
                    [&told](qkvm::base::Result<qkvm::core::Done> what) { told.append(std::move(what)); });
        QTRY_VERIFY_WITH_TIMEOUT(jet.lineAskings >= 1, 10000);
        QVERIFY(told.isEmpty());
        device.reset();
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 5000);
        QCOMPARE(told.constFirst().error().code, std::string("stopped"));
        QTest::qWait(300);
        QCOMPARE(told.size(), 1);
    }

    void aShowHereThatIsNotShownLeavesTheProgramAsideAsBefore()
    {
        // From the third reading of the twins (L3): the program stood aside for another viewer, and "Show here" did not
        // come to that (here: its driver went first). It stands aside again, as before the press: nothing of a driver
        // of it connects by itself to take the other viewer's picture later. Pressed again, it is shown here.
        JetKvmByHand jet;
        jet.linesSayNothing = true;
        auto kept = std::make_shared<JetKvmKept>();
        kept->aside = true;
        auto device = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, quick(), kept);
        QList<qkvm::base::Result<qkvm::core::Done>> told;
        device->act(qkvm::core::Action::plain(u"resume"_s),
                    [&told](qkvm::base::Result<qkvm::core::Done> what) { told.append(std::move(what)); });
        QTRY_VERIFY_WITH_TIMEOUT(jet.lineAskings >= 1, 10000);
        QVERIFY(!kept->aside);
        device.reset();
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, 5000);
        QVERIFY(!told.constFirst());
        QVERIFY(kept->aside);
        JetKvmDevice next(at(jet), u"secret"_s, quick(), kept);
        const int asked = jet.lineAskings;
        const base::Result<Snapshot> card = cardOf(next);
        QVERIFY(!card);
        QCOMPARE(card.error().code, std::string("in_use"));
        QTest::qWait(600);
        QCOMPARE(jet.lineAskings, asked);
    }

    // That the program stepped aside outlives the driver: the next one (the KVM changed) stays aside.
    void asideOutlivesTheDriver()
    {
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        auto kept = std::make_shared<JetKvmKept>();
        auto first = std::make_unique<JetKvmDevice>(at(jet), u"secret"_s, quick(), kept);
        QVERIFY(pictureOf(*first));
        viewer.handOver();
        QTRY_VERIFY(kept->aside);
        first.reset();
        JetKvmDevice next(at(jet), u"secret"_s, quick(), kept);
        const qsizetype offers = jet.offersHeard.size();
        const base::Result<Snapshot> card = cardOf(next);
        QVERIFY(!card);
        QCOMPARE(card.error().code, std::string("in_use"));
        QCOMPARE(lookedAt(next).state, u"busy"_s);
        QTest::qWait(600);
        QCOMPARE(jet.offersHeard.size(), offers);
    }

    // The reading of 0479e1d..e40a02f, M3: a JetKVM that is rebooting, its port dropping every connection at once,
    // is asked less and less often while cards ask, as the web's (its _check_backoff on the road that connects): at
    // the driver's growing hold (200 ms, doubled while it stays away, 1600 at the most), whatever the viewer's own
    // pause after a refusal (RETRY_PAUSE, which the web leaves to that hold where it is away). A card meanwhile is
    // told that it is away.
    void aJetKvmThatDropsItsLineIsAskedLessAndLessOften_data()
    {
        QTest::addColumn<int>("pause");
        QTest::newRow("a pause shorter than the hold") << 100;
        QTest::newRow("a pause longer than the hold") << 3000;
    }
    void aJetKvmThatDropsItsLineIsAskedLessAndLessOften()
    {
        QFETCH(int, pause);
        Refusing refusing;
        JetKvmDevice::Pace pace = heldWhileAway();
        pace.viewer.retryPauseMs = pause;
        JetKvmDevice::Where where;
        where.address = refusing.address();
        auto kept = std::make_shared<JetKvmKept>();
        JetKvmDevice device(where, QString(), pace, kept);
        QStringList codes;
        QElapsedTimer asking;
        asking.start();
        while (asking.elapsed() < 6000) {
            const base::Result<Snapshot> card = cardOf(device);
            if (!card) {
                codes.append(QString::fromStdString(card.error().code));
            }
            QTest::qWait(50);
        }
        // At 0, 0.2, 0.6, 1.4, 3.0 and 4.6 s or so (each with its own line's time): about six.
        const QString said = refusing.said();
        QVERIFY2(refusing.cameAt.size() >= 4 && refusing.cameAt.size() <= 8, qPrintable(said));
        // The steps grow, up to the longest hold.
        QVERIFY2(refusing.steps().last() >= 1500, qPrintable(said));
        QVERIFY2(refusing.steps().first() < 1000, qPrintable(said));
        // (And no connection is begun meanwhile: a card that asks while it is held makes none.)
        QVERIFY2(kept->serials <= static_cast<quint64>(refusing.cameAt.size()) + 1,
                 qPrintable(QString::number(kept->serials)));
        QVERIFY2(codes.contains(u"dropped"_s) || codes.contains(u"unreachable"_s), qPrintable(codes.join(u' ')));
    }
    // The same while a window shows it: its own retries wait for that hold, and come as it ends.
    void aWindowOnAJetKvmThatDropsItsLineAsksLessAndLessOften_data()
    {
        aJetKvmThatDropsItsLineIsAskedLessAndLessOften_data();
    }
    void aWindowOnAJetKvmThatDropsItsLineAsksLessAndLessOften()
    {
        QFETCH(int, pause);
        Refusing refusing;
        JetKvmDevice::Pace pace = heldWhileAway();
        pace.viewer.retryPauseMs = pause;
        JetKvmDevice::Where where;
        where.address = refusing.address();
        JetKvmDevice device(where, QString(), pace);
        const std::unique_ptr<qkvm::core::OwnLive> live = device.makeLive();
        const Window window(live->open({}));
        QTest::qWait(6000);
        QVERIFY2(refusing.cameAt.size() >= 4 && refusing.cameAt.size() <= 8, qPrintable(refusing.said()));
        QVERIFY2(refusing.steps().last() >= 1500, qPrintable(refusing.said()));
        QVERIFY(window.closed.isEmpty());
    }

    // A JetKVM whose line lets it in and whose video connection never comes about (it hears the offer and never
    // answers), while a window shows it: each such end is an attempt that found it away, and the window's next comes
    // as the hold that grows from one to the next ends (the person is watching it, and asked for it). The line letting
    // it in says nothing yet of whether it can be used (the web's _signalling): that is said by the first thing it
    // answers over a connection, by what it is, or by a sign-in.
    void aWindowOnAConnectionThatNeverComesAboutAsksLessAndLessOften()
    {
        JetKvmDevice::Pace pace = heldWhileAway();
        pace.viewer.connection.answerWithinMs = 300;
        Rig rig(pace);
        rig.jet.answersNothing = true;
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        const Window window(live->open({}));
        QTest::qWait(6000);
        // At 0, 0.5, 1.2, 2.3 and 4.2 s or so: five, where the pause alone asked every 0.4 s.
        QVERIFY2(rig.jet.offersHeard.size() >= 3 && rig.jet.offersHeard.size() <= 7,
                 qPrintable(QString::number(rig.jet.offersHeard.size())));
        QVERIFY(window.closed.isEmpty());
    }

    // From the second reading of c7ec58c, B: each offer takes the JetKVM's one viewer from whoever has it (its own
    // page, where an operator works a production server), and where this PC's video never comes through (UDP
    // blocked, ICE failing) the program never hears that other viewer, so it never steps aside. After an offer that
    // came to nothing, cards make no other: over two minutes of the wall's (here a tenth of its times), ONE offer. A
    // card is told so, and its look says it on the card with "Show here"; the look goes on as before, and says online.
    void cardsMakeOneOfferWhereOneCameToNothing()
    {
        JetKvmDevice::Pace pace = heldWhileAway();
        pace.viewer.connection.answerWithinMs = 300;
        Rig rig(pace);
        rig.jet.answersNothing = true;
        QElapsedTimer asking;
        asking.start();
        int looks = 0;
        qint64 lookedAtMs = -1000;
        while (asking.elapsed() < 12'000) {
            (void)cardOf(rig.device);
            if (asking.elapsed() - lookedAtMs >= 400) {
                lookedAtMs = asking.elapsed();
                ++looks;
                QCOMPARE(lookedAt(rig.device).state, u"online"_s);
            }
            QTest::qWait(100);
        }
        QCOMPARE(rig.jet.offersHeard.size(), 1);
        QVERIFY(toldNotThrough(rig.device));
        const Status looked = lookedAt(rig.device);
        QCOMPARE(looked.state, u"online"_s);
        QCOMPARE(looked.errorCode, u"not_through"_s);
        QVERIFY(looks >= 20);
        // (Looks went out the plain way all the while, and it answered them.)
        QVERIFY2(rig.jet.asked("/device/status") >= 20, qPrintable(QString::number(rig.jet.asked("/device/status"))));
    }

    // Only a window that is opened offers again, at the hold's pace while it stays open; once it is closed, the cards
    // make no offer again. That outlives the driver (the KVM changed): the next one's cards make none either.
    void aWindowOffersAgainAtTheHoldsPaceAndCardsDoNot()
    {
        JetKvmDevice::Pace pace = heldWhileAway();
        pace.viewer.connection.answerWithinMs = 300;
        auto kept = std::make_shared<JetKvmKept>();
        auto rig = std::make_unique<Rig>(pace, kept);
        rig->jet.answersNothing = true;
        QTRY_VERIFY_WITH_TIMEOUT(toldNotThrough(rig->device), 10000);
        QCOMPARE(rig->jet.offersHeard.size(), 1);
        {
            const std::unique_ptr<qkvm::core::OwnLive> live = rig->device.makeLive();
            const Window window(live->open({}));
            QTest::qWait(6000);
            // From where the cards' offer left the hold: 400, 800, 1600, 1600, and each offer's 300.
            QVERIFY2(rig->jet.offersHeard.size() >= 4 && rig->jet.offersHeard.size() <= 9,
                     qPrintable(QString::number(rig->jet.offersHeard.size())));
        }
        // (The window's last connection may be on its way still, its offer not out yet: it goes as it was begun, and
        // ends in its time, 300 ms for its answer; what is counted from here is what the cards do.)
        QTest::qWait(1500);
        const qsizetype offers = rig->jet.offersHeard.size();
        QElapsedTimer asking;
        asking.start();
        while (asking.elapsed() < 4000) {
            (void)cardOf(rig->device);
            QTest::qWait(100);
        }
        QCOMPARE(rig->jet.offersHeard.size(), offers);
        QVERIFY(toldNotThrough(rig->device));
        QVERIFY(kept->notThrough);
        JetKvmDevice next(at(rig->jet), u"secret"_s, pace, kept);
        QVERIFY(toldNotThrough(next));
        QTest::qWait(600);
        QVERIFY(toldNotThrough(next));
        QCOMPARE(rig->jet.offersHeard.size(), offers);
        // A window whose connection comes about ends it: its cards have their picture from then on.
        rig->jet.answersNothing = false;
        {
            const std::unique_ptr<qkvm::core::OwnLive> live = next.makeLive();
            Window window(live->open({}));
            QTRY_VERIFY_WITH_TIMEOUT(!window.pictures.isEmpty(), 15000);
        }
        QVERIFY(!kept->notThrough);
        QVERIFY(pictureOf(next));
    }

    // A JetKVM that asks for a password where none is saved refuses every line (401): asked again only after the
    // viewer's pause (the web's RETRY_PAUSE for a no, which is not "away"), whoever asks meanwhile, and a card is told
    // why.
    void aJetKvmWithNoSavedPasswordIsAskedAtThePause()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.retryPauseMs = 500;
        JetKvmByHand jet;
        JetKvmDevice device(at(jet), QString(), pace);
        QStringList codes;
        QElapsedTimer asking;
        asking.start();
        while (asking.elapsed() < 5000) {
            const base::Result<Snapshot> card = cardOf(device);
            if (!card) {
                codes.append(QString::fromStdString(card.error().code));
            }
            QTest::qWait(50);
        }
        // At 0, 0.5, 1.0 s and so on, each with its own refusal's time: ten or so, and never one a card. (Refused since
        // J5 by the question of which JetKVM it is, which goes first: its line is not asked for.)
        const int asked = jet.asked("/device");
        QVERIFY2(asked >= 3 && asked <= 11, qPrintable(QString::number(asked)));
        QCOMPARE(jet.lineAskings, 0);
        QVERIFY(codes.contains(u"no_password"_s));
        QCOMPARE(jet.signIns, 0);
    }

    // A person who asks ("Show here") is not held back by the hold: tried at once (the web's urgent). After an offer
    // that came to nothing, it is what offers again (from the second reading of c7ec58c, B), at once, and its card
    // is no longer told that it does not come through.
    void showHereIsNotHeldBack()
    {
        Rig rig(heldLong());
        rig.jet.answersNothing = true;
        QTRY_VERIFY_WITH_TIMEOUT(foundAway(rig.device), 10000);
        rig.jet.answersNothing = false;
        QVERIFY(toldNotThrough(rig.device));
        QCOMPARE(rig.jet.offersHeard.size(), 1);
        const qsizetype offers = rig.jet.offersHeard.size();
        QElapsedTimer resuming;
        resuming.start();
        const base::Result<qkvm::core::Done> done = answerTo<qkvm::core::Done>(
            [&](auto then) { rig.device.act(qkvm::core::Action::plain(u"resume"_s), std::move(then)); });
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QVERIFY2(resuming.elapsed() < 2500, qPrintable(QString::number(resuming.elapsed())));
        QCOMPARE(rig.jet.offersHeard.size(), offers + 1);
        QVERIFY(pictureOf(rig.device));
        QVERIFY(lookedAt(rig.device).errorCode.isEmpty());
    }

    // A "Show here" that does not come to it (here: its signalling line is never answered, and no offer goes) leaves
    // the cards making no offer, as before the press: only a window, or another press.
    void aShowHereThatDoesNotComeToItLeavesTheCardsQuiet()
    {
        JetKvmDevice::Pace pace = heldWhileAway();
        pace.viewer.connection.answerWithinMs = 300;
        pace.lineWithinMs = 800;
        Rig rig(pace);
        rig.jet.answersNothing = true;
        QTRY_VERIFY_WITH_TIMEOUT(toldNotThrough(rig.device), 10000);
        rig.jet.answersNothing = false;
        rig.jet.linesSayNothing = true;
        const base::Result<qkvm::core::Done> done = answerTo<qkvm::core::Done>(
            [&](auto then) { rig.device.act(qkvm::core::Action::plain(u"resume"_s), std::move(then)); });
        QVERIFY(!done);
        rig.jet.linesSayNothing = false;
        QElapsedTimer asking;
        asking.start();
        while (asking.elapsed() < 3000) {
            QVERIFY(toldNotThrough(rig.device));
            QTest::qWait(100);
        }
        QCOMPARE(rig.jet.offersHeard.size(), 1);
    }
    // Nor is a person who adds it (identify: the web's "a person is waiting: try now").
    void identifyIsNotHeldBack()
    {
        Rig rig(heldLong());
        rig.jet.answersNothing = true;
        QTRY_VERIFY_WITH_TIMEOUT(foundAway(rig.device), 10000);
        const int lines = rig.jet.lineAskings;
        const base::Result<JetKvmDevice::Identity> who =
            answerTo<JetKvmDevice::Identity>([&](auto done) { rig.device.identify(std::move(done)); });
        QVERIFY2(who, who ? "" : who.error().message.c_str());
        QCOMPARE(rig.jet.lineAskings, lines + 1);
    }

    // Every road that connects reads the hold: a connection begun while it stands sends nothing, and ends in what
    // found the JetKVM away.
    void aConnectionBegunWhileHeldSendsNothing()
    {
        Refusing refusing;
        JetKvmDevice::Where where;
        where.address = refusing.address();
        JetKvmDevice device(where, QString(), heldLong());
        // (Found away once, by a card: held 3 s.)
        (void)cardOf(device);
        QTRY_COMPARE(refusing.cameAt.size(), 1);
        QTRY_VERIFY(foundAway(device));
        const int connections = refusing.connections;
        qkvm::core::JetKvmConnection connection(device, 99, qkvm::core::JetKvmConnection::Pace{});
        QSignalSpy ended(&connection, &qkvm::core::JetKvmConnection::ended);
        connection.start();
        QVERIFY(ended.wait(2000));
        QVERIFY2(ended.first().at(0).toString() == u"dropped"_s || ended.first().at(0).toString() == u"unreachable"_s,
                 qPrintable(ended.first().at(0).toString()));
        QCOMPARE(refusing.connections, connections);
    }

    // The reading of 077e539..2c3f523, M: the wall looks at a JetKVM every so often, and where no connection is open
    // that look is the plain one (its status over HTTP). A JetKVM whose HTTP answers and whose video never comes
    // about (its offers never answered, or its signalling line dropped at once) answers those looks: that says it was
    // heard from, and nothing of whether a connection to it comes about, so the growing hold is not undone (the web's
    // probe() notes _last_ok and calls no _answered()). Its offers, each of which takes its one viewer from whoever has
    // it, come less and less often, looked at or not (here a window's: after one that came to nothing, cards make no
    // offer, cardsMakeOneOfferWhereOneCameToNothing). And the look is held by nothing of that road's: it says what it
    // found, online, through every hold (from the second reading of c7ec58c, A: it said "offline" there).
    void aLookDoesNotUndoTheHold_data()
    {
        QTest::addColumn<bool>("dropsItsLine");
        QTest::newRow("its offers never answered") << false;
        QTest::newRow("its line dropped at once") << true;
    }
    void aLookDoesNotUndoTheHold()
    {
        QFETCH(bool, dropsItsLine);
        JetKvmDevice::Pace pace = heldWhileAway();
        pace.viewer.connection.answerWithinMs = 300;
        Rig rig(pace);
        int lines = 0;
        if (dropsItsLine) {
            rig.jet.stand->upgrade = [&lines](QTcpSocket* socket, const QByteArray& request) {
                if (!request.startsWith("GET /webrtc/signaling/client ")) {
                    return false;
                }
                ++lines;
                socket->abort();
                return true;
            };
        } else {
            rig.jet.answersNothing = true;
        }
        const auto attempts = [&] { return dropsItsLine ? lines : static_cast<int>(rig.jet.offersHeard.size()); };
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        const Window window(live->open({}));
        int looks = 0;
        QStringList said;
        QElapsedTimer asking;
        asking.start();
        qint64 lookedAtMs = -1000;
        qint64 askedAtMs = -1000;
        while (asking.elapsed() < 6000) {
            // (A card every 250 ms and a look every 400: the wall's second and four seconds, as the hold here is a
            // tenth of the driver's.)
            if (asking.elapsed() - askedAtMs >= 250) {
                askedAtMs = asking.elapsed();
                (void)cardOf(rig.device);
            }
            if (asking.elapsed() - lookedAtMs >= 400) {
                lookedAtMs = asking.elapsed();
                const QString state = lookedAt(rig.device).state;
                if (state != u"online"_s && !said.contains(state)) {
                    said.append(state);
                }
                ++looks;
            }
            QTest::qWait(20);
        }
        // About five or six, where looks that undid the hold had one every half second or so.
        QVERIFY2(attempts() >= 3 && attempts() <= 7, qPrintable(QString::number(attempts())));
        QVERIFY(looks >= 10);
        QVERIFY2(said.isEmpty(), qPrintable(said.join(u", "_s)));
        // (Looks went out the plain way between the holds, and it answered them.)
        QVERIFY2(rig.jet.asked("/device/status") >= 1, qPrintable(QString::number(rig.jet.asked("/device/status"))));
    }

    // From the second reading of c7ec58c, A: a JetKVM that is well, whose HTTP answers and whose video never comes
    // through (its offers heard and never answered: ICE failing), while a window and its card show it, raises no alert
    // over two minutes of the wall's (here a tenth of its times, its alerts as the wall keeps them: two looks in a row
    // say what changed). Held by the road's hold, its looks said "offline" through each hold, and once the hold had
    // grown an "Offline" and a "Recovered" came about every 30 s.
    void aWellJetKvmWhoseVideoNeverComesRaisesNoAlert()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        qkvm::core::Incidents incidents(directory.filePath(u"incidents.json"_s));
        Rig rig(aTenthOfTheWalls());
        rig.jet.answersNothing = true;
        const std::unique_ptr<qkvm::core::OwnLive> live = rig.device.makeLive();
        const Window window(live->open({}));
        QStringList changes;
        QStringList states;
        QElapsedTimer asking;
        asking.start();
        qint64 lookedAtMs = -1000;
        while (asking.elapsed() < 12'000) {
            (void)cardOf(rig.device);
            if (asking.elapsed() - lookedAtMs >= 400) {
                lookedAtMs = asking.elapsed();
                const QString state = lookedAt(rig.device).state;
                states.append(state);
                for (const auto& change : incidents.observe(u"own"_s, {{u"jet"_s, u"JetKVM"_s, state}})) {
                    changes.append(change.state);
                }
            }
            QTest::qWait(100);
        }
        QVERIFY2(changes.isEmpty(), qPrintable(changes.join(u", "_s)));
        QVERIFY2(!states.contains(u"offline"_s), qPrintable(states.join(u' ')));
        // (Its offers went on at the hold's pace all the while: the window's.)
        QVERIFY2(rig.jet.offersHeard.size() >= 4, qPrintable(QString::number(rig.jet.offersHeard.size())));
    }

    // A JetKVM that comes back after it was away long enough for both its roads' holds to grow to their longest: each
    // lets it in once its own hold ends, a look and a card. And the look's own answer ended its count: one look lost
    // after that is held as the first after a well one, and not as long as the looks before had grown it (where only
    // a status over a connection ended the count, it stayed at its longest, and one lost look later the JetKVM was said
    // "offline" that long: in the tray, or stepped aside, an alert for a JetKVM that is well).
    void aJetKvmThatComesBackIsLetInOnceEachHoldEnds()
    {
        const JetKvmDevice::Pace pace = heldWhileAway(); // (200 ms, doubled, 1600 at the most)
        auto away = std::make_unique<Refusing>();
        const quint16 port = away->server.serverPort();
        JetKvmDevice::Where where;
        where.address = away->address();
        JetKvmDevice device(where, u"secret"_s, pace);
        QElapsedTimer asking;
        asking.start();
        while (asking.elapsed() < 5000) {
            (void)cardOf(device);
            QCOMPARE(lookedAt(device).state, u"offline"_s);
            QTest::qWait(100);
        }
        // Back, at the same port, and well. A look lets it in within the look's longest hold; no card asks meanwhile.
        away.reset();
        auto back = std::make_unique<JetKvmByHand>(std::nullopt, port);
        QVERIFY(back->stand->isListening());
        QElapsedTimer coming;
        coming.start();
        QTRY_COMPARE_WITH_TIMEOUT(lookedAt(device).state, u"online"_s, 5000);
        QVERIFY2(coming.elapsed() < pace.longestHoldMs + 1500, qPrintable(QString::number(coming.elapsed())));
        // One look lost, and back: held as after a well one (200 ms), not 1600.
        back.reset();
        QCOMPARE(lookedAt(device).state, u"offline"_s);
        back = std::make_unique<JetKvmByHand>(std::nullopt, port);
        QVERIFY(back->stand->isListening());
        coming.restart();
        QTRY_COMPARE_WITH_TIMEOUT(lookedAt(device).state, u"online"_s, 5000);
        QVERIFY2(coming.elapsed() < pace.longestHoldMs - 300, qPrintable(QString::number(coming.elapsed())));
        // And a card is let in on the road that connects, once its own hold is over: its picture.
        const JetKvmViewerByHand viewer{*back};
        QTRY_VERIFY_WITH_TIMEOUT(!foundAway(device), pace.longestHoldMs + 2000);
        const base::Result<Snapshot> card = pictureOf(device);
        QVERIFY2(card, card ? "" : card.error().message.c_str());
    }

    // What says it can be used (here its sign-in, and the first status over a connection) ends the look's count too:
    // one look lost after its connection is gone is held as after a well one, not as long as the looks before had
    // grown it.
    void aConnectionThatComesAboutEndsTheLooksCountToo()
    {
        const JetKvmDevice::Pace pace = heldWhileAway(); // (200 ms, doubled, 1600 at the most)
        auto away = std::make_unique<Refusing>();
        const quint16 port = away->server.serverPort();
        JetKvmDevice::Where where;
        where.address = away->address();
        JetKvmDevice device(where, u"secret"_s, pace);
        QElapsedTimer asking;
        asking.start();
        while (asking.elapsed() < 4000) {
            QCOMPARE(lookedAt(device).state, u"offline"_s);
            QTest::qWait(100);
        }
        // Back: a card connects (no look the plain way meanwhile), and a look is answered over its connection.
        away.reset();
        auto back = std::make_unique<JetKvmByHand>(std::nullopt, port);
        auto viewer = std::make_unique<JetKvmViewerByHand>(*back);
        QVERIFY(pictureOf(device));
        QCOMPARE(lookedAt(device).state, u"online"_s);
        // Gone, connection and all; one look lost; and back.
        viewer.reset();
        back.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!device.viewer().isOpen(), 10000);
        QCOMPARE(lookedAt(device).state, u"offline"_s);
        back = std::make_unique<JetKvmByHand>(std::nullopt, port);
        QVERIFY(back->stand->isListening());
        QElapsedTimer coming;
        coming.start();
        QTRY_COMPARE_WITH_TIMEOUT(lookedAt(device).state, u"online"_s, 5000);
        QVERIFY2(coming.elapsed() < pace.longestHoldMs - 300, qPrintable(QString::number(coming.elapsed())));
    }
};

QTEST_GUILESS_MAIN(TestJetKvmLive)
#include "test_jetkvm_live.moc"
