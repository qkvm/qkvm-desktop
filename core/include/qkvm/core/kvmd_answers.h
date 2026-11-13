#pragma once

#include <optional>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QNetworkCookie>
#include <QSize>
#include <QString>
#include <QUrlQuery>

#include <qkvm/base/result.h>
#include <qkvm/core/server_types.h>

// What a KVM that runs kvmd (PiKVM, GL.iNet Comet) says, read into the program's own types, and what
// it is asked, put together (SPEC.md section 6.3). Nothing here touches the network: these are the
// readers and the writers, kept apart from the lines they travel on so that each can be tried with
// whatever a device might send.
//
// It does what the server's driver does (../app/qkvm/kvm.py), which is where real KVMs' ways were
// learned; where it does otherwise, the reason stands beside it. A KVM is somebody else's software
// on the network: every text of its is cut to a line, every number is held to a range, and an
// answer that is not of the expected shape is no answer.
namespace qkvm::core::kvmd {

// The most a KVM may send in answer to anything but a picture.
inline constexpr qsizetype largestReplyBytes = qsizetype{2} * 1024 * 1024;
// The most it may send as one picture: as the server's driver takes it (a large screen, not
// packed well, is some megabytes; one that is larger is cut off).
inline constexpr qsizetype largestPictureBytes = qsizetype{32} * 1024 * 1024;
// A picture's stated size, before anything is made of it: a few kilobytes can say they are sixteen
// thousand pixels square.
inline constexpr int longestPictureSide = 16384;
inline constexpr qint64 mostPicturePixels = 40'000'000;
// The most one "type text" sends, in characters, and the most computers a KVM with several lists.
inline constexpr qsizetype mostTypedCharacters = 2000;
inline constexpr int mostPorts = 16;

// A text of a KVM's as one short line that can be shown and written down: what does not print is
// a blank, blanks are one blank, and it is cut. Nothing where it is no text or number, or empty.
QString plain(const QJsonValue& value, qsizetype limit = 80);

// kvmd answers everything as {"ok": ..., "result": {...}}. The result of an answer, whatever it
// says of itself; "bad_response" where the answer is not of that shape.
base::Result<QJsonObject> resultFrom(const QByteArray& answer);
// Whether what answered an unsigned "who is signed in?" (GET /api/auth/check) is a kvmd: it
// answers in its own shape, signed in or not.
bool isKvmd(const QByteArray& answer);

// Whether this could be a session's pass: it is sent back to the KVM in a header with every
// question, so only letters that are safe there, and not without end.
bool isToken(const QByteArray& value);
// The pass a good answer to a sign-in brings: one maker hands it over in the answer, the other as
// the cookie "auth_token". Empty: the answer brought none, or one that is not a pass, or two.
QByteArray tokenFrom(const QByteArray& answer, const QList<QNetworkCookie>& cookies);

// What a KVM says it is.
struct About {
    QString model;
    QString version;
    bool operator==(const About&) const = default;
};
About aboutFrom(const QJsonObject& version);
// The name a KVM gives itself (one maker's only); empty where it says none.
QString hostnameFrom(const QJsonObject& said);

// The computers behind a KVM that has several, and which one is on its screen. Nothing for a KVM
// with one: such a KVM may still answer with the empty shell of this.
struct Ports {
    QList<Port> list;
    int shown = -1;
};
std::optional<Ports> portsFrom(const QJsonObject& switchState);

// How a KVM is, from its answers: the video (the one that says whether it is there at all), and
// where they came, its hardware, its keyboard, its power board. `ports` and `about` as read above.
// What it could not say is left as not known: that changes what a card can tell, and not whether
// the KVM counts as there.
struct Looked {
    QJsonObject streamer;
    std::optional<QJsonObject> info;
    std::optional<QJsonObject> hid;
    std::optional<QJsonObject> atx;
    About about;
    std::optional<Ports> ports;
};
Status statusFrom(const Looked& looked);
// Whether the computer behind a KVM's power board is on, by what the board says of itself (GET /api/atx, or the `atx`
// event on its line for events); nothing where it does not say. GLKVM's own word decides where the answer has one:
// "on", and "sleep" (the power is there, and a short press wakes it), or "off"; a word of another kind, or one not
// known, says nothing, and the light beside it is no witness there: a real Comet's stays off whatever the computer
// does. Stock kvmd has no word, and its light (`leds.power`) it is. (The web's `power_said`.)
std::optional<bool> powerSaid(const QJsonObject& atx);
// Whether a power board's state says that the board is there: only a plain yes. (A Comet's board says "no board" to
// whoever asks while it answers somebody else, on its line for events too: that says nothing of the board.)
bool boardIsThere(const QJsonObject& atx);
// The longest message on a KVM's line for events that is read as its power board's state (a real Comet's is some
// ninety bytes; the web's MOST_BOARD_WORD).
inline constexpr qsizetype mostBoardWord = 4096;
// A power board's state as it said it on the KVM's line for events (kvmd's /api/ws: an `atx` event, the whole state
// each time, in the shape of /api/atx's answer); nothing where the message is no such thing. Only what is read of it is
// kept, each by its kind, and what is of another kind is kept as saying nothing (a null): whether it is there and
// busy, GLKVM's word for the computer's power where it said one, and the power light. (The web's `board_heard`.)
std::optional<QJsonObject> boardHeard(const QByteArray& message);
// Whether a Fingerbot is paired with the KVM, by its `fingerbot` event on the line for events: `{"exist": true}` on a
// real Comet with one, `{"exist": false}` on one without; nothing where the message is no such thing, or says neither
// (the web's `fingerbot_heard`). No longer than mostBoardWord.
std::optional<bool> fingerbotHeard(const QByteArray& message);
// A Fingerbot's battery in percent, by the `result` of the Comet's answer at /api/fingerbot/battery (`{"battery": 101}`
// on a real one): above a hundred is a full one; nothing where it is no number of 0 to 255 (the web's `battery_said`).
std::optional<int> batterySaid(const QJsonObject& result);
// Below this many percent, a Fingerbot's battery is low, as the Comet's own page shows it (the web's FINGERBOT_LOW).
inline constexpr int lowBattery = 25;
// How wide the picture is that the computer sends, where the KVM says; 0: it does not.
int sourceWidthFrom(const QJsonObject& streamer);

// Which size of picture is asked of a KVM for a card `width` pixels wide: the smallest of the
// KVM's own sizes that is at least that wide, so that nothing is ever drawn larger than it came;
// "full" beyond the largest, and where the computer's own picture is no wider than the size would
// be (`sourceWidth`, 0: not known). Shrinking costs the KVM a few tenths of a second and pays only
// for small pictures; the picture as captured costs it next to nothing.
QString sizeFor(int width, int sourceWidth);
// What stands behind the question mark of GET /api/streamer/snapshot for that size.
QUrlQuery snapshotQuery(const QString& size);
// Width and height a PNG or a JPEG says it has, read from its first parts; nothing: it is neither,
// or does not say.
std::optional<QSize> pictureSize(const QByteArray& data);
// Whether a picture of that stated size is shown at all.
bool isShown(QSize size);

// One question that does something: where it goes, what goes with it, and how long the KVM may
// take over it before it counts as not answering (it answers when it is done, and typing a long
// text or switching to another computer takes it a while).
struct Call {
    QString path;
    QUrlQuery query;
    QByteArray body;
    int quietMs = 0; // how long nothing may come: 0, as for any question
};
// The keyboard's Power key (USB 0x66, "Power" by the web's name for it) shuts a machine down without
// the question every power action is asked first. kvmd is told a key's name as it comes, and has
// this one: no keys action that names it is sent, on this PC's wall or from a file, and the menu
// does not offer to send it. In the server's words (kvm.py: names_the_power_key, POWER_KEY_REFUSED,
// its 422 "invalid"). A name between commas, whole: "PowerOff" is another.
bool namesThePowerKey(QStringView keys);
inline constexpr const char* powerKeyRefused =
    "The keyboard's Power key is not sent. A machine's power is under Power.";

// What an action is for a KVM of this family; refused ("invalid") where it is none of its, or is
// not filled in as that kind needs (keys that name the keyboard's Power key among them). `ports`:
// how many computers the KVM has, as it last said.
base::Result<Call> callFor(const Action& action, int ports);
// Why a KVM did not do it, in its own words where it has any (cut to a line); else that it refused.
base::Error refusalOf(int httpStatus, const QByteArray& answer);
// The computers a KVM has saved to wake (one maker's only).
QList<WakeTarget> wakeTargetsFrom(const QJsonObject& said);

} // namespace qkvm::core::kvmd
