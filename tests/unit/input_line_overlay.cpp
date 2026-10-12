// Regression coverage for InputBuilder::lineOverlay(callback, key).
#include "components/input.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Model = components::input_detail::InputModel;
using State = Model::InputState;
using TextLine = Model::TextLine;
using Decoration = components::input_detail::LineDecoration;
using Overlay = std::function<void(core::dsl::Ui&, const TextLine&, const core::Rect&)>;

constexpr float kControlX = 23.0f;
constexpr float kControlY = 11.0f;
constexpr float kWidth = 260.0f;
constexpr float kHeight = 132.0f;
constexpr float kInset = 17.0f;
constexpr float kFontSize = 16.0f;
constexpr float kTextWidth = kWidth - 2.0f * kInset;
constexpr float kTextY = kFontSize * 0.2f + 2.0f;
constexpr float kTextHeight = kHeight - (kFontSize * 0.1f + 2.0f) * 2.0f - 12.0f;
constexpr float kLineHeight = kFontSize * 1.2f;
constexpr const char* kFont = "monospace";

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Call {
    int sourceStart = -1;
    bool lineStart = false;
    core::Rect bounds{};
};
struct Frame {
    core::dsl::Ui ui;
    State* state = nullptr;
    std::vector<Call> calls;
};

void compose(Frame& frame, const std::string& text,
             const std::vector<Decoration>& decorations = {},
             const Overlay& overlay = {}, const std::string& overlayKey = {},
             int cursor = 0, float verticalScroll = 0.0f) {
    frame.calls.clear();
    frame.ui.begin("input-line-overlay-draft");
    frame.state = &frame.ui.state<State>("editor");
    Model::loadDocument(*frame.state, text);
    frame.state->cursor = cursor;
    frame.state->selectionStart = cursor;
    frame.state->selectionEnd = cursor;
    frame.state->followCaret = false;
    frame.state->wordWrap = true;
    frame.state->verticalScroll = verticalScroll;

    auto input = components::input(frame.ui, "editor")
        .position(kControlX, kControlY)
        .size(kWidth, kHeight)
        .inset(kInset)
        .fontSize(kFontSize)
        .fontFamily(kFont)
        .multiline(true)
        .wordWrap(true)
        .scrollbar(false)
        .value(text)
        .lineDecorator([decorations](const std::string&) { return decorations; });
    if (overlay) {
        input.lineOverlay(overlay, overlayKey);
    }
    input.build();
    frame.ui.end();
    frame.ui.layout(core::dsl::Screen{640.0f, 360.0f});
}

Overlay recordingOverlay(std::vector<Call>& calls) {
    return [&calls](core::dsl::Ui& ui, const TextLine& line, const core::Rect& bounds) {
        calls.push_back({line.start, line.lineStart, bounds});
        // Materialize a named DSL element so absence of the opt-in callback can be
        // checked in the composed tree, and retained-key changes can be inspected.
        const std::string id = "probe.line-overlay." + std::to_string(calls.size());
        ui.rect(id).position(bounds.x, bounds.y).size(bounds.width, bounds.height)
            .color(core::Color{0.2f, 0.6f, 0.9f, 0.25f}).build();
    };
}

void testDefaultIsNullAndOverlayKeyInvalidatesRoot() {
    const std::string text = "plain\ntext";
    Frame frame;
    compose(frame, text);
    check(frame.calls.empty(), "omitting lineOverlay must invoke no callback");
    check(frame.ui.find("probe.line-overlay.1") == nullptr,
          "omitting lineOverlay must compose no overlay draw element");

    const Overlay overlay = recordingOverlay(frame.calls);
    compose(frame, text, {}, overlay, "overlay-state-A");
    auto* rootA = frame.ui.find("editor");
    check(rootA != nullptr, "input root remains addressable");
    const std::string firstRootKey = rootA ? rootA->dirtyKey : std::string{};
    check(firstRootKey.find("overlay-state-A") != std::string::npos,
          "caller overlay key participates in the input root dirtyKey");

    compose(frame, text, {}, overlay, "overlay-state-B");
    auto* rootB = frame.ui.find("editor");
    check(rootB != nullptr, "input root remains addressable after key change");
    if (rootB) {
        check(rootB->dirtyKey.find("overlay-state-B") != std::string::npos,
              "changed overlay state is represented by the new root dirtyKey");
        check(rootB->dirtyKey != firstRootKey,
              "changing only the caller overlay key invalidates retained root content");
    }
}

std::string makeUtf8Document() {
    std::string text = "源🙂";
    // Force soft-wrapped continuations while retaining one UTF-8 source-line start.
    text += std::string(72, 'x');
    text += "\n视图隐藏\n折叠隐藏\n尾行🙂";
    for (int i = 0; i < 20; ++i) text += "\nrow-" + std::to_string(i);
    return text;
}

std::vector<Decoration> makeHiddenDecorations() {
    std::vector<Decoration> rows(24);
    rows[1].hidden = true;
    rows[1].hiddenByView = true; // planned field: hidden for view semantics only
    rows[2].hidden = true;
    rows[2].hiddenByFold = true; // fold state is the only reveal-triggering cause
    return rows;
}

void testVisibleLineStartsAndComponentCoordinates() {
    const std::string text = makeUtf8Document();
    const auto decorations = makeHiddenDecorations();
    Frame frame;
    const float requestedScroll = kLineHeight * 0.25f + 3.0f;
    compose(frame, text, decorations, recordingOverlay(frame.calls), "visible-lines-v1",
            0, requestedScroll);

    check(frame.state != nullptr, "input state exists");
    if (!frame.state) return;
    check(frame.state->verticalScroll > 0.0f,
          "fixture keeps a nonzero vertical scroll after layout clamping");
    check(!frame.calls.empty(), "visible line starts invoke the overlay callback");

    const int utf8TailStart = static_cast<int>(text.find("尾行"));
    check(utf8TailStart > static_cast<int>(text.find("视图隐藏")) + 4,
          "UTF-8 fixture has distinct source byte offsets");
    bool sawDocumentStart = false;
    bool sawUtf8TailStart = false;
    std::vector<int> starts;
    for (const Call& call : frame.calls) {
        starts.push_back(call.sourceStart);
        check(call.lineStart, "overlay callback is restricted to TextLine::lineStart rows");
        check(call.sourceStart == 0 || call.sourceStart == utf8TailStart ||
              (call.sourceStart > utf8TailStart && text[static_cast<std::size_t>(call.sourceStart - 1)] == '\n'),
              "callback start is a real UTF-8 source byte boundary, not a codepoint index");
        sawDocumentStart |= call.sourceStart == 0;
        sawUtf8TailStart |= call.sourceStart == utf8TailStart;
        check(std::fabs(call.bounds.x - kInset) < 0.01f,
              "overlay x uses the text inset in input-local coordinates");
        check(std::fabs(call.bounds.width - kTextWidth) < 0.01f,
              "overlay width uses the text column width");
        const auto line = std::find_if(frame.state->cachedLines.begin(), frame.state->cachedLines.end(),
            [&](const TextLine& candidate) { return candidate.start == call.sourceStart && candidate.lineStart; });
        check(line != frame.state->cachedLines.end(), "callback maps to a cached source line start");
        if (line != frame.state->cachedLines.end()) {
            const float expectedY = kTextY + line->top - frame.state->verticalScroll;
            check(std::fabs(call.bounds.y - expectedY) < 0.02f,
                  "overlay y is textY + geometry.top - vertical scroll");
            check(std::fabs(call.bounds.height - line->lineHeight) < 0.02f,
                  "overlay height matches the row geometry height");
        }
    }
    std::sort(starts.begin(), starts.end());
    check(std::adjacent_find(starts.begin(), starts.end()) == starts.end(),
          "soft-wrap continuation TextLines do not duplicate a source-line overlay");
    check(sawDocumentStart, "first visible source line uses byte offset zero");
    check(sawUtf8TailStart, "multibyte source text retains its UTF-8 byte start");
    check(std::find(starts.begin(), starts.end(), static_cast<int>(text.find("视图隐藏"))) == starts.end(),
          "view-hidden line receives no overlay callback");
    check(std::find(starts.begin(), starts.end(), static_cast<int>(text.find("折叠隐藏"))) == starts.end(),
          "fold-hidden line receives no overlay callback while not cursor-revealed");
}

void testRevealOnlyForFoldHiddenLines() {
    const std::string text = "可见\n视图隐藏\n折叠隐藏\n末尾";
    const auto decorations = makeHiddenDecorations();
    int revealCount = 0;
    int revealedByte = -1;

    Frame viewFrame;
    const int viewByte = static_cast<int>(text.find("视图隐藏"));
    viewFrame.ui.begin("view-hidden-reveal-draft");
    viewFrame.state = &viewFrame.ui.state<State>("editor");
    Model::loadDocument(*viewFrame.state, text);
    viewFrame.state->cursor = viewByte;
    viewFrame.state->selectionStart = viewFrame.state->selectionEnd = viewByte;
    viewFrame.state->followCaret = false;
    auto viewInput = components::input(viewFrame.ui, "editor")
        .position(kControlX, kControlY).size(kWidth, kHeight).inset(kInset)
        .fontSize(kFontSize).fontFamily(kFont).multiline(true).wordWrap(false)
        .scrollbar(false).value(text)
        .lineDecorator([decorations](const std::string&) { return decorations; })
        .onRevealHiddenLine([&](int byte) { ++revealCount; revealedByte = byte; });
    viewInput.build();
    viewFrame.ui.end();
    viewFrame.ui.layout(core::dsl::Screen{640.0f, 360.0f});
    check(revealCount == 0,
          "a view-hidden cursor line does not request fold reveal");

    Frame foldFrame;
    const int foldByte = static_cast<int>(text.find("折叠隐藏"));
    foldFrame.ui.begin("fold-hidden-reveal-draft");
    foldFrame.state = &foldFrame.ui.state<State>("editor");
    Model::loadDocument(*foldFrame.state, text);
    foldFrame.state->cursor = foldByte;
    foldFrame.state->selectionStart = foldFrame.state->selectionEnd = foldByte;
    foldFrame.state->followCaret = false;
    auto foldInput = components::input(foldFrame.ui, "editor")
        .position(kControlX, kControlY).size(kWidth, kHeight).inset(kInset)
        .fontSize(kFontSize).fontFamily(kFont).multiline(true).wordWrap(false)
        .scrollbar(false).value(text)
        .lineDecorator([decorations](const std::string&) { return decorations; })
        .onRevealHiddenLine([&](int byte) { ++revealCount; revealedByte = byte; });
    foldInput.build();
    foldFrame.ui.end();
    foldFrame.ui.layout(core::dsl::Screen{640.0f, 360.0f});
    check(revealCount == 1,
          "a fold-hidden cursor line requests exactly one reveal");
    check(revealedByte == foldByte,
          "fold reveal reports the original UTF-8 byte offset");
}
} // namespace

int main() {
    testDefaultIsNullAndOverlayKeyInvalidatesRoot();
    testVisibleLineStartsAndComponentCoordinates();
    testRevealOnlyForFoldHiddenLines();
    return failures ? 1 : 0;
}




