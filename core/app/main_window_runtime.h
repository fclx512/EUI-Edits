#pragma once

#include "eui/app.h"
#include "core/app/app_runner.h"
#include "core/app/frame_pacing.h"
#include "core/input/input_state.h"
#include "core/render/render_backend.h"
#include "core/render/render_surface.h"
#include "core/render/resize_trace.h"
#include "core/window/window_backend.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace app {

struct MainWindowMetrics {
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    float dpiScale = 1.0f;
    float pointerScale = 1.0f;

    bool valid() const {
        return framebufferWidth > 0 && framebufferHeight > 0 && dpiScale > 0.0f && pointerScale > 0.0f;
    }
};

class MainWindowRuntime {
public:
    explicit MainWindowRuntime(AppRunner& runner) : runner_(runner) {}

    template <typename AfterUpdateFn, typename UpdateChildrenFn, typename SetTitleFn, typename ChildAnimatingFn>
    void runFrame(core::window::Handle window,
                  core::render::RenderBackend& renderBackend,
                  const MainWindowMetrics& metrics,
                  double now,
                  double refreshRate,
                  bool inputEnabled,
                  AfterUpdateFn&& afterUpdate,
                  UpdateChildrenFn&& updateChildren,
                  SetTitleFn&& setTitle,
                  ChildAnimatingFn&& childAnimating,
                  bool waitForDeadline = true) {
        FrameGuard frameGuard(*this);
        if (!frameGuard.isOuterFrame()) {
            return;
        }

        runner_.updateFrameInterval(refreshRate, now);
        const bool updateRequested = runner_.consumeUpdateRequest();
        const bool frameWakeRequested = runner_.consumeFrameRequest();
        const bool frameRequested =
            runner_.paintRequested ||
            updateRequested ||
            frameWakeRequested ||
            runner_.anyAnimating(childAnimating()) ||
            core::hasPendingPointerInput(window, metrics.pointerScale);
        while (frameRequested && waitForDeadline) {
            const double remaining = runner_.nextFrameTime - core::window::timeSeconds();
            if (remaining <= 0.0) {
                break;
            }
            app::detail::waitForFrameDuration(remaining);
        }

        const double frameTime = core::window::timeSeconds();
        const float deltaSeconds = runner_.consumeFrameDelta(frameTime);

        const bool completed = updateAndRender(window,
                        renderBackend,
                        metrics,
                        deltaSeconds,
                        updateRequested,
                        inputEnabled,
                        std::forward<AfterUpdateFn>(afterUpdate));

        updateChildren(deltaSeconds, updateRequested);
        runner_.updateFrameTitle(core::window::timeSeconds(), std::forward<SetTitleFn>(setTitle));
        runner_.advanceFrameClock(core::window::timeSeconds(), runner_.anyAnimating(childAnimating()));
        if (!completed && runner_.paintRequested) {
            runner_.nextFrameTime = core::window::timeSeconds() + runner_.frameInterval;
        }
    }

    void markUnavailableFrame(double now) {
        runner_.paintRequested = true;
        runner_.resetTiming(now);
    }

    /**
     * @brief "现在该睡多久"：由最近的 pending 定时器决定，没有定时器就返回 +inf（可以无限期等事件）。
     *
     * childTimerWakeSeconds 是子窗口里最近的定时器剩余时间（同样 +inf 表示没有）。
     * 返回值已经扣掉"距离上一帧过去了多久"，因为运行时登记的是**上一帧结束那一刻**的剩余量。
     */
    double idleWakeSeconds(double now, float childTimerWakeSeconds) const {
        if (runner_.paintRequested) {
            return 0.0;
        }

        float wake = app::nextTimerWakeSeconds();
        if (std::isfinite(childTimerWakeSeconds)) {
            wake = std::isfinite(wake) ? std::min(wake, childTimerWakeSeconds) : childTimerWakeSeconds;
        }
        if (!std::isfinite(wake)) {
            return std::numeric_limits<double>::infinity();
        }
        const double sinceLastFrame = now - runner_.lastFrameTime;
        return std::max(0.0, static_cast<double>(wake) - sinceLastFrame);
    }

    template <typename AfterUpdateFn>
    bool updateAndRender(core::window::Handle window,
                         core::render::RenderBackend& renderBackend,
                         const MainWindowMetrics& metrics,
                         float deltaSeconds,
                         bool updateRequested,
                         bool inputEnabled,
                         AfterUpdateFn&& afterUpdate) {
        if (!metrics.valid()) {
            runner_.paintRequested = true;
            return false;
        }

        renderBackend.makeCurrent();
        core::render::ScopedRenderBackend scopedRenderBackend(renderBackend);
        const double updateStarted = core::render::resizeTraceStart();
        if (app::update(window,
                        deltaSeconds,
                        metrics.framebufferWidth,
                        metrics.framebufferHeight,
                        metrics.dpiScale,
                        metrics.pointerScale,
                        updateRequested,
                        inputEnabled)) {
            runner_.paintRequested = true;
        }
        core::render::resizeTraceEnd("update", updateStarted, metrics.framebufferWidth, metrics.framebufferHeight);

        afterUpdate();

        if (!runner_.paintRequested) {
            return false;
        }

        const auto renderStart = std::chrono::steady_clock::now();
        const double drawStarted = core::render::resizeTraceStart();
        renderBackend.beginFrame({
            window,
            core::window::nativeWindowInfo(window),
            metrics.framebufferWidth,
            metrics.framebufferHeight,
            metrics.dpiScale
        });
        if (!renderBackend.frameReady()) {
            runner_.paintRequested = true;
            runner_.nextFrameTime = core::window::timeSeconds() + runner_.frameInterval;
            return false;
        }
        app::render(metrics.framebufferWidth, metrics.framebufferHeight, metrics.dpiScale);
        core::render::resizeTraceEnd("draw", drawStarted, metrics.framebufferWidth, metrics.framebufferHeight);
        renderBackend.present();
        if (!renderBackend.framePresented()) {
            runner_.paintRequested = true;
            core::platform::requestUiUpdate();
            runner_.nextFrameTime = core::window::timeSeconds() + runner_.frameInterval;
            return false;
        }
        core::render::publishRenderFrameStats();
        runner_.recordRenderStats(core::render::lastRenderFrameStats());
        const auto renderEnd = std::chrono::steady_clock::now();
        runner_.recordRenderDuration(std::chrono::duration<double, std::milli>(renderEnd - renderStart).count());
        runner_.markRendered();
        return true;
    }

private:
    class FrameGuard {
    public:
        explicit FrameGuard(MainWindowRuntime& runtime) : runtime_(runtime) {
            if (runtime_.frameActive_) {
                runtime_.repaintAfterFrame_ = true;
                runtime_.runner_.paintRequested = true;
                return;
            }
            runtime_.frameActive_ = true;
            runtime_.repaintAfterFrame_ = false;
            outerFrame_ = true;
        }

        FrameGuard(const FrameGuard&) = delete;
        FrameGuard& operator=(const FrameGuard&) = delete;

        ~FrameGuard() {
            if (!outerFrame_) {
                return;
            }
            if (runtime_.repaintAfterFrame_) {
                runtime_.runner_.paintRequested = true;
            }
            runtime_.frameActive_ = false;
        }

        bool isOuterFrame() const { return outerFrame_; }

    private:
        MainWindowRuntime& runtime_;
        bool outerFrame_ = false;
    };

    AppRunner& runner_;
    bool frameActive_ = false;
    bool repaintAfterFrame_ = false;
};

} // namespace app
