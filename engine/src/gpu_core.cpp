#include "gpu_core.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#include <qkvm/engine/scene.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

namespace qkvm::engine {

namespace {

// Decoding in software is what fills these threads, and a wall of small tiles needs several of them
// at once; two processors are left to the interface and to drawing.
unsigned decodeThreads()
{
    const unsigned processors = std::thread::hardware_concurrency();
    return std::clamp(processors > 2 ? processors - 2 : 1U, 2U, 12U);
}

std::string narrow(const wchar_t* wide)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string text(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), length, nullptr, nullptr);
    return text;
}

void lockDevice(void* gpu)
{
    static_cast<GpuCore*>(gpu)->lock().lock();
}

void unlockDevice(void* gpu)
{
    static_cast<GpuCore*>(gpu)->lock().unlock();
}

bool hasH264Decoder(ID3D11Device* device)
{
    ComPtr<ID3D11VideoDevice> video;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video)))) {
        return false;
    }
    const UINT profiles = video->GetVideoDecoderProfileCount();
    for (UINT index = 0; index < profiles; ++index) {
        GUID profile{};
        if (SUCCEEDED(video->GetVideoDecoderProfile(index, &profile)) &&
            profile == D3D11_DECODER_PROFILE_H264_VLD_NOFGT) {
            return true;
        }
    }
    return false;
}

} // namespace

base::Error graphicsError(const char* doing, HRESULT result)
{
    char number[16] = {};
    std::snprintf(number, sizeof(number), "0x%08lX", static_cast<unsigned long>(result));
    return {meansDeviceLost(result) ? deviceLostCode : "graphics", std::string(doing) + " failed (" + number + ")."};
}

// The adapter a choice comes to; none for Windows' own choice, which Direct3D makes itself when
// it is given none, and none where Windows has no list that is ordered by power (before Windows 10
// of April 2018), where Windows' own is taken as well.
ComPtr<IDXGIAdapter1> adapterOf(AdapterChoice choice)
{
    if (choice == AdapterChoice::AsWindowsChooses) {
        return nullptr;
    }
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    const DXGI_GPU_PREFERENCE preference = choice == AdapterChoice::MostPowerful ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                                                                 : DXGI_GPU_PREFERENCE_MINIMUM_POWER;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByGpuPreference(0, preference, IID_PPV_ARGS(&adapter)))) {
        return nullptr;
    }
    return adapter;
}

std::vector<std::string> Gpu::adapters()
{
    std::vector<std::string> names;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return names;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        names.push_back(narrow(description.Description));
    }
    return names;
}

// The adapter a choice comes to, also where that is Windows' own: the first it lists, which Direct3D
// takes when it is given none.
ComPtr<IDXGIAdapter1> adapterSeenFor(AdapterChoice choice)
{
    ComPtr<IDXGIAdapter1> adapter = adapterOf(choice);
    if (adapter == nullptr) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory->EnumAdapters1(0, &adapter) != S_OK) {
            return nullptr;
        }
    }
    return adapter;
}

std::string Gpu::adapterFor(AdapterChoice choice)
{
    const ComPtr<IDXGIAdapter1> adapter = adapterSeenFor(choice);
    if (adapter == nullptr) {
        return {};
    }
    DXGI_ADAPTER_DESC1 description{};
    adapter->GetDesc1(&description);
    return narrow(description.Description);
}

bool Gpu::adapterHasMonitor(AdapterChoice choice)
{
    const ComPtr<IDXGIAdapter1> adapter = adapterSeenFor(choice);
    ComPtr<IDXGIOutput> output;
    return adapter != nullptr && adapter->EnumOutputs(0, &output) == S_OK;
}

base::Result<std::unique_ptr<Gpu>> Gpu::create(const Options& options)
{
    auto gpu = std::make_unique<GpuCore>(options);
    if (base::Result<void> started = gpu->start(); !started) {
        return started.error();
    }
    return std::unique_ptr<Gpu>(std::move(gpu));
}

GpuCore::GpuCore(const Options& options)
    : m_options(options),
      m_pool(options.decodeThreads > 0 ? static_cast<unsigned>(options.decodeThreads) : decodeThreads()),
      m_decoding(static_cast<int>(options.decoding))
{
    quietFfmpeg();
}

GpuCore::~GpuCore()
{
    const std::lock_guard guard(m_lock);
    dropDevice();
}

base::Result<void> GpuCore::start()
{
    const std::lock_guard guard(m_lock);
    return makeDevice();
}

base::Result<void> GpuCore::makeDevice()
{
    ComPtr<IDXGIAdapter1> chosen;
    if (m_options.adapter >= 0 && !m_options.software) {
        ComPtr<IDXGIFactory1> factory;
        if (const HRESULT made = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(made)) {
            return graphicsError("Asking Windows for its graphics adapters", made);
        }
        if (factory->EnumAdapters1(static_cast<UINT>(m_options.adapter), &chosen) != S_OK) {
            return base::Error{"no_adapter", "This PC has no graphics adapter with that number."};
        }
    } else if (!m_options.software) {
        chosen = adapterOf(m_options.choice);
    }

    // A named adapter is asked for by itself; without one, Windows chooses, or its software adapter is taken.
    const D3D_DRIVER_TYPE driver = chosen != nullptr    ? D3D_DRIVER_TYPE_UNKNOWN
                                   : m_options.software ? D3D_DRIVER_TYPE_WARP
                                                        : D3D_DRIVER_TYPE_HARDWARE;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (m_options.checked) {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    // Video is asked for first, and done without where the driver has none: the wall still draws,
    // and everything is decoded in software.
    HRESULT made = E_FAIL;
    for (const UINT video : {static_cast<UINT>(D3D11_CREATE_DEVICE_VIDEO_SUPPORT), 0U}) {
        if (video != 0 && m_options.software) {
            continue;
        }
        made =
            D3D11CreateDevice(chosen.Get(), driver, nullptr, flags | video, levels,
                              static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
        if (SUCCEEDED(made)) {
            break;
        }
    }
    if (FAILED(made) && chosen != nullptr && m_options.adapter < 0) {
        // The adapter the choice came to could not: the one Windows gives, rather than no pictures.
        made = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &m_device, nullptr,
                                 &m_context);
        if (FAILED(made)) {
            made = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                     static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &m_device, nullptr,
                                     &m_context);
        }
    }
    if (FAILED(made)) {
        return graphicsError("Making the Direct3D device", made);
    }

    // FFmpeg's decoding threads and the drawing thread share the one context.
    if (ComPtr<ID3D10Multithread> threads; SUCCEEDED(m_context.As(&threads))) {
        threads->SetMultithreadProtected(TRUE);
    }
    ComPtr<IDXGIDevice1> dxgi;
    if (const HRESULT asked = m_device.As(&dxgi); FAILED(asked)) {
        return graphicsError("Asking the device for its adapter", asked);
    }
    // Never more than one finished picture waiting for the monitor.
    dxgi->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    if (const HRESULT asked = dxgi->GetAdapter(&adapter); FAILED(asked)) {
        return graphicsError("Asking the device for its adapter", asked);
    }
    adapter.As(&m_adapter);
    if (const HRESULT asked = adapter->GetParent(IID_PPV_ARGS(&m_factory)); FAILED(asked)) {
        return graphicsError("Asking the adapter for its factory", asked);
    }
    if (m_adapter != nullptr) {
        DXGI_ADAPTER_DESC1 description{};
        m_adapter->GetDesc1(&description);
        m_adapterName = narrow(description.Description);
    }
    m_tearingAllowed = false;
    if (ComPtr<IDXGIFactory5> newer; SUCCEEDED(m_factory.As(&newer))) {
        BOOL allowed = FALSE;
        if (SUCCEEDED(newer->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof(allowed)))) {
            m_tearingAllowed = allowed == TRUE;
        }
    }

    m_decodesH264 = hasH264Decoder(m_device.Get());
    m_decodingDevice.reset();
    if (m_decodesH264) {
        BufferPtr reference(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
        if (reference != nullptr) {
            auto* general = reinterpret_cast<AVHWDeviceContext*>(reference->data);
            auto* ours = static_cast<AVD3D11VADeviceContext*>(general->hwctx);
            // FFmpeg lets go of both when it is done with them, so it is given references of its own.
            ours->device = m_device.Get();
            ours->device->AddRef();
            ours->device_context = m_context.Get();
            ours->device_context->AddRef();
            ours->lock = lockDevice;
            ours->unlock = unlockDevice;
            ours->lock_ctx = this;
            // A decoded picture is drawn straight from the decoder's texture: it never leaves the
            // card and is never copied (DESIGN.md section 3.2).
            ours->BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (av_hwdevice_ctx_init(reference.get()) >= 0) {
                m_decodingDevice = std::move(reference);
            }
        }
        m_decodesH264 = m_decodingDevice != nullptr;
    }
    return {};
}

void GpuCore::dropDevice()
{
    m_decodingDevice.reset();
    if (m_context != nullptr) {
        // What was let go is only gone once the context has been told to forget it: a window's
        // chain of buffers has to be gone before the next device may make one for that window.
        m_context->ClearState();
        m_context->Flush();
    }
    m_context.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();
}

bool GpuCore::ready()
{
    if (!lost()) {
        const std::lock_guard guard(m_lock);
        // A device that Windows took away says so when asked, before any call has failed.
        if (m_device != nullptr && FAILED(m_device->GetDeviceRemovedReason())) {
            noteLost();
        } else {
            return m_device != nullptr;
        }
    }

    // Decoders on the card first, and without the device's lock: a decode that is under way may be
    // waiting for that lock, and is waited for here.
    {
        const std::lock_guard holders(m_holdersLock);
        for (CardDecoderHolder* holder : m_holders) {
            holder->dropCardDecoder();
        }
    }

    const std::lock_guard guard(m_lock);
    if (!lost()) {
        return m_device != nullptr; // another window's drawing thread has done it meanwhile
    }
    if (m_device != nullptr) {
        tell(Fault::DeviceLost);
        for (DeviceDependent* dependent : m_dependents) {
            dependent->dropDeviceObjects();
        }
        dropDevice();
    }
    if (!makeDevice()) {
        dropDevice();
        return false; // still gone; whoever draws tries again
    }
    for (DeviceDependent* dependent : m_dependents) {
        dependent->makeDeviceObjects();
    }
    m_lost.store(false, std::memory_order_release);
    m_recovered.fetch_add(1, std::memory_order_relaxed);
    tell(Fault::DeviceRestored);
    return true;
}

void GpuCore::tell(Fault fault)
{
    for (const Host* host : m_hosts) {
        if (host->fault) {
            host->fault(fault);
        }
    }
}

std::string GpuCore::adapterName() const
{
    const std::lock_guard guard(m_lock);
    return m_adapterName;
}

bool GpuCore::decodesH264() const
{
    return m_decodesH264.load(std::memory_order_acquire);
}

bool GpuCore::mayMakeCardDecoder() const noexcept
{
    return !lost() && m_decodesH264.load(std::memory_order_acquire) &&
           m_cardDecoders.load(std::memory_order_acquire) < m_options.cardDecoders;
}

void GpuCore::setDecoding(DecodingChoice decoding)
{
    if (m_decoding.exchange(static_cast<int>(decoding), std::memory_order_acq_rel) != static_cast<int>(decoding)) {
        m_decodingChanges.fetch_add(1, std::memory_order_acq_rel);
    }
}

DecodingChoice GpuCore::decoding() const
{
    return decodingChoice();
}

void* GpuCore::nativeDevice() const
{
    const std::lock_guard guard(m_lock);
    return m_device.Get();
}

void* GpuCore::nativeContext() const
{
    const std::lock_guard guard(m_lock);
    return m_context.Get();
}

Gpu::Memory GpuCore::memory() const
{
    const std::lock_guard guard(m_lock);
    Memory memory;
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (m_adapter != nullptr && SUCCEEDED(m_adapter.As(&adapter)) &&
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        memory.usedBytes = info.CurrentUsage;
        memory.budgetBytes = info.Budget;
    }
    return memory;
}

void GpuCore::loseDevice()
{
    noteLost();
}

std::uint64_t GpuCore::timesRecovered() const
{
    return m_recovered.load(std::memory_order_relaxed);
}

std::vector<std::string> GpuCore::objections()
{
    const std::lock_guard guard(m_lock);
    std::vector<std::string> found;
    ComPtr<ID3D11InfoQueue> queue;
    if (m_device == nullptr || FAILED(m_device.As(&queue))) {
        return found;
    }
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T length = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &length)) || length == 0) {
            continue;
        }
        std::vector<std::byte> room(length);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(room.data());
        if (FAILED(queue->GetMessage(index, message, &length))) {
            continue;
        }
        // What Direct3D only remarks on (a state set twice, an object made) is not an objection.
        if (message->Severity == D3D11_MESSAGE_SEVERITY_INFO || message->Severity == D3D11_MESSAGE_SEVERITY_MESSAGE) {
            continue;
        }
        found.emplace_back(message->pDescription,
                           message->DescriptionByteLength > 0 ? message->DescriptionByteLength - 1 : 0);
    }
    queue->ClearStoredMessages();
    return found;
}

BufferPtr GpuCore::decodingDevice()
{
    if (lost()) {
        return nullptr;
    }
    const std::lock_guard guard(m_lock);
    return BufferPtr(m_decodingDevice != nullptr ? av_buffer_ref(m_decodingDevice.get()) : nullptr);
}

void GpuCore::attach(DeviceDependent* dependent)
{
    const std::lock_guard guard(m_lock);
    m_dependents.push_back(dependent);
}

void GpuCore::detach(DeviceDependent* dependent)
{
    const std::lock_guard guard(m_lock);
    std::erase(m_dependents, dependent);
}

void GpuCore::attach(CardDecoderHolder* holder)
{
    const std::lock_guard guard(m_holdersLock);
    m_holders.push_back(holder);
}

void GpuCore::detach(CardDecoderHolder* holder)
{
    const std::lock_guard guard(m_holdersLock);
    std::erase(m_holders, holder);
}

void GpuCore::attach(const Host* host)
{
    const std::lock_guard guard(m_lock);
    m_hosts.push_back(host);
}

void GpuCore::detach(const Host* host)
{
    const std::lock_guard guard(m_lock);
    std::erase(m_hosts, host);
}

bool GpuCore::takeCardDecoder() noexcept
{
    int now = m_cardDecoders.load(std::memory_order_relaxed);
    while (now < m_options.cardDecoders) {
        if (m_cardDecoders.compare_exchange_weak(now, now + 1, std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

void GpuCore::giveBackCardDecoder() noexcept
{
    m_cardDecoders.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace qkvm::engine
