#pragma once

// Windows RPC headers define `small` as `char`. Keep that macro local to its
// owner without allowing it to rewrite the spacing token used below.
#ifdef small
#pragma push_macro("small")
#undef small
#define NEO_POPUP_PRESENTATION_RESTORE_SMALL
#endif

#include "components/contextmenu.h"
#include "state/app_state.h"
#include "ui/widgets.h"

namespace neo::popup_presentation {

enum class Owner {
    None,
    MainMenu,
    EditorContext,
    VaultContext,
    FindOptions,
    MarkdownHeading,
    Language,
};

// Explicit root z-index wins; declaration order breaks equal values. Keep the
// existing event-time dismissal behavior independently of panel presentation.
inline Owner activeOwner(const AppState& state) {
    if (state.languageMenuOpen) return Owner::Language;
    if (state.vaultContextMenuOpen) return Owner::VaultContext;
    if (state.contextMenuOpen) return Owner::EditorContext;
    if (state.findOptionsOpen) return Owner::FindOptions;
    if (state.openMenu != MenuKind::None) return Owner::MainMenu;
    if (state.markdownHeadingMenuOpen) return Owner::MarkdownHeading;
    return Owner::None;
}

// Event-time transition: opening one popup dismisses the others. MainMenu's
// caller supplies its File/Edit/View kind immediately after this operation.
inline void activate(AppState& state, Owner owner) {
    if (owner != Owner::MainMenu) state.openMenu = MenuKind::None;
    state.contextMenuOpen = owner == Owner::EditorContext;
    state.vaultContextMenuOpen = owner == Owner::VaultContext;
    state.findOptionsOpen = owner == Owner::FindOptions;
    state.markdownHeadingMenuOpen = owner == Owner::MarkdownHeading;
    state.languageMenuOpen = owner == Owner::Language;
    state.tabListOpen = false;
}

// A full-page navigation must finish transient popup ownership at event time.
// Hidden editor menus are not composed, so their outside-click callbacks cannot
// close them while settings owns the page.
inline void setSettingsOpen(AppState& state, bool open) {
    activate(state, Owner::None);
    if (state.settingsOpen && !open) state.findEditorFocusPending = true;
    state.settingsOpen = open;
    state.fontPicker = FontPickerTarget::None;
    state.themeListOpen = false;
}

inline core::Color opaque(core::Color color) {
    color.a = 1.0f;
    return color;
}

inline components::ContextMenuStyle style(
    UiStyle uiStyle, const components::theme::ThemeColorTokens& tokens) {
    components::ContextMenuStyle style(tokens);
    style.background = opaque(style.background);
    // UiStyle is explicit: the token snapshot can belong to the previous style
    // during a settings change. Only presentation changes, never row geometry.
    const components::theme::RadiusTokens rounded;
    if (uiStyle == UiStyle::Square) {
        style.radius = 0.0f;
        style.highlightInset = tokens.metrics.spacing.micro;
        style.highlightRadius = 0.0f;
        style.border = opaque(tokens.border);
        style.shadow.blur = 12.0f;
        style.shadow.offset = {0.0f, 2.0f};
    } else {
        style.radius = rounded.card;
        style.highlightInset = tokens.metrics.spacing.small;
        style.highlightRadius = rounded.card - rounded.tiny;
    }
    return style;
}

inline components::ContextMenuMotion motion(UiStyle uiStyle, bool animations, bool allowExit = true) {
    components::ContextMenuMotion result;
    const bool square = uiStyle == UiStyle::Square;
    result.offsetY = square ? -8.0f : 4.0f;
    result.clipContent = square && animations;
    result.directionalSubmenus = square && animations;
    if (animations) {
        result.enter = core::Transition::make(0.12f, core::Ease::OutCubic);
        if (allowExit) result.exit = core::Transition::make(0.07f, core::Ease::OutQuad);
        if (!square) result.highlight = core::Transition::make(0.075f, core::Ease::OutCubic);
    }
    return result;
}

inline void apply(components::ContextMenuBuilder& menu,
                  const AppState& state,
                  const components::theme::ThemeColorTokens& tokens,
                  bool requestedOpen) {
    // A replacement popup hides the previous one immediately. Only a dismissal
    // to the document gets the short exit, so two sets of labels never overlap.
    const bool allowExit = requestedOpen || activeOwner(state) == Owner::None;
    menu.style(style(state.uiStyle, tokens))
        .motion(motion(state.uiStyle, animationsEnabled(state), allowExit));
}

} // namespace neo::popup_presentation

#ifdef NEO_POPUP_PRESENTATION_RESTORE_SMALL
#pragma pop_macro("small")
#undef NEO_POPUP_PRESENTATION_RESTORE_SMALL
#endif
