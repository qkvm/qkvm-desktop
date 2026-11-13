#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <qkvm/engine/gpu.h>

#include "d3d.h"
#include "decode_pool.h"
#include "ffmpeg.h"

namespace qkvm::engine {

// Whatever is made from the device, and so has to be let go and made again when the device was
// lost. Both are called on the thread that recovers, with the device's lock held.
class DeviceDependent {
public:
    virtual void dropDeviceObjects() = 0;
    virtual void makeDeviceObjects() = 0;

protected:
    ~DeviceDependent() = default;
};

// A stream as the Gpu needs to know it: something that may hold a decoder on the card.
class CardDecoderHolder {
public:
    // Lets go of the decoder on the card and of every picture it made, waiting for a decode that is
    // under way. Called without the device's lock held.
    virtual void dropCardDecoder() = 0;

protected:
    ~CardDecoderHolder() = default;
};

// The Gpu as the engine's own files see it.
//
// Three locks, and the order they are taken in:
//
//   the streams' list  ->  a stream's decoding  ->  the device
//
// * The device's lock guards the immediate context and everything that draws: scenes, surfaces,
//   canvases keep their drawing state under it. FFmpeg takes it around its own calls into the
//   context, from the decoding threads. It is recursive because FFmpeg asks for that.
// * A stream's decoding lock is held by whoever decodes for that stream, and is never asked for
//   while the device's lock is held.
// * The streams' list is held while the list changes and while a recovery walks it, and is never
//   asked for while one of the other two is held.
// * A stream's screen (Codec::Rgb: the server's screen as it stands, in this program's memory) has a
//   lock of its own, held while a packet is applied to it on a decoding thread and while what changed
//   is copied to the card. The drawing, which holds the device's lock, only tries it, and draws the
//   screen as it was where a packet is being applied; no other lock is asked for while it is held.
class GpuCore final : public Gpu {
public:
    explicit GpuCore(const Options& options);
    ~GpuCore() override;

    // Makes the device. Once, right after the constructor.
    base::Result<void> start();

    std::string adapterName() const override;
    bool decodesH264() const override;
    void setDecoding(DecodingChoice decoding) override;
    DecodingChoice decoding() const override;
    void* nativeDevice() const override;
    void* nativeContext() const override;
    std::unique_ptr<Scene> makeScene(Host host) override;
    base::Result<std::unique_ptr<Surface>> makeSurface(void* window) override;
    base::Result<std::unique_ptr<Canvas>> makeCanvas(SizePx size) override;
    Memory memory() const override;
    void loseDevice() override;
    void failTextures(int count) override { m_texturesToFail.store(count, std::memory_order_release); }
    // Whether the texture about to be made is one a test said is not to be (failTextures). Any thread.
    bool textureFails() noexcept
    {
        int left = m_texturesToFail.load(std::memory_order_acquire);
        while (left > 0 && !m_texturesToFail.compare_exchange_weak(left, left - 1, std::memory_order_acq_rel)) {
        }
        return left > 0;
    }
    void slowApplying(int ms) override { m_applyingMs.store(ms, std::memory_order_release); }
    int applyingMs() const noexcept { return m_applyingMs.load(std::memory_order_acquire); }
    std::uint64_t timesRecovered() const override;
    std::vector<std::string> objections() override;

    std::recursive_mutex& lock() noexcept { return m_lock; }
    // With the device's lock held. Null while the device is gone.
    ID3D11Device* device() const noexcept { return m_device.Get(); }
    ID3D11DeviceContext* context() const noexcept { return m_context.Get(); }
    IDXGIFactory2* factory() const noexcept { return m_factory.Get(); }
    bool tearingAllowed() const noexcept { return m_tearingAllowed; }

    // A new reference to FFmpeg's view of the device, for a decoder on the card; null when the
    // device is gone or cannot decode. Takes the device's lock itself.
    BufferPtr decodingDevice();

    // Any thread: a call has said the device is gone.
    void noteLost() noexcept { m_lost.store(true, std::memory_order_release); }
    bool lost() const noexcept { return m_lost.load(std::memory_order_acquire); }
    // On a drawing thread, without the device's lock held, before it draws: makes the device anew
    // when it was lost. False while there is no device to draw with.
    bool ready();

    void attach(DeviceDependent* dependent);
    void detach(DeviceDependent* dependent);
    void attach(CardDecoderHolder* holder);
    void detach(CardDecoderHolder* holder);
    void attach(const Host* host);
    void detach(const Host* host);

    DecodePool& decodePool() noexcept { return m_pool; }
    DecodingChoice decodingChoice() const noexcept
    {
        return static_cast<DecodingChoice>(m_decoding.load(std::memory_order_acquire));
    }
    // How often the choice of decoding was changed: a stream that sees another number looks at
    // how it should be decoded now (StreamCore::run).
    std::uint64_t decodingChanges() const noexcept { return m_decodingChanges.load(std::memory_order_acquire); }

    // How many decoders on the card there are, and whether one more may be made.
    bool takeCardDecoder() noexcept;
    // Whether a decoder on the card could be made just now: the device decodes H.264 itself and
    // not every decoder it may have is taken. Any thread; what it says may be untrue a moment later.
    bool mayMakeCardDecoder() const noexcept;
    void giveBackCardDecoder() noexcept;

private:
    base::Result<void> makeDevice();
    void dropDevice();
    void tell(Fault fault);

    const Options m_options;
    DecodePool m_pool;

    mutable std::recursive_mutex m_lock;
    ComPtr<IDXGIFactory2> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    BufferPtr m_decodingDevice;
    std::string m_adapterName;
    // (Set as the device is made, with the lock held; read without it.)
    std::atomic<bool> m_decodesH264{false};
    bool m_tearingAllowed = false;
    std::vector<DeviceDependent*> m_dependents;
    std::vector<const Host*> m_hosts;

    std::mutex m_holdersLock;
    std::vector<CardDecoderHolder*> m_holders;

    std::atomic<bool> m_lost{false};
    std::atomic<std::uint64_t> m_recovered{0};
    std::atomic<int> m_cardDecoders{0};
    std::atomic<int> m_decoding;
    std::atomic<std::uint64_t> m_decodingChanges{0};
    std::atomic<int> m_texturesToFail{0};
    std::atomic<int> m_applyingMs{0};
};

} // namespace qkvm::engine
