#pragma once

#include "components/theme.h"
#include "core/dsl.h"

#include <string>

namespace components {

struct ImageStyle {
    ImageStyle() : ImageStyle(theme::dark()) {}

    explicit ImageStyle(const theme::ThemeColorTokens& tokens) {
        tint = theme::color(1.0f, 1.0f, 1.0f, 1.0f);
        radius = theme::pageVisuals(tokens).sectionRounding;
    }

    core::Color tint;
    float radius = 0.0f;
    float opacity = 1.0f;
};

inline core::dsl::ImageBuilder image(core::dsl::Ui& ui, const std::string& id, const ImageStyle& style) {
    auto builder = ui.image(id);
    builder.tint(style.tint)
        .radius(style.radius)
        .opacity(style.opacity);
    return builder;
}

inline core::dsl::ImageBuilder image(core::dsl::Ui& ui, const std::string& id) {
    return image(ui, id, ImageStyle{});
}

inline core::dsl::ImageBuilder image(core::dsl::Ui& ui, const std::string& id, const theme::ThemeColorTokens& tokens) {
    return image(ui, id, ImageStyle(tokens));
}

} // namespace components
