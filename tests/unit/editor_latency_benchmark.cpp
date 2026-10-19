// Headless, sequential InputModel -> LP -> InputLayout stage probe.
// This is wall time in the model/decorator/layout chain. It excludes
// InputBuilder, UI construction, rendering, native IME, and presentation.
#include "components/input.h"
#include "model/lp_decorations.h"
#include "model/style_schema.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
using Model = components::input_detail::InputModel;
using State = Model::InputState;
using EditInfo = components::input_detail::DecoratorEditInfo;
using Changes = components::input_detail::LineDecorationChanges;
using Snapshot = components::input_detail::LineDecorationSnapshot;
using View = components::input_detail::LineDecorationView;

constexpr std::size_t kMiB = 1024 * 1024;
constexpr float kControlWidth = 800.0f;
constexpr float kControlHeight = 500.0f;
constexpr float kViewportWidth = 768.0f;  // 8 DIP insets and 16 DIP scrollbar allowance
constexpr float kViewportHeight = 480.0f;
constexpr float kFontSize = 16.0f;
constexpr char kInsert[] = "中";  // exactly three UTF-8 bytes
constexpr char kOtherComposition[] = "文";

enum class Format { Plain, Markdown };
enum class Mode { Edit, Scroll, Preedit, Steady };

struct Options {
    std::size_t bytes = kMiB;
    Format format = Format::Markdown;
    Mode mode = Mode::Edit;
    int rounds = 5;
    int warmups = 2;
    bool wrap = true;
    bool preeditGrowth = false;
};

struct Fixture {
    std::string text;
    std::size_t rows = 0;  // newline-terminated source rows, including a short final row
    std::uint64_t fnv1a64 = 0;
    int middleLineStart = 0;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

double micros(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

std::uint64_t fnv1a64(const std::string& value) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

Fixture makeFixture(std::size_t bytes, Format format) {
    Fixture result;
    result.text.reserve(bytes);
    for (std::size_t row = 0; result.text.size() < bytes; ++row) {
        std::string line;
        if (format == Format::Plain) {
            line = "plain sample words for measured short rows 0123456789 abcdefghijklmnop 中🙂\n";
        } else if (row % 64 == 0) {
            line = "## 小节 " + std::to_string(row / 64) + "\n";
        } else if (row % 2 != 0) {
            line = "\n";  // separated paragraphs; no dense all-markup synthetic document
        } else {
            line = "段落 中🙂 words **bold** and `code` for a short Markdown row.\n";
        }
        const std::size_t remaining = bytes - result.text.size();
        if (line.size() <= remaining) {
            result.text += line;
        } else {
            // Preserve exact requested byte count without cutting a UTF-8 sequence.
            if (remaining > 1) result.text.append(remaining - 1, 'x');
            result.text += '\n';
        }
    }
    result.rows = static_cast<std::size_t>(std::count(result.text.begin(), result.text.end(), '\n'));
    result.fnv1a64 = fnv1a64(result.text);
    std::size_t pos = result.text.rfind('\n', bytes / 2);
    pos = pos == std::string::npos ? 0 : pos + 1;
    while (pos < result.text.size() && result.text[pos] == '\n') ++pos;
    if (pos >= result.text.size()) fail("fixture has no middle edit row");
    result.middleLineStart = static_cast<int>(pos);
    return result;
}

int parseCount(const std::string& raw, int maximum, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(raw, &consumed);
    if (consumed != raw.size() || value > static_cast<unsigned long long>(maximum))
        fail(std::string("invalid ") + name);
    return static_cast<int>(value);
}

Options parseArgs(int argc, char** argv) {
    Options options;
    if ((argc - 1) % 2 != 0) fail("options require values");
    for (int i = 1; i < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--bytes") {
            std::size_t consumed = 0;
            const auto n = std::stoull(value, &consumed);
            if (consumed != value.size() || n < 1024 || n > 16 * kMiB ||
                n > static_cast<unsigned long long>(std::numeric_limits<int>::max())) fail("invalid --bytes");
            options.bytes = static_cast<std::size_t>(n);
        } else if (key == "--format") {
            if (value == "plain") options.format = Format::Plain;
            else if (value == "markdown") options.format = Format::Markdown;
            else fail("invalid --format");
        } else if (key == "--mode") {
            if (value == "edit") options.mode = Mode::Edit;
            else if (value == "scroll") options.mode = Mode::Scroll;
            else if (value == "preedit") options.mode = Mode::Preedit;
            else if (value == "steady") options.mode = Mode::Steady;
            else fail("invalid --mode");
        } else if (key == "--rounds") {
            options.rounds = parseCount(value, 1000, "--rounds");
            if (options.rounds == 0) fail("--rounds must be positive");
        } else if (key == "--warmups") {
            options.warmups = parseCount(value, 1000, "--warmups");
        } else if (key == "--wrap") {
            if (value == "1") options.wrap = true;
            else if (value == "0") options.wrap = false;
            else fail("--wrap must be 0 or 1");
        } else if (key == "--preedit-growth") {
            if (value == "1") options.preeditGrowth = true;
            else if (value == "0") options.preeditGrowth = false;
            else fail("--preedit-growth must be 0 or 1");
        } else {
            fail("unknown option: " + key);
        }
    }
    return options;
}

const char* formatName(Format format) { return format == Format::Markdown ? "markdown" : "plain"; }
const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::Edit: return "edit";
        case Mode::Scroll: return "scroll";
        case Mode::Preedit: return "preedit";
        case Mode::Steady: return "steady";
    }
    return "unknown";
}

void printCounters(const neo::LpPlanDebugStats& planBefore,
                   const neo::lp::DecorationDebugStats& decorationBefore,
                   const Model::LayoutDebugStats& layoutBefore) {
    const auto& p = neo::planDebugStats();
    const auto& d = neo::lp::decorationDebugStats();
    const auto& l = Model::debugLayoutStats();
#define DELTA(group, field, before) \
    std::cout << ",\"" #group "_" #field "\":" << (group.field - before.field)
    DELTA(p, full, planBefore);
    DELTA(p, partial, planBefore);
    DELTA(p, fallback, planBefore);
    DELTA(p, verifyMismatch, planBefore);
    DELTA(d, full, decorationBefore);
    DELTA(d, incremental, decorationBefore);
    DELTA(d, fallback, decorationBefore);
    DELTA(d, cursorPartial, decorationBefore);
    DELTA(d, cursorRebuiltRows, decorationBefore);
    DELTA(d, editChangedRows, decorationBefore);
    DELTA(l, full, layoutBefore);
    DELTA(l, incremental, layoutBefore);
    DELTA(l, fallback, layoutBefore);
    DELTA(l, cursorPatched, layoutBefore);
    DELTA(l, cursorMeasuredRows, layoutBefore);
    DELTA(l, editPatched, layoutBefore);
    DELTA(l, editMeasuredRows, layoutBefore);
    DELTA(l, detailMeasuredRows, layoutBefore);
    DELTA(l, coarseRows, layoutBefore);
    DELTA(l, geometryRebuilt, layoutBefore);
#undef DELTA
}

void run(const Options& options) {
    const Fixture fixture = makeFixture(options.bytes, options.format);
    if (sizeof(kInsert) != 4 || sizeof(kOtherComposition) != 4)
        fail("expected three-byte UTF-8 compositions");
    neo::lp::invalidatePlanCache();
    neo::lp::invalidateDecorationCache();
    State state;
    Model::loadDocument(state, fixture.text);
    Model::moveCursorTo(state, fixture.middleLineStart, false);
    state.followCaret = false;
    state.wordWrap = options.wrap;
    state.viewportMetrics = true;
    if (options.mode == Mode::Preedit) state.compositionText = kInsert;
    const auto colors = neo::editorColors(neo::ThemeMode::Dark);
    const auto style = neo::markdownStyle(kFontSize, "monospace", "monospace", colors);
    bool inserted = false;
    bool otherComposition = false;
    bool highScroll = false;
    float scrollHigh = 0.0f;

    // Cold first: no warmed layout/LP cache. The first edit/scroll mutation is
    // deferred until the warm rounds so the cold row is a common baseline build.
    const auto iteration = [&](int round, const char* phase, bool cold) {
        const auto planBefore = neo::planDebugStats();
        const auto lpStageBefore = neo::lpPlanStageProfile();
        const auto decorationBefore = neo::lp::decorationDebugStats();
        const auto layoutBefore = Model::debugLayoutStats();
        State* display = nullptr;
        Snapshot snapshot;
        Changes changes;
        const auto t0 = Clock::now();
        if (!cold) {
            if (options.mode == Mode::Edit) {
                if (inserted) {
                    Model::eraseRange(state, fixture.middleLineStart,
                                      fixture.middleLineStart + 3);
                } else {
                    Model::moveCursorTo(state, fixture.middleLineStart, false);
                    Model::insertAtCursor(state, kInsert);
                }
                inserted = !inserted;
            } else if (options.mode == Mode::Scroll) {
                highScroll = !highScroll;
                state.verticalScroll = highScroll ? scrollHigh : 0.0f;
            } else if (options.mode == Mode::Preedit) {
                otherComposition = !otherComposition;
                state.compositionText = otherComposition ? kOtherComposition : kInsert;
                if (otherComposition && options.preeditGrowth) state.compositionText += "🙂";
                ++state.compositionRevision;
            }
        }
        const auto t1 = Clock::now();
        const bool composing = options.mode == Mode::Preedit;
        display = &Model::displayState(state, composing);
        const auto t2 = Clock::now();
        EditInfo info;
        info.textRevision = state.textRevision;
        info.committed = !composing;
        info.edit = info.committed && state.pendingEdit.valid &&
                    state.pendingEdit.revision == state.textRevision
                        ? &state.pendingEdit : nullptr;
        info.changes = &changes;
        const neo::LpPlan* plan = nullptr;
        if (options.format == Format::Markdown)
            plan = &neo::lp::cachedPlan(display->text, &info);
        const auto t3 = Clock::now();
        if (plan)
            snapshot = neo::lp::cachedDecorationSnapshot(
                *plan, neo::lp::planCache().version, state.cursor, style, "monospace",
                neo::ThemeMode::Dark, {}, nullptr, display->text, &colors, &info);
        const auto t4 = Clock::now();
        const View view = snapshot ? View(snapshot.get()) : View{};
        const auto layout = Model::InputLayout::build(
            *display, kViewportWidth, kViewportHeight, kControlWidth,
            8.0f, 8.0f, 8.0f, 20.0f, "monospace", kFontSize, true,
            view, snapshot, &changes);
        const auto t5 = Clock::now();
        const auto lpStageAfter = neo::lpPlanStageProfile();
        if (cold) scrollHigh = std::min(4096.0f, layout.maxVerticalScroll);

        // Basic checks stay outside the timed interval; the full document oracle
        // belongs in correctness tests, not in every measured iteration.
        if (state.text.size() != fixture.text.size() + (inserted ? 3u : 0u) ||
            Model::clampUtf8Boundary(state.text, state.cursor) != state.cursor ||
            state.cursor < 0 || state.cursor > static_cast<int>(state.text.size()) ||
            layout.lineList().empty() || !state.viewportMetrics || state.followCaret)
            fail("document, cursor, or layout invariant failed");
        if (options.mode == Mode::Edit && inserted &&
            state.text.compare(static_cast<std::size_t>(fixture.middleLineStart), 3, kInsert) != 0)
            fail("inserted UTF-8 text missing at middle boundary");
        if (composing) {
            if (display != state.preedit.get() ||
                display->text.size() != state.text.size() + state.compositionText.size() ||
                display->text.compare(static_cast<std::size_t>(fixture.middleLineStart),
                                      state.compositionText.size(), state.compositionText) != 0 ||
                display->cursor != fixture.middleLineStart + static_cast<int>(state.compositionText.size()) ||
                state.pendingEdit.valid)
                fail("synthesized preedit invariant failed");
        } else if (display != &state || state.preedit) {
            fail("unexpected display state");
        }
        if (options.mode == Mode::Scroll &&
            (state.verticalScroll < 0 || state.verticalScroll > layout.maxVerticalScroll))
            fail("scroll is outside measured geometry");

        std::cout << std::fixed << std::setprecision(3)
                  << "{\"schema\":\"editor-latency-stages/1\",\"scope\":\"headless_model_lp_layout\""
                  << ",\"phase\":\"" << phase << "\",\"round\":" << round
                  << ",\"mode\":\"" << modeName(options.mode) << "\""
                  << ",\"format\":\"" << formatName(options.format) << "\""
                  << ",\"fixture_bytes\":" << fixture.text.size()
                  << ",\"fixture_rows\":" << fixture.rows
                  << ",\"fixture_fnv1a64\":\"" << std::hex << std::setw(16)
                  << std::setfill('0') << fixture.fnv1a64 << std::dec << std::setfill(' ')
                  << "\",\"middle_edit_byte\":" << fixture.middleLineStart
                  << ",\"control_width\":" << kControlWidth
                  << ",\"control_height\":" << kControlHeight
                  << ",\"viewport_width\":" << kViewportWidth
                  << ",\"viewport_height\":" << kViewportHeight
                  << ",\"font_size\":" << kFontSize
                  << ",\"font_family\":\"monospace\",\"wrap\":" << (options.wrap ? "true" : "false")
                  << ",\"preedit_growth\":" << (options.preeditGrowth ? "true" : "false")
                  << ",\"scroll_y\":" << state.verticalScroll
                  << ",\"mutation_us\":" << micros(t0, t1)
                  << ",\"display_us\":" << micros(t1, t2);
        if (plan) std::cout << ",\"plan_us\":" << micros(t2, t3)
                            << ",\"decoration_us\":" << micros(t3, t4);
        else std::cout << ",\"plan_us\":null,\"decoration_us\":null";
        std::cout << ",\"layout_us\":" << micros(t4, t5)
                  << ",\"total_us\":" << micros(t0, t5)
                  << ",\"lp_index_us\":" << (lpStageAfter.indexRawLinesNs - lpStageBefore.indexRawLinesNs) / 1000.0
                  << ",\"lp_pass_a_us\":" << (lpStageAfter.skeletonPassANs - lpStageBefore.skeletonPassANs) / 1000.0
                  << ",\"lp_carry_us\":" << (lpStageAfter.carryCompareNs - lpStageBefore.carryCompareNs) / 1000.0
                  << ",\"lp_md4c_us\":" << (lpStageAfter.md4cNs - lpStageBefore.md4cNs) / 1000.0
                  << ",\"lp_finish_us\":" << (lpStageAfter.finishNs - lpStageBefore.finishNs) / 1000.0;
        printCounters(planBefore, decorationBefore, layoutBefore);
        std::cout << "}\n";
        if (!std::cout) fail("JSONL output failed");
    };

    iteration(-options.warmups - 1, "cold", true);
    for (int round = -options.warmups; round < 0; ++round)
        iteration(round, "warmup", false);
    for (int round = 0; round < options.rounds; ++round)
        iteration(round, "measured", false);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        run(parseArgs(argc, argv));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "editor_latency_benchmark: " << error.what() << "\n"
                  << "usage: editor_latency_benchmark --bytes 1048576|9437184 "
                     "--format plain|markdown --mode edit|scroll|preedit|steady "
                     "--rounds N --warmups N --wrap 0|1 --preedit-growth 0|1\n";
        return 2;
    }
}
