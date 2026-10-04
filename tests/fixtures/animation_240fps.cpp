#include "eui_neo.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace app {
namespace {

struct AnimationTestState {
    int frame = 0;
    float elapsed = 0.0f;
};

AnimationTestState& state() {
    static AnimationTestState value;
    return value;
}

eui::Color colorForIndex(int index, float time, float alpha = 1.0f) {
    const float shift = std::sin(time * 0.73f + static_cast<float>(index) * 0.11f) * 0.18f;
    const float hue = static_cast<float>((index * 37) % 360) / 360.0f + shift;
    return {
        std::clamp(0.48f + 0.32f * std::sin((hue + 0.00f) * 6.2831853f), 0.12f, 0.94f),
        std::clamp(0.50f + 0.30f * std::sin((hue + 0.33f) * 6.2831853f), 0.12f, 0.94f),
        std::clamp(0.54f + 0.28f * std::sin((hue + 0.66f) * 6.2831853f), 0.12f, 0.94f),
        alpha
    };
}

void drawAnimatedRects(eui::Ui& ui, float width, float height, const AnimationTestState& test) {
    const float cellWidth = width / 12.0f;
    const float cellHeight = height / 8.0f;
    for (int i = 0; i < 96; ++i) {
        const int column = i % 12;
        const int row = i / 12;
        const float wave = std::sin(test.elapsed * 3.7f + static_cast<float>(i) * 0.29f);
        const float waveB = std::cos(test.elapsed * 2.9f + static_cast<float>(i) * 0.41f);
        const float waveC = std::sin(test.elapsed * 1.9f + static_cast<float>(i) * 0.17f);
        const float boxWidth = 42.0f + static_cast<float>((i % 5) * 5) + wave * 10.0f;
        const float boxHeight = 34.0f + static_cast<float>((i % 4) * 4) + waveB * 8.0f;
        const float x = static_cast<float>(column) * cellWidth + cellWidth * 0.5f - boxWidth * 0.5f +
                        waveC * (14.0f + static_cast<float>(i % 3) * 5.0f);
        const float y = static_cast<float>(row) * cellHeight + cellHeight * 0.5f - boxHeight * 0.5f +
                        waveB * (10.0f + static_cast<float>(i % 4) * 3.0f);

        ui.rect("anim.rect." + std::to_string(i))
            .x(x)
            .y(y)
            .size(boxWidth, boxHeight)
            .color(colorForIndex(i, test.elapsed, 0.74f))
            .radius(12.0f + 10.0f * (0.5f + 0.5f * waveC) + static_cast<float>(i % 4))
            .border(1.0f, {0.92f, 0.96f, 1.0f, 0.20f + 0.10f * (0.5f + 0.5f * wave)})
            .shadow(10.0f + 10.0f * (0.5f + 0.5f * waveB),
                    0.0f,
                    4.0f + 6.0f * (0.5f + 0.5f * waveC),
                    {0.0f, 0.0f, 0.0f, 0.15f + 0.10f * (0.5f + 0.5f * wave)})
            .translate(wave * (18.0f + static_cast<float>(i % 4) * 4.0f),
                       waveB * (14.0f + static_cast<float>(i % 5) * 3.0f))
            .scale(1.0f + wave * 0.16f, 1.0f + waveB * 0.12f)
            .rotate(wave * 0.52f)
            .rotateX(waveB * 0.22f)
            .rotateY(waveC * 0.26f)
            .perspective(560.0f)
            .transformOrigin(0.5f, 0.5f)
            .opacity(0.78f + 0.16f * (0.5f + 0.5f * wave))
            .build();
    }
}

void drawAnimatedText(eui::Ui& ui, float width, const AnimationTestState& test) {
    ui.text("animation.title")
        .x(28.0f)
        .y(20.0f)
        .size(std::max(0.0f, width - 56.0f), 34.0f)
        .text("EUI DSL animation test 240 FPS")
        .fontSize(25.0f + std::sin(test.elapsed * 1.6f) * 1.2f)
        .lineHeight(32.0f)
        .color(colorForIndex(240, test.elapsed, 1.0f))
        .translate(std::sin(test.elapsed * 1.8f) * 18.0f, std::cos(test.elapsed * 1.4f) * 5.0f)
        .scale(1.0f + std::sin(test.elapsed * 1.2f) * 0.035f)
        .build();

    for (int i = 0; i < 10; ++i) {
        ui.text("animation.row." + std::to_string(i))
            .x(30.0f + static_cast<float>(i % 2) * 310.0f)
            .y(70.0f + static_cast<float>(i / 2) * 25.0f)
            .size(280.0f, 22.0f)
            .text("animated row " + std::to_string(i) + " frame " + std::to_string(test.frame))
            .fontSize(15.0f + std::sin(test.elapsed * 1.3f + static_cast<float>(i)) * 1.2f)
            .lineHeight(21.0f)
            .color(colorForIndex(i + 130, test.elapsed, 0.90f))
            .translate(std::sin(test.elapsed * 2.1f + static_cast<float>(i)) * 12.0f,
                       std::cos(test.elapsed * 1.7f + static_cast<float>(i) * 0.5f) * 4.0f)
            .rotate(std::sin(test.elapsed * 1.5f + static_cast<float>(i)) * 0.06f)
            .build();
    }
}

} // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("EUI 240 FPS Animation Test")
        .pageId("animation-240fps")
        .clearColor({0.045f, 0.052f, 0.064f, 1.0f})
        .windowSize(1280, 720)
        .showDebugStatsInTitle(true)
        .fps(240.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    AnimationTestState& test = state();
    ui.stack("frame-driver")
        .size(screen.width, screen.height)
        .onFrame([&](float deltaSeconds) {
            ++test.frame;
            test.elapsed += std::max(0.0f, deltaSeconds);
        })
        .build();

    ui.rect("background")
        .size(screen.width, screen.height)
        .gradient({0.045f, 0.052f, 0.064f, 1.0f},
                  {0.090f, 0.100f, 0.120f, 1.0f},
                  eui::GradientDirection::Vertical)
        .build();

    drawAnimatedRects(ui, screen.width, screen.height, test);
    drawAnimatedText(ui, screen.width, test);
}

} // namespace app
