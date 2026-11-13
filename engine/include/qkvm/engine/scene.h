#pragma once

#include <functional>
#include <span>
#include <vector>

#include <qkvm/engine/gpu.h>
#include <qkvm/engine/types.h>

namespace qkvm::engine {

// What the engine asks of whoever hosts it. Both may be called from any thread, and must come back
// at once: they are for noting that something is to be done, not for doing it.
struct Host {
    std::function<void()> somethingNew; // a newer picture is ready: draw soon
    std::function<void(Fault)> fault;
    // A newer picture of a stream shown large or controlled is ready: draw it as soon as can be,
    // and on its own. Where a host gives none, somethingNew is told instead.
    std::function<void()> somethingUrgent;
};

// One machine's pictures: packets go in, and the scene draws the newest picture that came out.
// Between the two there is room for exactly one picture; nothing queues.
class Stream {
public:
    virtual ~Stream() = default;

    // Hands in one packet. From one thread at a time. Copies it and comes back at once; when
    // decoding is behind, packets are thrown away up to the next key frame, and that is counted.
    virtual void push(const Packet& packet) = 0;
    // How large the stream is drawn, and how much it matters: decides how it is decoded. Any thread.
    virtual void shownAt(SizePx onScreen, Importance importance) = 0;
    // True once after pictures were lost: the host asks the server for a key frame. Any thread.
    virtual bool takeKeyFrameWish() = 0;
    // Any thread.
    virtual Stats stats() const = 0;
};

// Where a stream is drawn in the window. The picture keeps its shape inside the rectangle; what it
// leaves free is black. `clip`: of all that, only what lies inside this rectangle is drawn (the view
// a picture larger than it is scrolled in); none (no width or no height): all of it.
struct Placed {
    StreamId stream = 0;
    RectPx where;
    RectPx clip{};
};

// A copy of the displayed remote picture, at its decoded size, without window overlays.
// Rows run from the top; each pixel is blue, green, red, alpha.
struct Screenshot {
    SizePx size;
    std::vector<std::uint8_t> pixels;
};

// What one window shows: its streams and where each is drawn.
class Scene {
public:
    virtual ~Scene() = default;

    // Any thread; a draw that is under way on another is waited for. A stream is the scene's: what
    // add() gave is not used after remove() of the same number, nor after the scene.
    virtual Stream& add(StreamId id, Codec codec) = 0;
    virtual void remove(StreamId id) = 0;
    // Taken over at the next draw. Streams that are not placed are not drawn. Any thread.
    virtual void place(std::span<const Placed> placed) = 0;

    // On the drawing thread: draws the newest picture of every placed stream into the target.
    // False when nothing had changed since the last draw and so nothing was drawn.
    virtual bool draw(const Target& target) = 0;
    // For a host that draws the window itself. On the host's drawing thread, inside its pass, with
    // its render target bound and `size` that target's size: draws every placed stream into what is
    // bound, where it is placed, and nothing else. What the host drew before stays, what it draws
    // afterwards lies on top. The host sets its own state again when this comes back. Unlike draw()
    // this draws every time: the host has just cleared what was there.
    virtual void drawUnder(SizePx size) = 0;
    // On the drawing thread: what the last draw drew has reached the screen at this time.
    virtual void shown(std::int64_t atUs) = 0;
    // On the drawing thread, between the host's external-command boundaries. No picture
    // is decoded or requested by this operation. Scenes without readback return no picture.
    virtual base::Result<Screenshot> screenshot(StreamId /*id*/)
    {
        return base::Error{"no_picture", "There is no displayed picture to copy."};
    }
};

} // namespace qkvm::engine
