#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace neo::platform {

// 目录变更监视：后台线程用 ReadDirectoryChangesW 盯着 vault 根目录（含子树），
// 有变动就递增计数。UI 层每帧比对计数、防抖后重扫——文档库侧栏因此能实时反映
// 外部对目录的增删改（外部编辑器/脚本写文件不用手动点刷新）。
// 非 Windows 平台为空实现（计数恒 0，不启动线程）。
class VaultWatcher {
public:
    VaultWatcher();
    ~VaultWatcher();
    VaultWatcher(const VaultWatcher&) = delete;
    VaultWatcher& operator=(const VaultWatcher&) = delete;

    /**
     * @brief 监视 utf8Root（UTF-8 路径）。root 变化时自动重启线程；
     *  相同 root 且监视线程仍活着时是空操作；若线程已因目录不存在等原因自行退出，
     *  会在有界退避（≤1s）后 join 掉旧线程并重启——所以目录恢复后靠**重复调用**
     *  本函数（UI 每帧一次）即可让监视复活，不必换 root。空 root 等价于 stop()。
     */
    void watch(const std::string& utf8Root);

    /**
     * @brief 变动通知回调，在**监视线程**上调用（首次 watch 之前设置）。
     *  框架是按需渲染：回调里要唤醒主循环（如 requestUpdate），否则空闲时
     *  compose 不跑，UI 层的轮询 tick 永远没机会执行。
     */
    void onChange(std::function<void()> callback);

    /** @brief 停止监视并回收线程（析构自动调用）。 */
    void stop();

    /** @brief 自对象创建以来的变动总次数。UI 记住上次见到的值做比对。 */
    std::uint64_t changeCount() const;

#if defined(NEO_VAULT_WATCHER_TEST_HOOKS)
    /**
     * @brief 测试专用：手工制造一次「目录有变动」，等价于监视线程收到事件后的那次 bump。
     *  只在构建目标定义了 NEO_VAULT_WATCHER_TEST_HOOKS（tests/unit/vault_watcher.cpp）
     *  时才编进来，产品构建看不到这个符号。作用：并发测试要能在**任意平台**上从另一条
     *  线程持续灌事件——非 Windows 分支没有监视线程，光靠真实文件系统事件测不到竞争。
     *  与 onChange/stop 一样，只能从外部线程调用，不要在回调里重入本函数之外的启停 API。
     */
    void fireChangeForTest();
#endif

private:
    struct Impl;
    Impl* impl_;
};

} // namespace neo::platform
