#include <functional>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_feed.h>

#include "memory_secrets.h"
#include "own_kvms_view.h"
#include "pc_source.h"
#include "server_session.h"
#include "session_view.h"
#include "stand_in.h"
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
        ui::PcSource pc(own, 60'000);
        // Its kvmd KVMs' virtual media are had through the wall itself (GAPS A2).
        QCOMPARE(pc.mediaPlace(), static_cast<core::MediaPlace*>(&own));
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

    void aVncServerInTheClearIsSaidOnTheBarAsAKvmOverPlainHttpIs()
    {
        // Outside review c9a-4, M1: the bar of this PC's wall said nothing of a VNC server over vnc://, where its card
        // writes vnc:// before its address and its row in the settings says it is unencrypted: what is seen and typed
        // there can be read on the way, and its VNC answer worked out from what goes. It is said as of a KVM over plain
        // HTTP: with a VNC server in the clear alone, a KVM alone, and both; and of neither over vncs:// or HTTPS. Its
        // card and its row say the same of every KVM of the wall, by the wall's one rule (OwnKvm::isInTheClear). A
        // Proxmox guest, whose API is spoken to over HTTPS and its console by wss:// at that address, is of the
        // encrypted beside each, and said of by none (after the rebase of the Proxmox VE guests).
        QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::Settings sessionsOwn(folder.filePath(u"session.json"_s));
        MemorySecrets secrets;
        core::LocalWall own(settings, secrets);
        ui::PcSource pc(own, 60'000);
        // And the bar as the program has it, of the wall shown in its window.
        ui::SessionView view;
        ui::WallView wall;
        ui::ServerSession session(view, wall, sessionsOwn, secrets, 100);
        session.useOwnWall(own);
        session.begin();
        QVERIFY(view.ownWall());
        QTRY_VERIFY(view.encrypted());
        ui::OwnKvmsView rows;
        rows.use(&own);
        // (At ports where nothing listens, which Windows gives nobody else meanwhile.)
        const core::testing::NobodyThere secureVncsPlace;
        const core::testing::NobodyThere plainVncsPlace;
        const core::testing::NobodyThere secureKvmsPlace;
        const core::testing::NobodyThere plainKvmsPlace;
        const core::testing::NobodyThere guestsPlace;
        const auto added = [&own](core::OwnKvm kvm, const QString& kind) {
            kvm.kind = kind;
            const base::Result<QString> id = own.add(kvm, u"secret"_s);
            return id.ok() ? id.value() : QString();
        };
        // Of every KVM of the wall, in its order: those its card says are spoken to in the clear, and those its row in
        // the settings says so of.
        const auto cardsInTheClear = [&own] {
            std::optional<base::Result<core::Wall>> came;
            own.askWall([&came](base::Result<core::Wall> answer) { came = std::move(answer); });
            static_cast<void>(QTest::qWaitFor([&came] { return came.has_value(); }, 5000));
            QStringList said;
            for (const core::Machine& machine : came && came->ok() ? came->value().machines : QList<core::Machine>()) {
                if (machine.plain) {
                    said.append(machine.name);
                }
            }
            return said;
        };
        const auto rowsInTheClear = [&rows] {
            rows.begin();
            const QHash<int, QByteArray> roles = rows.roleNames();
            QStringList said;
            for (int row = 0; row < rows.rowCount(); ++row) {
                if (rows.index(row, 0).data(roles.key(QByteArray("way"))).toString() == u"plain"_s) {
                    said.append(rows.index(row, 0).data(roles.key(QByteArray("name"))).toString());
                }
            }
            rows.end();
            return said;
        };

        // Over vncs:// and HTTPS: encrypted, and said of none.
        const QString secureVnc =
            added(at(u"vncs://127.0.0.1:%1"_s.arg(secureVncsPlace.port()), u"Secure VNC"_s), u"vnc"_s);
        const QString secureKvm =
            added(at(u"https://127.0.0.1:%1"_s.arg(secureKvmsPlace.port()), u"Secure KVM"_s), u"kvmd"_s);
        // And a Proxmox guest, added as its box adds one (its token's ID as its user, its secret kept).
        core::OwnKvm guest = at(u"https://127.0.0.1:%1"_s.arg(guestsPlace.port()), u"Guest"_s);
        guest.kind = u"proxmox"_s;
        guest.node = u"pve1"_s;
        guest.vmid = 100;
        guest.user = u"qkvm@pve!wall"_s;
        const base::Result<QString> aGuest = own.add(guest, u"6c1d0e2f-8a3b-4c5d-9e7f-0a1b2c3d4e5f"_s);
        QVERIFY(aGuest.ok());
        QCOMPARE(own.kvms().size(), 3);
        QVERIFY(!secureVnc.isEmpty() && !secureKvm.isEmpty());
        QVERIFY(pc.isEncrypted());
        QVERIFY(view.encrypted());
        QCOMPARE(cardsInTheClear(), QStringList());
        QCOMPARE(rowsInTheClear(), QStringList());
        // A VNC server over vnc://, alone in the clear.
        const QString plainVnc =
            added(at(u"vnc://127.0.0.1:%1"_s.arg(plainVncsPlace.port()), u"Plain VNC"_s), u"vnc"_s);
        QVERIFY(!plainVnc.isEmpty());
        QVERIFY(!pc.isEncrypted());
        QTRY_VERIFY(!view.encrypted());
        QCOMPARE(cardsInTheClear(), QStringList{u"Plain VNC"_s});
        QCOMPARE(rowsInTheClear(), QStringList{u"Plain VNC"_s});
        // And a KVM over plain HTTP beside it: both.
        const QString plainKvm =
            added(at(u"http://127.0.0.1:%1"_s.arg(plainKvmsPlace.port()), u"Plain KVM"_s), u"kvmd"_s);
        QVERIFY(!plainKvm.isEmpty());
        QVERIFY(!pc.isEncrypted());
        QCOMPARE(cardsInTheClear(), (QStringList{u"Plain VNC"_s, u"Plain KVM"_s}));
        QCOMPARE(rowsInTheClear(), (QStringList{u"Plain VNC"_s, u"Plain KVM"_s}));
        // The KVM alone.
        QVERIFY(own.remove(plainVnc).ok());
        QVERIFY(!pc.isEncrypted());
        QTest::qWait(300);
        QVERIFY(!view.encrypted());
        QCOMPARE(cardsInTheClear(), QStringList{u"Plain KVM"_s});
        QCOMPARE(rowsInTheClear(), QStringList{u"Plain KVM"_s});
        // And none: encrypted again.
        QVERIFY(own.remove(plainKvm).ok());
        QVERIFY(pc.isEncrypted());
        QTRY_VERIFY(view.encrypted());
        QCOMPARE(cardsInTheClear(), QStringList());
        QCOMPARE(rowsInTheClear(), QStringList());
        // (Whatever else an address may come to have, only https:// and vncs:// are encrypted, as the web's row in its
        // settings has it: /^(https|vncs):/.)
        for (const QString& address : {u"https://box"_s, u"vncs://box"_s}) {
            QVERIFY2(!at(address, {}).isInTheClear(), qPrintable(address));
        }
        QVERIFY(!guest.isInTheClear());
        for (const QString& address : {u"http://box"_s, u"vnc://box"_s, u"ftp://box"_s, u"box"_s, QString()}) {
            QVERIFY2(at(address, {}).isInTheClear(), qPrintable(address));
        }
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
