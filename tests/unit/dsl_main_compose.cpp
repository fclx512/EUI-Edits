// Exercise the real main-window compose control flow without a native window,
// render backend, or application entry point.
#include "eui/detail/dsl_app_impl.h"

#include <iostream>

namespace {

int composeCalls = 0;
int timerFires = 0;
int lastComposedTimerFires = 0;
int failures = 0;
bool timerEnabled = false;
const char* focusTarget = nullptr;
bool lastComposeSawFocus = false;
bool requestUpdateOnCompose = false;
int updatesRequestedDuringCompose = 0;
std::vector<std::uint64_t> composedSerials;
constexpr float timerInterval = 0.5f;

void check(bool condition, const char* scenario, const char* message) {
    if (!condition) {
        std::cerr << scenario << ": " << message << '\n';
        ++failures;
    }
}

} // namespace

namespace app {

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = [] {
        DslAppConfig value;
        value.pageIdValue = "dsl-main-compose-test";
        value.showDebugOverlayValue = false;
        value.showDebugStatsInTitleValue = false;
        value.debugOverlayCompose = [](eui::Ui&, const eui::Screen&) {};
        return value;
    }();
    return config;
}

void compose(eui::Ui& ui, const eui::Screen&) {
    ++composeCalls;
    composedSerials.push_back(app::updateSerial());
    lastComposedTimerFires = timerFires;
    if (focusTarget) {
        // Like InputBuilder, focus-dependent content reads the focus that was
        // known while composing, before Runtime applies this focus request.
        lastComposeSawFocus = ui.isFocused(focusTarget);
        ui.rect(focusTarget).position(0.0f, 0.0f).size(20.0f, 20.0f).focusable().build();
        ui.requestFocus(focusTarget);
    }
    if (requestUpdateOnCompose) {
        requestUpdateOnCompose = false;
        ++updatesRequestedDuringCompose;
        app::requestUpdate();
    }
    if (timerEnabled) {
        ui.rect("timer").position(0.0f, 0.0f).size(20.0f, 20.0f)
            .onTimer(timerInterval, [] { ++timerFires; }).build();
    }
}

} // namespace app

namespace {

void frame(const char* scenario, int width, int height, float dpiScale,
           bool requested, float delta, int expectedComposes,
           bool expectedChanged = true, int expectedTimerFires = 0) {
    const int beforeCompose = composeCalls;
    const int beforeTimer = timerFires;
    const auto beforeSerial = app::updateSerial();
    const auto firstCompose = composedSerials.size();
    const bool changed = app::update(nullptr, delta, width, height,
                                     dpiScale, 1.0f, requested, false);
    check(composeCalls - beforeCompose == expectedComposes, scenario, "unexpected compose count");
    check(changed == expectedChanged, scenario, "unexpected changed result");
    check(timerFires - beforeTimer == expectedTimerFires, scenario, "unexpected timer count");
    check(app::updateSerial() == beforeSerial + 1, scenario, "update serial did not advance exactly once");
    for (auto i = firstCompose; i < composedSerials.size(); ++i) {
        check(composedSerials[i] == app::updateSerial(), scenario,
              "multiple compose passes received different work-budget generations");
    }
    check(lastComposedTimerFires == timerFires, scenario, "timer state was not composed this frame");
    const auto& state = app::detail::dslAppState();
    check(state.composed && state.logicalWidth == static_cast<float>(width) / dpiScale &&
          state.logicalHeight == static_cast<float>(height) / dpiScale,
          scenario, "composed logical dimensions are stale");
}

void invalidDimensionsDoNotCompose() {
    struct InvalidFrame { int width; int height; float dpiScale; };
    const InvalidFrame invalidFrames[] = {
        {0, 620, 1.0f}, {-1, 620, 1.0f},
        {820, 0, 1.0f}, {820, -1, 1.0f},
        {820, 620, 0.0f}, {820, 620, -1.0f}
    };
    const auto beforeState = app::detail::dslAppState();
    const int beforeCompose = composeCalls;
    const auto beforeSerial = app::updateSerial();
    for (const auto& invalid : invalidFrames) {
        check(!app::update(nullptr, 0.0f, invalid.width, invalid.height,
                           invalid.dpiScale, 1.0f, true, false),
              "invalid dimensions", "invalid frame reported a change");
        const auto& state = app::detail::dslAppState();
        check(composeCalls == beforeCompose, "invalid dimensions", "invalid frame composed");
        check(app::updateSerial() == beforeSerial, "invalid dimensions", "invalid frame consumed an update serial");
        check(state.composed == beforeState.composed && state.logicalWidth == beforeState.logicalWidth &&
              state.logicalHeight == beforeState.logicalHeight,
              "invalid dimensions", "invalid frame changed composed dimensions");
    }
}

void focusRequestedDuringComposeSettles() {
    timerEnabled = false;
    // Re-enter the main app's first-composition branch with an initialized,
    // unfocused runtime. Do not constrain this state-changing path to one pass.
    app::detail::dslAppState() = {};
    focusTarget = "initial-focus";
    lastComposeSawFocus = false;
    check(app::update(nullptr, 0.0f, 800, 600, 1.0f, 1.0f, true, false),
          "initial compose focus", "focus frame did not report a change");
    check(lastComposeSawFocus, "initial compose focus",
          "final composition did not see the requested focus");

    // Resize + explicit must preserve the same convergence when focus transfers.
    focusTarget = "resize-focus";
    lastComposeSawFocus = false;
    check(app::update(nullptr, 0.0f, 810, 610, 1.0f, 1.0f, true, false),
          "resize compose focus", "focus frame did not report a change");
    check(lastComposeSawFocus, "resize compose focus",
          "final composition did not see the transferred focus");
    focusTarget = nullptr;
}

void composeUpdateRequestSurvivesUntilNextFrame() {
    // Clear any unrelated setup request, then use the real consuming overload.
    // It consumes exactly once at each frame's start, without peeking at the
    // pending flag and accidentally taking the request intended for next frame.
    core::platform::consumeUiUpdate();
    requestUpdateOnCompose = true;
    app::requestUpdate();
    check(app::update(nullptr, 0.0f, 810, 610, 1.0f, 1.0f),
          "compose requests update", "initial requested frame did not change");
    check(updatesRequestedDuringCompose == 1, "compose requests update",
          "compose did not issue exactly one additional update request");

    const int beforeNextFrame = composeCalls;
    check(app::update(nullptr, 0.0f, 810, 610, 1.0f, 1.0f),
          "compose requests update", "next frame lost the update request");
    check(composeCalls > beforeNextFrame, "compose requests update",
          "next frame did not compose the pending update");

    const int beforeIdleFrame = composeCalls;
    check(!app::update(nullptr, 0.0f, 810, 610, 1.0f, 1.0f),
          "compose requests update", "one-shot request left another changed frame");
    check(composeCalls == beforeIdleFrame && updatesRequestedDuringCompose == 1,
          "compose requests update", "one-shot request did not return to idle");
}

} // namespace

int main() {
    auto& runtime = app::detail::dslRuntime();
    runtime.setKeyEventHandler(app::dslAppConfig().keyEventHandler);
    if (!runtime.initialize()) return 1;

    frame("first plus explicit", 800, 600, 1.0f, true, 0.0f, 1);
    frame("size plus explicit", 810, 610, 1.0f, true, 0.0f, 1);
    frame("size only", 820, 620, 1.0f, false, 0.0f, 1);
    frame("explicit only", 820, 620, 1.0f, true, 0.0f, 1);
    frame("no change", 820, 620, 1.0f, false, 0.0f, 0, false);
    invalidDimensionsDoNotCompose();

    // Identical physical dimensions with a new DPI still require new layout.
    frame("DPI changes logical dimensions", 820, 620, 2.0f, false, 0.0f, 1);
    frame("unchanged DPI and dimensions", 820, 620, 2.0f, false, 0.0f, 0, false);

    // Arm the real timer first: a newly activated timer ignores its first delta.
    timerEnabled = true;
    frame("timer setup", 820, 620, 2.0f, true, 0.0f, 1);
    // Coalesce size + explicit before update, but preserve the necessary compose
    // after the timer changes state. Its zero-delta update must not fire again.
    frame("size explicit and timer", 840, 640, 2.0f, true, timerInterval, 2, true, 1);
    frame("timer only", 840, 640, 2.0f, false, timerInterval, 1, true, 1);

    focusRequestedDuringComposeSettles();
    composeUpdateRequestSurvivesUntilNextFrame();

    runtime.shutdown(false);
    core::releaseInputQueue(nullptr);
    core::TextPrimitive::setLayoutPixelScale(1.0f);
    return failures == 0 ? 0 : 1;
}
