#pragma once

// GPU resize 低干扰计数器（B1，为实机条件卡测量做准备）。
//
// 口径（对应实施指引 §2.1）：
//   - framebuffer/window refresh 回调触发次数：每次 GLFW 回调进入即计。
//   - liveResizePaints：paintResizeIfDue 真正执行 paintResize 的次数
//     （防重入跳过、30Hz 限速跳过均不计）。
//   - renderCacheAllocations/Releases：主 render cache 超过容量、真实
//     glGenFramebuffers/glGenTextures 分配与 glDelete* 释放的事件次数。
//     容量内改逻辑宽高复用不算分配。
//   - retainedLayerAllocations/Releases：retained layer storage
//     （opengl_image.cpp resizeLayer 路径）真实重建纹理/FBO 的事件次数。
//     容量内复用不算分配。
//
// 输出方式：只有 NEO_GPU_STATS 精确等于 "1" 时才在退出时打印一次汇总；
// 默认路径零 IO、零 GL 同步查询，仅原子计数（relaxed）。
// 注意：RenderFrameStats 是逐帧 thread_local 且每帧清零（见
// render_backend.cpp beginRenderFrameStats），不适合做进程级累计，
// 故这里用独立的全局原子计数器。
//
// 2026-09-30 补：周期性心跳落盘（heartbeat）。
// 起因是 GPU 卡死（LiveKernelEvent 141）取证：原来的汇总只在正常退出时打印，
// 一旦硬挂死就什么都拿不到，无法区分"回调风暴"、"真实分配增长"和"某一次重绘
// 自身耗时爆炸"。现在 NEO_GPU_STATS=1 时，从 resize 回调与重绘路径按间隔把
// 计数器追加写入文本文件；卡死时主线程不再进入回调，文件最后一行就是卡住前的
// 现场。路径 NEO_GPU_STATS_FILE（缺省 <cwd>/neo_gpu_stats.log，用 ASCII 路径），
// 间隔 NEO_GPU_STATS_INTERVAL_MS（缺省 1000，下限 50）。阶段切换时允许以 100ms
// 为下限插一行，保证"最后一次进入的是哪个阶段"可读。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace core::render {

// NEO_GPU_STATS 判定纯函数：只有精确 "1" 开启汇总，其余（含未设置）关闭。
inline bool gpuStatsEnabled(const char* value) {
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

struct GpuResizeStats {
    std::atomic<std::uint64_t> framebufferResizeCallbacks{0};
    std::atomic<std::uint64_t> windowRefreshCallbacks{0};
    std::atomic<std::uint64_t> liveResizePaints{0};
    std::atomic<std::uint64_t> renderCacheAllocations{0};
    std::atomic<std::uint64_t> renderCacheReleases{0};
    std::atomic<std::uint64_t> retainedLayerAllocations{0};
    std::atomic<std::uint64_t> retainedLayerReleases{0};
    // 真正执行的那次重绘自身耗时（微秒）：末次值与历史最大值。
    // maxPaint 远大于正常帧耗时，说明卡死前最后一次重绘本身就在长时间等待。
    std::atomic<std::uint64_t> lastResizePaintMicros{0};
    std::atomic<std::uint64_t> maxResizePaintMicros{0};
    std::atomic<std::uint64_t> heartbeatsWritten{0};

    // 心跳落盘的私有状态：只在渲染线程使用，不加锁。
    bool trailStarted = false;
    std::chrono::steady_clock::time_point trailStartedAt{};
    std::chrono::steady_clock::time_point lastTrailWrite{};
    char lastPhase[48]{};

    // 读环境变量。只读不写，MSVC 的 getenv "不安全"提示在此用法下没有意义
    // （同 model/settings.cpp、model/font_catalog.cpp 的注释口径）。
    static const char* readEnv(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        const char* value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        return value;
    }

    // 进程内只读一次环境变量（magic static，线程安全）。
    static bool summaryEnabled() {
        static const bool enabled = [] { return gpuStatsEnabled(readEnv("NEO_GPU_STATS")); }();
        return enabled;
    }

    static long long heartbeatIntervalMs() {
        static const long long value = [] {
            const char* raw = readEnv("NEO_GPU_STATS_INTERVAL_MS");
            long long parsed = 1000;
            if (raw != nullptr && raw[0] != '\0') {
                char* end = nullptr;
                const long long candidate = std::strtoll(raw, &end, 10);
                if (end != raw && candidate >= 50 && candidate <= 60000) {
                    parsed = candidate;
                }
            }
            return parsed;
        }();
        return value;
    }

    static const char* trailPath() {
        static const char* value = []() -> const char* {
            const char* raw = readEnv("NEO_GPU_STATS_FILE");
            return (raw != nullptr && raw[0] != '\0') ? raw : "neo_gpu_stats.log";
        }();
        return value;
    }

    // 追加模式打开一次并写会话表头；打不开就永久放弃（不重复尝试、不影响主流程）。
    static std::FILE* trailFile() {
        static std::FILE* file = []() -> std::FILE* {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
            std::FILE* opened = std::fopen(trailPath(), "a");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
            if (opened != nullptr) {
                const std::time_t now = std::time(nullptr);
                std::tm local{};
#if defined(_MSC_VER)
                localtime_s(&local, &now);
#else
                localtime_r(&now, &local);
#endif
                char stamp[32]{};
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
                std::fprintf(opened, "\n# neo_gpu_stats session start %s\n", stamp);
                std::fflush(opened);
            }
            return opened;
        }();
        return file;
    }

    void addFramebufferResizeCallback() { framebufferResizeCallbacks.fetch_add(1, std::memory_order_relaxed); }
    void addWindowRefreshCallback() { windowRefreshCallbacks.fetch_add(1, std::memory_order_relaxed); }
    void addLiveResizePaint() { liveResizePaints.fetch_add(1, std::memory_order_relaxed); }
    void addRenderCacheAllocation() { renderCacheAllocations.fetch_add(1, std::memory_order_relaxed); }
    void addRenderCacheRelease() { renderCacheReleases.fetch_add(1, std::memory_order_relaxed); }
    void addRetainedLayerAllocation() { retainedLayerAllocations.fetch_add(1, std::memory_order_relaxed); }
    void addRetainedLayerRelease() { retainedLayerReleases.fetch_add(1, std::memory_order_relaxed); }

    void addResizePaintDuration(double milliseconds) {
        const std::uint64_t micros =
            milliseconds > 0.0 ? static_cast<std::uint64_t>(milliseconds * 1000.0) : 0;
        lastResizePaintMicros.store(micros, std::memory_order_relaxed);
        std::uint64_t previous = maxResizePaintMicros.load(std::memory_order_relaxed);
        while (micros > previous &&
               !maxResizePaintMicros.compare_exchange_weak(previous, micros, std::memory_order_relaxed)) {
        }
    }

    // 周期性把计数器追加写入文本文件，作为卡死时的现场。
    // phase 用固定字面量（如 "refresh-cb" / "paint-enter" / "paint-done"）。
    void heartbeat(const char* phase) {
        if (!summaryEnabled()) {
            return;
        }
        std::FILE* file = trailFile();
        if (file == nullptr) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!trailStarted) {
            trailStarted = true;
            trailStartedAt = now;
            lastTrailWrite = now - std::chrono::hours(24);  // 首行立即写出
        }
        const char* current = phase != nullptr ? phase : "";
        const auto sinceWriteMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTrailWrite).count();
        const bool phaseChanged = std::strcmp(current, lastPhase) != 0;
        const bool due = sinceWriteMs >= heartbeatIntervalMs();
        const bool phaseEdge = phaseChanged && sinceWriteMs >= 100;
        if (!due && !phaseEdge) {
            return;
        }
        lastTrailWrite = now;
        std::snprintf(lastPhase, sizeof(lastPhase), "%s", current);
        const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - trailStartedAt).count();
        std::fprintf(file,
                     "t=%lldms phase=%-12s fb=%llu refresh=%llu paints=%llu cache=%llu/%llu layer=%llu/%llu "
                     "lastPaint=%.1fms maxPaint=%.1fms\n",
                     static_cast<long long>(totalMs), current,
                     static_cast<unsigned long long>(framebufferResizeCallbacks.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(windowRefreshCallbacks.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(liveResizePaints.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(renderCacheAllocations.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(renderCacheReleases.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(retainedLayerAllocations.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(retainedLayerReleases.load(std::memory_order_relaxed)),
                     static_cast<double>(lastResizePaintMicros.load(std::memory_order_relaxed)) / 1000.0,
                     static_cast<double>(maxResizePaintMicros.load(std::memory_order_relaxed)) / 1000.0);
        std::fflush(file);
        heartbeatsWritten.fetch_add(1, std::memory_order_relaxed);
    }

    // 退出时一次性汇总。未启用（默认）时是零开销空操作。
    void printSummary(const char* heading) const {
        if (!summaryEnabled()) {
            return;
        }
        std::fprintf(stderr, "[NEO_GPU_STATS] session: %s\n", heading != nullptr ? heading : "");
        std::fprintf(stderr, "[NEO_GPU_STATS] framebuffer resize callbacks: %llu\n",
                     static_cast<unsigned long long>(framebufferResizeCallbacks.load(std::memory_order_relaxed)));
        std::fprintf(stderr, "[NEO_GPU_STATS] window refresh callbacks: %llu\n",
                     static_cast<unsigned long long>(windowRefreshCallbacks.load(std::memory_order_relaxed)));
        std::fprintf(stderr, "[NEO_GPU_STATS] live-resize paints executed: %llu\n",
                     static_cast<unsigned long long>(liveResizePaints.load(std::memory_order_relaxed)));
        std::fprintf(stderr, "[NEO_GPU_STATS] render cache texture/FBO allocations: %llu\n",
                     static_cast<unsigned long long>(renderCacheAllocations.load(std::memory_order_relaxed)));
        std::fprintf(stderr, "[NEO_GPU_STATS] render cache texture/FBO releases: %llu\n",
                     static_cast<unsigned long long>(renderCacheReleases.load(std::memory_order_relaxed)));
        std::fprintf(stderr, "[NEO_GPU_STATS] retained layer storage allocations: %llu\n",
                     static_cast<unsigned long long>(retainedLayerAllocations.load(std::memory_order_relaxed)));
        std::fprintf(stderr, "[NEO_GPU_STATS] retained layer storage releases: %llu\n",
                     static_cast<unsigned long long>(retainedLayerReleases.load(std::memory_order_relaxed)));
        std::fprintf(stderr, "[NEO_GPU_STATS] resize paint last/max: %.1fms / %.1fms\n",
                     static_cast<double>(lastResizePaintMicros.load(std::memory_order_relaxed)) / 1000.0,
                     static_cast<double>(maxResizePaintMicros.load(std::memory_order_relaxed)) / 1000.0);
        std::fprintf(stderr, "[NEO_GPU_STATS] trail file: %s (lines: %llu)\n", trailPath(),
                     static_cast<unsigned long long>(heartbeatsWritten.load(std::memory_order_relaxed)));
        std::fflush(stderr);
    }
};

inline GpuResizeStats& gpuResizeStats() {
    static GpuResizeStats stats;
    return stats;
}

} // namespace core::render
