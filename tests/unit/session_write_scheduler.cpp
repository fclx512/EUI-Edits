// 阶段 E 单元测试：串行后台写协调器（顺序、取代、正文复用、失败、forgetTab）。
// 注入 writer，不写真实磁盘正文（仅用真实 sessionstorage 做一次端到端）。
//
// N-FAULT 增补：真实 sessionstorage + atomicwrite 故障注入点覆盖写 body/写 manifest
// 失败、checkpointBlocking 退出屏障超时/失败、forgetTab 复用；用 inline 模式让失败
// 路径确定性执行，避免与 worker 线程竞争全局故障注入开关。
#include "core/platform/async.h"
#include "model/atomic_write.h"
#include "model/session_storage.h"
#include "model/settings.h"
#include "state/session_writer.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

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

struct AsyncShutdownGuard {
    ~AsyncShutdownGuard() { core::async::shutdown(); }
};

// 可控 writer：记录被写入的活动 ID，可阻塞第一次调用以制造 in-flight 窗口。
struct GatedWriter {
    std::mutex mutex;
    std::condition_variable cv;
    bool hold = false;
    bool entered = false;
    bool released = false;
    bool result = true;
    std::vector<uint64_t> written;

    bool operator()(const std::vector<neo::sessionstorage::WriteRecord>&, neo::sessionstorage::TabId active) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool shouldHold = hold && !released;
        written.push_back(active);
        if (shouldHold) {
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return released; });
        }
        return result;
    }
    void blockNext() {
        std::lock_guard<std::mutex> lock(mutex);
        hold = true; entered = false; released = false;
    }
    bool waitEntered(int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true; hold = false;
        cv.notify_all();
    }
};

neo::sessionwriter::Submission submissionFor(uint64_t id, const neo::textfile::Document* document, bool dirty) {
    neo::sessionwriter::Submission submission;
    submission.activeId = id;
    neo::sessionwriter::Record record;
    record.id = id;
    record.path = "doc.md";
    record.language = "markdown";
    record.dirty = dirty;
    record.revision = document ? 1 : 0;
    record.document = document;
    submission.records.push_back(std::move(record));
    return submission;
}

// 捕获 writer：记录 active ID 与每个记录正文，可阻塞一次以制造 in-flight 窗口。
struct CaptureWriter {
    std::mutex mutex;
    std::condition_variable cv;
    bool hold = false;
    bool released = false;
    bool entered = false;
    bool result = true;
    std::vector<neo::sessionstorage::TabId> active;
    std::vector<std::pair<neo::sessionstorage::TabId, std::string>> bodies;

    bool operator()(const std::vector<neo::sessionstorage::WriteRecord>& records,
                    neo::sessionstorage::TabId activeId) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool shouldHold = hold && !released;
        active.push_back(activeId);
        for (const neo::sessionstorage::WriteRecord& record : records) {
            bodies.emplace_back(record.id, record.document ? record.document->text : std::string{});
        }
        if (shouldHold) {
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return released; });
        }
        return result;
    }
    void blockNext() {
        std::lock_guard<std::mutex> lock(mutex);
        hold = true; entered = false; released = false;
    }
    bool waitEntered(int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true; hold = false;
        cv.notify_all();
    }
    std::string lastBodyFor(neo::sessionstorage::TabId id) {
        std::lock_guard<std::mutex> lock(mutex);
        std::string out;
        for (const auto& entry : bodies) {
            if (entry.first == id) out = entry.second;
        }
        return out;
    }
    std::vector<neo::sessionstorage::TabId> activeSnapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return active;
    }
};

std::string readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool redirectConfig(const fs::path& root) {
    const std::string utf8 = neo::textfile::pathToUtf8(root);
#if defined(_WIN32)
    return _putenv_s("APPDATA", utf8.c_str()) == 0;
#else
    return setenv("XDG_CONFIG_HOME", utf8.c_str(), 1) == 0;
#endif
}

std::size_t fileCount(const fs::path& directory) {
    std::size_t count = 0;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) ++count;
    return count;
}

} // namespace

int main() {
    AsyncShutdownGuard shutdownGuard;
    neo::textfile::Document alpha;
    alpha.text = "alpha body";
    neo::textfile::Document beta;
    beta.text = "beta body";
    neo::textfile::Document gamma;
    gamma.text = "gamma body";

    // ── 串行顺序 + 待提交请求被最新请求取代 ──
    neo::sessionwriter::resetForTest();
    GatedWriter writer;
    neo::sessionwriter::setWriterForTest([&writer](const std::vector<neo::sessionstorage::WriteRecord>& r,
                                                   neo::sessionstorage::TabId a) { return writer(r, a); });
    writer.blockNext();
    const uint64_t first = neo::sessionwriter::submit(submissionFor(1, &alpha, true));
    check(first != 0, "first submit is accepted");
    if (!writer.waitEntered()) { std::cerr << "FAIL: writer never entered\n"; ++failures; }
    const uint64_t second = neo::sessionwriter::submit(submissionFor(2, &alpha, true));
    const uint64_t third = neo::sessionwriter::submit(submissionFor(3, &beta, true));
    check(second != 0 && third != 0 && second < third, "later submits get increasing sequence numbers");
    writer.release();
    check(pumpUntil([&] { return neo::sessionwriter::committedSeq() == third; }),
          "latest pending transaction commits after the in-flight one");
    {
        std::lock_guard<std::mutex> lock(writer.mutex);
        check(writer.written.size() == 2 && writer.written[0] == 1 && writer.written[1] == 3,
              "writes stay serial and the superseded middle request never reaches disk");
    }
    check(neo::sessionwriter::stats().supersededCount >= 1, "superseded request is accounted");

    // ── 正文复用：同 (TabId, revision) 不重复深拷贝 ──
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setWriterForTest([](const std::vector<neo::sessionstorage::WriteRecord>&,
                                            neo::sessionstorage::TabId) { return true; });
    neo::sessionwriter::submit(submissionFor(7, &alpha, true));
    check(pumpUntil([] { return neo::sessionwriter::committedSeq() >= 1; }), "first body copy commits");
    const auto afterFirst = neo::sessionwriter::stats();
    neo::sessionwriter::submit(submissionFor(7, &alpha, true));
    check(pumpUntil([] { return neo::sessionwriter::committedSeq() >= 2; }), "second submit commits");
    const auto afterSecond = neo::sessionwriter::stats();
    check(afterFirst.bodyCopies == 1 && afterSecond.bodyCopies == 1 && afterSecond.bodyReuses >= 1,
          "unchanged (TabId, revision) reuses the owned body instead of copying");
    check(afterSecond.retainedBodyBytes >= alpha.text.capacity(),
          "dirty recovery body is accounted in the dedupe cache");
    neo::sessionwriter::Submission cleanAfterSave;
    cleanAfterSave.activeId = 7;
    neo::sessionwriter::Record cleanRecord;
    cleanRecord.id = 7;
    cleanRecord.path = "doc.md";
    cleanAfterSave.records.push_back(std::move(cleanRecord));
    const auto cleanSeq = neo::sessionwriter::submit(std::move(cleanAfterSave));
    check(pumpUntil([&] { return neo::sessionwriter::committedSeq() == cleanSeq; }),
          "clean save snapshot commits");
    check(neo::sessionwriter::stats().retainedBodyBytes == 0,
          "dirty-to-clean transition releases the cached document copy");
    const auto copiesBeforeRedirty = neo::sessionwriter::stats().bodyCopies;
    neo::sessionwriter::submit(submissionFor(7, &alpha, true));
    check(pumpUntil([&] { return neo::sessionwriter::committedSeq() > cleanSeq; }),
          "dirty document can be submitted again after a clean save");
    const auto afterRedirty = neo::sessionwriter::stats();
    check(afterRedirty.bodyCopies == copiesBeforeRedirty + 1 && afterRedirty.retainedBodyBytes > 0,
          "redirty after clean save creates a new cached recovery copy");

    // A clean latest snapshot supersedes a dirty pending snapshot while an old
    // write still owns its independent document copy.
    neo::sessionwriter::resetForTest();
    {
        CaptureWriter capture;
        neo::sessionwriter::setWriterForTest([&capture](const std::vector<neo::sessionstorage::WriteRecord>& records,
                                                        neo::sessionstorage::TabId id) { return capture(records, id); });
        capture.blockNext();
        neo::textfile::Document inFlightDoc;
        inFlightDoc.text = "in-flight revision one";
        neo::textfile::Document pendingDoc;
        pendingDoc.text = "pending revision two";
        neo::textfile::Document redirtyDoc;
        redirtyDoc.text = "redirty revision three";
        neo::sessionwriter::submit(submissionFor(17, &inFlightDoc, true));
        if (!capture.waitEntered()) { std::cerr << "FAIL: clean-overwrite writer never entered\n"; ++failures; }
        auto dirtyPending = submissionFor(17, &pendingDoc, true);
        dirtyPending.records.front().revision = 2;
        const auto pendingDirtySeq = neo::sessionwriter::submit(std::move(dirtyPending));
        check(pendingDirtySeq != 0 && neo::sessionwriter::stats().retainedBodyBytes >= pendingDoc.text.capacity(),
              "new dirty pending revision is retained while the old snapshot is in flight");
        neo::sessionwriter::Submission cleanPending;
        cleanPending.activeId = 17;
        neo::sessionwriter::Record cleanRecord;
        cleanRecord.id = 17;
        cleanRecord.path = "doc.md";
        cleanPending.records.push_back(std::move(cleanRecord));
        const auto pendingCleanSeq = neo::sessionwriter::submit(std::move(cleanPending));
        check(pendingCleanSeq > pendingDirtySeq, "clean snapshot supersedes the dirty pending snapshot");
        check(neo::sessionwriter::stats().retainedBodyBytes == 0,
              "dirty-to-clean pending replacement releases the recovery dedupe copy immediately");
        capture.release();
        check(pumpUntil([&] { return neo::sessionwriter::committedSeq() == pendingCleanSeq; }),
              "clean latest snapshot commits after the prior in-flight write");
        const auto activeAfterClean = capture.activeSnapshot();
        check(activeAfterClean.size() == 2 && activeAfterClean[0] == 17 && activeAfterClean[1] == 17,
              "only the in-flight and latest clean snapshots reach the writer");
        check(capture.lastBodyFor(17).empty(), "clean snapshot carries no recovery body");
        const auto copiesBeforeNewRevision = neo::sessionwriter::stats().bodyCopies;
        auto redirty = submissionFor(17, &redirtyDoc, true);
        redirty.records.front().revision = 3;
        neo::sessionwriter::submit(std::move(redirty));
        check(pumpUntil([&] { return neo::sessionwriter::committedSeq() > pendingCleanSeq; }),
              "a newer dirty revision can be saved after clean overwrite");
        check(capture.lastBodyFor(17) == "redirty revision three" &&
                  neo::sessionwriter::stats().bodyCopies == copiesBeforeNewRevision + 1,
              "new revision is copied and written instead of reusing pre-clean content");
    }

    // ── 失败保留：committedSeq 不推进，回调报告失败 ──
    neo::sessionwriter::resetForTest();
    int callbackOkCount = 0, callbackFailCount = 0;
    neo::sessionwriter::setCommitCallback([&](bool ok, std::uint64_t) { ok ? ++callbackOkCount : ++callbackFailCount; });
    neo::sessionwriter::setWriterForTest([](const std::vector<neo::sessionstorage::WriteRecord>&,
                                            neo::sessionstorage::TabId) { return false; });
    const uint64_t failed = neo::sessionwriter::submit(submissionFor(9, &beta, true));
    check(pumpUntil([&] { return callbackFailCount >= 1; }), "failed transaction reports through the callback");
    check(neo::sessionwriter::committedSeq() == 0 && failed != 0, "failed write does not advance committedSeq");
    check(neo::sessionwriter::stats().failedCount >= 1, "failure is accounted");

    // ── forgetTab：Id 复用后不复用旧正文快照 ──
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setWriterForTest([](const std::vector<neo::sessionstorage::WriteRecord>&,
                                            neo::sessionstorage::TabId) { return true; });
    neo::sessionwriter::submit(submissionFor(11, &alpha, true));
    pumpUntil([] { return neo::sessionwriter::committedSeq() >= 1; });
    const auto beforeForget = neo::sessionwriter::stats().bodyCopies;
    neo::sessionwriter::forgetTab(11);
    neo::sessionwriter::submit(submissionFor(11, &alpha, true));
    pumpUntil([] { return neo::sessionwriter::committedSeq() >= 2; });
    check(neo::sessionwriter::stats().bodyCopies == beforeForget + 1,
          "forgetTab drops the cached body so a reused id copies fresh content");

    // ── 真实 sessionstorage 故障注入：body / manifest 原子写失败 ──
    // inline 模式让 runWrite 在调用线程同步执行，配合 atomicwrite::testing 的全局
    // failBeforeReplace，可确定性地在某次原子写阶段失败。断言三点：committedSeq 不推进、
    // 回调报失败、不留下"半个提交"（旧 manifest/body 逐字节保留，无残留临时/半个 body）。
    neo::sessionwriter::resetForTest();
    {
        std::error_code error;
        const fs::path root = fs::temp_directory_path(error) / "neo_session_write_scheduler_fault";
        if (error) { std::cerr << "FAIL: fault temp path\n"; ++failures; }
        fs::remove_all(root, error);
        error.clear();
        fs::create_directories(root, error);
        if (error) { std::cerr << "FAIL: create fault root\n"; ++failures; }
        check(redirectConfig(root), "redirect config directory for fault injection");
        const fs::path directory = neo::textfile::pathFromUtf8(neo::settings::configDirectory()) / "session";
        const fs::path manifest = directory / "manifest.json";

        neo::sessionwriter::setInlineForTest(true);
        int okCb = 0, failCb = 0;
        neo::sessionwriter::setCommitCallback([&](bool ok, std::uint64_t) { ok ? ++okCb : ++failCb; });

        neo::textfile::Document committed;
        committed.text = "committed body";
        const uint64_t baseSeq = neo::sessionwriter::submit(submissionFor(101, &committed, true));
        check(baseSeq != 0 && neo::sessionwriter::committedSeq() == baseSeq && okCb == 1,
              "baseline inline commit advances committedSeq and reports success");
        const std::string goodManifest = readBytes(manifest);
        check(!goodManifest.empty() && fileCount(directory) == 2,
              "baseline leaves exactly the manifest and one content-addressed body");

        // body 写失败：新内容需要新 body，原子写 body 阶段失败。
        neo::textfile::Document fresh;
        fresh.text = "fresh body";
        neo::atomicwrite::testing::failBeforeReplace(true);
        const uint64_t bodyFailSeq = neo::sessionwriter::submit(submissionFor(102, &fresh, true));
        neo::atomicwrite::testing::failBeforeReplace(false);
        check(bodyFailSeq > baseSeq && neo::sessionwriter::committedSeq() == baseSeq,
              "body write failure does not advance committedSeq");
        check(failCb == 1 && okCb == 1, "body write failure reports through the commit callback");
        check(!neo::sessionwriter::pending(), "body write failure clears in-flight state");
        check(readBytes(manifest) == goodManifest,
              "body write failure keeps the previous manifest byte-identical");
        check(fileCount(directory) == 2,
              "body write failure leaves no partial body and no atomic-write temp file");
        {
            std::vector<neo::sessionstorage::ReadRecord> restored;
            neo::sessionstorage::TabId active = 0;
            check(neo::sessionstorage::read(restored, active) && restored.size() == 1 &&
                      restored[0].id == 101 && restored[0].document.text == "committed body",
                  "session still restores the last committed snapshot after a body failure");
        }

        // manifest 写失败：内容未变的 body 已存在（复用），只尝试原子提交 manifest。
        neo::atomicwrite::testing::failBeforeReplace(true);
        const uint64_t manifestFailSeq = neo::sessionwriter::submit(submissionFor(101, &committed, true));
        neo::atomicwrite::testing::failBeforeReplace(false);
        check(manifestFailSeq > bodyFailSeq && neo::sessionwriter::committedSeq() == baseSeq,
              "manifest write failure does not advance committedSeq");
        check(neo::sessionwriter::stats().bodyReuses >= 1,
              "manifest-failure submit reused the existing body, so the failing atomic write is the manifest");
        check(failCb == 2, "manifest write failure reports through the commit callback");
        check(readBytes(manifest) == goodManifest, "manifest write failure keeps the previous manifest");
        check(fileCount(directory) == 2, "manifest write failure leaves no stray file");
        {
            std::vector<neo::sessionstorage::ReadRecord> restored;
            neo::sessionstorage::TabId active = 0;
            check(neo::sessionstorage::read(restored, active) && restored.size() == 1 && restored[0].id == 101,
                  "session still restores the previous snapshot after a manifest failure");
        }

        // 失败后仍可恢复：下一次提交成功并推进 committedSeq。
        const uint64_t recoverSeq = neo::sessionwriter::submit(submissionFor(103, &fresh, true));
        check(recoverSeq > manifestFailSeq && neo::sessionwriter::committedSeq() == recoverSeq && okCb == 2,
              "writer accepts and commits after injected failures");
        {
            std::vector<neo::sessionstorage::ReadRecord> restored;
            neo::sessionstorage::TabId active = 0;
            check(neo::sessionstorage::read(restored, active) && restored.size() == 1 &&
                      restored[0].id == 103 && restored[0].document.text == "fresh body",
                  "post-failure commit is durable and restores the new snapshot");
        }

        neo::sessionwriter::resetForTest();
        fs::remove_all(root, error);
    }

    // ── checkpointBlocking：内联确认 / 超时不取消 / 写失败不确认 ──
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(true);
    neo::sessionwriter::setWriterForTest([](const std::vector<neo::sessionstorage::WriteRecord>&,
                                            neo::sessionstorage::TabId) { return true; });
    check(neo::sessionwriter::checkpointBlocking(submissionFor(21, &alpha, true), 500),
          "checkpointBlocking confirms an inline commit");
    check(neo::sessionwriter::committedSeq() == neo::sessionwriter::lastSeq() &&
              neo::sessionwriter::committedSeq() != 0,
          "barrier commit advanced committedSeq to its own request sequence");

    // 退出屏障超时：worker 阻塞时到点返回 false，但事务未被取消，释放后仍会提交。
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(false);
    {
        CaptureWriter gate;
        neo::sessionwriter::setWriterForTest([&gate](const std::vector<neo::sessionstorage::WriteRecord>& records,
                                                     neo::sessionstorage::TabId id) { return gate(records, id); });
        gate.blockNext();
        const uint64_t before = neo::sessionwriter::committedSeq();
        const bool confirmed = neo::sessionwriter::checkpointBlocking(submissionFor(31, &beta, true), 120);
        check(!confirmed, "checkpointBlocking times out while the in-flight write is blocked");
        check(neo::sessionwriter::committedSeq() == before, "a timed-out barrier has not committed");
        gate.release();
        check(pumpUntil([&] { return neo::sessionwriter::committedSeq() > before; }),
              "a timed-out barrier does not cancel the transaction; it still commits");
        check(neo::sessionwriter::committedSeq() == neo::sessionwriter::lastSeq(),
              "the eventual commit reaches the barrier request sequence");
    }

    // 写入持续失败：屏障等到超时也不确认，committedSeq 不推进、无残留 in-flight。
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(false);
    neo::sessionwriter::setWriterForTest([](const std::vector<neo::sessionstorage::WriteRecord>&,
                                            neo::sessionstorage::TabId) { return false; });
    {
        const bool confirmed = neo::sessionwriter::checkpointBlocking(submissionFor(41, &beta, true), 100);
        check(!confirmed, "checkpointBlocking reports an unconfirmed barrier when the write fails");
        check(neo::sessionwriter::committedSeq() == 0,
              "failed barrier transaction never advances committedSeq");
        check(!neo::sessionwriter::pending(), "failed barrier transaction leaves no in-flight work");
        check(neo::sessionwriter::stats().failedCount >= 1, "failed barrier transaction is accounted");
    }

    // ── forgetTab：revision 复用契约 + 打破陈旧复用 ──
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(false);
    {
        CaptureWriter capture;
        neo::sessionwriter::setWriterForTest([&capture](const std::vector<neo::sessionstorage::WriteRecord>& records,
                                                        neo::sessionstorage::TabId id) { return capture(records, id); });
        neo::sessionwriter::submit(submissionFor(51, &alpha, true));
        check(pumpUntil([] { return neo::sessionwriter::committedSeq() >= 1; }), "reuse baseline commits");
        neo::sessionwriter::submit(submissionFor(51, &gamma, true));  // 同 id、同 revision，内容不同
        check(pumpUntil([] { return neo::sessionwriter::committedSeq() >= 2; }), "same-revision resubmit commits");
        check(capture.lastBodyFor(51) == "alpha body",
              "same (TabId,revision) reuses the owned body even when the caller passes new text (revision contract)");
        const auto beforeForgetCopy = neo::sessionwriter::stats().bodyCopies;
        neo::sessionwriter::forgetTab(51);
        neo::sessionwriter::submit(submissionFor(51, &gamma, true));
        check(pumpUntil([] { return neo::sessionwriter::committedSeq() >= 3; }), "post-forget submit commits");
        check(capture.lastBodyFor(51) == "gamma body",
              "forgetTab forces a fresh copy so a reused id writes the new content");
        check(neo::sessionwriter::stats().bodyCopies == beforeForgetCopy + 1,
              "forgetTab drops the cached body and re-copies exactly once");
    }

    // forgetTab 不影响在飞的拥有快照；在飞完成后按新快照提交新内容。
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(false);
    {
        CaptureWriter capture;
        neo::sessionwriter::setWriterForTest([&capture](const std::vector<neo::sessionstorage::WriteRecord>& records,
                                                        neo::sessionstorage::TabId id) { return capture(records, id); });
        capture.blockNext();
        neo::sessionwriter::submit(submissionFor(61, &alpha, true));  // in-flight owns "alpha body"
        if (!capture.waitEntered()) { std::cerr << "FAIL: in-flight writer never entered\n"; ++failures; }
        neo::sessionwriter::forgetTab(61);
        neo::sessionwriter::submit(submissionFor(61, &gamma, true));  // pending, 同 id，新内容
        capture.release();
        check(pumpUntil([&] { return neo::sessionwriter::committedSeq() >= 2; }),
              "in-flight transaction finishes and the pending one commits after forgetTab");
        {
            std::lock_guard<std::mutex> lock(capture.mutex);
            check(capture.bodies.size() >= 2 && capture.bodies.front().second == "alpha body" &&
                      capture.bodies.back().second == "gamma body",
                  "forgetTab does not corrupt the in-flight snapshot; the reused id commits fresh content");
        }
    }

    // ── 顺序：单 in-flight 串行，旧 writer 永不在新提交之后落盘 ──
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(false);
    {
        CaptureWriter capture;
        neo::sessionwriter::setWriterForTest([&capture](const std::vector<neo::sessionstorage::WriteRecord>& records,
                                                        neo::sessionstorage::TabId id) { return capture(records, id); });
        capture.blockNext();
        const uint64_t oldSeq = neo::sessionwriter::submit(submissionFor(71, &alpha, true));
        if (!capture.waitEntered()) { std::cerr << "FAIL: ordering writer never entered\n"; ++failures; }
        const uint64_t midSeq = neo::sessionwriter::submit(submissionFor(72, &beta, true));    // superseded
        const uint64_t newSeq = neo::sessionwriter::submit(submissionFor(73, &gamma, true));   // newest
        check(oldSeq < midSeq && midSeq < newSeq, "ordering submits get increasing sequence numbers");
        capture.release();

        std::vector<uint64_t> observed;
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (neo::sessionwriter::committedSeq() < newSeq && Clock::now() < deadline) {
            const uint64_t now = neo::sessionwriter::committedSeq();
            if (observed.empty() || observed.back() != now) observed.push_back(now);
            core::async::dispatchReady();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        observed.push_back(neo::sessionwriter::committedSeq());
        check(neo::sessionwriter::committedSeq() == newSeq, "the newest request commits last");

        bool monotonic = true;
        uint64_t previous = 0;
        for (const uint64_t value : observed) {
            if (value < previous) monotonic = false;
            previous = value;
        }
        check(monotonic, "observed committedSeq never decreases: an old writer never lands after a newer save");
        const std::vector<neo::sessionstorage::TabId> active = capture.activeSnapshot();
        check(active.size() == 2 && active[0] == 71 && active[1] == 73,
              "the superseded middle request never reaches the writer; write order stays serial and increasing");
        check(neo::sessionwriter::committedSeq() == neo::sessionwriter::lastSeq(),
              "lastSeq matches the newest committed request");
    }

    // Normal close must stop submissions, drop the queued snapshot, wait for the
    // sole in-flight write, then clear storage so that write cannot resurrect it.
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(false);
    {
        std::error_code error;
        const fs::path root = fs::temp_directory_path(error) / "neo_session_writer_close_clear";
        if (error) { std::cerr << "FAIL: close-clear temp path\n"; ++failures; }
        fs::remove_all(root, error);
        error.clear();
        fs::create_directories(root, error);
        if (error) { std::cerr << "FAIL: create close-clear root\n"; ++failures; }
        check(redirectConfig(root), "redirect config directory for close-clear test");
        const fs::path directory = neo::textfile::pathFromUtf8(neo::settings::configDirectory()) / "session";

        std::mutex gateMutex;
        std::condition_variable gateCv;
        bool writerEntered = false;
        bool releaseWriter = false;
        int realWrites = 0;
        neo::sessionwriter::setWriterForTest([&](const std::vector<neo::sessionstorage::WriteRecord>& records,
                                                 neo::sessionstorage::TabId activeId) {
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                ++realWrites;
                if (realWrites == 1) {
                    writerEntered = true;
                    gateCv.notify_all();
                    gateCv.wait(lock, [&] { return releaseWriter; });
                }
            }
            return neo::sessionstorage::write(records, activeId);
        });
        neo::textfile::Document older;
        older.text = "write already in flight";
        neo::textfile::Document queued;
        queued.text = "must be discarded on close";
        const auto oldSeq = neo::sessionwriter::submit(submissionFor(801, &older, true));
        bool entered = false;
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            entered = gateCv.wait_for(lock, std::chrono::seconds(3), [&] { return writerEntered; });
        }
        check(oldSeq != 0 && entered, "close-clear test reaches a gated in-flight storage write");
        const auto queuedSeq = neo::sessionwriter::submit(submissionFor(802, &queued, true));
        check(queuedSeq > oldSeq, "a newer snapshot is pending behind the gated write");
        bool finalized = false;
        bool finalizerOk = false;
        check(neo::sessionwriter::beginDiscardOnClose([&](bool ok) {
            finalizerOk = ok;
            finalized = true;
        }), "close finalizer starts without blocking on in-flight write");
        check(neo::sessionwriter::submit(submissionFor(803, &alpha, true)) == 0,
              "new snapshots are rejected as soon as close finalization starts");
        check(!finalized, "close finalizer waits asynchronously for the old write");
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            releaseWriter = true;
            gateCv.notify_all();
        }
        check(pumpUntil([&] { return finalized; }), "close finalizer completes after the in-flight write");
        check(finalizerOk, "close finalizer reports successful storage clear");
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            check(realWrites == 1, "pending snapshot is dropped instead of written after the old write");
        }
        check(!fs::exists(directory / "manifest.json"), "completed close removes the persisted manifest");
        check(fileCount(directory) == 0, "completed close removes owned recovery bodies");
        std::vector<neo::sessionstorage::ReadRecord> restored;
        neo::sessionstorage::TabId activeId = 999;
        check(neo::sessionstorage::read(restored, activeId) && restored.empty() && activeId == 0,
              "the old in-flight snapshot cannot reappear after close finalization");
        check(neo::sessionwriter::submit(submissionFor(804, &alpha, true)) == 0,
              "writer remains stopped after successful finalization");
        neo::sessionwriter::resetForTest();
        fs::remove_all(root, error);
    }

    // A stopped async scheduler can reject dispatch during teardown. The writer
    // must still settle the request as failed instead of leaving inFlight stuck.
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setWriterForTest([](const std::vector<neo::sessionstorage::WriteRecord>&,
                                            neo::sessionstorage::TabId) { return true; });
    core::async::shutdown();
    const auto rejectedDispatchSeq = neo::sessionwriter::submit(submissionFor(901, &alpha, true));
    check(rejectedDispatchSeq != 0, "teardown dispatch attempt receives a request sequence");
    check(!neo::sessionwriter::pending(), "rejected async dispatch clears in-flight state");
    check(neo::sessionwriter::stats().failedCount == 1,
          "rejected async dispatch is accounted as a failed write");
    bool unavailableClearCompleted = false;
    bool unavailableClearOk = true;
    check(neo::sessionwriter::beginDiscardOnClose([&](bool ok) {
        unavailableClearOk = ok;
        unavailableClearCompleted = true;
    }), "close finalizer reports startup even when the async runtime is unavailable");
    check(unavailableClearCompleted && !unavailableClearOk,
          "unavailable async clear reports failure without waiting");
    const auto retryAfterClearFailure = neo::sessionwriter::submit(submissionFor(902, &alpha, true));
    check(retryAfterClearFailure != 0,
          "clear failure restores submission acceptance so the caller can re-persist recovery");
    check(!neo::sessionwriter::pending(), "a second rejected dispatch also leaves no stuck in-flight state");

    neo::sessionwriter::resetForTest();
    std::cout << (failures == 0 ? "PASS" : "FAIL") << ": session_write_scheduler (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
