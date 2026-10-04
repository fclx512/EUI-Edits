#include "eui_neo.h"

#include <algorithm>
#include <string>

namespace app {
namespace {

struct ViewerState {
    float scrollOffset = 0.0f;
    bool checked = true;
    int level = 0;
    eui::Signal<float> slider{0.42f};
};

ViewerState& state() {
    static ViewerState value;
    return value;
}

eui::Color color(float r, float g, float b, float a = 1.0f) {
    return {r, g, b, a};
}

components::theme::ThemeColorTokens dayTheme() {
    return {
        color(0.94f, 0.96f, 0.98f),
        color(0.22f, 0.43f, 0.86f),
        color(1.0f, 1.0f, 1.0f),
        color(0.90f, 0.94f, 0.99f),
        color(0.80f, 0.87f, 0.96f),
        color(0.10f, 0.13f, 0.18f),
        color(0.75f, 0.80f, 0.88f),
        false
    };
}

eui::Transition quickTransition() {
    return eui::Transition::make(0.10f, eui::Ease::OutCubic);
}

eui::Transition galleryTransition() {
    return eui::Transition::make(0.12f, eui::Ease::OutCubic);
}

const char* levelName(int level) {
    switch (level) {
    case 0: return "0 simple";
    case 1: return "1 padded";
    case 2: return "2 charts";
    default: return "3 property cards";
    }
}

void setLevel(int level) {
    ViewerState& s = state();
    s.level = std::clamp(level, 0, 3);
    s.scrollOffset = 0.0f;
    app::requestUpdate();
}

void text(eui::Ui& ui, const std::string& id, const std::string& value, float width, float height, float size, eui::Color c) {
    ui.text(id)
        .size(width, height)
        .text(value)
        .fontSize(size)
        .lineHeight(size + 4.0f)
        .color(c)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

void hoverTile(eui::Ui& ui, const std::string& id, float width, int index) {
    const eui::Color base = index % 2 == 0
        ? color(0.97f, 0.98f, 0.99f)
        : color(0.92f, 0.95f, 0.98f);
    const eui::Color hover = index % 3 == 0
        ? color(0.84f, 0.91f, 1.0f)
        : color(0.87f, 0.96f, 0.92f);

    ui.stack(id)
        .size(width, 58.0f)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, 58.0f)
                .states(base, hover, color(0.76f, 0.86f, 0.98f))
                .radius(8.0f)
                .border(1.0f, color(0.70f, 0.76f, 0.84f))
                .shadow(10.0f, 0.0f, 4.0f, color(0.08f, 0.10f, 0.14f, 0.08f))
                .transition(quickTransition())
                .build();

            text(ui,
                 id + ".label",
                 "hover control " + std::to_string(index + 1),
                 width - 32.0f,
                 24.0f,
                 15.0f,
                 color(0.13f, 0.16f, 0.20f));
            ui.text(id + ".meta")
                .y(28.0f)
                .size(width - 32.0f, 18.0f)
                .text("wheel-scroll across this column quickly")
                .fontSize(12.0f)
                .lineHeight(16.0f)
                .color(color(0.42f, 0.47f, 0.54f))
                .build();
        })
        .build();
}

void controlRow(eui::Ui& ui, const std::string& id, float width, int index) {
    ViewerState& s = state();
    ui.row(id)
        .size(width, 48.0f)
        .gap(12.0f)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            components::button(ui, id + ".button")
                .theme(dayTheme())
                .size(144.0f, 38.0f)
                .text("button " + std::to_string(index + 1))
                .colors(color(0.22f, 0.43f, 0.86f), color(0.30f, 0.52f, 0.94f), color(0.16f, 0.34f, 0.74f))
                .transition(quickTransition())
                .build();

            components::checkbox(ui, id + ".check")
                .theme(dayTheme())
                .size(160.0f, 32.0f)
                .text(index % 2 == 0 ? "checkbox" : "hover row")
                .checked(s.checked)
                .onChange([&s](bool value) {
                    s.checked = value;
                })
                .build();

            ui.stack(id + ".slider.wrap")
                .size(std::max(120.0f, width - 328.0f), 32.0f)
                .content([&] {
                    components::slider(ui, id + ".slider")
                        .theme(dayTheme())
                        .size(std::max(120.0f, width - 328.0f), 28.0f)
                        .bind(s.slider)
                        .build();
                })
                .build();
        })
        .build();
}

void chartSection(eui::Ui& ui, const std::string& id, float width) {
    const float gap = 18.0f;
    const float chartW = std::max(168.0f, (width - gap * 2.0f) / 3.0f);
    ui.row(id)
        .size(width, 236.0f)
        .gap(gap)
        .content([&] {
            components::lineChart(ui, id + ".line")
                .theme(dayTheme())
                .size(chartW, 236.0f)
                .title("LineChart")
                .values({0.22f, 0.30f, 0.20f, 0.55f, 0.42f, 0.86f})
                .labels({"Jan", "Feb", "Mar", "Apr", "May", "Jun"})
                .transition(galleryTransition())
                .build();

            components::barChart(ui, id + ".bar")
                .theme(dayTheme())
                .size(chartW, 236.0f)
                .title("BarChart")
                .values({0.92f, 0.36f, 0.68f, 0.52f})
                .labels({"D1", "D2", "D3", "D4"})
                .transition(galleryTransition())
                .build();

            components::pieChart(ui, id + ".pie")
                .theme(dayTheme())
                .size(chartW, 236.0f)
                .title("PieChart")
                .values({0.42f, 0.24f, 0.18f, 0.16f})
                .labels({"Blue", "Green", "Orange", "Pink"})
                .transition(galleryTransition())
                .build();
        })
        .build();
}

void propertyCard(eui::Ui& ui, const std::string& id, const std::string& title, float width, int kind) {
    const eui::Color base = kind == 0 ? color(0.86f, 0.92f, 1.0f) : kind == 1 ? color(1.0f, 1.0f, 1.0f) : color(0.90f, 0.96f, 0.84f);
    ui.stack(id)
        .size(width, 144.0f)
        .visualStateFrom(id + ".bg", 0.95f)
        .content([&] {
            auto rect = ui.rect(id + ".bg")
                .size(width, 144.0f)
                .states(base, color(0.80f, 0.89f, 1.0f), color(0.72f, 0.82f, 0.96f))
                .radius(18.0f)
                .border(1.0f, color(0.74f, 0.80f, 0.88f))
                .transition(galleryTransition());
            if (kind == 1) {
                rect.shadow(18.0f, 0.0f, 8.0f, color(0.10f, 0.14f, 0.22f, 0.12f));
            } else if (kind == 2) {
                rect.rotate(0.08f).transformOrigin(0.5f, 0.5f);
            }
            rect.build();

            ui.text(id + ".title")
                .y(36.0f)
                .size(width, 32.0f)
                .text(title)
                .fontSize(22.0f)
                .lineHeight(28.0f)
                .color(color(0.11f, 0.14f, 0.18f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();

            ui.text(id + ".note")
                .y(90.0f)
                .size(width, 24.0f)
                .text("hover + transition")
                .fontSize(15.0f)
                .lineHeight(20.0f)
                .color(color(0.42f, 0.47f, 0.54f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();
        })
        .build();
}

void propertySection(eui::Ui& ui, const std::string& id, float width) {
    const float gap = 18.0f;
    const float cardW = std::max(120.0f, (width - gap * 2.0f) / 3.0f);
    ui.row(id)
        .size(width, 144.0f)
        .gap(gap)
        .content([&] {
            propertyCard(ui, id + ".color", "Color", cardW, 0);
            propertyCard(ui, id + ".shadow", "Shadow", cardW, 1);
            propertyCard(ui, id + ".rotate", "Rotate", cardW, 2);
        })
        .build();
}

void composeMiddleColumn(eui::Ui& ui, float width) {
    const int level = state().level;
    text(ui,
         "content.title",
         std::string("Hover scroll repro - ") + levelName(level),
         width,
         34.0f,
         24.0f,
         color(0.09f, 0.11f, 0.15f));
    text(ui,
         "content.note",
         "Keep the mouse over the controls and wheel fast. Drag the scrollbar as the comparison path.",
         width,
         28.0f,
         14.0f,
         color(0.38f, 0.43f, 0.50f));

    const int simpleCount = level >= 2 ? 18 : 34;
    for (int i = 0; i < simpleCount; ++i) {
        if (i % 5 == 2) {
            controlRow(ui, "control.row." + std::to_string(i), width, i);
        } else {
            hoverTile(ui, "hover.tile." + std::to_string(i), width, i);
        }
    }

    if (level >= 2) {
        text(ui, "charts.title", "Charts / tooltip hover targets", width, 30.0f, 22.0f, color(0.09f, 0.11f, 0.15f));
        chartSection(ui, "charts.row.0", width);
        chartSection(ui, "charts.row.1", width);
    }

    if (level >= 3) {
        text(ui, "properties.title", "Primitive property cards", width, 30.0f, 22.0f, color(0.09f, 0.11f, 0.15f));
        propertySection(ui, "properties.row.0", width);
        propertySection(ui, "properties.row.1", width);
    }
}

void toolbar(eui::Ui& ui, float width) {
    ViewerState& s = state();
    ui.row("level.toolbar")
        .size(width, 40.0f)
        .gap(10.0f)
        .content([&] {
            for (int level = 0; level < 4; ++level) {
                const bool active = s.level == level;
                components::button(ui, "level." + std::to_string(level))
                    .theme(dayTheme(), active)
                    .size(160.0f, 36.0f)
                    .text(levelName(level))
                    .colors(active ? color(0.22f, 0.43f, 0.86f) : color(0.96f, 0.97f, 0.99f),
                            active ? color(0.30f, 0.52f, 0.94f) : color(0.88f, 0.92f, 0.98f),
                            active ? color(0.16f, 0.34f, 0.74f) : color(0.78f, 0.84f, 0.92f))
                    .textColor(active ? color(1.0f, 1.0f, 1.0f) : color(0.12f, 0.15f, 0.20f))
                    .transition(quickTransition())
                    .onClick([level] {
                        setLevel(level);
                    })
                    .build();
            }
        })
        .build();
}

} // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("Hover Scroll Repro Viewer")
        .pageId("hover_scroll_repro_viewer")
        .clearColor(color(0.90f, 0.93f, 0.95f))
        .windowSize(1180, 820)
        .showDebugStatsInTitle(true)
        .fps(120.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ViewerState& s = state();
    const float pagePad = 44.0f;
    const float toolbarY = 18.0f;
    const float viewportW = std::max(640.0f, screen.width - pagePad * 2.0f);
    const float viewportH = std::max(360.0f, screen.height - 112.0f);
    const float viewportX = (screen.width - viewportW) * 0.5f;
    const float viewportY = 70.0f;
    const float sidePad = 112.0f;
    const float middleW = std::max(360.0f, viewportW - sidePad * 2.0f - 28.0f);

    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.rect("background")
                .size(screen.width, screen.height)
                .color(color(0.90f, 0.93f, 0.95f))
                .build();

            ui.stack("toolbar.position")
                .position(viewportX, toolbarY)
                .size(viewportW, 40.0f)
                .content([&] {
                    toolbar(ui, viewportW);
                })
                .build();

            components::scrollView(ui, "repro.scroll")
                .theme(dayTheme())
                .position(viewportX, viewportY)
                .size(viewportW, viewportH)
                .offset(s.scrollOffset)
                .gap(10.0f)
                .step(64.0f)
                .scrollbarWidth(10.0f)
                .scrollbarGap(18.0f)
                .onChange([&s](float value) {
                    s.scrollOffset = value;
                })
                .content([&](eui::Ui& contentUi, float contentWidth, float) {
                    contentUi.row("content.row")
                        .width(contentWidth)
                        .height(eui::SizeValue::wrapContent())
                        .gap(14.0f)
                        .content([&] {
                            contentUi.stack("left.padding")
                                .size(sidePad, 1.0f)
                                .build();

                            contentUi.column("middle.controls")
                                .width(middleW)
                                .height(eui::SizeValue::wrapContent())
                                .gap(10.0f)
                                .content([&] {
                                    composeMiddleColumn(contentUi, middleW);
                                })
                                .build();

                            contentUi.stack("right.padding")
                                .size(sidePad, 1.0f)
                                .build();
                        })
                        .build();
                })
                .build();
        })
        .build();
}

} // namespace app
