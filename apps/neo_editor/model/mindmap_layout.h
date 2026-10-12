#pragma once

// 只读思维导图布局：吃 Plan 树与外部测好的标签尺寸，产出确定性世界几何。
// 纯模型层，不测字、不碰 UI、不吃 zoom。任何非法或几何溢出的输入整树拒绝
// （valid=false、rejectReason 非空、nodes/edges 为空），绝不返回半个结果。

#include "model/mindmap_plan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace neo::mindmap {

struct Size {
    float width = 0.0f;
    float height = 0.0f;
};

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool intersects(const Rect& other) const; // 闭区间
};

struct LayoutNode {
    Rect bounds;        // 节点自身框
    Rect subtreeBounds; // 含全部后代的包围盒，供可见性剪枝
};

// 正交连线，恒 4 点：父右边中点 → (midX, 父 y) → (midX, 子 y) → 子左边中点。
struct Edge {
    int parent = -1;
    int child = -1;
    Point points[4]{};
    int pointCount = 0;
};

struct Options {
    float paddingX = 8.0f;
    float paddingY = 4.0f;
    float columnGap = 40.0f;
    float rowGap = 12.0f;
    std::size_t maxDepth = 32; // 根 depth=0，越界即拒绝
    std::size_t maxNodes = 2000;
};

struct Layout {
    std::vector<LayoutNode> nodes; // 与 plan.nodes 同序
    std::vector<Edge> edges;
    Rect bounds;              // 整树包围盒
    std::string rejectReason; // 空 = 通过；非空为稳定标识（见 .cpp）
    bool valid = false;
    explicit operator bool() const { return valid; }
};

// O(N)：根左子右，后序累加子树跨度、前序分配坐标；宽树不做每儿子累计重扫。
Layout buildLayout(const Plan& plan, const std::vector<Size>& labelSizes, const Options& options = {});

// 派生内存按 vector capacity 估算，不含分配器/字符串开销。
std::size_t estimateLayoutBytes(const Layout& layout);
std::size_t estimateLayoutBytes(std::size_t nodeCount);

// 可见节点索引，以及任一段与视口相交（非只判端点）的连线索引。
// >textNodeBudget 只置 textOmitted 建议隐藏文字，不丢节点/连线。
struct Visibility {
    std::vector<int> nodes;
    std::vector<int> edges;
    bool textOmitted = false;
};

// O(N + E)。视口非有限、负宽高或端点相加溢出时返回空结果。
Visibility queryVisible(const Layout& layout, const Rect& viewport, std::size_t textNodeBudget = 300);

} // namespace neo::mindmap
