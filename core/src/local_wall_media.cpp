#include <qkvm/core/local_wall.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <QTimer>

// This PC's own wall as the place of its kvmd KVMs', JetKVMs' and Proxmox guests' virtual media (core::MediaPlace; GAPS
// A2 and A3, wave 2): what the web's server does before its driver is asked (server.py, media_target, media_named,
// media_held, and what the driver offers), done here before the machine's driver is, which does the rest (OwnMedia:
// kvmd_media.cpp, jetkvm_media.cpp, proxmox_media.cpp). There are no accounts here: whoever sits at this PC may. (So a
// mount or an unmount that drops a JetKVM's keyboard and mouse for a moment is not held for a window that has them: the
// web holds it for somebody else's, and its asker is told in the box, as here.)

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// What a box that lists a KVM's media is bound to (MediaList::boundTo): where the KVM is, and the certificate it is
// known by; of a Proxmox guest, its number as well (every guest of one Proxmox has its address and its certificate);
// of a JetKVM, the mark of the number it gives for itself (J5: one at its address that gives another is another, over
// plain HTTP too, where no certificate tells two apart). A new name or group leaves it as it was; another address,
// another guest, another JetKVM taken for it, or another certificate said yes to, does not.
QString boundOf(const OwnKvm& kvm)
{
    return kvm.address + u'\n' + (kvm.knownByCertificate ? QString::fromLatin1(kvm.certificate.toHex()) : QString()) +
           (kvm.kind == "proxmox"_L1 ? u"\n"_s + QString::number(kvm.vmid) : QString()) +
           (kvm.kind == "jetkvm"_L1 ? u"\n"_s + kvm.number : QString());
}

} // namespace

LocalWall::OfMedia LocalWall::mediaOf(const QString& machine, const QString& named, const char* what)
{
    // A machine of the wall, asked what it is, of a family that has virtual media and has some now (its drive is
    // switched on: by the driver's last word on it), called as the question named it.
    OfMedia of;
    of.one = held(machine);
    const auto kvm = std::ranges::find_if(kvms(), [&machine](const OwnKvm& known) { return known.id == machine; });
    if (of.one == nullptr || kvm == kvms().end()) {
        of.refused = base::Error{"not_found", "Unknown device"};
        return of;
    }
    if (!of.one->asked) {
        of.refused = notAskedYet();
        return of;
    }
    of.called = kvm->name.isEmpty() ? hostOf(*kvm) : kvm->name;
    of.inASentence = calledInASentence(*kvm);
    of.driver = dynamic_cast<OwnMedia*>(of.one->device.get());
    if (of.driver == nullptr || !of.driver->mediaOffer()) {
        of.refused = base::Error{"no_virtual_media", u"%1 has no virtual media"_s.arg(of.called).toStdString()};
        return of;
    }
    if (what != nullptr && named != of.called) {
        of.refused = base::Error{"changed",
                                 u"The confirmation named %1, and this machine is called %2 now. Nothing was %3: look "
                                 u"at it again."_s.arg(named, of.called, QLatin1StringView(what))
                                     .toStdString()};
    }
    return of;
}

std::optional<base::Error> LocalWall::mediaHeld(const OfMedia& of, const QString& image, bool restart) const
{
    // One thing at a time to a machine, as for an action; nor anything of the image that is being uploaded to it; and
    // a restart, a power button's, not while somebody has its keyboard and mouse live (server.py, media_held,
    // keyboard_held, in its words: whoever has them here is a window of this PC, as there are no accounts). Putting an
    // image in alone touches no keyboard. (Nor are they given to anybody while it goes, a guest's: actedOn.)
    if (of.one->busy) {
        return base::Error{
            "busy", u"Something else is being done to %1 at this moment. Wait for it to finish."_s.arg(of.inASentence)
                        .toStdString()};
    }
    if (const std::optional<MediaUploading> up = of.driver->uploading();
        up && !image.isEmpty() && (image == up->name || image == up->name + ".incomplete"_L1)) {
        return base::Error{
            "busy",
            u"%1 is being uploaded to %2 at this moment. Wait for it to finish."_s.arg(image, of.called).toStdString()};
    }
    if (restart && of.one->live != nullptr && of.one->live->keyboardHeld()) {
        return base::Error{"busy", u"A window of this PC has %1's keyboard and mouse over a live line at this moment. "
                                   u"Wait until they are given back."_s.arg(of.called)
                                       .toStdString()};
    }
    return std::nullopt;
}

std::optional<base::Error> LocalWall::movedSince(const QString& machine, const QString& bound) const
{
    const auto kvm = std::ranges::find_if(kvms(), [&machine](const OwnKvm& known) { return known.id == machine; });
    if (kvm == kvms().end()) {
        return base::Error{"not_found", "Unknown device"};
    }
    if (boundOf(*kvm) == bound) {
        return std::nullopt;
    }
    return base::Error{"changed", u"%1 is not the KVM this box was opened for any more: its address, or what it is "
                                  u"known by, changed. Nothing was done: open its media again."_s
                                      .arg(kvm->name.isEmpty() ? hostOf(*kvm) : kvm->name)
                                      .toStdString()};
}

void LocalWall::askMedia(const QString& machine, std::function<void(base::Result<MediaList>)> done)
{
    const OfMedia of = mediaOf(machine, {}, nullptr);
    if (of.refused) {
        later([done = std::move(done), why = *of.refused] { done(why); });
        return;
    }
    // (Said with what the KVM is bound to as it is asked: its driver, made of that, answers, or nobody does.)
    const auto kvm = std::ranges::find_if(kvms(), [&machine](const OwnKvm& known) { return known.id == machine; });
    of.driver->askMedia(
        owedBy<MediaList>(*of.one, [bound = boundOf(*kvm), done = std::move(done)](base::Result<MediaList> list) {
            if (list) {
                list.value().boundTo = bound;
            }
            done(std::move(list));
        }));
}

void LocalWall::changeMedia(const QString& machine, const QString& image, const QString& named, const char* what,
                            bool restart,
                            const std::function<void(OwnMedia&, std::function<void(base::Result<MediaDone>)>)>& change,
                            std::function<void(base::Result<MediaDone>)> done)
{
    const OfMedia of = mediaOf(machine, named, what);
    std::optional<base::Error> refused = of.refused;
    if (!refused) {
        refused = mediaHeld(of, image, restart);
    }
    if (refused) {
        later([done = std::move(done), why = *refused] { done(why); });
        return;
    }
    // Held as an action is, until it is answered, whatever replaces its driver meanwhile (act).
    of.one->busy = true;
    change(*of.driver, owedBy<MediaDone>(*of.one, [wall = QPointer<LocalWall>(this), machine,
                                                   done = std::move(done)](base::Result<MediaDone> result) {
        if (Held* still = wall != nullptr ? wall->held(machine) : nullptr) {
            still->busy = false;
        }
        done(std::move(result));
    }));
}

void LocalWall::mountMedia(const QString& machine, const MediaMount& mount,
                           std::function<void(base::Result<MediaDone>)> done)
{
    changeMedia(
        machine, mount.image, mount.machine, "mounted", mount.restart || mount.boot == "once"_L1,
        [mount](OwnMedia& driver, std::function<void(base::Result<MediaDone>)> then) {
            driver.mountMedia(mount, std::move(then));
        },
        std::move(done));
}

void LocalWall::unmountMedia(const QString& machine, const QString& image, const QString& named,
                             const QString& /*listed*/, std::function<void(base::Result<MediaDone>)> done)
{
    // (Not held for an upload's image: whatever is in the drive is no image on its way.)
    changeMedia(
        machine, {}, named, "taken out", false,
        [image](OwnMedia& driver, std::function<void(base::Result<MediaDone>)> then) {
            driver.unmountMedia(image, std::move(then));
        },
        std::move(done));
}

void LocalWall::deleteMedia(const QString& machine, const QString& image, const QString& named,
                            const QString& /*listed*/, std::function<void(base::Result<MediaDone>)> done)
{
    // (As the web's server: of a machine whose images are not its own to delete, none is: a guest's are Proxmox's.)
    if (const OfMedia of = mediaOf(machine, {}, nullptr);
        !of.refused && !of.driver->mediaOffer().value_or(MediaOffer{}).canDelete) {
        later([done = std::move(done),
               why = base::Error{"invalid",
                                 u"%1 has no images of its own that qKVM deletes"_s.arg(of.called).toStdString()}] {
            done(why);
        });
        return;
    }
    changeMedia(
        machine, image, named, "deleted", false,
        [image](OwnMedia& driver, std::function<void(base::Result<MediaDone>)> then) {
            driver.deleteMedia(image, std::move(then));
        },
        std::move(done));
}

std::function<void()> LocalWall::uploadMedia(const QString& machine, const QString& name, const QString& named,
                                             const QString& /*listed*/, std::unique_ptr<QIODevice> image,
                                             std::function<void(base::Result<MediaUploaded>)> done)
{
    const OfMedia of = mediaOf(machine, named, "uploaded");
    std::optional<base::Error> refused = of.refused;
    if (!refused && !of.driver->mediaOffer().value_or(MediaOffer{}).upload) {
        // (As the web's server: a guest's images are on Proxmox, and none is carried there from here.)
        refused = base::Error{"invalid", u"Images are not carried through qKVM to %1"_s.arg(of.called).toStdString()};
    }
    if (!refused) {
        if (const std::optional<MediaUploading> up = of.driver->uploading()) {
            refused = base::Error{"busy", u"%1 is being uploaded to %2 at this moment, by %3. Wait for it to finish."_s
                                              .arg(up->name, of.called, up->by)
                                              .toStdString()};
        }
    }
    if (refused) {
        later([done = std::move(done), why = *refused] { done(why); });
        return [] {};
    }
    // While it goes, the KVM is not changed or taken off the wall: what is under way to it is not cut off.
    of.one->uploading = true;
    return of.driver->uploadMedia(
        name, std::move(image),
        owedBy<MediaUploaded>(*of.one, [wall = QPointer<LocalWall>(this), machine,
                                        done = std::move(done)](base::Result<MediaUploaded> result) {
            if (Held* still = wall != nullptr ? wall->held(machine) : nullptr) {
                still->uploading = false;
            }
            done(std::move(result));
        }));
}

void LocalWall::askUpload(const QString& machine, std::function<void(base::Result<std::optional<MediaUploading>>)> done)
{
    const OfMedia of = mediaOf(machine, {}, nullptr);
    if (of.refused) {
        later([done = std::move(done), why = *of.refused] { done(why); });
        return;
    }
    later([done = std::move(done), up = of.driver->uploading()] { done(up); });
}

} // namespace qkvm::core
