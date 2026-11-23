#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/media_place.h>
#include <qkvm/core/read_ahead.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>

#include "far_away_file.h"
#include "heard_of_threads.h"
#include "jetkvm_by_hand.h"
#include "jetkvm_storage_by_hand.h"
#include "jetkvm_viewer_by_hand.h"
#include "memory_secrets.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
namespace core = qkvm::core;
using core::JetKvmDevice;
using core::MediaUploaded;
using core::ReadAhead;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::FarAwayFile;
using core::testing::JetKvmByHand;
using core::testing::JetKvmStorageByHand;
using core::testing::JetKvmViewerByHand;
using core::testing::StandIn;
using core::testing::Tls;

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
    pace.pacing = std::make_shared<core::SignInPacing>();
    pace.viewer.connection.watchEveryMs = 100;
    pace.viewer.unwatchedEveryMs = 100;
    pace.viewer.retryPauseMs = 300;
    pace.uploadQuietMs = 3000;
    return pace;
}

// What claims to be a file of this many bytes of the upload tests' pattern, holds none of them, and notes how far it
// was read (on the thread that reads it).
class Pattern final : public QIODevice {
public:
    explicit Pattern(qint64 size, std::shared_ptr<std::atomic<qint64>> readTo = {})
        : m_size(size), m_readTo(std::move(readTo))
    {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    qint64 size() const override { return m_size; }
    bool isSequential() const override { return false; }

protected:
    qint64 readData(char* data, qint64 most) override
    {
        const qint64 some = std::min(most, m_size - pos());
        if (some <= 0) {
            return some == 0 ? 0 : -1;
        }
        for (qint64 at = 0; at < some; ++at) {
            data[at] = core::testing::patternAt(pos() + at);
        }
        if (m_readTo) {
            *m_readTo = std::max(m_readTo->load(), pos() + some);
        }
        return some;
    }
    qint64 writeData(const char* /*data*/, qint64 /*length*/) override { return -1; }

private:
    qint64 m_size;
    std::shared_ptr<std::atomic<qint64>> m_readTo;
};

QByteArray patternOf(qint64 size)
{
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    for (qint64 at = 0; at < size; ++at) {
        bytes[static_cast<qsizetype>(at)] = core::testing::patternAt(at);
    }
    return bytes;
}

std::string codeOf(const base::Result<MediaUploaded>& done)
{
    return done ? std::string("(ok)") : done.error().code;
}

QString saidOf(const base::Result<MediaUploaded>& done)
{
    return done ? done.value().sha256 : QString::fromStdString(done.error().message);
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

Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

// A JetKVM by hand with its storage (one image on it), its viewer, and a driver of it, its card's connection up. Over
// HTTPS where it is given a certificate, and known by that one; at a name where it is given one (its port its own).
struct Rig {
    explicit Rig(const JetKvmDevice::Pace& pace = quick(), const std::optional<Tls>& over = std::nullopt,
                 const QString& named = QString())
        : jet(over), device(where(jet, over, named), u"secret"_s, pace)
    {
        viewer.storage = &images;
        jet.storage = &images;
        images.add(u"there.iso"_s, 700'000);
    }
    static JetKvmDevice::Where where(const JetKvmByHand& jet, const std::optional<Tls>& over, const QString& named)
    {
        JetKvmDevice::Where at;
        at.address = jet.address();
        if (!named.isEmpty()) {
            at.address.setHost(named);
        }
        if (over) {
            at.knownByCertificate = true;
            at.certificate = StandIn::certificateIn(over->certificateFile).digest(QCryptographicHash::Sha256);
        }
        return at;
    }
    bool connected()
    {
        QElapsedTimer waited;
        waited.start();
        while (!device.viewer().isOpen() && waited.elapsed() < 20'000) {
            device.askSnapshot(320, [](const base::Result<core::Snapshot>&) {});
            QTest::qWait(100);
        }
        QSignalSpy over(&device, &JetKvmDevice::looked);
        device.look();
        return over.wait(20'000) && device.mediaOffer().has_value();
    }
    // Uploads, and waits for the answer as the program's own loop waits.
    base::Result<MediaUploaded> upload(std::unique_ptr<QIODevice> image, const QString& name = u"new.iso"_s,
                                       const std::function<void()>& meanwhile = {})
    {
        std::optional<base::Result<MediaUploaded>> answer;
        QEventLoop waiting;
        device.uploadMedia(name, std::move(image), [&](const base::Result<MediaUploaded>& done) {
            answer = done;
            waiting.quit();
        });
        if (meanwhile) {
            meanwhile();
        }
        QTimer::singleShot(60000, &waiting, &QEventLoop::quit);
        if (!answer) {
            waiting.exec();
        }
        return answer.value_or(base::Error{"never", "The upload was never answered."});
    }
    int calledLike(const QString& beginning)
    {
        return static_cast<int>(
            std::ranges::count_if(images.called(), [&](const QString& one) { return one.startsWith(beginning); }));
    }
    QList<Asked> posts() const
    {
        QList<Asked> found;
        for (const Asked& one : jet.stand->asked) {
            if (one.method == "POST" && one.path.startsWith("/storage/upload")) {
                found.append(one);
            }
        }
        return found;
    }
    bool anyPartLeft()
    {
        const auto held = images.lock();
        return std::ranges::any_of(images.files, [](const auto& one) { return one.first.endsWith(u".incomplete"_s); });
    }

    JetKvmStorageByHand images;
    JetKvmByHand jet;
    JetKvmViewerByHand viewer{jet};
    JetKvmDevice device;
};

} // namespace

// An image carried from this PC to a JetKVM's own storage (GAPS A3; the web's 0.3.0, jetkvm.py media_upload, _carry,
// media_discard; test_virtual_media_on_a_jetkvm.py of web 0.3.1): opened over its connection, its bytes by POST
// /storage/upload on a line of their own after the JetKVM that answers was asked which it is under the same session;
// from the file as it is read, never whole in this program, hashed as it goes; one at a time; refused before a byte
// goes where the web refuses it; what an upload that ended otherwise left taken off. Against the JetKVM by hand. These
// wait for uploads: a program of their own.
class TestJetKvmUpload : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_folder{QCoreApplication::applicationDirPath() + u"/jetkvm-upload-test-XXXXXX"_s};

    QString fileOf(const QString& name, qint64 size)
    {
        const QString path = m_folder.filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(patternOf(size));
        }
        return path;
    }
    static std::unique_ptr<ReadAhead> opened(const QString& path, qint64 ahead = ReadAhead::aheadBytes)
    {
        std::optional<base::Result<std::unique_ptr<ReadAhead>>> made;
        QObject context;
        ReadAhead::open(
            ReadAhead::unchangedFileAt(path), &context,
            [&made](base::Result<std::unique_ptr<ReadAhead>> file) { made = std::move(file); }, ahead);
        if (!QTest::qWaitFor([&made] { return made.has_value(); }, 10000)) {
            return nullptr;
        }
        base::Result<std::unique_ptr<ReadAhead>> file = std::move(made).value_or(
            base::Result<std::unique_ptr<ReadAhead>>(base::Error{"never", "It was never opened."}));
        return file ? std::move(file).value() : nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(core::useWindowsOwnTls());
        QVERIFY2(core::testing::JetKvmPeerByHand::filmIsThere(), core::testing::JetKvmPeerByHand::noFilm);
        QVERIFY(m_folder.isValid());
    }

    void anImageIsCarriedAfterItIsAskedWhichItIs()
    {
        Rig rig;
        QVERIFY(rig.connected());
        const QString path = fileOf(u"a.iso"_s, 3'000'001);
        const base::Result<MediaUploaded> done = rig.upload(opened(path), u"debian 13+netinst.iso"_s);
        QVERIFY2(done, qPrintable(saidOf(done)));
        const QByteArray sha = QCryptographicHash::hash(patternOf(3'000'001), QCryptographicHash::Sha256).toHex();
        QCOMPARE(done.value().sha256, QString::fromLatin1(sha));
        {
            const auto held = rig.images.lock();
            QCOMPARE(rig.images.files.at(u"debian 13+netinst.iso"_s).sha256, sha);
            QVERIFY(!rig.images.files.contains(u"debian 13+netinst.iso.incomplete"_s));
            QCOMPARE(rig.images.uploads.size(), qsizetype{1});
            QVERIFY(rig.images.uploads.constLast().complete);
        }
        // Opened over its connection, with its name and length; then asked which JetKVM it is, with that upload open,
        // under the session its bytes then go with; the bytes on a line of their own, by the upload's id, the session
        // in the cookie alone.
        QCOMPARE(rig.calledLike(u"startStorageFileUpload {\"filename\":\"debian 13+netinst.iso\",\"size\":3000001}"_s),
                 1);
        QCOMPARE(rig.jet.deviceAskedWithUploadsOpen.constLast(), 1);
        const QList<Asked> posts = rig.posts();
        QCOMPARE(posts.size(), qsizetype{1});
        const Asked& post = posts.constFirst();
        QVERIFY(post.path.startsWith("/storage/upload?uploadId=upload_"));
        QCOMPARE(post.headers.value("content-length"), "3000001"_ba);
        QCOMPARE(post.headers.value("content-type"), "application/octet-stream"_ba);
        QVERIFY(post.headers.value("cookie").startsWith("authToken=jet."));
        const auto device =
            std::ranges::find_if(rig.jet.stand->asked, [](const Asked& one) { return one.path == "/device"; });
        QVERIFY(device != rig.jet.stand->asked.end());
        QCOMPARE(rig.jet.stand->asked.constLast().path, post.path);
        QCOMPARE(rig.jet.stand->asked.at(rig.jet.stand->asked.size() - 2).path, "/device"_ba);
        QCOMPARE(rig.jet.stand->asked.at(rig.jet.stand->asked.size() - 2).headers.value("cookie"),
                 post.headers.value("cookie"));
        QVERIFY(!rig.device.uploading());
        // Listed with its SHA-256 from here on, while it is that image; mounted, its drive says it too.
        const base::Result<core::MediaList> list =
            answerTo<core::MediaList>([&](auto then) { rig.device.askMedia(std::move(then)); });
        QVERIFY(list);
        const auto image = std::ranges::find_if(
            list.value().images, [](const core::MediaImage& one) { return one.id == "debian 13+netinst.iso"_L1; });
        QVERIFY(image != list.value().images.end());
        QCOMPARE(image->sha256, QString::fromLatin1(sha));
        const core::MediaMount mount{u"debian 13+netinst.iso"_s, u"jet"_s, u"cd"_s, false, u"none"_s, false, QString()};
        const base::Result<core::MediaDone> mounted =
            answerTo<core::MediaDone>([&](auto then) { rig.device.mountMedia(mount, std::move(then)); });
        QVERIFY(mounted);
        QCOMPARE(mounted.value().mounted.value_or(core::MountedMedia{}).sha256, QString::fromLatin1(sha));
        QVERIFY(answerTo<core::MediaDone>(
            [&](auto then) { rig.device.unmountMedia(u"debian 13+netinst.iso"_s, std::move(then)); }));
        // Deleted, it is forgotten: another of that name is not said to be it, though it be of that size and time.
        QString made;
        {
            const auto held = rig.images.lock();
            made = rig.images.files.at(u"debian 13+netinst.iso"_s).createdAt;
        }
        QVERIFY(answerTo<core::MediaDone>(
            [&](auto then) { rig.device.deleteMedia(u"debian 13+netinst.iso"_s, std::move(then)); }));
        rig.images.add(u"debian 13+netinst.iso"_s, 3'000'001);
        {
            const auto held = rig.images.lock();
            rig.images.files[u"debian 13+netinst.iso"_s].createdAt = made;
        }
        const base::Result<core::MediaList> again =
            answerTo<core::MediaList>([&](auto then) { rig.device.askMedia(std::move(then)); });
        QVERIFY(again);
        QVERIFY(std::ranges::all_of(again.value().images,
                                    [](const core::MediaImage& one) { return one.sha256.isEmpty(); }));
        QVERIFY(rig.images.neverAsked().isEmpty());
    }

    // Done only where every byte went and the hash is of every byte read: one the JetKVM answered "Upload completed" to
    // before that is not this image, whatever it lists afterwards (the web asserts its hash is there). (Qt reads no
    // answer before it has written the whole body, so a JetKVM by hand cannot answer it sooner: this is the rule
    // itself.)
    void onlyWhatWentWholeIsDone()
    {
        const QString sha = QString::fromLatin1(QCryptographicHash::hash("x", QCryptographicHash::Sha256).toHex());
        QVERIFY(core::jetkvm::wentWhole(1000, 1000, sha));
        QVERIFY(!core::jetkvm::wentWhole(999, 1000, sha));        // not every byte written
        QVERIFY(!core::jetkvm::wentWhole(0, 1000, sha));          // nothing said of it at all
        QVERIFY(!core::jetkvm::wentWhole(1000, 1000, QString())); // not every byte read: no hash yet
        QVERIFY(!core::jetkvm::wentWhole(1000, 1000, sha.left(63)));
        QVERIFY(!core::jetkvm::wentWhole(0, 0, sha));
    }

    void itIsReadAsItGoesAndNeverWholeFirst()
    {
        // Forty-eight megabytes, of which no more than the file's own read-ahead and a few pieces were read when the
        // JetKVM had its first megabyte.
        Rig rig;
        QVERIFY(rig.connected());
        const auto readTo = std::make_shared<std::atomic<qint64>>(0);
        qint64 readAtTheFirst = -1;
        rig.jet.stand->hearing = [&](qint64 soFar) {
            if (readAtTheFirst < 0 && soFar > qint64{1} << 20) {
                readAtTheFirst = readTo->load();
            }
        };
        constexpr qint64 size = qint64{48} << 20;
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(size, readTo), u"large.img"_s);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(readAtTheFirst > 0);
        QVERIFY2(readAtTheFirst < qint64{12} << 20, QByteArray::number(readAtTheFirst).constData());
        QCOMPARE(rig.posts().constLast().body.size(), size);
    }

    void howFarItHasComeIsSaidAndOneGoesAtATime()
    {
        Rig rig;
        QVERIFY(rig.connected());
        constexpr qint64 size = 6'000'000;
        qint64 mostSeen = 0;
        std::optional<base::Result<MediaUploaded>> second;
        QTimer watch;
        QObject::connect(&watch, &QTimer::timeout, &watch, [&] {
            if (const std::optional<core::MediaUploading> now = rig.device.uploading()) {
                QCOMPARE(now->name, u"far.iso"_s);
                QCOMPARE(now->size, size);
                QCOMPARE(now->by, u"this PC"_s);
                mostSeen = std::max(mostSeen, now->sent);
            }
        });
        const base::Result<MediaUploaded> done =
            rig.upload(ReadAhead::of(std::make_unique<FarAwayFile>(size, 30)), u"far.iso"_s, [&] {
                watch.start(20);
                rig.device.uploadMedia(u"other.iso"_s, std::make_unique<Pattern>(1000),
                                       [&second](base::Result<MediaUploaded> came) { second = std::move(came); });
            });
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(mostSeen > 0);
        QVERIFY(mostSeen <= size);
        QVERIFY(QTest::qWaitFor([&second] { return second.has_value(); }, 5000));
        QCOMPARE(codeOf(*second), std::string("busy"));
        QVERIFY(saidOf(*second).contains("far.iso is being uploaded"_L1));
        QCOMPARE(rig.posts().size(), qsizetype{1});
        QVERIFY(!rig.device.uploading());
    }

    void anUploadIsRefusedBeforeAByteGoesWhereTheWebRefusesIt_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<qint64>("size");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("said");
        QTest::newRow("empty") << u"a.iso"_s << qint64{0} << u"invalid"_s << u"An empty file is no image."_s;
        QTest::newRow("larger than may be") << u"a.iso"_s << qint64{16'000'000'001} << u"too_large"_s
                                            << u"An image carried through qKVM may be 16 GB at the most."_s;
        QTest::newRow("a dot first") << u".a.iso"_s << qint64{10} << u"invalid"_s
                                     << QString::fromLatin1(core::imageNameRule);
        QTest::newRow("a slash") << u"isos/a.iso"_s << qint64{10} << u"invalid"_s
                                 << QString::fromLatin1(core::imageNameRule);
        QTest::newRow("a part's name") << u"a.iso.incomplete"_s << qint64{10} << u"invalid"_s
                                       << QString::fromLatin1(core::imageNameRule);
        QTest::newRow("there already")
            << u"there.iso"_s << qint64{10} << u"duplicate"_s
            << u"there.iso is on the JetKVM's own storage already: delete it first, or upload it under another name."_s;
        QTest::newRow("a part of an earlier one")
            << u"half.iso"_s << qint64{10} << u"duplicate"_s
            << u"A part of an earlier upload of half.iso is on the JetKVM: delete it in the list first, or upload it "
               u"under another name."_s;
        QTest::newRow("no room") << u"a.iso"_s << qint64{7'000'000'000} << u"no_space"_s
                                 << u"The JetKVM's own storage has 6,999,299,995 bytes free, and the image is "
                                    u"7,000,000,000: delete an image there first."_s;
    }
    void anUploadIsRefusedBeforeAByteGoesWhereTheWebRefusesIt()
    {
        QFETCH(QString, name);
        QFETCH(qint64, size);
        QFETCH(QString, code);
        QFETCH(QString, said);
        Rig rig;
        rig.images.add(u"half.iso.incomplete"_s, 5);
        QVERIFY(rig.connected());
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(size), name);
        QCOMPARE(codeOf(done), code.toStdString());
        QCOMPARE(saidOf(done), said);
        QCOMPARE(rig.calledLike(u"startStorageFileUpload"_s), 0);
        QVERIFY(rig.posts().isEmpty());
        QVERIFY(!rig.device.uploading());
    }

    // Its name is found at one address for the asking of which JetKVM it is, and would be at another a moment later
    // (two JetKVMs that answer to one name, as two out of their boxes at `jetkvm.local`): the asking and the bytes
    // after it both go to the one address it was found at, its name and port in their Host header, the session with
    // them; nothing goes to the other address. (Stricter than the web, whose bytes' connection finds the name anew.)
    void theAskingAndTheBytesGoToTheOneAddressItsNameWasFoundAt_data()
    {
        QTest::addColumn<bool>("https");
        QTest::newRow("plain HTTP") << false;
        QTest::newRow("HTTPS, known by its certificate") << true;
    }
    void theAskingAndTheBytesGoToTheOneAddressItsNameWasFoundAt()
    {
        QFETCH(bool, https);
        // The JetKVM it was added for at its name, "localhost" (127.0.0.1, as Windows finds it for every other line of
        // its driver), and at 127.0.0.3; another JetKVM at 127.0.0.2, at the same port. Its name found at 127.0.0.3
        // the first time, and at 127.0.0.2 every time after.
        const QHostAddress its(u"127.0.0.3"_s);
        const QHostAddress another(u"127.0.0.2"_s);
        auto names = std::make_shared<QStringList>();
        JetKvmDevice::Pace pace = quick();
        pace.connectWithinMs = 5000; // (a TLS handshake under the address sanitizer on a busy PC)
        if (https) {
            // (And as long to be quiet, its own lines and the upload's: Qt's quiet runs from the asking on, through the
            // handshake, as test_kvm_editing's quickProbe says.)
            pace.quietMs = pace.connectWithinMs;
            pace.uploadQuietMs = pace.connectWithinMs;
        }
        pace.lookUp = [names, its, another](const QString& name, QObject* context,
                                            std::function<void(QList<QHostAddress>)> found) {
            const QHostAddress at = names->isEmpty() ? its : another;
            names->append(name);
            QTimer::singleShot(0, context, [found = std::move(found), at] { found({at}); });
        };
        Rig rig(pace, https ? std::optional<Tls>(tls("first")) : std::nullopt, u"localhost"_s);
        QVERIFY(rig.jet.stand->alsoAt(its));
        const quint16 port = rig.jet.stand->serverPort();
        const auto answers = [](const Asked& asked) {
            return asked.path == "/device" ? Answered(200, R"({"deviceId":"another-jetkvm-0001"})")
                                           : Answered(404, R"({"error":"Upload not found"})");
        };
        const std::unique_ptr<StandIn> otherOne =
            https ? std::make_unique<StandIn>(answers, tls("first")) : std::make_unique<StandIn>(answers);
        StandIn& other = *otherOne;
        other.takeNoMore();
        QVERIFY(other.alsoAt(another, port));
        QVERIFY(rig.connected());
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(200'000), u"named.iso"_s);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(*names, QStringList{u"localhost"_s});
        const QByteArray named = "localhost:" + QByteArray::number(port);
        const QList<Asked> posts = rig.posts();
        QCOMPARE(posts.size(), qsizetype{1});
        const Asked& post = posts.constFirst();
        QCOMPARE(post.at, its);
        QCOMPARE(post.headers.value("host"), named);
        QCOMPARE(post.body.size(), qsizetype{200'000});
        // Asked which it is right before, at that very address, under the session the bytes went with.
        const QList<Asked>& asked = rig.jet.stand->asked;
        QCOMPARE(asked.constLast().path, post.path);
        const Asked& asking = asked.at(asked.size() - 2);
        QCOMPARE(asking.path, "/device"_ba);
        QCOMPARE(asking.at, its);
        QCOMPARE(asking.headers.value("host"), named);
        QCOMPARE(asking.headers.value("cookie"), post.headers.value("cookie"));
        QVERIFY(post.headers.value("cookie").startsWith("authToken=jet."));
        // Nothing went to the other, and the rest of its driver's lines went by its name, as ever.
        QVERIFY(other.asked.isEmpty());
        QCOMPARE(other.lines, 0);
        QVERIFY(std::ranges::all_of(asked.first(asked.size() - 2),
                                    [](const Asked& one) { return one.at == QHostAddress(QHostAddress::LocalHost); }));
        QVERIFY(rig.images.has(u"named.iso"_s));
    }

    // (kept_with.h, as test_core_server_client_upload has it of an upload to a server.) What an upload holds while the
    // name of its JetKVM's address is being found (the upload, its file, whoever is to hear its end) ends on this
    // thread where the driver ends first: Qt hears nothing of a lookup whose context has ended, and lets go of what was
    // to hear its answer on its own lookup thread then. Played by the lookup of the pace, which keeps what it is handed
    // until the driver has ended, and lets go of it on another thread.
    void whatAnUploadHoldsWhileItsNameIsFoundEndsOnThisThreadWhereTheDriverEndsFirst()
    {
        struct Lookup {
            bool asked = false;
            QPointer<QObject> context;
            std::function<void(QList<QHostAddress>)> found;
        };
        const auto lookup = std::make_shared<Lookup>();
        JetKvmDevice::Pace pace = quick();
        pace.lookUp = [lookup](const QString& /*name*/, QObject* context,
                               std::function<void(QList<QHostAddress>)> found) {
            lookup->asked = true;
            lookup->context = context;
            lookup->found = std::move(found);
        };
        auto rig = std::make_unique<Rig>(pace, std::nullopt, u"localhost"_s);
        QVERIFY(rig->connected());
        std::atomic<QThread*> fileEndedOn{nullptr};
        std::atomic<QThread*> hearerEndedOn{nullptr};
        {
            std::unique_ptr<ReadAhead> image = ReadAhead::of(std::make_unique<Pattern>(200'000));
            QVERIFY(image != nullptr);
            QObject::connect(
                image.get(), &QObject::destroyed, image.get(),
                [&fileEndedOn] { fileEndedOn = QThread::currentThread(); }, Qt::DirectConnection);
            // (Whoever is to hear its end holds this, which says where it ended.)
            const std::shared_ptr<void> hearer(nullptr,
                                               [&hearerEndedOn](void*) { hearerEndedOn = QThread::currentThread(); });
            static_cast<void>(rig->device.uploadMedia(u"named.iso"_s, std::move(image),
                                                      [hearer](const base::Result<MediaUploaded>&) {}));
        }
        QVERIFY(QTest::qWaitFor([&lookup] { return lookup->asked; }, 20'000));
        QVERIFY(fileEndedOn.load() == nullptr);
        const core::testing::HeardOfThreads heard;
        rig.reset();
        // Its context gone with the driver: Qt would hand the answer to nobody, and let go of what was to hear it.
        QVERIFY(lookup->context == nullptr);
        std::thread elsewhere([lookup] { lookup->found = nullptr; });
        elsewhere.join();
        QVERIFY2(fileEndedOn.load() == QThread::currentThread(), "The file ended on another thread.");
        QVERIFY2(hearerEndedOn.load() == QThread::currentThread(),
                 "Whoever was to hear the upload's end ended on another thread.");
        const QStringList said = core::testing::HeardOfThreads::said();
        QVERIFY2(said.isEmpty(), qPrintable(said.join(u" | "_s)));
    }

    // J5: another JetKVM answers at its address by now (as after a new lease), its connection held to the one it was
    // added for: none of the image goes to it, and what was opened for it is taken off again.
    void anotherJetKvmAtTheAddressIsSentNoneOfTheImage()
    {
        Rig rig;
        QVERIFY(rig.connected());
        rig.jet.deviceId = u"0a0b0c0d0e0f1011"_s;
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(1000));
        QCOMPARE(codeOf(done), std::string("another_machine"));
        QVERIFY2(saidOf(done).startsWith("The upload did not get through to the JetKVM (Another JetKVM answers at this "
                                         "address"_L1),
                 qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).endsWith("Nothing of it is left on the JetKVM."_L1));
        QVERIFY(rig.posts().isEmpty());
        QVERIFY(!rig.anyPartLeft());
    }

    // Somebody signed in to it meanwhile (its own page): its session is not taken, and nothing goes.
    void aSessionItNoLongerTakesIsSaid()
    {
        Rig rig;
        QVERIFY(rig.connected());
        rig.jet.signInElsewhere();
        base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(1000));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        // (And that trying again signs whoever signed in at its own page out there: it has one session for everybody.
        // Review cx4-1 L6.)
        QCOMPARE(saidOf(done), u"The upload did not get through to the JetKVM (The JetKVM did not take qKVM's session "
                               u"for the upload (somebody signed in to it meanwhile): trying again signs qKVM in to it "
                               u"anew, which signs out whoever signed in at its own page.). Nothing of it is left on "
                               u"the JetKVM."_s);
        QVERIFY(rig.posts().isEmpty());
        QVERIFY(!rig.anyPartLeft());
        // Tried again, as it says, while its connection stays up: under a session given anew, and it goes. (Outside
        // review 4.8: the session refused was kept, and every try was refused until the connection was made anew.)
        QVERIFY(rig.device.viewer().isOpen());
        const int signIns = rig.jet.signIns;
        const base::Result<MediaUploaded> again = rig.upload(std::make_unique<Pattern>(1000));
        QVERIFY2(again, qPrintable(saidOf(again)));
        QCOMPARE(rig.jet.signIns, signIns + 1);
        QCOMPARE(rig.posts().size(), qsizetype{1});
        // The same where its bytes were refused so: the next upload signs in anew.
        rig.images.uploadInstead = Answered(401, R"({"error":"Unauthorized"})");
        done = rig.upload(std::make_unique<Pattern>(1000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        rig.images.uploadInstead.reset();
        const base::Result<MediaUploaded> third = rig.upload(std::make_unique<Pattern>(1000), u"y.iso"_s);
        QVERIFY2(third, qPrintable(saidOf(third)));
        QCOMPARE(rig.jet.signIns, signIns + 2);
    }

    void whatItDidNotTakeWholeIsTakenOffAndSaid_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<qint64>("takesOnly");
        QTest::addColumn<bool>("stays");
        QTest::addColumn<QString>("said");
        QTest::newRow("a body that ended early, answered done")
            << 200 << qint64{400} << false
            << u"The JetKVM does not list the image whole after its upload. Nothing of it is left on the JetKVM."_s;
        QTest::newRow("its session refused")
            << 401 << qint64{-1} << false
            << u"The JetKVM did not take qKVM's session for the upload (somebody signed in to it meanwhile): trying "
               u"again signs qKVM in to it anew, which signs out whoever signed in at its own page. Nothing of it is "
               u"left on the JetKVM."_s;
        QTest::newRow("an upload it does not know")
            << 404 << qint64{-1} << false
            << u"The JetKVM did not take the image (HTTP 404). Nothing of it is left on the JetKVM."_s;
        QTest::newRow("a part that stays")
            << 500 << qint64{-1} << true
            << u"The JetKVM did not take the image (HTTP 500). What was written of it is on the JetKVM still, as "
               u"x.iso.incomplete: delete it there."_s;
    }
    void whatItDidNotTakeWholeIsTakenOffAndSaid()
    {
        QFETCH(int, status);
        QFETCH(qint64, takesOnly);
        QFETCH(bool, stays);
        QFETCH(QString, said);
        Rig rig;
        QVERIFY(rig.connected());
        if (status != 200) {
            rig.images.uploadInstead = Answered(status, R"({"error":"no"})");
        }
        rig.images.takesOnly = takesOnly;
        rig.images.deletesNothing = stays;
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(1000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), said);
        QCOMPARE(rig.anyPartLeft(), stays);
        QVERIFY(!rig.images.has(u"x.iso"_s));
        QVERIFY(!rig.device.uploading());
    }

    // A list of its storage that is no list, or not read whole, never says that nothing of an image is there: an
    // upload is refused before anything is opened where its name is not found in one; and what an upload that did not
    // get through left is not said gone where the list after it was not read whole. (Outside review 4.6: either was
    // taken for "not there", and "Nothing of it is left" said beside its `.incomplete`.)
    void aListNotReadWholeNeverSaysNothingIsThere()
    {
        Rig rig;
        QVERIFY(rig.connected());
        {
            const auto held = rig.images.lock();
            rig.images.filesSayInstead = QJsonObject{{u"x.iso"_s, 1}};
        }
        base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(1000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QVERIFY2(saidOf(done).endsWith(u"so whether x.iso, or a part of an earlier upload of it, is there already is "
                                       u"not known: look at it on its own page."_s),
                 qPrintable(saidOf(done)));
        QCOMPARE(rig.calledLike(u"startStorageFileUpload"_s), 0);
        // Listed whole before, refused by the JetKVM, and its list after that not read whole: what it left is said
        // to be there still, maybe.
        {
            const auto held = rig.images.lock();
            rig.images.filesSayInsteadAfter = rig.images.listsSaid + 1;
        }
        rig.images.uploadInstead = Answered(404, R"({"error":"no"})");
        done = rig.upload(std::make_unique<Pattern>(1000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), u"The JetKVM did not take the image (HTTP 404). What was written of it may be on the "
                               u"JetKVM still, as x.iso.incomplete (its list of its own storage could not be read "
                               u"whole): delete it there."_s);
        QVERIFY(rig.images.has(u"x.iso.incomplete"_s));
        QCOMPARE(rig.calledLike(u"deleteStorageFile"_s), 0);
        // Taken whole, and its list after it not read whole (though it lists the image whole): not done, and nothing
        // taken off, as it is not known whether a part of it is left beside it.
        rig.images.uploadInstead.reset();
        {
            const auto held = rig.images.lock();
            rig.images.filesSayInstead =
                QJsonArray{QJsonObject{{u"filename"_s, u"z.iso"_s}, {u"size"_s, 1000}, {u"createdAt"_s, u"now"_s}},
                           QJsonObject{{u"size"_s, 1}}};
            rig.images.filesSayInsteadAfter = rig.images.listsSaid + 1;
        }
        done = rig.upload(std::make_unique<Pattern>(1000), u"z.iso"_s);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QVERIFY2(saidOf(done).endsWith(u"so whether z.iso is there whole after its upload is not known: look at it on "
                                       u"its own page."_s),
                 qPrintable(saidOf(done)));
        QVERIFY(rig.images.has(u"z.iso"_s));
        QCOMPARE(rig.calledLike(u"deleteStorageFile"_s), 0);
    }

    // Every byte went, and the JetKVM keeps fewer of them under the image's own name (one that is none: a real one
    // keeps a body that ended early as `.incomplete`): held to the size it lists, it is not done, and what it lists
    // there is said, and is not called nothing. (Outside review 4.10: no case had the listed size checked.)
    void anImageListedAtAnotherSizeAfterItsUploadIsNotDone()
    {
        Rig rig;
        QVERIFY(rig.connected());
        rig.images.takesOnly = 400;
        rig.images.keepsWhatCame = true;
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(1000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), u"The JetKVM does not list the image whole after its upload. It lists x.iso at 400 "
                               u"bytes, not 1,000: delete it there."_s);
        QVERIFY(rig.images.has(u"x.iso"_s));
        QVERIFY(!rig.anyPartLeft());
        QVERIFY(!rig.device.uploading());
        // (Every byte of it went: past the rule that it went whole, to the listing.)
        QCOMPARE(rig.posts().size(), qsizetype{1});
        QCOMPARE(rig.posts().constFirst().body.size(), qsizetype{1000});
    }

    void aStartItDidNotOpenAsAskedIsSaid()
    {
        Rig rig;
        QVERIFY(rig.connected());
        // Its own id of another shape: what it opened anew is taken off.
        rig.images.startSaysInstead = QJsonObject{{u"alreadyUploadedBytes"_s, 0}, {u"dataChannel"_s, u"../x"_s}};
        base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(1000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QCOMPARE(saidOf(done), u"The JetKVM did not open the upload as it was asked."_s);
        QVERIFY(!rig.anyPartLeft());
        // Added to what was there, it says: that stays as it is, and nothing goes.
        rig.images.startSaysInstead = QJsonObject{{u"alreadyUploadedBytes"_s, 5},
                                                  {u"dataChannel"_s, u"upload_0a1b2c3d-0a1b-4c3d-8e9f-0a1b2c3d4e5f"_s}};
        done = rig.upload(std::make_unique<Pattern>(1000), u"z.iso"_s);
        QCOMPARE(codeOf(done), std::string("bad_response"));
        QVERIFY(rig.images.has(u"z.iso.incomplete"_s));
        QVERIFY(rig.posts().isEmpty());
    }

    // A start that got no answer in time may have opened the upload all the same (its `<name>.incomplete`): what it
    // opened is taken off, and the next upload of that name is not refused for a part of an earlier one.
    void aStartThatGotNoAnswerInTimeLeavesNothing()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.callWithinMs = 2000;
        Rig rig(pace);
        QVERIFY(rig.connected());
        rig.viewer.slowOn("startStorageFileUpload", 5000);
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(1000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("timeout"));
        QCOMPARE(saidOf(done), u"The JetKVM did not answer in time"_s);
        QVERIFY(!rig.anyPartLeft());
        QVERIFY(rig.posts().isEmpty());
        rig.viewer.slowOn({}, 0);
        const base::Result<MediaUploaded> again = rig.upload(std::make_unique<Pattern>(1000), u"x.iso"_s);
        QVERIFY2(again, qPrintable(saidOf(again)));
        QVERIFY(rig.images.has(u"x.iso"_s));
    }

    void aStoppedUploadLeavesNothingOnTheJetKvm()
    {
        Rig rig;
        QVERIFY(rig.connected());
        std::function<void()> stop;
        std::optional<base::Result<MediaUploaded>> answer;
        stop = rig.device.uploadMedia(u"far.iso"_s, ReadAhead::of(std::make_unique<FarAwayFile>(20'000'000, 30)),
                                      [&answer](base::Result<MediaUploaded> came) { answer = std::move(came); });
        QVERIFY(
            QTest::qWaitFor([&] { return rig.device.uploading().value_or(core::MediaUploading{}).sent > 0; }, 10000));
        QVERIFY(rig.images.has(u"far.iso.incomplete"_s));
        stop();
        stop(); // (twice is once)
        QVERIFY(QTest::qWaitFor([&answer] { return answer.has_value(); }, 10000));
        QCOMPARE(codeOf(*answer), std::string(core::MediaPlace::uploadStopped));
        QCOMPARE(saidOf(*answer),
                 u"The upload was stopped before all of it had gone. Nothing of it is left on the JetKVM."_s);
        QVERIFY(!rig.anyPartLeft());
        QVERIFY(!rig.device.uploading());
        // Stopped before it went: nothing went, nothing to take off.
        answer.reset();
        const int started = rig.calledLike(u"startStorageFileUpload"_s);
        stop = rig.device.uploadMedia(u"soon.iso"_s, std::make_unique<Pattern>(1000),
                                      [&answer](base::Result<MediaUploaded> came) { answer = std::move(came); });
        stop();
        QVERIFY(QTest::qWaitFor([&answer] { return answer.has_value(); }, 10000));
        QCOMPARE(codeOf(*answer), std::string(core::MediaPlace::uploadStopped));
        QCOMPARE(saidOf(*answer), u"The upload was stopped before any of it went."_s);
        QCOMPARE(rig.calledLike(u"startStorageFileUpload"_s), started);
        QVERIFY(!rig.anyPartLeft());
    }

    void aFileThatChangesWhileItIsReadIsRefused()
    {
        Rig rig;
        QVERIFY(rig.connected());
        const QString path = fileOf(u"grows.iso"_s, 8'000'000);
        std::unique_ptr<ReadAhead> file = opened(path, ReadAhead::pieceBytes);
        QVERIFY(file != nullptr);
        const base::Result<MediaUploaded> done = rig.upload(std::move(file), u"changes.iso"_s, [&] {
            QFile changed(path);
            QVERIFY(changed.open(QIODevice::Append));
            changed.write("more");
        });
        QCOMPARE(codeOf(done), std::string("disconnected"));
        QVERIFY2(saidOf(done).contains("It was changed while it was read"_L1), qPrintable(saidOf(done)));
        QVERIFY2(saidOf(done).endsWith("Nothing of it is left on the JetKVM."_L1), qPrintable(saidOf(done)));
        QVERIFY(!rig.images.has(u"changes.iso"_s));
        QVERIFY(!rig.anyPartLeft());
    }

    void aLineThatFallsUnderItIsSaidAndWhatWasLeftTakenOff()
    {
        Rig rig;
        QVERIFY(rig.connected());
        Answered gone;
        gone.hangUp = true;
        rig.images.uploadInstead = gone;
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(5000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("dropped"));
        QVERIFY2(saidOf(done).startsWith("The upload did not get through to the JetKVM ("_L1),
                 qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).endsWith("Nothing of it is left on the JetKVM."_L1));
        QVERIFY(!rig.anyPartLeft());
    }

    // Its connection, which the listing after the bytes goes over, is kept while they go: not given back for nobody
    // watching (UNWATCHED), as a key that goes keeps it. (From the first byte the JetKVM had to its last: what it takes
    // to answer after that, the JetKVM by hand hashing 36 MB on a busy box, is no byte going.)
    void itsConnectionIsKeptWhileTheBytesGo()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.unwatchedMs = 1000;
        Rig rig(pace);
        QVERIFY(rig.connected());
        QSignalSpy over(&rig.device.viewer(), &core::JetKvmViewer::connectionOver);
        constexpr qint64 size = 36'000'000;
        qsizetype overAtTheFirstByte = -1;
        qsizetype overAtTheLastByte = -1;
        QElapsedTimer went;
        qint64 wentForMs = -1;
        rig.jet.stand->hearing = [&](qint64 soFar) {
            if (overAtTheFirstByte < 0 && soFar > 0) {
                overAtTheFirstByte = over.count();
                went.start();
            }
            if (overAtTheLastByte < 0 && soFar >= size) {
                overAtTheLastByte = over.count();
                wentForMs = went.elapsed();
            }
        };
        const base::Result<MediaUploaded> done =
            rig.upload(ReadAhead::of(std::make_unique<FarAwayFile>(size, 30)), u"far.iso"_s);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(overAtTheFirstByte >= 0);
        QVERIFY(overAtTheLastByte >= 0);
        // (Its bytes went for longer than a connection is kept for nobody, three times: else this says nothing.)
        QVERIFY2(wentForMs > qint64{3} * pace.viewer.unwatchedMs, QByteArray::number(wentForMs).constData());
        QCOMPARE(overAtTheLastByte, overAtTheFirstByte);
    }

    // Its answer to the bytes is followed nowhere: one that sends them on to another address (a 307, which keeps the
    // way they were asked and their body) has them refused, and nothing goes there.
    void anAnswerThatSendsTheBytesElsewhereIsFollowedNowhere()
    {
        Rig rig;
        QVERIFY(rig.connected());
        StandIn elsewhere([](const Asked&) { return Answered(200, R"({"message":"Upload completed"})"); });
        rig.images.uploadInstead = Answered(
            307, {}, "text/plain", {"Location: " + elsewhere.address().toLatin1() + "/storage/upload?uploadId=x"});
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(5000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), u"The JetKVM did not take the image (HTTP 307). Nothing of it is left on the JetKVM."_s);
        QVERIFY(elsewhere.asked.isEmpty());
        QVERIFY(!rig.anyPartLeft());
    }

    // The upload's id is the JetKVM's, and is never told: not where the JetKVM refuses the bytes in words that name it.
    void theUploadsIdIsNeverTold()
    {
        Rig rig;
        QVERIFY(rig.connected());
        rig.images.namesTheIdInItsRefusal = true;
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(5000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), u"The JetKVM did not take the image (HTTP 500). Nothing of it is left on the JetKVM."_s);
        const QList<Asked> posts = rig.posts();
        QCOMPARE(posts.size(), qsizetype{1});
        const QString id = QString::fromLatin1(posts.constFirst().path).section(u'=', 1);
        QVERIFY(core::jetkvm::isUploadId(id));
        QVERIFY(!saidOf(done).contains(id));
        QVERIFY(!saidOf(done).contains(u"upload_"_s));
    }

    // Over HTTPS the line the bytes go on is held to the certificate the JetKVM is known by, as every line of its
    // driver is (one Windows does not trust for its name: only that one is let in).
    void overHttpsTheBytesGoOnALineHeldToItsCertificate()
    {
        JetKvmDevice::Pace pace = quick();
        pace.connectWithinMs = 5000; // (a TLS handshake under the address sanitizer on a busy PC)
        // (And as long to be quiet, its own lines and the upload's: Qt's quiet runs from the asking on, through the
        // handshake, as test_kvm_editing's quickProbe says.)
        pace.quietMs = pace.connectWithinMs;
        pace.uploadQuietMs = pace.connectWithinMs;
        Rig rig(pace, tls("first"));
        QVERIFY(rig.connected());
        const base::Result<MediaUploaded> done = rig.upload(std::make_unique<Pattern>(300'000), u"secure.iso"_s);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(rig.posts().size(), qsizetype{1});
        QCOMPARE(rig.posts().constFirst().body.size(), qsizetype{300'000});
        QVERIFY(rig.images.has(u"secure.iso"_s));
    }

    // Through this PC's own wall: the part of an upload under way is not deleted meanwhile, under either of its names
    // (the web's final review of 0.3.0); one upload at a time; the JetKVM neither changed nor taken off while it goes.
    void thePartOfAnUploadUnderWayIsNotDeletedMeanwhile()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        core::DriverPaces paces;
        paces.jetkvm = quick();
        core::LocalWall wall(settings, secrets, paces);
        JetKvmStorageByHand images;
        JetKvmByHand jet;
        JetKvmViewerByHand viewer{jet};
        viewer.storage = &images;
        jet.storage = &images;
        core::OwnKvm own;
        own.address = jet.address().toString();
        own.name = u"jet"_s;
        own.kind = u"jetkvm"_s;
        const QString id = wall.add(own, u"secret"_s).value();
        using Statuses = QHash<QString, core::Status>;
        const auto offers = [&] {
            wall.askSnapshot(id, 320, 0, [](const base::Result<core::Snapshot>&) {});
            const base::Result<Statuses> all = answerTo<Statuses>([&](auto done) { wall.askStatus(std::move(done)); });
            return all && all.value().value(id).offersMedia;
        };
        QTRY_VERIFY_WITH_TIMEOUT(offers(), 30'000);
        core::MediaPlace& place = wall;
        std::optional<base::Result<MediaUploaded>> carried;
        const std::function<void()> stop = place.uploadMedia(
            id, u"slow.iso"_s, u"jet"_s, QString(), ReadAhead::of(std::make_unique<FarAwayFile>(20'000'000, 30)),
            [&carried](base::Result<MediaUploaded> came) { carried = std::move(came); });
        QTRY_VERIFY_WITH_TIMEOUT(images.has(u"slow.iso.incomplete"_s), 20'000);
        for (const QString& image : {u"slow.iso.incomplete"_s, u"slow.iso"_s}) {
            const base::Result<core::MediaDone> refused = answerTo<core::MediaDone>(
                [&](auto then) { place.deleteMedia(id, image, u"jet"_s, QString(), std::move(then)); });
            QVERIFY(!refused);
            QCOMPARE(refused.error().code, std::string("busy"));
            QCOMPARE(QString::fromStdString(refused.error().message),
                     image + u" is being uploaded to jet at this moment. Wait for it to finish."_s);
        }
        QVERIFY(images.has(u"slow.iso.incomplete"_s));
        const base::Result<MediaUploaded> another = answerTo<MediaUploaded>([&](auto then) {
            static_cast<void>(place.uploadMedia(id, u"y.iso"_s, u"jet"_s, QString(),
                                                ReadAhead::of(std::make_unique<FarAwayFile>(1000, 0)),
                                                std::move(then)));
        });
        QCOMPARE(codeOf(another), std::string("busy"));
        QCOMPARE(saidOf(another),
                 u"slow.iso is being uploaded to jet at this moment, by this PC. Wait for it to finish."_s);
        QCOMPARE(wall.remove(id).error().code, std::string("busy"));
        stop();
        QTRY_VERIFY_WITH_TIMEOUT(carried.has_value(), 20'000);
        QCOMPARE(codeOf(*carried), std::string(core::MediaPlace::uploadStopped));
        QVERIFY(!images.has(u"slow.iso.incomplete"_s));
        QVERIFY(wall.remove(id));
    }
};

QTEST_GUILESS_MAIN(TestJetKvmUpload)
#include "test_jetkvm_upload.moc"
