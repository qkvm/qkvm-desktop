#include <memory>
#include <optional>
#include <utility>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <qkvm/base/clock.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_live.h>
#include <qkvm/core/live.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/settings.h>

#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmdDevice;
using core::KvmdLive;
using core::Live;
using core::LocalWall;
using core::testing::Answered;
using core::testing::jpeg;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::NobodyThere;
using core::testing::refusal;

Q_DECLARE_METATYPE(qkvm::core::testing::Answered)

namespace {

// Where a KVM has its stream of pictures, and where it is asked for one picture.
QByteArray place()
{
    return "/streamer/stream"_ba;
}
QByteArray onePicture()
{
    return "/api/streamer/snapshot"_ba;
}

// The word ustreamer puts between its pictures, and how it says so.
QByteArray word()
{
    return "boundarydonotcross"_ba;
}
QByteArray kind()
{
    return "multipart/x-mixed-replace;boundary=" + word();
}
QByteArray boundary()
{
    return "--" + word() + "\r\n";
}

// One picture as ustreamer sends it: its lines, the picture, and the boundary of the next.
QByteArray part(const QByteArray& picture)
{
    return "Content-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(picture.size()) +
           "\r\nX-Timestamp: 1728000000.123456\r\n\r\n" + picture + "\r\n" + boundary();
}

// How a KVM begins its stream: the first boundary, and what the test wants sent with it.
Answered streaming(const QByteArray& first = {})
{
    Answered answer(200, boundary() + first, kind());
    answer.streams = true;
    return answer;
}

// What a gateway says in a KVM's place of a stream that is not behind it.
Answered gatewaysPage()
{
    return {502, "<html><head><title>502 Bad Gateway</title></head><body>502 Bad Gateway</body></html>"_ba,
            "text/html"_ba};
}

// kvmd's words, as the made-up KVM is to read them.
QJsonObject keyOf(const QString& name, bool down)
{
    return {{u"event_type"_s, u"key"_s}, {u"event"_s, QJsonObject{{u"key"_s, name}, {u"state"_s, down}}}};
}
QJsonObject buttonOf(const QString& name, bool down)
{
    return {{u"event_type"_s, u"mouse_button"_s}, {u"event"_s, QJsonObject{{u"button"_s, name}, {u"state"_s, down}}}};
}

// USB's numbers for the keys the tests press.
constexpr int keyA = 0x04;
constexpr int keyB = 0x05;
constexpr int enter = 0x28;

// What a KVM heard on its lines (/api/ws) of keys, buttons and the pointer, in the order it came:
// everything but the asking whether it is there.
QList<QJsonObject> pressedOn(const MadeUpKvm& kvm)
{
    QList<QJsonObject> found;
    for (const QJsonObject& said : std::as_const(kvm.stand->live.said)) {
        if (said[u"event_type"_s].toString() != u"ping"_s) {
            found.append(said);
        }
    }
    return found;
}

// What one who has a KVM live was told.
struct Told {
    explicit Told(const Live* live)
    {
        QObject::connect(live, &Live::opened, &heard, [this] { ++opened; });
        QObject::connect(live, &Live::videoChanged, &heard, [this](const Live::Video& video) { videos.append(video); });
        QObject::connect(live, &Live::pictureCame, &heard,
                         [this](const Live::Picture& picture) { pictures.append(picture); });
        QObject::connect(live, &Live::stateChanged, &heard,
                         [this](const QString& state, const QString& code) { states.append({state, code}); });
        QObject::connect(live, &Live::controlChanged, &heard,
                         [this](bool on, const QString& code, const QString& message) {
                             control.append({on ? u"on"_s : u"off"_s, code, message});
                             said.append(on ? u"on"_s : u"off"_s);
                         });
        QObject::connect(live, &Live::leftDown, &heard, [this] {
            ++leftDown;
            said.append(u"left down"_s);
        });
        QObject::connect(live, &Live::closed, &heard, [this](const QString& code, const QString& message) {
            closed.append({code, message});
            said.append(u"closed"_s);
        });
    }

    // The pictures' bytes, and their counters, in the order they came.
    QList<QByteArray> data() const
    {
        QList<QByteArray> all;
        for (const Live::Picture& picture : pictures) {
            all.append(picture.data);
        }
        return all;
    }
    QList<quint32> counters() const
    {
        QList<quint32> all;
        for (const Live::Picture& picture : pictures) {
            all.append(picture.counter);
        }
        return all;
    }

    QObject heard; // what is heard ends with this
    int opened = 0;
    QList<Live::Video> videos;
    QList<Live::Picture> pictures;
    QList<QStringList> states;
    QList<QStringList> control;
    QList<QStringList> closed;
    // How often it was told that something may still be down on the machine; and what it was told
    // of keyboard and mouse and of its end, in the order it came: "on", "off", "left down", "closed".
    int leftDown = 0;
    QStringList said;
};

// A screen that is another at every look: the made-up KVM's picture is changed every few
// milliseconds. (A picture that is the one before is not handed on a second time.)
struct ChangingScreen {
    explicit ChangingScreen(MadeUpKvm& kvm)
    {
        QObject::connect(&timer, &QTimer::timeout, &timer,
                         [this, &kvm] { kvm.picture = jpeg(kvm.width, kvm.height, 64 + (++turns % 900)); });
        timer.start(5);
    }

    QTimer timer;
    int turns = 0;
};

// A KVM that says now and then that it is there, on the line that was made last, as a real one
// answers when it is asked: a line for keyboard and mouse on which nothing comes for three
// seconds is cut.
struct SaysItIsThere {
    explicit SaysItIsThere(MadeUpKvm& kvm)
    {
        QObject::connect(&timer, &QTimer::timeout, &timer,
                         [&kvm] { kvm.stand->live.sayRaw(uR"({"event_type":"pong","event":{}})"_s); });
        timer.start(400);
    }

    QTimer timer;
};

// When each asking for its stream reached a made-up KVM, in milliseconds from when this was made.
struct StreamAskings {
    explicit StreamAskings(const MadeUpKvm& kvm)
    {
        clock.start();
        QObject::connect(&looking, &QTimer::timeout, &looking, [this, &kvm] {
            while (at.size() < kvm.asked(place())) {
                at.append(clock.elapsed());
            }
        });
        looking.start(3);
    }

    QList<qint64> at;
    QElapsedTimer clock;
    QTimer looking;
};

// The times a real KVM is given, made short.
KvmdDevice::Pace quick()
{
    KvmdDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.refusedHoldMs = 1200;
    pace.signInRetryMs = 800;
    pace.freshMs = 200;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    pace.restAfterMs = 900;
    pace.streamQuietMs = 3000;
    return pace;
}

// And those of a live: a KVM that rests is asked again after 60 ms, one that failed after 450
// (longer than its driver leaves it alone, as in the program: each failure is a question).
KvmdLive::Pace brisk()
{
    KvmdLive::Pace pace;
    pace.restingAgainMs = 60;
    pace.failedAgainMs = 450;
    return pace;
}

// This PC's own wall for a test: what it remembers in a folder that goes with the test, its
// passwords in memory, and drivers that do not wait long.
struct Own {
    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    MemorySecrets secrets;
    LocalWall wall{settings, secrets, quick()};

    // Puts a made-up KVM on the wall, and says what the wall calls it.
    QString add(const MadeUpKvm& kvm, const QString& name)
    {
        core::OwnKvm own;
        own.address = kvm.stand->address();
        own.name = name;
        return wall.add(own, u"secret"_s).value();
    }
};

// A machine live, as a wall is asked for it: what was handed over, or why nothing was.
struct LiveOf {
    LiveOf(core::WallSource& source, const QString& machine, Live::Need need = {1920, 30})
    {
        source.openLive(machine, need, [this](base::Result<std::unique_ptr<Live>> made) {
            answered = true;
            if (made) {
                live = std::move(made).value();
            } else {
                refused = made.error();
            }
        });
        static_cast<void>(QTest::qWaitFor([this] { return answered; }, 10000));
    }

    bool answered = false;
    std::unique_ptr<Live> live;
    std::optional<base::Error> refused;
};

} // namespace

// A KVM of this PC's own, live: its pictures by its stream or one after the other, shared by all
// who have it live, and keyboard and mouse to it. Against a made-up KVM.
class TestKvmdLive : public QObject {
    Q_OBJECT

private slots:
    void theMouseGoesTheWayTheKvmIsSetAndIsNeverSwitched()
    {
        // The KVM's mouse output is a setting of the device that its other users share (rule 26 of the web's
        // FOR-THE-DESKTOP-APP): followed as /api/hid says it, and never switched for a window that opens. kvmd drops a
        // place while its mouse is relative, and steps while it is absolute.
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        const auto mouse = [](bool absolute) {
            return core::testing::good(
                {{u"keyboard"_s, QJsonObject{{u"online"_s, true}}},
                 {u"mouse"_s,
                  QJsonObject{{u"online"_s, true},
                              {u"absolute"_s, absolute},
                              {u"outputs"_s, QJsonObject{{u"available"_s, QJsonArray{u"usb"_s, u"usb_rel"_s}},
                                                         {u"active"_s, absolute ? u"usb"_s : u"usb_rel"_s}}}}}});
        };
        kvm.instead.insert("/api/hid"_ba, mouse(false));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        auto one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        // (Looked at as a wall looks at it, every few seconds.)
        QSignalSpy looked(&device, &KvmdDevice::looked);
        device.look();
        QVERIFY(looked.wait(10000));
        QTRY_COMPARE(one->pointerMode(), Live::Pointer::Relative);
        // Whoever shows it asks for the other kind: keyboard and mouse are given, the KVM is left as it is set, and the
        // pointer goes by steps.
        one->chooseRelativeMouse(false);
        one->askControl(true);
        QTRY_VERIFY(one->hasControl());
        QCOMPARE(kvm.asked("/api/hid/set_params"), 0);
        QCOMPARE(one->pointerMode(), Live::Pointer::Relative);
        // Set otherwise at the KVM (its own page, another program): followed at the next look, and said.
        QSignalSpy changed(one.get(), &Live::pointerModeChanged);
        kvm.instead.insert("/api/hid"_ba, mouse(true));
        device.look();
        QVERIFY(looked.wait(10000));
        QTRY_COMPARE(changed.count(), 1);
        QCOMPARE(one->pointerMode(), Live::Pointer::Absolute);
        // A look that says the same says nothing new.
        device.look();
        QVERIFY(looked.wait(10000));
        QTest::qWait(100);
        QCOMPARE(changed.count(), 1);
        // A KVM that does not say how its mouse is set: not known, and not switched either.
        kvm.instead.insert("/api/hid"_ba, core::testing::good({{u"mouse"_s, QJsonObject{{u"online"_s, true}}}}));
        device.look();
        QVERIFY(looked.wait(10000));
        QTRY_COMPARE(changed.count(), 2);
        QCOMPARE(one->pointerMode(), Live::Pointer::Unknown);
        one->askControl(false);
        QTRY_VERIFY(!one->hasControl());
        one->chooseRelativeMouse(true);
        one->askControl(true);
        QTRY_VERIFY(one->hasControl());
        QCOMPARE(kvm.asked("/api/hid/set_params"), 0);
    }

    void initTestCase()
    {
        // The device chooses Windows' own TLS, which is refused once TLS is in use: so one is
        // made before anything else touches TLS (the stand-ins do).
        const KvmdDevice first(KvmdDevice::Where{}, QString());
    }

    // --- its pictures ---

    void aKvmThatServesAStreamGivesItsPicturesAsTheyCome()
    {
        MadeUpKvm kvm;
        const QByteArray first = jpeg(1920, 1080, 500);
        const QByteArray second = jpeg(1920, 1080, 90'000);
        const QByteArray third = jpeg(1280, 720, 10);
        kvm.instead.insert(place(), streaming(part(first)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const qint64 before = base::steadyNowUs();
        const std::unique_ptr<Live> one = live.open({1280, 30});
        const Told told(one.get());
        // It is live from when it is handed over, and says nothing before whoever asked holds it.
        QVERIFY(one->isOpen());
        QCOMPARE(told.opened, 0);
        QVERIFY(told.pictures.isEmpty());
        QTRY_COMPARE(told.pictures.size(), 1);
        QCOMPARE(told.opened, 1);
        QVERIFY(kvm.stand->sendMore(part(second) + part(third)));
        QTRY_COMPARE(told.pictures.size(), 3);
        QCOMPARE(told.data(), (QList<QByteArray>{first, second, third}));
        // Each a JPEG and a whole picture, counted from one, with when it came on this PC's clock.
        QCOMPARE(told.counters(), (QList<quint32>{1, 2, 3}));
        qint64 last = before;
        for (const Live::Picture& picture : told.pictures) {
            QVERIFY(!picture.h264);
            QVERIFY(picture.keyFrame);
            QVERIFY(picture.takenUs >= last);
            last = picture.takenUs;
        }
        QVERIFY(last <= base::steadyNowUs());
        // The size is said before the first, and again only when it is another.
        QCOMPARE(told.videos, (QList<Live::Video>{{false, 1920, 1080}, {false, 1280, 720}}));
        QVERIFY(kvm.stand->sendMore(part(third)));
        QTRY_COMPARE(told.pictures.size(), 4);
        QCOMPARE(told.videos.size(), 2);
        // One stream, signed in once, and no picture asked for beside it.
        QCOMPARE(kvm.asked(place()), 1);
        QCOMPARE(kvm.signIns, 1);
        QCOMPARE(kvm.asked(onePicture()), 0);
        QVERIFY(!live.servesNoStream());
        QVERIFY(told.closed.isEmpty());
        // The Janus probe was refused. A JPEG key-frame request adds no HTTP request.
        one->askForAKeyFrame();
        QTest::qWait(100);
        QCOMPARE(kvm.asked("/janus/ws"_ba), 1);
        QCOMPARE(kvm.stand->asked.size(), 4); // (and the question asked before the sign-in)
    }

    void whatStatesNoSizeOrOneThatIsNotShownIsNotHandedOn()
    {
        MadeUpKvm kvm;
        const QByteArray good = jpeg(1920, 1080, 300);
        kvm.instead.insert(place(), streaming(part("nothing a decoder could read"_ba)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(kvm.stand->isStreaming());
        // Small to send, enormous to draw; half a picture's beginning; and then a picture.
        QVERIFY(kvm.stand->sendMore(part(jpeg(20'000, 20'000, 10)) + part(good.first(12)) + part(good)));
        QTRY_COMPARE(told.pictures.size(), 1);
        QCOMPARE(told.pictures[0].data, good);
        QCOMPARE(told.pictures[0].counter, 1U);
        QCOMPARE(told.videos, (QList<Live::Video>{{false, 1920, 1080}}));
        QVERIFY(told.closed.isEmpty());
    }

    void aKvmWithoutAStreamIsAskedForItsPicturesOneAfterTheOther_data()
    {
        QTest::addColumn<Answered>("stream");
        QTest::addColumn<int>("streamsAsked");
        QTest::newRow("it has nothing at that place") << Answered(404, "{}"_ba) << 1;
        // A KVM that answers so also while its video is awake. (One maker's was seen to answer so
        // while its video rests, and as in the next row while it is awake.) A gateway's page alone
        // does not say whether the video only rests: so the stream is asked for once more, when a
        // picture has shown the video awake, and the same page then says that there is none.
        // (This row took "one or two": the driver told every such page as "no stream here", at
        // the first asking, also of a KVM whose video only rested.)
        QTest::newRow("its gateway answers with a page") << gatewaysPage() << 2;
        // As the same maker's KVM does it while its video is awake: a yes, and something without
        // end that is of another kind than a stream of pictures. Asked for once: that is no
        // stream that broke, to be asked for again for as long as the KVM is shown.
        Answered another(200, "what is no picture"_ba, "application/octet-stream"_ba);
        another.streams = true;
        QTest::newRow("it answers with a yes and something of another kind") << another << 1;
    }

    void aKvmWithoutAStreamIsAskedForItsPicturesOneAfterTheOther()
    {
        QFETCH(Answered, stream);
        QFETCH(int, streamsAsked);
        MadeUpKvm kvm;
        kvm.instead.insert(place(), stream);
        const ChangingScreen screen(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(told.pictures.size() >= 3);
        // Whole JPEGs as the KVM gave them, counted from one, and their size said once.
        for (qsizetype index = 0; index < told.pictures.size(); ++index) {
            const Live::Picture& picture = told.pictures[index];
            QCOMPARE(picture.counter, static_cast<quint32>(index + 1));
            QVERIFY(!picture.h264);
            QVERIFY(picture.keyFrame);
            QCOMPARE(core::kvmd::pictureSize(picture.data), std::optional<QSize>(QSize(1920, 1080)));
        }
        QCOMPARE(told.videos, (QList<Live::Video>{{false, 1920, 1080}}));
        QVERIFY(told.closed.isEmpty());
        // Each as the KVM captured it: nothing that has it make a smaller one.
        QVERIFY(!kvm.pictureAsks.isEmpty());
        for (const QUrlQuery& asked : std::as_const(kvm.pictureAsks)) {
            QCOMPARE(asked.queryItemValue(u"allow_offline"_s), u"1"_s);
            QVERIFY2(!asked.hasQueryItem(u"preview"_s), qPrintable(asked.toString()));
        }
        // That it serves no stream is remembered.
        QTRY_VERIFY(live.servesNoStream());
        const int streams = kvm.asked(place());
        QCOMPARE(streams, streamsAsked);
        // Its stream is not asked for again: not for a second who comes, and not for one who
        // comes when nobody had it live any more.
        std::unique_ptr<Live> two = live.open({1280, 30});
        const Told other(two.get());
        QTRY_VERIFY(other.pictures.size() >= 2);
        QCOMPARE(other.pictures[0].counter, 1U);
        QCOMPARE(other.videos, (QList<Live::Video>{{false, 1920, 1080}}));
        one.reset();
        two.reset();
        QCOMPARE(live.watchers(), 0);
        QTest::qWait(300);
        const std::unique_ptr<Live> later = live.open({1920, 30});
        const Told again(later.get());
        QTRY_VERIFY(again.pictures.size() >= 2);
        QCOMPARE(again.pictures[0].counter, 1U);
        QCOMPARE(kvm.asked(place()), streams);
    }

    void theNextPictureIsAskedForWhenTheLastHasCome()
    {
        // A KVM that takes 400 ms over each picture, and one who would have thirty a second.
        // (The driver joins a question that comes while one is out to that one, so the KVM
        // would hear one at a time whatever is asked of the driver: what is seen here is the two
        // of them together.)
        MadeUpKvm kvm;
        kvm.pictureAfterMs = 400;
        const ChangingScreen screen(kvm);
        KvmdDevice::Pace pace = quick();
        pace.patienceMs = 1200;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        // When each question for a picture reached the KVM.
        QList<qint64> came;
        QElapsedTimer clock;
        clock.start();
        QTimer looking;
        connect(&looking, &QTimer::timeout, &looking, [&] {
            while (came.size() < kvm.asked(onePicture())) {
                came.append(clock.elapsed());
            }
        });
        looking.start(3);
        QTRY_VERIFY_WITH_TIMEOUT(told.pictures.size() >= 4, 10000);
        QVERIFY(came.size() >= 4);
        for (qsizetype index = 1; index < came.size(); ++index) {
            QVERIFY2(came[index] - came[index - 1] >= 300, qPrintable(QString::number(came[index] - came[index - 1])));
        }
        QVERIFY(told.closed.isEmpty());
    }

    void picturesAreAskedForNoMoreOftenThanWhoeverLooksNeeds()
    {
        MadeUpKvm kvm;
        const ChangingScreen screen(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        // Two pictures a second.
        const std::unique_ptr<Live> one = live.open({1920, 2});
        const Told told(one.get());
        QTRY_VERIFY(told.pictures.size() >= 1);
        int before = kvm.asked(onePicture());
        QTest::qWait(1600);
        int heard = kvm.asked(onePicture()) - before;
        // One every half second: three in that time, and a fourth where the first was just due.
        QVERIFY2(heard >= 2 && heard <= 4, qPrintable(QString::number(heard)));
        // Somebody needs four a second: said, and asked for at that pace from then on, without
        // the slower pause being waited out.
        one->setNeed({1920, 4});
        before = kvm.asked(onePicture());
        QTest::qWait(1600);
        heard = kvm.asked(onePicture()) - before;
        QVERIFY2(heard >= 5 && heard <= 8, qPrintable(QString::number(heard)));
        QVERIFY(told.closed.isEmpty());
    }

    void whoeverHasItLiveGetsMoreThanFivePicturesASecondAndNeverAsksForTwoAtOnce()
    {
        // A KVM without a stream, which gives a picture as captured in three hundredths of a
        // second, and a screen that is another at every look. The driver answers a card's
        // question that follows closely on another with the picture it has, which is five
        // different pictures a second at the most: whoever has the KVM live asks for a new one
        // each time, up to twenty a second.
        MadeUpKvm kvm;
        kvm.pictureAfterMs = 30;
        const ChangingScreen screen(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(told.pictures.size() >= 1);
        const qsizetype had = told.pictures.size();
        const int askedBefore = kvm.asked(onePicture());
        QTest::qWait(2000);
        QSet<QByteArray> different;
        for (qsizetype index = had; index < told.pictures.size(); ++index) {
            different.insert(told.pictures[index].data);
        }
        // In two seconds: more than the ten or eleven that five a second are, by a margin (a PC
        // that is busy gives fewer than the forty there could be), and each of them another.
        QVERIFY2(different.size() >= 14, qPrintable(QString::number(different.size())));
        QCOMPARE(different.size(), told.pictures.size() - had);
        // No more often than the live's own pace, and never a second question while one is out:
        // the made-up KVM takes long enough over each to see two at once, if there were.
        const int asked = kvm.asked(onePicture()) - askedBefore;
        QVERIFY2(asked <= 2 * KvmdLive::Pace{}.mostASecond + 2, qPrintable(QString::number(asked)));
        QCOMPARE(kvm.mostPicturesAtOnce, 1);
        QVERIFY(told.closed.isEmpty());
    }

    void aKvmWhoseVideoIsKnownToBeAwakeAndWhoseGatewayAnswersWithAPageIsAskedForItsStreamOnce()
    {
        // The wall has been taking its pictures: its video is awake, and the driver knows. A
        // gateway's page at the stream's place then says at the first asking that there is none.
        MadeUpKvm kvm;
        kvm.instead.insert(place(), gatewaysPage());
        const ChangingScreen screen(kvm);
        KvmdDevice::Pace pace = quick();
        pace.freshMs = 20'000;
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        // (Asked until one has come: a first asking may be told to wait for the KVM's first answer.)
        bool came = false;
        for (int asking = 0; asking < 20 && !came; ++asking) {
            bool answered = false;
            device.askSnapshot(400, [&answered, &came](const base::Result<core::Snapshot>& picture) {
                answered = true;
                came = picture.ok();
            });
            QTRY_VERIFY(answered);
        }
        QVERIFY(came);
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(told.pictures.size() >= 3);
        QVERIFY(live.servesNoStream());
        QCOMPARE(kvm.asked(place()), 1);
    }

    void aKvmWhoseVideoRestsIsAskedAgainSoonAndThatIsNoFailure()
    {
        // It rests whatever line it has, for most of a second.
        MadeUpKvm kvm;
        kvm.restsThoughWatched = true;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTest::qWait(900);
        // Asked again and again, more often than three failures would have let it; its video is
        // being woken meanwhile; and the live is there.
        QVERIFY(kvm.asked(onePicture()) >= 2);
        QVERIFY(device.isKeptAwake());
        QVERIFY(told.pictures.isEmpty());
        QVERIFY(told.closed.isEmpty());
        QVERIFY(one->isOpen());
        kvm.restsThoughWatched = false;
        QTRY_COMPARE(told.pictures.size(), 1);
        QVERIFY(told.closed.isEmpty());
    }

    void threeFailuresInARowEndTheLive()
    {
        MadeUpKvm kvm;
        const ChangingScreen screen(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(told.pictures.size() >= 1);
        // Two failures and then a picture, twice over: four failures, and never three in a row.
        const Answered ownError(500, "oops"_ba, "text/plain"_ba);
        for (int round = 0; round < 2; ++round) {
            kvm.first[onePicture()] = {ownError, ownError};
            QTRY_VERIFY(kvm.first[onePicture()].isEmpty());
            const qsizetype had = told.pictures.size();
            QTRY_VERIFY(told.pictures.size() > had);
            QVERIFY(told.closed.isEmpty());
        }
        // Three in a row: over, with the driver's code and sentence for the last.
        kvm.instead.insert(onePicture(), ownError);
        QTRY_COMPARE(told.closed.size(), 1);
        QCOMPARE(told.closed[0], (QStringList{u"bad_response"_s, u"Snapshot failed (HTTP 500)"_s}));
        QVERIFY(!one->isOpen());
        QCOMPARE(live.watchers(), 0);
        // And the KVM is asked for nothing more.
        const qsizetype heard = kvm.stand->asked.size();
        QTest::qWait(1200);
        QCOMPARE(kvm.stand->asked.size(), heard);
        QCOMPARE(told.closed.size(), 1);
    }

    void aKvmThatGoesAwayUnderItEndsTheLive()
    {
        MadeUpKvm kvm;
        const ChangingScreen screen(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(told.pictures.size() >= 1);
        // It falls over at every question from now on.
        Answered fallsOver;
        fallsOver.hangUp = true;
        kvm.instead.insert(onePicture(), fallsOver);
        kvm.instead.insert("/api/streamer"_ba, fallsOver);
        QTRY_COMPARE_WITH_TIMEOUT(told.closed.size(), 1, 10000);
        QCOMPARE(told.closed[0].at(0), u"dropped"_s);
        QVERIFY(!one->isOpen());
    }

    void aKvmThatIsNotThereEndsTheLiveBeforeItBegan()
    {
        const NobodyThere nobody;
        KvmdDevice::Where where;
        where.address = QUrl(nobody.address());
        KvmdDevice device(where, u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.closed, (QList<QStringList>{{u"unreachable"_s, u"Device is unreachable"_s}}));
        QVERIFY(!one->isOpen());
        QVERIFY(told.pictures.isEmpty());
        // That is not a KVM that said it serves no stream.
        QVERIFY(!live.servesNoStream());
    }

    void aStreamThatEndsIsAskedForAgainAfterAWaitThatGrowsAndItsPicturesAreAskedForMeanwhile()
    {
        // (Turned round. This was "asked for again, once", and showed a stream that ended after a
        // picture asked for again at once, each time: at the pace of a KVM's answers, without
        // end. And where the one that was asked for again gave none, the live was over.)
        // A KVM that sends one picture and hangs up, each time it is asked for its stream.
        MadeUpKvm kvm;
        const QByteArray streamed = jpeg(1920, 1080, 400);
        const Answered onePictureAndTheEnd(200, boundary() + part(streamed), kind());
        kvm.instead.insert(place(), onePictureAndTheEnd);
        const ChangingScreen screen(kvm);
        const StreamAskings askings(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive::Pace pace = brisk();
        pace.streamAgainMs = 300;
        pace.streamAgainLongestMs = 1200;
        KvmdLive live(device, pace);
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY_WITH_TIMEOUT(askings.at.size() >= 5, 20'000);
        // Asked for again after 300 ms, then 600, then 1200, and 1200 from then on: never sooner,
        // and the last no later than the longest wait allows.
        const QList<int> waits{300, 600, 1200, 1200};
        for (qsizetype index = 0; index < waits.size(); ++index) {
            const qint64 between = askings.at[index + 1] - askings.at[index];
            QVERIFY2(between >= waits[index] - 40,
                     qPrintable(u"asking %1 came %2 ms after the one before"_s.arg(index + 2).arg(between)));
        }
        QVERIFY2(askings.at[4] - askings.at[3] < 2 * pace.streamAgainLongestMs - 200,
                 qPrintable(QString::number(askings.at[4] - askings.at[3])));
        // Each of its streams gave its picture. And meanwhile its pictures were asked for one
        // after the other, as where there is no stream: the picture did not stand still.
        QVERIFY(told.data().count(streamed) >= 4);
        QVERIFY2(kvm.asked(onePicture()) >= 10, qPrintable(QString::number(kvm.asked(onePicture()))));
        QVERIFY2(told.pictures.size() >= 14, qPrintable(QString::number(told.pictures.size())));
        for (qsizetype index = 0; index < told.pictures.size(); ++index) {
            QCOMPARE(told.pictures[index].counter, static_cast<quint32>(index + 1));
        }
        // A stream that ends is not a KVM without one, and not the end of the live.
        QVERIFY(!live.servesNoStream());
        QVERIFY(told.closed.isEmpty());

        // Its stream holds now. From its first picture on nothing is asked for one after the other.
        const QByteArray next = jpeg(1920, 1080, 500);
        kvm.instead.insert(place(), streaming(part(streamed)));
        const qsizetype streamsBefore = askings.at.size();
        QTRY_VERIFY_WITH_TIMEOUT(askings.at.size() > streamsBefore && kvm.stand->isStreaming(), 5000);
        QTRY_COMPARE(told.data().constLast(), streamed);
        QTest::qWait(200);
        const int asked = kvm.asked(onePicture());
        // It holds for as long as the longest wait, which makes it a good one: when it ends
        // after that, it is asked for again after the first wait, and not the longest.
        QTest::qWait(pace.streamAgainLongestMs + 200);
        QCOMPARE(kvm.asked(onePicture()), asked);
        QVERIFY(kvm.stand->sendMore(part(next)));
        QTRY_COMPARE(told.data().constLast(), next);
        kvm.instead.insert(place(), onePictureAndTheEnd);
        const qsizetype had = askings.at.size();
        QElapsedTimer since;
        since.start();
        kvm.stand->endStream();
        QTRY_VERIFY_WITH_TIMEOUT(askings.at.size() > had, 5000);
        QVERIFY2(since.elapsed() >= pace.streamAgainMs - 40 && since.elapsed() < pace.streamAgainLongestMs - 200,
                 qPrintable(QString::number(since.elapsed())));
        QVERIFY(told.closed.isEmpty());
    }

    void thatAKvmServesNoStreamIsRememberedForATimeAndNotForGood()
    {
        // (The made-up KVM has nothing at the stream's place, as long as no test says otherwise.)
        MadeUpKvm kvm;
        const ChangingScreen screen(kvm);
        const StreamAskings askings(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive::Pace pace = brisk();
        pace.noStreamForMs = 900;
        KvmdLive live(device, pace);
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(live.servesNoStream());
        QTRY_VERIFY(told.pictures.size() >= 2);
        QCOMPARE(kvm.asked(place()), 1);
        // While that is remembered its stream is not asked for. When its time is over, it is:
        // once, and the same answer is remembered for as long again.
        QTest::qWait(pace.noStreamForMs / 2);
        QCOMPARE(kvm.asked(place()), 1);
        QTRY_COMPARE_WITH_TIMEOUT(kvm.asked(place()), 2, 5000);
        // (The times are noted by one who looks every few milliseconds: noted by now.)
        QTRY_VERIFY(askings.at.size() >= 2);
        QVERIFY2(askings.at[1] - askings.at[0] >= pace.noStreamForMs - 60,
                 qPrintable(QString::number(askings.at[1] - askings.at[0])));
        QTRY_VERIFY(live.servesNoStream());
        const qsizetype had = told.pictures.size();
        QTRY_VERIFY(told.pictures.size() >= had + 2);
        // The KVM has a stream from now on (kvmd was started anew, its maker gave it one). The
        // next time it is asked, there it is: its pictures come by it, and nothing is remembered.
        const QByteArray streamed = jpeg(1920, 1080, 700);
        kvm.instead.insert(place(), streaming(part(streamed)));
        QTRY_VERIFY_WITH_TIMEOUT(told.data().contains(streamed), 5000);
        QCOMPARE(kvm.asked(place()), 3);
        QTRY_VERIFY(askings.at.size() >= 3);
        QVERIFY2(askings.at[2] - askings.at[1] >= pace.noStreamForMs - 60,
                 qPrintable(QString::number(askings.at[2] - askings.at[1])));
        QVERIFY(!live.servesNoStream());
        QVERIFY(told.closed.isEmpty());
    }

    void whatIsRememberedOfAKvmsStreamIsForgottenWhenItsDriverSignsInAnew()
    {
        MadeUpKvm kvm;
        const ChangingScreen screen(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive::Pace pace = brisk();
        pace.streamAgainMs = 200;
        // (Remembered for a minute, as in the program: far longer than this test takes.)
        KvmdLive live(device, pace);
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(live.servesNoStream());
        QTRY_VERIFY(told.pictures.size() >= 2);
        QCOMPARE(kvm.asked(place()), 1);
        QCOMPARE(kvm.signIns, 1);
        // kvmd is started anew: it knows no pass it gave before, and has its stream. The next
        // question for a picture finds that out and signs in anew: with that, what was
        // remembered of its stream is forgotten, and the stream is asked for a moment later.
        const QByteArray streamed = jpeg(1920, 1080, 700);
        kvm.instead.insert(place(), streaming(part(streamed)));
        kvm.passes.clear();
        QTRY_VERIFY_WITH_TIMEOUT(told.data().contains(streamed), 5000);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.asked(place()), 2);
        QVERIFY(!live.servesNoStream());
        QVERIFY(told.closed.isEmpty());
    }

    void anAnswerThatSaysTheKvmItselfIsBrokenIsNotRememberedAsNoStream()
    {
        // The stream's place answers with an error of the KVM's own, once; after that its stream
        // is there.
        MadeUpKvm kvm;
        const QByteArray streamed = jpeg(1920, 1080, 700);
        kvm.first[place()].append(Answered(500, "oops"_ba, "text/plain"_ba));
        kvm.instead.insert(place(), streaming(part(streamed)));
        const ChangingScreen screen(kvm);
        const StreamAskings askings(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive::Pace pace = brisk();
        pace.streamAgainMs = 500;
        KvmdLive live(device, pace);
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        // Its pictures are asked for one after the other meanwhile, and nothing is remembered.
        QTRY_VERIFY(!told.pictures.isEmpty());
        QVERIFY(!live.servesNoStream());
        QVERIFY(told.pictures[0].data != streamed);
        // Its stream is asked for again after the first wait, and is there.
        QTRY_VERIFY_WITH_TIMEOUT(told.data().contains(streamed), 5000);
        QCOMPARE(kvm.asked(place()), 2);
        QTRY_VERIFY(askings.at.size() >= 2);
        QVERIFY2(askings.at[1] - askings.at[0] >= pace.streamAgainMs - 40,
                 qPrintable(QString::number(askings.at[1] - askings.at[0])));
        QVERIFY(!live.servesNoStream());
        QVERIFY(told.closed.isEmpty());
    }

    void aKvmWhoseGatewayAnswersForItsStreamForAMomentHasItsStreamAgainAfterThat()
    {
        // A KVM is had live by its stream. kvmd is started anew, and what stands before it
        // answers for the stream with a page of its own for a moment.
        MadeUpKvm kvm;
        const QByteArray before = jpeg(1920, 1080, 600);
        const QByteArray after = jpeg(1920, 1080, 700);
        kvm.instead.insert(place(), streaming(part(before)));
        const ChangingScreen screen(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive::Pace pace = brisk();
        pace.streamAgainMs = 200;
        pace.noStreamForMs = 800;
        KvmdLive live(device, pace);
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        kvm.instead.insert(place(), gatewaysPage());
        kvm.stand->cutStream();
        // Its pictures come one after the other meanwhile, which shows its video awake: so the
        // page says that there is no stream here, and that is remembered. The live goes on.
        QTRY_VERIFY(live.servesNoStream());
        QTRY_VERIFY(told.pictures.size() >= 3);
        QVERIFY(told.closed.isEmpty());
        // For a time, and not for good: kvmd is back, and so is the stream.
        kvm.instead.insert(place(), streaming(part(after)));
        QTRY_VERIFY_WITH_TIMEOUT(told.data().contains(after), 6000);
        QVERIFY(!live.servesNoStream());
        QCOMPARE(kvm.asked(place()), 3);
        QVERIFY(told.closed.isEmpty());
    }

    void aStreamWhosePartsAreNoPicturesHasGivenNothingAndIsEndedWhenAPictureHasHadItsTime_data()
    {
        QTest::addColumn<bool>("gaveFirst");
        QTest::newRow("from its beginning") << false;
        QTest::newRow("after a picture had come") << true;
    }

    void aStreamWhosePartsAreNoPicturesHasGivenNothingAndIsEndedWhenAPictureHasHadItsTime()
    {
        // Parts of one byte, each with a right length, every tenth of a second: the stream is
        // never silent, and each part is whole at once, so both of the stream's own limits are
        // kept. No picture comes of it, and the live would stand still for ever.
        QFETCH(bool, gaveFirst);
        MadeUpKvm kvm;
        const QByteArray good = jpeg(1920, 1080, 300);
        const QByteArray none = "x"_ba;
        kvm.instead.insert(place(), streaming(part(gaveFirst ? good : none)));
        const ChangingScreen screen(kvm);
        KvmdDevice::Pace slow = quick();
        slow.streamPictureMs = 900;
        KvmdDevice device(kvm.where(), u"secret"_s, slow);
        KvmdLive::Pace pace = brisk();
        pace.streamAgainMs = 20'000;
        KvmdLive live(device, pace);
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(kvm.stand->isStreaming());
        const qsizetype had = gaveFirst ? 1 : 0;
        QTRY_COMPARE(told.pictures.size(), had);
        QTimer parts;
        connect(&parts, &QTimer::timeout, &parts, [&kvm, &none] { kvm.stand->sendMore(part(none)); });
        parts.start(100);
        QElapsedTimer since;
        since.start();
        // For as long as a stream may take over one picture nothing else is asked for.
        QTest::qWait(slow.streamPictureMs - 400);
        QCOMPARE(kvm.asked(onePicture()), 0);
        QCOMPARE(told.pictures.size(), had);
        QVERIFY(kvm.stand->isStreaming());
        // Then it is ended as a stream that sent no picture, and the KVM's pictures are asked
        // for one after the other.
        QTRY_VERIFY_WITH_TIMEOUT(told.pictures.size() >= had + 2, 6000);
        QVERIFY2(since.elapsed() >= slow.streamPictureMs - 200, qPrintable(QString::number(since.elapsed())));
        QTRY_VERIFY(!kvm.stand->isStreaming());
        QVERIFY(!live.servesNoStream());
        QVERIFY(told.closed.isEmpty());
    }

    void aStreamThatOnlyRestsIsAskedForOnceMoreWhenAPictureHasCome_data()
    {
        QTest::addColumn<Answered>("resting");
        QTest::newRow("as kvmd says it") << refusal(503, u"UnavailableError"_s);
        // A stock KVM, whenever nobody watches: the program that makes the stream is not
        // running, and what stands before kvmd answers for it.
        QTest::newRow("as its gateway says it") << gatewaysPage();
    }

    void aStreamThatOnlyRestsIsAskedForOnceMoreWhenAPictureHasCome()
    {
        // Its video rests when the stream is first asked for; a moment later the stream is there.
        QFETCH(Answered, resting);
        MadeUpKvm kvm;
        kvm.first[place()].append(resting);
        const QByteArray streamed = jpeg(1920, 1080, 700);
        const QByteArray next = jpeg(1920, 1080, 800);
        kvm.instead.insert(place(), streaming(part(streamed)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        // A picture by asking first, which is what there is; then its stream.
        QTRY_VERIFY(told.data().contains(streamed));
        QCOMPARE(told.data().constFirst(), jpeg(1920, 1080));
        QCOMPARE(kvm.asked(place()), 2);
        QVERIFY(!live.servesNoStream());
        // From then on nothing is asked for one after the other. (An asking that was on its way
        // when the stream's first picture came has arrived by the time this counts.)
        QTest::qWait(200);
        const int asked = kvm.asked(onePicture());
        QVERIFY(asked >= 1);
        QVERIFY(kvm.stand->sendMore(part(next)));
        QTRY_COMPARE(told.data().constLast(), next);
        QTest::qWait(400);
        QCOMPARE(kvm.asked(onePicture()), asked);
        QVERIFY(told.closed.isEmpty());
        // Counted on through both ways.
        for (qsizetype index = 0; index < told.pictures.size(); ++index) {
            QCOMPARE(told.pictures[index].counter, static_cast<quint32>(index + 1));
        }
    }

    void aPictureThatCameLongAgoIsNotShownAsNewNorTakenForAVideoThatIsAwake()
    {
        // A picture as captured comes after whoever asked for it had given up: nobody was handed
        // it, and the driver keeps it. Then nobody looks for a while, and the KVM's video rests.
        MadeUpKvm kvm;
        KvmdDevice::Pace pace = quick();
        KvmdDevice device(kvm.where(), u"secret"_s, pace);
        device.look();
        QTRY_VERIFY(!device.isStale());
        const QByteArray old = jpeg(1920, 1080, 111);
        kvm.picture = old;
        kvm.pictureAfterMs = pace.patienceMs + 200;
        std::optional<std::string> told0;
        device.askNewSnapshot(core::kvmd::longestPictureSide, [&told0](const base::Result<core::Snapshot>& came) {
            told0 = came ? std::string("a picture") : came.error().message;
        });
        QTRY_VERIFY(told0.has_value());
        QCOMPARE(*told0, std::string("The KVM is slow to answer"));
        QTest::qWait(200 + 100 + pace.freshMs + 300);
        QCOMPARE(kvm.pictureAsks.size(), 1);
        // Now it is had live. What stands before it answers for its stream with a page until
        // somebody has woken its video by taking a picture, as a stock KVM's does; from then on
        // its stream is there.
        const QByteArray streamed = jpeg(1920, 1080, 700);
        kvm.picture = jpeg(1920, 1080, 222);
        kvm.pictureAfterMs = 0;
        kvm.pictured = false;
        kvm.untilPictured.insert(place(), gatewaysPage());
        kvm.instead.insert(place(), streaming(part(streamed)));
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        // The picture from before is handed to nobody: what is shown first is asked of the KVM
        // now. And only that says that the video is awake: the stream is asked for once more
        // when it has come, and is there.
        QTRY_VERIFY(!told.pictures.isEmpty());
        QCOMPARE(told.pictures[0].data, kvm.picture);
        QTRY_VERIFY(told.data().contains(streamed));
        QVERIFY(!told.data().contains(old));
        QCOMPARE(kvm.asked(place()), 2);
        QVERIFY(!live.servesNoStream());
        QVERIFY(told.closed.isEmpty());
    }

    // --- shared by all who have it live ---

    void twoWhoHaveItLiveShareOneStream()
    {
        MadeUpKvm kvm;
        const QByteArray first = jpeg(1920, 1080, 100);
        const QByteArray second = jpeg(1920, 1080, 200);
        const QByteArray third = jpeg(1920, 1080, 300);
        kvm.instead.insert(place(), streaming(part(first)));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        std::unique_ptr<Live> one = live.open({800, 10});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        std::unique_ptr<Live> two = live.open({1920, 25});
        const Told other(two.get());
        QCOMPARE(live.watchers(), 2);
        // Both get every picture from then on, each counted for itself and told the size.
        QVERIFY(kvm.stand->sendMore(part(second)));
        QTRY_COMPARE(other.pictures.size(), 1);
        QTRY_COMPARE(told.pictures.size(), 2);
        QCOMPARE(told.data(), (QList<QByteArray>{first, second}));
        QCOMPARE(other.data(), (QList<QByteArray>{second}));
        QCOMPARE(other.counters(), (QList<quint32>{1}));
        QCOMPARE(other.videos, (QList<Live::Video>{{false, 1920, 1080}}));
        QCOMPARE(told.videos.size(), 1);
        QCOMPARE(other.opened, 1);
        // One stream, and one sign-in: two windows are not two loads on a KVM.
        QCOMPARE(kvm.asked(place()), 1);
        QCOMPARE(kvm.signIns, 1);

        // One closes, and the other goes on.
        one->close();
        QVERIFY(!one->isOpen());
        QCOMPARE(live.watchers(), 1);
        QVERIFY(kvm.stand->sendMore(part(third)));
        QTRY_COMPARE(other.pictures.size(), 2);
        QCOMPARE(told.pictures.size(), 2);
        QVERIFY(kvm.stand->isStreaming());
        QCOMPARE(kvm.asked(place()), 1);
        // The last closes: its stream is given up, and the KVM is asked for nothing more.
        two->close();
        QTRY_VERIFY(!kvm.stand->isStreaming());
        const qsizetype heard = kvm.stand->asked.size();
        QTest::qWait(700);
        QCOMPARE(kvm.stand->asked.size(), heard);
        // Whoever closed is told nothing of it.
        QVERIFY(told.closed.isEmpty());
        QVERIFY(other.closed.isEmpty());
    }

    void whatAllOfThemNeedIsTheWidestAndTheFastest()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        QCOMPARE(live.need(), (Live::Need{0, 0}));
        const std::unique_ptr<Live> one = live.open({800, 10});
        QCOMPARE(live.need(), (Live::Need{800, 10}));
        std::unique_ptr<Live> two = live.open({1920, 5});
        QCOMPARE(live.need(), (Live::Need{1920, 10}));
        two->setNeed({1280, 25});
        QCOMPARE(live.need(), (Live::Need{1280, 25}));
        one->setNeed({2560, 2});
        QCOMPARE(live.need(), (Live::Need{2560, 25}));
        // Whoever goes takes its need along.
        two.reset();
        QCOMPARE(live.need(), (Live::Need{2560, 2}));
    }

    void aPictureThatIsTheOneBeforeGoesOnlyToWhoeverHasHadNone()
    {
        // A screen on which nothing changes, of a KVM that is asked for one picture after the
        // other.
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        // The KVM is asked on, and gives the same picture each time: handed on once.
        const int asked = kvm.asked(onePicture());
        QTRY_VERIFY(kvm.asked(onePicture()) >= asked + 2);
        QCOMPARE(told.pictures.size(), 1);
        // A second who comes has had none, and gets it; the first not a second time.
        const std::unique_ptr<Live> two = live.open({1920, 30});
        const Told other(two.get());
        QTRY_COMPARE(other.pictures.size(), 1);
        QCOMPARE(other.pictures[0].data, told.pictures[0].data);
        QCOMPARE(other.videos, (QList<Live::Video>{{false, 1920, 1080}}));
        QCOMPARE(told.pictures.size(), 1);
        // The screen changes: both get the new one.
        kvm.picture = jpeg(1920, 1080, 999);
        QTRY_COMPARE(told.pictures.size(), 2);
        QTRY_COMPARE(other.pictures.size(), 2);
        QCOMPARE(told.pictures[1].data, kvm.picture);
        QCOMPARE(told.counters(), (QList<quint32>{1, 2}));
        QCOMPARE(other.counters(), (QList<quint32>{1, 2}));
    }

    void whenNobodyHasItLiveNothingIsAskedOfTheKvm()
    {
        MadeUpKvm kvm;
        const ChangingScreen screen(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        // Nothing before anybody asks.
        QTest::qWait(200);
        QVERIFY(kvm.stand->asked.isEmpty());
        std::unique_ptr<Live> one = live.open({1920, 30});
        std::unique_ptr<Live> two = live.open({1920, 30});
        const Told told(one.get());
        const Told other(two.get());
        QTRY_VERIFY(told.pictures.size() >= 2);
        QTRY_VERIFY(other.pictures.size() >= 2);
        // One is thrown away, and the other still gets its pictures.
        one.reset();
        const qsizetype had = other.pictures.size();
        QTRY_VERIFY(other.pictures.size() >= had + 2);
        // The last is: what was asked is answered to nobody, and nothing more is asked.
        two.reset();
        QCOMPARE(live.watchers(), 0);
        QTest::qWait(500);
        const qsizetype heard = kvm.stand->asked.size();
        QTest::qWait(900);
        QCOMPARE(kvm.stand->asked.size(), heard);
    }

    void whoeverIsHandedAPictureMayLetGoThere()
    {
        MadeUpKvm kvm;
        const QByteArray picture = jpeg(1920, 1080, 100);
        kvm.instead.insert(place(), streaming());
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        std::unique_ptr<Live> one = live.open({1920, 30});
        std::unique_ptr<Live> two = live.open({1920, 30});
        std::unique_ptr<Live> three = live.open({1920, 30});
        const Told told(one.get());
        const Told other(two.get());
        const Told third(three.get());
        // The first to be handed a picture closes itself, and throws the second away.
        connect(one.get(), &Live::pictureCame, this, [&] {
            one->close();
            two.reset();
        });
        QTRY_VERIFY(kvm.stand->isStreaming());
        QVERIFY(kvm.stand->sendMore(part(picture)));
        QTRY_COMPARE(third.pictures.size(), 1);
        QCOMPARE(told.pictures.size(), 1);
        QVERIFY(other.pictures.isEmpty());
        QCOMPARE(live.watchers(), 1);
        // And the last, handed the next, lets go of the KVM altogether.
        connect(three.get(), &Live::pictureCame, this, [&] { three->close(); });
        QVERIFY(kvm.stand->sendMore(part(picture) + part(picture)));
        QTRY_VERIFY(!kvm.stand->isStreaming());
        QCOMPARE(third.pictures.size(), 2);
        QCOMPARE(live.watchers(), 0);
    }

    // --- keyboard and mouse ---

    void keyboardAndMouseGoToTheKvmFromTheOneWhoHoldsThem()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        std::unique_ptr<Live> one = live.open({1920, 30});
        std::unique_ptr<Live> two = live.open({1280, 30});
        const Told first(one.get());
        const Told second(two.get());
        QTRY_COMPARE(first.pictures.size(), 1);
        // No line for them until somebody asks: the one line there is keeps the video awake.
        QTRY_COMPARE(kvm.stand->live.lines, 1);
        QVERIFY2(kvm.stand->live.path.startsWith("/api/ws?stream=1"), kvm.stand->live.path.constData());
        QVERIFY(!one->hasControl());
        one->key(keyA, true);
        one->key(keyA, false);
        QTest::qWait(200);
        QCOMPARE(kvm.stand->live.lines, 1);
        QVERIFY(pressedOn(kvm).isEmpty());

        // Asked for: the driver's line for them, signed in as the driver is. Held once the KVM
        // has let the line in, and not before.
        one->askControl(true);
        QVERIFY(!one->hasControl());
        QVERIFY(first.control.isEmpty());
        QTRY_COMPARE(first.control, (QList<QStringList>{{u"on"_s, QString(), QString()}}));
        QVERIFY(one->hasControl());
        QCOMPARE(kvm.stand->live.lines, 2);
        // (Signed in as the driver is, by its pass in the cookie; in the address only to a KVM known to be GL.iNet's,
        // which nothing has said of this one yet: test_kvmd_device, theSessionGoesInTheAddressOnlyToAGlinetKvm.)
        QCOMPARE(kvm.stand->live.path, "/api/ws?stream=0"_ba);
        QCOMPARE(kvm.stand->live.headers.value("cookie"), "auth_token=pass1"_ba);
        QCOMPARE(kvm.signIns, 1);
        // Asked for once more by the one who has them: nothing.
        one->askControl(true);
        one->key(keyA, true);
        one->button(1, true);
        QTRY_COMPARE(pressedOn(kvm), (QList<QJsonObject>{keyOf(u"KeyA"_s, true), buttonOf(u"left"_s, true)}));

        // A second who asks is told no, and the KVM is asked nothing for it. Nothing that one
        // presses goes anywhere, and the first keeps what it holds.
        two->askControl(true);
        QVERIFY(second.control.isEmpty());
        QTRY_COMPARE(second.control.size(), 1);
        QCOMPARE(second.control[0].at(0), u"off"_s);
        QCOMPARE(second.control[0].at(1), u"in_use"_s);
        QCOMPARE(second.control[0].at(2),
                 u"Keyboard and mouse of this machine are in use in another window. Release them there first."_s);
        QVERIFY(!two->hasControl());
        two->key(keyB, true);
        two->button(2, true);
        two->pointerAt(100, 100);
        two->pointerBy(3, 3);
        two->wheel(0, 1);
        two->releaseAll();
        two->askControl(false);
        QTest::qWait(250);
        QCOMPARE(pressedOn(kvm).size(), 2);
        QCOMPARE(kvm.stand->live.lines, 2);
        QVERIFY(one->hasControl());
        // (Its giving back is answered, though it held nothing: every asking and every giving
        // back gets a word of its own. This held that it gets none.)
        QCOMPARE(second.control.size(), 2);
        QCOMPARE(second.control[1], (QStringList{u"off"_s, QString(), QString()}));
        QCOMPARE(first.control.size(), 1);

        // Given back: what is down is let go first (the button, then the key), then the line
        // goes, and that is said a turn later.
        one->askControl(false);
        QVERIFY(!one->hasControl());
        QCOMPARE(first.control.size(), 1);
        QTRY_COMPARE(pressedOn(kvm), (QList<QJsonObject>{keyOf(u"KeyA"_s, true), buttonOf(u"left"_s, true),
                                                         buttonOf(u"left"_s, false), keyOf(u"KeyA"_s, false)}));
        QTRY_VERIFY(!kvm.stand->live.isConnected());
        QTRY_COMPARE(first.control.size(), 2);
        QCOMPARE(first.control[1], (QStringList{u"off"_s, QString(), QString()}));
        // What that one presses now goes nowhere.
        one->key(enter, true);
        one->key(enter, false);

        // Now the other can have them: a line of its own.
        two->askControl(true);
        QTRY_COMPARE(second.control.size(), 3);
        QCOMPARE(second.control[2], (QStringList{u"on"_s, QString(), QString()}));
        QCOMPARE(kvm.stand->live.lines, 3);
        two->key(keyB, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 5);
        QCOMPARE(pressedOn(kvm).constLast(), keyOf(u"KeyB"_s, true));
        // Closed while a key is down: let go first, and whoever closed it is told nothing.
        two->close();
        QTRY_COMPARE(pressedOn(kvm).size(), 6);
        QCOMPARE(pressedOn(kvm).constLast(), keyOf(u"KeyB"_s, false));
        QTRY_VERIFY(!kvm.stand->live.isConnected());
        QTest::qWait(150);
        QCOMPARE(second.control.size(), 3);
        QVERIFY(second.closed.isEmpty());

        // And thrown away while one is down.
        one->askControl(true);
        QTRY_COMPARE(first.control.size(), 3);
        QCOMPARE(kvm.stand->live.lines, 4);
        one->key(enter, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 7);
        one.reset();
        QTRY_COMPARE(pressedOn(kvm).size(), 8);
        QCOMPARE(pressedOn(kvm).constLast(), keyOf(u"Enter"_s, false));
        QTRY_VERIFY(!kvm.stand->live.isConnected());
        // The pictures went on through all of it, until the last who had it live was gone.
        QVERIFY(first.closed.isEmpty());
        QTRY_VERIFY(!kvm.stand->isStreaming());
        // Everything could be let go each time: nobody was told that something may still be down.
        QCOMPARE(first.leftDown, 0);
        QCOMPARE(second.leftDown, 0);
    }

    void keyboardAndMouseGivenBackBeforeTheirLineWasThereAskTheKvmForNone()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        QTRY_COMPARE(kvm.stand->live.lines, 1);
        // Asked for and given back in one breath: the line that the driver makes a moment later
        // is nobody's, and never reaches the KVM. Two words come for the two that were said, in
        // their order: a no for the asking, which is over with the giving back, and the word for
        // the giving back. (One word came for both, and whoever asked could not tell whether the
        // asking's was still to come: the button for keyboard and mouse stayed inert for seconds.)
        one->askControl(true);
        one->askControl(false);
        QVERIFY(told.control.isEmpty());
        QTest::qWait(400);
        QCOMPARE(kvm.stand->live.lines, 1);
        QCOMPARE(told.control,
                 (QList<QStringList>{
                     {u"off"_s, u"given_up"_s, u"Keyboard and mouse were given back before the KVM had given them."_s},
                     {u"off"_s, QString(), QString()}}));
        QVERIFY(!one->hasControl());
        // Asked for again, they are given.
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 3);
        QCOMPARE(told.control[2].at(0), u"on"_s);
        QCOMPARE(kvm.stand->live.lines, 2);
        // Given back while they are held: one word, as before.
        one->askControl(false);
        QTRY_COMPARE(told.control.size(), 4);
        QCOMPARE(told.control[3], (QStringList{u"off"_s, QString(), QString()}));
        // And once more by the one who now holds nothing: one word again, and no more than one.
        one->askControl(false);
        QTRY_COMPARE(told.control.size(), 5);
        QCOMPARE(told.control[4], (QStringList{u"off"_s, QString(), QString()}));
        QTest::qWait(200);
        QCOMPARE(told.control.size(), 5);
    }

    void aLineForKeyboardAndMouseThatEndsByItselfIsSaidAndThePicturesGoOn()
    {
        MadeUpKvm kvm;
        const QByteArray next = jpeg(1920, 1080, 321);
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        one->key(keyA, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 1);
        // The KVM drops the line: said with the line's own code and sentence.
        kvm.stand->live.fallOver();
        QTRY_COMPARE(told.control.size(), 2);
        QCOMPARE(told.control[1], (QStringList{u"off"_s, u"dropped"_s, u"The device closed the connection"_s}));
        QVERIFY(!one->hasControl());
        // The live is there, and its pictures come.
        QVERIFY(one->isOpen());
        QVERIFY(kvm.stand->sendMore(part(next)));
        QTRY_COMPARE(told.pictures.size(), 2);
        QVERIFY(told.closed.isEmpty());
        // And keyboard and mouse can be asked for again.
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 3);
        QCOMPARE(told.control[2].at(0), u"on"_s);
        QVERIFY(one->hasControl());
    }

    void whereTheLineEndsByItselfWithAKeyDownTheHolderIsToldThatItMayStillBeDown()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const std::unique_ptr<Live> two = live.open({1920, 30});
        const Told told(one.get());
        const Told other(two.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        // The line falls while nothing is down: nothing is left down, and nothing is said of it.
        QTest::qWait(450);
        kvm.stand->live.fallOver();
        QTRY_COMPARE(told.control.size(), 2);
        QCOMPARE(told.leftDown, 0);
        // It falls while a key is down, which cannot be let go from here any more: the line says
        // so, and that is passed on to whoever held keyboard and mouse, before they are told gone.
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 3);
        one->key(keyA, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 1);
        kvm.stand->live.fallOver();
        QTRY_COMPARE(told.control.size(), 4);
        QCOMPARE(told.control[3], (QStringList{u"off"_s, u"dropped"_s, u"The device closed the connection"_s}));
        QCOMPARE(told.leftDown, 1);
        QCOMPARE(told.said, (QStringList{u"on"_s, u"off"_s, u"on"_s, u"left down"_s, u"off"_s}));
        // Nobody else is told, and the live goes on.
        QCOMPARE(other.leftDown, 0);
        QVERIFY(told.closed.isEmpty());
        QVERIFY(one->isOpen());
    }

    void whereTheLineTakesNoMoreWhileItIsLetGoOfTheHolderIsToldThatSomethingMayStillBeDown_data()
    {
        QTest::addColumn<QString>("how");
        QTest::newRow("keyboard and mouse are given back") << u"given back"_s;
        QTest::newRow("the live is ended under whoever holds them") << u"ended"_s;
        QTest::newRow("whoever holds them closes the live") << u"closed"_s;
        QTest::newRow("whoever holds them throws the live away") << u"thrown away"_s;
    }

    void whereTheLineTakesNoMoreWhileItIsLetGoOfTheHolderIsToldThatSomethingMayStillBeDown()
    {
        QFETCH(QString, how);
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        one->key(keyA, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 1);
        // The KVM drops the line over something it has not read, which resets it; and keyboard
        // and mouse go before the program has got round to hearing of that. The "up" finds a
        // line that takes no more: it is cut in the letting go, which says nothing of an end.
        one->button(1, true);
        kvm.stand->live.fallOver();
        QThread::msleep(50);
        if (how == u"given back"_s) {
            one->askControl(false);
        } else if (how == u"ended"_s) {
            live.end(base::Error{"not_found", "Unknown device"});
        } else if (how == u"closed"_s) {
            one->close();
        } else {
            one.reset();
        }
        // Never from inside the call; a turn of the loop later.
        QCOMPARE(told.leftDown, 0);
        if (how == u"thrown away"_s) {
            // Nobody is left to tell.
            QTest::qWait(250);
            QCOMPARE(told.said, (QStringList{u"on"_s}));
            return;
        }
        QTRY_COMPARE(told.leftDown, 1);
        QTest::qWait(200);
        if (how == u"given back"_s) {
            QCOMPARE(told.said, (QStringList{u"on"_s, u"left down"_s, u"off"_s}));
            QCOMPARE(told.control[1], (QStringList{u"off"_s, QString(), QString()}));
            QVERIFY(one->isOpen());
        } else if (how == u"ended"_s) {
            QCOMPARE(told.said, (QStringList{u"on"_s, u"left down"_s, u"off"_s, u"closed"_s}));
            QCOMPARE(told.control[1], (QStringList{u"off"_s, u"not_found"_s, u"Unknown device"_s}));
        } else {
            // Whoever closed it is told that, and nothing else.
            QCOMPARE(told.said, (QStringList{u"on"_s, u"left down"_s}));
        }
        // Neither went up on the machine as far as the made-up KVM heard.
        QVERIFY(!pressedOn(kvm).contains(keyOf(u"KeyA"_s, false)));
        QVERIFY(!pressedOn(kvm).contains(buttonOf(u"left"_s, false)));
    }

    void whereALineThatWasLetGoIsResetWhenItsTimeIsUpWhoeverHeldItIsToldLate_data()
    {
        QTest::addColumn<QString>("how");
        QTest::newRow("keyboard and mouse are given back") << u"given back"_s;
        QTest::newRow("whoever holds them closes the live") << u"closed"_s;
        QTest::newRow("whoever holds them throws the live away") << u"thrown away"_s;
    }

    void whereALineThatWasLetGoIsResetWhenItsTimeIsUpWhoeverHeldItIsToldLate()
    {
        // A key is down, and the way to the KVM dies: nothing is read there any more, and the
        // line is never ended from there. Before the line's own watch has noticed, keyboard and
        // mouse go. The "up" is said, Windows takes it, and all counts as let go: nobody is told
        // of anything left down. Two seconds later the KVM has not ended the line, and it is
        // reset with the "up" still in it. Whoever held keyboard and mouse is told then that
        // something may still be down on the machine, if that one is still there.
        QFETCH(const QString, how);
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        one->key(keyA, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 1);
        kvm.stand->live.hearNoMore();
        if (how == u"given back"_s) {
            one->askControl(false);
        } else if (how == u"closed"_s) {
            one->close();
        } else {
            one.reset();
        }
        // All was let go, as far as the program saw: nothing is said of anything left down. And
        // the word for a giving back waits for the line to have gone (rule 27: what is sent next
        // waits for that word), which this one has not.
        QTest::qWait(400);
        QCOMPARE(told.leftDown, 0);
        QCOMPARE(told.said, (QStringList{u"on"_s}));
        if (how == u"thrown away"_s) {
            // Nobody is left to tell: the line is reset when its time is up, and that is all.
            QTest::qWait(2500);
            QCOMPARE(told.said, (QStringList{u"on"_s}));
            return;
        }
        // The line's time is up, and the KVM has not ended it.
        QTRY_COMPARE_WITH_TIMEOUT(told.leftDown, 1, 5000);
        QTest::qWait(200);
        if (how == u"given back"_s) {
            // (Turned round: this recorded the word for the giving back at once, and that
            // something may still be down after it, when the line was reset.)
            QCOMPARE(told.said, (QStringList{u"on"_s, u"left down"_s, u"off"_s}));
            QCOMPARE(told.control[1], (QStringList{u"off"_s, QString(), QString()}));
            QVERIFY(one->isOpen());
        } else {
            QCOMPARE(told.said, (QStringList{u"on"_s, u"left down"_s}));
        }
        QCOMPARE(told.leftDown, 1);
        // The key never went up on the machine, as far as the made-up KVM heard.
        QVERIFY(!pressedOn(kvm).contains(keyOf(u"KeyA"_s, false)));
    }

    void theWordForAGivingBackComesOnceWhatWasLetGoOfHasLanded()
    {
        // Rule 27 of the web's FOR-THE-DESKTOP-APP: whatever is sent to the machine after keyboard and
        // mouse were given back (a typed text, a key combination, a saved action) waits for the
        // letting go to have landed, or a key combination lands beside a Ctrl that is still on its
        // way up. What is sent next waits for the word for the giving back
        // (MachineControl::textAfterRelease); so that word comes once the line has gone, the KVM's
        // goodbye behind what it was told.
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        constexpr int controlLeft = 0xE0;
        one->key(controlLeft, true);
        QTRY_COMPARE(pressedOn(kvm), QList<QJsonObject>{keyOf(u"ControlLeft"_s, true)});
        // What the KVM had heard when the word came.
        QList<QJsonObject> heardThen;
        QObject probe;
        QObject::connect(one.get(), &Live::controlChanged, &probe, [&](bool on, const QString&, const QString&) {
            if (!on) {
                heardThen = pressedOn(kvm);
            }
        });
        one->askControl(false);
        QTRY_COMPARE(told.control.size(), 2);
        QCOMPARE(told.control[1], (QStringList{u"off"_s, QString(), QString()}));
        QCOMPARE(heardThen, (QList<QJsonObject>{keyOf(u"ControlLeft"_s, true), keyOf(u"ControlLeft"_s, false)}));
        // The goodbye came: nothing is said of anything left down. (One that does not come in the
        // line's time: whereALineThatWasLetGoIsResetWhenItsTimeIsUpWhoeverHeldItIsToldLate.)
        QTest::qWait(200);
        QCOMPARE(told.leftDown, 0);
        QCOMPARE(told.said, (QStringList{u"on"_s, u"off"_s}));
    }

    void aPassTheKvmRefusesOnTheLineForKeyboardAndMouseIsNotAskedWithASecondTime()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        QCOMPARE(kvm.signIns, 1);
        // kvmd was started anew, and knows no pass it gave before. Nothing is asked of the KVM
        // meanwhile (its stream is open), so the driver cannot see that: the line is refused.
        kvm.passes.clear();
        kvm.stand->handsLinesOver = false; // (the asking for the line is answered as a question is)
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        QCOMPARE(told.control[0].at(0), u"off"_s);
        QCOMPARE(told.control[0].at(1), u"auth_failed"_s);
        QCOMPARE(kvm.signIns, 1);
        // Asked for again: the driver was told which pass was refused, signs in anew, and the
        // line is asked for with the new pass.
        kvm.stand->handsLinesOver = true;
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 2);
        QCOMPARE(told.control[1].at(0), u"on"_s);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.stand->live.headers.value("cookie"), "auth_token=pass2"_ba);
        QVERIFY(told.closed.isEmpty());
    }

    void aKvmThatSignsThisProgramInAndRefusesTheLineAllTheSameIsNotSignedInToWithEveryAsking()
    {
        // Whoever is signed in may not use keyboard and mouse there: the KVM gives a pass, serves
        // everything else under it, and answers "not you" where the line is asked for.
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        QCOMPARE(kvm.signIns, 1);
        const QByteArray line = "/api/ws"_ba;
        kvm.stand->handsLinesOver = false; // (the asking for the line is answered as a question is)
        kvm.instead.insert(line, refusal(403, u"ForbiddenError"_s));
        // The first refusal may be the pass's fault, as where kvmd was started anew: the driver
        // forgets the pass, and signs in anew for the next line.
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        QCOMPARE(told.control[0], (QStringList{u"off"_s, u"auth_failed"_s,
                                               u"The device did not accept the session for keyboard and mouse"_s}));
        QCOMPARE(kvm.signIns, 1);
        // Refused again, under a pass the KVM has only just given: that is not the pass's fault.
        // It is the line that the KVM refuses, and whoever asked is told so.
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 2);
        QCOMPARE(told.control[1],
                 (QStringList{u"off"_s, u"not_allowed"_s,
                              u"The device signed this program in and refuses keyboard and mouse all the same"_s}));
        QCOMPARE(kvm.signIns, 2);
        const int linesAsked = kvm.asked(line);
        QCOMPARE(linesAsked, 2);
        // From then on a click costs the KVM nothing: no password is posted for it, and the line
        // is not asked for under that pass again for a while.
        for (int click = 0; click < 3; ++click) {
            one->askControl(true);
            QTRY_COMPARE(told.control.size(), 3 + click);
            QCOMPARE(told.control.constLast().at(1), u"not_allowed"_s);
        }
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.asked(line), linesAsked);
        // The pass is kept: everything else is served under it.
        device.look();
        QTRY_VERIFY(!device.isStale());
        QCOMPARE(device.status().state, u"online"_s);
        QCOMPARE(kvm.signIns, 2);
        // When that while is over, the line is asked for once more, under the pass that was
        // kept. Refused again, it is still the line's doing: no password goes out.
        QTest::qWait(quick().signInRetryMs + 100);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 6);
        QCOMPARE(told.control.constLast().at(1), u"not_allowed"_s);
        QCOMPARE(kvm.asked(line), linesAsked + 1);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 7);
        QCOMPARE(kvm.asked(line), linesAsked + 1);
        QCOMPARE(kvm.signIns, 2);
        // The KVM allows it after all: the line is let in under that pass.
        kvm.instead.remove(line);
        kvm.stand->handsLinesOver = true;
        QTest::qWait(quick().signInRetryMs + 100);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 8);
        QCOMPARE(told.control.constLast().at(0), u"on"_s);
        QCOMPARE(kvm.signIns, 2);
        QCOMPARE(kvm.stand->live.path, "/api/ws?stream=0&auth_token=pass2"_ba);
        QVERIFY(told.closed.isEmpty());
    }

    void aKvmWithoutAStreamHasKeyboardAndMouseAllTheSame()
    {
        // (As the real one has: no stream, and the line let in and held.)
        MadeUpKvm kvm;
        kvm.instead.insert(place(), Answered(404, "{}"_ba));
        const ChangingScreen screen(kvm);
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_VERIFY(told.pictures.size() >= 1);
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        QCOMPARE(told.control[0].at(0), u"on"_s);
        one->key(keyA, true);
        one->key(keyA, false);
        QTRY_COMPARE(pressedOn(kvm), (QList<QJsonObject>{keyOf(u"KeyA"_s, true), keyOf(u"KeyA"_s, false)}));
        const qsizetype had = told.pictures.size();
        QTRY_VERIFY(told.pictures.size() >= had + 2);
    }

    // --- how the KVM is, and its end ---

    void howTheKvmIsIsPassedOnWhenItChanges()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const Told told(one.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        QVERIFY(told.states.isEmpty());
        // The driver looks, and the KVM is there.
        device.look();
        QTRY_COMPARE(told.states, (QList<QStringList>{{u"online"_s, QString()}}));
        // A look that finds it as it was says nothing.
        QTest::qWait(250);
        device.look();
        QTRY_VERIFY(!device.isStale());
        QCOMPARE(told.states.size(), 1);
        // Its computer sends no picture any more.
        kvm.signal = false;
        QTest::qWait(250);
        device.look();
        QTRY_COMPARE(told.states.size(), 2);
        QCOMPARE(told.states[1], (QStringList{u"no_signal"_s, QString()}));
    }

    void endedUnderWhoeverHasItKeyboardAndMouseAreLetGoFirst()
    {
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        std::unique_ptr<Live> two = live.open({1920, 30});
        const std::unique_ptr<Live> three = live.open({1920, 30});
        const Told first(one.get());
        const Told second(two.get());
        const Told third(three.get());
        QTRY_COMPARE(first.pictures.size(), 1);
        one->askControl(true);
        QTRY_COMPARE(first.control.size(), 1);
        one->key(keyA, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 1);

        live.end(base::Error{"not_found", "Unknown device"});
        // Nobody is told from inside that; it is over for each of them all the same.
        QVERIFY(first.closed.isEmpty());
        QVERIFY(!one->isOpen());
        QVERIFY(!one->hasControl());
        QVERIFY(!two->isOpen());
        QCOMPARE(live.watchers(), 0);
        // One of them closes before it was told: that one is told nothing.
        two->close();
        const QList<QStringList> why{{u"not_found"_s, u"Unknown device"_s}};
        QTRY_COMPARE(first.closed, why);
        QTRY_COMPARE(third.closed, why);
        // Who held keyboard and mouse is told that they went, with why; the others are not.
        QCOMPARE(first.control.size(), 2);
        QCOMPARE(first.control[1], (QStringList{u"off"_s, u"not_found"_s, u"Unknown device"_s}));
        QVERIFY(third.control.isEmpty());
        QVERIFY(second.closed.isEmpty());
        // What was down was let go, and both lines are given up.
        QTRY_COMPARE(pressedOn(kvm), (QList<QJsonObject>{keyOf(u"KeyA"_s, true), keyOf(u"KeyA"_s, false)}));
        QTRY_VERIFY(!kvm.stand->live.isConnected());
        QTRY_VERIFY(!kvm.stand->isStreaming());
        // What they ask now goes nowhere.
        one->askControl(true);
        one->key(keyB, true);
        one->setNeed({800, 5});
        const qsizetype heard = kvm.stand->asked.size();
        QTest::qWait(400);
        QCOMPARE(kvm.stand->asked.size(), heard);
        QCOMPARE(pressedOn(kvm).size(), 2);
        // And whoever asks for it live all the same is told the same.
        const std::unique_ptr<Live> late = live.open({1920, 30});
        const Told again(late.get());
        QVERIFY(!late->isOpen());
        QTRY_COMPARE(again.closed, why);
        QCOMPARE(again.opened, 0);
        QCOMPARE(kvm.stand->asked.size(), heard);
    }

    void aLiveThatEndsByItselfUnderWhoeverHoldsKeyboardAndMouseLetsGoFirstAndTellsThem_data()
    {
        QTest::addColumn<bool>("byItsPictures");
        QTest::newRow("its pictures fail three times in a row") << true;
        QTest::newRow("its stream is asked for and never answered") << false;
    }

    void aLiveThatEndsByItselfUnderWhoeverHoldsKeyboardAndMouseLetsGoFirstAndTellsThem()
    {
        QFETCH(bool, byItsPictures);
        MadeUpKvm kvm;
        const ChangingScreen screen(kvm);
        const SaysItIsThere there(kvm);
        Answered silence;
        silence.never = true;
        if (!byItsPictures) {
            kvm.instead.insert(place(), silence);
        }
        KvmdDevice::Pace patient = quick();
        patient.streamQuietMs = 1500;
        KvmdDevice device(kvm.where(), u"secret"_s, patient);
        KvmdLive live(device, brisk());
        const std::unique_ptr<Live> one = live.open({1920, 30});
        const std::unique_ptr<Live> two = live.open({1920, 30});
        const Told told(one.get());
        const Told other(two.get());
        if (byItsPictures) {
            QTRY_VERIFY(told.pictures.size() >= 1);
        }
        // Keyboard and mouse are had whether or not a picture has come, and a key goes down.
        one->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        QCOMPARE(told.control[0].at(0), u"on"_s);
        one->key(keyA, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 1);
        QStringList why;
        if (byItsPictures) {
            kvm.instead.insert(onePicture(), Answered(500, "oops"_ba, "text/plain"_ba));
            why = {u"bad_response"_s, u"Snapshot failed (HTTP 500)"_s};
        } else {
            why = {u"timeout"_s, u"Device did not respond in time"_s};
        }
        // The live is over, by itself. What was down was let go first, by the line, and its line
        // is let go; whoever held keyboard and mouse is told that they went, with why the live
        // ended, and then that it ended. Nothing may still be down, and nothing is said of that.
        QTRY_COMPARE_WITH_TIMEOUT(told.closed.size(), 1, 10'000);
        QCOMPARE(told.closed[0], why);
        QCOMPARE(told.said, (QStringList{u"on"_s, u"off"_s, u"closed"_s}));
        QCOMPARE(told.control[1], (QStringList{u"off"_s, why.at(0), why.at(1)}));
        QCOMPARE(told.leftDown, 0);
        QVERIFY(!one->hasControl());
        QTRY_COMPARE(pressedOn(kvm), (QList<QJsonObject>{keyOf(u"KeyA"_s, true), keyOf(u"KeyA"_s, false)}));
        QTRY_VERIFY(!kvm.stand->live.isConnected());
        // Whoever only looked is told of the end, and nothing of keyboard and mouse.
        QCOMPARE(other.said, (QStringList{u"closed"_s}));
        QCOMPARE(other.closed[0], why);
    }

    // --- on this PC's own wall ---

    void aKvmThatIsTakenOffTheWallEndsItsLive()
    {
        Own own;
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        const QString id = own.add(kvm, u"Build server"_s);
        const LiveOf asked(own.wall, id);
        QVERIFY(asked.live != nullptr);
        const Told told(asked.live.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        asked.live->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        asked.live->key(keyA, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 1);

        QVERIFY(own.wall.remove(id).ok());
        // Told a turn later, as the wall tells whoever it owes an answer, and in those words.
        QVERIFY(told.closed.isEmpty());
        QVERIFY(!asked.live->isOpen());
        QTRY_COMPARE(told.closed, (QList<QStringList>{{u"not_found"_s, u"Unknown device"_s}}));
        QCOMPARE(told.control.size(), 2);
        QCOMPARE(told.control[1], (QStringList{u"off"_s, u"not_found"_s, u"Unknown device"_s}));
        // Keyboard and mouse were let go first, and the KVM is asked for nothing more.
        QTRY_COMPARE(pressedOn(kvm), (QList<QJsonObject>{keyOf(u"KeyA"_s, true), keyOf(u"KeyA"_s, false)}));
        QTRY_VERIFY(!kvm.stand->live.isConnected());
        QTRY_VERIFY(!kvm.stand->isStreaming());
        QTest::qWait(300);
        const qsizetype heard = kvm.stand->asked.size();
        QTest::qWait(600);
        QCOMPARE(kvm.stand->asked.size(), heard);
        // And it is not to be had live any more.
        const LiveOf again(own.wall, id);
        QVERIFY(again.live == nullptr);
        QCOMPARE(again.refused.value_or(base::Error{}).code, std::string("not_found"));
    }

    void aKvmWhoseDriverIsReplacedEndsItsLiveAndIsHadLiveAnew()
    {
        Own own;
        MadeUpKvm kvm;
        kvm.instead.insert(place(), streaming(part(jpeg(1920, 1080))));
        const SaysItIsThere there(kvm);
        const QString id = own.add(kvm, u"Build server"_s);
        const LiveOf asked(own.wall, id);
        QVERIFY(asked.live != nullptr);
        const Told told(asked.live.get());
        QTRY_COMPARE(told.pictures.size(), 1);
        asked.live->askControl(true);
        QTRY_COMPARE(told.control.size(), 1);
        asked.live->key(keyA, true);
        QTRY_COMPARE(pressedOn(kvm).size(), 1);
        QCOMPARE(kvm.signIns, 1);

        // Its password is typed anew: another driver, which signs in anew.
        const core::OwnKvm as = own.wall.kvms().constFirst();
        QVERIFY(own.wall.change(id, as, std::optional<QString>(u"secret"_s)).ok());
        QVERIFY(told.closed.isEmpty());
        QTRY_COMPARE(told.closed, (QList<QStringList>{{u"no_stream"_s, u"Connecting to the KVM"_s}}));
        QCOMPARE(told.control.size(), 2);
        QCOMPARE(told.control[1], (QStringList{u"off"_s, u"no_stream"_s, u"Connecting to the KVM"_s}));
        QTRY_COMPARE(pressedOn(kvm), (QList<QJsonObject>{keyOf(u"KeyA"_s, true), keyOf(u"KeyA"_s, false)}));
        QTRY_VERIFY(!kvm.stand->isStreaming());
        // Asked for again, as that word says to: live through the new driver.
        const LiveOf anew(own.wall, id);
        QVERIFY(anew.live != nullptr);
        const Told again(anew.live.get());
        QTRY_COMPARE(again.pictures.size(), 1);
        QCOMPARE(again.pictures[0].counter, 1U);
        QCOMPARE(kvm.signIns, 2);
        QVERIFY(again.closed.isEmpty());
        // A new name is no new driver: the live stays.
        core::OwnKvm renamed = own.wall.kvms().constFirst();
        renamed.name = u"Build server 2"_s;
        QVERIFY(own.wall.change(id, renamed, std::nullopt).ok());
        QTest::qWait(300);
        QVERIFY(again.closed.isEmpty());
        QVERIFY(anew.live->isOpen());
    }
};

QTEST_GUILESS_MAIN(TestKvmdLive)
#include "test_kvmd_live.moc"
