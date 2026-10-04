#include "eui/dsl_app.h"
#include "components/button.h"
#include <cmath>
#include <iostream>
#include <type_traits>

static bool same(const eui::Color& a, const eui::Color& b) {
    return std::fabs(a.r - b.r) < 0.00001f && std::fabs(a.g - b.g) < 0.00001f &&
           std::fabs(a.b - b.b) < 0.00001f && std::fabs(a.a - b.a) < 0.00001f;
}

int main() {
    using eui::Color;
    static_assert(sizeof(Color) == 4 * sizeof(float));
    static_assert(std::is_trivially_copyable_v<Color> && std::is_standard_layout_v<Color>);
    constexpr Color rgba{0.2f, 0.4f, 0.6f, 0.8f};
    constexpr Color partial{0};
    static_assert(partial.r == 0.f && partial.g == 1.f && partial.b == 1.f && partial.a == 1.f);
    constexpr auto rgb = Color::fromHex(0x336699u);
    static_assert(rgb.r == 0.2f && rgb.a == 1.0f);
    for (const char* value : {"#369c", "#369C", "#336699cc", "#336699CC"}) {
        if (!same(Color(value), rgba)) return 1;
    }
    if (!same(Color("#369"), rgb) || !same(Color("#336699"), rgb) ||
        !same(Color(std::string_view("#336699")), rgb) || !same(Color::fromHex("#369"), rgb) ||
        !same(Color::fromHexRgba(0x336699ccu), rgba) ||
        !same(Color::fromHexRgba(0x00000080u), Color{0.f, 0.f, 0.f, 128.f / 255.f}) ||
        !same(Color{}, Color{1.f, 1.f, 1.f, 1.f})) return 2;
    for (const char* bad : {"", "#", "#12", "#12345", "#1234567", "#123456789", "#GGG", "336699", " #369", "#369 ", "#-12345"}) {
        Color output = rgb;
        if (Color::tryFromHex(bad, output) || !same(output, rgb) ||
            !same(Color(bad), Color{0.f, 0.f, 0.f, 0.f})) return 3;
    }
    if (!same(Color(static_cast<const char*>(nullptr)), Color{0.f, 0.f, 0.f, 0.f})) return 4;
    core::dsl::Ui ui;
    ui.begin("hex");
    ui.rect("rect").color("#369").border(1.f, "#fff").gradient("#000", "#fff8").build();
    ui.text("text").color(std::string("#336699cc")).build();
    components::button(ui, "button").colors("#123", "#456", "#789").textColor("#fff").build();
    ui.end();
    if (!same(ui.find("rect")->border.color, Color{1.f, 1.f, 1.f, 1.f}) ||
        !same(ui.find("text")->textColor, rgba) ||
        !same(app::DslAppConfig{}.clearColor("#369").clearColorValue, rgb)) return 5;
    std::cout << "HEX parsing, alpha, invalid input, RGBA compatibility and DSL conversions passed\n";
}
