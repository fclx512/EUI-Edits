#pragma once

#include "components/theme.h"
#include "core/dsl.h"

#include <string>

namespace components {

struct PanelStyle {
    PanelStyle() : PanelStyle(theme::dark()) {}

    explicit PanelStyle(const theme::ThemeColorTokens& tokens) {
        color = tokens.surface;
        border = theme::border(tokens);
        shadow = theme::panelShadow(tokens);
        radius = theme::pageVisuals(tokens).sectionRounding;
    }

    core::Color color;
    core::Gradient gradient;
    core::Border border;
    core::Shadow shadow;
    float radius = 12.0f;
    float opacity = 1.0f;
};

inline core::dsl::RectBuilder panel(core::dsl::Ui& ui, const std::string& id, const PanelStyle& style) {
    auto builder = ui.rect(id);
    builder.color(style.color)
        .gradient(style.gradient)
        .border(style.border)
        .shadow(style.shadow)
        .radius(style.radius)
        .opacity(style.opacity);
    return builder;
}

inline core::dsl::RectBuilder panel(core::dsl::Ui& ui, const std::string& id) {
    return panel(ui, id, PanelStyle{});
}

inline core::dsl::RectBuilder panel(core::dsl::Ui& ui, const std::string& id, const theme::ThemeColorTokens& tokens) {
    return panel(ui, id, PanelStyle(tokens));
}

} // namespace components
