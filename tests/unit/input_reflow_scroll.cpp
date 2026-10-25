#include "components/input.h"
#include "core/dsl_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using Model = components::input_detail::InputModel;
using State = Model::InputState;
using Schedule = components::InputWidthReflowSchedule;
using Decoration = components::input_detail::LineDecoration;

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
bool near(float a, float b) { return std::fabs(a - b) < 0.15f; }

struct Editor {
    core::dsl::Runtime runtime;
    core::dsl::Ui* ui = nullptr;
    State* state = nullptr;
    std::string text;
    std::vector<Decoration> decorations;

    explicit Editor(std::string value) : text(std::move(value)) { runtime.initialize(); }
    ~Editor() { runtime.shutdown(); }

    void compose(float width, std::uint64_t serial, bool focus = false) {
        runtime.compose("reflow-scroll", 600.0f, 240.0f,
            [&](core::dsl::Ui& view, const core::dsl::Screen&) {
                ui = &view;
                auto input = components::input(view, "editor");
                input.size(width, 120.0f).inset(12.0f)
                    .fontFamily("monospace").fontSize(16.0f)
                    .multiline(true).wordWrap(true)
                    .widthReflowFrameToken(serial)
                    .value(text);
                if (!decorations.empty()) {
                    input.lineDecorator([this](const std::string&,
                        const components::input_detail::DecoratorEditInfo&) {
                        return decorations;
                    });
                }
                input.build();
                if (focus) view.requestFocus("editor.hit");
            });
        state = &ui->state<State>("editor");
    }

    void nextSliceNow() {
        ui->state<Schedule>("editor.width-reflow-schedule").nextSliceDue =
            std::chrono::steady_clock::time_point::min();
    }
    void pauseSlices() {
        ui->state<Schedule>("editor.width-reflow-schedule").nextSliceDue =
            std::chrono::steady_clock::time_point::max();
    }
    void finish(float width, std::uint64_t& serial) {
        for (int i = 0; i < 1000 && Model::widthReflowPending(*state); ++i) {
            nextSliceNow();
            compose(width, ++serial);
        }
        check(!Model::widthReflowPending(*state), "width draft eventually publishes through InputBuilder");
    }
};

std::string document() {
    std::string value;
    for (int i = 0; i < 280; ++i)
        value += "paragraph " + std::to_string(i) + " with 中文🙂 and several words that wrap\n";
    return value;
}

void testManualScrollAnchor() {
    Editor editor(document());
    editor.compose(360.0f, 1);
    auto& state = *editor.state;
    check(state.cachedGeometry.count() > 100, "fixture has enough visual rows for scrolling");
    if (state.cachedGeometry.count() <= 100) return;
    state.followCaret = false;
    state.verticalScroll = state.cachedGeometry.top(90) + 5.0f;
    editor.pauseSlices();
    editor.compose(200.0f, 2);
    check(Model::widthReflowPending(state), "resize leaves old complete layout visible");
    if (!Model::widthReflowPending(state)) return;

    auto* hit = editor.ui->find("editor.hit");
    check(hit && static_cast<bool>(hit->onScroll), "real input wheel callback exists while pending");
    if (!hit || !hit->onScroll) return;
    const float beforeWheels = state.verticalScroll;
    core::ScrollEvent wheel;
    wheel.y = -1.0;
    for (int i = 0; i < 3; ++i) hit->onScroll(wheel);
    check(state.verticalScroll > beforeWheels + 40.0f,
          "multiple pending-frame wheel events accumulate on old geometry");
    const int oldRow = std::clamp(state.cachedGeometry.lineAtY(state.verticalScroll),
                                  0, static_cast<int>(state.cachedLines.size()) - 1);
    const int anchorByte = state.cachedLines[static_cast<std::size_t>(oldRow)].start;
    const float anchorOffset = state.verticalScroll - state.cachedGeometry.top(oldRow);

    std::uint64_t serial = 2;
    editor.finish(200.0f, serial);
    check(near(state.cachedViewportWidth, 176.0f), "new text width publishes");

    State oracle;
    Model::loadDocument(oracle, editor.text);
    oracle.wordWrap = true;
    oracle.followCaret = false;
    const auto newLayout = Model::InputLayout::build(
        oracle, 176.0f, 112.8f, 200.0f, 12.0f, 12.0f,
        5.2f, 19.2f, "monospace", 16.0f, true);
    const int anchorRow = newLayout.lineIndexFor(anchorByte);
    const float expected = std::clamp(newLayout.geometryTable().top(anchorRow) + anchorOffset,
                                      0.0f, newLayout.maxVerticalScroll);
    check(near(state.verticalScroll, expected),
          "published width maps latest manual-scroll source-byte anchor");
    check(!state.followCaret, "width publication preserves manual scrolling mode");
}

void testFollowCaretPriority() {
    Editor editor(document());
    editor.compose(360.0f, 10);
    auto& state = *editor.state;
    state.cursor = static_cast<int>(state.text.size());
    state.selectionStart = state.selectionEnd = state.cursor;
    state.followCaret = true;
    state.verticalScroll = 0.0f;
    editor.pauseSlices();
    editor.compose(200.0f, 11);
    check(Model::widthReflowPending(state), "caret-follow resize starts a draft");
    if (!Model::widthReflowPending(state)) return;
    std::uint64_t serial = 11;
    editor.finish(200.0f, serial);
    const int last = static_cast<int>(state.cachedLines.size()) - 1;
    check(last >= 0 && state.followCaret, "caret-follow state survives publication");
    if (last < 0) return;
    const float lastTop = state.cachedGeometry.top(last);
    check(lastTop >= state.verticalScroll - 0.2f &&
          lastTop < state.verticalScroll + 112.8f,
          "caret following keeps the final row visible after publication");
}

void testTableContinuationAnchor() {
    std::string text;
    for (int i = 0; i < 4; ++i)
        text += "intro row " + std::to_string(i) + "\n";
    const int sourceStart = static_cast<int>(text.size());
    std::string longCell;
    for (int i = 0; i < 110; ++i)
        longCell += "word" + std::to_string(i) + " ";
    const int cellBegin = sourceStart + 5; // "|  | " before the long cell
    const int cellEnd = cellBegin + static_cast<int>(longCell.size());
    text += "|  | " + longCell + " |\ntail";
    Editor editor(std::move(text));
    editor.decorations.resize(6);
    auto& table = editor.decorations[4];
    table.tableId = 91;
    table.cellPadding = 5.0f;
    table.cells = {{sourceStart + 2, sourceStart + 2}, {cellBegin, cellEnd}};
    table.holes = {{sourceStart, sourceStart + 2},
                   {sourceStart + 2, cellBegin},
                   {cellEnd, cellEnd + 2}};
    editor.compose(360.0f, 30);
    auto& state = *editor.state;
    std::vector<int> oldTableRows;
    for (int i = 0; i < static_cast<int>(state.cachedLines.size()); ++i)
        if (state.cachedLines[static_cast<std::size_t>(i)].tableId == 91)
            oldTableRows.push_back(i);
    check(oldTableRows.size() > 5, "long second cell has several visual table segments");
    if (oldTableRows.size() <= 5) return;
    const int oldRow = oldTableRows[4];
    const auto& oldStops = state.cachedLines[static_cast<std::size_t>(oldRow)].rare().tableDocCaretStops;
    check(!oldStops.empty() && oldStops.front().column == 1,
          "continuation has active long-cell stops, not the empty first cell");
    if (oldStops.empty()) return;
    const int anchorByte = oldStops.front().byteIndex;
    state.followCaret = false;
    state.verticalScroll = state.cachedGeometry.top(oldRow) + 4.0f;
    editor.pauseSlices();
    editor.compose(220.0f, 31);
    check(Model::widthReflowPending(state), "table resize leaves draft pending");
    if (!Model::widthReflowPending(state)) return;
    std::uint64_t serial = 31;
    editor.finish(220.0f, serial);

    const int newRow = state.cachedGeometry.lineAtY(state.verticalScroll);
    check(newRow >= 0 && newRow < static_cast<int>(state.cachedLines.size()),
          "published table anchor maps to a visual row");
    if (newRow < 0 || newRow >= static_cast<int>(state.cachedLines.size())) return;
    const auto& line = state.cachedLines[static_cast<std::size_t>(newRow)];
    check(line.tableId == 91 && line.start == sourceStart && newRow > oldTableRows.front(),
          "table continuation remains in its source row, beyond the first segment");
    int currentFirst = -1;
    for (const auto& stop : line.rare().tableDocCaretStops)
        if (stop.column == 1) { currentFirst = stop.byteIndex; break; }
    int nextFirst = -1;
    if (newRow + 1 < static_cast<int>(state.cachedLines.size())) {
        const auto& next = state.cachedLines[static_cast<std::size_t>(newRow + 1)];
        if (next.tableId == 91 && next.start == sourceStart)
            for (const auto& stop : next.rare().tableDocCaretStops)
                if (stop.column == 1) { nextFirst = stop.byteIndex; break; }
    }
    check(currentFirst >= 0 && currentFirst <= anchorByte &&
          (nextFirst < 0 || anchorByte < nextFirst),
          "new visual table segment contains the old active-cell source byte");
}

void testCancelledPreeditCallback() {
    Editor editor("first 中文🙂 line\nsecond line\nthird line");
    editor.compose(300.0f, 20, true);
    check(editor.ui->isFocused("editor.hit"), "fixture focuses the real input");
    if (!editor.ui->isFocused("editor.hit")) return;
    auto& state = *editor.state;
    state.cursor = state.selectionStart = state.selectionEnd = 6;
    core::TextInputEvent preedit;
    preedit.composing = true;
    preedit.compositionChanged = true;
    preedit.compositionText = "拼🙂";
    editor.ui->find("editor.hit")->onTextInput(preedit);
    editor.compose(300.0f, 21, true);
    check(state.preedit != nullptr, "composition has a temporary display state");
    auto* hit = editor.ui->find("editor.hit");
    check(hit && static_cast<bool>(hit->onPress), "composition frame exposes a press callback");
    if (!state.preedit || !hit || !hit->onPress) return;
    const auto stalePress = hit->onPress;
    const core::Rect bounds{hit->frame.x, hit->frame.y, hit->frame.width, hit->frame.height};

    core::TextInputEvent cancel;
    hit->onTextInput(cancel);
    editor.compose(300.0f, 22, true);
    check(state.preedit == nullptr, "cancel releases temporary display state");
    const int cursorAfterCancel = state.cursor;
    core::PointerEvent press;
    press.action = core::PointerAction::Press;
    press.button = core::PointerButton::Left;
    press.buttons = core::PointerButton::Left;
    press.x = bounds.x + 14.0f;
    press.y = bounds.y + 15.0f;
    stalePress(press, bounds);
    check(state.cursor == cursorAfterCancel,
          "retained preedit callback ignores event after display state cancellation");
}
} // namespace

int main() {
    testManualScrollAnchor();
    testFollowCaretPriority();
    testTableContinuationAnchor();
    testCancelledPreeditCallback();
    return failures ? 1 : 0;
}
