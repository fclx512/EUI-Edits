#include "apps/neo_editor/model/file_types.h"
#include "apps/neo_editor/model/source_decorations.h"

#include <iostream>
#include <string>
#include <vector>

namespace {
using components::input_detail::DecoratorEditInfo;
using components::input_detail::PendingTextEdit;
using neo::lp::SyntaxToken;
using neo::lp::TokenKind;

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << "source_decorations: " << message << '\n';
    return condition;
}

bool hasToken(const std::vector<SyntaxToken>& tokens, const std::string& line,
              TokenKind kind, const std::string& spelling) {
    for (const auto& token : tokens) {
        if (token.kind == kind && token.beg >= 0 && token.end >= token.beg &&
            static_cast<std::size_t>(token.end) <= line.size() &&
            line.substr(static_cast<std::size_t>(token.beg),
                        static_cast<std::size_t>(token.end - token.beg)) == spelling) {
            return true;
        }
    }
    return false;
}

PendingTextEdit replaceFirstLine(const std::string& oldText, const std::string& newText,
                                 unsigned long long revision) {
    const auto oldBreak = oldText.find('\n');
    const auto newBreak = newText.find('\n');
    PendingTextEdit edit;
    edit.valid = true;
    edit.revision = revision;
    edit.byteBeg = 0;
    edit.oldEnd = static_cast<int>(oldBreak);
    edit.newEnd = static_cast<int>(newBreak);
    edit.firstLine = 0;
    edit.newLastLine = 0;
    edit.oldTailLine = 1;
    edit.cursorBefore = 0;
    return edit;
}

std::vector<int> lineStarts(const std::string& text) {
    std::vector<int> starts{0};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') starts.push_back(static_cast<int>(i + 1));
    }
    return starts;
}

struct EditCase {
    std::string text;
    PendingTextEdit edit;
};

EditCase replaceLine(const std::string& text, int line, const std::string& replacement,
                     unsigned long long revision) {
    const auto starts = lineStarts(text);
    const int beg = starts[static_cast<std::size_t>(line)];
    const auto newline = text.find('\n', static_cast<std::size_t>(beg));
    const int end = newline == std::string::npos ? static_cast<int>(text.size())
                                                 : static_cast<int>(newline);
    EditCase result{text, {}};
    result.text.replace(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg), replacement);
    auto& edit = result.edit;
    edit.valid = true; edit.revision = revision; edit.byteBeg = beg;
    edit.oldEnd = end; edit.newEnd = beg + static_cast<int>(replacement.size());
    edit.firstLine = line; edit.newLastLine = line; edit.oldTailLine = line + 1;
    edit.cursorBefore = beg;
    return result;
}

EditCase insertLineAfter(const std::string& text, int line, const std::string& inserted,
                         unsigned long long revision) {
    const auto starts = lineStarts(text);
    const int beg = starts[static_cast<std::size_t>(line)];
    const auto newline = text.find('\n', static_cast<std::size_t>(beg));
    const bool last = newline == std::string::npos;
    const int at = last ? static_cast<int>(text.size()) : static_cast<int>(newline);
    const std::string splice = "\n" + inserted;
    EditCase result{text, {}};
    result.text.insert(static_cast<std::size_t>(at), splice);
    auto& edit = result.edit;
    edit.valid = true; edit.revision = revision; edit.byteBeg = at;
    edit.oldEnd = at; edit.newEnd = at + static_cast<int>(splice.size());
    edit.firstLine = line; edit.newLastLine = line + 1;
    edit.oldTailLine = last ? static_cast<int>(starts.size()) : line + 1;
    edit.cursorBefore = at;
    return result;
}

EditCase deleteLine(const std::string& text, int line, unsigned long long revision) {
    const auto starts = lineStarts(text);
    const int start = starts[static_cast<std::size_t>(line)];
    const bool last = static_cast<std::size_t>(line + 1) == starts.size();
    const int beg = last && line > 0 ? start - 1 : start;
    const int end = last ? static_cast<int>(text.size()) : starts[static_cast<std::size_t>(line + 1)];
    const int first = last && line > 0 ? line - 1 : line;
    EditCase result{text, {}};
    result.text.erase(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg));
    auto& edit = result.edit;
    edit.valid = true; edit.revision = revision; edit.byteBeg = beg;
    edit.oldEnd = end; edit.newEnd = beg;
    edit.firstLine = first; edit.newLastLine = first;
    edit.oldTailLine = last ? static_cast<int>(starts.size()) : line + 2;
    edit.cursorBefore = beg;
    return result;
}

components::input_detail::LineDecorationSnapshot fresh(const std::string& text,
                                                        const std::string& language,
                                                        const neo::EditorColors& colors,
                                                        unsigned long long palette,
                                                        unsigned long long revision) {
    neo::source::Cache oracle;
    DecoratorEditInfo info;
    info.textRevision = revision;
    info.committed = true;
    return neo::source::build(oracle, text, language, colors, palette, info);
}

bool colorEqual(const core::Color& a, const core::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool testFileTypes() {
    using neo::filetypes::Category;
    using neo::filetypes::detect;
    bool ok = true;
    const auto expect = [&](const char* path, Category category, const char* language,
                            const char* description) {
        const auto type = detect(path);
        return check(type.category == category && type.language == language, description);
    };
    ok &= expect(R"(D:\src\MAIN.PY)", Category::Code, "python", "extension matching is case insensitive");
    ok &= expect("folder.with.dots/source.JSON", Category::Data, "json",
                 "dots in a parent directory do not become the extension");
    ok &= expect("folder.with.dots/no-extension", Category::Unknown, "text",
                 "extensionless files inside dotted directories stay unknown");
    ok &= expect(".env", Category::Data, "ini", ".env is a recognized data file");
    ok &= expect(".env.local", Category::Data, "ini", ".env suffix files are recognized");
    ok &= expect(".gitignore", Category::Unknown, "text", ".gitignore uses the generic file category");
    ok &= check(neo::filetypes::known(".gitignore") && neo::filetypes::known(".gitattributes") &&
                neo::filetypes::known(".gitmodules"),
                "recognized git metadata remains included in the document library");
    ok &= expect("CMakeLists.txt", Category::Code, "cmake", "CMakeLists.txt is detected by basename");
    ok &= expect("LICENSE", Category::Unknown, "text", "LICENSE uses the generic category without an extension");
    ok &= check(neo::filetypes::known("LICENSE") && neo::filetypes::known("README") &&
                neo::filetypes::known("hosts"),
                "recognized common extensionless text names remain known");
    ok &= expect("README.MD", Category::Markdown, "markdown", "README markdown extension is detected");
    ok &= expect("notes.unknown", Category::Unknown, "text", "unknown extension remains plain text");
    ok &= expect("/folder.with.dot/.hidden", Category::Unknown, "text",
                 "unknown dotfiles are not guessed from parent names");
    return ok;
}

bool testJsonAndPythonTokens() {
    bool ok = true;
    const auto tokenize = [](const std::string& line, const std::string& language) {
        neo::lp::SyntaxState state;
        std::vector<SyntaxToken> tokens;
        neo::lp::tokenizeCodeLine(line, 0, static_cast<int>(line.size()), language, state, tokens);
        return tokens;
    };

    const std::string json = R"({"enabled": true, "count": 12, "name": "neo"})";
    const auto jsonTokens = tokenize(json, "json");
    ok &= check(hasToken(jsonTokens, json, TokenKind::Property, "\"enabled\""),
                "JSON object keys are property tokens");
    ok &= check(hasToken(jsonTokens, json, TokenKind::Keyword, "true"),
                "JSON boolean literals are keyword tokens");
    ok &= check(hasToken(jsonTokens, json, TokenKind::Number, "12"),
                "JSON numeric values are number tokens");
    ok &= check(hasToken(jsonTokens, json, TokenKind::String, "\"neo\""),
                "JSON string values remain string tokens");

    const std::string strictJson = R"({"key": 1} // not a JSON comment)";
    const auto strictTokens = tokenize(strictJson, "json");
    ok &= check(!hasToken(strictTokens, strictJson, TokenKind::Comment, "// not a JSON comment)"),
                "strict JSON does not classify comments");
    const std::string strictBlockJson = R"({"key": 1} /* not a JSON comment */)";
    const auto strictBlockTokens = tokenize(strictBlockJson, "json");
    ok &= check(!hasToken(strictBlockTokens, strictBlockJson, TokenKind::Comment,
                          "/* not a JSON comment */"),
                "strict JSON does not classify block comments");
    const std::string jsonc = R"({"key": 1} // JSONC comment)";
    const auto jsoncTokens = tokenize(jsonc, "jsonc");
    ok &= check(hasToken(jsoncTokens, jsonc, TokenKind::Comment, "// JSONC comment"),
                "JSONC accepts line comments");

    const std::string python = "def render(value): return value + 7 # note";
    const auto pythonTokens = tokenize(python, "python");
    ok &= check(hasToken(pythonTokens, python, TokenKind::Keyword, "def"),
                "Python def is a keyword");
    ok &= check(hasToken(pythonTokens, python, TokenKind::Function, "render"),
                "Python call target is a function token");
    ok &= check(hasToken(pythonTokens, python, TokenKind::Number, "7"),
                "Python numeric literal is a number token");
    ok &= check(hasToken(pythonTokens, python, TokenKind::Comment, "# note"),
                "Python line comment runs to end of line");
    const std::string literals = "True False None";
    const auto literalTokens = tokenize(literals, "python");
    ok &= check(hasToken(literalTokens, literals, TokenKind::Keyword, "True") &&
                hasToken(literalTokens, literals, TokenKind::Keyword, "False") &&
                hasToken(literalTokens, literals, TokenKind::Keyword, "None"),
                "Python singleton and boolean literals use keyword tokens");
    return ok;
}

bool testJsonTrustedEdits(const neo::EditorColors& colors) {
    bool ok = true;
    std::string text = "{\"first\": 1,\n\"middle\": 2,\n\"last\": 3}";
    unsigned long long revision = 100;
    neo::source::Cache cache;
    DecoratorEditInfo initial;
    initial.textRevision = revision;
    const auto built = neo::source::build(cache, text, "json", colors, 700, initial);
    ok &= check(built && cache.rows.size() == 3, "JSON oracle fixture starts with three source rows");

    const auto commit = [&](EditCase change, const char* label, std::size_t expectedRows,
                            std::size_t expectedTokenizedRows) {
        ++revision;
        change.edit.revision = revision;
        DecoratorEditInfo info;
        info.textRevision = revision;
        info.committed = true;
        info.edit = &change.edit;
        const auto actual = neo::source::build(cache, change.text, "json", colors, 700, info);
        const auto oracle = fresh(change.text, "json", colors, 700, revision);
        ok &= check(actual && oracle && *actual == *oracle, label);
        ok &= check(cache.rows.size() == expectedRows && actual && actual->size() == expectedRows,
                    "trusted edit row count matches text and snapshot");
        ok &= check(cache.tokenizedRows == expectedTokenizedRows,
                    "trusted JSON edit takes the described partial-reparse path");
        text = std::move(change.text);
    };

    commit(replaceLine(text, 0, "{\"first\": 9,", revision + 1),
           "same-length first-row JSON edit equals fresh oracle", 3, 1);
    commit(replaceLine(text, 1, "\"middle\": 200,", revision + 1),
           "growing middle-row JSON edit equals fresh oracle", 3, 1);
    commit(replaceLine(text, 2, "\"last\": 4}", revision + 1),
           "same-length last-row JSON edit equals fresh oracle", 3, 1);

    commit(insertLineAfter(text, 0, "\"added\": 5,", revision + 1),
           "cross-line insertion after first row equals fresh oracle", 4, 2);
    commit(deleteLine(text, 1, revision + 1),
           "cross-line deletion of middle row equals fresh oracle", 3, 1);
    commit(insertLineAfter(text, 2, "\"tail\": 6", revision + 1),
           "cross-line insertion at end equals fresh oracle", 4, 2);
    commit(deleteLine(text, 3, revision + 1),
           "cross-line deletion of last row equals fresh oracle", 3, 1);
    commit(deleteLine(text, 0, revision + 1),
           "cross-line deletion of first row equals fresh oracle", 2, 1);
    return ok;
}

bool testIncrementalStateAndFreshOracle(const neo::EditorColors& colors) {
    bool ok = true;
    const auto run = [&](const std::string& language, const std::string& oldText,
                         const std::string& newText, const char* label,
                         bool oldStateOnFirstRow, bool changedStateOnSecondRow,
                         bool expectSuffixConvergence) {
        neo::source::Cache cache;
        DecoratorEditInfo initial;
        initial.textRevision = 20;
        initial.committed = true;
        const auto previous = neo::source::build(cache, oldText, language, colors, 44, initial);
        ok &= check(previous && previous->size() == cache.rows.size(),
                    "initial source snapshot matches the cached line count");
        ok &= check(cache.rows.size() >= 4, "multiline test has suffix rows");
        if (cache.rows.size() < 4) return;
        if (language == "python") {
            ok &= check(hasToken(cache.rows[0].tokens, cache.rows[0].text, TokenKind::String, "'''"),
                        "Python triple-string opening delimiter is highlighted");
        }
        const bool oldState = language == "python" ? cache.rows[0].out.tripleString
                                                    : cache.rows[0].out.blockComment;
        ok &= check(oldState == oldStateOnFirstRow,
                    "first source row carries the expected multiline state");

        PendingTextEdit edit = replaceFirstLine(oldText, newText, 21);
        DecoratorEditInfo changed;
        changed.textRevision = 21;
        changed.committed = true;
        changed.edit = &edit;
        const auto incremental = neo::source::build(cache, newText, language, colors, 44, changed);
        const auto oracle = fresh(newText, language, colors, 44, 21);
        ok &= check(incremental && oracle && *incremental == *oracle,
                    label);
        if (expectSuffixConvergence) {
            ok &= check(cache.tokenizedRows >= 2 && cache.tokenizedRows < cache.rows.size(),
                        "edit reparses until multiline state converges, then reuses suffix rows");
        } else {
            ok &= check(cache.tokenizedRows == cache.rows.size(),
                        "unclosed Python triple string propagates changed state through EOF");
        }
        const bool changedState = language == "python" ? cache.rows[1].in.tripleString
                                                       : cache.rows[1].in.blockComment;
        ok &= check(changedState == changedStateOnSecondRow,
                    "changed multiline state reaches the next row");
        ok &= check(previous->size() == cache.rows.size() && *previous == *fresh(oldText, language, colors, 44, 20),
                    "published old generation stays immutable after incremental update");
    };

    const std::string oldPython = "'''open\ninside string\n'''\ndef render(): return 1\n";
    const std::string newPython = "label = 1\ninside string\n'''\ndef render(): return 1\n";
    run("python", oldPython, newPython, "Python multiline-string edit equals independent full oracle",
        true, false, false);

    const std::string oldCpp = "/* open\ninside comment\n*/\nint render() { return 1; }\n";
    const std::string newCpp = "int value = 1;\ninside comment\n*/\nint render() { return 1; }\n";
    run("cpp", oldCpp, newCpp, "C-style multiline-comment edit equals independent full oracle",
        true, false, true);
    return ok;
}

bool testImeAndUntrustedEditIsolation(const neo::EditorColors& colors) {
    bool ok = true;
    neo::source::invalidate();
    const std::string committedText = "def keep(): return 1\nvalue = 2\n";
    DecoratorEditInfo committedInfo;
    committedInfo.textRevision = 70;
    committedInfo.committed = true;
    const auto committed = neo::source::snapshot(committedText, "python", colors, 900, committedInfo);
    const auto& global = neo::source::cache();
    ok &= check(global.snapshot == committed && global.revision == 70,
                "committed source text becomes the shared cache generation");

    DecoratorEditInfo imeInfo;
    imeInfo.textRevision = 71;
    imeInfo.committed = false;
    const auto preedit = neo::source::snapshot("def preview(): return 99\n", "python",
                                               colors, 900, imeInfo);
    ok &= check(preedit && preedit != committed,
                "IME preedit gets a separate temporary snapshot");
    ok &= check(global.snapshot == committed && global.revision == 70 && global.bytes == committedText.size(),
                "IME preedit does not alter committed cache metadata or generation");

    // Missing trusted edit metadata forces a full parse, not an unsafe incremental splice.
    const std::string externalText = "def external(): return 4\nvalue = 8\n";
    DecoratorEditInfo externalInfo;
    externalInfo.textRevision = 71;
    externalInfo.committed = true;
    const auto external = neo::source::snapshot(externalText, "python", colors, 900, externalInfo);
    const auto oracle = fresh(externalText, "python", colors, 900, 71);
    ok &= check(external && oracle && *external == *oracle,
                "untrusted external replacement falls back to a fresh full parse");
    ok &= check(global.snapshot == external && global.revision == 71 && global.bytes == externalText.size(),
                "full fallback publishes only the new committed generation");
    neo::source::invalidate();
    return ok;
}

bool testThemeSnapshotImmutability() {
    bool ok = true;
    neo::source::Cache cache;
    auto light = neo::makeColors(true);
    DecoratorEditInfo info;
    info.textRevision = 3;
    const std::string text = "return True\n";
    const auto old = neo::source::build(cache, text, "python", light, 100, info);
    const auto oldColor = light.tokenKeyword;
    light.tokenKeyword = neo::rgba(.93f, .11f, .23f);
    const auto newer = neo::source::build(cache, text, "python", light, 101, info);
    const auto oracle = fresh(text, "python", light, 101, 3);
    ok &= check(old && newer && oracle && *newer == *oracle,
                "palette change rebuilds tokens to match an independent fresh oracle");
    ok &= check(old != newer, "palette switch publishes a new immutable snapshot");
    if (old && old->size() && newer && newer->size()) {
        const auto oldRuns = (*old)[0].runs;
        const auto newRuns = (*newer)[0].runs;
        ok &= check(!oldRuns.empty() && !newRuns.empty(), "keyword line has colored token runs");
        if (!oldRuns.empty() && !newRuns.empty()) {
            ok &= check(colorEqual(oldRuns[0].style.color, oldColor),
                        "held old snapshot keeps its original theme color");
            ok &= check(colorEqual(newRuns[0].style.color, light.tokenKeyword),
                        "new snapshot uses the new theme color");
        }
    }
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    ok &= testFileTypes();
    ok &= testJsonAndPythonTokens();
    const auto colors = neo::makeColors(true);
    ok &= testIncrementalStateAndFreshOracle(colors);
    ok &= testJsonTrustedEdits(colors);
    ok &= testImeAndUntrustedEditIsolation(colors);
    ok &= testThemeSnapshotImmutability();
    return ok ? 0 : 1;
}
