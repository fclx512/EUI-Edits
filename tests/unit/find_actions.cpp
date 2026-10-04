#include "ui/find_bar.h"
#include <iostream>

// Isolate persistence and notifications while exercising the real queued actions
// and the editor's undo model. No user's recovery file is touched by this test.
namespace app { void requestUpdate() {} }
namespace neo::settings {
bool writeRecovery(const std::string&, const std::string&, const textfile::Document*) { return true; }
}
namespace neo {
void showToast(AppState& state, std::string title, std::string message) {
    state.toastTitle = std::move(title);
    state.toastMessage = std::move(message);
}
}

int main() {
    using Model = components::input_detail::InputModel;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    eui::Ui ui;
    neo::AppState state;
    auto& input = ui.state<Model::InputState>(neo::kEditorInputId);
    const std::string original = "Alpha alpha ALPHA alphabet";
    input.text = state.doc.text = original;
    input.textRevision = state.revision = 1;
    state.findOpen = true;
    state.findQuery = "Alpha";
    state.findReplacement = "Beta";
    state.findWrap = false;
    neo::applyFindState(ui, state);
    check(state.findCurrent == 0, "initial match starts at document caret");
    const auto action = [&](neo::FindAction value) {
        state.pendingFindAction = value;
        neo::applyFindState(ui, state);
    };
    for (int i = 0; i < 6; ++i) action(neo::FindAction::Next);
    check(state.findCurrent == 3, "non-wrapping next stops at last match");
    action(neo::FindAction::ReplaceOne);
    check(input.text == "Alpha alpha ALPHA Betabet", "replace one acts on selected final match");
    check(Model::undoEdit(input) && input.text == original, "replace one undoes in one step");
    state.doc.text = input.text;
    ++state.revision;
    state.findWrap = true;
    neo::applyFindState(ui, state);
    neo::selectFindMatch(input, state, 0);
    action(neo::FindAction::Previous);
    check(state.findCurrent == 3, "wrapping previous returns to last match");
    action(neo::FindAction::Next);
    check(state.findCurrent == 0, "wrapping next returns to first match");
    input.cursor = input.selectionStart = input.selectionEnd = 7;
    action(neo::FindAction::Next);
    check(state.findCurrent == 2, "navigation follows a manually moved document caret");
    state.findWholeWord = true;
    action(neo::FindAction::ReplaceAll);
    check(input.text == "Beta Beta Beta alphabet", "replace all honors whole-word filter");
    check(Model::undoEdit(input) && input.text == original, "replace all undoes in one step");
    state.doc.text = input.text;
    ++state.revision;
    state.findWholeWord = false;
    state.findMatchCase = true;
    action(neo::FindAction::ReplaceAll);
    check(input.text == "Beta alpha ALPHA alphabet", "replace all honors case filter");
    check(Model::undoEdit(input) && input.text == original, "case-sensitive replace undo");
    state.doc.text = input.text;
    ++state.revision;
    state.findMatchCase = false;
    state.findReplacement.clear();
    action(neo::FindAction::ReplaceAll);
    check(input.text == "   bet", "empty replacement removes only non-overlapping matches");
    check(Model::undoEdit(input) && input.text == original, "deletion replacement undo");
    state.vaultContextMenuOpen = true;
    state.findOptionsOpen = true;
    neo::dismissFindOnEscape(state);
    check(state.findOpen && !state.findOptionsOpen && state.vaultContextMenuOpen,
          "Escape dismisses search options before closing the find panel");
    neo::dismissFindOnEscape(state);
    check(state.findOpen && !state.vaultContextMenuOpen, "Escape dismisses an overlaid menu first");
    neo::dismissFindOnEscape(state);
    check(!state.findOpen && state.findEditorFocusPending, "second Escape returns to document");
    for (float font : {14.0f, 18.0f}) {
        for (float width : {280.0f, 550.0f}) {
            eui::Ui compact;
            neo::AppState view;
            view.uiFontSize = font;
            view.findOpen = view.findReplaceOpen = true;
            compact.begin("find-layout");
            neo::findBarView(compact, view, width, 10, 10, {width+20, 400});
            // 布局断言与动画无关：这里沿用 compose 末尾那趟"关闭动画"的瞬时化，
            // 让被测树的字体与反馈状态和真实运行一致。
            neo::applyInteractionDefaults(compact, neo::kUiFontFamily, false);
            compact.end(); compact.layout({width+20, 400});
            const auto* panel = compact.find("find.bar");
            const auto* query = compact.find("find.query");
            const auto* all = compact.find("find.replace.all");
            const auto* close = compact.find("find.close");
            check(panel && query && all && close, "compact find controls are present");
            if (panel && query && all && close) {
                check(all->frame.x+all->frame.width <= panel->frame.x+panel->frame.width &&
                      close->frame.x+close->frame.width <= panel->frame.x+panel->frame.width &&
                      query->frame.width >= 24 &&
                      all->frame.y+all->frame.height <= panel->frame.y+panel->frame.height,
                      "compact find controls fit at narrow widths and large fonts");
                check(panel->frame.height <= 96, "find panel stays compact without explanatory rows");
            }
            check(!compact.find("find.hint") && !compact.find("find.title"), "search omits redundant explanatory text");
        }
    }
    return failures ? 1 : 0;
}
