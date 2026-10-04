// T18：VaultWatcher（apps/neo_editor/platform/vault_watcher.cpp）的并发压测。
//
// 先说清楚：这份测试**不是** ThreadSanitizer。本机/CI 的 MSVC 没有 TSan，跑不了
// 数据竞争检测器，所以这里做的是两件事：
//   1) 竞态结构审查（代码层面，详见 vault_watcher.cpp 文件头的「锁纪律」注释）：
//        · onChange 只在独立的 notifyMutex 下换 shared_ptr<const std::function>；
//        · bump 只在 notifyMutex 下复制 shared_ptr，**出锁后**才调用回调；
//        · notifyMutex 与 watch/stop 的生命周期锁 impl_->mutex 互不嵌套；
//        · changes 计数本身是 std::atomic，无数据竞争。
//   2) 无崩溃 / 无死锁压测（运行层面）：多线程互灌事件 + 换回调 + 启停，
//      每个场景都套超时看门狗，到点没跑完就判定死锁、非零退出。
//
// 三条覆盖（对应 T18 要求）：
//   A. 一个线程连换 ≥1000 次回调，另一条线程持续灌事件 → 不崩，且能看到某个
//      有效回调 +「最后装进去的最新回调」100% 命中；
//   B. watch(A) → watch(B) → stop()/join 与事件灌入并发 → 不死锁（带超时）；
//   C. 回调里重入 onChange → 不死锁（「回调在锁外被调用」的直接验证）。
//
// HIGH-1 回归（真实文件系统，不走 fireChangeForTest 合成事件）：
//   D. 目录不存在 → 线程因 CreateFileW 失败自行退出 → 创建目录 → 同一路径 watch()
//      → 真实文件变更触发 changeCount/回调（证明监视线程真的复活）；
//   E. stop() → 立刻同 root watch() 循环 → 不死锁，且启停后真实监听仍可用；
//   F. 无间隔 watch(A)→watch(B) 高频切换（+ 并发灌事件）→ 不死锁。
//
// 构建：随主工程 EUI_BUILD_TEST_FIXTURES=ON 一起编（CMakeLists.txt 给这个目标
// 额外挂了 apps/neo_editor/platform/vault_watcher.cpp、model/text_file.cpp、
// apps/neo_editor include 目录，并定义 NEO_VAULT_WATCHER_TEST_HOOKS 打开
// fireChangeForTest 测试钩子——非 Windows 分支没有监视线程，没有钩子灌不进事件，
// onChange/bump 的并发就测不到）。

#include "model/text_file.h"
#include "platform/vault_watcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (ok) {
        std::printf("  [ok] %s\n", what.c_str());
    } else {
        ++gFailures;
        std::printf("  [!!] %s\n", what.c_str());
    }
}

// 超时看门狗：把一段压测放进工作线程跑，主线程限时等；到点还没结束就判死锁。
// 判死后不能 join（会永远卡住），也不能跑正常退出路径（局部量还被工作线程踩着），
// 所以刷完输出直接 _Exit 非零，让 ctest 记失败。
void runWithTimeout(const std::string& name, const std::function<void()>& body,
                    std::chrono::seconds limit) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    std::thread worker([&] {
        body();
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, limit, [&] { return done; })) {
            lock.unlock();
            worker.detach();
            std::fprintf(stderr, "  [!!] %s：超过 %lld 秒未结束，判定为死锁/挂起\n", name.c_str(),
                         static_cast<long long>(limit.count()));
            std::fflush(nullptr);
            std::_Exit(EXIT_FAILURE);
        }
    }
    worker.join();
}

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& leaf) {
        std::error_code error;
        path = std::filesystem::temp_directory_path(error) / leaf;
        if (error) {
            path.clear();
            return;
        }
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }

    ~TempDir() {
        std::error_code error;
        if (!path.empty()) {
            std::filesystem::remove_all(path, error);
        }
    }
};

void touchFile(const std::filesystem::path& dir, int index) {
    const std::filesystem::path file = dir / ("w" + std::to_string(index % 8) + ".md");
    std::ofstream output(file, std::ios::binary | std::ios::app);
    output << "x";
    output.close();
    if ((index % 3) == 0) {
        std::error_code error;
        std::filesystem::remove(file, error);
    }
}

// A：一个线程连换 ≥1000 次回调，另一条线程持续灌事件。
void scenarioSwapAgainstPump() {
    std::atomic<int> observedGen{-1};
    std::atomic<long long> invoked{0};
    std::atomic<long long> hitsDuringSwap{0};
    std::atomic<bool> inSwapLoop{false};
    std::atomic<bool> stopPump{false};

    // watcher 放在共享状态之后：析构时那些状态还活着，回调引用不会悬空。
    neo::platform::VaultWatcher watcher;

    std::thread pump([&] {
        while (!stopPump.load(std::memory_order_relaxed)) {
            watcher.fireChangeForTest();
        }
    });

    inSwapLoop.store(true, std::memory_order_relaxed);
    int swaps = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
        watcher.onChange([&, gen = swaps] {
            observedGen.store(gen, std::memory_order_relaxed);
            invoked.fetch_add(1, std::memory_order_relaxed);
            if (inSwapLoop.load(std::memory_order_relaxed)) {
                hitsDuringSwap.fetch_add(1, std::memory_order_relaxed);
            }
        });
        ++swaps;
        if ((swaps % 32) == 0) {
            std::this_thread::yield();  // 给 pump 线程穿插进来的机会
        }
        if (swaps >= 1000 && hitsDuringSwap.load(std::memory_order_relaxed) > 0) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            break;
        }
    }
    inSwapLoop.store(false, std::memory_order_relaxed);
    stopPump.store(true, std::memory_order_relaxed);
    pump.join();

    const int lastGen = observedGen.load(std::memory_order_relaxed);
    const long long duringSwap = hitsDuringSwap.load(std::memory_order_relaxed);

    // 换完之后再装一个「最新回调」，灌 100 次事件，必须次次命中。
    std::atomic<int> finalHits{0};
    watcher.onChange([&] { finalHits.fetch_add(1, std::memory_order_relaxed); });
    for (int i = 0; i < 100; ++i) {
        watcher.fireChangeForTest();
    }

    check(swaps >= 1000, "A: 换回调至少 1000 次");
    check(duringSwap > 0, "A: 换回调的同时确有事件打进来（真发生过并发）");
    check(lastGen >= 0 && lastGen < swaps, "A: 被调到的是某个有效回调（gen 合法）");
    check(finalHits.load() == 100, "A: 最新回调 100 次事件全部命中");
    check(invoked.load() > 0, "A: 回调累计被调用过");
}

// B：watch(A) → watch(B) → stop()/join 与事件灌入并发，不死锁。
//
// 注意 watch() 之间留了 settle 间隔：现有实现里 stopLocked() 靠「监视线程先
// CreateFileW + dirHandle.store(dir)，stop() 再 exchange 取句柄去 CancelIoEx」
// 完成握手；两次 watch 挤在微秒级里时线程可能还没登记句柄，join 会等在无人取消的
// ReadDirectoryChangesW 上。这是 run()/stopLocked() 自带的**既有**启动竞态，与
// T18 的 notify 锁无关（不改锁也一样），所以这里只保证「句柄已登记」再往下走，
// 把要测的东西（bump 与 stop 的 join 并发）交由持续灌入的事件覆盖。
void scenarioWatchStopAgainstEvents() {
    TempDir root("neo_vault_watcher_t18");
    check(!root.path.empty(), "B: 拿到临时目录");
    if (root.path.empty()) {
        return;
    }
    const std::filesystem::path dirA = root.path / "a";
    const std::filesystem::path dirB = root.path / "b";
    std::error_code error;
    std::filesystem::create_directories(dirA, error);
    std::filesystem::create_directories(dirB, error);
    check(!error, "B: 建出子目录 A/B");
    if (error) {
        return;
    }

    std::atomic<long long> notified{0};
    std::atomic<bool> stopThreads{false};

    neo::platform::VaultWatcher watcher;
    watcher.onChange([&] { notified.fetch_add(1, std::memory_order_relaxed); });

    // 线程 1：持续灌合成事件，顺带换回调（onChange 与 watch/stop 并发）。
    std::thread pumper([&] {
        long long count = 0;
        while (!stopThreads.load(std::memory_order_relaxed)) {
            watcher.fireChangeForTest();
            if ((++count % 64) == 0) {
                watcher.onChange([&] { notified.fetch_add(1, std::memory_order_relaxed); });
            }
        }
    });

    // 线程 2：真实文件事件——Windows 上监视线程会真的 bump（走 run() 那条路）。
    std::thread writer([&] {
        int index = 0;
        while (!stopThreads.load(std::memory_order_relaxed)) {
            touchFile(dirA, index);
            touchFile(dirB, index + 1);
            ++index;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    const std::string rootA = neo::textfile::pathToUtf8(dirA);
    const std::string rootB = neo::textfile::pathToUtf8(dirB);
    const std::chrono::milliseconds settle(2);

    for (int i = 0; i < 60; ++i) {
        watcher.watch(rootA);
        std::this_thread::sleep_for(settle);
        watcher.watch(rootB);
        std::this_thread::sleep_for(settle);
        watcher.stop();
        std::this_thread::sleep_for(settle);
    }

    stopThreads.store(true, std::memory_order_relaxed);
    pumper.join();
    writer.join();
    watcher.stop();

    check(notified.load() > 0, "B: 事件灌入期间回调被调用过");
    check(watcher.changeCount() > 0, "B: changeCount 有增长");
}

// C：回调里重入 onChange 不死锁（回调是在锁外被调用的直接验证）。
void scenarioCallbackReentersOnChange() {
    std::atomic<int> calls{0};
    std::atomic<int> reinstalls{0};
    std::atomic<bool> stopPump{false};

    neo::platform::VaultWatcher watcher;

    // 每被调用一次就把「更年轻一代的自己」装回去；装的动作发生在回调体内。
    std::function<void(int)> install;
    install = [&](int gen) {
        watcher.onChange([&, gen] {
            calls.fetch_add(1, std::memory_order_relaxed);
            if (gen < 200) {
                install(gen + 1);  // ← 重入点：回调内再调 onChange
                reinstalls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    };
    install(0);

    std::thread pump([&] {
        while (!stopPump.load(std::memory_order_relaxed)) {
            watcher.fireChangeForTest();
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reinstalls.load(std::memory_order_relaxed) < 200 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stopPump.store(true, std::memory_order_relaxed);
    pump.join();
    watcher.onChange({});  // 断掉回调，之后不再有任何调用

    check(reinstalls.load() >= 200, "C: 回调内重入 onChange 至少 200 次没死锁");
    check(calls.load() > 0, "C: 回调被调用过");
}

// D：真实复现 HIGH-1——「目录不存在 → 监视线程退出 → 创建目录 → 同一路径 watch()
// → 真实文件变更触发 changeCount/回调」。
// 全程不碰 fireChangeForTest：合成 bump 只能证明回调槽还在，证明不了监视线程复活；
// 只有线程真的重新 CreateFileW 成功并进入 ReadDirectoryChangesW，写文件才会 bump。
void scenarioWatcherRevivesWhenRootReappears() {
#if !defined(_WIN32)
    // 非 Windows 分支 watch() 是空实现、没有监视线程，这条真实事件复现跑不了。
    std::printf("  [skip] D: 非 Windows 分支无监视线程，跳过真实事件复现\n");
    return;
#else
    TempDir root("neo_vault_watcher_t18_revive");
    check(!root.path.empty(), "D: 拿到临时目录");
    if (root.path.empty()) {
        return;
    }
    const std::filesystem::path vault = root.path / "vault";
    std::error_code error;
    std::filesystem::remove_all(vault, error);
    check(!std::filesystem::exists(vault), "D: 目标目录初始不存在");

    neo::platform::VaultWatcher watcher;
    std::atomic<long long> notified{0};
    watcher.onChange([&] { notified.fetch_add(1, std::memory_order_relaxed); });

    const std::string rootUtf8 = neo::textfile::pathToUtf8(vault);
    watcher.watch(rootUtf8);  // 目录不存在 → run() 里 CreateFileW 失败 → 线程自行退出
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    watcher.watch(rootUtf8);  // 同 root 第二次调用：HIGH-1 的早退点（旧代码永远不重启）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(watcher.changeCount() == 0, "D: 目录不存在期间没有产生任何事件");

    std::filesystem::create_directories(vault, error);
    check(!error, "D: 目录被创建出来（恢复）");
    if (error) {
        return;
    }

    // 模拟 UI 每帧调用 watch()：退避窗口过后，同一路径必须重启监视线程。
    const std::uint64_t baseline = watcher.changeCount();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int index = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        watcher.watch(rootUtf8);      // ← 与触发 bug 时完全相同的调用
        touchFile(vault, index++);    // 真实文件变更（不经过测试钩子）
        if (watcher.changeCount() > baseline && notified.load() > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    watcher.stop();

    check(watcher.changeCount() > baseline,
          "D: 目录恢复后同一路径 watch() 重启了监视线程（真实变更触发 bump）");
    check(notified.load() > 0, "D: 复活后的监视线程通过真实事件调用了回调");
#endif
}

// E：stop() → 立刻同 root watch()（HIGH-1 修复后的另一条重启路径）不死锁，
// 且启停循环之后真实监听仍然可用（退避状态不会把重启永久掐死）。
void scenarioStopThenImmediateWatch() {
    TempDir root("neo_vault_watcher_t18_stopwatch");
    check(!root.path.empty(), "E: 拿到临时目录");
    if (root.path.empty()) {
        return;
    }
    const std::filesystem::path vault = root.path / "v";
    std::error_code error;
    std::filesystem::create_directories(vault, error);
    check(!error, "E: 建出被监视目录");
    if (error) {
        return;
    }
    const std::string rootUtf8 = neo::textfile::pathToUtf8(vault);

    neo::platform::VaultWatcher watcher;
    std::atomic<long long> notified{0};
    watcher.onChange([&] { notified.fetch_add(1, std::memory_order_relaxed); });

    for (int i = 0; i < 150; ++i) {
        watcher.watch(rootUtf8);
        watcher.stop();
        watcher.watch(rootUtf8);  // stop 之后同 root 立刻 watch：必须真重启而非早退
        if ((i % 25) == 0) {
            touchFile(vault, i);
        }
    }

    // 最后一次 watch 之后，真实文件变更必须还能进来（监听确实恢复了）。
#if defined(_WIN32)
    const std::uint64_t baseline = watcher.changeCount();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int index = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        watcher.watch(rootUtf8);
        touchFile(vault, index++);
        if (watcher.changeCount() > baseline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    watcher.stop();

    check(watcher.changeCount() > baseline, "E: stop→watch 循环后真实监听仍然可用");
    check(notified.load() > 0, "E: 启停循环期间/之后回调被真实事件调用过");
#else
    watcher.stop();
    std::printf("  [skip] E: 非 Windows 分支无监视线程，跳过真实事件可用性断言\n");
#endif
}

// F：无间隔的 watch(A)→watch(B) 高频切换 + 并发灌事件，不死锁。
// 与 B 的区别：B 的切换之间留了 settle 间隔（等句柄登记），F 故意不留——
// 每次切换都可能在旧线程尚未 publish 句柄时就走 stopLocked 的 join，
// 这正是 stop/join 握手最容易挂死的形态。
void scenarioRapidWatchSwitch() {
    TempDir root("neo_vault_watcher_t18_rapid");
    check(!root.path.empty(), "F: 拿到临时目录");
    if (root.path.empty()) {
        return;
    }
    const std::filesystem::path dirA = root.path / "a";
    const std::filesystem::path dirB = root.path / "b";
    std::error_code error;
    std::filesystem::create_directories(dirA, error);
    std::filesystem::create_directories(dirB, error);
    check(!error, "F: 建出子目录 A/B");
    if (error) {
        return;
    }
    const std::string rootA = neo::textfile::pathToUtf8(dirA);
    const std::string rootB = neo::textfile::pathToUtf8(dirB);

    std::atomic<long long> notified{0};
    std::atomic<bool> stopThreads{false};

    neo::platform::VaultWatcher watcher;
    watcher.onChange([&] { notified.fetch_add(1, std::memory_order_relaxed); });

    std::thread pumper([&] {
        while (!stopThreads.load(std::memory_order_relaxed)) {
            watcher.fireChangeForTest();
        }
    });
    std::thread writer([&] {
        int index = 0;
        while (!stopThreads.load(std::memory_order_relaxed)) {
            touchFile(dirA, index);
            touchFile(dirB, index + 1);
            ++index;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (int i = 0; i < 300; ++i) {
        watcher.watch(rootA);  // 紧接着就切走：没有 settle，没有给句柄登记留时间
        watcher.watch(rootB);
        if ((i % 60) == 0) {
            watcher.stop();
        }
    }

    stopThreads.store(true, std::memory_order_relaxed);
    pumper.join();
    writer.join();
    watcher.stop();

    check(notified.load() > 0, "F: 高频切换期间回调被调用过");
    check(watcher.changeCount() > 0, "F: changeCount 有增长");
}

} // namespace

int main() {
    std::printf("T18 VaultWatcher 并发压测：竞态结构审查 + 无崩溃/无死锁压测（非 TSan，MSVC 无此工具）\n");

    runWithTimeout("A 换回调 × 灌事件", scenarioSwapAgainstPump, std::chrono::seconds(30));
    runWithTimeout("B watch/stop × 事件", scenarioWatchStopAgainstEvents, std::chrono::seconds(120));
    runWithTimeout("C 回调重入 onChange", scenarioCallbackReentersOnChange, std::chrono::seconds(60));
    runWithTimeout("D 目录恢复后监视重启", scenarioWatcherRevivesWhenRootReappears,
                   std::chrono::seconds(60));
    runWithTimeout("E stop→watch", scenarioStopThenImmediateWatch, std::chrono::seconds(60));
    runWithTimeout("F 快速 watch(A)→watch(B)", scenarioRapidWatchSwitch, std::chrono::seconds(60));

    std::printf("vault_watcher: checks=%d failures=%d\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
