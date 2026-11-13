#pragma once

#include <qkvm/base/result.h>
#include <qkvm/bmc/aspeed.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The console of a management controller that runs AMI's firmware (MegaRAC): what is sent, what arrives, how a
// frame is put together from it, and the keyboard and mouse reports that go up to it. As the web's app/qkvm/ami.py,
// without the network, the sign-in or the console's manners (all of which are elsewhere: core's ServerScreen).
//
// Redfish says how a server is; it has no request for what is on its screen. The screen is in the controller's
// own console, the one its web page opens: a WebSocket at /kvm that speaks a protocol of AMI's, with the picture
// in the format of the chip that takes it (aspeed.h). How a session goes, as the web's has it: the controller
// says the connection is allowed (`kind::allowed`); it is answered with the console pass (`hello`), and says
// whether the pass is good (`kind::passSeen`). From then on it sends the screen, a full frame first and then
// what changes, every frame in pieces (`Frames` puts them together). A session is ended by saying so
// (`goodbye`). A console is shared: when another comes, the controller asks the holder whether they may have it
// (`kind::sharing`), and whoever only looks lets them in (`letIn`).
//
// Nothing here trusts what arrives: a message is no longer than a frame may be, no more of them are taken in
// at once than a console sends, and a frame is no longer than a screen can be, said before its pieces are kept.
// Plain C++ on the standard library alone; one thread at a time uses a reader.
namespace qkvm::bmc::ami {

using Bytes = std::vector<std::uint8_t>;

// The kinds of message that matter for looking (the numbers are the protocol's).
namespace kind {
inline constexpr std::uint16_t hid = 1;         // a keyboard or mouse report, going up to the controller
inline constexpr std::uint16_t resume = 6;      // "send the picture"
inline constexpr std::uint16_t stop = 8;        // the controller ends the session (its status says why)
inline constexpr std::uint16_t mouseMode = 10;  // which mouse the server shows: absolute or relative
inline constexpr std::uint16_t fullScreen = 11; // "send the whole screen, not only what changes"
inline constexpr std::uint16_t pass = 18;       // the console pass, shown to the controller
inline constexpr std::uint16_t passSeen = 19;   // ...and what it makes of it
inline constexpr std::uint16_t webSession = 21; // which web session this console belongs to
inline constexpr std::uint16_t tooMany = 22;    // every console seat is taken
inline constexpr std::uint16_t allowed = 23;    // the first thing the controller says
inline constexpr std::uint16_t video = 25;      // a piece of a frame
inline constexpr std::uint16_t sharing = 32;    // who may use a console that more than one has open
inline constexpr std::uint16_t others = 39;     // who else has the console open
inline constexpr std::uint16_t nextHolder = 50; // who holds the console from now on
inline constexpr std::uint16_t licence = 53;    // what the controller is licensed for
inline constexpr std::uint16_t stillThere = 57; // asked by the controller, and echoed
inline constexpr std::uint16_t complete = 58;   // "the connection is complete", to firmware that lets one reconnect
inline constexpr std::uint16_t refused = 59;
inline constexpr std::uint16_t behind = 60; // how many frames the viewer has not drawn yet (it is told: none)
} // namespace kind

// Why the controller ends a session (the status of its `kind::stop`), in words for the log; empty for a number
// it is not known to say. One of them is no fault: the session's time was up.
inline constexpr std::uint16_t timeUp = 9;
std::string_view endedBecause(std::uint16_t status) noexcept;

// What a message about sharing is about (the low byte of its status)...
namespace sharing {
inline constexpr unsigned asked = 1;    // whoever holds the console is asked whether another may have it
inline constexpr unsigned waiting = 2;  // ...and that other is told to wait for the answer
inline constexpr unsigned answered = 4; // the answer, to the one who waited
// The low byte that says this console is now the one that holds the keyboard and mouse ("the console is
// yours"): a lone session is greeted with it (`kind::nextHolder`, status 6).
inline constexpr unsigned becameMaster = 6;
} // namespace sharing
// ...and the answer it carries (the high byte).
namespace answer {
inline constexpr unsigned letIn = 0;
inline constexpr unsigned turnedAway = 1;
inline constexpr unsigned pictureOnly = 2;
inline constexpr unsigned holderReturning = 7; // nobody answered: the holder's console dropped, and they may come back
inline constexpr unsigned shutOut = 8;         // the holder has set that nobody else comes in
} // namespace answer
// The status a `kind::passSeen` comes with where this is the first console session there is: the controller
// attaches its virtual keyboard and mouse to the server only now, and they take moments to appear.
inline constexpr std::uint16_t firstSession = 2;
// Which mouse the server shows (the body of a `kind::mouseMode`).
namespace mouse {
inline constexpr std::uint8_t absolute = 2;
inline constexpr std::uint8_t relative = 1;
} // namespace mouse

// No message is longer than a frame may be, and a controller that says one is, is not believed.
inline constexpr std::size_t longest = aspeed::longest;
// ...nor do more than this many arrive in one go. The smallest is eight bytes: megabytes of those, every one made
// into an object on the thread that served every browser, and the web's served nothing for nine seconds.
inline constexpr std::size_t mostAtOnce = 256;
// What every message begins with: its kind (16 bits), how many bytes follow (32), and a number whose meaning is
// the kind's (16), least significant byte first.
inline constexpr std::size_t headBytes = 8;
// ...and the most that arrives in one go (one message of the WebSocket the console is): the longest message and a
// little, as the web's takes at the most (its socket's max_size). The line that carries it is held to the same.
inline constexpr std::size_t largestArrival = longest + 64;
// Where in a frame's first piece the facts about the frame stand (after the two bytes every piece begins with),
// and how many there are.
inline constexpr std::size_t frameFacts = 86;

// What the controller's own page calls its user (it has no way of knowing who that is), and so what a
// controller is used to.
inline constexpr std::string_view defaultName = "domain/username";
// Firmware that lets a console reconnect after a break says so in the list of what it can do.
inline constexpr std::string_view reconnects = "KVM_SESSION_RECONNECT";

struct Message {
    std::uint16_t kind = 0;
    std::uint16_t status = 0;
    Bytes body;
};

// One message, as it is sent. (Its body is the program's own, or one a controller sent: never longer than
// `longest`.)
Bytes message(std::uint16_t kind, std::uint16_t status = 0, std::span<const std::uint8_t> body = {});

// What the controller is answered with when it says the connection is allowed: the pass, and "send the picture".
// `completeFirst`: begin with "the connection is complete", as firmware that lets a console reconnect expects.
// Each text goes in a field of its own size, padded with zeros and never filled to its last byte (what reads it
// there looks for the zero that ends it); it is taken as UTF-8, and a character that is not ASCII goes as "?".
Bytes hello(std::string_view consolePass, std::string_view ownAddress, std::string_view controllerAddress,
            std::string_view name = defaultName, bool completeFirst = false);

// What the console's holder says to let another have it, keys and all; `who` as the controller named them when
// it asked. Said twice over, as the controller's own page says it: once as the answer, once as "the console is
// theirs now".
Bytes letIn(std::span<const std::uint8_t> who);

// What ends a session, said before the connection is closed.
Bytes goodbye();

// What a question about the licence (kind::licence) is answered with, as the controller's own page answers it: its
// web page's session, as text in 35 bytes (padded with zeros, and filled to the last where it is that long).
Bytes answerToLicence(std::string_view webSession);

// Keys and the mouse go up to the controller in a message of `kind::hid` each, which it hands to the server's
// virtual USB keyboard or mouse. Its body is an "IUSB" packet: a header of 32 bytes that names the device the report
// is for and sums to zero (the controller checks), one byte that says how long the report is, and the report as the
// USB HID standard has it for a boot keyboard and mouse. The header's fields are the controller's, as its own web
// viewer sends them (read from that viewer by the web's ami.py; the real boards are driven so). Each report carries
// a number of 31 bits, one higher than the one before it, as the controller's own viewer numbers them.
//
// The keys one keyboard report holds beside the modifiers: a USB boot keyboard's own limit.
inline constexpr std::size_t mostKeys = 6;
// The farthest place of an absolute mouse, across the screen and down it (0 is the top left).
inline constexpr int farthest = 0x7FFF;

// The whole keyboard: the modifiers held, a bit each (bit 0 left Ctrl, then left Shift, left Alt, left Windows,
// and the same four of the right), and the keys down, by their USB numbers (HID usage page 7), the first six of
// them. A key that is in no report any more is let go of: a combination is held by saying it, and let go of by a
// report without it. (The second byte of the report is held at zero: the controller's own viewer puts a letting go
// of its own there, which would not let a combination be held.)
Bytes keyboard(std::uint8_t modifiers, std::span<const std::uint8_t> keys, std::uint32_t sequence);
// The mouse, where the server's is absolute (as AMI's boards are by default, and as the controller says:
// `kind::mouseMode`): the buttons held (bit 0 left, bit 1 right, bit 2 middle), the place, each way 0 to
// `farthest` (beyond is the edge), and a turn of the wheel (-127 to 127; beyond is the most).
Bytes mouseAbsolute(std::uint8_t buttons, int x, int y, int wheel, std::uint32_t sequence);
// The mouse, where the server's is relative: the buttons held, how far it moved each way and a turn of the wheel
// (each -127 to 127; beyond is the most). This program sends it only to let go of buttons (ServerScreen).
Bytes mouseRelative(std::uint8_t buttons, int dx, int dy, int wheel, std::uint32_t sequence);

// What arrives over the connection, cut into the messages it is made of (they need not arrive whole).
class Messages {
public:
    // Every whole message there is now, or why what arrived is no console's ("garbled"): more in one go than
    // largestArrival (refused before any of it is kept), a message that says it is longer than any, or more at
    // once than a console sends. After a refusal the connection is not read on.
    base::Result<std::vector<Message>> feed(std::span<const std::uint8_t> data);
    // What is kept for the next time: less than a message's head, or a message that has not all arrived (so
    // less than `headBytes + longest` once a feed has been taken in).
    std::size_t held() const noexcept { return m_waiting.size(); }

private:
    Bytes m_waiting;
};

// A frame, put together: its header, and its data as long as the header said.
struct WholeFrame {
    aspeed::FrameHeader header;
    Bytes data;
};

// Frames, put together from the pieces they are sent in. A frame's first piece says how the frame is to be read
// and how long it is; the pieces that follow are more of its data, until that many bytes are there.
class Frames {
public:
    // Takes one video message's body: a frame when this piece completes one, or why it cannot be one
    // ("garbled": a first piece too short to say how to read the frame, a picture sent encrypted, or a frame
    // longer than a screen can be). After a refusal, begin with a new `Frames`, as the web's does.
    base::Result<std::optional<WholeFrame>> piece(std::span<const std::uint8_t> body);
    // The bytes of a frame not yet whole: never more than it said it would be.
    std::size_t held() const noexcept { return m_data.size(); }

private:
    std::optional<aspeed::FrameHeader> m_header;
    std::size_t m_wanted = 0;
    Bytes m_data;
};

} // namespace qkvm::bmc::ami
