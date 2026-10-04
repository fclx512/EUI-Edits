#pragma once

#include "state/app_actions.h"
#include "ui/find_bar.h"
#include "ui/metrics.h"
#include "ui/tab_bar.h"
#include "ui/widgets.h"
#include "ui/ui_language.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

// Menu paths identify rows; commands identify actions independently of grouping and translation.
namespace neo {
namespace menu_detail {

using MenuItem = components::ContextMenuItem;

inline MenuItem checkedItem(bool checked, const std::string& label) {
    return MenuItem(label).withChecked(checked);
}

// 字号档位。当前值不在预设里时（手改过 settings.ini）插到最前面，
// 免得菜单上看不到自己现在是多少号。
inline std::vector<float> fontSizeOptions(const AppState& state) {
    const std::vector<float> presets{12.0f, 14.0f, 16.0f, 18.0f, 20.0f, 24.0f};
    std::vector<float> options;
    const float current = state.editorFontSize;
    const bool hasCurrent = std::any_of(presets.begin(), presets.end(), [current](float value) {
        return std::fabs(value - current) < 0.5f;
    });
    if (!hasCurrent) {
        options.push_back(current);
    }
    options.insert(options.end(), presets.begin(), presets.end());
    return options;
}

enum class Command {
    None = 0, NewText, NewMarkdown, Open, CloseTab, Exit, Save, SaveAs, OpenVault,
    Undo, Redo, Cut, Copy, SelectAll, Find, Syntax,
    Simple, Vault, Dark, Light, FollowSystem, Lines, Status, Wrap, Readable,
    LanguageSystem, LanguageChinese, LanguageEnglish, Settings,
    FontSizeBase = 100, EncodingBase = 200
};

inline MenuItem action(Command command, const char* id) {
    return MenuItem(i18n::tr(id)).withCommand(static_cast<int>(command));
}
inline MenuItem toggle(Command command, const char* id, bool value) {
    return action(command, id).withChecked(value);
}
inline std::vector<MenuItem> fileMenuItems() {
    std::vector<MenuItem> encodings;
    int index = 0;
    for (const ReopenEncodingOption& option : reopenEncodingOptions())
        encodings.push_back(MenuItem(option.label).withCommand(static_cast<int>(Command::EncodingBase) + index++));
    return {
        MenuItem(i18n::tr("menu.new"), {action(Command::NewText, "menu.text"), action(Command::NewMarkdown, "menu.markdown")}),
        action(Command::Open, "menu.open").withShortcut("Ctrl+O"),
        action(Command::CloseTab, "menu.close_tab").withShortcut("Ctrl+W").withSeparatorBefore(),
        action(Command::Save, "menu.save").withShortcut("Ctrl+S"),
        action(Command::SaveAs, "menu.save_as").withShortcut("Ctrl+Shift+S"),
        action(Command::Exit, "menu.exit").withSeparatorBefore(),
        action(Command::OpenVault, "menu.open_vault").withSeparatorBefore(),
        MenuItem(i18n::tr("menu.reopen_encoding"), std::move(encodings)),
    };
}
inline std::vector<MenuItem> editMenuItems() {
    return {
        action(Command::Undo, "menu.undo").withShortcut("Ctrl+Z"),
        action(Command::Redo, "menu.redo").withShortcut("Ctrl+Y"),
        action(Command::Cut, "menu.cut").withShortcut("Ctrl+X").withSeparatorBefore(),
        action(Command::Copy, "menu.copy").withShortcut("Ctrl+C"),
        action(Command::SelectAll, "menu.select_all").withShortcut("Ctrl+A"),
        action(Command::Find, "menu.find").withShortcut("Ctrl+F").withSeparatorBefore(),
        action(Command::Syntax, "menu.syntax").withSeparatorBefore(),
    };
}
inline std::vector<MenuItem> viewMenuItems(const AppState& state) {
    std::vector<MenuItem> sizes;
    int index = 0;
    for (float value : fontSizeOptions(state)) {
        sizes.push_back(MenuItem(std::to_string(static_cast<int>(std::lround(value))))
            .withChecked(std::fabs(value-state.editorFontSize)<.5f)
            .withCommand(static_cast<int>(Command::FontSizeBase)+index++));
    }
    return {
        MenuItem(i18n::tr("menu.layout"), {toggle(Command::Simple, "menu.simple", state.mode==EditorMode::Simple),
                                        toggle(Command::Vault, "menu.vault", state.mode==EditorMode::Vault)}),
        toggle(Command::Lines, "menu.lines", state.showLineNumbers),
        toggle(Command::Status, "menu.status", state.showStatusBar),
        toggle(Command::Wrap, "menu.wrap", state.wordWrap()).withSeparatorBefore(),
        toggle(Command::Readable, "menu.readable", state.readableWidth),
        MenuItem(i18n::tr("menu.appearance"), {
            toggle(Command::FollowSystem, "menu.theme_system", state.themeFollowSystem),
            toggle(Command::Dark, "menu.dark", !state.themeFollowSystem && state.theme==ThemeMode::Dark),
            toggle(Command::Light, "menu.light", !state.themeFollowSystem && state.theme==ThemeMode::Light)}).withSeparatorBefore(),
        MenuItem(i18n::tr("menu.font_size"), std::move(sizes))
            // 字号子级固定 112 DIP、数字居中；其余子级按统一紧凑槽位自适应宽度。
            .withChildrenPresentation(components::ContextMenuItem::ChildrenPresentation{112.0f, true, true}),
        MenuItem(i18n::tr("menu.ui_language"), {
            toggle(Command::LanguageSystem, "menu.language_system", i18n::preference()=="system"),
            toggle(Command::LanguageChinese, "menu.language_chinese", i18n::preference()=="zh-CN"),
            toggle(Command::LanguageEnglish, "menu.language_english", i18n::preference()=="en")}).withSeparatorBefore(),
        action(Command::Settings, "menu.settings").withShortcut("Ctrl+,"),
    };
}
inline int commandAtPath(const std::vector<MenuItem>& items, const std::vector<int>& path) {
    const auto* level = &items;
    const MenuItem* item = nullptr;
    for (int index : path) {
        if (index < 0 || index >= static_cast<int>(level->size())) return 0;
        item = &(*level)[index];
        level = &item->children;
    }
    return item && !item->hasChildren() && item->enabled ? item->commandId : 0;
}
inline void dispatchCommand(AppState& state, int id) {
    if (id >= static_cast<int>(Command::EncodingBase)) {
        requestReloadWithEncoding(state, static_cast<std::size_t>(id-static_cast<int>(Command::EncodingBase)));
        return;
    }
    if (id >= static_cast<int>(Command::FontSizeBase)) {
        const auto sizes = fontSizeOptions(state);
        const auto index = static_cast<std::size_t>(id-static_cast<int>(Command::FontSizeBase));
        if (index < sizes.size()) applyEditorFontSize(state, sizes[index]);
        return;
    }
    switch (static_cast<Command>(id)) {
        case Command::NewText: case Command::NewMarkdown:
            state.pendingNewMarkdown = id==static_cast<int>(Command::NewMarkdown);
            newDocument(state);
            break;
        case Command::Open:
            openFileFromDialog(state);
            break;
        case Command::CloseTab:
            requestCloseTab(state, state.tabId);
            break;
        case Command::Exit:
            if (requestCloseDocument(state)) app::requestClose();
            break;
        case Command::Save: saveDocument(state); break;
        case Command::SaveAs: saveDocumentAs(state); break;
        case Command::OpenVault: chooseVaultDirectory(state); break;
        case Command::Undo: state.pendingEditorCommand=EditorCommand::Undo; break;
        case Command::Redo: state.pendingEditorCommand=EditorCommand::Redo; break;
        case Command::Cut: state.pendingEditorCommand=EditorCommand::Cut; break;
        case Command::Copy: state.pendingEditorCommand=EditorCommand::Copy; break;
        case Command::SelectAll: state.pendingEditorCommand=EditorCommand::SelectAll; break;
        case Command::Find: openFindBar(state); break;
        case Command::Syntax: state.languageMenuOpen=true; break;
        case Command::Simple: case Command::Vault:
            state.mode=id==static_cast<int>(Command::Vault)?EditorMode::Vault:EditorMode::Simple;
            persistSettings(state); break;
        case Command::Dark: case Command::Light:
            applyTheme(state,id==static_cast<int>(Command::Light)?ThemeMode::Light:ThemeMode::Dark); break;
        case Command::FollowSystem: applyFollowSystemTheme(state); break;
        case Command::Lines: state.showLineNumbers=!state.showLineNumbers; persistSettings(state); break;
        case Command::Status: state.showStatusBar=!state.showStatusBar; applyShowStatusBar(state); break;
        case Command::Wrap: state.wrapOverride=state.wordWrap()?0:1; break;
        case Command::Readable: state.readableWidth=!state.readableWidth; persistSettings(state); break;
        case Command::LanguageSystem: applyUiLanguage(state,"system"); break;
        case Command::LanguageChinese: applyUiLanguage(state,"zh-CN"); break;
        case Command::LanguageEnglish: applyUiLanguage(state,"en"); break;
        case Command::Settings: state.settingsOpen=true; break;
        default: break;
    }
    app::requestUpdate();
}
inline void dispatchFileItem(AppState& state, const std::vector<int>& path) {
    dispatchCommand(state,commandAtPath(fileMenuItems(),path));
}
inline void dispatchEditItem(AppState& state, const std::vector<int>& path) {
    dispatchCommand(state,commandAtPath(editMenuItems(),path));
}
inline void dispatchViewItem(AppState& state, const std::vector<int>& path) {
    dispatchCommand(state,commandAtPath(viewMenuItems(state),path));
}

inline float menuTitleWidth(const std::string& label, const UiMetrics& metrics) {
    return estimateTextWidth(label, metrics.menuFontSize) + metrics.menuTitlePadding * 2.0f;
}

// 菜单栏上的一个可点标题（背景命中块 + 文字，跟 contextMenu 的画法一致）。
// 空回调的 std::function 会被框架当成"没有回调"，所以这里可以无条件挂上。
inline void menuTitleView(eui::Ui& ui,
                          const EditorColors& colors,
                          const UiMetrics& metrics,
                          const char* fontFamily,
                          const std::string& id,
                          const std::string& label,
                          float x,
                          float width,
                          float fontSize,
                          bool active,
                          bool interactive,
                          std::function<void()> onClick,
                          std::function<void(bool)> onHover) {
    const float height = metrics.menuBarHeight;
    ui.rect(id + ".hit")
        .x(x)
        .y(0.0f)
        .size(width, height)
        .radius(6.0f)
        .states(active ? rgba(colors.accent.r, colors.accent.g, colors.accent.b, 0.16f)
                       : transparentColor(),
                colors.rowHover,
                colors.pressed)
        // 点击不该把编辑区的焦点抢走（开发记录 §3.5）：否则点完菜单还得再点一次编辑区。
        .preserveFocusOnPress()
        .disabled(!interactive)
        .onClick(std::move(onClick))
        .onHover(std::move(onHover))
        .transition(quickTransition())
        .build();

    ui.text(id + ".label")
        .x(x)
        .y(0.0f)
        .size(width, height)
        .text(label)
        .fontFamily(fontFamily)
        .fontSize(fontSize)
        .color(active ? colors.accent : colors.text)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

// 菜单栏右侧的设置按钮：齿轮 + "设置"文字。纯图标那版用户看不惯——
// 齿轮语义不直观，恢复文字描述后按钮宽度按文字实际宽度算，右缘留边对齐。
inline float menuSettingsButtonWidth(const UiMetrics& metrics, const char* label) {
    const float glyph = std::min(16.0f, metrics.menuBarHeight - 14.0f);
    return 8.0f + glyph + 6.0f + estimateTextWidth(label, metrics.menuFontSize) + 12.0f;
}

inline void menuSettingsButtonView(eui::Ui& ui,
                                   const EditorColors& colors,
                                   const UiMetrics& metrics,
                                   const std::string& id,
                                   UiIcon icon,
                                   const char* label,
                                   const char* fontFamily,
                                   float x,
                                   float width,
                                   bool active,
                                   bool interactive,
                                   std::function<void()> onClick) {
    const float height = metrics.menuBarHeight;
    const float glyph = std::min(16.0f, height - 14.0f);
    ui.rect(id + ".hit")
        .x(x)
        .y(0.0f)
        .size(width, height)
        .radius(6.0f)
        .states(active ? rgba(colors.accent.r, colors.accent.g, colors.accent.b, 0.16f)
                       : transparentColor(),
                colors.rowHover,
                colors.pressed)
        .preserveFocusOnPress()
        .disabled(!interactive)
        .onClick(std::move(onClick))
        .transition(quickTransition())
        .build();
    iconView(ui, id + ".glyph", icon, x + 8.0f, (height - glyph) * 0.5f, glyph,
             active ? colors.accent : colors.text);
    ui.text(id + ".label")
        .x(x + 8.0f + glyph + 6.0f)
        .y(0.0f)
        .size(std::max(0.0f, width - 8.0f - glyph - 6.0f - 12.0f), height)
        .text(label)
        .fontFamily(fontFamily)
        .fontSize(metrics.menuFontSize)
        .color(active ? colors.accent : colors.text)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

} // namespace menu_detail

inline void menuBarView(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);

    struct Title {
        MenuKind kind;
        const char* label;
    };
    const std::array<Title, 3> titles{{{MenuKind::File, i18n::tr("menu.file")},
                                       {MenuKind::Edit, i18n::tr("menu.edit")},
                                       {MenuKind::View, i18n::tr("menu.view")}}};

    std::array<float, 3> xs{};
    std::array<float, 3> widths{};
    float cursor = 10.0f;
    for (std::size_t index = 0; index < titles.size(); ++index) {
        widths[index] = menu_detail::menuTitleWidth(titles[index].label, metrics);
        xs[index] = cursor;
        cursor += widths[index] + metrics.menuTitleGap;
    }

    // ContextMenuStyle 里没有字号，字号只在 metrics 里——这里覆盖成菜单栏字号，
    // 弹层和标题就不会一个 15 一个 14。
    components::theme::ThemeColorTokens menuTokens = colors.tokens;
    menuTokens.metrics.typography.option = metrics.menuFontSize;

    // 设置面板开着的时候不让菜单栏抢事件，否则菜单会盖在对话框上面。
    const bool interactive = !state.settingsOpen;

    // Dismiss covers content below the bar; title clicks and hover remain interactive.
    const auto popup = [&](MenuKind kind,
                           const std::string& id,
                           float x,
                           std::vector<components::ContextMenuItem> items,
                           std::function<void(const std::vector<int>&)> onSelect) {
        components::contextMenu(ui, id)
            .open(state.openMenu == kind)
            .screen(screen.width, screen.height)
            .position(x, metrics.menuBarHeight)
            .dismissTopInset(metrics.menuBarHeight)
            .size(std::max(metrics.menuWidth, i18n::language()=="en"?300.0f:metrics.menuWidth), metrics.menuItemHeight)
            .items(std::move(items))
            .theme(menuTokens)
            .transition(quickTransition())
            .zIndex(900)
            .onSelectPath(std::move(onSelect))
            .onOpenChange([&state, kind](bool open) {
                // 点空白 / 选中某项后关闭。标题上的点击不经过遮罩，不会走到这里。
                if (!open && state.openMenu == kind) {
                    state.openMenu = MenuKind::None;
                }
            })
            .build();
    };

    popup(MenuKind::File,
          "menubar.menu.file",
          xs[0],
          menu_detail::fileMenuItems(),
          [&state](const std::vector<int>& path) {
              menu_detail::dispatchFileItem(state, path);
          });
    popup(MenuKind::Edit,
          "menubar.menu.edit",
          xs[1],
          menu_detail::editMenuItems(),
          [&state](const std::vector<int>& path) {
              menu_detail::dispatchEditItem(state, path);
          });
    popup(MenuKind::View,
          "menubar.menu.view",
          xs[2],
          menu_detail::viewMenuItems(state),
          [&state](const std::vector<int>& path) { menu_detail::dispatchViewItem(state, path); });

    // Modal dismiss layer for the tab list. It is inserted before the menu row so
    // menu titles, tab cards, and the list itself remain directly clickable.
    if (state.tabListOpen) {
        ui.rect("menubar.tabs.list.dismiss")
            .size(screen.width, screen.height)
            .color(transparentColor())
            .zIndex(1100)
            .onClick([&state, &ui] {
                state.tabListOpen = false;
                ui.state<std::uint64_t>("menubar.tabs.hovered") = 0;
                ui.requestFocus(editorInputId(state.tabId) + ".hit");
                app::requestUpdate();
            })
            .build();
    }

    ui.stack("menubar")
        .size(screen.width, metrics.menuBarHeight)
        .zIndex(2000)
        .content([&] {
            ui.rect("menubar.bg")
                .fill()
                .color(colors.toolbar)
                .build();
            ui.rect("menubar.hairline")
                .x(0.0f)
                .y(metrics.menuBarHeight - 1.0f)
                .size(screen.width, 1.0f)
                .color(colors.border)
                .build();

            for (std::size_t index = 0; index < titles.size(); ++index) {
                const MenuKind kind = titles[index].kind;
                const bool active = state.openMenu == kind;
                menu_detail::menuTitleView(
                    ui,
                    colors,
                    metrics,
                    uiFontFamily(state),
                    "menubar.title." + std::to_string(index),
                    titles[index].label,
                    xs[index],
                    widths[index],
                    metrics.menuFontSize,
                    active,
                    interactive,
                    [&state, &ui, kind] {
                        state.tabListOpen = false;
                        ui.state<std::uint64_t>("menubar.tabs.hovered") = 0;
                        ui.requestFocus(editorInputId(state.tabId) + ".hit");
                        state.openMenu = state.openMenu == kind ? MenuKind::None : kind;
                        app::requestUpdate();
                    },
                    [&state, kind](bool hovered) {
                        if (hovered && state.openMenu!=MenuKind::None && state.openMenu!=kind) {
                            state.openMenu=kind;
                            app::requestUpdate();
                        }
                    });
            }

            const char* settingsLabel = i18n::tr("menu.settings_short");
            const float settingsWidth = menu_detail::menuSettingsButtonWidth(metrics, settingsLabel);
            const float settingsX = std::max(10.0f, screen.width - settingsWidth - 10.0f);
            menu_detail::menuSettingsButtonView(ui,
                                                colors,
                                                metrics,
                                                "menubar.settings",
                                                UiIcon::Settings,
                                                settingsLabel,
                                                uiFontFamily(state),
                                                settingsX,
                                                settingsWidth,
                                                state.settingsOpen,
                                                interactive,
                                                [&state, &ui] {
                                                    state.tabListOpen = false;
                                                    ui.state<std::uint64_t>("menubar.tabs.hovered") = 0;
                                                    ui.requestFocus(editorInputId(state.tabId) + ".hit");
                                                    state.openMenu = MenuKind::None;
                                                    state.settingsOpen = true;
                                                    app::requestUpdate();
                                                });

            const float menuEnd = cursor - metrics.menuTitleGap;
            const float sidebarWidth = state.mode == EditorMode::Vault
                ? std::min(state.vaultWidth, screen.width * 0.38f) : 0.0f;
            const float tabsLeft = std::max(menuEnd + 12.0f,
                                            state.mode == EditorMode::Vault ? sidebarWidth + 1.0f : 0.0f);
            tabBarView(ui, state, screen, tabsLeft, settingsX - 8.0f);
        })
        .build();
}

} // namespace neo
