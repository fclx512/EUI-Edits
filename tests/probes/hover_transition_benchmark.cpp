// Synthetic pointer trajectory. Recorder by default; optional hidden D2D surface.
// Neither mode measures native input latency or screen presentation.
#include "core/dsl_runtime.h"
#include "core/window/window_backend.h"
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
core::render::RenderBackend* rasterBackend = nullptr;
core::window::Handle rasterWindow = nullptr;


class Recorder final : public core::render::RenderBackend {
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
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&, int, int) override { ++draws; }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}
    int draws = 0;
};

struct Counts {
    int paints = 0;
    int composeRequests = 0;
    int fullPaints = 0;
    std::uint64_t dirtyPixels = 0;
    int draws = 0;
};

struct Fixture {
    Recorder backend;
    core::render::ScopedRenderBackend scoped{rasterBackend ? *rasterBackend : backend};
    core::dsl::Runtime runtime;
    core::window::Handle window = rasterWindow;

    Fixture(bool smooth, int buttons, float limit = 60.0f, bool reveal = false) {
        runtime.setDecorationFrameRateLimit(limit);
        runtime.initialize();
        runtime.compose("hover-probe", 640, 480, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            for (int i = 0; i < 100; ++i) {
                ui.rect("static" + std::to_string(i))
                    .position(float((i % 10) * 30), float((i / 10) * 24))
                    .size(20, 16).color({.1f, .1f, .1f, 1}).build();
            }
            for (int i = 0; i < buttons; ++i) {
                core::RevealEffect light{}; light.enabled = reveal; light.radius = 72;
                light.tint = {.2f,.5f,.9f,1}; light.borderOpacity = .85f; light.fillOpacity = .1f;
                ui.rect("button" + std::to_string(i))
                    .position(12.0f + float(i % 10) * 60.0f, 24.0f + float(i / 10) * 36.0f)
                    .size(52, 28)
                    .states({.15f, .15f, .15f, .10f}, {.3f, .5f, .9f, .38f},
                            {.35f, .55f, .95f, .55f})
                    .reveal(light)
                    .smoothStates(smooth)
                    .transition(smooth ? core::Transition::make(.14f, core::Ease::OutCubic)
                                       : core::Transition::none())
                    .build();
            }
        });
        step(620, 460, 1.0f / 60.0f);
    }

    ~Fixture() { runtime.shutdown(); core::releaseInputQueue(window); if(rasterBackend) rasterBackend->releaseRenderCache(); }

    Counts step(float x, float y, float dt) {
        Counts result;
        backend.draws = 0;
        core::queuePointerMotion(window, x, y, {}, {});
        const bool paint = runtime.update(window, dt, 1, 1);
        result.composeRequests = runtime.composeRequested() ? 1 : 0;
        if (paint) {
            result.paints = 1;
            if (rasterBackend) {
                rasterBackend->beginFrame({window, core::window::nativeWindowInfo(window),640,480,1});
                if (!rasterBackend->frameReady()) ++failures;
            }
            runtime.render(640, 480, 1, {.08f, .08f, .08f, 1});
            if (rasterBackend) {
                rasterBackend->present();
                if (!rasterBackend->framePresented()) ++failures;
            }
            const auto stats = core::render::lastRenderFrameStats();
            result.fullPaints = stats.fullPaint ? 1 : 0;
            result.dirtyPixels = stats.dirtyPixels;
            result.draws = backend.draws;
        }
        return result;
    }
};

void add(Counts& total, const Counts& value) {
    total.paints += value.paints;
    total.composeRequests += value.composeRequests;
    total.fullPaints += value.fullPaints;
    total.dirtyPixels += value.dirtyPixels;
    total.draws += value.draws;
}

void trial(float limit, int buttons, int hz, const char* trajectory, int round, bool reveal) {
    const bool smooth = true;
    Fixture fixture(smooth, buttons, limit, reveal);
    for (int i = 0; i < 30; ++i) fixture.step(620, 460, 1.0f / 60.0f);
    Counts motion;
    int fastAnimationSamples = 0;
    int lastMotionPaint = -1;
    std::vector<double> stepTimes;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < hz; ++i) {
        float x = 24, y = 38;
        if (std::string(trajectory) == "alternating") {
            // The same 120 target changes in one simulated second at every update rate.
            const int index = ((i * 120) / hz) % buttons;
            x = 24.0f + float(index % 10) * 60.0f;
            y = 38.0f + float(index / 10) * 36.0f;
        }
        const auto stepStart = std::chrono::steady_clock::now();
        const auto stepCounts = fixture.step(x, y, 1.0f / float(hz));
        add(motion, stepCounts);
        if (stepCounts.paints) lastMotionPaint = i;
        stepTimes.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-stepStart).count());
        fastAnimationSamples += fixture.runtime.isAnimating() ? 1 : 0;
    }
    const double wallUs = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    Counts settle;
    // Fixed pointer after motion: check whether visual animation converges.
    for (int i = 0; i < hz; ++i) add(settle, fixture.step(24, 38, 1.0f / float(hz)));
    Counts quiet;
    for (int i = 0; i < hz; ++i) add(quiet, fixture.step(24, 38, 1.0f / float(hz)));
    if (motion.composeRequests != 0 || motion.fullPaints != 0 ||
        settle.composeRequests != 0 || settle.fullPaints != 0 ||
        quiet.composeRequests != 0 || quiet.fullPaints != 0 || quiet.paints != 0) {
        ++failures;
        std::cerr << "FAIL: hover probe expected retained paint and quiet convergence"
                  << " (round=" << round << ", hz=" << hz << ", buttons=" << buttons
                  << ", smooth=" << smooth << ")\n";
    }
    if (limit > 0 && (motion.paints > 61 || fastAnimationSamples != 0)) {
        ++failures; std::cerr << "FAIL: decoration exceeded shared cadence or requested fast animation\n";
    }
    if (fixture.runtime.isAnimating() || std::isfinite(fixture.runtime.nextTimerWakeSeconds())) {
        ++failures; std::cerr << "FAIL: settled decorations still schedule wakeups\n";
    }
    std::sort(stepTimes.begin(), stepTimes.end());
    std::cout << "{\"backend\":\"" << (rasterBackend ? "d2d-software" : "recorder") << "\",\"limit_hz\":" << limit << ",\"reveal\":" << (reveal ? "true" : "false")
              << ",\"fast_animation_samples\":" << fastAnimationSamples
              << ",\"step_wall_us_p50\":" << stepTimes[stepTimes.size()/2]
              << ",\"step_wall_us_p95\":" << stepTimes[stepTimes.size()*95/100]
              << ",\"round\":" << round << ",\"smooth\":" << (smooth ? "true" : "false")
              << ",\"buttons\":" << buttons << ",\"hz\":" << hz
              << ",\"trajectory\":\"" << trajectory << "\",\"motion_wall_us\":" << wallUs
              << ",\"motion_paints\":" << motion.paints
              << ",\"last_motion_paint_seconds\":" << double(lastMotionPaint+1)/hz
              << ",\"motion_compose\":" << motion.composeRequests
              << ",\"motion_full\":" << motion.fullPaints
              << ",\"motion_dirty_pixels\":" << motion.dirtyPixels
              << ",\"motion_draws\":" << motion.draws
              << ",\"settle_paints\":" << settle.paints
              << ",\"quiet_paints\":" << quiet.paints << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    // A/B uses identical event rates, geometry, colors and target trajectories.
    // Optional arm mode supports separate-process ABBA without changing code.
    const bool d2d = argc == 3 && std::string(argv[1]) == "--d2d-arm";
    const bool arm = argc == 3 && (std::string(argv[1]) == "--arm" || d2d);
#if defined(EUI_RENDER_BACKEND_D2D) && defined(_WIN32)
    std::unique_ptr<core::render::RenderBackend> rasterOwner;
    if (d2d) {
        SetEnvironmentVariableA("NEO_D2D_SOFTWARE", "1");
        SetEnvironmentVariableA("NEO_WIN32_DC", "1");
        rasterWindow = static_cast<core::window::Handle>(CreateWindowExW(0,L"STATIC",L"Decoration benchmark",WS_POPUP,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr));
        if (!rasterWindow) return 2;
        rasterOwner = core::render::createRenderBackend(rasterWindow);
        if (!rasterOwner || !rasterOwner->initialize()) return 2;
        rasterBackend = rasterOwner.get();
    }
#else
    if (d2d) return 2;
#endif
    const float selected = arm ? std::stof(argv[2]) : 60.0f;
    for (int round = 0; round < (arm ? 1 : 3); ++round) {
        for (int hz : {240, 1000}) {
            for (int buttons : {1, 120}) {
                for (bool reveal : {false, true}) {
                    for (float limit : (arm ? std::vector<float>{selected} : std::vector<float>{0,60,60,0}))
                        trial(limit, buttons, hz, buttons == 1 ? "single" : "alternating", round, reveal);
                }
            }
        }
    }
#if defined(EUI_RENDER_BACKEND_D2D) && defined(_WIN32)
    rasterOwner.reset();
    if (rasterWindow) DestroyWindow(static_cast<HWND>(rasterWindow));
#endif
    return failures ? 1 : 0;
}
