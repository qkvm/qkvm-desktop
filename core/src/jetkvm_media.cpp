#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/jetkvm_media.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <QJsonObject>
#include <QPointer>

#include <qkvm/core/kvmd_media.h>

// A JetKVM's virtual media (the web's 0.3.0, A3: jetkvm.py's media parts and server.py's media routes, whose checks and
// words a KVM of this PC's own wall has here, the wall being its own server): what is in its drive, kept as the web
// keeps it, and what is done to it, over the connection its card holds. Never mountWithHTTP nor checkMountUrl, nor its
// built-in images, nor setMassStorageMode: nothing is fetched from an address.

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

// What a drive is called in a sentence (server.py MEDIA_DRIVE_SAID).
QString driveCalled(const QString& drive)
{
    return drive == "cd"_L1 ? u"a CD-ROM drive"_s : drive == "flash"_L1 ? u"a USB flash drive"_s : u"a disk"_s;
}

base::Error notOneOfItsImages()
{
    return invalid(u"That is not an image of the JetKVM's that qKVM lists"_s);
}

base::Error connectionEnded()
{
    return {"dropped", "The connection to the JetKVM ended"};
}

} // namespace

QString JetKvmDevice::mediaKey() const
{
    return u"jetkvm\n"_s + m_where.address.toString(QUrl::FullyEncoded) + u'\n' + m_machine;
}

std::optional<MediaOffer> JetKvmDevice::mediaOffer() const
{
    if (!m_vmKnown) {
        return std::nullopt; // (not said yet, or an older JetKVM that has no such methods)
    }
    // (Restarted from here where its ATX extension is on, as the last look found it: the web's "reset" in its
    // abilities.)
    return jetkvm::offerOf(m_status && m_status->powerIsKnown);
}

std::optional<MountedMedia> JetKvmDevice::mediaNow() const
{
    if (!m_vmKnown) {
        return std::nullopt;
    }
    return jetkvm::mountedOf(m_vm, m_listed, kvmd::MediaNotes::program(), mediaKey());
}

void JetKvmDevice::vmSaid(const base::Result<json::Value>& said)
{
    // (What its card says of it is read from this when it is asked: status().)
    if (!said) {
        if (said.error().code != "action_failed" || said.error().message != "Method not found") {
            return; // (no answer: what was known stays)
        }
        m_vmKnown = false;
        m_vm = {};
        return;
    }
    m_vmKnown = true;
    m_vm = said.value();
}

void JetKvmDevice::withMediaConnection(std::function<void(base::Result<Connected>)> then)
{
    // A person asked for it: tried now, whatever hold stands (the web's _connected, urgent). (One its own page took
    // meanwhile refuses every call: "in_use", the web's IN_USE of a session taken.)
    tryNow();
    m_viewer->connected([then = std::move(then)](const base::Result<QPointer<JetKvmConnection>>& on) {
        if (!on) {
            then(on.error());
        } else if (on.value() == nullptr) {
            then(connectionEnded());
        } else {
            then(on.value());
        }
    });
}

void JetKvmDevice::callOver(const Connected& on, const QString& method, const QJsonObject& params,
                            std::function<void(base::Result<json::Value>)> then, std::optional<int> withinMs)
{
    if (on == nullptr) {
        then(connectionEnded());
        return;
    }
    on->call(method, params, std::move(then), withinMs);
}

void JetKvmDevice::askVm(const Connected& on, std::function<void(base::Result<std::optional<MountedMedia>>)> then)
{
    callOver(on, u"getVirtualMediaState"_s, {},
             [self = QPointer<JetKvmDevice>(this), then = std::move(then)](const base::Result<json::Value>& said) {
                 if (self == nullptr) {
                     return;
                 }
                 if (!said) {
                     then(said.error());
                     return;
                 }
                 self->vmSaid(said);
                 then(self->mediaNow());
             });
}

void JetKvmDevice::listFiles(const Connected& on, std::function<void(base::Result<jetkvm::Files>)> then)
{
    callOver(on, u"listStorageFiles"_s, {},
             [self = QPointer<JetKvmDevice>(this), then = std::move(then)](const base::Result<json::Value>& said) {
                 if (self == nullptr) {
                     return;
                 }
                 if (!said) {
                     then(said.error());
                     return;
                 }
                 jetkvm::Files files = jetkvm::filesOf(said.value(), kvmd::MediaNotes::program(), self->mediaKey());
                 self->m_listed = files.listed;
                 then(std::move(files));
             });
}

void JetKvmDevice::askMedia(std::function<void(base::Result<MediaList>)> done)
{
    withMediaConnection([self = QPointer<JetKvmDevice>(this),
                         done = std::move(done)](const base::Result<Connected>& on) {
        if (self == nullptr) {
            return;
        }
        if (!on) {
            done(on.error());
            return;
        }
        self->askVm(on.value(), [self, on = on.value(), done](const base::Result<std::optional<MountedMedia>>& now) {
            if (self == nullptr) {
                return;
            }
            if (!now) {
                done(now.error());
                return;
            }
            self->listFiles(on, [self, on, done](const base::Result<jetkvm::Files>& files) {
                if (self == nullptr) {
                    return;
                }
                if (!files) {
                    done(files.error());
                    return;
                }
                callOver(on, u"getStorageSpace"_s, {},
                         [self, files = files.value(), done](const base::Result<json::Value>& said) {
                             if (self == nullptr) {
                                 return;
                             }
                             MediaList list;
                             list.images = files.images;
                             list.leftOut = files.leftOut;
                             // (A list not read whole is never shown as all there is: review cx4-1 L5, an empty
                             // storage.)
                             if (!files.whole) {
                                 list.readWhole = false;
                                 list.notes.append(kvmd::notReadWholeNote(QString::fromStdString(
                                     jetkvm::notReadWhole(u"not every image on it may be shown here"_s).message)));
                             }
                             if (said) {
                                 const jetkvm::Space space = jetkvm::spaceOf(said.value());
                                 if (space.used && space.free) {
                                     list.free = space.free;
                                     list.size = *space.used + *space.free;
                                 }
                             } else if (said.error().code == "in_use" || said.error().code == "dropped") {
                                 done(said.error());
                                 return;
                             }
                             list.mounted = self->mediaNow();
                             list.offers = self->mediaOffer().value_or(jetkvm::offerOf(false));
                             list.uploading = self->uploading();
                             done(std::move(list));
                         });
            });
        });
    });
}

void JetKvmDevice::mountMedia(const MediaMount& mount, std::function<void(base::Result<MediaDone>)> done)
{
    const QString& named = mount.machine;
    // As the server takes it (server.py, media_mount), by what the last look found its drive can do; and then as the
    // driver takes it (jetkvm.py, media_mount).
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
    } else if (!jetkvm::isOneName(mount.image) || mount.image.endsWith(jetkvm::partialEnd)) {
        refused = notOneOfItsImages();
    }
    // (A CD-ROM drive or a disk, read-only: the offer's, above. The web's driver checks that a second time; here it is
    // that one rule.)
    if (refused) {
        later([done = std::move(done), why = *refused] { done(why); });
        return;
    }
    withMediaConnection([self = QPointer<JetKvmDevice>(this), mount, restart,
                         done = std::move(done)](const base::Result<Connected>& on) {
        if (self == nullptr) {
            return;
        }
        if (!on) {
            done(on.error());
            return;
        }
        self->listFiles(on.value(), [self, on = on.value(), mount, restart,
                                     done](const base::Result<jetkvm::Files>& files) {
            if (self == nullptr) {
                return;
            }
            if (!files) {
                done(files.error());
                return;
            }
            const QList<MediaImage>& images = files.value().images;
            const auto found =
                std::ranges::find_if(images, [&mount](const MediaImage& image) { return image.id == mount.image; });
            if (found == images.end()) {
                done(changedMeanwhile(
                    u"%1 is not on the JetKVM's own storage any more: look again."_s.arg(mount.image)));
                return;
            }
            if (mount.drive == "cd"_L1 && found->size && *found->size < kvmd::leastOfACd) {
                done(invalid(u"%1 is %2 bytes, and a CD-ROM drive takes an image of %3 bytes at least (300 sectors of "
                             u"2,048 bytes): mount it as a disk."_s.arg(mount.image, kvmd::withCommas(*found->size),
                                                                        kvmd::withCommas(kvmd::leastOfACd))));
                return;
            }
            self->mountFound(on, mount, restart, done);
        });
    });
}

void JetKvmDevice::mountFound(const Connected& on, const MediaMount& mount, bool restart,
                              const std::function<void(base::Result<MediaDone>)>& done)
{
    // (That image of its storage, as that drive: one mounted from an address that goes by the same name is not it.)
    const auto asAsked = [here = QPointer<JetKvmDevice>(this), image = mount.image,
                          drive = mount.drive](const std::optional<MountedMedia>& now) {
        return now && now->image == image && now->drive == drive && here != nullptr &&
               jetkvm::fromItsStorage(here->m_vm);
    };
    askVm(on, [self = QPointer<JetKvmDevice>(this), on, mount, restart, asAsked,
               done](const base::Result<std::optional<MountedMedia>>& now) {
        if (self == nullptr) {
            return;
        }
        if (!now) {
            done(now.error());
            return;
        }
        if (now.value()) {
            if (now.value()->image == kvmd::unreadDrive) {
                done(jetkvm::driveNotRead());
                return;
            }
            if (asAsked(now.value())) {
                done(MediaDone{u"It was mounted so already."_s, now.value()});
                return;
            }
            done(changedMeanwhile(
                u"%1 is mounted on it now: unmount it first."_s.arg(kvmd::opensASentence(*now.value()))));
            return;
        }
        const QJsonObject asked{{u"filename"_s, mount.image},
                                {u"mode"_s, mount.drive == "cd"_L1 ? u"CDROM"_s : u"Disk"_s}};
        callOver(
            on, u"mountWithStorage"_s, asked,
            [self, on, mount, restart, asAsked, done](const base::Result<json::Value>& put) {
                if (self == nullptr) {
                    return;
                }
                if (!put) {
                    done(jetkvm::notSent(put.error()));
                    return;
                }
                self->m_statusAt = self->now() - self->m_pace.freshMs - 1;
                self->askVm(on, [self, mount, restart, asAsked,
                                 done](const base::Result<std::optional<MountedMedia>>& after) {
                    if (self == nullptr) {
                        return;
                    }
                    if (!after) {
                        done(after.error());
                        return;
                    }
                    if (!asAsked(after.value())) {
                        done(base::Error{"action_failed", "The JetKVM does not say it has the image in its drive as it "
                                                          "was asked: look at it on its own page."});
                        return;
                    }
                    if (!restart) {
                        done(MediaDone{{}, after.value()});
                        return;
                    }
                    self->restartForMedia(mount.machine, [self, done](const base::Result<QString>& said) {
                        if (!said) {
                            done(said.error());
                            return;
                        }
                        done(MediaDone{said.value(), self != nullptr ? self->mediaNow() : std::nullopt});
                    });
                });
            },
            self->m_pace.mountWithinMs);
    });
}

void JetKvmDevice::restartForMedia(const QString& named, std::function<void(base::Result<QString>)> then)
{
    // A machine that cannot be told to start from it, restarted for its firmware to find it: a press of its ATX
    // extension's button, by every rule of one (act). Switched on where it is off, else reset (the web's
    // `media_restart`). What came of it after the mount is said after the image is mounted (server.py, media_failed).
    const auto notRestarted = [named](base::Error why) {
        why.message = u"The image is mounted, and %1 was not restarted: "_s.arg(named).toStdString() + why.message;
        return why;
    };
    if (!m_status || !m_status->powerIsKnown) {
        later([then = std::move(then),
               why = notRestarted(invalid(u"This JetKVM has no ATX power extension switched on to restart the "
                                          u"machine with"_s))] { then(why); });
        return;
    }
    // Whether it is off, asked anew now, and not as the last look saw it: the mount may have taken half a minute, and
    // a machine switched on at its front panel meanwhile is shut down by its power button, or switched off during its
    // POST (outside review 4.3). Where it does not say so plainly, in time, it is reset, as a kvmd's is where its board
    // does not say (a reset does nothing to a machine that is off), and said so. "Off" is its power light's word: one
    // whose disk light is on at that moment is on, its power light's lead not connected, and is reset too (review
    // cx4-1 L4).
    withMediaConnection([self = QPointer<JetKvmDevice>(this), notRestarted,
                         then = std::move(then)](const base::Result<Connected>& on) {
        if (self == nullptr) {
            return;
        }
        if (!on) {
            then(notRestarted(on.error()));
            return;
        }
        callOver(on.value(), u"getATXState"_s, {}, [self, notRestarted, then](const base::Result<json::Value>& said) {
            if (self == nullptr) {
                return;
            }
            const json::Value* power = said && said.value().isObject() ? said.value().find(u"power") : nullptr;
            const json::Value* disk = said && said.value().isObject() ? said.value().find(u"hdd") : nullptr;
            const bool diskOn = disk != nullptr && disk->isFlag() && disk->flag;
            const bool known = (power != nullptr && power->isFlag()) || diskOn;
            const bool wasOff = power != nullptr && power->isFlag() && !power->flag && !diskOn;
            self->act(Action::plain(wasOff ? u"atx_power"_s : u"atx_reset"_s), [wasOff, known, notRestarted, then](
                                                                                   const base::Result<Done>& pressed) {
                if (!pressed) {
                    then(notRestarted(pressed.error()));
                    return;
                }
                const QString words =
                    wasOff  ? u"It was off, and was switched on with its power button"_s
                    : known ? u"It was restarted with its reset button"_s
                            : u"It was reset with its reset button (whether it was on was not known: if it was off, "
                              u"it is off still)"_s;
                then(words +
                     u": pick the JetKVM's drive in its boot menu where it does not start from it by itself."_s);
            });
        });
    });
}

void JetKvmDevice::unmountMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done)
{
    withMediaConnection([self = QPointer<JetKvmDevice>(this), image,
                         done = std::move(done)](const base::Result<Connected>& on) {
        if (self == nullptr) {
            return;
        }
        if (!on) {
            done(on.error());
            return;
        }
        self->askVm(on.value(), [self, on = on.value(), image,
                                 done](const base::Result<std::optional<MountedMedia>>& now) {
            if (self == nullptr) {
                return;
            }
            if (!now) {
                done(now.error());
                return;
            }
            if (!now.value()) {
                done(MediaDone{u"nothing was mounted"_s, std::nullopt});
                return;
            }
            if (now.value()->image == kvmd::unreadDrive) {
                done(jetkvm::driveNotRead());
                return;
            }
            if (now.value()->image != image) {
                // (One from an address, or another that goes by an id of its own, is told by that id, which says
                // nothing to a person.)
                const QString asked =
                    image.startsWith(u"(an address)/"_s) ? u"the image from an address it had when it was looked at"_s
                    : jetkvm::isOneName(image)           ? image
                                                         : u"the image it had in its drive when it was looked at"_s;
                done(changedMeanwhile(
                    u"%1 is mounted on it now, and not %2: nothing was taken out. Look at it again."_s.arg(
                        kvmd::opensASentence(*now.value()), asked)));
                return;
            }
            callOver(
                on, u"unmountImage"_s, {},
                [self, on, done](const base::Result<json::Value>& out) {
                    if (self == nullptr) {
                        return;
                    }
                    if (!out) {
                        done(jetkvm::notSent(out.error()));
                        return;
                    }
                    self->m_statusAt = self->now() - self->m_pace.freshMs - 1;
                    self->askVm(on, [done](const base::Result<std::optional<MountedMedia>>& after) {
                        if (!after) {
                            done(after.error());
                            return;
                        }
                        if (after.value()) {
                            done(base::Error{"action_failed", "The JetKVM says the image is still in its drive: look "
                                                              "at it on its own page."});
                            return;
                        }
                        done(MediaDone{});
                    });
                },
                self->m_pace.mountWithinMs);
        });
    });
}

void JetKvmDevice::deleteMedia(const QString& image, std::function<void(base::Result<MediaDone>)> done)
{
    const QString base = image.endsWith(jetkvm::partialEnd) ? image.chopped(jetkvm::partialEnd.size()) : image;
    if (!jetkvm::isOneName(base)) {
        later([done = std::move(done)] { done(notOneOfItsImages()); });
        return;
    }
    const auto listed = [image](const jetkvm::Files& files) {
        return std::ranges::any_of(files.images, [&image](const MediaImage& one) { return one.id == image; });
    };
    withMediaConnection([self = QPointer<JetKvmDevice>(this), image, listed,
                         done = std::move(done)](const base::Result<Connected>& on) {
        if (self == nullptr) {
            return;
        }
        if (!on) {
            done(on.error());
            return;
        }
        self->listFiles(
            on.value(), [self, on = on.value(), image, listed, done](const base::Result<jetkvm::Files>& files) {
                if (self == nullptr) {
                    return;
                }
                if (!files) {
                    done(files.error());
                    return;
                }
                if (!listed(files.value())) {
                    // (Not found in a list that was not read whole: not known to be gone. Outside review 4.6.)
                    if (!files.value().whole) {
                        done(jetkvm::notReadWhole(u"whether %1 is there is not known"_s.arg(image)));
                        return;
                    }
                    done(MediaDone{u"it was not on the JetKVM any more"_s, std::nullopt});
                    return;
                }
                self->askVm(on, [self, on, image, listed, done](const base::Result<std::optional<MountedMedia>>& now) {
                    if (self == nullptr) {
                        return;
                    }
                    if (!now) {
                        done(now.error());
                        return;
                    }
                    // (Not while what is in its drive cannot be read: it may be this one. Outside review 4.7.)
                    if (now.value() && now.value()->image == kvmd::unreadDrive) {
                        done(jetkvm::driveNotRead());
                        return;
                    }
                    // (Nor while what is in it is an image of its storage by a name qKVM does not read as one of its
                    // images: it may be this one. Review cx4-1 M2.)
                    if (now.value() && kvmd::isNameless(now.value()->image)) {
                        const QString inIt = kvmd::opensASentence(*now.value());
                        done(base::Error{"busy", u"%1 is in its drive, by a name qKVM does not read as one of its "
                                                 u"images, so it may be %2: unmount it first."_s.arg(inIt, image)
                                                     .toStdString()});
                        return;
                    }
                    if (now.value() && now.value()->image == image && jetkvm::fromItsStorage(self->m_vm)) {
                        done(base::Error{"busy", u"%1 is mounted: unmount it first."_s.arg(image).toStdString()});
                        return;
                    }
                    callOver(on, u"deleteStorageFile"_s, QJsonObject{{u"filename"_s, image}},
                             [self, on, image, listed, done](const base::Result<json::Value>& gone) {
                                 if (self == nullptr) {
                                     return;
                                 }
                                 if (!gone) {
                                     done(gone.error());
                                     return;
                                 }
                                 kvmd::MediaNotes::program().forget(self->mediaKey(), image);
                                 self->listFiles(on, [image, listed, done](const base::Result<jetkvm::Files>& after) {
                                     if (!after) {
                                         done(after.error());
                                         return;
                                     }
                                     if (listed(after.value())) {
                                         done(base::Error{"action_failed", "The JetKVM says the image is still on its "
                                                                           "storage: look at it on its own page."});
                                         return;
                                     }
                                     if (!after.value().whole) {
                                         done(jetkvm::notReadWhole(u"whether %1 is gone is not known"_s.arg(image)));
                                         return;
                                     }
                                     done(MediaDone{});
                                 });
                             });
                });
            });
    });
}

} // namespace qkvm::core
