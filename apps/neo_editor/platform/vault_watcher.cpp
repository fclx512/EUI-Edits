#include "platform/vault_watcher.h"

#include "model/text_file.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo::platform {
namespace {

// 变动通知器 = 原子计数 + 回调槽，Windows / 非 Windows 两个 Impl 共用这一份实现，
// 保证两边语义一致（非 Windows 没有监视线程：生产环境下 count() 恒 0、回调不触发）。
//
// ★ 锁纪律（T18 的核心）★
// notifyMutex 只保护回调槽这一个 shared_ptr，且**永远不与 Impl::mutex
// （watch/stop 的生命周期锁，stop() 会持它 join 监视线程）嵌套获取**：
//   · set()（onChange）：只拿 notifyMutex。新回调在锁外构造，锁内换指针，
//     被换下来的旧回调在锁外析构；
//   · bump()（监视线程收到事件）：只拿 notifyMutex 复制 shared_ptr，**立刻放锁**，
//     出锁之后才调用回调；
//   · 回调执行期间不持任何锁 → 回调里重入 onChange 不会自锁。
// 两把锁从不嵌套 ⇒ 没有锁顺序约束 ⇒ 不可能因这两把锁死锁。
// 反例（禁止的写法）：让 bump() 为了读回调去拿 Impl::mutex，就会形成
//   UI 线程：持 Impl::mutex → stopLocked() 里 join 等监视线程结束
//   监视线程：在 bump() 里等 Impl::mutex
// 两边互等，死锁。
struct ChangeNotifier {
    std::mutex notifyMutex;
    // 槽里放「不可变快照」：整体换指针而不是原地改写 std::function，并发的
    // snapshot() 拿到的永远是构造完成的对象，也不会出现「边改边被销毁」。
    std::shared_ptr<const std::function<void()>> notify;
    // 计数本身是原子的：UI 线程 changeCount() 并发读、监视线程 bump() 写，
    // 没有数据竞争（读用 relaxed 足够——UI 每帧都会重读，回调只是唤醒主循环）。
    std::atomic<std::uint64_t> changes{0};

    void set(std::function<void()> callback) {
        // 分配放在锁外，锁内只做两次指针交换。
        std::shared_ptr<const std::function<void()>> next =
            std::make_shared<std::function<void()>>(std::move(callback));
        std::shared_ptr<const std::function<void()>> previous;
        {
            std::lock_guard<std::mutex> lock(notifyMutex);
            previous = std::move(notify);
            notify = std::move(next);
        }
        // 被换下来的旧回调在锁外析构：清理逻辑不该跑在锁里。
    }

    std::shared_ptr<const std::function<void()>> snapshot() {
        std::lock_guard<std::mutex> lock(notifyMutex);
        return notify; // 返回时锁已释放——调用方拿到的是锁外的快照
    }

    void bump() {
        ++changes;
        const std::shared_ptr<const std::function<void()>> callback = snapshot();
        if (callback && *callback) {
            (*callback)(); // 不持锁调用
        }
    }

    std::uint64_t count() const { return changes.load(std::memory_order_relaxed); }
};

} // namespace

#if defined(_WIN32)

struct VaultWatcher::Impl {
    // 生命周期锁：只管 root / thread / dirHandle 的启停（watch / stop / stopLocked）。
    // 绝不与 ChangeNotifier::notifyMutex 嵌套获取，理由见文件头的锁纪律说明。
    std::mutex mutex;
    std::thread thread;
    std::string root;
    ChangeNotifier notifier;
    // 句柄由线程创建、stop()/线程收尾两边抢着关：atomic exchange 保证只关一次。
    std::atomic<HANDLE> dirHandle{INVALID_HANDLE_VALUE};
    // 停止握手（见 stopLocked 注释）：stopRequested 置位后监视线程在下一次进入
    // ReadDirectoryChangesW 之前自行退出；threadExited 由 run() 收尾处置位，
    // 供 stopLocked() 轮询，替代「一次 CancelIoEx + 直接 join」。
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> threadExited{true};
    // 退避状态：只在 watch() 持 mutex 时读写（与 root/thread 同一把锁）。
    // 背景：run() 里 CreateFileW/ReadDirectoryChangesW 失败会让线程自行退出，但
    // thread 对象在 join 之前一直 joinable——watch() 是**每帧**调的，同 root 且
    // joinable 就早退的话，目录恢复后监视永远不重启；反过来若每次都立刻重建，
    // 目录长期不存在时就是「每帧建/拆一次线程」的繁忙循环。所以同 root 但线程
    // 已退出时先看 retryAfter 窗口：窗口内直接返回，窗口过后才 join 掉旧线程再
    // 起新的；连续失败按 restartBackoff() 指数放大、封顶 1s（目录恢复后最多 1s
    // 出头就重启）。一旦观察到线程仍活着（正常实时监听），两者一起清零。
    int consecutiveFailures = 0;
    std::chrono::steady_clock::time_point retryAfter{};

    // 第 n 次（n≥1）检测到「线程自行退出」后的重建间隔：100ms 起步、逐次翻倍、封顶 1s。
    static std::chrono::milliseconds restartBackoff(int failures) {
        long long ms = 100;
        for (int i = 1; i < failures && ms < 1000; ++i) {
            ms *= 2;
        }
        if (ms > 1000) {
            ms = 1000;
        }
        return std::chrono::milliseconds(static_cast<long long>(ms));
    }

    void run(const std::string& utf8Root) {
        const HANDLE dir = CreateFileW(textfile::pathFromUtf8(utf8Root).wstring().c_str(),
                                       FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dir != INVALID_HANDLE_VALUE) {
            dirHandle.store(dir);
            std::vector<std::uint8_t> buffer(64 * 1024);
            for (;;) {
                // 收到停止请求就退出——**必须**在每次进入阻塞读之前检查：
                // 只靠 CancelIoEx 有漏网窗口（见 stopLocked）。
                if (stopRequested.load(std::memory_order_acquire)) {
                    break;
                }
                DWORD bytes = 0;
                const BOOL ok = ReadDirectoryChangesW(
                    dir, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                    &bytes, nullptr, nullptr);
                if (!ok) {
                    break;  // stop() 取消或目录句柄失效
                }
                // bytes==0 表示缓冲区溢出——变动太密，同样算一次变动。
                notifier.bump();
            }
            // exchange 决定唯一持有者：stopLocked() 若已取走句柄，这里拿到 INVALID，
            // 关闭责任归 stopLocked()；否则由本线程关。
            const HANDLE handle = dirHandle.exchange(INVALID_HANDLE_VALUE);
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
        }
        // 走到这里可能是 CreateFileW 失败（目录还不存在）或读失败：线程就此退出，
        // thread 仍 joinable。watch() 下一次同 root 调用会看到 threadExited=true，
        // 经退避窗口后 join 掉本线程再重建（见 watch()/restartBackoff）。
        threadExited.store(true, std::memory_order_release);
    }

    void stopLocked() {
        // 先置请求再取句柄：若监视线程还没轮到 store(dirHandle)（拿到 INVALID），
        // 它会自己看到 stopRequested 而退出；反之句柄归我们，由下面反复取消。
        stopRequested.store(true);
        HANDLE handle = dirHandle.exchange(INVALID_HANDLE_VALUE);
        if (thread.joinable()) {
            // CancelIoEx 只能取消**此刻已挂起**的 I/O。监视线程可能正处在两次
            // ReadDirectoryChangesW 之间（在 bump() 里，或刚从上一个事件返回），
            // 这一次取消会落空（ERROR_NOT_FOUND），它随后再进入阻塞读就再没人叫醒
            // 它 → 直接 join 会永久挂死。所以持着句柄反复取消，直到 run() 收尾。
            //（这是 stop()/run() 的既有竞态，与回调锁无关：原实现是「单次 CancelIoEx
            //  + 立即 join」，事件密集时必然踩中。）
            for (;;) {
                if (handle == INVALID_HANDLE_VALUE) {
                    // 线程可能还没 publish 句柄（exchange 抢在 store 之前）：
                    // 一旦它 publish 就立刻接管，否则没人取消得了它。
                    handle = dirHandle.exchange(INVALID_HANDLE_VALUE);
                }
                if (handle != INVALID_HANDLE_VALUE) {
                    CancelIoEx(handle, nullptr);
                }
                if (threadExited.load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            thread.join();
        }
        if (handle != INVALID_HANDLE_VALUE) {
            // 线程已 join，绝不会再碰这个句柄；原实现只 CancelIoEx 不关，每次 stop 都漏一个。
            CloseHandle(handle);
        }
        stopRequested.store(false);
        root.clear();
    }
};

void VaultWatcher::watch(const std::string& utf8Root) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (utf8Root.empty()) {
        impl_->stopLocked();  // 空 root = 停止（与旧语义一致）
        return;
    }
    // 只看 joinable() 不够：run() 里 CreateFileW/ReadDirectoryChangesW 失败时线程
    // 会自行退出并置 threadExited，但 join 之前 thread 依旧 joinable。少了下面这
    // 一条，同 root 的 watch() 会永远早退，目录恢复后监视永不重启（T18 HIGH-1）。
    const bool running = impl_->thread.joinable() &&
                         !impl_->threadExited.load(std::memory_order_acquire);
    if (impl_->root == utf8Root && running) {
        // 正常监听中：维持旧的「同 root 空操作」，顺带清掉退避状态。
        impl_->consecutiveFailures = 0;
        impl_->retryAfter = std::chrono::steady_clock::time_point{};
        return;
    }
    if (impl_->root == utf8Root && impl_->thread.joinable()) {
        // 同 root 但线程已自行退出（多为目录还不存在）：退避窗口内不重建，
        // 否则每帧 watch() 都会 join + 起新线程，形成繁忙循环。
        const auto now = std::chrono::steady_clock::now();
        if (now < impl_->retryAfter) {
            return;
        }
        if (impl_->consecutiveFailures < 32) {
            ++impl_->consecutiveFailures;  // 封顶只为防长期失败下的计数溢出
        }
        impl_->retryAfter = now + Impl::restartBackoff(impl_->consecutiveFailures);
    } else {
        // root 变化或首启：显式换目录永远给一次立即尝试（退避只拦「原地重试」）。
        impl_->consecutiveFailures = 0;
        impl_->retryAfter = std::chrono::steady_clock::time_point{};
    }
    impl_->stopLocked();  // join 旧线程（已退出则立即返回）+ 关残留句柄 + 清 root
    impl_->root = utf8Root;
    // 新线程重新武装握手：threadExited 上一轮收尾时已被置真，不重置的话
    // 下一次 stop() 会以为线程已退出、只取消一次就 join，又回到漏取消的挂死路径。
    impl_->threadExited.store(false);
    impl_->stopRequested.store(false);
    impl_->thread = std::thread([impl = impl_, root = utf8Root] { impl->run(root); });
}

void VaultWatcher::stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopLocked();
}

#else

struct VaultWatcher::Impl {
    // 与 Windows 分支共用 ChangeNotifier：回调槽 / 计数的并发规则一字不差。
    // 这里没有监视线程（watch/stop 是空操作）：生产环境下没有事件源，
    // count() 恒 0、回调不会被触发——语义与旧实现一致。
    ChangeNotifier notifier;
};

void VaultWatcher::watch(const std::string&) {}
void VaultWatcher::stop() {}

#endif

VaultWatcher::VaultWatcher() : impl_(new Impl()) {}

VaultWatcher::~VaultWatcher() {
    stop();
    delete impl_;
}

void VaultWatcher::onChange(std::function<void()> callback) {
    impl_->notifier.set(std::move(callback));
}

std::uint64_t VaultWatcher::changeCount() const {
    return impl_->notifier.count();
}

#if defined(NEO_VAULT_WATCHER_TEST_HOOKS)
// 测试钩子：不经过文件系统地制造一次「目录有变动」，走监视线程收到事件时的
// 同一条 bump() 路径。只在测试目标里定义 NEO_VAULT_WATCHER_TEST_HOOKS 时编入，
// 产品构建没有这个符号；没有它就只能靠真实目录事件驱动 bump，非 Windows 分支
// （没有监视线程）会完全测不到。
void VaultWatcher::fireChangeForTest() {
    impl_->notifier.bump();
}
#endif

} // namespace neo::platform
