#pragma once

#include "eui/app.h"
#include "core/dsl_runtime.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace app {

/**
 * @brief T2：**纯尺寸变化**来源的 compose 节流闸。
 *
 * 拖拽窗口边框时 OS 每秒派发几十个尺寸 tick，每个 tick 都整树 compose（布局 + 全量塑形）
 * 会把主线程拖垮。这里只给"尺寸变化"这一种 compose 来源加时间闸：两次尺寸 compose 之间
 * 至少隔 `kWindowSeconds`（约 50ms）；窗口内再来的尺寸 tick 直接跳过——**不更新**
 * composedWidth/Height，只置 pending。
 *
 * 不被节流的三条来源（首帧 / 显式 updateRequested / runtime 的 composeRequested）
 * 都绕过本闸直接 compose。
 *
 * 时间基准只累加 update() 传进来的帧间隔（advance()），**不读墙钟**：主循环空闲时会停在
 * glfwWaitEvents 里，读墙钟会把睡眠抖动算进窗口；帧间隔本身就覆盖了睡过头的时长，所以
 * 事件一停，下一帧的 deltaSeconds 立刻把窗口补满 → 终态必有一次 compose。
 *
 * pending 通过 DslWindowRuntime::nextTimerWakeSeconds() 暴露给主循环：主循环据此带超时地睡，
 * 到点醒来再走一次 update 补终态帧（≤50ms）。**故意不**从 isAnimating() 暴露：那会把空闲
 * 主循环推进持续 poll（烧 CPU），也会把"按需渲染空闲"标成动画。
 */
class ResizeComposeThrottle {
public:
    /** 目标节流窗口：两次"纯尺寸变化" compose 之间至少间隔这么久（约 50ms）。 */
    static constexpr float kWindowSeconds = 0.050f;

    /** 窗口/应用生命周期边界（initialize/shutdown）：清计时、清 pending、清计数。 */
    void reset() {
        elapsedSeconds_ = 0.0f;
        sizeComposePending_ = false;
        composeCount_ = 0;
        skippedSizeComposeCount_ = 0;
    }

    /** 每次 update() 开头调用：只吃 update 的 deltaSeconds，负值/零值忽略。 */
    void advance(float deltaSeconds) {
        if (deltaSeconds > 0.0f) {
            elapsedSeconds_ += deltaSeconds;
        }
    }

    /**
     * @brief 本帧是否需要为"尺寸变化"compose。
     *
     * - 尺寸没变 → 清 pending（拖回原尺寸时已经 composed 的状态就是终态，无需补帧），返回 false；
     * - 尺寸变了且距上次尺寸 compose ≥ 窗口 → true（本帧 compose）；
     * - 尺寸变了但还在窗口内 → 置 pending、计入 skippedSizeComposeCount_，返回 false。
     *
     * 调用方跳过时不会写 composedWidth/Height，所以本闸的"尺寸变了"判定每帧都会重算，
     * pending 与真实尺寸差始终保持一致。
     */
    bool shouldComposeForSize(bool sizeChanged) {
        if (!sizeChanged) {
            sizeComposePending_ = false;
            return false;
        }
        if (elapsedSeconds_ >= kWindowSeconds) {
            return true;
        }
        sizeComposePending_ = true;
        ++skippedSizeComposeCount_;
        return false;
    }

    /** 一次 compose 已经把当前尺寸写进 composed 状态：清 pending、重置计时，累计一次 compose。 */
    void noteComposed() {
        elapsedSeconds_ = 0.0f;
        sizeComposePending_ = false;
        ++composeCount_;
    }

    /** 是否有一帧"尺寸已变但还没 compose"的欠账。 */
    bool sizeComposePending() const {
        return sizeComposePending_;
    }

    /** pending 的补帧还剩多少秒（+inf = 没有 pending）。主循环据此睡到窗口结束。 */
    float sizeComposeWakeSeconds() const {
        if (!sizeComposePending_) {
            return std::numeric_limits<float>::infinity();
        }
        return std::max(0.0f, kWindowSeconds - elapsedSeconds_);
    }

    /** 累计执行过的 compose 次数（含首帧/updateRequested/composeRequested 路径），只读。 */
    int composeCount() const {
        return composeCount_;
    }

    /** 累计被本闸跳过的尺寸 tick 数，只读。 */
    int skippedSizeComposeCount() const {
        return skippedSizeComposeCount_;
    }

private:
    float elapsedSeconds_ = 0.0f;
    bool sizeComposePending_ = false;
    int composeCount_ = 0;
    int skippedSizeComposeCount_ = 0;
};

namespace detail {

/** DslWindowRuntime::update 的入参（抽成结构体，便于给 update 主体瘦身 + 单测直调）。 */
struct WindowUpdateArgs {
    core::window::Handle window = nullptr;
    float deltaSeconds = 0.0f;
    /** 本帧的目标逻辑尺寸（调用方已按 uiScale 折算）。 */
    float logicalWidth = 0.0f;
    float logicalHeight = 0.0f;
    float pointerScale = 1.0f;
    float effectiveScale = 1.0f;
    bool updateRequested = false;
    bool inputEnabled = true;
};

/**
 * @brief DslWindowRuntime::update 的主体，抽成模板是为了能用 fake runtime 单测节流语义。
 *
 * 语义与改造前一致，唯一差别是：只有"尺寸变化"这一种 compose 来源过
 * ResizeComposeThrottle 时间闸；首帧（!composed）、显式 updateRequested、
 * runtime 的 composeRequested 三条路径一律立即 compose。
 *
 * composed / composedWidth / composedHeight / paintRequested 是宿主窗口的状态，
 * 以引用传入（跳过尺寸 compose 时**不**写 composedWidth/Height）。
 */
template <typename RuntimeT, typename RequestT>
bool updateWindowFrame(RuntimeT& runtime,
                       const RequestT& request,
                       ResizeComposeThrottle& throttle,
                       bool& composed,
                       float& composedWidth,
                       float& composedHeight,
                       bool& paintRequested,
                       const WindowUpdateArgs& args) {
    // 时间基准：只累加 update 的帧间隔，不读墙钟。最小化/零尺寸时主循环根本不调 update，
    // 计时原地冻结；恢复后第一帧的帧间隔把停摆时长算回来 → 立即越过窗口 → 立即 compose。
    throttle.advance(args.deltaSeconds);

    bool changed = false;

    const auto composeFrame = [&] {
        runtime.compose(request.pageId, args.logicalWidth, args.logicalHeight,
            [&](core::dsl::Ui& ui, const core::dsl::Screen& screen) {
                request.compose(ui, screen);
            });
        composed = true;
        composedWidth = args.logicalWidth;
        composedHeight = args.logicalHeight;
        throttle.noteComposed();
    };

    // 只有"已经 composed 之后尺寸又变了"才是可节流的尺寸来源；首帧 (!composed) 单独放行。
    const bool sizeChanged =
        composed && (composedWidth != args.logicalWidth || composedHeight != args.logicalHeight);
    const bool composeForSize = throttle.shouldComposeForSize(sizeChanged);

    if (!composed || composeForSize || args.updateRequested) {
        composeFrame();
        paintRequested = true;
        changed = true;
    }

    if (runtime.update(args.window, args.deltaSeconds, args.pointerScale, args.effectiveScale, args.inputEnabled)) {
        paintRequested = true;
        changed = true;
    }

    if (runtime.composeRequested()) {
        // A compose can change retained content without changing the element structure.
        // Rebuild the complete cache so state-driven text is visible immediately.
        runtime.requestFullPaint();
        composeFrame();
        if (runtime.update(args.window, 0.0f, args.pointerScale, args.effectiveScale, args.inputEnabled)) {
            changed = true;
        }
        paintRequested = true;
        changed = true;
    }

    return changed;
}

/**
 * @brief 运行时定时器与"尺寸补帧"pending 取更早的那个唤醒时刻（+inf = 都没有）。
 *
 * 抽出来是为了能单测：pending 时必须返回 (0, kWindowSeconds] 内的有限值，
 * 否则空闲主循环会停在 glfwWaitEvents 里，终态 compose 永远等不到。
 */
inline float composeThrottleWakeSeconds(float runtimeWakeSeconds, const ResizeComposeThrottle& throttle) {
    const float throttleWakeSeconds = throttle.sizeComposeWakeSeconds();
    if (!std::isfinite(throttleWakeSeconds)) {
        return runtimeWakeSeconds;
    }
    if (!std::isfinite(runtimeWakeSeconds)) {
        return throttleWakeSeconds;
    }
    return std::min(runtimeWakeSeconds, throttleWakeSeconds);
}

} // namespace detail

class DslWindowRuntime {
public:
    bool initialize(core::window::Handle window, DslWindowRequest request) {
        request_ = std::move(request);
        paintRequested_ = true;
        runtime_.setKeyEventHandler(request_.onKeyEvent);
        // 登记主窗口句柄：应用层菜单命令（右键菜单"粘贴"等）要读剪贴板。
        core::window::setMainWindowHandle(window);
        sizeComposeThrottle_.reset();
        return runtime_.initialize(window);
    }

    void shutdown(bool releaseCachedImageTextures = false) {
        runtime_.shutdown(releaseCachedImageTextures);
        request_ = {};
        composed_ = false;
        paintRequested_ = false;
        logicalWidth_ = 0.0f;
        logicalHeight_ = 0.0f;
        sizeComposeThrottle_.reset();
    }

    const DslWindowRequest& request() const {
        return request_;
    }

    /**
     * @brief 只看运行时自身的动画状态。
     *
     * resize 节流的 pending **不**从这里暴露：它由 nextTimerWakeSeconds() 驱动一次带超时的
     * 唤醒即可，从这里暴露只会让空闲主循环退化成持续 poll，并把按需渲染空闲标成动画。
     */
    bool isAnimating() const {
        return runtime_.isAnimating();
    }

    /** @brief 该窗口最近一个 pending 定时器还剩多少秒（+inf = 没有）。 */
    float nextTimerWakeSeconds() const {
        // 尺寸 compose 被节流出欠账时，必须比任何定时器都早醒，否则空闲时没人来补终态帧。
        return detail::composeThrottleWakeSeconds(runtime_.nextTimerWakeSeconds(), sizeComposeThrottle_);
    }

    /** @brief 尺寸 compose 节流闸（只读），给测试/诊断用的计数与 pending 状态。 */
    const ResizeComposeThrottle& sizeComposeThrottle() const {
        return sizeComposeThrottle_;
    }

    bool paintRequested() const {
        return paintRequested_;
    }

    void requestPaint() {
        paintRequested_ = true;
    }

    void requestFullPaint() {
        runtime_.requestFullPaint();
        paintRequested_ = true;
    }

    bool update(core::window::Handle window,
                float deltaSeconds,
                float logicalWidth,
                float logicalHeight,
                float pointerScale,
                float dpiScale,
                bool updateRequested,
                bool inputEnabled = true) {
        const float configuredScale = uiScale();
        const float effectiveScale = dpiScale * configuredScale;
        logicalWidth /= configuredScale;
        logicalHeight /= configuredScale;

        return detail::updateWindowFrame(runtime_,
                                         request_,
                                         sizeComposeThrottle_,
                                         composed_,
                                         logicalWidth_,
                                         logicalHeight_,
                                         paintRequested_,
                                         {window,
                                          deltaSeconds,
                                          logicalWidth,
                                          logicalHeight,
                                          pointerScale,
                                          effectiveScale,
                                          updateRequested,
                                          inputEnabled});
    }

    void render(core::render::RenderBackend& renderBackend, int framebufferWidth, int framebufferHeight, float dpiScale) {
        core::render::ScopedRenderBackend scopedRenderBackend(renderBackend);
        runtime_.render(framebufferWidth, framebufferHeight, dpiScale * uiScale(), request_.clearColor);
        paintRequested_ = runtime_.paintRequested();
    }

private:
    core::dsl::Runtime runtime_;
    DslWindowRequest request_;
    ResizeComposeThrottle sizeComposeThrottle_;
    bool composed_ = false;
    bool paintRequested_ = true;
    float logicalWidth_ = 0.0f;
    float logicalHeight_ = 0.0f;
};

} // namespace app
