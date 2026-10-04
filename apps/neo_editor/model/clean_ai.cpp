#include "model/clean_ai.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neo::cleanai {
namespace {

using Mask = std::vector<unsigned char>;

void protect(Mask& mask, std::size_t begin, std::size_t end) {
    end = std::min(end, mask.size());
    for (std::size_t i = std::min(begin, end); i < end; ++i) {
        mask[i] = 1;
    }
}

bool starts(const std::string& text, std::size_t at, std::size_t end, std::string_view token) {
    return at + token.size() <= end && text.compare(at, token.size(), token.data(), token.size()) == 0;
}

bool startsAsciiInsensitive(const std::string& text, std::size_t at, std::size_t end,
                            std::string_view token) {
    if (at + token.size() > end) return false;
    for (std::size_t i = 0; i < token.size(); ++i) {
        char left = text[at + i];
        char right = token[i];
        if (left >= 'A' && left <= 'Z') left = static_cast<char>(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z') right = static_cast<char>(right + ('a' - 'A'));
        if (left != right) return false;
    }
    return true;
}

void addProtectedInterval(std::vector<int>& diff, std::size_t begin, std::size_t end) {
    if (begin < end) {
        ++diff[begin];
        --diff[end];
    }
}

struct Line {
    std::size_t begin;
    std::size_t end;
    std::size_t next;
};

Line lineAt(const std::string& text, std::size_t begin) {
    const std::size_t newline = text.find('\n', begin);
    const std::size_t end = newline == std::string::npos ? text.size() : newline;
    const std::size_t next = newline == std::string::npos ? text.size() : newline + 1;
    return {begin, end, next};
}

std::size_t skipContainerPrefix(const std::string& text, std::size_t begin, std::size_t end) {
    std::size_t at = begin;
    for (;;) {
        const std::size_t beforeSpaces = at;
        while (at < end && at - beforeSpaces < 3 && text[at] == ' ') {
            ++at;
        }
        if (at < end && text[at] == '>') {
            ++at;
            if (at < end && text[at] == ' ') {
                ++at;
            }
            continue;
        }
        std::size_t marker = at;
        if (at < end && (text[at] == '-' || text[at] == '+' || text[at] == '*')) {
            ++marker;
        } else {
            while (marker < end && text[marker] >= '0' && text[marker] <= '9' && marker - at < 10) {
                ++marker;
            }
            if (marker == at || marker >= end || (text[marker] != '.' && text[marker] != ')')) {
                marker = at;
            } else {
                ++marker;
            }
        }
        if (marker > at && marker < end && (text[marker] == ' ' || text[marker] == '\t')) {
            at = marker + 1;
            continue;
        }
        return at;
    }
}

bool hasIndentedCodeLine(const std::string& text, const Line& line) {
    std::size_t at = line.begin;
    for (;;) {
        const std::size_t prefixStart = at;
        std::size_t probe = at;
        std::size_t spaces = 0;
        while (probe < line.end && text[probe] == ' ' && spaces < 3) {
            ++probe;
            ++spaces;
        }
        if (probe < line.end && text[probe] == '>') {
            at = probe + 1;
            if (at < line.end && text[at] == ' ') ++at;
            continue;
        }
        std::size_t marker = probe;
        if (probe < line.end && (text[probe] == '-' || text[probe] == '+' || text[probe] == '*')) {
            ++marker;
        } else {
            while (marker < line.end && text[marker] >= '0' && text[marker] <= '9' && marker - probe < 10) ++marker;
            if (marker == probe || marker >= line.end || (text[marker] != '.' && text[marker] != ')')) {
                marker = probe;
            } else {
                ++marker;
            }
        }
        if (marker > probe && marker < line.end && (text[marker] == ' ' || text[marker] == '\t')) {
            at = marker + 1;
            continue;
        }
        at = prefixStart;
        break;
    }
    std::size_t indent = 0;
    while (at < line.end && text[at] == ' ') {
        ++at;
        ++indent;
    }
    return indent >= 4 || (at < line.end && text[at] == '\t');
}

struct Fence {
    char marker = 0;
    std::size_t count = 0;
    std::size_t after = 0;
};

Fence fenceOnLine(const std::string& text, const Line& line) {
    std::size_t at = skipContainerPrefix(text, line.begin, line.end);
    std::size_t spaces = 0;
    while (at < line.end && text[at] == ' ' && spaces < 3) {
        ++at;
        ++spaces;
    }
    if (at >= line.end || (text[at] != '`' && text[at] != '~')) {
        return {};
    }
    const char marker = text[at];
    const std::size_t runBegin = at;
    while (at < line.end && text[at] == marker) {
        ++at;
    }
    const std::size_t count = at - runBegin;
    return count >= 3 ? Fence{marker, count, at} : Fence{};
}

bool closesFence(const std::string& text, const Line& line, const Fence& fence) {
    const Fence candidate = fenceOnLine(text, line);
    if (candidate.marker != fence.marker || candidate.count < fence.count) {
        return false;
    }
    for (std::size_t i = candidate.after; i < line.end; ++i) {
        if (text[i] != ' ' && text[i] != '\t') {
            return false;
        }
    }
    return true;
}

bool isBlank(const std::string& text, const Line& line) {
    for (std::size_t i = line.begin; i < line.end; ++i) {
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') {
            return false;
        }
    }
    return true;
}

bool startsHtmlBlock(const std::string& text, const Line& line) {
    std::size_t at = skipContainerPrefix(text, line.begin, line.end);
    while (at < line.end && at - line.begin <= 3 && text[at] == ' ') {
        ++at;
    }
    if (at >= line.end || text[at] != '<') {
        return false;
    }
    if (starts(text, at, line.end, "<!--") || starts(text, at, line.end, "<?") ||
        starts(text, at, line.end, "<![CDATA[") || starts(text, at, line.end, "<!")) {
        return true;
    }
    std::size_t name = at + 1;
    if (name < line.end && text[name] == '/') {
        ++name;
    }
    return name < line.end && ((text[name] >= 'A' && text[name] <= 'Z') ||
                               (text[name] >= 'a' && text[name] <= 'z'));
}

bool lineContainsAsciiInsensitive(const std::string& text, std::size_t begin, std::size_t end,
                                  std::string_view needle) {
    if (needle.empty() || needle.size() > end - begin) return false;
    for (std::size_t i = begin; i + needle.size() <= end; ++i) {
        if (startsAsciiInsensitive(text, i, end, needle)) return true;
    }
    return false;
}

std::string rawHtmlClosingTag(const std::string& text, const Line& line) {
    std::size_t at = skipContainerPrefix(text, line.begin, line.end);
    while (at < line.end && at - line.begin < 3 && text[at] == ' ') ++at;
    if (at >= line.end || text[at] != '<') return {};
    if (starts(text, at, line.end, "<!--")) return "-->";
    if (starts(text, at, line.end, "<![CDATA[")) return "]]>";
    if (starts(text, at, line.end, "<?")) return "?>";
    if (starts(text, at, line.end, "<!")) return ">";
    ++at;
    if (at < line.end && text[at] == '/') return {};
    const std::size_t begin = at;
    while (at < line.end && ((text[at] >= 'A' && text[at] <= 'Z') ||
                             (text[at] >= 'a' && text[at] <= 'z'))) ++at;
    if (begin == at) return {};
    std::string name = text.substr(begin, at - begin);
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    if (name == "script" || name == "style" || name == "pre" || name == "textarea") {
        return "</" + name + ">";
    }
    return {};
}

bool lineHasReferenceDefinition(const std::string& text, const Line& line) {
    std::size_t at = line.begin;
    while (at < line.end && (text[at] == ' ' || text[at] == '\t')) {
        ++at;
    }
    if (at >= line.end || text[at] != '[') {
        return false;
    }
    std::size_t close = at + 1;
    while (close < line.end && text[close] != ']') {
        if (text[close] == '\\' && close + 1 < line.end) ++close;
        ++close;
    }
    if (close >= line.end || close + 1 >= line.end || text[close + 1] != ':') {
        return false;
    }
    return true;
}

Mask protectedRanges(const std::string& text) {
    Mask mask(text.size(), 0);
    if (text.empty()) {
        return mask;
    }

    // YAML/TOML front matter is special only at byte zero.
    const Line first = lineAt(text, 0);
    const std::string_view firstText(text.data(), first.end);
    const bool frontMatter = firstText == "---" || firstText == "+++";
    if (frontMatter) {
        protect(mask, 0, first.next);
        std::size_t at = first.next;
        bool closed = false;
        while (at < text.size()) {
            const Line line = lineAt(text, at);
            const std::string_view value(text.data() + line.begin, line.end - line.begin);
            protect(mask, line.begin, line.next);
            if (value == firstText) {
                closed = true;
                at = line.next;
                break;
            }
            at = line.next;
        }
        if (!closed) {
            protect(mask, 0, text.size());
            return mask;
        }
    }

    Fence activeFence;
    bool inHtmlBlock = false;
    bool inReferenceDefinition = false;
    std::string rawHtmlClose;
    for (std::size_t at = 0; at < text.size();) {
        const Line line = lineAt(text, at);
        if (activeFence.marker != 0) {
            protect(mask, line.begin, line.next);
            if (closesFence(text, line, activeFence)) {
                activeFence = {};
            }
            at = line.next;
            continue;
        }
        if (mask[line.begin]) {
            at = line.next;
            continue;
        }
        if (inHtmlBlock) {
            if (!rawHtmlClose.empty()) {
                protect(mask, line.begin, line.next);
                if (lineContainsAsciiInsensitive(text, line.begin, line.end, rawHtmlClose)) {
                    inHtmlBlock = false;
                    rawHtmlClose.clear();
                }
                at = line.next;
                continue;
            } else if (isBlank(text, line)) {
                inHtmlBlock = false;
            } else {
                protect(mask, line.begin, line.next);
                at = line.next;
                continue;
            }
        }

        const Fence fence = fenceOnLine(text, line);
        if (fence.marker != 0) {
            activeFence = fence;
            protect(mask, line.begin, line.next);
            at = line.next;
            continue;
        }

        if (hasIndentedCodeLine(text, line)) {
            protect(mask, line.begin, line.next);
            at = line.next;
            continue;
        }

        if (startsHtmlBlock(text, line)) {
            inHtmlBlock = true;
            rawHtmlClose = rawHtmlClosingTag(text, line);
            protect(mask, line.begin, line.next);
            if (!rawHtmlClose.empty() &&
                lineContainsAsciiInsensitive(text, line.begin, line.end, rawHtmlClose)) {
                inHtmlBlock = false;
                rawHtmlClose.clear();
            }
            at = line.next;
            continue;
        }

        if (inReferenceDefinition) {
            if (isBlank(text, line)) {
                inReferenceDefinition = false;
            } else {
                // Destinations and quoted titles may continue on later lines.
                // Block openers above still need to establish their own context.
                protect(mask, line.begin, line.next);
                at = line.next;
                continue;
            }
        }
        if (lineHasReferenceDefinition(text, line)) {
            protect(mask, line.begin, line.next);
            inReferenceDefinition = true;
        }
        at = line.next;
    }
    if (activeFence.marker != 0) {
        protect(mask, 0, text.size());
    }

    // HTML tags and comments, including multiline/unclosed forms, are scanned once.
    for (std::size_t i = 0; i < text.size();) {
        if (mask[i] || text[i] != '<') {
            ++i;
            continue;
        }
        if (starts(text, i, text.size(), "<!--")) {
            const std::size_t close = text.find("-->", i + 4);
            const std::size_t end = close == std::string::npos ? text.size() : close + 3;
            protect(mask, i, end);
            i = end;
            continue;
        }
        std::size_t nameAt = i + 1;
        if (nameAt < text.size() && text[nameAt] == '/') ++nameAt;
        const bool tagLike = nameAt < text.size() &&
            ((text[nameAt] >= 'A' && text[nameAt] <= 'Z') ||
             (text[nameAt] >= 'a' && text[nameAt] <= 'z') || text[nameAt] == '!' || text[nameAt] == '?');
        if (!tagLike) {
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        char quote = 0;
        bool closed = false;
        while (j < text.size()) {
            const char c = text[j];
            if (quote != 0) {
                if (c == quote) quote = 0;
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '>') {
                ++j;
                closed = true;
                break;
            }
            ++j;
        }
        if (closed) {
            protect(mask, i, j);
            i = j;
        } else {
            // An unfinished tag-like sequence is ambiguous: preserve its suffix.
            protect(mask, i, text.size());
            break;
        }
    }

    // Pair equal-size inline backtick runs. Each delimiter is pushed/popped once,
    // so a long unmatched run cannot trigger repeated suffix searches.
    {
        std::unordered_map<std::size_t, std::vector<std::size_t>> openTicks;
        std::vector<int> tickDiff(text.size() + 1, 0);
        for (std::size_t i = 0; i < text.size();) {
            if (mask[i] || text[i] != '`') {
                ++i;
                continue;
            }
            const std::size_t begin = i;
            while (i < text.size() && !mask[i] && text[i] == '`') {
                ++i;
            }
            const std::size_t count = i - begin;
            auto& stack = openTicks[count];
            if (stack.empty()) {
                stack.push_back(begin);
            } else {
                const std::size_t open = stack.back();
                stack.pop_back();
                addProtectedInterval(tickDiff, open, i);
            }
        }
        int tickDepth = 0;
        for (std::size_t p = 0; p < text.size(); ++p) {
            tickDepth += tickDiff[p];
            if (tickDepth > 0) mask[p] = 1;
        }
    }

    // Pair bracket runs in one pass. This identifies wikilinks/reference links
    // without repeatedly searching malformed, unclosed bracket suffixes.
    std::vector<std::size_t> bracketMatch(text.size(), text.size());
    std::vector<std::size_t> brackets;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (mask[i]) continue;
        if (text[i] == '[') {
            brackets.push_back(i);
        } else if (text[i] == ']' && !brackets.empty()) {
            const std::size_t open = brackets.back();
            brackets.pop_back();
            bracketMatch[open] = i;
            bracketMatch[i] = open;
        }
    }
    std::vector<std::size_t>().swap(brackets);

    // Preserve [[wikilink targets]] as one source construct, including any cleanup
    // token or Unicode whitespace inside the target.
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (mask[i] || text[i] != '[' || text[i + 1] != '[') continue;
        const std::size_t close = bracketMatch[i];
        if (close < text.size() && close > i + 2 && text[close - 1] == ']') {
            protect(mask, i, close + 1);
            i = close;
        }
    }

    // Protect reference-link labels/IDs and inline link/image destinations.
    // Parentheses in quoted titles do not close the destination early.
    std::vector<int> linkDiff(text.size() + 1, 0);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (mask[i] || text[i] != ']' || bracketMatch[i] >= text.size()) continue;
        const std::size_t open = bracketMatch[i];
        if (i + 1 < text.size() && text[i + 1] == '[' && bracketMatch[i + 1] < text.size()) {
            const std::size_t referenceEnd = bracketMatch[i + 1];
            addProtectedInterval(linkDiff, open, referenceEnd + 1);
            i = referenceEnd;
            continue;
        }
        if (i + 1 >= text.size() || text[i + 1] != '(') continue;
        std::size_t depth = 1;
        std::size_t j = i + 2;
        char quote = 0;
        bool angleDestination = false;
        bool escaped = false;
        for (; j < text.size(); ++j) {
            if (mask[j]) break;
            const char c = text[j];
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (quote != 0) {
                if (c == quote) quote = 0;
            } else if (angleDestination) {
                if (c == '>') angleDestination = false;
            } else if ((c == '\'' || c == '"') && j > i + 2 &&
                       (text[j - 1] == ' ' || text[j - 1] == '\t')) {
                quote = c;
            } else if (c == '<' && j == i + 2) {
                angleDestination = true;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')' && --depth == 0) {
                ++j;
                break;
            }
        }
        if (depth == 0) {
            addProtectedInterval(linkDiff, open, j);
            i = j - 1;
        } else {
            // An unmatched target makes the remainder ambiguous. Stop once instead
            // of rescanning its suffix for each later bracket.
            addProtectedInterval(linkDiff, open, text.size());
            break;
        }
    }
    std::vector<std::size_t>().swap(bracketMatch);
    int linkDepth = 0;
    for (std::size_t p = 0; p < text.size(); ++p) {
        linkDepth += linkDiff[p];
        if (linkDepth > 0) mask[p] = 1;
    }
    return mask;
}

bool asciiDigit(char c) { return c >= '0' && c <= '9'; }

constexpr std::size_t kMaxCitationTokenBytes = 4096;

std::size_t numericCitationEnd(const std::string& text, std::size_t at) {
    constexpr std::string_view prefix = "[cite:";
    if (!startsAsciiInsensitive(text, at, text.size(), prefix)) return at;
    std::size_t i = at + prefix.size();
    auto skipHorizontalSpace = [&]() {
        const std::size_t begin = i;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        return i - begin;
    };
    skipHorizontalSpace();
    auto digits = [&]() {
        const std::size_t begin = i;
        while (i < text.size() && asciiDigit(text[i])) ++i;
        return i > begin;
    };
    if (!digits()) return at;
    for (;;) {
        skipHorizontalSpace();
        if (i >= text.size() || text[i] != ',') break;
        ++i;
        skipHorizontalSpace();
        if (!digits()) return at;
    }
    skipHorizontalSpace();
    if (i - at > kMaxCitationTokenBytes) return at;
    return i < text.size() && text[i] == ']' ? i + 1 : at;
}

std::size_t knownBracketCitationEnd(const std::string& text, std::size_t at,
                                    std::size_t closeAt) {
    constexpr std::string_view open = "【";
    constexpr std::string_view close = "】";
    if (!starts(text, at, text.size(), open)) return at;
    if (closeAt >= text.size() || !starts(text, closeAt, text.size(), close)) return at;
    if (closeAt - at > kMaxCitationTokenBytes) return at;
    std::size_t i = at + open.size();
    if (starts(text, i, closeAt, "oai_citation:")) {
        i += 13;
        const std::size_t digits = i;
        while (i < closeAt && asciiDigit(text[i])) ++i;
        if (i == digits || i >= closeAt || text[i] != '|') return at;
        ++i;
        if (i >= closeAt) return at;
    } else {
        const std::size_t digits = i;
        while (i < closeAt && asciiDigit(text[i])) ++i;
        if (i == digits) return at;
        if (i < closeAt && text[i] == ':') {
            ++i;
            const std::size_t second = i;
            while (i < closeAt && asciiDigit(text[i])) ++i;
            if (i == second) return at;
        }
        if (!starts(text, i, closeAt, "†")) return at;
        i += 3;
        if (i >= closeAt) return at;
    }
    return closeAt + close.size();
}

bool allEditable(const Mask& mask, std::size_t begin, std::size_t end,
                 std::size_t selectionBegin, std::size_t selectionEnd) {
    if (begin < selectionBegin || end > selectionEnd) return false;
    if (end - begin > kMaxCitationTokenBytes) return false;
    for (std::size_t i = begin; i < end; ++i) {
        if (mask[i]) return false;
    }
    return true;
}

} // namespace

Result clean(const std::string& document, std::size_t selectionBegin, std::size_t selectionEnd) {
    Result result;
    result.selectionBegin = std::min(selectionBegin, document.size());
    result.selectionEnd = std::clamp(selectionEnd, result.selectionBegin, document.size());
    const Mask mask = protectedRanges(document);

    // Monotonic cached searches keep malformed citation candidates linear without
    // allocating one size_t per input byte.
    std::size_t closeSearch = 0;
    std::size_t lineSearch = 0;
    std::size_t nextClose = std::string::npos;
    std::size_t nextLineBreak = std::string::npos;
    const auto nextMatch = [&document](std::size_t from, std::string_view token,
                                       std::size_t& scan, std::size_t& cached) {
        if (cached != std::string::npos && cached < from) {
            scan = cached + token.size();
            cached = std::string::npos;
        }
        if (cached == std::string::npos) {
            scan = std::max(scan, from);
            cached = document.find(token, scan);
            if (cached == std::string::npos) scan = document.size();
        }
        return cached;
    };
    const auto nextLine = [&document](std::size_t from, std::size_t& scan, std::size_t& cached) {
        if (cached != std::string::npos && cached < from) {
            scan = cached + 1;
            if (document[cached] == '\r' && scan < document.size() && document[scan] == '\n') ++scan;
            cached = std::string::npos;
        }
        if (cached == std::string::npos) {
            scan = std::max(scan, from);
            cached = document.find_first_of("\r\n", scan);
            if (cached == std::string::npos) scan = document.size();
        }
        return cached;
    };

    std::string output;
    output.reserve(document.size());
    const std::size_t selectedBegin = result.selectionBegin;
    const std::size_t selectedEnd = result.selectionEnd;
    bool selectionEndRecorded = false;
    auto recordSelectionEnd = [&]() {
        if (!selectionEndRecorded) {
            result.selectionEnd = output.size();
            selectionEndRecorded = true;
        }
    };
    if (selectedEnd == 0) recordSelectionEnd();
    std::size_t i = 0;
    while (i < document.size()) {
        if (i == selectedEnd) recordSelectionEnd();
        if (!mask[i] && i >= selectedBegin && i < selectedEnd) {
            std::size_t end = i;
            if (startsAsciiInsensitive(document, i, document.size(), "[cite_start]")) {
                end = i + 12;
            } else if (startsAsciiInsensitive(document, i, document.size(), "[cite_end]")) {
                end = i + 10;
            } else {
                end = numericCitationEnd(document, i);
                if (end == i) {
                    const std::size_t close = nextMatch(i, "】", closeSearch, nextClose);
                    const std::size_t lineBreak = nextLine(i, lineSearch, nextLineBreak);
                    end = knownBracketCitationEnd(document, i, std::min(close, lineBreak));
                }
            }
            if (end > i && allEditable(mask, i, end, selectedBegin, selectedEnd)) {
                result.changed = true;
                i = end;
                if (i == selectedEnd) recordSelectionEnd();
                continue;
            }
            if (starts(document, i, document.size(), "\xE2\x80\x8B") &&
                allEditable(mask, i, i + 3, selectedBegin, selectedEnd)) {
                result.changed = true;
                i += 3;
                if (i == selectedEnd) recordSelectionEnd();
                continue;
            }
            if (starts(document, i, document.size(), "\xEF\xBB\xBF") &&
                allEditable(mask, i, i + 3, selectedBegin, selectedEnd)) {
                result.changed = true;
                i += 3;
                if (i == selectedEnd) recordSelectionEnd();
                continue;
            }
            if (starts(document, i, document.size(), "\xC2\xA0") &&
                allEditable(mask, i, i + 2, selectedBegin, selectedEnd)) {
                output.push_back(' ');
                result.changed = true;
                i += 2;
                if (i == selectedEnd) recordSelectionEnd();
                continue;
            }
        }
        output.push_back(document[i++]);
        if (i == selectedEnd) recordSelectionEnd();
    }
    if (!selectionEndRecorded) recordSelectionEnd();
    result.text = std::move(output);
    return result;
}

} // namespace neo::cleanai
