#pragma once

#include "apps/neo_editor/model/unicode_search_data.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace neo::search {

struct Options {
    bool matchCase = false;
    bool wholeWord = false;
};

struct Match {
    int begin = 0;
    int end = 0;
};

namespace detail {

struct Unit {
    std::uint32_t value;
    std::size_t begin;
    std::size_t end;
};

inline Unit decodeOne(const std::string& text, std::size_t offset) {
    const auto b0 = static_cast<unsigned char>(text[offset]);
    std::uint32_t cp = 0;
    std::size_t count = 0;
    if (b0 < 0x80) { cp = b0; count = 1; }
    else if (b0 >= 0xC2 && b0 <= 0xDF) { cp = b0 & 0x1F; count = 2; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) { cp = b0 & 0x0F; count = 3; }
    else if (b0 >= 0xF0 && b0 <= 0xF4) { cp = b0 & 0x07; count = 4; }
    bool valid = count != 0 && offset + count <= text.size();
    for (std::size_t j = 1; valid && j < count; ++j) {
        const auto b = static_cast<unsigned char>(text[offset + j]);
        if ((b & 0xC0) != 0x80) { valid = false; break; }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (valid) {
        valid = !(count == 3 && ((b0 == 0xE0 &&
                     static_cast<unsigned char>(text[offset + 1]) < 0xA0) ||
                     (b0 == 0xED && static_cast<unsigned char>(text[offset + 1]) >= 0xA0))) &&
                 !(count == 4 && ((b0 == 0xF0 &&
                     static_cast<unsigned char>(text[offset + 1]) < 0x90) ||
                     (b0 == 0xF4 && static_cast<unsigned char>(text[offset + 1]) >= 0x90)));
    }
    if (!valid) return Unit{0x110000u + b0, offset, offset + 1};
    return Unit{cp, offset, offset + count};
}

inline std::uint32_t simpleFold(std::uint32_t cp) {
    if (cp < 0x80) return cp >= 'A' && cp <= 'Z' ? cp + 0x20 : cp;
    const auto& folds = unicode_data::folds;
    const auto it = std::lower_bound(folds.begin(), folds.end(), cp,
        [](const unicode_data::Fold& entry, std::uint32_t value) {
            return entry.from < value;
        });
    return it != folds.end() && it->from == cp ? it->to : cp;
}

inline bool isWord(std::uint32_t cp) {
    const auto& ranges = unicode_data::wordRanges;
    const auto it = std::upper_bound(ranges.begin(), ranges.end(), cp,
        [](std::uint32_t value, const unicode_data::Range& range) {
            return value < range.first;
        });
    if (it == ranges.begin()) return false;
    const auto& range = *std::prev(it);
    return cp <= range.last;
}

} // namespace detail

inline std::vector<Match> matches(const std::string& text,
                                 const std::string& query,
                                 Options options = {}) {
    std::vector<Match> result;
    if (query.empty()) return result;

    std::vector<std::uint32_t> pattern;
    for (std::size_t offset = 0; offset < query.size();) {
        const auto unit = detail::decodeOne(query, offset);
        pattern.push_back(options.matchCase ? unit.value : detail::simpleFold(unit.value));
        offset = unit.end;
    }
    if (pattern.empty()) return result;

    std::vector<std::size_t> prefix(pattern.size(), 0);
    for (std::size_t i = 1, j = 0; i < pattern.size(); ++i) {
        while (j && pattern[i] != pattern[j]) j = prefix[j - 1];
        if (pattern[i] == pattern[j]) ++j;
        prefix[i] = j;
    }

    const std::size_t ringSize = pattern.size() + 1;
    std::vector<detail::Unit> recent(ringSize);
    std::size_t sourceIndex = 0;
    std::size_t offset = 0;
    std::size_t matched = 0;
    while (offset < text.size()) {
        const auto unit = detail::decodeOne(text, offset);
        recent[sourceIndex % ringSize] = unit;
        const auto value = options.matchCase ? unit.value : detail::simpleFold(unit.value);
        while (matched && value != pattern[matched]) matched = prefix[matched - 1];
        if (value == pattern[matched]) ++matched;
        if (matched == pattern.size()) {
            const std::size_t firstIndex = sourceIndex + 1 - pattern.size();
            const auto& first = recent[firstIndex % ringSize];
            bool leftOk = true;
            if (options.wholeWord && firstIndex != 0) {
                const auto& before = recent[(firstIndex - 1) % ringSize];
                leftOk = !detail::isWord(before.value);
            }
            bool rightOk = true;
            if (options.wholeWord && unit.end < text.size()) {
                const auto after = detail::decodeOne(text, unit.end);
                rightOk = !detail::isWord(after.value);
            }
            if (leftOk && rightOk && first.begin <= 0x7fffffffU && unit.end <= 0x7fffffffU) {
                result.push_back(Match{static_cast<int>(first.begin), static_cast<int>(unit.end)});
                matched = 0; // accepted spans cannot overlap
            } else {
                matched = prefix[matched - 1];
            }
        }
        offset = unit.end;
        ++sourceIndex;
    }
    return result;
}

} // namespace neo::search
