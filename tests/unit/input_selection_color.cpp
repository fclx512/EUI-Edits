#include "components/input.h"
#include "core/dsl_runtime.h"
#include <cmath>
#include <iostream>

int main() {
    using Model = components::input_detail::InputModel;
    core::dsl::Runtime runtime;
    runtime.initialize();
    core::dsl::Ui* view = nullptr;
    components::InputStyle style;
    style.cursor = {.12f, .34f, .56f, 1};
    const auto same = [](core::Color a, core::Color b) {
        return std::abs(a.r-b.r)<.00001f && std::abs(a.g-b.g)<.00001f &&
               std::abs(a.b-b.b)<.00001f && std::abs(a.a-b.a)<.00001f;
    };
    auto compose = [&](bool composition = false) {
        runtime.compose("selection-test", 320, 120, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        view = &ui;
        auto& state = ui.state<Model::InputState>("field");
        if (state.text.empty()) Model::loadDocument(state, "alpha beta");
        state.selectionStart=0; state.selectionEnd=5; state.cursor=5;
        state.compositionText = composition ? "test" : "";
        ++state.compositionRevision;
        ui.requestFocus("field.hit");
        components::input(ui,"field").size(320,120).fontSize(16).caretBlink(false)
            .value("alpha beta").style(style).build();
        });
    };
    compose(); compose();
    auto& ui = *view;
    const auto selection = [&]() { const auto* p=ui.find("field.selection.0"); return p ? p->color : core::Color{}; };
    if (!ui.find("field.selection.0") || !same(selection(), {.12f,.34f,.56f,.24f})) return 1;
    style.selectionBackground = core::Color{.8f,.2f,.3f,.6f};
    compose();
    if (!same(selection(), *style.selectionBackground)) return 2;
    const auto key = ui.find("field")->dirtyKey;
    style.cursor = {.3f,.9f,.4f,1};
    compose();
    if (!same(selection(), *style.selectionBackground)) return 3;
    style.selectionBackground->a = 0;
    compose();
    if (!same(selection(), *style.selectionBackground) || ui.find("field")->dirtyKey == key) return 4;
    style.selectionBackground->a = .6f;
    compose(true);
    if (!same(selection(), {.8f,.2f,.3f,.3f})) return 5;
    const auto* underline=ui.find("field.composition.underline.0");
    if (!underline || !same(underline->color, style.cursor)) return 6;
    runtime.shutdown();
    std::cout << "input_selection_color: explicit RGBA, zero alpha, caret isolation and composition PASS\n";
    return 0;
}
