#pragma once

#include "core/window/window_types.h"

#include <string>
#include <vector>

namespace core::window {

Handle createWindow(const WindowCreateRequest& request);
void destroyWindow(Handle window);
NativeWindowInfo nativeWindowInfo(Handle window);
#if defined(EUI_WINDOW_BACKEND_SDL2)
// SDL2 desktop Linux only: returns Xft.dpi / 96 for an X11 window,
// or 0.0f when SDL selected another video backend.
float x11ContentScale(Handle window);
#endif

ContextKey currentContextKey();
double timeSeconds();
void postEmptyEvent();

void getCursorPosition(Handle window, double& x, double& y);
std::string clipboardText(Handle window);
void setClipboardText(const std::string& text);

// 主窗口句柄登记（DSL 单窗口场景在 DslWindowRuntime::initialize 时写入）。
// 给拿不到 window 参数的应用层代码用（例如菜单命令里读剪贴板：SDL 后端的
// clipboardText 本就忽略句柄，GLFW 后端需要真句柄）。
Handle mainWindowHandle();
std::vector<std::string> consumeDroppedPaths(Handle window);
void setMainWindowHandle(Handle window);

CursorHandle createStandardCursor(CursorType type);
void setCursor(Handle window, CursorHandle cursor);
void destroyCursor(CursorHandle cursor);

void setWindowIcon(Handle window, int width, int height, unsigned char* pixels);
void setImeCursorRect(Handle window, float x, float y, float width, float height);
void installInputCallbacks(Handle window);
void uninstallInputCallbacks(Handle window);
bool queryImeComposition(Handle window, std::string& text, bool& composing);
// Cancel unconfirmed native preedit before transferring focus or moving its anchor.
void cancelImeComposition(Handle window);

} // namespace core::window
