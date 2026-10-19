#pragma once

#include "model/lp_plan.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace neo {

struct LocalHeadingLink {
    std::string path;       // Percent-decoded local path; may be empty for #heading.
    std::string fragment;   // Percent-decoded heading; empty when absent or after '#'.
    bool hasFragment = false;
};

namespace heading_anchor_detail {

inline int hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

inline bool validUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first == 0) return false;
        if (first < 0x80u) continue;
        std::uint32_t codepoint = 0;
        int following = 0;
        if (first >= 0xc2u && first <= 0xdfu) {
            codepoint = first & 0x1fu; following = 1;
        } else if (first >= 0xe0u && first <= 0xefu) {
            codepoint = first & 0x0fu; following = 2;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            codepoint = first & 0x07u; following = 3;
        } else {
            return false;
        }
        if (static_cast<std::size_t>(following) > text.size() - i) return false;
        for (int n = 0; n < following; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if ((following == 1 && codepoint < 0x80u) ||
            (following == 2 && codepoint < 0x800u) ||
            (following == 3 && codepoint < 0x10000u) ||
            codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

inline bool percentDecode(std::string_view encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%') {
            if (encoded.size() - i < 3) return false;
            const int high = hexDigit(encoded[i + 1]);
            const int low = hexDigit(encoded[i + 2]);
            if (high < 0 || low < 0) return false;
            decoded.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else {
            decoded.push_back(encoded[i]);
        }
    }
    return validUtf8(decoded);
}

inline bool space(std::uint32_t codepoint) {
    return codepoint == ' ' || (codepoint >= '\t' && codepoint <= '\r') ||
           codepoint == 0x85u || codepoint == 0xa0u || codepoint == 0x1680u ||
           (codepoint >= 0x2000u && codepoint <= 0x200au) ||
           codepoint == 0x2028u || codepoint == 0x2029u || codepoint == 0x202fu ||
           codepoint == 0x205fu || codepoint == 0x3000u;
}

// A deliberately small local slug rule, not a GitHub or Obsidian compatibility promise.
inline std::string slug(std::string_view title) {
    std::string result;
    result.reserve(title.size());
    bool separator = false;
    for (std::size_t i = 0; i < title.size();) {
        const std::size_t begin = i;
        const auto first = static_cast<unsigned char>(title[i++]);
        std::uint32_t codepoint = first;
        if (first >= 0xc2u && first <= 0xdfu) {
            codepoint = first & 0x1fu;
            codepoint = (codepoint << 6u) | (static_cast<unsigned char>(title[i++]) & 0x3fu);
        } else if (first >= 0xe0u && first <= 0xefu) {
            codepoint = first & 0x0fu;
            for (int n = 0; n < 2; ++n) codepoint = (codepoint << 6u) | (static_cast<unsigned char>(title[i++]) & 0x3fu);
        } else if (first >= 0xf0u && first <= 0xf4u) {
            codepoint = first & 0x07u;
            for (int n = 0; n < 3; ++n) codepoint = (codepoint << 6u) | (static_cast<unsigned char>(title[i++]) & 0x3fu);
        }
        if (space(codepoint) || codepoint == '-') {
            separator = !result.empty();
            continue;
        }
        if (codepoint < 0x80u && !((codepoint >= 'a' && codepoint <= 'z') ||
                                    (codepoint >= 'A' && codepoint <= 'Z') ||
                                    (codepoint >= '0' && codepoint <= '9'))) {
            continue; // Common ASCII punctuation is removed.
        }
        if (separator && !result.empty()) result.push_back('-');
        separator = false;
        if (codepoint >= 'A' && codepoint <= 'Z') result.push_back(static_cast<char>(codepoint + ('a' - 'A')));
        else result.append(title.data() + begin, i - begin); // Keep Unicode bytes unchanged.
    }
    return result;
}

inline std::string visibleTitle(std::string_view text, const LpLine& line) {
    const int maximum = static_cast<int>(std::min(text.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int beg = std::clamp(line.srcBeg, 0, maximum);
    const int end = std::clamp(line.srcEnd, beg, maximum);
    std::string result;
    int cursor = beg;
    for (const LpRange& hidden : line.conceal) {
        const int hiddenBeg = std::clamp(hidden.beg, beg, end);
        const int hiddenEnd = std::clamp(hidden.end, hiddenBeg, end);
        if (hiddenBeg > cursor) result.append(text.data() + cursor, static_cast<std::size_t>(hiddenBeg - cursor));
        cursor = std::max(cursor, hiddenEnd);
    }
    if (cursor < end) result.append(text.data() + cursor, static_cast<std::size_t>(end - cursor));
    const auto isSpace = [](unsigned char ch) { return ch == ' ' || (ch >= '\t' && ch <= '\r'); };
    const auto first = std::find_if_not(result.begin(), result.end(), isSpace);
    const auto last = std::find_if_not(result.rbegin(), result.rend(), isSpace).base();
    return first < last ? std::string(first, last) : std::string{};
}

} // namespace heading_anchor_detail

// Split at the literal '#', then decode each component. Encoded %23 stays part
// of the filename. The caller filters external URLs before calling this helper.
inline std::optional<LocalHeadingLink> parseLocalHeadingLink(std::string_view navigationUrl) {
    LocalHeadingLink result;
    const std::size_t separator = navigationUrl.find('#');
    result.hasFragment = separator != std::string_view::npos;
    const std::string_view encodedPath = navigationUrl.substr(0, separator);
    const std::string_view encodedFragment = result.hasFragment ? navigationUrl.substr(separator + 1) : std::string_view{};
    if (!heading_anchor_detail::percentDecode(encodedPath, result.path) ||
        !heading_anchor_detail::percentDecode(encodedFragment, result.fragment)) return std::nullopt;
    return result;
}

// Return the heading's source byte offset, or -1 if the decoded fragment is
// absent. Only heading-start lines participate; setext underlines and code
// fences are excluded by the LpPlan heading/section fields.
inline int findHeadingAnchor(const LpPlan& plan, std::string_view text,
                             std::string_view decodedFragment) {
    if (decodedFragment.empty() || !heading_anchor_detail::validUtf8(decodedFragment)) return -1;
    const std::string requestedId = heading_anchor_detail::slug(decodedFragment);
    std::unordered_set<std::string> used;
    std::unordered_map<std::string, unsigned> nextSuffix;
    int exactTitle = -1;
    for (const LpLine& line : plan.lines) {
        if (line.kind != LpKind::Heading || line.headingLevel < 1 ||
            line.headingLevel > 6 || line.sectionEndLine < 0) continue;
        const std::string title = heading_anchor_detail::visibleTitle(text, line);
        if (exactTitle < 0 && std::string_view(title) == decodedFragment) exactTitle = line.srcBeg;
        if (!heading_anchor_detail::validUtf8(title)) continue;
        const std::string base = heading_anchor_detail::slug(title);
        if (base.empty()) continue;
        std::string id = base;
        if (used.count(id) != 0) {
            unsigned& suffix = nextSuffix[base];
            if (suffix == 0) suffix = 1;
            do { id = base + '-' + std::to_string(suffix++); } while (used.count(id) != 0);
        }
        used.insert(id);
        if (!requestedId.empty() && id == requestedId) return line.srcBeg;
    }
    return exactTitle;
}

} // namespace neo
