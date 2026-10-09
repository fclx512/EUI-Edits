#include "components/input_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using Model = components::input_detail::InputModel;
using State = Model::InputState;
using Layout = Model::InputLayout;
using Line = Model::TextLine;
using Decoration = components::input_detail::LineDecoration;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool near(float a, float b, float epsilon = 0.02f) {
    return std::fabs(a - b) <= epsilon;
}

std::string makeDocument(int rows, int charsPerRow = 46) {
    std::string text;
    text.reserve(static_cast<std::size_t>(rows) * (charsPerRow + 1));
    for (int row = 0; row < rows; ++row) {
        if (row % 29 == 0) {
            text += "中文UTF8-行-";
        } else if (row % 29 == 1) {
            text += "**styled** `code` [link](target) ";
        } else if (row % 29 == 2) {
            text += "- [ ] glyph and indent ";
        } else if (row % 29 == 3) {
            text += "| head | value |\n| --- | --- |\n| cell | data |";
            // These three source rows are emitted together below; skip the next two.
            if (row + 2 < rows) {
                row += 2;
            }
        }
        const std::size_t lineStart = text.rfind('\n') == std::string::npos
            ? 0 : text.rfind('\n') + 1;
        const std::size_t lineLength = text.size() - lineStart;
        if (lineLength < static_cast<std::size_t>(charsPerRow)) {
            text.append(static_cast<std::size_t>(charsPerRow) - lineLength,
                        static_cast<char>('a' + row % 23));
        }
        if (row + 1 < rows) text.push_back('\n');
    }
    return text;
}

std::vector<Decoration> makeDecorations(const std::string& text, bool rich) {
    std::vector<Decoration> rows;
    int start = 0;
    while (start <= static_cast<int>(text.size())) {
        const auto nl = text.find('\n', static_cast<std::size_t>(start));
        const int end = nl == std::string::npos ? static_cast<int>(text.size()) : static_cast<int>(nl);
        Decoration d;
        if (rich && rows.size() % 29 == 0 && end - start >= 3) {
            d.holes.push_back({start, start + 1});
        }
        if (rich && rows.size() % 29 == 1 && end - start >= 12) {
            components::input_detail::LineRun run;
            run.beg = start + 2;
            run.end = std::min(end, start + 10);
            run.style.weight = 700;
            d.runs.push_back(run);
        }
        if (rich && rows.size() % 29 == 2) {
            d.glyph.checkbox = true;
            d.glyph.advance = 18.0f;
            d.contentIndent = 3.0f;
        }
        if (rich && rows.size() % 29 == 6) {
            d.hidden = true;
            d.hiddenByFold = true;
        }
        if (rich && rows.size() % 29 == 7) {
            d.spaceBefore = 5.5f;
            d.fontSize = 18.0f;
            d.lineHeight = 24.0f;
            d.textShiftY = 1.5f;
        }
        // Mark complete 3-row groups as one table. The row text is deliberately
        // varied and short enough to exercise the full-cell-caret path.
        if (rich && rows.size() % 29 >= 3 && rows.size() % 29 <= 5) {
            d.tableId = static_cast<int>(rows.size() - rows.size() % 29 + 3);
            d.cells = {{start, std::min(end, start + 6), 1},
                       {std::min(end, start + 7), std::min(end, start + 15), 1}};
            d.tableHeaderRow = rows.size() % 29 == 3;
        }
        rows.push_back(std::move(d));
        if (nl == std::string::npos) break;
        start = static_cast<int>(nl) + 1;
    }
    return rows;
}

components::input_detail::LineDecorationSnapshot snapshotFor(const std::string& text,
                                                             bool rich = true) {
    return std::make_shared<const components::input_detail::LineDecorationTable>(
        makeDecorations(text, rich));
}

Layout build(State& state, float scroll,
             const components::input_detail::LineDecorationSnapshot& snapshot,
             float width = 280.0f, float height = 240.0f, bool wordWrap = true) {
    if (state.decorationSnapshot != snapshot) {
        state.decorationSnapshot = snapshot;
        state.decorations.clear();
    }
    state.verticalScroll = scroll;
    state.followCaret = false;
    state.wordWrap = wordWrap;
    return Layout::build(state, width, height, width, 8.0f, 0.0f, 0.0f,
                         18.0f, "monospace", 14.0f, true,
                         components::input_detail::LineDecorationView(*snapshot), snapshot);
}

bool sameRuns(const std::vector<components::input_detail::TextRun>& a,
              const std::vector<components::input_detail::TextRun>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].beg != b[i].beg || a[i].end != b[i].end ||
            !near(a[i].x, b[i].x) || !near(a[i].width, b[i].width) || a[i].style != b[i].style) {
            return false;
        }
    }
    return true;
}

bool sameLineShape(const Line& a, const Line& b, int index) {
    const auto prefix = "row geometry mismatch at visual line " + std::to_string(index);
    if (a.start != b.start || a.end != b.end || a.hardBreakAfter != b.hardBreakAfter ||
        !near(a.top, b.top) || !near(a.lineHeight, b.lineHeight) ||
        !near(a.fontSize, b.fontSize) || !near(a.metrics.width, b.metrics.width) ||
        !near(a.spaceBefore, b.spaceBefore) || !near(a.textShiftY, b.textShiftY) ||
        !near(a.textBandHeight, b.textBandHeight) ||
        a.hidden != b.hidden || a.hiddenByFold != b.hiddenByFold ||
        a.lineNumber != b.lineNumber || a.lineStart != b.lineStart ||
        a.tableId != b.tableId || a.holes != b.holes || !sameRuns(a.runs, b.runs) ||
        !near(a.contentIndent, b.contentIndent) || a.glyph != b.glyph) {
        std::cerr << prefix << '\n';
        return false;
    }
    return true;
}

std::size_t caretStops(const std::vector<Line>& lines, bool tablesOnly = false) {
    std::size_t result = 0;
    for (const auto& line : lines) {
        if (tablesOnly && line.tableId < 0) continue;
        result += std::min(line.metrics.byteIndices.size(), line.metrics.caretX.size());
    }
    return result;
}

std::size_t caretCapacityBytes(const std::vector<Line>& lines) {
    std::size_t bytes = 0;
    for (const auto& line : lines) {
        bytes += line.metrics.byteIndices.capacity() * sizeof(int);
        bytes += line.metrics.caretX.capacity() * sizeof(float);
    }
    return bytes;
}

void compareLayouts(const Layout& full, const Layout& compact, const char* context) {
    const auto& a = full.lineList();
    const auto& b = compact.lineList();
    check(a.size() == b.size(), std::string(context) + ": visual line count is identical");
    if (a.size() != b.size()) return;
    for (std::size_t i = 0; i < a.size(); ++i) {
        check(sameLineShape(a[i], b[i], static_cast<int>(i)),
              std::string(context) + ": compact/full exact geometry agrees");
        check(full.geometryTable().top(static_cast<int>(i)) == compact.geometryTable().top(static_cast<int>(i)) &&
              full.geometryTable().height(static_cast<int>(i)) == compact.geometryTable().height(static_cast<int>(i)),
              std::string(context) + ": prefix geometry table agrees");
    }
    check(full.geometryTable().total() == compact.geometryTable().total(),
          std::string(context) + ": total content height agrees");
}

void verifyRandomQueries(State& fullState, State& compactState, Layout& full,
                         Layout& compact, const std::string& text, unsigned seed) {
    std::mt19937 random(seed);
    std::uniform_int_distribution<int> byte(0, static_cast<int>(text.size()));
    std::uniform_int_distribution<int> row(0, static_cast<int>(full.lineList().size()) - 1);
    for (int i = 0; i < 80; ++i) {
        int offset = byte(random);
        offset = Model::clampUtf8Boundary(text, offset);
        check(full.xFor(offset) == compact.xFor(offset), "random byte x lookup matches and hydrates on demand");

        const int lineIndex = row(random);
        const float x = static_cast<float>((i * 37) % 280);
        check(full.closestCaret(lineIndex, x) == compact.closestCaret(lineIndex, x),
              "random distant hit-test caret matches after lazy line hydration");

        const auto& line = full.lineList()[static_cast<std::size_t>(lineIndex)];
        const float y = full.geometryTable().top(lineIndex) + full.geometryTable().height(lineIndex) * 0.5f -
                        full.currentVerticalScroll;
        const float screenX = 8.0f + x;
        const auto fullHit = full.pointerHit(screenX, y, {0.0f, 0.0f, 280.0f, 240.0f}, 280.0f, 8.0f);
        const auto compactHit = compact.pointerHit(screenX, y, {0.0f, 0.0f, 280.0f, 240.0f}, 280.0f, 8.0f);
        check(fullHit.lineIndex == compactHit.lineIndex && fullHit.byteIndex == compactHit.byteIndex,
              "pointer hit index matches on arbitrary rows");
        (void)line;
    }
    (void)fullState;
    (void)compactState;
}

void verifyNavigation(State& full, State& compact, const std::string& font, float width, float height) {
    const std::array<int, 5> cursors{0, 47, static_cast<int>(full.text.size() / 3),
                                     static_cast<int>(full.text.size() * 2 / 3),
                                     static_cast<int>(full.text.size())};
    for (int cursor : cursors) {
        cursor = Model::clampUtf8Boundary(full.text, cursor);
        full.cursor = compact.cursor = cursor;
        check(Model::prevCursorIndex(full, font, 14.0f, true, width) ==
              Model::prevCursorIndex(compact, font, 14.0f, true, width), "left navigation matches");
        check(Model::nextCursorIndex(full, font, 14.0f, true, width) ==
              Model::nextCursorIndex(compact, font, 14.0f, true, width), "right navigation matches");
        for (bool toEnd : {false, true}) {
            full.cursor = compact.cursor = cursor;
            Model::moveCursorToLineEdge(full, toEnd, false, font, 14.0f, width);
            Model::moveCursorToLineEdge(compact, toEnd, false, font, 14.0f, width);
            check(full.cursor == compact.cursor, "Home/End visual-line destination matches");
        }
        full.cursor = compact.cursor = cursor;
        Model::moveCursorVertical(full, 1, false, font, 14.0f, width, height);
        Model::moveCursorVertical(compact, 1, false, font, 14.0f, width, height);
        check(full.cursor == compact.cursor, "single-line vertical movement destination matches");
    }
}

void verifyPageNavigation(State& full, State& compact, const std::string& font,
                          float width, float height) {
    const int start = Model::clampUtf8Boundary(full.text, static_cast<int>(full.text.size() / 2));
    for (int direction : {-1, 1}) {
        for (bool keepSelection : {false, true}) {
            full.cursor = compact.cursor = start;
            full.selectionStart = compact.selectionStart = start;
            full.selectionEnd = compact.selectionEnd = start;
            full.hasPreferredCursorX = compact.hasPreferredCursorX = false;
            Model::moveCursorPage(full, direction, keepSelection, font, 14.0f, width, height);
            Model::moveCursorPage(compact, direction, keepSelection, font, 14.0f, width, height);
            check(full.cursor == compact.cursor && full.selectionStart == compact.selectionStart &&
                  full.selectionEnd == compact.selectionEnd,
                  std::string("Page") + (direction < 0 ? "Up" : "Down") +
                      (keepSelection ? " with selection" : " without selection") + " matches");
        }
    }
}

void compareSelectionRects(const Layout& full, const Layout& compact, const char* context) {
    check(full.selectionRects.size() == compact.selectionRects.size(),
          std::string(context) + ": selection rectangle count matches");
    const std::size_t n = std::min(full.selectionRects.size(), compact.selectionRects.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = full.selectionRects[i];
        const auto& b = compact.selectionRects[i];
        check(near(a.x, b.x) && near(a.y, b.y) && near(a.width, b.width) &&
              near(a.height, b.height) && near(a.lineHeight, b.lineHeight),
              std::string(context) + ": viewport selection geometry matches");
    }
}

void compareState(const State& full, const State& compact, const char* context) {
    check(full.text == compact.text && full.cursor == compact.cursor &&
          full.selectionStart == compact.selectionStart && full.selectionEnd == compact.selectionEnd,
          std::string(context) + ": document text and caret/selection match");
}

} // namespace

int main() {
    constexpr int rowCount = 7500;
    constexpr float viewportHeight = 240.0f;
    constexpr float viewportWidth = 280.0f;
    constexpr const char* font = "monospace";
    const std::string text = makeDocument(rowCount);
    check(text.size() >= 256u * 1024u, "fixture exceeds viewport-metrics activation threshold");
    const auto richSnapshot = snapshotFor(text, true);

    State fullState;
    State compactState;
    fullState.text = compactState.text = text;
    compactState.viewportMetrics = true;
    fullState.cursor = compactState.cursor = 0;
    fullState.wordWrap = compactState.wordWrap = true;

    auto fullLayout = build(fullState, 0.0f, richSnapshot);
    auto compactLayout = build(compactState, 0.0f, richSnapshot);
    check(compactState.cachedViewportMetrics, "large opted-in document selects compact caret metrics");
    compareLayouts(fullLayout, compactLayout, "initial rich document");
    const std::size_t fullInitialStops = caretStops(fullLayout.lineList());
    const std::size_t compactInitialStops = caretStops(compactLayout.lineList());
    check(compactInitialStops < fullInitialStops / 5,
          "compact layout initially retains only viewport/cursor details plus full table paths");
    check(caretStops(compactLayout.lineList(), true) > 0,
          "table rows preserve exact full caret arrays");

    // Walk forward in many viewport-sized steps. Geometric output must be
    // identical and the detail arrays must be evicted back to a bounded set.
    std::size_t maximumResident = 0;
    for (int step = 1; step <= 40; ++step) {
        const float scroll = std::min(compactLayout.geometryTable().total() - viewportHeight,
                                      static_cast<float>(step) * 420.0f);
        fullLayout = build(fullState, scroll, richSnapshot);
        compactLayout = build(compactState, scroll, richSnapshot);
        compareLayouts(fullLayout, compactLayout, "scrolled rich document");
        const std::size_t resident = caretStops(compactLayout.lineList());
        maximumResident = std::max(maximumResident, resident);
        check(resident < fullInitialStops / 4,
              "scroll pruning keeps resident caret arrays below one quarter of full layout");
        verifyRandomQueries(fullState, compactState, fullLayout, compactLayout, text,
                            static_cast<unsigned>(step * 977));
    }
    check(maximumResident < fullInitialStops / 4,
          "repeated distant hit tests do not grow resident caret arrays without bound");

    const auto fullStatsBeforeWarmBuilds = Model::debugLayoutStats();
    for (int step = 41; step <= 45; ++step) {
        fullLayout = build(fullState, static_cast<float>(step) * 300.0f, richSnapshot);
        compactLayout = build(compactState, static_cast<float>(step) * 300.0f, richSnapshot);
    }
    const auto fullStatsAfterWarmBuilds = Model::debugLayoutStats();
    check(fullStatsAfterWarmBuilds.full == fullStatsBeforeWarmBuilds.full,
          "warm viewport builds do not increase full document layout count");

    verifyNavigation(fullState, compactState, font, viewportWidth, viewportHeight);
    verifyPageNavigation(fullState, compactState, font, viewportWidth, viewportHeight);

    // A selection spanning the whole document should materialize only visible
    // rectangles while preserving their exact clipped geometry in compact mode.
    fullState.selectionStart = compactState.selectionStart = 0;
    fullState.selectionEnd = compactState.selectionEnd = static_cast<int>(text.size());
    fullState.cursor = compactState.cursor = static_cast<int>(text.size());
    constexpr float selectionScroll = 38000.0f;
    fullLayout = build(fullState, selectionScroll, richSnapshot);
    compactLayout = build(compactState, selectionScroll, richSnapshot);
    compareSelectionRects(fullLayout, compactLayout, "whole-document selection at middle viewport");

    // Reflow inputs include wrap mode and width. Compare both a narrower wrap
    // and no-wrap mode followed by resizing back to a wider viewport.
    for (const auto [width, wrap] : {std::pair<float, bool>{190.0f, true},
                                     std::pair<float, bool>{190.0f, false},
                                     std::pair<float, bool>{360.0f, true}}) {
        fullLayout = build(fullState, 0.0f, richSnapshot, width, viewportHeight, wrap);
        compactLayout = build(compactState, 0.0f, richSnapshot, width, viewportHeight, wrap);
        compareLayouts(fullLayout, compactLayout, wrap ? "resize with wrap" : "no-wrap reflow");
    }

    // Display-state transfer during composition must preserve the opt-in and
    // continue to produce the same layout for the temporary composed text.
    fullState.compositionText = compactState.compositionText = "合成中文";
    fullState.selectionStart = compactState.selectionStart = Model::clampUtf8Boundary(fullState.text, 120);
    fullState.selectionEnd = compactState.selectionEnd = Model::clampUtf8Boundary(fullState.text, 128);
    auto& fullDisplay = Model::displayState(fullState, true);
    auto& compactDisplay = Model::displayState(compactState, true);
    check(compactDisplay.viewportMetrics, "IME display state inherits viewport metrics mode");
    const auto displaySnapshot = snapshotFor(fullDisplay.text, true);
    auto fullDisplayLayout = build(fullDisplay, 0.0f, displaySnapshot);
    auto compactDisplayLayout = build(compactDisplay, 0.0f, displaySnapshot);
    compareLayouts(fullDisplayLayout, compactDisplayLayout, "IME display-state migration");
    Model::displayState(fullState, false);
    Model::displayState(compactState, false);
    check(compactState.viewportMetrics, "return from IME retains viewport metrics mode");

    // Same operations and text must still match after a genuine edit invalidates
    // cached rows. Use plain text decorations so their ranges remain meaningful.
    fullState.text = compactState.text = text;
    fullState.layoutCacheValid = compactState.layoutCacheValid = false;
    fullState.cursor = compactState.cursor = Model::clampUtf8Boundary(fullState.text, 50);
    Model::insertAtCursor(fullState, "编辑");
    Model::insertAtCursor(compactState, "编辑");
    check(fullState.text == compactState.text, "edit text remains identical");
    fullState.selectionStart = compactState.selectionStart = Model::clampUtf8Boundary(fullState.text, 80);
    fullState.selectionEnd = compactState.selectionEnd = Model::clampUtf8Boundary(fullState.text, 95);
    fullState.cursor = compactState.cursor = fullState.selectionEnd;
    Model::eraseSelection(fullState);
    Model::eraseSelection(compactState);
    check(fullState.text == compactState.text, "selected deletion remains identical");
    auto editedSnapshot = snapshotFor(fullState.text, false);
    fullLayout = build(fullState, 0.0f, editedSnapshot);
    compactLayout = build(compactState, 0.0f, editedSnapshot);
    compareLayouts(fullLayout, compactLayout, "post-edit layout rebuild");

    // Undo and redo must restore identical text, caret, selection, and layout.
    for (int i = 0; i < 2; ++i) {
        const bool fullUndid = Model::undoEdit(fullState);
        const bool compactUndid = Model::undoEdit(compactState);
        check(fullUndid == compactUndid, "undo availability matches");
        compareState(fullState, compactState, "undo result");
        editedSnapshot = snapshotFor(fullState.text, false);
        fullLayout = build(fullState, 0.0f, editedSnapshot);
        compactLayout = build(compactState, 0.0f, editedSnapshot);
        compareLayouts(fullLayout, compactLayout, "undo geometry");
    }
    for (int i = 0; i < 2; ++i) {
        const bool fullRedid = Model::redoEdit(fullState);
        const bool compactRedid = Model::redoEdit(compactState);
        check(fullRedid == compactRedid, "redo availability matches");
        compareState(fullState, compactState, "redo result");
        editedSnapshot = snapshotFor(fullState.text, false);
        fullLayout = build(fullState, 0.0f, editedSnapshot);
        compactLayout = build(compactState, 0.0f, editedSnapshot);
        compareLayouts(fullLayout, compactLayout, "redo geometry");
    }

    // Repeat a smaller set at a non-unit text layout scale: geometry should
    // remain equivalent across compact and full metric modes at the same DPI.
    core::TextPrimitive::setLayoutPixelScale(1.25f);
    fullState.layoutCacheValid = compactState.layoutCacheValid = false;
    fullLayout = build(fullState, 0.0f, editedSnapshot);
    compactLayout = build(compactState, 0.0f, editedSnapshot);
    compareLayouts(fullLayout, compactLayout, "1.25x DPI layout");
    core::TextPrimitive::setLayoutPixelScale(1.0f);

    // A 2 MiB plain document checks the larger memory shape directly from the
    // component's resident caret vectors. This intentionally does not depend on
    // the separate global TextMetrics cache byte counters or elapsed time.
    std::string twoMiBText;
    constexpr char chunk[] = "abcdefghijklmnopqrstuvwxyz0123456789 ABCDEFGHIJKLMNO\n";
    twoMiBText.reserve(2u * 1024u * 1024u + sizeof(chunk) * 4000u);
    while (twoMiBText.size() < 2u * 1024u * 1024u) twoMiBText += chunk;
    const auto twoMiBSnapshot = snapshotFor(twoMiBText, false);
    State twoMiBFull;
    State twoMiBCompact;
    twoMiBFull.text = twoMiBCompact.text = twoMiBText;
    twoMiBCompact.viewportMetrics = true;
    auto twoMiBFullLayout = build(twoMiBFull, 0.0f, twoMiBSnapshot, 900.0f, viewportHeight);
    auto twoMiBCompactLayout = build(twoMiBCompact, 0.0f, twoMiBSnapshot, 900.0f, viewportHeight);
    check(twoMiBText.size() >= 2u * 1024u * 1024u, "large fixture reaches 2 MiB");
    check(twoMiBCompact.cachedViewportMetrics, "2 MiB opted-in document uses compact metrics");
    check(twoMiBFullLayout.lineList().size() == twoMiBCompactLayout.lineList().size() &&
          twoMiBFullLayout.geometryTable().total() == twoMiBCompactLayout.geometryTable().total(),
          "2 MiB full and compact layouts preserve visual line count and total geometry");
    const auto& twoMiBFullLines = twoMiBFullLayout.lineList();
    const auto& twoMiBCompactLines = twoMiBCompactLayout.lineList();
    for (std::size_t i = 0; i < twoMiBFullLines.size(); i += 997) {
        check(twoMiBFullLines[i].start == twoMiBCompactLines[i].start &&
              twoMiBFullLines[i].end == twoMiBCompactLines[i].end &&
              twoMiBFullLayout.geometryTable().top(static_cast<int>(i)) ==
                  twoMiBCompactLayout.geometryTable().top(static_cast<int>(i)) &&
              twoMiBFullLayout.geometryTable().height(static_cast<int>(i)) ==
                  twoMiBCompactLayout.geometryTable().height(static_cast<int>(i)),
              "2 MiB sampled row geometry remains equivalent");
    }
    check(caretCapacityBytes(twoMiBCompactLines) < caretCapacityBytes(twoMiBFullLines) / 8,
          "2 MiB compact caret capacity is bounded independently of text-metrics cache bytes");

    std::cout << (failures == 0 ? "PASS" : "FAIL")
              << ": input_viewport_metrics (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
