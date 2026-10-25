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
    state.imagePreviewPath = "fixture.png";
    state.imagePreviewWidth = 800;
    state.imagePreviewHeight = 600;
    ui.begin("image.ui.test");
    neo::imagePreviewOverlay(ui, state, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
    const auto* imageHit = ui.find("imagePreview.hit");
    const auto* image = ui.find("imagePreview.img");
    const auto* imageClose = ui.find("imagePreview.close.hit");
    check(imageHit && imageHit->zIndex > 2600 && imageHit->zIndex < 3000,
          "Image preview must receive input above menus and tab popovers, below window controls");
    check(image && std::abs(image->frame.x + image->frame.width*.5f-screen.width*.5f)<.01f &&
          std::abs(image->frame.y + image->frame.height*.5f-screen.height*.5f)<.01f,
          "New image is centered without a pointer-dependent offset");
    if (imageHit && imageClose) {
        click.x = 500; click.y = 15;
        imageHit->onPress(click, core::Rect{0, 0, screen.width, screen.height});
        check(!state.imagePreviewPath.empty(), "Left press must not dismiss the image");
        core::dsl::DragEvent drag;
        drag.button=core::PointerButton::Left;drag.deltaX=48;drag.deltaY=-24;
        imageHit->onDrag(drag);
        check(state.imageViewport.panX==0 && state.imageViewport.panY==0,
              "A fitted image remains centered when dragged");
        state.imageViewport.zoom=2;
        imageHit->onDrag(drag);
        check(state.imageViewport.panX==48 && state.imageViewport.panY==-24 && !state.dirty(),
              "Left drag moves only an enlarged image viewport without editing the document");
        check(imageClose->frame.x>screen.width-60 && imageClose->frame.y<100,
              "Image has an explicit upper-right close control");
        imageClose->onClick();
        check(state.imagePreviewPath.empty() && state.imageViewport.panX==0 && state.imageViewport.panY==0 &&
              state.findEditorFocusPending,"Close button resets the view and restores editor focus");
        state.imagePreviewPath="fixture.png";state.imagePreviewWidth=800;state.imagePreviewHeight=600;
        ui.begin("image.ui.test");neo::imagePreviewOverlay(ui,state,screen);ui.end();ui.layout(screen.width,screen.height);
        core::KeyEvent escape;escape.key=core::InputKey::Escape;
        ui.find("imagePreview.hit")->onKeyEvent(escape);
        check(state.imagePreviewPath.empty(),"Escape closes the image without a click");
    }
    return failures ? 1 : 0;
}
