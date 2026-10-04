#pragma once

#include <algorithm>
#include <cmath>

namespace neo {

// Zoom is relative to the initial fit-to-window view, never to decoded pixels.
struct ImageViewport {
    float zoom = 1.0f;
    float panX = 0.0f, panY = 0.0f;
    float pointerX = 0.0f, pointerY = 0.0f;
    bool pointerKnown = false;
    bool focusPending = true;

    float fit(float imageW, float imageH, float windowW, float windowH) const {
        return std::min(std::max(1.0f, windowW * 0.92f) / std::max(1.0f, imageW),
                        std::max(1.0f, windowH * 0.92f) / std::max(1.0f, imageH));
    }

    void constrain(float imageW, float imageH, float windowW, float windowH) {
        const float scale = fit(imageW, imageH, windowW, windowH) * zoom;
        const float width = imageW * scale, height = imageH * scale;
        // Keep a visible strip so a dragged image can always be found again.
        const float limitX = std::max(0.0f, (windowW + width) * 0.5f - std::min(64.0f, width * 0.5f));
        const float limitY = std::max(0.0f, (windowH + height) * 0.5f - std::min(64.0f, height * 0.5f));
        panX = std::clamp(panX, -limitX, limitX);
        panY = std::clamp(panY, -limitY, limitY);
    }

    void wheel(float steps, float imageW, float imageH, float windowW, float windowH) {
        if (!std::isfinite(steps) || steps == 0.0f) return;
        const float next = std::clamp(zoom * std::pow(1.2f, std::clamp(steps, -32.0f, 32.0f)), 0.1f, 16.0f);
        const float ratio = next / zoom;
        const float anchorX = pointerKnown ? pointerX - windowW * 0.5f : 0.0f;
        const float anchorY = pointerKnown ? pointerY - windowH * 0.5f : 0.0f;
        panX = anchorX - (anchorX - panX) * ratio;
        panY = anchorY - (anchorY - panY) * ratio;
        zoom = next;
        constrain(imageW, imageH, windowW, windowH);
    }
};

} // namespace neo
