#pragma once

// NEO_LIVE_RESIZE 环境变量判定（B1 提取的纯函数）。
//
// 语义与 glfw_app_main.cpp 原有的 GetEnvironmentVariableW(buffer=2) 判定完全一致：
//   - 未设置            -> 返回 false（实时 resize 刷新保持开启）
//   - 精确 "0"          -> 返回 true （关闭拖拽期间的实时刷新，回到安全模式）
//   - "1" / 非法值 / 多字符（"01"、"true" 等）-> 返回 false（保持开启，约 30Hz 回调）
//
// 默认值不变：本函数只负责判定，是否改为 opt-in 属于另行裁决。

namespace app {

inline bool liveResizeDisabled(const char* value) {
    return value != nullptr && value[0] == '0' && value[1] == '\0';
}

} // namespace app
