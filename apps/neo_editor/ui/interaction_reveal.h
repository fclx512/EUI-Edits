#pragma once

#include "../model/style_schema.h"
#include <cstdlib>

namespace neo::interaction_reveal {

inline bool enabled(UiStyle style, bool animations, const char* overrideValue) {
    return style == UiStyle::Square && animations &&
           !(overrideValue && overrideValue[0] == '0');
}
inline bool enabled(UiStyle style, bool animations) {
    const char* value = std::getenv("NEO_REVEAL");
    // Preserve the accepted trial's diagnostic switch for existing launchers.
    if (!value) value = std::getenv("NEO_SETTINGS_REVEAL");
    return enabled(style, animations, value);
}

// Theme selects the light's color; geometry style independently opts in.
inline core::RevealEffect effect(const EditorColors& colors, bool enabled) {
    core::RevealEffect result{};
    result.enabled = enabled;
    result.tint = colors.accent;
    result.radius = 72.0f;
    result.borderWidth = 1.0f;
    result.borderOpacity = 0.85f;
    result.fillOpacity = 0.10f;
    result.pressRadius = 96.0f;
    result.pressOpacity = 0.24f;
    return result;
}

// Application policy, not a framework default. Stateful action surfaces are
// eligible; editor input, drag canvases and inert click-catching shields are not.
inline bool eligible(const core::dsl::Element& element) {
    return element.kind == core::dsl::ElementKind::Rect && element.onClick &&
        (element.hasStateColors || (element.id.find(".item.") != std::string::npos &&
                                   element.cursor == core::CursorShape::Hand)) &&
        !element.onDrag && !element.onDragUpdate && !element.onTextInput &&
        !element.hasImeRect && element.sliderStateId.empty() &&
        element.scrollStateId.empty() && element.pointerRuntimeSourceId.empty();
}

inline bool blocksSubtree(const core::dsl::Element& element) {
    const auto named = [&](const char* prefix) {
        return element.id.rfind(prefix, 0) == 0 ||
            element.id.find(std::string(".") + prefix) != std::string::npos;
    };
    return element.disabled || element.hitTestMode == core::dsl::HitTestMode::None ||
        named("window.") || named("themeWorkbench.preview");
}

inline void apply(core::dsl::Element& element, const core::RevealEffect& config, bool blocked) {
    if (config.enabled && !blocked && eligible(element)) {
        if (!element.reveal) element.reveal = std::make_unique<core::RevealEffect>();
        *element.reveal = config;
    } else {
        element.reveal.reset();
    }
}

} // namespace neo::interaction_reveal
