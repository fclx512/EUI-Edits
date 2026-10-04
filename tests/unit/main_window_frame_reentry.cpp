#include "core/app/main_window_runtime.h"

#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

std::function<void()> onAppUpdate;
int appUpdates = 0;
int appRenders = 0;

} // namespace

// Headless app entry points: exercise real runFrame scheduling and the fake
// backend without initializing GLFW, creating a window, or submitting GL.
namespace app {
const char* windowTitle() { return "frame reentry test"; }
bool showDebugStatsInTitle() { return false; }
double debugTitleUpdateInterval() { return 1.0; }
double frameRateLimit() { return 0.0; }
bool isAnimating() { return false; }
float nextTimerWakeSeconds() { return std::numeric_limits<float>::infinity(); }
bool update(core::window::Handle, float, int, int, float, float, bool, bool) {
    ++appUpdates;
    // One shot also bounds the test if the production guard regresses.
    if (auto hook = std::exchange(onAppUpdate, {})) {
        hook();
    }
    return true;
}
void render(int, int, float) { ++appRenders; }
} // namespace app

namespace {

class FakeBackend final : public core::render::RenderBackend {
public:
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override { ++calls; }
    void beginFrame(const core::render::RenderSurface&) override { ++calls; }
    void present() override { ++calls; }
    bool frameReady() const override { return ready; }
    bool framePresented() const override { return presented; }
    bool ensureRenderCache(int, int) override { return false; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int, int, const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode,
                         const std::vector<core::Rect>&) override {}
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&, int, int) override {}
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}

    int calls = 0;
    bool ready = true, presented = true;
};

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

template <typename ChildFn>
void frame(app::MainWindowRuntime& runtime,
           app::AppRunner& runner,
           FakeBackend& backend,
           ChildFn&& children,
           const app::MainWindowMetrics& metrics = {}) {
    runner.nextFrameTime = 0.0;
    runtime.runFrame(nullptr,
                     backend,
                     metrics,
                     0.0,
                     60.0,
                     false,
                     [] {},
                     [&](float, bool) { children(); },
                     [](const char*) {},
                     [] { return false; });
}

} // namespace

int main() {
    app::AppRunner runner;
    app::MainWindowRuntime runtime(runner);
    FakeBackend backend;

    runner.paintRequested = false;
    int outerChildren = 0;
    int nestedChildren = 0;
    frame(runtime, runner, backend, [&] {
        ++outerChildren;
        core::platform::requestUiUpdate();
        core::platform::requestFrame();
        frame(runtime, runner, backend, [&] { ++nestedChildren; });
        check(core::platform::consumeUiUpdate(), "nested frame consumed the pending UI update");
        check(core::platform::consumeFrameRequest(), "nested frame consumed the pending frame request");
        // Model a successful outer render clearing the paint flag. The guard
        // must restore the nested request even after this happens.
        runner.markRendered();
        check(!runner.paintRequested, "markRendered did not clear the paint flag");
    });
    check(outerChildren == 1, "outer child update did not run exactly once");
    check(nestedChildren == 0, "nested frame ran child updates");
    check(backend.calls == 0, "invalid-metrics test touched the render backend");
    check(runner.paintRequested, "nested frame did not leave a deferred repaint request");
    check(runtime.idleWakeSeconds(100.0, std::numeric_limits<float>::infinity()) == 0.0,
          "deferred repaint did not wake the next idle-loop frame");

    // A subsequent ordinary frame must enter the outer-frame path again.
    int nextFrameChildren = 0;
    frame(runtime, runner, backend, [&] { ++nextFrameChildren; });
    check(nextFrameChildren == 1, "frame guard was not released after a nested frame");

    bool threw = false;
    try {
        frame(runtime, runner, backend, [&] {
            frame(runtime, runner, backend, [&] { ++nestedChildren; });
            runner.markRendered();
            throw std::runtime_error("test exception");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "child callback exception did not propagate");
    check(runner.paintRequested, "exception discarded the deferred repaint request");

    int afterExceptionChildren = 0;
    frame(runtime, runner, backend, [&] { ++afterExceptionChildren; });
    check(afterExceptionChildren == 1, "frame guard was not released after an exception");

    // A valid outer frame must bind/present exactly once. A nested request from
    // app::update must stop before update, context binding, or render and remain
    // pending after the real markRendered() path clears paintRequested.
    onAppUpdate = [&] {
        frame(runtime, runner, backend, [&] { ++nestedChildren; }, {320, 240, 1.0f, 1.0f});
    };
    const int callsBefore = backend.calls;
    frame(runtime, runner, backend, [] {}, {320, 240, 1.0f, 1.0f});
    check(appUpdates == 1, "app update reentered during an active frame");
    check(appRenders == 1, "nested frame rendered");
    check(backend.calls - callsBefore == 3, "nested frame bound/presented the backend");
    check(nestedChildren == 0, "nested update callback ran child updates");
    check(runner.paintRequested, "outer render discarded the nested repaint request");

    frame(runtime, runner, backend, [] {}, {320, 240, 1.0f, 1.0f});
    check(appUpdates == 2 && appRenders == 2, "deferred ordinary frame did not render");
    check(!runner.paintRequested, "ordinary completed frame left a spurious repaint request");
    check(std::isinf(runtime.idleWakeSeconds(100.0, std::numeric_limits<float>::infinity())),
          "ordinary completed frame did not return to idle");

    const int completedBeforeFailure = runner.renderedFrames;
    const int renderedBeforeFailure = appRenders;
    backend.ready = false;
    frame(runtime, runner, backend, [] {}, {320, 240, 1.0f, 1.0f});
    check(appRenders == renderedBeforeFailure, "failed frame setup still drew the app");
    check(runner.renderedFrames == completedBeforeFailure && runner.paintRequested,
          "failed frame setup acknowledged or discarded a paint request");
    check(runner.nextFrameTime > core::window::timeSeconds(), "failed frame setup has no bounded retry deadline");
    backend.ready = true; backend.presented = false;
    frame(runtime, runner, backend, [] {}, {320, 240, 1.0f, 1.0f});
    check(runner.renderedFrames == completedBeforeFailure && runner.paintRequested,
          "failed presentation acknowledged or discarded a paint request");
    check(core::platform::consumeUiUpdate(), "failed presentation did not request a fresh update");
    backend.presented = true;
    frame(runtime, runner, backend, [] {}, {320, 240, 1.0f, 1.0f});
    check(runner.renderedFrames == completedBeforeFailure + 1 && !runner.paintRequested,
          "successful recovery did not complete exactly one pending paint");

    return failures == 0 ? 0 : 1;
}
