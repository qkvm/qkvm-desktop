#include <qkvm/core/proxmox_device.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include <QCryptographicHash>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <QtLogging>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/proxmox_answers.h>

// A Proxmox guest's virtual media (ProxmoxDevice, OwnMedia; c9b, wave 2, step 7): the web's "virtual media" of 0.3.0
// (proxmox.py: media_images, media_mount, media_boot, _order_back, _restart, media_unmount, _this_guest, _change). An
// ISO image already on one of Proxmox's storages goes into the guest's CD-ROM drive by its settings (`ide2:
// local:iso/x.iso,media=cdrom`) and out of it (`none,media=cdrom`), and the guest is started from it by its boot order
// (`boot: order=ide2;scsi0;net0`), once or until it is unmounted (review c9b-2, M1). Nothing moves: no download-url
// (Proxmox would fetch an address, and with it probe its own network), no upload, no deletion.

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

base::Error invalid(const QString& why)
{
    return {"invalid", why.toStdString()};
}

base::Error changedMeanwhile(const QString& why)
{
    return {"changed", why.toStdString()};
}

// What its CD-ROM drive holds where it is not the image a start from it was confirmed for (outside review of 0.2.0,
// 3.4): put there, or taken out, by another hand since.
base::Error notTheOneConfirmed(const std::optional<proxmox::Cd>& cd)
{
    if (!cd || cd->disc == "none"_L1) {
        return changedMeanwhile(
            u"The image was taken out of its CD-ROM drive by another hand since: look at it before trying again."_s);
    }
    const std::optional<proxmox::IsoVolume> iso = proxmox::isoVolumeOf(cd->disc);
    return changedMeanwhile(u"Another image is in its CD-ROM drive now (%1), put there by another hand since: look at "
                            u"it before trying again."_s.arg(iso ? iso->name : kvmd::plain(QJsonValue(cd->disc), 120)));
}

// What a drive is called in a sentence (server.py MEDIA_DRIVE_SAID).
QString driveCalled(const QString& drive)
{
    return drive == "cd"_L1 ? u"a CD-ROM drive"_s : drive == "flash"_L1 ? u"a USB flash drive"_s : u"a disk"_s;
}

// What a guest's drive can do here (the web's MEDIA): an image of Proxmox's in its CD-ROM drive, read-only, started
// from once or until it is unmounted, and the guest restarted for that.
MediaOffer offeredHere()
{
    MediaOffer offer;
    offer.drives = {u"cd"_s};
    offer.boot = {u"once"_s, u"until_unmounted"_s};
    offer.restart = true;
    offer.where = u"proxmox"_s;
    return offer;
}

// A boot order as the guest's settings write one.
QString orderSetting(const QStringList& order)
{
    return u"order="_s + order.join(u';');
}

// What is said of a start from the image, as far as it came (the web's words, and the desktop's own where the web says
// none: review c9b-3, M1).
const QString& untilUnmounted()
{
    static const QString said =
        u"It starts from the image at its next start by Proxmox, until the image is unmounted."_s;
    return said;
}
const QString& asBefore()
{
    static const QString said =
        u"Its next start by Proxmox is from its boot order as before; a restart from inside the "
        u"guest starts from the image again while it is mounted."_s;
    return said;
}
const QString& changedOnProxmoxSince()
{
    static const QString said = u"Its boot order was changed on Proxmox meanwhile, and is left as it is."_s;
    return said;
}
// What follows an order not put back at an unmount, as the program does it: an unmount needs an image in the drive,
// and a mount that leaves the order as it is starts the guest from that image until then (review c9b-4, L9).
const QString& untilPutBack()
{
    static const QString said =
        u"until qKVM puts it back at its next start from an image \"once\", or at the unmount of the next image put "
        u"in (from which the guest starts until then), or it is set on Proxmox."_s;
    return said;
}

// A form's fields as Proxmox reads them: each name and value %-encoded whole, so that a plus in an image's name goes as
// what it is and not as a blank, and a comma or an equals sign of a drive's setting stays in its value.
QByteArray formOf(const QList<std::pair<QString, QString>>& fields)
{
    QByteArray form;
    for (const auto& [name, value] : fields) {
        if (!form.isEmpty()) {
            form += '&';
        }
        form += QUrl::toPercentEncoding(name) + '=' + QUrl::toPercentEncoding(value);
    }
    return form;
}

// Whether the guest's settings hold each change asked (a drive: its disc, as a CD-ROM drive, whatever Proxmox wrote
// beside it, its size say; its boot order: that order, as it is read for its next start).
bool holds(const QJsonObject& now, const QList<std::pair<QString, QString>>& changes)
{
    return std::ranges::all_of(changes, [&now](const std::pair<QString, QString>& change) {
        if (change.first == "boot"_L1) {
            return now.value(change.first) == QJsonValue(change.second);
        }
        const std::optional<proxmox::Drive> there = proxmox::driveOf(now.value(change.first));
        const std::optional<proxmox::Drive> asked = proxmox::driveOf(QJsonValue(change.second));
        return there && asked && there->disc == asked->disc &&
               there->options.value(u"media"_s) == asked->options.value(u"media"_s);
    });
}

// Proxmox's words where a change was refused because the settings file is not what was read for it (the web's
// CHANGED_MEANWHILE).
bool changedOnProxmox(const QString& said)
{
    static const QRegularExpression words(
        u"checksum mismatch|file change by other user|detected modified configuration"_s,
        QRegularExpression::CaseInsensitiveOption);
    return words.match(said).hasMatch();
}

} // namespace

struct ProxmoxDevice::Gathering {
    QString node; // where the storages are listed: the guest's node when they were
    QStringList storages;
    qsizetype next = 0;
    qint64 untilMs = 0; // (this driver's clock) the moment by which the listing is over, storages not asked by then
    MediaList list;
    std::function<void(base::Result<MediaList>, const QString& node)> done;
};

// A start from the image under way: what was asked, the guest's boot order before any of this program's and with the
// drive first, and who waits.
struct ProxmoxDevice::Boot {
    QString how; // "once", "until_unmounted"
    bool restart = false;
    QString named;             // the guest, as the question named it
    QString image;             // the image it was confirmed for, which it is started from and from no other
    bool putBackFirst = false; // the order from before was put back first, its record standing (outside review, 3.6)
    QStringList own;
    QStringList first;
    std::function<void(base::Result<QString>)> done;
};

QString ProxmoxDevice::listedMark(const Where& where, const QString& node)
{
    // (Of its Proxmox, its number and the node: nothing of the token. 32 hex digits, a server's mark's shape.)
    const QByteArray of = "qKVM: the images of a Proxmox guest\n" +
                          where.address.toString(QUrl::FullyEncoded).toUtf8() + '\n' + QByteArray::number(where.vmid) +
                          '\n' + node.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(of, QCryptographicHash::Sha256).toHex().first(32));
}

void ProxmoxDevice::keepBootOrdersBy(BootOrderKeeper keeper)
{
    m_keepBoot = std::move(keeper);
}

std::optional<BootOrderWas> ProxmoxDevice::keptBootOrder() const
{
    // (Of this guest alone: another number, or another number it gives for itself, is another virtual machine's.)
    if (m_bootWas && m_bootWas->vmid == m_where.vmid && m_bootWas->machine == m_machine) {
        return m_bootWas;
    }
    return std::nullopt;
}

base::Result<void> ProxmoxDevice::keepBootOrder(const std::optional<BootOrderWas>& was)
{
    // Kept with the guest first (the wall: its stored data); what could not be kept there is not taken for kept here.
    if (m_keepBoot) {
        if (const base::Result<void> kept = m_keepBoot(was); !kept) {
            return kept.error();
        }
    }
    m_bootWas = was;
    return {};
}

std::optional<MediaOffer> ProxmoxDevice::mediaOffer() const
{
    return offeredHere(); // (every guest: what it has is read at each question, and said there)
}

std::optional<MediaUploading> ProxmoxDevice::uploading() const
{
    return std::nullopt;
}

void ProxmoxDevice::driveFrom(const QJsonValue& config)
{
    m_drive = proxmox::mountedIn(config);
}

std::optional<MountedMedia> ProxmoxDevice::mediaNow() const
{
    // (Nothing of another virtual machine's is said as the guest's.)
    return m_other ? std::nullopt : m_drive;
}

void ProxmoxDevice::thisGuest(std::function<void(base::Result<QJsonObject>)> done)
{
    Ask asked;
    asked.path = u"config"_s;
    ofTheGuest(asked, u"read this guest's settings"_s,
               [this, done = std::move(done)](const base::Result<QJsonValue>& config) {
                   if (!config) {
                       done(config.error());
                       return;
                   }
                   if (!config.value().isObject()) {
                       done(base::Error{"bad_response", "Proxmox did not say the guest's settings"});
                       return;
                   }
                   if (const std::optional<base::Error> refused = judge(proxmox::ownNumber(config.value()))) {
                       done(*refused);
                       return;
                   }
                   driveFrom(config.value());
                   done(config.value().toObject());
               });
}

void ProxmoxDevice::askMedia(std::function<void(base::Result<MediaList>)> done)
{
    listMedia(
        [done = std::move(done)](base::Result<MediaList> list, const QString& /*node*/) { done(std::move(list)); });
}

void ProxmoxDevice::listMedia(std::function<void(base::Result<MediaList>, const QString& node)> done)
{
    // The guest's settings first, judged to be the guest's, and on the node it is on now: its images are on that
    // node's storages.
    thisGuest([this, done = std::move(done)](const base::Result<QJsonObject>& config) mutable {
        if (!config) {
            done(config.error(), {});
            return;
        }
        const QString node = m_where.node;
        Ask asked;
        asked.path = u"/nodes/%1/storage"_s.arg(node);
        asked.query.addQueryItem(u"content"_s, u"iso"_s);
        asked.query.addQueryItem(u"enabled"_s, u"1"_s);
        request(asked, [this, node, done = std::move(done)](const base::Result<Reply>& reply) mutable {
            if (!reply) {
                done(reply.error(), node);
                return;
            }
            const base::Result<QJsonValue> listed = proxmox::dataOf(
                answerIn(reply.value()), u"list the storages of the guest's node", m_where.user, m_secret.view());
            if (!listed) {
                done(listed.error(), node);
                return;
            }
            const proxmox::Storages storages = proxmox::storagesIn(listed.value());
            auto gathering = std::make_shared<Gathering>();
            gathering->node = node;
            gathering->storages = storages.names;
            // One bound for the images of them all: a few storages that do not answer (NFS, say) no longer hold a
            // Media box and a mount, and the guest's power buttons with it, for one question's time each. (The last
            // asked is given a connection's time and half a second at the least, request(): so they are listed within
            // that much past it. SPEC.md 6.4 says it whole. Review c9b-3, L1.)
            gathering->untilMs = now() + m_pace.connectWithinMs + m_pace.quietMs + m_pace.transferMs;
            gathering->list.unseen = storages.unseen;
            gathering->list.offers = offeredHere();
            gathering->done = std::move(done);
            imagesOn(gathering);
        });
    });
}

void ProxmoxDevice::imagesOn(const std::shared_ptr<Gathering>& gathering)
{
    if (gathering->next < gathering->storages.size() && now() >= gathering->untilMs) {
        // The time for the listing is over: the storages not asked yet are said, and not asked.
        for (const QString& left : gathering->storages.sliced(gathering->next)) {
            gathering->list.unseen.append(
                u"%1: not listed: listing its node's storages took all the time there is for it"_s.arg(left));
        }
        gathering->next = gathering->storages.size();
    }
    if (gathering->next >= gathering->storages.size()) {
        std::ranges::sort(gathering->list.images, [](const MediaImage& one, const MediaImage& other) {
            const int byName = one.name.compare(other.name, Qt::CaseInsensitive);
            return byName != 0 ? byName < 0 : one.where < other.where;
        });
        gathering->list.mounted = mediaNow();
        // (Of the node its images were listed on: what is picked from it goes in on that node alone.)
        gathering->list.listed = listedMark(m_where, gathering->node);
        const auto done = std::exchange(gathering->done, nullptr);
        done(std::move(gathering->list), gathering->node);
        return;
    }
    // One storage at a time: a slow one (NFS, say) is waited for alone, and one that does not answer is said.
    const QString storage = gathering->storages.at(gathering->next++);
    Ask asked;
    asked.path = u"/nodes/%1/storage/%2/content"_s.arg(gathering->node, storage);
    asked.query.addQueryItem(u"content"_s, u"iso"_s);
    asked.vital = false;
    asked.largest = proxmox::largestListBytes;
    asked.untilMs = gathering->untilMs;
    request(asked, [this, gathering, storage](const base::Result<Reply>& reply) {
        const base::Result<QJsonValue> found =
            reply ? proxmox::dataOf(answerIn(reply.value()), u"list the images on " + storage, m_where.user,
                                    m_secret.view())
                  : base::Result<QJsonValue>(reply.error());
        if (!found) {
            gathering->list.unseen.append(storage + u": "_s + QString::fromStdString(found.error().message));
        } else {
            const proxmox::Images images =
                proxmox::imagesIn(found.value(), storage, mostMediaImages - gathering->list.images.size());
            gathering->list.images.append(images.images);
            gathering->list.leftOut += images.leftOut;
        }
        imagesOn(gathering);
    });
}

void ProxmoxDevice::changeSettings(const QJsonObject& config, const QList<std::pair<QString, QString>>& changes,
                                   const QString& what, std::function<void(base::Result<void>)> done)
{
    const QString digest = proxmox::digestOf(config);
    if (digest.isEmpty()) {
        later([done = std::move(done)] {
            done(base::Error{"bad_response", "Proxmox did not say which version of the guest's settings it read"});
        });
        return;
    }
    QList<std::pair<QString, QString>> fields = changes;
    fields.append({u"digest"_s, digest});
    Ask asked;
    asked.method = QByteArrayLiteral("PUT");
    asked.path = guestPath(u"config"_s);
    asked.body = formOf(fields);
    // (Not sent twice: on the strength of the digest, a second one after a first that was carried out is refused.)
    asked.doesSomething = true;
    request(asked, [this, changes, what, done = std::move(done)](const base::Result<Reply>& reply) {
        if (!reply) {
            done(reply.error());
            return;
        }
        const proxmox::Answer answer = answerIn(reply.value());
        if (answer.status == 500 && changedOnProxmox(proxmox::saidBy(answer, m_where.user, m_secret.view()))) {
            // Qt asks a PUT once more by itself where the line fell before its answer (device_asker.h): the first may
            // have been carried out, and the second is then refused for it. Read again, judged: where the settings
            // hold what was asked, it is done; otherwise they were changed meanwhile, and nothing was changed here.
            thisGuest([changes, done](const base::Result<QJsonObject>& now) {
                if (!now) {
                    // Not read again: whether the first of the two was carried out is not known (review c9b-2, L1).
                    // Another virtual machine under the number is said as such.
                    done(now.error().code == "another_machine"
                             ? now.error()
                             : base::Error{"timeout", "Proxmox refused the change for the version of the guest's "
                                                      "settings it was made on, and they could not be read again "
                                                      "since (" +
                                                          now.error().message +
                                                          "): it may have been done. Look at the guest before trying "
                                                          "again."});
                    return;
                }
                if (holds(now.value(), changes)) {
                    done(base::Result<void>());
                    return;
                }
                done(changedMeanwhile(u"The guest's settings were changed on Proxmox meanwhile, and nothing was "
                                      u"changed here. Look at it again."_s));
            });
            return;
        }
        const base::Result<QJsonValue> said = proxmox::dataOf(answer, what, m_where.user, m_secret.view());
        if (said) {
            done(base::Result<void>());
            return;
        }
        if (proxmox::faultOf(answer) == proxmox::Fault::AnswerLost) {
            done(base::Error{"timeout", "Proxmox's answer did not come back (" + said.error().message +
                                            "): it may have been done. Look at the guest before trying again."});
            return;
        }
        done(said.error());
    });
}

void ProxmoxDevice::mountMedia(const MediaMount& mount, std::function<void(base::Result<MediaDone>)> done)
{
    // As the server takes it (server.py, media_mount), by what a guest's drive can do here; and then as the driver
    // takes it (proxmox.py, media_mount).
    const QString& named = mount.machine;
    const MediaOffer offer = offeredHere();
    const bool restart = mount.restart || mount.boot == "once"_L1;
    std::optional<base::Error> refused;
    if (mount.drive != "cd"_L1 && mount.drive != "flash"_L1 && mount.drive != "disk"_L1) {
        refused = invalid(u"%1 takes no image as that"_s.arg(named));
    } else if (!offer.drives.contains(mount.drive)) {
        refused = invalid(u"%1 takes no image as %2"_s.arg(named, driveCalled(mount.drive)));
    } else if (mount.writable && !offer.writable.contains(mount.drive)) {
        refused = invalid(u"%1 takes an image as %2 read-only, and only so"_s.arg(named, driveCalled(mount.drive)));
    } else if (mount.boot != "none"_L1 && !offer.boot.contains(mount.boot)) {
        refused = invalid(u"%1 cannot be told to start from an image that way"_s.arg(named));
    } else if (mount.restart && mount.boot == "none"_L1 && !offer.boot.isEmpty()) {
        refused = invalid(u"A restart that does not start from the image is a power button's: use the power buttons"_s);
    } else if (restart && !offer.restart) {
        refused = invalid(u"%1 cannot be restarted from here"_s.arg(named));
    } else if (!proxmox::isoVolumeOf(mount.image)) {
        refused = invalid(u"That is not an image of Proxmox's that qKVM lists"_s);
    }
    if (refused) {
        later([done = std::move(done), why = *refused] { done(why); });
        return;
    }
    // Only an image listed now, from the storages the token may see: nothing else is handed to Proxmox.
    listMedia([this, mount, restart, done = std::move(done)](const base::Result<MediaList>& listed,
                                                             const QString& listedOn) mutable {
        if (!listed) {
            done(listed.error());
            return;
        }
        // Picked from a list of the images of the node the guest is on now, and of no other (outside review of 0.2.0,
        // 3.5): a guest moved to another node of its cluster since the box listed them may find an image of that name
        // on a storage of that name there, which the person never saw. (The web takes what this listing finds.)
        if (mount.listed != listedMark(m_where, listedOn)) {
            done(changedMeanwhile(
                mount.listed.isEmpty()
                    ? u"The image was not picked from a list of the guest's images: nothing was changed. List "
                      u"them, and pick it there."_s
                    : u"The image was picked from the images of another node than the one the guest is on now "
                      u"(%1): nothing was changed. List its images again."_s.arg(listedOn)));
            return;
        }
        if (std::ranges::none_of(listed.value().images,
                                 [&mount](const MediaImage& one) { return one.id == mount.image; })) {
            done(changedMeanwhile(
                u"%1 is not on the storages of the guest's node that the token may see: look again."_s.arg(
                    proxmox::isoVolumeOf(mount.image).value_or(proxmox::IsoVolume{}).name)));
            return;
        }
        put(mount, listedOn, [this, mount, restart, done = std::move(done)](const base::Result<MediaDone>& in) {
            if (!in || mount.boot == "none"_L1) {
                done(in);
                return;
            }
            // Started from it, as asked: what fails of that is said after what was done, and how far the start came
            // (the web's media_failed, "started from it"; review c9b-3, M1).
            mediaBoot(mount.boot, restart, mount.machine, mount.image,
                      [self = QPointer<ProxmoxDevice>(this), mounted = in.value(),
                       done](const base::Result<QString>& booted) {
                          if (!booted) {
                              done(booted.error());
                              return;
                          }
                          QStringList notes{mounted.note, booted.value()};
                          notes.removeAll(QString());
                          done(MediaDone{notes.join(u' '), self != nullptr ? self->mediaNow() : std::nullopt});
                      });
        });
    });
}

void ProxmoxDevice::mediaBoot(const QString& how, bool restart, const QString& named, const QString& image,
                              std::function<void(base::Result<QString>)> done)
{
    auto boot = std::make_shared<Boot>();
    boot->how = how;
    boot->restart = restart;
    boot->named = named;
    boot->image = image;
    boot->done = std::move(done);
    bootAnew(boot);
}

void ProxmoxDevice::bootAnew(const std::shared_ptr<Boot>& boot)
{
    // The image in its drive, its boot order as its settings say it now (read and judged: rule 30), the drive first.
    thisGuest([this, boot](const base::Result<QJsonObject>& config) {
        const QString notStarted = u"was not started from it"_s;
        if (!config) {
            bootFailed(boot, notStarted, config.error(), {});
            return;
        }
        const std::optional<proxmox::Cd> cd = proxmox::cdOf(config.value());
        if (!cd || cd->disc == "none"_L1) {
            bootFailed(boot, notStarted,
                       base::Error{"action_failed", "Nothing is in the guest's CD-ROM drive to start from"}, {});
            return;
        }
        if (cd->disc != boot->image) {
            // Another image than the one confirmed, put there by another hand since it went in: not started from, and
            // nothing is changed (the web starts from whatever is in the drive).
            bootFailed(boot, notStarted, notTheOneConfirmed(cd), {});
            return;
        }
        const std::optional<QStringList> order = proxmox::bootOrderOf(config.value());
        if (!order) {
            // (No order this program cannot read whole is changed by it.)
            bootFailed(boot, notStarted,
                       base::Error{"action_failed",
                                   "The guest's boot order is written the old way, or not at all, and qKVM changes no "
                                   "order it cannot read whole: set it on Proxmox (Options, Boot Order), then try "
                                   "again."},
                       {});
            return;
        }
        // Where the order this program wrote stands, the guest's own is the one it replaced, as it is kept with the
        // guest (after a start that failed halfway, or a restart of the program); one changed on Proxmox since is of no
        // more use.
        const std::optional<BootOrderWas> kept = keptBootOrder();
        const bool ours = kept && kept->written == order->join(u';');
        boot->own = ours ? kept->order.split(u';') : *order;
        boot->first = QStringList{cd->slot};
        for (const QString& one : std::as_const(boot->own)) {
            if (one != cd->slot) {
                boot->first.append(one);
            }
        }
        if (ours && boot->first != *order && !std::exchange(boot->putBackFirst, true)) {
            // The order this program wrote stands, and the drive was moved since: another order is to be written. The
            // order from before is put back first, its record standing until it is, and only then is the new one kept
            // and written (read afresh). The record is never replaced while its order is not back: a program that ends
            // between the two finds on Proxmox the order its record names as written, or the guest's own, and never the
            // first beside the record of the second, which would lose the guest's own (outside review of 0.2.0, 3.6).
            orderBack(boot->own, *order, [this, boot](const base::Result<Back>& back) {
                if (!back) {
                    bootFailed(boot, u"was not started from it"_s, back.error(), {});
                    return;
                }
                bootAnew(boot);
            });
            return;
        }
        if (boot->first == *order) {
            if (kept && !ours) {
                static_cast<void>(keepBootOrder(std::nullopt));
            }
            bootFrom(boot);
            return;
        }
        // Kept with the guest before it is written: the order it replaces is never lost to this program.
        const BootOrderWas was{boot->own.join(u';'), boot->first.join(u';'), m_where.vmid, m_machine};
        if (const base::Result<void> keeps = keepBootOrder(was); !keeps) {
            bootFailed(boot, notStarted, keeps.error(), {}); // ("... could not be saved, so nothing was changed.")
            return;
        }
        // (What stood before it, where this write is not done: the record of an order of this program's that still
        // stands, its drive moved since, say; none where there was none, or one of no more use. Review c9b-4, L2.)
        const std::optional<BootOrderWas> before = ours ? kept : std::nullopt;
        changeSettings(config.value(), {{u"boot"_s, orderSetting(boot->first)}}, u"set the guest's boot order"_s,
                       [this, boot, before](const base::Result<void>& set) {
                           if (!set) {
                               // Not written: the record stands as it did before. (Where it may have been, the new one
                               // stays kept: the unmount or the next start "once" puts it back.)
                               if (!mayHaveBeenDone(set.error())) {
                                   static_cast<void>(keepBootOrder(before));
                               }
                               bootFailed(boot, u"was not started from it"_s, set.error(), {});
                               return;
                           }
                           bootFrom(boot);
                       });
    });
}

void ProxmoxDevice::bootFrom(const std::shared_ptr<Boot>& boot)
{
    m_statusAt = std::numeric_limits<qint64>::min() / 2; // (its card says it at the next look)
    if (boot->how == "until_unmounted"_L1 && !boot->restart) {
        // (Read again for its card: a read that fails is no failure of what was done.)
        thisGuest([boot](const base::Result<QJsonObject>& /*again*/) { boot->done(untilUnmounted()); });
        return;
    }
    const QPointer<ProxmoxDevice> self(this);
    restartGuest(boot->image, [self, boot](const Restarted& restarted) {
        if (self == nullptr) {
            return; // (let go of meanwhile: the wall tells whoever asked, own_device.h)
        }
        // Once: the order put back after, for every start after this one, also where the start did not come about (a
        // restart that did not come about leaves no order behind); where a start may have come about, its process took
        // its order as it began.
        if (boot->how != "once"_L1 || boot->first == boot->own) {
            self->bootDone(boot, restarted, std::nullopt);
            return;
        }
        // (What tidy follows to is the words of an error out of memory: nothing of ours throws, DESIGN.md section 7.)
        // NOLINTNEXTLINE(bugprone-exception-escape)
        self->orderBack(boot->own, boot->first, [self, boot, restarted](const base::Result<Back>& back) {
            if (self != nullptr) {
                self->bootDone(boot, restarted, back);
            }
        });
    });
}

void ProxmoxDevice::bootDone(const std::shared_ptr<Boot>& boot, const Restarted& restarted,
                             const std::optional<base::Result<Back>>& back)
{
    using Came = Restarted::Came;
    const bool once = boot->how == "once"_L1;
    if (restarted.came == Came::Started) {
        const QString started = u"It started from the image."_s;
        if (!once) {
            // (Read again for its card: a read that fails is no failure of what was done.)
            thisGuest([boot](const base::Result<QJsonObject>& /*again*/) { boot->done(QString()); });
            return;
        }
        if (!back) {
            boot->done(started + u" Its boot order began with the CD-ROM drive already, and still does."_s);
            return;
        }
        if (!*back) {
            boot->done(base::Error{
                "action_failed",
                u"The image is mounted, and %1 started from it, and its boot order could not be put back (%2): it "
                u"starts from the image at its next start too, until qKVM puts it back when the image is unmounted, "
                u"or it is set on Proxmox."_s.arg(boot->named, QString::fromStdString(back->error().message))
                    .toStdString()});
            return;
        }
        boot->done(started + u' ' + (back->value() == Back::ChangedMeanwhile ? changedOnProxmoxSince() : asBefore()));
        return;
    }
    // Not started from it by this program, said as far as it came.
    QString came;
    base::Error why = restarted.why;
    switch (restarted.came) {
    case Came::MayHaveStopped:
        came = u"may have been stopped, and was not started from it"_s;
        break;
    case Came::Stopped:
        came = u"was stopped, and not started again"_s;
        break;
    case Came::MayHaveStarted:
        came = restarted.stopped ? u"was stopped, and may have been started from it"_s
                                 : u"may have been started from it"_s;
        break;
    case Came::ByAnother:
        came = restarted.stopped ? u"was stopped, and started again by another hand before qKVM started it"_s
                                 : u"was started by another hand before qKVM started it"_s;
        why = base::Error{"action_failed", "look at it to see what it started from"};
        break;
    default:
        came = u"was not started from it"_s;
        break;
    }
    QString after;
    if (!once) {
        after = untilUnmounted();
    } else if (back && *back) {
        after = back->value() == Back::PutBack            ? u"Its boot order was put back."_s
                : back->value() == Back::ChangedMeanwhile ? changedOnProxmoxSince()
                                                          : QString();
    } else if (back) {
        const QString couldNot = QString::fromStdString(back->error().message);
        qWarning("qKVM: the boot order of %s (VM %lld on %s) could not be put back after a start from an image that "
                 "did not come about: %s",
                 qPrintable(kvmd::plain(QJsonValue(boot->named), 80)), static_cast<long long>(m_where.vmid),
                 qPrintable(m_where.node), qPrintable(couldNot));
        after = u"Its boot order could not be put back (%1): it begins with the CD-ROM drive until qKVM puts it back "
                u"when the image is unmounted, or it is set on Proxmox."_s.arg(couldNot);
    }
    bootFailed(boot, came, why, after);
}

void ProxmoxDevice::bootFailed(const std::shared_ptr<Boot>& boot, const QString& came, base::Error why,
                               const QString& after)
{
    QString said = QString::fromStdString(why.message);
    if (!after.isEmpty()) {
        if (!said.endsWith(u'.')) {
            said += u'.';
        }
        said += u' ' + after;
    }
    why.message = (u"The image is mounted, and %1 %2: "_s.arg(boot->named, came) + said).toStdString();
    boot->done(why);
}

void ProxmoxDevice::orderBack(const QStringList& order, const QStringList& written,
                              std::function<void(base::Result<Back>)> done)
{
    // Put back where the guest's order is still the one written in its place; left as it is where it is its own again,
    // or was changed on Proxmox since (and kept no longer either way).
    thisGuest([this, order, written, done = std::move(done)](const base::Result<QJsonObject>& config) mutable {
        if (!config) {
            done(config.error()); // (kept: the unmount, or the next start "once", puts it back)
            return;
        }
        const std::optional<QStringList> now = proxmox::bootOrderOf(config.value());
        if (now == std::optional<QStringList>(order) || now != std::optional<QStringList>(written)) {
            static_cast<void>(keepBootOrder(std::nullopt));
            done(now == std::optional<QStringList>(order) ? Back::ItsOwnAlready : Back::ChangedMeanwhile);
            return;
        }
        changeSettings(config.value(), {{u"boot"_s, orderSetting(order)}}, u"put the guest's boot order back"_s,
                       [this, done](const base::Result<void>& back) mutable {
                           if (!back) {
                               done(back.error());
                               return;
                           }
                           static_cast<void>(keepBootOrder(std::nullopt));
                           // Read again for its card: a read that fails after it was put back is no failure of the
                           // put-back.
                           thisGuest([done](const base::Result<QJsonObject>& /*again*/) { done(Back::PutBack); });
                       });
    });
}

void ProxmoxDevice::restartGuest(const QString& image, std::function<void(const Restarted&)> done)
{
    // Started anew by Proxmox: stopped at once where it runs (the plug pulled), and started. Only a start takes a new
    // boot order: a reset is the same process begun again. Each is a power button's press, with its waits and its
    // question which virtual machine has the number (act). (What act says is said on the program's loop, whether or
    // not this driver is still there: each step looks first.) Every time it is asked, as the web's _restart: the person
    // said yes to a guest stopped at once and started, and what a running guest started from is not Proxmox's to say
    // (its process's age and the order it began with are not what its firmware started from: review c9b-4, M1).
    using Came = Restarted::Came;
    Ask state;
    state.path = u"status/current"_s;
    ofTheGuest(
        state, u"look at this guest"_s, [this, image, done = std::move(done)](const base::Result<QJsonValue>& found) {
            if (!found) {
                done(Restarted{Came::NotDone, false, found.error()});
                return;
            }
            const bool running = proxmox::currentOf(found.value()).on == std::optional<bool>(true);
            const QPointer<ProxmoxDevice> self(this);
            const auto start = [self, image, done](bool stopped) {
                if (self == nullptr) {
                    return; // (the wall tells whoever asked that the driver went)
                }
                // What is in its drive, asked again right before it is started: another image put there while it was
                // stopped is not started from (outside review of 0.2.0, 3.4).
                self->thisGuest([self, image, done, stopped](const base::Result<QJsonObject>& config) {
                    if (self == nullptr) {
                        return;
                    }
                    const Came notStarted = stopped ? Came::Stopped : Came::NotDone;
                    if (!config) {
                        done(Restarted{notStarted, stopped, config.error()});
                        return;
                    }
                    if (const std::optional<proxmox::Cd> cd = proxmox::cdOf(config.value()); !cd || cd->disc != image) {
                        done(Restarted{notStarted, stopped, notTheOneConfirmed(cd)});
                        return;
                    }
                    // ...and once more on the start's own reading of its settings, right before its command (review
                    // cx3-1, L5): a start takes no digest, so the gap is narrowed to that, not closed.
                    const SettingsCheck stillThere = [image](const QJsonValue& now) -> std::optional<base::Error> {
                        if (const std::optional<proxmox::Cd> in = proxmox::cdOf(now); !in || in->disc != image) {
                            return notTheOneConfirmed(in);
                        }
                        return std::nullopt;
                    };
                    self->pressButton(u"power_on"_s, stillThere, [done, stopped](const base::Result<Done>& started) {
                        if (!started) {
                            done(Restarted{mayHaveBeenDone(started.error()) ? Came::MayHaveStarted
                                           : stopped                        ? Came::Stopped
                                                                            : Came::NotDone,
                                           stopped, started.error()});
                            return;
                        }
                        // (Running again before it was started: by another hand, from whatever its order was then.)
                        done(Restarted{
                            started.value().note == "already on"_L1 ? Came::ByAnother : Came::Started, stopped, {}});
                    });
                });
            };
            if (!running) {
                start(false);
                return;
            }
            act(Action::plain(u"atx_power_long"_s), [start, done](const base::Result<Done>& stopped) {
                if (!stopped) {
                    done(Restarted{mayHaveBeenDone(stopped.error()) ? Came::MayHaveStopped : Came::NotDone, false,
                                   stopped.error()});
                    return;
                }
                start(true);
            });
        });
}

void ProxmoxDevice::put(const MediaMount& mount, const QString& listedOn,
                        std::function<void(base::Result<MediaDone>)> done)
{
    // Which virtual machine has the guest's number, asked right before the change; and the change made on the settings
    // read for it.
    thisGuest([this, mount, listedOn, done = std::move(done)](const base::Result<QJsonObject>& config) mutable {
        if (!config) {
            done(config.error());
            return;
        }
        // Followed to another node of its cluster since its images were listed: what was listed is of the storages of
        // the node it was on, and a storage of the same name on this one need not hold the image (review c9b-2, L2).
        if (m_where.node != listedOn) {
            done(changedMeanwhile(u"The guest is on node %1 of its cluster now, and its images were listed on %2: "
                                  u"nothing was changed. Look again."_s.arg(m_where.node, listedOn)));
            return;
        }
        const std::optional<proxmox::Cd> cd = proxmox::cdOf(config.value());
        QString slot;
        QString note;
        if (!cd) {
            const QStringList& places = proxmox::newCdSlots();
            const auto free =
                std::ranges::find_if(places, [&config](const QString& one) { return !config.value().contains(one); });
            if (free == places.end()) {
                done(base::Error{"action_failed", "The guest has no CD-ROM drive, and no free place for one (ide0 to "
                                                  "ide3, sata0 to sata5): add one on Proxmox."});
                return;
            }
            slot = *free;
            note = u"The guest had no CD-ROM drive: one was added (%1). A running guest has it from its next start by "
                   u"Proxmox."_s.arg(slot);
        } else if (cd->disc == mount.image) {
            done(MediaDone{u"It was in the guest's CD-ROM drive already."_s, mediaNow()});
            return;
        } else {
            slot = cd->slot;
        }
        changeSettings(config.value(), {{slot, mount.image + u",media=cdrom"_s}},
                       u"put an image in the guest's CD-ROM drive"_s,
                       [this, note, done = std::move(done)](const base::Result<void>& changed) mutable {
                           if (!changed) {
                               done(changed.error());
                               return;
                           }
                           m_statusAt = std::numeric_limits<qint64>::min() / 2; // (its card says it at the next look)
                           thisGuest([this, note, done = std::move(done)](const base::Result<QJsonObject>& again) {
                               if (!again) {
                                   done(again.error());
                                   return;
                               }
                               done(MediaDone{note, mediaNow()});
                           });
                       });
    });
}

void ProxmoxDevice::unmountMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done)
{
    thisGuest([this, image, done = std::move(done)](const base::Result<QJsonObject>& config) mutable {
        if (!config) {
            done(config.error());
            return;
        }
        const std::optional<proxmox::Cd> cd = proxmox::cdOf(config.value());
        if (!cd || cd->disc == "none"_L1) {
            done(MediaDone{u"nothing was mounted"_s, std::nullopt});
            return;
        }
        // (What another hand put there is said as Proxmox says it, and is taken out by those words.)
        const std::optional<MountedMedia> shown = mediaNow();
        if (!shown || shown->image != image) {
            const QString named = shown ? shown->name : kvmd::plain(QJsonValue(cd->disc), 120);
            done(changedMeanwhile(
                u"Another image is in the guest's CD-ROM drive now (%1): nothing was taken out. Look again."_s.arg(
                    named)));
            return;
        }
        changeSettings(
            config.value(), {{cd->slot, u"none,media=cdrom"_s}}, u"take the image out of the guest's CD-ROM drive"_s,
            [this, slot = cd->slot, done = std::move(done)](const base::Result<void>& changed) mutable {
                if (!changed) {
                    done(changed.error());
                    return;
                }
                m_statusAt = std::numeric_limits<qint64>::min() / 2;
                // (Out of its drive, whatever a read after says.)
                m_drive.reset();
                // The order a start wrote, put back where it is still that (the web's media_unmount); kept with the
                // guest until it is, or is found changed on Proxmox since (review c9b-3, M1).
                const std::optional<BootOrderWas> was = keptBootOrder();
                thisGuest([this, slot, was, done = std::move(done)](const base::Result<QJsonObject>& again) mutable {
                    if (!again) {
                        done(MediaDone{was ? u"Its boot order could not be read to be put back (%1): it still begins "
                                             u"with the CD-ROM drive, which is empty and passed over, %2"_s.arg(
                                                 QString::fromStdString(again.error().message), untilPutBack())
                                           : QString(),
                                       mediaNow()});
                        return;
                    }
                    const std::optional<QStringList> order = proxmox::bootOrderOf(again.value());
                    if (was && order && order->join(u';') == was->written) {
                        changeSettings(
                            again.value(), {{u"boot"_s, u"order="_s + was->order}},
                            u"put the guest's boot order back"_s, [this, done](const base::Result<void>& back) mutable {
                                if (!back) {
                                    done(MediaDone{u"Its boot order could not be put back (%1): it still begins with "
                                                   u"the CD-ROM drive, which is empty and passed over, %2"_s.arg(
                                                       QString::fromStdString(back.error().message), untilPutBack()),
                                                   mediaNow()});
                                    return;
                                }
                                static_cast<void>(keepBootOrder(std::nullopt));
                                // (Read again for its card: a read that fails after it was put back is no failure.)
                                thisGuest([self = QPointer<ProxmoxDevice>(this),
                                           done](const base::Result<QJsonObject>& /*read*/) {
                                    done(MediaDone{QString(), self != nullptr ? self->mediaNow() : std::nullopt});
                                });
                            });
                        return;
                    }
                    // (Changed on Proxmox since it was written: left as it is, and of no more use.)
                    if (was) {
                        static_cast<void>(keepBootOrder(std::nullopt));
                    }
                    const bool first = order && !order->isEmpty() && order->first() == slot;
                    done(MediaDone{first ? u"Its boot order still begins with the CD-ROM drive (not set so from "
                                           u"qKVM): empty, it is passed over."_s
                                         : QString(),
                                   mediaNow()});
                });
            });
    });
}

void ProxmoxDevice::deleteMedia(const QString& /*image*/, std::function<void(base::Result<MediaDone>)> done)
{
    later([done = std::move(done)] { done(invalid(u"A guest's images are on Proxmox: qKVM deletes none of them"_s)); });
}

std::function<void()> ProxmoxDevice::uploadMedia(const QString& /*name*/, std::unique_ptr<QIODevice> /*image*/,
                                                 std::function<void(base::Result<MediaUploaded>)> done)
{
    later([done = std::move(done)] {
        done(invalid(u"Images are not carried through qKVM to a guest of Proxmox: put them on its storage there"_s));
    });
    return [] {};
}

} // namespace qkvm::core
