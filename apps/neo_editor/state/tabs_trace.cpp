#include "state/tabs_trace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo::tracelog {
namespace {

constexpr std::size_t kMaxQueuedRecords = 512;

struct TraceState {
    bool initialized = false;
    bool on = false;
    std::ofstream file;
    std::deque<std::string> queue;
    std::mutex mutex;
};

TraceState& state() {
    static TraceState value;
    return value;
}

// 切页请求打点：写与读必须用同一个对象（函数内 static 各是独立对象，会读不到）。
std::atomic<std::uint64_t>& switchRequestStamp() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

std::uint64_t threadId() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

void initLocked(TraceState& trace) {
    trace.initialized = true;
    const char* path = std::getenv("NEO_TABS_TRACE");
    if (path == nullptr || *path == '\0') {
        return;
    }
    trace.file.open(path, std::ios::out | std::ios::trunc);
    if (!trace.file) {
        return;
    }
    trace.on = true;
    trace.file << "qpc_us,thread,request,stage,detail\n";
}

void writeRecord(TraceState& trace, const std::string& record) {
    trace.queue.push_back(record);
    if (trace.queue.size() >= kMaxQueuedRecords) {
        for (const std::string& line : trace.queue) {
            trace.file << line << '\n';
        }
        trace.queue.clear();
        trace.file.flush();
    }
}

} // namespace

bool enabled() {
    TraceState& trace = state();
    std::lock_guard<std::mutex> lock(trace.mutex);
    if (!trace.initialized) {
        initLocked(trace);
    }
    return trace.on;
}

std::uint64_t nowMicros() {
#if defined(_WIN32)
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    if (frequency.QuadPart == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        (static_cast<long double>(counter.QuadPart) * 1000000.0L) /
        static_cast<long double>(frequency.QuadPart));
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

std::uint64_t nextRequestId() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint64_t nextFrameSequence() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

void markSwitchRequest() {
    switchRequestStamp().store(nowMicros(), std::memory_order_relaxed);
}

std::uint64_t switchRequestMicros() {
    return switchRequestStamp().load(std::memory_order_relaxed);
}

void clearSwitchRequest() {
    switchRequestStamp().store(0, std::memory_order_relaxed);
}

void event(const char* stage, std::uint64_t requestId, const std::string& detail) {
    TraceState& trace = state();
    if (!enabled()) {
        return;
    }
    std::ostringstream line;
    line << nowMicros() << ',' << threadId() << ',' << requestId << ',' << stage;
    if (!detail.empty()) {
        line << ',' << detail;
    }
    std::lock_guard<std::mutex> lock(trace.mutex);
    writeRecord(trace, line.str());
}

Span::Span(const char* stage, std::uint64_t requestId)
    : stage_(stage), requestId_(requestId), begin_(nowMicros()) {}

Span::~Span() {
    if (!enabled()) {
        return;
    }
    const std::uint64_t elapsed = nowMicros() >= begin_ ? nowMicros() - begin_ : 0;
    std::ostringstream detail;
    detail << "elapsed_us=" << elapsed;
    if (!detail_.empty()) {
        detail << ' ' << detail_;
    }
    event(stage_, requestId_, detail.str());
}

std::uint64_t Span::elapsedMicros() const {
    const std::uint64_t now = nowMicros();
    return now >= begin_ ? now - begin_ : 0;
}

void flush() {
    TraceState& trace = state();
    std::lock_guard<std::mutex> lock(trace.mutex);
    if (!trace.on) {
        return;
    }
    for (const std::string& line : trace.queue) {
        trace.file << line << '\n';
    }
    trace.queue.clear();
    trace.file.flush();
}

} // namespace neo::tracelog
