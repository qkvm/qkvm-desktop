#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include <qkvm/base/clock.h>

#include "gpu_core.h"
#include "stream_core.h"

// The shaders, compiled when the engine is built (engine/shaders).
#include "card.ps.h"
#include "fill.ps.h"
#include "planar.ps.h"
#include "quad.vs.h"

namespace qkvm::engine {

namespace {

// How long one draw may spend copying software pictures to the card. What does not fit waits for
// the next draw, with its older picture drawn meanwhile: one large picture must not hold up all.
constexpr std::int64_t copyingMayTakeUs = 4000;

// What every shader of a tile is told about that tile: struct Tile in engine/shaders/tile.hlsli.
struct TileConstants {
    float place[4];
    float crop[4];
    float toRed[4];
    float toGreen[4];
    float toBlue[4];
    float fill[4];
};

class SceneCore final : public Scene, private DeviceDependent {
public:
    SceneCore(GpuCore& gpu, Host host) : m_gpu(gpu), m_host(std::move(host))
    {
        m_gpu.attach(static_cast<DeviceDependent*>(this));
        m_gpu.attach(&m_host);
        const std::lock_guard guard(m_gpu.lock());
        makeDeviceObjects();
    }

    ~SceneCore() override
    {
        // What it took away and the pool still decodes may tell its host until it ends: waited for.
        {
            std::unique_lock guard(m_retiring->lock);
            m_retiring->none.wait(guard, [this] { return m_retiring->count == 0; });
        }
        // After this no recovery calls into the scene, and its streams are nobody's but its own. They
        // end with the scene's other members, without the device's lock: each waits for its decoding.
        m_gpu.detach(&m_host);
        m_gpu.detach(static_cast<DeviceDependent*>(this));
    }

    Stream& add(StreamId id, Codec codec) override
    {
        auto stream = std::make_unique<StreamCore>(m_gpu, m_host, codec);
        StreamCore& made = *stream;
        std::unique_ptr<StreamCore> replaced;
        {
            const std::lock_guard guard(m_gpu.lock());
            replaced = std::exchange(m_streams[id], std::move(stream));
            std::erase(m_drawnNow, replaced.get());
            m_changed = true;
        }
        retire(std::move(replaced));
        return made;
    }

    void remove(StreamId id) override
    {
        std::unique_ptr<StreamCore> gone;
        {
            const std::lock_guard guard(m_gpu.lock());
            if (const auto found = m_streams.find(id); found != m_streams.end()) {
                gone = std::move(found->second);
                m_streams.erase(found);
                std::erase(m_drawnNow, gone.get());
            }
            m_changed = true;
        }
        retire(std::move(gone));
    }

    void place(std::span<const Placed> placed) override
    {
        const std::lock_guard guard(m_gpu.lock());
        m_placed.assign(placed.begin(), placed.end());
        m_changed = true;
    }

    bool draw(const Target& target) override
    {
        if (!m_gpu.ready()) {
            return false;
        }
        const std::lock_guard guard(m_gpu.lock());
        ID3D11DeviceContext* context = m_gpu.context();
        auto* view = static_cast<ID3D11RenderTargetView*>(target.renderTargetView);
        if (context == nullptr || view == nullptr || target.size.width <= 0 || target.size.height <= 0 ||
            m_quad == nullptr) {
            return false;
        }
        if (target.made != m_gpu.timesRecovered()) {
            // It was asked for before the device was lost and made anew, here or in another window's
            // draw: it is gone. The host asks for the target again and draws again.
            m_changed = true;
            if (m_host.somethingNew) {
                m_host.somethingNew();
            }
            return false;
        }

        bool something = m_changed || target.size != m_lastSize || target.renderTargetView != m_lastView;
        for (const Placed& placed : m_placed) {
            const auto found = m_streams.find(placed.stream);
            something = something || (found != m_streams.end() && found->second->hasNewer());
        }
        if (!something) {
            return false;
        }
        m_changed = false;
        m_lastSize = target.size;
        m_lastView = target.renderTargetView;

        const D3D11_VIEWPORT whole{0, 0, static_cast<float>(target.size.width), static_cast<float>(target.size.height),
                                   0, 1};
        const float black[4] = {0, 0, 0, 1};
        context->OMSetRenderTargets(1, &view, nullptr);
        context->RSSetViewports(1, &whole);
        context->ClearRenderTargetView(view, black);
        drawTiles(context, target.size);
        return true;
    }

    void drawUnder(SizePx size) override
    {
        // The host is in the middle of its own frame with this device: a device that is gone is
        // not made anew from in here. The host hears of it and draws again.
        if (m_gpu.lost()) {
            return;
        }
        const std::lock_guard guard(m_gpu.lock());
        ID3D11DeviceContext* context = m_gpu.context();
        if (context == nullptr || size.width <= 0 || size.height <= 0 || m_quad == nullptr) {
            return;
        }
        const D3D11_VIEWPORT whole{0, 0, static_cast<float>(size.width), static_cast<float>(size.height), 0, 1};
        context->RSSetViewports(1, &whole);
        m_changed = false;
        drawTiles(context, size);
    }

    void shown(std::int64_t atUs) override
    {
        const std::lock_guard guard(m_gpu.lock());
        for (StreamCore* stream : m_drawnNow) {
            stream->noteShown(atUs);
        }
        // (A wall has a few dozen streams at the most: looked for in the frame's list, one by one.)
        for (const auto& [id, stream] : m_streams) {
            if (std::ranges::find(m_drawnNow, stream.get()) == m_drawnNow.end()) {
                stream->noteNotShown();
            }
        }
        m_drawnNow.clear();
    }

    base::Result<Screenshot> screenshot(StreamId id) override
    {
        const std::lock_guard guard(m_gpu.lock());
        const auto found = m_streams.find(id);
        if (m_gpu.lost() || m_gpu.context() == nullptr || m_quad == nullptr || found == m_streams.end()) {
            return base::Error{"no_picture", "There is no displayed picture to copy."};
        }
        const Drawable picture = found->second->drawable();
        const SizePx size = picture.picture;
        if (picture.planes == 0 || size.width <= 0 || size.height <= 0 || size.width > maxPictureWidth ||
            size.height > maxPictureHeight) {
            return base::Error{"no_picture", "There is no displayed picture to copy."};
        }
        auto canvas = m_gpu.makeCanvas(size);
        if (!canvas) {
            return canvas.error();
        }
        auto* context = m_gpu.context();
        ComPtr<ID3D11RenderTargetView> oldTarget;
        ComPtr<ID3D11DepthStencilView> oldDepth;
        context->OMGetRenderTargets(1, &oldTarget, &oldDepth);
        D3D11_VIEWPORT oldViews[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT oldCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context->RSGetViewports(&oldCount, oldViews);
        auto* target = static_cast<ID3D11RenderTargetView*>(canvas.value()->target().renderTargetView);
        const D3D11_VIEWPORT whole{0, 0, static_cast<float>(size.width), static_cast<float>(size.height), 0, 1};
        context->OMSetRenderTargets(1, &target, nullptr);
        context->RSSetViewports(1, &whole);
        prepareDrawing(context);
        const bool drawn = drawTile(context, size, {0, 0, size.width, size.height}, picture);
        ID3D11ShaderResourceView* const none[3] = {};
        context->PSSetShaderResources(0, 3, none);
        context->OMSetRenderTargets(1, oldTarget.GetAddressOf(), oldDepth.Get());
        context->RSSetViewports(oldCount, oldViews);
        if (!drawn) {
            return base::Error{"screenshot", "The displayed picture could not be copied."};
        }
        auto pixels = canvas.value()->pixels();
        if (!pixels) {
            return pixels.error();
        }
        return Screenshot{size, std::move(pixels).value()};
    }

private:
    // The rectangle `inside` in the clip space of a target of this size: left, top, right, bottom.
    static void placeOf(const SizePx& target, float x, float y, float width, float height, float place[4])
    {
        place[0] = x / static_cast<float>(target.width) * 2 - 1;
        place[1] = 1 - y / static_cast<float>(target.height) * 2;
        place[2] = (x + width) / static_cast<float>(target.width) * 2 - 1;
        place[3] = 1 - (y + height) / static_cast<float>(target.height) * 2;
    }

    void prepareDrawing(ID3D11DeviceContext* context)
    {
        // Everything is set anew at each draw: nothing is assumed of what the context was left with.
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(m_quad.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
        context->PSSetSamplers(0, 1, m_smooth.GetAddressOf());
        context->RSSetState(m_solid.Get());
        context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(m_flat.Get(), 0);
    }

    // Draws every placed stream into what is bound. With the device's lock held.
    void drawTiles(ID3D11DeviceContext* context, const SizePx& size)
    {
        prepareDrawing(context);
        m_drawnNow.clear();
        const std::int64_t until = base::steadyNowUs() + copyingMayTakeUs;
        for (const Placed& placed : m_placed) {
            const auto found = m_streams.find(placed.stream);
            if (found == m_streams.end() || placed.where.width <= 0 || placed.where.height <= 0) {
                continue;
            }
            StreamCore& stream = *found->second;
            if (stream.takeNewer(until)) {
                // Left for the next draw; the host is asked for one, at once where the stream's
                // pictures are wanted so.
                m_changed = true;
                if (stream.urgent() && m_host.somethingUrgent) {
                    m_host.somethingUrgent();
                } else if (m_host.somethingNew) {
                    m_host.somethingNew();
                }
            }
            // A picture larger than the view it is shown in is drawn only inside that (b10): what of
            // its place lies outside is left as it was, under the bars around the view.
            const bool clipped = placed.clip.width > 0 && placed.clip.height > 0;
            if (clipped) {
                const D3D11_RECT inside{placed.clip.x, placed.clip.y, placed.clip.x + placed.clip.width,
                                        placed.clip.y + placed.clip.height};
                context->RSSetState(m_clipped.Get());
                context->RSSetScissorRects(1, &inside);
            }
            drawTile(context, size, placed.where, stream.drawable());
            if (clipped) {
                context->RSSetState(m_solid.Get());
            }
            m_drawnNow.push_back(&stream);
        }

        // Nothing of a stream stays bound: a decoder's texture is written to by the decoder.
        ID3D11ShaderResourceView* const none[3] = {};
        context->PSSetShaderResources(0, 3, none);
    }

    bool drawQuad(ID3D11DeviceContext* context, ID3D11PixelShader* shader, const TileConstants& constants)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (const HRESULT got = context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped); FAILED(got)) {
            if (meansDeviceLost(got)) {
                m_gpu.noteLost();
            }
            return false;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(m_constants.Get(), 0);
        context->PSSetShader(shader, nullptr, 0);
        context->Draw(4, 0);
        return true;
    }

    bool drawTile(ID3D11DeviceContext* context, const SizePx& target, const RectPx& where, const Drawable& drawable)
    {
        const auto tileX = static_cast<float>(where.x);
        const auto tileY = static_cast<float>(where.y);
        const auto tileWidth = static_cast<float>(where.width);
        const auto tileHeight = static_cast<float>(where.height);

        // The picture keeps its shape: as large as fits, in the middle, on whole pixels.
        float width = 0;
        float height = 0;
        if (drawable.planes != 0 && drawable.picture.width > 0 && drawable.picture.height > 0) {
            const float scale = std::min(tileWidth / static_cast<float>(drawable.picture.width),
                                         tileHeight / static_cast<float>(drawable.picture.height));
            width = std::min(tileWidth, std::round(static_cast<float>(drawable.picture.width) * scale));
            height = std::min(tileHeight, std::round(static_cast<float>(drawable.picture.height) * scale));
        }

        TileConstants constants{};
        constants.fill[3] = 1;
        if (width < tileWidth || height < tileHeight) {
            // What the picture leaves free of its tile is black, and so is a tile without one.
            placeOf(target, tileX, tileY, tileWidth, tileHeight, constants.place);
            if (!drawQuad(context, m_fill.Get(), constants)) {
                return false;
            }
        }
        if (width <= 0 || height <= 0) {
            return true;
        }
        placeOf(target, tileX + std::floor((tileWidth - width) / 2), tileY + std::floor((tileHeight - height) / 2),
                width, height, constants.place);
        constants.crop[2] = drawable.cropRight;
        constants.crop[3] = drawable.cropBottom;
        std::memcpy(constants.toRed, drawable.colours.toRed, sizeof(constants.toRed));
        std::memcpy(constants.toGreen, drawable.colours.toGreen, sizeof(constants.toGreen));
        std::memcpy(constants.toBlue, drawable.colours.toBlue, sizeof(constants.toBlue));
        ID3D11ShaderResourceView* const views[3] = {drawable.plane[0].Get(), drawable.plane[1].Get(),
                                                    drawable.plane[2].Get()};
        context->PSSetShaderResources(0, static_cast<UINT>(drawable.planes), views);
        return drawQuad(context, drawable.planes == 2 ? m_card.Get() : m_planar.Get(), constants);
    }

    void dropDeviceObjects() override
    {
        m_quad.Reset();
        m_planar.Reset();
        m_card.Reset();
        m_fill.Reset();
        m_constants.Reset();
        m_smooth.Reset();
        m_solid.Reset();
        m_clipped.Reset();
        m_flat.Reset();
        m_lastView = nullptr;
        m_drawnNow.clear();
        for (const auto& [id, stream] : m_streams) {
            stream->dropDrawObjects();
        }
    }

    void makeDeviceObjects() override
    {
        ID3D11Device* device = m_gpu.device();
        if (device == nullptr) {
            return;
        }
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(TileConstants);
        constants.Usage = D3D11_USAGE_DYNAMIC;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        // Between the pixels of a picture that is drawn larger or smaller than it is, the values in between.
        D3D11_SAMPLER_DESC smooth{};
        smooth.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smooth.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        smooth.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        smooth.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smooth.MaxLOD = D3D11_FLOAT32_MAX;

        D3D11_RASTERIZER_DESC solid{};
        solid.FillMode = D3D11_FILL_SOLID;
        solid.CullMode = D3D11_CULL_NONE;
        solid.DepthClipEnable = TRUE;
        // The same, held to a rectangle: a picture larger than the view it is shown in (Placed::clip).
        D3D11_RASTERIZER_DESC clipped = solid;
        clipped.ScissorEnable = TRUE;

        // No depth: a wall is flat, and what is drawn later lies on top.
        D3D11_DEPTH_STENCIL_DESC flat{};
        flat.DepthEnable = FALSE;
        flat.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        flat.DepthFunc = D3D11_COMPARISON_ALWAYS;
        flat.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        flat.BackFace = flat.FrontFace;

        const bool made = SUCCEEDED(device->CreateVertexShader(g_quadVs, sizeof(g_quadVs), nullptr, &m_quad)) &&
                          SUCCEEDED(device->CreatePixelShader(g_planarPs, sizeof(g_planarPs), nullptr, &m_planar)) &&
                          SUCCEEDED(device->CreatePixelShader(g_cardPs, sizeof(g_cardPs), nullptr, &m_card)) &&
                          SUCCEEDED(device->CreatePixelShader(g_fillPs, sizeof(g_fillPs), nullptr, &m_fill)) &&
                          SUCCEEDED(device->CreateBuffer(&constants, nullptr, &m_constants)) &&
                          SUCCEEDED(device->CreateSamplerState(&smooth, &m_smooth)) &&
                          SUCCEEDED(device->CreateRasterizerState(&solid, &m_solid)) &&
                          SUCCEEDED(device->CreateRasterizerState(&clipped, &m_clipped)) &&
                          SUCCEEDED(device->CreateDepthStencilState(&flat, &m_flat));
        if (!made) {
            // Without all of them nothing is drawn; draw() sees that the first one is missing.
            m_quad.Reset();
        }
        m_changed = true;
    }

    // A stream taken away while its decoding may be under way is ended by the pool once that is over
    // (2c's reading, L4: taking it away waited for it, up to 45 ms of the interface thread); the scene
    // waits for those it took away as it ends itself, since until then they may still tell its host.
    struct Retiring {
        std::mutex lock;
        std::condition_variable none;
        int count = 0;
    };
    void retire(std::unique_ptr<StreamCore> stream)
    {
        if (stream == nullptr) {
            return;
        }
        {
            const std::lock_guard guard(m_retiring->lock);
            ++m_retiring->count;
        }
        stream->retire();
        StreamCore* const work = stream.get();
        m_gpu.decodePool().retire(
            work, [going = std::shared_ptr<StreamCore>(std::move(stream)), retiring = m_retiring]() mutable {
                going.reset();
                {
                    const std::lock_guard guard(retiring->lock);
                    --retiring->count;
                }
                retiring->none.notify_all();
            });
    }

    GpuCore& m_gpu;
    const Host m_host;
    const std::shared_ptr<Retiring> m_retiring = std::make_shared<Retiring>();

    // All of this with the device's lock held.
    std::map<StreamId, std::unique_ptr<StreamCore>> m_streams;
    std::vector<Placed> m_placed;
    std::vector<StreamCore*> m_drawnNow;
    bool m_changed = true;
    SizePx m_lastSize;
    void* m_lastView = nullptr;
    ComPtr<ID3D11VertexShader> m_quad;
    ComPtr<ID3D11PixelShader> m_planar;
    ComPtr<ID3D11PixelShader> m_card;
    ComPtr<ID3D11PixelShader> m_fill;
    ComPtr<ID3D11Buffer> m_constants;
    ComPtr<ID3D11SamplerState> m_smooth;
    ComPtr<ID3D11RasterizerState> m_solid;
    ComPtr<ID3D11RasterizerState> m_clipped;
    ComPtr<ID3D11DepthStencilState> m_flat;
};

} // namespace

std::unique_ptr<Scene> GpuCore::makeScene(Host host)
{
    return std::make_unique<SceneCore>(*this, std::move(host));
}

} // namespace qkvm::engine
