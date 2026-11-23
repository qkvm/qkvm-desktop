#include <qkvm/core/proxmox_adding.h>

#include <algorithm>
#include <utility>

#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QTimer>

#include <qkvm/core/kvm_adding.h>
#include <qkvm/core/proxmox_device.h>
#include <qkvm/core/tls.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

base::Error busyAdding()
{
    return {"busy", "A Proxmox is being asked already. Wait for it to finish."};
}

// Where guests of the Proxmox are on the wall, and it shows a certificate none of their cards is known by. (A yes on a
// card is no word on what shows it: what answers there is asked what it is before the token goes to it.)
base::Error shownOtherwise()
{
    return {"cert_mismatch",
            "This Proxmox shows another certificate than its guests on the wall are known by: nothing was sent to it. "
            "Where that certificate is the Proxmox's own, look at it on one of their cards and say yes to it there "
            "(what shows it is still asked whether it is a Proxmox before the token goes); then list its guests "
            "again."};
}

} // namespace

// What is out: who waits, and the lines and drivers made for it, which go with it.
struct ProxmoxAdding::Out {
    std::function<void(base::Result<Offered>)> listed;
    std::function<void(base::Result<Added>)> added;
    Typed typed;
    std::optional<proxmox::Address> address;
    // What the cards of this Proxmox on the wall know it by, each way once, that is not asked by yet; and whether it is
    // known by them (and by nothing it shows on first use).
    QList<DeviceAsker::Where> ways;
    bool ofTheWall = false;
    std::unique_ptr<ProxmoxDevice> lister;
    std::unique_ptr<OneSignIn> signIn;
    bool over = false;
};

ProxmoxAdding::ProxmoxAdding(LocalWall& wall, DriverPaces paces, QObject* parent)
    : QObject(parent), m_wall(wall), m_paces(std::move(paces))
{}

ProxmoxAdding::~ProxmoxAdding()
{
    giveUp();
}

void ProxmoxAdding::later(std::function<void()> what)
{
    QTimer::singleShot(0, this, std::move(what));
}

void ProxmoxAdding::ended()
{
    m_busy = false;
    if (m_out) {
        m_out->over = true;
        // (Let go of a turn of the loop later: one of them may be in the middle of handing its answer over.)
        if (m_out->lister) {
            m_out->lister.release()->deleteLater();
        }
        if (m_out->signIn) {
            m_out->signIn.release()->deleteLater();
        }
    }
    m_out.reset();
}

void ProxmoxAdding::giveUp()
{
    ended();
}

void ProxmoxAdding::list(const Typed& typed, std::function<void(base::Result<Offered>)> done)
{
    if (m_busy) {
        later([done = std::move(done)] { done(busyAdding()); });
        return;
    }
    const base::Result<proxmox::Address> address = proxmox::addressOf(typed.address);
    if (!address) {
        later([done = std::move(done), why = address.error()] { done(why); });
        return;
    }
    // (Its ID held to what every user name of the wall is held to, in the same words: review c9b-1, M5.)
    if (const base::Result<QString> id = kvm::userFrom(typed.tokenId); !id) {
        later([done = std::move(done), why = id.error()] { done(why); });
        return;
    }
    if (!proxmox::isTokenId(typed.tokenId) || !proxmox::isTokenSecret(typed.secret)) {
        // (Nothing is sent: both go into a header, and what is no token goes nowhere.)
        later([done = std::move(done)] { done(proxmox::notAToken()); });
        return;
    }
    if (const base::Result<QString> group = kvm::groupFrom(typed.group); !group) {
        later([done = std::move(done), why = group.error()] { done(why); });
        return;
    }
    m_busy = true;
    auto out = std::make_shared<Out>();
    out->listed = std::move(done);
    out->typed = typed;
    out->address = address.value();
    m_out = out;
    // Its certificate first, before the token goes anywhere, and what answers asked whether it is a Proxmox: by the
    // lister's own sign-in, which asks the unsigned question of its API first on its own line (ProxmoxDevice). Where
    // guests of this Proxmox are on the wall, on lines held to what their cards are known by, each way in turn, and
    // never to a certificate it shows now on first use: its certificate is every guest's of it, and one that shows
    // another than they are known by is sent nothing until one of them was said yes to it (outside review of 0.2.0,
    // 3.1; SPEC.md section 8, "a different certificate later is not taken"). Else on a line that Windows' trust
    // decides, and a Proxmox Windows does not trust for its name is known by what it shows.
    out->ways = knownByTheWall(address.value().origin);
    out->ofTheWall = !out->ways.isEmpty();
    if (out->ofTheWall) {
        listAt(out, out->ways.takeFirst(), false);
        return;
    }
    listAt(out, DeviceAsker::Where{address.value().origin, false, {}}, true);
}

QList<DeviceAsker::Where> ProxmoxAdding::knownByTheWall(const QUrl& origin) const
{
    // (A card of this Proxmox: one whose place is its host and port, whichever guest it is.)
    OwnKvm here;
    here.kind = u"proxmox"_s;
    here.address = origin.toString(QUrl::FullyEncoded);
    const LocalWall::Place at = LocalWall::placeOf(here);
    QList<DeviceAsker::Where> ways;
    for (const OwnKvm& card : m_wall.kvms()) {
        if (card.kind != "proxmox"_L1) {
            continue;
        }
        LocalWall::Place there = LocalWall::placeOf(card);
        there.guest = 0;
        if (!LocalWall::atOnePlace(there, at)) {
            continue;
        }
        const QByteArray certificate = card.knownByCertificate ? card.certificate : QByteArray();
        if (std::ranges::none_of(ways, [&card, &certificate](const DeviceAsker::Where& way) {
                return way.knownByCertificate == card.knownByCertificate && way.certificate == certificate;
            })) {
            ways.append(DeviceAsker::Where{origin, card.knownByCertificate, certificate});
        }
    }
    return ways;
}

void ProxmoxAdding::listAt(const std::shared_ptr<Out>& out, const DeviceAsker::Where& known, bool firstUse)
{
    // (The lister before it may be in the middle of handing its answer over: let go of a turn of the loop later.)
    if (out->lister) {
        out->lister.release()->deleteLater();
    }
    ProxmoxDevice::Where where{known.address,     {}, 0,           out->typed.tokenId, known.knownByCertificate,
                               known.certificate, {}, std::nullopt};
    // A person typed this token just now: tried as for the first time, whatever was said to it before.
    ProxmoxDevice::tryAfresh(where, out->typed.secret, m_paces.proxmox);
    QString secret = out->typed.secret;
    out->lister = std::make_unique<ProxmoxDevice>(where, secret, m_paces.proxmox);
    ProxmoxDevice* lister = out->lister.get();
    lister->listing([this, out, known, firstUse, lister](const base::Result<proxmox::Listing>& listing) {
        if (out->over) {
            return;
        }
        if (!listing) {
            const std::string& code = listing.error().code;
            if (out->ofTheWall && (code == "cert_mismatch" || code == "cert_invalid")) {
                // Not what this way knows it by: the next way its cards know it by, and none on first use.
                if (!out->ways.isEmpty()) {
                    listAt(out, out->ways.takeFirst(), false);
                    return;
                }
                const auto listed = std::move(out->listed);
                ended();
                listed(shownOtherwise());
                return;
            }
            const QByteArray shown = lister->shownCertificate();
            if (firstUse && code == "cert_invalid" && shown.size() == 32) {
                // Known by what it shows, on first use: asked what it is on a line held to that certificate, before
                // the token goes there. (The line it showed it on was refused before a byte of the question went.)
                listAt(out, DeviceAsker::Where{known.address, true, shown}, false);
                return;
            }
            const auto listed = std::move(out->listed);
            ended();
            listed(listing.error());
            return;
        }
        auto offered = std::make_shared<Offered>();
        offered->origin = known.address;
        offered->knownByCertificate = known.knownByCertificate;
        offered->certificate = known.certificate;
        offered->guests = listing.value().guests;
        offered->containers = listing.value().containers;
        if (const std::optional<qint64> named = out->address->vmid) {
            for (const proxmox::Listed& guest : std::as_const(offered->guests)) {
                if (guest.vmid == *named) {
                    offered->named = guest;
                }
            }
            if (!offered->named) {
                const auto listed = std::move(out->listed);
                ended();
                listed(base::Error{
                    "invalid", u"The token sees no virtual machine %1 at that Proxmox"_s.arg(*named).toStdString()});
                return;
            }
        }
        // The guests whose number a card of this Proxmox has: read for the number each gives for itself, to tell the
        // card's guest from a new virtual machine under its number (rule 30, the web's numbers_of).
        QList<qint64> toRead;
        OwnKvm here;
        here.kind = u"proxmox"_s;
        here.address = known.address.toString(QUrl::FullyEncoded);
        for (const proxmox::Listed& guest : std::as_const(offered->guests)) {
            here.vmid = guest.vmid;
            if (std::ranges::any_of(m_wall.kvms(), [&here](const OwnKvm& card) {
                    return card.kind == "proxmox"_L1 && LocalWall::atOnePlace(card, here);
                })) {
                toRead.append(guest.vmid);
            }
        }
        judge(out, offered, toRead);
    });
}

void ProxmoxAdding::judge(const std::shared_ptr<Out>& out, const std::shared_ptr<Offered>& offered,
                          QList<qint64> toRead)
{
    if (toRead.isEmpty()) {
        const auto listed = std::move(out->listed);
        ended();
        listed(*offered);
        return;
    }
    const qint64 vmid = toRead.takeFirst();
    QString node;
    for (const proxmox::Listed& guest : std::as_const(offered->guests)) {
        if (guest.vmid == vmid) {
            node = guest.node;
        }
    }
    out->lister->numberOfGuest(node, vmid, [this, out, offered, toRead, vmid](const base::Result<QString>& number) {
        if (out->over) {
            return;
        }
        OwnKvm here;
        here.kind = u"proxmox"_s;
        here.address = offered->origin.toString(QUrl::FullyEncoded);
        here.vmid = vmid;
        for (const OwnKvm& card : m_wall.kvms()) {
            if (card.kind != "proxmox"_L1 || !LocalWall::atOnePlace(card, here)) {
                continue;
            }
            const QString called = LocalWall::calledInASentence(card);
            // (Not read: what the list says of it goes by its number, as before.)
            if (!number || card.machine.isEmpty() || number.value() == card.machine) {
                offered->onTheWall.insert(vmid, called);
            } else {
                offered->newUnder.insert(vmid, called);
            }
        }
        judge(out, offered, toRead);
    });
}

void ProxmoxAdding::add(const Typed& typed, const Offered& at, qint64 vmid,
                        std::function<void(base::Result<Added>)> done)
{
    if (m_busy) {
        later([done = std::move(done)] { done(busyAdding()); });
        return;
    }
    std::optional<proxmox::Listed> guest;
    for (const proxmox::Listed& one : at.guests) {
        if (one.vmid == vmid) {
            guest = one;
        }
    }
    const base::Result<QString> group = kvm::groupFrom(typed.group);
    if (!guest || !group || !proxmox::isTokenId(typed.tokenId) || !proxmox::isTokenSecret(typed.secret)) {
        const base::Error why = !guest   ? base::Error{"invalid", "That is not a guest the Proxmox listed"}
                                : !group ? group.error()
                                         : proxmox::notAToken();
        // (What tidy follows to is the words of an error out of memory: nothing of ours throws, DESIGN.md section 7.)
        // NOLINTNEXTLINE(bugprone-exception-escape)
        later([done = std::move(done), why] { done(why); });
        return;
    }
    // One that is on the wall already is asked nothing (the wall's own words: LocalWall::add).
    OwnKvm here;
    here.kind = u"proxmox"_s;
    here.address = at.origin.toString(QUrl::FullyEncoded);
    here.vmid = guest->vmid;
    if (std::ranges::any_of(m_wall.kvms(), [&here](const OwnKvm& card) { return LocalWall::atOnePlace(card, here); })) {
        later([done = std::move(done)] { done(base::Error{"duplicate", "This device is already on the wall"}); });
        return;
    }
    // Where guests of this Proxmox came onto the wall since the listing, it is held to what their cards are known by,
    // as a listing is (review cx3-1, L3: outside review of 0.2.0, 3.1 at the add): a listing that knew it by another
    // certificate, taken on first use then, adds nothing, and nothing is sent; its guests are listed again first.
    const QByteArray listedBy = at.knownByCertificate ? at.certificate : QByteArray();
    if (const QList<DeviceAsker::Where> ways = knownByTheWall(at.origin);
        !ways.isEmpty() && std::ranges::none_of(ways, [&at, &listedBy](const DeviceAsker::Where& way) {
            return way.knownByCertificate == at.knownByCertificate && way.certificate == listedBy;
        })) {
        later([done = std::move(done)] { done(shownOtherwise()); });
        return;
    }
    m_busy = true;
    auto out = std::make_shared<Out>();
    out->added = std::move(done);
    out->typed = typed;
    m_out = out;
    OneSignIn::Where where;
    where.address = at.origin;
    where.user = typed.tokenId;
    where.knownByCertificate = at.knownByCertificate;
    where.certificate = at.certificate;
    where.node = guest->node;
    where.vmid = guest->vmid;
    base::Result<std::unique_ptr<OneSignIn>> made = OneSignIn::of(u"proxmox"_s, where, typed.secret, m_paces, {});
    if (!made) {
        const auto added = std::move(out->added);
        ended();
        later([added, why = made.error()] { added(why); });
        return;
    }
    out->signIn = std::move(made).value();
    out->signIn->identify(
        [this, out, where, guest = *guest, group = group.value()](const base::Result<OneSignIn::Said>& said) {
            if (out->over) {
                return;
            }
            const auto added = std::move(out->added);
            if (!said) {
                ended();
                added(said.error());
                return;
            }
            OwnKvm kvm;
            kvm.kind = u"proxmox"_s;
            kvm.address = where.address.toString(QUrl::FullyEncoded);
            kvm.user = where.user;
            kvm.group = group;
            kvm.knownByCertificate = where.knownByCertificate;
            kvm.certificate = where.certificate;
            kvm.node = where.node;
            kvm.vmid = where.vmid;
            kvm.machine = said.value().machine;
            // Named after the guest, as the web names its tile.
            const base::Result<QString> name =
                kvm::nameFrom(said.value().hostname.isEmpty() ? guest.name : said.value().hostname);
            kvm.name = name ? name.value() : QString();
            // One guest by another address of its Proxmox: the number it gives for itself says so.
            if (const std::optional<OwnKvm> twin = LocalWall::twinOf(m_wall.kvms(), kvm)) {
                ended();
                added(LocalWall::sameDevice(*twin));
                return;
            }
            QString secret = out->typed.secret;
            const WipedOnLeaving overTheSecret(secret);
            const base::Result<QString> id = m_wall.add(kvm, secret);
            ended();
            if (!id) {
                added(id.error());
                return;
            }
            added(Added{id.value(), kvm.name, said.value().model, said.value().version});
        });
}

} // namespace qkvm::core
