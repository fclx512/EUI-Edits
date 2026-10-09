// CPU-only warm InputBuilder compose stages; no rendering or GUI latency claims.
#include "components/input.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <string>

using Clock = std::chrono::steady_clock;
using Model = components::input_detail::InputModel;

double elapsed(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

void run(std::size_t bytes, bool longLine, bool borrowed) {
    std::string text;
    const std::string row = longLine ? "a中🙂" : std::string(96, 'a') + "中🙂\n";
    while (text.size() < bytes) text += row;
    core::dsl::Ui ui;
    // Cold layout excluded. Every subsequent frame changes the selection.
    for (int round = -1; round < 3; ++round) {
        double transfer = 0, build = 0, tail = 0;
        const int iterations = round < 0 ? 2 : 80;
        for (int i = 0; i < iterations; ++i) {
            const auto t0 = Clock::now();
            ui.begin("compose-bench");
            auto& state = ui.state<Model::InputState>("editor");
            state.followCaret = false;
            state.cursor = i % 40;
            state.selectionStart = 0;
            state.selectionEnd = state.cursor;
            auto input = components::input(ui, "editor");
            input.size(800, 500).multiline(true).wordWrap(false)
                .viewportMetrics(true).inset(8).fontFamily("monospace").fontSize(16)
                .transition(core::Transition::none());
            const auto t1 = Clock::now();
            if (borrowed) input.valueRef(text);
            else input.value(text);
            const auto t2 = Clock::now();
            input.build();
            const auto t3 = Clock::now();
            ui.end();
            ui.layout(core::dsl::Screen{800, 500});
            const auto t4 = Clock::now();
            transfer += elapsed(t1, t2);
            build += elapsed(t2, t3);
            tail += elapsed(t0, t1) + elapsed(t3, t4);
            if (state.text != text) std::abort();
        }
        if (round >= 0) std::cout << "{\"mode\":\"" << (borrowed ? "borrowed" : "owned")
            << "\",\"bytes\":" << text.size()
            << ",\"long_line\":" << (longLine ? "true" : "false")
            << ",\"round\":" << round << ",\"iterations\":" << iterations
            << ",\"transfer_us\":" << transfer / iterations
            << ",\"build_us\":" << build / iterations
            << ",\"tail_us\":" << tail / iterations << "}\n";
    }
}
int main(int argc, char** argv) {
    const bool borrowed = argc > 1 && std::string(argv[1]) == "borrowed";
    std::cout << std::fixed << std::setprecision(3);
    run(80 * 1024, true, borrowed);
    if (argc > 2 && std::string(argv[2]) == "long") {
        run(1024 * 1024, true, borrowed);
        return 0;
    }
    for (std::size_t bytes : {80 * 1024, 1024 * 1024, 8 * 1024 * 1024}) run(bytes, false, borrowed);
}
