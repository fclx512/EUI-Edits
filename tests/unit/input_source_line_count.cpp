#include "components/input.h"
#include <iostream>

using Model = components::input_detail::InputModel;
using State = Model::InputState;
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}

void layout(State& state, float width, const std::vector<components::input_detail::LineDecoration>* dec = nullptr) {
    state.wordWrap = true;
    state.viewportMetrics = true;
    Model::InputLayout::build(state, width, 200, width + 16, 8, 8, 8, 19.2f,
                             "monospace", 16, true, dec);
}

int main() {
    State state;
    for (const std::string text : {"", "one", "one\n", "\n\n", "中🙂\r\nlast\n"}) {
        Model::loadDocument(state, text);
        const int expected = Model::countNewlines(text, static_cast<int>(text.size())) + 1;
        check(Model::sourceLineCount(state, text) == expected, "cold source count handles empty and trailing rows");
        layout(state, 24);
        check(Model::sourceLineCount(state, text) == expected, "wrapped cached source count equals physical rows");
    }
    std::string text;
    for (int i = 0; i < 99; ++i) text += "abcdefghijklmnopqrstuvwxyz 中🙂\n";
    text += "tail"; // 100 physical rows, many more visual segments.
    Model::loadDocument(state, text);
    layout(state, 35);
    check(state.cachedLines.size() > 100, "fixture exercises wrapped segments");
    check(Model::sourceLineCount(state, text) == 100, "visual segments must not inflate gutter digits");
    const std::string original = text;
    text[0] = '\n'; // Same length and no caller-provided revision.
    check(Model::sourceLineCount(state, text) == 101, "external same-length rewrite must reject old count");
    text = original;
    state.text = text;
    state.text[0] = '\n'; // Direct state mutation still cannot bypass content validation.
    check(Model::sourceLineCount(state, state.text) == 101, "direct rewrite cannot reuse stale layout rows");
    Model::loadDocument(state, original);
    layout(state, 35);
    state.cursor = state.selectionStart = state.selectionEnd = static_cast<int>(state.text.size());
    Model::insertAtCursor(state, "\nnew");
    check(Model::sourceLineCount(state, state.text) == 101, "committed newline invalidates cached count before layout");
    layout(state, 70);
    check(Model::sourceLineCount(state, state.text) == 101, "width reflow preserves physical count");
    Model::undoEdit(state);
    check(Model::sourceLineCount(state, state.text) == 100, "undo restores physical count before layout");
    layout(state, 50);
    Model::redoEdit(state);
    check(Model::sourceLineCount(state, state.text) == 101, "redo restores inserted row before layout");
    layout(state, 50);
    state.compositionText = "\n预编辑🙂";
    auto& display = Model::displayState(state, true);
    layout(display, 50);
    check(Model::sourceLineCount(state, state.text) == 101, "IME gutter follows committed source rows");
    state.compositionText.clear();
    Model::displayState(state, false);
    check(Model::sourceLineCount(state, state.text) == 101, "preedit cache transfer cannot supply temporary row count");
    layout(state, 50);
    std::vector<components::input_detail::LineDecoration> decorations(101);
    for (auto& d : decorations) { d.hidden = true; d.lineHeight = 0; }
    layout(state, 50, &decorations);
    check(Model::sourceLineCount(state, state.text) == 101, "hidden rows remain physical source rows");

    // A table's last physical row can end in several visual column segments.
    const std::string tableText = "| 中🙂文字文字 | tail |\n| --- | --- |\n| x | 字符字符字符 |";
    Model::loadDocument(state, tableText);
    std::vector<components::input_detail::LineDecoration> tableDecorations(3);
    int rowStart = 0;
    for (int row = 0; row < 3; ++row) {
        const auto newline = tableText.find('\n', rowStart);
        const int end = newline == std::string::npos ? static_cast<int>(tableText.size()) : static_cast<int>(newline);
        const int middle = static_cast<int>(tableText.find('|', rowStart + 1));
        auto& d = tableDecorations[row];
        d.tableId = 0; d.fontSize = 16; d.lineHeight = 24; d.cellPadding = 5;
        d.tableHeaderRow = row == 0;
        d.tableSeparator = row == 1;
        if (row == 1) d.holes = {{rowStart, end}};
        else {
            d.cells = {{rowStart + 2, middle - 1}, {middle + 2, end - 2}};
            d.holes = {{rowStart, rowStart + 2}, {middle - 1, middle + 2}, {end - 2, end}};
        }
        rowStart = end + 1;
    }
    layout(state, 60, &tableDecorations);
    check(state.cachedLines.size() > 3, "table fixture exercises column wrapping");
    check(Model::sourceLineCount(state, tableText) == 3, "table visual segments preserve final source row number");

    // Actual InputBuilder gutter digits across the 99 -> 100 source-row boundary.
    core::dsl::Ui ui;
    std::string document(98, '\n');
    const auto compose = [&] {
        ui.begin("gutter-count");
        components::input(ui, "editor").size(400, 180).multiline(true).lineNumbers(true)
            .fontFamily("monospace").fontSize(16).valueRef(document)
            .transition(core::Transition::none()).build();
        ui.end();
        ui.layout(core::dsl::Screen{400, 180});
        return ui.find("editor.textViewport")->frame.x;
    };
    const float initial = compose();
    check(compose() == initial, "warm gutter width stays stable");
    document += '\n';
    const float wider = compose();
    check(wider > initial, "100th source row widens gutter on the same frame as external edit");
    document.erase(document.size() - 1);
    check(compose() == initial, "removing 100th row restores gutter width immediately");
    return failures ? 1 : 0;
}
