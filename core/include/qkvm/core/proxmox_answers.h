#pragma once

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/server_types.h>

// What a Proxmox VE says through its API, and what is typed for one, read into the program's own types (wave 2, c9b).
// Nothing here touches the network: these are the readers, kept apart from the lines they travel on so that each can
// be tried with whatever a Proxmox might send, as redfish_answers.h is for a server's controller.
//
// It does what the web's driver does (../app/qkvm/proxmox.py, the frozen copy of web 0.3.1), which is where a real
// Proxmox VE 9.2's ways were learned; where it does otherwise, the reason stands beside it. A Proxmox is somebody
// else's software on the network: every text of its is cut to a line (kvmd::plain), every number is held to a range,
// and what is not of the shape it should be is not taken for one.
namespace qkvm::core::proxmox {

// Where its API and its own page answer, unless the address says otherwise.
inline constexpr int usualPort = 8006;
// The numbers Proxmox gives its guests.
inline constexpr qint64 lowestGuest = 100;
inline constexpr qint64 highestGuest = 999'999'999;
// Guests of one Proxmox listed to pick from, and the bytes of that list (a guest is some 350: ten thousand fit).
inline constexpr qsizetype mostGuests = 2000;
inline constexpr qsizetype largestListBytes = qsizetype{4} * 1024 * 1024;
// What a tile's card or a sentence says where Proxmox said the token back in a spelling not cut out.
inline constexpr QLatin1StringView saidTheToken{"its words held the API token, and are not shown"};

// A Proxmox, and a guest on it where one is named: `origin` is https, its host, and its port unless that is 443 (as
// kvm::addressFrom writes an address).
struct Address {
    QUrl origin;
    QString node;               // empty: none named
    std::optional<qint64> vmid; // none: none named
};

// What somebody typed or pasted, as a Proxmox and the guest it names; "invalid" in a sentence where it is none.
// 'pve.lan', 'pve.lan:8006', 'https://pve.lan:8006/' (no guest: the guests are listed to pick from); 'pve.lan/100' (a
// guest by its number), 'pve.lan/pve1/100' (and its node); the address as the wall keeps it,
// 'https://pve.lan:8006/nodes/pve1/qemu/100'; and the addresses Proxmox's own page shows: its console's
// ('...?console=kvm&vmid=100&node=pve1') and a guest's ('...#v1:0:=qemu%2F100:...'). Without a port: 8006. Only HTTPS:
// its API is spoken over nothing else. A container (lxc) is refused: only virtual machines are shown.
base::Result<Address> addressOf(const QString& typed);
// ...and a guest's address as a wall keeps one (a wall's file, the web's and this program's): read by the same rules,
// but without a port it is at HTTPS's own, 443, as both writers mean it (each keeps 443 unwritten: kvm::addressFrom,
// the web's device_url), and not at 8006 as what is typed is (review cx3-3, M1). The web's own reader takes such a
// guest at 8006: a guest of a Proxmox behind a proxy at 443 comes back from the web's file at another port.
base::Result<Address> keptAddressOf(const QString& kept);
// A guest's address as the web keeps it (its settings, a wall's file): "https://pve.lan:8006/nodes/pve1/qemu/100", and
// "https://pve.lan/nodes/pve1/qemu/100" for one at 443.
QString guestUrl(const QUrl& origin, const QString& node, qint64 vmid);
// Whether this names a node as a host is named, and whether this is a guest's number.
bool isNode(QStringView text);
bool isGuestNumber(qint64 vmid);
// Where a browser opens the guest's own console on Proxmox's page (after signing in there).
QUrl consolePage(const QUrl& origin, const QString& node, qint64 vmid);

// What a token is: user@realm!name, and its secret, a UUID. Both go into a header and nowhere else, so only these
// shapes are sent at all: what is not a token goes nowhere. Its ID is 64 characters at the most, as every user name of
// the wall is (the web's _a_line(64) for it; kvm::longestUser, which the settings, the edit box and a wall's file
// hold to): one longer is refused as it is typed, and is no token's (review c9b-1, M5).
inline constexpr qsizetype longestTokenId = 64;
bool isTokenId(QStringView text);
bool isTokenSecret(QStringView text);
// The words for what is no token, before anything is sent.
base::Error notAToken();

// Where Proxmox hands a request on to the node a guest is on, and that came to nothing: 595 where the node could not
// be connected to (nothing reached it); 596 and 597, and a proxy's 502, 503 and 504 in front of Proxmox, where the
// answer did not come back, after the node may have carried it out (the web's ANSWER_LOST).
bool answerLost(int status);
// An answer of Proxmox's as it came: its status, its body, and the words of its status line (DeviceAsker::Reply::
// reason). Proxmox's API server says why it refused in its status line, with a body of {"data":null} (and `errors` for
// a parameter it did not take); only its other format (extjs) puts a `message` in the body (review c9b-2, H1).
struct Answer {
    int status = 0;
    QByteArray body;
    QByteArray reason;
};

// How an answer that is no yes stands, beside its words (dataOf): the guest is not on the node asked (it was moved,
// or the node is no more: the web's WrongNode, by its words, in its body or its status line); an error of Proxmox's
// own, one of its moments and no refusal (Unwell); one whose answer was lost on its way back, after it may have been
// carried out (AnswerLost).
enum class Fault { None, WrongNode, Unwell, AnswerLost };
Fault faultOf(const Answer& answer);

// What Proxmox said with an answer that is no yes, as one line (the web's `_said`): its message, else the words of its
// status line (of ASCII alone, as the web's reader decodes them), else "HTTP <status>"; never the token the request
// carried. Where Proxmox (or a proxy before it) said the token back, it is cut out, and where it said it in a spelling
// that is not cut out (split by a line break, without its dashes, %-encoded), none of its words are said
// (saidTheToken): what Proxmox says goes onto the card and into the log. A line of 1000 before the token is cut out,
// and of 160 after.
QString saidBy(const Answer& answer, QStringView tokenId, QStringView secret);
// The `data` of a yes (200 with {"data": ...}), or why not, in the web's words (`what`: what was asked, "look at this
// guest"): "bad_response" for a 403 ("Proxmox does not let the token ..."), for what is not its API's answer, and for
// any other no; "unreachable" for a node Proxmox cannot reach (595, 596).
base::Result<QJsonValue> dataOf(const Answer& answer, QStringView what, QStringView tokenId, QStringView secret);

// The unsigned question of a Proxmox's API, asked before its token goes anywhere (outside review of 0.2.0, 3.2): GET
// /api2/json/version with no token, on the very line the token is to go on, right before every sign-in with it
// (ProxmoxDevice). What answers it is taken for a Proxmox VE where it refuses it 401, as a real one refuses every
// question that carries no token, or says its version as Proxmox's API says it ({"data":{"version":"9.2.21"}}). A 404,
// a login page, a redirect, an error or an answer of another shape is somebody else at that address (another HTTPS
// service, a printer's, a KVM's), and the token does not go to it: "bad_response", in these words (notAProxmoxThere).
// So is a 401 that asks for a sign-in of its own (`challenged`: it carries WWW-Authenticate, Basic, Digest, Negotiate
// or any other): Proxmox's API server asks for none (its 401 is its status line's words, "No ticket", and nothing
// more), and a token in the Authorization header answers no such challenge, so no Proxmox the token could work with is
// refused by it (review cx3-1, L2: a camera's, a switch's, a Windows sign-in's).
bool saysItIsAProxmox(const Answer& answer, bool challenged);
base::Error notAProxmoxThere(int status, bool challenged);

// A virtual machine of a cluster's list (/cluster/resources?type=vm), for somebody adding guests.
struct Listed {
    qint64 vmid = 0;
    QString node;
    QString name;   // never empty: "VM <number>" where it has none
    QString status; // "running", "stopped", "paused"; empty: neither
    bool operator==(const Listed&) const = default;
};
struct Listing {
    QList<Listed> guests; // by number
    int containers = 0;   // containers it held, which are not listed (only virtual machines are shown)
};
// The virtual machines of that list, taken by kind: what is no guest (a template, a node, a container, a number that
// is none, a node that is no name) is left out; each number once, the first; no more than mostGuests.
Listing guestsIn(const QJsonValue& resources);

// How a guest is, as its status (status/current) says it: running, stopped, or neither (nothing said, or a word of a
// kind of its own); and what goes on its card.
struct Current {
    std::optional<bool> on;
    QString qmpStatus;            // its QEMU's word ("running", "paused"), a line
    QString name;                 // what it is called, a line
    std::optional<qint64> uptime; // seconds its process has run
    int cpus = 0;                 // 0: not said
    std::optional<double> memoryGib;
};
Current currentOf(const QJsonValue& data);
// A guest's card as a look found it (the web's `_read_status`): "online" while it runs, "no_signal" while it is
// stopped; said as a server is (no picture of its own); its power, its processors and memory; "VM <number> on
// <node>" and Proxmox's version. What it can do (`can`) is the driver's to add.
Status statusOf(const Current& current, qint64 vmid, const QString& node, const QString& version);

// Rule 30: the number a virtual machine gives for itself, by which a guest is known whatever number Proxmox gives it (a
// new one gets the lowest that is free: a guest deleted and another made has its number). Its SMBIOS UUID, as its
// settings (`config`) say it in `smbios1` ("uuid=23e4e388-...,manufacturer=QEMU"; a clone is given a new one), kept
// as a digest: "proxmox:" and 32 hex digits of its SHA-256 (the web's own_number), which a wall's file carries as a
// device's own number (exchange::isADevicesOwnNumber). Empty where there is none worth the name: none said, a setting
// longer than a setting is, or a UUID of fewer than four different digits (noughts).
QString ownNumber(const QJsonValue& config);

// --- Its virtual media (step 7): an ISO image on one of Proxmox's storages, in the guest's CD-ROM drive ------------
// Nothing moves: the image is on Proxmox already, and the guest's settings name it (`ide2:
// local:iso/x.iso,media=cdrom`), as the web's 0.3.0 has it (proxmox.py, "virtual media", read from Proxmox's own
// source: qemu-server's Qemu.pm, pve-storage's Storage.pm). Never Proxmox's download-url (Proxmox would fetch an
// address, and with it probe its own network) and no upload.

// The storages of a node whose images are listed (MOST_STORAGES); the images, mostMediaImages at the most.
inline constexpr qsizetype mostStorages = 64;
// What Proxmox names a storage by (pve-storage-id).
bool isStorageId(QStringView text);
// An ISO image on a storage as Proxmox names it, its volume ("local:iso/debian-13.iso"), and as the program hands it
// back to Proxmox: a name of the characters Proxmox leaves in the name of an image uploaded to it and no others (a
// comma would add an option to the drive's setting, a slash name another place), ending in .iso or .img (ISO_VOLUME).
// Its storage and its name; none for what is no such volume.
struct IsoVolume {
    QString storage;
    QString name;
};
std::optional<IsoVolume> isoVolumeOf(QStringView volume);
// A drive's setting as Proxmox writes it ("local:iso/x.iso,media=cdrom,size=1G", "file=local:iso/x.iso,media=cdrom",
// "none,media=cdrom"): what is in it, and its options; none for what is no such setting (the web's drive_of).
struct Drive {
    QString disc;
    QHash<QString, QString> options;
};
std::optional<Drive> driveOf(const QJsonValue& setting);
// Where a guest's CD-ROM drive may be, in the order they are looked at: ide2 first (where Proxmox's own page adds one),
// ide0, ide1, ide3, sata0 to sata5, scsi0 to scsi30 (CD_SLOTS); and where one is added, the first ten (NEW_CD_SLOTS).
const QStringList& cdSlots();
const QStringList& newCdSlots();
// The guest's CD-ROM drive, the first that holds something, else the first there is, and what is in it ("none":
// nothing); none where its settings have no CD-ROM drive (the web's cd_of).
struct Cd {
    QString slot;
    QString disc;
};
std::optional<Cd> cdOf(const QJsonValue& config);
// A guest's boot order ("order=scsi0;ide2;net0") as a list; none where it is written the old way ("cdn", and where
// none is written, which Proxmox takes for that) or otherwise than this reads (the web's boot_order).
std::optional<QStringList> bootOrderOf(const QJsonValue& config);
// What is in the guest's CD-ROM drive, as its settings say it, for its card and its media's list (the web's
// media_now): an image of a storage by its name and its storage, the host's own CD drive ("cdrom"), or what else is
// there by the end of its path, as a line; read-only; whether its boot order begins with that drive. None where
// nothing is in it, or it has none.
std::optional<MountedMedia> mountedIn(const QJsonValue& config);
// The SHA-1 of the guest's settings file as Proxmox says it with them (`digest`, 40 hex); empty where it says none.
QString digestOf(const QJsonValue& config);
// The storages of a node that hold ISO images (GET /nodes/<node>/storage?content=iso): each named as a storage is,
// once, holding "iso" among its contents, mostStorages of them; and a sentence for each that Proxmox says is not
// active, which is not asked.
struct Storages {
    QStringList names;
    QStringList unseen;
};
Storages storagesIn(const QJsonValue& listed);
// The images of one storage (GET /nodes/<node>/storage/<storage>/content?content=iso), taken by kind: an ISO image of
// this storage, named as isoVolumeOf takes one, of content "iso", with its size where it is a number of 0 to 2^50;
// `room` of them at the most. And how many entries were left out.
struct Images {
    QList<MediaImage> images;
    int leftOut = 0;
};
Images imagesIn(const QJsonValue& content, const QString& storage, qsizetype room);

} // namespace qkvm::core::proxmox
