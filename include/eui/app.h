#pragma once

#include "eui/dsl.h"
#include "eui/types.h"
#include "eui/window.h"

#include <functional>
#include <string>
#include <vector>

namespace app {

using DslWindowCompose = std::function<void(eui::Ui&, const eui::Screen&)>;

struct DslWindowRequest {
    std::string title = "Window";
    std::string pageId = "window";
    eui::Color clearColor = {0.16f, 0.18f, 0.20f, 1.0f};
    int width = 640;
    int height = 420;
    bool modal = false;
    std::function<void(const eui::KeyEvent&)> onKeyEvent;
    DslWindowCompose compose;
};

const char* windowTitle();
bool showDebugStatsInTitle();
/**
 * @brief 单实例闸门：独立应用的 main() 里、任何初始化之前调用一次。
 *  默认实现恒返回 true（无单实例）；需要单实例的应用通过
 *  app::detail::setSingleInstanceGate 注册自己的实现（NeoEditor 走静态初始化注册，
 *  避免 main 直接引用强符号牵连其它应用链接失败）。
 *  返回 false = 已有实例在跑（命令行文档已转发、旧窗口已置前），进程立即退出。
 */
bool singleInstanceGate();
double debugTitleUpdateInterval();
bool showDebugOverlay();
double frameRateLimit();
int initialWindowWidth();
int initialWindowHeight();
int initialWindowX();
int initialWindowY();
bool initialWindowPositionSet();
int minimumWindowWidth();
int minimumWindowHeight();
int maximumWindowWidth();
int maximumWindowHeight();
bool windowResizable();
bool windowHighDpi();
bool windowDecorated();
bool windowAlwaysOnTop();
bool windowMaximized();
float uiScale();
bool trayEnabled();
const char* trayTitle();
const char* trayIconPath();
void requestUpdate();
// Close requests are processed on the main thread and may be vetoed by the app.
void requestClose();
bool consumeCloseRequest();
bool allowCloseRequest();
bool initialize(eui::window::Handle window);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale, bool updateRequested);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale, bool updateRequested, bool inputEnabled);
bool isAnimating();
/**
 * @brief 最近一个 pending 定时器（`onTimer`）还剩多少秒；没有定时器时返回 +inf。
 *
 * 供主循环"睡到定时器到点"用：定时器不该靠持续渲染来等时间。
 */
float nextTimerWakeSeconds();
void render(int windowWidth, int windowHeight, float dpiScale);
void releaseGraphicsResources();
void shutdown();
std::vector<DslWindowRequest> consumeWindowRequests();

namespace detail {
void requestFullPaint();
void setSingleInstanceGate(bool (*gate)());
}

} // namespace app
