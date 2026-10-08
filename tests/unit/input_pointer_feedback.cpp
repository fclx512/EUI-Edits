#include "components/input.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Model = components::input_detail::InputModel;
using State = Model::InputState;

void composeInput(core::dsl::Ui& ui, int& linkActions) {
    components::input_detail::LineDecoration decoration;
    decoration.lineHeight = 28.0f;
    components::input_detail::LineRunStyle linkStyle;
    linkStyle.link = true;
    linkStyle.underline = true;
    decoration.runs.push_back(components::input_detail::LineRun{7, 11, linkStyle});

    ui.begin("input-pointer-feedback");
    components::input(ui, "field")
        .position(20.0f, 16.0f)
        .size(500.0f, 100.0f)
        .value("prefix link suffix")
        .fontFamily("monospace")
        .fontSize(16.0f)
        .multiline(true)
        .inset(0.0f)
        .lineDecorator([decoration](const std::string& text) {
            return text == "prefix link suffix"
                ? std::vector<components::input_detail::LineDecoration>{decoration}
                : std::vector<components::input_detail::LineDecoration>{};
        })
        .onPointerHit([&linkActions](const Model::PointerHit& hit) {
            if (hit.onLink) ++linkActions;
        })
        .onContextMenu([](float, float) {})
        .build();
    ui.end();
    ui.layout(core::dsl::Screen{640.0f, 240.0f});
}

core::dsl::Element* findRun(core::dsl::Ui& ui, const std::string& id) {
    return ui.find(id);
}

bool near(float left, float right) {
    return std::fabs(left - right) < 0.01f;
}

core::Rect rectOf(const core::dsl::Element& element) {
    return {element.frame.x, element.frame.y, element.frame.width, element.frame.height};
}

bool hoverTracksRunAndLinkColorTransitions() {
    core::dsl::Ui ui;
    int linkActions = 0;
    composeInput(ui, linkActions);
    State& state = ui.state<State>("field");
    core::dsl::Element* hit = ui.find("field.hit");
    core::dsl::Element* prefix = findRun(ui, "field.text.0r0");
    core::dsl::Element* link = findRun(ui, "field.text.0r1");
    core::dsl::Element* linkHover = findRun(ui, "field.text.0r1.linkhover");
    if (hit == nullptr || !hit->onMove || !hit->onHoverChanged || prefix == nullptr ||
        link == nullptr || linkHover == nullptr) {
        std::cerr << "input runs did not expose the expected real DSL elements\n";
        return false;
    }

    const auto pointIn = [](const core::Rect& frame, float xRatio) {
        core::PointerEvent event;
        event.x = frame.x + frame.width * xRatio;
        event.y = frame.y + frame.height * 0.5f;
        return event;
    };
    const core::Rect bounds = rectOf(*hit);
    const core::PointerEvent linkPoint = pointIn(rectOf(*linkHover), 0.5f);
    if (!hit->onMove(linkPoint, bounds) || !state.pointerHoverValid ||
        state.pointerHoverLine != 0 || state.pointerHoverLinkBeg != 7 ||
        state.pointerHoverLinkEnd != 11) {
        std::cerr << "hovering the link did not request an update with its byte range\n";
        return false;
    }

    const core::PointerEvent prefixPoint = pointIn(rectOf(*prefix), 0.20f);
    if (!hit->onMove(prefixPoint, bounds) || state.pointerHoverLine != 0 ||
        state.pointerHoverLinkBeg != -1 || state.pointerHoverLinkEnd != -1) {
        std::cerr << "moving between runs on one line did not update or clear link hover\n";
        return false;
    }

    if (!hit->onMove(linkPoint, bounds)) {
        std::cerr << "returning to the link did not request a hover update\n";
        return false;
    }
    composeInput(ui, linkActions);
    linkHover = ui.find("field.text.0r1.linkhover");
    if (linkHover == nullptr || !linkHover->transition.enabled ||
        !near(linkHover->transition.durationSeconds, 0.10f) ||
        !core::hasAnimProperty(linkHover->transition.properties, core::AnimProperty::Color) ||
        !near(linkHover->color.a, 0.12f)) {
        std::cerr << "hovered link background lacks its 100ms color transition\n";
        return false;
    }

    hit = ui.find("field.hit");
    hit->onHoverChanged(false);
    if (state.pointerHoverValid || state.pointerHoverLinkBeg != -1 ||
        state.pointerHoverLinkEnd != -1) {
        std::cerr << "leaving the input did not clear the link hover state\n";
        return false;
    }
    composeInput(ui, linkActions);
    linkHover = ui.find("field.text.0r1.linkhover");
    if (linkHover == nullptr || !near(linkHover->color.a, 0.0f) ||
        !linkHover->transition.enabled || !near(linkHover->transition.durationSeconds, 0.10f) ||
        !core::hasAnimProperty(linkHover->transition.properties, core::AnimProperty::Color)) {
        std::cerr << "clearing link hover did not retain the animated transparent target\n";
        return false;
    }
    return true;
}

bool rightClickTracksContextTargetAndPreservesSelection() {
    core::dsl::Ui ui;
    int linkActions = 0;
    composeInput(ui, linkActions);
    State& state = ui.state<State>("field");
    state.cursor = 3;
    state.selectionStart = 0;
    state.selectionEnd = static_cast<int>(state.text.size());

    core::dsl::Element* hit = ui.find("field.hit");
    core::dsl::Element* linkHover = ui.find("field.text.0r1.linkhover");
    core::dsl::Element* prefix = ui.find("field.text.0r0");
    if (hit == nullptr || !hit->onContextMenu || linkHover == nullptr || prefix == nullptr) {
        std::cerr << "input context callback or run geometry was not composed\n";
        return false;
    }
    const auto eventAt = [](const core::Rect& frame, float ratio) {
        core::PointerEvent event;
        event.x = frame.x + frame.width * ratio;
        event.y = frame.y + frame.height * 0.5f;
        return event;
    };
    const core::Rect bounds = rectOf(*hit);
    const int oldCursor = state.cursor;
    const int oldSelectionStart = state.selectionStart;
    const int oldSelectionEnd = state.selectionEnd;

    hit->onContextMenu(eventAt(rectOf(*linkHover), 0.5f), bounds);
    if (state.contextLinkByte < 7 || state.contextLinkByte >= 11 ||
        state.cursor != oldCursor || state.selectionStart != oldSelectionStart ||
        state.selectionEnd != oldSelectionEnd) {
        std::cerr << "right-clicking a selected link changed the selection or missed its target\n";
        return false;
    }

    hit->onContextMenu(eventAt(rectOf(*prefix), 0.20f), bounds);
    if (state.contextLinkByte != -1 || state.cursor != oldCursor ||
        state.selectionStart != oldSelectionStart || state.selectionEnd != oldSelectionEnd) {
        std::cerr << "right-clicking selected plain text retained a link target or changed selection\n";
        return false;
    }
    return true;
}

bool linkActionWaitsForValidRelease() {
    core::dsl::Ui ui;
    int linkActions = 0;
    composeInput(ui, linkActions);
    State& state = ui.state<State>("field");
    state.cursor = 3;
    state.selectionStart = 0;
    state.selectionEnd = static_cast<int>(state.text.size());
    core::dsl::Element* hit = ui.find("field.hit");
    core::dsl::Element* linkHover = ui.find("field.text.0r1.linkhover");
    if (hit == nullptr || !hit->onPress || !hit->onRelease || !hit->onFocusChanged ||
        linkHover == nullptr) {
        std::cerr << "input link press/release callbacks were not composed\n";
        return false;
    }

    const auto pointAt = [](const core::Rect& frame, float xOffset = 0.0f) {
        core::PointerEvent event;
        event.x = frame.x + frame.width * 0.5f + xOffset;
        event.y = frame.y + frame.height * 0.5f;
        return event;
    };
    const core::Rect bounds = rectOf(*hit);
    const core::Rect linkFrame = rectOf(*linkHover);
    const core::PointerEvent down = pointAt(linkFrame);
    const auto setWholeSelectionAtPrefix = [&state]() {
        state.cursor = 3;
        state.selectionStart = 0;
        state.selectionEnd = static_cast<int>(state.text.size());
    };
    const auto press = [&]() {
        core::PointerEvent event = down;
        event.action = core::PointerAction::Press;
        event.button = core::PointerButton::Left;
        event.buttons = core::PointerButton::Left;
        hit->onPress(event, bounds);
    };
    const auto release = [&](core::PointerEvent event) {
        event.action = core::PointerAction::Release;
        event.button = core::PointerButton::Left;
        event.buttons = {};
        hit->onRelease(event, bounds);
    };

    press();
    if (linkActions != 0 || state.cursor != 3 || state.selectionStart != 0 ||
        state.selectionEnd != static_cast<int>(state.text.size()) || state.pressedLinkByte < 7 ||
        state.pressedLinkByte >= 11) {
        std::cerr << "link press fired early or altered the existing selection\n";
        return false;
    }
    release(down);
    if (linkActions != 1) {
        std::cerr << "same-position link release did not fire exactly once\n";
        return false;
    }

    setWholeSelectionAtPrefix();
    press();
    release(pointAt(linkFrame, 5.0f));
    if (linkActions != 1) {
        std::cerr << "dragging more than four DIPs fired a link action\n";
        return false;
    }

    setWholeSelectionAtPrefix();
    press();
    core::dsl::DragEvent drag;
    drag.x = down.x + 8.0f;
    drag.y = down.y;
    hit->onDragUpdate(drag);
    release(down);
    if (linkActions != 1) {
        std::cerr << "dragging out and back did not cancel the pending link action\n";
        return false;
    }

    setWholeSelectionAtPrefix();
    press();
    hit->onFocusChanged(false);
    release(down);
    if (linkActions != 1 || state.pressedLinkByte != -1) {
        std::cerr << "focus loss did not cancel the pending link action\n";
        return false;
    }
    return true;
}

bool snapshotsComposeBlockDecorations() {
    using Decoration = components::input_detail::LineDecoration;
    std::vector<Decoration> decorations(5);
    for (auto& line : decorations) line.lineHeight = 24.0f;
    for (int i = 0; i < 3; ++i) {
        decorations[i].box.background = {0.10f, 0.15f, 0.20f, 1.0f};
        decorations[i].box.backgroundRadius = 4.0f;
    }
    decorations[0].box.backgroundBlockFirst = true;
    decorations[2].box.backgroundBlockLast = true;
    decorations[3].box.barColor = {0.20f, 0.40f, 0.80f, 1.0f};
    decorations[3].box.barWidth = 2.0f;
    decorations[3].box.barCount = 2;
    decorations[4].tableId = 4;
    decorations[4].cells = {{12, 13, 0}, {14, 15, 0}};
    decorations[4].box.gridColor = {0.50f, 0.50f, 0.50f, 1.0f};
    const auto snapshot = std::make_shared<const components::input_detail::LineDecorationTable>(decorations);
    const auto compose = [&](core::dsl::Ui& ui, bool shared) {
        ui.begin("block-decorations");
        auto input = components::input(ui, "block");
        input.size(500.0f, 200.0f).value("code\n\nend\nq\na|b")
            .fontFamily("monospace").fontSize(16.0f).multiline(true);
        if (shared) {
            input.lineDecorationSnapshot([snapshot](const std::string&, const auto&) { return snapshot; });
        } else {
            input.lineDecorator([decorations](const std::string&) { return decorations; });
        }
        input.build();
        ui.end();
        ui.layout(core::dsl::Screen{640.0f, 240.0f});
    };
    core::dsl::Ui legacy, shared;
    compose(legacy, false);
    compose(shared, true);
    if (!shared.state<State>("block").decorations.empty() ||
        shared.state<State>("block").decorationSnapshot != snapshot) return false;
    for (const char* id : {"block.linebg.0", "block.linebar.3", "block.linebar.3.1",
                           "block.grid.v.4.0", "block.grid.v.4.1", "block.grid.v.4.2"}) {
        auto* a = legacy.find(id);
        auto* b = shared.find(id);
        if (!a || !b || !near(a->frame.x, b->frame.x) || !near(a->frame.y, b->frame.y) ||
            !near(a->frame.width, b->frame.width) || !near(a->frame.height, b->frame.height) ||
            !components::input_detail::colorEquals(a->color, b->color)) {
            std::cerr << "snapshot lost or changed a rendered block decoration: " << id << '\n';
            return false;
        }
    }
    if (shared.find("block.linebg.1") || shared.find("block.linebg.2")) {
        std::cerr << "blank code line split the continuous block background\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!hoverTracksRunAndLinkColorTransitions()) return 1;
    if (!rightClickTracksContextTargetAndPreservesSelection()) return 1;
    if (!linkActionWaitsForValidRelease()) return 1;
    if (!snapshotsComposeBlockDecorations()) return 1;
    return 0;
}
