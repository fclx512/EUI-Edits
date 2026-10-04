// 阶段 B 单元测试：按根共享的扫描缓存 + 后台扫描协调器。
// 注入扫描实现，不依赖真实文件系统；用 core::async::dispatchReady pump 完成回调。
#include "core/platform/async.h"
#include "state/vault_cache.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

using Clock = std::chrono::steady_clock;

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate()) {
        core::async::dispatchReady();
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

neo::vault::ScanResult makeScan(int files) {
    neo::vault::ScanResult result;
    result.ok = true;
    result.fileCount = files;
    neo::vault::Entry entry;
    entry.name = "a.md";
    entry.relative = "a.md";
    result.roots.push_back(entry);
    return result;
}

struct AsyncShutdownGuard {
    ~AsyncShutdownGuard() { core::async::shutdown(); }
};

// 可控扫描器：统计调用次数，并可在测试要求时阻塞到放行（制造"完成时根已不匹配"）。
struct GatedScanner {
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::condition_variable cv;
    int files = 3;
    std::set<int> blockedCalls;
    std::set<int> enteredCalls;
    std::set<int> releasedCalls;
    std::vector<int> fileResults;

    neo::vault::ScanResult operator()(const std::string&) {
        const int call = calls.fetch_add(1) + 1;
        std::unique_lock<std::mutex> lock(mutex);
        if (blockedCalls.count(call) != 0) {
            enteredCalls.insert(call);
            cv.notify_all();
            cv.wait(lock, [this, call] { return releasedCalls.count(call) != 0; });
        }
        const int resultFiles = call > 0 && static_cast<std::size_t>(call) <= fileResults.size()
            ? fileResults[static_cast<std::size_t>(call - 1)] : files;
        return makeScan(resultFiles);
    }
    void blockNext() {
        std::lock_guard<std::mutex> lock(mutex);
        blockedCalls.insert(calls.load() + 1);
    }
    void blockCall(int call) {
        std::lock_guard<std::mutex> lock(mutex);
        blockedCalls.insert(call);
    }
    bool waitEntered(int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !enteredCalls.empty(); });
    }
    bool waitEnteredCall(int call, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, call] {
            return enteredCalls.count(call) != 0;
        });
    }
    bool hasEnteredCall(int call) {
        std::lock_guard<std::mutex> lock(mutex);
        return enteredCalls.count(call) != 0;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        releasedCalls.insert(blockedCalls.begin(), blockedCalls.end());
        cv.notify_all();
    }
    void releaseCall(int call) {
        std::lock_guard<std::mutex> lock(mutex);
        releasedCalls.insert(call);
        cv.notify_all();
    }
};

} // namespace

int main() {
    AsyncShutdownGuard shutdownGuard;

    // ── 同根请求去重：一个 in-flight + 一个"需要重扫"标记，不是每次调用都排任务 ──
    neo::vaultcache::resetForTest();
    neo::vaultcache::clear();
    {
        GatedScanner scanner;
        neo::vaultcache::setScannerForTest([&scanner](const std::string& root) { return scanner(root); });
        scanner.blockNext();
        const std::string root = "/tmp/neo-scan-a";
        const auto first = neo::vaultcache::requestScan(root);
        check(first.scheduled, "first request schedules a background scan");
        if (!scanner.waitEntered()) { std::cerr << "FAIL: scanner never entered\n"; ++failures; }
        const auto second = neo::vaultcache::requestScan(root);
        check(!second.scheduled, "same-root request while in-flight only marks rescan pending");
        check(scanner.calls.load() == 1, "no duplicate task dispatched while one is in flight");
        const auto viewDuring = neo::vaultcache::query(root);
        check(viewDuring.scanning && !viewDuring.scan, "query reports in-flight with no snapshot yet");
        scanner.release();
        // 放行后：第一次结果发布，随后 rescanPending 触发第二次扫描。
        check(pumpUntil([&] { return scanner.calls.load() >= 2 && !neo::vaultcache::query(root).scanning; }),
              "pending rescan runs after the first completes");
        const auto published = neo::vaultcache::query(root);
        check(published.scan && published.scan->ok && published.generation >= 2,
              "snapshot published with monotonic generation");
        check(neo::vaultcache::query(root).scan == published.scan, "queries share one immutable snapshot");
    }

    // ── 完成时根已被丢弃：旧结果不得污染新根 ──
    neo::vaultcache::resetForTest();
    neo::vaultcache::clear();
    {
        GatedScanner scanner;
        neo::vaultcache::setScannerForTest([&scanner](const std::string& root) { return scanner(root); });
        scanner.blockNext();
        const std::string root = "/tmp/neo-scan-drop";
        neo::vaultcache::requestScan(root);
        if (!scanner.waitEntered()) { std::cerr << "FAIL: scanner never entered (drop)\n"; ++failures; }
        neo::vaultcache::invalidate(root);
        scanner.release();
        check(pumpUntil([] { return neo::vaultcache::stats().scansDropped >= 1; }),
              "stale scan completion after invalidate is dropped");
        check(!neo::vaultcache::query(root).scan, "dropped result does not repopulate the discarded root");
    }

    // ── 同步扫描发布 + 预算修剪（不动活动根）──
    neo::vaultcache::resetForTest();
    neo::vaultcache::clear();
    {
        int calls = 0;
        neo::vaultcache::setScannerForTest([&calls](const std::string&) { ++calls; return makeScan(2); });
        const std::string active = "/tmp/neo-scan-active";
        const std::string otherA = "/tmp/neo-scan-other-a";
        const std::string otherB = "/tmp/neo-scan-other-b";
        check(neo::vaultcache::requestScanSync(active) >= 1, "sync scan publishes immediately");
        neo::vaultcache::requestScanSync(otherA);
        neo::vaultcache::requestScanSync(otherB);
        check(neo::vaultcache::stats().cachedRoots == 3, "three roots cached before pruning");
        neo::vaultcache::setRootCountLimit(1);
        neo::vaultcache::prune(active);
        const auto after = neo::vaultcache::stats();
        check(after.cachedRoots == 1 && neo::vaultcache::query(active).scan != nullptr,
              "prune keeps the active root and evicts inactive ones");
        check(neo::vaultcache::query(otherA).scan == nullptr && neo::vaultcache::query(otherB).scan == nullptr,
              "evicted roots no longer serve snapshots");
    }

    // ── Sync publication supersedes a blocked async result without opening a
    // second async slot; pending requests run only after the stale worker drains.
    neo::vaultcache::resetForTest();
    neo::vaultcache::clear();
    {
        GatedScanner scanner;
        scanner.fileResults = {11, 22, 33};
        scanner.blockCall(1);  // old async scan
        scanner.blockCall(3);  // the pending rescan after old async drains
        neo::vaultcache::setScannerForTest([&scanner](const std::string& root) { return scanner(root); });
        const std::string root = "/tmp/neo-scan-sync-race";

        const auto oldRequest = neo::vaultcache::requestScan(root);
        check(oldRequest.scheduled, "async scan starts before sync refresh");
        check(scanner.waitEnteredCall(1), "old async scan is held inside scanner");

        const auto syncGeneration = neo::vaultcache::requestScanSync(root);
        auto current = neo::vaultcache::query(root);
        check(syncGeneration == 1 && current.generation == 1 && current.scan &&
                  current.scan->fileCount == 22,
              "sync scan publishes its newer snapshot and generation");
        check(current.scanning, "superseded async worker still occupies the same-root scan slot");

        const auto pending = neo::vaultcache::requestScan(root);
        current = neo::vaultcache::query(root);
        check(!pending.scheduled && current.rescanPending,
              "request during stale async drain coalesces into one pending rescan");
        check(scanner.calls.load() == 2, "pending request does not start a parallel async scan");

        scanner.releaseCall(1);
        check(pumpUntil([&] { return scanner.hasEnteredCall(3); }),
              "pending scan starts after the stale worker drains");
        current = neo::vaultcache::query(root);
        auto stats = neo::vaultcache::stats();
        check(current.scan && current.scan->fileCount == 22 && current.generation == 1 &&
                  stats.scansCompleted == 1 && stats.scansDropped == 1,
              "released stale async result neither replaces snapshot nor increments generation");

        scanner.releaseCall(3);
        check(pumpUntil([&] { return scanner.calls.load() == 3 && !neo::vaultcache::query(root).scanning; }),
              "coalesced future request completes after stale result is discarded");
        current = neo::vaultcache::query(root);
        check(current.scan && current.scan->fileCount == 33 && current.generation == 2,
              "later request publishes exactly one newer generation");
    }

    neo::vaultcache::resetForTest();
    neo::vaultcache::clear();
    std::cout << (failures == 0 ? "PASS" : "FAIL") << ": vault_scan_scheduler (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
