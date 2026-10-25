#pragma once

#include "core/render/render_types.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace core::dsl::runtime {

// Stateful sampler for pointer-driven RevealEffect. Every input can update the
// latest local pointer and press origin; visual fields are committed only at
// the 60 Hz sample boundary (apart from the first immediately visible sample).
class RevealState {
public:
    static constexpr float kSampleIntervalSeconds = 1.0f / 60.0f;
    static constexpr float kHoverDurationSeconds = 0.12f;
    static constexpr float kPressDurationSeconds = 0.26f;

    bool update(const core::RevealEffect& config,
                core::Vec2 pointerLocal,
                float width,
                float height,
                bool hover,
                bool pressed,
                bool inputValid,
                float deltaSeconds,
                bool allowVisualSample = true) {
        const core::RevealEffect previous = effect_;
        const bool valid = config.enabled && inputValid &&
            std::isfinite(width) && std::isfinite(height) && width > 0.0f && height > 0.0f &&
            std::isfinite(pointerLocal.x) && std::isfinite(pointerLocal.y);
        if (!valid) {
            clear();
            return !sameVisual(previous, effect_);
        }

        config_ = config;
        latestPointer_ = pointerLocal; // Preserve coordinates outside the element.
        lastWidth_ = width;
        lastHeight_ = height;
        const bool firstInput = !initialized_;
        const bool hoverChanged = firstInput ? hover : hover_ != hover;
        const bool pressedNow = pressed && !pressed_;
        initialized_ = true;
        hover_ = hover;
        pressed_ = pressed;

        const float rawDelta = std::isfinite(deltaSeconds)
            ? std::max(0.0f, deltaSeconds)
            : 0.0f;
        if (std::isfinite(cooldownRemaining_)) {
            cooldownRemaining_ = std::max(0.0f, cooldownRemaining_ - rawDelta);
        if (cooldownRemaining_ <= 0.000001f) cooldownRemaining_ = 0.0f;
        }

        if (hoverChanged) hoverTarget_ = hover ? 1.0f : 0.0f;
        if (pressedNow) {
            pressCenter_ = pointerLocal;
            pressElapsed_ = 0.0f;
            pressActive_ = config.pressRadius > 0.0f && config.pressOpacity > 0.0f;
        }

        // A newly-started animation never consumes the preceding idle gap.
        // Existing animations advance once per input batch, using the host's
        // delta only on the final pointer event.
        if (!firstInput && !hoverChanged && !pressedNow) {
            advanceAnimations(std::min(rawDelta, 0.05f));
        }

        const core::RevealEffect candidate = sampledEffect(width, height);
        const bool pending = !sameVisual(candidate, effect_);
        if (allowVisualSample && pending && cooldownRemaining_ <= 0.0f) {
            effect_ = candidate;
            cooldownRemaining_ = kSampleIntervalSeconds;
        }
        return !sameVisual(previous, effect_);
    }

    const core::RevealEffect& effect() const { return effect_; }

    float wakeSeconds() const {
        if (!initialized_) return std::numeric_limits<float>::infinity();
        const bool pending = hasPendingVisual();
        const bool animating = hasVisualAnimation();
        if (!pending && !animating) return std::numeric_limits<float>::infinity();
        if (cooldownRemaining_ > 0.0f) return cooldownRemaining_;
        return pending ? 0.0f : kSampleIntervalSeconds;
    }

    void clear() {
        initialized_ = false;
        hover_ = false;
        pressed_ = false;
        hoverTarget_ = 0.0f;
        hoverProgress_ = 0.0f;
        latestPointer_ = {};
        pressCenter_ = {};
        pressElapsed_ = 0.0f;
        pressActive_ = false;
        cooldownRemaining_ = 0.0f;
        config_ = {};
        effect_ = {};
        lastWidth_ = 0.0f;
        lastHeight_ = 0.0f;
    }

private:
    static bool samePoint(core::Vec2 a, core::Vec2 b) {
        return a.x == b.x && a.y == b.y;
    }

    static bool sameColor(core::Color a, core::Color b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    static bool sameVisual(const core::RevealEffect& a, const core::RevealEffect& b) {
        // Canonicalize all invisible states. In particular, pointer motion far
        // from both opted-in controls must not dirty either one.
        if (!a.enabled && !b.enabled) return true;
        return a.enabled == b.enabled && samePoint(a.center, b.center) &&
               a.radius == b.radius && sameColor(a.tint, b.tint) &&
               a.fillOpacity == b.fillOpacity && a.borderOpacity == b.borderOpacity &&
               a.borderWidth == b.borderWidth && samePoint(a.pressCenter, b.pressCenter) &&
               a.pressRadius == b.pressRadius && a.pressOpacity == b.pressOpacity;
    }

    static float distanceToRect(float x, float y, float width, float height) {
        const float dx = std::max({0.0f, -x, x - width});
        const float dy = std::max({0.0f, -y, y - height});
        return std::sqrt(dx * dx + dy * dy);
    }

    bool hasVisualAnimation() const {
        const bool hoverVisible = config_.fillOpacity > 0.0f && hoverProgress_ != hoverTarget_;
        const bool pressVisible = pressActive_ &&
            (config_.pressOpacity > 0.0f || config_.pressRadius > 0.0f);
        return hoverVisible || pressVisible;
    }

    bool hasPendingVisual() const {
        return !sameVisual(sampledEffect(lastWidth_, lastHeight_), effect_);
    }

    void advanceAnimations(float deltaSeconds) {
        if (deltaSeconds <= 0.0f) return;
        const float step = deltaSeconds / kHoverDurationSeconds;
        if (hoverProgress_ < hoverTarget_) {
            hoverProgress_ = std::min(hoverTarget_, hoverProgress_ + step);
        } else if (hoverProgress_ > hoverTarget_) {
            hoverProgress_ = std::max(hoverTarget_, hoverProgress_ - step);
        }
        if (pressActive_) {
            pressElapsed_ = std::min(kPressDurationSeconds, pressElapsed_ + deltaSeconds);
            if (pressElapsed_ >= kPressDurationSeconds) pressActive_ = false;
        }
    }

    core::RevealEffect sampledEffect(float width, float height) const {
        core::RevealEffect result{};
        const float radius = std::max(0.0f, config_.radius);
        const float distance = distanceToRect(latestPointer_.x, latestPointer_.y, width, height);
        const float proximity = radius > 0.0f
            ? std::clamp(1.0f - distance / radius, 0.0f, 1.0f)
            : (distance == 0.0f ? 1.0f : 0.0f);
        const float hover = std::clamp(hoverProgress_, 0.0f, 1.0f);
        const float smoothHover = hover * hover * (3.0f - 2.0f * hover);
        const float fill = std::max(0.0f, config_.fillOpacity) * smoothHover;
        const float border = std::max(0.0f, config_.borderOpacity) * proximity;
        const float pressT = pressActive_
            ? std::clamp(pressElapsed_ / kPressDurationSeconds, 0.0f, 1.0f)
            : 1.0f;
        const float pressOpacity = pressActive_
            ? std::max(0.0f, config_.pressOpacity) * (1.0f - pressT)
            : 0.0f;
        const float pressRadius = pressActive_
            ? std::max(0.0f, config_.pressRadius) * pressT
            : 0.0f;
        const bool visible = fill > 0.0f || border > 0.0f || pressOpacity > 0.0f;
        if (!visible) return result;

        result.enabled = true;
        result.center = latestPointer_;
        result.radius = radius;
        result.tint = config_.tint;
        result.fillOpacity = fill;
        result.borderOpacity = border;
        result.borderWidth = std::max(0.0f, config_.borderWidth);
        result.pressCenter = pressCenter_;
        result.pressRadius = pressRadius;
        result.pressOpacity = pressOpacity;
        return result;
    }

    bool initialized_ = false;
    bool hover_ = false;
    bool pressed_ = false;
    float hoverTarget_ = 0.0f;
    float hoverProgress_ = 0.0f;
    core::Vec2 latestPointer_{};
    core::Vec2 pressCenter_{};
    float pressElapsed_ = 0.0f;
    bool pressActive_ = false;
    float cooldownRemaining_ = 0.0f;
    float lastWidth_ = 0.0f;
    float lastHeight_ = 0.0f;
    core::RevealEffect config_{};
    core::RevealEffect effect_{};
};

} // namespace core::dsl::runtime
