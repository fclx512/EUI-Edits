#include "core/render/d2d/d2d_backend.h"
#include "core/window/window_backend.h"
#include "core/platform/platform.h"
#include "core/render/d2d/cache_capacity.h"
#include "core/render/d2d/atlas_capacity.h"
#include "core/render/d2d/render_policy.h"
#include "core/render/resize_trace.h"
#include "core/render/gpu_resize_stats.h"
#if defined(EUI_WINDOW_BACKEND_WIN32)
#include "core/window/win32_host.h"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace core::render::d2d {
namespace {
using Microsoft::WRL::ComPtr;
D2D1_RECT_F box(const Rect& r) { return D2D1::RectF(r.x, r.y, r.x + r.width, r.y + r.height); }
D2D1_COLOR_F color(const Color& c, float opacity = 1.0f) {
    return D2D1::ColorF(c.r, c.g, c.b, std::clamp(c.a * opacity, 0.0f, 1.0f));
}
float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float roundedDistance(float x, float y, float w, float h, float r) {
    x = std::fabs(x - w * 0.5f) - w * 0.5f + r;
    y = std::fabs(y - h * 0.5f) - h * 0.5f + r;
    return std::hypot(std::max(x, 0.0f), std::max(y, 0.0f)) + std::min(std::max(x, y), 0.0f) - r;
}
// UI commands carry screen and local vertices. Recover the affine transform,
// keeping local geometry/gradient coordinates independent of scrolling/scale.
D2D1_MATRIX_3X2_F transform(const std::vector<PrimitiveGeometryVertex>& v) {
    if (v.size() < 6) return D2D1::Matrix3x2F::Identity();
    const auto& a = v[0]; const auto& b = v[1]; const auto& c = v[5];
    const float w = b.local.x - a.local.x, h = c.local.y - a.local.y;
    if (std::fabs(w) < 0.0001f || std::fabs(h) < 0.0001f) return D2D1::Matrix3x2F::Identity();
    D2D1_MATRIX_3X2_F m{(b.screen.x - a.screen.x) / w, (b.screen.y - a.screen.y) / w,
                       (c.screen.x - a.screen.x) / h, (c.screen.y - a.screen.y) / h, 0, 0};
    m.dx = a.screen.x - a.local.x * m.m11 - a.local.y * m.m21;
    m.dy = a.screen.y - a.local.x * m.m12 - a.local.y * m.m22;
    return m;
}
void report(const char* where, HRESULT hr) {
    std::fprintf(stderr, "[D2D] %s failed: 0x%08lx\n", where, static_cast<unsigned long>(hr));
}
constexpr std::size_t kFrozenBudget = 64u * 1024u * 1024u;
constexpr std::size_t kFrozenPanelLimit = 4;
constexpr std::size_t kStripBudget = 4u * 1024u * 1024u;
bool pixelBytes(int width, int height, std::size_t& bytes) {
    if (width <= 0 || height <= 0) return false;
    const auto amount = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4u;
    if (amount > kFrozenBudget) return false;
    bytes = static_cast<std::size_t>(amount);
    return true;
}
double millisecondsSince(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}
bool frozenTraceEnabled() {
    static const bool enabled = [] {
        char value[8]{};
        return GetEnvironmentVariableA("NEO_FROZEN_TRACE", value, sizeof(value)) == 1 && value[0] == '1';
    }();
    return enabled;
}
}

struct D2DRenderBackend::Impl {
    struct Atlas { ComPtr<ID2D1Bitmap> bitmap; std::uint64_t generation = 0; int width = 0, height = 0, sourceHeight = 0; };
    struct Texture {
        ComPtr<ID2D1Bitmap> bitmap;
        std::vector<unsigned char> bgra; // Recovery source, never a second RGBA copy.
        int width = 0, height = 0;
        Color tint{1, 1, 1, 1};
    };
    struct ShadowMask {
        std::array<float, 8> key{};
        ComPtr<ID2D1Bitmap> bitmap;
        std::size_t bytes = 0;
        int width = 0, height = 0;
        float left = 0, top = 0;
    };
    struct RevealBrush {
        ID2D1RenderTarget* owner = nullptr;
        Color tint{};
        ComPtr<ID2D1RadialGradientBrush> brush;
        std::uint64_t lastUse = 0;
    };
    struct FrozenPanel {
        std::uint64_t id = 0;
        int x = 0, y = 0, width = 0, height = 0;
        int cropX = 0, cropY = 0, cropWidth = 0, cropHeight = 0;
        float sigma = 0.0f;
        ComPtr<ID2D1Bitmap> bitmap;
        // A bitmap brush fills the current rounded geometry directly; its
        // transform maps the panel's source crop onto the current local rect.
        ComPtr<ID2D1BitmapBrush> clipBrush;
        ID2D1RenderTarget* clipOwner = nullptr;
        std::uint64_t clipDeviceGeneration = 0;
        bool clipBrushFailed = false;
        std::size_t bytes = 0;
        std::uint64_t lastUse = 0;
    };
    struct FrozenScene {
        std::uint64_t session = 0, revision = 0, deviceGeneration = 0;
        int width = 0, height = 0;
        float dpi = 0.0f;
        bool pending = false, poisoned = false, invalidationPending = false;
        bool settling = false;
        int settleFrames = 0;
        ComPtr<ID2D1Bitmap> bitmap;
        std::size_t sceneBytes = 0, panelBytes = 0;
        std::uint64_t use = 0;
        std::vector<FrozenPanel> panels;
        std::unordered_set<std::uint64_t> failedPanels;
        void clearPixels() {
            panels.clear(); panelBytes = 0;
            failedPanels.clear();
            bitmap.Reset(); sceneBytes = 0; pending = false; settling = false;
        }
        void close() {
            clearPixels(); session = revision = deviceGeneration = 0;
            width = height = 0; dpi = 0.0f; poisoned = false; invalidationPending = false; use = 0; settleFrames = 0;
        }
        std::size_t residentBytes() const { return sceneBytes + panelBytes; }
    } frozen, strip;
    std::uint64_t deviceGeneration = 1;
    HWND hwnd = nullptr;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1RenderTarget> window;
    ComPtr<ID2D1HwndRenderTarget> hwndTarget;
    ComPtr<ID2D1DCRenderTarget> dcTarget;
    HDC windowDC = nullptr;
    bool gdiPresentation = false;
    ComPtr<ID2D1BitmapRenderTarget> cache;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Layer> imageClip;
    ID2D1RenderTarget* target = nullptr;
    Atlas gray, colored;
    std::unordered_set<Texture*> textures;
    std::vector<ShadowMask> shadows;
    std::array<RevealBrush, 4> revealBrushes{};
    std::uint64_t revealBrushUse = 0;
    std::size_t shadowBytes = 0;
    int width = 0, height = 0, cacheWidth = 0, cacheHeight = 0;
    CacheCapacity capacity;
    double cacheSizeChangedAt = 0;
    bool failed = false, presented = false, cacheInvalid = true;
    bool software = false, frame = false, cacheFrame = false, clipped = false, recreated = false;
    bool compactAtlases = true;
    bool warnedBackdrop = false, warnedImageEffects = false;

    ~Impl() { releaseDevice(); for (auto* texture : textures) delete texture; }
    void unclip() { if (target && clipped) target->PopAxisAlignedClip(); clipped = false; }
    void clearRevealBrushes() {
        for (auto& entry : revealBrushes) {
            entry.brush.Reset();
            entry.owner = nullptr;
            entry.lastUse = 0;
        }
    }
    void releaseDevice() {
        strip.clearPixels(); strip.poisoned = strip.session != 0;
        if (frozen.session) {
            frozen.invalidationPending = frozen.invalidationPending || frozen.bitmap.Get() != nullptr;
            frozen.clearPixels(); frozen.poisoned = true;
        }
        ++deviceGeneration;
        unclip(); target = nullptr; frame = cacheFrame = false;
        clearRevealBrushes();
        imageClip.Reset(); brush.Reset();
        if (cache) core::render::gpuResizeStats().addRenderCacheRelease();
        cache.Reset(); window.Reset(); hwndTarget.Reset(); dcTarget.Reset();
        if (windowDC) { ReleaseDC(hwnd, windowDC); windowDC = nullptr; }
        gray = {}; colored = {};
        cacheWidth = cacheHeight = 0; capacity = {}; cacheInvalid = true;
        shadows.clear(); shadowBytes = 0;
        for (auto* texture : textures) texture->bitmap.Reset();
    }
    bool createWindowTarget() {
        RECT r{}; GetClientRect(hwnd, &r);
        const auto props = D2D1::RenderTargetProperties(
            software ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        HRESULT hr;
        if (gdiPresentation) {
            // CPU rasterization to a native DC; EndDraw presents through GDI.
            hr = factory->CreateDCRenderTarget(&props, dcTarget.GetAddressOf());
            if (SUCCEEDED(hr)) {
                windowDC = GetDC(hwnd);
                if (!windowDC) hr = E_FAIL;
                else hr = dcTarget->BindDC(windowDC, &r);
            }
            if (SUCCEEDED(hr)) hr = dcTarget.As(&window);
        } else {
            hr = factory->CreateHwndRenderTarget(props,
                D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(std::max<LONG>(1, r.right), std::max<LONG>(1, r.bottom))),
                hwndTarget.GetAddressOf());
            if (SUCCEEDED(hr)) hr = hwndTarget.As(&window);
        }
        if (FAILED(hr)) {
            report("Create window render target", hr);
            releaseDevice();
            RenderPolicy fallback{software, gdiPresentation};
#if defined(EUI_WINDOW_BACKEND_WIN32)
            constexpr bool nativeWin32 = true;
#else
            constexpr bool nativeWin32 = false;
#endif
            if (fallback.useSoftwareFallback(nativeWin32)) {
                software = fallback.software;
                gdiPresentation = fallback.gdiPresentation;
                std::fprintf(stderr, "[D2D] hardware-preferred target unavailable; retrying software\n");
                return createWindowTarget();
            }
            return false;
        }
        hr = window->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush.GetAddressOf());
        if (FAILED(hr)) { report("CreateSolidColorBrush", hr); releaseDevice(); return false; }
        char trace[8]{};
        if (GetEnvironmentVariableA("NEO_D2D_TRACE", trace, sizeof(trace)) == 1 && trace[0] == '1') {
            std::fprintf(stderr, "[D2D] target=%s requested-raster=%s actual-adapter=unconfirmed\n",
                gdiPresentation ? "DC/GDI" : "Hwnd", software ? "SOFTWARE" : "DEFAULT");
        }
        return true;
    }
    bool check(HRESULT hr, const char* where) {
        if (SUCCEEDED(hr)) return true;
        failed = true; cacheInvalid = true;
        report(where, hr);
        if (hr == D2DERR_RECREATE_TARGET) {
            releaseDevice();
#if defined(EUI_WINDOW_BACKEND_WIN32)
            core::window::win32::notePresentationLost(hwnd);
#endif
            core::platform::requestUiUpdate();
            core::platform::requestFrame();
            core::window::postEmptyEvent();
        }
        return false;
    }
    ID2D1Brush* solid(const Color& c, float opacity = 1.0f) {
        brush->SetColor(color(c, opacity)); return brush.Get();
    }
    ID2D1RadialGradientBrush* radial(const Color& tint) {
        if (!target) return nullptr;
        for (auto& entry : revealBrushes) {
            if (entry.brush && entry.owner == target &&
                entry.tint.r == tint.r && entry.tint.g == tint.g && entry.tint.b == tint.b) {
                entry.lastUse = ++revealBrushUse;
                return entry.brush.Get();
            }
        }
        RevealBrush* entry = nullptr;
        for (auto& candidate : revealBrushes) {
            if (!candidate.brush) { entry = &candidate; break; }
        }
        if (!entry) {
            entry = &*std::min_element(revealBrushes.begin(), revealBrushes.end(),
                [](const RevealBrush& a, const RevealBrush& b) { return a.lastUse < b.lastUse; });
        }
        entry->brush.Reset();
        entry->owner = nullptr;
        const D2D1_COLOR_F full = color(Color{tint.r, tint.g, tint.b, 1.0f});
        const D2D1_GRADIENT_STOP stops[] = {
            {0.00f, full},
            {0.18f, color(Color{tint.r, tint.g, tint.b, 0.85f})},
            {0.46f, color(Color{tint.r, tint.g, tint.b, 0.45f})},
            {0.72f, color(Color{tint.r, tint.g, tint.b, 0.10f})},
            {1.00f, color(Color{tint.r, tint.g, tint.b, 0.00f})}
        };
        ComPtr<ID2D1GradientStopCollection> collection;
        HRESULT hr = target->CreateGradientStopCollection(
            stops, static_cast<UINT32>(sizeof(stops) / sizeof(stops[0])), D2D1_GAMMA_1_0,
            D2D1_EXTEND_MODE_CLAMP, collection.GetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = target->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                    D2D1::Point2F(0.0f, 0.0f), 1.0f, 1.0f),
                collection.Get(), entry->brush.GetAddressOf());
        }
        if (FAILED(hr)) { report("reveal radial brush", hr); return nullptr; }
        ++currentRenderFrameStats().revealBrushCreates;
        entry->owner = target;
        entry->tint = tint;
        entry->lastUse = ++revealBrushUse;
        return entry->brush.Get();
    }
    bool atlas(Atlas& a, const TextAtlasPageData& page) {
        if (!page.pixels || page.width <= 0 || page.height <= 0) return false;
        const int previousHeight = a.width == page.width && a.sourceHeight == page.height ? a.height : 0;
        const int uploadHeight = atlasUploadHeight(page.height, page.usedHeight, previousHeight, compactAtlases);
        if (a.bitmap && a.generation == page.generation && a.width == page.width && a.height == uploadHeight &&
            a.sourceHeight == page.height) return true;
        // text.cpp swaps the channels of FreeType's premultiplied BGRA;
        // swapping back must not multiply the alpha a second time.
        std::vector<unsigned char> converted;
        const unsigned char* pixels = page.pixels;
        if (page.channels == 4) {
            converted.resize(static_cast<std::size_t>(page.width) * uploadHeight * 4);
            for (std::size_t i = 0; i < converted.size(); i += 4) {
                converted[i] = pixels[i + 2];
                converted[i + 1] = pixels[i + 1];
                converted[i + 2] = pixels[i];
                converted[i + 3] = pixels[i + 3];
            }
            pixels = converted.data();
        } else if (page.channels != 1) return false;
        const UINT pitch = page.width * page.channels;
        HRESULT hr;
        if (!a.bitmap || a.width != page.width || a.height != uploadHeight) {
            a.bitmap.Reset();
            hr = window->CreateBitmap(D2D1::SizeU(page.width, uploadHeight), pixels, pitch,
                D2D1::BitmapProperties(D2D1::PixelFormat(page.channels == 1 ? DXGI_FORMAT_A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM,
                                                       D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), a.bitmap.GetAddressOf());
        } else hr = a.bitmap->CopyFromMemory(nullptr, pixels, pitch);
        if (!check(hr, "atlas upload")) return false;
        a.generation = page.generation; a.width = page.width; a.height = uploadHeight; a.sourceHeight = page.height;
        return true;
    }
    void mask(ID2D1Bitmap* bitmap, const D2D1_RECT_F& dest, const D2D1_RECT_F& source, const Color& c) {
        if (!target || !bitmap || !brush) return;
        const auto previous = target->GetAntialiasMode();
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        target->FillOpacityMask(bitmap, solid(c), D2D1_OPACITY_MASK_CONTENT_GRAPHICS, dest, source);
        target->SetAntialiasMode(previous);
    }
    void shadow(const RoundedRectDrawCommand& c) {
        const float radius = clampedPrimitiveRadius(c.radius, c.rect), blur = std::max(1.0f, c.shadowBlur);
        const std::array<float, 8> key{c.rect.width, c.rect.height, radius, blur,
            c.shadowOffset.x, c.shadowOffset.y, c.shadowSpread, c.insetShadowPass ? 1.0f : 0.0f};
        ShadowMask* entry = nullptr;
        for (auto& s : shadows) if (s.key == key) { entry = &s; break; }
        if (!entry) {
            const float extent = c.insetShadowPass ? 0.0f : std::ceil(blur + 1.0f);
            ShadowMask s; s.key = key; s.left = -extent; s.top = -extent;
            s.width = static_cast<int>(std::ceil(c.rect.width + 2 * extent));
            s.height = static_cast<int>(std::ceil(c.rect.height + 2 * extent));
            if (s.width <= 0 || s.height <= 0 || s.width > 8192 || s.height > 8192) return;
            s.bytes = static_cast<std::size_t>(s.width) * s.height;
            // A bounded mask cache; animated dimensions cannot accumulate indefinitely.
            constexpr std::size_t budget = 2u * 1024u * 1024u;
            if (s.bytes > budget) return;
            if (shadowBytes + s.bytes > budget || shadows.size() >= 32) { shadows.clear(); shadowBytes = 0; }
            std::vector<unsigned char> alpha(s.bytes);
            const float len = std::hypot(c.shadowOffset.x, c.shadowOffset.y);
            const float sx = len > 0.01f ? -c.shadowOffset.x / len : 0.0f;
            const float sy = len > 0.01f ? -c.shadowOffset.y / len : 1.0f;
            for (int y = 0; y < s.height; ++y) for (int x = 0; x < s.width; ++x) {
                const float px = x + 0.5f - extent, py = y + 0.5f - extent;
                const float d = roundedDistance(px, py, c.rect.width, c.rect.height, radius);
                float a = 1.0f - smooth(-blur, blur, d);
                if (c.insetShadowPass) {
                    const float side = std::clamp(0.34f + 0.66f * ((px - c.rect.width * .5f) / std::max(c.rect.width * .5f, 1.0f) * sx +
                        (py - c.rect.height * .5f) / std::max(c.rect.height * .5f, 1.0f) * sy), 0.0f, 1.0f);
                    a = smooth(-blur - std::max(c.shadowSpread, 0.0f), 0.0f, d) * side * (1 - smooth(-.75f, .75f, d));
                }
                alpha[static_cast<std::size_t>(y) * s.width + x] = static_cast<unsigned char>(std::lround(a * 255));
            }
            if (!check(window->CreateBitmap(D2D1::SizeU(s.width, s.height), alpha.data(), s.width,
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                    s.bitmap.GetAddressOf()), "shadow mask")) return;
            shadowBytes += s.bytes; shadows.push_back(std::move(s)); entry = &shadows.back();
        }
        Color shade = c.fillColor; shade.a *= c.opacity;
        mask(entry->bitmap.Get(), D2D1::RectF(c.rect.x + entry->left, c.rect.y + entry->top,
            c.rect.x + entry->left + entry->width, c.rect.y + entry->top + entry->height),
            D2D1::RectF(0, 0, static_cast<float>(entry->width), static_cast<float>(entry->height)), shade);
    }
};

D2DRenderBackend::D2DRenderBackend(core::window::Handle window) : impl_(std::make_unique<Impl>()) {
    impl_->hwnd = static_cast<HWND>(core::window::nativeWindowInfo(window).platformWindow);
    char software[8]{};
    const DWORD optionLength = GetEnvironmentVariableA("NEO_D2D_SOFTWARE", software, sizeof(software));
    char dcOption[8]{};
    const DWORD dcLength = GetEnvironmentVariableA("NEO_WIN32_DC", dcOption, sizeof(dcOption));
#if defined(EUI_WINDOW_BACKEND_WIN32)
    constexpr bool nativeWin32 = true;
#else
    constexpr bool nativeWin32 = false;
#endif
    const auto policy = RenderPolicy::fromOptions(nativeWin32,
        optionLength < sizeof(software) ? std::string_view(software, optionLength) : std::string_view{},
        dcLength < sizeof(dcOption) ? std::string_view(dcOption, dcLength) : std::string_view{});
    impl_->software = policy.software;
    impl_->gdiPresentation = policy.gdiPresentation;
    char compact[8]{};
    const DWORD compactLength = GetEnvironmentVariableA("NEO_D2D_COMPACT_ATLAS", compact, sizeof(compact));
    impl_->compactAtlases = !(compactLength == 1 && compact[0] == '0');
}
D2DRenderBackend::~D2DRenderBackend() = default;
bool D2DRenderBackend::initialize() {
    auto& p = *impl_;
    if (!p.hwnd || FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, p.factory.GetAddressOf()))) return false;
    return p.createWindowTarget();
}
bool D2DRenderBackend::valid() const { return impl_->window != nullptr; }
bool D2DRenderBackend::frameReady() const { return impl_->frame && !impl_->failed; }
bool D2DRenderBackend::framePresented() const { return impl_->presented; }
void D2DRenderBackend::beginFrame(const RenderSurface& s) {
    auto& p = *impl_;
    if (p.frame || s.framebufferWidth <= 0 || s.framebufferHeight <= 0) return;
    p.failed = false; p.presented = false;
    if (!p.window && !p.createWindowTarget()) return;
    const auto size = p.window->GetPixelSize();
    if (size.width != static_cast<UINT>(s.framebufferWidth) || size.height != static_cast<UINT>(s.framebufferHeight)) {
        const double started = resizeTraceStart();
        HRESULT hr;
        if (p.dcTarget) {
            RECT bounds{0, 0, s.framebufferWidth, s.framebufferHeight};
            hr = p.dcTarget->BindDC(p.windowDC, &bounds);
        } else hr = p.hwndTarget->Resize(D2D1::SizeU(s.framebufferWidth, s.framebufferHeight));
        resizeTraceEnd("bind", started, s.framebufferWidth, s.framebufferHeight);
        if (!p.check(hr, "Resize/BindDC")) return;
    }
    p.width = s.framebufferWidth; p.height = s.framebufferHeight;
    p.window->BeginDraw(); p.target = p.window.Get(); p.frame = true;
    p.target->SetTransform(D2D1::Matrix3x2F::Identity());
}
void D2DRenderBackend::present() {
    auto& p = *impl_;
    if (!p.frame) return;
    if (p.cacheFrame) endRenderCacheFrame();
    if (!p.frame || !p.window) return;
    p.unclip();
    const double started = resizeTraceStart();
    const HRESULT hr = p.window->EndDraw(); p.frame = false; p.target = nullptr;
    resizeTraceEnd("end-draw", started, p.width, p.height);
    if (!p.check(hr, "present")) return;
    if (p.failed) return;
    p.presented = true;
#if defined(EUI_WINDOW_BACKEND_WIN32)
    core::window::win32::notePresented(p.hwnd, p.width, p.height);
#endif
    resizeTrace().record("present", 0, p.width, p.height);
    auto& stats = currentRenderFrameStats(); ++stats.backendPresents;
    stats.backendPresentPixels += static_cast<std::uint64_t>(p.width) * p.height;
}
bool D2DRenderBackend::ensureRenderCache(int width, int height) {
    auto& p = *impl_; p.recreated = false;
    if (!p.frame) return false;
    const double now = core::window::timeSeconds();
    const bool changed = p.cacheWidth != width || p.cacheHeight != height;
    if (changed) p.cacheSizeChangedAt = now;
    const bool shrink = p.capacity.oversized(width, height) && now - p.cacheSizeChangedAt >= 2.0;
    // Logical-size invalidation requests a full repaint, but is not an allocation.
    p.recreated = changed || p.cacheInvalid;
    if (!p.cache || !p.capacity.fits(width, height) || shrink) {
        if (p.frozen.session && p.frozen.bitmap) {
            p.frozen.invalidationPending = true;
            p.frozen.clearPixels();
            p.frozen.poisoned = true;
        }
        const auto next = CacheCapacity::forSize(width, height, shrink ? CacheCapacity{} : p.capacity,
                                                 static_cast<int>(p.window->GetMaximumBitmapSize()));
        if (!next.fits(width, height)) return false;
        ComPtr<ID2D1BitmapRenderTarget> replacement;
        const double started = resizeTraceStart();
        const HRESULT hr = p.window->CreateCompatibleRenderTarget(
            D2D1::SizeF(static_cast<float>(next.width), static_cast<float>(next.height)),
            replacement.GetAddressOf());
        resizeTraceEnd("cache-allocate", started, next.width, next.height);
        if (!p.check(hr, "render cache")) {
            // A surviving window target can still render a complete direct frame.
            if (p.window) p.failed = false;
            return false;
        }
        if (p.cache) core::render::gpuResizeStats().addRenderCacheRelease();
        p.clearRevealBrushes();
        p.cache = std::move(replacement); p.capacity = next;
        core::render::gpuResizeStats().addRenderCacheAllocation();
        p.recreated = true; p.cacheInvalid = true;
    }
    p.cacheWidth = width; p.cacheHeight = height;
    return true;
}
bool D2DRenderBackend::renderCacheWasRecreated() const { return impl_->recreated; }
void D2DRenderBackend::releaseRenderCache() {
    impl_->strip.clearPixels(); impl_->strip.poisoned = impl_->strip.session != 0;
    if (impl_->frozen.session) {
        impl_->frozen.invalidationPending = impl_->frozen.invalidationPending || impl_->frozen.bitmap.Get() != nullptr;
        impl_->frozen.clearPixels();
        impl_->frozen.poisoned = true;
    }
    impl_->clearRevealBrushes();
    if (impl_->cache) core::render::gpuResizeStats().addRenderCacheRelease();
    impl_->cache.Reset(); impl_->capacity = {}; impl_->cacheInvalid = true;
    impl_->cacheWidth = impl_->cacheHeight = 0;
}
void D2DRenderBackend::beginRenderCacheFrame(int, int, const std::vector<Rect>&) {
    auto& p = *impl_; if (!p.frame || !p.cache || p.cacheFrame) return;
    p.unclip(); p.cache->BeginDraw(); p.target = p.cache.Get(); p.cacheFrame = true;
    p.target->SetTransform(D2D1::Matrix3x2F::Identity());
}
void D2DRenderBackend::endRenderCacheFrame() {
    auto& p = *impl_; if (!p.cacheFrame) return;
    p.unclip(); const HRESULT hr = p.cache->EndDraw(); p.cacheFrame = false; p.target = p.window.Get();
    if (p.check(hr, "cache EndDraw")) p.cacheInvalid = false;
}
void D2DRenderBackend::blitRenderCache(int width, int height, RenderCacheBlitMode, const std::vector<Rect>&) {
    auto& p = *impl_; if (!p.target || !p.cache) return;
    p.unclip(); p.target->SetTransform(D2D1::Matrix3x2F::Identity());
    ComPtr<ID2D1Bitmap> bitmap;
    if (p.check(p.cache->GetBitmap(bitmap.GetAddressOf()), "cache bitmap"))
        p.target->DrawBitmap(bitmap.Get(), D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), 1,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
            D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)));
}
void D2DRenderBackend::clear(const Color& c) {
#if defined(EUI_WINDOW_BACKEND_WIN32)
    core::window::win32::setPaintBackground(impl_->hwnd, RGB(
        static_cast<BYTE>(std::clamp(c.r, 0.0f, 1.0f) * 255),
        static_cast<BYTE>(std::clamp(c.g, 0.0f, 1.0f) * 255),
        static_cast<BYTE>(std::clamp(c.b, 0.0f, 1.0f) * 255)));
#endif
    if (impl_->target) impl_->target->Clear(color(c));
}
void D2DRenderBackend::setScissor(bool enabled, const Rect& r, int) {
    auto& p = *impl_; if (!p.target) return;
    p.unclip(); p.target->SetTransform(D2D1::Matrix3x2F::Identity());
    if (enabled) { p.target->PushAxisAlignedClip(box(r), D2D1_ANTIALIAS_MODE_ALIASED); p.clipped = true; }
}
void D2DRenderBackend::prepareBackdropBlur(const Rect&, float, int, int) {
    if (!impl_->warnedBackdrop) {
        std::fprintf(stderr, "[D2D] experimental backend does not implement backdrop blur\n");
        impl_->warnedBackdrop = true;
    }
}
bool D2DRenderBackend::beginFrozenBackdropFrame(std::uint64_t session, std::uint64_t sceneRevision,
                                                int width, int height, float dpiScale) {
    auto& p = *impl_;
    auto& frozen = p.frozen;
    if (session == 0) {
        if (frozen.bitmap || frozen.invalidationPending) ++currentRenderFrameStats().frozenInvalidations;
        frozen.close();
        currentRenderFrameStats().frozenBytes = 0;
        return false;
    }
    if (frozen.session != session) {
        frozen.close();
        frozen.session = session;
        frozen.revision = sceneRevision;
        frozen.width = width;
        frozen.height = height;
        frozen.dpi = dpiScale;
        frozen.deviceGeneration = p.deviceGeneration;
        std::size_t bytes = 0;
        frozen.settling = pixelBytes(width, height, bytes) && p.frame && p.cache &&
            width <= static_cast<int>(p.window->GetMaximumBitmapSize()) &&
            height <= static_cast<int>(p.window->GetMaximumBitmapSize());
        frozen.poisoned = !frozen.settling;
        frozen.settleFrames = 1;
        currentRenderFrameStats().frozenCaptureDeferred = frozen.settling;
        currentRenderFrameStats().frozenBytes = 0;
        return false;
    }
    // Opening a menu can hand focus away from a text field on the next compose.
    // Observe two equal scene identities before the sole capture. This is a
    // bounded pre-capture phase, never a retry of an already captured image.
    if (frozen.settling && !frozen.poisoned) {
        const bool same = frozen.revision == sceneRevision && frozen.width == width &&
            frozen.height == height && frozen.dpi == dpiScale &&
            frozen.deviceGeneration == p.deviceGeneration;
        if (!same) {
            frozen.revision = sceneRevision;
            frozen.width = width; frozen.height = height; frozen.dpi = dpiScale;
            frozen.deviceGeneration = p.deviceGeneration;
            std::size_t bytes = 0;
            if (++frozen.settleFrames > 3 || !pixelBytes(width, height, bytes) || !p.frame || !p.cache ||
                width > static_cast<int>(p.window->GetMaximumBitmapSize()) ||
                height > static_cast<int>(p.window->GetMaximumBitmapSize())) {
                frozen.clearPixels(); frozen.poisoned = true;
                return false;
            }
            currentRenderFrameStats().frozenCaptureDeferred = true;
            return false;
        }
        frozen.settling = false;
        frozen.pending = true;
        return true;
    }
    // An opening is immutable. A changed scene, viewport, DPI, or device falls
    // back to the theme's opaque fill until a new menu opening supplies a new ID.
    if (frozen.invalidationPending) {
        ++currentRenderFrameStats().frozenInvalidations;
        frozen.invalidationPending = false;
    }
    if (frozen.revision != sceneRevision || frozen.width != width || frozen.height != height ||
        frozen.dpi != dpiScale || frozen.deviceGeneration != p.deviceGeneration) {
        if (!frozen.poisoned && frozen.bitmap) ++currentRenderFrameStats().frozenInvalidations;
        if (!frozen.poisoned && frozenTraceEnabled())
            std::fprintf(stderr, "[D2D frozen] invalidated session=%llu\n",
                static_cast<unsigned long long>(session));
        frozen.clearPixels();
        frozen.poisoned = true;
    }
    currentRenderFrameStats().frozenBytes = frozen.residentBytes();
    return frozen.pending && !frozen.poisoned;
}

void D2DRenderBackend::captureFrozenBackdropScene() {
    auto& p = *impl_;
    auto& frozen = p.frozen;
    if (!frozen.pending || frozen.poisoned) return;
    if (!p.cacheFrame || !p.target || !p.window) {
        frozen.clearPixels();
        frozen.poisoned = true;
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    p.unclip();
    p.target->SetTransform(D2D1::Matrix3x2F::Identity());
    const D2D1_PIXEL_FORMAT format = p.target->GetPixelFormat();
    ComPtr<ID2D1Bitmap> scene;
    HRESULT hr = p.window->CreateBitmap(D2D1::SizeU(frozen.width, frozen.height), nullptr, 0,
        D2D1::BitmapProperties(format, 96.0f, 96.0f), scene.GetAddressOf());
    if (SUCCEEDED(hr)) {
        const D2D1_POINT_2U destination{0, 0};
        const D2D1_RECT_U source{0, 0, static_cast<UINT32>(frozen.width), static_cast<UINT32>(frozen.height)};
        hr = scene->CopyFromRenderTarget(&destination, p.target, &source);
    }
    frozen.pending = false;
    if (FAILED(hr)) {
        if (frozenTraceEnabled())
            std::fprintf(stderr, "[D2D frozen] scene capture failed: 0x%08lx\n",
                static_cast<unsigned long>(hr));
        frozen.clearPixels();
        frozen.poisoned = true;
        return;
    }
    frozen.bitmap = std::move(scene);
    pixelBytes(frozen.width, frozen.height, frozen.sceneBytes);
    auto& stats = currentRenderFrameStats();
    ++stats.frozenSceneCaptures;
    stats.frozenCaptureMs += millisecondsSince(started);
    stats.frozenBytes = frozen.residentBytes();
    if (frozenTraceEnabled())
        std::fprintf(stderr, "[D2D frozen] scene captured %dx%d in %.3f ms\n",
            frozen.width, frozen.height, stats.frozenCaptureMs);
}

bool D2DRenderBackend::cachedStripNeedsValidation(std::uint64_t session) const {
    return session != impl_->strip.session || !impl_->strip.poisoned;
}

bool D2DRenderBackend::beginCachedStripFrame(std::uint64_t session, std::uint64_t revision,
                                            int width, int height, float dpi) {
    auto& p = *impl_;
    auto& strip = p.strip;
    auto& stats = currentRenderFrameStats();
    if (!session) {
        if (strip.bitmap) ++stats.stripInvalidations;
        strip.close();
        return false;
    }
    if (strip.session != session) {
        strip.close();
        strip.session = session; strip.revision = revision;
        strip.width = width; strip.height = height; strip.dpi = dpi;
        strip.deviceGeneration = p.deviceGeneration;
        strip.settling = p.frame && p.cache;
        strip.poisoned = !strip.settling;
        strip.settleFrames = 1;
        stats.stripCaptureDeferred = strip.settling;
        return false;
    }
    const bool same = strip.revision == revision && strip.width == width && strip.height == height &&
        strip.dpi == dpi && strip.deviceGeneration == p.deviceGeneration;
    if (strip.settling && !strip.poisoned) {
        if (!same) {
            strip.revision = revision; strip.width = width; strip.height = height; strip.dpi = dpi;
            strip.deviceGeneration = p.deviceGeneration;
            if (++strip.settleFrames > 3) { strip.clearPixels(); strip.poisoned = true; return false; }
            stats.stripCaptureDeferred = true;
            return false;
        }
        strip.settling = false;
        strip.pending = true;
    } else if (!same && !strip.poisoned) {
        if (strip.bitmap) ++stats.stripInvalidations;
        strip.clearPixels(); strip.poisoned = true;
        if (frozenTraceEnabled()) std::fprintf(stderr, "[D2D strip] invalidated session=%llu\n",
            static_cast<unsigned long long>(session));
    }
    stats.stripBytes = strip.residentBytes();
    return strip.pending && !strip.poisoned;
}

void D2DRenderBackend::captureCachedStrip(const core::FrozenBackdrop& material) {
    auto& p = *impl_;
    auto& strip = p.strip;
    auto& stats = currentRenderFrameStats();
    if (!material.session || material.session != strip.session || strip.poisoned) return;
    if (strip.bitmap) return;
    // A popup must never become the source of persistent chrome. Block this
    // activation rather than retrying capture on every hover/animation frame.
    if (!material.allowCapture) {
        strip.clearPixels(); strip.poisoned = true; stats.stripCaptureDeferred = false;
        return;
    }
    if (!strip.pending) return;
    if (!stats.fullPaint) { stats.stripCaptureDeferred = true; return; }
    strip.pending = false;
    const Rect& b = material.sourceBounds;
    if (!p.cacheFrame || !p.window || !p.target || !std::isfinite(material.sigma) ||
        material.sigma <= 0.0f || material.sigma > 32.0f ||
        !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.width) || !std::isfinite(b.height) ||
        b.x < 0 || b.y < 0 || b.width <= 0 || b.height <= 0 ||
        b.x + b.width > strip.width || b.y + b.height > strip.height) {
        strip.poisoned = true; return;
    }
    const int x = static_cast<int>(std::floor(b.x)), y = static_cast<int>(std::floor(b.y));
    const int right = static_cast<int>(std::ceil(b.x + b.width));
    const int bottom = static_cast<int>(std::ceil(b.y + b.height));
    const int pad = static_cast<int>(std::ceil(3.0f * material.sigma));
    const int cropX = std::max(0, x - pad), cropY = std::max(0, y - pad);
    const int cropRight = std::min(strip.width, right + pad), cropBottom = std::min(strip.height, bottom + pad);
    const int width = cropRight - cropX, height = cropBottom - cropY;
    std::size_t bytes = 0;
    // Input, output and compatible target are bounded together. Effect/driver
    // internals are outside this explicit bitmap budget.
    if (!pixelBytes(width, height, bytes) || bytes > kStripBudget / 3) { strip.poisoned = true; return; }
    const auto format = p.target->GetPixelFormat();
    const D2D1_SIZE_U size{static_cast<UINT32>(width), static_cast<UINT32>(height)};
    const D2D1_SIZE_F sizeFloat{static_cast<float>(width), static_cast<float>(height)};
    ComPtr<ID2D1Bitmap> input, output;
    HRESULT hr = p.window->CreateBitmap(size, nullptr, 0, D2D1::BitmapProperties(format, 96, 96), input.GetAddressOf());
    if (SUCCEEDED(hr)) {
        const D2D1_POINT_2U dest{0, 0};
        const D2D1_RECT_U source{static_cast<UINT32>(cropX), static_cast<UINT32>(cropY),
            static_cast<UINT32>(cropRight), static_cast<UINT32>(cropBottom)};
        hr = input->CopyFromRenderTarget(&dest, p.target, &source);
    }
    ComPtr<ID2D1BitmapRenderTarget> outputTarget;
    if (SUCCEEDED(hr)) hr = p.window->CreateCompatibleRenderTarget(&sizeFloat, &size, &format,
        D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, outputTarget.GetAddressOf());
    ComPtr<ID2D1DeviceContext> context;
    if (SUCCEEDED(hr)) hr = outputTarget.As(&context);
    ComPtr<ID2D1Effect> effect;
    if (SUCCEEDED(hr)) hr = context->CreateEffect(CLSID_D2D1GaussianBlur, effect.GetAddressOf());
    if (SUCCEEDED(hr)) {
        effect->SetInput(0, input.Get());
        hr = effect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, material.sigma);
    }
    if (SUCCEEDED(hr)) hr = effect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    if (SUCCEEDED(hr)) {
        context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0, 0, 0)); context->DrawImage(effect.Get());
        hr = context->EndDraw();
        if (SUCCEEDED(hr)) hr = outputTarget->GetBitmap(output.GetAddressOf());
    }
    if (FAILED(hr) || !output) { strip.clearPixels(); strip.poisoned = true; return; }
    Impl::FrozenPanel panel;
    panel.id = material.panel; panel.x = x; panel.y = y; panel.width = right - x; panel.height = bottom - y;
    panel.cropX = cropX; panel.cropY = cropY; panel.cropWidth = width; panel.cropHeight = height;
    panel.bitmap = output; panel.bytes = bytes; panel.sigma = material.sigma;
    strip.bitmap = std::move(output); strip.panelBytes = bytes; strip.panels.push_back(std::move(panel));
    ++stats.stripCaptures; ++stats.stripFilters; stats.stripBytes = bytes;
    if (frozenTraceEnabled()) std::fprintf(stderr, "[D2D strip] captured %dx%d sigma=%.2f bytes=%zu session=%llu\n",
        width, height, material.sigma, bytes, static_cast<unsigned long long>(material.session));
}

void D2DRenderBackend::prepareFrozenBackdrop(const core::FrozenBackdrop& material) {
    if (material.cachedStrip) {
        if (material.session == impl_->strip.session && impl_->strip.bitmap)
            ++currentRenderFrameStats().stripReuses;
        return;
    }
    auto& p = *impl_;
    auto& frozen = p.frozen;
    if (!material.session || material.session != frozen.session || frozen.poisoned || !frozen.bitmap ||
        !p.window || !std::isfinite(material.sigma) || material.sigma <= 0.0f ||
        material.sigma > 64.0f || !std::isfinite(material.sourceBounds.x) ||
        !std::isfinite(material.sourceBounds.y) || !std::isfinite(material.sourceBounds.width) ||
        !std::isfinite(material.sourceBounds.height)) return;
    const Rect& b = material.sourceBounds;
    if (b.x < 0.0f || b.y < 0.0f || b.width <= 0.0f || b.height <= 0.0f ||
        b.x + b.width > frozen.width || b.y + b.height > frozen.height) return;
    const int x = static_cast<int>(std::floor(b.x));
    const int y = static_cast<int>(std::floor(b.y));
    const int right = static_cast<int>(std::ceil(b.x + b.width));
    const int bottom = static_cast<int>(std::ceil(b.y + b.height));
    if (right <= x || bottom <= y) return;
    for (auto& panel : frozen.panels) {
        if (panel.id == material.panel && panel.x == x && panel.y == y &&
            panel.width == right - x && panel.height == bottom - y && panel.sigma == material.sigma) {
            panel.lastUse = ++frozen.use;
            ++currentRenderFrameStats().frozenPanelReuses;
            return;
        }
    }
    for (auto it = frozen.panels.begin(); it != frozen.panels.end();) {
        if (it->id == material.panel) {
            frozen.panelBytes -= it->bytes;
            it = frozen.panels.erase(it);
        } else {
            ++it;
        }
    }
    if (frozen.failedPanels.find(material.panel) != frozen.failedPanels.end()) return;
    if (frozen.failedPanels.size() >= 16) return;
    const int padding = static_cast<int>(std::ceil(3.0f * material.sigma));
    const int cropX = std::max(0, x - padding), cropY = std::max(0, y - padding);
    const int cropRight = std::min(frozen.width, right + padding);
    const int cropBottom = std::min(frozen.height, bottom + padding);
    const int cropWidth = cropRight - cropX, cropHeight = cropBottom - cropY;
    std::size_t bytes = 0;
    if (!pixelBytes(cropWidth, cropHeight, bytes) ||
        frozen.sceneBytes + 2 * bytes > kFrozenBudget) {
        frozen.failedPanels.insert(material.panel);
        return;
    }
    while (!frozen.panels.empty() &&
           (frozen.panels.size() >= kFrozenPanelLimit ||
            frozen.residentBytes() + 2 * bytes > kFrozenBudget)) {
        const auto oldest = std::min_element(frozen.panels.begin(), frozen.panels.end(),
            [](const Impl::FrozenPanel& a, const Impl::FrozenPanel& b) { return a.lastUse < b.lastUse; });
        frozen.panelBytes -= oldest->bytes;
        frozen.panels.erase(oldest);
    }
    if (frozen.residentBytes() + 2 * bytes > kFrozenBudget) {
        frozen.failedPanels.insert(material.panel);
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    const D2D1_PIXEL_FORMAT format = frozen.bitmap->GetPixelFormat();
    const D2D1_SIZE_U size{static_cast<UINT32>(cropWidth), static_cast<UINT32>(cropHeight)};
    const D2D1_SIZE_F sizeFloat{static_cast<float>(cropWidth), static_cast<float>(cropHeight)};
    ComPtr<ID2D1Bitmap> input;
    HRESULT hr = p.window->CreateBitmap(size, nullptr, 0,
        D2D1::BitmapProperties(format, 96.0f, 96.0f), input.GetAddressOf());
    if (SUCCEEDED(hr)) {
        const D2D1_POINT_2U destination{0, 0};
        const D2D1_RECT_U source{static_cast<UINT32>(cropX), static_cast<UINT32>(cropY),
                                  static_cast<UINT32>(cropRight), static_cast<UINT32>(cropBottom)};
        hr = input->CopyFromBitmap(&destination, frozen.bitmap.Get(), &source);
    }
    ComPtr<ID2D1BitmapRenderTarget> outputTarget;
    if (SUCCEEDED(hr)) hr = p.window->CreateCompatibleRenderTarget(&sizeFloat, &size, &format,
        D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, outputTarget.GetAddressOf());
    ComPtr<ID2D1DeviceContext> context;
    if (SUCCEEDED(hr)) hr = outputTarget.As(&context);
    ComPtr<ID2D1Effect> effect;
    if (SUCCEEDED(hr)) hr = context->CreateEffect(CLSID_D2D1GaussianBlur, effect.GetAddressOf());
    if (SUCCEEDED(hr)) {
        effect->SetInput(0, input.Get());
        hr = effect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, material.sigma);
    }
    if (SUCCEEDED(hr)) hr = effect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    ComPtr<ID2D1Bitmap> output;
    if (SUCCEEDED(hr)) {
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0, 0, 0));
        context->DrawImage(effect.Get());
        hr = context->EndDraw();
        if (SUCCEEDED(hr)) hr = outputTarget->GetBitmap(output.GetAddressOf());
    }
    if (FAILED(hr) || !output || output->GetPixelSize().width != size.width ||
        output->GetPixelSize().height != size.height) {
        if (frozenTraceEnabled())
            std::fprintf(stderr, "[D2D frozen] panel filter failed id=%llu: 0x%08lx\n",
                static_cast<unsigned long long>(material.panel), static_cast<unsigned long>(hr));
        frozen.failedPanels.insert(material.panel);
        return;
    }
    Impl::FrozenPanel panel;
    panel.id = material.panel;
    panel.x = x; panel.y = y; panel.width = right - x; panel.height = bottom - y;
    panel.cropX = cropX; panel.cropY = cropY; panel.cropWidth = cropWidth; panel.cropHeight = cropHeight;
    panel.sigma = material.sigma; panel.bitmap = std::move(output);
    panel.bytes = bytes; panel.lastUse = ++frozen.use;
    frozen.panelBytes += bytes;
    frozen.panels.push_back(std::move(panel));
    auto& stats = currentRenderFrameStats();
    ++stats.frozenPanelFilters;
    stats.frozenFilterMs += millisecondsSince(started);
    stats.frozenBytes = frozen.residentBytes();
    if (frozenTraceEnabled())
        std::fprintf(stderr, "[D2D frozen] panel=%llu %dx%d sigma=%.2f filtered in %.3f ms\n",
            static_cast<unsigned long long>(material.panel), cropWidth, cropHeight,
            material.sigma, stats.frozenFilterMs);
}

void D2DRenderBackend::drawRoundedRect(const RoundedRectDrawCommand& c, int, int) {
    auto& p = *impl_; if (!p.target || !roundedRectHasVisibleContent(c)) return;
    p.target->SetTransform(transform(c.vertices));
    if (c.shadowPass) { p.shadow(c); if (p.target) p.target->SetTransform(D2D1::Matrix3x2F::Identity()); return; }
    const float radius = clampedPrimitiveRadius(c.radius, c.rect);
    const auto rr = D2D1::RoundedRect(box(c.rect), radius, radius);
    bool frozenDrawn = false;
    auto& materialScene = c.frozenBackdrop.cachedStrip ? p.strip : p.frozen;
    if (c.frozenBackdrop.session && c.frozenBackdrop.session == materialScene.session &&
        !materialScene.poisoned && materialScene.bitmap && !c.gradient.enabled) {
        for (auto& panel : materialScene.panels) {
            if (panel.id != c.frozenBackdrop.panel || !panel.bitmap) continue;
            const D2D1_RECT_F source = D2D1::RectF(
                static_cast<float>(panel.x - panel.cropX),
                static_cast<float>(panel.y - panel.cropY),
                static_cast<float>(panel.x - panel.cropX + panel.width),
                static_cast<float>(panel.y - panel.cropY + panel.height));
            if (radius <= 0.0f) {
                p.target->PushAxisAlignedClip(box(c.rect), D2D1_ANTIALIAS_MODE_ALIASED);
                p.target->DrawBitmap(panel.bitmap.Get(), box(c.rect), c.opacity,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
                p.target->PopAxisAlignedClip();
                frozenDrawn = true;
            } else {
                if (panel.clipOwner != p.target ||
                    panel.clipDeviceGeneration != p.deviceGeneration) {
                    panel.clipBrush.Reset();
                    panel.clipOwner = nullptr;
                }
                if (!panel.clipBrush && !panel.clipBrushFailed) {
                    const HRESULT hr = p.target->CreateBitmapBrush(
                        panel.bitmap.Get(), panel.clipBrush.GetAddressOf());
                    if (SUCCEEDED(hr)) {
                        panel.clipOwner = p.target;
                        panel.clipDeviceGeneration = p.deviceGeneration;
                        if (frozenTraceEnabled())
                            std::fprintf(stderr, "[D2D frozen] clip brush created panel=%llu\n",
                                static_cast<unsigned long long>(panel.id));
                    } else {
                        panel.clipBrushFailed = true;
                        if (frozenTraceEnabled())
                            std::fprintf(stderr, "[D2D frozen] clip brush failed panel=%llu: 0x%08lx\n",
                                static_cast<unsigned long long>(panel.id), static_cast<unsigned long>(hr));
                    }
                }
                if (panel.clipBrush && panel.width > 0 && panel.height > 0) {
                    const float scaleX = c.rect.width / static_cast<float>(panel.width);
                    const float scaleY = c.rect.height / static_cast<float>(panel.height);
                    panel.clipBrush->SetTransform(D2D1_MATRIX_3X2_F{
                        scaleX, 0.0f, 0.0f, scaleY,
                        c.rect.x - source.left * scaleX,
                        c.rect.y - source.top * scaleY});
                    panel.clipBrush->SetOpacity(c.opacity);
                    p.target->FillRoundedRectangle(rr, panel.clipBrush.Get());
                    frozenDrawn = true;
                }
            }
            break;
        }
    }
    if (c.frozenBackdrop.session && !frozenDrawn) {
        if (c.frozenBackdrop.cachedStrip) ++currentRenderFrameStats().stripFallbacks;
        else ++currentRenderFrameStats().frozenFallbacks;
    }
    if (c.gradient.enabled) {
        const D2D1_GRADIENT_STOP stops[]{{0, color(c.gradient.start, c.opacity)}, {1, color(c.gradient.end, c.opacity)}};
        ComPtr<ID2D1GradientStopCollection> collection; ComPtr<ID2D1LinearGradientBrush> gradient;
        HRESULT hr = p.target->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_1_0, D2D1_EXTEND_MODE_CLAMP, collection.GetAddressOf());
        if (SUCCEEDED(hr)) hr = p.target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(c.rect.x, c.rect.y), c.gradient.direction == GradientDirection::Horizontal ?
            D2D1::Point2F(c.rect.x + c.rect.width, c.rect.y) : D2D1::Point2F(c.rect.x, c.rect.y + c.rect.height)),
            collection.Get(), gradient.GetAddressOf());
        if (SUCCEEDED(hr)) p.target->FillRoundedRectangle(rr, gradient.Get()); else report("gradient", hr);
    } else p.target->FillRoundedRectangle(rr, p.solid(c.fillColor,
        c.opacity * (frozenDrawn ? std::clamp(c.frozenBackdrop.tintOpacity, 0.0f, 1.0f) : 1.0f)));
    ID2D1RadialGradientBrush* revealBrush = nullptr;
    if (c.reveal.enabled && c.reveal.tint.a > 0.0f &&
        ((c.reveal.radius > 0.0f &&
          (c.reveal.fillOpacity > 0.0f || c.reveal.borderOpacity > 0.0f)) ||
         (c.reveal.pressRadius > 0.0f && c.reveal.pressOpacity > 0.0f))) {
        revealBrush = p.radial(c.reveal.tint);
    }
    const auto positionReveal = [&](const Vec2& center, float glowRadius, float alpha) -> bool {
        if (!revealBrush || !std::isfinite(center.x) || !std::isfinite(center.y) ||
            !std::isfinite(glowRadius) || glowRadius <= 0.0f ||
            !std::isfinite(alpha) || alpha <= 0.0f) return false;
        revealBrush->SetCenter(D2D1::Point2F(c.rect.x + center.x, c.rect.y + center.y));
        revealBrush->SetRadiusX(glowRadius);
        revealBrush->SetRadiusY(glowRadius);
        revealBrush->SetOpacity(std::clamp(alpha * c.reveal.tint.a * c.opacity, 0.0f, 1.0f));
        return true;
    };
    if (positionReveal(c.reveal.center, c.reveal.radius, c.reveal.fillOpacity)) {
        p.target->FillRoundedRectangle(rr, revealBrush);
        ++currentRenderFrameStats().revealDraws;
    }
    if (positionReveal(c.reveal.pressCenter, c.reveal.pressRadius, c.reveal.pressOpacity)) {
        p.target->FillRoundedRectangle(rr, revealBrush);
        ++currentRenderFrameStats().revealDraws;
    }
    const float border = clampedPrimitiveBorderWidth(c.border.width, c.rect);
    if (border > 0 && c.border.color.a > 0) {
        Rect inner{c.rect.x + border * .5f, c.rect.y + border * .5f, c.rect.width - border, c.rect.height - border};
        p.target->DrawRoundedRectangle(D2D1::RoundedRect(box(inner), std::max(0.0f, radius - border * .5f),
            std::max(0.0f, radius - border * .5f)), p.solid(c.border.color, c.opacity), border);
    }
    const float revealBorder = clampedPrimitiveBorderWidth(c.reveal.borderWidth, c.rect);
    if (revealBorder > 0.0f &&
        positionReveal(c.reveal.center, c.reveal.radius, c.reveal.borderOpacity)) {
        Rect inner{c.rect.x + revealBorder * .5f, c.rect.y + revealBorder * .5f,
                   c.rect.width - revealBorder, c.rect.height - revealBorder};
        p.target->DrawRoundedRectangle(D2D1::RoundedRect(box(inner),
            std::max(0.0f, radius - revealBorder * .5f),
            std::max(0.0f, radius - revealBorder * .5f)), revealBrush, revealBorder);
        ++currentRenderFrameStats().revealDraws;
    }
    p.target->SetTransform(D2D1::Matrix3x2F::Identity());
}
void D2DRenderBackend::drawPolygon(const PolygonDrawCommand& c, int, int) {
    auto& p = *impl_; if (!p.target || c.edges.empty()) return;
    ComPtr<ID2D1PathGeometry> geometry; ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(p.factory->CreatePathGeometry(geometry.GetAddressOf())) || FAILED(geometry->Open(sink.GetAddressOf()))) return;
    sink->BeginFigure(D2D1::Point2F(c.edges[0].from.x, c.edges[0].from.y), D2D1_FIGURE_BEGIN_FILLED);
    for (const auto& edge : c.edges) sink->AddLine(D2D1::Point2F(edge.to.x, edge.to.y));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED); if (FAILED(sink->Close())) return;
    p.target->SetTransform(transform(c.vertices)); p.target->FillGeometry(geometry.Get(), p.solid(c.fillColor, c.opacity));
    p.target->SetTransform(D2D1::Matrix3x2F::Identity());
}
void D2DRenderBackend::drawText(const TextDrawCommand& c, int, int) {
    auto& p = *impl_; if (!p.target || !c.vertices) return;
    if (!p.atlas(p.gray, c.grayAtlas)) return;
    const bool hasColor = p.atlas(p.colored, c.colorAtlas);
    if (!p.target) return;
    for (std::size_t i = 0; i + 30 <= c.vertexFloatCount; i += 30) {
        const float* v = c.vertices + i;
        const bool colored = v[4] > .5f; const auto& a = colored ? p.colored : p.gray;
        if (colored && !hasColor) continue;
        const float w = (v[7] - v[2]) * a.width, h = (v[28] - v[3]) * a.sourceHeight;
        if (w <= 0 || h <= 0) continue;
        const D2D1_MATRIX_3X2_F m{(v[5] - v[0]) / w, (v[6] - v[1]) / w,
            (v[25] - v[0]) / h, (v[26] - v[1]) / h, v[0], v[1]};
        p.target->SetTransform(m);
        const auto source = D2D1::RectF(v[2] * a.width, v[3] * a.sourceHeight, v[7] * a.width, v[28] * a.sourceHeight);
        const auto dest = D2D1::RectF(0, 0, w, h);
        if (colored) p.target->DrawBitmap(a.bitmap.Get(), dest, c.color.a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
        else p.mask(a.bitmap.Get(), dest, source, c.color);
        if (!p.target) return;
    }
    p.target->SetTransform(D2D1::Matrix3x2F::Identity());
}
D2DRenderBackend::TextureHandle D2DRenderBackend::createTexture(const unsigned char* pixels, int width, int height) {
    auto* texture = new Impl::Texture;
    if (!updateTexture(texture, pixels, width, height)) { delete texture; return nullptr; }
    impl_->textures.insert(texture); return texture;
}
bool D2DRenderBackend::updateTexture(TextureHandle handle, const unsigned char* pixels, int width, int height) {
    auto& p = *impl_; auto* texture = static_cast<Impl::Texture*>(handle);
    if (!texture || !pixels || !p.window || width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
    const bool resize = width != texture->width || height != texture->height;
    texture->bgra.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < texture->bgra.size(); i += 4) {
        const unsigned alpha = pixels[i + 3];
        texture->bgra[i] = static_cast<unsigned char>((pixels[i + 2] * alpha + 127) / 255);
        texture->bgra[i + 1] = static_cast<unsigned char>((pixels[i + 1] * alpha + 127) / 255);
        texture->bgra[i + 2] = static_cast<unsigned char>((pixels[i] * alpha + 127) / 255);
        texture->bgra[i + 3] = static_cast<unsigned char>(alpha);
    }
    if (resize) texture->bitmap.Reset();
    texture->width = width; texture->height = height;
    texture->tint = {1, 1, 1, 1};
    HRESULT hr;
    if (!texture->bitmap) hr = p.window->CreateBitmap(D2D1::SizeU(width, height), texture->bgra.data(), width * 4,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), texture->bitmap.GetAddressOf());
    else hr = texture->bitmap->CopyFromMemory(nullptr, texture->bgra.data(), width * 4);
    return p.check(hr, "image upload");
}
void D2DRenderBackend::destroyTexture(TextureHandle handle) {
    auto* texture = static_cast<Impl::Texture*>(handle); impl_->textures.erase(texture); delete texture;
}
void D2DRenderBackend::drawTexture(TextureHandle handle, const float* v, std::size_t count,
    const Color& tint, const Rect& rect, float radius, float blur, int, int) {
    auto& p = *impl_; auto* texture = static_cast<Impl::Texture*>(handle);
    if (!p.target || !texture || !v || count < 42) return;
    const bool tintChanged = texture->tint.r != tint.r || texture->tint.g != tint.g || texture->tint.b != tint.b;
    if (!texture->bitmap || tintChanged) {
        std::vector<unsigned char> tinted;
        const unsigned char* pixels = texture->bgra.data();
        if (tint.r != 1 || tint.g != 1 || tint.b != 1) {
            tinted = texture->bgra;
            for (std::size_t i = 0; i < tinted.size(); i += 4) {
                tinted[i] = static_cast<unsigned char>(tinted[i] * std::clamp(tint.b, 0.0f, 1.0f));
                tinted[i + 1] = static_cast<unsigned char>(tinted[i + 1] * std::clamp(tint.g, 0.0f, 1.0f));
                tinted[i + 2] = static_cast<unsigned char>(tinted[i + 2] * std::clamp(tint.r, 0.0f, 1.0f));
            }
            pixels = tinted.data();
        }
        HRESULT hr;
        if (texture->bitmap) hr = texture->bitmap->CopyFromMemory(nullptr, pixels, texture->width * 4);
        else hr = p.window->CreateBitmap(D2D1::SizeU(texture->width, texture->height), pixels, texture->width * 4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
            texture->bitmap.GetAddressOf());
        if (!p.check(hr, "image tint/recovery")) return;
        texture->tint = tint;
    }
    // Image vertices use screen xyz, local xy, uv. Preserve fit/crop and affine mapping.
    const float w = v[10] - v[3], h = v[39] - v[4];
    if (w <= 0 || h <= 0) return;
    const D2D1_MATRIX_3X2_F m{(v[7] - v[0]) / w, (v[8] - v[1]) / w,
        (v[35] - v[0]) / h, (v[36] - v[1]) / h,
        v[0] - v[3] * (v[7] - v[0]) / w - v[4] * (v[35] - v[0]) / h,
        v[1] - v[3] * (v[8] - v[1]) / w - v[4] * (v[36] - v[1]) / h};
    if (blur > 0 && !p.warnedImageEffects) {
        std::fprintf(stderr, "[D2D] experimental image path: image blur pending\n"); p.warnedImageEffects = true;
    }
    p.target->SetTransform(m);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    bool pushed = false;
    if (radius > 0) {
        const float r = clampedPrimitiveRadius(radius, rect);
        HRESULT hr = p.factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box(rect), r, r), clip.GetAddressOf());
        if (SUCCEEDED(hr) && !p.imageClip) hr = p.target->CreateLayer(p.imageClip.GetAddressOf());
        if (SUCCEEDED(hr)) {
            p.target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), p.imageClip.Get()); pushed = true;
        }
    }
    p.target->DrawBitmap(texture->bitmap.Get(), D2D1::RectF(v[3], v[4], v[10], v[39]), tint.a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        D2D1::RectF(v[5] * texture->width, v[6] * texture->height, v[12] * texture->width, v[41] * texture->height));
    if (pushed) p.target->PopLayer();
    p.target->SetTransform(D2D1::Matrix3x2F::Identity());
}

} // namespace core::render::d2d
