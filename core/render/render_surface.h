#pragma once

#include "core/window/window_types.h"

namespace core::render {

struct RenderSurface {
    core::window::Handle window = nullptr;
    core::window::NativeWindowInfo native;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    float dpiScale = 1.0f;

    bool valid() const {
        return window != nullptr && framebufferWidth > 0 && framebufferHeight > 0 && dpiScale > 0.0f;
    }
};

} // namespace core::render
