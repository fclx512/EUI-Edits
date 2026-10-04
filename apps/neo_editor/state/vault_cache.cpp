#include "state/vault_cache.h"

#include "core/platform/async.h"
#include "model/text_file.h"
#include "state/tabs_trace.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo::vaultcache {
namespace {

constexpr std::size_t kDefaultRootBudgetBytes = 32u * 1024u * 1024u;
constexpr std::size_t kDefaultRootCountLimit = 3;  // 活动根 + 2 个非活动根（计划 §B3 初值）

struct Entry {
    std::string root;                                       // 原始根字符串（用于重新扫描/展示）
    std::shared_ptr<const vault::ScanResult> scan;
    std::uint64_t generation = 0;
    std::uint64_t requestEpoch = 0;
    // The request epoch identifies the newest result allowed to publish. Keep
    // the physical async epoch separately: a sync refresh may supersede its
    // result while that worker is still occupying the per-root scan slot.
    std::uint64_t asyncEpoch = 0;
    std::uint64_t syncEpoch = 0;
    bool hasSnapshot = false;
    bool scanning = false;
    bool syncInFlight = false;
    bool rescanPending = false;
    std::size_t bytes = 0;
    std::uint64_t lastUse = 0;
};

struct Store {
    std::unordered_map<std::string, Entry> entries;
    std::size_t budgetBytes = kDefaultRootBudgetBytes;
    std::size_t rootLimit = kDefaultRootCountLimit;
    std::uint64_t useClock = 0;
    std::uint64_t epochClock = 0;
    std::uint64_t scansCompleted = 0;
    std::uint64_t scanRequests = 0;
    std::uint64_t scansDropped = 0;
    Scanner scanner;
    std::mutex mutex;
    std::string lastRootKeyInput;  // rootKey 记忆（避免每帧 weakly_canonical）
    std::string lastRootKey;
};

Store& store() {
    static Store value;
    return value;
}

std::string foldCase(std::string value) {
#if defined(_WIN32)
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return value;
}

vault::ScanResult runScan(const std::string& root) {
    Store& s = store();
    if (s.scanner) {
        return s.scanner(root);
    }
    return vault::scan(root);
}

std::size_t entryBytes(const Entry& entry) {
    return entry.bytes + entry.root.size() + sizeof(Entry) + 64;
}

std::size_t totalBytesLocked(const Store& s) {
    std::size_t total = 0;
    for (const auto& pair : s.entries) {
        total += entryBytes(pair.second);
    }
    return total;
}

// 淘汰最久未用的非活动根，直到回到预算内。绝不动活动根。
void pruneLocked(Store& s, const std::string& activeKey) {
    while (true) {
        std::size_t snapshotRoots = 0;
        std::size_t total = totalBytesLocked(s);
        for (const auto& pair : s.entries) {
            if (pair.second.hasSnapshot) ++snapshotRoots;
        }
        if (snapshotRoots <= s.rootLimit && total <= s.budgetBytes) {
            return;
        }
        const Entry* victim = nullptr;
        std::string victimKey;
        for (const auto& pair : s.entries) {
            if (pair.first == activeKey || !pair.second.hasSnapshot || pair.second.scanning) {
                continue;
            }
            if (victim == nullptr || pair.second.lastUse < victim->lastUse) {
                victim = &pair.second;
                victimKey = pair.first;
            }
        }
        if (victim == nullptr) {
            return;  // 只剩活动根（或有扫描在飞）——不再淘汰。
        }
        s.entries.erase(victimKey);
    }
}

void publishLocked(Store& s, const std::string& key, std::shared_ptr<const vault::ScanResult> scan) {
    auto found = s.entries.find(key);
    if (found == s.entries.end()) {
        s.scansDropped++;  // 根已被丢弃/换根：旧结果不得污染新根。
        return;
    }
    Entry& entry = found->second;
    entry.scan = std::move(scan);
    entry.bytes = entry.scan ? estimateBytes(*entry.scan) : 0;
    ++entry.generation;
    entry.hasSnapshot = true;
    ++s.scansCompleted;
}

void scheduleLocked(Store& s, const std::string& key, const std::string& root) {
    Entry& entry = s.entries[key];
    entry.root = root;
    entry.scanning = true;
    entry.rescanPending = false;
    entry.lastUse = ++s.useClock;
    entry.requestEpoch = ++s.epochClock;
    entry.asyncEpoch = entry.requestEpoch;
    ++s.scanRequests;
    const std::uint64_t requestEpoch = entry.requestEpoch;
    // restart：同一 root key 完成后再重扫时状态是 Done，runOnce 会被 beginTask 拒绝；
    // 协调查询已保证同一时刻只有一个 in-flight，restart 只用于复用 key 的后续重扫。
    core::async::restart("neo.vault.scan." + key,
                         [root]() { return runScan(root); },
                         [key, requestEpoch](core::async::Result<vault::ScanResult> result) {
                             Store& inner = store();
                             std::lock_guard<std::mutex> lock(inner.mutex);
                             auto found = inner.entries.find(key);
                             if (found == inner.entries.end()) {
                                 // The root was invalidated while this worker ran.
                                 inner.scansDropped++;
                                 return;
                             }
                             Entry& entry = found->second;
                             if (entry.asyncEpoch != requestEpoch) {
                                 // 已被更新的请求取代：丢弃这次结果，保留最新请求。
                                 inner.scansDropped++;
                                 return;
                             }
                             entry.asyncEpoch = 0;
                             if (entry.requestEpoch == requestEpoch) {
                                 publishLocked(inner, key,
                                               std::make_shared<const vault::ScanResult>(std::move(result.value)));
                             } else {
                                 // A newer synchronous publication owns the snapshot.
                                 inner.scansDropped++;
                             }
                             entry.scanning = entry.syncInFlight || entry.asyncEpoch != 0;
                             if (!entry.scanning && entry.rescanPending) {
                                 entry.rescanPending = false;
                                 scheduleLocked(inner, key, entry.root);
                             }
                         });
}

} // namespace

std::string rootKey(const std::string& root) {
    if (root.empty()) {
        return {};
    }
    // 记忆上一次 (root → key)：query/requestScan 每帧都可能被调用，weakly_canonical
    // 是文件系统调用，不能每帧重做。同一个 root 字符串直接命中记忆。
    {
        Store& s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (root == s.lastRootKeyInput) {
            return s.lastRootKey;
        }
    }
    std::error_code error;
    auto path = std::filesystem::weakly_canonical(textfile::pathFromUtf8(root), error);
    if (error) {
        path = std::filesystem::absolute(textfile::pathFromUtf8(root), error).lexically_normal();
    }
    std::string text = textfile::pathToUtf8(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    text = foldCase(std::move(text));
    {
        Store& s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.lastRootKeyInput = root;
        s.lastRootKey = text;
    }
    return text;
}

std::size_t estimateBytes(const vault::ScanResult& result) {
    std::size_t total = sizeof(vault::ScanResult) + result.error.size() + result.warning.size();
    // 递归遍历条目树；每个条目至少一条字符串。
    std::vector<const std::vector<vault::Entry>*> stack;
    stack.push_back(&result.roots);
    while (!stack.empty()) {
        const std::vector<vault::Entry>* level = stack.back();
        stack.pop_back();
        for (const vault::Entry& entry : *level) {
            total += sizeof(vault::Entry) + entry.name.size() + entry.relative.size();
            if (!entry.children.empty()) {
                stack.push_back(&entry.children);
            }
        }
    }
    return total;
}

ScanView query(const std::string& root) {
    ScanView view;
    if (root.empty()) {
        return view;
    }
    const std::string key = rootKey(root);
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto found = s.entries.find(key);
    if (found == s.entries.end()) {
        return view;
    }
    Entry& entry = found->second;
    entry.lastUse = ++s.useClock;
    view.scan = entry.scan;
    view.generation = entry.generation;
    view.scanning = entry.scanning;
    view.rescanPending = entry.rescanPending;
    return view;
}

RequestResult requestScan(const std::string& root) {
    RequestResult result;
    if (root.empty()) {
        return result;
    }
    const std::string key = rootKey(root);
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto found = s.entries.find(key);
    if (found != s.entries.end() && found->second.scanning) {
        found->second.rescanPending = true;  // 一个 in-flight + 一个"需要重扫"标记。
        return result;
    }
    scheduleLocked(s, key, root);
    result.scheduled = true;
    result.epoch = s.entries[key].requestEpoch;
    return result;
}

std::uint64_t requestScanSync(const std::string& root) {
    if (root.empty()) {
        return 0;
    }
    const std::string key = rootKey(root);
    Store& s = store();

    // Claim a newer epoch before scanning outside the store lock. This makes
    // already-running async results stale without allowing another request to
    // start a second async scan for this root. Requests arriving during the
    // synchronous scan coalesce into the existing pending-rescan bit.
    std::uint64_t requestEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        Entry& entry = s.entries[key];
        entry.root = root;
        entry.requestEpoch = requestEpoch = ++s.epochClock;
        entry.syncEpoch = requestEpoch;
        entry.syncInFlight = true;
        entry.scanning = true;
        entry.rescanPending = false;  // This scan covers requests already pending.
    }

    vault::ScanResult scan = runScan(root);
    std::lock_guard<std::mutex> lock(s.mutex);
    auto found = s.entries.find(key);
    if (found == s.entries.end()) {
        ++s.scansDropped;
        return 0;
    }
    Entry& entry = found->second;
    if (entry.syncEpoch != requestEpoch || entry.requestEpoch != requestEpoch) {
        ++s.scansDropped;
    } else {
        publishLocked(s, key, std::make_shared<const vault::ScanResult>(std::move(scan)));
    }
    if (entry.syncEpoch == requestEpoch) {
        entry.syncEpoch = 0;
        entry.syncInFlight = false;
    }
    entry.scanning = entry.syncInFlight || entry.asyncEpoch != 0;
    if (!entry.scanning && entry.rescanPending) {
        entry.rescanPending = false;
        scheduleLocked(s, key, entry.root);
    }
    return entry.generation;
}

void invalidate(const std::string& root) {
    if (root.empty()) {
        return;
    }
    const std::string key = rootKey(root);
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.entries.erase(key);
}

void clear() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.entries.clear();
}

void prune(const std::string& activeRoot) {
    const std::string key = rootKey(activeRoot);  // rootKey 自己取锁：必须在持锁前算。
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    pruneLocked(s, key);
}

std::size_t rootBudgetBytes() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.budgetBytes;
}

void setRootBudgetBytes(std::size_t bytes) {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.budgetBytes = bytes;
}

std::size_t rootCountLimit() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.rootLimit;
}

void setRootCountLimit(std::size_t count) {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.rootLimit = count == 0 ? 1 : count;
}

Stats stats() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    Stats out;
    out.cachedRoots = s.entries.size();
    out.estimatedBytes = totalBytesLocked(s);
    out.scansCompleted = s.scansCompleted;
    out.scanRequests = s.scanRequests;
    out.scansDropped = s.scansDropped;
    return out;
}

void setScannerForTest(Scanner scanner) {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.scanner = std::move(scanner);
}

void resetScannerForTest() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.scanner = nullptr;
}

void resetForTest() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.entries.clear();
    s.scanner = nullptr;
    s.scansCompleted = 0;
    s.scanRequests = 0;
    s.scansDropped = 0;
    s.useClock = 0;
    s.epochClock = 0;
    s.budgetBytes = kDefaultRootBudgetBytes;
    s.rootLimit = kDefaultRootCountLimit;
    s.lastRootKeyInput.clear();
    s.lastRootKey.clear();
}

} // namespace neo::vaultcache
