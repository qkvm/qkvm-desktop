#include <functional>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "../../core/tests/far_away_file.h"
#include "../../core/tests/stand_in.h"
#include "media_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::FarAwayFile;
using core::testing::StandIn;
using core::testing::ThreadWatch;

namespace {

// A QKVM with one machine's media, as its routes answer (app/qkvm/server.py, media_*): what is listed and mounted,
// and what a mount, an unmount and a deletion are answered with, each remembered.
struct Media {
    QJsonArray images{
        QJsonObject{{u"id"_s, u"debian-13.iso"_s},
                    {u"name"_s, u"debian-13.iso"_s},
                    {u"where"_s, QJsonValue()},
                    {u"size"_s, 700'000'000},
                    {u"partial"_s, false},
                    {u"sha256"_s, QJsonValue()}},
        QJsonObject{{u"id"_s, u"win11.iso"_s},
                    {u"name"_s, u"win11.iso"_s},
                    {u"size"_s, 6'000'000'000},
                    {u"partial"_s, false},
                    {u"sha256"_s, u"0123456789abcdef0123456789abcdef"_s}},
        QJsonObject{{u"id"_s, u"half.img"_s}, {u"name"_s, u"half.img"_s}, {u"size"_s, 1000}, {u"partial"_s, true}}};
    QJsonObject offers{{u"drives"_s, QJsonArray{u"cd"_s, u"flash"_s}},
                       {u"boot"_s, QJsonArray{}},
                       {u"restart"_s, true},
                       {u"upload"_s, true},
                       {u"where"_s, u"device"_s},
                       {u"writable"_s, QJsonArray{u"flash"_s}},
                       {u"delete"_s, true},
                       {u"drops_input"_s, false}};
    QJsonValue mounted;
    QJsonArray notes;
    qint64 free = 9'000'000'000;
    // What the server says of the upload under way, while one is.
    QJsonValue uploading;
    // What a mount, an unmount and a deletion are answered with; without one, that it was done.
    std::function<Answered(const Asked&)> onChange;
    bool listFails = false;
    int listAsked = 0;
    QList<Asked> changes;

    QByteArray list() const
    {
        return QJsonDocument(QJsonObject{{u"images"_s, images},
                                         {u"unseen"_s, QJsonArray{}},
                                         {u"left_out"_s, 0},
                                         {u"notes"_s, notes},
                                         {u"space"_s, QJsonObject{{u"free"_s, free}, {u"size"_s, 32'000'000'000}}},
                                         {u"mounted"_s, mounted},
                                         {u"offers"_s, offers},
                                         {u"uploading"_s, QJsonValue()}})
            .toJson(QJsonDocument::Compact);
    }

    Answered answer(const Asked& asked)
    {
        if (asked.path == "/healthz") {
            return {200, R"({"status": "ok", "version": "0.3.0", "api": ["v1"]})"};
        }
        if (asked.method == "GET" && asked.path == "/api/v1/devices/kvm/media/upload") {
            return {200, QJsonDocument(QJsonObject{{u"uploading"_s, uploading}}).toJson(QJsonDocument::Compact)};
        }
        if (asked.method == "GET" && asked.path == "/api/v1/devices/kvm/media") {
            ++listAsked;
            return listFails ? Answered{504, R"({"code": "timeout", "message": "The KVM did not answer."})"}
                             : Answered{200, list()};
        }
        if (asked.method == "POST" && asked.path.startsWith("/api/v1/devices/kvm/media")) {
            changes.append(asked);
            return onChange ? onChange(asked) : Answered{200, R"({"ok": true, "note": null, "mounted": null})"};
        }
        return {404, R"({"code": "not_found", "message": "Unknown"})"};
    }
};

struct Box {
    explicit Box(Media& media) : server([&media](const Asked& asked) { return media.answer(asked); })
    {
        static_cast<void>(client.setAddress(server.address()));
        client.setSession(QByteArray(43, 's'));
        QObject::connect(&view, &ui::MediaView::told, &view, [this](const QString& said, bool good) {
            told.append((good ? u"ok: "_s : u"no: "_s) + said);
        });
        QObject::connect(&view, &ui::MediaView::done, &view, [this](const QString& key) { done.append(key); });
        view.setImageDoor([this] { return base::Result<QString>(chosen); });
    }

    // A file of this name and size, of a pattern, that the box is handed when it asks for an image to upload.
    QString file(const QString& name, qint64 size)
    {
        const QString path = folder.filePath(name);
        QFile written(path);
        if (written.open(QIODevice::WriteOnly)) {
            QByteArray piece(static_cast<qsizetype>(size), 0);
            for (qsizetype at = 0; at < piece.size(); ++at) {
                piece[at] = static_cast<char>((at * 13 + 5) & 0xFF);
            }
            written.write(piece);
        }
        chosen = path;
        return path;
    }

    void open(const QString& name = u"Build server"_s)
    {
        view.open(&client, u"kvm"_s, name, u"wall:kvm"_s, &underWay, u"one:kvm"_s);
    }
    bool listed()
    {
        return QTest::qWaitFor([&] { return !view.images().isEmpty() || !view.said().isEmpty(); }, 10000);
    }
    QStringList hints() const
    {
        QStringList all;
        for (const QVariant& image : view.images()) {
            all.append(image.toMap().value(u"name"_s).toString() + u": "_s + image.toMap().value(u"hint"_s).toString());
        }
        return all;
    }

    QTemporaryDir folder{QCoreApplication::applicationDirPath() + u"/media-view-XXXXXX"_s};
    QString chosen;
    StandIn server;
    core::ServerClient client;
    ui::MediaView::UnderWay underWay;
    ui::MediaView view;
    QStringList told;
    QStringList done;
};

// Where the box reads its image from a share far away: this file, made when it is opened (on the thread that reads it).
std::function<core::ReadAhead::Source(const QString&)> farAway(const std::function<std::unique_ptr<QIODevice>()>& made)
{
    return [made](const QString&) {
        return core::ReadAhead::Source([made] { return base::Result<std::unique_ptr<QIODevice>>(made()); });
    };
}

QByteArray patterned(qint64 size)
{
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    for (qsizetype at = 0; at < bytes.size(); ++at) {
        bytes[at] = core::testing::patternAt(at);
    }
    return bytes;
}

QJsonObject bodyOf(const Asked& asked)
{
    return QJsonDocument::fromJson(asked.body).object();
}

} // namespace

// The box for a machine's virtual media through its QKVM, as the page's Media box has it (app.js, mediaDialog): what
// it shows, what it asks first, what it sends, and what it says of what came back.
class TestMediaView : public QObject {
    Q_OBJECT

private slots:
    void itShowsTheImagesWhereTheyAreAndWhatIsInTheDrive()
    {
        Media media;
        media.mounted = QJsonObject{{u"drive"_s, u"flash"_s}, {u"image"_s, u"win11.iso"_s}, {u"name"_s, u"win11.iso"_s},
                                    {u"read_only"_s, false},  {u"first"_s, QJsonValue()},   {u"by"_s, u"anna"_s}};
        Box box(media);
        box.open();
        QVERIFY(box.view.opened());
        QCOMPARE(box.view.title(), u"Virtual media of Build server"_s);
        QCOMPARE(box.view.now(), u"Looking..."_s);
        QVERIFY(box.listed());
        QCOMPARE(box.view.where(), u"On the KVM's own storage · 9.0 GB free of 32 GB"_s);
        QCOMPARE(box.view.now(), u"win11.iso · USB flash drive · writable · mounted by anna"_s);
        QCOMPARE(box.hints(),
                 (QStringList{u"debian-13.iso: 700 MB"_s,
                              u"win11.iso: 6.0 GB · mounted now · uploaded from qKVM, SHA-256 0123456789abcdef..."_s,
                              u"half.img: an upload of it was cut off: only to delete · 1.0 kB"_s}));
        QVERIFY(box.view.foot().startsWith(u"An image goes in as a CD-ROM drive (read-only) or a USB flash drive"_s));
        QVERIFY(box.view.canUnmount());
        // Nothing is picked: nothing to mount or delete.
        QVERIFY(!box.view.canMount());
        QVERIFY(!box.view.canDelete());
        // What is mounted is not deleted; a part of an upload is not mounted, only deleted.
        box.view.pick(u"win11.iso"_s);
        QVERIFY(box.view.canMount());
        QVERIFY(!box.view.canDelete());
        box.view.pick(u"half.img"_s);
        QVERIFY(!box.view.canMount());
        QVERIFY(box.view.canDelete());
        // Only what its drive takes is offered: no disk, and a USB flash drive writable where it says so.
        QStringList offered;
        for (const QVariant& drive : box.view.drives()) {
            if (drive.toMap().value(u"enabled"_s).toBool()) {
                offered.append(drive.toMap().value(u"value"_s).toString());
            }
        }
        QCOMPARE(offered, (QStringList{u"cd"_s, u"flash"_s, u"flash_rw"_s}));
    }

    // A drive this program does not know (one added since) is said in the server's word, as it came.
    void aDriveThisProgramDoesNotKnowIsSaidAsItCame()
    {
        Media media;
        media.mounted = QJsonObject{{u"drive"_s, u"floppy"_s},
                                    {u"image"_s, u"win11.iso"_s},
                                    {u"name"_s, u"win11.iso"_s},
                                    {u"read_only"_s, true},
                                    {u"by"_s, u"anna"_s}};
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        QCOMPARE(box.view.now(), u"win11.iso · floppy · read-only · mounted by anna"_s);
    }

    void aMountIsAskedFirstNamingTheMachineAndTheImageAndSendsTheNameAsAsked()
    {
        Media media;
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.chooseDrive(u"flash_rw"_s);
        box.view.mount();
        // Nothing went yet: the question names the machine and the image, and says what writable means.
        QVERIFY(media.changes.isEmpty());
        const QVariantMap asked = box.view.question();
        QCOMPARE(asked.value(u"title"_s).toString(), u"Mount debian-13.iso"_s);
        QCOMPARE(asked.value(u"text"_s).toString(),
                 u"Build server gets debian-13.iso as a USB flash drive, writable: the machine can change and delete "
                 "what is on it, and nothing else is done: pick the KVM's drive in its boot menu at its next start."_s);
        QCOMPARE(asked.value(u"yes"_s).toString(), u"Mount"_s);
        QVERIFY(asked.value(u"danger"_s).toBool());
        // The machine is called otherwise on the wall meanwhile: what goes is what the question named.
        box.view.rename(u"Web server"_s);
        box.view.answer(true);
        QVERIFY(box.view.busy());
        QVERIFY(box.underWay.contains(u"one:kvm"_s));
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy(); }, 10000));
        QCOMPARE(media.changes.size(), 1);
        QCOMPARE(bodyOf(media.changes.first()), QJsonObject({{u"image"_s, u"debian-13.iso"_s},
                                                             {u"machine"_s, u"Build server"_s},
                                                             {u"drive"_s, u"flash"_s},
                                                             {u"writable"_s, true},
                                                             {u"boot"_s, u"none"_s},
                                                             {u"restart"_s, false}}));
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        QCOMPARE(box.told, QStringList{u"ok: Web server: Mount debian-13.iso done"_s});
        QCOMPARE(box.done, QStringList{u"wall:kvm"_s});
        // And the box looks again.
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == 2; }, 10000));
    }

    void aNoIsANoAndNothingIsSent()
    {
        Media media;
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        QVERIFY(!box.view.question().isEmpty());
        box.view.answer(false);
        QVERIFY(box.view.question().isEmpty());
        QTest::qWait(200);
        QVERIFY(media.changes.isEmpty());
        QVERIFY(!box.view.busy());
    }

    void anotherMachineByThenIsSaidInTheServersWordsAndNothingMoreIsSent()
    {
        Media media;
        media.onChange = [](const Asked&) {
            return Answered{
                409,
                R"({"code": "changed", "message": "The confirmation named Build server, and this machine is called Web now. Nothing was mounted: look at it again."})"};
        };
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.chooseRestart(true);
        box.view.mount();
        QCOMPARE(box.view.question().value(u"yes"_s).toString(), u"Mount and restart"_s);
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.listAsked == 2; }, 10000));
        QVERIFY(QTest::qWaitFor([&] { return box.view.said().startsWith(u"The confirmation named"_s); }, 10000));
        QVERIFY(box.view.saidBad());
        QCOMPARE(box.view.said(),
                 u"The confirmation named Build server, and this machine is called Web now. Nothing was mounted: look "
                 "at it again."_s);
        QTest::qWait(300);
        QCOMPARE(media.changes.size(), 1);
        QVERIFY(box.told.isEmpty());
    }

    void noAnswerIsSaidAsSomethingThatMayHaveBeenDone()
    {
        Media media;
        media.onChange = [](const Asked&) { return Answered{502, "<html>Bad Gateway</html>"}; };
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return box.view.said().startsWith(u"No answer"_s); }, 15000));
        QCOMPARE(box.view.said(), u"No answer to \"Mount debian-13.iso\": it may have been done. Look at Build server "
                                  "before trying again."_s);
        QVERIFY(box.view.saidBad());
    }

    void anUnmountAndADeletionAreAskedFirstAndNameWhatTheyAreOf()
    {
        Media media;
        media.mounted = QJsonObject{{u"drive"_s, u"cd"_s},
                                    {u"image"_s, u"win11.iso"_s},
                                    {u"name"_s, u"win11.iso"_s},
                                    {u"read_only"_s, true},
                                    {u"first"_s, true}};
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.unmount();
        QCOMPARE(box.view.question().value(u"text"_s).toString(),
                 u"win11.iso is taken out of the drive of Build server, and its boot order put back where it was set "
                 "from here. A system running from it loses it."_s);
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.changes.size() == 1; }, 10000));
        QCOMPARE(media.changes.first().path, QByteArray("/api/v1/devices/kvm/media/unmount"));
        QCOMPARE(bodyOf(media.changes.first()),
                 QJsonObject({{u"image"_s, u"win11.iso"_s}, {u"machine"_s, u"Build server"_s}}));
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == 2; }, 10000));
        box.view.pick(u"debian-13.iso"_s);
        box.view.remove();
        QCOMPARE(box.view.question().value(u"text"_s).toString(),
                 u"debian-13.iso is deleted from the KVM's own storage of Build server. It cannot be brought back "
                 "from here."_s);
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.changes.size() == 2; }, 10000));
        QCOMPARE(media.changes.last().path, QByteArray("/api/v1/devices/kvm/media/delete"));
        QCOMPARE(bodyOf(media.changes.last()),
                 QJsonObject({{u"image"_s, u"debian-13.iso"_s}, {u"machine"_s, u"Build server"_s}}));
    }

    void oneThingAtATimeOnAMachineWhoeverSentIt()
    {
        Media media;
        media.onChange = [](const Asked&) {
            Answered later{200, R"({"ok": true, "note": null, "mounted": null})"};
            later.afterMs = 1500;
            return later;
        };
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        // Something else is under way on the machine, sent from another window.
        box.underWay.insert(u"one:kvm"_s, u"Press power button"_s);
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        QCOMPARE(box.view.said(), u"\"Press power button\" is still under way on Build server. Wait for its answer."_s);
        QVERIFY(box.view.saidBad());
        QVERIFY(!box.view.busy());
        QTest::qWait(200);
        QVERIFY(media.changes.isEmpty());
        box.underWay.clear();
        // While a mount of its own is out, the box holds still: nothing else is begun from it.
        box.view.mount();
        box.view.answer(true);
        QVERIFY(box.view.busy());
        box.view.pick(u"win11.iso"_s);
        box.view.remove();
        QVERIFY(box.view.question().isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy(); }, 10000));
        QCOMPARE(media.changes.size(), 1);
    }

    void whatTheBoxOffersFollowsHowTheMachineCanBeStarted()
    {
        Media media;
        // A server: started from it once (restarted now), or until it is unmounted (restarted where that is chosen).
        media.offers = QJsonObject{{u"drives"_s, QJsonArray{u"cd"_s}},
                                   {u"boot"_s, QJsonArray{u"once"_s, u"until_unmounted"_s}},
                                   {u"restart"_s, true},
                                   {u"upload"_s, false},
                                   {u"where"_s, u"qkvm"_s},
                                   {u"writable"_s, QJsonArray{}},
                                   {u"delete"_s, false}};
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        QVERIFY(!box.view.canUpload());
        QVERIFY(!box.view.restartEnabled());
        box.view.chooseBoot(u"once"_s);
        QVERIFY(box.view.restart());
        QVERIFY(!box.view.restartEnabled());
        box.view.chooseBoot(u"until_unmounted"_s);
        QVERIFY(box.view.restartEnabled());
        box.view.chooseRestart(false);
        QVERIFY(!box.view.restart());
        box.view.chooseBoot(u"none"_s);
        QVERIFY(!box.view.restart());
        // What it does not offer is not taken.
        box.view.chooseDrive(u"flash_rw"_s);
        QCOMPARE(box.view.drive(), u"cd"_s);
        box.view.pick(u"debian-13.iso"_s);
        box.view.chooseBoot(u"once"_s);
        box.view.mount();
        QCOMPARE(box.view.question().value(u"text"_s).toString(),
                 u"Build server gets debian-13.iso in its CD-ROM drive, read-only, and is restarted now to start from "
                 "it once: stopped at once (unsaved work on it is lost) and started, with its boot order as before "
                 "after that."_s);
        QCOMPARE(box.view.question().value(u"yes"_s).toString(), u"Mount and restart"_s);
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.changes.size() == 1; }, 10000));
        QCOMPARE(bodyOf(media.changes.first()).value(u"boot"_s).toString(), u"once"_s);
        QCOMPARE(bodyOf(media.changes.first()).value(u"restart"_s).toBool(), true);
    }

    void aLookThatGetsNoAnswerKeepsWhatWasShownButTheDriveAndIsAskedAgainOnce()
    {
        Media media;
        media.mounted = QJsonObject{
            {u"drive"_s, u"cd"_s}, {u"image"_s, u"win11.iso"_s}, {u"name"_s, u"win11.iso"_s}, {u"read_only"_s, true}};
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        QVERIFY(box.view.canUnmount());
        media.listFails = true;
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == 2; }, 10000));
        QVERIFY(QTest::qWaitFor([&] { return box.view.now().startsWith(u"Not known"_s); }, 10000));
        QCOMPARE(box.view.now(), u"Not known just now: its controller did not answer."_s);
        QCOMPARE(box.view.images().size(), 3);
        // What is in the drive is not known now: nothing is offered or said of it as if it were.
        QVERIFY(!box.view.canUnmount());
        QVERIFY(!box.hints().join(u'\n').contains(u"mounted now"_s));
        QCOMPARE(box.view.said(), u"Mount debian-13.iso: done. The KVM did not answer. Asked again in a moment."_s);
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == 3; }, 8000));
        QTest::qWait(5500);
        QCOMPARE(media.listAsked, 3);
    }

    void aBoxLetGoOfLeavesNothingHeldAndItsAnswerIsNobodys()
    {
        Media media;
        media.onChange = [](const Asked&) {
            Answered later{200, R"({"ok": true, "note": null, "mounted": null})"};
            later.afterMs = 800;
            return later;
        };
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        QVERIFY(box.underWay.contains(u"one:kvm"_s));
        box.view.letGo();
        QVERIFY(!box.view.opened());
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        // Something else is begun on the machine meanwhile: the answer that comes takes nothing of it.
        box.underWay.insert(u"one:kvm"_s, u"Press power button"_s);
        QVERIFY(QTest::qWaitFor([&] { return media.changes.size() == 1; }, 10000));
        QTest::qWait(1200);
        QCOMPARE(box.underWay.value(u"one:kvm"_s), u"Press power button"_s);
        QVERIFY(box.told.isEmpty());
    }

    void aMountThatGoesOnWhenTheBoxIsClosedIsToldInTheCorner()
    {
        Media media;
        media.onChange = [](const Asked&) {
            Answered later{
                409,
                R"({"code": "busy", "message": "Something else is being done to Build server at this moment. Wait for it to finish."})"};
            later.afterMs = 500;
            return later;
        };
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        box.view.close();
        QVERIFY(QTest::qWaitFor([&] { return !box.told.isEmpty(); }, 10000));
        QCOMPARE(box.told, QStringList{u"no: Build server: Something else is being done to Build server at this "
                                       "moment. Wait for it to finish."_s});
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
    }

    void anUploadIsAskedFirstThenGoesAndSaysHowFarItHasCome()
    {
        Media media;
        media.uploading = QJsonObject{
            {u"name"_s, u"tools.img"_s}, {u"size"_s, 3'000'000}, {u"sent"_s, 1'500'000}, {u"by"_s, u"anna"_s}};
        media.onChange = [](const Asked& asked) {
            const QByteArray sha = QCryptographicHash::hash(asked.body, QCryptographicHash::Sha256).toHex();
            Answered later{200, "{\"ok\": true, \"image\": null, \"sha256\": \"" + sha + "\"}"};
            later.afterMs = 2500;
            return later;
        };
        Box box(media);
        const QString path = box.file(u"tools.img"_s, 3'000'000);
        box.open();
        QVERIFY(box.listed());
        QVERIFY(box.view.canUpload());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        const QVariantMap asked = box.view.question();
        QCOMPARE(asked.value(u"title"_s).toString(), u"Upload tools.img"_s);
        QCOMPARE(asked.value(u"text"_s).toString(),
                 u"tools.img (3.0 MB) is carried through qKVM to the KVM's own storage of Build server as this program "
                 "reads it, and kept nowhere on the way. Keep this box open until it is done: closing it stops the "
                 "upload, and what came of it is taken off the KVM."_s);
        QCOMPARE(asked.value(u"yes"_s).toString(), u"Upload"_s);
        QVERIFY(!asked.value(u"danger"_s).toBool());
        QVERIFY(media.changes.isEmpty());
        box.view.answer(true);
        QVERIFY(box.view.busy());
        QCOMPARE(box.view.said(), u"Uploading tools.img..."_s);
        // How far it has come, as the server counts it.
        QVERIFY(QTest::qWaitFor([&] { return box.view.said() == u"Uploading tools.img: 1.5 MB of 3.0 MB (50 %)"_s; },
                                5000));
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy(); }, 10000));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        const QString sha = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
        // (In the corner that it was done; its SHA-256 in the box, as the page says them.)
        QCOMPARE(box.told, QStringList{u"ok: Build server: Upload tools.img done"_s});
        QCOMPARE(media.changes.size(), 1);
        const Asked& sent = media.changes.first();
        QCOMPARE(sent.path, QByteArray("/api/v1/devices/kvm/media/upload?name=tools.img&machine=Build%20server"));
        QCOMPARE(sent.headers.value("content-length"), QByteArray("3000000"));
        QCOMPARE(sent.headers.value("content-type"), QByteArray("application/octet-stream"));
        QVERIFY(sent.body == content);
        QVERIFY(
            QTest::qWaitFor([&] { return box.view.said() == u"tools.img is on the KVM, SHA-256 "_s + sha; }, 10000));
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
    }

    void whatTheLastLookSaysStandsAgainstAnUploadIsSaidAndNothingIsSent_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<qint64>("size");
        QTest::addColumn<qint64>("free");
        QTest::addColumn<QString>("said");
        QTest::newRow("there already")
            << u"debian-13.iso"_s << qint64{1000} << qint64{9'000'000'000}
            << u"debian-13.iso is on the KVM's own storage already: delete it first, or upload it under another name."_s;
        QTest::newRow("a part of it there")
            << u"half.img"_s << qint64{1000} << qint64{9'000'000'000}
            << u"A part of an earlier upload of half.img is on the KVM: delete it in the list first, or upload it "
               "under another name."_s;
        QTest::newRow("no room") << u"tools.img"_s << qint64{3'000'000} << qint64{1000}
                                 << u"The KVM's own storage has 1.0 kB free, and tools.img is 3.0 MB: delete an image "
                                    "there first."_s;
        QTest::newRow("empty") << u"tools.img"_s << qint64{0} << qint64{9'000'000'000}
                               << u"An empty file is no image."_s;
        QTest::newRow("a name it does not take")
            << u"#tools.img"_s << qint64{1000} << qint64{9'000'000'000}
            << u"An image's name is of letters, digits, spaces and . _ + - ( ) [ ] ~ , (200 at the most), and neither "
               "begins nor ends with a space or a dot. Rename the file and upload it again."_s;
    }

    void whatTheLastLookSaysStandsAgainstAnUploadIsSaidAndNothingIsSent()
    {
        QFETCH(const QString, name);
        QFETCH(const qint64, size);
        QFETCH(const qint64, free);
        QFETCH(const QString, said);
        Media media;
        media.free = free;
        Box box(media);
        box.file(name, size);
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.said().isEmpty(); }, 5000));
        QVERIFY(box.view.question().isEmpty());
        QCOMPARE(box.view.said(), said);
        QVERIFY(box.view.saidBad());
        QTest::qWait(200);
        QVERIFY(media.changes.isEmpty());
    }

    void anUploadCutOffIsSaidInTheServersWordsAndNothingIsShownAsDone()
    {
        Media media;
        media.onChange = [](const Asked&) {
            return Answered{
                504,
                R"({"code": "timeout", "message": "The KVM took nothing for 60 seconds, and the upload was stopped. Nothing of it is left on the KVM.", "sent": 1000})"};
        };
        Box box(media);
        box.file(u"tools.img"_s, 3'000'000);
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.listAsked == 2; }, 10000));
        QVERIFY(QTest::qWaitFor([&] { return box.view.said().startsWith(u"The KVM took nothing"_s); }, 10000));
        QCOMPARE(box.view.said(), u"The KVM took nothing for 60 seconds, and the upload was stopped. Nothing of it is "
                                  "left on the KVM."_s);
        QVERIFY(box.view.saidBad());
        QVERIFY(box.told.isEmpty());
    }

    void closingTheBoxStopsAnUpload()
    {
        Media media;
        media.onChange = [](const Asked&) {
            Answered never;
            never.never = true;
            return never;
        };
        Box box(media);
        box.file(u"tools.img"_s, 3'000'000);
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.changes.size() == 1; }, 10000));
        QVERIFY(box.view.busy());
        box.view.close();
        QVERIFY(!box.view.opened());
        QVERIFY(QTest::qWaitFor([&] { return !box.underWay.contains(u"one:kvm"_s); }, 5000));
        QVERIFY(!box.view.busy());
        QTest::qWait(300);
        QVERIFY(box.told.isEmpty());
    }

    // An image on a share far away, two seconds to open and two a read: the box asks, and the image goes, while the
    // thread the program draws its windows on goes on the whole time; and it comes whole and in order.
    void anImageOnAShareFarAwayIsReadBesideTheBoxAndGoesWhole()
    {
        Media media;
        media.onChange = [](const Asked&) { return Answered{200, R"({"ok": true, "image": null, "sha256": null})"}; };
        Box box(media);
        box.file(u"far.img"_s, 600'000);
        box.view.setImageSource(
            farAway([] { return std::make_unique<FarAwayFile>(600'000, 2000, -1, -1, nullptr, 2000); }));
        box.open();
        QVERIFY(box.listed());
        const ThreadWatch watched;
        box.view.upload();
        // (A moment later, while it is not open yet, the box says that it is being opened; asked once it is.)
        QVERIFY(QTest::qWaitFor([&] { return box.view.said() == u"Opening far.img..."_s; }, 1900));
        QVERIFY(box.view.question().isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 10000));
        QCOMPARE(box.view.question().value(u"title"_s).toString(), u"Upload far.img"_s);
        QCOMPARE(box.view.said(), QString());
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.changes.size() == 1 && !box.view.busy(); }, 30000));
        QVERIFY(media.changes.first().body == patterned(600'000));
        QCOMPARE(media.changes.first().headers.value("content-length"), QByteArray("600000"));
        QCOMPARE(box.told, QStringList{u"ok: Build server: Upload far.img done"_s});
        QVERIFY2(watched.longest < 500, qPrintable(u"the thread stood still for %1 ms"_s.arg(watched.longest)));
    }

    // The share stops answering on the way: the box goes on, the upload waits, and is cut off as one whose line stops;
    // a read that fails says why. Neither is shown as done.
    void aShareThatStopsAnsweringOrFailsLeavesTheBoxGoingAndTheUploadIsNotDone()
    {
        Media media;
        Box box(media);
        box.client.setUploadQuietMs(1500);
        box.file(u"stops.img"_s, 4'000'000);
        const auto gate = std::make_shared<FarAwayFile::Gate>();
        box.view.setImageSource(
            farAway([gate] { return std::make_unique<FarAwayFile>(4'000'000, 0, -1, 1'000'000, gate, 2000); }));
        box.open();
        QVERIFY(box.listed());
        const ThreadWatch watched;
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 10000));
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.listAsked == 2; }, 15000));
        gate->letGo();
        QVERIFY(QTest::qWaitFor([&] { return box.view.said().startsWith(u"No answer"_s); }, 5000));
        QCOMPARE(
            box.view.said(),
            u"No answer to \"Upload stops.img\": it may have been done. Look at Build server before trying again."_s);
        QVERIFY(box.view.saidBad());
        QVERIFY(box.told.isEmpty());
        QVERIFY2(watched.longest < 500, qPrintable(u"the thread stood still for %1 ms"_s.arg(watched.longest)));

        box.view.setImageSource(
            farAway([] { return std::make_unique<FarAwayFile>(4'000'000, 0, 700'000, -1, nullptr, 2000); }));
        const ThreadWatch again;
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 10000));
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.listAsked == 3; }, 15000));
        QVERIFY(QTest::qWaitFor([&] { return box.view.said().startsWith(u"The image could not be read"_s); }, 5000));
        QCOMPARE(box.view.said(), u"The image could not be read on the way, and nothing more of it went: The specified "
                                  "network name is no longer available."_s);
        QVERIFY(box.view.saidBad());
        QVERIFY(box.told.isEmpty());
        QVERIFY2(again.longest < 500, qPrintable(u"the thread stood still for %1 ms"_s.arg(again.longest)));
    }

    // A file that comes open after the box was closed, or after another was chosen, is nobody's: no question is asked
    // of it, and it is let go of.
    void aFileThatComesOpenTooLateIsNobodys()
    {
        Media media;
        Box box(media);
        box.file(u"late.img"_s, 600'000);
        box.view.setImageSource(
            farAway([] { return std::make_unique<FarAwayFile>(600'000, 0, -1, -1, nullptr, 1000); }));
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        box.view.close();
        box.open();
        QVERIFY(box.listed());
        QTest::qWait(1500);
        QVERIFY(box.view.question().isEmpty());
        // Another chosen meanwhile: the question is of that one.
        box.view.upload();
        box.file(u"later.img"_s, 600'000);
        box.view.setImageSource(farAway([] { return std::make_unique<FarAwayFile>(700'000, 0); }));
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        QCOMPARE(box.view.question().value(u"title"_s).toString(), u"Upload later.img"_s);
        QTest::qWait(1500);
        QCOMPARE(box.view.question().value(u"title"_s).toString(), u"Upload later.img"_s);
        QVERIFY(box.view.question().value(u"text"_s).toString().contains(u"(700 kB)"_s));
        QVERIFY(media.changes.isEmpty());
    }

    void aCountOfBytesIsWrittenAsThePageWritesIt()
    {
        QCOMPARE(ui::MediaView::bytesText(0), u"0 bytes"_s);
        QCOMPARE(ui::MediaView::bytesText(999), u"999 bytes"_s);
        QCOMPARE(ui::MediaView::bytesText(614'400), u"614 kB"_s);
        QCOMPARE(ui::MediaView::bytesText(2'359'293'952), u"2.4 GB"_s);
        QCOMPARE(ui::MediaView::bytesText(16'000'000'000), u"16 GB"_s);
    }
};

QTEST_MAIN(TestMediaView)
#include "test_media_view.moc"
