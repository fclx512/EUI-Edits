#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#endif

#if defined(EUI_WINDOW_BACKEND_WIN32)
#include "core/window/win32_host.h"
using namespace core::window::win32;
#else
#include "core/window/glfw_host.h"
using namespace core::window::glfwHost;
#endif

#include "eui/app.h"
#include "eui/detail/dsl_app_impl.h"
#include "core/app/app_runner.h"
#include "core/app/dsl_window_manager.h"
#include "core/app/dsl_window_runtime.h"
#include "core/app/frame_pacing.h"
#include "core/app/live_resize_option.h"
#include "core/app/main_window_runtime.h"
#include "core/input/input_state.h"
#include "core/platform/platform.h"
#if defined(EUI_EDITS_BUNDLED_RESOURCES) && defined(_WIN32)
#include "core/platform/bundled_resources.h"
#endif
#include "core/window/window_backend.h"
#include "core/render/gpu_resize_stats.h"
#include "core/render/render_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

struct WindowState : app::AppRunner {
    bool hideToTrayRequested = false;
    bool forceClose = false;
    bool iconified = false;
    Window* modalChildWindow = nullptr;
    // Native DC frames coalesce at the display budget without sleeping inside
    // window callbacks. The older GLFW/GL path retains its 30Hz safety limit.
    // NEO_LIVE_RESIZE=0 and complete-frame reentry protection remain available.
    std::function<void()> paintResize;
    bool paintingResize = false;
    double lastResizePaintTime = -1.0;
#if defined(EUI_WINDOW_BACKEND_WIN32)
    Window* resizeWindow = nullptr;
    double resizeDeadline = 0, lastResizeCost = 0;
#endif

    void paintResizeIfDue() {
        if (!paintResize || paintingResize) {
#if defined(EUI_WINDOW_BACKEND_WIN32)
            if (paintResize && resizeWindow) requestRefreshAfter(resizeWindow, frameInterval);
#endif
            return;
        }
        const double now = core::window::timeSeconds();
#if defined(EUI_WINDOW_BACKEND_WIN32)
        if (now < resizeDeadline) {
            requestRefreshAfter(resizeWindow, resizeDeadline - now);
            return;
        }
#else
        if (lastResizePaintTime >= 0.0 && now - lastResizePaintTime < 1.0 / 30.0) {
            return;
        }
#endif
        paintingResize = true;
        struct ResetPainting {
            bool& painting;
            ~ResetPainting() { painting = false; }
        } reset{paintingResize};
        // 卡死取证：进入重绘前先落一行心跳。若进程在 paintResize 内挂死，
        // 足迹文件的最后一行就是 "paint-enter"，可直接区分"回调风暴"与
        // "某一次重绘自身卡住"。
        auto& stats = core::render::gpuResizeStats();
        stats.heartbeat("paint-enter");
        const double paintStarted = core::window::timeSeconds();
        paintResize();
        const double cost = core::window::timeSeconds() - paintStarted;
        stats.addResizePaintDuration(cost * 1000.0);
        // 只统计真正执行的刷新；防重入/限速跳过不计。
        stats.addLiveResizePaint();
        stats.heartbeat("paint-done");
        lastResizePaintTime = core::window::timeSeconds();
#if defined(EUI_WINDOW_BACKEND_WIN32)
        lastResizeCost = cost;
        const double interval = 1.0 / std::clamp(refreshRate(resizeWindow), 30.0, 500.0);
        resizeDeadline = paintStarted + std::max(interval, lastResizeCost);
        if (paintRequested) requestRefreshAfter(resizeWindow, std::max(0.0, resizeDeadline - lastResizePaintTime));
#endif
    }
};

struct ManagedWindow {
    Window* window = nullptr;
    WindowState state;
    app::DslWindowRuntime content;
    std::unique_ptr<core::render::RenderBackend> renderBackend;
};

struct TimerResolutionGuard {
    TimerResolutionGuard() {
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
    }

    ~TimerResolutionGuard() {
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    }
};

float getDpiScale(Window* window) {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    contentScale(window, &scaleX, &scaleY);
    return (scaleX + scaleY) * 0.5f;
}

float getPointerScale(Window* window) {
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    size(window, &windowWidth, &windowHeight);
    framebufferSize(window, &framebufferWidth, &framebufferHeight);

    if (windowWidth <= 0 || windowHeight <= 0) {
        return 1.0f;
    }

    const float scaleX = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
    const float scaleY = static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight);
    return (scaleX + scaleY) * 0.5f;
}

double getWindowRefreshRate(Window* window) { return refreshRate(window); }

void updateFrameInterval(Window* window, WindowState& windowState, double now, bool force = false) {
    windowState.updateFrameInterval(getWindowRefreshRate(window), now, force);
}

void waitForNextFrame(Window* window, const WindowState& windowState) {
    while (!shouldClose(window)) {
        const double remaining = windowState.nextFrameTime - core::window::timeSeconds();
        if (remaining <= 0.0) {
            break;
        }

        app::detail::waitForFrameDuration(remaining);
    }
}

void hideWindowToTray(Window* window, WindowState& windowState, core::render::RenderBackend& renderBackend) {
    if (!windowState.trayAvailable || windowState.hiddenToTray) {
        return;
    }

    core::render::ScopedRenderBackend scopedRenderBackend(renderBackend);
    app::releaseGraphicsResources();
    core::cancelInput(window);
    hide(window);
    windowState.hiddenToTray = true;
    windowState.hideToTrayRequested = false;
    windowState.paintRequested = false;
    windowState.renderedFrames = 0;
    windowState.nextFrameTime = core::window::timeSeconds();
}

void restoreWindowFromTray(Window* window, WindowState& windowState) {
    if (!windowState.hiddenToTray) {
        return;
    }

    restore(window);
    show(window);
    focus(window);
    windowState.hiddenToTray = false;
    windowState.hideToTrayRequested = false;
    windowState.paintRequested = true;
    app::detail::requestFullPaint();
    windowState.nextFrameTime = core::window::timeSeconds();
}

void installWindowCallbacks(Window* window, WindowState& windowState) {
#if defined(EUI_WINDOW_BACKEND_WIN32)
    windowState.resizeWindow = window;
#endif
    setUserPointer(window, &windowState);
    setSizeCallback(window, [](Window* currentWindow, int w, int h) {
        WindowState* state = static_cast<WindowState*>(userPointer(currentWindow));
        if (state == nullptr) {
            return;
        }
        core::render::gpuResizeStats().addFramebufferResizeCallback();
        core::render::gpuResizeStats().heartbeat("fb-cb");
        // Modal resize paints share the rate limit and the complete-frame reentry guard.
        state->paintRequested = true;
        if (w > 0 && h > 0) {
            app::detail::requestFullPaint();
            state->paintResizeIfDue();
        }
    });
    setRefreshCallback(window, [](Window* currentWindow) {
        WindowState* state = static_cast<WindowState*>(userPointer(currentWindow));
        if (state == nullptr) {
            return;
        }
        core::render::gpuResizeStats().addWindowRefreshCallback();
        core::render::gpuResizeStats().heartbeat("refresh-cb");
        // A native WM_PAINT or GLFW refresh uses the same bounded callback path.
#if defined(EUI_WINDOW_BACKEND_WIN32)
        // Exposure and the final modal-size frame must not wait behind the
        // dragging deadline. Reentry protection still defers nested callbacks.
        if (!inSizeMove(currentWindow)) state->resizeDeadline = 0;
#endif
        state->paintRequested = true;
#if !defined(EUI_WINDOW_BACKEND_WIN32)
        app::detail::requestFullPaint();
#endif
        state->paintResizeIfDue();
    });
    setScaleCallback(window, [](Window* currentWindow, float, float) {
        static_cast<WindowState*>(userPointer(currentWindow))->paintRequested = true;
        app::detail::requestFullPaint();
    });
    setFocusCallback(window, [](Window* currentWindow, int focused) {
        WindowState* state = static_cast<WindowState*>(userPointer(currentWindow));
        if (!state) {
            return;
        }
        if (focused != True) {
            core::cancelInput(currentWindow);
        }
        state->paintRequested = true;
        if (focused && state->modalChildWindow != nullptr && !shouldClose(state->modalChildWindow)) {
            focus(state->modalChildWindow);
        }
    });
    setIconifyCallback(window, [](Window* currentWindow, int iconified) {
        WindowState* state = static_cast<WindowState*>(userPointer(currentWindow));
        if (!state) {
            return;
        }
        state->iconified = iconified == True;
        if (state->iconified) {
            core::cancelInput(currentWindow);
        } else {
            state->paintRequested = true;
        }
    });
}

std::unique_ptr<ManagedWindow> createManagedWindow(const app::DslWindowRequest& request,
                                                   Window* parentWindow,
                                                   core::render::RenderBackend& shareBackend) {
    core::window::WindowCreateRequest windowRequest;
    windowRequest.width = request.width;
    windowRequest.height = request.height;
    windowRequest.title = request.title.c_str();
    windowRequest.parent = parentWindow;
    windowRequest.renderApi = core::render::windowRenderApi();
    Window* childWindow = static_cast<Window*>(core::window::createWindow(windowRequest));
    if (!childWindow) {
        return {};
    }

    auto managed = std::make_unique<ManagedWindow>();
    managed->window = childWindow;
    managed->renderBackend = core::render::createRenderBackend(childWindow, &shareBackend);
    if (!managed->renderBackend) {
        core::window::destroyWindow(childWindow);
        return {};
    }
    if (!managed->renderBackend->initialize()) {
        core::window::destroyWindow(childWindow);
        return {};
    }
    managed->state.lastTitleUpdate = core::window::timeSeconds();
    managed->state.nextFrameTime = managed->state.lastTitleUpdate;
    installWindowCallbacks(childWindow, managed->state);

    if (!managed->content.initialize(childWindow, request)) {
        managed->renderBackend.reset();
        core::releaseInputQueue(childWindow);
        core::window::destroyWindow(childWindow);
        return {};
    }

    managed->state.paintRequested = true;
    if (managed->content.request().modal) {
        focus(childWindow);
    }
    return managed;
}

void destroyManagedWindow(std::unique_ptr<ManagedWindow>& managed) {
    if (!managed || managed->window == nullptr) {
        managed.reset();
        return;
    }

    Window* windowToDestroy = managed->window;
    if (managed->renderBackend) {
        managed->renderBackend->makeCurrent();
        managed->renderBackend->releaseRenderCache();
    }
    core::releaseInputQueue(windowToDestroy);
    if (managed->renderBackend) {
        core::render::ScopedRenderBackend scopedRenderBackend(*managed->renderBackend);
        managed->content.shutdown(false);
    } else {
        managed->content.shutdown(false);
    }
    managed->renderBackend.reset();
    core::window::destroyWindow(windowToDestroy);
    managed.reset();
}

bool updateManagedWindow(ManagedWindow& managed, float deltaSeconds, bool updateRequested) {
    if (managed.window == nullptr || shouldClose(managed.window)) {
        return false;
    }

    managed.renderBackend->makeCurrent();

    managed.state.iconified = attribute(managed.window, Iconified) == True;
    if (managed.state.iconified) {
        managed.renderBackend->releaseRenderCache();
        managed.state.paintRequested = true;
        managed.content.requestFullPaint();
        managed.state.resetTiming(core::window::timeSeconds());
        return true;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    framebufferSize(managed.window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        managed.renderBackend->releaseRenderCache();
        managed.state.paintRequested = true;
        managed.content.requestFullPaint();
        managed.state.resetTiming(core::window::timeSeconds());
        return true;
    }

    const float dpiScale = getDpiScale(managed.window);
    const float pointerScale = getPointerScale(managed.window);
    const float logicalWidth = static_cast<float>(framebufferWidth) / dpiScale;
    const float logicalHeight = static_cast<float>(framebufferHeight) / dpiScale;

    core::render::ScopedRenderBackend scopedRenderBackend(*managed.renderBackend);
    if (managed.content.update(managed.window, deltaSeconds, logicalWidth, logicalHeight, pointerScale, dpiScale, updateRequested)) {
        managed.state.paintRequested = true;
    }

    if (managed.state.paintRequested || managed.content.paintRequested()) {
        managed.renderBackend->beginFrame({
            managed.window,
            core::window::nativeWindowInfo(managed.window),
            framebufferWidth,
            framebufferHeight,
            dpiScale
        });
        if (!managed.renderBackend->frameReady()) {
            managed.state.paintRequested = true;
            managed.content.requestFullPaint();
            core::platform::requestUiUpdate();
            return true;
        }
        managed.content.render(*managed.renderBackend, framebufferWidth, framebufferHeight, dpiScale);
        managed.renderBackend->present();
        if (managed.renderBackend->framePresented()) {
            managed.state.paintRequested = false;
            ++managed.state.renderedFrames;
        } else {
            managed.state.paintRequested = true;
            managed.content.requestFullPaint();
            core::platform::requestUiUpdate();
        }
    }
    return true;
}

bool isManagedWindowClosed(const ManagedWindow& managed) {
    return managed.window == nullptr || shouldClose(managed.window);
}

bool isManagedWindowRenderable(const ManagedWindow& managed) {
    if (managed.window == nullptr || shouldClose(managed.window)) {
        return false;
    }
    if (managed.state.iconified || attribute(managed.window, Iconified) == True) {
        return false;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    framebufferSize(managed.window, &framebufferWidth, &framebufferHeight);
    return framebufferWidth > 0 && framebufferHeight > 0;
}

bool anyRenderableManagedWindowAnimating(const app::DslWindowManager<ManagedWindow>& windows) {
    return windows.anyAnimating(isManagedWindowRenderable);
}

void pruneClosedWindows(app::DslWindowManager<ManagedWindow>& windows) {
    windows.pruneClosed(isManagedWindowClosed, destroyManagedWindow);
}

void createRequestedWindows(app::DslWindowManager<ManagedWindow>& windows,
                            Window* shareWindow,
                            core::render::RenderBackend& shareBackend,
                            const std::vector<app::DslWindowRequest>& requests) {
    windows.createPending(requests, [&](const app::DslWindowRequest& request) {
        return createManagedWindow(request, shareWindow, shareBackend);
    });
    shareBackend.makeCurrent();
}

Window* findModalChildWindow(app::DslWindowManager<ManagedWindow>& windows) {
    ManagedWindow* managed = windows.modalWindow(isManagedWindowClosed);
    return managed != nullptr ? managed->window : nullptr;
}

int eui_app_run() {
    core::platform::repairCurrentWorkingDirectory();
    core::render::initializeRenderBackendLoader();
    if (!initialize()) {
        return -1;
    }
    TimerResolutionGuard timerResolution;

    core::window::WindowCreateRequest windowRequest;
    windowRequest.width = app::initialWindowWidth();
    windowRequest.height = app::initialWindowHeight();
    windowRequest.x = app::initialWindowX();
    windowRequest.y = app::initialWindowY();
    windowRequest.positionSet = app::initialWindowPositionSet();
    windowRequest.minWidth = app::minimumWindowWidth();
    windowRequest.minHeight = app::minimumWindowHeight();
    windowRequest.maxWidth = app::maximumWindowWidth();
    windowRequest.maxHeight = app::maximumWindowHeight();
    windowRequest.resizable = app::windowResizable();
    windowRequest.highDpi = app::windowHighDpi();
    windowRequest.decorated = app::windowDecorated();
    windowRequest.alwaysOnTop = app::windowAlwaysOnTop();
    windowRequest.maximized = app::windowMaximized();
    windowRequest.title = app::windowTitle();
    windowRequest.renderApi = core::render::windowRenderApi();
    Window* window = static_cast<Window*>(core::window::createWindow(windowRequest));
    if (!window) {
        shutdownHost();
        return -1;
    }

    WindowState windowState;
    windowState.resetTiming(core::window::timeSeconds());
    updateFrameInterval(window, windowState, windowState.lastTitleUpdate, true);
    if (app::showDebugStatsInTitle()) {
        char title[128];
        std::snprintf(title, sizeof(title), "%s - 0 FPS", app::windowTitle());
        setTitle(window, title);
    }
    installWindowCallbacks(window, windowState);

    const auto cleanupMainWindow = [&] {
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        shutdownHost();
    };

    auto renderBackend = core::render::createRenderBackend(window);
    if (!renderBackend) {
        cleanupMainWindow();
        return -1;
    }
    if (!renderBackend->initialize()) {
        cleanupMainWindow();
        return -1;
    }

    if (!app::initialize(window)) {
        app::shutdown();
        renderBackend.reset();
        cleanupMainWindow();
        return -1;
    }
    app::MainWindowRuntime mainWindowRuntime(windowState);
    windowState.initializeTray();
    setCloseCallback(window, [](Window* currentWindow) {
        WindowState* state = static_cast<WindowState*>(userPointer(currentWindow));
        if (state && state->modalChildWindow != nullptr && !shouldClose(state->modalChildWindow)) {
            focus(state->modalChildWindow);
            setShouldClose(currentWindow, False);
            return;
        }
        if (state && state->trayAvailable && !state->forceClose) {
            state->hideToTrayRequested = true;
            setShouldClose(currentWindow, False);
            return;
        }
        if (!app::allowCloseRequest()) {
            setShouldClose(currentWindow, False);
            app::requestUpdate();
        }
    });
    setIconifyCallback(window, [](Window* currentWindow, int iconified) {
        WindowState* state = static_cast<WindowState*>(userPointer(currentWindow));
        if (!state) {
            return;
        }
        state->iconified = iconified == True;
        if (iconified) {
            core::cancelInput(currentWindow);
        } else {
            state->paintRequested = true;
            app::detail::requestFullPaint();
        }
    });

    app::DslWindowManager<ManagedWindow> childWindows;

#ifdef _WIN32
    windowState.paintResize = [&] {
        if (windowState.hiddenToTray || attribute(window, Iconified) == True ||
            shouldClose(window)) {
            return;
        }
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        framebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            return;
        }

        // runFrame owns context binding. Its reentry guard must run before any
        // context switch, including callbacks dispatched while child windows draw.
        mainWindowRuntime.runFrame(
            window,
            *renderBackend,
            {framebufferWidth, framebufferHeight, getDpiScale(window), getPointerScale(window)},
            core::window::timeSeconds(),
            getWindowRefreshRate(window),
            false,
            [&] { windowState.modalChildWindow = findModalChildWindow(childWindows); },
            [](float, bool) {},
            [](const char*) {},
            [&] { return false; }
#if defined(EUI_WINDOW_BACKEND_WIN32)
            , false
#endif
        );
    };
    // 判定逻辑抽在 core/app/live_resize_option.h（纯函数，单测覆盖
    // unset/0/1/非法/多字符）。这里的取值方式保持与原实现等价：
    // GetEnvironmentVariableW(buffer=2) 只有在值恰好 1 个字符时返回 1，
    // 多字符值返回所需缓冲大小（>=3），未设置/空值返回 0。
    wchar_t liveResizeOption[2]{};
    char liveResizeOptionNarrow[2]{};
    if (GetEnvironmentVariableW(L"NEO_LIVE_RESIZE", liveResizeOption, 2) == 1 &&
        liveResizeOption[0] < 0x80) {
        liveResizeOptionNarrow[0] = static_cast<char>(liveResizeOption[0]);
    }
    if (app::liveResizeDisabled(liveResizeOptionNarrow)) {
        windowState.paintResize = {};
#if defined(EUI_WINDOW_BACKEND_WIN32)
        setLiveResize(window, false);
#endif
    }
#endif

    while (!shouldClose(window)) {
        renderBackend->makeCurrent();
        windowState.pollTray(false);
        if (windowState.consumeTrayExitRequested() || app::consumeCloseRequest()) {
            if (app::allowCloseRequest()) {
                windowState.forceClose = true;
                setShouldClose(window, True);
                break;
            }
            restoreWindowFromTray(window, windowState);
        }
        if (windowState.consumeTrayShowRequested()) {
            restoreWindowFromTray(window, windowState);
        }
        pruneClosedWindows(childWindows);
        windowState.modalChildWindow = findModalChildWindow(childWindows);
        if (windowState.hideToTrayRequested && !childWindows.empty()) {
            windowState.hideToTrayRequested = false;
        }
        if (windowState.hideToTrayRequested) {
            renderBackend->releaseRenderCache();
            hideWindowToTray(window, windowState, *renderBackend);
        }
        if (windowState.hiddenToTray) {
            waitEventsTimeout(0.10);
            windowState.resetTiming(core::window::timeSeconds());
            continue;
        }

        windowState.iconified = attribute(window, Iconified) == True;
        if (windowState.iconified) {
            renderBackend->releaseRenderCache();
            windowState.paintRequested = false;
            windowState.consumeFrameRequest();
            windowState.resetTiming(core::window::timeSeconds());
            waitEventsTimeout(0.25);
            continue;
        }

        if (windowState.anyAnimating(anyRenderableManagedWindowAnimating(childWindows))) {
            waitForNextFrame(window, windowState);
        }

        const double currentFrameTime = core::window::timeSeconds();

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        framebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            renderBackend->releaseRenderCache();
            windowState.paintRequested = true;
            app::detail::requestFullPaint();
            windowState.consumeFrameRequest();
            waitEvents();
            mainWindowRuntime.markUnavailableFrame(core::window::timeSeconds());
            continue;
        }

        const float dpiScale = getDpiScale(window);
        const float pointerScale = getPointerScale(window);
        const bool mainInputEnabled = windowState.modalChildWindow == nullptr;

        // 卡死取证：正常主循环帧也留心跳，用来区分"卡在模态 resize 回调里"
        // 与"卡在普通帧绘制里"。
        core::render::gpuResizeStats().heartbeat("main-frame");

        mainWindowRuntime.runFrame(
            window,
            *renderBackend,
            {framebufferWidth, framebufferHeight, dpiScale, pointerScale},
            currentFrameTime,
            getWindowRefreshRate(window),
            mainInputEnabled,
            [&] {
                createRequestedWindows(childWindows, window, *renderBackend, app::consumeWindowRequests());
                pruneClosedWindows(childWindows);
                windowState.modalChildWindow = findModalChildWindow(childWindows);
            },
            [&](float frameDelta, bool updateRequested) {
                childWindows.updateAll([&](ManagedWindow& managed) {
                    updateManagedWindow(managed, frameDelta, updateRequested);
                });

                createRequestedWindows(childWindows, window, *renderBackend, app::consumeWindowRequests());
                pruneClosedWindows(childWindows);
                windowState.modalChildWindow = findModalChildWindow(childWindows);
            },
            [&](const char* title) {
                setTitle(window, title);
            },
            [&] {
                return anyRenderableManagedWindowAnimating(childWindows);
            });

        const bool anyAnimating = windowState.anyAnimating(anyRenderableManagedWindowAnimating(childWindows));
        if (anyAnimating) {
            pollEvents();
        } else {
            // 没有动画、但有"到点要唤醒"的定时器时，带超时地睡到那个时刻；
            // 一个都没有才无限期等事件（这才是真正的空闲，CPU 归零）。
            const double wakeSeconds = mainWindowRuntime.idleWakeSeconds(
                core::window::timeSeconds(), childWindows.earliestTimerWakeSeconds(isManagedWindowRenderable));
            if (wakeSeconds <= 0.0) {
                pollEvents();
            } else if (std::isfinite(wakeSeconds)) {
                waitEventsTimeout(wakeSeconds);
            } else {
                waitEvents();
            }
        }
    }

    childWindows.destroyAll(destroyManagedWindow);
    core::releaseInputQueue(window);
    renderBackend->makeCurrent();
    renderBackend->releaseRenderCache();
    core::platform::shutdownTray();
    {
        core::render::ScopedRenderBackend scopedRenderBackend(*renderBackend);
        app::shutdown();
    }
    renderBackend.reset();
    core::window::destroyWindow(window);
    shutdownHost();
    // NEO_GPU_STATS=1 时打印一次会话级 resize/分配汇总；默认零开销空操作。
    core::render::gpuResizeStats().printSummary("desktop_app_main");
    return 0;
}

#ifndef EUI_APP_RUNNER_LIBRARY
int main() {
#if defined(EUI_EDITS_BUNDLED_RESOURCES) && defined(_WIN32)
    const int licenseCommandResult = core::platform::handleEuiEditsLicenseCommandLine();
    if (licenseCommandResult >= 0) return licenseCommandResult;
#endif
    // 单实例闸门必须在 initialize 之前：二次启动要在这里零成本退出。
    if (!app::singleInstanceGate()) {
        return 0;
    }
    return eui_app_run();
}
#endif
