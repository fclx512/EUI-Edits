#include "eui_neo.h"

#include <algorithm>
#include <string>

namespace app {
namespace {

constexpr float kPanelPadding = 24.0f;
constexpr float kTitleY = 20.0f;
constexpr float kTitleHeight = 34.0f;
constexpr float kRowsTop = 78.0f;
constexpr float kRowHeight = 52.0f;
constexpr float kRowGap = 16.0f;
constexpr float kLabelWidth = 150.0f;
constexpr float kLabelGap = 24.0f;
constexpr int kRowCount = 5;

void alignmentRow(eui::Ui& ui,
                  const std::string& id,
                  const std::string& label,
                  const std::string& text,
                  float y,
                  float width,
                  float fontSize,
                  const std::string& fontFamily = {}) {
    const float boxWidth = std::max(220.0f, width - kLabelWidth - kLabelGap);

    ui.text(id + ".label")
        .position(0.0f, y)
        .size(kLabelWidth, kRowHeight)
        .text(label)
        .fontSize(14.0f)
        .lineHeight(20.0f)
        .color({0.72f, 0.76f, 0.82f, 1.0f})
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    ui.stack(id + ".box")
        .position(kLabelWidth + kLabelGap, y)
        .size(boxWidth, kRowHeight)
        .content([&] {
            ui.rect(id + ".bg")
                .size(boxWidth, kRowHeight)
                .color({0.13f, 0.16f, 0.21f, 1.0f})
                .radius(8.0f)
                .border(1.0f, {0.32f, 0.38f, 0.48f, 1.0f})
                .build();

            ui.rect(id + ".center")
                .y(kRowHeight * 0.5f)
                .size(boxWidth, 1.0f)
                .color({0.30f, 0.54f, 0.95f, 0.55f})
                .build();

            auto textBuilder = ui.text(id + ".text")
                .size(boxWidth, kRowHeight)
                .text(text)
                .fontSize(fontSize)
                .lineHeight(fontSize + 8.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color({0.93f, 0.96f, 1.0f, 1.0f});
            if (!fontFamily.empty()) {
                textBuilder.fontFamily(fontFamily);
            }
            textBuilder.build();
        })
        .build();
}

} // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("Text Alignment Viewer")
        .pageId("text_alignment_viewer")
        .clearColor({0.07f, 0.08f, 0.10f, 1.0f})
        .windowSize(860, 500)
        .showDebugStatsInTitle(false)
        .fps(0.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    const float panelWidth = std::min(760.0f, std::max(360.0f, screen.width - 64.0f));
    const float contentHeight = kRowsTop + static_cast<float>(kRowCount) * kRowHeight +
        static_cast<float>(kRowCount - 1) * kRowGap;
    const float panelHeight = contentHeight + kPanelPadding * 2.0f;
    const float panelX = std::max(0.0f, (screen.width - panelWidth) * 0.5f);
    const float panelY = std::max(0.0f, (screen.height - panelHeight) * 0.5f);

    ui.rect("background")
        .size(screen.width, screen.height)
        .gradient({0.07f, 0.08f, 0.10f, 1.0f}, {0.10f, 0.11f, 0.15f, 1.0f}, eui::GradientDirection::Vertical)
        .build();

    components::card(ui, "panel")
        .position(panelX, panelY)
        .size(panelWidth, panelHeight)
        .padding(kPanelPadding)
        .color({0.10f, 0.12f, 0.16f, 1.0f})
        .radius(12.0f)
        .border(1.0f, {0.24f, 0.28f, 0.36f, 1.0f})
        .content([&] {
            ui.text("title")
                .position(0.0f, kTitleY)
                .size(panelWidth - 48.0f, 34.0f)
                .text("Text Vertical Alignment")
                .fontSize(24.0f)
                .lineHeight(30.0f)
                .fontWeight(700)
                .verticalAlign(eui::VerticalAlign::Center)
                .color({0.94f, 0.96f, 1.0f, 1.0f})
                .build();

            const float rowWidth = panelWidth - 48.0f;
            const float rowStep = kRowHeight + kRowGap;
            ui.stack("rows")
                .size(rowWidth, contentHeight)
                .content([&] {
                    alignmentRow(ui, "latin", "Latin", "OpenAI Codex", kRowsTop, rowWidth, 18.0f);
                    alignmentRow(ui, "cjk", "CJK", "中文文本居中", kRowsTop + rowStep, rowWidth, 18.0f);
                    alignmentRow(ui, "emoji", "Emoji", "Hello EUI-NEO :)", kRowsTop + rowStep * 2.0f, rowWidth, 18.0f);
                    alignmentRow(ui, "mono", "Monospace", "inline_code();", kRowsTop + rowStep * 3.0f, rowWidth, 17.0f, "monospace");
                    alignmentRow(ui, "icon", "Icon Font", "\xEF\x80\x95", kRowsTop + rowStep * 4.0f, rowWidth, 24.0f, "FontAwesome");
                })
                .build();
        })
        .build();
}

} // namespace app
