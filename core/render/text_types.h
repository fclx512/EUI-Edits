#pragma once

#include "core/render/render_types.h"

#include <string>

namespace core {

enum class HorizontalAlign {
    Left,
    Center,
    Right
};

enum class VerticalAlign {
    Top,
    Center,
    Bottom
};

struct TextStyle {
    std::string text;
    std::string fontFamily;
    float fontSize = 16.0f;
    int fontWeight = 400;
    Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    float maxWidth = 0.0f;
    bool wrap = false;
    HorizontalAlign horizontalAlign = HorizontalAlign::Left;
    VerticalAlign verticalAlign = VerticalAlign::Top;
    float lineHeight = 0.0f;
};

} // namespace core
