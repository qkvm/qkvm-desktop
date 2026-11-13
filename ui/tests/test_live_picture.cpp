#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <QElapsedTimer>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <qkvm/base/clock.h>
#include <qkvm/core/live.h>
#include <qkvm/core/live_link.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/wall_source.h>
#include <qkvm/engine/scene.h>

#include "live_picture.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::jpeg;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::StandIn;
using ui::LivePicture;

namespace {

// A stream that decodes nothing: it notes what was handed in and how it is shown. It says that
// it has made a picture of everything handed in, unless a test has it make none; and it can be
// made to wish for a key frame.
class NotedStream : public engine::Stream {
public:
    explicit NotedStream(engine::Codec codec, bool makesPictures) : codec(codec), makesPictures(makesPictures) {}

    void push(const engine::Packet& packet) override
    {
        QByteArray copy(static_cast<qsizetype>(packet.data.size()), '\0');
        std::memcpy(copy.data(), packet.data.data(), packet.data.size());
        pushed.append(copy);
        keyFrames.append(packet.keyFrame);
        arrivals.append(packet.arrivedUs);
    }
    void shownAt(engine::SizePx onScreen, engine::Importance importance) override
    {
        size = onScreen;
        matters = importance;
    }
    bool takeKeyFrameWish() override { return std::exchange(wishes, false); }
    engine::Stats stats() const override
    {
        engine::Stats made = reported;
        made.decoded = makesPictures ? static_cast<std::uint64_t>(pushed.size()) : 0;
        return made;
    }

    engine::Stats reported;
    engine::Codec codec;
    bool makesPictures;
    QList<QByteArray> pushed;
    QList<bool> keyFrames;
    // When each was said to have arrived, on this PC's steady clock.
    QList<std::int64_t> arrivals;
    engine::SizePx size;
    engine::Importance matters = engine::Importance::Preview;
    bool wishes = false;
};

// A scene that draws nothing: it notes which streams it was given and which were taken away.
class NotedScene : public engine::Scene {
public:
    engine::Stream& add(engine::StreamId id, engine::Codec codec) override
    {
        added.append(id);
        streams[id] = std::make_unique<NotedStream>(codec, makesPictures);
        return *streams[id];
    }
    void remove(engine::StreamId id) override
    {
        removed.append(id);
        streams.erase(id);
    }
    void place(std::span<const engine::Placed> /*placed*/) override {}
    bool draw(const engine::Target& /*target*/) override { return false; }
    void drawUnder(engine::SizePx /*size*/) override {}
    void shown(std::int64_t /*atUs*/) override {}

    NotedStream* stream(engine::StreamId id) { return streams.contains(id) ? streams[id].get() : nullptr; }

    // Whether the streams that are added from now on make pictures of what they are handed.
    bool makesPictures = true;
    QList<engine::StreamId> added;
    QList<engine::StreamId> removed;
    std::map<engine::StreamId, std::unique_ptr<NotedStream>> streams;
};

const QByteArray aPass = R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})";

Answered pass(const Asked& /*asked*/)
{
    return {200, aPass};
}

Answered fallsOver(const Asked& /*asked*/)
{
    Answered none;
    none.hangUp = true;
    return none;
}

// A server that says what it is, as a QKVM does, and falls over at everything else: a machine
// that is asked for live, here. And what it was asked for a machine, in the order it came.
Answered fallsOverAtAMachine(const Asked& asked)
{
    return asked.path == "/healthz" ? Answered{200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"]})"}
                                    : fallsOver(asked);
}
QList<QByteArray> askedForAMachine(const StandIn& server)
{
    QList<QByteArray> machines;
    for (const Asked& one : server.asked) {
        if (one.path != "/healthz") {
            machines.append(one.path);
        }
    }
    return machines;
}

// A server, a client that is signed in to it, a scene, and the live picture of one window. The
// waits are short ones: a machine is asked for 10 ms after it is shown, again after 100 ms and
// never later than 800 ms, the engine is asked every 5 ms, and a snapshot is given 20 ms.
struct Window {
    explicit Window(std::function<Answered(const Asked&)> answer = pass, LivePicture::Pace pace = {10, 100, 800, 5, 20})
        : server(std::move(answer))
    {
        static_cast<void>(client.setAddress(server.address()));
        client.setSession("s1");
        live = std::make_unique<LivePicture>(
            LivePicture::Around{[this]() -> engine::Scene* { return hasScene ? &scene : nullptr; },
                                [this] { return ++lastNumber; },
                                [this](const QString& key) { return ui::Reached{place, key, {}}; },
                                {}},
            pace);
        changed = std::make_unique<QSignalSpy>(live.get(), &LivePicture::changed);
    }

    // The machine "a" is shown, its line is open and a first picture has come and is drawn:
    // stream 41. False where that did not come about.
    bool hasALive()
    {
        live->show(u"a"_s, 800, true);
        if (!QTest::qWaitFor([this] { return server.live.isConnected(); })) {
            return false;
        }
        server.live.send(1, true, 1, 0, "key");
        return QTest::qWaitFor([this] { return live->stream() == 41; });
    }

    StandIn server;
    core::ServerClient client;
    // The place every key reaches: the server, until a test says another.
    core::WallSource* place = &client;
    NotedScene scene;
    bool hasScene = true;
    quint32 lastNumber = 40;
    std::unique_ptr<LivePicture> live;
    std::unique_ptr<QSignalSpy> changed;
};

// A machine live of which nothing ever comes: for a place that hands one over and has it end.
class SilentLive : public core::Live {
public:
    bool isOpen() const override { return false; }
    void setNeed(Need /*need*/) override {}
    void askForAKeyFrame() override {}
    void askControl(bool /*on*/) override {}
    bool hasControl() const override { return false; }
    void key(int /*usage*/, bool /*down*/) override {}
    void pointerAt(int /*x*/, int /*y*/) override {}
    void pointerBy(int /*dx*/, int /*dy*/) override {}
    void button(int /*number*/, bool /*down*/) override {}
    void wheel(int /*dx*/, int /*dy*/) override {}
    void releaseAll() override {}
    void close() override {}
};

// A place a wall's machines come from that has no machine live, and says so in a word a test
// names: by refusing, or by handing over a line that ends in that word a turn later. It counts
// how often it was asked. Nothing else is ever asked of it here.
class PlaceThatSays : public core::WallSource {
public:
    std::string code;
    bool byALine = false;
    int asked = 0;

    void openLive(const QString& /*machine*/, core::Live::Need /*need*/,
                  std::function<void(base::Result<std::unique_ptr<core::Live>>)> done) override
    {
        ++asked;
        QTimer::singleShot(0, &m_later, [this, done = std::move(done)] {
            if (!byALine) {
                done(base::Error{code, "No"});
                return;
            }
            auto line = std::make_unique<SilentLive>();
            SilentLive* handed = line.get();
            done(std::unique_ptr<core::Live>(std::move(line)));
            QTimer::singleShot(0, handed,
                               [handed, said = QString::fromStdString(code)] { emit handed->closed(said, u"No"_s); });
        });
    }
    void askWall(std::function<void(base::Result<core::Wall>)> /*done*/) override {}
    void askStatus(std::function<void(base::Result<QHash<QString, core::Status>>)> /*done*/) override {}
    void askSnapshot(const QString& /*machine*/, int /*width*/, double /*mayBeOldSeconds*/,
                     std::function<void(base::Result<core::Snapshot>)> /*done*/) override
    {}
    void act(const QString& /*machine*/, const core::Action& /*what*/,
             std::function<void(base::Result<core::Done>)> /*done*/) override
    {}
    bool mayHaveBeenDone(const QString& /*machine*/, const base::Error& /*error*/) const override { return false; }
    void askSavedActions(std::function<void(base::Result<QList<core::SavedAction>>)> /*done*/) override {}
    void askWakeTargets(const QString& /*machine*/,
                        std::function<void(base::Result<QList<core::WakeTarget>>)> /*done*/) override
    {}
    void askConsole(const QString& /*machine*/, std::function<void(base::Result<QUrl>)> /*done*/) override {}
    void arrange(const QStringList& /*machines*/, std::function<void(base::Result<QStringList>)> /*done*/) override {}

private:
    QObject m_later; // what is answered later ends with this
};

class EndsWhileClosed : public SilentLive {
public:
    bool inDestructor = false;
    ~EndsWhileClosed() override
    {
        if (inDestructor)
            emit closed(u"unreachable"_s, u"late"_s);
    }
    void close() override
    {
        if (!inDestructor)
            emit closed(u"unreachable"_s, u"late"_s);
    }
};
class PlaceThatHandsOver : public PlaceThatSays {
public:
    bool destructorRow = false;
    void openLive(const QString&, core::Live::Need,
                  std::function<void(base::Result<std::unique_ptr<core::Live>>)> done) override
    {
        ++asked;
        auto line = std::make_unique<EndsWhileClosed>();
        line->inDestructor = destructorRow;
        done(std::unique_ptr<core::Live>(std::move(line)));
    }
};

// Where a KVM has its stream of pictures, and one picture of it as ustreamer sends it.
QByteArray streamOfAKvm()
{
    return "/streamer/stream"_ba;
}
QByteArray partOf(const QByteArray& picture)
{
    return "Content-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(picture.size()) + "\r\n\r\n" + picture +
           "\r\n--boundarydonotcross\r\n";
}

// This PC's own wall with one made-up KVM on it, which streams `first` to whoever asks for its
// stream; what the wall remembers in a folder that goes with the test, its passwords in memory.
struct OwnWall {
    explicit OwnWall(const QByteArray& first)
    {
        Answered stream(200, "--boundarydonotcross\r\n" + partOf(first),
                        "multipart/x-mixed-replace;boundary=boundarydonotcross"_ba);
        stream.streams = true;
        kvm.instead.insert(streamOfAKvm(), stream);
        core::OwnKvm own;
        own.address = kvm.stand->address();
        own.name = u"Build server"_s;
        id = wall.add(own, u"secret"_s).value();
    }

    static core::KvmdDevice::Pace quick()
    {
        core::KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.holdMs = 400;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        return pace;
    }

    MadeUpKvm kvm;
    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    MemorySecrets secrets;
    core::LocalWall wall{settings, secrets, quick()};
    QString id;
};

// A wall's machines as a test has them: the machine "a" alone, which is on the wall, and whose key
// reaches `place` only while `reaches` is set.
class OneMachine final : public ui::WallMachines {
public:
    explicit OneMachine(core::WallSource& place) : m_place(place)
    {
        core::Machine a;
        a.id = u"a"_s;
        a.name = u"a"_s;
        a.hasPicture = true;
        m_machines.append(a);
    }

    const QList<core::Machine>& machines() const override { return m_machines; }
    const QHash<QString, core::Status>& status() const override { return m_status; }
    ui::Reached reach(const QString& key) const override
    {
        return reaches && key == u"a"_s ? ui::Reached{&m_place, key, {}} : ui::Reached{};
    }
    void lookAgainSoon(const QString& /*key*/) override {}

    bool reaches = false;

private:
    core::WallSource& m_place;
    QList<core::Machine> m_machines;
    QHash<QString, core::Status> m_status;
};

} // namespace

// One window's live picture: the machine shown large, had live for as long as it is.
class NativeLiveByHand : public SilentLive {
public:
    bool isOpen() const override { return true; }
    QString transport() const override { return failed == 0 ? path : u"snapshots"_s; }
    QString fallbackReason() const override { return failed == 0 ? QString{} : u"Decoder did not produce a picture"_s; }
    void videoFailed() override
    {
        ++failed;
        emit transportChanged();
    }
    quint64 picturesLetGo() const override { return letGo; }
    QString path = u"webrtc"_s;
    int failed = 0;
    quint64 letGo = 0;
};
class NativePlaceByHand : public PlaceThatSays {
public:
    void openLive(const QString&, core::Live::Need,
                  std::function<void(base::Result<std::unique_ptr<core::Live>>)> done) override
    {
        auto made = std::make_unique<NativeLiveByHand>();
        line = made.get();
        done(std::unique_ptr<core::Live>(std::move(made)));
    }
    QPointer<NativeLiveByHand> line;
};

class TestLivePicture : public QObject {
    Q_OBJECT

private slots:
    void aLineThatEndsWhileItIsClosedIsLetGoOfSafely_data()
    {
        QTest::addColumn<bool>("destructor");
        QTest::newRow("live turned off (endLine)") << false;
        QTest::newRow("window thrown away (~LivePicture)") << true;
    }
    void aLineThatEndsWhileItIsClosedIsLetGoOfSafely()
    {
        QFETCH(bool, destructor);
        Window window;
        PlaceThatHandsOver place;
        place.destructorRow = destructor;
        window.place = &place;
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(place.asked, 1);
        if (destructor)
            window.live.reset();
        else
            window.live->show(u"a"_s, 800, false);
        QTest::qWait(50);
        QVERIFY(true); // reaching here is the pass
    }

    void aNativeTransportSwitchDoesNotJudgeThePreviousJpegDecoder()
    {
        NativePlaceByHand place;
        Window window;
        window.place = &place;
        window.scene.makesPictures = false;
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(place.line != nullptr);
        place.line->path = u"mjpeg"_s;
        core::Live::Picture packet;
        packet.keyFrame = true;
        packet.data = "undecodable-jpeg"_ba;
        emit place.line->pictureCame(packet);
        QTest::qWait(5200);
        emit place.line->pictureCame(packet);
        place.line->path = u"webrtc"_s;
        emit place.line->transportChanged();
        QCOMPARE(place.line->failed, 0);
    }
    void aNativeDecoderThatStopsMakingProgressSelectsFallback()
    {
        NativePlaceByHand place;
        Window window;
        window.place = &place;
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(place.line != nullptr);
        core::Live::Picture packet;
        packet.h264 = true;
        packet.keyFrame = true;
        packet.data = "video"_ba;
        emit place.line->pictureCame(packet);
        QTRY_VERIFY(window.live->stream() != 0);
        auto* decoder = window.scene.stream(window.live->stream());
        QVERIFY(decoder != nullptr);
        decoder->makesPictures = false;
        QTimer incoming;
        connect(&incoming, &QTimer::timeout, this, [&] { emit place.line->pictureCame(packet); });
        incoming.start(50);
        QTRY_COMPARE_WITH_TIMEOUT(place.line->failed, 1, 8000);
        incoming.stop();
        QCOMPARE(window.live->diagnostics().value(u"transport"_s).toString(), u"snapshots"_s);
    }
    void aNativeDecoderThatProducesNothingSelectsFallback()
    {
        NativePlaceByHand place;
        Window window;
        window.place = &place;
        window.scene.makesPictures = false;
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(place.line != nullptr);
        emit place.line->videoChanged({true, 1280, 720});
        core::Live::Picture packet;
        packet.h264 = true;
        packet.keyFrame = true;
        packet.data = "not-video"_ba;
        emit place.line->pictureCame(packet);
        QTRY_COMPARE_WITH_TIMEOUT(place.line->failed, 1, 5000);
        QCOMPARE(window.live->diagnostics().value(u"transport"_s).toString(), u"snapshots"_s);
        QVERIFY(!window.live->diagnostics().value(u"fallbackReason"_s).toString().isEmpty());
    }

    // M6 increment 2a (design 3.2, L7): a picture that its giver stamped as it became whole on this PC
    // (a WebRTC receiver, on its library's thread) is handed to the engine with that stamp, so that the
    // steps between arrivals are the network's and not those of the turn of the loop that drained the
    // mailbox. Two stamped 120 ms apart and handed over in one turn arrive 120 ms apart. One that
    // nobody stamped (a server's line, a KVM's JPEGs) is stamped as it comes, as before. (Before: every
    // picture was stamped as it came, so the two were the same microsecond in the engine.)
    void theEngineIsHandedTheArrivalStamp()
    {
        NativePlaceByHand place;
        Window window;
        window.place = &place;
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(place.line != nullptr);
        core::Live::Picture packet;
        packet.h264 = true;
        packet.keyFrame = true;
        packet.data = "video"_ba;
        const std::int64_t first = base::steadyNowUs() - 500'000;
        packet.arrivedUs = first;
        emit place.line->pictureCame(packet);
        packet.keyFrame = false;
        packet.arrivedUs = first + 120'000;
        emit place.line->pictureCame(packet);
        packet.arrivedUs = 0;
        const std::int64_t before = base::steadyNowUs();
        emit place.line->pictureCame(packet);
        const std::int64_t after = base::steadyNowUs();
        NotedStream* stream = window.scene.stream(window.live->stream() != 0 ? window.live->stream() : 41);
        QVERIFY(stream != nullptr);
        QCOMPARE(stream->arrivals.size(), 3);
        QCOMPARE(stream->arrivals[0], first);
        QCOMPARE(stream->arrivals[1] - stream->arrivals[0], 120'000);
        QVERIFY2(stream->arrivals[2] >= before && stream->arrivals[2] <= after,
                 qPrintable(u"stamped %1 us from the turn"_s.arg(stream->arrivals[2] - before)));
    }

    // M6 2c, after its reading (L2): a still screen's JPEGs are let go by the engine as the same (the
    // picture on the screen is that one already), and they count in the rate shown: Video details says
    // the pace they come at, not 0.0 a second.
    void theShownRateCountsTheStillsLetGoAsTheSame()
    {
        NativePlaceByHand place;
        Window window(pass, {10, 100, 800, 5, 20, 100});
        window.place = &place;
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(place.line != nullptr);
        place.line->path = u"mjpeg"_s;
        core::Live::Picture packet;
        packet.keyFrame = true;
        packet.data = "a JPEG"_ba;
        emit place.line->pictureCame(packet);
        QTRY_VERIFY(window.live->stream() != 0);
        NotedStream* stream = window.scene.stream(window.live->stream());
        QVERIFY(stream != nullptr);
        QTimer same;
        connect(&same, &QTimer::timeout, this, [&] { ++stream->reported.stillsSame; });
        same.start(20);
        QTRY_VERIFY(window.live->diagnostics().value(u"shownRateKnown"_s).toBool() &&
                    window.live->diagnostics().value(u"shownFps"_s).toDouble() > 10);
        same.stop();
    }

    // M6 2b, after its reading: pictures the live's mailbox let go for want of room are dropped behind
    // as much as those the engine let go, and Video details (and video.jsonl's samples) say them so.
    void whatTheMailboxLetGoIsCountedAsDroppedBehind()
    {
        NativePlaceByHand place;
        Window window(pass, {10, 100, 800, 5, 20, 50});
        window.place = &place;
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(place.line != nullptr);
        core::Live::Picture packet;
        packet.h264 = true;
        packet.keyFrame = true;
        packet.data = "video"_ba;
        emit place.line->pictureCame(packet);
        QTRY_VERIFY(window.live->stream() != 0);
        NotedStream* stream = window.scene.stream(window.live->stream());
        QVERIFY(stream != nullptr);
        stream->reported.droppedBehind = 2;
        place.line->letGo = 5;
        QTRY_COMPARE(window.live->diagnostics().value(u"dropped"_s).toULongLong(), 7ULL);
    }

    // 2c, the second reading (L6): what the lines of one machine let go is counted from the window's
    // own start and never goes down: a line that ends and the next one opened for the same machine add
    // up (the engine's own count is the new stream's).
    void whatTheLinesLetGoAddsUpWhileTheMachineStaysShown()
    {
        NativePlaceByHand place;
        Window window(pass, {10, 50, 800, 5, 20, 50});
        window.place = &place;
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(place.line != nullptr);
        core::Live::Picture packet;
        packet.h264 = true;
        packet.keyFrame = true;
        packet.data = "video"_ba;
        emit place.line->pictureCame(packet);
        QTRY_VERIFY(window.live->stream() != 0);
        place.line->letGo = 5;
        QTRY_COMPARE(window.live->diagnostics().value(u"dropped"_s).toULongLong(), 5ULL);
        QPointer<NativeLiveByHand> first = place.line;
        emit first->closed(u"closed"_s, u"It went"_s);
        QTRY_VERIFY(place.line != nullptr && place.line != first);
        QCOMPARE(window.live->diagnostics().value(u"dropped"_s).toULongLong(), 5ULL);
        place.line->letGo = 2;
        emit place.line->pictureCame(packet);
        QTRY_COMPARE(window.live->diagnostics().value(u"dropped"_s).toULongLong(), 7ULL);
        // Another machine: counted from nought.
        window.live->show(u"b"_s, 800, true);
        QTRY_COMPARE(window.live->diagnostics().value(u"dropped"_s).toULongLong(), 0ULL);
    }

    void theMachineThatIsShownLargeIsHadLive()
    {
        Window window;
        window.live->show(u"a b"_s, 1280, true);
        // Asked for a pass, as everything is asked; and the line opened with it.
        QTRY_COMPARE(window.server.live.lines, 1);
        QCOMPARE(window.server.asked.size(), 1);
        QCOMPARE(window.server.asked[0].method, QByteArray("POST"));
        QCOMPARE(window.server.asked[0].path, QByteArray("/api/v1/devices/a%20b/live"));
        QCOMPARE(window.server.live.path, QByteArray("/api/v1/devices/a%20b/live"));
        QTRY_COMPARE(window.server.live.said.size(), 1);
        const QJsonObject hello = window.server.live.said.at(0);
        QCOMPARE(hello[u"ticket"_s].toString(), u"p4ss-for-one-line_0123456789"_s);
        QCOMPARE(hello[u"max_w"_s].toInt(), 1280);
        QCOMPARE(hello[u"max_fps"_s].toInt(), 30);
        // A line is no picture yet: the snapshots are what there is.
        QCOMPARE(window.live->stream(), 0U);
        QVERIFY(window.live->wantsSnapshots());
        QCOMPARE(window.changed->count(), 0);
        QVERIFY(window.scene.added.isEmpty());

        // The first picture: from now on there is a stream to draw, of the kind the pictures are,
        // and the snapshots are not needed.
        window.server.live.send(1, true, 1, 0, "a key frame");
        QTRY_COMPARE(window.changed->count(), 1);
        QCOMPARE(window.live->stream(), 41U);
        QVERIFY(!window.live->wantsSnapshots());
        QCOMPARE(window.scene.added, (QList<engine::StreamId>{41}));
        NotedStream* stream = window.scene.stream(41);
        QVERIFY(stream != nullptr);
        QCOMPARE(stream->codec, engine::Codec::H264);
        QCOMPARE(stream->matters, engine::Importance::ShownLarge);
        QCOMPARE(stream->size, (engine::SizePx{1280, 720}));
        QCOMPARE(stream->pushed, (QList<QByteArray>{"a key frame"}));
        QCOMPARE(stream->keyFrames, (QList<bool>{true}));
        // The next go into the same stream, and nothing is said again.
        window.server.live.send(1, false, 2, 0, "what builds on it");
        QTRY_COMPARE(stream->pushed.size(), 2);
        QCOMPARE(stream->keyFrames, (QList<bool>{true, false}));
        QTest::qWait(50);
        QCOMPARE(window.changed->count(), 1);
        QCOMPARE(window.server.asked.size(), 1);
    }

    void aStreamIsDrawnOnlyOnceTheEngineHasMadeAPictureOfIt()
    {
        // (Until then it would be drawn black: the snapshot stays up meanwhile.)
        Window window;
        window.scene.makesPictures = false;
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.live.lines, 1);
        QTRY_VERIFY(window.server.live.isConnected());
        window.server.live.send(1, true, 1, 0, "key");
        QTRY_COMPARE(window.scene.added.size(), 1);
        QTest::qWait(100);
        QCOMPARE(window.live->stream(), 0U);
        QVERIFY(window.live->wantsSnapshots());
        QCOMPARE(window.changed->count(), 0);
        // The engine has made one, and no further picture has to come for that to be seen.
        window.scene.stream(41)->makesPictures = true;
        QTRY_COMPARE(window.live->stream(), 41U);
        QVERIFY(!window.live->wantsSnapshots());
        QCOMPARE(window.changed->count(), 1);
    }

    void anEngineThatNeverMakesAPictureLeavesTheSnapshots()
    {
        // (What the server sends is nothing the engine can read: larger than it takes, or rubbish.)
        Window window;
        window.scene.makesPictures = false;
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.live.lines, 1);
        QTRY_VERIFY(window.server.live.isConnected());
        for (quint32 counter = 1; counter <= 5; ++counter) {
            window.server.live.send(2, true, counter, 0, "no JPEG");
        }
        QTRY_VERIFY(window.scene.stream(41) != nullptr);
        QTRY_COMPARE(window.scene.stream(41)->pushed.size(), 5);
        QTest::qWait(150);
        QCOMPARE(window.live->stream(), 0U);
        QVERIFY(window.live->wantsSnapshots());
        QCOMPARE(window.changed->count(), 0);
    }

    void picturesOfAnotherKindGoIntoAStreamOfThatKind()
    {
        Window window;
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.live.lines, 1);
        QTRY_VERIFY(window.server.live.isConnected());
        window.server.live.send(2, false, 1, 0, "a JPEG");
        QTRY_COMPARE(window.live->stream(), 41U);
        QCOMPARE(window.scene.stream(41)->codec, engine::Codec::Still);
        QCOMPARE(window.scene.stream(41)->keyFrames, (QList<bool>{true}));
        // The server goes over to H.264: another stream. Until the engine has made a picture of
        // it the last JPEG stays up, and then it is gone.
        window.scene.makesPictures = false;
        window.server.live.send(1, true, 2, 0, "a key frame");
        QTRY_COMPARE(window.scene.added, (QList<engine::StreamId>{41, 42}));
        QTest::qWait(60);
        QCOMPARE(window.live->stream(), 41U);
        QVERIFY(window.scene.removed.isEmpty());
        window.scene.stream(42)->makesPictures = true;
        QTRY_COMPARE(window.live->stream(), 42U);
        QCOMPARE(window.changed->count(), 2);
        QCOMPARE(window.scene.removed, (QList<engine::StreamId>{41}));
        QCOMPARE(window.scene.stream(42)->codec, engine::Codec::H264);
        QCOMPARE(window.scene.stream(42)->pushed, (QList<QByteArray>{"a key frame"}));
    }

    void aServersScreenGoesIntoAStreamOfItsOwnKind()
    {
        // (M7 3d.) A server's screen comes as what changed of it (core::Live::Picture::changes, ServerLive): into a
        // stream of that kind (engine::Codec::Rgb), which applies each to the screen as it stands, and the diagnostics
        // say so; a JPEG after it goes into a still stream, as any picture of another kind.
        NativePlaceByHand place;
        Window window;
        window.place = &place;
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(place.line != nullptr);
        core::Live::Video video;
        video.changes = true;
        video.width = 64;
        video.height = 32;
        emit place.line->videoChanged(video);
        core::Live::Picture screen;
        screen.changes = true;
        screen.keyFrame = true;
        screen.data = "a whole screen"_ba;
        emit place.line->pictureCame(screen);
        QTRY_VERIFY(window.live->stream() != 0);
        const engine::StreamId first = window.live->stream();
        QCOMPARE(window.scene.stream(first)->codec, engine::Codec::Rgb);
        QCOMPARE(window.scene.stream(first)->pushed, (QList<QByteArray>{"a whole screen"}));
        QTRY_COMPARE(window.live->diagnostics()[u"codec"_s].toString(), u"rgb"_s);
        // What changed after it goes into the same stream.
        screen.keyFrame = false;
        screen.data = "what changed"_ba;
        emit place.line->pictureCame(screen);
        QCOMPARE(window.scene.added.size(), qsizetype{1});
        QCOMPARE(window.scene.stream(first)->pushed, (QList<QByteArray>{"a whole screen", "what changed"}));
        core::Live::Picture jpeg;
        jpeg.keyFrame = true;
        jpeg.data = "a JPEG"_ba;
        emit place.line->pictureCame(jpeg);
        QTRY_COMPARE(window.scene.added.size(), qsizetype{2});
        QCOMPARE(window.scene.stream(window.scene.added.back())->codec, engine::Codec::Still);
    }

    void anotherWidthIsSaidToTheServerAndToTheEngine()
    {
        Window window;
        QVERIFY(window.hasALive());
        window.live->show(u"a"_s, 1920, true);
        QTRY_COMPARE(window.server.live.saidOf(u"shape"_s).size(), 1);
        QCOMPARE(window.server.live.saidOf(u"shape"_s).at(0)[u"max_w"_s].toInt(), 1920);
        QCOMPARE(window.scene.stream(41)->size, (engine::SizePx{1920, 1080}));
        QCOMPARE(window.scene.stream(41)->matters, engine::Importance::ShownLarge);
        // The same again is nothing new: no word, and no other line.
        window.live->show(u"a"_s, 1920, true);
        QTest::qWait(150);
        QCOMPARE(window.server.live.saidOf(u"shape"_s).size(), 1);
        QCOMPARE(window.server.live.lines, 1);
        QCOMPARE(window.server.asked.size(), 1);
    }

    void whoeverDecodesAsksForAPictureToBeginAt()
    {
        // (The numbers are looked at once a minute here: what is asked for comes with the picture.)
        Window window(pass, {10, 100, 800, 5, 20, 60'000});
        QVERIFY(window.hasALive());
        QTest::qWait(100);
        QCOMPARE(window.server.live.saidOf(u"key_frame"_s).size(), 0);
        // The decoder threw pictures away: it says so when the next one is handed in.
        window.scene.stream(41)->wishes = true;
        window.server.live.send(1, false, 2, 0, "second");
        QTRY_COMPARE(window.server.live.saidOf(u"key_frame"_s).size(), 1);
        window.server.live.send(1, false, 3, 0, "third");
        QTRY_COMPARE(window.scene.stream(41)->pushed.size(), 3);
        QTest::qWait(100);
        QCOMPARE(window.server.live.saidOf(u"key_frame"_s).size(), 1);
    }

    // A wish that comes up after the last picture was handed in, and no picture after it (a screen
    // that does not change sends none): it is passed on all the same, at the next look at the
    // numbers.
    void aWishWithNoPictureAfterItIsPassedOnAllTheSame()
    {
        Window window(pass, {10, 100, 800, 5, 20, 200});
        QVERIFY(window.hasALive());
        QTRY_COMPARE(window.scene.stream(41)->pushed.size(), 1);
        window.scene.stream(41)->wishes = true;
        QTRY_COMPARE(window.server.live.saidOf(u"key_frame"_s).size(), 1);
        QTest::qWait(500);
        QCOMPARE(window.server.live.saidOf(u"key_frame"_s).size(), 1);
        QCOMPARE(window.scene.stream(41)->pushed.size(), 1);
    }

    // The rate on the screen is not known at the first look at the numbers, where the arriving
    // rate is: a while with one stream has to pass first.
    void theRateOnTheScreenIsKnownAfterAWhileWithOneStream()
    {
        Window window(pass, {10, 100, 800, 5, 20, 1000});
        QVERIFY(window.hasALive());
        QTRY_VERIFY(window.live->diagnostics()[u"rateKnown"_s].toBool());
        QVERIFY(!window.live->diagnostics()[u"shownRateKnown"_s].toBool());
        QTRY_VERIFY(window.live->diagnostics()[u"shownRateKnown"_s].toBool());
    }

    void aWindowWithoutAnEngineHasTheLineAllTheSame()
    {
        // (As the tests' windows are, and a window whose engine has ended.)
        Window window;
        window.hasScene = false;
        QVERIFY(window.hasALive());
        window.server.live.send(1, false, 2, 0, "second");
        QTest::qWait(150);
        // One number for the line's pictures, not one for each.
        QCOMPARE(window.live->stream(), 41U);
        QCOMPARE(window.changed->count(), 1);
        QVERIFY(window.scene.added.isEmpty());
        window.live->show({}, 0, false);
        QVERIFY(window.scene.removed.isEmpty());
    }

    void anEngineThatEndsWhileTheLineIsOpenIsLeftAlone()
    {
        Window window;
        QVERIFY(window.hasALive());
        NotedStream* stream = window.scene.stream(41);
        QCOMPARE(stream->pushed.size(), 1);
        // The window's engine ends, and took its streams with it: nothing is handed into one,
        // none is told a size, and none is taken out of it.
        window.hasScene = false;
        window.server.live.send(1, false, 2, 0, "second");
        window.server.live.send(1, false, 3, 0, "third");
        QTest::qWait(200);
        QCOMPARE(stream->pushed.size(), 1);
        window.live->show(u"a"_s, 1600, true);
        QCOMPARE(stream->size, (engine::SizePx{800, 450}));
        window.live->show({}, 0, false);
        QVERIFY(window.scene.removed.isEmpty());
    }

    void noMachineShownLargeEndsTheLineAndLeavesNothingUp()
    {
        Window window;
        QVERIFY(window.hasALive());
        window.live->show({}, 0, false);
        // There and then, for whoever asks next; and not said, since whoever said this knows.
        QCOMPARE(window.live->stream(), 0U);
        QCOMPARE(window.scene.removed, (QList<engine::StreamId>{41}));
        QCOMPARE(window.changed->count(), 1);
        QTRY_VERIFY(!window.server.live.isConnected());
        // And it is not come back to.
        QTest::qWait(400);
        QCOMPARE(window.server.asked.size(), 1);
        QCOMPARE(window.server.live.lines, 1);
        QCOMPARE(window.changed->count(), 1);
    }

    void aMachineThatMayNotBeHadLiveJustNowKeepsItsLastPicture()
    {
        // (Nobody can see the window, or the server does not answer.)
        Window window;
        QVERIFY(window.hasALive());
        window.live->show(u"a"_s, 800, false);
        QTRY_VERIFY(!window.server.live.isConnected());
        // The line is gone, and its last picture is still what is drawn; snapshots are wanted.
        QCOMPARE(window.live->stream(), 41U);
        QVERIFY(window.live->wantsSnapshots());
        QVERIFY(window.scene.removed.isEmpty());
        QTest::qWait(400);
        QCOMPARE(window.server.asked.size(), 1);
        // A snapshot comes: given a moment to be drawn, it is what there is, and that is said.
        window.live->snapshotCame();
        QCOMPARE(window.live->stream(), 41U);
        QTRY_COMPARE(window.live->stream(), 0U);
        QCOMPARE(window.changed->count(), 2);
        QCOMPARE(window.scene.removed, (QList<engine::StreamId>{41}));
        // A snapshot more is nothing more.
        window.live->snapshotCame();
        QTest::qWait(80);
        QCOMPARE(window.changed->count(), 2);

        // It may be had live again: another line, and its pictures in another stream.
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.live.lines, 2);
        QTRY_VERIFY(window.server.live.isConnected());
        window.server.live.send(1, true, 1, 0, "key again");
        QTRY_COMPARE(window.live->stream(), 42U);
        QVERIFY(!window.live->wantsSnapshots());
    }

    void theLastPictureOfALineStaysUntilTheNextLineHasOne()
    {
        // (A snapshot is given 300 ms here: longer than the next line's first picture takes.)
        Window window(pass, {10, 100, 800, 5, 300});
        QVERIFY(window.hasALive());
        window.live->show(u"a"_s, 800, false);
        QTRY_VERIFY(!window.server.live.isConnected());
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.live.lines, 2);
        QTRY_VERIFY(window.server.live.isConnected());
        QCOMPARE(window.live->stream(), 41U);
        // A snapshot was on its way to making it go; the line's first picture is there sooner,
        // and nothing is said twice.
        window.live->snapshotCame();
        window.server.live.send(1, true, 1, 0, "key again");
        QTRY_COMPARE(window.live->stream(), 42U);
        QCOMPARE(window.scene.removed, (QList<engine::StreamId>{41}));
        QTest::qWait(500);
        QCOMPARE(window.live->stream(), 42U);
        QCOMPARE(window.changed->count(), 2);
    }

    void anotherMachineIsAnotherLineAndNothingOfTheOneBeforeStays()
    {
        Window window;
        QVERIFY(window.hasALive());
        window.live->show(u"b"_s, 800, true);
        QCOMPARE(window.live->stream(), 0U);
        QCOMPARE(window.scene.removed, (QList<engine::StreamId>{41}));
        QTRY_COMPARE(window.server.live.lines, 2);
        QCOMPARE(window.server.asked.size(), 2);
        QCOMPARE(window.server.asked[1].path, QByteArray("/api/v1/devices/b/live"));
        QCOMPARE(window.server.live.path, QByteArray("/api/v1/devices/b/live"));
        QTRY_VERIFY(window.server.live.isConnected());
        window.server.live.send(1, true, 1, 0, "key of b");
        QTRY_COMPARE(window.live->stream(), 42U);
        QCOMPARE(window.scene.stream(42)->pushed, (QList<QByteArray>{"key of b"}));

        // Nor does a last picture that was still up: the machine is another.
        window.live->show(u"b"_s, 800, false);
        QCOMPARE(window.live->stream(), 42U);
        window.live->show(u"a"_s, 800, false);
        QCOMPARE(window.live->stream(), 0U);
        QCOMPARE(window.scene.removed, (QList<engine::StreamId>{41, 42}));
    }

    void aMachineIsAskedForOnlyOnceItHasStayedShownForAMoment()
    {
        // Somebody steps through the machines with a key held down.
        Window window(pass, {400, 100, 800, 5, 20});
        for (const QString& machine : {u"a"_s, u"b"_s, u"c"_s, u"d"_s}) {
            window.live->show(machine, 800, true);
            QTest::qWait(30);
        }
        QCOMPARE(window.server.asked.size(), 0);
        // The one that is stayed at is the one that is asked for, once.
        QTRY_COMPARE(window.server.live.lines, 1);
        QCOMPARE(window.server.asked.size(), 1);
        QCOMPARE(window.server.asked[0].path, QByteArray("/api/v1/devices/d/live"));
    }

    void aNoIsANoWhileTheMachineStaysShown_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("code");
        QTest::newRow("no live picture of this machine") << 404 << QByteArray("no_live");
        QTest::newRow("no such machine") << 404 << QByteArray("not_found");
        QTest::newRow("not for this account") << 403 << QByteArray("not_allowed");
        QTest::newRow("not for anybody") << 403 << QByteArray("forbidden");
    }

    void aNoIsANoWhileTheMachineStaysShown()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, code);
        Window window([=](const Asked&) {
            return Answered{status, QByteArray("{\"code\": \"") + code + "\", \"message\": \"No\"}"};
        });
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.asked.size(), 1);
        // Well over the time after which a server that was not reached is asked again.
        QTest::qWait(600);
        QCOMPARE(window.server.asked.size(), 1);
        QVERIFY2(window.server.live.lines == 0, window.server.live.path.constData());
        QCOMPARE(window.live->stream(), 0U);
        // The same machine again while it stays shown asks nothing. Shown anew, it is asked anew.
        window.live->show(u"a"_s, 900, true);
        QTest::qWait(150);
        QCOMPARE(window.server.asked.size(), 1);
        window.live->show({}, 0, false);
        window.live->show(u"a"_s, 900, true);
        QTRY_COMPARE(window.server.asked.size(), 2);
    }

    void whatThisCopyOfTheProgramDoesNotSpeakToIsNotAskedAgainWhileTheMachineStaysShown_data()
    {
        QTest::addColumn<bool>("byALine");
        QTest::newRow("the place refuses in that word") << false;
        QTest::newRow("a line is handed over, and ends in that word") << true;
    }

    void whatThisCopyOfTheProgramDoesNotSpeakToIsNotAskedAgainWhileTheMachineStaysShown()
    {
        // The program's own word that it speaks to nothing over HTTPS (tls.h): asking again gets
        // the same answer, for as long as the program runs. (It was not among the words that
        // end the asking: such a machine was asked for live again for ever, at the pace that
        // grows.)
        QFETCH(bool, byALine);
        PlaceThatSays place;
        place.code = core::tlsDamaged;
        place.byALine = byALine;
        NotedScene scene;
        quint32 lastNumber = 40;
        LivePicture live(LivePicture::Around{[&scene]() -> engine::Scene* { return &scene; },
                                             [&lastNumber] { return ++lastNumber; },
                                             [&place](const QString& key) { return ui::Reached{&place, key, {}}; },
                                             {}},
                         {10, 100, 800, 5, 20});
        live.show(u"a"_s, 800, true);
        QTRY_COMPARE(place.asked, 1);
        // Well over the time after which a place that was not reached is asked again.
        QTest::qWait(700);
        QCOMPARE(place.asked, 1);
        QCOMPARE(live.stream(), 0U);
        QVERIFY(live.live() == nullptr);
        // Shown anew, it is asked anew.
        live.show({}, 0, false);
        live.show(u"a"_s, 800, true);
        QTRY_COMPARE(place.asked, 2);
    }

    void aFormOfLivePicturesThisProgramDoesNotKnowIsNotAskedForAgain()
    {
        Window window(
            [](const Asked&) { return Answered{200, R"({"v": 3, "ticket": "p4ss-for-one-line_0123456789"})"}; });
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.asked.size(), 1);
        QTest::qWait(600);
        QCOMPARE(window.server.asked.size(), 1);
        QCOMPARE(window.server.live.lines, 0);
    }

    void aRefusalThatSaysNoSuchThingIsTriedAgain_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("answer");
        QTest::newRow("whatever stands in front of the server is not ready") << 502 << QByteArray("Bad Gateway");
        QTest::newRow("too many at once") << 429 << QByteArray(R"({"code": "busy", "message": "Try again shortly"})");
        QTest::newRow("the session is gone")
            << 401 << QByteArray(R"({"code": "unauthorized", "message": "Sign in first"})");
    }

    void aRefusalThatSaysNoSuchThingIsTriedAgain()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, answer);
        Window window([=](const Asked&) { return Answered{status, answer}; });
        window.live->show(u"a"_s, 800, true);
        QTRY_VERIFY(window.server.asked.size() >= 3);
        QCOMPARE(window.server.live.lines, 0);
    }

    void aServerThatIsNotReachedIsAskedAgainAndEachTimeLater()
    {
        // It falls over wherever a machine is asked for live. Asked at once, then after 200 ms,
        // 400 ms, 800 ms. (It says what it is: after a call that got no answer it is asked that
        // before the next goes out, and a question that only asks is one Qt asks again by itself
        // when the line falls, a few times. Counted here are the askings for a machine.)
        Window window(fallsOverAtAMachine, {10, 200, 5000, 5, 20});
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(askedForAMachine(window.server).size(), 2);
        // Not every 200 ms: by now that would have been six or more.
        QTest::qWait(1100);
        const qsizetype soFar = askedForAMachine(window.server).size();
        QVERIFY2(soFar >= 3, qPrintable(QString::number(soFar)));
        QVERIFY2(soFar <= 4, qPrintable(QString::number(soFar)));
        QCOMPARE(window.server.live.lines, 0);
        // And no longer once no machine is shown.
        window.live->show({}, 0, false);
        const qsizetype asked = window.server.asked.size();
        QTest::qWait(1800);
        QCOMPARE(window.server.asked.size(), asked);
    }

    void anotherMachineIsNotMadeToWaitAsLongAsTheOneBefore()
    {
        // (As above: the server says what it is, and the askings for a machine are counted.)
        Window window(fallsOverAtAMachine, {10, 200, 60'000, 5, 20});
        const auto askedFor = [&window] { return askedForAMachine(window.server); };
        window.live->show(u"a"_s, 800, true);
        // Asked at once, after 200 ms, after 400 more, after 800 more: the next would be 1.6 s on.
        QTRY_COMPARE_WITH_TIMEOUT(askedFor().size(), 4, 4000);
        window.live->show(u"b"_s, 800, true);
        QTRY_COMPARE(askedFor().size(), 5);
        QCOMPARE(askedFor()[4], QByteArray("/api/v1/devices/b/live"));
        // Its first wait is the short one again.
        QTRY_COMPARE_WITH_TIMEOUT(askedFor().size(), 6, 1000);
        QCOMPARE(askedFor()[5], QByteArray("/api/v1/devices/b/live"));
    }

    void theWaitIsNeverLongerThanTheLongest()
    {
        // (As above: the server says what it is, and the askings for a machine are counted.)
        Window window(fallsOverAtAMachine, {10, 100, 200, 5, 20});
        window.live->show(u"a"_s, 800, true);
        // At once, after 100, and then every 200: six in about a second. Doubling on, the sixth
        // would come after three.
        QTRY_COMPARE_WITH_TIMEOUT(askedForAMachine(window.server).size(), 6, 1800);
    }

    void aLineThatEndedKeepsItsLastPictureAndIsOpenedAgain()
    {
        Window window;
        QVERIFY(window.hasALive());
        window.server.live.fallOver();
        // Said: the snapshots are wanted again. Its last picture stays up until one has come.
        QTRY_COMPARE(window.changed->count(), 2);
        QVERIFY(window.live->wantsSnapshots());
        QCOMPARE(window.live->stream(), 41U);
        QVERIFY(window.scene.removed.isEmpty());
        // Another pass, another line, and its pictures in another stream, which takes its place.
        QTRY_COMPARE(window.server.live.lines, 2);
        QCOMPARE(window.server.asked.size(), 2);
        QTRY_VERIFY(window.server.live.isConnected());
        QCOMPARE(window.live->stream(), 41U);
        window.server.live.send(1, true, 1, 0, "key again");
        QTRY_COMPARE(window.live->stream(), 42U);
        QVERIFY(!window.live->wantsSnapshots());
        QCOMPARE(window.changed->count(), 3);
        QCOMPARE(window.scene.removed, (QList<engine::StreamId>{41}));
        QCOMPARE(window.scene.stream(42)->pushed, (QList<QByteArray>{"key again"}));
    }

    void aLineThatNeverGaveAPictureAndEndsSaysNothing()
    {
        Window window;
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.live.lines, 1);
        QTRY_VERIFY(window.server.live.isConnected());
        window.server.live.fallOver();
        QTRY_COMPARE(window.server.live.lines, 2);
        QCOMPARE(window.changed->count(), 0);
        QVERIFY(window.scene.added.isEmpty());
    }

    void aLineTheServerEndsWithANoIsNotOpenedAgain()
    {
        Window window;
        QVERIFY(window.hasALive());
        window.server.live.hangUp(u"no_live"_s, u"This machine has no live picture any more"_s);
        QTRY_COMPARE(window.changed->count(), 2);
        QVERIFY(window.live->wantsSnapshots());
        QTest::qWait(600);
        QCOMPARE(window.server.asked.size(), 1);
        QCOMPARE(window.server.live.lines, 1);
    }

    // Part D (web 0.2.0): what a server's goodbye says decides whether the line is opened again. The
    // words that end it for good, the new "unconfirmed" among them, and any word this program does not
    // know, end it cleanly with what the server said, and nothing is asked again while the machine
    // stays shown; the words of a passing end open it again. (Before: every word but a few was asked
    // again, an unknown one too.)
    void aServersGoodbyeDecidesWhetherTheLineIsOpenedAgain_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<bool>("again");
        for (const char* word : {"signed_out", "unconfirmed", "gone", "flooding", "bad_message", "not_allowed",
                                 "not_found", "a_word_this_program_does_not_know"})
            QTest::newRow(word) << QString::fromLatin1(word) << false;
        for (const char* word : {"closed", "slow", "internal", "closing", "no_pass"})
            QTest::newRow(word) << QString::fromLatin1(word) << true;
    }
    // M6 2h: "in_use" is a no (a JetKVM whose one viewer somebody else has): refused so, the machine is not
    // asked for again while it stays shown, and "Show here" (openAgain) asks for it at once. (Before: asked
    // again and again.)
    void inUseIsANoAndShowHereAsksAgain()
    {
        bool inUse = true;
        Window window([&inUse](const Asked& asked) {
            if (inUse && asked.method == "POST" && asked.path.endsWith("/live")) {
                return Answered{409, R"({"code": "in_use", "message": "Someone is using its own console."})"};
            }
            return pass(asked);
        });
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.asked.size(), 1);
        QTest::qWait(600);
        QCOMPARE(window.server.asked.size(), 1);
        inUse = false;
        window.live->openAgain(u"a"_s);
        QTRY_VERIFY(window.server.live.isConnected());
    }

    // A line that ends in_use is not opened again, also after another machine's "Show here"; that machine's
    // own opens it at once; and one whose line is open is asked for nothing more by it.
    void aLineEndedInUseIsOpenedAgainByItsOwnShowHereOnly()
    {
        Window window;
        QVERIFY(window.hasALive());
        window.server.live.hangUp(u"in_use"_s, u"Someone is using this JetKVM's own console."_s);
        QTRY_COMPARE(window.changed->count(), 2);
        QTest::qWait(600);
        QCOMPARE(window.server.live.lines, 1);
        window.live->openAgain(u"b"_s);
        QTest::qWait(300);
        QCOMPARE(window.server.live.lines, 1);
        window.live->openAgain(u"a"_s);
        QTRY_COMPARE(window.server.live.lines, 2);
        QTRY_VERIFY(window.server.live.isConnected());
        window.live->openAgain(u"a"_s);
        QTest::qWait(300);
        QCOMPARE(window.server.live.lines, 2);
    }

    // A line that ended in a passing word waits its time as ever: "Show here" asks for nothing sooner.
    void showHereAsksNothingOfALineNotStoppedOnANo()
    {
        Window window(pass, LivePicture::Pace{10, 1500, 3000, 5, 20});
        QVERIFY(window.hasALive());
        window.server.live.hangUp(u"closed"_s, u"Closing"_s);
        QTRY_COMPARE(window.changed->count(), 2);
        window.live->openAgain(u"a"_s);
        QTest::qWait(500);
        QCOMPARE(window.server.live.lines, 1);
        QTRY_COMPARE_WITH_TIMEOUT(window.server.live.lines, 2, 5000);
    }

    void aServersGoodbyeDecidesWhetherTheLineIsOpenedAgain()
    {
        QFETCH(QString, code);
        QFETCH(bool, again);
        Window window;
        QVERIFY(window.hasALive());
        QStringList told;
        connect(window.live.get(), &LivePicture::lineEndedBecause, this,
                [&](const QString& message) { told << message; });
        window.server.live.hangUp(code, u"What the server said of it"_s);
        QTRY_COMPARE(window.changed->count(), 2);
        QCOMPARE(told, QStringList{u"What the server said of it"_s});
        if (again) {
            QTRY_COMPARE(window.server.live.lines, 2);
        } else {
            QTest::qWait(600);
            QCOMPARE(window.server.asked.size(), 1);
            QCOMPARE(window.server.live.lines, 1);
        }
    }
    void linesThatGiveAPictureAndEndAreOpenedLaterEachTime()
    {
        // Each line gives a picture and falls over at once: such a server is not asked every
        // 150 ms for ever, but after 150 ms, 300, 600 and so on.
        Window window(pass, {10, 150, 60'000, 5, 20});
        window.live->show(u"a"_s, 800, true);
        for (int line = 1; line <= 3; ++line) {
            QTRY_COMPARE_WITH_TIMEOUT(window.server.live.lines, line, 2000);
            QTRY_VERIFY(window.server.live.isConnected());
            window.server.live.send(2, true, 1, 0, "a JPEG");
            QTRY_VERIFY(!window.live->wantsSnapshots());
            window.server.live.fallOver();
            QTRY_VERIFY(window.live->wantsSnapshots());
        }
        // The fourth comes 600 ms after the third ended, not 150.
        QTest::qWait(350);
        QCOMPARE(window.server.live.lines, 3);
        QTRY_COMPARE_WITH_TIMEOUT(window.server.live.lines, 4, 2000);
    }

    void aLineThatHeldForLongBeginsTheWaitingAnew()
    {
        // (For as long as the longest wait: a second and a half here.)
        Window window(pass, {10, 100, 1500, 5, 20});
        window.live->show(u"a"_s, 800, true);
        // Three that end at once, opened again after 100 ms, 200 and 400: the next wait would
        // be 800.
        for (int line = 1; line <= 3; ++line) {
            QTRY_COMPARE_WITH_TIMEOUT(window.server.live.lines, line, 2000);
            QTRY_VERIFY(window.server.live.isConnected());
            window.server.live.send(2, true, 1, 0, "a JPEG");
            QTRY_VERIFY(!window.live->wantsSnapshots());
            window.server.live.fallOver();
            QTRY_VERIFY(window.live->wantsSnapshots());
        }
        // The fourth holds for longer than that second and a half, and then ends: the fifth
        // comes after the short wait.
        QTRY_COMPARE_WITH_TIMEOUT(window.server.live.lines, 4, 2000);
        QTRY_VERIFY(window.server.live.isConnected());
        window.server.live.send(2, true, 1, 0, "a JPEG");
        QTRY_VERIFY(!window.live->wantsSnapshots());
        QTest::qWait(1700);
        window.server.live.fallOver();
        QTRY_VERIFY(window.live->wantsSnapshots());
        QTRY_COMPARE_WITH_TIMEOUT(window.server.live.lines, 5, 500);
    }

    void anAnswerForAMachineThatIsNoLongerShownOpensNothing()
    {
        Window window([](const Asked&) {
            Answered late{200, aPass};
            late.afterMs = 300;
            return late;
        });
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.asked.size(), 1);
        window.live->show({}, 0, false);
        QTest::qWait(700);
        QCOMPARE(window.server.live.lines, 0);
        // Nor does the answer for one machine open a line to the next.
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.asked.size(), 2);
        window.live->show(u"b"_s, 800, true);
        QTRY_COMPARE(window.server.live.lines, 1);
        QCOMPARE(window.server.live.path, QByteArray("/api/v1/devices/b/live"));
        QTest::qWait(500);
        QCOMPARE(window.server.live.lines, 1);
    }

    void itEndsItsLineWhenItEnds()
    {
        Window window;
        QVERIFY(window.hasALive());
        // (With a last picture of an earlier line still up, too.)
        window.server.live.send(2, true, 2, 0, "a JPEG");
        window.scene.makesPictures = false;
        QTRY_COMPARE(window.scene.added.size(), 2);
        window.changed.reset();
        window.live.reset();
        QCOMPARE(window.scene.removed.size(), 2);
        QTRY_VERIFY(!window.server.live.isConnected());
        // An answer that is still on its way finds nobody.
        Window other([](const Asked&) {
            Answered late{200, aPass};
            late.afterMs = 200;
            return late;
        });
        other.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(other.server.asked.size(), 1);
        other.changed.reset();
        other.live.reset();
        QTest::qWait(500);
        QCOMPARE(other.server.live.lines, 0);
    }

    // --- a KVM of this PC's own wall: asked of that wall, and shown as a server's machine is ---

    void aKvmOfThisPcsOwnWallIsHadLiveAsAServersMachineIs()
    {
        const QByteArray first = jpeg(1920, 1080, 400);
        const QByteArray second = jpeg(1920, 1080, 500);
        OwnWall own(first);
        NotedScene scene;
        quint32 lastNumber = 40;
        LivePicture live(LivePicture::Around{[&scene]() -> engine::Scene* { return &scene; },
                                             [&lastNumber] { return ++lastNumber; },
                                             [&own](const QString& key) { return ui::Reached{&own.wall, key, {}}; },
                                             {}},
                         LivePicture::Pace{10, 100, 800, 5, 20});
        const QSignalSpy changed(&live, &LivePicture::changed);
        live.show(own.id, 1280, true);
        // The KVM itself is asked, for its stream, signed in to as the wall signs in to it.
        QTRY_COMPARE(own.kvm.asked(streamOfAKvm()), 1);
        QCOMPARE(own.kvm.signIns, 1);
        // Its pictures go into a stream of the window's engine, stills as they are, shown large:
        // from the first one on that stream is what is drawn, and the snapshots are not needed.
        QTRY_COMPARE(changed.count(), 1);
        QCOMPARE(live.stream(), 41U);
        QVERIFY(!live.wantsSnapshots());
        QCOMPARE(scene.added, (QList<engine::StreamId>{41}));
        NotedStream* stream = scene.stream(41);
        QVERIFY(stream != nullptr);
        QCOMPARE(stream->codec, engine::Codec::Still);
        QCOMPARE(stream->matters, engine::Importance::ShownLarge);
        QCOMPARE(stream->size, (engine::SizePx{1280, 720}));
        QCOMPARE(stream->pushed, (QList<QByteArray>{first}));
        QCOMPARE(stream->keyFrames, (QList<bool>{true}));
        QVERIFY(own.kvm.stand->sendMore(partOf(second)));
        QTRY_COMPARE(stream->pushed, (QList<QByteArray>{first, second}));
        QCOMPARE(changed.count(), 1);
        // Drawn at another width: said to the engine, and nothing more is asked of the KVM.
        live.show(own.id, 1920, true);
        QCOMPARE(stream->size, (engine::SizePx{1920, 1080}));
        QTest::qWait(150);
        QCOMPARE(own.kvm.asked(streamOfAKvm()), 1);

        // No machine shown large: nothing of it stays up, its stream is given up, and the KVM
        // is asked for nothing more.
        live.show({}, 0, false);
        QCOMPARE(live.stream(), 0U);
        QCOMPARE(scene.removed, (QList<engine::StreamId>{41}));
        QTRY_VERIFY(!own.kvm.stand->isStreaming());
        const qsizetype heard = own.kvm.stand->asked.size();
        QTest::qWait(500);
        QCOMPARE(own.kvm.stand->asked.size(), heard);
    }

    void aKvmThatWasTakenOffTheWallIsNotAskedForAgainWhileItStaysShown()
    {
        OwnWall own(jpeg(1920, 1080, 400));
        NotedScene scene;
        quint32 lastNumber = 40;
        LivePicture live(LivePicture::Around{[&scene]() -> engine::Scene* { return &scene; },
                                             [&lastNumber] { return ++lastNumber; },
                                             [&own](const QString& key) { return ui::Reached{&own.wall, key, {}}; },
                                             {}},
                         LivePicture::Pace{10, 100, 800, 5, 20});
        live.show(own.id, 800, true);
        QTRY_COMPARE(live.stream(), 41U);
        // The wall's own word that the machine is not there: a no, as a server's is.
        QVERIFY(own.wall.remove(own.id).ok());
        QTRY_VERIFY(live.wantsSnapshots());
        QTRY_VERIFY(!own.kvm.stand->isStreaming());
        const qsizetype heard = own.kvm.stand->asked.size();
        QTest::qWait(600);
        QCOMPARE(own.kvm.stand->asked.size(), heard);
    }

    void givenAnotherPlaceItAsksThatOneAndOnlyWhileNothingIsShown()
    {
        Window window;
        const QByteArray first = jpeg(1920, 1080, 400);
        OwnWall own(first);
        QVERIFY(window.hasALive());
        // Under a machine that is shown, the place it has stays the one it asks.
        window.place = &own.wall;
        window.live->show(u"a"_s, 800, false);
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.asked.size(), 2);
        QTRY_COMPARE(window.server.live.lines, 2);
        QVERIFY(own.kvm.stand->asked.isEmpty());

        // Nothing shown: from now on this PC's own wall is asked, and the server no more.
        window.live->show({}, 0, false);
        window.place = &own.wall;
        window.live->show(own.id, 800, true);
        QTRY_COMPARE(own.kvm.asked(streamOfAKvm()), 1);
        QTRY_VERIFY(window.live->stream() != 0);
        NotedStream* stream = window.scene.stream(window.live->stream());
        QVERIFY(stream != nullptr);
        QCOMPARE(stream->codec, engine::Codec::Still);
        QCOMPARE(stream->pushed, (QList<QByteArray>{first}));
        QTest::qWait(300);
        QCOMPARE(window.server.asked.size(), 2);
        QCOMPARE(window.server.live.lines, 2);
        // And back, the same way.
        window.live->show({}, 0, false);
        window.place = &window.client;
        window.live->show(u"b"_s, 800, true);
        QTRY_COMPARE(window.server.asked.size(), 3);
        QCOMPARE(window.server.asked[2].path, QByteArray("/api/v1/devices/b/live"));
        QCOMPARE(own.kvm.asked(streamOfAKvm()), 1);
    }

    void whoeverSendsKeysIsToldThroughWhichLiveTheMachineIsHad()
    {
        Window window;
        // What is seen each time the word comes: the live it has then, and whether the one it
        // had before can still be spoken to.
        QList<core::Live*> had;
        QList<bool> theOneBeforeStillStood;
        QPointer<core::Live> before;
        connect(window.live.get(), &LivePicture::liveChanged, this, [&] {
            had.append(window.live->live());
            theOneBeforeStillStood.append(before != nullptr && before->isOpen());
            before = window.live->live();
        });
        // And why a line ended by itself, with how often the word above had come by then.
        QStringList ends;
        QList<qsizetype> wordsBeforeTheEnd;
        connect(window.live.get(), &LivePicture::lineEndedBecause, this, [&](const QString& message) {
            ends.append(message);
            wordsBeforeTheEnd.append(had.size());
        });
        QCOMPARE(window.live->live(), nullptr);
        // Had live: the word, with the one it is had through.
        QVERIFY(window.hasALive());
        QCOMPARE(had.size(), 1);
        core::Live* first = had.at(0);
        QVERIFY(first != nullptr);
        QCOMPARE(window.live->live(), first);
        // No machine is shown any more: said while that one still stands, and before it is
        // ended, so that what is down on the machine can be let go there.
        window.live->show({}, 0, false);
        QCOMPARE(had.size(), 2);
        QCOMPARE(had.at(1), nullptr);
        QVERIFY(theOneBeforeStillStood.at(1));
        QCOMPARE(window.live->live(), nullptr);
        QTRY_VERIFY(!window.server.live.isConnected());
        // Had again, through another one; and that one ends by itself: said as well.
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(had.size(), 3);
        QVERIFY(had.at(2) != nullptr);
        QTRY_VERIFY(window.server.live.isConnected());
        // (A line that was ended from here ended for no reason of its own: nothing was said.)
        QVERIFY(ends.isEmpty());
        window.server.live.fallOver();
        QTRY_COMPARE(had.size(), 4);
        QCOMPARE(had.at(3), nullptr);
        QCOMPARE(window.live->live(), nullptr);
        // Why it ended was said once, in a sentence, and before the word that there is none:
        // whoever had asked it for keyboard and mouse hears why no answer comes.
        QCOMPARE(ends.size(), 1);
        QVERIFY(!ends.at(0).isEmpty());
        QCOMPARE(wordsBeforeTheEnd.at(0), 3);
    }

    void aMachineOnTheWallWhoseKeyReachesNothingIsAskedOfNobody()
    {
        // The machine is on the wall, and is shown large and may be had live; its key reaches no
        // place.
        PlaceThatSays place;
        place.code = "no_live";
        OneMachine wall(place);
        NotedScene scene;
        quint32 lastNumber = 40;
        LivePicture live(LivePicture::Around{[&scene]() -> engine::Scene* { return &scene; },
                                             [&lastNumber] { return ++lastNumber; },
                                             [&wall](const QString& key) { return wall.reach(key); },
                                             {}},
                         {10, 100, 800, 5, 20});
        const QSignalSpy liveChanged(&live, &LivePicture::liveChanged);
        live.show(u"a"_s, 800, true);
        // Well over the time after which a machine that is shown is asked for, and after which one
        // that was not had is asked again: nothing is asked of any place, and nothing is had.
        QTest::qWait(400);
        QCOMPARE(place.asked, 0);
        QCOMPARE(liveChanged.count(), 0);
        QCOMPARE(live.live(), nullptr);
        QVERIFY(live.wantsSnapshots());
        QCOMPARE(live.stream(), 0U);
        QVERIFY(scene.added.isEmpty());

        // Its key reaches the place now. While the machine stays shown it is still asked of nobody:
        // where it is had live from is what its key reached when it came to be shown.
        wall.reaches = true;
        live.show(u"a"_s, 960, true);
        QTest::qWait(200);
        QCOMPARE(place.asked, 0);
        // Shown anew, it is asked of the place its key reaches then.
        live.show({}, 0, false);
        live.show(u"a"_s, 800, true);
        QTRY_COMPARE(place.asked, 1);
    }
    void diagnosticsReportObservedVideoAndTheActualDecoder()
    {
        Window window(pass, {10, 100, 800, 5, 20, 20});
        QVERIFY(window.live->diagnostics().isEmpty());
        QVERIFY(window.hasALive());
        emit window.live->live()->videoChanged({true, 1920, 1080});
        auto* stream = window.scene.stream(41);
        stream->reported.decoding = engine::Decoding::OnTheCard;
        stream->reported.picture = {1280, 720};
        stream->reported.droppedBehind = 3;
        stream->reported.refused = 2;
        stream->reported.decodeMs = 0.75;
        QTRY_COMPARE(window.live->diagnostics()[u"decoding"_s].toString(), u"gpu"_s);
        const auto details = window.live->diagnostics();
        QCOMPARE(details[u"codec"_s].toString(), u"h264"_s);
        QCOMPARE(details[u"width"_s].toInt(), 1280); // decoded size wins over source metadata
        QCOMPARE(details[u"height"_s].toInt(), 720);
        QCOMPARE(details[u"receivedPictures"_s].toULongLong(), 1ULL);
        QCOMPARE(details[u"receivedBytes"_s].toULongLong(), 3ULL);
        QCOMPARE(details[u"dropped"_s].toULongLong(), 3ULL);
        QCOMPARE(details[u"refused"_s].toULongLong(), 2ULL);
        QCOMPARE(details[u"decodeMs"_s].toDouble(), 0.75);
        QVERIFY(details[u"connected"_s].toBool());

        // What reached the screen, and how evenly: the engine's own counts, as they are.
        stream->reported.shown = 5;
        stream->reported.replaced = 1;
        stream->reported.arrivedToShownMs = 9.5;
        stream->reported.arrivedToShownMaxMs = 31.0;
        stream->reported.shownGapCounts = {0, 3, 2, 1, 0, 0, 0, 0};
        stream->reported.arrivalGapCounts = {0, 4, 5, 2, 0, 0, 0, 1};
        stream->reported.shownGapMaxMs = 41.0;
        stream->reported.arrivedToShownCounts = {0, 1, 2, 1, 0, 1, 0, 0};
        stream->reported.unevenCounts = {2, 1, 0, 4, 3, 0, 1, 0};
        stream->reported.waitedMs = 0.25;
        stream->reported.readyToDrawnMs = 6.5;
        stream->reported.drawnToShownMs = 1.75;
        stream->reported.readyToDrawnCounts = {0, 1, 0, 3, 0, 0, 0, 0};
        stream->reported.drawnToShownCounts = {4, 0, 0, 0, 0, 0, 0, 0};
        QTRY_COMPARE(window.live->diagnostics()[u"shown"_s].toULongLong(), 5ULL);
        const auto screen = window.live->diagnostics();
        QCOMPARE(screen[u"replaced"_s].toULongLong(), 1ULL);
        QCOMPARE(screen[u"arrivedToShownMs"_s].toDouble(), 9.5);
        QCOMPARE(screen[u"arrivedToShownMaxMs"_s].toDouble(), 31.0);
        QCOMPARE(screen[u"shownGapMaxMs"_s].toDouble(), 41.0);
        const QVariantList shownSteps{0ULL, 3ULL, 2ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL};
        const QVariantList arrivalSteps{0ULL, 4ULL, 5ULL, 2ULL, 0ULL, 0ULL, 0ULL, 1ULL};
        QCOMPARE(screen[u"shownGaps"_s].toList(), shownSteps);
        QCOMPARE(screen[u"arrivalGaps"_s].toList(), arrivalSteps);
        const QVariantList steps{12.0, 21.0, 38.0, 54.0, 71.0, 104.0, 250.0};
        QCOMPARE(screen[u"gapStepsMs"_s].toList(), steps);
        const QVariantList delays{0ULL, 1ULL, 2ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL};
        QCOMPARE(screen[u"arrivedToShown"_s].toList(), delays);
        const QVariantList delaySteps{2.0, 4.0, 8.0, 12.0, 16.0, 33.0, 100.0};
        QCOMPARE(screen[u"arrivedToShownStepsMs"_s].toList(), delaySteps);
        const QVariantList uneven{2ULL, 1ULL, 0ULL, 4ULL, 3ULL, 0ULL, 1ULL, 0ULL};
        QCOMPARE(screen[u"uneven"_s].toList(), uneven);
        // Of those: steps longer than two refreshes of a 60 Hz screen (38 ms), on it and as they
        // arrived, and pictures that took more than 12 ms (most of a refresh of one) longer or
        // shorter to the screen than the one before.
        QCOMPARE(screen[u"waitedMs"_s].toDouble(), 0.25);
        QCOMPARE(screen[u"readyToDrawnMs"_s].toDouble(), 6.5);
        QCOMPARE(screen[u"drawnToShownMs"_s].toDouble(), 1.75);
        QCOMPARE(screen[u"readyToDrawn"_s].toList(), (QVariantList{0ULL, 1ULL, 0ULL, 3ULL, 0ULL, 0ULL, 0ULL, 0ULL}));
        QCOMPARE(screen[u"drawnToShown"_s].toList(), (QVariantList{4ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL}));
        QCOMPARE(screen[u"lateSteps"_s].toLongLong(), 1LL);
        QCOMPARE(screen[u"sourceLateSteps"_s].toLongLong(), 3LL);
        QCOMPARE(screen[u"unevenSteps"_s].toLongLong(), 4LL);
        const QVariantList unevenSteps{2.0, 4.0, 8.0, 12.0, 17.0, 25.0, 34.0};
        QCOMPARE(screen[u"unevenStepsMs"_s].toList(), unevenSteps);

        // Hardware preference is not a hardware result: the real engine can fall back.
        stream->reported.decoding = engine::Decoding::InSoftware;
        QTRY_COMPARE(window.live->diagnostics()[u"decoding"_s].toString(), u"software"_s);
        window.hasScene = false;
        QTRY_COMPARE(window.live->diagnostics()[u"decoding"_s].toString(), u"unknown"_s);
        QVERIFY(!window.live->diagnostics()[u"engineKnown"_s].toBool());
        // With no stream of the engine to count, no rate on the screen is known, however long.
        QTest::qWait(100);
        QVERIFY(!window.live->diagnostics()[u"shownRateKnown"_s].toBool());
        QCOMPARE(window.live->diagnostics()[u"width"_s].toInt(), 1920);
    }

    void receiveRatesBecomeZeroOnSilenceAndAgeUsesThisPcsClock()
    {
        Window window(pass, {10, 100, 800, 5, 20, 200});
        QVERIFY(window.hasALive());
        // Source clocks need not match this PC. This timestamp must never be subtracted
        // from the local clock and reported as latency.
        window.server.live.send(1, false, 2, 9'000'000'000'000LL, "next");
        QTRY_COMPARE(window.live->diagnostics()[u"receivedPictures"_s].toULongLong(), 2ULL);
        QTRY_VERIFY(window.live->diagnostics()[u"rateKnown"_s].toBool());
        QTRY_VERIFY(window.live->diagnostics()[u"receivedFps"_s].toDouble() > 0);
        QTRY_VERIFY(window.live->diagnostics()[u"receivedFps"_s].toDouble() == 0);
        QCOMPARE(window.live->diagnostics()[u"receivedMbps"_s].toDouble(), 0.0);
        const auto age = window.live->diagnostics()[u"lastReceivedMs"_s].toLongLong();
        QVERIFY(age >= 0 && age < 5000);
    }

    // How many pictures a second reach the screen, beside how many arrive: the first is what is seen.
    void theShownRateFollowsTheEngineAndNeverGoesBelowNought()
    {
        Window window(pass, {10, 100, 800, 5, 20, 200});
        QVERIFY(window.hasALive());
        emit window.live->live()->videoChanged({true, 1920, 1080});
        auto* stream = window.scene.stream(41);
        QVERIFY(stream != nullptr);
        QCOMPARE(window.live->diagnostics()[u"shownFps"_s].toDouble(), 0.0);
        const auto rate = [&window] { return window.live->diagnostics()[u"shownFps"_s].toDouble(); };
        // The most it said over a while.
        const auto mostFor = [&rate](int ms) {
            double most = 0;
            QElapsedTimer watched;
            watched.start();
            while (watched.elapsed() < ms) {
                most = std::max(most, rate());
                QTest::qWait(20);
            }
            return most;
        };
        // The rate is reckoned from a count of this stream's: one while has passed first.
        QTRY_VERIFY(window.live->diagnostics()[u"shownRateKnown"_s].toBool());
        // 30 in a while of about 200 ms: about 150 a second.
        stream->reported.shown = 30;
        QTRY_VERIFY(rate() > 0);
        QVERIFY2(rate() <= 30 * 1000.0 / 200, qPrintable(QString::number(rate())));
        QTRY_COMPARE(rate(), 0.0);
        // The same stream counting from nought again is no negative rate, and none of billions:
        // no rate for that while, and from that count on.
        stream->reported.shown = 4;
        QCOMPARE(mostFor(450), 0.0);
        stream->reported.shown = 8;
        QTRY_VERIFY(rate() > 0);
        QVERIFY2(rate() <= 4 * 1000.0 / 200, qPrintable(QString::number(rate())));
        QTRY_COMPARE(rate(), 0.0);

        // The numbers come to be another stream's, which has shown many more: that is not a rate.
        window.server.live.send(2, true, 2, 0, "jpeg");
        QTRY_VERIFY(window.scene.added.size() >= 2);
        NotedStream* other = window.scene.stream(window.scene.added.last());
        QVERIFY(other != nullptr && other != stream);
        other->reported.shown = 5000;
        QTRY_VERIFY(!window.live->diagnostics()[u"shownRateKnown"_s].toBool());
        const double most = mostFor(800);
        QVERIFY2(most < 100, qPrintable(QString::number(most)));
        QVERIFY(window.live->diagnostics()[u"shownRateKnown"_s].toBool());
        other->reported.shown = 5010;
        QTRY_VERIFY(rate() > 0);
        QVERIFY2(rate() <= 10 * 1000.0 / 200, qPrintable(QString::number(rate())));
    }

    void diagnosticsResetBetweenMachinesAndConnections()
    {
        Window window(pass, {10, 100, 800, 5, 20, 20});
        QVERIFY(window.hasALive());
        window.live->show(u"a"_s, 800, false);
        QVERIFY(!window.live->diagnostics()[u"connected"_s].toBool());
        QCOMPARE(window.live->diagnostics()[u"receivedPictures"_s].toULongLong(), 1ULL);
        window.live->show(u"a"_s, 800, true);
        QTRY_COMPARE(window.server.live.lines, 2);
        QCOMPARE(window.live->diagnostics()[u"receivedPictures"_s].toULongLong(), 0ULL);
        QCOMPARE(window.live->diagnostics()[u"lastReceivedMs"_s].toLongLong(), -1LL);
        QCOMPARE(window.live->diagnostics()[u"codec"_s].toString(), u"unknown"_s);
        window.server.live.send(2, true, 1, 0, "jpeg");
        QTRY_COMPARE(window.live->diagnostics()[u"codec"_s].toString(), u"jpeg"_s);
        window.live->show(u"b"_s, 800, true);
        QCOMPARE(window.live->diagnostics()[u"receivedPictures"_s].toULongLong(), 0ULL);
        window.live->show({}, 0, false);
        QVERIFY(window.live->diagnostics().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestLivePicture)

#include "test_live_picture.moc"
