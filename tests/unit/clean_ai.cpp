#include "components/input.h"
#include "model/clean_ai.h"
#include "state/app_state.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace neo::settings {
bool writeRecovery(const std::string&, const std::string&, const textfile::Document*) { return true; }
bool readRecovery(RecoverySnapshot&) { return false; }
} // namespace neo::settings

namespace app {
void requestUpdate() {}
} // namespace app


namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "[clean_ai] FAIL: " << message << '\n';
    }
}

void testKnownCitationFormsAndConservativeCounterexamples() {
    const std::string source =
        "A[cite_start] B [CITE: 1, 2] C [CiTe:1,2] D [cite:\t1 ,\t2  ] E 【1†source】 F 【1:3†title】 G 【oai_citation:1|title】";
    const auto cleaned = neo::cleanai::clean(source, 0, source.size());
    check(cleaned.changed, "recognized citation markers are removed");
    check(cleaned.text == "A B  C  D  E  F  G ", "only the documented citation spellings are removed");

    const std::string ordinary = "[1] [^1]\n[ref]: https://example.test\n[CITE:x] [cite: 1, x] [cite: ] [cite:\n1] 【abc†x】";
    const auto untouched = neo::cleanai::clean(ordinary, 0, ordinary.size());
    check(!untouched.changed && untouched.text == ordinary,
          "plain numeric refs, footnotes, link definitions, and malformed lookalikes survive");
}

void testInvisibleCharactersAndUnicodePreservation() {
    const std::string source = std::string("a") + "\xE2\x80\x8B" + "b" + "\xEF\xBB\xBF" +
        "c" + "\xC2\xA0" + "d" + "\xE2\x80\x8C" + "x" + "\xE2\x80\x8D" +
        "y" + "\xE2\x80\xAE" + "z";
    const std::string expected = std::string("abc d") + "\xE2\x80\x8C" + "x" + "\xE2\x80\x8D" +
        "y" + "\xE2\x80\xAE" + "z";
    const auto result = neo::cleanai::clean(source, 0, source.size());
    check(result.changed && result.text == expected,
          "remove only ZWSP/FEFF and convert NBSP; keep joiners and bidi controls");
}

void testMarkdownProtectionFromWholeDocumentContext() {
    const std::string source =
        "---\n[cite_start]\n---\n"
        "body [cite_start]\n"
        "- > ```text\n- > [cite_start]\n- > ```\n"
        "    [cite_start]\n"
        ">     [cite_start]\n"
        "-     [cite_start]\n"
        "inline `[cite_start]`\n"
        "[link](https://host.test/[cite_start]) ![image](assets/[cite_start].png)\n"
        "<div data-ref=\"[cite_start]\">\n[cite_start]\n\n"
        "<!--\n\n[cite_start]\n-->\n"
        "<![CDATA[\n\n[cite_start]\n]]>\n"
        "<?xml-stylesheet\n\n[cite_start]\n?>\n"
        "[[目标[cite_start]]] and [[id target]] and [reference][cite_start]\n"
        "[a](url \"[cite_start])\")\n"
        "<script>\nconst marker = '[cite_start]';\n\nstill code [cite_start]\n</script>\n"
        "tail [cite_start]";
    const std::string expected =
        "---\n[cite_start]\n---\n"
        "body \n"
        "- > ```text\n- > [cite_start]\n- > ```\n"
        "    [cite_start]\n"
        ">     [cite_start]\n"
        "-     [cite_start]\n"
        "inline `[cite_start]`\n"
        "[link](https://host.test/[cite_start]) ![image](assets/[cite_start].png)\n"
        "<div data-ref=\"[cite_start]\">\n[cite_start]\n\n"
        "<!--\n\n[cite_start]\n-->\n"
        "<![CDATA[\n\n[cite_start]\n]]>\n"
        "<?xml-stylesheet\n\n[cite_start]\n?>\n"
        "[[目标[cite_start]]] and [[id target]] and [reference][cite_start]\n"
        "[a](url \"[cite_start])\")\n"
        "<script>\nconst marker = '[cite_start]';\n\nstill code [cite_start]\n</script>\n"
        "tail ";
    const auto result = neo::cleanai::clean(source, 0, source.size());
    check(result.text == expected, "protected Markdown and HTML source stays byte-for-byte intact");

    const std::string selected = "prefix `[cite_start]` body [cite_start] suffix";
    const std::size_t codeMarker = selected.find("[cite_start]");
    const auto codeSelection = neo::cleanai::clean(selected, codeMarker, codeMarker + 12);
    check(!codeSelection.changed && codeSelection.text == selected,
          "a selection inside inline code is protected using full-document context");

    const std::size_t bodyMarker = selected.rfind("[cite_start]");
    const auto bodySelection = neo::cleanai::clean(selected, bodyMarker, bodyMarker + 12);
    check(bodySelection.changed && bodySelection.text == "prefix `[cite_start]` body  suffix",
          "only the selected body token changes when code context lies outside selection");

    const std::string malformedLink = "[broken](url [cite_start] tail [cite_start]";
    const auto malformedLinkResult = neo::cleanai::clean(malformedLink, 0, malformedLink.size());
    check(!malformedLinkResult.changed && malformedLinkResult.text == malformedLink,
          "an unclosed inline link target protects the ambiguous suffix");
}

void testSelectionOffsetsAndNoOp() {
    const std::string source = std::string("前 [cite_start] 中") + "\xC2\xA0" + "后";
    const std::size_t begin = source.find("[cite_start]");
    const std::size_t end = source.size();
    const auto result = neo::cleanai::clean(source, begin, end);
    check(result.text == std::string("前  中 后"), "selected content cleans without touching prefix");
    check(result.selectionBegin == begin && result.selectionEnd == result.text.size(),
          "selected result range accounts for removed markers and NBSP byte width");

    const std::string counterexample = "ordinary [1] with  two spaces  \n";
    const auto noOp = neo::cleanai::clean(counterexample, 0, counterexample.size());
    check(!noOp.changed && noOp.text == counterexample, "preserved formatting yields a true no-op");
}

void testMultilineReferenceDefinitions() {
    const std::string source =
        "[ref]:\nhttps://host.test/[cite_start]\n\"title [cite_end]\"\n\n"
        "[escaped\\]]: url/[cite_start]\n\"title [cite_end]\"\n\nbody [cite_start]";
    const auto cleaned = neo::cleanai::clean(source, 0, source.size());
    check(cleaned.changed && cleaned.text == source.substr(0, source.size() - 12),
          "multiline and escaped-label reference definitions survive while ordinary body cleans");
    const std::size_t marker = source.find("[cite_start]");
    const auto selected = neo::cleanai::clean(source, marker, marker + 12);
    check(!selected.changed && selected.text == source,
          "a selected multiline reference target is protected by whole-document context");
    const std::string followedByCode =
        "[ref]: url\n```\n[cite_start]\n\n[cite_start]\n```\n\nbody [cite_start]";
    const auto code = neo::cleanai::clean(followedByCode, 0, followedByCode.size());
    check(code.text == followedByCode.substr(0, followedByCode.size() - 12),
          "a fence following a definition still protects code beyond internal blank lines");
}

void testAdversarialUnclosedSyntaxRuns() {
    std::string source;
    for (int i = 0; i < 12000; ++i) source += "[unclosed reference\n";
    source.append(24000, '<');
    source += " [cite_start] ";
    for (int i = 1; i <= 2000; ++i) {
        source.append(static_cast<std::size_t>(i % 7 + 1), '`');
        source += "x";
    }
    const auto result = neo::cleanai::clean(source, 0, source.size());
    check(result.changed && result.text.find("[cite_start]") == std::string::npos,
          "long unmatched brackets, angle brackets, and mixed backtick runs remain safe to scan");
}

void reportDocumentSizeTimings() {
    for (const std::size_t bytes : {1024u * 1024u, 8u * 1024u * 1024u}) {
        std::string source;
        source.reserve(bytes);
        while (source.size() < bytes) source += "普通正文 [cite_start] 连接词和文本\n";
        using InputModel = components::input_detail::InputModel;
        source.resize(static_cast<std::size_t>(InputModel::clampUtf8Boundary(source, static_cast<int>(bytes))));
        source.append(bytes - source.size(), ' ');
        const auto begin = std::chrono::steady_clock::now();
        const auto result = neo::cleanai::clean(source, 0, source.size());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();
        check(result.changed, "large-document timing sample includes removable markers");
        std::cout << "[clean_ai] " << (bytes / (1024u * 1024u)) << " MiB: " << elapsed << " ms\n";
    }
}

void testActualCommandUndoRedoAndNoOpHistory() {
    using InputModel = components::input_detail::InputModel;
    eui::Ui ui;
    neo::AppState app;
    app.lastRecoveryWrite = std::chrono::steady_clock::now();
    auto& input = ui.state<InputModel::InputState>(neo::kEditorInputId);
    InputModel::loadDocument(input, "A[cite_start] [1]");
    app.doc.text = input.text;
    app.foldedHeadings.insert(42);
    const unsigned long long initialRevision = app.revision;
    const unsigned long long initialDecorationRevision = input.decorationRevision;

    app.pendingEditorCommand = neo::EditorCommand::CleanAi;
    neo::applyEditorCommand(ui, app);
    check(input.text == "A [1]" && app.doc.text == input.text, "queued command changes editor and document text");
    check(input.undoStack.size() == 1, "cleanup is one undo record");
    check(input.selectionStart == 0 && input.selectionEnd == 5 && input.cursor == 5,
          "whole-document cleanup selects its result and leaves caret at the end");
    check(app.revision == initialRevision + 1, "one cleanup advances app revision once");
    check(app.foldedHeadings.empty() && input.decorationRevision == initialDecorationRevision + 1,
          "byte-offset fold keys are cleared and decorations invalidated");

    app.pendingEditorCommand = neo::EditorCommand::Undo;
    neo::applyEditorCommand(ui, app);
    check(input.text == "A[cite_start] [1]" && app.doc.text == input.text, "undo restores original text");
    check(input.selectionStart == 0 && input.selectionEnd == 0 && input.cursor == 0,
          "undo restores the original caret and selection");
    app.pendingEditorCommand = neo::EditorCommand::Redo;
    neo::applyEditorCommand(ui, app);
    check(input.text == "A [1]" && app.doc.text == input.text, "redo reapplies cleanup");

    InputModel::loadDocument(input, "plain [1]");
    app.doc.text = input.text;
    const unsigned long long beforeNoop = app.revision;
    app.pendingEditorCommand = neo::EditorCommand::CleanAi;
    neo::applyEditorCommand(ui, app);
    check(input.undoStack.empty() && app.revision == beforeNoop,
          "no-op command adds no history entry and advances no revision");

    InputModel::loadDocument(input, "left [cite_start] right [cite_start]");
    app.doc.text = input.text;
    input.selectionStart = 17;
    input.selectionEnd = 5;
    input.cursor = 5;
    app.pendingEditorCommand = neo::EditorCommand::CleanAi;
    neo::applyEditorCommand(ui, app);
    check(input.text == "left  right [cite_start]" && app.doc.text == input.text,
          "selection command changes only its selected citation");
    app.pendingEditorCommand = neo::EditorCommand::Undo;
    neo::applyEditorCommand(ui, app);
    check(input.text == "left [cite_start] right [cite_start]" &&
          input.selectionStart == 17 && input.selectionEnd == 5 && input.cursor == 5,
          "undo restores selected bytes and the original selection direction");

    InputModel::loadDocument(input, "A[cite_start]");
    app.doc.text = input.text;
    input.compositionText = "字";
    app.pendingEditorCommand = neo::EditorCommand::CleanAi;
    neo::applyEditorCommand(ui, app);
    check(app.pendingEditorCommand == neo::EditorCommand::CleanAi && input.text == "A[cite_start]" &&
          input.undoStack.empty(), "cleanup stays queued while IME composition is active");
    input.compositionText.clear();
    neo::applyEditorCommand(ui, app);
    check(input.text == "A" && app.doc.text == "A" && app.pendingEditorCommand == neo::EditorCommand::None,
          "queued cleanup applies once composition ends");
}

} // namespace

int main() {
    testKnownCitationFormsAndConservativeCounterexamples();
    testInvisibleCharactersAndUnicodePreservation();
    testMarkdownProtectionFromWholeDocumentContext();
    testSelectionOffsetsAndNoOp();
    testMultilineReferenceDefinitions();
    testAdversarialUnclosedSyntaxRuns();
    testActualCommandUndoRedoAndNoOpHistory();
    reportDocumentSizeTimings();
    if (failures != 0) {
        std::cerr << failures << " clean_ai test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "clean_ai tests passed\n";
    return EXIT_SUCCESS;
}
