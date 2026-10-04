#include "apps/neo_editor/model/image_viewport.h"

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
    check(near(view.zoom, 16), "upper zoom bound");
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
    view = neo::ImageViewport{};
    check(view.zoom == 1 && view.panX == 0 && view.panY == 0 && !view.pointerKnown && view.focusPending, "reopen resets view and acquires focus");
    std::printf("image viewport: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
