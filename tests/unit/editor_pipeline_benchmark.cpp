// Real LP plan -> snapshot -> InputBuilder warm CPU stages. No GUI/present.
#include "components/input.h"
#include "model/lp_decorations.h"
#include "model/style_schema.h"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>

using Clock = std::chrono::steady_clock;
using Model = components::input_detail::InputModel;
double us(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

void run(std::size_t bytes, bool markdown, bool numbers) {
    std::string text;
    int rows = 0;
    while (text.size() < bytes) {
        text += markdown ? (rows % 16 == 0 ? "## heading 中🙂\n\n" :
                "paragraph 中🙂 **bold** and `code` sample text sample text sample text\n\n")
                         : std::string(96, 'a') + "中🙂\n";
        ++rows;
    }
    const auto colors = neo::editorColors(neo::ThemeMode::Dark);
    const auto style = neo::markdownStyle(16, "monospace", "monospace", colors);
    neo::lp::invalidatePlanCache();
    neo::lp::invalidateDecorationCache();
    core::dsl::Ui ui;
    for (int round = -1; round < 3; ++round) {
        double total = 0, planTime = 0, decorationTime = 0, buildTime = 0;
        const int iterations = round < 0 ? 2 : 80;
        for (int i = 0; i < iterations; ++i) {
            const auto start = Clock::now();
            ui.begin("pipeline-bench");
            auto& state = ui.state<Model::InputState>("editor");
            state.followCaret = false;
            state.cursor = state.selectionEnd = i % 12;
            state.selectionStart = 0;
            auto input = components::input(ui, "editor");
            input.size(800, 500).valueRef(text).multiline(true).wordWrap(true)
                .viewportMetrics(true).lineNumbers(numbers).fontFamily("monospace")
                .fontSize(16).inset(8).transition(core::Transition::none());
            if (markdown) input.lineDecorationSnapshot(
                [&](const std::string& committed, const components::input_detail::DecoratorEditInfo& info) {
                    const auto p0 = Clock::now();
                    const auto& plan = neo::lp::cachedPlan(committed, &info);
                    const auto p1 = Clock::now();
                    // Like held-pointer editorView, freeze the active block cursor.
                    const auto snapshot = neo::lp::cachedDecorationSnapshot(
                        plan, neo::lp::planCache().version, 0, style, "monospace",
                        neo::ThemeMode::Dark, {}, nullptr, committed, &colors, &info);
                    const auto p2 = Clock::now();
                    planTime += us(p0, p1);
                    decorationTime += us(p1, p2);
                    return snapshot;
                });
            const auto b0 = Clock::now();
            input.build();
            const auto b1 = Clock::now();
            ui.end();
            ui.layout(core::dsl::Screen{800, 500});
            total += us(start, Clock::now());
            buildTime += us(b0, b1);
            if (state.text != text) std::abort();
        }
        if (round >= 0) std::cout << "{\"bytes\":" << text.size()
            << ",\"markdown\":" << (markdown ? "true" : "false")
            << ",\"numbers\":" << (numbers ? "true" : "false")
            << ",\"round\":" << round << ",\"iterations\":" << iterations
            << ",\"total_us\":" << total / iterations
            << ",\"build_us\":" << buildTime / iterations
            << ",\"plan_us\":" << planTime / iterations
            << ",\"decoration_us\":" << decorationTime / iterations << "}\n";
    }
}
int main(int argc, char** argv) {
    const auto bytes = argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 1024 * 1024;
    const bool markdown = argc > 2 && std::string(argv[2]) == "markdown";
    const bool numbers = argc > 3 && std::string(argv[3]) == "numbers";
    std::cout << std::fixed << std::setprecision(3);
    run(bytes, markdown, numbers);
}
