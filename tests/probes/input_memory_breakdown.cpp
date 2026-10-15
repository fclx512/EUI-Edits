// Parameterised headless capacity inventory; not process memory or GUI acceptance.
#include "components/input.h"
#include "model/source_decorations.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_set>

using Model = components::input_detail::InputModel;
using Bytes = unsigned long long;
Bytes heapString(const std::string& s) {
    // MSVC x64 Release std::string has 15-byte inline storage. Inline heads are
    // counted in enclosing sizeof(), not again as dynamic allocations.
    return s.capacity() > 15 ? s.capacity() + 1 : 0;
}
template<class T> Bytes arrayBytes(const std::vector<T>& v) { return v.capacity() * sizeof(T); }

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) { std::cerr << "usage: input_memory_breakdown <text-file> <plain|source> <viewport|full>\n"; return 2; }
    std::ifstream file(std::filesystem::path(argv[1]), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    if (!file || text.empty()) return 2;
    const bool source = std::wstring(argv[2]) == L"source";
    const bool viewport = std::wstring(argv[3]) == L"viewport";
    if ((!source && std::wstring(argv[2]) != L"plain") ||
        (!viewport && std::wstring(argv[3]) != L"full")) return 2;
    core::dsl::Ui ui;
    const neo::EditorColors colors{};
    for (int frame = 0; frame < 3; ++frame) {
        ui.begin("memory-inventory");
        auto& s = ui.state<Model::InputState>("editor"); s.followCaret = false;
        auto builder = components::input(ui, "editor");
        builder.size(800, 500).valueRef(text).multiline().wordWrap(false)
            .viewportMetrics(viewport).lineNumbers(true).fontFamily("monospace")
            .fontSize(16).inset(8).transition(core::Transition::none());
        if (source) builder.lineDecorationSnapshot([&](const std::string& value,
            const components::input_detail::DecoratorEditInfo& info) {
                return neo::source::snapshot(value, "c", colors, 0, info);
            });
        builder.build(); ui.end(); ui.layout(800, 500);
    }
    const auto& s = ui.state<Model::InputState>("editor");
    if (s.text != text || s.cachedLines.empty()) return 1;
    std::map<std::string, Bytes> b;
    b["fixture_text_heap"] = heapString(text);
    b["input_text_heap"] = heapString(s.text);
    b["layout_text_heap"] = heapString(s.cachedLayoutText);
    b["line_headers"] = arrayBytes(s.cachedLines);
    b["line_geometry"] = s.cachedGeometry.residentCapacityBytes();
    b["detail_row_index"] = arrayBytes(s.detailedRows);
    Bytes detailed = 0;
    std::unordered_set<const void*> rareRecords, styles;
    for (const auto& row : s.cachedLines) {
        if (row.hasPresentation()) b["presentation_headers"] += sizeof(Model::TextLinePresentation);
        if (row.metrics.hasDetails()) b["metrics_detail_headers"] += sizeof(Model::TextLineMetrics::Details);
        b["caret_arrays"] += arrayBytes(row.metrics.byteIndices()) + arrayBytes(row.metrics.caretX());
        detailed += !row.metricsDeferred;
        b["line_run_arrays"] += arrayBytes(row.presentation().runs);
        b["line_aux_arrays"] += arrayBytes(row.presentation().holes) + arrayBytes(row.rare().tableDocCaretStops)
            + arrayBytes(row.rare().tableCellRanges);
        b["line_string_heaps"] += heapString(row.rare().fontFamily) + heapString(row.rare().imagePath) +
            heapString(row.rare().imageFailText) + heapString(row.presentation().glyph.text) + heapString(row.rare().gutterGlyph.text)
            + heapString(row.rare().languageLabel);
        for (const auto& run : row.presentation().runs)
            if (run.style.identity() && styles.insert(run.style.identity()).second)
                b["line_style_records_approx"] += run.style.residentCapacityBytes();
        if (row.hasRare() && rareRecords.insert(row.rareIdentity()).second)
            b["rare_record_headers_approx"] += sizeof(Model::TextLineRare) + 2 * sizeof(void*);
    }
    if (source) {
        const auto& c = neo::source::cache();
        b["source_row_headers"] = arrayBytes(c.rows);
        b["source_start_offsets"] = arrayBytes(c.starts);
        for (const auto& row : c.rows) {
            b["source_token_arrays"] += arrayBytes(row.tokens);
        }
        // The shared generation has one owner allocation; do not count it in
        // both the source cache and InputState.
        b["shared_decorations_approx"] = c.snapshot ? c.snapshot->residentCapacityBytes() : 0;
    }
    Bytes total = 0;
    std::cout << "{\"schema\":\"input-capacity/1\",\"file_bytes\":" << text.size()
        << ",\"source\":" << (source ? "true" : "false")
        << ",\"viewport\":" << (viewport ? "true" : "false")
        << ",\"line_sizeof\":" << sizeof(Model::TextLine)
        << ",\"decoration_sizeof\":" << sizeof(components::input_detail::LineDecoration)
        << ",\"line_run_sizeof\":" << sizeof(components::input_detail::LineRun)
        << ",\"text_run_sizeof\":" << sizeof(components::input_detail::TextRun)
        << ",\"line_size\":" << s.cachedLines.size() << ",\"line_capacity\":" << s.cachedLines.capacity()
        << ",\"detailed_rows\":" << detailed << ",\"capacities\":{";
    bool first = true;
    for (const auto& entry : b) {
        if (!first) std::cout << ','; first = false;
        std::cout << '"' << entry.first << "\":" << entry.second; total += entry.second;
    }
    std::cout << "},\"accounted_capacity_bytes\":" << total << "}\n";
    // Excludes allocator overhead, freed-but-retained heap, runtime/render/font
    // caches, other app state and native windows. Never sum with EX2 counters.
    return 0;
}
