// Production popup policy on a hidden Direct2D surface. Synthetic events and
// render-call wall times do not measure native input or screen presentation.
// Keep this source compatible with the frozen baseline: only apply() is used.
#include "ui/popup_presentation.h"
#include "core/dsl_runtime.h"
#include "core/window/window_backend.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace app { void requestUpdate() {} }

namespace {
using Clock = std::chrono::steady_clock;
constexpr int kWidth = 800, kHeight = 600, kHz = 240;
constexpr float kDelta = 1.0f / kHz;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
double micros(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
double processCpuMicros() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0,
            "process CPU accounting");
    const auto ticks = [](const FILETIME& value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    // All process threads, not only the calling thread. FILETIME's 100-ns unit
    // does not imply this accuracy: existing receipts showed ~15.625-ms steps.
    return static_cast<double>(ticks(kernel) + ticks(user)) * .1;
#else
    return -1;
#endif
}

struct CycleSnapshot {
    std::uint64_t process = 0, mainThread = 0;
    bool valid = false;
};
#if defined(_WIN32)
using CycleQuery = BOOL (WINAPI*)(HANDLE, ULONG64*);
CycleQuery processCycleQuery = nullptr, threadCycleQuery = nullptr;
#endif

void prepareCycleQueries(bool enabled) {
    if (!enabled) return;
#if defined(_WIN32)
    const HMODULE module = GetModuleHandleW(L"kernel32.dll");
    processCycleQuery = reinterpret_cast<CycleQuery>(GetProcAddress(module, "QueryProcessCycleTime"));
    threadCycleQuery = reinterpret_cast<CycleQuery>(GetProcAddress(module, "QueryThreadCycleTime"));
    require(processCycleQuery && threadCycleQuery, "CPU cycle APIs unavailable");
#else
    throw std::runtime_error("--cycles requires Windows");
#endif
}

CycleSnapshot readCycles(bool enabled, bool ending) {
    CycleSnapshot result;
    if (!enabled) return result;
#if defined(_WIN32)
    ULONG64 process = 0, thread = 0;
    // Nest the main-thread interval inside the process interval. These are
    // sequential samples, not an atomic per-thread attribution snapshot.
    if (ending) {
        require(threadCycleQuery(GetCurrentThread(), &thread) != 0, "main-thread cycle accounting");
        require(processCycleQuery(GetCurrentProcess(), &process) != 0, "process cycle accounting");
    } else {
        require(processCycleQuery(GetCurrentProcess(), &process) != 0, "process cycle accounting");
        require(threadCycleQuery(GetCurrentThread(), &thread) != 0, "main-thread cycle accounting");
    }
    result = {process, thread, true};
#endif
    return result;
}
double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    const auto left = static_cast<std::size_t>(position);
    const auto right = std::min(left + 1, values.size() - 1);
    return values[left] + (values[right] - values[left]) * (position - left);
}

struct Recorder final : core::render::RenderBackend {
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
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode, const std::vector<core::Rect>&) override {}
    bool supportsRetainedLayers() const override { return false; }
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override { ++materials; }
    void prepareFrozenBackdrop(const core::FrozenBackdrop&) override { ++materials; }
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& command, int, int) override {
        if (command.shadowPass) ++shadowRects;
        else if (core::closeEnough(command.rect.x, 300.0f) &&
                 core::closeEnough(command.rect.width, 440.0f) &&
                 core::closeEnough(command.rect.height, 1.0f)) ++backgroundRects;
        else ++menuRects;
    }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override { ++textDrawCommands; }
    int materials = 0;
    int backgroundRects = 0, menuRects = 0, shadowRects = 0, textDrawCommands = 0;
};

struct Segment {
    int paints = 0, composes = 0;
    double updateUs = 0, paintUs = 0, composeUs = 0;
    int backgroundRects = 0, menuRects = 0, shadowRects = 0, textDrawCommands = 0;
};

struct Counts {
    int updates = 0, paints = 0, composes = 0, full = 0, fast = 0;
    int draws = 0, revealDraws = 0, brushCreates = 0, materialCaptures = 0;
    std::uint64_t dirtyPixels = 0, blitPixels = 0;
    double updateUs = 0, paintUs = 0, composeUs = 0;
    std::vector<double> updateSamples, paintSamples, composeSamples;
    Segment entry, hover, submenu, exit;
    double cycleCpuUs = -1;
    double cycleWallUs = -1;
    CycleSnapshot cyclesBegin, cyclesEnd;
    bool recorderClassified = false;
    int backgroundRects = 0, menuRects = 0, shadowRects = 0, textDrawCommands = 0;
};

Segment snapshot(const Counts& counts) {
    return {counts.paints, counts.composes, counts.updateUs, counts.paintUs, counts.composeUs,
            counts.backgroundRects, counts.menuRects, counts.shadowRects, counts.textDrawCommands};
}

Segment difference(const Segment& current, const Segment& previous) {
    return {current.paints - previous.paints, current.composes - previous.composes,
            current.updateUs - previous.updateUs, current.paintUs - previous.paintUs,
            current.composeUs - previous.composeUs, current.backgroundRects - previous.backgroundRects,
            current.menuRects - previous.menuRects, current.shadowRects - previous.shadowRects,
            current.textDrawCommands - previous.textDrawCommands};
}

class Fixture {
public:
    Fixture(neo::UiStyle style, bool animations, bool multi, bool d2d, bool paced,
            bool cycles, bool legacyOffset)
        : multi_(multi), d2d_(d2d), paced_(paced), cycles_(cycles), legacyOffset_(legacyOffset) {
        const auto start = Clock::now();
        state_.uiStyle = style;
        state_.animations = animations;
        state_.theme = neo::ThemeMode::Light;
        neo::state().theme = neo::ThemeMode::Light;
        neo::setUiStyle(style);
        tokens_ = neo::editorColors(neo::ThemeMode::Light).tokens;
#if defined(_WIN32) && defined(EUI_RENDER_BACKEND_D2D)
        if (d2d_) {
            window_ = static_cast<core::window::Handle>(CreateWindowExW(0, L"STATIC",
                L"EUI menu style benchmark", WS_POPUP, 0, 0, kWidth, kHeight,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
            require(window_ && !IsWindowVisible(static_cast<HWND>(window_)), "hidden surface creation");
            raster_ = core::render::createRenderBackend(window_);
            require(raster_ && raster_->initialize(), "Direct2D initialization");
        }
#endif
        if (!window_) window_ = reinterpret_cast<core::window::Handle>(&windowTag_);
        backend_ = raster_ ? raster_.get() : &recorder_;
        scoped_ = std::make_unique<core::render::ScopedRenderBackend>(*backend_);
        require(runtime_.initialize(), "Runtime initialization");
        initializationUs_ = micros(start);
    }
    ~Fixture() {
        runtime_.shutdown();
        core::releaseInputQueue(window_);
        backend_->releaseRenderCache();
        scoped_.reset();
        raster_.reset();
#if defined(_WIN32) && defined(EUI_RENDER_BACKEND_D2D)
        if (d2d_ && window_) DestroyWindow(static_cast<HWND>(window_));
#endif
    }

    double initializationUs() const { return initializationUs_; }

    Counts prime() {
        Counts counts;
        compose(counts);
        core::queuePointerMotion(window_, 780, 580, {}, {});
        step(counts, 0);
        return counts;
    }

    Counts cycle() {
        Counts counts;
        Segment boundary;
        const double cpuStart = processCpuMicros();
        counts.cyclesBegin = readCycles(cycles_, false);
        const auto cycleStart = Clock::now();
        state_.openMenu = neo::MenuKind::File;
        compose(counts);
        for (int frame = 0; frame < 336; ++frame) {
            // Snapshots contain scalar counters only. They happen outside the
            // update/compose/paint clocks and do not operate on the Runtime.
            if (frame == 72 || frame == 144 || frame == 264) {
                const Segment current = snapshot(counts);
                if (frame == 72) counts.entry = difference(current, boundary);
                if (frame == 144) counts.hover = difference(current, boundary);
                if (frame == 264) counts.submenu = difference(current, boundary);
                boundary = current;
            }
            if (frame == 72) pointerAt("level.0.item.1");
            if (frame == 96) pointerAt("level.0.item.2");
            if (frame == 120) pointerAt("level.0.item.3");
            if (frame == 144) pointerAt("level.0.item.0");
            if (frame == 192) pointerAt(multi_ ? "level.1.item.0" : "level.0.item.2");
            if (frame == 216) pointerAt(multi_ ? "level.2.item.0" : "level.0.item.1");
            if (frame == 264) {
                state_.openMenu = neo::MenuKind::None;
                core::queuePointerMotion(window_, 780, 580, {}, {});
                compose(counts);
            }
            step(counts, kDelta);
            if (!state_.animations && frame == 60)
                require(!runtime_.isAnimating() && !std::isfinite(runtime_.nextTimerWakeSeconds()),
                        "disabled animations retain a wake");
            pace(cycleStart, frame + 1);
        }
        counts.exit = difference(snapshot(counts), boundary);
        // Quiet interval is asserted separately from the measured trajectory.
        Counts quiet;
        for (int frame = 0; frame < 120; ++frame) {
            step(quiet, kDelta);
            pace(cycleStart, 336 + frame + 1);
        }
        require(quiet.paints == 0 && quiet.composes == 0 && quiet.fast == 0,
                "closed menu did not become quiet");
        require(!runtime_.isAnimating() && !std::isfinite(runtime_.nextTimerWakeSeconds()),
                "closed menu retains an animation wake");
        require(counts.fast == 0, "menu requested display-rate animation");
        require(counts.composes < 24, "menu requested repeated per-frame compose");
        require(counts.materialCaptures == 0 && recorder_.materials == 0,
                "opaque menu requested material capture");
        // Two accounting queries per cycle, outside the per-call wall clocks.
        // Include the existing quiet loop and optional paced waiting. Neither
        // is an explicit worker drain or proof of renderer completion.
        counts.cycleWallUs = micros(cycleStart);
        counts.cyclesEnd = readCycles(cycles_, true);
        if (cycles_)
            require(counts.cyclesEnd.process >= counts.cyclesBegin.process &&
                    counts.cyclesEnd.mainThread >= counts.cyclesBegin.mainThread,
                    "CPU cycle counter moved backwards");
        const double cpuEnd = processCpuMicros();
        if (cpuStart >= 0 && cpuEnd >= cpuStart) counts.cycleCpuUs = cpuEnd - cpuStart;
        return counts;
    }

private:
    void pace(Clock::time_point start, int completedSteps) const {
        if (!paced_) return;
        // Keep the same fixed dt and events. No timer-resolution request and no
        // Runtime work is added; OS scheduling may overshoot these deadlines.
        std::this_thread::sleep_until(start + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(static_cast<double>(completedSteps) / kHz)));
    }

    void compose(Counts& counts) {
        const auto start = Clock::now();
        runtime_.compose("production-menu-style", kWidth, kHeight,
            [&](core::dsl::Ui& view, const core::dsl::Screen&) {
                ui_ = &view;
                // Fixed inexpensive background gives dirty redraw a real scene.
                for (int row = 0; row < 24; ++row)
                    view.rect("background." + std::to_string(row))
                        .position(300, static_cast<float>(row * 22)).size(440, 1)
                        .color({.7f,.7f,.7f,1}).build();
                using Item = components::ContextMenuItem;
                std::vector<Item> items = multi_ ? std::vector<Item>{
                    Item("Parent", {Item("Child A", {Item("Grandchild A"), Item("Grandchild B")}),
                                    Item("Child B"), Item("Child C")}),
                    Item("Action one"), Item("Action two"), Item("Action three")}
                    : std::vector<Item>{Item("Action zero"), Item("Action one"),
                                        Item("Action two"), Item("Action three")};
                items[2].withSeparatorBefore();
                items[3].withChecked();
                const bool open = state_.openMenu != neo::MenuKind::None;
                auto menu = components::contextMenu(view, "benchmark.menu")
                    .open(open).screen(kWidth, kHeight).position(30, 50).size(210, 32)
                    .items(std::move(items)).theme(tokens_);
                neo::popup_presentation::apply(menu, state_, tokens_, open);
                if (legacyOffset_) {
                    // Diagnostic ablation only: use the shared old interface so
                    // this exact probe also builds against the frozen baseline.
                    // Win11's policy has the same enter/exit/highlight timings;
                    // only its hidden translation changes from +4 to -4 DIP.
                    components::ContextMenuMotion legacy;
                    legacy.offsetY = -4.0f;
                    if (neo::animationsEnabled(state_)) {
                        legacy.enter = core::Transition::make(.12f, core::Ease::OutCubic);
                        if (open || neo::popup_presentation::activeOwner(state_) ==
                                    neo::popup_presentation::Owner::None)
                            legacy.exit = core::Transition::make(.07f, core::Ease::OutQuad);
                        legacy.highlight = core::Transition::make(.075f, core::Ease::OutCubic);
                    }
                    menu.motion(legacy);
                }
                menu.skipClosedContent().onOpenChange([&](bool value) {
                    state_.openMenu = value ? neo::MenuKind::File : neo::MenuKind::None;
                }).build();
                neo::applyInteractionDefaults(view, neo::kUiFontFamily,
                                             neo::animationsEnabled(state_));
            });
        const double elapsed = micros(start);
        counts.composeUs += elapsed;
        counts.composeSamples.push_back(elapsed);
        ++counts.composes;
        require(!ui_->frozenBackdropSession() && !ui_->cachedStripSession(),
                "opaque menu created material session");
    }

    void pointerAt(const std::string& suffix) {
        const auto* item = ui_->find("benchmark.menu." + suffix);
        require(item && !item->disabled, "trajectory menu row missing");
        // A right-side point avoids a child popup overlapping its parent row.
        core::queuePointerMotion(window_, item->frame.x + item->frame.width - 12,
            item->frame.y + item->frame.height * .5f, {}, {});
    }

    bool update(Counts& counts, float delta) {
        const auto start = Clock::now();
        const bool paint = runtime_.update(window_, delta, 1, 1);
        const double elapsed = micros(start);
        counts.updateUs += elapsed;
        counts.updateSamples.push_back(elapsed);
        ++counts.updates;
        counts.fast += runtime_.isAnimating() ? 1 : 0;
        return paint;
    }

    void step(Counts& counts, float delta) {
        bool paint = update(counts, delta);
        if (runtime_.composeRequested()) {
            compose(counts);
            paint = update(counts, 0) || paint;
        }
        if (!paint) return;
        recorder_.backgroundRects = recorder_.menuRects = recorder_.shadowRects =
            recorder_.textDrawCommands = 0;
        const auto start = Clock::now();
        backend_->beginFrame({window_, raster_ ? core::window::nativeWindowInfo(window_)
                                             : core::window::NativeWindowInfo{},
                              kWidth, kHeight, 1});
        if (raster_) require(backend_->frameReady(), "Direct2D frame not ready");
        runtime_.render(kWidth, kHeight, 1, {.95f,.95f,.95f,1});
        backend_->present();
        if (raster_) require(backend_->framePresented(), "Direct2D frame not presented");
        const double elapsed = micros(start);
        counts.paintUs += elapsed;
        counts.paintSamples.push_back(elapsed);
        ++counts.paints;
        const auto stats = core::render::lastRenderFrameStats();
        counts.full += stats.fullPaint ? 1 : 0;
        counts.dirtyPixels += stats.dirtyPixels;
        counts.blitPixels += stats.blitPixels;
        counts.draws += stats.rectDraws + stats.textDraws + stats.polygonDraws + stats.imageDraws;
        counts.revealDraws += stats.revealDraws;
        counts.brushCreates += stats.revealBrushCreates;
        counts.materialCaptures += stats.backdropCaptures + stats.frozenSceneCaptures + stats.stripCaptures;
        if (!raster_) {
            counts.recorderClassified = true;
            counts.backgroundRects += recorder_.backgroundRects;
            counts.menuRects += recorder_.menuRects;
            counts.shadowRects += recorder_.shadowRects;
            counts.textDrawCommands += recorder_.textDrawCommands;
        }
    }

    int windowTag_ = 0;
    bool multi_ = false, d2d_ = false, paced_ = false;
    bool cycles_ = false, legacyOffset_ = false;
    double initializationUs_ = 0;
    core::window::Handle window_ = nullptr;
    Recorder recorder_;
    std::unique_ptr<core::render::RenderBackend> raster_;
    core::render::RenderBackend* backend_ = nullptr;
    std::unique_ptr<core::render::ScopedRenderBackend> scoped_;
    core::dsl::Runtime runtime_;
    core::dsl::Ui* ui_ = nullptr;
    neo::AppState state_;
    components::theme::ThemeColorTokens tokens_ = components::theme::light();
};

void writeDrawClassification(bool available, int background, int menu, int shadow, int text) {
    const auto write = [available](const char* name, int value) {
        std::cout << ",\"" << name << "\":";
        if (available) std::cout << value;
        else std::cout << "null";
    };
    write("background_rect_draws", background);
    write("menu_rect_draws", menu);
    write("shadow_rect_draws", shadow);
    write("text_draws", text);
}

void report(const Counts& c, const char* phase, int round, neo::UiStyle style,
            bool multi, bool animations, bool d2d, bool paced, bool cycles,
            bool legacyOffset, double initUs = 0) {
    std::cout << std::fixed << std::setprecision(3)
        << "{\"schema\":1,\"scope\":\"hidden-surface-render-calls\",\"backend\":\""
        << (d2d ? "d2d-software-dc" : "recorder") << "\",\"phase\":\"" << phase
        << "\",\"round\":" << round << ",\"style\":\""
        << (style == neo::UiStyle::Square ? "win10" : "win11")
        << "\",\"scene\":\"" << (multi ? "three-level" : "short")
        << "\",\"animations\":" << (animations ? "true" : "false")
        << ",\"paced\":" << (paced ? "true" : "false")
        << ",\"cycles_requested\":" << (cycles ? "true" : "false")
        << ",\"legacy_offset\":" << (legacyOffset ? "true" : "false")
        << ",\"input_hz\":" << kHz << ",\"surface_width\":" << kWidth
        << ",\"surface_height\":" << kHeight << ",\"dpi_scale\":1"
        << ",\"initialization_us\":" << initUs
        << ",\"updates\":" << c.updates << ",\"paints\":" << c.paints
        << ",\"composes\":" << c.composes << ",\"full_paints\":" << c.full
        << ",\"draws\":" << c.draws << ",\"dirty_pixels\":" << c.dirtyPixels
        << ",\"blit_pixels\":" << c.blitPixels << ",\"reveal_draws\":" << c.revealDraws
        << ",\"reveal_brush_creates\":" << c.brushCreates
        << ",\"material_captures\":" << c.materialCaptures
        << ",\"fast_animation_updates\":" << c.fast
        << ",\"quiet_paints\":0,\"quiet_composes\":0,\"quiet_wake\":false"
        << ",\"update_us\":" << c.updateUs << ",\"paint_us\":" << c.paintUs
        << ",\"compose_us\":" << c.composeUs
        << ",\"update_us_p50\":" << percentile(c.updateSamples,.50)
        << ",\"update_us_p95\":" << percentile(c.updateSamples,.95)
        << ",\"paint_us_p50\":" << percentile(c.paintSamples,.50)
        << ",\"paint_us_p95\":" << percentile(c.paintSamples,.95)
        << ",\"compose_us_p50\":" << percentile(c.composeSamples,.50)
        << ",\"compose_us_p95\":" << percentile(c.composeSamples,.95)
        << ",\"cycle_cpu_us\":";
    if (c.cycleCpuUs >= 0) std::cout << c.cycleCpuUs;
    else std::cout << "null";
    std::cout << ",\"cycle_wall_us\":";
    if (c.cycleWallUs >= 0) std::cout << c.cycleWallUs;
    else std::cout << "null";
    std::cout << ",\"cycle_cpu_includes_quiet\":true"
        << ",\"cycle_wall_includes_quiet\":true,\"cycle_wall_includes_wait\":true"
        << ",\"cycle_cpu_precision_note\":\"coarse accounting; approx 15.625 ms increments observed\""
        << ",\"cycle_process_cycles\":";
    const bool haveCycles = c.cyclesBegin.valid && c.cyclesEnd.valid;
    if (haveCycles) std::cout << c.cyclesEnd.process - c.cyclesBegin.process;
    else std::cout << "null";
    std::cout << ",\"cycle_thread_cycles\":";
    if (haveCycles) std::cout << c.cyclesEnd.mainThread - c.cyclesBegin.mainThread;
    else std::cout << "null";
    std::cout << ",\"cycle_counter_snapshots\":";
    if (haveCycles)
        std::cout << "{\"process_begin\":" << c.cyclesBegin.process
            << ",\"process_end\":" << c.cyclesEnd.process
            << ",\"main_thread_begin\":" << c.cyclesBegin.mainThread
            << ",\"main_thread_end\":" << c.cyclesEnd.mainThread << '}';
    else std::cout << "null";
    std::cout << ",\"cycle_counters_note\":\"raw CPU cycles; includes quiet; not elapsed time or atomic thread attribution\"";
    std::cout << ",\"draw_classification_available\":" << (c.recorderClassified ? "true" : "false");
    writeDrawClassification(c.recorderClassified, c.backgroundRects, c.menuRects,
                            c.shadowRects, c.textDrawCommands);
    std::cout << ",\"segments\":{";
    const auto writeSegment = [&](const char* name, const Segment& segment, bool comma) {
        if (comma) std::cout << ',';
        std::cout << '"' << name << "\":{\"update_us\":" << segment.updateUs
            << ",\"compose_us\":" << segment.composeUs << ",\"paint_us\":" << segment.paintUs
            << ",\"paints\":" << segment.paints << ",\"composes\":" << segment.composes;
        writeDrawClassification(c.recorderClassified, segment.backgroundRects, segment.menuRects,
                                segment.shadowRects, segment.textDrawCommands);
        std::cout << '}';
    };
    writeSegment("entry", c.entry, false);
    writeSegment("hover", c.hover, true);
    writeSegment("submenu", c.submenu, true);
    writeSegment("exit", c.exit, true);
    std::cout << "}}\n";
}
}

int main(int argc, char** argv) {
    try {
        int rounds = 1, warmups = 1;
        std::string selectedStyle = "both", selectedScene = "both", selectedAnimations = "both";
        bool paced = false, cycles = false, legacyOffset = false;
#if defined(_WIN32) && defined(EUI_RENDER_BACKEND_D2D)
        bool d2d = true;
#else
        bool d2d = false;
#endif
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--recorder") d2d = false;
            else if (option == "--paced") paced = true;
            else if (option == "--cycles") cycles = true;
            else if (option == "--legacy-offset") legacyOffset = true;
            else {
                require(i + 1 < argc, "missing CLI value");
                const std::string value = argv[++i];
                if (option == "--rounds") rounds = std::stoi(value);
                else if (option == "--warmups") warmups = std::stoi(value);
                else if (option == "--style") selectedStyle = value;
                else if (option == "--scene") selectedScene = value;
                else if (option == "--animations") selectedAnimations = value;
                else throw std::runtime_error("unknown CLI option");
            }
        }
        require(rounds >= 1 && rounds <= 100 && warmups >= 0 && warmups <= 100, "CLI round range");
        require(selectedStyle == "both" || selectedStyle == "win10" || selectedStyle == "win11", "CLI style");
        require(selectedScene == "both" || selectedScene == "short" || selectedScene == "three-level", "CLI scene");
        require(selectedAnimations == "both" || selectedAnimations == "on" || selectedAnimations == "off", "CLI animations");
        require(!legacyOffset || selectedStyle == "win11", "--legacy-offset requires --style win11");
        prepareCycleQueries(cycles);
#if defined(_WIN32)
        // Freeze product experiment overrides so baseline/candidate use one policy.
        SetEnvironmentVariableA("NEO_FEEDBACK", nullptr);
        SetEnvironmentVariableA("NEO_REVEAL", nullptr);
        SetEnvironmentVariableA("NEO_SETTINGS_REVEAL", nullptr);
        if (d2d) {
            SetEnvironmentVariableA("NEO_D2D_SOFTWARE", "1");
            SetEnvironmentVariableA("NEO_WIN32_DC", "1");
        }
#endif
        for (const auto style : {neo::UiStyle::Square, neo::UiStyle::Rounded}) {
            if (selectedStyle == "win10" && style != neo::UiStyle::Square) continue;
            if (selectedStyle == "win11" && style != neo::UiStyle::Rounded) continue;
            for (bool multi : {false,true}) for (bool animations : {false,true}) {
                if (selectedScene == "short" && multi) continue;
                if (selectedScene == "three-level" && !multi) continue;
                if (selectedAnimations == "on" && !animations) continue;
                if (selectedAnimations == "off" && animations) continue;
                Fixture fixture(style, animations, multi, d2d, paced, cycles, legacyOffset);
                report(fixture.prime(), "initialization", 0, style, multi, animations, d2d, paced, cycles, legacyOffset,
                       fixture.initializationUs());
                report(fixture.cycle(), "cold", 0, style, multi, animations, d2d, paced, cycles, legacyOffset);
                for (int round = 0; round < warmups; ++round)
                    report(fixture.cycle(), "warmup", round, style, multi, animations, d2d, paced, cycles, legacyOffset);
                for (int round = 0; round < rounds; ++round)
                    report(fixture.cycle(), "measured", round, style, multi, animations, d2d, paced, cycles, legacyOffset);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "menu_style_benchmark FAIL: " << error.what() << '\n';
        return 1;
    }
}
