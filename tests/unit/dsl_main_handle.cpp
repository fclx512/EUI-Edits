#if defined(_WIN32) && defined(EUI_WINDOW_BACKEND_WIN32)
#include "eui/detail/dsl_app_impl.h"
#include "core/window/win32_host.h"
#include <windows.h>
#include <iostream>

namespace app {
const DslAppConfig& dslAppConfig() { static DslAppConfig config; return config; }
void compose(eui::Ui&, const eui::Screen&) {}
}

int main() {
    if (!core::window::win32::initialize()) return 1;
    core::window::WindowCreateRequest request;
    request.width = 160; request.height = 100;
    request.title = "DSL main window handle contract";
    request.renderApi = core::window::RenderApi::Native;
    auto window = core::window::createWindow(request);
    if (!window) return 1;
    core::window::win32::hide(static_cast<core::window::win32::Window*>(window));
    bool ok = app::initialize(window) && core::window::mainWindowHandle() == window;
    SendMessageW(static_cast<HWND>(window), WM_SETFOCUS, 0, 0);
    SendMessageW(static_cast<HWND>(window), WM_IME_STARTCOMPOSITION, 0, 0);
    bool composing = false;
    std::string text;
    core::window::queryImeComposition(window, text, composing);
    ok = ok && composing;
    core::window::cancelImeComposition(core::window::mainWindowHandle());
    core::window::queryImeComposition(window, text, composing);
    ok = ok && !composing;
    // Candidate cancellation must restore the next real application key.
    core::consumeKeyEvents(window);
    SendMessageW(static_cast<HWND>(window), WM_KEYDOWN, VK_ESCAPE, 1);
    const auto keys = core::consumeKeyEvents(window);
    ok = ok && keys.size() == 1 && keys.front().key == core::InputKey::Escape;
    app::shutdown();
    ok = ok && core::window::mainWindowHandle() == nullptr;
    core::window::destroyWindow(window);
    core::window::win32::shutdownHost();
    std::cout << "DSL main handle registration/cancellation/teardown: " << (ok ? "passed" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
#else
int main() { return 77; }
#endif
