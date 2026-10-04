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
void check(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}
void pixel(HWND hwnd, int x, int y, int r, int g, int b, const char* label) {
    GdiFlush(); Sleep(60);
    HDC dc = GetDC(hwnd);
    const COLORREF c = GetPixel(dc, x, y); ReleaseDC(hwnd, dc);
    const bool match = c != CLR_INVALID && std::abs(static_cast<int>(GetRValue(c)) - r) <= 4 &&
        std::abs(static_cast<int>(GetGValue(c)) - g) <= 4 && std::abs(static_cast<int>(GetBValue(c)) - b) <= 4;
    if (!match) std::fprintf(stderr, "%s: actual %d,%d,%d expected %d,%d,%d\n", label,
        GetRValue(c), GetGValue(c), GetBValue(c), r, g, b);
    check(match, label);
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
    };
    begin(); check(backend->renderCacheWasRecreated(), "first cache created");
    backend->clear({.1f,.2f,.3f,1});
    core::render::RoundedRectDrawCommand rect;
    rect.rect = {20,20,60,60}; rect.fillColor = {1,0,0,1};
    const auto geometry = core::render::roundedRectGeometryVertices(rect.rect, {}, {}, false, rect.rect);
    rect.vertices.assign(geometry.begin(), geometry.end());
    backend->drawRoundedRect(rect, width, height);
    present(); pixel(hwnd, 40,40,255,0,0,"initial rectangle");

    begin(); check(!backend->renderCacheWasRecreated(), "same size cache reused");
    backend->setScissor(true, {120,20,60,60}, height); backend->clear({0,1,0,1});
    present();
    pixel(hwnd,40,40,255,0,0,"dirty clear preserves prior pixels");
    pixel(hwnd,140,40,0,255,0,"dirty region cleared");
    pixel(hwnd,220,40,26,51,77,"dirty clear preserves background");

    begin();
    const unsigned char white[]{255,255,255,255};
    auto texture = backend->createTexture(white,1,1);
    check(texture != nullptr,"image created");
    const float image[]{300,30,1,300,30,0,0, 340,30,1,340,30,1,0, 340,70,1,340,70,1,1,
                        300,30,1,300,30,0,0, 340,70,1,340,70,1,1, 300,70,1,300,70,0,1};
    backend->drawTexture(texture,image,42,{.5f,1,.25f,1},{300,30,40,40},12,0,width,height);
    std::array<unsigned char,256> gray; gray.fill(128);
    std::array<unsigned char,1024> colored{};
    for (int i=0;i<256;++i) { colored[i*4]=128; colored[i*4+3]=128; }
    core::render::TextDrawCommand text;
    text.grayAtlas = {core::render::TextAtlasPageKind::Gray,16,16,1,1,gray.data()};
    text.colorAtlas = {core::render::TextAtlasPageKind::Color,16,16,4,1,colored.data()};
    text.color = {1,0,0,1};
    auto vertices = glyph(440,40,false); text.vertices=vertices.data(); text.vertexFloatCount=vertices.size();
    backend->drawText(text,width,height);
    vertices = glyph(480,40,true); backend->drawText(text,width,height);
    present();
    pixel(hwnd,320,50,127,255,63,"RGB image tint");
    pixel(hwnd,300,30,26,51,77,"rounded image corner");
    pixel(hwnd,448,48,141,25,38,"gray glyph opacity");
    pixel(hwnd,488,48,141,25,38,"color glyph premultiplied alpha");
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
    check(backend->framePresented(), "shrunk frame did not present");
    pixel(hwnd, 40,40,255,0,0,"shrunk bitmap is cropped without scaling");
    pixel(hwnd, 140,40,0,255,0,"shrunk bitmap keeps marker coordinates");
    SetWindowPos(hwnd, nullptr, 0, 0, bounds.right-bounds.left, bounds.bottom-bounds.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    host::framebufferSize(static_cast<host::Window*>(window), &width, &height);
    backend->beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
    check(backend->ensureRenderCache(width, height), "re-expanded cache unavailable");
    backend->blitRenderCache(width, height); backend->present();
    pixel(hwnd,40,40,255,0,0,"re-expanded bitmap keeps marker coordinates");
    check(core::render::gpuResizeStats().renderCacheAllocations.load() == allocations,
          "capacity-contained shrink/grow reallocated the bitmap");
#if defined(EUI_WINDOW_BACKEND_WIN32)
    // Deliberately omit a refresh callback to model a deferred complete frame.
    // WM_PAINT must cover only the newly exposed strips before EndPaint.
    core::window::win32::setPaintBackground(static_cast<host::Window*>(window), RGB(17,23,31));
    SetWindowPos(hwnd, nullptr, 0, 0, bounds.right-bounds.left+40, bounds.bottom-bounds.top+40,
                 SWP_NOMOVE | SWP_NOZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    host::framebufferSize(static_cast<host::Window*>(window), &width, &height);
    pixel(hwnd,width-10,40,17,23,31,"deferred new right strip has theme background");
    pixel(hwnd,40,height-10,17,23,31,"deferred new bottom strip has theme background");
    pixel(hwnd,40,40,255,0,0,"deferred fill preserves prior content");
#endif
    backend->releaseRenderCache(); backend.reset();
    core::window::destroyWindow(window); host::shutdownHost();
    if (!failures) std::puts("D2D pixel contracts: ALL PASS");
    return failures ? 1 : 0;
}
#else
int main() { return 0; }
#endif
