#pragma once

#include "app_state.h"
#include "model/mindmap_cache.h"

#include <limits>
#include <string_view>
#include <utility>

namespace neo {

// ── 只读思维导图预览的模型层（实施计划阶段 C/D）────────────────────────────────
// 只读性硬契约：打开 / 视口 / 关闭 / 各类拒绝都不得改动 doc.text、revision、
// savedRevision、撤销重做历史、textRevision、折叠态与 dirty()。解析树 move 进
// 页自有 mindmap::Cache，PreviewState 不持有第二份树。

namespace mindmap_detail {

// 锚点重校验：用缓存记录的块起始偏移重新 selectTarget，区间与字节都必须与缓存
// 快照一致。防"同 revision 的外部等长回写把顶级围栏变列表续行"这类廉价判据漏网。
// 不要求 cache.valid：冷打开后布局未建（valid=false）但 plan/source 已就位，
// "模型存在"与"布局就绪"是两个守卫（后者是 mindmapCacheCurrent）。
inline bool mindmapBlockAnchorCurrent(const mindmap::Cache& cache, const AppState& state,
                                      const lp::PlanCache& lp) {
    // 源码视图不建 LP cache（editor_view 早退）：残留的上一帧缓存不可信，一律失效。
    if (!state.markdownCapable() || state.sourceView() || !lp.valid ||
        lp.text != state.doc.text) {
        return false;
    }
    const std::size_t beg = cache.plan.sourceBeg;
    const std::size_t end = cache.plan.sourceEnd;
    if (beg >= end || end > state.doc.text.size() ||
        state.doc.text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const auto target = mindmap::selectTarget(state.doc.text, lp.plan, static_cast<int>(beg));
    return target && static_cast<std::size_t>(target.blockBeg) == beg &&
           static_cast<std::size_t>(target.blockEnd) == end &&
           std::string_view(state.doc.text.data() + beg, end - beg) ==
               std::string_view(cache.source);
}

} // namespace mindmap_detail

// ── 打开（无 UI、无测字，可单测）──────────────────────────────────────────────
// 守卫（markdown / 计划就绪 / 输入同源）→ **先按当前光标 selectTarget**（绝不
// 直接信缓存的旧块：同文档换块重开必须显示光标所在的块）→ 暖命中（键身份 + 目标
// 块区间一致 + 锚点重校验，不重解析）→ 冷路径：parseBlock，解析结果 move 进
// cache。解析失败不留半套结果（parsed 是局部量）；既有页缓存保留——它是键控的
// （key 与 preview 身份、块区间必须逐项一致才可能被展示），光标移回旧块还能暖开。
inline mindmap::RejectReason openMindmapModel(
    mindmap::PreviewState& preview, mindmap::Cache& cache, const AppState& state,
    const components::input_detail::InputModel::InputState& input,
    const lp::PlanCache& lp) {
    preview.close();
    const auto reject = [&preview](mindmap::RejectReason reason) {
        preview.rejectReason = reason;
        return reason;
    };
    // 源码视图（.c/.json 等语法模式）不建 LP cache：必须先于语言检查拒绝——
    // 否则 sourceView() && markdownCapable()==false 的组合里残留缓存可能误命中。
    if (state.sourceView()) return reject(mindmap::RejectReason::PlanNotReady);
    if (!state.markdownCapable()) return reject(mindmap::RejectReason::NotMarkdown);
    if (!lp.valid || lp.text != state.doc.text || input.text != state.doc.text)
        return reject(mindmap::RejectReason::PlanNotReady);

    const auto target = mindmap::selectTarget(state.doc.text, lp.plan, input.cursor);
    if (!target) return reject(target.rejectReason);
    if (target.blockBeg < 0 || target.blockEnd <= target.blockBeg ||
        static_cast<std::size_t>(target.blockEnd) > state.doc.text.size())
        return reject(mindmap::RejectReason::InvalidFence);

    const bool warm = cache.key.tabId == state.tabId && cache.key.path == state.path &&
                      cache.key.revision == state.revision &&
                      cache.key.textRevision == input.textRevision &&
                      cache.key.blockBeg == static_cast<std::size_t>(target.blockBeg) &&
                      cache.key.blockEnd == static_cast<std::size_t>(target.blockEnd) &&
                      mindmap_detail::mindmapBlockAnchorCurrent(cache, state, lp);
    if (warm) {
        preview.tabId = state.tabId;
        preview.path = state.path;
        preview.revision = state.revision;
        preview.textRevision = input.textRevision;
        preview.rejectReason = mindmap::RejectReason::None;
        preview.open = true;
        return mindmap::RejectReason::None;
    }

    const std::string_view source(state.doc.text.data() + target.blockBeg,
                                  static_cast<std::size_t>(target.blockEnd - target.blockBeg));
    auto parsed = mindmap::parseBlock(source, static_cast<std::size_t>(target.blockBeg));
    if (!parsed) return reject(parsed.rejectReason);

    cache.release();
    cache.plan = std::move(parsed.plan);
    cache.source.assign(source);
    cache.key = mindmap::CacheKey{};
    cache.key.tabId = state.tabId;
    cache.key.path = state.path;
    cache.key.revision = state.revision;
    cache.key.textRevision = input.textRevision;
    cache.key.blockBeg = cache.plan.sourceBeg;
    cache.key.blockEnd = cache.plan.sourceEnd;
    cache.key.codeLang = "mermaid";
    cache.valid = false;  // 布局尚未建；storeMindmapCache 成功后才置真
    cache.bytes = 0;
    ++cache.generation;
    preview.tabId = state.tabId;
    preview.path = state.path;
    preview.revision = state.revision;
    preview.textRevision = input.textRevision;
    preview.open = true;
    return mindmap::RejectReason::None;
}

// ── 当前性守卫（每帧 compose 调用；外部回写一律失效）──────────────────────────
// 只认"打开时固定的那个块"（开块固定）：不查 input.cursor，光标移动不失效。
// 不要求 cache.valid：冷打开当帧布局未建也必须放行，由 mindmapCacheCurrent
// 决定是否重建布局；否则冷开永远活不到布局那一步。
inline bool mindmapModelCurrent(const mindmap::PreviewState& preview, const mindmap::Cache& cache,
                                const AppState& state,
                                const components::input_detail::InputModel::InputState& input,
                                const lp::PlanCache& lp) {
    if (!preview.open) return false;
    if (preview.tabId != state.tabId || preview.path != state.path ||
        preview.revision != state.revision || preview.textRevision != input.textRevision ||
        input.text != state.doc.text) {
        return false;
    }
    if (cache.key.tabId != preview.tabId || cache.key.path != preview.path ||
        cache.key.revision != preview.revision ||
        cache.key.textRevision != preview.textRevision ||
        cache.key.blockBeg != cache.plan.sourceBeg || cache.key.blockEnd != cache.plan.sourceEnd ||
        cache.key.codeLang != "mermaid") {
        return false;
    }
    return mindmap_detail::mindmapBlockAnchorCurrent(cache, state, lp);
}

// ── 样式键（UI 层每帧组装；字符串用 view 避免拷贝）────────────────────────────
struct MindmapStyleKey {
    std::string_view editorFontFamily;
    std::string_view codeFontFamily;
    std::string_view themeFile;
    float fontSize = 0.0f;
    float effectiveScale = 1.0f;
    float panelWidth = 0.0f;
    int theme = 0;
    unsigned long long themeVersion = 0;
};

// 缓存当前性（= 布局就绪 + 键全一致）：预览打开、结果成套（valid），且字体/字号/
// DPI/面板宽度/主题/主题文件任一变化 → false（上层只重测重建，不重解析 Markdown）。
inline bool mindmapCacheCurrent(const mindmap::Cache& cache, const mindmap::PreviewState& preview,
                                const MindmapStyleKey& style) {
    if (!preview.open || !cache.valid) return false;
    if (cache.layout.nodes.size() != cache.plan.nodes.size()) return false;
    const auto& key = cache.key;
    return key.cacheVersion == mindmap::kMindmapCacheVersion &&
           key.tabId == preview.tabId && key.path == preview.path &&
           key.revision == preview.revision && key.textRevision == preview.textRevision &&
           key.blockBeg == cache.plan.sourceBeg && key.blockEnd == cache.plan.sourceEnd &&
           key.codeLang == "mermaid" &&
           key.editorFontFamily == std::string_view(style.editorFontFamily) &&
           key.codeFontFamily == std::string_view(style.codeFontFamily) &&
           key.themeFile == std::string_view(style.themeFile) &&
           key.fontSize == style.fontSize && key.effectiveScale == style.effectiveScale &&
           key.panelWidth == style.panelWidth && key.theme == style.theme &&
           key.themeVersion == style.themeVersion;
}

// ── 入库（UI 层测好标签尺寸并 buildLayout 之后调用）──────────────────────────
// 尺寸与布局 move 进缓存（不留第二份）；键补全样式维度；估算驻留字节并对照
// 活动预算（mindmapActiveBudgetBytes）—— 超限整体释放并返回 false，回退保留原文。
inline bool storeMindmapCache(mindmap::Cache& cache, const mindmap::PreviewState& preview,
                              const MindmapStyleKey& style,
                              std::vector<mindmap::Size>&& labelSizes,
                              mindmap::Layout&& layout) {
    if (!preview.open || !layout.valid ||
        labelSizes.size() != cache.plan.nodes.size() || cache.plan.nodes.empty()) {
        cache.release();
        return false;
    }
    cache.key.editorFontFamily.assign(style.editorFontFamily);
    cache.key.codeFontFamily.assign(style.codeFontFamily);
    cache.key.themeFile.assign(style.themeFile);
    cache.key.fontSize = style.fontSize;
    cache.key.effectiveScale = style.effectiveScale;
    cache.key.panelWidth = style.panelWidth;
    cache.key.theme = style.theme;
    cache.key.themeVersion = style.themeVersion;
    cache.key.cacheVersion = mindmap::kMindmapCacheVersion;
    cache.labelSizes = std::move(labelSizes);
    cache.layout = std::move(layout);
    cache.valid = true;
    cache.bytes = mindmap::estimateCacheBytes(cache);
    if (cache.bytes > mindmap::mindmapActiveBudgetBytes()) {
        cache.release();
        return false;
    }
    return true;
}

} // namespace neo
