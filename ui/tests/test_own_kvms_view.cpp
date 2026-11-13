#include <algorithm>
#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/settings.h>

#include "memory_secrets.h"
#include "own_kvms_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::MemorySecrets;

namespace {

// The times of a KVM's driver, made short. The KVMs of these tests are at ports of this PC's own
// address where nothing answers: a driver that looks is refused at once, and nothing else is asked.
core::KvmdDevice::Pace quick()
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
    return pace;
}

// A certificate's SHA-256, made up: what a KVM is known by in these tests.
QByteArray certificate(char letter)
{
    return QByteArray(32, letter);
}

// Secrets in memory that count how often each was read: how often the store was asked for a KVM's.
class CountedSecrets final : public core::SecretStore {
public:
    base::Result<void> keep(const QString& name, const QByteArray& secret) override { return all.keep(name, secret); }
    QByteArray kept(const QString& name) override
    {
        ++reads[name];
        return all.kept(name);
    }
    void forget(const QString& name) override { all.forget(name); }

    // How often the secret of a KVM was read, by the wall's name for it.
    int readsOf(const QString& id) const { return reads.value(core::LocalWall::secretOf(id)); }
    int readsInAll() const
    {
        int sum = 0;
        for (const int one : reads) {
            sum += one;
        }
        return sum;
    }

    MemorySecrets all;
    QHash<QString, int> reads;
};

// This PC's own wall for one test: its settings in a folder that goes with the test, its secrets in
// memory.
struct Held {
    Held() : settings(folder.filePath(u"settings.json"_s)), wall(settings, secrets, quick()) {}

    // Puts a KVM on the wall as one that was added by hand, with a password (none: empty); and gives
    // the wall's name for it. `known`: by the certificate of this letter; 0: by none.
    QString put(const QString& address, const QString& name, char known = 0,
                const QString& password = QStringLiteral("secret"))
    {
        core::OwnKvm kvm;
        kvm.address = address;
        kvm.name = name;
        kvm.knownByCertificate = known != 0;
        kvm.certificate = known != 0 ? certificate(known) : QByteArray();
        const base::Result<QString> id = wall.add(kvm, password);
        return id ? id.value() : QString();
    }
    core::OwnKvm kvm(const QString& id) const
    {
        const QList<core::OwnKvm>& all = wall.kvms();
        const auto found = std::ranges::find_if(all, [&id](const core::OwnKvm& one) { return one.id == id; });
        return found == all.end() ? core::OwnKvm{} : *found;
    }

    QTemporaryDir folder;
    core::Settings settings;
    CountedSecrets secrets;
    core::LocalWall wall;
};

// What a row of the view says, by its role's name.
QVariant at(const ui::OwnKvmsView& view, int row, const char* role)
{
    const QHash<int, QByteArray> roles = view.roleNames();
    const int which = roles.key(QByteArray(role), -1);
    return which < 0 ? QVariant() : view.index(row, 0).data(which);
}

// The wall's names of the KVMs of the rows, in their order.
QStringList keysOf(const ui::OwnKvmsView& view)
{
    QStringList keys;
    for (int row = 0; row < view.rowCount(); ++row) {
        keys.append(at(view, row, "key").toString());
    }
    return keys;
}

} // namespace

class TestOwnKvmsView : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void eachKvmOfTheWallIsARowOfWhatItIsCalledWhereItIsAndHowItIsSpokenTo()
    {
        Held held;
        const QString rack = held.put(u"https://127.0.0.1:20101"_s, u"Rack 1"_s, 'a');
        const QString unnamed = held.put(u"https://127.0.0.1:20102"_s, QString());
        // One was added without a password.
        const QString nas = held.put(u"http://127.0.0.1:20103"_s, u"NAS"_s, 0, QString());
        // The same device as the first: known by the certificate that one is known by.
        const QString twin = held.put(u"https://127.0.0.1:20104"_s, u"Rack 1 again"_s, 'a');
        QVERIFY(!rack.isEmpty() && !unnamed.isEmpty() && !nas.isEmpty() && !twin.isEmpty());
        ui::OwnKvmsView view;
        view.use(&held.wall);
        // Not shown: nothing is listed, and nothing is read.
        QCOMPARE(view.count(), 0);
        QCOMPARE(held.secrets.readsInAll(), 0);

        view.begin();
        QCOMPARE(keysOf(view), (QStringList{rack, unnamed, nas, twin}));
        QCOMPARE(view.count(), 4);
        // What it is called: its name; and where it is, where it has none.
        QCOMPARE(at(view, 0, "name").toString(), u"Rack 1"_s);
        QCOMPARE(at(view, 1, "name").toString(), u"127.0.0.1:20102"_s);
        QCOMPARE(at(view, 2, "name").toString(), u"NAS"_s);
        // Where it is, as its card says it.
        QCOMPARE(at(view, 0, "host").toString(), u"127.0.0.1:20101"_s);
        QCOMPARE(at(view, 2, "host").toString(), u"127.0.0.1:20103"_s);
        // How it is spoken to: known by its certificate, by Windows' word, in the clear.
        QCOMPARE(at(view, 0, "way").toString(), u"certificate"_s);
        QCOMPARE(at(view, 1, "way").toString(), u"windows"_s);
        QCOMPARE(at(view, 2, "way").toString(), u"plain"_s);
        QCOMPARE(at(view, 3, "way").toString(), u"certificate"_s);
        // The two that are one device each say which other it is, as a sentence calls it; the
        // others are the same device as none.
        QCOMPARE(at(view, 0, "twins").toString(), u"\"Rack 1 again\""_s);
        QCOMPARE(at(view, 3, "twins").toString(), u"\"Rack 1\""_s);
        QCOMPARE(at(view, 1, "twins").toString(), QString());
        QCOMPARE(at(view, 2, "twins").toString(), QString());
        // Whether a password is kept is in each row from the start, as the wall's mark says it.
        QVERIFY(at(view, 2, "noPassword").toBool());
        for (const int row : {0, 1, 3}) {
            QVERIFY(!at(view, row, "noPassword").toBool());
        }
        for (int row = 0; row < 4; ++row) {
            QVERIFY(!at(view, row, "gone").toBool());
        }
        view.end();
    }

    void theListHoldsStillWhileItIsShownAndIsTheWallsAsItIsWhenShownAnew()
    {
        Held held;
        const QString one = held.put(u"http://127.0.0.1:20111"_s, u"One"_s);
        const QString two = held.put(u"https://127.0.0.1:20112"_s, u"Two"_s, 'b');
        const QString three = held.put(u"https://127.0.0.1:20113"_s, u"Three"_s, 'b');
        ui::OwnKvmsView view;
        view.use(&held.wall);
        view.begin();
        QCOMPARE(keysOf(view), (QStringList{one, two, three}));
        QCOMPARE(at(view, 1, "twins").toString(), u"\"Three\""_s);
        const QSignalSpy removed(&view, &QAbstractItemModel::rowsRemoved);
        const QSignalSpy moved(&view, &QAbstractItemModel::rowsMoved);
        const QSignalSpy reset(&view, &QAbstractItemModel::modelReset);
        const QSignalSpy laidOut(&view, &QAbstractItemModel::layoutChanged);
        const QSignalSpy inserted(&view, &QAbstractItemModel::rowsInserted);
        const QSignalSpy changedRows(&view, &QAbstractItemModel::dataChanged);

        // A KVM is taken off: its row stays, and says so. Nothing moves up into its place.
        QVERIFY(held.wall.remove(two).ok());
        QCOMPARE(keysOf(view), (QStringList{one, two, three}));
        QVERIFY(at(view, 1, "gone").toBool());
        QVERIFY(!at(view, 0, "gone").toBool());
        QVERIFY(!at(view, 2, "gone").toBool());
        // (Nor is the one that was its twin the same device as anything any more.)
        QCOMPARE(at(view, 2, "twins").toString(), QString());
        // A KVM is added: last.
        const QString four = held.put(u"http://127.0.0.1:20114"_s, u"Four"_s);
        QCOMPARE(keysOf(view), (QStringList{one, two, three, four}));
        QCOMPARE(inserted.count(), 1);
        // One is renamed: in its row.
        core::OwnKvm renamed = held.kvm(one);
        renamed.name = u"First"_s;
        QVERIFY(held.wall.change(one, renamed, std::nullopt).ok());
        QCOMPARE(at(view, 0, "name").toString(), u"First"_s);
        QCOMPARE(keysOf(view), (QStringList{one, two, three, four}));
        // The wall's order is another: the rows stay where they are.
        bool arranged = false;
        held.wall.arrange({four, three, one}, [&arranged](const base::Result<QStringList>&) { arranged = true; });
        QTRY_VERIFY(arranged);
        QCOMPARE(keysOf(view), (QStringList{one, two, three, four}));
        QCOMPARE(removed.count(), 0);
        QCOMPARE(moved.count(), 0);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(laidOut.count(), 0);
        QVERIFY(!changedRows.isEmpty());
        QCOMPARE(view.count(), 4);

        // While a second box shows it as well, and after the first one no longer does, it holds.
        view.begin();
        view.end();
        QVERIFY(view.isShown());
        QCOMPARE(keysOf(view), (QStringList{one, two, three, four}));
        // Shown anew after no box showed it: the wall as it is, in its order, and nothing that is
        // not on it.
        view.end();
        QVERIFY(!view.isShown());
        view.begin();
        QCOMPARE(keysOf(view), (QStringList{four, three, one}));
        for (int row = 0; row < view.rowCount(); ++row) {
            QVERIFY(!at(view, row, "gone").toBool());
        }
        QCOMPARE(at(view, 2, "name").toString(), u"First"_s);
        view.end();
    }

    void whetherAPasswordIsKeptIsTheWallsMarkOrWhatItsDriverFound()
    {
        Held held;
        // (Over HTTPS: a KVM that was asked what it is in an earlier run has a driver from the
        // start, which looks.)
        const QString one = held.put(u"https://127.0.0.1:20121"_s, u"One"_s, 'b');
        const QString two = held.put(u"https://127.0.0.1:20122"_s, u"Two"_s, 'c', QString());
        ui::OwnKvmsView view;
        view.use(&held.wall);

        // Shown: by the wall's mark, in each row from the start.
        view.begin();
        QVERIFY(!at(view, 0, "noPassword").toBool());
        QVERIFY(at(view, 1, "noPassword").toBool());
        // A password is given for the second elsewhere: the mark says so, and so does its row.
        QVERIFY(held.wall.change(two, held.kvm(two), u"secret"_s).ok());
        QVERIFY(!at(view, 1, "noPassword").toBool());
        // A KVM is added: its row has it from the start.
        const QString three = held.put(u"http://127.0.0.1:20123"_s, u"Three"_s, 0, QString());
        QCOMPARE(keysOf(view), (QStringList{one, two, three}));
        QVERIFY(at(view, 2, "noPassword").toBool());
        view.end();

        // Windows loses the first one's password, behind the wall's back, and the program is started
        // again: the mark says that one is kept. The wall's driver finds none when it looks, and that
        // wins: its row says so once the wall has said that the look ended, while the list is shown.
        held.secrets.all.all.remove(core::LocalWall::secretOf(one));
        core::Settings settings(held.folder.filePath(u"settings.json"_s));
        core::LocalWall wall(settings, held.secrets, quick());
        QVERIFY(wall.marksAPasswordFor(one));
        ui::OwnKvmsView again;
        again.use(&wall);
        again.begin();
        QVERIFY(!at(again, 0, "noPassword").toBool());
        QVERIFY(!at(again, 1, "noPassword").toBool());
        QVERIFY(at(again, 2, "noPassword").toBool());
        QSignalSpy cameToSomething(&wall, &core::LocalWall::cameToSomething);
        wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
        QVERIFY(cameToSomething.wait(5000));
        QTRY_VERIFY(at(again, 0, "noPassword").toBool());
        QVERIFY(wall.foundWithoutAPassword().contains(one));
        QVERIFY(!at(again, 1, "noPassword").toBool());
        // (Of a row that says it from the start, nothing changes.)
        QVERIFY(at(again, 2, "noPassword").toBool());
        // Its password given anew: the driver that finds none is replaced, and the row says nothing.
        QVERIFY(wall.change(one, wall.kvms().constFirst(), u"secret"_s).ok());
        QVERIFY(!at(again, 0, "noPassword").toBool());
        again.end();
    }

    void whetherAPasswordIsKeptIsWhatTheWallSaysOfItsKvmAskedYetOrNot()
    {
        Held held;
        const QString kept = held.put(u"https://127.0.0.1:20131"_s, u"Kept"_s, 'c');
        // One added without a password.
        const QString none = held.put(u"https://127.0.0.1:20132"_s, u"None kept"_s, 'd', QString());
        // Two taken from a file, and not asked yet what they are: one came without a password.
        core::LocalWall::FromAFile without;
        without.kvm.address = u"http://127.0.0.1:20133"_s;
        without.kvm.name = u"From a file"_s;
        core::LocalWall::FromAFile with = without;
        with.kvm.address = u"http://127.0.0.1:20134"_s;
        with.password = u"from the file"_s;
        const QList<base::Result<QString>> taken = held.wall.takeFromAFile({without, with});
        QVERIFY(taken.size() == 2 && taken.at(0).ok() && taken.at(1).ok());
        // The next start of the program, with what is remembered as it is.
        core::Settings settings(held.folder.filePath(u"settings.json"_s));
        core::LocalWall wall(settings, held.secrets, quick());
        QCOMPARE(wall.kvms().size(), 4);
        ui::OwnKvmsView view;
        view.use(&wall);
        view.begin();
        QCOMPARE(keysOf(view), (QStringList{kept, none, taken.at(0).value(), taken.at(1).value()}));
        // At once, and whether it was asked what it is or not.
        QVERIFY(!at(view, 0, "noPassword").toBool());
        QVERIFY(at(view, 1, "noPassword").toBool());
        QVERIFY(at(view, 2, "noPassword").toBool());
        QVERIFY(!at(view, 3, "noPassword").toBool());
        QVERIFY(!wall.wasAskedWhatItIs(taken.at(0).value()));
        QVERIFY(!wall.wasAskedWhatItIs(taken.at(1).value()));
        view.end();
    }

    void theListReadsNoPassword()
    {
        // Of a KVM added by hand, one whose password Windows lost, and one taken from a file that
        // was not asked yet what it is: the store counts every reading of each.
        Held held;
        const QString one = held.put(u"http://127.0.0.1:20151"_s, u"One"_s);
        const QString two = held.put(u"https://127.0.0.1:20152"_s, u"Two"_s, 'e');
        held.secrets.all.all.remove(core::LocalWall::secretOf(two));
        core::LocalWall::FromAFile fromAFile;
        fromAFile.kvm.address = u"http://127.0.0.1:20153"_s;
        fromAFile.password = u"from the file"_s;
        QCOMPARE(held.wall.takeFromAFile({fromAFile}).size(), 1);
        ui::OwnKvmsView view;
        view.use(&held.wall);
        held.secrets.reads.clear();

        // Shown, shown by a second box as well, and shown anew: nothing is read.
        view.begin();
        view.begin();
        view.end();
        view.end();
        view.begin();
        QCOMPARE(view.count(), 3);
        QCOMPARE(held.secrets.readsInAll(), 0);

        // What the wall reads for a change of its own is the change's: the list reads nothing beside
        // it. (Each change once with the list not shown, and once more with it shown.)
        const auto readsFor = [&held](const std::function<void()>& change) {
            held.secrets.reads.clear();
            change();
            return held.secrets.readsInAll();
        };
        const auto renamed = [&held, one](const QString& name) {
            core::OwnKvm kvm = held.kvm(one);
            kvm.name = name;
            QVERIFY(held.wall.change(one, kvm, std::nullopt).ok());
        };
        const auto given = [&held, two](const QString& password) {
            QVERIFY(held.wall.change(two, held.kvm(two), password).ok());
        };
        view.end();
        const int byARename = readsFor([&] { renamed(u"First"_s); });
        const int byAPassword = readsFor([&] { given(u"secret"_s); });
        view.begin();
        QCOMPARE(readsFor([&] { renamed(u"One"_s); }), byARename);
        QCOMPARE(readsFor([&] { given(u"another"_s); }), byAPassword);
        QCOMPARE(readsFor([&] { QVERIFY(!held.put(u"http://127.0.0.1:20154"_s, u"Four"_s).isEmpty()); }), 0);
        QCOMPARE(readsFor([&] { QVERIFY(held.wall.remove(one).ok()); }), 0);
        // Nor when the wall says by itself that its KVMs came to something, once their drivers
        // have looked (which reads nothing either).
        QSignalSpy cameToSomething(&held.wall, &core::LocalWall::cameToSomething);
        held.secrets.reads.clear();
        held.wall.askStatus([](const base::Result<QHash<QString, core::Status>>&) {});
        QVERIFY(cameToSomething.wait(5000));
        QTest::qWait(50);
        QCOMPARE(held.secrets.readsInAll(), 0);
        view.end();
    }

    void eachRowSaysTheFamilyOfItsKvm()
    {
        Held held;
        const QString kvmd = held.put(u"https://127.0.0.1:20701"_s, u"Rack"_s, 'a');
        core::OwnKvm jet;
        jet.address = u"http://127.0.0.1:20702"_s;
        jet.name = u"Desk"_s;
        jet.kind = u"jetkvm"_s;
        const base::Result<QString> added = held.wall.add(jet, QString());
        QVERIFY2(added.ok(), added.ok() ? "" : added.error().message.c_str());
        ui::OwnKvmsView view;
        view.use(&held.wall);
        view.begin();
        QCOMPARE(keysOf(view), (QStringList{kvmd, added.value()}));
        QCOMPARE(at(view, 0, "family").toString(), u"kvmd"_s);
        QCOMPARE(at(view, 1, "family").toString(), u"jetkvm"_s);
        // (One out of its box runs without a password: none is kept, and the list says so, as the
        // wall's mark has it; how that is shown is the screen's.)
        QVERIFY(at(view, 1, "noPassword").toBool());
    }

    void withNoWallTheListIsEmptyAndAsksNothing()
    {
        ui::OwnKvmsView view;
        view.begin();
        QCOMPARE(view.count(), 0);
        view.end();
        // A wall that goes: the list does not reach for it.
        auto held = std::make_unique<Held>();
        QVERIFY(!held->put(u"http://127.0.0.1:20141"_s, u"One"_s).isEmpty());
        view.use(&held->wall);
        view.begin();
        QCOMPARE(view.count(), 1);
        view.end();
        held.reset();
        view.begin();
        QCOMPARE(view.count(), 0);
        view.end();
        // The roles a screen reads, by their names.
        const QList<QByteArray> names = view.roleNames().values();
        for (const char* role : {"key", "name", "host", "way", "twins", "noPassword", "gone"}) {
            QVERIFY2(names.contains(QByteArray(role)), role);
        }
    }
};

QTEST_GUILESS_MAIN(TestOwnKvmsView)
#include "test_own_kvms_view.moc"
