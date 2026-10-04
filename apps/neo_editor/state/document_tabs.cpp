#include "state/app_actions.h"
#include "model/session_storage.h"
#include "model/tab_presentation.h"
#include "state/file_check.h"
#include "state/session_writer.h"
#include "state/tabs_trace.h"
#include "state/vault_cache.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo {
namespace {
bool equalComponent(const std::filesystem::path& a, const std::filesystem::path& b) {
#if defined(_WIN32)
    const auto& x = a.native(); const auto& y = b.native();
    return CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
std::filesystem::path normalizedPath(const std::string& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(textfile::pathFromUtf8(path), error);
    if (error) result = std::filesystem::absolute(textfile::pathFromUtf8(path), error).lexically_normal();
    return result;
}

// ── 派生缓存上下文（执行计划阶段 C）────────────────────────────────────────
// 三张全局单例缓存（LP 计划、装饰表、源码高亮）只服务活动页。切页时把全局缓存 move
// 进离场页的槽位、把入场页的槽位 move 回全局 —— 每页各自保有 plan/prevPlan/version、
// 装饰快照与源码行，暖切不再整篇重解析/重排，同时天然杜绝跨页误命中（两者永远成套
// 搬迁，不会出现 A 的计划配 B 的装饰表）。
//
// 这些槽位是可重建的派生缓存：受字节/条目预算约束、可被 LRU 淘汰；正文、撤销栈、
// 光标/选区/滚动等**编辑状态**留在 doc 与 editorMemory，绝不因预算被清。
constexpr std::uint64_t kDefaultDerivedBudgetMiB = 64;
constexpr std::size_t kDefaultDerivedPageLimit = 2;

std::uint64_t derivedBudgetBytes() {
    if (const char* env = std::getenv("NEO_TAB_CACHE_BUDGET_MIB")) {
        const long value = std::strtol(env, nullptr, 10);
        if (value > 0) return static_cast<std::uint64_t>(value) * 1024ull * 1024ull;
    }
    return kDefaultDerivedBudgetMiB * 1024ull * 1024ull;
}

std::size_t derivedPageLimit() {
    if (const char* env = std::getenv("NEO_TAB_CACHE_PAGES")) {
        const long value = std::strtol(env, nullptr, 10);
        if (value > 0) return static_cast<std::size_t>(value);
    }
    return kDefaultDerivedPageLimit;
}

// 逐项估算驻留字节：外层容器的 sizeof 远远不够 —— 每行挂着的 spans/holes/runs/cells
// 才是大头。计账必须深入一层，否则大文稿会被误判成"预算内"而常驻。
std::uint64_t lpLineBytes(const LpLine& line) {
    // LpLine objects are already covered by the containing vector's capacity.
    std::uint64_t bytes = 0;
    bytes += line.codeLang.capacity() + line.pureImageSrc.capacity();
    bytes += static_cast<std::uint64_t>(line.conceal.capacity()) * sizeof(LpRange);
    bytes += static_cast<std::uint64_t>(line.spans.capacity()) * sizeof(LpSpan);
    bytes += static_cast<std::uint64_t>(line.cells.capacity()) * sizeof(LpRange);
    bytes += static_cast<std::uint64_t>(line.cellAligns.capacity());
    return bytes;
}

std::uint64_t lpPlanBytes(const LpPlan& plan) {
    // LpPlan itself is inline in PlanCache; only its dynamic line storage is additional.
    std::uint64_t bytes = static_cast<std::uint64_t>(plan.lines.capacity()) * sizeof(LpLine);
    for (const LpLine& line : plan.lines) {
        bytes += lpLineBytes(line);
    }
    return bytes;
}

std::uint64_t textLineBytes(const components::input_detail::InputModel::TextLine& line) {
    // TextLine objects are included once by cachedLines.capacity() below.
    std::uint64_t bytes = 0;
    bytes += line.imagePath.capacity() + line.imageFailText.capacity() +
             line.fontFamily.capacity() + line.languageLabel.capacity() +
             line.glyph.text.capacity() + line.gutterGlyph.text.capacity();
    bytes += static_cast<std::uint64_t>(line.holes.capacity()) * sizeof(components::input_detail::LineHole);
    bytes += static_cast<std::uint64_t>(line.runs.capacity()) * sizeof(components::input_detail::TextRun);
    for (const auto& run : line.runs) bytes += run.style.fontFamily.capacity();
    // 逐字度量（byteIndices/caretX）是排版缓存里最大的一块，绝不能漏计。
    bytes += static_cast<std::uint64_t>(line.metrics.byteIndices.capacity()) * sizeof(int);
    bytes += static_cast<std::uint64_t>(line.metrics.caretX.capacity()) * sizeof(float);
    return bytes;
}

std::uint64_t decorationRowDynamicBytes(const components::input_detail::LineDecoration& row) {
    std::uint64_t bytes = row.fontFamily.capacity() + row.imagePath.capacity() +
                          row.imageFailText.capacity() + row.languageLabel.capacity() +
                          row.glyph.text.capacity() + row.gutterGlyph.text.capacity();
    bytes += static_cast<std::uint64_t>(row.holes.capacity()) * sizeof(components::input_detail::LineHole);
    bytes += static_cast<std::uint64_t>(row.runs.capacity()) * sizeof(components::input_detail::LineRun);
    bytes += static_cast<std::uint64_t>(row.cells.capacity()) * sizeof(components::input_detail::LineCell);
    for (const components::input_detail::LineRun& run : row.runs) {
        bytes += run.style.fontFamily.capacity();
    }
    return bytes;
}

std::uint64_t decorationRowBytes(const components::input_detail::LineDecoration& row) {
    return sizeof(row) + decorationRowDynamicBytes(row);
}

std::uint64_t decorationTableBytes(const components::input_detail::LineDecorationTable& table) {
    // Uniform pages own one row. Count allocated storage, not logical row count.
    return table.residentCapacityBytes();
}

std::uint64_t sourceCacheBytes(const source::Cache& cache) {
    // cache.bytes is the indexed source-text length, not an owned allocation. Count
    // the actual row strings below instead of charging the same text length again.
    std::uint64_t bytes = sizeof(source::Cache);
    bytes += static_cast<std::uint64_t>(cache.starts.capacity()) * sizeof(int);
    bytes += static_cast<std::uint64_t>(cache.rows.capacity()) * sizeof(source::Row);
    for (const source::Row& row : cache.rows) {
        bytes += row.text.capacity();
        bytes += static_cast<std::uint64_t>(row.tokens.capacity()) * sizeof(lp::SyntaxToken);
    }
    return bytes;
}

// 派生缓存驻留字节（计划/装饰/源码三块）。计划含当前代 + 上一代（增量证明用）。
std::uint64_t estimateDerivedBytes(const DocumentSession& page) {
    std::uint64_t bytes = 0;
    if (page.derivedPlan) {
        const lp::PlanCache& plan = *page.derivedPlan;
        bytes += sizeof(lp::PlanCache) + plan.text.capacity();
        bytes += lpPlanBytes(plan.plan) + lpPlanBytes(plan.prevPlan);
    }
    if (page.derivedDecoration) {
        bytes += sizeof(lp::DecorationCache);
        bytes += page.derivedDecoration->fontFamily.capacity() +
                 page.derivedDecoration->codeFontFamily.capacity() +
                 page.derivedDecoration->docDir.capacity() +
                 page.derivedDecoration->foldKey.capacity();
        bytes += static_cast<std::uint64_t>(page.derivedDecoration->cursorBoundaries.capacity()) * sizeof(int);
        bytes += static_cast<std::uint64_t>(page.derivedDecoration->cursorSpans.capacity()) * sizeof(lp::CursorSpanDependency);
        bytes += static_cast<std::uint64_t>(page.derivedDecoration->cursorFolds.capacity()) * sizeof(lp::CursorFoldSeed);
        bytes += static_cast<std::uint64_t>(page.derivedDecoration->legacyRows.capacity()) *
                 sizeof(components::input_detail::LineDecoration);
        for (const auto& row : page.derivedDecoration->legacyRows) {
            bytes += decorationRowDynamicBytes(row);
        }
        if (page.derivedDecoration->snapshot) {
            bytes += decorationTableBytes(*page.derivedDecoration->snapshot);
        }
    }
    if (page.derivedSource) {
        bytes += sourceCacheBytes(*page.derivedSource);
    }
    return bytes;
}

// 输入组件（editorMemory）的排版/几何/装饰快照字节。单独一项：它在换页后的
// syncDocumentTabInputs 里才挂到页上，捕获那一刻还算不到。
std::uint64_t estimateInputBytes(const components::input_detail::InputModel::InputState& input,
                                 const lp::DecorationCache* sharedDecoration) {
    std::uint64_t bytes = sizeof(input);
    for (const auto& line : input.cachedLines) {
        bytes += textLineBytes(line);
    }
    bytes += static_cast<std::uint64_t>(input.cachedLines.capacity()) * sizeof(components::input_detail::InputModel::TextLine);
    bytes += static_cast<std::uint64_t>(input.cachedTables.capacity()) * sizeof(components::input_detail::InputModel::TableColumns);
    bytes += static_cast<std::uint64_t>(input.cachedTableIntrinsic.capacity()) * sizeof(components::input_detail::InputModel::TableColumns);
    const auto tableColumnsBytes = [](const components::input_detail::InputModel::TableColumns& table) {
        return (static_cast<std::uint64_t>(table.x.capacity()) +
                static_cast<std::uint64_t>(table.width.capacity())) * sizeof(float);
    };
    for (const auto& table : input.cachedTables) bytes += tableColumnsBytes(table);
    for (const auto& table : input.cachedTableIntrinsic) bytes += tableColumnsBytes(table);
    bytes += input.cachedGeometry.residentCapacityBytes();
    // The index object itself is part of sizeof(InputState); account its bucket array
    // and an approximate per-node allocation. Standard-library node overhead varies.
    using TableIndex = components::input_detail::InputModel::TableColumnIndex;
    bytes += static_cast<std::uint64_t>(input.cachedTableIndex.bucket_count()) * sizeof(void*);
    bytes += static_cast<std::uint64_t>(input.cachedTableIndex.size()) *
             (sizeof(TableIndex::value_type) + 2 * sizeof(void*));
    bytes += input.cachedLayoutText.capacity();
    bytes += input.cachedFontFamily.capacity();
    bytes += static_cast<std::uint64_t>(input.cachedMetrics.byteIndices.capacity()) * sizeof(int);
    bytes += static_cast<std::uint64_t>(input.cachedMetrics.caretX.capacity()) * sizeof(float);
    bytes += static_cast<std::uint64_t>(input.detailedRows.capacity()) * sizeof(std::size_t);
    // Legacy vector providers own a separate row vector; count inline rows once and
    // then their nested allocations. Snapshot generations are accounted separately.
    bytes += static_cast<std::uint64_t>(input.decorations.capacity()) *
             sizeof(components::input_detail::LineDecoration);
    for (const auto& row : input.decorations) bytes += decorationRowDynamicBytes(row);
    // The app DecorationCache and InputState commonly share this immutable snapshot.
    // It is counted by estimateDerivedBytes when identities match; avoid charging its
    // payload twice. If the input owns a distinct generation, include that generation.
    const bool sharedWithDerived = sharedDecoration && input.decorationSnapshot &&
                                   input.decorationSnapshot == sharedDecoration->snapshot;
    if (input.decorationSnapshot && !sharedWithDerived) {
        bytes += decorationTableBytes(*input.decorationSnapshot);
    }
    return bytes;
}

// 把全局缓存搬进这一页的槽位（离场时调用）。搬完全局复位为空，供入场页安装或重建。
void captureDerivedCaches(DocumentSession& page) {
    page.derivedPlan = std::make_unique<lp::PlanCache>(std::move(lp::planCache()));
    page.derivedDecoration = std::make_unique<lp::DecorationCache>(std::move(lp::decorationCache()));
    page.derivedSource = std::make_unique<source::Cache>(std::move(source::cache()));
    lp::planCache() = lp::PlanCache{};
    lp::decorationCache() = lp::DecorationCache{};
    source::cache() = source::Cache{};
    page.derivedCacheBytes = estimateDerivedBytes(page);
    page.derivedCacheUse = tracelog::nowMicros() != 0 ? tracelog::nowMicros() : 1;
    tracelog::event("cache-save", 0, "bytes=" + std::to_string(page.derivedCacheBytes));
}

// 把这一页的槽位搬回全局（入场时调用）；没有槽位则复位全局（该页冷启，下一步重建）。
void installDerivedCaches(DocumentSession& page) {
    if (page.derivedPlan) {
        lp::planCache() = std::move(*page.derivedPlan);
        page.derivedPlan.reset();
    } else {
        lp::invalidatePlanCache();
    }
    if (page.derivedDecoration) {
        lp::decorationCache() = std::move(*page.derivedDecoration);
        page.derivedDecoration.reset();
    } else {
        lp::invalidateDecorationCache();
    }
    if (page.derivedSource) {
        source::cache() = std::move(*page.derivedSource);
        page.derivedSource.reset();
    } else {
        source::invalidate();
    }
    page.derivedCacheBytes = 0;
    page.derivedCacheUse = 0;
}

// 丢弃这一页的派生缓存（LRU 淘汰 / 正文被替换时）。用 swap 释放 capacity，避免只 clear
// 留下高水位；编辑状态（doc / editorMemory 的撤销栈与光标）原样保留。
void releaseDerivedCaches(DocumentSession& page) {
    page.derivedPlan.reset();
    page.derivedDecoration.reset();
    page.derivedSource.reset();
    page.derivedCacheBytes = 0;
    page.derivedCacheUse = 0;
    tracelog::event("cache-evict", 0, "");
    if (page.editorMemory) {
        auto& input = *page.editorMemory;
        // 先逐行释放逐字度量，再 swap 掉行容器（顺序反了就只能释放外层）。
        for (auto& line : input.cachedLines) {
            std::vector<int>{}.swap(line.metrics.byteIndices);
            std::vector<float>{}.swap(line.metrics.caretX);
        }
        std::vector<components::input_detail::InputModel::TextLine>{}.swap(input.cachedLines);
        std::vector<components::input_detail::InputModel::TableColumns>{}.swap(input.cachedTables);
        std::vector<components::input_detail::InputModel::TableColumns>{}.swap(input.cachedTableIntrinsic);
        components::input_detail::InputModel::TableColumnIndex{}.swap(input.cachedTableIndex);
        std::vector<std::size_t>{}.swap(input.detailedRows);
        input.detailTrackingValid = false;
        std::string{}.swap(input.cachedLayoutText);
        input.cachedGeometry.release();
        input.layoutCacheValid = false;
        input.cachedTextRevision = static_cast<unsigned long long>(-1);
        input.decorationSnapshot.reset();
        ++input.decorationRevision;
    }
}

// 预算修剪：非活动页派生缓存超过字节/条目上限时淘汰最久未用的一页。
void enforceDerivedBudget(AppState& state) {
    const std::uint64_t budget = derivedBudgetBytes();
    const std::size_t pageLimit = derivedPageLimit();
    for (;;) {
        // 用捕获时算好的字节数（derivedCacheBytes）：每帧重算会在大文稿上白付
        // 一次 O(行数) 的全量遍历。
        std::uint64_t total = 0;
        std::size_t holders = 0;
        for (const auto& pair : state.inactiveTabs) {
            if (pair.second.derivedCacheUse == 0) continue;
            total += pair.second.derivedCacheBytes;
            ++holders;
        }
        if (total <= budget && holders <= pageLimit) {
            return;
        }
        DocumentSession* victim = nullptr;
        for (auto& pair : state.inactiveTabs) {
            DocumentSession& page = pair.second;
            if (page.derivedCacheUse == 0) continue;
            if (victim == nullptr || page.derivedCacheUse < victim->derivedCacheUse) {
                victim = &page;
            }
        }
        if (victim == nullptr) {
            return;
        }
        releaseDerivedCaches(*victim);
    }
}

void suspendActiveDocument(AppState& state) {
    auto& document = static_cast<DocumentSession&>(state);
    // 后台页保留编辑状态、库视图投影与派生缓存（预算内的最近使用页），不再每次清空。
    // 真正需要释放时由 enforceDerivedBudget 按 LRU 淘汰。
    state.inactiveTabs.insert_or_assign(document.tabId, std::move(document));
    enforceDerivedBudget(state);
}

void afterSwitch(AppState& state) {
    core::window::cancelImeComposition(core::window::mainWindowHandle());
    state.pendingEditorCommand = EditorCommand::None;
    state.pendingTaskByte = -1; state.pendingImagePaste = false; state.pendingImageLink.clear();
    state.contextMenuOpen = false; state.vaultContextMenuOpen = false;
    state.languageMenuOpen = false; state.openMenu = MenuKind::None;
    state.findEditorFocusPending = true; state.tabListReveal = state.tabId;
    {
        tracelog::Span merge("merge-roots");
        mergeRelatedVaultRoots(state);
    }
    // 关键路径上不再同步整库扫描、也不再同步读盘核验、更不清全局 LP/source 缓存：
    //   · 库扫描按根共享，命中快照直接复用，未命中才排后台扫描（阶段 B）；
    //   · 干净页的外部变化交给后台核验，不再阻塞切页（阶段 D）；
    //   · 派生缓存随页搬迁，暖切直接复用（阶段 C）。
    {
        tracelog::Span adopt("scan-adopt");
        adoptVaultScan(state, false);
    }
    {
        tracelog::Span verify("verify-sched");
        scheduleActiveFileVerification(state);
    }
    // 根的更新入口之一：切页/建页/关页（含其内部 merge）之后重解算展示分组。
    refreshTabPresentation(state);
    settings::current().lastFile = state.path;
    app::requestUpdate();
}
// 离场页的输入控件：只清**临时状态**（IME 合成/预编辑、指针拖选/悬停/链接按下、
// 待消费编辑区间）。排版/几何/装饰快照属于可复用派生缓存，随页保留（阶段 C），
// 不再像旧实现那样每次切页清光。
void retainEditingState(components::input_detail::InputModel::InputState& input) {
    input.compositionText.clear(); input.preedit.reset(); ++input.compositionRevision;
    input.selecting = false; input.pointerHoverValid = false; input.pressedLinkByte = -1;
    input.pendingEdit = components::input_detail::PendingTextEdit{};
}
}


DocumentSession* documentTab(AppState& state, std::uint64_t id) {
    if (state.tabId == id) return &state;
    const auto found = state.inactiveTabs.find(id);
    return found == state.inactiveTabs.end() ? nullptr : &found->second;
}
const DocumentSession* documentTab(const AppState& state, std::uint64_t id) {
    if (state.tabId == id) return &state;
    const auto found = state.inactiveTabs.find(id);
    return found == state.inactiveTabs.end() ? nullptr : &found->second;
}
bool sameDocumentFile(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty()) return false;
    std::error_code error;
    if (std::filesystem::equivalent(textfile::pathFromUtf8(left), textfile::pathFromUtf8(right), error) && !error) return true;
    const auto a = normalizedPath(left), b = normalizedPath(right);
    auto x = a.begin(); auto y = b.begin();
    for (; x != a.end() && y != b.end(); ++x, ++y) if (!equalComponent(*x, *y)) return false;
    return x == a.end() && y == b.end();
}
bool documentWithinRoot(const std::string& path, const std::string& root) {
    if (path.empty() || root.empty()) return false;
    const auto a = normalizedPath(path), b = normalizedPath(root);
    auto x = a.begin();
    for (const auto& part : b) {
        if (x == a.end() || !equalComponent(*x, part)) return false;
        ++x;
    }
    return true;
}
bool mergeRelatedVaultRoots(AppState& state) {
    // 每次切页都会走这里：下面按"每个标签的根"两两比较（O(n²) 次 weakly_canonical，
    // 10 标签约 200 次、实测 ~7.8ms）。根集合没变时结果必为 no-op，用签名短路掉。
    const auto rootSignature = [&state]() {
        std::vector<std::string> signature;
        signature.reserve(state.tabOrder.size());
        for (const auto id : state.tabOrder) if (const auto* document = documentTab(state, id))
            if (!document->vaultRoot.empty()) signature.push_back(std::to_string(id) + "=" + document->vaultRoot);
        std::sort(signature.begin(), signature.end());
        return signature;
    };
    if (rootSignature() == state.mergedVaultRootSignature) return false;

    bool activeChanged = false;
    // Only already-open containing roots participate; never invent a drive-wide common root.
    std::vector<std::string> roots;
    for (const auto id : state.tabOrder) if (const auto* document = documentTab(state, id))
        if (!document->vaultRoot.empty()) roots.push_back(document->vaultRoot);
    for (const auto id : state.tabOrder) {
        auto* document = documentTab(state, id);
        if (!document || document->vaultRoot.empty()) continue;
        auto root = document->vaultRoot;
        for (const auto& candidate : roots) if (documentWithinRoot(root, candidate)) root = candidate;
        if (sameDocumentFile(root, document->vaultRoot)) continue;
        std::error_code error;
        auto prefix = textfile::pathToUtf8(std::filesystem::relative(textfile::pathFromUtf8(document->vaultRoot), textfile::pathFromUtf8(root), error));
        std::replace(prefix.begin(), prefix.end(), '\\', '/');
        const auto rebase = [&](const std::string& relative) {
            return relative.empty() || error ? relative : prefix + "/" + relative;
        };
        std::set<std::string> expanded;
        if (!error && prefix != ".") expanded.insert(prefix);
        for (const auto& entry : document->expanded) expanded.insert(rebase(entry));
        document->expanded = std::move(expanded);
        document->vaultSelectedPath = rebase(document->vaultSelectedPath);
        document->vaultRowFocusedPath.clear(); document->vaultFocusRestorePath.clear();
        document->vaultPendingReveal = rebase(document->vaultPendingReveal);
        document->vaultRoot = root; document->vaultAddress = root;
        document->vaultScan.reset(); document->rows.clear(); document->vaultRowsGeneration = 0;
        activeChanged = activeChanged || id == state.tabId;
    }
    // 记录合并后的签名（根已收敛），下一次调用即可稳定短路。
    state.mergedVaultRootSignature = rootSignature();
    return activeChanged;
}
// 标签展示分组刷新（2026-10-03 视觉改进批次）。只在**动作阶段**（切页/建页/关页/打开/
// 另存跟随/选择库/重命名/merge 之后）调用；轻量签名不变时立即返回，不做 canonical、
// 不重建 map。规范化按"原始根文本"缓存，组内多页共用一次 canonical。
void refreshTabPresentation(AppState& state) {
    std::vector<std::string> signature;
    signature.reserve(state.tabOrder.size());
    std::unordered_map<std::string, bool> activeRawRoots;
    for (const auto id : state.tabOrder) {
        const auto* document = documentTab(state, id);
        if (!document) continue;
        signature.push_back(std::to_string(id) + "=" + document->vaultRoot);
        if (!document->vaultRoot.empty()) activeRawRoots.emplace(document->vaultRoot, true);
    }
    // 展示顺序不改变根集合；重排不能触发分组表重建。
    std::sort(signature.begin(), signature.end());
    if (signature == state.tabPresentationSignature) return;
    state.tabPresentationSignature = std::move(signature);

    std::vector<tabpresentation::Registry::OpenTab> openTabs;
    std::unordered_map<std::uint64_t, std::string> keyByTab;
    openTabs.reserve(state.tabOrder.size());
    keyByTab.reserve(state.tabOrder.size());
    for (const auto id : state.tabOrder) {
        const auto* document = documentTab(state, id);
        if (!document || document->vaultRoot.empty()) continue;
        auto cached = state.tabCanonicalRootCache.find(document->vaultRoot);
        if (cached == state.tabCanonicalRootCache.end()) {
            cached = state.tabCanonicalRootCache
                         .emplace(document->vaultRoot, tabpresentation::canonicalRootKey(document->vaultRoot))
                         .first;
        }
        openTabs.push_back({id, cached->second});
        keyByTab.emplace(id, cached->second);
    }
    state.tabColorRegistry.sync(openTabs, state.tabPresentationKeyByTab);

    state.tabPresentationSlot.clear();
    state.tabPresentationOrdinal.clear();
    for (const auto& entry : keyByTab) {
        const auto group = state.tabColorRegistry.state(entry.second);
        state.tabPresentationSlot.emplace(entry.first, group.slot);
        state.tabPresentationOrdinal.emplace(entry.first, group.ordinal);
    }
    // 保留本轮 TabId→key 供下一次合并继承（关闭的 ID 立即消失）；辅助缓存只留无主条目之外
    // 仍在用的原始根文本，避免访问过的目录无限堆积。
    state.tabPresentationKeyByTab = keyByTab;
    for (auto it = state.tabCanonicalRootCache.begin(); it != state.tabCanonicalRootCache.end();) {
        if (activeRawRoots.find(it->first) == activeRawRoots.end()) it = state.tabCanonicalRootCache.erase(it);
        else ++it;
    }
}

// 有效缩放：GetDpiForWindow/96 × ui_scale（与 framework 的 effectiveScale 口径一致）。
// GetDpiForWindow 是 1607+ API，动态取，避免旧 SDK 链接问题（与 native_dialogs 同做法）。
float effectiveWindowScale() {
    float dpiScale = 1.0f;
#if defined(_WIN32)
    HWND window = static_cast<HWND>(core::window::mainWindowHandle());
    if (window != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
            if (getDpi != nullptr) {
                const UINT dpi = getDpi(window);
                if (dpi >= 96) dpiScale = static_cast<float>(dpi) / 96.0f;
            }
        }
    }
#endif
    const float uiScale = state().uiScale > 0.0f ? state().uiScale : 1.0f;
    return dpiScale * uiScale;
}

bool cursorClientPosition(float& x, float& y) {
    const core::window::Handle window = core::window::mainWindowHandle();
    if (!window) return false;
    double physicalX = 0.0, physicalY = 0.0;
    core::window::getCursorPosition(window, physicalX, physicalY);
    const float scale = effectiveWindowScale();
    if (scale <= 0.0f) return false;
    x = static_cast<float>(physicalX) / scale;
    y = static_cast<float>(physicalY) / scale;
    return true;
}

std::vector<TabInfo> documentTabs(const AppState& state) {    std::vector<TabInfo> result;
    for (const auto id : state.tabOrder) {
        const auto* document = documentTab(state, id);
        if (!document) continue;
        auto name = document->displayName();
        if (document->path.empty()) name += " " + std::to_string(id);
        TabInfo info{id, std::move(name), document->path, document->vaultRoot, document->language(), document->dirty()};
        if (const auto slot = state.tabPresentationSlot.find(id); slot != state.tabPresentationSlot.end()) {
            info.hasGroup = true;
            info.colorSlot = slot->second;
            if (const auto ordinal = state.tabPresentationOrdinal.find(id);
                ordinal != state.tabPresentationOrdinal.end()) {
                info.groupOrdinal = ordinal->second;
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}
bool activateDocumentTabInternal(AppState& state, std::uint64_t id) {
    if (state.tabId == id) return true;
    auto found = state.inactiveTabs.find(id);
    if (found == state.inactiveTabs.end()) return false;
    auto next = std::move(found->second); state.inactiveTabs.erase(found);
    // 先把当前活动页的全局派生缓存搬回它的槽位，再换页、再把目标页的槽位装回全局。
    {
        tracelog::Span capture("cache-capture");
        captureDerivedCaches(state);
        filecheck::cancel("neo.file.check." + std::to_string(state.tabId));
        suspendActiveDocument(state);
    }
    {
        tracelog::Span install("cache-install");
        static_cast<DocumentSession&>(state) = std::move(next);
        installDerivedCaches(state);
    }
    afterSwitch(state);
    return true;
}
void clearTabCloseAnchor(AppState& state) {
    if (state.closeAnchor.enabled) state.closeAnchorRevealPending = true;
    state.closeAnchor = AppState::CloseAnchor{};
    state.closeAnchorPendingApply = false;
}
bool activateDocumentTab(AppState& state, std::uint64_t id) {
    if (documentModalOpen(state) || state.settingsOpen) return false;
    clearTabCloseAnchor(state);
    const std::uint64_t requestId = tracelog::nextRequestId();
    // 应用侧"请求受理"时刻：app.cpp 的 compose 用它折算请求→下一帧开始的排队间隔。
    tracelog::markSwitchRequest();
    tracelog::Span span("switch", requestId);
    span.note("target=" + std::to_string(id));
    if (!activateDocumentTabInternal(state, id)) return false;
    {
        tracelog::Span persist("session-submit", requestId);
        persistDocumentSession(state);
    }
    return true;
}
bool createDocumentTab(AppState& state) {
    if (state.tabOrder.size() >= 256 || state.nextTabId == std::numeric_limits<std::uint64_t>::max()) {
        showToast(state, i18n::tr("safety.open_failed"), i18n::tr("tabs.limit")); return false;
    }
    VaultContext context;
    context.vaultRoot = state.vaultRoot;
    context.expanded = state.expanded; context.filter = state.filter;
    context.vaultScroll = state.vaultScroll; context.vaultTab = state.vaultTab;
    context.vaultSelectedPath = state.vaultSelectedPath;
    captureDerivedCaches(state);
    filecheck::cancel("neo.file.check." + std::to_string(state.tabId));
    suspendActiveDocument(state);
    static_cast<DocumentSession&>(state) = DocumentSession{};
    state.tabId = state.nextTabId++; state.tabOrder.push_back(state.tabId);
    static_cast<VaultContext&>(state) = std::move(context);
    state.vaultAddress = state.vaultRoot;
    state.editorStateDirty = true;
    installDerivedCaches(state);
    afterSwitch(state);
    return true;
}
void closeActiveDocumentTab(AppState& state) {
    const auto closed = state.tabId;
    const auto found = std::find(state.tabOrder.begin(), state.tabOrder.end(), closed);
    const auto index = static_cast<std::size_t>(found - state.tabOrder.begin());
    if (found != state.tabOrder.end()) state.tabOrder.erase(found);
    state.releasedTabIds.push_back(closed);
    // 关闭的页从后台写协调器与核验队列里退场：避免 Id 复用后误复用旧正文快照。
    filecheck::cancel("neo.file.check." + std::to_string(closed));
    sessionwriter::forgetTab(closed);
    if (state.tabOrder.empty()) {
        const auto root = state.vaultRoot;
        captureDerivedCaches(state);
        static_cast<DocumentSession&>(state) = DocumentSession{};
        // No live tabs remain, so wrap the identifier allocator safely after releasing the old UI scope.
        if (state.nextTabId == std::numeric_limits<std::uint64_t>::max()) state.nextTabId = 1;
        state.tabId = state.nextTabId++; state.tabOrder.push_back(state.tabId);
        state.vaultRoot = root; state.vaultAddress = root;
        state.editorStateDirty = true;
        installDerivedCaches(state);
    } else {
        const auto next = state.tabOrder[std::min(index, state.tabOrder.size() - 1)];
        captureDerivedCaches(state);
        static_cast<DocumentSession&>(state) = std::move(state.inactiveTabs.at(next));
        state.inactiveTabs.erase(next);
        installDerivedCaches(state);
    }
    afterSwitch(state); persistDocumentSession(state);
}
// 干净页激活后的后台文件核验（阶段 D）。请求固定住页面身份/路径/正文代次；完成后只有
// 路径、页面、正文代次与干净状态仍吻合才应用新内容 —— 期间用户编辑、另存、重命名、
// 关闭/复开一律丢弃自动覆盖。脏页不核验（原指纹保留到确证保存）。
void scheduleActiveFileVerification(AppState& appState) {
    if (appState.path.empty() || appState.dirty() || appState.unavailable) {
        return;
    }
    const std::uint64_t tabId = appState.tabId;
    const std::string path = appState.path;
    const filesafety::Fingerprint baseline = appState.diskFingerprint;
    const unsigned long long revision = appState.revision;
    const std::uint64_t requestId = tracelog::nextRequestId();
    filecheck::Request request;
    request.requestId = requestId;
    request.path = path;
    request.baseline = baseline;
    filecheck::request("neo.file.check." + std::to_string(tabId), std::move(request),
                       [tabId, path, revision, requestId](filecheck::Result result) {
                           AppState& s = state();
                           if (result.status != filecheck::Status::Changed || !result.loaded.ok) {
                               tracelog::event("file-check", requestId,
                                               std::string("status=") +
                                                   (result.status == filecheck::Status::Unchanged ? "unchanged"
                                                    : result.status == filecheck::Status::Missing ? "missing"
                                                    : result.status == filecheck::Status::Failed  ? "failed"
                                                                                                  : "canceled"));
                               return;
                           }
                           auto* page = documentTab(s, tabId);
                           if (page == nullptr) {
                               return;  // 页已关闭
                           }
                           if (page->dirty() || page->revision != revision) {
                               tracelog::event("result-drop", requestId, "reason=edited");
                               return;  // 期间有编辑：不覆盖用户正文
                           }
                           if (!sameDocumentFile(page->path, path)) {
                               tracelog::event("result-drop", requestId, "reason=path-changed");
                               return;  // 另存/重命名过
                           }
                           const bool active = tabId == s.tabId;
                           page->doc = std::move(result.loaded.document);
                           page->diskFingerprint = result.observed;
                           ++page->revision;
                           page->savedRevision = page->revision;
                           page->unavailable = false;
                           page->loadError.clear();
                           page->foldedHeadings.clear();
                           page->statsRevision = static_cast<unsigned long long>(-1);
                           page->findMatchesRevision = static_cast<unsigned long long>(-1);
                           // 内容被替换：该页的正文派生缓存全部失效（不能套旧几何/旧装饰）。
                           releaseDerivedCaches(*page);
                           page->editorMemory.reset();
                           if (active) {
                               s.editorStateDirty = true;
                           }
                           tracelog::event("result-apply", requestId, "kind=file-changed");
                           app::requestUpdate();
                       });
}
void requestCloseTab(AppState& state, std::uint64_t id) {
    if (documentModalOpen(state) || state.settingsOpen) return;
    if (!activateDocumentTabInternal(state, id)) return;
    if (!state.dirty()) { closeActiveDocumentTab(state); return; }
    state.pending = PendingAction::CloseTab; state.confirmationTabId = id;
    state.safetyChoice = 2; state.openMenu = MenuKind::None; state.tabListOpen = false;
    app::requestUpdate();
}
void moveDocumentTab(AppState& state, std::uint64_t id, int direction) {
    if (documentModalOpen(state) || state.settingsOpen || direction == 0) return;
    auto found = std::find(state.tabOrder.begin(), state.tabOrder.end(), id);
    if (found == state.tabOrder.end()) return;
    if (direction < 0 && found != state.tabOrder.begin()) std::iter_swap(found, found - 1);
    if (direction > 0 && found + 1 != state.tabOrder.end()) std::iter_swap(found, found + 1);
    persistDocumentSession(state); app::requestUpdate();
}
namespace {
// 汇总一次完整的会话提交（所有标签的元数据 + 脏页正文指针）。协调器负责把它们转成
// 拥有的不可变快照并按 (TabId, revision) 复用正文，UI 线程不做磁盘事务。
bool buildSessionSubmission(const AppState& state, sessionwriter::Submission& submission) {
    submission.activeId = state.tabId;
    submission.records.reserve(state.tabOrder.size());
    for (const auto id : state.tabOrder) {
        const auto* document = documentTab(state, id);
        if (!document) return false;
        sessionwriter::Record record;
        record.id = id;
        record.path = document->path.empty() ? document->recoveredFrom : document->path;
        record.vaultRoot = document->vaultRoot;
        record.language = document->language();
        record.wrapOverride = document->wrapOverride;
        record.dirty = document->dirty() && !document->suppressRecovery;
        record.revision = document->revision;
        record.document = record.dirty ? &document->doc : nullptr;
        submission.records.push_back(std::move(record));
    }
    return true;
}
} // namespace

bool persistDocumentSession(AppState& state) {
    if (state.sessionStorageBlocked) {
        if (state.dirty() && !state.recoveryWriteWarning) {
            state.recoveryWriteWarning = true;
            showToast(state, i18n::tr("safety.save_failed"), i18n::tr("tabs.recovery_failed"));
        }
        return false;
    }
    if (settings::configDirectory().empty()) {
        if (state.dirty() && !state.suppressRecovery) return settings::writeRecovery(state.doc.text, recoveryOrigin(state), &state.doc);
        settings::clearRecovery(); return true;
    }
    // 会话写交给串行后台协调器（阶段 E）：这里只把当前状态汇总成一次提交，不再在
    // UI 线程跑 sessionstorage::write。返回"是否入队"，持久化以 committedSeq 为准。
    sessionwriter::Submission submission;
    if (!buildSessionSubmission(state, submission)) {
        return false;
    }
    const auto sequence = sessionwriter::submit(std::move(submission));
    if (tracelog::enabled()) {
        tracelog::event("recovery-cache", sequence,
            "retained_body_bytes=" + std::to_string(sessionwriter::stats().retainedBodyBytes));
    }
    return sequence != 0;
}

// 退出/关闭屏障：提交最终会话并等待 commit ack（有界超时）。只在关闭/退出路径调用，
// 它自己 pump dispatchReady（此时帧循环可能已停）。返回是否在超时前确认提交。
bool flushDocumentSession(AppState& state, int timeoutMs) {
    if (state.sessionStorageBlocked || settings::configDirectory().empty()) {
        return persistDocumentSession(state);
    }
    sessionwriter::Submission submission;
    if (!buildSessionSubmission(state, submission)) {
        return false;
    }
    return sessionwriter::checkpointBlocking(std::move(submission), timeoutMs);
}
void syncDocumentTabInputs(eui::Ui& ui, AppState& state) {
    using InputState = components::input_detail::InputModel::InputState;
    if (state.displayedInputTabId && state.displayedInputTabId != state.tabId) {
        const auto old = state.displayedInputTabId;
        auto& input = ui.state<InputState>(editorInputId(old));
        if (auto* document = documentTab(state, old)) {
            retainEditingState(input);
            document->editorMemory = std::make_unique<InputState>(std::move(input));
            // 排版/几何/装饰快照随输入内存挂到这一页：把它的字节并进预算计账，并重新
            // 标记为"持有派生缓存"，否则这一页会在槽位被逐出后（use 归零）永远逃过预算。
            const std::uint64_t inputBytes = estimateInputBytes(
                *document->editorMemory, document->derivedDecoration.get());
            document->derivedCacheBytes += inputBytes;
            if (inputBytes > 65536) {
                document->derivedCacheUse = tracelog::nowMicros() != 0 ? tracelog::nowMicros() : 1;
            }
        }
        ui.releaseStateScope(editorInputId(old));
    }
    for (const auto id : state.releasedTabIds) {
        if (id != state.displayedInputTabId) ui.releaseStateScope(editorInputId(id));
    }
    state.releasedTabIds.clear();
    if (state.editorMemory && !state.editorStateDirty) {
        auto& input = ui.state<InputState>(editorInputId(state));
        input = std::move(*state.editorMemory); state.editorMemory.reset();
        // Preserve the stored viewport; automatic follow would undo scrolling on every switch.
        input.followCaret = false;
    }
    state.displayedInputTabId = state.tabId;
    // 输入内存刚挂回页上，预算可能因此被顶破：这里再校一次（页数不多，O(页) 很便宜）。
    enforceDerivedBudget(state);
}
} // namespace neo
