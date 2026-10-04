#include "model/lp_plan.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(EUI_HAS_MD4C)
#include <md4c.h>
#endif

namespace neo {
namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

inline bool isSpaceByte(char ch) { return ch == ' ' || ch == '\t'; }

// 制表符按 4 列算（CommonMark 的规则其实按 4 列制表位展开，这里够用）。
inline int indentWidth(const std::string& text, int beg, int end) {
    int width = 0;
    for (int i = beg; i < end; ++i) {
        const char ch = text[static_cast<std::size_t>(i)];
        if (ch == ' ') {
            ++width;
        } else if (ch == '\t') {
            width += 4;
        } else {
            break;
        }
    }
    return width;
}

inline int skipSpaces(const std::string& text, int beg, int end) {
    int p = beg;
    while (p < end && isSpaceByte(text[static_cast<std::size_t>(p)])) {
        ++p;
    }
    return p;
}

inline int skipTrailingSpaces(const std::string& text, int beg, int end) {
    int p = end;
    while (p > beg && isSpaceByte(text[static_cast<std::size_t>(p - 1)])) {
        --p;
    }
    return p;
}

inline bool rangeTextIs(const std::string& text, int beg, int end, const char* chars) {
    if (beg > end || end > static_cast<int>(text.size())) {
        return false;
    }
    for (int i = beg; i < end; ++i) {
        bool found = false;
        for (const char* c = chars; *c != '\0'; ++c) {
            if (text[static_cast<std::size_t>(i)] == *c) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// 一段区间是否全是同一个字符（用于判断 === / --- / *** 这种整行标记）。
inline bool rangeIsRunOf(const std::string& text, int beg, int end, char ch) {
    if (beg >= end) {
        return false;
    }
    for (int i = beg; i < end; ++i) {
        if (text[static_cast<std::size_t>(i)] != ch) {
            return false;
        }
    }
    return true;
}

// CommonMark 的 hr：≥3 个 - _ * ，之间可以有空格，且不能混用字符。
inline bool isThematicBreak(const std::string& text, int beg, int end) {
    int count = 0;
    char mark = '\0';
    for (int i = beg; i < end; ++i) {
        const char ch = text[static_cast<std::size_t>(i)];
        if (ch == ' ' || ch == '\t') {
            continue;
        }
        if (ch != '-' && ch != '_' && ch != '*') {
            return false;
        }
        if (mark == '\0') {
            mark = ch;
        } else if (mark != ch) {
            return false;
        }
        ++count;
    }
    return count >= 3;
}

// 引用/列表标记：识别成功则返回内容起点，失败返回 -1。
// depthOut/quoteOut 累加层数。列表嵌套深度用 listStack 维护。
struct ContainerResult {
    int content = -1;
    int quoteDepth = 0;
    int listDepth = 0;
    bool ordered = false;
    unsigned orderedNumber = 0;
    // 呈现序号（C1）：有序组 = 组起点字面数字 + 组内已见项数；无序恒 0。
    unsigned listOrdinal = 0;
    bool task = false;
    bool taskChecked = false;
    int taskStateByte = -1;
    bool anyListMarker = false;
    // 本行内容（剥掉引用前缀之后）的缩进宽度。与 ListGroup::indent /
    // contentColumn 同一坐标基准 —— 用调用侧的 indentWidth(行首) 会被 "> " 前缀
    // 带偏（">   cont" 的缩进应当算 2，而不是 0）。
    int lineIndent = 0;
};

// 一个列表组（C1）：同一嵌套层里"连续的同种列表"。组由第一个项的标记决定
// 类型与起点，后续项的呈现序号 = 起点字面数字 + 组内偏移（字面数字不参与呈现）。
// CommonMark 语义：同层换标记字符 / 有序⇄无序 / 换分隔符都开新组；空行不断组
// （宽松列表）；更浅的非空行才弹栈。
struct ListGroup {
    int indent = 0;         // 标记字符的缩进列
    bool ordered = false;
    char marker = 0;        // 无序 = '-'/'+'/'*'；有序 = 分隔符 '.'/')'
    unsigned startNumber = 0;  // 组内第一项的字面数字（仅有序）
    unsigned count = 0;        // 组内已见项数
    int contentColumn = 0;     // 最近一项的正文缩进列（续行归属判据）
    int lastItemLine = -1;     // 最近一项标记行的行号（续行的 owner）
};

ContainerResult scanContainers(const std::string& text,
                               int beg,
                               int end,
                               int lineNumber,
                               std::vector<ListGroup>& listStack) {
    ContainerResult result;
    int p = beg;
    for (int guard = 0; guard < 32; ++guard) {
        const int before = p;
        const int indented = indentWidth(text, p, end);
        int q = p + (indented < 4 ? 0 : 0);
        // 在允许的缩进内跳过空格（引用标记最多前导 3 个空格）
        int ws = p;
        int width = 0;
        while (ws < end && isSpaceByte(text[static_cast<std::size_t>(ws)]) && width < 4) {
            width += text[static_cast<std::size_t>(ws)] == '\t' ? 4 : 1;
            ++ws;
        }

        // 引用标记 ">"
        if (ws < end && text[static_cast<std::size_t>(ws)] == '>') {
            ++result.quoteDepth;
            ++ws;
            if (ws < end && text[static_cast<std::size_t>(ws)] == ' ') {
                ++ws;
            }
            p = ws;
            if (p == before) {
                break;
            }
            continue;
        }

        // 列表标记
        const int markerIndent = width;
        int markerEnd = -1;
        bool ordered = false;
        unsigned orderedNumber = 0;
        if (ws < end) {
            const char ch = text[static_cast<std::size_t>(ws)];
            if ((ch == '-' || ch == '+' || ch == '*') && ws + 1 < end &&
                isSpaceByte(text[static_cast<std::size_t>(ws + 1)])) {
                markerEnd = ws + 1;
            } else if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
                int digits = ws;
                while (digits < end && digits - ws < 9 &&
                       std::isdigit(static_cast<unsigned char>(text[static_cast<std::size_t>(digits)])) != 0) {
                    ++digits;
                }
                if (digits < end && (text[static_cast<std::size_t>(digits)] == '.' ||
                                     text[static_cast<std::size_t>(digits)] == ')') &&
                    digits + 1 < end && isSpaceByte(text[static_cast<std::size_t>(digits + 1)])) {
                    ordered = true;
                    orderedNumber = static_cast<unsigned>(std::stoul(
                        text.substr(static_cast<std::size_t>(ws), static_cast<std::size_t>(digits - ws))));
                    markerEnd = digits + 1;
                }
            }
        }

        if (markerEnd >= 0) {
            // 列表嵌套深度：按标记缩进维护一个栈；同层换标记（无序换子弹字符 /
            // 有序⇄无序 / 换分隔符）开新组（C1 的组与呈现序号都从这条规则出发）
            const char groupMarker = ordered ? text[static_cast<std::size_t>(markerEnd - 1)]
                                             : text[static_cast<std::size_t>(ws)];
            while (!listStack.empty() && markerIndent < listStack.back().indent) {
                listStack.pop_back();
            }
            if (listStack.empty() || markerIndent > listStack.back().indent) {
                listStack.push_back({markerIndent, ordered, groupMarker, orderedNumber, 0, 0, -1});
            } else if (listStack.back().ordered != ordered ||
                       listStack.back().marker != groupMarker) {
                listStack.back() = ListGroup{markerIndent, ordered, groupMarker, orderedNumber, 0, 0, -1};
            }
            ListGroup& group = listStack.back();
            result.listDepth = static_cast<int>(listStack.size());
            result.ordered = ordered;
            result.orderedNumber = orderedNumber;
            result.listOrdinal = ordered ? group.startNumber + group.count : 0;
            ++group.count;
            result.anyListMarker = true;

            int after = markerEnd;
            while (after < end && isSpaceByte(text[static_cast<std::size_t>(after)]) &&
                   after - markerEnd < 4) {
                ++after;
            }
            // 组内的"当前项"信息随每个标记行刷新：正文列（续行归属判据）与
            // 标记行号（续行的 owner）。正文列用容器相对坐标（after - p，p 是本轮
            // 容器内容起点）—— 与 markerIndent / lineIndent 同一基准，否则带引用
            // 前缀的列表（"> 1. x"）会因坐标混用而判错续行。
            group.contentColumn = after - p;
            group.lastItemLine = lineNumber;
            // 任务标记 [ ] / [x] / [X]
            if (!ordered && after + 2 < end && text[static_cast<std::size_t>(after)] == '[' &&
                (text[static_cast<std::size_t>(after + 2)] == ']')) {
                const char state = text[static_cast<std::size_t>(after + 1)];
                if (state == ' ' || state == 'x' || state == 'X') {
                    result.task = true;
                    result.taskChecked = state == 'x' || state == 'X';
                    // after 指向 '['（文档绝对偏移），状态字符就是它后面那一字节。
                    result.taskStateByte = after + 1;
                    after += 3;
                    if (after < end && isSpaceByte(text[static_cast<std::size_t>(after)])) {
                        ++after;
                    }
                }
            }
            p = after;
            // 列表项内部还可能嵌引用/子列表，继续循环
            if (p == before) {
                break;
            }
            continue;
        }

        result.lineIndent = width;  // 剥掉引用前缀后的内容缩进（容器相对坐标）
        break;
    }
    result.content = p;
    return result;
}

// 剥掉引用前缀后，内容之前的缩进宽度（容器相对坐标，与 scanContainers 的
// ContainerResult::lineIndent 同口径）。只读，不改任何扫描状态。
inline int containerIndentWidth(const std::string& text, int beg, int end) {
    int p = beg;
    for (int guard = 0; guard < 32; ++guard) {
        int ws = p;
        int width = 0;
        while (ws < end && isSpaceByte(text[static_cast<std::size_t>(ws)]) && width < 4) {
            width += text[static_cast<std::size_t>(ws)] == '\t' ? 4 : 1;
            ++ws;
        }
        if (ws < end && text[static_cast<std::size_t>(ws)] == '>') {
            ++ws;
            if (ws < end && text[static_cast<std::size_t>(ws)] == ' ') {
                ++ws;
            }
            if (ws == p) {
                break;
            }
            p = ws;
            continue;
        }
        return width;
    }
    return 0;
}

// 块级"终止符"（标题 / 围栏 / 分隔线 / frontmatter）结束列表上下文：它之后的列表
// 必须重新起组计数。缩进已经到达项正文列的行属于列表项内部（项内标题），不终止。
//
// 不做这一步的后果是实测过的：`### 标题` 之后重新从 1 起的列表会被渲染成 4. ——
// 顶格列表的标记列是 0，而围栏 / 标题 / 分隔线这些分支在分类阶段就 continue 了，
// 老代码"缩进比标记列更浅才弹栈"的判据对它们永远不成立。
inline void terminateListContext(std::vector<ListGroup>& listStack, int width) {
    while (!listStack.empty() && width < listStack.back().contentColumn) {
        listStack.pop_back();
    }
}

// 该行是否像表格行：第一个非空字符是 '|'。
inline bool looksLikeTableRow(const std::string& text, int beg, int end) {
    const int p = skipSpaces(text, beg, end);
    return p < end && text[static_cast<std::size_t>(p)] == '|';
}

// 围栏：返回 ≥3 的反引号/波浪线数量（0 = 不是围栏）。
inline int fenceRun(const std::string& text, int beg, int end, char* charOut) {
    const int p = skipSpaces(text, beg, end);
    if (p >= end) {
        return 0;
    }
    const char ch = text[static_cast<std::size_t>(p)];
    if (ch != '`' && ch != '~') {
        return 0;
    }
    int count = 0;
    int i = p;
    while (i < end && text[static_cast<std::size_t>(i)] == ch) {
        ++count;
        ++i;
    }
    if (count < 3) {
        return 0;
    }
    if (charOut != nullptr) {
        *charOut = ch;
    }
    return count;
}

// 闭围栏：≥ 开围栏长度，且后面只有空白。
inline bool isClosingFence(const std::string& text, int beg, int end, char ch, int minCount) {
    char found = '\0';
    const int count = fenceRun(text, beg, end, &found);
    if (count < minCount || found != ch) {
        return false;
    }
    int i = skipSpaces(text, beg, end);
    while (i < end && text[static_cast<std::size_t>(i)] == ch) {
        ++i;
    }
    return skipSpaces(text, i, end) >= end;
}

// ---------------------------------------------------------------------------
// 行索引（与 InputModel::measureLines 的行切分保持一致：以 '\n' 结尾的文本
// 会在末尾多出一个空行）
// ---------------------------------------------------------------------------

struct RawLine {
    int beg = 0;
    int end = 0;
};

std::vector<RawLine> indexRawLines(const std::string& text) {
    std::vector<RawLine> lines;
    int start = 0;
    const int size = static_cast<int>(text.size());
    while (start <= size) {
        const std::size_t newline = text.find('\n', static_cast<std::size_t>(start));
        const int end = newline == std::string::npos ? size : static_cast<int>(newline);
        lines.push_back({start, end});
        if (newline == std::string::npos) {
            break;
        }
        start = static_cast<int>(newline) + 1;
    }
    if (lines.empty()) {
        lines.push_back({0, 0});
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Pass B：md4c 给出的叶子块与行内 span
// ---------------------------------------------------------------------------

struct MdBlockLine {
    int beg = 0;
    int end = 0;
};

struct MdBlock {
    int type = 0;
    int beg = 0;
    int end = 0;
    bool verbatim = false;
    std::vector<MdBlockLine> lines;
    // 表格 cell（TH/TD）的对齐方式（MD_ALIGN 值）。md_process_table_cell 在以
    // block_source 报出 cell 区间之后，紧接着以 MD_BLOCK_TD_DETAIL 调 enter_block；
    // 区间回调本身不带 detail，所以靠 enter_block 的顺序配对补上（见 MdState）。
    unsigned char align = 0;
};

struct MdSpanEvent {
    LpSpanKind kind = LpSpanKind::Emphasis;
    int beg = 0;
    int end = 0;
    bool enter = false;
};

#if defined(EUI_HAS_MD4C)
LpSpanKind spanKindFromMd(int type) {
    switch (static_cast<MD_SPANTYPE>(type)) {
        case MD_SPAN_EM: return LpSpanKind::Emphasis;
        case MD_SPAN_STRONG: return LpSpanKind::Strong;
        case MD_SPAN_A: return LpSpanKind::Link;
        case MD_SPAN_IMG: return LpSpanKind::Image;
        case MD_SPAN_CODE: return LpSpanKind::InlineCode;
        case MD_SPAN_DEL: return LpSpanKind::Strikethrough;
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY: return LpSpanKind::Math;
        case MD_SPAN_WIKILINK: return LpSpanKind::WikiLink;
        case MD_SPAN_U: return LpSpanKind::Underline;
        default: return LpSpanKind::Emphasis;
    }
}

struct MdParseResult {
    std::vector<MdBlock> blocks;
    std::vector<MdSpanEvent> spans;
};

struct MdState {
    MdParseResult* out = nullptr;
    // 刚被 block_source 报出、还差 align 的 TH/TD 块下标（-1 = 没有）。
    // cell 的事件顺序固定是 block_source → enter_block(TH/TD, &det)，一一配对。
    int pendingCellBlock = -1;
};

void mdOnBlockSource(MD_BLOCKTYPE type, MD_OFFSET beg, MD_OFFSET end, int nLines,
                     const MD_SOURCE_LINE* lines, int verbatim, void* userdata) {
    auto* state = static_cast<MdState*>(userdata);
    MdBlock block;
    block.type = static_cast<int>(type);
    block.beg = static_cast<int>(beg);
    block.end = static_cast<int>(end);
    block.verbatim = verbatim != 0;
    if (block.type == static_cast<int>(MD_BLOCK_TH) ||
        block.type == static_cast<int>(MD_BLOCK_TD)) {
        state->pendingCellBlock = static_cast<int>(state->out->blocks.size());
    }
    if (lines != nullptr) {
        const MD_OFFSET* raw = reinterpret_cast<const MD_OFFSET*>(lines);
        const int stride = block.verbatim ? 3 : 2;  // MD_VERBATIMLINE(beg,end,indent) vs MD_LINE(beg,end)
        for (int i = 0; i < nLines; ++i) {
            block.lines.push_back({static_cast<int>(raw[i * stride]), static_cast<int>(raw[i * stride + 1])});
        }
    }
    state->out->blocks.push_back(std::move(block));
}

void mdOnSpanSource(MD_SPANTYPE type, MD_OFFSET beg, MD_OFFSET end, int enter, void* userdata) {
    auto* state = static_cast<MdState*>(userdata);
    state->out->spans.push_back({spanKindFromMd(static_cast<int>(type)), static_cast<int>(beg),
                                 static_cast<int>(end), enter != 0});
}

// enter_block 只为一件实事：接住 TH/TD 的 MD_BLOCK_TD_DETAIL.align（S3f 批次 E，
// `:-:` 对齐渲染）。其余事件照旧 no-op —— 本层不依赖 md4c 的块树。
int mdOnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* state = static_cast<MdState*>(userdata);
    if ((type == MD_BLOCK_TH || type == MD_BLOCK_TD) &&
        state->pendingCellBlock >= 0 &&
        state->pendingCellBlock < static_cast<int>(state->out->blocks.size())) {
        auto* tdDetail = static_cast<MD_BLOCK_TD_DETAIL*>(detail);
        state->out->blocks[static_cast<std::size_t>(state->pendingCellBlock)].align =
            tdDetail != nullptr ? static_cast<unsigned char>(tdDetail->align) : 0;
    }
    state->pendingCellBlock = -1;
    return 0;
}
int mdOnLeaveBlock(MD_BLOCKTYPE, void*, void*) { return 0; }
int mdOnEnterSpan(MD_SPANTYPE, void*, void*) { return 0; }
int mdOnLeaveSpan(MD_SPANTYPE, void*, void*) { return 0; }
int mdOnText(MD_TEXTTYPE, const MD_CHAR*, MD_SIZE, void*) { return 0; }
void mdOnLog(const char*, void*) {}

bool parseWithMd4c(const std::string& text, MdParseResult* out) {
    MD_PARSER parser;
    std::memset(&parser, 0, sizeof(parser));
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS | MD_FLAG_WIKILINKS |
                   MD_FLAG_UNDERLINE | MD_FLAG_PERMISSIVEATXHEADERS;
    parser.enter_block = mdOnEnterBlock;
    parser.leave_block = mdOnLeaveBlock;
    parser.enter_span = mdOnEnterSpan;
    parser.leave_span = mdOnLeaveSpan;
    parser.text = mdOnText;
    parser.debug_log = mdOnLog;
    parser.block_source = mdOnBlockSource;
    parser.span_source = mdOnSpanSource;

    MdState state;
    state.out = out;
    const int rc = md_parse(text.c_str(), static_cast<MD_SIZE>(text.size()), &parser, &state);
    return rc == 0;
}
#endif  // EUI_HAS_MD4C

// 把收集到的 span 事件按 enter/leave 配成 span。标记区间为空的（宽松自动链接）直接丢弃。
// text = 整篇原文（偏移已是全文坐标），只用来读 span 自己的首字节做下面那条 WikiLink 校正。
std::vector<LpSpan> pairSpans(const std::string& text, const std::vector<MdSpanEvent>& events) {
    struct Open {
        LpSpanKind kind = LpSpanKind::Emphasis;
        int beg = 0;
        int end = 0;
    };
    std::vector<Open> stack;
    std::vector<LpSpan> spans;
    for (const MdSpanEvent& event : events) {
        if (event.enter) {
            stack.push_back({event.kind, event.beg, event.end});
            continue;
        }
        // 从栈顶往下找同类型的开标记（跨类型嵌套时以最近的同类型为准）
        std::size_t found = stack.size();
        for (std::size_t i = stack.size(); i > 0; --i) {
            if (stack[i - 1].kind == event.kind) {
                found = i - 1;
                break;
            }
        }
        if (found == stack.size()) {
            continue;
        }
        const Open open = stack[found];
        stack.resize(found);
        const LpRange openMark{open.beg, open.end};
        const LpRange closeMark{event.beg, event.end};
        if (openMark.empty() && closeMark.empty()) {
            continue;  // 宽松自动链接：没有可隐藏的标记
        }
        LpSpan span;
        span.kind = open.kind;
        span.openMark = openMark;
        span.closeMark = closeMark;
        span.content = {openMark.empty() ? open.beg : openMark.end,
                        closeMark.empty() ? event.beg : closeMark.beg};
        if (span.content.beg > span.content.end) {
            span.content.end = span.content.beg;
        }
        // T22：WikiLink 别名分隔符。md4c 对 `[[目标|别名]]` 报的 openMark 只到 `[[目标`
        // （`opener->end = delim->beg`），首个 `|` 落在 content 头上 —— 可管道既不进 holes
        // （投影多露一个 `|`）又被 accent 染色。把**首个**管道并进 openMark：
        // openMark.end = content.beg+1、content.beg++。只吸收首个：`[[foo|]]`（content 不以
        // `|` 开头，管道在 closeMark 里）与 `[[a|b|c]]` 的第二个 `|` 保持原语义。
        // 全量与 buildLpPlanPartial 都走这一处，偏移已是全文坐标，故直接读 text。
        if (span.kind == LpSpanKind::WikiLink && span.content.beg < span.content.end &&
            static_cast<std::size_t>(span.content.beg) < text.size() &&
            text[static_cast<std::size_t>(span.content.beg)] == '|') {
            span.openMark.end = span.content.beg + 1;
            ++span.content.beg;
        }
        spans.push_back(span);
    }
    return spans;
}

// 行分组的键：决定哪些相邻行属于同一个"块"（后续"活动行整块不装饰"要用）。
struct GroupKey {
    int kindClass = 0;
    int level = 0;
    int quoteDepth = 0;

    bool operator==(const GroupKey& other) const {
        return kindClass == other.kindClass && level == other.level && quoteDepth == other.quoteDepth;
    }
};

GroupKey groupKeyFor(const LpLine& line) {
    GroupKey key;
    key.quoteDepth = line.quoteDepth;
    switch (line.kind) {
        case LpKind::Blank:
            key.kindClass = 0;
            break;
        case LpKind::Text:
        case LpKind::ListItem:
        case LpKind::TaskItem:
            key.kindClass = 1;  // 段落/列表项：连续行算一块
            break;
        case LpKind::Heading:
            // setext 的两行（文字行 + 下划线行）算一块；ATX 标题各占一块
            key.kindClass = line.setext ? 2 : 3;
            key.level = line.headingLevel;
            break;
        case LpKind::Code:
            key.kindClass = 4;
            break;
        case LpKind::Table:
            key.kindClass = 5;
            break;
        case LpKind::Divider:
            key.kindClass = 6;
            break;
        case LpKind::Frontmatter:
            key.kindClass = 7;
            break;
    }
    return key;
}

void addConceal(LpLine& line, LpRange range) {
    if (range.empty()) {
        return;
    }
    line.conceal.push_back(range);
}

void normalizeConceal(const std::string& text, LpLine& line, LpPlanStats* stats) {
    std::vector<LpRange> kept;
    kept.reserve(line.conceal.size());
    for (const LpRange& range : line.conceal) {
        if (range.empty()) {
            continue;
        }
        if (range.beg < line.srcBeg || range.end > line.srcEnd) {
            ++stats->invalidRanges;  // 越界：整条丢掉，别把错误扩散到渲染层
            continue;
        }
        kept.push_back(range);
    }
    std::sort(kept.begin(), kept.end(), [](const LpRange& a, const LpRange& b) {
        return a.beg != b.beg ? a.beg < b.beg : a.end < b.end;
    });
    line.conceal.clear();
    for (const LpRange& range : kept) {
        if (!line.conceal.empty() && range.beg <= line.conceal.back().end) {
            LpRange& back = line.conceal.back();
            if (range.beg == back.beg && range.end == back.end) {
                ++stats->duplicateRanges;  // 同一标记被两种 span 各报一次（EM+STRONG 之类）
            } else {
                ++stats->mergedRanges;  // 首尾相接或部分重叠：合并成一段
            }
            back.end = std::max(back.end, range.end);
            continue;
        }
        line.conceal.push_back(range);
    }
    stats->concealRanges += static_cast<int>(line.conceal.size());
    if (!line.conceal.empty()) {
        ++stats->linesWithConceal;
    }
    (void) text;
}

// 图片 span 的 src 原文：closeMark 形如 "](url)"、"](<url>)" 或 "](url "title")"。
// 这里只负责**取出来**；路径解析与落盘存在性检查属于装饰层（要碰文件系统）。
inline std::string imageSourceOf(const std::string& text, const LpSpan& span) {
    if (span.closeMark.beg >= span.closeMark.end) {
        return {};
    }
    const std::size_t markBeg = static_cast<std::size_t>(span.closeMark.beg);
    const std::size_t markEnd = std::min(static_cast<std::size_t>(span.closeMark.end), text.size());
    if (markEnd == 0) {
        return {};
    }
    const std::size_t open = text.find('(', markBeg);
    if (open == std::string::npos || open >= markEnd) {
        return {};
    }
    const std::size_t close = text.rfind(')', markEnd - 1);
    if (close == std::string::npos || close <= open) {
        return {};
    }
    std::string raw = text.substr(open + 1, close - open - 1);
    const auto isBlank = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; };
    while (!raw.empty() && isBlank(raw.front())) {
        raw.erase(raw.begin());
    }
    while (!raw.empty() && isBlank(raw.back())) {
        raw.pop_back();
    }
    if (raw.empty()) {
        return {};
    }
    if (raw.front() == '<') {
        const std::size_t gt = raw.find('>');
        return gt == std::string::npos ? std::string{} : raw.substr(1, gt - 1);
    }
    // 带 title 的写法 `url "标题"`：v1 取到第一个空白为止。
    const std::size_t space = raw.find_first_of(" \t");
    return space == std::string::npos ? raw : raw.substr(0, space);
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

int LpPlan::lineIndexFor(int byteIndex) const {
    if (lines.empty()) {
        return -1;
    }
    const auto it = std::upper_bound(lines.begin(), lines.end(), byteIndex,
                                     [](int value, const LpLine& line) { return value < line.srcBeg; });
    if (it == lines.begin()) {
        return 0;
    }
    const int index = static_cast<int>(std::distance(lines.begin(), it)) - 1;
    return std::clamp(index, 0, static_cast<int>(lines.size()) - 1);
}

int LpPlan::taskStateByteFor(int byteIndex) const {
    const LpLine* line = lineAt(lineIndexFor(byteIndex));
    if (line == nullptr || line->kind != LpKind::TaskItem || line->taskStateByte < 0) {
        return -1;
    }
    return line->taskStateByte;
}

// 折叠目标：标题行（起始行，含 setext 文本行）折叠自己的章节；其余行取包住它的标题。
// setext 下划线行 sectionEndLine 为 -1 → 落到 sectionHeadingLine（= 配对文本行）。
int LpPlan::foldHeadingBegFor(int byteIndex) const {
    const int index = lineIndexFor(byteIndex);
    if (index < 0) {
        return -1;
    }
    const LpLine& line = lines[static_cast<std::size_t>(index)];
    if (line.kind == LpKind::Heading && line.sectionEndLine >= 0) {
        return line.srcBeg;
    }
    if (line.sectionHeadingLine >= 0) {
        return lines[static_cast<std::size_t>(line.sectionHeadingLine)].srcBeg;
    }
    return -1;
}

namespace {

// ---- Pass A：逐行分类 + 计算容器标记（围栏状态机 / 容器 / 标题 / setext）----
// 抽成独立函数是为了让 T5 的局部路径**对全篇重跑一遍**：跑完再拿区间外每一行的
// 字段与上一代逐字段比对 —— 比得上才说明"这次编辑没有把状态（inCode / listStack /
// lastNonBlank / frontmatterEnd）漏出局部范围"，比不上就回退全量。
// 扫描器本身一行未改，只把落点从 plan.lines/plan.stats 换成入参。
void runPassA(const std::string& text,
              const std::vector<RawLine>& raw,
              std::vector<LpLine>& lines,
              LpPlanStats& stats) {
    struct ScanState {
        bool inCode = false;
        char fenceChar = '`';
        int fenceCount = 0;
        int codeBlockBeg = -1;
        // 当前（还没闭合的）代码块首行 = 开围栏行在 plan.lines 里的下标，-1 = 不在代码块里。
        // 闭围栏时只回填 [codeBlockFirstLine, 闭围栏行] 这一段（T6），不必扫整张行表。
        int codeBlockFirstLine = -1;
        std::vector<ListGroup> listStack;
        int lastNonBlank = -1;
    };
    ScanState state;

    // 前置：YAML frontmatter 区间（首行是 --- 且之后能找到 --- / ...）
    int frontmatterEnd = -1;
    if (!raw.empty()) {
        const int firstEnd = skipTrailingSpaces(text, raw[0].beg, raw[0].end);
        if (firstEnd - raw[0].beg == 3 && text.compare(static_cast<std::size_t>(raw[0].beg), 3, "---") == 0) {
            for (std::size_t i = 1; i < raw.size(); ++i) {
                const int e = skipTrailingSpaces(text, raw[i].beg, raw[i].end);
                const std::string body = text.substr(static_cast<std::size_t>(raw[i].beg),
                                                     static_cast<std::size_t>(e - raw[i].beg));
                if (body == "---" || body == "...") {
                    frontmatterEnd = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    for (std::size_t i = 0; i < raw.size(); ++i) {
        LpLine& line = lines[i];
        const int beg = raw[i].beg;
        const int end = raw[i].end;
        const int firstNonSpace = skipSpaces(text, beg, end);
        const int lastNonSpace = skipTrailingSpaces(text, beg, end);

        // Empty physical lines still belong to their enclosing verbatim block.
        // Dropping that identity breaks the continuous background and active block.
        if (firstNonSpace >= end) {
            if (state.inCode) {
                line.kind = LpKind::Code;
                line.codeBlockBeg = state.codeBlockBeg;
                state.lastNonBlank = static_cast<int>(i);
            } else if (frontmatterEnd >= 0 && static_cast<int>(i) <= frontmatterEnd) {
                line.kind = LpKind::Frontmatter;
            } else {
                line.kind = LpKind::Blank;
            }
            continue;
        }

        // frontmatter
        if (frontmatterEnd >= 0 && static_cast<int>(i) <= frontmatterEnd) {
            line.kind = LpKind::Frontmatter;
            addConceal(line, {beg, firstNonSpace});
            terminateListContext(state.listStack, containerIndentWidth(text, beg, end));
            continue;
        }

        // 代码块内
        if (state.inCode) {
            line.kind = LpKind::Code;
            line.codeBlockBeg = state.codeBlockBeg;
            line.codeFence = isClosingFence(text, beg, end, state.fenceChar, state.fenceCount);
            if (line.codeFence) {
                addConceal(line, {beg, end});  // 闭围栏整行隐藏
                state.inCode = false;
                // 收尾：整块区间 = 开围栏行首 ~ 闭围栏行尾。
                // 只遍历本块的行区间 [开围栏行, 闭围栏行]（T6）：块外行靠旧谓词排除，
                // Blank code lines carry the same owner and receive the same end.
                const int firstLine = std::max(state.codeBlockFirstLine, 0);
                const int lastLine = static_cast<int>(i);
                for (int index = firstLine; index <= lastLine; ++index) {
                    LpLine& codeLine = lines[static_cast<std::size_t>(index)];
                    if (codeLine.kind == LpKind::Code && codeLine.codeBlockBeg == state.codeBlockBeg) {
                        codeLine.codeBlockEnd = end;
                    }
                }
                ++stats.fencedBlocks;
                state.codeBlockBeg = -1;
                state.codeBlockFirstLine = -1;
            }
            state.lastNonBlank = static_cast<int>(i);
            continue;
        }

        // 开围栏
        {
            char fenceChar = '\0';
            const int run = fenceRun(text, beg, end, &fenceChar);
            if (run > 0 && indentWidth(text, beg, end) <= 3) {
                int infoBeg = skipSpaces(text, beg, end);
                while (infoBeg < end && text[static_cast<std::size_t>(infoBeg)] == fenceChar) {
                    ++infoBeg;
                }
                // 反引号围栏的 info string 里不能再出现反引号（出现了就不是围栏）
                const bool valid = fenceChar != '`' ||
                                   text.find('`', static_cast<std::size_t>(infoBeg)) == std::string::npos ||
                                   static_cast<int>(text.find('`', static_cast<std::size_t>(infoBeg))) >= end;
                if (valid) {
                    infoBeg = skipSpaces(text, infoBeg, end);
                    const int infoEnd = std::max(infoBeg, lastNonSpace);
                    line.kind = LpKind::Code;
                    line.codeFence = true;
                    line.codeBlockBeg = beg;
                    line.codeLang = text.substr(static_cast<std::size_t>(infoBeg),
                                                static_cast<std::size_t>(infoEnd - infoBeg));
                    addConceal(line, {beg, end});  // 开围栏整行隐藏（活动行时由视图恢复）
                    state.inCode = true;
                    state.fenceChar = fenceChar;
                    state.fenceCount = run;
                    state.codeBlockBeg = beg;
                    state.codeBlockFirstLine = static_cast<int>(i);  // 本块首行 = 开围栏行
                    state.lastNonBlank = static_cast<int>(i);
                    terminateListContext(state.listStack, containerIndentWidth(text, beg, end));
                    continue;
                }
            }
        }

        // Spaced thematic breaks such as "- - -" must win over list markers.
        // Plain "---" stays on the existing path so it can form a setext heading.
        if (indentWidth(text, beg, end) <= 3 &&
            isThematicBreak(text, firstNonSpace, lastNonSpace) &&
            !rangeIsRunOf(text, firstNonSpace, lastNonSpace, '-')) {
            line.kind = LpKind::Divider;
            addConceal(line, {beg, end});
            state.lastNonBlank = static_cast<int>(i);
            terminateListContext(state.listStack, containerIndentWidth(text, beg, end));
            continue;
        }

        // 容器标记（引用 / 列表 / 任务）
        const ContainerResult container = scanContainers(text, beg, end, static_cast<int>(i), state.listStack);
        line.quoteDepth = container.quoteDepth;
        line.listDepth = container.listDepth;

        // setext 下划线（必须排在分隔线判断之前）
        const bool runOfEquals = rangeIsRunOf(text, firstNonSpace, lastNonSpace, '=');
        const bool runOfDashes = rangeIsRunOf(text, firstNonSpace, lastNonSpace, '-');
        if ((runOfEquals || runOfDashes) && container.quoteDepth == 0 && !container.anyListMarker &&
            state.lastNonBlank >= 0 && // A first-line underline has no preceding paragraph.
            state.lastNonBlank == static_cast<int>(i) - 1 &&
            !lines[static_cast<std::size_t>(state.lastNonBlank)].setext) {
            LpLine& previous = lines[static_cast<std::size_t>(state.lastNonBlank)];
            const bool previousIsParagraph = previous.kind == LpKind::Text && previous.listDepth == 0 &&
                                             previous.quoteDepth == container.quoteDepth;
            if (previousIsParagraph && (!runOfDashes || previous.srcEnd > previous.srcBeg)) {
                previous.kind = LpKind::Heading;
                previous.headingLevel = runOfEquals ? 1 : 2;
                previous.setext = true;
                line.kind = LpKind::Heading;
                line.headingLevel = previous.headingLevel;
                line.setext = true;
                addConceal(line, {beg, end});  // 下划线整行隐藏
                state.lastNonBlank = static_cast<int>(i);
                terminateListContext(state.listStack, container.lineIndent);
                continue;
            }
        }

        // 内容起点之后的部分
        int contentBeg = container.content;
        if (contentBeg < beg || contentBeg > end) {
            contentBeg = beg;
        }
        contentBeg = skipSpaces(text, contentBeg, end);
        if (contentBeg > end) {
            contentBeg = end;
        }
        line.contentBeg = contentBeg;

        if (container.anyListMarker) {
            line.kind = container.task ? LpKind::TaskItem : LpKind::ListItem;
            line.ordered = container.ordered;
            line.orderedNumber = container.orderedNumber;
            line.listOrdinal = container.listOrdinal;
            line.listItemLine = static_cast<int>(i);
            line.task = container.task;
            line.taskChecked = container.taskChecked;
            line.taskStateByte = container.taskStateByte;
        } else {
            line.kind = LpKind::Text;
        }

        // ATX 标题
        if (contentBeg < end && text[static_cast<std::size_t>(contentBeg)] == '#') {
            int hashes = 0;
            int p = contentBeg;
            while (p < end && text[static_cast<std::size_t>(p)] == '#') {
                ++hashes;
                ++p;
            }
            if (hashes >= 1 && hashes <= 6 && (p >= end || isSpaceByte(text[static_cast<std::size_t>(p)]))) {
                line.kind = LpKind::Heading;
                line.headingLevel = hashes;
                line.setext = false;
                int bodyBeg = skipSpaces(text, p, end);
                addConceal(line, {beg, std::min(bodyBeg, end)});
                // 结尾的 ### 也隐藏（CommonMark 的 closing sequence）
                int tail = lastNonSpace;
                int tailHashes = tail;
                while (tailHashes > bodyBeg && text[static_cast<std::size_t>(tailHashes - 1)] == '#') {
                    --tailHashes;
                }
                if (tailHashes < tail && tailHashes > bodyBeg &&
                    isSpaceByte(text[static_cast<std::size_t>(tailHashes - 1)])) {
                    addConceal(line, {skipTrailingSpaces(text, bodyBeg, tailHashes), tail});
                }
                state.lastNonBlank = static_cast<int>(i);
                terminateListContext(state.listStack, container.lineIndent);
                continue;
            }
        }

        // 分隔线
        if (isThematicBreak(text, contentBeg, lastNonSpace) && !container.anyListMarker &&
            container.quoteDepth == 0) {
            line.kind = LpKind::Divider;
            addConceal(line, {beg, end});
            state.lastNonBlank = static_cast<int>(i);
            terminateListContext(state.listStack, container.lineIndent);
            continue;
        }

        // 表格行
        if (looksLikeTableRow(text, contentBeg, end)) {
            line.kind = LpKind::Table;
        }

        // 非列表行：先决定列表上下文是否还活着，再给"列表项的物理续行"记账（C1）。
        // 存活判据（CommonMark 语义）：这一行必须缩进到栈顶项的**正文列**（缩进续段，
        // 可以跨空行 —— 宽松列表），或者是紧贴上一条列表内容行的 lazy 续段；两者都不
        // 满足就说明它不属于任何列表项，必须弹栈。空行是宽松列表的分隔符，不弹。
        //
        // 为什么必须有这条：早先只在"缩进比标记列更浅"时弹栈，而顶格列表的标记列是 0，
        // 于是标题 / 围栏 / 分隔线 / 空行后的顶格段落全都弹不掉 —— 组会跨过它们继续
        // 计数（`### 标题` 之后重新从 1 起的列表会被渲染成 4.，实测见 2026-09-29 截图）。
        if (!container.anyListMarker) {
            const int width = container.lineIndent;  // 容器相对缩进（不是行首缩进）
            const bool lazyOwner =
                line.kind == LpKind::Text && state.lastNonBlank >= 0 &&
                static_cast<int>(i) == state.lastNonBlank + 1 &&
                lines[static_cast<std::size_t>(state.lastNonBlank)].listItemLine >= 0;
            if (line.kind != LpKind::Blank) {
                while (!state.listStack.empty()) {
                    const ListGroup& top = state.listStack.back();
                    if (width >= top.contentColumn || lazyOwner) {
                        break;
                    }
                    state.listStack.pop_back();
                }
            }
            if (!state.listStack.empty() && line.kind == LpKind::Text) {
                const ListGroup& top = state.listStack.back();
                if (width >= top.contentColumn || lazyOwner) {
                    line.listItemLine = top.lastItemLine;
                }
            }
        }

        // 段落类行的容器标记隐藏区间（列表标记 / 任务框 / 引用前缀 / 缩进都在里面）
        if (contentBeg > beg) {
            addConceal(line, {beg, contentBeg});
        }
        state.lastNonBlank = static_cast<int>(i);
    }
}

// ---------------------------------------------------------------------------
// T5（局部重解析）：风险扫描、行搬运与 Pass A 结果核对
// ---------------------------------------------------------------------------

inline char asciiLower(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool startsWithCi(const std::string& text, int p, int end, const char* needle) {
    for (const char* c = needle; *c != '\0'; ++c, ++p) {
        if (p >= end || asciiLower(text[static_cast<std::size_t>(p)]) != *c) {
            return false;
        }
    }
    return true;
}

// 链接引用定义的**必要**子串：CommonMark 的定义长成 `[label]: dest`，`]` 与 `:`
// 必须相邻（中间有东西就不是定义）。所以"全文不含 `]:`" ⇒ 必然没有引用定义 ⇒
// md4c 的引用表为空 ⇒ 行内解析逐块独立 —— 这正是局部切片能等价的前提。
// 判据故意取"必要条件"的补集：宁可把 `]:` 当成普通文字的文档误伤成全量，也绝不
// 放过一个真定义。
bool hasLinkRefDefSyntax(const std::string& text) {
    std::size_t p = text.find(']');
    while (p != std::string::npos) {
        if (p + 1 < text.size() && text[p + 1] == ':') {
            return true;
        }
        p = text.find(']', p + 1);
    }
    return false;
}

// 行首（≤3 空格 / 1 个制表符）之后是否是**能跨空行**的 HTML 块起始
// （CommonMark 类型 1/2/3）。这些块不认空行，md4c 的 verbatim 叶子块会越过
// 局部边界 → 局部切片无法证明与全量一致。其余 HTML 类型在空行处结束，
// 被"局部两侧必须是空行"这条判据天然挡住，不在此列。
bool htmlBlockOpenAt(const std::string& text, int beg, int end) {
    int p = beg;
    int width = 0;
    while (p < end && isSpaceByte(text[static_cast<std::size_t>(p)]) && width < 4) {
        width += text[static_cast<std::size_t>(p)] == '\t' ? 4 : 1;
        ++p;
    }
    if (p >= end || text[static_cast<std::size_t>(p)] != '<') {
        return false;
    }
    if (startsWithCi(text, p, end, "<!") || startsWithCi(text, p, end, "<?")) {
        return true;
    }
    static const char* const kOpeners[] = {"<script", "<pre", "<style", "<textarea"};
    for (const char* opener : kOpeners) {
        if (startsWithCi(text, p, end, opener)) {
            return true;
        }
    }
    return false;
}

// 局部字节范围内的行有没有 HTML 块起始行。
bool regionHasHtmlBlockRisk(const std::string& text, int begByte, int endByte) {
    std::size_t p = static_cast<std::size_t>(begByte);
    const std::size_t stop = static_cast<std::size_t>(endByte);
    while (p <= stop) {
        const std::size_t nl = text.find('\n', p);
        const int lineEnd = nl == std::string::npos || nl > stop ? static_cast<int>(stop)
                                                                : static_cast<int>(nl);
        if (htmlBlockOpenAt(text, static_cast<int>(p), lineEnd)) {
            return true;
        }
        if (nl == std::string::npos || nl >= stop) {
            break;
        }
        p = nl + 1;
    }
    return false;
}

// 局部范围内有没有"围栏行"（≥3 个 ` 或 ~ 的连排，任意缩进都算 —— 比 Pass A 的
// 判定更严：Pass A 与 md4c 对围栏的边角规则若有分歧，分歧点绝不能落在局部里）。
bool regionHasFenceRun(const std::string& text, int begByte, int endByte) {
    std::size_t p = static_cast<std::size_t>(begByte);
    const std::size_t stop = static_cast<std::size_t>(endByte);
    while (p < stop) {
        const std::size_t nl = text.find('\n', p);
        const int lineEnd = nl == std::string::npos || nl > stop ? static_cast<int>(stop)
                                                                : static_cast<int>(nl);
        char fenceChar = '\0';
        if (fenceRun(text, static_cast<int>(p), lineEnd, &fenceChar) >= 3) {
            return true;
        }
        if (nl == std::string::npos || nl >= stop) {
            break;
        }
        p = nl + 1;
    }
    return false;
}

// index 行是否"在更早打开的围栏代码块里"。代码容器内空行同样是 Code。
// 跳过普通 Blank 行，查上一条容器记录：开围栏行与内容行
// 都算在块里，闭围栏行不算（它的 codeBlockBeg 指向块首、小于自己的行首）。
bool inCodeStateAt(const std::vector<LpLine>& lines, int index) {
    int j = index - 1;
    while (j >= 0 && lines[static_cast<std::size_t>(j)].kind == LpKind::Blank) {
        --j;
    }
    if (j < 0) {
        return false;
    }
    const LpLine& line = lines[static_cast<std::size_t>(j)];
    if (line.kind != LpKind::Code) {
        return false;
    }
    return !(line.codeFence && line.codeBlockBeg < line.srcBeg);
}

// ── Pass A 的可观测字段（扫描器只写这些；conceal 由字节 + 这些字段唯一决定，
//    见 buildLpPlanPartial 的"证明边界"注释）。区间外每一行都必须逐字段相等。
//    byteShift：后缀行的字节平移量（前缀行传 0）—— 比对时直接加，免得搬一份副本。
//    lineShift：后缀行的**行号**平移量（前缀行传 0）—— C1 起 listItemLine 这类
//    行号字段也参与证明，同样按平移折算。
bool passAFieldsEqual(const LpLine& fresh, const LpLine& carried, int byteShift, int lineShift,
                      std::string* field) {
    const auto bad = [field](const char* name) {
        if (field != nullptr) {
            *field = name;
        }
        return false;
    };
    const auto shiftIn = [byteShift](int value) {
        return value >= 0 ? value + byteShift : value;
    };
    const auto shiftLineIn = [lineShift](int value) {
        return value >= 0 ? value + lineShift : value;
    };
    // kind 必先相等：它同时是 setext / frontmatter / 围栏三类跨行状态漏出的
    // 观测面（旧行与新行的字节逐字节相同，kind 不同只能是状态变了）。
    if (fresh.kind != carried.kind) return bad("kind");
    if (fresh.quoteDepth != carried.quoteDepth) return bad("quoteDepth");
    // 只在"该行真的有列表标记"时才比列表字段：listDepth>0 ⟺ 本行有标记
    // （无标记时扫描器恒写 0），两侧字节相同 ⇒ 无标记那侧必然同为 0。
    if ((fresh.listDepth > 0) != (carried.listDepth > 0)) return bad("listDepth");
    if (fresh.listDepth > 0) {
        if (fresh.listDepth != carried.listDepth) return bad("listDepth");
        if (fresh.ordered != carried.ordered) return bad("ordered");
        if (fresh.orderedNumber != carried.orderedNumber) return bad("orderedNumber");
        if (fresh.listOrdinal != carried.listOrdinal) return bad("listOrdinal");
    }
    // 列表归属（C1）：标记行记自己、续行记 owner；owner 落在重算区间里时必然
    // 对不上 → 按判据失败回退全量（保守而正确）。
    if (fresh.listItemLine != shiftLineIn(carried.listItemLine)) return bad("listItemLine");
    if (fresh.contentBeg != shiftIn(carried.contentBeg)) return bad("contentBeg");
    if (fresh.kind == LpKind::Heading) {
        if (fresh.headingLevel != carried.headingLevel) return bad("headingLevel");
        if (fresh.setext != carried.setext) return bad("setext");
    }
    if (fresh.kind == LpKind::TaskItem || fresh.kind == LpKind::ListItem) {
        if (fresh.task != carried.task) return bad("task");
        if (fresh.taskChecked != carried.taskChecked) return bad("taskChecked");
        if (fresh.taskStateByte != shiftIn(carried.taskStateByte)) return bad("taskStateByte");
    }
    if (fresh.kind == LpKind::Code) {
        if (fresh.codeFence != carried.codeFence) return bad("codeFence");
        if (fresh.codeLang != carried.codeLang) return bad("codeLang");
        if (fresh.codeBlockBeg != shiftIn(carried.codeBlockBeg)) return bad("codeBlockBeg");
        // codeBlockEnd：Pass A 只在"碰到闭围栏"时回填，未闭合块要等收尾把它填成
        // 行尾（见 buildLpPlanImpl 的代码块/头信息那一段）。上一代带的是收尾之后
        // 的值，所以新鲜结果按同一条规则先补上再比。
        const int freshCodeEnd = fresh.codeBlockEnd < 0 ? fresh.srcEnd : fresh.codeBlockEnd;
        if (freshCodeEnd != shiftIn(carried.codeBlockEnd)) return bad("codeBlockEnd");
    }
    return true;
}

// 一行的"内容起点"（md4c 校正后的口径）：从行首开始的那条 conceal 的终点。
int contentBegOfLine(const LpLine& line) {
    int value = line.srcBeg;
    for (const LpRange& range : line.conceal) {
        if (range.beg == line.srcBeg && range.end > value) {
            value = range.end;
        }
    }
    return value;
}
// 骨架行（srcBeg/srcEnd/number）—— Pass A 的落点与"新鲜重跑"的临时行表都用它。
void initLineSkeleton(const std::vector<RawLine>& raw, std::vector<LpLine>& lines) {
    lines.resize(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        lines[i].srcBeg = raw[i].beg;
        lines[i].srcEnd = raw[i].end;
        lines[i].number = static_cast<int>(i);
    }
}

void initPlanLines(const std::vector<RawLine>& raw, LpPlan& plan) {
    initLineSkeleton(raw, plan.lines);
    plan.stats.lines = static_cast<int>(raw.size());
}

inline bool rejectPlan(const char* reason, std::string* reject) {
    if (reject != nullptr) {
        *reject = reason;
    }
    return false;
}

// ── 计划构建的唯一实现（全量与局部共用）────────────────────────────────────
// mdInput / mdBase     : md4c 的输入串与它在全文里的字节基址。全量 = (text, 0)；
//                        局部 = (编辑段切片, 切片起点)，喂给 md4c 之后整体平移回来。
// regionBeg/regionEnd  : **允许被 md4c 结果改动**的行下标区间（全量 = 整表）。
//                        任何 md4c 上报的区间/行下标落在它之外 → 局部立刻失败。
// prevPlan/delta*      : 非空 = 局部模式：先拿 Pass A 的全篇重跑结果逐行核对，
//                        通过的区间外行再按上一代平移复用 md4c 派生字段。
//                        不等 → 状态漏出局部 → 失败。
bool buildLpPlanImpl(LpPlan& plan,
                     const std::vector<RawLine>& raw,
                     const std::string& text,
                     const std::string& mdInput,
                     int mdBase,
                     int regionBeg,
                     int regionEnd,
                     const LpPlan* prevPlan,
                     int deltaBytes,
                     int deltaLines,
                     std::string* reject) {
    initPlanLines(raw, plan);
    // 局部入口已证明上一代无风险、区间外字节未变、完整局部行也无风险。
    // 直接沿用这个证明的结果；全量仍按整篇文本计算同样的两个标记。
    if (prevPlan == nullptr) {
        plan.stats.linkRefDef = hasLinkRefDefSyntax(text);
        for (const RawLine& line : raw) {
            if (htmlBlockOpenAt(text, line.beg, line.end)) {
                plan.stats.htmlBlockRisk = true;
                break;
            }
        }
    }
    const int mdRegionBegByte = mdBase;
    const int mdRegionEndByte = mdBase + static_cast<int>(mdInput.size());

    // ── Pass A：全篇重跑（全量与局部同一趟；落点都是 plan.lines）──────────────
    runPassA(text, raw, plan.lines, plan.stats);

    // 【局部】区间外逐行核对，成功后同一趟搬 md4c 派生字段与字节偏移。
    //    扫描器不读 spans/cells/block*，因此不必在扫描前单独遍历整张行表搬运。
    //    pureImageSrc / tableSeparator 仍由收尾重算。
    //    复现全量的 md4c "内容起点校正"与 span 标记。
    //    核对不上的当场失败（状态漏出局部）；核对得上的行，conceal 由
    //    "新鲜的扫描器分量 + 上一代的 md4c 校正值 + 上一代的 span 标记"拼出来，
    //    normalize 之后与全量逐字节相同（证明见 buildLpPlanPartial 的注释）。
    if (prevPlan != nullptr) {
        const int prevCount = static_cast<int>(prevPlan->lines.size());
        for (int i = 0; i < static_cast<int>(raw.size()); ++i) {
            if (i >= regionBeg && i <= regionEnd) {
                continue;
            }
            const int src = i < regionBeg ? i : i - deltaLines;
            if (src < 0 || src >= prevCount) {
                return rejectPlan("行映射越出上一代计划", reject);
            }
            LpLine& line = plan.lines[static_cast<std::size_t>(i)];
            const LpLine& from = prevPlan->lines[static_cast<std::size_t>(src)];
            const int shift = i > regionEnd ? deltaBytes : 0;
            const int lineShift = i > regionEnd ? deltaLines : 0;
            if (line.srcBeg != from.srcBeg + shift || line.srcEnd != from.srcEnd + shift) {
                return rejectPlan("复用行的行切分与上一代对不上", reject);
            }
            std::string field;
            if (!passAFieldsEqual(line, from, shift, lineShift, &field)) {
                planRejectReason() = "Pass A：行 " + std::to_string(i) + " 的 " + field +
                                     " 与上一代不一致（编辑把状态漏出了局部范围）";
                if (reject != nullptr) {
                    *reject = planRejectReason();
                }
                return false;
            }
            const auto shiftByte = [shift](int value) {
                return value >= 0 ? value + shift : value;
            };
            line.blockBeg = shiftByte(from.blockBeg);
            line.blockEnd = shiftByte(from.blockEnd);
            line.blockFirstLine = from.blockFirstLine;
            line.blockLastLine = from.blockLastLine;
            line.tableId = shiftByte(from.tableId);
            line.tableHeaderRow = from.tableHeaderRow;
            line.spans = from.spans;
            line.cells = from.cells;
            line.cellAligns = from.cellAligns;
            if (shift != 0) {
                // 只平移刚搬来的 md4c 区间；Pass A 已在新文本坐标下生成，不能再搬。
                for (LpSpan& span : line.spans) {
                    span.content.beg += shift;
                    span.content.end += shift;
                    span.openMark.beg += shift;
                    span.openMark.end += shift;
                    span.closeMark.beg += shift;
                    span.closeMark.end += shift;
                }
                for (LpRange& cell : line.cells) {
                    cell.beg += shift;
                    cell.end += shift;
                }
            }
            // md4c 的内容起点：上一代最终 conceal 里"从行首开始的那组合并区间"的终点
            // （normalize 之后它就是 md4c 校正 + span 标记并起来的头，见证明）。
            const int mdContentRaw = contentBegOfLine(from) + shift;
            // 与扫描器自己的起点比对 —— 这正是全量 step 1 里 markerMismatch 的判据。
            int ownContent = line.srcBeg;
            for (const LpRange& range : line.conceal) {
                if (range.beg == line.srcBeg) {
                    ownContent = std::max(ownContent, range.end);
                }
            }
            const bool skipKind = line.kind == LpKind::Blank || line.kind == LpKind::Code ||
                                  line.kind == LpKind::Divider || line.kind == LpKind::Frontmatter ||
                                  line.kind == LpKind::Table;
            if (!skipKind) {
                int mdContent = std::clamp(mdContentRaw, line.srcBeg, line.srcEnd);
                mdContent = std::min(skipSpaces(text, mdContent, line.srcEnd), line.srcEnd);
                if (ownContent != mdContent) {
                    ++plan.stats.markerMismatch;
                    const LpRange own{line.srcBeg, ownContent};
                    line.conceal.erase(std::remove_if(line.conceal.begin(), line.conceal.end(),
                                                      [&own](const LpRange& r) {
                                                          return r.beg == own.beg && r.end == own.end;
                                                      }),
                                       line.conceal.end());
                    addConceal(line, {line.srcBeg, mdContent});
                }
            }
        }
        // Spans are stored on their opening row, but a closing mark may be on
        // another source row. Reproduce full step 3's destination, after all
        // per-row content corrections, rather than clipping both marks to the owner.
        for (int i = 0; i < static_cast<int>(raw.size()); ++i) {
            if (i >= regionBeg && i <= regionEnd) continue;
            for (const LpSpan& span : plan.lines[static_cast<std::size_t>(i)].spans) {
                for (const LpRange& mark : {span.openMark, span.closeMark}) {
                    if (mark.empty()) continue;
                    const int target = plan.lineIndexFor(mark.beg);
                    if (target < 0 || target >= static_cast<int>(raw.size()) ||
                        (target >= regionBeg && target <= regionEnd))
                        return rejectPlan("复用 span 标记跨入局部重建区间", reject);
                    addConceal(plan.lines[static_cast<std::size_t>(target)], mark);
                }
            }
        }
    }

    // ---- md4c：叶子块（校正内容起点 + 块区间）与行内 span（Pass B） ----
#if defined(EUI_HAS_MD4C)
    MdParseResult md;
    if (parseWithMd4c(mdInput, &md)) {
        plan.stats.usedMd4c = true;
        if (mdBase != 0) {
            // 局部切片 → 全文坐标（source offset 与行号平移的另一半）。
            for (MdBlock& block : md.blocks) {
                block.beg += mdBase;
                block.end += mdBase;
                for (MdBlockLine& line : block.lines) {
                    line.beg += mdBase;
                    line.end += mdBase;
                }
            }
            for (MdSpanEvent& span : md.spans) {
                span.beg += mdBase;
                span.end += mdBase;
            }
        }

        // 局部模式的"越界即失败"：md4c 上报的任何区间/行下标都必须落在局部
        // 范围内；越出去 = 这一段的块结构跨过了局部边界（叶子块跨空行、引用定义、
        // 围栏/HTML 吞行……），切片证明当场作废 → 整体回退全量。
        // 全量模式下这两个范围恒为"整篇"，判据永不触发（md4c 的偏移天然落在
        // [0, text.size()]、行下标天然落在整表内），行为与改造前逐字一致。
        const auto guardRange = [&](int beg, int end) {
            if (beg >= mdRegionBegByte && end <= mdRegionEndByte) {
                return true;
            }
            return rejectPlan("md4c 上报区间越出局部范围（叶子块跨界）", reject);
        };
        const auto guardLine = [&](int index) {
            if (index >= regionBeg && index <= regionEnd) {
                return true;
            }
            return rejectPlan("md4c 上报行越出局部范围（叶子块跨界）", reject);
        };

        // 1) 逐行内容起点校正
        const auto lineIndexOf = [&plan](int byteIndex) { return plan.lineIndexFor(byteIndex); };
        for (const MdBlock& block : md.blocks) {
            if (block.verbatim || block.lines.empty()) {
                continue;  // CODE/HTML 的行条目是合并过的，交给围栏状态机
            }
            for (const MdBlockLine& entry : block.lines) {
                const int index = lineIndexOf(entry.beg);
                if (index < 0) {
                    continue;
                }
                if (!guardRange(entry.beg, entry.end) || !guardLine(index)) {
                    return false;
                }
                LpLine& line = plan.lines[static_cast<std::size_t>(index)];
                if (line.kind == LpKind::Blank || line.kind == LpKind::Code ||
                    line.kind == LpKind::Divider || line.kind == LpKind::Frontmatter ||
                    line.kind == LpKind::Table) {
                    continue;  // 这些行的隐藏规则与 md4c 的口径不同，不参与校正
                }
                int mdContent = std::clamp(entry.beg, line.srcBeg, line.srcEnd);
                mdContent = std::min(skipSpaces(text, mdContent, line.srcEnd), line.srcEnd);
                // 与本层算出的起点比对（诊断用）
                int ownContent = line.srcBeg;
                for (const LpRange& range : line.conceal) {
                    if (range.beg == line.srcBeg) {
                        ownContent = std::max(ownContent, range.end);
                    }
                }
                if (ownContent != mdContent) {
                    ++plan.stats.markerMismatch;
                    // 以 md4c 为准：丢掉本层"从行首开始"的那条区间，换成 md4c 的
                    const LpRange own{line.srcBeg, ownContent};
                    line.conceal.erase(std::remove_if(line.conceal.begin(), line.conceal.end(),
                                                      [&own](const LpRange& r) {
                                                          return r.beg == own.beg && r.end == own.end;
                                                      }),
                                       line.conceal.end());
                    addConceal(line, {line.srcBeg, mdContent});
                }
            }
        }

        // 2) 块区间（按块覆盖的物理行范围标记）
        for (const MdBlock& block : md.blocks) {
            if (block.verbatim || block.beg < 0 || block.end <= block.beg) {
                continue;
            }
            // 表格单元格也是"叶子块"（S3f-0 补丁报出来的），但它们不是行级块：
            // 一行里有多格，逐格写 blockBeg/blockEnd 会让最后一次写入获胜（行号/底色/
            // 活动块判定全乱）。表格行的块区间由 TABLE 块统一给，见 2b。
            if (block.type == static_cast<int>(MD_BLOCK_TH) ||
                block.type == static_cast<int>(MD_BLOCK_TD)) {
                continue;
            }
            const int first = lineIndexOf(block.beg);
            const int last = lineIndexOf(block.end - 1);
            if (first < 0 || last < 0) {
                continue;
            }
            if (!guardRange(block.beg, block.end) || !guardLine(first) || !guardLine(last)) {
                return false;
            }
            for (int index = first; index <= last && index < static_cast<int>(plan.lines.size()); ++index) {
                LpLine& line = plan.lines[static_cast<std::size_t>(index)];
                if (line.kind == LpKind::Code || line.kind == LpKind::Frontmatter) {
                    continue;
                }
                line.blockBeg = block.beg;
                line.blockEnd = block.end;
                line.blockFirstLine = index == first;
                line.blockLastLine = index == last;
            }
        }

        // 2b) 表格（S3f 批次 D）：TABLE 块给出表分组，TH/TD 块给出每格的内容区间。
        // 分隔行 |---|---| 不产生 cell —— 靠"表内没有 cell 的行"识别（md4c 不为它报）。
        // 无前导管道的表（`a | b` 起头）不算 LpKind::Table，这里按 md4c 的口径补正，
        // 免得同一张表里两种行类型不一致。
        for (const MdBlock& block : md.blocks) {
            if (block.type != static_cast<int>(MD_BLOCK_TABLE) || block.end <= block.beg) {
                continue;
            }
            const int first = lineIndexOf(block.beg);
            const int last = lineIndexOf(block.end - 1);
            if (first < 0 || last < 0) {
                continue;
            }
            if (!guardRange(block.beg, block.end) || !guardLine(first) || !guardLine(last)) {
                return false;
            }
            for (int index = first; index <= last && index < static_cast<int>(plan.lines.size()); ++index) {
                LpLine& line = plan.lines[static_cast<std::size_t>(index)];
                if (line.kind == LpKind::Code || line.kind == LpKind::Frontmatter ||
                    line.kind == LpKind::Heading) {
                    continue;
                }
                line.tableId = block.beg;
                if (line.kind != LpKind::Table) {
                    line.kind = LpKind::Table;
                }
            }
        }
        for (const MdBlock& block : md.blocks) {
            const bool isHeaderCell = block.type == static_cast<int>(MD_BLOCK_TH);
            if (!isHeaderCell && block.type != static_cast<int>(MD_BLOCK_TD)) {
                continue;
            }
            if (block.beg < 0 || block.beg > static_cast<int>(text.size())) {
                continue;
            }
            const int index = lineIndexOf(block.beg);
            if (index < 0 || index >= static_cast<int>(plan.lines.size())) {
                continue;
            }
            if (!guardRange(block.beg, block.end) || !guardLine(index)) {
                return false;
            }
            LpLine& line = plan.lines[static_cast<std::size_t>(index)];
            if (line.tableId < 0) {
                continue;
            }
            line.cells.push_back({block.beg, block.end});
            line.cellAligns.push_back(block.align);
            if (isHeaderCell) {
                line.tableHeaderRow = true;
            }
        }
        // 管道符与两侧空白**不进 conceal**：它要"永远隐藏"（不随"光标所在块露出源码"而
        // 露出），而 conceal 的语义是"可以被活动块露出来"。装饰层拿 cells 反推缺口即可
        // （见 lp_decorations 的表格段）。分隔行同理：cells 为空 = 整行藏掉。
        for (LpLine& line : plan.lines) {
            if (line.tableId >= 0 && line.cells.empty()) {
                line.tableSeparator = true;
            }
        }

        // 3) 行内 span
        const std::vector<LpSpan> spans = pairSpans(text, md.spans);
        plan.stats.spanCount = static_cast<int>(spans.size());
        for (const LpSpan& span : spans) {
            const int markBeg = !span.openMark.empty() ? span.openMark.beg : span.content.beg;
            const int index = lineIndexOf(markBeg);
            if (index < 0) {
                continue;
            }
            const int spanEnd = std::max(span.content.end, span.closeMark.end);
            if (!guardRange(markBeg, spanEnd) || !guardLine(index)) {
                return false;
            }
            plan.lines[static_cast<std::size_t>(index)].spans.push_back(span);
            // 标记自身可能跨行（开标记在一行、闭标记在另一行），各自归到所在的隐藏表
            for (const LpRange& mark : {span.openMark, span.closeMark}) {
                if (mark.empty()) {
                    continue;
                }
                if (!guardRange(mark.beg, mark.end)) {
                    return false;
                }
                const int markLine = lineIndexOf(mark.beg);
                if (markLine >= 0 && markLine < static_cast<int>(plan.lines.size())) {
                    addConceal(plan.lines[static_cast<std::size_t>(markLine)], mark);
                }
            }
        }
    }
#endif

    // ---- 收尾：规范化隐藏区间、块分组 ----
    for (LpLine& line : plan.lines) {
        normalizeConceal(text, line, &plan.stats);
        std::sort(line.spans.begin(), line.spans.end(),
                  [](const LpSpan& a, const LpSpan& b) { return a.content.beg < b.content.beg; });
    }

    // 纯图行（S3f 批次 B）：正文行里整行只有这一个图片 span、且 span 前后只有空白
    // —— 口径对齐 ZCode 的 `^\s*!\[...\]\(...\)\s*$`（规划 §8.2）。src 原样存进
    // pureImageSrc；路径解析/尺寸/文件存在性留给装饰层（那步要碰文件系统）。
    for (LpLine& line : plan.lines) {
        if (line.kind != LpKind::Text || line.spans.size() != 1) {
            continue;
        }
        const LpSpan& span = line.spans.front();
        // 需要 closeMark 才取得到 src；content 可以为空（`![](a.png)` 的空 alt 合法）。
        if (span.kind != LpSpanKind::Image || span.closeMark.empty()) {
            continue;
        }
        const int spanBeg = !span.openMark.empty() ? span.openMark.beg : span.content.beg;
        int spanEnd = span.content.end;
        if (span.closeMark.beg < span.closeMark.end) {
            spanEnd = std::max(spanEnd, span.closeMark.end);
        }
        const auto blank = [&text](int index) {
            const char ch = text[static_cast<std::size_t>(index)];
            return ch == ' ' || ch == '\t' || ch == '\r';
        };
        bool pure = true;
        for (int i = line.srcBeg; i < spanBeg && pure; ++i) {
            pure = blank(i);
        }
        for (int i = spanEnd; i < line.srcEnd && pure; ++i) {
            pure = blank(i);
        }
        if (pure) {
            line.pureImageSrc = imageSourceOf(text, span);
        }
    }

    // 章节表（S3f 批次 C）。两遍：
    // 第一遍 = 每个标题起始行的章节终点（下一个同级或更高级标题；文档末尾 = 行数）。
    // setext 是"文本行 + 下划线行"两行一个标题：下划线行不记终点（它属于标题自己），
    // 扫描时也要跳过"自己那条下划线"，否则章节会在标题内部就结束。
    const auto isSetextUnderline = [&plan](std::size_t index) {
        if (index == 0 || index >= plan.lines.size()) {
            return false;
        }
        const LpLine& line = plan.lines[index];
        const LpLine& previous = plan.lines[index - 1];
        return line.kind == LpKind::Heading && line.setext &&
               previous.kind == LpKind::Heading && previous.setext;
    };
    // 第一遍改用单调栈：O(N)（旧写法每个标题都向后线性扫，最坏 O(N²)）。按行序维护
    // "还没闭合的标题"栈，栈内级别从底到顶严格递增；标题行命中栈里级别 >= 自己的标题时
    // 自顶向下弹栈、把当前行号写进 sectionEndLine；扫完还留在栈里的标题终点 = 行数。
    // 与旧的朴素扫描**逐位等价**（tests/unit/lp_decorations.cpp 用朴素 oracle 逐行比对），
    // 有两个细节必须跟旧扫描逐字对齐，否则 setext 文档会跑偏：
    //  1) 旧扫描的候选是**所有**标题行，而不只是"标题起始行"：setext 配对里第二个文本行
    //     会被 isSetextUnderline 连坐误判成下划线（它前一行也是 setext 标题），可它照样
    //     要给上一个章节收尾 —— 所以弹栈判定对每个标题行都要做，包括被误判的那些。
    //     （真正的下划线行就算参与判定也弹不动任何东西：它的文本行级别相同且排在前面，
    //      该收尾的早就被文本行收掉了，所以"下划线不入栈、不记终点"这条照样成立。）
    //  2) 旧扫描对"自己那条下划线"有一条跳过规则 setext(i) && setext(j) && j == i + 1；
    //     但栈里 i 下面的元素仍可能被同一行 j 收尾（它的级别 >= j 的级别）。栈只能自顶
    //     向下，所以命中这条规则时把栈顶"暂存"出来，继续弹完更深的元素，再放回栈顶。
    //     暂存成立时当前行必然紧邻且同为 setext（即 isSetextUnderline 为真），它不会再
    //     入栈 —— 栈的严格递增不变量不会被破坏。
    std::vector<int> stack;
    // 栈里级别严格递增，而 headingLevel ∈ [1,6] → 深度恒 ≤ 6，够放就行
    // （reserve 只是省掉增长期的重分配，涨超了 vector 自己会翻倍）。
    stack.reserve(8);
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        const LpLine& line = plan.lines[i];
        if (line.kind != LpKind::Heading) {
            continue;
        }
        int deferred = -1;
        while (!stack.empty()) {
            const int top = stack.back();
            if (plan.lines[static_cast<std::size_t>(top)].headingLevel < line.headingLevel) {
                break;  // 栈顶级别更低：自己的章节还没结束，下面的更低，一律不动
            }
            if (top + 1 == static_cast<int>(i) && plan.lines[static_cast<std::size_t>(top)].setext &&
                line.setext) {
                deferred = top;  // 命中"自己的 setext 下划线"：本行不给它收尾
                stack.pop_back();
                continue;
            }
            plan.lines[static_cast<std::size_t>(top)].sectionEndLine = static_cast<int>(i);
            stack.pop_back();
        }
        if (deferred >= 0) {
            stack.push_back(deferred);
        }
        if (!isSetextUnderline(i)) {
            stack.push_back(static_cast<int>(i));
        }
    }
    for (const int top : stack) {
        plan.lines[static_cast<std::size_t>(top)].sectionEndLine = static_cast<int>(plan.lines.size());
    }

    // 第二遍：单调栈给出每行"最近的上方标题"（sectionHeadingLine）与标题的父链
    //（parentHeadingLine）。终点已就位，弹栈条件 = 标题的章节在本行之前已结束。
    // setext 下划线行不入栈：它的 sectionHeadingLine 自然指向配对文本行。
    {
        std::vector<int> stack;
        for (std::size_t i = 0; i < plan.lines.size(); ++i) {
            LpLine& line = plan.lines[i];
            while (!stack.empty() &&
                   plan.lines[static_cast<std::size_t>(stack.back())].sectionEndLine <=
                       static_cast<int>(i)) {
                stack.pop_back();
            }
            line.sectionHeadingLine = stack.empty() ? -1 : stack.back();
            if (line.kind == LpKind::Heading && !isSetextUnderline(i)) {
                line.parentHeadingLine = stack.empty() ? -1 : stack.back();
                stack.push_back(static_cast<int>(i));
            }
        }
    }

    // 块分组：md4c 已经给出块区间的行不动（它的边界就是 CommonMark 的叶子块，
    // 例如列表里每一项都是一个独立块）；md4c 没覆盖到的行（空行、围栏、frontmatter、
    // 分隔线）按"相邻同键归为一块"补上，供视图层做"活动块整块不装饰"。
    std::size_t blockFirst = 0;
    bool hasGroup = false;
    GroupKey currentKey;
    const auto flushGroup = [&](std::size_t lastIndex) {
        if (!hasGroup) {
            return;
        }
        for (std::size_t i = blockFirst; i <= lastIndex; ++i) {
            LpLine& line = plan.lines[i];
            if (line.blockBeg >= 0) {
                continue;
            }
            line.blockBeg = plan.lines[blockFirst].srcBeg;
            line.blockEnd = plan.lines[lastIndex].srcEnd;
            line.blockFirstLine = i == blockFirst;
            line.blockLastLine = i == lastIndex;
        }
        hasGroup = false;
    };
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        LpLine& line = plan.lines[i];
        const GroupKey key = groupKeyFor(line);
        const bool standalone = key.kindClass == 0 || key.kindClass == 3 || key.kindClass == 6;
        if (standalone) {
            flushGroup(i == 0 ? 0 : i - 1);
            if (line.blockBeg < 0) {
                line.blockBeg = line.srcBeg;
                line.blockEnd = line.srcEnd;
                line.blockFirstLine = true;
                line.blockLastLine = true;
            }
            continue;
        }
        if (hasGroup && key == currentKey) {
            continue;
        }
        flushGroup(i == 0 ? 0 : i - 1);
        currentKey = key;
        hasGroup = true;
        blockFirst = i;
    }
    flushGroup(plan.lines.size() - 1);

    // 代码块/头信息：块区间由扫描器给出，同时填进 blockBeg/blockEnd 便于视图统一处理
    for (LpLine& line : plan.lines) {
        if (line.kind == LpKind::Code) {
            if (line.codeBlockEnd < 0) {
                line.codeBlockEnd = line.srcEnd;
            }
            line.blockBeg = line.codeBlockBeg;
            line.blockEnd = line.codeBlockEnd;
            line.blockFirstLine = line.srcBeg == line.codeBlockBeg;
            line.blockLastLine = line.srcEnd == line.codeBlockEnd;
        }
    }

    // 表格行：活动块按**行**算（S3f 批次 D）。md4c 给的 TABLE 块覆盖整张表，若照用，
    // 光标进任一格都会让"整张表露出源码"——列对齐当场消失。逐行成块后，只有光标那一行
    // 露出其行内标记，其余行继续对齐（同 Obsidian 的逐行源码）。管道符不在 conceal 里，
    // 所以任何情况下都不会露出来（见 2b 的说明）。
    for (LpLine& line : plan.lines) {
        if (line.tableId < 0) {
            continue;
        }
        line.blockBeg = line.srcBeg;
        line.blockEnd = line.srcEnd;
        line.blockFirstLine = true;
        line.blockLastLine = true;
    }

    // 【局部】spanCount 要的是**全篇**口径（= 各行 spans 之和，全量下等价于
    // md4c 配对出的 span 总数），局部只重算了局部，必须整表重数一次。
    if (prevPlan != nullptr) {
        plan.stats.partial = true;
        int spanCount = 0;
        for (const LpLine& line : plan.lines) {
            spanCount += static_cast<int>(line.spans.size());
        }
        plan.stats.spanCount = spanCount;
    }
    return true;
}

// 诊断核验（NEO_LP_PLAN_VERIFY=1）：局部结果再跟全量逐字段对一遍。
// 这是**诊断**，不是正确性的前提 —— 正确性由上面的判据保证；它存在的意义是
// 在真实文档上不断给那些判据"喂反例"，一旦有漏网之鱼就当场回退并计数。
bool partialVerifyEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NEO_LP_PLAN_VERIFY");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

std::string planFieldDiff(const LpPlan& a, const LpPlan& b) {
    const auto diff = [](const std::string& what) { return what; };
    if (a.lines.size() != b.lines.size()) {
        return diff("lines.size " + std::to_string(a.lines.size()) + " vs " +
                    std::to_string(b.lines.size()));
    }
    for (std::size_t i = 0; i < a.lines.size(); ++i) {
        const LpLine& x = a.lines[i];
        const LpLine& y = b.lines[i];
        std::string field;
        if (!passAFieldsEqual(x, y, 0, 0, &field)) {
            return diff("行 " + std::to_string(i) + " PassA." + field);
        }
        const auto cmp = [&](bool same, const char* name) {
            if (!same && field.empty()) {
                field = name;
            }
            return same;
        };
        cmp(x.srcBeg == y.srcBeg && x.srcEnd == y.srcEnd && x.number == y.number, "src");
        cmp(x.conceal == y.conceal, "conceal");
        cmp(x.blockBeg == y.blockBeg && x.blockEnd == y.blockEnd &&
                x.blockFirstLine == y.blockFirstLine && x.blockLastLine == y.blockLastLine,
            "block");
        cmp(x.sectionHeadingLine == y.sectionHeadingLine &&
                x.parentHeadingLine == y.parentHeadingLine && x.sectionEndLine == y.sectionEndLine,
            "section");
        cmp(x.tableId == y.tableId && x.tableHeaderRow == y.tableHeaderRow &&
                x.tableSeparator == y.tableSeparator && x.cells == y.cells &&
                x.cellAligns == y.cellAligns, "table");
        cmp(x.pureImageSrc == y.pureImageSrc, "pureImageSrc");
        if (x.spans.size() != y.spans.size()) {
            cmp(false, "spans.size");
        } else {
            for (std::size_t s = 0; s < x.spans.size(); ++s) {
                cmp(x.spans[s].kind == y.spans[s].kind &&
                        x.spans[s].content == y.spans[s].content &&
                        x.spans[s].openMark == y.spans[s].openMark &&
                        x.spans[s].closeMark == y.spans[s].closeMark, "span");
            }
        }
        if (!field.empty()) {
            return diff("行 " + std::to_string(i) + " " + field);
        }
    }
    const auto statSame = [&](int x, int y, const char* name) {
        return x == y ? std::string() : diff(std::string("stats.") + name);
    };
    const std::vector<std::string> checks = {
        statSame(a.stats.lines, b.stats.lines, "lines"),
        statSame(a.stats.spanCount, b.stats.spanCount, "spanCount"),
        statSame(a.stats.concealRanges, b.stats.concealRanges, "concealRanges"),
        statSame(a.stats.linesWithConceal, b.stats.linesWithConceal, "linesWithConceal"),
        statSame(a.stats.fencedBlocks, b.stats.fencedBlocks, "fencedBlocks"),
        statSame(a.stats.invalidRanges, b.stats.invalidRanges, "invalidRanges"),
        a.stats.usedMd4c == b.stats.usedMd4c ? std::string() : diff("stats.usedMd4c"),
    };
    for (const std::string& check : checks) {
        if (!check.empty()) {
            return check;
        }
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

LpPlan buildLpPlan(const std::string& text) {
    ++planDebugStats().full;
    const std::vector<RawLine> raw = indexRawLines(text);
    LpPlan plan;
    std::string reject;
    buildLpPlanImpl(plan, raw, text, text, 0, 0, std::numeric_limits<int>::max(), nullptr, 0, 0,
                    &reject);
    return plan;
}

// 证明边界（先说清楚"局部为什么能等价于全量"，实现里的每条判据都对应这里一段）：
//
// 计划 = Pass A（扫描器）+ Pass B（md4c）+ 收尾（章节表 / 块分组 / 规范化）。
//   · **收尾的三段**对全篇重跑，只吃行表本身（kind/区间/块号），与"谁算出来的"无关
//     → 局部与全量天然同源。
//   · **Pass A** 也对全篇重跑。它只依赖两类输入：每行自己的字节（前缀/后缀字节
//     逐字节未变 ⇒ 逐行输入未变）与四个跨行状态 inCode / listStack / lastNonBlank /
//     frontmatterEnd。跨行状态从局部漏出去的**唯一可观测后果**就是 Pass A 字段
//     （kind / 标题与 setext / 引用列表深度 / 任务与状态字节 / 围栏与块区间 / codeLang）
//     在区间外对不上 —— 所以逐行比对这组字段**等价于**"状态没漏出去"，对不上即回退。
//     conceal 的 Pass A 分量 = f(行字节, 上述字段)（容器前缀、围栏整行、setext 下划线、
//     ATX 头尾、分隔线 —— 见 runPassA 里 addConceal 的全部调用点），故同样被证明。
//   · **md4c** 是唯一真正"只重算局部"的部分，它没有可执行的等价判据，只能证明：
//     ① 无链接引用定义（`]:` 是定义的必要子串；定义是 md4c 唯一的跨块内联状态）；
//     ② 无跨空行的 HTML 块（否则 verbatim 叶子块跨界）；
//     ③ 局部两侧都是空行 ⇒ 叶子块（段落/标题/表格/HR）不可能跨过局部边界；
//     ④ 局部不含围栏行、起点不在围栏块内 ⇒ CODE 这种能跨空行的 verbatim 块也不跨界；
//     ⑤ 运行时兜底：md4c 上报的每个区间/行下标必须落回局部范围，越界即回退。
//   以上任何一条不成立、或 Pass A 比对不过 → 返回 false，调用方**必须**全量。
bool buildLpPlanPartial(const std::string& text,
                        const std::string& prevText,
                        const LpPlan& prevPlan,
                        const LpTextEdit& edit,
                        LpPlan& out,
                        std::string* rejectReason) {
    const auto reject = [&](const std::string& reason) {
        if (rejectReason != nullptr) {
            *rejectReason = reason;
        }
        planRejectReason() = reason;
        ++planDebugStats().fallback;
        return false;
    };
    out = LpPlan{};

    // ── 0. 文本关系证明：prevText + edit 必须真的把 prevText 变成 text ─────
    // （复用上一代行数据的唯一依据；对不上就没有"上一代"可言。）
    if (!edit.valid) {
        return reject("编辑区间无效");
    }
    const int newSize = static_cast<int>(text.size());
    const int oldSize = static_cast<int>(prevText.size());
    if (edit.byteBeg < 0 || edit.oldEnd < edit.byteBeg || edit.oldEnd > oldSize ||
        edit.newEnd < edit.byteBeg || edit.newEnd > newSize) {
        return reject("编辑区间越界");
    }
    if (newSize - oldSize != edit.newEnd - edit.oldEnd) {
        return reject("前后文本长度与编辑区间不自洽");
    }
    if (prevText.compare(0, edit.byteBeg, text, 0, edit.byteBeg) != 0) {
        return reject("前缀不再逐字节相同（上一代不可复用）");
    }
    if (prevText.compare(edit.oldEnd, oldSize - edit.oldEnd, text, edit.newEnd,
                         newSize - edit.newEnd) != 0) {
        return reject("后缀不再逐字节相同（上一代不可复用）");
    }
    if (prevPlan.lines.empty()) {
        return reject("上一代计划为空");
    }

    // ── 1. 行号自洽（与 T4 的区间自检同款）────────────────────────────────
    // 行表切一次（只切一次，后面建计划复用同一份）；行数由换行数直接可数，
    // 与"prevCount + 行数差"必须对上 —— 对不上说明 edit 的行号元数据是错的。
    const std::vector<RawLine> raw = indexRawLines(text);
    const int prevCount = static_cast<int>(prevPlan.lines.size());
    const int first = edit.firstLine;
    const int newLast = edit.newLastLine;
    const int oldTail = edit.oldTailLine;
    if (first < 0 || newLast < first || oldTail < first || first >= prevCount ||
        oldTail > prevCount) {
        return reject("行号区间非法");
    }
    const int deltaLines = newLast + 1 - oldTail;
    const int deltaBytes = edit.newEnd - edit.oldEnd;
    const int newCount = prevCount + deltaLines;
    if (newCount != static_cast<int>(raw.size()) || newLast >= newCount) {
        return reject("新文本行数与编辑区间推出的行数不一致");
    }
    // raw 已按每个换行后的字节切过全文；查行首的上界与再数一次前缀换行等价。
    // 换行符本身属于前一行，EOF 属于最后一行（含尾随换行生成的空行）。
    const auto sourceLineAt = [&raw](int byte) {
        const auto after = std::upper_bound(raw.begin(), raw.end(), byte,
                                            [](int position, const RawLine& line) {
                                                return position < line.beg;
                                            });
        return static_cast<int>(after - raw.begin()) - 1;
    };
    if (first != sourceLineAt(edit.byteBeg)) {
        return reject("firstLine 与新文本前缀的换行数对不上");
    }
    if (edit.newEnd > edit.byteBeg) {
        // 与 recordPendingEdit 逐字对齐：newLastLine 数的是 [byteBeg, newEnd-1) 里的
        // 换行 —— 末字节所在的那一行才算"受影响的最后一行"。
        if (newLast != sourceLineAt(edit.newEnd - 1)) {
            return reject("newLastLine 与新文本受影响段的换行数对不上");
        }
    } else if (newLast != first) {
        return reject("newLastLine 与 firstLine 不一致（纯插入却跨了行）");
    }
    // 后缀锚点：新文本里第一个"仍可整行复用"的行，必须正好是旧行 oldTail 平移 deltaBytes。
    if (newLast + 1 < newCount) {
        const std::size_t anchor = static_cast<std::size_t>(newLast + 1);
        if (raw[anchor].beg != prevPlan.lines[static_cast<std::size_t>(oldTail)].srcBeg +
                                   deltaBytes) {
            return reject("后缀首行的行首与上一代平移结果对不上");
        }
    }

    // ── 2. 局部范围 = 受影响行 [first, newLast] 沿"非空行"向外扩到空行边界 ──
    // 空行边界正是 md4c 叶子块的边界（段落/标题/表格/HR 都在空行处结束），
    // 这样"局部"就等于"若干个**完整**的块"，块内 md4c 输出可由切片独立复现。
    int regionBeg = first;
    while (regionBeg > 0 && prevPlan.lines[static_cast<std::size_t>(regionBeg - 1)].kind !=
                                LpKind::Blank) {
        --regionBeg;
    }
    int regionEnd = newLast;
    while (regionEnd + 1 < newCount) {
        const int src = regionEnd + 1 - deltaLines;
        if (src < 0 || src >= prevCount ||
            prevPlan.lines[static_cast<std::size_t>(src)].kind == LpKind::Blank) {
            break;
        }
        ++regionEnd;
    }
    if (regionBeg > 0 && prevPlan.lines[static_cast<std::size_t>(regionBeg - 1)].kind !=
                             LpKind::Blank) {
        return reject("局部起点前不是空行（块可能跨界）");
    }
    if (regionEnd + 1 < newCount) {
        const int src = regionEnd + 1 - deltaLines;
        if (src < 0 || src >= prevCount ||
            prevPlan.lines[static_cast<std::size_t>(src)].kind != LpKind::Blank) {
            return reject("局部终点后不是空行（块可能跨界）");
        }
    }
    if (regionBeg == 0 && regionEnd == newCount - 1) {
        return reject("局部范围覆盖整篇（没有可复用的区间外行）");
    }
    // 局部字节范围：直接取自本次行表（行首 ≤ byteBeg ⇒ 与上一代同一位置，前缀
    // 逐字节相同已证），终点行在后缀里。
    const int regionEndLine = regionEnd - deltaLines;
    const int regionBegByte = raw[static_cast<std::size_t>(regionBeg)].beg;
    const int regionEndByte = raw[static_cast<std::size_t>(regionEnd)].end;
    if (first >= static_cast<int>(raw.size()) ||
        raw[static_cast<std::size_t>(first)].beg !=
            prevPlan.lines[static_cast<std::size_t>(first)].srcBeg) {
        return reject("firstLine 的行首与上一代对不上");
    }
    if (regionEndLine < prevCount && regionEnd > newLast) {
        // 终点行落在后缀里（复用区）→ 可与上一代平移结果互证；等于 newLast 时它在
        // 被改写的区间里，没有可对照的旧行，行表本身就是唯一真值。
        if (regionEndLine < 0 ||
            raw[static_cast<std::size_t>(regionEnd)].end !=
                prevPlan.lines[static_cast<std::size_t>(regionEndLine)].srcEnd + deltaBytes) {
            return reject("局部终点行的行尾与上一代平移结果对不上");
        }
    }
    if (regionEndLine < -1 || regionBegByte < 0 || regionEndByte > newSize ||
        regionBegByte > regionEndByte) {
        return reject("局部字节范围非法");
    }

    // ── 3. md4c 文档级状态（切片无法证明的部分）────────────────────────────
    if (prevPlan.stats.linkRefDef) {
        return reject("上一代文档含链接引用定义（md4c 引用表跨块，切片无法证明等价）");
    }
    const std::size_t refLike = text.find("]:", static_cast<std::size_t>(regionBegByte));
    if (refLike != std::string::npos && refLike < static_cast<std::size_t>(regionEndByte)) {
        return reject("局部里出现链接引用定义的 `]:`");
    }
    if (prevPlan.stats.htmlBlockRisk) {
        return reject("上一代文档含跨空行的 HTML 块");
    }
    if (regionHasHtmlBlockRisk(text, regionBegByte, regionEndByte)) {
        return reject("局部里出现跨空行的 HTML 块起始行");
    }

    // ── 4. 围栏（md4c 的 CODE 是能跨空行的 verbatim 块）─────────────────────
    if (inCodeStateAt(prevPlan.lines, regionBeg)) {
        return reject("局部起点落在围栏代码块内");
    }
    if (regionHasFenceRun(text, regionBegByte, regionEndByte)) {
        return reject("局部含围栏行（代码块结构无法用切片证明）");
    }

    // ── 5. 重建：区间外按上一代平移复用 + Pass A 全篇重跑核对 + 局部跑 md4c ──
    LpPlan built;
    std::string reason;
    const std::string slice = text.substr(static_cast<std::size_t>(regionBegByte),
                                          static_cast<std::size_t>(regionEndByte - regionBegByte));
    if (!buildLpPlanImpl(built, raw, text, slice, regionBegByte, regionBeg, regionEnd, &prevPlan,
                         deltaBytes, deltaLines, &reason)) {
        return reject(reason.empty() ? std::string("局部重建中途失败") : reason);
    }

    // ── 6. 诊断核验（可选）：与全量逐字段对一遍 ────────────────────────────
    if (partialVerifyEnabled()) {
        LpPlan fullPlan;
        std::string fullReject;
        buildLpPlanImpl(fullPlan, raw, text, text, 0, 0, std::numeric_limits<int>::max(), nullptr,
                        0, 0, &fullReject);
        const std::string diff = planFieldDiff(built, fullPlan);
        if (!diff.empty()) {
            ++planDebugStats().verifyMismatch;
            return reject("诊断核验：局部 ≠ 全量（" + diff + "）");
        }
    }

    out = std::move(built);
    ++planDebugStats().partial;
    return true;
}

}  // namespace neo
