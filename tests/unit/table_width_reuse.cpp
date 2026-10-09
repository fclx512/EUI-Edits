#include "components/input_model.h"
#include <iostream>

using M = components::input_detail::InputModel;
using D = components::input_detail::LineDecoration;

bool same(const M::TextLine& a, const M::TextLine& b) {
    if (a.start != b.start || a.end != b.end || a.holes != b.holes ||
        a.metrics.byteIndices != b.metrics.byteIndices || a.metrics.caretX != b.metrics.caretX ||
        a.metrics.width != b.metrics.width || a.lineHeight != b.lineHeight ||
        a.textBandHeight != b.textBandHeight ||
        a.tableId != b.tableId || a.runs.size() != b.runs.size()) return false;
    for (size_t i = 0; i < a.runs.size(); ++i) {
        const auto& x = a.runs[i]; const auto& y = b.runs[i];
        if (x.beg != y.beg || x.end != y.end || x.x != y.x || x.width != y.width || x.style != y.style) return false;
    }
    return true;
}

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* why) { if (!ok) { std::cerr << why << '\n'; ++fails; } };
    M::InputState state;
    // UTF-8 combines multibyte text, a combining mark, ligature candidates and hidden delimiters.
    state.text = "**office e\xCC\x81 \xE4\xB8\xAD\xE6\x96\x87**\nWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW\nshort\n";
    state.textRevision = 1;
    const int firstEnd = static_cast<int>(state.text.find('\n'));
    const int secondEnd = static_cast<int>(state.text.find('\n', firstEnd + 1));
    const int thirdEnd = static_cast<int>(state.text.find('\n', secondEnd + 1));
    std::vector<D> rows(4);
    for (int i = 0; i < 3; ++i) {
        rows[i].tableId = i == 2 ? 900001 : 17;
        rows[i].lineHeight = 28; rows[i].cellPadding = 5;
    }
    rows[0].cells = {{2, firstEnd - 2, 2}};
    rows[0].holes = {{0, 2}, {firstEnd - 2, firstEnd}};
    components::input_detail::LineRun run;
    run.beg = 2; run.end = firstEnd - 2; run.style.weight = 700;
    rows[0].runs = {run};
    rows[1].cells = {{firstEnd + 1, secondEnd, 3}};
    rows[2].cells = {{secondEnd + 1, thirdEnd}};
    auto build = [&](float width, float font = 16) {
        M::ensureLayoutCache(state, "Microsoft YaHei", font, width, true, &rows);
        std::vector<M::TableColumns> referenceTables;
        const auto reference = M::measureLines(state.text, "Microsoft YaHei", font, width, &rows, &referenceTables);
        check(state.cachedLines.size() == reference.size(), "cached wrap count must equal cold layout");
        for (size_t i = 0; i < std::min(reference.size(), state.cachedLines.size()); ++i)
            check(same(state.cachedLines[i], reference[i]), "cached caret/runs must equal cold layout");
        check(state.cachedTables.size() == referenceTables.size(), "table count mismatch");
        for (size_t i = 0; i < std::min(referenceTables.size(), state.cachedTables.size()); ++i) {
            const auto& a = state.cachedTables[i]; const auto& b = referenceTables[i];
            check(a.x == b.x && a.width == b.width && a.total == b.total, "columns must be constrained from original widths");
        }
    };
    M::debugLayoutStats() = {};
    build(800);
    const auto raw = state.cachedTableIntrinsic;
    for (float width : {80.f, 350.f, 90.f, 800.f}) build(width);
    check(M::debugLayoutStats().tableIntrinsicReused == 4, "width-only changes must reuse intrinsic widths");
    check(state.cachedTableIntrinsic[0].width == raw[0].width, "viewport constraint must not overwrite raw widths");
    check(M::debugLayoutStats().tableWholeCellReused > 0, "unwrapped styled cells must reuse whole layout");
    auto assertInvalidated = [&](const char* why, float font = 16) {
        const auto before = M::debugLayoutStats().tableIntrinsicReused;
        build(710, font);
        check(M::debugLayoutStats().tableIntrinsicReused == before, why);
    };
    rows[1].hidden = true; assertInvalidated("hiding widest row must invalidate raw widths");
    rows[1].hidden = false; assertInvalidated("revealing row must invalidate raw widths");
    assertInvalidated("font size must invalidate raw widths", 20);
    core::TextPrimitive::setLayoutPixelScale(1.5f);
    assertInvalidated("DPI metrics change must invalidate raw widths", 20);
    core::TextPrimitive::setLayoutPixelScale(1.f);
    state.text[secondEnd - 1] = 'i'; ++state.textRevision;
    assertInvalidated("edited text must invalidate raw widths");
    build(90);
    state.layoutCacheValid = false; assertInvalidated("explicit invalidation must remeasure");
    M::InputState plainState;
    plainState.text = state.text; plainState.textRevision = 1;
    M::ensureLayoutCache(plainState, "Microsoft YaHei", 16, 800, true, &rows);
    plainState.text = "plain\n"; ++plainState.textRevision; plainState.layoutCacheValid = false;
    M::ensureLayoutCache(plainState, "Microsoft YaHei", 16, 800, true);
    check(plainState.cachedTableIntrinsic.capacity() == 0, "switching documents must release raw plan storage");
    M::InputState target;
    M::transferLayoutCache(state, target);
    check(!target.cachedTableIntrinsic.empty() && state.cachedTableIntrinsic.empty(), "IME transfer must move raw plans");
    M::ensureLayoutCache(target, "monospace", 16, 500, false);
    check(target.cachedTableIntrinsic.empty(), "single-line mode must release intrinsic cache");
    state.text = "plain\n"; ++state.textRevision;
    M::ensureLayoutCache(state, "monospace", 16, 500, true);
    check(state.cachedTableIntrinsic.empty(), "plain text must release intrinsic cache");
    return fails ? 1 : 0;
}
