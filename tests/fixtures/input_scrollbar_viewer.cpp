#include "eui_neo.h"

namespace app {
namespace {
std::string sample(int count = 2000) {
    std::string text;
    for (int i = 1; i <= count; ++i)
        text += std::to_string(i) + "  |  Editable text - scroll, drag, select and type.\n";
    return text;
}
std::string editor = sample();
std::string comparison = sample();
bool showBar = true;
}

const DslAppConfig& dslAppConfig() {
    static const auto config = DslAppConfig{}.title("Input scrollbar + HEX colors")
        .windowSize(1100, 720).clearColor("#0F172A").showDebugStatsInTitle(false);
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    const float width = std::max(120.f, (screen.width - 72.f) * 0.5f);
    const float height = std::max(60.f, screen.height - 230.f);
    ui.text("title").position(24, 20).size(900, 40).fontSize(28).color("#F8FAFC")
        .text("Text editor / HEX colors").build();
    ui.text("hint").position(24, 66).size(1000, 30).fontSize(15).color("#94A3B8")
        .text("2,000 editable lines. Try wheel, thumb drag, track click, selection, typing and resize.").build();
    components::button(ui, "toggle").position(24, 110).size(240, 38)
        .text(showBar ? "Scrollbar: ON (click to hide)" : "Scrollbar: OFF (click to show)")
        .colors("#2563EB", "#3B82F6", "#1D4ED8").onClick([] { showBar = !showBar; }).build();
    components::button(ui, "short").position(280, 110).size(160, 38).text("Short text")
        .onClick([] { editor = "Short text: scrollbar hides automatically."; }).build();
    components::button(ui, "restore").position(456, 110).size(170, 38).text("Restore 2,000 lines")
        .onClick([] { editor = sample(); }).build();
    components::button(ui, "large").position(642, 110).size(160, 38).text("Load 20,000 lines")
        .onClick([] { editor = sample(20000); }).build();
    ui.text("left.label").position(24, 164).size(width, 26).fontSize(15).color("#38BDF8")
        .text("Configurable scrollbar").build();
    ui.text("right.label").position(48 + width, 164).size(width, 26).fontSize(15).color("#A78BFA")
        .text("Default: scrollbar hidden (wheel still works)").build();
    components::InputStyle style;
    style.background = "#1E293B";
    style.focused = "#1E293B";
    style.text = "#E2E8F0";
    style.border = "#475569";
    style.focusBorder = "#38BDF8";
    components::input(ui, "editor").position(24, 196).size(width, height).fontSize(16)
        .multiline().scrollbar(showBar).style(style).value(editor)
        .onChange([](const std::string& value) { editor = value; }).build();
    components::input(ui, "comparison").position(48 + width, 196).size(width, height).fontSize(16)
        .multiline().style(style).value(comparison)
        .onChange([](const std::string& value) { comparison = value; }).build();
}
}
