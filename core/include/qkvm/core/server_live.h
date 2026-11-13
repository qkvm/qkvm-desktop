#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <QByteArray>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <qkvm/bmc/aspeed.h>
#include <qkvm/core/live.h>
#include <qkvm/core/server_screen.h>
#include <qkvm/core/typed_keys.h>

namespace qkvm::core {

// A server's own screen, live (live.h): its pictures from its controller's console (ServerScreen) as what changed of
// it, and keyboard and mouse to it over the same console. As KvmdLive is for a KVM of this PC's own; the engine draws
// its pictures as its Codec::Rgb, copying to the card what changed and nothing else.
//
// Made over a screen that whoever made it keeps: the server's driver, which holds one console for the card's still
// and for the live picture (the owner's decision, 2026-10-29), and hands this the screen's frames from its Shown
// (shown()). It keeps the console while it lives (ServerScreen::look, every Limits::lookEveryMs).
//
// Pictures. Each a packet of base/screen_changes.h (Picture::changes): the whole screen first, said with its size
// (`videoChanged`), and whenever the size is another or a new console session begins; then the rectangles each frame
// changed. A whole screen is a key frame, and askForAKeyFrame() has one at once (the screen is held here as it
// stands, the console's own picture of it). A frame that changes nothing gives nothing.
//
// Keyboard and mouse, as ServerScreen's turns. `askControl(true)` asks for the console's keyboard and mouse
// (ServerScreen::askForInput), and `controlChanged(true)` says the turn was given; a no passes on the screen's code
// and sentence. Every asking and every giving back is answered by one word of its own, in the order they were said,
// a turn of the loop later at the soonest; an asking while it is being given, or held, gets no second word (also one
// that follows a giving back said while the first was still being asked for: asked, given back, asked again, the
// words are a no for the first, and the giving back's; as KvmdLive has it). Keys go
// as the whole keyboard (typed::Keyboard), the pointer as an absolute place (each 0 to 32767 over the screen; a move
// steps that place by as much of the screen as it is in pixels), buttons 1 to 3 (back and forward have no bit
// there), the wheel a report a notch up to sixteen. Where the turn ends (the console was given to somebody at the
// controller's own page, switched to another session, or ended), `controlChanged(false)` says why, as soon as it is
// seen (every Limits::lookEveryMs at the latest, or at the next key). Given back, closed or thrown away: what is down
// is let go of first. Where the screen says that what was let go of may have stayed down (ServerScreen::leftDown),
// whoever holds this is told (`leftDown`): while it holds keyboard and mouse, just before the `controlChanged(false)`
// for that end; and after they went (given back, taken away, closed from here), where it had something down then,
// until the console's session ends (the controller lets go of what a session had down as it ends).
//
// `closed` is said once, where the screen is thrown away ("no_picture"): the screen opens its console again by
// itself, and a live of a server otherwise ends when whoever shows it closes it. A screen that is closed (not thrown
// away) shows nothing more: whoever closes it closes this too (the wiring's). On the screen's thread (where the screen
// lives on another, whoever shows it passes through a line of its own: 3d's wiring).
class ServerLive : public Live {
    Q_OBJECT

public:
    struct Limits {
        int lookEveryMs = 2000; // how often the console is kept, and whether the turn still stands is looked at
    };

    // `screen` outlives this, or is closed before it goes.
    ServerLive(ServerScreen* screen, Limits limits, QObject* parent = nullptr);
    ~ServerLive() override;
    ServerLive(const ServerLive&) = delete;
    ServerLive& operator=(const ServerLive&) = delete;
    ServerLive(ServerLive&&) = delete;
    ServerLive& operator=(ServerLive&&) = delete;

    // What the screen showed (its Shown): the picture as it stands and what the frame changed.
    void shown(const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed);

    Pointer pointerMode() const override { return Pointer::Absolute; }
    bool isOpen() const override;
    void setNeed(Need need) override;
    void askForAKeyFrame() override;
    void askControl(bool on) override;
    bool hasControl() const override { return m_turn.has_value(); }
    void key(int usage, bool down) override;
    void pointerAt(int x, int y) override;
    void pointerBy(int dx, int dy) override;
    void button(int number, bool down) override;
    void wheel(int dx, int dy) override;
    void releaseAll() override;
    void close() override;

private:
    // Hands on the screen whole, as it stands; nothing where it has no picture.
    void sayWhole(const bmc::aspeed::Picture& picture);
    void say(QByteArray packet, bool whole);
    void look();
    // A report of the mouse as it stands, at its place.
    void sayMouse(int wheel);
    // What a key or the mouse came to: where the turn is over, keyboard and mouse are no longer this one's.
    void keep(const base::Result<void>& said);
    // Keyboard and mouse are no longer this one's, for this reason (an empty code: given back).
    void lose(const QString& code, const QString& message);
    // One word for an asking or a giving back, a turn of the loop later.
    void tell(bool on, const QString& code, const QString& message);
    // What the screen answered an asking for keyboard and mouse, decided (a turn of the loop after it came).
    void answered(const base::Result<int>& turn);

    QPointer<ServerScreen> m_screen;
    Limits m_limits;
    QTimer m_looks;
    bool m_closed = false;
    bool m_wholeNext = true; // the next frame goes as the whole screen
    std::pair<int, int> m_size;
    quint32 m_counter = 0;
    // Keyboard and mouse: the turn held, whether one is being asked for, and what is down there by this one's word.
    std::optional<int> m_turn;
    bool m_asking = false;
    bool m_givenBackWhileAsking = false;
    // Something this one had down was let go of, and the session it was let go of in has not ended: what the screen
    // says may have stayed down is this one's to pass on.
    bool m_lettingGo = false;
    typed::Keyboard m_keyboard;
    std::uint8_t m_buttons = 0;
    std::pair<int, int> m_place{16383, 16383};
};

} // namespace qkvm::core
