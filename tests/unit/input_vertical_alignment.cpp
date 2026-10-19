#include "components/input.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string>

namespace {

bool near(float actual, float expected, float epsilon = 0.05f) {
    return std::fabs(actual - expected) <= epsilon;
}

bool checkSingleLine(float height, float fontSize, float layoutScale, const std::string& value) {
    using InputState = components::input_detail::InputModel::InputState;
    core::TextPrimitive::setLayoutPixelScale(layoutScale);

    core::dsl::Ui ui;
    ui.begin("input.vertical");
    InputState& state = ui.state<InputState>("field");
    state.text = value; // Keep component initialization from resetting the requested selection.
    state.cursor = static_cast<int>(value.size());
    if (!value.empty()) {
        state.selectionStart = 0;
        state.selectionEnd = static_cast<int>(value.size());
    }
    components::input(ui, "field")
        .position(0.0f, 0.0f)
        .size(260.0f, height)
        .fontFamily("monospace")
        .fontSize(fontSize)
        .placeholder("Search 中文")
        .value(value)
        .build();
    ui.end();
    ui.layout(260.0f, height);

    const float lineHeight = fontSize * 1.2f;
    const float expectedTop = std::max(0.0f, (height - lineHeight) * 0.5f);
    const auto* root = ui.find("field");
    const auto* viewport = ui.find("field.textViewport");
    const auto* text = ui.find("field.text");
    const auto* hit = ui.find("field.hit");
    const auto* surface = ui.find("field.surface");
    if (!surface || surface->frame.x <= root->frame.x ||
        surface->frame.x + surface->frame.width >= root->frame.x + root->frame.width ||
        !near(hit->frame.width,260)) {
        std::cerr << "surface must retain scissor clearance without shrinking pointer bounds\n";
        return false;
    }
    if (!root || !viewport || !text || !hit) {
        std::cerr << "single-line input tree is incomplete\n";
        return false;
    }
    if (!root->clip || !viewport->clip ||
        !near(viewport->frame.y, expectedTop) || !near(viewport->frame.height, lineHeight)) {
        std::cerr << "single-line viewport geometry mismatch for height=" << height
                  << " font=" << fontSize << " scale=" << layoutScale << "\n";
        return false;
    }
    if (text->verticalAlign != core::VerticalAlign::Center || text->text != (value.empty() ? "Search 中文" : value)) {
        std::cerr << "placeholder and value must both use ink-centered vertical alignment\n";
        return false;
    }
    if (!hit->hasImeRect ||
        !near(hit->imeRect.y + hit->frame.y + hit->imeRect.height * 0.5f,
              viewport->frame.y + viewport->frame.height * 0.5f, 0.2f)) {
        std::cerr << "IME anchor must remain centered in the single-line text box\n";
        return false;
    }

    if (!value.empty()) {
        const auto* selection = ui.find("field.selection.0");
        if (!selection ||
            !near(selection->frame.y + selection->frame.height * 0.5f,
                  viewport->frame.y + viewport->frame.height * 0.5f, 0.2f)) {
            std::cerr << "single-line selection band must stay centered on the text line\n";
            return false;
        }
    }
    return true;
}

bool multilineStillUsesTopAlignment(float fontSize, float layoutScale) {
    core::TextPrimitive::setLayoutPixelScale(layoutScale);
    core::dsl::Ui ui;
    ui.begin("input.multiline");
    components::input(ui, "field")
        .position(0.0f, 0.0f)
        .size(260.0f, 120.0f)
        .fontFamily("monospace")
        .fontSize(fontSize)
        .multiline()
        .value("line one\nline two")
        .build();
    ui.end();
    ui.layout(260.0f, 120.0f);

    const auto* text = ui.find("field.text.0");
    if (!text || text->verticalAlign != core::VerticalAlign::Top) {
        std::cerr << "multiline input must keep top-aligned per-line rendering\n";
        return false;
    }
    return true;
}

bool reservedContentKeepsScrollbarAtEdge(float width, float reserve, float scale) {
    core::TextPrimitive::setLayoutPixelScale(scale);
    core::dsl::Ui full, reference;
    std::string text;
    for (int i = 0; i < 60; ++i) text += "Long line for wrapping and scrollbar geometry 中文 0123456789\n";
    const auto compose = [&](core::dsl::Ui& ui, float outer, float inset) {
        ui.begin("reserved.content");
        components::input(ui, "field").size(outer, 180).multiline().scrollbar()
            .fontFamily("monospace").fontSize(16).lineNumbers()
            .inset(12).contentRightInset(inset).maxTextWidth(700).value(text).build();
        ui.end();
        ui.layout(outer, 180);
    };
    compose(full, width, reserve);
    compose(reference, width - reserve, 0);
    const auto* bar = full.find("field.scrollbar.thumb");
    const auto* viewport = full.find("field.textViewport");
    const auto* oldViewport = reference.find("field.textViewport");
    const auto* oldBar = reference.find("field.scrollbar.thumb");
    if (!bar || !oldBar || !viewport || !oldViewport ||
        !near(bar->frame.x + bar->frame.width, width) ||
        !near(viewport->frame.x, oldViewport->frame.x) ||
        !near(viewport->frame.width, oldViewport->frame.width) ||
        !near(bar->frame.height, oldBar->frame.height)) {
        std::cerr << "content clearance must preserve wrapping while the scrollbar stays at the outer edge\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    constexpr std::array<float, 2> heights{32.0f, 36.0f};
    constexpr std::array<float, 3> fontSizes{12.0f, 14.0f, 18.0f};
    constexpr std::array<float, 3> scales{1.0f, 1.25f, 1.5f};
    constexpr std::array<const char*, 2> values{"", "Actual text 中文"};

    for (const float scale : scales) {
        for (const float width : {320.0f, 640.0f, 1000.0f}) {
            if (!reservedContentKeepsScrollbarAtEdge(width, 72.0f, scale)) return 3;
        }
        for (const float height : heights) {
            for (const float fontSize : fontSizes) {
                for (const char* value : values) {
                    if (!checkSingleLine(height, fontSize, scale, value)) {
                        core::TextPrimitive::setLayoutPixelScale(1.0f);
                        return 1;
                    }
                }
                if (!multilineStillUsesTopAlignment(fontSize, scale)) {
                    core::TextPrimitive::setLayoutPixelScale(1.0f);
                    return 2;
                }
            }
        }
    }
    core::TextPrimitive::setLayoutPixelScale(1.0f);
    std::cout << "Input vertical alignment, viewport, selection and IME geometry passed\n";
    return 0;
}
