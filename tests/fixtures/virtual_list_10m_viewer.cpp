#include "eui_neo.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace app {
namespace {

constexpr std::int64_t kRowCount = 10000000;
constexpr float kRowHeight = 32.0f;

struct ViewerState {
    eui::Signal<float> scroll{0.0f};
};

ViewerState& state() {
    static ViewerState value;
    return value;
}

eui::Color color(float r, float g, float b, float a = 1.0f) {
    return {r, g, b, a};
}

std::uint32_t hashIndex(std::int64_t index, std::uint32_t salt) {
    std::uint32_t value = static_cast<std::uint32_t>(index) ^ (salt * 0x9E3779B9u);
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return value;
}

float channel(std::int64_t index, std::uint32_t salt, float low, float high) {
    const float t = static_cast<float>(hashIndex(index, salt) & 0xFFu) / 255.0f;
    return low + (high - low) * t;
}

eui::Color rowAccent(std::int64_t index) {
    return color(channel(index, 1u, 0.12f, 0.92f),
                 channel(index, 2u, 0.22f, 0.78f),
                 channel(index, 3u, 0.28f, 0.96f),
                 1.0f);
}

std::string rowNumber(std::int64_t index) {
    std::ostringstream stream;
    stream << "Row #" << std::setw(8) << std::setfill('0') << index;
    return stream.str();
}

void label(eui::Ui& ui,
           const std::string& id,
           float x,
           float y,
           float width,
           const std::string& text,
           float size,
           eui::Color textColor) {
    const float height = size + 12.0f;
    ui.text(id)
        .x(x)
        .y(y)
        .size(width, height)
        .text(text)
        .fontSize(size)
        .lineHeight(size + 4.0f)
        .color(textColor)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

void composeRow(eui::Ui& ui, const std::string& rowId, std::int64_t index, float width, float height) {
    const eui::Color accent = rowAccent(index);
    const bool even = (index % 2) == 0;
    const float mix = even ? 0.0f : 0.018f;
    const eui::Color bg = color(0.965f - mix, 0.975f - mix, 0.982f - mix, 1.0f);
    const eui::Color soft = color(accent.r, accent.g, accent.b, 0.12f);
    const std::string mainText = rowNumber(index) + "  color("
        + std::to_string(static_cast<int>(accent.r * 255.0f)) + ", "
        + std::to_string(static_cast<int>(accent.g * 255.0f)) + ", "
        + std::to_string(static_cast<int>(accent.b * 255.0f)) + ")";
    const std::string subText = "virtual item " + std::to_string(index % 9973)
        + " / bucket " + std::to_string((index * 37) % 4096);

    ui.rect(rowId + ".bg")
        .x(8.0f)
        .y(3.0f)
        .size(std::max(0.0f, width - 16.0f), height - 6.0f)
        .radius(8.0f)
        .color(bg)
        .border(1.0f, soft)
        .build();

    ui.rect(rowId + ".accent")
        .x(18.0f)
        .y(10.0f)
        .size(5.0f, height - 18.0f)
        .radius(3.0f)
        .color(accent)
        .build();

    ui.rect(rowId + ".chip")
        .x(std::max(28.0f, width - 150.0f))
        .y(6.0f)
        .size(118.0f, height - 12.0f)
        .radius(12.0f)
        .color(color(accent.r, accent.g, accent.b, 0.16f))
        .build();

    label(ui, rowId + ".text", 36.0f, 3.0f, std::max(0.0f, width - 210.0f), mainText, 14.0f, color(0.10f, 0.12f, 0.16f));
    label(ui, rowId + ".sub", std::max(38.0f, width - 140.0f), 4.0f, 96.0f, subText, 11.0f, color(accent.r, accent.g, accent.b, 0.82f));
}

} // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("Virtual List 10M Viewer")
        .pageId("virtual_list_10m_viewer")
        .clearColor(color(0.91f, 0.93f, 0.94f, 1.0f))
        .windowSize(1180, 820)
        .showDebugStatsInTitle(true)
        .fps(120.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ViewerState& viewer = state();
    const float margin = std::clamp(screen.width * 0.04f, 30.0f, 56.0f);
    const float top = 30.0f;
    const float listY = 136.0f;
    const float listW = std::max(520.0f, screen.width - margin * 2.0f);
    const float listH = std::max(260.0f, screen.height - listY - 34.0f);
    const float listX = (screen.width - listW) * 0.5f;
    const std::int64_t firstApprox = static_cast<std::int64_t>(std::max(0.0f, viewer.scroll.get()) / kRowHeight);

    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.rect("background")
                .size(screen.width, screen.height)
                .gradient(color(0.91f, 0.94f, 0.95f), color(0.97f, 0.96f, 0.91f), eui::GradientDirection::Vertical)
                .build();

            label(ui, "title", margin, top, listW, "Virtual List 10M Viewer", 30.0f, color(0.07f, 0.09f, 0.12f));
            label(ui,
                  "subtitle",
                  margin,
                  top + 46.0f,
                  listW,
                  "10,000,000 fixed-height rows. Text and color are generated from the row index; visible rows are overscanned.",
                  15.0f,
                  color(0.34f, 0.38f, 0.44f));
            label(ui,
                  "offset",
                  margin,
                  top + 72.0f,
                  listW,
                  "approx first visible row: " + std::to_string(firstApprox),
                  13.0f,
                  color(0.45f, 0.34f, 0.20f));

            components::virtualList(ui, "virtual.list")
                .position(listX, listY)
                .size(listW, listH)
                .itemCount(kRowCount)
                .rowHeight(kRowHeight)
                .bind(viewer.scroll)
                .step(kRowHeight * 2.0f)
                .overscanViewports(1.0f)
                .scrollbarWidth(10.0f)
                .scrollbarGap(14.0f)
                .transition(eui::Transition{})
                .row(composeRow)
                .build();
        })
        .build();
}

} // namespace app
