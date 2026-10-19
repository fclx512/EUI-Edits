#include "state/vault_cache.h"

#include "core/platform/async.h"
#include "model/text_file.h"
#include "state/tabs_trace.h"

#include <algorithm>
#include <atomic>
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
constexpr std::size_t kCursorLimit = 64;

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
    std::shared_ptr<std::atomic<bool>> scanCancel;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> pageTasks;
    std::set<std::string> expanded;
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
    std::string activeKey;
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

vault::ScanResult runScan(const std::string& root,
                          const std::shared_ptr<std::atomic<bool>>& cancel = {}) {
    tracelog::Span span("vault-root-scan");
    Store& s = store();
    if (s.scanner) {
        return s.scanner(root);
    }
    return vault::scanPage(root, {}, {},
                           [cancel] { return cancel && cancel->load(std::memory_order_relaxed); });
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
        if (victim->scan) vault::closeCursors(*victim->scan);
        s.entries.erase(victimKey);
    }
}

struct PreparedScan {
    std::shared_ptr<const vault::ScanResult> scan;
    std::size_t bytes = 0;
};

PreparedScan prepareScan(const std::string& root,
                         const std::shared_ptr<std::atomic<bool>>& cancel = {}) {
    PreparedScan prepared;
    prepared.scan = std::make_shared<const vault::ScanResult>(runScan(root, cancel));
    // Walking a large tree belongs to the scan worker, not the UI completion
    // callback or the shared store lock. Publish only the immutable snapshot
    // and its already computed estimate.
    prepared.bytes = estimateBytes(*prepared.scan);
    return prepared;
}

void publishLocked(Store& s, const std::string& key, PreparedScan prepared) {
    auto found = s.entries.find(key);
    if (found == s.entries.end()) {
        s.scansDropped++;  // 根已被丢弃/换根：旧结果不得污染新根。
        return;
    }
    Entry& entry = found->second;
    if (prepared.scan->cancelled) { ++s.scansDropped; return; }
    // A shallow root refresh keeps already loaded descendants only while the
    // corresponding direct directory still exists. The root's continuation is
    // reset to its first page, so it cannot point past a changed ordering.
    if (entry.scan && entry.scan->ok && prepared.scan->ok) {
        auto merged = std::make_shared<vault::ScanResult>(*prepared.scan);
        for (auto& next : merged->roots) {
            if (!next.isDir) continue;
            const auto old = std::find_if(entry.scan->roots.begin(), entry.scan->roots.end(),
                [&](const vault::Entry& value) { return value.isDir && value.relative == next.relative; });
            if (old != entry.scan->roots.end()) {
                next.children = old->children;
                next.childrenLoaded = old->childrenLoaded;
                next.hasMore = old->hasMore;
                next.cursor = old->cursor;
                next.cacheLimitReached = old->cacheLimitReached;
                next.pageError = old->pageError;
            }
        }
        for (const auto& old : entry.scan->roots) {
            if (!old.isDir) continue;
            const auto kept = std::find_if(merged->roots.begin(), merged->roots.end(),
                [&](const vault::Entry& value) { return value.isDir && value.relative == old.relative; });
            if (kept == merged->roots.end()) vault::closeCursors(old);
        }
        prepared.scan = std::move(merged);
        prepared.bytes = estimateBytes(*prepared.scan);
    }
    if (entry.scan && entry.scan->cursor && entry.scan->cursor != prepared.scan->cursor)
        vault::closeCursor(entry.scan->cursor);
    entry.scan = std::move(prepared.scan);
    entry.bytes = prepared.bytes;
    ++entry.generation;
    entry.hasSnapshot = true;
    ++s.scansCompleted;
}

void scheduleLocked(Store& s, const std::string& key, const std::string& root) {
    Entry& entry = s.entries[key];
    if (entry.scanCancel) entry.scanCancel->store(true, std::memory_order_relaxed);
    // Child-page readers continue while the root's first page is revalidated;
    // their cursor is retained only for directories still present afterward.
    entry.scanCancel = std::make_shared<std::atomic<bool>>(false);
    const auto cancel = entry.scanCancel;
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
                         [root, cancel]() { return prepareScan(root, cancel); },
                         [key, requestEpoch, cancel](core::async::Result<PreparedScan> result) {
                             Store& inner = store();
                             std::lock_guard<std::mutex> lock(inner.mutex);
                             auto found = inner.entries.find(key);
                             if (found == inner.entries.end()) {
                                 // The root was invalidated while this worker ran.
                                 inner.scansDropped++;
                                 return;
                             }
                             Entry& entry = found->second;
                             if (entry.asyncEpoch != requestEpoch ||
                                 entry.scanCancel != cancel || cancel->load(std::memory_order_relaxed)) {
                                 // 已被更新的请求取代：丢弃这次结果，保留最新请求。
                                 inner.scansDropped++;
                                 return;
                             }
                             entry.asyncEpoch = 0;
                             if (entry.requestEpoch == requestEpoch) {
                                 publishLocked(inner, key, std::move(result.value));
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

const vault::Entry* findDirectory(const std::vector<vault::Entry>& entries,
                                  const std::string& relative) {
    for (const auto& entry : entries) {
        if (!entry.isDir) continue;
        if (entry.relative == relative) return &entry;
        if (relative.rfind(entry.relative + "/", 0) == 0) {
            if (const auto* child = findDirectory(entry.children, relative)) return child;
        }
    }
    return nullptr;
}

vault::Entry* findDirectory(std::vector<vault::Entry>& entries,
                            const std::string& relative) {
    for (auto& entry : entries) {
        if (!entry.isDir) continue;
        if (entry.relative == relative) return &entry;
        if (relative.rfind(entry.relative + "/", 0) == 0) {
            if (auto* child = findDirectory(entry.children, relative)) return child;
        }
    }
    return nullptr;
}

bool releaseCollapsed(std::vector<vault::Entry>& entries, const std::set<std::string>& expanded) {
    bool changed = false;
    for (auto& entry : entries) {
        if (!entry.isDir) continue;
        if (expanded.count(entry.relative) == 0) {
            if (!entry.linkLeaf && (entry.childrenLoaded || !entry.children.empty() || entry.cursor ||
                                    entry.hasMore || entry.cacheLimitReached || !entry.pageError.empty())) {
                vault::closeCursors(entry);
                entry.children.clear();
                entry.cursor.reset();
                entry.hasMore = false;
                entry.childrenLoaded = false;
                entry.cacheLimitReached = false;
                entry.pageError.clear();
                changed = true;
            }
        } else {
            changed = releaseCollapsed(entry.children, expanded) || changed;
        }
    }
    return changed;
}

std::size_t countCursors(const vault::ScanResult& scan) {
    std::size_t count = scan.cursor ? 1 : 0;
    const auto visit = [&](const auto& self, const std::vector<vault::Entry>& entries) -> void {
        for (const auto& entry : entries) {
            if (entry.cursor) ++count;
            self(self, entry.children);
        }
    };
    visit(visit, scan.roots);
    return count;
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
    view.loadingPages = !entry.pageTasks.empty();
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

RequestResult requestPage(const std::string& root, const std::string& relative, bool refresh) {
    RequestResult result;
    if (root.empty()) return result;
    const std::string key = rootKey(root);
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto found = s.entries.find(key);
    if (found == s.entries.end() || !found->second.scan || !found->second.scan->ok) return result;
    Entry& cache = found->second;
    if (cache.scanning && relative.empty()) return result;
    const vault::Entry* directory = relative.empty() ? nullptr : findDirectory(cache.scan->roots, relative);
    if (!relative.empty() && (!directory || directory->cacheLimitReached && !refresh ||
                              directory->childrenLoaded && !directory->hasMore && !refresh)) return result;
    if (relative.empty() && (!cache.scan->hasMore || cache.scan->cacheLimitReached) && !refresh) return result;
    const auto cursor = refresh ? std::shared_ptr<vault::PageCursor>{}
        : relative.empty() ? cache.scan->cursor : directory->cursor;
    if (auto old = cache.pageTasks.find(relative); old != cache.pageTasks.end()) {
        if (!refresh) return result;
        old->second->store(true, std::memory_order_relaxed);
        cache.pageTasks.erase(old);
    }
    if (refresh) {
        if (directory) vault::closeCursors(*directory);
        else vault::closeCursor(cache.scan->cursor);
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cache.pageTasks.emplace(relative, cancel);
    result.scheduled = true;
    result.epoch = ++s.epochClock;
    ++s.scanRequests;
    core::async::restart("neo.vault.page." + key + "/" + relative,
        [root, relative, cursor, cancel] {
            return vault::scanPage(root, relative, cursor,
                [cancel] { return cancel->load(std::memory_order_relaxed); });
        },
        [key, relative, refresh, cursor, cancel](core::async::Result<vault::ScanResult> result) {
            Store& inner = store();
            std::lock_guard<std::mutex> guard(inner.mutex);
            auto found = inner.entries.find(key);
            if (found == inner.entries.end()) { ++inner.scansDropped; return; }
            Entry& current = found->second;
            auto task = current.pageTasks.find(relative);
            if (task == current.pageTasks.end() || task->second != cancel ||
                cancel->load(std::memory_order_relaxed) ||
                result.value.cancelled) {
                ++inner.scansDropped;
                return;
            }
            current.pageTasks.erase(task);
            if (!current.scan) { ++inner.scansDropped; return; }
            if (!result.value.ok) {
                auto failed = std::make_shared<vault::ScanResult>(*current.scan);
                auto* failedDirectory = relative.empty() ? nullptr : findDirectory(failed->roots, relative);
                const std::string message = result.value.error.empty() ? result.value.warning : result.value.error;
                if (failedDirectory) failedDirectory->pageError = message;
                else if (relative.empty()) failed->pageError = message;
                else { ++inner.scansDropped; return; }
                current.scan = std::move(failed);
                current.bytes = estimateBytes(*current.scan);
                ++current.generation;
                ++inner.scansCompleted;
                return;
            }
            auto next = std::make_shared<vault::ScanResult>(*current.scan);
            auto* destination = relative.empty() ? nullptr : findDirectory(next->roots, relative);
            if (!relative.empty() && !destination) { ++inner.scansDropped; return; }
            if (!refresh && (destination ? destination->cursor : next->cursor) != cursor) {
                ++inner.scansDropped; return;
            }
            if (!result.value.warning.empty()) next->warning = result.value.warning;
            auto& items = destination ? destination->children : next->roots;
            if (refresh) items.clear();
            items.insert(items.end(), std::make_move_iterator(result.value.roots.begin()),
                         std::make_move_iterator(result.value.roots.end()));
            if (destination) {
                destination->childrenLoaded = true;
                destination->hasMore = result.value.hasMore;
                destination->cursor = std::move(result.value.cursor);
                destination->cacheLimitReached = false;
                destination->pageError.clear();
            } else {
                next->hasMore = result.value.hasMore;
                next->cursor = std::move(result.value.cursor);
                next->cacheLimitReached = false;
                next->pageError.clear();
                next->fileCount = (refresh ? 0 : next->fileCount) + result.value.fileCount;
                next->directoryCount = (refresh ? 0 : next->directoryCount) + result.value.directoryCount;
            }
            releaseCollapsed(next->roots, current.expanded);
            const std::size_t bytes = estimateBytes(*next);
            if (bytes > inner.budgetBytes || countCursors(*next) > kCursorLimit) {
                // The enumerator advanced, so discard that continuation and
                // retain the prior visible list. A refresh can restart it.
                if (relative.empty()) vault::closeCursor(next->cursor);
                else if (const auto* candidate = findDirectory(next->roots, relative))
                    vault::closeCursor(candidate->cursor);
                next = std::make_shared<vault::ScanResult>(*current.scan);
                auto* limited = relative.empty() ? nullptr : findDirectory(next->roots, relative);
                if (limited) {
                    limited->cacheLimitReached = true;
                    vault::closeCursor(limited->cursor);
                    limited->cursor.reset();
                } else if (relative.empty()) {
                    next->cacheLimitReached = true;
                    vault::closeCursor(next->cursor);
                    next->cursor.reset();
                }
                current.bytes = estimateBytes(*next);
            } else {
                current.bytes = bytes;
            }
            current.scan = std::move(next);
            ++current.generation;
            ++inner.scansCompleted;
        });
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

    PreparedScan prepared = prepareScan(root);
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
        publishLocked(s, key, std::move(prepared));
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
    auto found = s.entries.find(key);
    if (found != s.entries.end()) {
        if (found->second.scan) vault::closeCursors(*found->second.scan);
        if (found->second.scanCancel) found->second.scanCancel->store(true, std::memory_order_relaxed);
        for (auto& task : found->second.pageTasks) task.second->store(true, std::memory_order_relaxed);
    }
    s.entries.erase(key);
}

void clear() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (auto& pair : s.entries) {
        if (pair.second.scan) vault::closeCursors(*pair.second.scan);
        if (pair.second.scanCancel) pair.second.scanCancel->store(true, std::memory_order_relaxed);
        for (auto& task : pair.second.pageTasks) task.second->store(true, std::memory_order_relaxed);
    }
    s.entries.clear();
    s.activeKey.clear();
}

void activateRoot(const std::string& root) {
    const std::string active = rootKey(root);
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (active == s.activeKey) return;
    s.activeKey = active;
    for (auto& pair : s.entries) {
        if (pair.first == active) continue;
        if (pair.second.scan) vault::closeCursors(*pair.second.scan);
        if (pair.second.scanCancel) pair.second.scanCancel->store(true, std::memory_order_relaxed);
        for (auto& task : pair.second.pageTasks) task.second->store(true, std::memory_order_relaxed);
    }
    // Inactive snapshots have enumeration handles that cannot be resumed after
    // cancellation. Drop them; the next visit starts from a fresh first batch.
    for (auto it = s.entries.begin(); it != s.entries.end();) {
        if (it->first != active) it = s.entries.erase(it);
        else ++it;
    }
}

void setExpanded(const std::string& root, const std::set<std::string>& expanded) {
    if (root.empty()) return;
    const std::string key = rootKey(root);
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto found = s.entries.find(key);
    if (found == s.entries.end() || found->second.expanded == expanded) return;
    Entry& entry = found->second;
    entry.expanded = expanded;
    for (auto task = entry.pageTasks.begin(); task != entry.pageTasks.end();) {
        if (!task->first.empty() && expanded.count(task->first) == 0) {
            task->second->store(true, std::memory_order_relaxed);
            task = entry.pageTasks.erase(task);
        } else ++task;
    }
    if (!entry.scan) return;
    auto next = std::make_shared<vault::ScanResult>(*entry.scan);
    if (!releaseCollapsed(next->roots, expanded)) return;
    entry.scan = std::move(next);
    entry.bytes = estimateBytes(*entry.scan);
    ++entry.generation;
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
    for (auto& pair : s.entries) {
        if (pair.second.scan) vault::closeCursors(*pair.second.scan);
        if (pair.second.scanCancel) pair.second.scanCancel->store(true, std::memory_order_relaxed);
        for (auto& task : pair.second.pageTasks) task.second->store(true, std::memory_order_relaxed);
    }
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
    s.activeKey.clear();
}

} // namespace neo::vaultcache
