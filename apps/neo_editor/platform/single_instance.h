#pragma once

#include <functional>
#include <string>

namespace neo::platform {

// 单实例（Windows）：第二个进程启动时立即退出，把命令行文档路径转发给
// 已运行的实例并把它带回前台；主实例被命名事件唤醒后，在下一帧 compose
// 的 tick 里消费转发的路径（requestOpenPath 走既有的脏文档确认流程）。
//
// 协议（名字固定，不含用户/路径哈希——temp 目录本身按用户隔离）：
//   mutex  Local\EUI-Edits.SingleInstance   拥有者 = 主实例，随进程存活
//   event  Local\EUI-Edits.OpenRequest      auto-reset，转发方 SetEvent 唤醒
//   file   %TEMP%\EUI-Edits.next-open       UTF-8 文档路径；空内容 = 只激活窗口
//
// 非 Windows：acquire 恒 true、take 恒空、shutdown 空操作（本功能不启用）。

/**
 * @brief 尝试成为主实例。成功（返回 true）后监听线程已在跑，onWake 会在
 *  **监听线程**上被调用——只能做"唤醒 UI"这类线程安全操作（契约同
 *  vault_watcher 的 onChange；app 层传 app::requestUpdate）。
 *  返回 false = 已有实例在跑：转发（把 forwardPath 写入路径文件 + SetEvent +
 *  置前旧窗口）已在本函数内完成，调用方应立即退出进程，不要再做任何初始化。
 */
bool singleInstanceAcquire(std::function<void()> onWake, const std::string& forwardPath = {});

/**
 * @brief 每帧 compose tick 消费：返回并删除转发的文档路径；没有请求时返回空。
 *  返回后由调用方走 requestOpenPath（脏文档确认、失败保留原稿都在那条链上）。
 */
std::string takeSingleInstanceOpenPath();

/** @brief 退出时回收监听线程与句柄。不调用也会随进程回收，主动调用只为干净。 */
void singleInstanceShutdown();

} // namespace neo::platform
