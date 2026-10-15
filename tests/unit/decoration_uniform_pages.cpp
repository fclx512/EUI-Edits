#include "components/input_model.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace components::input_detail;
using Decoration = LineDecoration;
using Table = LineDecorationTable;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool exactRow(const Decoration& a, const Decoration& b) {
    // Reuse the public semantic equality for non-geometric fields, then compare
    // every field whose legacy equality intentionally allows a subpixel delta.
    return a == b && a.fontSize == b.fontSize && a.lineHeight == b.lineHeight &&
        a.contentIndent == b.contentIndent && a.textShiftY == b.textShiftY &&
        a.spaceBefore == b.spaceBefore && a.rare().imageWidth == b.rare().imageWidth &&
        a.rare().imageHeight == b.rare().imageHeight && a.cellPadding == b.cellPadding &&
        a.glyph.advance == b.glyph.advance && a.rare().gutterGlyph.advance == b.rare().gutterGlyph.advance &&
        a.rare().box.backgroundRadius == b.rare().box.backgroundRadius && a.rare().box.barWidth == b.rare().box.barWidth;
}

std::vector<Decoration> uniformRows(std::size_t count) {
    return std::vector<Decoration>(count);
}

void checkAllRows(const Table& table, const std::vector<Decoration>& expected, const char* label) {
    check(table.size() == expected.size(), std::string(label) + ": size is preserved");
    if (table.size() != expected.size()) return;
    const auto copied = table.copyRows();
    check(copied.size() == expected.size(), std::string(label) + ": copyRows size is preserved");
    for (std::size_t row = 0; row < expected.size(); ++row) {
        if (!exactRow(table[row], expected[row])) {
            check(false, std::string(label) + ": operator[] differs at row " + std::to_string(row));
        }
        if (!exactRow(copied[row], expected[row])) {
            check(false, std::string(label) + ": copyRows differs at row " + std::to_string(row));
        }
    }
}

void testUniformCompressionAndAccess() {
    constexpr std::size_t rowsCount = Table::pageSize * 256;
    const auto rows = uniformRows(rowsCount);
    const Table table(rows);
    const std::uint64_t compressedBytes = table.residentCapacityBytes();
    const std::uint64_t uncompressedRowBytes = rowsCount * sizeof(Decoration);
    check(table.contiguousRows() == nullptr, "uniform table is represented by compressed pages");
    check(compressedBytes < uncompressedRowBytes / 8,
          "large ordinary same-shape table stores substantially fewer decoration rows");
    checkAllRows(table, rows, "uniform pages");
}

void testQuarterPageThreshold() {
    constexpr std::size_t pageCount = 64;
    constexpr std::size_t count = pageCount * Table::pageSize;
    std::vector<Decoration> rows(count);
    for (std::size_t page = pageCount / 4; page < pageCount; ++page) {
        for (std::size_t offset = 0; offset < Table::pageSize; ++offset) {
            Decoration& row = rows[page * Table::pageSize + offset];
            row.editRare().fontFamily = "dense-" + std::to_string(page) + "-" + std::to_string(offset);
        }
    }
    const Table table(std::move(rows));
    check(table.contiguousRows() == nullptr,
          "exactly one quarter uniform pages is enough to select page storage");
    check(table[0].rare().fontFamily.empty() && table[Table::pageSize / 2].rare().fontFamily.empty(),
          "uniform pages survive threshold packing");
    check(table[pageCount / 4 * Table::pageSize].rare().fontFamily == "dense-16-0" &&
          table[count - 1].rare().fontFamily == "dense-63-63",
          "nonuniform pages retain per-row unique values");
}

void testSparsePayloadPreservation() {
    constexpr std::size_t count = Table::pageSize * 128;
    auto rows = uniformRows(count);
    const std::size_t holesRow = Table::pageSize * 17;
    const std::size_t runsRow = Table::pageSize * 19;
    const std::size_t cellsRow = Table::pageSize * 21;
    const std::size_t imageRow = Table::pageSize * 23;
    const std::size_t foldedRow = Table::pageSize * 25;
    rows[holesRow].holes = {{static_cast<int>(holesRow * 3 + 1), static_cast<int>(holesRow * 3 + 4)}};
    LineRun run;
    run.beg = static_cast<int>(runsRow * 3 + 5);
    run.end = run.beg + 4;
    run.style.edit().weight = 700;
    run.style.edit().fontFamily = "Mono Sparse";
    rows[runsRow].runs.push_back(run);
    rows[cellsRow].tableId = 41;
    rows[cellsRow].cells = {{static_cast<int>(cellsRow * 3), static_cast<int>(cellsRow * 3 + 5), 1}};
    rows[imageRow].editRare().imagePath = "images/sparse.png";
    rows[imageRow].editRare().imageWidth = 42.5f;
    rows[imageRow].editRare().imageHeight = 24.25f;
    rows[foldedRow].hidden = true;
    rows[foldedRow].hiddenByFold = true;

    const Table table(rows);
    check(table.contiguousRows() == nullptr, "sparse payload table still compresses its uniform pages");
    check(table[holesRow].holes == rows[holesRow].holes, "holes survive in their nonuniform page");
    check(table[runsRow].runs == rows[runsRow].runs, "runs survive in their nonuniform page");
    check(table[cellsRow].cells == rows[cellsRow].cells && table[cellsRow].tableId == 41,
          "table cells and identity survive a nonuniform page");
    check(table[imageRow].rare().imagePath == rows[imageRow].rare().imagePath &&
          table[imageRow].rare().imageWidth == rows[imageRow].rare().imageWidth &&
          table[imageRow].rare().imageHeight == rows[imageRow].rare().imageHeight,
          "image metadata survives in its nonuniform page");
    check(table[foldedRow].hidden && table[foldedRow].hiddenByFold,
          "fold-hidden metadata survives in its nonuniform page");
    checkAllRows(table, rows, "sparse payload");
}

using Mutator = std::function<void(Decoration&)>;
using Getter = std::function<float(const Decoration&)>;

void testExactGeometryEquality() {
    const std::vector<std::pair<std::string, std::pair<Mutator, Getter>>> geometricFields = {
        {"fontSize", {[](Decoration& d) { d.fontSize = 0.0005f; }, [](const Decoration& d) { return d.fontSize; }}},
        {"lineHeight", {[](Decoration& d) { d.lineHeight = 0.0005f; }, [](const Decoration& d) { return d.lineHeight; }}},
        {"contentIndent", {[](Decoration& d) { d.contentIndent = 0.0005f; }, [](const Decoration& d) { return d.contentIndent; }}},
        {"textShiftY", {[](Decoration& d) { d.textShiftY = 0.0005f; }, [](const Decoration& d) { return d.textShiftY; }}},
        {"spaceBefore", {[](Decoration& d) { d.spaceBefore = 0.0005f; }, [](const Decoration& d) { return d.spaceBefore; }}},
        {"imageWidth", {[](Decoration& d) { d.editRare().imageWidth = 0.0005f; }, [](const Decoration& d) { return d.rare().imageWidth; }}},
        {"imageHeight", {[](Decoration& d) { d.editRare().imageHeight = 0.0005f; }, [](const Decoration& d) { return d.rare().imageHeight; }}},
        {"cellPadding", {[](Decoration& d) { d.cellPadding = 0.0005f; }, [](const Decoration& d) { return d.cellPadding; }}},
        {"glyph.advance", {[](Decoration& d) { d.glyph.advance = 0.0005f; }, [](const Decoration& d) { return d.glyph.advance; }}},
        {"gutterGlyph.advance", {[](Decoration& d) { d.editRare().gutterGlyph.advance = 0.0005f; }, [](const Decoration& d) { return d.rare().gutterGlyph.advance; }}},
        {"box.backgroundRadius", {[](Decoration& d) { d.editRare().box.backgroundRadius = 0.0005f; }, [](const Decoration& d) { return d.rare().box.backgroundRadius; }}},
        {"box.barWidth", {[](Decoration& d) { d.editRare().box.barWidth = 0.0005f; }, [](const Decoration& d) { return d.rare().box.barWidth; }}},
    };
    constexpr std::size_t count = Table::pageSize * 64;
    for (const auto& field : geometricFields) {
        std::vector<Decoration> rows(count);
        field.second.first(rows[1]);
        check(rows[0] == rows[1], field.first + ": legacy equality treats 0.0005 delta as equivalent");
        const Table table(std::move(rows));
        const float got = field.second.second(table[1]);
        check(got == 0.0005f,
              field.first + ": exact small geometry delta survives page packing");
        check(table.contiguousRows() == nullptr,
              field.first + ": other uniform pages still activate compression");
    }
}

void testReplacingGenerationsAndTailPage() {
    constexpr std::size_t count = Table::pageSize * 64 + Table::pageSize + 11;
    const auto sourceRows = uniformRows(count);
    const auto source = std::make_shared<const Table>(sourceRows);
    check(source->size() % Table::pageSize == 11, "fixture has a short tail page");

    Decoration atFirst;
    atFirst.fontSize = 11.0f;
    Decoration atPageEdge;
    atPageEdge.fontSize = 12.0f;
    Decoration atSecondPage;
    atSecondPage.fontSize = 13.0f;
    Decoration atLastFullPage;
    atLastFullPage.fontSize = 14.0f;
    Decoration atTail;
    atTail.fontSize = 15.0f;
    std::vector<Decoration> firstPatches{atFirst, atPageEdge, atSecondPage, atLastFullPage, atTail};
    unsigned long long copiedRows = 0;
    const std::vector<int> firstIndices{0, 63, 64, static_cast<int>(count - 12), static_cast<int>(count - 1)};
    const auto generation1 = source->replacing(firstIndices, firstPatches, &copiedRows);
    check(generation1 != nullptr, "first replacement generation is created");
    check(source->operator[](0).fontSize == 0.0f && source->operator[](count - 1).fontSize == 0.0f,
          "publishing a generation leaves its source snapshot unchanged");
    check(generation1 && generation1->operator[](0).fontSize == 11.0f &&
          generation1->operator[](63).fontSize == 12.0f &&
          generation1->operator[](64).fontSize == 13.0f &&
          generation1->operator[](count - 12).fontSize == 14.0f &&
          generation1->operator[](count - 1).fontSize == 15.0f,
          "first, last full, and short-tail page replacements preserve each patch");
    check(copiedRows == 62 + 63 + 63 + 10,
          "replacement copies only untouched rows inside touched full and short pages");
    if (generation1) {
        check(&(*source)[128] == &(*generation1)[128], "untouched compressed page is shared by generation");
        Decoration secondPatch;
        secondPatch.hidden = true;
        secondPatch.hiddenByFold = true;
        const auto generation2 = generation1->replacing({100}, {secondPatch});
        check(generation2 && generation2->operator[](100).hiddenByFold,
              "second generation contains its independent patch");
        check(generation1->operator[](100).hiddenByFold == false &&
              source->operator[](100).hiddenByFold == false,
              "later generations do not mutate earlier snapshots");
        if (generation2) {
            Decoration thirdPatch;
            thirdPatch.spaceBefore = 3.5f;
            const auto generation3 = generation2->replacing({count - 2}, {thirdPatch});
            check(generation3 && generation3->operator[](count - 2).spaceBefore == 3.5f,
                  "third generation can patch the short tail page");
            check(generation2->operator[](count - 2).spaceBefore == 0.0f &&
                  generation1->operator[](count - 2).spaceBefore == 0.0f,
                  "tail patch preserves all earlier immutable generations");
        }
    }
}

void testDenseUniqueKeepsBase() {
    constexpr std::size_t count = Table::pageSize * 96;
    std::vector<Decoration> rows;
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Decoration row;
        row.editRare().fontFamily = "unique-font-" + std::to_string(i);
        row.editRare().imagePath = "unique-image-" + std::to_string(i);
        rows.push_back(std::move(row));
    }
    const Table table(rows);
    check(table.contiguousRows() != nullptr,
          "dense all-unique table retains the original contiguous base representation");
    check(table.residentCapacityBytes() < count * (sizeof(Decoration) + sizeof(LineDecorationRare) + 2 * sizeof(void*) + 256u),
          "dense unique table does not allocate a second page copy of every row");
    check(table[0].rare().fontFamily == "unique-font-0" &&
          table[count / 2].rare().fontFamily == "unique-font-" + std::to_string(count / 2) &&
          table[count - 1].rare().imagePath == "unique-image-" + std::to_string(count - 1),
          "base-backed dense table retains unique payloads");
}

} // namespace

void testCompactValueIsolation() {
    LineRunStyle original;
    original.weight = 700;
    original.fontFamily = "exact-style";
    original.color = {0.2f, 0.3f, 0.4f, 1.0f};
    LineStyleHandle first(original), same(original);
    check(first.identity() == same.identity(), "exact style values share one record");
    LineRunStyle nearby = original;
    nearby.color.r += 0.00001f;
    LineStyleHandle distinct(nearby);
    check(distinct.identity() != first.identity() && distinct.get().color.r == nearby.color.r,
          "interner never rounds close but distinct colors");
    auto edited = first;
    edited.edit().weight = 400;
    edited.edit().fontFamily = "changed";
    check(first.get().weight == 700 && same.get().fontFamily == "exact-style",
          "editing a copied run preserves retained style values");
    Decoration rich;
    rich.editRare().imagePath = "kept-image";
    rich.editRare().box.backgroundRadius = 8;
    LineRun run;run.beg = 1;run.end = 5;run.style = first;rich.runs.push_back(run);
    const auto table = std::make_shared<const Table>(std::vector<Decoration>{rich, rich});
    auto replacement = (*table)[0];
    replacement.editRare().imagePath = "new-image";
    replacement.runs[0].style.edit().color.r = 0.9f;
    auto next = table->replacing({0}, {replacement});
    check(next && (*table)[0].rare().imagePath == "kept-image" &&
          (*next)[1].rare().imagePath == "kept-image" &&
          (*table)[0].runs[0].style.get().color.r == 0.2f &&
          (*next)[1].runs[0].style.get().color.r == 0.2f,
          "page patches detach rich metadata and style without mutating old or sibling rows");
    std::vector<LineStyleHandle> retained;
    for (int i = 0; i < 300; ++i) {
        LineRunStyle value;value.weight = 1000 + i;retained.emplace_back(value);
    }
    check(retained.front().get().weight == 1000 && retained.back().get().weight == 1299,
          "bounded dictionary eviction never invalidates live styles");
}

int main() {
    testCompactValueIsolation();
    testUniformCompressionAndAccess();
    testQuarterPageThreshold();
    testSparsePayloadPreservation();
    testExactGeometryEquality();
    testReplacingGenerationsAndTailPage();
    testDenseUniqueKeepsBase();
    std::cout << (failures == 0 ? "PASS" : "FAIL")
              << ": decoration_uniform_pages (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
