#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <qkvm/core/board_holds.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_media.h>
#include <qkvm/core/read_ahead.h>
#include <qkvm/core/sign_in_pacing.h>

#include "far_away_file.h"
#include "made_up_kvm.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::KvmdDevice;
using core::MediaUploaded;
using core::ReadAhead;
using core::testing::Answered;
using core::testing::FarAwayFile;
using core::testing::MadeUpKvm;
using core::testing::refusal;

namespace {

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

// What came, once it did: nothing that came is no answer.
template <typename T>
base::Result<T> heard(const std::optional<base::Result<T>>& came)
{
    return came.value_or(base::Result<T>(base::Error{"never", "Nothing was answered."}));
}

std::string codeOf(const base::Result<MediaUploaded>& done)
{
    return done ? std::string("(ok)") : done.error().code;
}

std::string saidOf(const base::Result<MediaUploaded>& done)
{
    return done ? done.value().sha256.toStdString() : done.error().message;
}

} // namespace

// An image carried from this PC to a kvmd KVM's own storage (GAPS A2; the web's 0.3.0, kvm.py media_upload, _carry,
// media_discard): POST /api/msd/write with its length said first and remove_incomplete, from the file as it is read
// (never whole in this program), hashed as it goes; one at a time; refused before a byte goes where the web refuses
// it; what the KVM did not take whole, a stop, and a file that changed while it was read leave nothing on the KVM; the
// KVM's word that it has it all is held to its listing. Against a made-up KVM. These wait for uploads: a program of
// their own.
class TestKvmdUpload : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_folder{QCoreApplication::applicationDirPath() + u"/kvmd-upload-test-XXXXXX"_s};

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
        pace.msdEveryMs = 1200;
        pace.listedWithinMs = 1000;
        pace.listedEveryMs = 100;
        pace.listedForADiscardMs = 400;
        pace.discardWaitMs = 1500;
        pace.discardEveryMs = 100;
        pace.uploadQuietMs = 3000;
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.boards = std::make_shared<core::BoardHolds>();
        return pace;
    }
    // A KVM with a drive and one image on its storage, looked at once.
    static void withADrive(MadeUpKvm& kvm)
    {
        kvm.hasDrive = true;
        kvm.msd.images = {{u"there.iso"_s, QJsonObject{{u"size"_s, 700'000}, {u"complete"_s, true}}}};
    }
    static void looked(KvmdDevice& device)
    {
        std::optional<bool> over;
        QObject::connect(&device, &KvmdDevice::looked, &device, [&over] { over = true; });
        device.look();
        static_cast<void>(QTest::qWaitFor([&over] { return over.has_value(); }, 20000));
    }
    // A file of this many bytes of the pattern, in the test's folder.
    QString fileOf(const QString& name, qint64 size)
    {
        const QString path = m_folder.filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(patternOf(size));
        }
        return path;
    }
    // A file opened to be read as the box reads one: on a thread of its own, held to stay as it was.
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
    // Uploads, and waits for the answer as the program's own loop waits.
    static base::Result<MediaUploaded> upload(KvmdDevice& device, std::unique_ptr<QIODevice> image,
                                              const QString& name = u"debian 13+netinst.iso"_s,
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
    static int writes(const MadeUpKvm& kvm) { return kvm.asked("/api/msd/write"); }
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
        QVERIFY(m_folder.isValid());
    }

    void anImageGoesWholeAsItIsReadWithItsLengthSaidFirst()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        const QString path = fileOf(u"a.iso"_s, 3'000'001);
        const base::Result<MediaUploaded> done = upload(device, opened(path));
        QVERIFY2(done, saidOf(done).c_str());
        const QByteArray sha = QCryptographicHash::hash(patternOf(3'000'001), QCryptographicHash::Sha256).toHex();
        QCOMPARE(done.value().sha256, QString::fromLatin1(sha));
        // As kvmd takes it: the name (a blank and a plus reach it as they are), and what a cut-off write leaves
        // removed by kvmd itself; its length said first; nothing but the image as the body; the session in the cookie
        // alone, never in the address.
        QCOMPARE(writes(kvm), 1);
        const core::testing::Asked* write = kvm.last("/api/msd/write");
        QVERIFY(write != nullptr);
        QCOMPARE(write->method, "POST"_ba);
        QCOMPARE(write->path, "/api/msd/write?image=debian%2013%2Bnetinst.iso&remove_incomplete=1"_ba);
        QCOMPARE(write->headers.value("content-length"), "3000001"_ba);
        QCOMPARE(write->headers.value("content-type"), "application/octet-stream"_ba);
        QVERIFY(write->headers.value("cookie").startsWith("auth_token=pass"));
        QCOMPARE(QCryptographicHash::hash(write->body, QCryptographicHash::Sha256).toHex(), sha);
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd write debian 13+netinst.iso remove_incomplete 1"});
        // Listed with its SHA-256 from here on, while it is that image.
        std::optional<base::Result<core::MediaList>> list;
        device.askMedia([&list](base::Result<core::MediaList> came) { list = std::move(came); });
        QVERIFY(QTest::qWaitFor([&list] { return list.has_value(); }, 10000));
        const base::Result<core::MediaList> listed = heard(list);
        QVERIFY(listed);
        const auto image = std::ranges::find_if(
            listed.value().images, [](const core::MediaImage& one) { return one.name == "debian 13+netinst.iso"_L1; });
        QVERIFY(image != listed.value().images.end());
        QCOMPARE(image->sha256, QString::fromLatin1(sha));
        QVERIFY(!device.uploading());
    }

    void itIsReadAsItGoesAndNeverWholeFirst()
    {
        // Forty-eight megabytes, of which no more than the file's own read-ahead and a few pieces were read when the
        // KVM had its first megabyte.
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        const auto readTo = std::make_shared<std::atomic<qint64>>(0);
        qint64 readAtTheFirst = -1;
        kvm.stand->hearing = [&](qint64 soFar) {
            if (readAtTheFirst < 0 && soFar > qint64{1} << 20) {
                readAtTheFirst = readTo->load();
            }
        };
        constexpr qint64 size = qint64{48} << 20;
        const base::Result<MediaUploaded> done =
            upload(device, std::make_unique<Pattern>(size, readTo), u"large.img"_s);
        QVERIFY2(done, saidOf(done).c_str());
        QVERIFY(readAtTheFirst > 0);
        QVERIFY2(readAtTheFirst < qint64{12} << 20, QByteArray::number(readAtTheFirst).constData());
        QCOMPARE(kvm.last("/api/msd/write")->body.size(), size);
    }

    void howFarItHasComeIsSaidAndOneGoesAtATime()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        // (A file far away: a piece every 30 ms.)
        constexpr qint64 size = 6'000'000;
        qint64 mostSeen = 0;
        std::optional<base::Result<MediaUploaded>> second;
        QTimer watch;
        QObject::connect(&watch, &QTimer::timeout, &watch, [&] {
            if (const std::optional<core::MediaUploading> now = device.uploading()) {
                QCOMPARE(now->name, u"far.iso"_s);
                QCOMPARE(now->size, size);
                mostSeen = std::max(mostSeen, now->sent);
            }
        });
        const base::Result<MediaUploaded> done =
            upload(device, ReadAhead::of(std::make_unique<FarAwayFile>(size, 30)), u"far.iso"_s, [&] {
                watch.start(20);
                device.uploadMedia(u"other.iso"_s, std::make_unique<Pattern>(1000),
                                   [&second](base::Result<MediaUploaded> came) { second = std::move(came); });
            });
        QVERIFY2(done, saidOf(done).c_str());
        QVERIFY(mostSeen > 0);
        QVERIFY(mostSeen <= size);
        QVERIFY(QTest::qWaitFor([&second] { return second.has_value(); }, 5000));
        QCOMPARE(codeOf(heard(second)), std::string("busy"));
        QVERIFY(QString::fromStdString(saidOf(heard(second))).contains("far.iso is being uploaded"_L1));
        QCOMPARE(writes(kvm), 1);
        QVERIFY(!device.uploading());
    }

    void anUploadIsRefusedBeforeAByteGoesWhereTheWebRefusesIt_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<qint64>("size");
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("part");
        QTest::newRow("empty") << u"a.iso"_s << qint64{0} << QString() << u"invalid"_s
                               << u"An empty file is no image"_s;
        QTest::newRow("larger than may be")
            << u"a.iso"_s << qint64{16'000'000'001} << QString() << u"too_large"_s << u"16 GB at the most"_s;
        QTest::newRow("a dot first") << u".a.iso"_s << qint64{10} << QString() << u"invalid"_s << u"Rename the file"_s;
        QTest::newRow("a slash") << u"isos/a.iso"_s << qint64{10} << QString() << u"invalid"_s << u"Rename the file"_s;
        QTest::newRow("a JetKVM's part") << u"a.iso.incomplete"_s << qint64{10} << QString() << u"invalid"_s
                                         << u"Rename the file"_s;
        QTest::newRow("there already") << u"there.iso"_s << qint64{10} << QString() << u"duplicate"_s
                                       << u"there.iso is on the KVM's own storage already"_s;
        QTest::newRow("no room") << u"a.iso"_s << qint64{2'000} << u"full"_s << u"no_space"_s
                                 << u"has 1,000 bytes free, and the image is 2,000: delete an image there first"_s;
        QTest::newRow("busy") << u"a.iso"_s << qint64{10} << u"busy"_s << u"busy"_s << u"busy with something else"_s;
        QTest::newRow("offline") << u"a.iso"_s << qint64{10} << u"offline"_s << u"action_failed"_s << u"(offline)"_s;
        QTest::newRow("File Sharing") << u"a.iso"_s << qint64{10} << u"sharing"_s << u"action_failed"_s
                                      << u"File Sharing is on"_s;
        QTest::newRow("switched off") << u"a.iso"_s << qint64{10} << u"off"_s << u"no_virtual_media"_s
                                      << u"switched off"_s;
        // (Not found in a list not read whole: it may be there all the same. Outside review 4.6.)
        QTest::newRow("its list not read") << u"a.iso"_s << qint64{10} << u"unread"_s << u"bad_response"_s
                                           << u"so whether a.iso is there already is not known"_s;
    }
    void anUploadIsRefusedBeforeAByteGoesWhereTheWebRefusesIt()
    {
        QFETCH(QString, name);
        QFETCH(qint64, size);
        QFETCH(QString, state);
        QFETCH(QString, code);
        QFETCH(QString, part);
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        kvm.msd.free = state == "full"_L1 ? 1000 : kvm.msd.free;
        kvm.msd.busy = state == "busy"_L1;
        kvm.msd.online = state != "offline"_L1;
        kvm.msd.fileSharing = state == "sharing"_L1;
        kvm.msd.enabled = state != "off"_L1;
        if (state == "unread"_L1) {
            kvm.msd.imagesSayInstead = QJsonValue(u"none"_s);
        }
        const base::Result<MediaUploaded> done = upload(device, std::make_unique<Pattern>(size), name);
        QCOMPARE(codeOf(done), code.toStdString());
        QVERIFY2(QString::fromStdString(saidOf(done)).contains(part), saidOf(done).c_str());
        QCOMPARE(writes(kvm), 0);
        QVERIFY(!device.uploading());
    }

    void whatTheKvmDidNotTakeWholeIsTakenOffAndSaid_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("left");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("said");
        QTest::newRow("less written")
            << 200 << u"partial"_s << u"action_failed"_s
            << u"The KVM did not take the image whole (it says 99,999 of 100,000 bytes were written). Nothing of it "
               u"is left on the KVM."_s;
        QTest::newRow("no room")
            << 507 << u"partial"_s << u"no_space"_s
            << u"The KVM's own storage has not got room for it. Nothing of it is left on the KVM."_s;
        QTest::newRow("the session refused")
            << 401 << u"none"_s << u"action_failed"_s
            << u"The KVM did not take qKVM's session for the upload: try again. Nothing of it is left on the KVM."_s;
        QTest::newRow("its own words")
            << 500 << u"none"_s << u"action_failed"_s
            << u"The KVM did not take the image whole (Disk\twrite failed). Nothing of it is left on the KVM."_s;
        QTest::newRow("whole after all")
            << 500 << u"whole"_s << u"action_failed"_s
            << u"The KVM did not take the image whole (Disk\twrite failed). It is on the KVM whole all the same "
               u"(100,000 bytes), and stays there: delete it there if it is not wanted."_s;
        // (Listed without its saying whether it is whole: it may be, and is never removed. Review cx4-1 L1: it was.)
        QTest::newRow("whole or not, not said")
            << 500 << u"unsaid"_s << u"action_failed"_s
            << u"The KVM did not take the image whole (Disk\twrite failed). It is on the KVM (100,000 bytes), which "
               u"does not say whether it is whole, and stays there: delete it there if it is not wanted."_s;
        QTest::newRow("a part that stays")
            << 200 << u"stays"_s << u"action_failed"_s
            << u"The KVM did not take the image whole (it says 99,999 of 100,000 bytes were written). What was "
               u"written of it is on the KVM still: delete it there."_s;
        // (Its list not read whole after it: nothing found there is not "nothing left". Outside review 4.6.)
        QTest::newRow("its list not read after it")
            << 500 << u"unread"_s << u"action_failed"_s
            << u"The KVM did not take the image whole (Disk\twrite failed). What was written of it may be on the KVM "
               u"still (its list of its own storage could not be read whole): delete it there."_s;
    }
    void whatTheKvmDidNotTakeWholeIsTakenOffAndSaid()
    {
        QFETCH(int, status);
        QFETCH(QString, left);
        QFETCH(QString, code);
        QFETCH(QString, said);
        said.replace(u'\t', u' ');
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        // What the KVM answers to the write, and what it leaves of the image.
        if (status == 200) {
            kvm.instead.insert("/api/msd/write",
                               core::testing::good({{u"image"_s, QJsonObject{{u"name"_s, u"x.iso"_s},
                                                                             {u"size"_s, 100'000},
                                                                             {u"written"_s, 99'999}}}}));
        } else if (status == 500) {
            kvm.instead.insert("/api/msd/write", refusal(500, u"MsdError"_s, u"Disk\twrite failed"_s));
        } else {
            kvm.instead.insert("/api/msd/write", refusal(status, u"Error"_s));
        }
        kvm.stand->beforeAnswer = [&kvm, left] {
            if (!kvm.stand->asked.isEmpty() && kvm.stand->asked.last().path.startsWith("/api/msd/write") &&
                left == "unread"_L1) {
                kvm.msd.imagesSayInstead = QJsonValue(u"none"_s);
            } else if (!kvm.stand->asked.isEmpty() && kvm.stand->asked.last().path.startsWith("/api/msd/write") &&
                       left == "unsaid"_L1) {
                kvm.msd.images.insert(u"x.iso"_s, QJsonObject{{u"size"_s, 100'000}});
            } else if (!kvm.stand->asked.isEmpty() && kvm.stand->asked.last().path.startsWith("/api/msd/write") &&
                       left != "none"_L1) {
                kvm.msd.images.insert(u"x.iso"_s, QJsonObject{{u"size"_s, left == "whole"_L1 ? 100'000 : 99'999},
                                                              {u"complete"_s, left == "whole"_L1}});
            }
        };
        if (left == "stays"_L1) {
            kvm.instead.insert("/api/msd/remove", core::testing::good());
        }
        const base::Result<MediaUploaded> done = upload(device, std::make_unique<Pattern>(100'000), u"x.iso"_s);
        QCOMPARE(codeOf(done), code.toStdString());
        QCOMPARE(QString::fromStdString(saidOf(done)), said);
        const bool removed = left == "partial"_L1 || left == "stays"_L1;
        // A session it refused is forgotten: what is asked after it is asked under one given anew.
        QCOMPARE(kvm.signIns, status == 401 ? 2 : 1);
        QCOMPARE(kvm.asked("/api/msd/remove"), removed ? 1 : 0);
        if (left == "partial"_L1) {
            QVERIFY(!kvm.msd.images.contains("x.iso"_L1));
        }
        QVERIFY(!device.uploading());
    }

    void whatTheKvmSaysItWroteWholeIsHeldToItsListing()
    {
        // kvmd said it wrote every byte, and does not list it whole: said, and never taken off.
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        kvm.instead.insert(
            "/api/msd/write",
            core::testing::good(
                {{u"image"_s, QJsonObject{{u"name"_s, u"x.iso"_s}, {u"size"_s, 5000}, {u"written"_s, 5000}}}}));
        const base::Result<MediaUploaded> done = upload(device, std::make_unique<Pattern>(5000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done), std::string("The KVM said it wrote all of the image, and after 1 seconds still does not "
                                           "list it whole: look at it on the KVM's own page before mounting it."));
        QCOMPARE(kvm.asked("/api/msd/remove"), 0);
        // Listed a moment later (as its watch on the storage saw it): it is.
        kvm.instead.insert(
            "/api/msd/write",
            core::testing::good(
                {{u"image"_s, QJsonObject{{u"name"_s, u"y.iso"_s}, {u"size"_s, 5000}, {u"written"_s, 5000}}}}));
        QTimer::singleShot(400, &device, [&kvm] {
            kvm.msd.images.insert(u"y.iso"_s, QJsonObject{{u"size"_s, 5000}, {u"complete"_s, true}});
        });
        const base::Result<MediaUploaded> later = upload(device, std::make_unique<Pattern>(5000), u"y.iso"_s);
        QVERIFY2(later, saidOf(later).c_str());
    }

    void aStoppedUploadLeavesNothingOnTheKvm()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        std::function<void()> stop;
        std::optional<base::Result<MediaUploaded>> answer;
        stop = device.uploadMedia(u"far.iso"_s, ReadAhead::of(std::make_unique<FarAwayFile>(20'000'000, 30)),
                                  [&answer](base::Result<MediaUploaded> came) { answer = std::move(came); });
        QVERIFY(QTest::qWaitFor([&] { return device.uploading().value_or(core::MediaUploading{}).sent > 0; }, 10000));
        // What kvmd has of it meanwhile, and its drive busy with its end a moment longer.
        kvm.msd.images.insert(u"far.iso"_s, QJsonObject{{u"size"_s, 300'000}, {u"complete"_s, false}});
        kvm.msd.busy = true;
        QTimer::singleShot(300, &device, [&kvm] { kvm.msd.busy = false; });
        stop();
        stop(); // (twice is once)
        QVERIFY(QTest::qWaitFor([&answer] { return answer.has_value(); }, 10000));
        QCOMPARE(codeOf(heard(answer)), std::string("desktop:stopped"));
        QVERIFY2(QString::fromStdString(saidOf(heard(answer))).endsWith("Nothing of it is left on the KVM."_L1),
                 saidOf(heard(answer)).c_str());
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd remove far.iso"});
        QVERIFY(!kvm.msd.images.contains("far.iso"_L1));
        QVERIFY(!device.uploading());
        // Stopped before it went: nothing went, nothing to take off.
        kvm.did.clear();
        answer.reset();
        stop = device.uploadMedia(u"soon.iso"_s, std::make_unique<Pattern>(1000),
                                  [&answer](base::Result<MediaUploaded> came) { answer = std::move(came); });
        stop();
        QVERIFY(QTest::qWaitFor([&answer] { return answer.has_value(); }, 10000));
        QCOMPARE(codeOf(heard(answer)), std::string("desktop:stopped"));
        QCOMPARE(writes(kvm), 0);
        QVERIFY(msdDone(kvm).isEmpty());
    }

    void aFileThatChangesWhileItIsReadIsRefused_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("longer") << u"longer"_s;
        QTest::newRow("written anew, as long as it was") << u"anew"_s;
    }
    void aFileThatChangesWhileItIsReadIsRefused()
    {
        QFETCH(QString, change);
        const bool longer = change == "longer"_L1;
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        const QString path = fileOf(longer ? u"grows.iso"_s : u"anew.iso"_s, 8'000'000);
        // (Read a piece ahead at the most: the rest of it is read once the upload goes.)
        std::unique_ptr<ReadAhead> file = opened(path, ReadAhead::pieceBytes);
        QVERIFY(file != nullptr);
        const base::Result<MediaUploaded> done = upload(device, std::move(file), u"changes.iso"_s, [&] {
            QFile changed(path);
            if (longer) {
                QVERIFY(changed.open(QIODevice::Append));
                changed.write("more");
            } else {
                QVERIFY(changed.open(QIODevice::ReadWrite));
                QVERIFY(
                    changed.setFileTime(QDateTime::currentDateTimeUtc().addSecs(5), QFileDevice::FileModificationTime));
            }
        });
        QCOMPARE(codeOf(done), std::string("disconnected"));
        QVERIFY2(QString::fromStdString(saidOf(done)).contains("It was changed while it was read"_L1),
                 saidOf(done).c_str());
        QVERIFY2(QString::fromStdString(saidOf(done)).endsWith("Nothing of it is left on the KVM."_L1),
                 saidOf(done).c_str());
        QCOMPARE(writes(kvm), 0);
        QVERIFY(!kvm.msd.images.contains("changes.iso"_L1));
    }

    void aFileThatStaysAsItWasIsTaken()
    {
        // (Read to its end without a word: its last piece, too.)
        const QString path = fileOf(u"stays.iso"_s, 1'000'000);
        std::unique_ptr<ReadAhead> file = opened(path);
        QVERIFY(file != nullptr);
        QCOMPARE(file->size(), 1'000'000);
        QByteArray all;
        QVERIFY(QTest::qWaitFor(
            [&] {
                all += file->readAll();
                return all.size() == 1'000'000 || !file->failure().isEmpty();
            },
            10000));
        QVERIFY(file->failure().isEmpty());
        QCOMPARE(all, patternOf(1'000'000));
    }

    void aLineThatFallsUnderItIsSaidAndWhatWasLeftTakenOff()
    {
        MadeUpKvm kvm;
        withADrive(kvm);
        KvmdDevice device(kvm.where(), kvm.password, quick());
        looked(device);
        Answered gone;
        gone.hangUp = true;
        kvm.instead.insert("/api/msd/write", gone);
        kvm.stand->beforeAnswer = [&kvm] {
            if (!kvm.stand->asked.isEmpty() && kvm.stand->asked.last().path.startsWith("/api/msd/write")) {
                kvm.msd.images.insert(u"x.iso"_s, QJsonObject{{u"size"_s, 99}, {u"complete"_s, false}});
            }
        };
        const base::Result<MediaUploaded> done = upload(device, std::make_unique<Pattern>(5000), u"x.iso"_s);
        QCOMPARE(codeOf(done), std::string("dropped"));
        QVERIFY2(QString::fromStdString(saidOf(done)).startsWith("The upload did not get through to the KVM ("_L1),
                 saidOf(done).c_str());
        QVERIFY(QString::fromStdString(saidOf(done)).endsWith("Nothing of it is left on the KVM."_L1));
        QCOMPARE(msdDone(kvm), QList<QByteArray>{"msd remove x.iso"});
    }
};

QTEST_MAIN(TestKvmdUpload)
#include "test_kvmd_upload.moc"
