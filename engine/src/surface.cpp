#include <algorithm>
#include <cstring>
#include <memory>

#include "gpu_core.h"

// Where a scene is drawn: a window's chain of buffers, or a picture in memory.

namespace qkvm::engine {

namespace {

// Two buffers that Windows flips between: the model every newer Windows wants, and the only one
// that lets a picture be shown without waiting for the monitor.
constexpr UINT buffersInTheChain = 2;
constexpr DXGI_FORMAT screenFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

// The handle through which Windows says that a chain has room, closed when nobody waits on it any more.
struct Room {
    explicit Room(HANDLE from) : handle(from) {}
    ~Room() { CloseHandle(handle); }
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;
    HANDLE handle;
};

class WindowSurface final : public Surface, private DeviceDependent {
public:
    WindowSurface(GpuCore& gpu, HWND window) : m_gpu(gpu), m_window(window) { m_gpu.attach(this); }
    ~WindowSurface() override
    {
        m_gpu.detach(this);
        const std::lock_guard guard(m_gpu.lock());
        dropDeviceObjects();
    }

    base::Result<void> start()
    {
        const std::lock_guard guard(m_gpu.lock());
        return makeChain();
    }

    void resize(SizePx size) override
    {
        const std::lock_guard guard(m_gpu.lock());
        m_wanted = size;
    }

    Target target() override
    {
        const std::lock_guard guard(m_gpu.lock());
        if (m_chain == nullptr && m_gpu.device() != nullptr) {
            // The window was given no chain at the last recovery (a window can have only one, and
            // its old one may not have been gone yet): tried again now.
            if (!makeChain()) {
                dropDeviceObjects();
            }
        }
        if (m_chain == nullptr) {
            return {};
        }
        if (m_wanted.width > 0 && m_wanted.height > 0 && m_wanted != m_size) {
            // The chain's buffers can only change size while nothing refers to them.
            m_view.Reset();
            m_gpu.context()->OMSetRenderTargets(0, nullptr, nullptr);
            const HRESULT resized =
                m_chain->ResizeBuffers(0, static_cast<UINT>(m_wanted.width), static_cast<UINT>(m_wanted.height),
                                       DXGI_FORMAT_UNKNOWN, chainFlags());
            if (FAILED(resized)) {
                if (meansDeviceLost(resized)) {
                    m_gpu.noteLost();
                }
                return {};
            }
            if (!makeView()) {
                return {};
            }
        }
        return {m_view.Get(), m_size, m_gpu.timesRecovered()};
    }

    void waitForRoom(int longestMs) override
    {
        // The handle is kept open for the wait by this copy of what holds it: a recovery on another
        // window's thread may let go of the chain meanwhile.
        std::shared_ptr<Room> room;
        {
            const std::lock_guard guard(m_gpu.lock());
            // Windows says "room" once for each picture it has shown. Room that was waited for and
            // then not used (nothing new was there to draw) is still there: waiting again would be
            // waiting for a picture that was never handed over, to the end of the time allowed.
            if (m_roomTaken) {
                return;
            }
            room = m_room;
        }
        if (room != nullptr &&
            WaitForSingleObjectEx(room->handle, static_cast<DWORD>(std::max(longestMs, 0)), FALSE) == WAIT_OBJECT_0) {
            const std::lock_guard guard(m_gpu.lock());
            m_roomTaken = room == m_room;
        }
    }

    base::Result<void> present(Pace pace) override
    {
        // With the device's lock held, and so never while a decoder on the card is in the middle of
        // a picture: one maker's driver waits inside Present for the card, with Direct3D's own lock
        // held, and a decoder that needs that lock to finish its picture then waits for ever
        // (seen and measured). The wait for the monitor is not in here: it is waitForRoom().
        const std::lock_guard guard(m_gpu.lock());
        if (m_chain == nullptr) {
            return base::Error{deviceLostCode, "There is no graphics device to show a picture with."};
        }
        const bool atOnce = pace == Pace::AtOnce;
        const HRESULT presented =
            m_chain->Present(atOnce ? 0 : 1, atOnce && m_gpu.tearingAllowed() ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (FAILED(presented)) {
            if (meansDeviceLost(presented)) {
                m_gpu.noteLost();
            }
            return graphicsError("Showing a picture", presented);
        }
        m_roomTaken = false;
        if (presented == DXGI_STATUS_OCCLUDED) {
            m_unseen.fetch_add(1, std::memory_order_relaxed);
        }
        return {};
    }

    std::uint64_t timesUnseen() const override { return m_unseen.load(std::memory_order_relaxed); }

private:
    // Windows says through a handle when the chain has room for another picture, and a picture may
    // be shown without waiting for the monitor where the PC allows that.
    UINT chainFlags() const
    {
        return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
               (m_gpu.tearingAllowed() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U);
    }

    base::Result<void> makeChain()
    {
        if (m_gpu.device() == nullptr) {
            return base::Error{deviceLostCode, "There is no graphics device to show a picture with."};
        }
        DXGI_SWAP_CHAIN_DESC1 wanted{};
        wanted.Format = screenFormat;
        wanted.SampleDesc.Count = 1;
        wanted.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        wanted.BufferCount = buffersInTheChain;
        wanted.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        wanted.Scaling = DXGI_SCALING_STRETCH;
        wanted.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        wanted.Flags = chainFlags();
        const HRESULT made =
            m_gpu.factory()->CreateSwapChainForHwnd(m_gpu.device(), m_window, &wanted, nullptr, nullptr, &m_chain);
        if (FAILED(made)) {
            return graphicsError("Making a window's chain of buffers", made);
        }
        // Alt+Enter is the program's to decide, not DXGI's.
        m_gpu.factory()->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
        // Never more than one finished picture waiting for the monitor.
        if (ComPtr<IDXGISwapChain2> newer; SUCCEEDED(m_chain.As(&newer))) {
            newer->SetMaximumFrameLatency(1);
            if (const HANDLE handle = newer->GetFrameLatencyWaitableObject(); handle != nullptr) {
                m_room = std::make_shared<Room>(handle);
            }
        }
        return makeView();
    }

    base::Result<void> makeView()
    {
        ComPtr<ID3D11Texture2D> buffer;
        if (const HRESULT got = m_chain->GetBuffer(0, IID_PPV_ARGS(&buffer)); FAILED(got)) {
            return graphicsError("Asking a window's chain for its buffer", got);
        }
        D3D11_TEXTURE2D_DESC is{};
        buffer->GetDesc(&is);
        if (const HRESULT made = m_gpu.device()->CreateRenderTargetView(buffer.Get(), nullptr, &m_view); FAILED(made)) {
            return graphicsError("Making a window's buffer drawable", made);
        }
        m_size = {static_cast<int>(is.Width), static_cast<int>(is.Height)};
        return {};
    }

    void dropDeviceObjects() override
    {
        m_view.Reset();
        m_chain.Reset();
        m_room.reset();
        m_roomTaken = false;
    }

    void makeDeviceObjects() override
    {
        // A window that cannot be given a chain just now is given one when it is next drawn into.
        if (!makeChain()) {
            dropDeviceObjects();
        }
    }

    GpuCore& m_gpu;
    HWND m_window;
    ComPtr<IDXGISwapChain1> m_chain;
    ComPtr<ID3D11RenderTargetView> m_view;
    std::shared_ptr<Room> m_room;
    bool m_roomTaken = false;
    SizePx m_size;
    SizePx m_wanted;
    std::atomic<std::uint64_t> m_unseen{0};
};

class MemoryCanvas final : public Canvas, private DeviceDependent {
public:
    MemoryCanvas(GpuCore& gpu, SizePx size) : m_gpu(gpu), m_size(size) { m_gpu.attach(this); }
    ~MemoryCanvas() override
    {
        m_gpu.detach(this);
        const std::lock_guard guard(m_gpu.lock());
        dropDeviceObjects();
    }

    base::Result<void> start()
    {
        const std::lock_guard guard(m_gpu.lock());
        return make();
    }

    Target target() override
    {
        const std::lock_guard guard(m_gpu.lock());
        return {m_view.Get(), m_view != nullptr ? m_size : SizePx{}, m_gpu.timesRecovered()};
    }

    base::Result<std::vector<std::uint8_t>> pixels() override
    {
        const std::lock_guard guard(m_gpu.lock());
        if (m_texture == nullptr) {
            return base::Error{deviceLostCode, "There is no graphics device to read a picture from."};
        }
        // A texture that is drawn into cannot be read from memory; a copy made for reading can.
        D3D11_TEXTURE2D_DESC copy{};
        m_texture->GetDesc(&copy);
        copy.Usage = D3D11_USAGE_STAGING;
        copy.BindFlags = 0;
        copy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readable;
        if (const HRESULT made = m_gpu.device()->CreateTexture2D(&copy, nullptr, &readable); FAILED(made)) {
            return graphicsError("Making room to read a picture back", made);
        }
        m_gpu.context()->CopyResource(readable.Get(), m_texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (const HRESULT got = m_gpu.context()->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped); FAILED(got)) {
            if (meansDeviceLost(got)) {
                m_gpu.noteLost();
            }
            return graphicsError("Reading a picture back", got);
        }
        const std::size_t rowBytes = static_cast<std::size_t>(m_size.width) * 4;
        std::vector<std::uint8_t> pixels(rowBytes * static_cast<std::size_t>(m_size.height));
        for (int row = 0; row < m_size.height; ++row) {
            std::memcpy(pixels.data() + rowBytes * static_cast<std::size_t>(row),
                        static_cast<const std::uint8_t*>(mapped.pData) +
                            static_cast<std::size_t>(mapped.RowPitch) * static_cast<std::size_t>(row),
                        rowBytes);
        }
        m_gpu.context()->Unmap(readable.Get(), 0);
        return pixels;
    }

private:
    base::Result<void> make()
    {
        if (m_gpu.device() == nullptr) {
            return base::Error{deviceLostCode, "There is no graphics device to draw with."};
        }
        D3D11_TEXTURE2D_DESC wanted{};
        wanted.Width = static_cast<UINT>(m_size.width);
        wanted.Height = static_cast<UINT>(m_size.height);
        wanted.MipLevels = 1;
        wanted.ArraySize = 1;
        wanted.Format = screenFormat;
        wanted.SampleDesc.Count = 1;
        wanted.Usage = D3D11_USAGE_DEFAULT;
        wanted.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (const HRESULT made = m_gpu.device()->CreateTexture2D(&wanted, nullptr, &m_texture); FAILED(made)) {
            return graphicsError("Making a picture to draw into", made);
        }
        if (const HRESULT made = m_gpu.device()->CreateRenderTargetView(m_texture.Get(), nullptr, &m_view);
            FAILED(made)) {
            return graphicsError("Making a picture drawable", made);
        }
        return {};
    }

    void dropDeviceObjects() override
    {
        m_view.Reset();
        m_texture.Reset();
    }

    void makeDeviceObjects() override
    {
        if (!make()) {
            dropDeviceObjects();
        }
    }

    GpuCore& m_gpu;
    SizePx m_size;
    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<ID3D11RenderTargetView> m_view;
};

} // namespace

base::Result<std::unique_ptr<Surface>> GpuCore::makeSurface(void* window)
{
    auto surface = std::make_unique<WindowSurface>(*this, static_cast<HWND>(window));
    if (base::Result<void> started = surface->start(); !started) {
        return started.error();
    }
    return std::unique_ptr<Surface>(std::move(surface));
}

base::Result<std::unique_ptr<Canvas>> GpuCore::makeCanvas(SizePx size)
{
    if (size.width <= 0 || size.height <= 0 || size.width > 16384 || size.height > 16384) {
        return base::Error{"too_large", "A picture of that size cannot be drawn into."};
    }
    auto canvas = std::make_unique<MemoryCanvas>(*this, size);
    if (base::Result<void> started = canvas->start(); !started) {
        return started.error();
    }
    return std::unique_ptr<Canvas>(std::move(canvas));
}

} // namespace qkvm::engine
