#include "core/runtime/runtime_geometry.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

bool expectClose(const std::string& label, float actual, float expected) {
    if (std::fabs(actual - expected) <= 0.01f) {
        return true;
    }
    std::cerr << label << ": expected " << expected << ", got " << actual << "\n";
    return false;
}

// The untransformed anchor path is what Runtime::updateImeCursorRect pushes at
// every DPI: the logical caret rect scales with dpiScale while the hit matrix
// of a static element stays identity.
bool imeRectFollowsDpiScale() {
    core::dsl::Element element;
    element.frame = {40.0f, 60.0f, 200.0f, 40.0f};
    element.imeRect = {10.0f, 12.0f, 2.0f, 20.0f};

    const core::TransformMatrix identity;
    const core::Rect rect = core::dsl::imeCursorPixelRect(element, 2.0f, identity);

    return expectClose("DPI IME x", rect.x, 100.0f) &&
           expectClose("DPI IME y", rect.y, 144.0f) &&
           expectClose("DPI IME width", rect.width, 4.0f) &&
           expectClose("DPI IME height", rect.height, 40.0f);
}

bool imeRectFollowsScrollTransform() {
    core::dsl::Element element;
    element.frame = {100.0f, 240.0f, 320.0f, 44.0f};
    element.imeRect = {72.0f, 12.0f, 1.5f, 20.0f};

    core::Transform scrollTransform;
    scrollTransform.translate.y = -80.0f;
    const core::TransformMatrix transform = core::dsl::matrixForTransform(
        core::dsl::toPixelRect(element.frame, 2.0f),
        core::dsl::scaleTransform(scrollTransform, 2.0f));
    const core::Rect rect = core::dsl::imeCursorPixelRect(element, 2.0f, transform);

    return expectClose("IME x", rect.x, 344.0f) &&
           expectClose("IME y", rect.y, 344.0f) &&
           expectClose("IME width", rect.width, 3.0f) &&
           expectClose("IME height", rect.height, 40.0f);
}

} // namespace

int main() {
    return imeRectFollowsScrollTransform() && imeRectFollowsDpiScale() ? 0 : 1;
}
