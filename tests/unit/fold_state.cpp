// R8：查找命中展开折叠祖先 + 折叠状态的统一治理（指引 §4 R8 的必补行为测试）。
//
// 这些用例打的是**应用层入口逻辑**，不是 lp 折叠模型本身：
//   · `3.` 折叠 H1+H2 后查找普通正文 → 全部祖先展开（旧实现从 parentHeadingLine 起，
//     普通正文该字段恒 -1，所以原来根本展开不了）；
//   · 命中标题行自身 / setext 下划线行 → 不展开标题自己（它本来就可见）；
//   · 文本改动有可信 delta → 折叠键平移；无 delta / 一帧跨多次编辑 → 清空并展开；
//   · undo 后同样按 delta 反向平移（同一条落点）；
//   · 换文档（标题偏移碰巧相同）→ 不复用旧折叠键；
//   · plan 未就绪 → 命中偏移排队，plan 就绪那一帧再展开。

#include "components/input_model.h"
#include "model/fold_state.h"
#include "model/lp_decorations.h"
#include "state/app_state.h"
#include "ui/find_bar.h"

#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

using Model = components::input_detail::InputModel;
using InputState = Model::InputState;

std::set<int> keys(std::initializer_list<int> values) { return std::set<int>(values); }

// 备好"应用状态 + 输入状态 + 计划缓存"三者一致的场景（与 compose 里的顺序同源：
// 先有 doc.text，再由装饰层调 cachedPlan 建缓存）。
void prepare(neo::AppState& appState, InputState& input, const std::string& doc) {
    input.text = doc;
    input.textRevision += 1;
    Model::clearPendingEdit(input);
    appState.doc.text = doc;
    appState.foldsTextRevision = input.textRevision;
    appState.foldedHeadings.clear();
    neo::lp::cachedPlan(doc);
}

// 做一次真实编辑：beginEdit/endEdit 是组件的公共漏斗，会推进 textRevision 并写下
// 可信的 PendingTextEdit（应用层对账就靠它）。
void insertAt(InputState& input, int at, const std::string& text) {
    Model::beginEdit(input, at, at);
    input.text.insert(static_cast<std::size_t>(at), text);
    input.cursor = at + static_cast<int>(text.size());
    Model::endEdit(input);
}

} // namespace

int main() {
    // ── 1. mapFoldedKeys 的纯规则 ────────────────────────────────────────────
    {
        components::input_detail::PendingTextEdit edit;
        edit.valid = true;
        edit.revision = 2;
        edit.byteBeg = 10;
        edit.oldEnd = 12;
        edit.newEnd = 15;  // 2 字节换成 5 字节 → delta = +3

        std::set<int> inside{0, 11, 20};
        check(!neo::foldstate::mapFoldedKeys(inside, edit),
              "有键落在被替换区间内部 → 必须报告失败（要求调用方清空）");
        check(inside == keys({0, 11, 20}), "报告失败时不得改动键集合（半映射比不映射更危险）");

        std::set<int> clean{0, 9, 12, 20};
        check(neo::foldstate::mapFoldedKeys(clean, edit), "无键落在区间内部 → 应成功映射");
        check(clean == keys({0, 9, 15, 23}),
              "起点之前的键不动、其余按 delta=+3 平移（实际 " + std::to_string(clean.size()) + " 项）");

        components::input_detail::PendingTextEdit pure;
        pure.valid = true;
        pure.revision = 3;
        pure.byteBeg = 5;
        pure.oldEnd = 5;
        pure.newEnd = 8;  // 纯插入 3 字节
        std::set<int> inserted{0, 5, 40};
        check(neo::foldstate::mapFoldedKeys(inserted, pure), "纯插入不做否定判断");
        check(inserted == keys({0, 5, 43}),
              "纯插入：编辑点处的键不动（行首没变），其后的键整体 +3");
    }

    // ── 2. sectionAncestorKeys：命中行的祖先链 ───────────────────────────────
    {
        const std::string doc = "# 一级\n\n## 二级\n\n正文命中\n";
        const neo::LpPlan& plan = neo::lp::cachedPlan(doc);
        const std::size_t body = doc.find("正文命中");
        const std::vector<int> ancestors =
            neo::foldstate::sectionAncestorKeys(plan, static_cast<int>(body));
        check(ancestors.size() == 2, "正文行应有两级祖先标题（实际 " +
                                         std::to_string(ancestors.size()) + "）");
        check(!ancestors.empty() && ancestors.front() == plan.lines[2].srcBeg,
              "祖先链由内到外：先二级标题");
        check(ancestors.size() >= 2 && ancestors[1] == plan.lines[0].srcBeg,
              "再一级标题");

        // 命中标题自己的文本行：标题行可见，只需要展开它的父标题
        const std::size_t onHeading = doc.find("二级");
        const std::vector<int> headingAncestors =
            neo::foldstate::sectionAncestorKeys(plan, static_cast<int>(onHeading));
        check(headingAncestors.size() == 1 && headingAncestors[0] == plan.lines[0].srcBeg,
              "命中标题自身：只列父标题，不含它自己");
    }
    {
        const std::string doc = "标题\n=====\n\n正文\n";
        const neo::LpPlan& plan = neo::lp::cachedPlan(doc);
        const std::size_t underline = doc.find("=====");
        check(neo::foldstate::sectionAncestorKeys(plan, static_cast<int>(underline)).empty(),
              "setext 下划线行命中：不展开标题自己（下划线行本来可见）");
    }

    // ── 3. 折叠 H1+H2 后查找普通正文 → 全部祖先展开 ──────────────────────────
    {
        neo::AppState appState;
        InputState input;
        const std::string doc = "# 一级\n\n## 二级\n\n正文命中\n";
        prepare(appState, input, doc);
        const neo::LpPlan& plan = neo::lp::cachedPlan(doc);
        const int h1 = plan.lines[0].srcBeg;
        const int h2 = plan.lines[2].srcBeg;
        appState.foldedHeadings = keys({h1, h2});

        const std::size_t body = doc.find("正文命中");
        const int bodyEnd = static_cast<int>(body) + 12;  // 4 个汉字 = 12 字节
        appState.findMatches = {{static_cast<int>(body), bodyEnd}};
        neo::selectFindMatch(input, appState, 0);

        check(appState.foldedHeadings.empty(),
              "折叠两级标题后查找正文：两个祖先都必须展开（旧实现一个都不展开）");
        check(input.selectionStart == static_cast<int>(body) && input.cursor == bodyEnd,
              "命中区间同时落到选择与光标上");
    }

    // ── 4. 命中标题自身：展开父标题、保留它自己的折叠 ───────────────────────
    {
        neo::AppState appState;
        InputState input;
        const std::string doc = "# 一级\n\n## 二级\n\n正文\n";
        prepare(appState, input, doc);
        const neo::LpPlan& plan = neo::lp::cachedPlan(doc);
        const int h1 = plan.lines[0].srcBeg;
        const int h2 = plan.lines[2].srcBeg;
        appState.foldedHeadings = keys({h1, h2});
        const std::size_t onHeading = doc.find("二级");
        appState.findMatches = {{static_cast<int>(onHeading), static_cast<int>(onHeading) + 6}};
        neo::selectFindMatch(input, appState, 0);
        check(appState.foldedHeadings == keys({h2}),
              "命中标题自身：只展开父标题，标题自己的折叠保留（它那行始终可见）");
    }

    // ── 5. 文本改动的对账：delta 平移 / 无 delta 清空 / undo 反平移 ──────────
    {
        neo::AppState appState;
        InputState input;
        const std::string doc = "# 一级\n\n## 二级\n\n正文\n";
        prepare(appState, input, doc);
        const neo::LpPlan& plan = neo::lp::cachedPlan(doc);
        const int h2 = plan.lines[2].srcBeg;
        appState.foldedHeadings = keys({h2});

        insertAt(input, 0, "abc");  // 文档头插入 3 字节
        neo::syncFoldsWithEditorText(appState, input);
        check(appState.foldedHeadings == keys({h2 + 3}),
              "有可信 delta：折叠键按 +3 平移（折叠态在打字时不该被清掉）");

        check(Model::undoEdit(input), "撤销应生效");
        neo::syncFoldsWithEditorText(appState, input);
        check(appState.foldedHeadings == keys({h2}),
              "undo 走同一条落点：折叠键按反向 delta 平移回原位");

        // 无 delta（外部赋值 / 组件外改写）：只能清空
        input.text.insert(0, "zz");
        input.textRevision += 1;
        Model::clearPendingEdit(input);  // 断链 = 没有可信区间
        neo::syncFoldsWithEditorText(appState, input);
        check(appState.foldedHeadings.empty(), "没有可信 delta：清空并展开（宁可不保守）");
        check(appState.foldsTextRevision == input.textRevision, "对账代次跟着推进，不重复对账");
    }
    {
        neo::AppState appState;
        InputState input;
        const std::string doc = "# 一级\n\n## 二级\n\n正文\n";
        prepare(appState, input, doc);
        const neo::LpPlan& plan = neo::lp::cachedPlan(doc);
        const int h2 = plan.lines[2].srcBeg;
        appState.foldedHeadings = keys({h2});
        // 一帧里发生两次编辑（代次 +2）：链断了，证明不了 → 清空
        insertAt(input, 0, "a");
        insertAt(input, 1, "b");
        neo::syncFoldsWithEditorText(appState, input);
        check(appState.foldedHeadings.empty(),
              "一帧跨多次编辑（代次差 >1）：不做半可信映射，直接清空");
    }

    // ── 6. 换文档：清折叠态，且不复用"偏移碰巧相同"的旧键 ────────────────────
    {
        neo::AppState appState;
        InputState input;
        prepare(appState, input, "# 同名标题\n\n正文\n");
        appState.foldedHeadings = keys({0});
        appState.pendingFindRevealByte = 3;

        input.textRevision += 10;  // 换文档：组件侧 loadDocument 的等价效果
        neo::clearFoldsForDocumentSwitch(appState, input);
        check(appState.foldedHeadings.empty() && appState.pendingFindRevealByte < 0 &&
                  appState.foldsTextRevision == input.textRevision,
              "换文档：折叠态与排队展开一并清掉，对账代次对齐到新代次");

        // B 文档的标题偏移与 A 相同（都是 0）—— 也不许复用旧折叠态
        const std::string docB = "# 另一个标题\n\n正文\n";
        input.text = docB;
        neo::lp::cachedPlan(docB);
        neo::syncFoldsWithEditorText(appState, input);
        check(appState.foldedHeadings.empty(),
              "切到标题偏移相同的新文档后仍不折叠（旧键不复用）");
    }

    // ── 7. plan 未就绪：命中偏移排队，plan 就绪那一帧再展开 ───────────────────
    {
        neo::AppState appState;
        InputState input;
        const std::string doc = "# 一级\n\n## 二级\n\n正文\n";
        prepare(appState, input, doc);
        const neo::LpPlan& plan = neo::lp::cachedPlan(doc);
        const int h1 = plan.lines[0].srcBeg;
        const int h2 = plan.lines[2].srcBeg;
        appState.foldedHeadings = keys({h1, h2});

        neo::lp::invalidatePlanCache();  // plan 不可用（刚换文档 / 缓存失效）
        const std::size_t body = doc.find("正文");
        appState.findMatches = {{static_cast<int>(body), static_cast<int>(body) + 6}};
        neo::selectFindMatch(input, appState, 0);
        check(appState.pendingFindRevealByte == static_cast<int>(body),
              "plan 未就绪：命中偏移进排队，不抢先做无 delta 解析");
        check(appState.foldedHeadings == keys({h1, h2}), "plan 未就绪：折叠态一帧都不动");

        neo::lp::cachedPlan(doc);  // plan 就绪（下一帧装饰层会做这件事）
        check(!neo::applyPendingFindReveal(appState, input),
              "plan 就绪后一次性消费排队（不再需要下一帧）");
        check(appState.foldedHeadings.empty() && appState.pendingFindRevealByte < 0,
              "排队展开在同一帧生效并清空 pending");
    }

    // Live query refinement must stay on the same match, rather than moving
    // from the previous selection's end on every typed character.
    {
        neo::AppState appState;
        InputState input;
        prepare(appState, input, "Alpha alpha ALPHA alphabet");
        input.cursor = input.selectionStart = input.selectionEnd = 0;
        appState.findQuery = "A";
        neo::refreshFindMatches(appState, input);
        check(input.selectionStart == 0, "first query selects the first match");
        appState.findQuery = "Alpha";
        neo::refreshFindMatches(appState, input);
        check(input.selectionStart == 0 && input.selectionEnd == 5,
              "refining a query preserves its match start");
        check(appState.findMatches.size() == 4, "default search ignores case");
        appState.findMatchCase = true;
        neo::refreshFindMatches(appState, input);
        check(appState.findMatches.size() == 1, "case toggle invalidates match cache");
        appState.findMatchCase = false;
        appState.findWholeWord = true;
        neo::refreshFindMatches(appState, input);
        check(appState.findMatches.size() == 3, "whole-word toggle excludes alphabet");
        appState.findQuery = "missing";
        neo::refreshFindMatches(appState, input);
        check(appState.findCurrent == -1 && input.selectionStart == input.selectionEnd,
              "zero matches clears the previous match selection");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "R8 fold/find behaviour passed\n";
    return 0;
}
