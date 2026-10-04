// T2：DslWindowRuntime 的 resize compose 节流（无头单测）。
//
// DslWindowRuntime::update 只做 uiScale 折算，主体在 detail::updateWindowFrame 里；
// 这里用 fake runtime 驱动**同一段生产代码**，验四件事：
//   1) 只有"纯尺寸变化"这一种 compose 来源被节流——首帧 / 显式 updateRequested /
//      runtime composeRequested 三条路径一律立即 compose；
//   2) 连续 60 个 5ms 尺寸 tick → compose 次数接近每 50ms 一次，而不是每个 tick 一次；
//   3) 尺寸事件停下后 ≤50ms 必有一次终态补帧（pending 通过 nextTimerWakeSeconds 暴露）；
//   4) 节流 pending 不进 isAnimating()：空闲不会被永久标成动画。
//
// 计时只吃 update 的 deltaSeconds，不读墙钟 —— 所以用确定性的假帧间隔就能覆盖全部时序。

#include "core/app/dsl_window_runtime.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace {

int g_failures = 0;

void checkImpl(bool ok, const char* expression, int line) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAILED (line %d): %s\n", line, expression);
    }
}

#define CHECK(cond) checkImpl(static_cast<bool>(cond), #cond, __LINE__)

/** 只需要"被调用了几次"的假运行时；compose 回调不执行（测试不关心 UI 内容）。 */
struct FakeRuntime {
    int composeCalls = 0;
    int updateCalls = 0;
    int fullPaintCalls = 0;
    float lastComposedWidth = -1.0f;
    float lastComposedHeight = -1.0f;
    bool composeRequestedFlag = false;
    bool updateReportsChanged = false;
    bool animatingFlag = false;
    float timerWakeSeconds = std::numeric_limits<float>::infinity();

    template <typename ComposeFn>
    void compose(const std::string&, float logicalWidth, float logicalHeight, ComposeFn&& composeFn) {
        ++composeCalls;
        lastComposedWidth = logicalWidth;
        lastComposedHeight = logicalHeight;
        // 照真实 Runtime::compose 的调用形态把回调跑一遍，验证 request.compose 接线没断。
        core::dsl::Ui ui;
        const core::dsl::Screen screen{logicalWidth, logicalHeight};
        composeFn(ui, screen);
    }

    bool update(core::window::Handle, float, float, float, bool) {
        ++updateCalls;
        return updateReportsChanged;
    }

    bool composeRequested() const {
        return composeRequestedFlag;
    }

    void requestFullPaint() {
        ++fullPaintCalls;
    }

    bool isAnimating() const {
        return animatingFlag;
    }

    float nextTimerWakeSeconds() const {
        return timerWakeSeconds;
    }
};

struct FakeRequest {
    std::string pageId = "page";
    mutable int composeCalls = 0;

    // 生产里的 DslWindowRequest::compose 是 std::function（operator() 为 const），
    // update 主体拿到的是 const 请求对象，这里照抄同样的 const 签名。
    void compose(core::dsl::Ui&, const core::dsl::Screen&) const {
        ++composeCalls;
    }
};

/**
 * 复刻 DslWindowRuntime 的窗口状态与 update 入口；主体走生产代码
 * detail::updateWindowFrame（真实类只做 uiScale 折算后转发到这里）。
 */
struct Harness {
    FakeRuntime runtime;
    FakeRequest request;
    app::ResizeComposeThrottle throttle;
    bool composed = false;
    float composedWidth = 0.0f;
    float composedHeight = 0.0f;
    bool paintRequested = true;

    bool update(float deltaSeconds, float width, float height, bool updateRequested = false) {
        return app::detail::updateWindowFrame(runtime,
                                              request,
                                              throttle,
                                              composed,
                                              composedWidth,
                                              composedHeight,
                                              paintRequested,
                                              {nullptr, deltaSeconds, width, height, 1.0f, 1.0f, updateRequested, true});
    }

    /** 主循环拿到的"该睡多久"（运行时定时器与尺寸补帧取更早者）。 */
    float wakeSeconds() const {
        return app::detail::composeThrottleWakeSeconds(runtime.nextTimerWakeSeconds(), throttle);
    }
};

// 首帧：没有任何已 composed 状态，即使 deltaSeconds 为 0 也必须立即 compose。
void firstFrameIsNeverThrottled() {
    Harness h;
    CHECK(h.update(0.0f, 800.0f, 600.0f));
    CHECK(h.composed);
    CHECK(h.runtime.composeCalls == 1);
    CHECK(h.composedWidth == 800.0f && h.composedHeight == 600.0f);
    CHECK(h.throttle.composeCount() == 1);
    CHECK(h.request.composeCalls == h.runtime.composeCalls); // 应用层 compose 回调确实被调用
    CHECK(!h.throttle.sizeComposePending());
}

// 被跳过的尺寸 tick：不 compose、**不更新** composedWidth/Height、不置重绘，只留 pending。
void skippedTickKeepsComposedState() {
    Harness h;
    h.update(0.016f, 800.0f, 600.0f);
    h.paintRequested = false;

    h.update(0.005f, 810.0f, 610.0f); // 距上次 compose 只有 5ms → 跳过
    CHECK(h.throttle.sizeComposePending());
    CHECK(h.runtime.composeCalls == 1);
    CHECK(h.composedWidth == 800.0f && h.composedHeight == 600.0f);
    CHECK(h.throttle.skippedSizeComposeCount() == 1);
    CHECK(!h.paintRequested); // 本帧没有任何"必须重绘"的来源
    CHECK(std::isfinite(h.wakeSeconds())); // pending 必须能唤醒空闲主循环
}

// 核心场景：60 个 5ms 尺寸 tick（共 300ms）→ 节流；随后 100ms 无新尺寸事件 → 补一次终态帧。
void resizeTicksAreThrottledThenCatchUp() {
    Harness h;
    h.update(0.016f, 800.0f, 600.0f); // 首帧
    CHECK(h.runtime.composeCalls == 1);

    constexpr int kTicks = 60;
    constexpr float kTickSeconds = 0.005f;
    constexpr float kBaseWidth = 800.0f;
    constexpr float kBaseHeight = 600.0f;
    for (int i = 1; i <= kTicks; ++i) {
        h.update(kTickSeconds, kBaseWidth + i, kBaseHeight + i);
    }

    const int composeDuringResize = h.throttle.composeCount(); // 含首帧
    const int sizeComposes = composeDuringResize - 1;          // 扣掉首帧：纯尺寸来源的 compose 数
    const int skipped = h.throttle.skippedSizeComposeCount();
    std::printf("  60 x 5ms resize: composes=%d (含首帧 1 次), 尺寸来源=%d, 跳过=%d\n",
                composeDuringResize, sizeComposes, skipped);

    // 300ms ÷ 50ms ≈ 6 次 → 合理区间 5~8（下界：不可能比窗口数还少；上界：6 + 首帧 + 窗口边界的
    // 浮点/帧对齐抖动）。**不能**写成 ≤5：50ms 窗口压不出"300ms 只 compose 5 次含首帧"这种口径，
    // 那与窗口本身自相矛盾。
    CHECK(composeDuringResize >= 5 && composeDuringResize <= 8);
    CHECK(sizeComposes >= 4 && sizeComposes <= 7);
    // 每个 tick 要么 compose、要么被跳过，两者互补
    CHECK(skipped == kTicks - sizeComposes);
    // 事件停在最后一个 tick，50ms 窗口还没走完 → 终态欠账必须留着
    CHECK(h.throttle.sizeComposePending());
    // 跳过期间不写 composed 尺寸
    CHECK(h.composedWidth < kBaseWidth + kTicks);

    // ---- 事件停下：100ms 内只允许补一次，且必须是最终尺寸 ----
    const int beforeSilence = h.throttle.composeCount();
    const float finalWidth = kBaseWidth + kTicks;
    const float finalHeight = kBaseHeight + kTicks;
    for (int i = 0; i < 20; ++i) { // 20 x 5ms = 100ms，尺寸不再变化
        h.update(0.005f, finalWidth, finalHeight);
    }
    const int catchUpComposes = h.throttle.composeCount() - beforeSilence;
    std::printf("  100ms silence: 补帧 compose=%d, 最后补帧时刻 <= 50ms\n", catchUpComposes);
    CHECK(catchUpComposes == 1);
    CHECK(h.composedWidth == finalWidth && h.composedHeight == finalHeight);
    CHECK(h.runtime.lastComposedWidth == finalWidth);
    CHECK(!h.throttle.sizeComposePending());
    CHECK(std::isinf(h.wakeSeconds())); // 欠账清了 → 主循环恢复可以无限期睡
    CHECK(!h.runtime.isAnimating());    // 空闲不标动画（pending 走定时唤醒，不走 animating）

    // 补帧之后继续静止 100ms：不得再多 compose
    const int afterCatchUp = h.throttle.composeCount();
    for (int i = 0; i < 20; ++i) {
        h.update(0.005f, finalWidth, finalHeight);
    }
    CHECK(h.throttle.composeCount() == afterCatchUp);

    // 单次大 delta（事件停了、主循环被 50ms 唤醒超时一次醒来）同样必须补帧
    Harness h2;
    h2.update(0.016f, kBaseWidth, kBaseHeight);
    h2.update(0.005f, kBaseWidth + 1.0f, kBaseHeight + 1.0f);
    CHECK(h2.throttle.sizeComposePending());
    h2.update(0.100f, kBaseWidth + 7.0f, kBaseHeight + 7.0f); // 100ms 后的一帧
    CHECK(h2.throttle.composeCount() == 2);
    CHECK(h2.composedWidth == kBaseWidth + 7.0f);
    CHECK(!h2.throttle.sizeComposePending());
}

// 慢速 resize（每 60ms 一个 tick）本来就不该被节流：窗口已经过了。
void slowResizeIsNotThrottled() {
    Harness h;
    h.update(0.016f, 800.0f, 600.0f);
    for (int i = 1; i <= 5; ++i) {
        h.update(0.060f, 800.0f + i, 600.0f + i);
    }
    CHECK(h.throttle.composeCount() == 6); // 每个 tick 都 compose
    CHECK(h.composedWidth == 805.0f);
    CHECK(h.throttle.skippedSizeComposeCount() == 0);
    CHECK(!h.throttle.sizeComposePending());
}

// 显式 updateRequested（内容驱动）：即使在窗口内、即使尺寸刚被跳过，也立即 compose。
void updateRequestedIsNeverThrottled() {
    Harness h;
    h.update(0.016f, 800.0f, 600.0f); // 首帧
    h.update(0.005f, 810.0f, 610.0f); // 尺寸 tick 被跳过
    CHECK(h.throttle.sizeComposePending());
    CHECK(h.runtime.composeCalls == 1);

    h.update(0.005f, 820.0f, 620.0f, /*updateRequested=*/true);
    CHECK(h.runtime.composeCalls == 2);
    CHECK(h.composedWidth == 820.0f && h.composedHeight == 620.0f);
    CHECK(!h.throttle.sizeComposePending());

    // 尺寸不变的纯内容驱动 compose 也照旧
    h.update(0.001f, 820.0f, 620.0f, /*updateRequested=*/true);
    CHECK(h.runtime.composeCalls == 3);
    CHECK(h.throttle.composeCount() == 3);
}

// runtime composeRequested（状态变化）：立即 compose + 重建完整缓存，不受时间闸约束。
void composeRequestedPathIsNeverThrottled() {
    Harness h;
    h.update(0.016f, 800.0f, 600.0f); // 首帧

    h.runtime.composeRequestedFlag = true;
    h.update(0.005f, 810.0f, 610.0f); // 尺寸 tick 本身被跳过，但 composeRequested 路径照旧
    CHECK(h.runtime.composeCalls == 2);
    CHECK(h.runtime.fullPaintCalls == 1); // 照旧重建完整缓存
    CHECK(h.composedWidth == 810.0f && h.composedHeight == 610.0f);
    CHECK(!h.throttle.sizeComposePending());
    CHECK(h.runtime.updateCalls == 3); // 每帧一次 update + compose 后再同步一轮
    CHECK(h.paintRequested);
}

// pending 的唤醒语义：主循环据此睡到窗口结束，醒来补终态帧。
void pendingIsExposedThroughTimerWake() {
    Harness idle;
    CHECK(std::isinf(idle.wakeSeconds())); // 既无定时器也无 pending → 可以无限期睡

    idle.runtime.timerWakeSeconds = 0.02f;
    CHECK(idle.wakeSeconds() == 0.02f); // 运行时定时器原样透传

    Harness h;
    h.update(0.016f, 800.0f, 600.0f);
    h.update(0.005f, 810.0f, 610.0f); // 被跳过 → pending
    CHECK(h.throttle.sizeComposePending());
    const float pendingWake = h.wakeSeconds();
    std::printf("  pending wake = %.4fs (窗口 %.0fms)\n",
                pendingWake, app::ResizeComposeThrottle::kWindowSeconds * 1000.0f);
    CHECK(std::isfinite(pendingWake));
    CHECK(pendingWake > 0.0f && pendingWake <= app::ResizeComposeThrottle::kWindowSeconds);

    h.runtime.timerWakeSeconds = 0.01f;
    CHECK(h.wakeSeconds() == 0.01f); // 两者都有 → 取更早的
    h.runtime.timerWakeSeconds = 0.5f;
    CHECK(h.wakeSeconds() == pendingWake); // 定时器更晚 → 用尺寸补帧的窗口
}

// 拖回原尺寸：已 composed 的状态就是终态，pending 直接作废，不补帧。
void revertingSizeDropsPendingWithoutCompose() {
    Harness h;
    h.update(0.016f, 800.0f, 600.0f);
    h.update(0.005f, 810.0f, 610.0f);
    CHECK(h.throttle.sizeComposePending());

    h.update(0.005f, 800.0f, 600.0f);
    CHECK(h.runtime.composeCalls == 1);
    CHECK(!h.throttle.sizeComposePending());
    CHECK(std::isinf(h.wakeSeconds()));
}

// 零/负 delta（模态循环里时间戳没动、系统时钟回拨）：不推进窗口，但不破坏状态。
void nonPositiveDeltaIsIgnored() {
    Harness h;
    h.update(0.0f, 800.0f, 600.0f); // 首帧不依赖时间
    h.update(0.0f, 810.0f, 610.0f);
    CHECK(h.throttle.sizeComposePending());
    CHECK(h.runtime.composeCalls == 1);
    h.update(-0.5f, 820.0f, 620.0f);
    CHECK(h.runtime.composeCalls == 1);
    CHECK(h.throttle.sizeComposePending());
}

// 空闲态：节流 pending 不进 isAnimating()，按需渲染空闲不会被标成动画。
void idleIsNotMarkedAnimating() {
    app::DslWindowRuntime runtime; // 未初始化的窗口：只读状态，够用来断言默认行为
    CHECK(!runtime.isAnimating());
    CHECK(!runtime.sizeComposeThrottle().sizeComposePending());
    CHECK(std::isinf(runtime.nextTimerWakeSeconds()));
    CHECK(runtime.paintRequested()); // 新窗口默认待画（现状）

    Harness h;
    h.update(0.016f, 800.0f, 600.0f);
    h.update(0.005f, 810.0f, 610.0f); // pending 中
    CHECK(h.throttle.sizeComposePending());
    CHECK(!h.runtime.isAnimating());        // 动画状态只看运行时
    CHECK(std::isfinite(h.wakeSeconds()));  // 只有"到点唤醒"，没有持续渲染
}

} // namespace

int main() {
    firstFrameIsNeverThrottled();
    skippedTickKeepsComposedState();
    resizeTicksAreThrottledThenCatchUp();
    slowResizeIsNotThrottled();
    updateRequestedIsNeverThrottled();
    composeRequestedPathIsNeverThrottled();
    pendingIsExposedThroughTimerWake();
    revertingSizeDropsPendingWithoutCompose();
    nonPositiveDeltaIsIgnored();
    idleIsNotMarkedAnimating();

    if (g_failures != 0) {
        std::printf("dsl_window_resize_throttle: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("dsl_window_resize_throttle: all checks passed\n");
    return 0;
}
