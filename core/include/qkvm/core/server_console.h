#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include <QByteArray>
#include <QObject>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/bmc/aspeed.h>
#include <qkvm/core/typed_keys.h>

namespace qkvm::core {

// A server's own screen to whoever shows it, whichever console of its controller it comes from: AMI's (ServerScreen,
// its own protocol with the picture in ASPEED's format) or OpenBMC's (RfbScreen, RFB over a WebSocket). One picture
// path for both: the screen as it stands, the rectangles a frame changed (Shown), a card's still made of it, and its
// keyboard and mouse in turns. What each console's manners are is its own (server_screen.h, rfb_screen.h); what is
// said here holds for both.
//
// * look(): the screen is wanted now (whoever shows it asks at least every few seconds); it opens a session where none
//   is open and none was refused for now, and holds one open. Ok once the screen has been shown in the session that is
//   open; "no_stream" while one opens; why the last came to nothing, until the next may be asked for.
// * askForInput(): the console's keyboard and mouse, for a turn (a number no other turn has); keyboard(), mouse() and
//   inTurn() of that turn; and typings (press, type) one after the other.
// * ended: a session ended, and how (an empty code: nobody looked, and it was given back); leftDown: a key or a button
//   this program let go of may have stayed down on the server.
//
// On the thread it was made on.
class ServerConsole : public QObject {
    Q_OBJECT

public:
    // The screen, each time a frame changed it.
    using Shown = std::function<void(const bmc::aspeed::Picture&, std::span<const bmc::aspeed::Rect>)>;
    using StillAnswer = std::function<void(base::Result<QByteArray>)>;
    // A turn of a console's keyboard and mouse: a number no other turn of any console of this program has.
    using Turn = std::int64_t;
    using InputAnswer = std::function<void(base::Result<Turn>)>;
    using Still = std::function<bool()>;
    using Typed = std::function<void(base::Result<void>)>;

    explicit ServerConsole(QObject* parent = nullptr) : QObject(parent) {}

    // A turn of a console's keyboard and mouse, numbered for the whole program: a turn of one screen's is no other's.
    static Turn aNewTurn() noexcept;

    virtual base::Result<void> look() = 0;
    // Somebody has the screen live (a live of it asks for the screen now): a console whose pace is asked for (RFB's)
    // asks at a viewer's pace for a moment. Nothing for one that sends at its own pace.
    virtual void liveSeen() {}
    // The session's screen as it stands; empty before its first frame and after it ended.
    virtual const bmc::aspeed::Picture& picture() const noexcept = 0;
    // The screen's still for a card `width` pixels wide (base/screen_still.h), handed to `done` later on this thread:
    // the packet, or why there is none. Not answered where this is thrown away first.
    virtual void still(int width, StillAnswer done) = 0;
    // Ends the session that is open, letting go of what is down, and opens none again. Whoever waits for keys or a
    // typing is answered ("dropped").
    virtual void close() = 0;

    virtual void askForInput(InputAnswer answer) = 0;
    // The whole keyboard (the modifiers and the keys down), in that turn.
    virtual base::Result<void> keyboard(const typed::Report& keys, Turn turn) = 0;
    // The mouse, as an absolute one: the buttons held (bits, typed::buttonBit), the place (0 to 32767 each way over the
    // screen) and a turn of the wheel (one notch, up positive).
    virtual base::Result<void> mouse(std::uint8_t buttons, int x, int y, int wheel, Turn turn) = 0;
    virtual base::Result<void> inTurn(Turn turn) const = 0;
    // Keys pressed together and let go of (an action's keys, Ctrl+Alt+Del), or a text typed, one typing after the
    // other; `still` is asked whether whoever asked may still have it done, before its first key and as it goes. A
    // text with `thenEnter` has Enter pressed after it in the same typing, in its turn of the keyboard (as the web
    // types it: the text with a line's end after it), never in a typing of its own after it, which would ask for the
    // keyboard anew and might be given a new session's after the text was lost with the last (outside review 2.3).
    virtual void press(const QString& keys, Still still, Typed done) = 0;
    virtual void type(const QString& text, Still still, Typed done, bool thenEnter = false) = 0;
    virtual void stopTyping() = 0;

signals:
    void ended(const QString& code, const QString& message);
    void leftDown();
};

} // namespace qkvm::core
