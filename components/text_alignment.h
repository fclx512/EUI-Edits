#pragma once

#include "core/render/text.h"

namespace components::text_alignment {

// One reference per font/size/weight keeps neighbouring CJK, Latin and number
// labels on the same baseline. The renderer's existing ink cache includes font
// generation, fallback stack and physical pixel scale; do not cache a partial key.
inline core::Vec2 referenceInk(float size, const std::string& family = {}, int weight = 400) {
    core::TextStyle reference;
    reference.text = "Ag中文";
    reference.fontFamily = family;
    reference.fontSize = size;
    reference.fontWeight = weight;
    return core::TextPrimitive::measureSingleLineInk(reference);
}

inline float singleLineTop(float height, float size, const std::string& family = {}, int weight = 400) {
    const auto ink = referenceInk(size, family, weight);
    return (height - ink.x - ink.y) * .5f;
}

} // namespace components::text_alignment
