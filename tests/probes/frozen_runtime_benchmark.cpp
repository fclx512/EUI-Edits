// Hidden production Runtime + D2D fixture. A and B compose the same 520
// visible rectangles and Chinese labels. B adds one frozen file-menu pair.
// Hover frames deliberately request a full paint in both arms; they measure
// Runtime signature + full scene render, not the product's dirty-hover cost.
#if defined(EUI_RENDER_BACKEND_D2D) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/dsl_runtime.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kWidth = 1024;
constexpr int kHeight = 720;
constexpr int kMeasuredRounds = 12;
constexpr int kHoverFrames = 20;
constexpr core::Color kClear{0.09f, 0.11f, 0.14f, 1.0f};
constexpr std::array<char, 4> kOrder{{'A', 'B', 'B', 'A'}};
bool stripMode = false;

int captures(const core::render::RenderFrameStats& s) { return stripMode ? s.stripCaptures : s.frozenSceneCaptures; }
int filters(const core::render::RenderFrameStats& s) { return stripMode ? s.stripFilters : s.frozenPanelFilters; }
int reuses(const core::render::RenderFrameStats& s) { return stripMode ? s.stripReuses : s.frozenPanelReuses; }
int invalidations(const core::render::RenderFrameStats& s) { return stripMode ? s.stripInvalidations : s.frozenInvalidations; }
int fallbacks(const core::render::RenderFrameStats& s) { return stripMode ? s.stripFallbacks : s.frozenFallbacks; }
bool deferred(const core::render::RenderFrameStats& s) { return stripMode ? s.stripCaptureDeferred : s.frozenCaptureDeferred; }

struct Stamp {
    Clock::time_point wall;
    std::uint64_t cpu100ns = 0;
};

Stamp stamp() {
    Stamp result{Clock::now(), 0};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        result.cpu100ns = k.QuadPart + u.QuadPart;
    }
    return result;
}

struct FrameResult {
    bool okay = false;
    bool paintPending = false;
    int frames = 1;
    double wallMs = 0.0, threadCpuMs = 0.0;
    core::render::RenderFrameStats stats{};
};

void composeFixture(core::dsl::Runtime& runtime, std::uint64_t session, int focusRevision = 0) {
    runtime.compose("frozen-runtime-benchmark", static_cast<float>(kWidth),
                    static_cast<float>(kHeight), [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        ui.rect("scene.background").position(0, 0).size(kWidth, kHeight)
            .color(kClear).build();
        // 20 rows x 26 columns = 520 rectangles. The left six columns resemble
        // a document library; the remainder resemble editor lines and blocks.
        for (int row = 0; row < 20; ++row) {
            for (int col = 0; col < 26; ++col) {
                const int index = row * 26 + col;
                const float x = 12.0f + static_cast<float>(col * 38);
                const float y = 54.0f + static_cast<float>(row * 31);
                const core::Color shade = col < 6
                    ? core::Color{0.18f, 0.23f, 0.29f, 1.0f}
                    : core::Color{0.14f + static_cast<float>(index % 3) * 0.015f,
                                  0.18f, 0.23f, 1.0f};
                ui.rect("scene.cell." + std::to_string(index))
                    .position(x, y).size(34.0f, 21.0f).color(shade).build();
            }
        }
        for (int row = 0; row < 20; ++row) {
            ui.text("scene.library.label." + std::to_string(row))
                .position(14.0f, 56.0f + static_cast<float>(row * 31))
                .size(216.0f, 19.0f).text(u8"文档库 项目")
                .fontFamily("C:/Windows/Fonts/msyh.ttc").fontSize(11.0f)
                .color({0.86f, 0.88f, 0.90f, 1.0f}).build();
        }
        for (int row = 0; row < 60; ++row) {
            const float x = 254.0f + static_cast<float>((row % 3) * 248);
            const float y = 57.0f + static_cast<float>((row / 3) * 31);
            ui.text("scene.editor.label." + std::to_string(row))
                .position(x, y).size(232.0f, 19.0f).text(u8"编辑器 正文示例")
                .fontFamily("C:/Windows/Fonts/msyh.ttc").fontSize(11.0f)
                .color({0.88f, 0.90f, 0.92f, 1.0f}).build();
        }
        ui.text("scene.focus.label").position(14.0f, 680.0f).size(230.0f, 20.0f)
            .text(std::string(u8"路径输入状态 ") + std::to_string(focusRevision))
            .fontFamily("C:/Windows/Fonts/msyh.ttc").fontSize(11.0f)
            .color({0.86f, 0.88f, 0.90f, 1.0f}).build();
        ui.stack("menu.root").position(0, 0).size(kWidth, kHeight)
            .zIndex(900).excludeFromBackdropSnapshot().content([&] {
                const auto panel = [&](const char* id, const char* label,
                                       std::uint64_t panelId, float x, float y,
                                       float width, float height) {
                    auto background = ui.rect(id).position(x, y).size(width, height)
                        .color({0.92f, 0.94f, 0.96f, 1.0f});
                    if (session && !stripMode) background.frozenBackdrop(
                        core::FrozenBackdrop{session, panelId, {}, 8.0f, 0.78f});
                    background.build();
                    ui.text(std::string(id) + ".label").position(x + 12.0f, y + 12.0f)
                        .size(width - 24.0f, 22.0f).text(label)
                        .fontFamily("C:/Windows/Fonts/msyh.ttc").fontSize(13.0f)
                        .color({0.13f, 0.16f, 0.20f, 1.0f}).build();
                };
                panel("menu.file", u8"文件 菜单", 1, 180.0f, 60.0f, 280.0f, 430.0f);
                panel("menu.sub", u8"编码 子菜单", 2, 460.0f, 128.0f, 254.0f, 318.0f);
            }).build();
        ui.stack("topbar.root").position(0, 0).size(kWidth, 40)
            .zIndex(2000).excludeFromBackdropSnapshot().content([&] {
                auto surface = ui.rect("topbar.surface").position(0, 0).size(kWidth, 40)
                    .color({0.20f, 0.24f, 0.30f, 1.0f});
                if (stripMode && session) {
                    core::FrozenBackdrop strip{session, 1, {}, 6, 0.58f};
                    strip.cachedStrip = true; strip.sourceOffset = {0, 54};
                    surface.frozenBackdrop(strip);
                }
                surface.build();
                ui.text("topbar.title").position(16, 8).size(180, 24)
                    .text(u8"文件 编辑 视图")
                    .fontFamily("C:/Windows/Fonts/msyh.ttc").fontSize(14.0f)
                    .color({0.94f, 0.95f, 0.96f, 1.0f}).build();
            }).build();
    });
}

FrameResult runFrame(core::dsl::Runtime& runtime, core::render::RenderBackend& backend,
                     HWND hwnd, bool compose, std::uint64_t session, int focusRevision = 0) {
    FrameResult result;
    const Stamp before = stamp();
    if (compose) composeFixture(runtime, session, focusRevision);
    const auto handle = static_cast<core::window::Handle>(hwnd);
    runtime.update(handle, 1.0f / 60.0f, 1.0f, 1.0f);
    // Keep the identical full-paint hover trajectory in A and B. This makes
    // the Runtime signature measurable while excluding unrelated compose work.
    runtime.requestFullPaint();
    backend.beginFrame({handle, core::window::nativeWindowInfo(handle), kWidth, kHeight, 1.0f});
    if (!backend.frameReady()) return result;
    runtime.render(kWidth, kHeight, 1.0f, kClear);
    result.paintPending = runtime.paintRequested();
    backend.present();
    const Stamp after = stamp();
    result.okay = backend.framePresented();
    result.wallMs = std::chrono::duration<double, std::milli>(after.wall - before.wall).count();
    result.threadCpuMs = static_cast<double>(after.cpu100ns - before.cpu100ns) / 10000.0;
    result.stats = core::render::currentRenderFrameStats();
    return result;
}

void report(const char* phase, int round, char arm, const FrameResult& result) {
    const auto& s = result.stats;
    const bool warmup = round == 0;
    std::printf("{\"phase\":\"%s\",\"round\":%d,\"arm\":\"%c\",\"warmup\":%s,"
                "\"wall_ms\":%.4f,\"thread_cpu_ms\":%.4f,\"frames\":%d,"
                "\"capture_deferred\":%s,\"paint_pending\":%s,\"scene_captures\":%d,"
                "\"panel_filters\":%d,\"panel_reuses\":%d,\"invalidations\":%d,"
                "\"fallbacks\":%d,\"frozen_bytes\":%llu,\"full_paint\":%s,"
                "\"render_passes\":%d,\"rect_draws\":%d,\"text_draws\":%d,"
                "\"strip_captures\":%d,\"strip_filters\":%d,\"strip_reuses\":%d,\"strip_bytes\":%llu,"
                "\"strip_deferred\":%s,\"strip_invalidations\":%d,\"strip_fallbacks\":%d}\n",
                phase, round, arm, warmup ? "true" : "false",
                result.wallMs, result.threadCpuMs, result.frames,
                deferred(s) ? "true" : "false", result.paintPending ? "true" : "false",
                s.frozenSceneCaptures,
                s.frozenPanelFilters, s.frozenPanelReuses, s.frozenInvalidations,
                s.frozenFallbacks, static_cast<unsigned long long>(s.frozenBytes),
                s.fullPaint ? "true" : "false", s.renderDirectPasses, s.rectDraws, s.textDraws,
                s.stripCaptures, s.stripFilters, s.stripReuses, static_cast<unsigned long long>(s.stripBytes),
                s.stripCaptureDeferred ? "true" : "false", s.stripInvalidations, s.stripFallbacks);
}

FrameResult openSession(core::dsl::Runtime& runtime, core::render::RenderBackend& backend,
                        HWND hwnd, std::uint64_t session, int round, char arm) {
    FrameResult total;
    total.frames = 0;
    total.stats.fullPaint = true;
    for (int step = 0; step < 4; ++step) {
        const FrameResult part = runFrame(runtime, backend, hwnd, step == 0, session);
        report("open_frame", round, arm, part);
        total.okay = part.okay;
        total.paintPending = part.paintPending;
        total.frames += part.frames;
        total.wallMs += part.wallMs;
        total.threadCpuMs += part.threadCpuMs;
        total.stats.fullPaint = total.stats.fullPaint && part.stats.fullPaint;
        total.stats.frozenSceneCaptures += part.stats.frozenSceneCaptures;
        total.stats.frozenPanelFilters += part.stats.frozenPanelFilters;
        total.stats.frozenPanelReuses += part.stats.frozenPanelReuses;
        total.stats.frozenFallbacks += part.stats.frozenFallbacks;
        total.stats.frozenInvalidations += part.stats.frozenInvalidations;
        total.stats.renderDirectPasses += part.stats.renderDirectPasses;
        total.stats.rectDraws += part.stats.rectDraws;
        total.stats.textDraws += part.stats.textDraws;
        total.stats.frozenBytes = part.stats.frozenBytes;
        total.stats.stripCaptures += part.stats.stripCaptures;
        total.stats.stripFilters += part.stats.stripFilters;
        total.stats.stripReuses += part.stats.stripReuses;
        total.stats.stripBytes = part.stats.stripBytes;
        if (!part.okay || !deferred(part.stats)) break;
    }
    report("open", round, arm, total);
    return total;
}

} // namespace

int main(int argc, char** argv) {
    bool defaultBackend = false;
    int hoverFrames = kHoverFrames;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--default-backend") == 0) defaultBackend = true;
        else if (std::strcmp(argv[i], "--strip") == 0) stripMode = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            hoverFrames = std::atoi(argv[++i]);
            if (hoverFrames < 1 || hoverFrames > 10000) return 2;
        } else {
            std::fprintf(stderr, "usage: frozen_runtime_benchmark [--default-backend] [--frames N] [--strip]\n");
            return 2;
        }
    }
    if (defaultBackend) {
        SetEnvironmentVariableA("NEO_D2D_SOFTWARE", nullptr);
        SetEnvironmentVariableA("NEO_WIN32_DC", nullptr);
    } else {
        SetEnvironmentVariableA("NEO_D2D_SOFTWARE", "1");
        SetEnvironmentVariableA("NEO_WIN32_DC", "1");
    }
    std::printf("{\"backend_mode\":\"%s\",\"strip_mode\":%s}\n",
        defaultBackend ? "win32_default_env_unset" : "forced_software_dc", stripMode ? "true" : "false");
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"Frozen Runtime benchmark", WS_POPUP,
                                0, 0, kWidth, kHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd || IsWindowVisible(hwnd)) return 2;
    const auto handle = static_cast<core::window::Handle>(hwnd);
    auto backend = core::render::createRenderBackend(handle);
    if (!backend || !backend->initialize()) { DestroyWindow(hwnd); return 2; }
    bool okay = true;
    {
        core::render::ScopedRenderBackend active(*backend);
        core::dsl::Runtime runtime;
        if (!runtime.initialize()) okay = false;
        std::uint64_t nextSession = 10;
        for (int round = 0; okay && round <= kMeasuredRounds; ++round) {
            for (char arm : kOrder) {
                const std::uint64_t session = arm == 'B' ? ++nextSession : 0;
                const FrameResult open = openSession(runtime, *backend, hwnd, session, round, arm);
                okay = open.okay && open.stats.fullPaint &&
                    (arm == 'A' || (open.frames == 2 && captures(open.stats) == 1 &&
                                    filters(open.stats) == (stripMode ? 1 : 2))) && okay;
                for (int hover = 0; okay && hover < hoverFrames; ++hover) {
                    const FrameResult steady = runFrame(runtime, *backend, hwnd, false, session);
                    report("hover_fullpaint", round, arm, steady);
                    okay = steady.okay && steady.stats.fullPaint &&
                        !steady.paintPending &&
                        (arm == 'A' || (captures(steady.stats) == 0 && filters(steady.stats) == 0 &&
                                        reuses(steady.stats) == (stripMode ? 1 : 2))) && okay;
                }
            }
        }
        if (okay) {
            const std::uint64_t focusSession = ++nextSession;
            const FrameResult beforeFocus = runFrame(runtime, *backend, hwnd, true, focusSession, 0);
            const FrameResult changedFocus = runFrame(runtime, *backend, hwnd, true, focusSession, 1);
            const FrameResult stableFocus = runFrame(runtime, *backend, hwnd, false, focusSession);
            report("pre_capture_focus_revision", 0, 'B', changedFocus);
            okay = beforeFocus.okay && changedFocus.okay && stableFocus.okay &&
                deferred(beforeFocus.stats) && beforeFocus.paintPending &&
                deferred(changedFocus.stats) && changedFocus.paintPending &&
                captures(beforeFocus.stats) == 0 && captures(changedFocus.stats) == 0 &&
                invalidations(changedFocus.stats) == 0 &&
                captures(stableFocus.stats) == 1 && filters(stableFocus.stats) == (stripMode ? 1 : 2) && okay;
            const FrameResult afterCaptureChange = runFrame(runtime, *backend, hwnd, true, focusSession, 2);
            report("post_capture_revision", 0, 'B', afterCaptureChange);
            okay = afterCaptureChange.okay && invalidations(afterCaptureChange.stats) == 1 &&
                captures(afterCaptureChange.stats) == 0 &&
                fallbacks(afterCaptureChange.stats) == (stripMode ? 1 : 2) && okay;

            const std::uint64_t unstableSession = ++nextSession;
            const FrameResult unstable0 = runFrame(runtime, *backend, hwnd, true, unstableSession, 0);
            const FrameResult unstable1 = runFrame(runtime, *backend, hwnd, true, unstableSession, 1);
            const FrameResult unstable2 = runFrame(runtime, *backend, hwnd, true, unstableSession, 2);
            const FrameResult unstable3 = runFrame(runtime, *backend, hwnd, true, unstableSession, 3);
            report("unstable_capture_limit", 0, 'B', unstable3);
            okay = unstable0.okay && unstable1.okay && unstable2.okay && unstable3.okay &&
                deferred(unstable0.stats) && deferred(unstable1.stats) &&
                deferred(unstable2.stats) && !deferred(unstable3.stats) &&
                captures(unstable3.stats) == 0 && fallbacks(unstable3.stats) == (stripMode ? 1 : 2) && okay;
        }
        runtime.shutdown();
    }
    backend.reset();
    core::releaseInputQueue(handle);
    DestroyWindow(hwnd);
    std::printf("{\"status\":\"%s\",\"scope\":\"hidden production Runtime+D2D; all hover frames forced full paint; input/display NOT_RUN\"}\n",
                okay ? "PASS" : "FAIL");
    return okay ? 0 : 1;
}

#else
#include <cstdio>
int main() { std::puts("SKIP: frozen runtime benchmark requires Windows D2D"); return 0; }
#endif
