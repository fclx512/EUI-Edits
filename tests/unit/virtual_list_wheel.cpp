#include "components/virtuallist.h"
#include "core/app/dsl_window_runtime.h"

#include <chrono>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void verifyWheelQueue() {
    int tag = 0, otherTag = 0;
    auto window = reinterpret_cast<core::window::Handle>(&tag);
    auto other = reinterpret_cast<core::window::Handle>(&otherTag);
    for (int i = 0; i < 25000; ++i) core::queueScrollInput(window, .25, -1);
    const auto event = core::consumeScrollInput(window);
    check(event.x == 6250 && event.y == -25000, "wheel burst is aggregated without losing magnitude");
    check(!core::consumeScrollInput(window).active(), "wheel aggregate is consumed exactly once");
    check(!core::consumeScrollInput(other).active(), "wheel input remains isolated per window");
    core::releaseInputQueue(window); core::releaseInputQueue(other);
}

void verifyVirtualWheel(int ticksPerFrame, float deltaSeconds) {
    int tag = 0;
    auto window = reinterpret_cast<core::window::Handle>(&tag);
    core::dsl::Runtime runtime; runtime.initialize();
    float offset = 0;
    int composedRows = 0, maxRows = 0;
    std::int64_t firstRow = 0, lastRow = 0;
    constexpr int count = 10001;
    constexpr float rowHeight = 22, height = 600;
    constexpr float maximum = count * rowHeight - height;
    app::DslWindowRequest request;
    request.pageId = "virtual-wheel";
    request.compose = [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        composedRows = 0;
        components::virtualList(ui, "list").position(10, 10).size(250, height)
            .itemCount(count).rowHeight(rowHeight).step(78).offset(offset).overscanViewports(.5f)
            .transition(core::Transition::none()).onChange([&](float value) { offset = value; })
            .row([&](core::dsl::Ui& rowUi, const std::string& id, std::int64_t index, float width, float h) {
                if (!composedRows) firstRow = index;
                lastRow = index; ++composedRows;
                rowUi.rect(id + ".hit").size(width, h).states(core::Color{}, core::Color{}, core::Color{}).build();
            }).build();
        maxRows = std::max(maxRows, composedRows);
    };
    app::ResizeComposeThrottle throttle;
    bool composed = false, paint = false;
    float width = 0, frameHeight = 0;
    app::detail::WindowUpdateArgs args;
    args.window = window; args.logicalWidth = 300; args.logicalHeight = 650;
    args.deltaSeconds = deltaSeconds;
    auto frame = [&] {
        args.updateRequested = core::platform::consumeUiUpdate();
        core::platform::consumeFrameRequest();
        paint = false;
        app::detail::updateWindowFrame(runtime, request, throttle, composed, width, frameHeight, paint, args);
    };
    frame();
    core::queuePointerMotion(window, 100, 300, {}, {});
    frame();
    float previous = offset;
    int stalledFrames = 0, frames = 0;
    const auto begin = std::chrono::steady_clock::now();
    // Simulated frame/input schedules, not a native injection frequency or
    // real-machine frame-time benchmark. Drive far beyond the old flat-016xx stall.
    for (; frames < 12000 && offset < maximum; ++frames) {
        for (int tick = 0; tick < ticksPerFrame; ++tick) core::queueScrollInput(window, 0, -1);
        frame();
        if (offset <= previous && offset < maximum) ++stalledFrames;
        check(offset >= previous && offset <= maximum, "forward wheel stays monotonic and within bounds");
        previous = offset;
    }
    frame(); // consume any final virtual-range rebuild request
    check(stalledFrames == 0, "held forward wheel never stalls before boundary");
    check(std::fabs(offset - maximum) < .01f, "wheel reaches the true offset boundary");
    check(lastRow == count - 1, "final composed row is the actual last item, not a repeated screenshot");
    check(maxRows <= 58, "virtualization keeps row construction bounded by viewport and overscan");
    const auto atEnd = throttle.composeCount();
    for (int i = 0; i < 30; ++i) {
        core::queueScrollInput(window, 0, -25); frame();
    }
    check(offset == maximum && throttle.composeCount() == atEnd, "outward wheel at boundary does not rebuild rows");
    core::queueScrollInput(window, 0, 25); frame(); frame();
    check(offset < maximum && firstRow < count - 1, "reverse wheel resumes immediately from boundary");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count();
    std::cout << "ticks_per_frame=" << ticksPerFrame << " dt=" << deltaSeconds
              << " frames=" << frames << " max_rows=" << maxRows
              << " compose=" << throttle.composeCount() << " stalled=" << stalledFrames
              << " diagnostic_ms=" << ms << '\n';
    core::releaseInputQueue(window);
    core::platform::consumeUiUpdate(); core::platform::consumeFrameRequest();
}
}

int main() {
    verifyWheelQueue();
    verifyVirtualWheel(1, 1.0f / 120);
    verifyVirtualWheel(25, 1.0f / 120);
    verifyVirtualWheel(25, .003f);
    std::cout << (failures ? "FAIL" : "PASS") << ": virtual_list_wheel (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
