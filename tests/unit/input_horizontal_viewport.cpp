#include "components/input.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Model = components::input_detail::InputModel;
using Decoration = components::input_detail::LineDecoration;

int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

std::size_t textRuns(const core::dsl::Element& element) {
    std::size_t count = element.id.find("editor.text.0r") != std::string::npos &&
                        element.kind == core::dsl::ElementKind::Text;
    for (const auto& child : element.children) count += textRuns(*child);
    return count;
}
std::size_t textRuns(const core::dsl::Ui& ui) {
    std::size_t count = 0;
    for (const auto& root : ui.roots()) count += textRuns(*root);
    return count;
}

void testHorizontalViewport() {
    core::dsl::Ui ui;
    std::string text;
    Decoration d;
    for (int i = 0; i < 2000; ++i) {
        const int beg = static_cast<int>(text.size());
        text += i % 2 ? "中文" : "token";
        components::input_detail::LineRun run;
        run.beg = beg;
        run.end = static_cast<int>(text.size());
        run.style.color = i % 2 ? core::Color{1, 0, 0, 1} : core::Color{0, 0, 1, 1};
        if (i == 1) { run.style.link = true; run.style.background = {1, 1, 0, 1}; }
        if (i == 2) { run.style.strike = true; run.style.underline = true; }
        d.runs.push_back(run);
    }
    std::vector<Decoration> decorations{d};
    ui.begin("horizontal-viewport");
    auto& state = ui.state<Model::InputState>("editor");
    Model::loadDocument(state, text);
    state.cursor = 0;
    Model::clearSelection(state);
    state.followCaret = false;
    const auto compose = [&] {
        ui.begin("horizontal-viewport");
        components::input(ui, "editor").size(400, 160).inset(10).fontSize(16)
            .fontFamily("monospace").multiline().wordWrap(false).value(text)
            .lineDecorator(components::input_detail::LineDecorationProvider(
                [&](const std::string&) { return decorations; })).build();
        ui.end();
    };
    compose();
    check(!state.cachedLines.empty(), "the input must create a layout in its page state scope");
    if (state.cachedLines.empty()) return;
    const auto& row = state.cachedLines.front();
    check(row.runs.size() == 2000, "the full styled line must remain available for editing");
    const std::size_t emitted = textRuns(ui);
    std::cout << "initial styled nodes=" << emitted << ", source runs=" << row.runs.size() << '\n';
    check(emitted < 100, "offscreen styled tokens must not become UI text nodes");
    check(ui.find("editor.text.0r0") && ui.find("editor.text.0r1.linkhover") &&
          ui.find("editor.text.0r1.bg"), "visible token and link/chip decorations must be retained");
    check(ui.find("editor.text.0r2.strike") && ui.find("editor.text.0r2.underline"),
          "visible strike and underline must be retained");
    check(ui.find("editor.text.0r1999") == nullptr, "far right token must be omitted at the left edge");

    // Place a real text run across the left clip edge. Its source text and style
    // must remain intact, and hit/selection geometry still uses the full line.
    const auto crossing = row.runs[500];
    state.horizontalScroll = crossing.x + crossing.width * 0.5f;
    state.selectionStart = crossing.beg;
    state.selectionEnd = crossing.end;
    compose();
    const auto* token = ui.find("editor.text.0r500");
    check(token && token->text == text.substr(crossing.beg, crossing.end - crossing.beg),
          "partly clipped tokens must keep their complete source text");
    check(ui.find("editor.selection.0") != nullptr, "horizontal clipping must keep the visible selection");
    check(textRuns(ui) < 100, "horizontal scrolling must keep node count bounded");
    check(ui.find("editor.text.0r0") == nullptr, "scrolled-off left token must be omitted");

    state.horizontalScroll = state.cachedTextWidth;
    Model::clearSelection(state);
    compose();
    check(ui.find("editor.text.0r1999") != nullptr, "rightmost token must appear when scrolling to the end");
    check(state.text == text && state.undoStack.empty(), "render culling must not edit the document");

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) compose();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "20 stable compositions ms=" << ms << '\n';
}
}

int main() {
    testHorizontalViewport();
    return failures ? 1 : 0;
}
