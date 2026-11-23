#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QBuffer>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/media_place.h>
#include <qkvm/core/proxmox_adding.h>
#include <qkvm/core/proxmox_device.h>
#include <qkvm/core/settings.h>

#include "memory_secrets.h"
#include "proxmox_by_hand.h"
#include "proxmox_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
namespace core = qkvm::core;
using namespace qkvm::core::testing;
using core::LocalWall;
using core::MediaDone;
using core::MediaList;
using core::MediaMount;
using core::ProxmoxAdding;
using core::ProxmoxDevice;
using Guest = ProxmoxByHand::Guest;
using Storage = ProxmoxByHand::Storage;

namespace {

const QString debian = u"local:iso/debian-13.iso"_s;
const QString alpine = u"local:iso/alpine-3.21.iso"_s;
const QString firstOrder = u"order=scsi0;ide2;net0"_s;

QString numberOf(const Guest& guest)
{
    return u"proxmox:"_s +
           QString::fromLatin1(
               QCryptographicHash::hash(guest.uuid.toLower().toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

ProxmoxDevice::Pace fast()
{
    ProxmoxDevice::Pace pace = quickProxmox();
    pace.places = std::make_shared<ProxmoxDevice::Places>();
    pace.confirmEveryMs = 50;
    pace.followUpMs.clear();
    return pace;
}

// The stand-in's guest `vmid`, known by the number it gives for itself.
ProxmoxDevice::Where known(const ProxmoxByHand& pve, qint64 vmid = 100)
{
    ProxmoxDevice::Where where = guestOf(pve, vmid);
    where.machine = numberOf(pve.guests.value(vmid));
    return where;
}

// ...as a driver made anew has it where the boot order the driver before it replaced is kept with the guest (the wall
// hands it over so, also after a restart of the program).
ProxmoxDevice::Where keptWith(const ProxmoxByHand& pve, const std::optional<core::BootOrderWas>& was, qint64 vmid = 100)
{
    ProxmoxDevice::Where where = known(pve, vmid);
    where.bootOrderWas = was;
    return where;
}

// What happens on Proxmox right after the guest's start command was taken (none of its other commands).
void afterTheStart(ProxmoxByHand& pve, std::function<void()> then)
{
    pve.afterCommand = [&pve, then = std::move(then)] {
        if (!pve.tasks.isEmpty() && pve.tasks.last().type == u"qmstart"_s) {
            then();
        }
    };
}

// ...and after its stop command.
void afterTheStop(ProxmoxByHand& pve, std::function<void()> then)
{
    pve.afterCommand = [&pve, then = std::move(then)] {
        if (!pve.tasks.isEmpty() && pve.tasks.last().type == u"qmstop"_s) {
            then();
        }
    };
}

// The log line that says a guest's boot order was not put back after a start that did not come about: by its name,
// its number and its node (review c9b-3, M1).
void logSaysTheOrderOfWebWasNotPutBack()
{
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"\\AqKVM: the boot order of web \\(VM 100 on pve1\\) could "
                                                          u"not be put back after a start from an image that did not "
                                                          u"come about: "_s));
}

// The web's two guests: "web" (100) running, "db" (101) stopped.
void twoGuests(ProxmoxByHand& pve)
{
    Guest db;
    db.name = u"db"_s;
    db.status = u"stopped"_s;
    pve.guests.insert(101, db);
}

base::Result<MediaList> mediaOf(ProxmoxDevice& device)
{
    return answerOf<MediaList>(
        [&device](std::function<void(base::Result<MediaList>)> done) { device.askMedia(std::move(done)); });
}

// A mount picked from a list of the guest's images on the node it is on (here: filled in by mounted() of the driver it
// is asked of, or by onThe() of a guest of the stand-in), as a box's mount names the list it was picked from.
const QString pickedHere = u"(picked from a list of where the guest is)"_s;

MediaMount mountOf(const QString& image, const QString& machine = u"web"_s)
{
    MediaMount mount;
    mount.image = image;
    mount.machine = machine;
    mount.listed = pickedHere;
    return mount;
}

base::Result<MediaDone> mounted(ProxmoxDevice& device, MediaMount mount)
{
    if (mount.listed == pickedHere) {
        mount.listed = ProxmoxDevice::listedMark(device.where(), device.where().node);
    }
    return answerOf<MediaDone>([&device, &mount](std::function<void(base::Result<MediaDone>)> done) {
        device.mountMedia(mount, std::move(done));
    });
}

// ...and of the stand-in's guest `vmid`, through the wall.
MediaMount onThe(const ProxmoxByHand& pve, MediaMount mount, qint64 vmid = 100)
{
    mount.listed = ProxmoxDevice::listedMark(guestOf(pve, vmid), pve.guests.value(vmid).node);
    return mount;
}

base::Result<MediaDone> unmounted(ProxmoxDevice& device, const QString& image)
{
    return answerOf<MediaDone>([&device, &image](std::function<void(base::Result<MediaDone>)> done) {
        device.unmountMedia(image, std::move(done));
    });
}

// What is in a drive, as it is said (nothing in it: none of it).
core::MountedMedia inThe(const std::optional<core::MountedMedia>& drive)
{
    return drive.value_or(core::MountedMedia{});
}

QString saidOf(const base::Result<MediaDone>& what)
{
    return what ? what.value().note : QString::fromStdString(what.error().message);
}

// The images of a list, as (id, name, where, size).
QStringList imagesIn(const MediaList& list)
{
    QStringList images;
    for (const core::MediaImage& image : list.images) {
        images.append(u"%1 %2 %3 %4"_s.arg(image.id, image.name, image.where,
                                           image.size ? QString::number(*image.size) : u"-"_s));
    }
    return images;
}

// Started from it: once, or until it is unmounted (and restarted now, or not).
MediaMount startedFrom(const QString& image, const QString& how, const QString& machine = u"web"_s,
                       bool restart = false)
{
    MediaMount mount = mountOf(image, machine);
    mount.boot = how;
    mount.restart = restart;
    return mount;
}

// The guest's power commands, as Proxmox wrote their tasks (the web's power_tasks).
QStringList powerTasks(const ProxmoxByHand& pve)
{
    QStringList kinds;
    for (const ProxmoxByHand::Task& task : pve.tasks) {
        if (task.type.startsWith(u"qm"_s) && task.type != u"qmconfig"_s) {
            kinds.append(task.type);
        }
    }
    return kinds;
}

// Its boot order for its next start: what is pending, else its settings'.
QString bootOf(const Guest& guest)
{
    return guest.pending.value(u"boot"_s, guest.config.value(u"boot"_s));
}

base::Result<core::Done> pressedOn(ProxmoxDevice& device, const QString& kind)
{
    return answerOf<core::Done>([&device, &kind](std::function<void(base::Result<core::Done>)> done) {
        device.act(core::Action::plain(kind), std::move(done));
    });
}

// Requests that had Proxmox fetch an address, or carried an image to it: there are to be none.
QStringList fetchedOrSent(const ProxmoxByHand& pve)
{
    QStringList found;
    for (const QString& one : pve.requests) {
        if (one.contains(u"download-url"_s) || one.contains(u"/upload"_s) || one.contains(u"oci-registry-pull"_s)) {
            found.append(one);
        }
    }
    return found;
}

QStringList changesSent(const ProxmoxByHand& pve)
{
    QStringList sent;
    for (const QString& one : pve.requests) {
        if (one.endsWith(u"/config"_s) && !one.startsWith(u"GET "_s)) {
            sent.append(one);
        }
    }
    return sent;
}

// --- through the wall ---

core::DriverPaces quickPaces()
{
    core::DriverPaces paces;
    paces.proxmox = fast();
    return paces;
}

template <typename Answer>
base::Result<Answer> ofTheWall(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
{
    return answerOf<Answer>(ask);
}

// A guest of the stand-in added to the wall, as the box that adds one does: its id.
QString addedTo(LocalWall& wall, const ProxmoxByHand& pve, qint64 vmid)
{
    ProxmoxAdding adding(wall, quickPaces());
    const ProxmoxAdding::Typed typed{pve.address().toString(), pve.tokenId, pve.secret, u"lab"_s};
    const auto offered = ofTheWall<ProxmoxAdding::Offered>(
        [&](std::function<void(base::Result<ProxmoxAdding::Offered>)> done) { adding.list(typed, std::move(done)); });
    if (!offered) {
        return {};
    }
    const auto made =
        ofTheWall<ProxmoxAdding::Added>([&](std::function<void(base::Result<ProxmoxAdding::Added>)> done) {
            adding.add(typed, offered.value(), vmid, std::move(done));
        });
    return made ? made.value().id : QString();
}

// What a Proxmox says of a digest that is not its settings file's, as a test's rows: an older one's words, and a
// Proxmox VE 9's (review c9b-2, L5).
void digestWords()
{
    QTest::addColumn<QString>("words");
    QTest::newRow("older") << u"checksum mismatch (file change by other user?)"_s;
    QTest::newRow("current") << u"detected modified configuration - file changed by other user? Try again."_s;
}

core::Status statusIn(LocalWall& wall, const QString& id)
{
    return ofTheWall<QHash<QString, core::Status>>(
               [&wall](std::function<void(base::Result<QHash<QString, core::Status>>)> done) {
                   wall.askStatus(std::move(done));
               })
        .value()
        .value(id);
}

} // namespace

// A guest's virtual media (c9b, wave 2, step 7), as the web's 0.3.0 has them (proxmox.py, "virtual media"; its tests
// ../app/tests/test_virtual_media_on_a_proxmox_guest.py of web 0.3.1): an ISO image already on one of Proxmox's
// storages put in the guest's CD-ROM drive, read-only, and taken out; the images listed from the storages of its node
// that the token may see, taken by kind; which virtual machine has the guest's number asked right before each change,
// and each change made on the settings read for it (their digest), so that one made on Proxmox meanwhile is not
// written over; never Proxmox's download-url, and no upload. And through this PC's own wall, as its server does: the
// guest named as the question named it, one thing at a time.
class TestProxmoxMedia : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { const ProxmoxDevice first(ProxmoxDevice::Where{}, QString(), quickProxmox()); }

    // The web's test_an_admin_mounts_an_image_and_the_tile_and_the_activity_log_say_so, its list: the images of the
    // node's storages, by name; what a guest's drive can do; nothing mounted; and the card that can have them.
    void aGuestsImagesAreListedFromItsNodesStorages()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaList> listed = mediaOf(device);
        QVERIFY2(listed, listed ? "" : listed.error().message.c_str());
        QCOMPARE(imagesIn(listed.value()), (QStringList{alpine + u" alpine-3.21.iso local 62000000"_s,
                                                        debian + u" debian-13.iso local 700000000"_s}));
        const core::MediaOffer& offers = listed.value().offers;
        QCOMPARE(offers.drives, QStringList{u"cd"_s});
        // (Started from it once, or until it is unmounted, and restarted for that: the web's MEDIA. Review c9b-2, M1.)
        QCOMPARE(offers.boot, (QStringList{u"once"_s, u"until_unmounted"_s}));
        QVERIFY(offers.restart);
        QVERIFY(!offers.upload);
        QCOMPARE(offers.where, u"proxmox"_s);
        QVERIFY(offers.writable.isEmpty());
        QVERIFY(!offers.canDelete);
        QVERIFY(!offers.dropsInput);
        QVERIFY(!listed.value().mounted);
        QVERIFY(listed.value().unseen.isEmpty());
        QCOMPARE(listed.value().leftOut, 0);
        QVERIFY(!listed.value().free && !listed.value().size && !listed.value().uploading);
        // Its settings read first (which virtual machine has the number), then the storages of its node.
        const qsizetype config = pve.requests.indexOf(u"GET /nodes/pve1/qemu/100/config"_s);
        QVERIFY(config >= 0);
        QVERIFY(pve.requests.indexOf(u"GET /nodes/pve1/storage"_s) > config);
        QCOMPARE(pve.asked(u"GET /nodes/pve1/storage/local/content"_s), 1);
        // The card can have them, and nothing is in the drive.
        const core::Status status = lookedAt(device);
        QVERIFY(status.capabilities && status.capabilities->contains(u"media"_s));
        QVERIFY(status.offersMedia);
        QVERIFY(!status.media);
        QVERIFY(fetchedOrSent(pve).isEmpty());
        QVERIFY(changesSent(pve).isEmpty());
    }

    // The web's test_what_the_token_lacks_is_said_in_proxmoxs_words_and_nothing_else_is_done, its list: a storage the
    // token may not see is not even named, one Proxmox says is not active is said and not asked, one of another node
    // is not the guest's, and one whose images could not be listed is said.
    void whatTheTokenMayNotSeeIsNotListed()
    {
        ProxmoxByHand pve(proxmoxTls());
        Storage hidden;
        hidden.images = {{u"secret-install.iso"_s, 1}};
        hidden.maySee = false;
        pve.storages.insert(u"hidden"_s, hidden);
        Storage nas;
        nas.images = {{u"windows.iso"_s, 6'000'000'000}};
        nas.active = false;
        pve.storages.insert(u"nas"_s, nas);
        Storage isos;
        isos.images = {{u"ubuntu-24.04.iso"_s, 3'000'000'000}};
        isos.nodes = {u"pve2"_s};
        pve.storages.insert(u"isos"_s, isos);
        Storage away;
        away.images = {{u"gone.iso"_s, 1}};
        away.online = false;
        pve.storages.insert(u"away"_s, away);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaList> listed = mediaOf(device);
        QVERIFY2(listed, listed ? "" : listed.error().message.c_str());
        QCOMPARE(imagesIn(listed.value()), (QStringList{alpine + u" alpine-3.21.iso local 62000000"_s,
                                                        debian + u" debian-13.iso local 700000000"_s}));
        // (Those Proxmox says are not active first, as their list says them; then those whose images were not listed.)
        QCOMPARE(listed.value().unseen.size(), 2);
        QCOMPARE(listed.value().unseen.at(0), u"nas: Proxmox says it is not active"_s);
        QVERIFY2(listed.value().unseen.at(1).startsWith(u"away: "_s), qPrintable(listed.value().unseen.join(u"; ")));
        QVERIFY(listed.value().unseen.at(1).contains(u"is not online"_s));
        QVERIFY(!listed.value().unseen.join(u' ').contains(u"hidden"_s));
        QCOMPARE(pve.asked(u"GET /nodes/pve1/storage/nas/content"_s), 0);
        QCOMPARE(pve.asked(u"GET /nodes/pve1/storage/hidden/content"_s), 0);
        QCOMPARE(pve.asked(u"GET /nodes/pve1/storage/isos/content"_s), 0);
    }

    // Review c9b-2, L3: the images of a node's storages are listed within one bound as a whole (one question's time):
    // the storages not asked by then are said, and never asked.
    void aNodesImagesAreListedWithinOneBound()
    {
        ProxmoxByHand pve(proxmoxTls());
        for (int one = 1; one <= 5; ++one) {
            Storage slow;
            slow.images = {{u"slow%1.iso"_s.arg(one), 1}};
            slow.takesMs = 1500;
            pve.storages.insert(u"slow%1"_s.arg(one), slow);
        }
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QElapsedTimer took;
        took.start();
        const base::Result<MediaList> listed = mediaOf(device);
        QVERIFY2(listed, listed ? "" : listed.error().message.c_str());
        // (The fifth is due no sooner than six seconds on, past the bound of five and a half: never asked.)
        QCOMPARE(pve.asked(u"GET /nodes/pve1/storage/slow5/content"_s), 0);
        QVERIFY2(listed.value().unseen.contains(
                     u"slow5: not listed: listing its node's storages took all the time there is for it"_s),
                 qPrintable(listed.value().unseen.join(u"; ")));
        QVERIFY(imagesIn(listed.value()).contains(debian + u" debian-13.iso local 700000000"_s));
        QVERIFY2(took.elapsed() < 12'000, qPrintable(QString::number(took.elapsed())));
    }

    // Review c9b-3, L1: a storage asked late in the listing is held to the listing's bound (one connection's time and
    // half a second at the least), not given a question's whole time: one asked some three seconds in, which trickles
    // its answer and never ends it, is given up on at the bound, some two and a half seconds after it was asked, where
    // its own whole would be five and a half. (Measured from its asking, as the stand-in hears it, to the listing's
    // answer, which is waited for: what the questions and the storages before it took counts for nothing; review c9b-4,
    // L6.)
    void aStorageAskedLateIsHeldToTheListingsBound()
    {
        ProxmoxByHand pve(proxmoxTls());
        for (int one = 1; one <= 2; ++one) {
            Storage slow;
            slow.images = {{u"slow%1.iso"_s.arg(one), 1}};
            slow.takesMs = 1500;
            pve.storages.insert(u"slow%1"_s.arg(one), slow);
        }
        Storage hangs;
        hangs.images = {{u"late.iso"_s, 1}};
        hangs.trickles = true;
        pve.storages.insert(u"slow4"_s, hangs);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QElapsedTimer clock;
        clock.start();
        // (Asked in their order: local, slow1, slow2, and slow4 the fourth.)
        int askedFor = 0;
        qint64 lateAskedAt = -1;
        pve.afterImagesListed = [&askedFor, &lateAskedAt, &clock] {
            if (++askedFor == 4) {
                lateAskedAt = clock.elapsed();
            }
        };
        const base::Result<MediaList> listed = mediaOf(device);
        const qint64 endedAt = clock.elapsed();
        QVERIFY2(listed, listed ? "" : listed.error().message.c_str());
        QCOMPARE(pve.asked(u"GET /nodes/pve1/storage/slow4/content"_s), 1);
        QVERIFY(lateAskedAt >= 0);
        QVERIFY2(
            std::ranges::any_of(listed.value().unseen, [](const QString& one) { return one.startsWith(u"slow4: "_s); }),
            qPrintable(listed.value().unseen.join(u"; ")));
        QVERIFY(imagesIn(listed.value()).contains(u"slow2:iso/slow2.iso slow2.iso slow2 1"_s));
        // (Held: given up two to two and a half seconds after it was asked; its own whole: five and a half.)
        QVERIFY2(endedAt - lateAskedAt < 4500, qPrintable(QString::number(endedAt - lateAskedAt)));
    }

    // Review c9b-2, L2: a guest moved to another node of its cluster between the listing of its images and the change
    // is sent no change: what was listed is of the other node's storages.
    void aGuestMovedAfterItsImagesWereListedIsSentNoChange()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes.append(u"pve2"_s);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        pve.afterImagesListed = [&pve] { pve.guests[100].node = u"pve2"_s; };
        const base::Result<MediaDone> done = mounted(device, mountOf(debian));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), u"The guest is on node pve2 of its cluster now, and its images were listed on pve1: "
                               u"nothing was changed. Look again."_s);
        QVERIFY(changesSent(pve).isEmpty());
        QVERIFY(pve.changes.isEmpty());
        // Asked again, it is listed and changed there.
        pve.afterImagesListed = nullptr;
        const base::Result<MediaDone> again = mounted(device, mountOf(debian));
        QVERIFY2(again, qPrintable(saidOf(again)));
        QCOMPARE(changesSent(pve), QStringList{u"PUT /nodes/pve2/qemu/100/config"_s});
        QVERIFY(pve.asked(u"GET /nodes/pve2/storage/local/content"_s) == 1);
    }

    // Outside review of 0.2.0, 3.5: an image picked in a box is bound to the node its images were listed on. The list
    // carries a mark of the guest and that node (MediaList::listed), a mount names it, and a mount whose guest was
    // moved to another node of its cluster since is refused there, also where that node has an image of that name on a
    // storage of that name; a list made there carries that node's mark, and its image goes in. A mount that names no
    // list is refused too.
    void anImagePickedFromOneNodesListIsNotMountedOnAnother()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes.append(u"pve2"_s); // ("local" is on every node, with an image of that name on each)
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaList> onFirst = mediaOf(device);
        QVERIFY2(onFirst, onFirst ? "" : onFirst.error().message.c_str());
        QVERIFY2(core::isListedMark(onFirst.value().listed), qPrintable(onFirst.value().listed));
        // Moved by its high availability before the yes.
        pve.guests[100].node = u"pve2"_s;
        MediaMount picked = mountOf(debian);
        picked.listed = onFirst.value().listed;
        const base::Result<MediaDone> done = mounted(device, picked);
        QCOMPARE(codeOf(done), std::string("changed"));
        QVERIFY2(saidOf(done).contains(u"pve2"_s), qPrintable(saidOf(done)));
        QVERIFY(changesSent(pve).isEmpty());
        QVERIFY(pve.changes.isEmpty());
        // Listed there, its mark is another, and the image picked from that list goes in there.
        const base::Result<MediaList> onSecond = mediaOf(device);
        QVERIFY2(onSecond, onSecond ? "" : onSecond.error().message.c_str());
        QVERIFY(core::isListedMark(onSecond.value().listed));
        QVERIFY(onSecond.value().listed != onFirst.value().listed);
        picked.listed = onSecond.value().listed;
        const base::Result<MediaDone> there = mounted(device, picked);
        QVERIFY2(there, qPrintable(saidOf(there)));
        QCOMPARE(changesSent(pve), QStringList{u"PUT /nodes/pve2/qemu/100/config"_s});
        // A mount that names no list of the guest's images.
        MediaMount unnamed = mountOf(alpine);
        unnamed.listed.clear();
        const base::Result<MediaDone> nameless = mounted(device, unnamed);
        QCOMPARE(codeOf(nameless), std::string("changed"));
        QCOMPARE(pve.changes.size(), 1);
    }

    // The web's test_what_a_proxmox_lists_is_taken_by_kind.
    void whatAProxmoxListsIsTakenByKind()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.hostile = u"odd_images"_s;
        pve.storages[u"local"_s].images.insert(u"with space.iso"_s, 1);
        pve.storages[u"local"_s].images.insert(u"comma,media=disk.iso"_s, 1);
        pve.storages[u"local"_s].images.insert(u"notes.txt"_s, 1);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        base::Result<MediaList> listed = mediaOf(device);
        QVERIFY2(listed, listed ? "" : listed.error().message.c_str());
        QCOMPARE(imagesIn(listed.value()),
                 (QStringList{alpine + u" alpine-3.21.iso local 62000000"_s,
                              debian + u" debian-13.iso local 700000000"_s, u"local:iso/odd.iso odd.iso local -"_s}));
        QCOMPARE(listed.value().leftOut, 10);
        pve.hostile = u"odd_storages"_s;
        listed = mediaOf(device);
        QVERIFY2(listed, listed ? "" : listed.error().message.c_str());
        QVERIFY(std::ranges::all_of(listed.value().images,
                                    [](const core::MediaImage& one) { return one.where == "local"_L1; }));
        QCOMPARE(listed.value().unseen, QStringList{u"zz: Proxmox says it is not active"_s});
    }

    // What is in its drive, as the guest's settings say it (the web's media_now), on its card at a look.
    void theCardSaysWhatIsInTheDrive()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests[100].config.insert(u"ide2"_s, debian + u",media=cdrom"_s);
        ProxmoxDevice::Pace pace = fast();
        pace.sameGuestEveryMs = 0; // (its settings read at every look)
        ProxmoxDevice device(known(pve), pve.secret, pace);
        core::Status status = lookedAt(device);
        QVERIFY2(status.media, qPrintable(status.state + u' ' + status.errorMessage));
        QCOMPARE(inThe(status.media).drive, u"cd"_s);
        QCOMPARE(inThe(status.media).image, debian);
        QCOMPARE(inThe(status.media).name, u"debian-13.iso"_s);
        QCOMPARE(inThe(status.media).where, u"local"_s);
        QVERIFY(inThe(status.media).readOnly);
        QCOMPARE(inThe(status.media).first, std::optional<bool>(false));
        // The drive first in its boot order; the host's own CD drive; nothing.
        pve.guests[100].config.insert(u"boot"_s, u"order=ide2;scsi0;net0"_s);
        status = lookedAt(device);
        QVERIFY(status.media);
        QCOMPARE(inThe(status.media).first, std::optional<bool>(true));
        pve.guests[100].config.insert(u"ide2"_s, u"cdrom,media=cdrom"_s);
        status = lookedAt(device);
        QVERIFY(status.media);
        QCOMPARE(inThe(status.media).name, u"the host's own CD drive"_s);
        QVERIFY(inThe(status.media).where.isEmpty());
        pve.guests[100].config.insert(u"ide2"_s, u"none,media=cdrom"_s);
        QVERIFY(!lookedAt(device).media);
        // The list says it as the card does.
        pve.guests[100].config.insert(u"ide2"_s, alpine + u",media=cdrom,size=62M"_s);
        const base::Result<MediaList> listed = mediaOf(device);
        QVERIFY2(listed, listed ? "" : listed.error().message.c_str());
        QVERIFY(listed.value().mounted);
        QCOMPARE(inThe(listed.value().mounted).image, alpine);
    }

    // The web's test_an_admin_mounts_an_image_and_the_tile_and_the_activity_log_say_so: put in, on the settings read
    // for it and nothing else of them; on the card; taken out; and nothing taken out where nothing is in.
    void anImageIsPutInTheGuestsDriveAndTakenOut()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        base::Result<MediaDone> done = mounted(device, mountOf(debian));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(done.value().note.isEmpty());
        QVERIFY(done.value().mounted);
        QCOMPARE(inThe(done.value().mounted).image, debian);
        QCOMPARE(inThe(done.value().mounted).name, u"debian-13.iso"_s);
        QCOMPARE(inThe(done.value().mounted).where, u"local"_s);
        QVERIFY(inThe(done.value().mounted).readOnly);
        QCOMPARE(inThe(done.value().mounted).first, std::optional<bool>(false));
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), debian + u",media=cdrom"_s);
        QCOMPARE(pve.guests.value(100).config.value(u"boot"_s), firstOrder);
        QVERIFY(pve.guests.value(100).pending.isEmpty());
        QCOMPARE(pve.changes, QStringList{u"100 ide2="_s + debian + u",media=cdrom"_s});
        QCOMPARE(changesSent(pve), QStringList{u"PUT /nodes/pve1/qemu/100/config"_s});
        QVERIFY(pve.tasks.isEmpty()); // (no power command)
        QVERIFY(lookedAt(device).media);
        done = unmounted(device, debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(done.value().note.isEmpty());
        QVERIFY(!done.value().mounted);
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        QCOMPARE(pve.guests.value(100).config.value(u"boot"_s), firstOrder);
        QCOMPARE(pve.changes.size(), 2);
        QCOMPARE(pve.changes.last(), u"100 ide2=none,media=cdrom"_s);
        // Nothing in it: said so, and nothing is sent.
        done = unmounted(device, debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note, u"nothing was mounted"_s);
        QCOMPARE(pve.changes.size(), 2);
        QVERIFY(fetchedOrSent(pve).isEmpty());
    }

    // The web's test_the_confirmation_names_the_machine_and_the_image_or_nothing_is_done, its images: what is no image
    // listed for it now is not mounted (typed, an address, another node's, another kind, an option smuggled in), and
    // what its drive cannot do is not done.
    void whatIsNoImageListedForItIsNotMounted()
    {
        ProxmoxByHand pve(proxmoxTls());
        Storage isos;
        isos.images = {{u"ubuntu-24.04.iso"_s, 3'000'000'000}};
        isos.nodes = {u"pve2"_s};
        pve.storages.insert(u"isos"_s, isos);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        for (const QString& image : {u"local:iso/ubuntu-24.04.iso"_s, u"isos:iso/ubuntu-24.04.iso"_s}) {
            const base::Result<MediaDone> done = mounted(device, mountOf(image));
            QCOMPARE(codeOf(done), std::string("changed"));
            QVERIFY2(saidOf(done).contains(u"is not on the storages of the guest's node that the token may see"_s),
                     qPrintable(saidOf(done)));
        }
        const qsizetype asked = pve.requests.size();
        for (const QString& image :
             {u"http://192.0.2.9/boot.iso"_s, u"local:iso/../../etc/shadow.iso"_s, debian + u",media=disk"_s,
              u"local:images/vm-101-disk-0.raw"_s, u"/dev/sda"_s, u"cdrom"_s}) {
            const base::Result<MediaDone> done = mounted(device, mountOf(image));
            QCOMPARE(codeOf(done), std::string("invalid"));
            QCOMPARE(saidOf(done), u"That is not an image of Proxmox's that qKVM lists"_s);
        }
        // As a USB flash drive, writable, started from in a way it does not know, restarted without starting from it
        // (a power button's): not what a guest's drive does (the web's server.py, media_mount).
        MediaMount flash = mountOf(debian);
        flash.drive = u"flash"_s;
        QCOMPARE(saidOf(mounted(device, flash)), u"web takes no image as a USB flash drive"_s);
        MediaMount writable = mountOf(debian);
        writable.writable = true;
        QCOMPARE(saidOf(mounted(device, writable)), u"web takes an image as a CD-ROM drive read-only, and only so"_s);
        MediaMount now = mountOf(debian);
        now.boot = u"now"_s;
        QCOMPARE(saidOf(mounted(device, now)), u"web cannot be told to start from an image that way"_s);
        MediaMount restarted = mountOf(debian);
        restarted.restart = true;
        QCOMPARE(codeOf(mounted(device, restarted)), std::string("invalid"));
        QCOMPARE(saidOf(mounted(device, restarted)),
                 u"A restart that does not start from the image is a power button's: use the power buttons"_s);
        QCOMPARE(pve.requests.size(), asked); // (none of these asked anything)
        QVERIFY(pve.changes.isEmpty());
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
    }

    // An image in the drive already is said so; taken out by the name of another, nothing is taken out.
    void anImageInAlreadyOrAnotherIsSaidSo()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QVERIFY(mounted(device, mountOf(debian)));
        base::Result<MediaDone> done = mounted(device, mountOf(debian));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note, u"It was in the guest's CD-ROM drive already."_s);
        QCOMPARE(pve.changes.size(), 1);
        done = unmounted(device, alpine);
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done),
                 u"Another image is in the guest's CD-ROM drive now (debian-13.iso): nothing was taken out. "
                 u"Look again."_s);
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), debian + u",media=cdrom"_s);
        QCOMPARE(pve.changes.size(), 1);
    }

    // --- starting from it (review c9b-2, M1: the web's media_boot, _order_back, _restart) ---

    // The web's test_a_guest_started_from_an_image_once_is_started_anew_and_has_its_boot_order_back.
    void aGuestStartedFromAnImageOnceIsStartedAnewAndHasItsBootOrderBack()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY2(done.value().note.contains(u"inside the guest"_s), qPrintable(done.value().note));
        // Stopped at once and started (a reset would have begun from the order it began with), from the image.
        const Guest& guest = pve.guests[100];
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(guest.status, u"running"_s);
        QCOMPARE(guest.booted, QStringList{u"ide2 "_s + debian});
        // Its order as before for every start by Proxmox after this one: pending, as on a running guest.
        QCOMPARE(guest.runningOrder, u"order=ide2;scsi0;net0"_s);
        QCOMPARE(bootOf(guest), firstOrder);
        QVERIFY(done.value().mounted);
        QCOMPARE(inThe(done.value().mounted).first, std::optional<bool>(false));
        // A restart from inside the guest (a reset of the same process) starts from the image again, as the box says.
        pve.guests[100].booted.clear();
        QCOMPARE(codeOf(pressedOn(device, u"atx_reset"_s)), std::string("ok"));
        QCOMPARE(pve.guests.value(100).booted, QStringList{u"ide2 "_s + debian});
        // A start by Proxmox does not.
        QCOMPARE(codeOf(pressedOn(device, u"atx_power_long"_s)), std::string("ok"));
        QCOMPARE(codeOf(pressedOn(device, u"power_on"_s)), std::string("ok"));
        QCOMPARE(pve.guests.value(100).booted.last(), u"scsi0"_s);
        // Stopped, a guest is started from it (no stop first).
        QCOMPARE(codeOf(pressedOn(device, u"atx_power_long"_s)), std::string("ok"));
        pve.tasks.clear();
        const base::Result<MediaDone> again = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY2(again, qPrintable(saidOf(again)));
        QCOMPARE(powerTasks(pve), QStringList{u"qmstart"_s});
        QCOMPARE(pve.guests.value(100).booted.last(), u"ide2 "_s + debian);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        // Each change on the settings read right before it: the image, the order, the order back, twice.
        QCOMPARE(pve.changes.size(), 5);
        // Once is not until it is unmounted: an order set so on Proxmox afterwards is not "put back" at the unmount.
        pve.guests[100].pending.remove(u"boot"_s);
        pve.guests[100].config.insert(u"boot"_s, u"order=ide2;scsi0;net0"_s);
        const base::Result<MediaDone> out = unmounted(device, debian);
        QVERIFY2(out, qPrintable(saidOf(out)));
        QVERIFY2(out.value().note.contains(u"still begins with the CD-ROM drive"_s), qPrintable(out.value().note));
        QCOMPARE(bootOf(pve.guests.value(100)), u"order=ide2;scsi0;net0"_s);
        QVERIFY(fetchedOrSent(pve).isEmpty());
    }

    // An order changed on Proxmox while the guest was started from the image once is left as it is, and said (the
    // web's _order_back).
    void anOrderChangedOnProxmoxMeanwhileIsNotPutBack()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        ProxmoxDevice db(known(pve, 101), pve.secret, fast());
        pve.afterCommand = [&pve] { pve.guests[101].config.insert(u"boot"_s, u"order=net0;ide2;scsi0"_s); };
        const base::Result<MediaDone> done = mounted(db, startedFrom(debian, u"once"_s, u"db"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note,
                 u"It started from the image. Its boot order was changed on Proxmox meanwhile, and is "
                 u"left as it is."_s);
        QVERIFY(!db.bootOrderWas()); // (of no more use)
        QCOMPARE(bootOf(pve.guests.value(101)), u"order=net0;ide2;scsi0"_s);
        QCOMPARE(powerTasks(pve), QStringList{u"qmstart"_s});
    }

    // Outside review of 0.2.0, 3.4: a start from the image starts the guest from the image the person confirmed, and
    // from no other. Another image put in its drive by another hand after this one went in is not started from: put
    // there right after the mount (before the boot order is changed), nothing more is done; put there while the guest
    // is stopped for its start, it is not started again, and its boot order is put back.
    void aStartFromTheImageStartsFromTheOneConfirmed()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        pve.afterConfigRead = [&pve] {
            if (pve.changes.size() == 1 && pve.guests.value(100).config.value(u"ide2"_s).startsWith(debian)) {
                pve.guests[100].config.insert(u"ide2"_s, alpine + u",media=cdrom"_s);
            }
        };
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("changed"));
        QVERIFY2(saidOf(done).contains(u"alpine-3.21.iso"_s), qPrintable(saidOf(done)));
        QVERIFY(powerTasks(pve).isEmpty());
        QCOMPARE(pve.changes.size(), 1);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(pve.guests.value(100).booted.isEmpty());
        // Swapped while it is stopped for the start.
        pve.afterConfigRead = nullptr;
        pve.guests[100].config.insert(u"ide2"_s, u"none,media=cdrom"_s);
        afterTheStop(pve, [&pve] { pve.guests[100].config.insert(u"ide2"_s, alpine + u",media=cdrom"_s); });
        const base::Result<MediaDone> again = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(again), std::string("changed"));
        QVERIFY2(saidOf(again).contains(u"was stopped, and not started again"_s), qPrintable(saidOf(again)));
        QVERIFY2(saidOf(again).contains(u"alpine-3.21.iso"_s), qPrintable(saidOf(again)));
        QCOMPARE(powerTasks(pve), QStringList{u"qmstop"_s});
        QVERIFY(pve.guests.value(100).booted.isEmpty());
        QCOMPARE(pve.guests.value(100).status, u"stopped"_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
    }

    // ...and from the guest's start's own two reads (its state, and its settings for rule 30): another image put in its
    // drive right after the look at it before the start is not started from either (review cx3-1, L5). The guest stays
    // stopped, and its order is put back.
    void aStartFromTheImageLooksAtItsDriveInTheStartsOwnReads()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        bool swapped = false;
        pve.afterConfigRead = [&pve, &swapped] {
            // (The first reading of its settings once it was stopped for the start: the look right before the start.)
            if (!swapped && powerTasks(pve) == QStringList{u"qmstop"_s} &&
                pve.guests.value(100).status == u"stopped"_s) {
                swapped = true;
                pve.guests[100].config.insert(u"ide2"_s, alpine + u",media=cdrom"_s);
            }
        };
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY(swapped);
        QCOMPARE(codeOf(done), std::string("changed"));
        QVERIFY2(saidOf(done).contains(u"was stopped, and not started again"_s), qPrintable(saidOf(done)));
        QVERIFY2(saidOf(done).contains(u"alpine-3.21.iso"_s), qPrintable(saidOf(done)));
        QCOMPARE(powerTasks(pve), QStringList{u"qmstop"_s});
        QVERIFY(pve.guests.value(100).booted.isEmpty());
        QCOMPARE(pve.guests.value(100).status, u"stopped"_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
    }

    // Review cx3-2, L2: ...and on its reading again before the start is sent a second time after a 401 (the token taken
    // back for a moment, or Proxmox restarted: a sign-in's time and more go by): another image put in meanwhile is not
    // started from, and that is said in the same words; nor is any where that reading did not come.
    void aStartSentAgainAfterA401LooksAtItsDriveAgain_data()
    {
        QTest::addColumn<bool>("swapped");
        QTest::newRow("another image put in meanwhile") << true;
        QTest::newRow("its settings not read then") << false;
    }

    void aStartSentAgainAfterA401LooksAtItsDriveAgain()
    {
        QFETCH(bool, swapped);
        ProxmoxByHand pve(proxmoxTls());
        // (Known by no number it gives for itself where its settings are not read: then nothing else stops the start.)
        ProxmoxDevice device(swapped ? known(pve) : guestOf(pve), pve.secret, fast());
        if (!swapped) {
            pve.guests[100].uuid.clear();
        }
        const QString start = u"POST /nodes/pve1/qemu/100/status/start"_s;
        bool refused = false;
        pve.refuses = [&pve, &refused, &start, swapped](const QString& said) {
            if (said != start || std::exchange(refused, true)) {
                return false;
            }
            if (swapped) {
                pve.guests[100].config.insert(u"ide2"_s, alpine + u",media=cdrom"_s);
            } else {
                pve.configFails = 1;
            }
            return true;
        };
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY(refused);
        QCOMPARE(pve.refusedPaths, QStringList{start});
        QCOMPARE(pve.asked(start), 0);
        QVERIFY2(saidOf(done).contains(u"was stopped, and not started again"_s), qPrintable(saidOf(done)));
        if (swapped) {
            QCOMPARE(codeOf(done), std::string("changed"));
            QVERIFY2(saidOf(done).contains(u"alpine-3.21.iso"_s), qPrintable(saidOf(done)));
        } else {
            QVERIFY(!done);
        }
        QCOMPARE(powerTasks(pve), QStringList{u"qmstop"_s});
        QVERIFY(pve.guests.value(100).booted.isEmpty());
        QCOMPARE(pve.guests.value(100).status, u"stopped"_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
    }

    // ...and on its reading on the node it is followed to, where Proxmox answers the start that the guest is not on the
    // node it went to (moved within its cluster right after the start's own reading, another image put in with it): not
    // started from that image there, in the same words.
    void aStartFollowedToAnotherNodeLooksAtItsDriveThere()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes.append(u"pve2"_s);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        int readsSinceTheStop = 0;
        pve.afterConfigRead = [&pve, &readsSinceTheStop] {
            // (The second reading of its settings once it was stopped for the start: the start's own, right before it.)
            if (powerTasks(pve) == QStringList{u"qmstop"_s} && pve.guests.value(100).status == u"stopped"_s &&
                ++readsSinceTheStop == 2) {
                pve.guests[100].node = u"pve2"_s;
                pve.guests[100].config.insert(u"ide2"_s, alpine + u",media=cdrom"_s);
            }
        };
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY(readsSinceTheStop >= 2);
        QCOMPARE(device.where().node, u"pve2"_s);
        QCOMPARE(codeOf(done), std::string("changed"));
        QVERIFY2(saidOf(done).contains(u"was stopped, and not started again"_s), qPrintable(saidOf(done)));
        QVERIFY2(saidOf(done).contains(u"alpine-3.21.iso"_s), qPrintable(saidOf(done)));
        QCOMPARE(pve.asked(u"POST /nodes/pve2/qemu/100/status/start"_s), 0);
        QCOMPARE(powerTasks(pve), QStringList{u"qmstop"_s});
        QVERIFY(pve.guests.value(100).booted.isEmpty());
        QCOMPARE(pve.guests.value(100).status, u"stopped"_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
    }

    // Review cx3-3, L1: ...and where the start finds its driver signed out on its way (another question of the driver's
    // got nothing back while the start's own reading of its settings was out), on a reading after its sign-in again:
    // another image put in while that sign-in was out is not started from, in the same words, and nothing is sent.
    void aStartThatFindsItsDriverSignedOutLooksAtItsDriveAgain()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests[100].status = u"stopped"_s;
        ProxmoxDevice device(known(pve), pve.secret, fast());
        int readsSinceTheOrder = 0;
        bool signedOut = false;
        bool swapped = false;
        pve.answering = [&](const QString& said, Answered& answer) {
            if (said == u"GET /nodes/pve1/qemu/101/config"_s) {
                answer.hangUp = true; // (another question of the driver's: nothing comes back)
                signedOut = true;
                return;
            }
            // (Its settings read twice once its boot order was written: the look before the start, then the start's
            // own, right before its command. That one a moment in coming, and the other question asked meanwhile.)
            if (said == u"GET /nodes/pve1/qemu/100/config"_s && pve.changes.size() == 2 && ++readsSinceTheOrder == 2) {
                answer.afterMs = 1000;
                QTimer::singleShot(100, &device, [&device] {
                    device.numberOfGuest(u"pve1"_s, 101, [](const base::Result<QString>& /*number*/) {});
                });
                return;
            }
            if (said == u"GET /version"_s && signedOut && !std::exchange(swapped, true)) {
                pve.guests[100].config.insert(u"ide2"_s, alpine + u",media=cdrom"_s);
            }
        };
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY(signedOut);
        QVERIFY(swapped);
        QCOMPARE(codeOf(done), std::string("changed"));
        QVERIFY2(saidOf(done).contains(u"was not started from it"_s), qPrintable(saidOf(done)));
        QVERIFY2(saidOf(done).contains(u"alpine-3.21.iso"_s), qPrintable(saidOf(done)));
        QCOMPARE(pve.asked(u"POST /nodes/pve1/qemu/100/status/start"_s), 0);
        QVERIFY(powerTasks(pve).isEmpty());
        QVERIFY(pve.guests.value(100).booted.isEmpty());
        QCOMPARE(pve.guests.value(100).status, u"stopped"_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
    }

    // Outside review of 0.2.0, 3.6: a start from the image where the order qKVM wrote still stands and the CD-ROM
    // drive was moved since (another order to write) never loses the order from before it. That order is put back
    // first, and only then is the new one kept and written: a program that ends right after the new record was kept
    // (here: the record as it is on disk then, and its change never made) finds at its next unmount the guest's own
    // order on Proxmox, never the one it wrote first beside the record of the second.
    void aSecondStartWhoseDriveMovedKeepsTheOrderFromBefore()
    {
        ProxmoxByHand pve(proxmoxTls());
        Guest& guest = pve.guests[100];
        guest.status = u"stopped"_s;
        guest.config.remove(u"ide2"_s);
        guest.config.insert(u"sata0"_s, debian + u",media=cdrom"_s);
        guest.config.insert(u"boot"_s, u"order=ide2;scsi0;net0"_s); // (written by qKVM, the drive at ide2 then)
        const core::BootOrderWas was{u"scsi0;net0"_s, u"ide2;scsi0;net0"_s, 100, numberOf(guest)};
        std::optional<core::BootOrderWas> onDisk = was;
        bool over = false;
        ProxmoxDevice device(keptWith(pve, was), pve.secret, fast());
        device.keepBootOrdersBy([&pve, &onDisk, &over](const std::optional<core::BootOrderWas>& kept) {
            if (over) {
                return base::Result<void>(); // (the program has ended: nothing more reaches its disk)
            }
            onDisk = kept;
            if (kept && kept->written.startsWith(u"sata0"_s)) {
                // It ends here: the new record is on disk, and its order is never written.
                over = true;
                pve.changeRefused = u"can't lock file '/var/lock/qemu-server/lock-100.conf' - got timeout"_s;
            }
            return base::Result<void>();
        });
        static_cast<void>(mounted(device, startedFrom(debian, u"once"_s)));
        QVERIFY(over);
        pve.changeRefused.clear();
        // Its next run, from what is on its disk.
        ProxmoxDevice next(keptWith(pve, onDisk), pve.secret, fast());
        const base::Result<MediaDone> out = unmounted(next, debian);
        QVERIFY2(out, qPrintable(saidOf(out)));
        QCOMPARE(bootOf(pve.guests.value(100)), u"order=scsi0;net0"_s);
    }

    // Rule 30 right before the boot order is changed: another virtual machine made under the guest's number after the
    // image was put in is sent no order and no power command.
    void anotherVmMadeAfterTheImageWasPutInIsNotStarted()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        // (Its settings read for the listing, right before the change, and after it: then made anew under the number.)
        int reads = 0;
        pve.afterConfigRead = [&pve, &reads] {
            if (++reads != 3) {
                return;
            }
            Guest intruder = pve.guests.value(100);
            intruder.name = u"intruder"_s;
            intruder.uuid = ProxmoxByHand::fresh();
            pve.deleteGuest(100);
            pve.makeGuest(intruder);
        };
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("another_machine"));
        QVERIFY2(saidOf(done).startsWith(u"The image is mounted, and web was not started from it: "_s),
                 qPrintable(saidOf(done)));
        QCOMPARE(pve.changes.size(), 1); // (the image, in the guest's drive)
        QCOMPARE(pve.guests.value(100).config.value(u"boot"_s), firstOrder);
        QVERIFY(powerTasks(pve).isEmpty());
    }

    // The web's test_a_guest_started_from_an_image_until_it_is_unmounted_has_its_order_back_at_the_unmount.
    void aGuestStartedFromAnImageUntilItIsUnmountedHasItsOrderBackAtTheUnmount()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        // Stopped: the order is its own at once, and its next start is from the image.
        ProxmoxDevice db(known(pve, 101), pve.secret, fast());
        base::Result<MediaDone> done = mounted(db, startedFrom(debian, u"until_unmounted"_s, u"db"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY2(done.value().note.contains(u"next start"_s), qPrintable(done.value().note));
        QVERIFY(powerTasks(pve).isEmpty());
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), u"order=ide2;scsi0;net0"_s);
        QCOMPARE(inThe(done.value().mounted).first, std::optional<bool>(true));
        QCOMPARE(codeOf(pressedOn(db, u"power_on"_s)), std::string("ok"));
        QCOMPARE(pve.guests.value(101).booted, QStringList{u"ide2 "_s + debian});
        done = unmounted(db, debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note, QString());
        QCOMPARE(pve.guests.value(101).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        QCOMPARE(bootOf(pve.guests.value(101)), firstOrder);
        // Running, and restarted now: from the image at once; and its order back at the unmount, for its next start.
        ProxmoxDevice web(known(pve, 100), pve.secret, fast());
        pve.tasks.clear();
        done = mounted(web, startedFrom(debian, u"until_unmounted"_s, u"web"_s, true));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.guests.value(100).booted, QStringList{u"ide2 "_s + debian});
        QCOMPARE(bootOf(pve.guests.value(100)), u"order=ide2;scsi0;net0"_s);
        done = unmounted(web, debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        QVERIFY(fetchedOrSent(pve).isEmpty());
    }

    // The web's test_an_order_it_set_is_left_and_said_where_qkvm_does_not_know_the_one_before. (Where the web's is
    // after a restart of qKVM, the desktop's is a driver that keeps nothing of the guest's order: kept with the guest,
    // it outlives a restart: aBootOrderFromBeforeIsKeptWithTheGuestAcrossARestartOfTheProgram.)
    void anOrderItSetIsLeftAndSaidWhereTheOneBeforeIsNotKnown()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        ProxmoxDevice db(known(pve, 101), pve.secret, fast());
        QVERIFY(mounted(db, startedFrom(debian, u"until_unmounted"_s, u"db"_s)));
        // (What it wrote in the order's place is not known to this one.)
        ProxmoxDevice anew(known(pve, 101), pve.secret, fast());
        base::Result<MediaDone> done = unmounted(anew, debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note,
                 u"Its boot order still begins with the CD-ROM drive (not set so from qKVM): empty, it "
                 u"is passed over."_s);
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), u"order=ide2;scsi0;net0"_s);
        QCOMPARE(pve.guests.value(101).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        // And one changed on Proxmox since it was set is left as it is: its own words, no "put back" over them (as the
        // web's; and once more where the order before was remembered: put back only over what was written).
        QVERIFY(mounted(db, startedFrom(debian, u"until_unmounted"_s, u"db"_s)));
        pve.guests[101].config.insert(u"boot"_s, u"order=ide2;net0;scsi0"_s);
        QVERIFY(unmounted(db, debian));
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), u"order=ide2;net0;scsi0"_s);
        QVERIFY(!db.bootOrderWas()); // (of no more use)
        pve.guests[101].config.insert(u"boot"_s, firstOrder);
        QVERIFY(mounted(db, startedFrom(debian, u"until_unmounted"_s, u"db"_s)));
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), u"order=ide2;scsi0;net0"_s);
        pve.guests[101].config.insert(u"boot"_s, u"order=ide2;net0;scsi0"_s);
        QVERIFY(unmounted(db, debian));
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), u"order=ide2;net0;scsi0"_s);
    }

    // The web's test_a_boot_order_written_the_old_way_is_not_changed.
    void aBootOrderWrittenTheOldWayIsNotChanged()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests[100].config.insert(u"boot"_s, u"cdn"_s);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(saidOf(done).startsWith(u"The image is mounted, and web was not started from it: "_s),
                 qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).contains(u"Boot Order"_s));
        QCOMPARE(pve.guests.value(100).config.value(u"boot"_s), u"cdn"_s);
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), debian + u",media=cdrom"_s);
        QVERIFY(powerTasks(pve).isEmpty());
    }

    // The web's test_what_the_token_lacks_is_said_in_proxmoxs_words_and_nothing_else_is_done, its start from it.
    void whatTheTokenLacksToStartFromItIsSaidAndNothingElseIsDone()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        // No VM.Config.Disk: mounted, and its boot order left as it was, and said.
        pve.guests[100].mayDisk = false;
        base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"until_unmounted"_s));
        QVERIFY(!done);
        QVERIFY2(saidOf(done).startsWith(u"The image is mounted, and web was not started from it: "_s),
                 qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).contains(u"VM.Config.Disk"_s));
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), debian + u",media=cdrom"_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas()); // (not written: nothing to put back)
        // No VM.PowerMgmt: not restarted, and the order it set for the start that did not come about put back.
        pve.guests[100].mayDisk = true;
        pve.guests[100].mayPower = false;
        done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY(!done);
        QVERIFY2(saidOf(done).contains(u"VM.PowerMgmt"_s), qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).startsWith(u"The image is mounted, and web was not started from it: "_s));
        QVERIFY2(saidOf(done).endsWith(u". Its boot order was put back."_s), qPrintable(saidOf(done)));
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(powerTasks(pve).isEmpty());
        QVERIFY(!saidOf(done).contains(pve.secret));
        QVERIFY(!device.bootOrderWas());
    }

    // A change of the boot order Qt sent twice, the first carried out: read back, it holds what was asked (as a
    // drive's does: aChangeAskedTwiceByQtIsDoneOnce).
    void aBootOrderAskedTwiceByQtIsDoneOnce()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.configHangsUp = 2;
        ProxmoxDevice db(known(pve, 101), pve.secret, fast());
        const base::Result<MediaDone> done = mounted(db, startedFrom(debian, u"until_unmounted"_s, u"db"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(pve.changes,
                 (QStringList{u"101 ide2="_s + debian + u",media=cdrom"_s, u"101 boot=order=ide2;scsi0;net0"_s}));
        QCOMPARE(changesSent(pve).size(), 4);
    }

    // --- a start from the image that fails halfway (review c9b-3, M1) ---

    // Started from the image once, and its boot order then not put back (the line to Proxmox fell a moment): said as
    // started from it, with why the order is not back; the order from before kept with the guest. Tried again, it is
    // restarted as the person asked and said yes to (the web's _restart, every time: what the guest started from is
    // not Proxmox's to say; review c9b-4, M1), and the order from before is put back.
    void aStartOnceWhoseOrderIsNotPutBackIsSaidSoAndATryAgainRestartsIt()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        afterTheStart(pve, [&pve] { pve.configFails = 1; }); // (the read before the order is put back)
        base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(saidOf(done).startsWith(u"The image is mounted, and web started from it, and its boot order could "
                                         u"not be put back ("_s),
                 qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).endsWith(u"until qKVM puts it back when the image is unmounted, or it is set on "
                                      u"Proxmox."_s));
        QVERIFY(!saidOf(done).contains(u"not started"_s));
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.guests.value(100).booted, QStringList{u"ide2 "_s + debian});
        QCOMPARE(bootOf(pve.guests.value(100)), u"order=ide2;scsi0;net0"_s);
        const std::optional<core::BootOrderWas> kept = device.bootOrderWas();
        QVERIFY(kept);
        QCOMPARE(kept.value_or(core::BootOrderWas{}).order, u"scsi0;ide2;net0"_s);
        QCOMPARE(kept.value_or(core::BootOrderWas{}).written, u"ide2;scsi0;net0"_s);
        QCOMPARE(kept.value_or(core::BootOrderWas{}).vmid, qint64{100});
        QCOMPARE(kept.value_or(core::BootOrderWas{}).machine, numberOf(pve.guests.value(100)));
        // Tried again: stopped and started from it once more, as asked, and its order from before put back.
        pve.afterCommand = nullptr;
        done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note, u"It was in the guest's CD-ROM drive already. It started from the image. Its next "
                                    u"start by Proxmox is from its boot order as before; a restart from inside the "
                                    u"guest starts from the image again while it is mounted."_s);
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s, u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.guests.value(100).booted, (QStringList{u"ide2 "_s + debian, u"ide2 "_s + debian}));
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
    }

    // Review c9b-4, M1: started from the image until it is unmounted, and restarted for it, the guest runs from it; a
    // start "once, now" asked after that (its installer hangs, say, and the person wants it from its start again) is
    // done as asked: stopped, started, and its order from before put back. And so is "until unmounted" with a restart
    // asked a second time, and a third, where the order this program wrote stands.
    void aStartAfterAStartFromTheImageWithARestartRestartsItAsAsked()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QVERIFY(mounted(device, startedFrom(debian, u"until_unmounted"_s, u"web"_s, true)));
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.guests.value(100).booted, QStringList{u"ide2 "_s + debian});
        base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note, u"It was in the guest's CD-ROM drive already. It started from the image. Its next "
                                    u"start by Proxmox is from its boot order as before; a restart from inside the "
                                    u"guest starts from the image again while it is mounted."_s);
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s, u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.guests.value(100).booted, (QStringList{u"ide2 "_s + debian, u"ide2 "_s + debian}));
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
        for (int time = 1; time <= 2; ++time) {
            done = mounted(device, startedFrom(debian, u"until_unmounted"_s, u"web"_s, true));
            QVERIFY2(done, qPrintable(saidOf(done)));
            QCOMPARE(powerTasks(pve).size(), 4 + 2 * time);
            QCOMPARE(pve.guests.value(100).booted.size(), 2 + time);
            QCOMPARE(pve.guests.value(100).booted.last(), u"ide2 "_s + debian);
            QCOMPARE(bootOf(pve.guests.value(100)), u"order=ide2;scsi0;net0"_s);
        }
        // (Its order from before still kept, for the unmount.)
        QVERIFY(unmounted(device, debian));
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
    }

    // Review c9b-4, M1: a guest set to start from the image until it is unmounted, without a restart, and then moved
    // to another node of its cluster while it ran (a live migration: a young process there, which goes on from what
    // the guest ran on its first node, never from the image) is restarted when a start from the image is asked, on the
    // node it is on now.
    void aGuestMovedToAnotherNodeIsRestartedFromTheImageAsAsked()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes.append(u"pve2"_s);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QVERIFY(mounted(device, startedFrom(debian, u"until_unmounted"_s)));
        QVERIFY(powerTasks(pve).isEmpty());
        QVERIFY(device.bootOrderWas());
        // Moved: its process on pve2 is two seconds old, and runs on as it ran on pve1 (its order pending still).
        pve.guests[100].node = u"pve2"_s;
        pve.guests[100].uptime = 2;
        // (Its images listed again, as a box opened after the move lists them: on pve2, where it is picked from.)
        QVERIFY(mediaOf(device));
        QCOMPARE(device.where().node, u"pve2"_s);
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.asked(u"POST /nodes/pve2/qemu/100/status/stop"_s), 1);
        QCOMPARE(pve.asked(u"POST /nodes/pve2/qemu/100/status/start"_s), 1);
        QCOMPARE(pve.guests.value(100).booted, QStringList{u"ide2 "_s + debian});
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
    }

    // A guest that ran before this program's order stood: tried again "once", it is restarted (and so is one whose
    // order the program set "until unmounted", without a restart, while it ran).
    void aGuestThatRanBeforeTheOrderStoodIsRestarted()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QVERIFY(mounted(device, startedFrom(debian, u"until_unmounted"_s)));
        QVERIFY(powerTasks(pve).isEmpty());
        QVERIFY(device.bootOrderWas());
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.guests.value(100).booted, QStringList{u"ide2 "_s + debian});
        // (Once: its order before put back, the one the start "until unmounted" replaced.)
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
    }

    // Another image put in since the guest started from the one before: the guest does not run from this one, and is
    // restarted for it.
    void aGuestThatRanBeforeAnotherImageWasPutInIsRestarted()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QVERIFY(mounted(device, startedFrom(alpine, u"until_unmounted"_s, u"web"_s, true)));
        QCOMPARE(pve.guests.value(100).booted, QStringList{u"ide2 "_s + alpine});
        QVERIFY(mounted(device, mountOf(debian)));
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s, u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.guests.value(100).booted, (QStringList{u"ide2 "_s + alpine, u"ide2 "_s + debian}));
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
    }

    // A boot order kept of another guest (another number it gives for itself) is none of this one's: nothing of it is
    // put back.
    void aBootOrderKeptOfAnotherGuestIsNotUsed()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests[100].config.insert(u"ide2"_s, debian + u",media=cdrom"_s);
        pve.guests[100].config.insert(u"boot"_s, u"order=ide2;scsi0;net0"_s);
        const core::BootOrderWas another{u"net0;scsi0;ide2"_s, u"ide2;scsi0;net0"_s, 100,
                                         u"proxmox:"_s + QString(32, u'a')};
        ProxmoxDevice device(keptWith(pve, another), pve.secret, fast());
        const base::Result<MediaDone> done = unmounted(device, debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY2(done.value().note.contains(u"not set so from qKVM"_s), qPrintable(done.value().note));
        QCOMPARE(pve.guests.value(100).config.value(u"boot"_s), u"order=ide2;scsi0;net0"_s);
        QCOMPARE(pve.changes.size(), 1); // (the image out, and nothing else)
    }

    // Review c9b-4, L2: the order this program wrote stands, and the image is in another CD-ROM drive of the guest's
    // now (ide2 empty, the image in sata0: moved on Proxmox). The order is written anew with that drive first; where
    // Proxmox refuses that for certain, the order from before is still kept, and the unmount puts it back.
    void aRefusedOrderForAnotherDriveKeepsTheOrderFromBefore()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests[100].config.insert(u"boot"_s, u"order=ide2;scsi0;net0"_s);
        pve.guests[100].config.insert(u"ide2"_s, u"none,media=cdrom"_s);
        pve.guests[100].config.insert(u"sata0"_s, debian + u",media=cdrom"_s);
        const core::BootOrderWas was{u"scsi0;ide2;net0"_s, u"ide2;scsi0;net0"_s, 100, numberOf(pve.guests.value(100))};
        ProxmoxDevice device(keptWith(pve, was), pve.secret, fast());
        pve.guests[100].mayDisk = false; // (no VM.Config.Disk: the boot order is refused, 403)
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"until_unmounted"_s));
        QVERIFY(!done);
        QVERIFY2(saidOf(done).contains(u"VM.Config.Disk"_s), qPrintable(saidOf(done)));
        QCOMPARE(pve.guests.value(100).config.value(u"boot"_s), u"order=ide2;scsi0;net0"_s);
        QCOMPARE(device.bootOrderWas(), std::optional<core::BootOrderWas>(was));
        QVERIFY(powerTasks(pve).isEmpty());
        // Out of the drive: its order from before back.
        pve.guests[100].mayDisk = true;
        const base::Result<MediaDone> out = unmounted(device, debian);
        QVERIFY2(out, qPrintable(saidOf(out)));
        QCOMPARE(out.value().note, QString());
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QCOMPARE(pve.guests.value(100).config.value(u"sata0"_s), u"none,media=cdrom"_s);
        QVERIFY(!device.bootOrderWas());
    }

    // Its stop's task running past the wait: whether it was stopped is not known, it was not started from it, and its
    // order is put back.
    void aStopLongerThanItsWaitIsSaidAsPerhapsStopped()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = fast();
        pace.taskWithinMs = 1500;
        pace.confirmWithinMs = 1000;
        ProxmoxDevice device(known(pve), pve.secret, pace);
        pve.taskTakesMs = 60'000;
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("timeout"));
        QVERIFY2(saidOf(done).startsWith(u"The image is mounted, and web may have been stopped, and was not started "
                                         u"from it: Proxmox's task for the command is still running"_s),
                 qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).endsWith(u". Its boot order was put back."_s));
        QCOMPARE(powerTasks(pve), QStringList{u"qmstop"_s});
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
    }

    // Started "until unmounted" and restarted, its settings then not read: started all the same, and said so.
    void aStartUntilUnmountedWhoseReadAfterFailsIsStarted()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        afterTheStart(pve, [&pve] { pve.configFails = 1; });
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"until_unmounted"_s, u"web"_s, true));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QCOMPARE(pve.guests.value(100).booted, QStringList{u"ide2 "_s + debian});
        QVERIFY(device.bootOrderWas());
        pve.afterCommand = nullptr;
        QVERIFY(unmounted(device, debian));
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
    }

    // An order put back, its settings then not read: put back all the same, and said so.
    void aPutBackThatWasDoneIsSaidSoWhateverTheReadAfterIt()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        // (After the start: the read before the order is put back answered, the one after it not.)
        bool started = false;
        afterTheStart(pve, [&started] { started = true; });
        pve.afterConfigRead = [&pve, &started] {
            if (std::exchange(started, false)) {
                pve.configFails = 1;
            }
        };
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY2(done.value().note.startsWith(u"It started from the image. Its next start by Proxmox is from its boot "
                                              u"order as before"_s),
                 qPrintable(done.value().note));
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
    }

    // Not restarted (the token may not), and its order then not put back either (nor may it change that): both said,
    // the guest named in the log, and the order from before kept with the guest, for the unmount to put back (also by a
    // driver made anew, as after a restart of the program).
    void aStartThatDidNotComeAboutAndAnOrderNotPutBackAreBothSaid()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        pve.guests[100].mayPower = false;
        // (Once the image and the order are in: no boot order may be changed any more.)
        pve.afterConfigRead = [&pve] {
            if (pve.changes.size() == 2) {
                pve.guests[100].mayDisk = false;
            }
        };
        logSaysTheOrderOfWebWasNotPutBack();
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QVERIFY(!done);
        QVERIFY2(saidOf(done).startsWith(u"The image is mounted, and web was not started from it: "_s),
                 qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).contains(u"VM.PowerMgmt"_s));
        QVERIFY(saidOf(done).contains(u". Its boot order could not be put back ("_s));
        QVERIFY(saidOf(done).contains(u"VM.Config.Disk"_s));
        QVERIFY(saidOf(done).endsWith(u"it begins with the CD-ROM drive until qKVM puts it back when the image is "
                                      u"unmounted, or it is set on Proxmox."_s));
        QVERIFY(powerTasks(pve).isEmpty());
        QCOMPARE(bootOf(pve.guests.value(100)), u"order=ide2;scsi0;net0"_s);
        QVERIFY(device.bootOrderWas());
        pve.afterConfigRead = nullptr;
        pve.guests[100].mayDisk = true;
        pve.guests[100].mayPower = true;
        ProxmoxDevice anew(keptWith(pve, device.bootOrderWas()), pve.secret, fast());
        const base::Result<MediaDone> out = unmounted(anew, debian);
        QVERIFY2(out, qPrintable(saidOf(out)));
        QCOMPARE(out.value().note, QString());
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        QVERIFY(!anew.bootOrderWas());
    }

    // Stopped, and its start's task then running past the wait while it holds the guest's settings, so that the order
    // cannot be put back: whether it started is not known, and said so, with the order kept for the unmount.
    void aStartLongerThanItsWaitAndAnOrderNotPutBackAreBothSaid()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = fast();
        pace.taskWithinMs = 1500;
        pace.confirmWithinMs = 1000;
        ProxmoxDevice device(known(pve), pve.secret, pace);
        afterTheStop(pve, [&pve] {
            pve.taskTakesMs = 60'000;
            pve.changeRefused = u"can't lock file '/var/lock/qemu-server/lock-100.conf' - got timeout"_s;
        });
        logSaysTheOrderOfWebWasNotPutBack();
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("timeout"));
        QVERIFY(ProxmoxDevice::mayHaveBeenDone(done.error()));
        QVERIFY2(saidOf(done).startsWith(u"The image is mounted, and web was stopped, and may have been started from "
                                         u"it: Proxmox's task for the command is still running"_s),
                 qPrintable(saidOf(done)));
        QVERIFY(saidOf(done).contains(u". Its boot order could not be put back ("_s));
        QVERIFY(saidOf(done).contains(u"can't lock file"_s));
        QCOMPARE(powerTasks(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QVERIFY(device.bootOrderWas());
    }

    // Stopped, and its start then refused by Proxmox: said as stopped and not started again, its order put back.
    void aGuestStoppedAndNotStartedAgainIsSaidSo()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        afterTheStop(pve, [&pve] { pve.taskFails = u"start failed: QEMU exited with code 1"_s; });
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done),
                 u"The image is mounted, and web was stopped, and not started again: Proxmox could not "
                 u"start the guest: start failed: QEMU exited with code 1. Its boot order was put back."_s);
        QCOMPARE(pve.guests.value(100).status, u"stopped"_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
    }

    // Review c9b-3, L2: stopped, and started again by another hand before this program's start (seen stopped by the
    // stop's wait, running at the start's read): not said as started from the image by qKVM, nothing more pressed, and
    // its order put back.
    void aGuestStartedByAnotherHandMeanwhileIsNotSaidAsStartedFromTheImage()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        int reads = -1; // (counted from the stop on)
        afterTheStop(pve, [&reads] { reads = 0; });
        pve.beforeStatusRead = [&pve, &reads] {
            if (reads < 0) {
                return;
            }
            ++reads;
            if (reads == 2) {
                pve.startedByAnotherHand(100);
            }
        };
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QCOMPARE(saidOf(done),
                 u"The image is mounted, and web was stopped, and started again by another hand before "
                 u"qKVM started it: look at it to see what it started from. Its boot order was put back."_s);
        QCOMPARE(powerTasks(pve), QStringList{u"qmstop"_s});
        QCOMPARE(pve.guests.value(100).status, u"running"_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!device.bootOrderWas());
    }

    // The order from before not kept with the guest (the settings could not be written): the order is not changed.
    void anOrderThatCannotBeKeptIsNotChanged()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        device.keepBootOrdersBy([](const std::optional<core::BootOrderWas>& /*was*/) -> base::Result<void> {
            return base::Error{"not_saved", "The guest's boot order from before could not be saved (the disk is "
                                            "full), so nothing was changed."};
        });
        const base::Result<MediaDone> done = mounted(device, startedFrom(debian, u"once"_s));
        QCOMPARE(codeOf(done), std::string("not_saved"));
        QCOMPARE(saidOf(done), u"The image is mounted, and web was not started from it: The guest's boot order from "
                               u"before could not be saved (the disk is full), so nothing was changed."_s);
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QCOMPARE(pve.changes.size(), 1); // (the image alone)
        QVERIFY(powerTasks(pve).isEmpty());
        QVERIFY(!device.bootOrderWas());
    }

    // The image out, and the guest's settings then not read: the order from before kept, and put back at the next
    // unmount.
    void anUnmountWhoseReadFailsKeepsTheOrderFromBefore()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        ProxmoxDevice db(known(pve, 101), pve.secret, fast());
        QVERIFY(mounted(db, startedFrom(debian, u"until_unmounted"_s, u"db"_s)));
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), u"order=ide2;scsi0;net0"_s);
        int reads = 0;
        pve.afterConfigRead = [&pve, &reads] {
            if (++reads == 1) {
                pve.configFails = 1; // (the read after the image is out)
            }
        };
        base::Result<MediaDone> done = unmounted(db, debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY2(done.value().note.startsWith(u"Its boot order could not be read to be put back ("_s),
                 qPrintable(done.value().note));
        // (Said as it is done: an unmount needs an image in the drive, and the guest starts from that one until then:
        // review c9b-4, L9.)
        QVERIFY2(done.value().note.endsWith(u"empty and passed over, until qKVM puts it back at its next start from an "
                                            u"image \"once\", or at the unmount of the next image put in (from which "
                                            u"the guest starts until then), or it is set on Proxmox."_s),
                 qPrintable(done.value().note));
        QVERIFY(!done.value().mounted);
        QCOMPARE(pve.guests.value(101).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), u"order=ide2;scsi0;net0"_s);
        QVERIFY(db.bootOrderWas());
        pve.afterConfigRead = nullptr;
        QVERIFY(mounted(db, mountOf(debian, u"db"_s)));
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), u"order=ide2;scsi0;net0"_s);
        done = unmounted(db, debian);
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(done.value().note, QString());
        QCOMPARE(pve.guests.value(101).config.value(u"boot"_s), firstOrder);
        QVERIFY(!db.bootOrderWas());
    }

    // Through this PC's own wall: the order from before kept in the guest's stored data at once, before the program
    // ends, and put back at the unmount after a restart of the program.
    void aBootOrderFromBeforeIsKeptWithTheGuestAcrossARestartOfTheProgram()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        MemorySecrets secrets;
        QString web;
        {
            core::Settings settings(file);
            LocalWall wall(settings, secrets, quickPaces());
            web = addedTo(wall, pve, 100);
            QVERIFY(!web.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(statusIn(wall, web).capabilities.value_or(QStringList()).contains(u"media"_s),
                                     10'000);
            // (Its settings held by another task after the start: no change of them is taken. The wall's own looks read
            // them meanwhile, and change nothing.)
            afterTheStart(pve, [&pve] { pve.changeRefused = u"can't lock file - got timeout"_s; });
            core::MediaPlace& place = wall;
            const base::Result<MediaDone> done = ofTheWall<MediaDone>(
                [&](auto then) { place.mountMedia(web, onThe(pve, startedFrom(debian, u"once"_s)), std::move(then)); });
            QCOMPARE(codeOf(done), std::string("action_failed"));
            QVERIFY2(saidOf(done).startsWith(u"The image is mounted, and web started from it, and its boot order could "
                                             u"not be put back (Proxmox"_s),
                     qPrintable(saidOf(done)));
            QCOMPARE(bootOf(pve.guests.value(100)), u"order=ide2;scsi0;net0"_s);
            pve.changeRefused.clear();
            // (In the file already, before the program ends.)
            const core::Settings read(file);
            const std::optional<core::BootOrderWas> kept = read.remembered().kvms.value(0).bootOrderWas;
            QVERIFY(kept);
            QCOMPARE(kept.value_or(core::BootOrderWas{}).order, u"scsi0;ide2;net0"_s);
            QCOMPARE(kept.value_or(core::BootOrderWas{}).written, u"ide2;scsi0;net0"_s);
            pve.afterCommand = nullptr;
        }
        core::Settings settings(file);
        QVERIFY(settings.remembered().kvms.value(0).bootOrderWas);
        LocalWall wall(settings, secrets, quickPaces());
        QTRY_VERIFY_WITH_TIMEOUT(statusIn(wall, web).capabilities.value_or(QStringList()).contains(u"media"_s), 10'000);
        core::MediaPlace& place = wall;
        const base::Result<MediaDone> out = ofTheWall<MediaDone>(
            [&](auto then) { place.unmountMedia(web, debian, u"web"_s, QString(), std::move(then)); });
        QVERIFY2(out, qPrintable(saidOf(out)));
        QCOMPARE(out.value().note, QString());
        QCOMPARE(bootOf(pve.guests.value(100)), firstOrder);
        QVERIFY(!wall.kvms().value(0).bootOrderWas);
        QVERIFY(!core::Settings(file).remembered().kvms.value(0).bootOrderWas);
    }

    // The web's test_a_guest_without_a_cd_rom_drive_is_given_one: where Proxmox's page adds one (ide2 first, then the
    // others of IDE and SATA), a running guest's from its next start; and where there is no free place, said.
    void aGuestWithoutACdRomDriveIsGivenOne()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.guests[100].config.remove(u"ide2"_s);
        pve.guests[101].config.insert(u"ide2"_s, u"local-lvm:vm-101-disk-1,size=4G"_s); // (a disk: the drive beside it)
        ProxmoxDevice running(known(pve), pve.secret, fast());
        base::Result<MediaDone> done = mounted(running, mountOf(debian));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY2(done.value().note.contains(u"no CD-ROM drive: one was added (ide2)"_s), qPrintable(done.value().note));
        QVERIFY(done.value().note.contains(u"next start"_s));
        QCOMPARE(pve.guests.value(100).pending.value(u"ide2"_s), debian + u",media=cdrom"_s);
        QVERIFY(!pve.guests.value(100).config.contains(u"ide2"_s));
        ProxmoxDevice stopped(known(pve, 101), pve.secret, fast());
        done = mounted(stopped, mountOf(debian, u"db"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(pve.guests.value(101).config.value(u"ide0"_s), debian + u",media=cdrom"_s);
        QVERIFY(pve.guests.value(101).config.value(u"ide2"_s).startsWith(u"local-lvm:"_s));
        // No free place: IDE and SATA all disks.
        Guest full;
        full.name = u"full"_s;
        full.config.clear();
        for (const QString& slot : {u"ide0"_s, u"ide1"_s, u"ide2"_s, u"ide3"_s, u"sata0"_s, u"sata1"_s, u"sata2"_s,
                                    u"sata3"_s, u"sata4"_s, u"sata5"_s}) {
            full.config.insert(slot, u"local-lvm:vm-102-%1,size=1G"_s.arg(slot));
        }
        pve.guests.insert(102, full);
        ProxmoxDevice none(known(pve, 102), pve.secret, fast());
        const qsizetype sent = pve.changes.size();
        done = mounted(none, mountOf(debian, u"full"_s));
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(saidOf(done).contains(u"no free place for one (ide0 to ide3, sata0 to sata5)"_s),
                 qPrintable(saidOf(done)));
        QCOMPARE(pve.changes.size(), sent);
    }

    // The web's test_what_the_token_lacks_is_said_in_proxmoxs_words_and_nothing_else_is_done, its mount.
    void whatTheTokenLacksIsSaidInProxmoxsWordsAndNothingIsDone()
    {
        ProxmoxByHand pve(proxmoxTls());
        Storage hidden;
        hidden.images = {{u"secret-install.iso"_s, 1}};
        hidden.maySee = false;
        pve.storages.insert(u"hidden"_s, hidden);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QCOMPARE(codeOf(mounted(device, mountOf(u"hidden:iso/secret-install.iso"_s))), std::string("changed"));
        pve.guests[100].mayCdrom = false;
        // (Its words in its status line, as its API server says them; and in its body, as its other format does.)
        for (const bool inTheBody : {false, true}) {
            pve.wordsInTheBody = inTheBody;
            const base::Result<MediaDone> done = mounted(device, mountOf(debian));
            QVERIFY(!done);
            QVERIFY2(saidOf(done).contains(u"VM.Config.CDROM"_s), qPrintable(saidOf(done)));
            QVERIFY(
                saidOf(done).startsWith(u"Proxmox does not let the token put an image in the guest's CD-ROM drive"_s));
            QVERIFY(!saidOf(done).contains(pve.secret));
        }
        QVERIFY(pve.changes.isEmpty());
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        QVERIFY(fetchedOrSent(pve).isEmpty());
    }

    // The web's test_another_vm_under_the_guests_number_is_sent_nothing.
    void anotherVmUnderTheGuestsNumberIsSentNothing()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QVERIFY(mounted(device, mountOf(debian)));
        pve.deleteGuest(100);
        Guest intruder;
        intruder.name = u"intruder"_s;
        QCOMPARE(pve.makeGuest(intruder),
                 qint64{100}); // (Proxmox gives it the lowest number that is free: the guest's)
        const qsizetype storagesAsked = pve.asked(u"GET /nodes/pve1/storage"_s);
        QCOMPARE(codeOf(mediaOf(device)), std::string("another_machine"));
        QCOMPARE(codeOf(mounted(device, mountOf(alpine))), std::string("another_machine"));
        QCOMPARE(codeOf(unmounted(device, debian)), std::string("another_machine"));
        QCOMPARE(codeOf(mounted(device, startedFrom(alpine, u"once"_s))), std::string("another_machine"));
        QVERIFY(powerTasks(pve).isEmpty());
        QCOMPARE(pve.guests.value(100).config.value(u"boot"_s), firstOrder);
        QCOMPARE(pve.asked(u"GET /nodes/pve1/storage"_s), storagesAsked); // (nothing of it is listed)
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        QCOMPARE(pve.changes.size(), 1); // (the first mount, of the guest itself)
        const core::Status status = lookedAt(device);
        QVERIFY(!status.media);
        QCOMPARE(status.errorCode, u"another_machine"_s);
    }

    // Outside review of 0.2.0, 3.3, for a change of its settings (review cx3-1, L6): a change refused 401 (the token
    // taken back for a moment) is sent again only once which virtual machine has the guest's number was asked again.
    // Another made under it meanwhile is sent nothing a second time, not even a change its digest would have refused.
    void aChangeRefusedIsSentAgainOnlyToTheGuestItself()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const QString put = u"PUT /nodes/pve1/qemu/100/config"_s;
        bool once = false;
        pve.refuses = [&pve, &once, &put](const QString& said) {
            if (said != put || std::exchange(once, true)) {
                return false;
            }
            pve.deleteGuest(100);
            Guest intruder;
            intruder.name = u"intruder"_s;
            pve.makeGuest(intruder);
            return true;
        };
        QCOMPARE(codeOf(mounted(device, mountOf(debian))), std::string("another_machine"));
        QCOMPARE(pve.refusedPaths, QStringList{put});
        QCOMPARE(pve.asked(put), 0);
        QVERIFY(pve.changes.isEmpty());
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
    }

    // Review c9b-1, L5: a look that is out while another question (here its media) finds another virtual machine under
    // the guest's number says that, and nothing of what it read of that one.
    void aLookThatCrossesAVerdictSaysNothingOfTheOther()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        pve.deleteGuest(100);
        Guest intruder;
        intruder.name = u"intruder"_s;
        intruder.cpus = 16;
        pve.makeGuest(intruder);
        pve.statusTakesMs = 1500;
        QSignalSpy over(&device, &ProxmoxDevice::looked);
        device.look();
        QCOMPARE(codeOf(mediaOf(device)), std::string("another_machine"));
        QVERIFY(over.wait(10'000));
        const core::Status status = device.status();
        QCOMPARE(status.errorCode, u"another_machine"_s);
        QVERIFY(!status.server || status.server->processors != 16);
        QVERIFY(status.state != u"online"_s);
    }

    // Rule 30 right before the change: another virtual machine made under the guest's number after its images were
    // listed for the mount is sent nothing.
    void whichVmHasTheNumberIsAskedRightBeforeTheChange()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        bool swapped = false;
        pve.afterImagesListed = [&pve, &swapped] {
            if (std::exchange(swapped, true)) {
                return;
            }
            pve.deleteGuest(100);
            Guest intruder;
            intruder.name = u"intruder"_s;
            pve.makeGuest(intruder);
        };
        const base::Result<MediaDone> done = mounted(device, mountOf(debian));
        QCOMPARE(codeOf(done), std::string("another_machine"));
        QVERIFY(changesSent(pve).isEmpty());
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        // And for taking one out: made after the drive was read for it.
        ProxmoxByHand other(proxmoxTls());
        other.guests[100].config.insert(u"ide2"_s, debian + u",media=cdrom"_s);
        ProxmoxDevice second(known(other), other.secret, fast());
        QVERIFY(mediaOf(second));
        bool made = false;
        other.afterConfigRead = [&other, &made] {
            if (std::exchange(made, true)) {
                return;
            }
            const Guest was = other.guests.value(100);
            other.deleteGuest(100);
            Guest intruder = was;
            intruder.uuid = ProxmoxByHand::fresh();
            other.makeGuest(intruder);
        };
        // (Its settings read and judged: the guest's. Then made anew under the number, its drive as the guest's was:
        // its digest is another's, Proxmox refuses the change, and read again it is judged another virtual machine.)
        const base::Result<MediaDone> out = unmounted(second, debian);
        QCOMPARE(codeOf(out), std::string("another_machine"));
        QVERIFY(other.changes.isEmpty());
        QCOMPARE(other.guests.value(100).config.value(u"ide2"_s), debian + u",media=cdrom"_s);
    }

    // The web's test_settings_changed_on_proxmox_between_the_read_and_the_change_are_not_written_over.
    void settingsChangedMeanwhileAreNotWrittenOver_data() { digestWords(); }
    void settingsChangedMeanwhileAreNotWrittenOver()
    {
        QFETCH(QString, words);
        ProxmoxByHand pve(proxmoxTls());
        pve.digestSays = words;
        ProxmoxDevice device(known(pve), pve.secret, fast());
        int reads = 0;
        pve.afterConfigRead = [&pve, &reads] {
            ++reads;
            pve.guests[100].config.insert(u"net0"_s, u"virtio=BC:24:11:5E:1A:01,bridge=vmbr%1"_s.arg(reads));
        };
        const base::Result<MediaDone> done = mounted(device, mountOf(debian));
        QCOMPARE(codeOf(done), std::string("changed"));
        QCOMPARE(saidOf(done), u"The guest's settings were changed on Proxmox meanwhile, and nothing was changed here. "
                               u"Look at it again."_s);
        QVERIFY(pve.changes.isEmpty());
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"none,media=cdrom"_s);
        QCOMPARE(changesSent(pve).size(), 1); // (sent once, with the digest of what was read for it)
    }

    // A change whose answer was lost on its way back may have been done, and is said so; it is not sent twice.
    void aChangeWhoseAnswerWasLostMayHaveBeenDone()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.configAnswer = 596;
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaDone> done = mounted(device, mountOf(debian));
        QCOMPARE(codeOf(done), std::string("timeout"));
        QVERIFY2(saidOf(done).contains(u"it may have been done"_s), qPrintable(saidOf(done)));
        QVERIFY(ProxmoxDevice::mayHaveBeenDone(done.error()));
        QCOMPARE(changesSent(pve).size(), 1);
        QCOMPARE(pve.changes.size(), 1);
    }

    // An image's name goes to Proxmox as it is: a plus in it is no blank, and a comma of the drive's setting stays in
    // its value.
    void anImageWithAPlusInItsNameGoesAsItIs()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.storages[u"local"_s].images.insert(u"tools+drivers.iso"_s, 1'000'000);
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaDone> done = mounted(device, mountOf(u"local:iso/tools+drivers.iso"_s));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), u"local:iso/tools+drivers.iso,media=cdrom"_s);
    }

    // Qt asks a PUT once more by itself where the line falls before its answer (device_asker.h): the first carried
    // out, the second is refused for its digest; read again, the drive holds what was asked, and it is done.
    void aChangeAskedTwiceByQtIsDoneOnce_data() { digestWords(); }
    void aChangeAskedTwiceByQtIsDoneOnce()
    {
        QFETCH(QString, words);
        ProxmoxByHand pve(proxmoxTls());
        pve.digestSays = words;
        pve.configHangsUp = 1;
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaDone> done = mounted(device, mountOf(debian));
        QVERIFY2(done, qPrintable(saidOf(done)));
        QVERIFY(done.value().mounted);
        QCOMPARE(inThe(done.value().mounted).image, debian);
        QCOMPARE(changesSent(pve).size(), 2);
        QCOMPARE(pve.changes, QStringList{u"100 ide2="_s + debian + u",media=cdrom"_s});
    }

    // Review c9b-2, L1: refused for its digest, and its settings then not read again: whether the first of the two was
    // carried out is not known, and it may have been done.
    void aReadBackThatFailsAfterADigestRefusalMayHaveBeenDone()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.configHangsUp = 1;
        ProxmoxDevice device(known(pve), pve.secret, fast());
        // (The settings read for the listing, and right before the change; the one after the refusal is not answered.)
        int reads = 0;
        pve.afterConfigRead = [&pve, &reads] {
            if (++reads == 2) {
                pve.configFails = 1;
            }
        };
        const base::Result<MediaDone> done = mounted(device, mountOf(debian));
        QCOMPARE(codeOf(done), std::string("timeout"));
        QVERIFY2(saidOf(done).contains(u"it may have been done. Look at the guest before trying again."_s),
                 qPrintable(saidOf(done)));
        QVERIFY(ProxmoxDevice::mayHaveBeenDone(done.error()));
        QVERIFY(!saidOf(done).contains(pve.secret));
        QCOMPARE(changesSent(pve).size(), 2);
        QCOMPARE(pve.changes, QStringList{u"100 ide2="_s + debian + u",media=cdrom"_s}); // (and it was)
    }

    // A guest's images are Proxmox's: none is deleted or uploaded from here.
    void noImageIsDeletedOrUploaded()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(known(pve), pve.secret, fast());
        const base::Result<MediaDone> deleted =
            answerOf<MediaDone>([&device](std::function<void(base::Result<MediaDone>)> done) {
                device.deleteMedia(debian, std::move(done));
            });
        QCOMPARE(codeOf(deleted), std::string("invalid"));
        auto file = std::make_unique<QBuffer>();
        file->setData(QByteArray(1024, 'x'));
        file->open(QIODevice::ReadOnly);
        const base::Result<core::MediaUploaded> uploaded = answerOf<core::MediaUploaded>(
            [&device, &file](std::function<void(base::Result<core::MediaUploaded>)> done) {
                static_cast<void>(device.uploadMedia(u"x.iso"_s, std::move(file), std::move(done)));
            });
        QCOMPARE(codeOf(uploaded), std::string("invalid"));
        QVERIFY(!device.uploading());
        QVERIFY(pve.requests.isEmpty());
    }

    // Through this PC's own wall, as its server does it (server.py: media_target, media_named, media_held): the guest
    // named as the question named it, a box bound to the guest it listed, one thing at a time, and what a guest's
    // drive does not offer refused in the server's words.
    void aGuestsMediaThroughTheWall()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.powerTakesMs = 1500; // (a start seen a moment after its task)
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        LocalWall wall(settings, secrets, quickPaces());
        const QString web = addedTo(wall, pve, 100);
        const QString db = addedTo(wall, pve, 101);
        QVERIFY(!web.isEmpty() && !db.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(statusIn(wall, web).capabilities.value_or(QStringList()).contains(u"media"_s), 10'000);
        core::MediaPlace& place = wall;
        const base::Result<MediaList> list =
            ofTheWall<MediaList>([&](auto done) { place.askMedia(web, std::move(done)); });
        QVERIFY2(list, list ? "" : list.error().message.c_str());
        QCOMPARE(list.value().images.size(), 2);
        QVERIFY(!list.value().boundTo.isEmpty());
        // Two guests of one Proxmox share its address and its certificate: a box of one changes nothing of the other.
        const base::Result<MediaList> other =
            ofTheWall<MediaList>([&](auto done) { place.askMedia(db, std::move(done)); });
        QVERIFY(other);
        QVERIFY(other.value().boundTo != list.value().boundTo);
        QVERIFY(!place.movedSince(web, list.value().boundTo));
        QCOMPARE(place.movedSince(db, list.value().boundTo).value_or(base::Error{}).code, std::string("changed"));
        // Named otherwise than it is called: nothing is done.
        base::Result<MediaDone> done = ofTheWall<MediaDone>(
            [&](auto then) { place.mountMedia(web, onThe(pve, mountOf(debian, u"db"_s)), std::move(then)); });
        QCOMPARE(codeOf(done), std::string("changed"));
        QVERIFY(saidOf(done).contains(u"called web"_s));
        QVERIFY(pve.changes.isEmpty());
        done = ofTheWall<MediaDone>(
            [&](auto then) { place.mountMedia(web, onThe(pve, mountOf(debian)), std::move(then)); });
        QVERIFY2(done, qPrintable(saidOf(done)));
        QTRY_VERIFY_WITH_TIMEOUT(statusIn(wall, web).media.has_value(), 10'000);
        QCOMPARE(inThe(statusIn(wall, web).media).name, u"debian-13.iso"_s);
        // What a guest's drive does not offer, in the server's words.
        done = ofTheWall<MediaDone>(
            [&](auto then) { place.deleteMedia(web, debian, u"web"_s, QString(), std::move(then)); });
        QCOMPARE(codeOf(done), std::string("invalid"));
        QCOMPARE(saidOf(done), u"web has no images of its own that qKVM deletes"_s);
        auto file = std::make_unique<QBuffer>();
        file->setData(QByteArray(1024, 'x'));
        file->open(QIODevice::ReadOnly);
        const base::Result<core::MediaUploaded> uploaded =
            ofTheWall<core::MediaUploaded>([&](std::function<void(base::Result<core::MediaUploaded>)> then) {
                static_cast<void>(
                    place.uploadMedia(web, u"x.iso"_s, u"web"_s, QString(), std::move(file), std::move(then)));
            });
        QCOMPARE(codeOf(uploaded), std::string("invalid"));
        QCOMPARE(QString::fromStdString(uploaded.error().message), u"Images are not carried through qKVM to web"_s);
        // One thing at a time: not while its power button is pressed (the web's test_one_thing_at_a_time_on_a_machine).
        std::optional<base::Result<core::Done>> started;
        wall.act(db, core::Action::plain(u"power_on"_s),
                 [&started](base::Result<core::Done> came) { started = std::move(came); });
        QTRY_VERIFY_WITH_TIMEOUT(pve.tasks.size() == 1, 10'000);
        done = ofTheWall<MediaDone>(
            [&](auto then) { place.mountMedia(db, onThe(pve, mountOf(debian, u"db"_s), 101), std::move(then)); });
        QCOMPARE(codeOf(done), std::string("busy"));
        QTRY_VERIFY_WITH_TIMEOUT(started.has_value(), 20'000);
        const base::Result<core::Done> start =
            started.value_or(base::Result<core::Done>(base::Error{"never", "The press was never answered."}));
        QVERIFY2(start, start ? "" : start.error().message.c_str());
        QCOMPARE(pve.changes.size(), 1);
        done = ofTheWall<MediaDone>(
            [&](auto then) { place.mountMedia(db, onThe(pve, mountOf(debian, u"db"_s), 101), std::move(then)); });
        QVERIFY2(done, qPrintable(saidOf(done)));
        QCOMPARE(pve.changes.size(), 2);
    }
};

QTEST_MAIN(TestProxmoxMedia)
#include "test_proxmox_media.moc"
