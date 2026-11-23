#pragma once

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/core/server_types.h>

// What a KVM that runs kvmd says of its virtual media, its mass storage drive (kvmd's MSD), read into the program's own
// types, and what it is asked for it: the web's 0.3.0 (kvm.py, "virtual media (0.3.0, A2)"), whose words these are.
// Images in the KVM's own storage, one of them in its drive at a time, as a CD-ROM drive or a USB flash drive
// (read-only unless whoever mounts it chooses it writable). GET /api/msd says it all (`enabled`, `online`, `busy`,
// `drive`, `storage`); an image is chosen with set_params while the drive is disconnected and connected with
// set_connected, written with POST /api/msd/write, its length said first and `remove_incomplete=1`, and removed with
// remove. Never write_remote, nor an image by address (kvmd would fetch it: a way into the network it stands in), nor
// read, nor set_enabled (a machine that cannot have a drive and a keyboard on one port is the owner's to switch), nor
// GL.iNet's partition routes (some of them change things on a GET). Nothing here touches the network.
namespace qkvm::core::kvmd {

// kvmd's `msd` event on its line for events (the state of its drive and of its own storage, every image named in it:
// kilobytes) is read up to this length (the web's MOST_MSD_WORD); and the longest message that line takes at all, the
// rest of it thrown away unread (MOST_EVENT).
inline constexpr qsizetype mostMsdWord = qsizetype{1} << 18;
inline constexpr qsizetype mostEvent = qsizetype{1} << 20;
// Where no line is held, a look asks /api/msd this often at the most (MSD_EVERY): a GL.iNet KVM looks at its storage's
// partitions for every such answer.
inline constexpr int msdEveryMs = 30'000;
// A CD-ROM drive of Linux's USB gadget (a KVM's drive) takes an image of 300 sectors of 2048 bytes at least, and
// refuses a smaller one (LEAST_CD).
inline constexpr qint64 leastOfACd = qint64{300} * 2048;

// Of kvmd's state of its drive (or a part of one, as an event says what changed): what the program reads, and nothing
// else (MSD_KEPT, MSD_STORAGE_KEPT): `enabled`, `online`, `busy`, `drive`, `drive_partition`, and of `storage` its
// `images` and `parts`.
QJsonObject msdKept(const QJsonObject& state);
// kvmd's `msd` event as it is merged into what is known (`before`): a whole state (it has `online`) takes the place of
// what was known; a part of one is merged in key by key, and of `storage` each of its two by itself. Nothing where it
// is a part of a state nothing is known of yet (the next whole one, or a question, says it). (The web's `_msd_said`.)
std::optional<QJsonObject> msdMerged(const std::optional<QJsonObject>& before, const QJsonObject& event);
// What an `msd` event on the line says (its `event`); nothing where the message is no such event, or is longer than
// mostMsdWord. (Only a message that can be one is read as JSON.)
std::optional<QJsonObject> msdHeard(const QByteArray& message);

// The name of an image on a KVM's own storage, as the program lists it and hands it back (the web's DEVICE_IMAGE):
// an image's name (isImageName, without its rule against ".incomplete") or up to four such joined by slashes (kvmd
// lists images in folders so).
bool isDeviceImage(const QString& name);

// The SHA-256 of each image this program carried to a KVM, with the size and the time the KVM gave it: said of an image
// only while both are still those (another image of that name, put there on the KVM itself, is not said to be it). Kept
// while the program runs, beside the drivers, which are made anew at every change of a KVM; by the KVM's address, which
// no two KVMs of the wall share. The newest MOST of them. (The web's MEDIA_NOTES, but for who mounted an image: there
// are no accounts on this PC's own wall.)
class MediaNotes {
public:
    static constexpr int most = 2048;
    // The program's own.
    static MediaNotes& program();

    void carried(const QString& kvm, const QString& image, const QString& sha256, qint64 size,
                 std::optional<double> stamp);
    QString hashOf(const QString& kvm, const QString& image, std::optional<qint64> size,
                   std::optional<double> stamp) const;
    void forget(const QString& kvm, const QString& image);

private:
    struct Kept {
        QString sha256;
        qint64 size = 0;
        std::optional<double> stamp;
        quint64 order = 0;
    };
    QHash<QString, Kept> m_kept;
    quint64 m_order = 0;
};

// The images of the KVM's own storage, as kvmd lists them in its state: each by its kind, by name without regard to
// case, MOST of them (mostMediaImages); and how many entries were left out (no name the program hands back, or no
// image's shape, or beyond the most). Each with its size (bytes, where it is a number of 0 to 2^50), whether it is
// what a write cut off left (`complete` plainly false), and its SHA-256 where this program carried it there (`notes`
// for `kvm`) and it is still that image. (The web's `_msd_listed`.) And whether it was read whole: not where it lists
// no images at all, or one of a name the program hands back in no image's shape, or more than the most. What is not
// found in a list that was not read whole is not known not to be there, and is never said or taken to be gone (outside
// review 4.6): see notReadWhole.
struct Listed {
    QList<MediaImage> images;
    int leftOut = 0;
    bool whole = true;
};
Listed imagesOf(const QJsonObject& state, const MediaNotes& notes, const QString& kvm);
// What is said where something is not found in a list of its storage that was not read whole: that its list could not
// be read whole, so `notKnown` (as "whether x.iso is there is not known"), and to look at its own page.
base::Error notReadWhole(const QString& notKnown);
// Such words (a JetKVM's too) as a note of a list (MediaList::notes, which the box joins with ". "): without their last
// full stop.
QString notReadWholeNote(QString said);
// Whether an image of its storage is whole, as kvmd lists it (`complete`): nothing where it does not plainly say yes or
// no, or does not list it. (An image that does not plainly say it is whole is no image to mount and has no SHA-256, but
// is removed by a discard only where it plainly says it is not: review cx4-1 L1.)
std::optional<bool> completeOf(const QJsonObject& state, const QString& image);
// What stands against a change of the KVM's drive or storage, as kvmd says it now, in the web's words: its own drive
// switched off in its settings ("no_virtual_media"), offline, busy, or GL.iNet's File Sharing (its whole storage open
// to the machine, read-write); nothing where nothing does. (The web's `_msd_stands_against`.)
std::optional<base::Error> standsAgainst(const QJsonObject& state);
// The KVM's own drive is switched off in its own settings ("no_virtual_media").
base::Error switchedOff();
// What is in its drive: nothing where its drive is off, is none (kvmd's word for an offline drive) or plainly not
// connected. Its name as kvmd says it where that is one the program hands back, its id the same; else said as a line
// ("an image" where it says none), and its id namelessId's; a CD-ROM drive where kvmd plainly says so, else a USB flash
// drive; read-only where it is a CD-ROM drive or is plainly not writable (one that does not say is taken to be
// writable). A drive in no shape it reads, or one that does not plainly say whether it is connected, is no empty drive:
// unreadOf (outside review 4.7). (The web's `media_now`, which takes such a drive for an empty one, one that does not
// say whether it is writable for a read-only one, and a name it does not hand back for the name it reads as.)
std::optional<MountedMedia> mountedOf(const QJsonObject& state, const MediaNotes& notes, const QString& kvm);
// The id of what is in a drive that a KVM (kvmd's, or a JetKVM) says in no shape the program reads: no image of its
// storage is called so (a name of either never ends in a slash), so nothing is taken for it, and every change of its
// drive is refused while it is so (driveNotRead; a JetKVM's words are its own).
inline constexpr QLatin1StringView unreadDrive{"(not read)/"};
// What is in a drive that cannot be read: unreadDrive, called `said`, of no kind of drive that is known, not read-only.
MountedMedia unreadOf(const QString& said);
// The id of an image in a drive (kvmd's, or a JetKVM's of its own storage) whose name the KVM does not say as one the
// program hands back: none at all, or one of no image's shape, which as a line may read as the name of another image
// ("an image", "7", "a b.iso"). "(name not read: " and `digest` (made of what it says of that image, so that another
// such image has another id) and ")/". No image of either storage is called so (a name of either never ends in a
// slash): it is never "mounted so already" of one, has no SHA-256 of one, and while it is in the drive no image of the
// storage is deleted, as it may be that one (review cx4-1 M2). An unmount asked of it by this id takes out only it.
QString namelessId(const QString& digest);
bool isNameless(const QString& id);
// The id mountedOf gives the image chosen for its drive, connected or not; nothing where none is (review cx4-2 L6: a
// nameless image's id was compared with its raw name).
std::optional<QString> chosenIdOf(const QJsonObject& state);
// What is in a drive as a sentence that begins with it says it (a JetKVM's too): an image's name as the KVM gives it,
// as it is; the program's own words for one it goes by an id of its own for ("an image", "an image from an address,
// ...", "what is in its drive, ..."), with a capital (review cx4-2 L6).
QString opensASentence(const MountedMedia& now);
// Whether it is an image of the storage whose name the KVM does not say at all ("an image", by namelessId).
bool saysNoName(const MountedMedia& now);
// A change of its drive refused while what is in it cannot be read.
base::Error driveNotRead();
// The image chosen for its drive (set_params), whether it is in its drive yet or not, as kvmd says it now: its name
// exactly as kvmd gives it, whether it is connected, and its kind of drive and whether it is read-only as mountedOf
// reads them. Nothing where its drive is off, or none is chosen, or kvmd says it in no shape the program reads. (kvmd
// connects whatever is chosen when its connect comes: what a mount chose is read back before it connects, and after.)
struct Chosen {
    QString image;
    QString drive;
    bool readOnly = true;
    bool connected = false;
};
std::optional<Chosen> chosenOf(const QJsonObject& state);
// What its drive can do: nothing where its own drive is switched off (or nothing is known of it). Images as a CD-ROM
// drive or a USB flash drive (that one writable where chosen), never started from (a KVM is no firmware), restarted
// from here where it has a power board (`board`), uploaded through this program, kept on the KVM, deleted.
std::optional<MediaOffer> offerOf(const std::optional<QJsonObject>& state, bool board);
// What its own storage has free, of how much, in bytes, where it says (`storage.parts[""]`).
std::optional<qint64> freeOf(const QJsonObject& state);
std::optional<qint64> sizeOf(const QJsonObject& state);
// The code of an upload stopped by whoever began it, where nothing of it is left on the KVM: said by nobody, as whoever
// stopped it knows (the same as a server's client's, MediaPlace::uploadStopped).
inline constexpr const char* uploadStopped = "desktop:stopped";
// Of kvmd's answer to a write (POST /api/msd/write): how much of the image it says it wrote (`result.image.written`);
// nothing where it says no count of bytes.
std::optional<qint64> writtenOf(const QByteArray& answer);
// Of any of its answers: its own words for a refusal (`result.error_msg`), as a line of 200 at the most; empty: none.
QString errorOf(const QByteArray& answer);
// The time kvmd gives an image of its storage (`mod_ts`, in its state), where it says one.
std::optional<double> stampOf(const QJsonObject& state, const QString& image);
// A count of bytes with a comma between each three digits, as the web writes one in a sentence ("1,234,567").
QString withCommas(qint64 bytes);
// The words of a refusal of one of kvmd's changes (set_params, set_connected, remove), by its HTTP status and its
// `error_msg`: busy (409), or that it did not do `what`.
base::Error msdRefusal(int httpStatus, const QByteArray& answer, const QString& what);

} // namespace qkvm::core::kvmd
