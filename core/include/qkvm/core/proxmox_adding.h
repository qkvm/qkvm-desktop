#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/device_asker.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/own_sign_in.h>
#include <qkvm/core/proxmox_answers.h>

namespace qkvm::core {

// Adding Proxmox VE guests to this PC's own wall (wave 2, c9b), as the web's page does it (app.js fromProxmox, the
// server's POST /api/v1/proxmox/guests and its adding of a guest): the address of a Proxmox (one guest named after it
// where somebody wants that one at once: 192.168.1.5/100), the API token's ID and secret, and a group.
//
// 1. list(): what was typed read by the web's rules (proxmox::addressOf, the token's shapes: what is no token is sent
//    nowhere, "invalid"). Its certificate looked at before the token goes anywhere, by a driver of its own whose
//    sign-in asks the unsigned question of its API first (GET /api2/json/version, which carries nothing), on a line
//    that Windows' trust decides; where Windows does not trust what it shows, it is known by that certificate from then
//    on (trust on first use, as a KVM's certificate is, and as the web pins one on first contact), asked that question
//    by a driver on a line held to it, and the token goes only to what shows it. And only to a Proxmox: what answers
//    the question is taken for one where it refuses it 401, as a real one does, or says its version; anything else
//    ("bad_response") is sent nothing (outside review of 0.2.0, 3.2: proxmox::saysItIsAProxmox, which every driver of
//    a guest asks before every sign-in; the web sends the token with its first question, to whatever answers). A
//    Proxmox some of whose guests are on the wall already is known by what their cards are known by, each way in turn,
//    and by no certificate it shows on first use: one that shows another is sent nothing ("cert_mismatch") until one of
//    those cards was said yes to it (outside review of 0.2.0, 3.1: the web pins what it shows on first contact for
//    each listing). Then the virtual machines the token may see (ProxmoxDevice::listing), each with what the wall knows
//    of it: on the wall already (a card of this Proxmox with that number, and the number it gives for itself the
//    card's), or a new virtual machine under the number of a card's guest that is not that guest (it gives another
//    number for itself: rule 30). The one the address named, where it named one ("invalid" where the token sees none
//    of that number).
// 2. add(): one of them, after one sign-in of its own (OneSignIn of the family "proxmox", whose driver asks what
//    answers first, as every one does: which virtual machine has its number read and taken), put on the wall
//    (LocalWall::add) as a card named after the guest, known by what the listing found its Proxmox known by, and by the
//    number it gave. Another card of the wall at that place ("duplicate") or with that number (one guest by another
//    address of its Proxmox: "same_device") is not added; nor is one from a listing that knew its Proxmox by another
//    certificate than cards of it that came onto the wall since are known by ("cert_mismatch", nothing sent: review
//    cx3-1, L3).
//
// The token's secret goes to the drivers made here and to the wall, and nowhere else; it is in no sentence and no log.
// On the interface thread.
class ProxmoxAdding : public QObject {
    Q_OBJECT

public:
    struct Typed {
        QString address;
        QString tokenId;
        QString secret;
        QString group;
    };
    // What a Proxmox listed for a token, and what it is known by from now on.
    struct Offered {
        QUrl origin;
        bool knownByCertificate = false;
        QByteArray certificate; // its SHA-256, where it is known by one
        QList<proxmox::Listed> guests;
        int containers = 0;
        // By a guest's number: the card it is on the wall as; and the card under whose number another virtual machine
        // stands now (not that card's guest: remove that card to add this one).
        QHash<qint64, QString> onTheWall;
        QHash<qint64, QString> newUnder;
        // The guest the address named.
        std::optional<proxmox::Listed> named;
    };
    struct Added {
        QString id;
        QString name;
        QString model;
        QString version;
    };

    ProxmoxAdding(LocalWall& wall, DriverPaces paces, QObject* parent = nullptr);
    ~ProxmoxAdding() override;
    ProxmoxAdding(const ProxmoxAdding&) = delete;
    ProxmoxAdding& operator=(const ProxmoxAdding&) = delete;
    ProxmoxAdding(ProxmoxAdding&&) = delete;
    ProxmoxAdding& operator=(ProxmoxAdding&&) = delete;

    // One at a time: a second is refused while the first is out ("busy"). Answered once, a turn of the loop later at
    // the soonest; not answered where this is gone first.
    void list(const Typed& typed, std::function<void(base::Result<Offered>)> done);
    void add(const Typed& typed, const Offered& at, qint64 vmid, std::function<void(base::Result<Added>)> done);
    // What is out is not waited for any longer, and nothing of it is added.
    void giveUp();
    bool busy() const { return m_busy; }

private:
    struct Out;
    void later(std::function<void()> what);
    void ended();
    // What the cards of the Proxmox at `origin` on the wall know it by, each way once, in the wall's order.
    QList<DeviceAsker::Where> knownByTheWall(const QUrl& origin) const;
    // Its guests listed by a driver of its own on a line held to `known` (which asks what answers there first, before
    // its sign-in); where Windows does not trust what it shows and this is its first use (`firstUse`), listed once
    // more on a line held to that; where cards of it are on the wall, by each of their ways in turn.
    void listAt(const std::shared_ptr<Out>& out, const DeviceAsker::Where& known, bool firstUse);
    void judge(const std::shared_ptr<Out>& out, const std::shared_ptr<Offered>& offered, QList<qint64> toRead);

    LocalWall& m_wall;
    DriverPaces m_paces;
    bool m_busy = false;
    std::shared_ptr<Out> m_out;
};

} // namespace qkvm::core
