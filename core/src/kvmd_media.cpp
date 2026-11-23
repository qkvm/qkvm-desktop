#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_media.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <QIODevice>
#include <QJsonValue>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>

#include <qkvm/core/carried_image.h>
#include <qkvm/core/kept_with.h>
#include <qkvm/core/read_ahead.h>

// A kvmd KVM's virtual media (the web's 0.3.0, A2: kvm.py's msd parts and server.py's media routes, whose checks and
// words a KVM of this PC's own wall has here, the wall being its own server): the state of its drive, kept as the web
// keeps it, and what is done to it. (The image on its way, counted and hashed as it goes: CarriedImage, which a
// JetKVM's upload has as well.)

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// An image's name as it goes behind the question mark: kvmd reads a plus there as a blank (aiohttp, as a form is
// read), and Qt leaves a plus as it is (QUrlQuery), so a plus goes as what it is, "%2B", which Qt leaves as it is too.
QString inTheQuery(QString name)
{
    return name.replace(u'+', "%2B"_L1);
}

// What a drive is called in a sentence (server.py MEDIA_DRIVE_SAID).
QString driveCalled(const QString& drive)
{
    return drive == "cd"_L1 ? u"a CD-ROM drive"_s : drive == "flash"_L1 ? u"a USB flash drive"_s : u"a disk"_s;
}

base::Error invalid(const QString& why)
{
    return {"invalid", why.toStdString()};
}

base::Error changedMeanwhile(const QString& why)
{
    return {"changed", why.toStdString()};
}

// What a restart for an image says where the machine's board did not say whether it was on: it was reset, which did
// nothing to it if it was off. (A JetKVM's says the same.)
QString restartedNotKnowingWhetherOn()
{
    return u"It was reset with its reset button (whether it was on was not known: if it was off, it is off still)"_s;
}

// An image's name as kvmd gives it, said in a sentence: as one line of 200 at the most, "An image" where it is none.
QString nameSaid(const QString& name)
{
    const QString line = kvmd::plain(QJsonValue(name), 200);
    return line.isEmpty() ? u"An image"_s : line;
}

// Why a part of an upload may be left where it is not found in a list of its storage that was not read whole: that
// list's not being read whole is not "nothing left" (outside review 4.6).
base::Error listNotReadWhole()
{
    return {"bad_response", "its list of its own storage could not be read whole"};
}

} // namespace

QString KvmdDevice::mediaKey() const
{
    return m_where.address.toString(QUrl::FullyEncoded);
}

std::optional<MediaOffer> KvmdDevice::mediaOffer() const
{
    // (Restarted from here where it has a power board, as the last look found it: the web's "reset" in its abilities.)
    return kvmd::offerOf(m_msd, m_status && m_status->powerIsKnown);
}

bool KvmdDevice::driveIsDue() const
{
    // As its line said it of late, or as it was asked within Pace::msdEveryMs: not asked again (the web's `_msd_look`).
    // Nor one that did not answer that question, as the web's comment there has it ("asked again at the next look
    // after the pause, not at every one"): its code asks again at every look while nothing is known of the drive.
    const bool heard = m_awake && m_awake->isIn() && m_msdHeardAt && now() - *m_msdHeardAt < m_pace.eventsQuietMs;
    if (m_msd && heard) {
        return false;
    }
    return !m_msdAt || now() - *m_msdAt >= m_pace.msdEveryMs;
}

void KvmdDevice::askTheDrive(bool urgent, bool vital, std::function<void(base::Result<QJsonObject>)> then)
{
    Ask question;
    question.path = u"/api/msd"_s;
    question.urgent = urgent;
    question.vital = vital;
    ask(question, [this, then = std::move(then)](const base::Result<QJsonObject>& said) {
        if (!said) {
            then(said.error().code == "bad_response"
                     ? base::Error{"bad_response", "The KVM did not say how its drive is"}
                     : said.error());
            return;
        }
        driveSaid(kvmd::msdKept(said.value()), false);
        then(*m_msd);
    });
}

void KvmdDevice::driveSaid(QJsonObject state, bool onTheLine)
{
    const std::optional<MountedMedia> before =
        m_msd ? kvmd::mountedOf(*m_msd, kvmd::MediaNotes::program(), mediaKey()) : std::nullopt;
    const bool offeredBefore = mediaOffer().has_value();
    m_msd = std::move(state);
    m_msdAt = now();
    if (onTheLine) {
        m_msdHeardAt = m_msdAt;
    }
    const std::optional<MountedMedia> after = kvmd::mountedOf(*m_msd, kvmd::MediaNotes::program(), mediaKey());
    const auto same = [](const std::optional<MountedMedia>& one, const std::optional<MountedMedia>& other) {
        return one.has_value() == other.has_value() &&
               (!one || (one->image == other->image && one->drive == other->drive && one->readOnly == other->readOnly &&
                         one->sha256 == other->sha256));
    };
    if (!same(before, after) || offeredBefore != mediaOffer().has_value()) {
        // What its card says of its drive is not what it said: looked at again when it is next asked.
        m_statusAt = now() - m_pace.freshMs - 1;
        emit statusChanged();
    }
}

void KvmdDevice::askMedia(std::function<void(base::Result<MediaList>)> done)
{
    askTheDrive(true, true, [this, done = std::move(done)](const base::Result<QJsonObject>& state) {
        if (!state) {
            done(state.error());
            return;
        }
        if (!mediaOffer()) {
            done(kvmd::switchedOff());
            return;
        }
        MediaList list;
        const kvmd::Listed listed = kvmd::imagesOf(state.value(), kvmd::MediaNotes::program(), mediaKey());
        list.images = listed.images;
        list.leftOut = listed.leftOut;
        if (const std::optional<base::Error> against = kvmd::standsAgainst(state.value())) {
            list.notes.append(QString::fromStdString(against->message));
        }
        // (A list not read whole is never shown as all there is: review cx4-1 L5, an empty storage.)
        if (!listed.whole) {
            list.readWhole = false;
            list.notes.append(kvmd::notReadWholeNote(
                QString::fromStdString(kvmd::notReadWhole(u"not every image on it may be shown here"_s).message)));
        }
        const std::optional<qint64> free = kvmd::freeOf(state.value());
        const std::optional<qint64> size = kvmd::sizeOf(state.value());
        if (free && size) {
            list.free = free;
            list.size = size;
        }
        list.mounted = kvmd::mountedOf(state.value(), kvmd::MediaNotes::program(), mediaKey());
        list.offers = *mediaOffer();
        list.uploading = uploading();
        done(std::move(list));
    });
}

void KvmdDevice::changeTheDrive(const QString& path, const QUrlQuery& query, const QString& what,
                                std::function<void(std::optional<base::Error>)> then)
{
    Ask change;
    change.method = "POST"_ba;
    change.path = path;
    change.query = query;
    change.urgent = true;
    change.doesSomething = true;
    request(change, [what, then = std::move(then)](const base::Result<Reply>& reply) {
        if (!reply) {
            then(reply.error());
            return;
        }
        then(reply.value().status == 200
                 ? std::nullopt
                 : std::optional<base::Error>(kvmd::msdRefusal(reply.value().status, reply.value().body, what)));
    });
}

void KvmdDevice::listedUntil(const QString& name, int withinMs, Until until, qint64 whole,
                             std::function<void(base::Result<ListedNow>)> then)
{
    // kvmd lists a file, and stops listing one, as its watch on the storage last saw it, which may be mid-write: asked
    // again, Pace::listedEveryMs apart, until it lists it (no longer, `Gone`; whole at `whole` bytes, `WholeAt`), and
    // for `withinMs` at the most. Only the first question's failure is said: a later one is asked again. (The web's
    // `_msd_listing`.) Held by its questions and its timer alone, which go with this driver.
    auto listing = std::make_shared<Listing>();
    listing->name = name;
    listing->until = until;
    listing->whole = whole;
    listing->deadline = now() + withinMs;
    listing->then = std::move(then);
    listingRound(listing);
}

void KvmdDevice::listingRound(const std::shared_ptr<Listing>& listing)
{
    askTheDrive(true, true, [this, listing](const base::Result<QJsonObject>& state) {
        if (!state && !listing->last) {
            listing->then(state.error());
            return;
        }
        if (state) {
            const kvmd::Listed all = kvmd::imagesOf(state.value(), kvmd::MediaNotes::program(), mediaKey());
            ListedNow listed{state.value(), std::nullopt, all.whole};
            for (const MediaImage& image : all.images) {
                if (image.id == listing->name) {
                    listed.image = image;
                }
            }
            // (Gone only from a list read whole. Outside review 4.6.)
            const bool done = listing->until == Until::Gone ? !listed.image && listed.whole
                              : listing->until == Until::WholeAt
                                  ? listed.image && !listed.image->partial &&
                                        listed.image->size == std::optional<qint64>(listing->whole)
                                  : listed.image.has_value();
            listing->last = std::move(listed);
            if (done) {
                listing->then(*listing->last);
                return;
            }
        }
        if (now() >= listing->deadline) {
            listing->then(*listing->last);
            return;
        }
        QTimer::singleShot(m_pace.listedEveryMs, this, [this, listing] { listingRound(listing); });
    });
}

void KvmdDevice::mountMedia(const MediaMount& mount, std::function<void(base::Result<MediaDone>)> done)
{
    const QString& named = mount.machine;
    // As the server takes it (server.py, media_mount), by what the last look found its drive can do; and then as the
    // driver takes it (kvm.py, media_mount).
    const std::optional<MediaOffer> offer = mediaOffer();
    const bool restart = mount.restart || mount.boot == "once"_L1;
    std::optional<base::Error> refused;
    if (!offer) {
        refused = base::Error{"no_virtual_media", u"%1 has no virtual media"_s.arg(named).toStdString()};
    } else if (mount.drive != "cd"_L1 && mount.drive != "flash"_L1 && mount.drive != "disk"_L1) {
        refused = invalid(u"%1 takes no image as that"_s.arg(named));
    } else if (!offer->drives.contains(mount.drive)) {
        refused = invalid(u"%1 takes no image as %2"_s.arg(named, driveCalled(mount.drive)));
    } else if (mount.writable && !offer->writable.contains(mount.drive)) {
        refused = invalid(u"%1 takes an image as %2 read-only, and only so"_s.arg(named, driveCalled(mount.drive)));
    } else if (mount.boot != "none"_L1 && !offer->boot.contains(mount.boot)) {
        refused = invalid(u"%1 cannot be told to start from an image that way"_s.arg(named));
    } else if (restart && !offer->restart) {
        refused = invalid(u"%1 cannot be restarted from here"_s.arg(named));
    } else if (!kvmd::isDeviceImage(mount.image)) {
        refused = invalid(u"That is not an image of the KVM's that qKVM lists"_s);
    }
    if (refused) {
        later([done = std::move(done), why = *refused] { done(why); });
        return;
    }
    askTheDrive(true, true, [this, mount, restart, done = std::move(done)](const base::Result<QJsonObject>& state) {
        if (!state) {
            done(state.error());
            return;
        }
        if (const std::optional<base::Error> against = kvmd::standsAgainst(state.value())) {
            done(*against);
            return;
        }
        const QList<MediaImage> images = kvmd::imagesOf(state.value(), kvmd::MediaNotes::program(), mediaKey()).images;
        const auto found =
            std::ranges::find_if(images, [&mount](const MediaImage& image) { return image.id == mount.image; });
        if (found == images.end()) {
            done(changedMeanwhile(u"%1 is not on the KVM's own storage any more: look again."_s.arg(mount.image)));
            return;
        }
        if (found->partial) {
            done(changedMeanwhile(u"%1 is not on the KVM whole (an upload of it was cut off): delete it, or upload it "
                                  u"again."_s.arg(mount.image)));
            return;
        }
        if (mount.drive == "cd"_L1 && found->size && *found->size < kvmd::leastOfACd) {
            done(
                invalid(u"%1 is %2 bytes, and a CD-ROM drive takes an image of %3 bytes at least (300 sectors of 2,048 "
                        u"bytes): mount it as a USB flash drive."_s.arg(mount.image, kvmd::withCommas(*found->size),
                                                                        kvmd::withCommas(kvmd::leastOfACd))));
            return;
        }
        const auto asAsked = [image = mount.image, drive = mount.drive,
                              writable = mount.writable](const MountedMedia& now) {
            return now.image == image && now.drive == drive && now.readOnly == !writable;
        };
        if (const std::optional<MountedMedia> now =
                kvmd::mountedOf(state.value(), kvmd::MediaNotes::program(), mediaKey())) {
            if (now->image == kvmd::unreadDrive) {
                done(kvmd::driveNotRead());
                return;
            }
            if (asAsked(*now)) {
                done(MediaDone{u"It was mounted so already."_s, now});
                return;
            }
            done(changedMeanwhile(u"%1 is mounted on it now: unmount it first."_s.arg(kvmd::opensASentence(*now))));
            return;
        }
        // (A CD-ROM drive is always read-only, and kvmd ignores `rw` there; a USB flash drive is read-only unless it
        // was chosen writable.)
        QUrlQuery chosen;
        chosen.addQueryItem(u"image"_s, inTheQuery(mount.image));
        chosen.addQueryItem(u"cdrom"_s, mount.drive == "cd"_L1 ? u"1"_s : u"0"_s);
        chosen.addQueryItem(u"rw"_s, mount.writable ? u"1"_s : u"0"_s);
        // kvmd connects whatever is chosen for its drive when its connect comes, and has no connect that names an
        // image: what was chosen is read back before anything is connected, and nothing is connected where it is not
        // this image as asked (another client of the KVM may have chosen its own meanwhile), or where another client
        // has connected its drive meanwhile. That narrows the moment between, and cannot close it: one that chooses in
        // it has its choice connected by this mount's connect. kvmd takes a connect only while its drive is not
        // connected (stock kvmd and GL.iNet's refuse it otherwise, MsdConnectedError), so what a connect it answered
        // yes to finds in the drive was put there by that connect: where that is not as asked, this image or another
        // client's choice, it is taken out again, read back, and said (outside review 4.1 and 4.2, review cx4-1 M1;
        // the web connects at once, and takes out whatever came out otherwise).
        const auto isThis = [image = mount.image, drive = mount.drive,
                             writable = mount.writable](const std::optional<kvmd::Chosen>& chosen) {
            return chosen && chosen->image == image && chosen->drive == drive && chosen->readOnly == !writable;
        };
        changeTheDrive(u"/api/msd/set_params"_s, chosen, u"choose the image"_s,
                       [this, mount, restart, asAsked, isThis, done](const std::optional<base::Error>& refused) {
                           if (refused) {
                               done(*refused);
                               return;
                           }
                           askTheDrive(
                               true, true,
                               [this, mount, restart, asAsked, isThis, done](const base::Result<QJsonObject>& state) {
                                   if (!state) {
                                       done(state.error());
                                       return;
                                   }
                                   const std::optional<kvmd::Chosen> chosen = kvmd::chosenOf(state.value());
                                   if (chosen && chosen->connected) {
                                       // (Connected by somebody else after this mount chose, this image or another:
                                       // said as that, and not as a choice otherwise. Review cx4-1 L4.)
                                       done(changedMeanwhile(
                                           u"%1 was connected to the KVM's drive by another client of the KVM "
                                           u"meanwhile, not by this mount, which connected nothing: look at it on its "
                                           u"own page."_s.arg(nameSaid(chosen->image))));
                                       return;
                                   }
                                   if (!isThis(chosen)) {
                                       done(changedMeanwhile(
                                           u"The KVM's drive does not have %1 chosen as it was asked (another client "
                                           u"of the KVM may have chosen otherwise meanwhile): nothing was connected. "
                                           u"Look at it again."_s.arg(mount.image)));
                                       return;
                                   }
                                   connectChosen(mount, restart, asAsked, done);
                               });
                       });
    });
}

void KvmdDevice::connectChosen(const MediaMount& mount, bool restart,
                               const std::function<bool(const MountedMedia&)>& asAsked,
                               const std::function<void(base::Result<MediaDone>)>& done)
{
    QUrlQuery in;
    in.addQueryItem(u"connected"_s, u"1"_s);
    changeTheDrive(
        u"/api/msd/set_connected"_s, in, u"connect its drive"_s,
        [this, mount, restart, asAsked, done](const std::optional<base::Error>& refused) {
            if (refused) {
                done(*refused);
                return;
            }
            askTheDrive(true, true, [this, mount, restart, asAsked, done](const base::Result<QJsonObject>& state) {
                if (!state) {
                    // (Connected by it, whatever it put in: not said as a mount that did not happen. Review cx4-2 L2.)
                    done(base::Error{state.error().code,
                                     "This mount connected the KVM's drive, and what is in it since is not known (" +
                                         state.error().message + "): look at it on its own page."});
                    return;
                }
                const std::optional<MountedMedia> now =
                    kvmd::mountedOf(state.value(), kvmd::MediaNotes::program(), mediaKey());
                if (!now || !asAsked(*now)) {
                    const std::optional<kvmd::Chosen> chosen = kvmd::chosenOf(state.value());
                    mountedOtherwise(now, mount.image, chosen && chosen->image == mount.image, done);
                    return;
                }
                if (!restart) {
                    done(MediaDone{{}, now});
                    return;
                }
                restartForMedia(mount.machine, [this, done](const base::Result<QString>& said) {
                    const std::optional<MountedMedia> mounted =
                        m_msd ? kvmd::mountedOf(*m_msd, kvmd::MediaNotes::program(), mediaKey()) : std::nullopt;
                    if (!said) {
                        done(said.error());
                        return;
                    }
                    done(MediaDone{said.value(), mounted});
                });
            });
        });
}

void KvmdDevice::mountedOtherwise(const std::optional<MountedMedia>& now, const QString& image, bool thisOne,
                                  const std::function<void(base::Result<MediaDone>)>& done)
{
    // Not as it was asked, after a connect kvmd answered yes to, which it takes only while nothing is connected: what
    // is in its drive now was put there by that connect. This very image in another kind of drive (writable where it
    // was to be read-only), or another image that another client of the KVM chose in the moment between this mount's
    // look and its connect (and had not connected: kvmd would have refused this connect): taken out again at once,
    // read back as an unmount is, and said (outside review 4.1 and 4.2, review cx4-1 M1; the web takes out whatever
    // came out otherwise). What cannot be read is left as it is, and said.
    if (!now) {
        // (Connected, and out again by the read-back: another client of the KVM, or the machine, took it out. Review
        // cx4-2 L6: it was said not to have been mounted.)
        done(base::Error{
            "action_failed",
            u"%1 was connected, and is not in its drive now: look at it on its own page."_s.arg(image).toStdString()});
        return;
    }
    if (now->image == kvmd::unreadDrive) {
        done(base::Error{"bad_response", "This mount connected the KVM's drive, and the KVM does not say what is in it "
                                         "in a shape qKVM reads: nothing more was done. Look at it on its own page."});
        return;
    }
    // (This image is said in the web's words; another as what it is, and whose choice it was; one whose name the KVM
    // does not say at all as what it may be, whoever chose it: review cx4-2 L6.)
    const bool unnamed = !thisOne && kvmd::saysNoName(*now);
    const char* const code = thisOne ? "action_failed" : "changed";
    const QString what =
        thisOne   ? u"The KVM did not mount it as it was asked"_s
        : unnamed ? u"An image whose name the KVM does not say (it may be %1) was connected by this mount"_s.arg(image)
                  : u"%1, which another client of the KVM chose for its drive meanwhile, was connected by this mount "
                    u"in place of %2"_s.arg(kvmd::opensASentence(*now), image);
    const QString stillIn =
        thisOne ? u", and says it is still in its drive"_s : u", and the KVM says it is still in its drive"_s;
    QUrlQuery out;
    out.addQueryItem(u"connected"_s, u"0"_s);
    changeTheDrive(
        u"/api/msd/set_connected"_s, out, u"disconnect its drive"_s,
        [this, done, code, what, stillIn, thisOne, other = now->image](const std::optional<base::Error>& refused) {
            const auto said = [done, code, what](const QString& rest) {
                done(base::Error{code, (what + rest).toStdString()});
            };
            if (refused) {
                said(u", and could not be told to take it out again (%1): look at it on its own page."_s.arg(
                    QString::fromStdString(refused->message)));
                return;
            }
            // (Read back as an unmount's is: one that says it is in its drive still is not said to be out. Outside
            // review 4.2.)
            askTheDrive(true, true, [this, said, stillIn, thisOne, other](const base::Result<QJsonObject>& state) {
                if (!state) {
                    said(u", and was told to take it out again; what is in its drive since is not known (%1): look at "
                         u"it on its own page."_s.arg(QString::fromStdString(state.error().message)));
                    return;
                }
                if (kvmd::mountedOf(state.value(), kvmd::MediaNotes::program(), mediaKey())) {
                    said(stillIn + u" after it was told to take it out: look at it on its own page."_s);
                    return;
                }
                if (thisOne) {
                    said(u", and it is not mounted: look at it on its own page."_s);
                    return;
                }
                // (By the id it went by, a nameless image's too: review cx4-2 L6.)
                said(u", and was taken out again: nothing is mounted%1. Look at it on its own page."_s.arg(
                    kvmd::chosenIdOf(state.value()) == other ? u" (it is chosen for its drive there still)"_s
                                                             : QString()));
            });
        });
}

void KvmdDevice::restartForMedia(const QString& named, std::function<void(base::Result<QString>)> then)
{
    // A machine that cannot be told to start from it, restarted for its firmware to find it: a power button's press,
    // by every rule of one (act). Switched on where it is off, else reset; where its board does not say whether it is
    // on, reset, and said so (a reset does nothing to a machine that is off: review cx4-1 L4). (The web's
    // `media_restart`.)
    askTheBoard([this, named, then = std::move(then)](std::optional<bool> on, bool /*busy*/) {
        const bool wasOff = on == std::optional<bool>(false);
        act(Action::plain(wasOff ? u"atx_power"_s : u"atx_reset"_s),
            [named, on, then](const base::Result<Done>& pressed) {
                if (!pressed) {
                    base::Error why = pressed.error();
                    why.message =
                        u"The image is mounted, and %1 was not restarted: "_s.arg(named).toStdString() + why.message;
                    then(why);
                    return;
                }
                QString said = !on   ? restartedNotKnowingWhetherOn()
                               : *on ? u"It was restarted with its reset button"_s
                                     : u"It was off, and was switched on with its power button"_s;
                if (!pressed.value().note.isEmpty()) {
                    said += u" (%1)"_s.arg(pressed.value().note);
                }
                then(said + u": pick the KVM's drive in its boot menu where it does not start from it by itself."_s);
            });
    });
}

void KvmdDevice::unmountMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done)
{
    askTheDrive(true, true, [this, image, done = std::move(done)](const base::Result<QJsonObject>& state) {
        if (!state) {
            done(state.error());
            return;
        }
        if (!mediaOffer()) {
            done(kvmd::switchedOff());
            return;
        }
        const std::optional<MountedMedia> now = kvmd::mountedOf(state.value(), kvmd::MediaNotes::program(), mediaKey());
        if (!now) {
            done(MediaDone{u"nothing was mounted"_s, std::nullopt});
            return;
        }
        if (now->image == kvmd::unreadDrive) {
            done(kvmd::driveNotRead());
            return;
        }
        if (now->image != image) {
            // (One whose name was not read is told by its id, which says nothing to a person.)
            const QString asked =
                kvmd::isNameless(image) ? u"the image it had in its drive when it was looked at"_s : image;
            done(
                changedMeanwhile(u"%1 is mounted on it now, and not %2: nothing was taken out. Look at it again."_s.arg(
                    kvmd::opensASentence(*now), asked)));
            return;
        }
        if (state.value().value("busy"_L1) == QJsonValue(true)) {
            done(base::Error{"busy", "The KVM's drive is busy with something else (an upload, or its own page): try "
                                     "again in a moment"});
            return;
        }
        QUrlQuery out;
        out.addQueryItem(u"connected"_s, u"0"_s);
        changeTheDrive(u"/api/msd/set_connected"_s, out, u"disconnect its drive"_s,
                       [this, done](const std::optional<base::Error>& refused) {
                           if (refused) {
                               done(*refused);
                               return;
                           }
                           askTheDrive(true, true, [this, done](const base::Result<QJsonObject>& state) {
                               if (!state) {
                                   done(state.error());
                                   return;
                               }
                               if (kvmd::mountedOf(state.value(), kvmd::MediaNotes::program(), mediaKey())) {
                                   done(base::Error{"action_failed", "The KVM says the image is still in its drive: "
                                                                     "look at it on its own page."});
                                   return;
                               }
                               done(MediaDone{});
                           });
                       });
    });
}

void KvmdDevice::deleteMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done)
{
    if (!kvmd::isDeviceImage(image)) {
        later([done = std::move(done)] { done(invalid(u"That is not an image of the KVM's that qKVM lists"_s)); });
        return;
    }
    askTheDrive(true, true, [this, image, done = std::move(done)](const base::Result<QJsonObject>& state) {
        if (!state) {
            done(state.error());
            return;
        }
        if (const std::optional<base::Error> against = kvmd::standsAgainst(state.value())) {
            done(*against);
            return;
        }
        const kvmd::Listed listed = kvmd::imagesOf(state.value(), kvmd::MediaNotes::program(), mediaKey());
        if (std::ranges::none_of(listed.images, [&image](const MediaImage& one) { return one.id == image; })) {
            // (Not found in a list that was not read whole: not known to be gone. Outside review 4.6.)
            done(listed.whole
                     ? base::Result<MediaDone>(MediaDone{u"it was not on the KVM any more"_s, std::nullopt})
                     : base::Result<MediaDone>(kvmd::notReadWhole(u"whether %1 is there is not known"_s.arg(image))));
            return;
        }
        const std::optional<MountedMedia> now = kvmd::mountedOf(state.value(), kvmd::MediaNotes::program(), mediaKey());
        // (Not while what is in its drive cannot be read: it may be this one. Outside review 4.7.)
        if (now && now->image == kvmd::unreadDrive) {
            done(kvmd::driveNotRead());
            return;
        }
        // (Nor while what is in it goes by a name qKVM does not read as one of its images: it may be this one. Review
        // cx4-1 M2.)
        if (now && kvmd::isNameless(now->image)) {
            done(base::Error{"busy", u"%1 is in its drive, by a name qKVM does not read as one of its images, so it "
                                     u"may be %2: unmount it first."_s.arg(kvmd::opensASentence(*now), image)
                                         .toStdString()});
            return;
        }
        if (now && now->image == image) {
            done(base::Error{"busy", u"%1 is mounted: unmount it first."_s.arg(image).toStdString()});
            return;
        }
        QUrlQuery which;
        which.addQueryItem(u"image"_s, inTheQuery(image));
        changeTheDrive(
            u"/api/msd/remove"_s, which, u"delete it"_s,
            [this, image, done](const std::optional<base::Error>& refused) {
                if (refused) {
                    done(*refused);
                    return;
                }
                kvmd::MediaNotes::program().forget(mediaKey(), image);
                // (Listed a moment longer, as its watch on the storage saw it last.)
                listedUntil(
                    image, m_pace.listedWithinMs, Until::Gone, 0, [image, done](const base::Result<ListedNow>& listed) {
                        if (!listed) {
                            done(listed.error());
                            return;
                        }
                        if (listed.value().image) {
                            done(base::Error{"action_failed", "The KVM says the image is still on its storage: look "
                                                              "at it on its own page."});
                            return;
                        }
                        if (!listed.value().whole) {
                            done(kvmd::notReadWhole(u"whether %1 is gone is not known"_s.arg(image)));
                            return;
                        }
                        done(MediaDone{});
                    });
            });
    });
}

// The image on its way, from its asking to its answer: one at a time.
struct KvmdDevice::Upload {
    QString name;
    qint64 size = 0;
    qint64 sent = 0;
    bool stopped = false;
    std::shared_ptr<CarriedImage> body;
    QPointer<QNetworkReply> reply;
    std::function<void(base::Result<MediaUploaded>)> done;
};

std::optional<MediaUploading> KvmdDevice::uploading() const
{
    if (!m_upload) {
        return std::nullopt;
    }
    // (Nobody else uploads to this PC's own KVMs: whoever did, did it here.)
    return MediaUploading{m_upload->name, m_upload->size, m_upload->sent, u"this PC"_s};
}

std::function<void()> KvmdDevice::uploadMedia(const QString& name, std::unique_ptr<QIODevice> image,
                                              std::function<void(base::Result<MediaUploaded>)> done)
{
    // Only from a file that is read at a place, whose length is known before a byte goes; and read on a thread of its
    // own, never on this one (ReadAhead: a share that answers slowly, or not at all, holds nothing here still).
    std::unique_ptr<QIODevice> file;
    if (qobject_cast<ReadAhead*>(image.get()) != nullptr) {
        file = std::move(image);
    } else {
        file = ReadAhead::of(std::move(image));
    }
    const bool readable = file != nullptr && file->isOpen() && file->isReadable() && !file->isSequential();
    const qint64 size = readable ? file->size() - file->pos() : 0;
    std::optional<base::Error> refused;
    if (!readable) {
        refused = base::Error{"invalid", "An image is carried from a file."};
    } else if (size <= 0) {
        refused = base::Error{"invalid", "An empty file is no image."};
    } else if (size > mostUploadBytes) {
        refused = base::Error{"too_large", "An image carried through qKVM may be 16 GB at the most."};
    } else if (!isImageName(name)) {
        refused = base::Error{"invalid", imageNameRule};
    } else if (m_upload) {
        refused = base::Error{
            "busy", u"%1 is being uploaded to this KVM at this moment. Wait for it to finish."_s.arg(m_upload->name)
                        .toStdString()};
    }
    if (refused) {
        later([done = std::move(done), why = *refused] { done(why); });
        return [] {};
    }
    auto upload = std::make_shared<Upload>();
    upload->name = name;
    upload->size = size;
    upload->body = std::make_shared<CarriedImage>(std::move(file));
    upload->done = std::move(done);
    m_upload = upload;
    // (Signed in anew where its session ran out: the upload goes on a fresh one.)
    askTheDrive(true, true, [this, upload](const base::Result<QJsonObject>& state) {
        if (upload->stopped) {
            uploadOver(upload, stoppedBefore());
            return;
        }
        if (!state) {
            uploadOver(upload, state.error());
            return;
        }
        if (const std::optional<base::Error> against = kvmd::standsAgainst(state.value())) {
            uploadOver(upload, *against);
            return;
        }
        const kvmd::Listed listed = kvmd::imagesOf(state.value(), kvmd::MediaNotes::program(), mediaKey());
        if (std::ranges::any_of(listed.images, [&upload](const MediaImage& one) { return one.id == upload->name; })) {
            uploadOver(upload, base::Error{"duplicate", u"%1 is on the KVM's own storage already: delete it first, or "
                                                        u"upload it under another name."_s.arg(upload->name)
                                                            .toStdString()});
            return;
        }
        // (Not found in a list that was not read whole: it may be there all the same. Outside review 4.6.)
        if (!listed.whole) {
            uploadOver(upload, kvmd::notReadWhole(u"whether %1 is there already is not known"_s.arg(upload->name)));
            return;
        }
        if (const std::optional<qint64> free = kvmd::freeOf(state.value()); free && upload->size > *free) {
            uploadOver(upload, base::Error{"no_space", u"The KVM's own storage has %1 bytes free, and the image is %2: "
                                                       u"delete an image there first."_s
                                                           .arg(kvmd::withCommas(*free), kvmd::withCommas(upload->size))
                                                           .toStdString()});
            return;
        }
        withToken([this, upload](const base::Result<QByteArray>& token) {
            if (upload->stopped) {
                uploadOver(upload, stoppedBefore());
                return;
            }
            if (!token) {
                uploadOver(upload, token.error());
                return;
            }
            carry(upload, token.value());
        });
    });
    return [self = QPointer<KvmdDevice>(this), held = std::weak_ptr<Upload>(upload)] {
        const std::shared_ptr<Upload> upload = held.lock();
        if (self == nullptr || upload == nullptr || upload->stopped) {
            return;
        }
        upload->stopped = true;
        if (upload->reply != nullptr && !upload->reply->isFinished()) {
            upload->reply->abort();
        }
    };
}

base::Error KvmdDevice::stoppedBefore()
{
    return {kvmd::uploadStopped, "The upload was stopped before any of it went."};
}

void KvmdDevice::carry(const std::shared_ptr<Upload>& upload, const QByteArray& token)
{
    // On a line of its own, made for it and let go of after it, whatever comes (the web's `_carry`: a long upload cut
    // off is no state for the line every look uses), held to what the KVM is known by as every line of this driver is.
    auto* line = new DeviceAsker(
        DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate},
        DeviceAsker::Times{m_pace.connectWithinMs, m_pace.uploadQuietMs, m_pace.transferMs}, "auth_token"_ba,
        this); // NOLINT(cppcoreguidelines-owning-memory): this driver's, let go of once it is answered
    connect(line, &DeviceAsker::showedItsCertificate, this, &KvmdDevice::showed);
    DeviceAsker::Ask write;
    write.method = "POST"_ba;
    write.path = u"/api/msd/write"_s;
    write.query.addQueryItem(u"image"_s, inTheQuery(upload->name));
    // (What a write cut off leaves is removed by kvmd itself; and from here after it, where kvmd did not: discard.)
    write.query.addQueryItem(u"remove_incomplete"_s, u"1"_s);
    write.streamed = upload->body.get();
    write.bodyIs = "application/octet-stream"_ba;
    write.largest = kvmd::largestReplyBytes;
    write.quietMs = m_pace.uploadQuietMs;
    write.wholeMs = m_pace.uploadMostMs;
    write.doesSomething = true;
    write.handed = [held = std::weak_ptr<Upload>(upload)](QNetworkReply* reply) {
        const std::shared_ptr<Upload> upload = held.lock();
        if (upload == nullptr) {
            reply->abort();
            return;
        }
        upload->reply = reply;
        // (The file is the reply's for as long as it is: Qt may read it until it lets go. It ends with the reply, on
        // its thread: kept_with.h.)
        keptWith(*reply, upload->body);
        QObject::connect(reply, &QNetworkReply::uploadProgress, reply, [held](qint64 sent, qint64 /*total*/) {
            if (const std::shared_ptr<Upload> upload = held.lock()) {
                upload->sent = std::clamp<qint64>(sent, 0, upload->size);
            }
        });
        if (upload->stopped) {
            QMetaObject::invokeMethod(reply, &QNetworkReply::abort, Qt::QueuedConnection);
        }
    };
    line->send(write, token, [this, upload, line, token](const base::Result<DeviceAsker::Reply>& reply) {
        line->deleteLater();
        upload->reply = nullptr;
        written(upload, token, reply);
    });
}

void KvmdDevice::written(const std::shared_ptr<Upload>& upload, const QByteArray& token,
                         const base::Result<DeviceAsker::Reply>& reply)
{
    if (!reply) {
        // It ended before its answer: the sender's doing (stopped, or its file could not be read on the way), or the
        // KVM's. What came of it on the KVM is taken off where kvmd did not take it off itself, and said.
        discard(upload->name, [this, upload, why = reply.error(), failed = upload->body->failure()](
                                  const std::optional<QString>& left) { cutOff(upload, why, failed, left); });
        return;
    }
    const int status = reply.value().status;
    const std::optional<qint64> written = kvmd::writtenOf(reply.value().body);
    if (status != 200 || written != std::optional<qint64>(upload->size)) {
        if ((status == 401 || status == 403) && m_token == token) {
            m_token.clear();
        }
        discard(upload->name,
                [this, upload, status, written, detail = kvmd::errorOf(reply.value().body)](
                    const std::optional<QString>& left) { notWhole(upload, status, written, detail, left); });
        return;
    }
    // Written whole, it says: listed so a moment later (as its watch on the storage saw it last, mid-write,
    // meanwhile). Never taken off where it is not: kvmd said it wrote every byte.
    listedUntil(upload->name, m_pace.listedWithinMs, Until::WholeAt, upload->size,
                [this, upload](const base::Result<ListedNow>& listed) { listedAfter(upload, listed); });
}

void KvmdDevice::cutOff(const std::shared_ptr<Upload>& upload, const base::Error& why, const QString& failed,
                        const std::optional<QString>& left)
{
    const QString after = left.value_or(nothingLeft());
    if (upload->stopped) {
        uploadOver(upload,
                   base::Error{left ? "action_failed" : kvmd::uploadStopped,
                               (u"The upload was stopped before all of it had gone. "_s + after).toStdString()});
    } else if (!failed.isEmpty()) {
        uploadOver(upload,
                   base::Error{"disconnected", (u"The image could not be read on the way, and nothing more of it "
                                                u"went: "_s +
                                                failed + u' ' + after)
                                                   .toStdString()});
    } else {
        const bool kept = why.code == "timeout" || why.code == "dropped" || why.code == "unreachable";
        uploadOver(upload, base::Error{kept ? why.code : std::string("action_failed"),
                                       "The upload did not get through to the KVM (" + why.message + "). " +
                                           after.toStdString()});
    }
}

void KvmdDevice::notWhole(const std::shared_ptr<Upload>& upload, int status, std::optional<qint64> written,
                          const QString& detail, const std::optional<QString>& left)
{
    const QString after = left.value_or(nothingLeft());
    if (status == 507) {
        uploadOver(upload, base::Error{"no_space",
                                       (u"The KVM's own storage has not got room for it. "_s + after).toStdString()});
        return;
    }
    if (status == 401 || status == 403) {
        uploadOver(
            upload,
            base::Error{"action_failed",
                        (u"The KVM did not take qKVM's session for the upload: try again. "_s + after).toStdString()});
        return;
    }
    const QString why = !detail.isEmpty() ? detail
                        : status == 200   ? u"it says %1 of %2 bytes were written"_s.arg(
                                              kvmd::withCommas(written.value_or(0)), kvmd::withCommas(upload->size))
                                        : u"HTTP %1"_s.arg(status);
    uploadOver(upload, base::Error{"action_failed",
                                   u"The KVM did not take the image whole (%1). %2"_s.arg(why, after).toStdString()});
}

void KvmdDevice::listedAfter(const std::shared_ptr<Upload>& upload, const base::Result<ListedNow>& listed)
{
    if (!listed) {
        uploadOver(upload, listed.error());
        return;
    }
    const std::optional<MediaImage>& image = listed.value().image;
    if (!image || image->partial || image->size != std::optional<qint64>(upload->size)) {
        uploadOver(upload, base::Error{"action_failed",
                                       u"The KVM said it wrote all of the image, and after %1 seconds "
                                       u"still does not list it whole: look at it on the KVM's own "
                                       u"page before mounting it."_s.arg(qRound(m_pace.listedWithinMs / 1000.0))
                                           .toStdString()});
        return;
    }
    const QString sha = upload->body->sha256();
    kvmd::MediaNotes::program().carried(mediaKey(), upload->name, sha, upload->size,
                                        kvmd::stampOf(listed.value().state, upload->name));
    uploadOver(upload, MediaUploaded{sha});
}

QString KvmdDevice::nothingLeft()
{
    return u"Nothing of it is left on the KVM."_s;
}

void KvmdDevice::discard(const QString& name, std::function<void(std::optional<QString>)> then)
{
    discardOnceIdle(name, now() + m_pace.discardWaitMs, std::move(then));
}

void KvmdDevice::discardOnceIdle(const QString& name, qint64 until, std::function<void(std::optional<QString>)> then)
{
    // (The web's `media_discard`.) A write cut off is over for kvmd once it has read to the cut: its drive is busy with
    // it until then, a moment. Then what it left, as its storage lists it a moment longer: nothing, an image whole
    // after all (which stays), or a part, which is removed: only what kvmd plainly says is not whole (`complete`
    // false). One it does not say that of may be whole, and stays (review cx4-1 L1: it was removed, as what is not
    // plainly whole is no image to mount).
    const auto mayBe = [](const base::Error& why) {
        return std::optional<QString>(u"What was written of it may be on the KVM still (%1): delete it there."_s.arg(
            QString::fromStdString(why.message)));
    };
    askTheDrive(true, true, [this, name, until, mayBe, then = std::move(then)](const base::Result<QJsonObject>& state) {
        if (!state) {
            then(mayBe(state.error()));
            return;
        }
        if (state.value().value("busy"_L1) == QJsonValue(true) && now() < until) {
            QTimer::singleShot(m_pace.discardEveryMs, this,
                               [this, name, until, then] { discardOnceIdle(name, until, then); });
            return;
        }
        listedUntil(
            name, m_pace.listedForADiscardMs, Until::Listed, 0,
            [this, name, mayBe, then](const base::Result<ListedNow>& listed) {
                if (!listed) {
                    then(mayBe(listed.error()));
                    return;
                }
                if (!listed.value().image) {
                    then(listed.value().whole ? std::nullopt : mayBe(listNotReadWhole()));
                    return;
                }
                const std::optional<bool> whole = kvmd::completeOf(listed.value().state, name);
                if (whole != std::optional<bool>(false)) {
                    const std::optional<qint64> size = listed.value().image->size;
                    const QString bytes = size ? u" (%1 bytes)"_s.arg(kvmd::withCommas(*size)) : QString();
                    then(whole == std::optional<bool>(true)
                             ? u"It is on the KVM whole all the same%1, and stays there: delete it there if it is "
                               u"not wanted."_s.arg(bytes)
                             : u"It is on the KVM%1, which does not say whether it is whole, and stays there: "
                               u"delete it there if it is not wanted."_s.arg(bytes));
                    return;
                }
                QUrlQuery which;
                which.addQueryItem(u"image"_s, inTheQuery(name));
                changeTheDrive(u"/api/msd/remove"_s, which, u"delete what was written of it"_s,
                               [this, name, mayBe, then](const std::optional<base::Error>& refused) {
                                   if (refused) {
                                       then(mayBe(*refused));
                                       return;
                                   }
                                   listedUntil(name, m_pace.listedWithinMs, Until::Gone, 0,
                                               [mayBe, then](const base::Result<ListedNow>& gone) {
                                                   if (!gone) {
                                                       then(mayBe(gone.error()));
                                                   } else if (gone.value().image) {
                                                       then(u"What was written of it is on the KVM still: delete it "
                                                            u"there."_s);
                                                   } else if (!gone.value().whole) {
                                                       then(mayBe(listNotReadWhole()));
                                                   } else {
                                                       then(std::nullopt);
                                                   }
                                               });
                               });
            });
    });
}

void KvmdDevice::uploadOver(const std::shared_ptr<Upload>& upload, const base::Result<MediaUploaded>& result)
{
    if (m_upload == upload) {
        m_upload.reset();
    }
    if (auto done = std::exchange(upload->done, {})) {
        done(result);
    }
}

} // namespace qkvm::core
