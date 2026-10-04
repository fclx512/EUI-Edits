#include "core/render/d2d/d2d_backend.h"
#include "core/window/window_backend.h"
#include "core/platform/platform.h"
#include "core/render/d2d/cache_capacity.h"
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
#include <wrl/client.h>

#include <array>
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
}

struct D2DRenderBackend::Impl {
    struct Atlas { ComPtr<ID2D1Bitmap> bitmap; std::uint64_t generation = 0; int width = 0, height = 0; };
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
    std::size_t shadowBytes = 0;
    int width = 0, height = 0, cacheWidth = 0, cacheHeight = 0;
    CacheCapacity capacity;
    double cacheSizeChangedAt = 0;
    bool failed = false, presented = false, cacheInvalid = true;
    bool software = false, frame = false, cacheFrame = false, clipped = false, recreated = false;
    bool warnedBackdrop = false, warnedImageEffects = false;

    ~Impl() { releaseDevice(); for (auto* texture : textures) delete texture; }
    void unclip() { if (target && clipped) target->PopAxisAlignedClip(); clipped = false; }
    void releaseDevice() {
        unclip(); target = nullptr; frame = cacheFrame = false;
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
        if (FAILED(hr)) { report("Create window render target", hr); releaseDevice(); return false; }
        hr = window->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush.GetAddressOf());
        if (FAILED(hr)) { report("CreateSolidColorBrush", hr); releaseDevice(); return false; }
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
    bool atlas(Atlas& a, const TextAtlasPageData& page) {
        if (!page.pixels || page.width <= 0 || page.height <= 0) return false;
        if (a.bitmap && a.generation == page.generation && a.width == page.width && a.height == page.height) return true;
        // text.cpp swaps the channels of FreeType's premultiplied BGRA;
        // swapping back must not multiply the alpha a second time.
        std::vector<unsigned char> converted;
        const unsigned char* pixels = page.pixels;
        if (page.channels == 4) {
            converted.resize(static_cast<std::size_t>(page.width) * page.height * 4);
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
        if (!a.bitmap || a.width != page.width || a.height != page.height) {
            a.bitmap.Reset();
            hr = window->CreateBitmap(D2D1::SizeU(page.width, page.height), pixels, pitch,
                D2D1::BitmapProperties(D2D1::PixelFormat(page.channels == 1 ? DXGI_FORMAT_A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM,
                                                       D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), a.bitmap.GetAddressOf());
        } else hr = a.bitmap->CopyFromMemory(nullptr, pixels, pitch);
        if (!check(hr, "atlas upload")) return false;
        a.generation = page.generation; a.width = page.width; a.height = page.height;
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
#if defined(EUI_WINDOW_BACKEND_WIN32)
    // Native low-memory experiment defaults to software even when started directly.
    impl_->software = !(optionLength == 1 && software[0] == '0');
    char dcOption[8]{};
    impl_->gdiPresentation = impl_->software &&
        !(GetEnvironmentVariableA("NEO_WIN32_DC", dcOption, sizeof(dcOption)) == 1 && dcOption[0] == '0');
#else
    impl_->software = optionLength == 1 && software[0] == '1';
#endif
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
        p.cache = std::move(replacement); p.capacity = next;
        core::render::gpuResizeStats().addRenderCacheAllocation();
        p.recreated = true; p.cacheInvalid = true;
    }
    p.cacheWidth = width; p.cacheHeight = height;
    return true;
}
bool D2DRenderBackend::renderCacheWasRecreated() const { return impl_->recreated; }
void D2DRenderBackend::releaseRenderCache() {
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
void D2DRenderBackend::drawRoundedRect(const RoundedRectDrawCommand& c, int, int) {
    auto& p = *impl_; if (!p.target || !roundedRectHasVisibleContent(c)) return;
    p.target->SetTransform(transform(c.vertices));
    if (c.shadowPass) { p.shadow(c); if (p.target) p.target->SetTransform(D2D1::Matrix3x2F::Identity()); return; }
    const float radius = clampedPrimitiveRadius(c.radius, c.rect);
    const auto rr = D2D1::RoundedRect(box(c.rect), radius, radius);
    if (c.gradient.enabled) {
        const D2D1_GRADIENT_STOP stops[]{{0, color(c.gradient.start, c.opacity)}, {1, color(c.gradient.end, c.opacity)}};
        ComPtr<ID2D1GradientStopCollection> collection; ComPtr<ID2D1LinearGradientBrush> gradient;
        HRESULT hr = p.target->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_1_0, D2D1_EXTEND_MODE_CLAMP, collection.GetAddressOf());
        if (SUCCEEDED(hr)) hr = p.target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(c.rect.x, c.rect.y), c.gradient.direction == GradientDirection::Horizontal ?
            D2D1::Point2F(c.rect.x + c.rect.width, c.rect.y) : D2D1::Point2F(c.rect.x, c.rect.y + c.rect.height)),
            collection.Get(), gradient.GetAddressOf());
        if (SUCCEEDED(hr)) p.target->FillRoundedRectangle(rr, gradient.Get()); else report("gradient", hr);
    } else p.target->FillRoundedRectangle(rr, p.solid(c.fillColor, c.opacity));
    const float border = clampedPrimitiveBorderWidth(c.border.width, c.rect);
    if (border > 0 && c.border.color.a > 0) {
        Rect inner{c.rect.x + border * .5f, c.rect.y + border * .5f, c.rect.width - border, c.rect.height - border};
        p.target->DrawRoundedRectangle(D2D1::RoundedRect(box(inner), std::max(0.0f, radius - border * .5f),
            std::max(0.0f, radius - border * .5f)), p.solid(c.border.color, c.opacity), border);
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
        const float w = (v[7] - v[2]) * a.width, h = (v[28] - v[3]) * a.height;
        if (w <= 0 || h <= 0) continue;
        const D2D1_MATRIX_3X2_F m{(v[5] - v[0]) / w, (v[6] - v[1]) / w,
            (v[25] - v[0]) / h, (v[26] - v[1]) / h, v[0], v[1]};
        p.target->SetTransform(m);
        const auto source = D2D1::RectF(v[2] * a.width, v[3] * a.height, v[7] * a.width, v[28] * a.height);
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
