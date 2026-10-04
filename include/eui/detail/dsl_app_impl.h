#pragma once

#include "eui/dsl_app.h"
#include "eui/network.h"

#include "3rd/stb_image.h"
#include "core/dsl_runtime.h"
#include "core/platform/platform.h"
#include "core/render/text.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace app {

namespace detail {

inline core::dsl::Runtime& dslRuntime() {
    static core::dsl::Runtime runtime;
    return runtime;
}

inline std::vector<DslWindowRequest>& dslWindowRequests() {
    static std::vector<DslWindowRequest> requests;
    return requests;
}

struct DslAppState {
    bool composed = false;
    bool iconApplied = false;
    bool closeRequested = false;
    float logicalWidth = 0.0f;
    float logicalHeight = 0.0f;
};

inline DslAppState& dslAppState() {
    static DslAppState state;
    return state;
}

inline std::string resolveIconPath(const std::string& iconPath) {
    if (iconPath.empty()) {
        return {};
    }

    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path requested(iconPath);
    const fs::path current = fs::current_path(error);
    std::vector<fs::path> candidates;
    candidates.push_back(requested);
    if (!error) {
        candidates.push_back(current / requested);
        candidates.push_back(current / "assets" / requested.filename());
    }

    fs::path executableDir;
#if defined(__APPLE__)
    char executablePath[4096];
    uint32_t executablePathSize = sizeof(executablePath);
    if (_NSGetExecutablePath(executablePath, &executablePathSize) == 0) {
        executableDir = fs::absolute(fs::path(executablePath), error).parent_path();
    }
#elif defined(_WIN32)
    char executablePath[MAX_PATH];
    const DWORD executablePathSize = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
    if (executablePathSize > 0 && executablePathSize < MAX_PATH) {
        executableDir = fs::absolute(fs::path(executablePath), error).parent_path();
    }
#elif defined(__linux__)
    char executablePath[4096];
    const ssize_t executablePathSize = readlink("/proc/self/exe", executablePath, sizeof(executablePath) - 1);
    if (executablePathSize > 0) {
        executablePath[executablePathSize] = '\0';
        executableDir = fs::absolute(fs::path(executablePath), error).parent_path();
    }
#endif
    if (!executableDir.empty()) {
        candidates.push_back(executableDir / requested);
        candidates.push_back(executableDir / "assets" / requested.filename());
    }

    for (const fs::path& candidate : candidates) {
        error.clear();
        if (fs::exists(candidate, error) && !error) {
            return fs::absolute(candidate, error).string();
        }
    }
    return {};
}

inline void applyWindowIcon(core::window::Handle window) {
    if (window == nullptr) {
        return;
    }

    const std::string iconPath = resolveIconPath(dslAppConfig().iconPathValue);
    if (iconPath.empty()) {
        return;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* pixels = stbi_load(iconPath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return;
    }

    core::window::setWindowIcon(window, width, height, pixels);
    stbi_image_free(pixels);
}

} // namespace detail

void openWindow(const DslWindowConfig& config, DslWindowCompose composeFn) {
    if (!composeFn) {
        return;
    }

    DslWindowRequest request;
    request.title = config.titleValue.empty() ? "Window" : config.titleValue;
    request.pageId = config.pageIdValue.empty() ? request.title : config.pageIdValue;
    request.clearColor = config.clearColorValue;
    request.width = std::max(160, config.windowWidthValue);
    request.height = std::max(120, config.windowHeightValue);
    request.modal = config.modalValue;
    request.onKeyEvent = config.keyEventHandler;
    request.compose = std::move(composeFn);
    detail::dslWindowRequests().push_back(std::move(request));
    requestUpdate();
}

void openWindow(const char* title, int width, int height, DslWindowCompose composeFn) {
    openWindow(DslWindowConfig{}
                   .title(title != nullptr ? title : "Window")
                   .pageId(title != nullptr ? title : "window")
                   .windowSize(width, height),
               std::move(composeFn));
}

std::vector<DslWindowRequest> consumeWindowRequests() {
    std::vector<DslWindowRequest> requests = std::move(detail::dslWindowRequests());
    detail::dslWindowRequests().clear();
    return requests;
}

const char* windowTitle() {
    return dslAppConfig().titleValue.c_str();
}

bool showDebugStatsInTitle() {
    return dslAppConfig().showDebugStatsInTitleValue;
}

double debugTitleUpdateInterval() {
    const double interval = dslAppConfig().debugTitleIntervalValue;
    return std::isfinite(interval) && interval > 0.0 ? interval : 1.0;
}

bool showDebugOverlay() {
    return dslAppConfig().showDebugOverlayValue &&
           static_cast<bool>(dslAppConfig().debugOverlayCompose);
}

double frameRateLimit() {
    return dslAppConfig().fpsValue;
}

int initialWindowWidth() {
    return dslAppConfig().windowWidthValue;
}

int initialWindowHeight() {
    return dslAppConfig().windowHeightValue;
}

int initialWindowX() {
    return dslAppConfig().windowXValue;
}

int initialWindowY() {
    return dslAppConfig().windowYValue;
}

bool initialWindowPositionSet() {
    return dslAppConfig().windowPositionSetValue;
}

int minimumWindowWidth() {
    return dslAppConfig().minWindowWidthValue;
}

int minimumWindowHeight() {
    return dslAppConfig().minWindowHeightValue;
}

int maximumWindowWidth() {
    return dslAppConfig().maxWindowWidthValue;
}

int maximumWindowHeight() {
    return dslAppConfig().maxWindowHeightValue;
}

bool windowResizable() {
    return dslAppConfig().resizableValue;
}

bool windowHighDpi() {
    return dslAppConfig().highDpiValue;
}

bool windowDecorated() {
    return dslAppConfig().decoratedValue;
}

bool windowAlwaysOnTop() {
    return dslAppConfig().alwaysOnTopValue;
}

bool windowMaximized() {
    return dslAppConfig().maximizedValue;
}

float uiScale() {
    const float configuredScale = dslAppConfig().uiScaleValue;
    return configuredScale > 0.0f ? configuredScale : 1.0f;
}

bool trayEnabled() {
    return dslAppConfig().trayEnabledValue;
}

const char* trayTitle() {
    const DslAppConfig& config = dslAppConfig();
    return (config.trayTitleValue.empty() ? config.titleValue : config.trayTitleValue).c_str();
}

const char* trayIconPath() {
    const DslAppConfig& config = dslAppConfig();
    return (config.trayIconPathValue.empty() ? config.iconPathValue : config.trayIconPathValue).c_str();
}

void requestClose() { detail::dslAppState().closeRequested = true; requestUpdate(); }
bool consumeCloseRequest() {
    bool value = detail::dslAppState().closeRequested;
    detail::dslAppState().closeRequested = false; return value;
}
bool allowCloseRequest() {
    const auto& handler = dslAppConfig().closeRequestHandler;
    return !handler || handler();
}

void requestUpdate() {
    core::platform::requestUiUpdate();
}

namespace detail {

void requestFullPaint() {
    dslRuntime().requestFullPaint();
    core::platform::requestUiUpdate();
}

} // namespace detail

bool initialize(core::window::Handle window) {
    const DslAppConfig& config = dslAppConfig();
    core::TextPrimitive::setDefaultFontFiles(config.textFontFileValue, config.iconFontFileValue);
    detail::dslRuntime().setKeyEventHandler(config.keyEventHandler);

    detail::DslAppState& state = detail::dslAppState();
    if (!state.iconApplied) {
        detail::applyWindowIcon(window);
        state.iconApplied = true;
    }
    if (!detail::dslRuntime().initialize(window)) return false;
    core::window::setMainWindowHandle(window);
    return true;
}

bool update(core::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale) {
    const bool asyncReady = core::async::dispatchReady();
    const bool updateRequested = core::platform::consumeUiUpdate();
    return update(window, deltaSeconds, windowWidth, windowHeight, dpiScale, pointerScale, updateRequested || asyncReady);
}

bool update(core::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale, bool updateRequested) {
    return update(window, deltaSeconds, windowWidth, windowHeight, dpiScale, pointerScale, updateRequested, true);
}

bool update(core::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale, bool updateRequested, bool inputEnabled) {
    if (windowWidth <= 0 || windowHeight <= 0 || dpiScale <= 0.0f) {
        return false;
    }

    const DslAppConfig& config = dslAppConfig();
    detail::dslRuntime().setInteractiveCursor(config.interactiveCursorValue);
    const float effectiveScale = dpiScale * uiScale();
    // 排版层（组件的 measureTextMetrics / DSL 固有尺寸）在逻辑像素空间工作，而渲染时
    // 交给 TextPrimitive 的是设备像素字号。带 hinting 的 advance 不是字号的线性函数
    // （FreeType 把它量化到整数像素），两边各取一次整就会差出百分之几到 10%。
    // 把缩放交给文本模块，它就能用渲染同款 ppem 去量、再除回逻辑单位 ——
    // 见 core/render/text.h 的 setLayoutPixelScale。
    core::TextPrimitive::setLayoutPixelScale(effectiveScale);
    const float logicalWidth = static_cast<float>(windowWidth) / effectiveScale;
    const float logicalHeight = static_cast<float>(windowHeight) / effectiveScale;
    detail::DslAppState& state = detail::dslAppState();

    const auto composeFrame = [&] {
        detail::dslRuntime().compose(config.pageIdValue, logicalWidth, logicalHeight, [](core::dsl::Ui& ui, const core::dsl::Screen& screen) {
            compose(ui, screen);
            const DslAppConfig& config = dslAppConfig();
            if (showDebugOverlay()) {
                config.debugOverlayCompose(ui, screen);
            }
        });
        state.composed = true;
        state.logicalWidth = logicalWidth;
        state.logicalHeight = logicalHeight;
    };

    if (!state.composed || state.logicalWidth != logicalWidth || state.logicalHeight != logicalHeight) {
        composeFrame();
    }

    bool changed = false;
    if (updateRequested) {
        composeFrame();
        changed = true;
    }

    changed = detail::dslRuntime().update(window, deltaSeconds, pointerScale, effectiveScale, inputEnabled) || changed;
    if (detail::dslRuntime().composeRequested()) {
        // A compose can change retained content without changing the element structure.
        // Rebuild the complete cache so state and release visuals update in this frame.
        detail::dslRuntime().requestFullPaint();
        composeFrame();
        changed = detail::dslRuntime().update(window, 0.0f, pointerScale, effectiveScale, inputEnabled) || changed;
        changed = true;
    }

    return changed;
}

bool isAnimating() {
    return detail::dslRuntime().isAnimating();
}

float nextTimerWakeSeconds() {
    return detail::dslRuntime().nextTimerWakeSeconds();
}

void render(int windowWidth, int windowHeight, float dpiScale) {
    if (windowWidth <= 0 || windowHeight <= 0 || dpiScale <= 0.0f) {
        return;
    }

    const DslAppConfig& config = dslAppConfig();
    const float effectiveScale = dpiScale * uiScale();
    core::TextPrimitive::setLayoutPixelScale(effectiveScale);  // 同 update()：见那里的说明
    detail::dslRuntime().render(windowWidth, windowHeight, effectiveScale, config.clearColorValue);
}

void releaseGraphicsResources() {
    detail::dslRuntime().releaseGraphicsResources();
}

void shutdown() {
    core::async::shutdown();
    if (dslAppConfig().shutdownHandler) dslAppConfig().shutdownHandler();
    detail::dslRuntime().shutdown();
    core::window::setMainWindowHandle(nullptr);
    eui::network::shutdown();
}

// 单实例闸门：main() 在任何初始化之前调用。默认恒放行（绝大多数应用没有单实例
// 语义）；需要它的应用经 detail::setSingleInstanceGate 注册实现（见 eui/app.h）。
// 本头文件只被 glue main（glfw/sdl2_app_main）包含，是单翻译单元实现。
namespace detail {
bool (*singleInstanceGateFn)() = nullptr;
void setSingleInstanceGate(bool (*gate)()) { singleInstanceGateFn = gate; }
} // namespace detail

bool singleInstanceGate() {
    return detail::singleInstanceGateFn == nullptr || detail::singleInstanceGateFn();
}

} // namespace app
