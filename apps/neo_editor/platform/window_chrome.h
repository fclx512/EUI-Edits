#pragma once

#include "platform/system_appearance.h"
#include <algorithm>
#include <vector>

namespace neo::platform::chrome {

// Application-owned chrome; the backend still owns input, DPI, sizing and close
// callbacks. Keep WS_THICKFRAME / system styles for native resize and snapping.
inline constexpr float controlsWidth = 120;
inline constexpr float dragWidth = 64;
struct HitRect { float left, top, right, bottom; };
inline std::vector<HitRect> interactiveRects;
inline void exclude(float x, float y, float width, float height) {
    if (width > 0 && height > 0) interactiveRects.push_back({x,y,x+width,y+height});
}
inline bool captionAt(float x, float y, float width, float height) {
    if (x < 0 || x >= width || y < 0 || y >= height) return false;
    for (const auto& r : interactiveRects)
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return false;
    return true;
}
#if defined(_WIN32)
inline HWND window = nullptr;
inline float logicalWidth = 1, titleHeight = 34;
inline float rightInteractiveWidth = controlsWidth;
inline bool broadDrag = false;
inline bool dragEscapeArmed = false;
inline bool dragEscapePressed = false;
inline void armTabDragEscape(bool armed) {
    dragEscapeArmed = armed;
    if (!armed) dragEscapePressed = false;
}
inline bool tabDragEscapePressed() { return dragEscapePressed; }

inline LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_KEYDOWN && w == VK_ESCAPE && dragEscapeArmed) {
        dragEscapePressed = true;
        return 0;
    }
    if (msg == WM_NCCALCSIZE && w) {
        if (IsZoomed(hwnd)) {
            auto& r = reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0];
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
inline void update(void* native, float width, float height, bool broad, float interactiveRight = controlsWidth) {
    logicalWidth=width; titleHeight=height; broadDrag=broad;
    rightInteractiveWidth=interactiveRight;
    interactiveRects.clear();
    exclude(width-interactiveRight, 0, interactiveRight, height);
    auto hwnd=static_cast<HWND>(native);
    if (!hwnd || window==hwnd) return;
    if (!SetWindowSubclass(hwnd, procedure, 0x4E454F43, 0)) return;
    window=hwnd;
    // Windows 11's DWM owns antialiased corners and the shadow. Older systems
    // reject attribute 33 harmlessly; no window region or layered-window mask.
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static const auto setAttribute = reinterpret_cast<SetAttribute>(
        GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"));
    if (setAttribute) { const DWORD round = 2; setAttribute(hwnd, 33, &round, sizeof(round)); }
    SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}
inline bool maximized() { return window && IsZoomed(window); }
inline bool foreground() { return !window || GetForegroundWindow() == window; }
inline void minimize() { if(window) PostMessageW(window,WM_SYSCOMMAND,SC_MINIMIZE,0); }
inline void maximize() { if(window) PostMessageW(window,WM_SYSCOMMAND,maximized()?SC_RESTORE:SC_MAXIMIZE,0); }
inline void close() { if(window) PostMessageW(window,WM_CLOSE,0,0); }
#else
inline void armTabDragEscape(bool) {}
inline bool tabDragEscapePressed() { return false; }
inline void update(void*,float,float,bool,float=controlsWidth) {}
inline bool maximized() { return false; }
inline bool foreground() { return true; }
inline void minimize() {}
inline void maximize() {}
inline void close() {}
#endif
} // namespace neo::platform::chrome
