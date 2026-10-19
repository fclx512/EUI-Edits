#include "ui/overlays.h"
#include "core/dsl_runtime.h"

#include <iostream>
#include <string>

// Keep the overlay test independent of settings IO and the application window.
namespace app { void requestUpdate() {} }
namespace neo::settings {
bool writeRecovery(const std::string&, const std::string&, const textfile::Document*) { return true; }
}
namespace neo::cleanai {
Result clean(const std::string& document, std::size_t begin, std::size_t end) {
    return {document, begin, end, false};
}
}

namespace {

using Model = components::input_detail::InputModel;

struct Fixture {
    core::dsl::Runtime runtime;
    core::dsl::Ui* ui = nullptr;
    neo::AppState state;
    int failures = 0;

    Fixture() {
        runtime.initialize();
        state.doc.text = "[link](old.md#title)";
        compose();
    }
    ~Fixture() { runtime.shutdown(); }

    void check(bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    }

    void compose(bool consumeEditorFocus = false) {
        runtime.compose("link-editor-focus", 800, 600,
            [&](core::dsl::Ui& view, const core::dsl::Screen& screen) {
                ui = &view;
                components::input(view, neo::editorInputId(state))
                    .position(0, 0).size(780, 450).multiline(true)
                    .valueRef(state.doc.text)
                    .onChange([this](const std::string& value) { state.doc.text = value; })
                    .build();
                // Same focus handoff used after editorView in app.cpp.
                if (consumeEditorFocus && state.findEditorFocusPending) {
                    state.findEditorFocusPending = false;
                    view.requestFocus(neo::editorInputId(state) + ".hit");
                }
                neo::linkEditorOverlay(view, state, screen);
            });
    }

    void open(std::string url = "old.md#title") {
        state.linkEditorUrl = std::move(url);
        state.pendingLinkBeg = 7;
        state.pendingLinkEnd = 19;
        state.pendingLinkUrl.clear();
        state.pendingEditorCommand = neo::EditorCommand::None;
        state.findEditorFocusPending = false;
        state.linkEditorOpen = true;
        compose();
    }

    void closeFrame() { compose(); }

    core::dsl::Element* element(const char* id) {
        return ui->find(id);
    }
    Model::InputState& modalInput() {
        return ui->state<Model::InputState>("editor.linkedit.input");
    }
    Model::InputState& editorInput() {
        return ui->state<Model::InputState>(neo::editorInputId(state));
    }
};

core::KeyModifiers shortcut() {
    core::KeyModifiers modifiers;
#if defined(__APPLE__)
    modifiers.super = true;
#else
    modifiers.control = true;
#endif
    return modifiers;
}

} // namespace

int main() {
    Fixture f;
    f.open();
    f.check(f.ui->isFocused("editor.linkedit.input.hit"),
            "opening link editor focuses URL input");
    auto& modal = f.modalInput();
    f.check(modal.text == "old.md#title" && modal.selectionStart == 0 &&
            modal.selectionEnd == static_cast<int>(modal.text.size()),
            "opening link editor selects entire URL");
    modal.cursor = modal.selectionStart = modal.selectionEnd = 4;
    f.compose();
    f.check(modal.cursor == 4 && modal.selectionStart == 4 && modal.selectionEnd == 4,
            "ordinary recomposition does not reset the user's URL caret");

    auto* hit = f.element("editor.linkedit.input.hit");
    f.check(hit && hit->onKeyEvent, "link input has key callback");
    if (hit && hit->onKeyEvent) {
        core::TextInputEvent typed;
        typed.text = "X";
        hit->onTextInput(typed);
        f.check(f.modalInput().text != "old.md#title", "test input creates modal edit history");
        modal.compositionText = "候选";
        hit->onKeyEvent({core::InputKey::Enter, core::KeyAction::Press, {}});
        f.check(f.state.linkEditorOpen && f.state.pendingEditorCommand == neo::EditorCommand::None,
                "Enter does not submit while IME composition owns the input");
        hit->onKeyEvent({core::InputKey::Escape, core::KeyAction::Press, {}});
        f.check(f.state.linkEditorOpen && f.state.pendingEditorCommand == neo::EditorCommand::None,
                "Escape does not close while IME composition owns the input");
        modal.compositionText.clear();
        hit->onKeyEvent({core::InputKey::Escape, core::KeyAction::Press, {}});
        f.check(!f.state.linkEditorOpen && f.state.findEditorFocusPending &&
                f.state.pendingLinkBeg == -1 && f.state.pendingLinkEnd == -1 &&
                f.state.pendingLinkUrl.empty(),
                "Escape cancels and clears link payload, then returns focus to editor");
    }
    f.compose(true);
    f.check(f.ui->isFocused(neo::editorInputId(f.state) + ".hit"),
            "cancel focus handoff reaches editor input");

    f.closeFrame();
    f.open("new.md#heading");
    auto* primary = f.element("editor.linkedit.primary.bg");
    f.check(primary && primary->onClick, "link editor confirm button exists");
    if (primary && primary->onClick) primary->onClick();
    f.check(!f.state.linkEditorOpen && f.state.findEditorFocusPending &&
            f.state.pendingEditorCommand == neo::EditorCommand::EditLink &&
            f.state.pendingLinkUrl == "new.md#heading",
            "confirm queues one document edit and requests editor focus");
    neo::applyEditorCommand(*f.ui, f.state);
    f.check(f.editorInput().text == "[link](new.md#heading)" &&
            f.state.doc.text == f.editorInput().text,
            "confirm applies URL replacement to document");
    f.compose(true);
    f.check(f.ui->isFocused(neo::editorInputId(f.state) + ".hit"),
            "confirm focus handoff reaches editor input");
    if (auto* editorHit = f.element((neo::editorInputId(f.state) + ".hit").c_str())) {
        editorHit->onKeyEvent({core::InputKey::Z, core::KeyAction::Press, shortcut()});
        f.check(f.editorInput().text == "[link](old.md#title)",
                "first Ctrl+Z after confirm undoes the link replacement");
        editorHit->onKeyEvent({core::InputKey::Z, core::KeyAction::Press, shortcut()});
        f.check(f.editorInput().text == "[link](old.md#title)",
                "link replacement contributes only one undo record");
    } else f.check(false, "editor input exists after focus handoff");

    // A fresh opening with the same URL must discard the modal input's own
    // selection and undo history, even if its value is unchanged.
    f.closeFrame();
    f.open("old.md#title");
    hit = f.element("editor.linkedit.input.hit");
    if (hit && hit->onTextInput) {
        core::TextInputEvent typed;
        typed.text = "X";
        hit->onTextInput(typed);
        f.check(f.modalInput().text != "old.md#title", "same-URL session creates modal undo history");
        f.check(Model::undoEdit(f.modalInput()) && f.modalInput().text == "old.md#title" &&
                !f.modalInput().redoStack.empty(), "restore identical modal value with history retained");
        f.state.linkEditorUrl = f.modalInput().text;
    }
    auto* cancelForReopen = f.element("editor.linkedit.secondary.bg");
    if (cancelForReopen && cancelForReopen->onClick) cancelForReopen->onClick();
    f.closeFrame();
    f.open("old.md#title");
    f.check(f.ui->isFocused("editor.linkedit.input.hit") &&
            f.modalInput().selectionStart == 0 &&
            f.modalInput().selectionEnd == static_cast<int>(f.modalInput().text.size()) &&
            f.modalInput().undoStack.empty() && f.modalInput().redoStack.empty(),
            "reopening same URL selects it and clears stale modal undo history");
    const auto beforeNoop = f.state.revision;
    primary = f.element("editor.linkedit.primary.bg");
    if (primary && primary->onClick) primary->onClick();
    neo::applyEditorCommand(*f.ui, f.state);
    f.check(f.state.revision == beforeNoop && f.editorInput().text == "[link](old.md#title)" &&
            !Model::undoEdit(f.editorInput()),
            "confirming unchanged URL adds no document revision or undo record");

    f.closeFrame();
    f.open();
    auto* secondary = f.element("editor.linkedit.secondary.bg");
    f.check(secondary && secondary->onClick, "link editor cancel button exists");
    if (secondary && secondary->onClick) secondary->onClick();
    f.check(!f.state.linkEditorOpen && f.state.findEditorFocusPending &&
            f.state.pendingLinkBeg == -1 && f.state.pendingLinkEnd == -1 &&
            f.state.pendingLinkUrl.empty() &&
            f.state.pendingEditorCommand == neo::EditorCommand::None,
            "cancel button clears payload and returns focus without editing");

    f.closeFrame();
    f.open();
    auto* backdrop = f.element("editor.linkedit.backdrop");
    f.check(backdrop && backdrop->onClick, "link editor backdrop exists");
    if (backdrop && backdrop->onClick) backdrop->onClick();
    f.check(!f.state.linkEditorOpen && f.state.findEditorFocusPending &&
            f.state.pendingLinkBeg == -1 && f.state.pendingLinkEnd == -1 &&
            f.state.pendingLinkUrl.empty(),
            "backdrop cancellation clears payload and returns focus");

    f.closeFrame();
    f.open("keyboard.md#heading");
    hit = f.element("editor.linkedit.input.hit");
    f.check(hit && hit->onKeyEvent, "reopened link input has key callback");
    if (hit && hit->onKeyEvent)
        hit->onKeyEvent({core::InputKey::Enter, core::KeyAction::Press, {}});
    f.check(!f.state.linkEditorOpen && f.state.findEditorFocusPending &&
            f.state.pendingEditorCommand == neo::EditorCommand::EditLink &&
            f.state.pendingLinkUrl == "keyboard.md#heading",
            "Enter confirms the URL and returns focus to editor");

    // Exercise the queued edit with UTF-8 before and inside the destination.
    const std::string original = "中文🙂 [标签](<笔记.md#旧标题> \"保留提示\")";
    const std::string oldUrl = "笔记.md#旧标题";
    const std::string newUrl = "另一份.md#新标题🙂";
    Model::loadDocument(f.editorInput(), original);
    f.state.doc.text = original;
    f.state.pendingLinkBeg = static_cast<int>(original.find(oldUrl));
    f.state.pendingLinkEnd = f.state.pendingLinkBeg + static_cast<int>(oldUrl.size());
    f.state.pendingLinkUrl = newUrl;
    f.state.pendingEditorCommand = neo::EditorCommand::EditLink;
    neo::applyEditorCommand(*f.ui, f.state);
    std::string expected = original;
    expected.replace(original.find(oldUrl), oldUrl.size(), newUrl);
    f.check(f.editorInput().text == expected && f.state.doc.text == expected,
            "queued Unicode link edit preserves label, brackets and title");
    f.check(f.editorInput().undoStack.size() == 1 && Model::undoEdit(f.editorInput()) &&
            f.editorInput().text == original, "Unicode link edit restores original source in one undo");

    // Cancellation and unchanged confirmation must preserve an existing redo
    // branch, not merely avoid adding an undo entry to an empty history.
    {
        auto& history = f;
        const std::string before = "[link](old.md#title)";
        const std::string changed = "[link](new.md#heading)";
        Model::loadDocument(history.editorInput(), before);
        history.state.doc.text = before;
        history.state.linkEditorOpen = false;
        history.state.pendingEditorCommand = neo::EditorCommand::None;
        history.closeFrame();
        history.open("new.md#heading");
        auto* confirm = history.element("editor.linkedit.primary.bg");
        history.check(confirm && confirm->onClick, "redo fixture has a real confirm action");
        if (confirm && confirm->onClick) confirm->onClick();
        neo::applyEditorCommand(*history.ui, history.state);
        history.compose(true);
        auto* editorHit = history.element((neo::editorInputId(history.state) + ".hit").c_str());
        history.check(editorHit && editorHit->onKeyEvent, "redo fixture has an editor keyboard callback");
        if (editorHit && editorHit->onKeyEvent)
            editorHit->onKeyEvent({core::InputKey::Z, core::KeyAction::Press, shortcut()});
        history.check(history.editorInput().text == before && history.state.doc.text == before &&
                      history.editorInput().undoStack.empty() && history.editorInput().redoStack.size() == 1,
                      "confirmed link edit leaves a single usable redo record after keyboard undo");
        history.closeFrame();
        history.open("old.md#title");
        const auto revisionBeforeNoop = history.state.revision;
        confirm = history.element("editor.linkedit.primary.bg");
        if (confirm && confirm->onClick) confirm->onClick();
        neo::applyEditorCommand(*history.ui, history.state);
        history.check(history.state.revision == revisionBeforeNoop && history.state.doc.text == before &&
                      history.editorInput().text == before && history.editorInput().redoStack.size() == 1,
                      "unchanged link confirmation preserves the existing redo branch and document revision");
        history.closeFrame();
        history.open("discarded.md#other");
        auto* cancel = history.element("editor.linkedit.secondary.bg");
        history.check(cancel && cancel->onClick, "redo fixture has a real cancel action");
        if (cancel && cancel->onClick) cancel->onClick();
        history.compose(true);
        history.check(history.ui->isFocused(neo::editorInputId(history.state) + ".hit") &&
                      history.state.pendingEditorCommand == neo::EditorCommand::None &&
                      history.editorInput().text == before && history.state.doc.text == before &&
                      history.editorInput().redoStack.size() == 1,
                      "canceling a different URL returns focus without discarding the document redo branch");
        editorHit = history.element((neo::editorInputId(history.state) + ".hit").c_str());
        if (editorHit && editorHit->onKeyEvent)
            editorHit->onKeyEvent({core::InputKey::Y, core::KeyAction::Press, shortcut()});
        history.check(history.editorInput().text == changed && history.state.doc.text == changed &&
                      history.editorInput().redoStack.empty() && history.editorInput().undoStack.size() == 1,
                      "first keyboard redo after unchanged confirmation and cancellation restores the actual link edit");
    }

    if (!f.failures) std::cout << "link editor focus, keyboard, cancel and undo contracts passed\n";
    return f.failures ? 1 : 0;
}
