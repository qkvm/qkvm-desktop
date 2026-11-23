#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/media_place.h>
#include <qkvm/core/read_ahead.h>

#include "far_away_file.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::LocalWall;
using core::MediaDone;
using core::MediaPlace;
using core::OwnKvm;
using core::testing::FarAwayFile;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;

namespace {

template <typename T>
base::Result<T> answerTo(const std::function<void(std::function<void(base::Result<T>)>)>& ask)
{
    std::optional<base::Result<T>> answer;
    ask([&answer](base::Result<T> came) { answer = std::move(came); });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 20000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return *answer;
}

template <typename T>
std::string codeOf(const base::Result<T>& result)
{
    return result ? std::string("(ok)") : result.error().code;
}

template <typename T>
QString saidOf(const base::Result<T>& result)
{
    return result ? QString() : QString::fromStdString(result.error().message);
}

} // namespace

// This PC's own wall as the place of its kvmd KVMs' virtual media (GAPS A2; the web's server.py media routes, which it
// stands in for): a KVM of the wall, of a family that has them and has some now, named as the question named it; one
// thing at a time, as for an action, and nothing of an image on its way; the KVM not changed nor taken off while one
// is; whoever asks told by the wall where the KVM's driver is let go meanwhile. Against made-up KVMs.
class TestLocalWallMedia : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_folder;

    static core::KvmdDevice::Pace quick()
    {
        core::KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.holdMs = 400;
        pace.longestHoldMs = 1600;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.pressHeldMs = 100;
        pace.longPressHeldMs = 300;
        pace.boardAnswerMs = 1500;
        pace.seenWithinMs = 600;
        pace.seenEveryMs = 100;
        pace.listedWithinMs = 900;
        pace.listedEveryMs = 100;
        pace.listedForADiscardMs = 300;
        pace.discardWaitMs = 600;
        pace.discardEveryMs = 100;
        pace.boards = std::make_shared<core::BoardHolds>();
        return pace;
    }
    static void withADrive(MadeUpKvm& kvm)
    {
        kvm.hasDrive = true;
        kvm.msd.images = {{u"debian.iso"_s, QJsonObject{{u"size"_s, 700'000'000}, {u"complete"_s, true}}},
                          {u"x.iso.incomplete"_s, QJsonObject{{u"size"_s, 10}, {u"complete"_s, true}}}};
    }
    static OwnKvm at(const MadeUpKvm& kvm, const QString& name)
    {
        OwnKvm own;
        own.address = kvm.stand->address();
        own.name = name;
        return own;
    }
    // How a KVM of the wall is once its drive was asked: until its status says it has virtual media, or it has been
    // looked at a while.
    static core::Status settled(LocalWall& wall, const QString& id, bool media = true)
    {
        core::Status status;
        static_cast<void>(QTest::qWaitFor(
            [&] {
                status = answerTo<QHash<QString, core::Status>>([&](auto done) { wall.askStatus(std::move(done)); })
                             .value()
                             .value(id);
                return status.state == u"online"_s && status.offersMedia == media;
            },
            20000));
        return status;
    }
    static core::MediaMount mountOf(const QString& image, const QString& machine)
    {
        core::MediaMount mount;
        mount.image = image;
        mount.machine = machine;
        return mount;
    }

private slots:
    void initTestCase()
    {
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
        QVERIFY(m_folder.isValid());
    }

    void aKvmdKvmsMediaAreHadThroughTheWall()
    {
        core::Settings settings(m_folder.filePath(u"one.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        withADrive(kvm);
        const QString id = wall.add(at(kvm, u"NAS"_s), u"secret"_s).value();
        core::Status status = settled(wall, id);
        QVERIFY(status.offersMedia);
        MediaPlace& place = wall;
        QCOMPARE(place.mediaIdentity(), "this-pc"_ba);
        // Its own words are said as they are: nothing of this wall's is "no answer".
        QVERIFY(!place.saidNothing(base::Error{"timeout", "Device did not respond in time"}));
        const base::Result<core::MediaList> list =
            answerTo<core::MediaList>([&](auto done) { place.askMedia(id, std::move(done)); });
        QVERIFY2(list, qPrintable(saidOf(list)));
        QCOMPARE(list.value().images.size(), 2);
        QCOMPARE(list.value().offers.where, u"device"_s);
        base::Result<MediaDone> done = answerTo<MediaDone>(
            [&](auto then) { place.mountMedia(id, mountOf(u"debian.iso"_s, u"NAS"_s), std::move(then)); });
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(kvm.msd.connected);
        // On its card.
        QVERIFY(QTest::qWaitFor(
            [&] {
                return answerTo<QHash<QString, core::Status>>([&](auto then) { wall.askStatus(std::move(then)); })
                    .value()
                    .value(id)
                    .media.has_value();
            },
            10000));
        done = answerTo<MediaDone>(
            [&](auto then) { place.unmountMedia(id, u"debian.iso"_s, u"NAS"_s, QString(), std::move(then)); });
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(!kvm.msd.connected);
        done = answerTo<MediaDone>(
            [&](auto then) { place.deleteMedia(id, u"debian.iso"_s, u"NAS"_s, QString(), std::move(then)); });
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(!kvm.msd.images.contains("debian.iso"_L1));
    }

    void whatStandsAgainstItIsSaidInTheServersWords()
    {
        core::Settings settings(m_folder.filePath(u"two.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        withADrive(kvm);
        MadeUpKvm without;
        const QString id = wall.add(at(kvm, u"NAS"_s), u"secret"_s).value();
        const QString none = wall.add(at(without, u"Old KVM"_s), u"secret"_s).value();
        // (A JetKVM at an address that reaches nothing: its family has none here, and it is asked nothing.)
        OwnKvm jet;
        jet.address = u"http://192.0.2.5"_s;
        jet.kind = u"jetkvm"_s;
        jet.name = u"Rack JetKVM"_s;
        const QString jetkvm = wall.add(jet, u"secret"_s).value();
        static_cast<void>(settled(wall, id));
        static_cast<void>(settled(wall, none, false));
        MediaPlace& place = wall;
        const auto mount = [&](const QString& machine, const QString& named) {
            return answerTo<MediaDone>(
                [&](auto then) { place.mountMedia(machine, mountOf(u"debian.iso"_s, named), std::move(then)); });
        };
        base::Result<MediaDone> done = mount(u"no such"_s, u"NAS"_s);
        QCOMPARE(codeOf(done), std::string("not_found"));
        done = mount(jetkvm, u"Rack JetKVM"_s);
        QCOMPARE(codeOf(done), std::string("no_virtual_media"));
        QCOMPARE(saidOf(done), u"Rack JetKVM has no virtual media"_s);
        done = mount(none, u"Old KVM"_s);
        QCOMPARE(codeOf(done), std::string("no_virtual_media"));
        QCOMPARE(saidOf(done), u"Old KVM has no virtual media"_s);
        const base::Result<core::MediaList> offList =
            answerTo<core::MediaList>([&](auto then) { place.askMedia(none, std::move(then)); });
        QCOMPARE(codeOf(offList), std::string("no_virtual_media"));
        // Named otherwise than it is called now: nothing is done.
        done = mount(id, u"Build server"_s);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), u"The confirmation named Build server, and this machine is called NAS now. Nothing was "
                               u"mounted: look at it again."_s);
        QVERIFY(!kvm.msd.connected);
        const base::Result<MediaDone> taken = answerTo<MediaDone>(
            [&](auto then) { place.unmountMedia(id, u"debian.iso"_s, u"Rack"_s, QString(), std::move(then)); });
        QVERIFY(saidOf(taken).contains("Nothing was taken out"_L1));
        const base::Result<MediaDone> gone = answerTo<MediaDone>(
            [&](auto then) { place.deleteMedia(id, u"debian.iso"_s, u"Rack"_s, QString(), std::move(then)); });
        QVERIFY(saidOf(gone).contains("Nothing was deleted"_L1));
        QVERIFY(kvm.msd.images.contains("debian.iso"_L1));
        const base::Result<core::MediaList> list =
            answerTo<core::MediaList>([&](auto then) { place.askMedia(jetkvm, std::move(then)); });
        QCOMPARE(codeOf(list), std::string("no_virtual_media"));
    }

    void aBoxIsBoundToTheKvmItListedAndChangesNothingOfAnother()
    {
        // The wave-1 review, finding 7: a box that listed a KVM is bound to it, by where it is and what it is known
        // by; once that is another (its address edited, or another certificate said yes to), nothing a box sends goes
        // to whatever answers there now, though an image of the same name be there too.
        core::Settings settings(m_folder.filePath(u"bound.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm first;
        withADrive(first);
        MadeUpKvm other;
        withADrive(other);
        const QString id = wall.add(at(first, u"NAS"_s), u"secret"_s).value();
        static_cast<void>(settled(wall, id));
        MediaPlace& place = wall;
        const base::Result<core::MediaList> listed =
            answerTo<core::MediaList>([&](auto then) { place.askMedia(id, std::move(then)); });
        QVERIFY2(listed, qPrintable(saidOf(listed)));
        const QString bound = listed.value().boundTo;
        QVERIFY(!bound.isEmpty());
        QVERIFY(!place.movedSince(id, bound));
        // Another name, a group: the same KVM.
        OwnKvm renamed = wall.kvms().constFirst();
        renamed.name = u"Storage"_s;
        renamed.group = u"Rack 2"_s;
        QVERIFY(wall.change(id, renamed, std::nullopt).ok());
        QVERIFY(!place.movedSince(id, bound));
        // Its address edited to where another KVM is: no longer the one the box listed.
        OwnKvm moved = at(other, u"Storage"_s);
        QVERIFY(wall.change(id, moved, u"secret"_s).ok());
        const std::optional<base::Error> refused = place.movedSince(id, bound);
        QVERIFY(refused.has_value());
        QCOMPARE(refused.value_or(base::Error{}).code, std::string("changed"));
        QVERIFY2(QString::fromStdString(refused.value_or(base::Error{}).message)
                     .startsWith("Storage is not the KVM this box was opened for any more"_L1),
                 refused.value_or(base::Error{}).message.c_str());
        static_cast<void>(settled(wall, id));
        const base::Result<core::MediaList> again =
            answerTo<core::MediaList>([&](auto then) { place.askMedia(id, std::move(then)); });
        QVERIFY2(again, qPrintable(saidOf(again)));
        QVERIFY(again.value().boundTo != bound);
        QVERIFY(!place.movedSince(id, again.value().boundTo));
        // Gone from the wall: no longer either.
        QVERIFY(wall.remove(id).ok());
        QCOMPARE(place.movedSince(id, again.value().boundTo).value_or(base::Error{}).code, std::string("not_found"));
    }

    void oneThingAtATimeAndNothingOfAnImageOnItsWay()
    {
        core::Settings settings(m_folder.filePath(u"three.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quick());
        MadeUpKvm kvm;
        withADrive(kvm);
        kvm.atx = true;
        const QString id = wall.add(at(kvm, u"NAS"_s), u"secret"_s).value();
        static_cast<void>(settled(wall, id));
        MediaPlace& place = wall;
        // While a mount is out, nothing else is done to it: another mount, an action.
        std::optional<base::Result<MediaDone>> first;
        core::testing::Answered slow = core::testing::good();
        slow.afterMs = 800;
        kvm.instead.insert("/api/msd/set_connected", slow);
        place.mountMedia(id, mountOf(u"debian.iso"_s, u"NAS"_s),
                         [&first](base::Result<MediaDone> came) { first = std::move(came); });
        const base::Result<MediaDone> second = answerTo<MediaDone>(
            [&](auto then) { place.mountMedia(id, mountOf(u"debian.iso"_s, u"NAS"_s), std::move(then)); });
        QCOMPARE(codeOf(second), std::string("busy"));
        QCOMPARE(saidOf(second), u"Something else is being done to \"NAS\" at this moment. Wait for it to finish."_s);
        const base::Result<core::Done> pressed = answerTo<core::Done>(
            [&](auto then) { wall.act(id, core::Action::plain(u"atx_reset"_s), std::move(then)); });
        QCOMPARE(codeOf(pressed), std::string("busy"));
        QVERIFY(QTest::qWaitFor([&first] { return first.has_value(); }, 10000));
        kvm.instead.remove("/api/msd/set_connected");
        // An image on its way: nothing of it, and no second upload; the KVM neither changed nor taken off. An action
        // goes meanwhile, as the web lets it.
        std::optional<base::Result<core::MediaUploaded>> carried;
        const std::function<void()> stop = place.uploadMedia(
            id, u"x.iso"_s, u"NAS"_s, QString(), core::ReadAhead::of(std::make_unique<FarAwayFile>(20'000'000, 30)),
            [&carried](base::Result<core::MediaUploaded> came) { carried = std::move(came); });
        QVERIFY(QTest::qWaitFor(
            [&] {
                const base::Result<std::optional<core::MediaUploading>> now =
                    answerTo<std::optional<core::MediaUploading>>(
                        [&](auto then) { place.askUpload(id, std::move(then)); });
                return now && now.value() && now.value()->sent > 0;
            },
            10000));
        const base::Result<MediaDone> onIt = answerTo<MediaDone>(
            [&](auto then) { place.deleteMedia(id, u"x.iso.incomplete"_s, u"NAS"_s, QString(), std::move(then)); });
        QCOMPARE(codeOf(onIt), std::string("busy"));
        QCOMPARE(saidOf(onIt), u"x.iso.incomplete is being uploaded to NAS at this moment. Wait for it to finish."_s);
        const base::Result<core::MediaUploaded> another = answerTo<core::MediaUploaded>([&](auto then) {
            static_cast<void>(place.uploadMedia(id, u"y.iso"_s, u"NAS"_s, QString(),
                                                core::ReadAhead::of(std::make_unique<FarAwayFile>(1000, 0)),
                                                std::move(then)));
        });
        QCOMPARE(codeOf(another), std::string("busy"));
        QCOMPARE(saidOf(another),
                 u"x.iso is being uploaded to NAS at this moment, by this PC. Wait for it to finish."_s);
        QCOMPARE(codeOf(wall.remove(id)), std::string("busy"));
        OwnKvm moved = at(kvm, u"NAS"_s);
        moved.user = u"other"_s;
        QCOMPARE(codeOf(wall.change(id, moved, u"another"_s)), std::string("busy"));
        QVERIFY(answerTo<core::Done>(
            [&](auto then) { wall.act(id, core::Action::plain(u"atx_reset"_s), std::move(then)); }));
        stop();
        QVERIFY(QTest::qWaitFor([&carried] { return carried.has_value(); }, 10000));
        QCOMPARE(codeOf(*carried), std::string(MediaPlace::uploadStopped));
        // Over: it may be taken off now.
        QVERIFY(wall.remove(id));
    }
};

QTEST_MAIN(TestLocalWallMedia)
#include "test_local_wall_media.moc"
