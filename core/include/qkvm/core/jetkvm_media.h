#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/core/kvmd_media.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/strict_json.h>

// What a JetKVM says of its virtual media, read into the program's own types: the web's 0.3.0 (jetkvm.py, "virtual
// media (0.3.0, A3)", whose words these are), which read it from the JetKVM's firmware (jsonrpc.go and
// usb_mass_storage.go at dev d646be8, the same in release 0.5.9). Its own image storage, one image of it in its drive
// at a time, as a CD-ROM drive or a disk, read-only either way (its gadget has `ro` set for good). It is asked over the
// `rpc` channel of the connection its card holds (JetKvmDevice, jetkvm_media.cpp): getVirtualMediaState,
// listStorageFiles, getStorageSpace, mountWithStorage, unmountImage, deleteStorageFile and startStorageFileUpload; the
// bytes of an upload go by POST /storage/upload?uploadId= on a connection of their own. Never mountWithHTTP nor
// checkMountUrl (the JetKVM would fetch an address: a way into the network it stands in), nor its built-in images, nor
// setMassStorageMode. Nothing here touches the network.
namespace qkvm::core::jetkvm {

// What a JetKVM adds to the name of an image while it is being uploaded, and leaves after an upload cut off (the web's
// PARTIAL).
inline constexpr QLatin1StringView partialEnd{".incomplete"};

// One image's name as the web takes one (IMAGE_NAME): letters, digits, blanks and . _ + - ( ) [ ] ~ , (200 at the
// most), neither beginning nor ending with a blank or a dot. An image of a JetKVM's storage is one name, never a path.
// (Unlike isImageName, a name that ends in ".incomplete" is one here: what the web's own checks make of that is said
// where it is used.)
bool isOneName(const QString& name);
// The id of an upload, as a JetKVM hands one out: "upload_" and a UUID in small letters (the web's UPLOAD_ID). It is
// the JetKVM's, and is never shown.
bool isUploadId(const QString& id);

// When an image of its storage was made (`createdAt`), as the stamp the notes of carried images hold beside its size
// (kvmd::MediaNotes, whose stamp is a number): the web's plain(createdAt, 60), the text as one line of 60 at the most,
// made into a number that is the same for the same text and another for another (but by a chance of one in 2^48: the
// first six bytes of its SHA-256). Nothing where it says no text or number.
std::optional<double> stampOf(const json::Value* createdAt);

// The images of its own storage, as listStorageFiles answers (`files`: `filename`, `size`, `createdAt`): each by its
// kind, a partial one (an upload under way, or what one cut off left: `<name>.incomplete`) as such, named without that
// end; whole ones with their SHA-256 where this program carried them there and they are still that image (`notes` for
// `key`, by size and stamp); by name without regard to case, the whole one first; mostMediaImages of them. And how many
// entries were left out (no name the web hands back, or beyond the most), and of each listed its size and stamp by its
// id, for what is in its drive. A size is a whole number of 0 to 2^50 bytes, or none. (The web's `_files`.) And
// whether it was read whole: not where it is no list, or has an entry whose name cannot be read, or one beyond the
// most. What is not found in a list that was not read whole is not known not to be there, and is never said or taken
// to be gone (outside review 4.6): see notReadWhole.
struct Files {
    struct Listed {
        std::optional<qint64> size;
        std::optional<double> stamp;
    };
    QList<MediaImage> images;
    int leftOut = 0;
    QHash<QString, Listed> listed;
    bool whole = true;
};
Files filesOf(const json::Value& said, const kvmd::MediaNotes& notes, const QString& key);
// What is said where something is not found in a list of its storage that was not read whole: that its list could not
// be read whole, so `notKnown` (as "whether x.iso is there is not known"), and to look at its own page.
base::Error notReadWhole(const QString& notKnown);

// What is in its drive, as getVirtualMediaState answers (null, or `source` "Storage", "HTTP" or "WebRTC", `mode`
// "CDROM" or "Disk", `filename`, `size`): nothing where it says nothing is (null); something that cannot be read where
// it says what is no object, or does not say a `source` of those three (kvmd::unreadOf: never an empty drive, outside
// review 4.7 and review cx4-1 M2; the web takes the first for one, and the second for an image from an address). A
// CD-ROM drive where it says "CDROM", a disk where it says "Disk", else neither; read-only always. (A call answered
// with no `result` at all is read as null, an empty drive: the connection hands both on alike, and an empty drive may
// come so, its firmware leaving out a `result` that is nothing.) Of its own storage: the name as it says it where it is
// one the web hands back, with its SHA-256 as listFiles listed it last; else said as a line ("an image" where it says
// none), by kvmd::namelessId of digestOf, and with no SHA-256. Mounted at its own page from an address: by
// fromAnAddressId, and never the address itself, which may hold a password; sent from a browser at its own page:
// "(a browser)/" and digestOf. (The web's `media_now`, whose id of one from an address is "(an address)" for every
// one, and of an image of its storage the name it reads as.)
std::optional<MountedMedia> mountedOf(const json::Value& state, const QHash<QString, Files::Listed>& listed,
                                      const kvmd::MediaNotes& notes, const QString& key);
// The id of an image mounted at its own page from an address, as getVirtualMediaState says it: "(an address)/" and
// digestOf. Another address's mount has another id, so that one is never taken out for the other (outside review
// 4.4); no image of its storage is called so (a name has no slash); and the id says nothing of the address.
QString fromAnAddressId(const json::Value& state);
// 16 hex digits of an HMAC-SHA256 of what getVirtualMediaState says of what is in its drive (`source`, `url`, `mode`,
// `filename`, `size`, each as it came) under a key made at random once in this program's run and kept nowhere else:
// the same for the same, another for another, and nothing of what it was made of can be learned from it.
QString digestOf(const json::Value& state);
// A change of its drive refused while what is in it cannot be read (kvmd::unreadDrive).
base::Error driveNotRead();
// Whether what getVirtualMediaState answered is an image of its own storage in its drive (`source` "Storage"). One
// mounted from an address is not, though an image of its storage may be named "(an address)" too: it is never taken
// for that one (no SHA-256 of it, never "mounted so already", no refusal to delete it as mounted).
bool fromItsStorage(const json::Value& state);

// What its storage has, as getStorageSpace answers (`bytesUsed`, `bytesFree`, each a whole number of 0 to 2^60 bytes).
struct Space {
    std::optional<qint64> used;
    std::optional<qint64> free;
};
Space spaceOf(const json::Value& said);

// What its drive can do (the web's `_media_offer`): an image as a CD-ROM drive or a disk, read-only either way, never
// started from (a KVM is no firmware), restarted from here where its ATX extension is on (`atx`), uploaded through this
// program, kept on the JetKVM, deleted; and a change between CD-ROM and disk drops its keyboard and mouse for a moment
// (`dropsInput`).
MediaOffer offerOf(bool atx);

// What startStorageFileUpload answered (`alreadyUploadedBytes`, `dataChannel`): the upload's id where it opened one
// anew, of an id's shape; nothing otherwise. And whether it says it opened it anew (`alreadyUploadedBytes` 0), whatever
// else it says: what it opened is then this program's to take off again.
struct Started {
    std::optional<QString> id;
    bool anew = false;
};
Started startedOf(const json::Value& said);

// What a change of its drive (a mount, an unmount) that ended so is said as: one whose call never went out ("dropped"
// is said of a call that was never sent, and of nothing else) was not done, and is said so in words of its own (the
// web's _not_sent: "It was not done: ..."); any other end as it is.
base::Error notSent(const base::Error& why);

// Whether an image of `size` bytes that a JetKVM answered "Upload completed" to went from this PC whole: every byte of
// it written to the line (`sent`, as Qt said it) and read for that, its hash (`sha256`, empty until every byte was
// read) of all of them. One the JetKVM answered before did not (a real JetKVM reads to the end first; Qt reads no
// answer before it has written all, so only a JetKVM that is no JetKVM, or a Qt that does otherwise, comes to this).
bool wentWhole(qint64 sent, qint64 size, const QString& sha256);

} // namespace qkvm::core::jetkvm
