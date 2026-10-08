#include "components/input.h"
#include "core/app/dsl_window_runtime.h"

#include <chrono>
#include <iostream>
#include <string>

namespace {
using Model = components::input_detail::InputModel;
using State = Model::InputState;
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

// Run the real host compose/update/zero-delta-update path, with the real
// InputBuilder. Disable caret timers so only the gesture can request compose.
void verifyInputGesture(bool multiline) {
    int windowTag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&windowTag);
    core::dsl::Runtime runtime;
    runtime.initialize();
    State* state = nullptr;
    app::DslWindowRequest request;
    request.pageId = "drag-feedback";
    std::string text;
    for (int i = 0; i < 24000; ++i) text += "a中🙂";
    if (multiline) for (int i = 0; i < 200; ++i) text += "\nsecond 中🙂 line";
    request.compose = [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        state = &ui.state<State>("editor");
        components::input(ui, "editor").position(20, 20).size(500, 120)
            .inset(8).fontFamily("monospace").fontSize(16)
            .value(text).multiline(multiline).wordWrap(false).caretBlink(false)
            .transition(core::Transition::none()).build();
    };
    app::ResizeComposeThrottle throttle;
    bool composed = false, paintRequested = false;
    float width = 0, height = 0;
    app::detail::WindowUpdateArgs args;
    args.window = window;
    args.logicalWidth = 640;
    args.logicalHeight = 240;
    args.deltaSeconds = 1.0f / 120.0f;
    auto frame = [&] {
        paintRequested = false;
        return app::detail::updateWindowFrame(runtime, request, throttle, composed,
                                             width, height, paintRequested, args);
    };
    frame();
    core::queuePointerButton(window, 45, 38, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    frame();
    core::queuePointerMotion(window, 210, 38, core::PointerButton::Left, {});
    frame();
    const int cursor = state->cursor;
    const auto selection = Model::selectionRange(*state);
    check(selection.first < selection.second, "moving a held pointer selects text");
    check(Model::clampUtf8Boundary(text, cursor) == cursor, "drag ends on a UTF-8 boundary");
    const int before = throttle.composeCount();
    int paints = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 60; ++i) {
        frame();
        paints += paintRequested ? 1 : 0;
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "held-" << (multiline ? "multiline" : "singleline")
              << " compose=" << throttle.composeCount() - before
              << " paint=" << paints << " updates=60 ms=" << ms << '\n';
    check(throttle.composeCount() == before, "unchanged held selection must not recompose");
    check(paints == 0, "unchanged held selection must not request paint");
    check(state->cursor == cursor && Model::selectionRange(*state) == selection,
          "stationary ticks preserve selection");
    // Reverse direction, then release: suppressing idle feedback must not stop
    // actual motion or gesture completion.
    core::queuePointerMotion(window, 70, 38, core::PointerButton::Left, {});
    frame();
    check(state->cursor < cursor && throttle.composeCount() > before,
          "reverse drag updates the selection immediately");
    core::queuePointerButton(window, 70, 38, core::PointerButton::Left,
                             core::PointerAction::Release, {});
    frame();
    check(!state->selecting, "release completes selection");
    if (multiline) {
        core::queuePointerButton(window, 45, 38, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        frame();
        core::queuePointerMotion(window, 150, 175, core::PointerButton::Left, {});
        const int beforeOutside = throttle.composeCount();
        frame();
        const float firstScroll = state->verticalScroll;
        check(firstScroll > 0 && throttle.composeCount() > beforeOutside,
              "outside-viewport drag still follows the cursor and requests compose");
        // This fixture reaches the end. The last static hit must settle rather
        // than refreshing indefinitely; this is not an auto-scroll rate test.
        for (int i = 0; i < 4; ++i) frame();
        const int beforeAtEnd = throttle.composeCount();
        frame();
        check(throttle.composeCount() == beforeAtEnd,
              "outside-viewport selection at document end eventually settles");
        core::cancelPointerInput(window);
        frame();
        check(!state->selecting, "cancel ends captured selection");
    }
    runtime.shutdown();
    core::releaseInputQueue(window);
}

void verifyCallbackContract(bool feedback) {
    int windowTag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&windowTag);
    core::dsl::Runtime runtime;
    runtime.initialize();
    int calls = 0;
    bool changed = false;
    runtime.compose("drag-contract", 320, 180, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        auto target = ui.rect("target").position(20, 20).size(200, 100);
        if (feedback) {
            // The last registration wins, as it does for other event callbacks.
            target.onDrag([](const core::dsl::DragEvent&) {})
                  .onDragUpdate([&](const core::dsl::DragEvent&) { ++calls; return changed; });
        } else {
            target.onDragUpdate([](const core::dsl::DragEvent&) { return false; })
                  .onDrag([&](const core::dsl::DragEvent&) { ++calls; });
        }
        target.build();
    });
    core::queuePointerButton(window, 40, 40, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    core::queuePointerMotion(window, 100, 40, core::PointerButton::Left, {});
    runtime.update(window, 0, 1, 1);
    const int before = calls;
    for (int i = 0; i < 10; ++i) {
        runtime.update(window, 0, 1, 1);
        check(runtime.composeRequested() == !feedback,
              "feedback suppresses compose; legacy drag still requests it");
    }
    check(calls == before + 10, "stationary drag ticks remain dispatched for both APIs");
    if (feedback) {
        changed = true;
        runtime.update(window, 0, 1, 1);
        check(runtime.composeRequested(), "true feedback requests immediate compose");
    }
    core::cancelPointerInput(window);
    runtime.update(window, 0, 1, 1);
    const int afterCancel = calls;
    runtime.update(window, 0, 1, 1);
    check(calls == afterCancel, "cancel stops drag dispatch");
    runtime.shutdown();
    core::releaseInputQueue(window);
}
} // namespace

int main() {
    verifyInputGesture(false);
    verifyInputGesture(true);
    verifyCallbackContract(false);
    verifyCallbackContract(true);
    return failures ? 1 : 0;
}
