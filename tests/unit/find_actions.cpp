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

    {
        eui::Ui selectedUi;
        neo::AppState selected;
        auto& editor = selectedUi.state<Model::InputState>(neo::editorInputId(selected));
        const std::string before = "foo + foo end";
        editor.text = selected.doc.text = before;
        selected.findOpen = true;
        selected.findQuery = "foo";
        selected.findReplacement = "bar";
        neo::applyFindState(selectedUi, selected);
        check(selected.findCurrent == 0 && selected.findMatches.size() == 2,
              "second-match fixture begins with first match selected");
        if (selected.findMatches.size() == 2) {
            const auto second = selected.findMatches[1];
            editor.selectionStart = second.begin;
            editor.selectionEnd = second.end;
            editor.cursor = second.end;
            selected.pendingFindAction = neo::FindAction::ReplaceOne;
            neo::applyFindState(selectedUi, selected);
            check(editor.text == "foo + bar end" && selected.doc.text == editor.text,
                  "ReplaceOne edits the manually selected second match on first click");
            check(Model::undoEdit(editor) && editor.text == before,
                  "manual second-match replacement undoes in one step");
            check(Model::redoEdit(editor) && editor.text == "foo + bar end",
                  "manual second-match replacement redoes without touching other text");
        }
    }
    {
        eui::Ui utf8Ui;
        neo::AppState utf8;
        auto& editor = utf8Ui.state<Model::InputState>(neo::editorInputId(utf8));
        const std::string before = "前🙂 搜索 / 中🙂 搜索 / 后🙂";
        editor.text = utf8.doc.text = before;
        utf8.findOpen = true;
        utf8.findQuery = "搜索";
        utf8.findReplacement = "完成🙂";
        neo::applyFindState(utf8Ui, utf8);
        check(utf8.findMatches.size() == 2, "UTF-8 fixture has two search results");
        if (utf8.findMatches.size() == 2) {
            const auto second = utf8.findMatches[1];
            editor.selectionStart = second.end; // reverse selection
            editor.selectionEnd = second.begin;
            editor.cursor = second.begin;
            utf8.pendingFindAction = neo::FindAction::ReplaceOne;
            neo::applyFindState(utf8Ui, utf8);
            const std::string after = "前🙂 搜索 / 中🙂 完成🙂 / 后🙂";
            check(editor.text == after && utf8.doc.text == after,
                  "reverse UTF-8 selection replaces only its matching source-byte span");
            check(Model::undoEdit(editor) && editor.text == before,
                  "reverse UTF-8 replacement undoes to exact original bytes");
            check(Model::redoEdit(editor) && editor.text == after,
                  "reverse UTF-8 replacement redoes to exact expected bytes");
        }
    }
    {
        eui::Ui caretUi;
        neo::AppState caret;
        auto& editor = caretUi.state<Model::InputState>(neo::editorInputId(caret));
        const std::string before = "foo + foo end";
        editor.text = caret.doc.text = before;
        caret.findOpen = true;
        caret.findQuery = "foo";
        caret.findReplacement = "bar";
        caret.findWrap = false;
        neo::applyFindState(caretUi, caret);
        check(caret.findCurrent == 0 && caret.findMatches.size() == 2,
              "caret fixture begins on first match");
        if (caret.findMatches.size() == 2) {
            const auto second = caret.findMatches[1];
            editor.cursor = editor.selectionStart = editor.selectionEnd = 5;
            caret.pendingFindAction = neo::FindAction::ReplaceOne;
            neo::applyFindState(caretUi, caret);
            check(editor.text == before && editor.selectionStart == second.begin &&
                  editor.selectionEnd == second.end && caret.findCurrent == 1,
                  "ReplaceOne first selects the next match after a manually moved caret");

            editor.cursor = editor.selectionStart = editor.selectionEnd = static_cast<int>(before.size());
            caret.pendingFindAction = neo::FindAction::ReplaceOne;
            neo::applyFindState(caretUi, caret);
            check(editor.text == before && editor.selectionStart == static_cast<int>(before.size()) &&
                  editor.selectionEnd == static_cast<int>(before.size()) &&
                  !caret.findNotice.empty(),
                  "ReplaceOne past last match without wrap leaves text and caret intact and reports end");

            caret.findWrap = true;
            caret.pendingFindAction = neo::FindAction::ReplaceOne;
            neo::applyFindState(caretUi, caret);
            check(editor.text == before && editor.selectionStart == 0 &&
                  editor.selectionEnd == 3 && caret.findCurrent == 0,
                  "ReplaceOne past last match with wrap selects first match before editing");
        }
    }
    for (const bool reverse : {false, true}) {
        eui::Ui noMatchUi;
        neo::AppState noMatch;
        auto& editor = noMatchUi.state<Model::InputState>(neo::editorInputId(noMatch));
        const std::string before = "left 中文🙂 right";
        const std::string selected = "中文🙂";
        const int begin = static_cast<int>(before.find(selected));
        const int end = begin + static_cast<int>(selected.size());
        editor.text = noMatch.doc.text = before;
        noMatch.findOpen = true;
        noMatch.findQuery = "not-present";
        neo::applyFindState(noMatchUi, noMatch);
        check(noMatch.findMatches.empty(), "no-match fixture has no search results");
        editor.selectionStart = reverse ? end : begin;
        editor.selectionEnd = reverse ? begin : end;
        editor.cursor = editor.selectionEnd;
        Model::beginEdit(editor, begin, end);
        editor.text.replace(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin), "X");
        editor.cursor = editor.selectionStart = editor.selectionEnd = begin + 1;
        Model::endEdit(editor);
        noMatch.doc.text = editor.text;
        ++noMatch.revision;
        neo::applyFindState(noMatchUi, noMatch);
        check(Model::undoEdit(editor) && editor.text == before,
              "no-match fixture undo restores original UTF-8 document");
        noMatch.doc.text = editor.text;
        ++noMatch.revision;
        const int restoredStart = editor.selectionStart;
        const int restoredEnd = editor.selectionEnd;
        check(restoredStart == (reverse ? end : begin) && restoredEnd == (reverse ? begin : end),
              "undo restores original forward or reverse UTF-8 selection before find refresh");
        neo::applyFindState(noMatchUi, noMatch);
        check(editor.selectionStart == restoredStart && editor.selectionEnd == restoredEnd,
              "no-match refresh preserves selection restored by undo");
        editor.selectionStart = restoredStart;
        editor.selectionEnd = restoredEnd;
        editor.cursor = restoredEnd;
        noMatch.findQuery = "still-not-present";
        neo::applyFindState(noMatchUi, noMatch);
        check(editor.selectionStart == restoredStart && editor.selectionEnd == restoredEnd,
              "changing to another absent query preserves unrelated manual selection");
    }
    {
        eui::Ui staleUi;
        neo::AppState stale;
        auto& editor = staleUi.state<Model::InputState>(neo::editorInputId(stale));
        editor.text = stale.doc.text = "left 中文🙂 right";
        stale.findOpen = true;
        stale.findQuery = "中文🙂";
        neo::applyFindState(staleUi, stale);
        check(stale.findCurrent == 0 && editor.selectionStart != editor.selectionEnd,
              "old-search-selection fixture begins on the actual match");
        stale.findQuery = "not-present";
        neo::applyFindState(staleUi, stale);
        check(stale.findMatches.empty() && editor.selectionStart == editor.selectionEnd &&
              editor.selectionStart == editor.cursor,
              "absent new query clears an old selected search result");
    }

    // Real queued actions over retained per-document inputs. The helper only
    // supplies fixture state; range capture, search and edits use production code.
    struct ScopeFixture {
        eui::Ui ui;
        neo::AppState state;
        Model::InputState& editor() { return ui.state<Model::InputState>(neo::editorInputId(state)); }
        explicit ScopeFixture(const std::string& text) {
            editor().text = state.doc.text = text;
            editor().textRevision = state.revision = 1;
            state.findOpen = true;
            state.findQuery = "foo";
            state.findReplacement = "bar";
        }
        void select(int begin, int end) {
            editor().selectionStart = begin; editor().selectionEnd = end;
            editor().cursor = end;
        }
        void apply() { neo::applyFindState(ui, state); }
        void action(neo::FindAction action) { state.pendingFindAction = action; apply(); }
        void scope(int begin, int end) { select(begin, end); action(neo::FindAction::SelectionScope); }
    };
    {
        ScopeFixture f("foo [foo foo] foo");
        f.scope(5, 12);
        check(f.state.findSelectionOnly && f.state.findScope.valid && f.state.findMatches.size() == 2 &&
              f.state.findMatches.front().begin == 5 && f.state.findMatches.back().end == 12,
              "selection scope includes only completely contained source matches");
        neo::selectFindMatch(f.editor(), f.state, 0);
        f.action(neo::FindAction::Previous);
        check(f.state.findCurrent == 1, "previous wraps only within selected range");
        f.action(neo::FindAction::Next);
        check(f.state.findCurrent == 0, "next wraps only within selected range");
        f.state.findWrap = false;
        f.action(neo::FindAction::Previous);
        check(f.state.findCurrent == 0 && !f.state.findNotice.empty(),
              "disabled wrap stops at the selection's first match");
        f.state.findReplacement = "lengthier";
        f.action(neo::FindAction::ReplaceOne);
        check(f.editor().text == "foo [lengthier foo] foo" && f.state.findScope.valid &&
              f.state.findScope.begin == 5 && f.state.findScope.end == 18,
              "single replacement grows range by actual UTF-8 byte delta and preserves outside text");
        f.state.findQuery = "lengthier";
        f.state.findReplacement = "x";
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "foo [x foo] foo" && f.state.findScope.end == 10,
              "replacement contraction updates the existing range without admitting outside matches");
        f.state.findQuery = "foo";
        f.action(neo::FindAction::DocumentScope);
        check(!f.state.findSelectionOnly && f.state.findMatches.size() == 3,
              "explicit document scope restores all document matches");
    }
    {
        ScopeFixture f("outside [foo foo] outside");
        f.state.findOpen = false;
        f.select(9, 16);
        neo::openFindBar(f.state);
        f.apply();
        check(f.state.findQuery == "foo foo", "opening find may prefill the original selection");
        f.state.findQuery = "foo";
        f.apply();
        check(f.editor().selectionStart == 9 && f.editor().selectionEnd == 12,
              "changed query highlights a smaller match before scope is enabled");
        f.action(neo::FindAction::SelectionScope);
        check(f.state.findScope.valid && f.state.findScope.begin == 9 && f.state.findScope.end == 16 &&
              f.state.findMatches.size() == 2,
              "scope uses original selection rather than search-generated first-match highlight");
        f.state.pendingFindRevealByte = 10;
        neo::closeFindBar(f.state);
        check(!f.state.findSelectionOnly && !f.state.findScope.valid && !f.state.findScopeCandidate.valid &&
              f.state.pendingFindRevealByte == -1 && f.state.pendingFindAction == neo::FindAction::None,
              "closing find clears active and candidate ranges and stale reveal requests");
    }
    for (bool reverse : {false, true}) {
        const std::string selected = "中文🙂 / 中文🙂";
        const std::string before = "中文🙂 [" + selected + "] 中文🙂";
        const int begin = static_cast<int>(before.find('[') + 1);
        const int end = begin + static_cast<int>(selected.size());
        ScopeFixture f(before);
        f.state.findQuery = "中文🙂";
        f.state.findReplacement = "完";
        f.scope(reverse ? end : begin, reverse ? begin : end);
        f.action(neo::FindAction::ReplaceAll);
        const std::string after = "中文🙂 [完 / 完] 中文🙂";
        check(f.editor().text == after && f.state.doc.text == after && f.state.findScope.valid &&
              f.state.findScope.begin == begin && f.state.findScope.end == begin + 9,
              "forward and reverse Unicode scopes replace exact bytes and preserve both outside matches");
        check(Model::undoEdit(f.editor()) && f.editor().text == before,
              "scoped Unicode replace-all undoes in one edit");
        f.state.doc.text = f.editor().text;
        ++f.state.revision;
        f.action(neo::FindAction::ReplaceAll);
        check(f.state.findSelectionOnly && !f.state.findScope.valid && f.state.findMatches.empty() &&
              f.editor().text == before && !f.state.findNotice.empty(),
              "undo invalidates the saved range and cannot turn queued replace into a document edit");
        f.state.findWholeWord = !f.state.findWholeWord;
        f.state.findQuery = "outside";
        f.apply();
        check(!f.state.findScope.valid && !f.state.findNotice.empty() && f.state.findMatches.empty(),
              "query and option changes retain invalid-scope warning and empty matches");
    }
    {
        ScopeFixture f("foo[foo]foo");
        f.scope(4, 7);
        f.state.findReplacement.clear();
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "foo[]foo" && f.state.findSelectionOnly && f.state.findScope.valid &&
              f.state.findScope.begin == 4 && f.state.findScope.end == 4 && f.state.findMatches.empty(),
              "deleting all selected bytes leaves a valid empty scope without selecting outside text");
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "foo[]foo", "repeating replacement on an empty scope is inert");
    }
    {
        ScopeFixture f("foo [foo] foo");
        f.scope(5, 8);
        f.editor().text.replace(5, 3, "FOO"); // Same sizes and revisions; both models bypass the public edit funnel.
        f.state.doc.text = f.editor().text;
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "foo [FOO] foo" && f.state.findSelectionOnly &&
              !f.state.findScope.valid && f.state.findMatches.empty() && !f.state.findNotice.empty(),
              "same-revision mutation of scoped bytes rejects cached destructive actions");
        f.select(0, 3);
        f.action(neo::FindAction::SelectionScope);
        check(f.state.findScope.valid && f.state.findScope.begin == 0 && f.state.findScope.end == 3 &&
              f.state.findMatches.size() == 1,
              "explicit manual reselection recovers an invalid scope");
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "bar [FOO] foo", "recovered scope edits only its newly selected bytes");
    }
    {
        ScopeFixture f("foo [foo] foo");
        f.scope(5, 8);
        f.state.doc.text[0] = 'X'; // App-side external value differs outside the selected bytes only.
        f.action(neo::FindAction::ReplaceAll);
        check(f.state.doc.text == "Xoo [foo] foo" && f.editor().text == "foo [foo] foo" &&
              f.state.findSelectionOnly && !f.state.findScope.valid && f.state.findMatches.empty(),
              "replacement cannot overwrite an app-side external value when revisions were bypassed");
    }
    {
        ScopeFixture f("(foo)");
        f.state.findWholeWord = true;
        f.scope(1, 4);
        check(f.state.findMatches.size() == 1, "whole-word scope initially has real punctuation boundaries");
        f.editor().text[4] = 'y';
        f.state.doc.text = f.editor().text; // Outside bytes can change word-boundary semantics.
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "(fooy" && f.state.findMatches.empty(),
              "queued scope action rechecks whole-word context outside unchanged selected bytes");
    }
    {
        ScopeFixture f("foo [foo] foo");
        f.scope(5, 8);
        f.editor().text += '!';
        f.state.doc.text = f.editor().text;
        ++f.editor().textRevision; ++f.state.revision;
        f.apply();
        check(f.state.findSelectionOnly && !f.state.findScope.valid && f.state.findMatches.empty(),
              "ordinary document edits invalidate scope before a subsequent search action");
    }
    {
        ScopeFixture f("left foo right");
        f.select(4, 4);
        f.action(neo::FindAction::SelectionScope);
        check(!f.state.findSelectionOnly && !f.state.findScope.valid && !f.state.findNotice.empty(),
              "empty initial selection cannot enable selection-only search");
        f.editor().text = f.state.doc.text = "中🙂foo";
        f.select(1, 3);
        f.action(neo::FindAction::SelectionScope);
        check(!f.state.findSelectionOnly && !f.state.findScope.valid,
              "range capture rejects selection endpoints inside UTF-8 code points");
    }
    {
        ScopeFixture f("foo [foo] foo");
        f.scope(5, 8);
        neo::DocumentSession first = std::move(static_cast<neo::DocumentSession&>(f.state));
        static_cast<neo::DocumentSession&>(f.state) = neo::DocumentSession{};
        f.state.tabId = 42;
        f.editor().text = f.state.doc.text = "bar [bar bar] bar";
        f.editor().textRevision = f.state.revision = 1;
        f.state.findOpen = true; f.state.findQuery = "bar"; f.state.findReplacement = "X";
        f.scope(5, 12);
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "bar [X X] bar" && f.state.findScope.end == 8,
              "second document owns independent selection bounds and replacement text");
        neo::DocumentSession second = std::move(static_cast<neo::DocumentSession&>(f.state));
        static_cast<neo::DocumentSession&>(f.state) = std::move(first);
        f.apply();
        check(f.state.tabId == 1 && f.state.findSelectionOnly && f.state.findScope.valid &&
              f.state.findScope.begin == 5 && f.state.findScope.end == 8 && f.state.findQuery == "foo" &&
              f.editor().text == "foo [foo] foo" && f.state.findMatches.size() == 1,
              "switching back restores first document's scope without second document leakage");
        check(second.findScope.valid && second.findScope.text == "X X" && second.findQuery == "bar",
              "inactive document retains its own updated scope snapshot");
    }

    {
        ScopeFixture f("foo [foo] foo");
        f.scope(5, 8);
        f.editor().compositionText = "中文🙂";
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "foo [foo] foo" && f.state.findScope.valid &&
              f.state.pendingFindAction == neo::FindAction::None,
              "queued replace during preedit cannot change committed bytes or remain pending");
        f.editor().compositionText.clear();
        f.apply();
        check(f.editor().text == "foo [foo] foo", "ending preedit does not replay a blocked replacement");
        ++f.editor().textRevision; // Model identity changed independently of app revision.
        f.action(neo::FindAction::ReplaceAll);
        check(f.editor().text == "foo [foo] foo" && !f.state.findScope.valid &&
              f.state.findSelectionOnly && f.state.findMatches.empty(),
              "model-only revision change invalidates scope before a cached action");
    }

    const std::string previousLanguage = neo::i18n::preference();
    for (const char* language : {"zh-CN", "en"}) {
      neo::i18n::setPreference(language);
      for (float font : {12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 18.0f}) {
        for (float width : {280.0f, 320.0f, 360.0f, 550.0f}) {
          for (int scopeMode : {0, 1, 2}) {
            eui::Ui compact;
            neo::AppState view;
            view.uiFontSize = font;
            view.findOpen = view.findReplaceOpen = true;
            view.findOptionsOpen = true;
            view.findSelectionOnly = scopeMode != 0;
            view.findScope.valid = scopeMode == 1;
            view.findQuery = "foo";
            compact.begin("find-layout");
            neo::findBarView(compact, view, width, 10, 10, {width+20, 400});
            // 布局断言与动画无关：这里沿用 compose 末尾那趟"关闭动画"的瞬时化，
            // 让被测树的字体与反馈状态和真实运行一致。
            neo::applyInteractionDefaults(compact, neo::kUiFontFamily, false);
            compact.end(); compact.layout({width+20, 400});
            const auto* panel = compact.find("find.bar");
            const auto* query = compact.find("find.query");
            const auto* all = compact.find("find.replace.all");
            const auto* replacement = compact.find("find.replacement");
            const auto* replace = compact.find("find.replace");
            const auto* replaceLabel = compact.find("find.replace.text");
            const auto* allLabel = compact.find("find.replace.all.text");
            const auto* close = compact.find("find.close");
            check(panel && query && all && close, "compact find controls are present");
            if (panel && query && all && close) {
                const bool fits = all->frame.x+all->frame.width <= panel->frame.x+panel->frame.width &&
                      close->frame.x+close->frame.width <= panel->frame.x+panel->frame.width &&
                      query->frame.width >= 24 &&
                      all->frame.y+all->frame.height <= panel->frame.y+panel->frame.height;
                if (!fits) {
                    std::cerr << "find layout: language=" << language << " font=" << font
                              << " width=" << width << " scope=" << scopeMode
                              << " panelRight=" << panel->frame.x+panel->frame.width
                              << " replaceAllRight=" << all->frame.x+all->frame.width
                              << " closeRight=" << close->frame.x+close->frame.width
                              << " queryWidth=" << query->frame.width << '\n';
                }
                check(fits, "compact find controls fit at narrow widths and large fonts");
                check(panel->frame.height <= 96, "find panel stays compact without explanatory rows");
            }
            check(replacement && replace && replaceLabel && allLabel, "replacement row and text labels are present");
            if (panel && replacement && replace && all && replaceLabel && allLabel) {
                check(replacement->frame.width >= 24 &&
                      replace->frame.x >= replacement->frame.x+replacement->frame.width+8 &&
                      all->frame.x >= replace->frame.x+replace->frame.width+8 &&
                      all->frame.x+all->frame.width <= panel->frame.x+panel->frame.width-8,
                      "replacement row preserves minimum input, button gaps and right inset");
                check(replaceLabel->frame.width >= core::TextPrimitive::measureTextWidth(
                          neo::i18n::tr("find.replace"), neo::uiFontFamily(view), font) &&
                      allLabel->frame.width >= core::TextPrimitive::measureTextWidth(
                          neo::i18n::tr("find.replace_all"), neo::uiFontFamily(view), font),
                      "localized replacement labels fit without truncation or smaller type");
            }
            check(!compact.find("find.hint") && !compact.find("find.title"), "search omits redundant explanatory text");
            const auto* menu = compact.find("find.options.menu.level.0");
            auto* documentScope = compact.find("find.options.menu.level.0.item.3");
            auto* selectionScope = compact.find("find.options.menu.level.0.item.4");
            check(menu && documentScope && selectionScope,
                  "search options expose document and selection scope at narrow widths");
            if (menu && documentScope && selectionScope) {
                check(menu->frame.x >= 0 && menu->frame.x + menu->frame.width <= width + 20 &&
                      selectionScope->frame.x >= menu->frame.x &&
                      selectionScope->frame.x + selectionScope->frame.width <= menu->frame.x + menu->frame.width &&
                      selectionScope->frame.y + selectionScope->frame.height <= 400,
                      "scope menu items remain inside screen and menu bounds");
                if (selectionScope->onClick) selectionScope->onClick();
                check(view.pendingFindAction == neo::FindAction::SelectionScope && !view.findOptionsOpen,
                      "selection scope menu queues its action without directly editing retained input");
                if (documentScope->onClick) documentScope->onClick();
                check(view.pendingFindAction == neo::FindAction::DocumentScope,
                      "document scope menu queues explicit return to document search");
            }
          }
        }
      }
    }
    neo::i18n::setPreference(previousLanguage);
    return failures ? 1 : 0;
}
