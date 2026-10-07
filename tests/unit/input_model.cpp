#include "components/input.h"
#include "components/input_model.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

int main() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    using Model = components::input_detail::InputModel;

    const core::KeyEvent left{
        core::InputKey::Left, core::KeyAction::Repeat, {false, true}
    };
    core::KeyModifiers shortcutModifiers;
    shortcutModifiers.shift = true;
#if defined(__APPLE__)
    shortcutModifiers.super = true;
#else
    shortcutModifiers.control = true;
#endif
    const core::KeyEvent shiftedShortcut{
        core::InputKey::Z, core::KeyAction::Press, shortcutModifiers
    };
    const core::KeyEvent shiftRelease{
        core::InputKey::LeftShift, core::KeyAction::Release, {}
    };
    if (!left.isDown() || !left.modifiers.shift ||
        !shiftedShortcut.isDown() || !shiftedShortcut.modifiers.shortcut() ||
        !shiftedShortcut.modifiers.shift || shiftRelease.isDown()) {
        std::cerr << "Generic keyboard events lost key action or modifiers\n";
        return 1;
    }

    Model::InputState state;
    state.text = "first line\nsecond line";
    state.textRevision = 1;

    constexpr float inset = 12.0f;
    constexpr float fontSize = 17.0f;
    constexpr float width = 360.0f;
    constexpr float height = 116.0f;
    const float viewportWidth = width - inset * 2.0f;
    const float viewportHeight = height - inset * 2.0f;
    const Model::InputLayout layout = Model::InputLayout::build(
        state,
        viewportWidth,
        viewportHeight,
        width,
        inset,
        inset,
        inset,
        fontSize,
        "monospace",
        fontSize,
        true);

    const core::Rect bounds{100.0f, 200.0f, width, height};
    const int firstLineCursor = layout.cursorFromPointer(
        bounds.x + inset,
        bounds.y + inset + fontSize * 0.5f,
        bounds,
        width,
        inset);
    const int secondLineCursor = layout.cursorFromPointer(
        bounds.x + inset,
        bounds.y + inset + fontSize * 1.5f,
        bounds,
        width,
        inset);

    if (firstLineCursor >= 10) {
        std::cerr << "First line click resolved past newline: " << firstLineCursor << "\n";
        return 1;
    }
    if (secondLineCursor < 11) {
        std::cerr << "Second line click did not resolve to second line: " << secondLineCursor << "\n";
        return 1;
    }

    // ── S3f 批次 A：pointerHit 与 cursorFromPointer 同值 + 行首图元命中判定 ──
    {
        std::vector<components::input_detail::LineDecoration> glyphDecorations(2);
        glyphDecorations[0].glyph.codepoint = 0xF0C8;
        glyphDecorations[0].glyph.color = core::Color{1.0f, 1.0f, 1.0f, 1.0f};
        const Model::InputLayout glyphLayout = Model::InputLayout::build(
            state,
            viewportWidth,
            viewportHeight,
            width,
            inset,
            inset,
            inset,
            fontSize,
            "monospace",
            fontSize,
            true,
            &glyphDecorations);
        const float glyphAdvance = Model::glyphAdvanceOf(glyphDecorations[0].glyph, fontSize);
        bool ok = true;

        // 图元区内：命中行 0、字节 == 行首、onGlyph 为真、lineX ∈ [0, advance)。
        const auto glyphHit = glyphLayout.pointerHit(
            bounds.x + inset + glyphAdvance * 0.5f,
            bounds.y + inset + fontSize * 0.5f,
            bounds, width, inset);
        ok = ok && glyphHit.onGlyph && glyphHit.byteIndex == 0 && glyphHit.lineIndex == 0 &&
             glyphHit.lineX >= 0.0f && glyphHit.lineX < glyphAdvance;
        if (!ok) {
            std::cerr << "Glyph-region pointerHit mismatch: byte=" << glyphHit.byteIndex
                      << " line=" << glyphHit.lineIndex << " lineX=" << glyphHit.lineX
                      << " onGlyph=" << glyphHit.onGlyph << "\n";
            return 1;
        }

        // 越过 advance 的文字深处 → 不算图元。
        if (glyphLayout.pointerHit(bounds.x + inset + glyphAdvance + 40.0f,
                                   bounds.y + inset + fontSize * 0.5f,
                                   bounds, width, inset).onGlyph) {
            std::cerr << "Text-region click wrongly reported onGlyph\n";
            return 1;
        }
        // 左侧留白（lineX < 0）→ 不算图元（行号列/边距点击绝不触发切换）。
        const auto gutterHit = glyphLayout.pointerHit(
            bounds.x + inset * 0.25f, bounds.y + inset + fontSize * 0.5f, bounds, width, inset);
        if (gutterHit.onGlyph || gutterHit.lineX >= 0.0f) {
            std::cerr << "Gutter click wrongly reported onGlyph (lineX=" << gutterHit.lineX << ")\n";
            return 1;
        }
        // 没有图元的行（行 1）→ 永远不算图元。
        if (glyphLayout.pointerHit(bounds.x + inset + glyphAdvance * 0.5f,
                                   bounds.y + inset + fontSize * 1.5f,
                                   bounds, width, inset).onGlyph) {
            std::cerr << "Glyph-less line wrongly reported onGlyph\n";
            return 1;
        }

        // 恒等式：同输入下 cursorFromPointer == pointerHit().byteIndex（网格采样两套布局）。
        int mismatches = 0;
        for (float sy = 0.25f; sy <= 2.0f; sy += 0.25f) {
            for (float sx = 0.0f; sx <= width; sx += 17.0f) {
                const double px = bounds.x + sx;
                const double py = bounds.y + inset + fontSize * sy;
                if (glyphLayout.cursorFromPointer(px, py, bounds, width, inset) !=
                    glyphLayout.pointerHit(px, py, bounds, width, inset).byteIndex) {
                    ++mismatches;
                }
                if (layout.cursorFromPointer(px, py, bounds, width, inset) !=
                    layout.pointerHit(px, py, bounds, width, inset).byteIndex) {
                    ++mismatches;
                }
            }
        }
        if (mismatches != 0) {
            std::cerr << "cursorFromPointer diverged from pointerHit " << mismatches << " times\n";
            return 1;
        }
    }

    // ── C1：列表文本 marker 与续行缩进的"单一实测 advance"────────────────
    // 装饰层给 glyph.text（呈现文本）与 listIndentBeg/End（度量源区间），组件在
    // 测量阶段用本行字体实测区间宽度：marker 行充当图元 advance、物理续行叠加进
    // contentIndent —— 测量 / wrapWidth / caretX / run.x / 命中拿到同一个数。
    {
        Model::InputState listState;
        listState.text = "1. alpha\ncont line";
        listState.textRevision = 1;
        std::vector<components::input_detail::LineDecoration> listDecorations(2);
        // 行 0 = marker 行：呈现 "3."（序号重启语义的呈现值，不是源码字面 1），
        // 度量源 = 隐藏前缀 "1. "；行 1 = 物理续行，度量源 = owner 的同一段前缀。
        listDecorations[0].glyph.text = "3.";
        listDecorations[0].glyph.color = core::Color{1.0f, 1.0f, 1.0f, 1.0f};
        listDecorations[0].listIndentBeg = 0;
        listDecorations[0].listIndentEnd = 3;
        listDecorations[1].listIndentBeg = 0;
        listDecorations[1].listIndentEnd = 3;
        const Model::InputLayout listLayout = Model::InputLayout::build(
            listState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &listDecorations);
        const auto& lines = listLayout.lineList();
        if (lines.size() < 2) {
            std::cerr << "list layout should produce two lines, got " << lines.size() << "\n";
            return 1;
        }
        const float bodyColumn = core::TextPrimitive::measureTextWidth("1. ", "monospace", fontSize);
        // marker 行：正文列 = 实测前缀宽（caret 起点、run.x、行宽都含同一个 advance）。
        const float markerAdvance = lines[0].glyph.advance;
        if (!(markerAdvance > 0.0f && std::fabs(markerAdvance - bodyColumn) < 0.01f)) {
            std::cerr << "text-marker advance mismatch: advance=" << markerAdvance
                      << " measured=" << bodyColumn << "\n";
            return 1;
        }
        if (std::fabs(lines[0].metrics.caretX[0] - bodyColumn) >= 0.01f) {
            std::cerr << "marker line first caretX should sit at body column\n";
            return 1;
        }
        if (lines[0].glyph.text != "3.") {
            std::cerr << "marker text lost in layout\n";
            return 1;
        }
        // 物理续行：contentIndent = 同一个实测宽（与 marker 行正文起点对齐）。
        if (std::fabs(lines[1].contentIndent - bodyColumn) >= 0.01f) {
            std::cerr << "continuation contentIndent should equal measured body column\n";
            return 1;
        }
        // 命中：文本 marker 不是可点图元 —— 点 marker 区不得报 onGlyph
        //（普通 marker 点击不得触发任务切换的组件级保证）。
        const auto markerHit = listLayout.pointerHit(
            bounds.x + inset + markerAdvance * 0.5f,
            bounds.y + inset + fontSize * 0.5f, bounds, width, inset);
        if (markerHit.onGlyph) {
            std::cerr << "text-marker click wrongly reported onGlyph\n";
            return 1;
        }
    }

    // ── S3f 批次 C：折叠 hidden 行的几何 / 命中重映射 / 垂直移动落点 ──
    {
        Model::InputState foldState;
        foldState.text = "第一行\n第二行\n第三行\n第四行";
        foldState.textRevision = 1;

        // 行 1、2 折叠：几何 0 高、contentHeight 只剩可见两行、
        // 命中永远落在可见行、↓ 落到 hidden 行（展开由应用层回调负责）。
        std::vector<components::input_detail::LineDecoration> foldDecorations(4);
        foldDecorations[1].hidden = true;
        foldDecorations[2].hidden = true;
        const Model::InputLayout foldLayout = Model::InputLayout::build(
            foldState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &foldDecorations);
        bool ok = true;
        const auto& foldGeometry = foldLayout.geometryTable();
        ok = ok && foldGeometry.count() == 4;
        ok = ok && foldGeometry.height(0) > 0.0f;
        ok = ok && foldGeometry.height(1) == 0.0f && foldGeometry.height(2) == 0.0f;
        ok = ok && foldGeometry.height(3) > 0.0f;
        ok = ok && std::fabs(foldLayout.contentHeight - (foldGeometry.height(0) + foldGeometry.height(3))) < 0.01f;
        if (!ok) {
            std::cerr << "fold geometry wrong: heights="
                      << foldGeometry.height(0) << "," << foldGeometry.height(1) << ","
                      << foldGeometry.height(2) << "," << foldGeometry.height(3)
                      << " contentHeight=" << foldLayout.contentHeight << "\n";
            return 1;
        }
        // 折叠段与它下方可见行共享同一 top：点折叠处 → 应落到**可见**的行 3。
        const float foldY = inset + foldGeometry.top(3) + 1.0f;
        const auto hitOnFold = foldLayout.pointerHit(
            bounds.x + inset + 5.0f, bounds.y + foldY, bounds, width, inset);
        if (hitOnFold.lineIndex != 3) {
            std::cerr << "pointer at collapsed y resolved to line " << hitOnFold.lineIndex
                      << ", expected 3\n";
            return 1;
        }
        // 折叠吃到了文档末尾：lineAtY 会二分到隐藏行，必须被重映射回最近的可见行 0。
        std::vector<components::input_detail::LineDecoration> tailDecorations(3);
        tailDecorations[1].hidden = true;
        tailDecorations[2].hidden = true;
        Model::InputState tailState;
        tailState.text = "开头\n折叠甲\n折叠乙";
        tailState.textRevision = 1;
        const Model::InputLayout tailLayout = Model::InputLayout::build(
            tailState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &tailDecorations);
        const float belowAll = inset + tailLayout.contentHeight + 20.0f;
        const auto hitBelow = tailLayout.pointerHit(
            bounds.x + inset + 5.0f, bounds.y + belowAll, bounds, width, inset);
        if (hitBelow.lineIndex != 0) {
            std::cerr << "pointer below tail-fold resolved to line " << hitBelow.lineIndex
                      << ", expected remap to 0\n";
            return 1;
        }
        // ↓ 移动：光标从行 0 落到折叠的行 1（字节 = 行首）；应用层检测 hidden 后展开。
        foldState.cursor = 0;
        Model::moveCursorVertical(foldState, 1, false, "monospace", fontSize,
                                  viewportWidth, viewportHeight);
        if (foldState.cursor != static_cast<int>(std::string("第一行\n").size())) {
            std::cerr << "vertical move onto hidden line landed at " << foldState.cursor
                      << ", expected line-1 start\n";
            return 1;
        }
        if (foldState.cursor >= static_cast<int>(foldState.text.size()) ||
            !foldLayout.lineList()[static_cast<std::size_t>(
                 foldLayout.lineIndexFor(foldState.cursor))]
                 .hidden) {
            std::cerr << "cursor after vertical move should be on a hidden line\n";
            return 1;
        }

        // hiddenByFold 契约（光标行强制可见的折衷）：装饰只标 hiddenByFold（不标
        // hidden）的行保持正常高度，但 flags 原样传给布局 —— onRevealHiddenLine
        // 靠它识别"↓ 落进了折叠段"，否则自愈永远不触发，折叠卡在半开。
        std::vector<components::input_detail::LineDecoration> revealDecorations(2);
        revealDecorations[1].hiddenByFold = true;
        const Model::InputLayout revealLayout = Model::InputLayout::build(
            foldState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &revealDecorations);
        const auto& revealGeometry = revealLayout.geometryTable();
        const Model::InputLayout::Line& revealLine = revealLayout.lineList()[1];
        if (revealGeometry.height(1) <= 0.0f || revealLine.hidden || !revealLine.hiddenByFold) {
            std::cerr << "hiddenByFold-only line must keep height and carry the flag"
                      << " (h=" << revealGeometry.height(1)
                      << " hidden=" << revealLine.hidden
                      << " hiddenByFold=" << revealLine.hiddenByFold << ")\n";
            return 1;
        }
    }

    // ── S3f 批次 D：表格列吸附（逐格排版 + 列 x 对齐 + 边界 caret + 命中/截断）──
    {
        Model::InputState tableState;
        // 三列 + 多字节中文格 + **真管道空隙（holes）**：这三样缺一就测不出
        // "格分界处两个停靠点共享同一个投影偏移"那个坑（实机就是这么翻车的：
        // 点第 2 格中部，退格把中间那根管道吃了）。
        const std::string text =
            "| 甲甲 | 乙丑 | 丙丙 |\n"
            "| --- | --- | --- |\n"
            "| 子子子 | 丑丑 | 寅寅寅 |";
        tableState.text = text;
        tableState.textRevision = 1;

        const auto cellAt = [&text](const std::string& needle, std::size_t from) {
            const std::size_t beg = text.find(needle, from);
            return components::input_detail::LineCell{static_cast<int>(beg), static_cast<int>(beg + needle.size())};
        };
        const components::input_detail::LineCell header1 = cellAt("甲甲", 0);
        const components::input_detail::LineCell header2 = cellAt("乙丑", header1.end);
        const components::input_detail::LineCell header3 = cellAt("丙丙", header2.end);
        const components::input_detail::LineCell body1 = cellAt("子子子", header3.end);
        const components::input_detail::LineCell body2 = cellAt("丑丑", body1.end);
        const components::input_detail::LineCell body3 = cellAt("寅寅寅", body2.end);

        const int line0End = static_cast<int>(text.find('\n'));
        const int separatorBeg = line0End + 1;
        const int separatorEnd = separatorBeg + static_cast<int>(text.substr(separatorBeg).find('\n'));
        const int line2Beg = separatorEnd + 1;
        const int line2End = static_cast<int>(text.size());

        // 管道与两侧空白的洞：与装饰层同一口径（cells 之外的字节全藏）。
        const auto gapHoles = [](int lineBeg, int lineEnd,
                                 const std::vector<components::input_detail::LineCell>& cells) {
            std::vector<components::input_detail::LineHole> holes;
            int cursor = lineBeg;
            for (const components::input_detail::LineCell& cell : cells) {
                holes.push_back({cursor, std::max(cursor, cell.beg)});
                cursor = std::max(cursor, cell.end);
            }
            holes.push_back({cursor, lineEnd});
            return holes;
        };

        std::vector<components::input_detail::LineDecoration> tableDecorations(3);
        for (auto& decoration : tableDecorations) {
            decoration.fontSize = fontSize;
            decoration.lineHeight = fontSize * 1.2f;
            decoration.tableId = 7;
        }
        tableDecorations[0].cells = {header1, header2, header3};
        tableDecorations[0].tableHeaderRow = true;
        tableDecorations[0].holes = gapHoles(0, line0End, tableDecorations[0].cells);
        tableDecorations[1].tableSeparator = true;
        tableDecorations[1].lineHeight = 3.0f;
        tableDecorations[1].holes = {{separatorBeg, separatorEnd}};
        tableDecorations[2].cells = {body1, body2, body3};
        tableDecorations[2].holes = gapHoles(line2Beg, line2End, tableDecorations[2].cells);

        const Model::InputLayout tableLayout = Model::InputLayout::build(
            tableState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &tableDecorations);
        const Model::InputLayout::Line& row0 = tableLayout.lineList()[0];
        const Model::InputLayout::Line& row2 = tableLayout.lineList()[2];
        const Model::InputLayout::Line& separatorLine = tableLayout.lineList()[1];
        if (row0.runs.size() < 3 || row2.runs.size() < 3) {
            std::cerr << "table rows must lay out one run per cell (row0=" << row0.runs.size()
                      << " row2=" << row2.runs.size() << ")\n";
            return 1;
        }
        const float kEps = 0.01f;
        // ① 同列的行 x 必须一致（这正是"列对齐"）。
        for (std::size_t column = 0; column < 3; ++column) {
            if (std::fabs(row0.runs[column].x - row2.runs[column].x) > kEps) {
                std::cerr << "table column " << column << " not aligned: row0=" << row0.runs[column].x
                          << " row2=" << row2.runs[column].x << "\n";
                return 1;
            }
        }
        // ② 列间有内边距：第 2 格整体在第 1 格右侧且不重叠。
        if (!(row2.runs[1].x >= row2.runs[0].x + row2.runs[0].width) ||
            !(row2.runs[2].x >= row2.runs[1].x + row2.runs[1].width)) {
            std::cerr << "columns must not overlap (x0=" << row2.runs[0].x << " w0=" << row2.runs[0].width
                      << " x1=" << row2.runs[1].x << " w1=" << row2.runs[1].width
                      << " x2=" << row2.runs[2].x << ")\n";
            return 1;
        }
        // ③ 格首字节的光标 x = 该列文字起点（分界处归**下一格**：上一格末尾与下一格开头
        //    共享同一个投影偏移，谁覆盖谁决定这里对不对）。
        for (std::size_t column = 0; column < 3; ++column) {
            const int cellBeg = tableDecorations[2].cells[column].beg;
            const float caretX = components::input_detail::caretXForDocOffset(
                row2.metrics, row2.holes, row2.start, cellBeg);
            if (std::fabs(caretX - row2.runs[column].x) > kEps) {
                std::cerr << "caret at cell " << column << " start should snap to column x ("
                          << caretX << " vs " << row2.runs[column].x << ")\n";
                return 1;
            }
        }
        // ④ 命中：点第 2 格墨迹中部 → 字节必须落在第 2 格里。
        const float hitX = bounds.x + inset + row2.runs[1].x + row2.runs[1].width * 0.5f;
        const float hitY = bounds.y + inset + tableLayout.geometryTable().top(2) + 2.0f;
        const auto tableHit = tableLayout.pointerHit(hitX, hitY, bounds, width, inset);
        if (tableHit.byteIndex < body2.beg || tableHit.byteIndex > body2.end) {
            std::cerr << "pointer in column 2 resolved to byte " << tableHit.byteIndex
                      << ", expected within [" << body2.beg << "," << body2.end << "]\n";
            return 1;
        }
        // ④b 回归（2026-10-06）：点第 2 格**开头**（列间空隙的右缘）。格界共享停靠点
        //     的字节过去会按"歧义归洞之前"解析到第 1 格末尾 —— 点第 2 格开头输入，
        //     文字却插进第 1 格。命中字节必须 clamp 回第 2 格。
        const float hitStartX = bounds.x + inset + row2.runs[1].x + 0.5f;
        const auto hitStart = tableLayout.pointerHit(hitStartX, hitY, bounds, width, inset);
        const int rawProjectedHit = components::input_detail::docOffsetForX(
            row2.metrics, row2.holes, row2.start, row2.runs[1].x + 0.5f);
        if (rawProjectedHit != body1.end) {
            std::cerr << "fixture no longer exposes the hidden-cell-boundary ambiguity (raw hit="
                      << rawProjectedHit << ", expected first-cell end=" << body1.end << ")\n";
            return 1;
        }
        // The document-aware table map must keep both sides of each hidden pipe,
        // including horizontal navigation and exact cell endpoints.
        for (int column = 0; column < 3; ++column) {
            const auto& cell = tableDecorations[2].cells[static_cast<std::size_t>(column)];
            for (const int endpoint : {cell.beg, cell.end}) {
                const float x = Model::caretXInLine(row2, endpoint);
                const auto endpointHit = tableLayout.pointerHit(
                    bounds.x + inset + x, hitY, bounds, width, inset);
                if (endpointHit.byteIndex != endpoint) {
                    std::cerr << "table endpoint hit for column " << column << " byte "
                              << endpoint << " resolved to " << endpointHit.byteIndex << "\n";
                    return 1;
                }
            }
        }
        if (Model::previousCaretIndex(tableLayout.lineList(), body2.beg) != body1.end ||
            Model::nextCaretIndex(tableLayout.lineList(), tableState.text, body1.end) != body2.beg) {
            std::cerr << "horizontal movement must cross a hidden pipe from one cell edge to the other\n";
            return 1;
        }
        if (hitStart.byteIndex < body2.beg || hitStart.byteIndex > body2.end) {
            std::cerr << "pointer at column-2 start resolved to byte " << hitStart.byteIndex
                      << ", expected within [" << body2.beg << "," << body2.end << "]\n";
            return 1;
        }
        // ④b.1 端到端回归：把点击结果当作输入插入位置。只检查 byteIndex 范围容易
        //     漏掉消费者后续行为；这里确认第二格开头输入后，第一格字节完全未变，且
        //     标记确实出现在第二格内容里。
        const auto insertionKeepsTargetCell = [&](int byteIndex, const char* label) {
            if (byteIndex < body2.beg || byteIndex > body2.end) {
                std::cerr << label << " hit escaped column 2: " << byteIndex << "\n";
                return false;
            }
            std::string edited = text;
            edited.insert(static_cast<std::size_t>(byteIndex), "X");
            const std::string firstCell = edited.substr(
                static_cast<std::size_t>(body1.beg),
                static_cast<std::size_t>(body1.end - body1.beg));
            const std::string secondCell = edited.substr(
                static_cast<std::size_t>(body2.beg),
                static_cast<std::size_t>(body2.end - body2.beg + 1));
            if (firstCell != text.substr(static_cast<std::size_t>(body1.beg),
                                         static_cast<std::size_t>(body1.end - body1.beg)) ||
                secondCell.find('X') == std::string::npos) {
                std::cerr << label << " inserted outside column 2 (first=\"" << firstCell
                          << "\", second=\"" << secondCell << "\")\n";
                return false;
            }
            return true;
        };
        if (!insertionKeepsTargetCell(hitStart.byteIndex, "column-2 start")) return 1;
        // 列内右侧留白（第二格 padding）也属于第二格；点击后输入应落在第二格尾部，
        // 不能经隐藏管道的反投影跳回第一格末尾。
        const auto* hitColumns = tableLayout.tableColumnsFor(7);
        if (hitColumns == nullptr || hitColumns->count() < 2) {
            std::cerr << "table column geometry missing for padding hit\n";
            return 1;
        }
        const float secondCellRightPaddingX = bounds.x + inset +
            hitColumns->x[1] + hitColumns->width[1] - 1.0f;
        const auto hitPadding = tableLayout.pointerHit(
            secondCellRightPaddingX, hitY, bounds, width, inset);
        if (!insertionKeepsTargetCell(hitPadding.byteIndex, "column-2 right padding")) return 1;
        // ④b.2 窄的缩进表格：内容和 caret 会被 contentIndent 整体右移，列所有权判界
        //     也必须使用同一个坐标系。40px 缩进在压缩列宽下足以让第二列右侧 padding
        //     越过未平移的第二/三列中线；漏加偏移会把点击归到第三格。
        auto indentedDecorations = tableDecorations;
        for (auto& decoration : indentedDecorations) decoration.contentIndent = 40.0f;
        constexpr float indentedViewportWidth = 150.0f;
        Model::InputState indentedState;
        indentedState.text = text;
        indentedState.textRevision = 1;
        const Model::InputLayout indentedTableLayout = Model::InputLayout::build(
            indentedState, indentedViewportWidth, viewportHeight, indentedViewportWidth,
            inset, inset, inset, fontSize, "monospace", fontSize, true,
            &indentedDecorations);
        const Model::InputLayout::Line* indentedRow2 = nullptr;
        for (const Model::InputLayout::Line& candidate : indentedTableLayout.lineList()) {
            if (candidate.tableId == 7 && candidate.lineNumber == 3 && candidate.lineStart) {
                indentedRow2 = &candidate;
                break;
            }
        }
        if (indentedRow2 == nullptr || indentedRow2->runs.size() < 2) {
            std::cerr << "indented table body did not expose the second-column start run\n";
            return 1;
        }
        const core::Rect indentedBounds{bounds.x, bounds.y, indentedViewportWidth, bounds.height};
        const float indentedHitY = indentedBounds.y + inset +
            indentedTableLayout.geometryTable().top(
                static_cast<int>(indentedRow2 - indentedTableLayout.lineList().data())) + 2.0f;
        const auto* indentedColumns = indentedTableLayout.tableColumnsFor(7);
        if (indentedColumns == nullptr || indentedColumns->count() < 2) {
            std::cerr << "indented table column geometry is missing\n";
            return 1;
        }
        const float indentedColumn2PaddingX = indentedBounds.x + inset +
            40.0f + indentedColumns->x[1] + indentedColumns->width[1] - 1.0f;
        const auto indentedHit = indentedTableLayout.pointerHit(
            indentedColumn2PaddingX, indentedHitY, indentedBounds,
            indentedViewportWidth, inset);
        if (indentedHit.byteIndex < body2.beg || indentedHit.byteIndex > body2.end) {
            std::cerr << "column-2 right padding with contentIndent=40 resolved to byte "
                      << indentedHit.byteIndex << ", expected within ["
                      << body2.beg << "," << body2.end << "]\n";
            return 1;
        }
        // ④c 列间空隙的**左半**归第 1 格：字节落在第 1 格区间（格尾）——与共享
        //     停靠点把光标画在第 2 格开头的既有观感一致。
        const float gapMidX =
            bounds.x + inset + (row2.runs[0].x + row2.runs[0].width + row2.runs[1].x) * 0.5f;
        const auto hitGap = tableLayout.pointerHit(gapMidX, hitY, bounds, width, inset);
        if (hitGap.byteIndex < body1.beg || hitGap.byteIndex > body1.end) {
            std::cerr << "pointer in gap left half resolved to byte " << hitGap.byteIndex
                      << ", expected within [" << body1.beg << "," << body1.end << "]\n";
            return 1;
        }
        // ⑤ 全行 caret 表单调不减（列吸附的隐含约束；不单调会让光标左右乱跳）。
        int previousByte = -1;
        float previousX = -1.0f;
        for (int offset = row2.start; offset <= row2.end; ++offset) {
            const float caretX = components::input_detail::caretXForDocOffset(
                row2.metrics, row2.holes, row2.start, offset);
            if (caretX < previousX - kEps) {
                std::cerr << "caret x must not decrease (byte " << offset << " -> " << caretX
                          << " after " << previousX << ")\n";
                return 1;
            }
            previousX = caretX;
            ++previousByte;
        }
        (void) previousByte;
        // ⑥ 分隔行：整行藏空（投影文本为空）+ 高度换成细横条。
        const std::string separatorVisible =
            components::input_detail::projectText(text, separatorBeg, separatorEnd, separatorLine.holes);
        if (!separatorVisible.empty() || std::fabs(separatorLine.lineHeight - 3.0f) > 0.01f) {
            std::cerr << "separator row must be fully concealed and thin (visible=\"" << separatorVisible
                      << "\" height=" << separatorLine.lineHeight << ")\n";
            return 1;
        }
        // ⑦ 压进可用宽度（决策③）：整表不超过 viewportWidth。
        if (row2.metrics.width > viewportWidth + 0.5f) {
            std::cerr << "table row must fit the viewport (width=" << row2.metrics.width
                      << " viewport=" << viewportWidth << ")\n";
            return 1;
        }
        // ⑧ 窄视口：列宽被压进可用宽度（决策③），过长的格被硬截。
        //    视口取 200（三列 + 内边距仍压得下；90px 那种极端宽度会触发可读性下限、
        //    有意整体溢出交给横向裁切，不是这条判据的场景）。
        const float narrowWidth = 200.0f;
        const Model::InputLayout narrowLayout = Model::InputLayout::build(
            tableState, narrowWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &tableDecorations);
        const Model::InputLayout::Line& narrowRow2 = narrowLayout.lineList()[2];
        float narrowRight = 0.0f;
        for (const components::input_detail::TextRun& run : narrowRow2.runs) {
            narrowRight = std::max(narrowRight, run.x + run.width);
        }
        if (narrowRight > narrowWidth + 0.5f) {
            std::cerr << "narrow table must be compressed inside the viewport (right=" << narrowRight << ")\n";
            return 1;
        }
        bool clipped = false;
        for (std::size_t column = 0; column < narrowRow2.runs.size() && column < 3; ++column) {
            if (narrowRow2.runs[column].end < tableDecorations[2].cells[column].end) {
                clipped = true;
            }
        }
        if (!clipped) {
            std::cerr << "narrow table should have clipped the over-wide cell text\n";
            return 1;
        }
        // ⑧ 首尾相接的两个洞不能被合并（表格的管道空隙 + 紧挨着它的行内标记）：
        //    合并会把"光标进片段要露出来"的标记一起吞掉。
        std::vector<components::input_detail::LineDecoration> touchDecorations(1);
        touchDecorations[0].holes = {{0, 5}, {5, 8}};
        Model::InputState touchState;
        touchState.text = "abcdefgh";
        touchState.textRevision = 1;
        const Model::InputLayout touchLayout = Model::InputLayout::build(
            touchState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &touchDecorations);
        if (touchLayout.lineList()[0].holes.size() != 2) {
            std::cerr << "touching holes must stay separate, got "
                      << touchLayout.lineList()[0].holes.size() << "\n";
            return 1;
        }
    }

    // Wrapped table rows retain cell ownership on each physical segment. Empty
    // cells are hit-testable below segment zero, and vertical movement skips
    // duplicate short-cell tails instead of becoming stuck on the wrapped row.
    {
        using LineDecoration = components::input_detail::LineDecoration;
        using LineCell = components::input_detail::LineCell;
        using LineHole = components::input_detail::LineHole;
        const std::string longText(160, 'a');
        const std::string secondLongText(160, 'x');
        const std::string wrappedText = std::string("| h1 | h2 | h3 |\n") +
            "| --- | --- | --- |\n" +
            "| " + longText + " | b | c |\n" +
            "| " + secondLongText + " |  | c |";
        std::vector<LineDecoration> decorations;
        std::vector<std::vector<LineCell>> sourceCells;
        int lineBeg = 0;
        while (lineBeg < static_cast<int>(wrappedText.size())) {
            const int lineEnd = static_cast<int>(wrappedText.find('\n', static_cast<std::size_t>(lineBeg)));
            const int end = lineEnd < 0 ? static_cast<int>(wrappedText.size()) : lineEnd;
            LineDecoration decoration;
            decoration.fontSize = fontSize;
            decoration.lineHeight = fontSize * 1.2f;
            decoration.tableId = 42;
            std::vector<LineCell> cells;
            int pipe = wrappedText.find('|', static_cast<std::size_t>(lineBeg));
            while (pipe >= 0 && pipe < end) {
                const int next = static_cast<int>(wrappedText.find('|', static_cast<std::size_t>(pipe + 1)));
                if (next < 0 || next > end) break;
                int beg = pipe + 1;
                int cellEnd = next;
                while (beg < cellEnd && wrappedText[static_cast<std::size_t>(beg)] == ' ') ++beg;
                while (cellEnd > beg && wrappedText[static_cast<std::size_t>(cellEnd - 1)] == ' ') --cellEnd;
                cells.push_back({beg, cellEnd});
                pipe = next;
            }
            if (wrappedText.compare(static_cast<std::size_t>(lineBeg),
                                    static_cast<std::size_t>(end - lineBeg), "| --- | --- | --- |") == 0) {
                decoration.tableSeparator = true;
                decoration.lineHeight = 3.0f;
                decoration.holes.push_back({lineBeg, end});
            } else {
                decoration.cells = cells;
                int cursor = lineBeg;
                for (const LineCell& cell : cells) {
                    if (cursor < cell.beg) decoration.holes.push_back({cursor, cell.beg});
                    cursor = cell.end;
                }
                if (cursor < end) decoration.holes.push_back({cursor, end});
            }
            sourceCells.push_back(cells);
            decorations.push_back(std::move(decoration));
            lineBeg = end + 1;
        }
        Model::InputState wrappedState;
        wrappedState.text = wrappedText;
        wrappedState.textRevision = 1;
        constexpr float wrappedWidth = 220.0f;
        constexpr float wrappedInset = 8.0f;
        const Model::InputLayout wrappedLayout = Model::InputLayout::build(
            wrappedState, wrappedWidth, 500.0f, wrappedWidth, wrappedInset,
            wrappedInset, wrappedInset, fontSize, "monospace", fontSize, true, &decorations);
        const auto& lines = wrappedLayout.lineList();
        const int firstRowBeg = static_cast<int>(wrappedText.find(longText)) - 2;
        const int foundRowEnd = static_cast<int>(wrappedText.find('\n',
            static_cast<std::size_t>(firstRowBeg)));
        const int firstRowEnd = foundRowEnd < 0 ? static_cast<int>(wrappedText.size()) : foundRowEnd;
        const int emptyRowBeg = static_cast<int>(wrappedText.find(secondLongText)) - 2;
        const int foundEmptyRowEnd = static_cast<int>(wrappedText.find('\n',
            static_cast<std::size_t>(emptyRowBeg)));
        const int emptyRowEnd = foundEmptyRowEnd < 0
            ? static_cast<int>(wrappedText.size()) : foundEmptyRowEnd;
        std::vector<int> wrappedSegments;
        std::vector<int> emptySegments;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (lines[static_cast<std::size_t>(i)].tableId == 42 &&
                lines[static_cast<std::size_t>(i)].start == firstRowBeg &&
                lines[static_cast<std::size_t>(i)].end >= firstRowEnd) {
                wrappedSegments.push_back(i);
            }
            if (lines[static_cast<std::size_t>(i)].tableId == 42 &&
                lines[static_cast<std::size_t>(i)].start == emptyRowBeg &&
                lines[static_cast<std::size_t>(i)].end >= emptyRowEnd) {
                emptySegments.push_back(i);
            }
        }
        if (wrappedSegments.size() < 2 || emptySegments.size() < 2) {
            std::cerr << "long table fixture did not produce multiple visual segments for both rows\n";
            return 1;
        }
        const LineCell& emptyCell = sourceCells[3][1];
        const auto* columns = wrappedLayout.tableColumnsFor(42);
        if (columns == nullptr || columns->count() != 3 || emptyCell.beg != emptyCell.end) {
            std::cerr << "wrapped fixture must expose its empty second-column cell\n";
            return 1;
        }
        const int continuation = emptySegments[1];
        const auto& continuationLine = lines[static_cast<std::size_t>(continuation)];
        const float emptyCellX = wrappedInset + columns->x[1] + columns->width[1] * 0.5f;
        const float emptyCellY = wrappedInset + wrappedLayout.geometryTable().top(continuation) + 2.0f;
        const core::Rect wrappedBounds{0.0f, 0.0f, wrappedWidth, 500.0f};
        const auto emptyHit = wrappedLayout.pointerHit(
            emptyCellX, emptyCellY, wrappedBounds, wrappedWidth, wrappedInset);
        const bool hasContinuationEmptyStop = std::any_of(
            continuationLine.tableDocCaretStops.begin(), continuationLine.tableDocCaretStops.end(),
            [](const Model::TableDocCaretStop& stop) { return stop.column == 1; });
        if (emptyHit.byteIndex != emptyCell.beg || hasContinuationEmptyStop) {
            std::cerr << "empty cell hit on wrapped continuation escaped its cell (got "
                      << emptyHit.byteIndex << ")\n";
            return 1;
        }
        const LineCell& shortCell = sourceCells[2][1];
        wrappedState.cursor = shortCell.end;
        Model::moveCursorVertical(wrappedState, 1, false, "monospace", fontSize,
                                  wrappedWidth, 500.0f);
        if (wrappedState.cursor != emptyCell.beg) {
            std::cerr << "Down from short cell tail got stuck inside wrapped siblings (got "
                      << wrappedState.cursor << ", expected next row empty cell "
                      << emptyCell.beg << ")\n";
            return 1;
        }
    }

    // ── S3f 批次 E：表格对齐（居中/靠右列的平移）＋命中扩展（onGutter / onImage）──
    {
        // 三列：列 0 左对齐（default）、列 1 居中、列 2 靠右。两行正文给"同列
        // 不同内容宽"——居中列的中心必须重合、靠右列的右缘必须重合，这两个
        // 断言不依赖列宽的具体数值（列宽被 minWidth 夹逼，直接算反而脆）。
        const std::string text =
            "| 甲甲 | 乙 | 丙 |\n"
            "| --- | --- | --- |\n"
            "| 子子 | 丑丑 | 寅寅 |";
        Model::InputState alignState;
        alignState.text = text;
        alignState.textRevision = 1;

        const auto alignCell = [&text](const std::string& needle, std::size_t from, int align) {
            const std::size_t beg = text.find(needle, from);
            return components::input_detail::LineCell{static_cast<int>(beg),
                                                      static_cast<int>(beg + needle.size()),
                                                      static_cast<unsigned char>(align)};
        };
        const auto alignHoles = [&text](int lineBeg, int lineEnd,
                                        const std::vector<components::input_detail::LineCell>& cells) {
            std::vector<components::input_detail::LineHole> holes;
            int cursor = lineBeg;
            for (const components::input_detail::LineCell& cell : cells) {
                holes.push_back({cursor, std::max(cursor, cell.beg)});
                cursor = std::max(cursor, cell.end);
            }
            holes.push_back({cursor, lineEnd});
            return holes;
        };

        auto buildAlignDecorations = [&]() {
            const int line0End = static_cast<int>(text.find('\n'));
            const int line2Beg = static_cast<int>(text.rfind('\n')) + 1;
            const int line2End = static_cast<int>(text.size());
            std::vector<components::input_detail::LineDecoration> decorations(3);
            for (auto& decoration : decorations) {
                decoration.fontSize = fontSize;
                decoration.lineHeight = fontSize * 1.2f;
                decoration.tableId = 9;
            }
            // 行 0 全左对齐（default）当对照；行 2 = 中/右。表头与正文同列同对齐
            // 才是真实数据（对齐来自分隔行，整列统一）。
            std::vector<components::input_detail::LineCell> header = {
                alignCell("甲甲", 0, 0), alignCell("乙", 0, 0), alignCell("丙", 0, 0)};
            std::vector<components::input_detail::LineCell> body = {
                alignCell("子子", line2Beg, 0), alignCell("丑丑", line2Beg, 2),
                alignCell("寅寅", line2Beg, 3)};
            decorations[0].tableHeaderRow = true;
            decorations[0].cells = header;
            decorations[0].holes = alignHoles(0, line0End, header);
            decorations[1].tableSeparator = true;
            decorations[1].lineHeight = 3.0f;
            decorations[1].holes = {{line0End + 1, line2Beg - 1}};
            decorations[2].cells = body;
            decorations[2].holes = alignHoles(line2Beg, line2End, body);
            return decorations;
        };

        const std::vector<components::input_detail::LineDecoration> leftDecorations =
            buildAlignDecorations();
        // 基线：全左对齐（列 1/列 2 的 align 归零）——对齐版要跟它比偏移。
        // 注意两个 build 必须用**各自独立的 InputState**：InputLayout 是持
        // state.cachedLines 指针的轻量视图，同一 state 的第二次 build 会让第一个
        // 视图指向同一份（对齐后的）行表，对照断言全部失效。
        std::vector<components::input_detail::LineDecoration> baseline = leftDecorations;
        for (components::input_detail::LineCell& cell : baseline[2].cells) {
            cell.align = 0;
        }
        for (components::input_detail::LineCell& cell : baseline[0].cells) {
            cell.align = 0;
        }
        Model::InputState baseAlignState;
        baseAlignState.text = text;
        baseAlignState.textRevision = 1;

        const Model::InputLayout baseLayout = Model::InputLayout::build(
            baseAlignState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &baseline);
        const Model::InputLayout alignLayout = Model::InputLayout::build(
            alignState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &leftDecorations);
        const Model::InputLayout::Line& baseRow = baseLayout.lineList()[2];
        const Model::InputLayout::Line& alignedRow = alignLayout.lineList()[2];
        const float kEps = 0.01f;
        if (alignedRow.runs.size() < 3) {
            std::cerr << "aligned table row must have three runs\n";
            return 1;
        }
        // ① 左对齐列（default）不因对齐改动：与基线 x 完全一致。
        if (std::fabs(alignedRow.runs[0].x - baseRow.runs[0].x) > kEps) {
            std::cerr << "default-aligned column must not shift ("
                      << alignedRow.runs[0].x << " vs " << baseRow.runs[0].x << ")\n";
            return 1;
        }
        // ② 居中列：与表头行同列的中心重合（表头左对齐没平移，这里用正文行自身
        //    与基线比对：偏移 > 0 且恰好 (columnWidth - contentWidth) / 2 ——
        //    等价断言：居中列右移量 < 右对齐列右移量，且两者都为正）。
        const float centerShift = alignedRow.runs[1].x - baseRow.runs[1].x;
        const float rightShift = alignedRow.runs[2].x - baseRow.runs[2].x;
        if (!(centerShift > kEps && rightShift > centerShift)) {
            std::cerr << "center/right columns must shift right, right-shift growing"
                      << " (center=" << centerShift << " right=" << rightShift << ")\n";
            return 1;
        }
        // ③ 右对齐列：内容右缘 = 列内容区右缘。列宽由组件夹逼，但同一行内
        //    "右对齐格右缘 - 左对齐格右缘 == 右移量" 恒等，且右移量等于
        //    列宽与内容宽的差（>0，因为 minWidth 夹逼保证列宽 ≥ 内容宽）。
        const float rightEdgeAligned = alignedRow.runs[2].x + alignedRow.runs[2].width;
        const float rightEdgeBase = baseRow.runs[2].x + baseRow.runs[2].width;
        if (std::fabs((rightEdgeAligned - rightEdgeBase) - rightShift) > kEps) {
            std::cerr << "right-aligned cell must keep its width while shifting"
                      << " (edge delta=" << (rightEdgeAligned - rightEdgeBase)
                      << " shift=" << rightShift << ")\n";
            return 1;
        }
        // ④ 表头行（左对齐）不受正文对齐影响：同一列计划下 x 不变。
        const Model::InputLayout::Line& alignedHeader = alignLayout.lineList()[0];
        const Model::InputLayout::Line& baseHeader = baseLayout.lineList()[0];
        for (std::size_t column = 0; column < 3; ++column) {
            if (std::fabs(alignedHeader.runs[column].x - baseHeader.runs[column].x) > kEps) {
                std::cerr << "header column " << column << " must stay put\n";
                return 1;
            }
        }
        // ⑤ 对齐后 caret 表依旧单调不减、格首 caret == 格内容起点。
        float previousCaretX = -1.0f;
        for (int offset = alignedRow.start; offset <= alignedRow.end; ++offset) {
            const float caretX = components::input_detail::caretXForDocOffset(
                alignedRow.metrics, alignedRow.holes, alignedRow.start, offset);
            if (caretX < previousCaretX - kEps) {
                std::cerr << "aligned caret x must not decrease at byte " << offset << "\n";
                return 1;
            }
            previousCaretX = caretX;
        }
        for (std::size_t column = 0; column < 3; ++column) {
            const int cellBeg = leftDecorations[2].cells[column].beg;
            const float caretX = components::input_detail::caretXForDocOffset(
                alignedRow.metrics, alignedRow.holes, alignedRow.start, cellBeg);
            if (std::fabs(caretX - alignedRow.runs[column].x) > kEps) {
                std::cerr << "caret at aligned cell " << column << " start must equal run x ("
                          << caretX << " vs " << alignedRow.runs[column].x << ")\n";
                return 1;
            }
        }
        // ⑥ 超宽截断优先于对齐：右列内容超长、窄视口把列宽压到内容宽以下时，
        //    偏移必须归零（右对齐格与全左基线同 x）、照常从列左缘截断。
        const std::string wideText =
            "| 甲甲 | 乙 | 丙 |\n"
            "| --- | --- | --- |\n"
            "| 子子 | 丑丑 | 寅寅寅寅寅寅寅寅 |";
        Model::InputState wideState;
        wideState.text = wideText;
        wideState.textRevision = 1;
        const auto wideCell = [&wideText](const std::string& needle, std::size_t from, int align) {
            const std::size_t beg = wideText.find(needle, from);
            return components::input_detail::LineCell{static_cast<int>(beg),
                                                      static_cast<int>(beg + needle.size()),
                                                      static_cast<unsigned char>(align)};
        };
        const int wideBeg = static_cast<int>(wideText.rfind('\n')) + 1;
        const int wideEnd = static_cast<int>(wideText.size());
        std::vector<components::input_detail::LineCell> wideBody = {
            wideCell("子子", wideBeg, 0), wideCell("丑丑", wideBeg, 2),
            wideCell("寅寅寅寅寅寅寅寅", wideBeg, 3)};
        std::vector<components::input_detail::LineDecoration> wideDecorations(3);
        for (auto& decoration : wideDecorations) {
            decoration.fontSize = fontSize;
            decoration.lineHeight = fontSize * 1.2f;
            decoration.tableId = 9;
        }
        wideDecorations[0].tableHeaderRow = true;
        wideDecorations[0].cells = {wideCell("甲甲", 0, 0), wideCell("乙", 0, 0), wideCell("丙", 0, 0)};
        wideDecorations[0].holes = alignHoles(0, static_cast<int>(wideText.find('\n')),
                                              wideDecorations[0].cells);
        wideDecorations[1].tableSeparator = true;
        wideDecorations[1].lineHeight = 3.0f;
        wideDecorations[1].holes = {{static_cast<int>(wideText.find('\n')) + 1, wideBeg - 1}};
        wideDecorations[2].cells = wideBody;
        wideDecorations[2].holes = alignHoles(wideBeg, wideEnd, wideBody);
        std::vector<components::input_detail::LineDecoration> wideBaseline = wideDecorations;
        for (components::input_detail::LineCell& cell : wideBaseline[2].cells) {
            cell.align = 0;
        }
        for (components::input_detail::LineCell& cell : wideBaseline[0].cells) {
            cell.align = 0;
        }
        Model::InputState wideBaseState;
        wideBaseState.text = wideText;
        wideBaseState.textRevision = 1;
        const Model::InputLayout wideAlign = Model::InputLayout::build(
            wideState, 200.0f, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &wideDecorations);
        const Model::InputLayout wideBase = Model::InputLayout::build(
            wideBaseState, 200.0f, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &wideBaseline);
        const Model::InputLayout::Line& wideAlignedRow = wideAlign.lineList()[2];
        const Model::InputLayout::Line& wideBaseRow = wideBase.lineList()[2];
        if (std::fabs(wideAlignedRow.runs[2].x - wideBaseRow.runs[2].x) > kEps) {
            std::cerr << "over-wide right-aligned cell must not shift when clipped ("
                      << wideAlignedRow.runs[2].x << " vs " << wideBaseRow.runs[2].x << ")\n";
            return 1;
        }
        float wideRight = 0.0f;
        for (const components::input_detail::TextRun& run : wideAlignedRow.runs) {
            wideRight = std::max(wideRight, run.x + run.width);
        }
        if (wideRight > 200.0f + 0.5f) {
            std::cerr << "aligned narrow table must stay clipped inside the viewport (right="
                      << wideRight << ")\n";
            return 1;
        }

        // ── 命中扩展：onImage / onGutter / lineNumber ──
        // 第 2 源行是块级图片（整行藏进洞、装饰给宽高），第 3 行是普通正文。
        const std::string imgText = "# 标题\n![图](pic.png)\n正文";
        Model::InputState imgState;
        imgState.text = imgText;
        imgState.textRevision = 1;
        const int imgLineBeg = static_cast<int>(imgText.find('\n')) + 1;
        const int imgLineEnd = static_cast<int>(imgText.find('\n', imgLineBeg));
        std::vector<components::input_detail::LineDecoration> imgDecorations(3);
        for (auto& decoration : imgDecorations) {
            decoration.fontSize = fontSize;
            decoration.lineHeight = fontSize * 1.2f;
        }
        imgDecorations[1].imagePath = "C:/nowhere/pic.png";
        imgDecorations[1].imageWidth = 100.0f;
        imgDecorations[1].imageHeight = 50.0f;
        imgDecorations[1].holes = {{imgLineBeg, imgLineEnd}};
        const Model::InputLayout imgLayout = Model::InputLayout::build(
            imgState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &imgDecorations);
        const auto topOfLine = [&imgLayout, fontSize](int index) {
            return imgLayout.geometryTable().count() > 0 ? imgLayout.geometryTable().top(index)
                                                         : static_cast<float>(index) * fontSize * 1.2f;
        };
        // 点图片中部 → onImage，行号 = 源行 2；点图右侧空白 → 不是图。
        const float imgY = bounds.y + inset + topOfLine(1) + 5.0f;
        const auto imgHit = imgLayout.pointerHit(bounds.x + inset + 50.0f, imgY, bounds, width, inset);
        if (!imgHit.onImage || imgHit.lineNumber != 2) {
            std::cerr << "pointer on image center must hit image on source line 2 (onImage="
                      << imgHit.onImage << " line=" << imgHit.lineNumber << ")\n";
            return 1;
        }
        const auto imgRightHit = imgLayout.pointerHit(bounds.x + inset + 150.0f, imgY, bounds, width, inset);
        if (imgRightHit.onImage) {
            std::cerr << "pointer right of the image must not hit the image\n";
            return 1;
        }
        // 点行号列（文本原点左侧）→ onGutter；图片行的 gutter 上 onImage 不成立。
        const auto gutterHit = imgLayout.pointerHit(bounds.x + inset - 4.0f, imgY, bounds, width, inset);
        if (!gutterHit.onGutter || gutterHit.onImage) {
            std::cerr << "pointer in the gutter must set onGutter only (gutter="
                      << gutterHit.onGutter << " image=" << gutterHit.onImage << ")\n";
            return 1;
        }
        const auto textHit = imgLayout.pointerHit(bounds.x + inset + 8.0f,
                                                  bounds.y + inset + topOfLine(2) + 5.0f,
                                                  bounds, width, inset);
        if (textHit.onGutter || textHit.onImage || textHit.lineNumber != 3) {
            std::cerr << "pointer on plain text must not be gutter/image (line="
                      << textHit.lineNumber << ")\n";
            return 1;
        }
        // 单行输入没有 gutter（横向滚动后 targetX<0 只是空区）。
        Model::InputState singleState;
        singleState.text = "single";
        singleState.textRevision = 1;
        const Model::InputLayout singleLayout = Model::InputLayout::build(
            singleState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, false, nullptr);
        const auto singleHit = singleLayout.pointerHit(bounds.x + inset - 4.0f,
                                                       bounds.y + 10.0f, bounds, width, inset);
        if (singleHit.onGutter) {
            std::cerr << "single-line input must never report onGutter\n";
            return 1;
        }
    }

    // ── S3f 批次 F：图片失败态占位盒的字段落位 ──
    // imageFailed 行：imagePath 保持空（不画真图）、宽高与路径文本透传到行上，
    // 行高 = 盒高 + 留白（几何表吃下，滚动/命中自动跟上）；点击盒内**不算** onImage
    // —— 占位盒要能点进源码改路径，这是它与真图最大的行为差异。
    {
        const std::string failText = "# 标题\n![缺](nope.png)\n正文";
        Model::InputState failState;
        failState.text = failText;
        failState.textRevision = 1;
        const int failBeg = static_cast<int>(failText.find('\n')) + 1;
        const int failEnd = static_cast<int>(failText.find('\n', failBeg));
        std::vector<components::input_detail::LineDecoration> failDecorations(3);
        for (auto& decoration : failDecorations) {
            decoration.fontSize = fontSize;
            decoration.lineHeight = fontSize * 1.2f;
        }
        failDecorations[1].imageFailed = true;
        failDecorations[1].imageWidth = 224.0f;
        failDecorations[1].imageHeight = 176.0f;
        // 行高由装饰层给（组件只照几何表排版）：盒高 + 上下留白。
        failDecorations[1].lineHeight = 176.0f + 16.0f;
        failDecorations[1].imageFailText = "nope.png";
        failDecorations[1].holes = {{failBeg, failEnd}};
        const Model::InputLayout failLayout = Model::InputLayout::build(
            failState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &failDecorations);
        const auto& failLine = failLayout.lineList()[1];
        if (!failLine.imageFailed || !failLine.imagePath.empty() ||
            failLine.imageFailText != "nope.png") {
            std::cerr << "failed-image row must carry imageFailed + text and no imagePath\n";
            return 1;
        }
        const float failRowHeight = failLayout.geometryTable().count() > 0
            ? failLayout.geometryTable().height(1) : fontSize * 1.2f;
        if (std::fabs(failRowHeight - 192.0f) > 0.01f) {
            std::cerr << "failed-image row height must be box + padding (176+16), got "
                      << failRowHeight << "\n";
            return 1;
        }
        const float failY = bounds.y + inset + (failLayout.geometryTable().count() > 0
            ? failLayout.geometryTable().top(1) : fontSize * 1.2f) + 30.0f;
        const auto failHit = failLayout.pointerHit(bounds.x + inset + 40.0f, failY, bounds, width, inset);
        if (failHit.onImage) {
            std::cerr << "pointer inside the placeholder box must not report onImage\n";
            return 1;
        }
        // 相等语义：imageFailed / imageFailText 参与 LineDecoration 比较（增量重排依赖它）。
        components::input_detail::LineDecoration failA;
        components::input_detail::LineDecoration failB;
        failA.imageFailed = true;
        if (failA == failB) {
            std::cerr << "imageFailed must participate in LineDecoration equality\n";
            return 1;
        }
        failB.imageFailed = true;
        failA.imageFailText = "a.png";
        failB.imageFailText = "b.png";
        if (failA == failB) {
            std::cerr << "imageFailText must participate in LineDecoration equality\n";
            return 1;
        }
    }

    // ── T19 视觉修正 1：3px 表格分隔行不画行号（渲染侧接线）──
    // 判据本体在 InputModel::gutterLineUsable（input_line_map.cpp 打纯分支），
    // 这里打的是 input.h 的行号循环真的调了它：同一份装饰下，普通行有 lineno 图元、
    // 分隔行（行高 3px）没有。图元 id = "<id>.lineno.<行下标>"。
    {
        const std::string sepText = "正文一\n|---|\n正文二";
        const int sepBeg = static_cast<int>(sepText.find('\n')) + 1;
        const int sepEnd = sepBeg + static_cast<int>(sepText.substr(static_cast<std::size_t>(sepBeg)).find('\n'));
        const float gutterWidth = 300.0f;
        const float gutterHeight = 240.0f;
        core::dsl::Ui gutterUi;
        gutterUi.begin("gutter");
        components::input(gutterUi, "doc")
            .size(gutterWidth, gutterHeight)
            .fontSize(16.0f)
            .fontFamily("monospace")
            .multiline()
            .lineNumbers()
            .value(sepText)
            .lineDecorator([sepBeg, sepEnd](const std::string&) {
                std::vector<components::input_detail::LineDecoration> decorations(3);
                decorations[1].tableId = 3;
                decorations[1].tableSeparator = true;
                decorations[1].lineHeight = 3.0f;
                decorations[1].holes = {{sepBeg, sepEnd}};
                // 分隔条底色：装饰层同款（有底色 → tableId/表分隔标志才盖得到行上）。
                decorations[1].box.background = core::Color{0.4f, 0.4f, 0.4f, 1.0f};
                return decorations;
            })
            .build();
        gutterUi.end();
        gutterUi.layout(gutterWidth, gutterHeight);
        if (gutterUi.find("doc.lineno.0") == nullptr || gutterUi.find("doc.lineno.2") == nullptr) {
            std::cerr << "Plain rows must keep their line numbers\n";
            return 1;
        }
        if (gutterUi.find("doc.lineno.1") != nullptr) {
            std::cerr << "3px table separator row must not draw a line number\n";
            return 1;
        }
    }

    Model::InputState scrolledState;
    scrolledState.text = "one\ntwo\nthree\nfour\nfive\nsix";
    scrolledState.textRevision = 1;
    scrolledState.cursor = static_cast<int>(scrolledState.text.size());
    scrolledState.verticalScroll = 0.0f;
    scrolledState.followCaret = false;
    Model::InputLayout::build(
        scrolledState, viewportWidth, fontSize * 2.0f, width, inset, inset, inset,
        fontSize, "monospace", fontSize, true);    if (scrolledState.verticalScroll != 0.0f) {
        std::cerr << "Manual multiline scroll was overridden by caret following\n";
        return 1;
    }

    core::dsl::Ui ui;
    ui.begin("input.viewport");
    components::input(ui, "field")
        .size(120.0f, 42.0f)
        .value("A very long value that must scroll inside the field")
        .build();
    ui.end();
    ui.layout(120.0f, 42.0f);
    const core::dsl::Element* textViewport = ui.find("field.textViewport");
    if (textViewport == nullptr || !textViewport->clip || textViewport->frame.x <= 0.0f || textViewport->frame.width >= 120.0f) {
        std::cerr << "Input text viewport did not preserve the horizontal inset\n";
        return 1;
    }

    ui.begin("input.multiline");
    components::input(ui, "multiline")
        .size(180.0f, 100.0f)
        .fontSize(20.0f)
        .multiline()
        .value("first\nsecond")
        .build();
    ui.end();
    ui.layout(180.0f, 100.0f);
    const core::dsl::Element* firstTextLine = ui.find("multiline.text.0");
    const core::dsl::Element* secondTextLine = ui.find("multiline.text.1");
    if (firstTextLine == nullptr || secondTextLine == nullptr ||
        std::fabs(secondTextLine->frame.y - firstTextLine->frame.y - 24.0f) > 0.01f) {
        std::cerr << "Multiline input did not reserve a full text line height\n";
        return 1;
    }

    // ── 选区背景框几何（buildSelectionRects，2026-09-27 修复）──────────────────
    // 断言**具体矩形**（x/y/width/height），不是"有没有选区"：
    //   ① 普通多行：y/height 跟几何表、相邻行粘连 1px、全行起点 = inset、
    //      跨行选区铺满可读宽、末行止于内容宽（正文行为必须保持稳定）；
    //   ② contentIndent + 行首图元：全行选区从**行内容起点**起画（不再硬取 0）；
    //   ③ 表格 textShiftY：背景带 = 行盒减上下内边距，两行之间留完整 padding 缝；
    //   ④ hidden：折叠行一条背景都不画（旧版会留 1px 残留条）；
    //   ⑤ readable-width：左右都收在 [inset, controlWidth - rightInset]。
    {
        const float kEps = 0.01f;
        const auto nearFloat = [kEps](float a, float b) { return std::fabs(a - b) <= kEps; };
        bool ok = true;
        // 每条矩形断言都**独立求值**再并进 ok：若写成 `ok = ok && expectRect(...)`，
        // 前一条失败后的短路会把后面的断言整段跳过（连打印都没有），排查时看不到真因。
        const auto expectRect = [&ok, nearFloat](const Model::TextSelectionRect& rect, float ex,
                                                 float ey, float ew, float eh, const char* what) {
            const bool passed = nearFloat(rect.x, ex) && nearFloat(rect.y, ey) &&
                                nearFloat(rect.width, ew) && nearFloat(rect.height, eh);
            if (!passed) {
                std::cerr << what << ": rect=(" << rect.x << ", " << rect.y << ", " << rect.width
                          << ", " << rect.height << ") expected=(" << ex << ", " << ey << ", "
                          << ew << ", " << eh << ")\n";
            }
            ok = ok && passed;
            return passed;
        };
        // 可读行宽裁剪的右界：controlWidth - rightInset（本组用例 rightInset == inset）。
        const float clipRight = width - inset;

        // ① 普通多行（无装饰，行高恒为 fontSize × 1.2）：三行全选。
        Model::InputState plainState;
        plainState.text = "alpha\nbeta\ngamma";
        plainState.textRevision = 1;
        plainState.cursor = 0;
        plainState.followCaret = false;
        plainState.selectionStart = 0;
        plainState.selectionEnd = static_cast<int>(plainState.text.size());
        const Model::InputLayout plainLayout = Model::InputLayout::build(
            plainState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true);
        const float rowHeight = fontSize * 1.2f;
        ok = ok && plainLayout.selectionRects.size() == 3;
        if (plainLayout.selectionRects.size() == 3) {
            // 行 0：起点 = inset（内容原点）、铺满可读宽、y = 行盒顶、与行 1 粘连 1px。
            expectRect(plainLayout.selectionRects[0],
                                  inset, inset + 0.0f, viewportWidth, rowHeight + 1.0f,
                                  "plain line 0 selection");
            // 行 1：同上，y 顺推一行。
            expectRect(plainLayout.selectionRects[1],
                                  inset, inset + rowHeight, viewportWidth, rowHeight + 1.0f,
                                  "plain line 1 selection");
            // 行 2（末行，选区止于行尾）：右缘 = 内容宽、不再粘连下一行。
            const float lastContentWidth = plainLayout.lineList()[2].metrics.width;
            expectRect(plainLayout.selectionRects[2],
                                  inset, inset + rowHeight * 2.0f,
                                  std::min(inset + lastContentWidth, clipRight) - inset,
                                  rowHeight,
                                  "plain line 2 selection");
        }
        // The caret and selection share a vertical center; their heights differ.
        if (!plainLayout.selectionRects.empty()) {
            ok = ok && nearFloat(plainLayout.cursorY + plainLayout.cursorLineFontSize() * 1.18f * 0.5f,
                                plainLayout.selectionRects[0].y + plainLayout.selectionRects[0].lineHeight * 0.5f);
        }

        // ② contentIndent + 行首图元：全行选区从行内容起点（advance + indent）起画。
        Model::InputState indentState;
        indentState.text = "- [ ] task\nplain";
        indentState.textRevision = 1;
        indentState.cursor = 0;
        indentState.followCaret = false;
        const int indentLineEnd = static_cast<int>(indentState.text.find('\n'));
        indentState.selectionStart = 0;
        indentState.selectionEnd = indentLineEnd + 1;
        std::vector<components::input_detail::LineDecoration> indentDecorations(2);
        indentDecorations[0].glyph.codepoint = 0xF0C8;
        indentDecorations[0].glyph.advance = 24.0f;
        indentDecorations[0].contentIndent = 16.0f;
        const Model::InputLayout indentLayout = Model::InputLayout::build(
            indentState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &indentDecorations);
        const float contentStart = 24.0f + 16.0f;  // glyph advance + contentIndent
        ok = ok && indentLayout.selectionRects.size() == 1;
        if (indentLayout.selectionRects.size() == 1) {
            expectRect(indentLayout.selectionRects[0],
                                  inset + contentStart, inset,
                                  clipRight - (inset + contentStart), rowHeight + 1.0f,
                                  "indented whole-line selection");
        }
        // 行内部分选区：起止都走 caretX（已含 40px 平移），不从 0 起算。
        Model::InputState partState;
        partState.text = indentState.text;
        partState.textRevision = 1;
        partState.cursor = 0;
        partState.followCaret = false;
        partState.selectionStart = 2;
        partState.selectionEnd = 5;
        const Model::InputLayout partLayout = Model::InputLayout::build(
            partState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &indentDecorations);
        ok = ok && partLayout.selectionRects.size() == 1;
        if (partLayout.selectionRects.size() == 1) {
            const Model::InputLayout::Line& partLine = partLayout.lineList()[0];
            const float partStartX = components::input_detail::caretXForDocOffset(
                partLine.metrics, partLine.holes, partLine.start, 2);
            const float partEndX = components::input_detail::caretXForDocOffset(
                partLine.metrics, partLine.holes, partLine.start, 5);
            ok = ok && partStartX >= contentStart;  // 图元 + 缩进确实平移过光标表
            expectRect(partLayout.selectionRects[0],
                                  inset + partStartX, inset, partEndX - partStartX,
                                  rowHeight + 1.0f,
                                  "indented partial selection");
        }

        // ③ 表格行：行盒 = 纯文字行高 + 上下内边距（textShiftY = 单侧内边距）→
        //    背景带必须 = 行盒减上下内边距：y 上移一个内边距、高减两个，
        //    两行之间留出完整的 padding 缝（不粘连）。
        const std::string shiftText = "| 甲甲 | 乙乙 |\n| 子子 | 丑丑 |";
        Model::InputState shiftState;
        shiftState.text = shiftText;
        shiftState.textRevision = 1;
        shiftState.cursor = 0;
        shiftState.followCaret = false;
        shiftState.selectionStart = 0;
        shiftState.selectionEnd = static_cast<int>(shiftText.size());
        const auto cellOf = [&shiftText](const std::string& needle, std::size_t from) {
            const std::size_t beg = shiftText.find(needle, from);
            return components::input_detail::LineCell{static_cast<int>(beg),
                                                      static_cast<int>(beg + needle.size())};
        };
        const auto gapHoles = [](int lineBeg, int lineEnd,
                                 const std::vector<components::input_detail::LineCell>& cells) {
            std::vector<components::input_detail::LineHole> holes;
            int cursor = lineBeg;
            for (const components::input_detail::LineCell& cell : cells) {
                holes.push_back({cursor, std::max(cursor, cell.beg)});
                cursor = std::max(cursor, cell.end);
            }
            holes.push_back({cursor, lineEnd});
            return holes;
        };
        const int shiftRow0End = static_cast<int>(shiftText.find('\n'));
        const int shiftRow1Beg = shiftRow0End + 1;
        const components::input_detail::LineCell headerA = cellOf("甲甲", 0);
        const components::input_detail::LineCell headerB = cellOf("乙乙", headerA.end);
        const components::input_detail::LineCell bodyA = cellOf("子子", shiftRow1Beg);
        const components::input_detail::LineCell bodyB = cellOf("丑丑", bodyA.end);
        const float tablePaddingY = 4.0f;
        const float tableTextHeight = fontSize * 1.3f;
        std::vector<components::input_detail::LineDecoration> shiftDecorations(2);
        for (components::input_detail::LineDecoration& decoration : shiftDecorations) {
            decoration.fontSize = fontSize;
            decoration.lineHeight = tableTextHeight + tablePaddingY * 2.0f;
            decoration.textShiftY = tablePaddingY;
            decoration.tableId = 5;
        }
        shiftDecorations[0].cells = {headerA, headerB};
        shiftDecorations[0].holes = gapHoles(0, shiftRow0End, shiftDecorations[0].cells);
        shiftDecorations[1].cells = {bodyA, bodyB};
        shiftDecorations[1].holes = gapHoles(shiftRow1Beg, static_cast<int>(shiftText.size()),
                                             shiftDecorations[1].cells);
        const Model::InputLayout shiftLayout = Model::InputLayout::build(
            shiftState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &shiftDecorations);
        const float tableRowHeight = tableTextHeight + tablePaddingY * 2.0f;
        ok = ok && shiftLayout.selectionRects.size() == 2;
        if (shiftLayout.selectionRects.size() == 2) {
            const Model::InputLayout::Line& tableRow0 = shiftLayout.lineList()[0];
            const Model::InputLayout::Line& tableRow1 = shiftLayout.lineList()[1];
            const float rowContentX = tableRow0.runs.empty()
                ? 0.0f : tableRow0.runs[0].x;  // 列 0 文字起点（= 单元格左内边距）
            // 行 0：y = 行盒顶 + 内边距，高 = 纯文字行高（不是整个行盒）。
            expectRect(shiftLayout.selectionRects[0],
                                  inset + rowContentX, inset + tablePaddingY,
                                  clipRight - (inset + rowContentX), tableTextHeight,
                                  "table row 0 selection band");
            // 行 1（末行）：y 顺推行盒高，右缘止于行内容宽。
            const float row1Right = std::min(
                inset + components::input_detail::caretXForDocOffset(
                    tableRow1.metrics, tableRow1.holes, tableRow1.start, tableRow1.end),
                clipRight);
            const float row1ContentX = tableRow1.runs.empty()
                ? 0.0f : tableRow1.runs[0].x;
            expectRect(shiftLayout.selectionRects[1],
                                  inset + row1ContentX, inset + tableRowHeight + tablePaddingY,
                                  row1Right - (inset + row1ContentX), tableTextHeight,
                                  "table row 1 selection band");
            // 两行背景之间必须留出完整的上下内边距（8px），不许粘成一条。
            const float gap = shiftLayout.selectionRects[1].y -
                              (shiftLayout.selectionRects[0].y + shiftLayout.selectionRects[0].height);
            if (!nearFloat(gap, tablePaddingY * 2.0f)) {
                std::cerr << "table selection bands must keep the cell padding gap, got "
                          << gap << "\n";
                ok = false;
            }
        }
        // 文字带 helper 的口径：带顶/带高就是"行盒 + textShiftY / 行盒 - 2×内边距"。
        {
            const components::input_detail::LineTextBand band =
                components::input_detail::lineTextBand(100.0f, tableRowHeight, tablePaddingY);
            ok = ok && nearFloat(band.top, 100.0f + tablePaddingY) &&
                 nearFloat(band.height, tableTextHeight);
            const components::input_detail::LineTextBand plainBand =
                components::input_detail::lineTextBand(50.0f, rowHeight, 0.0f);
            ok = ok && nearFloat(plainBand.top, 50.0f) && nearFloat(plainBand.height, rowHeight);
        }

        // ④ hidden：折叠行一条背景都不画。
        Model::InputState foldState;
        foldState.text = "第一行\n第二行\n第三行\n第四行";
        foldState.textRevision = 1;
        foldState.cursor = 0;
        foldState.followCaret = false;
        foldState.selectionStart = 0;
        foldState.selectionEnd = static_cast<int>(foldState.text.size());
        std::vector<components::input_detail::LineDecoration> foldDecorations(4);
        foldDecorations[1].hidden = true;
        foldDecorations[2].hidden = true;
        const Model::InputLayout foldLayout = Model::InputLayout::build(
            foldState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, true, &foldDecorations);
        // 旧版会给两条 0 高 hidden 行各 push 一个 1px 残留条 → 4 条；现在只有 2 条。
        ok = ok && foldLayout.selectionRects.size() == 2;
        if (foldLayout.selectionRects.size() == 2) {
            const Model::InputLayout::Line& lastFoldLine = foldLayout.lineList()[3];
            expectRect(foldLayout.selectionRects[0],
                                  inset, inset, viewportWidth, rowHeight + 1.0f,
                                  "folded line 0 selection");
            // 折叠点下方的可见行紧接其后（折叠行 0 高、共享 top），末行不粘连。
            expectRect(foldLayout.selectionRects[1],
                                  inset, inset + rowHeight,
                                  lastFoldLine.metrics.width, rowHeight,
                                  "folded line 3 selection");
            for (const Model::TextSelectionRect& rect : foldLayout.selectionRects) {
                if (rect.height <= 1.5f) {
                    std::cerr << "folded selection must not leave a 1px residue, height="
                              << rect.height << "\n";
                    ok = false;
                }
            }
        }

        // ⑤ readable-width：单行 + 横向滚动下的左右裁剪。
        std::string wideText;
        for (int index = 0; index < 60; ++index) {
            wideText += 'a';
        }
        // (a) 光标在文首（scroll = 0）、可读列很窄 → 右缘收到 controlWidth - rightInset。
        Model::InputState narrowState;
        narrowState.text = wideText;
        narrowState.textRevision = 1;
        narrowState.cursor = 0;
        narrowState.followCaret = false;
        narrowState.selectionStart = 0;
        narrowState.selectionEnd = static_cast<int>(wideText.size());
        const float narrowRightInset = 300.0f;  // 可读列 = width - inset - rightInset = 48px
        const Model::InputLayout narrowLayout = Model::InputLayout::build(
            narrowState, viewportWidth, viewportHeight, width, inset, narrowRightInset, inset,
            fontSize, "monospace", fontSize, false);
        const float narrowClipRight = width - narrowRightInset;
        ok = ok && narrowLayout.metrics.width > narrowClipRight - inset;  // 确实要裁
        ok = ok && narrowLayout.selectionRects.size() == 1;
        if (narrowLayout.selectionRects.size() == 1) {
            expectRect(narrowLayout.selectionRects[0],
                                  inset, inset, narrowClipRight - inset, fontSize,
                                  "single-line readable-width right clip");
        }
        // (b) 光标在文末（scroll > 0）→ 左缘收到 inset，不画进行号列 / 左留白。
        Model::InputState scrollState;
        scrollState.text = wideText;
        scrollState.textRevision = 1;
        scrollState.cursor = static_cast<int>(wideText.size());
        scrollState.followCaret = false;
        scrollState.selectionStart = 0;
        scrollState.selectionEnd = static_cast<int>(wideText.size());
        const Model::InputLayout scrollLayout = Model::InputLayout::build(
            scrollState, viewportWidth, viewportHeight, width, inset, inset, inset,
            fontSize, "monospace", fontSize, false);
        ok = ok && scrollLayout.scroll > 0.0f;
        ok = ok && scrollLayout.selectionRects.size() == 1;
        if (scrollLayout.selectionRects.size() == 1) {
            // 右缘未到裁剪界：x + width = inset + 内容宽 - scroll。
            expectRect(scrollLayout.selectionRects[0],
                                  inset, inset,
                                  scrollLayout.metrics.width - scrollLayout.scroll, fontSize,
                                  "single-line readable-width left clip");
        }

        if (!ok) {
            std::cerr << "selection rect geometry checks failed\n";
            return 1;
        }
    }

    std::string longChineseText;
    for (int index = 0; index < 40; ++index) {
        longChineseText += "啊";
    }
    const float previousPixelScale = core::TextPrimitive::layoutPixelScale();
    for (const float dpiScale : std::array{1.25f, 1.5f}) {
        // Mirror DslAppImpl: layout uses device ppem divided back to DIP.
        // Comparing unhinted 100% metrics scaled arithmetically against another
        // ppem does not exercise the application's actual DPI contract.
        core::TextPrimitive::setLayoutPixelScale(dpiScale);
        const auto logicalMetrics = Model::measureMetrics(longChineseText, "monospace", fontSize);
        core::TextPrimitive::setLayoutPixelScale(1.0f);
        const auto pixelMetrics = Model::measureMetrics(longChineseText, "monospace", fontSize * dpiScale);
        core::TextPrimitive::setLayoutPixelScale(previousPixelScale);
        if (logicalMetrics.caretX.size() != pixelMetrics.caretX.size()) {
            std::cerr << "DPI metrics produced different caret counts at scale " << dpiScale << "\n";
            return 1;
        }
        float maximumCaretDrift = 0.0f;
        for (std::size_t index = 0; index < logicalMetrics.caretX.size(); ++index) {
            maximumCaretDrift = std::max(
                maximumCaretDrift,
                std::fabs(logicalMetrics.caretX[index] * dpiScale - pixelMetrics.caretX[index]));
        }
        if (maximumCaretDrift > 0.5f) {
            std::cerr << "Long Chinese caret drifted by " << maximumCaretDrift
                      << " pixels at DPI scale " << dpiScale << "\n";
            return 1;
        }
    }

    return 0;
}
