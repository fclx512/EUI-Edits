#include "components/input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <string>
#include <utility>

namespace allocation_probe {
std::atomic<bool> enabled{false};
std::atomic<std::size_t> bytes{0};
std::atomic<std::size_t> count{0};

void begin() {
    bytes.store(0, std::memory_order_relaxed);
    count.store(0, std::memory_order_relaxed);
    enabled.store(true, std::memory_order_release);
}

void end() { enabled.store(false, std::memory_order_release); }

void record(std::size_t size) {
    if (enabled.load(std::memory_order_acquire)) {
        bytes.fetch_add(size, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
    }
}
}  // namespace allocation_probe

void* operator new(std::size_t size) {
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        allocation_probe::record(size);
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        allocation_probe::record(size);
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using Model = components::input_detail::InputModel;
using State = Model::InputState;
using Layout = Model::InputLayout;
using PointerEvent = core::PointerEvent;
using Rect = core::Rect;

constexpr float kWidth = 360.0f;
constexpr float kHeight = 120.0f;
constexpr float kInset = 8.0f;
constexpr float kFontSize = 16.0f;
constexpr float kTextTop = kFontSize * 0.1f + 2.0f + kFontSize * 0.1f;
constexpr float kTextHeight = kHeight - (kFontSize * 0.1f + 2.0f) * 2.0f - 12.0f;
constexpr float kTextWidth = kWidth - kInset * 2.0f;
constexpr float kLineHeight = kFontSize * 1.2f;
constexpr const char* kFont = "monospace";

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct CallbackSet {
    std::function<void(const PointerEvent&, const Rect&)> press;
    std::function<bool(const PointerEvent&, const Rect&)> move;
    std::function<void(const core::dsl::DragEvent&)> drag;
    std::function<void(const PointerEvent&, const Rect&)> release;
    std::function<core::CursorShape(const PointerEvent&, const Rect&)> cursorAt;
    std::function<void(const PointerEvent&, const Rect&)> contextMenu;
    std::function<void(const core::ScrollEvent&)> scroll;

    void copyFrom(const core::dsl::Element& element) {
        press = element.onPress;
        move = element.onMove;
        if (element.onDragUpdate) {
            drag = element.onDragUpdate;
        } else {
            drag = element.onDrag;
        }
        release = element.onRelease;
        cursorAt = element.cursorAt;
        contextMenu = element.onContextMenu;
        scroll = element.onScroll;
    }

    bool complete() const {
        return press && move && drag && release && cursorAt && contextMenu && scroll;
    }
};

struct Frame {
    core::dsl::Ui ui;
    State* state = nullptr;
    core::dsl::Element* hit = nullptr;
    Rect bounds;
};

void compose(Frame& frame, const std::string& text, bool multiline,
             bool resetDocument = true, int cursor = 0) {
    frame.ui.begin("callback-layout");
    frame.state = &frame.ui.state<State>("editor");
    if (resetDocument) {
        Model::loadDocument(*frame.state, text);
        frame.state->cursor = cursor;
        frame.state->selectionStart = cursor;
        frame.state->selectionEnd = cursor;
        frame.state->followCaret = false;
    }
    frame.state->wordWrap = false;

    components::input(frame.ui, "editor")
        .position(20.0f, 12.0f)
        .size(kWidth, kHeight)
        .inset(kInset)
        .fontSize(kFontSize)
        .fontFamily(kFont)
        .multiline(multiline)
        .wordWrap(false)
        .value(text)
        .onContextMenu([](float, float) {})
        .build();
    frame.ui.end();
    frame.ui.layout(core::dsl::Screen{800.0f, 240.0f});
    frame.hit = frame.ui.find("editor.hit");
    if (frame.hit) {
        frame.bounds = {frame.hit->frame.x, frame.hit->frame.y,
                        frame.hit->frame.width, frame.hit->frame.height};
    }
}

Layout makeOracle(State& state) {
    return Layout::build(state, kTextWidth, kTextHeight, kWidth, kInset, kInset,
                         kTextTop, kLineHeight, kFont, kFontSize, true);
}

PointerEvent eventAt(const Rect& bounds, const Layout& oracle, int byteOffset, int lineIndex = 0) {
    PointerEvent event;
    event.action = core::PointerAction::Press;
    event.button = core::PointerButton::Left;
    event.buttons = core::PointerButton::Left;

    const auto& lines = oracle.lineList();
    const int boundedLine = std::clamp(lineIndex, 0, static_cast<int>(lines.size()) - 1);
    const float scale = kWidth > 0.0f ? bounds.width / kWidth : 1.0f;
    const float x = kInset + oracle.xFor(byteOffset) - oracle.scroll;
    const float y = kTextTop + oracle.geometryTable().top(boundedLine) +
                    oracle.geometryTable().height(boundedLine) * 0.5f -
                    oracle.currentVerticalScroll;
    event.x = bounds.x + x * scale;
    event.y = bounds.y + y * scale;
    return event;
}

int oracleByteAt(const Rect& bounds, const Layout& oracle, const PointerEvent& event) {
    return oracle.pointerHit(event.x, event.y, bounds, kWidth, kInset).byteIndex;
}

void testCopiesDoNotCloneLongLineMetrics() {
    std::string text;
    text.reserve(192000);
    constexpr int repeats = 24000;  // 72,000 Unicode codepoints and 192,000 UTF-8 bytes.
    for (int i = 0; i < repeats; ++i) text += "a中🙂";

    Frame frame;
    compose(frame, text, true);
    check(frame.hit != nullptr, "the real InputBuilder must expose its hit element");
    if (!frame.hit) return;

    // Force all independent setup, element lookup, and metric construction to finish before
    // the probe. Only copying the seven interaction callbacks is measured below.
    State oracleState;
    Model::loadDocument(oracleState, text);
    oracleState.cursor = 0;
    oracleState.followCaret = false;
    oracleState.wordWrap = false;
    Layout oracle = makeOracle(oracleState);
    check(!oracle.lineList().empty() && oracle.lineList().front().metrics.caretX.size() > 64000,
          "the fixture must contain full metrics for one very long physical line");

    CallbackSet callbacks;
    const auto started = std::chrono::steady_clock::now();
    allocation_probe::begin();
    callbacks.copyFrom(*frame.hit);
    allocation_probe::end();
    const double copyMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    const std::size_t copiedBytes = allocation_probe::bytes.load(std::memory_order_relaxed);
    const std::size_t allocationCount = allocation_probe::count.load(std::memory_order_relaxed);
    std::cout << "seven callback copies: " << copiedBytes << " allocated bytes in "
              << allocationCount << " allocations, " << copyMs << " ms\n";
    check(callbacks.complete(), "all seven real input callbacks must be copyable");
    check(copiedBytes <= 64u * 1024u,
          "copying callbacks must not allocate a duplicate of the long line metrics");

    if (!callbacks.complete()) return;
    const int afterOneCluster = static_cast<int>(std::string("a中🙂").size());
    int moveChecks = 0;
    for (const int target : {0, afterOneCluster, afterOneCluster * 9}) {
        Layout currentOracle = makeOracle(oracleState);
        const PointerEvent event = eventAt(frame.bounds, currentOracle, target);
        const int expected = oracleByteAt(frame.bounds, currentOracle, event);
        callbacks.press(event, frame.bounds);
        check(frame.state->cursor == expected,
              "press callback must resolve UTF-8 byte boundaries like InputLayout::pointerHit");
        check(callbacks.cursorAt(event, frame.bounds) == core::CursorShape::IBeam,
              "cursor callback must retain the text-cursor hit behavior");
        const bool moved = callbacks.move(event, frame.bounds);
        if (moveChecks++ == 0) {
            check(moved, "move callback must report the initial hovered-line change");
        } else {
            check(!moved, "repeated move on the same line must not request redundant layout");
        }

        core::dsl::DragEvent drag;
        drag.x = event.x;
        drag.y = event.y;
        callbacks.drag(drag);
        const int dragExpected = oracleByteAt(frame.bounds, currentOracle, event);
        check(frame.state->cursor == dragExpected,
              "drag callback must keep pointer-to-byte mapping consistent with the model oracle");
        callbacks.release(event, frame.bounds);
        check(!frame.state->selecting, "release callback must end a pointer selection");

        frame.state->selectionStart = frame.state->selectionEnd = frame.state->cursor;
        callbacks.contextMenu(event, frame.bounds);
        check(frame.state->cursor == expected,
              "context-menu callback must place the caret at the model oracle byte offset");
    }

    const float beforeScroll = frame.state->horizontalScroll;
    core::ScrollEvent wheel;
    wheel.x = -3.0;
    callbacks.scroll(wheel);
    check(frame.state->horizontalScroll > beforeScroll,
          "horizontal scroll callback must update the live input state");

    // Retire the old callback closures before rebuilding; their retained geometry must not
    // affect the next frame. A fresh callback copy must observe that frame's new scroll offset.
    callbacks = {};
    compose(frame, text, true, false);
    check(frame.hit != nullptr, "the next real InputBuilder frame must still compose");
    if (!frame.hit) return;
    CallbackSet nextCallbacks;
    nextCallbacks.copyFrom(*frame.hit);
    check(nextCallbacks.complete(), "the next frame must expose all seven callbacks");
    State nextOracleState;
    Model::loadDocument(nextOracleState, text);
    nextOracleState.cursor = 0;
    nextOracleState.followCaret = false;
    nextOracleState.wordWrap = false;
    nextOracleState.horizontalScroll = frame.state->horizontalScroll;
    Layout nextOracle = makeOracle(nextOracleState);
    check(std::fabs(nextOracle.scroll - frame.state->horizontalScroll) < 0.01f,
          "the rebuilt model oracle must preserve the current horizontal offset");
    const int scrolledTarget = afterOneCluster * 8;
    const PointerEvent scrolledEvent = eventAt(frame.bounds, nextOracle, scrolledTarget);
    const int scrolledExpected = oracleByteAt(frame.bounds, nextOracle, scrolledEvent);
    Layout unscrolledOracle = makeOracle(nextOracleState);
    unscrolledOracle.scroll = 0.0f;
    const int unscrolledExpected = oracleByteAt(frame.bounds, unscrolledOracle, scrolledEvent);
    check(scrolledExpected != unscrolledExpected,
          "the scrolled hit fixture must distinguish new-frame scroll from the old offset");
    nextCallbacks.press(scrolledEvent, frame.bounds);
    check(frame.state->cursor == scrolledExpected,
          "the next frame callbacks must map hits using the new horizontal offset");
}

void testMultilineUtf8Oracle() {
    const std::string text = "A中🙂Z\n第二行🙂Ω\nend";
    Frame frame;
    compose(frame, text, true);
    check(frame.hit != nullptr, "multiline InputBuilder must expose its hit element");
    if (!frame.hit) return;
    CallbackSet callbacks;
    callbacks.copyFrom(*frame.hit);
    check(callbacks.complete(), "multiline no-wrap input must expose all seven callbacks");
    if (!callbacks.complete()) return;

    State oracleState;
    Model::loadDocument(oracleState, text);
    oracleState.cursor = 0;
    oracleState.followCaret = false;
    oracleState.wordWrap = false;
    Layout oracle = makeOracle(oracleState);
    const int secondLineStart = static_cast<int>(text.find("第二行"));
    const int safeOffsets[] = {
        0,
        static_cast<int>(std::string("A中").size()),
        static_cast<int>(std::string("A中🙂Z\n第二").size()),
        secondLineStart,
        static_cast<int>(text.find("end")) + 1,
        static_cast<int>(text.size())
    };
    for (const int target : safeOffsets) {
        const int line = oracle.lineIndexFor(target);
        const PointerEvent event = eventAt(frame.bounds, oracle, target, line);
        const int expected = oracleByteAt(frame.bounds, oracle, event);
        callbacks.press(event, frame.bounds);
        check(frame.state->cursor == expected,
              "multiline Chinese/emoji callbacks must match InputLayout byte-boundary oracle");
    }
}
}  // namespace

int main() {
    testCopiesDoNotCloneLongLineMetrics();
    testMultilineUtf8Oracle();
    return failures ? 1 : 0;
}
