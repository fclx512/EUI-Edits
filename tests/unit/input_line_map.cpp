// Live Preview 排版核心的无头验收（S1）。
//
// 覆盖调研文档 §5 S1 列的判据：
//   ① 光标永不落在隐藏区间内；
//   ② 命中 ↔ 光标往返一致；
//   ③ contentHeight == Σ heights；
//   ④ 上下移动跨不同行高时水平位置按 preferredX 保持；
//   另加：装饰真的生效（行高/字号/投影文本都按装饰走）、投影数学的自洽性。
//
// 全部走真实 FreeType 测量，不需要窗口、不需要截图。

#include "components/input.h"
#include "components/input_model.h"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace detail = components::input_detail;
using Model = detail::InputModel;
using LineHole = detail::LineHole;
using LineDecoration = detail::LineDecoration;
using LineRun = detail::LineRun;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

bool near(float a, float b, float tolerance = 0.01f) {
    return std::fabs(a - b) <= tolerance;
}

// 按 '\n' 切出的物理行区间（不含换行符）。
std::vector<std::pair<int, int>> splitLines(const std::string& text) {
    std::vector<std::pair<int, int>> lines;
    int start = 0;
    for (int i = 0; i <= static_cast<int>(text.size()); ++i) {
        if (i == static_cast<int>(text.size()) || text[static_cast<std::size_t>(i)] == '\n') {
            lines.push_back({start, i});
            start = i + 1;
        }
    }
    return lines;
}

int lineIndexAt(const std::vector<std::pair<int, int>>& lines, int cursor) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (cursor >= lines[i].first && cursor <= lines[i].second) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(lines.size()) - 1;
}

// 很土的标记扫描：整行扫一遍，遇到 "**" 或 "`" 就记一段。测试用，够精确。
void collectInlineHoles(const std::string& text, int beg, int end, std::vector<LineHole>& out) {
    int cursor = beg;
    while (cursor < end) {
        if (text[static_cast<std::size_t>(cursor)] == '*' && cursor + 1 < end &&
            text[static_cast<std::size_t>(cursor + 1)] == '*') {
            out.push_back({cursor, cursor + 2});
            cursor += 2;
            continue;
        }
        if (text[static_cast<std::size_t>(cursor)] == '`') {
            out.push_back({cursor, cursor + 1});
            ++cursor;
            continue;
        }
        ++cursor;
    }
}

// 容器的行首标记（"# " / "> "）。
void collectLeadHoles(const std::string& text, int beg, int end, std::vector<LineHole>& out) {
    if (end - beg >= 2 && (text[static_cast<std::size_t>(beg)] == '#' ||
                           text[static_cast<std::size_t>(beg)] == '>') &&
        text[static_cast<std::size_t>(beg) + 1] == ' ') {
        out.push_back({beg, beg + 2});
    }
}

// provider 只吃 text（组件靠**内容比较**决定要不要重排），"活动块"由 provider 自己按当前光标算。
// 测试里用一个全局变量充当"当前光标"。
int g_testCursor = 0;

// 测试用的装饰规则：
//   第 0 行是标题（24px / 行高 30），其余行用控件默认字号；
//   光标所在的那一行**不隐藏任何标记**（"光标所在块显示原始源码"）。
std::vector<LineDecoration> decorationsFor(const std::string& text) {
    const std::vector<std::pair<int, int>> lines = splitLines(text);
    const int activeLine = lineIndexAt(lines, g_testCursor);
    std::vector<LineDecoration> table;
    table.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        LineDecoration decoration;
        if (i == 0) {
            decoration.fontSize = 24.0f;
            decoration.lineHeight = 30.0f;
        }
        if (static_cast<int>(i) != activeLine) {
            collectLeadHoles(text, lines[i].first, lines[i].second, decoration.holes);
            collectInlineHoles(text, lines[i].first, lines[i].second, decoration.holes);
        }
        table.push_back(std::move(decoration));
    }
    return table;
}

// ── ① 逐行几何表 ────────────────────────────────────────────────────────────
void testGeometryTable() {
    detail::LineGeometryTable table;
    table.build({20.0f, 40.0f, 20.0f, 20.0f});
    check(near(table.total(), 100.0f), "total 应等于各行高之和");
    check(table.count() == 4, "行数");
    check(near(table.top(0), 0.0f), "top(0)");
    check(near(table.top(1), 20.0f), "top(1) 承接第 0 行行高");
    check(near(table.top(2), 60.0f), "top(2)");
    check(near(table.bottom(1), 60.0f), "bottom(1) == top(1) + height(1)");

    check(table.lineAtY(0.0f) == 0 && table.lineAtY(19.9f) == 0, "y=19.9 落在第 0 行");
    check(table.lineAtY(20.0f) == 1 && table.lineAtY(59.9f) == 1, "y=59.9 落在第 1 行");
    check(table.lineAtY(60.0f) == 2, "y=60 落在第 2 行");
    check(table.lineAtY(1e6f) == 3, "越界 y 夹到最后一行");
    check(table.lineAtY(-100.0f) == 0, "负 y 夹到第一行");
    check(table.firstVisibleLine(60.0f) == 1, "可视窗口往上多留一行");
    check(table.lastVisibleLine(0.0f, 60.0f) == 3, "可视窗口往下多留一行（夹在末行）");

    for (int i = 0; i + 1 < table.count(); ++i) {
        check(near(table.top(i + 1), table.top(i) + table.height(i)),
              "top 必须是 height 的前缀和（行 " + std::to_string(i) + "）");
    }

    // 回归：2001 行 × 19.2f 逐行 float 累加会漂 0.8px，滚动条的滑块尺寸和最大滚动量都吃这个数。
    detail::LineGeometryTable longTable;
    longTable.build(std::vector<float>(2001, 19.2f));
    check(near(longTable.total(), 2001.0f * 19.2f, 0.01f),
          "长文档的前缀和不得漂移（float 累加会差 0.8px）");
}

// ── ①b 块间距（R1）：gap 落在行的**上方**，不改变任何一行自己的高度 ──────────
void testGeometryTableWithGaps() {
    // 3 行，第 1、2 行前各有 16 / 8 的块间距（第 0 行的 gap 必须被忽略）。
    detail::LineGeometryTable table;
    table.build({20.0f, 20.0f, 20.0f}, {16.0f, 16.0f, 8.0f});
    check(near(table.top(0), 0.0f), "首行的 spaceBefore 必须忽略（文档顶端无外间距）");
    check(near(table.top(1), 36.0f), "top(1) = 首行高 + 自己的 gap");
    check(near(table.top(2), 64.0f), "top(2) 继续累加 gap");
    check(near(table.total(), 84.0f), "total 含全部 gap");
    // gap 不改变行盒：bottom(i) 依然是 top(i) + height(i)。
    check(near(table.bottom(1), 56.0f), "gap 不得计进行盒（bottom = top + height）");
    check(near(table.height(1), 20.0f), "行高不受 gap 影响");

    // gap 区域（[36, 56) 之外的 [56, 64)）命中**上一个**行索引。
    check(table.lineAtY(35.9f) == 0, "gap 之前仍落在上一行");
    check(table.lineAtY(40.0f) == 1, "行盒内落在本行");
    check(table.lineAtY(57.0f) == 1, "gap 区域（下一行 top 之前）命中上一行");
    check(table.lineAtY(63.9f) == 1, "gap 末尾仍是上一行");
    check(table.lineAtY(64.0f) == 2, "下一行 top 起落在下一行");

    // 全 0 gap 时与旧重载逐位一致（等价性不变量）。
    detail::LineGeometryTable plain;
    detail::LineGeometryTable zeroGaps;
    const std::vector<float> heights = {24.0f, 31.06f, 24.0f, 21.0f};
    plain.build(heights);
    zeroGaps.build(heights, std::vector<float>(heights.size(), 0.0f));
    bool same = plain.count() == zeroGaps.count() && near(plain.total(), zeroGaps.total(), 0.001f);
    for (int i = 0; i < plain.count() && same; ++i) {
        same = near(plain.top(i), zeroGaps.top(i)) && near(plain.height(i), zeroGaps.height(i));
    }
    check(same, "全 0 gap 必须与旧重载逐位一致");
}

// ── ② 投影数学 ──────────────────────────────────────────────────────────────
void testProjectionMath() {
    // "abcdefghijklm"，隐藏 [2,5) 与 [10,12)。
    const std::vector<LineHole> holes = {{2, 5}, {10, 12}};
    const std::string text = "abcdefghijklm";

    check(detail::projectText(text, 0, 13, holes) == "abfghijm", "投影文本 = 原文去掉隐藏区间");
    check(detail::visibleLength(holes, 0, 2) == 2, "洞之前可见长度为 2");
    check(detail::visibleLength(holes, 0, 5) == 2, "洞内部不增加可见长度");
    check(detail::visibleLength(holes, 0, 6) == 3, "洞之后恢复增长");
    check(detail::visibleLength(holes, 0, 13) == 8, "总可见长度 = 原文 - 隐藏");

    check(!detail::insideHole(holes, 2), "洞的起点是可见位置");
    check(detail::insideHole(holes, 3), "洞内部");
    check(!detail::insideHole(holes, 5), "洞的终点是可见位置");
    check(!detail::insideHole(holes, 6), "洞之后");

    check(detail::snapVisible(holes, 4) == 2, "洞内吸附到洞之前");
    check(detail::nextVisibleOffset(holes, 2) == 5, "右移跨过整个洞");
    check(detail::nextVisibleOffset(holes, 1) == 1, "洞之外右移不变");
    check(detail::prevVisibleOffset(holes, 5) == 2, "左移跨过整个洞");
    check(detail::prevVisibleOffset(holes, 6) == 6, "洞之外左移不变");

    // 投影是"多对一"的：洞的前后两个间隙在屏幕上是同一个位置（洞的视觉宽度为 0）。
    // 所以判据不是"原地返回"，而是"回来后投影偏移不变，且结果一定可见"。
    for (int offset = 0; offset <= static_cast<int>(text.size()); ++offset) {
        if (detail::insideHole(holes, offset)) {
            continue;
        }
        const int projected = detail::visibleLength(holes, 0, offset);
        const int back = detail::unprojectVisible(holes, 0, projected);
        check(!detail::insideHole(holes, back), "往返结果必须可见 @" + std::to_string(offset));
        check(detail::visibleLength(holes, 0, back) == projected,
              "往返后投影偏移不变 @" + std::to_string(offset));
    }
    check(detail::visibleLength(holes, 0, 2) == detail::visibleLength(holes, 0, 5),
          "洞的前后间隙投影到同一视觉位置");
    check(detail::unprojectVisible(holes, 0, detail::visibleLength(holes, 0, 2)) == 2,
          "折叠时取洞前那个间隙");
    // 反投影的结果必须永远可见。
    for (int projected = 0; projected <= 8; ++projected) {
        check(!detail::insideHole(holes, detail::unprojectVisible(holes, 0, projected)),
              "反投影结果必须落在可见位置 @" + std::to_string(projected));
    }

    // 非零 anchor：行不是从文档开头开始的（软换行段）。
    check(detail::visibleLength(holes, 5, 13) == 6, "anchor 之后的可见长度");
    check(detail::unprojectVisible(holes, 5, 0) == 5, "anchor 处的投影偏移映射回 anchor");
    check(detail::unprojectVisible(holes, 5, 1) == 6, "anchor 之后逐字节前进");

    // 规范化：乱序、重叠、空区间都要收拾干净。
    const std::vector<LineHole> messy = {{10, 12}, {2, 5}, {11, 14}, {7, 7}};
    const std::vector<LineHole> tidy = detail::normalizeHoles(messy);
    check(tidy.size() == 2, "重叠区间合并后只剩两段");
    check(tidy[0].beg == 2 && tidy[0].end == 5, "第一段");
    check(tidy[1].beg == 10 && tidy[1].end == 14, "第二段吸收重叠");

    const std::vector<LineHole> clipped = detail::clipHoles(holes, 3, 11);
    check(clipped.size() == 2 && clipped[0].beg == 3 && clipped[0].end == 5, "裁剪左边界");
    check(clipped[1].beg == 10 && clipped[1].end == 11, "裁剪右边界");
}

// ── ③④⑤ 排版核心 ───────────────────────────────────────────────────────────
void testInputLayout() {
    const std::string document =
        "# Title\n"
        "plain text\n"
        "**bold** and `code`\n"
        "> quoted **deep**\n";

    const float inset = 10.0f;
    const float width = 420.0f;
    const float height = 200.0f;
    const float fontSize = 16.0f;

    core::dsl::Ui ui;
    // 装饰由 provider 按"当前光标"算（测试里用 g_testCursor 模拟），所以设好它再构建即可。
    auto compose = [&](int cursor) {
        g_testCursor = cursor;
        ui.begin("lp");
        components::input(ui, "md")
            .size(width, height)
            .inset(inset)
            .fontSize(fontSize)
            .fontFamily("monospace")
            .multiline()
            .value(document)
            .lineDecorator(decorationsFor)
            .build();
        ui.end();
        ui.layout(width, height);
    };

    compose(0);
    Model::InputState& state = ui.state<Model::InputState>("md");
    const std::vector<Model::InputLayout::Line>& lines = state.cachedLines;

    check(!lines.empty(), "排版结果非空");
    check(lines.size() == 5, "文档有 4 个换行符，应排出 5 个物理行（末尾一行为空）");

    // ── ③ contentHeight == Σ heights ──
    float sumHeights = 0.0f;
    for (const auto& line : lines) {
        sumHeights += line.lineHeight;
    }
    check(near(state.cachedGeometry.total(), sumHeights, 0.5f), "contentHeight == Σ heights");
    check(near(state.cachedGeometry.total(), 30.0f + 19.2f * 4.0f, 0.5f),
          "总高 = 标题行 30 + 4 行正文 19.2");

    // 装饰确实按行生效
    check(near(lines[0].fontSize, 24.0f), "第 0 行用装饰字号");
    check(near(lines[0].lineHeight, 30.0f), "第 0 行用装饰行高");
    check(near(lines[1].fontSize, fontSize), "第 1 行沿用控件字号");
    check(near(lines[1].lineHeight, fontSize * 1.2f), "第 1 行沿用控件行高");

    // ── ① 隐藏区间本身合法，且光标恒不落在其中 ──
    for (const auto& line : lines) {
        for (const LineHole& hole : line.holes) {
            check(hole.beg >= line.start && hole.end <= line.end,
                  "隐藏区间必须落在本行内");
            check(!hole.empty(), "隐藏区间非空");
        }
        for (int offset = line.start; offset <= line.end; ++offset) {
            // 光标吸附后再读 x，绝不该读到"洞里的字节"。
            const int snapped = detail::snapVisible(line.holes, offset);
            check(!detail::insideHole(line.holes, snapped), "吸附后的光标必须可见");
        }
        for (const LineHole& hole : line.holes) {
            check(detail::snapVisible(line.holes, hole.beg + (hole.end - hole.beg) / 2) == hole.beg,
                  "洞内任意位置都吸附到洞之前");
        }
    }

    // 命中（点鼠标）在任意 x 上都不能落进隐藏区间
    for (int step = -40; step <= 200; ++step) {
        const float targetX = static_cast<float>(step) * 3.0f;
        for (const auto& line : lines) {
            const int offset = detail::docOffsetForX(line.metrics, line.holes, line.start, targetX);
            check(!detail::insideHole(line.holes, offset), "命中结果不在隐藏区间内");
            check(offset >= line.start && offset <= line.end, "命中结果落在本行内");
        }
    }

    // ── ② 命中 ↔ 光标往返一致 ──
    for (const auto& line : lines) {
        for (int projected : line.metrics.byteIndices) {
            const int offset = detail::unprojectVisible(line.holes, line.start, projected);
            if (detail::insideHole(line.holes, offset)) {
                continue;
            }
            const float x = Model::caretXInLine(line, offset);
            check(near(x, detail::caretXInMetrics(line.metrics, projected), 0.01f),
                  "光标 x 与投影偏移一致 @" + std::to_string(offset));
            const int back = detail::docOffsetForX(line.metrics, line.holes, line.start, x);
            check(back == offset,
                  "命中↔光标往返一致：offset=" + std::to_string(offset) +
                      " 命中回 " + std::to_string(back));
        }
    }

    // 左右移动：每一步都必须落在可见位置（整段跨过隐藏标记）
    {
        // 用组件自己的 viewport 宽度，保证这些入口命中同一份排版缓存而不是重排一遍。
        const float viewport = state.cachedViewportWidth;
        const int savedCursor = state.cursor;
        for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            state.cursor = lines[lineIndex].start;
            for (int step = 0; step < 64; ++step) {
                const int next = Model::nextCursorIndex(state, "monospace", fontSize, true, viewport);
                if (next == state.cursor) {
                    break;
                }
                state.cursor = next;
                const int landed = Model::lineIndexFor(lines, state.cursor);
                check(!detail::insideHole(lines[static_cast<std::size_t>(landed)].holes, state.cursor),
                      "右移不得停在隐藏区间内 @" + std::to_string(state.cursor));
            }
        }
        for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            state.cursor = lines[lineIndex].end;
            for (int step = 0; step < 64; ++step) {
                const int previous = Model::prevCursorIndex(state, "monospace", fontSize, true, viewport);
                if (previous == state.cursor) {
                    break;
                }
                state.cursor = previous;
                const int landed = Model::lineIndexFor(lines, state.cursor);
                check(!detail::insideHole(lines[static_cast<std::size_t>(landed)].holes, state.cursor),
                      "左移不得停在隐藏区间内 @" + std::to_string(state.cursor));
            }
        }
        state.cursor = savedCursor;
    }

    // ── ④ 垂直移动：跨不同行高时按 preferredX 保持水平位置 ──
    {
        const float viewport = state.cachedViewportWidth;
        state.cursor = lines[1].start + 4;  // 正文行第 4 个字节
        const float originX = Model::caretXInLine(lines[1], state.cursor);
        state.hasPreferredCursorX = false;
        Model::moveCursorVertical(state, -1, false, "monospace", fontSize, viewport, height);
        check(state.hasPreferredCursorX, "垂直移动应记录 preferredCursorX");
        check(near(state.preferredCursorX, originX), "preferredCursorX 取自出发行的 x");
        check(!detail::insideHole(lines[0].holes, state.cursor), "上行落点不得在隐藏区间内");

        Model::moveCursorVertical(state, 1, false, "monospace", fontSize, viewport, height);
        const int backLine = Model::lineIndexFor(lines, state.cursor);
        check(backLine == 1, "下移应回到原来那一行");
        check(near(Model::caretXInLine(lines[1], state.cursor), originX, 0.5f),
              "回到原行时水平位置复原");
    }

    // ── 渲染侧：图元用的是投影文本、逐行字号与逐行行高 ──
    compose(21);  // 光标落在第 2 行（"**bold** and `code`" 内）：该行显示源码，其余行隐藏标记
    const core::dsl::Element* title = ui.find("md.text.0");
    const core::dsl::Element* plain = ui.find("md.text.1");
    const core::dsl::Element* inlineCode = ui.find("md.text.2");
    const core::dsl::Element* quote = ui.find("md.text.3");
    check(title != nullptr && plain != nullptr && inlineCode != nullptr && quote != nullptr,
          "各行都有文本图元");
    if (title != nullptr && plain != nullptr && inlineCode != nullptr && quote != nullptr) {
        check(title->text == "Title", "标题行渲染投影文本（'# ' 已隐藏）");
        check(inlineCode->text == "**bold** and `code`", "活动行渲染原始源码");
        check(quote->text == "quoted deep", "引用行的容器与行内标记都隐藏");

        check(near(title->fontSize, 24.0f), "标题行图元用装饰字号");
        check(near(title->lineHeight, 30.0f), "标题行图元用装饰行高");
        check(near(plain->fontSize, fontSize), "正文行图元用控件字号");
        check(near(plain->lineHeight, fontSize * 1.2f), "正文行图元用控件行高");

        check(near(plain->frame.y - title->frame.y,
                   30.0f + (fontSize * 1.2f - fontSize) * 0.5f - (30.0f - 24.0f) * 0.5f, 0.5f),
              "文本原点按各自行盒的居中留白推进，行盒仍相接");
        check(near(quote->frame.y - inlineCode->frame.y, fontSize * 1.2f, 0.5f),
              "正文行之间按 19.2px 推进");
    }
}

// ── ⑥ 装饰 × 软换行：折行边界与隐藏区间相邻时的段划分 ──
// 折行是在"投影文本"上切的，再把切点映射回文档偏移；这里验证段之间既不重叠也不留缝，
// 也就是"把各段渲染出来的字拼起来"必须等于"整行去掉隐藏内容"。
void testDecoratedSoftWrap() {
    std::string text;
    for (int i = 0; i < 60; ++i) {
        text += "word";
        if (i + 1 < 60) {
            text += ' ';
        }
    }
    // 一个行内代码标记，位置落在中段，逼折行边界与它相邻。
    text.insert(120, "`");
    text.insert(124, "`");
    const std::vector<LineHole> holes = {{120, 121}, {124, 125}};

    const float inset = 10.0f;
    const float width = 240.0f;
    const float height = 300.0f;
    core::dsl::Ui ui;
    ui.begin("wrap");
    components::input(ui, "wrapped")
        .size(width, height)
        .inset(inset)
        .fontSize(16.0f)
        .fontFamily("monospace")
        .multiline()
        .value(text)
        .lineDecorator([holes](const std::string&) {
            std::vector<LineDecoration> table(1);
            table[0].holes = holes;
            return table;
        })
        .build();
    ui.end();
    ui.layout(width, height);

    Model::InputState& state = ui.state<Model::InputState>("wrapped");
    const auto& lines = state.cachedLines;
    check(lines.size() > 1, "长行 + 装饰应当折行");

    std::string joined;
    for (const auto& line : lines) {
        check(line.metrics.width <= state.cachedViewportWidth + 0.5f,
              "每段的投影宽度不超过视口");
        for (const LineHole& hole : line.holes) {
            check(hole.beg >= line.start && hole.end <= line.end, "段的隐藏区间落在段内");
        }
        joined += detail::projectText(text, line.start, line.end, line.holes);
    }
    for (std::size_t i = 1; i < lines.size(); ++i) {
        check(lines[i].start >= lines[i - 1].end, "段之间不重叠");
    }

    std::string expected = text;
    expected.erase(124, 1);
    expected.erase(120, 1);
    check(joined == expected, "各段投影文本拼起来 = 整行去掉隐藏内容");
    check(state.cachedGeometry.total() > 0.0f, "折行后仍有正的总高度");
}

// ── ⑦ 行内样式段：逐段测量拼接出来的 caret 表必须自洽 ──
// 一行里同时有普通文字、粗体、行内代码（三种字体参数），验证：
//   * 各段首尾相接、覆盖整行可见文本，x 单调，宽度之和 = 整行宽度；
//   * caret 表与"整行一次性测量"等价（偏移升序、末位 = 可见长度、首位 x = 0）；
//   * 命中 ↔ 光标往返一致（样式段不破坏 S1 的那条不变量）；
//   * 段的 x 与 caret 表对得上（否则视图画出来的位置和光标不重合）。
void testStyledRuns() {
    const std::string text = "plain **BOLD** tail `code` end";
    const std::size_t boldMarkBeg = text.find("**");
    const std::size_t boldBeg = boldMarkBeg + 2;
    const std::size_t boldEnd = text.find("**", boldMarkBeg + 2);
    const std::size_t codeMarkBeg = text.find('`');
    const std::size_t codeBeg = codeMarkBeg + 1;
    const std::size_t codeEnd = text.find('`', codeMarkBeg + 1);
    check(boldEnd != std::string::npos && codeEnd != std::string::npos, "样例里应当能定位到粗体与行内代码");

    LineRun strong;
    strong.beg = static_cast<int>(boldBeg);
    strong.end = static_cast<int>(boldEnd);
    strong.style.weight = 700;
    LineRun code;
    code.beg = static_cast<int>(codeBeg);
    code.end = static_cast<int>(codeEnd);
    code.style.fontFamily = "monospace";
    code.style.background = core::Color(0.1f, 0.1f, 0.12f, 1.0f);

    const float inset = 10.0f;
    const float width = 900.0f;   // 足够宽：本用例不折行
    const float height = 200.0f;
    core::dsl::Ui ui;
    ui.begin("styled");
    components::input(ui, "styled")
        .size(width, height)
        .inset(inset)
        .fontSize(16.0f)
        .fontFamily("Microsoft YaHei")
        .multiline()
        .value(text)
        .lineDecorator([strong, code](const std::string&) {
            std::vector<LineDecoration> table(1);
            table[0].runs = {strong, code};
            return table;
        })
        .build();
    ui.end();
    ui.layout(width, height);

    Model::InputState& state = ui.state<Model::InputState>("styled");
    const auto& lines = state.cachedLines;
    check(!lines.empty(), "样式段场景应当排出至少一行");
    if (lines.empty()) {
        return;
    }
    const auto& line = lines.front();
    check(!line.runs.empty(), "有样式段的行必须产出可渲染的段");

    // ① 段首尾相接、覆盖整行可见文本、x 单调
    std::string joined;
    float previousRight = -1.0f;
    for (const auto& run : line.runs) {
        check(near(run.x, previousRight < 0.0f ? 0.0f : previousRight, 0.5f),
              "段之间不留缝也不重叠（x 应接上一段的右缘）");
        check(run.width > 0.0f, "每段都应当有正宽度");
        joined += detail::projectText(text, run.beg, run.end, line.holes);
        previousRight = run.x + run.width;
    }
    check(joined == text, "各段文字拼起来 = 本行可见文本（实际=\"" + joined + "\"）");
    check(near(previousRight, line.metrics.width, 0.5f), "最后一段的右缘 = 整行宽度");

    // ② caret 表：偏移升序、首位 0/0、末位 = 可见长度
    check(!line.metrics.byteIndices.empty(), "caret 表不应为空");
    check(line.metrics.byteIndices.front() == 0 && near(line.metrics.caretX.front(), 0.0f),
          "caret 表从 0 偏移 / x=0 开始");
    check(line.metrics.byteIndices.back() == static_cast<int>(text.size()),
          "caret 表覆盖到整行末尾");
    check(near(line.metrics.caretX.back(), line.metrics.width, 0.01f), "末位 caret x = 整行宽度");
    for (std::size_t i = 1; i < line.metrics.byteIndices.size(); ++i) {
        check(line.metrics.byteIndices[i] > line.metrics.byteIndices[i - 1], "caret 偏移严格递增");
    }

    // ③ 段的 x 必须与 caret 表一致（视图按段 x 画、光标按 caret 表画，两者必须重合）
    for (const auto& run : line.runs) {
        check(near(detail::caretXInMetrics(line.metrics, run.beg - line.start), run.x, 0.5f),
              "段起点 x 与 caret 表一致 @" + std::to_string(run.beg));
    }

    // ④ 命中 ↔ 光标往返一致（遍历全部可见偏移）
    for (int offset = 0; offset <= static_cast<int>(text.size()); ++offset) {
        const float x = detail::caretXForDocOffset(line.metrics, line.holes, line.start, offset);
        const int hit = detail::docOffsetForX(line.metrics, line.holes, line.start, x);
        check(hit == offset, "样式段场景下命中往返一致 @" + std::to_string(offset) +
                                 "（得到 " + std::to_string(hit) + "）");
    }

    // ⑤ 粗体不是空操作：同一串字用 700 量出来的宽度不应小于 400（本机若无粗体文件则相等）
    const float plainWidth = Model::measureMetrics("BOLD", "Microsoft YaHei", 16.0f).width;
    const float boldWidth = Model::measureMetrics("BOLD", "Microsoft YaHei", 16.0f, 700).width;
    std::cerr << "[info] BOLD 宽度：常规=" << plainWidth << " 粗体=" << boldWidth << "\n";
    check(boldWidth >= plainWidth - 0.01f, "粗体度量不应比常规更窄");
}

// ── ⑧ 样式段 × 软换行：折行后每段的段划分与文字都不能漏 ──
void testStyledRunsSoftWrap() {
    std::string text;
    for (int i = 0; i < 60; ++i) {
        text += "word";
        if (i + 1 < 60) {
            text += ' ';
        }
    }
    // 在偏后位置放一段粗体，逼折行边界穿过/贴近样式段。
    const std::size_t runBeg = 200;
    const std::size_t runEnd = 208;  // "word wor"
    LineRun strong;
    strong.beg = static_cast<int>(runBeg);
    strong.end = static_cast<int>(runEnd);
    strong.style.weight = 700;

    const float inset = 10.0f;
    const float width = 240.0f;
    const float height = 300.0f;
    core::dsl::Ui ui;
    ui.begin("styledwrap");
    components::input(ui, "styledwrap")
        .size(width, height)
        .inset(inset)
        .fontSize(16.0f)
        .fontFamily("monospace")
        .multiline()
        .value(text)
        .lineDecorator([strong](const std::string&) {
            std::vector<LineDecoration> table(1);
            table[0].runs = {strong};
            return table;
        })
        .build();
    ui.end();
    ui.layout(width, height);

    Model::InputState& state = ui.state<Model::InputState>("styledwrap");
    const auto& lines = state.cachedLines;
    check(lines.size() > 1, "样式段场景下的长行应当折行");

    std::string joined;
    float coveredBold = 0.0f;
    for (const auto& line : lines) {
        check(line.metrics.width <= state.cachedViewportWidth + 0.5f, "折行后每段宽度不超过视口");
        std::string lineText;
        float previousRight = -1.0f;
        for (const auto& run : line.runs) {
            check(run.beg >= line.start && run.end <= line.end, "样式段落在本段内");
            check(near(run.x, previousRight < 0.0f ? 0.0f : previousRight, 0.5f), "段之间不留缝");
            previousRight = run.x + run.width;
            const std::string piece = detail::projectText(text, run.beg, run.end, line.holes);
            lineText += piece;
            if (run.style.weight >= 700) {
                coveredBold += static_cast<float>(piece.size());
            }
        }
        check(near(previousRight, line.metrics.width, 0.5f), "段的右缘 = 本段宽度");
        joined += detail::projectText(text, line.start, line.end, line.holes);
    }
    check(joined == text, "折行后各段文字拼起来仍等于整行");
    check(near(coveredBold, static_cast<float>(runEnd - runBeg), 0.5f),
          "粗体段的字节数应当被完整保住（没有被折行吃掉）");
}

// ── ⑨ 行首图元（任务复选框）：整行文字右移，字节索引不动 ──
// applyLineGlyph 是纯粹的坐标平移，所以直接构造一条排版结果验证，
// 不必跑 FreeType 测量 —— 要判的只有"该移的都移了、不该动的一个都没动"。
void testLineGlyph() {
    Model::InputLayout::Line line;
    line.fontSize = 16.0f;
    line.lineHeight = 28.0f;
    line.metrics.width = 100.0f;
    line.metrics.byteIndices = {0, 3, 6};
    line.metrics.caretX = {0.0f, 25.0f, 50.0f};
    detail::TextRun run;
    run.beg = 0;
    run.end = 6;
    run.x = 0.0f;
    run.width = 50.0f;
    line.runs.push_back(run);

    detail::LineGlyph glyph;
    glyph.codepoint = 0xF0C8;
    Model::applyLineGlyph(line, glyph);

    const float advance = line.glyph.advance;
    check(advance > 15.0f && advance < 30.0f,
          "默认占位宽度应约 1.45 em（字号 16 → 23 上下），实际 " + std::to_string(advance));
    check(near(line.metrics.width, 100.0f + advance), "图元应把整行宽度加一个 advance");
    check(line.metrics.caretX.size() == 3 && near(line.metrics.caretX[0], advance) &&
              near(line.metrics.caretX[1], 25.0f + advance) &&
              near(line.metrics.caretX[2], 50.0f + advance),
          "每个 caret 位置都应右移一个 advance");
    check(line.metrics.byteIndices.size() == 3 && line.metrics.byteIndices[0] == 0 &&
              line.metrics.byteIndices[1] == 3 && line.metrics.byteIndices[2] == 6,
          "字节索引不应因图元改变（图元不对应文档里的任何字节）");
    check(line.runs.size() == 1 && near(line.runs[0].x, advance), "行内样式段也应右移");
    check(line.glyph.codepoint == 0xF0C8, "图元应被记到该行上");

    // 显式指定 advance 时按指定值走（给"图标比默认更宽/更窄"留口子）。
    Model::InputLayout::Line explicitLine;
    explicitLine.fontSize = 16.0f;
    explicitLine.metrics.width = 10.0f;
    detail::LineGlyph explicitGlyph;
    explicitGlyph.codepoint = 0xF14A;
    explicitGlyph.advance = 30.0f;
    Model::applyLineGlyph(explicitLine, explicitGlyph);
    check(near(explicitLine.glyph.advance, 30.0f) && near(explicitLine.metrics.width, 40.0f),
          "显式 advance 应原样生效");
}

// ── ⑩ 行号槽判据（T19 视觉修正 1）：3px 表格分隔行不画行号 ──
// 判据抽在 model（InputModel::gutterLineUsable），input.h 的行号循环只负责调它。
// 这里两层都打：纯判据的每个分支 + 真实排版里分隔行的几何高度确实只有 3px。
void testGutterLineUsable() {
    const float fontSize = 16.0f;

    // ── 纯判据 ──
    Model::InputLayout::Line separator;
    separator.tableSeparator = true;
    separator.lineHeight = 3.0f;
    check(!Model::gutterLineUsable(separator, 3.0f, fontSize), "3px 表格分隔行不画行号");
    // 行高异常（几何回落到默认行高）时，分隔行标志本身也要能拦住。
    check(!Model::gutterLineUsable(separator, fontSize * 1.2f, fontSize),
          "分隔行标志本身即跳过，不依赖行高");

    Model::InputLayout::Line plain;
    check(Model::gutterLineUsable(plain, fontSize * 1.2f, fontSize), "普通行（1.2em 行高）画行号");
    check(Model::gutterLineUsable(plain, fontSize, fontSize), "行高 == 字号仍画行号（判据是 <）");
    check(!Model::gutterLineUsable(plain, 3.0f, fontSize), "3px 的普通行同样装不下行号");

    Model::InputLayout::Line heading;
    check(Model::gutterLineUsable(heading, 40.0f, 28.0f), "标题行（行高 > 字号）画行号");
    Model::InputLayout::Line imageRow;
    check(Model::gutterLineUsable(imageRow, 120.0f, fontSize), "图片行（行高 = 图片高）画行号");

    // 已知边界（记录，不是期望行为）：极大字号 + 行高由很小的图片给出时，
    // 行盒装不下这一行的字 —— 同一条判据会把这个图片行的行号也跳过。
    check(!Model::gutterLineUsable(imageRow, 20.0f, 48.0f),
          "已知边界：20px 图片行 + 48px 字号会被判成装不下（记录用）");

    // ── 真实排版：标题 / 分隔行 / 图片行的几何高度 ──
    const std::string text = "# 标题\n|---|\n![图](pic.png)";
    const int sepBeg = static_cast<int>(text.find('\n')) + 1;
    const int sepEnd = sepBeg + static_cast<int>(text.substr(static_cast<std::size_t>(sepBeg)).find('\n'));
    const int imgBeg = sepEnd + 1;
    const int imgEnd = static_cast<int>(text.size());

    std::vector<LineDecoration> decorations(3);
    decorations[0].fontSize = 28.0f;
    decorations[0].lineHeight = 40.0f;
    decorations[1].tableId = 5;
    decorations[1].tableSeparator = true;
    decorations[1].lineHeight = 3.0f;
    decorations[1].holes = {{sepBeg, sepEnd}};
    decorations[1].box.background = core::Color{0.4f, 0.4f, 0.4f, 1.0f};  // 分隔条底色（app 侧同款）
    decorations[2].imagePath = "C:/nowhere/pic.png";
    decorations[2].imageWidth = 100.0f;
    decorations[2].imageHeight = 120.0f;
    decorations[2].lineHeight = 120.0f;
    decorations[2].holes = {{imgBeg, imgEnd}};

    Model::InputState state;
    state.text = text;
    state.textRevision = 1;
    const Model::InputLayout layout = Model::InputLayout::build(
        state, 336.0f, 400.0f, 400.0f, 12.0f, 12.0f, 12.0f, fontSize, "monospace", fontSize, true,
        &decorations);
    const auto& geometry = layout.geometryTable();
    check(geometry.count() == 3, "三行都应有几何");
    if (geometry.count() != 3) {
        return;
    }
    check(near(geometry.height(0), 40.0f), "标题行几何高 = 装饰行高");
    check(near(geometry.height(1), 3.0f), "分隔行几何高 = 3px");
    check(near(geometry.height(2), 120.0f), "图片行几何高 = 图片高");
    check(layout.lineList()[1].tableSeparator, "分隔行标志应盖到行上（底色块在 → 走到盖章循环）");

    const auto alignFontSizeOf = [&layout, fontSize](int index) {
        const float lineFontSize = layout.lineList()[static_cast<std::size_t>(index)].fontSize;
        return lineFontSize > 0.0f ? lineFontSize : fontSize;
    };
    check(!Model::gutterLineUsable(layout.lineList()[1], geometry.height(1), alignFontSizeOf(1)),
          "真实排版里分隔行（3px）不画行号");
    check(Model::gutterLineUsable(layout.lineList()[0], geometry.height(0), alignFontSizeOf(0)),
          "真实排版里标题行画行号");
    check(Model::gutterLineUsable(layout.lineList()[2], geometry.height(2), alignFontSizeOf(2)),
          "真实排版里图片行画行号");
}

// ── ⑪ 行首图元占位：wrapWidth 与 applyLineGlyph 用同一个公式（T19 视觉修正 2）──
// 测量前扣的（wrapWidth）与测量后加的（applyLineGlyph 平移）必须是同一个数：
//   * 公式一致 → 装饰行的**切段点** == 用 (viewport - indent - advance) 当视口的无装饰行；
//   * 平移一致 → 行上记的 glyph.advance == 期望值；
//   * 满行 → 每段 metrics.width <= viewport（修正前会整体溢出一个 advance）。
void testGlyphAdvanceWrapWidth() {
    const float viewport = 336.0f;
    const float fontSize = 16.0f;
    const float indent = 16.0f;

    // ── 两条公式的期望值（照清单写死，helper 被改错时这里先红）──
    const float fallbackAdvance = std::max(4.0f, fontSize * detail::kGlyphAdvanceEm);
    detail::LineGlyph fallbackGlyph;
    fallbackGlyph.codepoint = 0xF0C8;
    check(near(Model::glyphAdvanceOf(fallbackGlyph, fontSize), fallbackAdvance),
          "fallback 公式 = max(4, 字号 * kGlyphAdvanceEm)");
    detail::LineGlyph explicitGlyph;
    explicitGlyph.codepoint = 0xF14A;
    explicitGlyph.advance = 30.0f;
    check(near(Model::glyphAdvanceOf(explicitGlyph, fontSize), 30.0f), "显式 advance > 0 时用显式值");
    check(near(Model::glyphAdvanceOf(explicitGlyph, 48.0f), 30.0f), "显式 advance 与字号无关");
    detail::LineGlyph noGlyph;
    noGlyph.advance = 12.0f;  // 有数值但没有图元 → 仍然 0（wrapWidth 不扣、平移不动）
    check(near(Model::glyphAdvanceOf(noGlyph, fontSize), 0.0f), "无图元（codepoint == 0）不占位");

    // 字号与最终 line.fontSize 同源：decoration->fontSize > 0 ? 它 : 控件字号。
    LineDecoration bigFontDecoration;
    bigFontDecoration.fontSize = 32.0f;
    const float effectiveFontSize =
        bigFontDecoration.fontSize > 0.0f ? bigFontDecoration.fontSize : fontSize;
    check(near(Model::glyphAdvanceOf(fallbackGlyph, effectiveFontSize),
               std::max(4.0f, bigFontDecoration.fontSize * detail::kGlyphAdvanceEm)),
          "装饰字号优先于控件字号（与 line.fontSize 同源）");

    // 等宽语料：切点只由宽度决定，两侧结果可逐段比对。
    const std::string text(80, 'a');
    const auto measure = [&text, fontSize](const std::vector<LineDecoration>* decorations,
                                           float wrapViewport) {
        return Model::measureLines(text, "monospace", fontSize, wrapViewport, decorations);
    };
    const auto checkConsistency = [&](const std::vector<LineDecoration>& decorations, float advance,
                                      const std::string& label) {
        const std::vector<Model::InputLayout::Line> decorated = measure(&decorations, viewport);
        // 参照系：没有装饰的同一段文本，视口直接给"扣掉 indent + advance 之后"的宽度 ——
        // 装饰行的切段点必须与它逐字节相同，否则就是两侧公式不同源。
        const std::vector<Model::InputLayout::Line> reference =
            measure(nullptr, viewport - indent - advance);
        check(decorated.size() == reference.size() && !decorated.empty(),
              label + "：切段数量一致（" + std::to_string(decorated.size()) + " vs " +
                  std::to_string(reference.size()) + ")");
        if (decorated.size() != reference.size()) {
            return;
        }
        for (std::size_t i = 0; i < decorated.size(); ++i) {
            const Model::InputLayout::Line& line = decorated[i];
            const Model::InputLayout::Line& expected = reference[i];
            check(line.start == expected.start && line.end == expected.end,
                  label + "：第 " + std::to_string(i) + " 段切点一致（测量侧扣了 indent+advance）");
            check(near(line.glyph.advance, advance),
                  label + "：第 " + std::to_string(i) + " 段平移侧 advance = " +
                      std::to_string(advance) + "，实际 " + std::to_string(line.glyph.advance));
            check(near(line.contentIndent, indent), label + "：contentIndent 落到行上");
            check(line.metrics.width <= viewport + 0.01f,
                  label + "：满行段宽 " + std::to_string(line.metrics.width) +
                      " 不超过 viewport " + std::to_string(viewport));
        }
    };

    // ① fallback（advance 未给）+ contentIndent：满行不得溢出视口。
    std::vector<LineDecoration> fallbackDecorations(1);
    fallbackDecorations[0].glyph = fallbackGlyph;
    fallbackDecorations[0].contentIndent = indent;
    checkConsistency(fallbackDecorations, fallbackAdvance, "fallback");

    // ② 显式 advance：同一套判据换成显式值。
    std::vector<LineDecoration> explicitDecorations(1);
    explicitDecorations[0].glyph = explicitGlyph;
    explicitDecorations[0].contentIndent = indent;
    checkConsistency(explicitDecorations, 30.0f, "显式 advance");

    // ③ 只有 contentIndent、没有图元：扣 0 —— 与改造前的既有行为逐段一致。
    std::vector<LineDecoration> indentOnlyDecorations(1);
    indentOnlyDecorations[0].contentIndent = indent;
    checkConsistency(indentOnlyDecorations, 0.0f, "无图元");
}

// ── T4：带装饰的布局增量（组件层，不依赖 lp_plan）──────────────────────────
// 两个容易漏的边界：
//   ① 文本变了、但**装饰逐字节没变**（普通行没有洞/段）——判据必须先看文本，
//      否则会拿上一次的行表去渲染新文本（只比装饰就会掉进这个坑）；
//   ② 区间外的行必须**整行复用并平移**绝对偏移（start/end/holes/runs/lineNumber），
//      结果与"对新文本从空状态全量 measureLines"逐字段相等。
void testIncrementalLayoutWithDecorations() {
    const std::string fontFamily = "monospace";
    const float fontSize = 16.0f;
    const float width = 480.0f;
    const std::string oldText = "第一行普通文字\n第二行普通文字\n第三行普通文字\n";

    // 每个物理行一项：第 0 行带一个洞 + 一段样式段（绝对偏移），其余行是默认装饰。
    const auto buildDecorations = [](const std::string& text) {
        std::vector<LineDecoration> decorations;
        int lineBeg = 0;
        while (lineBeg <= static_cast<int>(text.size())) {
            const std::size_t newline = text.find('\n', static_cast<std::size_t>(lineBeg));
            const int lineEnd = newline == std::string::npos
                ? static_cast<int>(text.size())
                : static_cast<int>(newline);
            LineDecoration decoration;
            if (decorations.empty()) {
                decoration.holes.push_back({lineBeg, lineBeg + 2});
                LineRun run;
                run.beg = lineBeg;
                run.end = lineEnd;
                run.style.weight = 700;
                decoration.runs.push_back(run);
            }
            decorations.push_back(decoration);
            if (newline == std::string::npos) {
                break;
            }
            lineBeg = static_cast<int>(newline) + 1;
        }
        return decorations;
    };

    Model::InputState state;
    state.text = oldText;
    state.textRevision = 1;
    std::vector<LineDecoration> oldDecorations = buildDecorations(oldText);
    Model::InputLayout::build(state, width, 500.0f, width, 10.0f, 10.0f, 10.0f, fontSize * 1.2f,
                              fontFamily, fontSize, true, &oldDecorations);

    // 编辑第 1 行：第 0 行的装饰逐字节不变、其余行也没有装饰差 —— 正是边界 ①。
    const std::size_t position = oldText.find("第二行普通文字") + 6;
    state.cursor = static_cast<int>(position);
    Model::clearSelection(state);
    Model::insertAtCursor(state, "插入");

    std::vector<LineDecoration> newDecorations = buildDecorations(state.text);
    const Model::LayoutDebugStats before = Model::debugLayoutStats();
    Model::InputLayout::build(state, width, 500.0f, width, 10.0f, 10.0f, 10.0f, fontSize * 1.2f,
                              fontFamily, fontSize, true, &newDecorations);
    const Model::LayoutDebugStats after = Model::debugLayoutStats();
    check(after.incremental > before.incremental && after.full == before.full,
          "装饰没变但文本变了：必须走逐行增量而不是全量重排");

    const std::vector<Model::TextLine> reference =
        Model::measureLines(state.text, fontFamily, fontSize, width, &newDecorations);
    check(state.cachedLines.size() == reference.size(),
          "增量行表行数应与全量一致：" + std::to_string(state.cachedLines.size()) + " vs " +
              std::to_string(reference.size()));
    int drift = 0;
    for (std::size_t i = 0; i < reference.size() && i < state.cachedLines.size(); ++i) {
        const Model::TextLine& a = state.cachedLines[i];
        const Model::TextLine& b = reference[i];
        const bool same = a.start == b.start && a.end == b.end &&
                          a.lineNumber == b.lineNumber && a.hardBreakAfter == b.hardBreakAfter &&
                          a.holes == b.holes && a.metrics.caretX == b.metrics.caretX &&
                          a.metrics.byteIndices == b.metrics.byteIndices &&
                          a.runs.size() == b.runs.size();
        bool runsSame = a.runs.size() == b.runs.size();
        for (std::size_t r = 0; runsSame && r < a.runs.size(); ++r) {
            runsSame = a.runs[r].beg == b.runs[r].beg && a.runs[r].end == b.runs[r].end &&
                       a.runs[r].x == b.runs[r].x && a.runs[r].style == b.runs[r].style;
        }
        if (!same || !runsSame) {
            ++drift;
        }
    }
    check(drift == 0, "增量行表必须与全量逐字段等价，差 " + std::to_string(drift) + " 行");

    // 平移口径：编辑点之前的第 0 行偏移不动，编辑点之后的行整体右移插入字节数。
    if (state.cachedLines.size() >= 3 && reference.size() >= 3) {
        const int delta = static_cast<int>(state.text.size()) - static_cast<int>(oldText.size());
        check(state.cachedLines[0].start == 0 && state.cachedLines[0].holes[0].beg == 0,
              "编辑点之前的行不应被平移");
        check(state.cachedLines[2].start == reference[2].start &&
                  reference[2].start == oldText.find("第三行") + delta,
              "编辑点之后的行应整体平移插入的字节数");
    }
}

// ── T4 复审：软换行下的装饰增量（源行号 ≠ 物理行下标）────────────────────────
// updateChangedDecorations 的复用游标 previousCursor 走的是**物理行**（TextLine）下标，
// 而 first / oldTail 是**源行号**（按 '\n' 切的行）。viewportWidth=120 的长行会被软换行
// 拆成多条 TextLine —— 前面的行一折行，两者就不再相等；拿源行号直接比较/赋值的旧实现
// 会把每一次编辑都判成"复用游标与行号映射对不上"，退全量。这里断言：
//   ① 增量真的命中（incremental++，full / fallback 一动不动）；
//   ② 增量行表与"对新文本从空状态全量 measureLines"逐字段等价。
void testIncrementalLayoutWithSoftWrappedDecorations() {
    const std::string fontFamily = "monospace";
    const float fontSize = 16.0f;
    const float viewportWidth = 120.0f;  // 窄视口：长行必然软换行

    // 长行语料：每行远超 120px，且行首是 ASCII（行首洞不会切开多字节字符）。
    std::string oldText;
    for (int i = 0; i < 6; ++i) {
        oldText += "L" + std::to_string(i) + " ";
        for (int word = 0; word < 8; ++word) {
            oldText += "word" + std::to_string(word) + " ";
        }
        oldText += "tail sentence to force wrapping\n";
    }

    // 每个源行一项装饰：行首一个洞 + 行内一段样式段（绝对偏移随文本走）；
    // 第 0 行加大字号 —— 走"装饰路径"，且复用判据必须逐字段对得上。
    const auto buildDecorations = [](const std::string& text) {
        std::vector<LineDecoration> decorations;
        int lineBeg = 0;
        while (lineBeg <= static_cast<int>(text.size())) {
            const std::size_t newline = text.find('\n', static_cast<std::size_t>(lineBeg));
            const int lineEnd = newline == std::string::npos ? static_cast<int>(text.size())
                                                             : static_cast<int>(newline);
            LineDecoration decoration;
            decoration.holes.push_back({lineBeg, lineBeg + 2});
            if (decorations.empty()) {
                decoration.fontSize = 24.0f;
                decoration.lineHeight = 30.0f;
            } else if (lineEnd - lineBeg > 16) {
                LineRun run;
                run.beg = lineBeg + 4;
                run.end = lineEnd;
                run.style.weight = 700;
                decoration.runs.push_back(run);
            }
            decorations.push_back(std::move(decoration));
            if (newline == std::string::npos) {
                break;
            }
            lineBeg = static_cast<int>(newline) + 1;
        }
        return decorations;
    };

    Model::InputState state;
    state.text = oldText;
    state.textRevision = 1;
    std::vector<LineDecoration> oldDecorations = buildDecorations(oldText);
    Model::InputLayout::build(state, viewportWidth, 900.0f, viewportWidth, 10.0f, 10.0f, 10.0f,
                              fontSize * 1.2f, fontFamily, fontSize, true, &oldDecorations);

    // 基线必须真的发生了软换行，否则下面的断言全是假通过。
    const std::size_t sourceLineCount = 7;  // 6 行 + 末尾空行
    check(state.cachedLines.size() > sourceLineCount,
          "viewportWidth=120 的长行应折成多条 TextLine（实际 " +
              std::to_string(state.cachedLines.size()) + " 条 / 源行 " +
              std::to_string(sourceLineCount) + "）");

    // 编辑第 3 行（0 基）：改动区**前面**已有三行软换行 → 物理下标与源行号必然不相等。
    const std::size_t line3 = oldText.find("L3 ");
    check(line3 != std::string::npos, "样例里应能找到第 3 行");
    if (line3 == std::string::npos) {
        return;
    }
    state.cursor = static_cast<int>(line3) + 10;
    Model::clearSelection(state);
    Model::insertAtCursor(state, "插入的字");

    std::vector<LineDecoration> newDecorations = buildDecorations(state.text);
    const Model::LayoutDebugStats before = Model::debugLayoutStats();
    Model::layoutRejectReason().clear();
    Model::InputLayout::build(state, viewportWidth, 900.0f, viewportWidth, 10.0f, 10.0f, 10.0f,
                              fontSize * 1.2f, fontFamily, fontSize, true, &newDecorations);
    const Model::LayoutDebugStats after = Model::debugLayoutStats();
    check(after.incremental > before.incremental && after.full == before.full &&
              after.fallback == before.fallback,
          "前序软换行的编辑仍应走逐行增量（incremental=" +
              std::to_string(after.incremental - before.incremental) +
              " full=" + std::to_string(after.full - before.full) +
              " fallback=" + std::to_string(after.fallback - before.fallback) + "）原因：" +
              Model::layoutRejectReason());

    const std::vector<Model::TextLine> reference =
        Model::measureLines(state.text, fontFamily, fontSize, viewportWidth, &newDecorations);
    check(state.cachedLines.size() == reference.size(),
          "软换行增量的行数应与全量一致：" + std::to_string(state.cachedLines.size()) + " vs " +
              std::to_string(reference.size()));
    int drift = 0;
    std::string firstDiff;
    for (std::size_t i = 0; i < reference.size() && i < state.cachedLines.size(); ++i) {
        const Model::TextLine& a = state.cachedLines[i];
        const Model::TextLine& b = reference[i];
        bool same = a.start == b.start && a.end == b.end && a.lineNumber == b.lineNumber &&
                    a.lineStart == b.lineStart && a.hardBreakAfter == b.hardBreakAfter &&
                    a.holes == b.holes && a.runs.size() == b.runs.size() &&
                    near(a.fontSize, b.fontSize) && near(a.lineHeight, b.lineHeight) &&
                    a.metrics.caretX == b.metrics.caretX &&
                    a.metrics.byteIndices == b.metrics.byteIndices;
        for (std::size_t r = 0; same && r < a.runs.size(); ++r) {
            same = a.runs[r].beg == b.runs[r].beg && a.runs[r].end == b.runs[r].end &&
                   near(a.runs[r].x, b.runs[r].x) && near(a.runs[r].width, b.runs[r].width) &&
                   a.runs[r].style == b.runs[r].style;
        }
        if (!same) {
            if (firstDiff.empty()) {
                firstDiff = "行 " + std::to_string(i);
            }
            ++drift;
        }
    }
    check(drift == 0, "软换行增量行表必须与全量逐字段等价，差 " + std::to_string(drift) +
                          " 行（首个：" + firstDiff + "）");
}

}  // namespace

int main() {
    testGeometryTable();
    testGeometryTableWithGaps();
    testProjectionMath();
    testInputLayout();
    testDecoratedSoftWrap();
    testStyledRuns();
    testStyledRunsSoftWrap();
    testLineGlyph();
    testGutterLineUsable();
    testGlyphAdvanceWrapWidth();
    testIncrementalLayoutWithDecorations();
    testIncrementalLayoutWithSoftWrappedDecorations();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "Live Preview layout invariants passed\n";
    return 0;
}
