#include "core/dsl_runtime.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

constexpr float kInterval = 0.5f;

bool close(float actual, float expected) {
    return std::fabs(actual - expected) < 0.001f;
}

void composeTimer(core::dsl::Runtime& runtime,
                  bool enabled,
                  float interval,
                  int& callbackCount) {
    runtime.compose("runtime-timer-test", 120.0f, 80.0f,
                    [enabled, interval, &callbackCount](core::dsl::Ui& ui,
                                                       const core::dsl::Screen&) {
        auto timer = ui.rect("pulse").position(0.0f, 0.0f).size(20.0f, 20.0f);
        if (enabled) {
            timer.onTimer(interval, [&callbackCount] { ++callbackCount; });
        }
        timer.build();
    });
}

struct RuntimeHarness {
    int windowTag = 0;
    core::window::Handle window = reinterpret_cast<core::window::Handle>(&windowTag);
    core::dsl::Runtime runtime;

    bool initialize() { return runtime.initialize(); }

    void finish() {
        runtime.shutdown();
        core::releaseInputQueue(window);
    }
};

bool firstFrameIgnoresPreActivationDelta() {
    RuntimeHarness harness;
    if (!harness.initialize()) return false;
    int callbackCount = 0;
    composeTimer(harness.runtime, true, kInterval, callbackCount);

    harness.runtime.update(harness.window, 10.0f, 1.0f, 1.0f);
    const bool valid = callbackCount == 0 &&
        close(harness.runtime.nextTimerWakeSeconds(), kInterval) &&
        !harness.runtime.isAnimating() && !harness.runtime.composeRequested();
    harness.finish();
    if (!valid) {
        std::cerr << "new timer consumed the pre-activation delta or requested continuous frames\n";
    }
    return valid;
}

bool activeTimerKeepsPeriodicRemainderAndSleepsBetweenTicks() {
    RuntimeHarness harness;
    if (!harness.initialize()) return false;
    int callbackCount = 0;
    composeTimer(harness.runtime, true, kInterval, callbackCount);
    harness.runtime.update(harness.window, 10.0f, 1.0f, 1.0f);
    if (callbackCount != 0 || !close(harness.runtime.nextTimerWakeSeconds(), kInterval)) {
        harness.finish();
        std::cerr << "initial timer frame did not start from zero\n";
        return false;
    }

    harness.runtime.update(harness.window, 0.125f, 1.0f, 1.0f);
    if (callbackCount != 0 || !close(harness.runtime.nextTimerWakeSeconds(), 0.375f) ||
        harness.runtime.isAnimating() || harness.runtime.composeRequested()) {
        harness.finish();
        std::cerr << "timer wake did not track its remaining interval without animating\n";
        return false;
    }

    harness.runtime.update(harness.window, 0.50f, 1.0f, 1.0f);
    if (callbackCount != 1 || !close(harness.runtime.nextTimerWakeSeconds(), 0.375f) ||
        harness.runtime.isAnimating()) {
        harness.finish();
        std::cerr << "periodic timer did not preserve the 0.125 second remainder\n";
        return false;
    }

    harness.runtime.update(harness.window, 0.375f, 1.0f, 1.0f);
    const bool valid = callbackCount == 2 &&
        close(harness.runtime.nextTimerWakeSeconds(), kInterval) &&
        !harness.runtime.isAnimating();
    harness.finish();
    if (!valid) {
        std::cerr << "timer did not continue periodically after preserving its remainder\n";
    }
    return valid;
}

bool changedIntervalStartsAtZero() {
    RuntimeHarness harness;
    if (!harness.initialize()) return false;
    int callbackCount = 0;
    composeTimer(harness.runtime, true, kInterval, callbackCount);
    harness.runtime.update(harness.window, 0.0f, 1.0f, 1.0f);
    harness.runtime.update(harness.window, 0.20f, 1.0f, 1.0f);

    constexpr float changedInterval = 0.8f;
    composeTimer(harness.runtime, true, changedInterval, callbackCount);
    harness.runtime.update(harness.window, 10.0f, 1.0f, 1.0f);
    if (callbackCount != 0 ||
        !close(harness.runtime.nextTimerWakeSeconds(), changedInterval) ||
        harness.runtime.isAnimating()) {
        harness.finish();
        std::cerr << "interval change consumed delta from before the new interval\n";
        return false;
    }

    harness.runtime.update(harness.window, 0.30f, 1.0f, 1.0f);
    const bool valid = callbackCount == 0 &&
        close(harness.runtime.nextTimerWakeSeconds(), 0.50f);
    harness.finish();
    if (!valid) {
        std::cerr << "changed timer interval did not begin counting on the next frame\n";
    }
    return valid;
}

bool reenabledTimerStartsAtZeroAfterHiddenPeriod() {
    RuntimeHarness harness;
    if (!harness.initialize()) return false;
    int callbackCount = 0;
    composeTimer(harness.runtime, true, kInterval, callbackCount);
    harness.runtime.update(harness.window, 0.0f, 1.0f, 1.0f);

    composeTimer(harness.runtime, false, kInterval, callbackCount);
    harness.runtime.update(harness.window, 10.0f, 1.0f, 1.0f);
    if (!std::isinf(harness.runtime.nextTimerWakeSeconds()) || callbackCount != 0) {
        harness.finish();
        std::cerr << "hidden timer remained scheduled or fired while inactive\n";
        return false;
    }

    composeTimer(harness.runtime, true, kInterval, callbackCount);
    harness.runtime.update(harness.window, 10.0f, 1.0f, 1.0f);
    if (callbackCount != 0 || !close(harness.runtime.nextTimerWakeSeconds(), kInterval)) {
        harness.finish();
        std::cerr << "re-enabled timer consumed the hidden-period delta\n";
        return false;
    }

    harness.runtime.update(harness.window, 0.25f, 1.0f, 1.0f);
    if (callbackCount != 0 || !close(harness.runtime.nextTimerWakeSeconds(), 0.25f)) {
        harness.finish();
        std::cerr << "re-enabled timer did not count from its activation frame\n";
        return false;
    }
    harness.runtime.update(harness.window, 0.25f, 1.0f, 1.0f);
    const bool valid = callbackCount == 1 &&
        close(harness.runtime.nextTimerWakeSeconds(), kInterval) &&
        !harness.runtime.isAnimating();
    harness.finish();
    if (!valid) {
        std::cerr << "re-enabled timer failed to fire at its new interval\n";
    }
    return valid;
}

bool hoverAfterSleepStartsAnimationWithoutChangingTimerClock() {
    RuntimeHarness harness;
    if (!harness.initialize()) return false;
    int callbacks = 0;
    harness.runtime.compose("idle-hover", 120.0f, 80.0f,
        [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            ui.rect("hover").size(60.0f, 40.0f)
                .states({0.1f, 0.1f, 0.1f, 1.0f}, {0.3f, 0.3f, 0.3f, 1.0f},
                        {0.5f, 0.5f, 0.5f, 1.0f})
                .onClick([] {})
                .onTimer(kInterval, [&] { ++callbacks; }).build();
        });
    core::queuePointerMotion(harness.window, 100, 70, {}, {});
    harness.runtime.update(harness.window, 0.0f, 1.0f, 1.0f);
    core::queuePointerMotion(harness.window, 20, 20, {}, {});
    harness.runtime.update(harness.window, 2.0f, 1.0f, 1.0f);
    if (!harness.runtime.isAnimating() || callbacks != 1) {
        harness.finish();
        std::cerr << "idle hover skipped its animation or timer lost elapsed time\n";
        return false;
    }
    // Hover callbacks can request a second compose/update in the same frame.
    // That zero-time pass must not snap the smoothing to its final target.
    harness.runtime.update(harness.window, 0.0f, 1.0f, 1.0f);
    if (!harness.runtime.isAnimating() || callbacks != 1) {
        harness.finish();
        std::cerr << "same-frame update snapped hover or advanced the timer\n";
        return false;
    }
    // An already active visual consumes the actual next elapsed interval.
    harness.runtime.update(harness.window, 1.0f, 1.0f, 1.0f);
    if (harness.runtime.isAnimating()) {
        harness.finish();
        std::cerr << "active hover animation did not finish after elapsed time\n";
        return false;
    }
    core::queuePointerMotion(harness.window, 100, 70, {}, {});
    harness.runtime.update(harness.window, 2.0f, 1.0f, 1.0f);
    const bool exitAnimating = harness.runtime.isAnimating();
    harness.runtime.update(harness.window, 1.0f, 1.0f, 1.0f);
    const bool settled = !harness.runtime.isAnimating();
    harness.finish();
    return exitAnimating && settled;
}

} // namespace

int main() {
    if (!firstFrameIgnoresPreActivationDelta()) return 1;
    if (!activeTimerKeepsPeriodicRemainderAndSleepsBetweenTicks()) return 1;
    if (!changedIntervalStartsAtZero()) return 1;
    if (!reenabledTimerStartsAtZeroAfterHiddenPeriod()) return 1;
    if (!hoverAfterSleepStartsAnimationWithoutChangingTimerClock()) return 1;
    return 0;
}
