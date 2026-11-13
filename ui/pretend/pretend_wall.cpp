#include "pretend_wall.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

constexpr int picturedMachines = 9;

WallTile live(quint32 stream, const QString& name, const QString& host, const QString& meta)
{
    WallTile tile;
    tile.stream = stream;
    tile.name = name;
    tile.host = host;
    tile.state = u"online"_s;
    tile.label = u"Online"_s;
    tile.meta = meta;
    return tile;
}

WallTile without(const QString& name, const QString& host, const QString& state, const QString& title,
                 const QString& text, const QString& meta = {}, const QString& action = {})
{
    WallTile tile;
    tile.name = name;
    tile.host = host;
    tile.state = state;
    tile.label = title;
    tile.title = title;
    tile.text = text;
    tile.meta = meta;
    tile.action = action;
    return tile;
}

QVariantMap reading(const QString& label, const QString& value, const QString& tone = {})
{
    return {{u"label"_s, label}, {u"value"_s, value}, {u"tone"_s, tone}};
}

WallTile server(const QString& name, const QString& host, const QString& model, bool on, const QString& tone,
                const QVariantList& readings, const QString& hardware, bool locating = false)
{
    WallTile tile;
    tile.name = name;
    tile.host = host;
    // A server that is switched off counts with the machines that give no picture.
    tile.state = on ? u"online"_s : u"no_signal"_s;
    tile.label = on ? u"Online"_s : u"Powered off"_s;
    tile.meta = model;
    tile.server = {
        {u"power"_s, on ? u"on"_s : u"off"_s},
        {u"tone"_s, tone},
        {u"locating"_s, locating},
        {u"readings"_s, readings},
        {u"hardware"_s, hardware},
    };
    return tile;
}

QVariantMap port(const QString& label, const QString& hint, bool shown, bool empty)
{
    return {{u"label"_s, label}, {u"hint"_s, hint}, {u"shown"_s, shown}, {u"empty"_s, empty}};
}

} // namespace

int pretendWallPictured()
{
    return picturedMachines;
}

QList<WallTile> pretendWall()
{
    QList<WallTile> wall;
    wall.append(live(1, u"Build server"_s, u"192.168.1.21"_s, u"1920×1080 · Power on · 43°C"_s));
    wall.append(live(2, u"NAS"_s, u"192.168.1.22"_s, u"1920×1080 · Power on · 45°C"_s));
    wall.append(live(3, u"Hypervisor 1"_s, u"192.168.1.23"_s, u"1920×1080 · Power on · 47°C"_s));
    wall.append(live(4, u"Hypervisor 2"_s, u"192.168.1.24"_s, u"1920×1080 · Power on · 51°C"_s));
    wall.append(live(5, u"Firewall"_s, u"192.168.1.25"_s, u"1920×1080 · 43°C"_s));

    WallTile rack = live(6, u"Rack switch (4 ports)"_s, u"192.168.1.31"_s, u"web-01 · 1920×1080 · 43°C"_s);
    rack.ports = {
        port(u"1"_s, u"web-01: on screen now"_s, true, false),
        port(u"2"_s, u"db-01: show this computer"_s, false, false),
        port(u"3"_s, u"admin-pc: show this computer"_s, false, false),
        port(u"4"_s, u"Port 4: nothing connected"_s, false, true),
    };
    wall.append(rack);

    wall.append(live(7, u"Reception PC"_s, u"192.168.1.41"_s, u"1920×1080 · 43°C"_s));
    wall.append(live(8, u"Design workstation"_s, u"192.168.1.42"_s, u"2560×1440 · 42°C"_s));
    wall.append(live(9, u"Meeting room"_s, u"192.168.1.43"_s, u"1920×1080 · 42°C"_s));

    wall.append(without(u"Test bench"_s, u"192.168.1.51"_s, u"no_signal"_s, u"No signal"_s,
                        u"The computer is off, asleep or not plugged into the KVM."_s, u"Power off · 47°C"_s));
    wall.append(without(u"Old server"_s, u"192.168.1.52"_s, u"offline"_s, u"Offline"_s,
                        u"Device is unreachable · last seen 2 h ago"_s));
    wall.append(without(u"Backup box"_s, u"192.168.1.53"_s, u"auth"_s, u"Login failed"_s,
                        u"Login rejected - check the username and password"_s, {}, u"Update login"_s));

    wall.append(server(
        u"Database server"_s, u"192.168.1.71"_s, u"SRV-2200"_s, true, u"ok"_s,
        {reading(u"Health"_s, u"OK"_s, u"ok"_s), reading(u"Temp"_s, u"61 °C"_s), reading(u"Fans"_s, u"4 running"_s)},
        u"2 × 12 cores · 256 GiB"_s));
    wall.append(server(u"Render node"_s, u"192.168.1.72"_s, u"SRV-2200"_s, true, u"warn"_s,
                       {reading(u"Health"_s, u"Warning"_s, u"warn"_s), reading(u"Temp"_s, u"62 °C"_s),
                        reading(u"Fans · 1 not well"_s, u"4 running"_s, u"warn"_s)},
                       u"2 × 32 cores · 512 GiB"_s, true));
    wall.append(server(
        u"Spare node"_s, u"192.168.1.73"_s, u"SRV-1100"_s, false, u"ok"_s,
        {reading(u"Health"_s, u"OK"_s, u"ok"_s), reading(u"Temp"_s, u"30 °C"_s), reading(u"Fans"_s, u"0 running"_s)},
        u"6 cores · 32 GiB"_s));
    // Each is its own machine, known by its address. The first four stand in one rack, the next
    // five in another, three in the office, and the servers are in no group.
    for (qsizetype at = 0; at < wall.size(); ++at) {
        wall[at].key = wall[at].host;
        wall[at].group = at < 4 ? u"Rack A"_s : at < 9 ? u"Rack B"_s : at < 12 ? u"Office"_s : QString();
        // All but one have a console a browser can be sent to.
        wall[at].openable = at != 4;
        // And whoever looks at this wall may act on every machine of it, as the screens are
        // told (ActionsView::allowed).
        wall[at].actable = true;
    }
    return wall;
}

} // namespace qkvm::ui
