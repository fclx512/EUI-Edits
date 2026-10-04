#pragma once

// Live Preview 需要的「逐行排版几何」纯逻辑层。
//
// 编辑器组件原本建立在"所有行同高同字号"这个前提上（某行 y = 行号 × lineHeight）。
// Live Preview 打破了这个前提：标题行更大、代码块另算、标记被隐藏后行宽也变了。
// 这里把三件事抽成不依赖 UI、可以单独断言的纯函数：
//
//   1. 行内隐藏区间（hole）与"投影后文本"偏移的双向映射；
//   2. 逐行 top / height 的前缀和表 + 二分，替代"乘法算 y"；
//   3. 光标 x、命中、前后移动 —— 全部在"投影后文本"上计算。
//
// 于是"光标永不落在隐藏区间内"是可以被断言验证的不变量，而不是靠截图看出来的。
//
// 两个关键约定：
//   * LineHole 的偏移是**文本绝对偏移**，半开 [beg, end)，升序且互不重叠。
//   * "投影"= 把 hole 覆盖的字节删掉后剩下的文本。测量、光标、命中全部作用在投影文本上；
//     `anchor` 参数是该行（或该软换行段）在文档里的起点绝对偏移。
//
// 无头单测见 tests/unit/input_line_map.cpp。

#include "core/render/text.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace components::input_detail {

// 行内被隐藏的字节区间（文本绝对偏移，半开 [beg, end)）。
struct LineHole {
    int beg = 0;
    int end = 0;

    bool empty() const { return beg >= end; }
    int size() const { return end > beg ? end - beg : 0; }

    friend bool operator==(const LineHole& lhs, const LineHole& rhs) {
        return lhs.beg == rhs.beg && lhs.end == rhs.end;
    }
    friend bool operator!=(const LineHole& lhs, const LineHole& rhs) { return !(lhs == rhs); }
};

// 规范化：排序 + 丢掉空区间 + 合并重叠。调用方可以放心传"可能重叠"的原始区间。
// 规范化区间表：排序、去空、合并**重叠**。
// 刻意不合并"首尾相接"的区间（hole.beg < out.back().end 而非 <=）：表格的管道空隙
// 是**永远隐藏**的洞，而紧挨着它的行内标记（`**粗**`、`` `码` `` 紧跟管道时）在光标
// 进入该片段时要露出来 —— 一合并就把要露出的标记吞进洞里了（S3f 批次 D）。
inline std::vector<LineHole> normalizeHoles(std::vector<LineHole> holes) {
    std::sort(holes.begin(), holes.end(), [](const LineHole& lhs, const LineHole& rhs) {
        return lhs.beg < rhs.beg || (lhs.beg == rhs.beg && lhs.end < rhs.end);
    });
    std::vector<LineHole> out;
    out.reserve(holes.size());
    for (const LineHole& hole : holes) {
        if (hole.empty()) {
            continue;
        }
        if (!out.empty() && hole.beg < out.back().end) {
            out.back().end = std::max(out.back().end, hole.end);
            continue;
        }
        out.push_back(hole);
    }
    return out;
}

// 平移所有区间（"绝对偏移 → 相对某行起点"用这个）。
inline std::vector<LineHole> shiftHoles(const std::vector<LineHole>& holes, int delta) {
    std::vector<LineHole> out;
    out.reserve(holes.size());
    for (const LineHole& hole : holes) {
        LineHole shifted{hole.beg - delta, hole.end - delta};
        if (!shifted.empty()) {
            out.push_back(shifted);
        }
    }
    return out;
}

// 裁到 [beg, end)，丢掉裁后为空的区间。
inline std::vector<LineHole> clipHoles(const std::vector<LineHole>& holes, int beg, int end) {
    std::vector<LineHole> out;
    out.reserve(holes.size());
    for (const LineHole& hole : holes) {
        const int clippedBeg = std::max(hole.beg, beg);
        const int clippedEnd = std::min(hole.end, end);
        if (clippedBeg < clippedEnd) {
            out.push_back({clippedBeg, clippedEnd});
        }
    }
    return out;
}

// ── 投影：文本区间 [anchor, docOffset) 里"可见"的字节数 ────────────────────────
// 等价于"docOffset 在投影文本里的偏移"。docOffset <= anchor 时为 0。
inline int visibleLength(const std::vector<LineHole>& holes, int anchor, int docOffset) {
    if (docOffset <= anchor) {
        return 0;
    }
    int hidden = 0;
    for (const LineHole& hole : holes) {
        if (hole.end <= anchor) {
            continue;
        }
        if (hole.beg >= docOffset) {
            break;
        }
        const int overlapBeg = std::max(hole.beg, anchor);
        const int overlapEnd = std::min(hole.end, docOffset);
        if (overlapBeg < overlapEnd) {
            hidden += overlapEnd - overlapBeg;
        }
    }
    return docOffset - anchor - hidden;
}

// 反向：投影偏移（相对 anchor）→ 文档绝对偏移。
// 结果保证不落在任何 hole 内部：歧义处的投影间隙归给"洞之前"，这样光标不会出现在隐藏标记里。
inline int unprojectVisible(const std::vector<LineHole>& holes, int anchor, int projectedOffset) {
    int projectedBase = 0;
    int documentBase = anchor;
    for (const LineHole& hole : holes) {
        if (hole.end <= documentBase) {
            continue;
        }
        const int segment = hole.beg - documentBase;
        if (projectedOffset <= projectedBase + segment) {
            return documentBase + (projectedOffset - projectedBase);
        }
        projectedBase += segment;
        documentBase = hole.end;
    }
    return documentBase + (projectedOffset - projectedBase);
}

// ── "可见偏移"的判定与吸附 ──────────────────────────────────────────────────

// docOffset 是否严格落在某个 hole 内部。
// 注意偏移是"字节之间的间隙"：hole [beg,end) 覆盖 beg..end-1 这几个字节，
// 所以间隙 beg（在第一个被隐藏字节之前）与间隙 end（在最后一个之后）都是可见位置。
inline bool insideHole(const std::vector<LineHole>& holes, int docOffset) {
    for (const LineHole& hole : holes) {
        if (docOffset <= hole.beg) {
            return false;
        }
        if (docOffset < hole.end) {
            return true;
        }
    }
    return false;
}

// 把 docOffset 吸附到最近的可见位置：落在 hole 内时取 hole.beg（向前吸附，视觉不回退）。
inline int snapVisible(const std::vector<LineHole>& holes, int docOffset) {
    for (const LineHole& hole : holes) {
        if (docOffset <= hole.beg) {
            break;
        }
        if (docOffset < hole.end) {
            return hole.beg;
        }
    }
    return docOffset;
}

// 从 docOffset 出发向前跨过 hole（右移一步时用，避免光标"卡"在隐藏标记左侧）。
inline int nextVisibleOffset(const std::vector<LineHole>& holes, int docOffset) {
    for (const LineHole& hole : holes) {
        if (docOffset < hole.beg) {
            break;
        }
        if (docOffset < hole.end) {
            return hole.end;
        }
    }
    return docOffset;
}

// 从 docOffset 出发向后跨过 hole（左移一步时用）。
inline int prevVisibleOffset(const std::vector<LineHole>& holes, int docOffset) {
    for (const LineHole& hole : holes) {
        if (docOffset <= hole.beg) {
            break;
        }
        if (docOffset <= hole.end) {
            return hole.beg;
        }
    }
    return docOffset;
}

// ── 投影文本 ────────────────────────────────────────────────────────────────

// 取 text[beg,end) 去掉 holes 覆盖的部分，拼成"可见文本"。
// 这是 Live Preview 真正要渲染、也是 metrics 对应的那串字。
inline std::string projectText(const std::string& text, int beg, int end,
                               const std::vector<LineHole>& holes) {
    const int from = std::clamp(beg, 0, static_cast<int>(text.size()));
    const int to = std::clamp(end, from, static_cast<int>(text.size()));
    if (holes.empty()) {
        return text.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
    }
    std::string out;
    out.reserve(static_cast<std::size_t>(to - from));
    int cursor = from;
    for (const LineHole& hole : holes) {
        if (hole.end <= from) {
            continue;
        }
        if (hole.beg >= to) {
            break;
        }
        const int holeBeg = std::clamp(hole.beg, from, to);
        const int holeEnd = std::clamp(hole.end, holeBeg, to);
        if (holeBeg > cursor) {
            out.append(text, static_cast<std::size_t>(cursor), static_cast<std::size_t>(holeBeg - cursor));
        }
        cursor = std::max(cursor, holeEnd);
    }
    if (cursor < to) {
        out.append(text, static_cast<std::size_t>(cursor), static_cast<std::size_t>(to - cursor));
    }
    return out;
}

// ── 光标 ────────────────────────────────────────────────────────────────────

// 投影偏移 → x。语义与原来的 caretX() 一致（lower_bound 定位插槽）。
inline float caretXInMetrics(const core::TextPrimitive::TextMetrics& metrics, int projectedOffset) {
    if (metrics.byteIndices.empty() || metrics.caretX.empty()) {
        return 0.0f;
    }
    const auto it = std::lower_bound(metrics.byteIndices.begin(), metrics.byteIndices.end(), projectedOffset);
    const std::size_t slot = it == metrics.byteIndices.end()
        ? metrics.caretX.size() - 1
        : static_cast<std::size_t>(std::distance(metrics.byteIndices.begin(), it));
    return metrics.caretX[std::min(slot, metrics.caretX.size() - 1)];
}

// 文档偏移 → x。落在隐藏区间内的偏移会被投影吸附，所以光标永远不会出现在隐藏文本的中间。
inline float caretXForDocOffset(const core::TextPrimitive::TextMetrics& metrics,
                                const std::vector<LineHole>& holes,
                                int anchor,
                                int docOffset) {
    return caretXInMetrics(metrics, visibleLength(holes, anchor, docOffset));
}

// 投影偏移 → 前一个 caret 插槽（跨字符/连字都按 metrics 的合法位置走）。
inline int projectedPreviousCaret(const core::TextPrimitive::TextMetrics& metrics, int projectedOffset) {
    if (metrics.byteIndices.empty()) {
        return 0;
    }
    const auto it = std::lower_bound(metrics.byteIndices.begin(), metrics.byteIndices.end(), projectedOffset);
    if (it == metrics.byteIndices.begin()) {
        return metrics.byteIndices.front();
    }
    return *(it - 1);
}

// 投影偏移 → 后一个 caret 插槽。
inline int projectedNextCaret(const core::TextPrimitive::TextMetrics& metrics, int projectedOffset) {
    if (metrics.byteIndices.empty()) {
        return 0;
    }
    const auto it = std::upper_bound(metrics.byteIndices.begin(), metrics.byteIndices.end(), projectedOffset);
    if (it == metrics.byteIndices.end()) {
        return metrics.byteIndices.back();
    }
    return *it;
}

// 目标 x → 最近的可见文档偏移（命中测试）。
inline int docOffsetForX(const core::TextPrimitive::TextMetrics& metrics,
                         const std::vector<LineHole>& holes,
                         int anchor,
                         float targetX) {
    if (metrics.byteIndices.empty() || metrics.caretX.empty()) {
        return anchor;
    }
    const std::size_t count = std::min(metrics.byteIndices.size(), metrics.caretX.size());
    int bestProjected = metrics.byteIndices.front();
    float bestDistance = std::fabs(targetX - metrics.caretX.front());
    for (std::size_t i = 1; i < count; ++i) {
        const float distance = std::fabs(targetX - metrics.caretX[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestProjected = metrics.byteIndices[i];
        }
    }
    return unprojectVisible(holes, anchor, bestProjected);
}

// 文档偏移 → 前一个可见光标位置（左移 / Backspace 用）。
inline int previousVisibleCaret(const core::TextPrimitive::TextMetrics& metrics,
                                const std::vector<LineHole>& holes,
                                int anchor,
                                int docOffset) {
    const int projected = visibleLength(holes, anchor, docOffset);
    return unprojectVisible(holes, anchor, projectedPreviousCaret(metrics, projected));
}

// 文档偏移 → 后一个可见光标位置（右移 / Delete 用）。会整段跨过隐藏区间。
inline int nextVisibleCaret(const core::TextPrimitive::TextMetrics& metrics,
                            const std::vector<LineHole>& holes,
                            int anchor,
                            int docOffset) {
    const int projected = visibleLength(holes, anchor, docOffset);
    return unprojectVisible(holes, anchor, projectedNextCaret(metrics, projected));
}

// ── 行内「文字带」（选区背景 / 文字 / 光标共用的垂直几何）───────────────────
//
// 几何表给的行盒 ≠ 文字所在的那一条带：表格行把单元格的上下内边距算进行盒
// （行高 = 纯文字行高 + 上下内边距），文字靠 textShiftY 整体下移一个内边距，
// 行首图元 / 样式段 / 光标也都锚在 top + textShiftY 上（input.h 的文字循环）。
// 选区背景若直接用整个行盒，就会比文字高出上下各一份内边距，看着"框浮在字上"。
//
// 这里给唯一的口径（与 appendMeasuredTableCellLine 的"纯文字行盒 = 行盒总高减
// 上下内边距"同一个式子）：model 算选区、view 摆文字都调它，免得两处各自猜偏移。
// 纯几何、无状态，可直接单测。
struct LineTextBand {
    float top = 0.0f;     // 文字带顶（已含 textShiftY，相对行盒顶）
    float height = 0.0f;  // 文字带高（行高减去上下各一份内边距）
};

inline LineTextBand lineTextBand(float rowTop, float rowHeight, float textShiftY) {
    // 负的 textShiftY 没有语义（装饰层只给 ≥ 0 的内边距），按 0 处理。
    const float shift = std::max(0.0f, textShiftY);
    // 下限 1px：极矮行（表格分隔条 3px）也要画得出背景。
    return LineTextBand{rowTop + shift, std::max(1.0f, rowHeight - 2.0f * shift)};
}

// Keep one nominal em box centered in the text band. All styled runs share
// this origin, so mixed fonts keep a common baseline rather than ink-centering
// each fragment separately.
inline float lineTextOrigin(const LineTextBand& band, float fontSize) {
    return band.top + std::max(0.0f, band.height - fontSize) * 0.5f;
}

// ── 逐行几何表 ──────────────────────────────────────────────────────────────

// 逐行 top / height 的前缀和表。把"第 i 行 y = i × lineHeight"换成查表 + 二分，
// 这样每行可以有自己的高度。contentHeight 恒等于 total()。
//
// 内部用 double 累加：逐行 float 累加会有可观的漂移（2001 行 × 19.2f 实测差 0.8px），
// 而滚动条的滑块尺寸、最大滚动量都吃这个总数，必须在"行数 × 行高"这种量级上精确。
class LineGeometryTable {
public:
    void build(const std::vector<float>& heights) {
        build(heights, {});
    }

    void build(const std::vector<float>& heights, const std::vector<float>& spaceBefore) {
        tops_.resize(heights.size());
        heights_.assign(heights.begin(), heights.end());
        double y = 0.0;
        for (std::size_t i = 0; i < heights_.size(); ++i) {
            if (i < spaceBefore.size() && i > 0 && spaceBefore[i] > 0.0f) {
                y += spaceBefore[i];
            }
            tops_[i] = y;
            y += heights_[i] > 0.0 ? heights_[i] : 0.0;
        }
        total_ = y;
    }

    int count() const { return static_cast<int>(heights_.size()); }
    float total() const { return static_cast<float>(total_); }

    // Capacity-only estimate for callers that already account for sizeof(InputState).
    // The table owns two double arrays; allocator/node overhead is intentionally not
    // included, matching the approximate nature of the surrounding cache budget.
    std::size_t residentCapacityBytes() const {
        return tops_.capacity() * sizeof(double) + heights_.capacity() * sizeof(double);
    }

    // 释放内部容量（逐出页面/换文档时用）。clear()/赋值空表都会保留 capacity，
    // 只有 swap 空容器才真的把逐行几何还给分配器。
    void release() {
        std::vector<double>{}.swap(tops_);
        std::vector<double>{}.swap(heights_);
        total_ = 0.0;
    }

    float top(int line) const {
        if (heights_.empty()) {
            return 0.0f;
        }
        return static_cast<float>(tops_[static_cast<std::size_t>(std::clamp(line, 0, count() - 1))]);
    }

    float height(int line) const {
        if (heights_.empty()) {
            return 0.0f;
        }
        return static_cast<float>(heights_[static_cast<std::size_t>(std::clamp(line, 0, count() - 1))]);
    }

    float bottom(int line) const { return top(line) + height(line); }

    // 包含 y 的行（y 落在最后一行之下时返回最后一行，与旧的 floor 除法 + clamp 行为一致）。
    int lineAtY(float y) const {
        if (tops_.empty()) {
            return 0;
        }
        const double position = static_cast<double>(y);
        const auto it = std::upper_bound(tops_.begin(), tops_.end(), position);
        const int index = it == tops_.begin() ? 0 : static_cast<int>(std::distance(tops_.begin(), it)) - 1;
        return std::clamp(index, 0, count() - 1);
    }

    // 可视窗口（多留一行做缓冲，与旧实现一致）。
    int firstVisibleLine(float scrollY) const { return std::max(0, lineAtY(scrollY) - 1); }
    int lastVisibleLine(float scrollY, float viewportHeight) const {
        return std::min(count() - 1, lineAtY(scrollY + std::max(0.0f, viewportHeight)) + 1);
    }

private:
    std::vector<double> tops_;
    std::vector<double> heights_;
    double total_ = 0.0;
};

}  // namespace components::input_detail
