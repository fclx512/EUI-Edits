#pragma once

#include <algorithm>
#include <cmath>

namespace neo {

// Zoom is relative to the initial fit-to-window view, never to decoded pixels.
struct ImageViewport {
    enum class PanBounds { RecoverableCanvas, Window };
    float zoom = 1.0f;
    float panX = 0.0f, panY = 0.0f;
    float pointerX = 0.0f, pointerY = 0.0f;
    bool pointerKnown = false;
    bool focusPending = true;

    float fit(float imageW, float imageH, float windowW, float windowH) const {
        return std::min(std::max(1.0f, windowW * 0.92f) / std::max(1.0f, imageW),
                        std::max(1.0f, windowH * 0.92f) / std::max(1.0f, imageH));
    }

    void constrain(float imageW, float imageH, float windowW, float windowH,
                   PanBounds bounds = PanBounds::RecoverableCanvas) {
        const float scale = fit(imageW, imageH, windowW, windowH) * zoom;
        const float width = imageW * scale, height = imageH * scale;
        // Keep a visible strip so a dragged image can always be found again.
        // Photos expose only their overflowing viewport. Canvas users such as
        // mindmaps retain their existing recoverable free-panning policy.
        const float limitX = bounds == PanBounds::Window ? std::max(0.0f, (width-windowW)*0.5f)
            : std::max(0.0f, (windowW + width) * 0.5f - std::min(64.0f, width * 0.5f));
        const float limitY = bounds == PanBounds::Window ? std::max(0.0f, (height-windowH)*0.5f)
            : std::max(0.0f, (windowH + height) * 0.5f - std::min(64.0f, height * 0.5f));
        panX = std::clamp(panX, -limitX, limitX);
        panY = std::clamp(panY, -limitY, limitY);
    }

    void wheel(float steps, float imageW, float imageH, float windowW, float windowH,
               float maximumZoom = 16.0f, PanBounds bounds = PanBounds::RecoverableCanvas) {
        if (!std::isfinite(steps) || steps == 0.0f) return;
        if (!std::isfinite(maximumZoom) || maximumZoom < 0.1f) return;
        const float next = std::clamp(zoom * std::pow(1.2f, std::clamp(steps, -32.0f, 32.0f)), 0.1f, maximumZoom);
        const float ratio = next / zoom;
        const bool pointerAnchor = pointerKnown && bounds != PanBounds::Window;
        const float anchorX = pointerAnchor ? pointerX - windowW * 0.5f : 0.0f;
        const float anchorY = pointerAnchor ? pointerY - windowH * 0.5f : 0.0f;
        panX = anchorX - (anchorX - panX) * ratio;
        panY = anchorY - (anchorY - panY) * ratio;
        zoom = next;
        constrain(imageW, imageH, windowW, windowH, bounds);
    }
};

} // namespace neo
