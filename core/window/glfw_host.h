#pragma once
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <GLFW/glfw3native.h>
#endif
#include <algorithm>
namespace core::window::glfwHost {
using Window = GLFWwindow;
constexpr int True = GLFW_TRUE, False = GLFW_FALSE, Iconified = GLFW_ICONIFIED;
inline bool initialize() { return glfwInit() != 0; }
inline void shutdownHost() { glfwTerminate(); }
inline void pollEvents() { glfwPollEvents(); }
inline void waitEvents() { glfwWaitEvents(); }
inline void waitEventsTimeout(double t) { glfwWaitEventsTimeout(t); }
inline bool shouldClose(Window* w) { return glfwWindowShouldClose(w) != 0; }
inline void setShouldClose(Window* w, int v) { glfwSetWindowShouldClose(w,v); }
inline void setUserPointer(Window* w,void* v) { glfwSetWindowUserPointer(w,v); }
inline void* userPointer(Window* w) { return glfwGetWindowUserPointer(w); }
inline void setSizeCallback(Window* w,void(*f)(Window*,int,int)) { glfwSetFramebufferSizeCallback(w,f); }
inline void setRefreshCallback(Window* w,void(*f)(Window*)) { glfwSetWindowRefreshCallback(w,f); }
inline void setScaleCallback(Window* w,void(*f)(Window*,float,float)) { glfwSetWindowContentScaleCallback(w,f); }
inline void setFocusCallback(Window* w,void(*f)(Window*,int)) { glfwSetWindowFocusCallback(w,f); }
inline void setIconifyCallback(Window* w,void(*f)(Window*,int)) { glfwSetWindowIconifyCallback(w,f); }
inline void setCloseCallback(Window* w,void(*f)(Window*)) { glfwSetWindowCloseCallback(w,f); }
inline void contentScale(Window* w,float* x,float* y) { glfwGetWindowContentScale(w,x,y); }
inline void size(Window* w,int* x,int* y) { glfwGetWindowSize(w,x,y); }
inline void framebufferSize(Window* w,int* x,int* y) { glfwGetFramebufferSize(w,x,y); }
inline int attribute(Window* w,int v) { return glfwGetWindowAttrib(w,v); }
inline void setTitle(Window* w,const char* v) { glfwSetWindowTitle(w,v); }
inline void hide(Window* w) { glfwHideWindow(w); }
inline void show(Window* w) { glfwShowWindow(w); }
inline void restore(Window* w) { glfwRestoreWindow(w); }
inline void focus(Window* w) { glfwFocusWindow(w); }
inline GLFWmonitor* monitorForWindow(GLFWwindow* window) {
    if (GLFWmonitor* monitor = glfwGetWindowMonitor(window)) {
        return monitor;
    }

    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    GLFWmonitor* bestMonitor = glfwGetPrimaryMonitor();
    int bestArea = 0;

    for (int i = 0; i < monitorCount; ++i) {
        GLFWmonitor* monitor = monitors[i];
        int monitorX = 0;
        int monitorY = 0;
        glfwGetMonitorPos(monitor, &monitorX, &monitorY);
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (!mode) {
            continue;
        }

        const int overlapLeft = std::max(windowX, monitorX);
        const int overlapTop = std::max(windowY, monitorY);
        const int overlapRight = std::min(windowX + windowWidth, monitorX + mode->width);
        const int overlapBottom = std::min(windowY + windowHeight, monitorY + mode->height);
        const int overlapWidth = std::max(0, overlapRight - overlapLeft);
        const int overlapHeight = std::max(0, overlapBottom - overlapTop);
        const int overlapArea = overlapWidth * overlapHeight;
        if (overlapArea > bestArea) {
            bestArea = overlapArea;
            bestMonitor = monitor;
        }
    }

    return bestMonitor;
}

inline double refreshRate(GLFWwindow* window) {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    HMONITOR nativeMonitor = hwnd != nullptr ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (nativeMonitor != nullptr) {
        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(nativeMonitor, &monitorInfo)) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency > 1) {
                return static_cast<double>(mode.dmDisplayFrequency);
            }
        }
    }
#endif
    GLFWmonitor* monitor = monitorForWindow(window);
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (mode && mode->refreshRate > 0) {
        return static_cast<double>(mode->refreshRate);
    }
    return 60.0;
}

}
