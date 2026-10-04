#pragma once

#include <functional>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#endif

namespace neo::platform {

// UI-thread observer. Notifications only wake composition; appearance changes
// are applied there, outside the native window procedure and its reentrancy.
class SystemAppearanceObserver {
public:
    SystemAppearanceObserver() = default;
    ~SystemAppearanceObserver() { stop(); }
    SystemAppearanceObserver(const SystemAppearanceObserver&) = delete;
    SystemAppearanceObserver& operator=(const SystemAppearanceObserver&) = delete;

    bool watch(void* nativeWindow, std::function<void()> wake);
    void stop();
    bool consumeChange();

private:
    std::function<void()> wake_;
    bool pending_ = false;
#if defined(_WIN32)
    HWND window_ = nullptr;
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
#endif
};

} // namespace neo::platform
