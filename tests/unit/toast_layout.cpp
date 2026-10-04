#include "components/toast.h"
#include "components/vector_icon.h"
#include <iostream>
#include <algorithm>

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

int main() {
    {
        core::dsl::Ui ui;
        ui.begin("hidden-toast-test");
        components::toast(ui, "sample").visible(false).screen(500, 300).build();
        ui.end(); ui.layout({500, 300});
        const auto* root = ui.find("sample");
        const auto* body = ui.find("sample.body.scroll");
        if (!check(root && root->disabled && body,
                   "Hidden toast must disable its entire interactive subtree during fade-out")) return 1;
    }
    const auto asciiWidth = [](const std::string& text) { return static_cast<float>(text.size()); };
    const std::string message = "Please use Windows default apps to change .txt and .md before removing EUI-Edits.";
    const auto wrapped = components::text_wrap::lines(message, 22, asciiWidth);
    const std::string joined = components::text_wrap::join(wrapped);
    if (!check(joined.find("EUI-Edits") != std::string::npos && joined.find("Windows") != std::string::npos,
               "Normal English words were split")) return 1;
    for (const auto& line : wrapped)
        if (!check(line.size() <= 22, "A prose line exceeded its available width")) return 1;
    auto overlong = components::text_wrap::lines("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 10, asciiWidth);
    std::string reconstructed;
    for (const auto& line : overlong) reconstructed += line;
    if (!check(reconstructed == "ABCDEFGHIJKLMNOPQRSTUVWXYZ" && overlong.size() == 3,
               "Long-word fallback lost content")) return 1;
    const auto unicodeWidth = [](const std::string& value) {
        float width = 0;
        for (unsigned char c : value) if ((c & 0xc0) != 0x80) width += c < 128 ? 1 : 2;
        return width;
    };
    const auto chinese = components::text_wrap::lines("请在 Windows 设置里选择 EUI-Edits。", 14, unicodeWidth);
    std::string continuous;
    for (const auto& line : chinese) continuous += line;
    if (!check(continuous.find("EUI-Edits") != std::string::npos && continuous.find("Windows") != std::string::npos,
               "Mixed Chinese/English word wrapping failed")) return 1;

    for (float screenWidth : {800.0f, 300.0f}) {
        for (float screenHeight : {600.0f, 180.0f}) {
            core::dsl::Ui ui;
            ui.begin("toast-test");
            components::toast(ui, "sample").visible().screen(screenWidth, screenHeight)
                .title("注销未完成").message("请先在 Windows 默认应用设置中将 .txt 和 .md 改选为其他程序，再注销 EUI-Edits。")
                .fontFamily("Microsoft YaHei").fontSize(18).titleFontSize(21)
                .transition(core::Transition::none())
                .iconRenderer([](core::dsl::Ui& iconUi, const std::string& id, float x, float y, float size, core::Color color) {
                    components::vector_icon::drawCheckmark(iconUi, id, x, y, size, color);
                }).build();
            ui.end(); ui.layout({screenWidth, screenHeight});
            const auto* bg = ui.find("sample.bg");
            const auto* body = ui.find("sample.body.scroll");
            const auto* text = ui.find("sample.message");
            const auto* title = ui.find("sample.title");
            if (!check(bg && body && text && title, "Toast elements missing")) return 1;
            if (!check(bg->frame.x >= 0 && bg->frame.y >= 0 &&
                       bg->frame.x+bg->frame.width <= screenWidth && bg->frame.y+bg->frame.height <= screenHeight,
                       "Toast exceeded the viewport")) return 1;
            if (!check(body->frame.y >= title->frame.y+title->frame.height &&
                       body->frame.y+body->frame.height <= bg->frame.y+bg->frame.height-15,
                       "Body overlaps title or escapes its background")) return 1;
            if (!check(text->text.find("EUI-Edits") != std::string::npos &&
                       text->text.find("Windows") != std::string::npos,
                       "Actual measured wrapping split normal English words")) return 1;
            if (!check(text->frame.height <= body->frame.height+0.01f || body->scrollMaxOffset > 0,
                       "Short viewport does not provide scrolling for a long notification")) return 1;
        }
    }
    return 0;
}
