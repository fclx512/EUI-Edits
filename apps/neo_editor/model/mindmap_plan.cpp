// mermaid `mindmap` 最小兼容子集的纯解析逻辑：无 UI、无 lib/、无全局状态。
//
// 约束：
//   · 不重建 Markdown（绝不调用 buildLpPlan）。目标块由调用方用 source cursor 在既有
//     LpPlan 上定位：cursor ∈ [0, text.size]、落在闭合顶级 mermaid 围栏内、围栏几何
//     （缩进/字符/长度/闭后内容）与 LP 区间边界都要自检，绝不扫描全文挑图。
//   · parseBlock 只吃整块切片（含两侧围栏），严格有限语法：首条非空非注释内容行必须
//     恰为 `mindmap`；整数缩进栈（每增列只一层）、单根、整块拒绝 Tab 与控制/非法 UTF-8；
//     注释忽略，`%%{`/保留指令/`::`/边声明/Markdown 标记拒绝；包装仅 裸标签 /
//     ((label)) / [label]（id 可省略）。任何拒绝都清空整棵树。
//   · 节点 sourceBeg/sourceEnd 是标签本身在原文里的绝对字节区间：
//     text.substr(sourceBeg, sourceEnd-sourceBeg) == node.label。
//   · plan.sourceBeg/sourceEnd 是整块半开区间，sourceEnd = sourceBeg + block.size()。

#include "model/mindmap_plan.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace neo::mindmap {
namespace {

using sv = std::string_view;

bool isAsciiAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdStart(unsigned char c) { return isAsciiAlpha(static_cast<char>(c)) || c == '_'; }
bool isIdChar(unsigned char c) {
    return isAsciiAlpha(static_cast<char>(c)) || isAsciiDigit(static_cast<char>(c)) || c == '_' || c == '-';
}
bool isBlankByte(char c) { return c == ' ' || c == '\t'; }

bool startsWith(sv s, sv prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
bool endsWith(sv s, sv suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 严格 UTF-8：拒绝过长编码 / 代理 / >U+10FFFF / 不完整序列。
bool decodeUtf8(sv s, std::size_t& pos, std::uint32_t& cp) {
    if (pos >= s.size()) {
        return false;
    }
    const unsigned char b0 = static_cast<unsigned char>(s[pos]);
    if (b0 < 0x80) {
        cp = b0;
        ++pos;
        return true;
    }
    int extra = 0;
    std::uint32_t value = 0;
    if ((b0 & 0xE0) == 0xC0) {
        if (b0 < 0xC2) {
            return false;
        }
        extra = 1;
        value = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        extra = 2;
        value = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        if (b0 > 0xF4) {
            return false;
        }
        extra = 3;
        value = b0 & 0x07;
    } else {
        return false;
    }
    if (pos + static_cast<std::size_t>(extra) >= s.size()) {
        return false;
    }
    for (int k = 0; k < extra; ++k) {
        const unsigned char b = static_cast<unsigned char>(s[pos + 1 + static_cast<std::size_t>(k)]);
        if ((b & 0xC0) != 0x80) {
            return false;
        }
        value = (value << 6) | (b & 0x3F);
    }
    if (extra == 1 && value < 0x80) {
        return false;
    }
    if (extra == 2 && (value < 0x800 || (value >= 0xD800 && value <= 0xDFFF))) {
        return false;
    }
    if (extra == 3 && (value < 0x10000 || value > 0x10FFFF)) {
        return false;
    }
    pos += 1 + static_cast<std::size_t>(extra);
    cp = value;
    return true;
}

// 整块校验；control = C0（\n 除外）/ DEL / C1。badLine 为 1-based 行号。
bool utf8Valid(sv block, std::size_t& badLine) {
    std::size_t line = 1;
    std::size_t pos = 0;
    while (pos < block.size()) {
        std::uint32_t cp = 0;
        if (!decodeUtf8(block, pos, cp)) {
            badLine = line;
            return false;
        }
        if (cp == '\n') {
            ++line;
            continue;
        }
        if (cp == '\t') {
            continue;  // Tab 由整块 firstTabLine 统一按 InvalidIndent 处理
        }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            badLine = line;
            return false;
        }
    }
    return true;
}

std::size_t countCodepoints(sv s) {
    std::size_t pos = 0;
    std::size_t count = 0;
    while (pos < s.size()) {
        std::uint32_t cp = 0;
        if (!decodeUtf8(s, pos, cp)) {
            break;
        }
        ++count;
    }
    return count;
}

struct SLine {
    std::size_t beg = 0;
    std::size_t end = 0;  // 不含换行符
};

std::vector<SLine> splitLines(sv block) {
    std::vector<SLine> out;
    std::size_t i = 0;
    const std::size_t n = block.size();
    for (;;) {
        const std::size_t nl = block.find('\n', i);
        const std::size_t end = (nl == sv::npos) ? n : nl;
        out.push_back(SLine{i, end});
        if (nl == sv::npos) {
            break;
        }
        i = nl + 1;
        if (i > n) {
            break;
        }
    }
    return out;
}

bool lineIsBlank(sv block, SLine line) {
    for (std::size_t p = line.beg; p < line.end; ++p) {
        if (!isBlankByte(block[p])) {
            return false;
        }
    }
    return true;
}

std::size_t lineLead(sv block, SLine line) {
    std::size_t p = line.beg;
    while (p < line.end && block[p] == ' ') {
        ++p;
    }
    return p;
}

std::size_t lineLastNonSpace(sv block, SLine line, std::size_t from) {
    std::size_t p = line.end;
    while (p > from && isBlankByte(block[p - 1])) {
        --p;
    }
    return p;
}

// 第一个含 Tab 的行（1-based）；无则 0。
std::size_t firstTabLine(sv block) {
    std::size_t line = 1;
    for (const char c : block) {
        if (c == '\t') {
            return line;
        }
        if (c == '\n') {
            ++line;
        }
    }
    return 0;
}

struct Fence {
    char ch = '\0';
    std::size_t indent = 0;
    std::size_t run = 0;
    std::size_t infoBeg = 0;
    std::size_t infoEnd = 0;
    bool hasInfo = false;
};

// 缩进 ≤3、前缀仅空格、run ≥3、反引号 info 内不得再出现反引号。
bool scanFence(sv block, SLine line, Fence& out) {
    std::size_t p = line.beg;
    while (p < line.end && block[p] == ' ') {
        ++p;
    }
    out.indent = p - line.beg;
    if (out.indent > 3 || p >= line.end) {
        return false;
    }
    const char ch = block[p];
    if (ch != '`' && ch != '~') {
        return false;
    }
    std::size_t q = p;
    std::size_t run = 0;
    while (q < line.end && block[q] == ch) {
        ++run;
        ++q;
    }
    if (run < 3) {
        return false;
    }
    if (ch == '`') {
        const std::size_t tick = block.find('`', q);
        if (tick != sv::npos && tick < line.end) {
            return false;
        }
    }
    std::size_t ib = q;
    while (ib < line.end && block[ib] == ' ') {
        ++ib;
    }
    std::size_t ie = line.end;
    while (ie > ib && block[ie - 1] == ' ') {
        --ie;
    }
    out.ch = ch;
    out.run = run;
    out.infoBeg = ib;
    out.infoEnd = ie;
    out.hasInfo = ie > ib;
    return true;
}

struct NodeSpec {
    std::string label;
    std::string id;
    bool hasId = false;
    Shape shape = Shape::Rectangle;
    std::size_t labelBeg = 0;
    std::size_t labelEnd = 0;
};

bool isReservedPrefix(sv content) {
    static const char* const kWords[] = {"classDef", "class", "style", "click", "linkStyle", "icon", "callback"};
    for (const char* word : kWords) {
        const sv kw(word);
        if (startsWith(content, kw)) {
            const char next = content.size() > kw.size() ? content[kw.size()] : '\0';
            if (next == '\0' || next == ' ' || next == ':' || next == '.' || next == '(' || next == '[') {
                return true;
            }
        }
    }
    return false;
}

// 成对/前缀型 Markdown 标记（允许普通英文 underscore，如 foo_bar）。
bool hasMarkdownMarkers(sv content) {
    if (content.find("::") != sv::npos || content.find("--") != sv::npos || content.find("->") != sv::npos ||
        content.find("-.") != sv::npos || content.find("===") != sv::npos) {
        return true;
    }
    if (!content.empty() && (content.front() == '!' || content.front() == '_' || content.back() == '_')) {
        return true;
    }
    return content.find("__") != sv::npos;
}

RejectReason parseNode(sv content, std::size_t contentBeg, NodeSpec& out) {
    if (startsWith(content, "%%{")) {
        return RejectReason::UnsupportedSyntax;
    }
    for (const char c : content) {
        if (c == '<' || c == '>' || c == '`' || c == '*' || c == '~') {
            return RejectReason::UnsupportedSyntax;
        }
    }
    if (hasMarkdownMarkers(content) || isReservedPrefix(content)) {
        return RejectReason::UnsupportedSyntax;
    }
    const std::size_t n = content.size();

    const auto finish = [&](std::size_t labelRelBeg, std::size_t labelRelEnd, std::size_t idEnd,
                            bool withId, Shape shape) -> RejectReason {
        const sv lab = content.substr(labelRelBeg, labelRelEnd - labelRelBeg);
        if (lab.empty() || lab.front() == '_' || lab.back() == '_' || lab.find("__") != sv::npos) {
            return RejectReason::UnsupportedSyntax;
        }
        for (const char c : lab) {
            if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
                return RejectReason::UnsupportedSyntax;
            }
        }
        out.label.assign(lab.data(), lab.size());
        out.hasId = withId;
        out.id.clear();
        if (withId) {
            out.id.assign(content.data(), idEnd);
        }
        out.shape = shape;
        out.labelBeg = contentBeg + labelRelBeg;
        out.labelEnd = contentBeg + labelRelEnd;
        return RejectReason::None;
    };

    const auto bare = [&]() -> RejectReason {
        if (content.front() == '_' || content.back() == '_' || content.find("__") != sv::npos) {
            return RejectReason::UnsupportedSyntax;
        }
        for (const char c : content) {
            if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
                return RejectReason::UnsupportedSyntax;
            }
        }
        out.label.assign(content.data(), n);
        out.hasId = false;
        out.id.clear();
        out.shape = Shape::Rectangle;
        out.labelBeg = contentBeg;
        out.labelEnd = contentBeg + n;
        return RejectReason::None;
    };

    std::size_t idEnd = 0;
    if (isIdStart(static_cast<unsigned char>(content[0]))) {
        while (idEnd < n && isIdChar(static_cast<unsigned char>(content[idEnd]))) {
            ++idEnd;
        }
    }

    if (idEnd > 0) {
        const sv rest = content.substr(idEnd);
        if (startsWith(rest, "((") && endsWith(rest, "))") && rest.size() >= 4) {
            return finish(idEnd + 2, n - 2, idEnd, true, Shape::Circle);
        }
        if (rest.size() >= 2 && rest[0] == '[' && rest[rest.size() - 1] == ']') {
            return finish(idEnd + 1, n - 1, idEnd, true, Shape::Rectangle);
        }
        return bare();
    }
    if (startsWith(content, "((") && endsWith(content, "))") && n >= 4) {
        return finish(2, n - 2, 0, false, Shape::Circle);
    }
    if (n >= 2 && content[0] == '[' && content[n - 1] == ']') {
        return finish(1, n - 1, 0, false, Shape::Rectangle);
    }
    return bare();
}

}  // namespace

const char* rejectReasonName(RejectReason reason) {
    switch (reason) {
        case RejectReason::None: return "none";
        case RejectReason::PlanNotReady: return "plan_not_ready";
        case RejectReason::NotMarkdown: return "not_markdown";
        case RejectReason::NoBlock: return "no_block";
        case RejectReason::UnclosedFence: return "unclosed_fence";
        case RejectReason::InvalidFence: return "invalid_fence";
        case RejectReason::UnsupportedLanguage: return "unsupported_language";
        case RejectReason::NotMindmap: return "not_mindmap";
        case RejectReason::UnsupportedSyntax: return "unsupported_syntax";
        case RejectReason::InvalidIndent: return "invalid_indent";
        case RejectReason::MultipleRoots: return "multiple_roots";
        case RejectReason::EmptyMap: return "empty_map";
        case RejectReason::InvalidUtf8: return "invalid_utf8";
        case RejectReason::DuplicateId: return "duplicate_id";
        case RejectReason::SourceLimit: return "source_limit";
        case RejectReason::LabelLimit: return "label_limit";
        case RejectReason::DepthLimit: return "depth_limit";
        case RejectReason::NodeLimit: return "node_limit";
    }
    return "unknown";
}

Target selectTarget(const std::string& text, const LpPlan& plan, int cursor) {
    Target target;
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        target.rejectReason = RejectReason::PlanNotReady;  // 超 int 索引域，拒绝而非截断
        return target;
    }
    const int textSize = static_cast<int>(text.size());
    if (cursor < 0 || cursor > textSize) {
        target.rejectReason = RejectReason::NoBlock;
        return target;
    }
    if (plan.lines.empty()) {
        target.rejectReason = RejectReason::PlanNotReady;
        return target;
    }
    const int idx = plan.lineIndexFor(cursor);
    const LpLine* line = plan.lineAt(idx);
    if (line == nullptr) {
        target.rejectReason = RejectReason::PlanNotReady;
        return target;
    }
    // cursor 行自身范围，以及 cursor 必须落在"本行 beg 到下行 beg"的归属区间内
    // （防损坏 LP 用乱序行把 cursor 指向远处的块）。
    if (line->srcBeg < 0 || line->srcEnd < line->srcBeg || line->srcEnd > textSize || cursor < line->srcBeg) {
        target.rejectReason = RejectReason::InvalidFence;
        return target;
    }
    if (idx + 1 < static_cast<int>(plan.lines.size()) &&
        cursor >= plan.lines[static_cast<std::size_t>(idx) + 1].srcBeg) {
        target.rejectReason = RejectReason::InvalidFence;
        return target;
    }
    if (line->codeBlockBeg < 0) {
        target.rejectReason = RejectReason::NoBlock;
        return target;
    }
    const int beg = line->codeBlockBeg;
    const int end = line->codeBlockEnd;
    if (end < 0) {
        target.rejectReason = RejectReason::UnclosedFence;
        return target;
    }
    // 恶意/损坏 LP 的区间必须先验边界，scanFence 才不越界。
    if (beg >= end || end > textSize) {
        target.rejectReason = RejectReason::InvalidFence;
        return target;
    }
    const int openIdx = plan.lineIndexFor(beg);
    const int closeIdx = plan.lineIndexFor(end);
    const LpLine* open = plan.lineAt(openIdx);
    const LpLine* close = plan.lineAt(closeIdx);
    if (open == nullptr || close == nullptr) {
        target.rejectReason = RejectReason::InvalidFence;
        return target;
    }
    if (!open->codeFence || open->srcBeg != beg || open->srcEnd <= open->srcBeg || open->srcEnd > end ||
        open->codeBlockBeg != beg) {
        target.rejectReason = RejectReason::InvalidFence;
        return target;
    }
    // lp_plan 收尾会把未闭合块的 codeBlockEnd 填成末行 srcEnd；末行不是闭围栏（或闭行
    // 不在开行之后）⇒ 实际未闭合。
    if (closeIdx <= openIdx || !close->codeFence) {
        target.rejectReason = RejectReason::UnclosedFence;
        return target;
    }
    if (close->srcBeg < open->srcEnd || close->srcBeg > end || close->srcEnd != end ||
        close->srcEnd <= close->srcBeg || close->codeBlockBeg != beg || close->codeBlockEnd != end ||
        open->codeBlockEnd != end) {
        target.rejectReason = RejectReason::InvalidFence;
        return target;
    }
    // 整块不得含 Tab（与语言/缩进口径一致，保守拒绝）。
    for (int p = beg; p < end; ++p) {
        if (text[static_cast<std::size_t>(p)] == '\t') {
            target.rejectReason = RejectReason::InvalidFence;
            return target;
        }
    }

    SLine openLine{static_cast<std::size_t>(open->srcBeg), static_cast<std::size_t>(open->srcEnd)};
    Fence openFence;
    if (!scanFence(text, openLine, openFence)) {
        target.rejectReason = RejectReason::InvalidFence;
        return target;
    }
    const std::string lang(text, openFence.infoBeg, openFence.infoEnd - openFence.infoBeg);
    if (lang != "mermaid") {
        target.rejectReason = RejectReason::UnsupportedLanguage;
        return target;
    }
    SLine closeLine{static_cast<std::size_t>(close->srcBeg), static_cast<std::size_t>(close->srcEnd)};
    Fence closeFence;
    if (!scanFence(text, closeLine, closeFence) || closeFence.ch != openFence.ch ||
        closeFence.run < openFence.run || closeFence.hasInfo) {
        target.rejectReason = RejectReason::UnclosedFence;  // 几何不成立 ⇒ 实际未闭合
        return target;
    }
    if (open->quoteDepth > 0 || open->listDepth > 0) {
        target.rejectReason = RejectReason::InvalidFence;  // quote/list 内嵌
        return target;
    }
    // 缩进到列表项正文列及以上的围栏属于列表项内部，不是顶级块。prev 区间必须自身有效，
    // 无效直接拒绝（不 clamp 猜）。
    for (int j = openIdx - 1; j >= 0; --j) {
        const LpLine* prev = plan.lineAt(j);
        if (prev == nullptr) {
            break;
        }
        if (prev->kind == LpKind::Blank) {
            continue;
        }
        if (!(prev->listDepth > 0 || prev->listItemLine >= 0)) {
            break;
        }
        if (prev->srcBeg < 0 || prev->srcEnd < prev->srcBeg || prev->srcEnd > textSize ||
            prev->contentBeg < prev->srcBeg || prev->contentBeg > prev->srcEnd) {
            target.rejectReason = RejectReason::InvalidFence;
            return target;
        }
        if (static_cast<int>(openFence.indent) >= prev->contentBeg - prev->srcBeg) {
            target.rejectReason = RejectReason::InvalidFence;
            return target;
        }
        break;
    }

    target.blockBeg = beg;
    target.blockEnd = end;
    target.rejectReason = RejectReason::None;
    return target;
}

ParseResult parseBlock(std::string_view block, std::size_t sourceBeg, const Limits& limits) {
    ParseResult result;
    const auto reject = [&result](RejectReason reason, std::size_t line) {
        result.plan = Plan{};
        result.rejectReason = reason;
        result.sourceLine = line;
        return result;
    };

    if (block.size() > limits.sourceBytes ||
        sourceBeg > std::numeric_limits<std::size_t>::max() - block.size()) {
        return reject(RejectReason::SourceLimit, 0);
    }
    std::size_t badLine = 0;
    if (!utf8Valid(block, badLine)) {
        return reject(RejectReason::InvalidUtf8, badLine);
    }
    if (const std::size_t tabLine = firstTabLine(block); tabLine != 0) {
        return reject(RejectReason::InvalidIndent, tabLine);
    }

    const std::vector<SLine> lines = splitLines(block);

    std::size_t openIdx = 0;
    while (openIdx < lines.size() && lineIsBlank(block, lines[openIdx])) {
        ++openIdx;
    }
    if (openIdx >= lines.size()) {
        return reject(RejectReason::InvalidFence, openIdx + 1);
    }
    Fence openFence;
    if (!scanFence(block, lines[openIdx], openFence)) {
        return reject(RejectReason::InvalidFence, openIdx + 1);
    }
    const sv lang = block.substr(openFence.infoBeg, openFence.infoEnd - openFence.infoBeg);
    if (lang != "mermaid") {
        return reject(RejectReason::UnsupportedLanguage, openIdx + 1);
    }

    std::size_t closeIdx = lines.size();
    for (std::size_t j = openIdx + 1; j < lines.size(); ++j) {
        Fence f;
        if (scanFence(block, lines[j], f) && f.ch == openFence.ch && f.run >= openFence.run && !f.hasInfo) {
            closeIdx = j;
            break;
        }
    }
    if (closeIdx == lines.size()) {
        return reject(RejectReason::UnclosedFence, lines.size());
    }
    for (std::size_t j = closeIdx + 1; j < lines.size(); ++j) {
        if (!lineIsBlank(block, lines[j])) {
            return reject(RejectReason::InvalidFence, j + 1);
        }
    }

    // 首条非空非注释内容行必须恰为 `mindmap`。
    std::size_t mindmapIdx = closeIdx;
    for (std::size_t j = openIdx + 1; j < closeIdx; ++j) {
        const SLine line = lines[j];
        if (lineIsBlank(block, line)) {
            continue;
        }
        const std::size_t lead = lineLead(block, line);
        const sv content = block.substr(lead, lineLastNonSpace(block, line, lead) - lead);
        if (content.empty()) {
            continue;
        }
        if (startsWith(content, "%%")) {
            if (startsWith(content, "%%{")) {
                return reject(RejectReason::UnsupportedSyntax, j + 1);
            }
            continue;
        }
        if (content == "mindmap") {
            mindmapIdx = j;
        } else {
            return reject(RejectReason::NotMindmap, j + 1);
        }
        break;
    }
    if (mindmapIdx == closeIdx) {
        return reject(RejectReason::NotMindmap, 0);
    }

    Plan plan;
    std::vector<int> stackNodes;
    std::vector<int> stackIndents;
    std::unordered_set<std::string> seenIds;

    for (std::size_t j = mindmapIdx + 1; j < closeIdx; ++j) {
        const SLine line = lines[j];
        if (lineIsBlank(block, line)) {
            continue;
        }
        const std::size_t lead = lineLead(block, line);
        const sv content = block.substr(lead, lineLastNonSpace(block, line, lead) - lead);
        if (content.empty()) {
            continue;
        }
        if (startsWith(content, "%%")) {
            if (startsWith(content, "%%{")) {
                return reject(RejectReason::UnsupportedSyntax, j + 1);
            }
            continue;
        }

        NodeSpec spec;
        const RejectReason specReason = parseNode(content, lead, spec);
        if (specReason != RejectReason::None) {
            return reject(specReason, j + 1);
        }
        if (countCodepoints(spec.label) > limits.labelCodepoints) {
            return reject(RejectReason::LabelLimit, j + 1);
        }
        if (plan.nodes.size() >= limits.nodes) {
            return reject(RejectReason::NodeLimit, j + 1);
        }

        const int indent = static_cast<int>(lead - line.beg);
        int parent = -1;
        std::size_t depth = 0;
        if (stackNodes.empty()) {
            stackIndents.push_back(indent);
            stackNodes.push_back(static_cast<int>(plan.nodes.size()));
        } else if (indent > stackIndents.back()) {
            parent = stackNodes.back();
            depth = stackNodes.size();
            stackIndents.push_back(indent);
            stackNodes.push_back(static_cast<int>(plan.nodes.size()));
        } else if (indent == stackIndents.back()) {
            if (stackNodes.size() == 1) {
                return reject(RejectReason::MultipleRoots, j + 1);
            }
            parent = stackNodes[stackNodes.size() - 2];
            depth = stackNodes.size() - 1;
            stackNodes.back() = static_cast<int>(plan.nodes.size());
        } else {
            while (!stackIndents.empty() && stackIndents.back() > indent) {
                stackIndents.pop_back();
                stackNodes.pop_back();
            }
            if (stackIndents.empty() || stackIndents.back() != indent) {
                return reject(RejectReason::InvalidIndent, j + 1);
            }
            if (stackNodes.size() == 1) {
                return reject(RejectReason::MultipleRoots, j + 1);
            }
            parent = stackNodes[stackNodes.size() - 2];
            depth = stackNodes.size() - 1;
            stackNodes.back() = static_cast<int>(plan.nodes.size());
        }
        if (depth > limits.depth) {
            return reject(RejectReason::DepthLimit, j + 1);
        }
        if (spec.hasId && !seenIds.insert(spec.id).second) {
            return reject(RejectReason::DuplicateId, j + 1);
        }

        Node node;
        node.label = std::move(spec.label);
        node.id = spec.hasId ? std::move(spec.id) : std::string();
        node.shape = spec.shape;
        node.parent = parent;
        node.depth = depth;
        node.sourceBeg = sourceBeg + spec.labelBeg;
        node.sourceEnd = sourceBeg + spec.labelEnd;
        plan.nodes.push_back(std::move(node));
        const int idx = static_cast<int>(plan.nodes.size()) - 1;
        if (parent >= 0) {
            plan.nodes[static_cast<std::size_t>(parent)].children.push_back(idx);
        }
    }

    if (plan.nodes.empty()) {
        return reject(RejectReason::EmptyMap, 0);
    }
    plan.sourceBeg = sourceBeg;
    plan.sourceEnd = sourceBeg + block.size();
    result.plan = std::move(plan);
    result.rejectReason = RejectReason::None;
    result.sourceLine = 0;
    return result;
}

std::size_t estimatePlanBytes(const Plan& plan) {
    std::size_t bytes = sizeof(Plan);
    bytes += plan.nodes.capacity() * sizeof(Node);
    for (const Node& node : plan.nodes) {
        bytes += node.label.capacity();
        bytes += node.id.capacity();
        bytes += node.children.capacity() * sizeof(int);
    }
    return bytes;
}

}  // namespace neo::mindmap
