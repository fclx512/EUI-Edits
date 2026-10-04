#include "platform/system_appearance.h"

#include <utility>

namespace neo::platform {

bool SystemAppearanceObserver::watch(void* nativeWindow, std::function<void()> wake) {
#if defined(_WIN32)
    const auto window = static_cast<HWND>(nativeWindow);
    if (window && window == window_) return true;
    stop();
    if (!window) return false;
    wake_ = std::move(wake);
    if (!SetWindowSubclass(window, procedure, reinterpret_cast<UINT_PTR>(this),
                           reinterpret_cast<DWORD_PTR>(this))) {
        wake_ = {};
        return false;
    }
    window_ = window;
    // Reconcile changes between startup resolution and first composition.
    pending_ = true;
    return true;
#else
    (void)nativeWindow;
    (void)wake;
    return false;
#endif
}

void SystemAppearanceObserver::stop() {
#if defined(_WIN32)
    if (window_) RemoveWindowSubclass(window_, procedure, reinterpret_cast<UINT_PTR>(this));
    window_ = nullptr;
#endif
    pending_ = false;
    wake_ = {};
}

bool SystemAppearanceObserver::consumeChange() {
    return std::exchange(pending_, false);
}

#if defined(_WIN32)
LRESULT CALLBACK SystemAppearanceObserver::procedure(HWND window, UINT message,
        WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto& observer = *reinterpret_cast<SystemAppearanceObserver*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, procedure, id);
        observer.window_ = nullptr;
        observer.pending_ = false;
        observer.wake_ = {};
    } else if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED ||
               message == WM_SYSCOLORCHANGE || (message == WM_ACTIVATEAPP && wparam)) {
        observer.pending_ = true;
        if (observer.wake_) observer.wake_();
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
#endif

} // namespace neo::platform
