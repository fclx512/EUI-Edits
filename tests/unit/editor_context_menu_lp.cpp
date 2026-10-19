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

    // Closing still builds the disabled transition tree and clears cascades,
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
    if (!check(closing.onLink && closing.hasSelection && closing.headingLevel == 2 &&
               oldFirstItem && oldFirstItem->disabled &&
               findSuffix(ui, ".level.0.highlight") == nullptr &&
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
    if (!check(findSuffix(ui, ".level.0.highlight") == nullptr &&
               findSuffix(ui, ".level.1.item.0") == nullptr,
               "reopened menu retained a stale active cascade")) return 8;
    return 0;
}
