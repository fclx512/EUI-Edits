#include "model/i18n.h"
#include "model/obsidian_theme.h"
#include "model/text_file.h"
#include "model/theme_loader.h"
#include "model/theme_workbench.h"
#include "model/theme_auto_palette.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <tuple>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool same(eui::Color a, eui::Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool writeText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    return output.good();
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

eui::Color byteColor(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
    return eui::Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}

eui::Color parseRgba(const std::string& value) {
    if (value.size() != 9 || value[0] != '#') return {};
    const auto channel = [&](std::size_t offset) {
        return static_cast<unsigned>(std::stoul(value.substr(offset, 2), nullptr, 16));
    };
    return byteColor(channel(1), channel(3), channel(5), channel(7));
}

eui::Color composite(eui::Color foreground, eui::Color background) {
    const float a = std::clamp(foreground.a, 0.0f, 1.0f);
    return eui::Color{foreground.r * a + background.r * (1.0f - a),
                      foreground.g * a + background.g * (1.0f - a),
                      foreground.b * a + background.b * (1.0f - a), 1.0f};
}

double luminance(eui::Color color) {
    const auto linear = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
}

double contrastRatio(eui::Color foreground, eui::Color background) {
    const double a = luminance(composite(foreground, background));
    const double b = luminance(background);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

void checkGeneratedContrast(const neo::EditorColors& colors, const std::string& context) {
    const auto require = [&](const std::string& name, eui::Color foreground, eui::Color background) {
        const double ratio = contrastRatio(foreground, background);
        check(ratio >= 4.5, context + " contrast " + name + " >= 4.5 (actual " +
                             std::to_string(ratio) + ")");
    };
    for (const char* surface : {"window", "panel", "toolbar", "editor", "statusBar"}) {
        const auto bg = colors.*neo::findThemeColorField(surface)->member;
        require(std::string("text/") + surface, colors.text, bg);
        require(std::string("textMuted/") + surface, colors.textMuted, bg);
    }
    require("heading/editor", colors.heading, colors.editor);
    require("markdownAccent/editor", colors.markdownAccent, colors.editor);
    require("text/selection", colors.text,
            composite(colors.selectionBackground, colors.editor));
    for (const char* token : {"codeText", "tokenKeyword", "tokenString", "tokenNumber",
                              "tokenComment", "tokenOperator", "tokenFunction", "tokenProperty"})
        require(std::string(token) + "/codeBackground", colors.*neo::findThemeColorField(token)->member,
                colors.codeBackground);
    for (const char* state : {"rowHover", "rowActive", "pressed"})
        require(std::string("text/") + state, colors.text,
                composite(colors.*neo::findThemeColorField(state)->member, colors.panel));
}

const neo::ThemeImportEntry* reportEntry(const neo::ThemeFileData& data,
                                         bool light, const std::string& field) {
    for (const auto& entry : data.importReport) {
        if (entry.light == light && entry.field == field) return &entry;
    }
    return nullptr;
}

void testCssReportAndScope() {
    const std::string css =
        ".theme-light { --text-normal: #112233; --text-selection: rgba(20, 40, 60, .4);"
        " --line-height-normal: 2; --icon-color: #abcdef;"
        " --text-muted: var(--not-defined); --background-modifier-border: calc(1 + 2);"
        " --code-background: #ggg; }"
        ".theme-dark { --text-normal: #445566; --line-height-normal: 1.8;"
        " --icon-color: #fedcba; }";
    neo::ThemeFileData data;
    std::string error;
    check(neo::obsidiantheme::parse(css, "workbench fixture", data, error),
          "CSS fixture parses: " + error);
    if (data.version == 0) return;

    check(data.importReport.size() == neo::themeworkbench::fields().size() * 2,
          "CSS report has one entry per workbench color field and mode");
    const auto* selected = reportEntry(data, true, "selectionBackground");
    check(selected && selected->imported && selected->reason == "读取成功" &&
              selected->source == "--text-selection",
          "selectionBackground report identifies its imported CSS source");
    check(data.light.colors.count("selectionBackground") == 1 &&
              same(data.light.colors.at("selectionBackground"), eui::Color{20 / 255.0f,
                  40 / 255.0f, 60 / 255.0f, .4f}),
          "--text-selection imports an independent RGBA selection color");
    const auto* missing = reportEntry(data, true, "accent");
    check(missing && !missing->imported && !missing->reason.empty(),
          "unmapped accent gets a per-field fallback reason");
    const auto* absentVariable = reportEntry(data, true, "panel");
    check(absentVariable && absentVariable->reason == "未提供对应变量",
          "missing mapped variable has a specific report reason");
    const auto* unresolved = reportEntry(data, true, "textMuted");
    check(unresolved && unresolved->source == "--text-muted" &&
              unresolved->reason == "变量引用无法解析",
          "unresolved CSS variable is reported separately");
    const auto* unsupported = reportEntry(data, true, "border");
    check(unsupported && unsupported->source == "--background-modifier-border" &&
              unsupported->reason == "不支持的颜色表达式",
          "unsupported CSS expression is reported separately");
    const auto* invalid = reportEntry(data, true, "codeBackground");
    check(invalid && invalid->source == "--code-background" && invalid->reason == "颜色值无效",
          "malformed color is reported separately from unsupported syntax");

    check(!data.light.typography.bodyLineHeight && !data.dark.typography.bodyLineHeight,
          "CSS import does not import typography");
    for (const char* icon : {"iconFolder", "iconFile", "iconMd", "iconTxt", "iconCode", "iconData"}) {
        check(data.light.colors.count(icon) == 0 && data.dark.colors.count(icon) == 0,
              std::string("CSS import leaves fixed icon field unoverridden: ") + icon);
    }

    neo::ThemeFileData lightOnly;
    check(neo::obsidiantheme::parse(".theme-light { --text-normal: #112233; }",
                                    "light-only fixture", lightOnly, error),
          "light-only CSS fixture parses: " + error);
    const auto* absentSide = reportEntry(lightOnly, false, "selectionBackground");
    check(absentSide && !absentSide->imported && absentSide->reason == "未提供该明暗模式",
          "missing dark side is distinguished from a missing declaration");
    neo::ThemeFileData rootOnly;
    check(neo::obsidiantheme::parse(":root { --text-normal: #abcdef; }", "root-only", rootOnly, error),
          "global-only CSS colors are valid for both modes");
    check(rootOnly.light.colors.count("text") && rootOnly.dark.colors.count("text") &&
          rootOnly.importReport.size() == 52, "root-only import reports both palettes completely");
}

void testReadAndDraftIsolation(const fs::path& root) {
    const eui::Color activeWindow{.9f, .1f, .2f, 1.0f};
    neo::ThemeFileData active;
    active.version = 2;
    active.dark.colors["window"] = activeWindow;
    neo::themeloader::activate(active, "memory:read-isolation");
    const auto activeRevision = neo::activeTheme().revision;
    const auto activePath = neo::activeTheme().path;

    const fs::path source = root / "read-source.json";
    check(writeText(source, R"({"version":2,"dark":{"colors":{"window":"#123456"}}})"),
          "write read-only source fixture");
    neo::ThemeFileData readData;
    std::string error;
    check(neo::themeloader::read(neo::textfile::pathToUtf8(source), readData, error),
          "read validates a valid theme: " + error);
    check(neo::activeTheme().revision == activeRevision && neo::activeTheme().path == activePath &&
              same(neo::activeTheme().dark.colors.at("window"), activeWindow),
          "successful read does not activate or mutate the active theme");

    neo::ThemeFileData failedData;
    check(!neo::themeloader::read(neo::textfile::pathToUtf8(root / "missing.json"),
                                  failedData, error) && !error.empty(),
          "read reports a missing file");
    check(neo::activeTheme().revision == activeRevision && neo::activeTheme().path == activePath,
          "failed read does not activate or mutate the active theme");

    neo::ThemeFileData partial;
    check(neo::themeloader::parse(
              R"({"version":2,"dark":{"colors":{"window":"#123456"}}})", partial, error),
          "parse partial theme for draft isolation: " + error);
    auto draft = neo::themeworkbench::makeDraft(partial);
    check(draft.current.dark.colors.size() == std::size(neo::kThemeColorFields) &&
              draft.current.light.colors.size() == std::size(neo::kThemeColorFields),
          "draft materializes the full color palette for both modes");
    check(same(neo::themeworkbench::colors(draft, false).window,
               eui::Color{0x12 / 255.0f, 0x34 / 255.0f, 0x56 / 255.0f, 1.0f}),
          "draft retains the explicit dark override");
    check(same(neo::themeworkbench::colors(draft, true).window, neo::makeColors(true).window),
          "draft missing fields fall back to built-in light values, not active theme");
}

void testEditsUndoRestoreAndModeIsolation() {
    neo::ThemeFileData input;
    input.version = 2;
    auto draft = neo::themeworkbench::makeDraft(input);
    const eui::Color oldLightText = neo::themeworkbench::colors(draft, true).text;
    const eui::Color oldDarkText = neo::themeworkbench::colors(draft, false).text;
    const eui::Color oldLightBorder = neo::themeworkbench::colors(draft, true).border;
    const eui::Color edited = byteColor(28, 56, 84, 112);

    neo::themeworkbench::setColor(draft, true, "text", edited);
    check(neo::themeworkbench::changed(draft, true, "text") &&
              neo::themeworkbench::changedCount(draft, true) == 1,
          "RGBA text edit is detected as one changed light field");
    check(same(neo::themeworkbench::colors(draft, true).text, edited) &&
              same(neo::themeworkbench::colors(draft, false).text, oldDarkText) &&
              same(neo::themeworkbench::colors(draft, true).border, oldLightBorder),
          "edit changes only the selected field in the selected mode");
    check(neo::themeworkbench::undo(draft) &&
              same(neo::themeworkbench::colors(draft, true).text, oldLightText) &&
              !neo::themeworkbench::changed(draft, true, "text"),
          "undo restores the original RGBA value");
    check(neo::themeworkbench::redo(draft) &&
              same(neo::themeworkbench::colors(draft, true).text, edited),
          "redo restores the edited RGBA value");
    neo::themeworkbench::restoreColor(draft, true, "text");
    check(same(neo::themeworkbench::colors(draft, true).text, oldLightText) &&
              !neo::themeworkbench::changed(draft, true, "text"),
          "single-field restore returns exactly to baseline");
    check(neo::themeworkbench::undo(draft) &&
              same(neo::themeworkbench::colors(draft, true).text, edited) &&
              neo::themeworkbench::redo(draft) &&
              same(neo::themeworkbench::colors(draft, true).text, oldLightText),
          "restore operation participates in undo and redo history");

    const eui::Color alphaOnly{oldLightText.r, oldLightText.g, oldLightText.b,
                               oldLightText.a == 1.0f ? 0.5f : 1.0f};
    neo::themeworkbench::setColor(draft, true, "text", alphaOnly);
    check(neo::themeworkbench::changed(draft, true, "text") &&
              neo::themeworkbench::changedCount(draft, true) == 1 &&
              neo::themeworkbench::colors(draft, true).text.r == oldLightText.r &&
              neo::themeworkbench::colors(draft, true).text.g == oldLightText.g &&
              neo::themeworkbench::colors(draft, true).text.b == oldLightText.b,
          "alpha-only edit is counted as a changed field");
    const eui::Color lightBeforeDarkEdit = neo::themeworkbench::colors(draft, true).text;
    neo::themeworkbench::setColor(draft, false, "text", byteColor(204, 179, 153, 128));
    check(neo::themeworkbench::changed(draft, false, "text") &&
              neo::themeworkbench::changed(draft, true, "text") &&
              same(neo::themeworkbench::colors(draft, true).text, lightBeforeDarkEdit),
          "dark-mode edit leaves the independently edited light RGBA value unchanged");
}

void testContinuousEditUndoBoundary() {
    neo::ThemeFileData input;
    input.version = 2;
    input.light.typography.bodyLineHeight = 1.5f;
    input.dark.typography.bodyLineHeight = 1.8f;
    auto draft = neo::themeworkbench::makeDraft(input);
    const auto originalSelection = neo::themeworkbench::colors(draft, true).selectionBackground;
    const auto originalDarkSelection = neo::themeworkbench::colors(draft, false).selectionBackground;
    const auto precedingEdit = byteColor(90, 45, 120);
    neo::themeworkbench::setColor(draft, true, "heading", precedingEdit);

    // A long pointer gesture must remain one edit, without consuming the
    // 100-entry history or erasing the unrelated edit immediately before it.
    for (unsigned frame = 0; frame < 140; ++frame) {
        neo::themeworkbench::setColor(draft, true, "selectionBackground",
                                     byteColor(20 + frame, 80, 160, 30 + frame), frame == 0);
    }
    const auto finalSelection = neo::themeworkbench::colors(draft, true).selectionBackground;
    check(draft.undoStack.size() == 2 &&
              neo::themeworkbench::isLocked(draft, true, "selectionBackground"),
          "140 continuous RGBA updates consume one history entry and lock the edited field");
    check(neo::themeworkbench::undo(draft) &&
              same(neo::themeworkbench::colors(draft, true).selectionBackground, originalSelection) &&
              !neo::themeworkbench::isLocked(draft, true, "selectionBackground") &&
              same(neo::themeworkbench::colors(draft, true).heading, precedingEdit),
          "one gesture undo restores its initial color and lock while preserving the preceding edit");
    check(neo::themeworkbench::redo(draft) &&
              same(neo::themeworkbench::colors(draft, true).selectionBackground, finalSelection) &&
              neo::themeworkbench::isLocked(draft, true, "selectionBackground"),
          "one gesture redo restores the final RGBA color and lock");
    check(same(neo::themeworkbench::colors(draft, false).selectionBackground, originalDarkSelection) &&
              same(neo::themeworkbench::colors(draft, true, true).selectionBackground, originalSelection),
          "continuous light edits leave the dark side and restore baseline unchanged");

    auto typeDraft = neo::themeworkbench::makeDraft(input);
    for (unsigned frame = 0; frame < 140; ++frame) {
        auto typography = typeDraft.current.light.typography;
        typography.bodyLineHeight = 1.6f + static_cast<float>(frame) * .002f;
        neo::themeworkbench::setTypography(typeDraft, true, typography, frame == 0);
    }
    check(typeDraft.undoStack.size() == 1 &&
              typeDraft.current.dark.typography.bodyLineHeight == 1.8f &&
              neo::themeworkbench::undo(typeDraft) &&
              typeDraft.current.light.typography.bodyLineHeight == 1.5f,
          "continuous single-side typography remains one undo step and preserves the other side");
    const auto beforeShared = typeDraft.current;
    for (unsigned frame = 0; frame < 140; ++frame) {
        auto typography = typeDraft.current.light.typography;
        typography.bodyLineHeight = 1.7f + static_cast<float>(frame) * .002f;
        neo::themeworkbench::setTypographyBoth(typeDraft, typography, frame == 0);
    }
    check(typeDraft.undoStack.size() == 1 && typeDraft.redoStack.empty() &&
              typeDraft.current.light.typography.bodyLineHeight == typeDraft.current.dark.typography.bodyLineHeight &&
              neo::themeworkbench::undo(typeDraft) &&
              typeDraft.current.light.typography.bodyLineHeight == beforeShared.light.typography.bodyLineHeight &&
              typeDraft.current.dark.typography.bodyLineHeight == beforeShared.dark.typography.bodyLineHeight,
          "shared typography gesture clears stale redo and restores both independent starting values in one undo");
}

neo::ThemeFileData generatedFixture() {
    neo::ThemeFileData generated;
    generated.version = 2;
    for (std::size_t i = 0; i < neo::themeworkbench::fields().size(); ++i) {
        const auto& name = neo::themeworkbench::fields()[i].name;
        generated.light.colors[name] = byteColor(70 + static_cast<unsigned>(i),
                                                  90 + static_cast<unsigned>(i),
                                                  110 + static_cast<unsigned>(i), 255);
        generated.dark.colors[name] = byteColor(130 + static_cast<unsigned>(i),
                                                 100 + static_cast<unsigned>(i),
                                                 80 + static_cast<unsigned>(i), 255);
    }
    return generated;
}

void testAutoPaletteJsFixture() {
#ifndef EUI_TEST_SOURCE_DIR
    check(false, "theme_auto_palette fixture requires EUI_TEST_SOURCE_DIR");
    return;
#else
    const fs::path fixture = neo::textfile::pathFromUtf8(EUI_TEST_SOURCE_DIR) /
                             "tests/fixtures/theme_auto_palette.csv";
    std::ifstream input(fixture, std::ios::binary);
    check(input.good(), "open approved JavaScript palette fixture: " + neo::textfile::pathToUtf8(fixture));
    if (!input) return;

    std::string line;
    std::getline(input, line); // header
    std::tuple<std::string, std::string, int> activeRecipe;
    bool haveRecipe = false;
    neo::ThemeFileData generated;
    std::string error;
    std::size_t rows = 0;
    std::size_t recipes = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream row(line);
        std::string seed, variant, roundText, lightText, field, rgba;
        if (!std::getline(row, seed, ',') || !std::getline(row, variant, ',') ||
            !std::getline(row, roundText, ',') || !std::getline(row, lightText, ',') ||
            !std::getline(row, field, ',') || !std::getline(row, rgba, ',')) {
            check(false, "fixture row has six CSV columns at row " + std::to_string(rows + 2));
            continue;
        }
        const int round = std::stoi(roundText);
        const auto recipeKey = std::make_tuple(seed, variant, round);
        if (!haveRecipe || recipeKey != activeRecipe) {
            activeRecipe = recipeKey;
            haveRecipe = true;
            ++recipes;
            error.clear();
            check(neo::themeauto::generate(neo::ThemeAutoRecipe{seed, variant, round}, generated, error),
                  "generate fixture recipe " + seed + "/" + variant + "/" + roundText + ": " + error);
            if (error.empty()) {
                const auto label = seed + "/" + variant + "/" + roundText;
                checkGeneratedContrast(neo::themeworkbench::palette(generated, true), label + "/light");
                checkGeneratedContrast(neo::themeworkbench::palette(generated, false), label + "/dark");
            }
        }
        const bool light = lightText == "1";
        const auto& colors = light ? generated.light.colors : generated.dark.colors;
        const auto found = colors.find(field);
        check(found != colors.end(), "generator includes fixture field " + field);
        if (found != colors.end())
            check(same(found->second, parseRgba(rgba)),
                  "C++ palette exactly matches JS fixture " + seed + "/" + variant + "/" +
                      roundText + "/" + lightText + "/" + field +
                      " (expected " + rgba + ")");
        ++rows;
    }
    check(rows == 8736, "JavaScript fixture includes all expected palette fields and cases");
    check(recipes == 168, "fixture covers 8 seeds x 3 variants x 7 rounds");
    std::cerr << "[theme_workbench] palette fixture rows=" << rows
              << " recipes=" << recipes << " palettes=" << recipes * 2 << '\n';
#endif
}

bool samePalette(const neo::themeworkbench::Draft& a, const neo::themeworkbench::Draft& b) {
    for (const bool light : {true, false}) {
        const auto ca = neo::themeworkbench::colors(a, light);
        const auto cb = neo::themeworkbench::colors(b, light);
        for (const auto& field : neo::themeworkbench::fields())
            if (!same(ca.*field.member, cb.*field.member)) return false;
    }
    return true;
}

bool sameRecipe(const std::optional<neo::ThemeAutoRecipe>& a,
                const std::optional<neo::ThemeAutoRecipe>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->seed == b->seed && a->variant == b->variant && a->round == b->round;
}

void testAutoLocksAdoptionAndRestore() {
    const std::string css =
        ".theme-light { --text-normal: #112233; --text-selection: #223344; }"
        ".theme-dark { --text-normal: #445566; --text-selection: #556677; }";
    neo::ThemeFileData imported;
    std::string error;
    check(neo::obsidiantheme::parse(css, "auto-lock fixture", imported, error),
          "parse import fixture for generated palette adoption: " + error);
    auto draft = neo::themeworkbench::makeDraft(imported);
    const auto original = draft;
    const auto importedLightText = neo::themeworkbench::colors(draft, true).text;
    const auto importedLightSelection = neo::themeworkbench::colors(draft, true).selectionBackground;
    const auto importedDarkText = neo::themeworkbench::colors(draft, false).text;
    const auto originalLightAccent = neo::themeworkbench::colors(draft, true).accent;
    const eui::Color manualLightAccent = byteColor(18, 140, 77, 201);
    const eui::Color manualDarkText = byteColor(203, 57, 31, 177);
    auto sameColorEdit = neo::themeworkbench::makeDraft(imported);
    const auto sameText = neo::themeworkbench::colors(sameColorEdit, true).text;
    neo::themeworkbench::setColor(sameColorEdit, true, "text", sameText);
    check(neo::themeworkbench::isLocked(sameColorEdit, true, "text") &&
              sameColorEdit.undoStack.size() == 1 && neo::themeworkbench::undo(sameColorEdit) &&
              !neo::themeworkbench::isLocked(sameColorEdit, true, "text"),
          "same-value manual edit still records and can undo the new lock state");
    neo::themeworkbench::setColor(draft, true, "accent", manualLightAccent);
    neo::themeworkbench::setColor(draft, false, "text", manualDarkText);
    check(neo::themeworkbench::isLocked(draft, true, "accent") &&
              !neo::themeworkbench::isLocked(draft, false, "accent") &&
              neo::themeworkbench::isLocked(draft, false, "text") &&
              !neo::themeworkbench::isLocked(draft, true, "text"),
          "manual color changes automatically lock only that field in that mode");

    const neo::ThemeAutoRecipe recipe{"#3366CC", "vivid", 3};
    neo::ThemeFileData generated;
    check(neo::themeauto::generate(recipe, generated, error),
          "C++ palette generator produces a candidate: " + error);
    const auto beforeCanceledCandidate = draft;
    neo::themeworkbench::adoptGenerated(draft, generated, recipe, true);
    check(same(neo::themeworkbench::colors(draft, true).text, importedLightText) &&
              same(neo::themeworkbench::colors(draft, false).text, manualDarkText) &&
              same(neo::themeworkbench::colors(draft, true).selectionBackground, importedLightSelection),
          "missing-only adoption preserves imported-success and manually locked colors");
    check(same(neo::themeworkbench::colors(draft, true).accent, manualLightAccent) &&
              neo::themeworkbench::isLocked(draft, true, "accent"),
          "adoption does not overwrite a locked fallback color");
    check(same(neo::themeworkbench::colors(draft, true).border, generated.light.colors.at("border")) &&
              same(neo::themeworkbench::colors(draft, false).border, generated.dark.colors.at("border")),
          "missing-only adoption fills unread colors independently in both modes");
    check(draft.undoStack.size() == beforeCanceledCandidate.undoStack.size() + 1 &&
              draft.current.autoRecipe && draft.current.autoRecipe->seed == recipe.seed &&
              draft.current.autoRecipe->variant == recipe.variant && draft.current.autoRecipe->round == recipe.round,
          "one candidate adoption is one undo step and stores its recipe");

    const auto adopted = draft;
    check(neo::themeworkbench::undo(draft) && samePalette(draft, beforeCanceledCandidate) &&
              sameRecipe(draft.current.autoRecipe, beforeCanceledCandidate.current.autoRecipe) &&
              draft.current.lightLocks == beforeCanceledCandidate.current.lightLocks &&
              draft.current.darkLocks == beforeCanceledCandidate.current.darkLocks,
          "undo restores candidate colors, recipe, and per-mode locks together");
    check(neo::themeworkbench::redo(draft) && samePalette(draft, adopted) &&
              sameRecipe(draft.current.autoRecipe, adopted.current.autoRecipe) &&
              draft.current.lightLocks == adopted.current.lightLocks &&
              draft.current.darkLocks == adopted.current.darkLocks,
          "redo reapplies candidate colors, recipe, and locks together");
    check(same(neo::themeworkbench::colors(draft, true, true).text, importedLightText) &&
              same(neo::themeworkbench::colors(draft, false, true).text, importedDarkText) &&
              same(neo::themeworkbench::colors(draft, true, true).accent, originalLightAccent),
          "generation and undo/redo never move the original import baseline");

    const auto lockedAccent = neo::themeworkbench::colors(draft, true).accent;
    const auto generatedAgain = generatedFixture();
    const neo::ThemeAutoRecipe alternate{"#A34B72", "soft", 5};
    neo::themeworkbench::adoptGenerated(draft, generatedAgain, alternate, false);
    check(same(neo::themeworkbench::colors(draft, true).accent, lockedAccent) &&
              same(neo::themeworkbench::colors(draft, true).text, generatedAgain.light.colors.at("text")) &&
              sameRecipe(draft.current.autoRecipe, std::optional<neo::ThemeAutoRecipe>{alternate}),
          "full replacement overwrites imported colors but continues to preserve locks");

    const auto preRestore = draft;
    neo::themeworkbench::restoreColor(draft, true, "accent");
    check(same(neo::themeworkbench::colors(draft, true).accent, originalLightAccent) &&
              !neo::themeworkbench::isLocked(draft, true, "accent"),
          "single-field restore returns to initial value and unlocks it");
    check(neo::themeworkbench::undo(draft) &&
              same(neo::themeworkbench::colors(draft, true).accent, preRestore.current.light.colors.at("accent")) &&
              neo::themeworkbench::isLocked(draft, true, "accent") &&
              neo::themeworkbench::redo(draft) &&
              !neo::themeworkbench::isLocked(draft, true, "accent"),
          "undo/redo of single restore round-trips both color and lock state");

    auto beforeGenerationOnly = draft;
    neo::ThemeFileData ignoredCandidate;
    check(neo::themeauto::generate(neo::ThemeAutoRecipe{"#E8AC33", "neutral", 1},
                                   ignoredCandidate, error) &&
              samePalette(draft, beforeGenerationOnly) &&
              draft.current.lightLocks == beforeGenerationOnly.current.lightLocks &&
              sameRecipe(draft.current.autoRecipe, beforeGenerationOnly.current.autoRecipe) &&
              draft.undoStack.size() == beforeGenerationOnly.undoStack.size(),
          "generating or canceling a candidate alone does not mutate the draft or history");

    const auto beforeRestoreAll = draft;
    const auto baselineBeforeRestoreAll = draft.baseline;
    neo::themeworkbench::restoreAll(draft);
    check(samePalette(draft, original) && draft.current.lightLocks.empty() &&
              draft.current.darkLocks.empty() && !draft.current.autoRecipe &&
              draft.current.light.typography.bodyLineHeight == original.current.light.typography.bodyLineHeight,
          "restore-all returns colors and typography to initial values and clears locks/recipe");
    check(neo::themeworkbench::undo(draft) && samePalette(draft, beforeRestoreAll) &&
              draft.current.lightLocks == beforeRestoreAll.current.lightLocks &&
              sameRecipe(draft.current.autoRecipe, beforeRestoreAll.current.autoRecipe),
          "restore-all is one undo step including previous locks and recipe");
    check(same(neo::themeworkbench::colors(draft, true, true).text,
               neo::themeworkbench::colors(original, true, true).text) &&
              same(neo::themeworkbench::colors(draft, false, true).text,
                   neo::themeworkbench::colors(original, false, true).text) &&
              draft.baseline.importReport.size() == baselineBeforeRestoreAll.importReport.size(),
          "restore-all does not change import colors or report baseline");
}

void testSaveRoundTrip(const fs::path& root) {
    neo::ThemeFileData imported;
    imported.version = 2;
    imported.importSource = "sample/theme.css";
    imported.importReport.push_back({true, "selectionBackground", "--text-selection", "", true});
    imported.light.colors["selectionBackground"] = byteColor(26, 51, 77, 102);
    imported.dark.colors["selectionBackground"] = byteColor(128, 153, 179, 204);
    imported.lightLocks.insert("selectionBackground");
    imported.darkLocks.insert("accent");
    imported.autoRecipe = neo::ThemeAutoRecipe{"#62866B", "soft", 2};
    imported.importLightBaseline = imported.light;
    imported.importDarkBaseline = imported.dark;

    auto draft = neo::themeworkbench::makeDraft(imported);
    const auto baselineColor = neo::themeworkbench::colors(draft, true, true).selectionBackground;
    neo::themeworkbench::setColor(draft, true, "selectionBackground", byteColor(230, 26, 51, 77));
    std::string error;
    check(neo::themeworkbench::saveDraft(draft, neo::textfile::pathToUtf8(root / "saved"), error),
          "save workbench draft: " + error);
    if (draft.savedPath.empty()) return;

    neo::ThemeFileData loaded;
    check(neo::themeloader::read(draft.savedPath, loaded, error), "read saved draft: " + error);
    check(loaded.importSource == "sample/theme.css" &&
              loaded.importReport.size() == 52 &&
              loaded.importReport.front().field == "selectionBackground" &&
              loaded.importReport.front().source == "--text-selection" &&
              loaded.importReport.front().imported,
          "save/read retains import source and per-field report");
    check(loaded.lightLocks.count("selectionBackground") == 1 &&
              loaded.darkLocks.count("accent") == 1 && loaded.autoRecipe &&
              loaded.autoRecipe->seed == "#62866B" && loaded.autoRecipe->variant == "soft" &&
              loaded.autoRecipe->round == 2,
          "save/read retains per-mode locks and the applied auto-palette recipe");

    auto legacyCurrent = imported;
    legacyCurrent.lightLocks.clear();
    legacyCurrent.darkLocks.clear();
    legacyCurrent.autoRecipe.reset();
    const auto legacyBaseline = neo::themeworkbench::makeDraft(legacyCurrent).baseline;
    auto legacyJson = neo::themeloader::serializeWorkbench(legacyCurrent, legacyBaseline, "legacy/source.css");
    const auto removeOnce = [&](const std::string& needle) {
        const auto position = legacyJson.find(needle);
        if (position != std::string::npos) legacyJson.erase(position, needle.size());
    };
    removeOnce(",\n    \"lightLocks\": []");
    removeOnce(",\n    \"darkLocks\": []");
    neo::ThemeFileData legacyLoaded;
    check(neo::themeloader::parse(legacyJson, legacyLoaded, error) &&
              legacyLoaded.lightLocks.empty() && legacyLoaded.darkLocks.empty() &&
              !legacyLoaded.autoRecipe,
          "old workbench metadata without lock/recipe fields remains readable with unlocked defaults");

    auto invalidLockData = legacyCurrent;
    invalidLockData.lightLocks.insert("iconFolder");
    const auto invalidLockJson = neo::themeloader::serializeWorkbench(
        invalidLockData, legacyBaseline, "invalid/lock.css");
    neo::ThemeFileData rejectedLock;
    check(!neo::themeloader::parse(invalidLockJson, rejectedLock, error) && !error.empty(),
          "workbench parser rejects non-editable lock fields such as fixed icons");
    for (const auto& badRecipe : {neo::ThemeAutoRecipe{"#GGGGGG", "soft", 0},
                                  neo::ThemeAutoRecipe{"#62866B", "unknown", 0},
                                  neo::ThemeAutoRecipe{"#62866B", "soft", -1}}) {
        auto invalidRecipeData = legacyCurrent;
        invalidRecipeData.autoRecipe = badRecipe;
        const auto invalidRecipeJson = neo::themeloader::serializeWorkbench(
            invalidRecipeData, legacyBaseline, "invalid/recipe.css");
        neo::ThemeFileData rejectedRecipe;
        check(!neo::themeloader::parse(invalidRecipeJson, rejectedRecipe, error) && !error.empty(),
              "workbench parser rejects invalid auto-recipe metadata");
    }
    const auto* unknownSource = reportEntry(loaded, false, "window");
    check(unknownSource && !unknownSource->imported && unknownSource->reason == "未记录该项的导入来源",
          "incomplete historical report is completed without inventing successful imports");
    auto reopened = neo::themeworkbench::makeDraft(loaded, draft.savedPath);
    check(same(neo::themeworkbench::colors(reopened, true, true).selectionBackground,
               baselineColor),
          "save/read retains the original import baseline for before/after comparison");
    check(neo::themeworkbench::changed(reopened, true, "selectionBackground"),
          "saved current color remains changed relative to retained import baseline");

    const fs::path source = root / "original-theme.css";
    const std::string originalBytes = ".theme-light { --text-normal: #abcdef; }";
    check(writeText(source, originalBytes), "write original source theme fixture");
    draft.sourcePath = neo::textfile::pathToUtf8(source);
    check(neo::themeworkbench::saveDraft(draft, neo::textfile::pathToUtf8(root / "saved-again"), error),
          "save edited theme to a new path: " + error);
    check(readText(source) == originalBytes,
          "saving a workbench theme never modifies its original import source");

    const fs::path blocker = root / "not-a-directory";
    check(writeText(blocker, "keep this file"), "create blocked-output fixture");
    neo::themeworkbench::Draft failedSave = draft;
    failedSave.savedPath = "previously-saved.json";
    check(!neo::themeworkbench::saveDraft(failedSave,
                                         neo::textfile::pathToUtf8(blocker / "child"), error) &&
              !error.empty() && failedSave.savedPath == "previously-saved.json",
          "failed save reports error without advancing savedPath");
}

} // namespace

int main() {
    neo::i18n::initialize("zh-CN");
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) /
        ("eui_theme_workbench_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    if (ec) {
        std::cerr << "无法定位临时目录: " << ec.message() << '\n';
        return 1;
    }
    fs::create_directories(root, ec);
    if (ec) {
        std::cerr << "无法创建临时目录: " << ec.message() << '\n';
        return 1;
    }

    testCssReportAndScope();
    testReadAndDraftIsolation(root);
    testEditsUndoRestoreAndModeIsolation();
    testContinuousEditUndoBoundary();
    testAutoLocksAdoptionAndRestore();
    testAutoPaletteJsFixture();
    testSaveRoundTrip(root);

    neo::themeloader::reset();
    fs::remove_all(root, ec);
    if (failures != 0) {
        std::cerr << failures << " 项检查失败\n";
        return 1;
    }
    std::cout << "theme_workbench: CSS report, read isolation, edits, undo/redo and save round trip ALL PASS\n";
    return 0;
}
