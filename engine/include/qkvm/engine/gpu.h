#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <qkvm/base/result.h>
#include <qkvm/engine/types.h>

namespace qkvm::engine {

class Scene;
struct Host;

// Where a scene is drawn. Made by a Surface or a Canvas, or by a host that has a render target of
// its own; the pointer is a Direct3D 11 render target view and stays the host's.
struct Target {
    void* renderTargetView = nullptr;
    SizePx size;
    // Which making of the device it belongs to: Gpu::timesRecovered() when it was asked for. What
    // was asked for before the device was lost went with that device, and is not drawn into.
    std::uint64_t made = 0;
};

// A window's pictures on their way to the screen: the chain of buffers Windows shows in turn.
// Used from the drawing thread only.
class Surface {
public:
    enum class Pace {
        WithTheMonitor, // wait for the monitor's next refresh: nothing tears
        AtOnce,         // show now: for the controlled machine, where waiting is the cost
    };

    virtual ~Surface() = default;

    // The window changed size. Taken over before the next draw.
    virtual void resize(SizePx size) = 0;
    // Comes back when Windows has room for another picture, or after this many milliseconds. Call
    // it before drawing a picture that is to be shown with the monitor: what is drawn then is the
    // newest there is, and showing it does not wait. Never with a lock of one's own held.
    virtual void waitForRoom(int longestMs) = 0;
    // Where to draw the next picture.
    virtual Target target() = 0;
    // Hands what was drawn to Windows. With the monitor, and without waitForRoom() before it, this
    // waits for the monitor itself, and every decoder on the card waits with it. A lost device
    // comes back as the error "device_lost".
    virtual base::Result<void> present(Pace pace) = 0;
    // How often Windows took a picture and said that nobody could see it: the window was covered
    // or made small. A measurement made meanwhile says little.
    virtual std::uint64_t timesUnseen() const = 0;
};

// A picture drawn into memory instead of onto a window: for tests, and for a screenshot.
// Used from the drawing thread only.
class Canvas {
public:
    virtual ~Canvas() = default;
    virtual Target target() = 0;
    // What was drawn, row after row from the top, four bytes a pixel: blue, green, red, alpha.
    virtual base::Result<std::vector<std::uint8_t>> pixels() = 0;
};

// Which graphics adapter, where none is named by its number: the most powerful there is (a
// laptop's graphics card rather than the graphics in its processor), the one that uses the least
// power, or the one Windows gives a program that asks for none, which follows what is set for the
// program in Windows' own graphics settings.
enum class AdapterChoice { MostPowerful, PowerSaving, AsWindowsChooses };

// One graphics adapter: it owns the Direct3D device, loses it, and gets it back (DESIGN.md
// section 3.4). Everything made from it must be gone before it is.
class Gpu {
public:
    struct Options {
        // Windows' own software adapter instead of a graphics card. Slow and everywhere the same: for tests.
        bool software = false;
        // Direct3D's own checking of every call. Needs the Graphics Tools of Windows; for tests and debug builds.
        bool checked = false;
        // Which adapter, counted as adapters() lists them. Below zero: the one `choice` comes to.
        int adapter = -1;
        AdapterChoice choice = AdapterChoice::AsWindowsChooses;
        // How many streams may be decoded on the card at once. Each takes a set of surfaces in the
        // card's memory, a 1080p one about sixty megabytes.
        int cardDecoders = 16;
        // How H.264 is decoded to begin with (setDecoding).
        DecodingChoice decoding = DecodingChoice::Automatic;
        // How many threads decode in software; 0: as many as this PC's processors are worth, from 2 to
        // 12. Previews have all of them but one (DESIGN.md section 3.3). For tests and the measuring rig.
        int decodeThreads = 0;
    };

    // The adapters this PC has, as Windows lists them, by what they call themselves.
    static std::vector<std::string> adapters();
    // What the adapter that a choice comes to on this PC calls itself; empty where Windows names
    // none. For showing on this PC, never for a file that is kept.
    static std::string adapterFor(AdapterChoice choice);
    // Whether a monitor is on the adapter that a choice comes to on this PC. Where none is, a window
    // drawn on it is carried to the adapter of its monitor for every frame, by Windows.
    static bool adapterHasMonitor(AdapterChoice choice);

    static base::Result<std::unique_ptr<Gpu>> create(const Options& options);
    virtual ~Gpu() = default;

    // What the adapter calls itself. For a log on this PC, never for a file that is kept.
    virtual std::string adapterName() const = 0;
    // Whether this device can decode H.264 itself.
    virtual bool decodesH264() const = 0;

    // How H.264 is decoded from now on, by every stream of this device. Each whose way of decoding
    // that changes, where the change can be made, changes at its next key frame and wishes for one
    // (Stream::takeKeyFrameWish) as it decodes its next packet, unless that packet is the key frame
    // and makes the change; one whose way stays the same, or cannot change, goes on as it was.
    // Chosen on the card, a stream the card gave up on is tried on it again. Any thread.
    virtual void setDecoding(DecodingChoice decoding) = 0;
    virtual DecodingChoice decoding() const = 0;

    // The Direct3D 11 device and its immediate context, for a host that draws with the same device:
    // an interface toolkit is handed them before its first window is shown, and the engine then
    // draws underneath that toolkit's own drawing (Scene::drawUnder). They stay the Gpu's.
    virtual void* nativeDevice() const = 0;
    virtual void* nativeContext() const = 0;

    virtual std::unique_ptr<Scene> makeScene(Host host) = 0;
    // `window` is the window's handle (an HWND).
    virtual base::Result<std::unique_ptr<Surface>> makeSurface(void* window) = 0;
    virtual base::Result<std::unique_ptr<Canvas>> makeCanvas(SizePx size) = 0;

    // The adapter's memory in use by this program, and how much Windows lets it have. Any thread.
    struct Memory {
        std::uint64_t usedBytes = 0;
        std::uint64_t budgetBytes = 0;
    };
    virtual Memory memory() const = 0;

    // Treats the device as taken away by Windows, from the next draw on: the same path a real loss
    // takes. For tests and for the measuring rig. Any thread.
    virtual void loseDevice() = 0;
    // The next `count` textures made for pictures decoded in memory, or views made of a decoder's
    // texture on the card, cannot be made, as where the card's memory is full, with the device as it
    // was. For tests. Any thread.
    virtual void failTextures(int count) = 0;
    // Each packet of a screen sent as what changed of it (Codec::Rgb) holds its screen `ms` longer as it is applied,
    // from now on (0: as long as it takes): a busy server's screen, for tests. Any thread.
    virtual void slowApplying(int ms) = 0;
    // How often the device was lost and made anew since this Gpu exists. Any thread.
    virtual std::uint64_t timesRecovered() const = 0;

    // What Direct3D's checking has objected to since this was last asked, when it is switched on.
    // A test ends by asking, and a single objection fails it. On the drawing thread.
    virtual std::vector<std::string> objections() = 0;
};

} // namespace qkvm::engine
