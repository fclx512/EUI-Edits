#include "state/session_writer.h"

#include "core/platform/async.h"
#include "state/tabs_trace.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace neo::sessionwriter {
namespace {

// 一次写事务拥有的完整快照。worker 只读它；正文是 shared_ptr<const Document>，
// 与活动页的 doc 解耦，UI 之后的变化不会串进这次事务。
struct OwnedRecord {
    sessionstorage::TabId id = 0;
    std::string path;
    std::string vaultRoot;
    std::string language;
    int wrapOverride = -1;
    bool dirty = false;
    unsigned long long revision = 0;
    std::shared_ptr<const textfile::Document> document;
};

struct OwnedSnapshot {
    std::vector<OwnedRecord> records;
    sessionstorage::TabId activeId = 0;
    std::uint64_t seq = 0;
};

struct DocKey {
    unsigned long long revision = 0;
};

struct Store {
    std::mutex mutex;
    std::unordered_map<sessionstorage::TabId, std::shared_ptr<const textfile::Document>> lastDocs;
    std::unordered_map<sessionstorage::TabId, unsigned long long> lastDocRevision;
    bool inFlight = false;
    bool hasPending = false;
    OwnedSnapshot pending;
    bool accepting = true;
    bool discardFinalizing = false;
    DiscardCallback discardCallback;
    std::uint64_t seqClock = 0;
    std::uint64_t committedSeq = 0;
    std::uint64_t lastSeq = 0;
    Stats stats;
    CommitCallback commitCallback;
    Writer writer;
    bool inlineMode = false;
};

std::uint64_t documentBytes(const textfile::Document& document) {
    return static_cast<std::uint64_t>(sizeof(document)) + document.text.capacity();
}

void eraseCachedDocument(Store& s, sessionstorage::TabId id) {
    const auto found = s.lastDocs.find(id);
    if (found != s.lastDocs.end()) {
        const std::uint64_t bytes = found->second ? documentBytes(*found->second) : 0;
        s.stats.retainedBodyBytes = bytes <= s.stats.retainedBodyBytes
            ? s.stats.retainedBodyBytes - bytes : 0;
        s.lastDocs.erase(found);
    }
    s.lastDocRevision.erase(id);
}

Store& store() {
    static Store value;
    return value;
}

// 用拥有的快照跑一次真实写事务。返回是否提交成功。不碰共享统计（由 finish 在锁内记）。
bool runWrite(const OwnedSnapshot& snapshot, const Writer& writer) {
    std::vector<sessionstorage::WriteRecord> records;
    records.reserve(snapshot.records.size());
    for (const OwnedRecord& owned : snapshot.records) {
        sessionstorage::WriteRecord record;
        record.id = owned.id;
        record.path = owned.path;
        record.vaultRoot = owned.vaultRoot;
        record.language = owned.language;
        record.wrapOverride = owned.wrapOverride;
        record.dirty = owned.dirty;
        record.document = owned.document ? owned.document.get() : nullptr;
        records.push_back(std::move(record));
    }
    return writer ? writer(records, snapshot.activeId) : sessionstorage::write(records, snapshot.activeId);
}

// 取出待提交快照并标记 in-flight；需要在持锁时调用。
bool takePendingLocked(Store& s, OwnedSnapshot& out) {
    if (s.inFlight || !s.hasPending) {
        return false;
    }
    out = std::move(s.pending);
    s.pending = OwnedSnapshot{};
    s.hasPending = false;
    s.inFlight = true;
    return true;
}

void pump(Store& s);
void startDiscardClear(bool inlineMode);

void completeDiscard(bool ok) {
    Store& s = store();
    DiscardCallback callback;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.discardFinalizing = false;
        s.accepting = !ok;
        callback = std::move(s.discardCallback);
    }
    if (callback) callback(ok);
}

void startDiscardClear(bool inlineMode) {
    if (inlineMode) {
        completeDiscard(sessionstorage::clearForNormalExit());
        return;
    }
    const bool started = core::async::restart(
        "neo.session.clear",
        []() { return sessionstorage::clearForNormalExit(); },
        [](core::async::Result<bool> result) { completeDiscard(result.ok && result.value); });
    if (!started) completeDiscard(false);
}

// 事务结束结算（worker 回调或内联路径）。自己取锁，绝不在持锁时调用 ——
// commitCallback 可能重入 submit（inline 测试模式下尤其要避免自锁死）。
void finish(OwnedSnapshot snapshot, bool ok) {
    Store& s = store();
    CommitCallback callback;
    bool discard = false;
    bool inlineMode = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.inFlight = false;
        ++s.stats.writeCount;
        if (ok) {
            s.committedSeq = snapshot.seq;
            ++s.stats.committedCount;
        } else {
            ++s.stats.failedCount;
        }
        discard = s.discardFinalizing;
        inlineMode = s.inlineMode;
        callback = s.commitCallback;
    }
    if (callback) {
        callback(ok, snapshot.seq);
    }
    if (discard) startDiscardClear(inlineMode);
    else pump(s);
}

void dispatchAsync(OwnedSnapshot snapshot) {
    auto owned = std::make_shared<const OwnedSnapshot>(std::move(snapshot));
    // 用 restart（而非 runOnce）：同一个 key 完成后状态是 Done，runOnce 会被 beginTask
    // 拒绝，下一次提交就再也派发不出去。我们自己在 finish 里保证同一时刻只有一个 in-flight。
    const bool started = core::async::restart(
        "neo.session.write",
        [owned]() {
            Store& inner = store();
            Writer writer;
            {
                std::lock_guard<std::mutex> lock(inner.mutex);
                writer = inner.writer;
            }
            return runWrite(*owned, writer);
        },
        [owned](core::async::Result<bool> result) { finish(*owned, result.ok && result.value); });
    if (!started) finish(*owned, false);
}

// 派发待提交事务（无锁入口）。同一时刻只有一个 in-flight，保证提交顺序。
void pump(Store& s) {
    OwnedSnapshot snapshot;
    bool inlineMode = false;
    Writer writer;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!takePendingLocked(s, snapshot)) {
            return;
        }
        inlineMode = s.inlineMode;
        writer = s.writer;
    }
    if (inlineMode) {
        const bool ok = runWrite(snapshot, writer);
        finish(std::move(snapshot), ok);
        return;
    }
    dispatchAsync(std::move(snapshot));
}

// 把提交转成拥有的快照。dirty 正文按 (TabId, revision) 复用，避免重复深拷贝。
OwnedSnapshot buildSnapshot(Store& s, Submission& submission) {
    OwnedSnapshot snapshot;
    snapshot.activeId = submission.activeId;
    snapshot.seq = ++s.seqClock;
    snapshot.records.reserve(submission.records.size());
    for (Record& record : submission.records) {
        OwnedRecord owned;
        owned.id = record.id;
        owned.path = std::move(record.path);
        owned.vaultRoot = std::move(record.vaultRoot);
        owned.language = std::move(record.language);
        owned.wrapOverride = record.wrapOverride;
        owned.dirty = record.dirty;
        owned.revision = record.revision;
        if (record.dirty && record.document != nullptr) {
            auto cached = s.lastDocs.find(record.id);
            auto cachedRevision = s.lastDocRevision.find(record.id);
            if (cached != s.lastDocs.end() && cachedRevision != s.lastDocRevision.end() &&
                cachedRevision->second == record.revision && cached->second) {
                owned.document = cached->second;  // 内容未变：复用拥有的不可变副本。
                ++s.stats.bodyReuses;
            } else {
                owned.document = std::make_shared<const textfile::Document>(*record.document);
                eraseCachedDocument(s, record.id);
                s.lastDocs[record.id] = owned.document;
                s.lastDocRevision[record.id] = record.revision;
                s.stats.retainedBodyBytes += documentBytes(*owned.document);
                ++s.stats.bodyCopies;
            }
        } else if (!record.dirty) {
            // A saved/clean tab no longer needs its recovery-body dedupe copy.
            // In-flight and pending snapshots retain their own shared ownership.
            eraseCachedDocument(s, record.id);
        }
        snapshot.records.push_back(std::move(owned));
    }
    return snapshot;
}

} // namespace

std::uint64_t submit(Submission submission) {
    Store& s = store();
    std::uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.accepting) return 0;
        ++s.stats.submitCount;
        if (s.hasPending) {
            ++s.stats.supersededCount;  // 尚未开始的旧请求被最新请求替换。
        }
        OwnedSnapshot snapshot = buildSnapshot(s, submission);
        s.lastSeq = snapshot.seq;
        seq = snapshot.seq;
        s.pending = std::move(snapshot);
        s.hasPending = true;
    }
    pump(s);  // 锁外派发：commitCallback 可能重入 submit，内联模式也不能自锁。
    return seq;
}

std::uint64_t committedSeq() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.committedSeq;
}

bool pending() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.inFlight || s.hasPending;
}

std::uint64_t lastSeq() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.lastSeq;
}

void forgetTab(sessionstorage::TabId id) {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    eraseCachedDocument(s, id);
}

bool checkpointBlocking(Submission submission, int timeoutMs) {
    const std::uint64_t seq = submit(std::move(submission));
    if (seq == 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (committedSeq() < seq) {
        core::async::dispatchReady();
        if (committedSeq() >= seq) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool beginDiscardOnClose(DiscardCallback callback) {
    Store& s = store();
    bool clearNow = false;
    bool inlineMode = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.accepting || s.discardFinalizing) return false;
        s.accepting = false;
        s.discardFinalizing = true;
        s.discardCallback = std::move(callback);
        s.hasPending = false;
        s.pending = OwnedSnapshot{};
        s.lastDocs.clear();
        s.lastDocRevision.clear();
        s.stats.retainedBodyBytes = 0;
        clearNow = !s.inFlight;
        inlineMode = s.inlineMode;
    }
    if (clearNow) startDiscardClear(inlineMode);
    return true;
}

void setCommitCallback(CommitCallback callback) {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.commitCallback = std::move(callback);
}

Stats stats() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.stats;
}

void setWriterForTest(Writer writer) {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.writer = std::move(writer);
}

void setInlineForTest(bool inlineMode) {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.inlineMode = inlineMode;
}

void resetForTest() {
    Store& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.lastDocs.clear();
    s.lastDocRevision.clear();
    s.stats.retainedBodyBytes = 0;
    s.inFlight = false;
    s.hasPending = false;
    s.pending = OwnedSnapshot{};
    s.accepting = true;
    s.discardFinalizing = false;
    s.discardCallback = nullptr;
    s.seqClock = 0;
    s.committedSeq = 0;
    s.lastSeq = 0;
    s.stats = Stats{};
    s.commitCallback = nullptr;
    s.writer = nullptr;
    s.inlineMode = false;
}

} // namespace neo::sessionwriter
