#include "components/input.h"
#include "core/dsl_runtime.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using Model = components::input_detail::InputModel;
using State = Model::InputState;
using Decoration = components::input_detail::LineDecoration;
using Press = std::function<void(const core::PointerEvent&, const core::Rect&)>;

int failures = 0;
void check(bool ok, const char* why) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}

struct Frame {
    core::dsl::Ui ui;
    State* state = nullptr;
    core::Rect bounds{};
    Press press;
};

void compose(Frame& frame, const std::string& text, float controlWidth, std::uint64_t serial,
             bool enableReflow = true,
             const std::vector<Decoration>* decorations = nullptr) {
    frame.ui.begin("reflow-callback");
    frame.state = &frame.ui.state<State>("editor");
    frame.state->followCaret = false;
    frame.state->wordWrap = true;
    auto input = components::input(frame.ui, "editor");
    input.position(20.0f, 12.0f)
        .size(controlWidth, 132.0f)
        .inset(12.0f)
        .fontSize(16.0f)
        .fontFamily("monospace")
        .multiline(true)
        .wordWrap(true)
        .value(text);
    if (enableReflow) input.widthReflowFrameToken(serial);
    if (decorations)
        input.lineDecorator([rows = *decorations](const std::string&) { return rows; });
    input.build();
    frame.ui.end();
    frame.ui.layout(core::dsl::Screen{600.0f, 240.0f});
    auto* hit = frame.ui.find("editor.hit");
    check(hit != nullptr && static_cast<bool>(hit->onPress), "real editor hit callback is present");
    if (!hit) return;
    frame.bounds = {hit->frame.x, hit->frame.y, hit->frame.width, hit->frame.height};
    frame.press = hit->onPress;
}

core::PointerEvent eventAt(const core::Rect& bounds, float localY) {
    core::PointerEvent event;
    event.action = core::PointerAction::Press;
    event.button = core::PointerButton::Left;
    event.buttons = core::PointerButton::Left;
    event.x = bounds.x + 70.0f;
    event.y = bounds.y + localY;
    return event;
}

int pressCursor(Frame& frame, const Press& press, const core::Rect& bounds,
                const core::PointerEvent& event) {
    frame.state->cursor = 0;
    frame.state->selectionStart = 0;
    frame.state->selectionEnd = 0;
    press(event, bounds);
    return frame.state->cursor;
}

void testDecorationChangeRetiresOldPointerCallback() {
    const std::string prefix = "hidden-1234 ";
    const std::string text = prefix + "中文🙂 visible words to click";
    std::vector<Decoration> shown(1), hidden(1);
    hidden[0].holes = {{0, static_cast<int>(prefix.size())}};
    Frame frame;
    compose(frame, text, 360.0f, 1, true, &shown);
    check(frame.state && frame.press, "decorated source frame exposes pointer callback");
    if (!frame.state || !frame.press) return;
    const Press oldPress = frame.press;
    const core::Rect oldBounds = frame.bounds;
    const auto oldDecorationRevision = frame.state->decorationRevision;
    compose(frame, text, 360.0f, 2, true, &hidden);
    check(frame.state->decorationRevision != oldDecorationRevision,
          "same text with a newly hidden prefix changes decoration generation");
    check(!frame.state->cachedLines.empty(), "new decoration has a measured visible row");
    if (frame.state->cachedLines.empty()) return;
    const auto& projected = frame.state->cachedLines.front();
    const int afterFirstVisibleCharacter = static_cast<int>(prefix.size()) + 3; // 中 UTF-8 end
    const float projectedX = components::input_detail::caretXForDocOffset(
        projected.metrics, projected.presentation().holes, projected.start,
        afterFirstVisibleCharacter);
    const int expectedFreshByte = components::input_detail::docOffsetForX(
        projected.metrics, projected.presentation().holes, projected.start, projectedX);
    check(expectedFreshByte == afterFirstVisibleCharacter,
          "fresh-click fixture lands after a visible glyph beyond the hidden-prefix anchor");
    auto event = eventAt(oldBounds, 30.0f);
    event.x = oldBounds.x + 12.0f + projectedX;
    frame.state->cursor = 20;
    oldPress(event, oldBounds);
    check(frame.state->cursor == 20,
          "old pointer callback must not write caret after decoration-only geometry change");
    auto freshEvent = eventAt(frame.bounds, 30.0f);
    freshEvent.x = frame.bounds.x + 12.0f + projectedX;
    const int freshCursor = pressCursor(frame, frame.press, frame.bounds, freshEvent);
    check(freshCursor == expectedFreshByte && freshCursor != 20,
          "fresh callback uses the measured post-prefix caret at the same visible x");
}

void testSameRevisionExternalPreeditWrites() {
    core::dsl::Runtime runtime;
    runtime.initialize();
    core::dsl::Ui* ui = nullptr;
    State* state = nullptr;
    const std::string text = "first 中文🙂 line\nsecond line\nthird line";
    auto composeFocused = [&](std::uint64_t serial) {
        runtime.compose("reflow-preedit-byte-guard", 600.0f, 240.0f,
            [&](core::dsl::Ui& view, const core::dsl::Screen&) {
                ui = &view;
                components::input(view, "editor")
                    .size(300.0f, 120.0f).inset(12.0f)
                    .fontFamily("monospace").fontSize(16.0f)
                    .multiline(true).wordWrap(true)
                    .widthReflowFrameToken(serial).value(text).build();
                view.requestFocus("editor.hit");
            });
        state = &ui->state<State>("editor");
    };
    composeFocused(1);
    check(ui && ui->isFocused("editor.hit"), "preedit byte fixture focuses real input");
    if (!ui || !ui->isFocused("editor.hit")) { runtime.shutdown(); return; }
    state->cursor = state->selectionStart = state->selectionEnd = 6;
    auto* hit = ui->find("editor.hit");
    check(hit && static_cast<bool>(hit->onTextInput), "focused input exposes text-input callback");
    if (!hit || !hit->onTextInput) { runtime.shutdown(); return; }
    core::TextInputEvent preedit;
    preedit.composing = true;
    preedit.compositionChanged = true;
    preedit.compositionText = "拼🙂";
    hit->onTextInput(preedit);
    composeFocused(2);
    hit = ui->find("editor.hit");
    check(state->preedit && hit && static_cast<bool>(hit->onPress),
          "composition frame keeps temporary display state and pointer callback");
    if (!state->preedit || !hit || !hit->onPress) { runtime.shutdown(); return; }
    const Press firstPreeditPress = hit->onPress;
    const core::Rect firstBounds{hit->frame.x, hit->frame.y, hit->frame.width, hit->frame.height};
    core::PointerEvent press;
    press.action = core::PointerAction::Press;
    press.button = core::PointerButton::Left;
    press.buttons = core::PointerButton::Left;
    press.x = firstBounds.x + 14.0f;
    press.y = firstBounds.y + 15.0f;
    const auto textRevision = state->textRevision;
    const auto compositionRevision = state->compositionRevision;
    auto* originalPreedit = state->preedit.get();
    state->compositionText = "文🙂"; // same byte length, no revision bump or preedit replacement
    state->cursor = 16;
    firstPreeditPress(press, firstBounds);
    check(state->cursor == 16 && state->textRevision == textRevision &&
          state->compositionRevision == compositionRevision && state->preedit.get() == originalPreedit,
          "old preedit pointer callback rejects same-revision composition-byte write");

    state->compositionText = "拼🙂";
    composeFocused(3);
    hit = ui->find("editor.hit");
    check(hit && static_cast<bool>(hit->onPress), "fresh preedit frame exposes pointer callback");
    if (hit && hit->onPress) {
        const Press secondPreeditPress = hit->onPress;
        const core::Rect secondBounds{hit->frame.x, hit->frame.y, hit->frame.width, hit->frame.height};
        press.x = secondBounds.x + 14.0f;
        press.y = secondBounds.y + 15.0f;
        state->text[0] = 'F'; // external committed write, same byte length and revision
        state->cursor = 16;
        secondPreeditPress(press, secondBounds);
        check(state->cursor == 16 && state->textRevision == textRevision &&
              state->compositionRevision == compositionRevision,
              "old preedit pointer callback rejects same-revision committed-byte write");
    }
    runtime.shutdown();
}
} // namespace

int main() {
    testDecorationChangeRetiresOldPointerCallback();
    testSameRevisionExternalPreeditWrites();
    std::string text = "source 中文🙂 ";
    for (int i = 0; i < 300; ++i) text += "word" + std::to_string(i) + " ";
    text += "\ntail";
    Frame frame;
    compose(frame, text, 360.0f, 1);
    if (!frame.state || !frame.press) return 1;

    auto& schedule = frame.ui.state<components::InputWidthReflowSchedule>("editor.width-reflow-schedule");
    schedule.nextSliceDue = std::chrono::steady_clock::time_point::max();
    compose(frame, text, 200.0f, 2);
    check(Model::widthReflowPending(*frame.state), "narrow frame keeps an unfinished private reflow");
    if (!Model::widthReflowPending(*frame.state) || !frame.press) return 1;
    const Press oldPress = frame.press;
    const core::Rect oldBounds = frame.bounds;
    std::vector<core::PointerEvent> events;
    std::vector<int> oldResults;
    for (float y : {35.0f, 55.0f, 75.0f, 95.0f}) {
        events.push_back(eventAt(oldBounds, y));
        oldResults.push_back(pressCursor(frame, oldPress, oldBounds, events.back()));
    }

    for (int i = 0; i < 100000 && Model::widthReflowPending(*frame.state); ++i) {
        Model::stepWidthReflow(*frame.state, std::chrono::steady_clock::time_point::max(), 1);
    }
    check(!Model::widthReflowPending(*frame.state), "new generation reaches completion");
    check(frame.state->cachedViewportWidth < 200.0f, "new text viewport is published");
    for (std::size_t i = 0; i < events.size(); ++i)
        check(pressCursor(frame, oldPress, oldBounds, events[i]) == oldResults[i],
              "old callback keeps its frame's byte mapping after new layout publication");

    compose(frame, text, 200.0f, 3);
    check(frame.press != nullptr, "new frame exposes an interaction callback");
    if (frame.press) {
        bool observedDifferentWrap = false;
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto event = eventAt(frame.bounds, 35.0f + 20.0f * static_cast<float>(i));
            if (pressCursor(frame, frame.press, frame.bounds, event) != oldResults[i])
                observedDifferentWrap = true;
        }
        check(observedDifferentWrap, "fixture distinguishes old and newly wrapped callback geometry");
    }

    const std::string fontA = "C:/Windows/Fonts/msyh.ttc";
    const std::string fontB = "C:/Windows/Fonts/consola.ttf";
    check(std::filesystem::exists(fontA) && std::filesystem::exists(fontB),
          "stale callback test requires two real Windows fonts");
    if (std::filesystem::exists(fontA) && std::filesystem::exists(fontB)) {
        const auto oldGeneration = core::TextPrimitive::fontConfigurationGeneration();
        core::TextPrimitive::setDefaultFontFiles(fontA, {});
        if (core::TextPrimitive::fontConfigurationGeneration() == oldGeneration)
            core::TextPrimitive::setDefaultFontFiles(fontB, {});
        check(core::TextPrimitive::fontConfigurationGeneration() != oldGeneration,
              "default font switch changes callback context generation");
        frame.state->cursor = 123;
        oldPress(events.front(), oldBounds);
        check(frame.state->cursor == 123,
              "old event closure must not move caret to byte zero after font context changes");
        core::TextPrimitive::setDefaultFontFiles("", "");
    }

    // The timer is a pending-only child. It must advance at most once per app
    // update serial, even if the editor is composed repeatedly in that update.
    std::string manyRows;
    for (int i = 0; i < 380; ++i)
        manyRows += "row " + std::to_string(i) + " 中文🙂 with words to wrap into another segment\n";
    Frame scheduleFrame;
    compose(scheduleFrame, manyRows, 360.0f, 20);
    scheduleFrame.ui.state<components::InputWidthReflowSchedule>(
        "editor.width-reflow-schedule").nextSliceDue = std::chrono::steady_clock::time_point::min();
    compose(scheduleFrame, manyRows, 200.0f, 21);
    check(Model::widthReflowPending(*scheduleFrame.state), "large document remains pending after one scheduled slice");
    check(scheduleFrame.ui.find("editor.width-reflow-timer") != nullptr,
          "pending frame installs its wake timer child");
    const int once = scheduleFrame.state->widthReflow ? scheduleFrame.state->widthReflow->nextSourceLine : -1;
    check(once > 0, "first scheduled slice advances source rows");

    scheduleFrame.ui.state<components::InputWidthReflowSchedule>(
        "editor.width-reflow-schedule").nextSliceDue = std::chrono::steady_clock::time_point::min();
    compose(scheduleFrame, manyRows, 200.0f, 21);
    const int sameSerial = scheduleFrame.state->widthReflow ? scheduleFrame.state->widthReflow->nextSourceLine : -1;
    check(sameSerial == once, "two composes with one app update serial spend one slice");

    scheduleFrame.ui.state<components::InputWidthReflowSchedule>(
        "editor.width-reflow-schedule").nextSliceDue = std::chrono::steady_clock::time_point::max();
    compose(scheduleFrame, manyRows, 200.0f, 22);
    const int beforeDue = scheduleFrame.state->widthReflow ? scheduleFrame.state->widthReflow->nextSourceLine : -1;
    check(beforeDue == once, "new serial before due time does not spend a slice");

    scheduleFrame.ui.state<components::InputWidthReflowSchedule>(
        "editor.width-reflow-schedule").nextSliceDue = std::chrono::steady_clock::time_point::min();
    compose(scheduleFrame, manyRows, 200.0f, 22);
    const int nextSerial = scheduleFrame.state->widthReflow ? scheduleFrame.state->widthReflow->nextSourceLine : -1;
    check(nextSerial > once, "new serial after due time spends its next slice");

    for (int i = 0; i < 100000 && Model::widthReflowPending(*scheduleFrame.state); ++i)
        Model::stepWidthReflow(*scheduleFrame.state, std::chrono::steady_clock::time_point::max(), 1);
    compose(scheduleFrame, manyRows, 200.0f, 23);
    check(scheduleFrame.ui.find("editor.width-reflow-timer") == nullptr,
          "completed frame removes pending wake timer child");

    const bool pendingAgain = Model::requestWidthReflow(*scheduleFrame.state,
        "monospace", 16.0f, 136.0f);
    check(pendingAgain, "disable fixture has a pending width draft");
    compose(scheduleFrame, manyRows, 160.0f, 24, false);
    check(!Model::widthReflowPending(*scheduleFrame.state),
          "disabling reflow on the same editor id cancels its draft");
    check(scheduleFrame.ui.find("editor.width-reflow-timer") == nullptr,
          "disabled frame has no pending wake timer child");
    return failures ? 1 : 0;
}
