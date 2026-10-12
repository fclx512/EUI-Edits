#pragma once

// 只读思维导图预览的页自有缓存（实施计划阶段 D）。
//
// 归属与生命周期：由 DocumentSession 以 std::unique_ptr 持有（state/app_state.h），
// 随页搬迁（DocumentSession 整体 move）、关页/预算淘汰即释放；**不新增全局单例**，
// 不做全局↔页的 move（LP 计划那套全局槽位语义不适用——本缓存没有全局消费者）。
//
// 唯一一份派生数据：预览打开的冷路径把解析树 move 进缓存，测好的标签尺寸与布局
// 几何也 move 进来；PreviewState（model/mindmap_preview_state.h）只留身份锚与视口，
// 生产路径不保留第二份树或第二份布局副本。cache.source 是缓存自己的输入快照
// （锚点重校验用，受 Limits::sourceBytes 上限约束），与 lp::PlanCache 持有 text
// 是同一套口径，不算"第二份树"。
//
// 键必须覆盖（任一变化即重建，缺键不得命中旧树）：文档身份（tabId + path）、
// 正文代次（revision / textRevision）、目标围栏块（beg/end/语言）、字体（editor/code）、
// 字号、DPI/effectiveScale、面板宽度、主题与主题文件版本、缓存语义版本。

#include "model/mindmap_layout.h"
#include "model/mindmap_plan.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace neo::mindmap {

// 测字 / 布局 / 键口径变化时 +1：旧缓存整体失效。
inline constexpr std::uint64_t kMindmapCacheVersion = 1;

// 活动页预览的独立派生数据上限（建议 16 MiB，不含共享字体/整窗资源；非实测值，
// 可经 NEO_MINDMAP_CACHE_MIB 覆盖以便实测调整）。超限 → storeMindmapCache 整体
// 释放并回退（保留原文 + toast），绝不把超限结果留在页上。
inline constexpr std::uint64_t kDefaultMindmapActiveBudgetMiB = 16;

inline std::uint64_t mindmapActiveBudgetBytes() {
    if (const char* env = std::getenv("NEO_MINDMAP_CACHE_MIB")) {
        const long value = std::strtol(env, nullptr, 10);
        if (value > 0) return static_cast<std::uint64_t>(value) * 1024ull * 1024ull;
    }
    return kDefaultMindmapActiveBudgetMiB * 1024ull * 1024ull;
}

struct CacheKey {
    // ── 文档身份与正文代次（预览打开时的锚）──────────────────────────────────
    std::uint64_t tabId = 0;
    std::string path;
    unsigned long long revision = 0;
    unsigned long long textRevision = 0;
    // ── 目标围栏块（lp 区间 + 语言；selectTarget 只认闭合顶级围栏）────────────
    std::size_t blockBeg = 0;
    std::size_t blockEnd = 0;
    std::string codeLang;
    // ── 影响测字 / 布局 / 配色的样式维度 ────────────────────────────────────
    std::string editorFontFamily;
    std::string codeFontFamily;
    float fontSize = 0.0f;
    float effectiveScale = 0.0f;
    float panelWidth = 0.0f;
    int theme = 0;
    std::string themeFile;
    unsigned long long themeVersion = 0;
    std::uint64_t cacheVersion = kMindmapCacheVersion;

    friend bool operator==(const CacheKey& a, const CacheKey& b) {
        return a.tabId == b.tabId && a.path == b.path && a.revision == b.revision &&
               a.textRevision == b.textRevision && a.blockBeg == b.blockBeg &&
               a.blockEnd == b.blockEnd && a.codeLang == b.codeLang &&
               a.editorFontFamily == b.editorFontFamily &&
               a.codeFontFamily == b.codeFontFamily && a.fontSize == b.fontSize &&
               a.effectiveScale == b.effectiveScale && a.panelWidth == b.panelWidth &&
               a.theme == b.theme && a.themeFile == b.themeFile &&
               a.themeVersion == b.themeVersion && a.cacheVersion == b.cacheVersion;
    }
    friend bool operator!=(const CacheKey& a, const CacheKey& b) { return !(a == b); }
};

struct Cache {
    CacheKey key;
    Plan plan;                  // 解析树（唯一一份；冷路径 move 进来）
    std::string source;         // 块切片快照（锚点重校验；≤ Limits::sourceBytes）
    std::vector<Size> labelSizes;
    Layout layout;
    bool valid = false;         // plan + labelSizes + layout 成套可用
    std::size_t bytes = 0;      // estimateCacheBytes 快照；预算 / LRU 计账用
    unsigned long long generation = 0;  // 冷重建次数（诊断 / 测试观察，不参与键）

    // 全量释放：swap 归零容量（同 document_tabs 的 releaseDerivedCaches 口径，
    // 避免 clear 只留高水位）。generation 单调不归零。
    void release() {
        key = CacheKey{};
        plan = Plan{};
        std::string{}.swap(source);
        std::vector<Size>{}.swap(labelSizes);
        layout = Layout{};
        valid = false;
        bytes = 0;
    }
};

// 驻留字节估算：sizeof(Cache) 一次 + 全部动态容量（字符串 capacity、节点树、
// 标签尺寸、布局节点与连线）。与 lpPlanBytes 同一"深入一层"的计账口径。
inline std::size_t estimateCacheBytes(const Cache& cache) {
    const auto stringBytes = [](const std::string& value) -> std::uint64_t {
        return static_cast<std::uint64_t>(value.capacity());
    };
    std::uint64_t bytes = sizeof(Cache);
    bytes += stringBytes(cache.key.path) + stringBytes(cache.key.codeLang) +
             stringBytes(cache.key.editorFontFamily) + stringBytes(cache.key.codeFontFamily) +
             stringBytes(cache.key.themeFile) + stringBytes(cache.source);
    bytes += static_cast<std::uint64_t>(cache.plan.nodes.capacity()) * sizeof(Node);
    for (const Node& node : cache.plan.nodes) {
        bytes += stringBytes(node.label) + stringBytes(node.id);
        bytes += static_cast<std::uint64_t>(node.children.capacity()) * sizeof(int);
    }
    bytes += static_cast<std::uint64_t>(cache.labelSizes.capacity()) * sizeof(Size);
    bytes += static_cast<std::uint64_t>(cache.layout.nodes.capacity()) * sizeof(LayoutNode);
    bytes += static_cast<std::uint64_t>(cache.layout.edges.capacity()) * sizeof(Edge);
    bytes += stringBytes(cache.layout.rejectReason);
    return static_cast<std::size_t>(bytes);
}

} // namespace neo::mindmap
