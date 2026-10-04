#include "core/render/render_backend.h"
#include "core/render/render_types.h"
#include "core/window/window_backend.h"

#if defined(EUI_WINDOW_BACKEND_SDL2)
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#if defined(EUI_RENDER_BACKEND_VULKAN)
#include <SDL_vulkan.h>
#endif
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

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace {

int frameCountFromArgs(int argc, char** argv) {
    int frames = 3;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--manual") == 0) {
            return -1;
        }
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::max(1, std::atoi(argv[++i]));
        }
    }
    return frames;
}

void sleepBriefly() {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
}

void drawableSize(core::window::Handle window, int& width, int& height) {
#if defined(EUI_WINDOW_BACKEND_SDL2)
#if defined(EUI_RENDER_BACKEND_VULKAN)
    SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(window), &width, &height);
#else
    SDL_GL_GetDrawableSize(static_cast<SDL_Window*>(window), &width, &height);
#endif
#else
    host::framebufferSize(static_cast<host::Window*>(window), &width, &height);
#endif
}

void renderClearFrame(core::window::Handle window, core::render::RenderBackend& backend) {
    int width = 0;
    int height = 0;
    drawableSize(window, width, height);
    if (width <= 0 || height <= 0) {
        return;
    }

    backend.makeCurrent();
    backend.beginFrame({
        window,
        core::window::nativeWindowInfo(window),
        width,
        height,
        1.0f
    });
    backend.clear({0.16f, 0.18f, 0.20f, 1.0f});
    backend.present();
}

bool pollOnce(core::window::Handle window) {
#if defined(EUI_WINDOW_BACKEND_SDL2)
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
            return false;
        }
    }
#else
    host::pollEvents();
    if (host::shouldClose(static_cast<host::Window*>(window))) {
        return false;
    }
#endif
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const int frames = frameCountFromArgs(argc, argv);
    core::render::initializeRenderBackendLoader();

#if defined(EUI_WINDOW_BACKEND_SDL2)
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return 1;
    }
#else
    if (!host::initialize()) {
        return 1;
    }
#endif

    core::window::WindowCreateRequest request;
    request.width = 800;
    request.height = 600;
    request.title = "Render Clear Probe";
    request.renderApi = core::render::windowRenderApi();

    core::window::Handle window = core::window::createWindow(request);
    if (window == nullptr) {
#if defined(EUI_WINDOW_BACKEND_SDL2)
        SDL_Quit();
#else
        host::shutdownHost();
#endif
        return 1;
    }

    std::unique_ptr<core::render::RenderBackend> backend = core::render::createRenderBackend(window);
    if (!backend || !backend->initialize()) {
        core::window::destroyWindow(window);
#if defined(EUI_WINDOW_BACKEND_SDL2)
        SDL_Quit();
#else
        host::shutdownHost();
#endif
        return 1;
    }

    int renderedFrames = 0;
    while (frames < 0 || renderedFrames < frames) {
        if (!pollOnce(window)) {
            break;
        }
        renderClearFrame(window, *backend);
        sleepBriefly();
        ++renderedFrames;
    }

    backend->makeCurrent();
    backend->releaseRenderCache();
    backend.reset();
    core::window::destroyWindow(window);
#if defined(EUI_WINDOW_BACKEND_SDL2)
    SDL_Quit();
#else
    host::shutdownHost();
#endif
    return 0;
}
