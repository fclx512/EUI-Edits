#pragma once

// 链接目标提取（2026-09-26）：从 lp_plan 里找出某个字节落点所在的链接片段，
// 并把"要打开/要编辑的那段 URL"的区间切出来。纯逻辑，编辑区单击跳转与
// 右键菜单（打开链接 / 编辑链接）共用同一份判定。
//
// URL 区间的口径：
//   · [文字](url "title") / ![alt](<src> "title") —— 只取 destination，
//     编辑区间保留外层尖括号、空白和 title；
//   · [[目标#小节|别名]] —— 编辑区间只覆盖目标路径；navigationUrl 保留小节、
//     去掉别名，供打开链接使用；
//   · <autolink> —— closeMark 为空，content 就是 URL。

#include "model/lp_plan.h"

#include <algorithm>
#include <string>

namespace neo {

struct LinkTarget {
    bool valid = false;    // byteIndex 落在链接 / 图片 / WikiLink 片段范围内
    bool wikilink = false;
    int spanBeg = 0;       // 整个片段的文档区间（含标记）
    int spanEnd = 0;
    int urlBeg = 0;        // URL 部分的文档区间（"编辑链接"只替换这一段）
    int urlEnd = 0;
    std::string url;       // URL 原文（未解析、未解码）
    std::string navigationUrl; // 打开目标；Wiki 保留 #小节，但不包含 |别名
};

inline LinkTarget linkTargetAt(const LpPlan& plan, const std::string& text, int byteIndex) {
    LinkTarget out;
    const LpLine* line = plan.lineAt(plan.lineIndexFor(byteIndex));
    if (line == nullptr) {
        return out;
    }
    for (const LpSpan& span : line->spans) {
        if (span.kind != LpSpanKind::Link && span.kind != LpSpanKind::Image &&
            span.kind != LpSpanKind::WikiLink) {
            continue;
        }
        // 片段范围 = 开标记起点 ~ 闭标记终点（与装饰层的 spanBegOf/spanEndOf 同口径）。
        int beg = span.content.beg;
        int end = span.content.end;
        if (span.openMark.beg < span.openMark.end) {
            beg = std::min(beg, span.openMark.beg);
        }
        if (span.closeMark.beg < span.closeMark.end) {
            end = std::max(end, span.closeMark.end);
        }
        if (byteIndex < beg || byteIndex > end) {
            continue;
        }

        out.valid = true;
        out.spanBeg = std::max(0, beg);
        out.spanEnd = std::max(out.spanBeg, end);
        out.wikilink = span.kind == LpSpanKind::WikiLink;
        if (out.wikilink) {
            // md4c puts [[target| inside openMark and only the alias in content.
            // Read the complete source span so both forms have the same target.
            const int size = static_cast<int>(text.size());
            out.urlBeg = std::clamp(span.openMark.beg + 2, 0, size);
            const int targetEnd = std::clamp(span.closeMark.end - 2, out.urlBeg, size);
            int navigationEnd = targetEnd;
            for (int i = out.urlBeg; i < targetEnd; ++i) {
                if (text[static_cast<std::size_t>(i)] == '|') {
                    navigationEnd = i;
                    break;
                }
            }
            out.urlEnd = navigationEnd;
            for (int i = out.urlBeg; i < navigationEnd; ++i) {
                if (text[static_cast<std::size_t>(i)] == '#') {
                    out.urlEnd = i;
                    break;
                }
            }
            out.navigationUrl = text.substr(static_cast<std::size_t>(out.urlBeg),
                                            static_cast<std::size_t>(navigationEnd - out.urlBeg));
        } else if (span.closeMark.beg < span.closeMark.end &&
                   span.closeMark.end - span.closeMark.beg >= 2 &&
                   span.closeMark.beg >= 0 && span.closeMark.beg + 1 < static_cast<int>(text.size()) &&
                   text[static_cast<std::size_t>(span.closeMark.beg)] == ']' &&
                   text[static_cast<std::size_t>(span.closeMark.beg + 1)] == '(') {
            // md4c has already recognized the span. Restrict the edit range to
            // its destination; title and delimiters must survive replacement.
            out.urlBeg = std::min(static_cast<int>(text.size()), span.closeMark.beg + 2);
            const int limit = std::clamp(span.closeMark.end - 1, out.urlBeg,
                                         static_cast<int>(text.size()));
            const auto isSpace = [](char ch) {
                return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
            };
            while (out.urlBeg < limit && isSpace(text[static_cast<std::size_t>(out.urlBeg)]))
                ++out.urlBeg;
            const bool angled = out.urlBeg < limit && text[static_cast<std::size_t>(out.urlBeg)] == '<';
            if (angled) ++out.urlBeg;
            out.urlEnd = out.urlBeg;
            while (out.urlEnd < limit) {
                const char ch = text[static_cast<std::size_t>(out.urlEnd)];
                if (angled ? ch == '>' : isSpace(ch)) break;
                // Skip escaped punctuation while finding the closing '>'. Keep
                // source bytes intact: urlBeg/urlEnd are used by the editor.
                if (ch == '\\' && out.urlEnd + 1 < limit) {
                    const unsigned char next = static_cast<unsigned char>(text[static_cast<std::size_t>(out.urlEnd + 1)]);
                    if ((next >= '!' && next <= '/') || (next >= ':' && next <= '@') ||
                        (next >= '[' && next <= '`') || (next >= '{' && next <= '~')) ++out.urlEnd;
                }
                ++out.urlEnd;
            }
        } else if (span.closeMark.end - span.closeMark.beg >= 2) {
            // Reference-style links have no inline destination. Preserve their
            // existing extraction until reference-definition resolution is added.
            out.urlBeg = std::min(static_cast<int>(text.size()), span.closeMark.beg + 2);
            out.urlEnd = std::clamp(span.closeMark.end - 1, out.urlBeg,
                                    static_cast<int>(text.size()));
        } else {
            out.urlBeg = std::max(0, span.content.beg);
            out.urlEnd = std::clamp(span.content.end, out.urlBeg,
                                    static_cast<int>(text.size()));
        }
        out.urlEnd = std::min(out.urlEnd, static_cast<int>(text.size()));
        out.url = text.substr(static_cast<std::size_t>(out.urlBeg),
                              static_cast<std::size_t>(std::max(0, out.urlEnd - out.urlBeg)));
        if (!out.wikilink) out.navigationUrl = out.url;
        break;
    }
    return out;
}

}  // namespace neo
