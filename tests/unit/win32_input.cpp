#if defined(_WIN32) && defined(EUI_WINDOW_BACKEND_WIN32)

#include "core/input/input_state.h"
#include "core/window/win32_host.h"

#include <windows.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>
#include "core/window/window_backend.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool verifyFileDrop(core::window::Handle window) {
    const wchar_t paths[] = L"D:\\documents\\\x4e2d\x6587.py\0D:\\documents\\second.json\0";
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + sizeof(paths));
    if (!expect(memory != nullptr, "cannot allocate native file drop")) return false;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!drop) { GlobalFree(memory); return expect(false, "cannot lock native file drop"); }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), paths, sizeof(paths));
    GlobalUnlock(memory);
    // The host takes ownership through DragFinish, just as with an Explorer drop.
    SendMessageW(static_cast<HWND>(window), WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
    const auto received = core::window::consumeDroppedPaths(window);
    return expect(received.size() == 1 && received.front() == "D:\\documents\\\xe4\xb8\xad\xe6\x96\x87.py",
                  "native Unicode file drop did not queue the first path") &&
           expect(core::window::consumeDroppedPaths(window).empty(), "file drop was consumed twice");
}

bool verifyInputMessages(core::window::Handle window) {
    const HWND hwnd = static_cast<HWND>(window);
    bool ok = true;
    // Hiding the owned HWND can deliver a real KILLFOCUS. This synthetic input
    // contract starts a focused session without showing or activating the window.
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);

    SendMessageW(hwnd, WM_CHAR, L'a', 1);
    SendMessageW(hwnd, WM_CHAR, 0xd83d, 1);
    SendMessageW(hwnd, WM_CHAR, 0xde00, 1);
    auto text = core::consumeTextInput(window);
    ok &= expect(text.text == "a\xf0\x9f\x98\x80", "WM_CHAR or UTF-16 surrogate pair was not converted to UTF-8");

    SendMessageW(hwnd, WM_UNICHAR, 0x1f642, 1);
    text = core::consumeTextInput(window);
    ok &= expect(text.text == "\xf0\x9f\x99\x82", "WM_UNICHAR supplementary character was not converted to UTF-8");

    constexpr LPARAM keyBits = (0x4bL << 16) | (1L << 24);
    SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, keyBits);
    SendMessageW(hwnd, WM_KEYUP, VK_LEFT, keyBits |
                 static_cast<LPARAM>(1ULL << 30) | static_cast<LPARAM>(1ULL << 31));
    const std::vector<core::KeyEvent> keys = core::consumeKeyEvents(window);
    ok &= expect(keys.size() == 2, "WM_KEYDOWN/WM_KEYUP did not produce two key events");
    if (keys.size() == 2) {
        ok &= expect(keys[0].key == core::InputKey::Left && keys[0].action == core::KeyAction::Press,
                     "VK_LEFT key-down mapping was incorrect");
        ok &= expect(keys[1].key == core::InputKey::Left && keys[1].action == core::KeyAction::Release,
                     "VK_LEFT key-up mapping was incorrect");
    }

    // START precedes the first (possibly empty) COMPSTR. The native flag must
    // protect this gap, because Runtime dispatches keys before text callbacks.
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    const WPARAM candidateKeys[]{VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_RETURN, VK_ESCAPE,
                                VK_BACK, VK_DELETE, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, 'A', 'Z'};
    for (const WPARAM candidateKey : candidateKeys) {
        SendMessageW(hwnd, WM_KEYDOWN, candidateKey, 1);
        SendMessageW(hwnd, WM_KEYUP, candidateKey,
                     static_cast<LPARAM>(1ULL << 30) | static_cast<LPARAM>(1ULL << 31));
    }
    ok &= expect(core::consumeKeyEvents(window).empty(), "candidate keys leaked before first preedit text");
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, 1);
    ok &= expect(core::consumeKeyEvents(window).empty(), "empty ongoing preedit released candidate key protection");
    core::queueTextEditing(window, "cancel me");
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, 0);
    text = core::consumeTextInput(window);
    ok &= expect(text.compositionChanged && !text.composing && text.compositionText.empty(),
                 "WM_IME_COMPOSITION without GCS flags did not cancel preedit");
    SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, keyBits);
    const auto afterCancelKeys = core::consumeKeyEvents(window);
    ok &= expect(afterCancelKeys.size() == 1, "native cancellation left ordinary navigation blocked");
    SendMessageW(hwnd, WM_KEYUP, VK_LEFT, keyBits |
                 static_cast<LPARAM>(1ULL << 30) | static_cast<LPARAM>(1ULL << 31));
    core::consumeKeyEvents(window);
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);

    core::queueTextEditing(window, "synthetic preedit");
    SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    text = core::consumeTextInput(window);
    ok &= expect(text.compositionChanged && !text.composing && text.compositionText.empty(),
                 "focus loss did not clear a queued IME preedit");
    ok &= expect(core::detail::compositionText(window).empty(), "focus loss left shared preedit state behind");
    return ok;
}

bool verifyUninstallStopsInput(core::window::Handle window) {
    core::window::uninstallInputCallbacks(window);
    const HWND hwnd = static_cast<HWND>(window);
    SendMessageW(hwnd, WM_CHAR, L'x', 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_BACK, 1);
    SendMessageW(hwnd, WM_KEYUP, VK_BACK,
                 static_cast<LPARAM>(1ULL << 30) | static_cast<LPARAM>(1ULL << 31));

    const auto text = core::consumeTextInput(window);
    const auto keys = core::consumeKeyEvents(window);
    return expect(text.text.empty() && !text.hasInput(), "text was queued after input callbacks were uninstalled") &&
           expect(keys.empty(), "key events were queued after input callbacks were uninstalled");
}

bool verifyImeFocusLifecycle(core::window::Handle window) {
    const HWND hwnd = static_cast<HWND>(window);
    bool ok = true;
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    std::string nativeText;
    bool composing = false;
    core::window::queryImeComposition(window, nativeText, composing);
    ok &= expect(composing, "START with empty preedit was not exposed as native composing");
    core::queueTextEditing(window, "pending pinyin");
    PostMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    PostMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    PostMessageW(hwnd, WM_IME_CHAR, L'x', 1);
    core::window::cancelImeComposition(window);
    MSG pending{};
    ok &= expect(!PeekMessageW(&pending, hwnd, WM_IME_STARTCOMPOSITION, WM_IME_COMPOSITION, PM_NOREMOVE),
                 "cancel left messages from the old native composition queued");
    ok &= expect(!PeekMessageW(&pending, hwnd, WM_IME_CHAR, WM_IME_CHAR, PM_NOREMOVE),
                 "cancel left duplicate IME character messages queued");
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
    SendMessageW(hwnd, WM_IME_CHAR, L'x', 1);
    SendMessageW(hwnd, WM_CHAR, L'x', 1);
    auto input = core::consumeTextInput(window);
    ok &= expect(input.text.empty() && input.compositionText.empty() && !input.composing,
                 "late canceled result, preedit or character reached text queue");
    // A genuine new ordinary key permits its character, but not an old IME result.
    SendMessageW(hwnd, WM_KEYDOWN, 'A', 1);
    SendMessageW(hwnd, WM_CHAR, L'a', 1);
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    input = core::consumeTextInput(window);
    ok &= expect(input.text == "a" && !input.composing, "ordinary typing did not recover after native cancel");
    SendMessageW(hwnd, WM_KEYUP, 'A', static_cast<LPARAM>(1ULL << 30) | static_cast<LPARAM>(1ULL << 31));
    core::consumeKeyEvents(window);

    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    core::window::queryImeComposition(window, nativeText, composing);
    ok &= expect(composing, "fresh native composition stayed blocked after cancellation");
    core::queueTextEditing(window, "unconfirmed");
    SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
    SendMessageW(hwnd, WM_CHAR, L'z', 1);
    input = core::consumeTextInput(window);
    ok &= expect(input.text.empty() && input.compositionText.empty() && !input.composing,
                 "focus loss admitted a late native commit or preedit");
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    input = core::consumeTextInput(window);
    ok &= expect(!input.composing, "refocus revived canceled native preedit before a fresh START");
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    core::window::queryImeComposition(window, nativeText, composing);
    ok &= expect(composing, "native composition did not restart after refocus");
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    input = core::consumeTextInput(window);
    ok &= expect(!input.composing && input.compositionText.empty(), "END did not release native composition state");
    return ok;
}

bool verifyImeActivationLifecycle(core::window::Handle window) {
    const HWND hwnd = static_cast<HWND>(window);
    bool ok = true;
    // Check both same-app top-level deactivation and cross-app activation before
    // KILLFOCUS/SETCONTEXT, without changing the user's actual foreground window.
    for (const UINT activationMessage : {WM_ACTIVATE, WM_ACTIVATEAPP}) {
        SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
        SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
        core::queueTextEditing(window, "nihao");
        SendMessageW(hwnd, activationMessage,
                     activationMessage == WM_ACTIVATE ? WA_INACTIVE : FALSE, 0);
        SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
        SendMessageW(hwnd, WM_CHAR, L'n', 1);
        const auto canceled = core::consumeTextInput(window);
        ok &= expect(canceled.text.empty() && !canceled.composing && canceled.compositionText.empty(),
                     "deactivation admitted raw pinyin before KILLFOCUS");
        std::string nativeText;
        bool composing = false;
        core::window::queryImeComposition(window, nativeText, composing);
        ok &= expect(!composing, "deactivation retained native composition state");
        SendMessageW(hwnd, activationMessage,
                     activationMessage == WM_ACTIVATE ? WA_ACTIVE : TRUE, 0);
        SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
        core::window::queryImeComposition(window, nativeText, composing);
        ok &= expect(composing, "activation did not permit a fresh composition");
        SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
        core::consumeTextInput(window);
    }
    return ok;
}

} // namespace

int main() {
    if (!core::window::win32::initialize()) {
        std::cerr << "Win32 host initialization failed\n";
        return 1;
    }

    core::window::WindowCreateRequest request;
    request.width = 160;
    request.height = 100;
    request.title = "EUI-Edits Win32 input contract";
    request.decorated = false;
    request.resizable = false;
    request.renderApi = core::window::RenderApi::Native;

    const core::window::Handle window = core::window::createWindow(request);
    if (!window) {
        core::window::win32::shutdownHost();
        std::cerr << "Win32 test HWND creation failed\n";
        return 1;
    }
    core::window::win32::hide(static_cast<core::window::win32::Window*>(window));
    core::window::installInputCallbacks(window);

    bool ok = verifyInputMessages(window);
    ok &= verifyFileDrop(window);
    ok &= verifyImeFocusLifecycle(window);
    ok &= verifyImeActivationLifecycle(window);
    ok &= verifyUninstallStopsInput(window);

    SendMessageW(static_cast<HWND>(window), WM_CLOSE, 0, 0);
    ok &= expect(core::window::win32::shouldClose(static_cast<core::window::win32::Window*>(window)),
                 "WM_CLOSE did not mark the window as closing");
    ok &= expect(IsWindow(static_cast<HWND>(window)) != FALSE,
                 "WM_CLOSE destroyed the HWND before explicit teardown");

    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    ok &= expect(IsWindow(static_cast<HWND>(window)) == FALSE, "explicit destroy did not destroy the HWND");
    ok &= expect(core::window::win32::shouldClose(static_cast<core::window::win32::Window*>(window)),
                 "destroyed window was not treated as closed");
    core::window::win32::shutdownHost();
    return ok ? 0 : 1;
}

#else

int main() {
    return 77;
}

#endif
