#include "eui_neo.h"

#include <algorithm>

namespace {

core::Color color(float r, float g, float b, float a = 1.0f) {
    return {r, g, b, a};
}

void composeButton(eui::Ui& ui,
                   const std::string& id,
                   float x,
                   float y,
                   float width,
                   bool shadow,
                   const std::string& title) {
    auto surface = ui.rect(id + ".surface")
        .x(x)
        .y(y)
        .width(width)
        .height(112.0f)
        .radius(22.0f)
        .states(color(0.20f, 0.42f, 0.88f), color(0.24f, 0.48f, 0.96f), color(0.16f, 0.34f, 0.76f))
        .transition(0.16f, eui::Ease::OutCubic);

    if (shadow) {
        surface.shadow(34.0f, 0.0f, 14.0f, color(0.0f, 0.0f, 0.0f, 0.36f));
    }

    ui.text(id + ".label")
        .x(x)
        .y(y)
        .width(width)
        .height(112.0f)
        .text(title)
        .fontSize(22.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(color(1.0f, 1.0f, 1.0f));
}

} // namespace

namespace app {

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("Shadow Button GPU Viewer")
        .pageId("shadow_button_gpu_viewer")
        .clearColor({0.09f, 0.10f, 0.12f, 1.0f})
        .windowSize(760, 460)
        .showDebugStatsInTitle(true)
        .fps(90.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    const float contentW = 640.0f;
    const float x = (screen.width - contentW) * 0.5f;
    const float y = std::max(48.0f, screen.height * 0.25f);
    const float gap = 32.0f;
    const float buttonW = (contentW - gap) * 0.5f;

    ui.rect("background")
        .x(0.0f)
        .y(0.0f)
        .width(screen.width)
        .height(screen.height)
        .color(color(0.09f, 0.10f, 0.12f));

    ui.text("title")
        .x(x)
        .y(y)
        .width(contentW)
        .height(34.0f)
        .text("Hover or press each button and compare GPU/Render ms")
        .fontSize(24.0f)
        .color(color(0.94f, 0.95f, 0.97f));

    ui.text("note")
        .x(x)
        .y(y + 42.0f)
        .width(contentW)
        .height(52.0f)
        .text("Left is a plain button. Right uses the same interaction animation plus shadow.")
        .fontSize(15.0f)
        .wrap(true)
        .color(color(0.68f, 0.72f, 0.78f));

    composeButton(ui, "plain", x, y + 128.0f, buttonW, false, "Plain");
    composeButton(ui, "shadow", x + buttonW + gap, y + 128.0f, buttonW, true, "Shadow");

    ui.text("labels")
        .x(x)
        .y(y + 266.0f)
        .width(contentW)
        .height(28.0f)
        .text("Move the pointer between the two buttons. Press and hold to compare press animation cost.")
        .fontSize(14.0f)
        .color(color(0.55f, 0.60f, 0.68f));
}

} // namespace app
