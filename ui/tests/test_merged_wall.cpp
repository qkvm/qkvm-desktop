#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_feed.h>
#include <qkvm/core/wall_source.h>
#include <qkvm/engine/scene.h>

#include "actions_view.h"
#include "control.h"
#include "control_view.h"
#include "edit_kvm_view.h"
#include "files_view.h"
#include "keys_by_hand.h"
#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "merged_wall.h"
#include "pc_source.h"
#include "server_session.h"
#include "server_source.h"
#include "session_view.h"
#include "source.h"
#include "stand_in.h"
#include "wall_key.h"
#include "wall_model.h"
#include "wall_screen.h"
#include "wall_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::MadeUpKvm;
using core::testing::StandIn;

namespace {

core::Account anAdmin()
{
    core::Account admin;
    admin.id = u"u1"_s;
    admin.role = u"admin"_s;
    return admin;
}

// A place a wall's machines come from that has the machines a test names, each a server's
// controller that is online until a test says otherwise, and does nothing to any; or, `pictured`,
// each a KVM whose picture is had. It answers each question a turn later, and counts how often it
// was asked for its saved actions, for pictures and for an order.
class NamedPlace final : public QObject, public core::WallSource {
public:
    explicit NamedPlace(const QStringList& ids, bool pictured = false) : m_pictured(pictured)
    {
        // (A pace of its own, so that the wall's is seen to be told it.)
        wall.everySeconds = 7;
        for (const QString& id : ids) {
            core::Machine machine;
            machine.id = id;
            machine.name = id;
            machine.kind = pictured ? u"kvmd"_s : u"redfish"_s;
            machine.hasPicture = pictured;
            wall.machines.append(machine);
            core::Status how;
            how.state = u"online"_s;
            status.insert(id, how);
        }
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
        if (m_pictured) {
            ++snapshots;
            later([done = std::move(done)] { done(core::Snapshot{QByteArray("a picture"), true}); });
            return;
        }
        later([done = std::move(done)] { done(base::Error{"no_stream", "No picture"}); });
    }
    void act(const QString&, const core::Action&, std::function<void(base::Result<core::Done>)> done) override
    {
        later([done = std::move(done)] { done(core::Done{}); });
    }
    bool mayHaveBeenDone(const QString& /*machine*/, const base::Error& /*error*/) const override { return false; }
    void askSavedActions(std::function<void(base::Result<QList<core::SavedAction>>)> done) override
    {
        ++savedAsked;
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
        ++arranged;
        later([done = std::move(done), machines] { done(machines); });
    }
    void openLive(const QString&, core::Live::Need,
                  std::function<void(base::Result<std::unique_ptr<core::Live>>)> done) override
    {
        later([done = std::move(done)] { done(base::Error{"no_live", "No live picture"}); });
    }
    void sawNote(const QString& machine) override { notesSeen.append(machine); }

    core::Wall wall;
    QStringList notesSeen;
    QHash<QString, core::Status> status;
    int savedAsked = 0;
    int snapshots = 0;
    int arranged = 0;

private:
    void later(std::function<void()> what) { QTimer::singleShot(0, this, std::move(what)); }

    bool m_pictured;
};

// That place as a source of the wall's: somebody is at its wall where a test says so (an admin,
// unless it says otherwise), and its turn is over where its wall is left. Whoever sits here may
// make its order, and have its machines live.
class NamedSource final : public ui::Source {
public:
    NamedSource(const QString& id, const QStringList& machines, bool pictured = false)
        : named(machines, pictured), m_id(id), m_feed(named, 60'000)
    {}

    QString id() const override { return m_id; }
    core::WallSource& place() override { return named; }
    core::ServerClient* detailsClient() override { return historyClient; }
    core::ServerClient* historyClient = nullptr;
    core::WallFeed& feed() override { return m_feed; }
    bool isAtTheWall() const override { return m_atTheWall; }
    bool isAway() const override { return false; }
    bool mayBeLive() const override { return true; }
    bool manages() const override { return true; }
    quint64 turn() const override { return m_turn; }
    void looked() override { onLooked(); }

    void enter(const core::Account& account = anAdmin())
    {
        m_atTheWall = true;
        emit entered(account);
    }
    // Says who is at its wall now, whether or not anybody is.
    void tellWho(const core::Account& account) { emit whoChanged(account); }
    void leave()
    {
        m_atTheWall = false;
        ++m_turn;
        emit left();
        emit emptied();
    }

    NamedPlace named;
    std::function<void()> onLooked = [] {};

private:
    QString m_id;
    core::WallFeed m_feed;
    bool m_atTheWall = false;
    quint64 m_turn = 0;
};

// A QKVM as far as these tests go: whoever asks is signed in already (as an admin, unless a test
// says otherwise, or as a screen opened with a display link), and its wall has the KVMs a test
// names, each with a picture, called by its id and the server's name. It does what it is asked,
// after a while where a test says so, and notes it: what was done to which machine, each order it
// was told, and each picture asked for, of which machine and how wide. Where it is down, it hangs
// up on everything.
struct Qkvm {
    explicit Qkvm(QString called) : called(std::move(called)) {}

    QString called;
    QStringList machines{u"a"_s, u"b"_s};
    QString role = u"admin"_s;
    bool display = false;
    bool signedIn = true;
    bool down = false;
    int actsAfterMs = 0;
    int savedAfterMs = 0;
    QString savedName = u"Lock it"_s;
    QList<QJsonObject> acted;
    QList<QStringList> arranged;
    QList<std::pair<QString, int>> pictures;
    // The key of the one display link it gave out, and how often that was handed in; the versions
    // of its API it speaks; whether it has its machines live; whether each of them has a console
    // that a pass is made out for, after how long, and how often one was; and whether it refuses,
    // in its own words, whatever is to be done.
    QByteArray displayKey = "qkvm_" + QByteArray(43, 'k');
    int displaysOpened = 0;
    QByteArray apis = R"(["v1"])";
    bool live = false;
    // How often a live pass was asked for, and whether each is refused "in use" (a JetKVM another viewer has);
    // and how each of its machines is said to be.
    int liveAsked = 0;
    bool liveInUse = false;
    QString state = u"online"_s;
    std::optional<QStringList> capabilities;
    bool consoles = false;
    int consoleAfterMs = 0;
    int consoleAsked = 0;
    bool refusing = false;

    static Answered json(const QJsonObject& body) { return {200, QJsonDocument(body).toJson(QJsonDocument::Compact)}; }
    QString nameOf(const QString& machine) const { return machine + u" at "_s + called; }

    Answered answer(const Asked& asked)
    {
        Answered said = answered(asked);
        said.hangUp = down;
        return said;
    }

    Answered answered(const Asked& asked)
    {
        if (asked.path == "/healthz") {
            return {200, R"({"status": "ok", "version": "0.1.0", "api": )" + apis +
                             R"(, "page": "3f2a9c", "locked": false})"};
        }
        if (asked.path == "/api/v1/display") {
            if (QJsonDocument::fromJson(asked.body).object()[u"key"_s].toString().toLatin1() != displayKey) {
                return {
                    401,
                    R"({"code": "bad_key", "message": "This display link is not valid any more. Ask an admin for a new one."})"};
            }
            signedIn = true;
            display = true;
            ++displaysOpened;
            return json({{u"ok"_s, true},
                         {u"user"_s, QJsonObject{{u"id"_s, u"token:t1"_s},
                                                 {u"name"_s, u"Lobby screen"_s},
                                                 {u"role"_s, role},
                                                 {u"token"_s, true}}}});
        }
        if (asked.path == "/api/v1/session") {
            const QJsonObject user{{u"id"_s, display ? u"token:t1"_s : u"u1"_s},
                                   {u"name"_s, display ? u"Lobby screen"_s : u"anna"_s},
                                   {u"role"_s, role},
                                   {u"token"_s, display}};
            return json({{u"initialized"_s, true},
                         {u"authenticated"_s, signedIn},
                         {u"ask_name"_s, false},
                         {u"user"_s, signedIn ? QJsonValue(user) : QJsonValue()}});
        }
        if (!signedIn) {
            return {401, R"({"code": "unauthorized", "message": "Sign in first"})"};
        }
        if (asked.path == "/api/v1/wall") {
            QJsonArray devices;
            for (const QString& machine : std::as_const(machines)) {
                QJsonObject device{{u"id"_s, machine},
                                   {u"name"_s, nameOf(machine)},
                                   {u"host"_s, u"192.168.1.20"_s},
                                   {u"kind"_s, u"kvmd"_s},
                                   {u"picture"_s, true}};
                if (consoles) {
                    device[u"open_url"_s] = u"http://192.168.1.20/"_s;
                }
                devices.append(device);
            }
            QJsonObject whole{{u"interval"_s, 0.25}, {u"devices"_s, devices}};
            if (live) {
                whole[u"live"_s] = 2;
            }
            return json(whole);
        }
        if (asked.path == "/api/v1/status") {
            QJsonObject all;
            for (const QString& machine : std::as_const(machines)) {
                all[machine] = QJsonObject{{u"state"_s, state}, {u"console"_s, consoles}};
                if (capabilities) {
                    auto status = all[machine].toObject();
                    status[u"can"_s] = QJsonArray::fromStringList(*capabilities);
                    all[machine] = status;
                }
            }
            return json(all);
        }
        if (asked.method == "POST" && asked.path.endsWith("/console")) {
            ++consoleAsked;
            Answered pass = json({{u"url"_s, u"http://192.168.1.20/console?pass=one-use"_s}});
            pass.afterMs = consoleAfterMs;
            return pass;
        }
        if (asked.method == "POST" && asked.path.endsWith("/live") && live) {
            ++liveAsked;
            if (liveInUse) {
                return {409, R"({"code": "in_use", "message": "Someone is using this JetKVM's own console."})"};
            }
            return {200, R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})"};
        }
        if (asked.path == "/api/v1/actions") {
            Answered saved = json({{u"actions"_s, QJsonArray{QJsonObject{{u"id"_s, u"1"_s},
                                                                         {u"name"_s, savedName},
                                                                         {u"kind"_s, u"keys"_s},
                                                                         {u"keys"_s, u"MetaLeft,KeyL"_s},
                                                                         {u"changed"_s, 1}}}}});
            saved.afterMs = savedAfterMs;
            return saved;
        }
        if (asked.path.endsWith("/wol")) {
            return json({{u"targets"_s, QJsonArray()}});
        }
        if (asked.method == "POST" && asked.path.endsWith("/actions")) {
            QJsonObject action = QJsonDocument::fromJson(asked.body).object();
            action[u"machine"_s] = QString::fromLatin1(asked.path.split('/').value(4));
            acted.append(action);
            if (refusing) {
                return {400, R"({"code": "unsupported", "message": "Not now."})"};
            }
            Answered done = json({{u"ok"_s, true}, {u"note"_s, QJsonValue()}});
            done.afterMs = actsAfterMs;
            return done;
        }
        if (asked.method == "PUT" && asked.path == "/api/v1/wall/order") {
            QStringList named;
            for (const auto& id : QJsonDocument::fromJson(asked.body).object()[u"ids"_s].toArray()) {
                named.append(id.toString());
            }
            arranged.append(named);
            QStringList after = named;
            for (const QString& machine : std::as_const(machines)) {
                if (!after.contains(machine)) {
                    after.append(machine);
                }
            }
            machines = after;
            return json({{u"ok"_s, true}, {u"ids"_s, QJsonArray::fromStringList(after)}});
        }
        if (asked.path.startsWith("/api/v1/devices/") && asked.path.contains("/snapshot")) {
            const QUrl asking(QString::fromLatin1(asked.path));
            pictures.append({QString::fromLatin1(asked.path.split('/').value(4)),
                             QUrlQuery(asking).queryItemValue(u"w"_s).toInt()});
            return {200, core::testing::aPicture(), "image/jpeg"};
        }
        if (asked.method == "POST" && asked.path.endsWith("/live")) {
            return {404, R"({"code": "no_live", "message": "This machine has no live picture"})"};
        }
        return {404, R"({"code": "not_found", "message": "Unknown"})"};
    }

    // The machines it was asked for pictures of; and how wide it was asked for one of this
    // machine the last time, 0 for never.
    QSet<QString> pictured() const
    {
        QSet<QString> all;
        for (const auto& picture : pictures) {
            all.insert(picture.first);
        }
        return all;
    }
    int lastWidthOf(const QString& machine) const
    {
        for (auto picture = pictures.crbegin(); picture != pictures.crend(); ++picture) {
            if (picture->first == machine) {
                return picture->second;
            }
        }
        return 0;
    }
};

// A stream that decodes nothing: it says that it has made a picture of everything handed in.
class NotedStream final : public engine::Stream {
public:
    void push(const engine::Packet& /*packet*/) override { ++pushed; }
    void shownAt(engine::SizePx /*onScreen*/, engine::Importance /*importance*/) override {}
    bool takeKeyFrameWish() override { return false; }
    engine::Stats stats() const override
    {
        engine::Stats made;
        made.decoded = static_cast<std::uint64_t>(pushed);
        return made;
    }

    int pushed = 0;
};

// A window's engine that draws nothing: it notes which streams it was given and which were taken
// away.
class NotedScene final : public engine::Scene {
public:
    engine::Stream& add(engine::StreamId id, engine::Codec /*codec*/) override
    {
        streams[id] = std::make_unique<NotedStream>();
        return *streams[id];
    }
    void remove(engine::StreamId id) override
    {
        removed.append(id);
        streams.erase(id);
    }
    void place(std::span<const engine::Placed> /*placed*/) override {}
    bool draw(const engine::Target& /*target*/) override { return false; }
    void drawUnder(engine::SizePx /*size*/) override {}
    void shown(std::int64_t /*atUs*/) override {}

    bool has(quint32 id) const { return streams.contains(id); }

    QList<engine::StreamId> removed;
    std::map<engine::StreamId, std::unique_ptr<NotedStream>> streams;
};

// The times of the drivers of this PC's own KVMs, made short.
core::KvmdDevice::Pace quickKvms()
{
    core::KvmdDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.freshMs = 100;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    return pace;
}

// A machine's key on the wall.
QString key(const ui::Source& source, const QString& machine)
{
    return ui::wallKey(source.id(), machine).value_or(QString());
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

// What the card of this machine shows of one kind; nothing where it has no card.
QVariant onTheCardOf(ui::WallView& wall, const QString& machine, ui::WallModel::Role what)
{
    for (int row = 0; row < wall.machines()->rowCount(); ++row) {
        const QModelIndex card = wall.machines()->index(row, 0);
        if (card.data(ui::WallModel::KeyRole).toString() == machine) {
            return card.data(what);
        }
    }
    return {};
}

QString stateOnTheCardOf(ui::WallView& wall, const QString& machine)
{
    const QVariant state = onTheCardOf(wall, machine, ui::WallModel::StateRole);
    return state.isValid() ? state.toString() : u"(no card)"_s;
}

// What a key reaches, as "source: machine"; "nothing" for nothing.
QString reached(const ui::MergedWall& wall, const QString& key)
{
    const ui::Reached at = wall.reach(key);
    return at ? at.source + u": "_s + at.machine : u"nothing"_s;
}

// What was told in a window's corner and is still there, each as "kind: text", first to last; not
// what says that something is under way.
QStringList toldIn(ui::ActionsView& actions)
{
    QStringList told;
    ui::Notices& notices = *actions.notices();
    for (int row = 0; row < notices.rowCount(); ++row) {
        const QModelIndex notice = notices.index(row, 0);
        if (notice.data(ui::Notices::KindRole).toString() != "working"_L1) {
            told.append(notice.data(ui::Notices::KindRole).toString() + u": "_s +
                        notice.data(ui::Notices::TextRole).toString());
        }
    }
    return told;
}

// What the open menu's row of this id says; nothing where it has none.
QString labelOf(const ui::ActionsView& actions, const QString& row)
{
    const QVariantList sections = actions.menu().value(u"sections"_s).toList();
    for (const QVariant& section : sections) {
        const QVariantList rows = section.toMap().value(u"rows"_s).toList();
        for (const QVariant& one : rows) {
            if (one.toMap().value(u"id"_s).toString() == row) {
                return one.toMap().value(u"label"_s).toString();
            }
        }
    }
    return {};
}

// Whether the open menu has a row of this id.
bool offers(const ui::ActionsView& actions, const QString& row)
{
    return !labelOf(actions, row).isEmpty();
}

// Whether the open menu has a section of this title.
bool hasSection(const ui::ActionsView& actions, const QString& title)
{
    const QVariantList sections = actions.menu().value(u"sections"_s).toList();
    return std::ranges::any_of(
        sections, [&](const QVariant& section) { return section.toMap().value(u"title"_s).toString() == title; });
}

// To which machines something was sent, whatever the order it arrived in.
QSet<QString> machinesOf(const QList<QJsonObject>& acted)
{
    QSet<QString> machines;
    for (const QJsonObject& action : acted) {
        machines.insert(action[u"machine"_s].toString());
    }
    return machines;
}

// A window, as far as the wall goes: its wall, what is done to machines from it, and its engine.
struct Window {
    ui::WallView wall;
    ui::ActionsView actions;
    NotedScene scene;

    // Picks these machines on its wall, and no others.
    void pick(const QStringList& keys)
    {
        wall.setSelecting(true);
        wall.pickNone();
        for (const QString& one : keys) {
            wall.togglePicked(one);
        }
    }
};

// What stands on the desk for the tests of several sources at once: this PC's own wall, two QKVM
// servers ("first" and "second"), each as a source of the windows' wall, in that order; and the
// windows a test opens. Nobody is at any wall until a test comes to it. Whether this PC is a wall
// screen is said as the program says it.
struct Desk {
    Desk()
    {
        first.markScreensIn(screen);
        second.markScreensIn(screen);
        merged.onlyShowWhile(screen);
        merged.addSource(pc);
        merged.addSource(first);
        merged.addSource(second);
    }
    ~Desk()
    {
        // (The windows go before their wall does.)
        for (const auto& window : windows) {
            merged.remove(window->wall);
        }
        pc.feed().stop();
        first.feed().stop();
        second.feed().stop();
    }
    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;
    Desk(Desk&&) = delete;
    Desk& operator=(Desk&&) = delete;

    // A window more; `engine`: with an engine that notes its streams.
    Window& window(bool withEngine = false)
    {
        Window& one = *windows.emplace_back(std::make_unique<Window>());
        merged.add(one.wall, one.actions,
                   withEngine ? ui::MergedWall::Engine([scene = &one.scene]() -> engine::Scene* { return scene; })
                              : ui::MergedWall::Engine());
        return one;
    }

    // A KVM of this PC's own wall, at a made-up KVM of its own: its id there.
    QString addKvm(const QString& name)
    {
        const MadeUpKvm& kvm = *kvms.emplace_back(std::make_unique<MadeUpKvm>());
        core::OwnKvm wanted;
        wanted.address = kvm.stand->address();
        wanted.name = name;
        return own.add(wanted, u"secret"_s).value();
    }

    // This PC's own wall is shown, as the session shows it; and a server's is come to.
    void comeToThisPc()
    {
        pc.enter();
        pc.feed().start();
    }
    void comeTo(ui::ServerSource& server)
    {
        server.connectTo((&server == &first ? firstServer : secondServer).address());
    }
    void comeToAll()
    {
        comeToThisPc();
        comeTo(first);
        comeTo(second);
    }
    void leaveThisPc()
    {
        pc.feed().stop();
        pc.leave();
    }

    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    core::testing::MemorySecrets secrets;
    ui::WallScreen screen{secrets};
    Qkvm firstQkvm{u"first"_s};
    Qkvm secondQkvm{u"second"_s};
    StandIn firstServer{[this](const Asked& asked) { return firstQkvm.answer(asked); }};
    StandIn secondServer{[this](const Asked& asked) { return secondQkvm.answer(asked); }};
    ui::ServerSource first{u"first"_s, settings, secrets, 100};
    ui::ServerSource second{u"second"_s, settings, secrets, 100};
    std::vector<std::unique_ptr<MadeUpKvm>> kvms;
    core::LocalWall own{settings, secrets, quickKvms()};
    ui::PcSource pc{own, 100};
    // The program's own wall, which no window need show.
    ui::WallView wall;
    ui::MergedWall merged{wall};
    std::vector<std::unique_ptr<Window>> windows;
};

// The display link of a stand-in server, as the page shows it to be copied.
QString linkTo(const StandIn& server, const QByteArray& key)
{
    return server.address() + u"/#display&key="_s + QString::fromLatin1(key);
}

// One of the two certificates tools/make_testdata.ps1 makes, with its key.
core::testing::Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

// The program as far as a wall screen goes, as it is started on this PC: this PC's own wall and
// one QKVM server ("first") as sources of the windows' wall, in that order, and one window. What
// the program keeps from one start to the next (the settings and the secrets) outlives it, and so
// does the server it speaks to. Nobody is at any wall until a test comes to it.
struct Program {
    Program(core::Settings& settings, core::testing::MemorySecrets& secrets)
        : screen(secrets), server(u"first"_s, settings, secrets, 100), own(settings, secrets, quickKvms()), pc(own, 100)
    {
        server.markScreensIn(screen);
        merged.onlyShowWhile(screen);
        merged.addSource(pc);
        merged.addSource(server);
        merged.add(window.wall, window.actions, {});
        merged.setOrderOf(pc.id());
    }
    ~Program()
    {
        merged.remove(window.wall);
        pc.feed().stop();
        server.feed().stop();
    }
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    Program(Program&&) = delete;
    Program& operator=(Program&&) = delete;

    // A KVM of this PC's own wall, at a made-up KVM: its key on the wall.
    QString addKvm(const MadeUpKvm& kvm, const QString& name)
    {
        core::OwnKvm wanted;
        wanted.address = kvm.stand->address();
        wanted.name = name;
        return key(pc, own.add(wanted, u"secret"_s).value());
    }
    // The key on the wall of the first KVM this PC's own wall has.
    QString firstKvm() const { return key(pc, own.kvms().constFirst().id); }
    void comeToThisPc()
    {
        pc.enter();
        pc.feed().start();
    }

    ui::WallScreen screen;
    ui::ServerSource server;
    core::LocalWall own;
    ui::PcSource pc;
    ui::WallView wall;
    ui::MergedWall merged{wall};
    Window window;
};

// Whether whoever sits at the window can do nothing to any machine: no way into a menu is drawn,
// no card may be acted on, and neither does this machine's menu open nor is the machine picked.
bool nothingActs(Window& window, const QString& machine)
{
    bool none = !window.actions.allowed();
    for (int row = 0; row < window.wall.machines()->rowCount(); ++row) {
        none = none && !window.wall.machines()->index(row, 0).data(ui::WallModel::ActableRole).toBool();
    }
    window.actions.open(machine);
    none = none && window.actions.menu().isEmpty();
    window.actions.close();
    const bool displaying = window.wall.displaying();
    window.wall.setSelecting(true);
    window.wall.togglePicked(machine);
    none = none && window.wall.picked() == 0;
    window.wall.setSelecting(false);
    window.wall.setDisplaying(displaying);
    return none;
}

} // namespace

// The wall as the windows have it: what it holds that whoever has the sources does not see, and the
// machines of several sources at once.
class TestMergedWall : public QObject {
    Q_OBJECT

private slots:
    void aMachinesNoteIsSeenOnceItsDetailsAreOpened()
    {
        // From the reading of 1b-iii (L5): this PC's wall says "Over HTTPS now" in a KVM's details until whoever
        // sits here has seen them: the machine's place is told so once they are opened for it, and not before.
        NamedSource first(u"one"_s, {u"a"_s, u"b"_s});
        ui::WallView wall;
        ui::ActionsView actions;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(first));
        merged.add(wall, actions, {});
        first.enter();
        first.feed().start();
        QTRY_COMPARE(wall.machines()->rowCount(), 2);
        wall.enlarge(u"one/a"_s);
        QTest::qWait(50);
        QVERIFY(first.named.notesSeen.isEmpty());
        wall.details()->setOpened(true);
        QTRY_COMPARE(first.named.notesSeen, QStringList{u"a"_s});
        wall.details()->setOpened(false);
        wall.enlarge(u"one/b"_s);
        QTest::qWait(50);
        QCOMPARE(first.named.notesSeen, QStringList{u"a"_s});
        wall.details()->setOpened(true);
        QTRY_COMPARE(first.named.notesSeen, (QStringList{u"a"_s, u"b"_s}));
    }

    void detailsFollowTheWindowsSourceAccountAndVisibility()
    {
        NamedSource first(u"one"_s, {u"a"_s});
        NamedSource second(u"two"_s, {u"a"_s});
        core::ServerClient client;
        first.historyClient = &client;
        second.historyClient = &client;
        ui::WallView wall;
        ui::ActionsView actions;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(first));
        QVERIFY(merged.addSource(second));
        merged.add(wall, actions, {});
        first.enter();
        auto viewer = anAdmin();
        viewer.id = u"viewer"_s;
        viewer.role = u"viewer"_s;
        second.enter(viewer);
        first.feed().start();
        second.feed().start();
        QTRY_COMPARE(wall.machines()->rowCount(), 2);
        wall.enlarge(u"one/a"_s);
        QVERIFY(wall.details()->available());
        QVERIFY(wall.details()->administrator());
        first.tellWho(viewer);
        QVERIFY(wall.details()->available());
        QVERIFY(!wall.details()->administrator());
        wall.setUnseen(true);
        QVERIFY(!wall.details()->available());
        wall.setUnseen(false);
        QVERIFY(wall.details()->available());
        wall.enlarge(u"two/a"_s);
        QVERIFY(wall.details()->available());
        first.leave();
        QVERIFY(wall.details()->available());
        merged.setAppRestricted(true);
        QVERIFY(!wall.details()->available());
        merged.setAppRestricted(false);
        wall.setDisplaying(false);
        wall.enlarge(u"two/a"_s);
        QVERIFY(wall.details()->available());
        second.leave();
        QVERIFY(!wall.details()->available());
        merged.remove(wall);
    }

    void theVirtualMediaAreOfferedToAnAdminOfTheMachinesQkvmInPersonAlone()
    {
        // Through a qKVM only (the owner's decision: never of this PC's own wall, whose source has no server), to an
        // admin of it signed in in person; and the box goes with that.
        NamedSource server(u"one"_s, {u"a"_s});
        NamedSource own(u"pc"_s, {u"b"_s});
        core::ServerClient client;
        server.historyClient = &client;
        server.named.status[u"a"_s].capabilities = QStringList{u"power"_s, u"media"_s};
        own.named.status[u"b"_s].capabilities = QStringList{u"power"_s, u"media"_s};
        ui::WallView wall;
        ui::ActionsView actions;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(server));
        QVERIFY(merged.addSource(own));
        merged.add(wall, actions, {});
        server.enter();
        own.enter();
        server.feed().start();
        own.feed().start();
        QTRY_COMPARE(wall.machines()->rowCount(), 2);
        const auto offers = [&](const QString& key) {
            actions.open(key);
            const QVariantList sections = actions.menu().value(u"sections"_s).toList();
            const bool has = std::ranges::any_of(sections, [](const QVariant& section) {
                const QVariantList rows = section.toMap().value(u"rows"_s).toList();
                return std::ranges::any_of(
                    rows, [](const QVariant& row) { return row.toMap().value(u"id"_s).toString() == u"media"_s; });
            });
            actions.close();
            return has;
        };
        QVERIFY(offers(u"one/a"_s));
        QVERIFY(!offers(u"pc/b"_s));
        auto operating = anAdmin();
        operating.role = u"operator"_s;
        server.tellWho(operating);
        QVERIFY(!offers(u"one/a"_s));
        server.tellWho(anAdmin());
        QVERIFY(offers(u"one/a"_s));
        actions.open(u"one/a"_s);
        actions.choose(u"media"_s);
        QVERIFY(actions.media()->opened());
        QCOMPARE(actions.media()->client(), &client);
        // No longer an admin there: the box goes.
        server.tellWho(operating);
        QVERIFY(!actions.media()->opened());
        // An admin by a display link only shows: nothing is offered.
        auto screen = anAdmin();
        screen.isDisplay = true;
        server.tellWho(screen);
        QVERIFY(!offers(u"one/a"_s));
        merged.remove(wall);
    }

    void initTestCase()
    {
        // Whatever speaks over TLS here chooses Windows' own, which is refused once TLS is in
        // use: so one is made before anything else touches TLS (the stand-ins do).
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    // --- one source, as the wall has it ---

    void theCardsAreFilledBeforeTheSourceHearsAndThatBeforeWhoeverActs()
    {
        NamedSource place(u"one"_s, {u"a"_s, u"b"_s});
        ui::WallView wall;
        // Each of the three moments, in the order they come, with what the cards showed then.
        QStringList heard;
        int looks = 0;
        place.onLooked = [&] {
            ++looks;
            heard.append(u"looked: "_s + stateOnTheCardOf(wall, u"one/a"_s));
        };
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(place));
        QObject::connect(&merged, &ui::WallMachines::wallChanged, &merged, [&] {
            heard.append(u"wall: "_s + keysOf(wall).join(u',') + u" every "_s + QString::number(wall.pace()));
        });
        QObject::connect(&merged, &ui::WallMachines::statusChanged, &merged,
                         [&] { heard.append(u"status: "_s + stateOnTheCardOf(wall, u"one/a"_s)); });
        place.enter();
        place.feed().start();
        QTRY_COMPARE(looks, 1);
        // The wall: its cards, by their keys, and its pace, before whoever does things to the
        // machines hears of it. How they are: on the cards before the source hears that they were
        // looked at, and that before whoever does things to them.
        QCOMPARE(heard, (QStringList{u"wall: one/a,one/b every 7000"_s, u"looked: online"_s, u"status: online"_s}));

        // Looked at again, with another answer: the same three, in the same order.
        heard.clear();
        place.named.status[u"a"_s].state = u"no_signal"_s;
        place.feed().lookNow();
        QTRY_COMPARE(looks, 2);
        QCOMPARE(heard, (QStringList{u"looked: no_signal"_s, u"status: no_signal"_s}));
        place.feed().stop();
    }

    void aSourceThatLeavesTakesItsMachinesWithItAsAnyChangeOfTheWall()
    {
        NamedSource one(u"one"_s, {u"a"_s});
        NamedSource two(u"two"_s, {u"b"_s});
        ui::WallView wall;
        ui::ActionsView actions;
        // Each moment of the wall, with what the cards show then and what a key of each reaches.
        QStringList heard;
        const ui::MergedWall* seen = nullptr;
        const auto reaches = [&] {
            return u", one/a: "_s + reached(*seen, u"one/a"_s) + u", two/b: "_s + reached(*seen, u"two/b"_s);
        };
        ui::MergedWall merged(wall);
        seen = &merged;
        QVERIFY(merged.addSource(one));
        QVERIFY(merged.addSource(two));
        QObject::connect(&merged, &ui::WallMachines::wallChanged, &merged, [&] {
            heard.append(u"wall: "_s + keysOf(wall).join(u',') + u" every "_s + QString::number(wall.pace()) +
                         reaches());
        });
        QObject::connect(&merged, &ui::WallMachines::statusChanged, &merged,
                         [&] { heard.append(u"status: "_s + keysOf(wall).join(u',') + reaches()); });
        merged.add(wall, actions, {});
        one.enter();
        one.feed().start();
        two.enter();
        two.feed().start();
        QTRY_COMPARE(keysOf(wall), (QStringList{u"one/a"_s, u"two/b"_s}));
        QTRY_COMPARE(stateOnTheCardOf(wall, u"two/b"_s), u"online"_s);
        QCOMPARE(merged.reach(u"one/a"_s).place, &one.place());
        QCOMPARE(merged.reach(u"two/b"_s).place, &two.place());
        actions.open(u"one/a"_s);
        QCOMPARE(actions.menu().value(u"title"_s).toString(), u"a"_s);
        QTRY_COMPARE(one.named.savedAsked, 1);

        // The first's wall is left, its feed stopped first as its source has it: what the wall had
        // of it goes, by itself, told as any other change of the wall, with no key reaching it by
        // then; its machine's menu goes, and whoever sits here may still act on the other's.
        one.feed().stop();
        heard.clear();
        one.leave();
        QCOMPARE(heard, (QStringList{u"wall: two/b every 7000, one/a: nothing, two/b: two: b"_s,
                                     u"status: two/b, one/a: nothing, two/b: two: b"_s}));
        QVERIFY(actions.menu().isEmpty());
        QVERIFY(actions.allowed());
        // (Its feed still knows its machines: they are nothing to the wall.)
        QCOMPARE(one.feed().machines().size(), 1);
        actions.open(u"one/a"_s);
        QVERIFY(actions.menu().isEmpty());
        actions.open(u"two/b"_s);
        QCOMPARE(actions.menu().value(u"title"_s).toString(), u"b"_s);
        QTRY_COMPARE(two.named.savedAsked, 1);
        QCOMPARE(one.named.savedAsked, 1);

        // The second's is left too: the wall has nothing, and the pace of none.
        two.feed().stop();
        heard.clear();
        two.leave();
        QCOMPARE(heard, (QStringList{u"wall:  every 1000, one/a: nothing, two/b: nothing"_s,
                                     u"status: , one/a: nothing, two/b: nothing"_s}));
        QVERIFY(!actions.allowed());

        // And the first's wall is come to again: its machines are the wall's once its feed has
        // asked for them anew, and not before.
        heard.clear();
        one.enter();
        QVERIFY(keysOf(wall).isEmpty());
        one.feed().start();
        QTRY_COMPARE(keysOf(wall), QStringList{u"one/a"_s});
        QCOMPARE(heard.constFirst(), u"wall: one/a every 7000, one/a: one: a, two/b: nothing"_s);
        one.feed().stop();
    }

    void everySourcesFeedAsksAtThePaceChosenOnTheWall()
    {
        // KVMs whose pictures are had, at a pace of the places' own of seven seconds; and a pace
        // of half a second chosen on the wall.
        NamedSource one(u"one"_s, {u"a"_s}, true);
        NamedSource two(u"two"_s, {u"b"_s}, true);
        NamedSource three(u"three"_s, {u"c"_s}, true);
        ui::WallView wall;
        ui::ActionsView actions;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(one));
        QVERIFY(merged.addSource(two));
        merged.add(wall, actions, {});
        one.enter();
        one.feed().start();
        two.enter();
        two.feed().start();
        QTRY_COMPARE(keysOf(wall), (QStringList{u"one/a"_s, u"two/b"_s}));
        wall.setPace(500);
        QCOMPARE(wall.ownPace(), std::optional<int>(500));
        QTRY_VERIFY_WITH_TIMEOUT(one.named.snapshots >= 3 && two.named.snapshots >= 3, 2500);

        // A source that comes to the wall afterwards asks at it too, and not at its place's.
        QVERIFY(merged.addSource(three));
        three.enter();
        three.feed().start();
        QTRY_COMPARE(keysOf(wall), (QStringList{u"one/a"_s, u"two/b"_s, u"three/c"_s}));
        QTRY_VERIFY_WITH_TIMEOUT(three.named.snapshots >= 3, 2500);
        one.feed().stop();
        two.feed().stop();
        three.feed().stop();
    }

    void aSourceIsTakenOnceAndOnlyWithAnIdOfItsOwn()
    {
        NamedSource one(u"one"_s, {u"a"_s});
        NamedSource namedAlike(u"one"_s, {u"b"_s});
        NamedSource none(QString(), {u"c"_s});
        NamedSource slashed(u"o/ne"_s, {u"d"_s});
        ui::WallView wall;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(one));
        // Not twice, not another by the same id, and none whose id a key could not begin with.
        QVERIFY(!merged.addSource(one));
        QVERIFY(!merged.addSource(namedAlike));
        QVERIFY(!merged.addSource(none));
        QVERIFY(!merged.addSource(slashed));
        // What was not taken is nothing to the wall, whoever is at its wall.
        namedAlike.enter();
        namedAlike.feed().start();
        slashed.enter();
        slashed.feed().start();
        one.enter();
        one.feed().start();
        QTRY_COMPARE(keysOf(wall), QStringList{u"one/a"_s});
        QTest::qWait(100);
        QCOMPARE(keysOf(wall), QStringList{u"one/a"_s});
        QCOMPARE(reached(merged, u"one/b"_s), u"nothing"_s);

        // Taken away: its machines go with it, and the id is free for another source.
        merged.removeSource(one);
        QVERIFY(keysOf(wall).isEmpty());
        QCOMPARE(reached(merged, u"one/a"_s), u"nothing"_s);
        QVERIFY(merged.addSource(namedAlike));
        namedAlike.enter();
        // (Its machines are the wall's once its feed has told them since it was come to: what its
        // feed knew before is nothing to the wall.)
        namedAlike.feed().askAnew();
        QTRY_COMPARE(keysOf(wall), QStringList{u"one/b"_s});
        QCOMPARE(merged.reach(u"one/b"_s).place, &namedAlike.place());
        one.feed().stop();
        namedAlike.feed().stop();
        slashed.feed().stop();
    }

    void anOrderThatIsAnsweredAfterTheWallHasEndedIsNobodys()
    {
        NamedSource place(u"one"_s, {u"a"_s, u"b"_s});
        ui::WallView wall;
        ui::ActionsView actions;
        const QSignalSpy told(actions.notices(), &QAbstractItemModel::rowsInserted);
        auto merged = std::make_unique<ui::MergedWall>(wall);
        QVERIFY(merged->addSource(place));
        merged->add(wall, actions, {});
        merged->setOrderOf(place.id());
        place.enter();
        place.feed().start();
        QTRY_COMPARE(keysOf(wall), (QStringList{u"one/a"_s, u"one/b"_s}));

        // An order is made, sent by the place's own names for its machines, and the wall as the
        // windows have it ends before the place answers.
        wall.arrange();
        wall.placeAt(u"one/b"_s, 0);
        wall.saveOrder();
        QCOMPARE(place.named.arranged, 1);
        QVERIFY(wall.isSavingOrder());
        merged.reset();

        // The answer comes, and reaches nobody: the feed keeps its order, and the window is told
        // neither that the order was saved nor that it was not.
        QTest::qWait(100);
        QCOMPARE(place.feed().machines().constFirst().id, u"a"_s);
        QVERIFY(wall.isSavingOrder());
        QVERIFY(wall.arranging());
        QCOMPARE(told.count(), 0);
        place.feed().stop();
    }

    void withNoWindowTheProgramsOwnWallIsFilledAndThePageIsOpenedNowhere()
    {
        NamedSource place(u"one"_s, {u"a"_s});
        // The program's own wall, which no window shows.
        ui::WallView wall;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(place));
        QList<QUrl> opened;
        merged.openPagesWith([&opened](const QUrl& address) {
            opened.append(address);
            return true;
        });
        place.enter();
        place.feed().start();
        QTRY_COMPARE(stateOnTheCardOf(wall, u"one/a"_s), u"online"_s);
        // With no window there is nobody whose browser it would be for, and nowhere to say so.
        const QUrl page(u"http://127.0.0.1:8788/"_s);
        merged.openThePage(page);
        merged.askForPictures();
        QVERIFY(opened.isEmpty());

        // A window that comes now has the machines at once, and whoever is at the wall in it.
        ui::WallView shown;
        ui::ActionsView actions;
        merged.add(shown, actions, {});
        QCOMPARE(keysOf(shown), QStringList{u"one/a"_s});
        QVERIFY(actions.allowed());
        merged.openThePage(page);
        QCOMPARE(opened, QList<QUrl>{page});
        // And gone again, nothing is opened for it either.
        merged.remove(shown);
        merged.openThePage(page);
        QCOMPARE(opened.size(), 1);
        place.feed().stop();
    }

    // --- several sources at once: two QKVM servers and this PC's own wall ---

    void machinesOfEverySourceAreOnTheWallEachInItsOwnOrder()
    {
        Desk desk;
        desk.firstQkvm.machines = {u"b"_s, u"a"_s};
        desk.secondQkvm.machines = {u"c"_s, u"a"_s};
        const QString build = desk.addKvm(u"Build server"_s);
        const QString nas = desk.addKvm(u"NAS"_s);
        Window& window = desk.window();
        desk.comeToAll();
        // The sources in their order, each in its own; each machine by its source's id and its own.
        const QStringList all{key(desk.pc, build),     key(desk.pc, nas),        key(desk.first, u"b"_s),
                              key(desk.first, u"a"_s), key(desk.second, u"c"_s), key(desk.second, u"a"_s)};
        QTRY_COMPARE(keysOf(window.wall), all);
        QCOMPARE(keysOf(desk.wall), all);
        // Each card says which source it is of; and each key reaches its own.
        QCOMPARE(onTheCardOf(window.wall, key(desk.pc, nas), ui::WallModel::SourceRole).toString(), u"pc"_s);
        QCOMPARE(onTheCardOf(window.wall, key(desk.first, u"a"_s), ui::WallModel::SourceRole).toString(), u"first"_s);
        QCOMPARE(onTheCardOf(window.wall, key(desk.second, u"a"_s), ui::WallModel::SourceRole).toString(), u"second"_s);
        QCOMPARE(reached(desk.merged, key(desk.first, u"a"_s)), u"first: a"_s);
        QCOMPARE(desk.merged.reach(key(desk.first, u"a"_s)).place, &desk.first.place());
        QCOMPARE(reached(desk.merged, key(desk.second, u"a"_s)), u"second: a"_s);
        QCOMPARE(desk.merged.reach(key(desk.second, u"a"_s)).place, &desk.second.place());
        QCOMPARE(reached(desk.merged, key(desk.pc, build)), u"pc: "_s + build);
        // What is not on the wall reaches nothing, however it is made.
        QCOMPARE(reached(desk.merged, key(desk.first, u"c"_s)), u"nothing"_s);
        QCOMPARE(reached(desk.merged, u"a"_s), u"nothing"_s);
        QCOMPARE(reached(desk.merged, u"first"_s), u"nothing"_s);
        QCOMPARE(reached(desk.merged, QString()), u"nothing"_s);

        // A source's own order is another now (its server has a machine more, first): its part of
        // the wall alone is other.
        desk.firstQkvm.machines = {u"d"_s, u"b"_s, u"a"_s};
        QTRY_COMPARE(keysOf(window.wall), (QStringList{key(desk.pc, build), key(desk.pc, nas), key(desk.first, u"d"_s),
                                                       key(desk.first, u"b"_s), key(desk.first, u"a"_s),
                                                       key(desk.second, u"c"_s), key(desk.second, u"a"_s)}));
    }

    void twoServersEachWithAMachineCalledAAreTwoCards()
    {
        Desk desk;
        desk.firstQkvm.machines = {u"a"_s};
        desk.secondQkvm.machines = {u"a"_s};
        // What is done takes its time on both.
        desk.firstQkvm.actsAfterMs = 600;
        desk.secondQkvm.actsAfterMs = 600;
        Window& window = desk.window(true);
        desk.comeTo(desk.first);
        desk.comeTo(desk.second);
        const QString ofFirst = key(desk.first, u"a"_s);
        const QString ofSecond = key(desk.second, u"a"_s);
        QTRY_COMPARE(keysOf(window.wall), (QStringList{ofFirst, ofSecond}));
        // Two cards, each with a stream of its own in the window's engine.
        const quint32 firstStream = onTheCardOf(window.wall, ofFirst, ui::WallModel::StreamRole).toUInt();
        const quint32 secondStream = onTheCardOf(window.wall, ofSecond, ui::WallModel::StreamRole).toUInt();
        QVERIFY(firstStream != 0 && secondStream != 0 && firstStream != secondStream);
        QVERIFY(window.scene.has(firstStream) && window.scene.has(secondStream));
        QCOMPARE(onTheCardOf(window.wall, ofFirst, ui::WallModel::NameRole).toString(), u"a at first"_s);
        QCOMPARE(onTheCardOf(window.wall, ofSecond, ui::WallModel::NameRole).toString(), u"a at second"_s);

        // Something is done to the first's, and is under way there: the second's takes the same at
        // once, and each goes to its own server.
        window.actions.open(ofFirst);
        QCOMPARE(window.actions.menu().value(u"title"_s).toString(), u"a at first"_s);
        window.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(desk.firstQkvm.acted.size(), 1);
        window.actions.open(ofSecond);
        QCOMPARE(window.actions.menu().value(u"title"_s).toString(), u"a at second"_s);
        window.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(desk.secondQkvm.acted.size(), 1);
        QCOMPARE(desk.firstQkvm.acted.size(), 1);
        QCOMPARE(desk.firstQkvm.acted.constFirst()[u"machine"_s].toString(), u"a"_s);
        QCOMPARE(desk.secondQkvm.acted.constFirst()[u"machine"_s].toString(), u"a"_s);
        // Each answer is told of its own machine, and nothing was said to be under way already.
        QTRY_COMPARE(toldIn(window.actions).size(), 2);
        const QStringList told = toldIn(window.actions);
        QCOMPARE(QSet<QString>(told.begin(), told.end()),
                 (QSet<QString>{u"ok: a at first: Shift sent"_s, u"ok: a at second: Shift sent"_s}));
    }

    void aServerThatIsAwayGreysOnlyItsOwnTiles()
    {
        Desk desk;
        const QString build = desk.addKvm(u"Build server"_s);
        Window& window = desk.window();
        desk.comeToAll();
        QTRY_COMPARE(keysOf(window.wall).size(), 5);
        const auto away = [&](const QString& machine) {
            return onTheCardOf(window.wall, machine, ui::WallModel::AwayRole).toBool();
        };
        QVERIFY(!away(key(desk.first, u"a"_s)));

        // The first server stops answering: its cards say that what they show is what was last
        // known, and nobody else's do.
        desk.firstQkvm.down = true;
        QTRY_VERIFY(away(key(desk.first, u"a"_s)));
        QVERIFY(away(key(desk.first, u"b"_s)));
        QVERIFY(desk.first.isAway());
        QVERIFY(!away(key(desk.second, u"a"_s)));
        QVERIFY(!away(key(desk.second, u"b"_s)));
        QVERIFY(!away(key(desk.pc, build)));
        QCOMPARE(keysOf(window.wall).size(), 5);

        // It answers again: its cards are of the present again.
        desk.firstQkvm.down = false;
        QTRY_VERIFY(!away(key(desk.first, u"a"_s)));
        QVERIFY(!away(key(desk.first, u"b"_s)));
    }

    void aServerThatSignsOutTakesOnlyItsMachinesOff()
    {
        Desk desk;
        const QString build = desk.addKvm(u"Build server"_s);
        // What is done takes its time on both servers.
        desk.firstQkvm.actsAfterMs = 800;
        desk.secondQkvm.actsAfterMs = 800;
        Window& picking = desk.window();
        Window& menuOfFirst = desk.window();
        Window& menuOfSecond = desk.window();
        Window& boxOfFirst = desk.window();
        Window& boxOfSecond = desk.window();
        Window& runOverBoth = desk.window();
        Window& runOverOthers = desk.window();
        desk.comeToAll();
        QTRY_COMPARE(keysOf(picking.wall).size(), 5);
        const QString firstA = key(desk.first, u"a"_s);
        const QString firstB = key(desk.first, u"b"_s);
        const QString secondA = key(desk.second, u"a"_s);
        const QString secondB = key(desk.second, u"b"_s);
        const QString kvm = key(desk.pc, build);
        QTRY_COMPARE(stateOnTheCardOf(picking.wall, kvm), u"online"_s);
        // Machines of each are picked; a menu is open of each server's, and a box for keys; and a
        // run goes over a machine of each server, and another over the second's and this PC's.
        picking.pick({firstA, secondA, kvm});
        QCOMPARE(picking.wall.picked(), 3);
        menuOfFirst.actions.open(firstB);
        menuOfSecond.actions.open(secondB);
        boxOfFirst.actions.open(firstA);
        boxOfFirst.actions.choose(u"sendKeys"_s);
        boxOfSecond.actions.open(secondA);
        boxOfSecond.actions.choose(u"sendKeys"_s);
        QCOMPARE(boxOfFirst.actions.form().value(u"machine"_s).toString(), u"a at first"_s);
        QCOMPARE(boxOfSecond.actions.form().value(u"machine"_s).toString(), u"a at second"_s);
        runOverBoth.pick({firstA, secondA});
        runOverBoth.actions.openPicked();
        runOverBoth.actions.choose(u"keys:ShiftLeft"_s);
        runOverBoth.actions.answerWith(true, -1, false);
        runOverOthers.pick({secondB, kvm});
        runOverOthers.actions.openPicked();
        runOverOthers.actions.choose(u"keys:ShiftLeft"_s);
        runOverOthers.actions.answerWith(true, -1, false);
        QTRY_COMPARE(desk.secondQkvm.acted.size(), 2);
        QCOMPARE(desk.firstQkvm.acted.size(), 1);

        // The first server ends the session: its machines go, and with them what was its alone.
        desk.firstQkvm.signedIn = false;
        QTRY_COMPARE(keysOf(picking.wall), (QStringList{kvm, secondA, secondB}));
        QCOMPARE(keysOf(desk.wall), (QStringList{kvm, secondA, secondB}));
        // Its picks, and no others: machines are still being picked.
        QCOMPARE(picking.wall.pickedMachines(), (QStringList{kvm, secondA}));
        QVERIFY(picking.wall.selecting());
        // Its menu and its box; the second's stay as they are.
        QVERIFY(menuOfFirst.actions.menu().isEmpty());
        QCOMPARE(menuOfSecond.actions.menu().value(u"title"_s).toString(), u"b at second"_s);
        QVERIFY(boxOfFirst.actions.form().isEmpty());
        QCOMPARE(boxOfSecond.actions.form().value(u"machine"_s).toString(), u"a at second"_s);
        // The run that touches it is forgotten; the other goes on, and ends as it would have.
        QVERIFY(runOverBoth.actions.run().isEmpty());
        QVERIFY(!runOverOthers.actions.run().isEmpty());
        QTRY_VERIFY(!runOverOthers.actions.run().value(u"running"_s).toBool());
        QCOMPARE(runOverOthers.actions.run().value(u"summary"_s).toString(), u"Done on all 2 KVMs."_s);
        // Whoever sits here may still act on the others' machines; and what the forgotten run had
        // out to the second server frees its machine once it is answered there.
        QVERIFY(menuOfFirst.actions.allowed());
        QTest::qWait(900);
        boxOfSecond.actions.sendKeys(u"F5"_s);
        QCOMPARE(boxOfSecond.actions.question().value(u"text"_s).toString(), u"Send F5 on a at second?"_s);
        boxOfSecond.actions.answer(true);
        QTRY_COMPARE(desk.secondQkvm.acted.size(), 3);
        QCOMPARE(desk.secondQkvm.acted.constLast()[u"keys"_s].toString(), u"F5"_s);
        QVERIFY(!toldIn(boxOfSecond.actions).join(u'\n').contains(u"still under way"_s));
    }

    void leavingOneSourceDropsNoAnswerOfAnother()
    {
        Desk desk;
        const QString build = desk.addKvm(u"Build server"_s);
        desk.addKvm(u"NAS"_s);
        // The second server is slow to answer what is done, and with its saved actions.
        desk.secondQkvm.actsAfterMs = 600;
        desk.secondQkvm.savedAfterMs = 600;
        Window& one = desk.window();
        Window& two = desk.window();
        desk.comeToAll();
        QTRY_COMPARE(keysOf(one.wall).size(), 6);
        const QString secondA = key(desk.second, u"a"_s);
        const QString secondB = key(desk.second, u"b"_s);
        // An action to the second's machine, and the menu of another of its machines, whose saved
        // actions are on their way.
        one.actions.open(secondA);
        one.actions.choose(u"keys:ShiftLeft"_s);
        two.actions.open(secondB);
        QVERIFY(!offers(two.actions, u"saved:1"_s));

        // The first server's wall is left, and this PC's own.
        desk.first.leave();
        desk.leaveThisPc();
        QCOMPARE(keysOf(one.wall), (QStringList{secondA, secondB}));

        // The second's answers come, and are taken: the action is told, its machine is free, and
        // the menu has the saved actions.
        QTRY_COMPARE(toldIn(one.actions), QStringList{u"ok: a at second: Shift sent"_s});
        QTRY_VERIFY(offers(two.actions, u"saved:1"_s));
        QCOMPARE(two.actions.menu().value(u"title"_s).toString(), u"b at second"_s);
        one.actions.open(secondA);
        one.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(desk.secondQkvm.acted.size(), 2);

        // And this PC's wall, alone at the wall: an order made there is taken whatever a server is
        // asked meanwhile, each source with a turn of its own.
        desk.second.leave();
        desk.comeToThisPc();
        QTRY_COMPARE(keysOf(one.wall).size(), 2);
        const QString nas = keysOf(one.wall).constLast();
        desk.merged.setOrderOf(desk.pc.id());
        one.wall.arrange();
        one.wall.placeAt(nas, 0);
        one.wall.saveOrder();
        QVERIFY(one.wall.isSavingOrder());
        desk.comeTo(desk.first);
        QTRY_VERIFY(!one.wall.arranging());
        QCOMPARE(toldIn(one.actions).constLast(), u"ok: The order is saved, for everybody who looks at this wall"_s);
        QCOMPARE(keysOf(one.wall).constFirst(), nas);
        QVERIFY(desk.own.kvms().constFirst().id != build);
    }

    void picturesAreAskedOfEachSourceForItsOwnMachinesAtTheWidestWidth()
    {
        Desk desk;
        desk.firstQkvm.machines = {u"a"_s, u"b"_s};
        desk.secondQkvm.machines = {u"a"_s, u"c"_s};
        // Cards 300 pixels wide in one window, 500 in the other: asked for at twice the wider.
        Window& narrow = desk.window();
        narrow.wall.setCardPixels(300);
        Window& wide = desk.window();
        wide.wall.setCardPixels(500);
        desk.comeTo(desk.first);
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(wide.wall).size(), 4);
        QTRY_VERIFY(desk.firstQkvm.lastWidthOf(u"a"_s) == 1000 && desk.firstQkvm.lastWidthOf(u"b"_s) == 1000);
        QTRY_VERIFY(desk.secondQkvm.lastWidthOf(u"a"_s) == 1000 && desk.secondQkvm.lastWidthOf(u"c"_s) == 1000);
        // Each server is asked for its own machines, by its own names for them, and for no other's.
        QCOMPARE(desk.firstQkvm.pictured(), (QSet<QString>{u"a"_s, u"b"_s}));
        QCOMPARE(desk.secondQkvm.pictured(), (QSet<QString>{u"a"_s, u"c"_s}));

        // One window shows the second's "a" large, 800 pixels wide: it is asked for alone there, of
        // its own server, as large as it is drawn; the first's "a", which is another machine, and
        // the others, at the width of the window that still shows them.
        wide.wall.setEnlargedPixels(800);
        wide.wall.enlarge(key(desk.second, u"a"_s));
        QTRY_COMPARE(desk.secondQkvm.lastWidthOf(u"a"_s), 1600);
        QTRY_COMPARE(desk.firstQkvm.lastWidthOf(u"a"_s), 600);
        QTRY_COMPARE(desk.secondQkvm.lastWidthOf(u"c"_s), 600);
        QTest::qWait(600);
        QCOMPARE(desk.secondQkvm.lastWidthOf(u"a"_s), 1600);
        QCOMPARE(desk.firstQkvm.lastWidthOf(u"a"_s), 600);
        QCOMPARE(desk.firstQkvm.pictured(), (QSet<QString>{u"a"_s, u"b"_s}));
    }

    // b10: a machine shown large at a scale of its own pixels is asked for whole, its stills as wide as
    // the server gives any (16384, its own widest at the most), and fitted again as wide as it is drawn.
    void aMachineShownLargeAtAScaleIsAskedForWhole()
    {
        Desk desk;
        Window& window = desk.window();
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(window.wall).size(), 2);
        window.wall.setEnlargedPixels(800);
        window.wall.enlarge(key(desk.first, u"a"_s));
        QTRY_COMPARE(desk.firstQkvm.lastWidthOf(u"a"_s), 1600);
        window.wall.chooseScale(u"100"_s);
        QTRY_COMPARE(desk.firstQkvm.lastWidthOf(u"a"_s), 16384);
        window.wall.chooseScale(u"fit"_s);
        QTRY_COMPARE(desk.firstQkvm.lastWidthOf(u"a"_s), 1600);
    }

    // b10: and its live line too, at the widest the server's line takes (16384: one wider is none to it).
    void aMachineShownLargeAtAScaleIsHadLiveWhole()
    {
        Desk desk;
        desk.firstQkvm.live = true;
        desk.firstQkvm.capabilities = QStringList{u"live"_s};
        Window& window = desk.window();
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(window.wall).size(), 2);
        window.wall.setEnlargedPixels(1000);
        window.wall.enlarge(key(desk.first, u"a"_s));
        QTRY_VERIFY(desk.firstServer.live.isConnected());
        QTRY_VERIFY(!desk.firstServer.live.said.isEmpty());
        QCOMPARE(desk.firstServer.live.said.at(0)[u"max_w"_s].toInt(), 1120);
        window.wall.chooseScale(u"200"_s);
        QTRY_VERIFY(!desk.firstServer.live.saidOf(u"shape"_s).isEmpty() &&
                    desk.firstServer.live.saidOf(u"shape"_s).last()[u"max_w"_s].toInt() == 16384);
        // (Drawn wider than any machine is, fitted, it is still no wider than that.)
        window.wall.chooseScale(u"fit"_s);
        QTRY_VERIFY(desk.firstServer.live.saidOf(u"shape"_s).last()[u"max_w"_s].toInt() == 1120);
        const qsizetype shapes = desk.firstServer.live.saidOf(u"shape"_s).size();
        window.wall.setEnlargedPixels(20000);
        QTRY_VERIFY(desk.firstServer.live.saidOf(u"shape"_s).size() > shapes);
        QTest::qWait(200);
        for (const QJsonObject& shape : desk.firstServer.live.saidOf(u"shape"_s)) {
            QVERIFY(shape[u"max_w"_s].toInt() <= 16384);
        }
        QCOMPARE(desk.firstServer.live.saidOf(u"shape"_s).last()[u"max_w"_s].toInt(), 16384);
    }

    void whoMayActIsEachSourcesWord()
    {
        Desk desk;
        const QString build = desk.addKvm(u"Build server"_s);
        // An operator at the first server, a viewer at the second.
        desk.firstQkvm.role = u"operator"_s;
        desk.secondQkvm.role = u"viewer"_s;
        Window& window = desk.window();
        desk.comeToAll();
        QTRY_COMPARE(keysOf(window.wall).size(), 5);
        const QString firstA = key(desk.first, u"a"_s);
        const QString firstB = key(desk.first, u"b"_s);
        const QString secondA = key(desk.second, u"a"_s);
        const QString kvm = key(desk.pc, build);
        const auto actable = [&](const QString& machine) {
            return onTheCardOf(window.wall, machine, ui::WallModel::ActableRole).toBool();
        };
        // Each card says whether whoever sits here may act on it.
        QVERIFY(actable(firstA));
        QVERIFY(actable(kvm));
        QVERIFY(!actable(secondA));
        QVERIFY(window.actions.allowed());

        // The viewer's machine has no menu; the operator's has, and what is chosen there goes to
        // its server.
        window.actions.open(secondA);
        QVERIFY(window.actions.menu().isEmpty());
        window.actions.open(firstA);
        QCOMPARE(window.actions.menu().value(u"title"_s).toString(), u"a at first"_s);
        window.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(desk.firstQkvm.acted.size(), 1);
        QVERIFY(desk.secondQkvm.acted.isEmpty());
        // Picking takes the operator's machines and this PC's, and not the viewer's.
        window.wall.setSelecting(true);
        window.wall.togglePicked(secondA);
        QCOMPARE(window.wall.picked(), 0);
        window.wall.pickShown();
        QCOMPARE(window.wall.pickedMachines(), (QStringList{kvm, firstA, firstB}));

        // The first server makes its operator a viewer (said at the next asking of who is signed
        // in): its machines' menu goes, and their picks; this PC's stay picked.
        window.actions.open(firstB);
        QVERIFY(!window.actions.menu().isEmpty());
        desk.firstQkvm.role = u"viewer"_s;
        QTRY_VERIFY_WITH_TIMEOUT(!actable(firstA), 5000);
        QVERIFY(window.actions.menu().isEmpty());
        QCOMPARE(window.wall.pickedMachines(), QStringList{kvm});
        QVERIFY(window.wall.selecting());
        // And the second makes its viewer an operator: its machines may be acted on.
        desk.secondQkvm.role = u"operator"_s;
        QTRY_VERIFY_WITH_TIMEOUT(actable(secondA), 5000);
        window.actions.open(secondA);
        QCOMPARE(window.actions.menu().value(u"title"_s).toString(), u"a at second"_s);
    }

    void aSavedActionGoesOnlyToMachinesOfItsServer()
    {
        Desk desk;
        // Both servers have a saved action of the id "1": each its own.
        desk.firstQkvm.savedName = u"Lock it"_s;
        desk.secondQkvm.savedName = u"Restart the screens"_s;
        desk.secondQkvm.machines = {u"a"_s};
        Window& window = desk.window();
        desk.comeTo(desk.first);
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(window.wall).size(), 3);
        const QString firstA = key(desk.first, u"a"_s);
        const QString firstB = key(desk.first, u"b"_s);
        const QString secondA = key(desk.second, u"a"_s);

        // Machines of both servers: whatever their menu offers, the second's machine is sent no
        // action of the first's; and the menu offers none of either, so that choosing one does
        // nothing at all.
        window.pick({firstA, secondA});
        window.actions.openPicked();
        QCOMPARE(window.actions.menu().value(u"title"_s).toString(), u"2 KVMs selected"_s);
        QTest::qWait(300);
        const bool offered = offers(window.actions, u"saved:1"_s);
        window.actions.choose(u"saved:1"_s);
        const bool asked = !window.actions.question().isEmpty();
        if (asked) {
            window.actions.answerWith(true, -1, false);
        }
        QTest::qWait(300);
        QVERIFY(desk.secondQkvm.acted.isEmpty());
        QVERIFY(!offered);
        QVERIFY(!asked);
        QVERIFY(desk.firstQkvm.acted.isEmpty());
        // (Nor has their menu a place for them, until the screens word what it would say there.)
        window.actions.openPicked();
        QVERIFY(!window.actions.menu().isEmpty());
        QVERIFY(!hasSection(window.actions, u"Your actions"_s));

        // Machines of one server: its saved actions, and what is chosen goes to them and nowhere
        // else.
        window.pick({firstA, firstB});
        window.actions.openPicked();
        QTRY_VERIFY(offers(window.actions, u"saved:1"_s));
        QVERIFY(hasSection(window.actions, u"Your actions"_s));
        QCOMPARE(labelOf(window.actions, u"saved:1"_s), u"Lock it"_s);
        window.actions.choose(u"saved:1"_s);
        QCOMPARE(window.actions.question().value(u"title"_s).toString(), u"Lock it"_s);
        window.actions.answerWith(true, -1, false);
        QTRY_COMPARE(desk.firstQkvm.acted.size(), 2);
        QCOMPARE(machinesOf(desk.firstQkvm.acted), (QSet<QString>{u"a"_s, u"b"_s}));
        QCOMPARE(desk.firstQkvm.acted.constFirst()[u"custom_id"_s].toString(), u"1"_s);
        QTest::qWait(300);
        QVERIFY(desk.secondQkvm.acted.isEmpty());
        // The one machine of the second: its own saved action, its own name.
        window.actions.closeRun();
        window.actions.open(secondA);
        QTRY_COMPARE(labelOf(window.actions, u"saved:1"_s), u"Restart the screens"_s);
    }

    void theOrderIsMadeForOneSourceAtATime()
    {
        Desk desk;
        const QString build = desk.addKvm(u"Build server"_s);
        const QString nas = desk.addKvm(u"NAS"_s);
        desk.secondQkvm.machines = {u"c"_s, u"d"_s};
        Window& window = desk.window();
        desk.comeToAll();
        QTRY_COMPARE(keysOf(window.wall).size(), 6);
        const QString firstA = key(desk.first, u"a"_s);
        const QString firstB = key(desk.first, u"b"_s);
        const QString secondC = key(desk.second, u"c"_s);
        const QString secondD = key(desk.second, u"d"_s);
        // Whoever sits here is an admin of both servers, and of this PC's own wall.

        // An order of the first's: its machines alone are in it, and are all that is shown while
        // it is made. What is said meanwhile of which source the next order is to be of is for
        // the next one.
        desk.merged.setOrderOf(desk.first.id());
        window.wall.arrange();
        QVERIFY(window.wall.arranging());
        QCOMPARE(window.wall.arrangingOf(), desk.first.id());
        QCOMPARE(window.wall.arrangement(), (QStringList{firstA, firstB}));
        QCOMPARE(window.wall.shown()->keys(), (QStringList{firstA, firstB}));
        desk.merged.setOrderOf(desk.second.id());
        QCOMPARE(window.wall.arrangingOf(), desk.first.id());
        // It is sent to the first, by its own names for its machines, and to nobody else; and the
        // wall has it, in the first's part of it alone.
        window.wall.placeAt(firstB, 0);
        window.wall.saveOrder();
        QTRY_VERIFY(!window.wall.arranging());
        QCOMPARE(desk.firstQkvm.arranged, (QList<QStringList>{{u"b"_s, u"a"_s}}));
        QVERIFY(desk.secondQkvm.arranged.isEmpty());
        QCOMPARE(desk.own.kvms().constFirst().id, build);
        QCOMPARE(keysOf(window.wall),
                 (QStringList{key(desk.pc, build), key(desk.pc, nas), firstB, firstA, secondC, secondD}));

        // And one of the second's, the same way.
        window.wall.arrange();
        QCOMPARE(window.wall.arrangement(), (QStringList{secondC, secondD}));
        window.wall.placeAt(secondD, 0);
        window.wall.saveOrder();
        QTRY_VERIFY(!window.wall.arranging());
        QCOMPARE(desk.secondQkvm.arranged, (QList<QStringList>{{u"d"_s, u"c"_s}}));
        QCOMPARE(desk.firstQkvm.arranged.size(), 1);

        // A list that names machines of two sources, or those of another source than the order
        // being made is for, is refused, and asked of neither. (A window's wall asks no such list
        // today: it is asked here as one would be.)
        desk.merged.setOrderOf(desk.first.id());
        window.wall.arrange();
        emit window.wall.orderAsked({firstA, secondC});
        emit window.wall.orderAsked({secondC, secondD});
        QTest::qWait(200);
        QCOMPARE(desk.firstQkvm.arranged.size(), 1);
        QCOMPARE(desk.secondQkvm.arranged.size(), 1);
        window.wall.stopArranging();

        // At a source where whoever sits here is no admin now: its order is refused, and asked
        // of nobody.
        desk.second.leave();
        desk.secondQkvm.role = u"operator"_s;
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(window.wall).size(), 6);
        desk.merged.setOrderOf(desk.second.id());
        window.wall.arrange();
        window.wall.placeAt(secondC, 0);
        window.wall.saveOrder();
        QVERIFY(!window.wall.isSavingOrder());
        QTest::qWait(200);
        QCOMPARE(desk.secondQkvm.arranged.size(), 1);
        window.wall.stopArranging();

        // At this PC's own wall: an order whose answer comes after its wall was left is nobody's,
        // and the window is told nothing of it.
        desk.merged.setOrderOf(desk.pc.id());
        const qsizetype told = toldIn(window.actions).size();
        window.wall.arrange();
        QCOMPARE(window.wall.arrangement(), (QStringList{key(desk.pc, build), key(desk.pc, nas)}));
        window.wall.placeAt(key(desk.pc, nas), 0);
        window.wall.saveOrder();
        desk.leaveThisPc();
        QTest::qWait(200);
        QCOMPARE(toldIn(window.actions).size(), told);
        QVERIFY(!window.wall.arranging());
    }

    void aSourcesWordAndItsLeavingStopOnlyAnOrderOfItsOwn()
    {
        Desk desk;
        desk.secondQkvm.machines = {u"c"_s, u"d"_s};
        Window& one = desk.window();
        Window& two = desk.window();
        desk.comeTo(desk.first);
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(one.wall).size(), 4);
        // An order of the first's is being made in one window, and one of the second's in the
        // other.
        desk.merged.setOrderOf(desk.first.id());
        one.wall.arrange();
        desk.merged.setOrderOf(desk.second.id());
        two.wall.arrange();
        QCOMPARE(one.wall.arrangingOf(), desk.first.id());
        QCOMPARE(two.wall.arrangingOf(), desk.second.id());

        // The first makes whoever sits here an operator (said at the next asking of who is signed
        // in): the first's order is no longer theirs to make. The second's goes on, and is saved.
        desk.firstQkvm.role = u"operator"_s;
        QTRY_VERIFY_WITH_TIMEOUT(!one.wall.arranging(), 5000);
        QVERIFY(two.wall.arranging());
        two.wall.placeAt(key(desk.second, u"d"_s), 0);
        two.wall.saveOrder();
        QTRY_VERIFY(!two.wall.arranging());
        QCOMPARE(desk.secondQkvm.arranged, (QList<QStringList>{{u"d"_s, u"c"_s}}));
        QVERIFY(desk.firstQkvm.arranged.isEmpty());

        // An order of the second's again: the first's wall is left, and it goes on; the second's
        // is left, and it is over.
        one.wall.arrange();
        QCOMPARE(one.wall.arrangingOf(), desk.second.id());
        desk.first.leave();
        QTRY_COMPARE(keysOf(one.wall).size(), 2);
        QVERIFY(one.wall.arranging());
        desk.second.leave();
        QVERIFY(!one.wall.arranging());
    }

    void aDisplayLinkLetsNobodyActOnAnyMachine()
    {
        Desk desk;
        const QString build = desk.addKvm(u"Build server"_s);
        desk.secondQkvm.display = true;
        desk.secondQkvm.role = u"viewer"_s;
        Window& window = desk.window();
        desk.comeToThisPc();
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(window.wall).size(), 3);
        const QString kvm = key(desk.pc, build);
        const auto actable = [&](const QString& machine) {
            return onTheCardOf(window.wall, machine, ui::WallModel::ActableRole).toBool();
        };
        QTRY_COMPARE(stateOnTheCardOf(window.wall, kvm), u"online"_s);
        QVERIFY(window.actions.allowed());
        QVERIFY(actable(kvm));
        window.actions.open(kvm);
        QCOMPARE(window.actions.menu().value(u"title"_s).toString(), u"Build server"_s);
        QVERIFY(!desk.merged.onlyShows());

        // The second server's wall is come to by a display link: the whole program only shows.
        // Nobody may act on any machine, this PC's KVM included, which is still on the wall; what
        // was open is closed, and every window is only shown.
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(window.wall).size(), 5);
        QVERIFY(desk.merged.onlyShows());
        QVERIFY(!window.actions.allowed());
        QVERIFY(window.actions.menu().isEmpty());
        QVERIFY(window.wall.displaying());
        QVERIFY(desk.wall.displaying());
        QVERIFY(keysOf(window.wall).contains(kvm));
        for (const QString& machine : keysOf(window.wall)) {
            QVERIFY2(!actable(machine), qPrintable(machine));
        }
        window.actions.open(kvm);
        QVERIFY(window.actions.menu().isEmpty());
        window.actions.open(key(desk.first, u"a"_s));
        QVERIFY(window.actions.menu().isEmpty());
        window.wall.setDisplaying(false);
        window.wall.setSelecting(true);
        window.wall.togglePicked(kvm);
        QCOMPARE(window.wall.picked(), 0);

        // Its wall is left (back to saying which server): this PC is a wall screen all the same,
        // and nobody may act. Come to again, and signed out by a person here: whoever sits here
        // may act again.
        desk.second.leave();
        QTRY_COMPARE(keysOf(window.wall).size(), 3);
        QVERIFY(desk.merged.onlyShows());
        QVERIFY(!window.actions.allowed());
        QVERIFY(!actable(kvm));
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(window.wall).size(), 5);
        QVERIFY(desk.merged.onlyShows());
        desk.second.signOut();
        QVERIFY(!desk.merged.onlyShows());
        QVERIFY(window.actions.allowed());
        QVERIFY(actable(kvm));
        window.actions.open(kvm);
        QCOMPARE(window.actions.menu().value(u"title"_s).toString(), u"Build server"_s);
    }

    void aSourceThatLeavesTakesItsStreamsOutOfEveryEngine()
    {
        Desk desk;
        // This PC's KVM serves its stream of pictures: the first of them, and the line is kept.
        const QString build = desk.addKvm(u"Build server"_s);
        MadeUpKvm& made = *desk.kvms.back();
        const QByteArray picture = core::testing::jpeg(1920, 1080);
        Answered stream(200,
                        "--boundarydonotcross\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                            QByteArray::number(picture.size()) + "\r\n\r\n" + picture + "\r\n--boundarydonotcross\r\n",
                        "multipart/x-mixed-replace;boundary=boundarydonotcross");
        stream.streams = true;
        made.instead.insert("/streamer/stream", stream);
        desk.secondQkvm.machines = {u"c"_s};
        desk.merged.retryLiveAt({10, 150, 600, 5, 20});
        Window& one = desk.window(true);
        Window& two = desk.window(true);
        desk.comeToAll();
        QTRY_COMPARE(keysOf(one.wall).size(), 4);
        const QString kvm = key(desk.pc, build);
        const auto streamOf = [&](const QString& machine) {
            return onTheCardOf(one.wall, machine, ui::WallModel::StreamRole).toUInt();
        };
        const quint32 firstA = streamOf(key(desk.first, u"a"_s));
        const quint32 firstB = streamOf(key(desk.first, u"b"_s));
        const quint32 secondC = streamOf(key(desk.second, u"c"_s));
        const quint32 ofKvm = streamOf(kvm);
        // Each engine has a stream for each machine's picture.
        for (const Window* window : {&one, &two}) {
            QCOMPARE(window->scene.streams.size(), std::size_t{4});
            QVERIFY(window->scene.has(firstA) && window->scene.has(firstB) && window->scene.has(secondC) &&
                    window->scene.has(ofKvm));
        }
        // One window shows this PC's KVM large, and has it live: a stream of its own, in its engine.
        one.wall.setEnlargedPixels(1000);
        one.wall.enlarge(kvm);
        QTRY_VERIFY_WITH_TIMEOUT(one.wall.liveStream() != 0, 4000);
        const quint32 live = one.wall.liveStream();
        QTRY_COMPARE(one.wall.liveDiagnostics()[u"codec"_s].toString(), u"jpeg"_s);
        QVERIFY(one.wall.liveDiagnostics()[u"hasPicture"_s].toBool());
        QVERIFY(two.wall.liveDiagnostics().isEmpty());
        QVERIFY(one.scene.has(live));

        // The first server ends its session: its machines' streams go out of both engines, and
        // nothing else does; this PC's KVM is still had live.
        desk.firstQkvm.signedIn = false;
        QTRY_COMPARE(keysOf(one.wall).size(), 2);
        for (Window* window : {&one, &two}) {
            QVERIFY(!window->scene.has(firstA) && !window->scene.has(firstB));
            QVERIFY(window->scene.removed.contains(firstA) && window->scene.removed.contains(firstB));
            QVERIFY(window->scene.has(secondC) && window->scene.has(ofKvm));
        }
        QCOMPARE(one.wall.liveStream(), live);
        QVERIFY(one.scene.has(live));
        QVERIFY(made.stand->isStreaming());

        // The second is taken off the wall altogether: the same, its streams with it.
        desk.merged.removeSource(desk.second);
        QCOMPARE(keysOf(one.wall), QStringList{kvm});
        for (Window* window : {&one, &two}) {
            QVERIFY(!window->scene.has(secondC));
            QVERIFY(window->scene.has(ofKvm));
        }
        QCOMPARE(reached(desk.merged, key(desk.second, u"c"_s)), u"nothing"_s);
        QCOMPARE(one.wall.liveStream(), live);

        // And this PC's wall is left: its KVM's stream, and the line it was had live by, go too.
        desk.leaveThisPc();
        QVERIFY(keysOf(one.wall).isEmpty());
        QCOMPARE(one.wall.liveStream(), 0U);
        QVERIFY(one.wall.liveDiagnostics().isEmpty());
        for (Window* window : {&one, &two}) {
            QVERIFY(window->scene.streams.empty());
        }
        QTRY_VERIFY(!made.stand->isStreaming());
    }

    // --- a wall screen: from the moment a display link is used until a person signs it out ---

    void aScreenWhoseLinkIsTakenBackStillOnlyShows()
    {
        Qkvm qkvm(u"first"_s);
        qkvm.signedIn = false;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const MadeUpKvm kvm;
        Program program(settings, secrets);
        const QString build = program.addKvm(kvm, u"Build server"_s);
        program.comeToThisPc();
        QTRY_COMPARE(stateOnTheCardOf(program.window.wall, build), u"online"_s);
        QVERIFY(program.window.actions.allowed());
        QVERIFY(!program.merged.onlyShows());
        // The server's wall, by its display link: this PC is a wall screen from now on, and the
        // windows show the wall and nothing else.
        program.server.connectTo(linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(keysOf(program.window.wall).size(), 3);
        QVERIFY(program.merged.onlyShows());
        QVERIFY(program.window.wall.displaying());
        QVERIFY(nothingActs(program.window, build));

        // An admin takes the link back, and the server forgets the session: the server's wall is
        // left, and where it is named it is said why. This PC's KVM is on the wall still, and
        // nothing is done to it: the program only shows, as before.
        qkvm.displayKey = "qkvm_" + QByteArray(43, 'n');
        qkvm.signedIn = false;
        QTRY_COMPARE(keysOf(program.window.wall), QStringList{build});
        QTRY_COMPARE(program.server.shown().problem,
                     u"This display link is not valid any more. Ask an admin for a new one."_s);
        QVERIFY(program.merged.onlyShows());
        QVERIFY(nothingActs(program.window, build));
        QVERIFY(secrets.all.contains(ui::WallScreen::keptAs()));
    }

    void aScreenWhoseServerComesBackSpeakingAnotherVersionStillOnlyShows()
    {
        Qkvm qkvm(u"first"_s);
        qkvm.signedIn = false;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const MadeUpKvm kvm;
        Program program(settings, secrets);
        const QString build = program.addKvm(kvm, u"Build server"_s);
        program.comeToThisPc();
        program.server.connectTo(linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(keysOf(program.window.wall).size(), 3);
        QVERIFY(program.merged.onlyShows());

        // The server is updated, knows no session from before, and speaks another version of
        // its API: its wall is left, and that is said where it is named. Still only shown.
        qkvm.apis = R"(["v2"])";
        qkvm.signedIn = false;
        QTRY_COMPARE(keysOf(program.window.wall), QStringList{build});
        QTRY_VERIFY(program.server.shown().problem.contains(u"v2"_s));
        QVERIFY(program.merged.onlyShows());
        QVERIFY(nothingActs(program.window, build));
    }

    void aScreenWhoseServerShowsAnotherCertificateStillOnlyShows()
    {
        Qkvm qkvm(u"first"_s);
        qkvm.signedIn = false;
        auto server =
            std::make_unique<StandIn>([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("first"));
        const quint16 port = server->serverPort();
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const MadeUpKvm kvm;
        Program program(settings, secrets);
        const QString build = program.addKvm(kvm, u"Build server"_s);
        program.comeToThisPc();
        // Its certificate is looked at and said yes to, and the screen is signed in by its link.
        program.server.connectTo(linkTo(*server, qkvm.displayKey));
        QTRY_COMPARE(program.server.shown().stage, u"trust"_s);
        program.server.trust();
        QTRY_COMPARE(keysOf(program.window.wall).size(), 3);
        QVERIFY(program.merged.onlyShows());

        // Something takes the server's place that shows another certificate: the wall is left,
        // and the certificate is to be looked at. Still only shown.
        server.reset();
        StandIn other([&qkvm](const Asked& asked) { return qkvm.answer(asked); }, tls("second"), port);
        if (!other.isListening()) {
            QSKIP("Windows has given the port to somebody else in the meantime.");
        }
        QTRY_COMPARE_WITH_TIMEOUT(program.server.shown().stage, u"trust"_s, 20'000);
        QCOMPARE(keysOf(program.window.wall), QStringList{build});
        QVERIFY(program.merged.onlyShows());
        QVERIFY(nothingActs(program.window, build));
    }

    void aScreenThatStartsWhileItsServerIsAwayOnlyShows()
    {
        Qkvm qkvm(u"first"_s);
        qkvm.signedIn = false;
        auto server = std::make_unique<StandIn>([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QString address = server->address();
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const MadeUpKvm kvm;
        {
            Program before(settings, secrets);
            before.addKvm(kvm, u"Build server"_s);
            before.server.connectTo(linkTo(*server, qkvm.displayKey));
            QTRY_COMPARE(keysOf(before.window.wall).size(), 2);
            QVERIFY(before.merged.onlyShows());
        }
        // The PC was off, and the server is still.
        server.reset();
        Program program(settings, secrets);
        const QString build = program.firstKvm();
        // A wall screen from the start, before anything is asked anywhere.
        QVERIFY(program.merged.onlyShows());
        QVERIFY(program.window.wall.displaying());
        program.comeToThisPc();
        QTRY_COMPARE(stateOnTheCardOf(program.window.wall, build), u"online"_s);
        QVERIFY(nothingActs(program.window, build));
        // Its server is asked for, by the link that is kept for it, and is not there: it is asked
        // again by itself; this PC is a wall screen all the same.
        program.server.connectTo(address);
        QTRY_VERIFY(program.server.shown().tryingAgain);
        QVERIFY(program.merged.onlyShows());
        QVERIFY(nothingActs(program.window, build));
    }

    void aScreenThatIsStartedAgainWithItsLinkKeptOnlyShows()
    {
        Qkvm qkvm(u"first"_s);
        qkvm.signedIn = false;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const MadeUpKvm kvm;
        {
            Program before(settings, secrets);
            before.addKvm(kvm, u"Build server"_s);
            before.server.connectTo(linkTo(server, qkvm.displayKey));
            QTRY_COMPARE(keysOf(before.window.wall).size(), 2);
        }
        // The program is started again, and the server knows no session from before.
        qkvm.signedIn = false;
        Program program(settings, secrets);
        const QString build = program.firstKvm();
        QVERIFY(program.merged.onlyShows());
        program.comeToThisPc();
        program.server.connectTo(server.address());
        QTRY_COMPARE(keysOf(program.window.wall).size(), 3);
        QCOMPARE(qkvm.displaysOpened, 2);
        QVERIFY(program.merged.onlyShows());
        QVERIFY(nothingActs(program.window, build));
    }

    void onlyAPersonSigningTheScreenOutHereEndsIt()
    {
        Qkvm qkvm(u"first"_s);
        qkvm.signedIn = false;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        Qkvm otherQkvm(u"other"_s);
        StandIn other([&otherQkvm](const Asked& asked) { return otherQkvm.answer(asked); });
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const MadeUpKvm kvm;
        {
            Program program(settings, secrets);
            const QString build = program.addKvm(kvm, u"Build server"_s);
            program.comeToThisPc();
            program.server.connectTo(linkTo(server, qkvm.displayKey));
            QTRY_COMPARE(keysOf(program.window.wall).size(), 3);
            QVERIFY(program.merged.onlyShows());
            // Back to saying which server, and another one signed in to by a person: neither ends
            // the screen, nor does signing out of that other one.
            program.server.leave();
            QVERIFY(program.merged.onlyShows());
            program.server.connectTo(other.address());
            QTRY_COMPARE(keysOf(program.window.wall).size(), 3);
            QVERIFY(program.merged.onlyShows());
            QVERIFY(nothingActs(program.window, build));
            program.server.signOut();
            QVERIFY(program.merged.onlyShows());
            QVERIFY(nothingActs(program.window, build));

            // Back at the first, by the link kept for it, a person signs the screen out here: this
            // PC is no wall screen any more, and whoever sits here may act on its KVM.
            program.server.leave();
            program.server.connectTo(server.address());
            QTRY_COMPARE(keysOf(program.window.wall).size(), 3);
            program.server.signOut();
            QVERIFY(!program.merged.onlyShows());
            QVERIFY(program.window.actions.allowed());
            program.window.actions.open(build);
            QCOMPARE(program.window.actions.menu().value(u"title"_s).toString(), u"Build server"_s);
            QVERIFY(!secrets.all.contains(ui::WallScreen::keptAs()));
        }
        // And the next start is none's.
        const Program program(settings, secrets);
        QVERIFY(!program.merged.onlyShows());
    }

    void whileTheProgramOnlyShowsNoRoadActs_data()
    {
        QTest::addColumn<bool>("appLock");
        QTest::newRow("display-link") << false;
        QTest::newRow("app-lock") << true;
    }

    void whileTheProgramOnlyShowsNoRoadActs()
    {
        QFETCH(bool, appLock);
        // Whoever sits here is an operator at the first server, which refuses what is done, and
        // an admin at this PC's own wall.
        Desk desk;
        const QString build = desk.addKvm(u"Build server"_s);
        desk.firstQkvm.role = u"operator"_s;
        desk.firstQkvm.refusing = true;
        desk.secondQkvm.display = !appLock;
        desk.secondQkvm.role = u"admin"_s;
        Window& menu = desk.window();
        Window& box = desk.window();
        Window& run = desk.window();
        Window& picking = desk.window();
        Window& arranging = desk.window();
        desk.comeToThisPc();
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(menu.wall).size(), 3);
        const QString kvm = key(desk.pc, build);
        const QString firstA = key(desk.first, u"a"_s);
        QTRY_COMPARE(stateOnTheCardOf(menu.wall, kvm), u"online"_s);
        // A menu is open, a box for keys, a run that was refused and can be sent once more, a
        // pick, and this PC's order is being made.
        menu.actions.open(kvm);
        QVERIFY(!menu.actions.menu().isEmpty());
        box.actions.open(firstA);
        box.actions.choose(u"sendKeys"_s);
        QVERIFY(!box.actions.form().isEmpty());
        run.pick({firstA, key(desk.first, u"b"_s)});
        run.actions.openPicked();
        run.actions.choose(u"keys:ShiftLeft"_s);
        run.actions.answerWith(true, -1, false);
        QTRY_COMPARE(run.actions.run().value(u"again"_s).toString(), u"Retry failed"_s);
        const qsizetype refused = desk.firstQkvm.acted.size();
        picking.pick({kvm, firstA});
        QCOMPARE(picking.wall.picked(), 2);
        desk.merged.setOrderOf(desk.pc.id());
        arranging.wall.arrange();
        QVERIFY(arranging.wall.arranging());

        // The second's wall is come to by a display link. Nothing of it all is open any more,
        // and nothing can be done by any road: no menu, no box, no run sent once more, no
        // pick, no saved action, no order.
        if (appLock) {
            desk.merged.setAppRestricted(true);
        }
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(menu.wall).size(), 5);
        QVERIFY(desk.merged.onlyShows());
        const auto nothingOpen = [&] {
            for (Window* window : {&menu, &box, &run, &picking, &arranging}) {
                QVERIFY(!window->actions.allowed());
                QVERIFY(window->actions.menu().isEmpty());
                QVERIFY(window->actions.form().isEmpty());
                QVERIFY(window->actions.question().isEmpty());
                QVERIFY(window->actions.run().isEmpty());
                QCOMPARE(window->wall.picked(), 0);
                QVERIFY(!window->wall.arranging());
                QVERIFY(window->wall.displaying());
            }
        };
        nothingOpen();
        const auto tryEveryRoad = [&] {
            run.actions.retryRun();
            menu.actions.open(kvm);
            menu.actions.choose(u"keys:ShiftLeft"_s);
            menu.actions.openConsole(firstA);
            menu.actions.showPort(kvm, 1);
            box.actions.sendKeys(u"F5"_s);
            picking.pick({kvm, firstA});
            picking.actions.openPicked();
            picking.actions.choose(u"keys:ShiftLeft"_s);
            arranging.wall.arrange();
            arranging.wall.placeAt(kvm, 0);
            arranging.wall.saveOrder();
            QTest::qWait(300);
            QCOMPARE(desk.firstQkvm.acted.size(), refused);
            QVERIFY(desk.secondQkvm.acted.isEmpty());
            QVERIFY(desk.secondQkvm.arranged.isEmpty());
            QCOMPARE(desk.own.kvms().constFirst().id, build);
            QVERIFY(menu.actions.menu().isEmpty());
            QCOMPARE(picking.wall.picked(), 0);
            arranging.wall.stopArranging();
        };
        tryEveryRoad();

        // Its wall is left: still a wall screen, and the same by every road.
        desk.second.leave();
        QTRY_COMPARE(keysOf(menu.wall).size(), 3);
        QVERIFY(desk.merged.onlyShows());
        tryEveryRoad();
    }

    void aWallScreenChangesNoKvmOfThisPcAndAsksNothingOfTheWallAsAFile()
    {
        Qkvm qkvm(u"first"_s);
        qkvm.signedIn = false;
        StandIn server([&qkvm](const Asked& asked) { return qkvm.answer(asked); });
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::testing::MemorySecrets secrets;
        const MadeUpKvm made;
        core::LocalWall own(settings, secrets, quickKvms());
        core::OwnKvm wanted;
        wanted.address = made.stand->address();
        wanted.name = u"Build server"_s;
        const QString kvm = own.add(wanted, u"secret"_s).value();
        ui::SessionView view;
        ui::WallView wall;
        Window window;
        ui::ServerSession session(view, wall, settings, secrets, 100);
        session.add(window.wall, window.actions, nullptr);
        session.useOwnWall(own);
        ui::EditKvmView editing;
        session.serve(editing);
        ui::FilesView files;
        // (A file is kept here; none is chosen.)
        const QString keptAt = folder.filePath(u"wall.json"_s);
        files.setDoor({[keptAt](const QString&) { return base::Result<QString>(keptAt); },
                       [] { return base::Result<QString>(QString()); }});
        session.serve(files);
        const QSignalSpy opening(&editing, &ui::EditKvmView::openAsked);
        // At this PC's own wall the box opens for its KVM.
        view.goToOwnWall();
        editing.openFor(kvm, false);
        QCOMPARE(opening.count(), 1);
        QVERIFY(editing.mayChange());

        // To the server's wall by its display link: the box opens for no KVM of this PC's, and
        // nothing of the wall as a file is asked of the server.
        view.anotherServer();
        view.connectTo(linkTo(server, qkvm.displayKey));
        QTRY_COMPARE(view.stage(), u"wall"_s);
        QVERIFY(!editing.mayChange());
        editing.openFor(kvm, false);
        QCOMPARE(opening.count(), 1);
        const int askedWhatItIs = server.count("/healthz");
        files.exportWall({u"devices"_s}, QString(), u"the right one"_s);
        QTRY_VERIFY(!files.problem().isEmpty());
        QVERIFY2(files.problem().contains(u"only shows"_s), qPrintable(files.problem()));
        QTest::qWait(200);
        QCOMPARE(server.count("/healthz"), askedWhatItIs);
        QCOMPARE(server.count("/api/v1/export"), 0);
        // A person signs the screen out here: back at this PC's own wall, the box opens again.
        view.signOut();
        view.goToOwnWall();
        QVERIFY(editing.mayChange());
        editing.openFor(kvm, false);
        QCOMPARE(opening.count(), 2);
    }

    // --- what one source's word or leaving touches, and no other's ---

    void anOrderAnsweredForASourceThatWasTakenAwayAndAddedAgainIsNobodys()
    {
        NamedSource place(u"one"_s, {u"a"_s, u"b"_s});
        ui::WallView wall;
        ui::ActionsView actions;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(place));
        merged.add(wall, actions, {});
        merged.setOrderOf(place.id());
        place.enter();
        place.feed().start();
        QTRY_COMPARE(keysOf(wall), (QStringList{u"one/a"_s, u"one/b"_s}));
        // An order is sent; before it is answered, the source is taken off the wall and put on
        // it again, and its wall is come to anew (its own turn did not move meanwhile).
        wall.arrange();
        wall.placeAt(u"one/b"_s, 0);
        wall.saveOrder();
        QCOMPARE(place.named.arranged, 1);
        merged.removeSource(place);
        QVERIFY(merged.addSource(place));
        place.enter();
        place.feed().askAnew();
        // The answer is to the taking before: the feed keeps its order, and nothing is told.
        QTest::qWait(150);
        QCOMPARE(place.feed().machines().constFirst().id, u"a"_s);
        QCOMPARE(actions.notices()->rowCount(), 0);
        QTRY_COMPARE(keysOf(wall), (QStringList{u"one/a"_s, u"one/b"_s}));
        place.feed().stop();
    }

    void anOrderMadeWhileTheProgramOnlyShowsIsRefused()
    {
        Desk desk;
        const QString build = desk.addKvm(u"Build server"_s);
        desk.addKvm(u"NAS"_s);
        desk.secondQkvm.display = true;
        Window& window = desk.window();
        desk.comeToThisPc();
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(window.wall).size(), 4);
        QVERIFY(desk.merged.onlyShows());
        // Somebody leaves the wall's only being shown and makes this PC's order: refused there,
        // and this PC's wall keeps its own.
        desk.merged.setOrderOf(desk.pc.id());
        window.wall.setDisplaying(false);
        window.wall.arrange();
        QVERIFY(window.wall.arranging());
        window.wall.placeAt(keysOf(window.wall).at(1), 0);
        window.wall.saveOrder();
        QVERIFY(!window.wall.isSavingOrder());
        QTest::qWait(200);
        QCOMPARE(desk.own.kvms().constFirst().id, build);
        window.wall.stopArranging();
    }

    // M6 2h: two windows show a server's JetKVM large, and are refused it "in use" (somebody else has its one
    // viewer): neither asks again by itself. "Show here", done in one of them (its card's button, asked first),
    // reaches every window that shows it: both ask at once.
    void liveMayOpenAgainReachesEveryWindowShowingIt()
    {
        Desk desk;
        desk.firstQkvm.live = true;
        desk.firstQkvm.liveInUse = true;
        desk.firstQkvm.state = u"busy"_s;
        desk.firstQkvm.capabilities = QStringList{u"live"_s, u"resume"_s};
        desk.merged.retryLiveAt({10, 100, 600, 5, 20});
        Window& one = desk.window();
        Window& two = desk.window();
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(one.wall).size(), 2);
        const QString a = key(desk.first, u"a"_s);
        for (Window* window : {&one, &two}) {
            window->wall.setEnlargedPixels(1000);
            window->wall.enlarge(a);
        }
        QTRY_COMPARE(desk.firstQkvm.liveAsked, 2);
        QTest::qWait(500);
        QCOMPARE(desk.firstQkvm.liveAsked, 2);
        desk.firstQkvm.liveInUse = false;
        one.actions.cardAction(a);
        QVERIFY(!one.actions.question().isEmpty());
        one.actions.answer(true);
        QTRY_COMPARE(desk.firstQkvm.acted.size(), 1);
        QCOMPARE(desk.firstQkvm.acted.first().value(u"action"_s).toString(), u"resume"_s);
        QTRY_COMPARE(desk.firstQkvm.liveAsked, 4);
        // Said of another machine: nothing.
        emit desk.merged.liveMayOpenAgain(key(desk.first, u"b"_s));
        QTest::qWait(300);
        QCOMPARE(desk.firstQkvm.liveAsked, 4);
    }

    // M6 2h, after its report: the card of a server's machine in use elsewhere has "Show here" only for whoever may
    // act there (an operator or an admin), as the page has it; a viewer sees none, and none where the program
    // only shows. (Before: a viewer saw the button, and it did nothing.)
    void showHereIsOnlyForWhoeverMayActThere_data()
    {
        QTest::addColumn<QString>("role");
        QTest::addColumn<bool>("button");
        QTest::newRow("a viewer") << u"viewer"_s << false;
        QTest::newRow("an operator") << u"operator"_s << true;
        QTest::newRow("an admin") << u"admin"_s << true;
    }
    void showHereIsOnlyForWhoeverMayActThere()
    {
        QFETCH(QString, role);
        QFETCH(bool, button);
        Desk desk;
        desk.firstQkvm.role = role;
        desk.firstQkvm.state = u"busy"_s;
        Window& window = desk.window();
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(window.wall).size(), 2);
        const QString a = key(desk.first, u"a"_s);
        QTRY_COMPARE(onTheCardOf(window.wall, a, ui::WallModel::StateRole).toString(), u"busy"_s);
        QCOMPARE(onTheCardOf(window.wall, a, ui::WallModel::ActionRole).toString(),
                 button ? u"Show here"_s : QString());
        // And pressed all the same, it does nothing.
        window.actions.cardAction(a);
        QCOMPARE(window.actions.question().isEmpty(), !button);
    }

    void capabilityChangesDisableAndReleaseControlWithoutClosingAllowedVideo()
    {
        Desk desk;
        desk.firstQkvm.live = true;
        desk.firstQkvm.capabilities = QStringList{u"live"_s};
        Window& window = desk.window();
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl machine(view, keys);
        machine.setInFront(true);
        desk.merged.drive(window.wall, machine);
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(window.wall).size(), 2);
        window.wall.setEnlargedPixels(1000);
        window.wall.enlarge(key(desk.first, u"a"_s));
        QTRY_VERIFY(desk.firstServer.live.isConnected());
        QTRY_VERIFY(!view.isPossible() && !view.textAllowed());
        desk.firstQkvm.capabilities = QStringList{u"live"_s, u"control"_s, u"text"_s};
        desk.first.feed().askAnew();
        QTRY_VERIFY(view.isPossible() && view.textAllowed());
        view.take();
        QTRY_COMPARE(desk.firstServer.live.saidOf(u"control"_s).size(), 1);
        desk.firstServer.live.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
        QTRY_VERIFY(view.isOn());
        desk.firstQkvm.capabilities = QStringList{u"live"_s};
        desk.first.feed().askAnew();
        QTRY_VERIFY(!view.isOn() && !view.isPossible() && !view.textAllowed());
        QVERIFY(!keys.listens);
        QVERIFY(desk.firstServer.live.isConnected());
        desk.firstQkvm.capabilities = QStringList{};
        desk.first.feed().askAnew();
        QTRY_VERIFY(!desk.firstServer.live.isConnected());
    }

    void takeoverBelongsToTheSelectedSourcesPersonalAdminAccount()
    {
        Desk desk;
        desk.firstQkvm.live = true;
        desk.secondQkvm.live = true;
        desk.secondQkvm.role = u"operator"_s;
        Window& window = desk.window();
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl machine(view, keys);
        machine.setInFront(true);
        desk.merged.drive(window.wall, machine);
        desk.comeTo(desk.first);
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(window.wall).size(), 4);
        window.wall.setEnlargedPixels(1000);
        window.wall.enlarge(key(desk.first, u"a"_s));
        QTRY_VERIFY(desk.firstServer.live.isConnected());
        desk.firstServer.live.say(
            {{u"t"_s, u"control"_s}, {u"on"_s, false}, {u"code"_s, u"taken"_s}, {u"by"_s, u"Operator"_s}});
        QTRY_VERIFY(view.takeoverPossible());
        view.take();
        QVERIFY(view.takeoverPending());
        window.wall.enlarge(key(desk.second, u"a"_s));
        QTRY_VERIFY(desk.secondServer.live.isConnected());
        desk.secondServer.live.say(
            {{u"t"_s, u"control"_s}, {u"on"_s, false}, {u"code"_s, u"taken"_s}, {u"by"_s, u"Operator"_s}});
        QTRY_COMPARE(view.ownerName(), u"Operator"_s);
        QVERIFY(!view.takeoverPending());
        QVERIFY(!view.takeoverPossible());
        QVERIFY(view.isPossible());
        view.answerTakeover(true);
        QCOMPARE(desk.secondServer.live.saidOf(u"control"_s).size(), 0);
        desk.secondQkvm.display = true;
        desk.second.leave();
        desk.comeTo(desk.second);
        QTRY_VERIFY(desk.merged.onlyShows());
        QVERIFY(!view.takeoverPossible());
    }

    void keyboardAndMouseAreEachSourcesWord()
    {
        // An operator at the first server, a viewer at the second; both have their machines live.
        Desk desk;
        desk.firstQkvm.role = u"operator"_s;
        desk.secondQkvm.role = u"viewer"_s;
        desk.firstQkvm.live = true;
        desk.secondQkvm.live = true;
        Window& window = desk.window();
        ui::ControlView view;
        ui::KeysByHand keys;
        ui::MachineControl machine(view, keys);
        machine.setInFront(true);
        desk.merged.drive(window.wall, machine);
        desk.comeTo(desk.first);
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(window.wall).size(), 4);
        QVERIFY(window.actions.allowed());
        window.wall.setEnlargedPixels(1000);
        // The viewer's machine is had live, and nothing may be taken of it.
        window.wall.enlarge(key(desk.second, u"a"_s));
        QTRY_VERIFY(desk.secondServer.live.isConnected());
        QTest::qWait(200);
        QVERIFY(!view.isPossible());
        // The operator's: they may.
        window.wall.enlarge(key(desk.first, u"a"_s));
        QTRY_VERIFY(desk.firstServer.live.isConnected());
        QTRY_VERIFY(view.isPossible());
        // And nobody may, where the program only shows.
        desk.secondQkvm.display = true;
        desk.second.leave();
        desk.comeTo(desk.second);
        QTRY_VERIFY(desk.merged.onlyShows());
        QTRY_VERIFY(!view.isPossible());
    }

    void whatALeavingSourceHadUnderWayIsOverWhereAnotherStays()
    {
        // What is done to the first's machines takes long; the first's machines have consoles
        // that a pass is made out for, slowly, and so have the second's.
        Desk desk;
        desk.firstQkvm.actsAfterMs = 1500;
        desk.firstQkvm.consoles = true;
        desk.firstQkvm.consoleAfterMs = 1500;
        desk.secondQkvm.consoles = true;
        Window& window = desk.window();
        QList<QUrl> opened;
        desk.merged.openPagesWith([&opened](const QUrl& address) {
            opened.append(address);
            return true;
        });
        desk.comeTo(desk.first);
        desk.comeTo(desk.second);
        QTRY_COMPARE(keysOf(window.wall).size(), 4);
        const QString firstA = key(desk.first, u"a"_s);
        QTRY_VERIFY(onTheCardOf(window.wall, firstA, ui::WallModel::StateRole).toString() == u"online"_s);
        // A press to the first's machine is under way, and said so in the corner; and a pass into
        // the console of its other machine is being made out.
        window.actions.open(firstA);
        window.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(desk.firstQkvm.acted.size(), 1);
        window.actions.openConsole(key(desk.first, u"b"_s));
        QTRY_COMPARE(desk.firstQkvm.consoleAsked, 1);
        const auto working = [&] {
            int under = 0;
            for (int row = 0; row < window.actions.notices()->rowCount(); ++row) {
                const QModelIndex notice = window.actions.notices()->index(row, 0);
                under += notice.data(ui::Notices::KindRole).toString() == "working"_L1 ? 1 : 0;
            }
            return under;
        };
        QCOMPARE(working(), 2);

        // The first's wall is left while the second's stays: nothing of the first's is under way
        // any more, and nothing says so.
        desk.first.leave();
        QTRY_COMPARE(keysOf(window.wall).size(), 2);
        QCOMPARE(working(), 0);
        // A pass into a console of the second's is made out at once: none of the first's holds it
        // back.
        window.actions.openConsole(key(desk.second, u"a"_s));
        QTRY_COMPARE(desk.secondQkvm.consoleAsked, 1);
        // Come back to the first: its machine takes a press at once, and is not said to have one
        // under way still.
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(window.wall).size(), 4);
        window.actions.open(firstA);
        window.actions.choose(u"keys:ShiftLeft"_s);
        QTRY_COMPARE(desk.firstQkvm.acted.size(), 2);
        QVERIFY(!toldIn(window.actions).join(u'\n').contains(u"still under way"_s));
    }

    // --- the cards of a source, and how the windows show the wall ---

    void aWindowOpenedBeforeAFeedHasAnsweredShowsNothingOfBefore()
    {
        NamedSource place(u"one"_s, {u"a"_s});
        ui::WallView wall;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(place));
        place.enter();
        place.feed().start();
        QTRY_COMPARE(keysOf(wall), QStringList{u"one/a"_s});
        // Its wall is left, and its place has other machines now (as another server at the same
        // address, or the same after a while). Somebody comes to its wall again, and a window is
        // opened before the feed has told the machines anew: it shows none of the machines from
        // before, and no key of theirs reaches anything.
        place.feed().stop();
        place.leave();
        place.named.wall.machines.first().id = u"b"_s;
        place.named.wall.machines.first().name = u"b"_s;
        place.named.status.insert(u"b"_s, place.named.status.take(u"a"_s));
        place.enter();
        place.feed().start();
        ui::WallView shown;
        ui::ActionsView actions;
        merged.add(shown, actions, {});
        QVERIFY(keysOf(shown).isEmpty());
        QCOMPARE(reached(merged, u"one/a"_s), u"nothing"_s);
        // Then they come.
        QTRY_COMPARE(keysOf(shown), QStringList{u"one/b"_s});
        QCOMPARE(reached(merged, u"one/a"_s), u"nothing"_s);
        merged.remove(shown);
        place.feed().stop();
    }

    void whatASourceNobodyIsAtSaysOfWhoeverItIsIsNothing()
    {
        NamedSource one(u"one"_s, {u"a"_s});
        NamedSource two(u"two"_s, {u"b"_s});
        ui::WallView wall;
        ui::ActionsView actions;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(one));
        QVERIFY(merged.addSource(two));
        merged.add(wall, actions, {});
        // Nobody is at either wall: what the second says of whoever is at its wall lets nobody
        // act, and puts none of its machines on the wall.
        two.tellWho(anAdmin());
        QVERIFY(!actions.allowed());
        // Somebody who may only look is at the first's: still nobody may act.
        core::Account viewer = anAdmin();
        viewer.role = u"viewer"_s;
        one.enter(viewer);
        one.feed().start();
        QTRY_COMPARE(keysOf(wall), QStringList{u"one/a"_s});
        two.tellWho(anAdmin());
        QVERIFY(!actions.allowed());
        QVERIFY(keysOf(wall) == QStringList{u"one/a"_s});
        one.feed().stop();
    }

    void anotherSourceThatComesLeavesTheWayEveryWindowShowsTheWall()
    {
        NamedSource one(u"one"_s, {u"a"_s});
        NamedSource two(u"two"_s, {u"b"_s});
        ui::WallView wall;
        ui::ActionsView actions;
        ui::MergedWall merged(wall);
        QVERIFY(merged.addSource(one));
        QVERIFY(merged.addSource(two));
        merged.add(wall, actions, {});
        one.enter();
        one.feed().start();
        QTRY_COMPARE(keysOf(wall), QStringList{u"one/a"_s});
        // Whoever sits here chooses to have it only shown; somebody comes to the second's wall:
        // the window stays as it was chosen.
        wall.setDisplaying(true);
        two.enter();
        QVERIFY(wall.displaying());
        // And the other way round.
        two.leave();
        wall.setDisplaying(false);
        two.enter();
        QVERIFY(!wall.displaying());
        // The first to come to a wall nobody is at sees it as a person does.
        one.leave();
        two.leave();
        wall.setDisplaying(true);
        one.enter();
        QVERIFY(!wall.displaying());
        one.feed().stop();
    }

    void everySourcesPaceIsOffered()
    {
        // The two servers name a pace each, a quarter of a second; the second names another.
        Desk desk;
        Window& window = desk.window();
        desk.comeTo(desk.first);
        QTRY_COMPARE(keysOf(window.wall).size(), 2);
        QCOMPARE(window.wall.pace(), 250);
        // (A place of its own, with a pace of seven seconds, as the second source here.)
        NamedSource other(u"other"_s, {u"x"_s});
        QVERIFY(desk.merged.addSource(other));
        other.enter();
        other.feed().start();
        QTRY_COMPARE(keysOf(window.wall).size(), 3);
        // Both are among the choices; the first's holds.
        QVERIFY(window.wall.paces().contains(250));
        QVERIFY(window.wall.paces().contains(7000));
        QCOMPARE(window.wall.pace(), 250);
        window.wall.setPace(7000);
        QCOMPARE(window.wall.pace(), 7000);
        other.feed().stop();
        desk.merged.removeSource(other);
    }
};

QTEST_GUILESS_MAIN(TestMergedWall)
#include "test_merged_wall.moc"
