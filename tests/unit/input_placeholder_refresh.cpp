#include "components/input.h"

#include <iostream>
#include <string>

int main() {
    using Model = components::input_detail::InputModel;
    for (bool multiline : {false, true}) {
        core::dsl::Ui ui;
        std::string value;
        auto compose = [&](const std::string& placeholder) {
            ui.begin("placeholder-refresh");
            components::input(ui, "field").size(320, 160).multiline(multiline)
                .value(value).placeholder(placeholder).build();
            ui.end();
            ui.layout(320, 160);
        };
        compose("筛选文件…");
        auto& state = ui.state<Model::InputState>("field");
        const auto revision = state.textRevision;
        const auto rootKey = ui.find("field")->dirtyKey;
        const auto textKey = ui.find(value.empty() || !multiline ? "field.text" : "field.text.0")->dirtyKey;
        compose("Filter files…");
        const auto* text = ui.find(value.empty() || !multiline ? "field.text" : "field.text.0");
        if (ui.find("field")->dirtyKey == rootKey || text->dirtyKey == textKey ||
            text->text != "Filter files…" || state.textRevision != revision ||
            !state.text.empty() || state.cursor != 0 || !state.undoStack.empty()) return 1;
        const auto englishKey = ui.find("field")->dirtyKey;
        compose("Filter files…");
        if (ui.find("field")->dirtyKey != englishKey) return 2;
        // Reversing the language and changing mode must also update an empty field.
        compose("筛选文件…");
        if (ui.find("field")->dirtyKey != rootKey) return 3;
        compose("输入路径…");
        if (ui.find("field")->dirtyKey == rootKey ||
            ui.find(value.empty() || !multiline ? "field.text" : "field.text.0")->text != "输入路径…") return 4;
        // Placeholder-only updates must not invalidate a populated editor's text.
        value = "user text";
        compose("输入路径…");
        const auto populatedRoot = ui.find("field")->dirtyKey;
        const auto populatedText = ui.find(value.empty() || !multiline ? "field.text" : "field.text.0")->dirtyKey;
        const auto populatedRevision = state.textRevision;
        compose("Enter path…");
        if (ui.find("field")->dirtyKey != populatedRoot ||
            ui.find(value.empty() || !multiline ? "field.text" : "field.text.0")->dirtyKey != populatedText ||
            ui.find(value.empty() || !multiline ? "field.text" : "field.text.0")->text != value || state.textRevision != populatedRevision) return 5;
    }
    std::cout << "Placeholder language/mode refresh preserves input state and populated caches\n";
}
