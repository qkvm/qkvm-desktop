#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/jetkvm_media.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/media_place.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/strict_json.h>
#include <qkvm/core/tls.h>

#include "jetkvm_by_hand.h"
#include "jetkvm_storage_by_hand.h"
#include "jetkvm_viewer_by_hand.h"
#include "memory_secrets.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
namespace core = qkvm::core;
using core::JetKvmDevice;
using core::MediaDone;
using core::MediaList;
using core::MediaMount;
using core::Status;
using core::testing::JetKvmByHand;
using core::testing::JetKvmStorageByHand;
using core::testing::JetKvmViewerByHand;

namespace {

const QString debian = u"debian-13.iso"_s;
const QString alpine = u"alpine-3.21.iso"_s;

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
    pace.pacing = std::make_shared<core::SignInPacing>();
    pace.viewer.connection.watchEveryMs = 100;
    pace.viewer.unwatchedEveryMs = 100;
    pace.viewer.retryPauseMs = 300;
    return pace;
}

template <class T>
base::Result<T> answerTo(const std::function<void(std::function<void(base::Result<T>)>)>& ask)
{
    std::optional<base::Result<T>> answer;
    ask([&answer](base::Result<T> what) { answer = std::move(what); });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 30'000)) {
        return base::Error{"never", "Never answered."};
    }
    return *answer;
}

template <class T>
std::string codeOf(const base::Result<T>& result)
{
    return result ? std::string("(ok)") : result.error().code;
}

template <class T>
QString saidOf(const base::Result<T>& result)
{
    return result ? QString() : QString::fromStdString(result.error().message);
}

Status lookedAt(JetKvmDevice& device)
{
    QSignalSpy over(&device, &JetKvmDevice::looked);
    device.look();
    if (!over.wait(20'000)) {
        return {};
    }
    return device.status();
}

// What an optional holds, or what it is made of where it holds nothing (whose check is the test's own, before it).
template <class T>
T got(const std::optional<T>& held)
{
    return held.value_or(T{});
}

core::json::Value parsed(const QByteArray& text)
{
    return core::json::parse(text).value();
}

// How a KVM of the wall is, asked now.
Status statusOf(core::LocalWall& wall, const QString& id)
{
    const base::Result<QHash<QString, Status>> all =
        answerTo<QHash<QString, Status>>([&](auto done) { wall.askStatus(std::move(done)); });
    return all ? all.value().value(id) : Status{};
}

// A JetKVM by hand with its storage (two images on it), its viewer, and a driver of it.
struct Rig {
    explicit Rig(const JetKvmDevice::Pace& pace = quick(), bool storage = true) : device(where(jet), u"secret"_s, pace)
    {
        if (storage) {
            viewer.storage = &images;
        }
        images.add(debian, 700'000'000);
        images.add(alpine, 62'000'000);
    }
    static JetKvmDevice::Where where(const JetKvmByHand& jet)
    {
        JetKvmDevice::Where at;
        at.address = jet.address();
        return at;
    }
    // Its card asks until its connection is up, and a look over it says what is in its drive.
    bool connected()
    {
        QElapsedTimer waited;
        waited.start();
        while (!device.viewer().isOpen() && waited.elapsed() < 20'000) {
            device.askSnapshot(320, [](const base::Result<core::Snapshot>&) {});
            QTest::qWait(100);
        }
        if (!device.viewer().isOpen()) {
            return false;
        }
        const Status status = lookedAt(device);
        return status.state == u"online"_s;
    }
    base::Result<MediaList> list()
    {
        return answerTo<MediaList>([this](auto done) { device.askMedia(std::move(done)); });
    }
    base::Result<MediaDone> mount(const QString& image, const QString& drive = u"cd"_s, bool writable = false,
                                  const QString& boot = u"none"_s, bool restart = false)
    {
        const MediaMount asked{image, u"jet"_s, drive, writable, boot, restart, QString()};
        return answerTo<MediaDone>([&](auto done) { device.mountMedia(asked, std::move(done)); });
    }
    base::Result<MediaDone> unmount(const QString& image)
    {
        return answerTo<MediaDone>([&](auto done) { device.unmountMedia(image, std::move(done)); });
    }
    base::Result<MediaDone> remove(const QString& image)
    {
        return answerTo<MediaDone>([&](auto done) { device.deleteMedia(image, std::move(done)); });
    }
    int calledLike(const QString& beginning)
    {
        int found = 0;
        for (const QString& one : images.called()) {
            found += one.startsWith(beginning) ? 1 : 0;
        }
        return found;
    }

    JetKvmStorageByHand images;
    JetKvmByHand jet;
    JetKvmViewerByHand viewer{jet};
    JetKvmDevice device;
};

QStringList idsOf(const MediaList& list)
{
    QStringList ids;
    for (const core::MediaImage& one : list.images) {
        ids.append(one.id);
    }
    return ids;
}

} // namespace

// Virtual media on this PC's own JetKVMs (GAPS A3; the web's 0.3.0, jetkvm.py "virtual media" and server.py's media
// routes; test_virtual_media_on_a_jetkvm.py of web 0.3.1): its own image storage, one image of it in its drive as a
// CD-ROM drive or a disk, read-only either way, one deleted from it; over the connection its card holds, which is only
// ever to the JetKVM it was added for; and through this PC's own wall. Against the JetKVM by hand, whose storage is the
// web's stand-in's (jetkvm_storage_by_hand.h).
class TestJetKvmMedia : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(core::useWindowsOwnTls());
        QVERIFY2(core::testing::JetKvmPeerByHand::filmIsThere(), core::testing::JetKvmPeerByHand::noFilm);
    }

    // --- what it says, read as the web reads it --------------------------------------------------------------------

    void itsListIsReadAsTheWebReadsIt()
    {
        core::kvmd::MediaNotes notes;
        const core::json::Value made = parsed(R"("2026-10-30T09:12:44Z")"_ba);
        notes.carried(u"key"_s, u"Mine.iso"_s, u"ab"_s, 5000, core::jetkvm::stampOf(&made));
        const core::json::Value said =
            parsed(R"({"files":[{"filename":"zeta.img","size":10,"createdAt":"2026-10-30T09:12:44Z"},)"
                   R"({"filename":"Mine.iso","size":5000,"createdAt":"2026-10-30T09:12:44Z"},)"
                   R"({"filename":"mine.iso.incomplete","size":5,"createdAt":"x"},)"
                   R"({"filename":"mine.iso","size":7.5,"createdAt":12},)"
                   R"({"filename":"folder/a.iso","size":1},{"filename":".hidden","size":1},{"filename":7},"no entry",)"
                   R"({"filename":"a.iso.incomplete.incomplete","size":3}]})"_ba);
        const core::jetkvm::Files files = core::jetkvm::filesOf(said, notes, u"key"_s);
        // By name without regard to case, the whole one first; a part of an upload by its name, said to be one.
        QStringList ids;
        for (const core::MediaImage& one : files.images) {
            ids.append(one.id + u'|' + one.name + u'|' + (one.partial ? u"part"_s : u"whole"_s) + u'|' +
                       (one.size ? QString::number(*one.size) : u"-"_s) + u'|' + one.sha256);
        }
        QCOMPARE(ids, (QStringList{u"a.iso.incomplete.incomplete|a.iso.incomplete|part|3|"_s,
                                   u"Mine.iso|Mine.iso|whole|5000|ab"_s, u"mine.iso|mine.iso|whole|-|"_s,
                                   u"mine.iso.incomplete|mine.iso|part|5|"_s, u"zeta.img|zeta.img|whole|10|"_s}));
        // No name the web hands back, or none at all: left out.
        QCOMPARE(files.leftOut, 4);
        QVERIFY(files.listed.contains(u"Mine.iso"_s));
        // Its SHA-256 only while it is the image this program carried: another stamp, another size, another JetKVM.
        QCOMPARE(
            core::jetkvm::filesOf(parsed(R"({"files":[{"filename":"Mine.iso","size":5000,"createdAt":"later"}]})"_ba),
                                  notes, u"key"_s)
                .images.value(0)
                .sha256,
            QString());
        QCOMPARE(core::jetkvm::filesOf(parsed(R"({"files":[{"filename":"Mine.iso","size":5000,)"
                                              R"("createdAt":"2026-10-30T09:12:44Z"}]})"_ba),
                                       notes, u"another"_s)
                     .images.value(0)
                     .sha256,
                 QString());
        // What says no list lists nothing.
        QVERIFY(core::jetkvm::filesOf(parsed(R"({"files":"none"})"_ba), notes, u"key"_s).images.isEmpty());
        QVERIFY(core::jetkvm::filesOf(parsed("null"_ba), notes, u"key"_s).images.isEmpty());
        // And is not read whole, nor is a list with an entry whose name cannot be read, or one beyond the most: what
        // is not found in it is not known to be gone (outside review 4.6). Names it does not hand back are no image of
        // its, and leave it whole.
        QVERIFY(!files.whole);
        QVERIFY(!core::jetkvm::filesOf(parsed(R"({"files":"none"})"_ba), notes, u"key"_s).whole);
        QVERIFY(!core::jetkvm::filesOf(parsed("null"_ba), notes, u"key"_s).whole);
        QVERIFY(
            !core::jetkvm::filesOf(parsed(R"({"files":[{"filename":"a.iso"},{"size":1}]})"_ba), notes, u"key"_s).whole);
        QVERIFY(core::jetkvm::filesOf(parsed(R"({"files":[{"filename":"a.iso"},{"filename":"folder/b.iso"}]})"_ba),
                                      notes, u"key"_s)
                    .whole);
        QVERIFY(core::jetkvm::filesOf(parsed(R"({"files":[]})"_ba), notes, u"key"_s).whole);
        // (Made as JSON, not written out: moc does not read raw strings, and lost this class in one cut in two.)
        QJsonArray many;
        for (int at = 0; at <= core::mostMediaImages; ++at) {
            many.append(QJsonObject{{u"filename"_s, u"a%1.iso"_s.arg(at)}});
        }
        const core::jetkvm::Files tooMany = core::jetkvm::filesOf(
            parsed(QJsonDocument(QJsonObject{{u"files"_s, many}}).toJson(QJsonDocument::Compact)), notes, u"key"_s);
        QCOMPARE(tooMany.images.size(), qsizetype{core::mostMediaImages});
        QVERIFY(!tooMany.whole);
    }

    void whatIsInItsDriveIsReadAsTheWebReadsIt()
    {
        const core::kvmd::MediaNotes notes;
        const QHash<QString, core::jetkvm::Files::Listed> none;
        QVERIFY(!core::jetkvm::mountedOf(parsed("null"_ba), none, notes, u"key"_s));
        std::optional<core::MountedMedia> now = core::jetkvm::mountedOf(
            parsed(R"({"source":"Storage","mode":"CDROM","filename":"debian-13.iso","size":7})"_ba), none, notes,
            u"key"_s);
        QVERIFY(now);
        QCOMPARE(got(now).drive, u"cd"_s);
        QCOMPARE(got(now).image, debian);
        QCOMPARE(got(now).name, debian);
        QVERIFY(got(now).readOnly);
        QVERIFY(!got(now).first);
        now = core::jetkvm::mountedOf(parsed(R"({"source":"Storage","mode":"Disk","filename":"a\nb/c"})"_ba), none,
                                      notes, u"key"_s);
        QCOMPARE(got(now).drive, u"disk"_s);
        QCOMPARE(got(now).name, u"a b/c"_s);
        // A name it does not hand back, or none, is said as a line, and goes by an id that is no name of an image:
        // never the one it reads as (review cx4-1 M2: "an image", "7" and "a b" are names of images). The same again
        // for the same, another for another.
        QVERIFY2(core::kvmd::isNameless(got(now).image), qPrintable(got(now).image));
        QVERIFY(!core::jetkvm::isOneName(got(now).image));
        QCOMPARE(got(core::jetkvm::mountedOf(parsed(R"({"source":"Storage","mode":"Disk"})"_ba), none, notes, u"key"_s))
                     .name,
                 u"an image"_s);
        const auto nameless = [&](const char* said) {
            return got(core::jetkvm::mountedOf(parsed(said), none, notes, u"key"_s)).image;
        };
        for (const char* odd :
             {R"({"source":"Storage","mode":"Disk"})", R"({"source":"Storage","filename":7})",
              R"({"source":"Storage","filename":"a\tb"})", R"({"source":"Storage","filename":null})"}) {
            QVERIFY2(core::kvmd::isNameless(nameless(odd)), odd);
            QCOMPARE(nameless(odd), nameless(odd));
        }
        QVERIFY(nameless(R"({"source":"Storage","filename":7})") != nameless(R"({"source":"Storage","filename":8})"));
        // From where it does not say, or from where it does not know: what is in it cannot be read (review cx4-1 M2: it
        // was taken for an image from an address, which never stands against a deletion).
        for (const char* odd : {R"({"mode":"CDROM","filename":"debian-13.iso","size":7})",
                                R"({"source":7,"mode":"CDROM","filename":"debian-13.iso"})",
                                R"({"source":"storage","mode":"CDROM","filename":"debian-13.iso"})",
                                R"({"source":"USB","mode":"CDROM","filename":"debian-13.iso"})"}) {
            now = core::jetkvm::mountedOf(parsed(odd), none, notes, u"key"_s);
            QVERIFY2(now, odd);
            QCOMPARE(got(now).image, QString(core::kvmd::unreadDrive));
        }
        // Sent from a browser at its own page: none of its storage, said as what it is.
        now = core::jetkvm::mountedOf(parsed(R"({"source":"WebRTC","mode":"CDROM","filename":"debian-13.iso"})"_ba),
                                      none, notes, u"key"_s);
        QVERIFY2(got(now).image.startsWith(u"(a browser)/"_s), qPrintable(got(now).image));
        QCOMPARE(got(now).name, u"an image from a browser, mounted at the JetKVM's own page"_s);
        // (No SHA-256 of an image of its storage that a name it does not hand back reads as.)
        core::kvmd::MediaNotes carried;
        carried.carried(u"key"_s, debian, u"ab"_s, 7, std::nullopt);
        const QHash<QString, core::jetkvm::Files::Listed> listedDebian{
            {debian, core::jetkvm::Files::Listed{7, std::nullopt}}};
        QCOMPARE(got(core::jetkvm::mountedOf(parsed(R"({"source":"Storage","filename":"debian-13.iso","size":7})"_ba),
                                             listedDebian, carried, u"key"_s))
                     .sha256,
                 u"ab"_s);
        now = core::jetkvm::mountedOf(parsed(R"({"source":"Storage","filename":"debian-13.iso\n","size":7})"_ba),
                                      listedDebian, carried, u"key"_s);
        QCOMPARE(got(now).name, debian);
        QVERIFY(got(now).sha256.isEmpty());
        // What is neither null nor an object is no empty drive: it cannot be read, and no image is called so; nor is a
        // drive of no kind it says taken for a disk (outside review 4.7).
        for (const char* odd : {"7", "\"Storage\"", "[]", "true"}) {
            now = core::jetkvm::mountedOf(parsed(odd), none, notes, u"key"_s);
            QVERIFY2(now, odd);
            QCOMPARE(got(now).image, QString(core::kvmd::unreadDrive));
        }
        QVERIFY(!core::jetkvm::isOneName(QString(core::kvmd::unreadDrive)));
        for (const char* odd : {R"({"source":"Storage","filename":"a.iso"})", R"({"source":"Storage","mode":"Floppy"})",
                                R"({"source":"Storage","mode":7})"}) {
            now = core::jetkvm::mountedOf(parsed(odd), none, notes, u"key"_s);
            QVERIFY2(now && got(now).drive.isEmpty(), odd);
        }
        // Mounted at its own page from an address: what the address is, is not said (it may hold a password).
        now = core::jetkvm::mountedOf(
            // (Not a raw string: moc reads the two slashes in one as a comment.)
            parsed("{\"source\":\"HTTP\",\"mode\":\"CDROM\",\"url\":\"https://user:secret@192.0.2.7/boot.iso\","
                   "\"size\":1}"_ba),
            none, notes, u"key"_s);
        QVERIFY2(got(now).image.startsWith(u"(an address)/"_s), qPrintable(got(now).image));
        QCOMPARE(got(now).image.size(), qsizetype{13 + 16});
        QCOMPARE(got(now).name, u"an image from an address, mounted at the JetKVM's own page"_s);
        QVERIFY(!(got(now).image + got(now).name).contains(u"192.0.2.7"_s));
        // Its id is that address's mount's own: the same again for the same, another for another address, another
        // mode or none said (outside review 4.4: one id was every one's).
        const auto idOf = [&](const char* said) {
            return got(core::jetkvm::mountedOf(parsed(said), none, notes, u"key"_s)).image;
        };
        const QString one =
            idOf("{\"source\":\"HTTP\",\"mode\":\"CDROM\",\"url\":\"https://192.0.2.7/a.iso\",\"size\":1}");
        QCOMPARE(idOf("{\"source\":\"HTTP\",\"mode\":\"CDROM\",\"url\":\"https://192.0.2.7/a.iso\",\"size\":1}"), one);
        QVERIFY(idOf("{\"source\":\"HTTP\",\"mode\":\"CDROM\",\"url\":\"https://192.0.2.7/b.iso\",\"size\":1}") != one);
        QVERIFY(idOf("{\"source\":\"HTTP\",\"mode\":\"Disk\",\"url\":\"https://192.0.2.7/a.iso\",\"size\":1}") != one);
        QVERIFY(idOf("{\"source\":\"HTTP\",\"mode\":\"CDROM\",\"size\":1}") != one);
        QVERIFY(!core::jetkvm::isOneName(one));
        // Its space, its offer, an upload's start, its names and ids.
        const core::jetkvm::Space space =
            core::jetkvm::spaceOf(parsed(R"({"bytesUsed":762000000,"bytesFree":6238000000})"_ba));
        QCOMPARE(space.used, std::optional<qint64>(762'000'000));
        QCOMPARE(space.free, std::optional<qint64>(6'238'000'000));
        QVERIFY(!core::jetkvm::spaceOf(parsed(R"({"bytesUsed":-1,"bytesFree":"a lot"})"_ba)).used);
        QVERIFY(core::jetkvm::isOneName(u"debian 13+netinst (amd64).iso"_s));
        QVERIFY(core::jetkvm::isOneName(u"x.iso.incomplete"_s));
        for (const QString& no : {u"a/b.iso"_s, u".a"_s, u"a."_s, u" a"_s, u"a\nb"_s, QString(201, u'a'), QString()}) {
            QVERIFY2(!core::jetkvm::isOneName(no), qPrintable(no));
        }
        QVERIFY(core::jetkvm::isUploadId(u"upload_0a1b2c3d-0a1b-4c3d-8e9f-0a1b2c3d4e5f"_s));
        QVERIFY(!core::jetkvm::isUploadId(u"upload_0A1B2C3D-0a1b-4c3d-8e9f-0a1b2c3d4e5f"_s));
        QVERIFY(!core::jetkvm::isUploadId(u"upload_0a1b2c3d-0a1b-4c3d-8e9f-0a1b2c3d4e5f/../x"_s));
        const core::json::Value a = parsed(R"("a")"_ba);
        const core::json::Value alsoA = parsed(R"(" a ")"_ba);
        const core::json::Value b = parsed(R"("b")"_ba);
        const core::json::Value yes = parsed("true"_ba);
        QVERIFY(core::jetkvm::stampOf(&a));
        QVERIFY(core::jetkvm::stampOf(&a) == core::jetkvm::stampOf(&alsoA)); // (one line, as the web's plain)
        QVERIFY(core::jetkvm::stampOf(&a) != core::jetkvm::stampOf(&b));
        QVERIFY(!core::jetkvm::stampOf(&yes));
        QVERIFY(!core::jetkvm::stampOf(nullptr));
    }

    // A change of its drive whose call never went out was not done, and is said so (the web's _not_sent, as
    // test_a_jetkvm_press_that_never_went_out.py has it); one that went out and got no answer in time may have been,
    // and is said as it ended.
    void aChangeThatNeverWentOutIsSaidNotDone()
    {
        const base::Error never =
            core::jetkvm::notSent(base::Error{"dropped", "The JetKVM closed the channel it is asked over"});
        QCOMPARE(never.code, std::string("action_failed"));
        QCOMPARE(never.message, std::string("It was not done: The JetKVM closed the channel it is asked over"));
        QVERIFY(!JetKvmDevice::mayHaveBeenDone(never));
        for (const char* code : {"timeout", "in_use", "action_failed", "another_machine"}) {
            const base::Error kept = core::jetkvm::notSent(base::Error{code, "as it ended"});
            QCOMPARE(kept.code, std::string(code));
            QCOMPARE(kept.message, std::string("as it ended"));
        }
        QVERIFY(JetKvmDevice::mayHaveBeenDone(core::jetkvm::notSent(base::Error{"timeout", "no answer"})));
    }

    // --- what it offers --------------------------------------------------------------------------------------------

    // A look over its connection asks what is in its drive (the web's _read_status), and from then on it offers its
    // media, on its card and in its `can`; a look that goes the plain way (no connection: a look never connects) learns
    // nothing of it.
    void aLookOverItsConnectionFindsItsMedia()
    {
        Rig rig;
        QVERIFY(!lookedAt(rig.device).capabilities.value_or(QStringList()).contains(u"media"_s));
        QVERIFY(!rig.device.mediaOffer());
        QVERIFY(rig.images.called().isEmpty());
        QVERIFY(rig.connected());
        const Status status = rig.device.status();
        QVERIFY(status.capabilities.value_or(QStringList()).contains(u"media"_s));
        QVERIFY(status.offersMedia);
        QVERIFY(!status.media);
        const std::optional<core::MediaOffer> offer = rig.device.mediaOffer();
        QVERIFY(offer);
        QCOMPARE(got(offer).drives, (QStringList{u"cd"_s, u"disk"_s}));
        QVERIFY(got(offer).boot.isEmpty());
        QVERIFY(!got(offer).restart);
        QCOMPARE(got(offer).where, u"device"_s);
        QVERIFY(got(offer).writable.isEmpty());
        QVERIFY(got(offer).canDelete);
        QVERIFY(got(offer).dropsInput);
        // What is put in its drive meanwhile is on its card after the next look.
        {
            const auto held = rig.images.lock();
            rig.images.mounted = QJsonObject{
                {u"source"_s, u"Storage"_s}, {u"mode"_s, u"Disk"_s}, {u"filename"_s, alpine}, {u"size"_s, 62'000'000}};
        }
        const Status after = lookedAt(rig.device);
        QVERIFY(after.media);
        QCOMPARE(got(after.media).image, alpine);
        QCOMPARE(got(after.media).drive, u"disk"_s);
        QVERIFY(rig.images.neverAsked().isEmpty());
    }

    // Its connection given back for nobody watching, a look goes the plain way and does not ask its drive: what was in
    // it at the last look over the connection is not said as what is in it now (the web's every look connects), though
    // it still offers its media; the next look over a connection says it again.
    void aPlainLookSaysNothingOfItsDrive()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.unwatchedMs = 800;
        Rig rig(pace);
        {
            const auto held = rig.images.lock();
            rig.images.mounted = QJsonObject{
                {u"source"_s, u"Storage"_s}, {u"mode"_s, u"Disk"_s}, {u"filename"_s, alpine}, {u"size"_s, 62'000'000}};
        }
        QVERIFY(rig.connected());
        QCOMPARE(got(rig.device.status().media).image, alpine);
        QTRY_VERIFY_WITH_TIMEOUT(!rig.device.viewer().isOpen(), 10'000);
        // Changed at its own page meanwhile.
        {
            const auto held = rig.images.lock();
            rig.images.mounted = QJsonObject{{u"source"_s, u"Storage"_s},
                                             {u"mode"_s, u"CDROM"_s},
                                             {u"filename"_s, debian},
                                             {u"size"_s, 700'000'000}};
        }
        const qsizetype asked = rig.images.called().size();
        const Status plain = lookedAt(rig.device);
        QCOMPARE(plain.state, u"online"_s);
        QVERIFY(!plain.keyboard); // (the plain way: no connection was made for it)
        QCOMPARE(rig.images.called().size(), asked);
        QVERIFY(plain.offersMedia);
        QVERIFY(plain.capabilities.value_or(QStringList()).contains(u"media"_s));
        QVERIFY2(!plain.media, qPrintable(got(plain.media).image));
        QVERIFY(!rig.device.status().media);
        // Over a connection again: as it is now.
        QVERIFY(rig.connected());
        QCOMPARE(got(rig.device.status().media).image, debian);
        QCOMPARE(got(rig.device.status().media).drive, u"cd"_s);
    }

    // As the web's `_vm_said`: a look whose question of its drive got no answer leaves what was known as it was; one
    // that says it has no such method (its firmware put back to an older one) offers nothing from then on.
    void whatWasKnownStaysUntilItSaysItHasNoSuchMethod()
    {
        // (Every other call of a look is answered at once: only the drive's is let go unanswered, its call's whole
        // time.)
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.callWithinMs = 3000;
        Rig rig(pace);
        QVERIFY(rig.connected());
        QVERIFY(rig.device.mediaOffer());
        rig.viewer.silentOn = "getVirtualMediaState";
        Status status = lookedAt(rig.device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(status.offersMedia);
        rig.viewer.silentOn.clear();
        rig.viewer.storage = nullptr;
        status = lookedAt(rig.device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.offersMedia);
        QVERIFY(!status.capabilities.value_or(QStringList()).contains(u"media"_s));
        QVERIFY(!rig.device.mediaOffer());
    }

    // As the web's `_status_at = 0` after a change of its drive (which may bind its USB device anew): its card is due
    // to be looked at again.
    void aChangeOfItsDriveHasItsCardLookedAtAgain()
    {
        JetKvmDevice::Pace pace = quick();
        pace.freshMs = 60'000;
        Rig rig(pace);
        QVERIFY(rig.connected());
        QVERIFY(!rig.device.isStale());
        QVERIFY(rig.mount(debian, u"disk"_s));
        QVERIFY(rig.device.isStale());
        static_cast<void>(lookedAt(rig.device));
        QVERIFY(!rig.device.isStale());
        QVERIFY(rig.unmount(debian));
        QVERIFY(rig.device.isStale());
    }

    void anOlderJetKvmWithoutStorageOffersNothing()
    {
        Rig rig(quick(), false);
        QVERIFY(rig.connected());
        const Status status = rig.device.status();
        QVERIFY(!status.capabilities.value_or(QStringList()).contains(u"media"_s));
        QVERIFY(!status.offersMedia);
        QVERIFY(!status.media);
        QVERIFY(!rig.device.mediaOffer());
    }

    void itsAtxExtensionRestartsTheMachineForTheImage()
    {
        Rig rig;
        rig.viewer.extension = "atx-power";
        QVERIFY(rig.connected());
        QVERIFY(rig.device.mediaOffer().value_or(core::MediaOffer{}).restart);
        QVERIFY(rig.device.status().capabilities.value_or(QStringList()).endsWith(u"media"_s));
        const base::Result<MediaDone> done = rig.mount(debian, u"cd"_s, false, u"none"_s, true);
        QVERIFY2(done, qPrintable(saidOf(done)));
        // (Its computer is off, as its ATX extension says: switched on with its power button.)
        QCOMPARE(done.value().note, u"It was off, and was switched on with its power button: pick the JetKVM's "
                                    u"drive in its boot menu where it does not start from it by itself."_s);
        QStringList pressed;
        for (const auto& [method, params] : rig.viewer.rpcCalls()) {
            if (method == u"setATXPowerAction"_s) {
                pressed.append(params.value(u"action"_s).toString());
            }
        }
        QCOMPARE(pressed, QStringList{u"power-short"_s});
        QVERIFY(rig.images.mountedNow());
    }

    // Its machine was off at the last look, and is switched on at its front panel while the mount goes: the restart
    // asks its ATX extension anew, after the mount and before the press, and resets it, never presses its power button
    // (which shuts a running machine down, or off during its POST). One whose disk light is on is on, whatever its
    // power light says, and is reset. One whose state it does not say in time is reset too, and that is said. (Outside
    // review 4.3: the restart went by the last look; review cx4-1 L4 and L7.) Switched on once the mount's call has
    // come and before it is answered, so that an asking moved before the mount, or one that goes by the last look,
    // reads it off and presses the power button (review cx4-2 L4: it was switched on before the mount began, and only
    // the order check below told an asking before the mount).
    void aRestartAsksAnewWhetherTheMachineIsOff()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.callWithinMs = 2000;
        Rig rig(pace);
        rig.viewer.extension = "atx-power";
        QVERIFY(rig.connected());
        QCOMPARE(rig.device.status().power, std::optional<bool>(false));
        const auto pressed = [&rig] {
            QStringList all;
            for (const auto& [method, params] : rig.viewer.rpcCalls()) {
                if (method == u"setATXPowerAction"_s) {
                    all.append(params.value(u"action"_s).toString());
                }
            }
            return all;
        };
        // (Asked after the last mount's call and before the press began, which asks which extension is on first: not
        // by a look before the mount, nor by one between the press's first question and the press.)
        const auto askedBetween = [&rig] {
            const auto calls = rig.viewer.rpcCalls();
            qsizetype mount = -1;
            qsizetype press = -1;
            for (qsizetype at = 0; at < calls.size(); ++at) {
                mount = calls.at(at).first == u"mountWithStorage"_s ? at : mount;
                press = calls.at(at).first == u"setATXPowerAction"_s ? at : press;
            }
            qsizetype began = press;
            while (began > mount && calls.at(began).first != u"getActiveExtension"_s) {
                --began;
            }
            for (qsizetype at = mount + 1; mount >= 0 && at < began; ++at) {
                if (calls.at(at).first == u"getATXState"_s) {
                    return true;
                }
            }
            return false;
        };
        const auto mounts = [&rig] {
            const auto calls = rig.viewer.rpcCalls();
            return std::ranges::count_if(calls, [](const auto& call) { return call.first == u"mountWithStorage"_s; });
        };
        rig.viewer.slowOn("mountWithStorage", 500);
        QTimer atItsFrontPanel;
        QObject::connect(&atItsFrontPanel, &QTimer::timeout, &atItsFrontPanel,
                         [&rig, &mounts, &atItsFrontPanel, before = mounts()] {
                             if (mounts() > before) {
                                 rig.viewer.machineOn = true;
                                 atItsFrontPanel.stop();
                             }
                         });
        atItsFrontPanel.start(5);
        base::Result<MediaDone> done = rig.mount(debian, u"cd"_s, false, u"none"_s, true);
        rig.viewer.slowOn({}, 0);
        QVERIFY(rig.viewer.machineOn);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note, u"It was restarted with its reset button: pick the JetKVM's drive in its boot menu "
                                    u"where it does not start from it by itself."_s);
        QCOMPARE(pressed(), QStringList{u"reset"_s});
        QVERIFY(askedBetween());
        QVERIFY(rig.unmount(debian));
        // Off now, as asked anew: switched on.
        rig.viewer.machineOn = false;
        done = rig.mount(debian, u"cd"_s, false, u"none"_s, true);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY2(done.value().note.startsWith(u"It was off, and was switched on with its power button"_s),
                 qPrintable(done.value().note));
        QCOMPARE(pressed(), (QStringList{u"reset"_s, u"power-short"_s}));
        QVERIFY(askedBetween());
        QVERIFY(rig.unmount(debian));
        // Its power light off and its disk light on: on, its power light's lead not connected; reset.
        rig.viewer.diskLightOn = true;
        done = rig.mount(debian, u"cd"_s, false, u"none"_s, true);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY2(done.value().note.startsWith(u"It was restarted with its reset button:"_s),
                 qPrintable(done.value().note));
        QCOMPARE(pressed(), (QStringList{u"reset"_s, u"power-short"_s, u"reset"_s}));
        QVERIFY(rig.unmount(debian));
        rig.viewer.diskLightOn = false;
        // Not said in time: reset, whatever the last look saw, and said so.
        rig.viewer.silentOn = "getATXState";
        done = rig.mount(debian, u"cd"_s, false, u"none"_s, true);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note, u"It was reset with its reset button (whether it was on was not known: if it was "
                                    u"off, it is off still): pick the JetKVM's drive in its boot menu where it does "
                                    u"not start from it by itself."_s);
        QCOMPARE(pressed(), (QStringList{u"reset"_s, u"power-short"_s, u"reset"_s, u"reset"_s}));
    }

    // --- what is done to it ----------------------------------------------------------------------------------------

    void itListsItsImagesAndMountsOneReadOnlyAsACdOrADisk()
    {
        Rig rig;
        QVERIFY(rig.connected());
        const base::Result<MediaList> list = rig.list();
        QVERIFY2(list, qPrintable(saidOf(list)));
        QCOMPARE(idsOf(list.value()), (QStringList{alpine, debian}));
        QCOMPARE(list.value().images.at(1).size, std::optional<qint64>(700'000'000));
        QCOMPARE(list.value().free, std::optional<qint64>(6'238'000'000));
        QCOMPARE(list.value().size, std::optional<qint64>(7'000'000'000));
        QVERIFY(!list.value().mounted);
        QCOMPARE(list.value().offers.drives, (QStringList{u"cd"_s, u"disk"_s}));
        QVERIFY(list.value().offers.dropsInput);
        base::Result<MediaDone> done = rig.mount(debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(rig.images.mountedNow().value_or(QJsonObject{}), (QJsonObject{{u"source"_s, u"Storage"_s},
                                                                               {u"mode"_s, u"CDROM"_s},
                                                                               {u"filename"_s, debian},
                                                                               {u"size"_s, 700'000'000}}));
        QCOMPARE(rig.images.rebound(), 0);
        QVERIFY(done.value().mounted);
        QCOMPARE(got(done.value().mounted).drive, u"cd"_s);
        QCOMPARE(got(done.value().mounted).image, debian);
        QVERIFY(got(done.value().mounted).readOnly);
        QVERIFY(done.value().note.isEmpty());
        QCOMPARE(rig.mount(debian).value().note, u"It was mounted so already."_s);
        // One at a time.
        done = rig.mount(alpine);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), u"debian-13.iso is mounted on it now: unmount it first."_s);
        done = rig.unmount(debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(!rig.images.mountedNow());
        QCOMPARE(rig.unmount(debian).value().note, u"nothing was mounted"_s);
        // Never writable, nor a USB flash drive, nor started from, nor restarted: its drive has no such way.
        done = rig.mount(debian, u"disk"_s, true);
        QCOMPARE(codeOf(done), std::string("invalid"));
        QCOMPARE(saidOf(done), u"jet takes an image as a disk read-only, and only so"_s);
        QCOMPARE(saidOf(rig.mount(debian, u"flash"_s)), u"jet takes no image as a USB flash drive"_s);
        QCOMPARE(saidOf(rig.mount(debian, u"cd"_s, false, u"once"_s)),
                 u"jet cannot be told to start from an image that way"_s);
        QCOMPARE(saidOf(rig.mount(debian, u"cd"_s, false, u"none"_s, true)), u"jet cannot be restarted from here"_s);
        QCOMPARE(saidOf(rig.mount(u"../x.iso"_s)), u"That is not an image of the JetKVM's that qKVM lists"_s);
        QCOMPARE(saidOf(rig.mount(u"x.iso.incomplete"_s)), u"That is not an image of the JetKVM's that qKVM lists"_s);
        QCOMPARE(saidOf(rig.mount(u"gone.iso"_s)),
                 u"gone.iso is not on the JetKVM's own storage any more: look again."_s);
        // As a disk: its USB device bound anew (its keyboard and mouse drop for a moment, as the offer says).
        done = rig.mount(debian, u"disk"_s);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(rig.images.mountedNow().value_or(QJsonObject{}).value(u"mode"_s).toString(), u"Disk"_s);
        QCOMPARE(rig.images.rebound(), 1);
        QCOMPARE(got(done.value().mounted).drive, u"disk"_s);
        QCOMPARE(rig.list().value().mounted.value_or(core::MountedMedia{}).image, debian);
        QVERIFY(rig.unmount(debian));
        // Never an image from an address, nor its built-in ones, nor its mode switched.
        QVERIFY(rig.images.neverAsked().isEmpty());
        // Each request whole, as the stand-in noted it (its parameters written with their keys in order: nothing more
        // went with either; outside review 4.11).
        QCOMPARE(rig.calledLike(u"mountWithStorage {\"filename\":\"debian-13.iso\",\"mode\":\"CDROM\"}"_s), 1);
        QCOMPARE(rig.calledLike(u"mountWithStorage {\"filename\":\"debian-13.iso\",\"mode\":\"Disk\"}"_s), 1);
        QCOMPARE(rig.calledLike(u"mountWithStorage"_s), 2);
    }

    void anImageTooSmallForACdRomDriveIsSaidSoBeforeTheJetKvmIsAsked()
    {
        // (A trial on a real JetKVM before the web's 0.3.0: a 368 KB image as a CD-ROM drive was refused with the
        // kernel's own words, "... lun.0/file: errno 525".)
        Rig rig;
        rig.images.add(u"tiny.iso"_s, 376'832);
        QVERIFY(rig.connected());
        const base::Result<MediaDone> refused = rig.mount(u"tiny.iso"_s);
        QCOMPARE(codeOf(refused), std::string("invalid"));
        QCOMPARE(saidOf(refused), u"tiny.iso is 376,832 bytes, and a CD-ROM drive takes an image of 614,400 bytes at "
                                  u"least (300 sectors of 2,048 bytes): mount it as a disk."_s);
        QCOMPARE(rig.calledLike(u"mountWithStorage"_s), 0);
        QVERIFY(rig.mount(u"tiny.iso"_s, u"disk"_s));
        QVERIFY(rig.unmount(u"tiny.iso"_s));
    }

    void whatIsMountedFromAnAddressIsSaidWithoutTheAddress()
    {
        Rig rig;
        {
            const auto held = rig.images.lock();
            rig.images.mounted = QJsonObject{{u"source"_s, u"HTTP"_s},
                                             {u"mode"_s, u"CDROM"_s},
                                             {u"url"_s, u"https://user:secret@192.0.2.7/boot.iso"_s},
                                             {u"size"_s, 1}};
        }
        QVERIFY(rig.connected());
        const base::Result<MediaList> list = rig.list();
        QVERIFY(list);
        const QString id = got(list.value().mounted).image;
        QVERIFY2(id.startsWith(u"(an address)/"_s), qPrintable(id));
        const Status status = rig.device.status();
        QCOMPARE(status.media.value_or(core::MountedMedia{}).image, id);
        QVERIFY(!(got(status.media).name + got(status.media).image).contains(u"secret"_s));
        QCOMPARE(codeOf(rig.mount(debian)), std::string("changed")); // (one at a time)
        QVERIFY(rig.unmount(id));
        QVERIFY(!rig.images.mountedNow());
    }

    // The box listed an image mounted from one address, and somebody at its own page put one from another address in
    // its drive meanwhile: the box's unmount is of the one it listed, and takes nothing out, and says so; nor is an
    // image of its storage called "(an address)" taken for either. (Outside review 4.4: every image from an address
    // had one id, the same as that name's.)
    void anImageFromAnotherAddressIsNotTakenOutForTheOneListed()
    {
        Rig rig;
        rig.images.add(u"(an address)"_s, 700'000);
        const auto fromAddress = [&rig](const QString& url) {
            const auto held = rig.images.lock();
            rig.images.mounted =
                QJsonObject{{u"source"_s, u"HTTP"_s}, {u"mode"_s, u"CDROM"_s}, {u"url"_s, url}, {u"size"_s, 1}};
        };
        const auto inItsDrive = [&rig] {
            const base::Result<MediaList> list = rig.list();
            return list ? got(list.value().mounted).image : QString();
        };
        fromAddress(u"https://192.0.2.7/one.iso"_s);
        QVERIFY(rig.connected());
        const QString listed = inItsDrive();
        QVERIFY2(listed.startsWith(u"(an address)/"_s), qPrintable(listed));
        fromAddress(u"https://192.0.2.7/two.iso"_s);
        base::Result<MediaDone> done = rig.unmount(listed);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), u"An image from an address, mounted at the JetKVM's own page is mounted on it now, and "
                               u"not the image from an address it had when it was looked at: nothing was taken out. "
                               u"Look at it again."_s);
        done = rig.unmount(u"(an address)"_s);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(rig.calledLike(u"unmountImage"_s), 0);
        QCOMPARE(got(rig.images.mountedNow()).value(u"url"_s).toString(), u"https://192.0.2.7/two.iso"_s);
        // The one in it now, as looked at anew: taken out.
        const QString now = inItsDrive();
        QVERIFY(now.startsWith(u"(an address)/"_s) && now != listed);
        QVERIFY(rig.unmount(now));
        QVERIFY(!rig.images.mountedNow());
        // (The same address mounted again is that image again.)
        fromAddress(u"https://192.0.2.7/one.iso"_s);
        QCOMPARE(inItsDrive(), listed);
    }

    // An image of its own storage may be named "(an address)", the name an image mounted from an address at its own
    // page goes by here: neither is taken for the other (a mount of the one is no "mounted so already" of the other, a
    // deletion of the one no "is mounted" of the other, and the other has no SHA-256 of the one).
    void anImageNamedAsAnAddressIsNotTheOneFromAnAddress()
    {
        Rig rig;
        rig.images.add(u"(an address)"_s, 700'000);
        {
            const auto held = rig.images.lock();
            rig.images.mounted = QJsonObject{{u"source"_s, u"HTTP"_s},
                                             {u"mode"_s, u"CDROM"_s},
                                             {u"url"_s, u"https://192.0.2.7/boot.iso"_s},
                                             {u"size"_s, 1}};
        }
        QVERIFY(rig.connected());
        const base::Result<MediaDone> done = rig.mount(u"(an address)"_s);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), u"An image from an address, mounted at the JetKVM's own page is mounted on it now: "
                               u"unmount it first."_s);
        QCOMPARE(rig.calledLike(u"mountWithStorage"_s), 0);
        const base::Result<MediaDone> gone = rig.remove(u"(an address)"_s);
        QVERIFY2(gone, qPrintable(saidOf(gone)));
        QVERIFY(!rig.images.has(u"(an address)"_s));
        QCOMPARE(got(rig.images.mountedNow()).value(u"source"_s).toString(), u"HTTP"_s);
        // Read as the web reads it, but for the hash.
        core::kvmd::MediaNotes notes;
        notes.carried(u"key"_s, u"(an address)"_s, u"ab"_s, 700'000, std::nullopt);
        const QHash<QString, core::jetkvm::Files::Listed> listed{
            {u"(an address)"_s, core::jetkvm::Files::Listed{700'000, std::nullopt}}};
        const std::optional<core::MountedMedia> fromAnAddress =
            core::jetkvm::mountedOf(parsed(R"({"source":"HTTP","mode":"CDROM","size":1})"_ba), listed, notes, u"key"_s);
        QVERIFY(got(fromAnAddress).image.startsWith(u"(an address)/"_s));
        QVERIFY(got(fromAnAddress).sha256.isEmpty());
        const std::optional<core::MountedMedia> stored = core::jetkvm::mountedOf(
            // (Not a raw string: the name's closing bracket and quote would end it.)
            parsed("{\"source\":\"Storage\",\"mode\":\"CDROM\",\"filename\":\"(an address)\",\"size\":700000}"_ba),
            listed, notes, u"key"_s);
        QCOMPARE(got(stored).image, u"(an address)"_s);
        QCOMPARE(got(stored).sha256, u"ab"_s);
    }

    // A list of its storage too long for one message of its channel (64 KiB: a few hundred long names) is let go as
    // any larger message is, and what waited for it says that limit was met, not only that the JetKVM was silent (the
    // web ends the whole connection there).
    void aListTooLongForOneMessageSaysSo()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.callWithinMs = 3000;
        Rig rig(pace);
        // (As a sender that does not hold to the largest the offer says it takes: the stand-in's own library holds to
        // it.)
        rig.viewer.sendsAnySize = true;
        for (int at = 0; at < 400; ++at) {
            rig.images.add(QStringLiteral("%1-").arg(at, 4, 10, QLatin1Char('0')) + QString(190, u'x') + u".iso"_s, 1);
        }
        QVERIFY(rig.connected());
        const base::Result<MediaList> list = rig.list();
        QCOMPARE(codeOf(list), std::string("timeout"));
        QCOMPARE(saidOf(list), u"The JetKVM did not answer in time: what it sent meanwhile was larger than qKVM takes "
                               u"in one message (64 KiB)"_s);
        // (One that is merely silent is said as ever.)
        rig.viewer.silentOn = "listStorageFiles";
        QCOMPARE(saidOf(rig.list()), u"The JetKVM did not answer in time"_s);
    }

    // A list of its storage that is no list, or not read whole, never says an image is not there: a deletion of one
    // it does not find there is refused, and nothing is deleted; nor is one said gone that such a list no longer has.
    // (Outside review 4.6: either was taken for "not there", and the deletion said done.)
    void aListNotReadWholeNeverSaysAnImageIsGone()
    {
        Rig rig;
        QVERIFY(rig.connected());
        {
            const auto held = rig.images.lock();
            rig.images.filesSayInstead = u"none"_s;
        }
        base::Result<MediaDone> done = rig.remove(alpine);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QCOMPARE(saidOf(done), u"The JetKVM's list of its own storage could not be read whole (it is in no shape qKVM "
                               u"reads, or longer than the 2,000 entries qKVM reads), so whether alpine-3.21.iso is "
                               u"there is not known: look at it on its own page."_s);
        QCOMPARE(rig.calledLike(u"deleteStorageFile"_s), 0);
        QVERIFY(rig.images.has(alpine));
        // Its box says so, and never shows it as all there is (review cx4-1 L5: an empty storage).
        const base::Result<MediaList> list = rig.list();
        QVERIFY2(list, qPrintable(saidOf(list)));
        QVERIFY(list.value().images.isEmpty());
        QCOMPARE(list.value().notes,
                 QStringList{u"The JetKVM's list of its own storage could not be read whole (it is in no shape qKVM "
                             u"reads, or longer than the 2,000 entries qKVM reads), so not every image on it may be "
                             u"shown here: look at it on its own page"_s});
        // (And the box is told so beside the note: review cx4-2 L1.)
        QVERIFY(!list.value().readWhole);
        // Listed whole before the deletion, and not after it: not said gone.
        {
            const auto held = rig.images.lock();
            rig.images.filesSayInstead = QJsonArray{QJsonObject{{u"size"_s, 1}}};
            rig.images.filesSayInsteadAfter = rig.images.listsSaid + 1;
        }
        done = rig.remove(alpine);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QVERIFY2(saidOf(done).endsWith(u"so whether alpine-3.21.iso is gone is not known: look at it on its own "
                                       u"page."_s),
                 qPrintable(saidOf(done)));
        QCOMPARE(rig.calledLike(u"deleteStorageFile"_s), 1);
    }

    // What is in its drive said in no shape qKVM reads is never an empty drive: a deletion, an unmount and a mount are
    // each refused, and nothing is asked of its drive or storage. (Outside review 4.7: a deletion went for the image in
    // its drive.)
    void aDriveItSaysInNoShapeIsNoEmptyOne()
    {
        Rig rig;
        QVERIFY(rig.connected());
        {
            const auto held = rig.images.lock();
            rig.images.mounted = QJsonObject{{u"source"_s, u"Storage"_s},
                                             {u"mode"_s, u"CDROM"_s},
                                             {u"filename"_s, debian},
                                             {u"size"_s, 700'000'000}};
            rig.images.stateSaysInstead = QJsonValue(7);
        }
        const QString refused =
            u"The JetKVM does not say what is in its drive in a shape qKVM reads: look at it on its own page."_s;
        base::Result<MediaDone> done = rig.remove(debian);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QCOMPARE(saidOf(done), refused);
        QCOMPARE(saidOf(rig.unmount(debian)), refused);
        QCOMPARE(saidOf(rig.mount(alpine)), refused);
        QCOMPARE(rig.calledLike(u"deleteStorageFile"_s), 0);
        QCOMPARE(rig.calledLike(u"unmountImage"_s), 0);
        QCOMPARE(rig.calledLike(u"mountWithStorage"_s), 0);
        QVERIFY(rig.images.has(debian));
        const base::Result<MediaList> list = rig.list();
        QVERIFY(list);
        QCOMPARE(got(list.value().mounted).image, QString(core::kvmd::unreadDrive));
    }

    // What is in its drive from where it does not say (no `source`, or one it does not know) cannot be read; an image
    // of its storage whose name it does not say as one qKVM hands back may be any image of it. Either stands against a
    // deletion of every image, and is no "It was mounted so already" of one; the second is taken out by the id the box
    // listed. One sent from a browser is none of its storage. (Review cx4-1 M2: each was taken for an image from an
    // address, or for the image its name reads as, and a deletion of the image in the drive went ahead.)
    void aDriveWhoseSourceOrNameCannotBeReadStandsAgainstADeletion()
    {
        Rig rig;
        rig.images.add(u"an image"_s, 700'000);
        QVERIFY(rig.connected());
        const auto inItsDrive = [&rig](const QJsonObject& state) {
            const auto held = rig.images.lock();
            rig.images.mounted = state;
        };
        const QString refused =
            u"The JetKVM does not say what is in its drive in a shape qKVM reads: look at it on its own page."_s;
        for (const QJsonValue& source :
             {QJsonValue(QJsonValue::Undefined), QJsonValue(), QJsonValue(7), QJsonValue(u"storage"_s)}) {
            QJsonObject state{{u"mode"_s, u"CDROM"_s}, {u"filename"_s, debian}, {u"size"_s, 700'000'000}};
            if (!source.isUndefined()) {
                state.insert(u"source"_s, source);
            }
            inItsDrive(state);
            const base::Result<MediaDone> done = rig.remove(debian);
            QCOMPARE(codeOf(done), std::string("bad_response"));
            QCOMPARE(saidOf(done), refused);
            QCOMPARE(saidOf(rig.unmount(debian)), refused);
            QCOMPARE(saidOf(rig.mount(alpine)), refused);
        }
        QCOMPARE(rig.calledLike(u"deleteStorageFile"_s), 0);
        QCOMPARE(rig.calledLike(u"unmountImage"_s), 0);
        QVERIFY(rig.images.has(debian));
        // Of its storage, by no name it says.
        inItsDrive(QJsonObject{{u"source"_s, u"Storage"_s}, {u"mode"_s, u"CDROM"_s}, {u"size"_s, 700'000}});
        base::Result<MediaDone> done = rig.remove(debian);
        QCOMPARE(codeOf(done), std::string("busy"));
        QCOMPARE(saidOf(done), u"An image is in its drive, by a name qKVM does not read as one of its images, so it "
                               u"may be debian-13.iso: unmount it first."_s);
        done = rig.mount(u"an image"_s);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), u"An image is mounted on it now: unmount it first."_s);
        QCOMPARE(rig.calledLike(u"deleteStorageFile"_s), 0);
        QCOMPARE(rig.calledLike(u"mountWithStorage"_s), 0);
        const base::Result<MediaList> list = rig.list();
        QVERIFY(list);
        const QString listed = got(list.value().mounted).image;
        QVERIFY2(core::kvmd::isNameless(listed), qPrintable(listed));
        QCOMPARE(got(list.value().mounted).name, u"an image"_s);
        done = rig.unmount(listed);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(!rig.images.mountedNow());
        // An image of its storage that is called so is said by its own name, as it is (review cx4-2 L6).
        done = rig.mount(u"an image"_s);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(saidOf(rig.mount(alpine)), u"an image is mounted on it now: unmount it first."_s);
        QVERIFY(rig.unmount(u"an image"_s));
        // Sent from a browser: no image of its storage, which is deleted.
        inItsDrive(QJsonObject{{u"source"_s, u"WebRTC"_s}, {u"mode"_s, u"CDROM"_s}, {u"filename"_s, alpine}});
        QCOMPARE(
            saidOf(rig.mount(alpine)),
            u"An image from a browser, mounted at the JetKVM's own page is mounted on it now: unmount it first."_s);
        done = rig.remove(alpine);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(!rig.images.has(alpine));
    }

    void anImageIsDeletedAndNotWhileItIsMounted()
    {
        Rig rig;
        rig.images.add(u"cut.iso.incomplete"_s, 5);
        QVERIFY(rig.connected());
        const base::Result<MediaList> list = rig.list();
        QVERIFY(list);
        const auto cut = std::ranges::find_if(
            list.value().images, [](const core::MediaImage& one) { return one.id == "cut.iso.incomplete"_L1; });
        QVERIFY(cut != list.value().images.end());
        QCOMPARE(cut->name, u"cut.iso"_s);
        QCOMPARE(cut->size, std::optional<qint64>(5));
        QVERIFY(cut->partial);
        QVERIFY(cut->sha256.isEmpty());
        QCOMPARE(codeOf(rig.mount(u"cut.iso.incomplete"_s)), std::string("invalid")); // (a part of one: only to delete)
        QVERIFY(rig.mount(debian));
        const base::Result<MediaDone> busy = rig.remove(debian);
        QCOMPARE(codeOf(busy), std::string("busy"));
        QCOMPARE(saidOf(busy), u"debian-13.iso is mounted: unmount it first."_s);
        for (const QString& image : {u"cut.iso.incomplete"_s, alpine}) {
            const base::Result<MediaDone> done = rig.remove(image);
            QVERIFY2(done, qPrintable(saidOf(done)));
        }
        QVERIFY(!rig.images.has(alpine) && !rig.images.has(u"cut.iso.incomplete"_s) && rig.images.has(debian));
        QCOMPARE(rig.remove(alpine).value().note, u"it was not on the JetKVM any more"_s);
        QCOMPARE(saidOf(rig.remove(u"../x.iso"_s)), u"That is not an image of the JetKVM's that qKVM lists"_s);
        QCOMPARE(rig.calledLike(u"deleteStorageFile"_s), 2);
    }

    // What it answers otherwise than as it was asked is said, in the web's words.
    void whatItDoesOtherwiseThanAskedIsSaid()
    {
        Rig rig;
        QVERIFY(rig.connected());
        rig.images.mountsNothing = true;
        base::Result<MediaDone> done = rig.mount(debian);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), u"The JetKVM does not say it has the image in its drive as it was asked: look at it on "
                               u"its own page."_s);
        rig.images.mountsNothing = false;
        QVERIFY(rig.mount(alpine));
        done = rig.unmount(debian);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), u"alpine-3.21.iso is mounted on it now, and not debian-13.iso: nothing was taken out. "
                               u"Look at it again."_s);
        rig.images.unmountsNothing = true;
        done = rig.unmount(alpine);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), u"The JetKVM says the image is still in its drive: look at it on its own page."_s);
        rig.images.unmountsNothing = false;
        QVERIFY(rig.unmount(alpine));
        rig.images.deletesNothing = true;
        done = rig.remove(alpine);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), u"The JetKVM says the image is still on its storage: look at it on its own page."_s);
        rig.images.deletesNothing = false;
        // Its own refusal, in its own words.
        {
            const auto held = rig.images.lock();
            rig.images.files.erase(alpine);
        }
        done = rig.remove(alpine);
        QCOMPARE(done.value().note, u"it was not on the JetKVM any more"_s);
    }

    void aMountItDoesNotAnswerMayHaveBeenDone()
    {
        {
            JetKvmDevice::Pace pace = quick();
            pace.mountWithinMs = 800;
            Rig rig(pace);
            QVERIFY(rig.connected());
            rig.viewer.silentOn = "mountWithStorage";
            const base::Result<MediaDone> done = rig.mount(debian);
            QCOMPARE(codeOf(done), std::string("timeout"));
            QVERIFY(JetKvmDevice::mayHaveBeenDone(done.error()));
        }
        // Given its own time, longer than a call's: answered a second and a half after a call's time is over, and well
        // within its own.
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.callWithinMs = 3000;
        pace.mountWithinMs = 12'000;
        Rig rig(pace);
        QVERIFY(rig.connected());
        rig.viewer.slowOn("mountWithStorage", 4500);
        const base::Result<MediaDone> done = rig.mount(debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
    }

    // Its own page took it (another viewer has the JetKVM): nothing is asked of its media.
    void anotherViewerHasItAndNothingIsAsked()
    {
        Rig rig;
        QVERIFY(rig.connected());
        rig.viewer.handOver();
        QTRY_VERIFY_WITH_TIMEOUT(rig.device.viewer().aside(), 10'000);
        // Its card says so, and offers no media meanwhile.
        const Status busy = lookedAt(rig.device);
        QCOMPARE(busy.errorCode, u"in_use"_s);
        QVERIFY(!busy.offersMedia);
        QVERIFY(!busy.capabilities.value_or(QStringList()).contains(u"media"_s));
        const qsizetype before = rig.images.called().size();
        QCOMPARE(codeOf(rig.list()), std::string("in_use"));
        QCOMPARE(codeOf(rig.mount(debian)), std::string("in_use"));
        QCOMPARE(codeOf(rig.remove(alpine)), std::string("in_use"));
        QCOMPARE(rig.images.called().size(), before);
        QVERIFY(!rig.images.mountedNow());
    }

    // J5: another JetKVM answers at its address (as after a new lease): its media are asked nothing.
    void anotherJetKvmAtItsAddressIsAskedNothing()
    {
        Rig rig;
        QVERIFY(rig.connected());
        rig.viewer.dropCurrent();
        QTRY_VERIFY_WITH_TIMEOUT(!rig.device.viewer().isOpen(), 10'000);
        rig.jet.deviceId = u"0a0b0c0d0e0f1011"_s;
        const qsizetype before = rig.images.called().size();
        const base::Result<MediaList> list = rig.list();
        QCOMPARE(codeOf(list), std::string("another_machine"));
        QCOMPARE(codeOf(rig.mount(debian)), std::string("another_machine"));
        QCOMPARE(rig.images.called().size(), before);
    }

    // --- through this PC's own wall --------------------------------------------------------------------------------

    void aJetKvmsMediaAreHadThroughTheWall()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        core::DriverPaces paces;
        paces.jetkvm = quick();
        paces.jetkvm.viewer.unwatchedMs = 600'000;
        core::LocalWall wall(settings, secrets, paces);
        JetKvmStorageByHand images;
        images.add(debian, 700'000'000);
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        viewer.storage = &images;
        viewer.extension = "atx-power";
        core::OwnKvm own;
        own.address = jet.address().toString();
        own.name = u"Rack JetKVM"_s;
        own.kind = u"jetkvm"_s;
        const base::Result<QString> added = wall.add(own, u"secret"_s);
        QVERIFY2(added, added ? "" : added.error().message.c_str());
        const QString& id = added.value();
        const auto status = [&] { return statusOf(wall, id); };
        // Its card asks, and connects; a look over its connection finds its media.
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                wall.askSnapshot(id, 320, 0, [](const base::Result<core::Snapshot>&) {});
                return status().offersMedia;
            }(),
            30'000);
        QVERIFY(status().capabilities.value_or(QStringList()).contains(u"media"_s));
        core::MediaPlace& place = wall;
        const base::Result<MediaList> list =
            answerTo<MediaList>([&](auto done) { place.askMedia(id, std::move(done)); });
        QVERIFY2(list, qPrintable(saidOf(list)));
        QCOMPARE(idsOf(list.value()), QStringList{debian});
        // Bound to the JetKVM it listed, by its number too.
        QVERIFY(list.value().boundTo.endsWith(wall.kvms().constFirst().number));
        QVERIFY(!wall.kvms().constFirst().number.isEmpty());
        QVERIFY(!place.movedSince(id, list.value().boundTo));
        const MediaMount mount{debian, u"Rack JetKVM"_s, u"disk"_s, false, u"none"_s, false, QString()};
        base::Result<MediaDone> done =
            answerTo<MediaDone>([&](auto then) { place.mountMedia(id, mount, std::move(then)); });
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(images.mountedNow().value_or(QJsonObject{}).value(u"mode"_s).toString(), u"Disk"_s);
        QTRY_VERIFY_WITH_TIMEOUT(status().media.value_or(core::MountedMedia{}).image == debian, 10'000);
        // A restart from it waits while a window has its keyboard and mouse; an unmount, which drops them a moment, is
        // the asker's own to say yes to (there is nobody else here: the box says it).
        std::unique_ptr<core::Live> live;
        wall.openLive(id, core::Live::Need{}, [&live](base::Result<std::unique_ptr<core::Live>> opened) {
            if (opened) {
                live = std::move(opened.value());
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(live != nullptr, 10'000);
        bool control = false;
        QObject::connect(live.get(), &core::Live::controlChanged, live.get(),
                         [&control](bool on, const QString& /*code*/, const QString& /*message*/) { control = on; });
        live->askControl(true);
        QTRY_VERIFY_WITH_TIMEOUT(control, 30'000);
        done = answerTo<MediaDone>(
            [&](auto then) { place.unmountMedia(id, debian, u"Rack JetKVM"_s, QString(), std::move(then)); });
        QVERIFY2(done, qPrintable(saidOf(done)));
        const MediaMount again{debian, u"Rack JetKVM"_s, u"cd"_s, false, u"none"_s, true, QString()};
        done = answerTo<MediaDone>([&](auto then) { place.mountMedia(id, again, std::move(then)); });
        QCOMPARE(codeOf(done), std::string("busy"));
        QCOMPARE(saidOf(done), u"A window of this PC has Rack JetKVM's keyboard and mouse over a live line at this "
                               u"moment. Wait until they are given back."_s);
        QVERIFY(!images.mountedNow());
        live->askControl(false);
        QTRY_VERIFY_WITH_TIMEOUT(!control, 10'000);
        live.reset();
        done = answerTo<MediaDone>(
            [&](auto then) { place.deleteMedia(id, debian, u"Rack JetKVM"_s, QString(), std::move(then)); });
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(!images.has(debian));
        QVERIFY(images.neverAsked().isEmpty());
    }

    void anOlderJetKvmHasNoMediaThroughTheWall()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        core::DriverPaces paces;
        paces.jetkvm = quick();
        core::LocalWall wall(settings, secrets, paces);
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        core::OwnKvm own;
        own.address = jet.address().toString();
        own.name = u"Old JetKVM"_s;
        own.kind = u"jetkvm"_s;
        const QString id = wall.add(own, u"secret"_s).value();
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                wall.askSnapshot(id, 320, 0, [](const base::Result<core::Snapshot>&) {});
                return answerTo<core::Snapshot>([&](auto done) { wall.askSnapshot(id, 320, 0, std::move(done)); }).ok();
            }(),
            30'000);
        // (A look over its connection, which alone asks its drive: one says its keyboard, a plain look does not.)
        Status status;
        const auto overItsConnection = [&] {
            status = statusOf(wall, id);
            return status.keyboard.has_value();
        };
        QTRY_VERIFY_WITH_TIMEOUT(overItsConnection(), 30'000);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.offersMedia);
        QVERIFY(!status.capabilities.value_or(QStringList()).contains(u"media"_s));
        core::MediaPlace& place = wall;
        const base::Result<MediaList> list =
            answerTo<MediaList>([&](auto done) { place.askMedia(id, std::move(done)); });
        QCOMPARE(codeOf(list), std::string("no_virtual_media"));
        QCOMPARE(saidOf(list), u"Old JetKVM has no virtual media"_s);
    }
};

QTEST_GUILESS_MAIN(TestJetKvmMedia)
#include "test_jetkvm_media.moc"
