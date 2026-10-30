#if defined(EUI_RENDER_BACKEND_D2D) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_INCLUDE_NONE
#include <windows.h>
#if defined(EUI_WINDOW_BACKEND_WIN32)
#include "core/window/win32_host.h"
namespace host = core::window::win32;
#else
#include "core/window/glfw_host.h"
namespace host = core::window::glfwHost;
#endif
#include "core/render/render_backend.h"
#include "core/render/gpu_resize_stats.h"
#include "core/window/window_backend.h"

#include <array>
#include <cstdio>
#include <cmath>
#include <cstdint>

namespace {
int failures = 0;
// Contract groups. The composed-screen group decides whether a presented D2D
// frame actually reaches the desktop; the GDI-surface group decides whether
// the WM_PAINT handler filled the exposed strips before EndPaint. Neither
// group is skipped, and a failure in either fails the run.
int screenChecks = 0, screenFailures = 0;
int gdiSurfaceChecks = 0, gdiSurfaceFailures = 0;
std::uint64_t screenSampleDigest = 14695981039346656037ull;

void check(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}

// Mirrors the env-var semantics of core/render/d2d/d2d_backend.cpp only to
// label the run and choose the readback strategy; the backend remains the
// authority for what it actually creates.
struct PresentationArm {
    bool software = true;
    bool gdiPresentation = true;
    const char* name = "A (SOFTWARE + DC/GDI)";
};

PresentationArm presentationArm() {
    PresentationArm arm;
    char software[8]{};
    const DWORD length = GetEnvironmentVariableA("NEO_D2D_SOFTWARE", software, sizeof(software));
#if defined(EUI_WINDOW_BACKEND_WIN32)
    arm.software = !(length == 1 && software[0] == '0');
    char dc[8]{};
    arm.gdiPresentation = arm.software &&
        !(GetEnvironmentVariableA("NEO_WIN32_DC", dc, sizeof(dc)) == 1 && dc[0] == '0');
#else
    arm.software = length == 1 && software[0] == '1';
    arm.gdiPresentation = false;
#endif
    arm.name = arm.gdiPresentation ? "A (SOFTWARE + DC/GDI)"
             : arm.software        ? "B (SOFTWARE + Hwnd)"
                                   : "C (DEFAULT + Hwnd, actual raster unconfirmed)";
    return arm;
}
const PresentationArm presentation = presentationArm();

struct DwmApi {
    HMODULE module = nullptr;
    HRESULT(WINAPI* flush)() = nullptr;
    HRESULT(WINAPI* getWindowAttribute)(HWND, DWORD, void*, DWORD) = nullptr;
};
const DwmApi dwm = [] {
    DwmApi api;
    // dwmapi is not a link dependency of this probe target; load it dynamically
    // so no CMake change is needed. A missing export only degrades to Sleep().
    api.module = LoadLibraryW(L"dwmapi.dll");
    if (api.module) {
        api.flush = reinterpret_cast<HRESULT(WINAPI*)()>(
            reinterpret_cast<void*>(GetProcAddress(api.module, "DwmFlush")));
        api.getWindowAttribute = reinterpret_cast<HRESULT(WINAPI*)(HWND, DWORD, void*, DWORD)>(
            reinterpret_cast<void*>(GetProcAddress(api.module, "DwmGetWindowAttribute")));
    }
    return api;
}();

bool colorsClose(COLORREF c, int r, int g, int b) {
    return std::abs(static_cast<int>(GetRValue(c)) - r) <= 4 &&
        std::abs(static_cast<int>(GetGValue(c)) - g) <= 4 &&
        std::abs(static_cast<int>(GetBValue(c)) - b) <= 4;
}
void describe(COLORREF c) {
    std::fprintf(stderr, "%d,%d,%d", GetRValue(c), GetGValue(c), GetBValue(c));
}

// The window's GDI surface: what BeginPaint/GetDC(hwnd) samples. Under DC/GDI
// presentation this is also the presented surface; under Hwnd presentation it
// only receives GDI fills (WM_PAINT strips) and stays blank elsewhere.
bool readWindowDcPixel(HWND hwnd, int x, int y, COLORREF& out) {
    GdiFlush();
    const HDC dc = GetDC(hwnd);
    if (!dc) return false;
    const COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(hwnd, dc);
    if (c == CLR_INVALID) return false;
    out = c;
    return true;
}

// Composed-desktop sample at a client position of the probe's own visible
// foreground window. Every precondition is checked explicitly: foreground
// ownership, client bounds, ClientToScreen, DC acquisition and GetPixel
// failure. A successful read is never treated as "frame displayed" by itself;
// callers decide from the sampled values.
bool readScreenPixel(HWND hwnd, int x, int y, COLORREF& out) {
    if (GetForegroundWindow() != hwnd) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client) || x < client.left || x >= client.right ||
        y < client.top || y >= client.bottom) return false;
    POINT p{x, y};
    if (!ClientToScreen(hwnd, &p)) return false;
    const HWND hit = WindowFromPoint(p);
    if (!hit || GetAncestor(hit, GA_ROOT) != hwnd) {
        std::fprintf(stderr, "screen sample (%d,%d) is occluded or outside the probe's visible window\n", x, y);
        return false;
    }
    GdiFlush(); // Flush this fixture's GDI batch before waiting for composition.
    if (dwm.flush) dwm.flush(); // wait for the next DWM composition
    const HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const COLORREF c = GetPixel(dc, p.x, p.y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID) return false;
    out = c;
    return true;
}

// Primary pixel contract: the color DWM actually shows. The bounded retry
// loop absorbs composition latency only; tolerance and expected values never
// widen, and a persistent mismatch still fails. The window-DC read is kept as
// a diagnostic that distinguishes the two read paths instead of being allowed
// to substitute for the screen verdict.
void expectScreenPixel(HWND hwnd, int x, int y, int r, int g, int b, const char* label) {
    ++screenChecks;
    COLORREF screen{};
    bool matched = false, readable = false, lostForeground = false;
    for (int attempt = 0; attempt < 10 && !matched; ++attempt) {
        if (!readScreenPixel(hwnd, x, y, screen)) {
            if (GetForegroundWindow() != hwnd) { lostForeground = true; break; }
            Sleep(25);
            continue;
        }
        readable = true;
        matched = colorsClose(screen, r, g, b);
        if (!matched) Sleep(25);
    }
    if (matched) {
        // Deterministic ordered client samples supplement the color contracts
        // when comparing the same fixture with backend optimizations disabled.
        screenSampleDigest = (screenSampleDigest ^ static_cast<std::uint32_t>(screen)) * 1099511628211ull;
        COLORREF gdi{};
        const bool gdiReadable = readWindowDcPixel(hwnd, x, y, gdi);
        if (presentation.gdiPresentation) {
            // DC/GDI presentation draws into the same GDI surface this probe
            // sampled historically; keep that contract strict for arm A.
            ++gdiSurfaceChecks;
            if (!gdiReadable || !colorsClose(gdi, r, g, b)) {
                ++gdiSurfaceFailures; ++failures;
                std::fprintf(stderr, "%s: window-DC reads ", label);
                if (gdiReadable) describe(gdi);
                else std::fprintf(stderr, "unreadable");
                std::fprintf(stderr, " while the composed screen shows the expected %d,%d,%d\n", r, g, b);
                std::fprintf(stderr, "FAIL: %s (GDI surface mismatch under DC/GDI presentation)\n", label);
            }
        } else if (gdiReadable && !colorsClose(gdi, r, g, b)) {
            std::fprintf(stderr, "note: %s - window-DC reads ", label);
            describe(gdi);
            std::fprintf(stderr, " while the composed screen shows the expected %d,%d,%d "
                "(Hwnd presentation does not route through the GDI surface)\n", r, g, b);
        }
        return;
    }
    ++screenFailures; ++failures;
    if (lostForeground) {
        std::fprintf(stderr, "FAIL: %s (probe window lost foreground before the screen read; "
            "composed-desktop sampling requires the probe's own visible foreground window)\n", label);
        return;
    }
    if (!readable) {
        std::fprintf(stderr, "FAIL: %s (screen readback unavailable: foreground/client "
            "coordinates/GetPixel all checked; nothing was sampled)\n", label);
        return;
    }
    std::fprintf(stderr, "%s: actual %d,%d,%d expected %d,%d,%d (composed screen)\n", label,
        GetRValue(screen), GetGValue(screen), GetBValue(screen), r, g, b);
    COLORREF gdi{};
    if (readWindowDcPixel(hwnd, x, y, gdi)) {
        if (colorsClose(gdi, r, g, b))
            std::fprintf(stderr, "  note: window-DC holds the expected color while the screen does not; "
                "content reached the GDI surface but was not composited to the desktop\n");
        else {
            std::fprintf(stderr, "  note: window-DC reads ");
            describe(gdi);
            std::fprintf(stderr, " (diagnostic)\n");
        }
    } else {
        std::fprintf(stderr, "  note: window-DC read failed too (diagnostic)\n");
    }
    std::fprintf(stderr, "FAIL: %s\n", label);
}

// Contract of the WM_PAINT handler itself: only the newly exposed strips are
// filled on the window's GDI surface before EndPaint. That fill is
// presentation-agnostic, so it is verified on the window DC in every arm;
// whether the fill is visible on the composed desktop is presentation
// dependent and recorded as an observation, never as a skip.
void expectGdiSurfacePixel(HWND hwnd, int x, int y, int r, int g, int b, const char* label) {
    ++gdiSurfaceChecks;
    COLORREF gdi{};
    if (!readWindowDcPixel(hwnd, x, y, gdi)) {
        ++gdiSurfaceFailures; ++failures;
        std::fprintf(stderr, "FAIL: %s (window-DC read failed)\n", label);
        return;
    }
    if (colorsClose(gdi, r, g, b)) {
        COLORREF screen{};
        if (readScreenPixel(hwnd, x, y, screen) && !colorsClose(screen, r, g, b))
            std::fprintf(stderr, "note: %s - GDI surface holds the fill while the composed screen "
                "shows %d,%d,%d (visibility is presentation dependent)\n", label,
                GetRValue(screen), GetGValue(screen), GetBValue(screen));
        return;
    }
    ++gdiSurfaceFailures; ++failures;
    std::fprintf(stderr, "%s: actual %d,%d,%d expected %d,%d,%d (window-DC)\n", label,
        GetRValue(gdi), GetGValue(gdi), GetBValue(gdi), r, g, b);
    std::fprintf(stderr, "FAIL: %s\n", label);
}

std::array<float, 30> glyph(float x, float y, bool colored) {
    const float c = colored ? 1.0f : 0.0f;
    return {x,y,0,0,c, x+16,y,1,0,c, x+16,y+16,1,1,c,
            x,y,0,0,c, x+16,y+16,1,1,c, x,y+16,0,1,c};
}

void recordWindowRgbRegion(HWND hwnd, const char* group, int pass) {
    if (!presentation.gdiPresentation) {
        std::printf("D2D interior RGB hash group=%s pass=%d NOT_PROVIDED (Hwnd screen arm)\n",group,pass);
        return;
    }
    DWORD owner=0; GetWindowThreadProcessId(hwnd,&owner);
    RECT client{};
    if (owner!=GetCurrentProcessId() || !GetClientRect(hwnd,&client) ||
        client.right<=32 || client.bottom<=32) {
        check(false,"RGB region belongs to this process and has an interior"); return;
    }
    const int x=16,y=16,w=client.right-32,h=client.bottom-32;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=w; info.bmiHeader.biHeight=-h;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    GdiFlush();
    HDC source=GetDC(hwnd), copy=source?CreateCompatibleDC(source):nullptr;
    void* pixels=nullptr;
    HBITMAP bitmap=copy?CreateDIBSection(source,&info,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
    HGDIOBJ previous=bitmap?SelectObject(copy,bitmap):nullptr;
    const bool selected=previous && previous!=HGDI_ERROR;
    const bool copied=selected && BitBlt(copy,0,0,w,h,source,x,y,SRCCOPY);
    GdiFlush();
    if (copied && pixels) {
        std::uint64_t hash=14695981039346656037ull;
        const auto* bytes=static_cast<const unsigned char*>(pixels);
        const std::uint64_t count=static_cast<std::uint64_t>(w)*h;
        for (std::uint64_t i=0;i<count;++i)
            for (const int channel : {2,1,0}) hash=(hash^bytes[i*4+channel])*1099511628211ull;
        std::printf("D2D interior RGB hash group=%s pass=%d x=%d y=%d width=%d height=%d pixels=%llu "
            "fnv64=%016llx region_initialized=true (window DC; alpha ignored)\n",
            group,pass,x,y,w,h,static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(hash));
    } else check(false,"window-DC RGB region BitBlt readback");
    if (selected) SelectObject(copy,previous);
    if (bitmap) DeleteObject(bitmap);
    if (copy) DeleteDC(copy);
    if (source) ReleaseDC(hwnd,source);
}

// Keep this exact fixture buildable against the frozen backend/header too.
template <typename Stats>
auto reportScissorCounts(const Stats& stats, int)
    -> decltype((void)stats.scissorPushes, (void)stats.scissorPops, (void)stats.scissorReuses, void()) {
    char option[8]{};
    const DWORD length=GetEnvironmentVariableA("NEO_D2D_SCISSOR_CACHE",option,sizeof(option));
    const bool enabled=!(length==1 && option[0]=='0');
    std::printf("D2D outer scissor counters: pushes=%d pops=%d reuses=%d cache=%s "
        "(whole direct-backend fixture; temporary material clips excluded)\n",
        stats.scissorPushes,stats.scissorPops,stats.scissorReuses,enabled?"on":"off");
    check(stats.scissorPushes>0 && stats.scissorPushes==stats.scissorPops,
          "outer scissor pushes/pops balance across frames and reconstruction");
    check(enabled ? stats.scissorReuses>0 : stats.scissorReuses==0,
          "scissor diagnostic switch controls exact-clip reuse");
}
template <typename Stats>
void reportScissorCounts(const Stats&, long) {
    std::puts("D2D outer scissor counters: unavailable (legacy backend/header)");
}

void scissorPixelContracts(core::render::RenderBackend& backend,
                          core::window::Handle window, HWND hwnd, int width, int height) {
    static int invocation=0;
    const int pass=++invocation;
    const auto startWindow = [&] {
        backend.beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
        check(backend.frameReady(), "scissor window frame ready");
        check(backend.ensureRenderCache(width, height), "scissor render cache available");
    };
    const auto startCache = [&] { startWindow(); backend.beginRenderCacheFrame(width,height); };
    const auto presented = [&] {
        backend.present(); host::pollEvents();
        check(backend.framePresented(), "scissor frame EndDraw accepted");
    };
    const auto presentCache = [&] {
        // Deliberately leave the clip active: endRenderCacheFrame and present
        // own its cleanup, independently of callers disabling scissor first.
        backend.endRenderCacheFrame(); backend.blitRenderCache(width,height); presented();
    };
    const auto rectangle = [&](core::Rect bounds, core::Color color, core::Vec2 translation = {}) {
        core::render::RoundedRectDrawCommand c; c.rect=bounds; c.fillColor=color;
        core::Transform transform; transform.translate=translation;
        const auto vertices=core::render::roundedRectGeometryVertices(bounds, transform, {}, false, bounds);
        c.vertices.assign(vertices.begin(),vertices.end());
        backend.drawRoundedRect(c,width,height);
    };
    const core::Rect a{40,40,80,60}, b{160,40,40,60};
    startCache(); backend.clear({0,0,1,1});
    backend.setScissor(true,a,height); backend.clear({1,0,0,1});
    rectangle({20,20,100,60},{0,1,0,1},{20,20});
    backend.setScissor(true,a,height);
    rectangle({60,60,40,20},{1,1,0,1});
    backend.setScissor(true,b,height); backend.clear({0,1,0,1});
    backend.setScissor(false,{},height); backend.setScissor(false,{},height);
    rectangle({240,40,40,60},{1,0,0,1});
    presentCache();
    expectScreenPixel(hwnd,50,50,0,255,0,"repeat clip after transformed draw");
    expectScreenPixel(hwnd,70,70,255,255,0,"repeat clip restores identity");
    expectScreenPixel(hwnd,140,70,0,0,255,"changed clip preserves the gap");
    expectScreenPixel(hwnd,180,70,0,255,0,"changed clip applies new bounds");
    expectScreenPixel(hwnd,260,70,255,0,0,"disabled clip permits outside drawing");
    recordWindowRgbRegion(hwnd,"repeat-change-disable",pass);

    startCache(); backend.setScissor(false,{},height); backend.clear({0,0,1,1}); presentCache();
    startWindow();
    backend.setScissor(true,a,height); backend.clear({1,0,0,1});
    backend.beginRenderCacheFrame(width,height);
    // Same numerical clip on a different render target must be installed anew.
    backend.setScissor(true,a,height); backend.clear({0,1,0,1});
    backend.endRenderCacheFrame();
    backend.setScissor(true,a,height); backend.clear({1,1,0,1}); presented();
    expectScreenPixel(hwnd,70,70,255,255,0,"cache to window same clip");
    expectScreenPixel(hwnd,140,70,0,0,255,"cache to window clip boundary");
    recordWindowRgbRegion(hwnd,"cache-to-window",pass);
    startWindow(); backend.blitRenderCache(width,height); presented();
    expectScreenPixel(hwnd,70,70,0,255,0,"window to cache same clip");
    expectScreenPixel(hwnd,140,70,0,0,255,"window to cache clip boundary");
    recordWindowRgbRegion(hwnd,"window-to-cache",pass);
    startWindow(); backend.setScissor(true,a,height); backend.clear({0,1,1,1}); presented();
    startWindow(); // Previous present must have popped the active clip.
    rectangle({240,40,40,60},{1,0,0,1}); presented();
    expectScreenPixel(hwnd,70,70,0,255,255,"same clip in the next frame");
    expectScreenPixel(hwnd,260,70,255,0,0,"frame end clears the active clip");
    recordWindowRgbRegion(hwnd,"frame-end",pass);

    startCache(); backend.setScissor(false,{},height); backend.clear({0,0,1,1});
    const core::Rect imageClip{40,140,80,70};
    backend.setScissor(true,imageClip,height);
    const unsigned char white[]{255,255,255,255};
    const auto texture=backend.createTexture(white,1,1);
    check(texture!=nullptr,"scissor rounded texture available");
    const float image[]{50,150,1,50,150,0,0, 90,150,1,90,150,1,0, 90,190,1,90,190,1,1,
                        50,150,1,50,150,0,0, 90,190,1,90,190,1,1, 50,190,1,50,190,0,1};
    backend.drawTexture(texture,image,42,{0,1,0,1},{50,150,40,40},12,0,width,height);
    backend.setScissor(true,imageClip,height);
    rectangle({100,150,40,40},{1,0,0,1});
    presentCache(); backend.destroyTexture(texture);
    expectScreenPixel(hwnd,70,170,0,255,0,"rounded image inside repeated clip");
    expectScreenPixel(hwnd,50,150,0,0,255,"rounded image corner remains clipped");
    expectScreenPixel(hwnd,110,170,255,0,0,"rounded image PopLayer preserves outer clip");
    expectScreenPixel(hwnd,130,170,0,0,255,"repeated clip after image retains boundary");
    recordWindowRgbRegion(hwnd,"rounded-image",pass);

    // Exercise the optional square material's paired Push/Pop when supported;
    // otherwise assert its documented pure-color fallback with the same clip.
    startCache(); backend.setScissor(false,{},height); backend.clear({0,0,1,1});
    backend.beginFrozenBackdropFrame(9001,1,width,height,1); presentCache();
    startCache();
    if (backend.beginFrozenBackdropFrame(9001,1,width,height,1)) backend.captureFrozenBackdropScene();
    core::FrozenBackdrop material; material.session=9001; material.panel=1;
    material.sourceBounds={210,150,40,40}; material.sigma=1; material.tintOpacity=0;
    const auto filters=core::render::currentRenderFrameStats().frozenPanelFilters;
    backend.prepareFrozenBackdrop(material);
    const bool filtered=core::render::currentRenderFrameStats().frozenPanelFilters>filters;
    const core::Rect materialClip{200,140,80,70};
    backend.setScissor(true,materialClip,height);
    core::render::RoundedRectDrawCommand panel; panel.rect={210,150,40,40}; panel.fillColor={0,0,1,1};
    panel.frozenBackdrop=material;
    const auto vertices=core::render::roundedRectGeometryVertices(panel.rect,{}, {},false,panel.rect);
    panel.vertices.assign(vertices.begin(),vertices.end()); backend.drawRoundedRect(panel,width,height);
    backend.setScissor(true,materialClip,height); rectangle({260,150,40,40},{1,0,0,1});
    presentCache();
    expectScreenPixel(hwnd,230,170,0,0,255,"material or fallback retains its fill");
    expectScreenPixel(hwnd,270,170,255,0,0,"material PopClip preserves outer clip");
    expectScreenPixel(hwnd,290,170,0,0,255,"repeated clip after material retains boundary");
    recordWindowRgbRegion(hwnd,"material",pass);
    std::printf("scissor material route=%s; paired material clip %s\n",
        filtered ? "filtered" : "pure-color fallback", filtered ? "covered" : "NOT_RUN (filter unavailable)");
    backend.beginFrozenBackdropFrame(0,0,width,height,1);
    backend.releaseRenderCache();
    startCache(); backend.setScissor(false,{},height); backend.clear({0,0,1,1});
    backend.setScissor(true,a,height); backend.clear({0,1,0,1});
    backend.setScissor(false,{},height); rectangle({240,40,40,60},{1,0,0,1}); presentCache();
    expectScreenPixel(hwnd,70,70,0,255,0,"recreated cache installs clip");
    expectScreenPixel(hwnd,260,70,255,0,0,"recreated cache disables clip");
    recordWindowRgbRegion(hwnd,"cache-recreated",pass);
}
}

int main() {
    SetProcessDPIAware();
    if (!host::initialize()) { std::fprintf(stderr,"Host initialization failed\n"); return 2; }
    core::window::WindowCreateRequest request;
    request.width = 640; request.height = 360; request.title = "D2D contract probe";
    request.renderApi = core::render::windowRenderApi();
    auto window = core::window::createWindow(request);
    if (!window) { std::fprintf(stderr,"Window creation failed\n"); host::shutdownHost(); return 2; }
    HWND hwnd = static_cast<HWND>(core::window::nativeWindowInfo(window).platformWindow);
    const DWORD thread = GetCurrentThreadId();
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = foregroundThread && foregroundThread != thread && AttachThreadInput(thread, foregroundThread, TRUE);
    SetForegroundWindow(hwnd); SetFocus(hwnd);
    if (attached) AttachThreadInput(thread, foregroundThread, FALSE);
    Sleep(150);
    if (GetForegroundWindow() != hwnd) { std::fprintf(stderr,"Foreground ownership unavailable\n"); core::window::destroyWindow(window); host::shutdownHost(); return 2; }
    // Foreground ownership alone does not put a normal window above another
    // application's topmost window. Raise only this short-lived fixture, and
    // verify per-pixel ownership when sampling; never substitute its window DC
    // for a failed screen assertion.
    POINT sample{488,48};
    if (ClientToScreen(hwnd,&sample)) {
        const HWND initialHit=WindowFromPoint(sample);
        std::printf("Initial screen sample ownership: %s\n",
            initialHit && GetAncestor(initialHit,GA_ROOT)==hwnd ? "probe" : "occluded");
    }
    check(SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)!=0,
          "own probe window is visible above topmost overlays");
    // Product parity: the real runner services the window queue every frame
    // (core/app/desktop_app_main.inl). Dispatch the creation-time messages
    // here so the initial WM_PAINT fills the GDI surface before the first D2D
    // frame is drawn, matching startup order; afterwards the WM_PAINT handler
    // only fills strips beyond the already-presented area, so a queued
    // WM_PAINT can never overwrite presented content.
    host::pollEvents();
    std::printf("D2D pixel contracts, arm %s; primary readback: composed screen "
        "(%s + desktop DC at ClientToScreen coordinates); window-DC readback: %s; "
        "frame loop pumps the message queue via host pollEvents (product parity)\n",
        presentation.name, dwm.flush ? "DwmFlush" : "Sleep fallback",
        presentation.gdiPresentation ? "contract (DC/GDI draws into it)" : "diagnostic only");
    auto backend = core::render::createRenderBackend(window);
    if (!backend || !backend->initialize()) { std::fprintf(stderr,"Renderer initialization failed\n"); core::window::destroyWindow(window); host::shutdownHost(); return 2; }
    core::render::ScopedRenderBackend active(*backend);
    int width, height;
    host::framebufferSize(static_cast<host::Window*>(window), &width, &height);
    const auto begin = [&] {
        backend->beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
        check(backend->ensureRenderCache(width, height), "render cache supported");
        backend->beginRenderCacheFrame(width, height);
    };
    const auto present = [&] {
        backend->setScissor(false, {}, height);
        backend->endRenderCacheFrame(); backend->blitRenderCache(width, height); backend->present();
        // EndDraw acceptance is an API-level assertion only; it is never
        // treated as proof that a frame reached the display. Pump the queue
        // afterwards exactly like the product frame loop does.
        host::pollEvents();
        check(backend->framePresented(), "frame presented (EndDraw accepted, API success is not display proof)");
    };
    begin(); check(backend->renderCacheWasRecreated(), "first cache created");
    backend->clear({.1f,.2f,.3f,1});
    core::render::RoundedRectDrawCommand rect;
    rect.rect = {20,20,60,60}; rect.fillColor = {1,0,0,1};
    const auto geometry = core::render::roundedRectGeometryVertices(rect.rect, {}, {}, false, rect.rect);
    rect.vertices.assign(geometry.begin(), geometry.end());
    backend->drawRoundedRect(rect, width, height);
    present(); expectScreenPixel(hwnd, 40,40,255,0,0,"initial rectangle");

    begin(); check(!backend->renderCacheWasRecreated(), "same size cache reused");
    backend->setScissor(true, {120,20,60,60}, height); backend->clear({0,1,0,1});
    present();
    expectScreenPixel(hwnd,40,40,255,0,0,"dirty clear preserves prior pixels");
    expectScreenPixel(hwnd,140,40,0,255,0,"dirty region cleared");
    expectScreenPixel(hwnd,220,40,26,51,77,"dirty clear preserves background");

    begin();
    const unsigned char white[]{255,255,255,255};
    auto texture = backend->createTexture(white,1,1);
    check(texture != nullptr,"image created");
    const float image[]{300,30,1,300,30,0,0, 340,30,1,340,30,1,0, 340,70,1,340,70,1,1,
                        300,30,1,300,30,0,0, 340,70,1,340,70,1,1, 300,70,1,300,70,0,1};
    backend->drawTexture(texture,image,42,{.5f,1,.25f,1},{300,30,40,40},12,0,width,height);
    std::array<unsigned char,16*128> gray; gray.fill(128);
    std::array<unsigned char,16*128*4> colored{};
    for (int i=0;i<16*128;++i) { colored[i*4]=128; colored[i*4+3]=128; }
    core::render::TextDrawCommand text;
    text.grayAtlas = {core::render::TextAtlasPageKind::Gray,16,128,1,1,gray.data(),16};
    text.colorAtlas = {core::render::TextAtlasPageKind::Color,16,128,4,1,colored.data(),16};
    text.color = {1,0,0,1};
    auto vertices = glyph(440,40,false);
    const auto atlasRow = [&](float top) {
        for (std::size_t i=0; i<vertices.size(); i+=5) vertices[i+3] = top + vertices[i+3] * (16.0f/128.0f);
    };
    atlasRow(0); text.vertices=vertices.data(); text.vertexFloatCount=vertices.size();
    backend->drawText(text,width,height);
    vertices = glyph(480,40,true); atlasRow(0); backend->drawText(text,width,height);
    present();
    expectScreenPixel(hwnd,320,50,127,255,63,"RGB image tint");
    expectScreenPixel(hwnd,300,30,26,51,77,"rounded image corner");
    expectScreenPixel(hwnd,448,48,141,25,38,"gray glyph opacity");
    expectScreenPixel(hwnd,488,48,141,25,38,"color glyph premultiplied alpha");
    begin();
    text.grayAtlas.usedHeight = text.colorAtlas.usedHeight = 96;
    ++text.grayAtlas.generation; ++text.colorAtlas.generation;
    vertices = glyph(440,100,false); atlasRow(80.0f/128.0f); backend->drawText(text,width,height);
    vertices = glyph(480,100,true); atlasRow(80.0f/128.0f); backend->drawText(text,width,height);
    present();
    expectScreenPixel(hwnd,448,108,141,25,38,"gray atlas growth preserves source coordinates");
    expectScreenPixel(hwnd,488,108,141,25,38,"color atlas growth preserves source coordinates");
    backend->destroyTexture(texture);
    // Reuse a larger bitmap at smaller logical sizes: omission of the source
    // rectangle would scale the red/green markers and fail these pixel checks.
    const auto allocations = core::render::gpuResizeStats().renderCacheAllocations.load();
    RECT bounds{}; GetWindowRect(hwnd, &bounds);
    SetWindowPos(hwnd, nullptr, 0, 0, bounds.right-bounds.left-80, bounds.bottom-bounds.top-40,
                 SWP_NOMOVE | SWP_NOZORDER);
    host::framebufferSize(static_cast<host::Window*>(window), &width, &height);
    backend->beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
    check(backend->ensureRenderCache(width, height), "shrunk cache unavailable");
    backend->blitRenderCache(width, height); backend->present();
    host::pollEvents();
    check(backend->framePresented(), "shrunk frame did not present");
    expectScreenPixel(hwnd, 40,40,255,0,0,"shrunk bitmap is cropped without scaling");
    expectScreenPixel(hwnd, 140,40,0,255,0,"shrunk bitmap keeps marker coordinates");
    SetWindowPos(hwnd, nullptr, 0, 0, bounds.right-bounds.left, bounds.bottom-bounds.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    host::framebufferSize(static_cast<host::Window*>(window), &width, &height);
    backend->beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
    check(backend->ensureRenderCache(width, height), "re-expanded cache unavailable");
    backend->blitRenderCache(width, height); backend->present();
    host::pollEvents();
    expectScreenPixel(hwnd,40,40,255,0,0,"re-expanded bitmap keeps marker coordinates");
    check(core::render::gpuResizeStats().renderCacheAllocations.load() == allocations,
          "capacity-contained shrink/grow reallocated the bitmap");
#if defined(EUI_WINDOW_BACKEND_WIN32)
    // Deliberately omit a refresh callback to model a deferred complete frame.
    // WM_PAINT must cover only the newly exposed strips before EndPaint.
    // Under Hwnd presentation the prior frame lives in the presented D2D
    // surface rather than the GDI surface, so the two halves of this contract
    // are verified on different surfaces: strips on the GDI surface (the
    // presentation-agnostic WM_PAINT contract), prior content on the composed
    // screen (presented-surface continuity). Failures in either half are
    // reported; nothing is skipped.
    if (!presentation.gdiPresentation)
        std::fprintf(stderr, "note: deferred exposure under Hwnd presentation - strips are verified on the "
            "GDI surface (WM_PAINT contract), prior content on the composed screen (presented-surface continuity)\n");
    core::window::win32::setPaintBackground(static_cast<host::Window*>(window), RGB(17,23,31));
    SetWindowPos(hwnd, nullptr, 0, 0, bounds.right-bounds.left+40, bounds.bottom-bounds.top+40,
                 SWP_NOMOVE | SWP_NOZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    host::framebufferSize(static_cast<host::Window*>(window), &width, &height);
    expectGdiSurfacePixel(hwnd,width-10,40,17,23,31,"deferred right strip GDI fill");
    expectScreenPixel(hwnd,width-10,40,17,23,31,"deferred new right strip has theme background");
    expectGdiSurfacePixel(hwnd,40,height-10,17,23,31,"deferred bottom strip GDI fill");
    expectScreenPixel(hwnd,40,height-10,17,23,31,"deferred new bottom strip has theme background");
    expectScreenPixel(hwnd,40,40,255,0,0,"deferred fill preserves prior content");
#endif
    scissorPixelContracts(*backend,window,hwnd,width,height);
    backend->releaseRenderCache();
    {
        // Resource reconstruction is explicit; this is not an injected
        // D2DERR_RECREATE_TARGET or physical device-loss acceptance claim.
        auto rebuilt=core::render::createRenderBackend(window);
        check(rebuilt && rebuilt->initialize(),"scissor backend resource reconstruction");
        if (rebuilt && rebuilt->valid()) {
            core::render::ScopedRenderBackend rebuiltActive(*rebuilt);
            scissorPixelContracts(*rebuilt,window,hwnd,width,height);
        }
    }
    backend.reset();
    reportScissorCounts(core::render::currentRenderFrameStats(),0);
    core::window::destroyWindow(window); host::shutdownHost();
    std::printf("D2D pixel contracts arm %s: composed screen %d/%d, GDI surface %d/%d\n",
        presentation.name, screenChecks - screenFailures, screenChecks,
        gdiSurfaceChecks - gdiSurfaceFailures, gdiSurfaceChecks);
    if (!failures) std::puts("D2D pixel contracts: ALL PASS");
    std::printf("D2D ordered raw screen sample digest=%016llx (same-arm A/B diagnostic)\n",
        static_cast<unsigned long long>(screenSampleDigest));
    return failures ? 1 : 0;
}
#else
int main() { return 0; }
#endif
