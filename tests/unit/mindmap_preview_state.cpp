// 思维导图只读预览的模型层 A/B 回归（不接 UI、不开窗口）。
//
// 被测：openMindmapModel（解析树 move 进页自有 mindmap::Cache，PreviewState 只留
// 身份锚与视口——不保留第二份树）、mindmapModelCurrent（外部回写一律失效）、
// mindmapCacheCurrent / storeMindmapCache（完整缓存键 + 活动预算守卫）。布局用
// buildLayout 的合成尺寸离线构造，不做真实测字（无头环境不初始化字体后端）。
// 只读性硬契约：打开 / viewport / 关闭 / 反复关闭 / 各类拒绝都不得改动正文、
// revision、savedRevision、撤销重做历史、textRevision / pendingEdit、折叠态与
// dirty。模型层通过不等于 GUI 可用（绘制 / 命中 / DPI / IME 仍需独立验收）。

#include "components/input_model.h"
#include "model/lp_decorations.h"
#include "model/mindmap_cache.h"
#include "model/mindmap_layout.h"
#include "model/mindmap_plan.h"
#include "state/mindmap_preview_state.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

// Release 也有效的检查：不依赖 assert / NDEBUG，失败必打印并计数。
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// 空 std::string 在 MSVC 走 SSO，capacity() 通常是 15 而不是 0 —— 不能用 ==0 判"无堆驻留"。
// 用"容量不超过一个空串的档位"表达：release() 用 swap 之后即回到空串档。
bool noStringResident(const std::string& s) {
    return s.capacity() <= std::string{}.capacity();
}

using Model = components::input_detail::InputModel;
using InputState = Model::InputState;
using RejectReason = neo::mindmap::RejectReason;
using PreviewState = neo::mindmap::PreviewState;
using Cache = neo::mindmap::Cache;

// 含一个闭合顶级 mermaid mindmap 围栏块，块外还有普通正文（用来验证切片只取块）。
std::string makeDoc() {
    return "# 标题\n"
           "\n"
           "段落文字。\n"
           "\n"
           "```mermaid\n"
           "mindmap\n"
           "  root((中心))\n"
           "    子节点一\n"
           "    子节点二\n"
           "```\n"
           "\n"
           "文末。\n";
}

struct Scenario {
    neo::AppState app;
    InputState input;
    std::size_t blockCursor = 0;
};

// 备一份"真实文档 + 真实编辑历史"的场景：
//   · markdown 能力来自 newDocumentLanguage（path 为空的新文档）；
//   · 两次 insertAtCursor 后 undo 一次 —— 撤销栈与重做栈都非空，pendingEdit 也是
//     公共漏斗写下的可信值。编辑接口只在 fixture 里用，之后的被测操作一律不得再碰。
void buildScenario(Scenario& s) {
    s.app.newDocumentLanguage = "markdown";
    s.app.tabId = 42;
    s.app.path.clear();
    s.app.revision = 7;
    s.app.savedRevision = 6;  // dirty() == true，便于验证只读性
    s.app.foldedHeadings = std::set<int>{0};

    InputState& in = s.input;
    in.text = makeDoc();
    in.textRevision = 1;
    in.cursor = static_cast<int>(in.text.size());
    Model::insertAtCursor(in, "尾部一\n");
    in.cursor = static_cast<int>(in.text.size());
    Model::insertAtCursor(in, "尾部二\n");
    check(Model::undoEdit(in), "fixture: undo 应把最后一次插入移入重做栈");

    s.app.doc.text = in.text;
    const std::size_t at = s.app.doc.text.find("mindmap");
    check(at != std::string::npos, "fixture: 文档应含 mindmap 围栏块");
    in.cursor = static_cast<int>(at);
    s.blockCursor = at;

    // 计划缓存必须与文档同源：cache.text == doc.text，且 input.text == doc.text。
    neo::lp::cachedPlan(s.app.doc.text);
}

// 逐字段快照：文档身份 + 编辑元数据 + 撤销/重做历史 + 折叠态。
struct Snapshot {
    std::string docText;
    unsigned long long revision = 0;
    unsigned long long savedRevision = 0;
    bool dirty = false;
    std::set<int> folded;
    unsigned long long textRevision = 0;
    int cursor = 0;
    int selectionStart = 0;
    int selectionEnd = 0;
    components::input_detail::PendingTextEdit pending;
    std::vector<Model::EditRecord> undo;
    std::vector<Model::EditRecord> redo;
};

Snapshot capture(const Scenario& s) {
    Snapshot snap;
    snap.docText = s.app.doc.text;
    snap.revision = s.app.revision;
    snap.savedRevision = s.app.savedRevision;
    snap.dirty = s.app.dirty();
    snap.folded = s.app.foldedHeadings;
    snap.textRevision = s.input.textRevision;
    snap.cursor = s.input.cursor;
    snap.selectionStart = s.input.selectionStart;
    snap.selectionEnd = s.input.selectionEnd;
    snap.pending = s.input.pendingEdit;
    snap.undo = s.input.undoStack;
    snap.redo = s.input.redoStack;
    return snap;
}

bool sameRecord(const Model::EditRecord& a, const Model::EditRecord& b) {
    return a.beg == b.beg && a.removed == b.removed && a.inserted == b.inserted &&
           a.cursorBefore == b.cursorBefore && a.selectionStartBefore == b.selectionStartBefore &&
           a.selectionEndBefore == b.selectionEndBefore && a.cursorAfter == b.cursorAfter &&
           a.selectionStartAfter == b.selectionStartAfter && a.selectionEndAfter == b.selectionEndAfter;
}

bool sameRecords(const std::vector<Model::EditRecord>& a, const std::vector<Model::EditRecord>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!sameRecord(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

bool samePending(const components::input_detail::PendingTextEdit& a, const components::input_detail::PendingTextEdit& b) {
    return a.valid == b.valid && a.revision == b.revision && a.byteBeg == b.byteBeg &&
           a.oldEnd == b.oldEnd && a.newEnd == b.newEnd && a.firstLine == b.firstLine &&
           a.newLastLine == b.newLastLine && a.oldTailLine == b.oldTailLine &&
           a.cursorBefore == b.cursorBefore;
}

// 只读性总检：场景的每一个可观察字段都必须逐字段等于快照。
void checkUntouched(const Scenario& s, const Snapshot& snap, const char* stage) {
    const std::string p = std::string(stage) + ": ";
    check(s.app.doc.text == snap.docText, (p + "文档正文被改动").c_str());
    check(s.app.revision == snap.revision, (p + "revision 被改动").c_str());
    check(s.app.savedRevision == snap.savedRevision, (p + "savedRevision 被改动").c_str());
    check(s.app.dirty() == snap.dirty, (p + "dirty() 被改动").c_str());
    check(s.app.foldedHeadings == snap.folded, (p + "折叠集合被改动").c_str());
    check(s.input.textRevision == snap.textRevision, (p + "textRevision 被改动").c_str());
    check(s.input.cursor == snap.cursor, (p + "cursor 被改动").c_str());
    check(s.input.selectionStart == snap.selectionStart, (p + "selectionStart 被改动").c_str());
    check(s.input.selectionEnd == snap.selectionEnd, (p + "selectionEnd 被改动").c_str());
    check(samePending(s.input.pendingEdit, snap.pending), (p + "pendingEdit 被改动").c_str());
    check(sameRecords(s.input.undoStack, snap.undo), (p + "撤销栈被改动").c_str());
    check(sameRecords(s.input.redoStack, snap.redo), (p + "重做栈被改动").c_str());
}

bool openSuccess(PreviewState& preview, Cache& cache, Scenario& s) {
    return neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) ==
           RejectReason::None;
}

// 外部回写构造：替换文档文本、同步输入组件并重建计划缓存（保持 revision / tabId /
// path / textRevision 不变——正是"同 revision 块改变"要打的那条路）。
void externalWriteBack(Scenario& s, const std::string& text) {
    s.app.doc.text = text;
    s.input.text = text;
    neo::lp::cachedPlan(text);
}

neo::MindmapStyleKey makeStyle(float fontSize = 16.0f, float effectiveScale = 1.0f,
                               float panelWidth = 800.0f, int theme = 0,
                               const char* themeFile = "") {
    neo::MindmapStyleKey style;
    style.editorFontFamily = "test-editor-font";
    style.codeFontFamily = "test-code-font";
    style.themeFile = themeFile;
    style.fontSize = fontSize;
    style.effectiveScale = effectiveScale;
    style.panelWidth = panelWidth;
    style.theme = theme;
    style.themeVersion = 0;
    return style;
}

// 无测字的离线入库：合成标签尺寸 + buildLayout（model 纯逻辑，无字体后端依赖）。
bool buildAndStore(Cache& cache, PreviewState& preview, const neo::MindmapStyleKey& style,
                   std::size_t maxNodes = 2000) {
    std::vector<neo::mindmap::Size> sizes(cache.plan.nodes.size());
    for (std::size_t index = 0; index < cache.plan.nodes.size(); ++index) {
        sizes[index] = neo::mindmap::Size{
            static_cast<float>(cache.plan.nodes[index].label.size()) * 8.0f + 24.0f, 22.4f};
    }
    neo::mindmap::Options options;
    options.maxNodes = maxNodes;
    neo::mindmap::Layout layout = neo::mindmap::buildLayout(cache.plan, sizes, options);
    if (!layout) {
        return false;
    }
    return neo::storeMindmapCache(cache, preview, style, std::move(sizes), std::move(layout));
}

// fixture 轨迹修正：undo/redo 会把光标设回该编辑记录的 before/after（本例都在文档
// 尾部），而 selectTarget 只认"光标所在的闭合围栏块"。重开之前必须把目标光标显式放
// 回块内——绝不把"自动扫全文找块"塞进生产来迁就测试。
void focusMindmapBlock(Scenario& s) {
    const std::size_t at = s.app.doc.text.find("mindmap");
    check(at != std::string::npos, "fixture: 文档应仍含 mindmap 围栏块");
    s.input.cursor = static_cast<int>(at);
}

} // namespace

int main() {
    // ── 1. 成功打开（冷路径）：解析树 move 进页缓存，preview 无第二份树 ───────
    {
        Scenario s;
        buildScenario(s);
        const Snapshot before = capture(s);
        PreviewState preview;
        Cache cache;

        const unsigned long long fullBefore = neo::planDebugStats().full;
        const RejectReason opened =
            neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache());
        const unsigned long long fullAfter = neo::planDebugStats().full;

        check(opened == RejectReason::None, "打开成功应返回 None");
        check(preview.open, "打开后 preview.open 应为 true");
        check(preview.tabId == s.app.tabId, "preview.tabId 应来自 AppState");
        check(preview.path == s.app.path, "preview.path 应来自 AppState");
        check(preview.revision == s.app.revision, "preview.revision 应来自 AppState");
        check(preview.textRevision == s.input.textRevision, "preview.textRevision 应来自 InputState");
        check(preview.rejectReason == RejectReason::None, "成功打开的 rejectReason 应为 None");
        check(cache.valid == false, "冷打开后布局尚未建，cache.valid 应为 false");
        check(cache.generation == 1, "冷打开应把缓存 generation 推到 1");
        check(!cache.plan.nodes.empty(), "解析树应在页缓存里");
        check(cache.plan.nodes[0].label == "中心", "根节点 ((中心)) 的标签应为 中心");
        check(cache.source.rfind("```mermaid", 0) == 0 && cache.source.find("mindmap") != std::string::npos,
              "cache.source 应是含 mermaid 围栏的块切片");
        check(cache.source.find("文末") == std::string::npos,
              "块切片不得吞进围栏块之外的正文");
        const std::size_t sb = cache.plan.sourceBeg;
        const std::size_t se = cache.plan.sourceEnd;
        check(sb <= se && se <= s.app.doc.text.size() &&
                  std::string(s.app.doc.text.data() + sb, se - sb) == cache.source,
              "cache.source 必须等于 plan 区间在文档中的切片");
        check(cache.key.tabId == s.app.tabId && cache.key.revision == s.app.revision &&
                  cache.key.blockBeg == sb && cache.key.blockEnd == se &&
                  cache.key.codeLang == "mermaid" && cache.key.cacheVersion == neo::mindmap::kMindmapCacheVersion,
              "冷打开应填好键的身份/块/语言/语义版本维度");
        check(neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "刚打开（布局未建）时身份守卫不依赖布局：current 应为真");
        check(fullAfter == fullBefore,
              "打开期间不得重跑 buildLpPlan（planDebugStats().full 不增长）");
        checkUntouched(s, before, "打开");
    }

    // ── 2. 入库 + 缓存当前性 + viewport 只动视口 ─────────────────────────────
    {
        Scenario s;
        buildScenario(s);
        const Snapshot before = capture(s);
        PreviewState preview;
        Cache cache;
        check(openSuccess(preview, cache, s), "viewport 用例前置：应能打开");

        const neo::MindmapStyleKey style = makeStyle();
        check(!neo::mindmapCacheCurrent(cache, preview, style),
              "布局未入库前缓存不得判为 current");
        check(buildAndStore(cache, preview, style), "入库应成功");
        check(cache.valid && cache.bytes > 0, "入库后 valid 且字节估算为正");
        check(cache.layout.nodes.size() == cache.plan.nodes.size(),
              "布局节点应与解析树同序等长");
        check(cache.key.fontSize == style.fontSize && cache.key.panelWidth == style.panelWidth &&
                  cache.key.effectiveScale == style.effectiveScale && cache.key.theme == style.theme,
              "入库应补全键的样式维度");
        check(neo::mindmapCacheCurrent(cache, preview, style), "同键应判为 current");
        check(neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "入库后 current 应为真");

        const float zoom0 = preview.viewport.zoom;
        preview.viewport.wheel(2.0f, 400.0f, 300.0f, 800.0f, 600.0f);
        preview.viewport.panX += 30.0f;
        preview.viewport.panY -= 10.0f;
        preview.viewport.constrain(400.0f, 300.0f, 800.0f, 600.0f);

        check(preview.viewport.zoom > zoom0, "wheel 应放大视口（证明视口操作真实生效）");
        check(preview.open && cache.valid && !cache.plan.nodes.empty(),
              "viewport 操作不得关闭预览或清空页缓存");
        check(neo::mindmapCacheCurrent(cache, preview, style),
              "viewport 操作不得使缓存失效");
        checkUntouched(s, before, "viewport 操作");
    }

    // ── 2b. 样式键任一维度变化必须失效（字体/字号/DPI/宽度/主题/主题文件）────
    {
        Scenario s;
        buildScenario(s);
        PreviewState preview;
        Cache cache;
        check(openSuccess(preview, cache, s) && buildAndStore(cache, preview, makeStyle()),
              "样式键用例前置：应能打开并入库");

        const auto invalidate = [&](const char* what, const neo::MindmapStyleKey& changed) {
            check(!neo::mindmapCacheCurrent(cache, preview, changed),
                  (std::string("样式变化必须失效: ") + what).c_str());
        };
        // editor 字体：换字面必须失效。
        {
            neo::MindmapStyleKey changed = makeStyle();
            changed.editorFontFamily = "other-editor-font";
            invalidate("editor 字体", changed);
        }
        {
            neo::MindmapStyleKey changed = makeStyle();
            changed.codeFontFamily = "other-mono";
            invalidate("code 字体", changed);
        }
        invalidate("字号", makeStyle(18.0f));
        invalidate("DPI/effectiveScale", makeStyle(16.0f, 1.25f));
        invalidate("面板宽度", makeStyle(16.0f, 1.0f, 640.0f));
        invalidate("主题", makeStyle(16.0f, 1.0f, 800.0f, 1));
        invalidate("主题文件", makeStyle(16.0f, 1.0f, 800.0f, 0, "theme.json"));
        {
            neo::MindmapStyleKey changed = makeStyle();
            changed.themeVersion = 3;
            invalidate("主题文件版本", changed);
        }
        check(neo::mindmapCacheCurrent(cache, preview, makeStyle()),
              "样式复原后同键应再次 current");
    }

    // ── 2c. 开块固定、只认语法：移动光标不改变已打开的只读模型 ───────────────
    {
        Scenario s;
        buildScenario(s);
        const Snapshot before = capture(s);
        PreviewState preview;
        Cache cache;
        check(openSuccess(preview, cache, s), "光标无关性前置：应能打开");
        check(buildAndStore(cache, preview, makeStyle()), "光标无关性前置：应能入库");

        const std::size_t nodeCount = cache.plan.nodes.size();
        const unsigned long long fullBefore = neo::planDebugStats().full;

        const std::size_t outside = s.app.doc.text.find("文末");
        check(outside != std::string::npos, "fixture: 应有块外正文");
        s.input.cursor = static_cast<int>(outside);
        check(cache.plan.nodes.size() == nodeCount,
              "光标移出开块：已打开的解析树不变（开块固定）");
        check(neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "光标移出开块：只认语法的模型不因光标移动而失效");
        check(neo::planDebugStats().full == fullBefore, "光标移动不得触发 plan 重建");

        s.input.cursor = before.cursor;  // cursor 属于只读快照，恢复后逐字段核对
        checkUntouched(s, before, "光标移动");
    }

    // ── 3. 关闭：身份/视口复位；页自有缓存保留供暖开 ─────────────────────────
    {
        Scenario s;
        buildScenario(s);
        const Snapshot before = capture(s);
        PreviewState preview;
        Cache cache;
        check(openSuccess(preview, cache, s), "关闭用例前置：应能打开");
        check(buildAndStore(cache, preview, makeStyle()), "关闭用例前置：应能入库");

        preview.close();
        check(!preview.open, "关闭后 open 应为 false");
        check(preview.tabId == 0, "关闭后 tabId 应复位");
        check(preview.path.empty() && noStringResident(preview.path), "关闭后 path 应复位且无堆驻留");
        check(preview.revision == 0 && preview.textRevision == 0, "关闭后 revision/textRevision 应复位");
        check(preview.rejectReason == RejectReason::None, "关闭后 rejectReason 应复位");
        check(preview.viewport.zoom == 1.0f && preview.viewport.panX == 0.0f && preview.viewport.panY == 0.0f,
              "关闭后 viewport 应复位");
        // 页自有缓存原样保留（阶段 D：暖开 + 可淘汰）；正文/撤销不受影响。
        check(cache.valid && !cache.plan.nodes.empty() && cache.bytes > 0,
              "关闭只清预览状态，页自有缓存保留");
        checkUntouched(s, before, "关闭");

        // ── 4. 反复关闭：幂等、不残留 ───────────────────────────────────────
        preview.close();
        preview.close();
        check(!preview.open && preview.path.empty() && preview.revision == 0,
              "反复关闭不得残留 open / 身份");
        check(cache.valid && !cache.plan.nodes.empty(), "反复关闭不得误清页自有缓存");
        checkUntouched(s, before, "反复关闭");

        // ── 5. 暖开：同键重开不重解析（generation 不变、布局保留）────────────
        check(openSuccess(preview, cache, s), "暖开应成功");
        check(cache.generation == 1, "暖开不得重解析（generation 保持 1）");
        check(cache.valid && cache.layout.nodes.size() == cache.plan.nodes.size(),
              "暖开保留既有布局");
        check(neo::mindmapCacheCurrent(cache, preview, makeStyle()), "暖开后缓存应仍 current");
        checkUntouched(s, before, "暖开");
    }

    // ── 6. 拒绝（不改变文档身份）：先有 open，再验证拒绝把它清掉 ─────────────
    {
        Scenario s;
        buildScenario(s);
        const Snapshot before = capture(s);
        PreviewState preview;
        Cache cache;

        check(openSuccess(preview, cache, s), "拒绝用例前置：应能打开");
        check(preview.open, "拒绝用例前置：preview 应为 open");

        // 非 markdown：语言覆盖成纯文本。拒绝只关预览；页缓存是键控的
        // （展示必须键/锚点全对上），既有内容保留供暖开，不清。
        s.app.languageOverride = "text";
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) ==
                  RejectReason::NotMarkdown,
              "非 markdown 应以 NotMarkdown 拒绝");
        check(preview.rejectReason == RejectReason::NotMarkdown &&
                  std::string(neo::mindmap::rejectReasonName(preview.rejectReason)) == "not_markdown",
              "NotMarkdown 的稳定标识应为 not_markdown");
        check(!preview.open &&
                  !neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "非 md 拒绝后预览关闭且不判为 current");
        s.app.languageOverride.clear();

        // 计划未就绪：缓存失效（换文档 / 尚未重建）。
        neo::lp::invalidatePlanCache();
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) ==
                  RejectReason::PlanNotReady,
              "计划缓存失效应以 PlanNotReady 拒绝");
        check(preview.rejectReason == RejectReason::PlanNotReady && !preview.open,
              "未就绪拒绝后不得残留 open");
        neo::lp::cachedPlan(s.app.doc.text);  // 恢复缓存

        // 陈旧 input：输入组件文本与文档 / 缓存不一致（外部赋值但装饰层未重建）。
        const std::string savedText = s.input.text;
        s.input.text = "损坏的陈旧文本";
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) ==
                  RejectReason::PlanNotReady,
              "输入组件文本与文档不一致（陈旧 input）应以 PlanNotReady 拒绝");
        check(!preview.open, "陈旧 input 拒绝后不得残留 open");
        s.input.text = savedText;

        // cursor 无效：-1 夹到首行、越界夹到末行，都是"没有 fenced 块"→ NoBlock。
        const int savedCursor = s.input.cursor;
        s.input.cursor = -1;
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) == RejectReason::NoBlock,
              "cursor=-1 应落到首行并以 NoBlock 拒绝");
        s.input.cursor = static_cast<int>(s.app.doc.text.size()) + 100000;
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) == RejectReason::NoBlock,
              "cursor 越界应落到末行并以 NoBlock 拒绝");
        check(!preview.open, "cursor 无效拒绝后不得残留 open");
        s.input.cursor = savedCursor;

        // 源码视图：该模式编辑器不建 LP cache，即使残留上一帧缓存也必须以
        // PlanNotReady 拒绝（sourceView() 守卫），且 current 失效。
        s.app.languageOverride = "c";
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) ==
                  RejectReason::PlanNotReady,
              "源码视图应以 PlanNotReady 拒绝");
        check(!neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "源码视图下不得判为 current");
        s.app.languageOverride.clear();
        s.input.cursor = savedCursor;

        checkUntouched(s, before, "拒绝路径");
    }

    // ── 7. 块级拒绝：非 mermaid 围栏 / mermaid 但非 mindmap / 缩进非法 / 无块 ─
    {
        struct Case {
            const char* doc;
            RejectReason expected;
            const char* reason;
            const char* label;
        };
        const Case cases[] = {
            {"```python\nx = 1\n```\n", RejectReason::UnsupportedLanguage, "unsupported_language",
             "非 mermaid 围栏"},
            {"```mermaid\nflowchart LR\n  A --> B\n```\n", RejectReason::NotMindmap, "not_mindmap",
             "mermaid 围栏但内容不是 mindmap"},
            {"```mermaid\nmindmap\n  a\n      b\n   c\n```\n", RejectReason::InvalidIndent,
             "invalid_indent", "mindmap 缩进栈非法"},
            {"只有正文，没有围栏块\n", RejectReason::NoBlock, "no_block", "没有围栏块"},
        };
        for (const Case& c : cases) {
            Scenario s;
            buildScenario(s);
            // 这里刻意外部改写文档来构造拒绝输入：只读性快照要在改写之后拍，
            // 才能证明"openMindmapModel 本身"没有再动文档。
            externalWriteBack(s, c.doc);
            s.input.cursor = 0;
            const Snapshot before = capture(s);
            PreviewState preview;
            Cache cache;
            const RejectReason got =
                neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache());
            check(got == c.expected, (std::string(c.label) + " 的拒绝原因应精确匹配").c_str());
            check(preview.rejectReason == c.expected &&
                      std::string(neo::mindmap::rejectReasonName(preview.rejectReason)) == c.reason,
                  (std::string(c.label) + " 的 rejectReasonName 应精确匹配").c_str());
            check(!preview.open && !cache.valid && cache.plan.nodes.empty() &&
                      noStringResident(cache.source),
                  (std::string(c.label) + " 拒绝后不得残留 open / 页缓存驻留").c_str());
            checkUntouched(s, before, c.label);
        }
    }

    // ── 8. 状态头的 target 范围自检（防御性）：selectTarget 给出越界区间时，
    //        openMindmapModel 必须在拿它切片之前以 InvalidFence 挡下，绝不越界读。 ──
    {
        const std::string doc = "```mermaid\nmindmap\n  a\n```";  // 末行即闭围栏
        Scenario s;
        buildScenario(s);
        s.app.doc.text = doc;
        s.input.text = doc;
        s.input.cursor = 0;  // 落在开围栏行，其 codeBlockEnd 将被改越界

        neo::lp::PlanCache good;
        good.text = doc;
        good.plan = neo::buildLpPlan(doc);
        good.valid = true;
        check(!good.plan.lines.empty() && good.plan.lines[0].codeFence,
              "crafted: 首行应为开围栏");
        PreviewState okPreview;
        Cache okCache;
        check(neo::openMindmapModel(okPreview, okCache, s.app, s.input, good) == RejectReason::None,
              "crafted 前置：未越界的同一计划应能正常打开");

        neo::lp::PlanCache bad = good;  // 唯一差别：cursor 行的 codeBlockEnd 越界
        bad.plan.lines[0].codeBlockEnd = static_cast<int>(doc.size()) + 64;
        PreviewState preview;
        Cache cache;
        const RejectReason got = neo::openMindmapModel(preview, cache, s.app, s.input, bad);
        check(got == RejectReason::InvalidFence,
              "越界 target 区间必须以 InvalidFence 挡下（不越界切片）");
        check(!preview.open && !cache.valid && cache.plan.nodes.empty() &&
                  noStringResident(cache.source),
              "越界区间拒绝后不得残留 open / 页缓存驻留");
    }

    // ── 9. mindmapModelCurrent：外部回写 / 身份与缓存变化的全部失效通道 ────────
    {
        Scenario s;
        buildScenario(s);
        PreviewState preview;
        Cache cache;
        auto current = [&]() {
            return neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache());
        };
        auto reopen = [&]() { return openSuccess(preview, cache, s); };

        check(reopen() && buildAndStore(cache, preview, makeStyle()) && current(),
              "current 用例前置：打开、入库且为 current");

        // textRevision 变化（编辑元数据动了，即使文本字节恰好相同）。
        const unsigned long long savedTr = s.input.textRevision;
        s.input.textRevision += 1;
        check(!current(), "textRevision 变化必须 invalidate");
        s.input.textRevision = savedTr;
        check(reopen() && current(), "恢复 textRevision 后重开应再次 current");

        // path 变化（同一 tabId，但文档身份不同；仍为 markdown）。
        const std::string savedPath = s.app.path;
        s.app.path = "notes/x.md";
        check(!current(), "path 变化必须 invalidate");
        s.app.path = savedPath;
        check(reopen() && current(), "恢复 path 后重开应再次 current");

        // tabId 变化。
        const std::uint64_t savedTab = s.app.tabId;
        s.app.tabId = savedTab + 1;
        check(!current(), "tabId 变化必须 invalidate");
        s.app.tabId = savedTab;

        // markdown 能力变化。
        s.app.languageOverride = "text";
        check(!current(), "markdown 能力丢失必须 invalidate");
        s.app.languageOverride.clear();

        // cache.valid 变化（lp 计划缓存失效）。
        neo::lp::invalidatePlanCache();
        check(!current(), "lp cache.valid 变化必须 invalidate");
        neo::lp::cachedPlan(s.app.doc.text);
        check(current(), "lp cache 以同一文本重建后应再次 current");

        // lp cache.text 变化（缓存描述的是另一篇文档）。
        neo::lp::cachedPlan("完全另一篇文档\n");
        check(!current(), "lp cache.text 变化必须 invalidate");
        neo::lp::cachedPlan(s.app.doc.text);
        check(current(), "lp cache 文本回到本篇后应再次 current");

        // 输入同源：input.text 与 doc.text 等长不一致（无 textRevision 变化）也必须失效。
        const std::string syncedText = s.app.doc.text;
        std::string mutated = syncedText;
        mutated[0] = mutated[0] == '#' ? ' ' : '#';
        s.input.text = mutated;
        check(!current(), "input.text 与 doc.text 不一致必须 invalidate");
        s.input.text = syncedText;
        check(current(), "input.text 恢复同源后应再次 current");

        // 同 revision / 同 textRevision / 同 tabId / 同 path，但块字节被外部等长改写。
        std::string edited = s.app.doc.text;
        const std::size_t pos = edited.find("子节点一");
        check(pos != std::string::npos, "fixture: 应有 子节点一");
        edited.replace(pos, std::string("子节点一").size(), "子节点甲");
        externalWriteBack(s, edited);  // revision / tabId / path / textRevision 全部不变
        check(!current(), "同 revision 但块内容外部改写必须 invalidate");
        check(reopen() && buildAndStore(cache, preview, makeStyle()) && current(),
              "外部回写后用新 cache 重新打开应再次 current");
    }

    // ── 10. undo / redo 后必须用新 cache 重新打开 ─────────────────────────────
    {
        Scenario s;
        buildScenario(s);
        PreviewState preview;
        Cache cache;
        check(openSuccess(preview, cache, s), "undo/redo 用例前置：应能打开");
        const unsigned long long warmGeneration = cache.generation;

        check(Model::undoEdit(s.input), "undo 应生效");
        s.app.doc.text = s.input.text;
        check(!neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "undo 后旧 cache / 旧 preview 必须 invalidate");

        neo::lp::cachedPlan(s.app.doc.text);
        // 根因固定：undo/redo 把光标设回编辑记录的 before/after（本例落在文档尾部），
        // 已不在围栏块内 —— 直接拿这个光标重开会以 NoBlock 拒绝。这里先用原始光标
        // 探一次钉住根因，再由 fixture 把目标光标显式放回开块。
        {
            PreviewState probe;
            check(neo::openMindmapModel(probe, cache, s.app, s.input, neo::lp::planCache()) ==
                      RejectReason::NoBlock,
                  "根因：undo 后光标落在块外，应以 NoBlock 拒绝（不得自动扫全文）");
            // 拒绝只关预览；既有页缓存键控保留（陈旧锚点不可能被展示）。
            check(cache.generation == warmGeneration &&
                      !neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
                  "NoBlock 探针后缓存保留但不得判为可展示");
        }
        focusMindmapBlock(s);
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) == RejectReason::None,
              "undo 后把光标放回块内并用新 cache 重新打开应成功");
        check(cache.generation == warmGeneration + 1, "正文变化后的重开必须是冷重建");
        check(neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "undo 后新 cache 重新打开应为 current");

        check(Model::redoEdit(s.input), "redo 应生效");
        s.app.doc.text = s.input.text;
        neo::lp::cachedPlan(s.app.doc.text);
        focusMindmapBlock(s);  // redo 同样把光标设回文档尾部
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) == RejectReason::None,
              "redo 后把光标放回块内并用新 cache 重新打开应成功");
    }

    // ── 11. 锚点围栏身份重校验：同 revision 的外部回写把"顶级 2 空格围栏"变成
    //        列表续行，块 offset / 字节 / 缓存文本都足够新，旧 current 会误命中。 ──
    {
        const std::string fence =
            "  ```mermaid\n"
            "  mindmap\n"
            "    a\n"
            "  ```";
        const std::string prefixTop = "  text\n";    // 段落：围栏前一行不是列表
        const std::string prefixList = "- item\n";   // 列表项：同长度
        const std::string prefixPara = "  body\n";   // 又一个同长度段落（良性回写对照）
        check(prefixTop.size() == prefixList.size() && prefixTop.size() == prefixPara.size(),
              "fixture: 三个前缀必须同长度（块 offset 不变）");
        const std::string docTop = prefixTop + fence;
        const std::string docList = prefixList + fence;
        const std::string docPara = prefixPara + fence;

        Scenario s;
        s.app.newDocumentLanguage = "markdown";
        s.app.tabId = 77;
        s.app.path.clear();
        s.app.revision = 11;
        s.app.savedRevision = 11;
        s.input.text = docTop;
        s.input.textRevision = 5;
        s.input.cursor = static_cast<int>(prefixTop.size());  // 落在开围栏行（块内）
        s.app.doc.text = docTop;
        neo::lp::cachedPlan(docTop);

        PreviewState preview;
        Cache cache;
        check(openSuccess(preview, cache, s), "锚点身份用例前置：原文的顶级围栏应能打开");
        const std::size_t beg = cache.plan.sourceBeg;
        const std::size_t end = cache.plan.sourceEnd;
        const std::string src = cache.source;
        check(beg == prefixTop.size() && end == docTop.size(), "fixture: 块区间应紧跟前缀到文末");

        // 同 revision / 同 textRevision 的外部回写：段落前缀 → 列表前缀。
        externalWriteBack(s, docList);
        check(cache.plan.sourceBeg == beg && cache.plan.sourceEnd == end &&
                  std::string(docList.data() + beg, end - beg) == src,
              "fixture: 回写后块 offset 与字节均未变（source 仍是原样）");
        check(s.app.tabId == preview.tabId && s.app.path == preview.path &&
                  s.app.revision == preview.revision && s.input.textRevision == preview.textRevision &&
                  neo::lp::planCache().valid && neo::lp::planCache().text == s.app.doc.text &&
                  s.input.text == s.app.doc.text,
              "fixture: 旧 current 的全部廉价判据仍然成立（文本/身份都足够新）");
        check(!neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "同 revision 外部回写把顶级围栏变列表续行 → 必须 invalidate（锚点围栏身份重校验）");

        // 重开：列表容器吞掉了围栏 → 非顶级闭合块，selectTarget 以 InvalidFence
        // 拒绝（与 tests/unit/mindmap_plan.cpp 的列表续行 fixture 同一口径）。
        s.input.cursor = static_cast<int>(prefixList.size());
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) ==
                  RejectReason::InvalidFence,
              "顶级围栏变列表续行后重开应以 InvalidFence 拒绝");
        check(!preview.open &&
                  !neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "锚点拒绝后预览关闭且原缓存不得判为可展示");

        // 对照：同长度但仍是段落的良性回写，块仍为顶级 → 应保持 current（不误杀）。
        externalWriteBack(s, docPara);
        s.input.cursor = static_cast<int>(prefixPara.size());
        check(openSuccess(preview, cache, s), "良性回写后重开应成功");
        check(neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "同长度段落前缀的良性回写不得误杀（围栏仍是顶级闭合块）");
    }

    // ── 12. 同文档换块重开 / 不支持块拒绝后原缓存保留且不被展示 ────────────────
    {
        const std::string multiDoc =
            "```mermaid\nmindmap\n  root((甲))\n    甲一\n```\n"
            "```python\nx = 1\n```\n"
            "```mermaid\nmindmap\n  root((乙))\n    乙一\n```\n";
        Scenario s;
        s.app.newDocumentLanguage = "markdown";
        s.app.tabId = 91;
        s.app.revision = 3;
        s.app.savedRevision = 3;
        s.input.text = multiDoc;
        s.input.textRevision = 2;
        s.app.doc.text = multiDoc;
        neo::lp::cachedPlan(multiDoc);

        const std::size_t atJia = multiDoc.find("甲");
        const std::size_t atPython = multiDoc.find("python");
        const std::size_t atYi = multiDoc.find("乙");
        check(atJia != std::string::npos && atPython != std::string::npos &&
                  atYi != std::string::npos && atJia < atPython && atPython < atYi,
              "fixture: 三块按序存在");

        PreviewState preview;
        Cache cache;
        s.input.cursor = static_cast<int>(atJia);
        check(openSuccess(preview, cache, s), "换块用例前置：甲块应能打开");
        check(cache.plan.nodes[0].label == "甲", "fixture: 甲块根标签应为 甲");
        check(buildAndStore(cache, preview, makeStyle()), "换块用例前置：甲块应能入库");
        check(cache.valid, "换块用例前置：甲块布局应就绪");
        const unsigned long long generationJia = cache.generation;

        // 拒绝不支持块：只关预览；页缓存保留（键控，不会被展示），光标移回甲块
        // 立即暖开且布局零重建。
        s.input.cursor = static_cast<int>(atPython);
        check(neo::openMindmapModel(preview, cache, s.app, s.input, neo::lp::planCache()) ==
                  RejectReason::UnsupportedLanguage,
              "python 围栏应以 UnsupportedLanguage 拒绝");
        check(!preview.open &&
                  !neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "拒绝后预览关闭且原缓存不得判为可展示");
        check(cache.valid && cache.generation == generationJia &&
                  cache.plan.nodes[0].label == "甲",
              "拒绝不清页缓存（保留供暖开）");
        s.input.cursor = static_cast<int>(atJia);
        check(openSuccess(preview, cache, s), "移回甲块应暖开");
        check(cache.generation == generationJia && cache.valid &&
                  neo::mindmapCacheCurrent(cache, preview, makeStyle()),
              "暖开零重建：generation 不变、布局仍就绪且同键 current");
        preview.close();

        // 同文档换块重开：先按当前光标 selectTarget，旧块的键不再命中。
        s.input.cursor = static_cast<int>(atYi);
        check(openSuccess(preview, cache, s), "光标移到乙块重开应成功");
        check(cache.plan.nodes[0].label == "乙", "同文档换块必须显示光标所在块");
        check(cache.generation == generationJia + 1, "换块重开是冷重建");
        check(cache.key.blockBeg == cache.plan.sourceBeg &&
                  cache.key.blockBeg > 0,
              "键的块区间应指向乙块（不再是旧块）");
        check(cache.valid == false, "冷重建后布局未建，valid 应为 false");
        check(neo::mindmapModelCurrent(preview, cache, s.app, s.input, neo::lp::planCache()),
              "冷开未布局的模型应判为 current（由 mindmapCacheCurrent 决定重建）");
        check(!neo::mindmapCacheCurrent(cache, preview, makeStyle()),
              "布局未就绪时缓存不得判为 current");
    }

    // ── 13. 活动预算守卫：超 mindmapActiveBudgetBytes 的结果整体释放并回退 ─────
    {
        check(std::getenv("NEO_MINDMAP_CACHE_MIB") == nullptr,
              "fixture: 预算环境变量应未设置");
#if defined(_WIN32)
        _putenv_s("NEO_MINDMAP_CACHE_MIB", "1");  // 压到 1 MiB 便于构造超限样例
#else
        setenv("NEO_MINDMAP_CACHE_MIB", "1", 1);
#endif

        Scenario s;
        buildScenario(s);
        PreviewState preview;
        Cache cache;
        check(openSuccess(preview, cache, s), "预算用例前置：应能打开");

        // 合成一棵超过 1 MiB 的树（20001 节点：1 根 depth=0 + 2 万直接子节点 depth=1）。
        cache.plan.nodes.assign(20001, neo::mindmap::Node{});
        cache.plan.nodes[0].children.resize(20000);
        for (std::size_t index = 1; index <= 20000; ++index) {
            cache.plan.nodes[0].children[index - 1] = static_cast<int>(index);
            cache.plan.nodes[index].parent = 0;
            cache.plan.nodes[index].depth = 1;
        }
        std::vector<neo::mindmap::Size> sizes(cache.plan.nodes.size(),
                                              neo::mindmap::Size{40.0f, 22.0f});
        neo::mindmap::Options options;
        options.maxNodes = 40000;
        neo::mindmap::Layout layout = neo::mindmap::buildLayout(cache.plan, sizes, options);
        check(layout.valid, "fixture: 合成大树应能通过布局");
        const std::size_t bigBytes = neo::mindmap::estimateCacheBytes(cache);
        check(bigBytes > 1024ull * 1024ull, "fixture: 合成样例应超过 1 MiB 预算");

        check(!neo::storeMindmapCache(cache, preview, makeStyle(), std::move(sizes), std::move(layout)),
              "超过活动预算的入库必须失败（回退保留原文）");
        check(!cache.valid && cache.plan.nodes.empty() && cache.plan.nodes.capacity() == 0 &&
                  noStringResident(cache.source),
              "超限回退必须整体释放缓存（不留半套结果）");
        check(preview.open, "预算回退不改变 preview 状态（关闭由 UI 层负责）");

#if defined(_WIN32)
        _putenv_s("NEO_MINDMAP_CACHE_MIB", "");
#else
        unsetenv("NEO_MINDMAP_CACHE_MIB");
#endif
    }

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("mindmap preview model (open/store/close/invalidate/budget) passed\n");
    return 0;
}
