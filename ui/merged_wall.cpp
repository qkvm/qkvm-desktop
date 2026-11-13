#include "merged_wall.h"

#include <algorithm>
#include <span>
#include <utility>

#include <QCoreApplication>
#include <QSet>

#include <qkvm/base/clock.h>
#include <qkvm/core/server_client.h>

#include "control.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// (In the words of the session, which said them before the windows' wall was a class of its own.)
QString words(const char* text)
{
    return QCoreApplication::translate("Session", text);
}

QString sentence(const base::Error& error)
{
    return QString::fromStdString(error.message);
}

// How wide a picture is asked for, for a card whose picture is this many of the monitor's pixels
// wide: as the page asks (sharpWidth in app/qkvm/static/app.js), twice that and never less than 480.
// The spare detail keeps the scaled-down picture crisp; and whoever asks as the page does is given
// the picture the server already made for a browser looking at the same wall.
int widthToAskFor(int cardPixels)
{
    return std::max(480, cardPixels * 2);
}

// The widest picture asked for (the server's own limit, the page's WHOLE_WIDTH): the machine's whole
// picture, whatever it is, as the enlarged view asks for it at a scale of the machine's own pixels (b10).
constexpr int wholeWidth = 16384;

// How wide a machine's live pictures are asked for, where they are drawn this many of the
// monitor's pixels wide: no wider than they are drawn, and in steps, so that a window that is
// being dragged larger says so now and then and not with every pixel.
int liveWidthFor(int pixels)
{
    constexpr int step = 160;
    // (And never wider than a server's line takes: one wider is none to it, which gives the narrowest.)
    return std::min(wholeWidth, std::max(step, (pixels + step - 1) / step * step));
}

// The pace the wall's windows offer where no source that somebody is at names one: as a feed says
// it before its place has named one, a second.
constexpr int paceWhereNoneIsNamedMs = 1000;

} // namespace

MergedWall::MergedWall(WallView& wall, QObject* parent) : WallMachines(parent), m_wall(wall)
{
    m_clock.start();
    // How often pictures are asked for is chosen once, for every source.
    connect(&m_wall, &WallView::paceChanged, this, [this] {
        for (const auto& side : m_sides) {
            side->source->feed().setPictureEvery(m_wall.ownPace());
        }
    });
}

MergedWall::~MergedWall()
{
    // The streams are the engines', and an engine may go on after this.
    for (Screen& screen : m_screens) {
        dropStreams(screen);
    }
    // Keyboard and mouse of a machine are let go while its line still stands.
    for (const Screen& screen : m_screens) {
        if (screen.control != nullptr) {
            screen.control->have(nullptr, {});
        }
    }
}

template <typename Does>
void MergedWall::onEveryWall(Does does)
{
    does(m_wall);
    for (const Screen& screen : m_screens) {
        does(*screen.wall);
    }
}

MergedWall::Side* MergedWall::sideOf(const Source* source)
{
    const auto side = std::ranges::find_if(m_sides, [&](const auto& one) { return one->source == source; });
    return side == m_sides.end() ? nullptr : side->get();
}

MergedWall::Side* MergedWall::sideNamed(const QString& id)
{
    const auto side = std::ranges::find_if(m_sides, [&](const auto& one) { return one->id == id; });
    return side == m_sides.end() ? nullptr : side->get();
}

// --- the sources ---------------------------------------------------------------------------------------

bool MergedWall::addSource(Source& source)
{
    const QString id = source.id();
    if (!isSourceId(id) || sideOf(&source) != nullptr || sideNamed(id) != nullptr) {
        return false;
    }
    Side& side = *m_sides.emplace_back(std::make_unique<Side>());
    side.source = &source;
    side.id = id;
    side.serial = ++m_lastSide;
    listenTo(source);
    source.feed().setPictureEvery(m_wall.ownPace());
    return true;
}

void MergedWall::removeSource(Source& source)
{
    Side* side = sideOf(&source);
    if (side == nullptr) {
        return;
    }
    // Its wall is left here as when it leaves it by itself, and all that was shown of it goes.
    if (side->who) {
        leave(source);
    }
    empty(source);
    // Nothing of it is drawn any more, and nothing is asked of it for this wall.
    source.feed().setShown({});
    source.feed().setLookedAtClosely({});
    disconnect(&source.feed(), nullptr, this, nullptr);
    disconnect(&source, nullptr, this, nullptr);
    const QString id = side->id;
    for (Screen& screen : m_screens) {
        screen.actions->forgetSource(id);
    }
    std::erase_if(m_sides, [&](const auto& one) { return one->source == &source; });
    tellThePace();
}

bool MergedWall::onlyShows() const
{
    return m_appRestricted || (m_screen != nullptr && m_screen->isOne()) ||
           std::ranges::any_of(m_sides, [](const auto& side) { return side->who && side->who->isDisplay; });
}

void MergedWall::setAppRestricted(bool restricted)
{
    m_appRestricted = restricted;
    lookWhetherItOnlyShows();
}

void MergedWall::onlyShowWhile(const WallScreen& screen)
{
    if (m_screen != nullptr) {
        return;
    }
    m_screen = &screen;
    connect(&screen, &WallScreen::changed, this, &MergedWall::lookWhetherItOnlyShows, Qt::DirectConnection);
    lookWhetherItOnlyShows();
}

void MergedWall::lookWhetherItOnlyShows()
{
    const bool shows = onlyShows();
    if (shows == m_onlyShowed) {
        return;
    }
    m_onlyShowed = shows;
    if (shows) {
        // A screen nobody sits at: nobody is in the middle of making an order, and every window
        // shows the wall and nothing else. (In that order: a wall whose order is being made keeps
        // its bar.)
        onEveryWall([](WallView& wall) {
            wall.stopArranging();
            wall.setDisplaying(true);
        });
    }
    // Whoever sits here may act nowhere from now on, or where each source says again.
    tellWhoMayAct();
    emit onlyShowsChanged();
}

void MergedWall::setOrderOf(const QString& source)
{
    if (source == m_orderOf) {
        return;
    }
    m_orderOf = source;
    onEveryWall([&](WallView& wall) { wall.setOrderOf(source); });
}

bool MergedWall::mayAt(const Side& side) const
{
    return side.who && MachineActions::mayActAs(*side.who) && !onlyShows();
}

void MergedWall::listenTo(Source& added)
{
    // (By the source, which stays the same, and not by its side here, which goes with it.)
    Source* source = &added;
    core::WallFeed& feed = source->feed();
    connect(&feed, &core::WallFeed::wallChanged, this, [this, source] {
        Side* side = sideOf(source);
        if (side == nullptr || !side->who) {
            return;
        }
        // (Its feed has told its machines in this coming to its wall: they are the wall's now.)
        side->answered = true;
        fill(*side);
        // How often pictures are asked for: as was chosen on the wall, among paces of which the
        // sources' own are some.
        tellThePace();
        // Last of all: whoever does things to the machines hears of a changed wall once the wall's
        // cards are filled with it.
        emit wallChanged();
    });
    connect(&feed, &core::WallFeed::statusChanged, this, [this, source] {
        Side* side = sideOf(source);
        if (side == nullptr || !side->who) {
            return;
        }
        fill(*side);
        // The source hears that its machines were looked at once the cards show it, and before
        // whoever does things to them fills a menu anew.
        source->looked();
        emit statusChanged();
    });
    connect(&feed, &core::WallFeed::pictureCame, this,
            [this, source](const QString& machine, const QByteArray& picture, bool) {
                const Side* side = sideOf(source);
                const std::optional<QString> key = side != nullptr ? wallKey(side->id, machine) : std::nullopt;
                if (!key || !m_streams.contains(*key) || picture.isEmpty()) {
                    return;
                }
                // The picture goes to the engine as it came, a still being a picture by itself: to
                // the engine of every window, each of which draws it at its own size.
                const engine::Packet packet{
                    std::as_bytes(std::span(picture.constData(), static_cast<std::size_t>(picture.size()))),
                    base::steadyNowUs(), true};
                for (const Screen& screen : m_screens) {
                    if (engine::Stream* stream = screen.streams.value(*key); stream != nullptr) {
                        stream->push(packet);
                    }
                    // Where a last live picture of it was still up, this is newer.
                    if (screen.wall->enlarged() == *key) {
                        screen.live->snapshotCame();
                    }
                }
                // A still kept between sessions, or no longer one: its card says so at once (and its
                // age at the wall's next look).
                Pictured& pictured = m_streams[*key];
                const bool still = source->feed().isStill(machine);
                if (pictured.still != still && pictured.hasFrame) {
                    pictured.still = still;
                    if (Side* of = sideOf(source); of != nullptr) {
                        fill(*of);
                    }
                    return;
                }
                pictured.still = still;
                notePicture(*key, true);
            });
    connect(&feed, &core::WallFeed::pictureFailed, this,
            [this, source](const QString& machine, const QString& code, const QString&) {
                // The QKVM server's word that there is no picture to be had of this machine. That
                // the server itself could not be reached says nothing of the machine: the wall
                // shows what it last knew, a server's screen with the rest. Nor does a question
                // that was not sent, while what answers at the server's address has not said
                // again that it is the QKVM.
                const Side* side = sideOf(source);
                const std::optional<QString> key = side != nullptr ? wallKey(side->id, machine) : std::nullopt;
                if (key && code != QLatin1StringView(core::ServerClient::unreachable) &&
                    code != QLatin1StringView(core::ServerClient::notSent)) {
                    notePicture(*key, false);
                }
            });
    // What the source says of itself, there and then: a state that follows asks for pictures in
    // the same turn, and what is shown of it is shown before.
    connect(
        source, &Source::entered, this, [this, source](const core::Account& account) { enter(*source, account); },
        Qt::DirectConnection);
    connect(
        source, &Source::whoChanged, this, [this, source](const core::Account& account) { know(*source, account); },
        Qt::DirectConnection);
    connect(source, &Source::left, this, [this, source] { leave(*source); }, Qt::DirectConnection);
    connect(source, &Source::emptied, this, [this, source] { empty(*source); }, Qt::DirectConnection);
    connect(
        source, &Source::stateChanged, this,
        [this, source] {
            for (const Screen& screen : m_screens) {
                tellDetails(*screen.wall);
            }
            // Its cards say whether it is away: where that is other now, they are filled anew
            // (which asks for pictures).
            if (Side* side = sideOf(source); side != nullptr && side->who && side->away != source->isAway()) {
                stamp(*side);
                join();
                return;
            }
            askForPictures();
        },
        Qt::DirectConnection);
}

void MergedWall::enter(Source& source, const core::Account& account)
{
    Side* side = sideOf(&source);
    if (side == nullptr) {
        return;
    }
    const bool anybodyElse =
        std::ranges::any_of(m_sides, [&](const auto& other) { return other.get() != side && other->who; });
    side->who = account;
    // (Its machines come onto the wall once its feed has told them anew: what the feed holds from
    // before is nobody's.)
    side->answered = false;
    settle(*side);
    // How the windows show the wall. While the program only shows (a display link, which is what
    // has it only show), the wall and no bar, in every window. Otherwise, whoever comes first to a
    // wall nobody is at sees it as a person does; and whoever comes to another source's wall
    // beside one that is up leaves every window as it is, as somebody may have chosen it.
    if (onlyShows()) {
        onEveryWall([](WallView& wall) { wall.setDisplaying(true); });
    } else if (!anybodyElse) {
        onEveryWall([](WallView& wall) { wall.setDisplaying(false); });
    }
}

void MergedWall::know(Source& source, const core::Account& account)
{
    Side* side = sideOf(&source);
    // (Of a source nobody is at: whoever it says that is, is nobody here.)
    if (side == nullptr || !side->who) {
        return;
    }
    side->who = account;
    settle(*side);
}

void MergedWall::settle(const Side& side)
{
    tellWhoMayAct();
    // The wall's own order is an admin's to make: whoever is one no longer is not in the middle
    // of making this source's.
    if (side.who && side.who->role != "admin"_L1) {
        stopArranging(side);
    }
    // (A display link's: the program only shows from now on.)
    lookWhetherItOnlyShows();
}

void MergedWall::leave(Source& source)
{
    Side* side = sideOf(&source);
    if (side == nullptr) {
        return;
    }
    side->who.reset();
    for (const Screen& screen : m_screens) {
        screen.actions->leave(side->id);
        tellDetails(*screen.wall);
    }
    // Where it was what had the program only show, and this PC is no wall screen, whoever sits
    // here may act on the others again.
    lookWhetherItOnlyShows();
}

void MergedWall::empty(Source& source)
{
    Side* side = sideOf(&source);
    if (side == nullptr) {
        return;
    }
    // No machine of it is waited for to restart, picked, or being put in order; where no other
    // source's are on the wall, nothing is being picked at all.
    for (auto key = side->ids.cbegin(); key != side->ids.cend(); ++key) {
        m_restarts.remove(key.key());
    }
    const bool had = !side->machines.isEmpty();
    const bool othersStay = std::ranges::any_of(
        m_sides, [&](const auto& other) { return other.get() != side && !other->machines.isEmpty(); });
    if (!othersStay) {
        onEveryWall([](WallView& wall) { wall.setSelecting(false); });
    }
    stopArranging(*side);
    // Its cards go, and every engine's streams of them.
    side->machines.clear();
    side->status.clear();
    side->ids.clear();
    side->tiles.clear();
    join();
    tellThePace();
    if (had) {
        emit wallChanged();
        emit statusChanged();
    }
}

void MergedWall::stopArranging(const Side& side)
{
    onEveryWall([&](WallView& wall) {
        if (wall.arranging() && wall.arrangingOf() == side.id) {
            wall.stopArranging();
        }
    });
}

void MergedWall::tellWhoMayAct()
{
    for (const Screen& screen : m_screens) {
        for (const auto& side : m_sides) {
            if (side->who) {
                screen.actions->enter(side->id, mayAt(*side));
            }
        }
    }
    // A card says whether whoever sits here may act on it, and has its button only then: made anew.
    bool stamped = false;
    for (const auto& side : m_sides) {
        if (side->who && side->actable != mayAt(*side)) {
            build(*side);
            stamped = stamped || !side->tiles.isEmpty();
        }
    }
    if (stamped) {
        join();
    }
    for (const Screen& screen : m_screens) {
        tellControl(*screen.wall);
        tellDetails(*screen.wall);
    }
}

// --- the cards -----------------------------------------------------------------------------------------

void MergedWall::stamp(Side& side) const
{
    side.actable = mayAt(side);
    side.away = side.source->isAway();
    for (WallTile& tile : side.tiles) {
        tile.actable = side.actable;
        tile.away = side.away;
    }
}

void MergedWall::build(Side& side)
{
    side.machines.clear();
    side.status.clear();
    side.ids.clear();
    side.tiles.clear();
    stamp(side);
    // Nobody at its wall, or its feed has not told its machines since somebody came to it.
    if (!side.who || !side.answered) {
        return;
    }
    const core::WallFeed& feed = side.source->feed();
    for (const core::Machine& machine : feed.machines()) {
        const std::optional<QString> key = wallKey(side.id, machine.id);
        if (!key) {
            continue; // (never: the source's id was looked at where it was added)
        }
        const auto status = feed.status().constFind(machine.id);
        const core::Status* said = status == feed.status().constEnd() ? nullptr : &*status;
        // On the wall, by its key.
        core::Machine onTheWall = machine;
        onTheWall.id = *key;
        quint32 stream = 0;
        bool hasFrame = false;
        // A KVM has a picture. So has a server whose own screen the QKVM server reads; and never a PC
        // with Intel AMT in health only (hasAScreenToShow).
        if (core::hasAScreenToShow(machine, said)) {
            Pictured& pictured = m_streams[*key];
            if (pictured.number == 0) {
                pictured.number = ++m_lastStream;
                pictured.codec = cardCodecOf(machine);
                for (Screen& screen : m_screens) {
                    addStream(screen, *key, pictured.number, pictured.codec);
                }
            }
            stream = pictured.number;
            hasFrame = pictured.hasFrame;
        }
        // (Its last picture a still kept between sessions: how old it is now.)
        const std::optional<StillCame> still = stream != 0 && feed.isStill(machine.id)
                                                   ? std::optional<StillCame>(StillCame{feed.stillAgeOf(machine.id)})
                                                   : std::nullopt;
        WallTile card = cardFor(onTheWall, said, stream, hasFrame, still);
        card.source = side.id;
        card.away = side.away;
        card.actable = side.actable;
        // A card's button is for whoever may act on the machine: nobody else sees one (the page's veilContent
        // takes "Show here" away below an operator, and the rest below an admin; a screen that only shows acts
        // on nothing).
        if (!card.actable) {
            card.action.clear();
        }
        if (const auto restart = m_restarts.find(*key);
            restart != m_restarts.end() && !whileRestarting(card, *restart, m_clock.elapsed(), !machine.hasPicture)) {
            m_restarts.erase(restart);
        }
        side.machines.append(onTheWall);
        if (said != nullptr) {
            side.status.insert(*key, *said);
        }
        side.ids.insert(*key, machine.id);
        side.tiles.append(card);
    }
}

void MergedWall::fill(Side& side)
{
    build(side);
    join();
}

void MergedWall::join()
{
    m_machines.clear();
    m_status.clear();
    m_reached.clear();
    QList<WallTile> tiles;
    // The sources in their order, each in its own.
    for (const auto& side : m_sides) {
        m_machines.append(side->machines);
        tiles.append(side->tiles);
        for (auto status = side->status.cbegin(); status != side->status.cend(); ++status) {
            m_status.insert(status.key(), status.value());
        }
        for (auto id = side->ids.cbegin(); id != side->ids.cend(); ++id) {
            m_reached.insert(id.key(), Reached{&side->source->place(), id.value(), side->id});
        }
    }
    // (A machine that left the wall is not waited for.)
    m_restarts.removeIf([&](const auto& waited) { return !m_reached.contains(waited.key()); });
    // A machine that left the wall takes its stream with it; its card goes first, so that no card
    // names a stream that is gone.
    m_filling = true;
    onEveryWall([&](WallView& wall) { wall.machines()->setTiles(tiles); });
    m_filling = false;
    for (auto known = m_streams.begin(); known != m_streams.end();) {
        if (m_reached.contains(known.key())) {
            ++known;
            continue;
        }
        for (Screen& screen : m_screens) {
            if (screen.streams.remove(known.key())) {
                if (engine::Scene* scene = screen.scene(); scene != nullptr) {
                    scene->remove(known.value().number);
                }
            }
        }
        known = m_streams.erase(known);
    }
    askForPictures();
}

int MergedWall::serverPace() const
{
    for (const auto& side : m_sides) {
        if (side->who) {
            return side->source->feed().serverPaceMs();
        }
    }
    return paceWhereNoneIsNamedMs;
}

QList<int> MergedWall::otherPaces() const
{
    QList<int> others;
    bool first = true;
    for (const auto& side : m_sides) {
        if (!side->who) {
            continue;
        }
        if (!std::exchange(first, false)) {
            others.append(side->source->feed().serverPaceMs());
        }
    }
    return others;
}

void MergedWall::tellThePace()
{
    onEveryWall([this](WallView& wall) { tellThePace(wall); });
}

void MergedWall::tellThePace(WallView& wall) const
{
    // (The others first: the first's then holds among choices that are already all there.)
    wall.setOtherPaces(otherPaces());
    wall.setServerPace(serverPace());
}

void MergedWall::notePicture(const QString& key, bool came)
{
    const auto pictured = m_streams.find(key);
    if (pictured == m_streams.end() || pictured->hasFrame == came) {
        return;
    }
    pictured->hasFrame = came;
    if (Side* side = sideNamed(m_reached.value(key).source); side != nullptr) {
        fill(*side);
    }
}

void MergedWall::lookAgainSoon(const QString& key)
{
    // (One feed for each source: however many of its machines something was done to, that is one
    // round of looks there.)
    if (Side* side = sideNamed(m_reached.value(key).source); side != nullptr) {
        side->source->feed().lookAgainSoon();
    }
}

// --- the windows ---------------------------------------------------------------------------------------

void MergedWall::addStream(Screen& screen, const QString& key, quint32 number, engine::Codec codec)
{
    engine::Scene* scene = screen.scene();
    if (scene == nullptr) {
        return;
    }
    engine::Stream& stream = scene->add(number, codec);
    // A wall's pictures are small ones, decoded for a card and not for one machine shown large.
    stream.shownAt({}, engine::Importance::Preview);
    screen.streams.insert(key, &stream);
}

void MergedWall::dropStreams(Screen& screen)
{
    // (An engine that has ended took its streams with it.)
    if (engine::Scene* scene = screen.scene(); scene != nullptr) {
        for (auto stream = screen.streams.cbegin(); stream != screen.streams.cend(); ++stream) {
            scene->remove(m_streams.value(stream.key()).number);
        }
    }
    screen.streams.clear();
}

void MergedWall::add(WallView& wall, ActionsView& actions, Engine drawsIn)
{
    Screen& screen = m_screens.emplace_back();
    screen.wall = &wall;
    screen.scene = drawsIn ? std::move(drawsIn) : Engine([]() -> engine::Scene* { return nullptr; });
    screen.actions = std::make_unique<MachineActions>(actions, wall, *this, *m_actionRegistry);
    screen.actions->openPagesWith(m_opener);
    connect(screen.actions.get(), &MachineActions::restarted, this, [this](const QString& key) {
        m_restarts.insert(key, Restart{m_clock.elapsed() + restartWithinMs, false});
        if (Side* side = sideNamed(m_reached.value(key).source); side != nullptr) {
            fill(*side);
        }
    });
    connect(&wall, &WallView::cardPixelsChanged, this, &MergedWall::askForPictures);
    connect(&wall, &WallView::inSightChanged, this, &MergedWall::askForPictures);
    connect(&wall, &WallView::enlargedChanged, this, &MergedWall::askForPictures);
    connect(&wall, &WallView::enlargedPixelsChanged, this, &MergedWall::askForPictures);
    connect(&wall, &WallView::enlargedScaleChanged, this, &MergedWall::askForPictures);
    connect(&wall, &WallView::unseenChanged, this, &MergedWall::askForPictures);
    connect(&wall, &WallView::orderAsked, this,
            [this, &wall, &actions](const QStringList& keys) { arrange(wall, actions, keys); });
    screen.live = std::make_unique<LivePicture>(LivePicture::Around{screen.scene, [this] { return ++m_lastStream; },
                                                                    [this](const QString& key) { return reach(key); },
                                                                    m_openLive},
                                                m_livePace);
    connect(screen.live.get(), &LivePicture::changed, this, &MergedWall::askForPictures);
    // "Show here" in any window takes a JetKVM's viewer back for every window that shows it (M6 2h).
    connect(screen.actions.get(), &MachineActions::resumed, this, &WallMachines::liveMayOpenAgain);
    connect(this, &WallMachines::liveMayOpenAgain, screen.live.get(), &LivePicture::openAgain);
    connect(screen.live.get(), &LivePicture::diagnosticsChanged, &wall,
            [picture = screen.live.get(), &wall] { wall.setLiveDiagnostics(picture->diagnostics()); });
    // Whoever sends keyboard and mouse from this window hears of whatever they go by.
    screen.may = &actions;
    // A machine's virtual media through its qKVM: an admin of that qKVM, signed in in person, and not while the
    // program only shows. Never of this PC's own wall, which has no server.
    screen.actions->useMedia([this](const QString& source) -> core::ServerClient* {
        const Side* side = sideNamed(source);
        if (side == nullptr || !side->who || !side->answered || onlyShows() || side->who->role != "admin"_L1 ||
            side->who->isDisplay || side->who->mustEnroll) {
            return nullptr;
        }
        return side->source->detailsClient();
    });
    connect(screen.live.get(), &LivePicture::liveChanged, this, [this, &wall] { tellControl(wall); });
    connect(&actions, &ActionsView::allowedChanged, this, [this, &wall] { tellControl(wall); });
    connect(&wall, &WallView::displayingChanged, this, [this, &wall] { tellControl(wall); });
    connect(&wall, &WallView::enlargedChanged, this, [this, &wall] { tellControl(wall); });
    connect(this, &MergedWall::wallChanged, &wall, [this, &wall] { tellControl(wall); });
    connect(this, &MergedWall::statusChanged, &wall, [this, &wall] { tellControl(wall); });
    connect(&wall, &WallView::enlargedChanged, this, [this, &wall] { tellDetails(wall); });
    connect(&wall, &WallView::enlargedChanged, this, [this, &wall] { tellNoteSeen(wall); });
    connect(wall.details(), &DetailsView::changed, this, [this, &wall] { tellNoteSeen(wall); });
    connect(&wall, &WallView::displayingChanged, this, [this, &wall] { tellDetails(wall); });
    connect(&wall, &WallView::unseenChanged, this, [this, &wall] { tellDetails(wall); });
    connect(this, &MergedWall::wallChanged, &wall, [this, &wall] { tellDetails(wall); });
    connect(this, &MergedWall::statusChanged, &wall, [this, &wall] { tellDetails(wall); });
    // And of why a line ended by itself, before they are handed the next one or none.
    connect(screen.live.get(), &LivePicture::lineEndedBecause, this, [this, &wall](const QString& message) {
        const auto told = std::ranges::find_if(m_screens, [&](const Screen& one) { return one.wall == &wall; });
        if (told != m_screens.end() && told->control != nullptr) {
            told->control->lineEnded(message);
        }
    });
    for (auto pictured = m_streams.cbegin(); pictured != m_streams.cend(); ++pictured) {
        addStream(screen, pictured.key(), pictured->number, pictured->codec);
    }
    tellThePace(wall);
    wall.setOrderOf(m_orderOf);
    // A window of a program that only shows shows the wall and nothing else.
    if (onlyShows()) {
        wall.setDisplaying(true);
    }
    // A window that comes while somebody is at a source's wall: whoever is there is there in it
    // too, it has the machines the others have (those that a feed has told since somebody came to
    // its wall), and pictures of them now and not at their next turn (which, while the wall is
    // paused, would not come).
    bool anybody = false;
    for (const auto& side : m_sides) {
        if (side->who) {
            screen.actions->enter(side->id, mayAt(*side));
            anybody = true;
        }
    }
    if (!anybody) {
        return;
    }
    for (const auto& side : m_sides) {
        build(*side);
    }
    join();
    for (const auto& side : m_sides) {
        if (side->who) {
            side->source->feed().showAnew();
        }
    }
}

void MergedWall::remove(WallView& wall)
{
    wall.details()->bind(nullptr, {}, {}, false, false);
    wall.details()->setOpened(false);
    const auto screen = std::ranges::find_if(m_screens, [&](const Screen& one) { return one.wall == &wall; });
    if (screen == m_screens.end()) {
        return;
    }
    // Keyboard and mouse of its machine are let go while the machine's line still stands.
    if (screen->control != nullptr) {
        screen->control->have(nullptr, {});
    }
    dropStreams(*screen);
    m_screens.erase(screen);
    askForPictures();
}

void MergedWall::drive(WallView& wall, MachineControl& control)
{
    const auto screen = std::ranges::find_if(m_screens, [&](const Screen& one) { return one.wall == &wall; });
    if (screen == m_screens.end()) {
        return;
    }
    screen->control = &control;
    screen->actions->useControl(control);
    tellControl(wall);
}

void MergedWall::tellControl(const WallView& wall)
{
    const auto screen = std::ranges::find_if(m_screens, [&](const Screen& one) { return one.wall == &wall; });
    if (screen == m_screens.end() || screen->control == nullptr) {
        return;
    }
    // Whether whoever sits here may act on the machine shown large, by the word of its own source
    // (with none shown large, as on the window's wall at all); and not at a wall that is only shown:
    // nobody sits at such a screen.
    const QString enlarged = wall.enlarged();
    const bool may =
        enlarged.isEmpty() ? screen->may != nullptr && screen->may->allowed() : screen->actions->mayActOn(enlarged);
    screen->control->setAllowed(may && !wall.displaying() && screen->actions->supportsOn(enlarged, u"control"_s));
    screen->control->setTextAllowed(screen->actions->supportsOn(enlarged, u"text"_s));
    const Reached at = m_reached.value(enlarged);
    const Side* side = at ? sideNamed(at.source) : nullptr;
    screen->control->setTakeoverAllowed(side != nullptr && side->who && side->who->role == "admin"_L1 &&
                                        !side->who->isDisplay && !side->who->mustEnroll && !onlyShows());
    screen->control->have(screen->live->live(), enlarged);
}

void MergedWall::tellDetails(WallView& wall)
{
    const Reached at = m_reached.value(wall.enlarged());
    const Side* side = at ? sideNamed(at.source) : nullptr;
    if (side == nullptr || !side->who || !side->answered || onlyShows() || wall.displaying() || wall.isUnseen() ||
        side->who->mustEnroll || side->who->isDisplay || !side->source->mayBeLive()) {
        wall.details()->bind(nullptr, {}, {}, false, false);
        return;
    }
    // A KVM of this PC's own wall: its screen watches' alerts.
    if (side->source->detailsClient() == nullptr && side->source->screens() != nullptr) {
        wall.details()->bindOwn(side->source->screens(), at.machine);
        return;
    }
    const QString scope =
        QString::number(side->serial) + u':' + QString::number(side->source->turn()) + u':' + side->who->id;
    wall.details()->bind(side->source->detailsClient(), at.machine, scope, side->who->role == "admin"_L1, true);
}

void MergedWall::tellNoteSeen(WallView& wall)
{
    const auto screen = std::ranges::find_if(m_screens, [&](const Screen& one) { return one.wall == &wall; });
    if (screen == m_screens.end()) {
        return;
    }
    const QString enlarged = wall.enlarged();
    if (!wall.details()->opened() || enlarged.isEmpty()) {
        screen->noteSeenFor.clear();
        return;
    }
    if (screen->noteSeenFor == enlarged) {
        return;
    }
    // (Not at a screen that only shows: nobody sits there to have seen it.)
    const Reached at = m_reached.value(enlarged);
    const Side* side = at ? sideNamed(at.source) : nullptr;
    if (side == nullptr || !side->who || onlyShows() || wall.displaying() || wall.isUnseen()) {
        return;
    }
    screen->noteSeenFor = enlarged;
    side->source->place().sawNote(at.machine);
}

void MergedWall::arrange(WallView& wall, ActionsView& actions, const QStringList& keys)
{
    // Of the machines of one source alone, the one the window's arranging is of, by their own ids
    // there, and for whoever may make that source's order: an admin of it, at its wall. A list
    // that mixes sources, names a machine that is not on the wall, or is of another source than
    // the arranging's, is refused; and so is any while the program only shows.
    Side* side = nullptr;
    QStringList machines;
    bool ofOne = !keys.isEmpty();
    for (const QString& key : keys) {
        const Reached at = m_reached.value(key);
        Side* of = at ? sideNamed(at.source) : nullptr;
        if (of == nullptr || (side != nullptr && of != side)) {
            ofOne = false;
            break;
        }
        side = of;
        machines.append(at.machine);
    }
    if (!ofOne || side->id != wall.arrangingOf() || !side->who || !side->source->isAtTheWall() ||
        !side->source->manages() || onlyShows()) {
        wall.orderRefused();
        return;
    }
    // (Its answer is taken in the turn of that source it was asked in, while the source is the one
    // it was on this wall.)
    Source& source = *side->source;
    source.place().arrange(machines, [guard = QPointer<MergedWall>(this), of = QPointer<Source>(&source),
                                      serial = side->serial, turn = source.turn(), asked = QPointer<WallView>(&wall),
                                      told = QPointer<ActionsView>(&actions)](const base::Result<QStringList>& order) {
        // Its wall was left meanwhile, or it is no longer on this wall, or the window closed, or the
        // wall as the windows have it ended: the answer is nobody's.
        const Side* still = guard != nullptr && of != nullptr ? guard->sideOf(of.data()) : nullptr;
        if (still == nullptr || still->serial != serial || turn != of->turn() || asked == nullptr || told == nullptr) {
            return;
        }
        if (!order) {
            asked->orderRefused();
            told->notices()->tell(Notices::Kind::Failed, sentence(order.error()));
            return;
        }
        // The source's word for the order is the wall's from this moment, in every window; and the
        // window it was made in shows it.
        of->feed().takeOrder(order.value());
        asked->orderSaved();
        told->notices()->tell(Notices::Kind::Done, words("The order is saved, for everybody who looks at this wall"));
    });
}

void MergedWall::openPagesWith(std::function<bool(const QUrl&)> opener)
{
    m_opener = std::move(opener);
    for (const Screen& screen : m_screens) {
        screen.actions->openPagesWith(m_opener);
    }
}

void MergedWall::openThePage(const QUrl& address)
{
    if (!m_screens.empty()) {
        m_screens.front().actions->openThePage(address);
    }
}

void MergedWall::askForPictures()
{
    if (m_filling) {
        return;
    }
    // Of each machine that has a picture: the widest it is drawn in any window, asked of its own
    // source by its own id there. And the machines that are looked at alone.
    QHash<const Side*, QHash<QString, int>> shown;
    QHash<const Side*, QSet<QString>> closely;
    const auto wanted = [&](const QString& key, int width) -> const Side* {
        if (!m_streams.contains(key)) {
            return nullptr;
        }
        const Reached at = m_reached.value(key);
        const Side* side = sideNamed(at.source);
        if (side == nullptr) {
            return nullptr;
        }
        int& widest = shown[side][at.machine];
        widest = std::max(widest, width);
        return side;
    };
    for (const Screen& screen : m_screens) {
        WallView& wall = *screen.wall;
        // The machine a window shows large is had live there, where its own source has live
        // pictures in a form this program knows: while a server answers, or at this PC's own wall,
        // where each KVM fails by itself; and while somebody can see the window. What the window
        // draws of it is the live picture's to say.
        const QString enlarged = wall.enlarged();
        const Reached at = m_reached.value(enlarged);
        const Side* of = at ? sideNamed(at.source) : nullptr;
        const bool live = of != nullptr && of->source->feed().liveForm() == core::spokenLiveForm &&
                          of->source->mayBeLive() && !wall.isUnseen() &&
                          screen.actions->supportsOn(enlarged, u"live"_s);
        // (At a scale of its own pixels, the whole of its picture, as the page asks for it.)
        const bool whole = wall.enlargedScale() != u"fit"_s;
        screen.live->show(m_streams.contains(enlarged) ? enlarged : QString(),
                          whole ? wholeWidth : liveWidthFor(wall.enlargedPixels()), live);
        wall.setLiveStream(screen.live->stream());
        wall.setLive(!screen.live->wantsSnapshots());
        // A window nobody can see asks for no pictures. How the machines are is still looked at:
        // the program's icon says it.
        if (wall.isUnseen()) {
            continue;
        }
        // A machine that is shown large covers its window's wall: pictures of it, as large as it
        // is drawn and faster than the wall's, and for this window of no other. Unless its live
        // pictures come: then none is asked for.
        if (!enlarged.isEmpty()) {
            if (screen.live->wantsSnapshots()) {
                if (const Side* side = wanted(enlarged, whole ? wholeWidth : widthToAskFor(wall.enlargedPixels()));
                    side != nullptr) {
                    closely[side].insert(at.machine);
                }
            }
            continue;
        }
        // Every machine whose card is in sight, for the width its card has on the screen. A card
        // that the bar's choices have taken off the wall, or that is scrolled far out of sight,
        // asks for nothing.
        const int width = widthToAskFor(wall.cardPixels());
        const QStringList inSight = wall.inSight();
        for (const QString& key : inSight) {
            wanted(key, width);
        }
    }
    for (const auto& side : m_sides) {
        core::WallFeed& feed = side->source->feed();
        feed.setShown(shown.value(side.get()));
        feed.setLookedAtClosely(closely.value(side.get()));
    }
}

} // namespace qkvm::ui
