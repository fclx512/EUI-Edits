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

namespace {
int failures = 0;
// Contract groups. The composed-screen group decides whether a presented D2D
// frame actually reaches the desktop; the GDI-surface group decides whether
// the WM_PAINT handler filled the exposed strips before EndPaint. Neither
// group is skipped, and a failure in either fails the run.
int screenChecks = 0, screenFailures = 0;
int gdiSurfaceChecks = 0, gdiSurfaceFailures = 0;

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
    backend->releaseRenderCache(); backend.reset();
    core::window::destroyWindow(window); host::shutdownHost();
    std::printf("D2D pixel contracts arm %s: composed screen %d/%d, GDI surface %d/%d\n",
        presentation.name, screenChecks - screenFailures, screenChecks,
        gdiSurfaceChecks - gdiSurfaceFailures, gdiSurfaceChecks);
    if (!failures) std::puts("D2D pixel contracts: ALL PASS");
    return failures ? 1 : 0;
}
#else
int main() { return 0; }
#endif
