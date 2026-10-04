#include "core/input/input_state.h"
#include "core/dsl_runtime.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

bool verifyPointerQueue() {
    int firstTag = 0;
    int secondTag = 0;
    const auto firstWindow = reinterpret_cast<core::window::Handle>(&firstTag);
    const auto secondWindow = reinterpret_cast<core::window::Handle>(&secondTag);

    core::KeyModifiers modifiers;
    modifiers.shift = true;
    core::queuePointerMotion(firstWindow, 10.0, 20.0, {}, modifiers);
    core::queuePointerButton(firstWindow, 10.0, 20.0,
                             core::PointerButton::Middle,
                             core::PointerAction::Press,
                             modifiers);
    core::queuePointerMotion(firstWindow, 14.0, 25.0,
                             core::PointerButton::Middle,
                             modifiers);
    core::queuePointerButton(firstWindow, 14.0, 25.0,
                             core::PointerButton::Right,
                             core::PointerAction::Press,
                             modifiers);
    core::queuePointerButton(firstWindow, 14.0, 25.0,
                             core::PointerButton::Right,
                             core::PointerAction::Release,
                             modifiers);
    core::queuePointerButton(firstWindow, 14.0, 25.0,
                             core::PointerButton::Middle,
                             core::PointerAction::Release,
                             modifiers);

    if (!core::hasPendingPointerInput(firstWindow) ||
        core::hasPendingPointerInput(secondWindow)) {
        return false;
    }

    const std::vector<core::PointerEvent> events = core::consumePointerEvents(firstWindow);
    return events.size() == 6 &&
        events[0].action == core::PointerAction::Move &&
        events[1].isPress(core::PointerButton::Middle) &&
        events[1].modifiers.shift &&
        events[2].action == core::PointerAction::Move &&
        events[2].isDown(core::PointerButton::Middle) &&
        events[3].isPress(core::PointerButton::Right) &&
        events[3].isDown(core::PointerButton::Middle) &&
        events[3].isDown(core::PointerButton::Right) &&
        events[4].isRelease(core::PointerButton::Right) &&
        events[4].isDown(core::PointerButton::Middle) &&
        events[5].isRelease(core::PointerButton::Middle) &&
        events[5].buttons.empty();
}

bool verifyPointerCancel() {
    int windowTag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&windowTag);
    core::queuePointerButton(window, 5.0, 7.0,
                             core::PointerButton::Right,
                             core::PointerAction::Press,
                             {});
    core::cancelPointerInput(window);
    const std::vector<core::PointerEvent> events = core::consumePointerEvents(window);
    return events.size() == 2 &&
        events[0].isPress(core::PointerButton::Right) &&
        events[1].action == core::PointerAction::Cancel &&
        events[1].button == core::PointerButton::Right &&
        events[1].buttons.empty();
}

bool verifySideButtons() {
    int windowTag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&windowTag);
    core::queuePointerButton(window, 5.0, 7.0,
                             core::PointerButton::X1,
                             core::PointerAction::Press,
                             {});
    core::queuePointerButton(window, 5.0, 7.0,
                             core::PointerButton::X2,
                             core::PointerAction::Press,
                             {});
    core::queuePointerButton(window, 5.0, 7.0,
                             core::PointerButton::X1,
                             core::PointerAction::Release,
                             {});
    core::queuePointerButton(window, 5.0, 7.0,
                             core::PointerButton::X2,
                             core::PointerAction::Release,
                             {});
    const std::vector<core::PointerEvent> events = core::consumePointerEvents(window);
    core::releaseInputQueue(window);
    return events.size() == 4 &&
        events[0].isPress(core::PointerButton::X1) &&
        events[1].isPress(core::PointerButton::X2) &&
        events[1].isDown(core::PointerButton::X1) &&
        events[2].isRelease(core::PointerButton::X1) &&
        events[2].isDown(core::PointerButton::X2) &&
        events[3].isRelease(core::PointerButton::X2) &&
        events[3].buttons.empty();
}

bool verifyComposingKeyOrder() {
    int windowTag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&windowTag);
    core::queueTextEditing(window, "composition");
    core::queueKeyInput(window, {
        core::InputKey::Backspace, core::KeyAction::Press, {}
    });
    core::queueKeyInput(window, {
        core::InputKey::Backspace, core::KeyAction::Release, {}
    });
    const std::vector<core::KeyEvent> events = core::consumeKeyEvents(window);
    core::releaseInputQueue(window);
    return events.size() == 2 &&
        events[0].key == core::InputKey::Backspace &&
        events[0].action == core::KeyAction::Press &&
        events[1].key == core::InputKey::Backspace &&
        events[1].action == core::KeyAction::Release;
}

bool verifyKeyboardCancel() {
    int windowTag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&windowTag);
    core::KeyModifiers modifiers;
    modifiers.shift = true;
    core::queueKeyInput(window, {
        core::InputKey::LeftShift, core::KeyAction::Press, modifiers
    });
    core::queueKeyInput(window, {
        core::InputKey::Unknown, core::KeyAction::Press, modifiers, 123
    });
    core::cancelKeyboardInput(window);
    const std::vector<core::KeyEvent> events = core::consumeKeyEvents(window);
    const core::KeyModifiers current = core::detail::currentModifiers(window);
    core::releaseInputQueue(window);
    return events.size() == 3 &&
        events[0].key == core::InputKey::LeftShift &&
        events[0].action == core::KeyAction::Press &&
        events[1].key == core::InputKey::Unknown &&
        events[1].scanCode == 123 &&
        events[2].key == core::InputKey::LeftShift &&
        events[2].action == core::KeyAction::Release &&
        !current.shift;
}

bool verifyPointerOutsideWithoutPosition() {
    int windowTag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&windowTag);
    const std::vector<core::PointerEvent> events = core::consumePointerEvents(window);
    if (events.size() != 1 || events[0].action != core::PointerAction::Move ||
        events[0].x >= -999999.0 || events[0].y >= -999999.0) {
        return false;
    }
    core::queuePointerMotion(window, 40.0, 50.0, {}, {});
    const std::vector<core::PointerEvent> entered = core::consumePointerEvents(window);
    return entered.size() == 1 && entered[0].deltaX == 0.0 && entered[0].deltaY == 0.0;
}

bool verifyInteractionCapture() {
    const core::Rect bounds{0.0f, 0.0f, 100.0f, 100.0f};
    const core::PointerButtons accepted =
        core::PointerButton::Middle | core::PointerButton::Right;

    core::PointerEvent middlePress;
    middlePress.x = 20.0;
    middlePress.y = 30.0;
    middlePress.action = core::PointerAction::Press;
    middlePress.button = core::PointerButton::Middle;
    middlePress.buttons = core::PointerButton::Middle;

    core::InteractionState defaultInteraction;
    defaultInteraction.update(bounds, middlePress, true, core::PointerButton::Left);
    if (defaultInteraction.active || defaultInteraction.pressStarted) {
        return false;
    }

    core::InteractionState interaction;
    interaction.update(bounds, middlePress, true, accepted);
    if (!interaction.pressStarted || interaction.activeButton != core::PointerButton::Middle) {
        return false;
    }

    core::PointerEvent shortMove = middlePress;
    shortMove.x += 3.0;
    shortMove.action = core::PointerAction::Move;
    shortMove.button = core::PointerButton::None;
    interaction.update(bounds, shortMove, true, accepted, 5.0);
    if (interaction.drag) {
        return false;
    }

    core::PointerEvent dragMove = shortMove;
    dragMove.x += 3.0;
    interaction.update(bounds, dragMove, true, accepted, 5.0);
    if (!interaction.drag) {
        return false;
    }

    core::PointerEvent unrelatedRelease = middlePress;
    unrelatedRelease.action = core::PointerAction::Release;
    unrelatedRelease.button = core::PointerButton::Right;
    interaction.update(bounds, unrelatedRelease, true, accepted);
    if (!interaction.active || interaction.released) {
        return false;
    }

    core::PointerEvent middleRelease = middlePress;
    middleRelease.action = core::PointerAction::Release;
    middleRelease.buttons = {};
    interaction.update(bounds, middleRelease, true, accepted);
    return !interaction.active && interaction.released && interaction.clicked;
}

bool verifyLogicalPointerCallbackCoordinatesAt125Percent() {
    int windowTag = 0;
    const auto window = reinterpret_cast<core::window::Handle>(&windowTag);
    core::dsl::Runtime runtime;
    if (!runtime.initialize()) {
        return false;
    }

    std::vector<core::PointerEvent> moves;
    std::vector<core::Rect> moveBounds;
    std::vector<core::PointerEvent> presses;
    std::vector<core::Rect> pressBounds;
    std::vector<core::PointerEvent> releases;
    std::vector<core::Rect> releaseBounds;
    std::vector<core::dsl::DragEvent> drags;
    std::vector<core::PointerEvent> cursorEvents;
    std::vector<core::Rect> cursorBounds;

    runtime.compose("pointer-coordinate-regression", 400.0f, 300.0f,
                    [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        ui.rect("target")
            .position(10.0f, 20.0f)
            .size(200.0f, 100.0f)
            .onMove([&](const core::PointerEvent& event, const core::Rect& bounds) {
                moves.push_back(event);
                moveBounds.push_back(bounds);
                return false;
            })
            .onPress([&](const core::PointerEvent& event, const core::Rect& bounds) {
                presses.push_back(event);
                pressBounds.push_back(bounds);
            })
            .onRelease([&](const core::PointerEvent& event, const core::Rect& bounds) {
                releases.push_back(event);
                releaseBounds.push_back(bounds);
            })
            .onDrag([&](const core::dsl::DragEvent& event) { drags.push_back(event); })
            .cursorAt([&](const core::PointerEvent& event, const core::Rect& bounds) {
                cursorEvents.push_back(event);
                cursorBounds.push_back(bounds);
                return core::CursorShape::Arrow;
            })
            .build();
    });

    // The runtime receives 125%-DPI pixel positions while the element and callbacks use
    // logical coordinates: (25,50) px == (20,40) DIP.
    core::queuePointerMotion(window, 25.0, 50.0, {}, {});
    core::queuePointerButton(window, 25.0, 50.0, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    core::queuePointerMotion(window, 50.0, 75.0, core::PointerButton::Left, {});
    core::queuePointerButton(window, 50.0, 75.0, core::PointerButton::Left,
                             core::PointerAction::Release, {});
    runtime.update(window, 0.0f, 1.0f, 1.25f);

    const auto near = [](double actual, double expected) {
        return std::fabs(actual - expected) < 0.001;
    };
    const auto logicalBounds = [&](const std::vector<core::Rect>& values) {
        return !values.empty() && std::all_of(values.begin(), values.end(), [&](const core::Rect& bounds) {
            return near(bounds.x, 10.0) && near(bounds.y, 20.0) &&
                   near(bounds.width, 200.0) && near(bounds.height, 100.0);
        });
    };
    const bool moveOk = moves.size() >= 2 && near(moves.front().x, 20.0) &&
                        near(moves.front().y, 40.0) && near(moves[1].x, 40.0) &&
                        near(moves[1].y, 60.0) && near(moves[1].deltaX, 20.0) &&
                        near(moves[1].deltaY, 20.0) && logicalBounds(moveBounds);
    const bool pressOk = presses.size() == 1 && near(presses[0].x, 20.0) &&
                         near(presses[0].y, 40.0) && logicalBounds(pressBounds);
    const bool releaseOk = releases.size() == 1 && near(releases[0].x, 40.0) &&
                           near(releases[0].y, 60.0) && logicalBounds(releaseBounds);
    const bool dragOk = !drags.empty() && near(drags.front().x, 40.0) &&
                        near(drags.front().y, 60.0) && near(drags.front().deltaX, 20.0) &&
                        near(drags.front().deltaY, 20.0) && near(drags.front().totalX, 20.0) &&
                        near(drags.front().totalY, 20.0);
    const bool cursorOk = !cursorEvents.empty() && near(cursorEvents.front().x, 20.0) &&
                          near(cursorEvents.front().y, 40.0) && logicalBounds(cursorBounds);

    runtime.shutdown();
    core::releaseInputQueue(window);
    return moveOk && pressOk && releaseOk && dragOk && cursorOk;
}

} // namespace

int main() {
    if (!verifyPointerQueue()) {
        std::cerr << "Pointer event ordering or window isolation failed\n";
        return 1;
    }
    if (!verifyPointerCancel()) {
        std::cerr << "Pointer cancellation did not release held buttons\n";
        return 1;
    }
    if (!verifySideButtons()) {
        std::cerr << "X1/X2 button state or event ordering failed\n";
        return 1;
    }
    if (!verifyPointerOutsideWithoutPosition()) {
        std::cerr << "Unknown pointer position produced a false hover position\n";
        return 1;
    }
    if (!verifyComposingKeyOrder()) {
        std::cerr << "Composition filtering broke raw key event ordering\n";
        return 1;
    }
    if (!verifyKeyboardCancel()) {
        std::cerr << "Keyboard cancellation did not release known held keys\n";
        return 1;
    }
    if (!verifyInteractionCapture()) {
        std::cerr << "Pointer button acceptance or gesture capture failed\n";
        return 1;
    }
    if (!verifyLogicalPointerCallbackCoordinatesAt125Percent()) {
        std::cerr << "Runtime pointer callbacks did not use consistent logical coordinates at 125% DPI\n";
        return 1;
    }
    return 0;
}
