#pragma once

#include "model/i18n.h"

#include "model/lp_plan.h"
#include "state/app_state.h"
#include "ui/metrics.h"
#include "ui/widgets.h"
#include "ui/icons.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace neo {

// 只投影编辑器已经生成的 Live Preview 计划，不另跑一次 Markdown 解析。
// conceal 是同一计划里的可见性信息；ATX 前缀、闭合 # 与行内标记都由它去除。
inline std::string outlineTitle(const std::string& text, const LpLine& line) {
    const int beg = std::clamp(line.srcBeg, 0, static_cast<int>(text.size()));
    const int end = std::clamp(line.srcEnd, beg, static_cast<int>(text.size()));
    std::string result;
    int cursor = beg;
    for (const LpRange& hidden : line.conceal) {
        const int hiddenBeg = std::clamp(hidden.beg, beg, end);
        const int hiddenEnd = std::clamp(hidden.end, hiddenBeg, end);
        if (hiddenBeg > cursor) {
            result.append(text, static_cast<std::size_t>(cursor),
                          static_cast<std::size_t>(hiddenBeg - cursor));
        }
        cursor = std::max(cursor, hiddenEnd);
    }
    if (cursor < end) {
        result.append(text, static_cast<std::size_t>(cursor),
                      static_cast<std::size_t>(end - cursor));
    }
    const auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
    const auto first = std::find_if(result.begin(), result.end(), nonSpace);
    const auto last = std::find_if(result.rbegin(), result.rend(), nonSpace).base();
    if (first >= last) {
        return i18n::tr("outline.empty_heading");
    }
    return std::string(first, last);
}

inline std::vector<OutlineEntry> outlineFromPlan(const std::string& text, const LpPlan& plan) {
    std::vector<OutlineEntry> entries;
    for (const LpLine& line : plan.lines) {
        // setext 下划线也被标记为 Heading，但它不是第二个标题，且 sectionEndLine=-1。
        if (line.kind != LpKind::Heading || line.headingLevel < 1 ||
            line.headingLevel > 6 || line.sectionEndLine < 0) {
            continue;
        }
        entries.push_back({outlineTitle(text, line), line.headingLevel, line.srcBeg, line.number});
    }
    return entries;
}

inline void vaultTabsView(eui::Ui& ui, AppState& state, float inner, float fontSize) {
    const float tabsHeight = fontSize + 20.0f;
    const float half = std::max(0.0f, (inner - 6.0f) * 0.5f);
    ui.stack("vault.tabs").size(inner, tabsHeight).content([&] {
        ui.rect("vault.tabs.surface").fill().radius(7).color(core::mixColor(editorColors().panel, editorColors().border, .24f)).build();
        for (int i = 0; i < 2; ++i) {
            const bool active = state.vaultTab == (i == 0 ? VaultTab::Files : VaultTab::Outline);
            const std::string id = i == 0 ? "vault.tab.files" : "vault.tab.outline";
            const float x = 3.0f + i * half;
            ui.rect(id + ".hit").position(x, 3).size(half, tabsHeight-6).radius(5)
                .states(active ? editorColors().editor : transparentColor(), editorColors().rowHover,
                        editorColors().pressed)
                .border(active ? 1.0f : 0.0f, editorColors().border)
                .preserveFocusOnPress().cursor(eui::CursorShape::Hand)
                .onClick([&state, i] {
                    state.vaultTab = i == 0 ? VaultTab::Files : VaultTab::Outline;
                    app::requestUpdate();
                }).build();
            const float labelWidth = fontSize*2;
            const float labelX = x + (half-labelWidth-23)*.5f;
            iconView(ui, id + ".icon", i == 0 ? UiIcon::Folder : UiIcon::Outline,
                     labelX, (tabsHeight-16)*.5f, 16, active ? editorColors().accent : editorColors().textMuted);
            ui.text(id + ".label").position(labelX+23, 0).size(labelWidth, tabsHeight)
                .text(i == 0 ? i18n::tr("outline.files") : i18n::tr("outline.title")).fontFamily(uiFontFamily(state)).fontSize(fontSize)
                .color(active ? editorColors().text : editorColors().textMuted)
                .verticalAlign(eui::VerticalAlign::Center).build();
        }
    }).build();
}

inline void outlinePanelView(eui::Ui& ui, AppState& state, float width, float height) {
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);
    constexpr float padding = 10.0f;
    constexpr float gap = 6.0f;
    const float inner = std::max(0.0f, width - padding * 2.0f);
    const float rowHeight = metrics.vaultRowHeight + 8.0f;
    const float tabsHeight = metrics.vaultTitleFontSize + 20.0f;
    const float listHeight = std::max(0.0f, height - 16.0f - tabsHeight - 45.0f - gap * 2.0f);

    ui.stack("vault")
        .size(width, height)
        .content([&] {
            ui.rect("vault.bg").fill().color(colors.panel).build();
            ui.column("vault.content")
                .fill()
                .padding(padding, 8.0f)
                .gap(gap)
                .zIndex(1)
                .content([&] {
                    vaultTabsView(ui, state, inner, metrics.vaultTitleFontSize);
                    ui.stack("outline.document.card").size(inner, 45).content([&] {
                        ui.rect("outline.document.bg").fill().radius(6).color(colors.editor)
                            .border(1, colors.border).build();
                        iconView(ui, "outline.document.icon", UiIcon::MarkdownFile, 8, 13, 18, colors.iconMd);
                        ui.text("outline.document").position(33, 3).size(std::max(0.0f, inner-41), 22)
                            .text(elideToWidth(state.displayName(), inner-41, metrics.vaultTitleFontSize+2.0f))
                            .fontFamily(uiFontFamily(state)).fontSize(metrics.vaultTitleFontSize+2.0f).fontWeight(600)
                            .color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
                        ui.text("outline.summary").position(33, 25).size(std::max(0.0f, inner-41), 16)
                            .text(i18n::format("outline.count", {{"count", std::to_string(state.outline.size())}}))
                            .fontFamily(uiFontFamily(state)).fontSize(metrics.vaultPathFontSize)
                            .color(colors.textMuted).build();
                    }).build();

                    if (!state.markdownCapable()) {
                        ui.text("outline.empty")
                            .size(inner, 44.0f)
                            .text(i18n::tr("outline.plain_text"))
                            .fontFamily(uiFontFamily(state))
                            .fontSize(metrics.vaultFilterFontSize)
                            .color(colors.textMuted)
                            .wrap(true)
                            .build();
                        return;
                    }
                    if (state.outline.empty()) {
                        ui.text("outline.empty")
                            .size(inner, 44.0f)
                            .text(i18n::tr("outline.no_headings"))
                            .fontFamily(uiFontFamily(state))
                            .fontSize(metrics.vaultFilterFontSize)
                            .color(colors.textMuted)
                            .wrap(true)
                            .build();
                        return;
                    }

                    components::virtualList(ui, "outline.list")
                        .theme(colors.tokens)
                        .size(inner, listHeight)
                        .itemCount(static_cast<std::int64_t>(state.outline.size()))
                        .rowHeight(rowHeight)
                        .offset(state.outlineScroll)
                        .step(78.0f)
                        .overscanViewports(0.5f)
                        .transition(quickTransition())
                        .onChange([&state](float value) { state.outlineScroll = value; })
                        .row([&state, &colors, &metrics, rowHeight](eui::Ui& rowUi,
                             const std::string& rowId, std::int64_t index, float rowWidth, float) {
                            const std::size_t i = static_cast<std::size_t>(index);
                            if (i >= state.outline.size()) {
                                return;
                            }
                            const OutlineEntry entry = state.outline[i];
                            const float indent = 7.0f + static_cast<float>(entry.level - 1) *
                                std::min(8.0f, metrics.vaultIndentPerDepth);
                            const bool active = entry.byteOffset == state.outlineCurrentByte;
                            const float badgeWidth = metrics.vaultRowFontSize * 1.65f + 5.0f;
                            const float labelX = indent + badgeWidth + 7.0f;
                            const float labelWidth = std::max(0.0f, rowWidth - labelX - 7.0f);
                            rowUi.stack(rowId)
                                .size(rowWidth, rowHeight)
                                .content([&] {
                                    rowUi.rect(rowId + ".bg")
                                        .position(0, 2).size(rowWidth, rowHeight-4)
                                        .radius(5.0f)
                                        .states(active ? core::mixColor(colors.panel, colors.accent, .13f)
                                                      : entry.level == 1 ? core::mixColor(colors.panel, colors.border, .18f)
                                                                         : transparentColor(),
                                                colors.rowHover, colors.pressed)
                                        .transition(quickTransition())
                                        .cursor(eui::CursorShape::Hand)
                                        .preserveFocusOnPress()
                                        .onClick([&state, entry] {
                                            state.pendingOutlineJumpByte = entry.byteOffset;
                                            app::requestUpdate();
                                        })
                                        .build();
                                    if (active) {
                                        rowUi.rect(rowId + ".current").position(0, 7)
                                            .size(2, rowHeight-14).radius(1).color(colors.accent).build();
                                    }
                                    rowUi.rect(rowId + ".badge.bg").position(indent, (rowHeight-20)*.5f)
                                        .size(badgeWidth, 20).radius(4)
                                        .color(core::mixColor(colors.panel,
                                            active ? colors.accent : colors.textMuted, entry.level <= 2 ? .14f : .07f))
                                        .build();
                                    rowUi.text(rowId + ".badge").position(indent, 0).size(badgeWidth, rowHeight)
                                        .text("H" + std::to_string(entry.level)).fontFamily(uiFontFamily(state))
                                        .fontSize(metrics.vaultPathFontSize).fontWeight(600)
                                        .color(active ? colors.accent : colors.textMuted)
                                        .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
                                    rowUi.text(rowId + ".label")
                                        .position(labelX, 0.0f)
                                        .size(labelWidth, rowHeight)
                                        .text(elideToWidth(entry.title, labelWidth,
                                                           metrics.vaultRowFontSize))
                                        .fontFamily(uiFontFamily(state))
                                        .fontSize(metrics.vaultRowFontSize)
                                        .fontWeight(entry.level <= 2 ? 600 : 400)
                                        .color(active ? colors.accent : colors.text)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .build();
                                })
                                .build();
                        })
                        .build();
                })
                .build();
        })
        .build();
}

} // namespace neo
