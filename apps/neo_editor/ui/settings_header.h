#pragma once

#include "ui/icons.h"
#include "ui/metrics.h"
#include "platform/window_chrome.h"

namespace neo {

inline const char* settingsHeaderTitle(const AppState& state) {
    return i18n::tr(state.fontPicker != FontPickerTarget::None ? "settings.choose_font"
        : state.themeListOpen ? "settings.theme_library" : "settings.title");
}

struct SettingsHeaderLayout {
    bool compact = false;
    float height = 0, backY = 0, titleX = 0, titleY = 0, titleWidth = 0, titleHeight = 0;
};
inline SettingsHeaderLayout settingsHeaderLayout(const AppState& state, float width) {
    const auto metrics = uiMetrics(state);
    SettingsHeaderLayout layout;
    const float singleRowWidth = std::max(0.0f, width - 194.0f - platform::chrome::controlsWidth - 16.0f);
    core::TextStyle titleStyle;
    titleStyle.text = settingsHeaderTitle(state);
    titleStyle.fontFamily = uiFontFamily(state);
    titleStyle.fontSize = metrics.pageTitleFontSize;
    titleStyle.fontWeight = 600;
    const float titleAdvance = core::TextPrimitive::measureTextWidth(
        titleStyle.text, titleStyle.fontFamily, titleStyle.fontSize, titleStyle.fontWeight);
    layout.compact = width < 560.0f || titleAdvance + 8.0f > singleRowWidth;
    layout.titleX = layout.compact ? 18.0f : 194.0f;
    layout.titleY = layout.compact ? metrics.menuBarHeight + 8.0f : 0.0f;
    layout.titleWidth = layout.compact ? std::max(1.0f, width - 36.0f) : singleRowWidth;
    titleStyle.maxWidth = layout.titleWidth;
    titleStyle.wrap = layout.compact;
    layout.titleHeight = layout.compact
        ? std::max(metrics.pageTitleFontSize + 8.0f, core::TextPrimitive::measureTextSize(titleStyle).y + 8.0f)
        : std::max(72.0f, metrics.pageTitleFontSize + 36.0f);
    layout.height = layout.compact ? layout.titleY + layout.titleHeight + 8.0f : layout.titleHeight;
    layout.backY = ((layout.compact ? metrics.menuBarHeight : layout.height) - metrics.panelFontSize - 14.0f) * .5f;
    return layout;
}

inline void settingsHeaderView(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    const auto metrics = uiMetrics(state);
    const auto& colors = editorColors();
    const auto layout = settingsHeaderLayout(state, screen.width);
    const float height = layout.height;
    const float backHeight = metrics.panelFontSize + 14.0f;
    const float backY = layout.backY;
    platform::chrome::extendCaptionTo(height);
    platform::chrome::exclude(18, backY, 154, backHeight);
    ui.rect("settings.page.header").size(screen.width, height).color(colors.toolbar).build();
    ui.stack("settings.backToEditor.slot").position(18, backY).size(154, backHeight).content([&] {
        iconTextButton(ui, "settings.backToEditor", UiIcon::ArrowLeft, i18n::tr("settings.back_editor"), 154,
                       metrics.panelFontSize, [&state] {
                           state.settingsOpen = false;
                           state.fontPicker = FontPickerTarget::None;
                           state.themeListOpen = false;
                           app::requestUpdate();
                       }, uiFontFamily(state));
    }).build();
    // At high UI scale a narrow window cannot fit the back label, page title
    // and native window actions in one row. Put the title below the actions.
    ui.text("settings.page.title")
        .position(layout.titleX, layout.titleY).size(layout.titleWidth, layout.titleHeight)
        .text(settingsHeaderTitle(state)).wrap(layout.compact)
        .fontFamily(uiFontFamily(state)).fontSize(metrics.pageTitleFontSize).fontWeight(600)
        .color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
}

} // namespace neo
