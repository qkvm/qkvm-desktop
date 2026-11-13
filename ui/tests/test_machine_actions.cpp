#include <algorithm>
#include <functional>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/wall_feed.h>

#include "actions_view.h"
#include "keys_by_hand.h"
#include "machine_actions.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::StandIn;

namespace {

class HeldLive final : public core::Live {
public:
    bool isOpen() const override { return true; }
    void setNeed(Need) override {}
    void askForAKeyFrame() override {}
    void askControl(bool on) override { requests.append(on); }
    bool hasControl() const override { return held; }
    void key(int usage, bool down) override { keys.append({usage, down}); }
    void pointerAt(int, int) override {}
    void pointerBy(int, int) override {}
    void button(int, bool) override {}
    void wheel(int, int) override {}
    void releaseAll() override {}
    void close() override {}
    void answer(bool on)
    {
        held = on;
        emit controlChanged(on, {}, {});
    }
    bool held = false;
    QList<bool> requests;
    // Each key it was told of, by its USB number, and whether it went down.
    QList<std::pair<int, bool>> keys;
};

// A QKVM for these tests: a wall of a KVM with three computers behind it and a power board, and of
// a server. It does what it is asked, or what a test tells it to answer, and remembers what it was
// asked to do.
struct Pretended {
    // Everything that was asked to be done, in the order it came: the action as it was sent, and
    // under "machine" to which machine.
    QList<QJsonObject> acted;
    // What is answered to that; without one, that it was done.
    std::function<Answered(const QJsonObject& action)> onAction;
    QJsonArray saved{QJsonObject{{u"id"_s, u"a1"_s},
                                 {u"name"_s, u"Lock it"_s},
                                 {u"kind"_s, u"keys"_s},
                                 {u"keys"_s, u"MetaLeft,KeyL"_s},
                                 {u"enter"_s, false},
                                 {u"danger"_s, false},
                                 {u"changed"_s, 1}}};
    QJsonArray wake{QJsonObject{{u"mac"_s, u"00:11:22:aa:bb:cc"_s}, {u"name"_s, u"Backup box"_s}}};
    bool serverIsOn = true;
    bool kvmIsThere = true;
    std::optional<QStringList> capabilities;
    std::optional<QStringList> serverCapabilities;
    // More KVMs on the wall, after the server: "n1", "n2", … called "Node 1", "Node 2", …, each
    // with one computer and no power board.
    int more = 0;
    int savedAsked = 0;
    int wakeAsked = 0;
    int statusAsked = 0;
    int mediaAsked = 0;
    // How often a pass into a console was asked for, and what is answered to that; without one, a
    // pass into the server's console.
    int consoleAsked = 0;
    std::function<Answered()> onConsole;
    // A PC with Intel AMT on the wall too, last: "amt", called "Front desk", in this view; every view
    // it was asked to be put in; and what is answered to that (without one, that it is in it now).
    bool amtIsThere = false;
    QString amtView = u"stills"_s;
    // Whether its server says its view at all (0.2.2 says none: no `amt_view`, no `amt.view`).
    bool amtViewSaid = true;
    QStringList viewsAsked;
    std::function<Answered()> onView;

    QJsonObject wall() const
    {
        QJsonArray devices;
        if (kvmIsThere) {
            devices.append(QJsonObject{{u"id"_s, u"kvm"_s},
                                       {u"name"_s, u"NAS"_s},
                                       {u"host"_s, u"192.168.1.22"_s},
                                       {u"kind"_s, u"kvmd"_s},
                                       {u"open_url"_s, u"http://192.168.1.22/"_s},
                                       {u"picture"_s, true}});
        }
        devices.append(QJsonObject{{u"id"_s, u"srv"_s},
                                   {u"name"_s, u"Rack server"_s},
                                   {u"host"_s, u"192.168.1.60"_s},
                                   {u"kind"_s, u"redfish"_s},
                                   {u"open_url"_s, u"https://192.168.1.60/"_s},
                                   {u"picture"_s, false}});
        for (int node = 1; node <= more; ++node) {
            devices.append(QJsonObject{{u"id"_s, u"n%1"_s.arg(node)},
                                       {u"name"_s, u"Node %1"_s.arg(node)},
                                       {u"host"_s, u"192.168.1.%1"_s.arg(30 + node)},
                                       {u"kind"_s, u"kvmd"_s},
                                       {u"open_url"_s, u"http://192.168.1.%1/"_s.arg(30 + node)},
                                       {u"picture"_s, true}});
        }
        if (amtIsThere) {
            QJsonObject pc{{u"id"_s, u"amt"_s},
                           {u"name"_s, u"Front desk"_s},
                           {u"host"_s, u"192.168.1.80:16993"_s},
                           {u"kind"_s, u"amt"_s},
                           {u"picture"_s, false}};
            if (amtViewSaid) {
                pc[u"amt_view"_s] = amtView;
            }
            devices.append(pc);
        }
        return {{u"interval"_s, 60}, {u"devices"_s, devices}};
    }

    QJsonObject status() const
    {
        QJsonObject all;
        if (kvmIsThere) {
            all[u"kvm"_s] = QJsonObject{
                {u"state"_s, u"online"_s},
                {u"atx"_s, QJsonObject{{u"enabled"_s, true}, {u"power"_s, true}}},
                {u"ports"_s, QJsonObject{{u"active"_s, 0},
                                         {u"list"_s, QJsonArray{QJsonObject{{u"name"_s, QString()}},
                                                                QJsonObject{{u"name"_s, u"Build box"_s}},
                                                                QJsonObject{{u"name"_s, QString()}}}}}},
            };
        }
        all[u"srv"_s] = QJsonObject{
            {u"state"_s, serverIsOn ? u"online"_s : u"no_signal"_s},
            {u"picture"_s, false},
            {u"console"_s, true},
            {u"atx"_s, QJsonObject{{u"enabled"_s, true}, {u"power"_s, serverIsOn}}},
            {u"server"_s, QJsonObject{{u"health"_s, u"OK"_s}, {u"locator"_s, false}}},
        };
        if (serverCapabilities) {
            auto server = all[u"srv"_s].toObject();
            server[u"can"_s] = QJsonArray::fromStringList(*serverCapabilities);
            all[u"srv"_s] = server;
        }
        if (capabilities && kvmIsThere) {
            auto kvm = all[u"kvm"_s].toObject();
            kvm[u"can"_s] = QJsonArray::fromStringList(*capabilities);
            all[u"kvm"_s] = kvm;
        }
        for (int node = 1; node <= more; ++node) {
            all[u"n%1"_s.arg(node)] = QJsonObject{{u"state"_s, u"online"_s}};
        }
        if (amtIsThere) {
            all[u"amt"_s] = QJsonObject{
                {u"state"_s, u"online"_s},
                {u"picture"_s, false},
                {u"screen"_s, amtView != u"health"_s},
                {u"server"_s, QJsonObject{{u"health"_s, QJsonValue()},
                                          {u"amt"_s, amtViewSaid ? QJsonObject{{u"view"_s, amtView}} : QJsonObject{}}}},
            };
        }
        return all;
    }

    static Answered json(const QJsonObject& body) { return {200, QJsonDocument(body).toJson(QJsonDocument::Compact)}; }

    Answered answer(const Asked& asked)
    {
        // What a QKVM says of itself: asked before anything else goes to it after a call that
        // got no answer.
        if (asked.path == "/healthz") {
            return {200, R"({"status": "ok", "version": "0.1.0", "api": ["v1"]})"};
        }
        if (asked.path == "/api/v1/wall") {
            return json(wall());
        }
        if (asked.path == "/api/v1/status") {
            ++statusAsked;
            return json(status());
        }
        if (asked.path == "/api/v1/actions") {
            ++savedAsked;
            return json({{u"actions"_s, saved}});
        }
        if (asked.path.endsWith("/wol")) {
            ++wakeAsked;
            return json({{u"targets"_s, wake}});
        }
        if (asked.method == "PUT" && asked.path == "/api/v1/devices/amt/view") {
            const QString view = QJsonDocument::fromJson(asked.body).object().value(u"view"_s).toString();
            viewsAsked.append(view);
            if (onView) {
                return onView();
            }
            amtView = view;
            return json({{u"ok"_s, true}, {u"amt_view"_s, view}});
        }
        if (asked.method == "GET" && asked.path.endsWith("/media")) {
            ++mediaAsked;
            return {200, R"({"images": [], "offers": {"drives": ["cd"], "where": "device"}, "mounted": null})"};
        }
        if (asked.method == "POST" && asked.path.endsWith("/console")) {
            ++consoleAsked;
            return onConsole ? onConsole() : json({{u"url"_s, u"https://192.168.1.60/console?pass=one-use"_s}});
        }
        if (asked.method == "POST" && asked.path.endsWith("/actions")) {
            QJsonObject action = QJsonDocument::fromJson(asked.body).object();
            action[u"machine"_s] = QString::fromLatin1(asked.path.split('/').value(4));
            acted.append(action);
            return onAction ? onAction(action) : json({{u"ok"_s, true}, {u"note"_s, QJsonValue()}});
        }
        return {404, QByteArray("{\"code\": \"not_found\", \"message\": \"Unknown\"}")};
    }
};

// What stands on a desk for these tests: the stand-in server, a client and a feed that speak to it,
// the views, and what does things to machines. Somebody who may act is at the wall, and the wall
// has its machines and how they are.
struct Desk {
    explicit Desk(Pretended& pretended, const QString& role = u"operator"_s, int statusEveryMs = 60'000)
        : server([&pretended](const Asked& asked) { return pretended.answer(asked); }), feed(client, statusEveryMs),
          place(client, feed), actions(view, wall, place, underWay),
          machines(2 + pretended.more + (pretended.amtIsThere ? 1 : 0))
    {
        QObject::connect(view.notices(), &QAbstractItemModel::rowsInserted, &view,
                         [this](const QModelIndex&, int first, int) {
                             const QModelIndex notice = view.notices()->index(first, 0);
                             told.append(notice.data(ui::Notices::KindRole).toString() + u": "_s +
                                         notice.data(ui::Notices::TextRole).toString());
                         });
        // A browser is never opened by a test: what would have been opened is noted.
        actions.openPagesWith([this](const QUrl& address) {
            opened.append(address);
            return opens;
        });
        static_cast<void>(client.setAddress(server.address()));
        feed.start();
        core::Account account;
        account.id = u"u1"_s;
        account.name = u"anna"_s;
        account.role = role;
        actions.enter(account);
    }

    bool isReady() const { return feed.machines().size() == machines && feed.status().size() == machines; }

    // Picks these machines on the wall, and no others, as whoever sits here would: the wall has a
    // card for each machine the server named.
    void pick(const QStringList& ids)
    {
        QList<ui::WallTile> cards;
        for (const core::Machine& machine : feed.machines()) {
            ui::WallTile card;
            card.key = machine.id;
            card.name = machine.name;
            // (Whoever fills a wall says of each card that it may be acted on: none may be
            // picked otherwise.)
            card.actable = true;
            cards.append(card);
        }
        wall.machines()->setTiles(cards);
        wall.setSelecting(true);
        wall.pickNone();
        for (const QString& id : ids) {
            wall.togglePicked(id);
        }
    }

    // Of the run that is shown: a word of it, and its machines, each as "name: state: text".
    QString run(const QString& what) const { return view.run().value(what).toString(); }
    bool runIsOver() const { return !view.run().isEmpty() && !view.run().value(u"running"_s).toBool(); }
    QStringList runRows() const
    {
        const QVariantList rows = view.run().value(u"rows"_s).toList();
        QStringList said;
        for (const QVariant& row : rows) {
            const QVariantMap one = row.toMap();
            said.append(one.value(u"name"_s).toString() + u": "_s + one.value(u"state"_s).toString() + u": "_s +
                        one.value(u"text"_s).toString());
        }
        return said;
    }

    // Whether the open menu has a row of this id.
    bool offers(const QString& row) const
    {
        const QVariantList sections = view.menu().value(u"sections"_s).toList();
        for (const QVariant& section : sections) {
            const QVariantList rows = section.toMap().value(u"rows"_s).toList();
            for (const QVariant& one : rows) {
                if (one.toMap().value(u"id"_s).toString() == row) {
                    return true;
                }
            }
        }
        return false;
    }

    StandIn server;
    core::ServerClient client;
    core::WallFeed feed;
    // The machines of the wall, as what does things to them asks for them: the server's.
    ui::OnePlace place;
    ui::WallView wall;
    ui::ActionsView view;
    // What is under way on which machine: this window's actions write it, and any other's would.
    ui::UnderWay underWay;
    ui::MachineActions actions;
    // Every notice that was told, as "kind: text", in the order they came.
    QStringList told;
    // Every address a browser would have been sent to, and whether one "opens" at all.
    QList<QUrl> opened;
    bool opens = true;
    // How many machines the wall has once it has them all.
    qsizetype machines;
};

// To which machines something was sent, whatever the order it arrived in.
QSet<QString> machinesOf(const QList<QJsonObject>& acted)
{
    QSet<QString> machines;
    for (const QJsonObject& action : acted) {
        machines.insert(action[u"machine"_s].toString());
    }
    return machines;
}

Answered late(int afterMs)
{
    Answered answer{200, QByteArray("{\"ok\": true, \"note\": null}")};
    answer.afterMs = afterMs;
    return answer;
}

// The times of a KVM's driver, for a test: none of them long.
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

// What a made-up KVM is known by on this PC's own wall.
core::OwnKvm known(const MadeUpKvm& kvm, const QString& name)
{
    core::OwnKvm own;
    own.address = kvm.stand->address();
    own.name = name;
    return own;
}

// A desk at this PC's own wall, with no server anywhere: the wall of the KVMs that were added in the
// program, a feed that asks it, the views, and what does things to machines, which asks it too.
struct OwnDesk {
    OwnDesk()
    {
        QObject::connect(view.notices(), &QAbstractItemModel::rowsInserted, &view,
                         [this](const QModelIndex&, int first, int) {
                             const QModelIndex notice = view.notices()->index(first, 0);
                             told.append(notice.data(ui::Notices::KindRole).toString() + u": "_s +
                                         notice.data(ui::Notices::TextRole).toString());
                         });
    }

    // Puts a made-up KVM on the wall, and says what the wall calls it.
    QString add(const MadeUpKvm& kvm, const QString& name) { return own.add(known(kvm, name), u"secret"_s).value(); }

    // Somebody sits down at the wall. There are no accounts here: whoever sits at this PC may act.
    void sitDown()
    {
        feed.start();
        core::Account account;
        account.role = u"admin"_s;
        actions.enter(account);
    }

    // The wall has every KVM that was added, and each of them was spoken to and is there.
    bool isReady() const
    {
        return !feed.machines().isEmpty() && feed.machines().size() == own.kvms().size() &&
               std::ranges::all_of(feed.machines(), [this](const core::Machine& machine) {
                   return feed.status().value(machine.id).state == u"online"_s;
               });
    }

    // Picks these machines on the wall, and no others, as Desk does.
    void pick(const QStringList& ids)
    {
        QList<ui::WallTile> cards;
        for (const core::Machine& machine : feed.machines()) {
            ui::WallTile card;
            card.key = machine.id;
            card.name = machine.name;
            // (Whoever fills a wall says of each card that it may be acted on: none may be
            // picked otherwise.)
            card.actable = true;
            cards.append(card);
        }
        wall.machines()->setTiles(cards);
        wall.setSelecting(true);
        wall.pickNone();
        for (const QString& id : ids) {
            wall.togglePicked(id);
        }
    }

    // Of the run that is shown, as Desk tells it.
    QString run(const QString& what) const { return view.run().value(what).toString(); }
    bool runIsOver() const { return !view.run().isEmpty() && !view.run().value(u"running"_s).toBool(); }
    QStringList runRows() const
    {
        const QVariantList rows = view.run().value(u"rows"_s).toList();
        QStringList said;
        for (const QVariant& row : rows) {
            const QVariantMap one = row.toMap();
            said.append(one.value(u"name"_s).toString() + u": "_s + one.value(u"state"_s).toString() + u": "_s +
                        one.value(u"text"_s).toString());
        }
        return said;
    }

    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    MemorySecrets secrets;
    core::LocalWall own{settings, secrets, quick()};
    core::WallFeed feed{own, 50};
    ui::OnePlace place{own, feed};
    ui::WallView wall;
    ui::ActionsView view;
    ui::UnderWay underWay;
    ui::MachineActions actions{view, wall, place, underWay};
    // Every notice that was told, as "kind: text", in the order they came.
    QStringList told;
};

// The rows of the open menu that lead elsewhere: those at its top, each by its id.
QStringList waysOf(const ui::ActionsView& view)
{
    QStringList ids;
    const QVariantList rows = view.menu().value(u"sections"_s).toList().value(0).toMap().value(u"rows"_s).toList();
    for (const QVariant& row : rows) {
        ids.append(row.toMap().value(u"id"_s).toString());
    }
    return ids;
}

// A place the wall's machines come from that says of them what a test tells it to, and does
// nothing to any: for a machine as no made-up one of these tests is (a KVM that shows another
// certificate than it is known by needs two certificates, and a line to show them on).
class ToldSource final : public QObject, public core::WallSource {
public:
    // A machine of this wall that is said to be so: by this state, and this code for what is amiss.
    void add(const QString& id, bool own, const QString& state, const QString& errorCode = {})
    {
        core::Machine machine;
        machine.id = id;
        machine.name = id;
        machine.kind = u"kvmd"_s;
        machine.own = own;
        wall.machines.append(machine);
        core::Status how;
        how.state = state;
        how.errorCode = errorCode;
        status.insert(id, how);
    }

    void askWall(std::function<void(base::Result<core::Wall>)> done) override
    {
        later([this, done = std::move(done)] { done(wall); });
    }
    void askStatus(std::function<void(base::Result<QHash<QString, core::Status>>)> done) override
    {
        later([this, done = std::move(done)] { done(status); });
    }
    void askSnapshot(const QString&, int, double, std::function<void(base::Result<core::Snapshot>)> done) override
    {
        later([done = std::move(done)] { done(base::Error{"no_stream", "No picture"}); });
    }
    void act(const QString& machine, const core::Action& what,
             std::function<void(base::Result<core::Done>)> done) override
    {
        ++acted;
        actions.append(machine + u':' + what.kind);
        later([this, done = std::move(done)] {
            if (refuses) {
                done(*refuses);
            } else {
                done(core::Done{});
            }
        });
    }
    bool mayHaveBeenDone(const QString& /*machine*/, const base::Error& /*error*/) const override { return false; }
    void askSavedActions(std::function<void(base::Result<QList<core::SavedAction>>)> done) override
    {
        later([done = std::move(done)] { done(QList<core::SavedAction>{}); });
    }
    void askWakeTargets(const QString&, std::function<void(base::Result<QList<core::WakeTarget>>)> done) override
    {
        later([done = std::move(done)] { done(QList<core::WakeTarget>{}); });
    }
    void askConsole(const QString&, std::function<void(base::Result<QUrl>)> done) override
    {
        later([done = std::move(done)] { done(base::Error{"no_console", "No console"}); });
    }
    void arrange(const QStringList& machines, std::function<void(base::Result<QStringList>)> done) override
    {
        later([done = std::move(done), machines] { done(machines); });
    }
    void openLive(const QString&, core::Live::Need,
                  std::function<void(base::Result<std::unique_ptr<core::Live>>)> done) override
    {
        later([done = std::move(done)] { done(base::Error{"no_live", "No live picture"}); });
    }

    core::Wall wall;
    QHash<QString, core::Status> status;
    // How often something was to be done to a machine, and what ("machine:kind"); what it is refused, where set.
    int acted = 0;
    QStringList actions;
    std::optional<base::Error> refuses;

private:
    void later(std::function<void()> what) { QTimer::singleShot(0, this, std::move(what)); }
};

// The wall's machines as a test has them: those of one place (OnePlace), each on the wall and told
// of as it is, of which no key reaches anything while `reachesNothing` is set.
class ReachesNothing final : public ui::WallMachines {
public:
    explicit ReachesNothing(ui::OnePlace& place) : m_place(place)
    {
        connect(&m_place, &ui::WallMachines::wallChanged, this, &ui::WallMachines::wallChanged);
        connect(&m_place, &ui::WallMachines::statusChanged, this, &ui::WallMachines::statusChanged);
    }

    const QList<core::Machine>& machines() const override { return m_place.machines(); }
    const QHash<QString, core::Status>& status() const override { return m_place.status(); }
    ui::Reached reach(const QString& key) const override { return reachesNothing ? ui::Reached{} : m_place.reach(key); }
    void lookAgainSoon(const QString& key) override { m_place.lookAgainSoon(key); }

    bool reachesNothing = true;

private:
    ui::OnePlace& m_place;
};

// A desk as Desk is, with an admin at the wall, whose actions ask through ReachesNothing.
struct CutDesk {
    explicit CutDesk(Pretended& pretended)
        : server([&pretended](const Asked& asked) { return pretended.answer(asked); }), feed(client, 60'000),
          place(client, feed), cut(place), actions(view, wall, cut, underWay), machines(2 + pretended.more)
    {
        QObject::connect(view.notices(), &QAbstractItemModel::rowsInserted, &view,
                         [this](const QModelIndex&, int first, int) {
                             const QModelIndex notice = view.notices()->index(first, 0);
                             told.append(notice.data(ui::Notices::KindRole).toString() + u": "_s +
                                         notice.data(ui::Notices::TextRole).toString());
                         });
        actions.openPagesWith([this](const QUrl& address) {
            opened.append(address);
            return true;
        });
        static_cast<void>(client.setAddress(server.address()));
        feed.start();
        core::Account account;
        account.role = u"admin"_s;
        actions.enter(account);
    }

    bool isReady() const { return feed.machines().size() == machines && feed.status().size() == machines; }

    // Picks these machines on the wall, as Desk does.
    void pick(const QStringList& ids)
    {
        QList<ui::WallTile> cards;
        for (const core::Machine& machine : feed.machines()) {
            ui::WallTile card;
            card.key = machine.id;
            card.name = machine.name;
            // (Whoever fills a wall says of each card that it may be acted on: none may be
            // picked otherwise.)
            card.actable = true;
            cards.append(card);
        }
        wall.machines()->setTiles(cards);
        wall.setSelecting(true);
        wall.pickNone();
        for (const QString& id : ids) {
            wall.togglePicked(id);
        }
    }

    // Of the run that is shown, as Desk tells it.
    bool runIsOver() const { return !view.run().isEmpty() && !view.run().value(u"running"_s).toBool(); }
    QStringList runRows() const
    {
        const QVariantList rows = view.run().value(u"rows"_s).toList();
        QStringList said;
        for (const QVariant& row : rows) {
            const QVariantMap one = row.toMap();
            said.append(one.value(u"name"_s).toString() + u": "_s + one.value(u"state"_s).toString() + u": "_s +
                        one.value(u"text"_s).toString());
        }
        return said;
    }

    StandIn server;
    core::ServerClient client;
    core::WallFeed feed;
    ui::OnePlace place;
    ReachesNothing cut;
    ui::WallView wall;
    ui::ActionsView view;
    ui::UnderWay underWay;
    ui::MachineActions actions;
    QStringList told;
    QList<QUrl> opened;
    qsizetype machines;
};

// The wall's machines as a test has them: those of one place (OnePlace), each on the wall and told
// of as it is, where a key reaches the source a test names for it (`sources`, by key; `fallback`
// for the others) and that source's place (`places`, by source; the one place's for a source it
// does not name). As the windows' wall has several sources, and as a test changes them under what
// is open, without a word.
class Routed final : public ui::WallMachines {
public:
    explicit Routed(ui::OnePlace& place) : m_place(place)
    {
        connect(&m_place, &ui::WallMachines::wallChanged, this, &ui::WallMachines::wallChanged);
        connect(&m_place, &ui::WallMachines::statusChanged, this, &ui::WallMachines::statusChanged);
    }

    const QList<core::Machine>& machines() const override { return m_place.machines(); }
    const QHash<QString, core::Status>& status() const override { return m_place.status(); }
    ui::Reached reach(const QString& key) const override
    {
        const ui::Reached there = m_place.reach(key);
        if (!there) {
            return {};
        }
        const QString source = sources.value(key, fallback);
        return ui::Reached{places.value(source, there.place), there.machine, source};
    }
    void lookAgainSoon(const QString& key) override { m_place.lookAgainSoon(key); }

    QHash<QString, QString> sources;
    QHash<QString, core::WallSource*> places;
    QString fallback = u"one"_s;

private:
    ui::OnePlace& m_place;
};

// Two QKVM servers of one wall ("one" and "two"), whoever sits here allowed to act at both: the
// wall's machines are those of the first, and each key reaches the source Routed says, the first
// unless a test says otherwise.
struct TwoDesk {
    TwoDesk(Pretended& first, Pretended& second)
        : oneServer([&first](const Asked& asked) { return first.answer(asked); }),
          twoServer([&second](const Asked& asked) { return second.answer(asked); }), feed(oneClient, 60'000),
          place(oneClient, feed), routed(place), actions(view, wall, routed, underWay), machines(2 + first.more)
    {
        QObject::connect(view.notices(), &QAbstractItemModel::rowsInserted, &view,
                         [this](const QModelIndex&, int at, int) {
                             const QModelIndex notice = view.notices()->index(at, 0);
                             told.append(notice.data(ui::Notices::KindRole).toString() + u": "_s +
                                         notice.data(ui::Notices::TextRole).toString());
                         });
        actions.openPagesWith([this](const QUrl& address) {
            opened.append(address);
            return true;
        });
        static_cast<void>(oneClient.setAddress(oneServer.address()));
        static_cast<void>(twoClient.setAddress(twoServer.address()));
        routed.places = {{u"one"_s, &oneClient}, {u"two"_s, &twoClient}};
        feed.start();
        actions.enter(u"one"_s, true);
        actions.enter(u"two"_s, true);
    }

    bool isReady() const { return feed.machines().size() == machines && feed.status().size() == machines; }

    // Picks these machines on the wall, as Desk does.
    void pick(const QStringList& ids)
    {
        QList<ui::WallTile> cards;
        for (const core::Machine& machine : feed.machines()) {
            ui::WallTile card;
            card.key = machine.id;
            card.name = machine.name;
            card.actable = true;
            cards.append(card);
        }
        wall.machines()->setTiles(cards);
        wall.setSelecting(true);
        wall.pickNone();
        for (const QString& id : ids) {
            wall.togglePicked(id);
        }
    }

    bool runIsOver() const { return !view.run().isEmpty() && !view.run().value(u"running"_s).toBool(); }
    QStringList runRows() const
    {
        const QVariantList rows = view.run().value(u"rows"_s).toList();
        QStringList said;
        for (const QVariant& row : rows) {
            const QVariantMap one = row.toMap();
            said.append(one.value(u"name"_s).toString() + u": "_s + one.value(u"state"_s).toString() + u": "_s +
                        one.value(u"text"_s).toString());
        }
        return said;
    }
    bool offers(const QString& row) const
    {
        const QVariantList sections = view.menu().value(u"sections"_s).toList();
        for (const QVariant& section : sections) {
            const QVariantList rows = section.toMap().value(u"rows"_s).toList();
            for (const QVariant& one : rows) {
                if (one.toMap().value(u"id"_s).toString() == row) {
                    return true;
                }
            }
        }
        return false;
    }

    StandIn oneServer;
    StandIn twoServer;
    core::ServerClient oneClient;
    core::ServerClient twoClient;
    core::WallFeed feed;
    ui::OnePlace place;
    Routed routed;
    ui::WallView wall;
    ui::ActionsView view;
    ui::UnderWay underWay;
    ui::MachineActions actions;
    QStringList told;
    QList<QUrl> opened;
    qsizetype machines;
};

// The second server's saved action of the same id as the first's: another one.
QJsonArray anotherSavedAction()
{
    return QJsonArray{QJsonObject{{u"id"_s, u"a1"_s},
                                  {u"name"_s, u"Wipe the screen"_s},
                                  {u"kind"_s, u"keys"_s},
                                  {u"keys"_s, u"MetaLeft,KeyD"_s},
                                  {u"enter"_s, false},
                                  {u"danger"_s, false},
                                  {u"changed"_s, 1}}};
}

} // namespace

// Doing things to machines from the screens: the menu, the question, what is sent, and what is told.
class TestMachineActions : public QObject {
    Q_OBJECT

private slots:
    void changedCapabilitiesStopTextBeforeConfirmationAndBeforeRelease()
    {
        for (const bool waitingForRelease : {false, true}) {
            Pretended server;
            server.capabilities = QStringList{u"text"_s, u"control"_s};
            Desk desk(server);
            HeldLive live;
            ui::ControlView view;
            ui::KeysByHand keys;
            ui::MachineControl control(view, keys);
            desk.actions.useControl(control);
            control.setAllowed(true);
            control.setInFront(true);
            control.have(&live, u"kvm"_s);
            control.take();
            live.answer(true);
            QTRY_VERIFY(desk.isReady());
            desk.view.openText(u"kvm"_s);
            QVERIFY(!desk.view.form().isEmpty());
            desk.view.typeText(u"fixture"_s, false);
            QVERIFY(!desk.view.question().isEmpty());
            if (waitingForRelease)
                desk.view.answer(true);
            server.capabilities = QStringList{};
            desk.feed.askAnew();
            QTRY_VERIFY(!desk.actions.supportsOn(u"kvm"_s, u"text"_s));
            if (waitingForRelease)
                live.answer(false);
            else
                desk.view.answer(true);
            QTest::qWait(100);
            QVERIFY(server.acted.isEmpty());
            QVERIFY(desk.underWay.isEmpty());
            desk.view.openText(u"kvm"_s);
            QVERIFY(desk.view.form().isEmpty());
        }
    }

    void confirmedTextWaitsForOwnReleaseAndRequestsControlOnlyAfterSuccess()
    {
        Pretended server;
        Desk desk(server);
        HeldLive live;
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl control(view, keys);
        desk.actions.useControl(control);
        control.setAllowed(true);
        control.setInFront(true);
        control.have(&live, u"kvm"_s);
        control.take();
        live.answer(true);
        QVERIFY(control.isOn());
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"typeText"_s);
        desk.view.typeText(u"fixture"_s, false);
        desk.view.answer(true);
        QTest::qWait(100);
        QVERIFY(server.acted.isEmpty());
        QCOMPARE(live.requests, (QList<bool>{true, false}));
        live.answer(false);
        QTRY_COMPARE(server.acted.size(), 1);
        QTRY_COMPARE(live.requests, (QList<bool>{true, false, true}));
        QCOMPARE(server.acted.first().value(u"text"_s).toString(), u"fixture"_s);
    }

    void keysAndSavedActionsWaitForTheirOwnReleaseAsATextDoes_data()
    {
        QTest::addColumn<QString>("row");
        QTest::newRow("keys from the menu") << u"keys:ShiftLeft"_s;
        QTest::newRow("keys of one's own choosing") << u"sendKeys"_s;
        QTest::newRow("a saved action") << u"saved:a1"_s;
    }

    void keysAndSavedActionsWaitForTheirOwnReleaseAsATextDoes()
    {
        // Rule 27 of the web's FOR-THE-DESKTOP-APP: an action sent right after keyboard and mouse were
        // let go of (a typed text, a key combination) waits for that letting go, or a key combination
        // lands beside a Ctrl of this window's that is still on its way up. As a typed text does:
        // given back first, sent once that was confirmed, and asked for again after it.
        QFETCH(QString, row);
        Pretended server;
        Desk desk(server);
        HeldLive live;
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl control(view, keys);
        desk.actions.useControl(control);
        control.setAllowed(true);
        control.setInFront(true);
        control.have(&live, u"kvm"_s);
        control.take();
        live.answer(true);
        QVERIFY(control.isOn());
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        if (row == u"sendKeys"_s) {
            desk.view.choose(row);
            desk.view.sendKeys(u"ControlLeft,KeyC"_s);
            desk.view.answer(true);
        } else {
            QTRY_VERIFY(desk.offers(row));
            desk.view.choose(row);
        }
        QTest::qWait(100);
        QVERIFY(server.acted.isEmpty());
        QCOMPARE(live.requests, (QList<bool>{true, false}));
        live.answer(false);
        QTRY_COMPARE(server.acted.size(), 1);
        QTRY_COMPARE(live.requests, (QList<bool>{true, false, true}));
        QVERIFY(server.acted.first().value(u"action"_s).toString() != u"text"_s);
    }

    void keysWhoseReleaseIsNotConfirmedAreNotSent()
    {
        Pretended server;
        Desk desk(server);
        HeldLive live;
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl::Pace pace;
        pace.textReleaseWithinMs = 150;
        ui::MachineControl control(view, keys, pace);
        desk.actions.useControl(control);
        control.setAllowed(true);
        control.setInFront(true);
        control.have(&live, u"kvm"_s);
        control.take();
        live.answer(true);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        QTRY_VERIFY(desk.told.contains(u"error: NAS: Control release was not confirmed; nothing was sent."_s));
        // A word that comes late sends nothing either.
        live.answer(false);
        QTest::qWait(200);
        QVERIFY(server.acted.isEmpty());
    }

    void nothingThatPressesKeysGoesWhileSomethingMayStillBeDown_data()
    {
        QTest::addColumn<QString>("row");
        QTest::newRow("Delete, of one's own choosing") << u"sendKeys"_s;
        QTest::newRow("a saved action") << u"saved:a1"_s;
        QTest::newRow("a typed text") << u"typeText"_s;
    }

    void nothingThatPressesKeysGoesWhileSomethingMayStillBeDown()
    {
        // The merged review's MEDIUM 2 (2026-11-06): Ctrl held here, and an action that presses keys or types is
        // picked. Keyboard and mouse are given back first; the console line's goodbye does not come in time, the line
        // is reset, and its live says that something may still be down, then the plain word for the giving back
        // (KvmdLive, a7). Ctrl may still be down on the machine: the action is not sent (it could land as Ctrl+Delete),
        // it is refused in those words, and so is the next one, until keyboard and mouse were taken anew.
        QFETCH(QString, row);
        Pretended server;
        Desk desk(server);
        HeldLive live;
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl control(view, keys);
        desk.actions.useControl(control);
        control.setAllowed(true);
        control.setInFront(true);
        control.have(&live, u"kvm"_s);
        control.take();
        live.answer(true);
        QVERIFY(control.isOn());
        constexpr input::Pressed leftCtrl{0x1D, false, 0xA2};
        keys.press(leftCtrl);
        QCOMPARE(live.keys, (QList<std::pair<int, bool>>{{0xE0, true}}));
        QTRY_VERIFY(desk.isReady());
        const auto pick = [&] {
            desk.view.open(u"kvm"_s);
            if (row == u"sendKeys"_s) {
                desk.view.choose(row);
                desk.view.sendKeys(u"Delete"_s);
                desk.view.answer(true);
            } else if (row == u"typeText"_s) {
                desk.view.choose(row);
                desk.view.typeText(u"fixture"_s, false);
                desk.view.answer(true);
            } else {
                QTRY_VERIFY(desk.offers(row));
                desk.view.choose(row);
            }
        };
        const QString refused = row == u"typeText"_s
                                    ? u"error: NAS: Something that was pressed may still be down on this machine; no "
                                      "text was sent. Take keyboard and mouse again, and press and let go of it."_s
                                    : u"error: NAS: Something that was pressed may still be down on this machine; "
                                      "nothing was sent. Take keyboard and mouse again, and press and let go of it."_s;
        pick();
        QTest::qWait(50);
        QCOMPARE(live.requests, (QList<bool>{true, false}));
        emit live.leftDown();
        live.answer(false);
        QTRY_VERIFY2(desk.told.contains(refused), qPrintable(desk.told.join(u'\n')));
        QTest::qWait(100);
        QVERIFY(server.acted.isEmpty());
        QCOMPARE(live.requests, (QList<bool>{true, false}));
        // The next one, with nothing taken anew: refused at once.
        desk.told.clear();
        pick();
        QTRY_VERIFY2(desk.told.contains(refused), qPrintable(desk.told.join(u'\n')));
        QTest::qWait(100);
        QVERIFY(server.acted.isEmpty());
        QCOMPARE(live.requests, (QList<bool>{true, false}));
        // Taken anew, so that what stayed down can be pressed and let go there: the next one goes after its release.
        control.take();
        live.answer(true);
        QVERIFY(control.isOn());
        pick();
        QTest::qWait(50);
        QVERIFY(server.acted.isEmpty());
        live.answer(false);
        QTRY_COMPARE(server.acted.size(), 1);
    }

    void clipboardTextIsReadOnlyOnRequestAndStillNeedsConfirmation()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        int reads = 0;
        QString clipboard = u"example text\nsecond line"_s;
        desk.view.readClipboardWith([&] {
            ++reads;
            return clipboard;
        });
        const QSignalSpy loaded(&desk.view, &ui::ActionsView::clipboardLoaded);
        desk.view.loadClipboard();
        QCOMPARE(reads, 0);
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"typeText"_s);
        QCOMPARE(reads, 0);
        desk.view.loadClipboard();
        QCOMPARE(reads, 1);
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().first().toString(), clipboard);
        QVERIFY(pretended.acted.isEmpty());
        desk.view.typeText(clipboard, false);
        QVERIFY(pretended.acted.isEmpty());
        QVERIFY(!desk.view.question().isEmpty());
        desk.view.answer(false);
        QVERIFY(pretended.acted.isEmpty());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"typeText"_s);
        clipboard = QString(2001, u'x');
        desk.view.loadClipboard();
        QCOMPARE(loaded.size(), 1);
        QVERIFY(!desk.view.form().value(u"problem"_s).toString().isEmpty());
        clipboard.clear();
        desk.view.loadClipboard();
        QCOMPARE(loaded.size(), 1);
        desk.view.setAllowed(false);
        const int before = reads;
        desk.view.loadClipboard();
        QCOMPARE(reads, before);
        QVERIFY(pretended.acted.isEmpty());
    }

    void clipboardKeepsUnicodeWholeAndDiscardsAClosedForm()
    {
        ui::ActionsView view;
        view.setAllowed(true);
        const QVariantMap form{{u"kind"_s, u"text"_s}, {u"machine"_s, u"Fixture"_s}};
        view.showForm(form);
        const char32_t sign = 0x1F600;
        QString clipboard = QString::fromUcs4(&sign, 1).repeated(2000);
        view.readClipboardWith([&] { return clipboard; });
        const QSignalSpy loaded(&view, &ui::ActionsView::clipboardLoaded);
        view.loadClipboard();
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().first().toString(), clipboard);
        clipboard += u'x';
        view.loadClipboard();
        QCOMPARE(loaded.size(), 1);
        clipboard = QString(QChar(0xD800));
        view.loadClipboard();
        QCOMPARE(loaded.size(), 1);
        view.showForm(form);
        view.readClipboardWith([&] {
            view.showForm({});
            return u"no longer wanted"_s;
        });
        view.loadClipboard();
        QCOMPARE(loaded.size(), 1);
    }

    void whoMayOnlyLookGetsNoMenuAndSendsNothing()
    {
        Pretended pretended;
        Desk desk(pretended, u"viewer"_s);
        QTRY_VERIFY(desk.isReady());
        QVERIFY(!desk.view.allowed());
        desk.view.open(u"kvm"_s);
        desk.view.showPort(u"kvm"_s, 1);
        QTest::qWait(200);
        QVERIFY(desk.view.menu().isEmpty());
        QVERIFY(pretended.acted.isEmpty());
        QCOMPARE(pretended.savedAsked, 0);

        // An operator may, and so may an admin.
        Pretended other;
        const Desk operatorsDesk(other, u"operator"_s);
        QVERIFY(operatorsDesk.view.allowed());
        const Desk adminsDesk(other, u"admin"_s);
        QVERIFY(adminsDesk.view.allowed());
    }

    void whoMayNoLongerActHasNothingOpenAndNothingUnderWay()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) { return late(300); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        core::Account account;
        account.id = u"u1"_s;
        account.name = u"anna"_s;
        // Something is on its way to the KVM, a question stands over the server's menu, and
        // machines are being picked.
        desk.pick({u"kvm"_s, u"srv"_s});
        QVERIFY(desk.wall.selecting());
        desk.view.showPort(u"kvm"_s, 1);
        desk.view.open(u"srv"_s);
        desk.view.choose(u"power:power_off"_s);
        QVERIFY(!desk.view.question().isEmpty());
        QVERIFY(desk.view.busy());

        // Elsewhere an admin makes whoever sits here somebody who may only look: it is as if the
        // wall had been left by one and come to by another.
        account.role = u"viewer"_s;
        desk.actions.enter(account);
        QVERIFY(!desk.view.allowed());
        QVERIFY(desk.view.menu().isEmpty());
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QVERIFY(!desk.wall.selecting());
        QCOMPARE(desk.view.notices()->count(), 0);
        // The answer to what was on its way is nobody's, and nothing is opened or sent.
        QTest::qWait(500);
        QCOMPARE(desk.told, QStringList{u"working: NAS: Show Build box…"_s});
        desk.view.answer(true);
        desk.view.open(u"kvm"_s);
        desk.view.openPicked();
        QVERIFY(desk.view.menu().isEmpty());
        QCOMPARE(pretended.acted.size(), 1);

        // An operator again: may act, and the machine is free.
        account.role = u"operator"_s;
        desk.actions.enter(account);
        QVERIFY(desk.view.allowed());
        desk.view.open(u"kvm"_s);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        desk.view.choose(u"power:atx_power_long"_s);
        QVERIFY(!desk.view.question().isEmpty());
        // Made an admin while a question stands over a menu: nothing that is open is taken away,
        // and nothing either when the server says the same of them a minute later.
        account.role = u"admin"_s;
        desk.actions.enter(account);
        desk.actions.enter(account);
        QVERIFY(desk.view.allowed());
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        QVERIFY(!desk.view.question().isEmpty());
        QVERIFY(desk.view.busy());
        desk.view.answer(true);
        QTRY_COMPARE(pretended.acted.size(), 2);
        QCOMPARE(pretended.acted.constLast()[u"action"_s].toString(), u"atx_power_long"_s);
    }

    void aMenuOpensForAMachineWithWhatTheServerHasSaved()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        QVERIFY(desk.view.menu().isEmpty());
        desk.view.open(u"kvm"_s);
        // At once, with what is known already.
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        QVERIFY(desk.offers(u"keys:ShiftLeft"_s));
        QVERIFY(desk.offers(u"power:atx_power"_s));
        QVERIFY(desk.offers(u"port:1"_s));
        QVERIFY(!desk.offers(u"saved:a1"_s));
        // And then with the actions saved on the server, and whom the KVM can wake.
        QTRY_VERIFY(desk.offers(u"saved:a1"_s));
        QTRY_VERIFY(desk.offers(u"wake:00:11:22:aa:bb:cc"_s));
        QCOMPARE(pretended.savedAsked, 1);
        QCOMPARE(pretended.wakeAsked, 1);

        // Closed, and opened for the server: a controller wakes nobody, and is not asked.
        desk.view.close();
        QVERIFY(desk.view.menu().isEmpty());
        desk.view.open(u"srv"_s);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"Rack server"_s);
        QVERIFY(desk.offers(u"power:power_off"_s));
        QVERIFY(!desk.offers(u"power:power_on"_s));
        QTRY_COMPARE(pretended.savedAsked, 2);
        QTest::qWait(150);
        QCOMPARE(pretended.wakeAsked, 1);
        QVERIFY(!desk.offers(u"wake:00:11:22:aa:bb:cc"_s));

        // A machine the wall does not have has no menu: the one that is open stays.
        desk.view.open(u"nobody"_s);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"Rack server"_s);
    }

    void anOpenMenuIsOfTheMachineAsItIsNow()
    {
        Pretended pretended;
        Desk desk(pretended, u"operator"_s, 50);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"srv"_s);
        QVERIFY(desk.offers(u"power:power_off"_s));
        // Somebody switches the server off, elsewhere: it is offered "Power on" now, and says how it is.
        pretended.serverIsOn = false;
        QTRY_VERIFY(desk.offers(u"power:power_on"_s));
        QVERIFY(!desk.offers(u"power:power_off"_s));
        QCOMPARE(desk.view.menu().value(u"warning"_s).toString(), u"Powered off"_s);

        // A machine that leaves the wall takes its menu with it.
        desk.view.open(u"kvm"_s);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        pretended.kvmIsThere = false;
        QTRY_VERIFY(desk.view.menu().isEmpty());
    }

    void aRowIsSentAndToldInTheCorner()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) { return late(150); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        // Under way: said so, and the menu holds still. A second click is not a second request.
        QCOMPARE(desk.told, QStringList{u"working: NAS: Wake screen…"_s});
        QVERIFY(desk.view.busy());
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.choose(u"keys:Enter"_s);
        // Done: the notice of what was under way goes, and what was done is told.
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"ok: NAS: Shift sent"_s);
        QCOMPARE(desk.view.notices()->count(), 1);
        QVERIFY(!desk.view.busy());
        QCOMPARE(pretended.acted.size(), 1);
        QCOMPARE(pretended.acted.constFirst(),
                 (QJsonObject{{u"machine"_s, u"kvm"_s}, {u"action"_s, u"keys"_s}, {u"keys"_s, u"ShiftLeft"_s}}));
        // The menu stays open for the next thing.
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        desk.view.choose(u"keys:Enter"_s);
        QTRY_COMPARE(pretended.acted.size(), 2);
        QTRY_COMPARE(desk.told.constLast(), u"ok: NAS: Enter sent"_s);

        // A row the menu does not have sends nothing.
        desk.view.choose(u"power:power_on"_s);
        desk.view.choose(u"no such row"_s);
        QTest::qWait(250);
        QCOMPARE(pretended.acted.size(), 2);
    }

    void whatCannotBeTakenBackIsAskedAboutFirst()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"power:atx_power_long"_s);
        // Asked, in the page's words; nothing is sent, and the menu holds still behind the question.
        QCOMPARE(desk.view.question(),
                 (QVariantMap{{u"title"_s, u"Hold power button"_s},
                              {u"text"_s, u"Hold power button on NAS?"_s},
                              {u"detail"_s, QString()},
                              {u"warning"_s, u"A long press cuts the power. Unsaved work is lost."_s},
                              {u"danger"_s, true},
                              {u"yes"_s, u"Hold power button"_s},
                              {u"targets"_s, QVariantList()},
                              {u"tick"_s, QString()},
                              {u"paces"_s, QVariantList()}}));
        QVERIFY(desk.view.busy());
        QVERIFY(pretended.acted.isEmpty());
        QVERIFY(desk.told.isEmpty());

        // No: nothing is sent, and the menu is there again.
        desk.view.answer(false);
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());
        // An answer to nothing is nothing.
        desk.view.answer(true);
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());

        // Yes: sent, once.
        desk.view.choose(u"power:atx_power_long"_s);
        QVERIFY(!desk.view.question().isEmpty());
        desk.view.answer(true);
        QVERIFY(desk.view.question().isEmpty());
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told, (QStringList{u"working: NAS: Hold power button…"_s, u"ok: NAS: power button held"_s}));
        QCOMPARE(pretended.acted.size(), 1);
        QCOMPARE(pretended.acted.constFirst(),
                 (QJsonObject{{u"machine"_s, u"kvm"_s}, {u"action"_s, u"atx_power_long"_s}}));
        QVERIFY(!desk.view.busy());
    }

    void whatTheServerAddsIsTold()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) {
            return Answered{200, QByteArray("{\"ok\": true, \"note\": \"already on\"}")};
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        pretended.serverIsOn = false;
        desk.feed.lookAgainSoon();
        desk.view.open(u"srv"_s);
        QTRY_VERIFY_WITH_TIMEOUT(desk.offers(u"power:power_on"_s), 3000);
        desk.view.choose(u"power:power_on"_s);
        desk.view.answer(true);
        // Left as it was, and that is no failure.
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"ok: Rack server: already on"_s);
        QCOMPARE(pretended.acted.constFirst(), (QJsonObject{{u"machine"_s, u"srv"_s}, {u"action"_s, u"power_on"_s}}));
    }

    void aRefusalIsToldInTheServersWords()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) {
            return Answered{409, QByteArray("{\"code\": \"busy\", \"message\": \"Something else is being done to NAS "
                                            "at this moment. Wait for it to finish.\"}")};
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"power:jiggler_on"_s);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(),
                 u"error: NAS: Something else is being done to NAS at this moment. Wait for it to finish."_s);
        // The menu is there, and can be used again.
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        QCOMPARE(desk.view.notices()->count(), 1);
    }

    void noAnswerIsToldAsWhatItIsAndNothingIsSentAgain()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) {
            Answered none;
            none.hangUp = true;
            return none;
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"power:atx_power"_s);
        desk.view.answer(true);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"error: NAS: no answer to \"Press power button\". It may have been done all "
                                        "the same: look before sending it again."_s);
        QVERIFY(!desk.view.busy());
        QTest::qWait(300);
        QCOMPARE(pretended.acted.size(), 1);

        // The server's own word for having lost sight of it is told the same way, and not as the
        // server's sentence, which would read like a refusal.
        pretended.onAction = [](const QJsonObject&) {
            return Answered{504, QByteArray("{\"code\": \"timeout\", \"message\": \"NAS was being changed.\"}")};
        };
        desk.view.choose(u"power:atx_reset"_s);
        desk.view.answer(true);
        QTRY_COMPARE(desk.told.size(), 4);
        QCOMPARE(desk.told.constLast(), u"error: NAS: no answer to \"Press reset button\". It may have been done all "
                                        "the same: look before sending it again."_s);
        QCOMPARE(pretended.acted.size(), 2);

        // And so is its word for an answer of the machine's that broke off: it went out.
        pretended.onAction = [](const QJsonObject&) {
            return Answered{502, QByteArray("{\"code\": \"dropped\", \"message\": \"The connection to the JetKVM "
                                            "ended\"}")};
        };
        desk.view.choose(u"power:atx_power"_s);
        desk.view.answer(true);
        QTRY_COMPARE(desk.told.size(), 6);
        QCOMPARE(desk.told.constLast(), u"error: NAS: no answer to \"Press power button\". It may have been done all "
                                        "the same: look before sending it again."_s);
        QTest::qWait(300);
        QCOMPARE(pretended.acted.size(), 3);
    }

    void aMachineTheServerCannotReachIsARefusalThatCanBeSentAgain()
    {
        // The server answers that the KVM did not: nothing was done to it. That is the server's
        // sentence, and not "it may have been done all the same".
        Pretended pretended;
        pretended.onAction = [](const QJsonObject& action) -> Answered {
            if (action[u"machine"_s].toString() == u"kvm"_s) {
                return {502, QByteArray("{\"code\": \"unreachable\", \"message\": \"Device is unreachable\"}")};
            }
            return {200, QByteArray("{\"ok\": true, \"note\": null}")};
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"error: NAS: Device is unreachable"_s);
        desk.view.close();

        // In a run on several it is one that failed, and is offered again.
        desk.pick({u"kvm"_s, u"srv"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: bad: Device is unreachable"_s, u"Rack server: ok: Shift sent"_s}));
        QCOMPARE(desk.run(u"summary"_s), u"1 succeeded, 1 failed."_s);
        QCOMPARE(desk.run(u"again"_s), u"Retry failed"_s);
    }

    void oneThingAtATimeToAMachine()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) { return late(400); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        // From the numbers on the machine's card: sent, and the wall is not held still for it.
        desk.view.showPort(u"kvm"_s, 1);
        QCOMPARE(desk.told, QStringList{u"working: NAS: Show Build box…"_s});
        QVERIFY(!desk.view.busy());
        // While that is out, nothing else goes to this machine: said, and not sent. Not from its
        // card, and not from its menu, where it is said before anything is asked about.
        desk.view.showPort(u"kvm"_s, 2);
        QCOMPARE(desk.told.constLast(), u"info: NAS: \"Show Build box\" is still under way. Wait for its answer."_s);
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"power:atx_power_long"_s);
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.told.size(), 3);
        QCOMPARE(desk.told.constLast(), u"info: NAS: \"Show Build box\" is still under way. Wait for its answer."_s);
        // Another machine is free meanwhile.
        desk.view.open(u"srv"_s);
        desk.view.choose(u"light:on"_s);
        QCOMPARE(desk.told.constLast(), u"working: Rack server: Locator light on…"_s);
        QTRY_COMPARE(pretended.acted.size(), 2);
        // Both are answered: each is told, and the first machine is free again.
        QTRY_VERIFY(desk.told.contains(u"ok: NAS: showing Build box"_s));
        QTRY_VERIFY(desk.told.contains(u"ok: Rack server: locator light on"_s));
        desk.view.showPort(u"kvm"_s, 2);
        QTRY_COMPARE(pretended.acted.size(), 3);
        QCOMPARE(pretended.acted.constLast(),
                 (QJsonObject{{u"machine"_s, u"kvm"_s}, {u"action"_s, u"port"_s}, {u"port"_s, 2}}));
    }

    void whatOneWindowSentIsUnderWayForEveryWindow()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) { return late(400); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        // A second window: a wall and screens of its own, and the same word of what is under way.
        ui::WallView otherWall;
        ui::ActionsView otherView;
        auto other = std::make_unique<ui::MachineActions>(otherView, otherWall, desk.place, desk.underWay);
        core::Account account;
        account.id = u"u1"_s;
        account.name = u"anna"_s;
        account.role = u"operator"_s;
        other->enter(account);
        const auto toldThere = [&] {
            const ui::Notices& notices = *otherView.notices();
            return notices.count() == 0 ? QString()
                                        : notices.index(notices.count() - 1, 0).data(ui::Notices::TextRole).toString();
        };

        // Sent from the first: the second sends nothing to that machine meanwhile, and says why
        // where it was asked.
        desk.view.showPort(u"kvm"_s, 1);
        otherView.showPort(u"kvm"_s, 2);
        QCOMPARE(toldThere(), u"NAS: \"Show Build box\" is still under way. Wait for its answer."_s);
        QCOMPARE(desk.told.size(), 1);
        QTRY_VERIFY(desk.told.contains(u"ok: NAS: showing Build box"_s));
        QCOMPARE(pretended.acted.size(), 1);
        // Answered: the machine is free for both.
        QVERIFY(desk.underWay.isEmpty());
        otherView.showPort(u"kvm"_s, 2);
        QVERIFY(desk.underWay.contains(u"kvm"_s));
        desk.view.showPort(u"kvm"_s, 1);
        QCOMPARE(desk.told.constLast(), u"info: NAS: \"Show Port 3\" is still under way. Wait for its answer."_s);
        QTRY_COMPARE(pretended.acted.size(), 2);

        // The second window leaves the wall, or is closed, while what it sent is out: nobody is
        // left to hear the answer, and the machine is free for the first. What the first has out
        // itself stays under way.
        desk.view.open(u"srv"_s);
        desk.view.choose(u"light:on"_s);
        QVERIFY(desk.underWay.contains(u"srv"_s));
        other.reset();
        QVERIFY(!desk.underWay.contains(u"kvm"_s));
        QVERIFY(desk.underWay.contains(u"srv"_s));
        desk.view.showPort(u"kvm"_s, 1);
        QTRY_COMPARE(pretended.acted.size(), 4);
        QTRY_VERIFY(desk.underWay.isEmpty());
    }

    void aMenuHoldsStillForWhatItSentAndNotForAnothersAnswer()
    {
        Pretended pretended;
        // The server's controller takes its time; the KVM answers quickly.
        pretended.onAction = [](const QJsonObject& action) {
            return late(action[u"machine"_s].toString() == u"srv"_s ? 700 : 100);
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"srv"_s);
        desk.view.choose(u"light:on"_s);
        QVERIFY(desk.view.busy());
        // Meanwhile a number on the KVM's card is clicked, and that is answered first.
        desk.view.showPort(u"kvm"_s, 1);
        QTRY_VERIFY(desk.told.contains(u"ok: NAS: showing Build box"_s));
        // The menu is still waiting for its own.
        QVERIFY(desk.view.busy());
        QVERIFY(!desk.told.contains(u"ok: Rack server: locator light on"_s));
        QTRY_VERIFY(desk.told.contains(u"ok: Rack server: locator light on"_s));
        QVERIFY(!desk.view.busy());
    }

    void whatWasSentWhileAQuestionStoodIsNotSentOnTopOf()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) { return late(300); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"power:atx_power"_s);
        QVERIFY(!desk.view.question().isEmpty());
        // Meanwhile somebody clicks a number on the machine's card.
        desk.view.showPort(u"kvm"_s, 1);
        // Yes to the question: said that something is under way, not sent, and the menu is let go.
        desk.view.answer(true);
        QCOMPARE(desk.told.constLast(), u"info: NAS: \"Show Build box\" is still under way. Wait for its answer."_s);
        QVERIFY(!desk.view.busy());
        QTRY_VERIFY(desk.told.contains(u"ok: NAS: showing Build box"_s));
        QTest::qWait(150);
        QCOMPARE(pretended.acted.size(), 1);
    }

    void theNumbersOnACardShowAnotherComputer()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        // The one that is on the screen already, and a number the machine does not have: nothing.
        desk.view.showPort(u"kvm"_s, 0);
        desk.view.showPort(u"kvm"_s, 7);
        desk.view.showPort(u"srv"_s, 1);
        desk.view.showPort(u"nobody"_s, 1);
        QTest::qWait(200);
        QVERIFY(pretended.acted.isEmpty());
        QVERIFY(desk.told.isEmpty());

        desk.view.showPort(u"kvm"_s, 1);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"ok: NAS: showing Build box"_s);
        QCOMPARE(pretended.acted.constFirst(),
                 (QJsonObject{{u"machine"_s, u"kvm"_s}, {u"action"_s, u"port"_s}, {u"port"_s, 1}}));
        // No menu was opened for it.
        QVERIFY(desk.view.menu().isEmpty());
    }

    void aSavedActionThatWasChangedSinceIsNotRunOnTheStrengthOfAnOldRow()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) {
            return Answered{409, QByteArray("{\"code\": \"changed\", \"message\": \"\\\"Lock it\\\" was changed since "
                                            "you opened this list. Look at it again before running it.\"}")};
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        QTRY_VERIFY(desk.offers(u"saved:a1"_s));
        QCOMPARE(pretended.savedAsked, 1);
        desk.view.choose(u"saved:a1"_s);
        QTRY_COMPARE(desk.told.size(), 2);
        // It went with the state it was looked at, and the server's refusal is told.
        QCOMPARE(pretended.acted.constFirst(), (QJsonObject{{u"machine"_s, u"kvm"_s},
                                                            {u"action"_s, u"custom"_s},
                                                            {u"custom_id"_s, u"a1"_s},
                                                            {u"custom_changed"_s, 1}}));
        QCOMPARE(desk.told.constLast(), u"error: NAS: \"Lock it\" was changed since you opened this list. Look at it "
                                        "again before running it."_s);
        // The list it was chosen from is out of date: it is closed, and asked for anew.
        QVERIFY(desk.view.menu().isEmpty());
        QTRY_COMPARE(pretended.savedAsked, 2);
    }

    void keysOfOnesOwnChoosingGoThroughTheirBoxAndAQuestion()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        QVERIFY(desk.view.form().isEmpty());
        desk.view.open(u"kvm"_s);
        QVERIFY(desk.offers(u"sendKeys"_s));
        desk.view.choose(u"sendKeys"_s);
        // The box opens in the menu's place, for that machine.
        QVERIFY(desk.view.menu().isEmpty());
        QCOMPARE(desk.view.form(),
                 (QVariantMap{{u"kind"_s, u"keys"_s}, {u"machine"_s, u"NAS"_s}, {u"problem"_s, QString()}}));

        // Nothing chosen: said in the box, which stays; nothing is asked or sent.
        desk.view.sendKeys(QString());
        QCOMPARE(desk.view.form().value(u"problem"_s).toString(), u"Choose at least one key."_s);
        QVERIFY(desk.view.question().isEmpty());
        // The keyboard's Power key: said in the box in the server's words, and neither asked nor sent.
        desk.view.sendKeys(u"ControlLeft,Power"_s);
        QCOMPARE(desk.view.form().value(u"problem"_s).toString(),
                 u"The keyboard's Power key is not sent. A machine's power is under Power."_s);
        QVERIFY(desk.view.question().isEmpty());
        // A text is not taken by the box for keys.
        desk.view.typeText(u"dir"_s, true);
        QVERIFY(desk.view.question().isEmpty());

        // Keys: the box goes, and what was chosen is asked about once more.
        desk.view.sendKeys(u"ControlLeft,KeyC"_s);
        QVERIFY(desk.view.form().isEmpty());
        QCOMPARE(desk.view.question().value(u"title"_s).toString(), u"Send Ctrl+C"_s);
        QCOMPARE(desk.view.question().value(u"text"_s).toString(), u"Send Ctrl+C on NAS?"_s);
        QVERIFY(pretended.acted.isEmpty());
        desk.view.answer(true);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told, (QStringList{u"working: NAS: Send Ctrl+C…"_s, u"ok: NAS: Ctrl+C sent"_s}));
        QCOMPARE(pretended.acted.constFirst(),
                 (QJsonObject{{u"machine"_s, u"kvm"_s}, {u"action"_s, u"keys"_s}, {u"keys"_s, u"ControlLeft,KeyC"_s}}));
        // Keys with no box open go nowhere.
        desk.view.sendKeys(u"ControlLeft,KeyV"_s);
        QVERIFY(desk.view.question().isEmpty());
        QTest::qWait(150);
        QCOMPARE(pretended.acted.size(), 1);
    }

    void aTextGoesThroughItsBoxAndIsShownOnceMore()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"typeText"_s);
        QCOMPARE(desk.view.form(),
                 (QVariantMap{{u"kind"_s, u"text"_s}, {u"machine"_s, u"NAS"_s}, {u"problem"_s, QString()}}));
        desk.view.typeText(QString(), true);
        QCOMPARE(desk.view.form().value(u"problem"_s).toString(), u"Enter the text to type."_s);
        desk.view.sendKeys(u"F5"_s);
        QVERIFY(desk.view.question().isEmpty());

        desk.view.typeText(u"ipconfig /all"_s, true);
        QVERIFY(desk.view.form().isEmpty());
        QCOMPARE(desk.view.question().value(u"text"_s).toString(), u"Type this on NAS?"_s);
        QCOMPARE(desk.view.question().value(u"detail"_s).toString(), u"ipconfig /all ⏎"_s);
        // No: nothing is typed.
        desk.view.answer(false);
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());

        desk.view.open(u"kvm"_s);
        desk.view.choose(u"typeText"_s);
        desk.view.typeText(u"ipconfig /all"_s, false);
        desk.view.answer(true);
        QTRY_COMPARE(desk.told.size(), 2);
        // What was typed is told nowhere: that it was, is.
        QCOMPARE(desk.told, (QStringList{u"working: NAS: Type text…"_s, u"ok: NAS: text typed"_s}));
        QCOMPARE(pretended.acted.constFirst(), (QJsonObject{{u"machine"_s, u"kvm"_s},
                                                            {u"action"_s, u"text"_s},
                                                            {u"text"_s, u"ipconfig /all"_s},
                                                            {u"enter"_s, false}}));
    }

    void aBoxIsClosedByHandAndWithItsMachineAndWithTheWall()
    {
        Pretended pretended;
        Desk desk(pretended, u"operator"_s, 50);
        QTRY_VERIFY(desk.isReady());
        // By hand.
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"typeText"_s);
        QVERIFY(!desk.view.form().isEmpty());
        desk.view.closeForm();
        QVERIFY(desk.view.form().isEmpty());
        desk.view.typeText(u"dir"_s, true);
        QVERIFY(desk.view.question().isEmpty());

        // The machine leaves the wall: the box that was open for it goes with it.
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"sendKeys"_s);
        QVERIFY(!desk.view.form().isEmpty());
        pretended.kvmIsThere = false;
        QTRY_VERIFY(desk.view.form().isEmpty());
        pretended.kvmIsThere = true;
        QTRY_VERIFY(desk.isReady());

        // The wall is left.
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"sendKeys"_s);
        QVERIFY(!desk.view.form().isEmpty());
        desk.actions.leave();
        QVERIFY(desk.view.form().isEmpty());
        desk.view.sendKeys(u"F5"_s);
        QVERIFY(desk.view.question().isEmpty());
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());
    }

    void whatIsOutToAMachineIsSaidBeforeItsBoxAsksAnything()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) { return late(400); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"sendKeys"_s);
        // Meanwhile a number on the machine's card is clicked.
        desk.view.showPort(u"kvm"_s, 1);
        desk.view.sendKeys(u"F5"_s);
        // Said, and not asked about: it would not be sent.
        QCOMPARE(desk.told.constLast(), u"info: NAS: \"Show Build box\" is still under way. Wait for its answer."_s);
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(desk.view.form().isEmpty());
        QTRY_VERIFY(desk.told.contains(u"ok: NAS: showing Build box"_s));
        QCOMPARE(pretended.acted.size(), 1);
    }

    // b5: a PC with Intel AMT is put in another of its three views from its menu, as from the page's
    // Actions box: by its own call, the server's word told as the page's toast says it, and the menu
    // goes; a refusal is told in the server's words, and the menu stays. Not twice while one is asked.
    void aPcIsPutInAnotherViewFromItsMenu()
    {
        Pretended pretended;
        pretended.amtIsThere = true;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"amt"_s);
        QVERIFY(desk.offers(u"view:health"_s));
        QVERIFY(desk.offers(u"view:live"_s));
        // (The view it is in leads nowhere.)
        desk.view.choose(u"view:stills"_s);
        QTest::qWait(150);
        QVERIFY(pretended.viewsAsked.isEmpty());
        pretended.onView = [] {
            Answered later = Pretended::json({{u"ok"_s, true}, {u"amt_view"_s, u"health"_s}});
            later.afterMs = 200;
            return later;
        };
        desk.view.choose(u"view:health"_s);
        desk.view.choose(u"view:health"_s);
        QTRY_VERIFY(desk.told.contains(u"ok: Front desk: Health only"_s));
        QCOMPARE(pretended.viewsAsked, QStringList{u"health"_s});
        QVERIFY(desk.view.menu().isEmpty());
        pretended.amtView = u"health"_s;
        pretended.onView = [] {
            return Answered{403, R"({"code": "not_allowed", "message": "A token does not change how a tile is set."})"};
        };
        desk.view.open(u"amt"_s);
        desk.view.choose(u"view:live"_s);
        QTRY_VERIFY(desk.told.contains(u"error: Front desk: A token does not change how a tile is set."_s));
        QCOMPARE(pretended.viewsAsked, (QStringList{u"health"_s, u"live"_s}));
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"Front desk"_s);
        // A KVM has no views.
        desk.view.open(u"kvm"_s);
        QVERIFY(!desk.offers(u"view:health"_s));
    }

    // b11: a PC with Intel AMT that its server lists with no view (0.2.2, which has none, and answers PUT .../view with
    // its 404) is offered no view, and none is asked for: what a server does not have is not shown.
    void aPcItsServerSaysNoViewOfIsPutInNone()
    {
        Pretended pretended;
        pretended.amtIsThere = true;
        pretended.amtViewSaid = false;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"amt"_s);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"Front desk"_s);
        for (const QString& row : {u"view:health"_s, u"view:stills"_s, u"view:live"_s}) {
            QVERIFY2(!desk.offers(row), qPrintable(row));
            desk.view.choose(row);
        }
        QTest::qWait(150);
        QVERIFY(pretended.viewsAsked.isEmpty());
    }

    void aKvmsOwnConsoleIsOpenedAtItsDoor()
    {
        Pretended pretended;
        // A normal browser URL needs no signed-in console capability (kvmd advertises none).
        pretended.capabilities = QStringList{u"screen"_s, u"keys"_s, u"text"_s, u"live"_s, u"control"_s};
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        // From its card: the address the wall names for it, in a browser. A KVM hands out no pass.
        desk.view.openConsole(u"kvm"_s);
        QCOMPARE(desk.opened, QList<QUrl>{QUrl(u"http://192.168.1.22/"_s)});
        // From its menu, which stays: a console opens beside the program, not in its place.
        desk.view.open(u"kvm"_s);
        QVERIFY(desk.offers(u"console"_s));
        desk.view.choose(u"console"_s);
        QCOMPARE(desk.opened.size(), 2);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        // A machine the wall does not have has no console.
        desk.view.openConsole(u"nobody"_s);
        QCOMPARE(desk.opened.size(), 2);
        QTest::qWait(150);
        QCOMPARE(pretended.consoleAsked, 0);
        QVERIFY(desk.told.isEmpty());
    }

    void aServersConsoleIsEnteredSignedIn()
    {
        Pretended pretended;
        pretended.onConsole = [] {
            Answered pass = Pretended::json({{u"url"_s, u"https://192.168.1.60/console?pass=one-use"_s}});
            pass.afterMs = 200;
            return pass;
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.openConsole(u"srv"_s);
        // Its controller is making a pass out: said, and nothing is opened yet.
        QCOMPARE(desk.told, QStringList{u"working: Rack server: Open console…"_s});
        QVERIFY(desk.opened.isEmpty());
        // A second click meanwhile has no second pass made out.
        desk.view.openConsole(u"srv"_s);
        QTRY_COMPARE(desk.opened.size(), 1);
        // Straight in, with the pass the server named; and nothing more is said of it.
        QCOMPARE(desk.opened.constFirst(), QUrl(u"https://192.168.1.60/console?pass=one-use"_s));
        QCOMPARE(pretended.consoleAsked, 1);
        QCOMPARE(desk.told.size(), 1);
        QCOMPARE(desk.view.notices()->count(), 0);
        // The next time, a pass of its own.
        desk.view.openConsole(u"srv"_s);
        QTRY_COMPARE(desk.opened.size(), 2);
        QCOMPARE(pretended.consoleAsked, 2);
    }

    void whoMayOnlyLookIsSentToAConsolesDoor()
    {
        Pretended pretended;
        Desk desk(pretended, u"viewer"_s);
        QTRY_VERIFY(desk.isReady());
        // No pass is asked for: the server would refuse it, and writes down who asks.
        desk.view.openConsole(u"srv"_s);
        QCOMPARE(desk.opened, QList<QUrl>{QUrl(u"https://192.168.1.60/"_s)});
        QTest::qWait(150);
        QCOMPARE(pretended.consoleAsked, 0);
    }

    void aConsoleWithoutThePassCapabilityStillOpensAtItsDoor()
    {
        Pretended pretended;
        pretended.serverCapabilities = QStringList{};
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.openConsole(u"srv"_s);
        QCOMPARE(desk.opened, QList<QUrl>{QUrl(u"https://192.168.1.60/"_s)});
        QTest::qWait(150);
        QCOMPARE(pretended.consoleAsked, 0);
    }

    void aPassThatIsRefusedIsToldAndNothingIsOpened()
    {
        Pretended pretended;
        pretended.onConsole = [] {
            return Answered{502,
                            QByteArray("{\"code\": \"unreachable\", \"message\": \"The controller did not answer\"}")};
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.openConsole(u"srv"_s);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"error: Rack server: The controller did not answer"_s);
        QVERIFY(desk.opened.isEmpty());
        // And it can be tried again.
        pretended.onConsole = nullptr;
        desk.view.openConsole(u"srv"_s);
        QTRY_COMPARE(desk.opened.size(), 1);
    }

    void aPassThatLeadsElsewhereIsNotFollowed_data()
    {
        QTest::addColumn<QString>("named");
        QTest::newRow("another host") << u"https://elsewhere.example/console?pass=x"_s;
        QTest::newRow("another port") << u"https://192.168.1.60:8443/console?pass=x"_s;
        QTest::newRow("not encrypted") << u"http://192.168.1.60/console?pass=x"_s;
        QTest::newRow("a host that only begins so") << u"https://192.168.1.60.elsewhere.example/console"_s;
    }

    void aPassThatLeadsElsewhereIsNotFollowed()
    {
        QFETCH(const QString, named);
        Pretended pretended;
        pretended.onConsole = [named] { return Pretended::json({{u"url"_s, named}}); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.openConsole(u"srv"_s);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(),
                 u"error: Rack server: the server named a console that is not this machine's. Nothing was opened."_s);
        QVERIFY(desk.opened.isEmpty());
    }

    void whereNoBrowserOpensThatIsSaid()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.opens = false;
        desk.view.openConsole(u"kvm"_s);
        QCOMPARE(desk.told, QStringList{u"error: NAS: no browser could be opened for its console."_s});
        // And where the program was given no way to open one at all.
        desk.actions.openPagesWith({});
        desk.view.openConsole(u"kvm"_s);
        QCOMPARE(desk.told.size(), 2);
        QCOMPARE(desk.opened.size(), 1);
    }

    void aPassThatComesAfterTheWallWasLeftIsNotUsed()
    {
        Pretended pretended;
        pretended.onConsole = [] {
            Answered pass = Pretended::json({{u"url"_s, u"https://192.168.1.60/console?pass=one-use"_s}});
            pass.afterMs = 250;
            return pass;
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.openConsole(u"srv"_s);
        desk.actions.leave();
        QTest::qWait(500);
        QVERIFY(desk.opened.isEmpty());
        QCOMPARE(pretended.consoleAsked, 1);
        // Whoever comes to the wall next finds the way in free.
        core::Account account;
        account.id = u"u2"_s;
        account.role = u"admin"_s;
        desk.actions.enter(account);
        pretended.onConsole = nullptr;
        desk.view.openConsole(u"srv"_s);
        QTRY_COMPARE(desk.opened.size(), 1);
    }

    void theQkvmsOwnPageIsOpenedInTheBrowser()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.actions.openThePage(QUrl(u"http://192.168.1.10:8787/"_s));
        QCOMPARE(desk.opened, QList<QUrl>{QUrl(u"http://192.168.1.10:8787/"_s)});
        QVERIFY(desk.told.isEmpty());
        // What is no web address is not handed to Windows.
        desk.actions.openThePage(QUrl(u"file:///C:/Windows/System32/calc.exe"_s));
        desk.actions.openThePage(QUrl());
        QCOMPARE(desk.opened.size(), 1);
        QCOMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"error: No browser could be opened for the qKVM page."_s);
        // Nor is there a page where no browser opens.
        desk.opens = false;
        desk.actions.openThePage(QUrl(u"http://192.168.1.10:8787/"_s));
        QCOMPARE(desk.told.size(), 3);
    }

    void enlargeLeadsToTheMachineShownLarge()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        ui::WallTile card;
        card.key = u"kvm"_s;
        card.name = u"NAS"_s;
        desk.wall.machines()->setTiles({card});
        desk.view.open(u"kvm"_s);
        QVERIFY(desk.offers(u"enlarge"_s));
        desk.view.choose(u"enlarge"_s);
        QCOMPARE(desk.wall.enlarged(), u"kvm"_s);
        QVERIFY(desk.view.menu().isEmpty());
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());
        // Opened from there, the menu has no way there.
        desk.view.open(u"kvm"_s);
        QVERIFY(!desk.offers(u"enlarge"_s));
        QVERIFY(desk.offers(u"keys:ShiftLeft"_s));
    }

    void fromTheLargeViewTheServerIsToldTheMachineIsWatched()
    {
        // As the page sends it (app.js, perform): an action from the machine's large view says `watching`, and a PC
        // with Intel AMT then opens a session before it switches on, to show the start. From the wall it says nothing.
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        ui::WallTile card;
        card.key = u"kvm"_s;
        card.name = u"NAS"_s;
        desk.wall.machines()->setTiles({card});
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(pretended.acted.size(), 1);
        QVERIFY(!pretended.acted[0].contains(u"watching"_s));
        QTRY_VERIFY(!desk.view.busy());
        desk.view.close();
        desk.wall.enlarge(u"kvm"_s);
        QCOMPARE(desk.wall.enlarged(), u"kvm"_s);
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(pretended.acted.size(), 2);
        QCOMPARE(pretended.acted[1], (QJsonObject{{u"machine"_s, u"kvm"_s},
                                                  {u"action"_s, u"keys"_s},
                                                  {u"keys"_s, u"ShiftLeft"_s},
                                                  {u"watching"_s, true}}));
        // Of another machine than the one shown large: not watched.
        QTRY_VERIFY(!desk.view.busy());
        desk.view.close();
        desk.view.open(u"srv"_s);
        desk.view.choose(u"light:on"_s);
        QTRY_COMPARE(pretended.acted.size(), 3);
        QCOMPARE(pretended.acted[2][u"machine"_s].toString(), u"srv"_s);
        QVERIFY(!pretended.acted[2].contains(u"watching"_s));
    }

    void theVirtualMediaOpenInTheirBoxForWhoeverMayHaveThemAndGoWithThat()
    {
        // The menu's "Media..." opens the machine's media box (the page's mediaDialog), for whoever may have them by
        // the word of the windows' wall (useMedia: an admin of the machine's qKVM, in person); and the box goes the
        // moment whoever sits here may have them no longer, or the machine leaves the wall.
        Pretended pretended;
        pretended.capabilities = QStringList{u"keys"_s, u"text"_s, u"power"_s, u"ports"_s, u"media"_s};
        Desk desk(pretended, u"admin"_s, 50);
        core::ServerClient* media = &desk.client;
        desk.actions.useMedia([&](const QString&) { return media; });
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        QVERIFY(desk.offers(u"media"_s));
        desk.view.choose(u"media"_s);
        QVERIFY(desk.view.menu().isEmpty());
        ui::MediaView& box = *desk.view.media();
        QVERIFY(box.opened());
        QCOMPARE(box.title(), u"Virtual media of NAS"_s);
        QCOMPARE(box.key(), u"kvm"_s);
        QTRY_COMPARE(pretended.mediaAsked, 1);
        // The server lets go of whoever sits here as an admin: the box goes, and the row with it.
        media = nullptr;
        core::Account operating;
        operating.id = u"u1"_s;
        operating.name = u"anna"_s;
        operating.role = u"operator"_s;
        desk.actions.enter(operating);
        QVERIFY(!box.opened());
        desk.view.open(u"kvm"_s);
        QVERIFY(!desk.offers(u"media"_s));
        desk.view.close();
        // An admin once more: the box opens again, and goes with its machine when that leaves the wall.
        media = &desk.client;
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"media"_s);
        QVERIFY(box.opened());
        pretended.kvmIsThere = false;
        QTRY_VERIFY(!box.opened());
    }

    void theBiosSetupIsAskedFirstOpensTheLargeViewAndMayBeSetWithoutAnAnswer()
    {
        Pretended pretended;
        pretended.serverCapabilities =
            QStringList{u"power"_s, u"reset"_s, u"power_state"_s, u"locator"_s, u"bios_setup"_s};
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        ui::WallTile kvm;
        kvm.key = u"kvm"_s;
        kvm.name = u"NAS"_s;
        ui::WallTile srv;
        srv.key = u"srv"_s;
        srv.name = u"Rack server"_s;
        desk.wall.machines()->setTiles({kvm, srv});
        desk.view.open(u"srv"_s);
        QVERIFY(desk.offers(u"power:bios_setup"_s));
        desk.view.choose(u"power:bios_setup"_s);
        // Asked first, in the page's words: it runs, so it restarts.
        const QVariantMap question = desk.view.question();
        QCOMPARE(question.value(u"title"_s).toString(), u"Start into BIOS setup"_s);
        QCOMPARE(question.value(u"text"_s).toString(),
                 u"Rack server will restart now and start straight into its BIOS setup screen, once. Any unsaved work "
                 "on it is lost. Its large view opens so you can watch the setup."_s);
        QCOMPARE(question.value(u"yes"_s).toString(), u"Restart into setup"_s);
        QVERIFY(question.value(u"danger"_s).toBool());
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());
        QCOMPARE(desk.wall.enlarged(), QString());
        // Yes: its large view opens, the menu goes, and the server is told it is watched.
        desk.view.answer(true);
        QCOMPARE(desk.wall.enlarged(), u"srv"_s);
        QVERIFY(desk.view.menu().isEmpty());
        QTRY_COMPARE(pretended.acted.size(), 1);
        QCOMPARE(pretended.acted[0],
                 (QJsonObject{{u"machine"_s, u"srv"_s}, {u"action"_s, u"bios_setup"_s}, {u"watching"_s, true}}));
        QTRY_COMPARE(desk.told.constLast(), u"ok: Rack server: starting into BIOS setup"_s);

        // No answer of the server's: it may have been done, and the setup boot may be set.
        pretended.onAction = [](const QJsonObject&) {
            Answered none;
            none.hangUp = true;
            return none;
        };
        desk.view.open(u"srv"_s);
        desk.view.choose(u"power:bios_setup"_s);
        desk.view.answer(true);
        QTRY_COMPARE(pretended.acted.size(), 2);
        QTRY_COMPARE(desk.told.constLast(),
                     u"error: Rack server: no answer to \"Start into BIOS setup\". It may have been done, and its "
                     "setup boot may be set for the next boot: look at the machine before sending it again (an admin "
                     "finds how it ended in the activity log)."_s);
        // The server's `504 timeout`: its own words, which say whether the setup boot is left set.
        pretended.onAction = [](const QJsonObject&) {
            return Answered{504, QByteArray("{\"code\": \"timeout\", \"message\": \"Nothing was restarted, and the "
                                            "BIOS setup boot override may be set: the tile was changed meanwhile.\"}")};
        };
        desk.view.open(u"srv"_s);
        desk.view.choose(u"power:bios_setup"_s);
        desk.view.answer(true);
        QTRY_COMPARE(pretended.acted.size(), 3);
        QTRY_COMPARE(desk.told.constLast(), u"error: Rack server: Nothing was restarted, and the BIOS setup boot "
                                            "override may be set: the tile was changed meanwhile."_s);
        QTest::qWait(300);
        QCOMPARE(pretended.acted.size(), 3);
    }

    void aRestartClosesTheMenu()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        const QSignalSpy restarted(&desk.actions, &ui::MachineActions::restarted);
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"reboot"_s);
        QCOMPARE(desk.view.question().value(u"text"_s).toString(), u"Reboot NAS?"_s);
        desk.view.answer(true);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"ok: NAS: rebooting"_s);
        QVERIFY(desk.view.menu().isEmpty());
        // And whoever draws the wall is told which machine will be away for a while.
        QCOMPARE(restarted.count(), 1);
        QCOMPARE(restarted[0][0].toString(), u"kvm"_s);
    }

    void afterSomethingWasDoneTheMachinesAreLookedAtAgain()
    {
        // (The desk looks at its machines once a minute by itself.)
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        const int before = pretended.statusAsked;
        desk.view.showPort(u"kvm"_s, 1);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(pretended.statusAsked, before);
        // About a second after.
        QTRY_COMPARE_WITH_TIMEOUT(pretended.statusAsked, before + 1, 3000);
    }

    void leavingTheWallDropsWhatWasOpenAndWhatIsStillOut()
    {
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) { return late(300); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.showPort(u"srv"_s, 0);
        QCOMPARE(desk.view.notices()->count(), 1);
        desk.view.open(u"srv"_s);
        desk.view.choose(u"power:power_off"_s);
        QVERIFY(!desk.view.question().isEmpty());

        desk.actions.leave();
        QVERIFY(!desk.view.allowed());
        QVERIFY(desk.view.menu().isEmpty());
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.notices()->count(), 0);
        // The answer to what was out comes, and is nobody's: nothing is told of it.
        QTest::qWait(600);
        QCOMPARE(desk.told, QStringList{u"working: NAS: Wake screen…"_s});
        // Nothing is opened or sent for nobody.
        desk.view.open(u"kvm"_s);
        desk.view.answer(true);
        QVERIFY(desk.view.menu().isEmpty());
        QCOMPARE(pretended.acted.size(), 1);

        // Somebody is at the wall again: the machine that was acted on is free.
        core::Account account;
        account.id = u"u2"_s;
        account.role = u"admin"_s;
        desk.actions.enter(account);
        desk.view.showPort(u"kvm"_s, 1);
        QTRY_COMPARE(pretended.acted.size(), 2);
    }

    // --- several machines ---------------------------------------------------------------------------

    void thePickedMachinesHaveOneMenuBetweenThem()
    {
        Pretended pretended;
        pretended.more = 1;
        Desk desk(pretended, u"operator"_s, 50);
        QTRY_VERIFY(desk.isReady());
        // None is picked: no menu.
        desk.pick({});
        desk.view.openPicked();
        QVERIFY(desk.view.menu().isEmpty());
        // One alone has its own menu, with all that is its own.
        desk.pick({u"kvm"_s});
        desk.view.openPicked();
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        QVERIFY(desk.offers(u"port:1"_s));
        QTRY_COMPARE(pretended.wakeAsked, 1);
        // (Until the server's answer is here: what it has saved is known from then on.)
        QTRY_VERIFY(desk.offers(u"saved:a1"_s));
        QCOMPARE(pretended.savedAsked, 1);

        // Several: one menu, of what any of them can do, with the actions saved on the server.
        desk.pick({u"kvm"_s, u"srv"_s, u"n1"_s});
        desk.view.openPicked();
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"3 KVMs selected"_s);
        QVERIFY(desk.offers(u"keys:ShiftLeft"_s));
        QVERIFY(desk.offers(u"power:power_off"_s));
        QVERIFY(desk.offers(u"power:atx_power"_s));
        QVERIFY(desk.offers(u"light:on"_s));
        QVERIFY(desk.offers(u"saved:a1"_s));
        QVERIFY(!desk.offers(u"port:1"_s));
        QVERIFY(!desk.offers(u"enlarge"_s));
        QVERIFY(!desk.offers(u"console"_s));
        QTRY_COMPARE(pretended.savedAsked, 2);
        // Nobody is asked whom they can wake.
        QTest::qWait(150);
        QCOMPARE(pretended.wakeAsked, 1);
        QVERIFY(!desk.offers(u"wake:00:11:22:aa:bb:cc"_s));

        // It is the menu of those that are there. One leaves the wall: of two, and without the
        // button only that one had.
        pretended.kvmIsThere = false;
        QTRY_COMPARE(desk.view.menu().value(u"title"_s).toString(), u"2 KVMs selected"_s);
        QVERIFY(!desk.offers(u"power:atx_power"_s));
        QVERIFY(desk.offers(u"power:power_off"_s));
        // Another leaves: one is not several, and the menu goes.
        pretended.more = 0;
        QTRY_VERIFY(desk.view.menu().isEmpty());
        desk.view.choose(u"keys:ShiftLeft"_s);
        QVERIFY(desk.view.question().isEmpty());

        // Whoever may only look has no menu of several either, and asks the server nothing for one.
        Pretended other;
        Desk viewer(other, u"viewer"_s);
        QTRY_VERIFY(viewer.isReady());
        viewer.pick({u"kvm"_s, u"srv"_s});
        viewer.view.openPicked();
        QTest::qWait(150);
        QVERIFY(viewer.view.menu().isEmpty());
        QCOMPARE(other.savedAsked, 0);
    }

    void whatIsDoneToSeveralIsAskedAboutFirstAndThenShownAsItGoes()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s});
        desk.view.openPicked();
        // Even a key, which goes to one machine without a question.
        desk.view.choose(u"keys:ShiftLeft"_s);
        const QVariantMap question = desk.view.question();
        QCOMPARE(question.value(u"title"_s).toString(), u"Wake screen"_s);
        QCOMPARE(question.value(u"text"_s).toString(), u"Wake screen on 2 KVMs?"_s);
        QCOMPARE(question.value(u"yes"_s).toString(), u"Run on 2 KVMs"_s);
        QCOMPARE(question.value(u"danger"_s).toBool(), false);
        QCOMPARE(question.value(u"tick"_s).toString(), QString());
        QCOMPARE(question.value(u"paces"_s).toList().size(), 5);
        // The machines it is about, each with how it is, in the wall's order.
        QCOMPARE(question.value(u"targets"_s).toList(), (QVariantList{QVariantMap{{u"name"_s, u"NAS"_s},
                                                                                  {u"host"_s, u"192.168.1.22"_s},
                                                                                  {u"state"_s, u"online"_s},
                                                                                  {u"label"_s, u"Online"_s}},
                                                                      QVariantMap{{u"name"_s, u"Rack server"_s},
                                                                                  {u"host"_s, u"192.168.1.60"_s},
                                                                                  {u"state"_s, u"online"_s},
                                                                                  {u"label"_s, u"Online"_s}}}));
        // Nothing is sent, and the menu holds still behind the question.
        QVERIFY(desk.view.busy());
        QVERIFY(desk.view.run().isEmpty());
        QVERIFY(pretended.acted.isEmpty());

        // No: nothing is sent, and the menu is there again.
        desk.view.answer(false);
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"2 KVMs selected"_s);
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());
        QVERIFY(desk.view.run().isEmpty());

        // Yes: the menu goes, and how it goes is shown in its place.
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(desk.view.menu().isEmpty());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.run(u"title"_s), u"Wake screen · 2 KVMs"_s);
        QCOMPARE(desk.view.run().value(u"running"_s).toBool(), true);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"Done on all 2 KVMs."_s);
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: ok: Shift sent"_s, u"Rack server: ok: Shift sent"_s}));
        QCOMPARE(desk.run(u"again"_s), QString());
        // To each of them what goes to one, and with it what ties the two together in the server's
        // log: letters, figures and a dash, as the server takes it.
        QCOMPARE(pretended.acted.size(), 2);
        QCOMPARE(machinesOf(pretended.acted), (QSet<QString>{u"kvm"_s, u"srv"_s}));
        const QString batch = pretended.acted[0][u"batch"_s].toString();
        QVERIFY2(QRegularExpression(u"^[0-9a-z]{6,12}-[0-9a-z]{1,5}$"_s).match(batch).hasMatch(), qPrintable(batch));
        for (const QJsonObject& action : std::as_const(pretended.acted)) {
            QCOMPARE(action[u"action"_s].toString(), u"keys"_s);
            QCOMPARE(action[u"keys"_s].toString(), u"ShiftLeft"_s);
            QCOMPARE(action[u"batch"_s].toString(), batch);
        }
        // What its own box shows is not told in the corner as well.
        QVERIFY(desk.told.isEmpty());

        // Closed, it is gone; and the next run is another one in the log.
        desk.view.closeRun();
        QVERIFY(desk.view.run().isEmpty());
        QVERIFY(desk.told.isEmpty());
        desk.view.openPicked();
        desk.view.choose(u"keys:Enter"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_COMPARE(pretended.acted.size(), 4);
        QVERIFY(pretended.acted[3][u"batch"_s].toString() != batch);
        QCOMPARE(pretended.acted[3][u"batch"_s].toString(), pretended.acted[2][u"batch"_s].toString());
    }

    void whatCanLoseWorkOnSeveralTakesATickAsWellAsAYes()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s});
        desk.view.openPicked();
        desk.view.choose(u"power:atx_power_long"_s);
        QCOMPARE(desk.view.question().value(u"tick"_s).toString(), u"Yes, do this on all 2 machines"_s);
        QCOMPARE(desk.view.question().value(u"danger"_s).toBool(), true);
        // A yes without the tick is no yes, whatever a screen lets through.
        desk.view.answerWith(true, -1, false);
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());
        QVERIFY(desk.view.run().isEmpty());
        // Neither is the yes a question about one machine is answered with.
        desk.view.choose(u"power:atx_power_long"_s);
        desk.view.answer(true);
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());

        // A short press is no danger, and shuts a running system down all the same: a tick.
        desk.view.choose(u"power:atx_power"_s);
        QCOMPARE(desk.view.question().value(u"tick"_s).toString(), u"Yes, do this on all 2 machines"_s);
        QCOMPARE(desk.view.question().value(u"danger"_s).toBool(), false);
        desk.view.answerWith(true, -1, false);
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());

        // What is marked as a danger takes one whatever it is: keys that close a window.
        desk.view.choose(u"keys:AltLeft,F4"_s);
        QCOMPARE(desk.view.question().value(u"tick"_s).toString(), u"Yes, do this on all 2 machines"_s);
        desk.view.answerWith(true, -1, false);
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());

        // With the tick it is done.
        desk.view.choose(u"power:atx_power_long"_s);
        desk.view.answerWith(true, -1, true);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"Done on all 2 KVMs."_s);
        QCOMPARE(pretended.acted.size(), 2);
        QCOMPARE(machinesOf(pretended.acted), (QSet<QString>{u"kvm"_s, u"srv"_s}));
        QCOMPARE(pretended.acted[0][u"action"_s].toString(), u"atx_power_long"_s);
        QCOMPARE(pretended.acted[1][u"action"_s].toString(), u"atx_power_long"_s);
    }

    void aRunGoesAFewAtATimeOrOneAfterTheOther()
    {
        Pretended pretended;
        pretended.more = 2;
        pretended.onAction = [](const QJsonObject&) { return late(600); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"n1"_s, u"n2"_s});
        // A few at a time: all three are out together.
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: : Sending…"_s, u"Node 1: : Sending…"_s, u"Node 2: : Sending…"_s}));
        QTRY_COMPARE(pretended.acted.size(), 3);
        QVERIFY(!desk.runIsOver());
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"Done on all 3 KVMs."_s);
        desk.view.closeRun();

        // One after the other: the second goes when the first was answered, and not before.
        pretended.acted.clear();
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, 0, false);
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: : Sending…"_s, u"Node 1: : Waiting…"_s, u"Node 2: : Waiting…"_s}));
        QTRY_COMPARE(pretended.acted.size(), 1);
        QTest::qWait(300);
        QCOMPARE(pretended.acted.size(), 1);
        QTRY_COMPARE(pretended.acted.size(), 2);
        QCOMPARE(desk.run(u"summary"_s), u"1 of 3 done…"_s);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"Done on all 3 KVMs."_s);
        // In the order they were shown in.
        QCOMPARE(pretended.acted[0][u"machine"_s].toString(), u"kvm"_s);
        QCOMPARE(pretended.acted[1][u"machine"_s].toString(), u"n1"_s);
        QCOMPARE(pretended.acted[2][u"machine"_s].toString(), u"n2"_s);
        desk.view.closeRun();

        // With a pause between them: said, and kept.
        pretended.onAction = nullptr;
        pretended.acted.clear();
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, 30, false);
        QTRY_COMPARE(desk.run(u"summary"_s), u"1 of 3 done… Next in 30 s."_s);
        QTest::qWait(300);
        QCOMPARE(pretended.acted.size(), 1);
        // Stopped: what was not sent is not sent, and is offered again.
        desk.view.stopRun();
        QVERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"1 succeeded, 2 not sent."_s);
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: ok: Shift sent"_s, u"Node 1: skipped: Not sent"_s,
                                              u"Node 2: skipped: Not sent"_s}));
        QCOMPARE(desk.run(u"again"_s), u"Send the rest"_s);
        // Sent once more they go as the run went: one, and a pause. In the log they are of the same run.
        desk.view.retryRun();
        QTRY_COMPARE(desk.run(u"summary"_s), u"1 of 2 done… Next in 30 s."_s);
        QCOMPARE(pretended.acted.size(), 2);
        QCOMPARE(pretended.acted[1][u"machine"_s].toString(), u"n1"_s);
        QCOMPARE(pretended.acted[1][u"batch"_s].toString(), pretended.acted[0][u"batch"_s].toString());
        // Its box closed in the pause: that was all, and how it ended is said in the corner.
        QVERIFY(desk.told.isEmpty());
        desk.view.closeRun();
        QVERIFY(desk.view.run().isEmpty());
        QCOMPARE(desk.told, QStringList{u"error: Wake screen: 1 succeeded, 1 not sent."_s});
        QTest::qWait(200);
        QCOMPARE(pretended.acted.size(), 2);

        // A pause nobody would wait for is cut to an hour.
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, 1'000'000, false);
        QTRY_COMPARE(desk.run(u"summary"_s), u"1 of 3 done… Next in 3600 s."_s);
        desk.view.closeRun();
    }

    void aRunWhoseBoxIsClosedSendsNothingMoreAndSaysInTheCornerHowItEnded()
    {
        Pretended pretended;
        pretended.more = 1;
        pretended.onAction = [](const QJsonObject&) { return late(400); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s, u"n1"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, 0, false);
        QTRY_COMPARE(pretended.acted.size(), 1);
        // Closed while the first is on its way: the box is gone at once.
        desk.view.closeRun();
        QVERIFY(desk.view.run().isEmpty());
        QVERIFY(desk.told.isEmpty());

        // While that answer is out, nothing else is begun on several machines: said before anything
        // is asked about or put into a box, and nothing is sent.
        desk.view.openPicked();
        desk.view.choose(u"keys:Enter"_s);
        QCOMPARE(desk.told, QStringList{u"info: \"Wake screen\" is still being done to several machines. Wait for "
                                        "it to finish."_s});
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        desk.view.choose(u"typeText"_s);
        QCOMPARE(desk.told.size(), 2);
        QVERIFY(desk.view.form().isEmpty());
        desk.view.answerWith(true, -1, true);
        QVERIFY(desk.view.run().isEmpty());
        // (Their menu stays, for when it is over.)
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"3 KVMs selected"_s);

        // What was on its way is answered, and that was all: with its box, what the box offered
        // is gone too, and "once more" is said to nothing.
        QTRY_COMPARE(desk.told.size(), 3);
        QCOMPARE(desk.told.constLast(), u"error: Wake screen: 1 succeeded, 2 not sent."_s);
        QVERIFY(desk.view.run().isEmpty());
        desk.view.retryRun();
        QTest::qWait(500);
        QCOMPARE(pretended.acted.size(), 1);
        QVERIFY(desk.view.run().isEmpty());

        // All of them out when the box is closed: all are answered, and that is told as done.
        desk.view.choose(u"keys:Enter"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_COMPARE(pretended.acted.size(), 4);
        desk.view.closeRun();
        QTRY_COMPARE(desk.told.size(), 4);
        QCOMPARE(desk.told.constLast(), u"ok: Enter: Done on all 3 KVMs."_s);
    }

    void aRunSendsNothingOnTopOfWhatIsOutToAMachineAndNeitherDoesItsCard()
    {
        Pretended pretended;
        // Showing another computer takes its time here; a key is quick.
        pretended.onAction = [](const QJsonObject& action) {
            return late(action[u"action"_s].toString() == u"port"_s ? 700 : 30);
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        // A number on the KVM's card was clicked, and its answer is out.
        desk.view.showPort(u"kvm"_s, 1);
        desk.pick({u"kvm"_s, u"srv"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: bad: \"Show Build box\" is still under way here"_s,
                                              u"Rack server: ok: Shift sent"_s}));
        QCOMPARE(desk.run(u"summary"_s), u"1 succeeded, 1 failed."_s);
        QCOMPARE(desk.run(u"again"_s), u"Retry failed"_s);
        QCOMPARE(pretended.acted.size(), 2);
        // Once that is answered the machine takes it, and it alone is sent once more.
        QTRY_VERIFY(desk.told.contains(u"ok: NAS: showing Build box"_s));
        desk.view.retryRun();
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"Done on the KVM."_s);
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: ok: Shift sent"_s, u"Rack server: ok: Shift sent"_s}));
        QCOMPARE(desk.run(u"again"_s), QString());
        QCOMPARE(pretended.acted.size(), 3);
        QCOMPARE(pretended.acted[2][u"machine"_s].toString(), u"kvm"_s);
        desk.view.closeRun();

        // The other way round: what a run has out to a machine is not sent on top of from its card.
        pretended.onAction = [](const QJsonObject&) { return late(500); };
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        desk.view.showPort(u"kvm"_s, 2);
        QCOMPARE(desk.told.constLast(), u"info: NAS: \"Wake screen\" is still under way. Wait for its answer."_s);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(pretended.acted.size(), 5);
        // And is free again when the run has its answer.
        desk.view.showPort(u"kvm"_s, 2);
        QTRY_COMPARE(pretended.acted.size(), 6);
        QCOMPARE(pretended.acted[5][u"action"_s].toString(), u"port"_s);
    }

    void inARunARefusalIsOfferedAgainAndNoAnswerIsNot()
    {
        Pretended pretended;
        pretended.more = 1;
        pretended.onAction = [](const QJsonObject& action) -> Answered {
            const QString machine = action[u"machine"_s].toString();
            if (machine == u"srv"_s) {
                return {400, QByteArray("{\"code\": \"unsupported\", \"message\": \"Rack server has no keyboard to "
                                        "press keys on.\"}")};
            }
            if (machine == u"n1"_s) {
                Answered none;
                none.hangUp = true;
                return none;
            }
            return {200, QByteArray("{\"ok\": true, \"note\": \"already awake\"}")};
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s, u"n1"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        // One after the other, each answered before the next goes. (A line that falls takes the server for lost,
        // and a question to it that had not yet gone out is then told as no answer too: so it went in a gate, where
        // the three went at once and the last fell before the first had left.)
        desk.view.answerWith(true, 0, false);
        QTRY_VERIFY(desk.runIsOver());
        // What the server adds is said; a refusal in the server's words; and no answer as what it is.
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: ok: already awake"_s,
                                              u"Rack server: bad: Rack server has no keyboard to press keys on."_s,
                                              u"Node 1: unknown: No answer. It may have been done."_s}));
        QCOMPARE(desk.run(u"summary"_s), u"1 succeeded, 1 failed, 1 without an answer."_s);
        QCOMPARE(desk.run(u"again"_s), u"Retry failed"_s);
        QCOMPARE(pretended.acted.size(), 3);

        // Sent once more is what was refused, and that alone: what got no answer may have been done.
        pretended.onAction = nullptr;
        desk.view.retryRun();
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"Done on the KVM."_s);
        QCOMPARE(pretended.acted.size(), 4);
        QCOMPARE(pretended.acted[3][u"machine"_s].toString(), u"srv"_s);
        QCOMPARE(desk.runRows().value(2), u"Node 1: unknown: No answer. It may have been done."_s);
        // Nothing is left to send once more.
        QCOMPARE(desk.run(u"again"_s), QString());
        desk.view.retryRun();
        QTest::qWait(200);
        QCOMPARE(pretended.acted.size(), 4);
    }

    void inARunTheServersWordsForWhatWentOutAreNotOfferedAgain()
    {
        // `timeout` and `dropped` in the server's own shape: what it sent went out, and no answer
        // came, or a broken one. Neither is a refusal, and neither is pressed a second time.
        Pretended pretended;
        pretended.more = 1;
        pretended.onAction = [](const QJsonObject& action) -> Answered {
            const QString machine = action[u"machine"_s].toString();
            if (machine == u"kvm"_s) {
                return {502, QByteArray("{\"code\": \"dropped\", \"message\": \"The connection to the JetKVM "
                                        "ended\"}")};
            }
            if (machine == u"n1"_s) {
                return {504, QByteArray("{\"code\": \"timeout\", \"message\": \"Node 1 was being changed.\"}")};
            }
            return {400, QByteArray("{\"code\": \"unsupported\", \"message\": \"Rack server has no keyboard to "
                                    "press keys on.\"}")};
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s, u"n1"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: unknown: No answer. It may have been done."_s,
                                              u"Rack server: bad: Rack server has no keyboard to press keys on."_s,
                                              u"Node 1: unknown: No answer. It may have been done."_s}));
        QCOMPARE(desk.run(u"summary"_s), u"0 succeeded, 1 failed, 2 without an answer."_s);
        QCOMPARE(desk.run(u"again"_s), u"Retry failed"_s);
        QCOMPARE(pretended.acted.size(), 3);

        // Sent once more is what was refused, and that alone.
        pretended.onAction = nullptr;
        desk.view.retryRun();
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(pretended.acted.size(), 4);
        QCOMPARE(pretended.acted[3][u"machine"_s].toString(), u"srv"_s);
        QCOMPARE(desk.runRows().value(0), u"NAS: unknown: No answer. It may have been done."_s);
        QCOMPARE(desk.runRows().value(2), u"Node 1: unknown: No answer. It may have been done."_s);
        QCOMPARE(desk.run(u"again"_s), QString());
        desk.view.retryRun();
        QTest::qWait(200);
        QCOMPARE(pretended.acted.size(), 4);
    }

    void inARunTheServersOwnErrorIsNotOfferedAgain()
    {
        // The merged review's MEDIUM 5 (2026-11-06): a bulk power run. On one machine the press goes out and the
        // server then fails (writing its history, say): its middleware answers `500 internal` for any error in a
        // handler. That machine is not counted as failed, and "Retry failed" does not press it again.
        Pretended pretended;
        pretended.more = 1;
        pretended.onAction = [](const QJsonObject& action) -> Answered {
            if (action[u"machine"_s].toString() == u"kvm"_s) {
                return {500, QByteArray("{\"code\": \"internal\", \"message\": \"Something went wrong inside qKVM. "
                                        "The details are in its log.\"}")};
            }
            if (action[u"machine"_s].toString() == u"n1"_s) {
                return {409, QByteArray("{\"code\": \"busy\", \"message\": \"Something else is being done to Node 1 "
                                        "at this moment.\"}")};
            }
            return Pretended::json({{u"ok"_s, true}, {u"note"_s, QJsonValue()}});
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"n1"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.runRows(),
                 (QStringList{u"NAS: unknown: No answer. It may have been done."_s,
                              u"Node 1: bad: Something else is being done to Node 1 at this moment."_s}));
        QCOMPARE(desk.run(u"summary"_s), u"0 succeeded, 1 failed, 1 without an answer."_s);
        QCOMPARE(pretended.acted.size(), 2);
        pretended.onAction = nullptr;
        desk.view.retryRun();
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(pretended.acted.size(), 3);
        QCOMPARE(pretended.acted[2][u"machine"_s].toString(), u"n1"_s);
        QCOMPARE(desk.runRows().value(0), u"NAS: unknown: No answer. It may have been done."_s);
        QCOMPARE(desk.run(u"again"_s), QString());

        // And one machine's: told as no answer, and not as the server's sentence, which would read like a refusal.
        pretended.onAction = [](const QJsonObject&) {
            return Answered{500, QByteArray("{\"code\": \"internal\", \"message\": \"Something went wrong inside "
                                            "qKVM. The details are in its log.\"}")};
        };
        const qsizetype toldBefore = desk.told.size();
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"power:atx_power"_s);
        desk.view.answer(true);
        QTRY_COMPARE(desk.told.size(), toldBefore + 2);
        QCOMPARE(desk.told.constLast(), u"error: NAS: no answer to \"Press power button\". It may have been done all "
                                        "the same: look before sending it again."_s);
        QTest::qWait(300);
        QCOMPARE(pretended.acted.size(), 4);
    }

    void keysAndATextForSeveralGoThroughTheirBoxes()
    {
        Pretended pretended;
        Desk desk(pretended, u"operator"_s, 50);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s});
        desk.view.openPicked();
        desk.view.choose(u"sendKeys"_s);
        // The box opens in the menu's place, for all of them.
        QVERIFY(desk.view.menu().isEmpty());
        QCOMPARE(desk.view.form(),
                 (QVariantMap{{u"kind"_s, u"keys"_s}, {u"machine"_s, u"2 KVMs"_s}, {u"problem"_s, QString()}}));
        desk.view.sendKeys(QString());
        QCOMPARE(desk.view.form(), (QVariantMap{{u"kind"_s, u"keys"_s},
                                                {u"machine"_s, u"2 KVMs"_s},
                                                {u"problem"_s, u"Choose at least one key."_s}}));
        desk.view.sendKeys(u"F5"_s);
        QVERIFY(desk.view.form().isEmpty());
        QCOMPARE(desk.view.question().value(u"text"_s).toString(), u"Send F5 on 2 KVMs?"_s);
        QCOMPARE(desk.view.question().value(u"targets"_s).toList().size(), 2);
        QCOMPARE(desk.view.question().value(u"tick"_s).toString(), QString());
        QVERIFY(pretended.acted.isEmpty());
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"title"_s), u"Send F5 · 2 KVMs"_s);
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: ok: F5 sent"_s, u"Rack server: ok: F5 sent"_s}));
        QCOMPARE(pretended.acted.size(), 2);
        QCOMPARE(pretended.acted[0][u"keys"_s].toString(), u"F5"_s);
        QCOMPARE(pretended.acted[1][u"keys"_s].toString(), u"F5"_s);
        QVERIFY(!pretended.acted[0][u"batch"_s].toString().isEmpty());
        desk.view.closeRun();

        // A text is shown once more, and takes a tick: it does whatever the window it lands in
        // makes of it.
        desk.view.openPicked();
        desk.view.choose(u"typeText"_s);
        QCOMPARE(desk.view.form(),
                 (QVariantMap{{u"kind"_s, u"text"_s}, {u"machine"_s, u"2 KVMs"_s}, {u"problem"_s, QString()}}));
        desk.view.typeText(u"dir"_s, true);
        QVERIFY(desk.view.form().isEmpty());
        QCOMPARE(desk.view.question().value(u"text"_s).toString(), u"Type this on 2 KVMs?"_s);
        QCOMPARE(desk.view.question().value(u"detail"_s).toString(), u"dir ⏎"_s);
        QCOMPARE(desk.view.question().value(u"tick"_s).toString(), u"Yes, do this on all 2 machines"_s);
        desk.view.answerWith(true, -1, false);
        QTest::qWait(150);
        QCOMPARE(pretended.acted.size(), 2);
        desk.view.openPicked();
        desk.view.choose(u"typeText"_s);
        desk.view.typeText(u"dir"_s, true);
        desk.view.answerWith(true, 0, true);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(pretended.acted.size(), 4);
        QCOMPARE(pretended.acted[2][u"machine"_s].toString(), u"kvm"_s);
        QCOMPARE(pretended.acted[2][u"action"_s].toString(), u"text"_s);
        QCOMPARE(pretended.acted[2][u"text"_s].toString(), u"dir"_s);
        QCOMPARE(pretended.acted[2][u"enter"_s].toBool(), true);
        QCOMPARE(pretended.acted[3][u"machine"_s].toString(), u"srv"_s);
        QCOMPARE(pretended.acted[3][u"text"_s].toString(), u"dir"_s);
        // What was typed is told nowhere: that it was, is.
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: ok: text typed"_s, u"Rack server: ok: text typed"_s}));
        desk.view.closeRun();

        // A box that is open for several goes when one of them leaves the wall: they are no longer
        // those it was opened for.
        desk.view.openPicked();
        desk.view.choose(u"sendKeys"_s);
        QVERIFY(!desk.view.form().isEmpty());
        pretended.kvmIsThere = false;
        QTRY_VERIFY(desk.view.form().isEmpty());
        desk.view.sendKeys(u"F5"_s);
        QVERIFY(desk.view.question().isEmpty());
        QTest::qWait(150);
        QCOMPARE(pretended.acted.size(), 4);
    }

    void aRestartOfSeveralIsARestartOfEach()
    {
        Pretended pretended;
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        const QSignalSpy restarted(&desk.actions, &ui::MachineActions::restarted);
        const int looked = pretended.statusAsked;
        desk.pick({u"kvm"_s, u"srv"_s});
        desk.view.openPicked();
        desk.view.choose(u"reboot"_s);
        QCOMPARE(desk.view.question().value(u"text"_s).toString(), u"Reboot 2 KVMs?"_s);
        // (Everybody at their consoles is cut off: a tick.)
        QCOMPARE(desk.view.question().value(u"tick"_s).toString(), u"Yes, do this on all 2 machines"_s);
        desk.view.answerWith(true, -1, true);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.runRows(), (QStringList{u"NAS: ok: rebooting"_s, u"Rack server: ok: rebooting"_s}));
        QCOMPARE(restarted.count(), 2);
        QCOMPARE((QSet<QString>{restarted[0][0].toString(), restarted[1][0].toString()}),
                 (QSet<QString>{u"kvm"_s, u"srv"_s}));
        // And how the machines are now is looked at before the wall's next look at them.
        QTRY_VERIFY_WITH_TIMEOUT(pretended.statusAsked > looked, 3000);

        // A machine that refused was not restarted, and is not shown as restarting.
        pretended.onAction = [](const QJsonObject& action) -> Answered {
            if (action[u"machine"_s].toString() == u"srv"_s) {
                return {409, QByteArray("{\"code\": \"busy\", \"message\": \"Not now.\"}")};
            }
            return {200, QByteArray("{\"ok\": true, \"note\": null}")};
        };
        desk.view.closeRun();
        desk.view.openPicked();
        desk.view.choose(u"reboot"_s);
        desk.view.answerWith(true, -1, true);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"1 succeeded, 1 failed."_s);
        QCOMPARE(restarted.count(), 3);
        QCOMPARE(restarted[2][0].toString(), u"kvm"_s);
    }

    void aMachineThatLeftWhileTheQuestionStoodIsNotSentTo()
    {
        Pretended pretended;
        pretended.more = 1;
        Desk desk(pretended, u"operator"_s, 50);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s, u"n1"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        QCOMPARE(desk.view.question().value(u"targets"_s).toList().size(), 3);
        pretended.kvmIsThere = false;
        QTRY_COMPARE(desk.feed.machines().size(), 2);
        // (The question stands for those that are left.)
        QVERIFY(!desk.view.question().isEmpty());
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.runRows(), (QStringList{u"Rack server: ok: Shift sent"_s, u"Node 1: ok: Shift sent"_s}));
        QCOMPARE(machinesOf(pretended.acted), (QSet<QString>{u"srv"_s, u"n1"_s}));

        // Where what is left is one machine, that is not several: the question goes with the menu,
        // and a yes that comes after it is a yes to nothing.
        desk.view.closeRun();
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        QCOMPARE(desk.view.question().value(u"targets"_s).toList().size(), 2);
        pretended.more = 0;
        QTRY_VERIFY(desk.view.menu().isEmpty());
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        desk.view.answerWith(true, -1, false);
        QTest::qWait(150);
        QCOMPARE(pretended.acted.size(), 2);
        QVERIFY(desk.view.run().isEmpty());
    }

    void leavingTheWallForgetsWhatWasBeingDoneToSeveral()
    {
        Pretended pretended;
        pretended.more = 1;
        // (Showing another computer takes its time here.)
        pretended.onAction = [](const QJsonObject& action) {
            return late(action[u"action"_s].toString() == u"port"_s ? 1000 : 300);
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s, u"n1"_s});
        core::Account account;
        account.id = u"u2"_s;
        account.role = u"admin"_s;

        // A question about several goes with the wall, and a yes to it afterwards is a yes to nothing.
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        QVERIFY(!desk.view.question().isEmpty());
        desk.actions.leave();
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(desk.view.menu().isEmpty());
        QVERIFY(!desk.view.busy());
        desk.view.answerWith(true, -1, true);
        desk.actions.enter(account);
        desk.view.answerWith(true, -1, true);
        QTest::qWait(150);
        QVERIFY(pretended.acted.isEmpty());

        // A run that is under way: its box goes, nothing more is sent, and nothing is told of the
        // answer that was out.
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, 0, false);
        QTRY_COMPARE(pretended.acted.size(), 1);
        QVERIFY(!desk.view.run().isEmpty());
        desk.actions.leave();
        QVERIFY(desk.view.run().isEmpty());
        QVERIFY(desk.told.isEmpty());

        // Somebody is at the wall again at once, and has the KVM show another computer. The answer
        // to what the run sent before comes meanwhile: it is nobody's, and does not make the KVM
        // free for a second thing while the first of whoever is there now is still out.
        desk.actions.enter(account);
        QVERIFY(desk.view.run().isEmpty());
        desk.view.showPort(u"kvm"_s, 1);
        QCOMPARE(desk.told, QStringList{u"working: NAS: Show Build box…"_s});
        QTest::qWait(600);
        desk.view.showPort(u"kvm"_s, 2);
        QCOMPARE(desk.told.constLast(), u"info: NAS: \"Show Build box\" is still under way. Wait for its answer."_s);
        QCOMPARE(pretended.acted.size(), 2);
        QTRY_VERIFY(desk.told.contains(u"ok: NAS: showing Build box"_s));
        // Nothing more was sent of the run, and nothing is shown or told of it.
        QCOMPARE(pretended.acted.size(), 2);
        QVERIFY(desk.view.run().isEmpty());
        QCOMPARE(desk.told.size(), 3);

        // The machines are free, and a run can be begun.
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"Done on all 3 KVMs."_s);
        QCOMPARE(pretended.acted.size(), 5);
    }

    // --- this PC's own wall -------------------------------------------------------------------------

    void aKvmOfThisPcsOwnThatDidNotAnswerMayHaveDoneItAllTheSame()
    {
        // No server: the KVM is spoken to by the program itself.
        MadeUpKvm kvm;
        kvm.atx = true;
        OwnDesk desk;
        const QString nas = desk.add(kvm, u"NAS"_s);
        desk.sitDown();
        QTRY_VERIFY(desk.isReady());

        // What the KVM says no to is a no, in the KVM's words.
        kvm.instead.insert("/api/atx/click"_ba, core::testing::refusal(409, u"IsBusyError"_s, u"Busy"_s));
        desk.view.open(nas);
        desk.view.choose(u"power:atx_reset"_s);
        QVERIFY(!desk.view.question().isEmpty());
        desk.view.answer(true);
        QTRY_COMPARE(desk.told.size(), 2);
        QCOMPARE(desk.told.constLast(), u"error: NAS: Busy"_s);

        // The KVM takes the press of its power button, and the line falls before it has answered.
        Answered gone;
        gone.hangUp = true;
        kvm.instead.insert("/api/atx/click"_ba, gone);
        desk.view.choose(u"power:atx_power"_s);
        QVERIFY(!desk.view.question().isEmpty());
        desk.view.answer(true);
        // Told as what it is, in the words a server's unanswered action is told in. (What the KVM's
        // driver says of it, "The device closed the connection", would read like a refusal; and a
        // QKVM server's rule for "no answer" knows nothing of a KVM's fallen line.)
        QTRY_COMPARE(desk.told.size(), 4);
        QCOMPARE(desk.told.constLast(), u"error: NAS: no answer to \"Press power button\". It may have been done all "
                                        "the same: look before sending it again."_s);
        QVERIFY(!desk.view.busy());
        QVERIFY(desk.underWay.isEmpty());
        // And it was asked that once, after the reset that was refused: a power button pressed
        // twice undoes the first press.
        QTest::qWait(300);
        QCOMPARE(kvm.asked("/api/atx/click"), 2);
    }

    void inARunOverThisPcsOwnKvmsOneThatDidNotAnswerIsNotOfferedAgain()
    {
        // Three KVMs of this PC's own. The first does what it is asked; the second says no; the
        // third takes the keys, and the line falls before it has answered.
        const MadeUpKvm does;
        MadeUpKvm refuses;
        refuses.instead.insert("/api/hid/events/send_shortcut"_ba,
                               core::testing::refusal(400, u"BadRequestError"_s, u"No keyboard is connected"_s));
        MadeUpKvm falls;
        Answered gone;
        gone.hangUp = true;
        falls.instead.insert("/api/hid/events/send_shortcut"_ba, gone);
        OwnDesk desk;
        const QString first = desk.add(does, u"Build server"_s);
        const QString second = desk.add(refuses, u"NAS"_s);
        const QString third = desk.add(falls, u"Backup box"_s);
        desk.sitDown();
        QTRY_VERIFY(desk.isReady());
        desk.pick({first, second, third});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        // Done, a refusal in the KVM's words, and no answer as what it is.
        QCOMPARE(desk.runRows(),
                 (QStringList{u"Build server: ok: Shift sent"_s, u"NAS: bad: No keyboard is connected"_s,
                              u"Backup box: unknown: No answer. It may have been done."_s}));
        QCOMPARE(desk.run(u"summary"_s), u"1 succeeded, 1 failed, 1 without an answer."_s);
        QCOMPARE(desk.run(u"again"_s), u"Retry failed"_s);
        QCOMPARE(does.did, QList<QByteArray>{"keys ShiftLeft"_ba});

        // Sent once more is what was refused, and that alone: the KVM that did not answer may have
        // pressed the keys, and is not asked to press them a second time.
        refuses.instead.clear();
        desk.view.retryRun();
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.run(u"summary"_s), u"Done on the KVM."_s);
        QCOMPARE(desk.runRows().value(2), u"Backup box: unknown: No answer. It may have been done."_s);
        QCOMPARE(does.asked("/api/hid/events/send_shortcut"), 1);
        QCOMPARE(refuses.asked("/api/hid/events/send_shortcut"), 2);
        QCOMPARE(falls.asked("/api/hid/events/send_shortcut"), 1);
        // Nothing is left to send once more.
        QCOMPARE(desk.run(u"again"_s), QString());
        desk.view.retryRun();
        QTest::qWait(200);
        QCOMPARE(falls.asked("/api/hid/events/send_shortcut"), 1);
        QCOMPARE(refuses.asked("/api/hid/events/send_shortcut"), 2);
    }

    void whenTheMachinesComeFromAnotherPlaceWhatWasAskedOfTheOneBeforeIsLetGoOf()
    {
        // A QKVM server's wall, and this PC's own wall of one KVM.
        Pretended pretended;
        pretended.onAction = [](const QJsonObject&) { return late(300); };
        const MadeUpKvm kvm;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::LocalWall own(settings, secrets, quick());
        const QString mine = own.add(known(kvm, u"Build server"_s), u"secret"_s).value();
        // (A desk that looks at its machines often: a KVM of this PC's own is "connecting" at the
        // first look, and there at the next.)
        Desk desk(pretended, u"operator"_s, 50);
        QTRY_VERIFY(desk.isReady());
        // Something is on its way to the server's KVM, and a question stands over the menu of the
        // server's other machine.
        desk.view.showPort(u"kvm"_s, 1);
        desk.view.open(u"srv"_s);
        desk.view.choose(u"power:power_off"_s);
        QVERIFY(!desk.view.question().isEmpty());
        QVERIFY(desk.underWay.contains(u"kvm"_s));
        QCOMPARE(desk.told.size(), 1);

        // The machines come from this PC from now on. That is the server's wall left: what was
        // open is closed, nothing is under way that is anybody's, and nobody may act until
        // somebody is at the new wall.
        desk.feed.stop();
        desk.place.setPlace(own);
        desk.actions.leave();
        QVERIFY(!desk.view.allowed());
        QVERIFY(desk.view.menu().isEmpty());
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QVERIFY(desk.underWay.isEmpty());
        QCOMPARE(desk.view.notices()->count(), 0);
        // A yes to the question that stood is a yes to nothing, and the server's answer to what
        // was on its way comes and is told to nobody.
        desk.view.answer(true);
        QTest::qWait(500);
        QCOMPARE(desk.told.size(), 1);
        QCOMPARE(pretended.acted.size(), 1);

        // Somebody is at the new wall. What is done there goes to the KVM itself, and whether it
        // may have been done is the KVM's driver's to say; the server hears nothing of it.
        desk.feed.start();
        core::Account account;
        account.role = u"admin"_s;
        desk.actions.enter(account);
        QTRY_COMPARE(desk.feed.status().value(mine).state, u"online"_s);
        const qsizetype ofTheServer = desk.server.asked.size();
        desk.view.open(mine);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"Build server"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(desk.told.size(), 3);
        QCOMPARE(desk.told.constLast(), u"ok: Build server: Shift sent"_s);
        QCOMPARE(kvm.did, QList<QByteArray>{"keys ShiftLeft"_ba});
        QCOMPARE(desk.server.asked.size(), ofTheServer);
        QCOMPARE(pretended.acted.size(), 1);
    }

    void aKvmOfThisPcsOwnWallIsOfferedTheBoxThatChangesIt()
    {
        const MadeUpKvm kvm;
        const MadeUpKvm other;
        OwnDesk desk;
        const QString nas = desk.add(kvm, u"NAS"_s);
        const QString build = desk.add(other, u"Build server"_s);
        desk.sitDown();
        QTRY_VERIFY(desk.isReady());
        const QSignalSpy editAsked(&desk.view, &ui::ActionsView::editAsked);

        // Among the rows that lead elsewhere, after the way to the machine shown large.
        desk.view.open(nas);
        QCOMPARE(waysOf(desk.view), (QStringList{u"console"_s, u"enlarge"_s, u"edit"_s}));
        const QVariantMap row =
            desk.view.menu().value(u"sections"_s).toList().value(0).toMap().value(u"rows"_s).toList().value(2).toMap();
        QCOMPARE(row.value(u"label"_s).toString(), u"Edit…"_s);
        QVERIFY(row.value(u"enabled"_s).toBool());
        QVERIFY(!row.value(u"danger"_s).toBool());

        // Chosen, the menu goes and the box is asked for: for that machine, and not for its
        // certificate alone. Nothing is sent to the KVM, nothing is asked about, nothing is told.
        desk.view.choose(u"edit"_s);
        QVERIFY(desk.view.menu().isEmpty());
        QCOMPARE(editAsked.count(), 1);
        QCOMPARE(editAsked.at(0).at(0).toString(), nas);
        QCOMPARE(editAsked.at(0).at(1).toBool(), false);
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QTest::qWait(150);
        QVERIFY(kvm.did.isEmpty());
        QVERIFY(desk.told.isEmpty());
        QVERIFY(desk.underWay.isEmpty());

        // A machine that is shown large has it too: the box opens over that.
        desk.pick({});
        desk.wall.setSelecting(false);
        desk.wall.enlarge(build);
        QCOMPARE(desk.wall.enlarged(), build);
        desk.view.open(build);
        QCOMPARE(waysOf(desk.view), (QStringList{u"console"_s, u"edit"_s}));
        desk.view.choose(u"edit"_s);
        QCOMPARE(editAsked.count(), 2);
        QCOMPARE(editAsked.at(1).at(0).toString(), build);
        QCOMPARE(desk.wall.enlarged(), build);

        // Several machines that are picked are changed one by one: their menu has no such row.
        desk.wall.closeEnlarged();
        desk.pick({nas, build});
        desk.view.openPicked();
        QVERIFY(!desk.view.menu().isEmpty());
        QVERIFY(!waysOf(desk.view).contains(u"edit"_s));
        desk.view.choose(u"edit"_s);
        QCOMPARE(editAsked.count(), 2);
    }

    void noMachineOfAServersWallIsOfferedTheBoxThatChangesAKvm()
    {
        // What a QKVM server lists is changed on the server's page, by whoever may there.
        Pretended pretended;
        Desk desk(pretended, u"admin"_s);
        QTRY_VERIFY(desk.isReady());
        const QSignalSpy editAsked(&desk.view, &ui::ActionsView::editAsked);
        for (const QString& machine : {u"kvm"_s, u"srv"_s}) {
            desk.view.open(machine);
            QVERIFY(!desk.view.menu().isEmpty());
            QVERIFY2(!desk.offers(u"edit"_s), qPrintable(machine));
            // Nor is it to be had by naming the row all the same, or by a card's button.
            desk.view.choose(u"edit"_s);
            desk.view.cardAction(machine);
            QVERIFY(!desk.view.menu().isEmpty());
        }
        QCOMPARE(editAsked.count(), 0);
        QVERIFY(pretended.acted.isEmpty());
    }

    void theButtonOnACardOfThisPcsOwnWallLeadsToTheBoxThatChangesItsKvm()
    {
        // Four KVMs of this PC's own wall: one that refused the sign-in, one that shows another
        // certificate than it is known by, one with another trouble, one that is simply there.
        // And two of a server's wall, of which the server says the same two things.
        ToldSource source;
        source.add(u"refused"_s, true, u"auth"_s, u"auth_failed"_s);
        source.add(u"another"_s, true, u"error"_s, u"cert_mismatch"_s);
        source.add(u"amiss"_s, true, u"error"_s, u"bad_response"_s);
        source.add(u"there"_s, true, u"online"_s);
        source.add(u"server-refused"_s, false, u"auth"_s, u"auth_failed"_s);
        source.add(u"server-another"_s, false, u"error"_s, u"cert_mismatch"_s);
        core::WallFeed feed(source, 50);
        ui::OnePlace place(source, feed);
        ui::WallView wall;
        ui::ActionsView view;
        ui::UnderWay underWay;
        ui::MachineActions actions(view, wall, place, underWay);
        const QSignalSpy editAsked(&view, &ui::ActionsView::editAsked);
        feed.start();
        QTRY_COMPARE(feed.status().size(), 6);

        // Nobody is at the wall yet: a button leads nowhere.
        view.cardAction(u"refused"_s);
        QCOMPARE(editAsked.count(), 0);
        core::Account account;
        account.role = u"admin"_s;
        actions.enter(account);

        // "Update login" leads to the box; "Review certificate" to its look at the certificate.
        view.cardAction(u"refused"_s);
        QCOMPARE(editAsked.count(), 1);
        QCOMPARE(editAsked.at(0).at(0).toString(), u"refused"_s);
        QCOMPARE(editAsked.at(0).at(1).toBool(), false);
        view.cardAction(u"another"_s);
        QCOMPARE(editAsked.count(), 2);
        QCOMPARE(editAsked.at(1).at(0).toString(), u"another"_s);
        QCOMPARE(editAsked.at(1).at(1).toBool(), true);
        // A card that has no button leads nowhere, and neither does one of a server's machine,
        // or of a machine that is not on the wall.
        for (const QString& machine :
             {u"amiss"_s, u"there"_s, u"server-refused"_s, u"server-another"_s, u"nobody"_s, QString()}) {
            view.cardAction(machine);
        }
        QCOMPARE(editAsked.count(), 2);
        // A KVM that was mended meanwhile has no button any more, and one pressed late leads nowhere.
        source.status[u"refused"_s].state = u"online"_s;
        source.status[u"refused"_s].errorCode.clear();
        QTRY_COMPARE(feed.status().value(u"refused"_s).state, u"online"_s);
        view.cardAction(u"refused"_s);
        QCOMPARE(editAsked.count(), 2);
        // Nothing of it was sent to a machine, or told.
        QCOMPARE(source.acted, 0);
        QCOMPARE(view.notices()->count(), 0);
        // And the wall is left: nowhere.
        actions.leave();
        view.cardAction(u"another"_s);
        QCOMPARE(editAsked.count(), 2);
    }

    // M6 2h: "Show here" on the card of a machine in use elsewhere (a JetKVM whose one viewer somebody else has),
    // of this PC's wall or a server's, asks first, as the page does, and then takes its picture back (act
    // "resume"); done, it says so, and every window that shows it may ask for it again (resumed).
    void showHereAsksFirstThenResumes()
    {
        ToldSource source;
        source.add(u"jet"_s, true, u"busy"_s, u"in_use"_s);
        source.add(u"server-jet"_s, false, u"busy"_s, u"in_use"_s);
        core::WallFeed feed(source, 50);
        ui::OnePlace place(source, feed);
        ui::WallView wall;
        ui::ActionsView view;
        ui::UnderWay underWay;
        ui::MachineActions actions(view, wall, place, underWay);
        QSignalSpy resumed(&actions, &ui::MachineActions::resumed);
        QSignalSpy editAsked(&view, &ui::ActionsView::editAsked);
        feed.start();
        QTRY_COMPARE(feed.status().size(), 2);
        core::Account account;
        account.role = u"admin"_s;
        actions.enter(account);
        for (const QString& machine : {u"jet"_s, u"server-jet"_s}) {
            view.cardAction(machine);
            const QVariantMap question = view.question();
            QCOMPARE(question.value(u"text"_s).toString(), u"Show %1 here?"_s.arg(machine));
            QCOMPARE(question.value(u"warning"_s).toString(),
                     u"Whoever has its console open is disconnected from it."_s);
            QCOMPARE(question.value(u"yes"_s).toString(), u"Show here"_s);
            QTest::qWait(50);
            QVERIFY(!source.actions.contains(machine + u":resume"_s));
            view.answer(true);
            QTRY_VERIFY(source.actions.contains(machine + u":resume"_s));
            QTRY_VERIFY(!resumed.isEmpty() && resumed.last().at(0).toString() == machine);
        }
        QCOMPARE(resumed.size(), 2);
        QCOMPARE(editAsked.size(), 0);
        // A no to the question sends nothing.
        view.cardAction(u"jet"_s);
        view.answer(false);
        QTest::qWait(100);
        QCOMPARE(source.acted, 2);
    }

    void aFailedResumeOpensNothing()
    {
        ToldSource source;
        source.add(u"jet"_s, true, u"busy"_s, u"in_use"_s);
        source.refuses = base::Error{"not_connected", "The JetKVM is taking too long to connect"};
        core::WallFeed feed(source, 50);
        ui::OnePlace place(source, feed);
        ui::WallView wall;
        ui::ActionsView view;
        ui::UnderWay underWay;
        ui::MachineActions actions(view, wall, place, underWay);
        QSignalSpy resumed(&actions, &ui::MachineActions::resumed);
        feed.start();
        QTRY_COMPARE(feed.status().size(), 1);
        core::Account account;
        account.role = u"admin"_s;
        actions.enter(account);
        view.cardAction(u"jet"_s);
        view.answer(true);
        QTRY_COMPARE(source.acted, 1);
        QTRY_VERIFY(view.notices()->count() > 0);
        QTest::qWait(100);
        QCOMPARE(resumed.size(), 0);
    }

    void aRunThatGoesOnKeepsItsMachinesWhereASecondIsRefused()
    {
        // The box for keys is open for two machines while a run of another thing goes on two
        // others, one after the other. (Over the wall, the screens' box keeps that from happening
        // today; nothing here does.)
        Pretended pretended;
        pretended.more = 2;
        pretended.onAction = [](const QJsonObject& action) {
            // The server lost sight of what went to the rack server: it may have been done.
            if (action[u"machine"_s].toString() == u"srv"_s) {
                Answered lost{504,
                              QByteArray("{\"code\": \"timeout\", \"message\": \"Rack server was being changed.\"}")};
                lost.afterMs = 300;
                return lost;
            }
            return late(300);
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"n1"_s});
        desk.view.openPicked();
        desk.view.choose(u"sendKeys"_s);
        QVERIFY(!desk.view.form().isEmpty());
        desk.pick({u"srv"_s, u"n2"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, 0, false);
        QTRY_COMPARE(pretended.acted.size(), 1);

        // The box is sent while that run goes on: not begun, and not asked about first.
        desk.view.sendKeys(u"F5"_s);
        QVERIFY(desk.view.question().isEmpty());

        // The run that goes on is the run it was: its second machine is sent to once the first was
        // answered, and what got no answer is said as what it is and is not offered again.
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.runRows(), (QStringList{u"Rack server: unknown: No answer. It may have been done."_s,
                                              u"Node 2: ok: Shift sent"_s}));
        QCOMPARE(desk.run(u"again"_s), QString());
        QCOMPARE(pretended.acted.size(), 2);
        QCOMPARE(machinesOf(pretended.acted), (QSet<QString>{u"srv"_s, u"n2"_s}));
        // And why the second was not begun was said, as where one is chosen from the menu.
        QCOMPARE(desk.told, QStringList{u"info: \"Wake screen\" is still being done to several machines. Wait for it "
                                        "to finish."_s});
    }

    void aKeyThatIsOnTheWallAndReachesNothingHasNothingAskedOrSent()
    {
        // Every machine is on the wall, and is told of as it is; no key reaches a place.
        Pretended pretended;
        pretended.more = 1;
        CutDesk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        // No menu: neither its saved actions nor whom it can wake are asked for.
        desk.view.open(u"kvm"_s);
        QVERIFY(desk.view.menu().isEmpty());
        // No console: not entered with a pass, and not opened at its door either.
        desk.view.openConsole(u"srv"_s);
        desk.view.openConsole(u"kvm"_s);
        // Nothing from the numbers on a card: nothing sent, nothing under way, nothing told.
        desk.view.showPort(u"kvm"_s, 1);
        QVERIFY(desk.underWay.isEmpty());
        QVERIFY(desk.told.isEmpty());
        // Several that are picked have their menu, and no place is asked for saved actions; a run
        // over them sends nothing, and each of them is refused as one that is not on the wall.
        desk.pick({u"kvm"_s, u"n1"_s});
        desk.view.openPicked();
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"2 KVMs selected"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(desk.runRows(),
                 (QStringList{u"NAS: bad: Not on the wall any more."_s, u"Node 1: bad: Not on the wall any more."_s}));
        desk.view.closeRun();
        QTest::qWait(200);
        QCOMPARE(pretended.savedAsked, 0);
        QCOMPARE(pretended.wakeAsked, 0);
        QCOMPARE(pretended.consoleAsked, 0);
        QVERIFY(desk.opened.isEmpty());
        QVERIFY(pretended.acted.isEmpty());

        // A menu that was opened while its key reached the server: what is chosen in it once the
        // key reaches nothing goes nowhere, asked about first or not, and nothing holds still for it.
        desk.cut.reachesNothing = false;
        desk.view.open(u"kvm"_s);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        QTRY_COMPARE(pretended.savedAsked, 1);
        desk.cut.reachesNothing = true;
        desk.view.choose(u"keys:ShiftLeft"_s);
        QVERIFY(!desk.view.busy());
        desk.view.choose(u"power:atx_power"_s);
        QVERIFY(!desk.view.question().isEmpty());
        QVERIFY(desk.view.busy());
        desk.view.answer(true);
        QVERIFY(!desk.view.busy());
        QTest::qWait(200);
        QVERIFY(pretended.acted.isEmpty());
        QVERIFY(desk.underWay.isEmpty());
        QVERIFY(desk.told.isEmpty());
    }

    void aBoxForSeveralThatIsSentWhileARunGoesIsRefusedBeforeItIsAskedAbout()
    {
        // The box for keys is open for two machines while a run of another thing goes on two
        // others, slowly. (Over the wall, the screens' box keeps that from happening today.)
        Pretended pretended;
        pretended.more = 2;
        pretended.onAction = [](const QJsonObject&) { return late(300); };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"n1"_s});
        desk.view.openPicked();
        desk.view.choose(u"sendKeys"_s);
        const QVariantMap box{{u"kind"_s, u"keys"_s}, {u"machine"_s, u"2 KVMs"_s}, {u"problem"_s, QString()}};
        QCOMPARE(desk.view.form(), box);
        desk.pick({u"srv"_s, u"n2"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, 0, false);
        QTRY_COMPARE(pretended.acted.size(), 1);

        // The box is sent while that run goes on: refused there and then, as a row of the menu for
        // several is, and not asked about first. Nothing holds still for it, and what was typed
        // stays in its box.
        desk.view.sendKeys(u"F5"_s);
        QVERIFY(desk.view.question().isEmpty());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.told, QStringList{u"info: \"Wake screen\" is still being done to several machines. Wait for it "
                                        "to finish."_s});
        QCOMPARE(desk.view.form(), box);

        // Once that run is over, the same box is asked about, and goes to its own two.
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(pretended.acted.size(), 2);
        desk.view.closeRun();
        desk.view.sendKeys(u"F5"_s);
        QCOMPARE(desk.view.question().value(u"text"_s).toString(), u"Send F5 on 2 KVMs?"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QCOMPARE(pretended.acted.size(), 4);
        QCOMPARE(machinesOf(pretended.acted.mid(2)), (QSet<QString>{u"kvm"_s, u"n1"_s}));
    }

    void aQuestionForSeveralThatStoodWhileARunWasSentOnceMoreBeginsNothing()
    {
        // Two machines refuse what is sent to them the first time; and a box for keys is for two
        // others.
        Pretended pretended;
        pretended.more = 3;
        bool refusing = true;
        pretended.onAction = [&refusing](const QJsonObject& action) -> Answered {
            const QString machine = action[u"machine"_s].toString();
            if (refusing && (machine == u"srv"_s || machine == u"n1"_s)) {
                return {400, QByteArray("{\"code\": \"unsupported\", \"message\": \"Not now.\"}")};
            }
            return late(100);
        };
        Desk desk(pretended);
        QTRY_VERIFY(desk.isReady());
        // A run over the two, one after the other a second apart: both refuse.
        desk.pick({u"srv"_s, u"n1"_s});
        desk.view.openPicked();
        desk.view.choose(u"keys:ShiftLeft"_s);
        desk.view.answerWith(true, 1, false);
        QTRY_VERIFY_WITH_TIMEOUT(desk.runIsOver(), 4000);
        QCOMPARE(desk.run(u"again"_s), u"Retry failed"_s);
        QCOMPARE(pretended.acted.size(), 2);

        // The box for the two others is sent, and its question stands.
        desk.pick({u"n2"_s, u"n3"_s});
        desk.view.openPicked();
        desk.view.choose(u"sendKeys"_s);
        desk.view.sendKeys(u"F5"_s);
        QCOMPARE(desk.view.question().value(u"text"_s).toString(), u"Send F5 on 2 KVMs?"_s);

        // What was refused is sent once more meanwhile: the first now, the second a second later.
        refusing = false;
        desk.view.retryRun();
        QTRY_COMPARE(pretended.acted.size(), 3);
        // Yes to the question while that goes on: it begins nothing, and is said; and the run goes
        // on as it was, each of its machines sent where it was before and answered there.
        desk.view.answerWith(true, -1, false);
        QVERIFY(!desk.told.isEmpty());
        QCOMPARE(desk.told.constLast(), u"info: \"Wake screen\" is still being done to several machines. Wait for it "
                                        "to finish."_s);
        QTRY_VERIFY_WITH_TIMEOUT(desk.runIsOver(), 4000);
        QCOMPARE(desk.runRows(), (QStringList{u"Rack server: ok: Shift sent"_s, u"Node 1: ok: Shift sent"_s}));
        QCOMPARE(pretended.acted.size(), 4);
        QCOMPARE(machinesOf(pretended.acted.mid(2)), (QSet<QString>{u"srv"_s, u"n1"_s}));
    }

    // --- each source's word, held where it is sent ---

    void aSavedActionIsNotSentWhereItsMachineIsOfAnotherSourceByThen()
    {
        // Both servers have a saved action "a1", each its own. The menu of a machine is opened
        // while its key reaches the first, and lists the first's; by the time its row is chosen
        // the key reaches the second.
        Pretended first;
        Pretended second;
        second.saved = anotherSavedAction();
        TwoDesk desk(first, second);
        QTRY_VERIFY(desk.isReady());
        desk.view.open(u"kvm"_s);
        QTRY_VERIFY(desk.offers(u"saved:a1"_s));
        desk.routed.sources.insert(u"kvm"_s, u"two"_s);
        desk.view.choose(u"saved:a1"_s);
        QTest::qWait(300);
        // Not to the second, whose "a1" is another action, nor to the first: nothing.
        QVERIFY(second.acted.isEmpty());
        QVERIFY(first.acted.isEmpty());
        QVERIFY(!desk.view.busy());
        QVERIFY(desk.underWay.isEmpty());
        // A row that is no saved action goes where the key reaches now.
        desk.view.open(u"kvm"_s);
        desk.view.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(second.acted.size(), 1);
        QVERIFY(first.acted.isEmpty());
    }

    void aRunOfASavedActionSendsItToNoMachineOfAnotherSource()
    {
        // Two machines of the first are picked, and their saved action chosen; while the question
        // stands, their keys come to reach the second.
        Pretended first;
        first.more = 1;
        Pretended second;
        second.saved = anotherSavedAction();
        TwoDesk desk(first, second);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"n1"_s});
        desk.view.openPicked();
        QTRY_VERIFY(desk.offers(u"saved:a1"_s));
        desk.view.choose(u"saved:a1"_s);
        QVERIFY(!desk.view.question().isEmpty());
        desk.routed.sources.insert(u"kvm"_s, u"two"_s);
        desk.routed.sources.insert(u"n1"_s, u"two"_s);
        desk.view.answerWith(true, -1, false);
        QTRY_VERIFY(desk.runIsOver());
        QTest::qWait(200);
        QVERIFY(second.acted.isEmpty());
        QVERIFY(first.acted.isEmpty());
        const QStringList rows = desk.runRows();
        QCOMPARE(rows.size(), 2);
        for (const QString& row : rows) {
            QVERIFY2(row.contains(u"another place's"_s), qPrintable(row));
        }
    }

    void whatIsAskedOfOneMachineIsItsOwnSourcesWord()
    {
        // Whoever sits here may act at the first source, and not at the second. A machine's menu
        // is opened while its key reaches the first; by the time a row is chosen it reaches the
        // second.
        Pretended first;
        Pretended second;
        TwoDesk desk(first, second);
        desk.actions.enter(u"two"_s, false);
        QTRY_VERIFY(desk.isReady());
        QVERIFY(desk.view.allowed());
        desk.view.open(u"kvm"_s);
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"NAS"_s);
        desk.routed.sources.insert(u"kvm"_s, u"two"_s);
        // Neither asked about, nor sent.
        desk.view.choose(u"power:atx_power"_s);
        QVERIFY(desk.view.question().isEmpty());
        desk.view.choose(u"keys:ShiftLeft"_s);
        // Nor the numbers on its card.
        desk.view.showPort(u"kvm"_s, 1);
        QTest::qWait(300);
        QVERIFY(second.acted.isEmpty());
        QVERIFY(first.acted.isEmpty());
        QVERIFY(desk.underWay.isEmpty());
    }

    void aSignedInConsolePassIsItsOwnSourcesWord()
    {
        // The rack server's controller makes out passes into its console. Whoever sits here may
        // act at the first source only, and the server's key reaches the second.
        Pretended first;
        Pretended second;
        TwoDesk desk(first, second);
        desk.actions.enter(u"two"_s, false);
        desk.routed.sources.insert(u"srv"_s, u"two"_s);
        QTRY_VERIFY(desk.isReady());
        // To the console's door, as whoever may only look is sent: no pass is asked of either.
        desk.view.openConsole(u"srv"_s);
        QTRY_COMPARE(desk.opened.size(), 1);
        QCOMPARE(desk.opened.constFirst(), QUrl(u"https://192.168.1.60/"_s));
        QTest::qWait(200);
        QCOMPARE(first.consoleAsked, 0);
        QCOMPARE(second.consoleAsked, 0);
        // Of the first: entered signed in, by its pass.
        desk.routed.sources.remove(u"srv"_s);
        desk.view.openConsole(u"srv"_s);
        QTRY_COMPARE(first.consoleAsked, 1);
        QCOMPARE(second.consoleAsked, 0);
    }

    void theButtonOnACardIsItsOwnSourcesWord()
    {
        // A KVM of this PC's own wall that refused the sign-in; whoever sits here may act at
        // another source, and not at this PC's.
        ToldSource source;
        source.add(u"refused"_s, true, u"auth"_s, u"auth_failed"_s);
        core::WallFeed feed(source, 50);
        ui::OnePlace place(source, feed);
        Routed routed(place);
        routed.fallback = u"pc"_s;
        ui::WallView wall;
        ui::ActionsView view;
        ui::UnderWay underWay;
        ui::MachineActions actions(view, wall, routed, underWay);
        const QSignalSpy editAsked(&view, &ui::ActionsView::editAsked);
        feed.start();
        QTRY_COMPARE(feed.status().size(), 1);
        actions.enter(u"elsewhere"_s, true);
        actions.enter(u"pc"_s, false);
        QVERIFY(view.allowed());
        view.cardAction(u"refused"_s);
        QCOMPARE(editAsked.count(), 0);
        // This PC's word: it may.
        actions.enter(u"pc"_s, true);
        view.cardAction(u"refused"_s);
        QCOMPARE(editAsked.count(), 1);
        feed.stop();
    }

    void ofThePickedMachinesNoneOfASourceThatLetsNobodyActIsTaken()
    {
        // The wall says of both cards that they may be acted on; the second source's word is that
        // whoever sits here may not.
        Pretended first;
        Pretended second;
        TwoDesk desk(first, second);
        desk.actions.enter(u"two"_s, false);
        desk.routed.sources.insert(u"kvm"_s, u"two"_s);
        QTRY_VERIFY(desk.isReady());
        desk.pick({u"kvm"_s, u"srv"_s});
        QCOMPARE(desk.wall.picked(), 2);
        // The menu of what is picked is the first's machine's own: the second's is not taken.
        desk.view.openPicked();
        QCOMPARE(desk.view.menu().value(u"title"_s).toString(), u"Rack server"_s);
        QTest::qWait(200);
        QCOMPARE(second.savedAsked, 0);
    }
};

QTEST_GUILESS_MAIN(TestMachineActions)
#include "test_machine_actions.moc"
