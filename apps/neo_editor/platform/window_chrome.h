#pragma once

#include "platform/system_appearance.h"
#include <algorithm>
#include <vector>

#if defined(_WIN32)
#include <dwmapi.h>
#endif

namespace neo::platform::chrome {

// Application-owned chrome; the backend still owns input, DPI, sizing and close
// callbacks. Keep WS_THICKFRAME / system styles for native resize and snapping.
inline constexpr float controlsWidth = 120;
inline constexpr float dragWidth = 64;
struct HitRect { float left, top, right, bottom; };
inline std::vector<HitRect> interactiveRects;
inline float logicalWidth = 1, titleHeight = 34;
inline void exclude(float x, float y, float width, float height) {
    if (width > 0 && height > 0) interactiveRects.push_back({x,y,x+width,y+height});
}
inline bool captionAt(float x, float y, float width, float height) {
    if (x < 0 || x >= width || y < 0 || y >= height) return false;
    for (const auto& r : interactiveRects)
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return false;
    return true;
}
inline void beginFrame(float width, float height, float interactiveRight = controlsWidth) {
    logicalWidth = width;
    titleHeight = height;
    interactiveRects.clear();
    exclude(width-interactiveRight, 0, interactiveRight, height);
}
// Full-page headers can be taller than the window buttons. Register their
// actual controls separately, so the lower half of a header also moves.
inline void extendCaptionTo(float bottom) { titleHeight = std::max(titleHeight, bottom); }
#if defined(_WIN32)
inline HWND window = nullptr;
inline bool dragEscapeArmed = false;
inline bool dragEscapePressed = false;
// API results are diagnostics, not proof that the compositor displayed a shadow.
inline HRESULT frameExtensionResult = E_PENDING, cornerPreferenceResult = E_PENDING;
// 1 = DWMWCP_DONOTROUND（Win10 直角），2 = DWMWCP_ROUND（Win11 圆角）。
// Win10 上属性 33 本来就返回 E_INVALIDARG，窗口保持方角；这里显式请求 1。
inline bool squareCorners = false;
inline void applyDwmFrame(HWND hwnd) {
    static const auto module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using ExtendFrame = HRESULT(WINAPI*)(HWND, const MARGINS*);
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static const auto extendFrame = module ? reinterpret_cast<ExtendFrame>(
        GetProcAddress(module, "DwmExtendFrameIntoClientArea")) : nullptr;
    static const auto setAttribute = module ? reinterpret_cast<SetAttribute>(
        GetProcAddress(module, "DwmSetWindowAttribute")) : nullptr;
    // NCCALCSIZE removes the standard frame. Keep a minimal DWM frame on
    // Windows 10 as well; the Windows 11 corner preference alone cannot do this.
    // The client renderer stays opaque; do not use a full-window glass margin.
    const MARGINS margins{1, 1, 1, 1};
    frameExtensionResult = extendFrame ? extendFrame(hwnd, &margins) : E_NOTIMPL;
    const DWORD round = squareCorners ? 1u : 2u;
    cornerPreferenceResult = setAttribute ? setAttribute(hwnd, 33, &round, sizeof(round)) : E_NOTIMPL;
}
inline void setSquareCorners(bool square) {
    if (squareCorners == square) return;
    squareCorners = square;
    if (window) applyDwmFrame(window);
}
inline void armTabDragEscape(bool armed) {
    dragEscapeArmed = armed;
    if (!armed) dragEscapePressed = false;
}
inline bool tabDragEscapePressed() { return dragEscapePressed; }

inline LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    // Undocumented UAH caption/frame paint messages (also handled by Chromium's
    // custom frame). Forwarding them can paint native chrome over our buttons
    // even after NCCALCSIZE removed the standard title bar. Keep ordinary DWM,
    // activation and input messages on the backend's existing path.
    constexpr UINT ncUahDrawCaption = 0xAE, ncUahDrawFrame = 0xAF;
    if (msg == ncUahDrawCaption || msg == ncUahDrawFrame) return 0;
    if (msg == WM_KEYDOWN && w == VK_ESCAPE && dragEscapeArmed) {
        dragEscapePressed = true;
        return 0;
    }
    if (msg == WM_ACTIVATE || msg == WM_DWMCOMPOSITIONCHANGED || msg == WM_THEMECHANGED)
        applyDwmFrame(hwnd);
    if (msg == WM_NCCALCSIZE) {
        auto& r = w ? reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0]
                    : *reinterpret_cast<RECT*>(l);
        if (IsZoomed(hwnd)) {
            const UINT dpi = GetDpiForWindow(hwnd);
            const int x = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int y = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            InflateRect(&r, -x, -y);
        }
        return 0;
    }
    if (msg == WM_NCHITTEST) {
        POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        ScreenToClient(hwnd, &p);
        RECT r{}; GetClientRect(hwnd, &r);
        if (p.x < 0 || p.y < 0 || p.x >= r.right || p.y >= r.bottom) return HTNOWHERE;
        const float scale = r.right / std::max(1.0f, logicalWidth);
        const int edge = std::max(4, static_cast<int>(6 * GetDpiForWindow(hwnd) / 96.0f));
        if (!IsZoomed(hwnd)) {
            const bool left=p.x<edge, right=p.x>=r.right-edge, top=p.y<edge, bottom=p.y>=r.bottom-edge;
            if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
            if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
        }
        const float x = p.x / std::max(.1f, scale), y = p.y / std::max(.1f, scale);
        if (captionAt(x, y, logicalWidth, titleHeight)) return HTCAPTION;
        return HTCLIENT;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, procedure, id);
        if (window == hwnd) window = nullptr;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}
inline void update(void* native, float width, float height, float interactiveRight = controlsWidth) {
    beginFrame(width, height, interactiveRight);
    auto hwnd=static_cast<HWND>(native);
    if (!hwnd || window==hwnd) return;
    if (!SetWindowSubclass(hwnd, procedure, 0x4E454F43, 0)) return;
    window=hwnd;
    applyDwmFrame(hwnd);
    SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}
inline bool maximized() { return window && IsZoomed(window); }
inline bool foreground() { return !window || GetForegroundWindow() == window; }
inline bool alwaysOnTop() {
    return window && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}
inline bool setAlwaysOnTop(bool enabled) {
    return window && SetWindowPos(window, enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
}
// Keep a scrollbar's center inside client input, at the current system DPI
// and UI scale. Its outer half already contributes to the native sizing
// allowance. Maximized windows have no sizing strip.
inline float resizeInset(float scrollbarWidth = 0.0f) {
    if (!window || IsZoomed(window)) return 0;
    RECT r{}; GetClientRect(window, &r);
    const float scale = r.right / std::max(1.0f, logicalWidth);
    const int edge = std::max(4, static_cast<int>(6 * GetDpiForWindow(window) / 96.0f));
    return std::max(0.0f, (edge + 1.0f) / std::max(.1f, scale) -
        std::max(0.0f, scrollbarWidth) * 0.5f);
}
inline void minimize() { if(window) PostMessageW(window,WM_SYSCOMMAND,SC_MINIMIZE,0); }
inline void maximize() { if(window) PostMessageW(window,WM_SYSCOMMAND,maximized()?SC_RESTORE:SC_MAXIMIZE,0); }
inline void close() { if(window) PostMessageW(window,WM_CLOSE,0,0); }
#else
inline void armTabDragEscape(bool) {}
inline bool tabDragEscapePressed() { return false; }
inline void setSquareCorners(bool) {}
inline void update(void*, float width, float height, float interactiveRight = controlsWidth) {
    beginFrame(width, height, interactiveRight);
}
inline bool maximized() { return false; }
inline bool foreground() { return true; }
inline bool alwaysOnTop() { return false; }
inline bool setAlwaysOnTop(bool) { return false; }
inline float resizeInset(float = 0.0f) { return 0; }
inline void minimize() {}
inline void maximize() {}
inline void close() {}
#endif
} // namespace neo::platform::chrome
