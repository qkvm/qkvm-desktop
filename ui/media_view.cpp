#include "media_view.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// The box's words are the page's, translated under one heading.
QString words(const char* text)
{
    return QCoreApplication::translate("Media", text);
}

// How often an upload's progress is asked, and how soon a look that got no answer is asked again, once.
constexpr int progressEveryMs = 1000;
constexpr int lookAgainMs = 5000;
// How long a chosen file may take to be opened before the box says that it is being opened.
constexpr int openingSaidAfterMs = 500;
// What a CD-ROM drive of Linux's USB gadget (a KVM's drive) gives its machine of an image at the most (kvm.MOST_CD).
constexpr qint64 mostOfACd = (qint64{256} * 60 * 75 - 1) * 2048;

QString join(QStringList parts, const QString& with = u" · "_s)
{
    parts.removeAll(QString());
    return parts.join(with);
}

QString driveSaid(const QString& drive)
{
    if (drive == "cd"_L1) {
        return words("CD-ROM drive");
    }
    if (drive == "flash"_L1) {
        return words("USB flash drive");
    }
    // (One this program does not know, added since, is said in the server's word, as it came: README, "Clients and
    // versions".)
    return drive == "disk"_L1 ? words("disk") : drive;
}

QString whereSaid(const QString& where)
{
    if (where == "proxmox"_L1) {
        return words("On the storages of its Proxmox that the token may see");
    }
    if (where == "device"_L1) {
        return words("On the KVM's own storage");
    }
    return where == "qkvm"_L1 ? words("In the folder qKVM serves them from") : QString();
}

QString noneSaid(const QString& where)
{
    if (where == "proxmox"_L1) {
        return words("No ISO images here. They are put on a storage of the Proxmox that the token may see "
                     "(Datastore.Audit) on Proxmox itself.");
    }
    if (where == "device"_L1) {
        return words("No images on the KVM's own storage yet. Upload one: it is carried through qKVM as it is read, "
                     "and kept nowhere on the way.");
    }
    if (where == "qkvm"_L1) {
        return words("No ISO images in the folder for server images. Put them there on the computer qKVM runs on "
                     "(Settings, Server images).");
    }
    return words("No images here.");
}

// What the box says at its foot of a machine's drive, by what it offers (app.js, mediaFoot).
QString footOf(const core::MediaOffer& offers, const QString& servedFrom)
{
    if (offers.where == "proxmox"_L1) {
        return words("An image goes into the guest's CD-ROM drive, read-only. Proxmox has no start \"once\": qKVM "
                     "starts the guest anew with the CD-ROM drive first and puts its boot order back after, so a "
                     "restart from inside the guest starts from the image again while it is in the drive. A restart "
                     "stops the guest at once and starts it again.");
    }
    if (offers.where == "qkvm"_L1) {
        return join({words("An image goes into the server's virtual CD-ROM drive, read-only."),
                     (servedFrom.isEmpty() ? words("Its controller reads it from qKVM for as long as it is mounted, "
                                                   "by an address made for that mount alone: keep qKVM running "
                                                   "until it is unmounted.")
                                           : words("Its controller reads it from qKVM at %1 for as long as it is "
                                                   "mounted, by an address made for that mount alone: keep qKVM "
                                                   "running until it is unmounted.")
                                                 .arg(servedFrom)),
                     words("Most controllers do not check qKVM's certificate: on a network you do not trust, an "
                           "image could be changed on its way.")},
                    u" "_s);
    }
    return join(
        {offers.drives.contains(u"flash"_s)
             ? words("An image goes in as a CD-ROM drive (read-only) or a USB flash drive (read-only, unless "
                     "you choose it writable: then the machine can change and delete what is on it).")
             : words("An image goes in as a CD-ROM drive or a disk, both read-only."),
         offers.dropsInput ? words("A switch between the two drops its keyboard and mouse for a moment.") : QString(),
         !offers.boot.isEmpty() ? QString()
         : offers.restart       ? words("A KVM cannot tell the machine to start from it: restart it from "
                                              "here, or pick the KVM's drive in the machine's boot menu.")
                                : words("A KVM cannot tell the machine to start from it: pick the KVM's "
                                              "drive in the machine's boot menu."),
         words("One image at a time. Images stay on the KVM, readable to anyone with its own password: "
               "delete them after use.")},
        u" "_s);
}

// What is in a machine's drive, in one line (app.js, mountedText).
QString mountedText(const std::optional<core::MountedMedia>& mounted)
{
    if (!mounted) {
        return words("Nothing is mounted.");
    }
    const QString at = mounted->at ? words(" at %1").arg(QLocale().toString(
                                         QDateTime::fromSecsSinceEpoch(*mounted->at).time(), QLocale::ShortFormat))
                                   : QString();
    return join({
        mounted->where.isEmpty() ? mounted->name : words("%1 on %2").arg(mounted->name, mounted->where),
        mounted->drive != "cd"_L1 ? driveSaid(mounted->drive) : QString(),
        mounted->readOnly ? words("read-only") : words("writable"),
        mounted->first == std::optional<bool>(true)    ? words("first in its boot order")
        : mounted->first == std::optional<bool>(false) ? words("not first in its boot order")
                                                       : QString(),
        mounted->by.isEmpty() ? QString() : words("mounted by %1%2").arg(mounted->by, at),
    });
}

// Who is signed in at which server: an answer to what was asked as somebody else, or of another server, is nobody's.
QByteArray identityOf(const core::ServerClient* client)
{
    return client == nullptr ? QByteArray()
                             : QCryptographicHash::hash(client->address().toEncoded() + '\n' + client->session(),
                                                        QCryptographicHash::Sha256);
}

// Whether an error of something that was to be done is no answer of the server's: it may have been done (the page's
// `unanswered`). The server's own words are said as they are, its `timeout` and `dropped` among them, which say so.
bool unanswered(const base::Error& error)
{
    return error.code == core::ServerClient::unreachable || error.code == "internal";
}

} // namespace

MediaView::MediaView(QObject* parent) : QObject(parent)
{
    m_again.setSingleShot(true);
    // A file that is not open a moment after it was chosen: the box says that it is being opened.
    m_opening.setSingleShot(true);
    m_opening.setInterval(openingSaidAfterMs);
    connect(&m_opening, &QTimer::timeout, this, [this] {
        if (!m_openingName.isEmpty() && m_opened) {
            tell(words("Opening %1...").arg(m_openingName));
            emit changed();
        }
    });
    m_counting.setInterval(500);
    connect(&m_counting, &QTimer::timeout, this, &MediaView::countUp);
    m_progress.setInterval(progressEveryMs);
}

MediaView::~MediaView()
{
    letGo();
}

QString MediaView::bytesText(qint64 bytes)
{
    static const char* const units[] = {"bytes", "kB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1000 && unit < 4) {
        value /= 1000;
        ++unit;
    }
    const QString number = unit == 0 ? QString::number(bytes) : QString::number(value, 'f', value < 10 ? 1 : 0);
    return number + u' ' + QString::fromLatin1(units[unit]);
}

void MediaView::open(core::ServerClient* client, const QString& machine, const QString& name, const QString& key,
                     UnderWay* underWay, const QString& underWayKey)
{
    close();
    ++m_turn;
    m_client = client;
    m_identity = identityOf(client);
    m_machine = machine;
    m_name = name;
    m_key = key;
    m_underWay = underWay;
    m_underWayKey = underWayKey;
    m_opened = client != nullptr;
    m_shown.reset();
    m_picked.clear();
    m_drive = u"cd"_s;
    m_boot = u"none"_s;
    m_restart = false;
    m_where.clear();
    m_foot.clear();
    m_now = words("Looking...");
    m_nothing = words("Looking for images...");
    m_said.clear();
    m_saidBad = false;
    m_asked.reset();
    emit changed();
    if (m_opened) {
        load();
    }
}

void MediaView::rename(const QString& name)
{
    if (m_opened && name != m_name) {
        m_name = name;
        emit changed();
    }
}

void MediaView::close()
{
    if (!m_opened) {
        return;
    }
    m_opened = false;
    ++m_turn;
    m_again.stop();
    m_asked.reset();
    m_file.reset();
    ++m_openings;
    m_opening.stop();
    m_openingName.clear();
    // Closing the box stops an upload: what came of it is taken off (the server sees to that).
    if (m_stopUpload) {
        std::exchange(m_stopUpload, {})();
    }
    emit changed();
}

void MediaView::letGo()
{
    close();
    ++m_turn;
    // What is out is nobody's: what it holds of the machine is given back now, and its answer finds nothing.
    if (m_carrying != 0 && m_carryUnderWay != nullptr) {
        m_carryUnderWay->remove(m_carryKey);
    }
    m_carrying = 0;
    m_carryUnderWay = nullptr;
    m_counting.stop();
    m_progress.stop();
    m_client = nullptr;
    m_underWay = nullptr;
}

QString MediaView::title() const
{
    return words("Virtual media of %1").arg(m_name);
}

bool MediaView::isCurrent(quint64 turn) const
{
    return turn == m_turn && m_opened && m_client != nullptr && identityOf(m_client) == m_identity;
}

void MediaView::tell(const QString& text, bool bad)
{
    m_said = text;
    m_saidBad = !text.isEmpty() && bad;
}

const core::MediaImage* MediaView::picked() const
{
    if (!m_shown) {
        return nullptr;
    }
    const auto found =
        std::ranges::find_if(m_shown->images, [&](const core::MediaImage& one) { return one.id == m_picked; });
    return found == m_shown->images.end() ? nullptr : &*found;
}

void MediaView::load(bool retried)
{
    if (!m_opened || m_client == nullptr) {
        return;
    }
    // (A newer look outdates an answer still on its way, and owns the one asked again.)
    const quint64 look = ++m_look;
    m_again.stop();
    m_client->askMedia(m_machine, [guard = QPointer<MediaView>(this), turn = m_turn, look,
                                   retried](const base::Result<core::MediaList>& answer) {
        if (guard == nullptr || !guard->isCurrent(turn) || look != guard->m_look) {
            return;
        }
        guard->listed(answer, retried);
    });
}

void MediaView::listed(const base::Result<core::MediaList>& answer, bool retried)
{
    if (answer) {
        show(answer.value());
        const core::MediaList& list = answer.value();
        QStringList said = list.notes;
        if (!list.unseen.isEmpty()) {
            said.append(words("Not listed: %1").arg(list.unseen.join(u"; "_s)));
        }
        if (list.leftOut > 0) {
            said.append(list.leftOut == 1
                            ? words("1 entry was left out: no image that qKVM passes on")
                            : words("%1 entries were left out: no image that qKVM passes on").arg(list.leftOut));
        }
        if (list.uploading && !m_stopUpload) {
            said.append(words("%1 is being uploaded by %2: %3 of %4")
                            .arg(list.uploading->name, list.uploading->by, bytesText(list.uploading->sent),
                                 bytesText(list.uploading->size)));
        }
        tell(said.join(u". "_s), !list.unseen.isEmpty() || !list.notes.isEmpty());
    } else {
        // No answer this time: what the last look showed stays, but for what is in its drive, which is not known now;
        // it is asked again once, a little later.
        if (m_shown) {
            core::MediaList kept = *m_shown;
            kept.mounted.reset();
            show(kept);
            m_now = words("Not known just now: its controller did not answer.");
        } else {
            m_now.clear();
            m_nothing = words("No images to show.");
        }
        QString missed = QString::fromStdString(answer.error().message);
        while (!missed.isEmpty() && (missed.back() == u'.' || missed.back().isSpace())) {
            missed.chop(1);
        }
        tell(retried ? missed + u'.' : words("%1. Asked again in a moment.").arg(missed), true);
        if (!retried) {
            disconnect(&m_again, nullptr, this, nullptr);
            connect(&m_again, &QTimer::timeout, this, [this, turn = m_turn, look = m_look] {
                if (isCurrent(turn) && look == m_look) {
                    load(true);
                }
            });
            m_again.start(lookAgainMs);
        }
    }
    // What a thing done from the box said stays, where the look has nothing to say; and both, where it says something
    // amiss after what was done went well.
    if (m_keep) {
        const auto [message, bad] = *std::exchange(m_keep, std::nullopt);
        if (m_said.isEmpty()) {
            tell(message, bad);
        } else if (!message.isEmpty() && (bad || m_saidBad)) {
            tell(message + u' ' + m_said, true);
        }
    }
    fit();
    emit changed();
}

void MediaView::show(const core::MediaList& list)
{
    m_shown = list;
    const QString space = list.free && list.size
                              ? words(" · %1 free of %2").arg(bytesText(*list.free), bytesText(*list.size))
                              : QString();
    m_where = whereSaid(list.offers.where) + space;
    m_foot = footOf(list.offers, list.servedFrom);
    m_now = mountedText(list.mounted);
    if (picked() == nullptr) {
        m_picked.clear();
    }
    m_nothing = noneSaid(list.offers.where);
}

QVariantList MediaView::images() const
{
    QVariantList shown;
    if (!m_shown) {
        return shown;
    }
    for (const core::MediaImage& image : m_shown->images) {
        const QString hint = join({
            image.partial ? words("an upload of it was cut off: only to delete") : QString(),
            image.where,
            image.size ? bytesText(*image.size) : QString(),
            m_shown->mounted && m_shown->mounted->image == image.id ? words("mounted now") : QString(),
            image.sha256.isEmpty() ? QString() : words("uploaded from qKVM, SHA-256 %1...").arg(image.sha256.left(16)),
        });
        shown.append(QVariantMap{
            {u"id"_s, image.id}, {u"name"_s, image.name}, {u"hint"_s, hint}, {u"picked"_s, image.id == m_picked}});
    }
    return shown;
}

QVariantList MediaView::drives() const
{
    const core::MediaOffer offers = m_shown ? m_shown->offers : core::MediaOffer{};
    QVariantList shown;
    const std::pair<const char*, QString> all[] = {
        {"cd", words("CD-ROM drive, read-only")},
        {"flash", words("USB flash drive, read-only")},
        {"flash_rw", words("USB flash drive, writable")},
        {"disk", words("Disk, read-only")},
    };
    for (const auto& [value, label] : all) {
        const QString choice = QString::fromLatin1(value);
        const QString drive = choice.section(u'_', 0, 0);
        const bool enabled =
            offers.drives.contains(drive) && (!choice.endsWith("_rw"_L1) || offers.writable.contains(drive));
        shown.append(QVariantMap{{u"value"_s, choice}, {u"label"_s, label}, {u"enabled"_s, enabled}});
    }
    return shown;
}

QVariantList MediaView::boots() const
{
    const QStringList offered = m_shown ? m_shown->offers.boot : QStringList();
    return {
        QVariantMap{
            {u"value"_s, u"none"_s}, {u"label"_s, words("Leave its boot order as it is")}, {u"enabled"_s, true}},
        QVariantMap{{u"value"_s, u"once"_s},
                    {u"label"_s, words("Start from it once, now")},
                    {u"enabled"_s, offered.contains(u"once"_s)}},
        QVariantMap{{u"value"_s, u"until_unmounted"_s},
                    {u"label"_s, words("Start from it until it is unmounted")},
                    {u"enabled"_s, offered.contains(u"until_unmounted"_s)}},
    };
}

// What can be chosen, by what is so (app.js, fit): a choice that cannot be had is not kept.
void MediaView::fit()
{
    const core::MediaOffer offers = m_shown ? m_shown->offers : core::MediaOffer{};
    const QVariantList choices = drives();
    const auto enabled = [&](const QString& value) {
        return std::ranges::any_of(choices, [&](const QVariant& one) {
            return one.toMap().value(u"value"_s) == value && one.toMap().value(u"enabled"_s).toBool();
        });
    };
    if (!enabled(m_drive)) {
        m_drive = u"cd"_s;
        for (const QVariant& one : choices) {
            if (one.toMap().value(u"enabled"_s).toBool()) {
                m_drive = one.toMap().value(u"value"_s).toString();
                break;
            }
        }
    }
    if (m_boot != "none"_L1 && !offers.boot.contains(m_boot)) {
        m_boot = u"none"_s;
    }
    const bool boots = !offers.boot.isEmpty();
    if (m_boot == "once"_L1) {
        m_restart = true;
    }
    if (m_boot == "none"_L1 && boots) {
        m_restart = false;
    }
    // (Where it can be told to start from it, a restart goes with "until unmounted"; where it cannot, a KVM, on its
    // own.)
    if (!restartEnabled() && !boots) {
        m_restart = false;
    }
}

bool MediaView::restartEnabled() const
{
    const core::MediaOffer offers = m_shown ? m_shown->offers : core::MediaOffer{};
    return offers.restart && (offers.boot.isEmpty() || m_boot == "until_unmounted"_L1);
}

bool MediaView::canMount() const
{
    const core::MediaImage* image = picked();
    return image != nullptr && !image->partial;
}

bool MediaView::canDelete() const
{
    const core::MediaImage* image = picked();
    return m_shown && m_shown->offers.canDelete && image != nullptr &&
           !(m_shown->mounted && m_shown->mounted->image == image->id);
}

bool MediaView::canUpload() const
{
    return m_shown && m_shown->offers.upload;
}

bool MediaView::canUnmount() const
{
    return m_shown && m_shown->mounted.has_value();
}

QVariantMap MediaView::question() const
{
    if (!m_asked) {
        return {};
    }
    return {{u"title"_s, m_asked->title},
            {u"text"_s, m_asked->text},
            {u"yes"_s, m_asked->yes},
            {u"danger"_s, m_asked->danger}};
}

void MediaView::pick(const QString& id)
{
    if (!m_opened || busy() || m_shown == std::nullopt) {
        return;
    }
    m_picked = id;
    if (picked() == nullptr) {
        m_picked.clear();
    }
    fit();
    emit changed();
}

void MediaView::chooseDrive(const QString& value)
{
    if (!m_opened || busy()) {
        return;
    }
    m_drive = value;
    fit();
    emit changed();
}

void MediaView::chooseBoot(const QString& value)
{
    if (!m_opened || busy()) {
        return;
    }
    m_boot = value;
    fit();
    emit changed();
}

void MediaView::chooseRestart(bool restart)
{
    if (!m_opened || busy() || !restartEnabled()) {
        return;
    }
    m_restart = restart;
    fit();
    emit changed();
}

void MediaView::mount()
{
    const core::MediaImage* image = picked();
    if (!m_opened || busy() || !canMount() || image == nullptr || !m_shown) {
        return;
    }
    const core::MediaOffer& offers = m_shown->offers;
    const QString drive = m_drive.section(u'_', 0, 0);
    const bool writable = m_drive.endsWith("_rw"_L1);
    const bool again = m_restart;
    const QString into = drive == "cd"_L1 ? words("in its CD-ROM drive, read-only")
                         : drive == "flash"_L1
                             ? (writable ? words("as a USB flash drive, writable: the machine can change and delete "
                                                 "what is on it")
                                         : words("as a USB flash drive, read-only"))
                             : words("as a disk, read-only");
    QString then;
    if (!offers.boot.isEmpty()) {
        then = m_boot == "once"_L1 ? words("and is restarted now to start from it once: stopped at once (unsaved work "
                                           "on it is lost) and started, with its boot order as before after that.")
               : m_boot == "until_unmounted"_L1
                   ? (again ? words("and starts from it until it is unmounted: it is restarted now, stopped at once "
                                    "(unsaved work on it is lost) and started.")
                            : words("and starts from it at its next start, until it is unmounted."))
                   : words("and its boot order is left as it is.");
    } else {
        then = again ? words("and is restarted now with its reset button (its power button where it is off): unsaved "
                             "work on it is lost. Pick the KVM's drive in its boot menu.")
                     : words("and nothing else is done: pick the KVM's drive in its boot menu at its next start.");
    }
    const QString drops = offers.dropsInput ? words(" Its keyboard and mouse drop for a moment if this switches its "
                                                    "drive between CD-ROM and disk.")
                                            : QString();
    // (A KVM's drive is Linux's USB gadget, whose CD-ROM drive reads no further than 256*60*75 sectors.)
    const QString other = offers.drives.contains(u"disk"_s) ? words("a disk") : words("a USB flash drive");
    const QString cut =
        drive == "cd"_L1 && offers.where == "device"_L1 && image->size && *image->size > mostOfACd
            ? words(" Linux's USB gadget, which KVMs such as this one are built on, gives the machine only the first "
                    "2.36 GB of an image in a CD-ROM drive, so one this large may not be read whole. As %1 it is "
                    "(whether the machine starts from it there is the image's to say).")
                  .arg(other)
            : QString();
    Asked asked;
    asked.what = Asked::What::Mount;
    asked.title = words("Mount %1").arg(image->name);
    asked.text = (image->where.isEmpty()
                      ? words("%1 gets %2 %3, %4").arg(m_name, image->name, into, then)
                      : words("%1 gets %2 (on %3) %4, %5").arg(m_name, image->name, image->where, into, then)) +
                 drops + cut;
    asked.yes = again ? words("Mount and restart") : words("Mount");
    asked.danger = again || writable;
    asked.named = m_name;
    asked.mount = core::MediaMount{image->id, m_name, drive, writable, m_boot, again};
    m_file.reset();
    m_asked = asked;
    emit changed();
}

void MediaView::unmount()
{
    if (!m_opened || busy() || !m_shown || !m_shown->mounted) {
        return;
    }
    const core::MountedMedia& mounted = *m_shown->mounted;
    Asked asked;
    asked.what = Asked::What::Unmount;
    asked.title = words("Unmount %1").arg(mounted.name);
    asked.text = (mounted.first == std::optional<bool>(true)
                      ? words("%1 is taken out of the drive of %2, and its boot order put back where it was set "
                              "from here. A system running from it loses it.")
                      : words("%1 is taken out of the drive of %2. A system running from it loses it."))
                     .arg(mounted.name, m_name) +
                 (m_shown->offers.dropsInput ? words(" Its keyboard and mouse may drop for a moment.") : QString());
    asked.yes = words("Unmount");
    asked.named = m_name;
    asked.image = mounted.image;
    m_file.reset();
    m_asked = asked;
    emit changed();
}

void MediaView::remove()
{
    const core::MediaImage* image = picked();
    if (!m_opened || busy() || !canDelete() || image == nullptr) {
        return;
    }
    Asked asked;
    asked.what = Asked::What::Delete;
    asked.title = words("Delete %1").arg(image->name);
    asked.text = words("%1 is deleted from the KVM's own storage of %2. It cannot be brought back from here.")
                     .arg(image->name, m_name);
    asked.yes = words("Delete");
    asked.named = m_name;
    asked.image = image->id;
    m_file.reset();
    m_asked = asked;
    emit changed();
}

QString MediaView::refusedAtOnce(const QString& name, qint64 size) const
{
    if (!m_shown) {
        return {};
    }
    if (size <= 0) {
        return words("An empty file is no image.");
    }
    if (!core::isImageName(name)) {
        return words("An image's name is of letters, digits, spaces and . _ + - ( ) [ ] ~ , (200 at the most), and "
                     "neither begins nor ends with a space or a dot. Rename the file and upload it again.");
    }
    const auto there =
        std::ranges::find_if(m_shown->images, [&](const core::MediaImage& one) { return one.name == name; });
    if (there != m_shown->images.end() && there->partial) {
        return words("A part of an earlier upload of %1 is on the KVM: delete it in the list first, or upload it "
                     "under another name.")
            .arg(name);
    }
    if (there != m_shown->images.end()) {
        return words("%1 is on the KVM's own storage already: delete it first, or upload it under another name.")
            .arg(name);
    }
    if (m_shown->free && size > *m_shown->free) {
        return words("The KVM's own storage has %1 free, and %2 is %3: delete an image there first.")
            .arg(bytesText(*m_shown->free), name, bytesText(size));
    }
    if (m_shown->uploading) {
        return words("%1 is being uploaded to %2 at this moment, by %3. Wait for it to finish.")
            .arg(m_shown->uploading->name, m_name, m_shown->uploading->by);
    }
    return {};
}

void MediaView::upload()
{
    if (!m_opened || busy() || !canUpload()) {
        return;
    }
    const base::Result<QString> chosen = m_whichImage ? m_whichImage() : base::Result<QString>(QString());
    if (!chosen) {
        tell(QString::fromStdString(chosen.error().message), true);
        emit changed();
        return;
    }
    // (The box may have been closed, or another thing begun, while Windows' box was open.)
    if (chosen.value().isEmpty() || !m_opened || busy()) {
        return;
    }
    // The file is opened, and its length found, on a thread of its own (core::ReadAhead), as it is read later: a share
    // that answers slowly, or not at all, holds nothing here still. Asked once it is open; another choice meanwhile, or
    // the box closed, leaves it nobody's, and it is let go of. (Its name is of the path alone: nothing of the disk.)
    const QString& path = chosen.value();
    const QString name = QFileInfo(path).fileName();
    const quint64 opening = ++m_openings;
    m_openingName = name;
    m_opening.start();
    core::ReadAhead::open(
        m_source ? m_source(path) : core::ReadAhead::fileAt(path), this,
        [this, opening, turn = m_turn, name](base::Result<std::unique_ptr<core::ReadAhead>> file) {
            if (opening != m_openings || !isCurrent(turn) || busy()) {
                return;
            }
            m_opening.stop();
            m_openingName.clear();
            if (!file) {
                tell(words("%1 could not be read: %2").arg(name, QString::fromStdString(file.error().message)), true);
                emit changed();
                return;
            }
            askUpload(name, std::move(file).value());
        });
}

void MediaView::askUpload(const QString& name, std::unique_ptr<core::ReadAhead> file)
{
    // (Said while it was being opened: over.)
    if (m_said == words("Opening %1...").arg(name)) {
        tell({});
    }
    const qint64 size = file->size();
    if (size > core::mostUploadBytes) {
        tell(words("%1 is %2: an image carried through qKVM may be %3 at the most.")
                 .arg(name, bytesText(size), bytesText(core::mostUploadBytes)),
             true);
        emit changed();
        return;
    }
    if (const QString refused = refusedAtOnce(name, size); !refused.isEmpty()) {
        tell(refused, true);
        emit changed();
        return;
    }
    Asked asked;
    asked.what = Asked::What::Upload;
    asked.title = words("Upload %1").arg(name);
    asked.text = words("%1 (%2) is carried through qKVM to the KVM's own storage of %3 as this program reads it, "
                       "and kept nowhere on the way. Keep this box open until it is done: closing it stops the "
                       "upload, and what came of it is taken off the KVM.")
                     .arg(name, bytesText(size), m_name);
    asked.yes = words("Upload");
    asked.danger = false;
    asked.named = m_name;
    asked.image = name;
    m_asked = asked;
    m_file = std::move(file);
    emit changed();
}

void MediaView::answer(bool yes)
{
    if (!m_asked) {
        return;
    }
    const Asked asked = *std::exchange(m_asked, std::nullopt);
    // (The file of an upload goes with its answer: sent with a yes, let go of otherwise.)
    auto file = std::make_shared<std::unique_ptr<core::ReadAhead>>(std::move(m_file));
    emit changed();
    if (!yes || !m_opened || busy() || m_client == nullptr) {
        return;
    }
    const QString machine = m_machine;
    const QPointer<core::ServerClient> client = m_client;
    // Each says what came of it: what the server said with its yes, or why not.
    const auto said = [](const base::Result<core::MediaDone>& done) -> base::Result<QString> {
        return done ? base::Result<QString>(done.value().note) : base::Result<QString>(done.error());
    };
    switch (asked.what) {
    case Asked::What::Mount:
        carry(asked.title,
              asked.mount.restart ? words("Mounting and restarting: as long as it takes to stop and to start...")
                                  : words("Mounting..."),
              [client, machine, mount = asked.mount, said](const Answer& answered) {
                  client->mountMedia(machine, mount, [answered, said](const auto& done) { answered(said(done)); });
              });
        break;
    case Asked::What::Unmount:
        carry(asked.title, words("Unmounting..."), [client, machine, asked, said](const Answer& answered) {
            client->unmountMedia(machine, asked.image, asked.named,
                                 [answered, said](const auto& done) { answered(said(done)); });
        });
        break;
    case Asked::What::Delete:
        carry(asked.title, words("Deleting..."), [client, machine, asked, said](const Answer& answered) {
            client->deleteMedia(machine, asked.image, asked.named,
                                [answered, said](const auto& done) { answered(said(done)); });
        });
        break;
    case Asked::What::Upload:
        carry(
            asked.title, words("Uploading %1...").arg(asked.image),
            [this, asked, file](const Answer& answered) { sendUpload(asked, std::move(*file), answered); }, false);
        break;
    }
}

void MediaView::carry(const QString& what, const QString& note, const std::function<void(Answer)>& send,
                      bool inTheCorner)
{
    if (m_carrying != 0) {
        tell(words("\"%1\" is still under way. Wait for its answer.").arg(m_carryWhat), true);
        emit changed();
        return;
    }
    // One thing at a time for each machine, from whichever window.
    if (m_underWay != nullptr && m_underWay->contains(m_underWayKey)) {
        tell(words("\"%1\" is still under way on %2. Wait for its answer.")
                 .arg(m_underWay->value(m_underWayKey), m_name),
             true);
        emit changed();
        return;
    }
    const quint64 carrying = ++m_carries;
    m_carrying = carrying;
    m_carryTurn = m_turn;
    m_carryWhat = what;
    m_carryUnderWay = m_underWay;
    m_carryKey = m_underWayKey;
    if (m_carryUnderWay != nullptr) {
        m_carryUnderWay->insert(m_carryKey, what);
    }
    tell(note);
    // While it is out, its line counts the seconds (a server's mount can take minutes): it is under way.
    m_since.start();
    m_waited.clear();
    m_counting.start();
    emit changed();
    send([guard = QPointer<MediaView>(this), carrying, turn = m_turn, what, name = m_name, key = m_key,
          inTheCorner](const base::Result<QString>& answer) {
        if (guard == nullptr || guard->m_carrying != carrying) {
            return; // (let go of meanwhile: nobody's)
        }
        MediaView& self = *guard;
        self.m_carrying = 0;
        if (self.m_carryUnderWay != nullptr) {
            self.m_carryUnderWay->remove(self.m_carryKey);
        }
        self.m_carryUnderWay = nullptr;
        self.m_counting.stop();
        self.m_progress.stop();
        self.m_waited.clear();
        QString message;
        bool bad = false;
        if (answer) {
            emit self.told(answer.value().isEmpty() || !inTheCorner
                               ? words("%1: %2 done").arg(name, what)
                               : words("%1: %2 done. %3").arg(name, what, answer.value()),
                           true);
            message = answer.value().isEmpty() ? words("%1: done.").arg(what) : answer.value();
        } else if (answer.error().code != core::ServerClient::uploadStopped) {
            // The server's own words where it answered; where nothing came back, that it may have been done.
            message = unanswered(answer.error())
                          ? words("No answer to \"%1\": it may have been done. Look at %2 before trying again.")
                                .arg(what, name)
                          : QString::fromStdString(answer.error().message);
            bad = true;
        }
        emit self.done(key);
        if (!self.isCurrent(turn)) {
            if (bad && !self.m_opened) {
                // (The box was closed meanwhile: told in the corner, where somebody sees it.)
                emit self.told(words("%1: %2").arg(name, message), false);
            }
            emit self.changed();
            return;
        }
        self.tell(message, bad);
        self.m_keep = std::make_pair(message, bad);
        emit self.changed();
        self.load();
    });
}

void MediaView::countUp()
{
    const qint64 seconds = m_since.elapsed() / 1000;
    m_waited = seconds >= 1 ? words("%1 s").arg(seconds) : QString();
    emit changed();
}

void MediaView::sendUpload(const Asked& asked, std::unique_ptr<core::ReadAhead> file, const Answer& answered)
{
    if (file == nullptr) {
        answered(base::Error{"unreadable", words("%1 could not be read.").arg(asked.image).toStdString()});
        return;
    }
    const QString name = asked.image;
    m_stopUpload = m_client->uploadMedia(
        m_machine, name, asked.named, std::move(file),
        [guard = QPointer<MediaView>(this), answered, name](const base::Result<core::MediaUploaded>& done) {
            if (guard != nullptr) {
                guard->m_stopUpload = {};
            }
            answered(
                done ? base::Result<QString>(done.value().sha256.isEmpty()
                                                 ? QString()
                                                 : words("%1 is on the KVM, SHA-256 %2").arg(name, done.value().sha256))
                     : base::Result<QString>(done.error()));
        });
    // How far it has come, as the server counts it, every second.
    disconnect(&m_progress, nullptr, this, nullptr);
    connect(&m_progress, &QTimer::timeout, this, [this, name, carrying = m_carrying, turn = m_turn] {
        if (m_client == nullptr || m_carrying != carrying) {
            m_progress.stop();
            return;
        }
        m_client->askUpload(m_machine, [guard = QPointer<MediaView>(this), name, carrying,
                                        turn](const base::Result<std::optional<core::MediaUploading>>& now) {
            if (guard == nullptr || guard->m_carrying != carrying || !guard->isCurrent(turn) || !now || !now.value()) {
                return; // (asked again in a second)
            }
            const core::MediaUploading& up = *now.value();
            if (up.name != name || up.size <= 0) {
                return;
            }
            guard->tell(words("Uploading %1: %2 of %3 (%4 %)")
                            .arg(name, bytesText(up.sent), bytesText(up.size))
                            .arg(up.sent * 100 / up.size));
            emit guard->changed();
        });
    });
    m_progress.start();
}

} // namespace qkvm::ui
