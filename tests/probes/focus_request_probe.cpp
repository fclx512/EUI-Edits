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
#include <string>

int main() {
#if defined(EUI_WINDOW_BACKEND_SDL2)
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 1;
#else
    if (host::initialize() != host::True) return 1;
#endif
    core::window::WindowCreateRequest request;
    request.width = 280;
    request.height = 100;
    request.title = "Focus Request Probe";
    request.resizable = false;
#if defined(EUI_RENDER_BACKEND_D2D)
    request.renderApi = core::window::RenderApi::Native;
#elif defined(EUI_RENDER_BACKEND_VULKAN)
    request.renderApi = core::window::RenderApi::Vulkan;
#else
    request.renderApi = core::window::RenderApi::OpenGL;
#endif
    core::window::Handle window = core::window::createWindow(request);
    if (window == nullptr) return 2;

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    bool barOpen = false;
    bool focusBar = false;
    bool focusEditor = true;
    int enterCount = 0;
    int escapeCount = 0;
    int editorFocusCount = 0;
    int barFocusCount = 0;
    std::string query;
    core::dsl::Ui* uiPtr = nullptr;
    const auto compose = [&] {
        runtime.compose("focus", 280.0f, 100.0f,
                        [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            uiPtr = &ui;
            ui.rect("editor").size(270.0f, 30.0f)
                .onKeyEvent([](const core::KeyEvent&) { return true; })
                .onFocusChanged([&](bool focused) { if (focused) ++editorFocusCount; })
                .build();
            if (barOpen) {
                components::input(ui, "find")
                    .position(0.0f, 40.0f).size(270.0f, 30.0f)
                    .value(query)
                    .onChange([&](const std::string& value) { query = value; })
                    .onEnter([&] { ++enterCount; })
                    .onEscape([&] {
                        ++escapeCount;
                        barOpen = false;
                        focusEditor = true;
                    })
                    .onFocus([&](bool focused) { if (focused) ++barFocusCount; })
                    .build();
                if (focusBar) {
                    ui.requestFocus("find.hit");
                    focusBar = false;
                }
            }
            if (focusEditor) {
                ui.requestFocus("editor");
                focusEditor = false;
            }
        });
    };

    compose();
    const bool initialFocus = uiPtr->isFocused("editor") && editorFocusCount == 1;
    barOpen = true;
    focusBar = true;
    compose();
    const bool barFocused = uiPtr->isFocused("find.hit") && barFocusCount == 1;
    core::queueKeyInput(window, {core::InputKey::Enter, core::KeyAction::Press, {}});
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);
    const bool enterOnly = enterCount == 1 && escapeCount == 0 && barOpen;
    core::queueKeyInput(window, {core::InputKey::Escape, core::KeyAction::Press, {}});
    runtime.update(window, 1.0f / 60.0f, 1.0f, 1.0f);
    compose();
    const bool restored = !barOpen && escapeCount == 1 && enterCount == 1 &&
        uiPtr->isFocused("editor") && editorFocusCount == 2;

    runtime.shutdown();
    core::window::destroyWindow(window);
#if defined(EUI_WINDOW_BACKEND_SDL2)
    SDL_Quit();
#else
    host::shutdownHost();
#endif
    if (!(initialFocus && barFocused && enterOnly && restored)) {
        std::cerr << "Focus request / Enter / Escape regression failed\n";
        return 3;
    }
    return 0;
}
