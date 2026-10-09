// Diagnostic preparation for the selection-background redesign. This dumps
// current geometry; it is deliberately not a passing acceptance test.
#include "components/input_model.h"
#include <iostream>
#include <iomanip>

using Model = components::input_detail::InputModel;
using Decoration = components::input_detail::LineDecoration;

void run(const char* name, const std::string& left, const std::string& right,
         float width, bool reverse, bool sourceEdges = false) {
    Model::InputState state;
    state.text = "| " + left + " | " + right + " |";
    state.textRevision = 1;
    state.followCaret = false;
    const int leftBeg = 2;
    const int leftEnd = leftBeg + static_cast<int>(left.size());
    const int rightBeg = leftEnd + 3;
    const int rightEnd = rightBeg + static_cast<int>(right.size());
    const int start = sourceEdges ? 0 : leftBeg;
    const int end = sourceEdges ? static_cast<int>(state.text.size()) : rightEnd;
    state.cursor = start;
    state.selectionStart = reverse ? end : start;
    state.selectionEnd = reverse ? start : end;
    std::vector<Decoration> decorations(1);
    auto& row = decorations.front();
    row.tableId = 0;
    row.fontSize = 16;
    row.lineHeight = 29.6f; // body 16 * 1.3 plus padding 4.4 * 2
    row.textShiftY = 4.4f;
    row.cellPadding = 5;
    row.cells = {{leftBeg, leftEnd}, {rightBeg, rightEnd}};
    row.holes = {{0, leftBeg}, {leftEnd, rightBeg},
                 {rightEnd, static_cast<int>(state.text.size())}};
    auto layout = Model::InputLayout::build(state, width, 1000, width + 24,
                                            12, 12, 12, 20.8f,
                                            "Microsoft YaHei", 16, true, &decorations);
    std::cout << "{\"case\":\"" << name << "\",\"selection\":["
              << state.selectionStart << ',' << state.selectionEnd
              << "],\"legacy_caret_lookup_range\":[" << layout.lineIndexFor(start)
              << ',' << layout.lineIndexFor(end - 1)
              << "],\"end_caret_line\":" << layout.lineIndexFor(end)
              << ",\"previous_utf8_boundary_line\":"
              << layout.lineIndexFor(Model::clampUtf8Boundary(state.text, end - 1))
              << ",\"lines\":[";
    const auto& lines = layout.lineList();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) std::cout << ',';
        const auto& line = lines[i];
        const auto band = components::input_detail::lineTextBand(
            layout.geometryTable().top(static_cast<int>(i)),
            layout.geometryTable().height(static_cast<int>(i)), line.textShiftY, line.textBandHeight);
        std::cout << "{\"index\":" << i << ",\"height\":" << line.lineHeight
                  << ",\"shift\":" << line.textShiftY << ",\"band_top\":" << band.top
                  << ",\"band_height\":" << band.height << ",\"stops\":[";
        for (size_t s = 0; s < line.tableDocCaretStops.size(); ++s) {
            if (s) std::cout << ',';
            const auto& stop = line.tableDocCaretStops[s];
            std::cout << '[' << stop.column << ',' << stop.byteIndex << ',' << stop.x << ']';
        }
        std::cout << "]}";
    }
    std::cout << "],\"rects\":[";
    for (size_t i = 0; i < layout.selectionRects.size(); ++i) {
        if (i) std::cout << ',';
        const auto& r = layout.selectionRects[i];
        std::cout << '[' << r.x << ',' << r.y << ',' << r.width << ',' << r.height << ']';
    }
    std::cout << "]}\n";
}

int main() {
    std::cout << std::fixed << std::setprecision(3);
    const std::string cn = "批量操作的写回范式五步顺序事务外壳细则见模块设计验收判断完成后为假";
    const std::string ascii = "Batch operations write back in five steps with transaction scope and validation";
    run("chinese-forward", "ui/batch_ops.py", cn, 300, false);
    run("chinese-reverse", "ui/batch_ops.py", cn, 300, true);
    run("ascii-reverse", "ui/batch_ops.py", ascii, 300, true);
    run("emoji-reverse", "ui/batch_ops.py", ascii + "😀", 300, true);
    run("chinese-source-edges", "ui/batch_ops.py", cn, 300, true, true);
    run("chinese-no-wrap", "ui/batch_ops.py", "中文字符测试", 1600, true);
    run("both-cells-wrap", cn, cn, 300, true);
    run("one-visual-line", "甲", "乙", 300, true);
    return 0; // Successful data collection does not mean the selection is correct.
}
