#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <thread>

namespace app::detail {

#ifdef _WIN32
class FrameWaitTimer {
public:
    FrameWaitTimer() {
        handle_ = CreateWaitableTimerExW(nullptr,
                                         nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (handle_ == nullptr) {
            handle_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
    }

    ~FrameWaitTimer() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    bool wait(double seconds) {
        if (handle_ == nullptr || seconds <= 0.0) {
            return false;
        }

        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -static_cast<LONGLONG>(seconds * 10000000.0);
        if (!SetWaitableTimer(handle_, &dueTime, 0, nullptr, nullptr, FALSE)) {
            return false;
        }
        WaitForSingleObject(handle_, INFINITE);
        return true;
    }

private:
    HANDLE handle_ = nullptr;
};
#endif

inline void waitForFrameDuration(double seconds) {
    if (seconds <= 0.0) {
        return;
    }

#ifdef _WIN32
    if (seconds > 0.001) {
        static FrameWaitTimer timer;
        if (timer.wait(std::max(0.0, seconds - 0.00035))) {
            return;
        }
    }
#endif

    if (seconds > 0.003) {
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(seconds - 0.0015, 0.001)));
    } else if (seconds > 0.0005) {
        std::this_thread::yield();
    }
}

} // namespace app::detail
