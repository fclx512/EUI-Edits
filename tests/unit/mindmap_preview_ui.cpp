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
    check(imageHit && imageHit->zIndex > 2600 && imageHit->zIndex < 3000,
          "Image preview must receive input above menus and tab popovers, below window controls");
    if (imageHit) {
        click.x = 500; click.y = 15;
        imageHit->onPress(click, core::Rect{0, 0, screen.width, screen.height});
        check(state.imagePreviewPath.empty(), "Caption-area preview click closes without editing the document");
    }
    return failures ? 1 : 0;
}
