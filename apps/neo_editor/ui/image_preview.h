#pragma once

#include "state/app_state.h"

#include <algorithm>
#include <cmath>

namespace neo {

inline void closeImagePreview(AppState& state) {
    state.imagePreviewPath.clear();
    state.imagePreviewWidth = state.imagePreviewHeight = 0.0f;
    state.imageViewport = ImageViewport{};
    state.findEditorFocusPending = true;
    app::requestUpdate();
}

// Left click/Esc close; wheel zooms about the pointer and middle drag pans.
// One full-window interaction surface also blocks the underlying editor.
inline void imagePreviewOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    if (state.imagePreviewPath.empty() ||
        state.imagePreviewWidth <= 0.0f || state.imagePreviewHeight <= 0.0f) {
        return;
    }
    auto& view = state.imageViewport;
    view.constrain(state.imagePreviewWidth, state.imagePreviewHeight, screen.width, screen.height);
    const float scale = view.fit(state.imagePreviewWidth, state.imagePreviewHeight, screen.width, screen.height) * view.zoom;
    const float width = std::max(1.0f, state.imagePreviewWidth * scale);
    const float height = std::max(1.0f, state.imagePreviewHeight * scale);

    ui.stack("imagePreview")
        .size(screen.width, screen.height)
        .zIndex(1300)
        .clip()
        .content([&] {
            ui.rect("imagePreview.dim")
                .size(screen.width, screen.height)
                .zIndex(1300)
                .color(core::Color{0.0f, 0.0f, 0.0f, 0.85f})
                .build();
            ui.image("imagePreview.img")
                .zIndex(1301)
                .position((screen.width - width) * 0.5f + view.panX, (screen.height - height) * 0.5f + view.panY)
                .size(width, height)
                .source(state.imagePreviewPath)
                .contain()
                .radius(6.0f)
                .build();
            ui.rect("imagePreview.hit")
                .size(screen.width, screen.height)
                .zIndex(1302)
                .color(core::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .acceptedButtons(core::PointerButton::Left | core::PointerButton::Middle)
                .dragThreshold(0.0f)
                .onMove([&state](const core::PointerEvent& event, const core::Rect&) -> bool {
                    state.imageViewport.pointerX = static_cast<float>(event.x);
                    state.imageViewport.pointerY = static_cast<float>(event.y);
                    state.imageViewport.pointerKnown = true;
                    return false;
                })
                .onPress([&state](const core::PointerEvent& event, const core::Rect&) {
                    if (event.button != core::PointerButton::Left) return;
                    closeImagePreview(state);
                })
                .onDrag([&state, screen](const core::dsl::DragEvent& event) {
                    if (event.button != core::PointerButton::Middle) return;
                    state.imageViewport.panX += static_cast<float>(event.deltaX);
                    state.imageViewport.panY += static_cast<float>(event.deltaY);
                    state.imageViewport.constrain(state.imagePreviewWidth, state.imagePreviewHeight, screen.width, screen.height);
                })
                .onScroll([&state, screen](const core::ScrollEvent& event) {
                    state.imageViewport.wheel(static_cast<float>(event.y), state.imagePreviewWidth,
                                              state.imagePreviewHeight, screen.width, screen.height);
                })
                .onKeyEvent([&state](const core::KeyEvent& event) {
                    if (event.isDown() && event.key == core::InputKey::Escape) closeImagePreview(state);
                    return true;
                })
                // The native window keeps an IME context even for non-text
                // controls. Cancel any attempted preedit immediately so its
                // candidate window cannot consume Escape before this layer.
                .onTextInput([](const core::TextInputEvent& event) {
                    if (event.composing || event.compositionChanged) {
                        core::window::cancelImeComposition(core::window::mainWindowHandle());
                    }
                })
                .onContextMenu([](const core::PointerEvent&, const core::Rect&) {})
                .cursor(eui::CursorShape::Hand)
                .build();
        })
        .build();
    if (view.focusPending) {
        view.focusPending = false;
        ui.requestFocus("imagePreview.hit");
    }
}

} // namespace neo
