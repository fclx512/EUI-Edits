#pragma once

#include "state/app_actions.h"
#include "ui/widgets.h"
#include "ui/metrics.h"

#include <algorithm>
#include <cmath>

namespace neo {

// A read-only replacement for the editor region, not a window-level overlay.
// Each image tab owns only viewport metadata; decoded textures use the existing cache.
// Zoom/pan are direct manipulation: no animation, image copies or timers.
inline void imagePreviewView(eui::Ui& ui, AppState& state, float width, float height) {
    if (state.imagePreviewPath.empty()) return;
    const auto& colors = editorColors();
    const auto metrics = uiMetrics(state);
    const float font = metrics.menuFontSize;
    const float control = font + 14.0f;
    const float header = control + 12.0f;
    const float footer = control + 16.0f;
    if (state.unavailable) {
        ui.column("imagePreview.error").size(width,height).padding(24).gap(12).content([&] {
            ui.text("imagePreview.error.message").size(std::max(0.0f,width-48),100)
                .text(state.loadError).fontSize(font).color(colors.textMuted).wrap(true).build();
            toolButton(ui,"imagePreview.retry",i18n::tr("tabs.retry"),120,false,true,
                       [&state] { retryUnavailableDocument(state); },font);
        }).build();
        return;
    }
    const float canvasHeight = std::max(1.0f, height - header - footer);
    const float canvasWidth = std::max(1.0f, width);
    auto& view = state.imageViewport;
    view.constrain(state.imagePreviewWidth, state.imagePreviewHeight, canvasWidth, canvasHeight,
                   ImageViewport::PanBounds::Window);
    const float scale = view.fit(state.imagePreviewWidth, state.imagePreviewHeight, canvasWidth, canvasHeight) * view.zoom;
    const float imageWidth = std::max(1.0f, state.imagePreviewWidth * scale);
    const float imageHeight = std::max(1.0f, state.imagePreviewHeight * scale);
    const auto zoom = [&state, canvasWidth, canvasHeight](float steps) {
        state.imageViewport.wheel(steps, state.imagePreviewWidth, state.imagePreviewHeight,
                                  canvasWidth, canvasHeight, 16.0f, ImageViewport::PanBounds::Window);
        app::requestUpdate();
    };

    ui.stack("imagePreview").size(width, height).clip().content([&] {
        ui.rect("imagePreview.background").fill().color(colors.editor).build();
        ui.stack("imagePreview.header").size(width, header).content([&] {
            ui.rect("imagePreview.header.bg").fill().color(colors.toolbar).build();
            iconView(ui, "imagePreview.file.icon", UiIcon::Image, 12, (header-18)*.5f, 18, colors.textMuted);
            ui.text("imagePreview.name").position(40, 0).size(std::max(0.0f, width-control-60), header)
                .text(elideToWidth(textfile::fileName(state.imagePreviewPath),
                                  std::max(0.0f, width-control-60), font))
                .fontSize(font).color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
            ui.stack("imagePreview.close").position(std::max(0.0f, width-control-8), 6)
                .size(control, control).content([&] {
                    ui.rect("imagePreview.close.hit").fill().radius(neo::corner(colors, 6))
                        .states(transparentColor(), colors.rowHover, colors.pressed)
                        .preserveFocusOnPress().cursor(eui::CursorShape::Hand)
                        .transition(quickTransition()).onClick([&state] { closeImagePreview(state); }).build();
                    iconView(ui, "imagePreview.close.icon", UiIcon::Close,
                             (control-18)*.5f, (control-18)*.5f, 18, colors.textMuted);
                }).build();
        }).build();

        ui.stack("imagePreview.canvas").position(0, header).size(canvasWidth, canvasHeight).clip().content([&] {
            ui.image("imagePreview.img")
                .position((canvasWidth-imageWidth)*.5f+view.panX, (canvasHeight-imageHeight)*.5f+view.panY)
                .size(imageWidth, imageHeight).source(state.imagePreviewPath).contain().build();
            ui.rect("imagePreview.hit").size(canvasWidth, canvasHeight).zIndex(1).color(transparentColor())
                .acceptedButtons(core::PointerButton::Left | core::PointerButton::Middle).dragThreshold(0)
                .onDrag([&state, canvasWidth, canvasHeight](const core::dsl::DragEvent& event) {
                    if (event.button != core::PointerButton::Left && event.button != core::PointerButton::Middle) return;
                    auto& viewport = state.imageViewport;
                    viewport.panX += static_cast<float>(event.deltaX);
                    viewport.panY += static_cast<float>(event.deltaY);
                    viewport.constrain(state.imagePreviewWidth, state.imagePreviewHeight, canvasWidth, canvasHeight,
                                       ImageViewport::PanBounds::Window);
                })
                .onScroll([zoom](const core::ScrollEvent& event) { zoom(static_cast<float>(event.y)); })
                .onKeyEvent([&state](const core::KeyEvent& event) {
                    if (event.isDown() && event.key == core::InputKey::Escape) {
                        if (state.openMenu != MenuKind::None || state.vaultContextMenuOpen ||
                            state.contextMenuOpen || state.languageMenuOpen || state.tabListOpen)
                            return false;
                        closeImagePreview(state);
                        return true;
                    }
                    // Let app shortcuts navigate/open/close normally. Printable
                    // keys stay here and cannot edit the hidden manuscript.
                    return !event.modifiers.shortcut();
                })
                .onTextInput([](const core::TextInputEvent& event) {
                    if (event.composing || event.compositionChanged)
                        core::window::cancelImeComposition(core::window::mainWindowHandle());
                })
                .onContextMenu([](const core::PointerEvent&, const core::Rect&) {})
                .cursor(eui::CursorShape::Hand).build();
        }).build();

        const float buttonWidth = control+4;
        const float percentWidth = font*4.5f;
        const float fitWidth = std::max(76.0f, estimateTextWidth(i18n::tr("image.fit"), font)+20);
        const float controlsWidth = buttonWidth*2+percentWidth+fitWidth+18;
        ui.stack("imagePreview.footer").position(0, height-footer).size(width, footer).content([&] {
            ui.rect("imagePreview.footer.bg").fill().color(colors.toolbar).build();
            ui.row("imagePreview.controls").position(std::max(4.0f, (width-controlsWidth)*.5f), 8)
                .size(controlsWidth, control).gap(6).content([&] {
                    toolButton(ui, "imagePreview.zoomOut", "−", buttonWidth, false, view.zoom > .1001f,
                               [zoom] { zoom(-1); }, font);
                    ui.text("imagePreview.percent").size(percentWidth, control)
                        .text(std::to_string(static_cast<int>(std::lround(scale*100)))+"%")
                        .fontSize(font).color(colors.textMuted).horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center).build();
                    toolButton(ui, "imagePreview.zoomIn", "+", buttonWidth, false, view.zoom < 15.999f,
                               [zoom] { zoom(1); }, font);
                    toolButton(ui, "imagePreview.fit", i18n::tr("image.fit"), fitWidth, false, true, [&state] {
                        state.imageViewport.zoom = 1;
                        state.imageViewport.panX = state.imageViewport.panY = 0;
                        app::requestUpdate();
                    }, font);
                }).build();
        }).build();
    }).build();
    if (view.focusPending) {
        view.focusPending = false;
        ui.requestFocus("imagePreview.hit");
    }
}

} // namespace neo
