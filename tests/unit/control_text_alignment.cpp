#include "components/checkbox.h"
#include "components/contextmenu.h"
#include "components/datepicker.h"
#include "components/dropdown.h"
#include "components/radio.h"
#include "components/switch.h"
#include "components/tabs.h"
#include "components/timepicker.h"
#include "core/render/render_backend.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TextRecorder final : public core::render::RenderBackend {
public:
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int, int) override { return false; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int, int, const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode,
                         const std::vector<core::Rect>&) override {}
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&, int, int) override {}
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand& command, int, int) override {
        if (command.vertices != nullptr)
            vertices.insert(vertices.end(), command.vertices,
                            command.vertices + command.vertexFloatCount);
    }
    std::vector<float> vertices;
};

void require(bool valid, const std::string& message) {
    if (!valid) throw std::runtime_error(message);
}

float midpoint(const core::LayoutRect& rect) { return rect.y + rect.height * .5f; }

// Independently render the shared reference glyphs at the *produced element's*
// origin. This catches a wrong font-scale conversion or line-box placement even
// when every widget happens to call the same alignment helper.
float paintedReferenceCenter(const core::dsl::Element& label, float scale,
                             TextRecorder& backend) {
    require(label.verticalAlign == core::VerticalAlign::Top,
            label.id + " must use the shared baseline rather than per-label ink centering");
    backend.vertices.clear();
    core::TextPrimitive primitive;
    require(primitive.initialize(), "reference text renderer did not initialize");
    primitive.setText("Ag中文");
    primitive.setFontFamily(label.fontFamily);
    primitive.setFontSize(label.fontSize * scale);
    primitive.setFontWeight(label.fontWeight);
    primitive.setPosition(label.frame.x * scale, label.frame.y * scale);
    primitive.render(2048, 1536);
    require(!backend.vertices.empty() && backend.vertices.size() % 5 == 0,
            label.id + " did not produce reference glyph vertices");
    float top = std::numeric_limits<float>::infinity();
    float bottom = -top;
    for (std::size_t index = 1; index < backend.vertices.size(); index += 5) {
        top = std::min(top, backend.vertices[index]);
        bottom = std::max(bottom, backend.vertices[index]);
    }
    require(std::isfinite(top) && bottom > top, label.id + " has empty ink bounds");
    return (top + bottom) * .5f;
}

void checkCentered(const core::dsl::Element* label, const core::dsl::Element* row,
                   float scale, TextRecorder& backend, const std::string& context) {
    require(label != nullptr && row != nullptr, context + " element missing");
    const float inkCenter = paintedReferenceCenter(*label, scale, backend);
    const float target = midpoint(row->frame) * scale;
    if (std::fabs(inkCenter - target) > .65f) {
        throw std::runtime_error(context + " ink center=" + std::to_string(inkCenter) +
                                 " row center=" + std::to_string(target) +
                                 " scale=" + std::to_string(scale));
    }
}

const core::dsl::Element* find(core::dsl::Ui& ui, const std::string& id) {
    const auto* element = ui.find(id);
    require(element != nullptr, "missing " + id);
    return element;
}

void run(float scale, float font, TextRecorder& backend) {
    core::TextPrimitive::setLayoutPixelScale(scale);
    auto tokens = components::theme::light();
    tokens.metrics.typography.option = font;
    tokens.metrics.typography.micro = font - 1.0f;
    tokens.metrics.typography.control = font;
    tokens.metrics.typography.heading = font;
    tokens.metrics.typography.input = font;
    tokens.metrics.control.menuItem = 38.0f;

    core::dsl::Ui ui;
    ui.begin("control-text-alignment");
    using Item = components::ContextMenuItem;
    std::vector<Item> items;
    items.emplace_back("中文 English 123");
    items.back().withShortcut("Ctrl+1").withChecked();
    items.emplace_back("Ag 19");
    items.back().withShortcut("Ctrl+2");
    items.emplace_back("123", std::vector<Item>{Item("子菜单")});
    components::contextMenu(ui, "menu").open().screen(800, 600).position(50, 45)
        .size(260, 38).items(items).theme(tokens).build();
    components::dropdown(ui, "drop").size(220, 38).items({"中文 English 123", "Ag 19", "123"})
        .open().theme(tokens).build();
    components::checkbox(ui, "check").size(240, 38).text("中文 English 123")
        .fontSize(font).checked(true).theme(tokens).build();
    components::radio(ui, "radio").size(240, 38).text("Ag 19")
        .fontSize(font).selected(true).theme(tokens).build();
    components::toggleSwitch(ui, "switch").size(240, 38).text("123")
        .fontSize(font).checked(true).theme(tokens).build();
    components::tabs(ui, "tabs").size(330, 38).items({"中文 English 123", "Ag 19", "123"})
        .fontSize(font).theme(tokens).build();
    components::datePicker(ui, "date").open().screen(800, 600).theme(tokens).build();
    components::timePicker(ui, "time").open().screen(800, 600).theme(tokens).build();
    ui.end();
    ui.layout(core::dsl::Screen{800, 600});

    const auto* first = find(ui, "menu.level.0.label.0");
    const auto* row0 = find(ui, "menu.level.0.item.0");
    checkCentered(first, row0, scale, backend, "menu mixed label");
    checkCentered(find(ui, "menu.level.0.shortcut.0"), row0, scale, backend,
                  "menu shortcut beside checked item");
    require(find(ui, "menu.level.0.check.0") != nullptr,
            "checked row lost its vector checkmark");
    require(find(ui, "menu.level.0.arrow.2") != nullptr,
            "child row lost its vector arrow");
    for (int index = 1; index < 3; ++index) {
        const auto* label = find(ui, "menu.level.0.label." + std::to_string(index));
        const auto* row = find(ui, "menu.level.0.item." + std::to_string(index));
        checkCentered(label, row, scale, backend, "menu label " + std::to_string(index));
        require(std::fabs((label->frame.y - row->frame.y) -
                          (first->frame.y - row0->frame.y)) < .01f,
                "mixed-language menu rows no longer share one baseline");
    }
    checkCentered(find(ui, "menu.level.0.shortcut.1"),
                  find(ui, "menu.level.0.item.1"), scale, backend, "menu shortcut 1");

    for (int index = 0; index < 3; ++index)
        checkCentered(find(ui, "drop.item.label." + std::to_string(index)),
                      find(ui, "drop.item." + std::to_string(index)), scale, backend,
                      "dropdown item " + std::to_string(index));
    checkCentered(find(ui, "check.label"), find(ui, "check"), scale, backend, "checkbox");
    checkCentered(find(ui, "radio.label"), find(ui, "radio"), scale, backend, "radio");
    checkCentered(find(ui, "switch.label"), find(ui, "switch"), scale, backend, "switch");
    for (int index = 0; index < 3; ++index)
        checkCentered(find(ui, "tabs.label." + std::to_string(index)),
                      find(ui, "tabs.hit." + std::to_string(index)), scale, backend,
                      "tabs label " + std::to_string(index));
    checkCentered(find(ui, "date.title"), find(ui, "date.done.bg"), scale, backend,
                  "date picker heading");
    checkCentered(find(ui, "time.title"), find(ui, "time.done.bg"), scale, backend,
                  "time picker heading");
}

} // namespace

int main() {
    try {
        TextRecorder backend;
        core::render::ScopedRenderBackend scope(backend);
#if defined(_WIN32)
        // Match EUI-Edits' actual default UI font, rather than relying only on
        // the framework's system-font fallback. A second family checks refresh.
        core::TextPrimitive::setDefaultFontFiles("C:/Windows/Fonts/msyh.ttc", "");
#endif
        for (float scale : {1.25f, 1.5f})
            for (float font : {12.0f, 14.0f, 18.0f}) run(scale, font, backend);
#if defined(_WIN32)
        core::TextPrimitive::setDefaultFontFiles("C:/Windows/Fonts/segoeui.ttf", "");
        for (float scale : {1.25f, 1.5f}) run(scale, 14.0f, backend);
#endif
        core::TextPrimitive::setLayoutPixelScale(1.0f);
        core::TextStyle single;
        single.text = "Ag中文"; single.fontSize = 14; single.lineHeight = 30;
        auto multiple = single; multiple.text += "\nAg中文";
        const auto first = core::TextPrimitive::measureSingleLineInk(single);
        const auto compatible = core::TextPrimitive::measureSingleLineInk(multiple);
        const auto complete = core::TextPrimitive::measureTextInk(multiple);
        require(core::closeEnough(first, compatible), "single-line API changed its explicit-newline contract");
        require(std::fabs(complete.y-first.y-30)<.01f, "complete ink block lost the final wrapped line");
    } catch (const std::exception& error) {
        core::TextPrimitive::setLayoutPixelScale(1.0f);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "control text ink centers and shared baselines passed\n";
}
