// Hidden-window production D2D fixture. The frozen arm explicitly renders the
// same complete underlying scene twice on first open: clean prepass/capture,
// then normal menu frame. Timings exclude native input and visible presentation.
#if defined(EUI_RENDER_BACKEND_D2D) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/render/render_backend.h"
#include "core/window/window_backend.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kWidth = 1024;
constexpr int kHeight = 720;
constexpr int kLargeWidth = 3000;
constexpr int kLargeHeight = 3000;
constexpr int kMeasuredRounds = 12;
constexpr int kHoverFrames = 20;
constexpr std::array<core::Rect, 2> kPanels{{
    {180.0f, 60.0f, 280.0f, 430.0f},
    {460.0f, 128.0f, 254.0f, 318.0f}
}};
struct MenuConfig {
    bool rounded = false;
    float sigma = 8.0f;
    float tintOpacity = 0.78f;
};
MenuConfig menuConfig;

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

double wallMs(const Stamp& begin, const Stamp& end) {
    return std::chrono::duration<double, std::milli>(end.wall - begin.wall).count();
}
double cpuMs(const Stamp& begin, const Stamp& end) {
    return static_cast<double>(end.cpu100ns - begin.cpu100ns) / 10000.0;
}

core::render::RoundedRectDrawCommand rectCommand(const core::Rect& rect, const core::Color& color) {
    core::render::RoundedRectDrawCommand command;
    command.rect = rect;
    command.fillColor = color;
    command.opacity = 1.0f;
    command.vertices.resize(6);
    const std::array<core::Vec2, 6> points{{
        {rect.x, rect.y}, {rect.x + rect.width, rect.y},
        {rect.x + rect.width, rect.y + rect.height},
        {rect.x + rect.width, rect.y + rect.height},
        {rect.x, rect.y + rect.height}, {rect.x, rect.y + rect.height}
    }};
    for (std::size_t i = 0; i < points.size(); ++i) {
        command.vertices[i].local = points[i];
        command.vertices[i].screen = {points[i].x, points[i].y, 0.0f};
    }
    return command;
}

void drawScene(core::render::RenderBackend& backend, int width, int height) {
    backend.drawRoundedRect(rectCommand({0, 0, static_cast<float>(width), static_cast<float>(height)},
        {0.12f, 0.15f, 0.19f, 1.0f}), width, height);
    // A repeatable synthetic editor, tab bar and library behind both panels.
    for (int row = 0; row < 42; ++row) {
        const float y = 8.0f + static_cast<float>(row * 16);
        const float x = row % 3 == 0 ? 10.0f : 248.0f;
        const float lineWidth = row % 5 == 0 ? 800.0f : 410.0f;
        backend.drawRoundedRect(rectCommand({x, y, lineWidth, 3.0f},
            row % 4 == 0 ? core::Color{0.31f, 0.43f, 0.58f, 1.0f}
                         : core::Color{0.20f, 0.25f, 0.31f, 1.0f}), width, height);
    }
}

void drawMenus(core::render::RenderBackend& backend, std::uint64_t session,
               int width, int height, bool oversized = false) {
    const std::array<core::Rect, 2> panels = oversized
        ? std::array<core::Rect, 2>{{{250.0f, 250.0f, 2500.0f, 2500.0f}, {0, 0, 0, 0}}}
        : kPanels;
    for (std::size_t i = 0; i < panels.size(); ++i) {
        if (panels[i].width <= 0) continue;
        core::FrozenBackdrop material;
        material.session = session;
        material.panel = static_cast<std::uint64_t>(i + 1);
        material.sourceBounds = panels[i];
        material.sigma = menuConfig.sigma;
        material.tintOpacity = menuConfig.tintOpacity;
        auto command = rectCommand(panels[i], {0.90f, 0.92f, 0.94f, 1.0f});
        command.radius = menuConfig.rounded ? 12.0f : 0.0f;
        command.frozenBackdrop = material;
        if (session) backend.prepareFrozenBackdrop(material);
        backend.drawRoundedRect(command, width, height);
    }
}

struct FrameResult {
    bool okay = false;
    bool captureRequested = false;
    int frames = 1;
    core::render::RenderFrameStats stats{};
    double wall = 0.0, cpu = 0.0;
};

FrameResult frame(core::render::RenderBackend& backend, HWND hwnd, int width, int height,
                  float dpi, std::uint64_t session, std::uint64_t revision,
                  bool full, bool oversized = false, std::uint64_t stripSession = 0,
                  bool stripAllowed = true) {
    FrameResult result;
    const Stamp started = stamp();
    core::render::beginRenderFrameStats(width, height);
    const auto handle = static_cast<core::window::Handle>(hwnd);
    backend.beginFrame({handle, core::window::nativeWindowInfo(handle), width, height, dpi});
    if (!backend.frameReady() || !backend.ensureRenderCache(width, height)) return result;
    result.captureRequested = backend.beginFrozenBackdropFrame(session, revision, width, height, dpi);
    const bool stripRequested = backend.beginCachedStripFrame(stripSession, revision, width, height, dpi);
    const bool paintFull = full || result.captureRequested || stripRequested ||
        core::render::currentRenderFrameStats().stripCaptureDeferred ||
        core::render::currentRenderFrameStats().stripInvalidations > 0 ||
        core::render::currentRenderFrameStats().frozenCaptureDeferred ||
        core::render::currentRenderFrameStats().frozenInvalidations > 0;
    core::render::currentRenderFrameStats().fullPaint = paintFull;
    backend.beginRenderCacheFrame(width, height);
    if (result.captureRequested) {
        backend.setScissor(false, {}, height);
        backend.clear({0.12f, 0.15f, 0.19f, 1.0f});
        drawScene(backend, width, height);
        backend.setScissor(false, {}, height);
        backend.captureFrozenBackdropScene();
    }
    if (paintFull) {
        backend.setScissor(false, {}, height);
        backend.clear({0.12f, 0.15f, 0.19f, 1.0f});
    } else {
        // Same fixed hover trajectory for both arms. The first opaque scene
        // rectangle covers the prior menu only inside this dirty scissor.
        backend.setScissor(true, {175.0f, 55.0f, 545.0f, 445.0f}, height);
    }
    drawScene(backend, width, height);
    if (stripSession) {
        core::FrozenBackdrop strip;
        strip.session = stripSession; strip.panel = 1;
        strip.cachedStrip = true; strip.allowCapture = stripAllowed;
        strip.sourceBounds = {0, 40, static_cast<float>(width), 32};
        strip.sigma = 6; strip.tintOpacity = 0.58f;
        auto bar = rectCommand({0, 0, static_cast<float>(width), 32}, {0.92f, 0.93f, 0.94f, 1});
        bar.frozenBackdrop = strip;
        backend.captureCachedStrip(strip);
        drawMenus(backend, session, width, height, oversized);
        backend.prepareFrozenBackdrop(strip);
        backend.drawRoundedRect(bar, width, height);
    } else drawMenus(backend, session, width, height, oversized);
    backend.setScissor(false, {}, height);
    backend.endRenderCacheFrame();
    backend.blitRenderCache(width, height, paintFull ? core::render::RenderCacheBlitMode::Full
                                             : core::render::RenderCacheBlitMode::Dirty);
    backend.present();
    const Stamp ended = stamp();
    result.okay = backend.framePresented();
    result.stats = core::render::currentRenderFrameStats();
    result.wall = wallMs(started, ended);
    result.cpu = cpuMs(started, ended);
    return result;
}

bool expect(const char* label, bool condition) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", label);
    return condition;
}

void report(const char* phase, int round, char arm, const FrameResult& result) {
    const auto& s = result.stats;
    std::printf("{\"phase\":\"%s\",\"round\":%d,\"arm\":\"%c\","
                "\"warmup\":%s,\"wall_ms\":%.4f,\"thread_cpu_ms\":%.4f,"
                "\"capture_requested\":%s,\"capture_deferred\":%s,\"frames\":%d,\"scene_captures\":%d,"
                "\"panel_filters\":%d,\"panel_reuses\":%d,"
                "\"fallbacks\":%d,\"invalidations\":%d,\"bytes\":%llu,"
                "\"capture_ms\":%.4f,\"filter_ms\":%.4f}\n",
                phase, round, arm, round == 0 && (phase[0] == 'o' || phase[0] == 'h') ? "true" : "false",
                result.wall, result.cpu,
                result.captureRequested ? "true" : "false",
                s.frozenCaptureDeferred ? "true" : "false", result.frames,
                s.frozenSceneCaptures, s.frozenPanelFilters, s.frozenPanelReuses,
                s.frozenFallbacks, s.frozenInvalidations,
                static_cast<unsigned long long>(s.frozenBytes),
                s.frozenCaptureMs, s.frozenFilterMs);
}

FrameResult openSession(core::render::RenderBackend& backend, HWND hwnd, int width, int height,
                        float dpi, std::uint64_t session, std::uint64_t revision,
                        int round, char arm, bool oversized = false) {
    FrameResult total;
    total.frames = 0;
    for (int step = 0; step < 4; ++step) {
        const FrameResult part = frame(backend, hwnd, width, height, dpi, session, revision, true, oversized);
        report("open_frame", round, arm, part);
        total.okay = part.okay;
        total.frames += part.frames;
        total.wall += part.wall;
        total.cpu += part.cpu;
        total.captureRequested |= part.captureRequested;
        total.stats.frozenSceneCaptures += part.stats.frozenSceneCaptures;
        total.stats.frozenPanelFilters += part.stats.frozenPanelFilters;
        total.stats.frozenPanelReuses += part.stats.frozenPanelReuses;
        total.stats.frozenFallbacks += part.stats.frozenFallbacks;
        total.stats.frozenInvalidations += part.stats.frozenInvalidations;
        total.stats.frozenCaptureMs += part.stats.frozenCaptureMs;
        total.stats.frozenFilterMs += part.stats.frozenFilterMs;
        total.stats.frozenBytes = part.stats.frozenBytes;
        if (!part.okay || !part.stats.frozenCaptureDeferred) break;
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
        else if (std::strcmp(argv[i], "--rounded") == 0) menuConfig.rounded = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            hoverFrames = std::atoi(argv[++i]);
            if (hoverFrames < 1 || hoverFrames > 10000) return 2;
        } else if ((std::strcmp(argv[i], "--sigma") == 0 ||
                    std::strcmp(argv[i], "--tint") == 0) && i + 1 < argc) {
            const bool sigma = std::strcmp(argv[i], "--sigma") == 0;
            char* end = nullptr;
            const float value = std::strtof(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !std::isfinite(value) ||
                (sigma ? (value <= 0.0f || value > 64.0f) : (value < 0.0f || value > 1.0f))) {
                std::fprintf(stderr, "invalid %s value\n", sigma ? "sigma" : "tint");
                return 2;
            }
            if (sigma) menuConfig.sigma = value;
            else menuConfig.tintOpacity = value;
        } else {
            std::fprintf(stderr, "usage: frozen_menu_benchmark [--default-backend] [--frames N] [--rounded] [--sigma 12] [--tint .24]\n");
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
    std::printf("{\"backend_mode\":\"%s\",\"rounded\":%s,\"sigma\":%.2f,\"tint_opacity\":%.3f}\n",
                defaultBackend ? "win32_default_env_unset" : "forced_software_dc",
                menuConfig.rounded ? "true" : "false", menuConfig.sigma, menuConfig.tintOpacity);
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"Frozen menu backend fixture", WS_POPUP,
                                0, 0, kWidth, kHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd || IsWindowVisible(hwnd)) return 2;
    const auto handle = static_cast<core::window::Handle>(hwnd);
    auto backend = core::render::createRenderBackend(handle);
    if (!backend || !backend->initialize()) { DestroyWindow(hwnd); return 2; }

    bool okay = true;
    std::uint64_t session = 10;
    constexpr std::array<char, 4> order{{'A', 'B', 'B', 'A'}};
    // Round 0 is warmup. Twelve measured ABBA rounds use identical scene,
    // bounds, two panels, 20 hover redraws and the same hidden software target.
    for (int round = 0; round <= kMeasuredRounds; ++round) {
        for (char arm : order) {
            const bool frozen = arm == 'B';
            const std::uint64_t current = frozen ? ++session : 0;
            const FrameResult open = openSession(*backend, hwnd, kWidth, kHeight, 1.0f,
                                                 current, 100, round, arm);
            okay = expect("open frame", open.okay) && okay;
            if (frozen) {
                okay = expect("one scene capture", open.stats.frozenSceneCaptures == 1) && okay;
                okay = expect("two Gaussian panels", open.stats.frozenPanelFilters == 2) && okay;
                if (menuConfig.rounded)
                    okay = expect("rounded clips draw on capture frame",
                        open.stats.frozenFallbacks == 2) && okay;
            }
            for (int hover = 0; hover < hoverFrames; ++hover) {
                const FrameResult steady = frame(*backend, hwnd, kWidth, kHeight, 1.0f,
                                                 current, 100, false);
                okay = expect("steady frame", steady.okay) && okay;
                if (frozen) {
                    okay = expect("frozen hover no recapture/filter",
                        !steady.captureRequested && steady.stats.frozenSceneCaptures == 0 &&
                        steady.stats.frozenPanelFilters == 0 && steady.stats.frozenPanelReuses == 2 &&
                        (!menuConfig.rounded || steady.stats.frozenFallbacks == 0)) && okay;
                }
                report("hover", round, arm, steady);
            }
        }
    }

    const std::uint64_t focusSession = ++session;
    const FrameResult focusBefore = frame(*backend, hwnd, kWidth, kHeight, 1.0f,
                                          focusSession, 100, true);
    const FrameResult focusChanged = frame(*backend, hwnd, kWidth, kHeight, 1.0f,
                                           focusSession, 101, true);
    const FrameResult focusStable = frame(*backend, hwnd, kWidth, kHeight, 1.0f,
                                          focusSession, 101, true);
    report("pre_capture_focus_revision", 0, 'B', focusChanged);
    okay = expect("pre-capture focus revision defers without invalidation",
        focusBefore.stats.frozenCaptureDeferred && focusChanged.stats.frozenCaptureDeferred &&
        focusBefore.stats.frozenSceneCaptures == 0 && focusChanged.stats.frozenSceneCaptures == 0 &&
        focusChanged.stats.frozenInvalidations == 0 &&
        focusStable.stats.frozenSceneCaptures == 1 && focusStable.stats.frozenPanelFilters == 2) && okay;
    const std::uint64_t revisionSession = focusSession;
    const FrameResult revision = frame(*backend, hwnd, kWidth, kHeight, 1.0f,
                                       revisionSession, 102, false);
    report("revision_invalidation", 0, 'B', revision);
    okay = expect("revision invalidates exactly once", revision.stats.frozenInvalidations == 1 &&
        revision.stats.frozenFallbacks == 2 && revision.stats.frozenSceneCaptures == 0) && okay;
    const FrameResult stillPoisoned = frame(*backend, hwnd, kWidth, kHeight, 1.0f,
                                            revisionSession, 103, false);
    okay = expect("same opening remains opaque", stillPoisoned.stats.frozenInvalidations == 0 &&
        stillPoisoned.stats.frozenFallbacks == 2 && stillPoisoned.stats.frozenSceneCaptures == 0) && okay;
    const std::uint64_t dpiSession = ++session;
    okay = expect("new session before DPI", openSession(*backend, hwnd, kWidth, kHeight,
        1.0f, dpiSession, 100, 0, 'B').stats.frozenSceneCaptures == 1) && okay;
    const FrameResult dpi = frame(*backend, hwnd, kWidth, kHeight, 1.25f,
                                  dpiSession, 100, false);
    report("dpi_invalidation", 0, 'B', dpi);
    okay = expect("DPI invalidates", dpi.stats.frozenInvalidations == 1 &&
        dpi.stats.frozenFallbacks == 2) && okay;

    const std::uint64_t cacheSession = ++session;
    okay = expect("new session before cache release", openSession(*backend, hwnd, kWidth, kHeight,
        1.0f, cacheSession, 100, 0, 'B').stats.frozenSceneCaptures == 1) && okay;
    backend->releaseRenderCache();
    const FrameResult resource = frame(*backend, hwnd, kWidth, kHeight, 1.0f,
                                       cacheSession, 100, true);
    report("resource_invalidation", 0, 'B', resource);
    okay = expect("resource release invalidates", resource.stats.frozenInvalidations == 1 &&
        resource.stats.frozenFallbacks == 2) && okay;

    SetWindowPos(hwnd, nullptr, 0, 0, kWidth + 64, kHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const std::uint64_t sizeSession = ++session;
    okay = expect("new session before size", openSession(*backend, hwnd, kWidth + 64, kHeight,
        1.0f, sizeSession, 100, 0, 'B').stats.frozenSceneCaptures == 1) && okay;
    SetWindowPos(hwnd, nullptr, 0, 0, kWidth, kHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const FrameResult size = frame(*backend, hwnd, kWidth, kHeight, 1.0f,
                                   sizeSession, 100, true);
    report("size_invalidation", 0, 'B', size);
    okay = expect("size invalidates", size.stats.frozenInvalidations == 1 &&
        size.stats.frozenFallbacks == 2) && okay;
    const FrameResult reopened = openSession(*backend, hwnd, kWidth, kHeight, 1.0f,
                                              ++session, 100, 0, 'B');
    okay = expect("reopen captures again", reopened.stats.frozenSceneCaptures == 1) && okay;
    const FrameResult closed = frame(*backend, hwnd, kWidth, kHeight, 1.0f, 0, 0, true);
    okay = expect("close releases frozen resources", closed.stats.frozenBytes == 0 &&
        closed.stats.frozenInvalidations == 1) && okay;

    // Larger, still hidden fixture: the full scene fits in 64 MiB, while the
    // padded 2500-square panel plus its input/output temporaries exceeds it.
    SetWindowPos(hwnd, nullptr, 0, 0, kLargeWidth, kLargeHeight,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const FrameResult budget = openSession(*backend, hwnd, kLargeWidth, kLargeHeight,
                                           1.0f, ++session, 100, 0, 'B', true);
    report("budget_fallback", 0, 'B', budget);
    okay = expect("budget scene captured", budget.stats.frozenSceneCaptures == 1) && okay;
    okay = expect("over budget panel stays opaque", budget.stats.frozenPanelFilters == 0 &&
        budget.stats.frozenFallbacks == 2 && budget.stats.frozenBytes <= 64ull * 1024ull * 1024ull) && okay;

    const std::uint64_t unstableSession = ++session;
    const FrameResult unstable0 = frame(*backend, hwnd, kLargeWidth, kLargeHeight,
                                        1.0f, unstableSession, 200, true, true);
    const FrameResult unstable1 = frame(*backend, hwnd, kLargeWidth, kLargeHeight,
                                        1.0f, unstableSession, 201, true, true);
    const FrameResult unstable2 = frame(*backend, hwnd, kLargeWidth, kLargeHeight,
                                        1.0f, unstableSession, 202, true, true);
    const FrameResult unstable3 = frame(*backend, hwnd, kLargeWidth, kLargeHeight,
                                        1.0f, unstableSession, 203, true, true);
    report("unstable_capture_limit", 0, 'B', unstable3);
    okay = expect("unstable opening stops after three deferred frames",
        unstable0.stats.frozenCaptureDeferred && unstable1.stats.frozenCaptureDeferred &&
        unstable2.stats.frozenCaptureDeferred && !unstable3.stats.frozenCaptureDeferred &&
        unstable3.stats.frozenSceneCaptures == 0 && unstable3.stats.frozenFallbacks == 1) && okay;

    // Independent top-bar strip: bounded one-time work and no menu-session
    // interference. These contract frames are outside the ABBA timings above.
    const auto stripSettling = frame(*backend, hwnd, kWidth, kHeight, 1, 0, 100, true, false, 9001);
    const auto stripCapture = frame(*backend, hwnd, kWidth, kHeight, 1, 0, 100, true, false, 9001);
    okay = expect("strip settles before its sole narrow capture", stripSettling.stats.stripCaptures == 0 &&
        stripSettling.stats.stripCaptureDeferred && stripCapture.stats.stripCaptures == 1 &&
        stripCapture.stats.stripFilters == 1 && stripCapture.stats.frozenSceneCaptures == 0 &&
        stripCapture.stats.stripBytes < 4u * 1024u * 1024u) && okay;
    for (int reuse = 0; reuse < 60; ++reuse) {
        const auto stable = frame(*backend, hwnd, kWidth, kHeight, 1, 0, 100, true, false, 9001);
        okay = expect("strip steady frames perform no capture, Gaussian or deferred wake", stable.stats.stripCaptures == 0 &&
            stable.stats.stripFilters == 0 && stable.stats.stripReuses == 1 &&
            !stable.stats.stripCaptureDeferred) && okay;
    }
    const auto stripMenuSettle = frame(*backend, hwnd, kWidth, kHeight, 1, ++session, 100, true, false, 9001);
    const auto stripMenuCapture = frame(*backend, hwnd, kWidth, kHeight, 1, session, 100, true, false, 9001);
    okay = expect("menu and strip caches coexist", stripMenuSettle.stats.stripCaptures == 0 &&
        stripMenuCapture.stats.frozenSceneCaptures == 1 && stripMenuCapture.stats.frozenPanelFilters == 2 &&
        stripMenuCapture.stats.stripReuses == 1 && stripMenuCapture.stats.stripFilters == 0) && okay;
    const auto stripChanged = frame(*backend, hwnd, kWidth, kHeight, 1, 0, 101, true, false, 9001);
    const auto stripPoisoned = frame(*backend, hwnd, kWidth, kHeight, 1, 0, 101, true, false, 9001);
    okay = expect("content changes invalidate strip without refiltering", stripChanged.stats.stripInvalidations == 1 &&
        stripChanged.stats.stripCaptures == 0 && stripPoisoned.stats.stripCaptures == 0 &&
        stripPoisoned.stats.stripFallbacks == 1 && !stripPoisoned.stats.stripCaptureDeferred) && okay;
    frame(*backend, hwnd, kWidth, kHeight, 1, 0, 101, true, false, 9002);
    const auto stripRearmed = frame(*backend, hwnd, kWidth, kHeight, 1, 0, 101, true, false, 9002);
    okay = expect("new strip activation can capture once", stripRearmed.stats.stripCaptures == 1) && okay;
    frame(*backend, hwnd, kWidth, kHeight, 1, 0, 101, true, false, 9003, false);
    const auto stripBlocked = frame(*backend, hwnd, kWidth, kHeight, 1, 0, 101, true, false, 9003, false);
    okay = expect("popup blocks capture without repeated deferred frames", stripBlocked.stats.stripCaptures == 0 &&
        !stripBlocked.stats.stripCaptureDeferred && stripBlocked.stats.stripFallbacks == 1) && okay;
    std::printf("{\"strip_contract\":\"%s\",\"steady_frames\":60,\"capture_bytes\":%llu}\n",
        okay ? "PASS" : "FAIL", static_cast<unsigned long long>(stripCapture.stats.stripBytes));

    backend.reset();
    DestroyWindow(hwnd);
    std::printf("{\"status\":\"%s\",\"scope\":\"hidden production D2D backend; input/display NOT_RUN\"}\n",
                okay ? "PASS" : "FAIL");
    return okay ? 0 : 1;
}

#else
#include <cstdio>
int main() { std::puts("SKIP: frozen menu benchmark requires Windows D2D"); return 0; }
#endif
