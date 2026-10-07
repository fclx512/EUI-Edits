#pragma once

#include "components/input_line_map.h"
#include "core/dsl.h"
#include "core/render/text.h"
#include "core/window/window_backend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace components::input_detail {

// core::Color 没有相等语义（框架里颜色只用于绘制），装饰层的内容比较必须自己比四个分量。
// 用精确比较：这些颜色都由同一处样式表算出，逐位相同才算"没变"，语义上最可预期。
inline bool colorEquals(const core::Color& lhs, const core::Color& rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

// 行内样式段：把一行里"字体参数与修饰不同"的片段切开。
//
// 组件同样不认识 markdown：`**粗体**` 的"粗"、`` `code` `` 的"等宽 + 底色"、
// 链接的"主题色"，全部由上层填在这里。字段留 0 / 空 / 透明 = 沿用该行的默认。
struct LineRunStyle {
    int weight = 0;              // <= 0 表示沿用控件默认字重（400）
    // 注意 core::Color 的默认值是"不透明白"，不是透明：这里必须显式写成透明，
    // 否则"没设置颜色"会被当成"白字"。
    core::Color color{0.0f, 0.0f, 0.0f, 0.0f};           // alpha == 0 表示用行级文字色
    core::Color background{0.0f, 0.0f, 0.0f, 0.0f};      // alpha == 0 表示不画底色
    std::string fontFamily;      // 空表示沿用控件字体（行内代码用等宽）
    bool strike = false;         // 删除线：视图层在段中间画一条线
    // 链接段（2026-09-26）：命中测试据此判定"点在链接上"，样式仍由上层给色。
    bool link = false;
    bool underline = false;

    bool operator==(const LineRunStyle& other) const {
        return weight == other.weight && colorEquals(color, other.color) &&
               colorEquals(background, other.background) && fontFamily == other.fontFamily &&
               strike == other.strike && link == other.link && underline == other.underline;
    }
    bool operator!=(const LineRunStyle& other) const { return !(*this == other); }
};

// 一个行内样式段（文本绝对偏移，半开 [beg, end)）。**可以稀疏**：没被任何段覆盖的文字
// 就是默认样式，不需要上层为"普通文字"也填一段。
struct LineRun {
    int beg = 0;
    int end = 0;
    LineRunStyle style;

    bool empty() const { return beg >= end; }
    bool operator==(const LineRun& other) const {
        return beg == other.beg && end == other.end && style == other.style;
    }
    bool operator!=(const LineRun& other) const { return !(*this == other); }
};

// 调用方（编辑器）提供的逐行装饰，供 Live Preview 使用。
//
// 组件本身不认识 markdown：字号、行高、文字色、要隐藏哪些字节、行内样式段全部由上层给。
// 这样切分的理由是"装饰计划是文本的纯函数"，可以整体缓存，光标移动不会牵动重新解析。
//
// 语义与改造前的纯文本行为是兼容的：**整表为空 => 所有行沿用控件统一的字号/行高/字重、
// 不隐藏任何字节、不含任何样式段**。
// 一行的"矩形装饰"：块底色 + 左侧竖条。
//
// 底色按**整块**画成一个矩形（这样圆角才对）：块内首行标记 backgroundBlockFirst、
// 末行标记 backgroundBlockLast，组件据此找边界；两个都不标记时退化成
// "连续有背景的行算作一块"。之所以不逐行画：逐行画的话每行都要圆角，
// 块内部会露出一排圆角缺口。
// 竖条反过来是**逐行**画的：等宽竖直矩形，相邻行紧贴就成一条 —— 不需要块边界，也没有圆角。
struct LineBoxStyle {
    // core::Color 默认是"不透明白"，表示"没设置"的字段必须显式写透明。
    core::Color background{0.0f, 0.0f, 0.0f, 0.0f};  // alpha == 0 表示不画底色
    float backgroundRadius = 0.0f;                    // <= 0 用默认圆角（随字号）
    bool backgroundBlockFirst = false;                // 本行是底色的**第一个**行 → 上圆角
    bool backgroundBlockLast = false;                 // 本行是底色的**最后一个**行 → 下圆角
    core::Color barColor{0.0f, 0.0f, 0.0f, 0.0f};     // 左侧竖条（引用块），alpha == 0 不画
    float barWidth = 0.0f;                            // <= 0 不画
    // 竖条条数（C1 嵌套引用）：0 = 单条（默认语义，与历史行为一致）；>= 2 画多条，
    // 从单条的位置往左排开。文字位置不随条数变 —— 竖条本来就不占文字位置。
    unsigned char barCount = 0;
    core::Color gridColor{0.0f, 0.0f, 0.0f, 0.0f};    // 表格网格线（2026-09-25，alpha == 0 不画）
    // A rule and a table grid never occur on the same line; share gridColor.
    // A one-byte field avoids another color in both the decoration and layout tables.
    unsigned char horizontalRuleThickness = 0;

    bool operator==(const LineBoxStyle& other) const {
        return colorEquals(background, other.background) &&
               std::fabs(backgroundRadius - other.backgroundRadius) < 0.001f &&
               backgroundBlockFirst == other.backgroundBlockFirst &&
               backgroundBlockLast == other.backgroundBlockLast &&
               colorEquals(barColor, other.barColor) &&
               std::fabs(barWidth - other.barWidth) < 0.001f &&
               barCount == other.barCount &&
               colorEquals(gridColor, other.gridColor) &&
               horizontalRuleThickness == other.horizontalRuleThickness;
    }
    bool operator!=(const LineBoxStyle& other) const { return !(*this == other); }
};

// 行首图元：画在行文字左边、并把整行文字右移 advance 的一枚图标（S3c 的任务复选框）。
//
// 为什么不做成"图元 run"：run 的文本强制取自文档切片（投影后为空就直接跳过），
// 要让它承载图标得同时改布局、caret 表、渲染三条路径；而复选框需要的只是"在行首留一块
// 地方放图标 + 文字整体右移"。用行级字段表达最省，也完全不影响没有图元的行。
struct LineGlyph {
    bool checkbox = false;
    bool checked = false;
    int codepoint = 0;    // 0 = 不画图标（text 非空时画文本 marker）
    float advance = 0.0f; // 文字右移量；<= 0 时按字号推算（图标见 kGlyphAdvanceEm，
                          // 文本 marker 由组件按度量源区间实测，见 LineDecoration）
    core::Color color{0.0f, 0.0f, 0.0f, 0.0f};  // alpha == 0 = 用行文字色
    // 文本 marker（C1 列表，如 "•" / "3."）：非空 = 画这段**呈现文本**而不是图标。
    // 它不是文档切片 —— 序号数字绝不能拼进文档文本；宽度由组件按实际字体度量。
    // codepoint == 0 && text 非空时走文本路；codepoint != 0 时图标优先（任务框）。
    std::string text;

    bool operator==(const LineGlyph& other) const {
        return checkbox == other.checkbox && checked == other.checked &&
               codepoint == other.codepoint && std::fabs(advance - other.advance) < 0.001f &&
               colorEquals(color, other.color) && text == other.text;
    }
    bool operator!=(const LineGlyph& other) const { return !(*this == other); }
};

// 行首图元的默认占位宽度（em 倍数）：图标本身约 1.05em，到文字的间距约 0.4em。
inline constexpr float kGlyphAdvanceEm = 1.45f;

// 表格单元格的左右内边距（em 倍数）。Obsidian 表格 cell padding 为 4px 8px、
// 表内字号 = 正文（2026-09-25 实测，参考/obsidian-style/规格表 §1），
// 水平 8px @ 16px = 0.5em；垂直方向的 4px 由行高余量覆盖。
inline constexpr float kTableCellPaddingEm = 0.5f;
// 列宽的上下夹逼（ZCode：min-w-16 = 64px、max-w-md = 448px @ 14px 字号；按字号换算）。
inline constexpr float kTableColumnMinEm = 4.57f;
inline constexpr float kTableColumnMaxEm = 32.0f;
// 压缩到装不下时的列宽下限（低于它就没法读了；宁可整体超出，交给横向裁切）。
inline constexpr float kTableColumnFloorEm = 1.6f;

// 表格里的一格（S3f 批次 D）：文档字节区间，beg == end 表示空格子。
// align = 该列的对齐方式（MD_ALIGN 值，S3f 批次 E）：0 default/1 left 按左排，
// 2 center 居中、3 right 贴列右缘 —— 内容超列宽时一律回到左排（截断优先于对齐）。
struct LineCell {
    int beg = 0;
    int end = 0;
    unsigned char align = 0;

    bool operator==(const LineCell& other) const {
        return beg == other.beg && end == other.end && align == other.align;
    }
    bool operator!=(const LineCell& other) const { return !(*this == other); }
};

struct LineDecoration {
    float fontSize = 0.0f;    // <= 0 表示沿用控件默认字号
    float lineHeight = 0.0f;  // <= 0 表示沿用控件默认行高（该行字号的 1.2 倍）
    // 透明 = 沿用控件默认文字色（标题/代码块用它）。core::Color 默认是不透明白，必须显式写透明。
    core::Color textColor{0.0f, 0.0f, 0.0f, 0.0f};
    // 行级字体（空 = 沿用控件字体）。Live Preview 的代码块/frontmatter 用它换等宽：
    // 测量与渲染都以这份字面为准，样式段（语法 token 着色）没有自己的字体时也继承它。
    std::string fontFamily;
    std::vector<LineHole> holes;  // 该行要隐藏的区间（**文本绝对偏移**），乱序/重叠会被规范化
    // 行内样式段（**文本绝对偏移**），乱序会被排序；稀疏合法。
    // 注意：样式段目前只在**多行**模式下参与渲染（Live Preview 是唯一的调用方）。
    std::vector<LineRun> runs;
    // 行级矩形（代码块底色 / 引用竖条），见 LineBoxStyle。
    LineBoxStyle box;
    // 行首图元（任务复选框），见 LineGlyph。
    LineGlyph glyph;
    // 行号位图元（折叠箭头，2026-09-25）：非 0 时该源行的**行号**被这个图标替掉
    // （Obsidian 的标题折叠箭头）。画在行号槽里，命中走既有的 onGutter 路径。
    // 只在 lineStart 段画。
    LineGlyph gutterGlyph;
    // 行内容左缩进（像素）：整行文字（含行首图元之后的一切）从文本原点右移这么多。
    // 与 LineGlyph 的 advance 同一套平移机制（caretX / run.x 一起移），但不画任何图标 ——
    // Live Preview 的代码块用它把文字从背景条左缘让开（Obsidian 的 16px 左内边距）。
    float contentIndent = 0.0f;
    // 行内文字在行盒内的整体下移（像素，0 = 顶对齐）。
    // R1（2026-09-26）起 Live Preview 不再设置它：标题的 1em 上置间距改由
    // spaceBefore 承担（间距是行盒**之外**的 gap，不再把行盒撑高）。
    float textShiftY = 0.0f;
    // 块首行的上方间距（像素，>= 0）。
    // Obsidian 的 LP 把块间距装在行盒**内部**（.cm-line.HyperMD-header 的 padding-top
    // = var(--p-spacing)），逐行 top 因此首尾相接；本组件用"行间 gap"表达同一件事：
    // gap 出现在第 i 行之前，不改任何一行自己的 lineHeight / bottom()。
    // 只有块首源行的第一条可视 TextLine 会盖章（见 appendMeasuredLine），续行与隐藏行恒 0。
    // 实测依据：参考/块模型契约-2026-09-26.md。
    float spaceBefore = 0.0f;
    // 独占一行的块级图片（S3f 批次 B）。非空 = 该行文字整体藏进洞里、原位画这张图；
    // 宽高已在装饰层按文件头算好（渲染层不再猜尺寸）。行高由 lineHeight 给。
    std::string imagePath;
    float imageWidth = 0.0f;
    float imageHeight = 0.0f;
    // 图片失败态占位盒（S3f 批次 F）：本地路径解析失败/头解析不出 → 原位画一个
    // "图标 + 路径"的占位盒（尺寸在 imageWidth/Height，装饰层已按 ZCode 失败盒规格给）。
    // imageFailText 是要显示的路径（装饰层已按盒宽截断），不是文档切片。
    bool imageFailed = false;
    std::string imageFailText;
    // 代码块起始围栏行右上角的语言标签（Obsidian LP 的 "JavaScript"）。
    // 只在**起始**围栏行非空；渲染层右对齐画在底色条右上角。放末尾、NSDMI，
    // partial-init 聚合初始化不受影响。
    std::string languageLabel;
    // 折叠隐藏（S3f 批次 C）：true = 该行不占高度、不画、命中绕开（几何 0 高）。
    bool hidden = false;
    // 「折叠致隐」的原始值：光标行会被装饰层强制可见（hidden 放开），但"这一行
    // 本该折叠"的事实要保留给组件的 onRevealHiddenLine 自愈检查用（↓ 落进折叠段、
    // 撤销恢复到折叠区），否则自愈永远看不到落点，折叠就卡死在半开状态。
    bool hiddenByFold = false;

    // ── 表格列吸附（S3f 批次 D）────────────────────────────────────────────
    // 本行各单元格的**内容区间**（文档字节，左→右，beg == end 表示空格子）。
    // 非空 = 本行参与列吸附：列宽由组件对**整张表**测量后统一分配（同一 tableId
    // 的行共用一套列 x），每格的文字从它那一列的 x 起排，超出列宽就硬截。
    std::vector<LineCell> cells;
    // 所属表格（同一张表的所有行相同；< 0 = 不是表格行）。
    int tableId = -1;
    // 单元格左右内边距（像素；<= 0 用组件默认，随字号）。
    float cellPadding = 0.0f;
    // 表头行 / 分隔行（|---|---|）：装饰层已把分隔行整行藏进 holes，这里只用于
    // 让组件知道"这行也算表格行"（列计划要对齐所有行，包括分隔行）。
    bool tableHeaderRow = false;
    bool tableSeparator = false;

    // ── 列表正文缩进的度量源（C1）────────────────────────────────────────
    // >= 0 时组件在测量阶段用**本行字体与字号**实测 text[beg, end) 的宽度：
    //   · 行首图元带文本 marker（glyph.text 非空）→ 该宽度充当 glyph.advance
    //     （marker 行的正文列 = 源码正文列）；
    //   · 否则（物理续行）→ 该宽度叠加进 contentIndent（续行与所属项共享正文起点）。
    // 度量在组件测量阶段做而不是装饰层给死值：wrapWidth / caretX / run.x / 命中
    // 拿到的是同一个数，且随 DPI 缩放与行字体走。两端是文档字节偏移（增量平移
    // 时随 translateDecoration / decorationsEquivalent 折算）。
    int listIndentBeg = -1;
    int listIndentEnd = -1;

    float effectiveCellPadding() const {
        return cellPadding > 0.0f ? cellPadding : fontSize * kTableCellPaddingEm;
    }

    // 组件靠**内容比较**判断"要不要重新排版"（见 ensureLayoutCache），所以必须有相等语义。
    bool operator==(const LineDecoration& other) const {
        return std::fabs(fontSize - other.fontSize) < 0.001f &&
               std::fabs(lineHeight - other.lineHeight) < 0.001f &&
               colorEquals(textColor, other.textColor) &&
               fontFamily == other.fontFamily &&
               holes == other.holes &&
               runs == other.runs &&
               box == other.box &&
               glyph == other.glyph &&
               gutterGlyph == other.gutterGlyph &&
               std::fabs(contentIndent - other.contentIndent) < 0.001f &&
               std::fabs(textShiftY - other.textShiftY) < 0.001f &&
               std::fabs(spaceBefore - other.spaceBefore) < 0.001f &&
               imagePath == other.imagePath &&
               std::fabs(imageWidth - other.imageWidth) < 0.001f &&
               std::fabs(imageHeight - other.imageHeight) < 0.001f &&
               imageFailed == other.imageFailed &&
               imageFailText == other.imageFailText &&
               languageLabel == other.languageLabel &&
               hidden == other.hidden &&
               hiddenByFold == other.hiddenByFold &&
               cells == other.cells &&
               tableId == other.tableId &&
               std::fabs(effectiveCellPadding() - other.effectiveCellPadding()) < 0.001f &&
               tableHeaderRow == other.tableHeaderRow &&
               tableSeparator == other.tableSeparator &&
               listIndentBeg == other.listIndentBeg &&
               listIndentEnd == other.listIndentEnd;
    }
    bool operator!=(const LineDecoration& other) const { return !(*this == other); }
};

// 排版产出的样式段：视图层照着它逐段画。坐标已算好（相对本行/本段左端的 x 与宽度），
// 因为"每段多宽"取决于该段自己的字体参数，只能由测量阶段给出。
struct TextRun {
    int beg = 0;    // 文本绝对偏移（文档坐标），视图用它取该段的文字
    int end = 0;
    float x = 0.0f;      // 相对本行左端
    float width = 0.0f;
    LineRunStyle style;
};

// 返回"每个物理行一项"的装饰表（物理行 = 按 '\n' 切分的行）。
// 只吃 text，**是文本的纯函数**。Live Preview 约定"光标所在块显示原始源码"，那几行要清空 holes——
// 这件事由 provider 自己按当前光标算（用闭包捕获光标），组件不管"活动块"这个概念。
//
// 这样切分是为了性能：组件用内容比较判断装饰有没有变，于是
// **光标在同一块内移动 => 返回值逐字节相同 => 一次测量都不做**；
// 只有块切换（通常 2~6 行的装饰变了）才重排那几行。
using LineDecorationProvider = std::function<std::vector<LineDecoration>(const std::string& text)>;

// ── 编辑区间元数据（T4：装饰 / 布局增量的区间来源）────────────────────────
// 由**实际改文本的公共漏斗**在提交后写入：endEdit（普通编辑）、applyEditRecord
// （撤销 / 重做）。loadDocument 与组件外的 .value 回写把它清空 —— 那两条路没有
// 可信的前后对照，一律按"全量"处理。一次实际文本改变恰好对应一条记录，
// revision 记录提交后的 textRevision；消费方用「上一次构建的 revision + 1 ==
// 这条 revision」判断"这条元数据描述的正是上一次改动"，两次编辑之间必然断链。
//
// 行号都是 0 基**源行**（按 '\n' 切分，与 lp_plan.lines / 装饰表下标同源）：
//   firstLine   = byteBeg 所在行（编辑前后前缀 [0, byteBeg) 逐字节相同，两侧同号）；
//   newLastLine = 新文本里最后一个受影响的行（含）；
//   oldTailLine = 旧文本里第一个仍可整行复用的行（含）。
// 于是三段映射固定为：
//   新行 [0, firstLine)         ← 旧行 [0, firstLine)          原样复用（前缀未动）；
//   新行 [firstLine, newLastLine]                                 重算；
//   新行 [newLastLine+1, …)     ← 旧行 [oldTailLine, …)         复用并按
//       delta = newEnd - oldEnd 平移绝对偏移、按行数差平移行号。
// 复用前提 oldTailLine >= firstLine 与"两侧行数自洽"由消费方校验，不满足就全量。
struct PendingTextEdit {
    bool valid = false;
    unsigned long long revision = 0;  // 提交后的 InputState::textRevision
    int byteBeg = 0;
    int oldEnd = 0;
    int newEnd = 0;
    int firstLine = 0;
    int newLastLine = 0;
    int oldTailLine = 0;
    // 编辑**前**的光标（旧文本坐标）。装饰层用它找回"编辑前的活动块"——光标一旦
    // 离开旧块，那些行的标记显隐会翻转，必须跟着重算（否则与全量结果不一致）。
    int cursorBefore = 0;
};

// lineDecorator 回调携带的编辑上下文（T4 A2）。旧的单参 provider 仍然可用
// （input.h 提供重载包装），不认识编辑区间的调用方行为一字不变。
struct LineDecorationChanges;
struct DecoratorEditInfo {
    // 本次回调所给 text 对应的 InputState::textRevision。
    unsigned long long textRevision = 0;
    // false = 这份 text 是 IME 合成中的临时文本（displayState 的 preedit 副本）。
    // 它不是任何公共漏斗的产物，禁止据此做增量 —— 强制全量。
    bool committed = true;
    // nullptr = 没有可信的编辑区间（无编辑 / 已断链 / 外部赋值）→ 全量。
    const PendingTextEdit* edit = nullptr;
    // Optional output, reset for each provider call. Only cursor presentation
    // updates with unchanged committed text may publish a trusted transition.
    LineDecorationChanges* changes = nullptr;
};

// 带编辑上下文的 provider（T4 A2）。装饰层据此决定"区间增量"还是"全量重建"。
using LineDecorationProviderEx =
    std::function<std::vector<LineDecoration>(const std::string& text, const DecoratorEditInfo& info)>;

// A published table is immutable for its entire lifetime, including IME display states.
// Published generations own a directory of immutable pages, never their parent
// generation. Replacing rows copies only touched pages; unrelated payloads and
// their lifetimes are shared. Legacy vector providers use the same read view.
class LineDecorationTable {
public:
    static constexpr std::size_t pageSize = 64;
    using Page = std::vector<LineDecoration>;
    LineDecorationTable() = default;
    explicit LineDecorationTable(std::vector<LineDecoration> rows) : size_(rows.size()),
        pages_((size_ + pageSize - 1) / pageSize) {
        // Large plain/paragraph-heavy documents repeat the same decoration. A
        // uniform immutable page owns ONE exact row; source positions remain in
        // the plan/layout, never in a guessed decoration. Nonuniform pages retain
        // every row, including absolute ranges and syntax state.
        if (size_ >= 4096 && std::getenv("NEO_COMPACT_DECORATIONS_OFF") == nullptr) {
            std::vector<bool> uniform(pages_.size(), true);
            std::size_t uniformCount = 0;
            for (std::size_t page = 0; page < pages_.size(); ++page) {
                const auto first = page * pageSize;
                const auto end = std::min(size_, first + pageSize);
                for (auto row = first + 1; row < end; ++row) {
                    if (!exactlyEqual(rows[first], rows[row])) { uniform[page] = false; break; }
                }
                uniformCount += uniform[page] ? 1 : 0;
            }
            // Avoid repacking dense, unique decoration tables for negligible gain.
            if (uniformCount * 4 >= pages_.size()) {
                for (std::size_t page = 0; page < pages_.size(); ++page) {
                    const auto first = page * pageSize;
                    const auto end = std::min(size_, first + pageSize);
                    auto storage = std::make_shared<Page>();
                    storage->reserve(uniform[page] ? 1 : end - first);
                    if (uniform[page]) storage->push_back(std::move(rows[first]));
                    else for (auto row = first; row < end; ++row) storage->push_back(std::move(rows[row]));
                    pages_[page] = std::move(storage);
                }
                return; // discard the large builder vector, no parent/base retained
            }
        }
        base_ = std::make_shared<const Page>(std::move(rows));
    }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const LineDecoration& operator[](std::size_t row) const {
        const auto& page = pages_[row / pageSize];
        return page ? (*page)[page->size() == 1 ? 0 : row % pageSize] : (*base_)[row];
    }
    const Page* contiguousRows() const {
        return base_ && std::none_of(pages_.begin(), pages_.end(),
                                    [](const auto& page) { return bool(page); }) ? base_.get() : nullptr;
    }
    std::vector<LineDecoration> copyRows() const {
        std::vector<LineDecoration> result;
        result.reserve(size_);
        for (std::size_t row = 0; row < size_; ++row) result.push_back((*this)[row]);
        return result;
    }
    std::uint64_t residentCapacityBytes() const {
        std::uint64_t bytes = sizeof(*this) + pages_.capacity() * sizeof(pages_[0]);
        if (base_) bytes += pageBytes(*base_);
        for (const auto& page : pages_) if (page) bytes += pageBytes(*page);
        return bytes;
    }
    // rows is sorted/unique/in-range and proved against the unchanged source plan.
    // All replacements are prepared before publication. Copy gaps within touched
    // pages only, moving prepared rows instead of copying and discarding them.
    std::shared_ptr<const LineDecorationTable> replacing(
        const std::vector<int>& rows, std::vector<LineDecoration> patches,
        unsigned long long* copiedRows = nullptr) const {
        if (rows.size() != patches.size()) return {};
        int previous = -1;
        for (int row : rows) {
            if (row <= previous || row < 0 || static_cast<std::size_t>(row) >= size_) return {};
            previous = row;
        }
        auto result = std::make_shared<LineDecorationTable>(*this);
        std::size_t patch = 0;
        while (patch < rows.size()) {
            const auto pageIndex = static_cast<std::size_t>(rows[patch]) / pageSize;
            const auto length = std::min(pageSize, size_ - pageIndex * pageSize);
            auto page = std::make_shared<Page>();
            page->reserve(length);
            for (std::size_t offset = 0; offset < length; ++offset) {
                const auto row = pageIndex * pageSize + offset;
                if (patch < rows.size() && static_cast<std::size_t>(rows[patch]) == row) {
                    page->push_back(std::move(patches[patch++]));
                } else {
                    page->push_back((*this)[row]);
                    if (copiedRows) ++*copiedRows;
                }
            }
            result->pages_[pageIndex] = std::move(page);
        }
        if (std::all_of(result->pages_.begin(), result->pages_.end(),
                        [](const auto& page) { return bool(page); })) result->base_.reset();
        return result;
    }
private:
    static bool exactlyEqual(const LineDecoration& a, const LineDecoration& b) {
        // Legacy equality has sub-pixel tolerances. Storage sharing requires exact
        // geometry equality so close but distinct values are never rounded away.
        return a == b && a.fontSize == b.fontSize && a.lineHeight == b.lineHeight &&
            a.contentIndent == b.contentIndent && a.textShiftY == b.textShiftY &&
            a.spaceBefore == b.spaceBefore && a.imageWidth == b.imageWidth &&
            a.imageHeight == b.imageHeight && a.cellPadding == b.cellPadding &&
            a.glyph.advance == b.glyph.advance && a.gutterGlyph.advance == b.gutterGlyph.advance &&
            a.box.backgroundRadius == b.box.backgroundRadius && a.box.barWidth == b.box.barWidth;
    }
    static std::uint64_t pageBytes(const Page& rows) {
        std::uint64_t bytes = sizeof(Page) + rows.capacity() * sizeof(LineDecoration);
        for (const auto& row : rows) {
            bytes += row.fontFamily.capacity() + row.imagePath.capacity() + row.imageFailText.capacity() +
                row.languageLabel.capacity() + row.glyph.text.capacity() + row.gutterGlyph.text.capacity();
            bytes += row.holes.capacity() * sizeof(LineHole) + row.runs.capacity() * sizeof(LineRun) +
                row.cells.capacity() * sizeof(LineCell);
            for (const auto& run : row.runs) bytes += run.style.fontFamily.capacity();
        }
        return bytes;
    }
    std::size_t size_ = 0;
    // A partial generation may keep the full immutable base alive until its
    // final base slice is replaced. There is no chain of parent generations.
    std::shared_ptr<const Page> base_;
    std::vector<std::shared_ptr<const Page>> pages_;
};

class LineDecorationView {
public:
    LineDecorationView() = default;
    LineDecorationView(std::nullptr_t) {}
    LineDecorationView(const std::vector<LineDecoration>& rows) : vector_(&rows) {}
    LineDecorationView(const std::vector<LineDecoration>* rows) : vector_(rows) {}
    LineDecorationView(const LineDecorationTable& rows) : table_(&rows) {}
    LineDecorationView(const LineDecorationTable* rows) : table_(rows) {}
    const void* identity() const { return table_ ? static_cast<const void*>(table_) : vector_; }
    explicit operator bool() const { return identity() != nullptr; }
    bool operator==(std::nullptr_t) const { return !identity(); }
    bool operator!=(std::nullptr_t) const { return identity() != nullptr; }
    const LineDecorationView* operator->() const { return this; }
    const LineDecorationView& operator*() const { return *this; }
    std::size_t size() const { return table_ ? table_->size() : vector_ ? vector_->size() : 0; }
    bool empty() const { return size() == 0; }
    const LineDecoration& operator[](std::size_t row) const {
        return table_ ? (*table_)[row] : (*vector_)[row];
    }
    std::vector<LineDecoration> copyRows() const {
        return table_ ? table_->copyRows() : vector_ ? *vector_ : std::vector<LineDecoration>{};
    }
    class Iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = LineDecoration;
        using difference_type = std::ptrdiff_t;
        using pointer = const LineDecoration*;
        using reference = const LineDecoration&;
        Iterator() = default;
        Iterator(const std::vector<LineDecoration>* v, const LineDecorationTable* t, difference_type i)
            : vector_(v), table_(t), index_(i) {}
        reference operator*() const { return table_ ? (*table_)[index_] : (*vector_)[index_]; }
        pointer operator->() const { return &**this; }
        reference operator[](difference_type n) const { return *(*this + n); }
        Iterator& operator++() { ++index_; return *this; }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        Iterator& operator--() { --index_; return *this; }
        Iterator operator--(int) { auto old = *this; --*this; return old; }
        Iterator& operator+=(difference_type n) { index_ += n; return *this; }
        Iterator& operator-=(difference_type n) { index_ -= n; return *this; }
        Iterator operator+(difference_type n) const { auto i = *this; return i += n; }
        Iterator operator-(difference_type n) const { auto i = *this; return i -= n; }
        friend Iterator operator+(difference_type n, Iterator i) { return i += n; }
        difference_type operator-(const Iterator& other) const { return index_ - other.index_; }
        bool operator==(const Iterator& other) const { return index_ == other.index_ && vector_ == other.vector_ && table_ == other.table_; }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
        bool operator<(const Iterator& other) const { return index_ < other.index_; }
        bool operator>(const Iterator& other) const { return other < *this; }
        bool operator<=(const Iterator& other) const { return !(other < *this); }
        bool operator>=(const Iterator& other) const { return !(*this < other); }
    private:
        const std::vector<LineDecoration>* vector_ = nullptr;
        const LineDecorationTable* table_ = nullptr;
        difference_type index_ = 0;
    };
    Iterator begin() const { return {vector_, table_, 0}; }
    Iterator end() const { return {vector_, table_, static_cast<std::ptrdiff_t>(size())}; }
    bool operator==(LineDecorationView other) const {
        return identity() == other.identity() || (size() == other.size() && std::equal(begin(), end(), other.begin()));
    }
    bool operator!=(LineDecorationView other) const { return !(*this == other); }
private:
    const std::vector<LineDecoration>* vector_ = nullptr;
    const LineDecorationTable* table_ = nullptr;
};
inline bool operator==(const LineDecorationTable& a, const LineDecorationTable& b) {
    return LineDecorationView(a) == LineDecorationView(b);
}
inline bool operator==(const LineDecorationTable& a, const std::vector<LineDecoration>& b) {
    return LineDecorationView(a) == LineDecorationView(b);
}
inline bool operator==(const std::vector<LineDecoration>& a, const LineDecorationTable& b) { return b == a; }
inline bool operator!=(const LineDecorationTable& a, const LineDecorationTable& b) { return !(a == b); }
inline bool operator!=(const LineDecorationTable& a, const std::vector<LineDecoration>& b) { return !(a == b); }
inline bool operator!=(const std::vector<LineDecoration>& a, const LineDecorationTable& b) { return !(a == b); }
using LineDecorationSnapshot = std::shared_ptr<const LineDecorationTable>;
struct LineDecorationChanges {
    std::weak_ptr<const LineDecorationTable> previous;
    std::weak_ptr<const LineDecorationTable> next;
    unsigned long long textRevision = 0;
    std::vector<int> rows;  // sorted actual changes, zero-based source lines
};
using LineDecorationSnapshotProvider =
    std::function<LineDecorationSnapshot(const std::string&, const DecoratorEditInfo&)>;

struct InputModel {
    // A table row can have multiple document positions at one projected offset
    // (hidden pipes/spaces), and aligned columns can assign those positions
    // different x coordinates. Keep this editor mapping beside, not inside, the
    // renderer's single-valued projected metrics.
    struct TableDocCaretStop {
        int byteIndex = 0;
        float x = 0.0f;
        int column = 0;
    };

    struct TextLine {
        int start = 0;
        int end = 0;
        bool hardBreakAfter = false;
        core::TextPrimitive::TextMetrics metrics;

        // ── Live Preview 的逐行排版元数据 ──
        // metrics 对应的是**投影后文本**（本行的 holes 被删掉之后的那串字），下面这些是它的参数。
        // 不使用装饰时它们等于控件统一值，排版结果与改造前逐字节一致。
        float fontSize = 0.0f;    // 该行字号（> 0）
        float lineHeight = 0.0f;  // 该行行高（> 0）
        float top = 0.0f;         // 行顶相对内容区原点的 y（前缀和，由 ensureLayoutCache 统一写入）
        // 本行隐藏的区间。**与 start/end 同一坐标系（文本绝对偏移）**，空 = 本行不隐藏任何字节。
        std::vector<LineHole> holes;
        // 行级文字色（alpha == 0 = 用控件默认）。标题/代码块靠它上色。
        core::Color color{0.0f, 0.0f, 0.0f, 0.0f};
        // 本行的样式段（**已按投影文本排完版**，含"整段覆盖"的默认段）。空 = 该行没有样式段，
        // 视图走"整行一个文本图元"的老路径（未使用装饰时逐字节等价于改造前）。
        std::vector<TextRun> runs;
        // 行级矩形（代码块底色 / 引用竖条）。默认值 = 什么都不画，所以不使用装饰时与改造前等价。
        LineBoxStyle box;
        // 行首图元（任务复选框）。同样默认不画；注意它**已经**把 metrics.width 与
        // 各 caretX / run.x 平移过了（见 appendMeasuredLine），视图层只需按 x=0 画图标。
        LineGlyph glyph;
        // 行号位图元（折叠箭头）。默认不画；只在 lineStart 段的行号槽里画，
        // 顶替该行的行号数字。
        LineGlyph gutterGlyph;
        // 块级图片（S3f 批次 B）。非空 = 该行文字整体藏进 holes、原位画这张图。
        // 只在 lineStart 段画（软换行的续行沿用行高但不重复画，v1 已知限制）。
        std::string imagePath;
        float imageWidth = 0.0f;
        float imageHeight = 0.0f;
        // 图片失败态占位盒（S3f 批次 F）。imageFailed 时 imagePath 为空、
        // 视图按 imageWidth/Height 画占位盒；imageFailText 是盒内显示的路径。
        bool imageFailed = false;
        std::string imageFailText;
        // 行号（源文档第几行，从 1 起）。软换行把一个源行拆成多条可视行，
        // 它们共享同一个 lineNumber；lineStart 只在第一条上为 true，
        // 视图层用它决定"画不画行号"。
        int lineNumber = 1;
        bool lineStart = true;
        // 折叠隐藏（S3f 批次 C）。放在末尾：:1087 那处聚合初始化按位置取前几个字段，
        // 新字段带 NSDMI，partial-init 下取默认值，不受影响。
        bool hidden = false;
        // 「折叠致隐」原始值（光标行强制可见时 hidden=false 而 hiddenByFold 仍为 true），
        // onRevealHiddenLine 的自愈检查看它。同样放末尾、NSDMI。
        bool hiddenByFold = false;
        // 行内容左缩进（装饰层给，测量时已把 metrics/runs 平移过，渲染层只用来
        // 给"没有样式段的整行文本图元"补同一个偏移）。放末尾：partial-init 聚合
        // 初始化不受影响。
        float contentIndent = 0.0f;
        // 行内文字在行盒内的整体下移（像素，0 = 顶对齐）。渲染层画文字/行首图元、
        // 几何层算光标 Y 都要加上它。放末尾、NSDMI，partial-init 不受影响。
        float textShiftY = 0.0f;
        // 表格行（2026-09-25 网格线）：本行所属表格的 id（渲染层据此查列几何画
        // 网格）与"是否分隔行"。同上放末尾、NSDMI，partial-init 下取默认值。
        int tableId = -1;
        bool tableSeparator = false;
        // 行级字体（空 = 控件字体；代码块等宽从这里来）。渲染层的整行文字图元与
        // 没有自带字体的样式段都以它为准，测量层已经在 appendMeasuredLineCore 里
        // 用同一份字面算过 metrics。放末尾、NSDMI，partial-init 不受影响。
        std::string fontFamily;
        // 本行**之前**的块间距（像素，0 = 首尾相接）。只在本源行的第一条可视段上盖章
        // （软换行的续行不重复），隐藏行恒 0。几何表把它当行间 gap 用，行高/文字位置
        // 一点都不受影响。放末尾、NSDMI，partial-init 不受影响。
        float spaceBefore = 0.0f;
        // 代码块起始围栏行的语言标签（渲染层右对齐画在底色条右上角）。放末尾、NSDMI。
        std::string languageLabel;
        // Exact row geometry stays resident. Non-table caret arrays are materialized
        // only for the viewport or an explicit hit/navigation request.
        bool metricsDeferred = false;
        bool metricsTracked = false;
        // 表格行的命中 clamp（2026-10-06）：本行各单元格的文档字节区间（与装饰层
        // LineCell 同源，取前 columns 个）。格与格分界处上一格末尾与下一格开头共享
        // 同一个投影停靠点，其字节经"歧义归洞之前"总解析到上一格 —— pointerHit
        // 用这份区间把命中字节 clamp 回点击所在列。非表格行为空。放末尾、NSDMI。
        std::vector<LineCell> tableCellRanges;
        // Exact source-byte caret positions for this visual table segment. Unlike
        // metrics.byteIndices, this preserves both sides of hidden cell borders.
        std::vector<TableDocCaretStop> tableDocCaretStops;
    };

    struct TextSelectionRect {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        // 该行的**文字带高**（lineTextBand 给的 height，不含 height 里那 1px 粘连
        // 补偿）：组合下划线贴着带底画，跟着文字而不是跟着带粘连的背景框走。
        float lineHeight = 0.0f;
    };

    // ── 增量编辑记录（T3）────────────────────────────────────────────────────
    // 一条记录 = **一个连续 span** 的前后对照：把旧文本 [beg, beg+removed.size()) 换成
    // inserted，并带上编辑前后的光标/选区。
    //   undo：把 inserted 换回 removed，光标/选区回到 *Before；
    //   redo：把 removed 换回 inserted，光标/选区回到 *After。
    // 正逆应用的是**同一条记录**，撤销栈与重做栈因此可以互相持有同一个对象。
    //
    // 为什么不再存全文：改造前的 EditSnapshot 保存整篇文本，深度 128 = 1MB 文档要留
    // 128MB；现在一条记录只留"真正变过的那一小段"的前后字节。
    struct EditRecord {
        int beg = 0;
        std::string removed;
        std::string inserted;
        int cursorBefore = 0;
        int selectionStartBefore = 0;
        int selectionEndBefore = 0;
        int cursorAfter = 0;
        int selectionStartAfter = 0;
        int selectionEndAfter = 0;
    };

// ── 表格列计划（S3f 批次 D）──────────────────────────────────────────────
// 列对齐的全部秘密：同一张表的每行都按**同一套列 x**摆放自己的格子。
// 列宽先由"整表逐格测量取逐列最大值"得到，再夹逼到 [min, max] 并压进可用宽度
// （决策③：压进 textWidth；2026-09-26 起超宽内容在**格内换行**，不再截断）。
// 2026-09-25 起提升到 InputState 之前：InputState.cachedTables 与 InputLayout.tables
// 都要引用它。
struct TableColumns {
    int tableId = -1;
    std::vector<float> x;      // 各列文字左端（相对行内文本原点）
    std::vector<float> width;  // 各列可用宽度（超出在格内换行）
    float padding = 0.0f;
    float total = 0.0f;        // 整表占宽（末列右端）

    int count() const { return static_cast<int>(width.size()); }
};

// Store vector positions, never pointers: appending tables may relocate the vector.
// IDs are source anchors and may be sparse or change after an edit.
using TableColumnIndex = std::unordered_map<int, std::size_t>;

    struct InputState {
        std::string text;
        std::string compositionText;
        int cursor = 0;
        int selectionStart = 0;
        int selectionEnd = 0;
        int dragAnchor = 0;
        bool selecting = false;        // 指针按住（按下到松开）拖选中
        // 指针按下**前**的光标位置。对齐 Obsidian：拖拽选字期间"光标附近的标记显隐"
        // 冻结在这个值上，松手后再按实时光标判断 —— 否则拖选时布局跟着光标一路变形。
        int cursorBeforePress = 0;
        bool hasPreferredCursorX = false;
        bool followCaret = true;
        float preferredCursorX = 0.0f;
        float horizontalScroll = 0.0f;
        bool wordWrap = true;
        float verticalScroll = 0.0f;
        float scrollbarDragOffset = 0.0f;
        float scrollbarDragScale = 1.0f;
        unsigned long long textRevision = 0;
        unsigned long long compositionRevision = 0;
        core::Rect lastBounds;
        unsigned long long cachedTextRevision = static_cast<unsigned long long>(-1);
        std::string cachedFontFamily;
        float cachedFontSize = 0.0f;
        float cachedLayoutPixelScale = 0.0f;
        float cachedViewportWidth = -1.0f;
        bool cachedMultiline = false;
        std::string cachedLayoutText;
        core::TextPrimitive::TextMetrics cachedMetrics;
        std::vector<TextLine> cachedLines;
        bool viewportMetrics = false; // opt-in: document editor, not single-line UI
        bool cachedViewportMetrics = false;
        bool detailTrackingValid = false;
        std::vector<std::size_t> detailedRows;
        // cachedLines 的逐行 top/height 前缀和（行高不再全局统一，y 由它查表得到）。
        LineGeometryTable cachedGeometry;
        // 表格列几何（2026-09-25 网格线）：measureLines 顺手存下，渲染层画网格用。
        // Incremental layout preserves unchanged tables and remeasures changed table blocks.
        std::vector<TableColumns> cachedTables;
        TableColumnIndex cachedTableIndex;
        // Unconstrained widths: width-only reflows reuse the complete plan; incremental
        // edits preserve proven-equivalent table groups and replace changed groups.
        std::vector<TableColumns> cachedTableIntrinsic;
        float cachedTextWidth = 0.0f;
        bool layoutCacheValid = false;
        // ── Live Preview 装饰 ──
        // Legacy providers own a vector; snapshot providers leave it empty and share an
        // immutable generation. Read either representation through activeDecorations().
        std::vector<LineDecoration> decorations;
        LineDecorationSnapshot decorationSnapshot;
        // 装饰内容的版本号。**必须并进渲染用的 dirtyKey**：框架把 dirtyKey 当"内容签名"用
        // （key 不变就认为文本没变，于是不更新图元文本），而光标移动并不改变 textRevision——
        // 少了它，"活动块切换后这一行该显示源码/该隐藏标记"就不会反映到画面上。
        unsigned long long decorationRevision = 0;
        // ── 鼠标悬停 ──
        // 指针在控件内的位置（控件本地逻辑坐标，onMove 维护）与映射到的可视行。
        // 行号位的折叠箭头只在悬停行上显示（对齐 Obsidian），其余行显示行号。
        bool pointerHoverValid = false;
        float pointerHoverY = 0.0f;
        int pointerHoverLine = -1;
        int pressedGlyphLine = -1;
        int pointerHoverLinkBeg = -1;
        int pointerHoverLinkEnd = -1;
        int pressedLinkByte = -1;
        int pressedLinkLine = -1;
        float pressedLinkX = 0.0f;
        float pressedLinkY = 0.0f;
        int contextLinkByte = -1;
        // ── 光标闪烁 ──
        // 可见性 + "活动签名"。签名一变（输入、移动光标、改选区）就立刻变可见，
        // 并让紧接着的那一拍不翻转 —— 于是连续打字时是常亮的、停下来才闪。
        bool caretBlinkVisible = true;
        bool caretBlinkPendingReset = false;
        bool caretBlinkInitialized = false;
        int caretBlinkCursor = -1;
        int caretBlinkSelectionStart = -1;
        int caretBlinkSelectionEnd = -1;
        unsigned long long caretBlinkTextRevision = static_cast<unsigned long long>(-1);
        unsigned long long caretBlinkCompositionRevision = static_cast<unsigned long long>(-1);
        std::vector<EditRecord> undoStack;
        std::vector<EditRecord> redoStack;
        // ── 进行中的编辑捕获（T3）──────────────────────────────────────────
        // beginEdit 打开、endEdit 提交后立刻释放：只在"一次用户编辑"的执行期间存在，
        // 不属于任何长期状态 —— displayState 的预编辑副本是全新构造的 InputState，
        // 这些字段连同撤销/重做栈都不会被复制过去（历史只留在真身 state 上）。
        int editDepth = 0;          // > 0 = 捕获中；嵌套 begin 只计数（组合编辑只出一条记录）
        std::string editText;       // 捕获区间 [editBeg, editEnd) 的编辑前原文
        int editBeg = 0;
        int editEnd = 0;
        int editFullSize = 0;       // 编辑前整篇长度：用来推 span 在编辑后的长度
        int editCursorBefore = 0;
        int editSelectionStartBefore = 0;
        int editSelectionEndBefore = 0;
        // 预编辑只持有展示状态，不修改文档/撤销历史；提交或取消后立即释放。
        std::unique_ptr<InputState> preedit;
        // ── 待消费的编辑区间（T4）────────────────────────────────────────────
        // 每次真正改文本由公共漏斗覆写（见 PendingTextEdit），换文档 / 组件外赋值
        // 清空。它不属于撤销历史，但与 textRevision 一样跨帧存活，供 lineDecorator
        // 的回调把区间递给装饰层。displayState 的 preedit 是全新构造的副本，
        // 这里天然是 invalid —— 合成期的临时文本因此永远走全量。
        PendingTextEdit pendingEdit;
    };

    // 指针按下命中的完整结果（S3f 批次 A：组件把"点到了哪里"回调给应用层）。
    // byteIndex 与 cursorFromPointer 同源同值（IME 合成区间由调用方用 documentIndex 映射）；
    // lineX 是命中点在该行文本坐标系里的 x（已含 inset/横向滚动修正）；
    // onGlyph = 落在行首图元 [0, advance) 内 —— 任务复选框这类"行首可点图元"的判定。
    struct PointerHit {
        int byteIndex = 0;
        int lineIndex = 0;
        // 命中排版行的**源行号**（1 起，软换行的续行共享同号）：应用层拿它回查
        // lp_plan（plan 的行号是 0 基源行）——排版行号 ≠ 源行号（前面的行会软换行）。
        int lineNumber = 1;
        float lineX = 0.0f;
        bool onGlyph = false;
        // 点在文本原点左侧（行号列 / 左缘留白，S3f 批次 E）：标题行用它做"点击折叠"。
        // 仅多行模式置位 —— 单行没有行号列，横向滚动后 targetX<0 也不是 gutter。
        bool onGutter = false;
        // 点在链接段内（2026-09-26）：落点字节落在带 link 样式的段区间里（两端边界
        // 也算）。应用层拿 byteIndex 回查 lp_plan 找链接目标，单击跳转 / 右键编辑。
        bool onLink = false;
        int linkBeg = -1;
        int linkEnd = -1;
        // 点在块级图片上（S3f 批次 E，onImage 行的 [0, imageWidth] 区间）：
        // "点图看大图"，组件按下时要跳过移光标，应用层据此打开全屏预览。
        bool onImage = false;
    };

    struct InputLayout {
        using Line = TextLine;
        using SelectionRect = TextSelectionRect;

        core::TextPrimitive::TextMetrics metrics;
        const std::vector<Line>* lines = nullptr;
        // 逐行几何（指向 state.cachedGeometry，生命周期由调用方保证，与 lines 同）。
        const LineGeometryTable* geometry = nullptr;
        InputState* owner = nullptr; // same lifetime as lines/geometry
        // 表格列几何（指向 state.cachedTables，同上生命周期）。空 = 本文档没有表格。
        const std::vector<TableColumns>* tables = nullptr;
        const TableColumnIndex* tableIndex = nullptr;

        const TableColumns* tableColumnsFor(int tableId) const {
            return tables && tableIndex ? findTableColumns(*tables, *tableIndex, tableId) : nullptr;
        }
        std::vector<SelectionRect> selectionRects;
        float viewportWidth = 0.0f;
        float viewportHeight = 0.0f;
        float controlWidth = 0.0f;
        float inset = 0.0f;
        // 右缘内缩。与 inset 分开是因为行号列只占左侧（inset = 左内缩 + 行号列宽）。
        float rightInset = 0.0f;
        float textTop = 0.0f;
        // 控件默认行高。未装饰时所有行都是它；装饰后逐行不同，请改用 geometry。
        float lineHeight = 0.0f;
        float scroll = 0.0f;
        float textWidth = 0.0f;
        float contentHeight = 0.0f;
        float cursorPixel = 0.0f;
        float cursorX = 0.0f;
        float cursorY = 0.0f;
        float visibleTextWidth = 0.0f;
        float maxVerticalScroll = 0.0f;
        int selectionStart = 0;
        int selectionEnd = 0;
        int cursorLine = 0;
        bool multiline = false;
        float clippedSelectionX = 0.0f;
        float clippedSelectionWidth = 0.0f;

        static InputLayout build(InputState& state,
                                 float viewportWidth,
                                 float viewportHeight,
                                 float controlWidth,
                                 float inset,
                                 float rightInset,
                                 float textTop,
                                 float lineHeight,
                                 const std::string& fontFamily,
                                 float fontSize,
                                 bool multiline,
                                 LineDecorationView decorations = {},
                                 const LineDecorationSnapshot& snapshot = {},
                                 const LineDecorationChanges* changes = nullptr) {
            InputLayout layout;
            layout.viewportWidth = viewportWidth;
            layout.viewportHeight = viewportHeight;
            layout.controlWidth = controlWidth;
            layout.inset = inset;
            layout.rightInset = rightInset;
            layout.textTop = textTop;
            layout.lineHeight = lineHeight;
            layout.multiline = multiline;
            ensureLayoutCache(state, fontFamily, fontSize, viewportWidth, multiline, decorations, snapshot, changes);
            layout.owner = &state;
            resetDetailTracking(state);
            layout.lines = &state.cachedLines;
            layout.geometry = &state.cachedGeometry;
            // 表格列几何（画网格用）：empty 时给 nullptr，渲染层免判表。
            layout.tables = state.cachedTables.empty() ? nullptr : &state.cachedTables;
            layout.tableIndex = layout.tables ? &state.cachedTableIndex : nullptr;

            if (multiline) {
                layout.cursorLine = layout.lineIndexFor(state.cursor);
                ensureLineDetails(state, layout.cursorLine);
                const Line& cursorLine = layout.lineList()[static_cast<size_t>(layout.cursorLine)];
                layout.metrics = cursorLine.metrics;
                if(state.wordWrap) state.horizontalScroll = 0.0f;
                else {
                    const float maximum=std::max(0.0f,state.cachedTextWidth-viewportWidth+fontSize);
                    if(state.followCaret) {
                        const float x=caretXInLine(cursorLine,state.cursor);
                        if(x<state.horizontalScroll) state.horizontalScroll=x;
                        else if(x>state.horizontalScroll+viewportWidth-fontSize) state.horizontalScroll=x-viewportWidth+fontSize;
                    }
                    state.horizontalScroll=std::clamp(state.horizontalScroll,0.0f,maximum);
                }
                layout.scroll = state.horizontalScroll;
                layout.textWidth = state.cachedTextWidth;
                layout.contentHeight = layout.geometryTable().total();
                layout.cursorPixel = caretXInLine(cursorLine, state.cursor);
                layout.cursorX = inset + layout.cursorPixel - layout.scroll;
                layout.maxVerticalScroll = std::max(0.0f, layout.contentHeight - viewportHeight);
                state.verticalScroll = std::clamp(state.verticalScroll, 0.0f, layout.maxVerticalScroll);
                if (state.followCaret) {
                    syncVerticalScroll(state, layout, viewportHeight);
                }
                state.verticalScroll = std::clamp(state.verticalScroll, 0.0f, layout.maxVerticalScroll);
                layout.currentVerticalScroll = state.verticalScroll;
                prepareViewportDetails(state, viewportHeight);
                // 光标 Y 跟随行内文字下移量（标题行的 padding-top 在文字上方，
                // 光标要对着文字而不是行盒顶）。
                const std::vector<Line>& cursorLineList = layout.lineList();
                const float cursorTextShift =
                    layout.cursorLine >= 0 && layout.cursorLine < static_cast<int>(cursorLineList.size())
                        ? cursorLineList[static_cast<std::size_t>(layout.cursorLine)].textShiftY
                        : 0.0f;
                const auto cursorBand = lineTextBand(layout.geometryTable().top(layout.cursorLine),
                                                      layout.geometryTable().height(layout.cursorLine), cursorTextShift);
                const float caretHeight = layout.cursorLineFontSize() * 1.18f;
                layout.cursorY = textTop + cursorBand.top +
                                 std::max(0.0f, cursorBand.height - caretHeight) * 0.5f - state.verticalScroll;
                layout.visibleTextWidth = state.wordWrap ? viewportWidth : std::max(viewportWidth, state.cachedTextWidth + fontSize);
            } else {
                layout.metrics = state.cachedMetrics;
                syncScroll(state, viewportWidth, layout.metrics, fontSize);
                state.verticalScroll = 0.0f;
                layout.cursorLine = 0;
                layout.scroll = state.horizontalScroll;
                layout.textWidth = layout.metrics.width;
                layout.contentHeight = lineHeight;
                layout.cursorPixel = caretX(layout.metrics, state.cursor);
                layout.cursorX = inset + layout.cursorPixel - layout.scroll;
                layout.cursorY = textTop;
                layout.currentVerticalScroll = 0.0f;
                layout.visibleTextWidth = std::max(viewportWidth, layout.textWidth + 24.0f);
            }

            const auto selection = selectionRange(state);
            layout.selectionStart = selection.first;
            layout.selectionEnd = selection.second;
            layout.buildSelectionRects(selection.first, selection.second);
            return layout;
        }

        const std::vector<Line>& lineList() const {
            static const std::vector<Line> emptyLines;
            return lines ? *lines : emptyLines;
        }

        const LineGeometryTable& geometryTable() const {
            static const LineGeometryTable emptyTable;
            return geometry ? *geometry : emptyTable;
        }

        // 光标所在行的行高 / 字号。行高不再全局统一，光标、IME 矩形、组合下划线都要用它。
        float cursorLineHeight() const {
            const LineGeometryTable& table = geometryTable();
            if (table.count() == 0) {
                return lineHeight;
            }
            return table.height(cursorLine);
        }

        float cursorLineFontSize() const {
            const std::vector<Line>& list = lineList();
            if (list.empty()) {
                return 0.0f;
            }
            return list[static_cast<std::size_t>(std::clamp(cursorLine, 0, static_cast<int>(list.size()) - 1))].fontSize;
        }

        float xFor(int byteIndex) const {
            const int lineIndex = lineIndexFor(byteIndex);
            if (owner) ensureLineDetails(*owner, lineIndex);
            const Line& line = lineList()[static_cast<size_t>(lineIndex)];
            return caretXInLine(line, byteIndex);
        }

        float clampedCursorX() const {
            return std::clamp(cursorX, inset, std::max(inset, controlWidth - rightInset));
        }

        int cursorFromPointer(double pointerX, double pointerY, const core::Rect& bounds, float width, float inputInset) const {
            return pointerHit(pointerX, pointerY, bounds, width, inputInset).byteIndex;
        }

        // 命中的完整结果。cursorFromPointer 是它的 byteIndex 视图 —— 收口到同一处，
        // "回调给应用的字节位置 == cursorFromPointer 的结果"就恒成立，不靠两处同步。
        PointerHit pointerHit(double pointerX, double pointerY, const core::Rect& bounds, float width, float inputInset) const {
            const float scale = width > 0.0f ? bounds.width / width : 1.0f;
            const float localX = static_cast<float>((pointerX - bounds.x) / std::max(0.001f, scale));
            const float localY = static_cast<float>((pointerY - bounds.y) / std::max(0.001f, scale));
            const int lineIndex = multiline ? lineIndexFromY(localY + currentVerticalScroll) : 0;
            const float targetX = localX - inputInset + scroll;
            PointerHit hit;
            hit.lineIndex = lineIndex;
            hit.lineX = targetX;
            hit.byteIndex = closestCaret(lineIndex, targetX);
            const std::vector<Line>& lineListRef = lineList();
            if (lineIndex >= 0 && lineIndex < static_cast<int>(lineListRef.size())) {
                const Line& line = lineListRef[static_cast<std::size_t>(lineIndex)];
                hit.lineNumber = line.lineNumber;
                hit.onGlyph = line.lineStart && line.glyph.codepoint != 0 &&
                              targetX >= line.contentIndent &&
                              targetX < line.contentIndent + (line.glyph.checkbox
                                  ? std::min(line.fontSize, line.glyph.advance) : line.glyph.advance);
                // 链接命中（2026-09-26）：落点字节落在带 link 样式的段区间里（段两端
                // 边界也算 —— caret 恰好停在段边界时热区与可见文字保持一致）。
                for (const TextRun& run : line.runs) {
                    if (run.style.link && targetX >= run.x && targetX < run.x + run.width) {
                        hit.onLink = true;
                        hit.linkBeg = run.beg;
                        hit.linkEnd = run.end;
                        // The nearest caret can be the exclusive right boundary.
                        hit.byteIndex = std::clamp(hit.byteIndex, run.beg, std::max(run.beg, run.end - 1));
                        break;
                    }
                }
                // 行号列（文本原点左侧）。单行模式没有 gutter，targetX<0 只是滚动后的空区。
                hit.onGutter = multiline && targetX < 0.0f;
                // 块级图片（S3f 批次 E）：行框内 [0, imageWidth] 都是图的点击区
                // （行高含上下留白，留白也算——图就是这一行）。
                hit.onImage = line.lineStart && !line.imagePath.empty() &&
                              targetX >= 0.0f && targetX <= line.imageWidth;
            }
            return hit;
        }

        int closestCaret(int lineIndex, float targetX) const {
            if (owner) ensureLineDetails(*owner, lineIndex);
            const std::vector<Line>& lineListRef = lineList();
            if (lineListRef.empty()) {
                return 0;
            }
            const Line& line = lineListRef[static_cast<size_t>(std::clamp(lineIndex, 0, static_cast<int>(lineListRef.size()) - 1))];
            const int tableHit = tableDocOffsetForX(line, targetX, tableColumnsFor(line.tableId));
            if (tableHit >= 0) return tableHit;
            return docOffsetForX(line.metrics, line.holes, line.start, targetX);
        }

        // 表格换行续段的判定辅助（2026-09-26）：byteIndex 在这一段里有没有 caret
        // 停靠点。同一源行的各续段共享同一段字节区间，停靠点子集互不相同。
        bool lineHasCaretStop(const Line& line, int byteIndex) const {
            return lineHasDocCaretStop(line, byteIndex);
        }

        int lineIndexFor(int byteIndex) const {
            const std::vector<Line>& lineListRef = lineList();
            if (lineListRef.empty()) {
                return 0;
            }
            const auto it = std::upper_bound(
                lineListRef.begin(),
                lineListRef.end(),
                byteIndex,
                [](int value, const Line& line) {
                    return value < line.start;
                });
            int index = it == lineListRef.begin()
                ? 0
                : static_cast<int>(std::distance(lineListRef.begin(), it)) - 1;
            index = std::clamp(index, 0, static_cast<int>(lineListRef.size()) - 1);
            if (index + 1 < static_cast<int>(lineListRef.size()) &&
                !lineListRef[static_cast<size_t>(index)].hardBreakAfter &&
                byteIndex >= lineListRef[static_cast<size_t>(index)].end) {
                ++index;
            }
            // 表格换行续段（2026-09-26）：同一源行的多条可视行共享同一段字节区间
            // （单元格在列内折行），upper_bound 只能落到其中的**最后一段**。各段的
            // caret 表持有互不相同的停靠点子集 —— 向回走到真正持有这个字节的那一段；
            // 一路都没有就停在段首（行首段有 lineStart 挡着），光标 y 仍在本行内。
            // 软换行的分段字节区间互斥（start 各不相同），不会走进这个循环。
            while (index > 0 && !lineListRef[static_cast<size_t>(index)].lineStart &&
                   lineListRef[static_cast<size_t>(index - 1)].start ==
                       lineListRef[static_cast<size_t>(index)].start &&
                   lineListRef[static_cast<size_t>(index - 1)].end ==
                       lineListRef[static_cast<size_t>(index)].end &&
                   !lineHasCaretStop(lineListRef[static_cast<size_t>(index)], byteIndex)) {
                --index;
            }
            return std::clamp(index, 0, static_cast<int>(lineListRef.size()) - 1);
        }

        int lineIndexFromY(float localY) const {
            const std::vector<Line>& lineListRef = lineList();
            const LineGeometryTable& table = geometryTable();
            if (lineListRef.empty() || table.count() == 0 || table.total() <= 0.0f) {
                return 0;
            }
            return visibleLineFrom(std::clamp(table.lineAtY(localY - textTop), 0,
                                              static_cast<int>(lineListRef.size()) - 1));
        }

        // 折叠（S3f 批次 C）：0 高的 hidden 行永远不作为命中/可视行返回。
        // lineAtY 对"连续相同 top"二分会落进折叠段；折叠段下方的可见行与它共享同一
        // top，所以优先向下走，走不通（折叠吃到了文档末尾）再向上找。
        int visibleLineFrom(int index) const {
            const std::vector<Line>& lineListRef = lineList();
            const LineGeometryTable& table = geometryTable();
            const int count = static_cast<int>(lineListRef.size());
            int idx = std::clamp(index, 0, std::max(0, count - 1));
            if (count == 0 || !lineListRef[static_cast<std::size_t>(idx)].hidden) {
                return idx;
            }
            const int start = idx;
            while (idx + 1 < count && lineListRef[static_cast<std::size_t>(idx)].hidden &&
                   table.top(idx + 1) <= table.top(idx) + 0.001f) {
                ++idx;
            }
            if (!lineListRef[static_cast<std::size_t>(idx)].hidden) {
                return idx;
            }
            idx = start;
            while (idx > 0 && lineListRef[static_cast<std::size_t>(idx)].hidden) {
                --idx;
            }
            return lineListRef[static_cast<std::size_t>(idx)].hidden ? start : idx;
        }

        float currentVerticalScroll = 0.0f;

        void buildSelectionRects(int startIndex, int endIndex) {
            const std::vector<Line>& lineListRef = lineList();
            if (startIndex == endIndex || lineListRef.empty()) {
                return;
            }
            if (startIndex > endIndex) {
                std::swap(startIndex, endIndex);
            }

            const LineGeometryTable& table = geometryTable();
            // 单行输入没有几何表（cachedGeometry.build({})）：top/height 回退到
            // 常量行高，否则选区矩形高度恒为 0，整个选区不可见。
            const bool hasGeometry = table.count() > 0;
            const float clipLeft = inset;
            const float clipRight = std::max(inset, controlWidth - rightInset);
            const int firstSelectedLine = lineIndexFor(startIndex);
            const int lastSelectedLine = lineIndexFor(std::max(startIndex, endIndex - 1));
            const int firstVisibleLine = hasGeometry
                ? table.firstVisibleLine(currentVerticalScroll)
                : 0;
            const int lastVisibleLine = hasGeometry
                ? table.lastVisibleLine(currentVerticalScroll, viewportHeight)
                : static_cast<int>(lineListRef.size()) - 1;
            const int firstLine = std::max(firstSelectedLine, firstVisibleLine);
            const int lastLine = std::min(lastSelectedLine, lastVisibleLine);
            for (int lineIndex = firstLine; lineIndex <= lastLine; ++lineIndex) {
                const size_t i = static_cast<size_t>(lineIndex);
                const Line& line = lineListRef[i];
                // 折叠（S3f 批次 C）：hidden 行 0 高、没有文字，也就不该有背景 ——
                // 不跳的话它与折叠点下方的可见行共享同一 top，会留下一条 1px 残留条。
                if (line.hidden) {
                    continue;
                }
                const int selectableEnd = line.end + (line.hardBreakAfter ? 1 : 0);

                const int lineStart = std::clamp(startIndex, line.start, line.end);
                const int lineEnd = std::clamp(endIndex, line.start, line.end);
                const bool selectionContinuesPastLine = endIndex > line.end && startIndex <= line.end;
                if (lineStart == lineEnd && !selectionContinuesPastLine) {
                    continue;
                }

                const bool coversWholeLine = startIndex <= line.start && endIndex >= selectableEnd;
                // 起点一律走 caretXInLine：行首图元 advance 与 contentIndent 都已由
                // applyLineGlyph / applyLineIndent 平移进 metrics.caretX，全行选区因此
                // 从**行内容起点**起画（旧行为硬取 0，会把复选框 / 代码缩进一并盖住）。
                const float startX = caretXInLine(line, lineStart);
                const float endX = selectionContinuesPastLine
                    ? (line.hardBreakAfter || coversWholeLine ? viewportWidth : std::max(line.metrics.width, startX + 1.0f))
                    : caretXInLine(line, lineEnd);
                const float rawX = inset + startX - scroll;
                const float rawRight = inset + endX - scroll;
                // 可读行宽裁剪：背景左右都收在 [inset, controlWidth - rightInset] 内，
                // 横向滚动 / 超宽内容不会把选区画到行号列或右缘留白里去。
                const float clippedX = std::clamp(rawX, clipLeft, clipRight);
                const float clippedRight = std::clamp(rawRight, clipLeft, clipRight);
                const float width = std::max(1.0f, clippedRight - clippedX);
                const float rowTop = hasGeometry ? table.top(lineIndex) : 0.0f;
                const float rowHeight = hasGeometry ? table.height(lineIndex) : lineHeight;
                // 文字带（与渲染层同一个纯几何 helper）：y/height 跟着文字走，
                // 表格行的上下内边距不被吞进选区背景。
                const LineTextBand band = lineTextBand(rowTop, rowHeight, line.textShiftY);
                const float y = textTop + band.top - currentVerticalScroll;
                // 粘连补偿：只在与下一条**同样要画**的行首尾相接时补 1px（普通正文
                // 相邻行的带严格相接，行为与旧版一致）；中间隔着块间距 / 单元格内边距
                // 时不补 —— 补出来的那 1px 会变成悬在缝里的横线。
                float stitch = 0.0f;
                size_t next = i + 1;
                while (next < lineListRef.size() && lineListRef[next].hidden) {
                    ++next;
                }
                if (next < lineListRef.size()) {
                    const Line& nextLine = lineListRef[next];
                    const float nextTop = lineTextBand(hasGeometry ? table.top(static_cast<int>(next)) : 0.0f,
                                                       hasGeometry ? table.height(static_cast<int>(next)) : lineHeight,
                                                       nextLine.textShiftY).top;
                    if (nextTop - (band.top + band.height) <= 0.5f) {
                        stitch = 1.0f;
                    }
                }
                const float height = band.height + stitch;
                if (y + height < textTop || y > textTop + viewportHeight) {
                    continue;
                }
                selectionRects.push_back({clippedX, y, width, height, band.height});
            }
        }
    };

    static std::string filteredText(const std::string& input, bool multiline) {
        std::string output;
        for (char ch : input) {
            if (ch == '\r' || (!multiline && ch == '\n')) {
                continue;
            }
            output.push_back(ch);
        }
        return output;
    }

    static int clampUtf8Boundary(const std::string& value, int index) {
        int out = std::clamp(index, 0, static_cast<int>(value.size()));
        while (out > 0 && out < static_cast<int>(value.size()) &&
               (static_cast<unsigned char>(value[static_cast<std::size_t>(out)]) & 0xC0) == 0x80) {
            --out;
        }
        return out;
    }

    static std::pair<int, int> selectionRange(const InputState& state) {
        return {std::min(state.selectionStart, state.selectionEnd), std::max(state.selectionStart, state.selectionEnd)};
    }

    static bool hasTextSelection(const InputState& state) {
        return state.selectionStart != state.selectionEnd;
    }

    static void clearSelection(InputState& state) {
        state.selectionStart = state.cursor;
        state.selectionEnd = state.cursor;
        state.dragAnchor = state.cursor;
    }

    // 撤销历史的层数上限：达到就淘汰**最老**的一条（队首）。改造前后同一口径。
    static constexpr std::size_t kMaxUndoDepth = 128;

    // ── 编辑区间的行号推导（T4 A1）──────────────────────────────────────────
    // 行号 = 前面有多少个 '\n'，一律按**源行**口径（与 lp_plan.lines / 装饰表下标同源）。
    static int countNewlines(const std::string& text, int begin, int end) {
        const int from = std::clamp(begin, 0, static_cast<int>(text.size()));
        const int to = std::clamp(end, from, static_cast<int>(text.size()));
        return static_cast<int>(std::count(text.begin() + from, text.begin() + to, '\n'));
    }

    static int countNewlines(const std::string& text, int end) {
        return countNewlines(text, 0, end);
    }

    // 提交一次真实文本改动后写入编辑区间。removed / inserted 必须是**实际被替换的
    // 旧字节与新字节**（长度可能小于 EditRecord 的 span：文本被组件外改写过时
    // applyEditRecord 只替换能对上的那一段）。
    // 行号的推导只依赖"前缀 [0, byteBeg) 编辑前后逐字节相同"（两条漏斗都保证）：
    //   firstLine    数新文本前缀里的换行；
    //   newLastLine  数新文本 [byteBeg, newEnd) 末字节所在行；
    //   oldTailLine  = newLastLine + 1 - 行数差（行数差 = 新段换行 - 旧段换行）。
    static void recordPendingEdit(InputState& state, int byteBeg, int oldEnd, int newEnd,
                                  const std::string& removed, const std::string& inserted,
                                  int cursorBefore) {
        PendingTextEdit edit;
        const int size = static_cast<int>(state.text.size());
        const int beg = std::clamp(byteBeg, 0, size);
        const int after = std::clamp(newEnd, beg, size);
        edit.valid = true;
        edit.revision = state.textRevision;
        edit.byteBeg = beg;
        edit.oldEnd = std::max(beg, oldEnd);
        edit.newEnd = after;
        edit.firstLine = countNewlines(state.text, beg);
        // newLastLine = 末字节所在行 = 前缀里的换行数 + [beg, newEnd-1) 里的换行数。
        edit.newLastLine = after > beg
                               ? edit.firstLine + countNewlines(state.text, beg, after - 1)
                               : edit.firstLine;
        const int deltaLines =
            countNewlines(inserted, 0, static_cast<int>(inserted.size())) -
            countNewlines(removed, 0, static_cast<int>(removed.size()));
        edit.oldTailLine = edit.newLastLine + 1 - deltaLines;
        edit.cursorBefore = cursorBefore;
        state.pendingEdit = edit;
    }

    // 作废编辑区间：换文档 / 组件外赋值 / 捕获被丢弃时用。消费方看到 valid == false
    // 就一律全量重建（正确性优先，宁可多做一次全量）。
    static void clearPendingEdit(InputState& state) { state.pendingEdit = PendingTextEdit{}; }

    // ── 增量编辑的两步：beginEdit 捕获 → endEdit 提交 ─────────────────────────
    //
    // 约定：这一次编辑**必须全部落在 [beg, end) 之内**。区间拿不准时用整篇重载
    // beginEdit(state) —— 代价只是该命令执行期间的一份临时拷贝，endEdit 一到就还回去，
    // 绝不长期驻留（1MB × 128 份的历史问题与这份瞬时拷贝无关）。
    //
    // 可嵌套：内层 begin/end 只计数，由最外层提交。于是"选区擦除 + 插入"、
    // "翻转 + 连续两次 insertText"这类组合操作只产生**一条**记录，不会双 push。
    static void beginEdit(InputState& state, int beg, int end) {
        if (state.editDepth == 0) {
            const int size = static_cast<int>(state.text.size());
            const int clampedBeg = std::clamp(beg, 0, size);
            state.editBeg = clampedBeg;
            state.editEnd = std::clamp(std::max(beg, end), clampedBeg, size);
            state.editText = state.text.substr(static_cast<std::size_t>(state.editBeg),
                                               static_cast<std::size_t>(state.editEnd - state.editBeg));
            state.editFullSize = size;
            state.editCursorBefore = state.cursor;
            state.editSelectionStartBefore = state.selectionStart;
            state.editSelectionEndBefore = state.selectionEnd;
        }
        ++state.editDepth;
    }

    // 区间未知的整篇捕获（给"改动范围由命令内部决定"的调用方）。
    static void beginEdit(InputState& state) {
        beginEdit(state, 0, static_cast<int>(state.text.size()));
    }

    // 提交一次编辑：收缩出真实 span、入栈、推进 textRevision。没开捕获时是安全的空操作。
    static void endEdit(InputState& state) {
        if (state.editDepth <= 0) {
            state.editDepth = 0;
            return;
        }
        if (--state.editDepth > 0) {
            return;  // 还嵌在外层编辑里：由最外层统一提交
        }

        const int size = static_cast<int>(state.text.size());
        const int beg = std::min(state.editBeg, size);
        // span 在编辑后的长度 = 编辑前长度 + 整篇长度差（"改动都在 span 内"的约定下成立）。
        const int spanAfter = std::max(0, (state.editEnd - state.editBeg) + (size - state.editFullSize));
        const std::string after = state.text.substr(
            static_cast<std::size_t>(beg),
            static_cast<std::size_t>(std::min(spanAfter, std::max(0, size - beg))));
        // 公共前后缀收缩：把捕获区间收成"真正变过的那一小段"。
        // 这也是 applyInlineFormat 那类**两点修改**的处理方式 —— 两点之间整段作为
        // 一个 span 的 before/after，收缩后记录的仍是语义等价的最小变换。
        const std::size_t limit = std::min(state.editText.size(), after.size());
        std::size_t prefix = 0;
        while (prefix < limit && state.editText[prefix] == after[prefix]) {
            ++prefix;
        }
        std::size_t suffix = 0;
        const std::size_t suffixLimit = limit - prefix;
        while (suffix < suffixLimit &&
               state.editText[state.editText.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
            ++suffix;
        }

        EditRecord record;
        record.beg = beg + static_cast<int>(prefix);
        record.removed = state.editText.substr(prefix, state.editText.size() - prefix - suffix);
        record.inserted = after.substr(prefix, after.size() - prefix - suffix);
        record.cursorBefore = state.editCursorBefore;
        record.selectionStartBefore = state.editSelectionStartBefore;
        record.selectionEndBefore = state.editSelectionEndBefore;
        record.cursorAfter = state.cursor;
        record.selectionStartAfter = state.selectionStart;
        record.selectionEndAfter = state.selectionEnd;
        // 捕获只为提交而存在：提交完立刻释放，不占历史内存（历史字节见 undoHistoryBytes）。
        std::string().swap(state.editText);

        // same / no-op：文本一个字节都没变 → 不入栈、不推进 textRevision、不清 redo。
        if (record.removed.empty() && record.inserted.empty()) {
            return;
        }
        ++state.textRevision;      // 每次实际文本改变**恰好** bump 一次
        // T4 A1：把这次改动的区间记下来（必须在 push 移走 record 之前读它的字段）。
        recordPendingEdit(state, record.beg,
                          record.beg + static_cast<int>(record.removed.size()),
                          record.beg + static_cast<int>(record.inserted.size()),
                          record.removed, record.inserted, state.editCursorBefore);
        state.undoStack.push_back(std::move(record));
        if (state.undoStack.size() > kMaxUndoDepth) {
            state.undoStack.erase(state.undoStack.begin());  // 淘汰最老的一条
        }
        state.redoStack.clear();   // 新的真实编辑让重做链失效
    }

    // 只丢捕获、**不做文本对照也不补 bump**：换文档（loadDocument）用 —— 整篇文本马上
    // 被替换掉，拿旧捕获去比没有意义；补出来的那次 bump 还会破坏"loadDocument 自己恰好
    // bump 一次"的旧口径（tests/unit/undo_incremental ④ 与旧 resetEditorInputState
    // 逐字段对照）。abortEdit 也复用它收尾，两处的"深度归零 + 释放捕获"因此逐字段一致。
    static void discardCapture(InputState& state) {
        state.editDepth = 0;
        std::string().swap(state.editText);
        state.editBeg = 0;
        state.editEnd = 0;
        state.editFullSize = 0;
    }

    // 捕获期间文本是否真的变了（abortEdit 的对照）。**不能只比长度**，两层判据：
    //   ① 整篇长度差 = O(1)，先接住"增删"（哪怕只多/少一个字节，落在哪个区间都算）；
    //   ② 长度相同 → 捕获窗口 [editBeg, editEnd) 逐字节比 —— 同长度替换（abc→abd）
    //      全靠这一步才认得出来，只比 size 的实现会把这类改动整个漏掉。
    // 长度没变时窗口跨度与捕获时严格相等（beginEdit 存的就是这一段），于是② 就是完整的
    // 前后对照；万一场外把字段拧到对不上的长度，compare 返回非 0 → 按"变了"处理
    // （保守方向：宁可多 bump 一次 revision，也绝不漏）。
    //
    // 精确触发边界：区间捕获 beginEdit(state, beg, end) 下，**落在窗口之外的同长度改写**
    // 认不出来 —— 那本来就违反"这次编辑必须全部落在 [beg, end) 内"的约定（改动要么动了
    // 长度被 ① 抓住，要么落在窗口里被 ② 抓住）；整篇捕获 beginEdit(state) 的窗口就是
    // 全文，没有任何盲区。
    // 代价：O(窗口长度)，与 endEdit 收缩同量级，且只在 abort 这条**兜底**路径上付 ——
    // 每次按键的热路径（beginEdit 的捕获）一个字节都不多比。
    static bool captureTextChanged(const InputState& state) {
        const int size = static_cast<int>(state.text.size());
        if (size != state.editFullSize) {
            return true;
        }
        const int beg = std::min(state.editBeg, size);
        const int end = std::min(std::max(state.editEnd, beg), size);
        return state.text.compare(static_cast<std::size_t>(beg),
                                  static_cast<std::size_t>(end - beg),
                                  state.editText) != 0;
    }

    // 放弃一次还没提交的捕获：不入撤销栈、不动 redo。命令中途判失败 / 捕获失衡时用
    // （换文档走上面的 discardCapture），也是"调用方漏了 endEdit"的兜底 —— 深度必须
    // 回到 0，否则后续每一次编辑都会并进这条陈旧捕获，整条撤销链就废了（漏一条记录
    // 总比毁掉全部历史强）。
    //
    // ── 漏 endEdit 的补账（T3 复审）──────────────────────────────────────────
    // beginEdit 之后文本被改、调用方却没走到 endEdit 时，textRevision 不会被推进，而
    // applyEditorCommand 是否回写 doc.text、排版缓存（cachedTextRevision）、装饰增量链、
    // 框架 dirtyKey **全都只认 revision** —— 于是 inputState.text 与 appState.doc.text
    // 分叉（下一次 build 按 doc 的旧值把组件文本连光标一起冲掉），布局继续按旧文本排。
    // 这里在丢捕获前做一次真实文本对照（captureTextChanged），一旦不同就**补 bump 一次
    // revision**：不入栈、不清 redo（这次改动本就不该留记录，也**不回滚文本**——abort
    // 丢的是"捕获"，不是命令已经改出来的字节），bump 本身就是排版缓存的失效信号（与
    // applyEditRecord 同一机制："bump 即整体失效"），pendingEdit 同时作废 → 装饰退回
    // 全量重建。endEdit 那条"每次实际改动恰好 bump 一次"说的是**提交**；这里补的是
    // 从没被提交过的那一次，深度已归零，绝不会被后续 endEdit 再 bump 第二次。
    //
    // 返回 true = 放弃时捕获区间内的文本确实与捕获时不同。调用方（applyEditorCommand）
    // 据此同步自己的副本，不必再自己扫一遍全文。
    static bool abortEdit(InputState& state) {
        bool changed = false;
        if (state.editDepth > 0) {
            changed = captureTextChanged(state);
            // 有捕获被丢弃：这次"没收尾的改动"没有可信的前后对照 —— 编辑区间一并作废，
            // 否则消费方会拿上一条记录去描述一个已经被改过的文本（T4 A1）。
            clearPendingEdit(state);
            if (changed) {
                ++state.textRevision;  // 漏记的改动补账：只补 revision，不产生撤销记录
            }
        }
        discardCapture(state);
        return changed;
    }

    // 正逆共用的落盘。forward = true 是 redo（removed→inserted），false 是 undo。
    static void applyEditRecord(InputState& state, const EditRecord& record, bool forward) {
        const std::string& from = forward ? record.removed : record.inserted;
        const std::string& to = forward ? record.inserted : record.removed;
        const int size = static_cast<int>(state.text.size());
        // 文本可能已被组件外的直接赋值改写过：只替换能对上的长度，绝不越界；
        // 光标/选区一律按改写后的文本重新夹到 UTF-8 边界（restoreSnapshot 的老口径）。
        const int beg = std::clamp(record.beg, 0, size);
        const int end = std::min(size, beg + static_cast<int>(from.size()));
        // 实际被替换掉的旧字节（长度可能小于 from，见上）——T4 的编辑区间要用它，
        // 必须在 replace 之前抓出来。
        const std::string removedNow = state.text.substr(static_cast<std::size_t>(beg),
                                                         static_cast<std::size_t>(end - beg));
        state.text.replace(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg), to);
        state.cursor = clampUtf8Boundary(state.text, forward ? record.cursorAfter : record.cursorBefore);
        state.selectionStart =
            clampUtf8Boundary(state.text, forward ? record.selectionStartAfter : record.selectionStartBefore);
        state.selectionEnd =
            clampUtf8Boundary(state.text, forward ? record.selectionEndAfter : record.selectionEndBefore);
        state.dragAnchor = state.cursor;
        state.hasPreferredCursorX = false;
        // 排版缓存按 cachedTextRevision 与 textRevision 比对，bump 即整体失效
        // （layoutCacheValid 仍为 true，走段落级增量重排，不退回全量）。
        // composition/preedit 不在文档撤销范围内：组合中途按 Ctrl+Z 由 input.h 的
        // undo 分支先清组合，再走到这里。
        ++state.textRevision;
        // T4 A1：撤销/重做同样是"实际文本改变"，编辑区间照记 —— 正逆都用这一条。
        // cursorBefore 取记录里的值：正着应用（redo）之前的文本就是记录的"编辑前文本"，
        // 所以它在两个方向上都指的是"应用前光标"（旧文本坐标）。
        recordPendingEdit(state, beg, end, beg + static_cast<int>(to.size()),
                          removedNow, to, record.cursorBefore);
    }

    // 撤销/重做**自己不产生新的撤销记录**：只搬栈、只应用记录。
    static bool undoEdit(InputState& state) {
        if (state.undoStack.empty()) {
            return false;
        }
        EditRecord record = std::move(state.undoStack.back());
        state.undoStack.pop_back();
        applyEditRecord(state, record, false);
        // 同一条记录进重做栈：redo 会把它正着放回来（正逆共用一条记录）。
        state.redoStack.push_back(std::move(record));
        return true;
    }

    static bool redoEdit(InputState& state) {
        if (state.redoStack.empty()) {
            return false;
        }
        EditRecord record = std::move(state.redoStack.back());
        state.redoStack.pop_back();
        applyEditRecord(state, record, true);
        state.undoStack.push_back(std::move(record));
        return true;
    }

    // 只读统计：撤销/重做历史实际持有的文本字节数（removed + inserted 之和）。
    // 给测试用 —— 1MB 文档 × 200 次单字符编辑后必须远小于 2× 文档大小。
    static std::size_t undoHistoryBytes(const InputState& state) {
        std::size_t total = 0;
        for (const EditRecord& record : state.undoStack) {
            total += record.removed.size() + record.inserted.size();
        }
        for (const EditRecord& record : state.redoStack) {
            total += record.removed.size() + record.inserted.size();
        }
        return total;
    }

    // 删除当前选区。记录由这里自己产生：调用方不再"预告"一次，也就不会有错位的旧快照。
    static void eraseSelection(InputState& state) {
        const auto range = selectionRange(state);
        if (range.first == range.second) {
            return;
        }
        beginEdit(state, range.first, range.second);
        state.text.erase(static_cast<std::size_t>(range.first),
                         static_cast<std::size_t>(range.second - range.first));
        state.cursor = range.first;
        clearSelection(state);
        endEdit(state);
    }

    // 删除 [beg, end)（Delete / Backspace 共用）：光标落在保留下来的那一侧。
    static void eraseRange(InputState& state, int beg, int end) {
        const int size = static_cast<int>(state.text.size());
        const int from = std::clamp(beg, 0, size);
        const int to = std::clamp(end, from, size);
        if (to <= from) {
            return;
        }
        beginEdit(state, from, to);
        state.text.erase(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
        state.cursor = from;
        clearSelection(state);
        endEdit(state);
    }

    // 在光标处插入；有选区时"替换选区"整体算**一条**记录（组合操作不拆成两条）。
    static void insertAtCursor(InputState& state, const std::string& value) {
        if (value.empty()) {
            return;
        }
        const int size = static_cast<int>(state.text.size());
        const bool replacing = hasTextSelection(state);
        const auto range = selectionRange(state);
        const int insertAt = replacing ? range.first : std::clamp(state.cursor, 0, size);
        const int beg = replacing ? range.first : insertAt;
        const int end = replacing ? range.second : insertAt;
        beginEdit(state, beg, end);
        if (replacing) {
            state.text.erase(static_cast<std::size_t>(range.first),
                             static_cast<std::size_t>(range.second - range.first));
        }
        state.cursor = replacing ? range.first : insertAt;
        state.text.insert(static_cast<std::size_t>(state.cursor), value);
        state.cursor += static_cast<int>(value.size());
        state.hasPreferredCursorX = false;
        clearSelection(state);
        endEdit(state);
    }

    // ── 装载文档（T11）───────────────────────────────────────────────────────
    // 与 neo::loadDocument（apps 层从磁盘打开文件的 API）**同名、不同命名空间**：
    // 这里只是"把一串字节装进组件状态"，不碰文件系统，也不认识 lp_plan / 装饰计划缓存
    // ——那些仍是应用层的事。逐字段复现改造前 app_state.h::resetEditorInputState。
    // 刻意**不清** pointerHover* / caretBlink* / cachedTables：悬停行与光标闪烁的
    // 活动签名本来就跨文档存活（与旧行为一致）。
    static void loadDocument(InputState& state, const std::string& value) {
        state.text = value;
        ++state.textRevision;
        state.cursor = 0;
        state.selectionStart = 0;
        state.selectionEnd = 0;
        state.dragAnchor = 0;
        state.selecting = false;
        state.hasPreferredCursorX = false;
        state.followCaret = true;
        state.preferredCursorX = 0.0f;
        state.horizontalScroll = 0.0f;
        state.verticalScroll = 0.0f;
        state.compositionText.clear();
        state.preedit.reset();
        state.undoStack.clear();
        state.redoStack.clear();
        // 新撤销结构的收尾：丢掉任何没提交的捕获，免得下一次 endEdit 把上一篇的
        // 区间算到新文档上。**刻意不走 abortEdit**：整篇文本刚被替换，拿旧捕获去比没有
        // 意义，它补出来的那次 bump 还会破坏"loadDocument 自己恰好 bump 一次"的旧口径。
        discardCapture(state);
        // T4 A1：换文档没有可信的前后对照 —— 编辑区间置空，装饰/布局一律全量重建。
        clearPendingEdit(state);
        state.layoutCacheValid = false;
        state.cachedTextRevision = static_cast<unsigned long long>(-1);
        state.cachedLayoutText.clear();
        // 换文档后装饰也要重来一遍：版本号推进会让图元重新取文本（见 input.h 的 textDirtyKey）。
        state.decorations.clear();
        state.decorationSnapshot.reset();
        ++state.decorationRevision;
    }

    static void moveCursor(InputState& state,
                           int direction,
                           bool keepSelection,
                           const std::string& fontFamily,
                           float fontSize,
                           bool multiline,
                           float viewportWidth) {
        const int previous = state.cursor;
        if (!keepSelection && hasTextSelection(state)) {
            const auto range = selectionRange(state);
            state.cursor = direction < 0 ? range.first : range.second;
            state.hasPreferredCursorX = false;
            clearSelection(state);
            return;
        }
        state.cursor = direction < 0
            ? prevCursorIndex(state, fontFamily, fontSize, multiline, viewportWidth)
            : nextCursorIndex(state, fontFamily, fontSize, multiline, viewportWidth);
        state.hasPreferredCursorX = false;
        if (keepSelection) {
            if (!hasTextSelection(state)) {
                state.selectionStart = previous;
            }
            state.selectionEnd = state.cursor;
        } else {
            clearSelection(state);
        }
    }

    static void moveCursorToLineEdge(InputState& state,
                                     bool toEnd,
                                     bool keepSelection,
                                     const std::string& fontFamily,
                                     float fontSize,
                                     float viewportWidth) {
        const std::vector<InputLayout::Line>& lines = cachedLines(state, fontFamily, fontSize, viewportWidth, true);
        if (lines.empty()) {
            moveCursorTo(state, toEnd ? static_cast<int>(state.text.size()) : 0, keepSelection);
            return;
        }

        const int lineIndex = lineIndexFor(lines, state.cursor);
        const InputLayout::Line& line = lines[static_cast<size_t>(lineIndex)];
        moveCursorTo(state, toEnd ? line.end : line.start, keepSelection);
    }

    static void moveCursorVertical(InputState& state,
                                   int direction,
                                   bool keepSelection,
                                   const std::string& fontFamily,
                                   float fontSize,
                                   float viewportWidth,
                                   float viewportHeight) {
        const std::vector<InputLayout::Line>& lines = cachedLines(state, fontFamily, fontSize, viewportWidth, true);
        if (lines.empty()) {
            return;
        }

        // 垂直移动只比较 x：行可能各有各的字号/行高，但"保持列位置"这件事是在同一行的坐标系里做的。
        auto xFor = [&](int byteIndex) {
            const int lineIndex = lineIndexFor(lines, byteIndex);
            const InputLayout::Line& line = lines[static_cast<size_t>(lineIndex)];
            return caretXInLine(line, byteIndex);
        };

        auto closestOnLine = [&](int lineIndex, float targetX) {
            ensureLineDetails(state, lineIndex);
            const InputLayout::Line& line = lines[static_cast<size_t>(std::clamp(lineIndex, 0, static_cast<int>(lines.size()) - 1))];
            const int tableHit = tableDocOffsetForX(
                line, targetX, findTableColumns(state.cachedTables, state.cachedTableIndex, line.tableId));
            if (tableHit >= 0) return tableHit;
            return docOffsetForX(line.metrics, line.holes, line.start, targetX);
        };

        const int previous = state.cursor;
        const int currentLine = lineIndexFor(lines, state.cursor);
        const int nextLine = std::clamp(currentLine + direction, 0, static_cast<int>(lines.size()) - 1);
        if (nextLine == currentLine) {
            // 已在第一行（↑）/最后一行（↓）：跟随记事本、Obsidian 等的边界惯例，
            // 折到当前视觉行的行首/行尾，而不是无响应；光标已在该边缘则保持原样
            // （连同既有选区一起不动）。行语义与上方正常移动一致（折行后的视觉行）；
            // Shift 选区沿用本函数的 keepSelection 语义——选区随光标扩到行边缘，
            // 与 Home/End 行为对齐，这是刻意选择：只给原本无响应的边界加跳转，
            // 不改变其他场景的选区行为。
            const InputLayout::Line& line = lines[static_cast<size_t>(currentLine)];
            const int edge = direction < 0 ? line.start : line.end;
            if (state.cursor == edge) {
                return;
            }
            moveCursorToLineEdge(state, direction > 0, keepSelection, fontFamily, fontSize, viewportWidth);
            return;
        }

        if (!state.hasPreferredCursorX) {
            state.preferredCursorX = xFor(state.cursor);
            state.hasPreferredCursorX = true;
        }
        int targetLine = nextLine;
        int targetByte = closestOnLine(targetLine, state.preferredCursorX);
        // A short/empty cell has no caret stop on later wrapped segments. If the
        // nearest position is therefore its existing tail, keep scanning through
        // sibling segments so repeated Down/Up can reach the next real source row.
        while (targetByte == state.cursor && targetLine + direction >= 0 &&
               targetLine + direction < static_cast<int>(lines.size())) {
            const int adjacent = targetLine + direction;
            const InputLayout::Line& from = lines[static_cast<std::size_t>(targetLine)];
            const InputLayout::Line& to = lines[static_cast<std::size_t>(adjacent)];
            const bool sameWrappedRow = from.tableId >= 0 && from.tableId == to.tableId &&
                                        from.start == to.start && from.end == to.end;
            const InputLayout::Line& original = lines[static_cast<std::size_t>(currentLine)];
            const bool leavingStalledWrappedRow =
                from.tableId >= 0 && from.tableId == original.tableId &&
                from.start == original.start && from.end == original.end && !sameWrappedRow;
            if (!sameWrappedRow && !leavingStalledWrappedRow) break;
            targetLine = adjacent;
            targetByte = closestOnLine(targetLine, state.preferredCursorX);
        }
        state.cursor = clampUtf8Boundary(state.text, targetByte);
        syncVerticalScroll(state, lines, targetLine, viewportHeight);

        if (keepSelection) {
            if (!hasTextSelection(state)) {
                state.selectionStart = previous;
            }
            state.selectionEnd = state.cursor;
        } else {
            clearSelection(state);
        }
    }

    static void moveCursorPage(InputState& state, int direction, bool keepSelection,
                               const std::string& fontFamily, float fontSize,
                               float viewportWidth, float viewportHeight) {
        const auto& lines = cachedLines(state, fontFamily, fontSize, viewportWidth, true);
        if (lines.empty() || direction == 0) return;
        const int current = lineIndexFor(lines, state.cursor);
        if (!state.hasPreferredCursorX) {
            state.preferredCursorX = caretXInLine(lines[current], state.cursor);
            state.hasPreferredCursorX = true;
        }
        const float preferred = state.preferredCursorX;
        const float distance = std::max(std::max(1.0f, lines[current].lineHeight),
                                       viewportHeight - std::max(1.0f, lines[current].lineHeight));
        int target = state.cachedGeometry.lineAtY(lines[current].top + direction * distance);
        const auto visible = [&](int index) { return !lines[index].hidden && state.cachedGeometry.height(index) > 0.0f; };
        while (target >= 0 && target < static_cast<int>(lines.size()) && !visible(target)) target += direction;
        if (target < 0 || target >= static_cast<int>(lines.size())) {
            target = direction < 0 ? 0 : static_cast<int>(lines.size()) - 1;
            while (!visible(target) && target != current) target -= direction;
        }
        ensureLineDetails(state, target);
        const int tableHit = tableDocOffsetForX(
            lines[target], preferred,
            findTableColumns(state.cachedTables, state.cachedTableIndex, lines[target].tableId));
        moveCursorTo(state, tableHit >= 0 ? tableHit :
                     docOffsetForX(lines[target].metrics, lines[target].holes,
                                   lines[target].start, preferred), keepSelection);
        state.preferredCursorX = preferred;
        state.hasPreferredCursorX = true;
        syncVerticalScroll(state, lines, target, viewportHeight);
    }

    static void moveCursorTo(InputState& state, int position, bool keepSelection) {
        const int previous = state.cursor;
        state.cursor = clampUtf8Boundary(state.text, position);
        state.hasPreferredCursorX = false;
        if (keepSelection) {
            if (!hasTextSelection(state)) {
                state.selectionStart = previous;
            }
            state.selectionEnd = state.cursor;
        } else {
            clearSelection(state);
        }
    }

    static void copySelection(const InputState& state) {
        if (!hasTextSelection(state)) {
            return;
        }
        const auto range = selectionRange(state);
        const std::string selected = state.text.substr(static_cast<std::size_t>(range.first), static_cast<std::size_t>(range.second - range.first));
        core::window::setClipboardText(selected);
    }

    static core::TextPrimitive::TextMetrics measureMetrics(const std::string& value, const std::string& fontFamily, float fontSize) {
        return core::TextPrimitive::measureTextMetrics(value, fontFamily, fontSize, 400);
    }

    static core::TextPrimitive::TextMetrics measureMetrics(const std::string& value,
                                                           const std::string& fontFamily,
                                                           float fontSize,
                                                           int fontWeight) {
        return core::TextPrimitive::measureTextMetrics(value, fontFamily, fontSize,
                                                       fontWeight > 0 ? fontWeight : 400);
    }

    // ── 行内样式段 ──────────────────────────────────────────────────────────
    // 投影文本按样式段切开后的连续块。`style == nullptr` 表示"没被任何段覆盖的普通文字"。
    struct VisiblePiece {
        int visibleBeg = 0;  // 相对本行投影文本
        int visibleEnd = 0;
        const LineRunStyle* style = nullptr;
    };

    // 一段排版结果：caret 表的偏移**相对 visBeg**，runs 的 x 相对 visBeg、beg/end 是文档绝对偏移。
    struct StyledLineLayout {
        core::TextPrimitive::TextMetrics metrics;
        std::vector<TextRun> runs;
    };

    // caret 表去重：同一偏移只留第一个（各段的接缝处两边都会报同一个偏移）。
    static void addCaretStop(core::TextPrimitive::TextMetrics& metrics, int byteIndex, float x) {
        if (!metrics.byteIndices.empty() && metrics.byteIndices.back() == byteIndex) {
            return;
        }
        metrics.byteIndices.push_back(byteIndex);
        metrics.caretX.push_back(x);
    }

    // 表格专用：同一个投影偏移上，**后写的覆盖先写的**。
    // 格与格的分界处，上一格的末尾与下一格的开头落在同一个投影偏移上（管道被藏掉了），
    // 用 addCaretStop 会保留上一列的 x —— 后果是第 2 格起的光标 x 全被上一列尾部污染，
    // 点击/退格的字节定位整体偏一格（实机实测：点第 2 格中部，退格把中间那根管道吃了）。
    // 分界处归**下一格**：光标越过管道后停在下一格开头，与 Obsidian 一致。
    static void overrideCaretStop(core::TextPrimitive::TextMetrics& metrics, int byteIndex, float x) {
        if (!metrics.byteIndices.empty() && metrics.byteIndices.back() == byteIndex) {
            metrics.caretX.back() = x;
            return;
        }
        metrics.byteIndices.push_back(byteIndex);
        metrics.caretX.push_back(x);
    }

    // 把一行的投影文本按样式段切成连续片段（覆盖整行，无空洞）。
    // 段是稀疏的：没被任何段覆盖的部分自动补成"默认样式"的片段。
    static std::vector<VisiblePiece> buildVisiblePieces(const std::vector<LineHole>& holes,
                                                        int lineBeg,
                                                        int lineEnd,
                                                        const std::vector<LineRun>& runs) {
        std::vector<VisiblePiece> pieces;
        const int visibleLen = visibleLength(holes, lineBeg, lineEnd);
        if (visibleLen <= 0) {
            return pieces;
        }
        // 先按起点排序（上层理论上是升序的，但不能指望；重叠加后者胜出）。
        std::vector<const LineRun*> ordered;
        ordered.reserve(runs.size());
        for (const LineRun& run : runs) {
            if (!run.empty()) {
                ordered.push_back(&run);
            }
        }
        std::sort(ordered.begin(), ordered.end(), [](const LineRun* lhs, const LineRun* rhs) {
            return lhs->beg != rhs->beg ? lhs->beg < rhs->beg : lhs->end < rhs->end;
        });

        int cursor = 0;
        for (const LineRun* run : ordered) {
            const int clampedDocBeg = std::clamp(run->beg, lineBeg, lineEnd);
            const int clampedDocEnd = std::clamp(run->end, lineBeg, lineEnd);
            const int runBeg = visibleLength(holes, lineBeg, clampedDocBeg);
            const int runEnd = visibleLength(holes, lineBeg, clampedDocEnd);
            if (runEnd <= runBeg) {
                continue;  // 整段落在隐藏区间里
            }
            const int pieceBeg = std::max(runBeg, cursor);
            if (pieceBeg >= runEnd) {
                continue;  // 被前一段完全覆盖
            }
            if (pieceBeg > cursor) {
                pieces.push_back({cursor, pieceBeg, nullptr});
            }
            pieces.push_back({pieceBeg, runEnd, &run->style});
            cursor = runEnd;
        }
        if (cursor < visibleLen) {
            pieces.push_back({cursor, visibleLen, nullptr});
        }
        return pieces;
    }

    // 把 [visBeg, visEnd) 这段投影文本按片段逐段测量，再拼成一张 caret 表。
    // 拼接成立的前提：字形推进量逐字累加（没有字距调整），于是"整行的 caret x"
    // 就是各段 x 加上它前面所有段的宽度之和。
    static StyledLineLayout layoutStyledRange(const std::string& value,
                                              int lineBeg,
                                              int lineEnd,
                                              const std::vector<LineHole>& holes,
                                              const std::vector<VisiblePiece>& pieces,
                                              int visBeg,
                                              int visEnd,
                                              float fontSize,
                                              const std::string& defaultFontFamily) {
        StyledLineLayout layout;
        float x = 0.0f;
        for (const VisiblePiece& piece : pieces) {
            const int from = std::max(piece.visibleBeg, visBeg);
            const int to = std::min(piece.visibleEnd, visEnd);
            if (to <= from) {
                continue;
            }
            const int docBeg = std::max(lineBeg, unprojectVisible(holes, lineBeg, from));
            const int docEnd = std::max(docBeg, unprojectVisible(holes, lineBeg, to));
            const std::string text = holes.empty()
                ? value.substr(static_cast<std::size_t>(docBeg), static_cast<std::size_t>(docEnd - docBeg))
                : projectText(value, docBeg, docEnd, holes);
            const LineRunStyle style = piece.style != nullptr ? *piece.style : LineRunStyle{};
            const std::string family = style.fontFamily.empty() ? defaultFontFamily : style.fontFamily;
            const core::TextPrimitive::TextMetrics pieceMetrics = measureMetrics(text, family, fontSize, style.weight);
            const std::size_t stops = std::min(pieceMetrics.byteIndices.size(), pieceMetrics.caretX.size());
            for (std::size_t i = 0; i < stops; ++i) {
                addCaretStop(layout.metrics, from - visBeg + pieceMetrics.byteIndices[i],
                             x + pieceMetrics.caretX[i]);
            }
            TextRun run;
            run.beg = docBeg;
            run.end = docEnd;
            run.x = x;
            run.width = pieceMetrics.width;
            run.style = style;
            layout.runs.push_back(std::move(run));
            x += pieceMetrics.width;
        }
        layout.metrics.width = x;
        (void) lineEnd;
        return layout;
    }

    // 记下"这一次排版用的装饰"。内容变了就推进版本号——渲染侧的 dirtyKey 靠它决定要不要
    // 重新取行文本（框架的 dirtyKey 一旦与上次相同，就完全不更新图元的文本）。
    static LineDecorationView activeDecorations(const InputState& state) {
        return state.decorationSnapshot ? LineDecorationView(*state.decorationSnapshot) : LineDecorationView(state.decorations);
    }

    static void setDecorations(InputState& state, LineDecorationView next,
                               const LineDecorationSnapshot& snapshot = {}) {
        if (snapshot) {
            if (state.decorationSnapshot != snapshot) {
                state.decorationSnapshot = snapshot;
                state.decorations.clear();
                ++state.decorationRevision;
            }
            return;
        }
        if (activeDecorations(state) == next) {
            return;
        }
        state.decorations.assign(next.begin(), next.end());
        state.decorationSnapshot.reset();
        ++state.decorationRevision;
    }

    // ── 光标闪烁 ────────────────────────────────────────────────────────────
    // 两件事分开做，因为它俩的驱动时机不同：
    //   caretBlinkObserve()：每次排版时看一眼"有没有活动"。有就立刻可见，并记下"这一拍别翻"。
    //   caretBlinkTick()   ：由 onTimer(半周期) 驱动，安静了半周期才翻一次。
    // **刻意不用逐帧回调**：闪烁不需要 60fps，而逐帧等于让应用满速渲染去等时间
    // （实测那样会让编辑器的空闲开销从 0.3% 涨到 14% 单核）。
    static constexpr float kCaretBlinkHalfSeconds = 0.53f;

    static bool caretBlinkObserve(InputState& state) {
        const bool activityChanged = !state.caretBlinkInitialized ||
            state.caretBlinkCursor != state.cursor ||
            state.caretBlinkSelectionStart != state.selectionStart ||
            state.caretBlinkSelectionEnd != state.selectionEnd ||
            state.caretBlinkTextRevision != state.textRevision ||
            state.caretBlinkCompositionRevision != state.compositionRevision;
        if (!activityChanged) {
            return false;
        }
        state.caretBlinkInitialized = true;
        state.caretBlinkCursor = state.cursor;
        state.caretBlinkSelectionStart = state.selectionStart;
        state.caretBlinkSelectionEnd = state.selectionEnd;
        state.caretBlinkTextRevision = state.textRevision;
        state.caretBlinkCompositionRevision = state.compositionRevision;
        state.caretBlinkVisible = true;
        state.caretBlinkPendingReset = true;
        return true;
    }

    static void caretBlinkTick(InputState& state) {
        if (state.caretBlinkPendingReset) {
            state.caretBlinkPendingReset = false;
            state.caretBlinkVisible = true;  // 刚活动过：这一拍保持常亮，不翻
            return;
        }
        state.caretBlinkVisible = !state.caretBlinkVisible;
    }

    // 本帧要不要画光标。
    static bool caretVisible(bool blinkEnabled, bool focused, const InputState& state) {
        return !blinkEnabled || !focused || state.caretBlinkVisible;
    }

    // ── 排版增量的诊断计数（T4 §9）──────────────────────────────────────────
    // full = 整份 measureLines；incremental = 增量更新成功；fallback = 尝试增量
    // 但判据不过、退到全量（表格、行边界对不上、装饰表尺寸不符都在这里）。
    // 只给测试 / 诊断读，不属于组件的公开 builder API。
    struct LayoutDebugStats {
        unsigned long long full = 0;
        unsigned long long incremental = 0;
        unsigned long long fallback = 0;
        unsigned long long tableRebuilt = 0;
        unsigned long long tableReused = 0;
        unsigned long long tableIntrinsicBuilt = 0;
        unsigned long long tableIntrinsicReused = 0;
        unsigned long long tableWholeCellReused = 0;
        unsigned long long cursorPatched = 0;
        unsigned long long cursorMeasuredRows = 0;
        unsigned long long geometryRebuilt = 0;
        unsigned long long editPatched = 0;
        unsigned long long editMeasuredRows = 0;
        unsigned long long detailMeasuredRows = 0;
        unsigned long long coarseRows = 0;
    };

    static LayoutDebugStats& debugLayoutStats() {
        static LayoutDebugStats stats;
        return stats;
    }

    // 最近一次"布局增量被判据拒掉"的原因（诊断用，不进公开 API；测试据此定位回退点）。
    static std::string& layoutRejectReason() {
        static std::string reason;
        return reason;
    }

    // ── 行级元数据的绝对偏移平移（T4 C）────────────────────────────────────
    // TextLine 里所有"指向文档字节"的字段一次性搬过去：散落加法必漏字段 ——
    // 改造前的 updateChangedParagraphs 就漏了 runs（样式段的 beg/end 是文档绝对偏移，
    // 平移后仍指旧位置，下一次渲染会按错的字节切文本）。
    // 明确**不动**的字段：
    //   · metrics.byteIndices / caretX：行内投影坐标，随该行自己的切片走；
    //   · top：由 rebuildGeometry 统一重算；
    //   · box / glyph / gutterGlyph / image* / fontFamily / contentIndent /
    //     textShiftY / color / fontSize / lineHeight / hidden*：与字节偏移无关。
    static void translateLineMetadata(TextLine& line, int deltaBytes, int deltaLines) {
        if (deltaBytes == 0 && deltaLines == 0) {
            return;
        }
        line.start += deltaBytes;
        line.end += deltaBytes;
        line.lineNumber += deltaLines;
        if (line.tableId >= 0) line.tableId += deltaBytes;
        for (LineHole& hole : line.holes) {
            hole.beg += deltaBytes;
            hole.end += deltaBytes;
        }
        for (TextRun& run : line.runs) {
            run.beg += deltaBytes;
            run.end += deltaBytes;
        }
        // 表格命中 clamp 的格区间同样是文档绝对偏移，随行一起平移（2026-10-06）。
        for (LineCell& cell : line.tableCellRanges) {
            cell.beg += deltaBytes;
            cell.end += deltaBytes;
        }
        for (TableDocCaretStop& stop : line.tableDocCaretStops) {
            stop.byteIndex += deltaBytes;
        }
    }

    // 装饰等价判定（T4）：a 是旧装饰、b 是新装饰，b 的绝对偏移 = a + delta。
    // 逐字段比较、偏移字段按 delta 折算 —— 不拷贝不分配，专供"整行复用"的判据。
    // delta == 0 时与 operator== 等价（样式段的 x 等横向坐标本来就与偏移无关）。
    static bool decorationsEquivalent(const LineDecoration& a, const LineDecoration& b, int delta) {
        if (std::fabs(a.fontSize - b.fontSize) >= 0.001f ||
            std::fabs(a.lineHeight - b.lineHeight) >= 0.001f ||
            !colorEquals(a.textColor, b.textColor) || a.fontFamily != b.fontFamily ||
            !(a.box == b.box) || !(a.glyph == b.glyph) || !(a.gutterGlyph == b.gutterGlyph) ||
            std::fabs(a.contentIndent - b.contentIndent) >= 0.001f ||
            std::fabs(a.textShiftY - b.textShiftY) >= 0.001f ||
            std::fabs(a.spaceBefore - b.spaceBefore) >= 0.001f ||
            a.imagePath != b.imagePath ||
            std::fabs(a.imageWidth - b.imageWidth) >= 0.001f ||
            std::fabs(a.imageHeight - b.imageHeight) >= 0.001f ||
            a.imageFailed != b.imageFailed || a.imageFailText != b.imageFailText ||
            a.languageLabel != b.languageLabel ||
            a.hidden != b.hidden || a.hiddenByFold != b.hiddenByFold ||
            ((a.tableId < 0 || b.tableId < 0) ? a.tableId != b.tableId
                                             : a.tableId + delta != b.tableId) ||
            std::fabs(a.effectiveCellPadding() - b.effectiveCellPadding()) >= 0.001f ||
            a.tableHeaderRow != b.tableHeaderRow || a.tableSeparator != b.tableSeparator) {
            return false;
        }
        // listIndentBeg/End 是文档字节区间：按 delta 折算（-1 = 无度量源，两侧必须
        // 同为 -1，不能拿 -1 + delta 去比）。
        if (a.listIndentBeg < 0 || b.listIndentBeg < 0) {
            if (a.listIndentBeg != b.listIndentBeg || a.listIndentEnd != b.listIndentEnd) {
                return false;
            }
        } else if (a.listIndentBeg + delta != b.listIndentBeg ||
                   a.listIndentEnd + delta != b.listIndentEnd) {
            return false;
        }
        if (a.holes.size() != b.holes.size() || a.runs.size() != b.runs.size() ||
            a.cells.size() != b.cells.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.holes.size(); ++i) {
            if (a.holes[i].beg + delta != b.holes[i].beg || a.holes[i].end + delta != b.holes[i].end) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.runs.size(); ++i) {
            if (a.runs[i].beg + delta != b.runs[i].beg || a.runs[i].end + delta != b.runs[i].end ||
                a.runs[i].style != b.runs[i].style) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.cells.size(); ++i) {
            if (a.cells[i].beg + delta != b.cells[i].beg || a.cells[i].end + delta != b.cells[i].end ||
                a.cells[i].align != b.cells[i].align) {
                return false;
            }
        }
        return true;
    }

    // The producer proves unchanged text/presentation keys and supplies exactly
    // the changed rows. Identity checks reject skipped generations and IME caches.
    // Build every replacement before touching the live layout; table changes and
    // physical line count changes keep the general dependency-aware path.
    static bool patchCursorDecorations(InputState& state, const std::string& fontFamily,
                                       float fontSize, float viewportWidth,
                                       const LineDecorationSnapshot& snapshot,
                                       const LineDecorationChanges& changes) {
        if (!state.decorationSnapshot || changes.previous.lock() != state.decorationSnapshot ||
            changes.next.lock() != snapshot || changes.textRevision != state.textRevision ||
            snapshot->size() != state.decorationSnapshot->size() || changes.rows.empty()) return false;
        struct Patch { std::size_t first; std::vector<TextLine> lines; };
        std::vector<Patch> patches;
        bool geometryChanged = false;
        bool rescanWidth = false;
        float width = state.cachedTextWidth;
        int lastRow = -1;
        for (int row : changes.rows) {
            if (row <= lastRow || row < 0 || row >= static_cast<int>(snapshot->size())) return false;
            lastRow = row;
            const auto& next = (*snapshot)[row];
            const auto& previous = (*state.decorationSnapshot)[row];
            if (next.tableId >= 0 || previous.tableId >= 0) return false;
            const auto first = std::lower_bound(state.cachedLines.begin(), state.cachedLines.end(), row + 1,
                [](const TextLine& line, int number) { return line.lineNumber < number; });
            const auto end = std::upper_bound(first, state.cachedLines.end(), row + 1,
                [](int number, const TextLine& line) { return number < line.lineNumber; });
            if (first == end || !first->lineStart) return false;
            const int beginByte = first->start;
            const int endByte = (end - 1)->end;
            if (beginByte < 0 || endByte < beginByte || endByte > static_cast<int>(state.text.size()) ||
                (beginByte > 0 && state.text[beginByte - 1] != '\n') ||
                (endByte < static_cast<int>(state.text.size()) && state.text[endByte] != '\n')) return false;
            Patch patch{static_cast<std::size_t>(first - state.cachedLines.begin()), {}};
            appendMeasuredLine(patch.lines, state.text, beginByte, endByte,
                endByte < static_cast<int>(state.text.size()), fontSize, fontSize * 1.2f,
                fontFamily, viewportWidth, &next, row + 1);
            if (patch.lines.size() != static_cast<std::size_t>(end - first)) return false;
            for (std::size_t i = 0; i < patch.lines.size(); ++i) {
                const auto& old = first[i];
                auto& line = patch.lines[i];
                line.metricsTracked = old.metricsTracked;
                geometryChanged = geometryChanged || old.hidden != line.hidden ||
                    old.lineHeight != line.lineHeight || old.spaceBefore != line.spaceBefore;
                line.top = old.top;
                if (old.metrics.width >= state.cachedTextWidth && line.metrics.width < old.metrics.width) rescanWidth = true;
                width = std::max(width, line.metrics.width);
            }
            patches.push_back(std::move(patch));
        }
        for (auto& patch : patches) {
            for (std::size_t i = 0; i < patch.lines.size(); ++i) {
                state.cachedLines[patch.first + i] = std::move(patch.lines[i]);
                if (state.detailTrackingValid && compactMetricsEnabled(state))
                    ensureLineDetails(state, static_cast<int>(patch.first + i));
            }
        }
        if (rescanWidth) {
            width = 0.0f;
            for (const auto& line : state.cachedLines) width = std::max(width, line.metrics.width);
        }
        state.cachedTextWidth = width;
        if (geometryChanged) rebuildGeometry(state, fontSize * 1.2f);
        setDecorations(state, *snapshot, snapshot);
        ++debugLayoutStats().incremental;
        ++debugLayoutStats().cursorPatched;
        debugLayoutStats().cursorMeasuredRows += changes.rows.size();
        return true;
    }

    static void ensureLayoutCache(InputState& state,
                                  const std::string& fontFamily,
                                  float fontSize,
                                  float viewportWidth,
                                  bool multiline,
                                  LineDecorationView decorations = {},
                                  const LineDecorationSnapshot& snapshot = {},
                                  const LineDecorationChanges* changes = nullptr) {
        if(multiline && !state.wordWrap) viewportWidth = 0.0f;
        const LineDecorationView kNoDecorations;
        LineDecorationView nextDecorations =
            decorations != nullptr ? *decorations : kNoDecorations;

        // "除了装饰与文本之外的一切"都没变：字体、字号、宽度、单行还是多行。
        // 字体/宽度变化仍重排行；仅宽度变化时可复用表格固有列宽。
        const bool inputsUnchanged =
            state.layoutCacheValid && state.cachedMultiline == multiline &&
            state.cachedFontFamily == fontFamily &&
            state.cachedViewportMetrics == compactMetricsEnabled(state) &&
            std::fabs(state.cachedFontSize - fontSize) < 0.001f &&
            state.cachedLayoutPixelScale == core::TextPrimitive::layoutPixelScale() &&
            std::fabs(state.cachedViewportWidth - viewportWidth) < 0.001f;
        // 文本也没变：剩下的唯一变量是装饰（光标换到别的块了）。
        const bool layoutUnchanged = inputsUnchanged && state.cachedTextRevision == state.textRevision;

        // ── 事件相位的装饰引用别名（T4 复审 1）────────────────────────────────
        // cachedLines() 这类入口递进来的就是 **state.decorations 自己**。文本一旦已经变了
        // （编辑刚提交、装饰层还没按新文本重算过），这份"装饰"记的仍是旧行号 / 旧字节偏移：
        // 拿它按新文本测量，等于把旧行的 holes、字号、字体套到新行上 —— prevCursorIndex
        // 就会据此算出错的字节位置（同帧第二次 Backspace 删错字节）。
        // 注意"退全量"并不够：rebuildLayoutFull 同样会把这份旧装饰递给 measureLines。
        // 此刻没有新装饰可参考，唯一安全的口径是按**无装饰**整份重测（事件位置取保守的
        // 纯文本索引），并把装饰记账一并清掉 —— 下一帧 build 递来的新装饰因此一定被当成
        // "装饰全变了"逐行重测，旧行的 holes 不可能被复用。
        // 装饰本来就是空的不拦：那条无装饰增量路对新文本本来就是对的，不必多做一次全量。
        // Event helpers pass only the retained table reference, without a provider
        // snapshot. An explicit matching snapshot is the provider's current result:
        // it may intentionally share an unchanged generation across a text edit.
        if (decorations != nullptr && decorations.identity() == activeDecorations(state).identity() &&
            !(snapshot && snapshot.get() == decorations.identity()) &&
            !activeDecorations(state).empty() && state.cachedTextRevision != state.textRevision) {
            rebuildLayoutFull(state, fontFamily, fontSize, viewportWidth, multiline, kNoDecorations);
            return;
        }

        if (layoutUnchanged) {
            if (multiline && changes && snapshot && decorations.identity() == snapshot.get() &&
                patchCursorDecorations(state, fontFamily, fontSize, viewportWidth, snapshot, *changes)) {
                return;
            }
            // 只有装饰不同的那几行需要重新测量；光标在同一块内移动时两份装饰逐项相同，
            // 这里一行为零。表格行 / 判据不过 → 退全量（T4 §9 的 fallback 计数）。
            if (multiline) {
                if ((snapshot && snapshot == state.decorationSnapshot) ||
                    nextDecorations.identity() == activeDecorations(state).identity() ||
                    nextDecorations == activeDecorations(state)) {
                    // A cursor move can publish an equivalent generation. Keep its identity
                    // without rebuilding geometry or invalidating unchanged drawing content.
                    if (snapshot && snapshot != state.decorationSnapshot) {
                        state.decorationSnapshot = snapshot;
                        state.decorations.clear();
                    }
                    // 装饰也没动（光标在同一块内移动的常态）：一行都不用碰，
                    // 连几何/行宽都不必重算 —— 这一帧是纯读。
                    return;
                }
                if (updateChangedDecorations(state, nextDecorations, fontFamily, fontSize, viewportWidth)) {
                    // 行表就位后的公共收尾（几何表、行宽、装饰记账、缓存键）。
                    finishLayoutCache(state, fontFamily, fontSize, viewportWidth, multiline, nextDecorations, snapshot);
                    ++debugLayoutStats().incremental;
                } else {
                    ++debugLayoutStats().fallback;
                    rebuildLayoutFull(state, fontFamily, fontSize, viewportWidth, multiline, nextDecorations, snapshot);
                }
            }
            return;
        }

        const bool decorated = !nextDecorations.empty();
        if (multiline && inputsUnchanged) {
            // 文本（和/或装饰）变了，但测量输入没变 → 统一的逐行增量（T4 C）：
            // 受影响行重测、区间外整行复用并平移绝对偏移。装饰场景判据不过就落到
            // 下面的全量；无装饰场景沿用原来的段落级增量。
            bool incremental = false;
            bool stableGeometry = false;
            if (decorated) {
                incremental = updateChangedDecorations(state, nextDecorations, fontFamily, fontSize, viewportWidth,
                                                       &stableGeometry);
                if (!incremental) {
                    ++debugLayoutStats().fallback;
                }
            } else {
                updateChangedParagraphs(state, fontFamily, fontSize, viewportWidth);
                state.cachedTables.clear();  // 无装饰路径没有表格（见调用条件）
                std::vector<TableColumns>{}.swap(state.cachedTableIntrinsic);
                TableColumnIndex{}.swap(state.cachedTableIndex);
                incremental = true;
            }
            if (incremental) {
                finishLayoutCache(state, fontFamily, fontSize, viewportWidth, multiline, nextDecorations, snapshot,
                                  stableGeometry);
                ++debugLayoutStats().incremental;
                return;
            }
        }
        rebuildLayoutFull(state, fontFamily, fontSize, viewportWidth, multiline, nextDecorations, snapshot);
    }

    // 全量重排：整份 measureLines（装饰场景连表格列几何一起算）+ 公共收尾。
    // 单行路径原样保留在这里（改造前的 else 分支）。
    static void rebuildLayoutFull(InputState& state,
                                  const std::string& fontFamily,
                                  float fontSize,
                                  float viewportWidth,
                                  bool multiline,
                                  LineDecorationView nextDecorations,
                                  const LineDecorationSnapshot& snapshot = {}) {
        ++debugLayoutStats().full;
        if (multiline) {
            const bool reuseIntrinsic = state.layoutCacheValid && state.cachedMultiline &&
                state.cachedTextRevision == state.textRevision && state.cachedFontFamily == fontFamily &&
                std::fabs(state.cachedFontSize - fontSize) < 0.001f &&
                state.cachedLayoutPixelScale == core::TextPrimitive::layoutPixelScale() &&
                !state.cachedTableIntrinsic.empty() &&
                ((snapshot && snapshot == state.decorationSnapshot) ||
                 nextDecorations.identity() == activeDecorations(state).identity() || nextDecorations == activeDecorations(state));
            state.cachedLines = measureLines(state.text, fontFamily, fontSize, viewportWidth,
                                             nextDecorations, &state.cachedTables, &state.cachedTableIndex,
                                             reuseIntrinsic ? &state.cachedTableIntrinsic : nullptr,
                                             &state.cachedTableIntrinsic, compactMetricsEnabled(state));
            // 多行的光标与选择使用行级 metrics，不再重复 shaping 整篇文档。
            state.cachedMetrics = {};
            state.cachedLayoutText = state.text;
            state.cachedTextWidth = 0.0f;
            for (const TextLine& line : state.cachedLines) {
                state.cachedTextWidth = std::max(state.cachedTextWidth, line.metrics.width);
            }
            rebuildGeometry(state, fontSize * 1.2f);
        } else {
            state.cachedMetrics = measureMetrics(state.text, fontFamily, fontSize);
            state.cachedLines = {{0, static_cast<int>(state.text.size()), false, state.cachedMetrics,
                                  fontSize, fontSize * 1.2f, 0.0f, {}}};
            state.cachedTextWidth = state.cachedMetrics.width;
            state.cachedLayoutText.clear();
            state.cachedGeometry.build({});
            state.cachedTables.clear();
            std::vector<TableColumns>{}.swap(state.cachedTableIntrinsic);
            TableColumnIndex{}.swap(state.cachedTableIndex);
        }
        stampCacheKey(state, fontFamily, fontSize, viewportWidth, multiline);
        setDecorations(state, nextDecorations, snapshot);
    }

    // 增量路径的公共收尾（行表已在 updateChanged* 里就位）：几何表、缓存键、装饰记账。
    static void finishLayoutCache(InputState& state,
                                  const std::string& fontFamily,
                                  float fontSize,
                                  float viewportWidth,
                                  bool multiline,
                                  LineDecorationView nextDecorations,
                                  const LineDecorationSnapshot& snapshot = {},
                                  bool stableGeometry = false) {
        // updateChangedDecorations publishes raw and constrained columns together.
        state.cachedMetrics = {};
        state.cachedLayoutText = state.text;
        if (!stableGeometry) {
            state.cachedTextWidth = 0.0f;
            for (const TextLine& line : state.cachedLines) {
                state.cachedTextWidth = std::max(state.cachedTextWidth, line.metrics.width);
            }
            rebuildGeometry(state, fontSize * 1.2f);
        }
        stampCacheKey(state, fontFamily, fontSize, viewportWidth, multiline);
        setDecorations(state, nextDecorations, snapshot);
    }

    // 记下这次排版用的输入（下次据此判断"能不能走增量"）。
    static void stampCacheKey(InputState& state,
                              const std::string& fontFamily,
                              float fontSize,
                              float viewportWidth,
                              bool multiline) {
        state.cachedTextRevision = state.textRevision;
        state.cachedFontFamily = fontFamily;
        state.cachedFontSize = fontSize;
        state.cachedLayoutPixelScale = core::TextPrimitive::layoutPixelScale();
        state.cachedViewportWidth = viewportWidth;
        state.cachedMultiline = multiline;
        state.cachedViewportMetrics = compactMetricsEnabled(state);
        state.detailTrackingValid = false;
        state.layoutCacheValid = true;
    }

    // Called only after the byte-for-byte prefix/suffix proof below. With the same
    // source/visual row counts and geometry, edit the existing line storage in place.
    // Check every decoration (cursor reveal can change distant rows), and stage all
    // measurements before touching cachedLines. Tables retain their dependency path.
    static bool patchStableEditedRows(InputState& state,
                                      LineDecorationView nextDecorations,
                                      const std::string& fontFamily, float fontSize,
                                      float viewportWidth, int first, int last, int byteDelta) {
        const auto& previous = activeDecorations(state);
        const auto& cached = state.cachedLines;
        if (cached.empty() || nextDecorations.size() != previous.size() ||
            static_cast<int>(nextDecorations.size()) != cached.back().lineNumber ||
            first < 0 || last < first || last >= static_cast<int>(nextDecorations.size()) ||
            !state.cachedTables.empty()) return false;
        struct Patch { std::size_t begin; std::vector<TextLine> lines; };
        std::vector<Patch> patches;
        std::size_t physical = 0;
        int expectedBegin = 0;
        float width = state.cachedTextWidth;
        bool rescanWidth = false;
        for (int row = 0; row < static_cast<int>(nextDecorations.size()); ++row) {
            if (nextDecorations[row].tableId >= 0 || previous[row].tableId >= 0 ||
                physical >= cached.size() || cached[physical].lineNumber != row + 1) return false;
            const std::size_t begin = physical;
            while (physical < cached.size() && cached[physical].lineNumber == row + 1) ++physical;
            const int shift = row > last ? byteDelta : 0;
            const int newBegin = cached[begin].start + shift;
            if (row < first || row > last) {
                if (newBegin != expectedBegin) return false;
            } else if (row == first && cached[begin].start != expectedBegin) return false;
            int end;
            if (row >= first && row <= last) {
                const auto newline = state.text.find('\n', static_cast<std::size_t>(expectedBegin));
                end = newline == std::string::npos ? static_cast<int>(state.text.size())
                                                  : static_cast<int>(newline);
            } else {
                end = cached[physical - 1].end + shift;
            }
            if (end < expectedBegin || end > static_cast<int>(state.text.size()) ||
                (end < static_cast<int>(state.text.size()) && state.text[end] != '\n')) return false;
            const bool hardBreak = end < static_cast<int>(state.text.size());
            if ((row >= first && row <= last) ||
                !decorationsEquivalent(previous[row], nextDecorations[row], shift)) {
                Patch patch{begin, {}};
                appendMeasuredLine(patch.lines, state.text, expectedBegin, end, hardBreak,
                    fontSize, fontSize * 1.2f, fontFamily, viewportWidth, &nextDecorations[row], row + 1);
                if (patch.lines.size() != physical - begin) return false;
                for (std::size_t i = 0; i < patch.lines.size(); ++i) {
                    const auto& old = cached[begin + i];
                    auto& next = patch.lines[i];
                    if (old.hidden != next.hidden || old.lineHeight != next.lineHeight ||
                        old.spaceBefore != next.spaceBefore) return false;
                    next.top = old.top;
                    if (old.metrics.width == state.cachedTextWidth && next.metrics.width < old.metrics.width)
                        rescanWidth = true;
                    width = std::max(width, next.metrics.width);
                }
                patches.push_back(std::move(patch));
            }
            expectedBegin = end + (hardBreak ? 1 : 0);
        }
        if (physical != cached.size() || expectedBegin != static_cast<int>(state.text.size())) return false;
        // Indices remain stable. Translate only absolute metadata in the unchanged
        // suffix; all metrics/caret/run vector storage remains at its previous address.
        for (auto& line : state.cachedLines)
            if (line.lineNumber > last + 1) translateLineMetadata(line, byteDelta, 0);
        for (auto& patch : patches)
            std::move(patch.lines.begin(), patch.lines.end(), state.cachedLines.begin() + patch.begin);
        if (rescanWidth) {
            width = 0.0f;
            for (const auto& line : state.cachedLines) width = std::max(width, line.metrics.width);
        }
        state.cachedTextWidth = width;
        ++debugLayoutStats().editPatched;
        debugLayoutStats().editMeasuredRows += patches.size();
        return true;
    }

    // 文本和/或装饰变了：逐源行决定"整行复用（平移绝对偏移）"还是"重新测量"。
    //   · 文本没变（delta == 0）：只有装饰不同的行重测 —— 这就是"光标换块"的增量路
    //     （全量重测 2000 行是几十毫秒，而这条通常只碰 2~6 行）；
    //   · 文本变了：用前后缀差分定出受影响的源行区间，区间内重测，区间外整行复用并按
    //     delta 平移 start/end/holes/runs/lineNumber（T4 C 的 translateLineMetadata）。
    // 任何判据不过就返回 false，调用方整份重排（正确性优先，T4 §9）：
    // 表格（列 x 由整表测量决定）、复用段的行边界对不上、装饰平移后对不上、
    // 复用游标与行号映射对不上。
    static bool updateChangedDecorations(InputState& state,
                                         LineDecorationView nextDecorations,
                                         const std::string& fontFamily,
                                         float fontSize,
                                         float viewportWidth,
                                         bool* stableGeometry = nullptr) {
        if (stableGeometry) *stableGeometry = false;
        const std::string& newText = state.text;
        const std::string& oldText = state.cachedLayoutText;
        const auto previousDecorations = activeDecorations(state);
        // 文本与装饰都没动：一行都不用碰。**必须先看文本**——纯文本行的装饰可能全空，
        // 文本变了而装饰逐字节相同是完全可能的（此时不重测就会渲染旧行）。
        if (state.cachedTextRevision == state.textRevision &&
            nextDecorations == previousDecorations) {
            return true;
        }

        // ── 文本差分 → 受影响的源行区间 ────────────────────────────────────
        // 这里刻意**不依赖** InputState::pendingEdit：直接拿"上一次排版用的文本"
        // （cachedLayoutText）与现文本比，换文档 / IME 合成结束这类"没有编辑记录"
        // 的场景也能自洽（差分只会估得更宽，不会更窄）。
        int byteDelta = 0;
        int deltaLines = 0;
        int first = 0;      // 新文本第一个受影响行
        int newLast = -1;   // 新文本最后一个受影响行（-1 = 文本没变）
        int oldTail = 0;    // 旧文本第一个仍可整行复用的行
        if (newText != oldText) {
            const std::size_t commonSize = std::min(oldText.size(), newText.size());
            std::size_t prefix = 0;
            constexpr std::size_t block = 256;
            while (commonSize - prefix >= block &&
                   std::memcmp(oldText.data() + prefix, newText.data() + prefix, block) == 0) {
                prefix += block;
            }
            while (prefix < commonSize && oldText[prefix] == newText[prefix]) {
                ++prefix;
            }
            std::size_t suffix = 0;
            while (commonSize - prefix - suffix >= block &&
                   std::memcmp(oldText.data() + oldText.size() - suffix - block,
                               newText.data() + newText.size() - suffix - block, block) == 0) {
                suffix += block;
            }
            while (suffix < commonSize - prefix &&
                   oldText[oldText.size() - suffix - 1] == newText[newText.size() - suffix - 1]) {
                ++suffix;
            }
            const std::size_t oldEndByte = oldText.size() - suffix;
            const std::size_t newEndByte = newText.size() - suffix;
            // 行号两侧同源：前缀 [0, prefix) 逐字节相同，数新文本即可。
            first = countNewlines(newText, static_cast<int>(prefix));
            // newLast = 变动区间末字节所在行 = 前缀换行数 + [prefix, newEnd-1) 换行数。
            newLast = newEndByte > prefix
                          ? first + countNewlines(newText, static_cast<int>(prefix),
                                                  static_cast<int>(newEndByte) - 1)
                          : first;
            deltaLines = countNewlines(newText, static_cast<int>(prefix), static_cast<int>(newEndByte)) -
                         countNewlines(oldText, static_cast<int>(prefix), static_cast<int>(oldEndByte));
            byteDelta = static_cast<int>(newEndByte) - static_cast<int>(oldEndByte);
            oldTail = newLast + 1 - deltaLines;
            const int oldLineCount = countNewlines(oldText, static_cast<int>(oldText.size())) + 1;
            if (first < 0 || newLast < first || oldTail < first || oldTail > oldLineCount) {
                layoutRejectReason() = "差分：三段映射不自洽";
                return false;
            }
        }

        if (stableGeometry && newLast >= first && deltaLines == 0 &&
            patchStableEditedRows(state, nextDecorations, fontFamily, fontSize, viewportWidth,
                                  first, newLast, byteDelta)) {
            *stableGeometry = true;
            return true;
        }

        // A table's column widths depend on every visible cell. Preserve its geometry only
        // when every row maps to an equivalent old row; otherwise remeasure that whole table.
        std::vector<TableColumns> nextTables;
        std::vector<TableColumns> nextIntrinsic;
        nextIntrinsic.reserve(state.cachedTableIntrinsic.size());
        std::vector<std::pair<std::size_t, std::size_t>> intrinsicMoves;
        TableColumnIndex nextTableIndex;
        std::unordered_map<int, bool> dirtyTables;
        for (int begin = 0; begin < static_cast<int>(nextDecorations.size());) {
            const int id = nextDecorations[begin].tableId;
            if (id < 0) { ++begin; continue; }
            int end = begin + 1;
            while (end < static_cast<int>(nextDecorations.size()) && nextDecorations[end].tableId == id) ++end;
            if (dirtyTables.find(id) != dirtyTables.end()) {
                layoutRejectReason() = "non-contiguous table identity";
                return false;
            }
            bool dirty = false;
            int previousId = -1;
            int previousBegin = -1;
            for (int row = begin; row < end; ++row) {
                const int oldRow = row < first ? row : (row > newLast ? row - deltaLines : -1);
                const int shift = row > newLast ? byteDelta : 0;
                if (oldRow < 0 || oldRow >= static_cast<int>(previousDecorations.size()) ||
                    !decorationsEquivalent(previousDecorations[oldRow], nextDecorations[row], shift)) {
                    dirty = true;
                    break;
                }
                if (row == begin) { previousId = previousDecorations[oldRow].tableId; previousBegin = oldRow; }
                if (previousDecorations[oldRow].tableId != previousId) { dirty = true; break; }
            }
            // A removed row may have contained the widest cell. Equal surviving rows alone
            // do not prove the table is unchanged: its old group must have the same boundaries.
            if (!dirty && (previousBegin < 0 ||
                (previousBegin > 0 && previousDecorations[previousBegin - 1].tableId == previousId) ||
                (previousBegin + end - begin < static_cast<int>(previousDecorations.size()) &&
                 previousDecorations[previousBegin + end - begin].tableId == previousId))) dirty = true;
            const auto* oldTable = findTableColumns(state.cachedTables, state.cachedTableIndex, previousId);
            // Raw plans have the same order as constrained plans. Validate the slot before
            // using it; a missing plan is rebuilt, never inferred from clamped widths.
            const TableColumns* oldIntrinsic = nullptr;
            const auto oldSlot = state.cachedTableIndex.find(previousId);
            if (oldSlot != state.cachedTableIndex.end() && oldSlot->second < state.cachedTableIntrinsic.size() &&
                state.cachedTableIntrinsic[oldSlot->second].tableId == previousId) {
                oldIntrinsic = &state.cachedTableIntrinsic[oldSlot->second];
            }
            if (!dirty && oldTable != nullptr) {
                ++debugLayoutStats().tableReused;
                nextTables.push_back(*oldTable);
                nextTables.back().tableId = id;
                if (oldIntrinsic != nullptr) {
                    // Defer ownership transfer until all row reuse checks succeed. This
                    // avoids a width-vector allocation for every untouched table per edit.
                    intrinsicMoves.emplace_back(nextIntrinsic.size(), oldSlot->second);
                    nextIntrinsic.emplace_back();
                    nextIntrinsic.back().tableId = id;
                } else {
                    auto raw = buildTableIntrinsicColumns(newText, nextDecorations, fontFamily, fontSize, begin, end);
                    nextIntrinsic.insert(nextIntrinsic.end(), std::make_move_iterator(raw.begin()),
                                         std::make_move_iterator(raw.end()));
                }
            } else {
                ++debugLayoutStats().tableRebuilt;
                auto measured = buildTableIntrinsicColumns(newText, nextDecorations, fontFamily, fontSize, begin, end);
                nextIntrinsic.insert(nextIntrinsic.end(), measured.begin(), measured.end());
                constrainTableColumns(measured, fontSize, viewportWidth);
                nextTables.insert(nextTables.end(), std::make_move_iterator(measured.begin()),
                                  std::make_move_iterator(measured.end()));
            }
            dirtyTables.emplace(id, dirty || oldTable == nullptr);
            begin = end;
        }
        nextTableIndex.reserve(nextTables.size());
        for (std::size_t i = 0; i < nextTables.size(); ++i) {
            nextTableIndex.emplace(nextTables[i].tableId, i);
        }

        // 先把旧行表整份搬走，避免"边读边写"。
        std::vector<TextLine> previousLines = std::move(state.cachedLines);
        const float defaultLineHeight = fontSize * 1.2f;

        // ── 源行号 → 首条物理行的映射（T4 复审 2：软换行）─────────────────────
        // previousCursor 是 previousLines 的**物理行**下标，而 first / oldTail 是**源行号**
        // （按 '\n' 切的行，与装饰表下标同源）。软换行会把一个源行拆成多条 TextLine：
        // 前面任何一行折了行，源行号就不再等于物理下标 —— 直接比较会把每次编辑都判成
        // "对不上"退全量，直接赋值则会把游标指到改动区里的旧行。
        // 映射表按"每个源行号的首条物理行"记录；缺失的源行号留哨兵值 previousLines.size()，
        // 由下面的越界判据拒掉（宁可退全量，不做没把握的复用）。
        const bool textDiffers = newText != oldText;
        std::vector<std::size_t> firstPhysicalOf;
        if (textDiffers && !previousLines.empty()) {
            // 表按源行号非降序产出：末条的行号就是源行数（行号异常时下面的 resize 兜底）。
            const int sourceLineCount = std::max(1, previousLines.back().lineNumber);
            firstPhysicalOf.assign(static_cast<std::size_t>(sourceLineCount), previousLines.size());
            for (std::size_t index = 0; index < previousLines.size(); ++index) {
                const int source = previousLines[index].lineNumber - 1;
                if (source < 0) {
                    continue;
                }
                if (source >= static_cast<int>(firstPhysicalOf.size())) {
                    firstPhysicalOf.resize(static_cast<std::size_t>(source) + 1,
                                           previousLines.size());
                }
                if (firstPhysicalOf[static_cast<std::size_t>(source)] == previousLines.size()) {
                    firstPhysicalOf[static_cast<std::size_t>(source)] = index;  // 只记首个
                }
            }
        }
        const auto physicalIndexOf = [&](int sourceIndex) -> std::size_t {
            if (sourceIndex < 0 || sourceIndex >= static_cast<int>(firstPhysicalOf.size())) {
                return previousLines.size();  // 该源行没有对应的旧行 → 无从复用
            }
            return firstPhysicalOf[static_cast<std::size_t>(sourceIndex)];
        };

        std::vector<TextLine> lines;
        lines.reserve(previousLines.size());
        std::size_t previousCursor = 0;
        int lineBeg = 0;
        int lineIndex = 0;
        while (lineBeg <= static_cast<int>(newText.size())) {
            const std::size_t newline = newText.find('\n', static_cast<std::size_t>(lineBeg));
            const int lineEnd = newline == std::string::npos ? static_cast<int>(newText.size())
                                                             : static_cast<int>(newline);
            // 该行是否落在"文本变了"的区间里（区间内必须重测，旧对应行整段丢弃）。
            const bool textChanged = newLast >= first && lineIndex >= first && lineIndex <= newLast;
            if (lineIndex == first && newLast >= first) {
                // 进入重算区间：旧文本里被这次编辑吃掉的**源行**对应的物理段整段跳过。
                // first / oldTail 都是源行号，先换算成物理下标再与 previousCursor 比较/赋值
                // （软换行下两者不相等，见上面的映射表）。
                const std::size_t firstPhysical = physicalIndexOf(first);
                const std::size_t oldTailPhysical = physicalIndexOf(oldTail);
                if (firstPhysical >= previousLines.size() || previousCursor != firstPhysical ||
                    oldTailPhysical < previousCursor) {
                    layoutRejectReason() = "复用游标与行号映射对不上";
                    return false;
                }
                previousCursor = oldTailPhysical;
            }

            // 该行对应的旧源行（-1 = 不复用）。[0, first) 一一对应；newLast 之后整体
            // 平移 deltaLines 行、绝对偏移平移 byteDelta。
            const int oldIndex = lineIndex < first ? lineIndex
                : (lineIndex > newLast ? lineIndex - deltaLines : -1);
            const int shift = lineIndex > newLast ? byteDelta : 0;
            const int lineDeltaLines = lineIndex > newLast ? deltaLines : 0;

            const LineDecoration* next = lineIndex < static_cast<int>(nextDecorations.size())
                ? &nextDecorations[static_cast<std::size_t>(lineIndex)]
                : nullptr;

            bool reuse = false;
            std::size_t previousFirst = previousCursor;
            std::size_t previousLast = previousCursor;
            if (!textChanged) {
                // 该物理行覆盖的旧段（坐标先折回旧文本）：段的 start 落在 [b, e] 内。
                const int expectBeg = lineBeg - shift;
                const int expectEnd = lineEnd - shift;
                while (previousLast < previousLines.size() &&
                       previousLines[previousLast].start >= expectBeg &&
                       previousLines[previousLast].start <= expectEnd) {
                    ++previousLast;
                }
                const LineDecoration* previous =
                    oldIndex >= 0 && oldIndex < static_cast<int>(previousDecorations.size())
                        ? &previousDecorations[static_cast<std::size_t>(oldIndex)]
                        : nullptr;
                if (previousLast > previousCursor) {
                    // ① 装饰平移后必须逐字段一致（不一致 = 该行要按新装饰重测）；
                    const bool decorationsMatch =
                        (next == nullptr && previous == nullptr) ||
                        (next != nullptr && previous != nullptr &&
                         decorationsEquivalent(*previous, *next, shift));
                    // ② 旧段必须严丝合缝盖住这一行（首段起点、末段终点折回旧坐标后对齐）；
                    const bool boundariesMatch =
                        previousLines[previousCursor].start == expectBeg &&
                        previousLines[previousLast - 1].end == expectEnd;
                    // ③ 源行号也得对得上（软换行的续段共享同一个源行号）。
                    const bool lineNumbersMatch =
                        previousLines[previousCursor].lineNumber == oldIndex + 1;
                    reuse = decorationsMatch && boundariesMatch && lineNumbersMatch;
                }
                previousCursor = previousLast;
            }

            if (next != nullptr && next->tableId >= 0 && dirtyTables.at(next->tableId)) reuse = false;
            if (reuse) {
                for (std::size_t i = previousFirst; i < previousLast; ++i) {
                    TextLine line = std::move(previousLines[i]);
                    translateLineMetadata(line, shift, lineDeltaLines);
                    lines.push_back(std::move(line));
                }
            } else {
                // 行号必须跟着盖章（sourceLineNumber 默认是 1）：漏了的话，凡是走
                // 增量路径重测的行行号全是 1（全量重排一次才恢复）。
                appendMeasuredLine(lines, newText, lineBeg, lineEnd, newline != std::string::npos,
                                   fontSize, defaultLineHeight, fontFamily, viewportWidth, next,
                                   lineIndex + 1, next != nullptr && next->tableId >= 0
                                       ? findTableColumns(nextTables, nextTableIndex, next->tableId) : nullptr);
            }
            if (newline == std::string::npos) {
                break;
            }
            lineBeg = static_cast<int>(newline) + 1;
            ++lineIndex;
        }
        if (lines.empty()) {
            lines.push_back({0, 0, false, measureMetrics({}, fontFamily, fontSize),
                             fontSize, defaultLineHeight, 0.0f, {}});
        }
        state.cachedLines = std::move(lines);
        state.cachedTables = std::move(nextTables);
        for (const auto& move : intrinsicMoves) {
            const int id = nextIntrinsic[move.first].tableId;
            nextIntrinsic[move.first] = std::move(state.cachedTableIntrinsic[move.second]);
            nextIntrinsic[move.first].tableId = id;
        }
        if (nextIntrinsic.empty()) std::vector<TableColumns>{}.swap(nextIntrinsic);
        state.cachedTableIntrinsic = std::move(nextIntrinsic);
        state.cachedTableIndex = std::move(nextTableIndex);
        // 必须记下这一份：否则下一次还会拿"更早的表"做比较，既算错可复用的行、版本号也推不动。
        return true;
    }

    // 逐行 top 前缀和 + 块间距 gap 前缀和。行高为 0 的行（异常输入）回落到控件默认行高，
    // 不会把后面所有行顶飞。
    // 渲染用的 line.top 直接取自这张表，避免"渲染一条路径、滚动另一条路径"各算一遍。
    // 折叠（S3f 批次 C）的 hidden 行显式给 0 高：contentHeight / 滚动钳制 / 滚动条缩略图
    // 全部吃这张表，自动跟着变，一行都不用另改。
    // 块间距（R1）同样按行给：hidden 行的 gap 一并归零，折叠不会留下幽灵间距。
    static void rebuildGeometry(InputState& state, float fallbackLineHeight) {
        ++debugLayoutStats().geometryRebuilt;
        std::vector<float> heights;
        std::vector<float> spaceBefore;
        heights.reserve(state.cachedLines.size());
        spaceBefore.reserve(state.cachedLines.size());
        for (const TextLine& line : state.cachedLines) {
            heights.push_back(line.hidden ? 0.0f
                                          : (line.lineHeight > 0.0f ? line.lineHeight : fallbackLineHeight));
            spaceBefore.push_back(line.hidden ? 0.0f : std::max(0.0f, line.spaceBefore));
        }
        state.cachedGeometry.build(heights, spaceBefore);
        for (std::size_t i = 0; i < state.cachedLines.size(); ++i) {
            state.cachedLines[i].top = state.cachedGeometry.top(static_cast<int>(i));
        }
    }

    static void updateChangedParagraphs(InputState& state, const std::string& fontFamily,
                                        float fontSize, float viewportWidth) {
        const auto& oldText = state.cachedLayoutText;
        const auto& newText = state.text;
        const size_t common = std::min(oldText.size(), newText.size());
        size_t prefix = 0;
        constexpr size_t block = 256;
        while (common - prefix >= block && std::memcmp(oldText.data() + prefix, newText.data() + prefix, block) == 0) prefix += block;
        while (prefix < common && oldText[prefix] == newText[prefix]) ++prefix;
        if (prefix == oldText.size() && prefix == newText.size()) return;
        size_t suffix = 0;
        while (common - prefix - suffix >= block &&
               std::memcmp(oldText.data() + oldText.size() - suffix - block,
                           newText.data() + newText.size() - suffix - block, block) == 0) suffix += block;
        while (suffix < common - prefix && oldText[oldText.size() - suffix - 1] == newText[newText.size() - suffix - 1]) ++suffix;

        // 改动范围扩展到硬换行边界，保留 ligature、UTF-8 和软换行的完整 shaping 上下文。
        const size_t previousBreak = prefix ? newText.rfind('\n', prefix - 1) : std::string::npos;
        const size_t begin = previousBreak == std::string::npos ? 0 : previousBreak + 1;
        const size_t nextBreak = oldText.find('\n', oldText.size() - suffix);
        const size_t oldEnd = nextBreak == std::string::npos ? oldText.size() : nextBreak + 1;
        const size_t newEnd = newText.size() - (oldText.size() - oldEnd);
        const bool hasSuffix = oldEnd < oldText.size();
        auto replacement = measureLines(newText.substr(begin, newEnd - begin), fontFamily, fontSize, viewportWidth);
        if (hasSuffix) replacement.pop_back(); // 后缀已有该段开头，不保留截断串产生的虚拟空行。
        // 行号补偿（同 updateChangedDecorations 的盖章问题）：measureLines 拿到的是
        // 截出来的子串，行号从 1 编起；这里平移回它在整篇文档里的真实行号。
        const int replacementBase =
            static_cast<int>(std::count(newText.begin(), newText.begin() + static_cast<std::ptrdiff_t>(begin), '\n')) + 1;
        for (auto& line : replacement) {
            line.start += static_cast<int>(begin);
            line.end += static_cast<int>(begin);
            line.lineNumber += replacementBase - 1;
        }
        auto& lines = state.cachedLines;
        const auto lower = [&](size_t offset) {
            return std::lower_bound(lines.begin(), lines.end(), static_cast<int>(offset),
                [](const TextLine& line, int index) { return line.start < index; });
        };
        const size_t first = static_cast<size_t>(lower(begin) - lines.begin());
        const size_t last = hasSuffix ? static_cast<size_t>(lower(oldEnd) - lines.begin()) : lines.size();
        const size_t removed = last - first;
        // 常见的单字符修改不改变可视行数，不搬动整篇文档的 metrics 容器。
        if (removed == replacement.size()) {
            std::move(replacement.begin(), replacement.end(), lines.begin() + first);
        } else {
            lines.erase(lines.begin() + first, lines.begin() + last);
            lines.insert(lines.begin() + first, std::make_move_iterator(replacement.begin()), std::make_move_iterator(replacement.end()));
        }
        const int delta = static_cast<int>(newEnd) - static_cast<int>(oldEnd);
        // 替换段里增/删了换行时，其后所有行的源行号整体平移同样的行数；
        // start/end 的 delta 只反映字节数，行号必须单独补。
        const int deltaLines =
            static_cast<int>(std::count(newText.begin() + static_cast<std::ptrdiff_t>(begin),
                                        newText.begin() + static_cast<std::ptrdiff_t>(newEnd), '\n')) -
            static_cast<int>(std::count(oldText.begin() + static_cast<std::ptrdiff_t>(begin),
                                        oldText.begin() + static_cast<std::ptrdiff_t>(oldEnd), '\n'));
        for (size_t i = first + replacement.size(); i < lines.size(); ++i) {
            // 统一走行级平移 helper（T4 C）：散落加法会漏字段 —— 改造前这里就漏了
            // runs（样式段的 beg/end 是文档绝对偏移）。
            translateLineMetadata(lines[i], delta, deltaLines);
        }
    }

    static void transferLayoutCache(InputState& source, InputState& target) {
        target.cachedLines = std::move(source.cachedLines);
        target.viewportMetrics = source.viewportMetrics;
        target.cachedViewportMetrics = source.cachedViewportMetrics;
        target.detailTrackingValid = false;
        target.detailedRows.clear();
        target.cachedGeometry = source.cachedGeometry;
        target.cachedTables = std::move(source.cachedTables);
        target.cachedTableIndex = std::move(source.cachedTableIndex);
        target.cachedTableIntrinsic = std::move(source.cachedTableIntrinsic);
        target.cachedMetrics = std::move(source.cachedMetrics);
        target.cachedLayoutText = std::move(source.cachedLayoutText);
        target.cachedFontFamily = std::move(source.cachedFontFamily);
        target.cachedFontSize = source.cachedFontSize;
        target.cachedLayoutPixelScale = source.cachedLayoutPixelScale;
        target.cachedViewportWidth = source.cachedViewportWidth;
        target.cachedMultiline = source.cachedMultiline;
        target.cachedTextWidth = source.cachedTextWidth;
        target.layoutCacheValid = source.layoutCacheValid;
        // Legacy providers keep their owned vector; snapshot providers share an immutable generation.
        target.decorations = source.decorations;
        target.decorationSnapshot = source.decorationSnapshot;
        target.decorationRevision = source.decorationRevision;
        target.cachedTextRevision = target.textRevision - 1;
        source.layoutCacheValid = false;
    }

    static InputState& displayState(InputState& state, bool composing) {
        if (!composing) {
            if (state.preedit) transferLayoutCache(*state.preedit, state);
            state.preedit.reset();
            return state;
        }
        if (!state.preedit) {
            state.preedit = std::make_unique<InputState>();
            // 预编辑开始复用现有排版，避免首个拼音键再次测量整个长文档。
            transferLayoutCache(state, *state.preedit);
        }
        auto& display = *state.preedit;
        const auto range = selectionRange(state);
        std::string text = state.text;
        text.replace(static_cast<size_t>(range.first), static_cast<size_t>(range.second - range.first), state.compositionText);
        if (display.text != text) {
            display.text = std::move(text);
            ++display.textRevision;
        }
        display.cursor = range.first + static_cast<int>(state.compositionText.size());
        display.selectionStart = range.first;
        display.selectionEnd = display.cursor;
        display.followCaret = state.followCaret;
        display.horizontalScroll = state.horizontalScroll;
        display.wordWrap = state.wordWrap;
        display.viewportMetrics = state.viewportMetrics;
        display.verticalScroll = state.verticalScroll;
        return display;
    }

    // 光标/选择等入口拿到的行表必须与 layout 用的是同一份装饰，否则缓存会来回打架
    // （一边按装饰排、一边按纯文本排，每帧各重排一次）。装饰就存在 InputState 上。
    static const std::vector<InputLayout::Line>& cachedLines(InputState& state,
                                                            const std::string& fontFamily,
                                                            float fontSize,
                                                            float viewportWidth,
                                                            bool multiline) {
        ensureLayoutCache(state, fontFamily, fontSize, viewportWidth, multiline, activeDecorations(state));
        resetDetailTracking(state);
        const int cursorLine = lineIndexFor(state.cachedLines, state.cursor);
        for (int row = std::max(0, cursorLine - 1); row <= cursorLine + 1; ++row)
            ensureLineDetails(state, row);
        return state.cachedLines;
    }

    static const core::TextPrimitive::TextMetrics& cachedMetrics(InputState& state,
                                                                 const std::string& fontFamily,
                                                                 float fontSize,
                                                                 float viewportWidth) {
        ensureLayoutCache(state, fontFamily, fontSize, viewportWidth, false);
        return state.cachedMetrics;
    }

    static bool compactMetricsEnabled(const InputState& state) {
        return state.viewportMetrics && state.text.size() >= 256 * 1024;
    }

    static void releaseLineDetails(TextLine& line) {
        std::vector<int>{}.swap(line.metrics.byteIndices);
        std::vector<float>{}.swap(line.metrics.caretX);
        line.metricsDeferred = true;
        line.metricsTracked = false;
    }

    // Rehydrate the exact already-wrapped segment. Tables keep full caret arrays:
    // their segments share byte ranges and have non-contiguous per-cell stops.
    // This never changes wrap boundaries, width, top, height, runs or decoration.
    static void ensureLineDetails(InputState& state, int index) {
        if (index < 0 || index >= static_cast<int>(state.cachedLines.size())) return;
        auto& line = state.cachedLines[static_cast<std::size_t>(index)];
        if (line.metricsDeferred) {
            const auto& family = line.fontFamily.empty() ? state.cachedFontFamily : line.fontFamily;
            core::TextPrimitive::TextMetrics metrics;
            if (line.runs.empty()) {
                metrics = measureMetrics(projectText(state.text, line.start, line.end, line.holes),
                                         family, line.fontSize);
            } else {
                std::vector<LineRun> runs;
                runs.reserve(line.runs.size());
                for (const auto& run : line.runs) runs.push_back({run.beg, run.end, run.style});
                const auto pieces = buildVisiblePieces(line.holes, line.start, line.end, runs);
                metrics = layoutStyledRange(state.text, line.start, line.end, line.holes, pieces,
                    0, visibleLength(line.holes, line.start, line.end), line.fontSize, family).metrics;
            }
            const float shift = line.contentIndent + glyphAdvanceOf(line.glyph, line.fontSize);
            for (auto& x : metrics.caretX) x += shift;
            line.metrics.byteIndices = std::move(metrics.byteIndices);
            line.metrics.caretX = std::move(metrics.caretX);
            line.metricsDeferred = false;
            ++debugLayoutStats().detailMeasuredRows;
        }
        if (compactMetricsEnabled(state) && line.tableId < 0 && !line.metricsTracked) {
            line.metricsTracked = true;
            state.detailedRows.push_back(static_cast<std::size_t>(index));
        }
    }

    // Only a new layout generation needs a whole-row pass. Stable frames prune
    // the small resident set, instead of walking the document on each blink.
    static void resetDetailTracking(InputState& state) {
        if (state.detailTrackingValid) return;
        state.detailedRows.clear();
        if (state.cachedMultiline && compactMetricsEnabled(state)) {
            for (auto& line : state.cachedLines) {
                if (line.tableId < 0) releaseLineDetails(line);
            }
        }
        state.detailTrackingValid = true;
    }

    static void prepareViewportDetails(InputState& state, float height) {
        if (!compactMetricsEnabled(state)) return;
        const int count = static_cast<int>(state.cachedLines.size());
        const int first = std::max(0, state.cachedGeometry.firstVisibleLine(state.verticalScroll) - 2);
        const int last = std::min(count - 1, state.cachedGeometry.lastVisibleLine(state.verticalScroll, height) + 2);
        const int cursor = lineIndexFor(state.cachedLines, state.cursor);
        std::size_t kept = 0;
        for (const auto row : state.detailedRows) {
            const int index = static_cast<int>(row);
            if ((index >= first && index <= last) || std::abs(index - cursor) <= 1) {
                state.detailedRows[kept++] = row;
            } else {
                releaseLineDetails(state.cachedLines[row]);
            }
        }
        state.detailedRows.resize(kept);
        for (int row = first; row <= last; ++row) {
            if (!state.cachedLines[static_cast<std::size_t>(row)].hidden) ensureLineDetails(state, row);
        }
        for (int row = std::max(0, cursor - 1); row <= cursor + 1; ++row) ensureLineDetails(state, row);
    }

    // ── T19 视觉修正的纯判据（组件渲染/测量共用，可直接单测）──────────────────

    // 行首图元的文字右移量（T19 视觉修正 2 / C1 文本 marker）。
    //   · codepoint == 0 且 text 为空 = 这行没有图元 → 0（wrapWidth 不扣、applyLineGlyph 不移）；
    //   · advance > 0 → 用显式值（"图标比默认更宽/更窄"的口子；文本 marker 的实测
    //     宽也经这里放行 —— 装饰层留 0，组件在测量阶段把 [listIndentBeg, End) 的
    //     实测宽填进 advance，再走同一条公式）；
    //   · 否则按字号推算：max(4.0f, effectiveFontSize * kGlyphAdvanceEm)。
    // **测量与平移必须走这一个公式**：wrapWidth 在测量前把这块占位扣掉，applyLineGlyph
    // 又要在测量后把它平移上去 —— 两处各写一份必然漂移（T19 就是这么溢出的：平移算了、
    // 测量没算，带复选框的满行会超出视口一个 advance）。
    // effectiveFontSize 必须与最终 line.fontSize 同源：
    // decoration->fontSize > 0 ? decoration->fontSize : 控件字号。
    static float glyphAdvanceOf(const LineGlyph& glyph, float effectiveFontSize) {
        if (glyph.codepoint == 0 && glyph.text.empty()) {
            return 0.0f;
        }
        if (glyph.advance > 0.0f) {
            return glyph.advance;
        }
        return std::max(4.0f, effectiveFontSize * kGlyphAdvanceEm);
    }

    // 这一行要不要画行号/行号位图元（T19 视觉修正 1）。
    // 行号槽按行字号排（slot ≈ 1.2em），行盒本身装不下该行字号时画上去只会糊到邻行：
    // 典型就是 Live Preview 表格的分隔行（|---|，装饰层把行高压到 3px、整行藏进 holes）。
    //   · tableSeparator → 不画（分隔行没有可对齐的行文字）；
    //   · linePixelHeight < alignFontSize → 不画（行高装不下这一行的字）。
    // 已知边界：**极大字号 + 行高由很小的图片给出**的行也会被判成"装不下"而跳过行号
    // （图片行的行高来自装饰层的 imageHeight，不随字号缩放）。
    static bool gutterLineUsable(const TextLine& line, float linePixelHeight, float alignFontSize) {
        if (line.tableSeparator) {
            return false;
        }
        return linePixelHeight >= alignFontSize;
    }

    // 测量一个物理行，把结果（可能因超宽切成多段）追加到 out。
    // decoration 为 nullptr 表示"不装饰"：沿用控件默认字号/行高、不隐藏任何字节。
    // measureLines() 与 updateChangedDecorations() 共用这一条路，避免两处各写一份切段逻辑。
    // 行级矩形（底色/竖条）要附加到**本行产出的每一条** TextLine 上：软换行会把一个源行
    // 拆成多条，而底色/竖条是按源行给的。统一在这里补，而不是去 appendMeasuredLineCore 的
    // 每个出口写一遍 —— 那条路径有 4 个出口，漏一个就会出现"只有最后一段有底色"。
    static void appendMeasuredLine(std::vector<InputLayout::Line>& out,
                                   const std::string& value,
                                   int start,
                                   int end,
                                   bool hardBreakAfter,
                                   float defaultFontSize,
                                   float defaultLineHeight,
                                   const std::string& fontFamily,
                                   float viewportWidth,
                                   const LineDecoration* decoration,
                                   int sourceLineNumber = 1,
                                   const TableColumns* table = nullptr,
                                   bool detailed = true) {
        const std::size_t firstNew = out.size();
        // 换行宽度先扣掉行内容左缩进 + 行首图元占位（T19 视觉修正 2）：这两块都是
        // **测量之后**才平移上去的（applyLineIndent / applyLineGlyph），不减的话满行的
        // 软换行末字符会被推出视口裁掉。图元那块以前没扣 —— 带任务复选框的满行会整体
        // 超出视口一个 advance。公式与字号两处都与 applyLineGlyph 同源（见 glyphAdvanceOf）：
        // 扣的与加的必须是同一个数，否则行会一帧比一帧窄/宽。
        const float effectiveFontSize = decoration != nullptr && decoration->fontSize > 0.0f
            ? decoration->fontSize
            : defaultFontSize;
        // C1：图元/缩进的**解析值**。文本 marker（glyph.text 非空）与列表续行缩进
        // （listIndentBeg >= 0）都按"度量源区间"用本行字体实测 —— 测量、wrapWidth、
        // caretX、run.x、选择、命中拿到的是同一个数，且随 DPI 缩放走（不能在装饰层
        // 给死值：装饰层没有行字体与像素缩放）。marker 行的实测宽充当图元 advance，
        // 物理续行的实测宽叠加进 contentIndent —— 两端因此天然对齐（同一份源文前缀）。
        LineGlyph resolvedGlyph;
        float resolvedIndent = 0.0f;
        if (decoration != nullptr) {
            resolvedGlyph = decoration->glyph;
            resolvedIndent = decoration->contentIndent > 0.0f ? decoration->contentIndent : 0.0f;
            if (decoration->listIndentBeg >= 0 && decoration->listIndentEnd > decoration->listIndentBeg &&
                decoration->listIndentEnd <= static_cast<int>(value.size())) {
                const std::string measureSource = value.substr(
                    static_cast<std::size_t>(decoration->listIndentBeg),
                    static_cast<std::size_t>(decoration->listIndentEnd - decoration->listIndentBeg));
                const std::string& measureFont =
                    decoration->fontFamily.empty() ? fontFamily : decoration->fontFamily;
                const float bodyColumn =
                    core::TextPrimitive::measureTextWidth(measureSource, measureFont, effectiveFontSize);
                if (!resolvedGlyph.text.empty() && resolvedGlyph.codepoint == 0 &&
                    resolvedGlyph.advance <= 0.0f) {
                    resolvedGlyph.advance = bodyColumn;  // marker 行：正文列宽 = 图元占位
                } else {
                    resolvedIndent += bodyColumn;        // 物理续行：与所属项正文起点对齐
                }
            }
        }
        float reservedWidth = 0.0f;
        if (decoration != nullptr) {
            reservedWidth = resolvedIndent +
                            glyphAdvanceOf(resolvedGlyph, effectiveFontSize);
        }
        const float wrapWidth = reservedWidth > 0.0f
            ? std::max(0.0f, viewportWidth - reservedWidth)
            : viewportWidth;
        appendMeasuredLineCore(out, value, start, end, hardBreakAfter, defaultFontSize,
                               defaultLineHeight, fontFamily, wrapWidth, decoration, table, detailed);
        // 行号盖章：软换行产生的多条可视行共享同一源行号，lineStart 只标第一条。
        for (std::size_t index = firstNew; index < out.size(); ++index) {
            out[index].lineNumber = sourceLineNumber;
            out[index].lineStart = index == firstNew;
            // Table identity is a geometry dependency even without a visible box.
            if (table != nullptr && decoration != nullptr) {
                out[index].tableId = decoration->tableId;
                out[index].tableSeparator = decoration->tableSeparator;
            }
            if (!detailed && table == nullptr) {
                releaseLineDetails(out[index]);
                ++debugLayoutStats().coarseRows;
            }
        }
        // 行级字体盖章（代码块等宽）：渲染层从 TextLine.fontFamily 取，与测量同源。
        if (decoration != nullptr && !decoration->fontFamily.empty()) {
            for (std::size_t index = firstNew; index < out.size(); ++index) {
                out[index].fontFamily = decoration->fontFamily;
            }
        }
        // 行号位盖章（折叠箭头）：只在 lineStart 段画，续段保留行号数字。
        if (decoration != nullptr && decoration->gutterGlyph.codepoint != 0) {
            for (std::size_t index = firstNew; index < out.size(); ++index) {
                if (out[index].lineStart) {
                    out[index].gutterGlyph = decoration->gutterGlyph;
                }
            }
        }
        // 行内文字下移盖章（Obsidian 标题 padding-top）：文字/光标/行号对齐全用它。
        // 表格行的换行续段不盖（2026-09-26）：单元格的上下内边距只装在首段的
        // textShiftY 与末段的行盒余量里，中间续段要顶着上一段排。
        if (decoration != nullptr && decoration->textShiftY != 0.0f) {
            for (std::size_t index = firstNew; index < out.size(); ++index) {
                if (decoration->tableId >= 0 && index > firstNew) {
                    continue;
                }
                out[index].textShiftY = decoration->textShiftY;
            }
        }
        // 块间距盖章（R1）：只给本源行的**第一条**可视段 —— 软换行的续行跟在同一条
        // 源行里，重复盖章会在段中间撕出一道 gap。隐藏行不盖（折叠不留幽灵间距）。
        if (decoration != nullptr && decoration->spaceBefore > 0.0f && !decoration->hidden &&
            firstNew < out.size()) {
            out[firstNew].spaceBefore = decoration->spaceBefore;
        }
        // 语言标签盖章（只给本源行的第一条可视段，同 spaceBefore）。
        if (decoration != nullptr && !decoration->languageLabel.empty() && !decoration->hidden &&
            firstNew < out.size()) {
            out[firstNew].languageLabel = decoration->languageLabel;
        }
        if (decoration == nullptr) {
            return;
        }
        const LineBoxStyle& box = decoration->box;
        // gridColor 也算"有盒"：表格行现在**没有底色**（Obsidian 的表格是纯网格），
        // 网格线是这一行唯一的视觉信息，漏掉它整张表就只剩文字了。
        const bool hasBox = box.background.a > 0.0f ||
                            (box.barColor.a > 0.0f && box.barWidth > 0.0f) ||
                            box.gridColor.a > 0.0f;
        const bool hasGlyph = decoration->glyph.codepoint != 0 || !decoration->glyph.text.empty();
        const bool hasIndent = resolvedIndent > 0.0f;
        const bool hasImage = !decoration->imagePath.empty();
        const bool hasImageFail = decoration->imageFailed;
        const bool hasHidden = decoration->hidden || decoration->hiddenByFold;
        if (!hasBox && !hasGlyph && !hasIndent && !hasImage && !hasImageFail && !hasHidden) {
            return;
        }
        for (std::size_t index = firstNew; index < out.size(); ++index) {
            InputLayout::Line& line = out[index];
            if (decoration->tableId >= 0) {
                // 表格行身份（网格线用）：所有物理段都盖同一个 id（软换行理论上不出
                // 现在表格里，但盖满无害）。
                line.tableId = decoration->tableId;
                line.tableSeparator = decoration->tableSeparator;
            }
            if (hasBox) {
                line.box = box;
                // A physical source line can wrap into several visual segments.
                // Its rounded block edges belong only to the outermost segments.
                line.box.backgroundBlockFirst = box.backgroundBlockFirst && index == firstNew;
                line.box.backgroundBlockLast = box.backgroundBlockLast && index + 1 == out.size();
            }
            if (hasGlyph) {
                applyLineGlyph(line, resolvedGlyph);
            }
            if (hasIndent) {
                applyLineIndent(line, resolvedIndent);
            }
            if (hasImage) {
                line.imagePath = decoration->imagePath;
                line.imageWidth = decoration->imageWidth;
                line.imageHeight = decoration->imageHeight;
            }
            if (hasImageFail) {
                line.imageFailed = true;
                line.imageWidth = decoration->imageWidth;
                line.imageHeight = decoration->imageHeight;
                line.imageFailText = decoration->imageFailText;
            }
            if (hasHidden) {
                line.hidden = decoration->hidden;
                line.hiddenByFold = decoration->hiddenByFold || decoration->hidden;
            }
        }
    }

    // 行首图元：把整行的文字右移 advance（软换行出来的每一段都移，续行才对得齐）。
    //
    // 只动 metrics.width / caretX 与 run.x：排版结果里所有横向坐标都出自这两处，
    // 字节索引一个都不用改 —— 图元不对应文档里的任何字节，光标"越过"它就是行首。
    // advance 走 glyphAdvanceOf（与测量前扣 wrapWidth 同一个公式、同一份字号）：
    // line.fontSize 就是 appendMeasuredLineCore 的 lineFontSize，与装饰/控件字号同源。
    static void applyLineGlyph(InputLayout::Line& line, const LineGlyph& glyph) {
        const float advance = glyphAdvanceOf(glyph, line.fontSize);
        line.glyph = glyph;
        line.glyph.advance = advance;
        line.metrics.width += advance;
        for (float& caret : line.metrics.caretX) {
            caret += advance;
        }
        for (TableDocCaretStop& stop : line.tableDocCaretStops) stop.x += advance;
        for (TextRun& run : line.runs) {
            run.x += advance;
        }
    }

    // 行内容左缩进：与 applyLineGlyph 完全同一套平移（metrics.width / caretX / run.x），
    // 只是不画图标、也不参与 onGlyph 命中。缩进不对应文档里的任何字节，
    // 光标"越过"它就是行首；软换行的每一段都移（续行才对得齐）。
    static void applyLineIndent(InputLayout::Line& line, float indent) {
        line.contentIndent = indent;
        line.metrics.width += indent;
        for (float& caret : line.metrics.caretX) {
            caret += indent;
        }
        for (TableDocCaretStop& stop : line.tableDocCaretStops) stop.x += indent;
        for (TextRun& run : line.runs) {
            run.x += indent;
        }
    }

    static void appendMeasuredLineCore(std::vector<InputLayout::Line>& out,
                                   const std::string& value,
                                   int start,
                                   int end,
                                   bool hardBreakAfter,
                                   float defaultFontSize,
                                   float defaultLineHeight,
                                   const std::string& fontFamily,
                                   float viewportWidth,
                                   const LineDecoration* decoration,
                                   const TableColumns* table = nullptr,
                                   bool detailed = true) {
        float lineFontSize = defaultFontSize;
        float lineHeight = defaultLineHeight;
        // alpha == 0 = "本行没有行级颜色，交给控件默认"（见 TextLine::color 的约定）。
        // **必须显式透明**：core::Color 的默认构造是不透明白色（render_types.h），
        // 无装饰行不会进下面的 textColor 赋值分支，默认构造的白会被原样带进
        // TextLine.color —— 渲染层看 a > 0 判真，就永远拿白色画，主题色被架空。
        core::Color lineColor{0.0f, 0.0f, 0.0f, 0.0f};
        // 行级字体（Live Preview 的代码块等宽）：装饰给了就用它，否则控件字体。
        // 下面三条测量路径与渲染层用的是同一份字面，光标/caret 天然对得上。
        const std::string lineFontFamily =
            decoration != nullptr && !decoration->fontFamily.empty() ? decoration->fontFamily : fontFamily;
        std::vector<LineHole> holes;
        std::vector<LineRun> runs;
        if (decoration != nullptr) {
            if (decoration->fontSize > 0.0f) {
                lineFontSize = decoration->fontSize;
            }
            lineHeight = decoration->lineHeight > 0.0f ? decoration->lineHeight : lineFontSize * 1.2f;
            lineColor = decoration->textColor;
            if (!decoration->holes.empty()) {
                holes = clipHoles(normalizeHoles(decoration->holes), start, end);
            }
            if (!decoration->runs.empty()) {
                runs = decoration->runs;
            }
        }

        // ── 表格行：逐格排版 + 平移到列 x（S3f 批次 D）──
        // 占位在"样式段"之前：表格行的段是**按格**排的，不能按整行逐段铺开。
        if (table != nullptr && decoration != nullptr && !decoration->cells.empty()) {
            appendMeasuredTableCellLine(out, value, start, end, hardBreakAfter, lineFontSize, lineHeight,
                                        lineColor, lineFontFamily, holes, runs, *table, *decoration);
            return;
        }

        // ── 有行内样式段：逐段测量再拼 caret 表（`**粗体**`、`` `code` `` 这些真正参与排版）──
        if (!runs.empty()) {
            appendMeasuredStyledLine(out, value, start, end, hardBreakAfter, lineFontSize, lineHeight,
                                     lineColor, lineFontFamily, viewportWidth, holes, runs, detailed);
            return;
        }

        // 测量的是**投影文本**（隐藏区间已删掉）；没有隐藏区间时就是原行文本。
        const std::string visible = holes.empty()
            ? value.substr(static_cast<size_t>(start), static_cast<size_t>(end - start))
            : projectText(value, start, end, holes);
        const bool softWraps = viewportWidth > 1.0f;
        if (!detailed) {
            const float width = core::TextPrimitive::measureTextWidth(visible, lineFontFamily, lineFontSize);
            if (!softWraps || width <= viewportWidth) {
                core::TextPrimitive::TextMetrics coarse;
                coarse.width = width;
                out.push_back({start, end, hardBreakAfter, std::move(coarse), lineFontSize, lineHeight,
                               0.0f, holes, lineColor, {}});
                return;
            }
        }
        const core::TextPrimitive::TextMetrics metrics = measureMetrics(visible, lineFontFamily, lineFontSize);

        if (!softWraps || metrics.byteIndices.size() <= 2 || metrics.width <= viewportWidth) {
            out.push_back({start, end, hardBreakAfter, metrics, lineFontSize, lineHeight, 0.0f, holes, lineColor, {}});
            return;
        }

        // 溢出切段。切点在**投影文本**上算，再映射回文档偏移；
        // 没有隐藏区间时这条路径与改造前逐个字节等价。
        int segmentProjected = 0;
        float segmentStartX = 0.0f;
        int previousProjected = 0;
        int previousDocumentEnd = start;
        const size_t count = std::min(metrics.byteIndices.size(), metrics.caretX.size());
        for (size_t i = 1; i < count; ++i) {
            const int stop = metrics.byteIndices[i];
            const float x = metrics.caretX[i];
            if (previousProjected > segmentProjected && x - segmentStartX > viewportWidth) {
                const int segmentEndProjected = previousProjected;
                const int segmentDocumentBeg = std::max(previousDocumentEnd,
                    unprojectVisible(holes, start, segmentProjected));
                const int segmentDocumentEnd = std::max(segmentDocumentBeg,
                    unprojectVisible(holes, start, segmentEndProjected));
                const std::string segmentText = holes.empty()
                    ? value.substr(static_cast<size_t>(segmentDocumentBeg),
                                   static_cast<size_t>(segmentDocumentEnd - segmentDocumentBeg))
                    : projectText(value, segmentDocumentBeg, segmentDocumentEnd, holes);
                out.push_back({segmentDocumentBeg, segmentDocumentEnd, false,
                               measureMetrics(segmentText, lineFontFamily, lineFontSize),
                               lineFontSize, lineHeight, 0.0f,
                               clipHoles(holes, segmentDocumentBeg, segmentDocumentEnd), lineColor, {}});
                previousDocumentEnd = segmentDocumentEnd;
                segmentProjected = segmentEndProjected;
                segmentStartX = caretXInMetrics(metrics, segmentProjected);
            }
            previousProjected = stop;
        }
        if (segmentProjected < static_cast<int>(visible.size()) || visible.empty()) {
            const int segmentDocumentBeg = std::max(previousDocumentEnd,
                unprojectVisible(holes, start, segmentProjected));
            const std::string segmentText = holes.empty()
                ? value.substr(static_cast<size_t>(segmentDocumentBeg),
                               static_cast<size_t>(end - segmentDocumentBeg))
                : projectText(value, segmentDocumentBeg, end, holes);
            out.push_back({segmentDocumentBeg, end, hardBreakAfter,
                           measureMetrics(segmentText, lineFontFamily, lineFontSize),
                           lineFontSize, lineHeight, 0.0f,
                           clipHoles(holes, segmentDocumentBeg, end), lineColor, {}});
        }
    }

    // 带样式段的行测量。软换行的切点算法与无样式段那条路径一致，只是"语料"换成投影文本，
    // 且每一段都要带着自己的样式段重新排一遍（段的 x 要相对该段左端重新算）。
    static void appendMeasuredStyledLine(std::vector<InputLayout::Line>& out,
                                         const std::string& value,
                                         int start,
                                         int end,
                                         bool hardBreakAfter,
                                         float lineFontSize,
                                         float lineHeight,
                                         const core::Color& lineColor,
                                         const std::string& fontFamily,
                                         float viewportWidth,
                                         const std::vector<LineHole>& holes,
                                         const std::vector<LineRun>& runs,
                                         bool detailed = true) {
        const std::vector<VisiblePiece> pieces = buildVisiblePieces(holes, start, end, runs);
        const int visibleLen = visibleLength(holes, start, end);
        if (!detailed && !pieces.empty() && visibleLen > 0) {
            StyledLineLayout coarse;
            for (const auto& piece : pieces) {
                const int beg = std::max(start, unprojectVisible(holes, start, piece.visibleBeg));
                const int pieceEnd = std::max(beg, unprojectVisible(holes, start, piece.visibleEnd));
                const auto text = projectText(value, beg, pieceEnd, holes);
                const LineRunStyle style = piece.style ? *piece.style : LineRunStyle{};
                const auto& family = style.fontFamily.empty() ? fontFamily : style.fontFamily;
                const float width = core::TextPrimitive::measureTextWidth(text, family, lineFontSize,
                                                                         style.weight > 0 ? style.weight : 400);
                coarse.runs.push_back({beg, pieceEnd, coarse.metrics.width, width, style});
                coarse.metrics.width += width;
            }
            if (viewportWidth <= 1.0f || coarse.metrics.width <= viewportWidth) {
                out.push_back({start, end, hardBreakAfter, std::move(coarse.metrics), lineFontSize,
                               lineHeight, 0.0f, holes, lineColor, std::move(coarse.runs)});
                return;
            }
        }
        if (pieces.empty() || visibleLen <= 0) {
            out.push_back({start, end, hardBreakAfter, measureMetrics({}, fontFamily, lineFontSize),
                           lineFontSize, lineHeight, 0.0f, holes, lineColor, {}});
            return;
        }

        StyledLineLayout whole = layoutStyledRange(value, start, end, holes, pieces, 0, visibleLen,
                                                   lineFontSize, fontFamily);
        const bool softWraps = viewportWidth > 1.0f;
        if (!softWraps || whole.metrics.byteIndices.size() <= 2 || whole.metrics.width <= viewportWidth) {
            out.push_back({start, end, hardBreakAfter, std::move(whole.metrics), lineFontSize, lineHeight, 0.0f,
                           holes, lineColor, std::move(whole.runs)});
            return;
        }

        int segmentProjected = 0;
        float segmentStartX = 0.0f;
        int previousProjected = 0;
        int previousDocumentEnd = start;
        const size_t count = std::min(whole.metrics.byteIndices.size(), whole.metrics.caretX.size());
        for (size_t i = 1; i < count; ++i) {
            const int stop = whole.metrics.byteIndices[i];
            const float x = whole.metrics.caretX[i];
            if (previousProjected > segmentProjected && x - segmentStartX > viewportWidth) {
                const int segmentEndProjected = previousProjected;
                const int segmentDocumentBeg = std::max(previousDocumentEnd,
                    unprojectVisible(holes, start, segmentProjected));
                const int segmentDocumentEnd = std::max(segmentDocumentBeg,
                    unprojectVisible(holes, start, segmentEndProjected));
                StyledLineLayout segment = layoutStyledRange(value, start, end, holes, pieces,
                                                             segmentProjected, segmentEndProjected,
                                                             lineFontSize, fontFamily);
                out.push_back({segmentDocumentBeg, segmentDocumentEnd, false, std::move(segment.metrics),
                               lineFontSize, lineHeight, 0.0f,
                               clipHoles(holes, segmentDocumentBeg, segmentDocumentEnd),
                               lineColor, std::move(segment.runs)});
                previousDocumentEnd = segmentDocumentEnd;
                segmentProjected = segmentEndProjected;
                segmentStartX = caretXInMetrics(whole.metrics, segmentProjected);
            }
            previousProjected = stop;
        }
        if (segmentProjected < visibleLen) {
            const int segmentDocumentBeg = std::max(previousDocumentEnd,
                unprojectVisible(holes, start, segmentProjected));
            StyledLineLayout segment = layoutStyledRange(value, start, end, holes, pieces,
                                                         segmentProjected, visibleLen,
                                                         lineFontSize, fontFamily);
            out.push_back({segmentDocumentBeg, end, hardBreakAfter, std::move(segment.metrics),
                           lineFontSize, lineHeight, 0.0f,
                           clipHoles(holes, segmentDocumentBeg, end),
                           lineColor, std::move(segment.runs)});
        }
    }

    // ── 表格列计划（S3f 批次 D）──────────────────────────────────────────────
    // 列对齐的算法在 buildTableColumns（见 measureLines 之前）；结构体定义在上方。
    static std::vector<TableColumns> buildTableIntrinsicColumns(const std::string& value,
                                                       LineDecorationView decorations,
                                                       const std::string& fontFamily,
                                                       float fontSize,
                                                       int firstRow = 0, int endRow = -1,
                                                       TableColumnIndex* indexOut = nullptr) {
        std::vector<TableColumns> tables;
        TableColumnIndex index;
        if (endRow < 0) endRow = static_cast<int>(decorations.size());
        for (int row = firstRow; row < endRow; ++row) {
            const LineDecoration& decoration = decorations[static_cast<std::size_t>(row)];
            if (decoration.tableId < 0 || decoration.hidden || decoration.cells.empty()) {
                continue;
            }
            const float cellFontSize = decoration.fontSize > 0.0f ? decoration.fontSize : fontSize;
            TableColumns* table = nullptr;
            // Most rows follow another row of the same table. Avoid a hash lookup
            // for that common case, including a document with one large table.
            if (!tables.empty() && tables.back().tableId == decoration.tableId) {
                table = &tables.back();
            } else {
                const auto entry = index.try_emplace(decoration.tableId, tables.size());
                if (entry.second) {
                    tables.push_back(TableColumns{});
                    tables.back().tableId = decoration.tableId;
                    tables.back().padding = decoration.effectiveCellPadding();
                }
                table = &tables[entry.first->second];
            }
            if (table->count() < static_cast<int>(decoration.cells.size())) {
                table->width.resize(decoration.cells.size(), 0.0f);
            }
            // 该行的 holes 只在测量格内文字时用得上（格内的行内标记要投影掉）。
            const std::vector<LineHole> holes = decoration.holes.empty()
                ? std::vector<LineHole>{}
                : normalizeHoles(decoration.holes);
            for (std::size_t c = 0; c < decoration.cells.size(); ++c) {
                const LineCell& cell = decoration.cells[c];
                float width = 0.0f;
                if (cell.beg < cell.end) {
                    const std::vector<LineHole> clipped = clipHoles(holes, cell.beg, cell.end);
                    const std::string text = projectText(value, cell.beg, cell.end, clipped);
                    width = measureMetrics(text, fontFamily, cellFontSize).width;
                }
                if (width > table->width[c]) {
                    table->width[c] = width;
                }
            }
        }

        if (indexOut != nullptr) *indexOut = std::move(index);
        ++debugLayoutStats().tableIntrinsicBuilt;
        return tables;
    }

    static void constrainTableColumns(std::vector<TableColumns>& tables, float fontSize, float viewportWidth) {
        // 夹逼 + 压进可用宽度。文字左边一个 padding，列与列之间两个（右 padding + 左 padding）。
        const float minWidth = fontSize * kTableColumnMinEm;
        const float maxWidth = fontSize * kTableColumnMaxEm;
        const float floorWidth = fontSize * kTableColumnFloorEm;
        for (TableColumns& table : tables) {
            table.x.resize(table.width.size(), 0.0f);
            for (float& width : table.width) {
                width = std::clamp(width, minWidth, maxWidth);
            }
            float sum = 0.0f;
            for (const float width : table.width) {
                sum += width;
            }
            const float capacity = std::max(0.0f, viewportWidth - table.padding * 2.0f * table.count());
            if (sum > capacity && sum > 0.0f) {
                const float scale = capacity / sum;
                for (float& width : table.width) {
                    width = std::max(floorWidth, width * scale);
                }
            }
            float x = table.padding;
            for (std::size_t c = 0; c < table.x.size(); ++c) {
                table.x[c] = x;
                x += table.width[c] + table.padding * 2.0f;
            }
            table.total = table.x.empty() ? 0.0f : x - table.padding;
        }
    }

    static std::vector<TableColumns> buildTableColumns(const std::string& value,
                                                       LineDecorationView decorations,
                                                       const std::string& fontFamily, float fontSize,
                                                       float viewportWidth, int firstRow = 0, int endRow = -1,
                                                       TableColumnIndex* indexOut = nullptr) {
        auto tables = buildTableIntrinsicColumns(value, decorations, fontFamily, fontSize,
                                                 firstRow, endRow, indexOut);
        constrainTableColumns(tables, fontSize, viewportWidth);
        return tables;
    }

    static const TableColumns* findTableColumns(const std::vector<TableColumns>& tables,
                                                const TableColumnIndex& index, int tableId) {
        if (tables.size() == 1) return tables.front().tableId == tableId ? &tables.front() : nullptr;
        const auto found = index.find(tableId);
        if (found == index.end() || found->second >= tables.size()) return nullptr;
        const TableColumns& table = tables[found->second];
        return table.tableId == tableId ? &table : nullptr;
    }

    // 表格行的排版（2026-09-26 改为**单元格内换行**）：逐格排版（复用带样式段的
    // 排版），内容超过列宽时在格内折行 —— 旧版把越界部分整段硬截，长文本既看不见
    // 也选不中。同一格的第 v 段落在表格行的第 v 条可视行上；整行拆成"最高那格的
    // 段数"条可视行，行高逐段给（首段带单元格上内边距、末段带下内边距），中间段
    // 紧贴排。列吸附的 caret 表逐段给出，光标/命中/选区自动跟着列与段走（这是选
    // "列吸附"而不是"逐格画文本"的原因）。
    static void appendMeasuredTableCellLine(std::vector<InputLayout::Line>& out,
                                            const std::string& value,
                                            int start,
                                            int end,
                                            bool hardBreakAfter,
                                            float lineFontSize,
                                            float lineHeight,
                                            const core::Color& lineColor,
                                            const std::string& fontFamily,
                                            const std::vector<LineHole>& holes,
                                            const std::vector<LineRun>& runs,
                                            const TableColumns& table,
                                            const LineDecoration& decoration) {
        const int columns = std::min(table.count(), static_cast<int>(decoration.cells.size()));
        // 纯文字行盒 = 行盒总高减去上下内边距（装饰层的 textShiftY = 单侧内边距）。
        const float textLineHeight =
            std::max(1.0f, lineHeight - 2.0f * std::max(0.0f, decoration.textShiftY));
        // 每格先整段排版，再按列宽切成若干可视段。
        struct CellSegments {
            std::vector<StyledLineLayout> segs;  // 每段的排版结果（caret 相对段首）
            std::vector<int> segFrom;            // 每段起点（相对格内容投影文本）
            float cellOffset = 0.0f;             // 对齐平移量（整格共享）
            int visBeg = 0;                      // 格内容起点（相对行投影文本）
            int visEnd = 0;                      // 格内容终点
            bool empty = false;
            std::vector<LineHole> cellHoles;
        };
        std::vector<CellSegments> cells(static_cast<std::size_t>(columns));
        std::size_t maxSegments = 1;
        float rowWidth = 0.0f;
        for (int c = 0; c < columns; ++c) {
            const LineCell& cell = decoration.cells[static_cast<std::size_t>(c)];
            const float columnX = table.x[static_cast<std::size_t>(c)];
            const float columnWidth = table.width[static_cast<std::size_t>(c)];
            rowWidth = std::max(rowWidth, columnX + columnWidth);
            CellSegments& wrapped = cells[static_cast<std::size_t>(c)];
            wrapped.cellHoles = clipHoles(holes, cell.beg, cell.end);
            wrapped.visBeg = visibleLength(holes, start, cell.beg);
            wrapped.visEnd = visibleLength(holes, start, cell.end);
            if (cell.beg >= cell.end) {
                wrapped.empty = true;  // 空格子：只在第 0 段给停靠点
                continue;
            }
            const std::vector<VisiblePiece> pieces = buildVisiblePieces(holes, cell.beg, cell.end, runs);
            if (pieces.empty()) {
                wrapped.empty = true;
                continue;
            }
            const int cellVisLen = std::max(0, wrapped.visEnd - wrapped.visBeg);
            StyledLineLayout whole = layoutStyledRange(value, cell.beg, cell.end, holes, pieces,
                                                       0, cellVisLen, lineFontSize, fontFamily);
            // 对齐（S3f 批次 E）：居中/右对齐的列把格内容在列内平移。偏移按**整格**
            // 内容宽算；内容超列宽（会换行）时偏移归零、各段照常从列起点起排。
            if (cell.align == 2 || cell.align == 3) {
                const float contentWidth = std::min(whole.metrics.width, columnWidth);
                wrapped.cellOffset = cell.align == 2 ? (columnWidth - contentWidth) * 0.5f
                                                     : columnWidth - contentWidth;
            }
            // 整格超列宽 → 按 caret 停靠点切段（切法与普通行的软换行一致）。
            const std::size_t total = std::min(whole.metrics.byteIndices.size(),
                                               whole.metrics.caretX.size());
            int segmentFrom = 0;
            int previousStop = 0;
            float segmentStartX = 0.0f;
            for (std::size_t i = 1; i < total; ++i) {
                const int stop = whole.metrics.byteIndices[i];
                const float x = whole.metrics.caretX[i];
                if (previousStop > segmentFrom && x - segmentStartX > columnWidth) {
                    const int segmentTo = previousStop;
                    wrapped.segFrom.push_back(segmentFrom);
                    wrapped.segs.push_back(std::move(layoutStyledRange(
                        value, cell.beg, cell.end, holes, pieces, segmentFrom, segmentTo,
                        lineFontSize, fontFamily)));
                    segmentFrom = segmentTo;
                    segmentStartX = caretXInMetrics(whole.metrics, segmentFrom);
                }
                previousStop = stop;
            }
            if (segmentFrom < cellVisLen || wrapped.segs.empty()) {
                wrapped.segFrom.push_back(segmentFrom);
                if (segmentFrom == 0) {
                    // No wrap: the already shaped whole cell is exactly this range.
                    wrapped.segs.push_back(std::move(whole));
                    ++debugLayoutStats().tableWholeCellReused;
                } else {
                    wrapped.segs.push_back(layoutStyledRange(
                        value, cell.beg, cell.end, holes, pieces, segmentFrom, cellVisLen,
                        lineFontSize, fontFamily));
                }
            }
            maxSegments = std::max(maxSegments, wrapped.segs.size());
        }

        // 组装可视行：第 v 行收各格的第 v 段。
        for (std::size_t v = 0; v < maxSegments; ++v) {
            core::TextPrimitive::TextMetrics metrics;
            std::vector<TextRun> outRuns;
            std::vector<TableDocCaretStop> docStops;
            const bool lastSegment = v + 1 == maxSegments;
            for (int c = 0; c < columns; ++c) {
                const float columnX = table.x[static_cast<std::size_t>(c)];
                CellSegments& wrapped = cells[static_cast<std::size_t>(c)];
                if (wrapped.empty) {
                    if (v == 0) {
                        // 空格子：给一个停靠点，光标/点击能落进这一列。
                        overrideCaretStop(metrics, wrapped.visBeg, columnX);
                        docStops.push_back({decoration.cells[static_cast<std::size_t>(c)].beg,
                                            columnX, c});
                    }
                    continue;
                }
                if (v >= wrapped.segs.size()) {
                    // 这一格已经排完：这一物理续行不持有该格的 caret 停靠点；
                    // 命中空白时由列几何回退到该格末尾，文档光标仍归回真正末段。
                    overrideCaretStop(metrics, wrapped.visEnd, columnX);
                    continue;
                }
                StyledLineLayout& segment = wrapped.segs[v];
                const int segBase = wrapped.visBeg + wrapped.segFrom[v];
                const std::size_t stops = std::min(segment.metrics.byteIndices.size(),
                                                   segment.metrics.caretX.size());
                for (std::size_t i = 0; i < stops; ++i) {
                    const int cellVisibleOffset =
                        wrapped.segFrom[v] + segment.metrics.byteIndices[i];
                    overrideCaretStop(metrics,
                                      segBase + segment.metrics.byteIndices[i],
                                      columnX + wrapped.cellOffset + segment.metrics.caretX[i]);
                    docStops.push_back({unprojectVisible(
                                            wrapped.cellHoles,
                                            decoration.cells[static_cast<std::size_t>(c)].beg,
                                            cellVisibleOffset),
                                        columnX + wrapped.cellOffset + segment.metrics.caretX[i], c});
                }
                for (TextRun& run : segment.runs) {
                    if (run.beg >= run.end) {
                        continue;
                    }
                    run.x += columnX + wrapped.cellOffset;
                    outRuns.push_back(std::move(run));
                }
            }
            metrics.width = rowWidth;
            // 段高：末段带走整个行盒（含上下内边距），其余段用纯文字行盒紧贴排。
            // 只有一段时与旧版逐字节一致（行盒总高、textShiftY 由盖章循环给）。
            out.push_back({start, end, hardBreakAfter && lastSegment, std::move(metrics),
                           lineFontSize, lastSegment ? lineHeight : textLineHeight, 0.0f,
                           holes, lineColor, std::move(outRuns)});
            out.back().tableDocCaretStops = std::move(docStops);
            // 命中 clamp 用（2026-10-06）：每条可视行都带整行的格区间 —— 任一列在
            // 这条可视行上都至少有一个停靠点，点击列归属按 x 几何即可判定。
            out.back().tableCellRanges.assign(decoration.cells.begin(),
                                              decoration.cells.begin() + columns);
        }
    }

    static std::vector<InputLayout::Line> measureLines(const std::string& value,
                                                       const std::string& fontFamily,
                                                       float fontSize,
                                                       float viewportWidth,
                                                       LineDecorationView decorations = {},
                                                       std::vector<TableColumns>* tablesOut = nullptr,
                                                       TableColumnIndex* indexOut = nullptr,
                                                       const std::vector<TableColumns>* intrinsic = nullptr,
                                                       std::vector<TableColumns>* intrinsicOut = nullptr,
                                                       bool compactMetrics = false) {
        std::vector<InputLayout::Line> lines;
        const float defaultLineHeight = fontSize * 1.2f;
        // 表格列计划必须先于逐行测量算出来（同一张表的每一行都要用同一套列 x）。
        TableColumnIndex index;
        std::vector<TableColumns> tables;
        if (intrinsic != nullptr) {
            tables = *intrinsic;
            for (std::size_t i = 0; i < tables.size(); ++i) index.emplace(tables[i].tableId, i);
            ++debugLayoutStats().tableIntrinsicReused;
        } else if (decorations != nullptr) {
            tables = buildTableIntrinsicColumns(value, *decorations, fontFamily, fontSize, 0, -1, &index);
        }
        if (intrinsicOut != nullptr && intrinsicOut != intrinsic) {
            if (tables.empty()) std::vector<TableColumns>{}.swap(*intrinsicOut);
            else *intrinsicOut = tables;
        }
        constrainTableColumns(tables, fontSize, viewportWidth);
        int start = 0;
        int lineNumber = 0;
        while (start <= static_cast<int>(value.size())) {
            const size_t newline = value.find('\n', static_cast<size_t>(start));
            const int end = newline == std::string::npos
                ? static_cast<int>(value.size())
                : static_cast<int>(newline);
            const LineDecoration* decoration = nullptr;
            if (decorations != nullptr && lineNumber < static_cast<int>(decorations->size())) {
                decoration = &(*decorations)[static_cast<std::size_t>(lineNumber)];
            }
            const TableColumns* table = decoration != nullptr && decoration->tableId >= 0
                ? findTableColumns(tables, index, decoration->tableId)
                : nullptr;
            appendMeasuredLine(lines, value, start, end, newline != std::string::npos,
                               fontSize, defaultLineHeight, fontFamily, viewportWidth, decoration,
                               lineNumber + 1, table, !compactMetrics || table != nullptr);
            if (newline == std::string::npos) {
                break;
            }
            start = static_cast<int>(newline) + 1;
            ++lineNumber;
        }
        if (lines.empty()) {
            lines.push_back({0, 0, false, measureMetrics({}, fontFamily, fontSize),
                             fontSize, defaultLineHeight, 0.0f, {}});
        }
        if (tablesOut != nullptr) {
            *tablesOut = std::move(tables);
        }
        if (indexOut != nullptr) *indexOut = std::move(index);
        return lines;
    }

    static bool lineHasDocCaretStop(const InputLayout::Line& line, int byteIndex) {
        if (line.tableDocCaretStops.empty()) {
            const int clamped = std::clamp(byteIndex, line.start, line.end);
            const int visible = visibleLength(line.holes, line.start, clamped);
            const std::size_t count =
                std::min(line.metrics.byteIndices.size(), line.metrics.caretX.size());
            for (std::size_t i = 0; i < count; ++i) {
                if (line.metrics.byteIndices[i] == visible) return true;
            }
            return false;
        }
        for (const TableDocCaretStop& stop : line.tableDocCaretStops) {
            if (stop.byteIndex == byteIndex) return true;
        }
        // Decoration cell.end can include concealed closing markup. Compare in the
        // cell-local projection so that byte offsets inside that hidden suffix still
        // resolve to the visual segment owning the cell's final caret.
        for (std::size_t column = 0; column < line.tableCellRanges.size(); ++column) {
            const LineCell& cell = line.tableCellRanges[column];
            if (byteIndex < cell.beg || byteIndex > cell.end) continue;
            const auto cellHoles = clipHoles(line.holes, cell.beg, cell.end);
            const int visible = visibleLength(cellHoles, cell.beg, byteIndex);
            for (const TableDocCaretStop& stop : line.tableDocCaretStops) {
                if (stop.column == static_cast<int>(column) &&
                    visibleLength(cellHoles, cell.beg, stop.byteIndex) == visible) return true;
            }
        }
        return false;
    }

    static int lineIndexFor(const std::vector<InputLayout::Line>& lines, int byteIndex) {
        if (lines.empty()) {
            return 0;
        }
        const auto it = std::upper_bound(
            lines.begin(),
            lines.end(),
            byteIndex,
            [](int value, const InputLayout::Line& line) {
                return value < line.start;
            });
        int index = it == lines.begin()
            ? 0
            : static_cast<int>(std::distance(lines.begin(), it)) - 1;
        index = std::clamp(index, 0, static_cast<int>(lines.size()) - 1);
        if (index + 1 < static_cast<int>(lines.size()) &&
            !lines[static_cast<size_t>(index)].hardBreakAfter &&
            byteIndex >= lines[static_cast<size_t>(index)].end) {
            ++index;
        }
        while (index > 0 && !lines[static_cast<std::size_t>(index)].lineStart &&
               lines[static_cast<std::size_t>(index - 1)].start ==
                   lines[static_cast<std::size_t>(index)].start &&
               lines[static_cast<std::size_t>(index - 1)].end ==
                   lines[static_cast<std::size_t>(index)].end) {
            if (lineHasDocCaretStop(lines[static_cast<std::size_t>(index)], byteIndex)) break;
            --index;
        }
        return std::clamp(index, 0, static_cast<int>(lines.size()) - 1);
    }

    // 垂直滚动以"光标行的 top/bottom"为准（行高不再全局统一，缩放窗口那一套按行查表）。
    static void syncVerticalScrollTo(InputState& state, float cursorTop, float cursorBottom, float viewportHeight) {
        if (viewportHeight <= 0.0f) {
            state.verticalScroll = 0.0f;
            return;
        }
        if (cursorTop - state.verticalScroll < 0.0f) {
            state.verticalScroll = cursorTop;
        } else if (cursorBottom - state.verticalScroll > viewportHeight) {
            state.verticalScroll = cursorBottom - viewportHeight;
        }
        state.verticalScroll = std::max(0.0f, state.verticalScroll);
    }

    static void syncVerticalScroll(InputState& state, const InputLayout& layout, float viewportHeight) {
        const int line = layout.lineIndexFor(state.cursor);
        syncVerticalScrollTo(state,
                             layout.geometryTable().top(line),
                             layout.geometryTable().bottom(line),
                             viewportHeight);
    }

    static void syncVerticalScroll(InputState& state,
                                   const std::vector<InputLayout::Line>& lines,
                                   int cursorLine,
                                   float viewportHeight) {
        if (lines.empty()) {
            state.verticalScroll = 0.0f;
            return;
        }
        const InputLayout::Line& line =
            lines[static_cast<std::size_t>(std::clamp(cursorLine, 0, static_cast<int>(lines.size()) - 1))];
        const float height = line.lineHeight > 0.0f ? line.lineHeight : 0.0f;
        syncVerticalScrollTo(state, line.top, line.top + height, viewportHeight);
    }

    // 无隐藏区间时的光标 x（单行控件用）。多行走 caretXInLine()。
    static float caretX(const core::TextPrimitive::TextMetrics& metrics, int byteIndex) {
        return caretXInMetrics(metrics, byteIndex);
    }

    // 行内光标 x：文档偏移 → x。落在隐藏区间里会被投影吸附，看得到的光标一定落在可见文本上。
    static float caretXInLine(const InputLayout::Line& line, int docOffset) {
        for (const TableDocCaretStop& stop : line.tableDocCaretStops) {
            if (stop.byteIndex == docOffset) return stop.x;
        }
        int ownerColumn = -1;
        int closestByteDistance = std::numeric_limits<int>::max();
        for (std::size_t column = 0; column < line.tableCellRanges.size(); ++column) {
            const LineCell& cell = line.tableCellRanges[column];
            if (docOffset < cell.beg || docOffset > cell.end) continue;
            const int distance = std::min(std::abs(docOffset - cell.beg),
                                          std::abs(docOffset - cell.end));
            if (distance < closestByteDistance) {
                ownerColumn = static_cast<int>(column);
                closestByteDistance = distance;
            }
        }
        if (ownerColumn >= 0) {
            const TableDocCaretStop* nearest = nullptr;
            int nearestDistance = std::numeric_limits<int>::max();
            for (const TableDocCaretStop& stop : line.tableDocCaretStops) {
                if (stop.column != ownerColumn) continue;
                const int distance = std::abs(docOffset - stop.byteIndex);
                if (distance < nearestDistance) {
                    nearest = &stop;
                    nearestDistance = distance;
                }
            }
            if (nearest != nullptr) return nearest->x;
        }
        return caretXForDocOffset(line.metrics, line.holes, line.start, docOffset);
    }

    static int tableDocOffsetForX(const InputLayout::Line& line, float targetX,
                                  const TableColumns* columns) {
        if (line.tableDocCaretStops.empty() || columns == nullptr) return -1;
        const int count = std::min(columns->count(), static_cast<int>(line.tableCellRanges.size()));
        if (count <= 0) return -1;
        const float columnX = targetX - line.contentIndent - glyphAdvanceOf(line.glyph, line.fontSize);
        int selected = 0;
        for (int c = 1; c < count; ++c) {
            const float boundary = (columns->x[static_cast<std::size_t>(c - 1)] +
                                    columns->width[static_cast<std::size_t>(c - 1)] +
                                    columns->x[static_cast<std::size_t>(c)]) * 0.5f;
            if (columnX >= boundary) selected = c;
        }
        const TableDocCaretStop* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        for (const TableDocCaretStop& stop : line.tableDocCaretStops) {
            if (stop.column != selected) continue;
            const float distance = std::fabs(targetX - stop.x);
            if (distance < bestDistance) {
                best = &stop;
                bestDistance = distance;
            }
        }
        if (best != nullptr) return best->byteIndex;
        const LineCell& cell = line.tableCellRanges[static_cast<std::size_t>(selected)];
        return cell.beg == cell.end ? cell.beg : cell.end;
    }

    // 单行控件（无隐藏区间）的前后光标位置。
    static int previousCaretIndex(const core::TextPrimitive::TextMetrics& metrics, int byteIndex) {
        return projectedPreviousCaret(metrics, byteIndex);
    }

    static int nextCaretIndex(const core::TextPrimitive::TextMetrics& metrics, int byteIndex) {
        return projectedNextCaret(metrics, byteIndex);
    }

    static int previousCaretIndex(const std::vector<InputLayout::Line>& lines, int byteIndex) {
        if (lines.empty()) {
            return 0;
        }
        const int lineIndex = lineIndexFor(lines, byteIndex);
        const InputLayout::Line& line = lines[static_cast<size_t>(lineIndex)];
        if (line.hardBreakAfter && byteIndex == line.end + 1) {
            return line.end;
        }
        if (byteIndex > line.start) {
            if (!line.tableDocCaretStops.empty()) {
                int best = line.start;
                for (const TableDocCaretStop& stop : line.tableDocCaretStops) {
                    if (stop.byteIndex < byteIndex && stop.byteIndex > best) best = stop.byteIndex;
                }
                if (best < byteIndex) return best;
            }
            return previousVisibleCaret(line.metrics, line.holes, line.start, byteIndex);
        }
        if (lineIndex <= 0) {
            return line.start;
        }
        const InputLayout::Line& previousLine = lines[static_cast<size_t>(lineIndex - 1)];
        return previousLine.end;
    }

    static int nextCaretIndex(const std::vector<InputLayout::Line>& lines, const std::string& text, int byteIndex) {
        if (lines.empty()) {
            return 0;
        }
        const int lineIndex = lineIndexFor(lines, byteIndex);
        const InputLayout::Line& line = lines[static_cast<size_t>(lineIndex)];
        if (line.hardBreakAfter && byteIndex == line.end) {
            return std::min(static_cast<int>(text.size()), line.end + 1);
        }
        if (byteIndex < line.end) {
            if (!line.tableDocCaretStops.empty()) {
                int best = line.end;
                for (const TableDocCaretStop& stop : line.tableDocCaretStops) {
                    if (stop.byteIndex > byteIndex && stop.byteIndex < best) best = stop.byteIndex;
                }
                if (best > byteIndex) return best;
            }
            return nextVisibleCaret(line.metrics, line.holes, line.start, byteIndex);
        }
        if (lineIndex + 1 >= static_cast<int>(lines.size())) {
            return line.end;
        }
        const InputLayout::Line& nextLine = lines[static_cast<size_t>(lineIndex + 1)];
        return nextLine.start;
    }

    static int prevCursorIndex(InputState& state,
                               const std::string& fontFamily,
                               float fontSize,
                               bool multiline = false,
                               float viewportWidth = 0.0f) {
        if (multiline) {
            return clampUtf8Boundary(state.text, previousCaretIndex(cachedLines(state, fontFamily, fontSize, viewportWidth, true), state.cursor));
        }
        return clampUtf8Boundary(state.text, previousCaretIndex(cachedMetrics(state, fontFamily, fontSize, viewportWidth), state.cursor));
    }

    static int nextCursorIndex(InputState& state,
                               const std::string& fontFamily,
                               float fontSize,
                               bool multiline = false,
                               float viewportWidth = 0.0f) {
        if (multiline) {
            return clampUtf8Boundary(state.text, nextCaretIndex(cachedLines(state, fontFamily, fontSize, viewportWidth, true), state.text, state.cursor));
        }
        return clampUtf8Boundary(state.text, nextCaretIndex(cachedMetrics(state, fontFamily, fontSize, viewportWidth), state.cursor));
    }

    static void syncScroll(InputState& state, float viewportWidth, const std::string& fontFamily, float fontSize) {
        syncScroll(state, viewportWidth, cachedMetrics(state, fontFamily, fontSize, viewportWidth), fontSize);
    }

    static void syncScroll(InputState& state,
                           float viewportWidth,
                           const core::TextPrimitive::TextMetrics& metrics,
                           float fontSize) {
        const float textWidth = metrics.width;
        const float cursorPixel = caretX(metrics, state.cursor);
        if (textWidth <= viewportWidth) {
            state.horizontalScroll = 0.0f;
            return;
        }
        const float trailingPadding = std::max(6.0f, fontSize * 0.35f);
        const float rightSafe = std::max(1.0f, viewportWidth - trailingPadding);
        if (cursorPixel - state.horizontalScroll < 0.0f) {
            state.horizontalScroll = cursorPixel;
        } else if (cursorPixel - state.horizontalScroll > rightSafe) {
            state.horizontalScroll = cursorPixel - rightSafe;
        }
        state.horizontalScroll = std::clamp(state.horizontalScroll, 0.0f, std::max(0.0f, textWidth - viewportWidth + trailingPadding));
    }

    static std::string makeDirtyKey(const InputState& state, bool focused, const InputLayout& layout) {
        std::string key = focused ? "f|" : "b|";
        key += std::to_string(state.cursor);
        key += '|';
        key += std::to_string(state.selectionStart);
        key += '|';
        key += std::to_string(state.selectionEnd);
        key += '|';
        key += std::to_string(static_cast<int>(std::lround(layout.scroll * 64.0f)));
        key += '|';
        key += std::to_string(static_cast<int>(std::lround(state.verticalScroll * 64.0f)));
        key += '|';
        key += std::to_string(state.textRevision);
        key += '|';
        key += std::to_string(state.compositionRevision);
        key += "|hl" + std::to_string(state.pointerHoverLinkBeg) + ":" +
            std::to_string(state.pointerHoverLinkEnd);
        return key;
    }

};

} // namespace components::input_detail
