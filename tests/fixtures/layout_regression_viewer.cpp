#include "eui_neo.h"

#include <algorithm>
#include <string>

namespace app {
namespace {

struct LayoutRegressionState {
    float scrollOffset = 0.0f;
};

LayoutRegressionState& state() {
    static LayoutRegressionState value;
    return value;
}

constexpr float kCardWidth = 320.0f;
constexpr float kCardHeight = 180.0f;
constexpr float kCardInset = 18.0f;
constexpr float kGap = 18.0f;
constexpr float kLabelHeight = 22.0f;
constexpr float kWrapRowHeight = 42.0f;
constexpr float kWrapRowGap = 10.0f;
constexpr float kWrapContentHeight = kLabelHeight + kWrapRowHeight * 3.0f + kWrapRowGap * 3.0f;
constexpr float kWrapCardHeight = kWrapContentHeight + kCardInset * 2.0f;

void label(eui::Ui& ui, const std::string& id, const std::string& text, float y, float width) {
    ui.text(id)
        .position(0.0f, y)
        .size(width, 22.0f)
        .text(text)
        .fontSize(15.0f)
        .lineHeight(20.0f)
        .fontWeight(700)
        .color({0.91f, 0.94f, 1.0f, 1.0f})
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

void sampleBlock(eui::Ui& ui,
                 const std::string& id,
                 float x,
                 float y,
                 float width,
                 float height,
                 const eui::Color& color,
                 const std::string& text) {
    ui.stack(id)
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, height)
                .color(color)
                .radius(7.0f)
                .build();
            ui.text(id + ".text")
                .size(width, height)
                .text(text)
                .fontSize(13.0f)
                .lineHeight(18.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color({0.95f, 0.97f, 1.0f, 1.0f})
                .build();
        })
        .build();
}

void paddingCase(eui::Ui& ui, const std::string& id, float x, float y) {
    components::card(ui, id)
        .position(x, y)
        .size(kCardWidth, kCardHeight)
        .padding(kCardInset)
        .color({0.11f, 0.13f, 0.18f, 1.0f})
        .border(1.0f, {0.30f, 0.36f, 0.46f, 1.0f})
        .content([&] {
            label(ui, id + ".title", "padding / content inset", 0.0f, kCardWidth - kCardInset * 2.0f);
            sampleBlock(ui, id + ".block", 0.0f, 42.0f, kCardWidth - kCardInset * 2.0f, 72.0f,
                        {0.20f, 0.31f, 0.52f, 1.0f}, "content starts inside inset");
        })
        .build();

    ui.stack(id + ".debug.anchor")
        .position(x, y)
        .size(kCardWidth, kCardHeight)
        .content([&] {
            components::layoutDebugOverlay(ui, id + ".debug", kCardWidth, kCardHeight, kCardInset, "frame / padding / content");
        })
        .build();
}

void wrapContentCase(eui::Ui& ui, const std::string& id, float x, float y) {
    components::card(ui, id)
        .position(x, y)
        .width(kCardWidth)
        .wrapContentHeight()
        .padding(kCardInset)
        .color({0.11f, 0.13f, 0.18f, 1.0f})
        .border(1.0f, {0.30f, 0.36f, 0.46f, 1.0f})
        .content([&] {
            ui.column(id + ".content.column")
                .width(kCardWidth - kCardInset * 2.0f)
                .height(eui::SizeValue::wrapContent())
                .gap(kWrapRowGap)
                .content([&] {
                    label(ui, id + ".title", "wrapContentHeight", 0.0f, kCardWidth - kCardInset * 2.0f);
                    sampleBlock(ui, id + ".a", 0.0f, 0.0f, kCardWidth - kCardInset * 2.0f, kWrapRowHeight,
                                {0.35f, 0.28f, 0.52f, 1.0f}, "row A");
                    sampleBlock(ui, id + ".b", 0.0f, 0.0f, kCardWidth - kCardInset * 2.0f, kWrapRowHeight,
                                {0.28f, 0.42f, 0.40f, 1.0f}, "row B");
                    sampleBlock(ui, id + ".c", 0.0f, 0.0f, kCardWidth - kCardInset * 2.0f, kWrapRowHeight,
                                {0.48f, 0.32f, 0.32f, 1.0f}, "row C");
                })
                .build();
        })
        .build();
}

void fillCase(eui::Ui& ui, const std::string& id, float x, float y) {
    components::card(ui, id)
        .position(x, y)
        .size(kCardWidth, kCardHeight)
        .padding(kCardInset)
        .color({0.11f, 0.13f, 0.18f, 1.0f})
        .border(1.0f, {0.30f, 0.36f, 0.46f, 1.0f})
        .content([&] {
            label(ui, id + ".title", "fill child respects card content", 0.0f, kCardWidth - kCardInset * 2.0f);
            ui.stack(id + ".fill.target")
                .position(0.0f, 42.0f)
                .size(kCardWidth - kCardInset * 2.0f, 98.0f)
                .content([&] {
                    ui.rect(id + ".fill.bg")
                        .fill()
                        .color({0.17f, 0.33f, 0.26f, 1.0f})
                        .radius(7.0f)
                        .build();
                    ui.text(id + ".fill.text")
                        .fill()
                        .text("fill() stays inside target")
                        .fontSize(13.0f)
                        .lineHeight(18.0f)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color({0.95f, 0.97f, 1.0f, 1.0f})
                        .build();
                })
                .build();
        })
        .build();

    ui.stack(id + ".debug.anchor")
        .position(x, y)
        .size(kCardWidth, kCardHeight)
        .content([&] {
            components::layoutDebugOverlay(ui, id + ".debug", kCardWidth, kCardHeight, kCardInset, "fill bounds");
        })
        .build();
}

void scrollCase(eui::Ui& ui, const std::string& id, float x, float y) {
    LayoutRegressionState& s = state();
    const float viewportWidth = kCardWidth - kCardInset * 2.0f;
    const float viewportHeight = 112.0f;

    components::card(ui, id)
        .position(x, y)
        .size(kCardWidth, kCardHeight)
        .padding(kCardInset)
        .color({0.11f, 0.13f, 0.18f, 1.0f})
        .border(1.0f, {0.30f, 0.36f, 0.46f, 1.0f})
        .content([&] {
            label(ui, id + ".title", "scroll content height", 0.0f, viewportWidth);
            components::scrollView(ui, id + ".scroll")
                .size(viewportWidth, viewportHeight)
                .offset(s.scrollOffset)
                .gap(8.0f)
                .scrollbarWidth(7.0f)
                .scrollbarGap(10.0f)
                .onChange([&s](float value) {
                    s.scrollOffset = value;
                })
                .content([&](eui::Ui& contentUi, float contentWidth, float) {
                    for (int i = 0; i < 8; ++i) {
                        sampleBlock(contentUi,
                                    id + ".scroll.row." + std::to_string(i),
                                    0.0f,
                                    0.0f,
                                    contentWidth,
                                    30.0f,
                                    i % 2 == 0 ? eui::Color{0.24f, 0.32f, 0.48f, 1.0f} : eui::Color{0.30f, 0.24f, 0.42f, 1.0f},
                                    "scroll row " + std::to_string(i + 1));
                    }
                })
                .build();
        })
        .build();
}

void overlayCase(eui::Ui& ui, const std::string& id, float x, float y) {
    components::card(ui, id)
        .position(x, y)
        .size(kCardWidth, kCardHeight)
        .padding(kCardInset)
        .color({0.11f, 0.13f, 0.18f, 1.0f})
        .border(1.0f, {0.30f, 0.36f, 0.46f, 1.0f})
        .content([&] {
            label(ui, id + ".title", "overlay background ignores layout", 0.0f, kCardWidth - kCardInset * 2.0f);
            ui.column(id + ".lane")
                .position(0.0f, 42.0f)
                .size(kCardWidth - kCardInset * 2.0f, 98.0f)
                .padding(12.0f)
                .gap(8.0f)
                .justifyContent(eui::Align::CENTER)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    ui.rect(id + ".lane.bg")
                        .fill()
                        .ignoreLayout()
                        .color({0.18f, 0.22f, 0.30f, 1.0f})
                        .radius(7.0f)
                        .build();
                    sampleBlock(ui, id + ".lane.a", 0.0f, 0.0f, 132.0f, 28.0f,
                                {0.28f, 0.38f, 0.56f, 1.0f}, "center A");
                    sampleBlock(ui, id + ".lane.b", 0.0f, 0.0f, 132.0f, 28.0f,
                                {0.34f, 0.28f, 0.50f, 1.0f}, "center B");
                })
                .build();
        })
        .build();
}

void flexBoundsCase(eui::Ui& ui, const std::string& id, float x, float y) {
    components::card(ui, id)
        .position(x, y)
        .size(kCardWidth, kCardHeight)
        .padding(kCardInset)
        .color({0.11f, 0.13f, 0.18f, 1.0f})
        .border(1.0f, {0.30f, 0.36f, 0.46f, 1.0f})
        .content([&] {
            const float contentWidth = kCardWidth - kCardInset * 2.0f;
            label(ui, id + ".title", "flex stays inside fixed row", 0.0f, contentWidth);
            ui.row(id + ".row")
                .position(0.0f, 52.0f)
                .size(contentWidth, 54.0f)
                .gap(8.0f)
                .content([&] {
                    sampleBlock(ui, id + ".left", 0.0f, 0.0f, 54.0f, 44.0f,
                                {0.27f, 0.27f, 0.32f, 1.0f}, "L");
                    ui.stack(id + ".center")
                        .height(44.0f)
                        .flexGrow(1.0f)
                        .minWidth(72.0f)
                        .content([&] {
                            ui.rect(id + ".center.bg")
                                .fill()
                                .color({0.20f, 0.34f, 0.56f, 1.0f})
                                .radius(7.0f)
                                .build();
                            ui.text(id + ".center.text")
                                .fill()
                                .text("flex")
                                .fontSize(13.0f)
                                .lineHeight(18.0f)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color({0.95f, 0.97f, 1.0f, 1.0f})
                                .build();
                        })
                        .build();
                    sampleBlock(ui, id + ".right", 0.0f, 0.0f, 54.0f, 44.0f,
                                {0.34f, 0.28f, 0.42f, 1.0f}, "R");
                })
                .build();
        })
        .build();
}

} // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("Layout Regression Viewer")
        .pageId("layout_regression_viewer")
        .clearColor({0.07f, 0.08f, 0.10f, 1.0f})
        .windowSize(820, 620)
        .showDebugStatsInTitle(false)
        .fps(0.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    const float width = std::min(720.0f, std::max(360.0f, screen.width - 48.0f));
    const float left = std::max(0.0f, (screen.width - width) * 0.5f);
    const float top = 42.0f;

    ui.rect("background")
        .size(screen.width, screen.height)
        .gradient({0.07f, 0.08f, 0.10f, 1.0f}, {0.10f, 0.11f, 0.15f, 1.0f}, eui::GradientDirection::Vertical)
        .build();

    ui.text("title")
        .position(left, 12.0f)
        .size(width, 30.0f)
        .text("Layout Regression")
        .fontSize(24.0f)
        .lineHeight(30.0f)
        .fontWeight(700)
        .color({0.94f, 0.96f, 1.0f, 1.0f})
        .build();

    paddingCase(ui, "padding", left, top);
    wrapContentCase(ui, "wrap", left + kCardWidth + kGap, top);
    fillCase(ui, "fill", left, top + kCardHeight + kGap);
    scrollCase(ui, "scroll", left + kCardWidth + kGap, top + kWrapCardHeight + kGap);
    overlayCase(ui, "overlay", left, top + kCardHeight * 2.0f + kGap * 2.0f);
    flexBoundsCase(ui, "flex.bounds", left + kCardWidth + kGap, top + kCardHeight * 2.0f + kGap * 2.0f);
}

} // namespace app
