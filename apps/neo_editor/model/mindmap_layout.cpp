#include "model/mindmap_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace neo::mindmap {
namespace {

// 稳定拒绝标识（单测按字符串断言）。
constexpr const char* kRejectBadOptions = "bad-options";
constexpr const char* kRejectEmptyPlan = "empty-plan";
constexpr const char* kRejectLabelCount = "label-size-count";
constexpr const char* kRejectNodeLimit = "node-limit";
constexpr const char* kRejectBadLabelSize = "bad-label-size";
constexpr const char* kRejectBadShape = "bad-shape";
constexpr const char* kRejectRootCount = "root-count";
constexpr const char* kRejectBadParent = "bad-parent";
constexpr const char* kRejectBadChild = "bad-child";
constexpr const char* kRejectChildDup = "child-dup";
constexpr const char* kRejectParentMismatch = "parent-mismatch";
constexpr const char* kRejectDepthMismatch = "depth-mismatch";
constexpr const char* kRejectDepthLimit = "depth-limit";
constexpr const char* kRejectCycle = "cycle";
constexpr const char* kRejectUnreachable = "unreachable";
constexpr const char* kRejectGeometryOverflow = "geometry-overflow";

constexpr double kFloatMax = static_cast<double>(std::numeric_limits<float>::max());

bool finiteNonNegative(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

// 中间量必须落在可表示 float 范围内，否则最终写回的 float 会变成 inf。
bool withinFloat(double value) {
    return std::isfinite(value) && value >= -kFloatMax && value <= kFloatMax;
}

Layout reject(const char* reason) {
    Layout layout;
    layout.rejectReason = reason;
    return layout;
}

bool isSupportedShape(Shape shape) {
    return shape == Shape::Rectangle || shape == Shape::Circle;
}

// 矩形 = 标签 + 内边距；圆 = 完整包围"标签 + 内边距"矩形的正方形（边长取 hypot）。
bool nodeBoxSize(Shape shape, double labelWidth, double labelHeight, const Options& options, double& width,
                 double& height) {
    const double paddedWidth = labelWidth + 2.0 * static_cast<double>(options.paddingX);
    const double paddedHeight = labelHeight + 2.0 * static_cast<double>(options.paddingY);
    const double boxWidth = shape == Shape::Circle ? std::hypot(paddedWidth, paddedHeight) : paddedWidth;
    const double boxHeight = shape == Shape::Circle ? boxWidth : paddedHeight;
    if (!withinFloat(boxWidth) || !withinFloat(boxHeight) || boxWidth < 0.0 || boxHeight < 0.0) {
        return false;
    }
    width = boxWidth;
    height = boxHeight;
    return true;
}

bool pointInRect(const Point& p, const Rect& r) {
    return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
}

// double 中间量：坐标可达 ±FLT_MAX，float 相减/相乘会溢出成 inf 或 NaN。
double cross(const Point& o, const Point& a, const Point& b) {
    const double ox = static_cast<double>(o.x);
    const double oy = static_cast<double>(o.y);
    const double ax = static_cast<double>(a.x);
    const double ay = static_cast<double>(a.y);
    const double bx = static_cast<double>(b.x);
    const double by = static_cast<double>(b.y);
    return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox);
}

// 线段相交：规范相交或端点落在另一线段上（含共线）。
bool segmentsIntersect(const Point& a, const Point& b, const Point& c, const Point& d) {
    const double d1 = cross(c, d, a);
    const double d2 = cross(c, d, b);
    const double d3 = cross(a, b, c);
    const double d4 = cross(a, b, d);
    if (((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) &&
        ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0))) {
        return true;
    }
    const auto onSegment = [](const Point& p, const Point& q, const Point& r) {
        return std::min(p.x, r.x) <= q.x && q.x <= std::max(p.x, r.x) &&
               std::min(p.y, r.y) <= q.y && q.y <= std::max(p.y, r.y);
    };
    if (d1 == 0.0 && onSegment(c, a, d)) return true;
    if (d2 == 0.0 && onSegment(c, b, d)) return true;
    if (d3 == 0.0 && onSegment(a, c, b)) return true;
    if (d4 == 0.0 && onSegment(a, d, b)) return true;
    return false;
}

bool segmentIntersectsRect(const Point& a, const Point& b, const Rect& r) {
    if (pointInRect(a, r) || pointInRect(b, r)) return true;
    const Point lt{r.x, r.y};
    const Point rt{r.x + r.width, r.y};
    const Point rb{r.x + r.width, r.y + r.height};
    const Point lb{r.x, r.y + r.height};
    return segmentsIntersect(a, b, lt, rt) || segmentsIntersect(a, b, rt, rb) ||
           segmentsIntersect(a, b, rb, lb) || segmentsIntersect(a, b, lb, lt);
}

// 视口自身可用：宽高有限非负，且右/下边界不因相加溢出。
bool validViewport(const Rect& r) {
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !finiteNonNegative(r.width) ||
        !finiteNonNegative(r.height)) {
        return false;
    }
    const double right = static_cast<double>(r.x) + static_cast<double>(r.width);
    const double bottom = static_cast<double>(r.y) + static_cast<double>(r.height);
    return withinFloat(right) && withinFloat(bottom);
}

// 单根、父子互指、无重复子、depth 从根连续、连通无环、within 上限。
std::string validatePlan(const Plan& plan, const std::vector<Size>& labelSizes, const Options& options,
                         int& rootIndex) {
    if (!finiteNonNegative(options.paddingX) || !finiteNonNegative(options.paddingY) ||
        !finiteNonNegative(options.columnGap) || !finiteNonNegative(options.rowGap) ||
        options.maxNodes == 0) {
        return kRejectBadOptions;
    }
    if (plan.nodes.empty()) {
        return kRejectEmptyPlan;
    }
    if (labelSizes.size() != plan.nodes.size()) {
        return kRejectLabelCount;
    }
    if (plan.nodes.size() > options.maxNodes) {
        return kRejectNodeLimit;
    }
    for (const Size& size : labelSizes) {
        if (!finiteNonNegative(size.width) || !finiteNonNegative(size.height)) {
            return kRejectBadLabelSize;
        }
    }

    const int n = static_cast<int>(plan.nodes.size());
    std::vector<int> claimParent(static_cast<std::size_t>(n), -1);
    int rootCount = 0;
    rootIndex = -1;

    for (int i = 0; i < n; ++i) {
        const Node& node = plan.nodes[static_cast<std::size_t>(i)];
        if (!isSupportedShape(node.shape)) {
            return kRejectBadShape;
        }
        if (node.parent == -1) {
            ++rootCount;
            rootIndex = i;
        } else if (node.parent < 0 || node.parent >= n || node.parent == i) {
            return kRejectBadParent;
        }
    }
    if (rootCount != 1) {
        return kRejectRootCount;
    }
    // 根深度必须是 0：否则连续性与"最深不超过 N-1"不成立，列宽/坐标数组会被恶意
    // depth 值撑爆。
    if (plan.nodes[static_cast<std::size_t>(rootIndex)].depth != 0) {
        return kRejectDepthMismatch;
    }

    for (int i = 0; i < n; ++i) {
        for (int child : plan.nodes[static_cast<std::size_t>(i)].children) {
            if (child < 0 || child >= n || child == i) {
                return kRejectBadChild;
            }
            if (claimParent[static_cast<std::size_t>(child)] != -1) {
                return kRejectChildDup;
            }
            claimParent[static_cast<std::size_t>(child)] = i;
        }
    }
    for (int i = 0; i < n; ++i) {
        const int parent = plan.nodes[static_cast<std::size_t>(i)].parent;
        const int claimed = claimParent[static_cast<std::size_t>(i)];
        if (parent == -1) {
            if (claimed != -1) {
                return kRejectParentMismatch;
            }
        } else if (claimed != parent) {
            return kRejectParentMismatch;
        }
    }

    std::vector<char> visited(static_cast<std::size_t>(n), 0);
    std::vector<int> stack;
    stack.push_back(rootIndex);
    int visitedCount = 0;
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        if (visited[static_cast<std::size_t>(node)]) {
            return kRejectCycle;
        }
        visited[static_cast<std::size_t>(node)] = 1;
        ++visitedCount;
        const std::size_t nodeDepth = plan.nodes[static_cast<std::size_t>(node)].depth;
        for (int child : plan.nodes[static_cast<std::size_t>(node)].children) {
            const std::size_t childDepth = plan.nodes[static_cast<std::size_t>(child)].depth;
            if (childDepth != nodeDepth + 1) {
                return kRejectDepthMismatch;
            }
            if (childDepth > options.maxDepth) {
                return kRejectDepthLimit;
            }
            stack.push_back(child);
        }
    }
    if (visitedCount != n) {
        return kRejectUnreachable;
    }
    return std::string();
}

} // namespace

bool Rect::intersects(const Rect& other) const {
    return x <= other.x + other.width && other.x <= x + width && y <= other.y + other.height &&
           other.y <= y + height;
}

Layout buildLayout(const Plan& plan, const std::vector<Size>& labelSizes, const Options& options) {
    int rootIndex = -1;
    const std::string reason = validatePlan(plan, labelSizes, options, rootIndex);
    if (!reason.empty()) {
        return reject(reason.c_str());
    }

    const std::size_t count = plan.nodes.size();
    const double gx = static_cast<double>(options.columnGap);
    const double gy = static_cast<double>(options.rowGap);

    // 节点框尺寸（double 中间量，逐项检查 float 可表示）。
    std::vector<double> boxWidth(count, 0.0);
    std::vector<double> boxHeight(count, 0.0);
    std::size_t maxNodeDepth = 0;
    for (std::size_t i = 0; i < count; ++i) {
        double width = 0.0;
        double height = 0.0;
        if (!nodeBoxSize(plan.nodes[i].shape, static_cast<double>(labelSizes[i].width),
                         static_cast<double>(labelSizes[i].height), options, width, height)) {
            return reject(kRejectGeometryOverflow);
        }
        boxWidth[i] = width;
        boxHeight[i] = height;
        maxNodeDepth = std::max(maxNodeDepth, plan.nodes[i].depth);
    }

    // 列位：同层取最大宽，列间留 columnGap；前缀和逐项查溢出。
    std::vector<double> columnWidth(maxNodeDepth + 1, 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        columnWidth[plan.nodes[i].depth] = std::max(columnWidth[plan.nodes[i].depth], boxWidth[i]);
    }
    std::vector<double> columnX(maxNodeDepth + 1, 0.0);
    for (std::size_t depth = 1; depth <= maxNodeDepth; ++depth) {
        columnX[depth] = columnX[depth - 1] + columnWidth[depth - 1] + gx;
        if (!withinFloat(columnX[depth])) {
            return reject(kRejectGeometryOverflow);
        }
    }

    // 前序（父先于子）与逆前序（子先于父）。
    std::vector<int> pre;
    pre.reserve(count);
    std::vector<int> stack;
    stack.push_back(rootIndex);
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        pre.push_back(node);
        const std::vector<int>& children = plan.nodes[static_cast<std::size_t>(node)].children;
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(*it);
        }
    }

    std::vector<double> subtreeHeight(count, 0.0);
    std::vector<double> childrenHeight(count, 0.0);
    std::vector<double> subtreeRight(count, 0.0);

    // 后序：先算完后代；每个孩子只被父访问一次。
    for (auto it = pre.rbegin(); it != pre.rend(); ++it) {
        const std::size_t index = static_cast<std::size_t>(*it);
        const std::vector<int>& children = plan.nodes[index].children;
        const double rectHeight = boxHeight[index];
        double right = columnX[plan.nodes[index].depth] + boxWidth[index];
        double childrenTotal = 0.0;
        if (!children.empty()) {
            double sum = 0.0;
            for (int child : children) {
                const std::size_t c = static_cast<std::size_t>(child);
                sum += subtreeHeight[c];
                right = std::max(right, subtreeRight[c]);
            }
            childrenTotal = sum + gy * static_cast<double>(children.size() - 1);
        }
        if (!withinFloat(right) || !withinFloat(childrenTotal)) {
            return reject(kRejectGeometryOverflow);
        }
        childrenHeight[index] = childrenTotal;
        subtreeHeight[index] = std::max(rectHeight, childrenTotal);
        subtreeRight[index] = right;
    }

    Layout layout;
    layout.nodes.resize(count);
    layout.edges.reserve(count > 0 ? count - 1 : 0);

    // 前序：父先定 y 与子树顶，再把子分配进连续的垂直区段（兄弟子树互不重叠）。
    std::vector<double> subtreeTop(count, 0.0);
    for (int node : pre) {
        const std::size_t index = static_cast<std::size_t>(node);
        const double top = subtreeTop[index];
        const double height = subtreeHeight[index];
        const double rectHeight = boxHeight[index];
        const double x = columnX[plan.nodes[index].depth];
        const double y = top + (height - rectHeight) * 0.5;
        const double subtreeWidth = subtreeRight[index] - x;
        // 写回 float 前把每条边的右/下边界也在 double 里复核，避免 float 舍入后溢出。
        const double boundsRight = x + boxWidth[index];
        const double boundsBottom = y + rectHeight;
        const double subtreeRightEdge = x + subtreeWidth;
        const double subtreeBottom = top + height;
        if (!withinFloat(y) || !withinFloat(subtreeWidth) || subtreeWidth < 0.0 ||
            !withinFloat(boundsRight) || boundsRight < x || !withinFloat(boundsBottom) ||
            boundsBottom < y || !withinFloat(subtreeRightEdge) || !withinFloat(subtreeBottom) ||
            subtreeBottom < top) {
            return reject(kRejectGeometryOverflow);
        }

        LayoutNode& out = layout.nodes[index];
        out.bounds = Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(boxWidth[index]),
                          static_cast<float>(rectHeight)};
        out.subtreeBounds = Rect{static_cast<float>(x), static_cast<float>(top),
                                 static_cast<float>(subtreeWidth), static_cast<float>(height)};

        const std::vector<int>& children = plan.nodes[index].children;
        if (!children.empty()) {
            double cursor = top + (height - childrenHeight[index]) * 0.5;
            for (int child : children) {
                const std::size_t c = static_cast<std::size_t>(child);
                subtreeTop[c] = cursor;
                cursor += subtreeHeight[c] + gy;
                if (!withinFloat(subtreeTop[c]) || !withinFloat(cursor)) {
                    return reject(kRejectGeometryOverflow);
                }
            }
        }
    }
    layout.bounds = layout.nodes[static_cast<std::size_t>(rootIndex)].subtreeBounds;

    // 连线：父在左列、子在右列，三段都落在两列间隙里。所有相加在 double 里做：
    // 两个各自的 float 端点都有限，但相加（如 parentRight + childLeft）仍可能超过
    // float 范围；先算 double 再逐点 withinFloat 后写回。
    for (std::size_t i = 0; i < count; ++i) {
        const int parent = plan.nodes[i].parent;
        if (parent == -1) {
            continue;
        }
        const Rect& pb = layout.nodes[static_cast<std::size_t>(parent)].bounds;
        const Rect& cb = layout.nodes[i].bounds;
        const double parentRightX = static_cast<double>(pb.x) + static_cast<double>(pb.width);
        const double parentCenterY = static_cast<double>(pb.y) + static_cast<double>(pb.height) * 0.5;
        const double childLeftX = static_cast<double>(cb.x);
        const double childCenterY = static_cast<double>(cb.y) + static_cast<double>(cb.height) * 0.5;
        const double midX = (parentRightX + childLeftX) * 0.5;
        if (!withinFloat(parentRightX) || !withinFloat(parentCenterY) || !withinFloat(childLeftX) ||
            !withinFloat(childCenterY) || !withinFloat(midX)) {
            return reject(kRejectGeometryOverflow);
        }

        Edge edge;
        edge.parent = parent;
        edge.child = static_cast<int>(i);
        edge.points[0] = Point{static_cast<float>(parentRightX), static_cast<float>(parentCenterY)};
        edge.points[1] = Point{static_cast<float>(midX), static_cast<float>(parentCenterY)};
        edge.points[2] = Point{static_cast<float>(midX), static_cast<float>(childCenterY)};
        edge.points[3] = Point{static_cast<float>(childLeftX), static_cast<float>(childCenterY)};
        edge.pointCount = 4;
        layout.edges.push_back(edge);
    }

    layout.rejectReason.clear();
    layout.valid = true;
    return layout;
}

std::size_t estimateLayoutBytes(const Layout& layout) {
    return sizeof(Layout) + layout.nodes.capacity() * sizeof(LayoutNode) +
           layout.edges.capacity() * sizeof(Edge);
}

std::size_t estimateLayoutBytes(std::size_t nodeCount) {
    const std::size_t edgeCount = nodeCount > 0 ? nodeCount - 1 : 0;
    return sizeof(Layout) + nodeCount * sizeof(LayoutNode) + edgeCount * sizeof(Edge);
}

Visibility queryVisible(const Layout& layout, const Rect& viewport, std::size_t textNodeBudget) {
    Visibility result;
    if (!layout.valid || !validViewport(viewport)) {
        return result;
    }
    for (int i = 0; i < static_cast<int>(layout.nodes.size()); ++i) {
        if (layout.nodes[static_cast<std::size_t>(i)].bounds.intersects(viewport)) {
            result.nodes.push_back(i);
        }
    }
    for (int i = 0; i < static_cast<int>(layout.edges.size()); ++i) {
        const Edge& edge = layout.edges[static_cast<std::size_t>(i)];
        // 外部构造的 Layout 可能给出越界/负的 pointCount，按数组容量 [0,4] 夹紧。
        const int points = std::max(0, std::min(edge.pointCount, 4));
        bool visible = false;
        for (int segment = 0; segment + 1 < points; ++segment) {
            if (segmentIntersectsRect(edge.points[segment], edge.points[segment + 1], viewport)) {
                visible = true;
                break;
            }
        }
        if (visible) {
            result.edges.push_back(i);
        }
    }
    result.textOmitted = result.nodes.size() > textNodeBudget;
    return result;
}

} // namespace neo::mindmap
