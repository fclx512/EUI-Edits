#include "core/window/win32_host.h"
#include "core/input/input_state.h"
#include "core/platform/platform.h"
#include "core/render/resize_trace.h"
#include <windowsx.h>
#include <imm.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace core::window {
namespace {
struct State {
    WindowCreateRequest request;
    std::vector<std::string> droppedPaths;
    void* user = nullptr;
    bool close = false, input = false, composing = false, placingIme = false;
    bool cancellingIme = false, cancelledIme = false, discardCancelledChars = false, focusLost = false;
    wchar_t surrogate = 0;
    HCURSOR cursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    HICON icon = nullptr;
    RECT ime{};
    bool sizing = false, paintPending = false, liveResize = true;
    int coveredWidth = 0, coveredHeight = 0;
    COLORREF paintBackground = RGB(245, 245, 245);
    double refreshDeadline = 0;
    std::uint64_t sizeGeneration = 0, presentedGeneration = 0;
    void (*size)(void*, int, int) = nullptr;
    void (*refresh)(void*) = nullptr;
    void (*scale)(void*, float, float) = nullptr;
    void (*focus)(void*, int) = nullptr;
    void (*iconify)(void*, int) = nullptr;
    void (*closing)(void*) = nullptr;
    ~State() { if (icon) DestroyIcon(icon); }
};
std::unordered_map<HWND, std::unique_ptr<State>> states;
DWORD uiThread = 0;
Handle mainHandle = nullptr;
constexpr UINT wakeMessage = WM_APP + 0x317;
constexpr UINT_PTR resizeTimer = 0x317;
State* state(void* window) {
    auto i = states.find(static_cast<HWND>(window));
    return i == states.end() ? nullptr : i->second.get();
}
std::wstring wide(const std::string& text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
std::string utf8(const wchar_t* text, int count) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, count, nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, text, count, out.data(), n, nullptr, nullptr);
    return out;
}
KeyModifiers modifiers() {
    return {(GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetKeyState(VK_MENU) & 0x8000) != 0,
        ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0,
        (GetKeyState(VK_CAPITAL) & 1) != 0, (GetKeyState(VK_NUMLOCK) & 1) != 0};
}
InputKey key(WPARAM vk, LPARAM bits) {
    using K = InputKey;
    if (vk >= 'A' && vk <= 'Z') return static_cast<K>(static_cast<int>(K::A) + vk - 'A');
    if (vk >= '0' && vk <= '9') return static_cast<K>(static_cast<int>(K::Digit0) + vk - '0');
    if (vk >= VK_F1 && vk <= VK_F24) return static_cast<K>(static_cast<int>(K::F1) + vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return static_cast<K>(static_cast<int>(K::Numpad0) + vk - VK_NUMPAD0);
    const bool extended = (bits & (1 << 24)) != 0;
    switch (vk) {
    case VK_BACK: return K::Backspace; case VK_TAB: return K::Tab;
    case VK_RETURN: return extended ? K::NumpadEnter : K::Enter;
    case VK_ESCAPE: return K::Escape; case VK_SPACE: return K::Space;
    case VK_INSERT: return K::Insert; case VK_DELETE: return K::Delete;
    case VK_HOME: return K::Home; case VK_END: return K::End;
    case VK_PRIOR: return K::PageUp; case VK_NEXT: return K::PageDown;
    case VK_LEFT: return K::Left; case VK_RIGHT: return K::Right;
    case VK_UP: return K::Up; case VK_DOWN: return K::Down;
    case VK_SHIFT: return MapVirtualKeyW((bits >> 16) & 0xff, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? K::RightShift : K::LeftShift;
    case VK_CONTROL: return extended ? K::RightControl : K::LeftControl;
    case VK_MENU: return extended ? K::RightAlt : K::LeftAlt;
    case VK_LWIN: return K::LeftSuper; case VK_RWIN: return K::RightSuper;
    case VK_APPS: return K::Menu; case VK_CAPITAL: return K::CapsLock;
    case VK_NUMLOCK: return K::NumLock; case VK_SCROLL: return K::ScrollLock;
    case VK_SNAPSHOT: return K::PrintScreen; case VK_PAUSE: return K::Pause;
    case VK_DECIMAL: return K::NumpadDecimal; case VK_DIVIDE: return K::NumpadDivide;
    case VK_MULTIPLY: return K::NumpadMultiply; case VK_SUBTRACT: return K::NumpadSubtract;
    case VK_ADD: return K::NumpadAdd;
    case VK_OEM_1: return K::Semicolon; case VK_OEM_PLUS: return K::Equal;
    case VK_OEM_COMMA: return K::Comma; case VK_OEM_MINUS: return K::Minus;
    case VK_OEM_PERIOD: return K::Period; case VK_OEM_2: return K::Slash;
    case VK_OEM_3: return K::GraveAccent; case VK_OEM_4: return K::LeftBracket;
    case VK_OEM_5: return K::Backslash; case VK_OEM_6: return K::RightBracket;
    case VK_OEM_7: return K::Apostrophe;
    default: return K::Unknown;
    }
}
std::string imeString(HWND hwnd, DWORD flag) {
    HIMC context = ImmGetContext(hwnd);
    if (!context) return {};
    LONG bytes = ImmGetCompositionStringW(context, flag, nullptr, 0);
    std::wstring text(bytes > 0 ? bytes / sizeof(wchar_t) : 0, 0);
    if (bytes > 0) ImmGetCompositionStringW(context, flag, text.data(), bytes);
    ImmReleaseContext(hwnd, context);
    return utf8(text.data(), static_cast<int>(text.size()));
}
void placeIme(HWND hwnd, State& s) {
    if (s.placingIme) return;
    s.placingIme = true;
    struct Guard { bool& flag; ~Guard() { flag = false; } } guard{s.placingIme};
    HIMC context = ImmGetContext(hwnd);
    if (!context) return;
    COMPOSITIONFORM form{}; form.dwStyle = CFS_FORCE_POSITION;
    form.ptCurrentPos = {s.ime.left, s.ime.top}; form.rcArea = s.ime;
    ImmSetCompositionWindow(context, &form);
    CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = form.ptCurrentPos; candidate.rcArea = s.ime;
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(hwnd, context);
}
void cancelIme(HWND hwnd, State& s) {
    if (s.cancellingIme) return;
    if (!s.composing && !core::detail::isComposing(hwnd) && core::detail::compositionText(hwnd).empty()) {
        core::queueTextEditing(hwnd, {});
        return;
    }
    s.cancellingIme = true;
    s.cancelledIme = true;
    s.discardCancelledChars = true;
    s.composing = false;
    s.surrogate = 0;
    core::queueTextEditing(hwnd, {});
    HIMC context = ImmGetContext(hwnd);
    if (context) {
        // An IME may synchronously send composition/result messages here. The
        // cancellation latch is already set, so they cannot become document text.
        ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(hwnd, context);
    }
    // Remove messages already posted for the cancelled session. The native API
    // provides no generation identifier for these messages; future result-only
    // messages remain rejected until a fresh STARTCOMPOSITION.
    MSG pending{};
    while (PeekMessageW(&pending, hwnd, WM_IME_STARTCOMPOSITION, WM_IME_COMPOSITION, PM_REMOVE)) {}
    while (PeekMessageW(&pending, hwnd, WM_IME_CHAR, WM_IME_CHAR, PM_REMOVE)) {}
    core::queueTextEditing(hwnd, {});
    s.cancellingIme = false;
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        s = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, message, w, l);
    switch (message) {
    case WM_DROPFILES: {
        const auto drop=reinterpret_cast<HDROP>(w);
        const UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);
        // The application is single-document: retain one path rather than silently replacing several.
        if(count) {const UINT n=DragQueryFileW(drop,0,nullptr,0);std::vector<wchar_t> path(n+1);
            DragQueryFileW(drop,0,path.data(),n+1);s->droppedPaths.push_back(utf8(path.data(),static_cast<int>(n)));}
        DragFinish(drop);core::platform::requestUiUpdate();return 0;
    }
    case WM_CLOSE:
        s->close = true; if (s->closing) s->closing(hwnd); return 0;
    // No claim of background coverage before the paint path actually fills it.
    case WM_ERASEBKGND: return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
        s->paintPending = true;
        if (s->refresh) s->refresh(hwnd);
        RECT client{}; GetClientRect(hwnd, &client);
        // A deferred frame leaves old valid pixels intact; only newly exposed
        // strips are filled. BeginPaint clips these fills to the update region.
        const LONG coveredW = std::min<LONG>(s->coveredWidth, client.right);
        const LONG coveredH = std::min<LONG>(s->coveredHeight, client.bottom);
        RECT strips[]{{coveredW, 0, client.right, client.bottom},
                      {0, coveredH, coveredW, client.bottom}};
        HBRUSH brush = nullptr;
        for (const auto& strip : strips) {
            RECT exposed{};
            if (IntersectRect(&exposed, &strip, &paint.rcPaint)) {
                if (!brush) brush = CreateSolidBrush(s->paintBackground);
                if (brush) FillRect(dc, &exposed, brush);
                core::render::resizeTrace().record("cover", 0, exposed.right-exposed.left, exposed.bottom-exposed.top);
            }
        }
        if (brush) DeleteObject(brush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_SIZE:
        ++s->sizeGeneration; s->paintPending = true;
        s->coveredWidth = std::min(s->coveredWidth, static_cast<int>(LOWORD(l)));
        s->coveredHeight = std::min(s->coveredHeight, static_cast<int>(HIWORD(l)));
        core::render::resizeTrace().record("size", 0, LOWORD(l), HIWORD(l));
        if (s->iconify) s->iconify(hwnd, w == SIZE_MINIMIZED);
        if (s->size) s->size(hwnd, LOWORD(l), HIWORD(l)); return 0;
    case WM_ENTERSIZEMOVE:
        s->sizing = true;
        core::render::resizeTrace().record("drag-enter", 0);
        return 0;
    case WM_EXITSIZEMOVE:
        s->sizing = false; s->paintPending = true;
        win32::cancelRefresh(hwnd);
        core::render::resizeTrace().record("drag-exit", 0);
        if (s->refresh) s->refresh(hwnd); return 0;
    case WM_TIMER:
        if (w == resizeTimer) {
            win32::cancelRefresh(hwnd);
            if (s->paintPending && s->refresh && !IsIconic(hwnd)) s->refresh(hwnd);
            return 0;
        } break;
    case WM_DPICHANGED: {
        if (s->scale) s->scale(hwnd, LOWORD(w) / 96.0f, HIWORD(w) / 96.0f);
        const RECT* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* m = reinterpret_cast<MINMAXINFO*>(l); const auto& r = s->request;
        const float dpi = GetDpiForWindow(hwnd) / 96.0f;
        RECT bounds{0,0,static_cast<LONG>(r.minWidth*dpi),static_cast<LONG>(r.minHeight*dpi)};
        AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)), GetDpiForWindow(hwnd));
        if (r.minWidth > 0) m->ptMinTrackSize.x = bounds.right-bounds.left;
        if (r.minHeight > 0) m->ptMinTrackSize.y = bounds.bottom-bounds.top;
        bounds = {0,0,static_cast<LONG>(r.maxWidth*dpi),static_cast<LONG>(r.maxHeight*dpi)};
        AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)), GetDpiForWindow(hwnd));
        if (r.maxWidth > 0) m->ptMaxTrackSize.x = bounds.right-bounds.left;
        if (r.maxHeight > 0) m->ptMaxTrackSize.y = bounds.bottom-bounds.top;
        return 0;
    }
    case WM_ACTIVATE:
        // Deactivation precedes the default focus/context transfer on this HWND.
        // Cancel here, before an IME can finalize raw preedit during that transfer.
        s->focusLost = LOWORD(w) == WA_INACTIVE || HIWORD(w) != 0;
        if (s->focusLost) cancelIme(hwnd, *s);
        break;
    case WM_ACTIVATEAPP:
        s->focusLost = !w;
        if (s->focusLost) cancelIme(hwnd, *s);
        break;
    case WM_SETFOCUS: s->focusLost = false; if (s->focus) s->focus(hwnd,1); break;
    case WM_KILLFOCUS:
        s->focusLost = true;
        cancelIme(hwnd, *s);
        if (s->input) core::cancelInput(hwnd);
        if (s->focus) s->focus(hwnd,0); break;
    case WM_IME_SETCONTEXT:
        if (!w && s->input) cancelIme(hwnd, *s);
        // The component draws preedit inline; retain only native candidate UI.
        l &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
        return DefWindowProcW(hwnd, message, w, l);
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(s->cursor); return TRUE; } break;
    default: break;
    }
    if (!s->input) return DefWindowProcW(hwnd, message, w, l);
    switch (message) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&tracking);
        core::queuePointerPresence(hwnd, true);
        core::queuePointerMotion(hwnd, GET_X_LPARAM(l), GET_Y_LPARAM(l), core::detail::pointerState(hwnd).buttons, modifiers()); return 0;
    }
    case WM_MOUSELEAVE: core::queuePointerPresence(hwnd,false); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
        const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
        PointerButton button = PointerButton::Middle;
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) button = PointerButton::Left;
        if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) button = PointerButton::Right;
        if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP) button = GET_XBUTTON_WPARAM(w) == XBUTTON1 ? PointerButton::X1 : PointerButton::X2;
        if (down) SetCapture(hwnd);
        core::queuePointerButton(hwnd, GET_X_LPARAM(l), GET_Y_LPARAM(l), button,
            down ? PointerAction::Press : PointerAction::Release, modifiers());
        if (!down && core::detail::pointerState(hwnd).buttons.empty()) ReleaseCapture();
        return TRUE;
    }
    case WM_CAPTURECHANGED: if (reinterpret_cast<HWND>(l) != hwnd) core::cancelInput(hwnd); return 0;
    case WM_MOUSEWHEEL: core::queueScrollInput(hwnd,0,GET_WHEEL_DELTA_WPARAM(w)/120.0); return 0;
    case WM_MOUSEHWHEEL: core::queueScrollInput(hwnd,GET_WHEEL_DELTA_WPARAM(w)/120.0,0); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        // START can precede the first COMPSTR, and Runtime dispatches keys before
        // text callbacks. Keep candidate keys out of the application queue even
        // when the preedit string is still empty; let native IME/system handling
        // run through DefWindowProc (including Alt+F4).
        if (s->composing || s->focusLost || s->cancellingIme) break;
        const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
        if (!up && !s->focusLost && !s->cancellingIme) s->discardCancelledChars = false;
        core::queueKeyInput(hwnd,{key(w,l),up ? KeyAction::Release : ((l & (1<<30)) ? KeyAction::Repeat : KeyAction::Press), modifiers(),static_cast<int>((l>>16)&0xff)});
        // Preserve native Alt+F4/menu system commands.
        if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) break;
        return 0;
    }
    case WM_CHAR: {
        if (s->focusLost || s->discardCancelledChars || s->cancellingIme) return 0;
        const wchar_t ch = static_cast<wchar_t>(w);
        if (ch < 32 || ch == 127) return 0;
        if (ch >= 0xd800 && ch <= 0xdbff) { s->surrogate = ch; return 0; }
        if (ch >= 0xdc00 && ch <= 0xdfff && s->surrogate) {
            wchar_t pair[]{s->surrogate,ch}; s->surrogate = 0; core::queueTextInput(hwnd,utf8(pair,2));
        } else { s->surrogate = 0; core::queueTextInput(hwnd,utf8(&ch,1)); }
        return 0;
    }
    case WM_UNICHAR:
        if (w == UNICODE_NOCHAR) return TRUE;
        if (s->focusLost || s->discardCancelledChars || s->cancellingIme) return 0;
        { std::string text; core::detail::appendUtf8(text,static_cast<unsigned int>(w)); core::queueTextInput(hwnd,text); } return 0;
    case WM_IME_CHAR:
        // Result strings are handled through GCS_RESULTSTR; never ask the default
        // window procedure to translate this duplicate route into WM_CHAR.
        return 0;
    case WM_IME_STARTCOMPOSITION:
        if (s->focusLost || s->cancellingIme) return 0;
        s->cancelledIme = false; s->discardCancelledChars = false;
        s->composing = true; placeIme(hwnd,*s); return 0;
    case WM_IME_COMPOSITION:
        if (s->focusLost || s->cancelledIme || s->cancellingIme) return 0;
        placeIme(hwnd,*s);
        if (l == 0) { s->composing = false; core::queueTextEditing(hwnd,{}); return 0; }
        if (l & GCS_RESULTSTR) { core::queueTextInput(hwnd,imeString(hwnd,GCS_RESULTSTR)); s->composing = false; }
        if (l & GCS_COMPSTR) { core::queueTextEditing(hwnd,imeString(hwnd,GCS_COMPSTR)); s->composing = true; }
        // The result was consumed above; DefWindowProc would deliver it a second time.
        return 0;
    case WM_IME_ENDCOMPOSITION: s->composing = false; core::queueTextEditing(hwnd,{}); return 0;
    case WM_IME_NOTIFY: placeIme(hwnd,*s); break;
    default: break;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
}

Handle createWindow(const WindowCreateRequest& request) {
    if (request.renderApi != RenderApi::Native || !win32::initialize()) return nullptr;
    auto s = std::make_unique<State>(); s->request = request;
    DWORD style = request.decorated ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    if (!request.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    DWORD ex = request.alwaysOnTop ? WS_EX_TOPMOST : 0;
    UINT dpi = GetDpiForSystem();
    RECT r{0,0,MulDiv(request.width,dpi,96),MulDiv(request.height,dpi,96)};
    AdjustWindowRectExForDpi(&r,style,FALSE,ex,dpi);
    const auto name = wide(request.title ? request.title : "");
    HWND hwnd = CreateWindowExW(ex,L"EuiNeoWin32",name.c_str(),style,
        request.positionSet ? request.x : CW_USEDEFAULT, request.positionSet ? request.y : CW_USEDEFAULT,
        r.right-r.left,r.bottom-r.top,static_cast<HWND>(request.parent),nullptr,GetModuleHandleW(nullptr),s.get());
    if (!hwnd) return nullptr;
    states.emplace(hwnd,std::move(s));
    if(!request.parent) DragAcceptFiles(hwnd,TRUE);
    ShowWindow(hwnd,request.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    return hwnd;
}
void destroyWindow(Handle window) {
    auto hwnd = static_cast<HWND>(window);
    win32::cancelRefresh(hwnd);
    // Teardown messages must not recreate an input queue or call a retired runner.
    if (auto s = state(window)) {
        s->input = false; s->user = nullptr;
        s->size = nullptr; s->refresh = nullptr; s->scale = nullptr;
        s->focus = nullptr; s->iconify = nullptr; s->closing = nullptr;
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    states.erase(hwnd);
}
NativeWindowInfo nativeWindowInfo(Handle window) { return {window,window,nullptr,nullptr}; }
ContextKey currentContextKey() { return nullptr; }
double timeSeconds() { using Clock=std::chrono::steady_clock; static auto start=Clock::now(); return std::chrono::duration<double>(Clock::now()-start).count(); }
void postEmptyEvent() { if (uiThread) PostThreadMessageW(uiThread,wakeMessage,0,0); }
void getCursorPosition(Handle window,double& x,double& y) { POINT p{}; GetCursorPos(&p); ScreenToClient(static_cast<HWND>(window),&p); x=p.x; y=p.y; }
std::string clipboardText(Handle window) {
    if (!OpenClipboard(static_cast<HWND>(window))) return {};
    std::string result; HANDLE data=GetClipboardData(CF_UNICODETEXT);
    if (data) { auto text=static_cast<const wchar_t*>(GlobalLock(data)); if(text) { result=utf8(text,static_cast<int>(wcslen(text))); GlobalUnlock(data); } }
    CloseClipboard(); return result;
}
void setClipboardText(const std::string& text) {
    const auto value=wide(text); HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
    if (!data) return; void* buffer=GlobalLock(data); if(!buffer) {GlobalFree(data);return;}
    memcpy(buffer,value.c_str(),(value.size()+1)*sizeof(wchar_t)); GlobalUnlock(data);
    if (!OpenClipboard(static_cast<HWND>(mainHandle))) {GlobalFree(data);return;}
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT,data)) GlobalFree(data);
    CloseClipboard();
}
Handle mainWindowHandle() { return mainHandle; }
void setMainWindowHandle(Handle window) { mainHandle=window; }
CursorHandle createStandardCursor(CursorType type) { return LoadCursorW(nullptr,type==CursorType::Hand ? MAKEINTRESOURCEW(32649) : type==CursorType::IBeam ? MAKEINTRESOURCEW(32513) : MAKEINTRESOURCEW(32512)); }
void setCursor(Handle window,CursorHandle cursor) { if(auto s=state(window)) s->cursor=static_cast<HCURSOR>(cursor); SetCursor(static_cast<HCURSOR>(cursor)); }
void destroyCursor(CursorHandle) {} // Shared system cursor.
void setWindowIcon(Handle window,int width,int height,unsigned char* pixels) {
    auto s=state(window); if(!s || !pixels || width<=0 || height<=0) return;
    BITMAPV5HEADER header{}; header.bV5Size=sizeof(header); header.bV5Width=width; header.bV5Height=-height;
    header.bV5Planes=1; header.bV5BitCount=32; header.bV5Compression=BI_BITFIELDS;
    header.bV5RedMask=0x00ff0000; header.bV5GreenMask=0x0000ff00; header.bV5BlueMask=0x000000ff; header.bV5AlphaMask=0xff000000;
    void* bits=nullptr; HDC dc=GetDC(nullptr); HBITMAP bitmap=CreateDIBSection(dc,reinterpret_cast<BITMAPINFO*>(&header),DIB_RGB_COLORS,&bits,nullptr,0); ReleaseDC(nullptr,dc);
    if(!bitmap) return;
    auto dest=static_cast<unsigned char*>(bits);
    for(std::size_t i=0;i<static_cast<std::size_t>(width)*height*4;i+=4) {dest[i]=pixels[i+2];dest[i+1]=pixels[i+1];dest[i+2]=pixels[i];dest[i+3]=pixels[i+3];}
    HBITMAP mask=CreateBitmap(width,height,1,1,nullptr); ICONINFO info{TRUE,0,0,mask,bitmap}; HICON icon=CreateIconIndirect(&info);
    DeleteObject(mask); DeleteObject(bitmap); if(!icon) return;
    SendMessageW(static_cast<HWND>(window),WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));
    SendMessageW(static_cast<HWND>(window),WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icon));
    if(s->icon) DestroyIcon(s->icon); s->icon=icon;
}
void setImeCursorRect(Handle window,float x,float y,float width,float height) {
    if(auto s=state(window)) {s->ime={static_cast<LONG>(x),static_cast<LONG>(y),static_cast<LONG>(x+width),static_cast<LONG>(y+height)};placeIme(static_cast<HWND>(window),*s);}
}
std::vector<std::string> consumeDroppedPaths(Handle window) {if(auto s=state(window)) {auto paths=std::move(s->droppedPaths);s->droppedPaths.clear();return paths;}return {}; }
void installInputCallbacks(Handle window) { if(auto s=state(window)) s->input=true; }
void uninstallInputCallbacks(Handle window) { if(auto s=state(window)) s->input=false; }
bool queryImeComposition(Handle window,std::string& text,bool& composing) {
    auto s = state(window);
    composing = s && s->composing && !s->cancelledIme && !s->focusLost;
    text = composing ? imeString(static_cast<HWND>(window), GCS_COMPSTR) : std::string{};
    return true;
}
void cancelImeComposition(Handle window) { if (auto s = state(window)) cancelIme(static_cast<HWND>(window), *s); }

namespace win32 {
bool initialize() {
    if(uiThread) return true;
    uiThread=GetCurrentThreadId();
    MSG message{}; PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.lpfnWndProc=procedure; cls.hInstance=GetModuleHandleW(nullptr);
    cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)); cls.lpszClassName=L"EuiNeoWin32";
    // Prefer the executable's ICO frames over rescaling a large runtime PNG.
    // Shared resource handles live with the module and need no DestroyIcon.
    const UINT dpi = GetDpiForSystem();
    cls.hIcon = static_cast<HICON>(LoadImageW(cls.hInstance, L"IDI_APP_ICON", IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_SHARED));
    cls.hIconSm = static_cast<HICON>(LoadImageW(cls.hInstance, L"IDI_APP_ICON", IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_SHARED));
    if (!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) {uiThread=0;return false;}
    return true;
}
void shutdownHost() { while(!states.empty()) destroyWindow(states.begin()->first); mainHandle=nullptr; }
void pollEvents() {
    // Yield to the desktop frame loop even if producers keep the native queue
    // nonempty. Preserve FIFO and leave excess messages for the next pump.
    // The time budget is soft: a single native/modal handler may run longer.
    constexpr unsigned maxMessages = 256;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    MSG message{};
    unsigned processed = 0;
    while (processed < maxMessages && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        ++processed;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
}
void waitEventsTimeout(double seconds) {const auto ms=static_cast<DWORD>(std::clamp(std::ceil(seconds*1000),0.0,static_cast<double>(INFINITE-1))); MsgWaitForMultipleObjectsEx(0,nullptr,ms,QS_ALLINPUT,MWMO_INPUTAVAILABLE);pollEvents();}
void waitEvents() {MsgWaitForMultipleObjectsEx(0,nullptr,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);pollEvents();}
bool shouldClose(Window* window) {auto s=state(window);return !s || s->close;}
void setShouldClose(Window* window,int close) {if(auto s=state(window))s->close=close!=0;}
void setUserPointer(Window* window,void* data) {if(auto s=state(window))s->user=data;}
void* userPointer(Window* window) {auto s=state(window);return s?s->user:nullptr;}
void setSizeCallback(Window* w,void(*f)(Window*,int,int)){if(auto s=state(w))s->size=f;}
void setRefreshCallback(Window* w,void(*f)(Window*)){if(auto s=state(w))s->refresh=f;}
void setScaleCallback(Window* w,void(*f)(Window*,float,float)){if(auto s=state(w))s->scale=f;}
void setFocusCallback(Window* w,void(*f)(Window*,int)){if(auto s=state(w))s->focus=f;}
void setIconifyCallback(Window* w,void(*f)(Window*,int)){if(auto s=state(w))s->iconify=f;}
void setCloseCallback(Window* w,void(*f)(Window*)){if(auto s=state(w))s->closing=f;}
void contentScale(Window* w,float* x,float* y){*x=*y=GetDpiForWindow(static_cast<HWND>(w))/96.0f;}
void framebufferSize(Window* w,int* x,int* y){RECT r{};GetClientRect(static_cast<HWND>(w),&r);*x=r.right;*y=r.bottom;}
void size(Window* w,int* x,int* y){framebufferSize(w,x,y);}
int attribute(Window* w,int){return IsIconic(static_cast<HWND>(w))?1:0;}
void setTitle(Window* w,const char* text){const auto name=wide(text?text:"");SetWindowTextW(static_cast<HWND>(w),name.c_str());}
void hide(Window* w){ShowWindow(static_cast<HWND>(w),SW_HIDE);}
void show(Window* w){ShowWindow(static_cast<HWND>(w),SW_SHOW);}
void restore(Window* w){ShowWindow(static_cast<HWND>(w),SW_RESTORE);}
void focus(Window* w){SetForegroundWindow(static_cast<HWND>(w));SetFocus(static_cast<HWND>(w));}
double refreshRate(Window* w){MONITORINFOEXW info{};info.cbSize=sizeof(info);DEVMODEW mode{};mode.dmSize=sizeof(mode);if(GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(w),MONITOR_DEFAULTTONEAREST),&info)&&EnumDisplaySettingsW(info.szDevice,ENUM_CURRENT_SETTINGS,&mode)&&mode.dmDisplayFrequency>1)return mode.dmDisplayFrequency;return 60;}
void requestRefreshAfter(Window* w, double seconds) {
    if (auto s = state(w)) {
        if (!s->liveResize || IsIconic(static_cast<HWND>(w))) return;
        s->paintPending = true;
        const double deadline = timeSeconds() + std::max(0.0, seconds);
        if (s->refreshDeadline > 0 && s->refreshDeadline <= deadline) return;
        s->refreshDeadline = deadline;
        SetTimer(static_cast<HWND>(w), resizeTimer,
            static_cast<UINT>(std::clamp(std::ceil(seconds * 1000), 10.0, 1000.0)), nullptr);
    }
}
void cancelRefresh(Window* w) {
    KillTimer(static_cast<HWND>(w), resizeTimer);
    if (auto s = state(w)) s->refreshDeadline = 0;
}
void setLiveResize(Window* w, bool enabled) {
    if (auto s = state(w)) s->liveResize = enabled;
    if (!enabled) cancelRefresh(w);
}
void setPaintBackground(Window* w, COLORREF color) { if (auto s = state(w)) s->paintBackground = color; }
bool inSizeMove(Window* w) { auto s = state(w); return s && s->sizing; }
void notePresentationLost(Window* w) {
    if (auto s = state(w)) {
        s->coveredWidth = s->coveredHeight = 0;
        s->paintPending = true;
        requestRefreshAfter(w, 0);
    }
}
void notePresented(Window* w, int width, int height) {
    if (auto s = state(w)) {
        RECT client{}; GetClientRect(static_cast<HWND>(w), &client);
        s->coveredWidth = std::min<LONG>(width, client.right);
        s->coveredHeight = std::min<LONG>(height, client.bottom);
        if (width == client.right && height == client.bottom) {
            s->presentedGeneration = s->sizeGeneration;
            s->paintPending = false;
            cancelRefresh(w);
        } else requestRefreshAfter(w, 0);
    }
}
}
}
