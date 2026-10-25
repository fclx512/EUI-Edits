#include "ui/settings_header.h"
#include "ui/window_chrome.h"
#include <cmath>
#include <iostream>

namespace app { void requestUpdate() {} }

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void modalCaption() {
    namespace chrome = neo::platform::chrome;
    neo::AppState state;
    const eui::Screen screen{1000, 700};
    const auto caption = [&] { return chrome::captionAt(300, 20, 1000, chrome::titleHeight); };
    const auto compose = [&] {
        chrome::beginFrame(1000, 34);
        neo::protectWindowOverlayChrome(state, screen);
    };
    compose();
    check(caption(), "normal top blank area is draggable");
    state.imagePreviewPath = "fixture.png";
    compose();
    check(!caption(), "image preview receives input over the former caption");
    state.imagePreviewPath.clear();
    state.mindmapPreview.open = true;
    state.mindmapPreview.inlineMode = false;
    compose();
    check(!caption(), "full-window mindmap receives caption-area input");
    state.mindmapPreview.inlineMode = true;
    compose();
    check(caption(), "inline mindmap leaves the separate caption draggable");
    state.mindmapPreview.open = false;
    state.riskAction = neo::AppState::RiskAction::Confirm;
    compose();
    check(!caption(), "risk confirmation backdrop receives caption-area input");
    state.riskAction = neo::AppState::RiskAction::None;
    state.linkEditorOpen = true;
    compose();
    check(!caption(), "link dialog backdrop receives caption-area input");
    state.linkEditorOpen = false;
    state.saveConflictOpen = true;
    compose();
    check(!caption(), "save conflict backdrop receives caption-area input");
    state.saveConflictOpen = false;
    compose();
    check(caption(), "closing a modal restores caption input next frame");
}

void settingsHeader() {
    namespace chrome = neo::platform::chrome;
    neo::AppState state;
    eui::Ui ui;
    for (const float font : {12.0f, 14.0f, 18.0f}) {
        state.uiFontSize = font;
        for (const float width : {380.0f, 560.0f, 608.0f, 1000.0f}) {
            for (int page = 0; page < 3; ++page) {
                state.settingsOpen = true;
                state.themeListOpen = page == 1;
                state.fontPicker = page == 2 ? neo::FontPickerTarget::Editor : neo::FontPickerTarget::None;
                const auto metrics = neo::uiMetrics(state);
                chrome::beginFrame(width, metrics.menuBarHeight);
                ui.begin("settings-header");
                neo::settingsHeaderView(ui, state, eui::Screen{width, 600});
                ui.end();
                ui.layout(width, 600);
                const auto caption = [&](float x, float y) {
                    return chrome::captionAt(x, y, width, chrome::titleHeight);
                };
                const auto* header = ui.find("settings.page.header");
                const auto* title = ui.find("settings.page.title");
                auto* back = ui.find("settings.backToEditor.hit");
                check(header && back && back->onClick, "production settings header and return button exist");
                if (!header || !back || !back->onClick) continue;
                check(caption(180, 16) && caption(300, header->frame.height - 4),
                      "both halves of every settings header move the window");
                check(title && (title->frame.y >= metrics.menuBarHeight ||
                      title->frame.x + title->frame.width <= width - chrome::controlsWidth),
                      "page title does not overlap window controls at narrow widths and large fonts");
                if (title) {
                    const float advance = core::TextPrimitive::measureTextWidth(
                        title->text, title->fontFamily, title->fontSize, title->fontWeight);
                    core::TextStyle textStyle;
                    textStyle.text=title->text; textStyle.fontFamily=title->fontFamily;
                    textStyle.fontSize=title->fontSize; textStyle.fontWeight=title->fontWeight;
                    textStyle.maxWidth=title->frame.width; textStyle.wrap=title->wrap;
                    check(title->wrap || advance <= title->frame.width,
                          "long translated title wraps instead of painting beyond its frame");
                    check(core::TextPrimitive::measureTextSize(textStyle).y <= title->frame.height &&
                          title->frame.y + title->frame.height <= header->frame.height,
                          "measured title lines fit inside the production header");
                }
                const auto& b = back->frame;
                check(!caption(b.x + 1, b.y + 1) &&
                      !caption(b.x + b.width - 1, b.y + b.height - 1),
                      "entire actual return button stays interactive at every font size");
                check(caption(b.x + b.width + 2, b.y + b.height / 2),
                      "space beside the return button remains caption");
                check(!caption(width - 60, 16) && caption(width - 60, header->frame.height - 4),
                      "window buttons exclude only their actual row");
                check(!caption(300, header->frame.height) && !caption(300, header->frame.height + 40),
                      "settings content never moves the window");
                state.openMenu = neo::MenuKind::View;
                state.contextMenuOpen = state.vaultContextMenuOpen = state.findOptionsOpen = true;
                state.markdownHeadingMenuOpen = state.languageMenuOpen = state.tabListOpen = true;
                back->onClick();
                check(!state.settingsOpen && !state.themeListOpen &&
                      state.fontPicker == neo::FontPickerTarget::None,
                      "return button closes all settings subpages");
                check(neo::popup_presentation::activeOwner(state) == neo::popup_presentation::Owner::None &&
                      !state.tabListOpen && state.findEditorFocusPending,
                      "return button clears all transient menus and restores editor focus");
            }
        }
    }
    chrome::beginFrame(1000, 34);
    check(chrome::titleHeight == 34 && chrome::captionAt(50, 26, 1000, chrome::titleHeight) &&
          !chrome::captionAt(300, 60, 1000, chrome::titleHeight),
          "return to editor clears settings exclusions and caption height");
}

#if defined(_WIN32)
constexpr UINT nativeCaptionDrawMessage = 0xAE;
constexpr UINT nativeFrameDrawMessage = 0xAF;
int activations = 0, closeRequests = 0;
int nativeCaptionDraws = 0, nativeFrameDraws = 0;
LRESULT CALLBACK nativeProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_ACTIVATE) ++activations;
    if (msg == WM_CLOSE) { ++closeRequests; return 0; }
    if (msg == nativeCaptionDrawMessage) { ++nativeCaptionDraws; return 0; }
    if (msg == nativeFrameDrawMessage) { ++nativeFrameDraws; return 0; }
    return DefWindowProcW(hwnd, msg, w, l);
}

void nativeChrome() {
    namespace chrome = neo::platform::chrome;
    WNDCLASSW wc{};
    wc.lpfnWndProc = nativeProcedure;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EuiWindowChromeTest";
    check(RegisterClassW(&wc) != 0, "register hidden test window");
    // Negative screen coordinates exercise signed NCHITTEST conversion without
    // showing a window, changing focus, or touching another application's HWND.
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"chrome test", WS_OVERLAPPEDWINDOW,
                               -1200, -100, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    check(hwnd != nullptr, "create hidden test window");
    if (!hwnd) return;
    for (const WPARAM active : {WPARAM{0}, WPARAM{1}}) {
        SendMessageW(hwnd, nativeCaptionDrawMessage, active, 0);
        SendMessageW(hwnd, nativeFrameDrawMessage, active, 0);
    }
    check(nativeCaptionDraws == 2 && nativeFrameDraws == 2,
          "native caption and frame draw messages reach the base procedure before chrome attach");
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    chrome::update(hwnd, 1000, 34);
    check(chrome::window == hwnd && GetWindowLongPtrW(hwnd, GWL_STYLE) == style,
          "attach preserves resize, caption and system-menu styles");
    RECT beforePin{}, afterPin{};
    GetWindowRect(hwnd, &beforePin);
    const HWND foregroundBeforePin = GetForegroundWindow();
    const bool initiallyPinned = chrome::alwaysOnTop();
    SetLastError(ERROR_SUCCESS);
    const bool pinSucceeded = chrome::setAlwaysOnTop(true);
    const DWORD pinError = GetLastError();
    const bool pinnedAfterCall = chrome::alwaysOnTop();
    std::cout << "pin initial=" << initiallyPinned << " success=" << pinSucceeded
              << " after=" << pinnedAfterCall << " error=" << pinError
              << " exstyle=" << GetWindowLongPtrW(hwnd, GWL_EXSTYLE) << '\n';
    check(!initiallyPinned && pinSucceeded, "pin request succeeds for the isolated native window");
    if (pinSucceeded && !pinnedAfterCall) {
        // A background probe must not activate a window to gain foreground
        // rights. Report the missing observable Z-order state, rather than
        // treating API success as proof of topmost behavior. The final EXE
        // requires separate foreground GUI pin/unpin and occlusion acceptance.
        std::cout << "SKIP: topmost state unavailable for the nonactivated probe; foreground GUI acceptance required\n";
    }
    GetWindowRect(hwnd, &afterPin);
    check(EqualRect(&beforePin, &afterPin) && GetForegroundWindow() == foregroundBeforePin && !IsWindowVisible(hwnd),
          "pinning preserves size, position, visibility and foreground ownership");
    check(chrome::setAlwaysOnTop(false) && !chrome::alwaysOnTop(),
          "unpin clears the native topmost style");
    const int captionDrawsBefore = nativeCaptionDraws;
    const int frameDrawsBefore = nativeFrameDraws;
    for (const WPARAM active : {WPARAM{0}, WPARAM{1}}) {
        SendMessageW(hwnd, nativeCaptionDrawMessage, active, 0);
        SendMessageW(hwnd, nativeFrameDrawMessage, active, 0);
    }
    check(nativeCaptionDraws == captionDrawsBefore && nativeFrameDraws == frameDrawsBefore,
          "custom chrome consumes native caption and frame painting for active and inactive states");
    check(chrome::frameExtensionResult != E_PENDING && chrome::cornerPreferenceResult != E_PENDING,
          "DWM frame and corner requests produce diagnostics");
    std::cout << "DWM extension HRESULT=" << std::hex << chrome::frameExtensionResult
              << ", corner preference HRESULT=" << chrome::cornerPreferenceResult << std::dec << '\n';
    if (FAILED(chrome::frameExtensionResult))
        std::cout << "SKIP: compositor frame unavailable; continue native input and fallback checks\n";
    RECT rect{10, 20, 1010, 720};
    SendMessageW(hwnd, WM_NCCALCSIZE, FALSE, reinterpret_cast<LPARAM>(&rect));
    check(rect.left == 10 && rect.top == 20 && rect.right == 1010 && rect.bottom == 720,
          "RECT form of NCCALCSIZE preserves borderless client bounds");
    NCCALCSIZE_PARAMS params{};
    params.rgrc[0] = rect;
    SendMessageW(hwnd, WM_NCCALCSIZE, TRUE, reinterpret_cast<LPARAM>(&params));
    check(EqualRect(&rect, &params.rgrc[0]), "NCCALCSIZE_PARAMS form has the same client bounds");
    RECT client{};
    GetClientRect(hwnd, &client);
    chrome::beginFrame(static_cast<float>(client.right), 72);
    chrome::exclude(18, 22, 154, 28);
    const auto hit = [&](int x, int y) {
        POINT p{x, y};
        ClientToScreen(hwnd, &p);
        return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
    };
    check(hit(300, 20) == HTCAPTION && hit(300, 60) == HTCAPTION,
          "native settings header uses the full caption height");
    check(hit(50, 30) == HTCLIENT && hit(300, 100) == HTCLIENT,
          "native controls and content remain client input");
    check(hit(0, 0) == HTTOPLEFT && hit(client.right - 1, 0) == HTTOPRIGHT &&
          hit(0, client.bottom - 1) == HTBOTTOMLEFT &&
          hit(client.right - 1, client.bottom - 1) == HTBOTTOMRIGHT &&
          hit(300, 0) == HTTOP && hit(300, client.bottom - 1) == HTBOTTOM &&
          hit(0, 200) == HTLEFT && hit(client.right - 1, 200) == HTRIGHT,
          "all eight native resize directions take priority");
    check(hit(-1, 100) == HTNOWHERE && hit(client.right, 100) == HTNOWHERE,
          "outside-window coordinates are not resize handles");
    for (const float uiScale : {.8f, 1.0f, 2.0f}) {
        const float scale = GetDpiForWindow(hwnd) / 96.0f * uiScale;
        chrome::beginFrame(client.right / scale, 72);
        const float rail = chrome::resizeInset();
        const int scrollbarCenter = static_cast<int>(client.right - (rail + 4) * scale);
        check(rail > 0 && hit(scrollbarCenter, 200) == HTCLIENT &&
              hit(client.right - 1, 200) == HTRIGHT,
              "reserved rail keeps an 8-DIP scrollbar center interactive without losing native resize");
        for (const float scrollbarWidth : {4.0f, 8.0f, 12.0f}) {
            const float compactRail = chrome::resizeInset(scrollbarWidth);
            const int compactCenter = static_cast<int>(client.right -
                (compactRail + scrollbarWidth * 0.5f) * scale);
            check(compactRail >= 0.0f && compactRail <= rail &&
                  hit(compactCenter, 200) == HTCLIENT &&
                  hit(client.right - 1, 200) == HTRIGHT,
                  "scrollbar width contributes to sizing allowance while its center remains interactive");
            check(compactRail == 0.0f || compactCenter > scrollbarCenter,
                  "a visible scrollbar no longer reserves a second full native sizing strip");
        }
        check(chrome::resizeInset(-8.0f) == rail,
              "negative scrollbar width does not reduce the safe native sizing allowance");
    }
    chrome::beginFrame(static_cast<float>(client.right), 72);
    neo::AppState modal;
    modal.imagePreviewPath = "fixture.png";
    neo::protectWindowOverlayChrome(modal, eui::Screen{static_cast<float>(client.right), 700});
    check(hit(300, 20) == HTCLIENT && hit(0, 200) == HTLEFT,
          "native modal input overrides caption while retaining edge resizing");
    for (const auto message : {WM_ACTIVATE, WM_DWMCOMPOSITIONCHANGED, WM_THEMECHANGED}) {
        chrome::frameExtensionResult = E_PENDING;
        SendMessageW(hwnd, message, 0, 0);
        check(chrome::frameExtensionResult != E_PENDING,
              "DWM frame is reapplied after native appearance changes, including unavailable-compositor fallback");
    }
    check(activations > 0, "activation still reaches the backend");
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(closeRequests == 1 && IsWindow(hwnd), "close still reaches the backend confirmation path");
    DestroyWindow(hwnd);
    check(chrome::window == nullptr, "destroy detaches the chrome window");
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
#endif
} // namespace

int main() {
    modalCaption();
    for (const char* language : {"zh-CN", "en"}) {
        neo::i18n::setPreference(language);
        settingsHeader();
    }
    neo::i18n::setPreference("system");
#if defined(_WIN32)
    nativeChrome();
#endif
    if (!failures) std::cout << "window_chrome PASS (layout and hidden native messages; no visual shadow acceptance)\n";
    return failures ? 1 : 0;
}
