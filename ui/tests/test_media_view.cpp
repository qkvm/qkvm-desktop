#include <atomic>
#include <functional>
#include <memory>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <qkvm/core/board_holds.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/server_client.h>

#include "../../core/tests/far_away_file.h"
#include "../../core/tests/made_up_kvm.h"
#include "../../core/tests/memory_secrets.h"
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
    // What the server says of the upload under way, while one is; and what its lists say of one (another's).
    QJsonValue uploading;
    QJsonValue listsUploading;
    // What a mount, an unmount and a deletion are answered with; without one, that it was done.
    std::function<Answered(const Asked&)> onChange;
    bool listFails = false;
    int listAfterMs = 0; // a list answered only after this long
    int listAsked = 0;
    QList<Asked> changes;
    // As a server 0.3.2 (server.py, media_listed and media_moved): the mark of the machine the tile is at now, said
    // with every list; a change that sends another is refused, 409 "moved", and nothing is done (counted here); one
    // that sends none is served as before. Empty: a server 0.3.1, which lists none and reads none.
    QString listed = QString::fromLatin1(firstMark);
    int refusedAsMoved = 0;

    static constexpr const char* firstMark = "5d41402abc4b2a76b9719d911017c592";
    static constexpr const char* otherMark = "7d793037a0760186574b0282f2f435e7";

    QByteArray list() const
    {
        QJsonObject said{{u"images"_s, images},
                         {u"unseen"_s, QJsonArray{}},
                         {u"left_out"_s, 0},
                         {u"notes"_s, notes},
                         {u"space"_s, QJsonObject{{u"free"_s, free}, {u"size"_s, 32'000'000'000}}},
                         {u"mounted"_s, mounted},
                         {u"offers"_s, offers},
                         {u"uploading"_s, listsUploading}};
        if (!listed.isEmpty()) {
            said[u"listed"_s] = listed;
        }
        return QJsonDocument(said).toJson(QJsonDocument::Compact);
    }

    // The `listed` a change sent: in its body, or an upload's in its address; null: it sent none.
    static QString listedBy(const Asked& asked)
    {
        if (asked.path.contains("/media/upload?")) {
            const QUrlQuery query(QString::fromLatin1(asked.path.mid(asked.path.indexOf('?') + 1)));
            return query.hasQueryItem(u"listed"_s) ? query.queryItemValue(u"listed"_s) : QString();
        }
        const QJsonValue sent = QJsonDocument::fromJson(asked.body).object().value(u"listed"_s);
        return sent.isString() ? sent.toString() : QString();
    }

    Answered moved(const Asked& asked) const
    {
        const QString what = asked.path.contains("/media/upload")    ? u"uploaded"_s
                             : asked.path.endsWith("/media/unmount") ? u"taken out"_s
                             : asked.path.endsWith("/media/delete")  ? u"deleted"_s
                                                                     : u"mounted"_s;
        return {409, QJsonDocument(QJsonObject{{u"code"_s, u"moved"_s},
                                               {u"message"_s, u"Build server is not the machine its images were "
                                                              "listed for any more: its address, or what qKVM knows of "
                                                              "the machine there, was changed since. Nothing was %1: "
                                                              "list its images again."_s.arg(what)}})
                         .toJson(QJsonDocument::Compact)};
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
            Answered listing = listFails ? Answered{504, R"({"code": "timeout", "message": "The KVM did not answer."})"}
                                         : Answered{200, list()};
            listing.afterMs = listAfterMs;
            return listing;
        }
        if (asked.method == "POST" && asked.path.startsWith("/api/v1/devices/kvm/media")) {
            changes.append(asked);
            // (Before anything else is judged, and nothing asked of either machine.)
            if (const QString sent = listedBy(asked); !listed.isEmpty() && !sent.isNull() && sent != listed) {
                ++refusedAsMoved;
                return moved(asked);
            }
            return onChange ? onChange(asked) : Answered{200, R"({"ok": true, "note": null, "mounted": null})"};
        }
        return {404, R"({"code": "not_found", "message": "Unknown"})"};
    }
};

// The place a box is opened with: the server's client, through this, which counts what the box hands it and what it
// hands the box back, as each happens. That nothing went once a late answer came is then asked of what was handed
// over, not of how long was waited: a slow PC makes such a check late, never vacuous.
class Counted : public QObject, public core::MediaPlace {
public:
    explicit Counted(core::ServerClient& client) : m_client(client) {}

    int looks = 0;  // lists asked
    int looked = 0; // lists answered, counted once the box was handed the answer
    int sent = 0;   // mounts, unmounts, deletions and uploads handed over
    // Where `holdsUploads`: an upload goes to nobody, its answer is the case's to give (`uploads`, in the order they
    // came), and every stop of it is counted.
    struct Held {
        std::function<void(base::Result<core::MediaUploaded>)> done;
        std::shared_ptr<int> stops = std::make_shared<int>(0);
    };
    bool holdsUploads = false;
    QList<Held> uploads;

    void askMedia(const QString& machine, std::function<void(base::Result<core::MediaList>)> done) override
    {
        ++looks;
        m_client.askMedia(machine, [guard = QPointer<Counted>(this),
                                    done = std::move(done)](const base::Result<core::MediaList>& list) {
            done(list);
            if (guard != nullptr) {
                ++guard->looked;
            }
        });
    }
    void mountMedia(const QString& machine, const core::MediaMount& mount,
                    std::function<void(base::Result<core::MediaDone>)> done) override
    {
        ++sent;
        m_client.mountMedia(machine, mount, std::move(done));
    }
    void unmountMedia(const QString& machine, const QString& image, const QString& named, const QString& listed,
                      std::function<void(base::Result<core::MediaDone>)> done) override
    {
        ++sent;
        m_client.unmountMedia(machine, image, named, listed, std::move(done));
    }
    void deleteMedia(const QString& machine, const QString& image, const QString& named, const QString& listed,
                     std::function<void(base::Result<core::MediaDone>)> done) override
    {
        ++sent;
        m_client.deleteMedia(machine, image, named, listed, std::move(done));
    }
    std::function<void()> uploadMedia(const QString& machine, const QString& name, const QString& named,
                                      const QString& listed, std::unique_ptr<QIODevice> image,
                                      std::function<void(base::Result<core::MediaUploaded>)> done) override
    {
        ++sent;
        if (holdsUploads) {
            Held held{std::move(done)};
            uploads.append(held);
            return [stops = held.stops] { ++*stops; };
        }
        return m_client.uploadMedia(machine, name, named, listed, std::move(image), std::move(done));
    }
    void askUpload(const QString& machine,
                   std::function<void(base::Result<std::optional<core::MediaUploading>>)> done) override
    {
        m_client.askUpload(machine, std::move(done));
    }
    QByteArray mediaIdentity() const override { return m_client.mediaIdentity(); }
    bool saidNothing(const base::Error& error) const override { return m_client.saidNothing(error); }
    std::optional<base::Error> movedSince(const QString& machine, const QString& bound) const override
    {
        return m_client.movedSince(machine, bound);
    }

private:
    core::ServerClient& m_client;
};

// What the box says in the corner where a later look lists the machine by another mark than its first list did.
QString movedSaid(const QString& name = u"Build server"_s)
{
    return u"no: "_s + name +
           u" is not the machine its images were listed for any more: its address, or what qKVM knows of the machine "
           "there, was changed since. Its media box was closed: list its images again from its menu."_s;
}

// What the box says in the corner where this program was signed in otherwise while it was open.
QString signedInSaid(const QString& name = u"Build server"_s)
{
    return u"no: "_s + name +
           u": its media box was closed: it was opened under another sign-in than this program's now. Open it again "
           "from its menu."_s;
}

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
        view.open(&place, u"kvm"_s, name, u"wall:kvm"_s, &underWay, u"one:kvm"_s);
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
    Counted place{client};
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
        // (With the mark of the list it was picked from: 0.3.2.)
        QCOMPARE(bodyOf(media.changes.first()), QJsonObject({{u"image"_s, u"debian-13.iso"_s},
                                                             {u"machine"_s, u"Build server"_s},
                                                             {u"listed"_s, media.listed},
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
                 QJsonObject(
                     {{u"image"_s, u"win11.iso"_s}, {u"machine"_s, u"Build server"_s}, {u"listed"_s, media.listed}}));
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == 2; }, 10000));
        box.view.pick(u"debian-13.iso"_s);
        box.view.remove();
        QCOMPARE(box.view.question().value(u"text"_s).toString(),
                 u"debian-13.iso is deleted from the KVM's own storage of Build server. It cannot be brought back "
                 "from here."_s);
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.changes.size() == 2; }, 10000));
        QCOMPARE(media.changes.last().path, QByteArray("/api/v1/devices/kvm/media/delete"));
        QCOMPARE(bodyOf(media.changes.last()), QJsonObject({{u"image"_s, u"debian-13.iso"_s},
                                                            {u"machine"_s, u"Build server"_s},
                                                            {u"listed"_s, media.listed}}));
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

    // A guest of Proxmox, as its driver offers it (review c9b-2, M1): started from it once or until it is unmounted,
    // and its words the web's (MEDIA_FOOT, the mount and unmount questions): none of a KVM's boot menu.
    void aGuestOfProxmoxIsOfferedItsStartsInTheWebsWords()
    {
        Media media;
        media.offers = QJsonObject{{u"drives"_s, QJsonArray{u"cd"_s}},
                                   {u"boot"_s, QJsonArray{u"once"_s, u"until_unmounted"_s}},
                                   {u"restart"_s, true},
                                   {u"upload"_s, false},
                                   {u"where"_s, u"proxmox"_s},
                                   {u"writable"_s, QJsonArray{}},
                                   {u"delete"_s, false}};
        media.images = QJsonArray{QJsonObject{{u"id"_s, u"local:iso/debian-13.iso"_s},
                                              {u"name"_s, u"debian-13.iso"_s},
                                              {u"where"_s, u"local"_s},
                                              {u"size"_s, 700'000'000},
                                              {u"partial"_s, false}}};
        media.mounted = QJsonObject{{u"drive"_s, u"cd"_s},
                                    {u"image"_s, u"local:iso/alpine-3.21.iso"_s},
                                    {u"name"_s, u"alpine-3.21.iso"_s},
                                    {u"where"_s, u"local"_s},
                                    {u"read_only"_s, true},
                                    {u"first"_s, true}};
        Box box(media);
        box.open(u"web"_s);
        QVERIFY(box.listed());
        QCOMPARE(box.view.foot(),
                 u"An image goes into the guest's CD-ROM drive, read-only. Proxmox has no start \"once\": qKVM starts "
                 "the guest anew with the CD-ROM drive first and puts its boot order back after, so a restart from "
                 "inside the guest starts from the image again while it is in the drive. A restart stops the guest at "
                 "once and starts it again."_s);
        for (const QVariant& one : box.view.boots()) {
            QVERIFY2(one.toMap().value(u"enabled"_s).toBool(), qPrintable(one.toMap().value(u"value"_s).toString()));
        }
        box.view.pick(u"local:iso/debian-13.iso"_s);
        box.view.mount();
        QCOMPARE(box.view.question().value(u"text"_s).toString(),
                 u"web gets debian-13.iso (on local) in its CD-ROM drive, read-only, and its boot order is left as it "
                 "is."_s);
        box.view.answer(false);
        box.view.chooseBoot(u"until_unmounted"_s);
        box.view.mount();
        QCOMPARE(box.view.question().value(u"text"_s).toString(),
                 u"web gets debian-13.iso (on local) in its CD-ROM drive, read-only, and starts from it at its next "
                 "start, until it is unmounted."_s);
        QCOMPARE(box.view.question().value(u"yes"_s).toString(), u"Mount"_s);
        box.view.answer(false);
        box.view.chooseBoot(u"once"_s);
        box.view.mount();
        const QString once = box.view.question().value(u"text"_s).toString();
        QVERIFY2(once.contains(u"is restarted now to start from it once"_s), qPrintable(once));
        QVERIFY(!once.contains(u"KVM's drive"_s));
        box.view.answer(false);
        box.view.unmount();
        QCOMPARE(box.view.question().value(u"text"_s).toString(),
                 u"alpine-3.21.iso is taken out of the drive of web, and its boot order put back where it was set "
                 "from here. A system running from it loses it."_s);
        box.view.answer(false);
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
        QCOMPARE(sent.path, "/api/v1/devices/kvm/media/upload?name=tools.img&machine=Build%20server&listed=" +
                                media.listed.toLatin1());
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
        // (Its list, the look before the upload, and the look after it.)
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.listAsked == 3; }, 10000));
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

    // Upload A is stopped as the box is let go of (the program locked), and its answer comes only once upload B is
    // under way in the box opened again: that late answer is A's alone, and closing the box stops B still. (Outside
    // review 4.5: A's answer took B's stop away.)
    void aLateAnswerOfAStoppedUploadLeavesTheNextOneStoppable()
    {
        Media media;
        Box box(media);
        box.place.holdsUploads = true;
        const auto uploaded = [&box](const QString& name, qsizetype count) {
            box.file(name, 1000);
            box.view.upload();
            if (!QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000)) {
                return false;
            }
            box.view.answer(true);
            return QTest::qWaitFor([&] { return box.place.uploads.size() == count; }, 10000);
        };
        box.open();
        QVERIFY(box.listed());
        QVERIFY(uploaded(u"a.img"_s, 1));
        box.view.letGo();
        QCOMPARE(*box.place.uploads.at(0).stops, 1);
        box.open();
        QVERIFY(box.listed());
        QVERIFY(uploaded(u"b.img"_s, 2));
        QVERIFY(box.view.busy());
        box.place.uploads.at(0).done(
            base::Error{core::MediaPlace::uploadStopped, "The upload was stopped before all of it had gone."});
        QVERIFY(box.view.busy());
        QCOMPARE(*box.place.uploads.at(1).stops, 0);
        box.view.close();
        QCOMPARE(*box.place.uploads.at(1).stops, 1);
        QCOMPARE(*box.place.uploads.at(0).stops, 1);
        // B's own answer, after it was stopped, finds the box closed and nobody's stop.
        box.place.uploads.at(1).done(
            base::Error{core::MediaPlace::uploadStopped, "The upload was stopped before all of it had gone."});
        QVERIFY(!box.view.busy());
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
        // (Its list, the look before the upload, and the look after it.)
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.listAsked == 3; }, 15000));
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
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy() && media.listAsked == 5; }, 15000));
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

    void aChangeWhoseMachineMovedMeanwhileIsDoneNowhereAndTheBoxCloses_data()
    {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("done"); // (the server's word for it)
        QTest::newRow("a mount") << u"mount"_s << u"mounted"_s;
        QTest::newRow("an unmount") << u"unmount"_s << u"taken out"_s;
        QTest::newRow("a deletion") << u"delete"_s << u"deleted"_s;
    }

    void aChangeWhoseMachineMovedMeanwhileIsDoneNowhereAndTheBoxCloses()
    {
        // This program's finding 21 (web 0.3.2: server.py, media_moved; app.js, carry): the box listed one KVM, and its
        // tile was edited to another KVM on the server while the box was open. What it asks goes with the mark of the
        // list it was picked from; the server does nothing with it (409 moved), and the box, made for the machine it
        // listed, closes and says why in the server's words. Opened again, it lists the machine the tile is at now.
        QFETCH(const QString, what);
        QFETCH(const QString, done);
        Media media;
        media.mounted = QJsonObject{
            {u"drive"_s, u"cd"_s}, {u"image"_s, u"win11.iso"_s}, {u"name"_s, u"win11.iso"_s}, {u"read_only"_s, true}};
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        if (what == "mount"_L1) {
            box.view.mount();
        } else if (what == "unmount"_L1) {
            box.view.unmount();
        } else {
            box.view.remove();
        }
        QVERIFY(!box.view.question().isEmpty());
        // Meanwhile its tile is edited to another KVM: the server knows the tile's machine by another mark.
        media.listed = QString::fromLatin1(Media::otherMark);
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.opened(); }, 10000));
        QCOMPARE(media.changes.size(), 1);
        QCOMPARE(media.refusedAsMoved, 1);
        QCOMPARE(Media::listedBy(media.changes.first()), QString::fromLatin1(Media::firstMark));
        QCOMPARE(box.told, QStringList{u"no: Build server is not the machine its images were listed for any more: its "
                                       "address, or what qKVM knows of the machine there, was changed since. Nothing "
                                       "was "_s +
                                       done + u": list its images again."_s});
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        QCOMPARE(box.done, QStringList{u"wall:kvm"_s});
        // Closed, it asks nothing more.
        const int looks = media.listAsked;
        QTest::qWait(300);
        QCOMPARE(media.listAsked, looks);
        QCOMPARE(media.changes.size(), 1);
        // Opened again, of the machine the tile is at now: what is picked from that list goes.
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.changes.size() == 2 && !box.view.busy(); }, 10000));
        QCOMPARE(Media::listedBy(media.changes.last()), QString::fromLatin1(Media::otherMark));
        QCOMPARE(media.refusedAsMoved, 1);
        QVERIFY(box.view.opened());
        QCOMPARE(box.told.last(), u"ok: Build server: Mount debian-13.iso done"_s);
    }

    void anUploadWhoseMachineMovedMeanwhileSendsNothingAndTheBoxCloses_data()
    {
        QTest::addColumn<QString>("mark"); // what the look before the upload lists the tile's machine by
        QTest::newRow("another machine") << QString::fromLatin1(Media::otherMark);
        // (Stricter than the page, which lets the upload go where that list names no machine.)
        QTest::newRow("no mark any more") << QString();
    }

    void anUploadWhoseMachineMovedMeanwhileSendsNothingAndTheBoxCloses()
    {
        // (Web 0.3.2, 49a8f6d8: its independent review's L4.) The server refuses an upload listed for another machine
        // before it reads a byte of it, and closes the line under what is still being sent, so its answer can be lost
        // on the way; the box would then say that it may have been done, where nothing was. So the box asks for the
        // list again before a byte goes: where the tile is at another machine by now it says so, in the words of the
        // other three, and closes; nothing of the image is sent.
        QFETCH(const QString, mark);
        Media media;
        Box box(media);
        box.file(u"tools.img"_s, 3'000'000);
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        media.listed = mark;
        const int looks = media.listAsked;
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.opened(); }, 10000));
        QCOMPARE(media.listAsked, looks + 1);
        // (Closed in the look's answer, as it was handed over: nothing went then, or after.)
        QCOMPARE(box.place.sent, 0);
        QVERIFY(media.changes.isEmpty());
        QCOMPARE(box.told, QStringList{u"no: Build server is not the machine its images were listed for any more: its "
                                       "address, or what qKVM knows of the machine there, was changed since. Nothing "
                                       "was uploaded: list its images again."_s});
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
    }

    void anUploadWhoseLookBeforeGetsNoAnswerGoesAndTheServerStillHoldsToItsList()
    {
        // (The page's too.) Where that look gets no answer, the upload goes with the mark of its list, and the server
        // refuses it where the tile's machine is another by then: the box closes on its words, as for the other three.
        Media media;
        Box box(media);
        box.file(u"tools.img"_s, 300'000);
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        media.listFails = true;
        media.listed = QString::fromLatin1(Media::otherMark);
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.opened(); }, 10000));
        QCOMPARE(media.changes.size(), 1);
        QCOMPARE(media.refusedAsMoved, 1);
        QCOMPARE(Media::listedBy(media.changes.first()), QString::fromLatin1(Media::firstMark));
        QCOMPARE(box.told, QStringList{u"no: Build server is not the machine its images were listed for any more: its "
                                       "address, or what qKVM knows of the machine there, was changed since. Nothing "
                                       "was uploaded: list its images again."_s});
    }

    void closingTheBoxWhileTheLookBeforeAnUploadIsOutSendsNothing()
    {
        // (The page's: its Close stops the upload in that look too.) The box closed, the machine is nobody's at once,
        // and the file is let go of, whenever that look's answer comes: a server that is slow to list (it asks the
        // KVM) does not hold either for up to a minute. And when that answer comes, nothing of the image goes.
        Media media;
        Box box(media);
        box.file(u"tools.img"_s, 300'000);
        const auto gone = std::make_shared<std::atomic<bool>>(false);
        box.view.setImageSource(farAway([gone] {
            auto file = std::make_unique<FarAwayFile>(300'000, 0);
            QObject::connect(file.get(), &QObject::destroyed, file.get(), [gone] { *gone = true; });
            return file;
        }));
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        media.listAfterMs = 2500;
        const int looks = media.listAsked;
        const int looked = box.place.looked;
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == looks + 1; }, 5000));
        QVERIFY(box.view.busy());
        QVERIFY(box.underWay.contains(u"one:kvm"_s));
        QVERIFY(!gone->load());
        box.view.close();
        QVERIFY(!box.view.busy());
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        QCOMPARE(box.done, QStringList{u"wall:kvm"_s});
        // (Let go of on the thread that reads it, once that sees the stop: at once, but for how soon a loaded PC
        // wakes a thread.)
        QVERIFY(QTest::qWaitFor([&] { return gone->load(); }, 5000));
        // Its answer came, a while after, and was handed to the box: nothing more.
        QVERIFY(QTest::qWaitFor([&] { return box.place.looked == looked + 1; }, 10000));
        QCOMPARE(box.place.sent, 0);
        QVERIFY(media.changes.isEmpty());
        QVERIFY(box.told.isEmpty());
        QCOMPARE(box.done, QStringList{u"wall:kvm"_s});
    }

    void anUploadWhoseSignInChangedWhileTheLookWasOutSendsNothing()
    {
        // (Signed in to the server again as somebody else while the look is out, the box still open: the yes was
        // another sign-in's, and nothing of the image goes when the look's answer comes.)
        Media media;
        Box box(media);
        box.file(u"tools.img"_s, 300'000);
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        media.listAfterMs = 1000;
        const int looks = media.listAsked;
        const int looked = box.place.looked;
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == looks + 1; }, 5000));
        box.client.setSession(QByteArray(43, 't'));
        QVERIFY(QTest::qWaitFor([&] { return box.place.looked == looked + 1; }, 10000));
        QVERIFY(!box.view.busy());
        QCOMPARE(box.place.sent, 0);
        QVERIFY(media.changes.isEmpty());
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        // (And the box, open with nothing it asks ever answered now, closes and says why: B-L1.)
        QVERIFY(!box.view.opened());
        QCOMPARE(box.told, QStringList{signedInSaid()});
    }

    void aBoxLetGoOfWhileTheLookBeforeAnUploadIsOutSendsNothing()
    {
        // (Its server removed from this program, or signed out meanwhile: MachineActions lets go of the box.) As
        // closed: nothing goes when the look's answer comes.
        Media media;
        Box box(media);
        box.file(u"tools.img"_s, 300'000);
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        media.listAfterMs = 1000;
        const int looks = media.listAsked;
        const int looked = box.place.looked;
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == looks + 1; }, 5000));
        box.view.letGo();
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        QVERIFY(QTest::qWaitFor([&] { return box.place.looked == looked + 1; }, 10000));
        QCOMPARE(box.place.sent, 0);
        QVERIFY(media.changes.isEmpty());
        QVERIFY(box.told.isEmpty());
    }

    void aQuestionAskedBeforeASignInChangeSendsNothingAndTheBoxCloses_data()
    {
        QTest::addColumn<QString>("what");
        QTest::newRow("a mount") << u"mount"_s;
        QTest::newRow("an unmount") << u"unmount"_s;
        QTest::newRow("a deletion") << u"delete"_s;
    }

    void aQuestionAskedBeforeASignInChangeSendsNothingAndTheBoxCloses()
    {
        // The outside review of `listed`, B-L1: signed in to the server again while a question was open (as another
        // account, or to another server through the same client), the yes was sent with the new sign-in. Now it is
        // not: the question was the other sign-in's, and the box, which no look of it can answer any more, closes and
        // says why. (The upload has its own case: anUploadWhoseSignInChangedWhileTheLookWasOutSendsNothing.)
        QFETCH(const QString, what);
        Media media;
        media.mounted = QJsonObject{
            {u"drive"_s, u"cd"_s}, {u"image"_s, u"win11.iso"_s}, {u"name"_s, u"win11.iso"_s}, {u"read_only"_s, true}};
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        if (what == "mount"_L1) {
            box.view.mount();
        } else if (what == "unmount"_L1) {
            box.view.unmount();
        } else {
            box.view.remove();
        }
        QVERIFY(!box.view.question().isEmpty());
        box.client.setSession(QByteArray(43, 't'));
        box.view.answer(true);
        QCOMPARE(box.place.sent, 0);
        QVERIFY(!box.view.busy());
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        QVERIFY(!box.view.opened());
        QCOMPARE(box.told, QStringList{signedInSaid()});
        QVERIFY(media.changes.isEmpty());
    }

    void aLookAnsweredAfterASignInChangeClosesTheBox()
    {
        // (B-L1.) Every look asked under one sign-in is dropped under another: the box stood open, showing what it
        // showed or nothing, and took clicks it could not carry. It closes when such a look comes back, and shows
        // nothing of it.
        Media media;
        media.listAfterMs = 1000;
        Box box(media);
        box.open();
        QVERIFY(box.view.opened());
        box.client.setSession(QByteArray(43, 't'));
        QVERIFY(QTest::qWaitFor([&] { return box.place.looked == 1; }, 10000));
        QVERIFY(!box.view.opened());
        QVERIFY(box.view.images().isEmpty());
        QCOMPARE(box.told, QStringList{signedInSaid()});
        QCOMPARE(box.place.looks, 1);
    }

    void aFileThatComesOpenAfterASignInChangeClosesTheBox()
    {
        // (B-L1.) The file chosen to upload comes open under another sign-in than the box was opened under: nothing is
        // asked of it, and the box, which said that it was opening it, closes and says why.
        Media media;
        Box box(media);
        box.file(u"late.img"_s, 600'000);
        box.view.setImageSource(
            farAway([] { return std::make_unique<FarAwayFile>(600'000, 0, -1, -1, nullptr, 1000); }));
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        box.client.setSession(QByteArray(43, 't'));
        QVERIFY(QTest::qWaitFor([&] { return !box.view.opened(); }, 10000));
        QVERIFY(box.view.question().isEmpty());
        QCOMPARE(box.told, QStringList{signedInSaid()});
        QCOMPARE(box.place.sent, 0);
    }

    void aMovedAnsweredAfterASignInChangeClosesTheBoxToo()
    {
        // (B-L1.) A change sent before the sign-in changed, answered 409 moved after it: told in the server's words,
        // and the box, of this opening whoever is signed in now, closes as for any moved.
        Media media;
        media.onChange = [&media](const Asked& asked) {
            Answered later = media.moved(asked);
            later.afterMs = 800;
            return later;
        };
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.changes.size() == 1; }, 10000));
        box.client.setSession(QByteArray(43, 't'));
        QVERIFY(QTest::qWaitFor([&] { return !box.view.busy(); }, 10000));
        QVERIFY(!box.view.opened());
        QCOMPARE(box.told, QStringList{u"no: Build server is not the machine its images were listed for any more: its "
                                       "address, or what qKVM knows of the machine there, was changed since. Nothing "
                                       "was mounted: list its images again."_s});
        QCOMPARE(box.place.sent, 1);
    }

    void whatTheLookBeforeAnUploadSaysStandsAgainstItIsSaidAndNothingIsSent_data()
    {
        QTest::addColumn<QString>("since"); // what is so on the server since the box listed it
        QTest::addColumn<QString>("said");
        QTest::newRow("there now")
            << u"there"_s
            << u"tools.img is on the KVM's own storage already: delete it first, or upload it under another name."_s;
        QTest::newRow("a part of it there now")
            << u"part"_s
            << u"A part of an earlier upload of tools.img is on the KVM: delete it in the list first, or upload it "
               "under another name."_s;
        QTest::newRow("no room now") << u"room"_s
                                     << u"The KVM's own storage has 1.0 kB free, and tools.img is 3.0 MB: delete an "
                                        "image there first."_s;
        QTest::newRow("another upload under way now")
            << u"busy"_s
            << u"other.img is being uploaded to Build server at this moment, by bob. Wait for it to finish."_s;
    }

    void whatTheLookBeforeAnUploadSaysStandsAgainstItIsSaidAndNothingIsSent()
    {
        // The outside review of `listed`, B-L2 (beyond the page, which asks this of the list shown alone): the server
        // refuses an upload at once, before it reads a byte, for more than another machine: the name there now, a part
        // of it, no room, another upload under way; and it closes the line under what is still being sent, so its
        // answer is often lost, and the box said that it may have been done. The look before an upload has the fresh
        // list in hand: what it says stands against the upload is said in the box's words, and nothing is sent.
        QFETCH(const QString, since);
        QFETCH(const QString, said);
        Media media;
        Box box(media);
        box.file(u"tools.img"_s, 3'000'000);
        box.open();
        QVERIFY(box.listed());
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        // Meanwhile, from another PC:
        if (since == "there"_L1 || since == "part"_L1) {
            media.images.append(QJsonObject{{u"id"_s, u"tools.img"_s},
                                            {u"name"_s, u"tools.img"_s},
                                            {u"size"_s, 3'000'000},
                                            {u"partial"_s, since == "part"_L1}});
        } else if (since == "room"_L1) {
            media.free = 1000;
        } else {
            media.listsUploading = QJsonObject{
                {u"name"_s, u"other.img"_s}, {u"size"_s, 5'000'000}, {u"sent"_s, 1000}, {u"by"_s, u"bob"_s}};
        }
        const int looked = box.place.looked;
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return box.place.looked == looked + 1 && !box.view.busy(); }, 10000));
        QCOMPARE(box.place.sent, 0);
        QVERIFY2(box.view.said().startsWith(said), qPrintable(box.view.said()));
        QVERIFY(box.view.saidBad());
        QVERIFY(box.view.opened());
        QVERIFY(box.told.isEmpty());
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        QVERIFY(media.changes.isEmpty());
    }

    void aLookThatListsAnotherMachineClosesTheBoxAndNothingMoreGoesToEither_data()
    {
        QTest::addColumn<QString>("mark"); // what the server lists the tile's machine by after the edit
        QTest::addColumn<bool>("again");   // the look after the mount gets no answer, and the one asked again lists it
        QTest::newRow("another machine, in the look after a mount") << QString::fromLatin1(Media::otherMark) << false;
        QTest::newRow("no mark any more, in the look after a mount") << QString() << false;
        QTest::newRow("another machine, in the look asked again") << QString::fromLatin1(Media::otherMark) << true;
    }

    void aLookThatListsAnotherMachineClosesTheBoxAndNothingMoreGoesToEither()
    {
        // The outside review of `listed`, B-M1 (stricter than the page, whose box shows every list it is answered and
        // takes each one's mark): the box is open for Build server at KVM A, and a mount there goes. Meanwhile an
        // admin edits the tile to KVM B, a twin with debian-13.iso in its drive, and the look after the mount lists
        // B's. The box is held to the mark of the list it was opened with, as this PC's own wall's box is to its first
        // listing: B's list is never shown under the box's title, the box closes and says why in the server's words,
        // and Unmount pressed then sends nothing to either machine. (A server that lists no mark any more, one before
        // 0.3.2 at its address now, cannot say which machine it lists: the same.)
        QFETCH(const QString, mark);
        QFETCH(const bool, again);
        Media media;
        media.onChange = [&media, mark, again](const Asked&) {
            // (The mount taken on A; the tile edited to B before the look after it.)
            media.listed = mark;
            media.mounted = QJsonObject{{u"drive"_s, u"cd"_s},
                                        {u"image"_s, u"debian-13.iso"_s},
                                        {u"name"_s, u"debian-13.iso"_s},
                                        {u"read_only"_s, true},
                                        {u"by"_s, u"bob"_s}};
            media.listFails = again;
            return Answered{200, R"({"ok": true, "note": null, "mounted": null})"};
        };
        Box box(media);
        QStringList shown;
        QObject::connect(&box.view, &ui::MediaView::changed, &box.view, [&] {
            if (box.view.opened()) {
                shown.append(box.view.now());
            }
        });
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        if (again) {
            QVERIFY(QTest::qWaitFor([&] { return box.place.looked == 2; }, 10000));
            QVERIFY(box.view.opened());
            media.listFails = false;
        }
        QVERIFY(QTest::qWaitFor([&] { return !box.view.opened(); }, 15000));
        QCOMPARE(box.place.looked, again ? 3 : 2);
        QCOMPARE(box.place.sent, 1);
        QCOMPARE(Media::listedBy(media.changes.first()), QString::fromLatin1(Media::firstMark));
        QCOMPARE(box.told, (QStringList{u"ok: Build server: Mount debian-13.iso done"_s, movedSaid()}));
        // B's drive was never shown as the box's machine's.
        QVERIFY2(!shown.join(u'\n').contains(u"mounted by bob"_s), qPrintable(shown.join(u'\n')));
        // Unmount pressed then: nothing is asked, and nothing goes.
        box.view.unmount();
        QVERIFY(box.view.question().isEmpty());
        box.view.answer(true);
        QVERIFY(!box.view.busy());
        QCOMPARE(box.place.sent, 1);
        QVERIFY(!box.underWay.contains(u"one:kvm"_s));
        QCOMPARE(media.changes.size(), 1);
        QCOMPARE(media.refusedAsMoved, 0);
    }

    void aMarkThatIsAnotherForTheSameMachineClosesTheBoxTooAndOpenedAgainItHoldsToItsNewList()
    {
        // (A server 0.3.2 makes its mark anew of what is so: where it first learns the machine's number between two
        // looks, the mark is another for the same machine.) The box cannot tell that from another machine, and closes
        // as for one; opened again, it is held to its new first list, whose mark a change goes with, and it says
        // nothing of what was done in the box before.
        Media media;
        media.onChange = [&media](const Asked&) {
            media.listed = QString::fromLatin1(Media::otherMark);
            media.mounted = QJsonObject{{u"drive"_s, u"cd"_s},
                                        {u"image"_s, u"debian-13.iso"_s},
                                        {u"name"_s, u"debian-13.iso"_s},
                                        {u"read_only"_s, true}};
            return Answered{200, R"({"ok": true, "note": null, "mounted": null})"};
        };
        Box box(media);
        box.open();
        QVERIFY(box.listed());
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !box.view.opened(); }, 10000));
        QCOMPARE(box.told, (QStringList{u"ok: Build server: Mount debian-13.iso done"_s, movedSaid()}));
        media.onChange = nullptr;
        box.open();
        QVERIFY(QTest::qWaitFor([&] { return box.place.looked == 3; }, 10000));
        QVERIFY(box.view.opened());
        QVERIFY(box.view.canUnmount());
        QCOMPARE(box.view.said(), QString());
        box.view.unmount();
        box.view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return media.changes.size() == 2 && !box.view.busy(); }, 10000));
        QCOMPARE(Media::listedBy(media.changes.first()), QString::fromLatin1(Media::firstMark));
        QCOMPARE(Media::listedBy(media.changes.last()), QString::fromLatin1(Media::otherMark));
        QCOMPARE(media.refusedAsMoved, 0);
        QVERIFY(box.view.opened());
        QCOMPARE(box.told.last(), u"ok: Build server: Unmount debian-13.iso done"_s);
    }

    void aServerBefore032ListsNoMarkAndIsAskedAsBefore()
    {
        // A server 0.3.0 or 0.3.1 says no `listed`: nothing of it is sent, nothing more is asked or said, and every
        // change goes and is told as before.
        Media media;
        media.listed.clear();
        media.mounted = QJsonObject{
            {u"drive"_s, u"cd"_s}, {u"image"_s, u"win11.iso"_s}, {u"name"_s, u"win11.iso"_s}, {u"read_only"_s, true}};
        media.onChange = [](const Asked& asked) {
            if (asked.path.contains("/media/upload")) {
                const QByteArray sha = QCryptographicHash::hash(asked.body, QCryptographicHash::Sha256).toHex();
                return Answered{200, "{\"ok\": true, \"image\": null, \"sha256\": \"" + sha + "\"}"};
            }
            return Answered{200, R"({"ok": true, "note": null, "mounted": null})"};
        };
        Box box(media);
        box.file(u"tools.img"_s, 300'000);
        box.open();
        QVERIFY(box.listed());
        QVERIFY(box.view.said().isEmpty());
        const auto carried = [&](qsizetype count) {
            return QTest::qWaitFor([&] { return media.changes.size() == count && !box.view.busy(); }, 10000);
        };
        box.view.pick(u"debian-13.iso"_s);
        box.view.mount();
        box.view.answer(true);
        QVERIFY(carried(1));
        box.view.unmount();
        box.view.answer(true);
        QVERIFY(carried(2));
        box.view.pick(u"debian-13.iso"_s);
        box.view.remove();
        box.view.answer(true);
        QVERIFY(carried(3));
        // (Its first list, and a look after each change.)
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == 4; }, 10000));
        box.view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !box.view.question().isEmpty(); }, 5000));
        box.view.answer(true);
        QVERIFY(carried(4));
        // Nothing is asked before the upload goes (its list said no mark): the look after it, and no other. (Asked as
        // the upload's answer is handed over, before the box holds still no longer.)
        QCOMPARE(box.place.looks, 5);
        QVERIFY(QTest::qWaitFor([&] { return media.listAsked == 5; }, 10000));
        QCOMPARE(bodyOf(media.changes[0]), QJsonObject({{u"image"_s, u"debian-13.iso"_s},
                                                        {u"machine"_s, u"Build server"_s},
                                                        {u"drive"_s, u"cd"_s},
                                                        {u"writable"_s, false},
                                                        {u"boot"_s, u"none"_s},
                                                        {u"restart"_s, false}}));
        QCOMPARE(bodyOf(media.changes[1]),
                 QJsonObject({{u"image"_s, u"win11.iso"_s}, {u"machine"_s, u"Build server"_s}}));
        QCOMPARE(bodyOf(media.changes[2]),
                 QJsonObject({{u"image"_s, u"debian-13.iso"_s}, {u"machine"_s, u"Build server"_s}}));
        QCOMPARE(media.changes[3].path,
                 QByteArray("/api/v1/devices/kvm/media/upload?name=tools.img&machine=Build%20server"));
        QCOMPARE(box.told, (QStringList{u"ok: Build server: Mount debian-13.iso done"_s,
                                        u"ok: Build server: Unmount win11.iso done"_s,
                                        u"ok: Build server: Delete debian-13.iso done"_s,
                                        u"ok: Build server: Upload tools.img done"_s}));
        QVERIFY(box.view.opened());
    }

    void aCountOfBytesIsWrittenAsThePageWritesIt()
    {
        QCOMPARE(ui::MediaView::bytesText(0), u"0 bytes"_s);
        QCOMPARE(ui::MediaView::bytesText(999), u"999 bytes"_s);
        QCOMPARE(ui::MediaView::bytesText(614'400), u"614 kB"_s);
        QCOMPARE(ui::MediaView::bytesText(2'359'293'952), u"2.4 GB"_s);
        QCOMPARE(ui::MediaView::bytesText(16'000'000'000), u"16 GB"_s);
    }

    void aKvmOfThisPcsOwnWallHasTheSameBoxAndItsWordsAreSaidAsTheyAre()
    {
        // GAPS A2: the box for a kvmd KVM of this PC's own wall, the wall its place (core::LocalWall, a MediaPlace),
        // which speaks to the KVM itself: what the box shows and does is the server's box's, and every word of the
        // KVM's driver is said as it is (no "No answer to": there is no server between that could say nothing).
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        core::KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.listedWithinMs = 600;
        pace.listedEveryMs = 100;
        pace.boards = std::make_shared<core::BoardHolds>();
        core::LocalWall wall(settings, secrets, pace);
        core::testing::MadeUpKvm kvm;
        kvm.hasDrive = true;
        kvm.msd.images = {{u"debian.iso"_s, QJsonObject{{u"size"_s, 700'000'000}, {u"complete"_s, true}}}};
        core::OwnKvm own;
        own.address = kvm.stand->address();
        own.name = u"NAS"_s;
        const QString id = wall.add(own, u"secret"_s).value();
        QVERIFY(QTest::qWaitFor(
            [&] {
                std::optional<QHash<QString, core::Status>> now;
                wall.askStatus([&now](const base::Result<QHash<QString, core::Status>>& came) {
                    now = came ? came.value() : QHash<QString, core::Status>();
                });
                return QTest::qWaitFor([&now] { return now.has_value(); }, 5000) && now->value(id).offersMedia;
            },
            20000));
        ui::MediaView::UnderWay underWay;
        ui::MediaView view;
        QStringList told;
        QObject::connect(&view, &ui::MediaView::told, &view, [&told](const QString& said, bool good) {
            told.append((good ? u"ok: "_s : u"no: "_s) + said);
        });
        view.open(&wall, id, u"NAS"_s, u"pc/"_s + id, &underWay, u"pc:"_s + id);
        QVERIFY(view.opened());
        QCOMPARE(view.place(), static_cast<core::MediaPlace*>(&wall));
        QVERIFY(QTest::qWaitFor([&] { return !view.images().isEmpty(); }, 10000));
        QCOMPARE(view.where(), u"On the KVM's own storage · 8.0 GB free of 16 GB"_s);
        QCOMPARE(view.now(), u"Nothing is mounted."_s);
        QVERIFY(view.canUpload());
        view.pick(u"debian.iso"_s);
        QVERIFY(view.canMount());
        QVERIFY(view.canDelete());
        view.mount();
        QCOMPARE(view.question().value(u"title"_s).toString(), u"Mount debian.iso"_s);
        view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !view.busy() && !told.isEmpty(); }, 10000));
        QCOMPARE(told.constLast(), u"ok: NAS: Mount debian.iso done"_s);
        QVERIFY(kvm.msd.connected);
        QVERIFY(QTest::qWaitFor([&] { return view.now().startsWith(u"debian.iso"_s); }, 10000));
        // The KVM did not answer in time: its driver's words, as they are.
        core::testing::Answered silent;
        silent.never = true;
        kvm.instead.insert("/api/msd/set_connected", silent);
        view.unmount();
        view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !view.busy() && view.saidBad(); }, 20000));
        QCOMPARE(view.said(), u"Device did not respond in time"_s);
        QVERIFY(!underWay.contains(u"pc:"_s + id));
        view.letGo();
    }

    void aListNotReadWholeIsNotSaidToBeAnEmptyStorage()
    {
        // Review cx4-2 L1: where the KVM's list of its images is in no shape read, or every entry of it was left out,
        // the box's empty list says that no image could be read from it, and not that its storage has none yet (beside
        // a note that says not every image may be shown).
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        core::KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.boards = std::make_shared<core::BoardHolds>();
        core::LocalWall wall(settings, secrets, pace);
        core::testing::MadeUpKvm kvm;
        kvm.hasDrive = true;
        core::OwnKvm own;
        own.address = kvm.stand->address();
        own.name = u"NAS"_s;
        const QString id = wall.add(own, u"secret"_s).value();
        QVERIFY(QTest::qWaitFor(
            [&] {
                std::optional<QHash<QString, core::Status>> now;
                wall.askStatus([&now](const base::Result<QHash<QString, core::Status>>& came) {
                    now = came ? came.value() : QHash<QString, core::Status>();
                });
                return QTest::qWaitFor([&now] { return now.has_value(); }, 5000) && now->value(id).offersMedia;
            },
            20000));
        const auto nothingSaid = [&] {
            ui::MediaView::UnderWay underWay;
            ui::MediaView view;
            view.open(&wall, id, u"NAS"_s, u"pc/"_s + id, &underWay, u"pc:"_s + id);
            const bool listed = QTest::qWaitFor(
                [&view] { return !view.nothing().isEmpty() && view.nothing() != u"Looking for images..."_s; }, 10000);
            const QString said = listed ? view.nothing() : u"(never listed)"_s;
            const bool none = view.images().isEmpty();
            view.letGo();
            return none ? said : u"(images listed)"_s;
        };
        const QString unread = u"No image could be read from its list: look at it on its own page."_s;
        // Empty, and read whole: none yet.
        const QString empty = nothingSaid();
        QVERIFY2(empty.startsWith(u"No images on the KVM's own storage yet."_s), qPrintable(empty));
        // A list in no shape read.
        kvm.msd.imagesSayInstead = QJsonValue(u"none"_s);
        QCOMPARE(nothingSaid(), unread);
        // Every entry of it left out.
        kvm.msd.imagesSayInstead.reset();
        kvm.msd.images = {{u"../climbs.iso"_s, QJsonObject{{u"size"_s, 1}, {u"complete"_s, true}}}};
        QCOMPARE(nothingSaid(), unread);
    }

    void aBoxOfThisPcsOwnKvmChangesNothingOnceItsAddressIsAnother()
    {
        // The wave-1 review, finding 7: the box listed the KVM at one address, and the KVM was then edited to another,
        // where another KVM has an image of the same name. Its deletion goes nowhere: neither there nor here; the box
        // says it is to be opened again, and offers nothing more.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        core::KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.listedWithinMs = 600;
        pace.listedEveryMs = 100;
        pace.boards = std::make_shared<core::BoardHolds>();
        core::LocalWall wall(settings, secrets, pace);
        core::testing::MadeUpKvm first;
        core::testing::MadeUpKvm other;
        for (core::testing::MadeUpKvm* kvm : {&first, &other}) {
            kvm->hasDrive = true;
            kvm->msd.images = {{u"debian.iso"_s, QJsonObject{{u"size"_s, 700'000'000}, {u"complete"_s, true}}}};
        }
        core::OwnKvm own;
        own.address = first.stand->address();
        own.name = u"NAS"_s;
        const QString id = wall.add(own, u"secret"_s).value();
        const auto offers = [&] {
            return QTest::qWaitFor(
                [&] {
                    std::optional<QHash<QString, core::Status>> now;
                    wall.askStatus([&now](const base::Result<QHash<QString, core::Status>>& came) {
                        now = came ? came.value() : QHash<QString, core::Status>();
                    });
                    return QTest::qWaitFor([&now] { return now.has_value(); }, 5000) && now->value(id).offersMedia;
                },
                20000);
        };
        QVERIFY(offers());
        ui::MediaView::UnderWay underWay;
        ui::MediaView view;
        view.open(&wall, id, u"NAS"_s, u"pc/"_s + id, &underWay, u"pc:"_s + id);
        QVERIFY(QTest::qWaitFor([&] { return !view.images().isEmpty(); }, 10000));
        view.pick(u"debian.iso"_s);
        QVERIFY(view.canDelete());
        // Edited to the other KVM's address, its name as it was: what the box would delete is the other's.
        core::OwnKvm moved = own;
        moved.address = other.stand->address();
        QVERIFY(wall.change(id, moved, u"secret"_s).ok());
        QVERIFY(offers());
        view.remove();
        QCOMPARE(view.question().value(u"title"_s).toString(), u"Delete debian.iso"_s);
        view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !view.busy() && view.saidBad(); }, 10000));
        QVERIFY2(view.said().startsWith(u"NAS is not the KVM this box was opened for any more"_s),
                 qPrintable(view.said()));
        QTest::qWait(300);
        QVERIFY(other.msd.images.contains("debian.iso"_L1));
        QVERIFY(first.msd.images.contains("debian.iso"_L1));
        QCOMPARE(other.asked("/api/msd/remove"), 0);
        // And nothing more is offered: it is to be opened again.
        QVERIFY(!view.canDelete());
        QVERIFY(!view.canMount());
        QVERIFY(!view.canUpload());
        // Opened again, it is of the KVM where it is now.
        view.open(&wall, id, u"NAS"_s, u"pc/"_s + id, &underWay, u"pc:"_s + id);
        QVERIFY(QTest::qWaitFor([&] { return !view.images().isEmpty(); }, 10000));
        view.pick(u"debian.iso"_s);
        QVERIFY(view.canDelete());
        view.letGo();
    }
};

QTEST_MAIN(TestMediaView)
#include "test_media_view.moc"
