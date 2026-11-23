#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/board_holds.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_media.h>
#include <qkvm/core/sign_in_pacing.h>

#include "made_up_kvm.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmdDevice;
using core::MediaDone;
using core::MediaList;
using core::MediaMount;
using core::testing::MadeUpKvm;
using core::testing::refusal;

namespace {

// An image of the made-up KVM's storage, as kvmd lists one.
QJsonObject anImage(qint64 size, bool complete = true, double stamp = 1'762'000'000.5)
{
    return {{u"size"_s, size}, {u"complete"_s, complete}, {u"mod_ts"_s, stamp}};
}

// A KVM with a drive and three images on its storage.
void withADrive(MadeUpKvm& kvm)
{
    kvm.hasDrive = true;
    kvm.msd.images = {{u"debian 13+netinst.iso"_s, anImage(700'000'000)},
                      {u"Alpine.iso"_s, anImage(200'000'000)},
                      {u"half.img"_s, anImage(1000, false)}};
}

template <typename T>
base::Result<T> waited(const std::function<void(std::function<void(base::Result<T>)>)>& ask)
{
    std::optional<base::Result<T>> answer;
    ask([&answer](base::Result<T> came) { answer = std::move(came); });
    if (!QTest::qWaitFor([&] { return answer.has_value(); }, 20000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return *answer;
}

// What an optional holds, or one made empty: read after the case has looked that it holds one.
template <typename T>
T got(const std::optional<T>& maybe)
{
    return maybe.value_or(T{});
}

std::string codeOf(const base::Result<MediaDone>& done)
{
    return done ? std::string("(ok)") : done.error().code;
}

std::string saidOf(const base::Result<MediaDone>& done)
{
    return done ? done.value().note.toStdString() : done.error().message;
}

} // namespace

// A kvmd KVM's virtual media on this PC's own wall (GAPS A2; the web's 0.3.0, kvm.py's msd parts and server.py's media
// routes, their tests test_media.py): the state of its drive read and kept as the web keeps it, from a look (every
// thirty seconds at the most) and from its line for events; the list, a mount, an unmount and a deletion, each
// refused in the web's words where something stands against it. Against a made-up KVM.
class TestKvmdMedia : public QObject {
    Q_OBJECT

private:
    static KvmdDevice::Pace quick()
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
        pace.pressHeldMs = 150;
        pace.longPressHeldMs = 300;
        pace.boardAnswerMs = 1200;
        pace.seenWithinMs = 600;
        pace.seenEveryMs = 100;
        pace.eventsPingMs = 500;
        pace.eventsQuietMs = 1500;
        pace.msdEveryMs = 1200;
        pace.listedWithinMs = 900;
        pace.listedEveryMs = 100;
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.boards = std::make_shared<core::BoardHolds>();
        return pace;
    }
    static core::Status lookedAt(KvmdDevice& device)
    {
        QSignalSpy over(&device, &KvmdDevice::looked);
        device.look();
        // (And the question of its drive asked beside it.)
        if (!over.wait(20000) || !QTest::qWaitFor([&device] { return !device.asksItsDrive(); }, 20000)) {
            core::Status never;
            never.state = u"the look never ended"_s;
            return never;
        }
        return device.status();
    }
    // The same, for a case that holds the KVM's line for events through to its end (heldAwake): the line is let go of
    // once nobody has asked for a picture for restAfterMs. In quick()'s 900 ms, counted from the last picture
    // heldAwake asks for, the case had to end, or its line went with nothing asking again (outside review cx4-1, L2).
    // A minute, as somebody watching asks again within it (the program's own is 30 s).
    static KvmdDevice::Pace watched()
    {
        KvmdDevice::Pace pace = quick();
        pace.restAfterMs = 60'000;
        return pace;
    }
    // The driver holds the KVM's line for events, as it does while somebody watches its picture (made_up_kvm.h's
    // heldAwake: asked again until the driver's own line stands). Empty where it does; what was seen otherwise. (With
    // watched(): held to the case's end. Asked once, it failed 2 times in 6 whole runs on the loaded build box, and 7
    // in 20; asked again but taking a try given up on for held, the drive's state said on the line was lost 2 runs
    // in 20.)
    static QString heldAwake(KvmdDevice& device, MadeUpKvm& kvm)
    {
        return core::testing::heldAwake(device, kvm, watched().connectWithinMs).why();
    }
    static base::Result<MediaList> listOf(KvmdDevice& device)
    {
        return waited<MediaList>([&device](auto done) { device.askMedia(std::move(done)); });
    }
    static base::Result<MediaDone> mounted(KvmdDevice& device, const MediaMount& mount)
    {
        return waited<MediaDone>([&](auto done) { device.mountMedia(mount, std::move(done)); });
    }
    static base::Result<MediaDone> unmounted(KvmdDevice& device, const QString& image)
    {
        return waited<MediaDone>([&](auto done) { device.unmountMedia(image, std::move(done)); });
    }
    static base::Result<MediaDone> deleted(KvmdDevice& device, const QString& image)
    {
        return waited<MediaDone>([&](auto done) { device.deleteMedia(image, std::move(done)); });
    }
    static MediaMount asked(const QString& image, const QString& drive = u"cd"_s, bool writable = false)
    {
        MediaMount mount;
        mount.image = image;
        mount.machine = u"Rack 4"_s;
        mount.drive = drive;
        mount.writable = writable;
        return mount;
    }
    static QList<QByteArray> msdDone(const MadeUpKvm& kvm)
    {
        QList<QByteArray> found;
        for (const QByteArray& one : kvm.did) {
            if (one.startsWith("msd ")) {
                found.append(one);
            }
        }
        return found;
    }

private slots:
    void initTestCase()
    {
        // (Windows' own TLS is chosen before anything else touches TLS.)
        const KvmdDevice first(KvmdDevice::Where{}, QString());
    }

    void whatKvmdSaysOfItsDriveIsKeptAsTheWebKeepsIt()
    {
        // Of its state, what the web reads (MSD_KEPT, MSD_STORAGE_KEPT), and nothing else.
        const QJsonObject state{{u"enabled"_s, true},
                                {u"online"_s, true},
                                {u"busy"_s, false},
                                {u"drive"_s, QJsonObject{{u"connected"_s, false}}},
                                {u"drive_partition"_s, QJsonObject{{u"connected"_s, false}}},
                                {u"features"_s, QJsonObject{{u"multi"_s, true}}},
                                {u"storage"_s, QJsonObject{{u"images"_s, QJsonObject{}},
                                                           {u"parts"_s, QJsonObject{}},
                                                           {u"downloading"_s, QJsonValue::Null}}}};
        const QJsonObject kept = core::kvmd::msdKept(state);
        QCOMPARE(kept.keys(),
                 (QStringList{u"busy"_s, u"drive"_s, u"drive_partition"_s, u"enabled"_s, u"online"_s, u"storage"_s}));
        QCOMPARE(kept.value("storage"_L1).toObject().keys(), (QStringList{u"images"_s, u"parts"_s}));

        // A whole state (it has `online`) takes the place of what was known; a part of one is merged in, `storage`
        // key by key; a part of a state nothing is known of says nothing.
        QVERIFY(!core::kvmd::msdMerged(std::nullopt, QJsonObject{{u"busy"_s, true}}));
        const std::optional<QJsonObject> whole = core::kvmd::msdMerged(std::nullopt, state);
        QVERIFY(whole);
        const std::optional<QJsonObject> part = core::kvmd::msdMerged(
            whole, QJsonObject{{u"busy"_s, true},
                               {u"storage"_s, QJsonObject{{u"images"_s, QJsonObject{{u"a.iso"_s, anImage(1)}}}}}});
        QVERIFY(part);
        QCOMPARE(got(part).value("busy"_L1), QJsonValue(true));
        QCOMPARE(got(part).value("online"_L1), QJsonValue(true));
        QVERIFY(got(part).value("storage"_L1).toObject().value("images"_L1).toObject().contains("a.iso"_L1));
        QVERIFY(got(part).value("storage"_L1).toObject().contains("parts"_L1));
        const std::optional<QJsonObject> anew =
            core::kvmd::msdMerged(part, QJsonObject{{u"online"_s, false}, {u"enabled"_s, true}});
        QVERIFY(anew);
        QVERIFY(!got(anew).contains("storage"_L1));
        QVERIFY(!got(anew).contains("busy"_L1));

        // Its `msd` event on the line, and nothing else; not longer than the web reads one.
        const QByteArray event = R"({"event_type": "msd", "event": {"online": true}})";
        QVERIFY(core::kvmd::msdHeard(event));
        QVERIFY(!core::kvmd::msdHeard(R"({"event_type": "atx", "event": {"online": true}})"));
        QVERIFY(!core::kvmd::msdHeard(R"({"event_type": "msd", "event": [1]})"));
        QVERIFY(!core::kvmd::msdHeard(R"({"event_type": "msd"})"));
        const QByteArray tooLong = core::testing::written(
            {{u"event_type"_s, u"msd"_s},
             {u"event"_s, QJsonObject{{u"online"_s, true}, {u"x"_s, QString(core::kvmd::mostMsdWord, u'x')}}}});
        QVERIFY(!core::kvmd::msdHeard(tooLong));
    }

    void anImagesNameIsOneTheWebHandsBack_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("taken");
        QTest::newRow("plain") << u"debian-13.iso"_s << true;
        QTest::newRow("blanks and signs") << u"Win 11 (x64) [EN]~1,2+3.iso"_s << true;
        QTest::newRow("in folders") << u"isos/linux/a/b.iso"_s << true;
        QTest::newRow("five deep") << u"a/b/c/d/e.iso"_s << false;
        QTest::newRow("a dot first") << u".hidden"_s << false;
        QTest::newRow("a blank last") << u"a.iso "_s << false;
        QTest::newRow("a climb") << u"../x.iso"_s << false;
        QTest::newRow("a slash first") << u"/x.iso"_s << false;
        QTest::newRow("a percent") << u"a%2F.iso"_s << false;
        QTest::newRow("a quote") << u"a\".iso"_s << false;
        QTest::newRow("an ampersand") << u"a&b=c"_s << false;
        QTest::newRow("too long") << QString(201, u'a') << false;
        QTest::newRow("as long as may be") << QString(200, u'a') << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("a JetKVM's part") << u"x.iso.incomplete"_s << true;
    }
    void anImagesNameIsOneTheWebHandsBack()
    {
        QFETCH(QString, name);
        QFETCH(bool, taken);
        QCOMPARE(core::kvmd::isDeviceImage(name), taken);
    }

    void theImagesTheSpaceAndWhatIsMountedAreReadAsTheWebReadsThem()
    {
        core::kvmd::MediaNotes notes;
        notes.carried(u"kvm"_s, u"b.iso"_s, u"ab12"_s, 5000, 1.5);
        notes.carried(u"kvm"_s, u"c.iso"_s, u"cd34"_s, 5000, 1.5);
        QJsonObject images{{u"b.iso"_s, anImage(5000, true, 1.5)}, {u"A.iso"_s, anImage(9)},
                           {u"c.iso"_s, anImage(5000, true, 2.5)}, // (another of that name, put there since)
                           {u"cut.img"_s, anImage(70, false)},     {u"odd size.iso"_s, QJsonObject{{u"size"_s, -1}}},
                           {u"../climbs.iso"_s, anImage(1)},       {u"not an image"_s, 3}};
        const QJsonObject state{
            {u"enabled"_s, true},
            {u"online"_s, true},
            {u"storage"_s,
             QJsonObject{{u"images"_s, images},
                         {u"parts"_s, QJsonObject{{u""_s, QJsonObject{{u"free"_s, 1000}, {u"size"_s, 9000}}}}}}}};
        const core::kvmd::Listed listed = core::kvmd::imagesOf(state, notes, u"kvm"_s);
        QStringList names;
        for (const core::MediaImage& image : listed.images) {
            names.append(image.name);
        }
        QCOMPARE(names, (QStringList{u"A.iso"_s, u"b.iso"_s, u"c.iso"_s, u"cut.img"_s, u"odd size.iso"_s}));
        QCOMPARE(listed.leftOut, 2);
        QCOMPARE(listed.images[1].sha256, u"ab12"_s);
        QVERIFY(listed.images[2].sha256.isEmpty());
        QVERIFY(listed.images[3].partial);
        QCOMPARE(listed.images[3].size, std::optional<qint64>(70));
        QVERIFY(!listed.images[4].size);
        // (One that does not say it is complete is not taken for whole: outside review 4.7.)
        QVERIFY(listed.images[4].partial);
        QVERIFY(core::kvmd::imagesOf(
                    QJsonObject{{u"storage"_s, QJsonObject{{u"images"_s, QJsonObject{{u"a.iso"_s, anImage(1)}}}}}},
                    notes, u"kvm"_s)
                    .whole);
        QVERIFY(core::kvmd::imagesOf(
                    QJsonObject{
                        {u"storage"_s,
                         QJsonObject{{u"images"_s,
                                      QJsonObject{{u"a.iso"_s, QJsonObject{{u"size"_s, 1}, {u"complete"_s, 1}}}}}}}},
                    notes, u"kvm"_s)
                    .images.value(0)
                    .partial);
        QCOMPARE(core::kvmd::freeOf(state), std::optional<qint64>(1000));
        QCOMPARE(core::kvmd::sizeOf(state), std::optional<qint64>(9000));

        // What is in its drive: a CD-ROM drive only where kvmd plainly says so, read-only unless plainly writable.
        QJsonObject drive{{u"connected"_s, true},
                          {u"cdrom"_s, true},
                          {u"rw"_s, true},
                          {u"image"_s, QJsonObject{{u"name"_s, u"b.iso"_s}, {u"size"_s, 5000}, {u"mod_ts"_s, 1.5}}}};
        QJsonObject withIt = state;
        withIt.insert(u"drive"_s, drive);
        std::optional<core::MountedMedia> now = core::kvmd::mountedOf(withIt, notes, u"kvm"_s);
        QVERIFY(now);
        QCOMPARE(got(now).drive, u"cd"_s);
        QVERIFY(got(now).readOnly);
        QCOMPARE(got(now).image, u"b.iso"_s);
        QCOMPARE(got(now).sha256, u"ab12"_s);
        drive.insert(u"cdrom"_s, u"yes"_s);
        withIt.insert(u"drive"_s, drive);
        now = core::kvmd::mountedOf(withIt, notes, u"kvm"_s);
        QCOMPARE(got(now).drive, u"flash"_s);
        QVERIFY(!got(now).readOnly);
        // (Writable unless it plainly says it is not: outside review 4.7.)
        drive.insert(u"rw"_s, 1);
        drive.insert(u"image"_s, QJsonObject{{u"name"_s, u"../x\ny"_s}});
        withIt.insert(u"drive"_s, drive);
        now = core::kvmd::mountedOf(withIt, notes, u"kvm"_s);
        QVERIFY(!got(now).readOnly);
        QCOMPARE(got(now).name, u"../x y"_s);
        // A name it does not hand back, or none, is said as a line, and goes by an id that is no name of an image:
        // never the one it reads as, nor with that one's SHA-256 (review cx4-1 M2: "an image", "7" and "b.iso" are
        // names of images). The same again for the same, another for another.
        QVERIFY2(core::kvmd::isNameless(got(now).image), qPrintable(got(now).image));
        QVERIFY(!core::kvmd::isDeviceImage(got(now).image));
        const auto byName = [&](const QJsonValue& name) {
            QJsonObject odd = drive;
            odd.insert(u"image"_s, QJsonObject{{u"name"_s, name}, {u"size"_s, 5000}, {u"mod_ts"_s, 1.5}});
            QJsonObject withOdd = withIt;
            withOdd.insert(u"drive"_s, odd);
            return core::kvmd::mountedOf(withOdd, notes, u"kvm"_s);
        };
        QCOMPARE(got(byName(u"b.iso"_s)).image, u"b.iso"_s);
        QCOMPARE(got(byName(u"b.iso"_s)).sha256, u"ab12"_s);
        for (const QJsonValue& name :
             {QJsonValue(u"b.iso\n"_s), QJsonValue(7), QJsonValue(), QJsonValue(u" b.iso"_s)}) {
            const std::optional<core::MountedMedia> read = byName(name);
            QVERIFY2(core::kvmd::isNameless(got(read).image), qPrintable(got(read).image));
            QCOMPARE(got(byName(name)).image, got(read).image);
            QVERIFY(got(read).sha256.isEmpty());
        }
        QCOMPARE(got(byName(u"b.iso\n"_s)).name, u"b.iso"_s);
        QVERIFY(got(byName(7)).image != got(byName(8)).image);
        drive.insert(u"rw"_s, false);
        withIt.insert(u"drive"_s, drive);
        QVERIFY(got(core::kvmd::mountedOf(withIt, notes, u"kvm"_s)).readOnly);
        // A drive in no shape it reads, or one that does not plainly say whether it is connected, is no empty drive:
        // it cannot be read, and no image is called so. (None at all is what kvmd says of a drive that is offline.)
        QJsonObject offline = withIt;
        offline.insert(u"drive"_s, QJsonValue::Null);
        QVERIFY(!core::kvmd::mountedOf(offline, notes, u"kvm"_s));
        offline.remove(u"drive"_s);
        QVERIFY(!core::kvmd::mountedOf(offline, notes, u"kvm"_s));
        for (const QJsonValue& odd : {QJsonValue(u"cd"_s), QJsonValue(7), QJsonValue(QJsonArray{})}) {
            QJsonObject oddly = withIt;
            oddly.insert(u"drive"_s, odd);
            now = core::kvmd::mountedOf(oddly, notes, u"kvm"_s);
            QVERIFY(now);
            QCOMPARE(got(now).image, QString(core::kvmd::unreadDrive));
            QVERIFY(!got(now).readOnly);
        }
        QJsonObject unsure = drive;
        unsure.insert(u"connected"_s, u"yes"_s);
        withIt.insert(u"drive"_s, unsure);
        QCOMPARE(got(core::kvmd::mountedOf(withIt, notes, u"kvm"_s)).image, QString(core::kvmd::unreadDrive));
        QVERIFY(!core::kvmd::isDeviceImage(QString(core::kvmd::unreadDrive)));
        drive.insert(u"image"_s, QJsonValue::Null);
        withIt.insert(u"drive"_s, drive);
        QCOMPARE(got(core::kvmd::mountedOf(withIt, notes, u"kvm"_s)).name, u"an image"_s);
        QVERIFY(core::kvmd::isNameless(got(core::kvmd::mountedOf(withIt, notes, u"kvm"_s)).image));
        drive.insert(u"connected"_s, false);
        withIt.insert(u"drive"_s, drive);
        QVERIFY(!core::kvmd::mountedOf(withIt, notes, u"kvm"_s));
        drive.insert(u"connected"_s, true);
        withIt.insert(u"drive"_s, drive);
        withIt.insert(u"enabled"_s, false);
        QVERIFY(!core::kvmd::mountedOf(withIt, notes, u"kvm"_s));
        QVERIFY(!core::kvmd::offerOf(withIt, true));
        QVERIFY(!core::kvmd::offerOf(std::nullopt, true));

        // What its drive can do, as the web's kvmd offer says it.
        const std::optional<core::MediaOffer> offer = core::kvmd::offerOf(state, true);
        QVERIFY(offer);
        QCOMPARE(got(offer).drives, (QStringList{u"cd"_s, u"flash"_s}));
        QVERIFY(got(offer).boot.isEmpty());
        QVERIFY(got(offer).restart);
        QVERIFY(got(offer).upload);
        QCOMPARE(got(offer).where, u"device"_s);
        QCOMPARE(got(offer).writable, QStringList{u"flash"_s});
        QVERIFY(got(offer).canDelete);
        QVERIFY(!got(offer).dropsInput);
        QVERIFY(!got(core::kvmd::offerOf(state, false)).restart);
        QCOMPARE(core::kvmd::withCommas(614'400), u"614,400"_s);
        QCOMPARE(core::kvmd::withCommas(999), u"999"_s);
        QCOMPARE(core::kvmd::withCommas(16'000'000'000), u"16,000,000,000"_s);
    }

    void whatStandsAgainstAChangeIsSaidInTheWebsWords_data()
    {
        QTest::addColumn<QJsonObject>("state");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("part");
        const QJsonObject well{{u"enabled"_s, true}, {u"online"_s, true}, {u"busy"_s, false}};
        QJsonObject off = well;
        off.insert(u"enabled"_s, QJsonValue::Null);
        QJsonObject offline = well;
        offline.insert(u"online"_s, false);
        QJsonObject busy = well;
        busy.insert(u"busy"_s, true);
        QJsonObject sharing = well;
        sharing.insert(u"drive_partition"_s, QJsonObject{{u"connected"_s, true}});
        QTest::newRow("nothing") << well << QString() << QString();
        QTest::newRow("its drive switched off") << off << u"no_virtual_media"_s << u"switched off"_s;
        QTest::newRow("offline") << offline << u"action_failed"_s << u"(offline)"_s;
        QTest::newRow("busy") << busy << u"busy"_s << u"busy with something else"_s;
        QTest::newRow("File Sharing") << sharing << u"action_failed"_s << u"File Sharing is on"_s;
    }
    void whatStandsAgainstAChangeIsSaidInTheWebsWords()
    {
        QFETCH(QJsonObject, state);
        QFETCH(QString, code);
        QFETCH(QString, part);
        const std::optional<base::Error> against = core::kvmd::standsAgainst(state);
        QCOMPARE(against.has_value(), !code.isEmpty());
        if (against) {
            QCOMPARE(QString::fromStdString(got(against).code), code);
            QVERIFY2(QString::fromStdString(got(against).message).contains(part), got(against).message.c_str());
        }
    }

    void aLookAsksItsDriveEveryWhileAtTheMost()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.msd.connected = true;
        kvm.msd.chosen = u"Alpine.iso"_s;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(status.offersMedia);
        QVERIFY(status.media);
        QCOMPARE(got(status.media).name, u"Alpine.iso"_s);
        QCOMPARE(got(status.media).drive, u"cd"_s);
        QVERIFY(device.mediaOffer());
        QCOMPARE(kvm.asked("/api/msd"), 1);
        // Within the while, it is not asked again; after it, it is.
        QTest::qWait(300);
        static_cast<void>(lookedAt(device));
        QCOMPARE(kvm.asked("/api/msd"), 1);
        QTest::qWait(1200);
        kvm.msd.connected = false;
        status = lookedAt(device);
        QCOMPARE(kvm.asked("/api/msd"), 2);
        QVERIFY(!status.media);
        QVERIFY(status.offersMedia);
        // Its own drive switched off: no media, and none offered.
        kvm.msd.enabled = false;
        QTest::qWait(1300);
        status = lookedAt(device);
        QVERIFY(!status.offersMedia);
        QVERIFY(!device.mediaOffer());
    }

    void aKvmThatHasNoDriveIsNotAskedAtEveryLook()
    {
        MadeUpKvm kvm;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.offersMedia);
        QVERIFY(!status.media);
        QCOMPARE(kvm.asked("/api/msd"), 1);
        QTest::qWait(300);
        status = lookedAt(device);
        QCOMPARE(kvm.asked("/api/msd"), 1);
        QCOMPARE(status.state, u"online"_s);
    }

    void theLineSaysItsDriveAndALookAsksNothingMeanwhile()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.stand->live.greeting = {kvm.msdEvent()};
        KvmdDevice device(kvm.where(), kvm.password, watched());
        const QString notHeld = heldAwake(device, kvm);
        QVERIFY2(notHeld.isEmpty(), qPrintable(notHeld));
        QVERIFY(QTest::qWaitFor([&] { return device.mediaOffer().has_value() && !device.asksItsDrive(); }, 5000));
        const int asked = kvm.asked("/api/msd");
        // What changed of it, said on the line: on its card at once, and said (statusChanged).
        QSignalSpy changed(&device, &KvmdDevice::statusChanged);
        kvm.stand->live.say(QJsonObject{
            {u"event_type"_s, u"msd"_s},
            {u"event"_s,
             QJsonObject{{u"drive"_s, QJsonObject{{u"connected"_s, true},
                                                  {u"cdrom"_s, false},
                                                  {u"rw"_s, true},
                                                  {u"image"_s, QJsonObject{{u"name"_s, u"Alpine.iso"_s}}}}}}}});
        QVERIFY(QTest::qWaitFor([&] { return device.status().media.has_value(); }, 5000));
        QVERIFY(!changed.isEmpty());
        QCOMPARE(got(device.status().media).drive, u"flash"_s);
        QVERIFY(!got(device.status().media).readOnly);
        static_cast<void>(lookedAt(device));
        QCOMPARE(kvm.asked("/api/msd"), asked);
    }

    void aLongStateOfItsDriveOnTheLineIsHeardAndALongerOneLeftUnread()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        // Some 1500 images: some two hundred kilobytes, as kvmd says them as the line opens.
        for (int one = 0; one < 1500; ++one) {
            kvm.msd.images.insert(u"image-%1-of-a-long-list-of-them.iso"_s.arg(one, 4, 10, u'0'),
                                  QJsonObject{{u"size"_s, 700'000'000},
                                              {u"complete"_s, true},
                                              {u"mod_ts"_s, 1'762'000'000.5},
                                              {u"name"_s, u"x"_s}});
        }
        kvm.msd.chosen = u"image-0007-of-a-long-list-of-them.iso"_s;
        kvm.msd.connected = true;
        const QString long1 = kvm.msdEvent();
        QVERIFY(long1.size() > qsizetype{128} * 1024);
        QVERIFY(long1.size() < core::kvmd::mostMsdWord);
        kvm.stand->live.greeting = {long1};
        // (Only the line says it: asked, it does not answer.)
        kvm.instead.insert("/api/msd", refusal(500, u"ServerError"_s));
        KvmdDevice device(kvm.where(), kvm.password, watched());
        const QString notHeld = heldAwake(device, kvm);
        QVERIFY2(notHeld.isEmpty(), qPrintable(notHeld));
        QVERIFY(QTest::qWaitFor([&] { return device.status().media.has_value(); }, 5000));
        QCOMPARE(got(device.status().media).name, u"image-0007-of-a-long-list-of-them.iso"_s);
        // Longer than is read: thrown away unread, and the line stays.
        QString longer = long1;
        longer.insert(longer.size() - 2, u", \"x\": \""_s + QString(core::kvmd::mostMsdWord, u'x') + u'"');
        kvm.stand->live.sayRaw(longer);
        kvm.stand->live.say(QJsonObject{
            {u"event_type"_s, u"msd"_s},
            {u"event"_s, QJsonObject{{u"drive"_s, QJsonObject{{u"connected"_s, true},
                                                              {u"image"_s, QJsonObject{{u"name"_s, u"x.iso"_s}}}}}}}});
        QVERIFY(QTest::qWaitFor([&] { return device.status().media.value_or(core::MountedMedia{}).name == "x.iso"_L1; },
                                5000));
        QVERIFY(device.isKeptAwake());
        QVERIFY(kvm.stand->live.isConnected());
    }

    void theListSaysTheImagesTheSpaceAndWhatStandsAgainst()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.atx = true;
        kvm.msd.busy = true;
        kvm.msd.connected = true;
        kvm.msd.chosen = u"Alpine.iso"_s;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        const base::Result<MediaList> list = listOf(device);
        QVERIFY2(list, list ? "" : list.error().message.c_str());
        QCOMPARE(list.value().images.size(), 3);
        QCOMPARE(list.value().images[0].name, u"Alpine.iso"_s);
        QCOMPARE(list.value().images[1].name, u"debian 13+netinst.iso"_s);
        QVERIFY(list.value().images[2].partial);
        QCOMPARE(list.value().free, std::optional<qint64>(8'000'000'000));
        QCOMPARE(list.value().size, std::optional<qint64>(16'000'000'000));
        QVERIFY(list.value().mounted);
        QCOMPARE(got(list.value().mounted).image, u"Alpine.iso"_s);
        QCOMPARE(list.value().notes.size(), 1);
        QVERIFY(list.value().notes[0].contains("busy with something else"_L1));
        QVERIFY(list.value().offers.restart);
        QCOMPARE(list.value().offers.where, u"device"_s);
        QVERIFY(!list.value().uploading);
        // Its list not read whole: said, and never shown as all there is (review cx4-1 L5: an empty storage).
        kvm.msd.busy = false;
        kvm.msd.imagesSayInstead = QJsonValue(u"none"_s);
        const base::Result<MediaList> unread = listOf(device);
        QVERIFY(unread);
        QVERIFY(unread.value().images.isEmpty());
        // (And the box is told so beside the note: review cx4-2 L1.)
        QVERIFY(!unread.value().readWhole);
        QCOMPARE(
            unread.value().notes,
            QStringList{u"The KVM's list of its own storage could not be read whole (it is in no shape qKVM reads, "
                        u"or longer than the 2,000 images qKVM reads), so not every image on it may be shown "
                        u"here: look at it on its own page"_s});
        kvm.msd.imagesSayInstead.reset();
        const base::Result<MediaList> read = listOf(device);
        QVERIFY(read && read.value().notes.isEmpty());
        QVERIFY(read.value().readWhole);
        // Its own drive switched off: none.
        kvm.msd.enabled = false;
        const base::Result<MediaList> none = listOf(device);
        QVERIFY(!none);
        QCOMPARE(none.error().code, std::string("no_virtual_media"));
    }

    void anImageIsMountedAsTheWebMountsIt()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        // (A name with a blank and a plus reaches kvmd as it is.)
        base::Result<MediaDone> done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QVERIFY2(done, saidOf(done).c_str());
        QCOMPARE(msdDone(kvm), (QList<QByteArray>{"msd params debian 13+netinst.iso cdrom 1 rw 0", "msd connected 1"}));
        QVERIFY(done.value().note.isEmpty());
        QVERIFY(done.value().mounted);
        QCOMPARE(got(done.value().mounted).image, u"debian 13+netinst.iso"_s);
        QVERIFY(device.status().media);
        // As it is already: said so, and nothing sent.
        kvm.did.clear();
        done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(saidOf(done), std::string("It was mounted so already."));
        QVERIFY(msdDone(kvm).isEmpty());
        // A USB flash drive, writable.
        kvm.msd.connected = false;
        done = mounted(device, asked(u"Alpine.iso"_s, u"flash"_s, true));
        QVERIFY2(done, saidOf(done).c_str());
        QCOMPARE(msdDone(kvm), (QList<QByteArray>{"msd params Alpine.iso cdrom 0 rw 1", "msd connected 1"}));
        QVERIFY(!got(done.value().mounted).readOnly);
    }

    void aMountIsRefusedInTheWebsWords_data()
    {
        QTest::addColumn<QString>("image");
        QTest::addColumn<QString>("drive");
        QTest::addColumn<bool>("writable");
        QTest::addColumn<QString>("boot");
        QTest::addColumn<bool>("restart");
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("part");
        QTest::newRow("a disk") << u"Alpine.iso"_s << u"disk"_s << false << u"none"_s << false << QString()
                                << u"invalid"_s << u"Rack 4 takes no image as a disk"_s;
        QTest::newRow("a word it does not know") << u"Alpine.iso"_s << u"floppy"_s << false << u"none"_s << false
                                                 << QString() << u"invalid"_s << u"takes no image"_s;
        QTest::newRow("a writable CD") << u"Alpine.iso"_s << u"cd"_s << true << u"none"_s << false << QString()
                                       << u"invalid"_s << u"as a CD-ROM drive read-only, and only so"_s;
        QTest::newRow("started from") << u"Alpine.iso"_s << u"cd"_s << false << u"once"_s << false << QString()
                                      << u"invalid"_s << u"cannot be told to start from an image"_s;
        QTest::newRow("restarted without a board") << u"Alpine.iso"_s << u"cd"_s << false << u"none"_s << true
                                                   << QString() << u"invalid"_s << u"cannot be restarted from here"_s;
        QTest::newRow("no image's name") << u"../etc/passwd"_s << u"cd"_s << false << u"none"_s << false << QString()
                                         << u"invalid"_s << u"not an image of the KVM's"_s;
        QTest::newRow("offline") << u"Alpine.iso"_s << u"cd"_s << false << u"none"_s << false << u"offline"_s
                                 << u"action_failed"_s << u"(offline)"_s;
        QTest::newRow("busy") << u"Alpine.iso"_s << u"cd"_s << false << u"none"_s << false << u"busy"_s << u"busy"_s
                              << u"busy with something else"_s;
        QTest::newRow("File Sharing") << u"Alpine.iso"_s << u"cd"_s << false << u"none"_s << false << u"sharing"_s
                                      << u"action_failed"_s << u"File Sharing is on"_s;
        QTest::newRow("not there any more") << u"gone.iso"_s << u"cd"_s << false << u"none"_s << false << QString()
                                            << u"changed"_s << u"not on the KVM's own storage any more"_s;
        QTest::newRow("cut off") << u"half.img"_s << u"flash"_s << false << u"none"_s << false << QString()
                                 << u"changed"_s << u"an upload of it was cut off"_s;
        QTest::newRow("too small for a CD")
            << u"half.img"_s << u"cd"_s << false << u"none"_s << false << u"whole"_s << u"invalid"_s
            << u"is 1,000 bytes, and a CD-ROM drive takes an image of "
               u"614,400 bytes at least"_s;
        QTest::newRow("another in it") << u"Alpine.iso"_s << u"cd"_s << false << u"none"_s << false << u"another"_s
                                       << u"changed"_s << u"debian 13+netinst.iso is mounted on it now: unmount"_s;
        QTest::newRow("in it as another drive") << u"debian 13+netinst.iso"_s << u"flash"_s << false << u"none"_s
                                                << false << u"another"_s << u"changed"_s << u"unmount it first"_s;
        QTest::newRow("switched off") << u"Alpine.iso"_s << u"cd"_s << false << u"none"_s << false << u"off"_s
                                      << u"no_virtual_media"_s << u"Rack 4 has no virtual media"_s;
    }
    void aMountIsRefusedInTheWebsWords()
    {
        QFETCH(QString, image);
        QFETCH(QString, drive);
        QFETCH(bool, writable);
        QFETCH(QString, boot);
        QFETCH(bool, restart);
        QFETCH(QString, state);
        QFETCH(QString, code);
        QFETCH(QString, part);
        MadeUpKvm kvm;
        withADrive(kvm);
        if (state == "whole"_L1) {
            kvm.msd.images.insert(u"half.img"_s, anImage(1000));
        } else if (state == "another"_L1) {
            kvm.msd.chosen = u"debian 13+netinst.iso"_s;
            kvm.msd.connected = true;
        } else if (state == "off"_L1) {
            kvm.msd.enabled = false;
        }
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        kvm.msd.online = state != "offline"_L1;
        kvm.msd.busy = state == "busy"_L1;
        kvm.msd.fileSharing = state == "sharing"_L1;
        MediaMount mount = asked(image, drive, writable);
        mount.boot = boot;
        mount.restart = restart;
        kvm.did.clear();
        const base::Result<MediaDone> done = mounted(device, mount);
        QCOMPARE(codeOf(done), code.toStdString());
        QVERIFY2(QString::fromStdString(saidOf(done)).contains(part), saidOf(done).c_str());
        QVERIFY(msdDone(kvm).isEmpty());
    }

    void aMountThatComesOutOtherwiseIsTakenOutAgain()
    {
        // Chosen otherwise (a KVM that takes no `rw`): read back before anything is connected, and nothing is.
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.msd.mountsOtherwise = true;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        base::Result<MediaDone> done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("The KVM's drive does not have Alpine.iso chosen as it was asked (another "
                                           "client of the KVM may have chosen otherwise meanwhile): nothing was "
                                           "connected. Look at it again."));
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd params Alpine.iso cdrom 1 rw 0"});
        QVERIFY(!kvm.msd.connected);
        // This image, connected otherwise (writable, chosen so by another in the moment before the connect): taken out
        // again, and read back.
        kvm.msd.mountsOtherwise = false;
        kvm.did.clear();
        kvm.msd.othersChoiceAtConnect = u"Alpine.iso"_s;
        done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), std::string("The KVM did not mount it as it was asked, and it is not mounted: look at "
                                           "it on its own page."));
        QCOMPARE(msdDone(kvm),
                 (QList<QByteArray>{"msd params Alpine.iso cdrom 1 rw 0", "msd connected 1", "msd connected 0"}));
        QVERIFY(!kvm.msd.connected);
        // Told to take it out, answered yes, and still in: said so, not that it is out. (Outside review 4.2: whatever
        // was read back was "not mounted".)
        kvm.did.clear();
        kvm.msd.othersChoiceAtConnect = u"Alpine.iso"_s;
        kvm.msd.disconnectsNothing = true;
        done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), std::string("The KVM did not mount it as it was asked, and says it is still in its "
                                           "drive after it was told to take it out: look at it on its own page."));
        QCOMPARE(msdDone(kvm),
                 (QList<QByteArray>{"msd params Alpine.iso cdrom 1 rw 0", "msd connected 1", "msd connected 0"}));
        QVERIFY(kvm.msd.connected);
    }

    // Review cx4-2 L2 and L6: a connect kvmd answered yes to put something in its drive. Where the read-back after it
    // fails, that is said so, and nothing more is done (it was said as a mount that did not happen); where it is in
    // no shape read, likewise; where it finds nothing in the drive, that it was connected and is not in it now (it
    // was "did not mount it as it was asked"). An image whose name the KVM does not say at all that the connect put in
    // is said as what it may be, and where it stays chosen after the taking out, that is said by the id it went by.
    // And a read-back after the taking out that gets no answer says so, never "nothing is mounted".
    void whatAConnectPutInIsSaidWhereItsReadBackFailsOrFindsItOtherwise()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        const auto withTheDrive = [&kvm](const QJsonValue& drive) {
            QJsonObject state = kvm.msdState();
            state.insert(u"drive"_s, drive);
            return core::testing::good(state);
        };
        // The read-back after the connect is answered 500.
        kvm.msd.stateAfterConnect = core::testing::Answered{500, "<html>gateway</html>"_ba, "text/html"_ba};
        base::Result<MediaDone> done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QCOMPARE(saidOf(done), std::string("This mount connected the KVM's drive, and what is in it since is not known "
                                           "(The KVM did not say how its drive is): look at it on its own page."));
        QCOMPARE(msdDone(kvm), (QList<QByteArray>{"msd params Alpine.iso cdrom 1 rw 0", "msd connected 1"}));
        QVERIFY(kvm.msd.connected);
        // In no shape read.
        kvm.msd.connected = false;
        kvm.did.clear();
        kvm.msd.stateAfterConnect = withTheDrive(u"in no shape"_s);
        done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QCOMPARE(saidOf(done), std::string("This mount connected the KVM's drive, and the KVM does not say what is in "
                                           "it in a shape qKVM reads: nothing more was done. Look at it on its own "
                                           "page."));
        QCOMPARE(msdDone(kvm), (QList<QByteArray>{"msd params Alpine.iso cdrom 1 rw 0", "msd connected 1"}));
        // Nothing in it by the read-back.
        kvm.msd.connected = false;
        kvm.did.clear();
        QJsonObject nothingIn{{u"connected"_s, false}, {u"cdrom"_s, true}, {u"rw"_s, false}};
        nothingIn.insert(u"image"_s, QJsonObject{{u"name"_s, u"Alpine.iso"_s}});
        kvm.msd.stateAfterConnect = withTheDrive(nothingIn);
        done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done),
                 std::string("Alpine.iso was connected, and is not in its drive now: look at it on its own page."));
        QCOMPARE(msdDone(kvm), (QList<QByteArray>{"msd params Alpine.iso cdrom 1 rw 0", "msd connected 1"}));
        // An image whose name it does not say at all, taken out, and chosen still by the read-back after.
        kvm.msd.connected = false;
        kvm.did.clear();
        const QJsonObject nameless{{u"size"_s, 5}};
        kvm.msd.stateAfterConnect = withTheDrive(
            QJsonObject{{u"connected"_s, true}, {u"cdrom"_s, false}, {u"rw"_s, true}, {u"image"_s, nameless}});
        kvm.msd.stateAfterDisconnect = withTheDrive(
            QJsonObject{{u"connected"_s, false}, {u"cdrom"_s, false}, {u"rw"_s, true}, {u"image"_s, nameless}});
        done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("An image whose name the KVM does not say (it may be Alpine.iso) was "
                                           "connected by this mount, and was taken out again: nothing is mounted (it "
                                           "is chosen for its drive there still). Look at it on its own page."));
        QCOMPARE(msdDone(kvm),
                 (QList<QByteArray>{"msd params Alpine.iso cdrom 1 rw 0", "msd connected 1", "msd connected 0"}));
        // Taken out, and the read-back after that gets no answer: said, not "nothing is mounted".
        kvm.did.clear();
        kvm.msd.othersChoiceAtConnect = u"Alpine.iso"_s;
        core::testing::Answered silent;
        silent.never = true;
        kvm.msd.stateAfterDisconnect = silent;
        done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("Alpine.iso, which another client of the KVM chose for its drive meanwhile, "
                                           "was connected by this mount in place of debian 13+netinst.iso, and was "
                                           "told to take it out again; what is in its drive since is not known (Device "
                                           "did not respond in time): look at it on its own page."));
        QCOMPARE(msdDone(kvm), (QList<QByteArray>{"msd params debian 13+netinst.iso cdrom 1 rw 0", "msd connected 1",
                                                  "msd connected 0"}));
    }

    // kvmd connects whatever is chosen when its connect comes, and takes a connect only while nothing is connected:
    // another client of the KVM that chooses its own image right after this program's set_params has nothing connected
    // for it; one that chooses it in the moment before the connect has it connected by this program's connect, which
    // takes it out again, reads that back and says so; one that chooses and connects it in that moment has this
    // program's connect refused, and nothing is taken out; and one that connects this program's choice before its look
    // is said as that. (Outside review 4.1; review cx4-1 M1 and L4: the image chosen in the moment was left in,
    // connected and writable, and said "left as it is".)
    void aMountTakesOutWhatItsConnectPutInAndNothingAnotherConnected()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        kvm.msd.othersChoiceAfterParams = u"Alpine.iso"_s;
        base::Result<MediaDone> done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QVERIFY2(QString::fromStdString(saidOf(done))
                     .startsWith("The KVM's drive does not have debian 13+netinst.iso "
                                 "chosen as it was asked"_L1),
                 saidOf(done).c_str());
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd params debian 13+netinst.iso cdrom 1 rw 0"});
        QCOMPARE(kvm.msd.chosen, u"Alpine.iso"_s);
        QVERIFY(!kvm.msd.connected);
        // Chosen in the moment before the connect: connected by it, taken out again, read back, and said.
        kvm.did.clear();
        kvm.msd.othersChoiceAtConnect = u"Alpine.iso"_s;
        const int questions = kvm.asked("/api/msd");
        done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("Alpine.iso, which another client of the KVM chose for its drive "
                                           "meanwhile, was connected by this mount in place of debian 13+netinst.iso, "
                                           "and was taken out again: nothing is mounted (it is chosen for its drive "
                                           "there still). Look at it on its own page."));
        QCOMPARE(msdDone(kvm), (QList<QByteArray>{"msd params debian 13+netinst.iso cdrom 1 rw 0", "msd connected 1",
                                                  "msd connected 0"}));
        // (Its drive asked before the choice, after it, after the connect and after the taking out.)
        QCOMPARE(kvm.asked("/api/msd"), questions + 4);
        QVERIFY(!kvm.msd.connected);
        QCOMPARE(kvm.msd.chosen, u"Alpine.iso"_s);
        // Chosen and connected in that moment: kvmd refuses this connect, and nothing is taken out.
        kvm.did.clear();
        kvm.msd.othersMountAtConnect = u"Alpine.iso"_s;
        done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), std::string("The KVM did not connect its drive: MSD is connected to Server, but "
                                           "shouldn't for this operation"));
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd params debian 13+netinst.iso cdrom 1 rw 0"});
        QVERIFY(kvm.msd.connected);
        QCOMPARE(kvm.msd.chosen, u"Alpine.iso"_s);
        QVERIFY(kvm.msd.rw);
        // This program's choice connected by another before its look: said as that, and nothing connected by it or
        // taken out.
        kvm.msd.connected = false;
        kvm.did.clear();
        kvm.msd.othersConnectAfterParams = true;
        done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("debian 13+netinst.iso was connected to the KVM's drive by another client "
                                           "of the KVM meanwhile, not by this mount, which connected nothing: look at "
                                           "it on its own page."));
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd params debian 13+netinst.iso cdrom 1 rw 0"});
        QVERIFY(kvm.msd.connected);
        QCOMPARE(kvm.msd.chosen, u"debian 13+netinst.iso"_s);
    }

    // What this program's connect put in, another client's choice, is taken out again; where the KVM says it is in its
    // drive still after that, or refuses it, that is said, and never that nothing is mounted. (Review cx4-1 M1.)
    void anotherClientsChoiceThatStaysInIsSaid()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        kvm.msd.othersChoiceAtConnect = u"Alpine.iso"_s;
        kvm.msd.disconnectsNothing = true;
        base::Result<MediaDone> done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("Alpine.iso, which another client of the KVM chose for its drive "
                                           "meanwhile, was connected by this mount in place of debian 13+netinst.iso, "
                                           "and the KVM says it is still in its drive after it was told to take it "
                                           "out: look at it on its own page."));
        QVERIFY(kvm.msd.connected);
        kvm.msd.disconnectsNothing = false;
        kvm.msd.connected = false;
        kvm.msd.othersChoiceAtConnect = u"Alpine.iso"_s;
        kvm.msd.disconnectRefused = true;
        done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("Alpine.iso, which another client of the KVM chose for its drive "
                                           "meanwhile, was connected by this mount in place of debian 13+netinst.iso, "
                                           "and could not be told to take it out again (The KVM's drive is busy with "
                                           "something else: Performing another MSD operation, please try again): look "
                                           "at it on its own page."));
        QVERIFY(kvm.msd.connected);
        QCOMPARE(msdDone(kvm).constLast(), QByteArray("msd connected 1"));
    }

    void aRestartAfterTheMountPressesTheBoard()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.atx = true;
        kvm.powerWord = u"on"_s;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        QVERIFY(got(device.mediaOffer()).restart);
        MediaMount mount = asked(u"Alpine.iso"_s);
        mount.restart = true;
        base::Result<MediaDone> done = mounted(device, mount);
        QVERIFY2(done, saidOf(done).c_str());
        QVERIFY(kvm.did.contains("atx reset"));
        QVERIFY2(done.value().note.startsWith("It was restarted with its reset button"_L1), saidOf(done).c_str());
        QVERIFY(done.value().note.endsWith(
            "pick the KVM's drive in its boot menu where it does not start from it by itself."_L1));
        // One that is off is switched on with its power button. (Off, it shows no picture: a board that says off beside
        // one is no witness, below.)
        kvm.msd.connected = false;
        kvm.powerWord = u"off"_s;
        kvm.signal = false;
        QTest::qWait(400);
        static_cast<void>(lookedAt(device));
        kvm.did.clear();
        kvm.whenPressed = [&kvm](const QByteArray&) { kvm.powerWord = u"on"_s; };
        done = mounted(device, mount);
        QVERIFY2(done, saidOf(done).c_str());
        QVERIFY(kvm.did.contains("atx power"));
        QVERIFY2(done.value().note.startsWith("It was off, and was switched on with its power button"_L1),
                 saidOf(done).c_str());
        // A board that does not say whether it is on: reset, and that said, not that it was restarted (review cx4-1
        // L4).
        kvm.msd.connected = false;
        kvm.whenPressed = {};
        kvm.powerWord = u"unknown"_s;
        kvm.did.clear();
        done = mounted(device, mount);
        QVERIFY2(done, saidOf(done).c_str());
        QVERIFY(kvm.did.contains("atx reset"));
        QCOMPARE(saidOf(done), std::string("It was reset with its reset button (whether it was on was not known: if it "
                                           "was off, it is off still): pick the KVM's drive in its boot menu where it "
                                           "does not start from it by itself."));
        // (The owner's rule of 2026-11-19.) Stock kvmd's lights: a lit disk light beside a dark power light is a
        // machine that is on, and it is reset, never pressed to switch on.
        kvm.msd.connected = false;
        kvm.powerWord.reset();
        kvm.powerLight = false;
        kvm.diskLight = true;
        kvm.signal = true;
        kvm.did.clear();
        QTest::qWait(400);
        static_cast<void>(lookedAt(device));
        done = mounted(device, mount);
        QVERIFY2(done, saidOf(done).c_str());
        QVERIFY(kvm.did.contains("atx reset") && !kvm.did.contains("atx power"));
        QVERIFY2(done.value().note.startsWith("It was restarted with its reset button"_L1), saidOf(done).c_str());
        // Its disk light dark as well, beside the picture: its power light probably not wired, and whether it is on not
        // known. Its power button is not pressed: the image is mounted, and it was not restarted, said why.
        kvm.msd.connected = false;
        kvm.diskLight = false;
        kvm.did.clear();
        QTest::qWait(400);
        QCOMPARE(lookedAt(device).powerNoteCode, u"power_led_unwired"_s);
        done = mounted(device, mount);
        QVERIFY(!done);
        QCOMPARE(codeOf(done), std::string("power_unknown"));
        QVERIFY2(QString::fromStdString(saidOf(done))
                     .startsWith("The image is mounted, and Rack 4 was not restarted: Nothing was pressed: whether "
                                 "this machine is on is not known."_L1),
                 saidOf(done).c_str());
        QVERIFY(kvm.msd.connected);
        QVERIFY(!kvm.did.contains("atx power") && !kvm.did.contains("atx reset"));
        kvm.powerLight.reset();
        kvm.powerWord = u"on"_s;
        // A board that refuses: the image is mounted, and that is said.
        kvm.msd.connected = false;
        kvm.whenPressed = {};
        kvm.instead.insert("/api/atx/click", refusal(400, u"BadRequestError"_s, u"ATX is not available"_s));
        QTest::qWait(400);
        done = mounted(device, mount);
        QVERIFY(!done);
        QVERIFY2(QString::fromStdString(saidOf(done))
                     .startsWith("The image is mounted, and Rack 4 was not "
                                 "restarted: "_L1),
                 saidOf(done).c_str());
        QVERIFY(kvm.msd.connected);
    }

    void aRestartAfterTheMountPressesNoPowerButtonOfAKvmOfSeveralInputsBesideAPicture()
    {
        // (The owner's rule of 2026-11-19: the merged review of 0.2.0, A-M1.) A KVM that has shown several inputs, its
        // board saying off beside a picture: its board may be another computer's, and its card says Power unknown. Its
        // power button is not pressed for the restart: the image is mounted, and the machine was not restarted, said
        // why. Nothing of the board is pressed, its reset button neither.
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.atx = true;
        kvm.powerLight = false;
        kvm.signal = true;
        kvm.ports = {true, true};
        KvmdDevice device(kvm.where(), kvm.password, quick());
        const core::Status status = lookedAt(device);
        QCOMPARE(status.ports.size(), 2);
        QVERIFY(status.powerNoteCode.isEmpty());
        QVERIFY(got(device.mediaOffer()).restart);
        MediaMount mount = asked(u"Alpine.iso"_s);
        mount.restart = true;
        const base::Result<MediaDone> done = mounted(device, mount);
        QVERIFY(!done);
        QCOMPARE(codeOf(done), std::string("power_unknown"));
        QVERIFY2(QString::fromStdString(saidOf(done))
                     .startsWith("The image is mounted, and Rack 4 was not restarted: Nothing was pressed: whether "
                                 "this machine is on is not known."_L1),
                 saidOf(done).c_str());
        QVERIFY2(QString::fromStdString(saidOf(done)).contains("several inputs"_L1), saidOf(done).c_str());
        QVERIFY(kvm.msd.connected);
        QVERIFY2(!kvm.did.contains("atx power") && !kvm.did.contains("atx reset"), kvm.did.join(", ").constData());
        QCOMPARE(kvm.asked("/api/atx/click"_ba), 0);
    }

    void anImageIsUnmounted()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.msd.chosen = u"Alpine.iso"_s;
        kvm.msd.connected = true;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        // Another than is in it: nothing is taken out.
        base::Result<MediaDone> done = unmounted(device, u"debian 13+netinst.iso"_s);
        QCOMPARE(codeOf(done), std::string("changed"));
        QVERIFY2(QString::fromStdString(saidOf(done))
                     .contains("Alpine.iso is mounted on it now, and not debian "
                               "13+netinst.iso: nothing was taken out"_L1),
                 saidOf(done).c_str());
        // Busy: not now.
        kvm.msd.busy = true;
        done = unmounted(device, u"Alpine.iso"_s);
        QCOMPARE(codeOf(done), std::string("busy"));
        QVERIFY(msdDone(kvm).isEmpty());
        kvm.msd.busy = false;
        done = unmounted(device, u"Alpine.iso"_s);
        QVERIFY2(done, saidOf(done).c_str());
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd connected 0"});
        QVERIFY(done.value().note.isEmpty());
        QVERIFY(!device.status().media);
        // Nothing in it: done, and that said.
        done = unmounted(device, u"Alpine.iso"_s);
        QCOMPARE(saidOf(done), std::string("nothing was mounted"));
        QCOMPARE(msdDone(kvm).size(), 1);
        // One that stays in: said.
        kvm.msd.connected = true;
        kvm.instead.insert("/api/msd/set_connected", core::testing::good());
        done = unmounted(device, u"Alpine.iso"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY(QString::fromStdString(saidOf(done)).contains("still in its drive"_L1));
    }

    void anImageIsDeletedOnceItIsListedNoMore()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.msd.chosen = u"Alpine.iso"_s;
        kvm.msd.connected = true;
        kvm.msd.listedLonger = 3;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        // Not the one in its drive.
        base::Result<MediaDone> done = deleted(device, u"Alpine.iso"_s);
        QCOMPARE(codeOf(done), std::string("busy"));
        QCOMPARE(saidOf(done), std::string("Alpine.iso is mounted: unmount it first."));
        QVERIFY(msdDone(kvm).isEmpty());
        // Listed a moment longer as its watch saw it: asked again until it is not.
        const int before = kvm.asked("/api/msd");
        done = deleted(device, u"debian 13+netinst.iso"_s);
        QVERIFY2(done, saidOf(done).c_str());
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd remove debian 13+netinst.iso"});
        QVERIFY(kvm.asked("/api/msd") >= before + 4);
        // Not there any more: done, and that said.
        done = deleted(device, u"debian 13+netinst.iso"_s);
        QCOMPARE(saidOf(done), std::string("it was not on the KVM any more"));
        QCOMPARE(msdDone(kvm).size(), 1);
        // Listed still, after the while: said.
        kvm.msd.listedLonger = 1000;
        done = deleted(device, u"half.img"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY(QString::fromStdString(saidOf(done)).contains("still on its storage"_L1));
        // What stands against it, and a name that is none.
        kvm.msd.fileSharing = true;
        done = deleted(device, u"Alpine.iso"_s);
        QVERIFY(QString::fromStdString(saidOf(done)).contains("File Sharing"_L1));
        done = deleted(device, u"../x"_s);
        QCOMPARE(codeOf(done), std::string("invalid"));
    }

    // What is in its drive said in no shape qKVM reads is never an empty drive: a deletion, an unmount and a mount are
    // each refused, and nothing is changed. (Outside review 4.7: a deletion went for the image in its drive.)
    void aDriveItSaysInNoShapeIsNoEmptyOne()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.msd.chosen = u"Alpine.iso"_s;
        kvm.msd.connected = true;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        const std::string refused =
            "The KVM does not say what is in its drive in a shape qKVM reads: look at it on its own page.";
        for (const QJsonValue& odd :
             {QJsonValue(u"connected"_s),
              QJsonValue(
                  QJsonObject{{u"connected"_s, u"yes"_s}, {u"image"_s, QJsonObject{{u"name"_s, u"Alpine.iso"_s}}}})}) {
            kvm.msd.driveSaysInstead = odd;
            base::Result<MediaDone> done = deleted(device, u"Alpine.iso"_s);
            QCOMPARE(codeOf(done), std::string("bad_response"));
            QCOMPARE(saidOf(done), refused);
            QCOMPARE(saidOf(unmounted(device, u"Alpine.iso"_s)), refused);
            QCOMPARE(saidOf(mounted(device, asked(u"debian 13+netinst.iso"_s))), refused);
            QVERIFY(msdDone(kvm).isEmpty());
            const base::Result<MediaList> list = listOf(device);
            QVERIFY(list);
            QCOMPARE(got(list.value().mounted).image, QString(core::kvmd::unreadDrive));
        }
        QVERIFY(kvm.msd.images.contains("Alpine.iso"_L1));
    }

    // An image in its drive whose name it does not say as one qKVM hands back (none, or one of no image's shape) may be
    // any image of its storage: a deletion of every one is refused while it is in, and a mount of the one its name
    // reads as is no "It was mounted so already"; it is taken out by the id the box listed. (Review cx4-1 M2: it went
    // by the name it reads as, "an image" where it had none, and a deletion of the image in the drive went ahead.)
    void anImageWhoseNameCannotBeReadStandsAgainstADeletion()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.msd.chosen = u"Alpine.iso"_s;
        kvm.msd.connected = true;
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        const auto saysItsImage = [&kvm](const QJsonValue& image) {
            kvm.msd.driveSaysInstead =
                QJsonObject{{u"connected"_s, true}, {u"cdrom"_s, true}, {u"rw"_s, false}, {u"image"_s, image}};
        };
        saysItsImage(QJsonObject{{u"size"_s, 200'000'000}});
        base::Result<MediaDone> done = deleted(device, u"Alpine.iso"_s);
        QCOMPARE(codeOf(done), std::string("busy"));
        QCOMPARE(saidOf(done), std::string("An image is in its drive, by a name qKVM does not read as one of its "
                                           "images, so it may be Alpine.iso: unmount it first."));
        // A name that reads as one of its images, and is not one.
        saysItsImage(QJsonObject{{u"name"_s, u"debian\n13+netinst.iso"_s}, {u"size"_s, 700'000'000}});
        done = deleted(device, u"Alpine.iso"_s);
        QCOMPARE(codeOf(done), std::string("busy"));
        QVERIFY2(QString::fromStdString(saidOf(done)).startsWith("debian 13+netinst.iso is in its drive, by a name"_L1),
                 saidOf(done).c_str());
        done = mounted(device, asked(u"debian 13+netinst.iso"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("debian 13+netinst.iso is mounted on it now: unmount it first."));
        QVERIFY(msdDone(kvm).isEmpty());
        // Taken out by the id the box listed.
        const base::Result<MediaList> list = listOf(device);
        QVERIFY(list);
        const QString listed = got(list.value().mounted).image;
        QVERIFY2(core::kvmd::isNameless(listed), qPrintable(listed));
        done = unmounted(device, listed);
        // (The made-up KVM goes on saying the same of its drive after it, so it is said to be in still: what matters
        // here is that it was told to take that one out, not refused as another.)
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd connected 0"});
        QVERIFY2(QString::fromStdString(saidOf(done)).contains("still in its drive"_L1), saidOf(done).c_str());
        // Another, by a name that cannot be read either: nothing taken out, and the id said as what it was.
        kvm.msd.connected = true;
        kvm.did.clear();
        saysItsImage(QJsonObject{{u"name"_s, 7}});
        done = unmounted(device, listed);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), std::string("7 is mounted on it now, and not the image it had in its drive when it was "
                                           "looked at: nothing was taken out. Look at it again."));
        QVERIFY(msdDone(kvm).isEmpty());
        QVERIFY(kvm.msd.images.contains("Alpine.iso"_L1));
    }

    // A list of its storage that is no list, or not read whole, never says an image is not there: a deletion of one it
    // does not find there is refused, and nothing is removed; nor is one said gone that such a list no longer has.
    // (Outside review 4.6: either was taken for "not there", and the deletion said done.)
    void aListNotReadWholeNeverSaysAnImageIsGone()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        kvm.msd.imagesSayInstead = QJsonValue(u"none"_s);
        base::Result<MediaDone> done = deleted(device, u"Alpine.iso"_s);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QCOMPARE(saidOf(done), std::string("The KVM's list of its own storage could not be read whole (it is in no "
                                           "shape qKVM reads, or longer than the 2,000 images qKVM reads), so whether "
                                           "Alpine.iso is there is not known: look at it on its own page."));
        QVERIFY(msdDone(kvm).isEmpty());
        // An image of a name it hands back, in no image's shape: the same.
        kvm.msd.imagesSayInstead = QJsonObject{{u"Alpine.iso"_s, 3}};
        QCOMPARE(codeOf(deleted(device, u"Alpine.iso"_s)), std::string("bad_response"));
        QVERIFY(msdDone(kvm).isEmpty());
        // Listed whole before the deletion, and not after it: not said gone.
        kvm.msd.imagesSayInstead.reset();
        kvm.msd.imagesUnreadAfterRemove = true;
        done = deleted(device, u"Alpine.iso"_s);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QVERIFY2(QString::fromStdString(saidOf(done))
                     .endsWith("so whether Alpine.iso is gone is not known: look at it "
                               "on its own page."_L1),
                 saidOf(done).c_str());
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd remove Alpine.iso"});
    }

    void aRefusalOfKvmdsIsSaidInItsWords()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        static_cast<void>(lookedAt(device));
        kvm.instead.insert("/api/msd/set_params",
                           refusal(409, u"MsdIsBusyError"_s, u"Performing another MSD operation, please try again"_s));
        base::Result<MediaDone> done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("busy"));
        QCOMPARE(saidOf(done), std::string("The KVM's drive is busy with something else: Performing another MSD "
                                           "operation, please try again"));
        kvm.instead.insert("/api/msd/set_params", refusal(400, u"MsdUnknownImageError"_s, u"No such\nimage"_s));
        done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), std::string("The KVM did not choose the image: No such image"));
        kvm.instead.insert("/api/msd/set_params", core::testing::Answered{502, "<html>"});
        done = mounted(device, asked(u"Alpine.iso"_s));
        QCOMPARE(saidOf(done), std::string("The KVM did not choose the image (HTTP 502)"));
    }
};

QTEST_MAIN(TestKvmdMedia)
#include "test_kvmd_media.moc"
