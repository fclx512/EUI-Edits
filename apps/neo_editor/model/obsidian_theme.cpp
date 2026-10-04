#include "model/i18n.h"
#include "model/obsidian_theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace neo::obsidiantheme {
namespace {

using Vars = std::map<std::string, std::string>;

std::string trim(std::string text) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto begin = std::find_if_not(text.begin(), text.end(), isSpace);
    const auto end = std::find_if_not(text.rbegin(), text.rend(), isSpace).base();
    return begin >= end ? std::string{} : std::string(begin, end);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Keep byte positions intact so braces inside comments never affect the scanner.
std::string stripComments(std::string css) {
    for (std::size_t i = 0; i + 1 < css.size();) {
        if (css[i] == '/' && css[i + 1] == '*') {
            const std::size_t end = css.find("*/", i + 2);
            const std::size_t limit = end == std::string::npos ? css.size() : end + 2;
            for (; i < limit; ++i) if (css[i] != '\n') css[i] = ' ';
        } else {
            ++i;
        }
    }
    return css;
}

std::size_t matchingBrace(const std::string& css, std::size_t open, std::size_t limit) {
    int depth = 0;
    char quote = 0;
    for (std::size_t i = open; i < limit; ++i) {
        const char c = css[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return i;
        }
    }
    return limit;
}

enum class Scope { None, Global, Light, Dark, Both };

Scope selectorScope(const std::string& selector) {
    bool global = false, light = false, dark = false;
    std::size_t begin = 0;
    while (begin < selector.size()) {
        const std::size_t comma = selector.find(',', begin);
        const std::string part = trim(selector.substr(begin, comma - begin));
        if (part == ":root" || part == "body" || part == "html") global = true;
        else if (part == ".theme-light" || part == "body.theme-light" ||
                 part == "html.theme-light" || part == ":root.theme-light") light = true;
        else if (part == ".theme-dark" || part == "body.theme-dark" ||
                 part == "html.theme-dark" || part == ":root.theme-dark") dark = true;
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    if (light && dark) return Scope::Both;
    if (light) return Scope::Light;
    if (dark) return Scope::Dark;
    return global ? Scope::Global : Scope::None;
}

void declarations(const std::string& css, std::size_t begin, std::size_t end, Vars& target) {
    std::size_t cursor = begin;
    while (cursor < end) {
        const std::size_t semi = css.find(';', cursor);
        const std::size_t stop = std::min(semi == std::string::npos ? end : semi, end);
        const std::string item = trim(css.substr(cursor, stop - cursor));
        const std::size_t colon = item.find(':');
        if (item.rfind("--", 0) == 0 && colon != std::string::npos) {
            std::string value = trim(item.substr(colon + 1));
            const std::string suffix = "!important";
            if (value.size() >= suffix.size() &&
                lower(value.substr(value.size() - suffix.size())) == suffix) {
                value = trim(value.substr(0, value.size() - suffix.size()));
            }
            target[trim(item.substr(0, colon))] = value;
        }
        cursor = stop == end ? end : stop + 1;
    }
}

void scanRules(const std::string& css, std::size_t begin, std::size_t end,
               Vars& common, Vars& light, Vars& dark, int depth = 0) {
    if (depth > 16) return;
    std::size_t cursor = begin;
    while (cursor < end) {
        const std::size_t open = css.find('{', cursor);
        if (open == std::string::npos || open >= end) break;
        const std::size_t close = matchingBrace(css, open, end);
        if (close >= end) break;
        const std::string selector = trim(css.substr(cursor, open - cursor));
        if (!selector.empty() && selector[0] == '@') {
            scanRules(css, open + 1, close, common, light, dark, depth + 1);
        } else {
            switch (selectorScope(selector)) {
                case Scope::Global: declarations(css, open + 1, close, common); break;
                case Scope::Light: declarations(css, open + 1, close, light); break;
                case Scope::Dark: declarations(css, open + 1, close, dark); break;
                case Scope::Both:
                    declarations(css, open + 1, close, light);
                    declarations(css, open + 1, close, dark);
                    break;
                case Scope::None: break;
            }
        }
        cursor = close + 1;
    }
}

bool resolve(const Vars& vars, const std::string& raw, std::string& out,
             std::set<std::string>& chain, int depth) {
    if (depth > 8) return false;
    out = raw;
    for (;;) {
        const std::size_t start = out.find("var(");
        if (start == std::string::npos) return true;
        int nesting = 1;
        std::size_t end = start + 4;
        for (; end < out.size() && nesting; ++end) {
            if (out[end] == '(') ++nesting;
            else if (out[end] == ')') --nesting;
        }
        if (nesting) return false;
        const std::string args = out.substr(start + 4, end - start - 5);
        const std::size_t comma = args.find(',');
        const std::string key = trim(args.substr(0, comma));
        if (key.rfind("--", 0) != 0 || chain.count(key)) return false;
        const auto found = vars.find(key);
        const std::string source = found == vars.end()
            ? (comma == std::string::npos ? std::string{} : trim(args.substr(comma + 1)))
            : found->second;
        if (source.empty()) return false;
        chain.insert(key);
        std::string replacement;
        const bool ok = resolve(vars, source, replacement, chain, depth + 1);
        chain.erase(key);
        if (!ok) return false;
        out.replace(start, end - start, replacement);
    }
}

bool token(const Vars& vars, const char* name, std::string& out) {
    const auto found = vars.find(name);
    if (found == vars.end()) return false;
    std::set<std::string> chain{name};
    return resolve(vars, found->second, out, chain, 0);
}

bool number(const std::string& text, double& out) {
    const std::string value = trim(text);
    if (value.empty()) return false;
    char* end = nullptr;
    out = std::strtod(value.c_str(), &end);
    return end != value.c_str() && *end == '\0' && std::isfinite(out);
}

bool component(const std::string& raw, double& out, bool percent = false) {
    std::string text = trim(raw);
    const bool hasPercent = !text.empty() && text.back() == '%';
    if (hasPercent) text.pop_back();
    if (!number(text, out)) return false;
    if (hasPercent) out /= 100.0;
    else if (!percent) out /= 255.0;
    return out >= 0.0 && out <= 1.0;
}

std::vector<std::string> splitArguments(const std::string& args) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= args.size(); ++i) {
        if (i == args.size() || args[i] == ',') {
            parts.push_back(trim(args.substr(begin, i - begin)));
            begin = i + 1;
        }
    }
    return parts;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

eui::Color quantize(double r, double g, double b, double a) {
    const auto q = [](double x) { return static_cast<float>(std::round(x * 255.0) / 255.0); };
    return eui::Color{q(r), q(g), q(b), q(a)};
}

bool color(const std::string& raw, eui::Color& out) {
    const std::string value = lower(trim(raw));
    if (value == "white") { out = quantize(1, 1, 1, 1); return true; }
    if (value == "black") { out = quantize(0, 0, 0, 1); return true; }
    if (value == "transparent") { out = quantize(0, 0, 0, 0); return true; }
    if (!value.empty() && value[0] == '#') {
        if (value.size() != 4 && value.size() != 5 && value.size() != 7 && value.size() != 9)
            return false;
        std::array<int, 4> bytes{0, 0, 0, 255};
        const bool shortHex = value.size() <= 5;
        const int count = (value.size() == 5 || value.size() == 9) ? 4 : 3;
        for (int i = 0; i < count; ++i) {
            const int hi = hexDigit(value[1 + i * (shortHex ? 1 : 2)]);
            const int lo = shortHex ? hi : hexDigit(value[2 + i * 2]);
            if (hi < 0 || lo < 0) return false;
            bytes[i] = hi * 16 + lo;
        }
        out = quantize(bytes[0] / 255.0, bytes[1] / 255.0,
                       bytes[2] / 255.0, bytes[3] / 255.0);
        return true;
    }
    const std::size_t open = value.find('(');
    if (open == std::string::npos || value.back() != ')') return false;
    const std::string kind = value.substr(0, open);
    const auto parts = splitArguments(value.substr(open + 1, value.size() - open - 2));
    if (parts.size() != 3 && parts.size() != 4) return false;
    double alpha = 1.0;
    if (parts.size() == 4 && (!number(parts[3], alpha) || alpha < 0 || alpha > 1)) return false;
    double a = 0, b = 0, c = 0;
    if (kind == "rgb" || kind == "rgba") {
        if (!component(parts[0], a) || !component(parts[1], b) || !component(parts[2], c))
            return false;
        out = quantize(a, b, c, alpha);
        return true;
    }
    if (kind != "hsl" && kind != "hsla") return false;
    std::string hue = parts[0];
    if (hue.size() > 3 && hue.substr(hue.size() - 3) == "deg") hue.resize(hue.size() - 3);
    if (!number(hue, a) || !component(parts[1], b, true) ||
        !component(parts[2], c, true)) return false;
    const double h = std::fmod(std::fmod(a, 360.0) + 360.0, 360.0) / 60.0;
    const double chroma = (1.0 - std::abs(2.0 * c - 1.0)) * b;
    const double x = chroma * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    const double m = c - chroma / 2.0;
    const std::array<double, 3> rgb = h < 1 ? std::array<double, 3>{chroma, x, 0}
        : h < 2 ? std::array<double, 3>{x, chroma, 0}
        : h < 3 ? std::array<double, 3>{0, chroma, x}
        : h < 4 ? std::array<double, 3>{0, x, chroma}
        : h < 5 ? std::array<double, 3>{x, 0, chroma}
                : std::array<double, 3>{chroma, 0, x};
    out = quantize(rgb[0] + m, rgb[1] + m, rgb[2] + m, alpha);
    return true;
}

bool mappedColor(const Vars& vars, const std::vector<const char*>& candidates, eui::Color& out) {
    for (const char* candidate : candidates) {
        std::string resolved;
        if (token(vars, candidate, resolved) && color(resolved, out)) return true;
    }
    return false;
}

void mapColors(const Vars& vars, ThemeVariant& variant, int& skipped) {
    struct Entry { const char* field; std::vector<const char*> candidates; };
    const Entry entries[] = {
        {"panel", {"--background-secondary"}},
        {"toolbar", {"--titlebar-background"}},
        {"editor", {"--background-primary"}},
        {"statusBar", {"--status-bar-background"}},
        {"text", {"--text-normal"}},
        {"textMuted", {"--text-muted"}},
        {"border", {"--background-modifier-border"}},
        {"rowHover", {"--background-modifier-hover"}},
        {"rowActive", {"--background-modifier-active-hover"}},
        {"codeBackground", {"--code-background"}},
        {"codeText", {"--code-normal"}},
        {"divider", {"--hr-color"}},
        {"markdownAccent", {"--text-accent", "--link-color", "--interactive-accent"}},
        {"tokenKeyword", {"--code-keyword"}},
        {"tokenString", {"--code-string"}},
        {"tokenNumber", {"--code-value"}},
        {"tokenComment", {"--code-comment"}},
        {"tokenOperator", {"--code-operator"}},
        {"tokenFunction", {"--code-function"}},
        {"tokenProperty", {"--code-property"}},
        {"iconFolder", {"--icon-color"}},
        {"iconFile", {"--icon-color"}},
    };
    for (const Entry& entry : entries) {
        eui::Color parsed;
        if (mappedColor(vars, entry.candidates, parsed)) variant.colors[entry.field] = parsed;
        else ++skipped;
    }
}

void mapTypography(const Vars& vars, ThemeTypography& out) {
    const auto unitless = [&](const char* key, std::optional<float>& field, float lo, float hi) {
        std::string text;
        double value = 0;
        if (token(vars, key, text) && number(text, value) && value >= lo && value <= hi)
            field = static_cast<float>(value);
    };
    const auto length = [&](const char* key, const char* suffix, std::optional<float>& field,
                            float scale, float lo, float hi) {
        std::string text;
        if (!token(vars, key, text)) return;
        text = trim(text);
        const std::size_t n = std::char_traits<char>::length(suffix);
        if (text.size() <= n || text.substr(text.size() - n) != suffix) return;
        double value = 0;
        if (number(text.substr(0, text.size() - n), value)) {
            value *= scale;
            if (value >= lo && value <= hi) field = static_cast<float>(value);
        }
    };
    unitless("--line-height-normal", out.bodyLineHeight, 0.5f, 4.0f);
    length("--p-spacing", "rem", out.blockGap, 1.0f, 0.0f, 4.0f);
    length("--radius-s", "px", out.radius, 1.0f / 16.0f, 0.0f, 2.0f);
    length("--code-size", "em", out.codeSizeFactor, 1.0f, 0.5f, 2.0f);
    length("--h1-size", "em", out.h1SizeFactor, 1.0f, 0.5f, 4.0f);
    length("--h2-size", "em", out.h2SizeFactor, 1.0f, 0.5f, 4.0f);
    length("--h3-size", "em", out.h3SizeFactor, 1.0f, 0.5f, 4.0f);
    unitless("--h1-line-height", out.h1LineHeightFactor, 1.0f, 4.0f);
    unitless("--h2-line-height", out.h2LineHeightFactor, 1.0f, 4.0f);
    unitless("--h3-line-height", out.h3LineHeightFactor, 1.0f, 4.0f);
    // A CSS font stack cannot be passed to the renderer as a single font name.
    for (const auto& font : {std::make_pair("--font-text-override", &ThemeTypography::bodyFontFamily),
                             std::make_pair("--font-monospace-override", &ThemeTypography::codeFontFamily)}) {
        std::string text;
        if (token(vars, font.first, text) && text.find(',') == std::string::npos) {
            text = trim(text);
            if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
                text.back() == text.front()) text = text.substr(1, text.size() - 2);
            if (!text.empty() && text.find('(') == std::string::npos)
                out.*font.second = text;
        }
    }
}

} // namespace

bool parse(const std::string& css, const std::string& name,
           ThemeFileData& out, std::string& error) {
    out = ThemeFileData{};
    error.clear();
    Vars common, light, dark;
    const std::string clean = stripComments(css);
    scanRules(clean, 0, clean.size(), common, light, dark);
    const bool hasLight = !light.empty();
    const bool hasDark = !dark.empty();
    if (!hasLight && !hasDark) {
        error = i18n::tr("obsidian.no_variables");
        return false;
    }
    for (const auto& entry : common) {
        if (hasLight) light.insert(entry);
        if (hasDark) dark.insert(entry);
    }
    out.version = 2;
    out.name = name;
    if (hasLight) mapColors(light, out.light, out.skippedCssDeclarations);
    if (hasDark) mapColors(dark, out.dark, out.skippedCssDeclarations);
    if (hasLight) mapTypography(light, out.light.typography);
    if (hasDark) mapTypography(dark, out.dark.typography);
    if (out.light.colors.empty() && out.dark.colors.empty()) {
        error = i18n::tr("obsidian.no_colors");
        out = ThemeFileData{};
        return false;
    }
    return true;
}

} // namespace neo::obsidiantheme
