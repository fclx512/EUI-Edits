#include "components/input_model.h"
#include <iostream>

using M = components::input_detail::InputModel;
using D = components::input_detail::LineDecoration;

struct Fixture {
    std::string prefix = "intro\n";
    std::vector<std::string> first = {"WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", "short"};
    bool hidden = false, bold = false;
    std::string text;
    std::vector<D> rows;
    void build() {
        text = prefix;
        rows.assign(static_cast<size_t>(M::countNewlines(prefix, static_cast<int>(prefix.size()))), D{});
        auto group = [&](const std::vector<std::string>& cells, bool firstGroup) {
            const int id = static_cast<int>(text.size());
            for (size_t i = 0; i < cells.size(); ++i) {
                const int beg = static_cast<int>(text.size());
                text += cells[i] + '\n';
                D row;
                row.tableId = id; row.cells = {{beg, static_cast<int>(text.size()) - 1, 3}};
                row.cellPadding = 5; row.lineHeight = 28;
                row.hidden = firstGroup && i == 0 && hidden;
                if (firstGroup && i == 0 && bold) {
                    components::input_detail::LineRun run;
                    run.beg = beg; run.end = row.cells[0].end; run.style.weight = 700;
                    row.runs.push_back(run);
                }
                rows.push_back(std::move(row));
            }
            text += '\n'; rows.emplace_back();
        };
        if (!first.empty()) group(first, true);
        group({"office e\xCC\x81 \xE4\xB8\xAD\xE6\x96\x87", "second"}, false);
        rows.emplace_back();
    }
};

bool sameLine(const M::TextLine& a, const M::TextLine& b) {
    if (a.start != b.start || a.end != b.end || a.lineNumber != b.lineNumber ||
        a.holes != b.holes || a.metrics.byteIndices != b.metrics.byteIndices ||
        a.metrics.caretX != b.metrics.caretX || a.metrics.width != b.metrics.width ||
        a.lineHeight != b.lineHeight || a.hidden != b.hidden || a.tableId != b.tableId ||
        a.runs.size() != b.runs.size()) return false;
    for (size_t i = 0; i < a.runs.size(); ++i) {
        const auto& x = a.runs[i]; const auto& y = b.runs[i];
        if (x.beg != y.beg || x.end != y.end || x.x != y.x || x.width != y.width || x.style != y.style)
            return false;
    }
    return true;
}

int main() {
    int failures = 0, checks = 0;
    auto check = [&](bool ok, const char* why) {
        ++checks;
        if (!ok) { ++failures; std::cerr << why << '\n'; }
    };
    Fixture doc; doc.build();
    M::InputState state;
    state.text = doc.text; state.textRevision = 1;
    auto oracle = [&](float width) {
        std::vector<M::TableColumns> tables;
        const auto lines = M::measureLines(state.text, "Microsoft YaHei", 16, width, &doc.rows, &tables);
        check(lines.size() == state.cachedLines.size(), "physical row count differs from cold layout");
        for (size_t i = 0; i < std::min(lines.size(), state.cachedLines.size()); ++i)
            check(sameLine(lines[i], state.cachedLines[i]), "carets/styles/wrap differ from cold layout");
        const auto raw = M::buildTableIntrinsicColumns(state.text, doc.rows, "Microsoft YaHei", 16);
        check(raw.size() == state.cachedTableIntrinsic.size() && tables.size() == state.cachedTables.size(),
              "raw/visible table counts must match cold plans");
        for (size_t i = 0; i < std::min(raw.size(), state.cachedTableIntrinsic.size()); ++i) {
            const auto& a = raw[i]; const auto& b = state.cachedTableIntrinsic[i];
            check(a.tableId == b.tableId && a.width == b.width && a.padding == b.padding && b.x.empty(),
                  "retained raw plan differs from cold measurement");
            check(i < tables.size() && i < state.cachedTables.size() &&
                  tables[i].x == state.cachedTables[i].x && tables[i].width == state.cachedTables[i].width &&
                  tables[i].total == state.cachedTables[i].total, "visible geometry differs from cold plan");
        }
    };
    M::ensureLayoutCache(state, "Microsoft YaHei", 16, 800, true, &doc.rows);
    oracle(800);
    auto edit = [&](int expectedRebuilt) {
        doc.build(); state.text = doc.text; ++state.textRevision;
        std::vector<const float*> oldStorage;
        for (const auto& table : state.cachedTableIntrinsic) oldStorage.push_back(table.width.data());
        M::debugLayoutStats() = {};
        M::ensureLayoutCache(state, "Microsoft YaHei", 16, 800, true, &doc.rows);
        const auto stats = M::debugLayoutStats();
        check(stats.full == 0 && stats.incremental == 1, "fixture must exercise incremental publication");
        check(stats.tableRebuilt == static_cast<unsigned long long>(expectedRebuilt), "wrong dirty table count");
        check(stats.tableIntrinsicBuilt == static_cast<unsigned long long>(expectedRebuilt), "unaffected table remeasured");
        if (expectedRebuilt == 0 && oldStorage.size() == state.cachedTableIntrinsic.size()) {
            for (size_t i = 0; i < oldStorage.size(); ++i)
                check(oldStorage[i] == state.cachedTableIntrinsic[i].width.data(), "untouched width storage must transfer without allocation");
        }
        oracle(800);
        for (float width : {90.f, 500.f, 800.f}) {
            M::debugLayoutStats() = {};
            M::ensureLayoutCache(state, "Microsoft YaHei", 16, width, true, &doc.rows);
            check(M::debugLayoutStats().tableIntrinsicBuilt == 0 && M::debugLayoutStats().tableIntrinsicReused == 1,
                  "resize after edit must use published intrinsic plans");
            oracle(width);
        }
    };
    doc.prefix.insert(0, "prefix\n"); edit(0);
    doc.prefix = "intro\n"; edit(0); // undo-like byte and source-row translation
    doc.first[0] = "tiny"; edit(1); // shrinking former maximum
    doc.first.insert(doc.first.begin(), std::string(90, 'W')); edit(1);
    doc.first.erase(doc.first.begin()); edit(1); // deleting maximum row
    doc.hidden = true; edit(1);
    doc.hidden = false; edit(1);
    doc.bold = true; edit(1);
    doc.first.clear(); edit(1); // diff includes surviving boundary row: conservatively rebuild
    check(state.cachedTableIntrinsic.size() == 1, "deleted table retained");
    M::InputState transferred;
    transferred.text = state.text; transferred.textRevision = state.textRevision;
    M::transferLayoutCache(state, transferred);
    // Transfer deliberately requests one reconciliation before width reuse.
    M::ensureLayoutCache(transferred, "Microsoft YaHei", 16, 800, true, &doc.rows);
    M::debugLayoutStats() = {};
    M::ensureLayoutCache(transferred, "Microsoft YaHei", 16, 90, true, &doc.rows);
    check(M::debugLayoutStats().tableIntrinsicReused == 1, "IME cache transfer lost retained plans");
    transferred.text = "plain\n"; ++transferred.textRevision;
    M::ensureLayoutCache(transferred, "Microsoft YaHei", 16, 90, true);
    check(transferred.cachedTableIntrinsic.capacity() == 0, "plain text must release raw plans");
    std::cout << "[table-edit-intrinsic] checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
