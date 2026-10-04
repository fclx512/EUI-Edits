#pragma once

#include "components/button.h"
#include "components/dialog.h"
#include "components/input.h"
#include "components/scroll.h"
#include "core/render/text.h"
#include "state/app_actions.h"
#include "state/app_state.h"
#include "ui/metrics.h"
#include "ui/widgets.h"

#include <algorithm>

namespace neo {

// The stock dialog supplies the modal surface; custom content adds a third
// explicit action without interpreting backdrop/Escape as destructive consent.
inline void safetyDialog(eui::Ui& ui, AppState& state, const eui::Screen& screen,
                         bool conflict) {
    const bool open = conflict ? state.saveConflictOpen
        : state.pending != PendingAction::None && !state.saveConflictOpen;
    if (!open) { state.safetyDialogWasOpen = false; return; }
    if (!state.safetyDialogWasOpen) { state.safetyChoice = 2; state.safetyScroll = 0; }
    state.safetyDialogWasOpen = true;
    const auto& colors = editorColors();
    const std::string id = conflict ? "editor.saveConflict" : "editor.confirm";
    const float width = std::min(520.0f, std::max(220.0f, screen.width - 48.0f));
    const auto message = conflict
        ? i18n::format("safety.conflict_body", {{"file", textfile::fileName(state.conflictPath)}})
        : state.displayName() + "\n\n" + i18n::tr("safety.unsaved_body");
    core::TextStyle titleStyle, messageStyle;
    titleStyle.text = i18n::tr(conflict ? "safety.conflict_title" : "safety.unsaved_title");
    titleStyle.fontFamily = uiFontFamily(state);
    titleStyle.fontSize = uiMetrics(state).panelFontSize + 3;
    titleStyle.lineHeight = titleStyle.fontSize + 5;
    titleStyle.maxWidth = width - 60; titleStyle.wrap = true;
    messageStyle.text = message; messageStyle.fontFamily = titleStyle.fontFamily;
    messageStyle.fontSize = uiMetrics(state).panelFontSize;
    messageStyle.lineHeight = messageStyle.fontSize + 6;
    messageStyle.maxWidth = titleStyle.maxWidth; messageStyle.wrap = true;
    // Text measurement returns line-box height, while fallback glyph ink can
    // exceed that box slightly after raster-size rounding. Keep a font-relative
    // inset around the ink when centering it inside the clipped viewport.
    const float titleInkSafety = std::max(2.0f, titleStyle.fontSize * 0.25f);
    const float messageInkSafety = std::max(2.0f, messageStyle.fontSize * 0.25f);
    const float titleHeight = core::TextPrimitive::measureTextSize(titleStyle).y + titleInkSafety;
    const float messageHeight = core::TextPrimitive::measureTextSize(messageStyle).y + messageInkSafety;
    const float contentHeight = titleHeight + 16 + messageHeight;
    const float height = std::min(std::clamp(contentHeight + 84, 160.0f, 320.0f),
                                  std::max(140.0f, screen.height - 48.0f));
    const auto cancel = [&state, conflict] {
        if (conflict) resolveSaveConflict(state, 2);
        else resolveDocumentConfirmation(state, filesafety::UnsavedChoice::Cancel);
        app::requestUpdate();
    };
    ui.rect(id + ".keySink").size(0, 0).focusable()
        .onKeyEvent([&state, conflict, cancel](const eui::KeyEvent& event) {
            if (!event.isDown()) return true;
            if (event.key == eui::InputKey::Escape && event.action == core::KeyAction::Press) cancel();
            else if (event.key == eui::InputKey::Tab || event.key == eui::InputKey::Right ||
                     event.key == eui::InputKey::Left) {
                const bool previous = event.key == eui::InputKey::Left || event.modifiers.shift;
                state.safetyChoice = (state.safetyChoice + (previous ? 2 : 1)) % 3;
                app::requestUpdate();
            } else if ((event.key == eui::InputKey::Enter || event.key == eui::InputKey::Space) &&
                       event.action == core::KeyAction::Press) {
                if (conflict) resolveSaveConflict(state, state.safetyChoice);
                else resolveDocumentConfirmation(state,
                    state.safetyChoice == 0 ? filesafety::UnsavedChoice::Save :
                    state.safetyChoice == 1 ? filesafety::UnsavedChoice::Discard : filesafety::UnsavedChoice::Cancel);
                app::requestUpdate();
            }
            return true;
        }).build();
    ui.requestFocus(id + ".keySink");
    components::dialog(ui, id).open(true).theme(colors.tokens)
        .screen(screen.width, screen.height).size(width, height).zIndex(3500)
        .content([&] {
            const float viewport = height - 84;
            state.safetyScroll = std::clamp(state.safetyScroll, 0.0f, std::max(0.0f, contentHeight - viewport));
            const auto scrollId = id + ".bodyScroll";
            ui.stack(scrollId).position(24, 20).size(width - 48, viewport).clip()
                .scrollState(scrollId, state.safetyScroll, std::max(0.0f, contentHeight - viewport), 32)
                .onScrollOffsetChanged([&state](float value) { state.safetyScroll = value; })
                .content([&] {
                    ui.stack(id + ".body").size(width - 60, contentHeight).scrollContentFrom(scrollId)
                        .content([&] {
                            // The renderer places glyph bitmaps relative to the face ascent;
                            // CJK fallback glyph ink can extend above the line-box origin.
                            // Center the ink bounds in the measured line-box area so the
                            // clipped scroll viewport cannot shave the title's top edge.
                            ui.text(id + ".title")
                                .position(0, 0).size(width - 60, titleHeight)
                                .text(titleStyle.text).fontFamily(titleStyle.fontFamily).fontSize(titleStyle.fontSize)
                                .lineHeight(titleStyle.lineHeight)
                                .maxWidth(width - 60).wrap(true).color(colors.text)
                                .verticalAlign(eui::VerticalAlign::Center).build();
                            ui.text(id + ".message")
                                .position(0, titleHeight + 16)
                                .size(width - 60, messageHeight)
                                .text(message).fontFamily(messageStyle.fontFamily).fontSize(messageStyle.fontSize)
                                .lineHeight(messageStyle.lineHeight).maxWidth(width - 60)
                                .wrap(true).color(colors.text)
                                .verticalAlign(eui::VerticalAlign::Center).build();
                        }).build();
                }).build();
            if (contentHeight > viewport) components::scroll(ui, id + ".scrollbar")
                .theme(colors.tokens).position(width - 32, 20).size(6, viewport)
                .scrollStateId(scrollId).offset(state.safetyScroll).viewport(viewport).content(contentHeight).build();
            const float buttonWidth = (width - 64) / 3;
            const float buttonFont = std::min(uiMetrics(state).panelFontSize,
                buttonWidth < 85 ? 12.0f : uiMetrics(state).panelFontSize);
            const char* labels[] = {
                conflict ? "safety.overwrite" : "safety.save",
                conflict ? "safety.save_as" : "safety.discard",
                "safety.cancel"};
            for (int i = 0; i < 3; ++i) {
                components::button(ui, id + ".action" + std::to_string(i))
                    .theme(colors.tokens, !conflict && i == 0)
                    .position(24 + i * (buttonWidth + 8), height - 48)
                    .size(buttonWidth, 32).text(i18n::tr(labels[i]))
                    .fontSize(buttonFont).radius(6)
                    .border(1.5f, state.safetyChoice == i ? colors.accent : colors.tokens.border)
                    .onClick([&state, conflict, i] {
                        if (conflict) resolveSaveConflict(state, i);
                        else resolveDocumentConfirmation(state,
                            i == 0 ? filesafety::UnsavedChoice::Save :
                            i == 1 ? filesafety::UnsavedChoice::Discard : filesafety::UnsavedChoice::Cancel);
                        app::requestUpdate();
                    }).build();
            }
        }).onOpenChange([cancel](bool value) { if (!value) cancel(); }).build();
}
inline void confirmOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    safetyDialog(ui, state, screen, state.saveConflictOpen);
}

inline void riskOverlay(eui::Ui& ui,AppState& state,const eui::Screen& screen) {
    if(state.riskAction!=AppState::RiskAction::None) {
        ui.rect("settings.riskConfirm.keySink").size(0,0).focusable().onKeyEvent([&state](const eui::KeyEvent& e) {
            if(e.isDown() && e.key==eui::InputKey::Escape) {cancelRisk(state);app::requestUpdate();}
            return true;
        }).build();
        ui.requestFocus("settings.riskConfirm.keySink");
    }
    const auto& colors=editorColors();components::DialogStyle style(colors.tokens);
    style.primary=colors.text.r>.5f?rgba(.80f,.20f,.23f,1):rgba(.76f,.13f,.17f,1);
    style.primaryHover=rgba(.9f,.25f,.28f,1);style.primaryPressed=rgba(.66f,.1f,.13f,1);
    components::dialog(ui,"settings.riskConfirm").open(state.riskAction!=AppState::RiskAction::None)
        .theme(colors.tokens).style(style).screen(screen.width,screen.height).zIndex(3000)
        .title(state.riskTitle).message(state.riskMessage).primaryText(state.riskConfirm).primaryTextColor(rgba(1,1,1,1)).secondaryText(i18n::tr("safety.cancel"))
        .onPrimary([&state]{confirmRisk(state);app::requestUpdate();})
        .onSecondary([&state]{cancelRisk(state);app::requestUpdate();})
        .onOpenChange([&state](bool open){if(!open) cancelRisk(state);}).build();
}
inline const std::vector<std::vector<std::string>>& languageGroups() {
    static const std::vector<std::vector<std::string>> groups={{"json","jsonc","yaml","toml","ini","xml"},
        {"python","javascript","typescript","ruby","lua","shell","powershell","batch"},
        {"c","cpp","cs","java","rust","go"},
        {"html","css","sql","cmake","makefile","dockerfile"}};return groups;
}
inline void languageOverlay(eui::Ui& ui,AppState& state,const eui::Screen& screen) {
    std::vector<components::ContextMenuItem> items={components::ContextMenuItem(i18n::tr("safety.language_auto")).withChecked(state.languageOverride.empty()),
        components::ContextMenuItem(i18n::tr("safety.plain_text")).withChecked(state.language()=="text"),components::ContextMenuItem("Markdown").withChecked(state.language()=="markdown")};
    const char* labels[]={i18n::tr("safety.group_data"),i18n::tr("safety.group_scripts"),i18n::tr("safety.group_source"),i18n::tr("safety.group_other")};
    for(std::size_t i=0;i<languageGroups().size();++i) {
        std::vector<components::ContextMenuItem> children;
        for(const auto& lang:languageGroups()[i]) children.emplace_back(components::ContextMenuItem(filetypes::label(lang)).withChecked(state.language()==lang));
        items.emplace_back(labels[i],std::move(children));
    }
    components::contextMenu(ui,"editor.languageMenu").open(state.languageMenuOpen).screen(screen.width,screen.height)
        .position(std::max(0.0f,screen.width-270),std::max(0.0f,screen.height-275)).size(220,32)
        .theme(editorColors().tokens).zIndex(2500).items(std::move(items))
        .onSelectPath([&state](const std::vector<int>& path) {
            if(path.empty()) return;
            if(path[0]==0) state.languageOverride.clear();
            else if(path[0]==1) state.languageOverride="text";
            else if(path[0]==2) state.languageOverride="markdown";
            else if(path.size()==2 && path[0]>=3 && path[0]<7) {
                const auto& group=languageGroups()[path[0]-3];if(path[1]>=0 && path[1]<static_cast<int>(group.size())) state.languageOverride=group[path[1]];
            }
            state.languageMenuOpen=false;state.wrapOverride=-1;source::invalidate();lp::invalidatePlanCache();app::requestUpdate();
        }).onOpenChange([&state](bool open){state.languageMenuOpen=open;}).build();
}

// "编辑链接"弹窗（2026-09-26，右键菜单 → 编辑链接）：只改 URL 部分，链接文字
// 原样保留。确认后排队 EditorCommand::EditLink，compose 里经 applyEditLink 落地
// （撤销历史按一次编辑记录）。URL 区间在打开弹窗时已记进 pendingLinkBeg/End。
inline void linkEditorOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    constexpr float kPanelWidth = 460.0f;
    constexpr float kPanelHeight = 176.0f;
    constexpr float kPad = 24.0f;
    constexpr float kInner = kPanelWidth - kPad * 2.0f;
    const EditorColors& colors = editorColors();
    const components::theme::ThemeColorTokens tokens = colors.tokens;

    components::dialog(ui, "editor.linkedit")
        .open(state.linkEditorOpen)
        .theme(colors.tokens)
        .screen(screen.width, screen.height)
        .size(kPanelWidth, kPanelHeight)
        .zIndex(1200)
        .transition(quickTransition())
        .content([&] {
            ui.text("editor.linkedit.title")
                .position(kPad, 18.0f)
                .size(kInner, 24.0f)
                .text(i18n::tr("safety.edit_link"))
                .fontSize(15.0f)
                .color(colors.text)
                .build();

            components::InputStyle inputStyle(tokens);
            inputStyle.background = colors.editor;
            inputStyle.focused = colors.editor;
            inputStyle.border = tokens.border;
            inputStyle.focusBorder = colors.accent;
            inputStyle.text = colors.text;
            inputStyle.cursor = colors.accent;
            inputStyle.radius = 6.0f;
            inputStyle.shadow = eui::Shadow{};
            components::input(ui, "editor.linkedit.input")
                .position(kPad, 54.0f)
                .size(kInner, 32.0f)
                .value(state.linkEditorUrl)
                .placeholder(i18n::tr("safety.link_target"))
                .fontSize(13.0f)
                .inset(10.0f)
                .style(inputStyle)
                .transition(quickTransition())
                .onChange([&state](const std::string& value) { state.linkEditorUrl = value; })
                .build();

            const float buttonY = kPanelHeight - 54.0f;
            components::button(ui, "editor.linkedit.secondary")
                .theme(tokens, false)
                .position(kPanelWidth - kPad - 104.0f * 2.0f - 10.0f, buttonY)
                .size(104.0f, 32.0f)
                .text(i18n::tr("safety.cancel"))
                .fontSize(13.0f)
                .radius(6.0f)
                .border(1.0f, tokens.border)
                .shadow(0.0f, 0.0f, 0.0f, components::theme::color(0.0f, 0.0f, 0.0f, 0.0f))
                .onClick([&state] {
                    state.linkEditorOpen = false;
                    state.pendingLinkBeg = -1;
                    state.pendingLinkEnd = -1;
                    app::requestUpdate();
                })
                .build();
            components::button(ui, "editor.linkedit.primary")
                .theme(tokens, true)
                .position(kPanelWidth - kPad - 104.0f, buttonY)
                .size(104.0f, 32.0f)
                .text(i18n::tr("safety.ok"))
                .fontSize(13.0f)
                .radius(6.0f)
                .border(1.0f, components::theme::withAlpha(tokens.primary, 0.64f))
                .shadow(10.0f, 0.0f, 3.0f, components::theme::withAlpha(tokens.primary, 0.18f))
                .onClick([&state] {
                    // 弹窗里的新 URL 在确认这一刻才装进命令载荷（URL 区间在菜单里
                    // 点"编辑链接"时已记下；弹窗期间文档没被改过，区间仍然有效）。
                    state.pendingLinkUrl = state.linkEditorUrl;
                    state.pendingEditorCommand = EditorCommand::EditLink;
                    state.linkEditorOpen = false;
                    app::requestUpdate();
                })
                .build();
        })
        .onOpenChange([&state](bool open) {
            if (!open) {
                state.linkEditorOpen = false;
            }
        })
        .build();
}

inline void toastOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    components::toast(ui, "editor.toast")
        .visible(state.toastVisible)
        .theme(editorColors().tokens)
        .screen(screen.width, screen.height)
        .title(state.toastTitle)
        .message(state.toastMessage)
        .fontFamily(uiFontFamily(state))
        .fontSize(uiMetrics(state).panelFontSize)
        .titleFontSize(uiMetrics(state).panelFontSize+3)
        .iconRenderer([](eui::Ui& iconUi, const std::string& id, float x, float y, float size, eui::Color color) {
            iconView(iconUi, id, UiIcon::Info, x, y, size, color);
        })
        .transition(quickTransition())
        .duration(std::clamp(2.6f + state.toastMessage.size() * .035f, 2.6f, 12.0f))
        .onAutoDismiss([&state] { state.toastVisible = false; })
        .onDismiss([&state] { state.toastVisible = false; })
        .build();
}

} // namespace neo
