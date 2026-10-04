#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace components::text_wrap {

// UI prose: keep ordinary Latin words intact, break CJK between code points,
// and split a single overlong word only when it cannot fit on an empty line.
// The caller supplies the same measurement used by its text renderer.
template<class Measure>
std::vector<std::string> lines(const std::string& text, float width, Measure measure) {
    std::vector<std::string> result;
    std::string line;
    const auto flush = [&] { result.push_back(line); line.clear(); };
    const auto latin = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '\'' || c == '-';
    };
    width = std::max(1.0f, width);
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') { ++i; continue; }
        if (c == '\n') { flush(); ++i; continue; }
        if (c == ' ' || c == '\t') {
            if (!line.empty() && measure(line + " ") <= width) line += ' ';
            ++i; continue;
        }
        const std::size_t start = i;
        if (latin(c)) {
            do { ++i; } while (i < text.size() && latin(static_cast<unsigned char>(text[i])));
        } else {
            const std::size_t bytes = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            i += std::min(bytes, text.size()-i);
        }
        const std::string token = text.substr(start, i-start);
        if (!line.empty() && measure(line + token) > width) {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            flush();
        }
        if (measure(token) <= width) { line += token; continue; }
        for (std::size_t j=0; j<token.size();) {
            const unsigned char lead = static_cast<unsigned char>(token[j]);
            const std::size_t bytes = std::min<std::size_t>(lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4,
                                                          token.size()-j);
            const std::string unit = token.substr(j, bytes);
            if (!line.empty() && measure(line+unit) > width) flush();
            line += unit;
            j += bytes;
        }
    }
    if (!line.empty() || result.empty() || (!text.empty() && text.back() == '\n')) flush();
    return result;
}

inline std::string join(const std::vector<std::string>& lines) {
    std::string result;
    for (std::size_t i=0; i<lines.size(); ++i) {
        if (i) result += '\n';
        result += lines[i];
    }
    return result;
}
} // namespace components::text_wrap
