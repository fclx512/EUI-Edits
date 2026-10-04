#pragma once

// Live Preview 的「源码 → 逐行装饰计划」。
//
// 这一层是纯逻辑：只吃源码字符串，产出每行"是什么块 / 要隐藏哪些字节 / 行内有哪些片段"，
// **不碰任何 UI、不决定样式**（字号、颜色、间距全部留给视图层按 markdownStyle() 派生）。
// 这么切分的两个好处：
//   1) 可以脱离窗口写断言测试（见 参考/tools/lp_plan_test.cpp）；
//   2) 样式调整（用户已决定统一延后处理）不会牵动这一层。
//
// 数据来源有两条，互相印证：
//   A. 自带的行扫描器（确定性、可读）：围栏状态机、ATX 标题、引用、列表/任务、分隔线、setext。
//   B. md4c 的解析结果（3rd/md4c/src）：叶子块区间 + 每行内容起点 + 行内 span 标记区间。
//      md4c 的那两个回调是给本仓库打的补丁（纯新增），补丁不改变 md4c 任何原有输出。
//
// 逐行"容器标记"的隐藏区间以 md4c 的每行内容起点为准（它按 CommonMark 规则剥壳，
// 嵌套列表/引用/任务标记都能逐行给出）；md4c 没覆盖到的行（围栏、setext 下划线、
// 空行等）由扫描器补。两者算出不同结果时会累加 stats.markerMismatch 供排查。
//
// 注意：本层**不含"活动行"概念**——计划只依赖文本，可整体缓存；"光标所在块显示原始源码"
// 是视图层的事（渲染时跳过该行的 conceal 即可），这样光标移动不会触发重新解析。

#include <cstddef>
#include <string>
#include <vector>

namespace neo {

// 行的块类型。样式由视图层按类型映射，这里只保留语义。
enum class LpKind {
    Blank,        // 空行
    Text,         // 普通段落（含段落的续行）
    Heading,      // ATX 与 setext 标题
    ListItem,     // 有序/无序列表项
    TaskItem,     // 任务列表项（带 [ ] / [x]）
    Code,         // 围栏代码块（含围栏行本身）
    Divider,      // 分隔线 ---/***
    Table,        // 表格行
    Frontmatter,  // 文档开头的 YAML 头（--- 起 --- 止），v1 只标记类型不做特殊渲染
};

// 源码字节区间，半开 [beg, end)。
struct LpRange {
    int beg = 0;
    int end = 0;

    bool empty() const { return beg >= end; }
    int size() const { return end > beg ? end - beg : 0; }
    // 逐字段相等（T5 的局部/全量 oracle 要整表比区间向量）。
    bool operator==(const LpRange& other) const { return beg == other.beg && end == other.end; }
};

enum class LpSpanKind {
    Emphasis,       // *斜体*  _斜体_
    Strong,         // **粗体**
    InlineCode,     // `code`
    Link,           // [文字](url)  <自动链接>
    Image,          // ![alt](src)
    Strikethrough,  // ~~删除~~
    Math,           // $x$  $$x$$
    WikiLink,       // [[目标]]
    Underline,      // <u>下划线</u>（md4c 的 underline 扩展）
};

// 一个行内片段。content 是"去掉标记后剩下的内容"，两个 mark 是标记自身的区间
// （隐藏的就是它们）。链接的 closeMark 覆盖 "](url)" 这一整段，与 Obsidian 一致。
struct LpSpan {
    LpSpanKind kind = LpSpanKind::Emphasis;
    LpRange content;
    LpRange openMark;
    LpRange closeMark;
};

// 一行。srcBeg/srcEnd 是**不含换行符**的行区间（与窗口 text_file 的内存表示一致：
// 换行已在读入时归一化为 '\n'）。
struct LpLine {
    int srcBeg = 0;
    int srcEnd = 0;
    int number = 0;  // 0-based 行号

    LpKind kind = LpKind::Text;

    // Heading
    int headingLevel = 0;  // 1..6
    bool setext = false;   // true = 由下面的 ===/--- 下划线构成的标题

    // 容器
    int quoteDepth = 0;   // "> " 的层数（0 = 不在引用里）
    int listDepth = 0;    // 列表嵌套层数（0 = 不在列表里）
    bool ordered = false;
    unsigned orderedNumber = 0;
    bool task = false;
    bool taskChecked = false;

    // ── 列表呈现（C1）────────────────────────────────────────────────────
    // 所属列表项（带标记那一行）的**行号**：标记行记自己，物理续行（缩进续段 /
    // lazy 续段）记 owner。不在任何列表项里为 -1（空行 / 代码块 / 标题等恒 -1）。
    // 装饰层据此给续行补正文缩进（续行与标记行共享同一正文起点）。
    int listItemLine = -1;
    // 有序列表项的**呈现序号**：= 组内第一项的字面数字 + 组内偏移（`3. a` 后跟
    // `1. b` 呈现 3、4 —— 后项的字面数字不参与呈现）。无序列表恒 0。
    // 注意它不是源码字面数字 —— 那个仍是上面的 orderedNumber。
    unsigned listOrdinal = 0;
    // 本行正文起点（容器标记之后的第一个字节，扫描器口径、未经 md4c 的起点校正）。
    // 只在 Pass A 跑过容器扫描的行上有值（空行 / 围栏 / 分隔线 / frontmatter
    // / setext 下划线行保持 -1）。装饰层用它推列表项的正文缩进列。
    int contentBeg = -1;
    // 任务状态字符（' ' / 'x' / 'X'）的**绝对**字节偏移；非任务行为 -1。
    // "点复选框切换勾选"（S3f 批次 A）靠它直接定位要改的那个字节，不重新猜前缀。
    int taskStateByte = -1;

    // Code
    bool codeFence = false;      // 该行本身是 ``` 围栏
    std::string codeLang;        // 仅围栏首行有值
    int codeBlockBeg = -1;       // 所属代码块的整体区间（含两侧围栏），仅块内行有值
    int codeBlockEnd = -1;

    // 所属叶子块的区间（用于画整块底纹/边框；Blank 与未归属的行保持 -1）
    int blockBeg = -1;
    int blockEnd = -1;
    bool blockFirstLine = false;
    bool blockLastLine = false;

    // 要隐藏的标记区间：升序、互不重叠、都落在本行内。
    std::vector<LpRange> conceal;
    // 行内片段（升序）。
    std::vector<LpSpan> spans;

    // 纯图行（S3f 批次 B）：非空 = 本行只有这一个图片 span 且前后只有空白，
    // 值为 src 原文（未做路径解析 —— 那要碰文件系统，属于装饰层）。
    // 判定口径对齐 ZCode 的 `^\s*!\[...\]\(...\)\s*$`（S3f 规划 §8.2）。
    std::string pureImageSrc;

    // ── 章节表（S3f 批次 C：折叠）──────────────────────────────────────────
    // 每行"最近的上方标题"的**行号**（含自身所在标题；setext 下划线行归到配对文本行；
    // 标题行记的是它的父标题 —— 栈在压入自身之前取的顶）。-1 = 上方无标题。
    int sectionHeadingLine = -1;
    // 章节终点（不含）= 下一个同级或更高级标题；只在标题起始行上有值，-1 = 未知/非标题。
    // setext 的下划线行不记（它属于标题自己）。
    int sectionEndLine = -1;
    // 标题链上的父标题行号（只对标题起始行有意义）：折叠的祖先链要靠它往上走。
    int parentHeadingLine = -1;

    // ── 表格（S3f 批次 D：列对齐）──────────────────────────────────────────
    // 本行每个单元格的**内容区间**（已由 md4c 裁掉管道与两侧空白；beg == end 表示空
    // 单元格，仍占一列）。从左到右；非表格行恒空。
    // 注意：单元格内容里的行内标记（`**a**`）**没有**被裁掉，它们的隐藏区间仍在
    // conceal 里，装饰层照旧能按 span 上样式。
    std::vector<LpRange> cells;
    // 每格的对齐方式（与 cells 一一对应；分隔行无 cells 恒空）。取值 = md4c 的
    // MD_ALIGN：0 default/1 left/2 center/3 right，来自分隔行 `:---:` 的冒号位置。
    // align 信息的通路不经过补丁：md_process_table_cell 在 block_source 报出 cell 区间
    // 后，紧接着以 MD_BLOCK_TD_DETAIL 调 enter_block —— 本层用顺序配对把它接住。
    std::vector<unsigned char> cellAligns;
    // 所属表格的标识：同一张表的所有行相同（取 TABLE 块的起始字节偏移）。非表格行 -1。
    int tableId = -1;
    // 表头行（各行由 md4c 报 TH）——装饰层据此给底色。
    bool tableHeaderRow = false;
    // 分隔行（`|---|---|`）：md4c 不为它报 cell（cells 为空），整行内容要隐藏，
    // 装饰层把它当"表头下边框"那一行画（规划 §8.1 的 th border-b）。
    bool tableSeparator = false;
};

struct LpPlanStats {
    int lines = 0;
    int linesWithConceal = 0;
    int spanCount = 0;
    int concealRanges = 0;
    int fencedBlocks = 0;
    // 诊断计数：md4c 与自带扫描器对"该行内容起点"的判断不一致的次数（正常应为 0 或极少）。
    int markerMismatch = 0;
    // 同一区间被报了两遍（如 ***粗斜*** 会同时进 EM 与 STRONG）——正常现象。
    int duplicateRanges = 0;
    // 相邻或部分重叠而被合并的区间（如 `` `code` `` 与紧随其后的 `](url)` 首尾相接）——正常现象。
    int mergedRanges = 0;
    // 内部自检失败次数（区间越界，已丢弃）。> 0 说明计划不可信，视图层应整体放弃装饰。
    int invalidRanges = 0;
    // 本次是否使用了 md4c（0 = 未编译进 md4c，只有块级信息，没有行内片段）。
    bool usedMd4c = false;

    // ── T5（局部重解析）的文档级风险标记：全量 build 时顺带扫出 ──────────────
    // 局部路径拿它们当"能不能只重解析局部"的必要条件；为真一律回退全量。
    //
    // linkRefDef：文本里出现 "]:" —— CommonMark 链接引用定义 `[label]:` 的
    //   **必要**子串（`]` 与 `:` 必须相邻）。为真说明可能存在引用定义，md4c 的
    //   引用表是**跨块**的内联状态（定义能被它之前的引用用到），切片解析无法证明
    //   与全量一致 → 全量。
    // htmlBlockRisk：行首（≤3 空格）出现能**跨空行**的 HTML 块起始
    //   （`<!--`、`<!`、`<?`、`<script`、`<pre`、`<style`、`<textarea`，
    //   大小写不敏感）—— 这类块吞空行，md4c 的 verbatim 叶子块会越过局部边界 → 全量。
    //   （其余 HTML 块类型在空行处结束，被"局部两侧必须是空行"这条判据天然挡住。）
    bool linkRefDef = false;
    bool htmlBlockRisk = false;
    // 本计划由局部重解析产出。诊断口径提醒：markerMismatch / duplicateRanges /
    // mergedRanges 只统计被重算的局部范围，不是全篇诊断数（行字段仍是全篇的）。
    bool partial = false;
};

// ── T5：局部重解析的最小差分结构 ───────────────────────────────────────────
// 与 components::input_detail::PendingTextEdit 逐字段等价（lp_plan 不依赖组件层，
// 由 lp_decorations 转一道）。语义照抄 components/input_model.h 的约定：
//   firstLine    = byteBeg 所在行（编辑前后前缀 [0, byteBeg) 逐字节相同）；
//   newLastLine  = 新文本里最后一个受影响的行（含）；
//   oldTailLine  = 旧文本里第一个仍可整行复用的行（含）。
// 于是 新行 [0, firstLine) ← 旧行 [0, firstLine)、新行 [firstLine, newLastLine] 重算、
// 新行 [newLastLine+1, …) ← 旧行 [oldTailLine, …) 平移 deltaBytes / deltaLines。
struct LpTextEdit {
    bool valid = false;
    unsigned long long revision = 0;
    int byteBeg = 0;
    int oldEnd = 0;
    int newEnd = 0;
    int firstLine = 0;
    int newLastLine = 0;
    int oldTailLine = 0;
    // 编辑前光标（旧坐标）。局部重建计划不需要它，但转换 PendingTextEdit 时
    // 一并带过来，免得上层再维护两套结构。
    int cursorBefore = 0;
};

struct LpPlan {
    std::vector<LpLine> lines;
    LpPlanStats stats;

    const LpLine* lineAt(int index) const {
        return index >= 0 && index < static_cast<int>(lines.size()) ? &lines[static_cast<std::size_t>(index)]
                                                                    : nullptr;
    }
    // 二分：找出包含 byteIndex 的行（byteIndex 落在行尾换行符上时归入该行）。
    int lineIndexFor(int byteIndex) const;

    // 任务复选框点击判定：byteIndex 所在行是任务行 → 返回状态字符偏移，否则 -1。
    int taskStateByteFor(int byteIndex) const;

    // 折叠目标（S3f 批次 C）：光标应折叠/展开哪个标题 —— 光标在标题行上取它自己
    // （含 setext 配对），否则取"包住它的最近标题"。返回标题起始字节偏移，无标题 -1。
    // 折叠状态就按这个偏移 keyed（决策⑦：章节内编辑会漂，v1 接受）。
    int foldHeadingBegFor(int byteIndex) const;
};

// 解析整篇文本。text 必须已归一化换行（只有 '\n'）。
LpPlan buildLpPlan(const std::string& text);

// ── T5：局部重解析 ────────────────────────────────────────────────────────
// 单点编辑时**不重跑整篇 md_parse**：只对"编辑影响到的那一段（局部）"跑 md4c，
// 计划的其余部分按 prevPlan 平移复用；Pass A、frontmatter、章节表两遍、块分组
// 仍旧对**全篇**重跑（它们是 O(N) 的纯扫描，且重跑本身就是"局部是否安全"的可执行
// 证明 —— 区间外每一行的 Pass A 字段必须与上一代逐字段相等，否则当场回退）。
//
// 成功：写入 out 并返回 true，out 与 buildLpPlan(text) 逐字段等价（见实现里
//        "证明边界"的注释与 tests/unit/lp_decorations.cpp 的逐字段 oracle）。
// 失败：返回 false（同时把原因写进 planRejectReason()），**调用方必须回退全量**
//        buildLpPlan(text)。任何一条判据不满足、任何一处无法证明都算失败 ——
//        宁可全量，绝不产出一份"看起来差不多"的计划。
//
// 入参契约：
//   prevText / prevPlan 必须是编辑**作用前**那一版文本及其完整计划
//   （实现会用前缀/后缀逐字节相等把这条钉死，对不上就失败）；
//   edit 描述 prevText → text 的这一次改动。
// 局部结果若与全量不一致，只能靠上面的判据提前挡住（Debug 里可用环境变量
// NEO_LP_PLAN_VERIFY=1 让局部路径自己再跑一次全量逐字段核验 —— 那是诊断，
// 不是正确性的前提；Release 不设该变量时零开销）。
bool buildLpPlanPartial(const std::string& text,
                        const std::string& prevText,
                        const LpPlan& prevPlan,
                        const LpTextEdit& edit,
                        LpPlan& out,
                        std::string* rejectReason = nullptr);

// ── 计划构建的诊断计数（只给测试/诊断读，不属于公开 API）──────────────────
// full    = 走了整篇 buildLpPlan（含"局部尝试失败后的回退"那一次）
// partial = 局部重解析命中
// fallback= 局部尝试被判据拒掉的次数（原因见 planRejectReason()）
struct LpPlanDebugStats {
    unsigned long long full = 0;
    unsigned long long partial = 0;
    unsigned long long fallback = 0;
    // 诊断核验（NEO_LP_PLAN_VERIFY=1）发现局部≠全量的次数。恒为 0 才说明
    // 逐字段 oracle 在真实文档上没有漏网之鱼。
    unsigned long long verifyMismatch = 0;
};

inline LpPlanDebugStats& planDebugStats() {
    static LpPlanDebugStats stats;
    return stats;
}

// 最近一次局部尝试被拒的原因（诊断用）。
inline std::string& planRejectReason() {
    static std::string reason;
    return reason;
}

}  // namespace neo
