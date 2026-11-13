#include "janus_by_hand.h"
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUdpSocket>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <qkvm/base/clock.h>
#include <qkvm/core/kvmd_live.h>
#include <qkvm/core/kvmd_webrtc.h>
#include <qkvm/core/video_diagnostics.h>
#include <qkvm/core/webrtc_receiver.h>
#include <qkvm/engine/gpu.h>
#include <qkvm/engine/scene.h>
#include <thread>

using namespace Qt::StringLiterals;
using namespace qkvm;
using namespace qkvm::core::testing;
class TestNativeVideo : public QObject {
    Q_OBJECT
private slots:
    // M-D4. libdatachannel calls the receiver's mailbox on its own thread while holding a lock of
    // its own; the program must never call into the library while holding a mailbox lock, or each
    // thread can wait for the other for good. The seam counts every such call, from the answer made
    // when gathering ends to the key requests: here one every millisecond for a minute, against
    // pictures sent 60 times a second, with a watchdog on the Qt loop. (Before: requestKey() held
    // the lock while it asked the library; the stress may or may not hang, the seam always counts.)
    void theLibraryIsNeverCalledUnderTheMailboxLock()
    {
        const int before = core::WebRtcReceiver::libraryCallsUnderOwnLock();
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.paceEvery(16);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        int frames = 0;
        connect(live.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
            if (picture.h264) {
                ++frames;
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(frames >= 4, 15000);
        QCOMPARE(live->transport(), u"webrtc"_s);
        std::atomic<qint64> beat = base::steadyNowUs();
        std::atomic<bool> finished = false;
        std::thread watchdog([&] {
            while (!finished) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (!finished && base::steadyNowUs() - beat > 10'000'000) {
                    std::fputs("M-D4 watchdog: the Qt loop has not turned for 10 s; the receiver is deadlocked\n",
                               stderr);
                    std::fflush(stderr);
                    std::_Exit(3);
                }
            }
        });
        QTimer asking;
        asking.setTimerType(Qt::PreciseTimer);
        asking.setInterval(1);
        int asked = 0;
        // (A Windows timer of one millisecond turns about every eight, or every 15.6 at Windows' default
        // resolution: sixteen requests a turn keep the bar below at half of what the coarsest gives.)
        connect(&asking, &QTimer::timeout, this, [&] {
            beat = base::steadyNowUs();
            for (int each = 0; each < 16; ++each) {
                live->askForAKeyFrame();
                ++asked;
            }
        });
        const int packetsBefore = janus.packets;
        QElapsedTimer ran;
        ran.start();
        asking.start();
        while (ran.elapsed() < 60'000) {
            QTest::qWait(100);
        }
        asking.stop();
        finished = true;
        watchdog.join();
        const int sent = janus.packets - packetsBefore;
        qInfo() << "key requests:" << asked << "pictures sent:" << sent << "in" << ran.elapsed() << "ms";
        QVERIFY2(asked > 30'000, qPrintable(u"only %1 key requests in a minute"_s.arg(asked)));
        QVERIFY2(sent > 1'800, qPrintable(u"only %1 pictures sent in a minute"_s.arg(sent)));
        QCOMPARE(live->transport(), u"webrtc"_s);
        live->close();
        QCOMPARE(core::WebRtcReceiver::libraryCallsUnderOwnLock() - before, 0);
    }
    // M6 increment 2a (design 3.2, L7): each picture is stamped on this PC's steady clock as it
    // becomes whole, on the library's thread, before it waits in the mailbox for the interface thread;
    // and the stamp goes with it to whoever is handed it (Live::Picture::arrivedUs). Janus sends from a
    // thread of its own at 60 a second while the Qt loop is stalled for 22 ms at a time, until eight
    // pictures have waited out a stall (15 s at most): those are handed over in one turn with the one
    // before, and their stamps step as the film did, not by the microseconds between two handings.
    // (Before: no stamp; a stamp taken at the drain would step by nothing.) The stall is a busy wait
    // (a sleep of Windows' runs long by up to its timer's 15.6 ms), short of three pictures, which the
    // mailbox of two let go when this was written (2a; since 2b it holds eight).
    void picturesCarryWhenTheyBecameWhole()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.fromAThread(16);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        struct Seen {
            qint64 arrivedUs = 0;
            qint64 handedUs = 0;
        };
        QList<Seen> seen;
        connect(live.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
            if (picture.h264) {
                seen.append({picture.arrivedUs, base::steadyNowUs()});
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(seen.size() >= 4, 15000);
        QCOMPARE(live->transport(), u"webrtc"_s);
        // (Handed in one turn: within 2 ms of the one before; there is no other way to two handings so
        // close at 60 a second.)
        const auto handedTogether = [&seen] {
            int together = 0;
            for (qsizetype at = 1; at < seen.size(); ++at) {
                together += seen[at].handedUs - seen[at - 1].handedUs < 2'000 ? 1 : 0;
            }
            return together;
        };
        QElapsedTimer ran;
        ran.start();
        int stalls = 0;
        while (handedTogether() < 8 && ran.elapsed() < 15'000) {
            const qint64 until = base::steadyNowUs() + 22'000;
            while (base::steadyNowUs() < until) {
            }
            ++stalls;
            QTest::qWait(50);
        }
        live->close();
        int together = 0;
        qint64 steps = 0;
        for (qsizetype at = 1; at < seen.size(); ++at) {
            const qint64 arrived = seen[at].arrivedUs - seen[at - 1].arrivedUs;
            QVERIFY2(seen[at].arrivedUs != 0 && arrived >= 0,
                     qPrintable(u"picture %1 arrived %2 us after the one before"_s.arg(at).arg(arrived)));
            QVERIFY(seen[at].arrivedUs <= seen[at].handedUs);
            if (seen[at].handedUs - seen[at - 1].handedUs < 2'000) {
                ++together;
                steps += arrived;
            }
        }
        qInfo() << "stalls:" << stalls << "pictures:" << seen.size()
                << "handed in one turn with the one before:" << together << "their stamps' steps in all:" << steps
                << "us";
        QVERIFY2(together >= 8, qPrintable(u"only %1 pictures waited out a stall"_s.arg(together)));
        // About a picture's time apart on average (16.7 ms), held to 4 ms: on a PC that is busy, the
        // library's thread may take two pictures at once now and then. Stamped at the drain, they would
        // be microseconds apart, all of them.
        QVERIFY2(steps >= qint64{together} * 4'000,
                 qPrintable(u"%1 handed together were stamped %2 us apart in all"_s.arg(together).arg(steps)));
    }
    // M6 increment 2b (design 3.2, M1, F1): the mailbox holds as many pictures as the engine lets wait,
    // eight, so that a stall of the interface thread of up to about 130 ms with the engine's queue
    // empty loses nothing. Janus sends from a thread of its own at 60 a second into a stream that
    // decodes; the Qt loop is stalled by a busy 100 ms, three times, with half a second between: every
    // picture sent meanwhile reaches the stream (at most the two in flight at the edges missing), and
    // the stream drops none behind. (Before: the mailbox held two, and at the third threw them away and
    // waited for the next key frame, half a second of pictures here.)
    void aStallOfAHundredMsLosesNoPicture()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.fromAThread(16);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        auto made = engine::Gpu::create({.software = true});
        QVERIFY(made);
        auto gpu = std::move(made).value();
        auto scene = gpu->makeScene({});
        auto& stream = scene->add(1, engine::Codec::H264);
        stream.shownAt({1280, 720}, engine::Importance::ShownLarge);
        int handed = 0;
        connect(live.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
            if (picture.h264) {
                ++handed;
                stream.push(
                    {std::as_bytes(std::span(picture.data.constData(), static_cast<std::size_t>(picture.data.size()))),
                     picture.arrivedUs, picture.keyFrame});
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(stream.stats().decoded >= 30, 20000);
        QCOMPARE(live->transport(), u"webrtc"_s);
        const int sentBefore = janus.film().sent;
        const int handedBefore = handed;
        const std::uint64_t droppedBefore = stream.stats().droppedBehind;
        for (int stall = 0; stall < 3; ++stall) {
            const qint64 until = base::steadyNowUs() + 100'000;
            while (base::steadyNowUs() < until) {
            }
            QTest::qWait(500);
        }
        const int sent = janus.film().sent - sentBefore;
        const int got = handed - handedBefore;
        const std::uint64_t dropped = stream.stats().droppedBehind - droppedBefore;
        live->close();
        qInfo() << "sent:" << sent << "handed:" << got << "dropped behind:" << dropped;
        QVERIFY2(sent >= 60, qPrintable(u"only %1 sent in 1.8 s"_s.arg(sent)));
        QVERIFY2(got >= sent - 2, qPrintable(u"%1 sent, %2 handed"_s.arg(sent).arg(got)));
        QCOMPARE(dropped, std::uint64_t{0});
    }
    // A longer stall (half a second, thirty pictures at 60 a second) is more than may wait: the run is
    // lost, and a key frame is asked of the far end; the pictures go on from the film's next key frame.
    // (A Comet's pace: twice a second at most, as before 2b. Half a second, not a quarter: on a PC that
    // is busy, the far end's sending and the library's thread may be held back by the stall too.)
    void aLongerStallLosesTheRunAndAsksForAKeyFrame()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.fromAThread(16);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        int handed = 0;
        connect(live.get(), &core::Live::pictureCame, this,
                [&](const core::Live::Picture& picture) { handed += picture.h264 ? 1 : 0; });
        QTRY_VERIFY_WITH_TIMEOUT(handed >= 40, 15000);
        QCOMPARE(live->transport(), u"webrtc"_s);
        // While all goes well nothing is asked (2c, the second reading: put back, said only where nothing
        // was let go meanwhile either: a busy PC may lose a run of its own then, and ask for that).
        const int quietAsked = janus.keyFrameAsks;
        const quint64 quietLetGo = live->picturesLetGo();
        QTest::qWait(600);
        if (live->picturesLetGo() == quietLetGo) {
            QCOMPARE(janus.keyFrameAsks.load(), quietAsked);
        }
        const int asked = janus.keyFrameAsks;
        const quint64 letGoBefore = live->picturesLetGo();
        const qint64 until = base::steadyNowUs() + 500'000;
        while (base::steadyNowUs() < until) {
        }
        // Said as it stands, not as of the mailbox's last drain, which has not come yet (L6).
        QVERIFY2(live->picturesLetGo() >= letGoBefore + 9,
                 qPrintable(u"%1 let go before the drain"_s.arg(live->picturesLetGo() - letGoBefore)));
        QTRY_VERIFY_WITH_TIMEOUT(janus.keyFrameAsks > asked, 5000);
        const int after = handed;
        QTRY_VERIFY_WITH_TIMEOUT(handed > after + 30, 5000);
        // Every picture let go is counted, and said to whoever shows the machine (2b, after its reading),
        // also across the native connection made anew after the line fell.
        const quint64 letGo = live->picturesLetGo();
        qInfo() << "let go:" << letGo;
        QVERIFY2(letGo >= 9, qPrintable(QString::number(letGo)));
        janus.fall();
        const int beforeTheFall = handed;
        QTRY_VERIFY_WITH_TIMEOUT(handed > beforeTheFall + 10 && live->transport() == u"webrtc"_s, 15000);
        QVERIFY2(live->picturesLetGo() >= letGo && live->picturesLetGo() < letGo + 1000,
                 qPrintable(u"%1 after the fall, %2 before"_s.arg(live->picturesLetGo()).arg(letGo)));
        live->close();
    }
    // 2b, after its reading (a medium): a window's wish that the pace refused is asked once the pace has
    // passed. A second window opens and asks; a third opens 200 ms later, within the Comet's pace of
    // 500 ms, and its wish is asked when that has passed: the far end hears two requests, answers each
    // with a key frame, and the third window has pictures long before the film's own next key frame,
    // 2 s off. (Before: the third's wish was dropped, nothing asked, and it waited for that one.)
    void aWishThePaceRefusedIsAskedOnceThePaceHasPassed()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.fromAThread(67);
        janus.answersKeyFrameRequests = true;
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto first = source.open({});
        int firstPictures = 0;
        connect(first.get(), &core::Live::pictureCame, this,
                [&](const core::Live::Picture& picture) { firstPictures += picture.h264 ? 1 : 0; });
        QTRY_VERIFY_WITH_TIMEOUT(firstPictures >= 4, 15000);
        QTest::qWait(700);
        const int asked = janus.keyFrameAsks;
        auto second = source.open({});
        int secondPictures = 0;
        connect(second.get(), &core::Live::pictureCame, this,
                [&](const core::Live::Picture& picture) { secondPictures += picture.h264 ? 1 : 0; });
        QTRY_COMPARE_WITH_TIMEOUT(janus.keyFrameAsks.load(), asked + 1, 1000);
        QTest::qWait(150);
        QElapsedTimer opened;
        opened.start();
        auto third = source.open({});
        qint64 thirdFirstMs = -1;
        connect(third.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
            if (picture.h264 && thirdFirstMs < 0) {
                thirdFirstMs = opened.elapsed();
            }
        });
        QTest::qWait(150);
        QCOMPARE(janus.keyFrameAsks.load(), asked + 1);
        QTRY_COMPARE_WITH_TIMEOUT(janus.keyFrameAsks.load(), asked + 2, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(thirdFirstMs >= 0, 1500);
        qInfo() << "the third window's first picture after" << thirdFirstMs << "ms";
        QVERIFY2(thirdFirstMs < 1200, qPrintable(QString::number(thirdFirstMs)));
        QVERIFY(secondPictures > 0);
        third->close();
        second->close();
        first->close();
    }
    // 2c, the second reading (L3): a wish the pace refused is asked once the pace has passed also where
    // the far end sends nothing meanwhile (a screen that stands still, a device that sends only what
    // changes): no packet comes to ask it at, and a timer does.
    void aWishIsAskedOnceThePaceHasPassedAlsoWhereNothingComes()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.fromAThread(67);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto first = source.open({});
        int firstPictures = 0;
        connect(first.get(), &core::Live::pictureCame, this,
                [&](const core::Live::Picture& picture) { firstPictures += picture.h264 ? 1 : 0; });
        QTRY_VERIFY_WITH_TIMEOUT(firstPictures >= 4, 15000);
        QTest::qWait(700);
        janus.sendPictures = false;
        QTest::qWait(200);
        const int asked = janus.keyFrameAsks;
        auto second = source.open({});
        QTRY_COMPARE_WITH_TIMEOUT(janus.keyFrameAsks.load(), asked + 1, 1000);
        QTest::qWait(150);
        auto third = source.open({});
        QTest::qWait(100);
        QCOMPARE(janus.keyFrameAsks.load(), asked + 1);
        QTRY_COMPARE_WITH_TIMEOUT(janus.keyFrameAsks.load(), asked + 2, 1500);
        third->close();
        second->close();
        first->close();
    }
    // Wishes within the pace are asked once: fifty from the windows at once go out as one request (2b,
    // after its reading: the receiver asks the box, not only the box itself is held to it).
    void manyWishesWithinThePaceAreAskedOnce()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.fromAThread(16);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        int handed = 0;
        connect(live.get(), &core::Live::pictureCame, this,
                [&](const core::Live::Picture& picture) { handed += picture.h264 ? 1 : 0; });
        QTRY_VERIFY_WITH_TIMEOUT(handed >= 10, 15000);
        QTest::qWait(700);
        const int asked = janus.keyFrameAsks;
        for (int wish = 0; wish < 50; ++wish) {
            live->askForAKeyFrame();
        }
        QTRY_COMPARE_WITH_TIMEOUT(janus.keyFrameAsks.load(), asked + 1, 1000);
        QTest::qWait(300);
        QCOMPARE(janus.keyFrameAsks.load(), asked + 1);
        // (Those refused are one wish, asked once more after the pace where no key frame came first.)
        QTest::qWait(1200);
        QVERIFY2(janus.keyFrameAsks <= asked + 2, qPrintable(QString::number(janus.keyFrameAsks - asked)));
        live->close();
    }
    // M6 increment 2b (design 3.2, M2): a window that joins a shared connection asks for a key frame,
    // and the asking throws nothing away: the window that was there goes on. Janus sends at the film's
    // own 15 a second, whose key frames are 2 s apart, and answers no request for one; a second window
    // opens, four times, 700 ms apart, and the first's pictures keep arriving with no gap longer than
    // 400 ms, six of them (two, where the PC is not busy). (Before: the asking threw away what was being
    // put together and waited for the next key frame, which froze every window for up to 2 s.) The second
    // windows, which begin at a key frame, have pictures between them: their 2.8 s hold one or more
    // (2b, after its reading; answered requests are aWishThePaceRefusedIsAskedOnceThePaceHasPassed's).
    void aSecondWindowDoesNotFreezeTheFirst()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.fromAThread(67);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto first = source.open({});
        QList<qint64> arrivals;
        connect(first.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
            if (picture.h264) {
                arrivals.append(picture.arrivedUs);
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(arrivals.size() >= 4, 15000);
        QCOMPARE(first->transport(), u"webrtc"_s);
        const qsizetype from = arrivals.size();
        int secondPictures = 0;
        for (int round = 0; round < 4; ++round) {
            auto second = source.open({});
            connect(second.get(), &core::Live::pictureCame, this,
                    [&](const core::Live::Picture& picture) { secondPictures += picture.h264 ? 1 : 0; });
            QTest::qWait(700);
            second->close();
        }
        QTest::qWait(200);
        first->close();
        qint64 longest = 0;
        for (qsizetype at = from; at < arrivals.size(); ++at) {
            longest = std::max(longest, arrivals[at] - arrivals[at - 1]);
        }
        qInfo() << "pictures:" << arrivals.size() - from << "longest gap:" << longest / 1000 << "ms"
                << "pictures of the second windows:" << secondPictures;
        QVERIFY2(arrivals.size() - from >= 30, qPrintable(u"only %1 pictures"_s.arg(arrivals.size() - from)));
        QVERIFY2(longest <= 400'000, qPrintable(u"the first window waited %1 ms"_s.arg(longest / 1000)));
        QVERIFY(secondPictures > 0);
    }
    void signalingRejectsMalformedUtf8()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        std::unique_ptr<core::DeviceLine> line;
        driver.openVideoLine([&](auto answer) {
            if (answer) {
                line = std::move(answer).value();
            }
        });
        QTRY_VERIFY(line != nullptr);
        QSignalSpy text(line.get(), &core::DeviceLine::textCame);
        QSignalSpy ended(line.get(), &core::DeviceLine::ended);
        line->open();
        QTRY_VERIFY(line->isIn());
        // An invalid UTF-8 continuation arrives after a fragmented ASCII character.
        janus.frame(QByteArray::fromHex("010161890170800182"));
        QTRY_COMPARE(ended.size(), 1); // invalid UTF-8 continuation is refused
        QCOMPARE(text.size(), 0);
    }
    void fragmentedSignalingKeepsUtf8AcrossFrames()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        std::unique_ptr<core::DeviceLine> line;
        driver.openVideoLine([&](auto answer) {
            if (answer) {
                line = std::move(answer).value();
            }
        });
        QTRY_VERIFY(line != nullptr);
        QSignalSpy text(line.get(), &core::DeviceLine::textCame);
        QSignalSpy ended(line.get(), &core::DeviceLine::ended);
        line->open();
        QTRY_VERIFY(line->isIn());
        janus.frame(QByteArray::fromHex("0101c38901708001a9"));
        QTRY_COMPARE(text.size(), 1);
        QCOMPARE(text[0][0].toByteArray(), QByteArray::fromHex("c3a9"));
        QCOMPARE(ended.size(), 0);
        // Each fragment fits, but their combined message exceeds 128 KiB.
        const QByteArray body(70000, 'a');
        janus.frame(QByteArray::fromHex("017f0000000000011170") + body);
        janus.frame(QByteArray::fromHex("807f0000000000011170") + body);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(text.size(), 1);
    }
    // A Comet's Janus is held to the rule a JetKVM's addresses are (rtc_sdp.h, judgeAddress; 852bf06 and 2e's
    // second reading): an address it trickles is taken only where it is UDP to a host by its number (no name is
    // looked up), one that may be reached (not unspecified, multicast, the broadcast, nor this PC's loopback
    // unless the KVM itself is spoken to there), each refusal counted by its reason. (Before: each went to the
    // library as it came, names among them.) Every address here that is taken, or would have been before, is on
    // this PC: its loopback, or a multicast group that never leaves it.
    void aJanusAddressIsUdpToAHostByItsNumber_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<bool>("loopbackAllowed");
        QTest::addColumn<QByteArray>("reason"); // empty: taken
        const QByteArray at = "candidate:1 1 udp 2015363327 ";
        QTest::newRow("loopback, the KVM on it") << at + "127.0.0.1 9 typ host" << true << QByteArray();
        QTest::newRow("TCP") << QByteArray("candidate:1 1 TCP 2015363327 127.0.0.1 9 typ host tcptype active") << true
                             << QByteArray("notUdp");
        QTest::newRow("a name") << at + "localhost 9 typ host" << true << QByteArray("named");
        QTest::newRow("unspecified") << at + "0.0.0.0 9 typ host" << true << QByteArray("unreachable");
        QTest::newRow("unspecified IPv6") << at + ":: 9 typ host" << true << QByteArray("unreachable");
        QTest::newRow("multicast of this PC alone") << at + "ff01::1 9 typ host" << true << QByteArray("unreachable");
        QTest::newRow("loopback, the KVM not on it")
            << at + "127.0.0.1 9 typ host" << false << QByteArray("unreachable");
        QTest::newRow("IPv6 loopback, the KVM not on it")
            << at + "::1 9 typ host" << false << QByteArray("unreachable");
        QTest::newRow("a tab after its type") << at + "127.0.0.1 9 typ host x\ty" << true << QByteArray("malformed");
        QTest::newRow("no port") << at + "127.0.0.1 0 typ host" << true << QByteArray("malformed");
        QTest::newRow("a part with a nought first") << at + "127.0.0.01 9 typ host" << true << QByteArray("malformed");
    }
    void aJanusAddressIsUdpToAHostByItsNumber()
    {
        QFETCH(QByteArray, text);
        QFETCH(bool, loopbackAllowed);
        QFETCH(QByteArray, reason);
        JanusOfferByHand janus;
        core::WebRtcReceiver::Setup setup;
        setup.loopbackAllowed = loopbackAllowed;
        core::WebRtcReceiver receiver(setup);
        const QByteArray offer = janus.offer();
        QVERIFY(!offer.isEmpty());
        QVERIFY(receiver.offer(offer));
        const core::WebRtcReceiver::Refused before = receiver.refused();
        QCOMPARE(receiver.candidate(text), reason.isEmpty());
        const core::WebRtcReceiver::Refused after = receiver.refused();
        QCOMPARE(after.candidates - before.candidates, quint64(reason.isEmpty() ? 0 : 1));
        QCOMPARE(after.malformedCandidates - before.malformedCandidates, quint64(reason == "malformed" ? 1 : 0));
        QCOMPARE(after.candidatesNotUdp - before.candidatesNotUdp, quint64(reason == "notUdp" ? 1 : 0));
        QCOMPARE(after.namedCandidates - before.namedCandidates, quint64(reason == "named" ? 1 : 0));
        QCOMPARE(after.unreachableCandidates - before.unreachableCandidates, quint64(reason == "unreachable" ? 1 : 0));
        receiver.close();
    }

    // The addresses inside Janus's offer are taken out of it and go the way every address goes, after it: judged,
    // and counted toward the 64 with those trickled after it. Here its own, then one by a name, one of TCP, one
    // unspecified, 70 more of loopback and an end of candidates.
    void addressesInAJanusOfferGoTheWayEveryAddressGoes()
    {
        JanusOfferByHand janus;
        const QList<QByteArray> own = janus.addresses();
        QVERIFY(!own.isEmpty());
        // (Those refused first: only those taken count toward the 64.)
        QByteArray offer = janus.offer();
        offer += "a=candidate:9 1 UDP 1 localhost 9 typ host\r\na=candidate:9 1 TCP 1 127.0.0.1 9 typ host\r\n"
                 "a=candidate:9 1 UDP 1 0.0.0.0 9 typ host\r\n";
        for (int port = 0; port < 70; ++port) {
            offer += "a=candidate:9 1 UDP 1 127.0.0.1 " + QByteArray::number(20000 + port) + " typ host\r\n";
        }
        offer += "a=end-of-candidates\r\n";
        core::WebRtcReceiver::Setup setup;
        setup.loopbackAllowed = true;
        core::WebRtcReceiver receiver(setup);
        QVERIFY(receiver.offer(offer));
        const core::WebRtcReceiver::Refused refused = receiver.refused();
        QCOMPARE(refused.namedCandidates, quint64{1});
        QCOMPARE(refused.candidatesNotUdp, quint64{1});
        QCOMPARE(refused.unreachableCandidates, quint64{1});
        QCOMPARE(refused.candidatesBeyond, quint64(own.size() + 70 - 64));
        QCOMPARE(refused.candidates, quint64(own.size() + 70 - 64 + 3));
        // (And the 64 taken are no more: one more trickled is beyond.)
        QVERIFY(!receiver.candidate("candidate:9 1 UDP 1 127.0.0.1 30000 typ host"));
        QCOMPARE(receiver.refused().candidatesBeyond, quint64(own.size() + 70 - 64 + 1));
        receiver.close();
    }

    // What is refused is never asked of the network: an address of this PC's loopback, by its number or by a
    // name, where the KVM is not spoken to there, has no STUN sent to it (a service of this PC's that listens
    // there hears nothing). Where it is, the same address is asked (the test can see it).
    void anAddressRefusedIsNeverAsked()
    {
        for (const bool kvmOnLoopback : {false, true}) {
            QUdpSocket service;
            QVERIFY(service.bind(QHostAddress::LocalHost, 0));
            const QByteArray port = QByteArray::number(service.localPort());
            int heard = 0;
            connect(&service, &QUdpSocket::readyRead, this, [&] {
                while (service.hasPendingDatagrams()) {
                    service.receiveDatagram();
                    ++heard;
                }
            });
            JanusOfferByHand janus;
            core::WebRtcReceiver::Setup setup;
            setup.loopbackAllowed = kvmOnLoopback;
            core::WebRtcReceiver receiver(setup);
            // Said inside its offer, and trickled after it.
            QVERIFY(
                receiver.offer(janus.offer() + "a=candidate:3 1 UDP 2015363327 127.0.0.1 " + port + " typ host\r\n"));
            receiver.candidate("candidate:1 1 UDP 2015363327 127.0.0.1 " + port + " typ host");
            if (!kvmOnLoopback) {
                receiver.candidate("candidate:2 1 UDP 2015363327 localhost " + port + " typ host");
                QTest::qWait(2000);
                QCOMPARE(heard, 0);
            } else {
                QTRY_VERIFY_WITH_TIMEOUT(heard > 0, 10000);
            }
            receiver.close();
        }
    }

    // A Janus that says addresses the rule refuses, in its offer and trickled after it, more than 64 among
    // them, is still shown: they are let go and counted, and nothing ends. (Before: the 65th trickled ended it
    // as "an invalid connection candidate".)
    void aJanusWithAddressesNotTakenIsStillShown()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        janus.moreInItsOffer = {"candidate:9 1 UDP 1 localhost 9 typ host", "candidate:9 1 TCP 1 127.0.0.1 9 typ host",
                                "candidate:9 1 UDP 1 0.0.0.0 9 typ host"};
        for (int port = 0; port < 70; ++port) {
            janus.trickledAfterItsOffer.append("candidate:9 1 UDP 1 127.0.0.1 " + QByteArray::number(21000 + port) +
                                               " typ host");
        }
        janus.trickledAfterItsOffer.append("candidate:9 1 UDP 1 localhost 9 typ host");
        // (The reading of 56258cd, L2: what was refused is in the video's diagnostics, as the answer goes and as
        // the session ends.)
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        {
            core::VideoDiagnostics log(path);
            QVERIFY(log.isOpen());
            core::KvmdDevice driver(kvm.where(), kvm.password);
            core::KvmdWebRtc attempt(driver);
            QSignalSpy pictures(&attempt, &core::KvmdWebRtc::pictureCame);
            QSignalSpy ended(&attempt, &core::KvmdWebRtc::ended);
            attempt.start();
            QTRY_VERIFY_WITH_TIMEOUT(pictures.size() >= 10, 15000);
            QVERIFY2(ended.isEmpty(), ended.isEmpty() ? "" : qPrintable(ended.first().at(1).toString()));
            QVERIFY(!janus.protocolError);
            janus.fall();
            QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
        }
        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QList<QJsonObject> said;
        for (const QByteArray& line : written.readAll().split('\n')) {
            const QJsonObject one = QJsonDocument::fromJson(line).object();
            if (one.value(u"event"_s).toString() == u"rtc_addresses_refused"_s) {
                said.append(one);
            }
        }
        QCOMPARE(said.size(), 2);
        // At the answer: the three in its offer (and of those trickled after it, what came before the answer
        // was made: its name may); at the end, those trickled too (beyond the 64, and a name).
        QVERIFY(said.first().value(u"values"_s).toObject().value(u"named"_s).toInteger() >= 1);
        QCOMPARE(said.first().value(u"values"_s).toObject().value(u"not_udp"_s).toInteger(), 1);
        QCOMPARE(said.first().value(u"values"_s).toObject().value(u"unreachable"_s).toInteger(), 1);
        QCOMPARE(said.last().value(u"values"_s).toObject().value(u"named"_s).toInteger(), 2);
        QVERIFY(said.last().value(u"values"_s).toObject().value(u"beyond"_s).toInteger() >= 6);
        QCOMPARE(said.last().value(u"values"_s).toObject().value(u"all"_s).toInteger(),
                 said.last().value(u"values"_s).toObject().value(u"named"_s).toInteger() + 2 +
                     said.last().value(u"values"_s).toObject().value(u"beyond"_s).toInteger());
    }
    // The reading of 56258cd (L1): what the library is handed of Janus's addresses is made anew of what was
    // judged, inside its offer and trickled after it alike: its transport as UDP, its numbers as numbers, nothing
    // after its type.
    void theLibraryReadsAJanusAddressAsItWasJudged()
    {
        JanusOfferByHand janus;
        core::WebRtcReceiver::Setup setup;
        setup.loopbackAllowed = true;
        core::WebRtcReceiver receiver(setup);
        QVERIFY(receiver.offer(janus.offer() + "a=candidate:8 01 udp 0212232 127.0.0.1 10 typ host generation 0\r\n"));
        QVERIFY(receiver.candidate("candidate:7 01 udp 0212231 127.0.0.1 9 typ host generation 0 ufrag Ab12"));
        const QByteArray held = receiver.remoteDescription();
        QVERIFY2(held.contains("a=candidate:7 1 UDP 212231 127.0.0.1 9 typ host"), held.constData());
        QVERIFY2(held.contains("a=candidate:8 1 UDP 212232 127.0.0.1 10 typ host"), held.constData());
        QVERIFY2(!held.contains("generation") && !held.contains(" udp "), held.constData());
        receiver.close();
    }
    void signalingRequiresTheJanusSubprotocol()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        janus.noSubprotocol();
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdWebRtc attempt(driver);
        QSignalSpy ended(&attempt, &core::KvmdWebRtc::ended);
        attempt.start();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(janus.creates, 0);
    }
    void authenticatedVideoReachesExistingDecoder()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        QVERIFY(janus.fixtureLoaded());
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        auto made = engine::Gpu::create({.software = true});
        QVERIFY(made);
        auto gpu = std::move(made).value();
        auto scene = gpu->makeScene({});
        auto& stream = scene->add(1, engine::Codec::H264);
        stream.shownAt({1280, 720}, engine::Importance::ShownLarge);
        int frames = 0;
        connect(live.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
            if (picture.h264) {
                ++frames;
                stream.push(
                    {std::as_bytes(std::span(picture.data.constData(), static_cast<std::size_t>(picture.data.size()))),
                     base::steadyNowUs(), picture.keyFrame});
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(frames >= 8, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(stream.stats().decoded >= 5, 10000);
        QCOMPARE(stream.stats().picture.width, 1280);
        QCOMPARE(stream.stats().picture.height, 720);
        QCOMPARE(stream.stats().decoding, engine::Decoding::InSoftware);
        QCOMPARE(live->transport(), u"webrtc"_s);
        QVERIFY(live->fallbackReason().isEmpty());
        QVERIFY(janus.cookieSeen);
        QCOMPARE(kvm.signIns, 1);
        QVERIFY(!janus.protocolError);
        live->close();
    }
    void sharedReceiverFallsBackAndReconnects()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive::Pace pace;
        pace.nativeAgainMs = 250;
        core::KvmdLive source(driver, pace);
        auto first = source.open({});
        auto second = source.open({});
        int h264 = 0;
        int jpeg = 0;
        connect(first.get(), &core::Live::pictureCame, this, [&](const core::Live::Picture& picture) {
            if (picture.h264) {
                ++h264;
            } else {
                ++jpeg;
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(h264 >= 2, 15000);
        QCOMPARE(janus.creates, 1);
        const int beforeJpeg = jpeg;
        const int beforeH264 = h264;
        janus.fall();
        QTRY_VERIFY_WITH_TIMEOUT(jpeg > beforeJpeg, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(janus.creates >= 2 && h264 > beforeH264 + 1, 15000);
        QCOMPARE(second->transport(), u"webrtc"_s);
        first->close();
        QVERIFY(second->isOpen());
        const int connections = janus.connections;
        second->close();
        QTest::qWait(800);
        QCOMPARE(janus.connections, connections);
    }
    void unsupportedCodecUsesJpegWithoutRetryStorm()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        janus.unsupported = true;
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        QTRY_VERIFY_WITH_TIMEOUT(!live->fallbackReason().isEmpty(), 6000);
        QTRY_VERIFY(live->transport() == u"snapshots"_s || live->transport() == u"mjpeg"_s);
        QTest::qWait(500);
        QCOMPARE(janus.creates, 1);
        QCOMPARE(janus.answers, 0);
    }
    void nativeVideoCanStartWhenTheJpegStreamBreaks()
    {
        MadeUpKvm kvm;
        Answered fallen;
        fallen.hangUp = true;
        kvm.instead["/streamer/stream"_ba] = fallen;
        JanusByHand janus(kvm);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        QSignalSpy closed(live.get(), &core::Live::closed);
        QTRY_COMPARE_WITH_TIMEOUT(live->transport(), u"webrtc"_s, 8000);
        QCOMPARE(closed.size(), 0);
    }
    void decoderFailureSelectsAlternateVideo()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdLive source(driver);
        auto live = source.open({});
        QTRY_COMPARE_WITH_TIMEOUT(live->transport(), u"webrtc"_s, 15000);
        live->videoFailed();
        QTRY_VERIFY(live->transport() == u"snapshots"_s || live->transport() == u"mjpeg"_s);
        QVERIFY(live->fallbackReason().contains(u"decoded"_s));
        QTest::qWait(400);
        QCOMPARE(janus.creates, 1);
    }
    void timeoutAndWrongSessionCannotProducePictures()
    {
        for (const bool wrong : {false, true}) {
            MadeUpKvm kvm;
            JanusByHand janus(kvm);
            janus.silent = !wrong;
            janus.wrongSession = wrong;
            core::KvmdDevice driver(kvm.where(), kvm.password);
            core::KvmdWebRtc::Pace pace;
            pace.firstPictureMs = 1200;
            core::KvmdWebRtc receiver(driver, pace);
            QSignalSpy pictures(&receiver, &core::KvmdWebRtc::pictureCame);
            QSignalSpy ended(&receiver, &core::KvmdWebRtc::ended);
            receiver.start();
            QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 5000);
            QCOMPARE(pictures.size(), 0);
            QCOMPARE(ended.first().first().toString(), u"video_timeout"_s);
        }
    }
    void repeatedCloseDuringNegotiationHasNoLatePicture()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        for (int cycle = 0; cycle < 5; ++cycle) {
            auto receiver = std::make_unique<core::KvmdWebRtc>(driver);
            QSignalSpy pictures(receiver.get(), &core::KvmdWebRtc::pictureCame);
            receiver->start();
            QTest::qWait(100);
            receiver->close();
            receiver->close();
            QTest::qWait(100);
            QCOMPARE(pictures.size(), 0);
            receiver.reset();
        }
    }
    void mediaSilenceEndsAndKeepalivesAreAnswered()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdWebRtc::Pace pace;
        pace.pictureMs = 700;
        pace.keepaliveMs = 150;
        core::KvmdWebRtc receiver(driver, pace);
        QSignalSpy pictures(&receiver, &core::KvmdWebRtc::pictureCame);
        QSignalSpy ended(&receiver, &core::KvmdWebRtc::ended);
        receiver.start();
        QTRY_VERIFY_WITH_TIMEOUT(pictures.size() >= 2, 10000);
        QTRY_VERIFY(janus.keepalives >= 1);
        janus.sendPictures = false;
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 5000);
        QCOMPARE(ended.first().first().toString(), u"video_timeout"_s);
    }
    void rejectedVideoSessionRefreshesOnlyOnceWithinTheHold()
    {
        MadeUpKvm kvm;
        kvm.stand->handsLinesOver = false;
        kvm.instead["/janus/ws"_ba] = refusal(401, u"Unauthorized"_s);
        core::KvmdDevice driver(kvm.where(), kvm.password);
        for (int attempt = 0; attempt < 4; ++attempt) {
            core::KvmdWebRtc receiver(driver);
            QSignalSpy ended(&receiver, &core::KvmdWebRtc::ended);
            receiver.start();
            QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 5000);
        }
        QCOMPARE(kvm.signIns, 2);
    }
    void signalingUsesTheKnownCertificate()
    {
        const Tls tls{QStringLiteral(QKVM_TESTDATA "/tls/first.crt"), QStringLiteral(QKVM_TESTDATA "/tls/first.key")};
        MadeUpKvm kvm(tls);
        JanusByHand janus(kvm);
        auto where = kvm.where();
        where.knownByCertificate = true;
        where.certificate = StandIn::certificateIn(tls.certificateFile).digest(QCryptographicHash::Sha256);
        core::KvmdDevice driver(where, kvm.password);
        core::KvmdWebRtc receiver(driver);
        QSignalSpy pictures(&receiver, &core::KvmdWebRtc::pictureCame);
        receiver.start();
        QTRY_VERIFY_WITH_TIMEOUT(pictures.size() > 0, 15000);
        QVERIFY(janus.cookieSeen);
        receiver.close();
        const int before = janus.connections;
        core::DeviceLine::Where wrong;
        wrong.address = kvm.address();
        wrong.asking = "/janus/ws"_ba;
        wrong.cookie = "auth_token=test-token"_ba;
        wrong.knownByCertificate = true;
        wrong.certificate =
            StandIn::certificateIn(QStringLiteral(QKVM_TESTDATA "/tls/second.crt")).digest(QCryptographicHash::Sha256);
        core::DeviceLine refused(wrong, {});
        refused.receiveText("janus-protocol"_ba);
        QSignalSpy ended(&refused, &core::DeviceLine::ended);
        refused.open();
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10000);
        QCOMPARE(ended.first().first().value<core::DeviceLine::End>(), core::DeviceLine::End::CertificateChanged);
        QCOMPARE(janus.connections, before);
    }
    void malformedSignalingEndsAttempt()
    {
        MadeUpKvm kvm;
        JanusByHand janus(kvm);
        janus.sendPictures = false;
        core::KvmdDevice driver(kvm.where(), kvm.password);
        core::KvmdWebRtc receiver(driver);
        QSignalSpy ended(&receiver, &core::KvmdWebRtc::ended);
        receiver.start();
        QTRY_VERIFY_WITH_TIMEOUT(janus.answers > 0, 8000);
        janus.raw(u"{broken"_s);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(ended.first().first().toString(), u"video_protocol"_s);
    }
};
QTEST_GUILESS_MAIN(TestNativeVideo)
#include "test_native_video.moc"
