#include "ui/context_menu.h"

#include <iostream>
#include <memory>
#include <string>

// The menu action is never invoked here; keep this overlay test independent
// of the complete application action dispatcher.
namespace app { void requestUpdate() {} }
namespace neo {
void openLinkTarget(AppState&, std::string, const std::string&) {}
}

namespace {

using Model = components::input_detail::InputModel;
using Presentation = neo::context_menu_detail::EditorMenuPresentation;

core::dsl::Element* findSuffix(core::dsl::Element& element, const std::string& suffix) {
    if (element.id.size() >= suffix.size() &&
        element.id.compare(element.id.size() - suffix.size(), suffix.size(), suffix) == 0)
        return &element;
    for (const std::unique_ptr<core::dsl::Element>& child : element.children)
        if (auto* found = findSuffix(*child, suffix)) return found;
    return nullptr;
}

core::dsl::Element* findSuffix(core::dsl::Ui& ui, const std::string& suffix) {
    for (const std::unique_ptr<core::dsl::Element>& root : ui.roots())
        if (auto* found = findSuffix(*root, suffix)) return found;
    return nullptr;
}

void compose(core::dsl::Ui& ui, neo::AppState& state) {
    ui.begin("editor-context-menu-lp-test");
    neo::editorContextMenuOverlay(ui, state, eui::Screen{800.0f, 600.0f});
    ui.end();
    ui.layout(core::dsl::Screen{800.0f, 600.0f});
}

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

}  // namespace

int main() {
    neo::AppState& state = neo::state();
    state.newDocumentLanguage = "markdown";
    state.doc.text = "## 标题 [站点](https://example.com)\n\n普通段落\n";
    state.contextMenuOpen = false;
    state.animations = true;
    core::dsl::Ui ui;
    ui.begin("editor-context-menu-lp-test");
    auto& input = ui.state<Model::InputState>(neo::editorInputId(state));
    input.text = state.doc.text;
    input.cursor = 0;
    input.selectionStart = 0;
    input.selectionEnd = 0;
    input.contextLinkByte = static_cast<int>(state.doc.text.find("站点"));
    ui.end();

    // Mimic the LP plan installed by a synthesized preedit display. A closed
    // overlay must not evict that plan by asking for the committed document.
    neo::lp::invalidatePlanCache();
    const std::string preeditText = "中" + state.doc.text;
    (void)neo::lp::cachedPlan(preeditText);
    const auto version = neo::lp::planCache().version;
    const auto full = neo::planDebugStats().full;
    for (int i = 0; i < 3; ++i) compose(ui, state);
    if (!check(neo::lp::planCache().text == preeditText &&
               neo::lp::planCache().version == version &&
               neo::planDebugStats().full == full,
               "closed editor menu replaced preedit LP cache")) return 1;

    // Open state reads all three contextual facts and publishes the matching
    // menu presentation. The link byte and heading cursor are independent.
    state.contextMenuOpen = true;
    input.selectionEnd = 1;
    compose(ui, state);
    const auto& first = ui.state<Presentation>("editor.contextmenu.presentation");
    const auto* enabledCut = findSuffix(ui, ".level.0.item.7");
    if (!check(first.onLink && first.hasSelection && first.headingLevel == 2 &&
               enabledCut && !enabledCut->disabled && enabledCut->cursor == core::CursorShape::Hand &&
               neo::lp::planCache().text == state.doc.text,
               "open editor menu did not refresh link, selection, and heading")) return 2;
    auto* formatParent = findSuffix(ui, ".level.0.item.4");
    if (!check(formatParent && formatParent->onHoverChanged,
               "open linked menu missing format submenu")) return 3;
    formatParent->onHoverChanged(true);
    compose(ui, state);
    if (!check(findSuffix(ui, ".level.1.item.0") != nullptr,
               "format submenu failed to open")) return 4;

    // Closing still builds the disabled transition tree and clears active paths,
    // while retaining the last open presentation without another LP query.
    state.contextMenuOpen = false;
    input.contextLinkByte = -1;
    input.selectionStart = input.selectionEnd = 0;
    input.cursor = static_cast<int>(state.doc.text.find("普通段落"));
    (void)neo::lp::cachedPlan(preeditText);
    const auto closingVersion = neo::lp::planCache().version;
    const auto closingFull = neo::planDebugStats().full;
    compose(ui, state);
    const auto& closing = ui.state<Presentation>("editor.contextmenu.presentation");
    auto* oldFirstItem = findSuffix(ui, ".level.0.item.0");
    auto* closedHighlight = findSuffix(ui, ".level.0.highlight");
    if (!check(closing.onLink && closing.hasSelection && closing.headingLevel == 2 &&
               oldFirstItem && oldFirstItem->disabled &&
               closedHighlight && closedHighlight->opacity == 0.0f &&
               neo::lp::planCache().text == preeditText &&
               neo::lp::planCache().version == closingVersion &&
               neo::planDebugStats().full == closingFull,
               "closing menu lost its presentation or replaced preedit LP cache")) return 5;

    state.contextMenuOpen = true;
    compose(ui, state);
    const auto& reopened = ui.state<Presentation>("editor.contextmenu.presentation");
    const auto* disabledCut = findSuffix(ui, ".level.0.item.5");
    if (!check(!reopened.onLink && !reopened.hasSelection && reopened.headingLevel == 0,
               "reopened menu did not refresh link, selection, or heading")) return 6;
    // ContextMenuBuilder disables the *whole level* while closed. An open but
    // unavailable item stays in that level and uses Arrow plus a guarded click.
    if (!check(disabledCut && !disabledCut->disabled &&
               disabledCut->cursor == core::CursorShape::Arrow,
               "reopened menu cut row has wrong availability or visibility")) return 7;
    auto* reopenedHighlight = findSuffix(ui, ".level.0.highlight");
    if (!check(reopenedHighlight && reopenedHighlight->opacity == 0.0f &&
               findSuffix(ui, ".level.1.item.0") == nullptr,
               "reopened menu retained a stale active cascade")) return 8;
    // Non-Markdown modes never consult the LP parser, even when their bytes
    // happen to look like headings/links or a stale link hit remains installed.
    for (const char* path : {"note.txt", "source.cpp", "settings.json", "unknown.xyz"}) {
        state.path = path;
        state.languageOverride.clear();
        input.contextLinkByte = static_cast<int>(state.doc.text.find("站点"));
        input.selectionStart = 0; input.selectionEnd = 1;
        (void)neo::lp::cachedPlan(preeditText);
        const auto plainVersion = neo::lp::planCache().version;
        compose(ui, state);
        const auto& plain = ui.state<Presentation>("editor.contextmenu.presentation");
        auto items = neo::context_menu_detail::editorContextMenuItems(false, true, true, 2);
        if (!check(!plain.markdown && !plain.onLink && plain.headingLevel == 0 &&
                   plain.hasSelection && items.size() == 4 && !items[0].separatorBefore &&
                   std::all_of(items.begin(), items.end(), [](const auto& item) { return item.children.empty(); }) &&
                   neo::lp::planCache().version == plainVersion && neo::lp::planCache().text == preeditText &&
                   findSuffix(ui, ".level.0.item.3") && !findSuffix(ui, ".level.0.item.4"),
                   "plain/source context must contain only clipboard actions without LP work")) return 9;
        const neo::EditorCommand expected[]{neo::EditorCommand::Cut, neo::EditorCommand::Copy,
            neo::EditorCommand::Paste, neo::EditorCommand::SelectAll};
        for (int index=0; index<4; ++index) {
            state.pendingEditorCommand = neo::EditorCommand::None;
            neo::context_menu_detail::dispatchEditorContextItem(state, {index}, false, false, ui);
            if (!check(state.pendingEditorCommand == expected[index], "compact menu dispatched the wrong action")) return 10;
        }
        for (const std::vector<int>& invalid : {std::vector<int>{-1}, {4}, {0,0}, {2,4}}) {
            state.pendingEditorCommand = neo::EditorCommand::None;
            neo::context_menu_detail::dispatchEditorContextItem(state, invalid, false, false, ui);
            if (!check(state.pendingEditorCommand == neo::EditorCommand::None && !state.pendingImageImport,
                       "compact menu accepted an invalid/Markdown path")) return 11;
        }
    }
    state.languageOverride = "markdown";
    compose(ui, state);
    if (!check(ui.state<Presentation>("editor.contextmenu.presentation").markdown &&
               findSuffix(ui, ".level.0.item.6"), "explicit Markdown override must restore rich menu")) return 12;
    state.languageOverride = "text";
    state.pendingEditorCommand = neo::EditorCommand::None;
    neo::context_menu_detail::dispatchEditorContextItem(state, {2,0}, true, false, ui);
    if (!check(state.pendingEditorCommand == neo::EditorCommand::None,
               "stale Markdown menu cannot format a newly plain document")) return 13;
    return 0;
}
