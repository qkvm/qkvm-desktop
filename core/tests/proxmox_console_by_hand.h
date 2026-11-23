#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include "proxmox_by_hand.h"
#include "rfb_console_by_hand.h"

namespace qkvm::core::testing {

// The consoles of a Proxmox by hand's guests (the web's fake_proxmox.py vncwebsocket, with fake_vnc.py behind it): GET
// /api2/json/nodes/<node>/qemu/<vmid>/vncwebsocket?port=<port>&vncticket=<ticket> on the Proxmox's own stand-in, let
// in with the API token in its Authorization header and a ticket the Proxmox handed out (vncproxy) for that guest, at
// that port, within its window and once; and behind it the guest's VNC server (RfbConsoleByHand, one for each guest),
// which answers with the subprotocol "binary" and asks for VNC authentication with the ticket's one-time password.
// * Without the token, or with a ticket that is none, 401; a ticket used twice, or past its window: no answer at all
//   (the line kept, `hung`); "upgrade_500" (the Proxmox's `hostile`): 500.
// * A guest that is not running, or whose console fails (`fails`): the upgrade is taken, and closed half a second later
//   with no greeting.
class ProxmoxConsoles {
public:
    explicit ProxmoxConsoles(ProxmoxByHand& pve) : m_pve(pve)
    {
        pve.stand->upgrade = [this](QTcpSocket* socket, const QByteArray& request) { return upgrade(socket, request); };
    }

    int upgrades = 0;
    int hung = 0;
    int refused = 0;
    QSet<qint64> fails; // the guests whose console's worker fails

    // The VNC server of a guest, by its number: made the first time it is asked for.
    RfbConsoleByHand& of(qint64 vmid)
    {
        auto& made = m_consoles[vmid];
        if (made == nullptr) {
            made = std::make_unique<RfbConsoleByHand>();
            made->name = "QEMU (" + m_pve.guests.value(vmid).name.toStdString() + ")";
            made->security = {2};
            made->speaks({QStringLiteral("binary")});
            made->door = [this, vmid](const QByteArray& request) { return decide(vmid, request); };
        }
        return *made;
    }
    // (The guest deleted: its VNC server gone with it, so that one made later under its number has one of its own.)
    void forget(qint64 vmid) { m_consoles.erase(vmid); }
    // Every console's lines cut at once, with no goodbye (the Proxmox gone from the network): RfbConsoleByHand's.
    void cutEveryLine()
    {
        for (const auto& one : m_consoles) {
            one.second->cutEveryLine();
        }
    }

private:
    struct Asking {
        QString node;
        qint64 vmid = 0;
        QString port;
        QString ticket;
    };

    static std::optional<Asking> askingIn(const QByteArray& request)
    {
        const QByteArray line = request.left(request.indexOf('\r'));
        const QList<QByteArray> words = line.split(' ');
        if (words.size() < 2 || words.at(0) != "GET") {
            return std::nullopt;
        }
        const QUrl url(QString::fromLatin1(words.at(1)));
        const QStringList steps = url.path().split(u'/', Qt::SkipEmptyParts);
        if (steps.size() != 7 || steps.at(0) != QStringLiteral("api2") || steps.at(2) != QStringLiteral("nodes") ||
            steps.at(4) != QStringLiteral("qemu") || steps.at(6) != QStringLiteral("vncwebsocket")) {
            return std::nullopt;
        }
        const QUrlQuery query(url);
        return Asking{steps.at(3), steps.at(5).toLongLong(), query.queryItemValue(QStringLiteral("port")),
                      query.queryItemValue(QStringLiteral("vncticket"), QUrl::FullyDecoded)};
    }

    bool upgrade(QTcpSocket* socket, const QByteArray& request)
    {
        const std::optional<Asking> asked = askingIn(request);
        if (!asked) {
            return false;
        }
        return of(asked->vmid).letInFrom(socket, request);
    }

    RfbConsoleByHand::Door decide(qint64 vmid, const QByteArray& request)
    {
        static const QByteArray unauthorized = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
        const std::optional<Asking> asked = askingIn(request);
        if (!asked) {
            return {};
        }
        ++upgrades;
        if (request.left(request.indexOf("\r\n")).contains(m_pve.secret.toLatin1())) {
            m_pve.inTheClear.append(QStringLiteral("vncwebsocket"));
        }
        QByteArray authorization;
        for (const QByteArray& line : request.split('\n')) {
            if (line.toLower().startsWith("authorization:")) {
                authorization = line.mid(14).trimmed();
            }
        }
        if (QString::fromLatin1(authorization) != m_pve.authorization()) {
            ++refused;
            ++m_pve.refused;
            return {true, unauthorized};
        }
        if (m_pve.hostile == QStringLiteral("upgrade_500")) {
            return {true, QByteArray("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n")};
        }
        ProxmoxByHand::Ticket* found = nullptr;
        for (ProxmoxByHand::Ticket& one : m_pve.tickets) {
            if (one.ticket == asked->ticket) {
                found = &one;
            }
        }
        if (found == nullptr || found->vmid != vmid || found->port != asked->port || !m_pve.guests.contains(vmid) ||
            m_pve.guests.value(vmid).node != asked->node) {
            return {true, unauthorized}; // "permission denied - invalid PVEVNC ticket"
        }
        const qint64 age = m_pve.sinceMs() - found->madeMs;
        if (found->used || age > m_pve.windowMs || m_pve.hostile == QStringLiteral("silent_upgrade")) {
            ++hung;
            return {true, QByteArray()}; // (no answer at all: Proxmox leaves the asking hanging)
        }
        if (age > m_pve.ticketLifeMs) {
            return {true, unauthorized};
        }
        found->used = true;
        RfbConsoleByHand& console = of(vmid);
        const bool works = m_pve.guests.value(vmid).status == QStringLiteral("running") && !fails.contains(vmid);
        console.silent = !works;
        console.closesSilentlyAfterMs = works ? 0 : 500;
        // (What `qm vncproxy` sets as the guest's VNC password, for this connection.)
        console.password = found->password.toStdString();
        if (!works) {
            m_pve.tasks[found->task].status = QStringLiteral("Failed to run vncproxy.");
        }
        return {true, std::nullopt};
    }

    ProxmoxByHand& m_pve;
    std::map<qint64, std::unique_ptr<RfbConsoleByHand>> m_consoles;
};

} // namespace qkvm::core::testing
