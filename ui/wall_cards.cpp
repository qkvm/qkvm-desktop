#include "wall_cards.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// The words of a card are the page's, and are translated where the page's are: under one heading.
QString words(const char* text)
{
    return QCoreApplication::translate("Wall", text);
}

QString titleOf(const QString& state)
{
    if (state == "connecting"_L1) {
        return words("Connecting…");
    }
    if (state == "no_signal"_L1) {
        return words("No signal");
    }
    if (state == "offline"_L1) {
        return words("Offline");
    }
    if (state == "auth"_L1) {
        return words("Login failed");
    }
    if (state == "busy"_L1) {
        return words("In use elsewhere");
    }
    return state == "online"_L1 ? QString() : words("Problem");
}

// A state in a word, as the page says it beside a machine's name.
QString labelOf(const QString& state, const core::Status* status)
{
    if (state == "online"_L1) {
        return words("Online");
    }
    // A server that is switched off is not a computer without a picture: it is called what it is.
    if (state == "no_signal"_L1 && status != nullptr && !status->hasPicture) {
        return words("Powered off");
    }
    return titleOf(state);
}

QString sentenceOf(const core::Status& status)
{
    if (status.state == "no_signal"_L1) {
        return words("The computer is off, asleep or not plugged into the KVM.");
    }
    if (status.state == "offline"_L1 && status.lastSeenSecondsAgo) {
        const QString why = status.errorMessage.isEmpty() ? words("Device is unreachable") : status.errorMessage;
        return words("%1 · last seen %2").arg(why, ago(*status.lastSeenSecondsAgo));
    }
    if (status.state == "busy"_L1) {
        return words("Someone has this JetKVM's own console open. It shows its picture to one viewer at a time.");
    }
    return status.errorMessage;
}

// The right end of the line under a KVM's picture: whose picture it is, how large, whether the
// computer is on, how warm the KVM is.
QString machineMetaOf(const core::Status& status)
{
    QStringList parts;
    if (!status.hasPicture) {
        // A server's panel already says whether it is on and how warm: its line says which machine
        // it is. With its screen on the card, the line says what the panel would have.
        if (status.width <= 0) {
            return status.model;
        }
        parts.append(u"%1×%2"_s.arg(status.width).arg(status.height));
        if (status.server && !status.server->health.isEmpty() && status.server->health != "OK"_L1) {
            parts.append(status.server->health);
        }
        if (status.temperature) {
            parts.append(u"%1°C"_s.arg(std::lround(*status.temperature)));
        }
        return parts.join(u" · "_s);
    }
    if (status.shownPort >= 0) {
        parts.append(portName(status.ports[status.shownPort], status.shownPort));
    }
    if (status.width > 0 && status.height > 0) {
        parts.append(u"%1×%2"_s.arg(status.width).arg(status.height));
    }
    if (status.powerIsKnown) {
        // A computer that sends a picture is on. A power light that says otherwise is not wired up,
        // and "Power off" under a running machine is worse than saying nothing.
        if (status.power == std::optional<bool>(true)) {
            parts.append(words("Power on"));
        } else if (status.power == std::optional<bool>(false) && status.signal != std::optional<bool>(true)) {
            parts.append(words("Power off"));
        }
    }
    // A Fingerbot paired with the KVM, and its battery where that is low.
    if (status.fingerbot) {
        const std::optional<int>& battery = status.fingerbot->battery;
        parts.append(status.fingerbot->low && battery ? words("Fingerbot battery low (%1%)").arg(*battery)
                                                      : words("Fingerbot"));
    }
    if (status.keyboard == std::optional<bool>(false)) {
        parts.append(words("USB not connected"));
    }
    if (status.temperature) {
        parts.append(u"%1°C"_s.arg(std::lround(*status.temperature)));
    }
    return parts.join(u" · "_s);
}

// And after it what is in the machine's virtual drive, said to everybody who sees the card, a server's too (app.js,
// metaText): "CD: debian-13.iso".
QString metaOf(const core::Status& status)
{
    QStringList parts{machineMetaOf(status)};
    if (status.media) {
        const QString& drive = status.media->drive;
        const QString kind = drive == "cd"_L1      ? words("CD")
                             : drive == "flash"_L1 ? words("Flash")
                             : drive == "disk"_L1  ? words("Disk")
                                                   : words("Media");
        parts.append(words("%1: %2").arg(kind, status.media->name));
    }
    parts.removeAll(QString());
    return parts.join(u" · "_s);
}

QVariantList portsOf(const core::Machine& machine, const core::Status& status)
{
    QVariantList ports;
    for (qsizetype index = 0; index < status.ports.size(); ++index) {
        const core::Port& port = status.ports[index];
        const bool shown = index == status.shownPort;
        const QString note = shown        ? words("on screen now")
                             : port.empty ? words("nothing connected")
                                          : words("not on screen");
        ports.append(QVariantMap{
            {u"label"_s, QString::number(index + 1)},
            {u"hint"_s, words("%1 of %2: %3").arg(portName(port, index), machine.name, note)},
            {u"shown"_s, shown},
            {u"empty"_s, port.empty},
        });
    }
    return ports;
}

QVariantMap reading(const QString& label, const QString& value, const QString& tone = {})
{
    return {{u"label"_s, label}, {u"value"_s, value}, {u"tone"_s, tone}};
}

// What a server's card shows where a KVM's shows the screen.
QVariantMap panelOf(const core::Status& status)
{
    const core::ServerReadings& server = *status.server;
    const QString tone = server.health == "OK"_L1         ? u"ok"_s
                         : server.health == "Warning"_L1  ? u"warn"_s
                         : server.health == "Critical"_L1 ? u"bad"_s
                                                          : QString();
    QVariantList readings;
    readings.append(reading(words("Health"), server.health.isEmpty() ? words("Not reported") : server.health, tone));
    if (status.temperature) {
        readings.append(reading(words("Temp"), u"%1 °C"_s.arg(std::lround(*status.temperature))));
    }
    if (server.fans > 0) {
        readings.append(
            reading(server.fansNotWell > 0 ? words("Fans · %1 not well").arg(server.fansNotWell) : words("Fans"),
                    words("%1 running").arg(server.fansRunning), server.fansNotWell > 0 ? u"warn"_s : QString()));
    }
    QStringList hardware;
    if (!server.processor.isEmpty()) {
        hardware.append(server.processors > 1 ? u"%1 × %2"_s.arg(server.processors).arg(server.processor)
                                              : server.processor);
    }
    if (server.memoryGib) {
        hardware.append(u"%1 GiB"_s.arg(*server.memoryGib));
    }
    return {
        // Only a controller that says the server is off makes it "Off".
        {u"power"_s, status.power == std::optional<bool>(false) ? u"off"_s : u"on"_s},
        {u"tone"_s, tone},
        {u"locating"_s, server.locator.value_or(false)},
        {u"readings"_s, readings},
        {u"hardware"_s, hardware.join(u" · "_s)},
        // Why there is no picture where there could be one, as the page's panel says it.
        {u"why"_s, server.noScreen.isEmpty() ? QString() : words("Its screen is not shown: %1").arg(server.noScreen)},
        // A start into BIOS setup that did not end may have left its boot override set (review 4's MEDIUM 1).
        {u"setupLeft"_s, server.setupLeft},
    };
}

// What a card says of a still it shows, as the page's stillNote: "still, 12 s ago", "Stills, 2 min
// ago"; the word alone where its age is not said; nothing where it shows none.
QString stillNote(const std::optional<StillCame>& still, const QString& word)
{
    if (!still) {
        return {};
    }
    if (!still->ageSeconds) {
        return word;
    }
    const int age = std::max(0, *still->ageSeconds);
    return words("%1, %2 ago")
        .arg(word, age < 60 ? words("%1 s").arg(age) : words("%1 min").arg(std::lround(age / 60.0)));
}

QString actionOf(CardWay way)
{
    switch (way) {
    case CardWay::UpdateLogin:
        return words("Update login");
    case CardWay::ReviewCertificate:
        return words("Review certificate");
    case CardWay::ShowHere:
        return words("Show here");
    case CardWay::None:
        break;
    }
    return {};
}

} // namespace

engine::Codec cardCodecOf(const core::Machine& machine)
{
    return machine.own && machine.kind == "jetkvm"_L1 ? engine::Codec::KeyFrameStill : engine::Codec::Still;
}

CardWay cardWayOf(const core::Machine& machine, const core::Status* status)
{
    if (status == nullptr) {
        return CardWay::None;
    }
    // In use elsewhere: its picture taken back, on this PC's wall and a server's alike (the page's
    // veilContent: state "busy", "Show here").
    if (status->state == "busy"_L1) {
        return CardWay::ShowHere;
    }
    if (!machine.own) {
        return CardWay::None;
    }
    // (And where another JetKVM answers at its address: its password typed in the box that changes it takes the one
    // that answers now, as the card's words say. J5.)
    if (status->state == "auth"_L1 || status->errorCode == "another_machine"_L1) {
        return CardWay::UpdateLogin;
    }
    // A JetKVM of this PC's wall that answers, after an offer of this program's came to nothing (its video does not
    // come through to this PC): its cards make no other, and "Show here" tries again (from the second reading of
    // c7ec58c, B).
    if (status->errorCode == "not_through"_L1) {
        return CardWay::ShowHere;
    }
    return status->errorCode == "cert_mismatch"_L1 ? CardWay::ReviewCertificate : CardWay::None;
}

QString portName(const core::Port& port, qsizetype index)
{
    return port.name.isEmpty() ? words("Port %1").arg(index + 1) : port.name;
}

bool whileRestarting(WallTile& card, Restart& restart, qint64 nowMs, bool isServer)
{
    if (nowMs > restart.untilMs) {
        return false;
    }
    if (card.state == "online"_L1) {
        // Back, after having been away: over. Still there, because it has not gone down yet: waited for.
        return !restart.wentDown;
    }
    restart.wentDown = true;
    // However it is away (unreachable, refusing the login while it starts), it is restarting.
    card.state = u"connecting"_s;
    card.label = words("Rebooting…");
    card.title = words("Rebooting…");
    card.text = isServer ? words("Back in a minute or two.") : words("Back in about a minute.");
    card.action.clear();
    // Neither its last picture nor how a server was doing: what it says is in their place.
    card.stream = 0;
    card.server.clear();
    card.ports.clear();
    return true;
}

QString ago(int seconds)
{
    if (seconds < 10) {
        return words("just now");
    }
    if (seconds < 90) {
        return words("%1 s ago").arg(seconds);
    }
    if (seconds < 5400) {
        return words("%1 min ago").arg(std::lround(seconds / 60.0));
    }
    if (seconds < 129600) {
        return words("%1 h ago").arg(std::lround(seconds / 3600.0));
    }
    return words("%1 d ago").arg(std::lround(seconds / 86400.0));
}

QString viewNoteOf(const core::Machine& machine, const core::Status* status, const std::optional<StillCame>& still,
                   bool large)
{
    const bool screen = core::hasAScreenToShow(machine, status);
    const QString which = core::amtViewOf(machine, status);
    if (which.isEmpty()) {
        return screen ? stillNote(still, words("still")) : QString();
    }
    const QString view = amtViewWord(which);
    if (which == "stills"_L1 && screen && still) {
        return stillNote(still, view); // "Stills, 2 min ago"
    }
    if (which == "stills"_L1 && large && screen) {
        return words("Stills on the wall, live here");
    }
    const QString age = screen ? stillNote(still, words("still")) : QString();
    return age.isEmpty() ? view : view + u" · "_s + age;
}

QString amtViewWord(const QString& view)
{
    return view == "health"_L1 ? words("Health only") : view == "live"_L1 ? words("Live KVM") : words("Stills");
}

WallTile cardFor(const core::Machine& machine, const core::Status* status, quint32 stream, bool hasFrame,
                 const std::optional<StillCame>& still)
{
    // The end of its line: what it says otherwise, and what it says of what it shows.
    const auto withNote = [&](const QString& meta, bool large) {
        const QString note = viewNoteOf(machine, status, still, large);
        return meta.isEmpty() ? note : note.isEmpty() ? meta : meta + u" · "_s + note;
    };
    WallTile tile;
    tile.key = machine.id;
    tile.name = machine.name;
    // Where a machine is, and before it the way it is spoken to where that is the plain one: the
    // one thing about an address that whoever looks at the wall has to know.
    tile.host = machine.plain ? u"http://"_s + machine.host : machine.host;
    tile.group = machine.group;
    tile.note = machine.note;
    tile.openable = !machine.page.isEmpty();
    tile.stream = machine.hasPicture ? stream : 0;
    tile.amtView = core::amtViewOf(machine, status);
    // (How large its screen is, as its status says: its large view is drawn by that at a scale.)
    if (status != nullptr && status->width > 0 && status->height > 0) {
        tile.pictureSize = QSize(status->width, status->height);
    }
    if (status == nullptr) {
        // The server has not said yet how this machine is.
        tile.state = u"connecting"_s;
        tile.label = labelOf(tile.state, nullptr);
        tile.title = titleOf(tile.state);
        // (A PC with Intel AMT says its view by the wall's word until then.)
        tile.meta = withNote({}, false);
        tile.largeMeta = withNote({}, true);
        return tile;
    }
    tile.state = status->state;
    tile.label = labelOf(status->state, status);
    tile.meta = withNote(metaOf(*status), false);
    tile.largeMeta = withNote(metaOf(*status), true);
    if (status->server) {
        const bool runs = status->state == "online"_L1 || status->state == "error"_L1;
        // (Never the picture of a PC with Intel AMT in health only, whatever was handed over.)
        if (stream != 0 && hasFrame && runs && core::hasAScreenToShow(machine, status)) {
            tile.stream = stream;
        } else {
            tile.server = panelOf(*status);
        }
        return tile;
    }
    tile.title = titleOf(status->state);
    tile.text = status->state == "online"_L1 ? QString() : sentenceOf(*status);
    if (machine.own && status->errorCode == "not_through"_L1) {
        // It answers (its state and its alerts say so), and its picture does not come: said plainly over it.
        tile.title = words("No video from here");
        tile.text = words("The JetKVM answers, but its video does not come through to this PC. Show here tries again.");
    }
    tile.action = actionOf(cardWayOf(machine, status));
    tile.ports = portsOf(machine, *status);
    return tile;
}

} // namespace qkvm::ui
