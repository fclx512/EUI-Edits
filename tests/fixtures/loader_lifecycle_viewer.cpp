#include "eui_neo.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace app {
namespace {

struct LoaderLifecycleState {
    bool destroyActive = true;
    bool keepActive = true;
};

struct CounterState {
    int value = 0;
};

struct TextState {
    std::string value = "loader scoped input";
};

struct ComponentState {
    bool heartChecked = false;
    int buttonClicks = 0;
    float chartSeed = 0.0f;
};

LoaderLifecycleState& state() {
    static LoaderLifecycleState value;
    return value;
}

void title(eui::Ui& ui, const std::string& id, const std::string& text, float x, float y, float width) {
    ui.text(id)
        .position(x, y)
        .size(width, 28.0f)
        .text(text)
        .fontSize(20.0f)
        .lineHeight(26.0f)
        .fontWeight(700)
        .color({0.94f, 0.96f, 1.0f, 1.0f})
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

void note(eui::Ui& ui, const std::string& id, const std::string& text, float x, float y, float width) {
    ui.text(id)
        .position(x, y)
        .size(width, 42.0f)
        .text(text)
        .fontSize(14.0f)
        .lineHeight(20.0f)
        .color({0.70f, 0.75f, 0.84f, 1.0f})
        .wrap()
        .build();
}

void counterContent(eui::Ui& ui, const std::string& id, float width) {
    CounterState& counter = ui.state<CounterState>("counter");
    TextState& textState = ui.state<TextState>("input.value");
    ComponentState& component = ui.state<ComponentState>("component");

    ui.text(id + ".value")
        .position(0.0f, 0.0f)
        .size(width, 34.0f)
        .text("instance counter: " + std::to_string(counter.value))
        .fontSize(18.0f)
        .lineHeight(24.0f)
        .fontWeight(700)
        .color({0.94f, 0.96f, 1.0f, 1.0f})
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    ui.stack(id + ".inc.wrap")
        .position(0.0f, 54.0f)
        .size(132.0f, 40.0f)
        .content([&] {
            components::button(ui, id + ".inc")
                .size(132.0f, 40.0f)
                .text("Increment")
                .onClick([&counter] {
                    ++counter.value;
                })
                .build();
        })
        .build();

    ui.stack(id + ".input.wrap")
        .position(0.0f, 110.0f)
        .size(width, 42.0f)
        .content([&] {
            components::input(ui, id + ".input")
                .size(width, 42.0f)
                .value(textState.value)
                .placeholder("type here")
                .onChange([&textState](const std::string& value) {
                    textState.value = value;
                })
                .build();
        })
        .build();

    ui.text(id + ".components.title")
        .position(0.0f, 168.0f)
        .size(width, 24.0f)
        .text("component state")
        .fontSize(14.0f)
        .lineHeight(18.0f)
        .fontWeight(700)
        .color({0.70f, 0.76f, 0.86f, 1.0f})
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    ui.stack(id + ".heart.wrap")
        .position(0.0f, 204.0f)
        .size(58.0f, 58.0f)
        .content([&] {
            components::workshop::heartSwitch(ui, id + ".heart")
                .size(58.0f, 58.0f)
                .checked(component.heartChecked)
                .onChange([&component](bool value) {
                    component.heartChecked = value;
                })
                .build();
        })
        .build();

    ui.stack(id + ".soft.wrap")
        .position(74.0f, 194.0f)
        .size(std::max(140.0f, width - 74.0f), 78.0f)
        .content([&] {
            components::workshop::neumorphicButton(ui, id + ".soft")
                .size(std::max(140.0f, width - 74.0f), 78.0f)
                .text("Clicks " + std::to_string(component.buttonClicks))
                .onClick([&component] {
                    ++component.buttonClicks;
                    component.chartSeed = std::fmod(component.chartSeed + 0.17f, 1.0f);
                })
                .build();
        })
        .build();

    std::vector<float> values = {
        0.34f + component.chartSeed * 0.20f,
        0.22f,
        0.18f + (1.0f - component.chartSeed) * 0.12f,
        0.16f
    };
    ui.stack(id + ".pie.wrap")
        .position(0.0f, 290.0f)
        .size(width, 170.0f)
        .content([&] {
            components::pieChart(ui, id + ".pie")
                .size(width, 170.0f)
                .title("Scoped Pie")
                .values(std::move(values))
                .labels({"A", "B", "C", "D"})
                .build();
        })
        .build();
}

void loaderCase(eui::Ui& ui,
                const std::string& id,
                const std::string& heading,
                const std::string& description,
                bool& active,
                bool keepAlive,
                float x,
                float y,
                float width,
                float height) {
    components::card(ui, id)
        .position(x, y)
        .size(width, height)
        .padding(18.0f)
        .color({0.10f, 0.12f, 0.16f, 1.0f})
        .border(1.0f, {0.24f, 0.29f, 0.38f, 1.0f})
        .content([&] {
            title(ui, id + ".title", heading, 0.0f, 0.0f, width - 36.0f);
            note(ui, id + ".note", description, 0.0f, 36.0f, width - 36.0f);

            ui.stack(id + ".toggle.wrap")
                .position(0.0f, 88.0f)
                .size(132.0f, 40.0f)
                .content([&] {
                    components::button(ui, id + ".toggle")
                        .size(132.0f, 40.0f)
                        .text(active ? "Hide" : "Show")
                        .onClick([&active] {
                            active = !active;
                        })
                        .build();
                })
                .build();

            if (!active) {
                ui.text(id + ".empty")
                    .position(0.0f, 150.0f)
                    .size(width - 36.0f, 42.0f)
                    .text("loader inactive")
                    .fontSize(16.0f)
                    .lineHeight(22.0f)
                    .color({0.58f, 0.64f, 0.72f, 1.0f})
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }

            auto loader = ui.loader(id + ".loader")
                .active(active);
            if (keepAlive) {
                loader.keepAlive();
            } else {
                loader.destroyOnHide();
            }
            loader.content([&] {
                    ui.stack(id + ".loaded")
                        .position(0.0f, 150.0f)
                        .size(width - 36.0f, 470.0f)
                        .content([&] {
                            counterContent(ui, id + ".loaded.content", width - 36.0f);
                        })
                        .build();
                })
                .build();
        })
        .build();
}

} // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("Loader Lifecycle Viewer")
        .pageId("loader_lifecycle_viewer")
        .clearColor({0.07f, 0.08f, 0.10f, 1.0f})
        .windowSize(860, 760)
        .showDebugStatsInTitle(false)
        .fps(0.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    LoaderLifecycleState& s = state();
    const float contentWidth = std::min(780.0f, std::max(360.0f, screen.width - 64.0f));
    const float left = std::max(0.0f, (screen.width - contentWidth) * 0.5f);
    const float top = 56.0f;
    const float gap = 20.0f;
    const float cardWidth = (contentWidth - gap) * 0.5f;
    const float cardHeight = 650.0f;

    ui.rect("background")
        .size(screen.width, screen.height)
        .gradient({0.07f, 0.08f, 0.10f, 1.0f}, {0.10f, 0.11f, 0.15f, 1.0f}, eui::GradientDirection::Vertical)
        .build();

    ui.text("title")
        .position(left, 16.0f)
        .size(contentWidth, 32.0f)
        .text("Loader Lifecycle")
        .fontSize(25.0f)
        .lineHeight(31.0f)
        .fontWeight(700)
        .color({0.94f, 0.96f, 1.0f, 1.0f})
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    ui.text("subtitle")
        .position(left, 48.0f)
        .size(contentWidth, 28.0f)
        .text("Hide and show each side: DestroyOnHide resets scoped input/component state; KeepAlive restores it.")
        .fontSize(14.0f)
        .lineHeight(20.0f)
        .color({0.64f, 0.70f, 0.79f, 1.0f})
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    loaderCase(ui,
               "destroy",
               "DestroyOnHide",
               "Hiding releases state under this loader scope.",
               s.destroyActive,
               false,
               left,
               top + 30.0f,
               cardWidth,
               cardHeight);

    loaderCase(ui,
               "keep",
               "KeepAlive",
               "Hiding skips compose but keeps state for next show.",
               s.keepActive,
               true,
               left + cardWidth + gap,
               top + 30.0f,
               cardWidth,
               cardHeight);
}

} // namespace app
