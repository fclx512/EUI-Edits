// B1 GPU 部分：NEO_LIVE_RESIZE 判定纯函数 + GPU resize 计数器单测。
// 全部无头：不初始化 GLFW、不创建 GL 上下文，只测纯逻辑与原子计数。

#include "core/app/live_resize_option.h"
#include "core/render/gpu_resize_stats.h"

#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void expectEq(bool actual, bool expected, const char* label) {
    if (actual != expected) {
        std::cerr << "FAIL: " << label << " expected " << (expected ? "true" : "false")
                  << ", got " << (actual ? "true" : "false") << "\n";
        ++failures;
    }
}

void expectU64(std::uint64_t actual, std::uint64_t expected, const char* label) {
    if (actual != expected) {
        std::cerr << "FAIL: " << label << " expected " << expected << ", got " << actual << "\n";
        ++failures;
    }
}

void testLiveResizeDisabled() {
    // 未设置（nullptr）与空值：保持开启（不禁用）。
    expectEq(app::liveResizeDisabled(nullptr), false, "NEO_LIVE_RESIZE unset");
    expectEq(app::liveResizeDisabled(""), false, "NEO_LIVE_RESIZE empty");

    // 精确 "0"：禁用（安全模式）。
    expectEq(app::liveResizeDisabled("0"), true, "NEO_LIVE_RESIZE=0 disables");

    // "1"、非法值、多字符：均保持开启（与现 HEAD 行为一致，默认不变）。
    expectEq(app::liveResizeDisabled("1"), false, "NEO_LIVE_RESIZE=1 keeps enabled");
    expectEq(app::liveResizeDisabled("01"), false, "NEO_LIVE_RESIZE=01 (multi-char) keeps enabled");
    expectEq(app::liveResizeDisabled("0 "), false, "NEO_LIVE_RESIZE='0 ' (trailing space) keeps enabled");
    expectEq(app::liveResizeDisabled("00"), false, "NEO_LIVE_RESIZE=00 keeps enabled");
    expectEq(app::liveResizeDisabled("true"), false, "NEO_LIVE_RESIZE=true keeps enabled");
    expectEq(app::liveResizeDisabled("false"), false, "NEO_LIVE_RESIZE=false keeps enabled");
    expectEq(app::liveResizeDisabled("no"), false, "NEO_LIVE_RESIZE=no keeps enabled");
    expectEq(app::liveResizeDisabled("O"), false, "NEO_LIVE_RESIZE=O (letter) keeps enabled");
    expectEq(app::liveResizeDisabled("０"), false, "NEO_LIVE_RESIZE=fullwidth zero keeps enabled");
}

void testGpuStatsEnabled() {
    // NEO_GPU_STATS 只有精确 "1" 才开启汇总。
    expectEq(core::render::gpuStatsEnabled(nullptr), false, "NEO_GPU_STATS unset");
    expectEq(core::render::gpuStatsEnabled(""), false, "NEO_GPU_STATS empty");
    expectEq(core::render::gpuStatsEnabled("1"), true, "NEO_GPU_STATS=1 enables");
    expectEq(core::render::gpuStatsEnabled("0"), false, "NEO_GPU_STATS=0 disables");
    expectEq(core::render::gpuStatsEnabled("11"), false, "NEO_GPU_STATS=11 (multi-char) disables");
    expectEq(core::render::gpuStatsEnabled("true"), false, "NEO_GPU_STATS=true disables");
    expectEq(core::render::gpuStatsEnabled("2"), false, "NEO_GPU_STATS=2 disables");
}

void testGpuResizeStatsCounters() {
    core::render::GpuResizeStats stats;
    expectU64(stats.framebufferResizeCallbacks.load(), 0, "initial framebufferResizeCallbacks");
    expectU64(stats.retainedLayerAllocations.load(), 0, "initial retainedLayerAllocations");

    stats.addFramebufferResizeCallback();
    stats.addFramebufferResizeCallback();
    stats.addWindowRefreshCallback();
    stats.addLiveResizePaint();
    stats.addRenderCacheAllocation();
    stats.addRenderCacheRelease();
    stats.addRetainedLayerAllocation();
    stats.addRetainedLayerRelease();

    expectU64(stats.framebufferResizeCallbacks.load(), 2, "framebufferResizeCallbacks after adds");
    expectU64(stats.windowRefreshCallbacks.load(), 1, "windowRefreshCallbacks after adds");
    expectU64(stats.liveResizePaints.load(), 1, "liveResizePaints after adds");
    expectU64(stats.renderCacheAllocations.load(), 1, "renderCacheAllocations after adds");
    expectU64(stats.renderCacheReleases.load(), 1, "renderCacheReleases after adds");
    expectU64(stats.retainedLayerAllocations.load(), 1, "retainedLayerAllocations after adds");
    expectU64(stats.retainedLayerReleases.load(), 1, "retainedLayerReleases after adds");

    // 汇总打印在未启用（默认）时必须是无副作用的空操作；这里只要求不崩溃。
    stats.printSummary("unit-test");
}

} // namespace

int main() {
    testLiveResizeDisabled();
    testGpuStatsEnabled();
    testGpuResizeStatsCounters();
    if (failures != 0) {
        std::cerr << failures << " gpu_resize_stats assertions failed\n";
        return 1;
    }
    std::cout << "gpu_resize_stats: all assertions passed\n";
    return 0;
}
