#include "ui/mindmap_inline.h"
#include <cstdlib>
#include <functional>
#include <iostream>

namespace app { void requestUpdate() {} }
namespace neo {
void showToast(AppState&, std::string, std::string) {}
float effectiveWindowScale() { return 1.0f; }
}

namespace {
struct ScopedEnvironment {
    std::string name;
    std::string oldValue;
    bool hadValue = false;
    ScopedEnvironment(const char* key, const char* value) : name(key) {
        const char* previous = std::getenv(key);
        hadValue = previous != nullptr;
        if (hadValue) oldValue = previous;
#ifdef _WIN32
        _putenv_s(key, value);
#else
        setenv(key, value, 1);
#endif
    }
    ~ScopedEnvironment() {
#ifdef _WIN32
        _putenv_s(name.c_str(), hadValue ? oldValue.c_str() : "");
#else
        if (hadValue) setenv(name.c_str(), oldValue.c_str(), 1);
        else unsetenv(name.c_str());
#endif
    }
};
}

int main() {
    using namespace neo;
    using namespace mindmap_inline;
    using InputModel = components::input_detail::InputModel;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::cerr << label << '\n'; }
    };
    AppState state;
    state.newDocumentLanguage = "markdown";
    const std::string first = "```mermaid\nmindmap\n  root((First))\n    Child\n```";
    const std::string second = "```mermaid\nmindmap\n  root((Second))\n    Leaf\n```";
    state.doc.text = "before\n" + first + "\n\n" + second + "\n\n```mermaid\nmindmap\n  root((Open)\n";
    const int firstBeg = static_cast<int>(state.doc.text.find("```mermaid"));
    const int secondBeg = static_cast<int>(state.doc.text.find("```mermaid", firstBeg + 1));
    const int unclosedBeg = static_cast<int>(state.doc.text.find("```mermaid", secondBeg + 1));
    core::dsl::Ui ui;
    const std::string scope = "mindmap.inline.test";
    const std::string inputId = editorInputId(state);
    ui.begin(scope);
    auto& input = ui.state<InputModel::InputState>(inputId);
    input.text = state.doc.text;
    input.cursor = firstBeg + 16;
    input.selectionStart = input.cursor - 2;
    input.selectionEnd = input.cursor + 3;
    input.textRevision = 7;
    input.followCaret = true;
    lp::cachedPlan(state.doc.text);
    const auto originalText = input.text;
    const auto originalCursor = input.cursor;
    const auto originalSelectionStart = input.selectionStart;
    const auto originalSelectionEnd = input.selectionEnd;
    const auto originalTextRevision = input.textRevision;
    const auto originalUndoSize = input.undoStack.size();
    const auto originalDocRevision = state.revision;
    const auto originalSavedRevision = state.savedRevision;
    const auto originalDirty = state.dirty();

    // Composition is an immediate, non-destructive refusal.
    input.compositionText = "IME-preedit";
    const auto imeResult = openInlineMindmap(ui, state, firstBeg);
    check(imeResult == mindmap::RejectReason::PlanNotReady && input.compositionText == "IME-preedit" &&
          input.text == originalText && input.cursor == originalCursor && !state.mindmapPreview.open,
          "IME composition must be preserved and inline opening refused");
    input.compositionText.clear();

    const auto build = [&](const std::function<void()>& content) {
        ui.begin(scope);
        content();
        ui.end();
        ui.layout(1000.0f, 800.0f);
    };
    TextLine firstLine; firstLine.start = firstBeg; firstLine.lineStart = true;
    TextLine secondLine; secondLine.start = secondBeg; secondLine.lineStart = true;
    TextLine unclosedLine; unclosedLine.start = unclosedBeg; unclosedLine.lineStart = true;
    const Rect lineRect{10.0f, 20.0f, 900.0f, 24.0f};
    build([&] {
        drawInlineMindmapLine(ui, state, firstLine, lineRect);
        drawInlineMindmapLine(ui, state, secondLine, lineRect);
        drawInlineMindmapLine(ui, state, unclosedLine, lineRect);
    });
    check(ui.find("mindmapInline.toggle." + std::to_string(firstBeg)) != nullptr &&
          ui.find("mindmapInline.toggle." + std::to_string(secondBeg)) != nullptr,
          "Two closed Mermaid blocks must have distinct source buttons");
    check(ui.find("mindmapInline.toggle." + std::to_string(unclosedBeg)) == nullptr,
          "An unterminated Mermaid fence must not get a render button");

    const auto opened = openInlineMindmap(ui, state, firstBeg);
    check(opened == mindmap::RejectReason::None && state.mindmapPreview.open &&
          state.mindmapPreview.inlineMode, "Exact source block must open inline");
    check(input.text == originalText && input.cursor == originalCursor &&
          input.selectionStart == originalSelectionStart && input.selectionEnd == originalSelectionEnd &&
          input.textRevision == originalTextRevision && input.undoStack.size() == originalUndoSize &&
          state.revision == originalDocRevision && state.savedRevision == originalSavedRevision &&
          state.dirty() == originalDirty,
          "Opening inline preview must not edit text, selection, undo, dirty, or save state");

    build([&] { check(prepareInlineMindmap(ui, state, 900.0f), "Inline prepare must build the active layout"); });
    check(state.derivedMindmap && state.derivedMindmap->valid,
          "Prepared inline preview must own a valid layout");
    const int firstRow = lp::planCache().plan.lineIndexFor(firstBeg);
    const int finalRow = lp::planCache().plan.lineIndexFor(static_cast<int>(state.derivedMindmap->plan.sourceEnd) - 1);
    std::vector<LineDecoration> baseRows(lp::planCache().plan.lines.size());
    baseRows[static_cast<std::size_t>(finalRow)].hidden = true;
    baseRows[static_cast<std::size_t>(finalRow)].hiddenByFold = true;
    auto base = std::make_shared<const components::input_detail::LineDecorationTable>(std::move(baseRows));
    const auto decorated = inlineMindmapDecorations(state, base, kInlinePanelHeight);
    check(firstRow >= 0 && decorated &&
          (*decorated)[static_cast<std::size_t>(firstRow)].lineHeight >= kHeaderHeight + kCanvasHeight,
          "Host source row must have room for header and graph canvas");
    check(finalRow > firstRow && (*decorated)[static_cast<std::size_t>(firstRow)].holes.size() > 0 &&
          (*decorated)[static_cast<std::size_t>(firstRow + 1)].hiddenByView &&
          !(*decorated)[static_cast<std::size_t>(firstRow + 1)].hiddenByFold,
          "Inline source must be concealed with hiddenByView, not fold state");
    check((*decorated)[static_cast<std::size_t>(finalRow)].hiddenByFold &&
          !(*decorated)[static_cast<std::size_t>(finalRow)].hiddenByView,
          "An existing Fold-hidden row must keep its fold reason unchanged");

    build([&] {
        drawInlineMindmapLine(ui, state, firstLine, Rect{10.0f, 20.0f, 900.0f, kInlinePanelHeight});
        drawInlineMindmapLine(ui, state, secondLine, lineRect);
    });
    const auto* canvas = ui.find("mindmapInline.canvas");
    check(canvas && canvas->clip, "Active graph must have its own clipped canvas");
    check(ui.find("mindmapInline.node.0.fill") != nullptr,
          "Active inline canvas must draw graph nodes");
    check(ui.find("mindmapInline.toggle." + std::to_string(secondBeg)) != nullptr,
          "Non-active Mermaid block must retain its source toggle");

    // A style-width rebuild must keep the already-consumed viewer focus request consumed.
    input.cursor = 0;
    input.selectionStart = 0;
    input.selectionEnd = 0;
    check(!state.mindmapPreview.viewport.focusPending,
          "First inline draw must consume its pending viewer focus");
    build([&] { check(prepareInlineMindmap(ui, state, 920.0f),
                      "Width change must rebuild inline layout"); });
    check(state.mindmapPreview.open && !state.mindmapPreview.viewport.focusPending &&
          state.derivedMindmap->key.panelWidth == 920.0f,
          "Width rebuild must preserve focus state while refreshing geometry cache");
    build([&] { drawInlineMindmapLine(ui, state, firstLine,
                                      Rect{10.0f, 20.0f, 920.0f, kInlinePanelHeight}); });
    check(state.mindmapPreview.open && !state.mindmapPreview.viewport.focusPending,
          "Post-resize draw must not request focus from the inline viewer again");
    input.cursor = originalCursor;
    input.selectionStart = originalSelectionStart;
    input.selectionEnd = originalSelectionEnd;

    // A same-revision external rewrite is caught by the source anchor and closes before draw.
    std::string rewritten = state.doc.text;
    rewritten.replace(rewritten.find("First"), 5, "Other");
    state.doc.text = rewritten;
    input.text = rewritten;
    lp::cachedPlan(rewritten);
    build([&] { check(!prepareInlineMindmap(ui, state, 900.0f),
                      "Same-revision source rewrite must invalidate the active map"); });
    check(!state.mindmapPreview.open && input.text == rewritten,
          "External rewrite must fall back to editable source");

    // Force the existing cache-budget gate using a large Markdown page identity.
    const int currentFirstBeg = static_cast<int>(state.doc.text.find("```mermaid"));
    {
        ScopedEnvironment budget("NEO_MINDMAP_CACHE_MIB", "1");
        state.doc.text = originalText;
        input.text = originalText;
        lp::cachedPlan(originalText);
        state.path.assign(2 * 1024 * 1024, 'p');
        state.path += ".md";
        check(state.markdownCapable(), "Budget fixture path must remain Markdown");
        check(openInlineMindmap(ui, state, currentFirstBeg) == mindmap::RejectReason::None,
              "Budget refusal fixture must parse and open before layout");
        build([&] { check(!prepareInlineMindmap(ui, state, 900.0f),
                          "Oversized cache identity must be refused by the active budget"); });
        check(!state.mindmapPreview.open && input.text == originalText && state.derivedMindmap &&
              !state.derivedMindmap->valid,
              "Budget refusal must close to unchanged source text");
    }

    // Reopen a small map and verify the header's source route closes it and requests editor focus.
    state.path.clear();
    state.mindmapPreview.close();
    state.derivedMindmap.reset();
    state.derivedCacheBytes = 0;
    lp::cachedPlan(originalText);
    check(openInlineMindmap(ui, state, currentFirstBeg) == mindmap::RejectReason::None,
          "Small source fixture must reopen");
    build([&] { prepareInlineMindmap(ui, state, 900.0f); });
    build([&] { drawInlineMindmapLine(ui, state, firstLine, Rect{10.0f, 20.0f, 900.0f, kInlinePanelHeight}); });
    const auto* hit = ui.find(inputId + ".mindmap.inline.hit");
    check(hit != nullptr, "Active viewer must expose its focusable hit surface");
    if (hit) {
        core::PointerEvent sourceClick;
        sourceClick.button = core::PointerButton::Left;
        sourceClick.x = static_cast<int>(hit->frame.x + hit->frame.width - 130.0f);
        sourceClick.y = static_cast<int>(hit->frame.y + 16.0f);
        const core::Rect hitBounds{hit->frame.x, hit->frame.y, hit->frame.width, hit->frame.height};
        hit->onPress(sourceClick, hitBounds);
        core::KeyEvent saveKey;
        saveKey.key = core::InputKey::S;
        saveKey.modifiers.control = true;
        check(!hit->onKeyEvent(saveKey), "Ctrl+S must pass through the inline viewer");
        core::KeyEvent ordinaryKey;
        ordinaryKey.key = core::InputKey::A;
        check(hit->onKeyEvent(ordinaryKey), "Ordinary typing keys must be consumed by the viewer");
    }
    check(!state.mindmapPreview.open && !state.mindmapPreview.inlineMode &&
          input.text == originalText && input.cursor == originalCursor,
          "Source control must close the viewer without changing source caret");

    // Active composition while the viewer is open closes the view without modifying preedit.
    check(openInlineMindmap(ui, state, currentFirstBeg) == mindmap::RejectReason::None,
          "Viewer must reopen for IME guard");
    input.compositionText = "仍在输入";
    build([&] { check(!prepareInlineMindmap(ui, state, 900.0f),
                      "Composition must terminate inline mode"); });
    check(input.compositionText == "仍在输入" && !state.mindmapPreview.open,
          "IME close path must retain composition text");
    input.compositionText.clear();
    check(input.text == originalText && input.textRevision == originalTextRevision &&
          input.undoStack.size() == originalUndoSize && state.revision == originalDocRevision &&
          state.savedRevision == originalSavedRevision,
          "All viewer transitions must leave edit and save state untouched");
    return failures ? 1 : 0;
}
