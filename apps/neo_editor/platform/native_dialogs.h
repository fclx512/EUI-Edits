#pragma once

#include <string>
#include <vector>

namespace neo::dialogs {

struct PickResult {
    bool ok = false;
    bool cancelled = false;
    std::string path;
    std::string error;
};

// 框架只提供“打开文件”对话框，选目录和另存为自己补。
PickResult pickDirectory(const std::string& initialDirectory);

// Runs only for the private save-picker helper flag, before editor/session startup.
// Returns -1 for an ordinary launch, otherwise an exit code.
int runSaveDialogHelperIfRequested();

// extensionList 形如 {"md", "markdown", "txt"}，用于过滤器与默认扩展名。
PickResult pickSavePath(const std::string& initialDirectory,
                        const std::string& suggestedName,
                        const std::vector<std::string>& extensionList);

// 系统屏幕缩放（1.0 = 100%，1.25 = 125%）。框架不把它暴露给应用，
// 只能自己问系统。必须在 GLFW 初始化之后调用，否则进程还不带 DPI 感知，
// 拿到的永远是 1.0。
float systemScale();

} // namespace neo::dialogs
