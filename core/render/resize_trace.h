#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace core::render {

// Opt-in bounded in-memory events. One write at normal process shutdown; no
// synchronous heartbeat I/O or graphics queries in the measured frame path.
class ResizeTrace {
public:
    static bool enabled() {
        static const bool value = [] {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
            const char* option = std::getenv("NEO_RESIZE_TRACE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
            return option && option[0] == '1' && option[1] == '\0';
        }();
        return value;
    }
    static double now() {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void record(const char* kind, double ms, int width = 0, int height = 0) {
        if (!enabled()) return;
        if (events_.size() == 16384) { ++dropped_; return; }
        events_.push_back({kind, now(), ms, width, height});
    }
    ~ResizeTrace() {
        if (!enabled()) return;
        std::fputs("[resize-trace] {\"events\":[", stderr);
        const double start = events_.empty() ? 0 : events_[0].time;
        for (std::size_t i = 0; i < events_.size(); ++i) {
            const auto& e = events_[i];
            std::fprintf(stderr, "%s[\"%s\",%.3f,%.3f,%d,%d]", i ? "," : "",
                e.kind, e.time - start, e.ms, e.width, e.height);
        }
        std::fprintf(stderr, "],\"dropped\":%zu}\n", dropped_);
    }
private:
    struct Event { const char* kind = nullptr; double time = 0, ms = 0; int width = 0, height = 0; };
    std::vector<Event> events_;
    std::size_t dropped_ = 0;
};

inline ResizeTrace& resizeTrace() { static ResizeTrace trace; return trace; }
inline double resizeTraceStart() { return ResizeTrace::enabled() ? ResizeTrace::now() : 0; }
inline void resizeTraceEnd(const char* kind, double start, int w = 0, int h = 0) {
    if (ResizeTrace::enabled()) resizeTrace().record(kind, ResizeTrace::now() - start, w, h);
}

} // namespace core::render
