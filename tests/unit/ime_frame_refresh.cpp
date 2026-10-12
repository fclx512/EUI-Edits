// Native Win32 HWND/HIMC regression for the IME caret anchor lifecycle:
//
//  1. update() that grants text focus must publish the candidate exclude /
//     composition window rect for the focused element in the same frame.
//  2. After update, compose()/layout may move the element imeRect (caret on a
//     new line, layout shift). Composing alone must not touch the native IME;
//     both Runtime::render overloads call updateImeCursorRect before drawing
//     and must refresh the native exclude rect from the new geometry.
//  3. Focus leaving text (element without an ime anchor) suspends publishing;
//     returning focus to a moved text field must republish the moved anchor.
//  4. A DPI scale change rescales the same logical anchor before push.
//
// The same batch ordering contract is covered headlessly: wheel dispatch runs
// after the pointer tree update, so a custom onMove anchor published in the
// batch is consumed by the element's onScroll (built-in scrollState keeps its
// pre-pointer dispatch order; this test only pins the custom-callback path).

#if defined(_WIN32) && defined(EUI_WINDOW_BACKEND_WIN32)

#include "core/dsl_runtime.h"
#include "core/window/win32_host.h"

#include <windows.h>
#include <imm.h>

#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

// core::window::setImeCursorRect stores the float rect as the native LONG rect
// {left, top, left+width, top+height} before placing composition/candidate.
RECT expectedImeRect(float x, float y, float width, float height) {
    RECT rect{};
    rect.left = static_cast<LONG>(x);
    rect.top = static_cast<LONG>(y);
    rect.right = static_cast<LONG>(x + width);
    rect.bottom = static_cast<LONG>(y + height);
    return rect;
}

bool rectsEqual(const RECT& lhs, const RECT& rhs) {
    return lhs.left == rhs.left && lhs.top == rhs.top &&
           lhs.right == rhs.right && lhs.bottom == rhs.bottom;
}

// The runtime pushes the anchor through ImmSetCompositionWindow and the
// candidate exclude rect through ImmSetCandidateWindow; both are readable
// back from the thread's HIMC, which makes the native side observable.
bool nativeImeRects(core::window::Handle window,
                    COMPOSITIONFORM& composition,
                    CANDIDATEFORM& candidate) {
    const HWND hwnd = static_cast<HWND>(window);
    HIMC context = ImmGetContext(hwnd);
    if (!check(context != nullptr, "test HWND has no IME context")) {
        return false;
    }
    const BOOL compositionOk = ImmGetCompositionWindow(context, &composition);
    candidate.dwIndex = 0;
    const BOOL candidateOk = ImmGetCandidateWindow(context, 0, &candidate);
    ImmReleaseContext(hwnd, context);
    if (!check(compositionOk && candidateOk,
               "ImmGetCompositionWindow/ImmGetCandidateWindow failed")) {
        return false;
    }
    return true;
}

// Renders without a real GPU surface; ensureRenderCache fails so both render
// overloads take their direct path after updateImeCursorRect.
class FakeRenderBackend final : public core::render::RenderBackend {
public:
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int, int) override { return false; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int, int, const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode,
                         const std::vector<core::Rect>&) override {}
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&, int, int) override {}
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}
};

struct FieldLayout {
    float x = 40.0f;
    float y = 60.0f;
    float imeY = 12.0f;
};

void composePage(core::dsl::Runtime& runtime, const FieldLayout& field) {
    runtime.compose("ime-frame-refresh", 320.0f, 240.0f,
                    [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        ui.rect("field")
            .position(field.x, field.y)
            .size(200.0f, 40.0f)
            .focusable()
            .imeRect(10.0f, field.imeY, 2.0f, 20.0f)
            .build();
        ui.rect("button")
            .position(40.0f, 150.0f)
            .size(100.0f, 30.0f)
            .focusable()
            .build();
    });
}

void press(core::window::Handle window, double x, double y) {
    core::queuePointerButton(window, x, y, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    core::queuePointerButton(window, x, y, core::PointerButton::Left,
                             core::PointerAction::Release, {});
}

void verifySameBatchMotionThenWheelOrder() {
    int tag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&tag);
    core::dsl::Runtime runtime;
    if (!check(runtime.initialize(), "headless runtime initialization failed")) {
        return;
    }

    double anchor = -1.0;
    std::vector<std::pair<char, double>> order;
    runtime.compose("motion-wheel-order", 400.0f, 300.0f,
                    [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        ui.rect("pad")
            .position(0.0f, 0.0f)
            .size(200.0f, 200.0f)
            .onMove([&](const core::PointerEvent& event, const core::Rect&) {
                anchor = event.x;
                order.emplace_back('m', anchor);
                return false;
            })
            .onScroll([&](const core::ScrollEvent&) {
                // Consumes whatever anchor the onMove pass published in this batch.
                order.emplace_back('s', anchor);
            })
            .build();
    });

    core::queuePointerMotion(window, 50.0, 30.0, {}, {});
    core::queueScrollInput(window, 0.0, -1.0);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);

    check(order.size() == 2,
          "a motion+wheel batch must dispatch exactly one custom onMove and one onScroll");
    if (order.size() == 2) {
        check(order[0].first == 'm' && std::fabs(order[0].second - 50.0) < 0.001,
              "pointer motion in the batch did not run the custom onMove");
        check(order[1].first == 's' && std::fabs(order[1].second - 50.0) < 0.001,
              "wheel dispatch must run after the pointer tree update so onScroll "
              "consumes the anchor updated by onMove in the same batch");
    }

    runtime.shutdown();
    core::releaseInputQueue(window);
}

void verifyNativeImeFrameRefresh(core::window::Handle window) {
    core::dsl::Runtime runtime;
    if (!check(runtime.initialize(window), "runtime initialization failed")) {
        return;
    }
    composePage(runtime, FieldLayout{});

    COMPOSITIONFORM composition{};
    CANDIDATEFORM candidate{};

    // Pressing into the field grants text focus and must publish the caret
    // anchor in the same update: logical caret (50, 72) 2x20 at dpi 1.0.
    press(window, 60.0, 70.0);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);
    if (nativeImeRects(window, composition, candidate)) {
        const RECT expected = expectedImeRect(50.0f, 72.0f, 2.0f, 20.0f);
        check(rectsEqual(candidate.rcArea, expected),
              "the focus update did not publish the native candidate exclude rect");
        check(composition.ptCurrentPos.x == expected.left &&
                  composition.ptCurrentPos.y == expected.top &&
                  rectsEqual(composition.rcArea, expected),
              "the focus update did not publish the native composition window rect");
    }

    // compose() moves the caret to the next line (imeRect y 12 -> 32). Compose
    // alone must not touch the native IME; the refresh belongs to render().
    FieldLayout nextLine;
    nextLine.imeY = 32.0f;
    composePage(runtime, nextLine);
    if (nativeImeRects(window, composition, candidate)) {
        check(rectsEqual(candidate.rcArea, expectedImeRect(50.0f, 72.0f, 2.0f, 20.0f)),
              "compose/layout alone already moved the native candidate exclude rect");
    }
    {
        FakeRenderBackend backend;
        core::render::ScopedRenderBackend scoped(backend);
        runtime.render(320, 240, 1.0f, core::Color{});
    }
    if (nativeImeRects(window, composition, candidate)) {
        check(rectsEqual(candidate.rcArea, expectedImeRect(50.0f, 92.0f, 2.0f, 20.0f)),
              "render did not refresh the native candidate exclude rect after the "
              "post-update layout change");
    }

    // The direct render overload (no clear color) carries the same contract.
    FieldLayout thirdLine;
    thirdLine.imeY = 52.0f;
    composePage(runtime, thirdLine);
    {
        FakeRenderBackend backend;
        core::render::ScopedRenderBackend scoped(backend);
        runtime.render(320, 240, 1.0f);
    }
    if (nativeImeRects(window, composition, candidate)) {
        check(rectsEqual(candidate.rcArea, expectedImeRect(50.0f, 112.0f, 2.0f, 20.0f)),
              "direct render overload did not refresh the native candidate exclude rect");
    }

    // Focus moves to a control without an ime anchor: publishing suspends and
    // the native rect stays where the text field left it.
    press(window, 60.0, 160.0);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);
    if (nativeImeRects(window, composition, candidate)) {
        check(rectsEqual(candidate.rcArea, expectedImeRect(50.0f, 112.0f, 2.0f, 20.0f)),
              "non-text focus published a stale or cleared candidate exclude rect");
    }

    // The text field moves while focus is elsewhere; regaining text focus must
    // republish the anchor at the moved position.
    FieldLayout moved;
    moved.x = 140.0f;
    moved.imeY = 12.0f;
    composePage(runtime, moved);
    press(window, 150.0, 70.0);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);
    if (nativeImeRects(window, composition, candidate)) {
        check(rectsEqual(candidate.rcArea, expectedImeRect(150.0f, 72.0f, 2.0f, 20.0f)),
              "restoring text focus did not republish the moved caret anchor");
    }

    // A DPI change rescales the same logical anchor: (150,72)x(2,20) at dpi 2.
    core::queuePointerMotion(window, 300.0, 160.0, {}, {});
    runtime.update(window, 1.0f / 60.0f, 1.0f, 2.0f);
    if (nativeImeRects(window, composition, candidate)) {
        check(rectsEqual(candidate.rcArea, expectedImeRect(300.0f, 144.0f, 4.0f, 40.0f)),
              "the DPI change did not rescale the native candidate exclude rect");
    }

    runtime.shutdown();
}

} // namespace

int main() {
    if (!core::window::win32::initialize()) {
        std::cerr << "Win32 host initialization failed\n";
        return 1;
    }

    verifySameBatchMotionThenWheelOrder();

    core::window::WindowCreateRequest request;
    request.width = 320;
    request.height = 240;
    request.title = "EUI-Edits IME frame refresh";
    request.decorated = false;
    request.resizable = false;
    request.renderApi = core::window::RenderApi::Native;
    const core::window::Handle window = core::window::createWindow(request);
    if (!window) {
        core::window::win32::shutdownHost();
        std::cerr << "Win32 test HWND creation failed\n";
        return 1;
    }
    core::window::win32::hide(static_cast<core::window::win32::Window*>(window));

    verifyNativeImeFrameRefresh(window);

    core::window::destroyWindow(window);
    core::window::win32::shutdownHost();

    std::cout << (failures ? "FAIL" : "PASS") << ": ime_frame_refresh ("
              << failures << " failures)\n";
    return failures ? 1 : 0;
}

#else

int main() {
    return 77;
}

#endif
