#include "core/dsl_runtime.h"
#include "core/input/input_state.h"
#include "core/window/window_backend.h"
#include "components/input.h"

#if defined(EUI_WINDOW_BACKEND_SDL2)
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#else
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#if defined(EUI_WINDOW_BACKEND_WIN32)
#include "core/window/win32_host.h"
namespace host = core::window::win32;
#else
#include "core/window/glfw_host.h"
namespace host = core::window::glfwHost;
#endif
#endif

#include <iostream>

namespace {

core::window::RenderApi configuredRenderApi() {
#if defined(EUI_RENDER_BACKEND_D2D)
    return core::window::RenderApi::Native;
#elif defined(EUI_RENDER_BACKEND_VULKAN)
    return core::window::RenderApi::Vulkan;
#else
    return core::window::RenderApi::OpenGL;
#endif
}

bool initializeWindowBackend() {
#if defined(EUI_WINDOW_BACKEND_SDL2)
    SDL_SetMainReady();
    return SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0;
#else
    return host::initialize() == host::True;
#endif
}

void terminateWindowBackend() {
#if defined(EUI_WINDOW_BACKEND_SDL2)
    SDL_Quit();
#else
    host::shutdownHost();
#endif
}

} // namespace

int main() {
    if (!initializeWindowBackend()) {
        return 1;
    }

    core::window::WindowCreateRequest request;
    request.width = 200;
    request.height = 160;
    request.title = "Input Runtime Probe";
    request.resizable = false;
    request.renderApi = configuredRenderApi();
    core::window::Handle window = core::window::createWindow(request);
    if (window == nullptr) {
        terminateWindowBackend();
        return 2;
    }

    int presses = 0;
    int releases = 0;
    int middleDrags = 0;
    int rightDrags = 0;
    int contextMenus = 0;
    int focusedKeys = 0;
    int defaultFocusedKeys = 0;
    int applicationKeys = 0;

    core::dsl::Runtime runtime;
    runtime.setKeyEventHandler([&](const core::KeyEvent&) { ++applicationKeys; });
    runtime.initialize(window);
    runtime.compose("input", 200.0f, 160.0f,
        [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            ui.rect("target")
                .position(10.0f, 10.0f)
                .size(120.0f, 100.0f)
                .acceptedButtons(core::PointerButton::Left |
                                 core::PointerButton::Middle |
                                 core::PointerButton::Right)
                .dragThreshold(4.0f)
                .onPress([&](const core::PointerEvent&, const core::Rect&) {
                    ++presses;
                })
                .onRelease([&](const core::PointerEvent&, const core::Rect&) {
                    ++releases;
                })
                .onDrag([&](const core::dsl::DragEvent& event) {
                    if (event.button == core::PointerButton::Middle) {
                        ++middleDrags;
                    } else if (event.button == core::PointerButton::Right) {
                        ++rightDrags;
                    }
                })
                .onContextMenu([&](const core::PointerEvent&, const core::Rect&) {
                    ++contextMenus;
                })
                .onKeyEvent([&](const core::KeyEvent& event) {
                    ++focusedKeys;
                    return event.key == core::InputKey::F1;
                })
                .build();
            ui.rect("default-focus")
                .position(140.0f, 10.0f)
                .size(50.0f, 100.0f)
                .onKeyEvent([&](const core::KeyEvent&) {
                    ++defaultFocusedKeys;
                    return true;
                })
                .build();
        });

    const core::KeyModifiers modifiers{};
    core::queuePointerButton(window, 20.0, 20.0, core::PointerButton::Middle,
                             core::PointerAction::Press, modifiers);
    core::queuePointerMotion(window, 30.0, 20.0,
                             core::PointerButton::Middle, modifiers);
    core::queuePointerButton(window, 30.0, 20.0, core::PointerButton::Middle,
                             core::PointerAction::Release, modifiers);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);

    core::queuePointerButton(window, 20.0, 20.0, core::PointerButton::Right,
                             core::PointerAction::Press, modifiers);
    core::queuePointerButton(window, 20.0, 20.0, core::PointerButton::Right,
                             core::PointerAction::Release, modifiers);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);

    core::queuePointerButton(window, 20.0, 20.0, core::PointerButton::Right,
                             core::PointerAction::Press, modifiers);
    core::queuePointerMotion(window, 32.0, 20.0,
                             core::PointerButton::Right, modifiers);
    core::queuePointerButton(window, 32.0, 20.0, core::PointerButton::Right,
                             core::PointerAction::Release, modifiers);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);

    core::queuePointerButton(window, 20.0, 20.0, core::PointerButton::Left,
                             core::PointerAction::Press, modifiers);
    core::queuePointerButton(window, 20.0, 20.0, core::PointerButton::Left,
                             core::PointerAction::Release, modifiers);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);

    core::queuePointerButton(window, 150.0, 20.0, core::PointerButton::Right,
                             core::PointerAction::Press, modifiers);
    core::queuePointerButton(window, 150.0, 20.0, core::PointerButton::Right,
                             core::PointerAction::Release, modifiers);
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);

    core::queueKeyInput(window, {core::InputKey::F1, core::KeyAction::Press, {}});
    core::queueKeyInput(window, {core::InputKey::F2, core::KeyAction::Press, {}});
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);

    bool passed = presses == 4 && releases == 4 &&
        middleDrags > 0 && rightDrags > 0 && contextMenus == 1 &&
        focusedKeys == 2 && defaultFocusedKeys == 0 && applicationKeys == 1;
    if (!passed) {
        std::cerr << "Input Runtime dispatch failed: presses=" << presses
                  << " releases=" << releases
                  << " middleDrags=" << middleDrags
                  << " rightDrags=" << rightDrags
                  << " contextMenus=" << contextMenus
                  << " focusedKeys=" << focusedKeys
                  << " defaultFocusedKeys=" << defaultFocusedKeys
                  << " applicationKeys=" << applicationKeys << "\n";
    }

    // 经由 Runtime 的命中、捕获和焦点分发验证输入框滚动条，而非直接调用回调。
    std::string document;
    for (int i = 0; i < 2000; ++i) document += "row " + std::to_string(i) + "\n";
    core::dsl::Ui* editorUi = nullptr;
    using InputState = components::input_detail::InputModel::InputState;
    InputState* editorState = nullptr;
    int editorEnter = 0;
    int editorEscape = 0;
    const auto composeEditor = [&] {
        runtime.compose("editor", 200.f, 160.f, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            editorUi = &ui;
            components::input(ui, "field").position(10.f, 10.f).size(180.f, 140.f)
                .inset(10.f).fontSize(16.f).multiline().scrollbar().value(document)
                .onEnter([&] { ++editorEnter; }).onEscape([&] { ++editorEscape; })
                .onChange([&](const std::string& value) { document = value; }).build();
            editorState = &ui.state<InputState>("field");
        });
    };
    const auto updateEditor = [&] {
        runtime.update(window, 1.f / 60.f, 1.f, 1.f);
        composeEditor();
    };
    const auto click = [&](double x, double y) {
        core::queuePointerButton(window, x, y, core::PointerButton::Left, core::PointerAction::Press, modifiers);
        core::queuePointerButton(window, x, y, core::PointerButton::Left, core::PointerAction::Release, modifiers);
        updateEditor();
    };
    composeEditor();
    runtime.update(window, 1.f / 60.f, 1.f, 1.f);
    click(30, 30);
    core::KeyModifiers selectModifiers;
    selectModifiers.control = true;
    selectModifiers.super = true;
    core::queueKeyInput(window, {core::InputKey::A, core::KeyAction::Press, selectModifiers});
    updateEditor();
    const int selectionStart = editorState->selectionStart;
    const int selectionEnd = editorState->selectionEnd;
    auto* track = editorUi->find("field.scrollbar.track");
    if (!track) {
        std::cerr << "Overflow editor has no scrollbar\n";
        passed = false;
    } else {
        const auto trackFrame = track->frame;
        click(trackFrame.x + trackFrame.width * 0.5f, trackFrame.y + 1.f);
        passed = passed && editorState->verticalScroll == 0.f && editorUi->isFocused("field.hit");
        const auto thumbFrame = editorUi->find("field.scrollbar.thumb")->frame;
        const double x = thumbFrame.x + thumbFrame.width * 0.5f;
        const double y = thumbFrame.y + thumbFrame.height * 0.5f;
        const double travel = trackFrame.height - thumbFrame.height;
        core::queuePointerButton(window, x, y, core::PointerButton::Left, core::PointerAction::Press, modifiers);
        updateEditor();
        core::queuePointerMotion(window, x, y + travel * 0.25, core::PointerButton::Left, modifiers);
        updateEditor();
        const float quarterOffset = editorState->verticalScroll;
        core::queuePointerMotion(window, x, y + travel * 0.5, core::PointerButton::Left, modifiers);
        updateEditor();
        const float halfOffset = editorState->verticalScroll;
        core::queuePointerButton(window, x, y + travel * 0.5, core::PointerButton::Left, core::PointerAction::Release, modifiers);
        updateEditor();
        passed = passed && quarterOffset > 0.f && std::fabs(halfOffset - quarterOffset * 2.f) < 1.f &&
            std::fabs(editorState->verticalScroll - halfOffset) < 1.f &&
            editorState->selectionStart == selectionStart && editorState->selectionEnd == selectionEnd &&
            selectionStart != selectionEnd && editorUi->isFocused("field.hit");
        // 滑块上的滚轮也应滚动文本；之后输入必须仍送到原输入框。
        core::queueScrollInput(window, 0, -1);
        updateEditor();
        passed = passed && editorState->verticalScroll > halfOffset;
        core::queueTextInput(window, "X");
        updateEditor();
        passed = passed && document == "X" && editorState->followCaret &&
            editorState->verticalScroll == 0.f && !editorUi->find("field.scrollbar.thumb");
        if (!passed) std::cerr << "Input scrollbar Runtime drag/focus/selection/wheel/type regression failed\n";
    }

    document = "prefix suffix";
    composeEditor();
    using Model = components::input_detail::InputModel;
    Model::moveCursorTo(*editorState, 7, false);
    core::queueTextEditing(window, "中文测试");
    updateEditor();
    passed = passed && editorState->preedit && editorState->preedit->text == "prefix 中文测试suffix" &&
        document == "prefix suffix" && !editorUi->find("field.composition") &&
        editorUi->find("field.composition.underline.0");
    if (editorState->preedit) {
        std::string rendered;
        for (size_t i = 0; i < editorState->preedit->cachedLines.size(); ++i) {
            const auto* line = editorUi->find("field.text." + std::to_string(i));
            if (line) rendered += line->text;
        }
        passed = passed && rendered == "prefix 中文测试suffix";
    }
    // Exercise the real focused component callback via Runtime, including application
    // fallback. Candidate navigation must leave document selection and history intact.
    const core::InputKey compositionKeys[] = {
        core::InputKey::Left, core::InputKey::Right, core::InputKey::Up, core::InputKey::Down,
        core::InputKey::PageUp, core::InputKey::PageDown, core::InputKey::Home, core::InputKey::End,
        core::InputKey::Backspace, core::InputKey::Delete, core::InputKey::Enter, core::InputKey::Escape,
        core::InputKey::A, core::InputKey::C, core::InputKey::X, core::InputKey::V,
        core::InputKey::Z, core::InputKey::Y, core::InputKey::F
    };
    const int compositionCursor = editorState->cursor;
    const int compositionStart = editorState->selectionStart;
    const int compositionEnd = editorState->selectionEnd;
    const int compositionApplicationKeys = applicationKeys;
    for (const auto compositionKey : compositionKeys) {
        for (int shortcut = 0; shortcut < 2; ++shortcut) {
            // queueKeyInput synthesizes clipboard paste for Ctrl+V before Runtime.
            // Native composition must suppress that at the backend boundary; do
            // not touch the user's clipboard to simulate that separate contract.
            if (compositionKey == core::InputKey::V && shortcut) continue;
            // No real native IME owns this synthetic preedit, so republish it
            // each frame rather than letting the native query cancel it.
            core::queueTextEditing(window, "中文测试");
            core::queueKeyInput(window, {compositionKey, core::KeyAction::Press,
                                        shortcut ? selectModifiers : core::KeyModifiers{}});
            updateEditor();
            const bool protectedState = document == "prefix suffix" &&
                editorState->cursor == compositionCursor &&
                editorState->selectionStart == compositionStart &&
                editorState->selectionEnd == compositionEnd &&
                editorState->compositionText == "中文测试" &&
                applicationKeys == compositionApplicationKeys && editorEnter == 0 && editorEscape == 0;
            if (!protectedState) {
                std::cerr << "Composition key leaked: key=" << static_cast<int>(compositionKey)
                          << " shortcut=" << shortcut << '\n';
                passed = false;
            }
        }
    }
    core::queueTextEditing(window, "");
    updateEditor();
    passed = passed && !editorState->preedit && document == "prefix suffix";
    editorState->selectionStart = 7;
    editorState->selectionEnd = static_cast<int>(document.size());
    editorState->cursor = editorState->selectionEnd;
    core::queueTextEditing(window, "替换");
    updateEditor();
    passed = passed && editorState->preedit && editorState->preedit->text == "prefix 替换";
    core::queueTextEditing(window, "");
    core::queueTextInput(window, "替换");
    updateEditor();
    passed = passed && !editorState->preedit && document == "prefix 替换";
    core::queueKeyInput(window, {core::InputKey::Z, core::KeyAction::Press, selectModifiers});
    updateEditor();
    passed = passed && document == "prefix suffix";
    if (!passed) std::cerr << "Input Runtime preedit inline layout, cancel, replacement or undo failed\n";

    // IMEs can publish a committed prefix and the next COMPSTR in one event.
    // Keep the new preedit at the cursor after the committed prefix.
    document = "prefix suffix";
    composeEditor();
    Model::moveCursorTo(*editorState, 7, false);
    core::queueTextInput(window, "第一");
    core::queueTextEditing(window, "后续");
    updateEditor();
    const bool continued = document == "prefix 第一suffix" && editorState->preedit &&
        editorState->compositionText == "后续" && editorState->preedit->text == "prefix 第一后续suffix";
    passed = passed && continued;
    if (!continued) std::cerr << "Committed prefix discarded the continuing preedit\n";
    core::queueTextEditing(window, "");
    updateEditor();
    core::queueKeyInput(window, {core::InputKey::Z, core::KeyAction::Press, selectModifiers});
    updateEditor();
    passed = passed && document == "prefix suffix";

    Model::moveCursorTo(*editorState, 7, false);
    core::queueTextEditing(window, "待取消");
    updateEditor();
    click(199, 159);
    const bool blurredCleanly = editorState->compositionText.empty() && !editorState->preedit &&
        document == "prefix suffix";
    core::queueTextEditing(window, "");
    updateEditor();
    click(30, 30);
    const bool refocusedCleanly = editorState->compositionText.empty() && !editorState->preedit &&
        editorUi->isFocused("field.hit") && document == "prefix suffix";
    passed = passed && blurredCleanly && refocusedCleanly;
    if (!blurredCleanly || !refocusedCleanly) std::cerr << "Blur/refocus resurrected canceled preedit\n";

    // Same-batch text/press, programmatic focus and same-control relocation all
    // keep the committed prefix with its original owner and discard preedit.
    std::string left = "old\nsecond", right = "new";
    InputState* leftState = nullptr;
    InputState* rightState = nullptr;
    const auto composePair = [&](const char* focus = nullptr) {
        runtime.compose("pair", 200.f, 160.f, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            components::input(ui, "left").position(0.f, 0.f).size(95.f, 150.f)
                .fontSize(16.f).multiline().value(left)
                .onChange([&](const std::string& value) { left = value; }).build();
            components::input(ui, "right").position(100.f, 0.f).size(95.f, 150.f)
                .fontSize(16.f).value(right)
                .onChange([&](const std::string& value) { right = value; }).build();
            leftState = &ui.state<InputState>("left");
            rightState = &ui.state<InputState>("right");
            if (focus) ui.requestFocus(focus);
        });
    };
    const auto pairUpdate = [&] {
        runtime.update(window, 1.f / 60.f, 1.f, 1.f);
        composePair();
    };
    const auto pairClick = [&](double x, double y) {
        core::queuePointerButton(window, x, y, core::PointerButton::Left, core::PointerAction::Press, modifiers);
        core::queuePointerButton(window, x, y, core::PointerButton::Left, core::PointerAction::Release, modifiers);
    };
    composePair("left.hit");
    Model::moveCursorTo(*leftState, 0, false);
    core::queueTextEditing(window, "旧拼音");
    pairUpdate();
    core::queueTextInput(window, "词");
    core::queueTextEditing(window, "待取消");
    pairClick(125, 25);
    pairUpdate();
    const bool pointerOwner = left == "词old\nsecond" && right == "new" &&
        leftState->compositionText.empty() && rightState->compositionText.empty();
    passed = passed && pointerOwner;
    if (!pointerOwner) std::cerr << "Pointer focus change routed old preedit/result to new owner\n";

    composePair("left.hit");
    Model::moveCursorTo(*leftState, 0, false);
    core::queueTextInput(window, "好");
    core::queueTextEditing(window, "再取消");
    composePair("right.hit");
    pairUpdate();
    const bool requestedOwner = left == "好词old\nsecond" && right == "new" &&
        leftState->compositionText.empty() && rightState->compositionText.empty();
    passed = passed && requestedOwner;
    if (!requestedOwner) std::cerr << "Composed focus request leaked the old session\n";

    composePair("left.hit");
    Model::moveCursorTo(*leftState, 0, false);
    core::queueTextEditing(window, "跨行取消");
    pairUpdate();
    // Requeue synthetic preedit so the native query's inactive result cannot
    // cancel it before the actual press path under test.
    core::queueTextEditing(window, "跨行取消");
    pairClick(30, 70);
    pairUpdate();
    const bool relocatedCleanly = left == "好词old\nsecond" &&
        leftState->compositionText.empty() && !leftState->preedit && leftState->cursor > 0;
    passed = passed && relocatedCleanly;
    if (!relocatedCleanly) std::cerr << "Same-control press carried preedit to another insertion point\n";
    core::queueTextEditing(window, "禁用取消");
    pairUpdate();
    runtime.update(window, 1.f / 60.f, 1.f, 1.f, false);
    composePair();
    const bool disabledCleanly = leftState->compositionText.empty() && !leftState->preedit;
    passed = passed && disabledCleanly;
    if (!disabledCleanly) std::cerr << "Disabled runtime retained preedit\n";

    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    terminateWindowBackend();
    return passed ? 0 : 3;
}
