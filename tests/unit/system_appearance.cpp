#include "platform/system_appearance.h"
#include <iostream>

int main() {
#if defined(_WIN32)
    int failures = 0, wakes = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    const auto create = [] {
        // Hidden top-level window: receives the same native notifications without
        // changing the user's system preference or interacting with other apps.
        return CreateWindowExW(0,L"STATIC",L"appearance test",WS_OVERLAPPED,
                               0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    };
    HWND window = create();
    check(window != nullptr, "create native window");
    if (!window) return 1;
    neo::platform::SystemAppearanceObserver observer;
    check(observer.watch(window,[&] { ++wakes; }), "attach observer");
    check(observer.consumeChange() && !observer.consumeChange(), "initial reconciliation consumed once");
    check(observer.watch(window,[&] { ++wakes; }) && !observer.consumeChange(), "same handle does not reattach");
    for (UINT message : {WM_SETTINGCHANGE, WM_THEMECHANGED, WM_SYSCOLORCHANGE}) {
        const int before = wakes;
        SendMessageW(window,message,0,reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
        check(wakes==before+1 && observer.consumeChange() && !observer.consumeChange(), "appearance notification wakes once");
    }
    SendMessageW(window,WM_ACTIVATEAPP,FALSE,0);
    check(!observer.consumeChange(), "deactivation does not trigger refresh");
    SendMessageW(window,WM_ACTIVATEAPP,TRUE,0);
    check(observer.consumeChange(), "activation reconciles missed change");
    SendMessageW(window,WM_SETTINGCHANGE,0,0);
    SendMessageW(window,WM_THEMECHANGED,0,0);
    check(observer.consumeChange() && !observer.consumeChange(), "notification burst coalesces in composition");
    observer.stop();
    const int stopped = wakes;
    SendMessageW(window,WM_THEMECHANGED,0,0);
    check(wakes==stopped && !observer.consumeChange(), "stop detaches observer");
    observer.watch(window,[&] { ++wakes; });
    observer.consumeChange();
    DestroyWindow(window);
    check(!observer.consumeChange(), "destroy clears pending notification");
    window = create();
    check(observer.watch(window,[&] { ++wakes; }) && observer.consumeChange(), "observer can attach after destruction");
    observer.stop();
    DestroyWindow(window);
    if (failures) return 1;
#endif
    std::cout << "system appearance notification and observer lifetime passed\n";
    return 0;
}
