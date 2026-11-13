#include <functional>
#include <utility>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_feed.h>

#include "memory_secrets.h"
#include "pc_source.h"
#include "server_session.h"
#include "session_view.h"
#include "wall_key.h"
#include "wall_model.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::MemorySecrets;

namespace {

// A KVM as this PC's own wall knows one, at an address of this PC where nothing answers.
core::OwnKvm at(const QString& address, const QString& name)
{
    core::OwnKvm kvm;
    kvm.address = address;
    kvm.name = name;
    return kvm;
}

// This PC's own wall, which counts how often it was asked for its list and for how its KVMs are,
// at the moment each is asked.
class CountedWall final : public core::LocalWall {
public:
    using core::LocalWall::LocalWall;

    void askWall(std::function<void(base::Result<core::Wall>)> done) override
    {
        ++wallAsked;
        core::LocalWall::askWall(std::move(done));
    }
    void askStatus(std::function<void(base::Result<QHash<QString, core::Status>>)> done) override
    {
        ++statusAsked;
        core::LocalWall::askStatus(std::move(done));
    }

    int wallAsked = 0;
    int statusAsked = 0;
};

// The key of this PC's KVM on the wall.
QString keyOf(const QString& kvm)
{
    return ui::wallKey(ui::PcSource::idOfThisPc(), kvm).value_or(QString());
}

// The cards of a wall, by their keys, in its order.
QStringList keysOf(ui::WallView& wall)
{
    QStringList keys;
    for (int row = 0; row < wall.machines()->rowCount(); ++row) {
        keys.append(wall.machines()->index(row, 0).data(ui::WallModel::KeyRole).toString());
    }
    return keys;
}

} // namespace

// This PC's own wall as a place the wall's machines come from.
class TestPcSource : public QObject {
    Q_OBJECT

private slots:
    void whileItsWallIsNotShownNothingOfItIsSaidOrToldToItsFeed()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        CountedWall own(settings, secrets);
        const QString one = own.add(at(u"https://127.0.0.1:9"_s, u"Build server"_s), u"secret"_s).value();
        // Its own feed, at a pace far slower than this test. It runs here while its wall is not
        // shown, as it does not in the program (the session starts and stops it with the wall),
        // so that whatever is told to it shows.
        ui::PcSource pc(own, 60'000);
        core::WallFeed& feed = pc.feed();
        const QSignalSpy changed(&pc, &ui::PcSource::kvmsChanged);
        const QSignalSpy looked(&feed, &core::WallFeed::statusChanged);
        feed.start();
        QTRY_COMPARE(feed.machines().size(), 1);
        QTRY_COMPARE(looked.count(), 1);
        QCOMPARE(own.wallAsked, 1);
        QCOMPARE(own.statusAsked, 1);
        QVERIFY(!pc.isAtTheWall());

        // A KVM is added, and the wall says one came to something: none of it is told to the
        // feed, now or a little later, nor said.
        const QString two = own.add(at(u"https://127.0.0.1:19"_s, u"NAS"_s), u"secret"_s).value();
        emit own.cameToSomething();
        QCOMPARE(own.wallAsked, 1);
        QCOMPARE(own.statusAsked, 1);
        QTest::qWait(1200);
        QCOMPARE(own.wallAsked, 1);
        QCOMPARE(own.statusAsked, 1);
        QCOMPARE(feed.machines().size(), 1);
        QCOMPARE(changed.count(), 0);

        // Its wall is shown: what one came to has how they are asked for there and then, and not
        // at the feed's next turn a minute later (nor at the next look of a round of them).
        pc.enter();
        QVERIFY(pc.isAtTheWall());
        emit own.cameToSomething();
        QCOMPARE(own.statusAsked, 2);
        QCOMPARE(own.wallAsked, 1);
        // (Its answer names the KVM that was added meanwhile, and the list is asked for after it.)
        QTRY_COMPARE(feed.machines().size(), 2);
        QCOMPARE(own.wallAsked, 2);

        // A KVM that is added has the list asked for there and then too, and is said.
        const QString three = own.add(at(u"https://127.0.0.1:29"_s, u"Rack"_s), u"secret"_s).value();
        QCOMPARE(own.wallAsked, 3);
        QCOMPARE(changed.count(), 1);
        QTRY_COMPARE(feed.machines().size(), 3);
        QTRY_COMPARE(feed.status().size(), 3);
        // And the machines are looked at again soon after the adding, as after something done to a
        // machine: a KVM that was just added is being signed in to.
        const int now = own.statusAsked;
        QTRY_VERIFY_WITH_TIMEOUT(own.statusAsked > now, 2500);

        // Left again, and its feed stopped with it, as the session has it: one taken off is said to
        // nobody, and nothing is asked for it.
        feed.stop();
        pc.leave();
        const int wallAsked = own.wallAsked;
        QVERIFY(own.remove(three).ok());
        QCOMPARE(changed.count(), 1);
        QVERIFY(own.remove(two).ok());
        QVERIFY(own.remove(one).ok());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(own.wallAsked, wallAsked);
    }

    void itsWallIsEnteredAndLeftAsItSaysItself()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::LocalWall own(settings, secrets);
        ui::PcSource pc(own, 60'000);
        QStringList heard;
        QObject::connect(&pc, &ui::Source::entered, &pc, [&](const core::Account& account) {
            heard.append(u"entered "_s + account.id + (pc.isAtTheWall() ? u", at"_s : u", away"_s));
        });
        QObject::connect(&pc, &ui::Source::left, &pc, [&] { heard.append(u"left "_s + QString::number(pc.turn())); });
        QObject::connect(&pc, &ui::Source::emptied, &pc,
                         [&] { heard.append(u"emptied "_s + QString::number(pc.turn())); });
        QCOMPARE(pc.id(), u"pc"_s);
        QVERIFY(!pc.isAway());
        QVERIFY(pc.manages());
        const quint64 first = pc.turn();

        // Entered by whoever sits here; and left, its turn over before anybody hears of it. Each
        // once: a second enter or leave changes nothing, and says nothing.
        pc.enter();
        pc.enter();
        QVERIFY(pc.mayBeLive());
        pc.leave();
        pc.leave();
        QVERIFY(!pc.mayBeLive());
        const QString after = QString::number(first + 1);
        QCOMPARE(heard, (QStringList{u"entered this-pc, at"_s, u"left "_s + after, u"emptied "_s + after}));
        QCOMPARE(pc.turn(), first + 1);
    }

    void whoeverSitsHereMayDoEverythingAndPlainHttpIsSaid()
    {
        const core::Account whoever = ui::PcSource::whoever();
        QCOMPARE(whoever.id, u"this-pc"_s);
        QCOMPARE(whoever.role, u"admin"_s);
        QVERIFY(!whoever.isDisplay);

        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::LocalWall own(settings, secrets);
        const ui::PcSource pc(own, 60'000);
        // Nothing on the wall: nothing travels anywhere. One over HTTPS: encrypted. One over plain
        // HTTP besides: not, whether its wall is shown or not, for as long as that one is on it.
        QVERIFY(pc.isEncrypted());
        QVERIFY(own.add(at(u"https://127.0.0.1:9"_s, u"Build server"_s), u"secret"_s).ok());
        QVERIFY(pc.isEncrypted());
        const QString plain = own.add(at(u"http://127.0.0.1:29"_s, u"NAS"_s), u"secret"_s).value();
        QVERIFY(!pc.isEncrypted());
        QVERIFY(own.remove(plain).ok());
        QVERIFY(pc.isEncrypted());
    }

    void aSecondWallOfThisPcIsNotTaken()
    {
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        // Two walls of this PC's own, each kept in a file of its own: one KVM over HTTPS on the
        // first, one over plain HTTP on the second.
        core::Settings firstKept(folder.filePath(u"first.json"_s));
        core::Settings secondKept(folder.filePath(u"second.json"_s));
        core::LocalWall first(firstKept, secrets);
        core::LocalWall second(secondKept, secrets);
        const QString one = first.add(at(u"https://127.0.0.1:9"_s, u"Build server"_s), u"secret"_s).value();
        QVERIFY(second.add(at(u"http://127.0.0.1:19"_s, u"NAS"_s), u"secret"_s).ok());
        ui::SessionView view;
        ui::WallView wall;
        ui::ServerSession session(view, wall, settings, secrets, 100);
        session.useOwnWall(first);
        session.useOwnWall(second);
        session.begin();

        // The first is the wall that is shown, and the second is nothing to it: not its KVMs, not
        // how they are spoken to.
        QVERIFY(view.ownWall());
        QTRY_COMPARE(keysOf(wall), QStringList{keyOf(one)});
        QVERIFY(view.encrypted());
        // Nor what changes on it.
        const QSignalSpy shown(&view, &ui::SessionView::changed);
        const QString more = second.add(at(u"https://127.0.0.1:29"_s, u"Rack"_s), u"secret"_s).value();
        QTest::qWait(300);
        QCOMPARE(keysOf(wall), QStringList{keyOf(one)});
        QCOMPARE(shown.count(), 0);
        QVERIFY(second.remove(more).ok());
    }
};

QTEST_GUILESS_MAIN(TestPcSource)
#include "test_pc_source.moc"
