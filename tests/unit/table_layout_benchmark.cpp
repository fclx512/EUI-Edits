// CPU-only, repeated short cells isolate table-count lookup scaling from shaping.
// Compare the same binary inputs before/after; these are not GUI latency results.
#include "components/input_model.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using Model = components::input_detail::InputModel;
using Decoration = components::input_detail::LineDecoration;
using Clock = std::chrono::steady_clock;

// Exact pre-index lookup as a reference, separate from real-layout timings.
const Model::TableColumns* linearFind(const std::vector<Model::TableColumns>& tables, int id) {
    for (const auto& table : tables) if (table.tableId == id) return &table;
    return nullptr;
}

int lookupOnly(int rows, int repeats) {
    std::vector<Model::TableColumns> tables;
    Model::TableColumnIndex index;
    for (int t = 0; t < rows / 4; ++t) {
        Model::TableColumns table;
        table.tableId = 1000000000 - t * 37;
        table.total = static_cast<float>(t % 31 + 1);
        index.emplace(table.tableId, tables.size());
        tables.push_back(std::move(table));
    }
    for (int repeat = 0; repeat < repeats; ++repeat) {
        for (bool indexed : {false, true}) {
            double checksum = 0;
            const auto begin = Clock::now();
            for (const auto& row : tables) for (int query = 0; query < 3; ++query) {
                const auto* table = indexed ? Model::findTableColumns(tables, index, row.tableId)
                                            : linearFind(tables, row.tableId);
                if (!table) return 1;
                checksum += table->total;
            }
            const auto end = Clock::now();
            std::cout << "[lookupbench] tables=" << tables.size() << " indexed=" << indexed
                      << " repeat=" << repeat << " queries=" << tables.size() * 3
                      << " ms=" << std::chrono::duration<double, std::milli>(end - begin).count()
                      << " checksum=" << checksum << '\n';
        }
    }
    return 0;
}

struct Sample {
    std::string text;
    std::vector<Decoration> decorations;
    int tables = 0;
    int cellRows = 0;
    int tableRows = 0;
};

Sample sample(int rows, const std::string& kind) {
    Sample out;
    for (int row = 0; row < rows; ++row) {
        const int part = kind == "single" ? row : row % 4;
        const bool separator = part == 1;
        const bool blank = kind == "single" ? row == rows - 1 : part == 3;
        const int beg = static_cast<int>(out.text.size());
        out.text += blank ? "\n" : separator ? "| --- | ---: |\n" : "| alpha | 12345 |\n";
        Decoration dec;
        if (!blank && kind != "none") {
            // Deliberately sparse, descending IDs: no ordering/dense-ID assumption.
            dec.tableId = kind == "single" ? 1234567 : 1000000000 - (row / 4) * 37;
            dec.fontSize = 16;
            dec.lineHeight = separator ? 1 : 28;
            dec.tableHeaderRow = part == 0;
            dec.tableSeparator = separator;
            dec.cellPadding = 6;
            ++out.tableRows;
            if (part == 0) ++out.tables;
            if (separator) {
                dec.holes = {{beg, static_cast<int>(out.text.size()) - 1}};
            } else {
                dec.cells = {{beg + 2, beg + 7}, {beg + 10, beg + 15}};
                dec.holes = {{beg, beg + 2}, {beg + 7, beg + 10}, {beg + 15, beg + 17}};
                ++out.cellRows;
            }
        }
        out.decorations.push_back(std::move(dec));
    }
    out.decorations.emplace_back(); // trailing newline source row
    return out;
}

int resizeOnly(int rows, int repeats) {
    for (const std::string kind : {"many", "single", "none"}) {
        const auto doc = sample(rows, kind);
        for (int repeat = 0; repeat < repeats; ++repeat) {
            double reference = 0;
            for (bool reuse : {false, true}) {
                Model::InputState state;
                state.text = doc.text; state.textRevision = 1;
                Model::ensureLayoutCache(state, "monospace", 16, 800, true, &doc.decorations);
                Model::debugLayoutStats() = {};
                double elapsed = 0, checksum = 0;
                for (float width : {500.f, 900.f, 600.f, 800.f}) {
                    if (!reuse) state.layoutCacheValid = false;
                    const auto begin = Clock::now();
                    Model::ensureLayoutCache(state, "monospace", 16, width, true, &doc.decorations);
                    elapsed += std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
                    checksum += state.cachedLines.size();
                    for (const auto& table : state.cachedTables) checksum += table.total;
                    for (const auto& line : state.cachedLines) checksum += line.metrics.width;
                }
                if (!reuse) reference = checksum;
                else if (checksum != reference) return 1;
                size_t rawBytes = state.cachedTableIntrinsic.capacity() * sizeof(Model::TableColumns);
                for (const auto& table : state.cachedTableIntrinsic)
                    rawBytes += (table.x.capacity() + table.width.capacity()) * sizeof(float);
                const auto stats = Model::debugLayoutStats();
                std::cout << "[tableresize] kind=" << kind << " rows=" << rows << " repeat=" << repeat
                          << " reuse=" << reuse << " resizes=4 ms=" << elapsed
                          << " intrinsic_built=" << stats.tableIntrinsicBuilt
                          << " intrinsic_reused=" << stats.tableIntrinsicReused
                          << " raw_capacity_bytes=" << rawBytes << " checksum=" << checksum << '\n';
            }
        }
    }
    return 0;
}

// Edit before every table, then resize: includes byte/line identity translation.
int editResize(int rows, int repeats) {
    for (const std::string kind : {"many", "single", "none"}) {
        const auto doc = sample(rows, kind);
        auto edited = doc;
        edited.text.insert(0, "prefix\n");
        for (auto& row : edited.decorations) {
            if (row.tableId >= 0) row.tableId += 7;
            for (auto& hole : row.holes) { hole.beg += 7; hole.end += 7; }
            for (auto& cell : row.cells) { cell.beg += 7; cell.end += 7; }
        }
        edited.decorations.insert(edited.decorations.begin(), Decoration{});
        for (int repeat = 0; repeat < repeats; ++repeat) {
            Model::InputState state;
            state.text = doc.text; state.textRevision = 1;
            Model::ensureLayoutCache(state, "monospace", 16, 800, true, &doc.decorations);
            state.text = edited.text; ++state.textRevision;
            Model::debugLayoutStats() = {};
            const auto start = Clock::now();
            Model::ensureLayoutCache(state, "monospace", 16, 800, true, &edited.decorations);
            const auto afterEdit = Clock::now();
            const auto editStats = Model::debugLayoutStats();
            Model::ensureLayoutCache(state, "monospace", 16, 500, true, &edited.decorations);
            const auto end = Clock::now();
            const auto stats = Model::debugLayoutStats();
            double checksum = state.cachedLines.size();
            for (const auto& table : state.cachedTables) checksum += table.total;
            for (const auto& line : state.cachedLines) checksum += line.metrics.width;
            std::cout << "[tableeditresize] kind=" << kind << " rows=" << rows << " repeat=" << repeat
                      << " edit_ms=" << std::chrono::duration<double, std::milli>(afterEdit - start).count()
                      << " resize_ms=" << std::chrono::duration<double, std::milli>(end - afterEdit).count()
                      << " edit_full=" << editStats.full << " edit_incremental=" << editStats.incremental
                      << " intrinsic_built=" << stats.tableIntrinsicBuilt
                      << " intrinsic_reused=" << stats.tableIntrinsicReused << " checksum=" << checksum << '\n';
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 3;
    const int requested = argc > 1 ? std::atoi(argv[1]) : 0;
    if ((requested != 0 && (requested < 4 || requested % 4)) || repeats < 1 || repeats > 20) return 2;
    const std::vector<int> sizes = requested ? std::vector<int>{requested} : std::vector<int>{2000, 20000, 100000};
    std::cout << std::fixed << std::setprecision(4);
    if (argc > 3 && std::string(argv[3]) == "edit-resize") {
        for (int rows : sizes) if (editResize(rows, repeats)) return 1;
        return 0;
    }
    if (argc > 3 && std::string(argv[3]) == "resize") {
        for (int rows : sizes) if (resizeOnly(rows, repeats)) return 1;
        return 0;
    }
    if (argc > 3 && std::string(argv[3]) == "lookup") {
        for (int rows : sizes) if (lookupOnly(rows, repeats)) return 1;
        return 0;
    }
    for (int rows : sizes) for (const std::string kind : {"many", "single", "none"}) {
        const auto doc = sample(rows, kind);
        // warm font/shaping caches before collecting samples
        auto warm = Model::measureLines(doc.text, "monospace", 16, 800, &doc.decorations);
        for (int repeat = 0; repeat < repeats; ++repeat) {
            auto begin = Clock::now();
            auto columns = Model::buildTableColumns(doc.text, doc.decorations, "monospace", 16, 800);
            auto columnsDone = Clock::now();
            std::vector<Model::TableColumns> saved;
            auto lines = Model::measureLines(doc.text, "monospace", 16, 800, &doc.decorations, &saved);
            auto fullDone = Clock::now();
            if (static_cast<int>(columns.size()) != doc.tables || saved.size() != columns.size() || lines.size() != doc.decorations.size()) return 1;
            std::cout << "[tablebench] kind=" << kind << " rows=" << rows << " tables=" << doc.tables
                      << " repeat=" << repeat << " build_queries=" << doc.cellRows << " row_queries=" << doc.tableRows
                      << " columns_ms=" << std::chrono::duration<double, std::milli>(columnsDone - begin).count()
                      << " full_ms=" << std::chrono::duration<double, std::milli>(fullDone - columnsDone).count() << '\n';
        }
    }
}
