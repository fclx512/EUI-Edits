#include "core/runtime/reveal_state.h"
#include "core/input/input_state.h"
#include "core/dsl_runtime.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

bool near(float a, float b, float epsilon = 0.001f) {
    return std::fabs(a - b) <= epsilon;
}

core::RevealEffect config() {
    core::RevealEffect value;
    value.enabled = true;
    value.tint = {0.2f, 0.4f, 0.8f, 1.0f};
    value.radius = 72.0f;
    value.fillOpacity = 0.09f;
    value.borderOpacity = 0.75f;
    value.borderWidth = 1.0f;
    value.pressRadius = 28.0f;
    value.pressOpacity = 0.24f;
    return value;
}

void testSamplingAndLatestPointer() {
    core::dsl::runtime::RevealState state;
    const auto cfg = config();
    state.update(cfg, {0.0f, 10.0f}, 176.0f, 34.0f,
                 true, false, true, 0.0f);
    int visualPositionChanges = 0;
    int visualChanges = 0;
    float previousX = state.effect().center.x;
    for (int i = 1; i <= 240; ++i) {
        const float x = static_cast<float>(i) * 0.5f;
        if (state.update(cfg, {x, 10.0f}, 176.0f, 34.0f,
                         true, false, true, 1.0f / 240.0f)) ++visualChanges;
        if (state.effect().center.x != previousX) {
            ++visualPositionChanges;
            previousX = state.effect().center.x;
        }
    }
    check(visualPositionChanges <= 61, "high-frequency pointer is sampled at most 61 times per second");
    check(visualChanges <= 61, "all visible fields including alpha are sampled at most 61 times per second");
    state.update(cfg, {130.0f, 10.0f}, 176.0f, 34.0f,
                 true, false, true, 0.0f, false);
    check(std::isfinite(state.wakeSeconds()) && state.wakeSeconds() <= core::dsl::runtime::RevealState::kSampleIntervalSeconds,
          "latest pointer has a finite next-sample deadline");
    const float wait = state.wakeSeconds();
    state.update(cfg, {130.0f, 10.0f}, 176.0f, 34.0f,
                 true, false, true, wait);
    check(near(state.effect().center.x, 130.0f), "deadline samples the latest pointer, not an older event");
}

void testIdleLeaveAndDisable() {
    core::dsl::runtime::RevealState state;
    const auto cfg = config();
    state.update(cfg, {40.0f, 12.0f}, 176.0f, 34.0f,
                 true, false, true, 0.0f);
    for (int i = 0; i < 12 && std::isfinite(state.wakeSeconds()); ++i) {
        const float wait = state.wakeSeconds();
        state.update(cfg, {40.0f, 12.0f}, 176.0f, 34.0f,
                     true, false, true, wait);
    }
    check(!std::isfinite(state.wakeSeconds()), "settled stationary hover stops requesting wakeups");

    state.update(cfg, {-100.0f, 12.0f}, 176.0f, 34.0f,
                 false, false, true, 0.0f);
    check(std::isfinite(state.wakeSeconds()), "leave starts a bounded fade-out");
    for (int i = 0; i < 12 && std::isfinite(state.wakeSeconds()); ++i) {
        const float wait = state.wakeSeconds();
        state.update(cfg, {-100.0f, 12.0f}, 176.0f, 34.0f,
                     false, false, true, wait);
    }
    check(!state.effect().enabled && !std::isfinite(state.wakeSeconds()),
          "leave fade converges and stops");

    auto disabled = cfg;
    disabled.enabled = false;
    state.update(cfg, {40.0f, 12.0f}, 176.0f, 34.0f,
                 true, false, true, 1.0f);
    check(state.effect().enabled, "disable fixture starts from a visible state");
    check(state.update(disabled, {40.0f, 12.0f}, 176.0f, 34.0f,
                       true, false, true, 0.0f), "disable clears prior visual state immediately");
    check(!state.effect().enabled && !std::isfinite(state.wakeSeconds()),
          "disabled state has no active effect or deadline");
    state.update(cfg, {40.0f, 12.0f}, 176.0f, 34.0f,
                 true, false, true, 0.0f);
    state.update(cfg, {40.0f, 12.0f}, 176.0f, 34.0f,
                 true, false, false, 0.0f);
    check(!state.effect().enabled && !std::isfinite(state.wakeSeconds()),
          "invalid input clears state immediately");
}

void testPressAndLongIdle() {
    core::dsl::runtime::RevealState state;
    const auto cfg = config();
    state.update(cfg, {32.0f, 10.0f}, 176.0f, 34.0f,
                 true, false, true, 0.0f);
    state.update(cfg, {48.0f, 10.0f}, 176.0f, 34.0f,
                 true, true, true, 0.0f, false);
    check(state.effect().pressOpacity == 0.0f,
          "early event captures press state without sampling a partial batch");
    state.update(cfg, {48.0f, 10.0f}, 176.0f, 34.0f,
                 true, false, true, 1.0f / 60.0f);
    check(state.effect().pressOpacity > 0.0f && near(state.effect().pressCenter.x, 48.0f),
          "short press/release queue keeps one ripple and does not retrigger on release");

    core::dsl::runtime::RevealState afterIdle;
    afterIdle.update(cfg, {20.0f, 8.0f}, 176.0f, 34.0f,
                     true, false, true, 30.0f);
    check(afterIdle.effect().fillOpacity == 0.0f,
          "long idle does not skip the beginning of a new hover animation");
    const float wait = afterIdle.wakeSeconds();
    afterIdle.update(cfg, {20.0f, 8.0f}, 176.0f, 34.0f,
                     true, false, true, wait);
    check(afterIdle.effect().fillOpacity > 0.0f &&
          afterIdle.effect().fillOpacity < cfg.fillOpacity,
          "new hover animation advances by one bounded sample after idle");
}

void testProximityAndUnclampedCoordinates() {
    core::dsl::runtime::RevealState nearState;
    core::dsl::runtime::RevealState farState;
    const auto cfg = config();
    nearState.update(cfg, {-20.0f, 12.0f}, 176.0f, 34.0f,
                    false, false, true, 0.0f);
    check(nearState.effect().enabled && near(nearState.effect().center.x, -20.0f) &&
          nearState.effect().borderOpacity > 0.0f && nearState.effect().fillOpacity == 0.0f,
          "nearby outside pointer keeps its local coordinate and lights only the border");
    farState.update(cfg, {-100.0f, 12.0f}, 176.0f, 34.0f,
                    false, false, true, 0.0f);
    check(!farState.effect().enabled && !std::isfinite(farState.wakeSeconds()),
          "far outside pointer produces no effect and no wakeup");
}

void testNonClientVisualPointerDoesNotBecomeInput() {
    int marker = 0;
    const auto window = static_cast<core::window::Handle>(&marker);
    core::queuePointerMotion(window, 12.0, 34.0, {}, {});
    core::consumePointerEvents(window, 1.5f);
    auto visual = core::visualPointerPosition(window, 1.5f);
    check(visual.inside && near(static_cast<float>(visual.x), 18.0f) &&
          near(static_cast<float>(visual.y), 51.0f),
          "client motion feeds visual pointer in scaled render coordinates");

    core::queuePointerPresence(window, false);
    core::queueVisualPointerMotion(window, 14.0, 9.0);
    const auto input = core::consumePointerEvents(window, 1.5f);
    visual = core::visualPointerPosition(window, 1.5f);
    check(input.size() == 1 && input.front().x < -999999.0 &&
          !core::hasPendingPointerInput(window) &&
          visual.inside && near(static_cast<float>(visual.x), 21.0f) &&
          near(static_cast<float>(visual.y), 13.5f),
          "non-client visual motion cannot create client pointer input");

    core::queueVisualPointerPresence(window, false);
    check(!core::visualPointerPosition(window).inside,
          "visual light clears after pointer leaves the owned window");
    core::detail::pointerStates().erase(window);
}

class RevealRecorder final : public core::render::RenderBackend {
public:
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int, int) override { return true; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int, int, const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode,
                         const std::vector<core::Rect>&) override {}
    bool supportsRetainedLayers() const override { return false; }
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& command, int, int) override {
        if (!command.shadowPass) last = command.reveal;
    }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}
    core::RevealEffect last{};
};

void testRuntimeNonClientBorderAndPressOrigin() {
    int marker = 0;
    const auto window = static_cast<core::window::Handle>(&marker);
    RevealRecorder backend;
    core::render::ScopedRenderBackend scoped{backend};
    core::dsl::Runtime runtime;
    if (!runtime.initialize()) {
        check(false, "runtime fixture initializes");
        return;
    }
    int clicks = 0;
    runtime.compose("visual-pointer-runtime", 160.0f, 100.0f,
                    [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        ui.rect("action").position(20.0f, 20.0f).size(100.0f, 40.0f)
            .color({0.2f, 0.2f, 0.2f, 1.0f}).reveal(config())
            .onClick([&] { ++clicks; }).build();
    });
    const auto step = [&](float dt, bool inputEnabled = true) {
        if (runtime.update(window, dt, 1.0f, 1.0f, inputEnabled))
            runtime.render(160, 100, 1.0f, {0, 0, 0, 1});
    };

    // The whole queue is consumed in one update; the visual feed already holds
    // B when the earlier Press A is dispatched.
    core::queuePointerButton(window, 30.0, 30.0, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    core::queuePointerMotion(window, 70.0, 30.0,
                             core::detail::pointerState(window).buttons, {});
    step(1.0f / 60.0f);
    check(backend.last.enabled && near(backend.last.pressCenter.x, 10.0f) &&
          near(backend.last.pressCenter.y, 10.0f),
          "batched Press A keeps A as press origin after Move B");

    core::queuePointerButton(window, 70.0, 30.0, core::PointerButton::Left,
                             core::PointerAction::Release, {});
    step(1.0f / 60.0f);
    const int clicksBeforeNonClient = clicks;
    core::queuePointerPresence(window, false);
    core::queueVisualPointerMotion(window, 15.0, 30.0);
    for (int index = 0; index < 4; ++index) step(1.0f / 60.0f);
    check(backend.last.enabled && backend.last.borderOpacity > 0.0f &&
          backend.last.fillOpacity == 0.0f &&
          near(backend.last.center.x, -5.0f) && clicks == clicksBeforeNonClient,
          "non-client pointer lights only nearby border without client hover or click");

    step(1.0f / 60.0f, false);
    check(!backend.last.enabled, "input disabled clears non-client visual light");
    core::queuePointerMotion(window, 30.0, 30.0, {}, {});
    step(1.0f / 60.0f);
    core::queuePointerButton(window, 30.0, 30.0, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    core::cancelPointerInput(window);
    step(1.0f / 60.0f);
    check(!backend.last.enabled, "pointer Cancel clears visual light");

    runtime.shutdown();
    core::detail::pointerStates().erase(window);
}

} // namespace

int main() {
    testSamplingAndLatestPointer();
    testIdleLeaveAndDisable();
    testPressAndLongIdle();
    testProximityAndUnclampedCoordinates();
    testNonClientVisualPointerDoesNotBecomeInput();
    testRuntimeNonClientBorderAndPressOrigin();
    if (failures == 0) std::cout << "reveal_state: PASS\n";
    return failures == 0 ? 0 : 1;
}
