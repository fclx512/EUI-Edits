#pragma once

#include "state/app_state.h"
#include "ui/widgets.h"
#include "ui/icons.h"

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

// Images fit at the center; left drag pans only their overflowing viewport.
// Wheel zooms about the window center. Close explicitly with the
// upper-right button or Escape. The hidden wrapper primes a short opacity-only
// entry without shifting the centered image or animating direct manipulation.
inline void imagePreviewOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    const bool open = !state.imagePreviewPath.empty() &&
        state.imagePreviewWidth > 0.0f && state.imagePreviewHeight > 0.0f;
    auto& view = state.imageViewport;
    if (open) view.constrain(state.imagePreviewWidth, state.imagePreviewHeight, screen.width, screen.height,
                             ImageViewport::PanBounds::Window);
    const float scale = view.fit(state.imagePreviewWidth, state.imagePreviewHeight, screen.width, screen.height) * view.zoom;
    const float width = std::max(1.0f, state.imagePreviewWidth * scale);
    const float height = std::max(1.0f, state.imagePreviewHeight * scale);

    ui.stack("imagePreview")
        .size(screen.width, screen.height)
        .zIndex(2800)
        .disabled(!open)
        .opacity(open ? 1.0f : 0.0f)
        .transition(open && animationsEnabled(state)
            ? core::Transition::make(.16f, core::Ease::OutCubic).animate(core::AnimProperty::Opacity)
            : core::Transition::none())
        .clip()
        .content([&] {
            if (!open) return;
            ui.rect("imagePreview.dim")
                .size(screen.width, screen.height)
                .zIndex(2800)
                .color(core::Color{0.0f, 0.0f, 0.0f, 0.85f})
                .build();
            ui.image("imagePreview.img")
                .zIndex(2801)
                .position((screen.width - width) * 0.5f + view.panX, (screen.height - height) * 0.5f + view.panY)
                .size(width, height)
                .source(state.imagePreviewPath)
                .contain()
                .radius(neo::corner(editorColors(state.theme), 6.0f))
                .build();
            ui.rect("imagePreview.hit")
                .size(screen.width, screen.height)
                .zIndex(2802)
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
                    state.imageViewport.pointerX = static_cast<float>(event.x);
                    state.imageViewport.pointerY = static_cast<float>(event.y);
                    state.imageViewport.pointerKnown = true;
                })
                .onDrag([&state, screen](const core::dsl::DragEvent& event) {
                    if (event.button != core::PointerButton::Left && event.button != core::PointerButton::Middle) return;
                    state.imageViewport.panX += static_cast<float>(event.deltaX);
                    state.imageViewport.panY += static_cast<float>(event.deltaY);
                    state.imageViewport.constrain(state.imagePreviewWidth, state.imagePreviewHeight, screen.width, screen.height,
                                                  ImageViewport::PanBounds::Window);
                })
                .onScroll([&state, screen](const core::ScrollEvent& event) {
                    state.imageViewport.wheel(static_cast<float>(event.y), state.imagePreviewWidth,
                                              state.imagePreviewHeight, screen.width, screen.height,
                                              16.0f, ImageViewport::PanBounds::Window);
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
            ui.stack("imagePreview.close")
                .position(std::max(4.0f, screen.width - 48.0f), uiMetrics(state).menuBarHeight + 12.0f)
                .size(36.0f, 36.0f).zIndex(2803)
                .content([&] {
                    ui.rect("imagePreview.close.hit").fill()
                        .radius(neo::corner(editorColors(state.theme), 6.0f))
                        .states({0,0,0,.55f}, {0,0,0,.70f}, {0,0,0,.82f})
                        .preserveFocusOnPress().cursor(eui::CursorShape::Hand)
                        .transition(quickTransition())
                        .onClick([&state] { closeImagePreview(state); }).build();
                    iconView(ui, "imagePreview.close.icon", UiIcon::Close, 8, 8, 20, {1,1,1,.95f});
                }).build();
        })
        .build();
    if (open && view.focusPending) {
        view.focusPending = false;
        ui.requestFocus("imagePreview.hit");
    }
}

} // namespace neo
