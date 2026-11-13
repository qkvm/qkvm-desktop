#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <qkvm/core/incidents.h>
#include <qkvm/core/screen_watch.h>

#include "alerts_view.h"
#include "details_view.h"
#include "edit_kvm_view.h"
#include "memory_secrets.h"
#include "screen_shrink.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::ScreenWatch;

namespace {
base::Result<QByteArray> fast(QByteArrayView text, QByteArrayView salt, const std::stop_token&)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(text);
    hash.addData(salt);
    return hash.result();
}
void unlock(ui::AppLock& lock)
{
    lock.submit({}, u"example-password"_s, u"example-password"_s);
    QTRY_VERIFY(!lock.busy());
    QVERIFY(!lock.restricted());
}
core::Machine watched(const QString& id, const QString& name, int noSignal, int unchanged)
{
    core::Machine machine;
    machine.id = id;
    machine.name = name;
    machine.own = true;
    machine.noSignalMinutes = noSignal;
    machine.unchangedMinutes = unchanged;
    return machine;
}
core::Status status(const QString& state)
{
    core::Status result;
    result.state = state;
    return result;
}
ScreenWatch::Event alertOf(const QString& machine, const QString& watch, bool back, int minutes)
{
    ScreenWatch::Event said;
    said.machine = machine;
    said.watch = watch;
    said.back = back;
    said.minutes = minutes;
    said.at = QDateTime::currentDateTimeUtc();
    return said;
}
QByteArray aJpeg()
{
    QImage picture(320, 180, QImage::Format_RGB888);
    picture.fill(Qt::darkBlue);
    QByteArray jpeg;
    QBuffer into(&jpeg);
    into.open(QIODevice::WriteOnly);
    picture.save(&into, "JPEG");
    return jpeg;
}
} // namespace

// This PC's own screen watches in the alerts, in Details and in the Change box (c7): told as the page tells a server's
// (b7), in its words; the plain "no video signal" left to a KVM's no-signal watch; the picture shown from where it is
// kept; the minutes saved with the change.
class TestOwnScreenAlerts : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<ScreenWatch::Event>(); }

    void aPictureIsSquaresOfItsBrightness()
    {
        // 64 by 36 squares, each the average brightness of its part of the screen (the web's fingerprint).
        QImage half(640, 360, QImage::Format_RGB888);
        half.fill(Qt::black);
        QPainter(&half).fillRect(QRect(320, 0, 320, 360), Qt::white);
        const auto squaresOf = [](const QImage& picture) {
            return ScreenWatch::fingerprint(picture.constBits(), picture.width(), picture.height(),
                                            picture.bytesPerLine());
        };
        const QByteArray squares = squaresOf(half);
        QCOMPARE(squares.size(), qsizetype{64} * 36);
        QCOMPARE(static_cast<uchar>(squares.at(0)), uchar{0});
        QCOMPARE(static_cast<uchar>(squares.at(63)), uchar{255});
        // The noise of a capture is no change; a pointer that moved (two squares and more) is.
        QImage noisy = half.copy();
        QPainter(&noisy).fillRect(QRect(0, 0, 300, 300), QColor(8, 8, 8));
        QVERIFY(!ScreenWatch::differs(squares, squaresOf(noisy)));
        QImage one = half.copy();
        QPainter(&one).fillRect(QRect(0, 0, 10, 10), Qt::white);
        QVERIFY(!ScreenWatch::differs(squares, squaresOf(one)));
        QImage two = half.copy();
        QPainter(&two).fillRect(QRect(0, 0, 20, 10), Qt::white);
        QVERIFY(ScreenWatch::differs(squares, squaresOf(two)));
        // A JPEG is decoded and shrunk; what is none, or says it is larger than a 4K screen, is not.
        QByteArray jpeg;
        QBuffer into(&jpeg);
        into.open(QIODevice::WriteOnly);
        half.save(&into, "JPEG", 95);
        const std::optional<QByteArray> decoded = ui::shrunkScreen(jpeg);
        QVERIFY(decoded.has_value());
        QVERIFY(!ScreenWatch::differs(squares, decoded.value_or(QByteArray())));
        QVERIFY(!ui::shrunkScreen("not a picture"_ba).has_value());
        QImage huge(4000, 2200, QImage::Format_RGB888);
        huge.fill(Qt::gray);
        QByteArray tooLarge;
        QBuffer large(&tooLarge);
        large.open(QIODevice::WriteOnly);
        huge.save(&large, "JPEG", 50);
        QVERIFY(!ui::shrunkScreen(tooLarge).has_value());
    }

    void aScreenWatchsAlertIsAnEntryAndANotice()
    {
        QTemporaryDir dir;
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(secrets, fast, [] { return 1000; });
        unlock(lock);
        core::Incidents incidents(dir.filePath(u"history"_s));
        ui::AlertsView view(incidents, lock);
        view.watch(u"pc"_s, true, u"This PC"_s);
        view.context(u"pc"_s, true);
        view.inventoryFor(u"pc"_s, {watched(u"k1"_s, u"NAS"_s, 0, 30)});
        QSignalSpy told(&view, &ui::AlertsView::notification);
        view.screenAlertFor(u"pc"_s, alertOf(u"k1"_s, u"unchanged"_s, false, 30));
        QCOMPARE(told.size(), 1);
        QCOMPARE(told.constFirst().at(0).toString(), u"NAS: Screen unchanged for 30 minutes"_s);
        QVERIFY(told.constFirst().at(2).toBool());
        QCOMPARE(view.entries().size(), 1);
        QCOMPARE(view.entries().constFirst().toMap().value(u"state"_s).toString(),
                 u"Screen unchanged for 30 minutes"_s);
        QVERIFY(view.entries().constFirst().toMap().value(u"active"_s).toBool());
        QVERIFY(view.entries().constFirst().toMap().value(u"canPause"_s).toBool());
        // (The wall's list comes again: still watched, it stands.)
        view.inventoryFor(u"pc"_s, {watched(u"k1"_s, u"NAS"_s, 0, 30)});
        QCOMPARE(view.entries().size(), 1);
        // Back to normal: the entry is over, and that is told.
        view.screenAlertFor(u"pc"_s, alertOf(u"k1"_s, u"unchanged"_s, true, 30));
        QCOMPARE(told.size(), 2);
        QCOMPARE(told.at(1).at(0).toString(), u"NAS: Back to normal: the screen changes again"_s);
        QVERIFY(!told.at(1).at(2).toBool());
        QVERIFY(view.entries().isEmpty());
        // And kept as such: read back from the file.
        QVERIFY(incidents.flush());
        core::Incidents again(dir.filePath(u"history"_s));
        QVERIFY(!again.damaged());
        QCOMPARE(again.entries(u"pc"_s).size(), 1);
        QCOMPARE(again.entries(u"pc"_s).constFirst().minutes, 30);
    }

    void aWatchSwitchedOffIsNoLongerMonitored()
    {
        QTemporaryDir dir;
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(secrets, fast, [] { return 1000; });
        unlock(lock);
        core::Incidents incidents(dir.filePath(u"history"_s));
        ui::AlertsView view(incidents, lock);
        view.watch(u"pc"_s, true, u"This PC"_s);
        view.context(u"pc"_s, true);
        view.inventoryFor(u"pc"_s, {watched(u"k1"_s, u"NAS"_s, 5, 0)});
        view.screenAlertFor(u"pc"_s, alertOf(u"k1"_s, u"no_signal"_s, false, 5));
        QCOMPARE(view.entries().size(), 1);
        view.inventoryFor(u"pc"_s, {watched(u"k1"_s, u"NAS"_s, 0, 0)});
        QVERIFY(view.entries().isEmpty());
        view.setHistory(true);
        QVERIFY(view.entries().constFirst().toMap().value(u"removed"_s).toBool());
    }

    void aKvmWatchedForNoSignalLeavesThePlainAlertToTheWatch()
    {
        QTemporaryDir dir;
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(secrets, fast, [] { return 1000; });
        unlock(lock);
        core::Incidents incidents(dir.filePath(u"history"_s));
        ui::AlertsView view(incidents, lock);
        view.watch(u"pc"_s, true, u"This PC"_s);
        view.context(u"pc"_s, true);
        view.inventoryFor(u"pc"_s, {watched(u"k1"_s, u"NAS"_s, 10, 0), watched(u"k2"_s, u"Rack"_s, 0, 0)});
        QSignalSpy told(&view, &ui::AlertsView::notification);
        for (int look = 0; look < 3; ++look) {
            view.observeFor(u"pc"_s, {{u"k1"_s, status(u"online"_s)}, {u"k2"_s, status(u"online"_s)}}, true);
        }
        for (int look = 0; look < 3; ++look) {
            view.observeFor(u"pc"_s, {{u"k1"_s, status(u"no_signal"_s)}, {u"k2"_s, status(u"no_signal"_s)}}, true);
        }
        // The one without the watch is told after two looks; the one with it, by its watch, after its minutes.
        QCOMPARE(told.size(), 1);
        QVERIFY(told.constFirst().at(0).toString().startsWith(u"Rack"_s));
    }

    void anOwnKvmsDetailsSayItsScreenAlertsWithTheirPicture()
    {
        const QTemporaryDir folder;
        // What a screen watch told, as it is kept: written by one, read by another.
        {
            QDir().mkpath(folder.path());
            const QByteArray jpeg = aJpeg();
            QFile picture(QDir(folder.path()).filePath(u"k1-1793000000000.jpg"_s));
            QVERIFY(picture.open(QIODevice::WriteOnly));
            picture.write(jpeg);
            picture.close();
            QFile state(QDir(folder.path()).filePath(u"screens.json"_s));
            QVERIFY(state.open(QIODevice::WriteOnly));
            state.write(R"({"version": 1, "machines": {"k1": {"told": ["unchanged"], "said": [
                {"watch": "unchanged", "back": false, "minutes": 30, "lasted": 1800000,
                 "at": "2026-11-06T12:00:00.000Z", "stamp": "1793000000000", "pictureAt": "2026-11-06T11:59:58.000Z"}]}}})");
        }
        ScreenWatch watch;
        watch.keepIn(folder.path());
        ui::DetailsView details;
        details.bindOwn(&watch, u"k1"_s);
        details.setOpened(true);
        QVERIFY(details.available());
        QVERIFY(details.own());
        QVERIFY(details.administrator());
        // (What stands is read in the watches' own thread, and shown once it is in: review 4's LOW 5.)
        QTRY_COMPARE(details.activity().size(), 1);
        const QVariantMap row = details.activity().constFirst().toMap();
        QCOMPARE(row.value(u"what"_s).toString(), u"Alert: Screen unchanged for 30 minutes"_s);
        QVERIFY(row.value(u"picture"_s).toBool());
        details.showPicture(row.value(u"row"_s).toInt());
        QTRY_COMPARE(details.picture().value(u"state"_s).toString(), u"shown"_s);
        QCOMPARE(details.pictureImage().size(), QSize(320, 180));
        // A picture no longer kept is said so.
        QFile::remove(QDir(folder.path()).filePath(u"k1-1793000000000.jpg"_s));
        details.closePicture();
        details.showPicture(row.value(u"row"_s).toInt());
        QTRY_COMPARE(details.picture().value(u"state"_s).toString(), u"gone"_s);
        // Another machine of the wall has none.
        details.bindOwn(&watch, u"k2"_s);
        QVERIFY(details.activity().isEmpty());
        QVERIFY(details.activityNote().contains(u"No screen alerts"_s));
        // And a server's machine is no own one.
        details.bind(nullptr, {}, {}, false, false);
        QVERIFY(!details.own());
    }

    void theChangeBoxSaysAndTakesItsWatches()
    {
        ui::EditKvmView box;
        ui::EditKvmView::Shown shown;
        shown.id = u"k1"_s;
        shown.watchable = true;
        shown.noSignalMinutes = 5;
        box.show(shown, false);
        QVERIFY(box.watchable());
        QCOMPARE(box.noSignalMinutes(), 5);
        QCOMPARE(box.unchangedMinutes(), 0);
        QVERIFY(!box.typedWatches().has_value());
        box.typeWatches(u""_s, u"30"_s);
        QCOMPARE(box.typedWatches(), (std::optional<std::pair<int, int>>({0, 30})));
        box.typeWatches(u"5000"_s, u"x"_s);
        QCOMPARE(box.typedWatches(), (std::optional<std::pair<int, int>>({1440, 0})));
        // (What was typed goes with that box's saving alone: shown anew, nothing is typed.)
        box.show(shown, false);
        QVERIFY(!box.typedWatches().has_value());
    }
};

QTEST_MAIN(TestOwnScreenAlerts)
#include "test_own_screen_alerts.moc"
