#pragma once

// Live Preview 的适配层：把 lp_plan 的"逐行语义"翻译成输入组件能吃的"逐行装饰"。
//
// 四件事在这里汇合：
//   1. **字号 / 行高 / 文字色**：从 markdownStyle() 派生 —— 与原来的预览视图同一套比例；
//   2. **隐藏哪些字节**：块级标记与行内标记的规则不同，见 appendLineHoles；
//   3. **行内样式段**：`**粗体**`、`` `code` ``、链接、删除线真正参与渲染（见 appendLineRuns）；
//   4. **活动块 / 活动片段**：决定第 2 步里哪些标记要露出来。
//
// 第 3、4 条放在这一层而不是让组件认识 markdown：组件只按**内容比较**判断要不要重新排版，
// 于是"光标在同一处活动范围里移动"时这张表逐项相同 —— 一次文字测量都不会做。
//
// 关于"标记什么时候露出来"（与 Obsidian 对齐）：
//   * 块级标记（`# `、`> `、`- `、任务框、缩进、代码围栏）：光标落在**该块**内就露出来；
//   * 行内标记（`**`、`*`、`` ` ``、`~~`、`[]()`）：只有光标**进入该片段**时才露出来。
//   早期版本是"光标在块内 => 整块所有标记都露出来"，改行内规则后，同一行里的其它片段
//   仍然保持渲染后的样子。

#include "components/input_model.h"
#include "components/markdown.h"
#include "components/theme.h"
#include "core/render/text.h"
#include "model/image_size.h"
#include "model/lp_plan.h"
#include "model/lp_syntax.h"
#include "model/style_schema.h"
#include "model/text_file.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neo {
namespace lp {

using components::input_detail::DecoratorEditInfo;
using components::input_detail::LineCell;
using components::input_detail::LineDecoration;
using components::input_detail::LineHole;
using components::input_detail::LineRun;
using components::input_detail::PendingTextEdit;

// LineGlyph.codepoint 的历史非零 tag，用于维持布局/命中/装饰比较语义；
// components/input.h 按 checkbox/checked 字段绘制原生复选框几何，不再查图标字体。
inline constexpr int kTaskIcon = 0xF0C8;         // square（空方框）
inline constexpr int kTaskCheckedIcon = 0xF14A;  // square-check（带勾方框）
// 折叠箭头沿用历史非零 tag，供装饰缓存与 input.h 选择原生多边形方向。
inline constexpr int kFoldChevronDown = 0xF078;  // 展开 → 点击折叠
inline constexpr int kFoldChevronRight = 0xF054; // 已折叠 → 点击展开

// 一行用的字号与行高。
struct LineSize {
    float fontSize = 0.0f;
    float lineHeight = 0.0f;
};

// ── 块间距（R1，2026-09-26；实测见 参考/块模型契约-2026-09-26.md）────────────
// Obsidian 的 Live Preview 把块间距装在**行盒内部**：`.cm-line.HyperMD-header`
// 有 `padding-top: var(--p-spacing)`，逐行 top 因此首尾相接、行间 gap 恒为 0；
// 段与段之间的分离由**空源码行自己的 24px**给，所以除标题行与表格容器外，
// 块首不需要额外间距（阅读视图的 `--p-spacing` 块外边距不适用于 LP）。
// 1rem = --p-spacing，16px 正文下即 16px。主题 v2 才开放系数。
inline constexpr float kBlockGap = 16.0f;

// 单元格上下内边距，单位是 em（Obsidian `padding: 4px 8px`，行盒实测 29.6px
// = 20.8 行高 + 上下各 4.4 → 0.275em @16）。水平那 8px 在 input_model 的
// kTableCellPaddingEm 里。
inline constexpr float kTableRowPaddingYEm = 0.275f;

// 标题行盒的比例系数（Obsidian `--h{1,2,3}-line-height`）：h1/h2 = 1.2、h3 = 1.3。
// 注意 MarkdownStyle::h?LineHeight 里还烤着旧的 "+1em 上置留白" 且被只读的 Markdown
// 预览渲染器共用，R1 **不动那三个字段**，LP 自己按比例算纯文字行高（契约"标题行盒"）。
inline constexpr float kHeadingLineHeight1 = 1.2f;
inline constexpr float kHeadingLineHeight2 = 1.2f;
inline constexpr float kHeadingLineHeight3 = 1.3f;  // h4~h6 并入这一档

// Lower headings remain at least body size. Derive them from h3 so imported
// typography continues to control the hierarchy without extending the theme schema.
inline LineSize lineSizeFor(const LpLine& line, const components::MarkdownStyle& style) {
    switch (line.kind) {
        case LpKind::Heading:
            // 行盒 = 纯文字高度（1.2/1.3 × 字号）。旧版把 1em 上置留白烤进行高、
            // 再用 textShiftY 把文字压回去；R1 起这份留白改由 spaceBefore 承担，
            // 行盒不再被撑高（行号列/光标/命中都跟着回到文字高度）。
            switch (line.headingLevel) {
                case 1:
                    return {style.h1Size, style.h1TextLineHeight};
                case 2:
                    return {style.h2Size, style.h2TextLineHeight};
                case 4: {
                    const float size = std::max(style.bodySize, style.h3Size * 0.89f);
                    return {size, size * 1.35f};
                }
                case 5: {
                    const float size = std::max(style.bodySize, style.h3Size * 0.80f);
                    return {size, size * 1.4f};
                }
                case 6:
                    return {style.bodySize, style.bodySize * 1.4f};
                default:
                    return {style.h3Size, style.h3TextLineHeight};
            }
        case LpKind::Code:
        // frontmatter 跟代码块同一档字号：它也是"元数据"，比正文小一档才像附属信息。
        case LpKind::Frontmatter:
            return {style.codeSize, style.codeLineHeight};
        // 表格跟正文同一档字号（Obsidian 1.13 表格编辑器实测：表内 = 正文 16px，
        // 行高 1.3；规格表 §1）。列宽常量都是 em 基准，随字号等比缩放。
        // 行盒另加单元格的上下内边距（Obsidian `padding: 4px 8px`，实测行盒 29.6px
        // = 20.8 行高 + 上下各 4.4），文字用 textShiftY 跟着下移同样的量 ——
        // 行盒变高而文字顶对齐的话，多出来的空间会全落在文字下方。
        case LpKind::Table:
            return {style.bodySize,
                    style.bodySize * 1.3f + style.bodySize * kTableRowPaddingYEm * 2.0f};
        default:
            return {style.bodySize, style.bodyLineHeight};
    }
}

// ── 块首行的上方间距（R1）────────────────────────────────────────────────────
// 只依赖**本行与前两条源行**（不依赖可见性/折叠），增量重建的邻域因此是 ±2。
// 返回 0 = 与上一行首尾相接（LP 的常态：段与段的分离由空源码行自己的高度承担）。
inline float blockSpaceBeforeFor(const LpPlan& plan, std::size_t index,
                                const components::MarkdownStyle& style) {
    const std::vector<LpLine>& lines = plan.lines;
    if (index == 0 || index >= lines.size()) {
        return 0.0f;  // 文档首块的顶端没有外间距（契约"生成规则"第 3 条）
    }
    const LpLine& line = lines[index];
    float gap = 0.0f;

    // ① 标题上置间距：Obsidian `.cm-line.HyperMD-header { padding-top: var(--p-spacing) }`。
    //    唯一例外是同一条 CSS 的第三条规则 —— 「标题 + **恰好一条**空行 + 标题」时
    //    `padding-top: 0`（连续标题不再各自顶开 16px）；空行多于一条就不匹配该规则。
    if (line.kind == LpKind::Heading) {
        const bool blankRightAfterHeading =
            index >= 2 && lines[index - 1].kind == LpKind::Blank &&
            lines[index - 2].kind == LpKind::Heading;
        if (!blankRightAfterHeading) {
            gap = std::max(gap, style.headingSpaceBefore);
        }
    }

    // ② 表格容器的内边距：LP 里表格是 `.cm-embed-block` 部件（实测 `padding: 16px 16px`），
    //    上边距落在**整张表**的首行 —— 语义层把每个表格行都标成 blockFirstLine，
    //    所以只能靠 tableId 认首行（契约"生成规则"第 1 条的表格例外）。
    //    下边距落在表后紧跟的那一行：spaceBefore 只有"行前"这一个自由度，
    //    下边距只能由后一行承接。
    if (line.tableId >= 0 && lines[index - 1].tableId != line.tableId) {
        gap = std::max(gap, style.tableSpaceBefore);
    }
    if (lines[index - 1].tableId >= 0 && line.tableId != lines[index - 1].tableId) {
        gap = std::max(gap, style.tableSpaceBefore);
    }
    return gap;
}

// 一行所属的"块"（用于判断活动块）。代码块用整块区间，其余用扫描器给的块区间。
struct BlockRange {
    int beg = -1;
    int end = -1;
};
inline BlockRange blockRangeFor(const LpLine& line) {
    if (line.kind == LpKind::Code && line.codeBlockBeg >= 0) {
        return {line.codeBlockBeg, line.codeBlockEnd};
    }
    if (line.blockBeg >= 0) {
        return {line.blockBeg, line.blockEnd};
    }
    return {line.srcBeg, line.srcEnd};
}

// ── 行内容的起点（容器标记之后）──────────────────────────────────────────────
// 扫描器给的 conceal 里，从行首开始的那一段就是"容器标记"，它的终点即内容起点。
inline int contentBegOf(const LpLine& line) {
    for (const LpRange& range : line.conceal) {
        if (range.beg == line.srcBeg && range.end > range.beg) {
            return range.end;
        }
        if (range.beg > line.srcBeg) {
            break;
        }
    }
    return line.srcBeg;
}

// ── 行内标记的"露面"判定 ─────────────────────────────────────────────────────
// 片段范围 = 开标记起点 ~ 闭标记终点（含内容）。链接的闭标记覆盖 "](url)"，
// 所以光标进到 URL 里也算"进入该片段"，与 Obsidian 一致。
inline int spanBegOf(const LpSpan& span) {
    int beg = span.content.beg;
    if (span.openMark.beg < span.openMark.end) {
        beg = std::min(beg, span.openMark.beg);
    }
    return beg;
}

inline int spanEndOf(const LpSpan& span) {
    int end = span.content.end;
    if (span.closeMark.beg < span.closeMark.end) {
        end = std::max(end, span.closeMark.end);
    }
    return end;
}

inline bool caretInsideSpan(const LpSpan& span, int cursor) {
    return cursor >= spanBegOf(span) && cursor <= spanEndOf(span);
}

// 把一行的 conceal 按"这段标记属于谁"切开后决定藏不藏，结果追加到 out。
//
// hideBlockMarks = false 表示光标就在这个块里，块级标记要露出来（`# `、`- ` 这些）。
// 行内标记与块级标记可能被 lp_plan 合并成同一段（相邻区间会合并），所以必须**切开**判断，
// 不能整段一起处理 —— 否则 `- **粗**` 里 `- ` 的显隐会被 `**` 的规则带走。
inline void appendLineHoles(const LpLine& line,
                            bool hideBlockMarks,
                            int cursor,
                            std::vector<LineHole>& out) {
    if (line.conceal.empty()) {
        return;
    }
    struct MarkRange {
        int beg = 0;
        int end = 0;
        bool revealed = false;
    };
    std::vector<MarkRange> marks;
    marks.reserve(line.spans.size() * 2);
    for (const LpSpan& span : line.spans) {
        const bool revealed = caretInsideSpan(span, cursor);
        if (span.openMark.beg < span.openMark.end) {
            marks.push_back({span.openMark.beg, span.openMark.end, revealed});
        }
        if (span.closeMark.beg < span.closeMark.end) {
            marks.push_back({span.closeMark.beg, span.closeMark.end, revealed});
        }
    }
    std::sort(marks.begin(), marks.end(), [](const MarkRange& lhs, const MarkRange& rhs) {
        return lhs.beg != rhs.beg ? lhs.beg < rhs.beg : lhs.end < rhs.end;
    });

    for (const LpRange& range : line.conceal) {
        int position = range.beg;
        for (const MarkRange& mark : marks) {
            if (mark.end <= position || mark.beg >= range.end) {
                continue;
            }
            const int markBeg = std::max(mark.beg, position);
            const int markEnd = std::min(mark.end, range.end);
            if (markBeg > position && hideBlockMarks) {
                out.push_back({position, markBeg});  // 标记之前的空隙 = 块级标记，仍然藏
            }
            if (!mark.revealed) {
                out.push_back({markBeg, markEnd});
            }
            position = std::max(position, markEnd);
        }
        if (position < range.end && hideBlockMarks) {
            out.push_back({position, range.end});
        }
    }
}

// Leading quote containers stay structural in Live Preview, including while
// editing their contents. Inline syntax continues to reveal at the caret.
inline int quotePrefixEnd(const std::string& text, const LpLine& line) {
    if (line.srcBeg < 0 || line.srcEnd > static_cast<int>(text.size())) return line.srcBeg;
    int pos = line.srcBeg;
    for (int depth = 0; depth < line.quoteDepth; ++depth) {
        int marker = pos;
        while (marker < line.srcEnd && (text[marker] == ' ' || text[marker] == '\t')) ++marker;
        if (marker >= line.srcEnd || text[marker] != '>') break;
        pos = marker + 1;
        if (pos < line.srcEnd && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    }
    return pos;
}

// ── 行内样式段 ──────────────────────────────────────────────────────────────
// 只给"有可见样式差别"的片段生成段：粗体、斜体、行内代码、链接、删除线、行内公式。
// 强调（`*斜体*`）只在**字体真有斜体字面**时生成段：italicFamily 由
// core::TextPrimitive::resolveItalicFontPath 解析（找不到返回空串），为空就保持原样 ——
// 合成斜体是另一个量级（要动渲染管线），宁可不做（S3d 降级版）。
inline void appendLineRuns(const LpLine& line,
                           const components::MarkdownStyle& style,
                           const std::string& italicFamily,
                           std::vector<LineRun>& out) {
    for (const LpSpan& span : line.spans) {
        if (span.content.beg >= span.content.end) {
            continue;
        }
        LineRun run;
        run.beg = span.content.beg;
        run.end = span.content.end;
        switch (span.kind) {
            case LpSpanKind::Strong:
                run.style.weight = 700;
                break;
            case LpSpanKind::InlineCode:
            case LpSpanKind::Math:
                run.style.fontFamily = style.codeFontFamily;
                run.style.color = style.codeText;
                run.style.background = style.codeBackground;
                break;
            case LpSpanKind::Link:
            case LpSpanKind::Image:
            case LpSpanKind::WikiLink:
                run.style.color = style.accent;
                run.style.link = true;  // 命中测试用：单击跳转 / 右键编辑（2026-09-26）
                run.style.underline = true;
                break;
            case LpSpanKind::Strikethrough:
                run.style.strike = true;
                run.style.color = style.muted;
                break;
            case LpSpanKind::Emphasis:
                if (italicFamily.empty()) {
                    continue;  // 字体没有斜体字面：不生成段，内容保持常规字面
                }
                // 走 fontFamily = 斜体文件路径这条路，测量与渲染拿到的是同一份字面，
                // caret 宽度天然对得上（组件本来就把 run 的 fontFamily 透给两条路径）。
                run.style.fontFamily = italicFamily;
                break;
            case LpSpanKind::Underline:
                run.style.underline = true;
                break;
            default:
                continue;  // 没有可见样式差别：不生成段，让行保持"一个文本图元"的老路径
        }
        out.push_back(std::move(run));
    }
}

// Split at every span boundary: nested styles apply only inside their own range.
// A heading-wide bold span must not extend an inline code background or underline
// to the whole heading. Sweep events instead of rescanning all spans per boundary.
inline void mergeRuns(std::vector<LineRun>& runs) {
    if (runs.size() < 2) {
        return;
    }
    std::sort(runs.begin(), runs.end(), [](const LineRun& lhs, const LineRun& rhs) {
        return lhs.beg != rhs.beg ? lhs.beg < rhs.beg : lhs.end < rhs.end;
    });
    struct Event { int offset; std::size_t index; bool begin; };
    std::vector<Event> events;
    events.reserve(runs.size() * 2);
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].empty()) continue;
        events.push_back({runs[i].beg, i, true});
        events.push_back({runs[i].end, i, false});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.offset < b.offset;
    });
    std::vector<LineRun> merged;
    merged.reserve(events.size());
    std::set<std::size_t> active;
    std::size_t event = 0;
    while (event < events.size()) {
        const int beg = events[event].offset;
        while (event < events.size() && events[event].offset == beg) {
            if (events[event].begin) active.insert(events[event].index);
            else active.erase(events[event].index);
            ++event;
        }
        if (active.empty() || event == events.size()) continue;
        LineRun segment;
        segment.beg = beg;
        segment.end = events[event].offset;
        for (const std::size_t i : active) {
            const auto& style = runs[i].style;
            segment.style.weight = std::max(segment.style.weight, style.weight);
            if (segment.style.color.a == 0.0f) segment.style.color = style.color;
            if (segment.style.background.a == 0.0f) segment.style.background = style.background;
            if (segment.style.fontFamily.empty()) segment.style.fontFamily = style.fontFamily;
            segment.style.strike |= style.strike;
            segment.style.link |= style.link;
            segment.style.underline |= style.underline;
        }
        if (!merged.empty() && merged.back().end == segment.beg &&
            merged.back().style == segment.style) merged.back().end = segment.end;
        else merged.push_back(std::move(segment));
    }
    runs.swap(merged);
}

// 行级矩形：代码块底色 / 引用竖条（S3b）。
//
// 代码块的底色**连围栏行一起涂**：围栏行本身被整行折叠（没有文字），它留下的那个行高
// 正好当上下内边距 —— 所以不需要另做一套"块间距"机制，几何层一行都不用改。
// 块边界按 `codeBlockBeg` 判（开/闭围栏行也带这个值），这样两个紧贴的代码块不会被连成一块。
inline void appendTableBox(const LpPlan& plan,
                           std::size_t index,
                           const components::MarkdownStyle& style,
                           LineDecoration& decoration);

inline void appendLineBox(const LpPlan& plan,
                          std::size_t index,
                          const components::MarkdownStyle& style,
                          LineDecoration& decoration) {
    const LpLine& line = plan.lines[index];
    if (line.kind == LpKind::Code && line.codeBlockBeg >= 0) {
        decoration.box.background = style.codeBackground;
        decoration.box.backgroundRadius = style.radius;
        decoration.box.backgroundBlockFirst =
            index == 0 || plan.lines[index - 1].codeBlockBeg != line.codeBlockBeg;
        decoration.box.backgroundBlockLast =
            index + 1 >= plan.lines.size() || plan.lines[index + 1].codeBlockBeg != line.codeBlockBeg;
        // 代码文字从背景条左缘让开一个 em（Obsidian 的 16px 左内边距，规格表 §2）。
        // 缩进不占源文字节，走装饰层的 contentIndent 平移机制。
        decoration.contentIndent = style.bodySize;
    }
    // frontmatter：整段连成一个弱化小块，与代码块共用同一套矩形机制（底色 + 首末圆角）。
    // 块首/块末只能按"相邻行还是不是 frontmatter"判 —— 它没有 codeBlockBeg 那样的区间字段。
    if (line.kind == LpKind::Frontmatter) {
        decoration.box.background = style.codeBackground;
        decoration.box.backgroundRadius = style.radius;
        decoration.box.backgroundBlockFirst =
            index == 0 || plan.lines[index - 1].kind != LpKind::Frontmatter;
        decoration.box.backgroundBlockLast =
            index + 1 >= plan.lines.size() || plan.lines[index + 1].kind != LpKind::Frontmatter;
        decoration.contentIndent = style.bodySize;
    }
    if (line.quoteDepth > 0) {
        // Bars start at the content column; each nested bar gets its own slot.
        // Reserve that space in layout so wrapping, caret and hit testing agree.
        // 2px borders, 0.75em first padding and 1.2em nesting match the local
        // Obsidian Live Preview reference at 16px text and 125% system DPI.
        decoration.box.barColor = style.accent;
        decoration.box.barWidth = 2.0f;
        decoration.box.barCount = static_cast<unsigned char>(std::min(line.quoteDepth, 4));
        decoration.contentIndent += style.bodySize *
            (0.75f + (decoration.box.barCount - 1) * 1.2f);
    }
    appendTableBox(plan, index, style, decoration);
}

// ── 表格（S3f 批次 D；R3 2026-09-26 对齐 Obsidian 实测）─────────────────────
// Obsidian 的表格是"纯网格 + 无底色"：`border-collapse: collapse` + 每个单元格
// 四边 1px `--table-border-color`（= divider 色），**表头没有底色**（transparent）、
// 表头只是字重 600 + 正文字色，正文也没有斑马纹。所以本层不再给表格行任何
// 底色（box.background 留透明），只提供：
//   · `gridColor`（1px 网格线，渲染层画）；
//   · 分隔行的 1px 横条当表头下边框（折叠边框下就是一根 1px）；
//   · 单元格上下内边距（撑高行盒 + textShiftY 把文字跟着下移）。
// 早期版本用"表头 accent 16% 带 + 3px 分隔条 + 正文带"三段落差来暗示表格结构，
// 那是逐格网格线还没有的时候；网格线落地后它反而把 Obsidian 的观感盖掉了。

// 表头下边框 = 分隔行自己的高度（折叠边框下相邻两格只留一根 1px）。
inline constexpr float kTableSeparatorHeight = 1.0f;

inline void appendTableBox(const LpPlan& plan,
                           std::size_t index,
                           const components::MarkdownStyle& style,
                           LineDecoration& decoration) {
    const LpLine& line = plan.lines[index];
    if (line.tableId < 0) {
        return;
    }
    decoration.box.backgroundRadius = 0.0f;
    decoration.box.gridColor = style.divider;
    if (line.tableSeparator) {
        // 分隔行 = 表头下边框：1px 横条，竖线照穿（渲染层在分隔行不另画横线）。
        decoration.lineHeight = kTableSeparatorHeight;
        decoration.box.background = style.divider;
        decoration.box.backgroundBlockFirst = true;
        decoration.box.backgroundBlockLast = true;
        return;
    }
    // 单元格底色透明：不置 background，渲染层的底色带合并逻辑就不会画这一行。
    decoration.textShiftY = style.bodySize * kTableRowPaddingYEm;
    if (line.tableHeaderRow) {
        // Obsidian 表头：无底色 + 正文字色 + 字重 600（规格表 §1；字重由装饰层
        // 加的那条 weight=600 的段承担，见 buildDecorationForLine）。
        decoration.textColor = style.text;
    }
}

// ── 块级图片（S3f 批次 B）───────────────────────────────────────────────────
// 独占一行的 ![alt](src) 在**非活动块**时整行换成真图；活动块回到源码 —— 与链接
// 同一套活动规则。v1 范围（规划 §3）：只画本地可解析的位图，混排行 / 远程 /
// SVG / 头解析失败全部维持现状的文本，不比今天差。
//
// 尺寸盒取 ZCode markdown-image 的实测值（规划 §8.2）：宽 ≤ max-w-md(448)、
// 高 ≤ max-h-90(360)、小图不放大（max-* 语义），等比 contain。
inline constexpr float kImageMaxWidth = 448.0f;
inline constexpr float kImageMaxHeight = 360.0f;
// 图片行盒在图上下的留白（ZCode my-4 的观感档）。
inline constexpr float kImageVPad = 16.0f;
inline constexpr float kImageRadius = 12.0f;  // rounded-xl
// 图片失败态占位盒（S3f 批次 F）：ZCode markdown-image 失败态的 md 规格
// （w-56 × h-44 = 224×176），图标 + 路径文字由组件按这组尺寸画。
inline constexpr float kImageFailWidth = 224.0f;
inline constexpr float kImageFailHeight = 176.0f;

inline bool imageSrcIsRemote(const std::string& raw) {
    std::string head = raw.substr(0, 8);
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0;
}

// src → 本地绝对路径；远程 / 不存在 → 空（调用方退回文本占位）。
//
// exists + weakly_canonical 的记忆化（T7）在 image_size.h 的 resolveImagePathMemo：
// 键 = UTF-8 的 (src, docDir)，命中先复核源路径目标的 (mtime, size)，源路径不存在 /
// weakly_canonical 失败一律**不入表**（零负缓存：missing→appears 当场解析出来，
// "解析失败"也绝不会永久化）。u8path 那条 ANSI 代码页的坑同样在那里挡着 —— 入参是
// UTF-8，窄串构造会按 GBK 解码，目录名"参考"这类字节会直接抛 system_error。
// 本函数只留 lp 侧的输入口径：空串与远程地址不属于"本地路径解析"，不下沉。
inline std::string resolveLocalImageSrc(const std::string& raw, const std::string& docDir) {
    if (raw.empty() || imageSrcIsRemote(raw)) {
        return {};
    }
    return resolveImagePathMemo(raw, docDir);
}

// 失败盒里显示的路径：太长时保留尾部（文件名通常在后），按 UTF-8 边界截。
// 宽度用"中文 1em / ASCII 0.55em"估算 —— 占位盒够用，不做真实 shaping。
inline std::string imageFailDisplayText(const std::string& src, float maxWidth, float fontSize) {
    const auto charWidth = [&](unsigned char lead) {
        if (lead < 0x80) {
            return fontSize * 0.55f;
        }
        if ((lead & 0xE0) == 0xC0) {
            return fontSize * 0.8f;
        }
        return fontSize;
    };
    float width = 0.0f;
    std::size_t cut = src.size();
    while (cut > 0) {
        // 从尾部往回找 UTF-8 lead byte；中断序列按最坏宽度算。
        std::size_t begin = cut - 1;
        while (begin > 0 && (static_cast<unsigned char>(src[begin]) & 0xC0) == 0x80) {
            --begin;
        }
        const float w = charWidth(static_cast<unsigned char>(src[begin]));
        if (width + w > maxWidth) {
            break;
        }
        width += w;
        cut = begin;
    }
    if (cut == 0) {
        return src;
    }
    return "…" + src.substr(cut);
}

// 生成编辑区要的逐行装饰。cursor 决定哪一块 / 哪个行内片段保持"源码原样"。
// fontFamily 是编辑区当前字体（空 = 控件默认）：斜体字面由它解析出来，只算一次。
// docDir 是文档所在目录（相对图片路径的基准；空 = 用进程 cwd，测试用）。
// foldedHeadings = 折叠中的标题起始字节偏移（S3f 批次 C；空 = 一张不折叠）。
// ── 折叠标记的判定（S3f 批次 C / T4 B）────────────────────────────────────
// 沿标题父链向上走：**任何一层**被折叠都隐藏（嵌套折叠的展开顺序天然正确）；
// 标题自己（含 setext 下划线）不因自己被折叠而隐藏。预览横条按**最外层**被折叠
// 祖先分配，每个折叠章节只出一条（章节体的第一条非空行）—— claim 写进 previewOwners。
//
// 抽成函数的理由（T4）：增量重建要按全量语义**播种** previewOwners —— 区间外被复用
// 的行也得把它那一条 claim 记下来，否则区间内重算的行会重复出预览横条。
inline void foldLineFlags(const LpPlan& plan,
                          std::size_t lineIndex,
                          const std::set<int>* foldedHeadings,
                          std::set<int>& previewOwners,
                          bool& hidden,
                          int& outermostFolded,
                          bool& preview) {
    hidden = false;
    outermostFolded = -1;
    preview = false;
    if (foldedHeadings == nullptr || foldedHeadings->empty()) {
        return;
    }
    const LpLine& line = plan.lines[lineIndex];
    const bool ownUnderline = line.kind == LpKind::Heading && line.setext && lineIndex > 0 &&
                              plan.lines[lineIndex - 1].kind == LpKind::Heading &&
                              plan.lines[lineIndex - 1].setext;
    int owner = line.sectionHeadingLine;
    while (owner >= 0) {
        const LpLine& head = plan.lines[static_cast<std::size_t>(owner)];
        const bool headIsOwnPair = ownUnderline && owner + 1 == static_cast<int>(lineIndex);
        if (!headIsOwnPair && foldedHeadings->count(head.srcBeg) > 0) {
            hidden = true;
            outermostFolded = owner;  // 不 break：预览按最外层被折叠祖先分配
        }
        owner = head.parentHeadingLine;
    }
    if (hidden && line.kind != LpKind::Blank && outermostFolded >= 0) {
        preview = previewOwners.insert(outermostFolded).second;
    }
}

// ── 单行装饰的生成上下文（T4 B）────────────────────────────────────────────
// 把"整表循环"里跨行共享的东西收进一个结构，让增量路径能按**同一套规则**只重建
// 其中一段：语法状态机（SyntaxState/codeLang）与预览 claim（previewOwners）都沿
// 行序续跑，重建段从开围栏/文档头起步即可与全量逐字节一致。
struct DecorationBuildContext {
    const LpPlan* plan = nullptr;
    const components::MarkdownStyle* style = nullptr;
    int cursor = -1;
    int cursorLine = -1;
    BlockRange active;
    bool hasActive = false;
    std::string italicFamily;
    const std::string* text = nullptr;
    const EditorColors* colors = nullptr;
    const std::string* docDir = nullptr;
    const std::set<int>* foldedHeadings = nullptr;
    // 跨行状态（沿行走；重算段开始前必须复位，见 buildDecorationsIncremental）。
    SyntaxState syntaxState;
    std::string codeLang;
    std::set<int> previewOwners;
};

// 活动块（光标所在块）在整表循环开始前算一次：块切换决定哪些行露出源码标记。
inline void prepareDecorationContext(DecorationBuildContext& ctx) {
    const LpPlan& plan = *ctx.plan;
    ctx.cursorLine = plan.lineIndexFor(ctx.cursor);
    if (ctx.cursorLine >= 0 && ctx.cursorLine < static_cast<int>(plan.lines.size())) {
        ctx.active = blockRangeFor(plan.lines[static_cast<std::size_t>(ctx.cursorLine)]);
    }
    ctx.hasActive = ctx.active.beg >= 0 && ctx.active.end >= ctx.active.beg;
}

// 生成一行的装饰。局部引用把 ctx 摊平成与原整表循环逐字同名的变量，函数体因此
// 与改造前**一字不差**（全量与增量共用同一条路，避免两处各写一份慢慢漂移）。
// 注意 ctx 按**非 const** 传：语法状态机与预览 claim 是沿行序推进的跨行状态。
inline void buildDecorationForLine(DecorationBuildContext& ctx,
                                   std::size_t lineIndex,
                                   LineDecoration& decoration) {
    const LpPlan& plan = *ctx.plan;
    const components::MarkdownStyle& style = *ctx.style;
    const int cursor = ctx.cursor;
    const int cursorLine = ctx.cursorLine;
    const BlockRange& active = ctx.active;
    const bool hasActive = ctx.hasActive;
    const std::string& italicFamily = ctx.italicFamily;
    const std::string& text = *ctx.text;
    const EditorColors* colors = ctx.colors;
    const std::string& docDir = *ctx.docDir;
    const std::set<int>* foldedHeadings = ctx.foldedHeadings;
    SyntaxState& syntaxState = ctx.syntaxState;
    std::string& codeLang = ctx.codeLang;
    std::set<int>& previewOwners = ctx.previewOwners;

    const LpLine& line = plan.lines[lineIndex];
    const LineSize size = lineSizeFor(line, style);
        decoration.fontSize = size.fontSize;
        decoration.lineHeight = size.lineHeight;
        // 块间距（R1）：LP 的块间距只出现在标题行与表格容器的首/末两侧，
        // 其余位置由空源码行自己的高度承担（见 blockSpaceBeforeFor 的推导）。
        decoration.spaceBefore = blockSpaceBeforeFor(plan, lineIndex, style);

        switch (line.kind) {
            case LpKind::Heading:
                decoration.textColor = line.headingLevel == 6 ? style.muted : style.heading;
                // 标题的 1em 上置间距自 R1 起改由 decoration.spaceBefore 承担：
                // 行盒回到纯文字高度（1.2/1.3 × 字号），不再靠 textShiftY 把文字压到盒底。
                break;
            case LpKind::Code:
                // 代码块整行走等宽字面（行级字体，S3f 批次 J）：正文用的自选字体多为
                // 比例字体，字符宽窄不一，代码的缩进与对齐全被打破（极端时视觉重叠）。
                // 语法 token 着色段没有自己的字体，渲染时继承这一份（见 input.h）。
                decoration.fontFamily = style.codeFontFamily;
                decoration.textColor = style.codeText;
                // 起始围栏行右上角的语言标签（Obsidian LP 的 "JavaScript"，见规格表 §2）。
                // 只认**起始**围栏（srcBeg == codeBlockBeg）；结束围栏与内容行都不给。
                if (line.codeFence && line.srcBeg == line.codeBlockBeg) {
                    decoration.languageLabel = codeLanguageLabel(line.codeLang);
                }
                break;
            case LpKind::Frontmatter:
                // frontmatter 与代码块同一档：等宽 + 元数据弱化色。
                decoration.fontFamily = style.codeFontFamily;
                // 弱化成次要文字色：元数据不该跟正文抢注意力（同 Obsidian 的"属性"观感）。
                decoration.textColor = style.muted;
                break;
            default:
                break;  // 其余沿用控件默认文字色
        }

        const BlockRange range = blockRangeFor(line);
        const bool isActiveBlock = hasActive && range.beg == active.beg && range.end == active.end;
        appendLineHoles(line, !isActiveBlock, cursor, decoration.holes);
        const int quoteEnd = quotePrefixEnd(text, line);
        if (quoteEnd > line.srcBeg) decoration.holes.push_back({line.srcBeg, quoteEnd});

        appendLineRuns(line, style, italicFamily, decoration.runs);
        if (line.kind == LpKind::Heading) {
            // 标题整体加粗（与 Obsidian 一致）。用"整行一段"表达，标题里的 `**` 由 mergeRuns 合并。
            const int contentBeg = contentBegOf(line);
            if (contentBeg < line.srcEnd) {
                LineRun heading;
                heading.beg = contentBeg;
                heading.end = line.srcEnd;
                heading.style.weight = 700;
                decoration.runs.push_back(std::move(heading));
            }
        }
        if (line.tableId >= 0 && line.tableHeaderRow && !line.cells.empty()) {
            // 表头字重 600（Obsidian --table-header-weight，规格表 §1）。管道在洞里，
            // 段经 buildVisiblePieces 按格切开后只落在可见文字上。
            LineRun headerRun;
            headerRun.beg = line.srcBeg;
            headerRun.end = line.srcEnd;
            headerRun.style.weight = 600;
            decoration.runs.push_back(std::move(headerRun));
        }
        // 代码块语法着色（2026-09-25，规格表 §4）：只给围栏内容行上色；活动块
        // （源码露出）也照常上色 —— token 段按字节区间走，与洞互不干扰。
        if (line.kind == LpKind::Code) {
            if (line.codeFence) {
                codeLang = line.codeLang;
                syntaxState = SyntaxState{};
            } else if (colors != nullptr && !text.empty()) {
                std::vector<SyntaxToken> tokens;
                tokenizeCodeLine(text, line.srcBeg, line.srcEnd, codeLang, syntaxState, tokens);
                decoration.runs.reserve(decoration.runs.size() + tokens.size());
                for (const SyntaxToken& token : tokens) {
                    LineRun run;
                    run.beg = std::max(token.beg, line.srcBeg);
                    run.end = std::min(token.end, line.srcEnd);
                    if (run.empty()) {
                        continue;
                    }
                    switch (token.kind) {
                        case TokenKind::Keyword:
                            run.style.color = colors->tokenKeyword;
                            break;
                        case TokenKind::String:
                            run.style.color = colors->tokenString;
                            break;
                        case TokenKind::Number:
                            run.style.color = colors->tokenNumber;
                            break;
                        case TokenKind::Comment:
                            run.style.color = colors->tokenComment;
                            break;
                        case TokenKind::Operator:
                            run.style.color = colors->tokenOperator;
                            break;
                        case TokenKind::Function:
                            run.style.color = colors->tokenFunction;
                            break;
                        case TokenKind::Property:
                            run.style.color = colors->tokenProperty;
                            break;
                        case TokenKind::Plain:
                            continue;  // 普通标识符 = 行级 codeText，不加段
                    }
                    decoration.runs.push_back(std::move(run));
                }
            }
        }
        appendLineBox(plan, lineIndex, style, decoration);
        // The projected text is empty; draw a rule only while its source is concealed.
        // The active block shows editable Markdown markers instead.
        if (line.kind == LpKind::Divider && !isActiveBlock) {
            decoration.box.gridColor = style.divider;
            decoration.box.horizontalRuleThickness = 2;
        }

        // 表格（S3f 批次 D）：把格区间交给组件做列吸附，并把**管道与两侧空白**补成
        // "永远隐藏"的洞。为什么这步在装饰层而不是计划层（计划层只给 cells）：
        // 计划层的 conceal 会被"光标所在块露出源码"整段露出来，而管道一露出来，投影
        // 文本就多出字节、列吸附的坐标系当场崩掉。这里补的洞不参与活动块判定，
        // 任何情况下都藏 —— 于是"进格编辑只看得到格内容"，与 Obsidian 一致。
        if (line.tableId >= 0) {
            decoration.tableId = line.tableId;
            decoration.tableHeaderRow = line.tableHeaderRow;
            decoration.tableSeparator = line.tableSeparator;
            // 单元格内边距不在这里给：组件的 em 基准（0.86em ≈ ZCode 的 12px@14px）随
            // 表格字号缩放，比钉死一个像素值更稳（见 kTableCellPaddingEm）。
            if (line.tableSeparator) {
                decoration.holes.push_back({line.srcBeg, line.srcEnd});
            } else if (!line.cells.empty()) {
                decoration.cells.reserve(line.cells.size());
                for (std::size_t cellIndex = 0; cellIndex < line.cells.size(); ++cellIndex) {
                    const LpRange& cell = line.cells[cellIndex];
                    components::input_detail::LineCell out;
                    out.beg = std::max(cell.beg, line.srcBeg);
                    out.end = std::min(std::max(cell.end, cell.beg), line.srcEnd);
                    // 对齐方式（S3f 批次 E）：计划层从分隔行的 `:---:` 接来的 MD_ALIGN 值。
                    if (cellIndex < line.cellAligns.size()) {
                        out.align = line.cellAligns[cellIndex];
                    }
                    decoration.cells.push_back(out);
                }
                int cursor = line.srcBeg;
                for (const components::input_detail::LineCell& cell : decoration.cells) {
                    decoration.holes.push_back({cursor, std::max(cursor, cell.beg)});
                    cursor = std::max(cursor, cell.end);
                }
                decoration.holes.push_back({cursor, line.srcEnd});
            }
        }

        // 任务复选框：替换被隐藏的 `- [ ] `（文字已由组件按 glyph.advance 右移）。
        // 点击切换走组件的 onPointerHit 回调（S3f 批次 A，见 editor_view）。
        if (line.kind == LpKind::TaskItem) {
            decoration.glyph.checkbox = true;
            decoration.glyph.checked = line.taskChecked;
            decoration.glyph.codepoint = line.taskChecked ? kTaskCheckedIcon : kTaskIcon;
            decoration.glyph.color = line.taskChecked ? style.accent : style.muted;
            // A checkbox replaces the task prefix even on the active line.
            const int content = line.contentBeg >= 0 ? line.contentBeg : contentBegOf(line);
            decoration.holes.push_back({line.srcBeg, content});
            int marker = quoteEnd;
            while (marker >= 0 && marker < line.srcEnd && marker < static_cast<int>(text.size()) &&
                   (text[marker] == ' ' || text[marker] == '\t')) ++marker;
            if (marker > quoteEnd) {
                decoration.listIndentBeg = quoteEnd;
                decoration.listIndentEnd = marker;
            }
            if (line.taskChecked && content < line.srcEnd) {
                LineRun completed;
                completed.beg = content;
                completed.end = line.srcEnd;
                decoration.textColor = style.muted;
                completed.style.strike = true;
                decoration.runs.push_back(std::move(completed));
            }
        }

        // ── 列表 marker（C1）────────────────────────────────────────────────
        // 非活动块的列表项首行在隐藏的标记洞位画呈现标记：无序 = "•"、有序 =
        // 组内呈现序号 "3."（不是源码字面数字，见 lp_plan 的 listOrdinal）。
        // 图元 advance 不在这里定 —— 组件用 [listIndentBeg, listIndentEnd) 的源文
        // 实测正文列宽充当 advance（测量 / wrapWidth / caretX / 命中走同一个数，
        // 且随 DPI 缩放），marker 文本在洞里右对齐，正文列与源码列一致。
        // 任务项只保留复选框机制（不叠画 marker）。
        // 活动块（源码露出）不画 marker，也不给续行缩进 —— 源码态回到原样。
        // marker 只在标记行画一次（软换行续段共享 advance 平移但不重画，见 input.h）。
        if (!isActiveBlock && line.listItemLine >= 0) {
            const bool markerLine = static_cast<int>(lineIndex) == line.listItemLine;
            if (markerLine && line.kind == LpKind::ListItem && contentBegOf(line) > line.srcBeg) {
                decoration.glyph.codepoint = 0;
                decoration.glyph.text = line.ordered ? std::to_string(line.listOrdinal) + "."
                                                     : "•";
                decoration.glyph.color = style.muted;
                decoration.listIndentBeg = quoteEnd;
                decoration.listIndentEnd = line.contentBeg >= 0 ? line.contentBeg : contentBegOf(line);
            } else if (!markerLine && line.kind == LpKind::Text) {
                // 物理续行（缩进续段 / lazy 续段）：与所属项的正文起点对齐。
                const LpLine* owner = plan.lineAt(line.listItemLine);
                if (owner != nullptr && owner->quoteDepth == line.quoteDepth && owner->kind == LpKind::TaskItem) {
                    // 任务项的正文起点 = 复选框图元 advance（与 glyphAdvanceOf 的
                    // 默认公式同一份字号与常量；owner 行的字号就是正文档）。
                    decoration.contentIndent +=
                        std::max(4.0f, size.fontSize * components::input_detail::kGlyphAdvanceEm);
                } else if (owner != nullptr && owner->quoteDepth == line.quoteDepth && owner->kind == LpKind::ListItem &&
                           owner->contentBeg > owner->srcBeg) {
                    // 普通项：续行缩进 = owner 隐藏前缀的实测宽（与 owner 图元
                    // advance 是同一个数，两端天然对齐）。
                    decoration.listIndentBeg = quotePrefixEnd(text, *owner);
                    decoration.listIndentEnd = owner->contentBeg;
                }
            }
        }

        // 折叠箭头（2026-09-25，对齐 Obsidian）：标题**起始行**的行号位画 chevron，
        // 指向随折叠状态变——右箭头 = 已折叠，下箭头 = 展开。点行号/箭头折叠走
        // editorView 既有的 onGutter 命中（折叠判定条件与这里一致：起始行才可折）。
        // 光标所在行是折叠内容时会被强制展开，箭头随 foldedHeadings 的真实状态走。
        if (line.kind == LpKind::Heading && plan.foldHeadingBegFor(line.srcBeg) == line.srcBeg) {
            const bool folded = foldedHeadings != nullptr &&
                                foldedHeadings->count(line.srcBeg) > 0;
            decoration.gutterGlyph.codepoint = folded ? kFoldChevronRight : kFoldChevronDown;
            decoration.gutterGlyph.color = style.muted;
        }

        // 块级图片（S3f 批次 B）：非活动块的纯图行整行藏掉、原位画真图。
        // 行高换成"图高 + 上下留白"——几何表逐行吃 decoration.lineHeight，
        // 滚动条 / 命中 / 光标上下移动全部自动跟上，组件一行都不用改。
        // 活动块（光标进来了）不生成图片槽：源码显示由既有的 holes 机制负责。
        // 本地路径解析失败 / 文件缺失 / 头解析不出宽高 → 失败态占位盒（S3f 批次 F，
        // ZCode markdown-image 同规格：224×176 + 图标 + 路径），仍然"不比纯文本差"；
        // 远程 URL 维持 v1 的文本原样（渲染远程图不在本批范围）。
        if (!isActiveBlock && !line.pureImageSrc.empty()) {
            const std::string resolved = resolveLocalImageSrc(line.pureImageSrc, docDir);
            std::optional<ImageExtent> extent;
            if (!resolved.empty()) {
                extent = readImageExtent(resolved);
            }
            if (extent.has_value() && extent->width > 0 && extent->height > 0) {
                const float scale = std::min(
                    {1.0f,
                     kImageMaxWidth / static_cast<float>(extent->width),
                     kImageMaxHeight / static_cast<float>(extent->height)});
                decoration.imagePath = resolved;
                decoration.imageWidth = static_cast<float>(extent->width) * scale;
                decoration.imageHeight = static_cast<float>(extent->height) * scale;
                decoration.lineHeight = decoration.imageHeight + kImageVPad;
                // 整行源文藏进洞里（与块级标记的洞一起被 normalizeHoles 合并）。
                decoration.holes.push_back({line.srcBeg, line.srcEnd});
            } else if (!imageSrcIsRemote(line.pureImageSrc)) {
                decoration.imageFailed = true;
                decoration.imageWidth = kImageFailWidth;
                decoration.imageHeight = kImageFailHeight;
                decoration.lineHeight = decoration.imageHeight + kImageVPad;
                decoration.holes.push_back({line.srcBeg, line.srcEnd});
                // 路径文字按失败盒的内宽预算（左右各 12px 内边距），字号 = 正文 0.8 档。
                const float pathFontSize = size.fontSize * 0.8f;
                decoration.imageFailText = imageFailDisplayText(
                    line.pureImageSrc, decoration.imageWidth - 24.0f, pathFontSize);
            }
        }

        // ── 折叠（S3f 批次 C）── folded 集合里的标题，其章节体整段 hidden。
        // 沿标题父链向上走：**任何一层**被折叠都隐藏（嵌套折叠的展开顺序天然正确）；
        // 标题自己（含 setext 下划线）不因自己被折叠而隐藏；光标所在行强制可见 ——
        // 不变量"光标永不在折叠行上"的最后一道防线（撤销恢复到折叠区等罕见路径）。
        //
        // 带预览的横条（S3f 批次 G）：每个被折叠的章节，其**第一条非空行**不再藏掉，
        // 改画成"弱化文字 + 左侧 2px 竖条"的预览横条（Obsidian 折叠段观的感的替代）。
        // 预览行 hiddenByFold 保持 true：自愈检查（组件 onRevealHiddenLine）把它当
        // 折叠内容——光标落上去（点它 / ↓ 经过）自动展开整个章节。按**最外层**被折叠
        // 祖先分配预览：外层折叠时内层章节不再出自己的预览条。
        if (foldedHeadings != nullptr && !foldedHeadings->empty()) {
            // 判定与 claim 抽到 foldLineFlags（T4 B）：增量重建要按同一套语义播种。
            bool hidden = false;
            int outermostFolded = -1;
            bool preview = false;
            foldLineFlags(plan, lineIndex, foldedHeadings, previewOwners, hidden, outermostFolded,
                          preview);
            // hiddenByFold 记"折叠致隐"的原始值（预览行也是折叠内容）：光标行强制可见
            // 只放开 hidden，组件的 onRevealHiddenLine 自愈检查靠 hiddenByFold 识别
            // "落进了折叠段"。
            decoration.hiddenByFold = hidden;
            if (preview) {
                decoration.hidden = false;
                decoration.textColor = style.muted;
                decoration.box.barColor = style.muted;
                decoration.box.barWidth = 2.0f;
                decoration.box.barCount = 0;  // 预览横条恒单条（多条是嵌套引用的语义）
            } else if (static_cast<int>(lineIndex) == cursorLine) {
                decoration.hidden = false;
            } else {
                decoration.hidden = hidden;
            }
        }
        mergeRuns(decoration.runs);

}

// 生成整张装饰表（全量路径）。语义与改造前逐字节一致：逐行调 buildDecorationForLine。
inline void buildDecorations(const LpPlan& plan,
                             const components::MarkdownStyle& style,
                             int cursor,
                             std::vector<LineDecoration>& out,
                             const std::string& fontFamily = {},
                             const std::string& docDir = {},
                             const std::set<int>* foldedHeadings = nullptr,
                             const std::string& text = {},
                             const EditorColors* colors = nullptr) {
    out.clear();
    out.reserve(plan.lines.size());

    // 代码块语法着色（lp_syntax.h）：状态机沿着行序走，围栏行重置。
    // colors 为空（单测等场景）或没给 text 时完全不上色，行为与之前一致。
    DecorationBuildContext ctx;
    ctx.plan = &plan;
    ctx.style = &style;
    ctx.cursor = cursor;
    ctx.text = &text;
    ctx.colors = colors;
    ctx.docDir = &docDir;
    ctx.foldedHeadings = foldedHeadings;
    ctx.italicFamily = core::TextPrimitive::resolveItalicFontPath(fontFamily);
    prepareDecorationContext(ctx);

    // 用下标循环而不是 range-for：块首/块末判定要看相邻行（见 appendLineBox）。
    for (std::size_t lineIndex = 0; lineIndex < plan.lines.size(); ++lineIndex) {
        out.emplace_back();
        buildDecorationForLine(ctx, lineIndex, out.back());
    }
}

// ── 装饰的绝对偏移平移（T4 B5）────────────────────────────────────────────
// LineDecoration 里**指向文档字节**的字段全集（穷尽后集中在这一个 helper，不许散落加法）：
//   · holes  —— 要隐藏的字节区间；
//   · runs   —— 行内样式段的内容区间；
//   · cells  —— 表格单元格的内容区间（align 不是偏移，不动）；
//   · tableId —— 表格的起始字节偏移（-1 = 非表格行）；
//   · listIndentBeg/End —— 列表正文缩进的度量源区间（C1；-1 = 无）。
// 明确不动的字段：textShiftY / spaceBefore / contentIndent / glyph / gutterGlyph / box / image*
// (imagePath 是文件系统路径) / fontFamily / lineHeight / fontSize / hidden* —— 与字节无关。
// glyph.text 是呈现文本（"•" / 序号），不是文档切片，同样不动。
inline void translateDecoration(LineDecoration& decoration, int delta) {
    if (delta == 0) {
        return;
    }
    for (LineHole& hole : decoration.holes) {
        hole.beg += delta;
        hole.end += delta;
    }
    for (LineRun& run : decoration.runs) {
        run.beg += delta;
        run.end += delta;
    }
    for (LineCell& cell : decoration.cells) {
        cell.beg += delta;
        cell.end += delta;
    }
    if (decoration.tableId >= 0) {
        decoration.tableId += delta;
    }
    if (decoration.listIndentBeg >= 0) {
        decoration.listIndentBeg += delta;
    }
    if (decoration.listIndentEnd >= 0) {
        decoration.listIndentEnd += delta;
    }
}

// 编辑区间的"引用映射"：把旧计划里的字节/行号引用折算到新坐标。
//   · 前缀里的引用（编辑点之前）原样保留 —— 它们一个字节/一行都没动；
//   · 后缀里的引用整体平移；
//   · 跨在被替换区间里的引用无法对应 → 返回哨兵 -2（必然与 rhs 不等 → 全量）。
// 这套规则同时覆盖 head/tail 两侧，不需要"按侧切换 delta"——那正是漏掉
// "指向编辑点之前的引用不平移"这个坑的根源（sectionHeadingLine 曾因此误判）。
struct EditRefMap {
    int byteBeg = 0;
    int oldEnd = 0;
    int deltaBytes = 0;
    int first = 0;
    int oldTail = 0;
    int deltaLines = 0;

    int mapByte(int value) const {
        if (value < 0) return value;  // 哨兵（-1 = 无此字段）
        if (value < byteBeg) return value;
        if (value >= oldEnd) return value + deltaBytes;
        return -2;
    }

    int mapLine(int value) const {
        if (value < 0) return value;
        if (value < first) return value;
        if (value >= oldTail) return value + deltaLines;
        return -2;
    }
};

// 两代 LpLine 的"结构等价 up to 平移"：除绝对偏移按 EditRefMap 折算外必须逐字段相同。
// 任何一项对不上 = 这次编辑改到了别处的结构（围栏、列表、标题链、表格…），
// 此时无法证明区间外的行还等价 → 调用方直接全量重建（T4 §9：正确性优先）。
inline bool linesEquivalentUpToShift(const LpLine& lhs,
                                     const LpLine& rhs,
                                     const EditRefMap& map,
                                     std::string& fieldOut) {
    const auto sameRange = [&map](const LpRange& a, const LpRange& b) {
        return map.mapByte(a.beg) == b.beg && map.mapByte(a.end) == b.end;
    };
    const auto fail = [&fieldOut](const char* field) {
        fieldOut = field;
        return false;
    };
    if (lhs.kind != rhs.kind || lhs.headingLevel != rhs.headingLevel || lhs.setext != rhs.setext ||
        lhs.quoteDepth != rhs.quoteDepth || lhs.listDepth != rhs.listDepth ||
        lhs.ordered != rhs.ordered || lhs.orderedNumber != rhs.orderedNumber ||
        lhs.listOrdinal != rhs.listOrdinal ||
        lhs.task != rhs.task || lhs.taskChecked != rhs.taskChecked ||
        lhs.codeFence != rhs.codeFence || lhs.codeLang != rhs.codeLang ||
        lhs.blockFirstLine != rhs.blockFirstLine || lhs.blockLastLine != rhs.blockLastLine ||
        lhs.pureImageSrc != rhs.pureImageSrc || lhs.cellAligns != rhs.cellAligns ||
        lhs.tableHeaderRow != rhs.tableHeaderRow || lhs.tableSeparator != rhs.tableSeparator) {
        return fail("identity(kind/层级/容器/围栏/块首末…)");
    }
    // 列表归属（C1）：续行的 owner 是行号、正文起点是字节，按映射折算；owner 落进
    // 被替换区间时 mapLine/mapByte 返回哨兵 -2 → 不等 → 全量回退（保守而正确）。
    if (map.mapLine(lhs.listItemLine) != rhs.listItemLine) {
        return fail("listItemLine");
    }
    if (map.mapByte(lhs.contentBeg) != rhs.contentBeg) {
        return fail("contentBeg");
    }
    if (map.mapByte(lhs.srcBeg) != rhs.srcBeg || map.mapByte(lhs.srcEnd) != rhs.srcEnd) {
        return fail("srcBeg/srcEnd");
    }
    if (map.mapByte(lhs.taskStateByte) != rhs.taskStateByte) {
        return fail("taskStateByte");
    }
    if (map.mapByte(lhs.codeBlockBeg) != rhs.codeBlockBeg ||
        map.mapByte(lhs.codeBlockEnd) != rhs.codeBlockEnd) {
        return fail("codeBlockBeg/End");
    }
    if (map.mapByte(lhs.tableId) != rhs.tableId) {
        return fail("tableId");
    }
    if (map.mapLine(lhs.number) != rhs.number) {
        return fail("number");
    }
    if (map.mapLine(lhs.sectionHeadingLine) != rhs.sectionHeadingLine ||
        map.mapLine(lhs.parentHeadingLine) != rhs.parentHeadingLine) {
        return fail("sectionHeadingLine/parentHeadingLine");
    }
    // blockBeg/blockEnd **刻意不比**：它们只经 blockRangeFor 喂 isActiveBlock（活动块
    // 判定），而"哪些行是活动块"已由重算集本身保证（编辑前后两代的活动块整块标记 +
    // ±2 邻域）；同时"跨在编辑区间上的叶子块"（多行段落里插字）两侧本来就不同，
    // 硬比只会把最常见的编辑误判成回退。
    // sectionEndLine **刻意不比**：它是"章节到哪一行结束"，编辑点之前的标题行在文末
    // 追加/删除时这个值必然变（而那一行的装饰一个字节都没动）。装饰层只读
    // sectionHeadingLine / parentHeadingLine（折叠的父链）与 foldedHeadings，从不读它。
    // 需要"整段章节重算"时用的是**新计划**的 sectionEndLine（见下面的章节展开）。
    if (lhs.conceal.size() != rhs.conceal.size() || lhs.spans.size() != rhs.spans.size() ||
        lhs.cells.size() != rhs.cells.size()) {
        return fail("conceal/spans/cells 数量");
    }
    for (std::size_t i = 0; i < lhs.conceal.size(); ++i) {
        if (!sameRange(lhs.conceal[i], rhs.conceal[i])) {
            return fail("conceal 区间");
        }
    }
    for (std::size_t i = 0; i < lhs.spans.size(); ++i) {
        if (lhs.spans[i].kind != rhs.spans[i].kind || !sameRange(lhs.spans[i].content, rhs.spans[i].content) ||
            !sameRange(lhs.spans[i].openMark, rhs.spans[i].openMark) ||
            !sameRange(lhs.spans[i].closeMark, rhs.spans[i].closeMark)) {
            return fail("spans 片段");
        }
    }
    for (std::size_t i = 0; i < lhs.cells.size(); ++i) {
        if (!sameRange(lhs.cells[i], rhs.cells[i])) {
            return fail("cells 区间");
        }
    }
    return true;
}

// ── 装饰增量的诊断计数（T4 §9）────────────────────────────────────────────
// full = 全量重建；incremental = 区间增量成功；fallback = 尝试增量但判据不过 → 退全量。
// 只给测试 / 诊断读，不属于公开 API。
struct DecorationDebugStats {
    unsigned long long full = 0;
    unsigned long long incremental = 0;
    unsigned long long fallback = 0;
    unsigned long long cursorSemanticHit = 0;
    unsigned long long cursorPartial = 0;
    unsigned long long cursorRebuiltRows = 0;
    unsigned long long cursorCopiedRows = 0;
    unsigned long long cursorUnchanged = 0;
    unsigned long long editUnchanged = 0;
    unsigned long long editPaged = 0;
    unsigned long long editChangedRows = 0;
    unsigned long long editCopiedRows = 0;
    unsigned long long editShiftPaged = 0;
};

inline DecorationDebugStats& decorationDebugStats() {
    static DecorationDebugStats stats;
    return stats;
}

// 最近一次"增量被判据拒掉"的原因（诊断用，不进公开 API；测试据此定位回退点）。
inline std::string& decorationRejectReason() {
    static std::string reason;
    return reason;
}

// ── 编辑区间的增量装饰（T4 B 核心）────────────────────────────────────────
// 文本在第 K 行插/删 N 行之后，**区间外的行整行复用**（绝对偏移按 delta 平移），
// 区间内按新计划重算。目标：结果与"对新文本全量 buildDecorations"逐字段等价。
//
// 判据（任何一条不过就 return false → 调用方全量重建）：
//   ① 区间元数据与两代计划自洽（行号、尾段行数、前缀/后缀的字节对应）；
//   ② 表格被触及时扩到整个表格块；列吸附/网格几何由 InputModel 按块测量；
//   ③ 折叠键全部落在编辑点之前且仍是新计划里的标题起点 —— 键是字节偏移，
//      本层拿不到应用层的折叠集合去做平移，键可能漂移就全量（T4 B5 / §9）；
//   ④ 区间外每一行的 LpLine 结构等价 up to 平移；
//   ⑤ 重算段里的代码内容行，其所在围栏块必须整块都在重算集里（语法状态机从开围栏
//      起步，见 buildDecorations 的 SyntaxState/codeLang）。
//
// 重算集的构成（并集，最后统一做 ±2 邻域膨胀 + 围栏扩块，见下面的 (e)(f)）：
//   编辑区间 → 上一代代码块（按旧坐标展开再映射）→ 编辑前后光标所在的活动块
//   → 受影响行的整个章节（预览横条按章节分配）→ 新计划的代码块（fixpoint 展开）。
struct IncrementalDecorationPatches {
    std::vector<int> rows;
    std::vector<LineDecoration> decorations;
};

inline bool buildDecorationsIncremental(const LpPlan& prevPlan,
                                        const LpPlan& plan,
                                        const components::MarkdownStyle& style,
                                        int cursor,
                                        int previousCursor,
                                        std::vector<LineDecoration>& out,
                                        const std::string& fontFamily,
                                        const std::string& docDir,
                                        const std::set<int>* foldedHeadings,
                                        const std::string& text,
                                        const EditorColors* colors,
                                        const PendingTextEdit& edit,
                                        components::input_detail::LineDecorationView previous,
                                        bool* unchanged = nullptr,
                                        IncrementalDecorationPatches* changedPages = nullptr) {
    if (unchanged) *unchanged = false;
    if (changedPages) *changedPages = {};
    const int prevCount = static_cast<int>(prevPlan.lines.size());
    const int count = static_cast<int>(plan.lines.size());
    const int byteBeg = edit.byteBeg;
    const int oldEnd = edit.oldEnd;
    const int newEnd = edit.newEnd;
    const int first = edit.firstLine;
    int newLast = edit.newLastLine;
    int oldTail = edit.oldTailLine;
    const int deltaBytes = newEnd - oldEnd;
    // 行数差 = newLast + 1 - oldTail（由 recordPendingEdit 的定义反解，
    // 与"新尾段行数 - 旧尾段行数 = 行数差"的自检互为验证）。
    // 注意它取的是**编辑的行数差**：下面的联合推进成对 +1，这个值恒定不变。
    const int deltaLines = newLast + 1 - oldTail;
    const auto reject = [](const char* reason) {
        decorationRejectReason() = reason;
        return false;
    };

    // ① 区间自检。
    if (!edit.valid || count <= 0 || previous.size() != static_cast<std::size_t>(prevCount)) {
        return reject("区间自检：表行数与上一代计划不符");
    }
    if (byteBeg < 0 || oldEnd < byteBeg || newEnd < byteBeg ||
        newEnd > static_cast<int>(text.size())) {
        return reject("区间自检：字节区间越界");
    }
    if (first < 0 || newLast < first || oldTail < first || newLast >= count || oldTail > prevCount) {
        return reject("区间自检：行号区间非法");
    }
    if (count - (newLast + 1) != prevCount - oldTail) {
        return reject("区间自检：尾段行数不自洽");  // 区间元数据与文本脱节
    }
    if (plan.lineIndexFor(byteBeg) != first) {
        return reject("区间自检：byteBeg 行号与新计划对不上");
    }
    if (first > 0 && first < count && first < prevCount &&
        prevPlan.lines[static_cast<std::size_t>(first)].srcBeg !=
            plan.lines[static_cast<std::size_t>(first)].srcBeg) {
        return reject("区间自检：前缀不再一致");
    }
    // ── 后缀锚点的联合推进（T4 审计 HIGH-2）────────────────────────────────
    // recordPendingEdit 没有旧全文（它只有新文本与被改的区间），newLastLine/oldTailLine
    // 是按"新段里除末字节外的换行数"倒推的：在**行中 / 行尾**敲回车时，被回车劈开的
    // 那条旧行既没被新尾段盖住、也没被旧尾段跳过，锚点
    // `prev.lines[oldTail].srcBeg + deltaBytes == plan.lines[newLast+1].srcBeg`
    // 必然对不上 —— 于是"回车换行"这个最高频的编辑一律退全量（HIGH-2）。
    // 这里**成对**推进 newLast/oldTail：每 +1 等价于"多重算一条新行、同时多跳过一条
    // 旧行"，行数差 deltaLines 与恒等式 `count-(newLast+1) == prevCount-oldTail`
    // 在推进中逐次保持不变（推完再验一次，见下），两侧行数不会被推岔；推到任一侧
    // 耗尽时恒等式恰好让另一侧同时耗尽 = 没有后缀可对，锚点自检自然成立。
    // 推进**不绕开**任何判据：恒等式重验、④ 区间外逐行结构等价 up to 平移、⑤ 围栏
    // 整块重算照旧执行。被回车劈开的段落/标题，其受影响的行大多落在"编辑区间 ±2 邻域
    // + 新旧活动块"的重算集里（重算而非复用，结果天然与全量一致）；一旦**区间外**
    // 还留着复用行、而它的 blockFirst/Last 或 section 行号对不上（例：标题被劈开后
    // 下方正文行的 sectionHeadingLine 指向被吃掉的旧行号），④ 当场拒掉退全量 ——
    // 表格的列几何由 InputModel 按整块验证和重测，围栏改写由 ⑤ 挡下。
    while (newLast + 1 < count && oldTail < prevCount &&
           prevPlan.lines[static_cast<std::size_t>(oldTail)].srcBeg + deltaBytes !=
               plan.lines[static_cast<std::size_t>(newLast + 1)].srcBeg) {
        ++newLast;
        ++oldTail;
    }
    if (count - (newLast + 1) != prevCount - oldTail) {
        return reject("区间自检：联合推进后尾段行数不自洽");  // 恒等式是推进的安全带
    }
    // 推进后的锚点终检（循环本身就在对齐它，这里是恒定的再确认 —— 不许被"绕开自检"）。
    if (newLast + 1 < count && oldTail < prevCount &&
        prevPlan.lines[static_cast<std::size_t>(oldTail)].srcBeg + deltaBytes !=
            plan.lines[static_cast<std::size_t>(newLast + 1)].srcBeg) {
        return reject("区间自检：后缀不再一致");
    }

    // Table decorations are row-local. Column geometry is invalidated by table in InputModel.

    // ③ 折叠键。
    const bool hasFolds = foldedHeadings != nullptr && !foldedHeadings->empty();
    if (hasFolds) {
        for (const int key : *foldedHeadings) {
            if (key >= byteBeg) {
                return reject("折叠：键落在编辑点之后（键会漂，本层不做键平移）");
            }
            const int lineIndex = plan.lineIndexFor(key);
            if (lineIndex < 0 || lineIndex >= count) {
                return reject("折叠：键越出新计划");
            }
            const LpLine& head = plan.lines[static_cast<std::size_t>(lineIndex)];
            if (head.srcBeg != key || head.kind != LpKind::Heading) {
                return reject("折叠：键已不指向标题（此前的编辑让它过期）");
            }
        }
    }

    // 旧坐标行号 → 新坐标行号（-1 = 落在被吃掉的区间里，已经算进重算集）。
    const auto mapOldToNew = [&](int oldLine) {
        if (oldLine < first) {
            return oldLine;
        }
        if (oldLine >= oldTail) {
            return oldLine + deltaLines;
        }
        return -1;
    };

    // ── 构造重算集 ──
    std::vector<char> rebuild(static_cast<std::size_t>(count), 0);
    const auto markRange = [&](int begLine, int endLine) {
        if (endLine < begLine) {
            return;
        }
        const int from = std::max(0, begLine);
        const int to = std::min(count - 1, endLine);
        for (int i = from; i <= to; ++i) {
            rebuild[static_cast<std::size_t>(i)] = 1;
        }
    };
    // (a) 编辑区间（新坐标）。
    markRange(first, newLast);

    // (b) 上一代的代码块：编辑可能把闭围栏删掉/加进来，只看新计划会漏行 ——
    //     先在旧坐标里把受影响区间扩到整块，再映射回新坐标。
    int oldA = first;
    int oldB = oldTail - 1;  // 纯插入时 oldB < oldA：没有旧行被吃掉
    if (oldB >= oldA && oldA >= 0 && oldA < prevCount) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = oldA; i <= oldB && i < prevCount; ++i) {
                const LpLine& line = prevPlan.lines[static_cast<std::size_t>(i)];
                if (line.codeBlockBeg < 0) {
                    continue;
                }
                const int blockFirst = prevPlan.lineIndexFor(line.codeBlockBeg);
                const int blockLast = prevPlan.lineIndexFor(line.codeBlockEnd);
                if (blockFirst >= 0 && blockFirst < oldA) {
                    oldA = blockFirst;
                    changed = true;
                }
                if (blockLast > oldB && blockLast < prevCount) {
                    oldB = blockLast;
                    changed = true;
                }
            }
        }
        if (oldA < first) {
            markRange(oldA, std::min(oldB, first - 1));
        }
        if (oldB >= oldTail) {
            markRange(newLast + 1, oldB + deltaLines);
        }
    }

    // (c) 活动块：光标所在块决定标记显隐。编辑后光标换块时，旧块要重新"藏回去"、
    //     新块要重新"露出来"；撤销还可能把光标丢到很远的地方（cursorBefore）。
    const auto markActiveBlock = [&](const LpPlan& sourcePlan, int lineIndex, bool oldCoords) {
        if (lineIndex < 0 || lineIndex >= static_cast<int>(sourcePlan.lines.size())) {
            return;
        }
        const BlockRange range = blockRangeFor(sourcePlan.lines[static_cast<std::size_t>(lineIndex)]);
        if (range.beg < 0 || range.end < range.beg) {
            return;
        }
        const int from = sourcePlan.lineIndexFor(range.beg);
        const int to = std::min(static_cast<int>(sourcePlan.lines.size()) - 1,
                                sourcePlan.lineIndexFor(range.end));
        for (int i = from; i <= to; ++i) {
            if (oldCoords) {
                const int mapped = mapOldToNew(i);
                if (mapped >= 0 && mapped < count) {
                    rebuild[static_cast<std::size_t>(mapped)] = 1;
                }
            } else {
                markRange(i, i);
            }
        }
    };
    {
        const int newCursorLine = plan.lineIndexFor(cursor);
        markActiveBlock(plan, newCursorLine, false);
        // 旧活动块 = **上一次建表用的那个光标**（DecorationCache::cursor）所在的块：
        // 上一张表正是按它把标记露出来的，光标一走那些行要重新藏回去。
        // edit.cursorBefore 只是"编辑前的光标"，与"上一次建表用的光标"未必同一个
        // （先移光标再编辑就是两回事），两个都标上，宁可多重算几行。
        markActiveBlock(prevPlan, prevPlan.lineIndexFor(previousCursor), true);
        markActiveBlock(prevPlan, prevPlan.lineIndexFor(edit.cursorBefore), true);
    }

    // (d) 受影响行所在的整个章节（只在有折叠时）：预览横条按"章节体第一条非空行"分配，
    //     只重算半截章节会把预览判到旧行上（T4 B4）。
    if (hasFolds) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < count; ++i) {
                if (!rebuild[static_cast<std::size_t>(i)]) {
                    continue;
                }
                int owner = plan.lines[static_cast<std::size_t>(i)].sectionHeadingLine;
                while (owner >= 0 && owner < count) {
                    const LpLine& head = plan.lines[static_cast<std::size_t>(owner)];
                    const int sectionEnd = head.sectionEndLine;
                    if (sectionEnd <= owner || sectionEnd > count) {
                        return reject("折叠：章节区间不可信");
                    }
                    for (int j = owner; j < sectionEnd; ++j) {
                        if (!rebuild[static_cast<std::size_t>(j)]) {
                            rebuild[static_cast<std::size_t>(j)] = 1;
                            changed = true;
                        }
                    }
                    owner = head.parentHeadingLine;
                }
            }
        }
    }

    // (e) ±2 邻域膨胀：块首/块末、分段底色这类"看相邻行"的装饰不能跨界复用 ——
    //     相邻行变了而本行照抄会拿到过期的 backgroundBlockFirst/Last 等标志。
    //     R1 起块间距（blockSpaceBeforeFor）要回看**前两条源行**（标题—空行—标题的
    //     例外判定），所以半径从 1 扩到 2：只膨胀 1 的话，"把前两行改成标题"这类编辑
    //     会让本行照抄旧的 spaceBefore。
    //     必须在下面的围栏扩块**之前**做：膨胀可能牵进一条代码内容行，而扩块才是把
    //     它的开围栏一起拉进来的那一步；顺序反了就会撞上 ⑤ 的不变量（重算段里有代码
    //     内容行却没连围栏一起重算）→ 白白退全量。
    {
        std::vector<char> dilated = rebuild;
        for (int i = 0; i < count; ++i) {
            if (!rebuild[static_cast<std::size_t>(i)]) {
                continue;
            }
            for (int d = 1; d <= 2; ++d) {
                if (i - d >= 0) {
                    dilated[static_cast<std::size_t>(i - d)] = 1;
                }
                if (i + d < count) {
                    dilated[static_cast<std::size_t>(i + d)] = 1;
                }
            }
        }
        rebuild.swap(dilated);
    }

    // Table boundaries/header context are block-wide. Rebuild the entire touched group,
    // including neighbours pulled in by dilation; unrelated tables retain their rows.
    for (int begin = 0; begin < count;) {
        const int id = plan.lines[static_cast<std::size_t>(begin)].tableId;
        if (id < 0) { ++begin; continue; }
        int end = begin + 1;
        while (end < count && plan.lines[static_cast<std::size_t>(end)].tableId == id) ++end;
        const bool touched = std::any_of(rebuild.begin() + begin, rebuild.begin() + end,
                                        [](char value) { return value != 0; });
        if (touched) std::fill(rebuild.begin() + begin, rebuild.begin() + end, 1);
        begin = end;
    }

    // (f) 新计划的代码块：语法状态机沿行序走，SyntaxState 与 codeLang 在开围栏重置，
    //     所以重算集一旦碰到块内任何一行就扩到整块（从开围栏算到受影响块尾）。
    //     扩块只往块内加行，不会把新行带到半径外，所以一遍循环内部收敛即可，
    //     不需要再回到 (e) 膨胀。
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < count; ++i) {
                if (!rebuild[static_cast<std::size_t>(i)]) {
                    continue;
                }
                const LpLine& line = plan.lines[static_cast<std::size_t>(i)];
                if (line.codeBlockBeg < 0) {
                    continue;
                }
                const int blockFirst = plan.lineIndexFor(line.codeBlockBeg);
                const int blockLast = plan.lineIndexFor(line.codeBlockEnd);
                for (int j = blockFirst; j <= blockLast && j < count; ++j) {
                    if (!rebuild[static_cast<std::size_t>(j)]) {
                        rebuild[static_cast<std::size_t>(j)] = 1;
                        changed = true;
                    }
                }
            }
        }
    }

    // ⑤ 重算段里的代码内容行必须连开围栏一起重算（状态机从围栏起步）。
    for (int i = 0; i < count; ++i) {
        if (!rebuild[static_cast<std::size_t>(i)]) {
            continue;
        }
        const LpLine& line = plan.lines[static_cast<std::size_t>(i)];
        if (line.kind != LpKind::Code || line.codeFence || line.codeBlockBeg < 0) {
            continue;
        }
        const int blockFirst = plan.lineIndexFor(line.codeBlockBeg);
        const int blockLast = plan.lineIndexFor(line.codeBlockEnd);
        for (int j = blockFirst; j <= blockLast && j < count; ++j) {
            if (!rebuild[static_cast<std::size_t>(j)]) {
                return reject("语法：重算段里的代码内容行没连开围栏一起重算");
            }
        }
    }

    // ④ 区间外每一行的结构必须等价 up to 引用映射（EditRefMap 同时处理 head/tail）。
    const EditRefMap refMap{byteBeg, oldEnd, deltaBytes, first, oldTail, deltaLines};
    for (int i = 0; i < count; ++i) {
        if (rebuild[static_cast<std::size_t>(i)]) {
            continue;
        }
        const int mapped = i < first ? i : i - deltaLines;
        if (mapped < 0 || mapped >= prevCount) {
            return reject("结构：区间外行找不到对应的旧行");
        }
        std::string field;
        if (!linesEquivalentUpToShift(prevPlan.lines[static_cast<std::size_t>(mapped)],
                                      plan.lines[static_cast<std::size_t>(i)], refMap, field)) {
            decorationRejectReason() =
                "结构：行 " + std::to_string(i) + " 与旧行 " + std::to_string(mapped) +
                " 不等价（" + field + "；编辑改到了别处的块结构）";
            return false;
        }
    }

    // ── 生成：重算行走全量的逐行逻辑，其余整行复用并平移 ──
    out.clear();
    // Stable source-row identities permit page publication after byte insertion
    // or deletion too. Shift only decorations containing absolute references;
    // offset-free suffix rows keep their immutable payload. Structural proofs,
    // full-block rebuilds and fold-key checks above remain mandatory.
    const bool canShare = unchanged && deltaLines == 0 && count == prevCount &&
        (deltaBytes == 0 || changedPages);
    std::vector<std::pair<std::size_t, LineDecoration>> patches;
    bool same = canShare;
    if (!canShare) out.reserve(static_cast<std::size_t>(count));
    DecorationBuildContext ctx;
    ctx.plan = &plan;
    ctx.style = &style;
    ctx.cursor = cursor;
    ctx.text = &text;
    ctx.colors = colors;
    ctx.docDir = &docDir;
    ctx.foldedHeadings = foldedHeadings;
    ctx.italicFamily = core::TextPrimitive::resolveItalicFontPath(fontFamily);
    prepareDecorationContext(ctx);
    // 重建段从干净状态起步：语法状态机与预览 claim 在下面沿行走（复用行也补 claim，
    // 否则后面重算的行会重复出预览横条 —— T4 B4 的"全量语义播种"）。
    ctx.syntaxState = SyntaxState{};
    ctx.codeLang.clear();
    ctx.previewOwners.clear();

    for (int i = 0; i < count; ++i) {
        const std::size_t lineIndex = static_cast<std::size_t>(i);
        if (rebuild[lineIndex]) {
            if (canShare) {
                patches.emplace_back(lineIndex, LineDecoration{});
                buildDecorationForLine(ctx, lineIndex, patches.back().second);
                same = (patches.back().second == previous[lineIndex]) && same;
            } else {
                out.emplace_back();
                buildDecorationForLine(ctx, lineIndex, out.back());
            }
            continue;
        }
        const int mapped = i < first ? i : i - deltaLines;
        if (!canShare) {
            LineDecoration decoration = previous[static_cast<std::size_t>(mapped)];
            // Only rows after the edit shift their absolute offsets.
            translateDecoration(decoration, i > newLast ? deltaBytes : 0);
            decoration.tableId = plan.lines[lineIndex].tableId;
            out.push_back(std::move(decoration));
        } else {
            const auto& old = previous[lineIndex];
            const bool shiftReferences = i > newLast && deltaBytes != 0 &&
                (!old.holes.empty() || !old.runs.empty() || !old.cells.empty() ||
                 old.tableId >= 0 || old.listIndentBeg >= 0 || old.listIndentEnd >= 0);
            if (shiftReferences || old.tableId != plan.lines[lineIndex].tableId) {
                patches.emplace_back(lineIndex, old);
                translateDecoration(patches.back().second, i > newLast ? deltaBytes : 0);
                patches.back().second.tableId = plan.lines[lineIndex].tableId;
                same = (patches.back().second == old) && same;
            }
        }
        if (hasFolds) {
            bool hidden = false;
            int outermostFolded = -1;
            bool preview = false;
            foldLineFlags(plan, lineIndex, foldedHeadings, ctx.previewOwners, hidden,
                          outermostFolded, preview);
        }
    }
    if (canShare) {
        if (same) {
            *unchanged = true;
            return true;
        }
        if (changedPages) {
            // Publish rebuilt values and translated absolute references only;
            // offset-free rows and untouched prefix pages stay shared.
            for (auto& patch : patches) {
                patch.second.tableId = plan.lines[patch.first].tableId;
                if (patch.second == previous[patch.first]) continue;
                changedPages->rows.push_back(static_cast<int>(patch.first));
                changedPages->decorations.push_back(std::move(patch.second));
            }
            if (changedPages->rows.empty()) *unchanged = true;
            return true;
        }
        out.reserve(previous.size());
        std::size_t firstUncopied = 0;
        for (auto& patch : patches) {
            out.insert(out.end(), previous.begin() + firstUncopied, previous.begin() + patch.first);
            out.push_back(std::move(patch.second));
            firstUncopied = patch.first + 1;
        }
        out.insert(out.end(), previous.begin() + firstUncopied, previous.end());
        // Preserve mapped table identities even when a candidate changed.
        for (std::size_t row = 0; row < out.size(); ++row) out[row].tableId = plan.lines[row].tableId;
    }
    return true;
}

// ── 缓存 ────────────────────────────────────────────────────────────────────
// lp_plan 是文本的纯函数，按文本缓存；装饰表还是"光标"的函数，按 (计划版本, 光标, 样式输入) 缓存。
// 没有第二层缓存的话，开了光标闪烁之后每帧都要重建整张表（1MB 文档是几万个区间）。
struct PlanCache {
    std::string text;
    LpPlan plan;
    unsigned long long version = 0;
    bool valid = false;
    // ── 上一代计划（T4 B）──────────────────────────────────────────────────
    // 装饰增量要用它做两件事：① 校验"区间外的行结构等价 up to 平移"（没有它就无法
    // 证明复用是对的）；② 按上一代的代码块区间展开重算范围（编辑把闭围栏删掉这类
    // 情况只看新计划会漏行）。
    // **只保留一代**：重建时把当前代整体 move 过去（零拷贝），最老的一代随即释放 ——
    // 内存上限恒为两份计划，不随编辑次数累积，也不会为避免全量重建多出一份复制。
    LpPlan prevPlan;
    unsigned long long prevVersion = 0;
    bool prevValid = false;
    // ── T5：局部重解析的链条（与 DecorationCache::textRevision 同款语义）────
    // 这份计划是按哪个 textRevision 建的、当时是不是可信公共漏斗的产物。局部重解析
    // 要求「上一次建计划于 R、本次是 R+1、pendingEdit 描述的正是这次改动」，缺一不可。
    // 注意局部路径本身还会做**逐字节**的前缀/后缀证明（lp_plan::buildLpPlanPartial），
    // 所以即便这里的链条判断松了，错的 delta 也会在那一步被挡下来。
    unsigned long long textRevision = 0;
    bool builtCommitted = false;
};

inline PlanCache& planCache() {
    static PlanCache cache;
    return cache;
}

// ── 换文档时的滞留清理（T16）────────────────────────────────────────────────
// 非 markdown 文档不走装饰（ui/editor_view 的 decorationsForEditor 对非 md 直接返回
// 空表），cachedPlan 根本不会再被调用 —— 上一篇 markdown 的 text / plan / prevPlan 会
// **一直**挂在缓存里（大文档的计划是百 KB 级驻留，还连着上一代一起留着）。
// 应用层在换文档那一刻（state/app_state.h 的 resetEditorInputState）调这里清内容。
//
// 只清内容，**version / prevVersion 一律不动（单调不归零）**：
//   · version 是单调计数：T4 的 planVersion 链（planVersion == 上一代 + 1）与装饰缓存
//     的 planVersion 键都按"只增不减"消费。归零会让老装饰表的键重新"命中"——拿上一篇
//     文档的表画这一篇；保持单调则键永远对不上，下一次必然是全量重建；
//   · valid / prevValid 置假 + 两条 revision 链断开 ⇒ 下一次 cachedPlan 必然全量
//     build，version 恰好 +1 —— "文本变一次 version 恰好 +1"的不变式不破；
//   · 为什么在应用层：components 不认识 lp（禁止从 components include app model），
//     InputModel::loadDocument 只负责输入组件自己的字段，清 lp 缓存不下沉进去。
inline void invalidateDecorationCache();
inline void invalidatePlanCache() {
    PlanCache& cache = planCache();
    cache.text.clear();
    cache.plan = LpPlan{};
    cache.prevPlan = LpPlan{};  // 上一代同样是一整份计划，一起释放才是真清驻留
    cache.valid = false;
    cache.prevValid = false;    // 没有可比的上一代 ⇒ 局部重解析的链条必须断开
    cache.textRevision = 0;
    cache.builtCommitted = false;
    invalidateDecorationCache();
}

// 取（或建）当前文本的计划。
//
// editInfo == nullptr（或链不完整）→ 全量 buildLpPlan，行为与改造前逐字一致。
// editInfo 带着这次编辑的 PendingTextEdit 且 revision 链正好 +1 → **尝试局部重解析**
// （T5）：只对受影响的段落重跑 md4c，其余按上一代平移复用；任何一处证明不了就
// 当场回退全量（原因留在 neo::planRejectReason()）。
//
// 不变式（T4 依赖，动它之前先读 T4 B）：**文本变一次，version 恰好 +1**，且
// prevPlan/prevVersion 指向上一次建表用的那份计划 —— 局部与全量两条路都必须满足。
inline const LpPlan& cachedPlan(const std::string& text,
                                const DecoratorEditInfo* editInfo = nullptr) {
    PlanCache& cache = planCache();
    if (cache.valid && cache.text == text) {
        return cache.plan;  // 未变文本：不碰 version / prevPlan
    }

    // 当前代 → 上一代（零拷贝），旧文本搬到局部重建的入参里。
    const std::string prevText = std::move(cache.text);
    cache.prevPlan = std::move(cache.plan);
    cache.prevVersion = cache.version;
    cache.prevValid = cache.valid;
    cache.valid = false;  // 中途失败也不得让下一次把半成品当"当前代"

    // 编辑链（与 cachedDecorations 的判据一字不差）：可信公共漏斗 + revision 恰好 +1。
    const bool chain =
        editInfo != nullptr && editInfo->committed && editInfo->edit != nullptr &&
        editInfo->edit->valid && cache.prevValid && cache.builtCommitted &&
        cache.textRevision + 1 == editInfo->textRevision &&
        editInfo->edit->revision == editInfo->textRevision;
    bool partial = false;
    if (chain) {
        LpTextEdit delta;
        delta.valid = true;
        delta.revision = editInfo->edit->revision;
        delta.byteBeg = editInfo->edit->byteBeg;
        delta.oldEnd = editInfo->edit->oldEnd;
        delta.newEnd = editInfo->edit->newEnd;
        delta.firstLine = editInfo->edit->firstLine;
        delta.newLastLine = editInfo->edit->newLastLine;
        delta.oldTailLine = editInfo->edit->oldTailLine;
        delta.cursorBefore = editInfo->edit->cursorBefore;
        partial = buildLpPlanPartial(text, prevText, cache.prevPlan, delta, cache.plan, nullptr);
    } else {
        planRejectReason() = "编辑链不完整（无 delta / 非提交文本 / revision 断链）";
    }
    if (!partial) {
        cache.plan = buildLpPlan(text);  // 全量（++planDebugStats().full）
    }
    cache.text = text;
    ++cache.version;  // 局部与全量都只加这一次 —— T4 的 planVersion 链靠它
    cache.valid = true;
    cache.textRevision = editInfo != nullptr ? editInfo->textRevision : 0;
    cache.builtCommitted = editInfo != nullptr && editInfo->committed;
    return cache.plan;
}

// 第二层缓存：装饰表。键 = 计划版本 + 光标语义区间 + 样式输入 + 主题版本。
// markdownStyle() 完全由前几个输入派生，但**颜色**这一项由主题决定，所以主题必须进键：
// 少了它，切到浅色主题后装饰表会被判定为"没变"，文字仍画着深色主题的颜色。
struct CursorSpanDependency {
    int beg = 0;
    int end = 0;
    int row = 0;
    int prefixEnd = 0;
};
struct CursorFoldSeed {
    int owner = -1;
    bool preview = false;
};

struct DecorationCache {
    unsigned long long planVersion = static_cast<unsigned long long>(-1);
    int cursor = -1;
    // Conservative equivalence intervals: source-line starts preserve active
    // block/current fold row; inclusive span bounds preserve marker reveal state.
    std::vector<int> cursorBoundaries;
    std::size_t cursorRegion = 0;
    std::vector<CursorSpanDependency> cursorSpans;
    // Allocated only when folds exist; independent of the current cursor.
    std::vector<CursorFoldSeed> cursorFolds;
    float bodySize = -1.0f;
    std::string fontFamily;
    // 代码字体（代码块/行内代码等宽）单独进键：改它只变 style.codeFontFamily，
    // 上面的编辑器字体键不动，少了它换代码字体后装饰表不重建。
    std::string codeFontFamily;
    int theme = -1;
    // ── 主题文件版本（T13 收尾）────────────────────────────────────────────
    // ThemeMode 只有深/浅两档，而主题 JSON 是"叠在内置配色上的覆盖表"：换主题文件
    // 时**外观侧（ThemeMode）不变**、键里其它项（bodySize / 字体 / 目录 / 折叠…）
    // 一个都不动 —— revision 不进键就会命中上一套主题的表，标题 / 代码 / 引用继续
    // 画旧色，主题字体（typography 的正文/代码字体）也一并遗留。
    // 初值 ~0ULL：与 themeRevision() 的初值 0 必然不同（再叠 valid=false 双保险），
    // 同 editorColors() 的版本号缓存。
    unsigned long long themeRevision = ~0ULL;
    // 相对图片路径的解析基准：同一篇文本放在不同目录里解析结果不同，必须进键。
    std::string docDir;
    // 折叠状态（S3f 批次 C）：同文本同光标、折叠不同 → 装饰表必须重建（同主题那一课）。
    std::string foldKey;
    components::input_detail::LineDecorationSnapshot snapshot;
    std::weak_ptr<const components::input_detail::LineDecorationTable> legacySnapshot;
    std::vector<LineDecoration> legacyRows;
    bool valid = false;
    // ── 编辑驱动增量的链条（T4 B）──────────────────────────────────────────
    // 这张表是**哪个 revision 的文档正文**建出来的：增量要求"上次建表于 R、本次是
    // R+1，且 pendingEdit 描述的正是这次改动"。builtCommitted = false 表示上次建的
    // 是 IME 合成期的临时文本（不是任何公共漏斗的产物），链条断开 → 下次全量。
    unsigned long long textRevision = 0;
    bool builtCommitted = false;
};

inline std::string foldKeyOf(const std::set<int>* foldedHeadings) {
    if (foldedHeadings == nullptr || foldedHeadings->empty()) {
        return {};
    }
    std::string key;
    for (const int beg : *foldedHeadings) {
        key += std::to_string(beg);
        key.push_back(',');
    }
    return key;
}

inline DecorationCache& decorationCache() {
    static DecorationCache cache;
    return cache;
}

inline void buildCursorBoundaries(const LpPlan& plan, std::vector<int>& boundaries) {
    boundaries.clear();
    boundaries.reserve(plan.lines.size());
    for (const auto& line : plan.lines) {
        boundaries.push_back(line.srcBeg);
        // Include every span, not just those on the caret line: multiline spans
        // can reveal marks on another source line.
        for (const auto& span : line.spans) {
            boundaries.push_back(spanBegOf(span));
            const int end = spanEndOf(span);
            if (end < std::numeric_limits<int>::max()) boundaries.push_back(end + 1);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
}

inline std::size_t cursorRegionOf(const std::vector<int>& boundaries, int cursor) {
    return static_cast<std::size_t>(std::upper_bound(boundaries.begin(), boundaries.end(), cursor) -
                                    boundaries.begin());
}

// Sorted interval index: query all spans containing a caret, including spans
// whose marks are on another source row. Prefix maxima bound the reverse scan.
inline void buildCursorDependencies(const LpPlan& plan, DecorationCache& cache,
                                    const std::set<int>* foldedHeadings) {
    cache.cursorSpans.clear();
    for (std::size_t row = 0; row < plan.lines.size(); ++row) {
        for (const auto& span : plan.lines[row].spans) {
            cache.cursorSpans.push_back({spanBegOf(span), spanEndOf(span),
                                         static_cast<int>(row), 0});
        }
    }
    std::sort(cache.cursorSpans.begin(), cache.cursorSpans.end(),
        [](const CursorSpanDependency& a, const CursorSpanDependency& b) {
            return a.beg < b.beg;
        });
    int end = std::numeric_limits<int>::min();
    for (auto& span : cache.cursorSpans) {
        end = std::max(end, span.end);
        span.prefixEnd = end;
    }
    std::vector<CursorFoldSeed>().swap(cache.cursorFolds);
    if (foldedHeadings != nullptr && !foldedHeadings->empty()) {
        cache.cursorFolds.resize(plan.lines.size());
        std::set<int> owners;
        for (std::size_t row = 0; row < plan.lines.size(); ++row) {
            bool hidden = false;
            auto& seed = cache.cursorFolds[row];
            foldLineFlags(plan, row, foldedHeadings, owners, hidden, seed.owner, seed.preview);
        }
    }
}

// Same plan and presentation keys only. Rebuild the affected rows privately;
// unchanged candidates keep snapshot identity. Changed publication moves actual
// replacements into copied pages and shares all other immutable pages.
inline bool buildCursorDecorations(const LpPlan& plan, const DecorationCache& cache,
                                   const components::MarkdownStyle& style, int cursor,
                                   const std::string& fontFamily, const std::string& docDir,
                                   const std::set<int>* foldedHeadings, const std::string& text,
                                   const EditorColors* colors, components::input_detail::LineDecorationSnapshot& out,
                                   bool& changed, std::vector<int>* changedRows = nullptr) {
    if (!cache.snapshot || cache.snapshot->size() != plan.lines.size()) return false;
    if (foldedHeadings != nullptr && !foldedHeadings->empty() &&
        cache.cursorFolds.size() != plan.lines.size()) return false;
    std::vector<int> rows;
    const auto addRow = [&](int row) {
        if (row >= 0 && row < static_cast<int>(plan.lines.size())) rows.push_back(row);
    };
    const int oldLine = plan.lineIndexFor(cache.cursor);
    const int newLine = plan.lineIndexFor(cursor);
    if (!cache.cursorFolds.empty() && oldLine != newLine) {
        addRow(oldLine);
        addRow(newLine);  // current folded row is forced visible inside one block
    }
    const BlockRange oldBlock = oldLine >= 0 ? blockRangeFor(plan.lines[oldLine]) : BlockRange{};
    const BlockRange newBlock = newLine >= 0 ? blockRangeFor(plan.lines[newLine]) : BlockRange{};
    const auto addBlock = [&](const BlockRange& block) {
        if (block.beg < 0 || block.end < block.beg) return;
        const int first = plan.lineIndexFor(block.beg);
        const int last = plan.lineIndexFor(block.end);
        for (int row = first; row <= last; ++row) {
            const auto range = blockRangeFor(plan.lines[row]);
            if (range.beg == block.beg && range.end == block.end) addRow(row);
        }
    };
    if (oldBlock.beg != newBlock.beg || oldBlock.end != newBlock.end) {
        addBlock(oldBlock);
        addBlock(newBlock);
    }
    const auto addSpans = [&](int point, int other) {
        auto it = std::upper_bound(cache.cursorSpans.begin(), cache.cursorSpans.end(), point,
            [](int value, const CursorSpanDependency& span) { return value < span.beg; });
        while (it != cache.cursorSpans.begin()) {
            --it;
            if (it->prefixEnd < point) break;
            if (it->end >= point && !(other >= it->beg && other <= it->end)) addRow(it->row);
        }
    };
    addSpans(cache.cursor, cursor);
    addSpans(cursor, cache.cursor);
    // Syntax state is cursor-independent, but rebuilding a code row requires its
    // opening fence and all preceding tokens. Keep whole-code-block rebuilding
    // until a bounded syntax-state cache has independent evidence.
    const std::size_t candidateCount = rows.size();
    std::set<int> codeStarts;
    for (std::size_t i = 0; i < candidateCount; ++i) {
        const auto& line = plan.lines[rows[i]];
        if (line.kind != LpKind::Code) continue;
        const auto block = blockRangeFor(line);
        if (!codeStarts.insert(block.beg).second) continue;
        const int first = plan.lineIndexFor(block.beg);
        if (first < 0 || !plan.lines[first].codeFence ||
            plan.lines[first].srcBeg != block.beg) return false;
        addBlock(block);
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    DecorationBuildContext ctx;
    ctx.plan = &plan;
    ctx.style = &style;
    ctx.cursor = cursor;
    ctx.text = &text;
    ctx.colors = colors;
    ctx.docDir = &docDir;
    ctx.foldedHeadings = foldedHeadings;
    ctx.italicFamily = core::TextPrimitive::resolveItalicFontPath(fontFamily);
    prepareDecorationContext(ctx);
    std::vector<LineDecoration> patches;
    std::vector<int> actualRows;
    patches.reserve(rows.size());
    actualRows.reserve(rows.size());
    changed = false;
    for (int row : rows) {
        ctx.previewOwners.clear();
        if (!cache.cursorFolds.empty()) {
            const auto& seed = cache.cursorFolds[row];
            if (seed.owner >= 0 && !seed.preview) ctx.previewOwners.insert(seed.owner);
        }
        LineDecoration candidate;
        buildDecorationForLine(ctx, static_cast<std::size_t>(row), candidate);
        if (!(candidate == (*cache.snapshot)[row])) {
            actualRows.push_back(row);
            patches.push_back(std::move(candidate));
        }
    }
    decorationDebugStats().cursorRebuiltRows += rows.size();
    changed = !actualRows.empty();
    if (changed) {
        out = cache.snapshot->replacing(actualRows, std::move(patches),
                                       &decorationDebugStats().cursorCopiedRows);
        if (!out) return false;
        if (changedRows) *changedRows = std::move(actualRows);
    }
    return true;
}

inline components::input_detail::LineDecorationSnapshot cachedDecorationSnapshot(const LpPlan& plan,
                                                           unsigned long long planVersion,
                                                           int cursor,
                                                           const components::MarkdownStyle& style,
                                                           const std::string& fontFamily,
                                                           ThemeMode theme,
                                                           const std::string& docDir = {},
                                                           const std::set<int>* foldedHeadings = nullptr,
                                                           const std::string& text = {},
                                                           const EditorColors* colors = nullptr,
                                                           const DecoratorEditInfo* editInfo = nullptr,
                                                           // 主题文件版本（T13 收尾）。默认值**取当期值**而不是固定 0：
                                                           // 旧调用（单测等）不传也不会把键钉死在 0 上 —— 固定 0 会让
                                                           // "同 ThemeMode 只换主题文件"的那一次永不算变化，真实 UI 就
                                                           // 刷不出来。真实 UI（editor_view）按约定显式传入。
                                                           unsigned long long themeRevisionValue =
                                                               themeRevision()) {
    DecorationCache& cache = decorationCache();
    if (editInfo && editInfo->changes) *editInfo->changes = {};
    const int themeKey = static_cast<int>(theme);
    const std::string foldKey = foldKeyOf(foldedHeadings);
    const auto stampKeys = [&] {
        if (!cache.valid || cache.planVersion != planVersion) {
            buildCursorBoundaries(plan, cache.cursorBoundaries);
        }
        if (!cache.valid || cache.planVersion != planVersion || cache.foldKey != foldKey) {
            buildCursorDependencies(plan, cache, foldedHeadings);
        }
        cache.planVersion = planVersion;
        cache.cursor = cursor;
        cache.cursorRegion = cursorRegionOf(cache.cursorBoundaries, cursor);
        cache.bodySize = style.bodySize;
        cache.fontFamily = fontFamily;
        cache.codeFontFamily = style.codeFontFamily;
        cache.theme = themeKey;
        cache.themeRevision = themeRevisionValue;
        cache.docDir = docDir;
        cache.foldKey = foldKey;
        cache.valid = true;
        cache.textRevision = editInfo != nullptr ? editInfo->textRevision : 0;
        cache.builtCommitted = editInfo != nullptr && editInfo->committed;
    };

    // 同计划/样式/主题/目录/折叠；原始光标或保守语义区间相同均直接共享快照。
    if (cache.valid && cache.planVersion == planVersion &&
        cache.bodySize == style.bodySize && cache.fontFamily == fontFamily &&
        cache.codeFontFamily == style.codeFontFamily &&
        cache.theme == themeKey && cache.themeRevision == themeRevisionValue &&
        cache.docDir == docDir && cache.foldKey == foldKey) {
        if (cache.cursor == cursor) return cache.snapshot;
        if (cache.cursorRegion == cursorRegionOf(cache.cursorBoundaries, cursor)) {
            // Preserve committed-text provenance; keep the latest cursor for
            // the next edit's old-active-block proof without mutating the snapshot.
            cache.cursor = cursor;
            ++decorationDebugStats().cursorSemanticHit;
            return cache.snapshot;
        }
        components::input_detail::LineDecorationSnapshot cursorNext;
        std::vector<int> changedRows;
        const auto previousSnapshot = cache.snapshot;
        bool changed = false;
        if (buildCursorDecorations(plan, cache, style, cursor, fontFamily, docDir,
                                   foldedHeadings, text, colors, cursorNext, changed, &changedRows)) {
            if (changed) {
                cache.snapshot = std::move(cursorNext);
                if (editInfo && editInfo->changes && editInfo->committed &&
                    cache.builtCommitted && cache.textRevision == editInfo->textRevision) {
                    auto& changes = *editInfo->changes;
                    changes.previous = previousSnapshot;
                    changes.next = cache.snapshot;
                    changes.textRevision = editInfo->textRevision;
                    changes.rows = std::move(changedRows);
                }
            } else {
                ++decorationDebugStats().cursorUnchanged;
            }
            // A cursor refresh preserves the committed-text provenance of the
            // source generation, including when the caller supplies no edit info.
            cache.cursor = cursor;
            cache.cursorRegion = cursorRegionOf(cache.cursorBoundaries, cursor);
            ++decorationDebugStats().cursorPartial;
            return cache.snapshot;
        }
    }

    // ── 编辑驱动的区间增量（T4 B）──────────────────────────────────────────
    // 链条缺一不可，任一断开就退回全量：
    //   ① 样式/字体/主题/主题文件版本/目录/折叠与上一次构建完全一致（只有"文本 + 光标"动过）；
    //   ② 计划版本恰好 +1，且 PlanCache 的上一代正是上次建表用的那份计划；
    //   ③ 上一次建表针对的是**文档正文**（IME 合成期的临时文本不算），
    //      revision 恰好 +1，且 pendingEdit 就是这次改动的区间。
    // 主题文件版本必须出现在 ① 里：revision 变了说明配色/主题字体已换代，上一代增量表
    // 是按旧色建的，复用它等于把旧主题的颜色平移进新表（T4 增量链同 T13 键那一课）。
    const bool inputsMatch =
        cache.valid && cache.bodySize == style.bodySize && cache.fontFamily == fontFamily &&
        cache.codeFontFamily == style.codeFontFamily && cache.theme == themeKey &&
        cache.themeRevision == themeRevisionValue && cache.docDir == docDir &&
        cache.foldKey == foldKey;
    const bool revisionChain =
        editInfo != nullptr && editInfo->committed && editInfo->edit != nullptr &&
        editInfo->edit->valid && cache.builtCommitted &&
        cache.textRevision + 1 == editInfo->textRevision &&
        editInfo->edit->revision == editInfo->textRevision;
    const bool planChain = planVersion == cache.planVersion + 1 && planCache().prevValid &&
                           planCache().prevVersion == cache.planVersion;
    std::vector<LineDecoration> next;
    if (inputsMatch && revisionChain && planChain && cache.snapshot) {
        // Build privately; all consumers keep the old immutable generation until publication.
        const auto previous = cache.snapshot;
        const int previousCursor = cache.cursor;  // 上一次建表用的光标（旧活动块要用）
        cache.valid = false;  // 中途失败时不得让下一次再把这张离手的表当"上一次"
        bool unchanged = false;
        IncrementalDecorationPatches patches;
        if (buildDecorationsIncremental(planCache().prevPlan, plan, style, cursor, previousCursor,
                                        next, fontFamily, docDir, foldedHeadings, text,
                                        colors, *editInfo->edit, *previous, &unchanged, &patches)) {
            if (unchanged) {
                cache.snapshot = previous;
                ++decorationDebugStats().editUnchanged;
            } else if (!patches.rows.empty()) {
                const auto changedRows = patches.rows.size();
                cache.snapshot = previous->replacing(patches.rows, std::move(patches.decorations),
                    &decorationDebugStats().editCopiedRows);
                ++decorationDebugStats().editPaged;
                if (editInfo->edit->newEnd != editInfo->edit->oldEnd)
                    ++decorationDebugStats().editShiftPaged;
                decorationDebugStats().editChangedRows += changedRows;
            } else {
                cache.snapshot = std::make_shared<const components::input_detail::LineDecorationTable>(std::move(next));
            }
            stampKeys();
            ++decorationDebugStats().incremental;
            return cache.snapshot;
        }
        ++decorationDebugStats().fallback;
        next.clear();
    }

    buildDecorations(plan, style, cursor, next, fontFamily, docDir, foldedHeadings,
                     text, colors);
    cache.snapshot = std::make_shared<const components::input_detail::LineDecorationTable>(std::move(next));
    stampKeys();
    ++decorationDebugStats().full;
    return cache.snapshot;
}

inline void invalidateDecorationCache() {
    auto& cache = decorationCache();
    cache.valid = false;
    cache.snapshot.reset();
    cache.legacySnapshot.reset();
    std::vector<LineDecoration>().swap(cache.legacyRows);
    std::vector<int>().swap(cache.cursorBoundaries);
    std::vector<CursorSpanDependency>().swap(cache.cursorSpans);
    std::vector<CursorFoldSeed>().swap(cache.cursorFolds);
}

// Legacy read-only adapter; callers retaining a generation should use the snapshot API.
inline const std::vector<LineDecoration>& cachedDecorations(
    const LpPlan& plan, unsigned long long planVersion, int cursor,
    const components::MarkdownStyle& style, const std::string& fontFamily, ThemeMode theme,
    const std::string& docDir = {}, const std::set<int>* foldedHeadings = nullptr,
    const std::string& text = {}, const EditorColors* colors = nullptr,
    const DecoratorEditInfo* editInfo = nullptr,
    unsigned long long themeRevisionValue = themeRevision()) {
    const auto snapshot = cachedDecorationSnapshot(plan, planVersion, cursor, style, fontFamily, theme,
        docDir, foldedHeadings, text, colors, editInfo, themeRevisionValue);
    if (const auto* contiguous = snapshot->contiguousRows()) return *contiguous;
    auto& cache = decorationCache();
    if (cache.legacySnapshot.lock() != snapshot) {
        cache.legacyRows = snapshot->copyRows();
        cache.legacySnapshot = snapshot;
    }
    return cache.legacyRows;
}

// 诊断：设了环境变量 NEO_LP_DEBUG=<文件路径> 时，把每次生成的装饰摘要追加进去，
// 用来核对"光标在哪一行、哪个块被判成活动块、隐藏区间有多少"。不设就完全不产生开销。
inline void debugLogDecorations(int cursor,
                                int cursorLine,
                                components::input_detail::LineDecorationView table) {
    const char* path = std::getenv("NEO_LP_DEBUG");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    std::FILE* file = std::fopen(path, "a");
    if (file == nullptr) {
        return;
    }
    int ranges = 0;
    int runCount = 0;
    for (const LineDecoration& decoration : table) {
        ranges += static_cast<int>(decoration.holes.size());
        runCount += static_cast<int>(decoration.runs.size());
    }
    std::fprintf(file, "cursor=%d cursorLine=%d rows=%zu holeRanges=%d runs=%d row0Holes=%zu\n",
                 cursor, cursorLine, table.size(), ranges, runCount,
                 table.empty() ? 0 : table[0].holes.size());
    std::fclose(file);
}

// 供 InputBuilder::lineDecorator 使用的入口已移至 ui/editor_view.h（T10 拆层）：
// decorationsForEditor 要读应用状态（markdownCapable / path / theme / foldedHeadings），
// 属于应用编排而非 model 逻辑；model 层只暴露 cachedPlan / buildDecorations /
// cachedDecorations 这些"吃原始参数"的函数。

}  // namespace lp
}  // namespace neo
