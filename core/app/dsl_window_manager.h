#pragma once

#include "eui/app.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace app {

template <typename WindowT>
class DslWindowManager {
public:
    using WindowPtr = std::unique_ptr<WindowT>;

    bool empty() const {
        return windows_.empty();
    }

    template <typename CreateFn>
    void createPending(const std::vector<DslWindowRequest>& requests, CreateFn&& createWindow) {
        for (const DslWindowRequest& request : requests) {
            if (!request.compose) {
                continue;
            }
            if (WindowPtr window = createWindow(request)) {
                windows_.push_back(std::move(window));
            }
        }
    }

    template <typename ClosedFn, typename DestroyFn>
    void pruneClosed(ClosedFn&& isClosed, DestroyFn&& destroyWindow) {
        windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [&](WindowPtr& window) {
            if (!window) {
                return true;
            }
            if (!isClosed(*window)) {
                return false;
            }
            destroyWindow(window);
            return true;
        }), windows_.end());
    }

    template <typename ClosedFn>
    WindowT* modalWindow(ClosedFn&& isClosed) {
        for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
            WindowPtr& window = *it;
            if (window && window->content.request().modal && !isClosed(*window)) {
                return window.get();
            }
        }
        return nullptr;
    }

    template <typename Predicate>
    WindowT* find(Predicate&& predicate) {
        for (WindowPtr& window : windows_) {
            if (window && predicate(*window)) {
                return window.get();
            }
        }
        return nullptr;
    }

    bool anyAnimating() const {
        return anyAnimating([](const WindowT&) {
            return true;
        });
    }

    template <typename ActiveFn>
    bool anyAnimating(ActiveFn&& isActive) const {
        return std::any_of(windows_.begin(), windows_.end(), [&](const WindowPtr& window) {
            return window && isActive(*window) && window->content.isAnimating();
        });
    }

    /** @brief 所有活动窗口里最近的那个 pending 定时器还剩多少秒（+inf = 都没有）。
     *  主循环据此决定"睡到什么时候"，这样定时器不必靠持续渲染来等时间。 */
    template <typename ActiveFn>
    float earliestTimerWakeSeconds(ActiveFn&& isActive) const {
        float wake = std::numeric_limits<float>::infinity();
        for (const WindowPtr& window : windows_) {
            if (window && isActive(*window)) {
                wake = std::min(wake, window->content.nextTimerWakeSeconds());
            }
        }
        return wake;
    }

    template <typename UpdateFn>
    void updateAll(UpdateFn&& updateWindow) {
        for (WindowPtr& window : windows_) {
            if (window) {
                updateWindow(*window);
            }
        }
    }

    template <typename DestroyFn>
    void destroyAll(DestroyFn&& destroyWindow) {
        for (WindowPtr& window : windows_) {
            destroyWindow(window);
        }
        windows_.clear();
    }

private:
    std::vector<WindowPtr> windows_;
};

} // namespace app
