// Headless microbenchmark for InputModel::displayState's IME text construction.
// This deliberately does not call InputLayout::build, decorate, render or use an IME.
// Each stdout line is one raw JSONL sample; harnesses should retain stdout and
// bind it to source/EXE/config identity before comparing runs.
#include "components/input.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <new>
#include <string>

namespace allocation_probe {
std::atomic<bool> enabled{false};
std::atomic<unsigned long long> requests{0};
std::atomic<unsigned long long> bytes{0};

void record(std::size_t size) noexcept {
    if (enabled.load(std::memory_order_relaxed)) {
        requests.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(static_cast<unsigned long long>(size), std::memory_order_relaxed);
    }
}

void begin() noexcept {
    requests.store(0, std::memory_order_relaxed);
    bytes.store(0, std::memory_order_relaxed);
    enabled.store(true, std::memory_order_relaxed);
}

void end() noexcept { enabled.store(false, std::memory_order_relaxed); }
} // namespace allocation_probe

// Counts process-wide ordinary new/new[] requests only during displayState.
// This is requested size, not resident/private memory or an allocator attribution.
void* operator new(std::size_t size) {
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        allocation_probe::record(size);
        return memory;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        allocation_probe::record(size);
        return memory;
    }
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using Clock = std::chrono::steady_clock;
using Model = components::input_detail::InputModel;
using State = Model::InputState;

constexpr std::size_t kMiB = 1024u * 1024u;
constexpr std::size_t kLineBytes = 128;

enum class Mode { Unchanged, Changed, Steady };

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::Unchanged: return "composition_unchanged";
        case Mode::Changed: return "composition_changed_each_call";
        case Mode::Steady: return "noncomposing_steady";
    }
    return "invalid";
}

std::string makePlainText(std::size_t targetBytes) {
    // Exactly 1/9 MiB, 128-byte ASCII rows. The 9 MiB case has 73,728 rows;
    // there is no costly first layout because this probe never invokes layout.
    std::string result;
    result.reserve(targetBytes);
    std::string line(kLineBytes - 1, 'x');
    line.push_back('\n');
    for (std::size_t i = 0; i < targetBytes / kLineBytes; ++i) result += line;
    return result;
}

bool parseCount(const char* value, unsigned int& output) {
    try {
        const std::string raw(value);
        if (raw.empty() || raw.find_first_not_of("0123456789") != std::string::npos) return false;
        const auto parsed = std::stoull(raw);
        if (parsed == 0 || parsed > 1000) return false;
        output = static_cast<unsigned int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool runCase(std::size_t documentBytes, Mode mode, unsigned int warmup, unsigned int rounds) {
    State state;
    state.text = makePlainText(documentBytes);
    state.cursor = static_cast<int>(documentBytes / 2);
    state.selectionStart = state.selectionEnd = state.cursor;
    state.compositionText = "\xE4\xB8\xAD"; // 中, three UTF-8 bytes
    state.followCaret = false;
    const bool composing = mode != Mode::Steady;
    bool chooseOther = true;

    // Prime the owned preedit once outside the measured and warmup rounds.
    Model::displayState(state, composing);
    const auto invoke = [&]() -> State& {
        if (mode == Mode::Changed) {
            state.compositionText = chooseOther ? "\xE6\x96\x87" : "\xE4\xB8\xAD"; // 文 / 中
            chooseOther = !chooseOther;
        }
        return Model::displayState(state, composing);
    };
    for (unsigned int i = 0; i < warmup; ++i) invoke();
    unsigned long long previousRevision = composing ? state.preedit->textRevision : state.textRevision;

    for (unsigned int round = 0; round < rounds; ++round) {
        // For changed mode, the composition assignment is outside the timed
        // region; the measured operation is displayState alone.
        if (mode == Mode::Changed) {
            state.compositionText = chooseOther ? "\xE6\x96\x87" : "\xE4\xB8\xAD";
            chooseOther = !chooseOther;
        }
        allocation_probe::begin();
        const auto start = Clock::now();
        State& display = Model::displayState(state, composing);
        const auto stop = Clock::now();
        allocation_probe::end();
        const auto requests = allocation_probe::requests.load(std::memory_order_relaxed);
        const auto bytes = allocation_probe::bytes.load(std::memory_order_relaxed);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();

        if ((composing && (&display != state.preedit.get() ||
                           display.text.size() != state.text.size() + state.compositionText.size() ||
                           display.text.compare(static_cast<std::size_t>(state.cursor),
                                                state.compositionText.size(), state.compositionText) != 0 ||
                           display.textRevision != previousRevision + (mode == Mode::Changed ? 1u : 0u))) ||
            (!composing && (&display != &state || state.preedit))) {
            std::cerr << "displayState invariant failed\n";
            return false;
        }
        previousRevision = display.textRevision;
        std::cout << "{\"schema\":\"input-preedit-display-state/1\",\"scope\":\"display_state_only\""
                  << ",\"layout_included\":false,\"gui_included\":false"
                  << ",\"document_bytes\":" << state.text.size()
                  << ",\"document_lines\":" << (state.text.size() / kLineBytes)
                  << ",\"mode\":\"" << modeName(mode) << "\""
                  << ",\"prime_calls\":1,\"warmup_calls\":" << warmup
                  << ",\"round\":" << round
                  << ",\"display_state_wall_ns\":" << elapsed
                  << ",\"build_wall_ns\":null"
                  << ",\"ordinary_new_requests\":" << requests
                  << ",\"ordinary_new_requested_bytes\":" << bytes << "}\n";
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    unsigned int warmup = 5;
    unsigned int rounds = 20;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc || !parseCount(argv[i + 1],
                std::string(argv[i]) == "--warmup" ? warmup : rounds) ||
            (std::string(argv[i]) != "--warmup" && std::string(argv[i]) != "--rounds")) {
            std::cerr << "usage: input_preedit_benchmark [--warmup 1..1000] [--rounds 1..1000]\n";
            return 2;
        }
    }
    for (const std::size_t bytes : {kMiB, 9 * kMiB}) {
        for (const Mode mode : {Mode::Unchanged, Mode::Changed, Mode::Steady}) {
            if (!runCase(bytes, mode, warmup, rounds)) return 1;
        }
    }
    return 0;
}
