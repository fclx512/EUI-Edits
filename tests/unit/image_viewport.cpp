#include "apps/neo_editor/model/image_viewport.h"
#include <algorithm>

#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("FAIL: %s\n", label); ++failures; }
    };
    const auto near = [](float a, float b) { return std::abs(a - b) < 0.01f; };
    neo::ImageViewport view;
    check(near(view.fit(800, 400, 1000, 800), 1.15f), "fit retains aspect ratio");
    view.pointerKnown = true;
    view.pointerX = 600; view.pointerY = 450;
    view.wheel(1, 800, 400, 1000, 800);
    check(near(view.zoom, 1.2f) && near(view.panX, -20) && near(view.panY, -10), "pointer anchor stays fixed on zoom");
    view.wheel(-1, 800, 400, 1000, 800);
    check(near(view.zoom, 1) && near(view.panX, 0) && near(view.panY, 0), "zoom roundtrip");
    for (int i = 0; i < 100; ++i) view.wheel(32, 800, 400, 1000, 800);
    check(near(view.zoom, 16), "default image zoom remains capped at 16");
    for (int i = 0; i < 100; ++i) view.wheel(-32, 800, 400, 1000, 800);
    check(near(view.zoom, .1f), "lower zoom bound");
    view.panX = 1e9f; view.panY = -1e9f;
    view.constrain(800, 400, 1000, 800);
    check(near(view.panX, 500) && near(view.panY, -400), "small image remains recoverable after extreme drag");
    view.constrain(800, 400, 200, 120);
    check(near(view.panX, 100) && near(view.panY, -60), "resize reconstrains panning");
    const float before = view.zoom;
    view.wheel(std::numeric_limits<float>::infinity(), 800, 400, 200, 120);
    check(view.zoom == before, "nonfinite scroll ignored");

    // A large map's wheel cap can scale its fitted view back to normal world geometry.
    constexpr float mapWidth = 2400.0f;
    constexpr float mapHeight = 40000.0f;
    constexpr float windowWidth = 1000.0f;
    constexpr float windowHeight = 800.0f;
    neo::ImageViewport largeMap;
    largeMap.pointerKnown = true;
    largeMap.pointerX = 650.0f;
    largeMap.pointerY = 350.0f;
    const float adaptiveMaximum = std::max(16.0f, 1.0f / largeMap.fit(mapWidth, mapHeight,
                                                                     windowWidth, windowHeight));
    for (int i = 0; i < 20; ++i)
        largeMap.wheel(32.0f, mapWidth, mapHeight, windowWidth, windowHeight, adaptiveMaximum);
    check(near(largeMap.zoom, adaptiveMaximum) &&
          largeMap.fit(mapWidth, mapHeight, windowWidth, windowHeight) * largeMap.zoom >= 0.99f,
          "adaptive large-map cap permits readable world-scale geometry");

    const auto invalidCapLeavesState = [&](float cap) {
        neo::ImageViewport candidate;
        candidate.zoom = 2.0f;
        candidate.panX = 17.0f;
        candidate.panY = -23.0f;
        candidate.pointerKnown = true;
        candidate.pointerX = 640.0f;
        candidate.pointerY = 320.0f;
        candidate.wheel(4.0f, mapWidth, mapHeight, windowWidth, windowHeight, cap);
        return near(candidate.zoom, 2.0f) && near(candidate.panX, 17.0f) &&
               near(candidate.panY, -23.0f);
    };
    check(invalidCapLeavesState(std::numeric_limits<float>::quiet_NaN()),
          "NaN zoom cap leaves viewport state unchanged");
    check(invalidCapLeavesState(0.05f), "zoom cap below minimum leaves viewport state unchanged");

    // The photo viewer is centered per axis and cannot expose blank space by
    // panning beyond an enlarged image's edges. Canvas behavior above stays free.
    neo::ImageViewport photo;
    using Bounds=neo::ImageViewport::PanBounds;
    photo.panX=150;photo.panY=-75;
    photo.constrain(800,400,1000,800,Bounds::Window);
    check(near(photo.panX,0) && near(photo.panY,0),"fitted photo cannot be displaced by dragging");
    photo.pointerKnown=true;photo.pointerX=900;photo.pointerY=100;
    photo.wheel(1,800,400,1000,800,16,Bounds::Window);
    check(near(photo.panX,0) && near(photo.panY,0),"photo zoom keeps the window center regardless of pointer");
    photo.panX=500;photo.panY=500;
    photo.constrain(800,400,1000,800,Bounds::Window);
    check(near(photo.panX,52) && near(photo.panY,0),"only overflowing horizontal viewport can pan to its edge");
    photo.wheel(-1,800,400,1000,800,16,Bounds::Window);
    check(near(photo.zoom,1) && near(photo.panX,0) && near(photo.panY,0),"zooming back to fit recenters the photo");
    photo.zoom=2;photo.panX=500;photo.panY=-500;
    photo.constrain(400,800,1000,800,Bounds::Window);
    check(near(photo.panX,0) && near(photo.panY,-336),"portrait photo pans only in its overflowing vertical viewport");
    photo.constrain(400,800,400,2000,Bounds::Window);
    check(near(photo.panX,0) && near(photo.panY,0),"changed window aspect recenters the axis that now fits");

    view = neo::ImageViewport{};
    check(view.zoom == 1 && view.panX == 0 && view.panY == 0 && !view.pointerKnown && view.focusPending, "reopen resets view and acquires focus");
    std::printf("image viewport: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
