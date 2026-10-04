#pragma once

// 代码块的语法着色（2026-09-25 批次，Obsidian 规格表 §4 的 token 八色）。
//
// CodeMirror 那种按语言的全量文法不在范围里，这里是 80/20 的通用方案：
//   * 注释：行注释（按语言家族给 //、#、--、%）与块注释（/* */ 与 <!-- -->）；
//   * 字符串：' 与 "（带 \ 转义，不跨行）；跨行的只有三引号（Python/Ruby 的
//     """ '''）与 JS 模板串 ` —— 跨行状态机只维护"块注释 / 多行字符串"两个比特；
//   * 数字（含 0x 十六进制与小数）、关键字（按语言家族的三张词表）、
//     函数调用（标识符后随 "("）、属性（"." 后的标识符）、运算符。
//
// 对未知语言：只上 注释(// 与 /* */)/字符串/数字/运算符/函数/属性 六类，
// 不猜关键字（# 在 C 系是预处理，猜错比不上色更糟）。
//
// 纯逻辑、不碰 UI：输入一行源码文本 + 进入该行时的状态，输出 token 区间
// 并把状态推进到行尾。可以脱离窗口写断言测试（tests/unit/lp_decorations.cpp）。

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace neo {
namespace lp {

enum class TokenKind {
    Plain,
    Comment,
    Keyword,
    String,
    Number,
    Operator,
    Function,
    Property,
};

// 一个 token 的**绝对**字节区间（半开），与 LpLine 的 srcBeg/srcEnd 同一坐标系。
struct SyntaxToken {
    int beg = 0;
    int end = 0;
    TokenKind kind = TokenKind::Plain;
};

// 跨行状态：从代码块第一个内容行开始一路带下去（围栏行重置）。
struct SyntaxState {
    bool blockComment = false;
    bool tripleString = false;
    char tripleChar = 0;  // 多行字符串的引号字符（" 或 ' 或 `）
};

namespace syntax_detail {

inline bool isIdentStart(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           static_cast<unsigned char>(c) >= 0x80;  // 让中文等标识符整体保持 Plain
}

inline bool isIdent(char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

inline bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// 行注释的前缀，按语言家族。返回空串 = 该语言没有单行注释。
inline std::string lineCommentMarker(const std::string& lang) {
    if(lang=="json") return {};
    if (lang == "python" || lang == "py" || lang == "ruby" || lang == "rb" ||
        lang == "sh" || lang == "bash" || lang == "shell" || lang == "zsh" ||
        lang == "yaml" || lang == "yml" || lang == "toml" || lang == "perl" ||
        lang == "r" || lang == "makefile" || lang == "dockerfile" ||
        lang == "ini" || lang == "conf") {
        return "#";
    }
    if (lang == "sql" || lang == "lua" || lang == "haskell" || lang == "hs") {
        return "--";
    }
    if (lang == "matlab" || lang == "prolog" || lang == "erlang") {
        return "%";
    }
    if (lang == "vim" || lang == "lisp" || lang == "scheme" || lang == "clojure") {
        return ";";
    }
    return "//";  // C 系 / js / rust / go / java / 未知语言：只认 //
}

inline std::string blockCommentOpen(const std::string& lang) {
    if (lang == "html" || lang == "xml" || lang == "vue" || lang == "svelte" || lang == "svg") {
        return "<!--";
    }
    return "/*";
}

inline bool blockCommentLang(const std::string& lang) {
    // python/ruby/sh/sql 这些没有 /* */，别把 "/*" 误当注释开头。
    return lang != "json" && lang != "python" && lang != "py" && lang != "ruby" && lang != "rb" &&
           lang != "sh" && lang != "bash" && lang != "shell" && lang != "sql" &&
           lang != "yaml" && lang != "yml" && lang != "toml" && lang != "ini";
}

inline bool tripleQuoteLang(const std::string& lang) {
    return lang == "python" || lang == "py" || lang == "ruby" || lang == "rb";
}

inline bool templateStringLang(const std::string& lang) {
    return lang == "javascript" || lang == "js" || lang == "typescript" || lang == "ts";
}

inline bool inKeywordSet(const std::string& lang, const std::string& word) {
    if(lang=="json" || lang=="jsonc") return word=="true" || word=="false" || word=="null";
    if((lang=="python" || lang=="py") && (word=="True" || word=="False" || word=="None")) return true;
    static const char* kCurly[] = {
        "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch", "char",
        "class", "const", "constexpr", "continue", "decltype", "default", "delete", "do",
        "double", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
        "noexcept", "nullptr", "operator", "private", "protected", "public", "register",
        "reinterpret_cast", "return", "short", "signed", "sizeof", "static", "static_cast",
        "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
        "while", "override", "final", "abstract", "assert", "boolean", "byte", "extends",
        "implements", "instanceof", "interface", "native", "package", "strictfp", "super",
        "synchronized", "throws", "transient", "record", "sealed", "chan", "defer", "func",
        "import", "map", "range", "select", "type", "var", "nil", "async", "await", "box",
        "dyn", "impl", "in", "let", "loop", "mod", "move", "pub", "ref", "self", "trait",
        "unsafe", "use", "where", "crate", "mut", "fn", "typeof", "instanceof", "of",
        "from", "get", "set", "static", "with", "yield", "null", "undefined", "debugger",
        "finally", "val", "when", "out", "params", "readonly", "ref",
    };
    static const char* kScript[] = {
        "and", "as", "assert", "async", "await", "break", "class", "continue", "def",
        "del", "elif", "else", "except", "false", "finally", "for", "from", "global",
        "if", "import", "in", "is", "lambda", "none", "nonlocal", "not", "or", "pass",
        "raise", "return", "true", "try", "while", "with", "yield", "match", "case",
        "then", "elif", "fi", "done", "esac", "function", "select", "until", "coproc",
        "local", "end", "module", "unless", "when", "begin", "rescue", "ensure", "do",
        "require", "puts", "echo", "export", "source", "set",
    };
    static const char* kSql[] = {
        "select", "from", "where", "insert", "update", "delete", "create", "table",
        "alter", "drop", "join", "left", "right", "inner", "outer", "on", "group", "by",
        "order", "having", "limit", "values", "into", "and", "or", "not", "null", "as",
        "distinct", "primary", "key", "foreign", "references", "index", "view", "case",
        "when", "then", "else", "end", "union", "all", "exists", "between", "like",
    };
    auto contains = [](const char* const* table, std::size_t count, const std::string& word) {
        for (std::size_t i = 0; i < count; ++i) {
            if (word == table[i]) {
                return true;
            }
        }
        return false;
    };
    const std::string lower = [&] {
        std::string value = word;
        for (char& c : value) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return value;
    }();
    if (lang == "sql") {
        return contains(kSql, sizeof(kSql) / sizeof(kSql[0]), lower);
    }
    const std::string probe = lang == "vb" ? lower : word;
    if (contains(kCurly, sizeof(kCurly) / sizeof(kCurly[0]), probe)) {
        return true;
    }
    if (lang == "python" || lang == "py" || lang == "ruby" || lang == "rb" ||
        lang == "sh" || lang == "bash" || lang == "shell" || lang == "lua") {
        return contains(kScript, sizeof(kScript) / sizeof(kScript[0]), probe);
    }
    return false;
}

inline bool startsWith(const std::string& text, int pos, int end, const std::string& prefix) {
    if (pos + static_cast<int>(prefix.size()) > end) {
        return false;
    }
    return text.compare(static_cast<std::size_t>(pos), prefix.size(), prefix) == 0;
}

// 读一个标识符/数字，返回终点。
inline int scanWord(const std::string& text, int pos, int end, bool number) {
    int i = pos;
    if (number && i + 1 < end && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
        i += 2;
        while (i < end && (isDigit(text[i]) || (text[i] >= 'a' && text[i] <= 'f') ||
                           (text[i] >= 'A' && text[i] <= 'F'))) {
            ++i;
        }
    } else if (number) {
        while (i < end && (isDigit(text[i]) || text[i] == '.')) {
            ++i;
        }
    } else {
        ++i;
        while (i < end && isIdent(text[i])) {
            ++i;
        }
    }
    return i;
}

} // namespace syntax_detail

// 围栏 info 串 → 代码块右上角那个语言标签的文字。
// Obsidian 的 LP 画的是 CodeMirror 的 language name（```js 显示 "JavaScript"），
// 不是 info 串原文；表里没有的按原文显示，info 串为空则不画标签。
// 表只收常用别名，多余的一条都不加 —— 收错了反倒把别的语言标歪。
inline std::string codeLanguageLabel(const std::string& raw) {
    std::string lang = raw;
    std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lang.empty()) {
        return {};
    }
    struct Entry { const char* alias; const char* label; };
    static const Entry kTable[] = {
        {"js", "JavaScript"}, {"javascript", "JavaScript"},
        {"ts", "TypeScript"}, {"typescript", "TypeScript"},
        {"jsx", "JSX"}, {"tsx", "TSX"},
        {"py", "Python"}, {"python", "Python"},
        {"rb", "Ruby"}, {"ruby", "Ruby"},
        {"sh", "Shell"}, {"bash", "Shell"}, {"zsh", "Shell"}, {"shell", "Shell"},
        {"ps1", "PowerShell"}, {"powershell", "PowerShell"},
        {"cs", "C#"}, {"c#", "C#"}, {"csharp", "C#"},
        {"c", "C"}, {"h", "C"},
        {"cpp", "C++"}, {"c++", "C++"}, {"cc", "C++"}, {"hpp", "C++"}, {"cxx", "C++"},
        {"go", "Go"}, {"golang", "Go"},
        {"rs", "Rust"}, {"rust", "Rust"},
        {"kt", "Kotlin"}, {"kotlin", "Kotlin"},
        {"yml", "YAML"}, {"yaml", "YAML"},
        {"md", "Markdown"}, {"markdown", "Markdown"},
        {"tex", "LaTeX"}, {"latex", "LaTeX"},
        {"toml", "TOML"}, {"ini", "INI"}, {"conf", "INI"},
        {"hs", "Haskell"}, {"haskell", "Haskell"},
        {"matlab", "MATLAB"}, {"pl", "Perl"}, {"perl", "Perl"},
        {"sql", "SQL"}, {"json", "JSON"}, {"html", "HTML"}, {"htm", "HTML"},
        {"xml", "XML"}, {"css", "CSS"}, {"scss", "SCSS"}, {"sass", "Sass"}, {"less", "Less"},
        {"vue", "Vue"}, {"svelte", "Svelte"}, {"dart", "Dart"}, {"swift", "Swift"},
        {"java", "Java"}, {"scala", "Scala"}, {"php", "PHP"}, {"lua", "Lua"},
        {"r", "R"}, {"vim", "Vim"}, {"clj", "Clojure"}, {"clojure", "Clojure"},
        {"ex", "Elixir"}, {"elixir", "Elixir"}, {"erl", "Erlang"}, {"erlang", "Erlang"},
        {"makefile", "Makefile"}, {"dockerfile", "Dockerfile"}, {"diff", "Diff"},
        {"graphql", "GraphQL"}, {"gql", "GraphQL"}, {"proto", "Protobuf"},
        {"text", "Plain Text"}, {"plaintext", "Plain Text"},
    };
    for (const Entry& entry : kTable) {
        if (lang == entry.alias) {
            return entry.label;
        }
    }
    return raw;  // 不认识的按原文（```` `zig` ```` → "zig"）
}

// 关键字表：三大家族共用（curly = C/Java/Go/Rust/…，script = Python/Shell/Ruby/…，
// sql 单独大小写不敏感）。未知语言不入表。
// 把 [beg, end) 切成 token，并把 state 推进到行尾。
// text 必须是整篇文档（token 偏移是绝对字节）；[beg, end) 是本行内容。
inline void tokenizeCodeLine(const std::string& text,
                             int beg,
                             int end,
                             const std::string& lang,
                             SyntaxState& state,
                             std::vector<SyntaxToken>& out) {
    using namespace syntax_detail;
    const std::string lineOpen = blockCommentOpen(lang);
    const std::string lineClose = lineOpen == "<!--" ? "-->" : "*/";
    const std::string commentMarker = lineCommentMarker(lang);
    const bool allowBlockComment = blockCommentLang(lang);
    const bool allowTriple = tripleQuoteLang(lang);
    const bool allowTemplate = templateStringLang(lang);

    int i = beg;
    while (i < end) {
        if (state.blockComment) {
            const int close = static_cast<int>(text.find(lineClose, static_cast<std::size_t>(i)));
            if (close >= 0 && close < end) {
                out.push_back({i, close + static_cast<int>(lineClose.size()), TokenKind::Comment});
                i = close + static_cast<int>(lineClose.size());
                state.blockComment = false;
            } else {
                out.push_back({i, end, TokenKind::Comment});
                i = end;
            }
            continue;
        }
        if (state.tripleString) {
            const std::string closer(3, state.tripleChar);
            const int close = static_cast<int>(text.find(closer, static_cast<std::size_t>(i)));
            if (close >= 0 && close + 3 <= end) {
                out.push_back({i, close + 3, TokenKind::String});
                i = close + 3;
                state.tripleString = false;
            } else {
                out.push_back({i, end, TokenKind::String});
                i = end;
            }
            continue;
        }

        const char c = text[static_cast<std::size_t>(i)];
        // 行注释：吃到行尾。
        if (!commentMarker.empty() && startsWith(text, i, end, commentMarker)) {
            out.push_back({i, end, TokenKind::Comment});
            i = end;
            continue;
        }
        // 块注释开头。
        if (allowBlockComment && startsWith(text, i, end, lineOpen)) {
            state.blockComment = true;
            continue;
        }
        // Python/Ruby 三引号、JS 模板串：进多行字符串状态。
        if (allowTriple && startsWith(text, i, end, "\"\"\"")) {
            state.tripleString = true;
            state.tripleChar = '"';
            out.push_back({i,i+3,TokenKind::String});
            i += 3;
            continue;
        }
        if (allowTriple && startsWith(text, i, end, "'''")) {
            state.tripleString = true;
            state.tripleChar = '\'';
            out.push_back({i,i+3,TokenKind::String});
            i += 3;
            continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && allowTemplate)) {
            int j = i + 1;
            while (j < end && text[static_cast<std::size_t>(j)] != c) {
                if (text[static_cast<std::size_t>(j)] == '\\' && j + 1 < end) {
                    ++j;
                }
                ++j;
            }
            const int stop=j<end?j+1:end;
            int after=stop;while(after<end && (text[after]==' ' || text[after]=='\t')) ++after;
            const auto kind=(lang=="json" || lang=="jsonc") && after<end && text[after]==':' ? TokenKind::Property : TokenKind::String;
            out.push_back({i, stop, kind});
            i = j < end ? j + 1 : end;
            continue;
        }
        // 数字（前面不是标识符字符，避免 0x1 里的 x 被切成标识符）。
        if (isDigit(c) && (i == beg || !isIdent(text[static_cast<std::size_t>(i - 1)]))) {
            const int stop = scanWord(text, i, end, true);
            out.push_back({i, stop, TokenKind::Number});
            i = stop;
            continue;
        }
        // 标识符：关键字 / 函数 / 属性 / 普通词。
        if (isIdentStart(c)) {
            const int stop = scanWord(text, i, end, false);
            const std::string word = text.substr(static_cast<std::size_t>(i),
                                                 static_cast<std::size_t>(stop - i));
            TokenKind kind = TokenKind::Plain;
            int after = stop;
            while (after < end && (text[static_cast<std::size_t>(after)] == ' ' ||
                                   text[static_cast<std::size_t>(after)] == '\t')) {
                ++after;
            }
            if (inKeywordSet(lang, word)) {
                kind = TokenKind::Keyword;
            } else if (after < end && text[static_cast<std::size_t>(after)] == '(') {
                kind = TokenKind::Function;
            } else if (i > beg && text[static_cast<std::size_t>(i - 1)] == '.') {
                kind = TokenKind::Property;
            }
            if (kind != TokenKind::Plain) {
                out.push_back({i, stop, kind});
            }
            i = stop;
            continue;
        }
        // 运算符：单字符归类（标点/括号保持 Plain，与 Obsidian 的口径一致）。
        if (std::string("+-*/%=<>!&|^~?").find(c) != std::string::npos) {
            out.push_back({i, i + 1, TokenKind::Operator});
        }
        ++i;
    }
}

}  // namespace lp
}  // namespace neo
