#pragma once

// 链接目标提取（2026-09-26）：从 lp_plan 里找出某个字节落点所在的链接片段，
// 并把"要打开/要编辑的那段 URL"的区间切出来。纯逻辑，编辑区单击跳转与
// 右键菜单（打开链接 / 编辑链接）共用同一份判定。
//
// URL 区间的口径：
//   · [文字](url) / ![alt](src) —— closeMark 覆盖 "](url)"，URL = 去掉两端括号；
//   · [[目标|别名]] / [[目标#小节]] —— URL = '|' 与 '#' 之前的部分（别名与小节
//     不参与路径解析）；
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
            out.urlBeg = std::max(0, span.content.beg);
            int stop = std::min(static_cast<int>(text.size()), span.content.end);
            for (int i = out.urlBeg; i < stop; ++i) {
                const char ch = text[static_cast<std::size_t>(i)];
                if (ch == '|' || ch == '#') {
                    stop = i;
                    break;
                }
            }
            out.urlEnd = std::max(out.urlBeg, stop);
        } else if (span.closeMark.beg < span.closeMark.end &&
                   span.closeMark.end - span.closeMark.beg >= 2) {
            // "](url)"：URL 夹在 "](" 与 ")" 之间。
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
        break;
    }
    return out;
}

}  // namespace neo
