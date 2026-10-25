#include "components/input_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
using Model = components::input_detail::InputModel;
using State = Model::InputState;
using Layout = Model::InputLayout;
using Decoration = components::input_detail::LineDecoration;
using LineRun = components::input_detail::LineRun;
using Step = Model::WidthReflowStep;

constexpr float kHeight = 76.0f; // A deliberately short viewport for cross-viewport selections.
constexpr float kInset = 12.0f;
constexpr float kFontSize = 16.0f;
constexpr float kLineHeight = 19.2f;
constexpr const char* kFont = "monospace";
int failures = 0;

void check(bool ok, const std::string& why) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << why << '\n';
    }
}

bool near(float a, float b) { return std::fabs(a - b) <= 0.02f; }

Layout build(State& state, float width, const std::vector<Decoration>& decorations,
             bool allowPendingWidth = false, const std::string& font = kFont,
             float fontSize = kFontSize) {
    return Layout::build(state, width, kHeight, width + 2 * kInset, kInset, kInset,
                         12.0f, kLineHeight, font, fontSize, true,
                         &decorations, {}, nullptr, allowPendingWidth);
}

struct Fixture {
    std::string text;
    std::vector<Decoration> decorations;
    int tableId = 91;
    int middle = 0;

    Fixture() {
        text = "# **中文🙂** ";
        for (int i = 0; i < 72; ++i) text += "alpha中🙂beta `code` ";
        text += "\nshort beginning\n";
        for (int i = 0; i < 14; ++i) text += "ordinary paragraph " + std::to_string(i) + " with enough words to wrap\n";
        middle = static_cast<int>(text.size());
        appendTableRow("甲🙂", "a long table cell with CJK 中文 and emoji 🙂 that must wrap across the viewport");
        text += '\n';
        appendTableRow("second left column", "another long right cell 中文🙂 with a different intrinsic width");
        text += "\ntail paragraph after table";

        int begin = 0;
        while (begin <= static_cast<int>(text.size())) {
            const auto newline = text.find('\n', static_cast<std::size_t>(begin));
            const int end = newline == std::string::npos ? static_cast<int>(text.size()) : static_cast<int>(newline);
            Decoration row;
            if (begin == 0) {
                row.holes = {{0, 2}, {2, 4}, {14, 16}}; // heading and bold markers, UTF-8 byte offsets
                row.fontSize = 20.0f;
                row.lineHeight = 26.0f;
                LineRun strong;
                strong.beg = 4;
                strong.end = 14;
                strong.style.edit().weight = 700;
                row.runs.push_back(strong);
                std::size_t cursor = text.find("code", 16);
                while (cursor != std::string::npos && cursor + 4 <= static_cast<std::size_t>(end)) {
                    LineRun code;
                    code.beg = static_cast<int>(cursor);
                    code.end = static_cast<int>(cursor + 4);
                    code.style.edit().fontFamily = "monospace";
                    row.runs.push_back(code);
                    cursor = text.find("code", cursor + 4);
                }
            }
            if (begin == middle || (begin > middle && text[static_cast<std::size_t>(begin)] == '|')) {
                row.tableId = tableId;
                row.cellPadding = 5.0f;
                const int first = begin + 2;
                const int separator = static_cast<int>(text.find(" | ", static_cast<std::size_t>(first)));
                const int second = separator + 3;
                row.cells = {{first, separator}, {second, end - 2}};
                row.holes = {{begin, first}, {separator, second}, {end - 2, end}};
            }
            decorations.push_back(std::move(row));
            if (newline == std::string::npos) break;
            begin = end + 1;
        }
    }

    void appendTableRow(const std::string& a, const std::string& b) {
        text += "| " + a + " | " + b + " |";
    }
};

State makeState(const std::string& text) {
    State state;
    Model::loadDocument(state, text);
    state.followCaret = false;
    state.wordWrap = true;
    state.cursor = 8;
    state.selectionStart = 8;
    state.selectionEnd = static_cast<int>(text.size()) - 4;
    return state;
}

void compareLayout(State& actualState, State& oracleState,
                   const Layout& actual, const Layout& oracle,
                   const std::string& label, bool compareSelection = true) {
    const auto& a = actual.lineList();
    const auto& b = oracle.lineList();
    check(a.size() == b.size(), label + ": visual row count");
    check(actual.geometryTable().count() == oracle.geometryTable().count(), label + ": geometry row count");
    check(near(actual.geometryTable().total(), oracle.geometryTable().total()), label + ": document height");
    check(near(actual.contentHeight, oracle.contentHeight), label + ": scroll content height");
    check(near(actual.maxVerticalScroll, oracle.maxVerticalScroll), label + ": maximum scroll");
    check(near(actual.inset, oracle.inset) && near(actual.rightInset, oracle.rightInset) &&
          near(actual.textTop, oracle.textTop), label + ": inset geometry");
    if (compareSelection)
        check(actual.selectionStart == oracle.selectionStart && actual.selectionEnd == oracle.selectionEnd &&
              near(actual.clippedSelectionX, oracle.clippedSelectionX) &&
              near(actual.clippedSelectionWidth, oracle.clippedSelectionWidth),
              label + ": selection input geometry");
    check(actual.cursorLine == oracle.cursorLine && near(actual.cursorX, oracle.cursorX) &&
          near(actual.cursorY, oracle.cursorY), label + ": caret row and position");
    if (a.size() != b.size()) return;

    for (std::size_t i = 0; i < a.size(); ++i) {
        Model::ensureLineDetails(*actual.owner, static_cast<int>(i));
        Model::ensureLineDetails(*oracle.owner, static_cast<int>(i));
        const auto& x = a[i];
        const auto& y = b[i];
        const std::string at = label + ": row " + std::to_string(i);
        check(x.start == y.start && x.end == y.end && x.lineNumber == y.lineNumber &&
              x.lineStart == y.lineStart && x.hardBreakAfter == y.hardBreakAfter &&
              x.hidden == y.hidden && x.hiddenByFold == y.hiddenByFold &&
              x.tableId == y.tableId && x.tableSeparator == y.tableSeparator,
              at + " source bytes and flags");
        check(x.presentation().holes == y.presentation().holes, at + " hidden byte ranges");
        check(components::input_detail::projectText(actualState.text, x.start, x.end, x.presentation().holes) ==
              components::input_detail::projectText(oracleState.text, y.start, y.end, y.presentation().holes),
              at + " visible bytes");
        check(near(x.metrics.width, y.metrics.width) && near(x.fontSize, y.fontSize) &&
              near(x.lineHeight, y.lineHeight) && near(x.top, y.top) &&
              near(x.contentIndent, y.contentIndent) && near(x.textShiftY, y.textShiftY),
              at + " measured row");
        check(x.metrics.byteIndices() == y.metrics.byteIndices(), at + " caret byte indices");
        check(x.metrics.caretX().size() == y.metrics.caretX().size(), at + " caret stop count");
        for (std::size_t j = 0; j < std::min(x.metrics.caretX().size(), y.metrics.caretX().size()); ++j)
            check(near(x.metrics.caretX()[j], y.metrics.caretX()[j]), at + " caret x " + std::to_string(j));
        const auto& xr = x.presentation().runs;
        const auto& yr = y.presentation().runs;
        check(xr.size() == yr.size(), at + " style run count");
        for (std::size_t j = 0; j < std::min(xr.size(), yr.size()); ++j)
            check(xr[j].beg == yr[j].beg && xr[j].end == yr[j].end &&
                  near(xr[j].x, yr[j].x) && near(xr[j].width, yr[j].width) &&
                  xr[j].style == yr[j].style, at + " style run " + std::to_string(j));
        check(x.rare().tableDocCaretStops.size() == y.rare().tableDocCaretStops.size(), at + " table caret count");
        for (std::size_t j = 0; j < std::min(x.rare().tableDocCaretStops.size(), y.rare().tableDocCaretStops.size()); ++j) {
            const auto& u = x.rare().tableDocCaretStops[j];
            const auto& v = y.rare().tableDocCaretStops[j];
            check(u.byteIndex == v.byteIndex && u.column == v.column && near(u.x, v.x), at + " table caret stop");
        }
        check(x.rare().tableCellRanges.size() == y.rare().tableCellRanges.size() &&
              near(x.rare().textBandHeight, y.rare().textBandHeight),
              at + " table cell ranges and text band");
        for (std::size_t j = 0; j < std::min(x.rare().tableCellRanges.size(), y.rare().tableCellRanges.size()); ++j) {
            const auto& u = x.rare().tableCellRanges[j];
            const auto& v = y.rare().tableCellRanges[j];
            check(u.beg == v.beg && u.end == v.end && u.align == v.align,
                  at + " table cell source interval");
        }
        check(near(actual.geometryTable().top(static_cast<int>(i)), oracle.geometryTable().top(static_cast<int>(i))) &&
              near(actual.geometryTable().height(static_cast<int>(i)), oracle.geometryTable().height(static_cast<int>(i))),
              at + " geometry prefix");
    }
    for (int t : {91}) {
        const auto* x = actual.tableColumnsFor(t);
        const auto* y = oracle.tableColumnsFor(t);
        check((x != nullptr) == (y != nullptr), label + ": table presence");
        if (!x || !y) continue;
        check(x->x.size() == y->x.size() && x->width.size() == y->width.size(), label + ": table column count");
        for (std::size_t i = 0; i < std::min(x->x.size(), y->x.size()); ++i)
            check(near(x->x[i], y->x[i]) && near(x->width[i], y->width[i]), label + ": table column geometry");
        check(near(x->padding, y->padding) && near(x->total, y->total), label + ": table bounds");
    }
    if (compareSelection)
        check(actual.selectionRects.size() == oracle.selectionRects.size(), label + ": selection rectangle count");
    for (std::size_t i = 0; compareSelection && i < std::min(actual.selectionRects.size(), oracle.selectionRects.size()); ++i) {
        const auto& x = actual.selectionRects[i];
        const auto& y = oracle.selectionRects[i];
        check(near(x.x, y.x) && near(x.y, y.y) && near(x.width, y.width) &&
              near(x.height, y.height) && near(x.lineHeight, y.lineHeight),
              label + ": selection rect " + std::to_string(i));
    }
}

Step drain(State& state, const std::string& label) {
    Step result = Step::Idle;
    for (int i = 0; i < 100000 && Model::widthReflowPending(state); ++i) {
        result = Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1);
        if (result == Step::Published || result == Step::Cancelled) break;
    }
    check(!Model::widthReflowPending(state), label + ": bounded steps eventually settle");
    return result;
}

void testBudgetAndFullOracle() {
    Fixture fixture;
    State state = makeState(fixture.text);
    State oldOracle = makeState(fixture.text);
    State newOracle = makeState(fixture.text);
    build(state, 320.0f, fixture.decorations);
    const auto oldLayout = build(oldOracle, 320.0f, fixture.decorations);
    const auto expected = build(newOracle, 170.0f, fixture.decorations);
    check(Model::requestWidthReflow(state, kFont, kFontSize, 170.0f, &fixture.decorations), "width change schedules draft");
    check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 0) == Step::Yield,
          "zero operations yield without publication");
    check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) == Step::Yield,
          "one operation cannot publish long styled row and table");
    for (int i = 0; i < 4; ++i) {
        check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) == Step::Yield,
              "long styled source row yields at a piece boundary");
        check(state.widthReflow && state.widthReflow->nextSourceLine == 0,
              "one-operation budget must not consume the whole long styled source row");
    }
    check(Model::widthReflowPending(state), "draft remains pending after one operation");
    auto oldFrame = build(state, 170.0f, fixture.decorations, true);
    check(near(state.cachedViewportWidth, 320.0f), "unfinished draft does not replace published width");
    compareLayout(state, oldOracle, oldFrame, oldLayout, "unfinished old generation", false);
    check(drain(state, "styled/table draft") == Step::Published, "complete draft publishes once");
    auto published = build(state, 170.0f, fixture.decorations, true);
    compareLayout(state, newOracle, published, expected, "published generation");
    for (auto direction : {0, 1}) {
        state.selectionStart = direction ? static_cast<int>(fixture.text.size()) - 4 : 8;
        state.selectionEnd = direction ? 8 : static_cast<int>(fixture.text.size()) - 4;
        newOracle.selectionStart = state.selectionStart;
        newOracle.selectionEnd = state.selectionEnd;
        state.verticalScroll = newOracle.verticalScroll = 90.0f;
        auto actual = build(state, 170.0f, fixture.decorations, true);
        auto oracle = build(newOracle, 170.0f, fixture.decorations);
        compareLayout(state, newOracle, actual, oracle, direction ? "reverse cross viewport" : "forward cross viewport");
    }
}

void testLatestRequestAndCancellation() {
    Fixture fixture;
    State state = makeState(fixture.text);
    build(state, 320.0f, fixture.decorations);
    check(Model::requestWidthReflow(state, kFont, kFontSize, 210.0f, &fixture.decorations), "A to B pending");
    Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1);
    check(Model::requestWidthReflow(state, kFont, kFontSize, 160.0f, &fixture.decorations), "B superseded by C");
    check(drain(state, "A-B-C") == Step::Published, "C publishes after B cancellation");
    check(near(state.cachedViewportWidth, 160.0f), "B never publishes over C");
    State oracle = makeState(fixture.text);
    auto expected = build(oracle, 160.0f, fixture.decorations);
    auto actual = build(state, 160.0f, fixture.decorations, true);
    compareLayout(state, oracle, actual, expected, "A-B-C latest width");

    check(Model::requestWidthReflow(state, kFont, kFontSize, 210.0f, &fixture.decorations), "A to B again");
    Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1);
    check(!Model::requestWidthReflow(state, kFont, kFontSize, 160.0f, &fixture.decorations),
          "A-B-A reuses only already complete A");
    check(!Model::widthReflowPending(state) && near(state.cachedViewportWidth, 160.0f),
          "A-B-A cancels obsolete B without changing published A");
    check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) != Step::Published,
          "cancelled B cannot publish on a late step");
}

void testInvalidationsAndTeardown() {
    Fixture fixture;
    State state = makeState(fixture.text);
    build(state, 320.0f, fixture.decorations);
    auto start = [&](const std::string& stage) {
        check(Model::requestWidthReflow(state, kFont, kFontSize, 170.0f, &fixture.decorations),
              stage + ": invalidation fixture starts a draft");
        check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) == Step::Yield,
              stage + ": invalidation fixture has an unfinished draft");
    };
    start("external write");
    // External write that accidentally reuses the revision must still invalidate by bytes.
    state.text += "\nexternal same-revision row 中文🙂 with a visible new wrap";
    check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) != Step::Published,
          "same-revision external text write cannot publish old draft");
    Model::cancelWidthReflow(state);
    State externalOracle = makeState(state.text);
    externalOracle.cursor = state.cursor;
    externalOracle.selectionStart = state.selectionStart;
    externalOracle.selectionEnd = state.selectionEnd;
    auto recoveredOld = build(state, 320.0f, fixture.decorations);
    auto expectedOld = build(externalOracle, 320.0f, fixture.decorations);
    compareLayout(state, externalOracle, recoveredOld, expectedOld,
                  "same-revision external write old width");
    auto recoveredNew = build(state, 170.0f, fixture.decorations);
    auto expectedNew = build(externalOracle, 170.0f, fixture.decorations);
    compareLayout(state, externalOracle, recoveredNew, expectedNew,
                  "same-revision external write new width");
    build(state, 320.0f, fixture.decorations);

    start("decoration");
    fixture.decorations[0].lineHeight += 3.0f;
    build(state, 320.0f, fixture.decorations);
    check(!Model::widthReflowPending(state), "decoration change cancels draft");

    start("font family");
    build(state, 320.0f, fixture.decorations, false, "Microsoft YaHei");
    check(!Model::widthReflowPending(state), "font change cancels draft");

    build(state, 320.0f, fixture.decorations);
    start("DPI");
    const float previousScale = core::TextPrimitive::layoutPixelScale();
    core::TextPrimitive::setLayoutPixelScale(1.25f);
    build(state, 320.0f, fixture.decorations);
    check(!Model::widthReflowPending(state), "DPI change cancels draft");
    core::TextPrimitive::setLayoutPixelScale(previousScale);

    build(state, 320.0f, fixture.decorations);
    start("wrap mode");
    state.wordWrap = false;
    build(state, 320.0f, fixture.decorations);
    check(!Model::widthReflowPending(state), "wrap mode change cancels draft");
    state.wordWrap = true;

    build(state, 320.0f, fixture.decorations);
    start("undo");
    state.cursor = 12;
    Model::insertAtCursor(state, "inserted🙂");
    check(Model::undoEdit(state), "undo fixture has an edit to revert");
    build(state, 320.0f, fixture.decorations);
    check(!Model::widthReflowPending(state), "undo invalidates draft even when text returns to A");

    start("IME cancellation");
    state.compositionText = "候选🙂";
    ++state.compositionRevision;
    Model::displayState(state, true);
    state.compositionText.clear();
    ++state.compositionRevision;
    Model::displayState(state, false);
    build(state, 320.0f, fixture.decorations);
    check(!Model::widthReflowPending(state), "IME preedit cancellation invalidates draft");

    start("document replacement");
    Model::loadDocument(state, "new page🙂\nshort text");
    check(!Model::widthReflowPending(state) ||
          Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) != Step::Published,
          "document replacement cannot publish old draft");
    Model::cancelWidthReflow(state);
    check(!Model::widthReflowPending(state), "explicit cancellation releases draft");

    // The job's private state must be released safely when its InputState is destroyed.
    {
        State shortLived = makeState(fixture.text);
        build(shortLived, 320.0f, fixture.decorations);
        Model::requestWidthReflow(shortLived, kFont, kFontSize, 170.0f, &fixture.decorations);
        Model::stepWidthReflow(shortLived, std::chrono::steady_clock::time_point::max(), 1);
    }
}

void testOldLayoutOwnerSurvivesPublicationAndStateDestruction() {
    Fixture fixture;
    auto state = std::make_unique<State>(makeState(fixture.text));
    build(*state, 320.0f, fixture.decorations);
    auto oldFrame = build(*state, 320.0f, fixture.decorations, true);
    check(oldFrame.callbackOwner && oldFrame.owner == oldFrame.callbackOwner.get(),
          "callback frame owns an independent published generation");
    const std::size_t oldRows = oldFrame.lineList().size();
    const float oldHeight = oldFrame.geometryTable().total();
    const float oldCaretX = oldFrame.xFor(8);
    const core::Rect bounds{10.0f, 20.0f, 344.0f, 120.0f};
    const auto oldHit = oldFrame.pointerHit(bounds.x + kInset + oldCaretX,
                                          bounds.y + 12.0f, bounds, 344.0f, kInset);

    check(Model::requestWidthReflow(*state, kFont, kFontSize, 170.0f, &fixture.decorations),
          "old frame fixture starts new generation");
    check(drain(*state, "old layout owner") == Step::Published, "new generation publishes");
    check(oldFrame.lineList().size() == oldRows && near(oldFrame.geometryTable().total(), oldHeight) &&
          near(oldFrame.xFor(8), oldCaretX), "old frame remains stable after publication");
    state.reset();
    check(oldFrame.lineList().size() == oldRows && near(oldFrame.geometryTable().total(), oldHeight),
          "old frame geometry survives InputState destruction");
    const auto after = oldFrame.pointerHit(bounds.x + kInset + oldCaretX,
                                           bounds.y + 12.0f, bounds, 344.0f, kInset);
    check(after.byteIndex == oldHit.byteIndex && after.lineIndex == oldHit.lineIndex,
          "old frame hit mapping survives InputState destruction");
    const float previousScale = core::TextPrimitive::layoutPixelScale();
    core::TextPrimitive::setLayoutPixelScale(previousScale == 1.0f ? 1.25f : 1.0f);
    check(!oldFrame.contextCurrent() && oldFrame.pointerHit(bounds.x + kInset + oldCaretX,
          bounds.y + 12.0f, bounds, 344.0f, kInset).byteIndex == 0,
          "retired callback frame rejects hit remeasurement under a new DPI context");
    core::TextPrimitive::setLayoutPixelScale(previousScale);
    const std::string fontA = "C:/Windows/Fonts/msyh.ttc";
    const std::string fontB = "C:/Windows/Fonts/consola.ttf";
    if (std::filesystem::exists(fontA) && std::filesystem::exists(fontB)) {
        const auto previousGeneration = core::TextPrimitive::fontConfigurationGeneration();
        core::TextPrimitive::setDefaultFontFiles(fontA, {});
        if (core::TextPrimitive::fontConfigurationGeneration() == previousGeneration)
            core::TextPrimitive::setDefaultFontFiles(fontB, {});
        check(!oldFrame.contextCurrent() && oldFrame.pointerHit(bounds.x + kInset + oldCaretX,
              bounds.y + 12.0f, bounds, 344.0f, kInset).byteIndex == 0,
              "retired callback frame rejects hit remeasurement under a new font generation");
        core::TextPrimitive::setDefaultFontFiles("", "");
    }
}

void testDefaultFontConfigurationChange() {
    const std::string fontA = "C:/Windows/Fonts/msyh.ttc";
    const std::string fontB = "C:/Windows/Fonts/consola.ttf";
    check(std::filesystem::exists(fontA) && std::filesystem::exists(fontB),
          "font generation fixture requires two real Windows fonts");
    if (!std::filesystem::exists(fontA) || !std::filesystem::exists(fontB)) return;
    const std::vector<Decoration> plain;
    std::string text = "iiii WWWW 中文🙂 123456 repeated words for the default font\n";
    for (int i = 0; i < 48; ++i) text += "iiii WWWW 123456 中文🙂 another paragraph\n";
    core::TextPrimitive::setDefaultFontFiles(fontA, {});
    const auto generationA = core::TextPrimitive::fontConfigurationGeneration();
    const float widthA = core::TextPrimitive::measureTextMetrics("iiii WWWW 123456", "", 16.0f).width;
    State state = makeState(text);
    build(state, 320.0f, plain, false, "");
    check(Model::requestWidthReflow(state, "", 16.0f, 170.0f),
          "default-font generation fixture starts width draft");
    Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1);
    core::TextPrimitive::setDefaultFontFiles(fontB, {});
    const auto generationB = core::TextPrimitive::fontConfigurationGeneration();
    const float widthB = core::TextPrimitive::measureTextMetrics("iiii WWWW 123456", "", 16.0f).width;
    check(generationB != generationA && !near(widthA, widthB),
          "same family name resolves changed default font metrics");
    check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) == Step::Cancelled,
          "font file switch cancels draft despite unchanged family string");
    State oracle = makeState(text);
    auto expected = build(oracle, 170.0f, plain, false, "");
    auto actual = build(state, 170.0f, plain, false, "");
    compareLayout(state, oracle, actual, expected, "default font switch synchronous oracle");
    core::TextPrimitive::setDefaultFontFiles("", "");
}

void testCompactStyledCursorOracle() {
    Fixture fixture;
    while (fixture.text.size() < 256u * 1024u + 1024u) {
        fixture.text += '\n';
        fixture.text += std::string(128, 'x');
        fixture.decorations.emplace_back();
    }
    State state = makeState(fixture.text);
    State oracle = makeState(fixture.text);
    state.viewportMetrics = oracle.viewportMetrics = true;
    build(state, 320.0f, fixture.decorations);
    check(state.cachedViewportMetrics, "large fixture activates compact row details");
    check(Model::requestWidthReflow(state, kFont, kFontSize, 170.0f, &fixture.decorations),
          "compact fixture starts resumable styled width draft");
    check(drain(state, "compact styled draft") == Step::Published,
          "compact styled draft publishes after bounded operations");
    build(oracle, 170.0f, fixture.decorations);
    check(state.cachedLines.size() == oracle.cachedLines.size() &&
          near(state.cachedGeometry.total(), oracle.cachedGeometry.total()),
          "compact draft matches synchronous visual row count and total height");
    if (state.cachedLines.size() != oracle.cachedLines.size()) return;
    const std::size_t prefix = std::min<std::size_t>(state.cachedLines.size(), 24);
    std::vector<std::size_t> samples;
    for (std::size_t i = 0; i < prefix; ++i) samples.push_back(i);
    if (state.cachedLines.size() > prefix) samples.push_back(state.cachedLines.size() / 2);
    for (std::size_t i : samples) {
        Model::ensureLineDetails(state, static_cast<int>(i));
        Model::ensureLineDetails(oracle, static_cast<int>(i));
        const auto& a = state.cachedLines[i];
        const auto& b = oracle.cachedLines[i];
        const std::string at = "compact oracle row " + std::to_string(i);
        check(a.start == b.start && a.end == b.end && a.lineNumber == b.lineNumber &&
              a.presentation().holes == b.presentation().holes && near(a.metrics.width, b.metrics.width),
              at + " source/visible boundaries and measured width");
        check(a.metrics.byteIndices() == b.metrics.byteIndices() &&
              a.metrics.caretX().size() == b.metrics.caretX().size(), at + " restored caret bytes");
        for (std::size_t j = 0; j < std::min(a.metrics.caretX().size(), b.metrics.caretX().size()); ++j)
            check(near(a.metrics.caretX()[j], b.metrics.caretX()[j]), at + " restored caret x");
        check(near(state.cachedGeometry.top(static_cast<int>(i)), oracle.cachedGeometry.top(static_cast<int>(i))) &&
              near(state.cachedGeometry.height(static_cast<int>(i)), oracle.cachedGeometry.height(static_cast<int>(i))),
              at + " geometry prefix");
    }
}

void testVeryLongPlainSourceRow() {
    std::string text;
    text.reserve(100000);
    for (int i = 0; i < 12500; ++i) text += "a中🙂"; // 100,000 UTF-8 bytes in one source row.
    const std::vector<Decoration> plain;
    State state = makeState(text);
    State oracle = makeState(text);
    build(state, 320.0f, plain);
    check(Model::requestWidthReflow(state, kFont, kFontSize, 170.0f),
          "100k plain row starts a width draft");
    for (int i = 0; i < 4; ++i) {
        check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) == Step::Yield,
              "100k plain row yields under a one-operation budget");
        check(state.widthReflow && state.widthReflow->nextSourceLine == 0 &&
              state.widthReflow->nextByte == 0,
              "100k plain row remains privately in progress across slices");
        check(near(state.cachedViewportWidth, 320.0f),
              "partial plain row never publishes a partial width");
    }
    check(drain(state, "100k plain row") == Step::Published,
          "100k plain row eventually publishes complete geometry");
    check(text.size() == 100000u && state.cachedLines.size() > 1,
          "plain fixture really has 100k UTF-8 bytes and wraps to many visual rows");
    auto actual = build(state, 170.0f, plain, true);
    auto expected = build(oracle, 170.0f, plain);
    compareLayout(state, oracle, actual, expected, "100k plain row oracle");
    state.selectionStart = static_cast<int>(text.size()) - 4;
    state.selectionEnd = 8;
    oracle.selectionStart = state.selectionStart;
    oracle.selectionEnd = state.selectionEnd;
    state.verticalScroll = oracle.verticalScroll = 240.0f;
    actual = build(state, 170.0f, plain, true);
    expected = build(oracle, 170.0f, plain);
    compareLayout(state, oracle, actual, expected, "100k reverse offscreen selection oracle");
}

void testVeryLongStyledTableCell() {
    std::string cell;
    cell.reserve(18000);
    for (int i = 0; i < 1800; ++i) cell += "Aa中🙂 "; // 18,000 UTF-8 bytes.
    const std::string first = "| 甲🙂 | " + cell + " |";
    const std::string second = "| second left | short tail |";
    const std::string text = first + "\n" + second;
    const int leftBeg = 2;
    const int leftEnd = leftBeg + static_cast<int>(std::string("甲🙂").size());
    const int rightBeg = leftEnd + 3;
    const int rightEnd = rightBeg + static_cast<int>(cell.size());
    std::vector<Decoration> decorations(2);
    auto& row = decorations[0];
    row.tableId = 91;
    row.fontSize = 16.0f;
    row.lineHeight = 29.6f;
    row.textShiftY = 4.4f;
    row.cellPadding = 7.0f;
    row.cells = {{leftBeg, leftEnd}, {rightBeg, rightEnd}};
    row.holes = {{0, leftBeg}, {leftEnd, rightBeg}, {rightEnd, static_cast<int>(first.size())}};
    LineRun styled;
    styled.beg = rightBeg + 100;
    styled.end = rightBeg + 130; // three complete 10-byte UTF-8 units
    styled.style.edit().weight = 700;
    styled.style.edit().background = core::Color(0.2f, 0.3f, 0.4f, 1.0f);
    row.runs.push_back(styled);
    auto& trailing = decorations[1];
    const int secondBeg = static_cast<int>(first.size()) + 1;
    const int secondLeftBeg = secondBeg + 2;
    const int secondLeftEnd = secondLeftBeg + static_cast<int>(std::string("second left").size());
    const int secondRightBeg = secondLeftEnd + 3;
    const int secondRightEnd = secondRightBeg + static_cast<int>(std::string("short tail").size());
    trailing.tableId = 91;
    trailing.cellPadding = 7.0f;
    trailing.cells = {{secondLeftBeg, secondLeftEnd}, {secondRightBeg, secondRightEnd}};
    trailing.holes = {{secondBeg, secondLeftBeg}, {secondLeftEnd, secondRightBeg},
                      {secondRightEnd, static_cast<int>(text.size())}};

    State state = makeState(text);
    State oracle = makeState(text);
    state.cursor = oracle.cursor = leftBeg;
    state.selectionStart = oracle.selectionStart = leftBeg;
    state.selectionEnd = oracle.selectionEnd = rightEnd;
    build(state, 320.0f, decorations);
    check(Model::requestWidthReflow(state, kFont, kFontSize, 170.0f, &decorations),
          "18k table cell starts a width draft");
    for (int i = 0; i < 4; ++i) {
        check(Model::stepWidthReflow(state, std::chrono::steady_clock::time_point::max(), 1) == Step::Yield,
              "18k table cell yields under a one-operation budget");
        check(state.widthReflow && state.widthReflow->nextSourceLine == 0 &&
              state.widthReflow->nextByte == 0,
              "18k table cell remains privately in its first source row");
        check(near(state.cachedViewportWidth, 320.0f),
              "partial table cell never publishes a partial width");
    }
    check(drain(state, "18k styled table cell") == Step::Published,
          "18k table cell eventually publishes complete table geometry");
    bool sawStyledRun = false;
    bool sawTableCaret = false;
    std::size_t firstTableVisualRows = 0;
    for (const auto& line : state.cachedLines) {
        if (line.lineNumber == 1) ++firstTableVisualRows;
        for (const auto& run : line.presentation().runs)
            sawStyledRun = sawStyledRun ||
                (run.style.get().weight >= 700 && run.style.get().background.a > 0.0f);
        sawTableCaret = sawTableCaret || !line.rare().tableDocCaretStops.empty();
    }
    check(cell.size() == 18000u && firstTableVisualRows > 1 && sawStyledRun && sawTableCaret,
          "table fixture really wraps styled cell and exposes document caret stops");
    auto actual = build(state, 170.0f, decorations, true);
    auto expected = build(oracle, 170.0f, decorations);
    const auto* columns = actual.tableColumnsFor(91);
    check(columns && near(columns->padding, 7.0f) && columns->count() == 2 &&
          !actual.selectionRects.empty(),
          "table fixture retains two padded columns and visible selection geometry");
    compareLayout(state, oracle, actual, expected, "18k styled table cell oracle");
    state.selectionStart = rightEnd;
    state.selectionEnd = leftBeg;
    oracle.selectionStart = rightEnd;
    oracle.selectionEnd = leftBeg;
    state.verticalScroll = oracle.verticalScroll = 225.0f;
    actual = build(state, 170.0f, decorations, true);
    expected = build(oracle, 170.0f, decorations);
    compareLayout(state, oracle, actual, expected, "18k reverse table selection oracle");
}
} // namespace

int main() {
    testBudgetAndFullOracle();
    testLatestRequestAndCancellation();
    testInvalidationsAndTeardown();
    testOldLayoutOwnerSurvivesPublicationAndStateDestruction();
    testDefaultFontConfigurationChange();
    testCompactStyledCursorOracle();
    testVeryLongPlainSourceRow();
    testVeryLongStyledTableCell();
    return failures ? 1 : 0;
}
