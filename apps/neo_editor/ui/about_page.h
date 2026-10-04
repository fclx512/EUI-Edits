#pragma once

#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/widgets.h"
#include "state/app_actions.h"
#include "model/version.h"
#include "core/platform/bundled_resources.h"
#include <neo_editor_icon_generated.h>

#include <array>

namespace neo::settings_detail {

inline const char* aboutWindowBackend() {
#if defined(EUI_WINDOW_BACKEND_WIN32)
    return "Win32";
#elif defined(EUI_WINDOW_BACKEND_SDL2)
    return "SDL2";
#else
    return "GLFW";
#endif
}

inline const char* aboutRenderBackend() {
#if defined(EUI_RENDER_BACKEND_D2D)
    return "Direct2D";
#elif defined(EUI_RENDER_BACKEND_VULKAN)
    return "Vulkan";
#else
    return "OpenGL";
#endif
}

inline void aboutPage(eui::Ui& ui, AppState& state, const EditorColors& colors,
                      const UiMetrics& metrics, float width, float inset,
                      float availableHeight) {
    if (width <= 0.0f) return;
    const char* font = uiFontFamily(state);
    const float labelSize = metrics.panelFontSize;
    const float hintSize = metrics.panelHintFontSize;
    const float brandSize = metrics.pageTitleFontSize;
    const float sloganSize = metrics.panelLabelFontSize + 4.0f;
    const float padding = std::min(22.0f, width * .08f);
    const float inner = std::max(1.0f, width - padding * 2.0f);
    const float gap = 12.0f;
    const float buttonHeight = std::max(34.0f, labelSize + 16.0f);
    const auto textWidth = [&](const std::string& text, float size, int weight = 400) {
        return core::TextPrimitive::measureTextWidth(text, font, size, weight);
    };
    const auto textHeight = [&](const std::string& text, float size, float maxWidth, int weight = 400) {
        core::TextStyle style;
        style.text = text; style.fontFamily = font; style.fontSize = size; style.fontWeight = weight;
        style.maxWidth = std::max(1.0f, maxWidth); style.wrap = true; style.lineHeight = size + 6.0f;
        return std::max(style.lineHeight, core::TextPrimitive::measureTextSize(style).y);
    };
    const auto text = [&](const std::string& id, const std::string& value, float x, float y,
                          float textW, float size, eui::Color color, int weight = 400,
                          eui::HorizontalAlign align = eui::HorizontalAlign::Left) {
        ui.text(id).position(x, y).size(textW, textHeight(value, size, textW, weight))
            .text(value).wrap(true).fontFamily(font).fontSize(size).fontWeight(weight)
            .lineHeight(size + 6.0f).color(color).horizontalAlign(align).build();
    };
    const auto card = [&](const std::string& id, float y, float height, bool hero = false) {
        ui.rect(id).position(0, y).size(width, height).radius(hero ? 10.0f : 8.0f)
            .color(hero ? core::mixColor(colors.editor, colors.accent, .055f) : colors.editor)
            .border(1.0f, components::theme::withAlpha(colors.border, .45f)).build();
    };
    const auto buttonWidth = [&](const std::string& label) {
        return std::min(inner, textWidth(label, hintSize) + 28.0f);
    };
    const auto button = [&](const std::string& id, const std::string& label, float x,
                            float y, float buttonW, std::function<void()> action, bool link = false) {
        components::button(ui, id).position(x, y).size(buttonW, buttonHeight)
            .text(label).fontSize(hintSize)
            .textColor(link ? colors.accent : colors.text)
            .colors(colors.editor, colors.rowHover, colors.pressed)
            .border(1.0f, components::theme::withAlpha(colors.border, .7f)).radius(5.0f)
            .shadow(0, 0, 0, transparentColor()).pressScale(1.0f).preserveFocusOnPress()
            .onClick(std::move(action)).transition(quickTransition()).build();
    };

    // Measure the localized text at the current UI font and available width. The
    // same dimensions drive drawing and scroll extent, including enlarged fonts.
    const float logoSize = std::max(48.0f, labelSize * 3.4f);
    const bool stackedBrand = inner < logoSize + 16.0f + textWidth("EUI-Edits", brandSize, 600);
    const float brandX = stackedBrand ? padding : padding + logoSize + 14.0f;
    const float brandWidth = stackedBrand ? inner : std::max(1.0f, inner - logoSize - 14.0f);
    const float brandY = padding + (stackedBrand ? logoSize + 10.0f : 0.0f);
    const float nameHeight = textHeight("EUI-Edits", brandSize, brandWidth, 600);
    const float platformY = brandY + nameHeight + 3.0f;
    const float platformHeight = textHeight(i18n::tr("about.platform"), hintSize, brandWidth);
    const float sloganY = std::max(padding + logoSize, platformY + platformHeight) + 18.0f;
    const std::string lead = i18n::tr("about.slogan_lead"), end = i18n::tr("about.slogan_end");
    const float leadW = textWidth(lead, sloganSize, 600) + 6.0f;
    const bool stackedSlogan = leadW + textWidth(end, sloganSize, 600) > inner;
    const float leadH = textHeight(lead, sloganSize, stackedSlogan ? inner : leadW, 600);
    const float endW = stackedSlogan ? inner : std::max(1.0f, inner - leadW);
    const float endH = textHeight(end, sloganSize, endW, 600);
    const float sloganHeight = stackedSlogan ? leadH + endH : std::max(leadH, endH);
    const float descriptionY = sloganY + sloganHeight + 8.0f;
    const float heroHeight = descriptionY + textHeight(i18n::tr("about.description"), hintSize, inner) + padding;

    struct Feature { const char* title; const char* hint; UiIcon icon; };
    const std::array<Feature, 4> features{{
        {"about.markdown", "about.markdown_hint", UiIcon::MarkdownFile},
        {"about.text", "about.text_hint", UiIcon::CodeFile},
        {"about.library", "about.library_hint", UiIcon::FolderOpen},
        {"about.appearance", "about.appearance_hint", UiIcon::Sliders}
    }};
    float minFeatureWidth = 0;
    for (const auto& feature : features) {
        minFeatureWidth = std::max(minFeatureWidth, std::max(textWidth(i18n::tr(feature.title), labelSize),
                                    textWidth(i18n::tr(feature.hint), hintSize)) + 24.0f);
    }
    const int columns = width >= minFeatureWidth * 4 ? 4 : width >= minFeatureWidth * 2 ? 2 : 1;
    const int rows = 4 / columns;
    const float cellW = width / columns;
    const float featureIconSize = std::max(22.0f, labelSize * 1.5f);
    const float cellTextW = std::max(1.0f, cellW - 24.0f);
    std::array<float, 4> rowHeights{};
    for (int i = 0; i < 4; ++i) {
        rowHeights[i / columns] = std::max(rowHeights[i / columns], featureIconSize + 9.0f +
            textHeight(i18n::tr(features[i].title), labelSize, cellTextW) + 4.0f +
            textHeight(i18n::tr(features[i].hint), hintSize, cellTextW));
    }
    const float featureY = heroHeight + 22.0f;
    float featureHeight = 0;
    for (int row = 0; row < rows; ++row) featureHeight += rowHeights[row] + (row ? 16.0f : 0.0f);

    const std::string frameworkStack = i18n::format("about.framework_stack", {
        {"window_backend", aboutWindowBackend()}, {"render_backend", aboutRenderBackend()}});
    const std::string frameworkBody = std::string(i18n::tr("about.framework_description")) + "\n" + frameworkStack;
    const std::string euiLinkLabel = std::string(i18n::tr("about.framework_link")) + "  ↗";
    const float euiButtonW = buttonWidth(euiLinkLabel);
    const float euiHeadH = hintSize + 6.0f + 4.0f + textHeight("EUI-NEO", metrics.panelLabelFontSize, inner, 600);
    const bool stackedEui = inner < euiButtonW + std::max(textWidth("EUI-NEO", metrics.panelLabelFontSize, 600),
                                                 textWidth(i18n::tr("about.framework"), hintSize)) + 24.0f;
    const float euiY = featureY + featureHeight + 22.0f;
    const float euiBodyY = euiY + padding + std::max(euiHeadH, stackedEui ? 0.0f : buttonHeight) + 10.0f;
    const float euiBodyH = textHeight(frameworkBody, hintSize, inner);
    const float euiButtonY = stackedEui ? euiBodyY + euiBodyH + 12.0f
                                      : euiY + padding + (euiHeadH - buttonHeight) * .5f;
    const float euiHeight = (stackedEui ? euiButtonY + buttonHeight : euiBodyY + euiBodyH) - euiY + padding;

    const std::string copyLabel = i18n::tr("about.copy_version");
    const float copyW = buttonWidth(copyLabel);
    const bool stackedVersion = inner < copyW + std::max(textWidth(version::kVersion, labelSize + 4.0f, 600),
                                                  textWidth(i18n::tr("about.current_version"), hintSize)) + 24.0f;
    const float versionY = euiY + euiHeight + gap;
    const float versionTextW = stackedVersion ? inner : std::max(1.0f, inner - copyW - 20.0f);
    const float versionLabelH = textHeight(i18n::tr("about.current_version"), hintSize, versionTextW);
    const float versionValueH = textHeight(version::kVersion, labelSize + 4.0f, versionTextW, 600);
    const float versionHeadH = versionLabelH + 4.0f + versionValueH;
    const float copyY = versionY + padding + (stackedVersion ? versionHeadH + 12.0f : (versionHeadH-buttonHeight)*.5f);
    const float versionHeight = padding * 2.0f + std::max(versionHeadH, buttonHeight) +
                                (stackedVersion ? buttonHeight + 12.0f : 0.0f);

    const std::string licenseLabel = i18n::tr("about.licenses_button");
    const float licenseButtonW = buttonWidth(licenseLabel);
    const float licenseY = versionY + versionHeight + gap;
    const bool stackedLicense = inner < licenseButtonW + std::max(
        textWidth(i18n::tr("about.licenses_title"), labelSize, 600),
        textWidth(i18n::tr("about.licenses_hint"), hintSize)) + 24.0f;
    const float licenseTextW = stackedLicense ? inner : inner-licenseButtonW-20.0f;
    const float licenseTitleH = textHeight(i18n::tr("about.licenses_title"), labelSize, licenseTextW, 600);
    const float licenseHintY = licenseY + padding + licenseTitleH + 5.0f;
    const float licenseHintH = textHeight(i18n::tr("about.licenses_hint"), hintSize, licenseTextW);
    const float licenseHeadH = licenseTitleH + 5.0f + licenseHintH;
    const float licenseButtonY = licenseY + padding + (stackedLicense ? licenseHeadH + 14.0f
                                                                     : (licenseHeadH-buttonHeight)*.5f);
    const float licenseHeight = padding*2 + std::max(licenseHeadH, buttonHeight) +
                                (stackedLicense ? buttonHeight+14.0f : 0.0f);
    const float footerY = licenseY + licenseHeight + 18.0f;
    const float footerHeight = textHeight(i18n::tr("about.footer"), hintSize, inner) +
                               textHeight("EUI-Edits · Apache-2.0", hintSize, inner) + 4.0f;
    const float contentHeight = footerY + footerHeight + 18.0f;
    const float scrollTop = 18.0f;
    const float scrollHeight = std::max(0.0f, availableHeight - scrollTop - 18.0f);
    const float maxOffset = std::max(0.0f, contentHeight - scrollHeight);
    state.settingsScroll = std::clamp(state.settingsScroll, 0.0f, maxOffset);
    const float step = std::max(20.0f, std::min(80.0f, scrollHeight * .45f));
    const std::string scrollId = "settings.about.scroll";
    ui.stack(scrollId).position(inset, scrollTop).size(width, scrollHeight).clip()
        .scrollState(scrollId, state.settingsScroll, maxOffset, step)
        .onScrollOffsetChanged([&state](float value) {
            state.settingsScroll = value;
            app::requestUpdate();
        })
        .content([&] {
            ui.stack("settings.about.content").size(width, contentHeight).scrollContentFrom(scrollId)
                .content([&] {
                    card("settings.about.hero", 0, heroHeight, true);
                    ui.svg("settings.about.icon").position(padding, padding).size(logoSize, logoSize)
                        .source(application_icon::kSvg).build();
                    text("settings.about.brand", "EUI-Edits", brandX, brandY, brandWidth, brandSize, colors.text, 600);
                    text("settings.about.platform", i18n::tr("about.platform"), brandX, platformY, brandWidth, hintSize, colors.textMuted);
                    text("settings.about.slogan.lead", lead, padding, sloganY, stackedSlogan ? inner : leadW, sloganSize, colors.accent, 600);
                    text("settings.about.slogan.end", end, padding + (stackedSlogan ? 0 : leadW),
                         sloganY + (stackedSlogan ? leadH : 0), endW, sloganSize, colors.text, 600);
                    text("settings.about.description", i18n::tr("about.description"), padding, descriptionY, inner, hintSize, colors.textMuted);
                    float rowY = featureY;
                    for (int row = 0; row < rows; ++row) {
                        for (int col = 0; col < columns; ++col) {
                            const int i = row * columns + col;
                            const float x = col * cellW;
                            const std::string id = "settings.about.feature." + std::to_string(i);
                            iconView(ui, id + ".icon", features[i].icon, x + (cellW-featureIconSize)*.5f, rowY, featureIconSize, colors.accent);
                            const float titleY = rowY + featureIconSize + 9.0f;
                            text(id + ".title", i18n::tr(features[i].title), x + 12, titleY, cellTextW, labelSize, colors.text, 400, eui::HorizontalAlign::Center);
                            text(id + ".hint", i18n::tr(features[i].hint), x + 12,
                                 titleY + textHeight(i18n::tr(features[i].title), labelSize, cellTextW) + 4,
                                 cellTextW, hintSize, colors.textMuted, 400, eui::HorizontalAlign::Center);
                            if (col + 1 < columns) ui.rect(id + ".rule").position(x + cellW, rowY + 4)
                                .size(1, std::max(0.0f, rowHeights[row]-8)).color(colors.border).build();
                        }
                        rowY += rowHeights[row] + 16.0f;
                    }
                    card("settings.about.eui.card", euiY, euiHeight);
                    text("settings.about.eui.label", i18n::tr("about.framework"), padding, euiY + padding, inner, hintSize, colors.textMuted);
                    text("settings.about.eui.name", "EUI-NEO", padding, euiY + padding + hintSize + 10.0f, inner,
                         metrics.panelLabelFontSize, colors.text, 600);
                    text("settings.about.eui.body", frameworkBody, padding, euiBodyY, inner, hintSize, colors.textMuted);
                    button("settings.about.eui.link", euiLinkLabel, stackedEui ? padding : width-padding-euiButtonW,
                           euiButtonY, euiButtonW, [&state] {
                               if (!core::platform::openUrl("https://github.com/sudoevolve/EUI-NEO")) {
                                   showToast(state, i18n::tr("safety.link_failed"), "https://github.com/sudoevolve/EUI-NEO");
                                   app::requestUpdate();
                               }
                           }, true);
                    card("settings.about.version.card", versionY, versionHeight);
                    text("settings.about.version.label", i18n::tr("about.current_version"), padding, versionY+padding, versionTextW, hintSize, colors.textMuted);
                    text("settings.about.version", version::kVersion, padding, versionY+padding+versionLabelH+4,
                         versionTextW, labelSize+4, colors.text, 600);
                    button("settings.about.version.copy", copyLabel, stackedVersion ? padding : width-padding-copyW,
                           copyY, copyW, [&state] {
                               core::window::setClipboardText(version::kDisplayVersion);
                               showToast(state, i18n::tr("about.version_copied"), version::kDisplayVersion);
                               app::requestUpdate();
                           });
                    card("settings.about.licenses.card", licenseY, licenseHeight);
                    text("settings.about.licenses.title", i18n::tr("about.licenses_title"), padding, licenseY+padding, licenseTextW, labelSize, colors.text, 600);
                    text("settings.about.licenses.hint", i18n::tr("about.licenses_hint"), padding, licenseHintY, licenseTextW, hintSize, colors.textMuted);
                    button("settings.about.licenses.button", licenseLabel, stackedLicense ? padding : width-padding-licenseButtonW, licenseButtonY, licenseButtonW,
                           [] { core::platform::showEuiEditsLicenseExportDialog(); });
                    text("settings.about.footer", i18n::tr("about.footer"), padding, footerY, inner, hintSize, colors.textMuted);
                    text("settings.about.copyright", "EUI-Edits · Apache-2.0", padding,
                         footerY + textHeight(i18n::tr("about.footer"), hintSize, inner) + 4.0f, inner, hintSize, colors.textMuted);
                }).build();
        }).build();
    if (maxOffset > 0.0f) {
        components::scroll(ui, "settings.about.scrollbar").theme(colors.tokens).scrollStateId(scrollId)
            .position(width + inset - 2, scrollTop).size(8, scrollHeight).viewport(scrollHeight)
            .content(contentHeight).offset(state.settingsScroll).step(step).build();
    }
}

} // namespace neo::settings_detail
