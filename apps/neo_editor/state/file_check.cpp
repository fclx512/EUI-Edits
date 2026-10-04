#include "state/file_check.h"

#include "core/platform/async.h"

#include <mutex>

namespace neo::filecheck {
namespace {

struct State {
    Verifier verifier;
    std::mutex mutex;
};

State& state() {
    static State value;
    return value;
}

Result verify(const Request& request) {
    Verifier injected;
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        injected = s.verifier;
    }
    if (injected) {
        return injected(request);
    }

    Result result;
    result.requestId = request.requestId;
    const filesafety::Fingerprint current = filesafety::inspect(request.path);
    result.observed = current;
    if (current.status == filesafety::DiskStatus::Missing) {
        result.status = Status::Missing;
        result.error = current.error;
        return result;
    }
    const bool baselineComparable = request.baseline.status == filesafety::DiskStatus::Present;
    if (baselineComparable && current.status == filesafety::DiskStatus::Present &&
        filesafety::same(request.baseline, current)) {
        result.status = Status::Unchanged;  // 指纹一致：不解码，不读第二三遍。
        return result;
    }
    filesafety::Loaded loaded = filesafety::load(request.path);
    result.observed = loaded.fingerprint.status == filesafety::DiskStatus::Unknown
                          ? current
                          : loaded.fingerprint;
    if (loaded.result.ok) {
        result.status = Status::Changed;
        result.loaded = std::move(loaded.result);
    } else {
        result.status = current.status == filesafety::DiskStatus::Missing ? Status::Missing : Status::Failed;
        result.error = loaded.result.error;
        result.loaded = std::move(loaded.result);
    }
    return result;
}

} // namespace

void request(const std::string& key, Request request, Completion completion) {
    if (key.empty()) {
        return;
    }
    core::async::detail::start(
        key, /*restart=*/true,
        [request]() { return verify(request); },
        [completion](core::async::Result<Result> result) { completion(std::move(result.value)); });
}

void cancel(const std::string& key) {
    if (!key.empty()) {
        core::async::cancel(key);
    }
}

void setVerifierForTest(Verifier verifier) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.verifier = std::move(verifier);
}

void resetForTest() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.verifier = nullptr;
}

} // namespace neo::filecheck
