#include "own_wall.h"
#include "app_lock.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QStringList>
#include <QUrl>

#include <qkvm/core/server_client.h>
#include <qkvm/core/vnc_device.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// The one that was to tell these boxes to wait was ended before it had: nobody tells them now,
// and nothing came of what they asked.
void nothingCameOfItFor(const QList<QPointer<EditKvmView>>& boxes)
{
    for (const QPointer<EditKvmView>& box : boxes) {
        if (box != nullptr) {
            box->gaveUp();
        }
    }
}

// Why not, as a box shows it: the sentence it comes with, whoever said it.
QString sentenceOf(const base::Error& why)
{
    return QString::fromStdString(why.message);
}

// What a KVM is known by: its certificate's SHA-256, or nothing where it is known by none.
QByteArray knownBy(const core::OwnKvm& kvm)
{
    return kvm.knownByCertificate ? kvm.certificate : QByteArray();
}

// Spoken to in the clear: over plain HTTP, or a VNC server's vnc:// (the wall's one rule for it).
bool isPlain(const core::OwnKvm& kvm)
{
    return kvm.isInTheClear();
}

// Whether a saving left what a KVM is known by, or how it is spoken to, another than its box
// showed: that is to be seen where it was saved.
bool isToBeSeen(const core::KvmEditing::Changed& changed, const core::OwnKvm& shown)
{
    // (And that it answers over HTTPS now, where it was spoken to the plain way: a JetKVM.)
    return changed.certificate != knownBy(shown) || (changed.overPlainHttp && !isPlain(shown)) || changed.ontoHttps;
}

// Whether an address as it was typed is the one something was offered for: that host and port,
// over HTTPS where a certificate was shown there, and over plain HTTP where it is that way of
// speaking to it that was offered. A VNC server's as its family reads it (core::vnc::addressFrom,
// where an address without vnc:// or vncs:// is a vnc:// one): a certificate was shown at vncs://
// alone, so it is that scheme, that host and that port (outside review c9a-2, H1); and the way in
// the clear that was offered, at vnc:// alone, that very address (outside review c9a-3, M1).
bool isTypedFor(const QString& typed, const EditKvmView::Offer& offered, bool aVncServer)
{
    if (aVncServer) {
        const base::Result<QString> address = core::vnc::addressFrom(typed);
        if (!address || !core::vnc::namesOne(offered.at)) {
            return false;
        }
        const core::vnc::Place is = core::vnc::placeOf(address.value());
        const core::vnc::Place was = core::vnc::placeOf(offered.at);
        const bool there = is.host == was.host && is.port == was.port;
        return offered.plainHttp ? !is.tls && !was.tls && there && address.value() == offered.at
                                 : is.tls && was.tls && there;
    }
    const base::Result<QUrl> address = core::kvm::addressFrom(typed);
    return address && address.value().scheme() == (offered.plainHttp ? "http"_L1 : "https"_L1) &&
           address.value().authority(QUrl::FullyEncoded) == offered.at;
}

} // namespace

OwnWall::OwnWall(core::Settings& settings, core::SecretStore& secrets, QObject* parent)
    : OwnWall(settings, secrets, core::KvmAdding::Pace{}, parent)
{}

OwnWall::OwnWall(core::Settings& settings, core::SecretStore& secrets, const core::KvmAdding::Pace& pace,
                 QObject* parent)
    : QObject(parent), m_wall(settings, secrets, pace.drivers()), m_adding(m_wall, pace),
      m_proxmox(m_wall, pace.drivers()), m_pace(pace), m_editing(std::make_unique<core::KvmEditing>(m_wall, pace)),
      m_review(std::make_unique<core::KvmCertificateReview>(m_wall, pace.probe))
{
    connect(&m_wall, &core::LocalWall::changed, this, &OwnWall::followTheWallInTheBoxes);
    // The saving under way waits for an earlier sign-in to its KVM: its box says so.
    connect(m_editing.get(), &core::KvmEditing::signInWaits, this, [this] {
        for (const QPointer<EditKvmView>& box : std::as_const(m_editBoxes)) {
            if (box != nullptr && box.data() == m_changing.underWayFor) {
                box->signInWaits();
            }
        }
    });
}

void OwnWall::letGoOfEverything()
{
    m_adding.giveUp();
    m_proxmox.giveUp();
    if (m_editing != nullptr) {
        m_editing->giveUp();
    }
    m_wall.letGoOfEveryDriver();
}

void OwnWall::followTheWallInTheBoxes()
{
    m_editBoxes.removeIf([](const QPointer<EditKvmView>& box) { return box == nullptr; });
    // (Each of them as they are now: one that is told may close, and its window with it.)
    const QList<QPointer<EditKvmView>> boxes = m_editBoxes;
    for (const QPointer<EditKvmView>& box : boxes) {
        if (box == nullptr || box->machine().isEmpty()) {
            continue;
        }
        if (const std::optional<core::KvmEditing::Seen> now = seenNow(box->machine())) {
            const EditKvmView::Shown shown = shownOf(now->kvm);
            box->renamed(shown.called, shown.inASentence);
        } else {
            box->wentAway();
        }
    }
}

void OwnWall::serve(AddKvmView& view)
{
    m_addBoxes.append(&view);
    const QPointer<AddKvmView> box(&view);
    connect(&view, &AddKvmView::addAsked, this,
            [this, box](const QString& address, const QString& user, const QString& password, const QString& group,
                        bool keepBoth) {
                if (box == nullptr || !mayChange()) {
                    return;
                }
                core::KvmAdding::Typed typed;
                typed.address = address;
                typed.user = user;
                typed.password = password;
                typed.group = group;
                typed.keepBoth = keepBoth;
                // Whose KVM is being asked, where it is this box's: another box's stays that one's,
                // and this one is told to wait (the adding says so itself).
                const bool isThisOnes = !m_adding.isUnderWay();
                if (isThisOnes) {
                    m_asking = box.data();
                }
                box->began();
                m_adding.add(typed, [this, box, isThisOnes](const base::Result<core::KvmAdding::Added>& added) {
                    if (isThisOnes) {
                        m_asking = nullptr;
                    }
                    if (box == nullptr) {
                        return;
                    }
                    if (added) {
                        const QList<core::OwnKvm>& kvms = m_wall.kvms();
                        const bool aVncServer = std::ranges::any_of(kvms, [&added](const core::OwnKvm& one) {
                            return one.id == added.value().id && one.kind == u"vnc"_s;
                        });
                        box->took(
                            saidOf(added.value(), calledInASentence(added.value().id, added.value().name), aVncServer),
                            core::ServerClient::written(added.value().certificate));
                    } else if (added.error().code == "given_up") {
                        box->gaveUp();
                    } else {
                        // (Where it says by its number that it is a KVM of the wall, the person may keep both; where
                        // its plain address sends everybody on to HTTPS, that address is offered.)
                        const std::optional<QUrl> instead = core::kvm::httpsOffered(added.error());
                        box->refused(QString::fromStdString(added.error().message),
                                     core::LocalWall::saysItIsTheSame(added.error()),
                                     instead ? instead->toString(QUrl::FullyEncoded) : QString());
                    }
                });
            });
    // A Proxmox's guests (the page's fromProxmox): listed to tick; where the address names one, that one at once.
    connect(&view, &AddKvmView::listGuestsAsked, this,
            [this, box](const QString& address, const QString& tokenId, const QString& secret, const QString& group) {
                if (box == nullptr || !mayChange()) {
                    return;
                }
                // (A box that asks while another's is under way is told so: review c9b-1, L2.)
                if (m_proxmox.busy()) {
                    box->refused(tr("A Proxmox is being asked already. Wait for it to finish."));
                    return;
                }
                box->began();
                m_proxmoxFor = box.data();
                m_proxmoxBox = box;
                const core::ProxmoxAdding::Typed typed{address, tokenId, secret, group};
                m_proxmox.list(typed, [this, box, typed](const base::Result<core::ProxmoxAdding::Offered>& offered) {
                    // (Given up meanwhile, as a refusal ProxmoxAdding answers a turn of the loop later may be: told to
                    // nobody. Review c9b-2, L4.)
                    if (box == nullptr || m_proxmoxFor != box.data()) {
                        return;
                    }
                    if (!offered) {
                        m_proxmoxFor = nullptr;
                        box->refused(QString::fromStdString(offered.error().message));
                        return;
                    }
                    m_listed.insert(box.data(), offered.value());
                    if (const std::optional<core::proxmox::Listed> named = offered.value().named) {
                        // (A run of its own, which begins with its own lines, as one of the ticked ones does: review
                        // c9b-4, L7.)
                        box->beganARun();
                        addGuests(box, typed, {named->vmid});
                        return;
                    }
                    QVariantList guests;
                    for (const core::proxmox::Listed& guest : offered.value().guests) {
                        // (A VM made under the number of a guest that was deleted is not that card's guest: said so,
                        // and its box stays off while that card has the number. The page's words.)
                        const QString taken =
                            offered.value().onTheWall.contains(guest.vmid)
                                ? tr("on the wall as %1").arg(offered.value().onTheWall.value(guest.vmid))
                            : offered.value().newUnder.contains(guest.vmid)
                                ? tr("a new VM under the number of %1, whose guest it is not: remove that card to add "
                                     "this one")
                                      .arg(offered.value().newUnder.value(guest.vmid))
                                : QString();
                        guests.append(QVariantMap{{u"vmid"_s, guest.vmid},
                                                  {u"node"_s, guest.node},
                                                  {u"name"_s, guest.name},
                                                  {u"status"_s, guest.status},
                                                  {u"taken"_s, taken}});
                    }
                    m_proxmoxFor = nullptr;
                    box->listed(guests, offered.value().containers);
                });
            });
    connect(&view, &AddKvmView::addGuestsAsked, this,
            [this, box](const QString& address, const QString& tokenId, const QString& secret, const QString& group,
                        const QList<qint64>& vmids) {
                if (box == nullptr || !mayChange()) {
                    return;
                }
                if (m_proxmox.busy() || m_proxmoxFor != nullptr) {
                    box->refused(tr("A Proxmox is being asked already. Wait for it to finish."));
                    return;
                }
                // (Added from what this box listed, and only where that is the Proxmox typed now: never from what
                // another box listed, which may be known by another certificate. Outside review of 0.2.0, 3.7.)
                const base::Result<core::proxmox::Address> typedAt = core::proxmox::addressOf(address);
                const auto listed = m_listed.constFind(box.data());
                if (listed == m_listed.cend() || !typedAt || typedAt.value().origin != listed->origin) {
                    box->refused(tr("List the Proxmox's virtual machines again: what was typed is not what was "
                                    "listed."));
                    return;
                }
                box->beganARun();
                m_proxmoxFor = box.data();
                m_proxmoxBox = box;
                addGuests(box, core::ProxmoxAdding::Typed{address, tokenId, secret, group}, vmids);
            });
    connect(&view, &AddKvmView::giveUpAsked, this,
            [this, which = static_cast<const QObject*>(&view)] { giveUpFor(which); });
    // A window that is closed over its box: nobody is there to be told, and nothing is added
    // behind anybody's back. (And what it listed goes with it.)
    connect(&view, &QObject::destroyed, this, [this](const QObject* gone) {
        giveUpFor(gone);
        m_listed.remove(gone);
    });
}

void OwnWall::addGuests(const QPointer<AddKvmView>& box, const core::ProxmoxAdding::Typed& typed, QList<qint64> vmids)
{
    // (Given up meanwhile: the rest of the run is not added. Review c9b-1, L2.)
    const auto listed = box == nullptr ? m_listed.cend() : m_listed.constFind(box.data());
    if (box == nullptr || listed == m_listed.cend() || m_proxmoxFor != box.data()) {
        return;
    }
    // (Read here, and taken along by the answer's own copy: the box's listing may be listed anew or go meanwhile.)
    const core::ProxmoxAdding::Offered& at = *listed;
    if (vmids.isEmpty()) {
        m_proxmoxFor = nullptr;
        box->guestsDone();
        return;
    }
    const qint64 vmid = vmids.takeFirst();
    m_proxmox.add(
        typed, at, vmid, [this, box, typed, vmids, at](const base::Result<core::ProxmoxAdding::Added>& added) {
            // Told only where the run is still this box's: ProxmoxAdding answers a refusal a turn of the loop later,
            // and Cancel does not take that back (review c9b-2, L4). The rest of a run given up is not added either
            // (addGuests).
            if (box != nullptr && m_proxmoxFor == box.data()) {
                if (added) {
                    const QString model = QStringList{added.value().model, added.value().version}.join(u' ').trimmed();
                    const QString called = calledInASentence(added.value().id, added.value().name);
                    box->tookGuest(model.isEmpty() ? tr("%1 added").arg(called)
                                                   : tr("%1 added (%2)").arg(called, model),
                                   at.knownByCertificate ? core::ServerClient::written(at.certificate) : QString());
                } else {
                    box->refusedGuest(QString::fromStdString(added.error().message));
                }
            }
            addGuests(box, typed, vmids);
        });
}

void OwnWall::giveUpFor(const QObject* box)
{
    if (m_asking != nullptr && m_asking == box) {
        m_adding.giveUp();
    }
    // A Proxmox's listing, or a run of its guests' adds, for that box: given up there and then, and the box told,
    // where it is still there (review c9b-1, L2).
    if (m_proxmoxFor != nullptr && m_proxmoxFor == box) {
        m_proxmoxFor = nullptr;
        m_proxmox.giveUp();
        if (const QPointer<AddKvmView> told = std::exchange(m_proxmoxBox, nullptr); told != nullptr) {
            told->gaveUp();
        }
    }
}

bool OwnWall::Asked::begin(bool isUnderWay, EditKvmView& box)
{
    if (isUnderWay) {
        toldToWait.append(&box);
        return false;
    }
    underWayFor = &box;
    return true;
}

bool OwnWall::Asked::end(bool wasThisOnes, const QPointer<EditKvmView>& box)
{
    if (wasThisOnes) {
        // (What a box that was closed had asked was ended with the one that did it, and has no
        // answer; a box that gave it up and stays is answered, that it was given up. So this
        // is still that box's.)
        underWayFor = nullptr;
        return box != nullptr;
    }
    return toldToWait.removeOne(box) && box != nullptr;
}

bool OwnWall::Asked::letGo(const QObject* box)
{
    // (Boxes that are gone stand in the list as nothing, and go out of it here.)
    toldToWait.removeIf([box](const QPointer<EditKvmView>& one) { return one == nullptr || one.data() == box; });
    if (underWayFor == nullptr || underWayFor != box) {
        return false;
    }
    underWayFor = nullptr;
    return true;
}

void OwnWall::serve(EditKvmView& view)
{
    const QPointer<EditKvmView> box(&view);
    m_editBoxes.append(box);
    connect(&view, &EditKvmView::openAsked, this, [this, box](const QString& machine, bool withReview) {
        if (box != nullptr) {
            openFor(*box, machine, withReview);
        }
    });
    connect(&view, &EditKvmView::saveAsked, this,
            [this, box](const QString& machine, const QString& name, const QString& address, const QString& user,
                        const QString& password, const QString& group, const QByteArray& saidYesTo, bool keepBoth) {
                // Of the KVM this box was filled with, whatever the wish names, as a look at a
                // certificate and the yes to it are: a saving for another is nobody's, and fills
                // this box with nothing.
                if (box == nullptr || !mayChange() || !filledWith(box.data(), machine)) {
                    return;
                }
                core::KvmEditing::Typed typed;
                typed.name = name;
                typed.address = address;
                typed.user = user;
                typed.password = password;
                typed.group = group;
                typed.keepBoth = keepBoth;
                // Its screen watches, as the box had them when it was saved (c7).
                if (const std::optional<std::pair<int, int>> watches = box->typedWatches()) {
                    typed.noSignalMinutes = watches->first;
                    typed.unchangedMinutes = watches->second;
                }
                // A yes is one only to what this box was told to show, for this KVM and at the
                // address that is typed now; whatever certificate came with the wish. (Whatever
                // was offered is offered no longer: to this saving, or to none.)
                const std::optional<Offered> offered = takeOfferedTo(*box);
                const bool aVncServer = m_seen.value(box.data()).kvm.kind == u"vnc"_s;
                const bool saidYes = offered && !offered->at.isEmpty() && offered->machine == machine &&
                                     offered->certificate == saidYesTo && isTypedFor(address, *offered, aVncServer);
                if (saidYes && offered->plainHttp) {
                    typed.saidYesToPlainHttpAt = offered->at;
                } else if (saidYes) {
                    typed.saidYesTo = offered->certificate;
                    typed.saidYesOnWindowsWord = offered->onWindowsWord;
                }
                // Typed over the KVM as this box was filled with it. (A box that was filled
                // with nothing has seen no KVM: none is what it was, and nothing is saved.)
                const core::KvmEditing::Seen seen = m_seen.value(box.data());
                typed.seen = seen;
                // Whose change is under way, where it is this box's: another box's stays that
                // one's, and this one is told to wait (the editing says so itself).
                const bool isThisOnes = m_changing.begin(m_editing->isUnderWay(), *box);
                box->began(u"saving"_s);
                m_editing->change(
                    machine, std::move(typed),
                    [this, box, isThisOnes, machine, seen,
                     yes = saidYes ? offered : std::nullopt](const core::KvmEditing::Outcome& outcome) {
                        if (!m_changing.end(isThisOnes, box)) {
                            return;
                        }
                        const std::optional<core::KvmEditing::Seen> now = seenNow(machine);
                        if (outcome.result) {
                            // What it is known by now, or that it is spoken to in the clear now,
                            // is shown where it is another than this box showed: the box stays,
                            // on the KVM as it is. Otherwise it closes.
                            const core::KvmEditing::Changed& changed = outcome.result.value();
                            if (isToBeSeen(changed, seen.kvm) && now) {
                                m_seen.insert(box.data(), *now);
                                box->savedAndStays(shownOf(now->kvm), changed.ontoHttps);
                            } else {
                                box->saved(calledInASentence(machine, changed.name));
                            }
                        } else if (outcome.result.error().code == "certificate_differs") {
                            // What the box shows from here on is what a yes can be given to.
                            box->differing(outcome.remembered, outcome.presented, outcome.presentedAt,
                                           outcome.onWindowsWord);
                            // (Over HTTPS at that address, as it is typed: the yes goes with the
                            // address that was asked, which a JetKVM's two ways may have made
                            // another than the one typed. A VNC server's whole, vncs:// and all.)
                            box->retype(outcome.presentedAt);
                            keepAsOffered(*box, Offered{machine, outcome.presented, outcome.presentedAt,
                                                        outcome.onWindowsWord, false});
                        } else if (outcome.result.error().code == "plain_http_there") {
                            // So where it is plain HTTP at that address that a yes is given to.
                            box->givingUpHttps(outcome.remembered, outcome.presentedAt);
                            // (And the plain way at that address, as it is typed: a VNC server's
                            // whole, vnc:// and all, as it was handed over.)
                            box->retype(seen.kvm.kind == u"vnc"_s ? outcome.presentedAt
                                                                  : u"http://"_s + outcome.presentedAt);
                            keepAsOffered(*box, Offered{machine, QByteArray(), outcome.presentedAt, false, true});
                        } else if (outcome.result.error().code == "given_up") {
                            // (The box stays on what it showed: a yes can be given to that again.)
                            box->gaveUp();
                            if (yes) {
                                keepAsOffered(*box, *yes);
                            }
                        } else if (!fillAnewOver(*box, machine, outcome.result.error())) {
                            // (Its plain address sends everybody on to HTTPS: that address is offered.)
                            const std::optional<QUrl> instead = core::kvm::httpsOffered(outcome.result.error());
                            box->refused(sentenceOf(outcome.result.error()),
                                         core::LocalWall::saysItIsTheSame(outcome.result.error()),
                                         instead ? instead->toString(QUrl::FullyEncoded) : QString());
                        }
                    });
            });
    connect(&view, &EditKvmView::removeAsked, this, [this, box](const QString& machine) {
        if (box == nullptr || !mayChange()) {
            return;
        }
        const QString called = box->called();
        box->began(u"removing"_s);
        // The question said what becomes of a guest's boot order as the box was filled with it. Where that changed
        // since (started from an image, or the image taken out, in a KVM window meanwhile: the wall writes it and says
        // nothing of it), it is asked again over what stands now, and nothing is taken off (review cx6-1, L1).
        if (const std::optional<core::KvmEditing::Seen> now = seenNow(machine)) {
            const EditKvmView::Shown shown = shownOf(now->kvm);
            if (shown.bootOrderWas != box->bootOrderWas() || shown.bootOrderWritten != box->bootOrderWritten()) {
                m_seen.insert(box.data(), *now);
                box->refusedOver(shown,
                                 QCoreApplication::translate(
                                     "OwnWall", "The guest's boot order changed meanwhile: read the question again."));
                return;
            }
        }
        // (The wall says aloud that it has changed while it takes the KVM off, and whoever hears
        // that may close the box, or end its window.)
        const base::Result<void> taken = m_wall.remove(machine);
        if (box == nullptr) {
            return;
        }
        if (taken) {
            box->removed(called);
        } else {
            box->refused(sentenceOf(taken.error()));
        }
    });
    connect(&view, &EditKvmView::reviewAsked, this, [this, box](const QString& machine) {
        if (box == nullptr || !mayChange()) {
            return;
        }
        // Of the KVM this box was filled with, as it was filled with it, whatever the wish
        // names: what is read stands in the box beside that KVM. A wish for another is nobody's.
        const std::optional<core::KvmEditing::Seen> seen = filledWith(box.data(), machine);
        if (!seen) {
            return;
        }
        // (What was offered before this look is offered no longer.)
        m_offered.remove(box.data());
        const bool isThisOnes = m_looking.begin(m_review->isUnderWay(), *box);
        box->began(u"looking"_s);
        m_review->askCertificates(
            machine,
            [this, box, isThisOnes, machine](const base::Result<core::KvmCertificateReview::Shown>& shown) {
                if (!m_looking.end(isThisOnes, box)) {
                    return;
                }
                if (shown) {
                    showTheTwoTo(*box, machine, shown.value());
                } else if (shown.error().code == "given_up") {
                    box->gaveUp();
                } else if (!fillAnewOver(*box, machine, shown.error())) {
                    box->refused(sentenceOf(shown.error()));
                }
            },
            seen);
    });
    connect(&view, &EditKvmView::trustAsked, this, [this, box](const QString& machine, const QByteArray& presented) {
        if (box == nullptr || !mayChange()) {
            return;
        }
        // A yes is one only to what this box was told to show, for the KVM it was filled with,
        // at the address that one has; and only where the box says that this very thing is
        // what its yes was said to, having been looked at for its moment. Anything else is
        // none: nothing is asked for it, and what is offered stays offered.
        const std::optional<core::KvmEditing::Seen> seen = filledWith(box.data(), machine);
        const auto kept = m_offered.constFind(box.data());
        if (!seen || kept == m_offered.constEnd() || !kept->at.isEmpty() || kept->machine != machine ||
            kept->certificate != presented || box->saysYesTo() != *kept) {
            return;
        }
        const Offered yes = m_offered.take(box.data());
        const bool isThisOnes = m_looking.begin(m_review->isUnderWay(), *box);
        box->began(u"trusting"_s);
        // (Exactly what the box showed: the review reads the KVM once more, and remembers nothing
        // that is not this very one. And over the KVM as the box was filled with it: the yes
        // was said to the one beside the other.)
        m_review->trust(
            machine, yes.certificate,
            [this, box, isThisOnes, machine, yes](const core::KvmCertificateReview::Outcome& came) {
                if (!m_looking.end(isThisOnes, box)) {
                    return;
                }
                if (came.result) {
                    box->trusted(calledInASentence(machine, box->called()));
                } else if (came.result.error().code == "cert_mismatch" && !came.shownNow.presented.isEmpty()) {
                    // Nobody has seen what it shows by now: the box shows that one in the place
                    // of the one the yes was given to, and says so.
                    showTheTwoTo(*box, machine, came.shownNow);
                } else if (came.result.error().code == "given_up") {
                    // (The box stays on what it showed: a yes can be given again.)
                    box->gaveUp();
                    keepAsOffered(*box, yes);
                } else if (!fillAnewOver(*box, machine, came.result.error())) {
                    box->refused(sentenceOf(came.result.error()));
                    keepAsOffered(*box, yes);
                }
            },
            seen);
    });
    connect(&view, &EditKvmView::giveUpAsked, this, [this, box](bool closing) {
        if (box == nullptr) {
            return;
        }
        if (!closing) {
            giveUpChangingFor(box.data());
            return;
        }
        // (Before the one that did it is ended: it knows what it had done already, and would
        // tell nobody.)
        sayWhatWasDoneFor(*box);
        endChangingFor(box.data());
        m_offered.remove(box.data());
    });
    // A window that is closed over its box: nobody is there to be told, and nothing is changed
    // or trusted behind anybody's back.
    connect(&view, &QObject::destroyed, this, [this](const QObject* gone) {
        endChangingFor(gone);
        m_seen.remove(gone);
        m_offered.remove(gone);
    });
}

void OwnWall::showTheTwoTo(EditKvmView& box, const QString& machine, const core::KvmCertificateReview::Shown& shown)
{
    // What the box shows from here on is what a yes can be given to; unless it is the
    // certificate of another KVM of the wall, to which none is taken (the box offers none).
    // (That one is named in a sentence of the box's: as a sentence calls it.)
    const bool isAnothers = !shown.presentedIsOf.isEmpty() || !shown.presentedIsCalled.isEmpty();
    box.reviewed(shown.remembered, shown.presented,
                 isAnothers ? calledInASentence(shown.presentedIsOf, shown.presentedIsCalled) : QString(),
                 isAnothers && shown.presentedSaysSo);
    keepAsOffered(box, Offered{machine, shown.presented, QString(), false, false});
}

void OwnWall::keepAsOffered(const EditKvmView& box, const Offered& what)
{
    // What is offered is what is shown. A box that did not take what it was told (it had been
    // told to wait meanwhile, and waits for nothing) shows something else, or nothing: then
    // nothing is offered to it, and not what it shows either, which was offered before this.
    if (box.offers() == what) {
        m_offered.insert(&box, what);
    } else {
        m_offered.remove(&box);
    }
}

std::optional<OwnWall::Offered> OwnWall::takeOfferedTo(const EditKvmView& box)
{
    if (!m_offered.contains(&box)) {
        return std::nullopt;
    }
    Offered offered = m_offered.take(&box);
    // Only where it is the very thing the box says its yes was said to: what stood there when
    // its button was pressed, looked at for its moment. A saving that comes without that word
    // of the box's (its lines' own button; the signal, sent by whoever can) is one without a
    // yes, whatever certificate and address it names.
    return box.saysYesTo() == offered ? std::optional<Offered>(std::move(offered)) : std::nullopt;
}

std::optional<core::KvmEditing::Seen> OwnWall::filledWith(const QObject* box, const QString& machine) const
{
    const auto seen = m_seen.constFind(box);
    return seen != m_seen.constEnd() && seen->kvm.id == machine ? std::optional<core::KvmEditing::Seen>(*seen)
                                                                : std::nullopt;
}

bool OwnWall::fillAnewOver(EditKvmView& box, const QString& machine, const base::Error& why)
{
    const std::optional<core::KvmEditing::Seen> now = seenNow(machine);
    if (why.code != "conflict" || !now) {
        return false;
    }
    // Another window changed it: the box is filled anew with the KVM as it is, and what it
    // asks next is asked over that.
    m_seen.insert(&box, *now);
    box.refusedOver(shownOf(now->kvm), sentenceOf(why) + u' ' +
                                           QCoreApplication::translate(
                                               "OwnWall", "The fields were filled in again with what is saved now."));
    return true;
}

void OwnWall::sayWhatWasDoneFor(EditKvmView& box)
{
    if (m_changing.underWayFor == &box) {
        if (const std::optional<core::KvmEditing::Outcome> cameTo = m_editing->cameTo(); cameTo && cameTo->result) {
            // Saved, and the box not told yet: with what the box would have stayed open to
            // show, where it would have stayed.
            const core::KvmEditing::Changed& changed = cameTo->result.value();
            const bool known = isToBeSeen(changed, m_seen.value(&box).kvm);
            box.savedAsItClosed(calledInASentence(changed.id, changed.name), known,
                                known ? core::kvm::fingerprint(changed.certificate) : QString(),
                                known && changed.overPlainHttp);
        }
    }
    if (m_looking.underWayFor == &box) {
        if (const std::optional<core::KvmCertificateReview::Outcome> cameTo = m_review->cameTo();
            cameTo && cameTo->result) {
            box.trustedAsItClosed(calledInASentence(box.machine(), box.called()));
        }
    }
}

void OwnWall::giveUpChangingFor(const QObject* box)
{
    // The box stays, and waits to hear: the one that does it gives up, and says what came of
    // it. Where it was done in this very turn (the wall has taken it, and the box has not heard
    // yet), that is what the box is told, and not that it was given up. (A box that only waits
    // to be told to wait hears that.)
    if (box != nullptr && m_changing.underWayFor == box) {
        m_editing->giveUp();
    }
    if (box != nullptr && m_looking.underWayFor == box) {
        m_review->giveUp();
    }
}

void OwnWall::endChangingFor(const QObject* box)
{
    // Ended there and then, and not a turn of the loop later: what is given up does nothing
    // more, whatever a KVM answers in between. (What the wall had taken before that stays: the
    // wall has said so.)
    if (m_changing.letGo(box)) {
        m_editing = std::make_unique<core::KvmEditing>(m_wall, m_pace);
        nothingCameOfItFor(std::exchange(m_changing.toldToWait, {}));
    }
    if (m_looking.letGo(box)) {
        m_review = std::make_unique<core::KvmCertificateReview>(m_wall, m_pace.probe);
        nothingCameOfItFor(std::exchange(m_looking.toldToWait, {}));
    }
}

std::optional<core::KvmEditing::Seen> OwnWall::seenNow(const QString& machine) const
{
    const QList<core::OwnKvm>& kvms = m_wall.kvms();
    const auto known = std::ranges::find_if(kvms, [&machine](const core::OwnKvm& one) { return one.id == machine; });
    if (known == kvms.end()) {
        return std::nullopt;
    }
    return core::KvmEditing::Seen{*known, m_wall.secretWrites(machine)};
}

EditKvmView::Shown OwnWall::shownOf(const core::OwnKvm& kvm)
{
    const QUrl address(kvm.address);
    const QString scheme = address.scheme();
    EditKvmView::Shown shown;
    shown.id = kvm.id;
    shown.called = kvm.name.isEmpty() ? core::LocalWall::hostOf(kvm) : kvm.name;
    shown.inASentence = core::LocalWall::calledInASentence(kvm);
    shown.name = kvm.name;
    shown.https = scheme == "https"_L1;
    // (In the clear: a VNC server's vnc:// too, which its box says in its family's words. Outside review c9a-2, L4. By
    // the wall's one rule for it, as its card, its row and the bar say it: c9a-4, M1.)
    shown.overPlainHttp = kvm.isInTheClear();
    // As it is typed, and as what is typed is read back (kvm::addressFrom): an address without
    // "http://" before it is one that is spoken to over HTTPS. Its host stands as the address
    // has it, and not as it reads best: a name that is written "xn--" is typed so.
    shown.address = shown.https ? address.authority(QUrl::FullyEncoded) : kvm.address;
    // A Proxmox guest's with its port written, 443 too: what is typed for a guest is read as the Add box reads it
    // (proxmox::addressOf), where an address without a port is one at 8006 (review cx3-3, M1).
    if (kvm.kind == "proxmox"_L1 && shown.https && address.port() < 0) {
        shown.address += u":443"_s;
    }
    shown.user = kvm.user;
    shown.group = kvm.group;
    shown.family = kvm.kind;
    shown.certificate = kvm.knownByCertificate ? core::kvm::fingerprint(kvm.certificate) : QString();
    // (A server's controller hands out no picture of a KVM's here: nothing to watch. Nor a Proxmox guest's console,
    // which is said as a server is: the web watches a tile with a picture of its own alone.)
    shown.watchable = kvm.kind != "redfish"_L1 && kvm.kind != "proxmox"_L1;
    shown.noSignalMinutes = kvm.noSignalMinutes;
    shown.unchangedMinutes = kvm.unchangedMinutes;
    // (The boot order a start from an image replaced, which only the card remembers: its box says so before the card
    // goes, 6.4.)
    if (kvm.kind == "proxmox"_L1 && kvm.bootOrderWas) {
        shown.bootOrderWas = kvm.bootOrderWas->order;
        shown.bootOrderWritten = kvm.bootOrderWas->written;
    }
    return shown;
}

void OwnWall::openFor(EditKvmView& view, const QString& machine, bool withReview)
{
    if (!mayChange()) {
        return;
    }
    const std::optional<core::KvmEditing::Seen> now = seenNow(machine);
    if (!now || view.busy()) {
        return;
    }
    // What the box is filled with is what it saves over: kept here, where the screens do not
    // reach it. (And nothing is offered to it for a yes yet.)
    m_seen.insert(&view, *now);
    m_offered.remove(&view);
    const EditKvmView::Shown shown = shownOf(now->kvm);
    // (A vncs:// server's certificate is looked at and said yes to as a KVM's over HTTPS: outside review c9a-2, M1.)
    view.show(shown, withReview && EditKvmView::showsACertificate(shown));
}

QString OwnWall::saidOfSettingsThatCouldNotBeRead(core::Settings& settings)
{
    if (!settings.wasUnreadable()) {
        return {};
    }
    QString said =
        QCoreApplication::translate("OwnWall", "What this program remembered could not be read, so it starts as new.");
    if (!settings.putAsideWhatCouldNotBeRead()) {
        said += u' ' + QCoreApplication::translate("OwnWall", "The file could not be put aside either, so nothing is "
                                                              "remembered from here on: something else holds it.");
    } else if (!settings.putAside().isEmpty()) {
        // (Empty: somebody moved it away meanwhile, and there is nothing to say of where it is.)
        said += u' ' + QCoreApplication::translate("OwnWall", "The file is kept, beside the new one, as %1.")
                           .arg(QFileInfo(settings.putAside()).fileName());
    }
    return said;
}

QString OwnWall::calledInASentence(const QString& machine, const QString& otherwise) const
{
    const QList<core::OwnKvm>& kvms = m_wall.kvms();
    const auto known = std::ranges::find_if(kvms, [&machine](const core::OwnKvm& one) { return one.id == machine; });
    if (known != kvms.end()) {
        return core::LocalWall::calledInASentence(*known);
    }
    // Not on the wall (any more): what its card called it, set off by the same function.
    const QString setOff = core::exchange::nameSetOff(otherwise);
    return setOff.isEmpty() ? QCoreApplication::translate("OwnWall", "this KVM") : setOff;
}

void OwnWall::serve(OwnFilesView& view)
{
    if (m_lock != nullptr) {
        view.useAppLock(*m_lock);
    }
    // One of its own for each view, kept here for as long as the view is there: what it laid
    // out goes with it, each password written over.
    std::unique_ptr<core::WallExchange>& exchange = m_exchanges[&view];
    exchange = std::make_unique<core::WallExchange>(m_wall, m_sealing);
    view.use(exchange.get());
    connect(&view, &QObject::destroyed, this, [this](const QObject* gone) { m_exchanges.erase(gone); });
}

bool OwnWall::mayChange() const
{
    return m_lock == nullptr || !m_lock->restricted();
}

void OwnWall::useAppLock(AppLock& lock)
{
    m_lock = &lock;
    connect(
        &lock, &AppLock::changed, this,
        [this] {
            if (!mayChange()) {
                // A typing handed to a driver goes no further: whoever had it typed may have it no longer (6.18).
                m_wall.stopEveryTyping();
                m_adding.giveUp();
                for (const QPointer<AddKvmView>& box : m_addBoxes) {
                    if (box != nullptr) {
                        box->close();
                    }
                }
                for (const QPointer<EditKvmView>& box : m_editBoxes) {
                    if (box != nullptr) {
                        box->close();
                    }
                }
            }
        },
        Qt::DirectConnection);
}

QString OwnWall::saidOf(const core::KvmAdding::Added& added, const QString& called, bool aVncServer)
{
    // "Build server" added, and in brackets what else is to be said, with commas between (the page has a long
    // dash and a dot there; the owner's rule writes neither, 2026-11-13). (What it is called stands as a
    // sentence calls it, and not as it was typed: calledInASentence.)
    QStringList more;
    if (!added.model.isEmpty()) {
        more.append(added.model);
    }
    // (A VNC server's vnc:// is no HTTP: said in its family's words, as its row's "Unencrypted (vnc)".)
    if (added.overPlainHttp) {
        more.append(aVncServer ? u"unencrypted"_s : u"over plain HTTP"_s);
    }
    QString said = u"%1 added"_s.arg(called);
    if (!more.isEmpty()) {
        said += u" ("_s + more.join(u", "_s) + u')';
    }
    return said;
}

} // namespace qkvm::ui
