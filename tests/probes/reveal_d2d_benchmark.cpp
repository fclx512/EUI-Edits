// Bounded renderer fixture: two 176x34 settings buttons on a hidden Win32
// window, using the production D2D backend and RoundedRectPrimitive. This
// measures render-thread work, not native input, composition or display latency.
#if defined(EUI_RENDER_BACKEND_D2D) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/render/primitive.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kWidth = 512;
constexpr int kHeight = 192;
constexpr int kWideWidth = 1024;

struct Stamp {
    Clock::time_point wall;
    std::uint64_t thread100ns = 0;
};

Stamp stamp() {
    Stamp result;
    result.wall = Clock::now();
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        result.thread100ns = k.QuadPart + u.QuadPart;
    }
    return result;
}

struct Counts {
    std::uint64_t brushCreates = 0;
    std::uint64_t revealDraws = 0;
};

bool renderFrame(core::render::RenderBackend& backend, HWND hwnd,
                 std::array<core::RoundedRectPrimitive, 2>& buttons,
                 bool effect, int sequence, int width, const core::Color& tint,
                 Counts& totals) {
    core::render::ScopedRenderBackend active(backend);
    core::render::beginRenderFrameStats(width, kHeight);
    auto handle = static_cast<core::window::Handle>(hwnd);
    backend.beginFrame({handle, core::window::nativeWindowInfo(handle), width, kHeight, 1.0f});
    if (!backend.frameReady() || !backend.ensureRenderCache(width, kHeight)) return false;
    backend.beginRenderCacheFrame(width, kHeight);
    backend.setScissor(false, {}, kHeight);
    backend.clear({0.12f, 0.12f, 0.14f, 1.0f});

    const float pointerX = 254.0f + static_cast<float>((sequence * 7) % 176);
    const float pointerY = 28.0f + static_cast<float>((sequence * 3) % 34);
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        core::RevealEffect reveal{};
        if (effect) {
            reveal.enabled = true;
            reveal.center = {pointerX - 254.0f,
                             pointerY - (index == 0 ? 28.0f : 112.0f)};
            reveal.radius = 72.0f;
            reveal.tint = tint;
            reveal.fillOpacity = index == 0 ? 0.13f : 0.04f;
            reveal.borderOpacity = 0.72f;
            reveal.borderWidth = 1.0f;
            reveal.pressCenter = {84.0f, 17.0f};
            reveal.pressRadius = 12.0f + static_cast<float>(sequence % 24) * 3.0f;
            reveal.pressOpacity = index == 0 && sequence % 37 < 7 ? 0.22f : 0.0f;
        }
        buttons[index].setReveal(reveal);
        buttons[index].render(width, kHeight);
    }
    backend.endRenderCacheFrame();
    backend.blitRenderCache(width, kHeight);
    backend.present();
    if (!backend.framePresented()) return false;
    const auto& stats = core::render::currentRenderFrameStats();
    totals.brushCreates += static_cast<std::uint64_t>(stats.revealBrushCreates);
    totals.revealDraws += static_cast<std::uint64_t>(stats.revealDraws);
    return true;
}

bool expectCreates(core::render::RenderBackend& backend, HWND hwnd,
                   std::array<core::RoundedRectPrimitive, 2>& buttons,
                   int sequence, int width, const core::Color& tint,
                   std::uint64_t expected, const char* label) {
    Counts counts;
    if (!renderFrame(backend, hwnd, buttons, true, sequence, width, tint, counts)) return false;
    std::printf("cache,%s,brushCreates=%llu,revealDraws=%llu\n", label,
                static_cast<unsigned long long>(counts.brushCreates),
                static_cast<unsigned long long>(counts.revealDraws));
    return counts.brushCreates == expected && counts.revealDraws > 0;
}

} // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 3 && std::strcmp(argv[1], "--frames") == 0 &&
                       std::strcmp(argv[2], "3") == 0;
    const int framesPerArm = smoke ? 3 : 800;
    const int warmupFrames = smoke ? 3 : 120;
    SetEnvironmentVariableA("NEO_D2D_SOFTWARE", "1");
    SetEnvironmentVariableA("NEO_WIN32_DC", "1");

    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"EUI reveal renderer fixture",
                                WS_POPUP, 0, 0, kWidth, kHeight,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd || IsWindowVisible(hwnd)) {
        std::fprintf(stderr, "FAIL: hidden fixture HWND creation\n");
        if (hwnd) DestroyWindow(hwnd);
        return 2;
    }
    auto handle = static_cast<core::window::Handle>(hwnd);
    auto backend = core::render::createRenderBackend(handle);
    if (!backend || !backend->initialize()) {
        std::fprintf(stderr, "FAIL: D2D backend initialization\n");
        DestroyWindow(hwnd);
        return 2;
    }
    std::array<core::RoundedRectPrimitive, 2> buttons;
    buttons[0].setBounds(254.0f, 28.0f, 176.0f, 34.0f);
    buttons[1].setBounds(254.0f, 112.0f, 176.0f, 34.0f);
    for (auto& button : buttons) {
        button.initialize();
        button.setColor({0.18f, 0.18f, 0.21f, 1.0f});
        button.setCornerRadius(0.0f);
    }
    const core::Color tint{0.35f, 0.65f, 1.0f, 1.0f};
    const core::Color secondTint{0.85f, 0.45f, 0.32f, 1.0f};
    bool okay = true;
    for (int frame = 0; frame < warmupFrames; ++frame) {
        Counts ignored;
        okay = renderFrame(*backend, hwnd, buttons, frame % 2 != 0, frame,
                           kWidth, tint, ignored) && okay;
    }
    if (!okay) { std::fprintf(stderr, "FAIL: warmup frame\n"); backend.reset(); DestroyWindow(hwnd); return 2; }

    std::printf("fixture=D2D software hidden HWND; buttons=2x176x34; "
                "size=%dx%d; warmup=%d; framesPerArm=%d; rounds=3; "
                "CPU=GetThreadTimes; wall=steady_clock; display/input=NOT_RUN\n",
                kWidth, kHeight, warmupFrames, framesPerArm);
    constexpr std::array<bool, 4> order{false, true, true, false};
    for (int round = 0; round < 3; ++round) {
        for (std::size_t arm = 0; arm < order.size(); ++arm) {
            const Stamp before = stamp();
            Counts counts;
            for (int frame = 0; frame < framesPerArm; ++frame) {
                okay = renderFrame(*backend, hwnd, buttons, order[arm],
                                   round * 100000 + static_cast<int>(arm) * framesPerArm + frame,
                                   kWidth, tint, counts) && okay;
            }
            const Stamp after = stamp();
            const double wallUs = std::chrono::duration<double, std::micro>(
                after.wall - before.wall).count();
            const double cpuUs = static_cast<double>(after.thread100ns - before.thread100ns) / 10.0;
            std::printf("round=%d,arm=%zu,effect=%d,frames=%d,wall_us_per_frame=%.3f,"
                        "thread_cpu_us_per_frame=%.3f,brushCreates=%llu,revealDraws=%llu\n",
                        round + 1, arm + 1, order[arm] ? 1 : 0, framesPerArm,
                        wallUs / framesPerArm, cpuUs / framesPerArm,
                        static_cast<unsigned long long>(counts.brushCreates),
                        static_cast<unsigned long long>(counts.revealDraws));
            if (!order[arm] && (counts.brushCreates != 0 || counts.revealDraws != 0)) okay = false;
            if (order[arm] && counts.brushCreates != 0) okay = false; // warm cache
        }
    }

    okay = expectCreates(*backend, hwnd, buttons, 700001, kWidth, secondTint, 1, "theme-change") && okay;
    okay = expectCreates(*backend, hwnd, buttons, 700002, kWidth, secondTint, 0, "steady-theme") && okay;
    okay = expectCreates(*backend, hwnd, buttons, 700003, kWidth, tint, 0, "cached-original-theme") && okay;
    backend->releaseRenderCache();
    okay = expectCreates(*backend, hwnd, buttons, 700004, kWidth, tint, 1, "release-cache") && okay;
    SetWindowPos(hwnd, nullptr, 0, 0, kWideWidth, kHeight,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    okay = expectCreates(*backend, hwnd, buttons, 700005, kWideWidth, tint, 1, "resize-cache") && okay;
    backend.reset();
    backend = core::render::createRenderBackend(handle);
    if (!backend || !backend->initialize()) okay = false;
    else okay = expectCreates(*backend, hwnd, buttons, 700006, kWideWidth, tint, 1, "new-device") && okay;
    backend.reset();
    DestroyWindow(hwnd);
    std::printf("status=%s; scope=renderer fixture only\n", okay ? "PASS" : "FAIL");
    return okay ? 0 : 1;
}

#else
#include <cstdio>
int main() {
    std::puts("SKIP: reveal D2D benchmark requires Windows D2D");
    return 0;
}
#endif
