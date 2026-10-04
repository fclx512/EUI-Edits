#include "eui_neo.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>

namespace app {
namespace {

struct ViewerState {
    eui::Signal<float> scroll{0.0f};
    eui::Signal<float> density{0.58f};
    eui::Signal<bool> live{true};
    eui::Signal<bool> compact{false};
    eui::Signal<int> tab{1};
    int seed = 17;
};

ViewerState& state() {
    static ViewerState value;
    return value;
}

eui::Color rgba(float r, float g, float b, float a = 1.0f) {
    return {r, g, b, a};
}

float random01(int seed, int salt) {
    std::uint32_t value = static_cast<std::uint32_t>(seed * 747796405u + salt * 2891336453u + 1013904223u);
    value = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
    value = (value >> 22u) ^ value;
    return static_cast<float>(value & 0xFFFFu) / 65535.0f;
}

eui::Color palette(int index, float alpha = 1.0f) {
    static const eui::Color colors[] = {
        rgba(0.08f, 0.56f, 0.49f),
        rgba(0.22f, 0.42f, 0.90f),
        rgba(0.92f, 0.39f, 0.24f),
        rgba(0.76f, 0.51f, 0.13f),
        rgba(0.45f, 0.35f, 0.82f),
        rgba(0.12f, 0.62f, 0.78f)
    };
    eui::Color color = colors[index % 6];
    color.a = alpha;
    return color;
}

void label(eui::Ui& ui,
           const std::string& id,
           float x,
           float y,
           float width,
           const std::string& text,
           float size,
           eui::Color color = rgba(0.10f, 0.12f, 0.15f)) {
    ui.text(id)
        .x(x)
        .y(y)
        .size(width, size + 5.0f)
        .text(text)
        .fontSize(size)
        .lineHeight(size + 2.0f)
        .color(color)
        .build();
}

void pill(eui::Ui& ui, const std::string& id, float x, float y, float width, const std::string& text, eui::Color color) {
    ui.stack(id)
        .x(x)
        .y(y)
        .size(width, 28.0f)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, 28.0f)
                .radius(14.0f)
                .color(rgba(color.r, color.g, color.b, 0.16f))
                .border(1.0f, rgba(color.r, color.g, color.b, 0.34f))
                .build();
            ui.text(id + ".text")
                .x(10.0f)
                .y(6.0f)
                .size(std::max(0.0f, width - 20.0f), 14.0f)
                .text(text)
                .fontSize(13.0f)
                .lineHeight(13.0f)
                .color(color)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();
        })
        .build();
}

void scatterCards(eui::Ui& ui, const std::string& id, float width, float height, int seed, int count) {
    for (int i = 0; i < count; ++i) {
        const float w = 82.0f + random01(seed, i * 7 + 1) * 150.0f;
        const float h = 38.0f + random01(seed, i * 7 + 2) * 112.0f;
        const float x = random01(seed, i * 7 + 3) * std::max(1.0f, width - w);
        const float y = 46.0f + random01(seed, i * 7 + 4) * std::max(1.0f, height - h - 60.0f);
        const eui::Color accent = palette(i, 1.0f);
        ui.rect(id + ".scatter." + std::to_string(i))
            .x(x)
            .y(y)
            .size(w, h)
            .radius(8.0f + random01(seed, i * 7 + 5) * 18.0f)
            .color(rgba(1.0f, 1.0f, 1.0f, 0.64f))
            .border(1.0f, rgba(accent.r, accent.g, accent.b, 0.24f))
            .shadow(10.0f + random01(seed, i * 7 + 6) * 20.0f,
                    0.0f,
                    4.0f + random01(seed, i * 7 + 7) * 8.0f,
                    rgba(0.09f, 0.12f, 0.18f, 0.08f))
            .build();

        if ((i % 3) == 0) {
            label(ui,
                  id + ".scatter.label." + std::to_string(i),
                  x + 12.0f,
                  y + 12.0f,
                  std::max(0.0f, w - 24.0f),
                  "node " + std::to_string(i),
                  12.0f,
                  rgba(0.24f, 0.27f, 0.32f, 0.72f));
        }
    }
}

void drawControlCluster(eui::Ui& ui, const std::string& id, float width, int section) {
    ViewerState& viewer = state();
    const auto tokens = section % 2 == 0 ? components::theme::light() : components::theme::dark();
    components::button(ui, id + ".button.primary")
        .size(std::min(180.0f, width * 0.42f), 42.0f)
        .text("shuffle " + std::to_string(section + 1))
        .theme(tokens, true)
        .onClick([&viewer] {
            viewer.seed += 11;
            app::requestUpdate();
        })
        .build();

    ui.stack(id + ".offset.slider")
        .x(std::min(210.0f, width * 0.47f))
        .size(std::max(120.0f, width - std::min(210.0f, width * 0.47f)), 42.0f)
        .content([&] {
            label(ui, id + ".density.label", 0.0f, 0.0f, 120.0f, "density", 13.0f, rgba(0.40f, 0.44f, 0.50f));
            ui.stack(id + ".density.position")
                .x(88.0f)
                .y(3.0f)
                .size(std::max(80.0f, width - 310.0f), 28.0f)
                .content([&] {
                    components::slider(ui, id + ".density")
                        .size(std::max(80.0f, width - 310.0f), 28.0f)
                        .bind(viewer.density)
                        .theme(tokens)
                        .build();
                })
                .build();
        })
        .build();

    ui.stack(id + ".toggles")
        .y(54.0f)
        .size(width, 42.0f)
        .content([&] {
            components::checkbox(ui, id + ".check")
                .size(172.0f, 32.0f)
                .text("live rows")
                .bind(viewer.live)
                .theme(tokens)
                .build();
            ui.stack(id + ".switch.position")
                .x(190.0f)
                .size(180.0f, 32.0f)
                .content([&] {
                    components::toggleSwitch(ui, id + ".switch")
                        .size(180.0f, 32.0f)
                        .text("compact")
                        .bind(viewer.compact)
                        .theme(tokens)
                        .build();
                })
                .build();
            ui.stack(id + ".progress.position")
                .x(std::min(400.0f, width - 260.0f))
                .y(10.0f)
                .size(std::max(120.0f, width - 420.0f), 12.0f)
                .content([&] {
                    components::progress(ui, id + ".progress")
                        .size(std::max(120.0f, width - 420.0f), 12.0f)
                        .value(0.22f + static_cast<float>((section * 17) % 63) / 100.0f)
                        .theme(tokens)
                        .build();
                })
                .build();
        })
        .build();

    ui.stack(id + ".tabs.wrap")
        .y(104.0f)
        .size(std::min(width, 430.0f), 46.0f)
        .content([&] {
            components::tabs(ui, id + ".tabs")
                .size(std::min(width, 430.0f), 42.0f)
                .items({"Grid", "Signals", "Cache", "Paint"})
                .bind(viewer.tab)
                .theme(tokens)
                .build();
        })
        .build();
}

void drawDenseList(eui::Ui& ui, const std::string& id, float x, float y, float width, int rows, int seed) {
    for (int i = 0; i < rows; ++i) {
        const float rowY = y + static_cast<float>(i) * 34.0f;
        const eui::Color accent = palette(i + seed, 1.0f);
        ui.rect(id + ".row." + std::to_string(i))
            .x(x + random01(seed, i) * 18.0f)
            .y(rowY)
            .size(std::max(80.0f, width - random01(seed, i + 200) * 90.0f), 24.0f)
            .radius(7.0f)
            .color(i % 2 == 0 ? rgba(1.0f, 1.0f, 1.0f, 0.74f) : rgba(0.95f, 0.97f, 0.99f, 0.86f))
            .border(1.0f, rgba(accent.r, accent.g, accent.b, 0.16f))
            .build();
        ui.rect(id + ".row.dot." + std::to_string(i))
            .x(x + 10.0f + random01(seed, i + 300) * 12.0f)
            .y(rowY + 8.0f)
            .size(8.0f, 8.0f)
            .radius(4.0f)
            .color(accent)
            .build();
        label(ui,
              id + ".row.text." + std::to_string(i),
              x + 32.0f + random01(seed, i + 400) * 14.0f,
              rowY + 6.0f,
              std::max(0.0f, width - 54.0f),
              "mixed row " + std::to_string(i + 1),
              12.0f,
              rgba(0.18f, 0.21f, 0.25f, 0.74f));
    }
}

void drawSection(eui::Ui& ui, const std::string& id, float width, int section) {
    ViewerState& viewer = state();
    const float height = viewer.compact.get() ? 420.0f : 560.0f;
    const int seed = viewer.seed + section * 31;
    const eui::Color accent = palette(section, 1.0f);
    const int scatterCount = 18 + static_cast<int>(viewer.density.get() * 30.0f);

    ui.stack(id)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".panel")
                .size(width, height)
                .radius(8.0f)
                .gradient(rgba(0.98f, 0.99f, 0.98f),
                          rgba(0.91f + random01(seed, 2) * 0.04f, 0.94f, 0.98f),
                          section % 2 == 0 ? eui::GradientDirection::Vertical : eui::GradientDirection::Horizontal)
                .border(1.0f, rgba(accent.r, accent.g, accent.b, 0.22f))
                .shadow(18.0f, 0.0f, 7.0f, rgba(0.08f, 0.10f, 0.15f, 0.07f))
                .build();

            label(ui, id + ".title", 22.0f, 20.0f, width - 44.0f, "Section " + std::to_string(section + 1), 24.0f);
            label(ui,
                  id + ".note",
                  22.0f,
                  52.0f,
                  width - 44.0f,
                  "Randomized cards, controls, lists and retained surfaces inside one scroll viewport.",
                  13.0f,
                  rgba(0.42f, 0.46f, 0.52f));

            scatterCards(ui, id, width, height, seed, scatterCount);
            drawControlCluster(ui, id + ".cluster", std::max(0.0f, width - 48.0f), section);

            const float listX = section % 2 == 0 ? 28.0f : std::max(28.0f, width * 0.50f);
            const float listY = 252.0f + random01(seed, 44) * 42.0f;
            const float listW = std::min(width * 0.44f, width - listX - 28.0f);
            drawDenseList(ui, id + ".list", listX, listY, listW, viewer.live.get() ? 7 + section % 5 : 4, seed);

            for (int i = 0; i < 6; ++i) {
                const float px = 28.0f + random01(seed, 500 + i) * std::max(1.0f, width - 170.0f);
                const float py = 118.0f + random01(seed, 520 + i) * std::max(1.0f, height - 160.0f);
                pill(ui,
                     id + ".pill." + std::to_string(i),
                     px,
                     py,
                     96.0f + random01(seed, 540 + i) * 58.0f,
                     "tag " + std::to_string(i + section),
                     palette(i + section, 1.0f));
            }
        })
        .build();
}

} // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("Complex Scroll Stress Viewer")
        .pageId("complex_scroll_stress_viewer")
        .clearColor(rgba(0.90f, 0.93f, 0.94f, 1.0f))
        .windowSize(1280, 860)
        .showDebugStatsInTitle(true)
        .fps(120.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ViewerState& viewer = state();
    const float margin = std::clamp(screen.width * 0.035f, 28.0f, 48.0f);
    const float top = 28.0f;
    const float viewportY = 112.0f;
    const float viewportH = std::max(260.0f, screen.height - viewportY - 28.0f);
    const float viewportW = std::max(540.0f, screen.width - margin * 2.0f);
    const float left = (screen.width - viewportW) * 0.5f;

    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.rect("background")
                .size(screen.width, screen.height)
                .gradient(rgba(0.89f, 0.93f, 0.93f),
                          rgba(0.96f, 0.94f, 0.89f),
                          eui::GradientDirection::Vertical)
                .build();

            label(ui, "title", margin, top, viewportW, "Complex Scroll Stress Viewer", 30.0f, rgba(0.07f, 0.09f, 0.12f));
            label(ui,
                  "subtitle",
                  margin,
                  top + 40.0f,
                  viewportW,
                  "Debug title is enabled. Scroll the viewport and interact with controls to stress dirty repaint and cache paths.",
                  15.0f,
                  rgba(0.34f, 0.38f, 0.44f));

            components::scrollView(ui, "complex.scroll")
                .x(left)
                .y(viewportY)
                .size(viewportW, viewportH)
                .bind(viewer.scroll)
                .gap(18.0f)
                .step(72.0f)
                .scrollbarWidth(10.0f)
                .scrollbarGap(18.0f)
                .contentKey("complex-scroll-sections")
                .transition(0.14f, eui::Ease::OutCubic)
                .content([&](eui::Ui& contentUi, float contentWidth, float) {
                    for (int section = 0; section < 10; ++section) {
                        drawSection(contentUi, "section." + std::to_string(section), contentWidth, section);
                    }
                })
                .build();
        })
        .build();
}

} // namespace app
