// Live Preview 适配层（apps/neo_editor/model/lp_decorations.h）的无头测试。
//
// 这里验证的是**编辑器真正用的那条路**：lp_plan 的逐行语义 → 输入组件的逐行装饰。
// 三条判据：
//   1. 标记"露面"的粒度：块级标记跟随块、行内标记跟随光标所在片段（与 Obsidian 对齐）；
//   2. 行内样式段：粗体 / 行内代码 / 链接 / 删除线确实产出，且几何与样式符合样式表；
//   3. 自检：隐藏区间都在本行内、升序、互不重叠，且是 lp_plan 给的 conceal 的子集
//      （适配层只应该"少藏"，绝不应该凭空多藏）。
//
// 构建：随主工程 EUI_BUILD_TEST_FIXTURES=ON 一起编（CMakeLists.txt 里给这个目标额外
// 挂了 apps/neo_editor/model/lp_plan.cpp 与 apps/neo_editor 的 include 目录）。

#include "model/lp_decorations.h"
// 右键菜单文本命令（applyInlineFormat / applyLinePrefix / insertBlockTemplate /
// flipTaskCheckboxAt）与 markdownStyle(state) 便利包装在 state 层。T10 拆层后
// lp_decorations.h 不再传递包含 app_state.h，这里显式 include。
#include "state/app_state.h"
#include "ui/outline_view.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

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

std::string escape(const std::string& text) {
    std::string out;
    for (char ch : text) {
        if (ch == '\n') {
            out += "\\n";
        } else {
            out += ch;
        }
        if (out.size() > 60) {
            out += "...";
            break;
        }
    }
    return out;
}

std::string slice(const std::string& text, int beg, int end) {
    if (beg < 0 || end > static_cast<int>(text.size()) || beg > end) {
        return "<invalid>";
    }
    return text.substr(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg));
}

// 一段装饰里所有隐藏区间的文本（用来判"到底藏了什么"）。
std::string holesText(const std::string& text,
                      const std::vector<components::input_detail::LineDecoration>& table,
                      int lineIndex) {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(table.size())) {
        return "<no-line>";
    }
    std::string out;
    for (const components::input_detail::LineHole& hole : table[static_cast<std::size_t>(lineIndex)].holes) {
        out += slice(text, hole.beg, hole.end);
    }
    return out;
}

bool holesContain(const std::vector<components::input_detail::LineDecoration>& table,
                  int lineIndex,
                  int beg,
                  int end) {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(table.size())) {
        return false;
    }
    for (const components::input_detail::LineHole& hole : table[static_cast<std::size_t>(lineIndex)].holes) {
        if (hole.beg == beg && hole.end == end) {
            return true;
        }
    }
    return false;
}

int findInLine(const std::string& text, const neo::LpLine& line, const std::string& needle) {
    const std::size_t found = text.find(needle, static_cast<std::size_t>(line.srcBeg));
    if (found == std::string::npos || static_cast<int>(found) >= line.srcEnd) {
        return -1;
    }
    return static_cast<int>(found);
}

const std::string kDoc =
    "# Head 标题\n"
    "\n"
    "正文里有 **粗体** 和 `代码`，还有 [链接](https://a.b) 与 ~~删除~~。\n"
    "\n"
    "- 列表项 **粗** 尾巴\n"
    "\n"
    "```cpp\n"
    "int a = 1;\n"
    "```\n";

// S3b（行级矩形）用的样例：引用块 2 行 + **两个紧贴的**代码块（中间不留空行，
// 用来验证"两个块不会因为相邻就被连成一块"）。
const std::string kBoxDoc =
    "> 引用第一行\n"
    "> 引用第二行\n"
    "\n"
    "普通正文\n"
    "\n"
    "```\n"
    "code a\n"
    "```\n"
    "```\n"
    "code b\n"
    "```\n";

// S3c（任务复选框）/ S3e（frontmatter）用的样例。
// 第 0~3 行是 frontmatter（`---` 两行也算在内），第 7/8 行是两个任务项（未勾 / 已勾），
// 第 9 行是**普通**列表项 —— 它不应该出现复选框。
const std::string kPrefixDoc =
    "---\n"
    "title: 测试\n"
    "tags: [a, b]\n"
    "---\n"
    "\n"
    "# 标题\n"
    "\n"
    "- [ ] 待办一\n"
    "- [x] 已完成\n"
    "- 普通列表项\n";

// S3f 批次 D（表格列对齐）用的样例：表头 / 分隔行 / 两个正文行，
// 最后一行的第一格是 `` `x|y` ``（反引号里的管道**不能**把格拆开）。
const std::string kTableDoc =
    "| 列甲 | 列乙 |\n"
    "| --- | --- |\n"
    "| a1 | b1 |\n"
    "| `x|y` | c3 |\n";

bool sameColor(const core::Color& lhs, const core::Color& rhs) {
    return std::fabs(lhs.r - rhs.r) < 0.002f && std::fabs(lhs.g - rhs.g) < 0.002f &&
           std::fabs(lhs.b - rhs.b) < 0.002f && std::fabs(lhs.a - rhs.a) < 0.002f;
}

// ── T15：章节表第一遍的朴素 oracle ─────────────────────────────────────────────
// 与 lp_plan.cpp **改写前**的 O(N²) 扫描逐字对应：对每个标题起始行向后线性找第一个
// headingLevel <= 自己的标题行（setext 下划线排除、默认终点 = 行数）。单调栈只是换了
// 写法，语义一位不差 —— 靠这份 oracle 逐行比对钉死。
// isSetextUnderline 的"连坐"判定也照抄：下划线行的前一行必然也是 setext 标题行，
// 于是"配对里第二个文本行"（前一行是上一组的下划线）会被一并判成下划线行。
bool isSetextUnderlineRow(const neo::LpPlan& plan, std::size_t index) {
    if (index == 0 || index >= plan.lines.size()) {
        return false;
    }
    const neo::LpLine& line = plan.lines[index];
    const neo::LpLine& previous = plan.lines[index - 1];
    return line.kind == neo::LpKind::Heading && line.setext &&
           previous.kind == neo::LpKind::Heading && previous.setext;
}

std::vector<int> naiveSectionEnds(const neo::LpPlan& plan) {
    std::vector<int> ends(plan.lines.size(), -1);
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        const neo::LpLine& line = plan.lines[i];
        if (line.kind != neo::LpKind::Heading || isSetextUnderlineRow(plan, i)) {
            continue;
        }
        int end = static_cast<int>(plan.lines.size());
        for (std::size_t j = i + 1; j < plan.lines.size(); ++j) {
            const neo::LpLine& candidate = plan.lines[j];
            if (candidate.kind != neo::LpKind::Heading ||
                candidate.headingLevel > line.headingLevel) {
                continue;
            }
            if (line.setext && candidate.setext && j == i + 1) {
                continue;  // 自己的 setext 下划线
            }
            end = static_cast<int>(j);
            break;
        }
        ends[i] = end;
    }
    return ends;
}

// ── T6：闭围栏回填的朴素全表 oracle ───────────────────────────────────────────
// 与 lp_plan.cpp **改写前**的收尾逐字对应：每碰到一条闭围栏，就回头把**整张行表**里
// codeBlockBeg 相同的 Code 行统统盖上 codeBlockEnd（每块一次全表扫描，最坏 O(N²)）。
// T6 只是把这次回填缩到本块行区间 [开围栏行, 闭围栏行]，结论必须一位不差 —— 靠这份
// oracle 逐行比对钉死。
// 闭围栏的识别不依赖改动后的实现细节：围栏行里只要 codeBlockBeg（= 开围栏行首）
// 不是自己的行首，它就是闭围栏（开围栏行的 codeBlockBeg 恒等于自己的行首）。
// 未闭合块沿用 lp_plan 收尾的既有兜底：codeBlockEnd < 0 → 该行 srcEnd。
std::vector<int> naiveCodeBlockEnds(const neo::LpPlan& plan) {
    std::vector<int> ends(plan.lines.size(), -1);
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        const neo::LpLine& fence = plan.lines[i];
        if (fence.kind != neo::LpKind::Code || !fence.codeFence ||
            fence.srcBeg == fence.codeBlockBeg) {
            continue;
        }
        // 旧写法：扫整张行表（这里照抄，正是 T6 要消掉的那趟全表扫描）
        for (std::size_t j = 0; j < plan.lines.size(); ++j) {
            const neo::LpLine& candidate = plan.lines[j];
            if (candidate.kind == neo::LpKind::Code &&
                candidate.codeBlockBeg == fence.codeBlockBeg) {
                ends[j] = fence.srcEnd;
            }
        }
    }
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        const neo::LpLine& line = plan.lines[i];
        if (line.kind == neo::LpKind::Code && ends[i] < 0) {
            ends[i] = line.srcEnd;  // 未闭合块：终点 = 本行行尾（既有语义）
        }
    }
    return ends;
}

// ── T4：编辑区间增量的等价性验收 ────────────────────────────────────────────
// 判据（任务 §11）：对同一篇文档做一次操作后，
//   A. 增量：走**真实漏斗**（loadDocument → 组件编辑 → pendingEdit）拿到编辑区间，
//      再 cachedDecorations（区间增量）+ InputLayout::build（逐行增量）；
//   B. 全量：从空状态对新文本 buildDecorations + measureLines；
// 两者必须**逐字段**相等：装饰比 holes/runs/cells/glyph/box/font/lineHeight/textShiftY/
// hidden/源偏移，行表比 start/end/holes/runs/lineNumber + metrics。
// 另外按计数断言"该增量的真增量了、该回退的真回退了"（表格用例必须显示回退计数）。
using IncModel = components::input_detail::InputModel;
using IncLine = IncModel::TextLine;
using IncDeco = components::input_detail::LineDecoration;

bool nearFloat(float a, float b) { return std::fabs(a - b) < 0.001f; }

// 装饰逐字段比较：返回首个不一致的字段名（空串 = 逐字段等价）。
std::string decorationFieldDiff(const IncDeco& a, const IncDeco& b) {
    if (!nearFloat(a.fontSize, b.fontSize)) return "fontSize";
    if (!nearFloat(a.lineHeight, b.lineHeight)) return "lineHeight";
    if (!components::input_detail::colorEquals(a.textColor, b.textColor)) return "textColor";
    if (a.fontFamily != b.fontFamily) return "fontFamily";
    if (a.holes.size() != b.holes.size()) return "holes.size";
    for (std::size_t i = 0; i < a.holes.size(); ++i) {
        if (a.holes[i].beg != b.holes[i].beg || a.holes[i].end != b.holes[i].end) {
            return "holes[" + std::to_string(i) + "]";
        }
    }
    if (a.runs.size() != b.runs.size()) return "runs.size";
    for (std::size_t i = 0; i < a.runs.size(); ++i) {
        if (a.runs[i].beg != b.runs[i].beg || a.runs[i].end != b.runs[i].end) {
            return "runs[" + std::to_string(i) + "].offset";
        }
        if (a.runs[i].style != b.runs[i].style) return "runs[" + std::to_string(i) + "].style";
    }
    if (!(a.box == b.box)) return "box";
    if (!(a.glyph == b.glyph)) return "glyph";
    if (!(a.gutterGlyph == b.gutterGlyph)) return "gutterGlyph";
    if (!nearFloat(a.contentIndent, b.contentIndent)) return "contentIndent";
    if (!nearFloat(a.textShiftY, b.textShiftY)) return "textShiftY";
    if (!nearFloat(a.spaceBefore, b.spaceBefore)) return "spaceBefore";
    if (a.imagePath != b.imagePath) return "imagePath";
    if (!nearFloat(a.imageWidth, b.imageWidth)) return "imageWidth";
    if (!nearFloat(a.imageHeight, b.imageHeight)) return "imageHeight";
    if (a.imageFailed != b.imageFailed) return "imageFailed";
    if (a.imageFailText != b.imageFailText) return "imageFailText";
    if (a.languageLabel != b.languageLabel) return "languageLabel";
    if (a.hidden != b.hidden) return "hidden";
    if (a.hiddenByFold != b.hiddenByFold) return "hiddenByFold";
    if (a.cells.size() != b.cells.size()) return "cells.size";
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        if (a.cells[i].beg != b.cells[i].beg || a.cells[i].end != b.cells[i].end ||
            a.cells[i].align != b.cells[i].align) {
            return "cells[" + std::to_string(i) + "]";
        }
    }
    if (a.tableId != b.tableId) return "tableId";
    if (!nearFloat(a.effectiveCellPadding(), b.effectiveCellPadding())) return "cellPadding";
    if (a.tableHeaderRow != b.tableHeaderRow) return "tableHeaderRow";
    if (a.tableSeparator != b.tableSeparator) return "tableSeparator";
    return {};
}

// 行表逐字段比较（任务 §11 的 TextLine 字段全集）。
std::string lineFieldDiff(const IncLine& a, const IncLine& b) {
    if (a.start != b.start) return "start";
    if (a.end != b.end) return "end";
    if (a.hardBreakAfter != b.hardBreakAfter) return "hardBreakAfter";
    if (a.lineNumber != b.lineNumber) return "lineNumber";
    if (a.lineStart != b.lineStart) return "lineStart";
    if (!nearFloat(a.fontSize, b.fontSize)) return "fontSize";
    if (!nearFloat(a.lineHeight, b.lineHeight)) return "lineHeight";
    if (!nearFloat(a.contentIndent, b.contentIndent)) return "contentIndent";
    if (!nearFloat(a.textShiftY, b.textShiftY)) return "textShiftY";
    if (!nearFloat(a.spaceBefore, b.spaceBefore)) return "spaceBefore";
    if (a.holes != b.holes) return "holes";
    if (a.runs.size() != b.runs.size()) return "runs.size";
    for (std::size_t i = 0; i < a.runs.size(); ++i) {
        if (a.runs[i].beg != b.runs[i].beg || a.runs[i].end != b.runs[i].end) {
            return "runs[" + std::to_string(i) + "].offset";
        }
        if (a.runs[i].style != b.runs[i].style) return "runs[" + std::to_string(i) + "].style";
        if (!nearFloat(a.runs[i].x, b.runs[i].x)) return "runs[" + std::to_string(i) + "].x";
        if (!nearFloat(a.runs[i].width, b.runs[i].width)) {
            return "runs[" + std::to_string(i) + "].width";
        }
    }
    if (a.metrics.byteIndices != b.metrics.byteIndices) return "metrics.byteIndices";
    if (a.metrics.caretX != b.metrics.caretX) return "metrics.caretX";
    if (!nearFloat(a.metrics.width, b.metrics.width)) return "metrics.width";
    if (!components::input_detail::colorEquals(a.color, b.color)) return "color";
    if (!(a.box == b.box)) return "box";
    if (!(a.glyph == b.glyph)) return "glyph";
    if (!(a.gutterGlyph == b.gutterGlyph)) return "gutterGlyph";
    if (a.imagePath != b.imagePath) return "imagePath";
    if (!nearFloat(a.imageWidth, b.imageWidth)) return "imageWidth";
    if (!nearFloat(a.imageHeight, b.imageHeight)) return "imageHeight";
    if (a.imageFailed != b.imageFailed) return "imageFailed";
    if (a.imageFailText != b.imageFailText) return "imageFailText";
    if (a.languageLabel != b.languageLabel) return "languageLabel";
    if (a.hidden != b.hidden) return "hidden";
    if (a.hiddenByFold != b.hiddenByFold) return "hiddenByFold";
    if (a.tableId != b.tableId) return "tableId";
    if (a.tableSeparator != b.tableSeparator) return "tableSeparator";
    if (a.fontFamily != b.fontFamily) return "fontFamily";
    return {};
}

// 参照行表的 top（rebuildGeometry 的口径：hidden 行 0 高 + 0 gap，
// 行高回落到 1.2×字号；块间距记在**本行自己**的 top 上，所以循环要包住 index）。
float referenceTopAt(const std::vector<IncLine>& lines, int index, float fallbackHeight) {
    float top = 0.0f;
    for (int i = 0; i <= index && i < static_cast<int>(lines.size()); ++i) {
        const IncLine& line = lines[static_cast<std::size_t>(i)];
        if (line.hidden) continue;
        if (i > 0 && line.spaceBefore > 0.0f) {
            top += line.spaceBefore;
        }
        if (i < index) {
            top += line.lineHeight > 0.0f ? line.lineHeight : fallbackHeight;
        }
    }
    return top;
}

struct IncStep {
    const char* label = "";
    std::function<void(IncModel::InputState&)> edit;
};

// 一次"编辑 → 增量 vs 全量"的完整验收（每个 step 之间缓存链必须连续）。
void runIncrementalCase(const std::string& label,
                        const std::string& oldText,
                        const std::vector<IncStep>& steps,
                        const std::set<int>* foldedHeadings,
                        bool expectDecorIncremental,
                        bool expectLayoutIncremental) {
    const char* fontFamily = "Microsoft YaHei";
    const float fontSize = 16.0f;
    const float viewportWidth = 480.0f;
    const float lineHeight = fontSize * 1.2f;
    const neo::EditorColors& colors = neo::editorColors(neo::ThemeMode::Dark);
    const components::MarkdownStyle style =
        neo::markdownStyle(fontSize, fontFamily, "monospace", colors);
    const std::string docDir;  // 相对图片路径按 cwd 解析（样例用不存在的路径 → 失败态）

    IncModel::InputState state;
    IncModel::loadDocument(state, oldText);

    // ── 基线：全量装饰 + 全量布局（把缓存链建在当前 revision 上）──
    // T5：基线也带上 editInfo（committed、无 delta），与 decorationsForEditor 的
    // 调用形态一致 —— 这样紧接着的第一次编辑就能满足局部重解析的 revision 链。
    components::input_detail::DecoratorEditInfo infoA;
    infoA.textRevision = state.textRevision;
    infoA.committed = true;
    infoA.edit = nullptr;
    const neo::LpPlan& planA = neo::lp::cachedPlan(oldText, &infoA);
    const auto snapshotA = neo::lp::cachedDecorationSnapshot(
        planA, neo::lp::planCache().version, state.cursor, style, fontFamily, neo::ThemeMode::Dark,
        docDir, foldedHeadings, oldText, &colors, &infoA);
    const auto& decoA = *snapshotA;
    IncModel::InputLayout::build(state, viewportWidth, 600.0f, 480.0f, 10.0f, 10.0f, 10.0f,
                                 lineHeight, fontFamily, fontSize, true, &decoA, snapshotA);

    for (const IncStep& step : steps) {
        step.edit(state);
        check(state.pendingEdit.valid && state.pendingEdit.revision == state.textRevision,
              label + "/" + step.label + "：公共漏斗应当写下有效的编辑区间");

        const neo::lp::DecorationDebugStats decorBefore = neo::lp::decorationDebugStats();
        const IncModel::LayoutDebugStats layoutBefore = IncModel::debugLayoutStats();
        neo::lp::decorationRejectReason().clear();
        IncModel::layoutRejectReason().clear();

        // ── 增量：装饰（cachedDecorations 按 pendingEdit 走区间增量）──
        // T5：把编辑区间一并递给 cachedPlan —— 链完整时计划走局部重解析，装饰增量
        // 必须照旧成立（两条增量互不干扰，且局部计划逐字段等于全量）。
        components::input_detail::DecoratorEditInfo infoB;
        infoB.textRevision = state.textRevision;
        infoB.committed = true;
        infoB.edit = &state.pendingEdit;
        const neo::LpPlan& planB = neo::lp::cachedPlan(state.text, &infoB);
        const auto snapshotB = neo::lp::cachedDecorationSnapshot(
            planB, neo::lp::planCache().version, state.cursor, style, fontFamily,
            neo::ThemeMode::Dark, docDir, foldedHeadings, state.text, &colors, &infoB);
        const auto& decoB = *snapshotB;
        // ── 增量：布局（ensureLayoutCache 按文本差分 + 装饰差分逐行处理）──
        IncModel::InputLayout::build(state, viewportWidth, 600.0f, 480.0f, 10.0f, 10.0f, 10.0f,
                                     lineHeight, fontFamily, fontSize, true, &decoB, snapshotB);

        const neo::lp::DecorationDebugStats decorAfter = neo::lp::decorationDebugStats();
        const IncModel::LayoutDebugStats layoutAfter = IncModel::debugLayoutStats();
        const unsigned long long decorIncremental = decorAfter.incremental - decorBefore.incremental;
        const unsigned long long decorFallback = decorAfter.fallback - decorBefore.fallback;
        const unsigned long long decorFull = decorAfter.full - decorBefore.full;
        const unsigned long long layoutIncremental =
            layoutAfter.incremental - layoutBefore.incremental;
        const unsigned long long layoutFallback = layoutAfter.fallback - layoutBefore.fallback;
        const unsigned long long layoutFull = layoutAfter.full - layoutBefore.full;

        const std::string tag = label + "/" + step.label;
        const std::string decorReason =
            neo::lp::decorationRejectReason().empty() ? "" : "原因：" + neo::lp::decorationRejectReason();
        const std::string layoutReason =
            IncModel::layoutRejectReason().empty() ? "" : "原因：" + IncModel::layoutRejectReason();
        if (expectDecorIncremental) {
            check(decorIncremental >= 1 && decorFallback == 0 && decorFull == 0,
                  tag + "：装饰应走区间增量（incremental=" + std::to_string(decorIncremental) +
                      " fallback=" + std::to_string(decorFallback) +
                      " full=" + std::to_string(decorFull) + "）" + decorReason);
        } else {
            check(decorFallback >= 1 && decorFull >= 1,
                  tag + "：装饰应判据不过回退全量（incremental=" +
                      std::to_string(decorIncremental) + " fallback=" +
                      std::to_string(decorFallback) + " full=" + std::to_string(decorFull) + "）" +
                      decorReason);
        }
        if (expectLayoutIncremental) {
            check(layoutIncremental >= 1 && layoutFallback == 0 && layoutFull == 0,
                  tag + "：布局应走逐行增量（incremental=" + std::to_string(layoutIncremental) +
                      " fallback=" + std::to_string(layoutFallback) +
                      " full=" + std::to_string(layoutFull) + "）" + layoutReason);
        } else {
            check(layoutFallback >= 1 && layoutFull >= 1,
                  tag + "：布局应判据不过回退全量（incremental=" +
                      std::to_string(layoutIncremental) + " fallback=" +
                      std::to_string(layoutFallback) + " full=" + std::to_string(layoutFull) +
                      "）" + layoutReason);
        }

        // ── 参照：对新文本从空状态全量重建 ──
        std::vector<IncDeco> refDeco;
        neo::lp::buildDecorations(planB, style, state.cursor, refDeco, fontFamily, docDir,
                                  foldedHeadings, state.text, &colors);
        std::vector<IncModel::TableColumns> refTables;
        const std::vector<IncLine> refLines =
            IncModel::measureLines(state.text, fontFamily, fontSize, viewportWidth, &refDeco,
                                   &refTables);

        // ── 装饰逐字段比较 ──
        if (decoB.size() != refDeco.size()) {
            check(false, tag + "：装饰行数不等 incremental=" + std::to_string(decoB.size()) +
                             " full=" + std::to_string(refDeco.size()));
        } else {
            int diffCount = 0;
            std::string firstDiff;
            for (std::size_t i = 0; i < refDeco.size(); ++i) {
                const std::string diff = decorationFieldDiff(decoB[i], refDeco[i]);
                if (!diff.empty()) {
                    ++diffCount;
                    if (firstDiff.empty()) {
                        firstDiff = "行 " + std::to_string(i) + " 字段 " + diff;
                    }
                }
            }
            check(diffCount == 0,
                  tag + "：增量装饰必须与全量逐字段等价，差 " + std::to_string(diffCount) +
                      " 行（首个：" + firstDiff + "）");
        }

        // ── 行表逐字段比较 ──
        const std::vector<IncLine>& actualLines = state.cachedLines;
        if (actualLines.size() != refLines.size()) {
            check(false, tag + "：行表行数不等 incremental=" + std::to_string(actualLines.size()) +
                             " full=" + std::to_string(refLines.size()));
        } else {
            int diffCount = 0;
            int topDrift = 0;
            std::string firstDiff;
            for (std::size_t i = 0; i < refLines.size(); ++i) {
                const std::string diff = lineFieldDiff(actualLines[i], refLines[i]);
                if (!diff.empty()) {
                    ++diffCount;
                    if (firstDiff.empty()) {
                        firstDiff = "行 " + std::to_string(i) + " 字段 " + diff;
                    }
                }
                if (!nearFloat(actualLines[i].top,
                               referenceTopAt(refLines, static_cast<int>(i), lineHeight))) {
                    ++topDrift;
                }
            }
            check(diffCount == 0,
                  tag + "：增量行表必须与全量逐字段等价，差 " + std::to_string(diffCount) +
                      " 行（首个：" + firstDiff + "）");
            check(topDrift == 0,
                  tag + "：行顶前缀和与全量不一致 " + std::to_string(topDrift) + " 行");
        }

        // ── 表格列几何（回退路径也要逐项一致）──
        if (state.cachedTables.size() != refTables.size()) {
            check(false, tag + "：表格列计划数量不等 incremental=" +
                             std::to_string(state.cachedTables.size()) +
                             " full=" + std::to_string(refTables.size()));
        } else {
            int tableDrift = 0;
            for (std::size_t i = 0; i < refTables.size(); ++i) {
                const IncModel::TableColumns& a = state.cachedTables[i];
                const IncModel::TableColumns& b = refTables[i];
                check(IncModel::findTableColumns(state.cachedTables, state.cachedTableIndex, a.tableId) == &a,
                      tag + ": table index must follow insert/delete/anchor translation");
                if (a.tableId != b.tableId || a.x != b.x || a.width != b.width ||
                    !nearFloat(a.padding, b.padding) || !nearFloat(a.total, b.total)) {
                    ++tableDrift;
                }
            }
            check(tableDrift == 0,
                  tag + "：表格列计划与全量不一致 " + std::to_string(tableDrift) + " 张");
        }
        check(state.cachedTableIndex.size() == state.cachedTables.size(),
              tag + ": published table index must have no stale IDs");
    }
}

// ── 用例组（任务 §11 的文档/操作清单）──────────────────────────────────────
void testIncrementalEquivalence() {
    const std::string kRichDoc =
        "# 一级标题\n"
        "正文段落带 **粗体**、`行内码` 与 [链接](https://a.b)、[[维基]]、~~删除~~。\n"
        "> 引用第一行\n"
        "> 引用第二行\n"
        "- 列表项一\n"
        "- 列表项二\n"
        "  - 嵌套项\n"
        "- [ ] 待办\n"
        "- [x] 已完成\n"
        "尾部段落\n";

    // ① 标题/列表/引用/任务文档：在文档末尾**插入多行**（前面所有行逐字节未动，
    //    结构必须被判定为等价 → 走增量）。文档中部的插入若会改写别处的块结构
    //    （比如把一个列表劈成两个），按 §9 判据不过、回退全量 —— 见 ②③ 的变体。
    runIncrementalCase(
        "插入多行", kRichDoc,
        {{"末尾插三行", [](IncModel::InputState& state) {
             IncModel::moveCursorTo(state, static_cast<int>(state.text.size()), false);
             IncModel::insertAtCursor(state, "\n**新段** 起始\n> 插入的引用\n");
         }}},
        nullptr, true, true);

    // ② 同一篇：**删除多行**（跨块的选区擦除）
    runIncrementalCase(
        "删除多行", kRichDoc,
        {{"删除三行", [](IncModel::InputState& state) {
             const std::size_t beg = state.text.find("> 引用第一行");
             const std::size_t end = state.text.find("- 列表项一");
             state.cursor = static_cast<int>(beg);
             state.selectionStart = static_cast<int>(beg);
             state.selectionEnd = static_cast<int>(end);
             IncModel::eraseSelection(state);
         }}},
        nullptr, true, true);

    // ③ 跨行 fenced syntax：块注释与三引号都横跨多行，编辑落在块**中间** ——
    //    重算必须从开围栏起步、算到受影响块尾，否则后续行的着色会跟全量不一致。
    const std::string kSyntaxDoc =
        "前言\n"
        "```cpp\n"
        "int a = 1;\n"
        "/* 跨行\n"
        "   注释块 */\n"
        "int b = 2;\n"
        "```\n"
        "```python\n"
        "s = \"\"\"三引号\n"
        "跨行字符串\"\"\"\n"
        "t = 1\n"
        "```\n"
        "收尾\n";
    runIncrementalCase(
        "跨行语法", kSyntaxDoc,
        {{"块中插行", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("int b = 2;");
             IncModel::moveCursorTo(state, static_cast<int>(pos), false);
             IncModel::insertAtCursor(state, "int c = 3; /* 追加 */\n");
         }},
         {"三引号里改字", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("跨行字符串");
             IncModel::moveCursorTo(state, static_cast<int>(pos), false);
             IncModel::insertAtCursor(state, "改了");
         }}},
        nullptr, true, true);

    // ④ 图片行（失败态占位盒）+ wiki/链接：编辑图片行**之前**的段落
    const std::string kImageDoc =
        "开头段落\n"
        "![缺](nope-missing.png)\n"
        "![远](https://x.y/z.png)\n"
        "带 [链接](https://a.b) 和 [[维基]] 的段落\n"
        "结尾\n";
    runIncrementalCase(
        "图片与链接", kImageDoc,
        {{"图片前插行", [](IncModel::InputState& state) {
             IncModel::moveCursorTo(state, 0, false);
             IncModel::insertAtCursor(state, "第一行前面加一行\n");
         }},
         {"链接行改字", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("带 [链接]");
             IncModel::moveCursorTo(state, static_cast<int>(pos), false);
             IncModel::insertAtCursor(state, "前缀 ");
         }}},
        nullptr, true, true);

    // ⑤ 折叠预览：折叠标题在文档上半部、编辑点在其**后** → 键未漂，走增量；
    //    previewOwners 的全量语义必须被复现（重复预览横条会直接体现在 hidden 上）。
    const std::string kFoldDoc =
        "# 可折叠章节\n"
        "章节正文第一行\n"
        "章节正文第二行\n"
        "\n"
        "# 另一个标题\n"
        "后面的段落\n"
        "更后面的段落\n";
    {
        std::set<int> folds;
        folds.insert(static_cast<int>(kFoldDoc.find("# 可折叠章节")));
        runIncrementalCase(
            "折叠-编辑在后", kFoldDoc,
            {{"章节体内编辑", [](IncModel::InputState& state) {
                 const std::size_t pos = state.text.find("章节正文第二行");
                 IncModel::moveCursorTo(state, static_cast<int>(pos), false);
                 IncModel::insertAtCursor(state, "改这里。");
             }},
             {"折叠点之后编辑", [](IncModel::InputState& state) {
                 const std::size_t pos = state.text.find("更后面的段落");
                 IncModel::moveCursorTo(state, static_cast<int>(pos), false);
                 IncModel::insertAtCursor(state, "补一句。");
             }}},
            &folds, true, true);
    }
    // ⑥ 折叠 + 编辑点在折叠标题**之前** → 键会漂（本层不做键平移）→ 装饰回退全量。
    {
        std::set<int> folds;
        folds.insert(static_cast<int>(kFoldDoc.find("# 可折叠章节")));
        runIncrementalCase(
            "折叠-编辑在前", kFoldDoc,
            {{"折叠点之前编辑", [](IncModel::InputState& state) {
                 IncModel::moveCursorTo(state, 0, false);
                 IncModel::insertAtCursor(state, "开头加一行\n");
             }}},
            &folds, false, true);
    }

    // Table edits use incremental decorations and invalidate column geometry by table block.
    const std::string kTableDoc =
        "上方段落\n"
        "\n"
        "| 名称 | 数量 |\n"
        "| :--- | ---: |\n"
        "| 苹果 | 3 |\n"
        "| 梨 | 5 |\n"
        "\n"
        "下方段落\n";
    // 表格必须真的被解析出来（否则用例的"回退"断言就是假通过）。
    {
        const neo::LpPlan tablePlan = neo::buildLpPlan(kTableDoc);
        int tableRows = 0;
        for (const neo::LpLine& line : tablePlan.lines) {
            if (line.tableId >= 0) ++tableRows;
        }
        check(tableRows >= 4, "表格样例应解析出 4 行表格，实际 " + std::to_string(tableRows));
    }
    runIncrementalCase(
        "表格上方", kTableDoc,
        {{"表前编辑", [](IncModel::InputState& state) {
             IncModel::moveCursorTo(state, 0, false);
             IncModel::insertAtCursor(state, "更上面的一行\n");
         }}},
        nullptr, true, true);
    runIncrementalCase(
        "表格内部", kTableDoc,
        {{"格内改字", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("苹果");
             IncModel::moveCursorTo(state, static_cast<int>(pos) + 3, false);
             IncModel::insertAtCursor(state, "红富士");
         }},
         {"表下改字", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("下方段落");
             IncModel::moveCursorTo(state, static_cast<int>(pos), false);
             IncModel::insertAtCursor(state, "追加");
         }}},
        nullptr, true, true);

    runIncrementalCase("table rows and undo", kTableDoc + "\n" + kTableDoc,
        {{"grow cell and wrap", [](IncModel::InputState& state) {
            IncModel::moveCursorTo(state, static_cast<int>(state.text.find("苹果")), false);
            IncModel::insertAtCursor(state, std::string(90, 'W'));
        }}, {"remove widest row", [](IncModel::InputState& state) {
            const auto from = state.text.find("| W");
            const auto to = state.text.find('\n', from) + 1;
            IncModel::moveCursorTo(state, static_cast<int>(from), false);
            IncModel::moveCursorTo(state, static_cast<int>(to), true);
            IncModel::eraseSelection(state);
            check(state.text.find("| W") == std::string::npos, "the widest table row must actually be removed");
        }}, {"undo row removal", [](IncModel::InputState& state) { IncModel::undoEdit(state); }},
        {"redo row removal", [](IncModel::InputState& state) { IncModel::redoEdit(state); }},
        {"insert row", [](IncModel::InputState& state) {
            IncModel::moveCursorTo(state, static_cast<int>(state.text.find("| 梨")), false);
            IncModel::insertAtCursor(state, "| 新行带 **粗体** | 1234567890 |\n");
        }}}, nullptr, true, true);

    // ⑨ 撤销（applyEditRecord 同样是公共漏斗）：编辑 → 增量，撤销 → 仍要增量。
    runIncrementalCase(
        "撤销", kRichDoc,
        {{"插入", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("尾部段落");
             IncModel::moveCursorTo(state, static_cast<int>(pos), false);
             IncModel::insertAtCursor(state, "临时插入\n");
         }},
         {"撤销", [](IncModel::InputState& state) { IncModel::undoEdit(state); }}},
        nullptr, true, true);
}

// ── 10 万行普通段落的单点编辑（任务 §12）────────────────────────────────────
// 判据：装饰与布局都必须落进**增量计数**而不是全量；同时打印 T12 关心的
// decor 全量/增量耗时（不设脆弱的 16ms 断言，只记录）。
void testLargeDocumentIncremental() {
    constexpr int kLineCount = 100000;
    std::string text;
    text.reserve(static_cast<std::size_t>(kLineCount) * 48);
    for (int i = 0; i < kLineCount; ++i) {
        text += "第 " + std::to_string(i) + " 段普通正文，用于验证装饰与布局的增量路径。\n";
    }
    const char* fontFamily = "Microsoft YaHei";
    const float fontSize = 16.0f;
    const float viewportWidth = 900.0f;
    const float lineHeight = fontSize * 1.2f;
    const neo::EditorColors& colors = neo::editorColors(neo::ThemeMode::Dark);
    const components::MarkdownStyle style =
        neo::markdownStyle(fontSize, fontFamily, "monospace", colors);
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };

    IncModel::InputState state;
    IncModel::loadDocument(state, text);

    // 基线（全量）：建表 + 全量布局。
    const neo::LpPlan& planA = neo::lp::cachedPlan(text);
    components::input_detail::DecoratorEditInfo infoA;
    infoA.textRevision = state.textRevision;
    infoA.committed = true;
    infoA.edit = nullptr;
    const std::vector<IncDeco>& decoA = neo::lp::cachedDecorations(
        planA, neo::lp::planCache().version, state.cursor, style, fontFamily, neo::ThemeMode::Dark,
        {}, nullptr, text, &colors, &infoA);
    IncModel::InputLayout::build(state, viewportWidth, 600.0f, 900.0f, 10.0f, 10.0f, 10.0f,
                                 lineHeight, fontFamily, fontSize, true, &decoA);
    check(state.cachedLines.size() >= static_cast<std::size_t>(kLineCount),
          "10 万行文档的基线行数不应少于源行数");

    // 单点编辑：第 50000 段里插两个字。
    const std::size_t pos = text.find("第 50000 段");
    IncModel::moveCursorTo(state, static_cast<int>(pos) + 6, false);
    const neo::lp::DecorationDebugStats decorBefore = neo::lp::decorationDebugStats();
    const IncModel::LayoutDebugStats layoutBefore = IncModel::debugLayoutStats();
    const Clock::time_point editStart = Clock::now();
    IncModel::insertAtCursor(state, "插字");
    neo::lp::decorationRejectReason().clear();
    IncModel::layoutRejectReason().clear();
    const neo::LpPlan& planB = neo::lp::cachedPlan(state.text);
    components::input_detail::DecoratorEditInfo infoB;
    infoB.textRevision = state.textRevision;
    infoB.committed = true;
    infoB.edit = &state.pendingEdit;
    const Clock::time_point decorStart = Clock::now();
    const std::vector<IncDeco>& decoB = neo::lp::cachedDecorations(
        planB, neo::lp::planCache().version, state.cursor, style, fontFamily, neo::ThemeMode::Dark,
        {}, nullptr, state.text, &colors, &infoB);
    const Clock::time_point decorEnd = Clock::now();
    const Clock::time_point layoutStart = Clock::now();
    IncModel::InputLayout::build(state, viewportWidth, 600.0f, 900.0f, 10.0f, 10.0f, 10.0f,
                                 lineHeight, fontFamily, fontSize, true, &decoB);
    const Clock::time_point layoutEnd = Clock::now();
    const neo::lp::DecorationDebugStats decorAfter = neo::lp::decorationDebugStats();
    const IncModel::LayoutDebugStats layoutAfter = IncModel::debugLayoutStats();

    const unsigned long long decorIncremental = decorAfter.incremental - decorBefore.incremental;
    const unsigned long long decorFull = decorAfter.full - decorBefore.full;
    const unsigned long long decorFallback = decorAfter.fallback - decorBefore.fallback;
    const unsigned long long layoutIncremental = layoutAfter.incremental - layoutBefore.incremental;
    const unsigned long long layoutFull = layoutAfter.full - layoutBefore.full;
    const unsigned long long layoutFallback = layoutAfter.fallback - layoutBefore.fallback;
    check(decorIncremental >= 1 && decorFull == 0 && decorFallback == 0,
          "10 万行单点编辑：装饰必须走增量（incremental=" + std::to_string(decorIncremental) +
              " full=" + std::to_string(decorFull) +
              " fallback=" + std::to_string(decorFallback) + "）" +
              (neo::lp::decorationRejectReason().empty()
                   ? std::string()
                   : "原因：" + neo::lp::decorationRejectReason()));
    check(layoutIncremental >= 1 && layoutFull == 0 && layoutFallback == 0,
          "10 万行单点编辑：布局必须走增量（incremental=" + std::to_string(layoutIncremental) +
              " full=" + std::to_string(layoutFull) +
              " fallback=" + std::to_string(layoutFallback) + "）" +
              (IncModel::layoutRejectReason().empty()
                   ? std::string()
                   : "原因：" + IncModel::layoutRejectReason()));

    // 参照（全量）+ 耗时记录（T12：decor / layout 的全量 vs 增量）。
    std::vector<IncDeco> refDeco;
    const Clock::time_point fullDecorStart = Clock::now();
    neo::lp::buildDecorations(planB, style, state.cursor, refDeco, fontFamily, {}, nullptr,
                              state.text, &colors);
    const Clock::time_point fullDecorEnd = Clock::now();
    const Clock::time_point fullLayoutStart = Clock::now();
    const std::vector<IncLine> refLines =
        IncModel::measureLines(state.text, fontFamily, fontSize, viewportWidth, &refDeco);
    const Clock::time_point fullLayoutEnd = Clock::now();

    int decorDiff = 0;
    if (decoB.size() == refDeco.size()) {
        for (std::size_t i = 0; i < refDeco.size(); ++i) {
            if (!decorationFieldDiff(decoB[i], refDeco[i]).empty()) ++decorDiff;
        }
    } else {
        decorDiff = 1;
    }
    int lineDiff = 0;
    if (state.cachedLines.size() == refLines.size()) {
        for (std::size_t i = 0; i < refLines.size(); ++i) {
            if (!lineFieldDiff(state.cachedLines[i], refLines[i]).empty()) ++lineDiff;
        }
    } else {
        lineDiff = 1;
    }
    check(decorDiff == 0,
          "10 万行：增量装饰与全量逐字段等价（差 " + std::to_string(decorDiff) + " 行）");
    check(lineDiff == 0,
          "10 万行：增量行表与全量逐字段等价（差 " + std::to_string(lineDiff) + " 行）");

    std::printf("[T12] 10 万行：decor 全量 %.1f ms / 增量 %.1f ms；layout 全量 %.1f ms / 增量 "
                "%.1f ms；整次编辑 %.1f ms\n",
                ms(fullDecorStart, fullDecorEnd), ms(decorStart, decorEnd),
                ms(fullLayoutStart, fullLayoutEnd), ms(layoutStart, layoutEnd),
                ms(editStart, layoutEnd));
}

// ── T5：局部重解析（LpPlan 局部 vs 完整）────────────────────────────────────
// 三层验收：
//   ① **逐字段 oracle**：局部结果必须与"对新文本全量 buildLpPlan"一位不差 ——
//      行表 + 所有偏移字段（srcBeg/srcEnd/blockBeg/blockEnd/codeBlockBeg/End/
//      taskStateByte、span 的 openMark/closeMark/content、cell 区间、tableId）、
//      章节三字段（sectionHeadingLine/parentHeadingLine/sectionEndLine）、
//      conceal/spans/pureImageSrc，外加 stats 里的非诊断项。有差即失败。
//   ② **命中/回退**：安全的普通正文编辑必须真的走局部路径；复杂情形必须真的回退
//      （并且计数：partial/fallback 各 +1，原因串可读）。
//   ③ **缓存链**：走真实漏斗时 planVersion 恰好 +1、prevPlan 仍是上一代（T4 的
//      前提），且装饰仍落进区间增量。
//
// 诊断计数（markerMismatch / duplicateRanges / mergedRanges / invalidRanges）也进
// oracle：局部路径把区间外行的 conceal 按"新鲜扫描器分量 + 上一代 md4c 校正值 +
// 上一代 span 标记"重拼了一遍，normalize 的计数因此应当与全量逐个相等。
// 唯一不比的是 LpPlanStats::partial（全量恒 false 的"出身"标记）。

int countNewlinesIn(const std::string& text, int beg, int end) {
    int n = 0;
    for (int i = beg; i < end && i < static_cast<int>(text.size()); ++i) {
        if (text[static_cast<std::size_t>(i)] == '\n') {
            ++n;
        }
    }
    return n;
}

// 复刻 components::input_detail::InputModel::recordPendingEdit 的行号推导
// （firstLine 数新文本前缀换行、newLastLine 数受影响段换行、oldTailLine 反解）。
neo::LpTextEdit makePlanEdit(const std::string& newText,
                             int byteBeg,
                             int oldEnd,
                             int newEnd,
                             const std::string& removed,
                             const std::string& inserted) {
    neo::LpTextEdit edit;
    edit.valid = true;
    edit.revision = 1;
    edit.byteBeg = byteBeg;
    edit.oldEnd = oldEnd;
    edit.newEnd = newEnd;
    edit.firstLine = countNewlinesIn(newText, 0, byteBeg);
    edit.newLastLine = newEnd > byteBeg
                           ? edit.firstLine + countNewlinesIn(newText, byteBeg, newEnd - 1)
                           : edit.firstLine;
    const int deltaLines = countNewlinesIn(inserted, 0, static_cast<int>(inserted.size())) -
                           countNewlinesIn(removed, 0, static_cast<int>(removed.size()));
    edit.oldTailLine = edit.newLastLine + 1 - deltaLines;
    edit.cursorBefore = byteBeg;
    return edit;
}

std::string rangeDiff(const neo::LpRange& a, const neo::LpRange& b) {
    if (a.beg != b.beg || a.end != b.end) {
        return "[" + std::to_string(a.beg) + "," + std::to_string(a.end) + ") vs [" +
               std::to_string(b.beg) + "," + std::to_string(b.end) + ")";
    }
    return {};
}

// 逐字段 oracle：返回首个不一致的字段描述（空串 = 局部与全量逐字段等价）。
std::string planFieldDiff(const neo::LpPlan& a, const neo::LpPlan& b) {
    if (a.lines.size() != b.lines.size()) {
        return "lines.size " + std::to_string(a.lines.size()) + " vs " +
               std::to_string(b.lines.size());
    }
    const auto same = [](bool ok, const std::string& what) { return ok ? std::string() : what; };
    for (std::size_t i = 0; i < a.lines.size(); ++i) {
        const neo::LpLine& x = a.lines[i];
        const neo::LpLine& y = b.lines[i];
        const std::string where = "行 " + std::to_string(i) + " ";
        const std::vector<std::string> checks = {
            same(x.srcBeg == y.srcBeg && x.srcEnd == y.srcEnd && x.number == y.number,
                 where + "srcBeg/srcEnd/number（" + std::to_string(x.srcBeg) + "/" +
                     std::to_string(y.srcBeg) + "）"),
            same(x.kind == y.kind, where + "kind"),
            same(x.headingLevel == y.headingLevel && x.setext == y.setext,
                 where + "headingLevel/setext"),
            same(x.quoteDepth == y.quoteDepth && x.listDepth == y.listDepth &&
                     x.ordered == y.ordered && x.orderedNumber == y.orderedNumber &&
                     x.listOrdinal == y.listOrdinal,
                 where + "quoteDepth/listDepth/ordered/listOrdinal"),
            same(x.listItemLine == y.listItemLine && x.contentBeg == y.contentBeg,
                 where + "listItemLine/contentBeg"),
            same(x.task == y.task && x.taskChecked == y.taskChecked &&
                     x.taskStateByte == y.taskStateByte,
                 where + "task/taskStateByte（" + std::to_string(x.taskStateByte) + "/" +
                     std::to_string(y.taskStateByte) + "）"),
            same(x.codeFence == y.codeFence && x.codeLang == y.codeLang &&
                     x.codeBlockBeg == y.codeBlockBeg && x.codeBlockEnd == y.codeBlockEnd,
                 where + "code*"),
            same(x.blockBeg == y.blockBeg && x.blockEnd == y.blockEnd &&
                     x.blockFirstLine == y.blockFirstLine && x.blockLastLine == y.blockLastLine,
                 where + "blockBeg/End（" + std::to_string(x.blockBeg) + "/" +
                     std::to_string(y.blockBeg) + "）"),
            same(x.sectionHeadingLine == y.sectionHeadingLine &&
                     x.parentHeadingLine == y.parentHeadingLine &&
                     x.sectionEndLine == y.sectionEndLine,
                 where + "section*"),
            same(x.tableId == y.tableId && x.tableHeaderRow == y.tableHeaderRow &&
                     x.tableSeparator == y.tableSeparator,
                 where + "tableId/flags"),
            same(x.cells.size() == y.cells.size(), where + "cells.size"),
            same(x.cellAligns == y.cellAligns, where + "cellAligns"),
            same(x.conceal.size() == y.conceal.size(), where + "conceal.size"),
            same(x.spans.size() == y.spans.size(), where + "spans.size"),
            same(x.pureImageSrc == y.pureImageSrc, where + "pureImageSrc"),
        };
        for (const std::string& check : checks) {
            if (!check.empty()) {
                return check;
            }
        }
        for (std::size_t c = 0; c < x.conceal.size(); ++c) {
            const std::string d = rangeDiff(x.conceal[c], y.conceal[c]);
            if (!d.empty()) {
                return where + "conceal[" + std::to_string(c) + "] " + d;
            }
        }
        for (std::size_t s = 0; s < x.spans.size(); ++s) {
            const neo::LpSpan& sx = x.spans[s];
            const neo::LpSpan& sy = y.spans[s];
            if (sx.kind != sy.kind || !(sx.content == sy.content) ||
                !(sx.openMark == sy.openMark) || !(sx.closeMark == sy.closeMark)) {
                return where + "span[" + std::to_string(s) + "]（marks/content 不等）";
            }
        }
        for (std::size_t c = 0; c < x.cells.size(); ++c) {
            const std::string d = rangeDiff(x.cells[c], y.cells[c]);
            if (!d.empty()) {
                return where + "cell[" + std::to_string(c) + "] " + d;
            }
        }
    }
    const auto stat = [](int x, int y, const char* name) {
        return x == y ? std::string()
                      : std::string("stats.") + name + " " + std::to_string(x) + " vs " +
                            std::to_string(y);
    };
    const std::vector<std::string> stats = {
        stat(a.stats.lines, b.stats.lines, "lines"),
        stat(a.stats.spanCount, b.stats.spanCount, "spanCount"),
        stat(a.stats.concealRanges, b.stats.concealRanges, "concealRanges"),
        stat(a.stats.linesWithConceal, b.stats.linesWithConceal, "linesWithConceal"),
        stat(a.stats.fencedBlocks, b.stats.fencedBlocks, "fencedBlocks"),
        stat(a.stats.invalidRanges, b.stats.invalidRanges, "invalidRanges"),
        stat(a.stats.markerMismatch, b.stats.markerMismatch, "markerMismatch"),
        stat(a.stats.duplicateRanges, b.stats.duplicateRanges, "duplicateRanges"),
        stat(a.stats.mergedRanges, b.stats.mergedRanges, "mergedRanges"),
        same(a.stats.usedMd4c == b.stats.usedMd4c, "stats.usedMd4c"),
        same(a.stats.linkRefDef == b.stats.linkRefDef, "stats.linkRefDef"),
        same(a.stats.htmlBlockRisk == b.stats.htmlBlockRisk, "stats.htmlBlockRisk"),
        // partial 是"这份计划怎么来的"标记，全量恒 false，不参与等价比较。
    };
    for (const std::string& check : stats) {
        if (!check.empty()) {
            return check;
        }
    }
    return {};
}

struct PlanCaseOutcome {
    bool hit = false;
    std::string reason;
    std::string diff;
};

// 一次"编辑 → 局部 vs 全量"的完整验收。
//   expectHit = 该编辑**必须**命中局部；false = 必须回退（原因串一并断言可读）。
PlanCaseOutcome runPlanCase(const std::string& label,
                            const std::string& oldText,
                            int byteBeg,
                            const std::string& removed,
                            const std::string& inserted,
                            bool expectHit) {
    PlanCaseOutcome outcome;
    const auto expect = [&](bool ok, const std::string& what) { check(ok, label + "：" + what); };
    expect(removed.empty() || oldText.compare(static_cast<std::size_t>(byteBeg), removed.size(),
                                              removed) == 0,
           "样例的 removed 必须真的在该字节位置");
    const std::string newText = oldText.substr(0, static_cast<std::size_t>(byteBeg)) + inserted +
                                oldText.substr(static_cast<std::size_t>(byteBeg + removed.size()));
    const neo::LpPlan oldPlan = neo::buildLpPlan(oldText);
    const neo::LpPlan fullPlan = neo::buildLpPlan(newText);
    const neo::LpTextEdit edit = makePlanEdit(
        newText, byteBeg, byteBeg + static_cast<int>(removed.size()),
        byteBeg + static_cast<int>(inserted.size()), removed, inserted);
    const neo::LpPlanDebugStats before = neo::planDebugStats();
    neo::LpPlan partialPlan;
    std::string reason;
    outcome.hit = neo::buildLpPlanPartial(newText, oldText, oldPlan, edit, partialPlan, &reason);
    outcome.reason = reason;
    const neo::LpPlanDebugStats after = neo::planDebugStats();
    // 计数必须正好 +1 落在命中的那一侧（不允许"既没命中也没回退"的静默路径）。
    expect(after.partial - before.partial == (outcome.hit ? 1ULL : 0ULL),
           "局部命中计数应为 " + std::to_string(outcome.hit ? 1 : 0) + "（实际 " +
               std::to_string(after.partial - before.partial) + "）");
    expect(after.fallback - before.fallback == (outcome.hit ? 0ULL : 1ULL),
           "回退计数应为 " + std::to_string(outcome.hit ? 0 : 1) + "（实际 " +
               std::to_string(after.fallback - before.fallback) + "）");
    expect(outcome.hit == expectHit,
           expectHit ? "应当命中局部，却回退了（原因：" + reason + "）"
                     : "应当回退全量，却命中了局部");
    if (outcome.hit) {
        outcome.diff = planFieldDiff(partialPlan, fullPlan);
        expect(outcome.diff.empty(), "局部结果必须与全量逐字段等价，差在 " + outcome.diff);
    } else {
        expect(!reason.empty(), "回退必须给出可读原因");
    }
    return outcome;
}

// 定位：返回 needle 在文本中的字节位置（断言样例没写错）。
int at(const std::string& text, const std::string& needle) {
    const std::size_t pos = text.find(needle);
    check(pos != std::string::npos, "样例里应当能找到 " + needle);
    return pos == std::string::npos ? 0 : static_cast<int>(pos);
}

void testPartialProofBoundaries() {
    const std::string oldText = "# heading\n\nprefix **bold**\n\nEDIT\n\ntail 中文 😀\n";
    const int byte = at(oldText, "EDIT");
    const std::string newText = oldText.substr(0, byte) + "换" + oldText.substr(byte);
    const auto previous = neo::buildLpPlan(oldText);
    const auto valid = makePlanEdit(newText, byte, byte, byte + 3, {}, "换");
    const auto reject = [&](neo::LpTextEdit edit, std::string text, const char* label) {
        neo::LpPlan result;
        std::string reason;
        check(!neo::buildLpPlanPartial(text, oldText, previous, edit, result, &reason), label);
        check(!reason.empty(), std::string(label) + " rejection reason");
    };
    auto wrong = valid;
    ++wrong.firstLine;
    reject(wrong, newText, "partial rejects wrong first source line");
    wrong = valid;
    ++wrong.newLastLine;
    ++wrong.oldTailLine; // preserve total line-count equation, still wrong edit range
    reject(wrong, newText, "partial rejects wrong last source line with valid line count");
    wrong = valid;
    wrong.byteBeg = -1;
    reject(wrong, newText, "partial rejects negative byte boundary");
    wrong = valid;
    wrong.newEnd = static_cast<int>(newText.size()) + 1;
    reject(wrong, newText, "partial rejects out-of-document byte boundary");
    auto foreign = newText;
    foreign[0] = 'x';
    reject(valid, foreign, "partial rejects foreign prefix edit");
    foreign = newText;
    foreign.back() = 'x';
    reject(valid, foreign, "partial rejects foreign suffix edit");
    for (const std::string ending : {std::string{}, std::string{"\n"}}) {
        const std::string doc = "# h\n\nprefix\n\ntail" + ending;
        runPlanCase("EOF CJK insert", doc, static_cast<int>(doc.size()), {}, "中文", true);
    }
    runPlanCase("new reference risk", oldText, byte, {}, "[x]: /url\n", false);
    runPlanCase("new HTML risk", oldText, byte, {}, "<!--\n", false);
}

void testPartialOracle() {
    int hits = 0;
    int fallbacks = 0;
    const auto run = [&](const std::string& label, const std::string& doc, int byteBeg,
                         const std::string& removed, const std::string& inserted,
                         bool expectHit) {
        const PlanCaseOutcome outcome =
            runPlanCase(label, doc, byteBeg, removed, inserted, expectHit);
        if (outcome.hit) {
            ++hits;
        } else {
            ++fallbacks;
        }
        if (!outcome.hit) {
            std::printf("    · %-30s 回退：%s\n", label.c_str(), outcome.reason.c_str());
        }
    };

    // ── ① 安全的普通正文：必须命中 ────────────────────────────────────────
    const std::string kSafeDoc =
        "# 一级标题\n"
        "\n"
        "第一段正文，带 **粗体**、`代码` 与 [链接](https://a.b)。\n"
        "\n"
        "第二段正文，用于单点编辑。\n"
        "\n"
        "- 列表项一\n"
        "  的续行（同一项）\n"
        "- [ ] 待办项\n"
        "\n"
        "> 引用第一行\n"
        "> 引用第二行\n"
        "\n"
        "| 列甲 | 列乙 |\n"
        "| --- | --- |\n"
        "| a1 | b1 |\n"
        "\n"
        "```cpp\n"
        "int a = 1;\n"
        "```\n"
        "\n"
        "尾段正文。\n";
    run("正文里插字（含 span）", kSafeDoc, at(kSafeDoc, "单点编辑"), "", "插字", true);
    run("正文里插 emoji+中文（UTF-8 字节位移）", kSafeDoc, at(kSafeDoc, "单点编辑"), "",
        "\xF0\x9F\x98\x80\xE6\x8F\x92\xE5\xAD\x97", true);
    run("首行标题插字", kSafeDoc, at(kSafeDoc, "一级标题"), "", " X", true);
    run("末段插字", kSafeDoc, at(kSafeDoc, "尾段正文"), "", "尾巴", true);
    run("列表续行插字", kSafeDoc, at(kSafeDoc, "的续行"), "", "补", true);
    run("引用行插字", kSafeDoc, at(kSafeDoc, "引用第二行"), "", "补", true);
    run("任务行文字插字", kSafeDoc, at(kSafeDoc, "待办项"), "", "补", true);
    run("表格单元格改字", kSafeDoc, at(kSafeDoc, "| a1 |") + 2, "a1", "a1x", true);
    run("代码块后的尾段插字", kSafeDoc, at(kSafeDoc, "尾段正文"), "", "补", true);
    // 换行增删：行号平移（deltaLines）与字节平移（deltaBytes）都要被 oracle 压到。
    // ① 在**行首**插空行：旧行 oldTail 恰好可整行复用（后缀锚点成立）→ 必须命中。
    run("行首插出空行（增行）", kSafeDoc, kSafeDoc.find('\n', at(kSafeDoc, "尾段正文。")) + 1, "",
        "\n\n", true);
    // ② 删掉两个段落之间的一个换行：同样锚点成立 → 必须命中（deltaLines = -1）。
    const int kMergeBlank = static_cast<int>(kSafeDoc.find("\n\n", at(kSafeDoc, "单点编辑"))) + 1;
    run("删掉段间换行（减行）", kSafeDoc, kMergeBlank, "\n", "", true);
    // ③ 在**行尾换行前**插新段：末尾那条旧行会被劈开、不再"可整行复用"，后缀锚点
    //    不成立 —— 与 T4 的装饰增量同一个判据，按约定回退全量（消费方校验、不满足就全量）。
    run("行尾插出新段（增行，锚点不成立）", kSafeDoc,
        kSafeDoc.find('\n', at(kSafeDoc, "尾段正文。")), "", "\n新插入的一段。\n", false);
    run("删掉整段（减行）", kSafeDoc, at(kSafeDoc, "第二段正文"),
        "第二段正文，用于单点编辑。\n\n", "", true);

    // ── ② 围栏（代码块）：进块 / 改围栏必须回退，块外必须命中 ─────────────
    run("改代码块内容（块内）", kSafeDoc, at(kSafeDoc, "int a = 1;"), "", "int b = 2;", false);
    run("改开围栏 info 串", kSafeDoc, at(kSafeDoc, "cpp"), "cpp", "cxx", false);
    run("改闭围栏行", kSafeDoc, at(kSafeDoc, "```\n\n尾段"), "```", "``", false);

    // ── ③ setext 标题（下划线行 + 配对文本行同属一个局部）──────────────────
    const std::string kSetextDoc =
        "正文一\n"
        "\n"
        "标题文字\n"
        "=========\n"
        "\n"
        "正文二\n";
    run("setext 下的正文插字", kSetextDoc, at(kSetextDoc, "正文二"), "", "补", true);
    run("setext 标题文本行插字", kSetextDoc, at(kSetextDoc, "标题文字"), "", "补", true);
    run("setext 上方正文插字", kSetextDoc, at(kSetextDoc, "正文一"), "", "补", true);

    // ── ④ frontmatter：段外命中、改字段命中；让 frontmatterEnd 漂移必须回退 ──
    const std::string kFrontDoc =
        "---\n"
        "title: 测试\n"
        "tags: [a, b]\n"
        "---\n"
        "\n"
        "# 标题\n"
        "\n"
        "正文段落。\n";
    run("frontmatter 后的正文插字", kFrontDoc, at(kFrontDoc, "正文段落"), "", "补", true);
    run("改 frontmatter 字段", kFrontDoc, at(kFrontDoc, "测试"), "测试", "改名", true);
    // 插在"正文。"这一行的行尾换行之前 → 新文本多出一条**恰好是 `---` 的行**，
    // frontmatterEnd 从 -1 跳到它，**区间外**的行会从 Divider/Text 变成 Frontmatter
    // → Pass A 全篇核对必须当场回退。
    const std::string kOpenFrontDoc =
        "---\n"
        "title: T\n"
        "\n"
        "正文。\n"
        "\n"
        "尾段。\n";
    run("插出 frontmatterEnd 漂移", kOpenFrontDoc,
        static_cast<int>(kOpenFrontDoc.find('\n', at(kOpenFrontDoc, "正文。"))), "", "\n---",
        false);

    // ── ⑤ 标题 / 章节表（section* 由收尾两遍整表重算，仍要逐字段相等）────────
    const std::string kSectionDoc =
        "# 甲\n"
        "\n"
        "正文甲\n"
        "\n"
        "## 乙\n"
        "\n"
        "正文乙\n"
        "\n"
        "# 丙\n"
        "\n"
        "正文丙\n";
    run("章节体内插字", kSectionDoc, at(kSectionDoc, "正文乙"), "", "补", true);
    run("章节体内插出新标题", kSectionDoc, at(kSectionDoc, "正文乙"), "", "\n## 新标题\n", true);
    run("删掉一个标题", kSectionDoc, at(kSectionDoc, "## 乙"), "## 乙\n\n", "", true);

    // ── ⑥ 链接引用定义：定义前 / 定义后的任何编辑都必须回退 ─────────────────
    const std::string kRefDoc =
        "定义之前的正文。\n"
        "\n"
        "[foo]: /url\n"
        "\n"
        "引用 [foo] 的正文。\n";
    run("引用定义之前改字", kRefDoc, at(kRefDoc, "定义之前"), "", "补", false);
    run("引用定义之后改字", kRefDoc, at(kRefDoc, "引用 [foo]"), "", "补", false);

    // ── ⑦ 跨空行的 HTML 块：任何编辑都必须回退 ─────────────────────────────
    const std::string kHtmlDoc =
        "段落一\n"
        "\n"
        "<!-- 注释\n"
        "跨空行\n"
        "-->\n"
        "\n"
        "段落二\n";
    run("HTML 注释块存在时改字", kHtmlDoc, at(kHtmlDoc, "段落一"), "", "补", false);

    // ── ⑧ 未闭合围栏：块外命中、块内回退 ───────────────────────────────────
    const std::string kUnclosedDoc =
        "段落一\n"
        "\n"
        "```\n"
        "代码行\n";
    run("未闭合块之前的正文插字", kUnclosedDoc, at(kUnclosedDoc, "段落一"), "", "补", true);
    run("未闭合块之内改字", kUnclosedDoc, at(kUnclosedDoc, "代码行"), "", "补", false);

    // ── ⑨ 局部范围覆盖整篇（没有可复用的区间外行）→ 回退 ───────────────────
    const std::string kNoBlankDoc =
        "第一行正文\n"
        "第二行正文\n"
        "第三行正文";  // 末行不带换行：整篇就是一个 run
    run("无空行文档改中间行", kNoBlankDoc, at(kNoBlankDoc, "第二行正文"), "", "补", false);

    // ── ⑩ 合并 / 拆分段落：局部要扩成"完整的段落" ───────────────────────────
    const std::string kMergeDoc = "段甲\n\n段乙\n\n段丙\n";
    const int mergeAt = static_cast<int>(kMergeDoc.find("\n\n", at(kMergeDoc, "段甲"))) + 1;
    run("删空行合并两段", kMergeDoc, mergeAt, "\n", "", true);
    // 在"段乙"的行首插空行 → 后缀锚点成立，命中；在它**行中间**插空行会把旧行劈开，
    // 锚点不成立 → 与 T4 同判据回退全量。
    run("段首插空行（增行）", kMergeDoc, at(kMergeDoc, "段乙"), "", "\n\n", true);
    run("段中插空行（锚点不成立）", kMergeDoc, at(kMergeDoc, "段乙") + 3, "", "\n\n", false);

    std::printf("[T5] oracle 用例：局部命中 %d 次、回退 %d 次\n", hits, fallbacks);
    check(hits >= 10,
          "应当有足够多的安全正文编辑命中局部路径（实际 " + std::to_string(hits) + "）");
    check(fallbacks >= 6,
          "应当有足够多的复杂情形回退全量（实际 " + std::to_string(fallbacks) + "）");
}

// ── T5 + T4 的接缝：走真实漏斗建计划，缓存链与装饰增量都不许被破坏 ──────────
void testPartialViaCache() {
    const std::string oldText =
        "# 标题\n"
        "\n"
        "第一段正文，带 **粗体**。\n"
        "\n"
        "第二段正文，用于单点编辑。\n";
    const char* fontFamily = "Microsoft YaHei";
    const float fontSize = 16.0f;
    const neo::EditorColors& colors = neo::editorColors(neo::ThemeMode::Dark);
    const components::MarkdownStyle style =
        neo::markdownStyle(fontSize, fontFamily, "monospace", colors);

    IncModel::InputState state;
    IncModel::loadDocument(state, oldText);

    // 基线：全量建计划 + 建装饰链（与 decorationsForEditor 的调用形态一致）。
    components::input_detail::DecoratorEditInfo infoA;
    infoA.textRevision = state.textRevision;
    infoA.committed = true;
    infoA.edit = nullptr;
    const neo::LpPlan& planA = neo::lp::cachedPlan(oldText, &infoA);
    const unsigned long long versionA = neo::lp::planCache().version;
    check(!planA.stats.partial, "基线计划应当是全量建的");
    const std::vector<IncDeco>& decoA = neo::lp::cachedDecorations(
        planA, neo::lp::planCache().version, state.cursor, style, fontFamily, neo::ThemeMode::Dark,
        {}, nullptr, oldText, &colors, &infoA);
    check(!decoA.empty(), "基线装饰表不应为空");

    // 编辑：真实公共漏斗写出 pendingEdit。
    IncModel::moveCursorTo(state, at(oldText, "单点编辑"), false);
    const neo::LpPlanDebugStats planBefore = neo::planDebugStats();
    const neo::lp::DecorationDebugStats decorBefore = neo::lp::decorationDebugStats();
    IncModel::insertAtCursor(state, "插字");
    check(state.pendingEdit.valid && state.pendingEdit.revision == state.textRevision,
          "公共漏斗应当写下有效的编辑区间");

    components::input_detail::DecoratorEditInfo infoB;
    infoB.textRevision = state.textRevision;
    infoB.committed = true;
    infoB.edit = &state.pendingEdit;
    // 编辑链完整 → 这一步必须命中局部。
    const neo::LpPlan& planB = neo::lp::cachedPlan(state.text, &infoB);
    const neo::LpPlanDebugStats planAfter = neo::planDebugStats();
    check(planAfter.partial - planBefore.partial == 1 &&
              planAfter.fallback == planBefore.fallback,
          "真实漏斗的单点编辑应当命中局部（partial=" +
              std::to_string(planAfter.partial - planBefore.partial) + " fallback=" +
              std::to_string(planAfter.fallback - planBefore.fallback) + " 原因：" +
              neo::planRejectReason() + "）");
    check(planB.stats.partial, "命中局部的计划应带 partial 标记");
    // ── T4 的前提：version 恰好 +1、prevPlan 正是上一代 ──
    check(neo::lp::planCache().version == versionA + 1,
          "文本变一次 planVersion 必须恰好 +1（局部路径同样只加这一次）");
    check(neo::lp::planCache().prevValid && neo::lp::planCache().prevVersion == versionA,
          "prevPlan 必须仍是上一代（T4 的 planChain 依赖它）");
    check(&neo::lp::planCache().prevPlan != &planB, "prevPlan 与当前代不能是同一份对象");
    // 与全量逐字段等价（走缓存出来的那份也要过 oracle）。
    check(planFieldDiff(planB, neo::buildLpPlan(state.text)).empty(),
          "缓存里的局部计划必须与全量逐字段等价");
    // ── T4 不受影响：装饰仍走区间增量 ──
    const std::vector<IncDeco>& decoB = neo::lp::cachedDecorations(
        planB, neo::lp::planCache().version, state.cursor, style, fontFamily, neo::ThemeMode::Dark,
        {}, nullptr, state.text, &colors, &infoB);
    const neo::lp::DecorationDebugStats decorAfter = neo::lp::decorationDebugStats();
    check(decorAfter.incremental - decorBefore.incremental == 1 &&
              decorAfter.fallback == decorBefore.fallback && decorAfter.full == decorBefore.full,
          "局部计划之上装饰仍应走区间增量（incremental=" +
              std::to_string(decorAfter.incremental - decorBefore.incremental) + "）");
    std::vector<IncDeco> refDeco;
    neo::lp::buildDecorations(planB, style, state.cursor, refDeco, fontFamily, {}, nullptr,
                              state.text, &colors);
    int diffCount = 0;
    if (decoB.size() == refDeco.size()) {
        for (std::size_t i = 0; i < refDeco.size(); ++i) {
            if (!decorationFieldDiff(decoB[i], refDeco[i]).empty()) {
                ++diffCount;
            }
        }
    } else {
        diffCount = 1;
    }
    check(diffCount == 0,
          "局部计划上的装饰必须与全量逐字段等价，差 " + std::to_string(diffCount) + " 行");
    // 文本没变再来一次（光标移动那一帧）：命中缓存，version 不动。
    const unsigned long long versionB = neo::lp::planCache().version;
    neo::lp::cachedPlan(state.text, &infoB);
    check(neo::lp::planCache().version == versionB, "同文本重建缓存不应当推进 version");
}

// ── T12/T5：安全正文 2k / 20k / 100k 行的全量 vs 局部耗时（只记录不断言毫秒）──
void testPartialPerf() {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    const auto makeDoc = [](int lineCount) {
        // 每段两行（正文 + 空行），段与段之间空行隔开 —— 正是局部路径的"安全正文"。
        std::string text;
        int lines = 0;
        int index = 0;
        while (lines < lineCount) {
            text += "第 " + std::to_string(index) +
                    " 段普通正文，用于 T5 局部重解析与全量的耗时对照。\n\n";
            lines += 2;
            ++index;
        }
        return text;
    };
    for (const int lineCount : {2000, 20000, 100000}) {
        const std::string text = makeDoc(lineCount);
        // 局部：在中段正文里插两个字（"全量 vs 局部"必须对**同一份新文本**比）。
        const int byteBeg = at(text, "第 " + std::to_string(lineCount / 4) + " 段");
        const std::string inserted = "插字";
        const std::string newText = text.substr(0, static_cast<std::size_t>(byteBeg)) + inserted +
                                    text.substr(static_cast<std::size_t>(byteBeg));
        const neo::LpPlan oldPlan = neo::buildLpPlan(text);
        const neo::LpTextEdit edit =
            makePlanEdit(newText, byteBeg, byteBeg, byteBeg + static_cast<int>(inserted.size()),
                         "", inserted);
        // 全量（对新文本，取 3 次最小值压掉机器抖动）。
        double fullMs = 0.0;
        neo::LpPlan fullPlan;
        for (int i = 0; i < 3; ++i) {
            const Clock::time_point begin = Clock::now();
            fullPlan = neo::buildLpPlan(newText);
            const double elapsed = ms(begin, Clock::now());
            if (i == 0 || elapsed < fullMs) {
                fullMs = elapsed;
            }
        }
        double partialMs = 0.0;
        neo::LpPlan partialPlan;
        bool hit = false;
        std::string reason;
        for (int i = 0; i < 3; ++i) {
            const Clock::time_point begin = Clock::now();
            hit = neo::buildLpPlanPartial(newText, text, oldPlan, edit, partialPlan, &reason);
            const double elapsed = ms(begin, Clock::now());
            if (i == 0 || elapsed < partialMs) {
                partialMs = elapsed;
            }
        }
        check(hit, std::to_string(lineCount) + " 行安全正文的单点编辑应当命中局部（原因：" +
                       reason + "）");
        const std::string perfDiff = planFieldDiff(partialPlan, fullPlan);
        check(perfDiff.empty(),
              std::to_string(lineCount) + " 行的局部结果必须与全量逐字段等价，差在 " + perfDiff);
        std::printf("[T12/T5] %d 行（%zu 字节）：buildLpPlan 全量 %.1f ms / 局部 %.1f ms（%s）\n",
                    lineCount, text.size(), fullMs, partialMs, hit ? "命中局部" : "回退全量");
    }
}

    // ── T4 审计 HIGH-2：行中 / 行尾回车（后缀锚点对不齐的那条路）────────────────
// recordPendingEdit 没有旧全文，newLastLine/oldTailLine 是按"新段除末字节外的换行数"
// 倒推的：在**行中 / 行尾**敲回车时，被回车劈开的那条旧行既没被新尾段盖住、也没被
// 旧尾段跳过，后缀锚点 `prev.lines[oldTail].srcBeg + deltaBytes ==
// plan.lines[newLast+1].srcBeg` 必然错位 —— 改前这条路一律退全量，"回车换行"这个
// 最高频的编辑永远吃不到增量。修法：buildDecorationsIncremental 在后缀自检里
// **成对**推进 newLast/oldTail（每 +1 = 多重算一条新行、同时多跳过一条旧行），
// 推进后仍要过"剩余行数恒等式"与 ④ 区间外逐行结构等价 —— 判据一条都没绕开。
//
// 用例（装饰表与行表都由 runIncrementalCase 与"对新文本全量重建"逐字段对照）：
//   ① 行中回车：ab\ncd（同段落两行）、多段普通正文、10k 行中段 —— 结构没变 →
//      装饰必须真增量（incremental≥1）且 fallback == 0；
//   ② 行尾回车：ab\ncd 与独立普通正文（每段一行、段间空行）—— 被劈开的那条旧行
//      落在重算区间 / ±1 邻域 / 旧活动块的映射里（**重算而非复用**），结构证明因此
//      仍然成立 → 也必须真增量、fallback == 0（改前正是这里被锚点拒掉）；
//   ③ 结构确实不安全的一例：回车劈开**标题行**时，区间外正文行的 sectionHeadingLine
//      指向被吃掉的旧行号，EditRefMap 无法折算（mapLine → -2）→ ④ 判据不过 → 按
//      §9 约定回退全量。这一例断言回退，不为通过测试硬吃增量。
void testEnterIncremental() {
    // ① 行中回车：ab\ncd（同一个段落的两行）
    runIncrementalCase(
        "行中回车-两行段落", "ab\ncd\n",
        {{"行中回车", [](IncModel::InputState& state) {
             IncModel::moveCursorTo(state, 1, false);  // a|b
             IncModel::insertAtCursor(state, "\n");
         }}},
        nullptr, true, true);

    // ② 行尾回车 + 同一份 ab\ncd：锚点错位（改前必退），推进后被劈开的尾行与旧段落
    //    其余行都在重算集里（±1 邻域 + 旧活动块映射），复用的只有平移后等价的行。
    runIncrementalCase(
        "行尾回车-两行段落", "ab\ncd\n",
        {{"行尾回车", [](IncModel::InputState& state) {
             IncModel::moveCursorTo(state, 2, false);  // ab|
             IncModel::insertAtCursor(state, "\n");
         }}},
        nullptr, true, true);

    // 普通正文样例：每段一行、段与段之间空行隔开（markdown 结构的"安全形态"）。
    const std::string kPlainBody =
        "第一段正文，只有一行。\n"
        "\n"
        "第二段正文，只有一行。\n"
        "\n"
        "第三段正文，只有一行。\n";

    // ② 行尾回车 + 普通正文：只是多插一条空行，区间外的空行/末段逐字段等价。
    runIncrementalCase(
        "行尾回车-普通正文", kPlainBody,
        {{"行尾回车", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("第一段正文");
             const std::size_t eol = state.text.find('\n', pos);
             IncModel::moveCursorTo(state, static_cast<int>(eol), false);
             IncModel::insertAtCursor(state, "\n");
         }}},
        nullptr, true, true);

    // ① 行中回车 + 普通正文：单行段落被劈成两行（仍在同一段内），结构不变。
    runIncrementalCase(
        "行中回车-普通正文", kPlainBody,
        {{"行中回车", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("第二段正文");
             IncModel::moveCursorTo(state, static_cast<int>(pos) + 3, false);
             IncModel::insertAtCursor(state, "\n");
         }}},
        nullptr, true, true);

    // ③ 结构不安全的一例：回车劈开**标题行**（标题下留空行，让尾行不与被劈开的
    //    标题残段并进同一个块）。推进后区间外的正文行仍要按旧坐标引用
    //    sectionHeadingLine = 1，而旧行 1 已被尾区间吃掉 → EditRefMap 折不出来
    //    （mapLine → -2）→ ④ 判据不过 → 按 §9 约定回退全量（布局层按文本差分自己
    //    推区间，不依赖 PendingTextEdit 的尾锚点，仍走增量）。
    const std::string kSplitHeading =
        "前言\n"
        "# 标题\n"
        "\n"
        "正文一\n"
        "正文二\n";
    runIncrementalCase(
        "行中回车-劈开标题行（结构不安全→装饰回退）", kSplitHeading,
        {{"标题行中回车", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("# 标题");
             IncModel::moveCursorTo(state, static_cast<int>(pos) + 2, false);
             IncModel::insertAtCursor(state, "\n");
         }}},
        nullptr, false, true);
    // 回退原因必须是"结构折算不出来"，不能是别的偶然原因（否则这条断言在测别的东西）。
    check(neo::lp::decorationRejectReason().find("sectionHeadingLine") != std::string::npos,
          "劈开标题行的回退原因应指向无法折算的章节引用（实际：'" +
              neo::lp::decorationRejectReason() + "'）");

    // ① 10k 行（5000 段 × 两行）：中段行中回车 —— 规模上去后联合推进仍要保持
    //    增量命中、fallback == 0，且新表与全量逐字段一致。
    std::string big;
    big.reserve(5000 * 64);
    for (int i = 0; i < 5000; ++i) {
        big += "第 " + std::to_string(i) + " 段普通正文，用于行中回车的增量验收。\n\n";
    }
    runIncrementalCase(
        "行中回车-10k 行", big,
        {{"中段行中回车", [](IncModel::InputState& state) {
             const std::size_t pos = state.text.find("第 2500 段");
             IncModel::moveCursorTo(state, static_cast<int>(pos) + 4, false);
             IncModel::insertAtCursor(state, "\n");
         }}},
        nullptr, true, true);
}

// ── T13 收尾：主题文件版本（themeRevision）必须进装饰缓存键 ──────────────────
// 同 ThemeMode、只换主题文件（activeTheme().revision++）时，键里其它项（计划版本、
// 光标、字号、字体、目录、折叠）一个都不动 —— revision 不进键就会命中上一套主题的
// 表，标题 / 代码 / 引用继续画旧色、主题字体也一并遗留（app_actions.cpp 里记的 T13 缺口）。
// 三段：首次（内置配色）→ 切换（叠主题覆盖）→ reset（回内置），每步都断言：
//   ① 三处语义色**立即**等于当期配色表（标题 heading / 代码 codeText / 引用竖条 accent）；
//   ② 缓存走 full（全量重建），不许命中旧表、也不许拐进上一代增量；
//   ③ 输出与"按当期样式全量 buildDecorations"逐字段等价（颜色/字体不遗留）。
// 前两步**不传**新参数（走默认值）—— 默认值必须取当期 themeRevision() 而不是固定 0，
// 否则真实 UI 刷不出来；第三步显式传入，验证显式通路（editor_view 就是这么调的）。
void testThemeRevisionDecorationKey() {
    const std::string doc =
        "# 标题行\n"
        "\n"
        "> 引用行\n"
        "\n"
        "```cpp\n"
        "int a = 1;\n"
        "```\n";
    const char* fontFamily = "Microsoft YaHei";
    const std::string docDir;
    const int cursor = 0;

    // 三步共用同一份计划（文本不动 → planVersion 恒定），只有主题版本在动。
    const neo::LpPlan& plan = neo::lp::cachedPlan(doc);
    const unsigned long long planVersion = neo::lp::planCache().version;
    components::input_detail::DecoratorEditInfo info;
    info.textRevision = 0;
    info.committed = true;
    info.edit = nullptr;

    int headingLine = -1;
    int quoteLine = -1;
    int codeLine = -1;
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        const neo::LpLine& line = plan.lines[i];
        if (line.kind == neo::LpKind::Heading && headingLine < 0) {
            headingLine = static_cast<int>(i);
        }
        if (line.quoteDepth > 0 && quoteLine < 0) {
            quoteLine = static_cast<int>(i);
        }
        if (line.kind == neo::LpKind::Code && !line.codeFence && codeLine < 0) {
            codeLine = static_cast<int>(i);
        }
    }
    check(headingLine >= 0 && quoteLine >= 0 && codeLine >= 0,
          "主题键样例应解析出标题行 / 引用行 / 代码内容行");

    const auto checkThreeColors = [&](const std::vector<IncDeco>& table,
                                      const neo::EditorColors& colors, const char* label) {
        if (table.size() != plan.lines.size()) {
            check(false, std::string(label) + "：装饰行数应与计划一致（实际 " +
                             std::to_string(table.size()) + "）");
            return;
        }
        check(sameColor(table[static_cast<std::size_t>(headingLine)].textColor, colors.heading),
              std::string(label) + "：标题行文字色应立即等于当期 heading");
        check(sameColor(table[static_cast<std::size_t>(codeLine)].textColor, colors.codeText),
              std::string(label) + "：代码行文字色应立即等于当期 codeText");
        check(sameColor(table[static_cast<std::size_t>(quoteLine)].box.barColor,
                        colors.markdownAccent),
              std::string(label) + "：引用竖条应立即等于当期 markdownAccent");
    };
    const auto checkFullRebuilt = [&](const neo::lp::DecorationDebugStats& before,
                                      const neo::lp::DecorationDebugStats& after,
                                      const char* label) {
        check(after.full - before.full == 1 && after.incremental == before.incremental &&
                  after.fallback == before.fallback,
              std::string(label) + "：主题版本变化必须强制全量重建（full=" +
                  std::to_string(after.full - before.full) +
                  " incremental=" + std::to_string(after.incremental - before.incremental) +
                  " fallback=" + std::to_string(after.fallback - before.fallback) + "）");
    };
    // 两张装饰表逐字段对照，返回"差在哪一行 / 哪个字段"（空串 = 逐字段等价）。
    const auto tableDiff = [](const std::vector<IncDeco>& a, const std::vector<IncDeco>& b) {
        if (a.size() != b.size()) {
            return "行数 " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const std::string diff = decorationFieldDiff(a[i], b[i]);
            if (!diff.empty()) {
                return "行 " + std::to_string(i) + " 字段 " + diff;
            }
        }
        return std::string();
    };

    neo::ThemeFileData& theme = neo::activeTheme();
    const neo::ThemeFileData saved = theme;
    const unsigned long long revisionBuiltin = neo::themeRevision();

    // ── 首次：内置配色。不传主题版本参数 → 走默认值（当期 revision）。────────
    // 配色**按值拷贝**：editorColors() 返回的是它那张按 revision 失效的静态缓存的引用，
    // 下一次 revision 推进会**就地重建**同一份对象 —— 留引用的话 colors0 会跟着变成
    // 切换后的颜色，"前后应不同"的断言就成了自己比自己。
    const neo::EditorColors colors0 = neo::editorColors(neo::ThemeMode::Dark);
    const components::MarkdownStyle style0 =
        neo::markdownStyle(16.0f, fontFamily, "monospace", colors0);
    const neo::lp::DecorationDebugStats before0 = neo::lp::decorationDebugStats();
    // 注意按值拷贝：cachedDecorations 返回的是**缓存自己的那张表**的引用，下一次调用
    // 就地重建会把这张表覆盖掉 —— 三步要互相对照，必须各留一份快照。
    const std::vector<IncDeco> table0 = neo::lp::cachedDecorations(
        plan, planVersion, cursor, style0, fontFamily, neo::ThemeMode::Dark, docDir, nullptr, doc,
        &colors0, &info);
    checkFullRebuilt(before0, neo::lp::decorationDebugStats(), "首次建表");
    checkThreeColors(table0, colors0, "首次");

    // ── 切换：叠一份主题覆盖（**外观侧不变**：仍是 ThemeMode::Dark，字号字体全不动）。─
    theme.path = "<unit-test-theme>.json";
    theme.version = 1;
    theme.name = "单元测试主题";
    theme.baseLight = false;  // 叠在暗色上
    theme.colors.clear();
    theme.colors["heading"] = neo::rgba(1.0f, 0.0f, 0.0f);
    theme.colors["codeText"] = neo::rgba(0.0f, 1.0f, 0.0f);
    theme.colors["markdownAccent"] = neo::rgba(0.0f, 0.0f, 1.0f);
    theme.typography = neo::ThemeTypography{};  // 排版不覆盖：字号/字体键保持不变
    ++theme.revision;
    const neo::EditorColors colors1 = neo::editorColors(neo::ThemeMode::Dark);
    const components::MarkdownStyle style1 =
        neo::markdownStyle(16.0f, fontFamily, "monospace", colors1);
    check(!sameColor(colors1.heading, colors0.heading) &&
              !sameColor(colors1.codeText, colors0.codeText) &&
              !sameColor(colors1.markdownAccent, colors0.markdownAccent),
          "主题覆盖应真的改到 heading/codeText/markdownAccent 三项配色");
    const neo::lp::DecorationDebugStats before1 = neo::lp::decorationDebugStats();
    const std::vector<IncDeco> table1 = neo::lp::cachedDecorations(
        plan, planVersion, cursor, style1, fontFamily, neo::ThemeMode::Dark, docDir, nullptr, doc,
        &colors1, &info);
    checkFullRebuilt(before1, neo::lp::decorationDebugStats(), "切换主题文件");
    checkThreeColors(table1, colors1, "切换主题文件");
    // 缓存颜色不遗留：与"按当期样式全量重建"逐字段等价，且与上一代的表真的不同。
    std::vector<IncDeco> fresh1;
    neo::lp::buildDecorations(plan, style1, cursor, fresh1, fontFamily, docDir, nullptr, doc,
                              &colors1);
    const std::string freshDiff = tableDiff(table1, fresh1);
    check(freshDiff.empty(),
          "切换主题文件后的缓存表必须与当期全量重建逐字段等价（颜色/字体不遗留），差在 " +
              freshDiff);
    int colorDrift = 0;
    for (std::size_t i = 0; i < table0.size() && i < table1.size(); ++i) {
        if (!sameColor(table0[i].textColor, table1[i].textColor)) {
            ++colorDrift;
        }
    }
    check(colorDrift > 0, "切换主题文件后至少一行的文字色应与上一代不同（否则没测到键）");

    // ── reset：卸掉主题回内置配色。显式传入主题版本（editor_view 的调用形态）。──
    // 版本号必须比上一步**新**：回填 saved 会把 revision 一起带回旧值，键相等就会
    // 命中切换那一版的表 —— 那正是"缓存颜色遗留"的形态，所以这里递增到新值。
    theme = saved;
    theme.revision = revisionBuiltin + 2;
    const neo::EditorColors colors2 = neo::editorColors(neo::ThemeMode::Dark);
    const components::MarkdownStyle style2 =
        neo::markdownStyle(16.0f, fontFamily, "monospace", colors2);
    const neo::lp::DecorationDebugStats before2 = neo::lp::decorationDebugStats();
    const std::vector<IncDeco> table2 = neo::lp::cachedDecorations(
        plan, planVersion, cursor, style2, fontFamily, neo::ThemeMode::Dark, docDir, nullptr, doc,
        &colors2, &info, neo::themeRevision());
    checkFullRebuilt(before2, neo::lp::decorationDebugStats(), "reset 主题");
    checkThreeColors(table2, colors2, "reset 主题");
    const std::string resetDiff = tableDiff(table2, table0);
    check(resetDiff.empty(),
          "reset 后的装饰表必须与首次（同为内置配色）逐字段一致，差在 " + resetDiff);
    check(neo::themeRevision() == revisionBuiltin + 2 && theme.path.empty(),
          "样例收尾时主题数据应回到内置（revision 单调不回退）");
}

// ── T16：换到非 markdown 文档时清计划缓存（应用层 reset，version 单调不归零）──
// 场景：正在编辑 .md（计划已建表），随后打开一个 .txt —— 非 markdown 不走装饰，
// decorationsForEditor 早退、cachedPlan 不会再被调用，不清就一直驻留上一篇的
// text / plan / prevPlan。三条判据缺一不可：
//   ① md→txt：reset 之后 planCache 的 valid=false、text 空、当前代与上一代计划都释放；
//   ② version **单调不归零**：清缓存不推进也不回退，之后重建恰好 +1；
//   ③ 清理在**应用层** resetEditorInputState（本用例就从它打进去），组件侧的
//      InputModel::loadDocument 不认识 lp —— 它只搬自己的字段，reset 不许下沉。
void testPlanCacheResetOnNonMarkdown() {
    using InputModel = components::input_detail::InputModel;
    const std::string savedPath = neo::state().path;
    eui::Ui ui;

    // ① md 文档建表（模拟正在编辑 .md）。
    neo::state().path = "unit_t16.md";
    const std::string mdText = "# T16 标题\n\n正文段落，**加粗**。\n";
    neo::resetEditorInputState(ui, mdText);
    const neo::LpPlan& mdPlan = neo::lp::cachedPlan(mdText);
    check(!mdPlan.lines.empty(), "md 文档应建出计划");
    check(neo::lp::planCache().valid, "md 建表后缓存应有效");
    check(neo::lp::planCache().text == mdText, "缓存应记着 md 文本");
    const unsigned long long version = neo::lp::planCache().version;
    check(version > 0, "建过表的 version 应是非零单调计数");

    // ② 组件侧的 reset 不认识 lp：直接调 loadDocument 搬文本，lp 缓存必须原封不动
    //    （把清理挪进 components 的改法会在这里当场失败 —— 那正是被禁止的方向）。
    InputModel::loadDocument(ui.state<InputModel::InputState>(neo::kEditorInputId), "组件侧文本\n");
    check(neo::lp::planCache().valid, "组件侧 loadDocument 不得清 lp 计划缓存");
    check(neo::lp::planCache().text == mdText, "组件侧 loadDocument 不得改 lp 缓存内容");
    check(neo::lp::planCache().version == version, "组件侧 loadDocument 不得推进 lp version");

    // ③ md → txt（应用层 reset）：必须清内容，但 version 一个字节都不许动。
    neo::state().path = "unit_t16.txt";
    const std::string txtText = "纯文本内容\n";
    neo::resetEditorInputState(ui, txtText);
    check(!neo::lp::planCache().valid, "md→txt 后计划缓存必须失效（valid=false）");
    check(neo::lp::planCache().text.empty(), "md→txt 后缓存文本必须清空（不再驻留 md 正文）");
    check(neo::lp::planCache().plan.lines.empty(), "md→txt 后当前代计划必须释放");
    check(neo::lp::planCache().prevPlan.lines.empty(), "上一代计划也要一并释放（否则驻留只少一半）");
    check(!neo::lp::planCache().prevValid, "上一代必须断开（否则局部重解析会拿空计划当依据）");
    check(neo::lp::planCache().version == version, "清缓存既不得推进 version（更不得归零）");
    check(version != 0, "version 必须非零单调 —— 归零会让老装饰表的 planVersion 键重新命中");

    // ④ 清过之后重建 txt 计划：version 恰好 +1 —— 单调链与 T4 不变式都还活着。
    (void)neo::lp::cachedPlan(txtText);
    check(neo::lp::planCache().valid, "txt 文档照常能建表");
    check(neo::lp::planCache().version == version + 1, "重建恰好 +1（单调递增、不归零）");

    // ⑤ md→md 不清：计划按文本自证，留着无害 —— 把"只在非 md 滞留时清"钉住。
    neo::state().path = "unit_t16_b.md";
    const std::string mdText2 = "# 第二篇\n\n新正文。\n";
    neo::resetEditorInputState(ui, mdText2);
    check(neo::lp::planCache().valid, "md→md 不清缓存（内容按文本键自证）");
    check(neo::lp::planCache().text == txtText, "md→md 保留上一份文本，直到下一次 cachedPlan");

    // 收尾：现场恢复（path 归位，缓存按新文本重建，后续用例不吃这里的状态）。
    neo::state().path = savedPath;
    (void)neo::lp::cachedPlan(mdText2);
}

// ── R1：块间距的生成规则（spaceBefore）──────────────────────────────────────
// Obsidian 的 Live Preview 逐行 top 首尾相接（gap 恒 0）：块间距只在两个地方出现 ——
// ── C1：列表语义 → 呈现数据（计划反例 + 装饰投影）──────────────────────────
// 冻结的语义（与 Obsidian Live Preview / CommonMark 对齐）：
//   1. 有序组的呈现序号 = 组内第一项的字面数字 + 组内偏移 —— `3. a` 后跟 `1. b`
//      呈现 3、4；组内后续项的字面数字不参与呈现；空行不断组（宽松列表）。
//   2. 同层换标记（子弹字符 / 有序⇄无序 / 分隔符 . 与 )）开新组，各自从字面起点数。
//   3. 嵌套各层维护自己的组与序号；浅层项在子列表结束后继续计数。
//   4. 物理续行（缩进续段 / lazy 续段）归属最近的标记行（listItemLine），
//      正文起点 = 标记行的 contentBeg；lazy 续段不能跨空行。
//   5. 任务行沿用复选框机制：不画 marker，续行缩进 = 复选框 advance。
//   6. 引用内的列表照常分组；引用深度进装饰（多条竖条数据）。
void testListSemantics() {
    // ── 计划层：序号重启 / 组起点 / 嵌套 / 引用 / 任务 / 续行 ──
    {
        const std::string doc =
            "3. a\n"      // 0  组起点 = 3 → 呈现 3
            "1. b\n"      // 1  同组（同层同分隔符）→ 3+1 = 4（字面 1 不参与）
            "\n"          // 2
            "7. c\n"      // 3  空行不断组（宽松列表）→ 3+2 = 5（字面 7 不参与）
            "1) d\n"      // 4  分隔符 . → ) 换组 → 从字面 1 起
            "2) e\n";     // 5  同组 → 2
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].listOrdinal == 3 && plan.lines[0].ordered,
              "`3. a` 的呈现序号应为 3（组起点字面）");
        check(plan.lines[1].listOrdinal == 4,
              "`1. b` 应呈现 4（组内起点+1，字面不参与），实际 " +
                  std::to_string(plan.lines[1].listOrdinal));
        check(plan.lines[3].listOrdinal == 5,
              "空行后 `7. c` 仍在同组 → 呈现 5，实际 " +
                  std::to_string(plan.lines[3].listOrdinal));
        check(plan.lines[4].listOrdinal == 1 && plan.lines[5].listOrdinal == 2,
              "换分隔符 `1)` 开新组，后续按 +1 计数");
        for (const int i : {0, 1, 3, 4, 5}) {
            check(plan.lines[static_cast<std::size_t>(i)].listItemLine == i,
                  "标记行的 listItemLine 应记自己（行 " + std::to_string(i) + "）");
        }
        check(plan.lines[0].contentBeg == plan.lines[0].srcBeg + 3,
              "`3. a` 的正文起点应跳过 `3. `");
    }
    {
        // 无序 → 有序：换标记开新组；无序无序号
        const std::string doc = "- u\n1. o\n+ u2\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].listOrdinal == 0 && !plan.lines[0].ordered,
              "无序列表无序号");
        check(plan.lines[1].listOrdinal == 1,
              "`- u` 之后的 `1. o` 开新有序组 → 呈现 1，实际 " +
                  std::to_string(plan.lines[1].listOrdinal));
        check(plan.lines[2].listOrdinal == 0,
              "换子弹字符 `-`→`+` 开新无序组（仍无序号）");
    }
    {
        // 嵌套：各层自己的组；子列表结束后浅层继续计数
        const std::string doc =
            "1. one\n"     // 0 → 1
            "   - sub a\n" // 1 无序
            "   - sub b\n" // 2 无序
            "2. two\n"     // 3 → 2（弹出子层后回到外层组）
            "3. three\n";  // 4 → 3
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].listOrdinal == 1 && plan.lines[3].listOrdinal == 2 &&
                  plan.lines[4].listOrdinal == 3,
              "外层有序组应跨子列表继续计数");
        check(plan.lines[1].listDepth == 2 && plan.lines[1].listOrdinal == 0,
              "嵌套无序项记 listDepth=2 且无序号");
        check(plan.lines[0].listDepth == 1 && plan.lines[3].listDepth == 1,
              "子列表结束后外层回到 listDepth=1");
    }
    {
        // 引用内列表：分组照常 + 引用深度照常
        const std::string doc = "> 1. q1\n> 2. q2\n> > 深一层\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].quoteDepth == 1 && plan.lines[0].listItemLine == 0 &&
                  plan.lines[0].listOrdinal == 1,
              "引用内的 `1. q1` 应有引用深度与序号 1");
        check(plan.lines[1].listItemLine == 1 && plan.lines[1].listOrdinal == 2,
              "引用内第二项呈现 2（组跨引用前缀连续计数）");
        check(plan.lines[2].quoteDepth == 2,
              "嵌套引用深度应传到计划（多条竖条数据）");
    }
    {
        // 任务行 + 物理续行归属（缩进续段 / lazy 续段）
        const std::string doc =
            "- [ ] task\n"   // 0 任务
            "  task cont\n"  // 1 缩进续段（宽度2 ≥ "- " 内容列2）
            "- item\n"       // 2 普通项
            "  item cont\n"  // 3 缩进续段
            "lazy\n"         // 4 lazy 续段（紧贴、无缩进）
            "\n"             // 5
            "after blank\n"; // 6 空行后的顶格行不是续段
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].kind == neo::LpKind::TaskItem && plan.lines[0].listItemLine == 0,
              "任务行是标记行（记自己）");
        check(plan.lines[1].listItemLine == 0 && plan.lines[1].listOrdinal == 0,
              "任务的缩进续段归属任务行");
        check(plan.lines[3].listItemLine == 2 && plan.lines[4].listItemLine == 2,
              "普通项的缩进续段与 lazy 续段都归属最近的标记行");
        check(plan.lines[6].listItemLine == -1,
              "空行后的顶格行不是续段（lazy 不能跨空行）");
        check(plan.lines[2].contentBeg == plan.lines[2].srcBeg + 2 &&
                  plan.lines[3].contentBeg == plan.lines[3].srcBeg + 2,
              "续行的正文起点应剥掉自己的缩进");
    }

    // ── 装饰层：marker / 续行缩进 / 活动块 / 多条竖条 ──
    const components::MarkdownStyle style =
        neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
    using components::input_detail::LineDecoration;
    {
        const std::string doc = "3. a\n1. b\n- u\n\nplain\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        std::vector<LineDecoration> table;
        // 光标放在列表之外的普通段落（行 4）：光标 -1 会落到行 0（首块被当活动块）。
        neo::lp::buildDecorations(plan, style, plan.lines[4].srcBeg, table);
        check(table[0].glyph.codepoint == 0 && table[0].glyph.text == "3.",
              "有序项首行应给文本 marker \"3.\"，实际 \"" + table[0].glyph.text + "\"");
        check(table[1].glyph.text == "4.", "第二项应呈现 \"4.\"（序号重启语义）");
        check(table[2].glyph.text == "•", "无序项应给 \"•\" marker");
        check(table[0].glyph.advance <= 0.0f,
              "marker 的 advance 应留给组件实测（装饰层不给死值）");
        check(table[0].listIndentBeg == plan.lines[0].srcBeg &&
                  table[0].listIndentEnd == plan.lines[0].contentBeg,
              "marker 行的度量源区间应 = [srcBeg, contentBeg)");
        // 光标进项内（活动块）→ 源码露出，不画 marker
        std::vector<LineDecoration> active;
        neo::lp::buildDecorations(plan, style, plan.lines[0].contentBeg, active);
        check(active[0].glyph.text.empty(), "活动块内不画 marker（源码露出）");
        check(active[0].glyph.codepoint == 0,
              "活动块内的普通列表项也不应出现复选框图元");
    }
    {
        const std::string doc =
            "- [ ] task\n"
            "  task cont\n"
            "- item\n"
            "  item cont\n"
            "lazy\n"
            "\n"
            "plain\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        std::vector<LineDecoration> table;
        // 光标放在列表之外的普通段落（行 6，与列表隔一条空行）：
        // "plain" 若紧贴 lazy 续段会被 md4c 并进同一个段落块（活动块）。
        neo::lp::buildDecorations(plan, style, plan.lines[6].srcBeg, table);
        check(table[0].glyph.codepoint != 0 && table[0].glyph.text.empty(),
              "任务行沿用复选框图元，不画文本 marker");
        check(table[1].contentIndent > 0.0f && table[1].listIndentBeg < 0,
              "任务续行的缩进 = 复选框 advance（contentIndent 直给，不经度量源区间）");
        const float want = std::max(4.0f, 16.0f * components::input_detail::kGlyphAdvanceEm);
        check(std::fabs(table[1].contentIndent - want) < 0.001f,
              "任务续行缩进应与复选框 advance 同公式，实际 " +
                  std::to_string(table[1].contentIndent));
        check(table[3].listIndentBeg == plan.lines[2].srcBeg &&
                  table[3].listIndentEnd == plan.lines[2].contentBeg,
              "普通项续行的度量源区间 = owner 的 [srcBeg, contentBeg)");
        check(table[3].contentIndent <= 0.0f && table[4].listIndentBeg >= 0,
              "lazy 续行同样经度量源区间拿缩进");
    }
    {
        // 引用深度 → 多条竖条数据（barCount）
        const std::string doc = "> 一层\n> > 两层\nplain\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        std::vector<LineDecoration> table;
        neo::lp::buildDecorations(plan, style, plan.lines[2].srcBeg, table);
        check(table[0].box.barCount == 1, "单层引用 barCount=1（barCount=0 在渲染层同样按单条处理，兼容历史语义）");
        check(table[1].box.barCount == 2, "两层引用应给 barCount=2");
        check(table[1].box.barWidth > 0.0f && table[1].box.barColor.a > 0.0f,
              "多条竖条沿用单条的宽度与颜色");
    }
    {
        // 列表上下文的**终止**：标题 / 空行后的顶格段落（以及围栏、分隔线、表格）都
        // 结束列表，其后的列表重新从字面起点计数。2026-09-29 截图实测过反例：标题后
        // 的 `1.` 曾被渲染成 4.（组跨标题继续计数）。
        const std::string doc =
            "1. 第一个列表\n"      // 0 → 1
            "\n"                   // 1
            "### 小标题\n"         // 2 打断列表
            "\n"                   // 3
            "1. 新列表\n"          // 4 必须重新从 1 起
            "\n"                   // 5
            "顶格段落\n"           // 6 空行后的顶格段落同样打断
            "\n"                   // 7
            "1. 又一个新列表\n";   // 8 必须重新从 1 起
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].listOrdinal == 1, "首个列表项呈现 1");
        check(plan.lines[4].listOrdinal == 1,
              "标题应打断列表：标题后的 `1.` 重新从 1 起，实际 " +
                  std::to_string(plan.lines[4].listOrdinal));
        check(plan.lines[8].listOrdinal == 1,
              "空行后的顶格段落也应打断列表：其后的 `1.` 重新从 1 起，实际 " +
                  std::to_string(plan.lines[8].listOrdinal));
        check(plan.lines[2].listItemLine < 0 && plan.lines[6].listItemLine < 0,
              "标题与顶格段落都不是列表续行（不归属任何列表项）");
    }
    {
        // 引用内的列表：判续行归属与判组存活都用**容器相对**缩进（"> " 前缀不算缩进）。
        const std::string lazyDoc =
            "> 1. 引用项\n"      // 0
            "> lazy 续行\n"      // 1 顶格（容器内缩进 0）→ 只能靠 lazy 归属
            "> 2. 第二项\n";     // 2 同组 → 2
        const neo::LpPlan lazyPlan = neo::buildLpPlan(lazyDoc);
        check(lazyPlan.lines[0].listOrdinal == 1 && lazyPlan.lines[2].listOrdinal == 2,
              "引用内同组列表继续计数（续行不打断组）");
        check(lazyPlan.lines[1].listItemLine == 0, "引用内的 lazy 续行应归属该列表项");

        const std::string indentedDoc =
            "> 1. 引用项\n"       // 0
            ">    缩进续段\n"     // 1 容器内缩进 3 = 正文列
            "> 2. 第二项\n";      // 2 同组 → 2
        const neo::LpPlan indentedPlan = neo::buildLpPlan(indentedDoc);
        check(indentedPlan.lines[1].listItemLine == 0,
              "引用内缩进到正文列的续行同样归属该项");
        check(indentedPlan.lines[2].listOrdinal == 2, "缩进续段不打断组计数");
    }
}

// ① 标题行的 `padding-top: var(--p-spacing)`（含"标题—空行—标题"整条不生效的例外）；
// ② 表格容器（.cm-embed-block）的 16px 上下内边距。其余位置由空源码行自己的 24px 承担。
// 实测见 参考/块模型契约-2026-09-26.md。
void testBlockSpaceBefore() {
    const std::string doc =
        "# 一级\n"          // 0  文档首块：顶端无外间距
        "\n"                // 1
        "正文 A\n"          // 2
        "\n"                // 3
        "## 二级\n"         // 4  前一条非空是正文 → 16
        "\n"                // 5
        "### 三级\n"        // 6  标题—空行—标题 → 0
        "\n"                // 7
        "\n"                // 8
        "## 四级\n"         // 9  隔了两条空行 → 16（例外只认"恰好一条"）
        "正文 B\n"          // 10 标题紧随正文 → 0
        "\n"                // 11
        "| a | b |\n"       // 12 表格首行 → 16
        "| --- | --- |\n"   // 13 表内 → 0
        "| 1 | 2 |\n"       // 14 表内 → 0
        "\n"                // 15 表后紧跟的一行承接下边距 → 16
        "正文 C\n"          // 16 → 0
        "- 列表项\n"        // 17 → 0
        "\n"                // 18
        "```\n"             // 19 代码围栏 → 0
        "code\n"            // 20 → 0
        "```\n";            // 21 → 0
    const neo::LpPlan p = neo::buildLpPlan(doc);
    const components::MarkdownStyle style =
        neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
    std::vector<IncDeco> table;
    neo::lp::buildDecorations(p, style, 0, table);
    // 末尾换行会多出一条空行（与 kDoc 的口径一致）→ 正文 22 行 + 尾空行 = 23。
    check(table.size() == p.lines.size() && p.lines.size() == 23,
          "R1 样例应有 23 行装饰，实际 " + std::to_string(table.size()));

    const auto expectGap = [&](std::size_t i, float want, const char* what) {
        check(std::fabs(table[i].spaceBefore - want) < 0.001f,
              std::string(what) + "：spaceBefore 应为 " + std::to_string(want) +
                  "，实际 " + std::to_string(table[i].spaceBefore));
    };
    expectGap(0, 0.0f, "文档首块顶端");
    expectGap(4, neo::lp::kBlockGap, "空行后的标题");
    expectGap(6, 0.0f, "标题—空行—标题");
    expectGap(9, neo::lp::kBlockGap, "两条空行隔开的标题");
    expectGap(10, 0.0f, "标题后紧随的正文");
    expectGap(12, neo::lp::kBlockGap, "表格首行（上内边距）");
    expectGap(13, 0.0f, "表内行不得各自顶开（语义层每行都是块首）");
    expectGap(14, 0.0f, "表内末行");
    expectGap(15, neo::lp::kBlockGap, "表后一行（下内边距）");
    expectGap(16, 0.0f, "表后空行的下一行");
    expectGap(17, 0.0f, "列表项");
    expectGap(19, 0.0f, "代码围栏");
    expectGap(21, 0.0f, "代码块闭围栏");
    expectGap(22, 0.0f, "尾空行");

    // 表格行全是块首（语义层的既有约定）——这正是"间距只能落在整表首行"的原因。
    check(p.lines[12].blockFirstLine && p.lines[13].blockFirstLine && p.lines[14].blockFirstLine,
          "表格每一行都应当是块首（间距因此只能靠 tableId 认整表首行）");
    check(p.lines[12].tableId == p.lines[14].tableId && p.lines[11].tableId != p.lines[12].tableId,
          "同一张表的 tableId 必须相同、与表外行不同");
}

void testThematicBreakDecoration() {
    const components::MarkdownStyle style =
        neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
    for (const std::string marker : {"---", "***", "___", "- - -"}) {
        const std::string doc = marker + "\n\nbody\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].kind == neo::LpKind::Divider, marker + " should parse as a divider");
        std::vector<components::input_detail::LineDecoration> table;
        neo::lp::buildDecorations(plan, style, static_cast<int>(doc.find("body")), table);
        check(table.size() == plan.lines.size(), marker + " decoration count");
        if (table.size() != plan.lines.size()) {
            continue;
        }
        check(holesText(doc, table, 0) == marker, marker + " should conceal its source");
        check(sameColor(table[0].box.gridColor, style.divider),
              marker + " should use the theme divider color");
        check(table[0].box.horizontalRuleThickness == 2,
              marker + " should draw a 2px rule");
        check(std::fabs(table[0].lineHeight - style.bodyLineHeight) < 0.001f,
              marker + " should keep its normal line height");

        neo::lp::buildDecorations(plan, style, 0, table);
        check(holesText(doc, table, 0).empty(), marker + " should reveal source while active");
        check(table[0].box.horizontalRuleThickness == 0,
              marker + " should hide the rule while active");
    }
    for (const std::string doc : {"Title\n---\n", "---\ntitle: demo\n---\n"}) {
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        std::vector<components::input_detail::LineDecoration> table;
        neo::lp::buildDecorations(plan, style, 0, table);
        for (const auto& decoration : table) {
            check(decoration.box.horizontalRuleThickness == 0,
                  "setext and frontmatter markers must not become rules");
        }
    }
    {
        const std::string doc = "Paragraph\n\n---\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].kind == neo::LpKind::Text,
              "a blank line must prevent setext promotion of the prior paragraph");
        check(plan.lines[2].kind == neo::LpKind::Divider,
              "a rule following a blank line must stay a thematic break");
    }
}

void testVerbatimBlankAndWrappedBlockBackgrounds() {
    using Model = components::input_detail::InputModel;
    const auto style = neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
    const std::string doc = "---\nkey: value\n\n---\n\n```cpp\nfirst();\n\n  \nlast();\n```\n\nplain\n";
    const auto plan = neo::buildLpPlan(doc);
    std::vector<components::input_detail::LineDecoration> decorations;
    neo::lp::buildDecorations(plan, style, static_cast<int>(doc.find("plain")), decorations,
                              {}, {}, nullptr, doc);
    check(plan.lines[2].kind == neo::LpKind::Frontmatter && decorations[2].box.background.a > 0,
          "frontmatter blank line retains its continuous container background");
    for (int i = 5; i <= 10; ++i) {
        check(plan.lines[i].kind == neo::LpKind::Code &&
                  plan.lines[i].codeBlockBeg == plan.lines[5].srcBeg &&
                  plan.lines[i].codeBlockEnd == plan.lines[10].srcEnd &&
                  decorations[i].box.background.a > 0 &&
                  decorations[i].box.backgroundBlockFirst == (i == 5) &&
                  decorations[i].box.backgroundBlockLast == (i == 10),
              "code container owns blank/whitespace/fence row " + std::to_string(i));
    }
    check(decorations[11].box.background.a == 0, "ordinary blank after closed code stays outside container");

    // The closing fence is visible while editing the block; at a narrow width
    // it can wrap. Only its final visual segment may end the background.
    const std::string wrapped = "```\ncode\n````````````````````````````````\n";
    const auto wrappedPlan = neo::buildLpPlan(wrapped);
    neo::lp::buildDecorations(wrappedPlan, style, 1, decorations, {}, {}, nullptr, wrapped);
    Model::InputState state;
    state.text = wrapped;
    const auto layout = Model::InputLayout::build(state, 80, 500, 80, 0, 0, 0,
        style.bodyLineHeight, style.fontFamily, style.bodySize, true, &decorations);
    int lastFlags = 0, closingSegments = 0;
    for (const auto& line : layout.lineList()) {
        if (line.lineNumber == 3) {
            ++closingSegments;
            if (line.box.backgroundBlockLast) ++lastFlags;
        }
    }
    check(closingSegments > 1 && lastFlags == 1, "wrapped closing fence has only one block end");
}

void testQuoteIndentAndTaskGlyphHitGeometry() {
    using InputModel = components::input_detail::InputModel;
    using LineDecoration = components::input_detail::LineDecoration;
    const components::MarkdownStyle style =
        neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");

    {
        const std::string doc =
            "> **一层引用**\n"
            "> > 两层引用\n"
            "> > > 三层引用\n"
            "plain\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        std::vector<LineDecoration> decorations;
        neo::lp::buildDecorations(plan, style, plan.lines[3].srcBeg, decorations,
                                  {}, {}, nullptr, doc);
        check(decorations.size() == plan.lines.size(), "quote indent probe should decorate every line");
        if (decorations.size() == plan.lines.size()) {
            const auto quoteIndent = [&](int depth) {
                return style.bodySize * 0.75f + (depth - 1) * style.bodySize * 1.2f;
            };
            check(std::fabs(decorations[0].contentIndent - quoteIndent(1)) < 0.001f &&
                      std::fabs(decorations[1].contentIndent - quoteIndent(2)) < 0.001f &&
                      std::fabs(decorations[2].contentIndent - quoteIndent(3)) < 0.001f,
                  "quote contentIndent should use a 0.75em first slot plus 1.2em per nested level");
            check(decorations[0].box.barCount == 1 && decorations[1].box.barCount == 2 &&
                      decorations[2].box.barCount == 3,
                  "quote bar count should match the corresponding content indent depth");
        }

        const int boldContent = static_cast<int>(doc.find("一层引用"));
        neo::lp::buildDecorations(plan, style, boldContent, decorations,
                                  {}, {}, nullptr, doc);
        check(holesContain(decorations, 0, plan.lines[0].srcBeg,
                           static_cast<int>(doc.find("**"))),
              "active quote must keep its structural prefix hidden");
        check(holesText(doc, decorations, 0).find("> ") != std::string::npos,
              "active quote prefix should remain concealed while editing inline text");
        check(holesText(doc, decorations, 0).find("**") == std::string::npos,
              "active quote should reveal inline bold markers when the caret is inside them");
        check(components::input_detail::projectText(
                  doc, plan.lines[0].srcBeg, plan.lines[0].srcEnd, decorations[0].holes) == "**一层引用**",
              "active quote projection should omit its structural prefix while revealing inline bold markers");
    }

    {
        const std::string doc = "> - [ ] 引用任务\nplain\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[0].kind == neo::LpKind::TaskItem && plan.lines[0].quoteDepth == 1,
              "quoted task should retain both task kind and quote depth");
        std::vector<LineDecoration> decorations;
        neo::lp::buildDecorations(plan, style, plan.lines[1].srcBeg, decorations,
                                  {}, {}, nullptr, doc);
        check(decorations[0].glyph.codepoint == neo::lp::kTaskIcon,
              "quoted task should keep its checkbox glyph");
        const float oneQuoteIndent = style.bodySize * 0.75f;
        check(std::fabs(decorations[0].contentIndent - oneQuoteIndent) < 0.001f,
              "quoted task content indent should use the first quote slot");
        check(components::input_detail::projectText(
                  doc, plan.lines[0].srcBeg, plan.lines[0].srcEnd, decorations[0].holes) == "引用任务",
              "quoted task projection should hide quote and task prefixes instead of repeating them as text");
    }

    {
        const std::string doc =
            "> - [ ] 上层任务\n"
            ">   同层任务续行\n"
            "> > 深一层引用文本\n"
            "plain\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        check(plan.lines[2].quoteDepth == 2,
              "owner-depth probe should parse the following paragraph at quote depth two");
        std::vector<LineDecoration> decorations;
        neo::lp::buildDecorations(plan, style, plan.lines[3].srcBeg, decorations,
                                  {}, {}, nullptr, doc);
        if (decorations.size() == plan.lines.size()) {
            const float oneQuoteIndent = style.bodySize * 0.75f;
            const float twoQuoteIndent = oneQuoteIndent + style.bodySize * 1.2f;
            const float taskContinuationIndent =
                std::max(4.0f, style.bodySize * components::input_detail::kGlyphAdvanceEm);
            check(std::fabs(decorations[1].contentIndent -
                            (oneQuoteIndent + taskContinuationIndent)) < 0.001f,
                  "same-depth task continuation should add the task body indent once");
            check(std::fabs(decorations[2].contentIndent - twoQuoteIndent) < 0.001f,
                  "deeper quote line should use its own quote indent without inheriting task continuation indent");
        } else {
            check(false, "quote-owner depth probe should decorate every source line");
        }
    }

    {
        const std::string doc =
            "- [ ] This task wraps across several visual rows so only its first visual row owns a checkbox glyph. "
            "The continuation rows align with the task body and must not activate the checkbox hit target.\n"
            "plain\n";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        std::vector<LineDecoration> decorations;
        neo::lp::buildDecorations(plan, style, plan.lines[1].srcBeg, decorations,
                                  {}, {}, nullptr, doc);
        InputModel::InputState state;
        state.text = doc;
        state.cursor = plan.lines[1].srcBeg;
        constexpr float viewportWidth = 190.0f;
        constexpr float inset = 8.0f;
        const auto layout = InputModel::InputLayout::build(
            state, viewportWidth, 500.0f, viewportWidth, inset, 0.0f, 0.0f,
            style.bodyLineHeight, style.fontFamily, style.bodySize, true, &decorations);
        std::vector<int> visualRows;
        for (int i = 0; i < static_cast<int>(layout.lineList().size()); ++i) {
            if (layout.lineList()[static_cast<std::size_t>(i)].lineNumber == 1) visualRows.push_back(i);
        }
        check(visualRows.size() > 1, "long task should wrap to multiple visual rows in the fixed viewport");
        if (visualRows.size() > 1) {
            const auto& first = layout.lineList()[static_cast<std::size_t>(visualRows.front())];
            const double glyphX = inset + first.contentIndent + first.glyph.advance * 0.5f;
            const auto rowY = [&](int visualIndex) {
                return layout.geometryTable().top(visualIndex) +
                       layout.geometryTable().height(visualIndex) * 0.5f;
            };
            const core::Rect bounds{0.0f, 0.0f, viewportWidth, 500.0f};
            const auto firstHit = layout.pointerHit(glyphX, rowY(visualRows.front()), bounds,
                                                    viewportWidth, inset);
            const auto continuationHit = layout.pointerHit(glyphX, rowY(visualRows[1]), bounds,
                                                           viewportWidth, inset);
            check(firstHit.onGlyph && firstHit.lineNumber == 1,
                  "checkbox glyph should be hittable on the first visual row of its task");
            check(!continuationHit.onGlyph && continuationHit.lineNumber == 1,
                  "soft-wrapped continuation rows must not participate in checkbox glyph hits");
        }
    }
}

void testCompletedTaskInlineRunBoundaries() {
    const std::string doc =
        "- [x] lead **bold** middle `code` [link](https://example.com) [[Wiki]] tail\n"
        "plain\n";
    const neo::LpPlan plan = neo::buildLpPlan(doc);
    const components::MarkdownStyle style =
        neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
    check(!plan.lines.empty() && plan.lines[0].kind == neo::LpKind::TaskItem &&
              plan.lines[0].taskChecked,
          "completed task inline-style probe should parse a checked task row");
    if (plan.lines.size() < 2) {
        return;
    }

    std::vector<components::input_detail::LineDecoration> decorations;
    neo::lp::buildDecorations(plan, style, plan.lines[1].srcBeg, decorations,
                              {}, {}, nullptr, doc);
    check(decorations.size() == plan.lines.size(),
          "completed task inline-style probe should decorate every source row");
    if (decorations.empty()) {
        return;
    }

    const auto runAt = [&](int offset) -> const components::input_detail::LineRun* {
        for (const auto& run : decorations[0].runs) {
            if (run.beg <= offset && offset < run.end) return &run;
        }
        return nullptr;
    };
    const int leadAt = static_cast<int>(doc.find("lead"));
    const int boldAt = static_cast<int>(doc.find("bold"));
    const int middleAt = static_cast<int>(doc.find("middle"));
    const int codeAt = static_cast<int>(doc.find("`code`")) + 1;
    const int linkTextAt = static_cast<int>(doc.find("[link]")) + 1;
    const int wikiAt = static_cast<int>(doc.find("[[Wiki]]")) + 2;
    const int tailAt = static_cast<int>(doc.find("tail"));
    const auto* lead = runAt(leadAt);
    const auto* bold = runAt(boldAt);
    const auto* middle = runAt(middleAt);
    const auto* code = runAt(codeAt);
    const auto* link = runAt(linkTextAt);
    const auto* wiki = runAt(wikiAt);
    const auto* tail = runAt(tailAt);
    check(lead != nullptr && bold != nullptr && middle != nullptr && code != nullptr &&
              link != nullptr && wiki != nullptr && tail != nullptr,
          "completed task should produce merged runs across plain, strong, code and link text");
    if (lead == nullptr || bold == nullptr || middle == nullptr || code == nullptr ||
        link == nullptr || wiki == nullptr || tail == nullptr) {
        return;
    }

    for (const auto [name, run] : std::vector<std::pair<const char*, const components::input_detail::LineRun*>>{
             {"lead", lead}, {"bold", bold}, {"middle", middle}, {"code", code},
             {"link", link}, {"wiki", wiki}, {"tail", tail}}) {
        check(run->style.strike,
              std::string("completed task strike-through should cover ") + name);
    }
    check(bold->style.weight == 700,
          "completed task merge should preserve the strong style on bold text");
    check(code->style.fontFamily == style.codeFontFamily &&
              sameColor(code->style.background, style.codeBackground),
          "completed task inline code should retain its local code font and background");
    check(lead->style.background.a == 0.0f && middle->style.background.a == 0.0f &&
              link->style.background.a == 0.0f && wiki->style.background.a == 0.0f &&
              tail->style.background.a == 0.0f,
          "completed task code background must not spread beyond the inline-code range");
    check(link->style.link && link->style.underline,
          "completed task Markdown links should retain link styling and underline");
    check(wiki->style.link && wiki->style.underline,
          "completed task WikiLinks should retain link styling and underline");
    check(!lead->style.underline && !bold->style.underline && !middle->style.underline &&
              !tail->style.underline,
          "link underline should not spread to neighboring completed-task text");
}

void testHeadingHierarchyAndInlineSpanBoundaries() {
    const auto checkHeadings = [](float bodySize) {
        const components::MarkdownStyle style =
            neo::markdownStyle(bodySize, "Microsoft YaHei", "monospace");
        const std::string doc = "# One\n## Two\n### Three\n#### Four\n##### Five\n###### Six";
        const neo::LpPlan plan = neo::buildLpPlan(doc);
        std::vector<IncDeco> decorations;
        neo::lp::buildDecorations(plan, style, static_cast<int>(doc.size()), decorations);
        check(decorations.size() == 6 && plan.lines.size() == 6,
              "H1-H6 geometry probe should produce six heading rows");
        if (decorations.size() != 6 || plan.lines.size() != 6) {
            return std::vector<float>{};
        }

        const std::vector<float> sizes = {
            decorations[0].fontSize, decorations[1].fontSize, decorations[2].fontSize,
            decorations[3].fontSize, decorations[4].fontSize, decorations[5].fontSize};
        check(sizes[0] > sizes[1] && sizes[1] > sizes[2] && sizes[2] > sizes[3] &&
                  sizes[3] > sizes[4] && sizes[4] >= sizes[5],
              "heading sizes should descend through H6 without H4-H6 collapsing to H3");
        check(std::fabs(sizes[3] - std::max(style.bodySize, style.h3Size * 0.89f)) < 0.001f &&
                  std::fabs(sizes[4] - std::max(style.bodySize, style.h3Size * 0.80f)) < 0.001f &&
                  std::fabs(sizes[5] - style.bodySize) < 0.001f,
              "H4-H6 should derive from the current body/H3 typography, including the body floor");
        check(std::fabs(decorations[3].lineHeight / sizes[3] - 1.35f) < 0.001f &&
                  std::fabs(decorations[4].lineHeight / sizes[4] - 1.4f) < 0.001f &&
                  std::fabs(decorations[5].lineHeight / sizes[5] - 1.4f) < 0.001f,
              "H4-H6 line boxes should track their own sizes at their chosen leading ratios");
        for (std::size_t i = 0; i < 5; ++i) {
            check(sameColor(decorations[i].textColor, style.heading),
                  "H1-H5 should retain the heading color");
        }
        check(sameColor(decorations[5].textColor, style.muted),
              "H6 should use the muted text color");
        return sizes;
    };
    const std::vector<float> smallSizes = checkHeadings(14.0f);
    const std::vector<float> largeSizes = checkHeadings(20.0f);
    check(smallSizes.size() == 6 && largeSizes.size() == 6,
          "heading hierarchy should be checked at both requested body sizes");
    if (smallSizes.size() == 6 && largeSizes.size() == 6) {
        for (std::size_t i = 0; i < smallSizes.size(); ++i) {
            check(std::fabs(largeSizes[i] / smallSizes[i] - (20.0f / 14.0f)) < 0.002f,
                  "heading hierarchy should scale with body size at every level");
        }
    }

    // The heading-wide bold run overlaps a nested underline and code span, followed by an
    // adjacent code span outside the underline. Verify the actual parser/decorator output,
    // including source projection, rather than manufacturing runs for mergeRuns directly.
    const std::string doc =
        "# Pre _underline `inner-code` tail_ `outer-code` end\n\nplain cursor target\n";
    const neo::LpPlan plan = neo::buildLpPlan(doc);
    const components::MarkdownStyle style =
        neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
    const int cursor = static_cast<int>(doc.find("plain cursor target"));
    std::vector<IncDeco> decorations;
    neo::lp::buildDecorations(plan, style, cursor, decorations);
    check(!plan.lines.empty() && plan.lines[0].kind == neo::LpKind::Heading,
          "nested-span probe should parse its first physical line as a heading");
    if (plan.lines.empty() || decorations.empty()) {
        return;
    }

    const neo::LpLine& heading = plan.lines[0];
    const IncDeco& decoration = decorations[0];
    const auto findSpan = [&](neo::LpSpanKind kind, const std::string& needle) -> const neo::LpSpan* {
        for (const neo::LpSpan& span : heading.spans) {
            if (span.kind == kind && slice(doc, span.content.beg, span.content.end) == needle) {
                return &span;
            }
        }
        return nullptr;
    };
    const neo::LpSpan* underline = nullptr;
    for (const neo::LpSpan& span : heading.spans) {
        if (span.kind == neo::LpSpanKind::Underline &&
            slice(doc, span.content.beg, span.content.end).find("inner-code") != std::string::npos) {
            underline = &span;
            break;
        }
    }
    const neo::LpSpan* innerCode = findSpan(neo::LpSpanKind::InlineCode, "inner-code");
    const neo::LpSpan* outerCode = findSpan(neo::LpSpanKind::InlineCode, "outer-code");
    check(underline != nullptr && innerCode != nullptr && outerCode != nullptr,
          "parser should expose the underline and both nested/adjacent inline-code spans");
    if (underline == nullptr || innerCode == nullptr || outerCode == nullptr) {
        return;
    }

    const auto runAt = [&](int offset) -> const components::input_detail::LineRun* {
        for (const auto& run : decoration.runs) {
            if (run.beg <= offset && offset < run.end) {
                return &run;
            }
        }
        return nullptr;
    };
    const int prefixAt = static_cast<int>(doc.find("Pre"));
    const int underlineAt = static_cast<int>(doc.find("underline"));
    const int innerAt = static_cast<int>(doc.find("inner-code"));
    const int tailAt = static_cast<int>(doc.find("tail"));
    const int outerAt = static_cast<int>(doc.find("outer-code"));
    const int suffixAt = static_cast<int>(doc.find("end\n"));
    const auto* prefixRun = runAt(prefixAt);
    const auto* underlineRun = runAt(underlineAt);
    const auto* innerRun = runAt(innerAt);
    const auto* tailRun = runAt(tailAt);
    const auto* outerRun = runAt(outerAt);
    const auto* suffixRun = runAt(suffixAt);
    check(prefixRun != nullptr && prefixRun->style.weight == 700 && !prefixRun->style.underline &&
              prefixRun->style.background.a == 0.0f,
          "heading bold should cover its prefix without leaking underline or code background");
    check(underlineRun != nullptr && underlineRun->style.weight == 700 &&
              underlineRun->style.underline && underlineRun->style.background.a == 0.0f,
          "underline should apply only to its own text while inheriting heading bold");
    check(innerRun != nullptr && innerRun->style.weight == 700 && innerRun->style.underline &&
              innerRun->style.fontFamily == style.codeFontFamily &&
              sameColor(innerRun->style.background, style.codeBackground),
          "nested inline code should inherit underline and heading bold while keeping local code styling");
    check(tailRun != nullptr && tailRun->style.underline && tailRun->style.background.a == 0.0f,
          "underline should continue after nested code without inheriting its background");
    check(outerRun != nullptr && outerRun->style.weight == 700 && !outerRun->style.underline &&
              outerRun->style.fontFamily == style.codeFontFamily &&
              sameColor(outerRun->style.background, style.codeBackground),
          "adjacent code should keep code styling without inheriting the preceding underline");
    check(suffixRun != nullptr && !suffixRun->style.underline && suffixRun->style.background.a == 0.0f,
          "styles should stop at the end of the adjacent code span");
    const std::string projected = components::input_detail::projectText(
        doc, heading.srcBeg, heading.srcEnd, decoration.holes);
    check(projected == "Pre underline inner-code tail outer-code end",
          "inactive heading projection should hide syntax markers and preserve nested visible text");

    const std::string wrappedDoc =
        "# _first alpha beta gamma delta epsilon zeta eta theta iota kappa `wrapped-code` "
        "lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega_\n\nbody\n";
    const neo::LpPlan wrappedPlan = neo::buildLpPlan(wrappedDoc);
    std::vector<IncDeco> wrappedDecorations;
    neo::lp::buildDecorations(wrappedPlan, style,
                              static_cast<int>(wrappedDoc.find("body")), wrappedDecorations);
    const neo::LpLine& wrappedHeading = wrappedPlan.lines[0];
    const neo::LpSpan* wrappedUnderline = nullptr;
    const neo::LpSpan* wrappedCode = nullptr;
    for (const neo::LpSpan& span : wrappedHeading.spans) {
        if (span.kind == neo::LpSpanKind::Underline) wrappedUnderline = &span;
        if (span.kind == neo::LpSpanKind::InlineCode &&
            slice(wrappedDoc, span.content.beg, span.content.end) == "wrapped-code") wrappedCode = &span;
    }
    check(wrappedUnderline != nullptr && wrappedCode != nullptr,
          "soft-wrap probe should contain an underline with nested code");
    if (wrappedUnderline == nullptr || wrappedCode == nullptr || wrappedDecorations.empty()) {
        return;
    }
    IncModel::InputState state;
    state.text = wrappedDoc;
    state.cursor = static_cast<int>(wrappedDoc.find("body"));
    const auto layout = IncModel::InputLayout::build(
        state, 118.0f, 500.0f, 138.0f, 8.0f, 8.0f, 0.0f, style.bodyLineHeight,
        style.fontFamily, style.bodySize, true, &wrappedDecorations);
    (void)layout;
    int underlineVisualLines = 0;
    int nestedCodeVisualLines = 0;
    for (const IncLine& visual : state.cachedLines) {
        bool lineHasUnderline = false;
        bool lineHasNestedCode = false;
        for (const auto& run : visual.runs) {
            const bool overlapsUnderline = run.beg < wrappedUnderline->content.end &&
                                           wrappedUnderline->content.beg < run.end;
            const bool overlapsCode = run.beg < wrappedCode->content.end &&
                                      wrappedCode->content.beg < run.end;
            if (overlapsUnderline) {
                lineHasUnderline = true;
                check(run.style.underline,
                      "every measured visible run within the underline must retain underline styling");
            }
            if (overlapsCode) {
                lineHasNestedCode = true;
                check(run.style.underline && run.style.fontFamily == style.codeFontFamily,
                      "wrapped nested-code runs must retain both inherited underline and code font");
            }
        }
        if (lineHasUnderline) ++underlineVisualLines;
        if (lineHasNestedCode) ++nestedCodeVisualLines;
    }
    check(underlineVisualLines >= 3,
          "a narrow viewport should split the underline across several measured visual lines");
    check(nestedCodeVisualLines >= 1,
          "the nested-code range should survive projection and be represented in wrapped layout");
}

void testCursorSemanticSnapshots() {
    const std::string doc =
        "---\ntitle: example\n---\n\n# Heading **bold**\n\n"
        "Plain 中文 é office **bold _nested_ tail** then `code` [link](target.md) [[a|alias]]\n"
        "continuation *across\nsource lines* suffix\n\n"
        "> quote **text**\n> next row\n\n- [x] done ~~marked~~\n\n"
        "```cpp\n/* multi\nline comment */ int a = 1;\n```\n\n"
        "| A **header** | B |\n| --- | ---: |\n| `cell` | 42 |\n\n"
        "![missing](absent.png)\n\n---\n\n## Nested\n\nfold body\nsecond hidden row\n\n"
        "Tail plain text\n\nSetext **title**\n===\n\n"
        "## Child\n\nchild first\nchild second\n\n"
        "```python\nx = \"\"\"multi\nstring\"\"\"\n```\n";
    const auto plan = neo::buildLpPlan(doc);
    for (const auto theme : {neo::ThemeMode::Dark, neo::ThemeMode::Light}) {
        const auto colors = neo::editorColors(theme);
        const auto style = neo::markdownStyle(16.0f, "monospace", "monospace", colors);
        std::set<int> folds;
        for (const auto& line : plan.lines) {
            if (line.kind == neo::LpKind::Heading) folds.insert(line.srcBeg);
        }
        for (bool folded : {false, true}) {
            neo::lp::invalidateDecorationCache();
            const auto* foldInput = folded ? &folds : nullptr;
            // Exhaust both directions, including adjacent marks and every inclusive
            // span boundary. The oracle always calls the original full builder.
            for (int direction : {1, -1}) {
                for (int step = -1; step <= static_cast<int>(doc.size()) + 1; ++step) {
                    const int cursor = direction == 1 ? step : static_cast<int>(doc.size()) - step;
                    const auto snapshot = neo::lp::cachedDecorationSnapshot(plan, 9001, cursor,
                        style, "monospace", theme, {}, foldInput, doc, &colors);
                    const auto frozen = snapshot->copyRows();
                    std::vector<IncDeco> oracle;
                    neo::lp::buildDecorations(plan, style, cursor, oracle, "monospace", {},
                                              foldInput, doc, &colors);
                    check(*snapshot == oracle, "cursor semantic cache must equal full decoration oracle");
                    const auto next = neo::lp::cachedDecorationSnapshot(plan, 9001, cursor + 1,
                        style, "monospace", theme, {}, foldInput, doc, &colors);
                    check(*snapshot == frozen, "cursor refresh must preserve retained generations");
                }
            }
            // Distant jumps exercise dependencies that byte-by-byte movement
            // keeps active, including multiline marks and code/fold state.
            unsigned random = 0x91d3u;
            for (int jump = 0; jump < 160; ++jump) {
                random = random * 1664525u + 1013904223u;
                const int cursor = static_cast<int>(random % (doc.size() + 5)) - 2;
                const auto held = neo::lp::decorationCache().snapshot;
                const auto heldValue = held->copyRows();
                const auto statsBefore = neo::lp::decorationDebugStats();
                const auto snapshot = neo::lp::cachedDecorationSnapshot(plan, 9001, cursor,
                    style, "monospace", theme, {}, foldInput, doc, &colors);
                std::vector<IncDeco> oracle;
                neo::lp::buildDecorations(plan, style, cursor, oracle, "monospace", {},
                                          foldInput, doc, &colors);
                check(*snapshot == oracle, "distant cursor jump must equal full oracle");
                check(*held == heldValue, "distant cursor jump must preserve held generation");
                check(neo::lp::decorationDebugStats().full == statsBefore.full,
                      "unchanged plan cursor jumps must avoid full decoration builds");
            }
            const int plain = static_cast<int>(doc.find("Tail plain"));
            const auto first = neo::lp::cachedDecorationSnapshot(plan, 9001, plain, style,
                "monospace", theme, {}, foldInput, doc, &colors);
            const auto stats = neo::lp::decorationDebugStats();
            const auto same = neo::lp::cachedDecorationSnapshot(plan, 9001, plain + 3, style,
                "monospace", theme, {}, foldInput, doc, &colors);
            check(first == same && neo::lp::decorationDebugStats().full == stats.full &&
                neo::lp::decorationDebugStats().cursorSemanticHit == stats.cursorSemanticHit + 1,
                "ordinary same-line movement must share the immutable snapshot without rebuilding");
            check(neo::lp::decorationCache().cursor == plain + 3,
                  "semantic hit must retain latest cursor for later incremental edits");
            IncModel::InputState state;
            state.text = doc;
            IncModel::ensureLayoutCache(state, "monospace", 16, 800, true, first.get(), first);
            const auto layoutStats = IncModel::debugLayoutStats();
            const auto revision = state.decorationRevision;
            IncModel::ensureLayoutCache(state, "monospace", 16, 800, true, same.get(), same);
            check(state.decorationRevision == revision &&
                IncModel::debugLayoutStats().full == layoutStats.full &&
                IncModel::debugLayoutStats().incremental == layoutStats.incremental,
                "semantic hit must avoid decoration comparison and relayout downstream");
            const auto changedThemeRevision = neo::lp::cachedDecorationSnapshot(plan, 9001,
                plain + 4, style, "monospace", theme, {}, foldInput, doc, &colors, nullptr,
                neo::themeRevision() + 1);
            check(changedThemeRevision != same, "semantic equivalence must not bypass theme revision");
        }
    }
    neo::lp::invalidateDecorationCache();
    check(neo::lp::decorationCache().cursorBoundaries.capacity() == 0 &&
          neo::lp::decorationCache().cursorSpans.capacity() == 0 &&
          neo::lp::decorationCache().cursorFolds.capacity() == 0,
          "document/cache invalidation must release all cursor dependency storage");
}

void testCursorPartialBounds() {
    std::string doc = "Before **marked** tail\n\n";
    for (int i = 0; i < 1000; ++i) doc += "plain paragraph " + std::to_string(i) + "\n\n";
    const auto plan = neo::buildLpPlan(doc);
    const auto colors = neo::editorColors(neo::ThemeMode::Dark);
    const auto style = neo::markdownStyle(16, "monospace", "monospace", colors);
    neo::lp::invalidateDecorationCache();
    neo::lp::DecoratorEditInfo info;
    info.committed = true;
    info.textRevision = 42;
    const int plain = plan.lines[800].srcBeg;
    const auto first = neo::lp::cachedDecorationSnapshot(plan, 9100, plain, style,
        "monospace", neo::ThemeMode::Dark, {}, nullptr, doc, &colors, &info);
    const auto before = neo::lp::decorationDebugStats();
    const auto same = neo::lp::cachedDecorationSnapshot(plan, 9100, plan.lines[1600].srcBeg,
        style, "monospace", neo::ThemeMode::Dark, {}, nullptr, doc, &colors);
    const auto after = neo::lp::decorationDebugStats();
    check(same == first && after.cursorPartial == before.cursorPartial + 1 &&
          after.full == before.full && after.cursorRebuiltRows == before.cursorRebuiltRows + 2 &&
          after.cursorCopiedRows == before.cursorCopiedRows,
          "distant plain paragraphs must rebuild only two rows and share unchanged snapshot");
    check(neo::lp::decorationCache().builtCommitted && neo::lp::decorationCache().textRevision == 42,
          "cursor publication without edit info must preserve committed provenance");
    const auto mark = neo::lp::cachedDecorationSnapshot(plan, 9100, 10, style,
        "monospace", neo::ThemeMode::Dark, {}, nullptr, doc, &colors);
    const auto frozen = mark->copyRows();
    const auto markStats = neo::lp::decorationDebugStats();
    const auto exit = neo::lp::cachedDecorationSnapshot(plan, 9100, plan.lines[0].srcEnd,
        style, "monospace", neo::ThemeMode::Dark, {}, nullptr, doc, &colors);
    const auto exitStats = neo::lp::decorationDebugStats();
    std::vector<IncDeco> oracle;
    neo::lp::buildDecorations(plan, style, plan.lines[0].srcEnd, oracle, "monospace", {},
                              nullptr, doc, &colors);
    check(exit != mark && *exit == oracle && *mark == frozen,
          "marker exit must publish an exact independent generation");
    check(exitStats.cursorRebuiltRows == markStats.cursorRebuiltRows + 1 &&
          exitStats.cursorCopiedRows == markStats.cursorCopiedRows + components::input_detail::LineDecorationTable::pageSize - 1,
          "same-row marker exit must move its row and copy only its page siblings");
    // Caret movement within an unchanged code block changes neither its active
    // state nor syntax. A long block must not be retokenized without a dependency.
    std::string code = "```cpp\n";
    for (int i = 0; i < 100; ++i) code += "int value = 42;\n";
    code += "```\n";
    const auto codePlan = neo::buildLpPlan(code);
    neo::lp::invalidateDecorationCache();
    const auto codeFirst = neo::lp::cachedDecorationSnapshot(codePlan, 9200,
        codePlan.lines[1].srcBeg, style, "monospace", neo::ThemeMode::Dark, {}, nullptr, code, &colors);
    const auto codeStats = neo::lp::decorationDebugStats();
    const auto codeNext = neo::lp::cachedDecorationSnapshot(codePlan, 9200,
        codePlan.lines[99].srcBeg, style, "monospace", neo::ThemeMode::Dark, {}, nullptr, code, &colors);
    check(codeFirst == codeNext &&
          neo::lp::decorationDebugStats().cursorRebuiltRows == codeStats.cursorRebuiltRows &&
          neo::lp::decorationDebugStats().cursorCopiedRows == codeStats.cursorCopiedRows,
          "unchanged code block cursor movement must skip tokenizing and table copying");

    // Entering/leaving a complete code block prepares every code row privately.
    // Keep several rich rows outside that block so publication must copy gaps
    // on both sides while skipping all candidate payloads.
    const std::string mixedCode = "Intro **marked** tail\n\n" + code +
        "\nMiddle `inline` text\n\nTail **end** suffix\n";
    const auto mixedPlan = neo::buildLpPlan(mixedCode);
    for (const auto theme : {neo::ThemeMode::Dark, neo::ThemeMode::Light}) {
        const auto mixedColors = neo::editorColors(theme);
        const auto mixedStyle = neo::markdownStyle(16, "monospace", "monospace", mixedColors);
        neo::lp::invalidateDecorationCache();
        auto held = neo::lp::cachedDecorationSnapshot(mixedPlan, 9300, 0, mixedStyle,
            "monospace", theme, {}, nullptr, mixedCode, &mixedColors);
        const int codeCursor = static_cast<int>(mixedCode.find("int value"));
        const int tailCursor = static_cast<int>(mixedCode.find("Tail **end"));
        for (const int cursor : {codeCursor, tailCursor, codeCursor, 0}) {
            const auto frozenHeld = held->copyRows();
            const auto statsBefore = neo::lp::decorationDebugStats();
            const auto published = neo::lp::cachedDecorationSnapshot(mixedPlan, 9300, cursor,
                mixedStyle, "monospace", theme, {}, nullptr, mixedCode, &mixedColors);
            const auto statsAfter = neo::lp::decorationDebugStats();
            std::vector<IncDeco> full;
            neo::lp::buildDecorations(mixedPlan, mixedStyle, cursor, full, "monospace", {},
                nullptr, mixedCode, &mixedColors);
            const auto rebuilt = statsAfter.cursorRebuiltRows - statsBefore.cursorRebuiltRows;
            const auto copied = statsAfter.cursorCopiedRows - statsBefore.cursorCopiedRows;
            check(published != held && *published == full,
                  "long code-block transition must publish the exact full decoration oracle");
            check(*held == frozenHeld,
                  "gap-copy publication must preserve the retained rich generation");
            check(statsAfter.full == statsBefore.full && rebuilt >= 102 &&
                  rebuilt < mixedPlan.lines.size() && copied <= mixedPlan.lines.size() - 1,
                  "code-block publication must retain block proof and bound page sibling copies");
            held = published;
        }
    }

    // No unaffected row is required: a single marked row without a trailing
    // newline exercises empty prefix/tail gaps and zero deep copies.
    const std::string markedOnly = "Before **marked** tail";
    const auto markedOnlyPlan = neo::buildLpPlan(markedOnly);
    neo::lp::invalidateDecorationCache();
    const auto marked = neo::lp::cachedDecorationSnapshot(markedOnlyPlan, 9400, 10,
        style, "monospace", neo::ThemeMode::Dark, {}, nullptr, markedOnly, &colors);
    const auto markedFrozen = marked->copyRows();
    const auto allBefore = neo::lp::decorationDebugStats();
    const int afterMark = static_cast<int>(markedOnly.size());
    const auto unmarked = neo::lp::cachedDecorationSnapshot(markedOnlyPlan, 9400,
        afterMark, style, "monospace", neo::ThemeMode::Dark, {},
        nullptr, markedOnly, &colors);
    const auto allAfter = neo::lp::decorationDebugStats();
    std::vector<IncDeco> allOracle;
    neo::lp::buildDecorations(markedOnlyPlan, style, afterMark,
        allOracle, "monospace", {}, nullptr, markedOnly, &colors);
    check(unmarked != marked && *unmarked == allOracle && *marked == markedFrozen,
          "all-candidate publication must preserve old generation and full oracle");
    check(allAfter.cursorRebuiltRows - allBefore.cursorRebuiltRows == markedOnlyPlan.lines.size() &&
          allAfter.cursorCopiedRows == allBefore.cursorCopiedRows,
          "all-candidate publication must move all rows without copying old payloads");
    neo::lp::invalidateDecorationCache();
}

void testPagedSnapshotStorage() {
    using Table = components::input_detail::LineDecorationTable;
    using View = components::input_detail::LineDecorationView;
    std::vector<IncDeco> source(1025);
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i].holes = {{static_cast<int>(i * 10), static_cast<int>(i * 10 + 2)}};
        source[i].imageFailText = "中文/emoji 😀 page " + std::to_string(i);
    }
    auto first = std::make_shared<const Table>(source);
    const std::vector<int> rows{0, 63, 64, 128, 1024};
    std::vector<IncDeco> patches;
    auto expected = source;
    for (int row : rows) {
        expected[row].imageFailText += " changed";
        expected[row].holes.clear();
        patches.push_back(expected[row]);
    }
    unsigned long long copied = 0;
    auto next = first->replacing(rows, patches, &copied);
    check(next && *next == expected && *first == source,
          "page-boundary patches must match independent full rows and preserve old payloads");
    check(copied == 64 * 3 + 1 - rows.size(),
          "only sibling rows in three full pages and the last short page may be copied");
    for (std::size_t row = 0; row < source.size(); ++row) {
        const bool touched = row / 64 <= 2 || row == 1024;
        check((&(*first)[row] == &(*next)[row]) == !touched,
              "each unaffected page must share row identity; touched pages must own all their rows");
    }
    View view(next.get());
    check(view.copyRows() == expected && std::vector<IncDeco>(view.begin(), view.end()) == expected,
          "paged read and iterator adapters must preserve every row");
    check(*(view.begin() + 64) == expected[64] && view.end() - view.begin() == 1025 &&
          view == View(expected) && view.identity() == next.get(),
          "random access/equality must work across page boundaries without materializing");
    auto bad = [&](const std::vector<int>& invalid) {
        return first->replacing(invalid, std::vector<IncDeco>(invalid.size()));
    };
    check(!bad({64,63}) && !bad({63,63}) && !bad({-1}) && !bad({1025}) &&
          !first->replacing(rows, {}), "invalid replacements must be rejected before publication");
    std::weak_ptr<const Table> old = first;
    first.reset();
    check(old.expired() && *next == expected,
          "a child generation must retain shared pages without retaining its parent table");
    std::vector<std::weak_ptr<const Table>> oldGenerations;
    for (int i = 0; i < 200; ++i) {
        oldGenerations.push_back(next);
        auto candidate = (*next)[63];
        candidate.imageFailText = std::to_string(i);
        expected[63] = candidate;
        next = next->replacing({63}, {std::move(candidate)});
    }
    check(std::all_of(oldGenerations.begin(), oldGenerations.end(),
                     [](const auto& weak) { return weak.expired(); }) && *next == expected,
          "repeated publications must release every unreferenced directory while retaining exact shared rows");
}

void testImmutableSnapshots() {
    const std::string doc = "# title\n\nbody **bold**\n\n| A | B |\n| --- | --- |\n| cell | data |\n\ntail\n";
    const auto colors = neo::editorColors(neo::ThemeMode::Dark);
    const auto style = neo::markdownStyle(16.0f, "monospace", "monospace", colors);
    neo::lp::invalidatePlanCache();
    const auto& plan = neo::lp::cachedPlan(doc);
    const auto version = neo::lp::planCache().version;
    auto first = neo::lp::cachedDecorationSnapshot(plan, version, 0, style, "monospace",
        neo::ThemeMode::Dark, {}, nullptr, doc, &colors);
    const auto frozen = first->copyRows();
    auto same = neo::lp::cachedDecorationSnapshot(plan, version, 0, style, "monospace",
        neo::ThemeMode::Dark, {}, nullptr, doc, &colors);
    check(first == same, "cache hit must preserve snapshot identity");
    IncModel::InputState state;
    state.text = doc;
    IncModel::ensureLayoutCache(state, "monospace", 16.0f, 160.0f, true, first.get(), first);
    check(state.decorations.empty() && IncModel::activeDecorations(state).identity() == first.get(),
          "production layout must share the table without a vector copy");
    const auto stats = IncModel::debugLayoutStats();
    const auto revision = state.decorationRevision;
    IncModel::ensureLayoutCache(state, "monospace", 16.0f, 160.0f, true, same.get(), same);
    check(state.decorationRevision == revision && IncModel::debugLayoutStats().full == stats.full &&
        IncModel::debugLayoutStats().incremental == stats.incremental, "same snapshot must leave layout untouched");
    auto equivalent = std::make_shared<const components::input_detail::LineDecorationTable>(first->copyRows());
    IncModel::ensureLayoutCache(state, "monospace", 16.0f, 160.0f, true, equivalent.get(), equivalent);
    check(state.decorationSnapshot == equivalent && state.decorationRevision == revision &&
        IncModel::debugLayoutStats().incremental == stats.incremental,
        "equivalent publication must not relayout or invalidate content on a cursor move");

    std::weak_ptr<const components::input_detail::LineDecorationTable> old = first;
    auto second = neo::lp::cachedDecorationSnapshot(plan, version, static_cast<int>(doc.find("body")),
        style, "monospace", neo::ThemeMode::Dark, {}, nullptr, doc, &colors);
    check(first != second && *first == frozen, "activity changes must not mutate an existing generation");
    IncModel::ensureLayoutCache(state, "monospace", 16.0f, 160.0f, true, second.get(), second);
    check(state.decorationRevision > revision, "a new generation must invalidate rendering");
    check(IncModel::debugLayoutStats().tableReused > stats.tableReused &&
        IncModel::debugLayoutStats().tableRebuilt == stats.tableRebuilt,
        "changing activity outside a table must reuse its columns");
    state.compositionText = "中文";
    auto& display = IncModel::displayState(state, true);
    check(display.decorationSnapshot == second, "IME must share the immutable decoration generation");
    const auto& displayPlan = neo::lp::cachedPlan(display.text);
    components::input_detail::DecoratorEditInfo uncommitted{state.textRevision, false, nullptr};
    auto composing = neo::lp::cachedDecorationSnapshot(displayPlan, neo::lp::planCache().version, display.cursor,
        style, "monospace", neo::ThemeMode::Dark, {}, nullptr, display.text, &colors, &uncommitted);
    IncModel::ensureLayoutCache(display, "monospace", 16.0f, 160.0f, true, composing.get(), composing);
    check(display.cachedTables.size() == 1, "IME display must retain the table geometry");
    IncModel::displayState(state, false);
    check(state.decorationSnapshot == composing && state.cachedTables.size() == 1,
          "composition cancellation must transfer the complete layout cache");
    IncModel::ensureLayoutCache(state, "monospace", 16.0f, 160.0f, true, second.get(), second);
    IncModel::loadDocument(state, "plain text");
    check(!state.decorationSnapshot && state.decorations.empty(), "document load must release decorations");
    first.reset();
    check(!old.expired(), "a consumer must keep its old generation alive");
    same.reset();
    check(old.expired(), "an unreferenced old generation must be released");
    second.reset(); composing.reset();
    neo::lp::invalidatePlanCache();
    check(!neo::lp::decorationCache().snapshot, "switching away from Markdown must release the cache snapshot");
}

void testStableOffsetEditSnapshots() {
    struct Case { std::string text, before, after; };
    const std::vector<Case> cases = {
        {"# Head\n\n正文 sample 中文 😀\n\n**tail**\n", "中文", "汉字"},
        {"paragraph abc\n\n**tail**\n", "abc", "xyz"},
        {"paragraph abc\n\n**tail**\n", "abc", "a*c"},
        {"paragraph abc\n\n**tail**\n", "abc", "a\nc"},
        {"# Head\n\n- [x] task\n\nend\n", "[x]", "[ ]"},
        {"intro\n\n```cpp\nint abc = 42;\n```\n\nend\n", "int", "abc"},
        {"intro\n\n| A | B |\n| --- | --- |\n| abc | 42 |\n\nend\n", "abc", "xyz"},
        {"intro\n\n![img](missing.png)\n\nend\n", "missing", "changed"},
        {"# Head\n\nfirst abc\n\n## Child\n\nnext\n", "abc", "xyz"},
        {"# Head\n\nabc\n\nend\n", "abc", "# c"},
        {"intro\n\n**abc\ncontinued**\n\nend\n", "abc", "xyz"},
        {"intro\n\n[abc](url)\n\nend\n", "url", "new"},
    };
    for (auto theme : {neo::ThemeMode::Dark, neo::ThemeMode::Light})
    for (float width : {118.0f, 800.0f})
    for (const auto& sample : cases) {
        neo::lp::invalidatePlanCache();
        IncModel::InputState state;
        IncModel::loadDocument(state, sample.text);
        const auto colors = neo::editorColors(theme);
        const auto style = neo::markdownStyle(16, "monospace", "monospace", colors);
        const int pos = static_cast<int>(state.text.find(sample.before));
        IncModel::moveCursorTo(state, pos + static_cast<int>(sample.before.size()), false);
        const auto chain = [&]() {
            components::input_detail::DecoratorEditInfo info{state.textRevision, true, &state.pendingEdit};
            const auto& plan = neo::lp::cachedPlan(state.text, &info);
            auto snapshot = neo::lp::cachedDecorationSnapshot(plan, neo::lp::planCache().version,
                state.cursor, style, "monospace", theme, {}, nullptr, state.text, &colors, &info);
            IncModel::ensureLayoutCache(state, "monospace", 16, width, true, snapshot.get(), snapshot);
            return snapshot;
        };
        auto held = chain();
        for (int step = 0; step < 4; ++step) {
            const auto frozen = held->copyRows();
            const auto stats = neo::lp::decorationDebugStats();
            if (step == 1) IncModel::undoEdit(state);
            else if (step == 2) IncModel::redoEdit(state);
            else {
                IncModel::moveCursorTo(state, pos, false);
                IncModel::moveCursorTo(state, pos + static_cast<int>(sample.before.size()), true);
                IncModel::insertAtCursor(state, step == 0 ? sample.after : "more text");
            }
            const auto next = chain();
            const auto fullPlan = neo::buildLpPlan(state.text);
            std::vector<IncDeco> oracle;
            neo::lp::buildDecorations(fullPlan, style, state.cursor, oracle, "monospace",
                {}, nullptr, state.text, &colors);
            check(*next == oracle, "stable-offset edit/undo/redo must match full decorations");
            check(*held == frozen, "stable-offset edit must preserve retained generations");
            if (step < 3 && neo::lp::decorationDebugStats().incremental > stats.incremental) {
                check((next == held) == (oracle == frozen),
                    "proven stable-offset edits share iff all decorations remain equal");
            }
            if (step == 3) check((next == held) == (oracle == frozen),
                "byte-length changes share a generation only when its decorations remain equal");
            IncModel::InputState reference;
            IncModel::loadDocument(reference, state.text);
            reference.cursor = state.cursor;
            IncModel::ensureLayoutCache(reference, "monospace", 16, width, true, &oracle);
            bool equal = state.cachedLines.size() == reference.cachedLines.size();
            std::string mismatch = equal ? "" : "count";
            for (std::size_t row = 0; equal && row < state.cachedLines.size(); ++row)
            {
                mismatch = lineFieldDiff(state.cachedLines[row], reference.cachedLines[row]);
                if (mismatch.empty() && state.cachedLines[row].top != reference.cachedLines[row].top) mismatch = "top";
                equal = mismatch.empty();
            }
            check(equal && state.cachedGeometry.total() == reference.cachedGeometry.total() &&
                state.cachedTextWidth == reference.cachedTextWidth,
                "snapshot sharing full layout step=" + std::to_string(step) + " sample=" + sample.before +
                " diff=" + mismatch + " widths=" + std::to_string(state.cachedTextWidth) + "/" +
                std::to_string(reference.cachedTextWidth));
            held = next;
        }
    }
    check(neo::lp::decorationDebugStats().editUnchanged > 0, "stable-offset cases must exercise sharing");
    neo::lp::invalidatePlanCache();
}

void testEditedPagePublication() {
    // Sparse changes on both sides of a page boundary exercise actual differences,
    // page sibling ownership, old generation lifetime and full-layout equivalence.
    for (auto theme : {neo::ThemeMode::Dark, neo::ThemeMode::Light}) {
        neo::lp::invalidatePlanCache();
        std::string text;
        for (int row = 0; row < 260; ++row)
            text += row == 63 || row == 65 || row == 194 ? "- [x] task 中文 😀\n" : "plain 中文 😀\n";
        IncModel::InputState state;
        IncModel::loadDocument(state, text);
        const auto colors = neo::editorColors(theme);
        const auto style = neo::markdownStyle(16, "monospace", "monospace", colors);
        const auto chain = [&]() {
            components::input_detail::DecoratorEditInfo info{state.textRevision, true, &state.pendingEdit};
            const auto& plan = neo::lp::cachedPlan(state.text, &info);
            auto result = neo::lp::cachedDecorationSnapshot(plan, neo::lp::planCache().version,
                state.cursor, style, "monospace", theme, {}, nullptr, state.text, &colors, &info);
            IncModel::ensureLayoutCache(state, "monospace", 16, 118, true, result.get(), result);
            return result;
        };
        auto held = chain();
        for (int row : {63, 65, 194, 63, 65, 194}) {
            const auto frozen = held->copyRows();
            const auto before = neo::lp::decorationDebugStats();
            const auto& plan = neo::lp::planCache().plan;
            const int pos = plan.lines[row].srcBeg + 3;
            const char old = state.text[pos];
            IncModel::moveCursorTo(state, pos, false);
            IncModel::moveCursorTo(state, pos + 1, true);
            IncModel::insertAtCursor(state, old == 'x' ? " " : "x");
            auto next = chain();
            std::vector<IncDeco> oracle;
            const auto fullPlan = neo::buildLpPlan(state.text);
            neo::lp::buildDecorations(fullPlan, style, state.cursor, oracle, "monospace", {}, nullptr,
                state.text, &colors);
            check(*next == oracle && *held == frozen, "edited pages must equal full oracle and freeze old rows");
            check(neo::lp::decorationDebugStats().editPaged == before.editPaged + 1,
                "task style changes must take proven stable-offset page publication");
            std::set<std::size_t> touched;
            std::size_t changed = 0;
            for (std::size_t i = 0; i < oracle.size(); ++i) if (!(oracle[i] == frozen[i])) {
                touched.insert(i / components::input_detail::LineDecorationTable::pageSize);
                ++changed;
            }
            std::size_t pageRows = 0;
            for (auto page : touched) pageRows += std::min<std::size_t>(64, oracle.size() - page * 64);
            check(neo::lp::decorationDebugStats().editChangedRows == before.editChangedRows + changed &&
                neo::lp::decorationDebugStats().editCopiedRows == before.editCopiedRows + pageRows - changed,
                "edited page counters must count actual differences and only touched-page siblings");
            for (std::size_t i = 0; i < oracle.size(); ++i)
                check((&(*next)[i] == &(*held)[i]) == (touched.count(i / 64) == 0),
                    "unedited pages retain row identity, touched pages own immutable rows");
            IncModel::InputState reference;
            IncModel::loadDocument(reference, state.text);
            reference.cursor = state.cursor;
            IncModel::ensureLayoutCache(reference, "monospace", 16, 118, true, &oracle);
            bool same = state.cachedLines.size() == reference.cachedLines.size();
            for (std::size_t i = 0; same && i < state.cachedLines.size(); ++i)
                same = lineFieldDiff(state.cachedLines[i], reference.cachedLines[i]).empty() &&
                    state.cachedLines[i].top == reference.cachedLines[i].top;
            check(same && state.cachedGeometry.total() == reference.cachedGeometry.total() &&
                state.cachedTextWidth == reference.cachedTextWidth, "paged edit layout matches cold precise geometry");
            std::weak_ptr<const components::input_detail::LineDecorationTable> weak = held;
            held.reset();
            check(weak.expired(), "edit page publication must not retain the parent table");
            held = next;
        }
    }
    neo::lp::invalidatePlanCache();
}

void testShiftedEditPages() {
    for (int rich : {0, 1, 2})
    for (auto theme : {neo::ThemeMode::Dark, neo::ThemeMode::Light})
    for (float width : {118.0f, 800.0f}) {
        neo::lp::invalidatePlanCache();
        std::string text;
        for (int row = 0; row < 260; ++row) {
            if (row == 65) text += "EDIT_TARGET 中文 😀\n\n";
            else if (rich && row % 67 == 0) text += "**bold** and `code`\n\n";
            else text += "plain 中文 😀\n\n";
        }
        if (rich == 2) text += "- [x] task\n\n```cpp\nint x = 1;\n```\n\n"
            "| A | B |\n| --- | --- |\n| x | 42 |\n\n![img](missing.png)\n";
        IncModel::InputState state;
        IncModel::loadDocument(state, text);
        const int pos = static_cast<int>(text.find("EDIT_TARGET"));
        IncModel::moveCursorTo(state, pos, false);
        const auto colors = neo::editorColors(theme);
        const auto style = neo::markdownStyle(16, "monospace", "monospace", colors);
        const auto chain = [&]() {
            components::input_detail::DecoratorEditInfo info{state.textRevision, true, &state.pendingEdit};
            const auto& plan = neo::lp::cachedPlan(state.text, &info);
            auto next = neo::lp::cachedDecorationSnapshot(plan, neo::lp::planCache().version,
                state.cursor, style, "monospace", theme, {}, nullptr, state.text, &colors, &info);
            IncModel::ensureLayoutCache(state, "monospace", 16, width, true, next.get(), next);
            return next;
        };
        auto held = chain();
        for (int step = 0; step < 9; ++step) {
            const auto frozen = held->copyRows();
            const auto before = neo::lp::decorationDebugStats();
            if (step == 1 || step == 4 || step == 7) IncModel::undoEdit(state);
            else if (step == 2 || step == 5 || step == 8) IncModel::redoEdit(state);
            else {
                IncModel::moveCursorTo(state, pos, false);
                if (step == 3) {
                    IncModel::moveCursorTo(state, pos + 7, true);
                    IncModel::eraseSelection(state);
                } else IncModel::insertAtCursor(state, step == 0 ? "增😀" : "new\n");
            }
            auto next = chain();
            std::vector<IncDeco> oracle;
            const auto plan = neo::buildLpPlan(state.text);
            neo::lp::buildDecorations(plan, style, state.cursor, oracle, "monospace",
                {}, nullptr, state.text, &colors);
            check(*next == oracle && *held == frozen,
                "byte insertion/deletion and line fallback match full immutable oracle");
            if (step < 6) {
                check(neo::lp::decorationDebugStats().incremental == before.incremental + 1,
                    "same-row byte edits must preserve the trusted incremental proof");
                std::set<std::size_t> pages;
                std::size_t changed = 0;
                for (std::size_t row = 0; row < oracle.size(); ++row) if (oracle[row] != frozen[row]) {
                    pages.insert(row / 64); ++changed;
                }
                check((next == held) == (changed == 0),
                    "offset-free insertion/deletion may share the complete generation");
                std::size_t pageRows = 0;
                for (auto page : pages) pageRows += std::min<std::size_t>(64, oracle.size() - page * 64);
                check(neo::lp::decorationDebugStats().editCopiedRows == before.editCopiedRows + pageRows - changed,
                    "offset publication copies only siblings of actual translated/rebuilt rows");
                for (std::size_t row = 0; row < oracle.size(); ++row)
                    check((&(*next)[row] == &(*held)[row]) == (pages.count(row / 64) == 0),
                        "byte shifts preserve identity of offset-free untouched pages");
                if (rich) check(neo::lp::decorationDebugStats().editShiftPaged == before.editShiftPaged + 1,
                    "absolute suffix references must use translated page publication");
            }
            IncModel::InputState reference;
            IncModel::loadDocument(reference, state.text); reference.cursor = state.cursor;
            IncModel::ensureLayoutCache(reference, "monospace", 16, width, true, &oracle);
            bool equal = state.cachedLines.size() == reference.cachedLines.size();
            for (std::size_t row = 0; equal && row < state.cachedLines.size(); ++row)
                equal = lineFieldDiff(state.cachedLines[row], reference.cachedLines[row]).empty() &&
                    state.cachedLines[row].top == reference.cachedLines[row].top;
            check(equal && state.cachedGeometry.total() == reference.cachedGeometry.total() &&
                state.cachedTextWidth == reference.cachedTextWidth,
                "offset publication and row-count fallback retain exact cold geometry");
            std::weak_ptr<const components::input_detail::LineDecorationTable> weak = held;
            if (next != held) { held.reset(); check(weak.expired(), "translated pages must not own parent tables"); }
            held = next;
        }
    }
    neo::lp::invalidatePlanCache();
}

void testStableEditLayout() {
    const std::vector<std::string> docs = {
        "tiny_ 中文 😀\n\nTAIL_STATIC\n",
        "# Heading_\n\nPlain_ 中文 é **strong** `code` suffix\ncontinuation *across\n"
        "source lines* tail\n\n- [x] task_ **done**\n\n> quote_ text\n> next\n\n"
        "```cpp\n/* comment_\nend */ int value = 42;\n```\n\n"
        "## Child_\n\nfold first_\nfold second_\n\nSetext_ **title**\n===\n\nTAIL_STATIC\n",
        "Intro_\n\n| A_ **bold** | B |\n| --- | --- |\n| cell_ | 42 |\n\nTAIL_STATIC\n"
    };
    const auto initialStats = IncModel::debugLayoutStats();
    for (int kind = 0; kind < static_cast<int>(docs.size()); ++kind)
    for (auto theme : {neo::ThemeMode::Dark, neo::ThemeMode::Light})
    for (float width : {118.0f, 800.0f}) {
        neo::lp::invalidatePlanCache();
        IncModel::InputState state;
        IncModel::loadDocument(state, docs[kind]);
        const auto colors = neo::editorColors(theme);
        const auto style = neo::markdownStyle(16, "monospace", "monospace", colors);
        unsigned random = 981u;
        for (int step = 0; step < 96; ++step) {
            const auto held = state.decorationSnapshot;
            const auto frozen = held ? held->copyRows() : std::vector<IncDeco>{};
            const auto storage = state.cachedLines.data();
            const auto stats = IncModel::debugLayoutStats();
            if (step > 0) {
                random = random * 1664525u + 1013904223u;
                std::vector<int> points;
                for (int i = 0; i < static_cast<int>(state.text.size()); ++i)
                    if (state.text[i] == '_') points.push_back(i);
                const int position = points.empty() ? 0 : points[random % points.size()];
                IncModel::moveCursorTo(state, position, false);
                switch (step % 12) {
                case 1: case 2: case 3: case 4:
                    IncModel::insertAtCursor(state, "字😀"); break;
                case 5: IncModel::insertAtCursor(state, "\nextra_\n"); break;
                case 6: IncModel::undoEdit(state); break;
                case 7: IncModel::redoEdit(state); break;
                case 8: {
                    const int begin = static_cast<int>(state.text.find("TAIL_STATIC"));
                    IncModel::moveCursorTo(state, begin, false);
                    IncModel::moveCursorTo(state, begin + 4, true);
                    IncModel::insertAtCursor(state, "TAIL"); break;
                }
                case 9: IncModel::moveCursorTo(state, 0, false);
                    IncModel::insertAtCursor(state, "# "); break;
                case 10: IncModel::loadDocument(state, docs[kind]); break;
                default: IncModel::insertAtCursor(state, " **wide wide wide wide wide** "); break;
                }
            }
            std::set<int> folds;
            const auto child = state.text.find("## Child");
            if (step % 3 == 0 && child != std::string::npos) folds.insert(static_cast<int>(child));
            const auto* foldInput = folds.empty() ? nullptr : &folds;
            components::input_detail::DecoratorEditInfo info{
                state.textRevision, step % 11 != 0, &state.pendingEdit}; // includes untrusted IME-style requests
            const auto& plan = neo::lp::cachedPlan(state.text, &info);
            const auto next = neo::lp::cachedDecorationSnapshot(plan, neo::lp::planCache().version,
                state.cursor, style, "monospace", theme, {}, foldInput, state.text, &colors, &info);
            const auto actual = IncModel::InputLayout::build(state, width, 600, 480, 10, 10, 10,
                19.2f, "monospace", 16, true, next.get(), next);
            const auto actualStats = IncModel::debugLayoutStats();
            const auto fullPlan = neo::buildLpPlan(state.text);
            std::vector<IncDeco> oracle;
            neo::lp::buildDecorations(fullPlan, style, state.cursor, oracle, "monospace", {},
                                     foldInput, state.text, &colors);
            IncModel::InputState reference;
            reference.text = state.text; reference.cursor = state.cursor;
            reference.selectionStart = state.selectionStart; reference.selectionEnd = state.selectionEnd;
            reference.verticalScroll = state.verticalScroll; reference.followCaret = state.followCaret;
            const auto expected = IncModel::InputLayout::build(reference, width, 600, 480, 10, 10, 10,
                19.2f, "monospace", 16, true, &oracle);
            const auto label = "stable-edit kind=" + std::to_string(kind) + " step=" + std::to_string(step);
            check(planFieldDiff(plan, fullPlan).empty(), label + " full parser: " + planFieldDiff(plan, fullPlan));
            std::string decorDiff;
            if (next->size() != oracle.size()) decorDiff = "count";
            else for (std::size_t i = 0; i < oracle.size(); ++i) {
                decorDiff = decorationFieldDiff((*next)[i], oracle[i]);
                if (!decorDiff.empty()) { decorDiff = "row=" + std::to_string(i) + " " + decorDiff; break; }
            }
            check(decorDiff.empty(), label + " decorations: " + decorDiff + " plan: " + planFieldDiff(plan, fullPlan));
            std::string diff;
            if (state.cachedLines.size() != reference.cachedLines.size()) diff = "count";
            else for (std::size_t i = 0; i < state.cachedLines.size(); ++i) {
                diff = lineFieldDiff(state.cachedLines[i], reference.cachedLines[i]);
                if (diff.empty() && state.cachedLines[i].top != reference.cachedLines[i].top) diff = "top";
                if (!diff.empty()) break;
            }
            check(diff.empty() && state.cachedTextWidth == reference.cachedTextWidth &&
                state.cachedGeometry.total() == reference.cachedGeometry.total(), label + " full layout: " + diff);
            check(actual.cursorX == expected.cursorX && actual.cursorY == expected.cursorY &&
                actual.cursorLine == expected.cursorLine, label + " caret geometry");
            const core::Rect bounds{0, 0, width + 20, 600};
            for (float y : {20.0f, 100.0f, 390.0f})
                check(actual.cursorFromPointer(width / 2, y, bounds, width + 20, 10) ==
                    expected.cursorFromPointer(width / 2, y, bounds, width + 20, 10), label + " pointer hit");
            if (held) check(*held == frozen, label + " previous immutable generation");
            if (actualStats.editPatched > stats.editPatched) {
                check(state.cachedLines.data() == storage, label + " retained line storage");
                check(actualStats.geometryRebuilt == stats.geometryRebuilt,
                      label + " retained precise geometry");
            }
            if (kind == 2) check(actualStats.editPatched == stats.editPatched,
                                 label + " table dependency path retained");
        }
    }
    check(IncModel::debugLayoutStats().editPatched > initialStats.editPatched,
          "edit oracle exercises in-place stable geometry updates");
}

void testCursorLayoutTransitions() {
    using Changes = components::input_detail::LineDecorationChanges;
    const std::string doc = "# Heading **bold**\n\nPlain 中文 é **strong** `code` suffix\n"
        "continuation *across\nsource lines* tail\n\n- [x] task **done**\n\n"
        "> quote text\n> next\n\n```cpp\n/* comment\nend */ int value = 42;\n```\n\n"
        "| A **bold** | B |\n| --- | --- |\n| cell | 42 |\n\n"
        "## Child\n\nfold first\nfold second\n\n"
        "Setext **title**\n===\n\nFinal paragraph 中文 emoji 😀\n";
    const auto plan = neo::buildLpPlan(doc);
    const auto statsBefore = IncModel::debugLayoutStats();
    for (auto theme : {neo::ThemeMode::Dark, neo::ThemeMode::Light}) {
        const auto colors = neo::editorColors(theme);
        const auto style = neo::markdownStyle(16, "monospace", "monospace", colors);
        for (float width : {118.0f, 800.0f}) for (bool folded : {false, true}) {
            neo::lp::invalidateDecorationCache();
            std::set<int> folds{static_cast<int>(doc.find("## Child"))};
            const auto* foldInput = folded ? &folds : nullptr;
            IncModel::InputState state;
            state.text = doc; state.textRevision = 7;
            components::input_detail::DecoratorEditInfo info{7, true, nullptr};
            Changes changes;
            info.changes = &changes;
            unsigned random = 17;
            for (int step = 0; step < 160; ++step) {
                random = random * 1664525u + 1013904223u;
                state.cursor = step < 40 ? step : static_cast<int>(random % doc.size());
                const auto held = state.decorationSnapshot;
                const auto frozen = held ? held->copyRows() : std::vector<IncDeco>{};
                const auto next = neo::lp::cachedDecorationSnapshot(plan, 99001, state.cursor, style,
                    "monospace", theme, {}, foldInput, doc, &colors, &info);
                if (!changes.rows.empty()) {
                    check(changes.previous.lock() == held && changes.next.lock() == next,
                          "cursor transition must bind both immutable generations");
                    std::vector<int> actual;
                    for (std::size_t row = 0; row < next->size(); ++row)
                        if ((*next)[row] != (*held)[row]) actual.push_back(static_cast<int>(row));
                    check(actual == changes.rows, "published rows must include all and only actual changes");
                    auto invalid = changes;
                    invalid.textRevision = 8;
                    check(!IncModel::patchCursorDecorations(state, "monospace", 16, width, next, invalid),
                          "matching generations with a different text revision must be rejected");
                    invalid = changes;
                    invalid.next = held;
                    check(!IncModel::patchCursorDecorations(state, "monospace", 16, width, next, invalid),
                          "a transition for a different target generation must be rejected");
                }
                IncModel::ensureLayoutCache(state, "monospace", 16, width, true, next.get(), next, &changes);
                std::vector<IncDeco> oracle;
                neo::lp::buildDecorations(plan, style, state.cursor, oracle, "monospace", {}, foldInput, doc, &colors);
                IncModel::InputState reference;
                reference.text = doc;
                IncModel::ensureLayoutCache(reference, "monospace", 16, width, true, &oracle);
                std::string diff;
                if (state.cachedLines.size() != reference.cachedLines.size()) diff = "count";
                else for (std::size_t row = 0; row < state.cachedLines.size(); ++row) {
                    diff = lineFieldDiff(state.cachedLines[row], reference.cachedLines[row]);
                    if (!diff.empty() || state.cachedLines[row].top != reference.cachedLines[row].top) {
                        if (diff.empty()) diff = "top";
                        break;
                    }
                }
                check(diff.empty() && state.cachedTextWidth == reference.cachedTextWidth &&
                    state.cachedGeometry.total() == reference.cachedGeometry.total(),
                    "cursor patch must equal full layout: " + diff);
                if (held) check(*held == frozen, "cursor layout must not mutate previous decoration generations");
                check(changes.previous.lock() != state.decorationSnapshot || changes.rows.empty(),
                      "a consumed transition cannot be applied a second time");
            }
            info.committed = false;
            neo::lp::cachedDecorationSnapshot(plan, 99001, 0, style, "monospace", theme,
                {}, foldInput, doc, &colors, &info);
            check(changes.rows.empty(), "IME requests must not publish trusted cursor transitions");
        }
    }
    check(IncModel::debugLayoutStats().cursorPatched > statsBefore.cursorPatched,
          "cursor oracle must exercise the bounded patch path");
}

}  // namespace

int main() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    testStableEditLayout();
    testStableOffsetEditSnapshots();
    testEditedPagePublication();
    testShiftedEditPages();
    testCursorLayoutTransitions();
    testPagedSnapshotStorage();
    testImmutableSnapshots();
    testCursorSemanticSnapshots();
    testCursorPartialBounds();
    // Real Markdown -> decorations -> selection geometry, including concealed
    // inline code/strong markup and the collapsed table separator.
    {
        using M = components::input_detail::InputModel;
        const auto style = neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
        const std::string doc = "| 文件 | 说明 |\n| --- | --- |\n"
            "| `ui/batch_ops.py` | **批量操作的写回范式五步顺序事务外壳细则见模块设计验收判断完成后为假**😀 |\n";
        const auto plan = neo::buildLpPlan(doc);
        std::vector<components::input_detail::LineDecoration> decorations;
        neo::lp::buildDecorations(plan,style,0,decorations,style.fontFamily,{},nullptr,doc);
        M::InputState selectionState;
        selectionState.text=doc; selectionState.textRevision=1; selectionState.followCaret=false;
        check(decorations.size()>=3 && decorations[2].cells.size()==2,
              "selection fixture must parse two real Markdown table cells");
        if(decorations.size()>=3 && decorations[2].cells.size()==2) {
            const auto& cells=decorations[2].cells;
            selectionState.selectionStart=cells[1].end;
            selectionState.selectionEnd=cells[0].beg;
            const auto layout=M::InputLayout::build(selectionState,300,600,324,12,12,12,20.8f,
                style.fontFamily,16,true,&decorations);
            int bodySegments=0;
            for(size_t i=0;i<layout.lineList().size();++i) {
                const auto& line=layout.lineList()[i];
                if(line.lineNumber!=3) continue;
                ++bodySegments;
                const float expectedY=12+layout.geometryTable().top(static_cast<int>(i))+line.textShiftY;
                bool covered=false;
                for(const auto& r:layout.selectionRects) {
                    if(std::fabs(r.y-expectedY)<0.02f) covered=true;
                    check(std::fabs(r.height-20.8f)<0.02f,"real Markdown table selection must have equal text-band heights");
                }
                check(covered,"real Markdown reverse selection must cover every body segment");
            }
            check(bodySegments>=3,"real Markdown selection fixture must exercise wrapping");
            check(selectionState.text.substr(cells[0].beg,cells[1].end-cells[0].beg).find("😀")!=std::string::npos,
                  "selection byte range must preserve the final emoji and hidden Markdown source");
        }
    }
    // 大纲只投影已有解析计划：setext 配对只占一项，代码围栏内的 # 不算标题，
    // 源字节位置可直接交给编辑器跳转，编辑后的新计划也会给出新标题。
    {
        const std::string doc =
            "# 顶层 **重点** #\n\n副标题\n------\n\n```md\n# 伪标题\n```\n\n### 末节\n";
        const auto entries = neo::outlineFromPlan(doc, neo::buildLpPlan(doc));
        check(entries.size() == 3, "大纲应只列出三个真实标题");
        if (entries.size() == 3) {
            check(entries[0].title == "顶层 重点" && entries[0].level == 1 &&
                      entries[0].byteOffset == 0,
                  "ATX 标题应去掉标记并定位源码行首");
            check(entries[1].title == "副标题" && entries[1].level == 2 &&
                      entries[1].byteOffset == static_cast<int>(doc.find("副标题")),
                  "setext 文本与下划线只生成一条大纲");
            check(entries[2].title == "末节" && entries[2].level == 3 &&
                      entries[2].byteOffset == static_cast<int>(doc.find("### 末节")),
                  "末节应保留正确层级与跳转字节位置");
        }
        const std::string edited = "正文\n\n## 新标题\n";
        const auto updated = neo::outlineFromPlan(edited, neo::buildLpPlan(edited));
        check(updated.size() == 1 && updated[0].title == "新标题" &&
                  updated[0].byteOffset == static_cast<int>(edited.find("## 新标题")),
              "编辑后的计划应产出更新的大纲");
        check(neo::outlineFromPlan("普通纯文本\n", neo::buildLpPlan("普通纯文本\n")).empty(),
              "没有 Markdown 标题时大纲为空");
    }
    // 临时诊断（用完删除）：把实机探针文档的逐行语义打出来。
    {
        const components::MarkdownStyle diagStyle =
            neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
        const std::string diagDoc =
            "Divider idle probe.\n\nText before the rule.\n\n---\n\nText after the rule.\n";
        const neo::LpPlan diagPlan = neo::buildLpPlan(diagDoc);
        std::printf("---- diag plan ----\n");
        for (std::size_t i = 0; i < diagPlan.lines.size(); ++i) {
            const neo::LpLine& line = diagPlan.lines[i];
            std::printf("line %zu kind=%d setext=%d block=[%d,%d] src=[%d,%d] '%s'\n", i,
                        static_cast<int>(line.kind), line.setext ? 1 : 0, line.blockBeg,
                        line.blockEnd, line.srcBeg, line.srcEnd,
                        diagDoc.substr(static_cast<std::size_t>(line.srcBeg),
                                       static_cast<std::size_t>(line.srcEnd - line.srcBeg)).c_str());
        }
        std::vector<components::input_detail::LineDecoration> diagTable;
        neo::lp::buildDecorations(diagPlan, diagStyle, 0, diagTable);
        for (std::size_t i = 0; i < diagTable.size(); ++i) {
            std::printf("dec %zu lh=%.2f holes=%zu gridA=%.2f thick=%d\n", i,
                        static_cast<double>(diagTable[i].lineHeight), diagTable[i].holes.size(),
                        static_cast<double>(diagTable[i].box.gridColor.a),
                        static_cast<int>(diagTable[i].box.horizontalRuleThickness));
        }
    }
    using components::input_detail::LineDecoration;
    using components::input_detail::LineHole;
    using components::input_detail::LineRun;

    const components::MarkdownStyle style =
        neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace");
    const neo::LpPlan plan = neo::buildLpPlan(kDoc);
    std::printf("文档 %zu 字节 / %zu 行；md4c=%s，扫描器与 md4c 起点不一致 %d 处，非法区间 %d 条\n",
                kDoc.size(), plan.lines.size(), plan.stats.usedMd4c ? "启用" : "未启用",
                plan.stats.markerMismatch, plan.stats.invalidRanges);
    check(plan.stats.usedMd4c, "本次应当用到 md4c（否则没有行内片段）");
    check(plan.stats.invalidRanges == 0, "lp_plan 不应产生越界区间");

    const int headingLine = 0;
    const int bodyLine = 2;
    const int listLine = 4;
    const int fenceOpenLine = 6;
    const int fenceCloseLine = 8;

    // ── ① 块级标记：光标在本块内 => 露出来（这里用标题行验证）──
    {
        std::vector<LineDecoration> table;
        auto cursor = static_cast<int>(kDoc.find("标题"));
        neo::lp::buildDecorations(plan, style, cursor, table);
        check(table.size() == plan.lines.size(), "装饰表行数与计划一致");
        check(holesText(kDoc, table, headingLine).empty(), "光标在标题行时 `# ` 应当露出来");
        check(holesText(kDoc, table, bodyLine).find("**") != std::string::npos,
              "光标在标题行时，正文行的标记应当仍然隐藏（其它块不受影响）");
    }

    // ── ② 行内标记：光标不在任何片段内（行首）=> 全藏 ──
    {
        std::vector<LineDecoration> table;
        const neo::LpLine& line = plan.lines[bodyLine];
        neo::lp::buildDecorations(plan, style, line.srcBeg, table);
        const std::string hidden = holesText(kDoc, table, bodyLine);
        check(hidden.find("**") != std::string::npos, "`**` 应当隐藏");
        check(hidden.find('`') != std::string::npos, "`` ` `` 应当隐藏");
        check(hidden.find('[') != std::string::npos, "链接开标记应当隐藏");
        check(hidden.find("](https://a.b)") != std::string::npos, "链接闭标记应当隐藏");
        check(hidden.find("~~") != std::string::npos, "`~~` 应当隐藏");
        check(hidden.find("正文") == std::string::npos, "正文文字不应被隐藏");
    }

    // ── ③ 行内标记：光标进入某个片段 => 只露该片段（**这是本轮改的核心规则**）──
    {
        std::vector<LineDecoration> table;
        const neo::LpLine& line = plan.lines[bodyLine];
        const int boldBeg = findInLine(kDoc, line, "**粗体**");
        check(boldBeg > 0, "样例里应当能定位到 `**粗体**`");
        if (boldBeg > 0) {
            const int cursor = boldBeg + 4;  // 落在"粗"与"体"之间
            neo::lp::buildDecorations(plan, style, cursor, table);
            const std::string hidden = holesText(kDoc, table, bodyLine);
            check(hidden.find("**") == std::string::npos, "光标在 `**粗体**` 内时，这对 `**` 应当露出来");
            check(hidden.find('`') != std::string::npos, "同一行的 `` ` `` 仍应隐藏（粒度是片段，不是整行）");
            check(hidden.find("](https://a.b)") != std::string::npos, "同一行的链接标记仍应隐藏");
            check(hidden.find("~~") != std::string::npos, "同一行的 `~~` 仍应隐藏");
            // 光标在片段内容里时，最终显形的文本应当就是"标记都在"的那一段
            check(!holesContain(table, bodyLine, boldBeg, boldBeg + 2),
                  "光标所在片段的开标记不应出现在隐藏区间里");
        }
    }

    // ── ④ 行内标记：光标在链接的 URL 里 => 链接标记露出来 ──
    {
        std::vector<LineDecoration> table;
        const neo::LpLine& line = plan.lines[bodyLine];
        const int urlBeg = findInLine(kDoc, line, "https://a.b");
        check(urlBeg > 0, "样例里应当能定位到链接 URL");
        if (urlBeg > 0) {
            neo::lp::buildDecorations(plan, style, urlBeg, table);
            const std::string hidden = holesText(kDoc, table, bodyLine);
            check(hidden.find("](https://a.b)") == std::string::npos, "光标在 URL 内时链接标记应当露出来");
            check(hidden.find("**") != std::string::npos, "链接之外的 `**` 仍应隐藏");
        }
    }

    // ── ⑤ 列表项：光标在列表块内 => 容器标记露出来，但行内标记仍按片段规则 ──
    {
        std::vector<LineDecoration> table;
        const neo::LpLine& line = plan.lines[listLine];
        const int itemTextBeg = findInLine(kDoc, line, "列表项");
        check(itemTextBeg > 0, "样例里应当能定位到列表项文字");
        if (itemTextBeg > 0) {
            neo::lp::buildDecorations(plan, style, itemTextBeg, table);
            const std::string hidden = holesText(kDoc, table, listLine);
            check(hidden.find("- ") == std::string::npos, "光标在列表项内时 `- ` 应当露出来");
            // "列表项"和"尾巴"都不在 `**粗**` 里，所以那对 `**` 应当隐藏
            check(hidden.find("**") != std::string::npos,
                  "光标不在 `**粗**` 内时，它的标记仍应隐藏（即便光标在同一个块里）");
        }
    }

    // ── ⑥ 代码块：光标在块内 => 围栏行露出来 ──
    {
        std::vector<LineDecoration> table;
        const neo::LpLine& line = plan.lines[fenceOpenLine + 1];
        neo::lp::buildDecorations(plan, style, line.srcBeg + 1, table);
        check(holesText(kDoc, table, fenceOpenLine).empty(), "光标在代码块内时开围栏应当露出来");
        check(holesText(kDoc, table, fenceCloseLine).empty(), "光标在代码块内时闭围栏应当露出来");
    }

    // ── ⑦ 样式段：粗体 / 行内代码 / 链接 / 删除线 ──
    {
        std::vector<LineDecoration> table;
        neo::lp::buildDecorations(plan, style, plan.lines[bodyLine].srcBeg, table);
        const std::vector<LineRun>& runs = table[bodyLine].runs;
        std::printf("正文行的样式段 %zu 个：\n", runs.size());
        for (const LineRun& run : runs) {
            std::printf("  [%d,%d) weight=%d mono=%s color=%s strike=%s text=\"%s\"\n", run.beg, run.end,
                        run.style.weight, run.style.fontFamily.empty() ? "-" : run.style.fontFamily.c_str(),
                        run.style.color.a > 0.0f ? "有" : "-", run.style.strike ? "有" : "-",
                        escape(slice(kDoc, run.beg, run.end)).c_str());
        }
        bool hasStrong = false;
        bool hasCode = false;
        bool hasLink = false;
        bool hasStrike = false;
        for (const LineRun& run : runs) {
            const std::string text = slice(kDoc, run.beg, run.end);
            if (text == "粗体" && run.style.weight >= 700) {
                hasStrong = true;
            }
            if (text == "代码" && !run.style.fontFamily.empty() && run.style.background.a > 0.0f) {
                hasCode = true;
            }
            if (text == "链接" && run.style.color.a > 0.0f) {
                hasLink = true;
            }
            if (text == "删除" && run.style.strike) {
                hasStrike = true;
            }
        }
        check(hasStrong, "`**粗体**` 的内容段应当是粗体（weight >= 700）");
        check(hasCode, "`` `代码` `` 的内容段应当等宽 + 带底色");
        check(hasLink, "链接文字应当带主题色");
        check(hasStrike, "`~~删除~~` 的内容段应当带删除线");
        // 段互不重叠且升序（组件依赖这个前提）
        int previousEnd = -1;
        bool runsOrdered = true;
        for (const LineRun& run : runs) {
            if (run.beg < previousEnd) {
                runsOrdered = false;
            }
            previousEnd = run.end;
        }
        check(runsOrdered, "样式段应当升序且互不重叠");
    }

    // ── ⑧ 标题：整行一段且加粗，文字色是标题色 ──
    {
        std::vector<LineDecoration> table;
        neo::lp::buildDecorations(plan, style, plan.lines[bodyLine].srcBeg, table);
        check(table[headingLine].textColor.a > 0.0f, "标题行应当有行级文字色");
        check(!table[headingLine].runs.empty(), "标题行应当有样式段");
        if (!table[headingLine].runs.empty()) {
            const LineRun& run = table[headingLine].runs.front();
            check(run.style.weight >= 700, "标题内容段应当是粗体");
            check(run.beg >= plan.lines[headingLine].srcBeg + 2, "标题段应当从容器标记之后开始");
            check(slice(kDoc, run.beg, run.end) == "Head 标题",
                  "标题段内容应是不含 `# ` 的正文（实际=\"" + escape(slice(kDoc, run.beg, run.end)) + "\"）");
        }
        check(table[bodyLine].textColor.a == 0.0f, "正文行不应有特殊的行级文字色");
    }

    // ── ⑨ 自检：隐藏区间都在本行内、升序、互不重叠、且是 conceal 的子集 ──
    {
        int badGeometry = 0;
        int notSubset = 0;
        for (int cursorLine = 0; cursorLine < static_cast<int>(plan.lines.size()); ++cursorLine) {
            std::vector<LineDecoration> table;
            neo::lp::buildDecorations(plan, style, plan.lines[static_cast<std::size_t>(cursorLine)].srcBeg, table);
            for (std::size_t i = 0; i < table.size(); ++i) {
                const neo::LpLine& line = plan.lines[i];
                int previousEnd = -1;
                for (const components::input_detail::LineHole& hole : table[i].holes) {
                    if (hole.empty() || hole.beg < line.srcBeg || hole.end > line.srcEnd || hole.beg < previousEnd) {
                        ++badGeometry;
                    }
                    previousEnd = hole.end;
                    // 子集判定：这条 hole 必须被某条 conceal 完整覆盖
                    bool covered = false;
                    for (const neo::LpRange& range : line.conceal) {
                        if (hole.beg >= range.beg && hole.end <= range.end) {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered) {
                        ++notSubset;
                        if (notSubset <= 5) {
                            std::printf("  !! 行 %zu 的隐藏区间 [%d,%d)\"%s\" 不在 conceal 里\n", i, hole.beg,
                                        hole.end, escape(slice(kDoc, hole.beg, hole.end)).c_str());
                        }
                    }
                }
            }
        }
        check(badGeometry == 0, "隐藏区间几何（越界/空/乱序/重叠）应为 0，实际 " + std::to_string(badGeometry));
        check(notSubset == 0, "隐藏区间必须是 conceal 的子集（适配层只能少藏），越界 " + std::to_string(notSubset) + " 条");
    }

    // ── ⑤ S3b：行级矩形（代码块底色 / 引用竖条）──
    // 判据说明：这里只验证**装饰表里给的标记**（颜色有无、块首块末、圆角），
    // "画出来对不对"属视图层，由实机像素判据验收（参考/tools/verify_live_preview.py）。
    {
        const neo::LpPlan boxPlan = neo::buildLpPlan(kBoxDoc);
        std::vector<LineDecoration> boxTable;
        neo::lp::buildDecorations(boxPlan, style, 0, boxTable);
        check(boxTable.size() == boxPlan.lines.size(), "矩形样例：装饰表行数与计划一致");

        int quoteLines = 0;
        int quoteMissingBar = 0;
        int barOutsideQuote = 0;
        int codeLines = 0;
        int codeMissingBackground = 0;
        int backgroundOutsideCode = 0;
        int blockFirstCount = 0;
        int blockLastCount = 0;
        int radiusLines = 0;
        for (std::size_t i = 0; i < boxPlan.lines.size() && i < boxTable.size(); ++i) {
            const neo::LpLine& sourceLine = boxPlan.lines[i];
            const LineDecoration& decoration = boxTable[i];
            if (sourceLine.quoteDepth > 0) {
                ++quoteLines;
                if (!(decoration.box.barColor.a > 0.0f && decoration.box.barWidth > 0.0f)) {
                    ++quoteMissingBar;
                }
            } else if (decoration.box.barColor.a > 0.0f) {
                ++barOutsideQuote;
            }
            if (sourceLine.kind == neo::LpKind::Code) {
                ++codeLines;
                if (decoration.box.background.a <= 0.0f) {
                    ++codeMissingBackground;
                } else if (decoration.box.backgroundRadius > 0.0f) {
                    ++radiusLines;
                }
                if (decoration.box.backgroundBlockFirst) {
                    ++blockFirstCount;
                }
                if (decoration.box.backgroundBlockLast) {
                    ++blockLastCount;
                }
            } else if (decoration.box.background.a > 0.0f) {
                ++backgroundOutsideCode;
            }
        }
        check(quoteLines == 2, "矩形样例：应当有 2 行引用，实际 " + std::to_string(quoteLines));
        check(quoteMissingBar == 0, "引用行都应带竖条，缺 " + std::to_string(quoteMissingBar) + " 行");
        check(barOutsideQuote == 0, "非引用行不应带竖条，实际 " + std::to_string(barOutsideQuote) + " 行");
        check(codeLines == 6, "矩形样例：两个代码块共 6 行（含围栏），实际 " + std::to_string(codeLines));
        check(codeMissingBackground == 0,
              "代码块的每一行（含围栏）都应有底色，缺 " + std::to_string(codeMissingBackground) + " 行");
        check(backgroundOutsideCode == 0, "代码块之外不应有底色，实际 " + std::to_string(backgroundOutsideCode) + " 行");
        // 两个**紧贴**的代码块各算一块：底色不连片，各自两端才有圆角。
        check(blockFirstCount == 2, "两个代码块各应有一个块首，实际 " + std::to_string(blockFirstCount));
        check(blockLastCount == 2, "两个代码块各应有一个块末，实际 " + std::to_string(blockLastCount));
        check(radiusLines == 6, "代码块底色都应带圆角半径，实际 " + std::to_string(radiusLines) + " 行");
    }

    // ── ⑰ R3：代码块起始围栏行的语言标签（Obsidian LP 的 "JavaScript"）──
    {
        const std::string langDoc =
            "```js\n"
            "let a = 1;\n"
            "```\n"
            "\n"
            "```zig\n"
            "const b = 2;\n"
            "```\n"
            "\n"
            "```\n"
            "plain\n"
            "```\n";
        const neo::LpPlan langPlan = neo::buildLpPlan(langDoc);
        std::vector<IncDeco> langTable;
        neo::lp::buildDecorations(langPlan, style, 0, langTable);
        check(langTable.size() >= 11, "语言标签样例：行数应为 11，实际 " + std::to_string(langTable.size()));
        if (langTable.size() >= 11) {
            check(langTable[0].languageLabel == "JavaScript",
                  "```js 的起始围栏应标成 JavaScript，实际 [" + langTable[0].languageLabel + "]");
            check(langTable[1].languageLabel.empty(), "代码块内容行不应带语言标签");
            check(langTable[2].languageLabel.empty(), "结束围栏行不应带语言标签");
            check(langTable[4].languageLabel == "zig",
                  "未收录的语言按 info 串原样显示，实际 [" + langTable[4].languageLabel + "]");
            check(langTable[8].languageLabel.empty(), "没有 info 串的围栏不画标签");
        }
        // 别名表本身（含大小写与未知回退）。
        check(neo::lp::codeLanguageLabel("JS") == "JavaScript", "info 串大小写不敏感");
        check(neo::lp::codeLanguageLabel("py") == "Python", "py 应显示为 Python");
        check(neo::lp::codeLanguageLabel("cpp") == "C++", "cpp 应显示为 C++");
        check(neo::lp::codeLanguageLabel("") .empty(), "空 info 串应返回空");
    }

    // ── ⑩ S3e frontmatter 弱化 + S3c 任务复选框 ──
    {
        const neo::LpPlan prefixPlan = neo::buildLpPlan(kPrefixDoc);
        std::vector<LineDecoration> prefixTable;
        neo::lp::buildDecorations(prefixPlan, style, 0, prefixTable);
        check(prefixTable.size() == prefixPlan.lines.size(), "前缀样例：装饰表行数与计划一致");

        int frontmatterLines = 0;
        int wrongSize = 0;
        int wrongMutedColor = 0;
        int missingBackground = 0;
        int wrongFirstMark = 0;
        int wrongLastMark = 0;
        for (std::size_t i = 0; i < prefixPlan.lines.size() && i < prefixTable.size(); ++i) {
            if (prefixPlan.lines[i].kind != neo::LpKind::Frontmatter) {
                continue;
            }
            ++frontmatterLines;
            const LineDecoration& decoration = prefixTable[i];
            const bool first = i == 0 || prefixPlan.lines[i - 1].kind != neo::LpKind::Frontmatter;
            const bool last = i + 1 >= prefixPlan.lines.size() ||
                              prefixPlan.lines[i + 1].kind != neo::LpKind::Frontmatter;
            if (std::fabs(decoration.fontSize - style.codeSize) > 0.01f) {
                ++wrongSize;
            }
            if (!sameColor(decoration.textColor, style.muted)) {
                ++wrongMutedColor;
            }
            if (decoration.box.background.a <= 0.0f || decoration.box.backgroundRadius <= 0.0f) {
                ++missingBackground;
            }
            if (decoration.box.backgroundBlockFirst != first) {
                ++wrongFirstMark;
            }
            if (decoration.box.backgroundBlockLast != last) {
                ++wrongLastMark;
            }
        }
        check(frontmatterLines == 4, "前缀样例：应有 4 行 frontmatter，实际 " + std::to_string(frontmatterLines));
        check(wrongSize == 0, "frontmatter 应使用代码块档字号，不符 " + std::to_string(wrongSize) + " 行");
        check(wrongMutedColor == 0, "frontmatter 文字应为次要色，不符 " + std::to_string(wrongMutedColor) + " 行");
        check(missingBackground == 0, "frontmatter 应有底色与圆角，缺 " + std::to_string(missingBackground) + " 行");
        check(wrongFirstMark == 0, "frontmatter 只在整段首行标块首，不符 " + std::to_string(wrongFirstMark) + " 行");
        check(wrongLastMark == 0, "frontmatter 只在整段末行标块末，不符 " + std::to_string(wrongLastMark) + " 行");

        int backgroundOutside = 0;
        for (std::size_t i = 0; i < prefixPlan.lines.size() && i < prefixTable.size(); ++i) {
            if (prefixPlan.lines[i].kind != neo::LpKind::Frontmatter &&
                prefixTable[i].box.background.a > 0.0f) {
                ++backgroundOutside;
            }
        }
        check(backgroundOutside == 0,
              "frontmatter 之外不应有底色，实际 " + std::to_string(backgroundOutside) + " 行");

        int taskLines = 0;
        int taskWrongGlyph = 0;
        int glyphOutsideTask = 0;
        for (std::size_t i = 0; i < prefixPlan.lines.size() && i < prefixTable.size(); ++i) {
            const bool isTask = prefixPlan.lines[i].kind == neo::LpKind::TaskItem;
            const int codepoint = prefixTable[i].glyph.codepoint;
            if (isTask) {
                ++taskLines;
                const int expected = prefixPlan.lines[i].taskChecked ? neo::lp::kTaskCheckedIcon
                                                                     : neo::lp::kTaskIcon;
                if (codepoint != expected) {
                    ++taskWrongGlyph;
                }
            } else if (codepoint != 0) {
                ++glyphOutsideTask;
            }
        }
        check(taskLines == 2, "前缀样例：应有 2 个任务项，实际 " + std::to_string(taskLines));
        check(taskWrongGlyph == 0, "任务项的复选框图标不符（勾选态给错），共 " + std::to_string(taskWrongGlyph) + " 行");
        check(glyphOutsideTask == 0, "非任务行不应有复选框，实际 " + std::to_string(glyphOutsideTask) + " 行");

        const std::size_t uncheckedLine = 7;
        const std::size_t checkedLine = 8;
        if (checkedLine < prefixTable.size()) {
            check(prefixTable[uncheckedLine].glyph.codepoint == neo::lp::kTaskIcon,
                  "第 7 行（未勾）应给空方框图标");
            check(prefixTable[checkedLine].glyph.codepoint == neo::lp::kTaskCheckedIcon,
                  "第 8 行（已勾）应给带勾方框图标");
            check(sameColor(prefixTable[checkedLine].glyph.color, style.accent),
                  "已勾选的复选框应使用主色");
            check(sameColor(prefixTable[uncheckedLine].glyph.color, style.muted),
                  "未勾选的复选框应使用次要色");
        }
    }

    // ── ⑫ S3f 批次 A：任务状态字符偏移 + 点击翻转 ──
    {
        const neo::LpPlan prefixPlan = neo::buildLpPlan(kPrefixDoc);
        using InputModel = components::input_detail::InputModel;

        // 第 7/8 行是任务行：taskStateByte 必须正指着 [ ]/[x] 中间那个状态字符。
        const neo::LpLine* uncheckedLine = prefixPlan.lineAt(7);
        const neo::LpLine* checkedLine = prefixPlan.lineAt(8);
        const neo::LpLine* plainLine = prefixPlan.lineAt(9);
        check(uncheckedLine != nullptr && uncheckedLine->kind == neo::LpKind::TaskItem,
              "第 7 行应是任务行");
        check(checkedLine != nullptr && checkedLine->kind == neo::LpKind::TaskItem,
              "第 8 行应是任务行");
        if (uncheckedLine != nullptr && checkedLine != nullptr) {
            const int posA = uncheckedLine->taskStateByte;
            const int posB = checkedLine->taskStateByte;
            check(posA == uncheckedLine->srcBeg + 3,
                  "第 7 行状态字符应在线首 +3（`- [ ]`），实际 " + std::to_string(posA));
            check(posA > 0 && posA + 1 < static_cast<int>(kPrefixDoc.size()) &&
                      kPrefixDoc[static_cast<std::size_t>(posA)] == ' ' &&
                      kPrefixDoc[static_cast<std::size_t>(posA - 1)] == '[' &&
                      kPrefixDoc[static_cast<std::size_t>(posA + 1)] == ']',
                  "第 7 行 taskStateByte 未指向 [ ] 的状态字符");
            check(posB > 0 && posB + 1 < static_cast<int>(kPrefixDoc.size()) &&
                      kPrefixDoc[static_cast<std::size_t>(posB)] == 'x' &&
                      kPrefixDoc[static_cast<std::size_t>(posB - 1)] == '[' &&
                      kPrefixDoc[static_cast<std::size_t>(posB + 1)] == ']',
                  "第 8 行 taskStateByte 未指向 [x] 的状态字符");

            // 点击判定入口：任务行任意字节 → 偏移；非任务行/非任务字节 → -1。
            check(prefixPlan.taskStateByteFor(uncheckedLine->srcBeg) == posA,
                  "taskStateByteFor(任务行字节) 应返回状态字符偏移");
            check(prefixPlan.taskStateByteFor(posA) == posA,
                  "taskStateByteFor(状态字符本身) 也应返回偏移");
        }
        if (plainLine != nullptr) {
            check(plainLine->kind != neo::LpKind::TaskItem && plainLine->taskStateByte == -1,
                  "普通列表项不应带任务状态偏移");
            check(prefixPlan.taskStateByteFor(plainLine->srcBeg) == -1,
                  "普通列表项点击不应命中任务判定");
        }
        check(prefixPlan.taskStateByteFor(0) == -1, "frontmatter 行点击不应命中任务判定");

        // 围栏里的 `- [ ]` 不是任务（扫描器在进围栏前 continue），判定必须为 -1。
        const neo::LpPlan fencePlan = neo::buildLpPlan("```\n- [ ] 假的\n```\n");
        check(fencePlan.taskStateByteFor(5) == -1, "代码块内的 [ ] 不应命中任务判定");

        // 翻转：形状正确 → 成功且只翻状态字符；形状不对/越界 → 拒绝且文本不动；撤销可回。
        InputModel::InputState flip;
        flip.text = "- [ ] 待办一\n- [x] 已完成\n";
        flip.textRevision = 1;
        const std::string flipOriginal = flip.text;
        check(neo::flipTaskCheckboxAt(flip, 3), "翻转 [ ] → 应成功");
        check(flip.text == "- [x] 待办一\n- [x] 已完成\n",
              "翻转后只有状态字符变了，实际 \"" + escape(flip.text) + "\"");
        check(neo::flipTaskCheckboxAt(flip, 3), "翻转 [x] → 应成功（可切回）");
        check(flip.text == flipOriginal, "连续两次翻转应回到原文");
        check(!neo::flipTaskCheckboxAt(flip, 6), "落在正文里的位置应被形状校验拒绝");
        check(!neo::flipTaskCheckboxAt(flip, 0), "越界（<1）应被拒绝");
        check(!neo::flipTaskCheckboxAt(flip, static_cast<int>(flip.text.size())),
              "越界（≥末尾）应被拒绝");
        check(flip.text == flipOriginal, "被拒绝的调用不应改动文本");

        check(neo::flipTaskCheckboxAt(flip, 3), "为撤销做一次成功翻转");
        flip.cursor = 0;
        const bool undone = InputModel::undoEdit(flip);
        check(undone && flip.text == flipOriginal, "Ctrl+Z 撤销路径应恢复原文本");
    }

    // ── ⑪ S3d 斜体（降级版）：字体真有斜体字面才生成段 ──
    {
        const std::string kItalicDoc =
            "有 *斜体* 的段落\n"
            "\n"
            "***粗斜***\n";
        const neo::LpPlan italicPlan = neo::buildLpPlan(kItalicDoc);

        // 解析器本身：Windows 预装字体里 Times New Roman 有斜体字面、黑体/雅黑没有。
        const std::string simheiItalic = core::TextPrimitive::resolveItalicFontPath("C:/Windows/Fonts/simhei.ttf");
        const std::string yaheiItalic = core::TextPrimitive::resolveItalicFontPath("Microsoft YaHei");
        check(simheiItalic.empty(), "黑体没有斜体字面，resolveItalicFontPath 应返回空串");
        check(yaheiItalic.empty(), "雅黑没有斜体字面，resolveItalicFontPath 应返回空串");

        const std::string timesItalic = core::TextPrimitive::resolveItalicFontPath("C:/Windows/Fonts/times.ttf");
        std::printf("times.ttf 的斜体字面 = %s\n",
                    timesItalic.empty() ? "(无)" : timesItalic.c_str());
        if (std::filesystem::exists("C:/Windows/Fonts/times.ttf")) {
            check(!timesItalic.empty(), "times.ttf 存在时应解析出斜体字面");
            check(timesItalic != "C:/Windows/Fonts/times.ttf", "斜体字面不应就是常规字面本身");
            check(std::filesystem::exists(timesItalic), "解析出的斜体字面应当是真实存在的文件");

            std::vector<LineDecoration> table;
            neo::lp::buildDecorations(italicPlan, style, 0, table, "C:/Windows/Fonts/times.ttf");
            bool hasItalicRun = false;
            for (const LineRun& run : table[0].runs) {
                if (slice(kItalicDoc, run.beg, run.end) == "斜体" && run.style.fontFamily == timesItalic) {
                    hasItalicRun = true;
                }
            }
            check(hasItalicRun, "`*斜体*` 应产出一段 fontFamily=斜体字面的样式段");

            bool mergedBoldItalic = false;
            for (const LineRun& run : table[2].runs) {
                if (slice(kItalicDoc, run.beg, run.end) == "粗斜" && run.style.weight >= 700 &&
                    run.style.fontFamily == timesItalic) {
                    mergedBoldItalic = true;
                }
            }
            check(mergedBoldItalic, "`***粗斜***` 合并后应同时带 weight>=700 与斜体字面");
        } else {
            std::printf("  (跳过斜体装饰判据：本机没有 C:/Windows/Fonts/times.ttf)\n");
        }

        // 没有斜体字面的字体：`*斜体*` 不生成段（内容保持原样），粗体不受影响。
        {
            std::vector<LineDecoration> table;
            neo::lp::buildDecorations(italicPlan, style, 0, table, "C:/Windows/Fonts/simhei.ttf");
            check(table[0].runs.empty(), "字体没有斜体字面时，斜体段落不应产出任何样式段");
            bool boldWithoutItalic = false;
            for (const LineRun& run : table[2].runs) {
                if (slice(kItalicDoc, run.beg, run.end) == "粗斜" && run.style.weight >= 700 &&
                    run.style.fontFamily.empty()) {
                    boldWithoutItalic = true;
                }
            }
            check(boldWithoutItalic, "`***粗斜***` 在无斜体字面时仍应产出 weight=700 的粗体段");
        }
    }

    // ── ⑬ S3f 批次 B：文件头解析 + 纯图行判定 + 装饰落槽 ──
    {
        namespace fs = std::filesystem;
        const auto be32 = [](unsigned value) {
            return std::string({static_cast<char>((value >> 24) & 0xFF),
                                static_cast<char>((value >> 16) & 0xFF),
                                static_cast<char>((value >> 8) & 0xFF),
                                static_cast<char>(value & 0xFF)});
        };
        const auto be16 = [](unsigned value) {
            return std::string({static_cast<char>((value >> 8) & 0xFF),
                                static_cast<char>(value & 0xFF)});
        };
        const auto le16 = [](unsigned value) {
            return std::string({static_cast<char>(value & 0xFF),
                                static_cast<char>((value >> 8) & 0xFF)});
        };
        const auto le32 = [](unsigned value) {
            return std::string({static_cast<char>(value & 0xFF),
                                static_cast<char>((value >> 8) & 0xFF),
                                static_cast<char>((value >> 16) & 0xFF),
                                static_cast<char>((value >> 24) & 0xFF)});
        };

        fs::path tempDir = fs::temp_directory_path() / "neo_lp_image_test";
        std::error_code error;
        fs::remove_all(tempDir, error);
        error.clear();
        fs::create_directories(tempDir, error);
        check(!error, "应能创建图片测试临时目录");

        const auto writeFile = [&tempDir](const std::string& name, const std::string& bytes) {
            std::FILE* handle = std::fopen((tempDir / name).string().c_str(), "wb");
            if (handle == nullptr) {
                return;
            }
            std::fwrite(bytes.data(), 1, bytes.size(), handle);
            std::fclose(handle);
        };

        // 头部即全部需要的字节：解析器只读文件头，不校验像素/CRC。
        std::string png;
        png += std::string("\x89PNG\r\n\x1a\n", 8);
        png += std::string("\0\0\0\rIHDR", 8);  // len(4) + "IHDR" @12
        png += be32(240) + be32(135) + std::string(8, '\0');
        writeFile("pic.png", png);

        std::string big;
        big += std::string("\x89PNG\r\n\x1a\n", 8);
        big += std::string("\0\0\0\rIHDR", 8);
        big += be32(10000) + be32(5000) + std::string(8, '\0');
        writeFile("big.png", big);

        std::string gif = "GIF89a" + le16(320) + le16(200) + std::string(4, '\0');
        writeFile("a.gif", gif);

        std::string bmp = "BM" + le32(26) + le32(0) + le32(54) + le32(40) + le32(800) + le32(600);
        writeFile("b.bmp", bmp);

        std::string jpg;
        jpg += std::string("\xFF\xD8", 2);                        // SOI
        jpg += std::string("\xFF\xE0\x00\x10", 4);                // APP0 len=16
        jpg += std::string(14, '\x00');                           // APP0 载荷
        jpg += std::string("\xFF\xC0\x00\x11\x08", 5);            // SOF0 len=17 precision=8
        jpg += std::string("\x01\x00", 2);                        // height 256
        jpg += std::string("\x02\x00", 2);                        // width 512
        jpg += std::string(10, '\x00');                           // SOF 其余 + 余量
        writeFile("c.jpg", jpg);

        writeFile("broken.png", std::string("\x89PNG\r\n\x1a\n", 8));  // 截断
        writeFile("garbage.bin", std::string("not an image at all"));

        const std::string dir = tempDir.string();
        const auto extentOf = [&](const std::string& name) {
            return neo::lp::readImageExtent((tempDir / name).string());
        };

        // ① 四种格式的头解析 + 坏文件拒绝
        {
            const auto pngExtent = extentOf("pic.png");
            check(pngExtent.has_value() && pngExtent->width == 240 && pngExtent->height == 135,
                  "PNG 头应解析出 240x135");
            const auto gifExtent = extentOf("a.gif");
            check(gifExtent.has_value() && gifExtent->width == 320 && gifExtent->height == 200,
                  "GIF 头应解析出 320x200");
            const auto bmpExtent = extentOf("b.bmp");
            check(bmpExtent.has_value() && bmpExtent->width == 800 && bmpExtent->height == 600,
                  "BMP 头应解析出 800x600");
            const auto jpgExtent = extentOf("c.jpg");
            check(jpgExtent.has_value() && jpgExtent->width == 512 && jpgExtent->height == 256,
                  "JPEG SOF 应解析出 512x256");
            check(!extentOf("broken.png").has_value(), "截断的 PNG 应解析失败");
            check(!extentOf("garbage.bin").has_value(), "非图片文件应解析失败");
            check(!extentOf("does_not_exist.png").has_value(), "不存在的文件应解析失败");
            // 记忆化：同路径第二次直接命中缓存（结果一致即可，功能上等价）。
            const auto again = extentOf("pic.png");
            check(again.has_value() && again->width == 240, "记忆化后结果应保持一致");
        }

        // ② 纯图行判定（口径对齐 ZCode 正则）
        {
            const std::string kImgDoc =
                "![示例](pic.png)\n"
                "\n"
                "文字 ![示例](pic.png) 混排\n"
                "![尾随](pic.png) 尾\n"
                "![](pic.png)\n"
                "# ![标题里](pic.png)\n";
            const neo::LpPlan plan = neo::buildLpPlan(kImgDoc);
            // 6 行正文 + 1 个换行收尾的空行 = 7。
            check(plan.lines.size() == 7, "纯图样例应有 7 行，实际 " + std::to_string(plan.lines.size()));
            if (plan.lines.size() >= 6) {
                check(plan.lines[0].pureImageSrc == "pic.png", "独占一行的图片应判定为纯图行");
                check(plan.lines[2].pureImageSrc.empty(), "行首有文字的混排不应算纯图行");
                check(plan.lines[3].pureImageSrc.empty(), "行尾有文字的混排不应算纯图行");
                check(plan.lines[4].pureImageSrc == "pic.png", "空 alt `![]()` 也应算纯图行");
                check(plan.lines[5].pureImageSrc.empty(), "标题里的图片不应算纯图行");
            }
            // src 提取的两种合法写法。
            const neo::LpPlan anglePlan = neo::buildLpPlan("![a](<dir/pic.png> \"标题\")\n");
            check(!anglePlan.lines.empty() && anglePlan.lines[0].pureImageSrc == "dir/pic.png",
                  "尖括号 src 应去掉尖括号与 title");
            const neo::LpPlan titlePlan = neo::buildLpPlan("![a](pic.png \"标题\")\n");
            check(!titlePlan.lines.empty() && titlePlan.lines[0].pureImageSrc == "pic.png",
                  "带 title 的 src 应只取到空白前");
        }

        // ③ 装饰落槽：可解析的本地图 → 图片槽 + 整行洞 + 行高；缺图/头坏 → 失败态占位盒；
        //    远程 → 维持文本；超大图 → 夹进 [448,360] 盒；光标进入该块 → 槽清空回到源码。
        {
            const std::string kPlaceDoc =
                "![示例](pic.png)\n"
                "\n"
                "![缺](nope.png)\n"
                "\n"
                "![坏](broken.png)\n"
                "\n"
                "![远](https://x.y/z.png)\n"
                "\n"
                "![大](big.png)\n"
                "\n"
                "正文收尾\n";
            const neo::LpPlan plan = neo::buildLpPlan(kPlaceDoc);
            const int tailLine = plan.lineIndexFor(static_cast<int>(kPlaceDoc.size()) - 1);
            std::vector<LineDecoration> table;
            neo::lp::buildDecorations(plan, style, kPlaceDoc.size() - 2, table, "", dir);

            check(table.size() == plan.lines.size(), "图片样例：装饰表行数与计划一致");
            if (table.size() >= 9) {
                const LineDecoration& picLine = table[0];
                check(!picLine.imagePath.empty(), "本地可解析的纯图行应有图片槽");
                check(fs::exists(picLine.imagePath) &&
                          fs::equivalent(fs::path(picLine.imagePath), tempDir / "pic.png"),
                      "图片槽路径应解析到文档目录下的 pic.png，实际 " + picLine.imagePath);
                check(std::fabs(picLine.imageWidth - 240.0f) < 0.01f &&
                          std::fabs(picLine.imageHeight - 135.0f) < 0.01f,
                      "小图不放大，宽高应保持 240x135");
                check(std::fabs(picLine.lineHeight - (135.0f + neo::lp::kImageVPad)) < 0.01f,
                      "图片行高应为图高+留白");
                bool holeCoversLine = false;
                for (const LineHole& hole : picLine.holes) {
                    if (hole.beg <= plan.lines[0].srcBeg && hole.end >= plan.lines[0].srcEnd) {
                        holeCoversLine = true;
                    }
                }
                check(holeCoversLine, "图片行的源文应整体藏进洞里");

                // 失败态占位盒（S3f 批次 F）：路径缺失与头解析失败同规格 224×176。
                const LineDecoration& missLine = table[2];
                check(missLine.imagePath.empty() && missLine.imageFailed,
                      "缺图的纯图行应换成失败态占位盒（无图片槽）");
                check(std::fabs(missLine.imageWidth - neo::lp::kImageFailWidth) < 0.01f &&
                          std::fabs(missLine.imageHeight - neo::lp::kImageFailHeight) < 0.01f,
                      "占位盒应为 ZCode md 规格 224x176");
                check(std::fabs(missLine.lineHeight -
                                (neo::lp::kImageFailHeight + neo::lp::kImageVPad)) < 0.01f,
                      "占位盒行高应为盒高+留白");
                check(missLine.imageFailText == "nope.png", "占位盒应显示缺失的路径");
                bool missHoleCovers = false;
                for (const LineHole& hole : missLine.holes) {
                    if (hole.beg <= plan.lines[2].srcBeg && hole.end >= plan.lines[2].srcEnd) {
                        missHoleCovers = true;
                    }
                }
                check(missHoleCovers, "占位盒行的源文应整体藏进洞里");
                const LineDecoration& brokenLine = table[4];
                check(brokenLine.imagePath.empty() && brokenLine.imageFailed &&
                          brokenLine.imageFailText == "broken.png",
                      "头解析失败的本地文件也应有占位盒");

                check(table[6].imagePath.empty() && !table[6].imageFailed &&
                          std::fabs(table[6].lineHeight - style.bodyLineHeight) < 0.01f,
                      "远程图片 v1 维持文本占位（无占位盒、行高不变）");

                check(!table[8].imagePath.empty(), "超大图仍应生成图片槽");
                check(std::fabs(table[8].imageWidth - neo::lp::kImageMaxWidth) < 0.01f,
                      "超宽图应夹到 448，实际 " + std::to_string(table[8].imageWidth));
                check(std::fabs(table[8].imageHeight - 224.0f) < 0.01f,
                      "10000x5000 等比缩放后应为 448x224，实际 " +
                          std::to_string(table[8].imageHeight));
            }

            // 长路径截断：保留尾部、按 UTF-8 边界、以省略号开头。
            {
                const std::string longName = "一个相当长的中文子目录名/另一个同样很长的子目录/最终图片.png";
                const std::string kLongDoc = "![长](" + longName + ")\n";
                const neo::LpPlan longPlan = neo::buildLpPlan(kLongDoc);
                std::vector<LineDecoration> longTable;
                // 光标放文档末尾（末尾空行）：-1 会被 lineIndexFor 钳到第 0 行，
                // 图片行成了活动块、占位盒被抑制 —— 那是另一条已验证的路径。
                neo::lp::buildDecorations(longPlan, style, static_cast<int>(kLongDoc.size()),
                                          longTable, "", dir);
                check(!longTable.empty() && longTable[0].imageFailed,
                      "长路径缺图也应有占位盒");
                if (!longTable.empty() && longTable[0].imageFailed) {
                    const std::string& shown = longTable[0].imageFailText;
                    check(shown.size() < longName.size(), "长路径应被截断");
                    check(shown.rfind("最终图片.png") == shown.size() - std::strlen("最终图片.png"),
                          "截断应保留文件名尾部，实际 " + shown);
                    check(shown.front() == '\xe2', "截断应以省略号（…，U+2026）开头");
                }
            }

            // 光标进入图片行所属块 → 活动块回到源码：图片槽/占位盒都必须清空。
            std::vector<LineDecoration> activeTable;
            neo::lp::buildDecorations(plan, style, plan.lines[0].srcBeg + 1, activeTable, "", dir);
            check(activeTable.size() == 1 || !activeTable.empty(),
                  "活动块样例应产出装饰表");
            if (!activeTable.empty()) {
                check(activeTable[0].imagePath.empty() && !activeTable[0].imageFailed,
                      "光标进入图片块后应回到源码（图片槽与占位盒清空）");
            }
            (void) tailLine;
        }

        fs::remove_all(tempDir, error);
    }

    // ── ⑭ S3f 批次 C：章节表 + 折叠 hidden ──
    {
        // 行：0 `# 甲` /1空 /2段落一 /3 `## 乙` /4段落二 /5空 /6 `# 丙` /7段落三 /8收尾空行
        const std::string kFoldDoc =
            "# 甲\n"
            "\n"
            "段落一\n"
            "## 乙\n"
            "段落二\n"
            "\n"
            "# 丙\n"
            "段落三\n";
        const neo::LpPlan plan = neo::buildLpPlan(kFoldDoc);
        check(plan.lines.size() == 9, "折叠样例应有 9 行，实际 " + std::to_string(plan.lines.size()));
        if (plan.lines.size() >= 8) {
            // 章节终点：甲→下一个同级(丙)=6；乙→下一个 ≤2 级也是 丙=6；丙→文档末尾。
            check(plan.lines[0].kind == neo::LpKind::Heading && plan.lines[0].sectionEndLine == 6,
                  "H1 甲的章节终点应是第 6 行（丙），实际 " + std::to_string(plan.lines[0].sectionEndLine));
            check(plan.lines[3].kind == neo::LpKind::Heading && plan.lines[3].sectionEndLine == 6,
                  "H2 乙的章节终点应是第 6 行（丙），实际 " + std::to_string(plan.lines[3].sectionEndLine));
            check(plan.lines[6].sectionEndLine == static_cast<int>(plan.lines.size()),
                  "文档末尾的标题终点应是行数");
            // 最近上方标题 + 父链
            check(plan.lines[2].sectionHeadingLine == 0, "段落一属于甲");
            check(plan.lines[3].sectionHeadingLine == 0, "H2 乙（标题行自身）记的是父标题甲");
            check(plan.lines[4].sectionHeadingLine == 3, "段落二属于乙");
            check(plan.lines[7].sectionHeadingLine == 6, "段落三属于丙");
            check(plan.lines[0].sectionHeadingLine == -1, "首个标题上方无标题");
            check(plan.lines[3].parentHeadingLine == 0, "乙的父标题是甲");
            check(plan.lines[6].parentHeadingLine == -1, "丙是根标题");
            // 折叠目标：标题行取自己，体内行取包住它的标题
            check(plan.foldHeadingBegFor(plan.lines[0].srcBeg) == plan.lines[0].srcBeg,
                  "光标在甲标题行 → 折叠目标是甲");
            check(plan.foldHeadingBegFor(plan.lines[2].srcBeg + 1) == plan.lines[0].srcBeg,
                  "光标在段落一 → 折叠目标是甲");
            check(plan.foldHeadingBegFor(plan.lines[4].srcBeg + 1) == plan.lines[3].srcBeg,
                  "光标在段落二 → 折叠目标是乙（内层）");
            check(plan.foldHeadingBegFor(plan.lines[3].srcBeg + 2) == plan.lines[3].srcBeg,
                  "光标在乙标题行 → 折叠目标是乙");
        }

        // setext 配对：文本行 + 下划线行是一个标题，折叠目标必须回到文本行字节。
        const std::string kSetextFold =
            "setext 标题\n"
            "==========\n"
            "正文\n"
            "# 后续\n";
        const neo::LpPlan setextPlan = neo::buildLpPlan(kSetextFold);
        check(setextPlan.lines.size() >= 4, "setext 样例至少 4 行");
        if (setextPlan.lines.size() >= 4) {
            check(setextPlan.lines[0].kind == neo::LpKind::Heading && setextPlan.lines[0].setext,
                  "第 0 行应是 setext 标题");
            check(setextPlan.lines[1].kind == neo::LpKind::Heading && setextPlan.lines[1].setext,
                  "第 1 行应是 setext 下划线（同属标题）");
            check(setextPlan.lines[1].sectionHeadingLine == 0,
                  "下划线行归到配对文本行");
            check(setextPlan.lines[0].sectionEndLine == 3,
                  "setext 标题终点应跳过自己的下划线、到 # 后续，实际 " +
                      std::to_string(setextPlan.lines[0].sectionEndLine));
            check(setextPlan.foldHeadingBegFor(setextPlan.lines[1].srcBeg) ==
                      setextPlan.lines[0].srcBeg,
                  "光标在下划线行 → 折叠目标是配对文本行");
            check(setextPlan.lines[2].sectionHeadingLine == 0, "正文属于 setext 标题");
        }

        // hidden 装饰：折叠甲 → 甲的章节体（含内层标题乙）整段隐藏，但章节体的
        // 第一条非空行（段落一）改画"预览横条"（S3f 批次 G：可见 + 弱化 + 左条），
        // 甲/丙/段落三可见。
        std::set<int> folded = {plan.lines[0].srcBeg};
        std::vector<LineDecoration> table;
        // 光标放在丙（折叠区外），避免触发"光标行强制可见"干扰被测行。
        neo::lp::buildDecorations(plan, style, plan.lines[6].srcBeg, table, "", "", &folded);
        check(table.size() == plan.lines.size(), "折叠样例：装饰表行数与计划一致");
        if (table.size() >= 8) {
            // 行2 = 预览横条：可见（hidden=0）但 hiddenByFold 仍为 1（自愈把它当折叠内容）。
            const std::string expectHidden = "01011100";  // 行0..7：0=可见 1=隐藏
            std::string actual;
            for (std::size_t i = 0; i < 8 && i < table.size(); ++i) {
                actual += table[i].hidden ? '1' : '0';
            }
            check(actual == expectHidden,
                  "折叠甲后 hidden 图案应为 " + expectHidden + "（段落一=预览条），实际 " + actual);
            check(!table[2].hidden && table[2].hiddenByFold,
                  "预览行应可见但保留 hiddenByFold（光标落上去自愈展开）");
            check(table[2].textColor.a > 0.0f && table[2].textColor.r == style.muted.r,
                  "预览行文字应弱化");
            check(table[2].box.barColor.a > 0.0f && std::fabs(table[2].box.barWidth - 2.0f) < 0.01f,
                  "预览行应带 2px 左条");
            check(table[1].hidden && table[1].box.barColor.a == 0.0f,
                  "预览行之前的空行应照常隐藏且不带左条");
        }

        // 只折叠乙：段落二 = 乙章节体的第一条非空行 → 预览横条（可见、hiddenByFold），
        // 其后的空行照藏；甲的段落一与乙标题自身可见。
        folded = {plan.lines[3].srcBeg};
        neo::lp::buildDecorations(plan, style, plan.lines[7].srcBeg, table, "", "", &folded);
        if (table.size() >= 8) {
            std::string actual;
            for (std::size_t i = 0; i < 8 && i < table.size(); ++i) {
                actual += table[i].hidden ? '1' : '0';
            }
            check(actual == "00000100", "只折叠乙的 hidden 图案应为 00000100（段落二=预览条），实际 " + actual);
            check(!table[4].hidden && table[4].hiddenByFold, "段落二应成为乙的预览横条");
        }

        // 嵌套折叠：甲、乙都折叠 → 全文档只出**一条**预览（甲的章节体首行，
        // 按最外层被折叠祖先分配），乙的章节体不再出自己的预览条。
        folded = {plan.lines[0].srcBeg, plan.lines[3].srcBeg};
        neo::lp::buildDecorations(plan, style, plan.lines[6].srcBeg, table, "", "", &folded);
        if (table.size() >= 8) {
            std::string actual;
            for (std::size_t i = 0; i < 8 && i < table.size(); ++i) {
                actual += table[i].hidden ? '1' : '0';
            }
            check(actual == "01011100",
                  "嵌套折叠的 hidden 图案应与单折甲相同（只有甲的预览条），实际 " + actual);
        }

        // 祖先规则 + 光标行强制可见：折叠甲、光标停在段落二（体内）→
        // 段落一 = 预览横条（可见），段落二被"光标行强制可见"放开（不变量），
        // 其后同章节行照常隐藏。
        folded = {plan.lines[0].srcBeg};
        neo::lp::buildDecorations(plan, style, plan.lines[4].srcBeg, table, "", "", &folded);
        if (table.size() >= 8) {
            check(!table[2].hidden && table[2].hiddenByFold, "段落一应是预览横条（可见）");
            check(!table[4].hidden, "光标所在行必须强制可见（不变量兜底）");
            check(table[4].hiddenByFold, "强制可见行的 hiddenByFold 仍在（自愈识别）");
            check(table[5].hidden, "光标行之后的同章节行仍应隐藏");
        }

        // 空集合 / 不设置 → 一个都不藏（回归零影响）。
        neo::lp::buildDecorations(plan, style, 0, table, "", "", nullptr);
        int visibleAll = 0;
        for (const LineDecoration& decoration : table) {
            visibleAll += decoration.hidden ? 0 : 1;
        }
        check(visibleAll == static_cast<int>(table.size()), "不折叠时所有行都应可见");
    }

    // ── ⑮ S3f 批次 D：表格（cells / 表分组 / 分隔行 / 永远隐藏的管道空隙）──
    {
        const neo::LpPlan tablePlan = neo::buildLpPlan(kTableDoc);
        // 4 行表格 + 末尾换行后那条空行 = 5 条计划行。
        check(tablePlan.lines.size() == 5, "表格样例应为 5 条计划行（4 行表格 + 尾空行），实际 " +
              std::to_string(tablePlan.lines.size()));
        if (tablePlan.lines.size() >= 4) {
            check(tablePlan.lines[0].kind == neo::LpKind::Table, "第 0 行应识别为表格行");
            check(tablePlan.lines[0].tableHeaderRow, "第 0 行应标为表头行");
            check(tablePlan.lines[1].tableSeparator, "第 1 行应标为分隔行");
            check(!tablePlan.lines[2].tableSeparator, "第 2 行不该是分隔行");
            check(tablePlan.lines[0].tableId >= 0 && tablePlan.lines[0].tableId == tablePlan.lines[3].tableId,
                  "同一张表的所有行应共享 tableId");
            check(tablePlan.lines[0].cells.size() == 2, "表头应有两格，实际 " +
                  std::to_string(tablePlan.lines[0].cells.size()));
            if (tablePlan.lines[0].cells.size() == 2) {
                check(slice(kTableDoc, tablePlan.lines[0].cells[0].beg, tablePlan.lines[0].cells[0].end) == "列甲",
                      "第 1 格内容应为「列甲」");
            }
            // 反引号里的管道不拆格（md4c 给的是真格区间，不是按管道切）。
            const neo::LpLine& lastRow = tablePlan.lines[3];
            check(lastRow.cells.size() == 2, "末行应有两格（`x|y` 不能被拆开），实际 " +
                  std::to_string(lastRow.cells.size()));
            if (lastRow.cells.size() == 2) {
                check(slice(kTableDoc, lastRow.cells[0].beg, lastRow.cells[0].end) == "`x|y`",
                      "第 1 格应含反引号与其中的管道");
            }
        }

        std::vector<LineDecoration> table;
        // ① 管道与两侧空白：**任何光标位置**都必须藏在洞里（列对齐的支点）。
        int gapLeaks = 0;
        int gapCount = 0;
        const std::vector<int> cursors = {0, tablePlan.lines[2].srcBeg, tablePlan.lines[3].srcBeg + 1};
        for (const int cursor : cursors) {
            neo::lp::buildDecorations(tablePlan, style, cursor, table);
            for (std::size_t i = 0; i < table.size() && i < tablePlan.lines.size(); ++i) {
                const neo::LpLine& line = tablePlan.lines[i];
                if (line.tableId < 0 || line.tableSeparator) {
                    continue;
                }
                int position = line.srcBeg;
                for (const neo::LpRange& cell : line.cells) {
                    for (int byte = position; byte < cell.beg; ++byte) {
                        ++gapCount;
                        bool hidden = false;
                        for (const components::input_detail::LineHole& hole : table[i].holes) {
                            hidden = hidden || (byte >= hole.beg && byte < hole.end);
                        }
                        if (!hidden) {
                            ++gapLeaks;
                        }
                    }
                    position = std::max(position, cell.end);
                }
                for (int byte = position; byte < line.srcEnd; ++byte) {
                    ++gapCount;
                    bool hidden = false;
                    for (const components::input_detail::LineHole& hole : table[i].holes) {
                        hidden = hidden || (byte >= hole.beg && byte < hole.end);
                    }
                    if (!hidden) {
                        ++gapLeaks;
                    }
                }
            }
        }
        check(gapCount > 0 && gapLeaks == 0,
              "管道/空白在任意光标位置都必须隐藏，漏出 " + std::to_string(gapLeaks) + " / " +
              std::to_string(gapCount) + " 字节");

        // ② 分隔行：整行藏空 + 压成细横条 + 画成"表头下边框"。
        neo::lp::buildDecorations(tablePlan, style, 0, table);
        const neo::LpLine& separator = tablePlan.lines[1];
        check(table.size() >= 2 && !table[1].holes.empty() && table[1].cells.empty(),
              "分隔行应整行隐藏且没有格");
        check(holesText(kTableDoc, table, 1) == slice(kTableDoc, separator.srcBeg, separator.srcEnd),
              "分隔行的洞应覆盖整行内容");
        check(std::fabs(table[1].lineHeight - neo::lp::kTableSeparatorHeight) < 0.01f,
              "分隔行应压成 " + std::to_string(neo::lp::kTableSeparatorHeight) + "px 横条，实际 " +
              std::to_string(table[1].lineHeight));
        check(table[1].box.background.a > 0.0f, "分隔行应画成一条边框色横条");
        check(table[1].box.gridColor.a > 0.0f, "分隔行也要带网格色（竖线要穿过去）");

        // ③ 表格行**没有底色**（R3 对齐 Obsidian：纯 1px 网格 + 表头透明），
        //    行盒 = 正文 1.3 行高 + 单元格上下内边距，文字用 textShiftY 跟着下移。
        check(table[0].box.background.a <= 0.0f, "表头行不应有底色（Obsidian 表头 transparent）");
        check(table[2].box.background.a <= 0.0f, "正文行不应有底色");
        check(table[0].box.gridColor.a > 0.0f, "表格行应带网格色");
        {
            const float paddingY = style.bodySize * neo::lp::kTableRowPaddingYEm;
            check(std::fabs(table[2].lineHeight - (style.bodySize * 1.3f + paddingY * 2.0f)) < 0.01f,
                  "表格行高应为单元格行高 + 上下内边距，实际 " + std::to_string(table[2].lineHeight));
            check(std::fabs(table[2].textShiftY - paddingY) < 0.01f,
                  "表格行文字应下移一个内边距，实际 " + std::to_string(table[2].textShiftY));
        }
        check(table[0].textColor.a > 0.0f && table[0].textColor.r == style.text.r &&
                  table[0].textColor.a == style.text.a,
              "表头行文字应为正文字色");
        {
            bool headerWeight = false;
            for (const auto& run : table[0].runs) {
                if (run.style.weight == 600) {
                    headerWeight = true;
                }
            }
            check(headerWeight, "表头行应有 600 字重段");
        }
        check(table[0].cells.size() == 2 && table[3].cells.size() == 2, "表格行应把 cells 交给组件");
        check(table[0].tableId == table[3].tableId && table[0].tableId >= 0, "表格行应带上 tableId");

        // ④ 紧挨着管道的行内标记要能露出来（孔洞不合并、不与空隙粘连）：
        //    光标进 `` `x|y` `` 这段 → 反引号必须露，但它前面的空隙照旧藏。
        const int codeBeg = tablePlan.lines[3].cells.empty() ? -1 : tablePlan.lines[3].cells[0].beg;
        if (codeBeg >= 0) {
            neo::lp::buildDecorations(tablePlan, style, codeBeg + 1, table);
            int backtickHidden = 0;
            for (const components::input_detail::LineHole& hole : table[3].holes) {
                if (codeBeg >= hole.beg && codeBeg < hole.end) {
                    ++backtickHidden;
                }
            }
            check(backtickHidden == 0, "光标在 `` `x|y` `` 里时开反引号必须露出来");
            bool gapStillHidden = false;
            for (const components::input_detail::LineHole& hole : table[3].holes) {
                if (codeBeg - 1 >= hole.beg && codeBeg - 1 < hole.end) {
                    gapStillHidden = true;
                }
            }
            check(gapStillHidden, "反引号前的管道空隙仍必须隐藏");
        }

        // ⑤ 真实路径的表格点击：lp_plan 的 UTF-8 cell range → lp_decorations 的 holes/
        //    cells → InputLayout 的 pointerHit → 在命中字节插入。合成 LineDecoration
        //    容易漏掉 md4c 的实际 cell 起止与行内隐藏标记，所以在这里串起编辑器同款链路。
        {
            using InputModel = components::input_detail::InputModel;
            const std::string pointerDoc =
                "| 表头甲 | 表头乙 | 表头丙 |\n"
                "| --- | --- | --- |\n"
                "| 苹果梨 | **香蕉葡萄** | 西瓜柚子 |\n";
            const neo::LpPlan pointerPlan = neo::buildLpPlan(pointerDoc);
            std::vector<LineDecoration> pointerDecorations;
            neo::lp::buildDecorations(pointerPlan, style, 0, pointerDecorations,
                                      style.fontFamily, {}, nullptr, pointerDoc);
            check(pointerPlan.lines.size() >= 3 && pointerDecorations.size() >= 3 &&
                      pointerDecorations[2].cells.size() == 3,
                  "real Markdown table fixture should expose three UTF-8 body cells");
            if (pointerPlan.lines.size() >= 3 && pointerDecorations.size() >= 3 &&
                pointerDecorations[2].cells.size() == 3) {
                InputModel::InputState pointerState;
                pointerState.text = pointerDoc;
                pointerState.textRevision = 1;
                constexpr float pointerWidth = 480.0f;
                constexpr float pointerInset = 8.0f;
                const auto pointerLayout = InputModel::InputLayout::build(
                    pointerState, pointerWidth, 300.0f, pointerWidth,
                    pointerInset, pointerInset, pointerInset, style.bodySize,
                    style.fontFamily, style.bodySize, true, &pointerDecorations);
                const core::Rect pointerBounds{35.0f, 50.0f, pointerWidth, 300.0f};
                const auto* pointerColumns = pointerLayout.tableColumnsFor(
                    pointerDecorations[2].tableId);
                const auto& pointerCells = pointerDecorations[2].cells;
                if (pointerColumns == nullptr || pointerColumns->count() != 3) {
                    check(false, "real Markdown table layout should have three column geometries");
                } else {
                    const int visualRow = 2;
                    const float rowY = pointerBounds.y + pointerInset +
                        pointerLayout.geometryTable().top(visualRow) + 2.0f;
                    for (int column = 1; column < 3; ++column) {
                        const auto& cell = pointerCells[static_cast<std::size_t>(column)];
                        const float x = pointerBounds.x + pointerInset +
                            pointerColumns->x[static_cast<std::size_t>(column)] + 0.5f;
                        const auto hit = pointerLayout.pointerHit(
                            x, rowY, pointerBounds, pointerWidth, pointerInset);
                        const bool inCell = hit.byteIndex >= cell.beg && hit.byteIndex <= cell.end;
                        check(inCell, "real table click at column " + std::to_string(column + 1) +
                                          " start should resolve inside its md4c cell range (got " +
                                          std::to_string(hit.byteIndex) + ")");
                        if (inCell) {
                            std::string edited = pointerDoc;
                            edited.insert(static_cast<std::size_t>(hit.byteIndex), "X");
                            const auto& first = pointerCells[0];
                            const std::string firstBefore = pointerDoc.substr(
                                static_cast<std::size_t>(first.beg),
                                static_cast<std::size_t>(first.end - first.beg));
                            const std::string firstAfter = edited.substr(
                                static_cast<std::size_t>(first.beg), firstBefore.size());
                            const std::string targetAfter = edited.substr(
                                static_cast<std::size_t>(cell.beg),
                                static_cast<std::size_t>(cell.end - cell.beg + 1));
                            check(firstAfter == firstBefore && targetAfter.find('X') != std::string::npos,
                                  "insertion at real column " + std::to_string(column + 1) +
                                      " caret must edit that cell, not the first cell");
                        }
                    }
                    const auto& first = pointerCells[0];
                    const auto& second = pointerCells[1];
                    const float sharedBoundaryX = pointerColumns->x[1];
                    const float sharedBoundaryY = rowY;
                    const auto boundaryHit = pointerLayout.pointerHit(
                        pointerBounds.x + pointerInset + sharedBoundaryX + 0.5f,
                        sharedBoundaryY, pointerBounds, pointerWidth, pointerInset);
                    check(boundaryHit.byteIndex >= second.beg && boundaryHit.byteIndex <= second.end,
                          "real hidden pipe boundary click must belong to the next cell");
                    const auto& mappedLine = pointerLayout.lineList()[visualRow];
                    const float projectedFirstEndX = components::input_detail::caretXForDocOffset(
                        mappedLine.metrics, mappedLine.holes, mappedLine.start, first.end);
                    const float projectedSecondBegX = components::input_detail::caretXForDocOffset(
                        mappedLine.metrics, mappedLine.holes, mappedLine.start, second.beg);
                    const float mappedFirstEndX = InputModel::caretXInLine(mappedLine, first.end);
                    const float mappedSecondBegX = InputModel::caretXInLine(mappedLine, second.beg);
                    check(std::fabs(projectedFirstEndX - projectedSecondBegX) < 0.01f,
                          "legacy projected metrics should demonstrate the shared boundary ambiguity");
                    check(std::fabs(mappedFirstEndX - mappedSecondBegX) > 0.01f,
                          "direct document caret stops must preserve both sides of a hidden pipe");
                }
            }
        }

        // ⑥ 实机复现：第二列居中、第三列右对齐时，`b` 后方的留白点击应落在 b 之后。
        //    第 2 格末尾与第 3 格开头投影到同一 byte stop；当前聚合 metrics 会被第 3 格
        //    的 right-aligned x 覆盖，字节范围 clamp 无法修复同一格内部的错位。
        {
            using InputModel = components::input_detail::InputModel;
            const std::string pointerDoc =
                "# Table check\n\n"
                "| 第一列 | 第二列 | 第三列 | 第四列 |\n"
                "| --- | :---: | ---: | --- |\n"
                "| 甲乙 | 丙丁 | 戊己 | 庚辛 |\n"
                "| a | b | c | d |\n"
                "| 第一个很长很长内容 | 第二个很长内容 | 第三个内容 | 第四个内容 |\n";
            const neo::LpPlan pointerPlan = neo::buildLpPlan(pointerDoc);
            std::vector<LineDecoration> pointerDecorations;
            neo::lp::buildDecorations(pointerPlan, style, 0, pointerDecorations,
                                      style.fontFamily, {}, nullptr, pointerDoc);
            const std::size_t bLine = pointerDoc.find("| a | b | c | d |");
            const std::size_t bPos = pointerDoc.find('b', bLine);
            int sourceRow = -1;
            for (std::size_t i = 0; i < pointerPlan.lines.size(); ++i) {
                if (pointerPlan.lines[i].srcBeg <= static_cast<int>(bPos) &&
                    static_cast<int>(bPos) < pointerPlan.lines[i].srcEnd) {
                    sourceRow = static_cast<int>(i);
                    break;
                }
            }
            check(sourceRow >= 0 && sourceRow < static_cast<int>(pointerDecorations.size()) &&
                      pointerDecorations[static_cast<std::size_t>(sourceRow)].cells.size() == 4,
                  "aligned four-column repro should expose its real md4c cells");
            if (sourceRow >= 0 && sourceRow < static_cast<int>(pointerDecorations.size()) &&
                pointerDecorations[static_cast<std::size_t>(sourceRow)].cells.size() == 4) {
                InputModel::InputState pointerState;
                pointerState.text = pointerDoc;
                pointerState.textRevision = 1;
                constexpr float pointerWidth = 760.0f;
                constexpr float pointerInset = 8.0f;
                const auto pointerLayout = InputModel::InputLayout::build(
                    pointerState, pointerWidth, 500.0f, pointerWidth,
                    pointerInset, pointerInset, pointerInset, style.bodySize,
                    style.fontFamily, style.bodySize, true, &pointerDecorations);
                const int visualRow = pointerLayout.lineIndexFor(static_cast<int>(bPos));
                const auto& line = pointerLayout.lineList()[static_cast<std::size_t>(visualRow)];
                const auto& cells = pointerDecorations[static_cast<std::size_t>(sourceRow)].cells;
                const auto* columns = pointerLayout.tableColumnsFor(
                    pointerDecorations[static_cast<std::size_t>(sourceRow)].tableId);
                const auto run = std::find_if(line.runs.begin(), line.runs.end(),
                    [bPos](const components::input_detail::TextRun& candidate) {
                        return candidate.beg <= static_cast<int>(bPos) &&
                               static_cast<int>(bPos) < candidate.end;
                    });
                if (columns == nullptr || columns->count() != 4 || run == line.runs.end()) {
                    check(false, "aligned four-column repro should retain table geometry and b run");
                } else {
                    const auto& second = cells[1];
                    const float clickLocalX = std::min(
                        run->x + run->width + 24.0f,
                        columns->x[1] + columns->width[1] - 0.5f);
                    check(clickLocalX > run->x + run->width &&
                              clickLocalX < columns->x[2],
                          "repro click should be in second-column padding before column three");
                    const core::Rect bounds{35.0f, 50.0f, pointerWidth, 500.0f};
                    const float y = bounds.y + pointerInset +
                        pointerLayout.geometryTable().top(visualRow) + 2.0f;
                    const auto hit = pointerLayout.pointerHit(
                        bounds.x + pointerInset + clickLocalX, y, bounds,
                        pointerWidth, pointerInset);
                    const int expected = second.end;
                    if (hit.byteIndex != expected) {
                        std::printf("  !! aligned second-cell tail hit returned %d, expected %d\n",
                                    hit.byteIndex, expected);
                    }
                    check(hit.byteIndex == expected,
                          "clicking right padding after b must resolve to b's end byte");
                    std::string edited = pointerDoc;
                    edited.insert(static_cast<std::size_t>(hit.byteIndex), "X");
                    check(edited.find("| a | bX | c | d |") != std::string::npos,
                          "typing after the aligned second-cell padding click must produce bX");
                }
            }
        }
    }

    // ── ⑯ S3f 批次 E：表格对齐（分隔行 `:---:` → MD_ALIGN 配对 → 组件透传）──
    {
        const std::string alignDoc =
            "| 甲 | 乙 |\n"
            "|:---:|---:|\n"
            "| 居中 | 靠右 |\n";
        const neo::LpPlan alignPlan = neo::buildLpPlan(alignDoc);
        check(alignPlan.lines.size() >= 3, "对齐样例至少 3 条计划行，实际 " +
              std::to_string(alignPlan.lines.size()));
        if (alignPlan.lines.size() >= 3) {
            // 0 default/1 left/2 center/3 right：`:---:` → center、`---:` → right。
            const neo::LpLine& header = alignPlan.lines[0];
            const neo::LpLine& body = alignPlan.lines[2];
            check(header.cellAligns.size() == 2 && header.cellAligns[0] == 2 && header.cellAligns[1] == 3,
                  "表头两格应对齐为 居中/靠右，实际 " + std::to_string(header.cellAligns.size()) + " 格");
            check(body.cellAligns.size() == 2 && body.cellAligns[0] == 2 && body.cellAligns[1] == 3,
                  "正文两格继承同样的列对齐");
            check(alignPlan.lines[1].cellAligns.empty(),
                  "分隔行不产生 cell，也就没有对齐值");
        }
        // 没写对齐的表（kTableDoc）：全 0（default），装饰层照常透传。
        const neo::LpPlan plainPlan = neo::buildLpPlan(kTableDoc);
        if (!plainPlan.lines.empty() && !plainPlan.lines[0].cellAligns.empty()) {
            bool allDefault = true;
            for (const unsigned char align : plainPlan.lines[0].cellAligns) {
                allDefault = allDefault && align == 0;
            }
            check(allDefault, "无对齐标记的列应全为 default(0)");
        }
        // 装饰层透传：cells 的 align 原样到达组件。
        std::vector<LineDecoration> alignTable;
        neo::lp::buildDecorations(alignPlan, style, 0, alignTable);
        check(alignTable.size() >= 3 && alignTable[0].cells.size() == 2 &&
                  alignTable[0].cells[0].align == 2 && alignTable[0].cells[1].align == 3,
              "装饰层应把表头的 align 透传给组件");
        check(alignTable[2].cells.size() == 2 &&
                  alignTable[2].cells[0].align == 2 && alignTable[2].cells[1].align == 3,
              "装饰层应把正文的 align 透传给组件");
        // 相等语义：align 参与比较（缓存失效判断的依据）。
        components::input_detail::LineCell withAlign{alignTable[0].cells[0]};
        withAlign.align = 3;
        check(!(withAlign == alignTable[0].cells[0]), "LineCell 的 align 必须参与相等语义");
    }

    // ── 代码语法 token 着色（2026-09-25，规格表 §4）────────────────────────
    {
        const neo::EditorColors& light = neo::editorColors(neo::ThemeMode::Light);
        std::vector<LineDecoration> table;
        neo::lp::buildDecorations(plan, style, 0, table, "", {}, nullptr, kDoc, &light);
        const int codeLine = fenceOpenLine + 1;  // "int a = 1;"
        bool hasKeyword = false;
        bool hasNumber = false;
        bool hasOperator = false;
        for (const LineRun& run : table[static_cast<std::size_t>(codeLine)].runs) {
            const std::string word = slice(kDoc, run.beg, run.end);
            if (word == "int" && sameColor(run.style.color, light.tokenKeyword)) {
                hasKeyword = true;
            }
            if (word == "1" && sameColor(run.style.color, light.tokenNumber)) {
                hasNumber = true;
            }
            if (word == "=" && sameColor(run.style.color, light.tokenOperator)) {
                hasOperator = true;
            }
        }
        check(hasKeyword, "int 应按关键字上色");
        check(hasNumber, "1 应按数字上色");
        check(hasOperator, "= 应按运算符上色");
        // 围栏行本身不上色（它是标记）。
        check(table[static_cast<std::size_t>(fenceOpenLine)].runs.empty(),
              "围栏行不应产生 token 段");
        // 兼容旧签名（不给 text/colors）：代码行不上色、行为与之前一致。
        std::vector<LineDecoration> plain;
        neo::lp::buildDecorations(plan, style, 0, plain);
        check(plain[static_cast<std::size_t>(codeLine)].runs.empty(),
              "未接 token 色时（旧签名）代码行不应产生样式段");
        // 跨行状态：块注释吞到闭合为止；Python 的 # 是行注释、三引号跨行。
        {
            using neo::lp::SyntaxState;
            using neo::lp::TokenKind;
            SyntaxState state;
            std::vector<neo::lp::SyntaxToken> tokens;
            const std::string cl = "/* a\n";
            neo::lp::tokenizeCodeLine(cl, 0, static_cast<int>(cl.size()), "cpp", state, tokens);
            check(state.blockComment, "未闭合块注释应把状态带下行");
            const std::string cl2 = " b */ int";
            tokens.clear();
            neo::lp::tokenizeCodeLine(cl2, 0, static_cast<int>(cl2.size()), "cpp", state, tokens);
            check(!state.blockComment, "闭合后状态应复位");
            check(!tokens.empty() && tokens.front().kind == TokenKind::Comment &&
                      tokens.front().beg == 0 && tokens.front().end == 5,
                  "续行注释应从行首覆盖到 */");
            bool sawKeyword = false;
            for (const neo::lp::SyntaxToken& token : tokens) {
                if (token.kind == TokenKind::Keyword && slice(cl2, token.beg, token.end) == "int") {
                    sawKeyword = true;
                }
            }
            check(sawKeyword, "注释闭合后的 int 应按关键字上色");
            SyntaxState py;
            std::vector<neo::lp::SyntaxToken> pyTokens;
            const std::string py1 = "# 注释\n";
            neo::lp::tokenizeCodeLine(py1, 0, static_cast<int>(py1.size()), "python", py, pyTokens);
            check(!pyTokens.empty() && pyTokens.front().kind == TokenKind::Comment,
                  "Python 的 # 应按行注释处理");
            const std::string py2 = "x = \"\"\"abc\n";
            pyTokens.clear();
            neo::lp::tokenizeCodeLine(py2, 0, static_cast<int>(py2.size()), "python", py, pyTokens);
            check(py.tripleString, "未闭合三引号应把状态带下行");
        }
        // 相等语义：gridColor 参与比较（缓存失效判断的依据）。
        components::input_detail::LineBoxStyle withGrid;
        withGrid.gridColor = core::Color{0.1f, 0.2f, 0.3f, 1.0f};
        check(!(withGrid == components::input_detail::LineBoxStyle{}),
              "LineBoxStyle 的 gridColor 必须参与相等语义");
    }

    // ── 表格网格线的行身份（2026-09-25）────────────────────────────────
    {
        const std::string gridDoc =
            "| a | b |\n"
            "| --- | --- |\n"
            "| 1 | 2 |\n";
        const neo::LpPlan gridPlan = neo::buildLpPlan(gridDoc);
        std::vector<LineDecoration> gridTable;
        neo::lp::buildDecorations(gridPlan, style, 0, gridTable, "", {}, nullptr,
                                  gridDoc, &neo::editorColors(neo::ThemeMode::Light));
        check(gridTable.size() == 4, "表格文档（含末尾空行）应有 4 行装饰");
        check(gridTable[0].tableId >= 0 && gridTable[0].tableId == gridTable[2].tableId,
              "表头与数据行应共享同一个 tableId");
        check(gridTable[1].tableSeparator, "分隔行应带 tableSeparator 标记");
        check(gridTable[0].box.gridColor.a > 0.0f, "表格行应带网格线颜色");
    }

    // ── 右键菜单的文本命令（2026-09-25）────────────────────────────────
    {
        using InputModel = components::input_detail::InputModel;
        // 粗体开关：包住 → 再点一次解开。
        InputModel::InputState bold;
        bold.text = "hello world";
        bold.cursor = 6;
        bold.selectionStart = 6;
        bold.selectionEnd = 11;
        neo::applyInlineFormat(bold, "**");
        check(bold.text == "hello **world**", "粗体应包住选区");
        check(slice(bold.text, bold.selectionStart, bold.selectionEnd) == "world",
              "包裹后选区应保持包住原文字");
        neo::applyInlineFormat(bold, "**");
        check(bold.text == "hello world", "同格式再点一次应解开");
        // 无选区：插入空标记，光标落中间。
        InputModel::InputState empty;
        empty.text = "ab";
        empty.cursor = 1;
        empty.selectionStart = 1;
        empty.selectionEnd = 1;
        neo::applyInlineFormat(empty, "*");
        check(empty.text == "a**b" && empty.cursor == 2, "无选区应插入空标记且光标居中");
        // 清除格式：剥成对标记 + 链接还原。
        InputModel::InputState clearFormat;
        clearFormat.text = "**[文字](a.md)**";
        clearFormat.selectionStart = 0;
        clearFormat.selectionEnd = static_cast<int>(clearFormat.text.size());
        neo::clearInlineFormat(clearFormat);
        check(clearFormat.text == "文字", "清除格式应剥标记并还原链接");
        // 链接：选区转链接，光标落在目标占位处。
        InputModel::InputState link;
        link.text = "词语";
        link.selectionStart = 0;
        link.selectionEnd = static_cast<int>(link.text.size());
        neo::applyLinkFormat(link, "https://");
        check(link.text == "[词语](https://)", "链接应包住选区并落到目标占位");
        // 标题：设置 → 同级再点移除；多行选区逐行生效。
        InputModel::InputState heading;
        heading.text = "标题行\n正文行";
        heading.selectionStart = 1;
        heading.selectionEnd = 1;
        neo::applyLinePrefix(heading, "## ", true);
        check(heading.text == "## 标题行\n正文行", "应给当前行加二级标题");
        neo::applyLinePrefix(heading, "## ", true);
        check(heading.text == "标题行\n正文行", "同级再点一次应移除标题");
        InputModel::InputState multi;
        multi.text = "甲\n乙\n丙";
        multi.selectionStart = 2;  // 从"乙"行首选到"丙"行首，覆盖前两行
        multi.selectionEnd = 4;
        neo::applyLinePrefix(multi, "# ", true);
        check(multi.text == "# 甲\n# 乙\n丙", "多行选区应逐行加前缀");
        // 引用：逐行加 "> "。
        InputModel::InputState quote;
        quote.text = "甲\n乙";
        quote.selectionStart = 0;
        quote.selectionEnd = 0;
        neo::insertBlockTemplate(quote, 3);
        check(quote.text == "> 甲\n乙", "插入引用应给当前行加 > 前缀");
        // 代码块模板：光标落到首行代码处。
        InputModel::InputState codeBlock;
        codeBlock.text = "";
        codeBlock.cursor = 0;
        codeBlock.selectionStart = 0;
        codeBlock.selectionEnd = 0;
        neo::insertBlockTemplate(codeBlock, 1);
        check(codeBlock.text == "```\n\n```\n", "插入代码块应给完整模板");
        check(codeBlock.cursor == 4, "光标应落到首行代码处");
    }

    // Orphan setext underlines at row zero must never read lines[size_t(-1)].
    for (int repeat = 0; repeat < 32; ++repeat) {
        for (const char* text : {"---", "---\nbody\n", "===", "===\nbody\n"}) {
            const auto firstLinePlan = neo::buildLpPlan(text);
            check(!firstLinePlan.lines.empty() && !firstLinePlan.lines[0].setext,
                  "首行没有上一段，不应构造 setext 标题");
            check(firstLinePlan.lines[0].kind == (text[0] == '-' ? neo::LpKind::Divider : neo::LpKind::Text),
                  "首行横线是分隔线，首行等号是普通文本");
        }
    }

    // ── ⑮ T15：章节表第一遍 O(N²) → O(N) 单调栈，逐行对照朴素 oracle ──
    {
        struct SectionCase {
            const char* name;
            const char* doc;
        };
        const std::vector<SectionCase> cases = {
            {"跳级 #→###→##", "# 甲\n### 乙\n## 丙\n正文\n"},
            {"连续同级", "# 甲\n# 乙\n# 丙\n"},
            {"首行标题", "# 首行标题\n正文\n"},
            {"文末标题", "正文\n\n# 文末标题\n"},
            {"标题后无内容", "# 只有标题\n"},
            {"setext 单组配对", "setext 标题\n==========\n正文\n# 后续\n"},
            {"setext 二级 ---", "setext 标题\n---\n正文\n"},
            {"连续 setext 配对", "Title\n---\nBody\n===\n"},
            {"空行使 --- 成为分隔线", "# H1\nTitle\n\n---\nBody\n===\ntail\n"},
            {"setext 层级交替 ===/---", "A\n===\nB\n---\nC\n===\n"},
            {"开头分隔线", "---\n\n正文\n"},
            {"首行等号不能访问上一行", "===\n正文\n"},
            {"只有首行横线", "---"},
            {"只有首行等号", "==="},
            {"连续 --- 分隔线", "---\n---\n---\n正文\n"},
            {"连续 *** 分隔线", "***\n***\n正文\n"},
            {"混合层级收束", "# a\n## b\n### c\n#### d\n### e\n## f\n# g\n"},
            {"标题间夹列表与代码块",
             "# a\n\n- 1\n- 2\n\n```\ncode\n```\n\n## b\n"},
            {"空文档", ""},
            {"只有正文", "just text\n"},
        };

        int badDocs = 0;
        int headingStarts = 0;
        int underlineRows = 0;
        for (const SectionCase& item : cases) {
            const neo::LpPlan p = neo::buildLpPlan(item.doc);
            const std::vector<int> expect = naiveSectionEnds(p);
            check(expect.size() == p.lines.size(),
                  std::string(item.name) + "：oracle 行数应与计划一致");
            bool docOk = true;
            for (std::size_t i = 0; i < p.lines.size() && i < expect.size(); ++i) {
                const neo::LpLine& line = p.lines[i];
                if (line.sectionEndLine != expect[i]) {
                    docOk = false;
                    std::printf("  !! %s：第 %zu 行终点 实际 %d / 旧算法 %d\n", item.name, i,
                                line.sectionEndLine, expect[i]);
                }
                if (isSetextUnderlineRow(p, i)) {
                    ++underlineRows;
                    // 被识别为下划线的行永远不记终点（它属于标题自己）
                    check(line.sectionEndLine == -1,
                          std::string(item.name) + "：下划线行 " + std::to_string(i) +
                              " 的 sectionEndLine 应恒为 -1，实际 " +
                              std::to_string(line.sectionEndLine));
                }
                if (line.kind == neo::LpKind::Heading && !isSetextUnderlineRow(p, i)) {
                    ++headingStarts;
                    // setext 标题的终点永远越过自己那条紧邻下划线
                    if (line.setext && i + 1 < p.lines.size() &&
                        isSetextUnderlineRow(p, i + 1)) {
                        check(line.sectionEndLine >= static_cast<int>(i) + 2,
                              std::string(item.name) + "：setext 标题 " + std::to_string(i) +
                                  " 的终点不应落在自己的下划线行上，实际 " +
                                  std::to_string(line.sectionEndLine));
                    }
                    // 文档末尾的标题：终点 = 行数（"后面没有更高级标题"）
                    if (line.sectionEndLine >= static_cast<int>(p.lines.size())) {
                        check(line.sectionEndLine == static_cast<int>(p.lines.size()),
                              std::string(item.name) + "：终点越过了行数");
                    }
                }
            }
            if (!docOk) {
                ++badDocs;
            }
        }
        check(badDocs == 0,
              "T15：章节终点须与朴素 O(N²) oracle 逐行一致，" + std::to_string(badDocs) +
                  " 个样例不符");
        check(headingStarts >= 25,
              "T15：样例应覆盖足够多的标题起始行，实际 " + std::to_string(headingStarts));
        check(underlineRows >= 6,
              "T15：样例应覆盖足够多的 setext 下划线行，实际 " + std::to_string(underlineRows));

        // 关键样例的显式断言（不依赖 oracle，直接把旧算法的结论写死）
        {
            // 跳级：# 甲 / ### 乙 / ## 丙 / 正文 / 末尾空行 → 甲要扫过 乙、丙到文末，
            // 乙在 ## 丙 收尾，丙到文末。
            const neo::LpPlan p = neo::buildLpPlan("# 甲\n### 乙\n## 丙\n正文\n");
            check(p.lines.size() == 5, "跳级样例应有 5 行");
            if (p.lines.size() == 5) {
                check(p.lines[0].sectionEndLine == 5,
                      "H1 甲的章节应到文末，实际 " + std::to_string(p.lines[0].sectionEndLine));
                check(p.lines[1].sectionEndLine == 2,
                      "H3 乙应在 ## 丙 处收尾，实际 " + std::to_string(p.lines[1].sectionEndLine));
                check(p.lines[2].sectionEndLine == 5,
                      "H2 丙的章节应到文末，实际 " + std::to_string(p.lines[2].sectionEndLine));
            }
        }
        {
            // 连续同级：三级逐个收尾，最后一个到文末。
            const neo::LpPlan p = neo::buildLpPlan("# 甲\n# 乙\n# 丙\n");
            check(p.lines.size() == 4, "连续同级样例应有 4 行");
            if (p.lines.size() == 4) {
                check(p.lines[0].sectionEndLine == 1, "甲应在乙处收尾");
                check(p.lines[1].sectionEndLine == 2, "乙应在丙处收尾");
                check(p.lines[2].sectionEndLine == 4, "丙应到文末");
            }
        }
        {
            // 标题后无内容：只剩末尾那个空行，终点 = 行数。
            const neo::LpPlan p = neo::buildLpPlan("# 只有标题\n");
            check(p.lines.size() == 2, "标题后无内容样例应有 2 行");
            if (p.lines.size() == 2) {
                check(p.lines[0].sectionEndLine == 2,
                      "标题后无内容时终点应是行数，实际 " +
                          std::to_string(p.lines[0].sectionEndLine));
            }
        }
        {
            // setext 单组配对：文本行的终点跳过自己的下划线、落在 # 后续；
            // 下划线行自身 -1；正文归属配对文本行。
            const neo::LpPlan p = neo::buildLpPlan("setext 标题\n==========\n正文\n# 后续\n");
            check(p.lines.size() == 5, "setext 样例应有 5 行");
            if (p.lines.size() == 5) {
                check(p.lines[0].sectionEndLine == 3,
                      "setext 标题终点应是 # 后续，实际 " +
                          std::to_string(p.lines[0].sectionEndLine));
                check(p.lines[1].sectionEndLine == -1, "setext 下划线行终点应恒为 -1");
                check(p.lines[1].sectionHeadingLine == 0, "下划线行应归到配对文本行");
                check(p.lines[3].sectionEndLine == 5, "# 后续应到文末");
            }
        }
        {
            // 连续 setext 配对（旧算法的历史行为，由 oracle 固化）：
            // 第二组的文本行 Body 虽被 isSetextUnderline 连坐判成"下划线"、
            // 自己不记终点，但它照样要在它那一行给 Title 的章节收尾。
            const neo::LpPlan p = neo::buildLpPlan("Title\n---\nBody\n===\n");
            check(p.lines.size() == 5, "连续 setext 样例应有 5 行");
            if (p.lines.size() == 5) {
                check(p.lines[0].sectionEndLine == 2,
                      "Title 的章节应在 Body 处收尾，实际 " +
                          std::to_string(p.lines[0].sectionEndLine));
                check(p.lines[2].sectionEndLine == -1, "被连坐判定的文本行自身不记终点");
                check(p.lines[3].sectionEndLine == -1, "下划线行不记终点");
            }
        }
        {
            // 空行隔开的 --- 是分隔线，不能再把 Title 提升为 setext 标题。
            // Body/=== 仍是紧邻的一级 setext 标题，负责收束 H1。
            const neo::LpPlan p = neo::buildLpPlan("# H1\nTitle\n\n---\nBody\n===\ntail\n");
            check(p.lines.size() == 8, "空行隔开的下划线样例应有 8 行");
            if (p.lines.size() == 8) {
                check(p.lines[0].sectionEndLine == 4,
                      "H1 应在 Body 处收尾，实际 " + std::to_string(p.lines[0].sectionEndLine));
                check(p.lines[1].sectionEndLine == -1 && p.lines[1].kind == neo::LpKind::Text,
                      "Title 应保持正文，实际终点 " +
                          std::to_string(p.lines[1].sectionEndLine));
                check(p.lines[3].sectionEndLine == -1 && p.lines[3].kind == neo::LpKind::Divider,
                      "空行后的 --- 应保持分隔线，实际终点 " +
                          std::to_string(p.lines[3].sectionEndLine));
            }
        }
        {
            // 分隔线样例里不产生任何标题：全部行终点保持 -1。
            const std::vector<std::string> dividers = {"---\n\n正文\n", "---\n---\n---\n正文\n",
                                                       "***\n***\n正文\n"};
            for (const std::string& doc : dividers) {
                const neo::LpPlan p = neo::buildLpPlan(doc);
                bool allMinusOne = true;
                for (const neo::LpLine& line : p.lines) {
                    allMinusOne = allMinusOne && line.sectionEndLine == -1 &&
                                  line.kind != neo::LpKind::Heading;
                }
                check(allMinusOne, "分隔线样例不应产生标题章节终点");
            }
        }
    }

    // ── ⑯ T6：闭围栏回填"扫整张行表" → "只扫本块行区间"，逐行对照朴素 oracle ──
    {
        struct CodeCase {
            const char* name;
            std::string doc;
        };
        const std::string docBlocks =
            "前言\n"          // 0
            "\n"              // 1
            "```cpp\n"        // 2 开围栏（3 个反引号）
            "int a;\n"        // 3
            "\n"              // 4 块内空行
            "~~~~\n"          // 5 波浪线在反引号块里只是内容
            "int b;\n"        // 6
            "````\n"          // 7 闭围栏（4 个反引号 ≥ 3）
            "\n"              // 8
            "中间段落\n"      // 9 普通段落
            "\n"              // 10
            "~~~~\n"          // 11 开围栏（4 个波浪线）
            "tilde\n"         // 12
            "```\n"           // 13 反引号在波浪线块里只是内容
            "~~~\n"           // 14 3 个波浪线 < 4：关不掉本块
            "   \n"           // 15 块内空白行
            "~~~~~~\n"        // 16 闭围栏（6 ≥ 4）
            "\n"              // 17
            "尾段\n";         // 18（19 = 文末空行）
        const std::string docAdjacent = "```\na\n```\n```\nb\n```\n";
        const std::string docInnerBlanks = "```\n\n\nx\n\n```\n";
        const std::string docUnclosed = "正文\n```js\nvar x = 1;\n";
        const std::vector<CodeCase> cases = {
            {"多块夹普通段落 + 围栏字符/长度混用", docBlocks},
            {"两块紧贴", docAdjacent},
            {"块内多空行", docInnerBlanks},
            {"未闭合块", docUnclosed},
            {"主样例 kDoc", kDoc},
            {"紧贴双块 kBoxDoc", kBoxDoc},
            {"frontmatter 样例 kPrefixDoc", kPrefixDoc},
            {"表格样例 kTableDoc", kTableDoc},
        };

        int badDocs = 0;
        int openBlocks = 0;
        int closedBlocks = 0;
        int codeLines = 0;
        int blankLines = 0;
        int blankStamped = 0;
        int blankInsideCode = 0;
        for (const CodeCase& item : cases) {
            const neo::LpPlan p = neo::buildLpPlan(item.doc);
            const std::vector<int> expect = naiveCodeBlockEnds(p);
            check(expect.size() == p.lines.size(),
                  std::string(item.name) + "：oracle 行数应与计划一致");
            bool docOk = true;
            for (std::size_t i = 0; i < p.lines.size() && i < expect.size(); ++i) {
                const neo::LpLine& line = p.lines[i];
                if (line.kind == neo::LpKind::Code &&
                    slice(item.doc, line.srcBeg, line.srcEnd).find_first_not_of(" \t") == std::string::npos) {
                    ++blankInsideCode;
                }
                // Ordinary blanks remain outside code; verbatim blanks are Code.
                if (line.kind == neo::LpKind::Blank) {
                    ++blankLines;
                    if (line.codeBlockBeg != -1 || line.codeBlockEnd != -1) {
                        ++blankStamped;
                        docOk = false;
                        std::printf("  !! %s：第 %zu 行是空行却被盖章 codeBlockBeg=%d codeBlockEnd=%d\n",
                                    item.name, i, line.codeBlockBeg, line.codeBlockEnd);
                    }
                    // 紧夹在同一个代码块（上下最近的 Code 行同属一块）= 块内空行
                    int above = -1;
                    int below = -1;
                    for (std::size_t j = i; j-- > 0;) {
                        if (p.lines[j].kind == neo::LpKind::Code) {
                            above = static_cast<int>(j);
                            break;
                        }
                    }
                    for (std::size_t j = i + 1; j < p.lines.size(); ++j) {
                        if (p.lines[j].kind == neo::LpKind::Code) {
                            below = static_cast<int>(j);
                            break;
                        }
                    }
                    if (above >= 0 && below >= 0 && p.lines[static_cast<std::size_t>(above)].codeBlockBeg >= 0 &&
                        p.lines[static_cast<std::size_t>(above)].codeBlockBeg ==
                            p.lines[static_cast<std::size_t>(below)].codeBlockBeg) {
                        ++blankInsideCode;
                    }
                }
                // codeBlockEnd：与朴素全表回填 oracle 逐行一致
                if (line.codeBlockEnd != expect[i]) {
                    docOk = false;
                    std::printf("  !! %s：第 %zu 行 codeBlockEnd 实际 %d / 旧全表回填 %d\n", item.name, i,
                                line.codeBlockEnd, expect[i]);
                }
                if (line.kind != neo::LpKind::Code && line.codeBlockBeg != -1) {
                    docOk = false;
                    std::printf("  !! %s：第 %zu 行非 Code 却带 codeBlockBeg=%d\n", item.name, i,
                                line.codeBlockBeg);
                }
                if (line.kind == neo::LpKind::Code) {
                    ++codeLines;
                    if (line.codeFence && line.srcBeg == line.codeBlockBeg) {
                        ++openBlocks;
                    } else if (line.codeFence) {
                        ++closedBlocks;
                    }
                    // codeBlockBeg 必须指向本块的开围栏行（改动前语义，T6 不碰它）
                    int open = -1;
                    for (std::size_t j = 0; j <= i; ++j) {
                        if (p.lines[j].srcBeg == line.codeBlockBeg) {
                            open = static_cast<int>(j);
                            break;
                        }
                    }
                    if (open < 0 || p.lines[static_cast<std::size_t>(open)].kind != neo::LpKind::Code ||
                        !p.lines[static_cast<std::size_t>(open)].codeFence) {
                        docOk = false;
                        std::printf("  !! %s：第 %zu 行的 codeBlockBeg=%d 不是任何开围栏行\n", item.name, i,
                                    line.codeBlockBeg);
                    } else {
                        // 开围栏行必在回填区间内，且它到本行之间没有块外行
                        for (std::size_t j = static_cast<std::size_t>(open) + 1; j <= i; ++j) {
                            const neo::LpLine& inner = p.lines[j];
                            if (inner.kind != neo::LpKind::Code && inner.kind != neo::LpKind::Blank) {
                                docOk = false;
                                std::printf("  !! %s：第 %zu 行所属块越过了第 %zu 行（kind=%d）\n",
                                            item.name, i, j, static_cast<int>(inner.kind));
                                break;
                            }
                        }
                    }
                }
            }
            if (!docOk) {
                ++badDocs;
            }
        }
        check(badDocs == 0,
              "T6：codeBlockEnd 须与朴素全表回填 oracle 逐行一致，" + std::to_string(badDocs) +
                  " 个样例不符");
        check(blankStamped == 0,
              "T6：空行不应被盖章 codeBlockBeg/End，实际 " + std::to_string(blankStamped) + " 行");
        check(codeLines >= 30, "T6：样例应覆盖足够多的代码行，实际 " + std::to_string(codeLines));
        check(openBlocks >= 9, "T6：样例应覆盖足够多的开围栏，实际 " + std::to_string(openBlocks));
        check(closedBlocks >= 8, "T6：样例应覆盖足够多的闭围栏，实际 " + std::to_string(closedBlocks));
        check(blankLines >= 15, "T6：样例应覆盖足够多的空行，实际 " + std::to_string(blankLines));
        check(blankInsideCode >= 5,
              "T6：样例应覆盖足够多的块内空行，实际 " + std::to_string(blankInsideCode));

        // 关键样例的显式断言（不依赖 oracle，直接把改动前的结论写死）
        {
            // 多块夹普通段落：两块的回填互不串味，块间段落/块内空行都不被盖章。
            const neo::LpPlan p = neo::buildLpPlan(docBlocks);
            check(p.lines.size() == 20, "多块样例应有 20 行，实际 " + std::to_string(p.lines.size()));
            if (p.lines.size() == 20) {
                check(p.stats.fencedBlocks == 2, "多块样例应闭合 2 个代码块");
                const neo::LpLine& open1 = p.lines[2];
                const neo::LpLine& close1 = p.lines[7];
                check(open1.kind == neo::LpKind::Code && open1.codeFence &&
                          open1.srcBeg == open1.codeBlockBeg,
                      "第 2 行应是第一块的开围栏");
                check(open1.codeBlockEnd == close1.srcEnd,
                      "开围栏行也要盖上本块终点，实际 " + std::to_string(open1.codeBlockEnd));
                check(p.lines[3].kind == neo::LpKind::Code &&
                          p.lines[3].codeBlockBeg == open1.srcBeg &&
                          p.lines[3].codeBlockEnd == close1.srcEnd,
                      "块内行应属第一块且终点是闭围栏行尾");
                check(p.lines[4].kind == neo::LpKind::Code && p.lines[4].codeBlockBeg == open1.srcBeg &&
                          p.lines[4].codeBlockEnd == close1.srcEnd,
                      "块内空行应保持第一块归属");
                check(p.lines[5].kind == neo::LpKind::Code && p.lines[5].codeBlockEnd == close1.srcEnd,
                      "反引号块里的波浪线行应属第一块");
                check(close1.codeFence && close1.codeBlockBeg == open1.srcBeg &&
                          close1.codeBlockEnd == close1.srcEnd,
                      "闭围栏行自己也要有本块终点");
                check(p.lines[9].kind == neo::LpKind::Text && p.lines[9].codeBlockBeg == -1 &&
                          p.lines[9].codeBlockEnd == -1,
                      "块间普通段落不应被盖章");
                const neo::LpLine& open2 = p.lines[11];
                const neo::LpLine& close2 = p.lines[16];
                check(open2.kind == neo::LpKind::Code && open2.codeFence &&
                          open2.srcBeg == open2.codeBlockBeg && open2.codeBlockEnd == close2.srcEnd,
                      "第二块开围栏应自成一块且终点是第二个闭围栏");
                check(open2.codeBlockBeg != open1.codeBlockBeg, "两个代码块的 codeBlockBeg 不应相同");
                check(p.lines[13].kind == neo::LpKind::Code &&
                          p.lines[13].codeBlockBeg == open2.srcBeg &&
                          p.lines[13].codeBlockEnd == close2.srcEnd,
                      "波浪线块里的反引号行应盖第二块的终点");
                check(p.lines[14].kind == neo::LpKind::Code &&
                          p.lines[14].codeBlockEnd == close2.srcEnd,
                      "3 个波浪线关不掉 4 个波浪线的开围栏");
                check(p.lines[15].kind == neo::LpKind::Code && p.lines[15].codeBlockEnd == close2.srcEnd,
                      "第二块内空行应保持第二块归属");
                check(p.lines[18].kind == neo::LpKind::Text && p.lines[18].codeBlockBeg == -1 &&
                          p.lines[18].codeBlockEnd == -1,
                      "块后段落不应被盖章");
            }
        }
        {
            // 两块紧贴：第二块的回填绝不能碰到第一块的行（哪怕只隔一行）。
            const neo::LpPlan p = neo::buildLpPlan(docAdjacent);
            check(p.lines.size() == 7, "紧贴双块样例应有 7 行，实际 " + std::to_string(p.lines.size()));
            if (p.lines.size() == 7) {
                check(p.stats.fencedBlocks == 2, "紧贴双块样例应闭合 2 个代码块");
                check(p.lines[0].codeBlockBeg == p.lines[0].srcBeg &&
                          p.lines[0].codeBlockEnd == p.lines[2].srcEnd &&
                          p.lines[1].codeBlockEnd == p.lines[2].srcEnd,
                      "第一块应只盖到自己的闭围栏");
                check(p.lines[3].codeBlockBeg == p.lines[3].srcBeg &&
                          p.lines[3].codeBlockEnd == p.lines[5].srcEnd,
                      "第二块开围栏应自成一块");
                check(p.lines[4].codeBlockBeg == p.lines[3].srcBeg &&
                          p.lines[4].codeBlockEnd == p.lines[5].srcEnd,
                      "第二块内容行不应被第一块盖章");
                check(p.lines[2].codeBlockEnd == p.lines[2].srcEnd &&
                          p.lines[5].codeBlockEnd == p.lines[5].srcEnd,
                      "两个闭围栏行的终点都是自己行尾");
            }
        }
        {
            // 块内多空行：空行一律不盖章，内容行照旧盖到闭围栏。
            const neo::LpPlan p = neo::buildLpPlan(docInnerBlanks);
            check(p.lines.size() == 7, "多空行样例应有 7 行，实际 " + std::to_string(p.lines.size()));
            if (p.lines.size() == 7) {
                check(p.stats.fencedBlocks == 1, "多空行样例应闭合 1 个代码块");
                check(p.lines[1].kind == neo::LpKind::Code && p.lines[2].kind == neo::LpKind::Code &&
                          p.lines[4].kind == neo::LpKind::Code,
                      "块内 1/2/4 空行应属于代码容器");
                check(p.lines[1].codeBlockEnd == p.lines[5].srcEnd && p.lines[2].codeBlockEnd == p.lines[5].srcEnd &&
                          p.lines[4].codeBlockEnd == p.lines[5].srcEnd,
                      "块内空行应与闭围栏共享终点");
                check(p.lines[3].kind == neo::LpKind::Code &&
                          p.lines[3].codeBlockEnd == p.lines[5].srcEnd,
                      "块内内容行应盖到闭围栏行尾");
                check(p.lines[0].codeBlockEnd == p.lines[5].srcEnd &&
                          p.lines[5].codeBlockEnd == p.lines[5].srcEnd,
                      "开/闭围栏行都应有本块终点");
            }
        }
        {
            // 未闭合块：现有语义保持 —— 每行终点 = 本行行尾，且不计入已闭合块。
            const neo::LpPlan p = neo::buildLpPlan(docUnclosed);
            check(p.lines.size() == 4, "未闭合样例应有 4 行，实际 " + std::to_string(p.lines.size()));
            if (p.lines.size() == 4) {
                check(p.stats.fencedBlocks == 0, "未闭合块不应计入 fencedBlocks");
                check(p.lines[0].kind == neo::LpKind::Text && p.lines[0].codeBlockBeg == -1 &&
                          p.lines[0].codeBlockEnd == -1,
                      "块前段落不应被盖章");
                check(p.lines[1].kind == neo::LpKind::Code &&
                          p.lines[1].codeBlockBeg == p.lines[1].srcBeg &&
                          p.lines[1].codeBlockEnd == p.lines[1].srcEnd,
                      "未闭合开围栏的终点应是本行行尾");
                check(p.lines[2].kind == neo::LpKind::Code &&
                          p.lines[2].codeBlockBeg == p.lines[1].srcBeg &&
                          p.lines[2].codeBlockEnd == p.lines[2].srcEnd,
                      "未闭合块内容行的终点应是本行行尾");
                check(p.lines[3].kind == neo::LpKind::Code && p.lines[3].codeBlockBeg == p.lines[1].srcBeg &&
                          p.lines[3].codeBlockEnd == p.lines[3].srcEnd,
                      "未闭合代码块末尾空行仍保持块归属");
            }
        }
    }

    // ── T10 style_schema 拆层 golden：标题 textShiftY / 公式逐字段 / 浅深主题装饰 ──
    {
        // ① markdownStyle 公式逐字段（搬层后必须一位不差）：标题间距上置的
        //    h?LineHeight = 1.2×自身字号 + 1em，textShiftY 用 bodySize。
        const components::MarkdownStyle s16 =
            neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace",
                               neo::editorColors(neo::ThemeMode::Dark));
        check(std::fabs(s16.bodySize - 16.0f) < 0.001f &&
                  std::fabs(s16.bodyLineHeight - 24.0f) < 0.001f,
              "16px 正文行高应为 24（1.5×）");
        check(std::fabs(s16.h1Size - 16.0f * 1.618f) < 0.001f, "h1Size 应为 16×1.618");
        check(std::fabs(s16.h2Size - 16.0f * 1.462f) < 0.001f, "h2Size 应为 16×1.462");
        check(std::fabs(s16.h3Size - 16.0f * 1.318f) < 0.001f, "h3Size 应为 16×1.318");
        check(std::fabs(s16.h1LineHeight - (s16.h1Size * 1.2f + 16.0f)) < 0.001f,
              "h1 行盒应为 1.2×h1Size + 1em");
        check(std::fabs(s16.h2LineHeight - (s16.h2Size * 1.2f + 16.0f)) < 0.001f,
              "h2 行盒应为 1.2×h2Size + 1em");
        check(std::fabs(s16.h3LineHeight - (s16.h3Size * 1.2f + 16.0f)) < 0.001f,
              "h3 行盒应为 1.2×h3Size + 1em");
        check(std::fabs(s16.codeSize - 14.0f) < 0.001f &&
                  std::fabs(s16.codeLineHeight - 21.0f) < 0.001f,
              "16px 代码字号/行高应为 14/21");
        check(std::fabs(s16.blockGap - 16.0f) < 0.001f &&
                  std::fabs(s16.radius - 4.0f) < 0.001f,
              "blockGap/radius 应为 16/4");
        check(s16.fontFamily == "Microsoft YaHei" && s16.codeFontFamily == "monospace",
              "字体名应原样透传");
        // 非默认字号：比例公式照旧（em = bodySize）。
        const components::MarkdownStyle s24 =
            neo::markdownStyle(24.0f, "f", "m", neo::editorColors(neo::ThemeMode::Dark));
        check(std::fabs(s24.h1LineHeight - (24.0f * 1.618f * 1.2f + 24.0f)) < 0.001f,
              "24px 下 h1 行盒公式应随字号缩放");

        // ② 标题几何 golden（R1）：行盒 = 纯文字高度（h1/h2 = 1.2×、h3 = 1.3×，
        //    与 Obsidian 的 --h?-line-height 一致），间距走 spaceBefore（= --p-spacing），
        //    textShiftY 必须全为 0 —— 行内文字下移那套已经退役。
        std::vector<LineDecoration> shiftTable;
        neo::lp::buildDecorations(plan, style, plan.lines[bodyLine].srcBeg, shiftTable);
        int shiftWrong = 0;
        int gapWrong = 0;
        int heightWrong = 0;
        for (std::size_t i = 0; i < plan.lines.size() && i < shiftTable.size(); ++i) {
            if (std::fabs(shiftTable[i].textShiftY) > 0.001f) {
                ++shiftWrong;
            }
            // kDoc：0 标题 / 1 空行 / 2 正文 / 3 空行 / 4 列表 / 5 空行 / 6 围栏 …
            // 只有第 0 行是标题，且它前面没有空行（文档首块）→ 该行 gap 也是 0。
            const bool expectGap = plan.lines[i].kind == neo::LpKind::Heading &&
                                   !(i >= 2 && plan.lines[i - 1].kind == neo::LpKind::Blank &&
                                     plan.lines[i - 2].kind == neo::LpKind::Heading) && i > 0;
            if (std::fabs(shiftTable[i].spaceBefore - (expectGap ? neo::lp::kBlockGap : 0.0f)) > 0.001f) {
                ++gapWrong;
            }
            const neo::LpLine& line = plan.lines[i];
            if (line.kind == neo::LpKind::Heading) {
                float wantSize = style.h3Size;
                float wantHeight = style.h3Size * neo::lp::kHeadingLineHeight3;
                switch (line.headingLevel) {
                    case 1:
                        wantSize = style.h1Size;
                        wantHeight = wantSize * neo::lp::kHeadingLineHeight1;
                        break;
                    case 2:
                        wantSize = style.h2Size;
                        wantHeight = wantSize * neo::lp::kHeadingLineHeight2;
                        break;
                    case 4:
                        wantSize = std::max(style.bodySize, style.h3Size * 0.89f);
                        wantHeight = wantSize * 1.35f;
                        break;
                    case 5:
                        wantSize = std::max(style.bodySize, style.h3Size * 0.80f);
                        wantHeight = wantSize * 1.4f;
                        break;
                    case 6:
                        wantSize = style.bodySize;
                        wantHeight = wantSize * 1.4f;
                        break;
                    default:
                        break;
                }
                if (std::fabs(shiftTable[i].lineHeight - wantHeight) > 0.001f) {
                    ++heightWrong;
                }
            }
        }
        check(shiftWrong == 0,
              "R1 起标题行 textShiftY 必须全为 0，不符 " + std::to_string(shiftWrong) + " 行");
        check(gapWrong == 0,
              "标题行 spaceBefore 应为 --p-spacing（文档首块/连续标题例外为 0），不符 " +
                  std::to_string(gapWrong) + " 行");
        check(heightWrong == 0,
              "标题行盒应为纯文字高度（h1/h2 = 1.2×、h3 = 1.3×），不符 " +
                  std::to_string(heightWrong) + " 行");

        // ③ 浅 / 深主题装饰输出逐字段 golden：同一文档、同一光标，只有颜色字段
        //    随主题变，几何/字号/洞区完全一致；颜色逐项等于各自配色表。
        const neo::EditorColors& darkColors = neo::editorColors(neo::ThemeMode::Dark);
        const neo::EditorColors& lightColors = neo::editorColors(neo::ThemeMode::Light);
        const components::MarkdownStyle darkStyle =
            neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace", darkColors);
        const components::MarkdownStyle lightStyle =
            neo::markdownStyle(16.0f, "Microsoft YaHei", "monospace", lightColors);
        const int cursor = plan.lines[bodyLine].srcBeg;
        std::vector<LineDecoration> darkTable;
        std::vector<LineDecoration> lightTable;
        neo::lp::buildDecorations(plan, darkStyle, cursor, darkTable, "", {}, nullptr, kDoc,
                                  &darkColors);
        neo::lp::buildDecorations(plan, lightStyle, cursor, lightTable, "", {}, nullptr, kDoc,
                                  &lightColors);
        check(darkTable.size() == plan.lines.size() &&
                  lightTable.size() == plan.lines.size(),
              "两主题装饰表行数都应与计划一致");
        const auto checkThemeColors =
            [&](const std::vector<LineDecoration>& table, const neo::EditorColors& colors,
                const char* label) {
                // 颜色逐字段等于本主题配色表。
                check(sameColor(table[headingLine].textColor, colors.heading),
                      std::string(label) + "：标题行文字色应等于本主题 heading");
                check(sameColor(table[static_cast<std::size_t>(fenceOpenLine)].textColor,
                                colors.codeText),
                      std::string(label) + "：代码行文字色应等于本主题 codeText");
            };
        checkThemeColors(darkTable, darkColors, "深色");
        checkThemeColors(lightTable, lightColors, "浅色");
        check(sameColor(darkStyle.accent, darkColors.markdownAccent) &&
                  sameColor(darkStyle.muted, darkColors.textMuted) &&
                  sameColor(darkStyle.codeBackground, darkColors.codeBackground) &&
                  sameColor(lightStyle.accent, lightColors.markdownAccent) &&
                  sameColor(lightStyle.muted, lightColors.textMuted),
              "markdownStyle 语义色应逐字段来自对应主题配色表");
        // 两主题的装饰输出必须真的不同（否则主题没进装饰）。
        bool colorDiffers = false;
        for (std::size_t i = 0; i < darkTable.size() && i < lightTable.size(); ++i) {
            if (!sameColor(darkTable[i].textColor, lightTable[i].textColor)) {
                colorDiffers = true;
                break;
            }
        }
        check(colorDiffers, "浅/深主题的装饰文字色应至少一行不同（主题进了装饰输出）");
        // 几何逐字段不变：字号、行高、textShiftY、洞区、隐藏位。
        int geomDrift = 0;
        for (std::size_t i = 0; i < darkTable.size() && i < lightTable.size(); ++i) {
            const LineDecoration& a = darkTable[i];
            const LineDecoration& b = lightTable[i];
            if (std::fabs(a.fontSize - b.fontSize) > 0.001f ||
                std::fabs(a.lineHeight - b.lineHeight) > 0.001f ||
                std::fabs(a.textShiftY - b.textShiftY) > 0.001f ||
                a.holes.size() != b.holes.size() || a.hidden != b.hidden) {
                ++geomDrift;
            }
        }
        check(geomDrift == 0,
              "主题切换不应改动几何/洞区，漂 " + std::to_string(geomDrift) + " 行");
    }

    // ── T22：WikiLink 的 mark/content 区间与 [[目标|别名]] 投影 ───────────────────
    // 只验证现有 MD_SPAN_WIKILINK 的区间语义与 holes/accent 投影，不实现跳转/反链。
    // 四例：① [[目标]] ② [[目标|别名]] ③ 行内前后有普通文字 ④ 光标进入该行显示完整源码。
    // 附两例钉住"只吸收首个管道"：⑤ [[foo|]]（content 不以 | 开头 → 什么都不吸收）
    //                                ⑥ [[a|b|c]]（吸收首个 |，第二个 | 留在 content）。
    {
        const std::string wikiDoc =
            "[[目标]]\n"
            "[[目标|别名]]\n"
            "前文 [[目标|别名]] 后文\n"
            "[[foo|]]\n"
            "[[a|b|c]]\n";
        const neo::LpPlan wikiPlan = neo::buildLpPlan(wikiDoc);
        check(wikiPlan.stats.usedMd4c, "wiki 文档应启用 md4c（否则没有行内片段）");
        check(wikiPlan.stats.invalidRanges == 0, "wiki 文档不应产生越界区间");
        // 结尾 "\n" 会多出一个空行（与 kDoc 的行数口径一致），正文恰为前 5 行。
        check(wikiPlan.lines.size() == 6, "wiki 文档应解析为 5 行正文 + 1 个尾随空行");

        // 探测打印：每行的 conceal 与每个 span 的 open/content/close（区间证据）。
        for (const neo::LpLine& line : wikiPlan.lines) {
            std::printf("[T22] 行 %d conceal=", line.number);
            for (const neo::LpRange& range : line.conceal) {
                std::printf("[%d,%d)\"%s\" ", range.beg, range.end,
                            escape(slice(wikiDoc, range.beg, range.end)).c_str());
            }
            for (const neo::LpSpan& span : line.spans) {
                std::printf("\n[T22] 行 %d span=%s open=[%d,%d)\"%s\" content=[%d,%d)\"%s\" close=[%d,%d)\"%s\"",
                            line.number, span.kind == neo::LpSpanKind::WikiLink ? "WikiLink" : "其它",
                            span.openMark.beg, span.openMark.end,
                            escape(slice(wikiDoc, span.openMark.beg, span.openMark.end)).c_str(),
                            span.content.beg, span.content.end,
                            escape(slice(wikiDoc, span.content.beg, span.content.end)).c_str(),
                            span.closeMark.beg, span.closeMark.end,
                            escape(slice(wikiDoc, span.closeMark.beg, span.closeMark.end)).c_str());
            }
            std::printf("\n");
        }

        const neo::LpLine& plainLine = wikiPlan.lines[0];   // [[目标]]
        const neo::LpLine& aliasLine = wikiPlan.lines[1];   // [[目标|别名]]
        const neo::LpLine& textLine = wikiPlan.lines[2];    // 前文 … 后文
        check(plainLine.spans.size() == 1 && plainLine.spans[0].kind == neo::LpSpanKind::WikiLink,
              "例1：[[目标]] 应产出一个 WikiLink span");
        check(aliasLine.spans.size() == 1 && aliasLine.spans[0].kind == neo::LpSpanKind::WikiLink,
              "例2：[[目标|别名]] 应产出一个 WikiLink span");
        check(textLine.spans.size() == 1 && textLine.spans[0].kind == neo::LpSpanKind::WikiLink,
              "例3：前后有普通文字时仍应产出一个 WikiLink span");

        // 投影文本 = 行文本去掉 holes（组件的可见文字正是这个口径）。
        auto visibleOf = [&](const std::vector<LineDecoration>& table, const neo::LpLine& line) {
            std::string out;
            int position = line.srcBeg;
            for (const LineHole& hole : table[static_cast<std::size_t>(line.number)].holes) {
                out += slice(wikiDoc, position, hole.beg);
                position = hole.end;
            }
            out += slice(wikiDoc, position, line.srcEnd);
            return out;
        };
        auto hasAccentRun = [&](const std::vector<LineDecoration>& table, const neo::LpLine& line, int beg,
                                int end) {
            for (const LineRun& run : table[static_cast<std::size_t>(line.number)].runs) {
                if (run.beg <= beg && run.end >= end && run.style.color.a > 0.0f) {
                    return true;
                }
            }
            return false;
        };

        const int plainBeg = static_cast<int>(wikiDoc.find("[[目标]]"));
        const int aliasBeg = static_cast<int>(wikiDoc.find("[[目标|别名]]"));
        const int textBeg = static_cast<int>(wikiDoc.find("前文"));
        const int textLinkBeg = static_cast<int>(wikiDoc.find("目标", textBeg));
        check(plainBeg == 0 && aliasBeg > 0 && textBeg > 0 && textLinkBeg > 0, "样例位置应能定位");

        // 光标在三行之外的"前文"上（不在任何片段内）→ 行内标记全藏。
        std::vector<LineDecoration> table;
        neo::lp::buildDecorations(wikiPlan, style, textBeg, table);
        check(table.size() == wikiPlan.lines.size(), "wiki 装饰表行数与计划一致");

        // ── 例 1：[[目标]] ──
        {
            const neo::LpSpan& span = plainLine.spans[0];
            const std::string open = slice(wikiDoc, span.openMark.beg, span.openMark.end);
            const std::string content = slice(wikiDoc, span.content.beg, span.content.end);
            const std::string close = slice(wikiDoc, span.closeMark.beg, span.closeMark.end);
            check(open == "[[", "例1：openMark 应为 `[[`（实际 \"" + open + "\"）");
            check(content == "目标", "例1：content 应为目标 `目标`（实际 \"" + content + "\"）");
            check(close == "]]", "例1：closeMark 应为 `]]`（实际 \"" + close + "\"）");
            const std::string holes = holesText(wikiDoc, table, 0);
            check(holes.find("[[") != std::string::npos && holes.find("]]") != std::string::npos,
                  "例1：光标不在该片段上时 `[[`/`]]` 应进 holes 隐藏");
            const std::string visible = visibleOf(table, plainLine);
            std::printf("[T22] 例1 holes=\"%s\" 投影=\"%s\"\n", escape(holes).c_str(),
                        escape(visible).c_str());
            check(visible == "目标", "例1：投影文本应为 `目标`");
            check(hasAccentRun(table, plainLine, span.content.beg, span.content.end),
                  "例1：content 段应带 accent 色");
        }

        // ── 例 2：[[目标|别名]] ──
        {
            const neo::LpSpan& span = aliasLine.spans[0];
            const std::string open = slice(wikiDoc, span.openMark.beg, span.openMark.end);
            const std::string content = slice(wikiDoc, span.content.beg, span.content.end);
            const std::string close = slice(wikiDoc, span.closeMark.beg, span.closeMark.end);
            // 管道是分隔符不是可见文字：它必须并进 openMark（进 holes 藏掉），
            // content 只剩别名 —— 否则投影多一个 `|`、accent 还会把管道一起染色。
            check(open == "[[目标|", "例2：openMark 应为 `[[目标|`（吸收首个管道，实际 \"" + open + "\"）");
            check(content == "别名", "例2：content 应恰为 `别名`（实际 \"" + content + "\"）");
            check(content.find("目标") == std::string::npos,
                  "例2：目标应属于 openMark 而非 content（content \"" + content + "\"）");
            check(content.empty() || content[0] != '|',
                  "例2：首个管道应并入 openMark，content 不得以 `|` 开头（content \"" + content + "\"）");
            check(close == "]]", "例2：closeMark 应为 `]]`（实际 \"" + close + "\"）");
            const std::string holes = holesText(wikiDoc, table, 1);
            check(holes == "[[目标|]]",
                  "例2：holes 应恰好覆盖 `[[目标|` 与 `]]`（实际 \"" + escape(holes) + "\"）");
            const std::string visible = visibleOf(table, aliasLine);
            std::printf("[T22] 例2 holes=\"%s\" 投影=\"%s\"\n", escape(holes).c_str(),
                        escape(visible).c_str());
            check(visible == "别名", "例2：投影应恰为 `别名`（实际 \"" + escape(visible) + "\"）");
            check(visible.find('|') == std::string::npos,
                  "例2：投影不得含管道 `|`（实际 \"" + escape(visible) + "\"）");
            const int aliasTextBeg = static_cast<int>(wikiDoc.find("别名", aliasLine.srcBeg));
            check(hasAccentRun(table, aliasLine, aliasTextBeg, aliasTextBeg + 6),
                  "例2：别名文字应带 accent 色");
            const int pipePos = static_cast<int>(wikiDoc.find('|', aliasLine.srcBeg));
            check(pipePos > aliasLine.srcBeg && pipePos < aliasLine.srcEnd, "例2：样例行应含管道");
            check(!hasAccentRun(table, aliasLine, pipePos, pipePos + 1),
                  "例2：管道不属于 content，不应被 accent 染色");
        }

        // ── 例 3：行内前后有普通文字 ──
        {
            const std::string visible = visibleOf(table, textLine);
            std::printf("[T22] 例3 holes=\"%s\" 投影=\"%s\"\n",
                        escape(holesText(wikiDoc, table, 2)).c_str(), escape(visible).c_str());
            check(visible.find("前文") != std::string::npos && visible.find("后文") != std::string::npos,
                  "例3：链接前后的普通文字应保持可见");
            check(visible == "前文 别名 后文",
                  "例3：整行投影应为 `前文 别名 后文`（实际 \"" + escape(visible) + "\"）");
            check(visible.find('|') == std::string::npos,
                  "例3：投影不得含管道 `|`（实际 \"" + escape(visible) + "\"）");
            check(visible.find("别名") != std::string::npos && visible.find("目标") == std::string::npos,
                  "例3：链接段投影应为别名（实际 \"" + visible + "\"）");
            check(holesText(wikiDoc, table, 2).find("[[") != std::string::npos,
                  "例3：光标在 `前文` 上（未进片段）时 `[[` 仍应隐藏（粒度是片段）");
        }

        // ── 例 4：光标进入该行 → 显示完整源码 ──
        {
            // 4a. [[目标]] 整行即片段：光标落到行首就是进入片段。
            neo::lp::buildDecorations(wikiPlan, style, plainBeg, table);
            check(holesText(wikiDoc, table, 0).empty(), "例4a：光标进入 [[目标]] 行应显示完整源码");
            check(visibleOf(table, plainLine) == slice(wikiDoc, plainLine.srcBeg, plainLine.srcEnd),
                  "例4a：该行可见文本应等于源码");

            // 4b. [[目标|别名]] 同理（整行即片段）。
            neo::lp::buildDecorations(wikiPlan, style, aliasBeg, table);
            check(holesText(wikiDoc, table, 1).empty(), "例4b：光标进入 [[目标|别名]] 行应显示完整源码");
            check(visibleOf(table, aliasLine) == slice(wikiDoc, aliasLine.srcBeg, aliasLine.srcEnd),
                  "例4b：该行可见文本应等于源码");

            // 4c. 前后有文字的行：光标进入链接片段（落在 `目标` 上）才显示完整源码。
            neo::lp::buildDecorations(wikiPlan, style, textLinkBeg, table);
            check(holesText(wikiDoc, table, 2).empty(),
                  "例4c：光标进入该行的链接片段时应显示完整源码");
            check(visibleOf(table, textLine) == slice(wikiDoc, textLine.srcBeg, textLine.srcEnd),
                  "例4c：该行可见文本应等于源码");
        }

        // ── 例 5/6：只吸收**首个**管道，其余管道语义不动 ──
        // ⑤ `[[foo|]]`：md4c 把管道记进 closeMark，content 是 `foo`（不以 `|` 开头）
        //    → 不触发吸收，openMark 仍是 `[[`。
        // ⑥ `[[a|b|c]]`：只吃掉首个 `|`，第二个 `|` 留在 content 里原样可见/着色。
        {
            // 光标回到 `前文`：这两行都不是活动块，标记照常隐藏。
            neo::lp::buildDecorations(wikiPlan, style, textBeg, table);
            const neo::LpLine& emptyAliasLine = wikiPlan.lines[3];  // [[foo|]]
            const neo::LpLine& multiPipeLine = wikiPlan.lines[4];   // [[a|b|c]]
            check(emptyAliasLine.spans.size() == 1 &&
                      emptyAliasLine.spans[0].kind == neo::LpSpanKind::WikiLink,
                  "例5：[[foo|]] 应产出一个 WikiLink span");
            check(multiPipeLine.spans.size() == 1 &&
                      multiPipeLine.spans[0].kind == neo::LpSpanKind::WikiLink,
                  "例6：[[a|b|c]] 应产出一个 WikiLink span");

            const neo::LpSpan& emptySpan = emptyAliasLine.spans[0];
            const std::string emptyOpen = slice(wikiDoc, emptySpan.openMark.beg, emptySpan.openMark.end);
            const std::string emptyContent =
                slice(wikiDoc, emptySpan.content.beg, emptySpan.content.end);
            std::printf("[T22] 例5 open=\"%s\" content=\"%s\" close=\"%s\" 投影=\"%s\"\n",
                        escape(emptyOpen).c_str(), escape(emptyContent).c_str(),
                        escape(slice(wikiDoc, emptySpan.closeMark.beg, emptySpan.closeMark.end)).c_str(),
                        escape(visibleOf(table, emptyAliasLine)).c_str());
            check(emptyOpen == "[[" &&
                      (emptyContent.empty() || emptyContent[0] != '|'),
                  "例5：content 不以 `|` 开头 → 不吸收管道，openMark 应仍是 `[[`（open \"" +
                      escape(emptyOpen) + "\" content \"" + escape(emptyContent) + "\"）");
            check(visibleOf(table, emptyAliasLine).find("foo") != std::string::npos,
                  "例5：`foo` 应保持可见（投影 \"" +
                      escape(visibleOf(table, emptyAliasLine)) + "\"）");

            const neo::LpSpan& multiSpan = multiPipeLine.spans[0];
            const std::string multiOpen = slice(wikiDoc, multiSpan.openMark.beg, multiSpan.openMark.end);
            const std::string multiContent =
                slice(wikiDoc, multiSpan.content.beg, multiSpan.content.end);
            const std::string multiClose =
                slice(wikiDoc, multiSpan.closeMark.beg, multiSpan.closeMark.end);
            const std::string multiVisible = visibleOf(table, multiPipeLine);
            std::printf("[T22] 例6 open=\"%s\" content=\"%s\" close=\"%s\" 投影=\"%s\"\n",
                        escape(multiOpen).c_str(), escape(multiContent).c_str(),
                        escape(multiClose).c_str(), escape(multiVisible).c_str());
            check(multiOpen == "[[a|", "例6：openMark 应为 `[[a|`（只吸收首个管道，实际 \"" +
                                           escape(multiOpen) + "\"）");
            check(multiContent == "b|c", "例6：content 应为 `b|c`（第二个 `|` 保留，实际 \"" +
                                             escape(multiContent) + "\"）");
            check(multiClose == "]]", "例6：closeMark 应为 `]]`（实际 \"" + escape(multiClose) + "\"）");
            check(multiVisible == "b|c", "例6：投影应为 `b|c`（实际 \"" + escape(multiVisible) + "\"）");
        }
    }

    // ── R1：块间距（标题 padding-top / 表格容器内边距 / 其余为 0）──────────
    testListSemantics();
    testBlockSpaceBefore();
    testThematicBreakDecoration();
    testQuoteIndentAndTaskGlyphHitGeometry();
    testVerbatimBlankAndWrappedBlockBackgrounds();
    testCompletedTaskInlineRunBoundaries();
    testHeadingHierarchyAndInlineSpanBoundaries();

    // ── T4：编辑区间增量的等价性（装饰 + 布局，含回退计数与 10 万行性能记录）──
    testIncrementalEquivalence();
    // ── T4 审计 HIGH-2：行中 / 行尾回车的后缀锚点（联合推进 newLast/oldTail）──
    testEnterIncremental();
    testLargeDocumentIncremental();

    // ── T5：局部重解析（逐字段 oracle / 命中与回退 / 缓存链 / T12 耗时）─────
    testPartialProofBoundaries();
    testPartialOracle();
    testPartialViaCache();
    testPartialPerf();

    // ── T16：换到非 markdown 文档时应用层清计划缓存（version 单调不归零）────
    testPlanCacheResetOnNonMarkdown();

    // ── T13 收尾：主题文件版本进装饰缓存键（放最后：会临时改写 activeTheme()）──
    testThemeRevisionDecorationKey();

    // 局部重解析的诊断 oracle（NEO_LP_PLAN_VERIFY=1）：全程必须零 mismatch。
    // 有漏网之鱼时 buildLpPlanPartial 会当场拒掉并计数 —— 上面的"必须命中"断言多半
    // 会先失败，这里再把计数本身钉一次（不设该变量时计数恒 0，检查恒过）。
    if (const char* verify = std::getenv("NEO_LP_PLAN_VERIFY")) {
        if (verify[0] != '\0' && verify[0] != '0') {
            check(neo::planDebugStats().verifyMismatch == 0,
                  "NEO_LP_PLAN_VERIFY=1 下局部 oracle 应零 mismatch（实际 " +
                      std::to_string(neo::planDebugStats().verifyMismatch) + " 次）");
        }
    }

    std::printf("\n%s：%d 项检查，%d 项失败\n", gFailures == 0 ? "ALL PASS" : "FAILED", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
