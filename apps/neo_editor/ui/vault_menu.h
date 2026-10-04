#pragma once

#include "model/i18n.h"

#include "components/button.h"
#include "components/contextmenu.h"
#include "components/dialog.h"
#include "components/input.h"
#include "components/scroll.h"
#include "core/render/text.h"
#include "platform/vault_rename.h"
#include "state/app_actions.h"
#include "state/app_state.h"
#include "ui/metrics.h"
#include "ui/widgets.h"

#include <string>
#include <vector>

// 文档库右键菜单（2026-09-26）：条目上右键 → 打开 / 新建文件 / 新建文件夹 /
// 重命名 / 删除。菜单复用 components::contextMenu（与编辑区右键菜单同一套），
// 新建/重命名的输入与删除确认用 components::dialog + components::input。
// 菜单回调发生在事件阶段，文件 IO 走 app_actions 的 vault* 入口。
namespace neo {
namespace vault_menu_detail {

using MenuItem = components::ContextMenuItem;

// 菜单项与分发共用"目标是不是目录"这一份判定；目录才有"新建"（在它里面建），
// 文件的"新建"落在它所在目录。两项都提供（对齐 Obsidian 的文件树行为）。
inline std::string parentOf(const std::string& relative, bool isDir) {
    if (isDir) {
        return relative;
    }
    const std::size_t slash = relative.find_last_of('/');
    return slash == std::string::npos ? std::string{} : relative.substr(0, slash);
}

inline std::vector<MenuItem> vaultContextMenuItems(bool isDir) {
    std::vector<MenuItem> items;
    if (!isDir) {
        items.emplace_back(i18n::tr("vault.open"));
    }
    items.emplace_back(i18n::tr("vault.new_file"));
    items.emplace_back(i18n::tr("vault.new_folder"));
    items.emplace_back(i18n::tr("vault.rename"));
    items.emplace_back(MenuItem(i18n::tr("vault.delete")).withSeparatorBefore());
    return items;
}

// path[0] 的语义随 isDir 变（文件菜单多了"打开"在最前），分发集中在这里。
inline void dispatchVaultContextItem(AppState& state, int index) {
    const bool isDir = state.vaultContextIsDir;
    const std::string target = state.vaultContextPath;
    if (!isDir) {
        if (index == 0) {
            requestOpenPath(state, vaultAbsolutePath(state, target));
            return;
        }
        --index;
    }
    switch (index) {
        case 0:  // 新建文件
        case 1:  // 新建文件夹
            state.vaultPromptKind = index == 0 ? VaultPromptKind::NewFile : VaultPromptKind::NewFolder;
            state.vaultPromptText.clear();
            state.vaultPromptError.clear();
            state.vaultPromptFocusPending = true;
            state.vaultRenameReturnToRow = false;
            return;
        case 2:  // 重命名：输入框预填当前名称
            beginVaultRename(state, target, isDir, state.vaultRowFocusedPath == target);
            return;
        case 3:  // 删除（确认弹窗另开）
            state.vaultDeletePending = true;
            return;
        default:
            return;
    }
}

} // namespace vault_menu_detail

// 右键菜单本体。与其它全屏弹层一起在 compose 末尾声明（app.cpp）。
inline void vaultContextMenuOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);
    components::theme::ThemeColorTokens menuTokens = colors.tokens;
    menuTokens.metrics.typography.option = metrics.menuFontSize;

    components::contextMenu(ui, "vault.contextmenu")
        .open(state.vaultContextMenuOpen)
        .screen(screen.width, screen.height)
        .position(state.vaultContextMenuX, state.vaultContextMenuY)
        .size(metrics.menuWidth, metrics.menuItemHeight)
        .items(vault_menu_detail::vaultContextMenuItems(state.vaultContextIsDir))
        .theme(menuTokens)
        .transition(quickTransition())
        .zIndex(1100)
        .onSelectPath([&state](const std::vector<int>& path) {
            if (!path.empty()) {
                vault_menu_detail::dispatchVaultContextItem(state, path[0]);
            }
        })
        .onOutsideContextMenu([&state](float x, float y) {
            // 菜单开着时再次右键：刷新锚点位置（与编辑区右键菜单同一套）。
            state.vaultContextMenuX = x;
            state.vaultContextMenuY = y;
            app::requestUpdate();
        })
        .onOpenChange([&state](bool open) {
            if (!open) {
                state.vaultContextMenuOpen = false;
            }
        })
        .build();
}

// "新建文件 / 新建文件夹 / 重命名"的输入弹窗。确认即执行（IO 在回调里，
// app_actions 内部完成刷新/揭示/toast）。zIndex 压过右键菜单。
inline void vaultPromptOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    const VaultPromptKind kind = state.vaultPromptKind;
    if (kind == VaultPromptKind::None) {
        return;
    }
    const auto submit = [&state] {
        if (state.vaultPromptKind == VaultPromptKind::Rename) { confirmVaultRename(state); return; }
        const auto current = state.vaultPromptKind;
        const auto name = state.vaultPromptText;
        const auto parent = vault_menu_detail::parentOf(state.vaultContextPath, state.vaultContextIsDir);
        cancelVaultPrompt(state);
        if (current == VaultPromptKind::NewFile) vaultCreateFile(state, parent, name);
        else if (current == VaultPromptKind::NewFolder) vaultCreateFolder(state, parent, name);
    };
    if (state.vaultPromptFocusPending) {
        using InputModel = components::input_detail::InputModel;
        auto& input = ui.state<InputModel::InputState>("vault.prompt.input");
        InputModel::loadDocument(input, state.vaultPromptText);
        input.selectionStart = 0;
        input.selectionEnd = kind == VaultPromptKind::Rename
            ? platform::renameSelectionEnd(state.vaultPromptText, state.vaultContextIsDir)
            : static_cast<int>(state.vaultPromptText.size());
        input.cursor = input.selectionEnd;
        ui.requestFocus("vault.prompt.input.hit");
        state.vaultPromptFocusPending = false;
    }
    const char* title = kind == VaultPromptKind::NewFile
        ? i18n::tr("vault.new_file")
        : (kind == VaultPromptKind::NewFolder ? i18n::tr("vault.new_folder") : i18n::tr("vault.rename"));
    const EditorColors& colors = editorColors();
    const components::theme::ThemeColorTokens tokens = colors.tokens;
    const float kPanelWidth = std::min(520.0f, std::max(280.0f, screen.width - 48.0f));
    const float kPanelHeight = std::min(280.0f, std::max(184.0f, screen.height - 48.0f));
    constexpr float kPad = 20.0f;
    const float kInner = kPanelWidth - kPad * 2.0f;

    components::dialog(ui, "vault.prompt")
        .open(true)
        .theme(colors.tokens)
        .screen(screen.width, screen.height)
        .size(kPanelWidth, kPanelHeight)
        .zIndex(1200)
        .transition(quickTransition())
        .content([&, kind, tokens, colors, title] {
            ui.text("vault.prompt.title")
                .position(kPad, 18.0f)
                .size(kInner, 24.0f)
                .text(title)
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
            components::input(ui, "vault.prompt.input")
                .position(kPad, 54.0f)
                .size(kInner, 32.0f)
                .value(state.vaultPromptText)
                .placeholder(kind == VaultPromptKind::NewFolder ? i18n::tr("vault.folder_name") : i18n::tr("vault.file_name"))
                .fontSize(13.0f)
                .inset(10.0f)
                .style(inputStyle)
                .transition(quickTransition())
                .onChange([&state](const std::string& value) {
                    state.vaultPromptText = value;
                    state.vaultPromptError.clear();
                    state.vaultRenameSuggestedName.clear();
                    state.vaultRenameExtensionApprovalName.clear();
                })
                .onEscape([&state] { cancelVaultPrompt(state); })
                .onEnter(submit)
                .build();

            const float errorHeight = std::max(12.0f, kPanelHeight - 154);
            core::TextStyle errorStyle;
            errorStyle.text = state.vaultPromptError; errorStyle.fontSize = 13;
            errorStyle.fontFamily = uiFontFamily(state); errorStyle.maxWidth = kInner - 12; errorStyle.wrap = true;
            const float contentHeight = core::TextPrimitive::measureTextSize(errorStyle).y;
            const float maxOffset = std::max(0.0f, contentHeight - errorHeight);
            state.vaultPromptScroll = std::clamp(state.vaultPromptScroll, 0.0f, maxOffset);
            ui.stack("vault.prompt.errorViewport").position(kPad, 96).size(kInner, errorHeight).clip()
                .scrollState("vault.prompt.errorViewport", state.vaultPromptScroll, maxOffset, 24)
                .onScrollOffsetChanged([&state](float value) { state.vaultPromptScroll = value; })
                .content([&] {
                    ui.text("vault.prompt.error").size(kInner - 12, contentHeight)
                        .scrollContentFrom("vault.prompt.errorViewport")
                        .text(state.vaultPromptError).fontSize(13).fontFamily(uiFontFamily(state))
                        .maxWidth(kInner - 12).wrap(true).color(colors.textMuted).build();
                }).build();
            if (maxOffset > 0) components::scroll(ui, "vault.prompt.errorScrollbar").theme(tokens)
                .position(kPanelWidth - kPad - 6, 96).size(6, errorHeight)
                .scrollStateId("vault.prompt.errorViewport").offset(state.vaultPromptScroll)
                .viewport(errorHeight).content(contentHeight).build();
            const float buttonY = kPanelHeight - 54.0f;
            const float buttonWidth = state.vaultRenameSuggestedName.empty() ? 104.0f : (kInner - 16) / 3;
            const float primaryX = kPanelWidth - kPad - buttonWidth;
            const float secondaryX = primaryX - buttonWidth - 8;
            if (!state.vaultRenameSuggestedName.empty()) components::button(ui, "vault.prompt.numbered")
                .theme(tokens, false).position(kPad, buttonY).size(buttonWidth, 32)
                .text(i18n::tr("rename.use_numbered")).fontSize(12)
                .onClick([&state] { confirmVaultRename(state, true); }).build();
            components::button(ui, "vault.prompt.secondary")
                .theme(tokens, false)
                .position(secondaryX, buttonY)
                .size(buttonWidth, 32.0f)
                .text(i18n::tr("common.cancel"))
                .fontSize(13.0f)
                .radius(6.0f)
                .border(1.0f, tokens.border)
                .shadow(0.0f, 0.0f, 0.0f, components::theme::color(0.0f, 0.0f, 0.0f, 0.0f))
                .onClick([&state] { cancelVaultPrompt(state); })
                .build();
            components::button(ui, "vault.prompt.primary")
                .theme(tokens, true)
                .position(primaryX, buttonY)
                .size(buttonWidth, 32.0f)
                .text(kind == VaultPromptKind::Rename ?
                    (state.vaultRenameExtensionApprovalName == state.vaultPromptText && !state.vaultPromptText.empty()
                        ? i18n::tr("rename.confirm_extension") : i18n::tr("vault.rename")) : i18n::tr("vault.create"))
                .fontSize(13.0f)
                .radius(6.0f)
                .border(1.0f, components::theme::withAlpha(tokens.primary, 0.64f))
                .shadow(10.0f, 0.0f, 3.0f, components::theme::withAlpha(tokens.primary, 0.18f))
                .onClick(submit)
                .build();
        })
        .onOpenChange([&state](bool open) {
            if (!open) {
                cancelVaultPrompt(state);
            }
        })
        .build();
}

// 删除确认。Windows 会请求系统移入回收站；其他平台维持永久删除语义。
inline void vaultDeleteOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    const bool open = state.vaultDeletePending;
    const std::string name = state.vaultContextPath.substr(state.vaultContextPath.find_last_of('/') + 1);
    components::dialog(ui, "vault.delete")
        .open(open)
        .theme(editorColors().tokens)
        .screen(screen.width, screen.height)
#if defined(_WIN32)
        .title(i18n::tr("vault.recycle"))
        .message(state.vaultContextIsDir
                     ? i18n::format("vault.recycle_folder_prompt", {{"name", name}})
                     : i18n::format("vault.recycle_file_prompt", {{"name", name}}))
        .primaryText(i18n::tr("vault.recycle"))
#else
        .title(state.vaultContextIsDir ? i18n::tr("vault.delete_folder") : i18n::tr("vault.delete_file"))
        .message(state.vaultContextIsDir
                     ? i18n::format("vault.delete_folder_prompt", {{"name", name}})
                     : i18n::format("vault.delete_file_prompt", {{"name", name}}))
        .primaryText(i18n::tr("vault.delete_permanent"))
#endif
        .secondaryText(i18n::tr("common.cancel"))
        .zIndex(1200)
        .onPrimary([&state] {
            state.vaultDeletePending = false;
            vaultDeleteEntry(state, state.vaultContextPath, state.vaultContextIsDir);
        })
        .onSecondary([&state] { state.vaultDeletePending = false; })
        .onOpenChange([&state](bool open) {
            if (!open) {
                state.vaultDeletePending = false;
            }
        })
        .build();
}

} // namespace neo
