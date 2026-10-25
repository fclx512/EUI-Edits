#pragma once

#include "model/i18n.h"

#include "components/button.h"
#include "components/input.h"
#include "components/contextmenu.h"
#include "model/lp_decorations.h"
#include "model/find_search.h"
#include "state/app_actions.h"
#include "state/app_state.h"
#include "ui/metrics.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "ui/popup_presentation.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace neo {

inline constexpr const char* kFindQueryInputId = "find.query";

inline float findPanelHeight(const AppState& state) {
    const float fieldHeight = uiMetrics(state).menuFontSize + 18.0f;
    return 16.0f + fieldHeight + (state.findReplaceOpen ? fieldHeight + 8.0f : 0.0f);
}

inline void openFindBar(AppState& state, bool replace = false) {
    if (!state.findOpen) {
        state.findPrefillSelection = true;
    }
    state.findOpen = true;
    state.findOptionsOpen = false;
    if (replace) state.findReplaceOpen = true;
    state.findNotice.clear();
    state.findFocusPending = true;
    state.settingsOpen = false;
    state.openMenu = MenuKind::None;
    app::requestUpdate();
}

inline void closeFindBar(AppState& state) {
    state.findOpen = false;
    state.findOptionsOpen = false;
    state.pendingFindAction = FindAction::None;
    state.findSelectionOnly = false;
    state.findScope = state.findScopeCandidate = {};
    state.findObservedSelectionStart = state.findObservedSelectionEnd = -1;
    ++state.findScopeGeneration;
    state.pendingFindRevealByte = -1;
    state.findEditorFocusPending = true;
    app::requestUpdate();
}

inline void dismissFindOnEscape(AppState& state) {
    if (state.findOptionsOpen) {
        state.findOptionsOpen = false;
        app::requestUpdate();
    } else if (state.contextMenuOpen || state.vaultContextMenuOpen || state.openMenu != MenuKind::None) {
        state.contextMenuOpen = state.vaultContextMenuOpen = false;
        state.openMenu = MenuKind::None;
        app::requestUpdate();
    } else closeFindBar(state);
}

inline void queueFindAction(AppState& state, FindAction action) {
    state.pendingFindAction = action;
    app::requestUpdate();
}

// Literal UTF-8 search. A byte sequence counts only when both ends fall on code
// point boundaries; matches never overlap. Empty queries have no matches.
inline std::vector<FindMatch> literalMatches(const std::string& text, const std::string& query,
                                           bool matchCase = true, bool wholeWord = false,
                                           std::size_t begin = 0, std::size_t end = std::string::npos) {
    std::vector<FindMatch> matches;
    for (const auto& match : search::matches(text, query, {matchCase, wholeWord}, begin, end))
        matches.push_back({match.begin, match.end});
    return matches;
}

inline int findAtOrAfter(const std::vector<FindMatch>& matches, int byteOffset, bool wrap = true) {
    if (matches.empty()) {
        return -1;
    }
    const auto it = std::lower_bound(matches.begin(), matches.end(), byteOffset,
                                     [](const FindMatch& match, int offset) {
                                         return match.begin < offset;
                                     });
    return it == matches.end() ? (wrap ? 0 : -1) : static_cast<int>(it - matches.begin());
}

inline void selectFindMatch(components::input_detail::InputModel::InputState& input,
                            AppState& state, int index) {
    if (index < 0 || index >= static_cast<int>(state.findMatches.size())) {
        state.findCurrent = -1;
        return;
    }
    state.findCurrent = index;
    const FindMatch match = state.findMatches[static_cast<std::size_t>(index)];
    // 命中落在折叠章节里时，先把它的全部祖先标题展开，否则选中的字节区间没有可见行
    // 可以跟过去。判据在 foldstate（R8）：正文行从 sectionHeadingLine 起沿
    // parentHeadingLine 上溯 —— 旧实现直接拿 line->parentHeadingLine 当起点，而该字段
    // 只在标题行上有值、普通正文恒为 -1，于是"折叠标题后查找正文"根本没展开。
    // plan 未就绪时 revealFoldsAtByte 只排队（不做无 delta 的全量解析）；重试由
    // editorView 每帧的 applyPendingFindReveal 驱动（同一帧的 compose 里就在本函数之后，
    // 它还需要一帧时会自己 requestUpdate），所以这里不重复唤醒。
    revealFoldsAtByte(state, input, match.begin);
    input.selectionStart = match.begin;
    input.selectionEnd = match.end;
    input.cursor = match.end;
    input.dragAnchor = match.begin;
    input.selecting = false;
    input.hasPreferredCursorX = false;
    input.followCaret = true;
    state.findObservedSelectionStart = input.selectionStart;
    state.findObservedSelectionEnd = input.selectionEnd;
}

inline bool findRangeCurrent(const FindRange& range, const AppState& state,
                             const components::input_detail::InputModel::InputState& input,
                             bool verifyBytes) {
    if (!range.valid || range.revision != state.revision || range.inputRevision != input.textRevision ||
        range.documentBytes != state.doc.text.size() || range.documentBytes != input.text.size() ||
        range.begin < 0 || range.end < range.begin || static_cast<std::size_t>(range.end) > input.text.size()) return false;
    // Revisions cover normal edits, undo and external reload. Verify the actual
    // selected bytes before search rebuild/actions too, without rescanning an
    // unchanged selection on every idle frame.
    if (!verifyBytes) return true;
    const auto before = static_cast<std::size_t>(range.begin);
    const auto after = static_cast<std::size_t>(range.end);
    if (range.before.size() > before || range.after.size() > input.text.size() - after) return false;
    const auto matches = [&](const std::string& text) {
        return text.compare(range.begin, range.end - range.begin, range.text) == 0 &&
            text.compare(before - range.before.size(), range.before.size(), range.before) == 0 &&
            text.compare(after, range.after.size(), range.after) == 0;
    };
    return matches(input.text) && matches(state.doc.text);
}

inline FindRange captureFindRange(const AppState& state,
                                 const components::input_detail::InputModel::InputState& input) {
    FindRange range;
    const int begin = std::min(input.selectionStart, input.selectionEnd);
    const int end = std::max(input.selectionStart, input.selectionEnd);
    const auto boundary = [&](int byte) {
        return byte >= 0 && static_cast<std::size_t>(byte) <= input.text.size() &&
            (static_cast<std::size_t>(byte) == input.text.size() ||
             (static_cast<unsigned char>(input.text[byte]) & 0xC0) != 0x80);
    };
    if (begin >= end || !boundary(begin) || !boundary(end) || input.text != state.doc.text) return range;
    range.valid = true; range.begin = begin; range.end = end;
    range.revision = state.revision; range.inputRevision = input.textRevision;
    range.documentBytes = input.text.size();
    range.text = input.text.substr(begin, end - begin);
    auto before = static_cast<std::size_t>(begin);
    if (before) {
        --before;
        while (before && (static_cast<unsigned char>(input.text[before]) & 0xC0) == 0x80) --before;
    }
    range.before = input.text.substr(before, static_cast<std::size_t>(begin) - before);
    const auto after = static_cast<std::size_t>(end) < input.text.size()
        ? search::detail::decodeOne(input.text, end).end : static_cast<std::size_t>(end);
    range.after = input.text.substr(end, after - end);
    return range;
}

inline void refreshFindMatches(AppState& state,
                               components::input_detail::InputModel::InputState& input) {
    const bool cached = state.findMatchesRevision == state.revision &&
        state.findMatchesInputRevision == input.textRevision &&
        state.findMatchesScopeGeneration == state.findScopeGeneration &&
        state.findMatchesQuery == state.findQuery &&
        state.findMatchesCase == state.findMatchCase &&
        state.findMatchesWholeWord == state.findWholeWord;
    if (state.findSelectionOnly && !findRangeCurrent(state.findScope, state, input,
            !cached || state.pendingFindAction != FindAction::None)) {
        const bool wasValid = state.findScope.valid;
        state.findScope.valid = false;
        state.findMatches.clear(); state.findCurrent = -1;
        state.pendingFindAction = FindAction::None;
        state.pendingFindRevealByte = -1;
        state.findNotice = i18n::tr("find.scope_invalid");
        if (wasValid) showToast(state, i18n::tr("find.selection_scope"), state.findNotice);
        return;
    }
    if (cached) return;
    const bool queryChanged = state.findMatchesQuery != state.findQuery ||
        state.findMatchesCase != state.findMatchCase || state.findMatchesWholeWord != state.findWholeWord;
    const int selectedBeg = std::min(input.selectionStart, input.selectionEnd);
    const int selectedEnd = std::max(input.selectionStart, input.selectionEnd);
    int searchStart = input.cursor;
    bool selectedPreviousMatch = false;
    if (state.findCurrent >= 0 && state.findCurrent < static_cast<int>(state.findMatches.size())) {
        const auto& previous = state.findMatches[static_cast<std::size_t>(state.findCurrent)];
        selectedPreviousMatch = previous.begin == selectedBeg && previous.end == selectedEnd;
        if (selectedPreviousMatch) searchStart = selectedBeg;
    }
    state.findNotice.clear();
    state.findMatches = literalMatches(state.doc.text, state.findQuery, state.findMatchCase, state.findWholeWord,
        state.findSelectionOnly ? static_cast<std::size_t>(state.findScope.begin) : 0,
        state.findSelectionOnly ? static_cast<std::size_t>(state.findScope.end) : std::string::npos);
    state.findMatchesRevision = state.revision;
    state.findMatchesInputRevision = input.textRevision;
    state.findMatchesScopeGeneration = state.findScopeGeneration;
    state.findMatchesQuery = state.findQuery;
    state.findMatchesCase = state.findMatchCase;
    state.findMatchesWholeWord = state.findWholeWord;
    if (state.findMatches.empty()) {
        state.findCurrent = -1;
        // A query change may dismiss its old highlighted match. Document edits
        // and undo, however, own their selection even when search finds nothing.
        if (queryChanged && selectedPreviousMatch) {
            input.selectionStart = input.cursor;
            input.selectionEnd = input.cursor;
        }
        return;
    }
    for (std::size_t i = 0; i < state.findMatches.size(); ++i) {
        if (state.findMatches[i].begin == selectedBeg && state.findMatches[i].end == selectedEnd) {
            state.findCurrent = static_cast<int>(i);
            state.findObservedSelectionStart = input.selectionStart;
            state.findObservedSelectionEnd = input.selectionEnd;
            return;
        }
    }
    const int index = findAtOrAfter(state.findMatches, queryChanged ? searchStart : input.cursor, state.findWrap);
    if (queryChanged) {
        selectFindMatch(input, state, index);
    } else {
        state.findCurrent = index;
    }
}

inline void commitFindTextChange(AppState& state,
                                  components::input_detail::InputModel::InputState& input,
                                  unsigned long long beforeRevision) {
    if (input.textRevision == beforeRevision) {
        return;
    }
    state.doc.text = input.text;
    ++state.revision;
    if (state.findSelectionOnly && state.findScope.valid) {
        auto& scope = state.findScope;
        scope.end += static_cast<int>(input.text.size()) - static_cast<int>(scope.documentBytes);
        scope.documentBytes = input.text.size();
        scope.revision = state.revision; scope.inputRevision = input.textRevision;
        scope.text = input.text.substr(scope.begin, scope.end - scope.begin);
        state.findScopeCandidate = {};
    }
    state.recovered = false;
    maybeWriteRecovery(state);
    state.findMatchesRevision = static_cast<unsigned long long>(-1);
}

// Called once before editorView builds. UI event callbacks only queue actions;
// all text edits happen here while the retained editor InputState is available.
inline void applyFindState(eui::Ui& ui, AppState& state) {
    using InputModel = components::input_detail::InputModel;
    auto& input = ui.state<InputModel::InputState>(editorInputId(state));
    if (!state.findOpen) return;
    // Remember selections produced by this call. Manual drag frames need no
    // snapshot copy: capture the final range only before an action/query update
    // could consume it, or before the initial prefill selects a search result.
    struct SelectionObserver {
        AppState& state; InputModel::InputState& input;
        int begin, end;
        ~SelectionObserver() {
            if (begin != input.selectionStart || end != input.selectionEnd) {
                state.findObservedSelectionStart = input.selectionStart;
                state.findObservedSelectionEnd = input.selectionEnd;
            }
        }
    } observer{state, input, input.selectionStart, input.selectionEnd};
    const bool searchMaySelect = state.pendingFindAction != FindAction::None ||
        state.findMatchesQuery != state.findQuery || state.findMatchesCase != state.findMatchCase ||
        state.findMatchesWholeWord != state.findWholeWord;
    if (state.findPrefillSelection || (searchMaySelect &&
        (input.selectionStart != state.findObservedSelectionStart || input.selectionEnd != state.findObservedSelectionEnd))) {
        state.findScopeCandidate = captureFindRange(state, input);
    }
    if (state.findPrefillSelection) {
        state.findPrefillSelection = false;
        const int begin = std::min(input.selectionStart, input.selectionEnd);
        const int end = std::max(input.selectionStart, input.selectionEnd);
        if (end > begin && end - begin <= 256) {
            const std::string selected = input.text.substr(
                static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
            if (selected.find('\n') == std::string::npos && selected.find('\r') == std::string::npos) {
                state.findQuery = selected;
            }
        }
    }
    bool selectionScopeChanged = false;
    if (state.pendingFindAction == FindAction::SelectionScope) {
        state.pendingFindAction = FindAction::None;
        if (!input.compositionText.empty()) return;
        if (!findRangeCurrent(state.findScopeCandidate, state, input, true)) {
            state.findNotice = i18n::tr("find.scope_empty");
            showToast(state, i18n::tr("find.selection_scope"), state.findNotice);
            return;
        }
        state.findSelectionOnly = true;
        state.findScope = std::move(state.findScopeCandidate);
        state.findScopeCandidate = {};
        ++state.findScopeGeneration;
        state.findMatchesRevision = static_cast<unsigned long long>(-1);
        input.cursor = input.selectionStart = input.selectionEnd = input.dragAnchor = state.findScope.begin;
        selectionScopeChanged = true;
    } else if (state.pendingFindAction == FindAction::DocumentScope) {
        state.pendingFindAction = FindAction::None;
        state.findSelectionOnly = false; state.findScope = {};
        ++state.findScopeGeneration;
    }
    if (!input.compositionText.empty()) {
        state.pendingFindAction = FindAction::None;
        return;
    }
    refreshFindMatches(state, input);
    if (selectionScopeChanged && !state.findMatches.empty()) {
        selectFindMatch(input, state, state.findCurrent >= 0 ? state.findCurrent : 0);
    }
    const FindAction action = state.pendingFindAction;
    state.pendingFindAction = FindAction::None;
    if (action == FindAction::None || state.findMatches.empty()) {
        return;
    }
    // Refuse a stale model/app pairing before a destructive action even when a
    // caller bypassed revision updates. Never overwrite an external value.
    if ((action == FindAction::ReplaceOne || action == FindAction::ReplaceAll) && input.text != state.doc.text) {
        if (state.findSelectionOnly) {
            state.findScope.valid = false;
            state.findNotice = i18n::tr("find.scope_invalid");
            showToast(state, i18n::tr("find.selection_scope"), state.findNotice);
        }
        state.findMatches.clear(); state.findCurrent = -1;
        state.pendingFindRevealByte = -1;
        state.findMatchesRevision = static_cast<unsigned long long>(-1);
        return;
    }
    const int count = static_cast<int>(state.findMatches.size());
    if (action == FindAction::Next || action == FindAction::Previous) {
        const int direction = action == FindAction::Next ? 1 : -1;
        int index;
        const bool currentSelected = state.findCurrent >= 0 && state.findCurrent < count &&
            state.findMatches[static_cast<std::size_t>(state.findCurrent)].begin ==
                std::min(input.selectionStart, input.selectionEnd) &&
            state.findMatches[static_cast<std::size_t>(state.findCurrent)].end ==
                std::max(input.selectionStart, input.selectionEnd);
        if (currentSelected) index = state.findCurrent + direction;
        else {
            const auto it = std::lower_bound(state.findMatches.begin(), state.findMatches.end(), input.cursor,
                [](const FindMatch& match, int cursor) { return match.begin < cursor; });
            index = static_cast<int>(it - state.findMatches.begin());
            if (direction < 0) --index;
        }
        state.findNotice.clear();
        if (index < 0 || index >= count) {
            if (!state.findWrap) {
                state.findNotice = direction > 0 ? i18n::tr("find.at_last") : i18n::tr("find.at_first");
                return;
            }
            index = direction > 0 ? 0 : count - 1;
            state.findNotice = direction > 0 ? i18n::tr("find.wrap_start") : i18n::tr("find.wrap_end");
        }
        selectFindMatch(input, state, index);
        return;
    }

    const unsigned long long beforeRevision = input.textRevision;
    if (action == FindAction::ReplaceOne) {
        // Moving the document caret/selection does not invalidate the text
        // match cache. Resolve the live selection rather than its old index.
        const int selectedBeg = std::min(input.selectionStart, input.selectionEnd);
        const int selectedEnd = std::max(input.selectionStart, input.selectionEnd);
        const auto selected = std::lower_bound(state.findMatches.begin(), state.findMatches.end(), selectedBeg,
            [](const FindMatch& match, int offset) { return match.begin < offset; });
        if (selected == state.findMatches.end() || selected->begin != selectedBeg || selected->end != selectedEnd) {
            const int next = findAtOrAfter(state.findMatches, input.cursor, state.findWrap);
            state.findNotice.clear();
            selectFindMatch(input, state, next);
            if (next < 0) state.findNotice = i18n::tr("find.at_last");
            return;
        }
        state.findCurrent = static_cast<int>(selected - state.findMatches.begin());
        const FindMatch match = *selected;
        InputModel::beginEdit(input, match.begin, match.end);
        input.text.replace(static_cast<std::size_t>(match.begin),
                           static_cast<std::size_t>(match.end - match.begin), state.findReplacement);
        const int after = match.begin + static_cast<int>(state.findReplacement.size());
        input.cursor = after;
        input.selectionStart = after;
        input.selectionEnd = after;
        input.dragAnchor = after;
        InputModel::endEdit(input);
        commitFindTextChange(state, input, beforeRevision);
        refreshFindMatches(state, input);
        const int next = findAtOrAfter(state.findMatches, after);
        if (next >= 0 && (state.findWrap || state.findMatches[static_cast<std::size_t>(next)].begin >= after))
            selectFindMatch(input, state, next);
        else {
            state.findCurrent = -1;
            state.findNotice = i18n::tr("find.at_last");
        }
        return;
    }

    const int begin = state.findMatches.front().begin;
    const int end = state.findMatches.back().end;
    std::string replacement;
    replacement.reserve(static_cast<std::size_t>(end - begin));
    int cursor = begin;
    for (const FindMatch& match : state.findMatches) {
        replacement.append(input.text, static_cast<std::size_t>(cursor),
                           static_cast<std::size_t>(match.begin - cursor));
        replacement += state.findReplacement;
        cursor = match.end;
    }
    InputModel::beginEdit(input, begin, end);
    input.text.replace(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin),
                       replacement);
    input.cursor = begin + static_cast<int>(replacement.size());
    input.selectionStart = input.cursor;
    input.selectionEnd = input.cursor;
    input.dragAnchor = input.cursor;
    InputModel::endEdit(input);
    commitFindTextChange(state, input, beforeRevision);
    refreshFindMatches(state, input);
    if (input.textRevision != beforeRevision) {
        state.findNotice = i18n::format("find.replaced", {{"count", std::to_string(count)}});
        showToast(state, i18n::tr("find.replace_done"), state.findNotice);
    }
}

inline void findBarView(eui::Ui& ui, AppState& state, float width, float x, float y,
                        const eui::Screen& screen) {
    const auto& colors = editorColors();
    const float fontSize = uiMetrics(state).menuFontSize;
    const float fieldHeight = fontSize + 18.0f;
    const float height = findPanelHeight(state);
    const float top = 8.0f;
    const float replacementTop = top + fieldHeight + 8.0f;
    const float expandWidth = 28.0f;
    const float actionsWidth = fieldHeight * 4.0f;
    const float fieldX = 8.0f + expandWidth + 4.0f;
    const float fieldWidth = std::max(24.0f, width - fieldX - actionsWidth - 12.0f);
    const float navX = fieldX + fieldWidth + 4.0f;
    const std::string count = (state.findSelectionOnly ? std::string(state.findScope.valid ?
        i18n::tr("find.selection_count") : i18n::tr("find.scope_invalid_short")) + " " : std::string{}) +
                             (state.findCurrent < 0 ? "0" : std::to_string(state.findCurrent+1))+
                             " / "+std::to_string(state.findMatches.size());
    const float countWidth = ((state.findSelectionOnly && fieldWidth >= 96) || (!state.findQuery.empty() && fieldWidth >= 180))
        ? std::min(fieldWidth*.4f, core::TextPrimitive::measureTextWidth(count, uiFontFamily(state), fontSize)+12)
        : 0;
    components::InputStyle inputStyle(colors.tokens);
    inputStyle.background = inputStyle.focused = colors.editor;
    inputStyle.border = colors.border;
    inputStyle.focusBorder = colors.accent;
    inputStyle.text = colors.text;
    inputStyle.cursor = colors.accent;
    inputStyle.radius = neo::corner(colors, 5.0f);
    inputStyle.shadow = eui::Shadow{};
    const auto centerFieldText = [&](const char* id) {
        if (auto* viewport = ui.find(std::string(id) + ".textViewport")) {
            // Input centers its single line, but clips at that line's top.
            // Keep the centered origin and horizontal clipping while allowing
            // Chinese glyphs to use the field's full vertical breathing room.
            viewport->padding.top += viewport->y;
            viewport->y = 0.0f;
            viewport->height = core::SizeValue::fixed(fieldHeight);
        }
        if (auto* text = ui.find(std::string(id) + ".text")) {
            text->verticalAlign = eui::VerticalAlign::Center;
        }
    };
    const auto button = [&](const char* id, const char* label, float bx, float bw, FindAction action) {
        components::button(ui, id).position(bx, replacementTop).size(bw, fieldHeight)
            .text(label).fontSize(fontSize).radius(neo::corner(colors, 5.0f))
            .colors(colors.editor, colors.rowHover, colors.pressed)
            .textColor(colors.text).border(1, colors.border)
            .shadow(0, 0, 0, transparentColor()).preserveFocusOnPress()
            .disabled(state.findMatches.empty())
            .onClick([&state, action] { queueFindAction(state, action); }).build();
    };
    ui.stack("find.bar").position(x, y).size(width, height).zIndex(20).content([&] {
        ui.rect("find.background").fill().radius(neo::corner(colors, 9.0f)).color(colors.toolbar)
            .border(1, colors.border)
            .shadow(14, 0, 4, rgba(0, 0, 0, state.theme == ThemeMode::Light ? .12f : .24f))
            .onClick([] {}).build();
        iconButton(ui, "find.expand", state.findReplaceOpen ? UiIcon::ChevronDown : UiIcon::ChevronRight,
                   8, top + (fieldHeight-expandWidth)*.5f, expandWidth, true, [&state] {
                       state.findReplaceOpen = !state.findReplaceOpen;
                       app::requestUpdate();
                   });
        components::input(ui, kFindQueryInputId).position(fieldX, top)
            .size(fieldWidth-(countWidth > 0 ? countWidth+4 : 0), fieldHeight)
            .value(state.findQuery).placeholder(state.findSelectionOnly ?
                i18n::tr("find.selection_scope") : i18n::tr("find.find")).fontSize(fontSize)
            .fontFamily(uiFontFamily(state)).inset(10).style(inputStyle)
            .onChange([&state](const std::string& value) { state.findQuery = value; })
            .onEnter([&state] { queueFindAction(state, FindAction::Next); })
            .onEscape([&state] { dismissFindOnEscape(state); }).build();
        centerFieldText(kFindQueryInputId);
        if (countWidth > 0) {
            ui.text("find.count").position(navX-countWidth-4, top).size(countWidth, fieldHeight)
                .text(count).fontFamily(uiFontFamily(state)).fontSize(fontSize)
                .color(colors.textMuted).horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).clip().build();
        }
        if (auto* hit = ui.find(std::string(kFindQueryInputId) + ".hit")) {
            const auto original = hit->onKeyEvent;
            auto& query = ui.state<components::input_detail::InputModel::InputState>(kFindQueryInputId);
            hit->onKeyEvent = [&state, &query, original](const core::KeyEvent& event) {
                if (event.isDown() && !query.compositionText.empty()) return true;
                if (event.isDown() && event.key == eui::InputKey::Enter && event.modifiers.shift &&
                    !event.modifiers.shortcut() && !event.modifiers.alt) {
                    queueFindAction(state, FindAction::Previous);
                    return true;
                }
                return original ? original(event) : false;
            };
        }
        if (state.findFocusPending) {
            state.findFocusPending = false;
            ui.requestFocus(std::string(kFindQueryInputId) + ".hit");
            auto& input = ui.state<components::input_detail::InputModel::InputState>(kFindQueryInputId);
            input.selectionStart = 0;
            input.selectionEnd = static_cast<int>(input.text.size());
            input.cursor = input.selectionEnd;
        }
        const bool hasMatches = !state.findMatches.empty();
        iconButton(ui, "find.next", UiIcon::Next, navX, top, fieldHeight, hasMatches,
                   [&state] { queueFindAction(state, FindAction::Next); });
        iconButton(ui, "find.previous", UiIcon::Previous, navX + fieldHeight, top, fieldHeight, hasMatches,
                   [&state] { queueFindAction(state, FindAction::Previous); });
        iconButton(ui, "find.options", UiIcon::Sliders, navX + fieldHeight*2, top, fieldHeight, true,
                   [&state] {
                       if (state.findOptionsOpen) state.findOptionsOpen = false;
                       else popup_presentation::activate(state, popup_presentation::Owner::FindOptions);
                       app::requestUpdate();
                   });
        iconButton(ui, "find.close", UiIcon::Close, navX + fieldHeight*3, top, fieldHeight, true,
                   [&state] { closeFindBar(state); });
        if (state.findReplaceOpen) {
            const float replaceText = core::TextPrimitive::measureTextWidth(i18n::tr("find.replace"), uiFontFamily(state), fontSize);
            const float allText = core::TextPrimitive::measureTextWidth(i18n::tr("find.replace_all"), uiFontFamily(state), fontSize);
            // Reserve the input's minimum width, two 8px gaps and the right inset
            // before assigning button padding. English labels at 18px otherwise
            // overflow a 280px panel when the input is clamped to its minimum.
            constexpr float minimumFieldWidth = 24.0f;
            constexpr float replacementSpacing = 24.0f;
            const float buttonPadding = std::clamp((width-fieldX-minimumFieldWidth-replacementSpacing-replaceText-allText)*.5f, 8.0f, 24.0f);
            const float replaceWidth = replaceText + buttonPadding;
            const float allWidth = allText + buttonPadding;
            const float replaceField = std::max(minimumFieldWidth, width-fieldX-replaceWidth-allWidth-replacementSpacing);
            components::input(ui, "find.replacement").position(fieldX, replacementTop)
                .size(replaceField, fieldHeight).value(state.findReplacement).placeholder(i18n::tr("find.replace"))
                .fontSize(fontSize).fontFamily(uiFontFamily(state)).inset(10).style(inputStyle)
                .onChange([&state](const std::string& value) { state.findReplacement = value; })
                .onEnter([&state] { queueFindAction(state, FindAction::ReplaceOne); })
                .onEscape([&state] { dismissFindOnEscape(state); }).build();
            centerFieldText("find.replacement");
            button("find.replace", i18n::tr("find.replace"), fieldX+replaceField+8, replaceWidth, FindAction::ReplaceOne);
            button("find.replace.all", i18n::tr("find.replace_all"), fieldX+replaceField+replaceWidth+16, allWidth, FindAction::ReplaceAll);
        }
    }).build();
    using Item = components::ContextMenuItem;
    auto menuTokens = colors.tokens;
    menuTokens.metrics.typography.option = fontSize;
    auto optionsMenu = components::contextMenu(ui, "find.options.menu").open(state.findOptionsOpen)
        .theme(menuTokens).screen(screen.width, screen.height)
        .position(x+navX+fieldHeight*2-168, y+top+fieldHeight+6)
        .size(std::max(210.0f, fontSize*12+40), fontSize+18)
        .items(std::vector<Item>{Item(i18n::tr("find.case")).withChecked(state.findMatchCase),
                                Item(i18n::tr("find.whole_word")).withChecked(state.findWholeWord),
                                Item(i18n::tr("find.wrap")).withChecked(state.findWrap),
                                Item(i18n::tr("find.document_scope")).withChecked(!state.findSelectionOnly),
                                Item(i18n::tr("find.selection_scope")).withChecked(state.findSelectionOnly)});
    popup_presentation::apply(optionsMenu, state,
                          menuTokens, state.findOptionsOpen);
    optionsMenu.zIndex(1050).onSelectPath([&state](const std::vector<int>& path) {
            if (path.empty()) return;
            if (path[0] == 0) state.findMatchCase = !state.findMatchCase;
            if (path[0] == 1) state.findWholeWord = !state.findWholeWord;
            if (path[0] == 2) state.findWrap = !state.findWrap;
            if (path[0] == 3) state.pendingFindAction = FindAction::DocumentScope;
            if (path[0] == 4) state.pendingFindAction = FindAction::SelectionScope;
            state.findNotice.clear();
            state.findOptionsOpen = false;
            app::requestUpdate();
        }).onOpenChange([&state](bool open) { state.findOptionsOpen = open; app::requestUpdate(); }).build();
}

} // namespace neo
