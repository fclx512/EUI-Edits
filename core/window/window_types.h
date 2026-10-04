#pragma once

namespace core::window {

using Handle = void*;
using ContextKey = void*;
using CursorHandle = void*;

enum class CursorType {
    Arrow,
    Hand,
    IBeam
};

enum class RenderApi {
    OpenGL,
    Vulkan,
    Native
};

struct WindowCreateRequest {
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    bool positionSet = false;
    int minWidth = 0;
    int minHeight = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    const char* title = "";
    bool resizable = true;
    bool highDpi = true;
    bool decorated = true;
    bool alwaysOnTop = false;
    bool maximized = false;
    bool modal = false;
    Handle parent = nullptr;
    RenderApi renderApi = RenderApi::OpenGL;
};

struct NativeWindowInfo {
    Handle handle = nullptr;
    void* platformWindow = nullptr;
    void* platformDisplay = nullptr;
    void* platformView = nullptr;
};

} // namespace core::window
