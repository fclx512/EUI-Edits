// apps/neo_editor/model/mindmap_plan.cpp 的无头测试。
//
// 验证的是编辑器真正要用的那条路：真实 buildLpPlan 的 LpPlan → selectTarget
// 定位 cursor 所在的闭合 mermaid 围栏 → 从 doc.text 切片 → parseBlock 解析。
// 判据（全部用 printf/check，不用 assert —— Release 下 assert 会被 NDEBUG 吃掉）：
//   1. selectTarget：cursor 越界、open/inner/closing cursor、两块各自选中、非块/
//      未闭合/错误围栏/非 mermaid 语言/嵌套/空计划的拒绝与稳定 rejectReason；
//      不扫描全文挑图。
//   2. parseBlock：接受子集（裸标签 / ((label)) / [label] / 显式 id）的树结构、层级、
//      形状、id、source 字节区间（切片 == label）；注释/指令；UTF-8 中文与 emoji。
//   3. 每一个拒绝分支：rejectReason、清空整树、sourceLine 稳定。
//   4. 规模上限：码点/深度/节点数/源码字节；estimatePlanBytes 计 capacity/string/children。
//
// 构建约束：随主工程 EUI_BUILD_TEST_FIXTURES=ON 一起编；CMake 需给本目标编入
// apps/neo_editor/model/mindmap_plan.cpp 与 lp_plan.cpp（及 apps/neo_editor include）。
// 只编旧 lp_plan.cpp 不等于测到新代码。

#include "model/mindmap_plan.h"
#include "model/lp_plan.h"

#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("  !! %s\n", what.c_str());
    }
}

std::string slice(const std::string& text, std::size_t beg, std::size_t end) {
    if (end < beg || end > text.size()) {
        return {};
    }
    return text.substr(beg, end - beg);
}

std::size_t findByte(const std::string& text, const std::string& needle, std::size_t from = 0) {
    const std::size_t at = text.find(needle, from);
    return at == std::string::npos ? text.size() : at;
}

std::string fenceBlock(const std::string& body, const char* lang = "mermaid") {
    return std::string("```") + lang + "\n" + body + "\n```";
}

std::string reasonName(neo::mindmap::RejectReason reason) {
    return neo::mindmap::rejectReasonName(reason);
}

// 拒绝：断言 reason、整树清空、有稳定 sourceLine。
void expectReject(const neo::mindmap::ParseResult& result, neo::mindmap::RejectReason want,
                  const std::string& what) {
    check(result.rejectReason == want,
          what + "：应为 " + reasonName(want) + "，实际 " + reasonName(result.rejectReason));
    check(result.plan.nodes.empty(), what + "：拒绝必须清空整棵树（实际 " +
                                          std::to_string(result.plan.nodes.size()) + " 节点）");
    check(!static_cast<bool>(result), what + "：失败结果 operator bool 必须为 false");
}

// ── 样例文档（正文故意用项目名，含中文）────────────────────────────────────
const char* const kDoc =
    "# 标题\n"
    "\n"
    "前言段落。\n"
    "\n"
    "```mermaid\n"
    "mindmap\n"
    "  root((EUI-Edits))\n"
    "    编辑区\n"
    "      输入\n"
    "      Live Preview\n"
    "    文档\n"
    "      多标签\n"
    "      恢复\n"
    "```\n"
    "\n"
    "尾声。\n";

void testRejectReasonNames() {
    using neo::mindmap::RejectReason;
    check(std::string(neo::mindmap::rejectReasonName(RejectReason::None)) == "none", "reasonName None");
    check(std::string(neo::mindmap::rejectReasonName(RejectReason::UnclosedFence)) == "unclosed_fence",
          "reasonName UnclosedFence");
    check(std::string(neo::mindmap::rejectReasonName(RejectReason::DuplicateId)) == "duplicate_id",
          "reasonName DuplicateId");
    check(std::string(neo::mindmap::rejectReasonName(RejectReason::NodeLimit)) == "node_limit",
          "reasonName NodeLimit");
    check(std::string(neo::mindmap::rejectReasonName(static_cast<RejectReason>(999))) == "unknown",
          "reasonName 未知枚举应稳定返回 unknown");
}

void testSelectTargetBasic() {
    using namespace neo;
    const std::string doc = kDoc;
    const LpPlan plan = buildLpPlan(doc);
    check(!plan.lines.empty(), "样例计划应有行");

    const std::size_t openBeg = findByte(doc, "```mermaid");
    const std::size_t closeTick = doc.rfind("```");
    const std::size_t closeEnd = closeTick + 3;

    // open / inner / closing cursor 都应选到同一个闭合块。
    const std::size_t inner = findByte(doc, "编辑区");
    const std::size_t onOpen = openBeg + 1;
    const std::size_t onClose = closeTick + 1;
    for (const auto& [cursor, label] : std::vector<std::pair<std::size_t, const char*>>{
             {onOpen, "open"}, {inner, "inner"}, {onClose, "closing"}}) {
        const mindmap::Target target = mindmap::selectTarget(doc, plan, static_cast<int>(cursor));
        check(static_cast<bool>(target), std::string("cursor 在") + label + "围栏/内部应命中目标块");
        check(target.blockBeg == static_cast<int>(openBeg) &&
                  target.blockEnd == static_cast<int>(closeEnd),
              std::string("cursor 在") + label + "时应切出同一块区间");
    }

    // 非块光标 → NoBlock。
    const std::size_t prose = findByte(doc, "前言段落");
    const mindmap::Target noBlock = mindmap::selectTarget(doc, plan, static_cast<int>(prose));
    check(noBlock.rejectReason == mindmap::RejectReason::NoBlock, "段落光标应为 no_block");

    // 空计划 → PlanNotReady。
    const mindmap::Target notReady = mindmap::selectTarget(doc, LpPlan{}, 0);
    check(notReady.rejectReason == mindmap::RejectReason::PlanNotReady, "空计划应为 plan_not_ready");

    // cursor 越界：绝不能让 lineIndexFor(-1/-越界) 夹到首/末行而误选远处的块。
    const mindmap::Target neg = mindmap::selectTarget(doc, plan, -1);
    check(neg.rejectReason == mindmap::RejectReason::NoBlock && neg.blockBeg == -1,
          "cursor=-1 应拒绝且不选首行块");
    const mindmap::Target past = mindmap::selectTarget(doc, plan, static_cast<int>(doc.size()) + 1);
    check(past.rejectReason == mindmap::RejectReason::NoBlock && past.blockBeg == -1,
          "cursor 超出 text 应拒绝且不选末行块");
}

void testSelectTargetTwoBlocks() {
    using namespace neo;
    const std::string doc =
        "```mermaid\n"
        "mindmap\n"
        "  first\n"
        "```\n"
        "\n"
        "中间文字\n"
        "\n"
        "```python\n"
        "x = 1\n"
        "```\n"
        "\n"
        "```mermaid\n"
        "mindmap\n"
        "  second\n"
        "```\n";
    const LpPlan plan = buildLpPlan(doc);

    // cursor 在第二个 mermaid 块 → 必须选第二个（不得自动挑第一个导图）。
    const std::size_t secondCursor = findByte(doc, "second");
    const mindmap::Target second = mindmap::selectTarget(doc, plan, static_cast<int>(secondCursor));
    check(static_cast<bool>(second), "第二块 cursor 应命中");
    check(second.blockBeg == static_cast<int>(doc.rfind("```mermaid")),
          "第二块 cursor 应选它自己那块（rfind），不得扫描全文挑第一个");

    // cursor 在 python 块 → UnsupportedLanguage（不在 mermaid 块里就完全不介入）。
    const std::size_t pyCursor = findByte(doc, "x = 1");
    const mindmap::Target py = mindmap::selectTarget(doc, plan, static_cast<int>(pyCursor));
    check(py.rejectReason == mindmap::RejectReason::UnsupportedLanguage, "python 围栏应为 unsupported_language");

    // 第一个块的 cursor 仍命中第一块。
    const std::size_t firstCursor = findByte(doc, "first");
    const mindmap::Target first = mindmap::selectTarget(doc, plan, static_cast<int>(firstCursor));
    check(static_cast<bool>(first) && first.blockBeg == 0, "第一块 cursor 应命中第一块");
}

void testSelectTargetFenceRejects() {
    using namespace neo;
    // 未闭合围栏。
    {
        const std::string doc = "```mermaid\nmindmap\n  a\n";
        const LpPlan plan = buildLpPlan(doc);
        const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "a")));
        check(t.rejectReason == mindmap::RejectReason::UnclosedFence, "未闭合围栏应为 unclosed_fence");
    }
    // 错误围栏：开 4 个反引号、闭 3 个 → 不算闭合。
    {
        const std::string doc = "````mermaid\nmindmap\n  a\n```\n";
        const LpPlan plan = buildLpPlan(doc);
        const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "a")));
        check(t.rejectReason == mindmap::RejectReason::UnclosedFence,
              "闭围栏短于开围栏应为 unclosed_fence");
    }
    // 非 mermaid 语言。
    {
        const std::string doc = "```text\nmindmap\n  a\n```\n";
        const LpPlan plan = buildLpPlan(doc);
        const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "a")));
        check(t.rejectReason == mindmap::RejectReason::UnsupportedLanguage, "非 mermaid 语言应拒绝");
    }
    // 大小写敏感：Mermaid 不算。
    {
        const std::string doc = "```Mermaid\nmindmap\n  a\n```\n";
        const LpPlan plan = buildLpPlan(doc);
        const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "a")));
        check(t.rejectReason == mindmap::RejectReason::UnsupportedLanguage, "语言大小写按本实现口径拒绝");
    }
    // 引号内嵌围栏：lp_plan 不把它当围栏 → NoBlock（嵌套由上层/选择层挡住）。
    {
        const std::string doc = "> ```mermaid\n> mindmap\n>   a\n> ```\n";
        const LpPlan plan = buildLpPlan(doc);
        const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "mindmap")));
        check(t.rejectReason == mindmap::RejectReason::NoBlock, "引用内围栏应为 no_block");
    }
    // 列表续行缩进到项正文列的围栏属列表项内部，非顶级 → 拒绝（真实 buildLpPlan）。
    {
        const std::string doc = "- item\n  ```mermaid\n  mindmap\n    root\n  ```\n";
        const LpPlan plan = buildLpPlan(doc);
        const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "root")));
        check(t.rejectReason == mindmap::RejectReason::InvalidFence, "列表续行缩进围栏应拒绝（非顶级）");
    }
    // 开/闭围栏字符不配、闭围栏带 info：真实 LP 都不是闭合块 → unclosed_fence。
    {
        const std::string doc = "~~~mermaid\nmindmap\n  a\n```\n";
        const LpPlan plan = buildLpPlan(doc);
        const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "a")));
        check(t.rejectReason == mindmap::RejectReason::UnclosedFence, "开~~~闭```不配应为 unclosed_fence");
    }
    {
        const std::string doc = "```mermaid\nmindmap\n  a\n``` trailing\n";
        const LpPlan plan = buildLpPlan(doc);
        const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "a")));
        check(t.rejectReason == mindmap::RejectReason::UnclosedFence, "闭围栏带 info 应为 unclosed_fence");
    }
}

void testSelectTargetHostilePlan() {
    using namespace neo;
    const std::string doc = kDoc;
    LpPlan plan = buildLpPlan(doc);
    const int inner = static_cast<int>(findByte(doc, "编辑区"));
    const int idx = plan.lineIndexFor(inner);
    check(idx >= 0, "恶意计划：cursor 行可定位");
    {
        LpPlan bad = plan;
        bad.lines[idx].codeBlockEnd = static_cast<int>(doc.size()) + 100000;
        const mindmap::Target t = mindmap::selectTarget(doc, bad, inner);
        check(t.rejectReason == mindmap::RejectReason::InvalidFence && t.blockBeg == -1,
              "越界 codeBlockEnd 应拒绝且不越界扫描");
    }
    {
        LpPlan bad = plan;
        bad.lines[idx].codeBlockBeg = bad.lines[idx].codeBlockEnd;  // beg >= end
        const mindmap::Target t = mindmap::selectTarget(doc, bad, inner);
        check(t.rejectReason == mindmap::RejectReason::InvalidFence, "beg>=end 应拒绝");
    }
    {
        LpPlan bad = plan;
        const int oi = bad.lineIndexFor(bad.lines[idx].codeBlockBeg);
        bad.lines[oi].srcEnd = static_cast<int>(doc.size()) + 50;
        const mindmap::Target t = mindmap::selectTarget(doc, bad, inner);
        check(t.rejectReason == mindmap::RejectReason::InvalidFence, "开围栏 srcEnd 越界应拒绝");
    }
}

void testSelectTargetTab() {
    using namespace neo;
    const std::string doc = "```mermaid\t\nmindmap\n  a\n```\n";
    const LpPlan plan = buildLpPlan(doc);
    const mindmap::Target t = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "a")));
    check(t.rejectReason == mindmap::RejectReason::InvalidFence, "围栏块含 Tab 应保守拒绝");
}

void testParseCanonical() {
    using namespace neo;
    const std::string doc = kDoc;
    const LpPlan plan = buildLpPlan(doc);
    const std::size_t openBeg = findByte(doc, "```mermaid");
    const std::size_t closeEnd = doc.rfind("```") + 3;
    const mindmap::Target target = mindmap::selectTarget(doc, plan, static_cast<int>(findByte(doc, "编辑区")));
    check(static_cast<bool>(target), "canonical：selectTarget 应命中");

    const std::string block = slice(doc, static_cast<std::size_t>(target.blockBeg), static_cast<std::size_t>(target.blockEnd));
    const mindmap::ParseResult parsed =
        mindmap::parseBlock(block, static_cast<std::size_t>(target.blockBeg));
    check(static_cast<bool>(parsed), "canonical：parseBlock 应成功");

    const auto& nodes = parsed.plan.nodes;
    check(nodes.size() == 7, "canonical：应有 7 个节点，实际 " + std::to_string(nodes.size()));
    if (nodes.size() != 7) {
        return;
    }
    check(nodes[0].label == "EUI-Edits" && nodes[0].shape == mindmap::Shape::Circle, "根标签/形状");
    check(nodes[0].id == "root", "root((EUI-Edits)) 的 id = root（标签才是 EUI-Edits）");
    check(nodes[0].parent == -1 && nodes[0].depth == 0, "根 parent/depth");
    check(nodes[0].children == (std::vector<int>{1, 4}), "根 children 顺序");
    check(nodes[1].label == "编辑区" && nodes[1].depth == 1 && nodes[1].parent == 0, "编辑区层级");
    check(nodes[1].children == (std::vector<int>{2, 3}), "编辑区 children");
    check(nodes[2].label == "输入" && nodes[2].depth == 2 && nodes[2].parent == 1, "输入层级");
    check(nodes[3].label == "Live Preview" && nodes[3].depth == 2, "Live Preview 完整标签");
    check(nodes[4].label == "文档" && nodes[4].children == (std::vector<int>{5, 6}), "文档子树");
    check(nodes[5].label == "多标签" && nodes[6].label == "恢复", "叶子标签");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        check(nodes[i].shape == mindmap::Shape::Rectangle || nodes[i].shape == mindmap::Shape::Circle,
              "形状取值合法");
        if (i != 0) {
            check(nodes[i].id.empty(), "裸标签子节点的 id 应为空");
        }
    }
    check(parsed.plan.sourceBeg == static_cast<std::size_t>(target.blockBeg), "plan.sourceBeg = 块起点");
    check(parsed.plan.sourceEnd == static_cast<std::size_t>(target.blockEnd), "plan.sourceEnd = 块终点");
    check(parsed.sourceLine == 0, "成功解析 sourceLine = 0");

    // source 区间：切片必须逐字节等于标签（完整、不截断、不清洗）。
    for (const auto& node : nodes) {
        const std::string label = slice(doc, node.sourceBeg, node.sourceEnd);
        check(label == node.label, "source 区间切片应等于标签（实际 \"" + label + "\"）");
    }
    check(slice(doc, nodes[0].sourceBeg, nodes[0].sourceEnd) == "EUI-Edits",
          "圆形包装内的标签区间应只覆盖标签本身");
    (void)openBeg;
    (void)closeEnd;
}

void testParseIdsAndShapes() {
    using namespace neo;
    const std::string doc =
        "```mermaid\n"
        "mindmap\n"
        "  root((Center))\n"
        "    a1[Alpha]\n"
        "    b1((Beta))\n"
        "    [Gamma]\n"
        "```\n";
    const mindmap::ParseResult parsed = mindmap::parseBlock(doc, 0);
    check(static_cast<bool>(parsed), "id/shape：应成功，实际 " + reasonName(parsed.rejectReason));
    if (!static_cast<bool>(parsed)) {
        return;
    }
    const auto& n = parsed.plan.nodes;
    check(n.size() == 4, "id/shape：4 个节点");
    if (n.size() != 4) {
        return;
    }
    check(n[0].id == "root" && n[0].shape == mindmap::Shape::Circle && n[0].label == "Center",
          "root((Center)) → id=root / Circle / label=Center");
    check(n[1].id == "a1" && n[1].shape == mindmap::Shape::Rectangle && n[1].label == "Alpha", "id[label] → Rectangle");
    check(n[2].id == "b1" && n[2].shape == mindmap::Shape::Circle && n[2].label == "Beta", "id((label)) → Circle");
    check(n[3].id.empty() && n[3].shape == mindmap::Shape::Rectangle && n[3].label == "Gamma", "[label]（id 省略）→ Rectangle");
    check(n[0].children == (std::vector<int>{1, 2, 3}), "三个兄弟挂在根下");
    for (const auto& node : n) {
        check(slice(doc, node.sourceBeg, node.sourceEnd) == node.label, "id/shape：source 切片 == 标签");
    }
}

void testParseCommentsAndUtf8() {
    using namespace neo;
    const std::string doc =
        "```mermaid\n"
        "%% 头部注释\n"
        "mindmap\n"
        "  %% 行内注释\n"
        "  根节点😀\n"
        "    子节点\n"
        "```\n";
    const mindmap::ParseResult parsed = mindmap::parseBlock(doc, 0);
    check(static_cast<bool>(parsed), "注释/emoji：应成功，实际 " + reasonName(parsed.rejectReason));
    if (!static_cast<bool>(parsed)) {
        return;
    }
    const auto& n = parsed.plan.nodes;
    check(n.size() == 2, "注释/emoji：2 个节点，实际 " + std::to_string(n.size()));
    if (n.size() != 2) {
        return;
    }
    check(n[0].label == "根节点😀", "emoji 标签完整保留（实际 \"" + n[0].label + "\"）");
    check(n[1].label == "子节点", "中文子节点");
    check(slice(doc, n[0].sourceBeg, n[0].sourceEnd) == "根节点😀", "emoji source 切片完整");
}

void testParseRejects() {
    using namespace neo;
    using RR = mindmap::RejectReason;

    expectReject(mindmap::parseBlock(fenceBlock("nodes\n  root"), 0), RR::NotMindmap, "首行非 mindmap");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap"), 0), RR::EmptyMap, "只有 mindmap 无节点");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  a\n  b"), 0), RR::MultipleRoots, "第二个根级节点");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  a\n      b\n    c"), 0), RR::InvalidIndent,
                 "缩进回退未命中已有层");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  a\n\tb"), 0), RR::InvalidIndent, "含 Tab 的行");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  %%{init}%%\n  root"), 0), RR::UnsupportedSyntax,
                 "%%{ 指令");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  ::icon(fa)"), 0), RR::UnsupportedSyntax, "::icon");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  classDef red fill:#f00"), 0), RR::UnsupportedSyntax,
                 "classDef 保留指令");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  style red"), 0), RR::UnsupportedSyntax,
                 "style 保留指令");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  click a"), 0), RR::UnsupportedSyntax,
                 "click 保留指令");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  linkStyle 0"), 0), RR::UnsupportedSyntax,
                 "linkStyle 保留指令");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  icon x"), 0), RR::UnsupportedSyntax,
                 "icon 保留指令");
    // :: 在任意位置（不止行首）都要拒绝；边声明与成对 Markdown 标记同理。
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  Child ::foo"), 0), RR::UnsupportedSyntax,
                 ":: 不在行首也应拒绝");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  A---B"), 0), RR::UnsupportedSyntax, "边声明 ---");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  A--B"), 0), RR::UnsupportedSyntax, "边声明 --");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  A -.- B"), 0), RR::UnsupportedSyntax, "边声明 -.-");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  A -.o B"), 0), RR::UnsupportedSyntax, "边声明 -.o");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  A === B"), 0), RR::UnsupportedSyntax, "边声明 ===");
    // mindmap 标题行之前的 %%{ 指令同样拒绝（不只看首个内容节点）。
    expectReject(mindmap::parseBlock(fenceBlock("%%{init}%%\nmindmap\n  root"), 0), RR::UnsupportedSyntax,
                 "mindmap 前 %%{");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  :::theme"), 0), RR::UnsupportedSyntax, "::: 指令");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  _x_"), 0), RR::UnsupportedSyntax, "成对下划线");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  ~~x~~"), 0), RR::UnsupportedSyntax, "删除线");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  !x"), 0), RR::UnsupportedSyntax, "embed 前缀");
    // 普通英文 underscore 是合法裸标签，不得误伤。
    {
        const mindmap::ParseResult ok = mindmap::parseBlock(fenceBlock("mindmap\n  foo_bar"), 0);
        check(static_cast<bool>(ok) && ok.plan.nodes.size() == 1 && ok.plan.nodes[0].label == "foo_bar",
              "foo_bar 应作为普通裸标签接受");
    }
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  <b>bold</b>"), 0), RR::UnsupportedSyntax, "HTML 标签");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  `code`"), 0), RR::UnsupportedSyntax, "反引号");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  (curved)"), 0), RR::UnsupportedSyntax, "单括号包装不支持");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  {brace}"), 0), RR::UnsupportedSyntax, "花括号包装不支持");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  A --> B"), 0), RR::UnsupportedSyntax, "边声明");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  a(b)"), 0), RR::UnsupportedSyntax, "id+单括号");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  x[Alpha]\n    x[Beta]"), 0), RR::DuplicateId,
                 "显式重复 id");
    expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  a", "cpp"), 0), RR::UnsupportedLanguage, "非 mermaid 语言");
    expectReject(mindmap::parseBlock("```mermaid\nmindmap\n  root", 0), RR::UnclosedFence, "未闭合围栏");
    expectReject(mindmap::parseBlock("```mermaid\nmindmap\n  root\n```\njunk", 0), RR::InvalidFence,
                 "闭围栏后有多余内容");
    // 开/闭围栏字符不配，或闭围栏行带 info（其后并非仅空格）→ 实际未闭合。
    expectReject(mindmap::parseBlock("~~~mermaid\nmindmap\n  root\n```", 0), RR::UnclosedFence,
                 "开~~~不配闭```");
    expectReject(mindmap::parseBlock("```mermaid\nmindmap\n  root\n``` trailing", 0), RR::UnclosedFence,
                 "闭围栏后带 info");

    // 拒绝的 sourceLine 稳定：错误在第 4 行（含开围栏为第 1 行）。
    const mindmap::ParseResult multi = mindmap::parseBlock(fenceBlock("mindmap\n  a\n  b"), 0);
    check(multi.sourceLine == 4, "MultipleRoots 的 sourceLine 应为 4（实际 " +
                                     std::to_string(multi.sourceLine) + "）");
    // 部分解析后失败也必须清空整树。
    check(multi.plan.nodes.empty(), "部分解析后失败清空整树");
}

void testParseLimits() {
    using namespace neo;
    using RR = mindmap::RejectReason;

    // 标签码点上限 200：按**码点**计，不是字节（中文每字 3 字节、1 码点）。
    {
        std::string zh200;
        std::string zh201;
        for (int i = 0; i < 200; ++i) {
            zh200 += "中";
        }
        for (int i = 0; i < 201; ++i) {
            zh201 += "中";
        }
        const mindmap::ParseResult ok = mindmap::parseBlock(fenceBlock("mindmap\n  " + zh200), 0);
        check(static_cast<bool>(ok) && ok.plan.nodes.size() == 1 && ok.plan.nodes[0].label == zh200,
              "200 个中文码点应通过（字节 " + std::to_string(zh200.size()) + "）");
        expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  " + zh201), 0), RR::LabelLimit,
                     "201 个中文码点");
    }
    // 深度上限 32：根深度 0 → 33 节点链（深度 0..32）通过，34 节点链（深度 33）拒绝。
    {
        std::string okBody = "mindmap";
        std::string okIndent(2, ' ');
        for (int d = 0; d <= 32; ++d) {
            okBody += "\n" + okIndent + "n";
            okIndent += "  ";
        }
        const mindmap::ParseResult okParse = mindmap::parseBlock(fenceBlock(okBody), 0);
        check(static_cast<bool>(okParse), "深度 32（33 节点链）应通过，实际 " + reasonName(okParse.rejectReason));
        check(okParse.plan.nodes.size() == 33, "深度 32：应有 33 个节点");

        std::string body = "mindmap";
        std::string indent(2, ' ');
        for (int d = 0; d <= 33; ++d) {
            body += "\n" + indent + "n";
            indent += "  ";
        }
        expectReject(mindmap::parseBlock(fenceBlock(body), 0), RR::DepthLimit, "深度 33");
    }
    // 节点数上限 2000：根 + 2001 子节点拒绝。
    {
        std::string body = "mindmap\n  root";
        for (int i = 0; i < 2001; ++i) {
            body += "\n    c";
        }
        expectReject(mindmap::parseBlock(fenceBlock(body), 0), RR::NodeLimit, "节点数 2002");
    }
    // 源码字节上限 256 KiB。
    {
        std::string body = "mindmap\n  root\n%% " + std::string(300 * 1024, 'a');
        expectReject(mindmap::parseBlock(fenceBlock(body), 0), RR::SourceLimit, "源码 > 256 KiB");
    }
    // 自定义 Limits 生效。
    {
        mindmap::Limits small;
        small.nodes = 1;
        expectReject(mindmap::parseBlock(fenceBlock("mindmap\n  a\n    b"), 0, small), RR::NodeLimit,
                     "自定义节点上限");
    }
    // sourceBeg + block.size() 溢出 → SourceLimit（不溢出成小值）。
    {
        const std::string block = fenceBlock("mindmap\n  root");
        const std::size_t hugeBeg = std::numeric_limits<std::size_t>::max() - 3;
        expectReject(mindmap::parseBlock(block, hugeBeg), RR::SourceLimit, "sourceBeg 加和溢出");
    }
}

void testParseInvalidUtf8() {
    using namespace neo;
    using RR = mindmap::RejectReason;

    const auto withBytes = [](const std::string& raw) {
        return "```mermaid\nmindmap\n  root" + raw + "\n```";
    };
    const std::string overlong = std::string(1, static_cast<char>(0xC0)) + std::string(1, static_cast<char>(0x80));
    const std::string surrogate = std::string(1, static_cast<char>(0xED)) + std::string(1, static_cast<char>(0xA0)) +
                                  std::string(1, static_cast<char>(0x80));
    const std::string tooBig = std::string(1, static_cast<char>(0xF5)) + std::string(1, static_cast<char>(0x80)) +
                               std::string(1, static_cast<char>(0x80)) + std::string(1, static_cast<char>(0x80));
    const std::string control = std::string(1, static_cast<char>(0x01));
    const std::string nel = std::string(1, static_cast<char>(0xC2)) + std::string(1, static_cast<char>(0x85));

    expectReject(mindmap::parseBlock(withBytes(overlong), 0), RR::InvalidUtf8, "过长编码");
    expectReject(mindmap::parseBlock(withBytes(surrogate), 0), RR::InvalidUtf8, "代理区");
    expectReject(mindmap::parseBlock(withBytes(tooBig), 0), RR::InvalidUtf8, "超出 U+10FFFF");
    expectReject(mindmap::parseBlock(withBytes(control), 0), RR::InvalidUtf8, "控制字符");
    expectReject(mindmap::parseBlock(withBytes(nel), 0), RR::InvalidUtf8, "C1 控制 U+0085");
}

void testSourceOffsetsWithBase() {
    using namespace neo;
    const std::string block = fenceBlock("mindmap\n  root((EU))\n    leaf");
    const std::size_t base = 1234;
    const mindmap::ParseResult parsed = mindmap::parseBlock(block, base);
    check(static_cast<bool>(parsed), "sourceBase：应成功");
    if (!static_cast<bool>(parsed)) {
        return;
    }
    check(parsed.plan.sourceBeg == base && parsed.plan.sourceEnd == base + block.size(),
          "sourceBase：plan 区间按 base 平移");
    for (const auto& node : parsed.plan.nodes) {
        const std::string label = slice(block, node.sourceBeg - base, node.sourceEnd - base);
        check(label == node.label, "sourceBase：偏移按 base 平移且切片 == 标签");
    }
}

void testEstimatePlanBytes() {
    using namespace neo;
    const mindmap::ParseResult small = mindmap::parseBlock(fenceBlock("mindmap\n  root"), 0);
    const mindmap::ParseResult big =
        mindmap::parseBlock(fenceBlock("mindmap\n  root((R))\n    a\n    b\n    c\n      d"), 0);
    check(static_cast<bool>(small) && static_cast<bool>(big), "estimate：两份计划都应解析成功");
    if (!static_cast<bool>(small) || !static_cast<bool>(big)) {
        return;
    }
    const std::size_t smallBytes = mindmap::estimatePlanBytes(small.plan);
    const std::size_t bigBytes = mindmap::estimatePlanBytes(big.plan);
    check(smallBytes > 0, "estimate：应计入基础结构（>0）");
    check(bigBytes > smallBytes, "estimate：节点/字符串/children 更多时估算更大（" +
                                     std::to_string(smallBytes) + " -> " + std::to_string(bigBytes) + "）");
    check(mindmap::estimatePlanBytes(mindmap::Plan{}) == sizeof(mindmap::Plan), "estimate：空计划只有结构体开销");
}

}  // namespace

int main() {
    testRejectReasonNames();
    testSelectTargetBasic();
    testSelectTargetTwoBlocks();
    testSelectTargetFenceRejects();
    testSelectTargetHostilePlan();
    testSelectTargetTab();
    testParseCanonical();
    testParseIdsAndShapes();
    testParseCommentsAndUtf8();
    testParseRejects();
    testParseLimits();
    testParseInvalidUtf8();
    testSourceOffsetsWithBase();
    testEstimatePlanBytes();

    std::printf("\n%s：%d 项检查，%d 项失败\n", gFailures == 0 ? "ALL PASS" : "FAILED", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
