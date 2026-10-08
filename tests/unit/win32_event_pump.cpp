#include "core/window/window_backend.h"

#if defined(_WIN32) && defined(EUI_WINDOW_BACKEND_WIN32)
#include "core/window/win32_host.h"
#include <windows.h>
#include <chrono>
#include <iostream>

namespace {
constexpr UINT kWork = WM_APP + 431;
int consumed = 0, orderErrors = 0;
LRESULT CALLBACK testProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == kWork) {
        if (static_cast<int>(w) != consumed) ++orderErrors;
        ++consumed;
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
}

int main() {
    // A message-only owned test HWND: no visible UI or user window input.
    const auto module = GetModuleHandleW(nullptr);
    WNDCLASSW cls{}; cls.lpfnWndProc = testProc; cls.hInstance = module;
    cls.lpszClassName = L"EuiEventPumpTest";
    if (!RegisterClassW(&cls)) return 2;
    auto window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, module, nullptr);
    if (!window) { UnregisterClassW(cls.lpszClassName, module); return 2; }
    constexpr int count = 5000;
    for (int i = 0; i < count; ++i) {
        if (!PostMessageW(window, kWork, static_cast<WPARAM>(i), 0)) return 2;
    }
    const auto begin = std::chrono::steady_clock::now();
    core::window::win32::pollEvents();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count();
    const int firstBatch = consumed;
    // Returning with work still queued lets the normal host run a frame before
    // the next pump. Do not assert elapsed time; slow message handlers/machines
    // can exceed a soft time budget even when the count is bounded.
    bool ok = firstBatch > 0 && firstBatch <= 256 && firstBatch < count;
    int pumps = 1;
    while (consumed < count && pumps < count) { core::window::win32::pollEvents(); ++pumps; }
    ok = ok && consumed == count && orderErrors == 0;
    core::window::win32::pollEvents();
    ok = ok && consumed == count;
    DestroyWindow(window); UnregisterClassW(cls.lpszClassName, module);
    std::cout << "first_batch=" << firstBatch << " total=" << consumed << " pumps=" << pumps
              << " order_errors=" << orderErrors << " diagnostic_ms=" << ms << '\n';
    std::cout << (ok ? "PASS" : "FAIL") << ": win32_event_pump\n";
    return ok ? 0 : 1;
}
#else
int main() { return 0; }
#endif
