#pragma once
#include "core/window/window_backend.h"
#include <windows.h>

// Native HWND host operations used by the desktop runner. No GLFW state or GL context.
namespace core::window::win32 {
using Window = void;
constexpr int True = 1, False = 0, Iconified = 1;
bool initialize();
void shutdownHost();
void pollEvents();
void waitEvents();
void waitEventsTimeout(double seconds);
bool shouldClose(Window* window);
void setShouldClose(Window* window, int close);
void setUserPointer(Window* window, void* data);
void* userPointer(Window* window);
void setSizeCallback(Window*, void (*)(Window*, int, int));
void setRefreshCallback(Window*, void (*)(Window*));
void setScaleCallback(Window*, void (*)(Window*, float, float));
void setFocusCallback(Window*, void (*)(Window*, int));
void setIconifyCallback(Window*, void (*)(Window*, int));
void setCloseCallback(Window*, void (*)(Window*));
void contentScale(Window*, float*, float*);
void size(Window*, int*, int*);
void framebufferSize(Window*, int*, int*);
int attribute(Window*, int);
void setTitle(Window*, const char*);
void hide(Window*);
void show(Window*);
void restore(Window*);
void focus(Window*);
double refreshRate(Window*);
// All called on the UI thread. Deferred refresh uses a coalesced one-shot timer.
void requestRefreshAfter(Window*, double seconds);
void cancelRefresh(Window*);
void setLiveResize(Window*, bool enabled);
void setPaintBackground(Window*, COLORREF color);
void notePresented(Window*, int width, int height);
void notePresentationLost(Window*);
bool inSizeMove(Window*);
}
