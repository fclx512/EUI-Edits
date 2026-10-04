#include "components/input_model.h"

#include <iostream>
#include <limits>

using Model = components::input_detail::InputModel;
using Decoration = components::input_detail::LineDecoration;

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << message << '\n'; }
    };
    std::string text;
    std::vector<Decoration> rows;
    std::vector<int> ids;
    // Repeated vector growth and rehash, sparse IDs, different longest cells.
    for (int t = 0; t < 256; ++t) {
        const int id = t == 0 ? std::numeric_limits<int>::max() : 9000000 - t * 137;
        ids.push_back(id);
        for (int row = 0; row < 3; ++row) {
            const int start = static_cast<int>(text.size());
            text += std::string(row == 2 ? 80 : 2 + t % 27, 'W') + '\n';
            Decoration dec;
            dec.tableId = id;
            dec.hidden = row == 2;
            dec.cells = {{start, static_cast<int>(text.size()) - 1}};
            dec.fontSize = 16;
            dec.lineHeight = 24;
            dec.cellPadding = 4;
            rows.push_back(std::move(dec));
        }
    }
    rows.emplace_back();
    for (float width : {800.0f, 110.0f}) {
        Model::TableColumnIndex index;
        const auto tables = Model::buildTableColumns(text, rows, "monospace", 16, width, 0, -1, &index);
        check(tables.size() == ids.size() && index.size() == ids.size(), "one plan per visible table required");
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto* table = Model::findTableColumns(tables, index, ids[i]);
            check(table && table == &tables[i], "sparse IDs must preserve encounter order after growth");
            const auto isolated = Model::buildTableColumns(text, rows, "monospace", 16, width,
                                                          static_cast<int>(i * 3), static_cast<int>(i * 3 + 3));
            check(table && isolated.size() == 1 && table->x == isolated[0].x &&
                  table->width == isolated[0].width && table->total == isolated[0].total &&
                  table->padding == isolated[0].padding, "batch columns must match each isolated table");
        }
        check(!Model::findTableColumns(tables, index, -1) && !Model::findTableColumns(tables, index, 42),
              "missing IDs must return no plan");
        auto copied = tables;
        check(Model::findTableColumns(copied, index, ids[0]) == &copied[0], "index must not retain vector addresses");
        auto corrupt = index;
        corrupt[42] = tables.size();
        check(!Model::findTableColumns(tables, corrupt, 42), "out of bounds index must fail closed");
        corrupt[42] = 0;
        check(!Model::findTableColumns(tables, corrupt, 42), "index pointing at another ID must fail closed");
        Model::InputState state;
        state.text = text;
        state.textRevision = 1;
        const auto layout = Model::InputLayout::build(state, width, 500, width, 0, 0, 0,
                                                      24, "monospace", 16, true, &rows);
        check(layout.tableColumnsFor(ids.back()) == &state.cachedTables.back(), "draw lookup must use published plan index");
        Model::InputState transferred;
        Model::transferLayoutCache(state, transferred);
        check(Model::findTableColumns(transferred.cachedTables, transferred.cachedTableIndex, ids.back()) ==
              &transferred.cachedTables.back(), "IME layout transfer must move table index with plans");
        transferred.text = "plain\n";
        ++transferred.textRevision;
        transferred.layoutCacheValid = false;
        const auto plain = Model::InputLayout::build(transferred, width, 500, width, 0, 0, 0,
                                                     24, "monospace", 16, true);
        check(!plain.tableColumnsFor(ids.back()) && transferred.cachedTables.empty() && transferred.cachedTableIndex.empty(),
              "switching to plain text must discard previous table index");
    }
    // Preserve the full-layout behavior of aggregating repeated IDs across groups.
    rows[3].tableId = ids[0];
    Model::TableColumnIndex mergedIndex;
    const auto merged = Model::buildTableColumns(text, rows, "monospace", 16, 800, 0, -1, &mergedIndex);
    check(merged.size() == ids.size() && Model::findTableColumns(merged, mergedIndex, ids[0]) == &merged[0],
          "repeated non-contiguous IDs must retain full-layout aggregation");
    return failures ? 1 : 0;
}
