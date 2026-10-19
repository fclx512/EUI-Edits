#include "components/input.h"
#include <cmath>
#include <iostream>

int main() {
    using M = components::input_detail::InputModel;
    using K = core::InputKey;
    core::dsl::Ui ui;
    std::string value;
    for (int i = 0; i < 100; ++i) value += "中文 😀 alpha beta gamma delta epsilon\n";
    bool multiline = true;
    auto compose = [&](float width = 350.f) {
        ui.begin("navigation");
        components::input(ui, "field").size(width, 180.f).inset(10).fontSize(16)
            .multiline(multiline).scrollbar(true).value(value).build();
        ui.end(); ui.layout(width, 180.f);
    };
    compose();
    auto& state = ui.state<M::InputState>("field");
    auto key = [&](K code, bool control = false, bool shift = false) {
        core::KeyEvent event;
        event.key = code; event.action = core::KeyAction::Press;
        event.modifiers.control = control; event.modifiers.shift = shift;
        return ui.find("field.hit")->onKeyEvent(event);
    };
    const auto revision = state.textRevision;
    if (!key(K::Home, true) || state.cursor != 0) return 1;
    compose();
    if (state.verticalScroll != 0) return 2;
    if (!key(K::End, true, true) || state.cursor != static_cast<int>(value.size()) ||
        state.selectionStart != 0 || state.selectionEnd != state.cursor) return 3;
    compose();
    if (state.verticalScroll <= 0) return 4;
    key(K::Home, true);
    compose();
    key(K::PageDown, false, true);
    const int pagePosition = state.cursor;
    if (pagePosition <= 0 || state.selectionStart != 0 || state.selectionEnd != pagePosition ||
        M::clampUtf8Boundary(value, pagePosition) != pagePosition) return 5;
    compose();
    const int pageLine = M::lineIndexFor(state.cachedLines, pagePosition);
    const float viewport = ui.find("field.textViewport")->frame.height;
    if (state.cachedLines[pageLine].top < viewport * .5f ||
        state.cachedLines[pageLine].top > viewport + 1) return 6;
    key(K::PageUp, false, true);
    if (state.cursor != 0 || state.selectionStart != 0 || state.selectionEnd != 0) return 7;
    key(K::PageDown);
    compose();
    if (state.cursor != pagePosition || M::hasTextSelection(state)) return 8;
    key(K::End);
    const int lineEnd = state.cursor;
    if (lineEnd >= static_cast<int>(value.size()) || lineEnd <= pagePosition) return 9;
    key(K::Home);
    if (state.cursor >= lineEnd || state.cursor == 0) return 10;
    for (int i = 0; i < 200; ++i) { key(K::PageDown); compose(); }
    if (M::lineIndexFor(state.cachedLines, state.cursor) != static_cast<int>(state.cachedLines.size()) - 1) return 11;
    for (int i = 0; i < 200; ++i) { key(K::PageUp); compose(); }
    if (M::lineIndexFor(state.cachedLines, state.cursor) != 0) return 12;
    // Wrapping uses visual geometry, rather than counting source newlines.
    key(K::Home, true); compose(120);
    if (state.cachedLines.size() <= 101) return 13;
    key(K::PageDown); compose(120);
    if (state.cursor <= 0 || state.cursor >= static_cast<int>(value.size())) return 14;
    // Navigation leaves text/revision/undo untouched; IME owns candidate paging.
    if (state.text != value || state.textRevision != revision) return 15;
    state.compositionText = "拼音";
    const auto cursor = state.cursor;
    const auto selectionStart = state.selectionStart;
    const auto selectionEnd = state.selectionEnd;
    for (K code : {K::Home, K::End, K::PageDown, K::PageUp})
        // Consume at the component boundary so an app shortcut cannot receive
        // candidate keys. The native backend already delegates them to the IME.
        if (!key(code, true) || state.cursor != cursor ||
            state.selectionStart != selectionStart || state.selectionEnd != selectionEnd ||
            state.compositionText != "拼音" || state.textRevision != revision) return 16;
    state.compositionText.clear();
    multiline = false; compose();
    if (key(K::PageDown) || key(K::PageUp)) return 17;
    key(K::End, true); key(K::Home, true, true);
    if (state.cursor != 0 || state.selectionStart != static_cast<int>(value.size())) return 18;
    // 单行输入：↑/↓ 折到行首/行尾并消费事件（原先无响应）；Shift 沿用选区扩展语义。
    if (!key(K::Up) || state.cursor != 0 || M::hasTextSelection(state)) return 21;
    if (!key(K::Down) || state.cursor != static_cast<int>(value.size()) || M::hasTextSelection(state)) return 22;
    if (!key(K::Up, false, true) || state.cursor != 0 ||
        state.selectionStart != static_cast<int>(value.size()) || state.selectionEnd != 0) return 23;

    // Hidden lines do not consume page distance and cannot be landing targets.
    M::InputState hidden;
    hidden.text = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n";
    std::vector<components::input_detail::LineDecoration> decorations(11);
    for (int i = 1; i <= 7; ++i) decorations[i].hidden = true;
    M::ensureLayoutCache(hidden, "monospace", 16, 200, true, &decorations);
    M::moveCursorPage(hidden, 1, false, "monospace", 16, 200, 40);
    const int target = M::lineIndexFor(hidden.cachedLines, hidden.cursor);
    if (target < 8 || hidden.cachedLines[target].hidden) return 19;
    M::moveCursorPage(hidden, -1, true, "monospace", 16, 200, 40);
    if (hidden.cursor != 0 || hidden.selectionStart <= 0 || hidden.selectionEnd != 0) return 20;

    // 边界折行跳转（记事本惯例）：第一行 ↑ / 最后一行 ↓ 折到当前视觉行的行首/行尾，
    // 已在边缘则保持原样；行语义与正常上下移动一致（折行后的视觉行）。
    M::InputState edge;
    edge.text = "alpha\nbeta\ngamma";  // 行区间：0-5 / 6-10 / 11-16
    M::moveCursorVertical(edge, -1, false, "monospace", 16, 200, 40);
    if (edge.cursor != 0) return 24;  // 第一行行首 ↑：无操作
    edge.cursor = 2;
    M::moveCursorVertical(edge, -1, false, "monospace", 16, 200, 40);
    if (edge.cursor != 0) return 25;  // 第一行中间 ↑：折到行首
    edge.cursor = 2;
    M::moveCursorVertical(edge, -1, true, "monospace", 16, 200, 40);
    if (edge.cursor != 0 || edge.selectionStart != 2 || edge.selectionEnd != 0) return 26;  // Shift+↑ 扩选到行首
    edge.cursor = static_cast<int>(edge.text.size());
    M::moveCursorVertical(edge, 1, false, "monospace", 16, 200, 40);
    if (edge.cursor != static_cast<int>(edge.text.size())) return 27;  // 最后一行行尾 ↓：无操作
    edge.cursor = 13;
    M::moveCursorVertical(edge, 1, false, "monospace", 16, 200, 40);
    if (edge.cursor != static_cast<int>(edge.text.size())) return 28;  // 最后一行中间 ↓：折到行尾
    edge.cursor = 13;
    M::moveCursorVertical(edge, 1, true, "monospace", 16, 200, 40);
    if (edge.cursor != static_cast<int>(edge.text.size()) || edge.selectionStart != 13 ||
        edge.selectionEnd != static_cast<int>(edge.text.size())) return 29;  // Shift+↓ 扩选到行尾

    // A soft wrap gives the previous row's end and the next row's start the
    // same source offset. Left must pass that shared stop, while Right must
    // return to it; both directions must use projected UTF-8 caret boundaries.
    auto checkSoftWrap = [&](bool markdown) {
        M::InputState wrapped;
        wrapped.text = markdown ? "**中文测试😀中文测试 alpha beta gamma**"
                                : "中文测试😀中文测试 alpha beta gamma";
        std::vector<components::input_detail::LineDecoration> rows;
        if (markdown) {
            rows.resize(1);
            rows[0].holes = {{0, 2}, {static_cast<int>(wrapped.text.size()) - 2,
                                         static_cast<int>(wrapped.text.size())}};
        }
        constexpr float width = 45.f;
        M::ensureLayoutCache(wrapped, "monospace", 16.f, width, true,
                             markdown ? &rows : nullptr);
        int boundary = -1;
        int previous = -1;
        for (std::size_t i = 1; i < wrapped.cachedLines.size(); ++i) {
            const auto& prior = wrapped.cachedLines[i - 1];
            const auto& next = wrapped.cachedLines[i];
            if (prior.hardBreakAfter || prior.end != next.start) continue;
            const int candidate = components::input_detail::previousVisibleCaret(
                prior.metrics, prior.presentation().holes, prior.start, prior.end);
            if (candidate < prior.end && prior.end - candidate > 1) {
                boundary = prior.end;
                previous = candidate;
                break;
            }
        }
        if (boundary < 0 || M::lineIndexFor(wrapped.cachedLines, boundary) == 0) return false;
        wrapped.cursor = boundary;
        M::moveCursor(wrapped, -1, false, "monospace", 16.f, true, width);
        if (wrapped.cursor != previous || M::clampUtf8Boundary(wrapped.text, wrapped.cursor) != wrapped.cursor)
            return false;
        M::moveCursor(wrapped, 1, false, "monospace", 16.f, true, width);
        if (wrapped.cursor != boundary || M::hasTextSelection(wrapped)) return false;
        M::moveCursor(wrapped, -1, true, "monospace", 16.f, true, width);
        if (wrapped.cursor != previous || wrapped.selectionStart != boundary ||
            wrapped.selectionEnd != previous) return false;
        M::moveCursor(wrapped, 1, true, "monospace", 16.f, true, width);
        if (wrapped.cursor != boundary || wrapped.selectionStart != boundary ||
            wrapped.selectionEnd != boundary || M::hasTextSelection(wrapped)) return false;
        return true;
    };
    if (!checkSoftWrap(false)) return 30;
    if (!checkSoftWrap(true)) return 31;

    // The byte after a hard newline has a distinct stop before the newline.
    M::InputState hardBreak;
    hardBreak.text = "ab\n中";
    hardBreak.cursor = 3;
    M::moveCursor(hardBreak, -1, false, "monospace", 16.f, true, 45.f);
    if (hardBreak.cursor != 2) return 32;
    M::moveCursor(hardBreak, 1, false, "monospace", 16.f, true, 45.f);
    if (hardBreak.cursor != 3) return 33;

    // Inline backgrounds occupy layout space without inserting source bytes.
    // Exercise hidden markers, adjacent punctuation/chips and split UTF-8 runs.
    for (float scale : {1.0f, 1.25f, 1.5f}) {
        core::TextPrimitive::setLayoutPixelScale(scale);
        for (float width : {600.0f, 65.0f, 41.0f}) {
            M::InputState chipped;
            chipped.text = "`中文😀alpha`的`next`，tail";
            const int close = static_cast<int>(chipped.text.find('`', 1));
            const int second = static_cast<int>(chipped.text.find('`', close + 1));
            const int secondEnd = static_cast<int>(chipped.text.find('`', second + 1));
            std::vector<components::input_detail::LineDecoration> decorations(1);
            decorations[0].holes = {{0, 1}, {close, close + 1}, {second, second + 1}, {secondEnd, secondEnd + 1}};
            components::input_detail::LineRunStyle chip;
            chip.background = core::Color{.9f, .9f, .9f, 1};
            chip.borderWidth = 1;
            chip.chipPadding = 3;
            decorations[0].runs = {{1, close, chip}, {second + 1, secondEnd, chip}};
            auto layout = M::InputLayout::build(chipped, width, 600, width, 0, 0, 0, 20,
                                                "monospace", 16, true, &decorations);
            for (int row = 0; row < static_cast<int>(chipped.cachedLines.size()); ++row) {
                M::ensureLineDetails(chipped, row);
                const auto& line = chipped.cachedLines[row];
                if (line.metrics.width > width + .1f) {
                    std::cerr << "chip wrap overflow " << line.metrics.width << "/" << width << '\n'; return 34;
                }
                float previousRight = 0;
                for (const auto& run : line.presentation().runs) {
                    const float pad = components::input_detail::inlineChipPadding(run.style.get(), 16);
                    const float left = run.x - pad;
                    const float right = run.x + run.width + pad;
                    if (left < previousRight - .05f || right > line.metrics.width + .05f) return 35;
                    previousRight = right;
                    // Padding hit testing must resolve to a valid source boundary.
                    for (float x : {left, run.x, right}) {
                        const int hit = layout.closestCaret(row, x);
                        if (hit < line.start || hit > line.end || M::clampUtf8Boundary(chipped.text, hit) != hit) return 36;
                    }
                }
                const auto& stops = line.metrics.caretX();
                if (!std::is_sorted(stops.begin(), stops.end())) return 37;
                if (row > 0) {
                    chipped.cursor = line.start;
                    M::moveCursor(chipped, -1, false, "monospace", 16, true, width);
                    if (chipped.cursor >= line.start) return 38;
                    M::moveCursor(chipped, 1, false, "monospace", 16, true, width);
                    if (chipped.cursor != line.start) return 39;
                }
            }
            if (chipped.text != "`中文😀alpha`的`next`，tail") return 40;
            // Coarse and detailed no-wrap paths must reserve identical spacing.
            const auto exact = M::measureLines(chipped.text, "monospace", 16, 0, &decorations);
            const auto coarse = M::measureLines(chipped.text, "monospace", 16, 0, &decorations,
                                                nullptr, nullptr, nullptr, nullptr, true);
            if (std::fabs(exact[0].metrics.width - coarse[0].metrics.width) > .05f) return 41;
        }
    }
    core::TextPrimitive::setLayoutPixelScale(1);
    std::cout << "Navigation: document edges, viewport paging, selection, UTF-8, soft/hard wrapping, chip spacing, folding and IME passed\n";
}
