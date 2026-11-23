#include <functional>
#include <memory>
#include <optional>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>

#include "../../core/tests/far_away_file.h"
#include "../../core/tests/jetkvm_by_hand.h"
#include "../../core/tests/jetkvm_storage_by_hand.h"
#include "../../core/tests/jetkvm_viewer_by_hand.h"
#include "../../core/tests/memory_secrets.h"
#include "media_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

QByteArray patterned(qint64 size)
{
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    for (qsizetype at = 0; at < bytes.size(); ++at) {
        bytes[at] = core::testing::patternAt(at);
    }
    return bytes;
}

core::DriverPaces quick()
{
    core::DriverPaces paces;
    paces.jetkvm.connectWithinMs = 3000;
    paces.jetkvm.quietMs = 3000;
    paces.jetkvm.transferMs = 3000;
    paces.jetkvm.lineWithinMs = 4000;
    paces.jetkvm.freshMs = 0;
    paces.jetkvm.pacing = std::make_shared<core::SignInPacing>();
    paces.jetkvm.viewer.connection.watchEveryMs = 100;
    paces.jetkvm.viewer.unwatchedEveryMs = 100;
    paces.jetkvm.viewer.retryPauseMs = 300;
    return paces;
}

} // namespace

// The media box (MediaView, the page's Media box: app.js, mediaDialog) for a JetKVM of this PC's own wall (GAPS A3), as
// the web's browser test of a JetKVM's box has it (test_virtual_media_on_a_jetkvm_in_a_browser.py of web 0.3.1): what
// an image goes in as (a CD-ROM drive or a disk, read-only either way: no USB flash drive, nothing writable), its foot
// and its questions saying first that a change between the two drops its keyboard and mouse for a moment, and an upload
// into its own storage with its hash said after it. The box is c8a's, as for any machine with media; the wall is its
// place, which speaks to the JetKVM itself. Against the JetKVM by hand and its storage.
class TestJetKvmMediaBox : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(core::useWindowsOwnTls());
        QVERIFY2(core::testing::JetKvmPeerByHand::filmIsThere(), core::testing::JetKvmPeerByHand::noFilm);
    }

    void aJetKvmsBoxSaysFirstThatItsKeyboardDropsAndMountsAsADisk()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        core::LocalWall wall(settings, secrets, quick());
        core::testing::JetKvmStorageByHand images;
        images.add(u"debian-13.iso"_s, 700'000'000);
        core::testing::JetKvmByHand jet;
        core::testing::JetKvmViewerByHand viewer{jet};
        viewer.storage = &images;
        jet.storage = &images;
        core::OwnKvm own;
        own.address = jet.address().toString();
        own.name = u"jet"_s;
        own.kind = u"jetkvm"_s;
        const QString id = wall.add(own, u"secret"_s).value();
        // Its card asks, and connects; a look over its connection finds its media.
        using Statuses = QHash<QString, core::Status>;
        const auto offers = [&] {
            wall.askSnapshot(id, 320, 0, [](const base::Result<core::Snapshot>&) {});
            std::optional<base::Result<Statuses>> now;
            wall.askStatus([&now](const base::Result<Statuses>& came) { now = came; });
            return QTest::qWaitFor([&now] { return now.has_value(); }, 5000) && *now &&
                   now->value().value(id).offersMedia;
        };
        QVERIFY(QTest::qWaitFor(offers, 30000));
        ui::MediaView::UnderWay underWay;
        ui::MediaView view;
        QStringList told;
        QObject::connect(&view, &ui::MediaView::told, &view, [&told](const QString& said, bool good) {
            told.append((good ? u"ok: "_s : u"no: "_s) + said);
        });
        const QString path = folder.filePath(u"tools.img"_s);
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(patterned(2'000'000));
        }
        view.setImageDoor([&path] { return base::Result<QString>(path); });
        view.open(&wall, id, u"jet"_s, u"pc/"_s + id, &underWay, u"pc:"_s + id);
        QVERIFY(QTest::qWaitFor([&] { return !view.images().isEmpty(); }, 20000));
        QCOMPARE(view.where(), u"On the KVM's own storage · 6.3 GB free of 7.0 GB"_s);
        QCOMPARE(view.now(), u"Nothing is mounted."_s);
        // A CD-ROM drive or a disk, read-only either way: nothing writable, no flash drive.
        QStringList enabled;
        for (const QVariant& one : view.drives()) {
            if (one.toMap().value(u"enabled"_s).toBool()) {
                enabled.append(one.toMap().value(u"value"_s).toString());
            }
        }
        QCOMPARE(enabled, (QStringList{u"cd"_s, u"disk"_s}));
        QVERIFY2(view.foot().contains(u"A switch between the two drops its keyboard and mouse for a moment."_s),
                 qPrintable(view.foot()));
        QVERIFY(!view.restartEnabled());
        view.pick(u"debian-13.iso"_s);
        view.chooseDrive(u"disk"_s);
        view.mount();
        QCOMPARE(view.question().value(u"text"_s).toString(),
                 u"jet gets debian-13.iso as a disk, read-only, and nothing else is done: pick the KVM's drive in its "
                 u"boot menu at its next start. Its keyboard and mouse drop for a moment if this switches its drive "
                 u"between CD-ROM and disk."_s);
        view.answer(true);
        QVERIFY(QTest::qWaitFor([&] { return !view.busy() && !told.isEmpty(); }, 20000));
        QCOMPARE(told.constLast(), u"ok: jet: Mount debian-13.iso done"_s);
        QVERIFY2(QTest::qWaitFor([&] { return view.now() == u"debian-13.iso · disk · read-only"_s; }, 10000),
                 qPrintable(view.now()));
        QCOMPARE(images.mountedNow().value_or(QJsonObject{}).value(u"mode"_s).toString(), u"Disk"_s);
        QCOMPARE(images.rebound(), 1);
        // An unmount says it too.
        view.unmount();
        QVERIFY2(
            view.question().value(u"text"_s).toString().endsWith(u" Its keyboard and mouse may drop for a moment."_s),
            qPrintable(view.question().value(u"text"_s).toString()));
        view.answer(true);
        QVERIFY(QTest::qWaitFor(
            [&] { return !view.busy() && told.constLast() == u"ok: jet: Unmount debian-13.iso done"_s; }, 20000));
        QVERIFY(!images.mountedNow());
        // An upload, into its own storage, with its hash said after it.
        view.upload();
        QVERIFY(QTest::qWaitFor([&] { return !view.question().isEmpty(); }, 10000));
        QCOMPARE(view.question().value(u"title"_s).toString(), u"Upload tools.img"_s);
        view.answer(true);
        const QString sha =
            QString::fromLatin1(QCryptographicHash::hash(patterned(2'000'000), QCryptographicHash::Sha256).toHex());
        QVERIFY(QTest::qWaitFor([&] { return !view.busy() && told.constLast() == u"ok: jet: Upload tools.img done"_s; },
                                30000));
        QCOMPARE(view.said(), u"tools.img is on the KVM, SHA-256 "_s + sha);
        {
            const auto held = images.lock();
            QCOMPARE(images.files.at(u"tools.img"_s).sha256, sha.toLatin1());
        }
        QVERIFY(images.neverAsked().isEmpty());
        view.letGo();
    }
};

QTEST_MAIN(TestJetKvmMediaBox)
#include "test_jetkvm_media_box.moc"
