#pragma once

#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/widgets.h"
#include "state/app_actions.h"
#include "model/version.h"
#include "model/settings.h"
#include "core/platform/bundled_resources.h"
#include <neo_editor_icon_generated.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace neo::settings_detail {

inline std::string aboutIconPath(float physicalSize) {
    constexpr std::array<int, 6> sizes{{48, 60, 72, 96, 120, 144}};
    const int target = std::clamp(static_cast<int>(std::lround(physicalSize)), sizes.front(), sizes.back());
    std::size_t selected = 0;
    for (std::size_t i = 1; i < sizes.size(); ++i) {
        if (std::abs(sizes[i] - target) < std::abs(sizes[selected] - target)) selected = i;
    }
    static std::array<std::string, sizes.size()> paths{};
    static std::array<bool, sizes.size()> attempted{};
    if (attempted[selected]) return paths[selected];
    attempted[selected] = true;

    const auto resourceId = static_cast<core::platform::BundledResourceId>(510 + selected);
    const auto resource = core::platform::bundledResource(resourceId);
    if (!resource) return {};
    std::uint64_t contentHash = 14695981039346656037ull;
    for (std::size_t i = 0; i < resource.size; ++i) {
        contentHash = (contentHash ^ resource.data[i]) * 1099511628211ull;
    }
    const std::string configDirectory = settings::configDirectory();
    if (configDirectory.empty()) return {};
    char suffix[64]{};
    std::snprintf(suffix, sizeof(suffix), "about-icon-%d-%016llx.png", sizes[selected],
                  static_cast<unsigned long long>(contentHash));
    const std::filesystem::path cacheDirectory = textfile::pathFromUtf8(configDirectory) / "cache";
    const std::filesystem::path cacheFile = cacheDirectory / suffix;
    std::error_code error;
    std::filesystem::create_directories(cacheDirectory, error);
    if (error) return {};
    const bool complete = std::filesystem::exists(cacheFile, error) && !error &&
                          std::filesystem::file_size(cacheFile, error) == resource.size && !error;
    if (!complete) {
        std::ofstream output(cacheFile, std::ios::binary | std::ios::trunc);
        if (!output) return {};
        output.write(reinterpret_cast<const char*>(resource.data), static_cast<std::streamsize>(resource.size));
        output.close();
        if (!output) return {};
    }
    paths[selected] = cacheFile.u8string();
    return paths[selected];
}

inline void aboutPage(eui::Ui& ui, AppState& state, const EditorColors& colors,
                      const UiMetrics& metrics, float width, float inset,
                      float availableHeight) {
    if (width <= 0.0f) return;
    const char* font = uiFontFamily(state);
    const float labelSize = metrics.panelFontSize;
    const float hintSize = metrics.panelHintFontSize;
    const float brandSize = metrics.pageTitleFontSize;
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
        ui.rect(id).position(0, y).size(width, height).radius(neo::corner(colors, hero ? 10.0f : 8.0f))
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
            .border(1.0f, components::theme::withAlpha(colors.border, .7f)).radius(neo::corner(colors, 5.0f))
            .shadow(0, 0, 0, transparentColor()).pressScale(1.0f).preserveFocusOnPress()
            .onClick(std::move(action)).transition(quickTransition()).build();
    };

    static constexpr char repository[] = "https://github.com/fclx512/EUI-Edits";
    static constexpr char frameworkRepository[] = "https://github.com/sudoevolve/EUI-NEO";
    const auto openLink = [&state](const char* url) {
        if (!core::platform::openUrl(url)) {
            showToast(state, i18n::tr("safety.link_failed"), url);
            app::requestUpdate();
        }
    };

    const std::string repositoryLabel = std::string(i18n::tr("about.repository")) + "  ↗";
    const std::string slogan = i18n::tr("about.slogan");
    const float logoSize = std::max(48.0f, labelSize * 3.4f);
    const float repositoryW = buttonWidth(repositoryLabel);
    const float brandTextW = std::max(textWidth("EUI-Edits", brandSize, 600), textWidth(slogan, labelSize, 600));
    const bool logoAbove = inner < logoSize + 14.0f + textWidth("EUI-Edits", brandSize, 600);
    const float logoSpace = logoAbove ? 0.0f : logoSize + 14.0f;
    const bool repositoryBelow = logoAbove || inner < logoSpace + brandTextW + gap + repositoryW;
    const float brandWidth = std::max(1.0f, inner - logoSpace - (repositoryBelow ? 0.0f : gap + repositoryW));
    const float nameHeight = textHeight("EUI-Edits", brandSize, brandWidth, 600);
    const float brandHeight = nameHeight + 2.0f + textHeight(slogan, labelSize, brandWidth, 600);
    const float heroHeaderH = logoAbove ? logoSize + gap + brandHeight : std::max(logoSize, brandHeight);
    const float brandY = padding + (logoAbove ? logoSize + gap : (heroHeaderH - brandHeight) * .5f);
    const float logoY = padding + (logoAbove ? 0.0f : (heroHeaderH - logoSize) * .5f);
    const float repositoryY = padding + (repositoryBelow ? heroHeaderH + gap : (heroHeaderH - buttonHeight) * .5f);
    const float heroHeight = padding * 2.0f + heroHeaderH + (repositoryBelow ? gap + buttonHeight : 0.0f);

    struct FileKind { UiIcon icon; eui::Color color; std::array<const char*, 3> formats; };
    const std::array<FileKind, 6> fileKinds{{
        {UiIcon::MarkdownFile, colors.iconMd, {".md", ".markdown"}},
        {UiIcon::TextFile, colors.iconTxt, {".txt", ".log", ".csv"}},
        {UiIcon::CodeFile, colors.iconCode, {".cpp", ".py", ".js"}},
        {UiIcon::DataFile, colors.iconData, {".json", ".yaml", ".toml"}},
        {UiIcon::ImageFile, colors.accent, {".png", ".jpg", ".bmp"}},
        {UiIcon::File, colors.textMuted, {"README", "LICENSE"}}
    }};
    constexpr float pillGap = 4.0f;
    constexpr float iconGap = 7.0f;
    const float minCellWidth = std::max(96.0f, textWidth(".markdown", hintSize, 600) + 24.0f);
    const int columns = inner >= minCellWidth * 6 ? 6 : inner >= minCellWidth * 3 ? 3 : 2;
    const int rows = static_cast<int>(fileKinds.size()) / columns;
    const float cellW = inner / columns;
    const float pillAreaW = std::max(1.0f, cellW - pillGap * 2.0f);
    const float fileIconSize = std::max(30.0f, labelSize * 1.8f);

    struct PillLayout { const char* label = nullptr; float x = 0, width = 0; int row = 0; };
    std::array<std::array<PillLayout, 3>, 6> filePills{};
    std::array<float, 3> rowHeights{};
    for (std::size_t i = 0; i < fileKinds.size(); ++i) {
        std::array<float, 3> lineWidths{};
        int line = 0;
        for (std::size_t j = 0; j < fileKinds[i].formats.size(); ++j) {
            const char* format = fileKinds[i].formats[j];
            if (!format) break;
            const float pillW = std::min(pillAreaW, textWidth(format, hintSize, 600) + 12.0f);
            if (lineWidths[line] > 0 && lineWidths[line] + pillGap + pillW > pillAreaW) ++line;
            const float x = lineWidths[line] > 0 ? lineWidths[line] + pillGap : 0.0f;
            filePills[i][j] = {format, x, pillW, line};
            lineWidths[line] = x + pillW;
        }
        for (auto& pill : filePills[i]) {
            if (pill.label) pill.x += (cellW - lineWidths[pill.row]) * .5f;
        }
        const float height = fileIconSize + iconGap + (line + 1) * kFileFormatPillHeight + line * pillGap;
        auto& rowHeight = rowHeights[i / columns];
        rowHeight = std::max(rowHeight, height);
    }
    const float filesY = heroHeight + gap;
    const float filesGridY = filesY + padding;
    float filesHeight = padding * 2.0f + (rows - 1) * gap;
    for (int row = 0; row < rows; ++row) filesHeight += rowHeights[row];
    const float versionY = filesY + filesHeight + gap;

    struct RowLayout {
        float height, textW, labelH, buttonX, buttonY, buttonW;
    };
    const auto layoutRow = [&](const std::string& label, const std::string& value, const std::string& action) {
        const float actionW = buttonWidth(action);
        const float labelW = std::max(textWidth(label, hintSize), textWidth(value, labelSize + 4.0f, 600));
        const bool stacked = inner < actionW + labelW + gap;
        const float available = stacked ? inner : inner - actionW - gap;
        const float labelH = textHeight(label, hintSize, available);
        const float textH = labelH + 4.0f + textHeight(value, labelSize + 4.0f, available, 600);
        const float contentH = stacked ? textH + gap + buttonHeight : std::max(textH, buttonHeight);
        return RowLayout{padding * 2.0f + contentH, available, labelH,
                         stacked ? padding : width - padding - actionW,
                         padding + (stacked ? textH + gap : (contentH - buttonHeight) * .5f), actionW};
    };
    const auto drawRow = [&](const std::string& id, float y, const std::string& label, const std::string& value,
                             const std::string& action, const RowLayout& layout,
                             std::function<void()> callback) {
        card(id + ".card", y, layout.height);
        text(id + ".label", label, padding, y + padding, layout.textW, hintSize, colors.textMuted);
        text(id + ".value", value, padding, y + padding + layout.labelH + 4.0f, layout.textW,
             labelSize + 4.0f, colors.text, 600);
        button(id + ".action", action, layout.buttonX, y + layout.buttonY, layout.buttonW, std::move(callback));
    };

    const std::string versionLabel = i18n::tr("about.current_version");
    const std::string updateLabel = i18n::tr(settings::current().automaticUpdateCheck
        ? "about.update_on" : "about.update_off");
    const auto versionLayout = layoutRow(versionLabel, version::kVersion, updateLabel);
    const std::string licenseLabel = i18n::tr("about.licenses_title");
    const std::string licenseAction = i18n::tr("about.licenses_button");
    const auto licenseLayout = layoutRow(licenseLabel, "Apache-2.0", licenseAction);
    const float licenseY = versionY + versionLayout.height + gap;

    const std::string techTitle = i18n::tr("about.tech_title");
    const std::string frameworkAction = std::string(i18n::tr("about.framework_link")) + "  ↗";
    const float frameworkButtonW = buttonWidth(frameworkAction);
    const bool frameworkButtonBelow = inner < textWidth(techTitle, labelSize + 4.0f, 600) + gap + frameworkButtonW;
    const float techTitleW = frameworkButtonBelow ? inner : inner - frameworkButtonW - gap;
    const float techTitleH = textHeight(techTitle, labelSize + 4.0f, techTitleW, 600);
    const float techHeaderH = frameworkButtonBelow ? techTitleH + gap + buttonHeight : std::max(techTitleH, buttonHeight);
    const float techY = licenseY + licenseLayout.height + gap;
    const float techBodyY = techY + padding + techHeaderH + gap;
    struct TechSection { const char* title; const char* body; };
    const std::array<TechSection, 3> techSections{{
        {i18n::tr("about.tech_native"), i18n::tr("about.tech_native_body")},
        {i18n::tr("about.tech_render"), i18n::tr("about.tech_render_body")},
        {i18n::tr("about.tech_markdown"), i18n::tr("about.tech_markdown_body")}
    }};
    float techBottom = techBodyY;
    for (const auto& section : techSections) {
        techBottom += textHeight(section.title, labelSize, inner, 600) + 3.0f
            + textHeight(section.body, labelSize, inner) + gap;
    }
    const float techHeight = techBottom - gap + padding - techY;
    const float contentHeight = techY + techHeight + gap;
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
                    const auto logo = aboutIconPath(logoSize * effectiveWindowScale());
                    if (!logo.empty()) {
                        ui.image("settings.about.icon").position(padding, logoY).size(logoSize, logoSize)
                            .source(logo).contain().build();
                    } else {
                        ui.svg("settings.about.icon").position(padding, logoY).size(logoSize, logoSize)
                            .source(application_icon::kSvg).build();
                    }
                    const float brandX = padding + logoSpace;
                    text("settings.about.brand", "EUI-Edits", brandX, brandY, brandWidth, brandSize, colors.text, 600);
                    text("settings.about.slogan", slogan, brandX, brandY + nameHeight + 2.0f,
                         brandWidth, labelSize, colors.accent, 600);
                    button("settings.about.repository", repositoryLabel, repositoryBelow ? padding : width - padding - repositoryW,
                           repositoryY, repositoryW, [openLink] { openLink(repository); }, true);

                    card("settings.about.files.card", filesY, filesHeight);
                    float rowY = filesGridY;
                    for (int row = 0; row < rows; ++row) {
                        for (int col = 0; col < columns; ++col) {
                            const int i = row * columns + col;
                            const auto& kind = fileKinds[i];
                            const float x = padding + col * cellW;
                            const std::string id = "settings.about.files." + std::to_string(i);
                            iconView(ui, id + ".icon", kind.icon, x + (cellW - fileIconSize) * .5f,
                                     rowY, fileIconSize, kind.color);
                            for (std::size_t j = 0; j < filePills[i].size(); ++j) {
                                const auto& pill = filePills[i][j];
                                if (!pill.label) break;
                                const float pillY = rowY + fileIconSize + iconGap
                                    + pill.row * (kFileFormatPillHeight + pillGap);
                                fileFormatPill(ui, id + ".format." + std::to_string(j), pill.label,
                                               x + pill.x, pillY, pill.width, colors, kind.color, font, hintSize);
                            }
                        }
                        rowY += rowHeights[row] + gap;
                    }

                    drawRow("settings.about.version", versionY, versionLabel, version::kVersion, updateLabel,
                            versionLayout, [&state] {
                                auto& enabled = settings::current().automaticUpdateCheck;
                                enabled = !enabled;
                                if (!settings::flush()) {
                                    enabled = !enabled;
                                    showToast(state, i18n::tr("about.update_setting_failed"), {});
                                }
                                app::requestUpdate();
                            });
                    drawRow("settings.about.licenses", licenseY, licenseLabel, "Apache-2.0", licenseAction,
                            licenseLayout, [] { core::platform::showEuiEditsLicenseExportDialog(); });

                    card("settings.about.tech.card", techY, techHeight);
                    const float techHeaderY = techY + padding;
                    const float techTitleY = techHeaderY + (frameworkButtonBelow ? 0.0f : (techHeaderH - techTitleH) * .5f);
                    const float frameworkButtonY = techHeaderY + (frameworkButtonBelow
                        ? techTitleH + gap : (techHeaderH - buttonHeight) * .5f);
                    text("settings.about.tech.title", techTitle, padding, techTitleY,
                         techTitleW, labelSize + 4.0f, colors.text, 600);
                    button("settings.about.tech.framework", frameworkAction,
                           frameworkButtonBelow ? padding : width - padding - frameworkButtonW,
                           frameworkButtonY, frameworkButtonW, [openLink] { openLink(frameworkRepository); }, true);
                    float sectionY = techBodyY;
                    for (std::size_t i = 0; i < techSections.size(); ++i) {
                        const auto& section = techSections[i];
                        const std::string id = "settings.about.tech." + std::to_string(i);
                        text(id + ".title", section.title, padding, sectionY, inner, labelSize, colors.text, 600);
                        const float bodyY = sectionY + textHeight(section.title, labelSize, inner, 600) + 3.0f;
                        text(id + ".body", section.body, padding, bodyY, inner, labelSize, colors.textMuted);
                        sectionY = bodyY + textHeight(section.body, labelSize, inner) + gap;
                    }
                }).build();
        }).build();
    if (maxOffset > 0.0f) {
        components::scroll(ui, "settings.about.scrollbar").theme(colors.tokens).scrollStateId(scrollId)
            .position(width + 2.0f * inset - 12.0f, scrollTop).size(8, scrollHeight).viewport(scrollHeight)
            .content(contentHeight).offset(state.settingsScroll).step(step).build();
    }
}

} // namespace neo::settings_detail
