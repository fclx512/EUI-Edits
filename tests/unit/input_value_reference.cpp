#include "components/input.h"
#include "core/dsl_runtime.h"
#include <iostream>
#include <type_traits>
#include <utility>

using Model = components::input_detail::InputModel;
template<class T, class = void> struct CanBorrow : std::false_type {};
template<class T> struct CanBorrow<T, std::void_t<decltype(
    std::declval<components::InputBuilder&>().valueRef(std::declval<T>()))>> : std::true_type {};
static_assert(CanBorrow<std::string&>::value && CanBorrow<const std::string&>::value);
static_assert(!CanBorrow<std::string&&>::value && !CanBorrow<const std::string&&>::value);
static_assert(!CanBorrow<const char*>::value);

int main() {
    int failures = 0;
    auto check = [&](bool yes, const char* message) {
        if (!yes) { ++failures; std::cerr << message << '\n'; }
    };
    core::dsl::Runtime runtime;
    runtime.initialize();
    core::dsl::Ui* uiPointer = nullptr;
    runtime.compose("reference", 400, 160, [&](core::dsl::Ui& view, const core::dsl::Screen&) {
        uiPointer = &view;
    });
    auto& ui = *uiPointer;
    std::string document = "first 中🙂\nsecond";
    auto compose = [&](bool borrowed) {
        runtime.compose("reference", 400, 160, [&](core::dsl::Ui&, const core::dsl::Screen&) {
            auto input = components::input(ui, "editor");
            input.size(400, 160).multiline(true).wordWrap(false).lineNumbers(true)
                .fontFamily("monospace").fontSize(16).transition(core::Transition::none())
                .onChange([&](const std::string& text) { document = text; });
            if (borrowed) input.valueRef(document);
            else input.value(document);
            input.build();
            ui.requestFocus("editor.hit");
        });
    };
    compose(true);
    auto& state = ui.state<Model::InputState>("editor");
    check(state.text == document, "initial borrowed document must load");
    state.cursor = state.selectionEnd = 5;
    state.selectionStart = 0;
    const auto revision = state.textRevision;
    compose(true);
    check(state.textRevision == revision && state.cursor == 5 && state.selectionStart == 0,
          "equal borrowed text preserves revision and selection");
    // External rewrite of the same object, same length, without a revision token.
    document.replace(0, 5, "other");
    compose(true);
    check(state.text == document && state.textRevision == revision + 1 &&
          !state.pendingEdit.valid && state.selectionStart == state.selectionEnd,
          "same-sized external rewrite must invalidate and synchronize");
    // Both setter orders and builder copies/moves must select the last source.
    const std::string ignored = "ignored";
    ui.begin("reference");
    auto builder = components::input(ui, "editor");
    builder.size(400, 160).valueRef(ignored).value(document);
    auto moved = std::move(builder);
    moved.build();
    check(state.text == document, "owning setter clears previous borrowed source after move");
    auto second = components::input(ui, "editor");
    second.size(400, 160).value("ignored").valueRef(document);
    auto copied = second;
    copied.build();
    check(state.text == document, "borrowed setter wins after builder copy");
    ui.end();
    compose(true);
    check(ui.isFocused("editor.hit"), "runtime must focus the input for preedit display");
    state.cursor = state.selectionStart = state.selectionEnd = 0;
    // Event callbacks run after build, when the source contents may change.
    core::TextInputEvent preedit;
    preedit.compositionText = "拼🙂";
    preedit.composing = true;
    preedit.compositionChanged = true;
    auto* hit = ui.find("editor.hit");
    hit->onTextInput(preedit);
    const auto committed = state.text;
    compose(true);
    check(state.text == committed && state.compositionText == "拼🙂",
          "borrowed value must not overwrite IME preedit or committed text");
    core::TextInputEvent insert;
    insert.text = "新🙂";
    hit = ui.find("editor.hit");
    hit->onTextInput(insert);
    check(document == state.text && state.compositionText.empty(),
          "IME commit callback must update the document after build");
    compose(true);
    auto* updatedHit = ui.find("editor.hit");
    core::KeyModifiers shortcut;
#if defined(__APPLE__)
    shortcut.super = true;
#else
    shortcut.control = true;
#endif
    updatedHit->onKeyEvent({core::InputKey::Z, core::KeyAction::Press, shortcut});
    check(document == committed && state.text == committed, "undo remains synchronized");
    compose(true);
    shortcut.shift = true;
    ui.find("editor.hit")->onKeyEvent({core::InputKey::Z, core::KeyAction::Press, shortcut});
    check(document == state.text && document != committed, "redo remains synchronized");
    compose(true);
    // Changing source ownership after build cannot invalidate stored callbacks.
    document.clear();
    document.shrink_to_fit();
    ui.find("editor.hit")->onTextInput(insert);
    check(document == state.text, "callbacks must operate on InputState, not borrowed text");
    compose(false); // Existing owning API interoperates with the same state.
    check(document == state.text, "owning API remains compatible");
    runtime.shutdown();
    return failures ? 1 : 0;
}
