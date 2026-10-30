#include "ui/mindmap_preview.h"
#include "ui/image_preview.h"
#include <cmath>
#include <iostream>

namespace app {
void requestUpdate() {}
}
namespace neo {
void showToast(AppState&, std::string, std::string) {}
float effectiveWindowScale() { return 1.0f; }
// This fixture checks viewer controls; real tab removal is in file_safety.
void requestCloseTab(AppState& state, std::uint64_t) {
    state.imagePreviewPath.clear();state.imageViewport=ImageViewport{};
    state.findEditorFocusPending=true;
}
void retryUnavailableDocument(AppState&) {}
}

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::cerr << label << '\n'; }
    };
    neo::AppState state;
    state.newDocumentLanguage = "markdown";
    state.doc.text = "```mermaid\nmindmap\n  root((First))\n    Child\n```\n\n```mermaid\nmindmap\n  root((Second))\n    Leaf\n```\n";
    core::dsl::Ui ui;
    ui.begin("mindmap.ui.test");
    auto& input = ui.state<components::input_detail::InputModel::InputState>(neo::editorInputId(state));
    input.text = state.doc.text;
    input.cursor = static_cast<int>(input.text.find("First"));
    neo::lp::cachedPlan(state.doc.text);
    const eui::Screen screen{1000.0f, 700.0f};
    const auto compose = [&] {
        ui.begin("mindmap.ui.test");
        neo::mindmapPreviewOverlay(ui, state, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
    };
    neo::openMindmapPreview(ui, state);
    check(state.mindmapPreview.open, "Cold open must succeed");
    compose();
    check(state.derivedMindmap && state.derivedMindmap->valid, "Cold compose must build layout");
    const auto* hit = ui.find("mindmapPreview.hit");
    const auto* panel = ui.find("mindmapPreview.panel");
    const auto* label = ui.find("mindmapPreview.node.0.label");
    check(hit && panel && label, "Preview tree must include hit panel and label");
    if (!hit || !panel || !label) return 1;
    check(hit->zIndex > 2600 && hit->zIndex < 3000,
          "Full-window preview must receive input above menus and tab popovers, below window controls");
    check(panel->clip, "Map must be clipped to panel");
    check(label->fontSize == neo::uiMetrics(state).editorFontSize, "Zoom must not change font raster size");
    core::PointerEvent click;
    click.button = core::PointerButton::Left;
    click.x = 500; click.y = 350;
    hit->onPress(click, core::Rect{0, 0, screen.width, screen.height});
    check(state.mindmapPreview.open, "Panel interior must not close preview");
    state.mindmapPreview.viewport.zoom = 3;
    state.mindmapPreview.viewport.panX = 100;
    click.x = panel->frame.x + panel->frame.width - 140;
    click.y = panel->frame.y + 16;
    hit->onPress(click, core::Rect{0, 0, screen.width, screen.height});
    check(state.mindmapPreview.viewport.zoom == 1 && state.mindmapPreview.viewport.panX == 0,
          "Fit hit route must reset viewport");
    const auto firstBytes = state.derivedCacheBytes;
    for (int round = 0; round < 4; ++round) {
        neo::closeMindmapPreview(state);
        input.cursor = static_cast<int>(input.text.find(round % 2 ? "First" : "Second"));
        neo::openMindmapPreview(ui, state);
        compose();
        check(state.derivedCacheBytes == state.derivedMindmap->bytes,
              "Switching blocks must replace cache accounting rather than accumulate");
    }
    check(state.derivedCacheBytes < firstBytes * 2, "Repeated cold reopen must not inflate accounted bytes");
    hit = ui.find("mindmapPreview.hit");
    click.x = 0; click.y = 0;
    hit->onPress(click, core::Rect{0, 0, screen.width, screen.height});
    check(!state.mindmapPreview.open && !state.dirty(), "Outside close must preserve clean document");
    // Embed at a nonzero editor origin, with a library and caption outside it.
    // The image canvas must not intercept those independent controls.
    const auto composeImage = [&] {
        ui.begin("mindmap.ui.test");
        ui.stack("editor.host").position(240,34).size(760,640).content([&] {
            neo::imagePreviewView(ui, state, 760, 640);
        }).build();
        ui.end(); ui.layout(1000,700);
    };
    state.imagePreviewPath = "fixture.png";
    state.imagePreviewWidth = 800; state.imagePreviewHeight = 600;
    components::input_detail::InputModel::insertAtCursor(input, "保留撤销");
    const auto undoCount = input.undoStack.size();
    const auto originalText = input.text;
    const auto originalCursor = input.cursor;
    composeImage();
    const auto* imageRoot = ui.find("imagePreview");
    const auto* canvas = ui.find("imagePreview.canvas");
    const auto* imageHit = ui.find("imagePreview.hit");
    const auto* image = ui.find("imagePreview.img");
    const auto* imageClose = ui.find("imagePreview.close.hit");
    check(imageRoot && canvas && imageHit && image && imageClose, "Embedded image tree must be complete");
    if (!imageRoot || !canvas || !imageHit || !image || !imageClose) return 1;
    check(imageRoot->frame.x==240 && imageRoot->frame.y==34 && imageRoot->frame.width==760 &&
          imageRoot->frame.height==640 && imageRoot->clip && canvas->clip && imageHit->zIndex<1100,
          "Image is clipped inside the editor, below menus, outside library/caption/status bar");
    check(std::abs(image->frame.x+image->frame.width*.5f-(canvas->frame.x+canvas->frame.width*.5f))<.01f &&
          std::abs(image->frame.y+image->frame.height*.5f-(canvas->frame.y+canvas->frame.height*.5f))<.01f,
          "New image is centered in the canvas excluding header/footer");
    core::dsl::DragEvent drag;
    drag.button=core::PointerButton::Left; drag.deltaX=48; drag.deltaY=-24;
    imageHit->onDrag(drag);
    check(state.imageViewport.panX==0 && state.imageViewport.panY==0,
          "A fitted image remains centered when dragged");
    state.imageViewport.zoom=2;
    imageHit->onDrag(drag);
    check(state.imageViewport.panX==48 && state.imageViewport.panY==-24 && !state.dirty(),
          "Enlarged image pans without editing the manuscript");
    composeImage();
    ui.find("imagePreview.zoomIn.bg")->onClick();
    check(std::abs(state.imageViewport.zoom-2.4f)<.001f, "Zoom-in button uses the wheel's bounded model");
    composeImage();
    ui.find("imagePreview.zoomOut.bg")->onClick();
    check(std::abs(state.imageViewport.zoom-2)<.001f, "Zoom-out button reverses one step");
    composeImage();
    ui.find("imagePreview.fit.bg")->onClick();
    check(state.imageViewport.zoom==1 && state.imageViewport.panX==0 && state.imageViewport.panY==0,
          "Fit button resets zoom and both pan axes");
    composeImage();
    core::KeyEvent shortcut; shortcut.key=core::InputKey::O; shortcut.modifiers.control=true;
    check(!ui.find("imagePreview.hit")->onKeyEvent(shortcut), "Image focus lets application shortcuts navigate");
    core::KeyEvent escape; escape.key=core::InputKey::Escape;
    state.openMenu=neo::MenuKind::File;
    check(!ui.find("imagePreview.hit")->onKeyEvent(escape) && !state.imagePreviewPath.empty(),
          "Escape belongs to an open menu before the embedded viewer");
    state.openMenu=neo::MenuKind::None;
    ui.find("imagePreview.close.hit")->onClick();
    check(state.imagePreviewPath.empty() && state.imageViewport.panX==0 && state.imageViewport.panY==0 &&
          state.findEditorFocusPending, "Close button resets view and restores editor focus");
    composeImage();
    check(!ui.find("imagePreview") &&
          ui.state<components::input_detail::InputModel::InputState>(neo::editorInputId(state)).text==originalText &&
          ui.state<components::input_detail::InputModel::InputState>(neo::editorInputId(state)).cursor==originalCursor &&
          input.undoStack.size()==undoCount && undoCount>0,
          "Unmounting viewer preserves the hidden editor input state");
    state.imagePreviewPath="fixture.png"; state.imagePreviewWidth=800; state.imagePreviewHeight=600;
    composeImage();
    ui.find("imagePreview.hit")->onKeyEvent(escape);
    check(state.imagePreviewPath.empty(), "Escape returns from the image without modifying text");
    // Same controls must fit the narrow editor and use the selected style.
    for (auto style : {neo::UiStyle::Rounded,neo::UiStyle::Square}) {
        state.uiStyle=style; neo::setUiStyle(style);
        state.imagePreviewPath="fixture.png";
        ui.begin("mindmap.ui.test"); neo::imagePreviewView(ui,state,260,350); ui.end(); ui.layout(260,350);
        const auto* fit=ui.find("imagePreview.fit.bg");
        check(fit && fit->frame.x>=0 && fit->frame.x+fit->frame.width<=260 &&
              ((style==neo::UiStyle::Square)==(fit->radius==0)),
              "Bottom controls fit a narrow editor and inherit rounded/square style");
    }
    return failures ? 1 : 0;
}
