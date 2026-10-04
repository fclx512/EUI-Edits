// 阶段 D 单元测试：后台文件核验（未变不解码、同尺寸同时间戳发现变化、缺失、失败、
// restart 取代旧请求）。
#include "core/platform/async.h"
#include "state/file_check.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

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

void writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
}

struct GatedVerifier {
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    bool released = false;

    neo::filecheck::Result operator()(const neo::filecheck::Request& request) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++entered;
            cv.notify_all();
            cv.wait(lock, [this] { return released; });
        }
        neo::filecheck::Result result;
        result.requestId = request.requestId;
        result.status = neo::filecheck::Status::Unchanged;
        return result;
    }
    bool waitEntered(int count, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, count] { return entered >= count; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }
};

} // namespace

int main() {
    AsyncShutdownGuard shutdownGuard;
    std::error_code error;
    const fs::path dir = fs::temp_directory_path(error) /
        ("neo-file-check-" + std::to_string(Clock::now().time_since_epoch().count()));
    if (error) return 2;
    const fs::path file = dir / "sample.md";

    // ── 未变：指纹一致则跳过解码，返回 Unchanged ──
    neo::filecheck::resetForTest();
    fs::create_directories(dir, error);
    writeFile(file, "original content\n");
    const auto baseline = neo::filesafety::inspect(neo::textfile::pathToUtf8(file));
    check(baseline.status == neo::filesafety::DiskStatus::Present, "baseline fingerprint captured");
    {
        neo::filecheck::Result observed;
        bool done = false;
        neo::filecheck::Request request;
        request.path = neo::textfile::pathToUtf8(file);
        request.baseline = baseline;
        neo::filecheck::request("check.unchanged", std::move(request), [&](neo::filecheck::Result result) {
            observed = std::move(result);
            done = true;
        });
        check(pumpUntil([&] { return done; }), "unchanged check completes");
        check(observed.status == neo::filecheck::Status::Unchanged, "unchanged file skips decode");
    }

    // ── 同尺寸同时间戳的外部修改：内容哈希发现变化 ──
    {
        writeFile(file, "ORIGINAL CONTENT\n");  // 与 "original content\n" 等长
        fs::last_write_time(file, baseline.modified, error);  // 时间戳恢复成基线值
        const auto sameMeta = neo::filesafety::inspect(neo::textfile::pathToUtf8(file));
        check(sameMeta.status == neo::filesafety::DiskStatus::Present &&
                  sameMeta.size == baseline.size && sameMeta.modified == baseline.modified,
              "tampered file keeps size and timestamp");
        neo::filecheck::Result observed;
        bool done = false;
        neo::filecheck::Request request;
        request.path = neo::textfile::pathToUtf8(file);
        request.baseline = baseline;
        neo::filecheck::request("check.changed", std::move(request), [&](neo::filecheck::Result result) {
            observed = std::move(result);
            done = true;
        });
        check(pumpUntil([&] { return done; }), "changed check completes");
        check(observed.status == neo::filecheck::Status::Changed && observed.loaded.ok &&
                  observed.loaded.document.text == "ORIGINAL CONTENT\n",
              "same-size/same-mtime external change is discovered by content hash");
    }

    // ── 缺失 ──
    {
        fs::remove(file, error);
        neo::filecheck::Result observed;
        bool done = false;
        neo::filecheck::Request request;
        request.path = neo::textfile::pathToUtf8(file);
        request.baseline = baseline;
        neo::filecheck::request("check.missing", std::move(request), [&](neo::filecheck::Result result) {
            observed = std::move(result);
            done = true;
        });
        check(pumpUntil([&] { return done; }), "missing check completes");
        check(observed.status == neo::filecheck::Status::Missing, "missing file reported as Missing");
    }

    // ── restart 取代：旧请求的回调不得触发 ──
    neo::filecheck::resetForTest();
    {
        GatedVerifier gated;
        neo::filecheck::setVerifierForTest([&gated](const neo::filecheck::Request& request) { return gated(request); });
        std::atomic<int> callbacks{0};
        neo::filecheck::Request first;
        first.requestId = 1;
        neo::filecheck::Request second;
        second.requestId = 2;
        neo::filecheck::request("check.restart", first, [&](neo::filecheck::Result) { callbacks.fetch_add(1); });
        check(gated.waitEntered(1), "first verification entered");
        neo::filecheck::request("check.restart", second, [&](neo::filecheck::Result result) {
            if (result.requestId == 2) callbacks.fetch_add(2);  // 只有最新请求能带 2
        });
        check(gated.waitEntered(2), "second verification entered");
        gated.release();
        check(pumpUntil([&] { return callbacks.load() != 0; }), "superseding request completes");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        core::async::dispatchReady();
        check(callbacks.load() == 2, "superseded request callback never fires");
    }

    neo::filecheck::resetForTest();
    fs::remove_all(dir, error);
    std::cout << (failures == 0 ? "PASS" : "FAIL") << ": file_check_scheduler (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
