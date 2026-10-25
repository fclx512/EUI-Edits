#pragma once

#include "state/app_state.h"
#include "ui/metrics.h"
#include "ui/window_layout.h"
#include "ui/widgets.h"
#include "components/contextmenu.h"
#include "ui/popup_presentation.h"
#include "platform/window_chrome.h"

namespace neo {

inline bool markdownImmersive(const AppState& state) {
    return state.writingCapable() && state.markdownImmersive;
}
inline float markdownToolbarHeight(const AppState& state, float width) {
    if (state.compactWindow && !markdownImmersive(state)) return 0;
    if (!state.writingCapable()) return 0;
    if (!state.markdownCapable() && !markdownImmersive(state)) return 0;
    return state.markdownCapable() && !markdownImmersive(state) && width < 480 ? 80.0f : 44.0f;
}
inline float writingCapsuleRailWidth(const AppState& state, float editorWidth, const UiMetrics& metrics) {
    if (state.compactWindow) return 0;
    if (!state.writingCapable() || state.markdownCapable() || markdownImmersive(state)) return 0.0f;
    // The capsule begins at editorWidth - 87. Reuse the readable column's
    // existing right margin before shrinking the input. A source-byte count
    // bounds the line-number digits without scanning the document each frame.
    const float capsuleLeft = std::max(12.0f, editorWidth - 84.0f) - 3.0f;
    const float inset = metrics.editorInset;
    float gutter = 0.0f;
    if (state.showLineNumbers) {
        const int digits = std::max(2, static_cast<int>(std::to_string(state.doc.text.size() + 1).size()));
        gutter = core::TextPrimitive::measureTextWidth(std::string(digits, '8'),
            state.sourceView() ? codeFontFamily(state) : editorFontFamily(state), metrics.editorFontSize) + 8.0f;
    }
    const float maxTextWidth = state.readableWidth && !state.sourceView() && state.wordWrap()
        ? metrics.editorFontSize * 43.75f : 0.0f;
    const auto textRight = [&](float rail) {
        const float width = std::max(0.0f, editorWidth - rail);
        const float available = std::max(0.0f, width - 2.0f * inset - gutter);
        const float extra = maxTextWidth > 0.0f && available > maxTextWidth
            ? (available - maxTextWidth) * 0.5f : 0.0f;
        // Omitting the scrollbar gutter estimates a right edge no farther left
        // than the real one, so this remains safe when the scrollbar is on.
        return width - inset - extra;
    };
    if (textRight(0.0f) <= capsuleLeft - 1.0f) return 0.0f;
    float low = 0.0f, high = 88.0f;
    for (int i = 0; i < 10; ++i) {
        const float middle = (low + high) * 0.5f;
        if (textRight(middle) <= capsuleLeft - 1.0f) high = middle;
        else low = middle;
    }
    return high;
}
inline float markdownOutlineWidth(const AppState& state, float width) {
    return state.markdownCapable() && state.markdownOutlineOpen && !state.compactWindow && !markdownImmersive(state)
        && width >= 820 ? std::clamp(width * .24f, 220.0f, 280.0f) : 0.0f;
}

inline void writingIconButton(eui::Ui& ui, const std::string& id, UiIcon icon,
                              const std::string& label, float x, float y, bool active,
                              bool enabled, std::function<void()> action,
                              float buttonWidth = 32, float buttonHeight = 32, bool danger = false) {
    const auto& c = editorColors();
    ui.stack(id).position(x, y).size(buttonWidth, buttonHeight).content([&] {
        ui.rect(id + ".bg").fill().radius(danger || id.rfind("window.", 0) == 0 ? 0 : neo::corner(c, 6.0f))
            .states(active ? core::mixColor(c.editor, c.accent, .12f) : transparentColor(), danger ? rgba(.91f,.07f,.14f,1) : c.rowHover,
                    danger ? rgba(.72f,.04f,.10f,1) : c.pressed)
            .preserveFocusOnPress().disabled(!enabled).cursor(eui::CursorShape::Hand)
            .onClick(std::move(action)).onHover([&ui, id](bool hover) {
                ui.state<bool>(id + ".hover") = hover; app::requestUpdate();
            }).build();
        iconView(ui, id + ".icon", icon, (buttonWidth-18)*.5f, (buttonHeight-18)*.5f, 18,
                 danger && ui.state<bool>(id + ".hover") ? rgba(1,1,1,1) : active ? c.accent : c.textMuted);
        if (ui.state<bool>(id + ".hover")) {
            const float w = std::max(60.0f, core::TextPrimitive::measureTextWidth(label, kUiFontFamily, 12) + 20);
            ui.stack(id + ".tip").position(x < 100 && id.rfind("window.",0)!=0 ? 0 : buttonWidth-w, buttonHeight+4).size(w, 28).zIndex(2800).hitTestMode(core::dsl::HitTestMode::None).content([&] {
                ui.rect(id + ".tip.bg").fill().radius(neo::corner(c, 5.0f)).color(c.panel).border(1, c.border).build();
                ui.text(id + ".tip.text").fill().text(label).fontSize(12).color(c.text)
                    .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
            }).build();
        }
    }).build();
}

// Keep selection/focus in the editor; commands use the existing compose-time
// edit funnel, including one undo record and document/recovery synchronization.
inline void queueMarkdownTool(AppState& state, EditorCommand command, int kind) {
    if (!state.markdownCapable() || state.unavailable) return;
    state.markdownHeadingMenuOpen = false;
    state.pendingEditorCommand = command;
    if (command == EditorCommand::FormatInline) state.pendingFormatKind = kind;
    if (command == EditorCommand::SetHeading) state.pendingHeadingLevel = kind;
    if (command == EditorCommand::InsertBlock) state.pendingBlockKind = kind;
    state.findEditorFocusPending = true;
    app::requestUpdate();
}

inline void markdownToolbarView(eui::Ui& ui, AppState& state, float width) {
    if (state.compactWindow && !markdownImmersive(state)) return;
    if (!state.writingCapable()) return;
    const bool markdown = state.markdownCapable();
    const bool immersive = markdownImmersive(state);
    const bool floatingCapsule = !markdown && !immersive;
    const float height = floatingCapsule ? 44.0f : markdownToolbarHeight(state, width);
    const auto& colors = editorColors(state.theme);
    auto toolbar = ui.stack("markdown.toolbar");
    toolbar.size(width, height).zIndex(50);
    if (floatingCapsule) toolbar.position(0, 0).ignoreLayout();
    toolbar.content([&] {
        if (!floatingCapsule) ui.rect("markdown.toolbar.bg").fill()
            .color(colors.editor).build();
        if (markdown) ui.rect("markdown.toolbar.line").position(12, height - 1).size(std::max(0.0f,width-24), 1).color(colors.border).build();
        if (markdown && !immersive) {
            writingIconButton(ui, "markdown.heading", UiIcon::Heading, i18n::tr("writing.heading"), 12, 6,
                state.markdownHeadingMenuOpen, !state.unavailable, [&state] {
                    if (state.markdownHeadingMenuOpen) state.markdownHeadingMenuOpen = false;
                    else popup_presentation::activate(state, popup_presentation::Owner::MarkdownHeading);
                    state.openMenu = MenuKind::None; app::requestUpdate();
                });
            float x = 48;
            const auto format = [&](const char* id, UiIcon icon, const char* label, EditorCommand command, int kind) {
                writingIconButton(ui, id, icon, label, x, 6, false, !state.unavailable,
                    [&state, command, kind] { queueMarkdownTool(state, command, kind); });
                x += 36;
            };
            format("markdown.bold", UiIcon::Bold, i18n::tr("writing.bold"), EditorCommand::FormatInline, 1);
            format("markdown.italic", UiIcon::Italic, i18n::tr("writing.italic"), EditorCommand::FormatInline, 2);
            format("markdown.link", UiIcon::Link, i18n::tr("writing.link"), EditorCommand::InsertBlock, 6);
            writingIconButton(ui,"markdown.image",UiIcon::Image,i18n::tr("writing.image"),x,6,false,
                !state.unavailable,[&state]{ state.pendingImageImport=true; app::requestUpdate(); });
            x += 36;
            format("markdown.task", UiIcon::Task, i18n::tr("writing.task"), EditorCommand::InsertBlock, 7);
            format("markdown.table", UiIcon::Table, i18n::tr("writing.table"), EditorCommand::InsertBlock, 2);
            format("markdown.code", UiIcon::Code, i18n::tr("writing.code"), EditorCommand::InsertBlock, 1);
        }
        const float x = std::max(12.0f, width - (immersive ? 240 : markdown ? 120 : 84));
        if (immersive) {
            // Only the actual controls consume client input. The empty space
            // between these buttons and the window controls remains draggable.
            platform::chrome::exclude(x, height - 38, 32, 32);
            platform::chrome::exclude(x + 36, height - 38, 32, 32);
        }
        if (!markdown) ui.rect("writing.capsule").position(x-3,height-41).size(74,38)
            .radius(neo::corner(colors, 9.0f)).color(colors.panel).border(1,colors.border).build();
        writingIconButton(ui, "markdown.typewriter", UiIcon::Typewriter, i18n::tr("writing.typewriter"), x, height-38,
            state.markdownTypewriter, true, [&state] {
                state.markdownTypewriter = !state.markdownTypewriter;
                state.findEditorFocusPending = true; app::requestUpdate();
            });
        writingIconButton(ui, "markdown.immersive", UiIcon::Focus,
            immersive ? i18n::tr("writing.exit_immersive") : i18n::tr("writing.immersive"), x+36, height-38,
            immersive, true, [&state] {
                state.markdownImmersive = !state.markdownImmersive;
                state.markdownHeadingMenuOpen = false; state.openMenu = MenuKind::None;
                state.tabListOpen = false; state.languageMenuOpen = false;
                state.contextMenuOpen = false; state.vaultContextMenuOpen = false;
                state.findEditorFocusPending = true; app::requestUpdate();
            });
        if (markdown && !immersive) writingIconButton(ui, "markdown.outline", UiIcon::Outline, i18n::tr("outline.title"),
            x+72, height-38, state.markdownOutlineOpen, true, [&state] {
                state.markdownOutlineOpen = !state.markdownOutlineOpen; app::requestUpdate();
            });
    }).build();
}

inline void markdownHeadingMenu(eui::Ui& ui, AppState& state, const eui::Screen& screen, float x, float y) {
    if (!state.markdownCapable() || markdownImmersive(state)) {
        state.markdownHeadingMenuOpen = false;
        return;
    }
    const auto menuTokens = editorColors(state.theme).tokens;
    auto menu = components::contextMenu(ui, "markdown.heading.menu").open(state.markdownHeadingMenuOpen)
        .screen(screen.width, screen.height).position(x + 12, y + 44).size(180, 32)
        .items({components::ContextMenuItem(i18n::tr("context.body")),
                components::ContextMenuItem(i18n::tr("context.heading1")),
                components::ContextMenuItem(i18n::tr("context.heading2")),
                components::ContextMenuItem(i18n::tr("context.heading3"))})
        .theme(menuTokens);
    popup_presentation::apply(menu, state,
                          menuTokens, state.markdownHeadingMenuOpen);
    menu.skipClosedContent().zIndex(900)
        .onSelectPath([&state](const std::vector<int>& path) {
            if (path.size() == 1) queueMarkdownTool(state, EditorCommand::SetHeading, path[0]);
        })
        .onOpenChange([&state](bool open) { state.markdownHeadingMenuOpen = open; }).build();
}

} // namespace neo
