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
    std::cout << "Navigation: document edges, viewport paging, selection, UTF-8, wrapping, folding and IME passed\n";
}
