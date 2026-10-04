#include "core/runtime/runtime_state_bindings.h"

#include <cmath>
#include <iostream>

namespace {

bool near(float value, float expected) {
    return std::fabs(value - expected) <= 0.01f;
}

bool contains(const core::Rect& outer, const core::Rect& inner) {
    return outer.x <= inner.x + 0.01f && outer.y <= inner.y + 0.01f &&
           outer.x + outer.width >= inner.x + inner.width - 0.01f &&
           outer.y + outer.height >= inner.y + inner.height - 0.01f;
}

bool boundOffsetSynchronization() {
    using namespace core::dsl;
    Element owner;
    owner.scrollOffset = 480.0f;
    owner.scrollMaxOffset = 900.0f;
    runtime::ScrollStateInstance instance;

    if (syncOwnedScrollState(owner, instance, true) || !instance.initialized ||
        !near(instance.offset, 480.0f)) {
        std::cerr << "initial controlled scroll offset was not adopted\n";
        return false;
    }

    // A scroll callback can update the bound value to the live offset while
    // inertia is still active. Composing that acknowledgment must keep velocity.
    instance.offset = 620.0f;
    instance.velocity = 180.0f;
    instance.dirtyRect = {50.0f, 30.0f, 120.0f, 60.0f};
    instance.hasDirtyRect = true;
    owner.scrollOffset = 620.0f;
    if (syncOwnedScrollState(owner, instance, true) || !near(instance.offset, 620.0f) ||
        !near(instance.velocity, 180.0f)) {
        std::cerr << "bound callback acknowledgment interrupted live inertia\n";
        return false;
    }
    if (!instance.hasDirtyRect || !near(instance.dirtyRect.x, 50.0f) ||
        !near(instance.dirtyRect.y, 30.0f) || !near(instance.dirtyRect.width, 120.0f) ||
        !near(instance.dirtyRect.height, 60.0f)) {
        std::cerr << "scroll synchronization overwrote transformed viewport damage\n";
        return false;
    }

    // Runtime scrolling may advance without a recompose because the virtual row
    // window has not crossed its first slot. A later explicit reset to the same
    // value that was bound earlier must still be accepted during compose.
    instance.offset = 660.0f;
    instance.velocity = 120.0f;
    owner.scrollOffset = 480.0f;
    if (!syncOwnedScrollState(owner, instance, true) || !near(instance.offset, 480.0f) ||
        !near(instance.velocity, 0.0f)) {
        std::cerr << "explicit reset to the prior bound offset was ignored\n";
        return false;
    }

    // Update passes keep the composed value as a snapshot, while a reduced
    // content range still clamps the live offset and stops out-of-range inertia.
    owner.scrollMaxOffset = 200.0f;
    instance.offset = 660.0f;
    instance.velocity = 120.0f;
    if (!syncOwnedScrollState(owner, instance, false) || !near(instance.offset, 200.0f) ||
        !near(instance.velocity, 0.0f)) {
        std::cerr << "reduced scroll range did not clamp and stop motion\n";
        return false;
    }
    return true;
}

bool transformedViewportDamage() {
    using namespace core::dsl;
    const core::Rect frame{100.0f, 100.0f, 80.0f, 40.0f};
    const RenderTransform identity;
    const core::Rect singleLayer = scrollViewportDamageRect(frame, 1.0f, identity);
    if (!near(singleLayer.x, frame.x) || !near(singleLayer.y, frame.y) ||
        !near(singleLayer.width, frame.width) || !near(singleLayer.height, frame.height)) {
        std::cerr << "single-layer viewport damage changed its geometry\n";
        return false;
    }

    // Simulate an outer scroller that translated and scaled an inner viewport.
    // The old raw frame lies below and to the right of the viewport now visible
    // inside the clipped outer viewport.
    RenderTransform nested;
    nested.active = true;
    nested.matrix = {1.5f, 0.0f, -100.0f,
                     0.0f, 1.5f, -120.0f,
                     0.0f, 0.0f, 1.0f};
    const core::Rect damage = scrollViewportDamageRect(frame, 1.0f, nested);
    const core::Rect ancestorClip{70.0f, 40.0f, 75.0f, 30.0f};
    core::Rect visible{};
    if (!intersectRect(damage, ancestorClip, visible) || !contains(damage, visible) ||
        intersects(frame, visible)) {
        std::cerr << "nested scroll damage missed the transformed, clipped viewport\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    return boundOffsetSynchronization() && transformedViewportDamage() ? 0 : 1;
}
