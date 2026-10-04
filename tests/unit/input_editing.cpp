#include "components/input.h"
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

namespace detail = components::input_detail;
using EditModel = detail::InputModel;
using EditDecoration = detail::LineDecoration;

// 跨块装饰（标题 / 粗体 / 代码三种块）：装饰是**文本的纯函数**，与 Live Preview 同款。
//   · 标题行：行首 "# " 藏进 holes + 大字号；
//   · 粗体行：两个 "**" 藏进 holes；
//   · 代码行：反引号藏进 holes + 换等宽字体。
// 三种块同时出现，"holes 与字体变化"就都在同一篇文档里了。
std::vector<EditDecoration> blockDecorationsFor(const std::string& text) {
    std::vector<EditDecoration> table;
    int lineBeg = 0;
    while (lineBeg <= static_cast<int>(text.size())) {
        const std::size_t newline = text.find('\n', static_cast<std::size_t>(lineBeg));
        const int lineEnd = newline == std::string::npos ? static_cast<int>(text.size())
                                                         : static_cast<int>(newline);
        const std::string line = text.substr(static_cast<std::size_t>(lineBeg),
                                             static_cast<std::size_t>(lineEnd - lineBeg));
        EditDecoration decoration;
        if (line.rfind("# ", 0) == 0) {
            decoration.fontSize = 24.0f;
            decoration.lineHeight = 30.0f;
            decoration.holes.push_back({lineBeg, lineBeg + 2});
        } else {
            for (std::size_t p = line.find("**"); p != std::string::npos; p = line.find("**", p + 2)) {
                decoration.holes.push_back({lineBeg + static_cast<int>(p),
                                            lineBeg + static_cast<int>(p) + 2});
            }
            bool code = false;
            for (std::size_t p = line.find('`'); p != std::string::npos; p = line.find('`', p + 1)) {
                decoration.holes.push_back(
                    {lineBeg + static_cast<int>(p), lineBeg + static_cast<int>(p) + 1});
                code = true;
            }
            if (code) {
                decoration.fontFamily = "monospace";
            }
        }
        table.push_back(std::move(decoration));
        if (newline == std::string::npos) {
            break;
        }
        lineBeg = static_cast<int>(newline) + 1;
    }
    return table;
}

constexpr float kRegressionViewport = 480.0f;
constexpr float kRegressionFontSize = 16.0f;
const char* const kRegressionFont = "monospace";

// oracle：对**当前文本**从空状态全量 measureLines，再取前一个光标位置。
// decorations == nullptr = 纯文本全量布局（事件相位没有新装饰可参考时的口径）。
int oraclePrevIndex(const std::string& text, int cursor,
                    const std::vector<EditDecoration>* decorations) {
    const std::vector<EditModel::TextLine> lines =
        EditModel::measureLines(text, kRegressionFont, kRegressionFontSize, kRegressionViewport,
                                decorations);
    return EditModel::clampUtf8Boundary(text, EditModel::previousCaretIndex(lines, cursor));
}

// 整串是完整的 UTF-8 标量序列（不切断多字节字符）。
bool wholeUtf8Scalars(const std::string& value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        if ((lead & 0x80u) == 0x00u) {
            length = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            length = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            length = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t i = 1; i < length; ++i) {
            if ((static_cast<unsigned char>(value[index + i]) & 0xC0u) != 0x80u) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

// 行表逐字段（排版等价性用；top 由几何表统一写，不在此列）。
bool sameRuns(const std::vector<detail::TextRun>& actual,
              const std::vector<detail::TextRun>& reference) {
    if (actual.size() != reference.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].beg != reference[i].beg || actual[i].end != reference[i].end ||
            actual[i].x != reference[i].x || actual[i].width != reference[i].width ||
            !(actual[i].style == reference[i].style)) {
            return false;
        }
    }
    return true;
}

bool sameLayoutLines(const std::vector<EditModel::TextLine>& actual,
                     const std::vector<EditModel::TextLine>& reference) {
    if (actual.size() != reference.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const EditModel::TextLine& a = actual[i];
        const EditModel::TextLine& b = reference[i];
        if (a.start != b.start || a.end != b.end || a.lineNumber != b.lineNumber ||
            a.lineStart != b.lineStart || a.hardBreakAfter != b.hardBreakAfter ||
            a.holes != b.holes || !sameRuns(a.runs, b.runs) || a.fontSize != b.fontSize ||
            a.lineHeight != b.lineHeight || a.fontFamily != b.fontFamily ||
            a.metrics.caretX != b.metrics.caretX ||
            a.metrics.byteIndices != b.metrics.byteIndices ||
            a.metrics.width != b.metrics.width) {
            return false;
        }
    }
    return true;
}

// ── 同帧连按两次 Backspace：装饰别名必须在"按旧装饰重测"之前被拦住 ───────────────
// 第一次退格之后文本 revision 已推进、装饰层还没按新文本重算；同帧的第二次退格走
// prevCursorIndex → cachedLines() → ensureLayoutCache(&state.decorations)，传进来的
// 正是 state.decorations **自己**（引用别名），内容还是旧行号 / 旧字节偏移的。
// 拿旧行的 holes 按新文本测量，第二次退格就会删错字节。修复后的口径：
//   · 第一次：缓存有效（纯读）→ 位置 == "新装饰全量测量"的 oracle；
//   · 第二次：没有新装饰可参考 → 位置 == "纯文本全量布局"的保守 oracle，
//     且行表必须是无装饰的全量测量（旧行的 holes 一个都不能留）；
//   · 随后 build 递来的新装饰：布局必须回到"带装饰全量测量"的等价结果。
// 两次删掉的字节都必须是完整的 UTF-8 标量序列（样例含中文与 emoji）。
int sameFrameDoubleBackspace() {
    const std::string text =
        "# 标题文字\n"
        "**加粗**尾巴在这里\n"
        "代码 `片段` 结尾\U0001F642\n";
    int failures = 0;
    const auto fail = [&failures](const std::string& message) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    };

    const std::vector<EditDecoration> decorations = blockDecorationsFor(text);
    if (decorations.size() != 4 || decorations[0].fontSize != 24.0f ||
        decorations[1].holes.size() != 2 || decorations[2].fontFamily != "monospace" ||
        decorations[2].holes.size() != 2) {
        fail("跨块装饰样例没构造出来（标题字号 / 粗体洞 / 代码等宽+洞）：行数=" +
             std::to_string(decorations.size()) +
             " 标题字号=" + std::to_string(decorations.empty() ? 0.0f : decorations[0].fontSize) +
             " 粗体洞=" + std::to_string(decorations.size() > 1 ? decorations[1].holes.size() : 0) +
             " 代码字体=" + (decorations.size() > 2 ? decorations[2].fontFamily : std::string()) +
             " 代码洞=" + std::to_string(decorations.size() > 2 ? decorations[2].holes.size() : 0));
        return failures;
    }

    EditModel::InputState state;
    state.text = text;
    state.textRevision = 1;
    std::vector<EditDecoration> baseDecorations = decorations;
    EditModel::InputLayout::build(state, kRegressionViewport, 600.0f, kRegressionViewport, 10.0f,
                                  10.0f, 10.0f, kRegressionFontSize * 1.2f, kRegressionFont,
                                  kRegressionFontSize, true, &baseDecorations);

    // 光标落在粗体行收尾的 "**" 之前：它前面正好是一个 holes 的边界（跨块洞 + UTF-8）。
    const int cursor0 = static_cast<int>(text.find("**尾巴"));
    if (cursor0 < 0) {
        fail("样例里应能找到粗体行收尾的 **");
        return failures;
    }
    state.cursor = cursor0;
    EditModel::clearSelection(state);

    // ── 第一次退格：缓存有效（纯读），必须与"新装饰全量测量"一致 ──
    const int first = EditModel::prevCursorIndex(state, kRegressionFont, kRegressionFontSize, true,
                                                 kRegressionViewport);
    const int firstExpected = oraclePrevIndex(state.text, state.cursor, &decorations);
    if (first != firstExpected) {
        fail("第一次退格位置应与新装饰全量 oracle 一致：实际 " + std::to_string(first) +
             " 期望 " + std::to_string(firstExpected));
    }
    const std::string removedFirst = state.text.substr(
        static_cast<std::size_t>(first), static_cast<std::size_t>(state.cursor - first));
    if (!wholeUtf8Scalars(removedFirst)) {
        fail("第一次退格删掉的字节不是完整的 UTF-8 标量序列");
    }
    if (removedFirst != "粗") {
        fail("第一次退格应删掉光标前的那一个字（\"粗\"），实际 \"" + removedFirst + "\"");
    }
    EditModel::eraseRange(state, first, state.cursor);

    // ── 第二次退格：同帧、没有 build → 装饰别名 + 文本已变 ──
    const int second = EditModel::prevCursorIndex(state, kRegressionFont, kRegressionFontSize, true,
                                                  kRegressionViewport);
    const int secondExpected = oraclePrevIndex(state.text, state.cursor, nullptr);
    if (second != secondExpected) {
        fail("第二次退格位置应与纯文本全量 oracle 一致：实际 " + std::to_string(second) +
             " 期望 " + std::to_string(secondExpected));
    }
    const std::string removedSecond = state.text.substr(
        static_cast<std::size_t>(second), static_cast<std::size_t>(state.cursor - second));
    if (!wholeUtf8Scalars(removedSecond)) {
        fail("第二次退格删掉的字节不是完整的 UTF-8 标量序列（得到 \"" + removedSecond + "\")");
    }
    // 修复前这里会拿到 "**加"（旧行 holes 按新文本测量出来的错位置）。
    if (removedSecond != "加") {
        fail("第二次退格应删掉光标前的那一个字（\"加\"），实际 \"" + removedSecond + "\"");
    }
    // 行表必须是"无装饰"的全量测量：旧行的 holes / 字体一个都不能留在里面。
    if (!state.decorations.empty()) {
        fail("事件相位不该继续持有旧行号的装饰（" + std::to_string(state.decorations.size()) +
             " 项）");
    }
    const std::vector<EditModel::TextLine> plainReference =
        EditModel::measureLines(state.text, kRegressionFont, kRegressionFontSize,
                                kRegressionViewport);
    if (!sameLayoutLines(state.cachedLines, plainReference)) {
        fail("事件相位的行表应等于纯文本全量测量（旧行 holes 不得用于新行）");
    }
    EditModel::eraseRange(state, second, state.cursor);

    // ── 第三次（仍在同一帧）：装饰已清空 → 纯文本增量，口径不变 ──
    const int third = EditModel::prevCursorIndex(state, kRegressionFont, kRegressionFontSize, true,
                                                 kRegressionViewport);
    const int thirdExpected = oraclePrevIndex(state.text, state.cursor, nullptr);
    if (third != thirdExpected) {
        fail("第三次退格位置应与纯文本全量 oracle 一致：实际 " + std::to_string(third) +
             " 期望 " + std::to_string(thirdExpected));
    }
    EditModel::eraseRange(state, third, state.cursor);

    // 真实字节删除位置的端到端口径：三次退格依次删掉 "粗" / "加" / 一个 "*"。
    // 原行 "**加粗**尾巴" → "**加**尾巴" → "****尾巴" → "***尾巴"。
    const std::string expectedFinal =
        "# 标题文字\n"
        "***尾巴在这里\n"
        "代码 `片段` 结尾\U0001F642\n";
    if (state.text != expectedFinal) {
        fail("三次同帧退格后的文本与 oracle 不一致：得到 \"" + state.text + "\"");
    }

    // ── 编辑之后的 build：带新装饰的布局必须回到"全量测量"的等价结果 ──
    std::vector<EditDecoration> nextDecorations = blockDecorationsFor(state.text);
    EditModel::InputLayout::build(state, kRegressionViewport, 600.0f, kRegressionViewport, 10.0f,
                                  10.0f, 10.0f, kRegressionFontSize * 1.2f, kRegressionFont,
                                  kRegressionFontSize, true, &nextDecorations);
    const std::vector<EditModel::TextLine> decoratedReference =
        EditModel::measureLines(state.text, kRegressionFont, kRegressionFontSize,
                                kRegressionViewport, &nextDecorations);
    if (!sameLayoutLines(state.cachedLines, decoratedReference)) {
        fail("退格之后的 build 行表应与带装饰的全量测量逐字段等价");
    }

    if (failures == 0) {
        std::cout << "同帧连续退格：位置与行表都与全量 oracle 一致（删掉 \"" + removedFirst +
                         "\" / \"" + removedSecond + "\"）\n";
    }
    return failures;
}

}  // namespace

int main() {
    using Model = components::input_detail::InputModel;
    using Clock = std::chrono::steady_clock;
    if (const int failures = sameFrameDoubleBackspace()) {
        std::cerr << failures << " check(s) failed in the same-frame backspace regression\n";
        return 1;
    }
    Model::InputState edited;
    edited.text = "first paragraph\nsecond paragraph with enough characters to wrap\n中文🙂末尾\n";
    std::mt19937 random(42);
    for (int i = 0; i < 150; ++i) {
        const int position = Model::clampUtf8Boundary(edited.text, static_cast<int>(random() % (edited.text.size() + 1)));
        edited.cursor = position;
        Model::clearSelection(edited);
        if (i % 3 == 0 && position < static_cast<int>(edited.text.size())) {
            edited.selectionEnd = Model::clampUtf8Boundary(edited.text, std::min(static_cast<int>(edited.text.size()), position + 9));
            Model::eraseSelection(edited);
        } else {
            Model::insertAtCursor(edited, i % 2 ? "中文\nnew line\n" : "🙂abcdef");
        }
        const float width = i % 7 ? 120.f : 180.f;
        Model::ensureLayoutCache(edited, "monospace", 16.f, width, true);
        const auto reference = Model::measureLines(edited.text, "monospace", 16.f, width);
        if (edited.cachedLines.size() != reference.size()) {
            std::cerr << "Incremental line count differs at edit " << i << "\n";
            return 1;
        }
        for (size_t j = 0; j < reference.size(); ++j) {
            const auto& actual = edited.cachedLines[j];
            const auto& expected = reference[j];
            if (actual.start != expected.start || actual.end != expected.end ||
                actual.hardBreakAfter != expected.hardBreakAfter || actual.metrics.caretX != expected.metrics.caretX ||
                actual.metrics.byteIndices != expected.metrics.byteIndices) {
                std::cerr << "Incremental metrics differ at edit " << i << ", line " << j << "\n";
                return 2;
            }
        }
    }
    Model::InputState composition;
    composition.text = "prefix suffix";
    Model::moveCursorTo(composition, 7, false);
    composition.compositionText = "中文测试";
    if (Model::displayState(composition, true).text != "prefix 中文测试suffix" || composition.text != "prefix suffix") return 3;
    Model::displayState(composition, false);
    if (composition.preedit) return 4;
    for (const int count : {2000, 20000}) {
        Model::InputState state;
        for (int i = 0; i < count; ++i)
            state.text += std::to_string(i) + " editable text with a moderately long line of content.\n";
        ++state.textRevision;
        const auto layout = [&] {
            Model::InputLayout::build(state, 480.f, 500.f, 500.f, 10.f, 10.f, 10.f, 19.2f, "monospace", 16.f, true);
        };
        layout();
        const auto* prefixMetrics = state.cachedLines.front().metrics.caretX.data();
        const auto* suffixMetrics = state.cachedLines[state.cachedLines.size() - 2].metrics.caretX.data();
        state.cursor = Model::clampUtf8Boundary(state.text, static_cast<int>(state.text.size() / 2));
        Model::clearSelection(state);
        const auto start = Clock::now();
        for (int i = 0; i < 10; ++i) {
            // 记账已下沉到 insertAtCursor / eraseSelection（T3 增量撤销）：
            // 调用方不再预告 push，否则组合操作会拆成两条记录。
            Model::insertAtCursor(state, "x");
            layout();
            state.selectionStart = state.cursor - 1;
            state.selectionEnd = state.cursor;
            Model::eraseSelection(state);
            layout();
        }
        const auto milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (prefixMetrics != state.cachedLines.front().metrics.caretX.data() ||
            suffixMetrics != state.cachedLines[state.cachedLines.size() - 2].metrics.caretX.data()) {
            std::cerr << "Editing replaced untouched paragraph metrics\n";
            return 8;
        }
        std::cout << count << " lines: " << milliseconds / 20.0 << " ms/edit (middle insert/delete + layout + undo record)" << std::endl;
        const auto compositionStart = Clock::now();
        for (int i = 0; i < 10; ++i) {
            state.compositionText = "中文预编辑";
            auto& display = Model::displayState(state, true);
            Model::ensureLayoutCache(display, "monospace", 16.f, 480.f, true);
            if (display.cachedLines.front().metrics.caretX.data() != prefixMetrics) return 9;
            state.compositionText.clear();
            Model::displayState(state, false);
            layout();
            if (state.cachedLines.front().metrics.caretX.data() != prefixMetrics) return 10;
        }
        std::cout << count << " lines: " << std::chrono::duration<double, std::milli>(Clock::now() - compositionStart).count() / 20.0
                  << " ms/preedit update (start/cancel + layout)" << std::endl;
    }
}
